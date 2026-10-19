// SPDX-License-Identifier: GPL-3.0-only
// The native engine's Node test (web/engine/API.md section 4), per core, through engine.js as the page uses it:
//
//   node web/engines/native/test.mjs [id ...]      (after build.sh; default: every core in web/build/native)
//
//   1. createEngine from an empty flash; info() filled (id, name, version, licence, source, 14 button names)
//   2. screen: not blank (two or more colours) within 3 s of device time, run ms by ms as FM1VST's
//      tests/core_conformance.cpp does; that first picture is compared with FM1VST's own (FM1VST_DIR/build*/
//      conformance/<id>.ppm, when one exists); PNGs in web/build/test/: native-<id>-boot.png (that first picture),
//      native-<id>.png (at 3 s, rendered through render()) and native-<id>-saved.png (the flash round trip's)
//   3. sound: the core's conformance file (FM1VST core-glue/<family>/conformance_<id>.txt): its `sound` prelude
//      (none of them has one), settle_ms, then key D4 held 500 ms through setPanel; render() output at 44.1 kHz
//      must peak above 64 / 32768 within 1 s of the press
//   4. flash: the conformance file's `save` script on a fresh unit, flash().bytes, a second unit booted from them
//      runs `saved`; its screen must differ from a fresh unit's after the same script (saved-lcd-differs);
//      `flash none` skips it
//   5. reboot: the conformance file's `reboot` script (OCT- + OCT+ held: UBOOT): halted() turns non-zero, then
//      render() gives silence; `reboot none` / no script skips it
//   6. MIDI: a note-on / note-off through midiIn() sounds, and MASTER at 0 quietens it (reported, not required)
//   7. the real-time factor: 10 s at 48 kHz through render() and the resampler, chords held, timed
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";
import { createEngine, _midi } from "./engine.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WEB = path.resolve(HERE, "../..");
const BUILD = path.join(WEB, "build/native");
const OUT = path.join(WEB, "build/test");
const FM1VST_DIR = process.env.FM1VST_DIR ||
    "/Volumes/Q7Media-2025/Projects/Github/ChoralRootFM1/FM1VST/.claude/worktrees/cores-all";
const GLUE = fs.existsSync(path.join(BUILD, "src/core-glue")) ? path.join(BUILD, "src/core-glue")
                                                                 : path.join(FM1VST_DIR, "core-glue");
const W = 240, H = 240;
const BTN_NAME = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
const ENC_NAME = ["SELECT", "ALGORITHM", "PRESETS", "KNOB1", "KNOB2", "KNOB3", "KNOB4"];
const NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const noteName = (k) => NOTES[(53 + k) % 12] + (Math.floor((53 + k) / 12) - 1);

// ------------------------------------------------------------------------------------------------------ PNG ---
const CRC = new Int32Array(256).map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c;
});
function crc32(b) {
    let c = -1;
    for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 0xff] ^ (c >>> 8);
    return (c ^ -1) >>> 0;
}
function chunk(type, data) {
    const t = Buffer.from(type, "ascii"), len = Buffer.alloc(4), crc = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    crc.writeUInt32BE(crc32(Buffer.concat([t, data])));
    return Buffer.concat([len, t, data, crc]);
}
function rgbOf(px) {               // RGB565 (native value) -> 8-bit RGB, as FM1VST's ppm.h fm1g_rgb_of
    const rgb = Buffer.alloc(W * H * 3);
    for (let i = 0; i < W * H; i++) {
        const c = px[i];
        rgb[3 * i] = Math.floor(((c >> 11) & 31) * 255 / 31);
        rgb[3 * i + 1] = Math.floor(((c >> 5) & 63) * 255 / 63);
        rgb[3 * i + 2] = Math.floor((c & 31) * 255 / 31);
    }
    return rgb;
}
function writePng(file, px) {
    const rgb = rgbOf(px);
    const raw = Buffer.alloc((W * 3 + 1) * H);
    for (let y = 0; y < H; y++) rgb.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(W, 0);
    ihdr.writeUInt32BE(H, 4);
    ihdr[8] = 8; ihdr[9] = 2;
    fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
                                          chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]));
}
function readPpm(file) {           // a P6 240 x 240 file's pixels
    const b = fs.readFileSync(file);
    let i = 0, fields = [];
    while (fields.length < 4) {
        while (/\s/.test(String.fromCharCode(b[i]))) i++;
        if (b[i] === 35) { while (b[i] !== 10) i++; continue; }
        let s = "";
        while (!/\s/.test(String.fromCharCode(b[i]))) s += String.fromCharCode(b[i++]);
        fields.push(s);
    }
    return b.subarray(i + 1, i + 1 + W * H * 3);
}

