// SPDX-License-Identifier: GPL-3.0-only
// The .rfm1 engine's Node test (web/engine/API.md section 4), per file, through engine.js as the page uses it:
//
//   node web/engines/rfm1/test.mjs [--dir DIR] [--out DIR] [id ...]
//        DIR: the .rfm1 files (default web/build/rfm1: the RemoteFM1FW release's, downloaded there by hand; the repo
//        holds none); id: only those (the manifest's id)
//
//   1. createEngine from an empty flash; info() filled (the manifest's id, name, version, licence, source; 14 button
//      names; the keymap)
//   2. screen: not blank within 3 s of device time (render() at 44.1 kHz); PNGs in web/build/test/: rfm1-<id>.png (at
//      3 s) and rfm1-<id>-saved.png (the flash round trip's)
//   3. sound: the FM1VST conformance file's `sound` prelude (as the native test), settle_ms, then key D4 held 500 ms:
//      the output must peak above 64 / 32768 within 1 s of the press
//   4. flash: a unit booted from another's flash image boots and draws; where the conformance file has a `save` script,
//      the image after it, and `saved-lcd-differs`: the unit booted from it shows the change
//   5. reboot: the conformance file's `reboot` script: halted() turns non-zero, then render() gives silence
//   6. the render cost at 48 kHz (through the resampler), chords held, 10 s: per 128-frame quantum, mean / 99th
//      percentile / slowest, and the real-time factor
//   7. the same firmware's native core, where web/build/native/<id>.wasm is built (felucca, choralroot): the two
//      engines' audio and screen over the same script, compared (reported: a different build is not a failure)
//   8. refused: a truncated file, a module without the manifest, one importing something else, one whose manifest
//      and descriptor disagree, and one whose memory can grow
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createEngine, inspect } from "./engine.js";
import { W, H, noteName, writePng, readConformance, Panel, renderTo, runScript as runScriptIn, peakOf, blank, same } from "../common/testkit.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WEB = path.resolve(HERE, "../..");
const opt = (k) => { const i = process.argv.indexOf(k); if (i < 0) return null; const v = process.argv[i + 1]; process.argv.splice(i, 2); return v; };
const DIR = opt("--dir") || path.join(WEB, "build/rfm1");
const OUT = opt("--out") || path.join(WEB, "build/test");
const NATIVE = path.join(WEB, "build/native");
const FM1VST_DIR = process.env.FM1VST_DIR || path.join(WEB, "vendor/FM1VST");
const GLUE = fs.existsSync(path.join(NATIVE, "src/core-glue")) ? path.join(NATIVE, "src/core-glue") : path.join(FM1VST_DIR, "core-glue");
const conformance = (info) => readConformance([path.join(GLUE, info.family, `conformance_${info.id}.txt`), path.join(GLUE, info.family, "conformance.txt")]);
const runScript = (engine, lines, opts = {}) => runScriptIn(engine, lines, { ...opts, fm1vstDir: FM1VST_DIR });

