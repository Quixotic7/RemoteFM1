// SPDX-License-Identifier: GPL-3.0-only
// What the native and the .rfm1 engines' Node tests share (web/engine/API.md section 4): PNGs of the screen, FM1VST's
// conformance files (core-glue/<family>/conformance_<id>.txt: a sound prelude, save / saved / reboot scripts) and a
// runner for their scripts through setPanel, as the page drives an engine. Node only.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

export const W = 240, H = 240;
export const BTN_NAME = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
export const ENC_NAME = ["SELECT", "ALGORITHM", "PRESETS", "KNOB1", "KNOB2", "KNOB3", "KNOB4"];
const NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export const noteName = (k) => NOTES[(53 + k) % 12] + (Math.floor((53 + k) / 12) - 1);

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
export function rgbOf(px) {               // RGB565 (native value) -> 8-bit RGB, as FM1VST's ppm.h fm1g_rgb_of
    const rgb = Buffer.alloc(W * H * 3);
    for (let i = 0; i < W * H; i++) {
        const c = px[i];
        rgb[3 * i] = Math.floor(((c >> 11) & 31) * 255 / 31);
        rgb[3 * i + 1] = Math.floor(((c >> 5) & 63) * 255 / 63);
        rgb[3 * i + 2] = Math.floor((c & 31) * 255 / 31);
    }
    return rgb;
}
export function writePng(file, px) {
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
export function readPpm(file) {           // a P6 240 x 240 file's pixels
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
// the first of files that exists, read (the defaults when none)
export function readConformance(files) {
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
export function parseScript(lines, fm1vstDir) {
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
        } else if (cmd === "sysex") {      // FM1VST's script_runner: a file of SysEx frames (a manager's upload)
            const f = line.replace(/^.*?\bsysex\s+/, "").trim();
            ev.push({ ms: cur, op: "sysex", file: path.isAbsolute(f) ? f : path.join(fm1vstDir, f) });
        } else throw new Error(`script: unknown command ${cmd}`);
    }
    ev.sort((x, y) => x.ms - y.ms);
    return { ev, end: ev.length ? ev[ev.length - 1].ms + 15 : 0 };
}

export class Panel {                      // the page's side: held keys / buttons as matrix bits
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
export function renderTo(engine, ms, each) {
    const L = new Float32Array(128), R = new Float32Array(128);
    while (engine.deviceMs() < ms) {
        engine.render(L, R);
        if (each) each(L, R);
    }
}
// sysex FILE: its frames (F0 .. F7) one at a time through midiIn, the next after the core's reply (midiOut's F7), as a
// manager waits for each answer; the script does not end before the last one is answered
export function frames(file) {
    const b = fs.readFileSync(file), out = [];
    for (let i = 0; i < b.length; i++) {
        if (b[i] !== 0xf0) continue;
        const j = b.indexOf(0xf7, i);
        if (j < 0) break;
        out.push(b.subarray(i, j + 1));
        i = j;
    }
    return out;
}
// opts: { panel, each(L, R), fm1vstDir (a sysex file's base) }
export function runScript(engine, lines, opts = {}) {
    const { ev, end } = parseScript(lines, opts.fm1vstDir || ".");
    const panel = opts.panel || new Panel(engine);
    let i = 0;
    const L = new Float32Array(128), R = new Float32Array(128);
    const sx = { q: [], busy: false, sent: 0, replies: 0 };
    for (;;) {
        const ms = engine.deviceMs();
        let quit = false;
        while (i < ev.length && ev[i].ms <= ms) {
            if (ev[i].op === "quit") quit = true;
            else if (ev[i].op === "sysex") sx.q.push(...frames(ev[i].file));
            else panel.apply(ev[i]);
            i++;
        }
        for (const b of engine.midiOut()) if (b === 0xf7 && sx.busy) { sx.busy = false; sx.replies++; }
        if (!sx.busy && sx.q.length) { engine.midiIn(sx.q.shift()); sx.busy = true; sx.sent++; }
        if ((quit || (i >= ev.length && ms >= end)) && !sx.busy && !sx.q.length) break;
        engine.render(L, R);
        if (opts.each) opts.each(L, R);
    }
    if (sx.sent) console.log(`    sysex: ${sx.sent} frames sent, ${sx.replies} replies`);
    return panel;
}

export const peakOf = (L, R) => { let p = 0; for (let i = 0; i < L.length; i++) p = Math.max(p, Math.abs(L[i]), Math.abs(R[i])); return p; };
export const blank = (px) => { for (let i = 1; i < px.length; i++) if (px[i] !== px[0]) return false; return true; };
export const same = (a, b) => { for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; };
