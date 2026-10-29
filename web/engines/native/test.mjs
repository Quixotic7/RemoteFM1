// SPDX-License-Identifier: GPL-3.0-only
// The native engine's Node test (web/engine/API.md section 4), per core, through engine.js as the page uses it:
//
//   node web/engines/native/test.mjs [id ...]      (after build.sh; default: every core in web/build/native)
//   node web/engines/native/test.mjs --wasm FILE [--id ID] [--conformance FILE] [--reference PPM] [--out DIR]
//                                    [--json FILE]
//        one module from anywhere (an author's build: build.sh --ext, .github/scripts/fm1-native.sh, the site's sync):
//        ID the descriptor's id (default: the file's name), the conformance file and the reference picture (the host
//        build's first screen, build.sh --host) given; --json writes the results
//
//   1. createEngine from an empty flash; info() filled (id, name, version, licence, source, 14 button names)
//   2. screen: not blank (two or more colours) within 3 s of device time, run ms by ms as FM1VST's
//      tests/core_conformance.cpp does; that first picture is compared with FM1VST's own (FM1VST_DIR/build*/
//      conformance/<id>.ppm, when one exists); PNGs in web/build/test/: native-<id>-boot.png (that first picture),
//      native-<id>.png (at 3 s, rendered through render()) and native-<id>-saved.png (the flash round trip's)
//   3. sound: the core's conformance file (FM1VST core-glue/<family>/conformance_<id>.txt): its `sound` prelude
//      (LOOM-8's writes four kicks and presses PLAY: its keys are a pattern's steps), settle_ms, then key D4 held
//      500 ms through setPanel; render() output at 44.1 kHz
//      must peak above 64 / 32768 within 1 s of the press
//   4. flash: the conformance file's `save` script on a fresh unit, flash().bytes, a second unit booted from them
//      runs `saved`; its screen must differ from a fresh unit's after the same script (saved-lcd-differs);
//      `flash none` skips it
//   5. reboot: the conformance file's `reboot` script (OCT- + OCT+ held: UBOOT): halted() turns non-zero, then
//      render() gives silence; `reboot none` / no script skips it
//   6. MIDI: a note-on / note-off through midiIn() sounds, and MASTER at 0 quietens it (reported, not required)
//   7. the real-time factor: 10 s at 48 kHz through render() and the resampler, chords held, timed (after the `sound`
//      prelude, untimed: LOOM-8's pattern plays under the chords, which are steps and kit sounds there)
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createEngine, _midi } from "./engine.js";
import { W, H, noteName, writePng, rgbOf, readPpm, readConformance, Panel, renderTo, runScript as runScriptIn, peakOf, blank, same } from "../common/testkit.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WEB = path.resolve(HERE, "../..");
const BUILD = path.join(WEB, "build/native");
const opt = (k) => { const i = process.argv.indexOf(k); if (i < 0) return null; const v = process.argv[i + 1]; process.argv.splice(i, 2); return v; };
const ONE = { wasm: opt("--wasm"), id: opt("--id"), conformance: opt("--conformance"), reference: opt("--reference"),
              json: opt("--json") };
const OUT = opt("--out") || path.join(WEB, "build/test");
const FM1VST_DIR = process.env.FM1VST_DIR ||
    "/Volumes/Q7Media-2025/Projects/Github/ChoralRootFM1/FM1VST/.claude/worktrees/cores-all";
const GLUE = fs.existsSync(path.join(BUILD, "src/core-glue")) ? path.join(BUILD, "src/core-glue")
                                                                 : path.join(FM1VST_DIR, "core-glue");
// --------------------------------------------------------------------------------- the conformance file ---
function conformance(info) {
    return readConformance(ONE.wasm ? [ONE.conformance].filter(Boolean)
        : [path.join(GLUE, info.family, `conformance_${info.id}.txt`), path.join(GLUE, info.family, "conformance.txt")]);
}
const runScript = (engine, lines, opts = {}) => runScriptIn(engine, lines, { ...opts, fm1vstDir: FM1VST_DIR });

