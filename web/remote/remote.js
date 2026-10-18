// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
//
// "Use my FM-1" (PLAN.md M4, WEB-PLAN.md section 2): an FM-1 running RemoteFM1 becomes the panel of the firmware
// that runs in the browser. Keys, buttons, encoders and MASTER come in; the screen and LEDs go out.
//
// libremotefm1's portable session (host/src/session.c and friends, compiled to WebAssembly: web/remote/build.sh)
// does the protocol, exactly as fm1remote does on macOS. This file is its back end, in place of CoreMIDI and
// libusb:
//   - Web MIDI (sysex): candidate port pairs whose name contains "FM-1" (or "RemoteFM1"), never "update". The
//     session sends HELLO to each and keeps only the one answering "RemoteFM1" proto 2. The port name proves
//     nothing (macOS keeps whatever firmware's name it saw first, e.g. "ChoralRoot FM-1"), and no other device is
//     ever sent a byte.
//   - WebUSB (Chrome): the vendor interface (class 0xFF, interface 2, bulk EP 3) of 1209:0001. When it answers,
//     frames and LEDs go over bulk; the panel's plain MIDI events always come over Web MIDI.
// It runs on the page's main thread (or a Worker with WebUSB only), never in the AudioWorklet. See README.md.

const KIND_SX = 0, KIND_BK = 1;
const TRANSPORTS = { auto: 0, midi: 1, bulk: 2 };
const S_DISCOVER = 0, S_STARTING = 1, S_ACTIVE = 2, S_STOPPING = 3, S_STOPPED = 4;
const EV_KEY = 1, EV_BUTTON = 2, EV_ENC = 3, EV_MASTER = 4;
export const USB_FILTER = Object.freeze({ vendorId: 0x1209, productId: 0x0001, classCode: 0xff });
const VENDOR_IF = 2, BULK_EP = 3, USB_IN_LEN = 16384 + 64;
const NKEYS = 27, NBTN = 14, NENC = 7;
// rfm_session_stats_t's fields, in order (rw_stats)
const STAT_NAMES = ['frames', 'msgs', 'tiles', 'rects', 'body_bytes', 'wire_bytes', 'acks', 'ack_ms_sum',
    'ack_ms_max', 'ack_timeouts', 'decode_errs', 'shadow_resets', 'leds_sent', 'keepalives', 'panel_r', 'events',
    'sessions', 'discoveries', 'lost', 'in_flight', 'pending', 'last_ack_ms'];

// A port that may be an FM-1: its name says so. Updater ports are never touched (as host/src/macos/coremidi.c).
export function isCandidatePortName(name) {
    const n = String(name || '');
    return (/FM-1/i.test(n) || /RemoteFM1/i.test(n)) && !/update/i.test(n);
}

function connected(port) { return !port.state || port.state === 'connected'; }

// Web MIDI has separate inputs and outputs: pair each candidate output with the input of the same name (else with
// a candidate input not taken yet).
export function candidatePairs(access, match = isCandidatePortName) {
    const ins = [...access.inputs.values()].filter(p => connected(p) && match(p.name));
    const outs = [...access.outputs.values()].filter(p => connected(p) && match(p.name));
    const used = new Set(), pairs = [];
    for (const o of outs) {
        let i = ins.find(p => !used.has(p.id) && p.name === o.name && (p.manufacturer || '') === (o.manufacturer || ''));
        if (!i) i = ins.find(p => !used.has(p.id) && p.name === o.name);
        if (!i) i = ins.find(p => !used.has(p.id));
        if (!i) continue;
        used.add(i.id);
        pairs.push({ key: o.id + '|' + i.id, name: o.name, input: i, output: o });
    }
    return pairs;
}

function usbMatches(d) { return d.vendorId === USB_FILTER.vendorId && d.productId === USB_FILTER.productId; }

