// SPDX-License-Identifier: GPL-3.0-only
// The native engine (web/engine/API.md): FM1VST's own cores, compiled to WebAssembly by build.sh, one standalone
// module per core (web/build/native/<id>.wasm). This file is the whole runtime: it loads inside an
// AudioWorkletGlobalScope (no fetch, no DOM, no TextDecoder, no dynamic import) and in Node.
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
// THE RESAMPLER (44100 Hz -> the context rate; bypassed at exactly 44100): a windowed-sinc interpolator, 32 taps
// (16 input samples each side), Kaiser window beta 8 (about 80 dB stop band), cutoff 0.47 x the lower of the two
// rates (20.7 kHz from 44.1 kHz up; the transition band lies above it and around the Nyquist frequency, where FM-1
// firmwares put next to nothing). The kernel is tabulated at 256 sub-sample phases, each row normalised to unity
// DC gain, and interpolated linearly between rows. Group delay: 16 input samples (0.36 ms). Cost: 64
// multiply-adds per output frame (two channels), about 3 M/s at 48 kHz.
//
// THE COROUTINE (Dinghy's app_main, coro_wasm.c): the module carries Binaryen's Asyncify. fm1w_co_call runs the
// coroutine as a nested call of the export fmw_co_entry (first run, or a rewind into its last yield) and stops the
// unwind when it yielded; fm1w_co_yield starts the unwind, or stops the rewind when it is the call being resumed.

export const ENGINE = { name: "native", version: "0.1" };

const FS = 44100;
const BLOCK = 128;
const LCD_PIXELS = 240 * 240;
const NKEY = 27;
const NENC = 7;

// ---------------------------------------------------------------------------------------------- the resampler ---
const RS_HALF = 16;                 // taps each side
const RS_TAPS = 2 * RS_HALF;
const RS_PHASES = 256;
const RS_BETA = 8;

function besselI0(x) {
    let s = 1, t = 1;
    for (let k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
        if (t < 1e-12 * s) break;
    }
    return s;
}

// rows[p * RS_TAPS + j]: the weight of input sample (i0 - RS_HALF + 1 + j) for an output at i0 + p / RS_PHASES
function makeKernel(fc /* cutoff as a fraction of the input rate, < 0.5 */) {
    const rows = new Float32Array((RS_PHASES + 1) * RS_TAPS);
    const i0b = besselI0(RS_BETA);
    for (let p = 0; p <= RS_PHASES; p++) {
        const f = p / RS_PHASES;
        let sum = 0;
        for (let j = 0; j < RS_TAPS; j++) {
            const x = j - RS_HALF + 1 - f;               // distance (input samples) from the output instant
            const w = x / RS_HALF;
            const win = Math.abs(w) >= 1 ? 0 : besselI0(RS_BETA * Math.sqrt(1 - w * w)) / i0b;
            const a = 2 * Math.PI * fc * x;
            const sinc = x === 0 ? 2 * fc : Math.sin(a) / (Math.PI * x);
            const v = sinc * win;
            rows[p * RS_TAPS + j] = v;
            sum += v;
        }
        for (let j = 0; j < RS_TAPS; j++) rows[p * RS_TAPS + j] /= sum;
    }
    return rows;
}

class Resampler {
    constructor(outRate, pull /* () => [Float32Array L, Float32Array R] of BLOCK frames */) {
        this.step = FS / outRate;                        // input samples per output sample
        this.kernel = makeKernel(0.47 * Math.min(FS, outRate) / FS);
        this.pull = pull;
        this.cap = 4096;
        this.L = new Float32Array(this.cap);
        this.R = new Float32Array(this.cap);
        this.n = RS_HALF;                                // valid input samples (zeros first: the filter's history)
        this.pos = RS_HALF - 1;                          // the next output's instant, in input samples
    }
    render(outL, outR) {
        const k = this.kernel, L = this.L, R = this.R;
        for (let o = 0; o < outL.length; o++) {
            while (Math.floor(this.pos) + RS_HALF >= this.n) this.fill();
            const i0 = Math.floor(this.pos);
            const fp = (this.pos - i0) * RS_PHASES;
            const p = fp | 0, t = fp - p;
            const a = p * RS_TAPS, b = a + RS_TAPS, s = i0 - RS_HALF + 1;
            let l = 0, r = 0;
            for (let j = 0; j < RS_TAPS; j++) {
                const w = k[a + j] + (k[b + j] - k[a + j]) * t;
                l += L[s + j] * w;
                r += R[s + j] * w;
            }
            outL[o] = l;
            outR[o] = r;
            this.pos += this.step;
        }
    }
    fill() {
        // drop what the filter no longer reaches, then append one block
        const keep = Math.floor(this.pos) - RS_HALF;
        if (keep > 0 && this.n + BLOCK > this.cap) {
            this.L.copyWithin(0, keep, this.n);
            this.R.copyWithin(0, keep, this.n);
            this.n -= keep;
            this.pos -= keep;
        }
        const [bl, br] = this.pull();
        this.L.set(bl, this.n);
        this.R.set(br, this.n);
        this.n += BLOCK;
    }
}

