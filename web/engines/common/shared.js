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

export function now() {
    return typeof performance !== "undefined" && performance.now ? performance.now() : Date.now();
}