export class RemoteFM1 {
    // opts: { fps: 20, transport: 'auto' | 'midi' | 'bulk', usb: 'ask' | 'known' | 'off', keepaliveMs: 3000,
    //         budget: 0 (bytes per frame, 0 = the transport's), moduleUrl, portMatch(name) -> bool,
    //         navigator (tests), now() -> ms (tests), timers: true (false: the caller calls tick()) }
    constructor(opts = {}) {
        this.opts = {
            fps: 20, transport: 'auto', usb: 'ask', keepaliveMs: 3000, budget: 0, hostName: 'web', timers: true,
            usbReads: 4, moduleUrl: new URL('../build/remote/remotefm1.mjs', import.meta.url).href, ...opts,
        };
        this._nav = opts.navigator || globalThis.navigator;
        this._now = opts.now || (() => Math.floor(performance.now()));
        this._match = opts.portMatch || isCandidatePortName;
        this._ls = { status: new Set(), panel: new Set(), log: new Set() };
        this._links = new Map();             // tag -> link
        this._nextTag = 1;
        this._M = null;
        this._access = null;
        this._timer = null;
        this._running = false;
        this._status = { state: 'idle', transport: null, fps: 0, why: '', unit: null, ports: null };
        this._frame = null;                  // the last frame given (applied when the module is up)
        this._frameSeq = undefined;
        this._leds = null;
        this._ledsKey = '';
        this._encAcc = new Int32Array(NENC);
        this._downK = 0; this._downB = 0; this._tapK = 0; this._tapB = 0;
        this._panelDirty = false;
        this._released = true;
        this._stopWaiters = [];
        this._fpsT = 0; this._fpsAcks = 0; this._fps = 0;
        this._wasActive = false;
        this._usbOpening = false;
        this._onUsbDisconnect = e => {
            for (const l of this._links.values()) if (l.kind === 'usb' && l.dev === e.device) l.dead = true;
        };
        this._onUsbConnect = e => {
            if (this._running && usbMatches(e.device) && this._wantUsb()) this._openUsb(e.device);
        };
        this._onMidiState = () => {
            // a port gone: its link goes at once (the session then looks again); new ports wait for the rescan
            if (!this._access) return;
            for (const l of this._links.values())
                if (l.kind === 'midi' && (!connected(l.input) || !connected(l.output))) l.dead = true;
        };
    }

    // ---------------------------------------------------------------- the page's API
    on(type, cb) { this._ls[type].add(cb); return () => this._ls[type].delete(cb); }
    onStatus(cb) { return this.on('status', cb); }
    onPanel(cb) { return this.on('panel', cb); }
    onLog(cb) { return this.on('log', cb); }
    get status() { return { ...this._status }; }

    // Start looking for the unit and keep a session with it (reconnecting by itself) until stop() / disconnect().
    // Call it from a click: in Chrome it may open the WebUSB chooser (opts.usb 'ask', when no FM-1 is paired yet).
    async connect() {
        if (this._running) {
            if (this._M && this._M._rw_state() === S_STOPPED) this._M._rw_restart(this._t());
            return;
        }
        this._running = true;
        // the chooser first, while the click's user activation is fresh
        const usbPick = this._wantUsb() ? this._pickUsb(this.opts.usb === 'ask') : Promise.resolve([]);
        this._setStatus({ state: 'searching', why: 'connect' });
        try {
            const M = await this._loadModule();
            const midi = this._nav && this._nav.requestMIDIAccess
                ? this._nav.requestMIDIAccess({ sysex: true }).catch(e => { this._log(`Web MIDI: ${e.message || e}`); return null; })
                : Promise.resolve(null);
            const [access, devs] = await Promise.all([midi, usbPick.catch(() => [])]);
            if (!this._running) return;
            if (!access && !devs.length) throw new Error(this._nav && this._nav.requestMIDIAccess
                ? 'MIDI access (with SysEx) was refused' : 'this browser has no Web MIDI');
            this._access = access;
            if (access) {
                if (access.addEventListener) access.addEventListener('statechange', this._onMidiState);
            }
            const name = this.opts.hostName || 'web';
            const np = M._malloc(name.length * 3 + 1);
            M.stringToUTF8(name, np, name.length * 3 + 1);
            M.rfmWrite = (tag, bytes) => this._write(tag, bytes);
            M.rfmEvent = (t, i, s, v) => this._event(t, i, s, v);
            M.rfmState = (st, why) => this._state(st, why);
            M.rfmRescan = () => this._rescan();
            M.rfmLog = msg => this._log(msg);
            if (M._rw_init(this.opts.fps | 0, TRANSPORTS[this.opts.transport] ?? 0, this.opts.keepaliveMs | 0,
                this.opts.budget | 0, np)) throw new Error('out of memory');
            M._free(np);
            if (this._frame) this._applyFrame();
            if (this._leds) this._applyLeds();
            this._scanMidi();
            for (const d of devs) await this._openUsb(d);
            if (this._nav && this._nav.usb && this._nav.usb.addEventListener) {
                this._nav.usb.addEventListener('disconnect', this._onUsbDisconnect);
                this._nav.usb.addEventListener('connect', this._onUsbConnect);
            }
            if (!this._links.size) this._log('no candidate port yet: is the FM-1 plugged in and running RemoteFM1?');
            this.tick();
            if (this.opts.timers) this._timer = setInterval(() => this.tick(), 4);
        } catch (e) {
            this._running = false;
            this._setStatus({ state: 'error', why: String(e.message || e) });
            throw e;
        }
    }