// ------------------------------------------------------------------------------------------------ USB-MIDI ---
// raw MIDI bytes -> USB-MIDI event packets (cable 0, the byte order the cores take: CIN | b0 << 8 | b1 << 16 | b2 << 24)
class MidiParser {
    constructor() { this.status = 0; this.data = []; this.sx = []; }
    push(byte, out) {
        if (byte >= 0xf8) { out.push(0x0f | byte << 8); return; }        // real time: anywhere, one byte
        if (byte === 0xf0) { this.sx = [0xf0]; this.status = 0xf0; return; }
        if (byte === 0xf7) {
            if (this.status === 0xf0) { this.sx.push(0xf7); this.flushSysex(out, true); }
            this.status = 0;
            return;
        }
        if (byte >= 0x80) {
            if (this.status === 0xf0) this.flushSysex(out, true, true);   // (cut off by a status byte)
            this.status = byte;
            this.data = [];
            if (byte === 0xf6) { out.push(0x05 | byte << 8); this.status = 0; }   // tune request
            else if (byte >= 0xf4) this.status = 0;                         // undefined: dropped
            return;
        }
        const st = this.status;
        if (st === 0xf0) {
            this.sx.push(byte);
            if (this.sx.length === 3) this.flushSysex(out, false);
            return;
        }
        if (!st) return;                                                    // (data with no status)
        this.data.push(byte);
        const need = (st >= 0xc0 && st < 0xe0) || st === 0xf1 || st === 0xf3 ? 1 : 2;
        if (this.data.length < need) return;
        const [d1, d2 = 0] = this.data;
        const cin = st >= 0xf0 ? (need === 1 ? 0x02 : 0x03) : st >> 4;
        out.push((cin | st << 8 | d1 << 16 | d2 << 24) >>> 0);
        this.data = [];
        if (st >= 0xf0) this.status = 0;                                    // (no running status for system common)
    }
    flushSysex(out, end, cut = false) {
        const b = this.sx;
        if (cut) { this.sx = []; this.status = 0; return; }
        if (!end) { out.push((0x04 | b[0] << 8 | b[1] << 16 | b[2] << 24) >>> 0); this.sx = []; return; }
        const n = b.length;                                                 // 1..3 bytes ending with F7
        if (n === 0) return;
        const cin = 0x04 + n;
        out.push((cin | (b[0] | 0) << 8 | (b[1] | 0) << 16 | (b[2] | 0) << 24) >>> 0);
        this.sx = [];
    }
}
const CIN_LEN = [0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1];
function packetBytes(pkt, out) {
    const cin = pkt & 0x0f;
    let n = CIN_LEN[cin];
    if (cin >= 0x8 && cin <= 0xe) {
        const st = pkt >>> 8 & 0xff;
        n = st >= 0xc0 && st < 0xe0 ? 2 : 3;
    }
    for (let i = 0; i < n; i++) out.push(pkt >>> (8 * (i + 1)) & 0xff);
}

// ----------------------------------------------------------------------------------------------- the module ---
function utf8(bytes, p) {           // a NUL-terminated C string (no TextDecoder in an AudioWorkletGlobalScope)
    if (!p) return null;
    let s = "", e = p;
    while (bytes[e]) e++;
    for (let i = p; i < e;) {
        let c = bytes[i++];
        if (c >= 0xf0) c = (c & 7) << 18 | (bytes[i++] & 63) << 12 | (bytes[i++] & 63) << 6 | bytes[i++] & 63;
        else if (c >= 0xe0) c = (c & 15) << 12 | (bytes[i++] & 63) << 6 | bytes[i++] & 63;
        else if (c >= 0xc0) c = (c & 31) << 6 | bytes[i++] & 63;
        s += String.fromCodePoint(c);
    }
    return s;
}

function now() {
    return typeof performance !== "undefined" && performance.now ? performance.now() : Date.now();
}

async function instantiate(wasm, log) {
    const module = wasm instanceof WebAssembly.Module ? wasm : await WebAssembly.compile(wasm);
    let ex = null, u8 = null, dv = null, buf = null;
    const views = () => {
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
            if (started) ex.asyncify_start_rewind(data);
            ex.fmw_co_entry(c);
            if (ex.asyncify_get_state() === 1) ex.asyncify_stop_unwind();   // it yielded
        },
        fm1w_co_yield(data) {
            if (ex.asyncify_get_state() === 2) { ex.asyncify_stop_rewind(); return; }   // resumed here
            ex.asyncify_start_unwind(data);
        },
    };
    const imports = { env, wasi_snapshot_preview1: wasi };
    for (const imp of WebAssembly.Module.imports(module)) {
        const ns = imports[imp.module];
        if (imp.kind === "function" && (!ns || !(imp.name in ns)))
            throw new Error(`native engine: the core imports ${imp.module}.${imp.name}, which engine.js lacks`);
    }
    const inst = await WebAssembly.instantiate(module, imports);
    ex = inst.exports;
    if (ex._initialize) ex._initialize();
    return { ex, mem: () => (views(), { u8, dv, buf }) };
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
// options.log(fd, line): the firmware's printf (default: dropped)
export async function createEngine({ sampleRate, files, flash = null, options = {} }) {
    if (!files || !files.wasm) throw new Error("native engine: files.wasm is required");
    const rate = sampleRate || FS;
    const log = options.log || (() => {});
    const rt = await instantiate(files.wasm, log);
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
