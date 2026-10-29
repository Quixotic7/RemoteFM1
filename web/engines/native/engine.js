// SPDX-License-Identifier: GPL-3.0-only
// The native engine (web/engine/API.md): FM1VST's own cores, compiled to WebAssembly by build.sh, one standalone
// module per core (web/build/native/<id>.wasm). This file and ../common/shared.js (the resampler, the MIDI packets) are
// the whole runtime: they load inside an AudioWorkletGlobalScope (no fetch, no DOM, no TextDecoder, no dynamic import)
// and in Node.
//
// THE MODULE. A standalone wasm (emcc -sSTANDALONE_WASM: no Emscripten JavaScript), so one engine.js serves every
// core. Its imports are a few WASI calls (fd_write for the firmware's printf, the rest stubs: no filesystem, the
// flash comes in and goes out as bytes), emscripten_notify_memory_growth, and the coroutine's two calls (the Dinghy
// cores). Its exports are wasm_glue.c's fmw_* (the core ABI's descriptor behind a flat C interface).
//
// THE CLOCK is the core's own (core-api/fm1core.h, FM1VST engine/device.cpp run_ms): fmw_block() runs device
// milliseconds (tick, frame every 15 ms or idle, audio blocks as they fall due at 44100 Hz) until the next 128-frame
// block exists. render() pulls blocks as the resampler needs them, so the device runs exactly as far as the
// output requires: no lookahead beyond one block and the filter's half length.
//
// THE RESAMPLER (44100 Hz -> the context rate) and the MIDI packets: ../common/shared.js, shared with the .rfm1
// engine (web/engines/rfm1).
//
// THE COROUTINE (Dinghy's app_main, coro_wasm.c): the module carries Binaryen's Asyncify. fm1w_co_call runs the
// coroutine as a nested call of the export fmw_co_entry (first run, or a rewind into its last yield) and stops the
// unwind when it yielded; fm1w_co_yield starts the unwind, or stops the rewind when it is the call being resumed.

import { FS, BLOCK, RS_HALF, Resampler, MidiParser, packetBytes, utf8, now } from "../common/shared.js";

export const ENGINE = { name: "native", version: "0.1" };

const LCD_PIXELS = 240 * 240;
const NKEY = 27;
const NENC = 7;

// the module's imports: the only functions a core can reach (an author's build is checked against exactly this set
// before the site publishes it: PROVIDED_IMPORTS below, web/engines/native/fm1web.mjs checkModule)
function makeImports(rt, log) {
    let ex = null, u8 = null, dv = null, buf = null;
    const views = () => {
        ex = rt.ex;
        if (buf !== ex.memory.buffer) {
            buf = ex.memory.buffer;
            u8 = new Uint8Array(buf);
            dv = new DataView(buf);
        }
    };
    let line = [];
    const ENOSYS = 52, EBADF = 8;
    const wasi = {
        fd_write(fd, iovs, n, nwritten) {          // printf: the firmware's log, line by line
            views();
            let total = 0;
            for (let i = 0; i < n; i++) {
                const p = dv.getUint32(iovs + 8 * i, true), len = dv.getUint32(iovs + 8 * i + 4, true);
                for (let k = 0; k < len; k++) {
                    const c = u8[p + k];
                    if (c === 10) { log(fd, String.fromCharCode(...line)); line = []; }
                    else if (line.length < 4096) line.push(c);
                }
                total += len;
            }
            dv.setUint32(nwritten, total, true);
            return 0;
        },
        fd_read: () => EBADF,
        fd_close: () => 0,
        fd_seek: () => ENOSYS,
        clock_time_get(id, precision, out) {       // (diagnostics only: the cores run on simulated time)
            views();
            dv.setBigUint64(out, BigInt(Math.round(now() * 1e6)), true);
            return 0;
        },
        proc_exit(code) { throw new Error(`the core called proc_exit(${code})`); },
        environ_sizes_get(pc, ps) { views(); dv.setUint32(pc, 0, true); dv.setUint32(ps, 0, true); return 0; },
        environ_get: () => 0,
        args_sizes_get(pc, ps) { views(); dv.setUint32(pc, 0, true); dv.setUint32(ps, 0, true); return 0; },
        args_get: () => 0,
        fd_fdstat_get: () => EBADF,
        fd_prestat_get: () => EBADF,
        random_get(p, n) {                         // (deterministic: a simulated device)
            views();
            let x = 0x9e3779b9;
            for (let i = 0; i < n; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; u8[p + i] = x & 0xff; }
            return 0;
        },
    };
    const env = {
        emscripten_notify_memory_growth() {},
        // the coroutine (coro_wasm.c)
        fm1w_co_call(c, data, started) {
            const ex = rt.ex;
            if (started) ex.asyncify_start_rewind(data);
            ex.fmw_co_entry(c);
            if (ex.asyncify_get_state() === 1) ex.asyncify_stop_unwind();   // it yielded
        },
        fm1w_co_yield(data) {
            const ex = rt.ex;
            if (ex.asyncify_get_state() === 2) { ex.asyncify_stop_rewind(); return; }   // resumed here
            ex.asyncify_start_unwind(data);
        },
    };
    return { imports: { env, wasi_snapshot_preview1: wasi }, mem: () => (views(), { u8, dv, buf }) };
}