    // End the session: SESSION 0 goes out at once (synchronously on Web MIDI: safe in 'pagehide'); resolves on the
    // unit's ACK or after 300 ms. The unit returns to its waiting screen; the ports stay open (connect() resumes).
    stop() {
        const M = this._M;
        this._releasePanel();
        if (!M || !this._running) return Promise.resolve();
        const st = M._rw_state();
        M._rw_stop(this._t());
        if (M._rw_state() === S_STOPPED || st === S_STOPPED) return Promise.resolve();
        return new Promise(res => {
            this._stopWaiters.push(res);
            if (!this.opts.timers) return;
            setTimeout(() => this._flushStop(), 700);
        });
    }

    // stop(), then let go of every port and the USB interface.
    async disconnect() {
        await this.stop();
        if (this._timer) clearInterval(this._timer);
        this._timer = null;
        this._running = false;
        for (const l of [...this._links.values()]) this._dropLink(l);
        if (this._access && this._access.removeEventListener) this._access.removeEventListener('statechange', this._onMidiState);
        if (this._nav && this._nav.usb && this._nav.usb.removeEventListener) {
            this._nav.usb.removeEventListener('disconnect', this._onUsbDisconnect);
            this._nav.usb.removeEventListener('connect', this._onUsbConnect);
        }
        this._access = null;
        if (this._M) this._M._rw_free();
        this._wasActive = false;
        this._setStatus({ state: 'idle', transport: null, fps: 0, why: 'disconnected', unit: null, ports: null });
    }

    // The screen to show: engine.screen()'s { seq, pixels } (240*240 RGB565, host order). Same seq: ignored.
    setFrame(seq, pixels) {
        if (seq !== undefined && seq === this._frameSeq) return;
        this._frameSeq = seq;
        if (!this._frame) this._frame = new Uint16Array(240 * 240);
        this._frame.set(pixels.length > 57600 ? pixels.subarray(0, 57600) : pixels);
        if (this._M && this._running) this._applyFrame();
    }

    // The LEDs: engine.leds()'s levels (Uint8Array(44), levels[col * 4 + line], 0..255) and PLAY green 0..255.
    setLeds(levels, playGreen = 0) {
        const key = Array.prototype.join.call(levels, ',') + '/' + (playGreen >= 176 ? 1 : 0);
        if (key === this._ledsKey) return;
        this._ledsKey = key;
        this._leds = { levels: Uint8Array.from(levels).subarray(0, 44), playGreen: playGreen | 0 };
        if (this._M && this._running) this._applyLeds();
    }

    // The unit's MIDI ports once found (the page leaves them out of its own MIDI keyboard input and MIDI out).
    unitPorts() {
        const l = this._linkById(this._M && this._running ? this._M._rw_midi_link() : -1);
        return l ? { inputId: l.input.id, outputId: l.output.id, name: l.name } : null;
    }

    stats() {
        if (!this._M || !this._running) return null;
        const p = this._M._rw_stats() >> 2, n = this._M._rw_stats_count(), h = this._M.HEAPU32, o = {};
        for (let i = 0; i < n && i < STAT_NAMES.length; i++) o[STAT_NAMES[i]] = h[p + i];
        o.budget = this._M._rw_budget();
        return o;
    }