// ------------------------------------------------------------------------------------------------ per file ---
async function testFile(file) {
    const bytes = new Uint8Array(fs.readFileSync(file));
    const man = inspect(bytes).manifest, id = man.id;
    const mk = (flash = null, sampleRate = 44100) => createEngine({ sampleRate, files: { wasm: bytes }, flash, options: {} });
    const res = { id, version: man.version, bytes: bytes.length, checks: [], fails: 0 };
    const check = (ok, what) => { res.checks.push(`${ok ? "ok  " : "FAIL"}  ${what}`); if (!ok) res.fails++; };
    const note = (what) => res.checks.push(`      ${what}`);

    // 1. boot and info
    const tb = performance.now();
    let e = await mk();
    res.bootMs = performance.now() - tb;
    const info = e.info();
    check(info.id === man.id && info.name === man.name && info.version === man.version && info.license === man.license
          && info.source === man.source_url && info.buttonNames.length === 14 && info.buttonNames.every(Boolean),
          `info: ${info.name} ${info.version}, ${info.license}, ${info.family}; ${info.source} (booted in ${res.bootMs.toFixed(0)} ms)`);
    check(Array.isArray(info.keymap) && info.keymap.length === 6 && info.authors.length > 0 && info.licenseTexts.length > 0,
          `info: keymap [6][11], authors ${info.authors.join(", ")}, ${info.licenseTexts.length} licence texts, credits ${info.credits ? info.credits.length + " chars" : "none"}`);
    const conf = conformance(info);

    // 2. the screen, through render()
    const L = new Float32Array(128), R = new Float32Array(128);
    let px = e.screen().pixels;
    while (blank(px) && e.deviceMs() < conf.boot_ms) { e.render(L, R); px = e.screen().pixels; }
    check(!blank(px), `screen not blank within ${conf.boot_ms} ms (at ${e.deviceMs()} ms)`);
    renderTo(e, 3000);
    writePng(path.join(OUT, `rfm1-${id}.png`), e.screen().pixels);
    e.dispose();

    // 3. the sound: prelude, settle, key held
    e = await mk();
    if (conf.sound.length) runScript(e, conf.sound);
    renderTo(e, conf.settle_ms + 300);
    let before = 0, after = 0, heard = -1;
    const panel = new Panel(e);
    renderTo(e, e.deviceMs() + 1, (l, r) => { before = Math.max(before, peakOf(l, r)); });
    const t0 = e.deviceMs();
    const vel = new Uint8Array(27);
    vel[conf.key] = 100;
    panel.keys = 1 << conf.key;
    panel.set(undefined, vel);
    while (e.deviceMs() - t0 < 1000) {
        if (panel.keys && e.deviceMs() - t0 >= conf.hold) { panel.keys = 0; panel.set(); }
        e.render(L, R);
        const pk = peakOf(L, R);
        if (pk > 64 / 32768 && heard < 0) heard = e.deviceMs() - t0;
        after = Math.max(after, pk);
    }
    check(after > 64 / 32768 && !e.halted(), `key ${noteName(conf.key)} held ${conf.hold} ms: sound (peak ${Math.round(after * 32768)} from ${heard} ms; before the key ${Math.round(before * 32768)})`);
    res.peak = Math.round(after * 32768);
    e.dispose();

    // 4. the flash round trip
    {
        e = await mk();
        const fresh = e.flash().seq;
        if (conf.save.length && conf.flashNone === null) runScript(e, conf.save); else renderTo(e, 2000);
        const bytesA = e.flash().bytes, seqA = e.flash().seq;
        e.dispose();
        check(bytesA && bytesA.length === info.flashSize, `flash: a ${bytesA ? bytesA.length : 0}-byte image (seq ${fresh} -> ${seqA})`);
        const b = await mk(bytesA);
        let p2 = b.screen().pixels;
        while (blank(p2) && b.deviceMs() < 3000) { b.render(L, R); p2 = b.screen().pixels; }
        check(!blank(p2) && !b.halted(), `flash round trip: a unit booted from the image boots and draws (at ${b.deviceMs()} ms)`);
        if (conf.flashNone !== null) note(`flash: the saved change not checked (${conf.flashNone})`);
        else if (conf.save.length && conf.savedLcdDiffers) {
            runScript(b, conf.saved);
            const lcd2 = b.screen().pixels.slice();
            const c = await mk();
            runScript(c, conf.saved);
            const lcd3 = c.screen().pixels.slice();
            writePng(path.join(OUT, `rfm1-${id}-saved.png`), lcd2);
            check(!same(lcd2, lcd3), `flash round trip: the unit booted from the saved image ${!same(lcd2, lcd3) ? "shows the saved change" : "looks like a fresh one"}`);
            c.dispose();
        } else note("flash: no save script with saved-lcd-differs (booted only)");
        b.dispose();
    }

    // 5. reboot -> halted
    if (conf.reboot.length) {
        e = await mk();
        runScript(e, conf.reboot);
        const code = e.halted();
        let pk = 0;
        renderTo(e, e.deviceMs() + 100, (l, r) => { pk = Math.max(pk, peakOf(l, r)); });
        check(code !== 0 && pk === 0, `reboot script: halted() = 0x${code.toString(16)}, then silence`);
        e.dispose();
    } else note(`reboot: ${conf.rebootNone ?? "no script"}`);

    // 6. the render cost at 48 kHz, chords held
    {
        e = await mk(null, 48000);
        const p6 = conf.sound.length ? runScript(e, conf.sound) : new Panel(e);
        const N = Math.round(48000 * 10 / 128);
        const q = new Float64Array(N);
        let pk6 = 0;
        for (let i = 0; i < N; i++) {
            const ph = Math.floor(i / 375) % 4;             // a second on, a second off
            const keys = ph === 1 ? (1 << 9) | (1 << 13) | (1 << 16) : ph === 3 ? (1 << 4) | (1 << 8) | (1 << 11) : 0;
            if (keys !== p6.keys) { p6.keys = keys; p6.set(); }
            const t = performance.now();
            e.render(L, R);
            q[i] = performance.now() - t;
            pk6 = Math.max(pk6, peakOf(L, R));
        }
        const s = Array.from(q).sort((a, b) => a - b), sum = s.reduce((a, b) => a + b, 0);
        res.cost = { avg: sum / N * 1000, p99: s[Math.floor(N * 0.99)] * 1000, max: s[N - 1] * 1000, rtf: 10 / (sum / 1000) };
        check(pk6 > 64 / 32768 && !e.halted(), `48 kHz render: per quantum ${res.cost.avg.toFixed(1)} us mean, ${res.cost.p99.toFixed(1)} us p99, ${res.cost.max.toFixed(0)} us max (a quantum is 2667 us); real-time factor ${res.cost.rtf.toFixed(0)}x, peak ${Math.round(pk6 * 32768)}`);
        e.dispose();
    }

    // 7. the native core of the same firmware, where it is built here
    const nat = path.join(NATIVE, `${id}.wasm`);
    if (["felucca", "choralroot"].includes(id) && fs.existsSync(nat)) res.vsNative = await compareNative(bytes, nat, conf, note);
    return res;
}

