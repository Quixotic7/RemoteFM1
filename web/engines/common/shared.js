// SPDX-License-Identifier: GPL-3.0-only
// What the native and the .rfm1 engines share (web/engine/API.md): the resampler from the device's 44.1 kHz to the
// context rate, raw MIDI bytes <-> USB-MIDI packets, and C strings out of a module's memory. Plain JavaScript that
// loads in an AudioWorkletGlobalScope (no fetch, no DOM, no TextDecoder) and in Node.
//
// THE RESAMPLER (44100 Hz -> the context rate; bypassed at exactly 44100): a windowed-sinc interpolator, 32 taps
// (16 input samples each side), Kaiser window beta 8 (about 80 dB stop band), cutoff 0.47 x the lower of the two
// rates (20.7 kHz from 44.1 kHz up; the transition band lies above it and around the Nyquist frequency, where FM-1
// firmwares put next to nothing). The kernel is tabulated at 256 sub-sample phases, each row normalised to unity
// DC gain, and interpolated linearly between rows. Group delay: 16 input samples (0.36 ms). Cost: 64
// multiply-adds per output frame (two channels), about 3 M/s at 48 kHz.

export const FS = 44100;
export const BLOCK = 128;

// ---------------------------------------------------------------------------------------------- the resampler ---
export const RS_HALF = 16;                 // taps each side
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