// { module: [names] }: every function import engine.js gives a core (and nothing else: no memory, table or global)
export const PROVIDED_IMPORTS = Object.freeze(Object.fromEntries(Object.entries(makeImports({}, () => {}).imports)
    .map(([m, o]) => [m, Object.freeze(Object.keys(o).sort())])));
// the exports engine.js calls (wasm_glue.c); a core on the coroutine also exports Asyncify's and fmw_co_entry
export const REQUIRED_EXPORTS = Object.freeze(["memory", "fmw_malloc", "fmw_free", "fmw_open", "fmw_boot", "fmw_block",
    "fmw_ms", "fmw_run_ms", "fmw_input", "fmw_enc", "fmw_key_vel", "fmw_midi_in", "fmw_midi_out", "fmw_lcd_writes",
    "fmw_lcd", "fmw_leds", "fmw_play_green", "fmw_halted", "fmw_flash", "fmw_flash_size", "fmw_flash_dirty",
    "fmw_flash_sync", "fmw_shutdown", "fmw_str", "fmw_keymap", "fmw_btn_id", "fmw_enc_id", "fmw_enc_dir"]);
export const COROUTINE_EXPORTS = Object.freeze(["fmw_co_entry", "asyncify_start_unwind", "asyncify_stop_unwind",
    "asyncify_start_rewind", "asyncify_stop_rewind", "asyncify_get_state"]);

// what is wrong with a module, as engine.js would run it: [] when nothing. flags (an author's build's manifest):
// { asyncify } must say whether the module runs on the coroutine
export function moduleProblems(module, flags = null) {
    const bad = [];
    const imps = WebAssembly.Module.imports(module), exps = new Set(WebAssembly.Module.exports(module).map((e) => e.name));
    for (const imp of imps) {
        const names = PROVIDED_IMPORTS[imp.module];
        if (imp.kind !== "function") bad.push(`imports a ${imp.kind}, ${imp.module}.${imp.name} (only functions)`);
        else if (!names || !names.includes(imp.name)) bad.push(`imports ${imp.module}.${imp.name}, which engine.js does not provide`);
    }
    for (const e of REQUIRED_EXPORTS) if (!exps.has(e)) bad.push(`does not export ${e}`);
    const coro = imps.some((i) => i.name === "fm1w_co_yield" || i.name === "fm1w_co_call");
    if (coro) for (const e of COROUTINE_EXPORTS) if (!exps.has(e)) bad.push(`runs on the coroutine but does not export ${e}`);
    if (flags && typeof flags.asyncify === "boolean" && flags.asyncify !== coro)
        bad.push(`its manifest says asyncify ${flags.asyncify}, the module ${coro ? "runs" : "does not run"} on the coroutine`);
    return bad;
}

async function instantiate(wasm, log, flags = null) {
    const module = wasm instanceof WebAssembly.Module ? wasm : await WebAssembly.compile(wasm);
    const bad = moduleProblems(module, flags);
    if (bad.length) throw new Error(`native engine: the core ${bad.join("; ")}`);
    const rt = { ex: null };
    const { imports, mem } = makeImports(rt, log);
    const inst = await WebAssembly.instantiate(module, imports);
    rt.ex = inst.exports;
    if (rt.ex._initialize) rt.ex._initialize();
    return { ex: rt.ex, mem };
}

class NativeEngine {
    constructor(rt, sampleRate) {
        this.rt = rt;
        this.ex = rt.ex;
        this.sampleRate = sampleRate;
        this.disposed = false;
        this.lastMaster = undefined;
        this.midiPending = [];
        this.parser = new MidiParser();
        this.ledSeq = 0;
        this.ledLast = new Uint8Array(45);
        this.screenSeq = -1;
        this.screenCopy = null;
        this.blkL = new Float32Array(BLOCK);
        this.blkR = new Float32Array(BLOCK);
        this.rs = sampleRate === FS ? null : new Resampler(sampleRate, () => this.pullBlock());
        this._info = this.readInfo();
    }