// ----------------------------------------------------------------------------------------------- per core ---
async function testCore(id) {
    const wasm = fs.readFileSync(ONE.wasm || path.join(BUILD, `${id}.wasm`));
    const module = await WebAssembly.compile(wasm);
    const mk = (flash = null, sampleRate = 44100) => createEngine({ sampleRate, files: { wasm: module }, flash, options: {} });
    const res = { id, bytes: wasm.length, checks: [], fails: 0 };
    const check = (ok, what) => { res.checks.push(`${ok ? "ok  " : "FAIL"}  ${what}`); if (!ok) res.fails++; };
    const note = (what) => res.checks.push(`      ${what}`);

    // 1. boot and info
    let e = await mk();
    const info = e.info();
    check(info.id === id && info.name && info.version && info.license && info.source && info.buttonNames.every(Boolean),
          `info: ${info.name} ${info.version}, ${info.license}, ${info.family}; ${info.source}`);
    check(Array.isArray(info.keymap) && info.keymap.length === 6, "info: keymap [6][11]");
    const conf = conformance(info);

    // 2. the screen, ms by ms as FM1VST's conformance test
    let px = e.screen().pixels;
    while (blank(px) && e.deviceMs() < conf.boot_ms) { e.runMs(1); px = e.screen().pixels; }
    const drawn = e.deviceMs();
    check(!blank(px), `screen not blank within ${conf.boot_ms} ms (at ${drawn} ms)`);
    writePng(path.join(OUT, `native-${id}-boot.png`), px);
    const ref = ONE.wasm ? (ONE.reference && fs.existsSync(ONE.reference) ? ONE.reference : null)
        : ["build-public", "build", "build-release"].map((b) => path.join(FM1VST_DIR, b, "conformance", `${id}.ppm`))
            .find((f) => fs.existsSync(f));
    if (ref) {
        const a = rgbOf(px), b = readPpm(ref);
        let diff = 0;
        for (let i = 0; i < W * H; i++) if (a[3 * i] !== b[3 * i] || a[3 * i + 1] !== b[3 * i + 1] || a[3 * i + 2] !== b[3 * i + 2]) diff++;
        res.ref = diff === 0 ? "identical" : `${diff} px differ`;
        check(diff === 0, ONE.wasm ? `the first picture is the host build's (${path.basename(ref)}): ${res.ref}`
                                   : `the first picture is FM1VST's (${path.relative(FM1VST_DIR, ref)}): ${res.ref}`);
    } else if (ONE.wasm) note("the first picture: no reference picture (no host build)");
    res.info = { id: info.id, name: info.name, version: info.version, license: info.license, source: info.source,
                 family: info.family, credits: info.credits };
    res.drawnMs = drawn;
    e.dispose();
    e = await mk();
    renderTo(e, 3000);
    writePng(path.join(OUT, `native-${id}.png`), e.screen().pixels);
    e.dispose();

    // 3. the sound: prelude, settle, key D4 held
    e = await mk();
    if (conf.sound.length) runScript(e, conf.sound);
    renderTo(e, conf.settle_ms + 300);
    let before = 0, after = 0, heard = -1;
    const panel = new Panel(e);
    renderTo(e, e.deviceMs() + 1, (L, R) => { before = Math.max(before, peakOf(L, R)); });
    const t0 = e.deviceMs();
    const vel = new Uint8Array(27);
    vel[conf.key] = 100;
    panel.keys = 1 << conf.key;
    panel.set(undefined, vel);
    const L = new Float32Array(128), R = new Float32Array(128);
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
    if (conf.flashNone !== null) {
        note(`flash: skipped (${conf.flashNone})`);
        res.flash = "n/a";
    } else if (!conf.save.length) {
        note("flash: no save script");
        res.flash = "n/a";
    } else {
        e = await mk();
        const fresh = e.flash();
        runScript(e, conf.save);
        const before = e.flash().seq;
        const bytes = e.flash().bytes;           // (flash_sync first: the firmware's deferred saves written)
        const f = e.flash();
        e.dispose();
        check(bytes && bytes.length === 1 << 20 && f.seq !== fresh.seq,
              `flash: 1 MiB image, seq ${fresh.seq} -> ${before} after the script -> ${f.seq} after the sync`);
        const s2 = await mk(bytes);
        runScript(s2, conf.saved);
        const lcd2 = s2.screen().pixels.slice();
        const s3 = await mk();
        runScript(s3, conf.saved);
        const lcd3 = s3.screen().pixels.slice();
        writePng(path.join(OUT, `native-${id}-saved.png`), lcd2);
        const differs = !same(lcd2, lcd3);
        check(conf.savedLcdDiffers ? differs : true, `flash round trip: the unit booted from the image ${differs ? "shows the saved change" : "looks like a fresh one"}`);
        res.flash = differs ? "ok" : "FAIL";
        s2.dispose();
        s3.dispose();
    }

    // 5. reboot -> halted
    if (conf.reboot.length) {
        e = await mk();
        runScript(e, conf.reboot);
        const code = e.halted();
        let pk = 0;
        renderTo(e, e.deviceMs() + 100, (L2, R2) => { pk = Math.max(pk, peakOf(L2, R2)); });
        check(code !== 0 && pk === 0, `reboot script: halted() = 0x${code.toString(16)}, then silence`);
        e.dispose();
    } else note(`reboot: ${conf.rebootNone ?? "no script"}`);

    // 6. MIDI in (reported)
    e = await mk();
    renderTo(e, conf.settle_ms + 300);
    e.midiIn(Uint8Array.from([0x90, 62, 100]));
    let mpk = 0;
    const tm = e.deviceMs();
    renderTo(e, tm + 400, (L2, R2) => { mpk = Math.max(mpk, peakOf(L2, R2)); });
    e.midiIn(Uint8Array.from([0x80, 62, 0]));
    renderTo(e, tm + 600);
    const mo = e.midiOut();
    note(`MIDI in note D4: peak ${Math.round(mpk * 32768)}; MIDI out ${mo.length} bytes since boot`);
    // MASTER turned down to 0 (the first value is the pot's resting place, a change is applied), then the MIDI note
    e.setPanel({ keys: 0, buttons: 0, master: 600 });
    e.setPanel({ keys: 0, buttons: 0, master: 0 });
    renderTo(e, e.deviceMs() + 500);
    e.midiIn(Uint8Array.from([0x90, 62, 100]));
    let qpk = 0;
    renderTo(e, e.deviceMs() + 400, (L2, R2) => { qpk = Math.max(qpk, peakOf(L2, R2)); });
    e.midiIn(Uint8Array.from([0x80, 62, 0]));
    note(`MASTER at 0: the same note peaks ${Math.round(qpk * 32768)}`);
    e.dispose();

    // 7. the real-time factor at 48 kHz (through the resampler), chords held
    e = await mk(null, 48000);
    const p7 = conf.sound.length ? runScript(e, conf.sound) : new Panel(e);
    const N = 48000 * 10 / 128;
    const L7 = new Float32Array(128), R7 = new Float32Array(128);
    let pk7 = 0;
    const t = performance.now();
    for (let q = 0; q < N; q++) {
        const ph = Math.floor(q / 375) % 4;             // a second on, a second off
        const keys = ph === 1 ? (1 << 9) | (1 << 13) | (1 << 16) : ph === 3 ? (1 << 4) | (1 << 8) | (1 << 11) : 0;
        if (keys !== p7.keys) { p7.keys = keys; p7.set(); }
        e.render(L7, R7);
        pk7 = Math.max(pk7, peakOf(L7, R7));
    }
    const wall = (performance.now() - t) / 1000;
    res.rtf = 10 / wall;
    check(pk7 > 64 / 32768, `48 kHz render (resampler): 10 s in ${wall.toFixed(2)} s, real-time factor ${res.rtf.toFixed(1)}x, peak ${Math.round(pk7 * 32768)}`);
    e.dispose();
    return res;
}

