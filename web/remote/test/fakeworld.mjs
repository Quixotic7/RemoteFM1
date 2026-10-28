// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
//
// A fake world for remote.js: a mocked navigator.requestMIDIAccess and navigator.usb around libremotefm1's fake
// units (host/src/fake.c compiled to WASM: web/build/remote/fakeunit.mjs). No hardware is touched. Used by the
// Node test (simulated clock: step()) and the headless-Chromium page (real timers: run()).

export const FAKE_STATS = ['hellos', 'sessions_started', 'sessions_ended', 'keepalive_expired', 'leds', 'tiles_msgs',
    'frames', 'acks', 'acks_dropped', 'tile_bytes', 'decode_errs', 'panel_r', 'panel_req', 'msgs', 'sx_msgs', 'bk_msgs',
    'held_frames', 'hold_timeouts', 'panel_updates'];

// split a MIDI byte stream into messages, as Web MIDI delivers them (one SysEx, or one channel message, per event)
export function splitMidi(bytes, state) {
    const out = [];
    for (const b of bytes) {
        if (b >= 0xf8) { out.push(Uint8Array.of(b)); continue; }          // realtime
        if (state.sx) {
            state.cur.push(b);
            if (b === 0xf7) { out.push(Uint8Array.from(state.cur)); state.sx = false; state.cur = []; }
            continue;
        }
        if (b === 0xf0) { state.sx = true; state.cur = [b]; continue; }
        if (b & 0x80) {
            state.cur = [b];
            state.need = (b & 0xf0) === 0xc0 || (b & 0xf0) === 0xd0 ? 2 : b >= 0xf0 ? 1 : 3;
        } else if (state.cur.length) {
            state.cur.push(b);
        } else continue;
        if (state.cur.length === state.need) { out.push(Uint8Array.from(state.cur)); state.cur = state.cur.slice(0, 1); }
    }
    return out;
}

// the RemoteFM1 commands in a byte stream sent to a port (F0 7D 52 46 cmd ..)
export function sysexCommands(bytes) {
    const cmds = [];
    for (let i = 0; i + 4 < bytes.length; i++)
        if (bytes[i] === 0xf0 && bytes[i + 1] === 0x7d && bytes[i + 2] === 0x52 && bytes[i + 3] === 0x46) cmds.push(bytes[i + 4]);
    return cmds;
}

class Port extends EventTarget {
    constructor(world, id, name, type) {
        super();
        this.world = world; this.id = id; this.name = name; this.type = type; this.manufacturer = 'fake';
        this.version = '1'; this.state = 'connected'; this.connection = 'closed'; this.onmidimessage = null;
    }
    open() { this.connection = 'open'; return Promise.resolve(this); }
    close() { this.connection = 'closed'; return Promise.resolve(this); }
}
class Output extends Port {
    send(data) {
        if (this.state !== 'connected') throw new DOMException('port disconnected', 'InvalidStateError');
        const b = Uint8Array.from(data);
        if (b[0] === 0xf0 && !this.world.sysexGranted) throw new DOMException('no sysex', 'InvalidAccessError');
        this.world._send(this, b);
    }
    clear() {}
}

