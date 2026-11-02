// SPDX-License-Identifier: GPL-3.0-only
// Live audio input in the .rfm1 engine ("ain1", web/engine/API.md section 6), in Node: the manifest's "wants" and
// "needs", the optional exports, and the input path the worklet drives (audioIn / audioInput / render) through
// fixtures/ain_cart.c (built, committed: fixtures/build_ain.sh), whose output is its input and which checks the host's
// calls itself (rfm1_audio_in right before rfm1_audio with the same frames, info first, nothing while off).
//   1. inspect(): wants / needs validated; unknown wants ignored; ain1 in needs only for a Fantasy CFW
//   2. off: no rfm1_audio_in, no info call, silence however much input is pushed
//   3. on: one info call (state 1, the latency), then one rfm1_audio_in per rfm1_audio, in order
//   4. alignment: an impulse in comes out at the latency the cart is told (exact at 44.1 kHz, within a sample through
//      the resamplers at 48 and 96 kHz), the same at every point of the stream; a sine's level kept
//   5. underrun (input missing for quanta) and overrun (too much at once): counted, resynced, info re-sent with the new
//      latency, the delay steady again after
//   6. off again: info(0, 0), silence; dispose with input on: info(0, 0) first
//   node web/engines/rfm1/ain-test.mjs
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createEngine, inspect } from "./engine.js";
import { AudioIn, AIN_DEPTH, FS } from "../common/shared.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CART = fs.readFileSync(path.join(HERE, "fixtures/ain_cart.wasm"));
const PLAIN = fs.readFileSync(path.join(HERE, "fixtures/fs_cart_plain.wasm"));    // (no ain exports)
let fails = 0, n = 0;
const ok = (c, what) => { n++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return c; };

const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
const custom = (name, payload) => { const nb = Buffer.from(name), body = [...leb(nb.length), ...nb, ...payload]; return [0, ...leb(body.length), ...body]; };
const withManifest = (b, m) => Uint8Array.from([...b, ...custom("rfm1", Buffer.from(JSON.stringify(m)))]);
const MAN = { rfm1: 1, abi: 6, id: "aintest", name: "Audio In Test", version: "1.0", license: "GPL-3.0-only", source_url: "https://example.com/aintest@0" };
const WANTS = { ...MAN, wants: ["ain1"] };
const refusedBy = (bytes) => { try { inspect(bytes); return null; } catch (e) { return e.message; } };
const boot = (manifest = WANTS, rate = 44100, wasm = CART) => createEngine({ sampleRate: rate, files: { wasm: withManifest(wasm, manifest) }, flash: null, options: {} });

const Q = 128;
// nq quanta: the input's quantum pushed (unless skip(q)), then rendered; the output's left channel
function run(e, input, nq, { skip = () => false, extra = () => 0 } = {}) {
    const out = new Float32Array(nq * Q), L = new Float32Array(Q), R = new Float32Array(Q), z = new Float32Array(Q);
    for (let q = 0; q < nq; q++) {
        const at = q * Q, inL = at + Q <= input.length ? input.subarray(at, at + Q) : z;
        if (!skip(q)) e.audioInput(inL, inL);
        for (let k = extra(q); k > 0; k--) e.audioInput(z, z);
        e.render(L, R);
        out.set(L, at);
    }
    return out;
}
// the peak's position (parabolic interpolation) at or after `from`
function peakAt(x, from = 0, to = x.length) {
    let bi = from;
    for (let i = from; i < to; i++) if (Math.abs(x[i]) > Math.abs(x[bi])) bi = i;
    const a = x[bi - 1] || 0, b = x[bi], c = x[bi + 1] || 0, d = a - 2 * b + c;
    return { at: d ? bi + 0.5 * (a - c) / d : bi, v: b };
}
const impulses = (len, at, amp = 0.5) => { const x = new Float32Array(len); for (const t of at) x[t] = amp; return x; };