    // The session's clock: every 4 ms on a timer (opts.timers), or the caller's (tests).
    tick() {
        const M = this._M;
        if (!M || !this._running) return;
        const now = this._t();
        for (const l of [...this._links.values()]) if (l.dead) this._dropLink(l);
        M._rw_poll(now);
        this._afterCall();
        if (this._status.state === 'connected' && now - this._fpsT >= 1000) {
            const s = this.stats();
            const fps = Math.round((s.acks - this._fpsAcks) * 10000 / (now - this._fpsT)) / 10;
            this._fpsT = now;
            this._fpsAcks = s.acks;
            this._fps = fps;
            this._setStatus({ fps });
        }
    }

    // ---------------------------------------------------------------- internals
    _t() { return this._now() >>> 0; }
    _log(msg) { for (const cb of this._ls.log) cb(msg); }
    _wantUsb() { return this.opts.usb !== 'off' && this.opts.transport !== 'midi' && this._nav && this._nav.usb; }

    async _loadModule() {
        if (this._M) return this._M;
        const mod = this.opts.module || await import(/* @vite-ignore */ this.opts.moduleUrl);
        const factory = mod.default || mod.createRemoteFM1 || mod;
        this._M = await factory();
        return this._M;
    }

    _setStatus(patch) {
        const s = { ...this._status, ...patch };
        const changed = Object.keys(patch).some(k => this._status[k] !== s[k]);
        this._status = s;
        if (changed) for (const cb of this._ls.status) cb({ ...s });
    }

    _linkById(id) {
        if (id < 0) return null;
        for (const l of this._links.values()) if (l.id === id) return l;
        return null;
    }

    _write(tag, bytes) {
        const l = this._links.get(tag);
        if (!l || l.dead) return -1;
        if (l.kind === 'midi') {
            try { l.output.send(bytes); } catch (e) { this._log(`${l.name}: send failed: ${e.message || e}`); l.dead = true; return -1; }
            return 0;
        }
        l.outstanding++;
        l.dev.transferOut(l.epOut, bytes).then(r => {
            l.outstanding--;
            if (r.status !== 'ok' || (r.bytesWritten !== undefined && r.bytesWritten !== bytes.length)) l.dead = true;
        }, e => { l.outstanding--; if (!l.closed) { this._log(`${l.name}: ${e.message || e}`); l.dead = true; } });
        return 0;
    }

    _rx(l, data) {
        const M = this._M;
        if (!M || !this._running || l.dead || l.id < 0 || !data || !data.length) return;
        const p = M._rw_buf(data.length);
        M.HEAPU8.set(data, p);
        const now = this._t();
        M._rw_rx(l.id, data.length, now);
        M._rw_poll(now);                     // answer at once (an ACK starts the frames)
        this._afterCall();
    }

    _afterCall() {
        if (this._panelDirty) this._flushPanel();
        if (this._stopWaiters.length && this._M._rw_state() === S_STOPPED) this._flushStop();
    }

    _flushStop() {
        const w = this._stopWaiters;
        this._stopWaiters = [];
        for (const r of w) r();
    }

    _event(type, index, source, value) {
        const st = this._M._rw_state();
        if (st !== S_ACTIVE && st !== S_STARTING) return;   // the unit sends its panel always: only a session counts
        const bit = 1 << index;
        if (type === EV_KEY || type === EV_BUTTON) {
            const k = type === EV_KEY;
            if (value) { if (k) this._downK |= bit; else this._downB |= bit; }
            else if (k && (this._downK & bit)) this._tapK |= bit;
            else if (!k && (this._downB & bit)) this._tapB |= bit;
        } else if (type === EV_ENC) {
            this._encAcc[index] += value;
        }
        this._panelDirty = true;
    }

    _flushPanel() {
        const M = this._M;
        this._panelDirty = false;
        const p = M._rw_panel() >> 2, h = M.HEAPU32;
        const panel = {
            keys: h[p] >>> 0, buttons: h[p + 1] >>> 0, enc: Int32Array.from(this._encAcc),
            master: h[p + 3] ? h[p + 2] : null, keyVel: new Uint8Array(NKEYS),
            taps: { keys: (this._tapK & ~h[p]) >>> 0, buttons: (this._tapB & ~h[p + 1]) >>> 0 },
            battery_mv: h[p + 4], source: 'fm1',
        };
        this._encAcc.fill(0);
        this._downK = this._downB = this._tapK = this._tapB = 0;
        this._released = false;
        this._emitPanel(panel);
    }

