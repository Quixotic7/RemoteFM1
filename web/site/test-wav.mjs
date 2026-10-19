// SPDX-License-Identifier: GPL-3.0-only
// The recorder's WAV writer (js/recorder.js) in Node: the header's fields and sizes (16- and 24-bit), float to integer
// with clipping and rounding (no dither), a 1 s 1 kHz sine read back (its RMS and frequency), and a take assembled from
// many chunks, byte for byte the same as one written at once; the worklet's capture: its cost a quantum, its chunks, its cap.
//   node web/site/test-wav.mjs
import { floatToPcm, wavHeader, TakeWriter, parseWav, takeFileName, chunkFrames, fullScale, clock, MAX_TAKE_S } from "./js/recorder.js";

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(84)} ${c ? "ok" : "FAIL"}`); };
const cat = (parts) => { const n = parts.reduce((s, p) => s + p.length, 0), out = new Uint8Array(n); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } return out; };
const str = (u8, o) => String.fromCharCode(...u8.subarray(o, o + 4));

// ---- the header
for (const [bits, sr] of [[16, 44100], [24, 48000]]) {
  const frames = 12345, h = wavHeader({ sampleRate: sr, bits, frames }), v = new DataView(h.buffer);
  const ba = (bits / 8) * 2, data = frames * ba;
  ok(h.length === 44 && str(h, 0) === "RIFF" && str(h, 8) === "WAVE" && str(h, 12) === "fmt " && str(h, 36) === "data", `header ${bits}-bit: RIFF / WAVE / fmt / data tags at 0, 8, 12, 36`);
  ok(v.getUint32(4, true) === 36 + data && v.getUint32(40, true) === data, `header ${bits}-bit: RIFF size 36 + data, data size frames x ${ba}`);
  ok(v.getUint32(16, true) === 16 && v.getUint16(20, true) === 1 && v.getUint16(22, true) === 2, `header ${bits}-bit: fmt chunk 16 bytes, format 1 (PCM), 2 channels`);
  ok(v.getUint32(24, true) === sr && v.getUint32(28, true) === sr * ba && v.getUint16(32, true) === ba && v.getUint16(34, true) === bits,
     `header ${bits}-bit: rate ${sr}, byte rate ${sr * ba}, block align ${ba}, ${bits} bits`);
  const p = parseWav(cat([h, new Uint8Array(data)]));
  ok(p.format === 1 && p.channels === 2 && p.sampleRate === sr && p.bits === bits && p.frames === frames && p.dataOffset === 44 && p.riffSize === p.fileBytes - 8,
     `header ${bits}-bit: parseWav reads it back (RIFF size = file - 8)`);
}
{
  let threw = false;
  try { wavHeader({ sampleRate: 48000, bits: 24, frames: 2 ** 30 }); } catch (_) { threw = true; }
  ok(threw, "header: over 4 GB of data is refused");
  const cap = wavHeader({ sampleRate: 48000, bits: 24, frames: MAX_TAKE_S * 48000 });
  ok(new DataView(cap.buffer).getUint32(40, true) === 518400000, "header: the 30-minute cap at 48 kHz 24-bit is 518.4 MB of data");
}

// ---- float to integer: clamping, clip counting, rounding half away from zero, no dither, NaN
{
  const s16 = fullScale(16), s24 = fullScale(24);
  ok(s16 === 32767 && s24 === 8388607, "full scale: 32767 (16-bit), 8388607 (24-bit)");
  const l = Float32Array.from([0, 1, -1, 1.5, -2, 0.5 / s16, -0.5 / s16, 1.49 / s16, NaN, 1e-9]);
  const r = Float32Array.from([0.25, -0.25, 0, 0, 0, 0, 0, 0, 0, 0]);
  const { bytes, clips } = floatToPcm(l, r, l.length, 16);
  const v = new DataView(bytes.buffer), L = (i) => v.getInt16(i * 4, true), R = (i) => v.getInt16(i * 4 + 2, true);
  ok(bytes.length === l.length * 4, "16-bit: 4 bytes a frame, interleaved L R");
  ok(L(0) === 0 && L(1) === 32767 && L(2) === -32767, "16-bit: 0, +1 and -1 to 0, 32767, -32767");
  ok(L(3) === 32767 && L(4) === -32767 && clips === 2, "16-bit: beyond full scale clamps to it and counts as a clip (2)");
  // (the float32 inputs round slightly: 0.5/32767 as a float32 is just over or under a half step)
  const half = Math.fround(0.5 / s16) * s16, nhalf = Math.fround(-0.5 / s16) * s16;
  ok(L(5) === (half >= 0.5 ? 1 : 0) && L(6) === (nhalf <= -0.5 ? -1 : 0) && L(7) === 1, "16-bit: rounded to nearest, halves away from zero, symmetric");
  ok(L(8) === 0 && L(9) === 0, "16-bit: NaN writes 0; a tiny value rounds to 0 (no dither)");
  ok(R(0) === Math.round(0.25 * 32767) && R(1) === -Math.round(0.25 * 32767), "16-bit: the right channel in the odd slots");
  const exact = Float64Array.from([0.5, -0.5, 2.5 / s16, -2.5 / s16]);   // (float64 in: halves exactly)
  const e = floatToPcm(exact, exact, 4, 16), ev = new DataView(e.bytes.buffer);
  ok(ev.getInt16(8, true) === 3 && ev.getInt16(12, true) === -3 && ev.getInt16(0, true) === 16384 && ev.getInt16(4, true) === -16384,
     "16-bit: exact halves go away from zero (2.5 -> 3, -2.5 -> -3; 0.5 FS -> 16384)");
  const t = floatToPcm(Float32Array.from([1, -1, 0.5, -1.0001]), Float32Array.from([0, 0, -0.5, 0]), 4, 24);
  const b = t.bytes, i24 = (o) => { const x = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16); return x & 0x800000 ? x - 0x1000000 : x; };
  ok(b.length === 24 && i24(0) === 8388607 && i24(6) === -8388607 && i24(12) === Math.round(0.5 * s24) && i24(15) === -Math.round(0.5 * s24),
     "24-bit: 6 bytes a frame, little-endian, +1 / -1 to +-8388607, 0.5 to 4194304 (rounded)");
  ok(t.clips === 1 && i24(18) === -8388607, "24-bit: -1.0001 clamps and counts as one clip");
  const mono = floatToPcm(Float32Array.from([0.1, 0.2]), null, 2, 16), mv = new DataView(mono.bytes.buffer);
  ok(mv.getInt16(0, true) === mv.getInt16(2, true) && mv.getInt16(4, true) === mv.getInt16(6, true), "mono in (no right channel): duplicated to both");
}

// ---- a 1 s 1 kHz sine at -6 dBFS, 48 kHz, both depths: RMS and frequency read back
for (const bits of [24, 16]) {
  const sr = 48000, n = sr, amp = 0.5, f0 = 1000;
  const l = new Float32Array(n), r = new Float32Array(n);
  for (let i = 0; i < n; i++) { l[i] = amp * Math.sin((2 * Math.PI * f0 * i) / sr); r[i] = -l[i]; }
  const w = new TakeWriter({ sampleRate: sr, bits });
  const cf = chunkFrames(sr);
  for (let o = 0; o < n; o += cf) { const k = Math.min(cf, n - o); w.push(l.subarray(o, o + k), r.subarray(o, o + k), k); }
  const p = parseWav(cat(w.parts())), x = p.channel(0), y = p.channel(1);
  let ss = 0, err = 0, zc = 0;
  for (let i = 0; i < n; i++) { ss += x[i] * x[i]; err = Math.max(err, Math.abs(x[i] - l[i]), Math.abs(y[i] + x[i])); if (i && (x[i - 1] < 0) !== (x[i] < 0)) zc++; }
  const rms = Math.sqrt(ss / n), want = amp / Math.SQRT2, freq = zc / 2;   // (two zero crossings a cycle, over 1 s)
  // the frequency by the phase of a DFT bin too (1 kHz exactly: the 1 kHz bin holds the energy, 999 / 1001 none)
  const bin = (k) => { let re = 0, im = 0; for (let i = 0; i < n; i++) { const a = (2 * Math.PI * k * i) / n; re += x[i] * Math.cos(a); im -= x[i] * Math.sin(a); } return Math.hypot(re, im) / (n / 2); };
  ok(p.frames === n && p.sampleRate === sr && p.bits === bits, `sine ${bits}-bit: 48000 frames at 48 kHz read back`);
  ok(Math.abs(20 * Math.log10(rms / want)) < 0.01, `sine ${bits}-bit: RMS ${rms.toFixed(5)} = ${want.toFixed(5)} (within 0.01 dB)`);
  ok(Math.abs(freq - f0) <= 1 && bin(1000) > 0.49 && bin(999) < 0.01 && bin(1001) < 0.01, `sine ${bits}-bit: 1000 Hz (${freq} by zero crossings; the 1 kHz DFT bin ${bin(1000).toFixed(3)})`);
  ok(err <= 1 / fullScale(bits), `sine ${bits}-bit: every sample within one step (${err.toExponential(2)}), the right channel inverted`);
}

// ---- many chunks: the same bytes as one, sizes add up, clip counts add up
{
  const sr = 44100, n = 200003, l = new Float32Array(n), r = new Float32Array(n);
  let seed = 7; const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 2 ** 32) * 2.4 - 1.2;
  for (let i = 0; i < n; i++) { l[i] = rnd(); r[i] = rnd(); }
  const one = new TakeWriter({ sampleRate: sr, bits: 24 }); one.push(l, r, n);
  const many = new TakeWriter({ sampleRate: sr, bits: 24 });
  let o = 0, pieces = 0;
  while (o < n) { const k = Math.min(n - o, 1 + ((o * 7919) % 900)); many.push(l.subarray(o, o + k), r.subarray(o, o + k), k); o += k; pieces++; }
  const a = cat(one.parts()), b = cat(many.parts());
  ok(pieces > 200 && many.chunks.length === pieces, `chunks: ${pieces} chunks of 1..900 frames kept as a list`);
  ok(a.length === b.length && a.every((x, i) => x === b[i]), "chunks: the file is byte for byte the one written at once");
  ok(many.frames === n && many.size === 44 + n * 6 && b.length === many.size, "chunks: frames and size add up (44 + frames x 6)");
  ok(many.clips === one.clips && many.clips > 0, `chunks: clip counts add up (${many.clips})`);
  ok(parseWav(b).frames === n, "chunks: the header's data size covers every chunk");
  ok(new TakeWriter({ sampleRate: sr, bits: 16 }).push(l, r, 0) === 0, "chunks: an empty chunk adds nothing");
}

// ---- names and the chunk size
{
  const d = new Date(2026, 9, 19, 7, 5, 9);
  ok(takeFileName("felucca", "1.6.1", d) === "felucca-1.6.1-20261019-070509.wav", "file name: <id>-<version>-YYYYMMDD-HHMMSS.wav");
  ok(takeFileName("local-My FM/1", "Beta 4", d) === "local-My_FM_1-Beta_4-20261019-070509.wav", "file name: unsafe characters become _");
  ok(chunkFrames(48000) % 128 === 0 && Math.abs(chunkFrames(48000) / 48000 - 0.25) < 0.002 && chunkFrames(44100) % 128 === 0, "chunk: about 0.25 s, whole quanta");
  ok(clock(0) === "0:00" && clock(59.9) === "0:59" && clock(61) === "1:01" && clock(3725) === "1:02:05", "clock: m:ss (h:mm:ss past an hour)");
}

// ---- the worklet's capture (worklet/host.js), in this same V8: a quantum's process() with a take running and without,
// on an engine that only fills its buffers (so the capture's share is as large as it can be); chunks come back to it
{
  const posted = [];
  let proc = null;
  globalThis.sampleRate = 48000; globalThis.currentFrame = 0;
  globalThis.AudioWorkletProcessor = class { constructor() { this.port = { postMessage: (m) => posted.push(m) }; } };
  globalThis.registerProcessor = (_, C) => { proc = C; };
  await import("./worklet/host.js?capture");
  const h = new proc();
  h.engine = { setPanel() {}, render(L, R) { for (let i = 0; i < L.length; i++) { L[i] = Math.sin(i * 0.05) * 0.5; R[i] = L[i]; } },
               midiOut: () => null, screen: () => null, leds: () => null, flash: () => null, halted: () => 0 };
  h.measure = () => {}; h.panel.dirty = false;
  const out = [[new Float32Array(128), new Float32Array(128)]];
  const run = (n) => { const t0 = performance.now(); for (let i = 0; i < n; i++) { h.process([], out); globalThis.currentFrame += 128; if (posted.length) { for (const m of posted.splice(0)) if (m.type === "recChunk" && !m.final) h.onMessage({ type: "recBuf", l: m.l, r: m.r }); } } return ((performance.now() - t0) * 1000) / n; };
  run(20000);
  const offUs = [], onUs = [];
  for (let k = 0; k < 5; k++) {
    offUs.push(run(40000));
    h.onMessage({ type: "rec", on: true, id: 1, chunkFrames: chunkFrames(48000), maxFrames: 48000 * 1800 });
    onUs.push(run(40000));
    h.onMessage({ type: "rec", on: false });
    posted.length = 0;
  }
  const med = (a) => a.sort((x, y) => x - y)[2];
  const extra = med(onUs) - med(offUs), quantumUs = (128 / 48000) * 1e6;
  console.log(`  process() per quantum: ${med(offUs).toFixed(3)} us without a take, ${med(onUs).toFixed(3)} us with one (+${extra.toFixed(3)} us, ${(100 * extra / quantumUs).toFixed(3)} % of a 2667 us quantum)`);
  ok(extra < 5, "capture: under 5 us a quantum (under 0.2 % of the audio time)");
  // the take itself: whole chunks of 12032 frames, buffers recycled (two pairs made at the start, no more)
  posted.length = 0;
  h.onMessage({ type: "rec", on: true, id: 2, chunkFrames: 12032, maxFrames: 1e9 });
  const made = new Set(); let frames = 0;
  for (let i = 0; i < 1000; i++) { h.process([], out); for (const m of posted.splice(0)) if (m.type === "recChunk") { made.add(m.l.length + ":" + frames); frames += m.frames; h.onMessage({ type: "recBuf", l: m.l, r: m.r }); } }
  h.onMessage({ type: "rec", on: false });
  const fin = posted.find((m) => m.type === "recChunk" && m.final);
  ok(frames === 10 * 12032 && fin && fin.frames === 128000 - frames && fin.total === 128000 && fin.endFrame - fin.startFrame === 128000,
     "capture: 1000 quanta = 10 full chunks + the final part chunk; start / end frames span them");
  ok(h.recFree.length <= 3, `capture: the chunk buffers go round (${h.recFree.length} spare pairs)`);
  // the cap: it stops by itself at maxFrames, mid-quantum
  posted.length = 0;
  h.onMessage({ type: "rec", on: true, id: 3, chunkFrames: 12032, maxFrames: 1000 });
  for (let i = 0; i < 20; i++) h.process([], out);
  const capped = posted.filter((m) => m.type === "recChunk");
  ok(capped.length === 1 && capped[0].final && capped[0].reason === "cap" && capped[0].frames === 1000 && h.rec === null, "capture: stops cleanly at the cap (1000 frames, mid-quantum)");
}

console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