// ---- 1. the manifest
console.log("inspect");
{
    const f = inspect(withManifest(CART, WANTS));
    ok(f.ain === "wants" && f.ainExports.audioIn && f.ainExports.info && f.wants.join() === "ain1", "wants ain1 with both exports: ain \"wants\"");
    const g = inspect(withManifest(CART, { ...MAN, wants: ["ain1", "zz9", "midi2"] }));
    ok(g.ain === "wants" && g.wants.length === 3, "unknown names in wants: ignored (accepted)");
    ok(inspect(withManifest(CART, MAN)).ain === null, "no wants: ain null (the exports there but never called)");
    ok(inspect(withManifest(CART, { ...MAN, wants: [] })).ain === null, "wants []: ain null");
    let m = refusedBy(withManifest(CART, { ...MAN, wants: "ain1" }));
    ok(/"wants" is not a list of names/.test(m || ""), `wants not a list: refused (${m})`);
    m = refusedBy(withManifest(CART, { ...MAN, wants: ["ain1", 3] }));
    ok(/"wants" is not a list of names/.test(m || ""), "wants with a number in it: refused");
    const h = inspect(withManifest(CART, { ...MAN, virtual: true, needs: ["ain1"] }));
    ok(h.ain === "needs" && h.virtual, "needs ain1 (a Fantasy CFW): accepted, ain \"needs\"");
    ok(inspect(withManifest(CART, { ...MAN, virtual: true, needs: ["ain1"], wants: ["ain1"] })).ain === "needs", "in both: needs wins");
    m = refusedBy(withManifest(CART, { ...MAN, virtual: true, needs: ["ain2"] }));
    ok(/needs a newer RemoteFM1 \(it needs "ain2"\)/.test(m || ""), "needs an unknown name: refused as before");
    m = refusedBy(withManifest(CART, { ...MAN, needs: ["ain1"] }));
    ok(/lists "needs" but is not "virtual"/.test(m || ""), "needs ain1 without virtual: refused (needs is a Fantasy CFW's)");
    m = refusedBy(withManifest(CART, { ...MAN, virtual: true, needs: "ain1" }));
    ok(/"needs" is not a list of names/.test(m || ""), "needs not a list: refused");
    const p = inspect(withManifest(PLAIN, { ...MAN, id: "fscart", name: "FS Cart", wants: ["ain1"] }));
    ok(p.ain === "wants" && !p.ainExports.audioIn && !p.ainExports.info, "wants ain1 without the exports: accepted, nothing to call");
}

// ---- 2. off
console.log("off");
{
    const e = await boot(), x = e.rt.ex;
    const tone = new Float32Array(Q * 200).map((_, i) => 0.5 * Math.sin(i * 0.05));
    const out = run(e, tone, 200);
    ok(x.t_audio_calls() > 100 && x.t_in_calls() === 0 && x.t_info_calls() === 0, `input off: ${x.t_audio_calls()} rfm1_audio, no rfm1_audio_in, no info call`);
    ok(out.every((v) => v === 0), "input off: silence, though input was pushed");
    ok(e.info().ain === "wants" && e.audioInStats().on === false, "info().ain \"wants\"; audioInStats off");
    const p = await createEngine({ sampleRate: 48000, files: { wasm: withManifest(CART, MAN) } });
    p.audioIn(true, 0);
    run(p, tone, 50);
    ok(p.rt.ex.t_in_calls() === 0 && p.rt.ex.t_info_calls() === 0 && !p.audioInStats().on, "a cart without ain1 in its manifest: audioIn(true) ignored, nothing called");
    const q = await createEngine({ sampleRate: 48000, files: { wasm: withManifest(PLAIN, { ...MAN, id: "fscart", name: "FS Cart", wants: ["ain1"] }) } });
    q.audioIn(true, 0.01);
    const qo = run(q, tone, 50);
    ok(qo.every((v) => v === 0) && q.audioInStats().on, "wants ain1 without the exports: on, nothing called, renders as before");
}