class UsbDevice {
    constructor(world, unit) {
        this.world = world; this.unit = unit;
        this.vendorId = 0x1209; this.productId = 0x0001; this.productName = 'FM-1'; this.manufacturerName = 'M-VAVE';
        this.opened = false; this.configuration = null; this.claimed = new Set(); this.reads = []; this.inBuf = [];
        this.gone = false;
        const ep = (n, d, type = 'bulk', packetSize = 64) => ({ endpointNumber: n, direction: d, type, packetSize });
        const alt = (a, cls, eps) => ({ alternateSetting: a, interfaceClass: cls, interfaceSubclass: 0, interfaceProtocol: 0, endpoints: eps });
        const mk = (n, cls, eps, alt1) => { const a0 = alt(0, cls, eps);
            return { interfaceNumber: n, alternate: a0, alternates: alt1 ? [a0, alt(1, cls, alt1)] : [a0], claimed: false }; };
        // RemoteFM1 0.1 / 0.2.0: 0 audio control, 1 MIDI, 2 the vendor pair; 0.2.1 (USB audio): 2 audio in, 3 audio
        // out (alt 0 empty, alt 1 an isochronous EP4), 4 the vendor pair
        this.vendorIf = unit.vendorIf;
        const ifs = [mk(0, 1, []), mk(1, 1, [ep(1, 'in'), ep(1, 'out')])];
        if (this.vendorIf === 4)
            ifs.push(mk(2, 1, [], [ep(4, 'in', 'isochronous', 184)]), mk(3, 1, [], [ep(4, 'out', 'isochronous', 196)]));
        ifs.push(mk(this.vendorIf, 0xff, [ep(3, 'in'), ep(3, 'out')]));
        this._config = { configurationValue: 1, interfaces: ifs };
    }
    _check() { if (this.gone) throw new DOMException('The device was disconnected.', 'NotFoundError'); }
    async open() { this._check(); this.opened = true; this.world.log.push('usb open'); }
    async close() { this.opened = false; this.claimed.clear(); this.configuration = null; }
    async selectConfiguration(v) { this._check(); if (v !== 1) throw new Error('bad config'); this.configuration = this._config; }
    async claimInterface(n) {
        this._check();
        this.world.claims.push(n);
        if (!this.configuration) throw new DOMException('no configuration', 'InvalidStateError');
        const it = this.configuration.interfaces.find(i => i.interfaceNumber === n);
        if (!it) throw new DOMException('no such interface', 'NotFoundError');
        if (it.alternate.interfaceClass !== 0xff) throw new DOMException('protected interface class', 'SecurityError');
        this.claimed.add(n);
    }
    async releaseInterface(n) { this.claimed.delete(n); }
    async transferOut(ep, data) {
        this._check();
        if (ep !== 3 || !this.claimed.has(this.vendorIf)) throw new DOMException('bad endpoint', 'NotFoundError');
        const b = Uint8Array.from(data);
        this.world.usbOut += b.length;
        if (!this.unit.unplugged) this.world._toFake(this.unit, 'bk', b);
        return { status: 'ok', bytesWritten: b.length };
    }
    transferIn(ep, len) {
        try { this._check(); } catch (e) { return Promise.reject(e); }
        if (ep !== 3 || !this.claimed.has(this.vendorIf)) return Promise.reject(new DOMException('bad endpoint', 'NotFoundError'));
        return new Promise((resolve, reject) => { this.reads.push({ len, resolve, reject }); this._serve(); });
    }
    _serve() {
        while (this.reads.length && this.inBuf.length) {
            const r = this.reads.shift(), n = Math.min(r.len, this.inBuf.length);
            const chunk = Uint8Array.from(this.inBuf.splice(0, n));
            r.resolve({ status: 'ok', data: new DataView(chunk.buffer) });
        }
    }
    _unplug() {
        this.gone = true;
        for (const r of this.reads.splice(0)) r.reject(new DOMException('The transfer was cancelled.', 'NetworkError'));
    }
}