    // the unit is gone (or stopped): nothing stays held
    _releasePanel() {
        if (this._released) return;
        this._released = true;
        this._encAcc.fill(0);
        this._downK = this._downB = this._tapK = this._tapB = 0;
        this._emitPanel({ keys: 0, buttons: 0, enc: new Int32Array(NENC), master: null, keyVel: new Uint8Array(NKEYS),
            taps: { keys: 0, buttons: 0 }, battery_mv: 0, source: 'fm1' });
    }

    _emitPanel(p) { for (const cb of this._ls.panel) cb(p); }

    _state(st, why) {
        if (st === S_ACTIVE) {
            const M = this._M, fl = this._linkById(M._rw_frame_link()), ml = this._linkById(M._rw_midi_link());
            const hp = M._rw_hello() >> 2, h = M.HEAPU32;
            const unit = {
                name: M.UTF8ToString(M._rw_hello_str(0)), fwVer: M.UTF8ToString(M._rw_hello_str(1)),
                identity: M.UTF8ToString(M._rw_hello_str(2)), caps: h[hp], maxInFlight: h[hp + 1],
            };
            this._wasActive = true;
            this._fpsT = this._t();         // the fps window: frames acknowledged per second from here
            this._fpsAcks = this.stats().acks;
            this._setStatus({
                state: 'connected', why, transport: fl && fl.kind === 'usb' ? 'bulk' : 'midi', fps: 0, unit,
                ports: ml ? { inputId: ml.input.id, outputId: ml.output.id, name: ml.name } : null,
            });
            return;
        }
        if (st === S_DISCOVER) {
            this._releasePanel();
            if (this._wasActive) {
                this._wasActive = false;
                this._setStatus({ state: 'lost', why, transport: null, fps: 0 });
            } else {
                this._setStatus({ state: 'searching', why, transport: null, fps: 0 });
            }
            return;
        }
        if (st === S_STARTING) { this._releasePanel(); this._setStatus({ state: 'starting', why, fps: 0 }); return; }
        if (st === S_STOPPING) { this._setStatus({ state: 'stopping', why }); return; }
        if (st === S_STOPPED) {
            this._wasActive = false;
            this._releasePanel();
            this._setStatus({ state: 'stopped', why, transport: null, fps: 0 });
        }
    }

    // the session found no unit (or lost it): refresh the links
    _rescan() {
        if (this._status.state === 'lost') this._setStatus({ state: 'searching' });
        this._scanMidi();
        if (this._wantUsb() && ![...this._links.values()].some(l => l.kind === 'usb') && !this._usbOpening)
            this._pickUsb(false).then(ds => { if (ds.length && this._running) this._openUsb(ds[0]); }, () => {});
    }

    _scanMidi() {
        if (!this._access || !this._M) return;
        const pairs = candidatePairs(this._access, this._match), seen = new Set(pairs.map(p => p.key));
        for (const l of [...this._links.values()])
            if (l.kind === 'midi' && !seen.has(l.key)) this._dropLink(l);
        for (const p of pairs) {
            if ([...this._links.values()].some(l => l.key === p.key)) continue;
            const tag = this._nextTag++;
            const l = { kind: 'midi', tag, key: p.key, name: `midi "${p.name}"`, input: p.input, output: p.output, id: -1, dead: false };
            l.listener = e => this._rx(l, e.data);
            p.input.addEventListener('midimessage', l.listener);
            if (p.input.open) Promise.resolve(p.input.open()).catch(() => { l.dead = true; });
            if (p.output.open) Promise.resolve(p.output.open()).catch(() => { l.dead = true; });
            this._links.set(tag, l);
            l.id = this._addLink(KIND_SX, tag, l.name);
            if (l.id < 0) { this._dropLink(l); continue; }
            this._log(`candidate ${l.name}`);
        }
    }