    pullBlock() {
        const p = this.ex.fmw_block();
        const { buf } = this.rt.mem();
        const s = new Int16Array(buf, p, 2 * BLOCK);
        const L = this.blkL, R = this.blkR;
        for (let i = 0; i < BLOCK; i++) {
            L[i] = s[2 * i] * (1 / 32768);
            R[i] = s[2 * i + 1] * (1 / 32768);
        }
        return [L, R];
    }

    readInfo() {
        const ex = this.ex;
        const { u8 } = this.rt.mem();
        const s = (k) => utf8(u8, ex.fmw_str(k));
        const km = ex.fmw_keymap();
        let keymap = null;
        if (km) {
            keymap = [];
            for (let r = 0; r < 6; r++) {
                const row = [];
                for (let c = 0; c < 11; c++) row.push((u8[km + r * 11 + c] << 24) >> 24);
                keymap.push(row);
            }
        }
        const buttonNames = [];
        for (let i = 0; i < 14; i++) buttonNames.push(s(100 + i));
        const bid = ex.fmw_btn_id(), eid = ex.fmw_enc_id(), edir = ex.fmw_enc_dir();
        return {
            id: s(0),
            name: s(1),
            version: s(2),
            license: s(5),
            source: s(3),
            credits: s(6),
            family: s(4),
            latencyFrames: this.rs ? Math.round(RS_HALF * this.sampleRate / FS) : 0,
            keymap,
            buttonNames,
            // (beyond API.md) the firmware's panel table after power-on, as FM1VST's script runner uses it:
            // btnId[label] = matrix button (labels in FM1P_B_* order: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY
            // REC OCT- OCT+), encId[role] = matrix encoder and encDir[role] = +1 / -1 (roles SELECT ALGO PRESETS
            // KNOB1..KNOB4)
            panelTable: {
                btnId: Array.from(u8.subarray(bid, bid + 14)),
                encId: Array.from(u8.subarray(eid, eid + 7)),
                encDir: Array.from(u8.subarray(edir, edir + 7), (v) => (v << 24) >> 24),
            },
        };
    }

    info() { return this._info; }

    setPanel(p) {
        if (this.disposed) return;
        const ex = this.ex;
        const keys = (p.keys >>> 0) & ((1 << NKEY) - 1);
        if (p.keyVel) {                          // velocities before the keys' bits (fm1core.h)
            const lean = p.keyLean;
            for (let k = 0; k < NKEY; k++) ex.fmw_key_vel(k, p.keyVel[k] | 0, lean ? lean[k] | 0 : 0);
        }
        if (p.enc) for (let i = 0; i < NENC; i++) if (p.enc[i]) ex.fmw_enc(i, p.enc[i] | 0);
        const t = p.taps || {};
        // MASTER: the firmware's own default level stands until the pot moves. The first value the page gives is
        // taken as the pot's resting place; any later change is written to the panel (0..1023)
        let master = -1;
        if (typeof p.master === "number") {
            const m = Math.max(0, Math.min(1023, p.master | 0));
            if (this.lastMaster !== undefined && m !== this.lastMaster) master = m;
            this.lastMaster = m;
        }
        ex.fmw_input(keys, (p.buttons >>> 0) & 0x3fff, (t.keys >>> 0) || 0, (t.buttons >>> 0) || 0, master);
    }

    render(left, right) {
        if (this.disposed) { left.fill(0); right.fill(0); return; }
        this.feedMidi();
        if (!this.rs) {
            // 44.1 kHz: the device's blocks as they are (a render quantum is one block)
            let o = 0;
            while (o < left.length) {
                const [L, R] = this.pullBlock();
                const n = Math.min(BLOCK, left.length - o);
                left.set(n === BLOCK ? L : L.subarray(0, n), o);
                right.set(n === BLOCK ? R : R.subarray(0, n), o);
                o += n;
            }
            return;
        }
        this.rs.render(left, right);
    }

    feedMidi() {
        while (this.midiPending.length) {
            if (!this.ex.fmw_midi_in(this.midiPending[0])) break;   // (no room: next quantum)
            this.midiPending.shift();
        }
    }