// --------------------------------------------------------------------------------- the conformance file ---
function conformance(info) {
    const files = [path.join(GLUE, info.family, `conformance_${info.id}.txt`), path.join(GLUE, info.family, "conformance.txt")];
    const f = files.find((x) => fs.existsSync(x));
    const c = { file: f, boot_ms: 3000, settle_ms: 1000, key: 9, hold: 500, sound: [], save: [], saved: [], reboot: [],
                flashNone: null, rebootNone: null, savedLcdDiffers: false };
    if (!f) return c;
    for (let line of fs.readFileSync(f, "utf8").split("\n")) {
        line = line.replace(/#.*/, "").trim();
        if (!line) continue;
        const [w, ...rest] = line.split(/\s+/);
        const r = rest.join(" ");
        if (w === "boot_ms") c.boot_ms = +r;
        else if (w === "settle_ms") c.settle_ms = +r;
        else if (w === "hold") c.hold = +r;
        else if (w === "key") c.key = [...Array(27).keys()].find((k) => noteName(k).toUpperCase() === r.toUpperCase()) ?? 9;
        else if (w === "flash" && rest[0] === "none") c.flashNone = rest.slice(1).join(" ");
        else if (w === "reboot" && rest[0] === "none") c.rebootNone = rest.slice(1).join(" ");
        else if (w === "saved-lcd-differs") c.savedLcdDiffers = true;
        else if (["sound", "save", "saved", "reboot"].includes(w)) c[w].push(r);
    }
    return c;
}

// ------------------------------------------------------------------- a script (tests/script_runner's subset) ---
// wait N, btn|key NAME down|up|MS, knob NAME +-N, quit. Applied between render quanta at the device ms they fall
// on (the engine's ms counter), through setPanel as the page would.
function parseScript(lines) {
    const ev = [];
    let cur = 0;
    for (const line of lines) {
        const w = line.split(/\s+/);
        let t = 0;
        if (/^\d+$/.test(w[0])) { cur = +w[0]; t = 1; }
        const [cmd, a, b] = w.slice(t);
        if (cmd === "wait") cur += parseFloat(a) * (/s$/.test(a) && !/ms$/.test(a) ? 1000 : 1);
        else if (cmd === "quit") ev.push({ ms: cur, op: "quit" });
        else if (cmd === "btn" || cmd === "key") {
            const bi = BTN_NAME.indexOf(a.toUpperCase().replace("SCL", "SEL"));
            const ki = [...Array(27).keys()].find((k) => noteName(k).toUpperCase() === a.toUpperCase());
            const ctl = bi >= 0 ? { btn: bi } : ki !== undefined ? { key: ki } : null;
            if (!ctl) throw new Error(`script: unknown control ${a}`);
            if (b === "down" || b === "up") ev.push({ ms: cur, op: b, ...ctl });
            else {
                const d = b ? parseFloat(b) : 100;
                ev.push({ ms: cur, op: "down", ...ctl }, { ms: cur + d, op: "up", ...ctl });
                if (!t) cur += d;
            }
        } else if (cmd === "knob" || cmd === "turn") {
            const role = ENC_NAME.indexOf(a.toUpperCase() === "ALGO" ? "ALGORITHM" : a.toUpperCase());
            if (role < 0) throw new Error(`script: unknown knob ${a}`);
            ev.push({ ms: cur, op: "turn", role, n: b ? parseInt(b, 10) : 1 });
        } else throw new Error(`script: unknown command ${cmd}`);
    }
    ev.sort((x, y) => x.ms - y.ms);
    return { ev, end: ev.length ? ev[ev.length - 1].ms + 15 : 0 };
}

class Panel {                      // the page's side: held keys / buttons as matrix bits
    constructor(engine) {
        this.e = engine;
        this.t = engine.info().panelTable;
        this.keys = 0;
        this.buttons = 0;
    }
    apply(ev) {
        const enc = new Int32Array(7);
        if (ev.op === "down" || ev.op === "up") {
            const bit = ev.key !== undefined ? 1 << ev.key : 0, bbit = ev.btn !== undefined ? 1 << this.t.btnId[ev.btn] : 0;
            if (ev.op === "down") { this.keys |= bit; this.buttons |= bbit; }
            else { this.keys &= ~bit; this.buttons &= ~bbit; }
        } else if (ev.op === "turn") enc[this.t.encId[ev.role]] += ev.n * this.t.encDir[ev.role];
        this.set(enc);
    }
    set(enc = new Int32Array(7), vel = null) {
        this.e.setPanel({ keys: this.keys >>> 0, buttons: this.buttons, enc, master: undefined,
                          keyVel: vel || new Uint8Array(27) });
    }
}

// render quanta (128 frames at 44.1 kHz) until the device reaches ms; each(L, R) after each quantum
function renderTo(engine, ms, each) {
    const L = new Float32Array(128), R = new Float32Array(128);
    while (engine.deviceMs() < ms) {
        engine.render(L, R);
        if (each) each(L, R);
    }
}
function runScript(engine, lines, opts = {}) {
    const { ev, end } = parseScript(lines);
    const panel = opts.panel || new Panel(engine);
    let i = 0;
    const L = new Float32Array(128), R = new Float32Array(128);
    for (;;) {
        const ms = engine.deviceMs();
        let quit = false;
        while (i < ev.length && ev[i].ms <= ms) {
            if (ev[i].op === "quit") quit = true;
            else panel.apply(ev[i]);
            i++;
        }
        if (quit || (i >= ev.length && ms >= end)) break;
        engine.render(L, R);
        if (opts.each) opts.each(L, R);
    }
    return panel;
}

const peakOf = (L, R) => { let p = 0; for (let i = 0; i < L.length; i++) p = Math.max(p, Math.abs(L[i]), Math.abs(R[i])); return p; };
const blank = (px) => { for (let i = 1; i < px.length; i++) if (px[i] !== px[0]) return false; return true; };
const same = (a, b) => { for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; };

// ----------------------------------------------------------------------------------------------- per core ---
async function testCore(id) {
    const wasm = fs.readFileSync(path.join(BUILD, `${id}.wasm`));
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
    const ref = ["build-public", "build", "build-release"].map((b) => path.join(FM1VST_DIR, b, "conformance", `${id}.ppm`))
        .find((f) => fs.existsSync(f));
    if (ref) {
        const a = rgbOf(px), b = readPpm(ref);
        let diff = 0;
        for (let i = 0; i < W * H; i++) if (a[3 * i] !== b[3 * i] || a[3 * i + 1] !== b[3 * i + 1] || a[3 * i + 2] !== b[3 * i + 2]) diff++;
        res.ref = diff === 0 ? "identical" : `${diff} px differ`;
        check(diff === 0, `the first picture is FM1VST's (${path.relative(FM1VST_DIR, ref)}): ${res.ref}`);
    }
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
    const p7 = new Panel(e);
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

const ids = process.argv.slice(2).length ? process.argv.slice(2)
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
process.exit(fails || process.exitCode ? 1 : 0);