export async function createWorld(createFakeUnit, { start = 1000 } = {}) {
    const F = await createFakeUnit();
    const world = {
        F, t: start, units: [], others: [], log: [], claims: [], requestDeviceFilters: [], midiRequests: [],
        sysexGranted: true, usbOut: 0, permitted: new Set(),
        inputs: new Map(), outputs: new Map(), access: null,
    };
    const access = new EventTarget();
    access.inputs = world.inputs; access.outputs = world.outputs; access.sysexEnabled = true; access.onstatechange = null;
    world.access = access;
    const usb = new EventTarget();
    usb.getDevices = async () => world.units.filter(u => u.usb && !u.usb.gone && world.permitted.has(u.usb)).map(u => u.usb);
    usb.requestDevice = async opts => {
        world.requestDeviceFilters.push(JSON.parse(JSON.stringify(opts)));
        if (world.usbChooserCancels) throw new DOMException('No device selected.', 'NotFoundError');
        const f = opts.filters[0];
        const u = world.units.find(u => u.usb && !u.usb.gone && u.usb.vendorId === f.vendorId && u.usb.productId === f.productId);
        if (!u) throw new DOMException('No device selected.', 'NotFoundError');
        world.permitted.add(u.usb);
        return u.usb;
    };
    // the Web MIDI permission (navigator.permissions 'midi' with sysex): 'granted' (default), 'prompt' (a request
    // would ask: here it is granted at once, as a click on Allow), 'denied' (a request throws). world.setMidiPerm()
    // changes it and fires the PermissionStatus's 'change'.
    world.midiPerm = 'granted';
    world.permQueries = [];
    const permStatus = new EventTarget();
    Object.defineProperty(permStatus, 'state', { get: () => world.midiPerm });
    world.setMidiPerm = st => { world.midiPerm = st; permStatus.dispatchEvent(new Event('change')); };
    world.navigator = {
        requestMIDIAccess: async opts => {
            world.midiRequests.push(opts);
            if (world.midiPerm === 'denied') throw new DOMException('Permission denied.', 'NotAllowedError');
            if (world.midiPerm === 'prompt') world.midiPerm = 'granted';
            if (!opts || !opts.sysex) world.sysexGranted = false;
            return access;
        },
        permissions: {
            query: async d => {
                world.permQueries.push(d);
                if (!d || d.name !== 'midi') throw new TypeError('not a permission name');
                return permStatus;
            },
        },
        usb,
    };

    let nid = 1;
    const addPorts = (name, owner) => {
        const i = new Port(world, `in-${nid}`, name, 'input'), o = new Output(world, `out-${nid}`, name, 'output');
        nid++;
        i.owner = o.owner = owner;
        world.inputs.set(i.id, i);
        world.outputs.set(o.id, o);
        return { input: i, output: o };
    };

    // a fake unit: the RemoteFM1 firmware (default), or another firmware (name / proto) or a silent one; vendorIf: the
    // vendor interface's number (4: 0.2.1 with USB audio, 2: 0.1 / 0.2.0); ble: its MIDI ports are Bluetooth MIDI (its
    // HELLO_R says so: sxq 128); hold: RemoteFM1 0.2.2's HOLD (false: an older unit)
    world.addUnit = ({ midiName = 'FM-1', name = '', proto = 0, silent = false, bulk = false, sxRate = 300000,
        bkRate = 850000, vendorIf = 4, ble = false, hold = true } = {}) => {
        let p = 0;
        if (name) {
            p = F._malloc(name.length + 1);
            F.stringToUTF8(name, p, name.length + 1);
        }
        const h = F._fk_new(bulk ? 1 : 0, silent ? 1 : 0, p, proto, sxRate, bkRate, ble ? 1 : 0, hold ? 1 : 0);   // (copies the name)
        if (p) F._free(p);
        return world._unit(h, midiName, bulk, vendorIf);
    };
    world._unit = (h, midiName, bulk, vendorIf = 4) => {
        const u = { h, midiName, vendorIf, sent: [], cmds: [], unplugged: false, split: { sx: false, cur: [] } };
        Object.assign(u, addPorts(midiName, u));
        if (bulk) u.usb = new UsbDevice(world, u);
        world.units.push(u);
        return u;
    };
    // another MIDI device (not an FM-1): it records every byte it gets, which must stay none
    world.addOther = name => {
        const d = { name, bytes: 0 };
        Object.assign(d, addPorts(name, d));
        world.others.push(d);
        return d;
    };

    world._send = (port, b) => {
        const o = port.owner;
        if (!o) return;
        if (o.h === undefined) { o.bytes += b.length; return; }
        o.sent.push(b);
        o.cmds.push(...sysexCommands(b));
        if (!o.unplugged) world._toFake(o, 'sx', b);
    };
    world._toFake = (u, kind, b) => {
        const p = F._fk_inbuf(b.length);
        F.HEAPU8.set(b, p);
        if (kind === 'sx') F._fk_rx_sx(u.h, b.length, world.t >>> 0);
        else F._fk_rx_bk(u.h, b.length, world.t >>> 0);
    };

    // one millisecond of the units: their timers, their output to the ports and the bulk pipe
    world.pump = () => {
        for (const u of world.units) {
            F._fk_poll(u.h, world.t >>> 0);
            let n = F._fk_take_sx(u.h);
            if (n) {
                const bytes = F.HEAPU8.slice(F._fk_outbuf(), F._fk_outbuf() + n);
                if (!u.unplugged) for (const m of splitMidi(bytes, u.split)) {
                    const e = new Event('midimessage');
                    e.data = m;
                    e.receivedTime = world.t;
                    u.input.dispatchEvent(e);
                    if (u.input.onmidimessage) u.input.onmidimessage(e);
                }
            }
            n = F._fk_take_bk(u.h);
            if (n && u.usb && !u.unplugged) {
                const bytes = F.HEAPU8.slice(F._fk_outbuf(), F._fk_outbuf() + n);
                for (const b of bytes) u.usb.inBuf.push(b);
                u.usb._serve();
            }
        }
    };
    world.step = (ms = 1) => { for (let i = 0; i < ms; i++) { world.t++; world.pump(); } };

    world.unplug = u => {
        u.unplugged = true;
        u.input.state = u.output.state = 'disconnected';
        for (const p of [u.input, u.output]) { const e = new Event('statechange'); e.port = p; access.dispatchEvent(e); }
        if (u.usb) { u.usb._unplug(); const e = new Event('disconnect'); e.device = u.usb; usb.dispatchEvent(e); }
    };
    // a unit plugged in after the page started: its ports appear (statechange), its USB device too ('connect' only
    // when it was paired before: permitted)
    world.plugIn = (u, { paired = false } = {}) => {
        for (const p of [u.input, u.output]) { const e = new Event('statechange'); e.port = p; access.dispatchEvent(e); }
        if (u.usb && paired) { world.permitted.add(u.usb); const e = new Event('connect'); e.device = u.usb; usb.dispatchEvent(e); }
    };
    world.unplugUsb = u => { u.usb._unplug(); const e = new Event('disconnect'); e.device = u.usb; usb.dispatchEvent(e); };
    world.replug = u => {
        F._fk_reboot(u.h);                   // a power cycle
        u.unplugged = false;
        u.split = { sx: false, cur: [] };
        u.input.state = u.output.state = 'connected';
        for (const p of [u.input, u.output]) { const e = new Event('statechange'); e.port = p; access.dispatchEvent(e); }
        if (u.usb) {
            u.usb = new UsbDevice(world, u);
            world.permitted.add(u.usb);
            const e = new Event('connect'); e.device = u.usb; usb.dispatchEvent(e);
        }
    };

    // the unit's state
    world.session = u => F._fk_session(u.h);
    world.stats = u => { const p = F._fk_stats(u.h) >> 2, o = {}; FAKE_STATS.forEach((k, i) => { o[k] = F.HEAPU32[p + i]; }); return o; };
    world.shadow = u => F.HEAPU8.slice(F._fk_shadow(u.h), F._fk_shadow(u.h) + 240 * 240 * 2);
    world.panel = u => F.HEAPU8.slice(F._fk_panel(u.h), F._fk_panel(u.h) + 240 * 240 * 2);   // what the unit shows
    world.holding = u => F._fk_holding(u.h) !== 0;
    world.leds = u => { const p = F._fk_leds(u.h), b = F.HEAPU8;
        return { lit: b.slice(p, p + 11), mid: b.slice(p + 11, p + 22), dim: b.slice(p + 22, p + 33), faint: b.slice(p + 33, p + 44), flags: b[p + 44] }; };
    world.key = (u, k, d) => F._fk_key(u.h, k, d ? 1 : 0);
    world.button = (u, b, d) => F._fk_button(u.h, b, d ? 1 : 0);
    world.enc = (u, e, d) => F._fk_enc(u.h, e, d);
    world.master = (u, v) => F._fk_master(u.h, v);
    world.reboot = u => F._fk_reboot(u.h);
    world.deaf = (u, on) => F._fk_deaf(u.h, on ? 1 : 0);
    world.dropMidi = (u, n) => F._fk_drop_midi(u.h, n);
    return world;
}