export class Resampler {
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

// ------------------------------------------------------------------------------------------- audio input ---
// A cart's live audio input ("ain1", API.md section 6): the context's input quanta -> 44.1 kHz -> a FIFO the engine
// reads one 128-frame block from right before each rfm1_audio. The push side is the resampler above run the other way
// (the same kernel, its cutoff 0.47 x the lower rate; bypassed at 44100): 44.1 kHz frame k is the input at context
// instant k * rate / 44100 - 1. The FIFO starts with AIN_DEPTH frames of silence, enough for what the output side
// pulls ahead of the input (the output resampler's 16 taps and a block's granularity, ~160 frames at 48 kHz), so the
// input frame paired with output frame j is j - AIN_DEPTH: the delay is fixed, it cannot drift (both sides advance
// one quantum per process()). A read that finds too little (an underrun) is padded with silence and the FIFO filled
// back to AIN_DEPTH; a push that leaves more than AIN_DEPTH + AIN_SLACK (an overrun) drops the oldest down to
// AIN_DEPTH. delay44(): the input-to-output delay in 44.1 kHz frames, from the context's input to the context's output
// (resamplers included), which is what the cart's latency_frames carries (with the capture latency added).
export const AIN_DEPTH = 2 * BLOCK;
export const AIN_SLACK = 3 * BLOCK;
const AIN_CAP = 8192;                                    // (a power of two; far above AIN_DEPTH + AIN_SLACK + a quantum)

export class AudioIn {
    constructor(inRate) {
        this.rate = inRate;
        this.step = inRate / FS;                         // context samples per 44.1 kHz frame
        this.kernel = inRate === FS ? null : makeKernel(0.47 * Math.min(FS, inRate) / inRate);
        this.L = new Float32Array(AIN_CAP);              // the FIFO (44.1 kHz)
        this.R = new Float32Array(AIN_CAP);
        this.rcap = 4096;                                // the resampler's input (context rate)
        this.iL = new Float32Array(this.rcap);
        this.iR = new Float32Array(this.rcap);
        this.under = 0; this.over = 0;
        this.reset();
    }
    // empty: `prefill` frames of silence (AIN_DEPTH; less when the reader already runs ahead: engines/rfm1 audioIn),
    // the resampler's history cleared
    reset(prefill = AIN_DEPTH) {
        this.L.fill(0); this.R.fill(0);
        this.wr = prefill; this.rd = 0;                  // (absolute counts; the FIFO's slots are mod AIN_CAP)
        this.pushed = 0; this.read = 0; this.padded = prefill; this.dropped = 0;
        this.iL.fill(0); this.iR.fill(0);
        this.n = RS_HALF; this.pos = RS_HALF - 1;
    }
    get avail() { return this.wr - this.rd; }
    put(l, r) { const i = this.wr++ & (AIN_CAP - 1); this.L[i] = l; this.R[i] = r; this.pushed++; }
    // one quantum of the context's input (R may be L: a mono source)
    push(inL, inR) {
        const n = inL.length;
        if (!this.kernel) { for (let i = 0; i < n; i++) this.put(inL[i], inR[i]); }
        else {
            if (this.n + n > this.rcap) {                // drop what the filter no longer reaches
                const keep = Math.floor(this.pos) - RS_HALF;
                if (keep > 0) { this.iL.copyWithin(0, keep, this.n); this.iR.copyWithin(0, keep, this.n); this.n -= keep; this.pos -= keep; }
            }
            this.iL.set(inL, this.n); this.iR.set(inR, this.n); this.n += n;
            const k = this.kernel, L = this.iL, R = this.iR;
            while (Math.floor(this.pos) + RS_HALF < this.n) {
                const i0 = Math.floor(this.pos), fp = (this.pos - i0) * RS_PHASES, p = fp | 0, t = fp - p;
                const a = p * RS_TAPS, b = a + RS_TAPS, s = i0 - RS_HALF + 1;
                let l = 0, r = 0;
                for (let j = 0; j < RS_TAPS; j++) { const w = k[a + j] + (k[b + j] - k[a + j]) * t; l += L[s + j] * w; r += R[s + j] * w; }
                this.put(l, r);
                this.pos += this.step;
            }
        }
        if (this.avail > AIN_DEPTH + AIN_SLACK) {        // an overrun: back to AIN_DEPTH
            const d = this.avail - AIN_DEPTH;
            this.rd += d; this.dropped += d; this.over++;
        }
    }
    // the next n frames (n <= AIN_DEPTH) as interleaved int16 into out
    readInt16(out, n) {
        const have = this.avail;
        if (have < n) {                                  // an underrun: what there is, then silence, AIN_DEPTH left after
            this.under++;
            const pad = n - have + AIN_DEPTH;
            for (let i = 0; i < pad; i++) { const s = this.wr++ & (AIN_CAP - 1); this.L[s] = 0; this.R[s] = 0; }
            this.padded += pad;
        }
        const L = this.L, R = this.R;
        for (let i = 0; i < n; i++) {
            const s = this.rd++ & (AIN_CAP - 1);
            let l = L[s] * 32768, r = R[s] * 32768;
            out[2 * i] = l >= 32767 ? 32767 : l <= -32768 ? -32768 : Math.round(l);
            out[2 * i + 1] = r >= 32767 ? 32767 : r <= -32768 ? -32768 : Math.round(r);
        }
        this.read += n;
    }
    // the FIFO's delay now (44.1 kHz frames from an input frame's arrival to its read: silence put in minus input dropped)
    get fifoDelay() { return this.padded - this.dropped; }
    // context input -> context output, in 44.1 kHz frames: the FIFO, this side's resampler (one context sample) and the
    // output side's (one 44.1 kHz frame; none when the context runs at 44100)
    delay44() { return this.fifoDelay + (this.kernel ? 1 + FS / this.rate : 0); }
}

// ------------------------------------------------------------------------------------------------ USB-MIDI ---
// raw MIDI bytes -> USB-MIDI event packets (cable 0, the byte order the cores take: CIN | b0 << 8 | b1 << 16 | b2 << 24)
export class MidiParser {
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
export function packetBytes(pkt, out) {
    const cin = pkt & 0x0f;
    let n = CIN_LEN[cin];
    if (cin >= 0x8 && cin <= 0xe) {
        const st = pkt >>> 8 & 0xff;
        n = st >= 0xc0 && st < 0xe0 ? 2 : 3;
    }
    for (let i = 0; i < n; i++) out.push(pkt >>> (8 * (i + 1)) & 0xff);
}

// ---------------------------------------------------------------------------------------------- C strings ---
export function utf8(bytes, p) {           // a NUL-terminated C string (no TextDecoder in an AudioWorkletGlobalScope)
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

// ------------------------------------------------------------------------------------------ live labels ---
// FM1VST fm1core.h FM1CORE_LBL_*: 49 slots (FIZZ's SysEx LABELS order): 8 knobs (FM1P_E_*), 14 buttons (FM1P_B_*),
// the white keys W1..W16, the black keys B1..B11; a label is at most 10 characters
export const LBL = Object.freeze({ KNOB0: 0, BTN0: 8, WHITE0: 22, BLACK0: 38, N: 49 });
export const LBL_N = 49, LBL_LEN = 11;
// one label set (API.md engine.labels()): list[49] (string, "" = no job, null = not given) and its slices
export function labelSet(seq, list) {
    return { seq, list, knobs: list.slice(0, 8), buttons: list.slice(8, 22), white: list.slice(22, 38), black: list.slice(38, 49) };
}
// a label read through a C-string reader (native: utf8): at most 10 printable ASCII characters, trimmed; null: none
export function cleanLabel(s) {
    if (s == null) return null;
    let o = "";
    for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c >= 0x20 && c < 0x7f) o += s[i]; }
    return o.trim().slice(0, LBL_LEN - 1).trim();
}

export function now() {
    return typeof performance !== "undefined" && performance.now ? performance.now() : Date.now();
}
