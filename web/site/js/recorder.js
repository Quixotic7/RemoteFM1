// SPDX-License-Identifier: GPL-3.0-only
// The recorder's core, no DOM (web/site/test-wav.mjs runs it in Node): the running firmware's audio as a WAV file.
//
// The capture is the worklet's (worklet/host.js): the exact float PCM the engine renders, stereo, at the
// AudioContext's rate, before the page's volume (audio.js: a GainNode after the worklet), in chunks of about 0.25 s
// whose buffers are transferred, not copied. Here each chunk becomes interleaved little-endian integer PCM at once
// (24-bit by default, 16-bit as an option), kept as a list of chunks, never one growing buffer; Stop puts a RIFF
// header in front (wavParts) and the page makes one Blob of the list.
//
//   floatToPcm(l, r, frames, bits)       -> { bytes: Uint8Array (interleaved L R), clips }
//   wavHeader({ sampleRate, bits, frames, channels = 2 }) -> Uint8Array(44), WAVE_FORMAT_PCM
//   new TakeWriter({ sampleRate, bits }); w.push(l, r, frames); w.parts() -> [header, ...chunks]
//   parseWav(bytes)                      -> { sampleRate, channels, bits, frames, dataOffset, channel(c) -> Float32Array }
//   takeFileName(id, version, date)      -> "<id>-<version>-YYYYMMDD-HHMMSS.wav"
//
// Conversion: each sample clamped to [-1, 1], times 2^(bits-1) - 1, rounded half away from zero; no dither, no
// normalisation. A sample beyond full scale (|x| > 1) is a clip: counted, and written at full scale. NaN writes 0.

export const CHUNK_S = 0.25;                   // the worklet's chunk, in seconds (a whole number of quanta)
export const MAX_TAKE_S = 30 * 60;             // one take's cap: it stops cleanly there
export const WARN_TAKE_S = 25 * 60;            // a warning before the cap
export const BITS = [24, 16];

export const fullScale = (bits) => 2 ** (bits - 1) - 1;
export const chunkFrames = (sampleRate, quantum = 128) => quantum * Math.max(1, Math.round((sampleRate * CHUNK_S) / quantum));
export const bytesPerFrame = (bits, channels = 2) => (bits / 8) * channels;

// a float sample to an integer one (see the header): [value, clipped]
function toInt(x, scale) {
  if (!(x === x)) return 0;                  // (NaN)
  if (x > 1) x = 1; else if (x < -1) x = -1;
  const s = x * scale;
  return s < 0 ? -Math.round(-s) : Math.round(s);
}

export function floatToPcm(l, r, frames, bits) {
  if (bits !== 16 && bits !== 24) throw new Error(`no ${bits}-bit WAV`);
  r = r || l;
  const scale = fullScale(bits), bps = bits / 8, bytes = new Uint8Array(frames * 2 * bps);
  let clips = 0, o = 0;
  for (let i = 0; i < frames; i++) {
    const a = l[i], b = r[i];
    if (a > 1 || a < -1) clips++;
    if (b > 1 || b < -1) clips++;
    const va = toInt(a, scale), vb = toInt(b, scale);
    if (bps === 2) {
      bytes[o] = va & 255; bytes[o + 1] = (va >> 8) & 255;
      bytes[o + 2] = vb & 255; bytes[o + 3] = (vb >> 8) & 255;
      o += 4;
    } else {
      bytes[o] = va & 255; bytes[o + 1] = (va >> 8) & 255; bytes[o + 2] = (va >> 16) & 255;
      bytes[o + 3] = vb & 255; bytes[o + 4] = (vb >> 8) & 255; bytes[o + 5] = (vb >> 16) & 255;
      o += 6;
    }
  }
  return { bytes, clips };
}