// the MIDI conversions: raw bytes -> USB-MIDI packets -> bytes again (running status expanded)
{
    const msgs = [[0x90, 60, 100], [0x80, 60, 0], [0xb0, 7, 90], [0xc0, 5], [0xe0, 0, 64], [0xf8],
                  [0xf0, 0x7d, 1, 2, 3, 0xf7], [0xf0, 0x7d, 0xf7], [0xf0, 0x7d, 1, 0xf7], [0xf0, 0xf7], [0xf2, 1, 2], [0xf3, 4]];
    const flat = msgs.flat();
    const p = new _midi.MidiParser(), pk = [], back = [];
    for (const b of [...flat, 0x90, 61, 100, 62, 100]) p.push(b, pk);       // (the last: running status)
    for (const x of pk) _midi.packetBytes(x, back);
    const want = [...flat, 0x90, 61, 100, 0x90, 62, 100];
    const ok = back.length === want.length && back.every((v, i) => v === want[i]);
    console.log(`${ok ? "ok  " : "FAIL"}  MIDI bytes -> ${pk.length} USB-MIDI packets -> bytes${ok ? "" : ": " + back.join(" ")}`);
    if (!ok) process.exitCode = 1;
}
// a manager's frames (web/managers): long SysEx of every length mod 3 (a BACKUP_PUT's ~300 bytes), every data value,
// intact through the packets (CIN 4 runs, then 5 / 6 / 7), with a clock byte inside one (USB-MIDI: its own packet)
{
    let all = true, packets = 0;
    for (const n of [299, 300, 301, 4000]) {
        const body = Array.from({ length: n }, (_, i) => (i * 37 + n) & 0x7f);
        const frame = [0xf0, 0x7d, 0x46, 0x4c, ...body, 0xf7];
        const p = new _midi.MidiParser(), pk = [], back = [];
        const input = [...frame.slice(0, 50), 0xf8, ...frame.slice(50)];
        for (const b of input) p.push(b, pk);
        for (const x of pk) _midi.packetBytes(x, back);
        packets += pk.length;
        const got = back.filter((v) => v !== 0xf8);  // (the clock comes out between two packets of the SysEx)
        if (back.length !== got.length + 1 || got.length !== frame.length || got.some((v, i) => v !== frame[i])) all = false;
        if (!pk.every((x) => [4, 5, 6, 7, 15].includes(x & 15))) all = false;
    }
    console.log(`${all ? "ok  " : "FAIL"}  SysEx of 305..4006 bytes -> ${packets} USB-MIDI packets -> the same bytes`);
    if (!all) process.exitCode = 1;
}