// ---- 3-4. on: the calls and the alignment
for (const rate of [44100, 48000, 96000]) {
    console.log(`on, ${rate} Hz`);
    const e = await boot(WANTS, rate), x = e.rt.ex;
    run(e, new Float32Array(0), 20);
    e.audioIn(true, 0);
    const len = Q * 400, at = [3000, 3000 + 4111, 3000 + 9007, 3000 + 20011];
    const out = run(e, impulses(len, at), 400);
    const lat = e.audioInStats().latency, d = e.audioInStats().delay;
    ok(x.t_info_calls() === 1 && x.t_info_state() === 1 && x.t_info_latency() === lat, `one info call: state 1, latency ${lat} frames (44.1 kHz)`);
    ok(x.t_errors() === 0, "the cart saw rfm1_audio_in right before each rfm1_audio, the same frames, info first");
    ok(x.t_in_calls() === x.t_with_input() && x.t_in_calls() > 0 && x.t_audio_calls() - x.t_in_calls() > 0,
        `${x.t_in_calls()} rfm1_audio_in, each followed by its rfm1_audio (${x.t_audio_calls() - x.t_in_calls()} blocks before input was on)`);
    const want = d * rate / FS;                     // the delay in context frames
    const got = at.map((t) => peakAt(out, t, t + 2000));
    const err = Math.max(...got.map((g, i) => Math.abs(g.at - at[i] - want)));
    ok(rate === FS ? got.every((g, i) => g.at === at[i] + AIN_DEPTH && g.v === 0.5) : err < 0.5,
        `impulses out at +${want.toFixed(2)} frames (the latency told: ${d.toFixed(2)} at 44.1 kHz): measured ${got.map((g, i) => (g.at - at[i]).toFixed(2)).join(", ")}`);
    ok(got.every((g) => Math.abs(g.v) > 0.4 * Math.min(1, FS / rate)), `the impulses' peaks kept (${got.map((g) => g.v.toFixed(2)).join(", ")})`);
    // a sine through it: the level kept (the resamplers pass 1 kHz flat)
    const sine = new Float32Array(Q * 300).map((_, i) => 0.4 * Math.sin(2 * Math.PI * 1000 * i / rate));
    const so = run(e, sine, 300);
    let pk = 0; for (let i = Q * 100; i < so.length; i++) pk = Math.max(pk, Math.abs(so[i]));
    ok(Math.abs(pk - 0.4) < 0.01, `a 1 kHz sine at 0.4: out at ${pk.toFixed(3)}`);
    ok(e.audioInStats().under === 0 && e.audioInStats().over === 0, `no underrun or overrun in steady state (depth ${e.audioInStats().depth})`);
}