// the canonical 44-byte header: RIFF <size> WAVE, "fmt " (16 bytes, format 1: integer PCM), "data" <size>
export function wavHeader({ sampleRate, bits, frames, channels = 2 }) {
  const h = new Uint8Array(44), v = new DataView(h.buffer);
  const blockAlign = (bits / 8) * channels, dataBytes = frames * blockAlign;
  if (dataBytes + 36 > 0xFFFFFFFF) throw new Error("a WAV file holds at most 4 GB");
  const tag = (o, s) => { for (let i = 0; i < 4; i++) h[o + i] = s.charCodeAt(i); };
  tag(0, "RIFF"); v.setUint32(4, 36 + dataBytes, true); tag(8, "WAVE");
  tag(12, "fmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, channels, true);
  v.setUint32(24, sampleRate, true); v.setUint32(28, sampleRate * blockAlign, true);
  v.setUint16(32, blockAlign, true); v.setUint16(34, bits, true);
  tag(36, "data"); v.setUint32(40, dataBytes, true);
  return h;
}

// one take's PCM, chunk by chunk
export class TakeWriter {
  constructor({ sampleRate, bits = 24 }) {
    if (!BITS.includes(bits)) throw new Error(`no ${bits}-bit WAV`);
    this.sampleRate = sampleRate; this.bits = bits;
    this.chunks = []; this.frames = 0; this.clips = 0; this.dataBytes = 0;
  }
  push(l, r, frames) {
    if (!frames) return 0;
    const { bytes, clips } = floatToPcm(l, r, frames, this.bits);
    this.chunks.push(bytes); this.frames += frames; this.clips += clips; this.dataBytes += bytes.length;
    return clips;
  }
  get seconds() { return this.frames / this.sampleRate; }
  get size() { return 44 + this.dataBytes; }
  // the file, as parts for a Blob (no copy of the PCM)
  parts() { return [wavHeader({ sampleRate: this.sampleRate, bits: this.bits, frames: this.frames }), ...this.chunks]; }
}

// a WAV file read back (integer PCM, 8..32 bits; the chunks walked, so a file with extra chunks reads too)
export function parseWav(bytes) {
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const str = (o) => String.fromCharCode(u8[o], u8[o + 1], u8[o + 2], u8[o + 3]);
  if (u8.length < 44 || str(0) !== "RIFF" || str(8) !== "WAVE") throw new Error("not a RIFF WAVE file");
  const riffSize = v.getUint32(4, true);
  let o = 12, fmt = null, dataOffset = -1, dataBytes = 0;
  while (o + 8 <= u8.length) {
    const id = str(o), size = v.getUint32(o + 4, true);
    if (id === "fmt ") fmt = { format: v.getUint16(o + 8, true), channels: v.getUint16(o + 10, true), sampleRate: v.getUint32(o + 12, true),
      byteRate: v.getUint32(o + 16, true), blockAlign: v.getUint16(o + 20, true), bits: v.getUint16(o + 22, true) };
    else if (id === "data") { dataOffset = o + 8; dataBytes = size; break; }
    o += 8 + size + (size & 1);
  }
  if (!fmt || dataOffset < 0) throw new Error("no fmt or data chunk");
  const { channels, bits, blockAlign } = fmt, bps = bits / 8;
  const frames = Math.floor(Math.min(dataBytes, u8.length - dataOffset) / blockAlign);
  const scale = fullScale(bits);
  const channel = (c) => {
    const out = new Float32Array(frames);
    for (let i = 0, p = dataOffset + c * bps; i < frames; i++, p += blockAlign) {
      let x;
      if (bps === 2) x = v.getInt16(p, true);
      else if (bps === 3) { x = u8[p] | (u8[p + 1] << 8) | (u8[p + 2] << 16); if (x & 0x800000) x -= 0x1000000; }
      else if (bps === 4) x = v.getInt32(p, true);
      else x = u8[p] - 128;
      out[i] = x / scale;
    }
    return out;
  };
  return { ...fmt, riffSize, dataOffset, dataBytes, frames, fileBytes: u8.length, channel };
}

const pad = (n, w = 2) => String(n).padStart(w, "0");
const safe = (s) => String(s || "").trim().replace(/[^\w.+-]+/g, "_").replace(/^_+|_+$/g, "") || "x";
export function takeFileName(id, version, d = new Date()) {
  const stamp = `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
  return `${safe(id)}-${safe(version)}-${stamp}.wav`;
}
// m:ss (h:mm:ss past an hour)
export function clock(s) {
  s = Math.max(0, Math.floor(s));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  return h ? `${h}:${pad(m)}:${pad(x)}` : `${m}:${pad(x)}`;
}
export function sizeText(b) {
  return b >= 1e9 ? `${(b / 1e9).toFixed(2)} GB` : b >= 1e6 ? `${(b / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(b / 1e3))} kB`;
}