// the two engines over the same input at 44.1 kHz: the sound prelude, a key held, MIDI notes; their audio and screen
async function compareNative(bytes, natFile, conf, note) {
    const { createEngine: createNative } = await import("../native/engine.js");
    const a = await createEngine({ sampleRate: 44100, files: { wasm: bytes }, flash: null, options: {} });
    const b = await createNative({ sampleRate: 44100, files: { wasm: fs.readFileSync(natFile) }, flash: null, options: {} });
    const va = a.info().version, vb = b.info().version;
    const run = (e) => {
        const out = [], L = new Float32Array(128), R = new Float32Array(128);
        const keep = (l, r) => { out.push(Float32Array.from(l), Float32Array.from(r)); };
        if (conf.sound.length) runScript(e, conf.sound, { each: keep });
        renderTo(e, conf.settle_ms + 300, keep);
        const p = new Panel(e);
        p.keys = (1 << 9) | (1 << 13); p.set();
        renderTo(e, e.deviceMs() + 700, keep);
        p.keys = 0; p.set();
        e.midiIn(Uint8Array.from([0x90, 60, 100]));
        renderTo(e, e.deviceMs() + 500, keep);
        e.midiIn(Uint8Array.from([0x80, 60, 0]));
        renderTo(e, e.deviceMs() + 500, keep);
        return { out, lcd: e.screen().pixels.slice(), flash: e.flash().bytes };
    };
    const ra = run(a), rb = run(b);
    let first = -1, n = Math.min(ra.out.length, rb.out.length), diff = 0;
    for (let i = 0; i < n; i++) for (let k = 0; k < 128; k++) if (ra.out[i][k] !== rb.out[i][k]) { diff++; if (first < 0) first = (i >> 1) * 128 + k; }
    const lcdSame = same(ra.lcd, rb.lcd), flashSame = ra.flash && rb.flash && same(ra.flash, rb.flash);
    const frames = (n >> 1) * 128;
    const text = diff === 0 && lcdSame
        ? `vs the native core (${path.basename(natFile)} ${vb}): audio bit-identical over ${frames} frames, the screen identical, the flash ${flashSame ? "identical" : "differs"}`
        : `vs the native core (${path.basename(natFile)} ${vb}, this file ${va}): ${diff} of ${frames * 2} samples differ (the first at frame ${first}), the screen ${lcdSame ? "identical" : "differs"}, the flash ${flashSame ? "identical" : "differs"} (a different build of the glue or the compiler)`;
    note(text);
    a.dispose(); b.dispose();
    return { identical: diff === 0 && lcdSame, diff, first, frames, flashSame, nativeVersion: vb };
}