    _addLink(kind, tag, name) {
        const M = this._M, n = name.length * 3 + 1, p = M._malloc(n);
        M.stringToUTF8(name, p, n);
        const id = M._rw_add_link(kind, tag, p);
        M._free(p);
        return id;
    }

    _dropLink(l) {
        if (!this._links.has(l.tag)) return;
        this._links.delete(l.tag);
        l.closed = true;
        if (l.id >= 0 && this._M) this._M._rw_remove_link(l.id, this._t());
        l.id = -1;
        if (l.kind === 'midi') {
            l.input.removeEventListener('midimessage', l.listener);
            this._log(`link gone: ${l.name}`);
        } else {
            this._log(`link gone: ${l.name}`);
            Promise.resolve().then(async () => {
                try { await l.dev.releaseInterface(l.ifnum); } catch (e) { /* unplugged */ }
                try { await l.dev.close(); } catch (e) { /* unplugged */ }
            });
        }
    }

    async _pickUsb(ask) {
        const usb = this._nav && this._nav.usb;
        if (!usb) return [];
        let devs = [];
        try { devs = (await usb.getDevices()).filter(usbMatches); } catch (e) { devs = []; }
        if (!devs.length && ask) {
            try { devs = [await usb.requestDevice({ filters: [{ ...USB_FILTER }] })]; }
            catch (e) { this._log(`WebUSB: no FM-1 chosen (${e.name || e.message || e}): frames go over MIDI`); }
        }
        return devs;
    }

    async _openUsb(dev) {
        if (this._usbOpening || [...this._links.values()].some(l => l.kind === 'usb' && l.dev === dev)) return;
        this._usbOpening = true;
        let ifnum = VENDOR_IF, epIn = BULK_EP, epOut = BULK_EP;
        try {
            if (!dev.opened) await dev.open();
            if (!dev.configuration) await dev.selectConfiguration(1);
            const ifs = (dev.configuration && dev.configuration.interfaces) || [];
            for (const it of ifs) {
                const alt = it.alternate || (it.alternates && it.alternates[0]);
                if (!alt || alt.interfaceClass !== 0xff) continue;
                const eps = alt.endpoints || [];
                const i = eps.find(e => e.direction === 'in' && e.type === 'bulk');
                const o = eps.find(e => e.direction === 'out' && e.type === 'bulk');
                if (i && o) { ifnum = it.interfaceNumber; epIn = i.endpointNumber; epOut = o.endpointNumber; break; }
            }
            await dev.claimInterface(ifnum);
        } catch (e) {
            this._usbOpening = false;
            this._log(`WebUSB: cannot open the FM-1's vendor interface (${e.message || e}): frames go over MIDI`);
            return;
        }
        this._usbOpening = false;
        if (!this._running || !this._M) { try { await dev.close(); } catch (e) { /* */ } return; }
        const tag = this._nextTag++;
        const l = { kind: 'usb', tag, dev, ifnum, epIn, epOut, name: `bulk 1209:0001 if${ifnum} EP${epOut}`, id: -1, dead: false, outstanding: 0 };
        this._links.set(tag, l);
        l.id = this._addLink(KIND_BK, tag, l.name);
        if (l.id < 0) { this._dropLink(l); return; }
        this._log(`candidate ${l.name}`);
        const read = () => {
            if (l.closed || l.dead) return;
            dev.transferIn(epIn, USB_IN_LEN).then(r => {
                if (l.closed) return;
                if (r.status === 'ok' && r.data && r.data.byteLength)
                    this._rx(l, new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength));
                if (r.status !== 'ok') { l.dead = true; return; }   // 'stall', 'babble'
                read();
            }, e => { if (!l.closed) { this._log(`${l.name}: ${e.message || e}`); l.dead = true; } });
        };
        for (let i = 0; i < this.opts.usbReads; i++) read();
    }

    _applyFrame() {
        const M = this._M;
        M.HEAPU16.set(this._frame, M._rw_frame_buf() >> 1);
        M._rw_set_frame();
    }

    _applyLeds() {
        const M = this._M;
        M.HEAPU8.set(this._leds.levels, M._rw_leds_buf());
        M._rw_set_leds(this._leds.playGreen);
    }
}

export default RemoteFM1;