// a frame as engine.screen() gives it (host-order RGB565) and the bytes the unit's shadow must then hold (BE)
export function testFrame(phase = 0) {
    const px = new Uint16Array(240 * 240);
    for (let y = 0; y < 240; y++)
        for (let x = 0; x < 240; x++) {
            let v;
            if (y < 40) v = 0xf800;                                         // a red band
            else if (y < 80) v = ((x + phase) >> 3) & 1 ? 0xffff : 0x001f;  // stripes
            else if (y < 160) v = ((x * 7 + y * 13 + phase * 31) & 0xffff); // noise-ish (RAW / RLE)
            else v = (x >> 4) * 0x0841;                                     // a ramp
            px[y * 240 + x] = v;
        }
    return px;
}
export function beBytes(px) {
    const b = new Uint8Array(px.length * 2);
    for (let i = 0; i < px.length; i++) { b[2 * i] = px[i] >> 8; b[2 * i + 1] = px[i] & 0xff; }
    return b;
}
export function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
// levels[col * 4 + line] -> the planes the unit must show (fm1p_led_class; line l = row bit l + 1)
export function expectedPlanes(levels, playGreen) {
    const p = { lit: new Uint8Array(11), mid: new Uint8Array(11), dim: new Uint8Array(11), faint: new Uint8Array(11), flags: playGreen >= 176 ? 1 : 0 };
    for (let c = 0; c < 11; c++)
        for (let l = 0; l < 4; l++) {
            const v = levels[c * 4 + l], bit = 1 << (l + 1);
            if (v >= 176) p.lit[c] |= bit; else if (v >= 96) p.mid[c] |= bit; else if (v >= 40) p.dim[c] |= bit; else if (v) p.faint[c] |= bit;
        }
    return p;
}
export function samePlanes(a, b) {
    return ['lit', 'mid', 'dim', 'faint'].every(k => sameBytes(a[k], b[k])) && a.flags === b.flags;
}