// ------------------------------------------------------------------------------------------- refused files ---
// the top-level sections of a module: [{ id, start, end (the whole section, header included), body, name }]
function sections(b) {
    const out = [];
    let at = 8;
    const leb = () => { let r = 0, sh = 0, x; do { x = b[at++]; r += (x & 0x7f) * 2 ** sh; sh += 7; } while (x & 0x80); return r; };
    while (at < b.length) {
        const start = at, id = b[at++], size = leb(), body = at;
        let name = null;
        if (id === 0) { const n = leb(); name = Buffer.from(b.subarray(at, at + n)).toString(); }
        at = body + size;
        out.push({ id, start, end: at, body, name });
    }
    return out;
}
const replaceOnce = (b, from, to, within = [0, b.length]) => {
    const f = Buffer.from(from), t = Buffer.from(to), i = Buffer.from(b.buffer, b.byteOffset, b.length).indexOf(f, within[0]);
    if (i < 0 || i >= within[1] || f.length !== t.length) throw new Error(`test: no ${from} to replace`);
    const c = Uint8Array.from(b);
    c.set(t, i);
    return c;
};
async function refusals(file) {
    const b = new Uint8Array(fs.readFileSync(file)), secs = sections(b), m = inspect(b).manifest;
    const man = secs.find((s) => s.name === "rfm1"), imp = secs.find((s) => s.id === 2), mem = secs.find((s) => s.id === 5);
    // the memory section rewritten: maximum = initial + 1 page (a memory that can grow)
    const growing = (() => {
        const body = b.subarray(mem.body, mem.end);
        if (body[0] !== 1 || body[1] !== 1) throw new Error("test: an unexpected memory section");
        let at = 2, init = 0, sh = 0, x;
        do { x = body[at++]; init += (x & 0x7f) * 2 ** sh; sh += 7; } while (x & 0x80);
        const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
        const nb = [1, 1, ...leb(init), ...leb(init + 1)];
        return Uint8Array.from([...b.subarray(0, mem.start), 5, ...leb(nb.length), ...nb, ...b.subarray(mem.end)]);
    })();
    const cases = [
        ["a truncated file", b.subarray(0, Math.floor(b.length * 0.6)), /runs past the end|damaged/],
        ["a module without the manifest", Uint8Array.from([...b.subarray(0, man.start), ...b.subarray(man.end)]), /no rfm1 section/],
        ["a module importing something else", replaceOnce(b, "wasi_snapshot_preview1", "wasi_snapshot_preview2", [imp.start, imp.end]), /imports wasi_snapshot_preview2\..*doesn't allow/],
        ["a manifest that disagrees with the descriptor", replaceOnce(b, `"${m.id}"`, `"${m.id.slice(0, -1)}${m.id.endsWith("x") ? "y" : "x"}"`, [man.start, man.end]), /the manifest .* and the descriptor .* disagree/],
        ["a memory that can grow", growing, /its memory can grow/],
        ["not WebAssembly", Uint8Array.from(Buffer.from("PK\u0003\u0004 not a module")), /no \\0asm magic/],
    ];
    const out = [];
    for (const [what, bytes, want] of cases) {
        let msg = null;
        try { const e = await createEngine({ sampleRate: 48000, files: { wasm: bytes }, flash: null, options: {} }); e.dispose(); }
        catch (err) { msg = err.message || String(err); }
        out.push({ ok: !!msg && want.test(msg), what, msg });
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- the run ---
if (!fs.existsSync(DIR)) {
    console.error(`rfm1 test: no ${DIR} (download the .rfm1 files of https://github.com/Quixotic7/RemoteFM1FW/releases/latest there, or --dir)`);
    process.exit(2);
}
const want = process.argv.slice(2);
const files = fs.readdirSync(DIR).filter((f) => f.endsWith(".rfm1")).sort().map((f) => path.join(DIR, f))
    .filter((f) => !want.length || want.includes(path.basename(f).replace(/-[^-]*\.rfm1$/, "")));
fs.mkdirSync(OUT, { recursive: true });
let fails = 0;
const rows = [];
for (const f of files) {
    let r;
    try { r = await testFile(f); } catch (err) { r = { id: path.basename(f), bytes: 0, checks: [`FAIL  ${err.stack || err}`], fails: 1 }; }
    console.log(`== ${r.id} ${r.version || ""}`);
    for (const c of r.checks) console.log(`  ${c}`);
    fails += r.fails;
    rows.push(r);
}
if (files.length) {
    const pick = files.find((f) => /felucca/.test(f)) || files[0];
    console.log(`== refused (made from ${path.basename(pick)})`);
    for (const x of await refusals(pick)) {
        console.log(`  ${x.ok ? "ok  " : "FAIL"}  ${x.what}: ${x.msg || "(accepted)"}`);
        if (!x.ok) fails++;
    }
}
console.log("\n| id | version | file | boot | per quantum at 48 kHz: mean / p99 / max | real-time factor | vs native | result |\n|---|---|---|---|---|---|---|---|");
for (const r of rows)
    console.log(`| ${r.id} | ${r.version || "-"} | ${(r.bytes / 1024).toFixed(0)} KiB | ${r.bootMs ? r.bootMs.toFixed(0) + " ms" : "-"} | ${r.cost ? `${r.cost.avg.toFixed(1)} / ${r.cost.p99.toFixed(1)} / ${r.cost.max.toFixed(0)} us` : "-"} | ${r.cost ? r.cost.rtf.toFixed(0) + "x" : "-"} | ${r.vsNative ? (r.vsNative.identical ? "identical" : `${r.vsNative.diff} samples differ`) : "-"} | ${r.fails ? `FAIL (${r.fails})` : "pass"} |`);
console.log(fails ? `\n${fails} check(s) failed` : `\nall passed (${files.length} files)`);
process.exit(fails ? 1 : 0);