const ids = ONE.wasm ? [ONE.id || path.basename(ONE.wasm, ".wasm")]
    : process.argv.slice(2).length ? process.argv.slice(2)
    : JSON.parse(fs.readFileSync(path.join(BUILD, "cores.json"), "utf8")).map((c) => c.id);
fs.mkdirSync(OUT, { recursive: true });
const rows = [];
let fails = 0;
for (const id of ids) {
    let r;
    try {
        r = await testCore(id);
    } catch (err) {
        r = { id, bytes: 0, checks: [`FAIL  ${err.stack || err}`], fails: 1 };
    }
    console.log(`== ${id}`);
    for (const c of r.checks) console.log(`  ${c}`);
    fails += r.fails;
    rows.push(r);
}
console.log("\n| id | wasm size | real-time factor (Node, 48 kHz) | test result |\n|---|---|---|---|");
for (const r of rows)
    console.log(`| ${r.id} | ${(r.bytes / 1024).toFixed(0)} KiB | ${r.rtf ? r.rtf.toFixed(0) + "x" : "-"} | ${r.fails ? `FAIL (${r.fails})` : "pass"}${r.flash === "n/a" ? " (flash n/a)" : ""} |`);
console.log(fails ? `\n${fails} check(s) failed` : "\nall passed");
if (ONE.json) fs.writeFileSync(ONE.json, JSON.stringify({ fails: fails + (process.exitCode ? 1 : 0), cores: rows }, null, 1) + "\n");
process.exit(fails || process.exitCode ? 1 : 0);