// ---- 5. input missing / too much: the pairing kept (the input is locked to the output's clock)
console.log("missing and extra input");
{
    const e = await boot(WANTS, 48000), x = e.rt.ex;
    run(e, new Float32Array(0), 37);
    e.audioIn(true, 0.005);
    run(e, new Float32Array(0), 50);
    const lat0 = e.audioInStats().latency, d0 = e.audioInStats().delay, calls0 = x.t_info_calls();
    ok(lat0 === Math.round(d0 + 0.005 * FS), `the latency told: the FIFO, the output's lead and the resamplers (${d0.toFixed(1)}) + the capture's 5 ms = ${lat0}`);
    const at = [2000, 9000, 15000], len = Q * 160, want = d0 * 48000 / FS;
    // no input for 6 quanta (between the impulses), then two quanta's worth before one render, 8 times
    let out = run(e, impulses(len, at), 160, { skip: (q) => q >= 20 && q < 26, extra: (q) => (q >= 80 && q < 88 ? 1 : 0) });
    const s1 = e.audioInStats();
    ok(s1.missing === 6 * Q && s1.extra === 8 * Q, `counted: ${s1.missing / Q} quanta missing (silence), ${s1.extra / Q} extra (dropped)`);
    const got = at.map((t) => peakAt(out, t, t + 3000).at - t);
    ok(got.every((g) => Math.abs(g - want) < 0.5) && s1.delay === d0, `the delay unchanged: impulses at +${got.map((g) => g.toFixed(2)).join(", ")} (want ${want.toFixed(2)})`);
    ok(s1.under === 0 && s1.over === 0 && x.t_info_calls() === calls0 && x.t_errors() === 0, "the FIFO never under- or overran; no info call (the latency did not move)");
    // the capture's latency moves: info again when by AIN_RESYNC frames or more
    e.audioIn(true, 0.0055);
    run(e, new Float32Array(0), 2);
    ok(x.t_info_calls() === calls0, "the capture latency 0.5 ms (22 frames) later: not re-sent");
    e.audioIn(true, 0.02);
    run(e, new Float32Array(0), 2);
    ok(x.t_info_calls() === calls0 + 1 && x.t_info_latency() === e.audioInStats().latency && x.t_info_state() === 1, `20 ms: re-sent (${x.t_info_latency()})`);

    // ---- 6. off again, dispose
    const calls = x.t_info_calls(), ins = x.t_in_calls();
    e.audioIn(false);
    out = run(e, new Float32Array(Q * 50).fill(0.3), 50);
    ok(x.t_info_calls() === calls + 1 && x.t_info_state() === 0 && x.t_info_latency() === 0, "off: info(0, 0)");
    ok(x.t_in_calls() === ins && out.every((v) => v === 0), "off: no more rfm1_audio_in, silence");
    e.audioIn(true, 0);
    run(e, new Float32Array(0), 5);
    ok(x.t_info_state() === 1 && x.t_info_calls() === calls + 2 && e.audioInStats().depth > 0, "on again: info(1, ...) and a fresh FIFO");
    e.dispose();
    ok(x.t_info_state() === 0 && x.t_info_calls() === calls + 3 && x.t_errors() === 0, "dispose with input on: info(0, 0) first");
}

// ---- the FIFO alone: the delay formula, any quantum size
console.log("AudioIn");
{
    for (const rate of [22050, 44100, 48000, 88200]) {
        const io = new AudioIn(rate), o = new Int16Array(2 * Q), seq = [];
        const N = Math.ceil(rate * 0.4);
        const x = new Float32Array(N); x[5000] = 0.5;
        let w = 0;
        // push one 128-frame quantum, read 44.1 kHz blocks while the output side would (what it has rendered up to)
        let needOut = 0;
        while (w + Q <= N) {
            io.push(x.subarray(w, w + Q), x.subarray(w, w + Q)); w += Q;
            needOut = Math.floor((w + 0.5) * FS / rate) + 16 + Q;
            while (io.read + Q <= needOut) { io.readInt16(o, Q); for (let i = 0; i < Q; i++) seq.push(o[2 * i] / 32768); }
        }
        const p = peakAt(Float32Array.from(seq), 0);
        const want = 5000 * FS / rate + io.fifoDelay + (rate === FS ? 0 : FS / rate);
        ok(Math.abs(p.at - want) < 0.5 && io.under === 0, `${rate} Hz: impulse at 44.1 kHz frame ${p.at.toFixed(2)} (want ${want.toFixed(2)}), no underrun`);
    }
}

// ---- the FIFO's own safety net: an underrun (reads with no pushes) and an overrun (pushes with no reads) resync it
{
    const io = new AudioIn(48000), o = new Int16Array(2 * Q), z = new Float32Array(Q);
    for (let i = 0; i < 3; i++) io.readInt16(o, Q);       // (the third finds nothing)
    ok(io.under === 1 && io.avail === AIN_DEPTH && io.fifoDelay === AIN_DEPTH + Q + AIN_DEPTH,
        `underrun: counted once, padded with silence and AIN_DEPTH again (FIFO delay ${io.fifoDelay})`);
    for (let i = 0; i < 12; i++) io.push(z, z);
    ok(io.over >= 1 && io.avail <= AIN_DEPTH + 3 * Q, `overrun: counted (${io.over}), the oldest dropped (depth ${io.avail}, delay ${io.fifoDelay})`);
}

console.log(fails ? `ain-test: ${fails} of ${n} FAILED` : `ain-test: all ${n} passed`);
process.exit(fails ? 1 : 0);