    screen() {
        const seq = this.ex.fmw_lcd_writes() >>> 0;
        if (seq !== this.screenSeq || !this.screenCopy || this.screenCopy.length === 0) {
            const p = this.ex.fmw_lcd();
            const { buf } = this.rt.mem();
            this.screenCopy = new Uint16Array(buf.slice(p, p + 2 * LCD_PIXELS));
            this.screenSeq = seq;
        }
        return { seq, pixels: this.screenCopy };
    }

    leds() {
        const { u8 } = this.rt.mem();
        const p = this.ex.fmw_leds();
        const levels = u8.slice(p, p + 44);
        const playGreen = this.ex.fmw_play_green() & 0xff;
        let changed = playGreen !== this.ledLast[44];
        for (let i = 0; i < 44 && !changed; i++) changed = levels[i] !== this.ledLast[i];
        if (changed) {
            this.ledSeq++;
            this.ledLast.set(levels);
            this.ledLast[44] = playGreen;
        }
        return { seq: this.ledSeq, levels, playGreen };
    }

    midiIn(bytes) {
        for (const b of bytes) this.parser.push(b & 0xff, this.midiPending);
        if (this.midiPending.length > 4096) this.midiPending.splice(0, this.midiPending.length - 4096);
    }

    // (called every render quantum by the page's worklet: one 4-byte cell kept for the packets, freed with the module)
    midiOut() {
        const ex = this.ex;
        if (this.disposed) return new Uint8Array(0);
        const cell = this.outCell || (this.outCell = ex.fmw_malloc(4));
        const out = [];
        for (let i = 0; i < 4096 && ex.fmw_midi_out(cell); i++) {
            const { dv } = this.rt.mem();
            packetBytes(dv.getUint32(cell, true), out);
        }
        return Uint8Array.from(out);
    }

    flash() {
        const seq = this.ex.fmw_flash_dirty() >>> 0;
        const self = this;
        return {
            seq,
            // the image on request (reading it costs a flash_sync and a 1 MiB copy): the firmware's deferred saves
            // written first, so the bytes are current
            get bytes() { return self.flashBytes(); },
        };
    }

    flashBytes() {
        if (this.disposed) return null;
        if (!this.ex.fmw_halted()) this.ex.fmw_flash_sync();
        const p = this.ex.fmw_flash(), n = this.ex.fmw_flash_size();
        if (!p) return null;
        const { buf } = this.rt.mem();
        return new Uint8Array(buf.slice(p, p + n));
    }

    halted() { return this.disposed ? 0 : this.ex.fmw_halted() >>> 0; }

    dispose() {
        if (this.disposed) return;
        try { this.ex.fmw_shutdown(); } catch (e) { /* (the module goes anyway) */ }
        this.disposed = true;
        this.rt = this.ex = null;
    }

    // (beyond API.md, for tests) the device's simulated milliseconds since power-on
    deviceMs() { return this.disposed ? 0 : this.ex.fmw_ms(); }
    // (beyond API.md, for tests) n device milliseconds with their audio dropped (FM1VST's Device::run_ms loop)
    runMs(n) { if (!this.disposed) this.ex.fmw_run_ms(n); }
}

// (for tests) the MIDI byte <-> USB-MIDI packet conversions
export const _midi = { MidiParser, packetBytes };

// files.wasm: the core's .wasm as an ArrayBuffer / typed array, or an already compiled WebAssembly.Module.
// files.flags: an author's build's manifest flags ({ asyncify, abi }, the catalog's native entry): checked against
// the module before it runs. options.log(fd, line): the firmware's printf (default: dropped)
export async function createEngine({ sampleRate, files, flash = null, options = {} }) {
    if (!files || !files.wasm) throw new Error("native engine: files.wasm is required");
    const flags = files.flags || null;
    if (flags && flags.abi !== undefined && flags.abi !== 6) throw new Error(`native engine: the core is for ABI ${flags.abi}, not 6`);
    const rate = sampleRate || FS;
    const log = options.log || (() => {});
    const rt = await instantiate(files.wasm, log, flags);
    const ex = rt.ex;
    if (!ex.fmw_open()) throw new Error("native engine: the module has no ABI 6 core");
    let p = 0, n = 0;
    if (flash instanceof ArrayBuffer) flash = new Uint8Array(flash);
    if (flash && flash.length) {
        n = flash.length;
        p = ex.fmw_malloc(n);
        rt.mem().u8.set(flash, p);
    }
    ex.fmw_boot(p, n);
    if (p) ex.fmw_free(p);
    return new NativeEngine(rt, rate);
}
