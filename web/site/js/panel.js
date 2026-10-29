// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 drawn on a canvas, and its input. The faceplate is RemoteFM1 for iOS's (RemoteFM1IOS: RFMKit/Faceplate,
// design/README.md): the controls where the FM-1 has them, raised caps with lit LEDs, knurled knobs, Chakra Petch
// legends, laid out for the stage's shape (a phone upright, a phone on its side, a tablet or a desktop window). The
// computer-key map and the input state are FM1VST plugin/PanelComponent.cpp's (itself ChoralRoot's tools/emu/emu.c).
//
// Orders. The panel names buttons by LABEL (FM1VST EMU_B_*: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT-
// OCT+) and knobs by ROLE (EMU_E_*: SELECT ALGORITHM PRESETS KNOB1..4 MASTER). The engine takes MATRIX order
// (web/engine/API.md, RemoteFM1 proto/rfm1.h): buttons 0 OCT- 1 OCT+ 2 FX .. 13 REC; encoders 0 SELECT 1 ALGO
// 2..5 KNOB1..4 6 PRESETS. labelToMatrix / roleToMatrix convert (FM1VST core-glue/common/panel.h PANEL_DEFAULT).

import { css, mix, resolve, accentOf, PRESETS, DEFAULT_THEME } from "./theme.js";

// ================================================================ the panel ===

export const B = { FX: 0, SEL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11, OCTDN: 12, OCTUP: 13 };
export const E = { SELECT: 0, ALGO: 1, PRESETS: 2, K1: 3, K2: 4, K3: 5, K4: 6, MASTER: 7 };
export const NKEY = 27, NB = 14, NE = 8;
export const PANEL_LABELS = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
const LCD = 240;

export const keyBlack = (k) => (0x54A >> ((k + 5) % 12)) & 1;
const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export const noteName = (k) => { const m = 53 + k; return NOTE[m % 12] + ((m / 12 | 0) - 1); };

export const labelToMatrix = (b) => (b < 12 ? b + 2 : b - 12);
const ROLE_MATRIX = [0, 1, 6, 2, 3, 4, 5];
export const roleToMatrix = (r) => ROLE_MATRIX[r];
// the FM-1's wiring (FM1VST core-glue/common/panel.h FM1P_KEYMAP_INIT): id (0..13 matrix button, 14 + n note key n)
// at (row, column); rows 1..4 are LED lines 0..3
export const DEFAULT_KEYMAP = [
  [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
  [5, 11, 4, 10, 3, 9, 2, 8, -1, -1, -1],
  [34, 35, 36, 37, 38, 40, 39, 13, 7, 6, 12],
  [23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33],
  [0, 1, 15, 14, 17, 16, 19, 18, 20, 21, 22],
  [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
];
// id -> index into the LED levels (col * 4 + line), -1 none
export function ledIndexOf(keymap) {
  const km = keymap || DEFAULT_KEYMAP, out = new Int16Array(41).fill(-1);
  for (let r = 1; r <= 4; r++) for (let c = 0; c < 11; c++) { const id = km[r] && km[r][c]; if (id >= 0 && id < 41) out[id] = c * 4 + (r - 1); }
  return out;
}

// =================================================================== LEDs ===
const LX = [0, 12, 40, 96, 255], LY = [0, 0.2, 0.38, 0.62, 1];
export function ledAlpha(level) {
  const v = Math.max(0, Math.min(255, level | 0));
  for (let i = 1; i < 5; i++) if (v <= LX[i]) return LY[i - 1] + (LY[i] - LY[i - 1]) * (v - LX[i - 1]) / (LX[i] - LX[i - 1]);
  return 1;
}
const ledLit = (level) => level >= 176;

// ================================================================= keymap ===
// keymap.c's KEYMAP as FM1VST has it, by KeyboardEvent.code (the physical key, any layout)
export const KM = { KEY: 0, BTN: 1, OCTBOTH: 2, SELECT: 3, CYCLE: 4, TURN: 6, LCDVIEW: 7 };
export const KEYMAP = [
  ["F1", KM.KEY, 1, "F1"], ["F2", KM.KEY, 3, "F2"], ["F3", KM.KEY, 5, "F3"], ["F4", KM.KEY, 8, "F4"],
  ["Digit2", KM.KEY, 0, "2"], ["Digit3", KM.KEY, 2, "3"], ["Digit4", KM.KEY, 4, "4"], ["Digit5", KM.KEY, 7, "5"],
  ["Tab", KM.KEY, 6, "TAB"],
  ["KeyA", KM.KEY, 7, "A"], ["KeyS", KM.KEY, 9, "S"], ["KeyD", KM.KEY, 11, "D"], ["KeyF", KM.KEY, 12, "F"],
  ["KeyG", KM.KEY, 14, "G"], ["KeyH", KM.KEY, 16, "H"], ["KeyJ", KM.KEY, 18, "J"], ["KeyK", KM.KEY, 19, "K"],
  ["KeyL", KM.KEY, 21, "L"], ["Semicolon", KM.KEY, 23, ";"], ["Quote", KM.KEY, 24, "'"], ["BracketRight", KM.KEY, 26, "]"],
  ["KeyW", KM.KEY, 8, "W"], ["KeyE", KM.KEY, 10, "E"], ["KeyT", KM.KEY, 13, "T"], ["KeyY", KM.KEY, 15, "Y"],
  ["KeyU", KM.KEY, 17, "U"], ["KeyO", KM.KEY, 20, "O"], ["KeyP", KM.KEY, 22, "P"],
  ["KeyZ", KM.BTN, B.OCTDN, "Z"], ["KeyX", KM.BTN, B.OCTUP, "X"],
  ["Escape", KM.BTN, B.OCTDN, "ESC"], ["Enter", KM.BTN, B.OCTUP, "RETURN"], ["End", KM.OCTBOTH, 0, "END"],
  ["F5", KM.BTN, B.FX, "F5"], ["F6", KM.BTN, B.SEL, "F6"], ["F7", KM.BTN, B.ENV, "F7"],
  ["F8", KM.BTN, B.LFO, "F8"], ["F9", KM.BTN, B.EDIT, "F9"], ["F10", KM.BTN, B.GLO, "F10"],
  ["Digit7", KM.BTN, B.HOME, "7"], ["Digit8", KM.BTN, B.SAVE, "8"], ["Digit9", KM.BTN, B.ARP, "9"],
  ["Digit0", KM.BTN, B.SEQ, "0"], ["Minus", KM.BTN, B.PLAY, "-"], ["Equal", KM.BTN, B.REC, "="],
  ["KeyC", KM.BTN, B.FX, "C"], ["KeyV", KM.BTN, B.SEL, "V"], ["KeyB", KM.BTN, B.ENV, "B"],
  ["KeyN", KM.BTN, B.LFO, "N"], ["KeyM", KM.BTN, B.EDIT, "M"], ["Comma", KM.BTN, B.GLO, ","],
  ["PageDown", KM.CYCLE, 1, "PGDN"], ["PageUp", KM.CYCLE, -1, "PGUP"],
  ["ArrowUp", KM.TURN, 1, "UP"], ["ArrowDown", KM.TURN, -1, "DOWN"],
  ["Backquote", KM.LCDVIEW, 0, "`"],
].map(([code, kind, idx, cap]) => ({ code, kind, idx, cap }));
export function hintText(kind, idx) {
  const caps = KEYMAP.filter((e) => e.kind === kind && e.idx === idx).map((e) => e.cap);
  if (!caps.length) return "";
  const f = /^F\d/.test(caps[0]);
  return f && caps.length > 1 ? caps[0] + "/" + caps[1] : caps[0];
}
const KEY_HINT = Array.from({ length: NKEY }, (_, k) => hintText(KM.KEY, k));
const BTN_HINT = Array.from({ length: NB }, (_, b) => hintText(KM.BTN, b));
// ================================================================ controls ===
// The panel's input state, from every source (bits): the computer keys, the pointer, a latch, Web MIDI, the user's
// own FM-1 (web/remote), a learned MIDI control (js/midi-map.js). Every change calls onChange(msg) with the engine's
// panel in matrix order; enc holds the detents since the previous message.
export const SRC = { KEY: 1, ESC: 2, POINTER: 4, LATCH: 8, MIDI: 16, REMOTE: 32, MAP: 64 };
export class Controls {
  constructor(onChange) {
    this.onChange = onChange;
    this.keySrc = new Uint8Array(NKEY);
    this.btnSrc = new Uint8Array(NB);
    this.keyVel = new Uint8Array(NKEY);
    this.angle = new Float32Array(NE);
    this.encPending = new Int32Array(7);
    this.master = 768;
    this.selKnob = E.PRESETS;
    this.onDraw = null;
    this.angle[E.MASTER] = masterAngle(this.master);
  }
  dirty() { if (this.onDraw) this.onDraw(); }
  keyHeld(k) { return this.keySrc[k] !== 0; }
  btnHeld(b) { return this.btnSrc[b] !== 0; }
  // a latch (input.js: Shift-click, right-click or ctrl-click) holds a key or button until it is clicked again
  keyLatched(k) { return (this.keySrc[k] & SRC.LATCH) !== 0; }
  btnLatched(b) { return (this.btnSrc[b] & SRC.LATCH) !== 0; }
  anyLatched() { return this.keySrc.some((v) => v & SRC.LATCH) || this.btnSrc.some((v) => v & SRC.LATCH); }
  releaseLatches() { this.releaseSrc(SRC.LATCH); }
  holdKey(k, src, down, vel = 0) {
    if (k < 0 || k >= NKEY) return;
    const was = this.keySrc[k] !== 0;
    this.keySrc[k] = down ? this.keySrc[k] | src : this.keySrc[k] & ~src;
    if (down && vel) this.keyVel[k] = vel;
    if (!down && !this.keySrc[k]) this.keyVel[k] = 0;
    if (was !== (this.keySrc[k] !== 0)) this.emit();
    this.dirty();
  }
  holdBtn(b, src, down) {
    if (b < 0 || b >= NB) return;
    const was = this.btnSrc[b] !== 0;
    this.btnSrc[b] = down ? this.btnSrc[b] | src : this.btnSrc[b] & ~src;
    if (was !== (this.btnSrc[b] !== 0)) this.emit();
    this.dirty();
  }
  releaseSrc(mask) {
    let ch = false;
    for (let k = 0; k < NKEY; k++) if (this.keySrc[k] & mask) { this.keySrc[k] &= ~mask; if (!this.keySrc[k]) this.keyVel[k] = 0; ch = true; }
    for (let b = 0; b < NB; b++) if (this.btnSrc[b] & mask) { this.btnSrc[b] &= ~mask; ch = true; }
    if (ch) { this.emit(); this.dirty(); }
  }
  // a knob turned by detents (+ = clockwise); MASTER moves 16 per detent
  turn(role, steps) {
    if (!steps || role < 0 || role >= NE) return;
    if (role === E.MASTER) return this.setMaster(this.master + 16 * steps);
    this.angle[role] = (this.angle[role] + steps * (2 * Math.PI / 24)) % (2 * Math.PI);
    this.encPending[roleToMatrix(role)] += steps;
    this.emit();
    this.dirty();
  }
  setMaster(v) {
    v = Math.max(0, Math.min(1023, Math.round(v)));
    if (v === this.master) return;
    this.master = v;
    this.angle[E.MASTER] = masterAngle(v);
    this.emit();
    this.dirty();
  }
  select(role) { this.selKnob = role; this.dirty(); }
  // the engine's panel (API.md section 2), matrix order
  snapshot() {
    let keys = 0, buttons = 0;
    for (let k = 0; k < NKEY; k++) if (this.keySrc[k]) keys |= 1 << k;
    for (let b = 0; b < NB; b++) if (this.btnSrc[b]) buttons |= 1 << labelToMatrix(b);
    const enc = this.encPending.slice();
    this.encPending.fill(0);
    return { keys: keys >>> 0, buttons: buttons >>> 0, enc, master: this.master, keyVel: this.keyVel.slice() };
  }
  emit() { if (this.onChange) this.onChange(this.snapshot()); }
}
const masterAngle = (m) => (m / 1023 - 0.5) * 1.5 * Math.PI;

// =================================================================== the LCD ===
// one 240x240 canvas the device and the screen views draw from; pixels: RGB565 as the panel shows it (host order)
export class Lcd {
  constructor() {
    this.canvas = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(LCD, LCD) : Object.assign(document.createElement("canvas"), { width: LCD, height: LCD });
    this.ctx = this.canvas.getContext("2d");
    this.img = this.ctx.createImageData(LCD, LCD);
    this.seq = 0;
    this.ctx.fillStyle = "#000";
    this.ctx.fillRect(0, 0, LCD, LCD);
  }
  put(pixels) {
    const d = this.img.data;
    for (let i = 0, o = 0; i < LCD * LCD; i++, o += 4) {
      const p = pixels[i], r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
      d[o] = (r << 3) | (r >> 2); d[o + 1] = (g << 2) | (g >> 4); d[o + 2] = (b << 3) | (b >> 2); d[o + 3] = 255;
    }
    this.ctx.putImageData(this.img, 0, 0);
    this.seq++;
  }
  clear() { this.ctx.fillStyle = "#000"; this.ctx.fillRect(0, 0, LCD, LCD); this.seq++; }
}

// ================================================================ the layouts ===
// RemoteFM1 for iOS's faceplate (RemoteFM1IOS: RFMKit Faceplate/FaceplateLayout.swift, design/README.md section 4):
// the controls where the FM-1 has them, laid out for four reference frames (points). The stage picks one by its own
// shape: upright and under 600 wide, the phone's; wide and under 500 high, the phone's landscape; else the tablet's.
// The page's header is HTML, so the app's status row is left out: `box` is the part of the frame the stage shows.
// Positions follow the stage's width and height; square parts (knobs, the display) scale by the smaller of the two,
// so nothing stretches; when the shapes differ a lot, the frame scales uniformly and is centred on the faceplate.
export const LAYOUTS = {
  phonePortrait: {
    ref: [390, 844], box: { x: 0, y: 74, w: 390, h: 744 },
    main: { x: 16, y: 96, w: 148, knob: 46, rowGap: 8 },
    oct: { x: 16, y: 246, w: 148, h: 52, pad: 5, gap: 6, r: 14, br: 10, font: 13 },
    display: { x: 172, y: 96, size: 202 },
    kn: { row: true, x: 16, y: 316, w: 358, knob: 46, led: 10 },
    buttons: { x: 21, y: 400, bw: 48, bh: 42, gap: 8 },
    keybed: { x: 16, y: 530, w: 358, h: 272, keyW: 40 },
  },
  phoneLandscape: {
    ref: [844, 390], box: { x: 30, y: 8, w: 784, h: 376 },
    main: { x: 54, y: 20, w: 132, knob: 38, rowGap: 2 },
    oct: { x: 54, y: 144, w: 132, h: 48, pad: 3, gap: 4, r: 12, br: 9, font: 12 },
    display: { x: 198, y: 20, size: 172 },
    kn: { row: true, x: 386, y: 20, w: 408, knob: 38, led: 9 },
    buttons: { x: 386, y: 88, bw: 58, bh: 38, gap: 8 },
    keybed: { x: 50, y: 206, w: 744, h: 170 },
  },
  padLandscape: {
    ref: [1194, 834], box: { x: 24, y: 66, w: 1146, h: 744 },
    main: { x: 48, y: 88, w: 192, knob: 64, rowGap: 24 },
    oct: { x: 48, y: 334, w: 192, h: 54, pad: 5, gap: 6, r: 15, br: 11, font: 15 },
    display: { x: 272, y: 88, size: 300 },
    kn: { row: true, x: 604, y: 88, w: 542, knob: 64, led: 12 },
    buttons: { x: 606, y: 234, bw: 78, bh: 60, gap: 10 },
    keybed: { x: 48, y: 424, w: 1098, h: 362 },
  },
  padPortrait: {
    ref: [834, 1194], box: { x: 16, y: 66, w: 802, h: 1094 },
    main: { x: 40, y: 88, w: 224, knob: 84, rowGap: 8 },
    oct: { x: 40, y: 590, w: 220, h: 54, pad: 5, gap: 6, r: 15, br: 11, font: 15 },
    display: { x: 272, y: 88, size: 290 },
    kn: { row: false, x: 570, y: 88, w: 224, knob: 84, rowGap: 8, ledX: 677, ledY: 344, led: 10 },
    buttons: { x: 77, y: 404, bw: 100, bh: 64, gap: 12 },
    keybed: { x: 40, y: 660, w: 754, h: 476 },
  },
};
export function pickLayout(w, h) {
  if (h >= w) return w < 600 ? "phonePortrait" : "padPortrait";
  return h < 500 ? "phoneLandscape" : "padLandscape";
}
const STRETCH = 1.25, MAX_SCALE = 1.6;
// the frame's box -> the stage (w x h CSS pixels)
export function mapper(box, w, h) {
  const ax = w / box.w, ay = h / box.h;
  let sx = ax, sy = ay;
  if (Math.max(ax, ay) / Math.min(ax, ay) > STRETCH) sx = sy = Math.min(ax, ay);
  const m = Math.min(sx, sy);
  if (m > MAX_SCALE) { sx *= MAX_SCALE / m; sy *= MAX_SCALE / m; }
  const ox = (w - box.w * sx) / 2 - box.x * sx, oy = (h - box.h * sy) / 2 - box.y * sy;
  return { sx, sy, s: Math.min(sx, sy), x: (v) => ox + v * sx, y: (v) => oy + v * sy };
}

// the knobs: MAIN (left cluster) and KN 1-4, by role (E), with their printed labels
const MAIN_KNOBS = [[E.MASTER, "MASTER"], [E.SELECT, "SELECT"], [E.PRESETS, "PRESETS"], [E.ALGO, "ALGORITHM"]];
const KN_KNOBS = [[E.K1, "KN1"], [E.K2, "KN2"], [E.K3, "KN3"], [E.K4, "KN4"]];
// a knob's cell (KnobView.cell): the label lf high, 6, the knob, 6
const knobCell = (size) => { const lf = Math.max(9, Math.round(size * 0.17)); return { lf, w: size + 28, h: lf + 6 + size + 6 }; };

// ============================================================ the keybed's geometry ===
// KeybedGeometry.swift: 16 naturals in the lower row, 11 sharps above, each sharp on the gap after its natural; every
// key has a centred LED slot. row: all 27 across; scroll (a phone upright): naturals keyW wide, about 8 in view, a
// range strip of 16 cells above them (a frame shows the part in view; a tap centres there, a drag scrolls).
const NATURALS = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const SHARP_AFTER = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14];
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
export function keybedGeometry(W, H, keyW = 0) {
  const scroll = keyW > 0;
  const pad = clamp(Math.round(Math.min(W, H) * 0.05), 8, 18), gap = clamp(Math.round(W * 0.008), 3, 10);
  const g = { W, H, scroll, pad, gap, radius: Math.round(Math.min(W, H) * 0.08), stripH: 0, stripGap: 0, keys: [] };
  let ox = 0, oy = 0;
  if (scroll) {
    g.stripH = clamp(Math.round(H * 0.11), 22, 34);
    g.stripGap = Math.max(8, gap + 4);
    g.pitch = keyW + gap;
    const sy = pad + g.stripH + g.stripGap;
    g.stage = { x: pad, y: sy, w: W - 2 * pad, h: H - sy - pad };
    g.visible = (g.stage.w + gap) / g.pitch;
    g.maxOffset = Math.max(0, 16 - g.visible);
  } else {
    g.pitch = (W - 2 * pad + gap) / 16;
    g.stage = { x: 0, y: 0, w: W, h: H };
    g.visible = 16; g.maxOffset = 0;
    ox = pad; oy = pad;
  }
  const kw = g.pitch - gap, rowH = scroll ? g.stage.h : H - 2 * pad, vg = gap + 2;
  const bh = Math.round(rowH * 0.4), wh = Math.round(rowH - bh - vg - 4), bw = kw * 0.86, lw = Math.max(3, Math.round(kw * 0.08));
  NATURALS.forEach((k, i) => g.keys.push({ k, natural: true, step: i, x: ox + i * g.pitch, y: oy + bh + vg, w: kw, h: wh,
    r: Math.round(Math.min(kw * 0.42, 18)), lw, lh: Math.round(wh * 0.42) }));
  SHARP_AFTER.forEach((after) => {
    const cx = ox + (after + 1) * g.pitch - gap / 2;
    g.keys.push({ k: NATURALS[after] + 1, natural: false, step: -1, x: cx - bw / 2, y: oy, w: bw, h: bh,
      r: Math.round(Math.min(bw * 0.42, 18)), lw, lh: Math.round(bh * 0.42) });
  });
  g.defaultOffset = Math.min(g.maxOffset, 4);
  g.shift = (o) => -clamp(o, 0, g.maxOffset) * g.pitch;
  g.centering = (i) => clamp(i - g.visible / 2 + 0.5, 0, g.maxOffset);
  const mw = W - 2 * pad, cw = (mw - 15 * 2) / 16;
  g.stripCell = (i) => ({ x: pad + i * (cw + 2), y: pad, w: cw, h: g.stripH });
  g.stripWindow = (o) => ({ x: pad + clamp(o, 0, g.maxOffset) / 16 * mw, y: pad - 3, w: Math.min(g.visible, 16) / 16 * mw, h: g.stripH + 6 });
  g.stripIndex = (x) => Math.floor(clamp((x - pad) / mw * 16, 0, 15.999));
  g.stripOffsetAt = (x) => clamp((x - pad) / mw * 16 - g.visible / 2, 0, g.maxOffset);
  return g;
}

// ========================================================= drawing helpers ===
function rr(ctx, x, y, w, h, r) {
  r = Math.max(0, Math.min(r, w / 2, h / 2));
  ctx.beginPath();
  ctx.roundRect(x, y, w, h, r);
}
function fillRR(ctx, x, y, w, h, r, fill) { rr(ctx, x, y, w, h, r); ctx.fillStyle = fill; ctx.fill(); }
// a raised cap (Parts.swift CapFace): its hard shadow lo `drop` below, the lit top edge hi, the face 2 lower
function cap(ctx, x, y, w, h, r, face, hi, lo, drop = 4) {
  fillRR(ctx, x, y + drop, w, h, r, lo);
  ctx.save();
  rr(ctx, x, y, w, h, r); ctx.clip();
  ctx.fillStyle = hi; ctx.fillRect(x, y, w, h);
  fillRR(ctx, x, y + 2, w, h, r, face);
  ctx.restore();
}
// a recessed well (Parts.swift Well): the bed, darker at the top edge
function well(ctx, x, y, w, h, r, bed, inset = 2, dark = 0.18) {
  ctx.save();
  rr(ctx, x, y, w, h, r); ctx.clip();
  ctx.fillStyle = bed; ctx.fillRect(x, y, w, h);
  ctx.fillStyle = `rgba(0,0,0,${dark})`; ctx.fillRect(x, y, w, h);
  fillRR(ctx, x, y + inset, w, h, r, bed);
  ctx.restore();
}
function circle(ctx, cx, cy, r, fill) { ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.fillStyle = fill; ctx.fill(); }
const HW = '"Chakra Petch", "DM Sans", system-ui, sans-serif';
// a hardware legend (Chakra Petch Bold), centred on cx with its middle at cy, shrunk to maxW; tracking in em
function legend(ctx, s, cx, cy, size, fill, maxW = Infinity, tracking = 0.06, glow = null) {
  s = String(s);
  ctx.font = `700 ${size}px ${HW}`;
  const ls = "letterSpacing" in ctx;
  if (ls) ctx.letterSpacing = `${(size * tracking).toFixed(2)}px`;
  let w = ctx.measureText(s).width;
  if (w > maxW) {
    size = Math.max(size * 0.5, size * maxW / w);
    ctx.font = `700 ${size}px ${HW}`;
    if (ls) ctx.letterSpacing = `${(size * tracking).toFixed(2)}px`;
  }
  ctx.textAlign = "center"; ctx.textBaseline = "middle";
  if (glow) { ctx.shadowColor = glow; ctx.shadowBlur = 4; }
  // (letter spacing adds after the last glyph too: half of it shifts the centre)
  ctx.fillStyle = fill; ctx.fillText(s, cx + (ls ? size * tracking / 2 : 0), cy);
  if (glow) { ctx.shadowBlur = 0; ctx.shadowColor = "transparent"; }
  if (ls) ctx.letterSpacing = "0px";
}
const mixCss = (a, b, t) => css(mix(a, b, Math.max(0, Math.min(1, t))));

// the display (Parts.swift DisplayView): the bezel with its hard shadow, the LCD inside it pixel for pixel when a
// whole multiple of 240 fits (within 8 %), nearest-neighbour; x, y, size in CSS pixels
function drawDisplay(g, p, lcd, x, y, size, dpr, base) {
  const pad = Math.max(6, Math.round(size * 0.045)), br = Math.max(10, Math.round(size * 0.09)), lr = Math.max(4, Math.round(size * 0.035));
  if (base) {
    fillRR(g, x, y + 4, size, size, br, "rgba(0,0,0,0.25)");
    fillRR(g, x, y, size, size, br, css(p.bezel));
    return;
  }
  const inner = (size - 2 * pad) * dpr, k = Math.floor(inner / LCD);
  const s = k >= 1 && k * LCD >= inner * 0.92 ? k * LCD : Math.round(inner);
  const cx = (x + size / 2) * dpr, cy = (y + size / 2) * dpr;
  const x0 = Math.round(cx - s / 2), y0 = Math.round(cy - s / 2);
  g.save();
  g.setTransform(1, 0, 0, 1, 0, 0);
  rr(g, x0, y0, s, s, lr * dpr * s / inner); g.clip();
  g.fillStyle = "#000"; g.fillRect(x0, y0, s, s);
  g.imageSmoothingEnabled = s < LCD;
  if (lcd) g.drawImage(lcd.canvas, x0, y0, s, s);
  g.restore();
  g.imageSmoothingEnabled = true;
}

// ============================================================ device view ===
// The faceplate on a canvas, edge to edge in the theme's body colour. Coordinates are the canvas's CSS pixels:
// toUnits is the identity (input.js asks it, then hit()).
export class DeviceView {
  constructor(canvas, controls, lcd) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d");
    this.controls = controls;
    this.lcd = lcd;
    this.names = PANEL_LABELS.slice();
    this.levels = new Uint8Array(44);
    this.playGreen = 0;
    this.ledIdx = ledIndexOf(null);
    this.setTheme(PRESETS.find((p) => p.name === DEFAULT_THEME));
    this.dpr = 1; this.cssW = 0; this.cssH = 0;
    this.base = null;
    this.hints = false;
    this.running = false;
    this.keyOffset = -1;      // the scrolling keybed's offset in naturals (-1: its default)
    this.L = null;
    this.learn = null;        // MIDI learn (main.js): { knobs: Set of roles bound, btns: Set bound, sel: { kind, idx } | null }
    this.potAt = null;        // a learned MASTER pot waiting for pickup: its position 0..1023 (a faint mark)
  }
  setTheme(t) { this.theme = t; this.pal = resolve(t); this.accent = accentOf(t); this.base = null; }
  // the computer-key hints (the sheet's "Show computer-key hints", off by default): the letter under a key's LED, or
  // under a button's name
  setHints(on) { this.hints = !!on; }
  setRunning(on) { this.running = !!on; }
  setButtonNames(n) { this.names = (n && n.length === NB ? n : PANEL_LABELS).map((s) => String(s || "")); this.base = null; }
  setKeymap(km) { this.ledIdx = ledIndexOf(km); }
  setLeds(levels, playGreen) { this.levels = levels; this.playGreen = playGreen | 0; }
  setLearn(l) { this.learn = l || null; }
  setPotAt(v) { this.potAt = typeof v === "number" ? v : null; }
  keyLevel(k) { const i = this.ledIdx[14 + k]; return i >= 0 ? this.levels[i] : 0; }
  btnLevel(b) { const i = this.ledIdx[labelToMatrix(b)]; return i >= 0 ? this.levels[i] : 0; }

  // the canvas's CSS box (w x h) at dpr: the layout for its shape, mapped to it
  layout(w, h, dpr) {
    const W = Math.max(1, Math.round(w * dpr)), H = Math.max(1, Math.round(h * dpr));
    if (this.canvas.width !== W || this.canvas.height !== H) { this.canvas.width = W; this.canvas.height = H; }
    this.dpr = dpr; this.cssW = w; this.cssH = h;
    this.kind = pickLayout(w, h);
    const R = LAYOUTS[this.kind], m = mapper(R.box, w, h), s = m.s, L = { m, s, knobs: [], btns: [] };
    const placeGrid = (list, c) => {
      const size = c.knob * s, cell = knobCell(size), colW = c.w * m.sx / 2;
      list.forEach(([role, label], i) => {
        const col = i % 2, row = (i / 2) | 0, top = m.y(c.y) + row * (cell.h + c.rowGap * m.sy);
        L.knobs.push({ role, label, size, lf: cell.lf, cx: m.x(c.x) + colW * (col + 0.5), top, cy: top + cell.lf + 6 + size / 2, labelW: colW - 4, cellW: cell.w, cellH: cell.h });
      });
    };
    placeGrid(MAIN_KNOBS, R.main);
    if (R.kn.row) {
      const k = R.kn, size = k.knob * s, cell = knobCell(size), w2 = k.w * m.sx, ledW = k.led * s;
      const gap = (w2 - ledW - 4 * cell.w) / 4, top = m.y(k.y), cy = top + cell.lf + 6 + size / 2;
      L.indicator = { x: m.x(k.x), y: cy - ledW / 2, size: ledW };
      KN_KNOBS.forEach(([role, label], i) => L.knobs.push({ role, label, size, lf: cell.lf, cx: m.x(k.x) + ledW + gap * (i + 1) + cell.w * (i + 0.5),
        top, cy, labelW: cell.w + gap - 4, cellW: cell.w, cellH: cell.h }));
    } else {
      placeGrid(KN_KNOBS, R.kn);
      L.indicator = { x: m.x(R.kn.ledX), y: m.y(R.kn.ledY), size: R.kn.led * s };
    }
    // OCT- / OCT+
    const o = R.oct, ow = { x: m.x(o.x), y: m.y(o.y), w: o.w * m.sx, h: o.h * m.sy, r: o.r * s };
    const op = o.pad * s, og = o.gap * s, obw = (ow.w - 2 * op - og) / 2;
    L.octWell = ow;
    [B.OCTDN, B.OCTUP].forEach((b, i) => L.btns.push({ b, x: ow.x + op + i * (obw + og), y: ow.y + op, w: obw, h: ow.h - 2 * op - 3, r: o.br * s, font: o.font * s, drop: 3 }));
    // the 6 x 2 buttons in their well
    const bt = R.buttons, bw = bt.bw * m.sx, bh = bt.bh * m.sy, bg = bt.gap * s, bp = 10 * s;
    const wellW = 6 * bw + 5 * bg + 2 * bp, wellH = 2 * bh + bg + 2 * bp;
    const refW = (6 * bt.bw + 5 * bt.gap + 20) * m.sx;
    L.btnWell = { x: m.x(bt.x) + (refW - wellW) / 2, y: m.y(bt.y), w: wellW, h: wellH, r: Math.round(bh * 0.34) + 6 * s };
    for (let i = 0; i < 12; i++) L.btns.push({ b: i, x: L.btnWell.x + bp + (i % 6) * (bw + bg), y: L.btnWell.y + bp + ((i / 6) | 0) * (bh + bg), w: bw, h: bh,
      r: Math.round(bh * 0.24), font: Math.max(10, Math.round(bh * 0.26)), drop: 4, row: (i / 6) | 0 });
    // the display
    const d = R.display, ds = d.size * s;
    L.display = { x: m.x(d.x) + d.size * m.sx / 2 - ds / 2, y: m.y(d.y), size: ds };
    // the keybed
    const kb = R.keybed;
    L.keybed = { x: m.x(kb.x), y: m.y(kb.y), w: kb.w * m.sx, h: kb.h * m.sy };
    L.kg = keybedGeometry(L.keybed.w, L.keybed.h, kb.keyW ? kb.keyW * m.sx : 0);
    // the room above the faceplate's first row (the grabber sits there)
    L.top = Math.min(L.display.y, ...L.knobs.map((k) => k.top));
    this.L = L;
    this.base = null;
  }
  toUnits(cx, cy) { return { x: cx, y: cy }; }
  toCss(x, y) { return { x, y }; }
  unitPx() { return 1; }
  // where the page puts its own parts: "lcd" (the start overlay), "grabber" (the sheet's handle)
  anchor(name) {
    const L = this.L;
    if (!L) return { x: this.cssW / 2, y: this.cssH / 2 };
    if (name === "lcd") return { x: L.display.x + L.display.size / 2, y: L.display.y + L.display.size / 2 };
    return { x: this.cssW / 2, y: Math.max(4, L.top / 2), room: L.top };
  }
  // a control's centre in CSS pixels (KM.KEY / KM.BTN / KM.SELECT and its index; the e2e tests click there)
  where(kind, idx) {
    const L = this.L;
    if (!L) return null;
    if (kind === KM.BTN) { const b = L.btns.find((x) => x.b === idx); return b && { x: b.x + b.w / 2, y: b.y + b.h / 2 }; }
    if (kind === KM.SELECT) { const k = L.knobs.find((x) => x.role === idx); return k && { x: k.cx, y: k.cy }; }
    if (kind === KM.KEY) {
      const g = L.kg, k = g.keys.find((x) => x.k === idx);
      if (!k) return null;
      const ox = g.scroll ? g.stage.x + g.shift(this.keyOff()) : 0, oy = g.scroll ? g.stage.y : 0;
      return { x: L.keybed.x + ox + k.x + k.w / 2, y: L.keybed.y + oy + k.y + k.h / 2 };
    }
    return null;
  }
  keyOff() { const g = this.L && this.L.kg; return !g || !g.scroll ? 0 : this.keyOffset < 0 ? g.defaultOffset : Math.min(this.keyOffset, g.maxOffset); }
  // the range strip (input.js): a tap centres the keys on that cell; a drag scrolls
  stripTo(x, drag) {
    const g = this.L && this.L.kg;
    if (!g || !g.scroll) return;
    const lx = x - this.L.keybed.x;
    this.keyOffset = drag ? g.stripOffsetAt(lx) : g.centering(g.stripIndex(lx));
  }

  hit(x, y) {
    const L = this.L;
    if (!L) return null;
    const kb = L.keybed, g = L.kg;
    if (x >= kb.x && x < kb.x + kb.w && y >= kb.y && y < kb.y + kb.h) {
      let lx = x - kb.x, ly = y - kb.y;
      if (g.scroll) {
        if (ly < g.pad + g.stripH + g.stripGap / 2) return { kind: "strip" };
        if (lx < g.stage.x || lx >= g.stage.x + g.stage.w || ly < g.stage.y) return null;
        lx -= g.stage.x + g.shift(this.keyOff()); ly -= g.stage.y;
      }
      const slop = g.gap / 2;
      for (const k of g.keys) if (lx >= k.x - slop && lx < k.x + k.w + slop && ly >= k.y && ly < k.y + k.h) return { kind: KM.KEY, idx: k.k };
      return null;
    }
    for (const b of L.btns) if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h + b.drop) return { kind: KM.BTN, idx: b.b };
    for (const k of L.knobs) {
      const w = Math.max(44, k.cellW), h = Math.max(44, k.cellH);
      if (Math.abs(x - k.cx) <= w / 2 && y >= k.top - (h - k.cellH) / 2 && y < k.top + h) return { kind: KM.SELECT, idx: k.role };
    }
    const d = L.display;
    if (x >= d.x && x < d.x + d.size && y >= d.y && y < d.y + d.size) return { kind: "screen" };
    return null;
  }

  // what does not change from frame to frame: the faceplate, the wells, the knobs' labels, the bezel
  drawBase() {
    const W = this.canvas.width, H = this.canvas.height, L = this.L, p = this.pal;
    const c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(W, H) : Object.assign(document.createElement("canvas"), { width: W, height: H });
    const g = c.getContext("2d");
    g.fillStyle = css(p.body); g.fillRect(0, 0, W, H);
    g.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    if (L) {
      const ow = L.octWell, bw = L.btnWell, kb = L.keybed;
      well(g, ow.x, ow.y, ow.w, ow.h, ow.r, css(p.bed));
      well(g, bw.x, bw.y, bw.w, bw.h, bw.r, css(p.bed));
      well(g, kb.x, kb.y, kb.w, kb.h, L.kg.radius, css(p.bed), 3, 0.2);
      for (const k of L.knobs) legend(g, k.label, k.cx, k.top + k.lf / 2, k.lf, css(p.label), Math.max(k.size, k.labelW), 0.08);
      // the printed names where the firmware calls a button something else: above the top row, below the bottom one
      for (const b of L.btns) {
        if (b.b >= 12 || this.names[b.b].toUpperCase() === PANEL_LABELS[b.b]) continue;
        const room = b.y - bw.y, fs = Math.max(6, Math.min(9, room * 0.75));
        const y = b.row ? b.y + b.h + b.drop + (bw.y + bw.h - b.y - b.h - b.drop) / 2 : bw.y + room / 2;
        legend(g, PANEL_LABELS[b.b], b.x + b.w / 2, y, fs, css(p.keyText, 0.7), b.w);
      }
      drawDisplay(g, p, null, L.display.x, L.display.y, L.display.size, this.dpr, true);
    }
    this.base = c;
  }

  draw() {
    if (!this.L) return;
    if (!this.base) this.drawBase();
    const g = this.ctx, p = this.pal, ctl = this.controls, L = this.L;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.drawImage(this.base, 0, 0);
    g.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    // the indicator LED: lit while the firmware runs
    const ind = L.indicator;
    if (this.running) { g.shadowColor = css(p.indicator); g.shadowBlur = ind.size * 0.6; }
    fillRR(g, ind.x, ind.y, ind.size, ind.size, ind.size * 0.22, css(p.indicator, this.running ? 1 : 0.3));
    g.shadowBlur = 0; g.shadowColor = "transparent";
    for (const k of L.knobs) this.drawKnob(g, p, k, ctl.angle[k.role], ctl.selKnob === k.role);
    for (const b of L.btns) this.drawButton(g, p, b, ctl.btnHeld(b.b), this.btnLevel(b.b), b.b === B.PLAY ? this.playGreen : 0);
    this.drawKeybed(g, p);
    for (const b of L.btns) if (ctl.btnLatched(b.b)) this.drawLatch(g, b);
    drawDisplay(g, p, this.lcd, L.display.x, L.display.y, L.display.size, this.dpr, false);
    if (this.potAt !== null) this.drawPotAt(g, p);
    if (this.learn) this.drawLearn(g, p);
  }

  // MIDI learn: every learnable control outlined (dashed; solid when it has a binding), the picked one pulsing.
  // Knobs sit on the faceplate (the label's colour), buttons in their well (the LED's)
  drawLearn(g, p) {
    const l = this.learn, t = (typeof performance !== "undefined" ? performance.now() : 0) / 1000, pulse = 0.5 + 0.5 * Math.sin(t * 2 * Math.PI * 1.2);
    const mark = (sel, bound, col, path) => {
      path();
      g.setLineDash(sel || bound ? [] : [4, 4]);
      g.lineWidth = sel ? 2.5 + 1.5 * pulse : bound ? 2 : 1.5;
      g.strokeStyle = css(col, sel ? 0.55 + 0.45 * pulse : bound ? 0.95 : 0.6);
      if (sel) { g.shadowColor = css(col); g.shadowBlur = 6 + 6 * pulse; }
      g.stroke();
      g.shadowBlur = 0; g.shadowColor = "transparent"; g.setLineDash([]);
    };
    for (const k of this.L.knobs) {
      const sel = !!l.sel && l.sel.kind === KM.SELECT && l.sel.idx === k.role, bound = l.knobs.has(k.role), r = k.size / 2 + 6;
      mark(sel, bound, p.label, () => { g.beginPath(); g.arc(k.cx, k.cy, r, 0, 2 * Math.PI); });
      if (bound) { circle(g, k.cx + r * 0.71, k.cy - r * 0.71, 4.5, css(p.body)); circle(g, k.cx + r * 0.71, k.cy - r * 0.71, 3, css(p.label)); }
    }
    for (const b of this.L.btns) {
      const sel = !!l.sel && l.sel.kind === KM.BTN && l.sel.idx === b.b, bound = l.btns.has(b.b), o = 3;
      mark(sel, bound, p.led, () => rr(g, b.x - o, b.y - o, b.w + 2 * o, b.h + b.drop + 2 * o, (b.r || 6) + o));
    }
  }
  // the pot's position on MASTER while it waits to pick MASTER up: a small hollow dot outside the ring
  drawPotAt(g, p) {
    const k = this.L.knobs.find((x) => x.role === E.MASTER);
    if (!k) return;
    const a = masterAngle(this.potAt), r = k.size / 2 + 5;
    g.beginPath(); g.arc(k.cx + r * Math.sin(a), k.cy - r * Math.cos(a), 2.6, 0, 2 * Math.PI);
    g.lineWidth = 1.5; g.strokeStyle = css(p.label, 0.75); g.stroke();
  }

  // Parts.swift KnobBody: the knurled ring, the cap, the pointer; MASTER's angle is its value, the others turn
  drawKnob(g, p, k, a, sel) {
    const r = k.size / 2, u = k.size / 100, cx = k.cx, cy = k.cy;
    circle(g, cx, cy + Math.max(3, Math.round(k.size * 0.06)), r, "rgba(0,0,0,0.28)");
    circle(g, cx, cy, r, css(p.knob));
    g.beginPath(); g.arc(cx, cy, r - 5 * u, 0, 2 * Math.PI);
    g.setLineDash([3.2 * u, 3.9 * u]); g.lineWidth = 8 * u; g.strokeStyle = "rgba(255,255,255,0.16)"; g.stroke(); g.setLineDash([]);
    circle(g, cx, cy, r - 14 * u, css(p.knob));
    circle(g, cx, cy, r - 14 * u, "rgba(255,255,255,0.07)");
    const sa = Math.sin(a), ca = Math.cos(a);
    g.beginPath(); g.moveTo(cx + 20 * u * sa, cy - 20 * u * ca); g.lineTo(cx + 40 * u * sa, cy - 40 * u * ca);
    g.lineWidth = 7 * u; g.lineCap = "round"; g.strokeStyle = css(p.mark); g.stroke(); g.lineCap = "butt";
    if (sel) { g.beginPath(); g.arc(cx, cy, r + 3.5 * Math.max(1, u * 1.6), 0, 2 * Math.PI); g.lineWidth = Math.max(1.5, 3 * u); g.strokeStyle = css(p.led, 0.85); g.stroke(); }
  }

  // Parts.swift PanelButton: lit or held, the face lights (hi); the legend takes the LED's colour, and glows when lit;
  // PLAY has its green triangle, REC its red dot
  drawButton(g, p, b, held, lv, green) {
    const lit = ledLit(lv), tint = lit ? 1 : ledAlpha(lv) * 0.5, dy = held ? 1 : 0;
    cap(g, b.x, b.y + dy, b.w, b.h, b.r, css(lit || held ? p.hi : p.key), css(p.hi), css(p.lo), b.drop - dy);
    const name = (this.names[b.b] || PANEL_LABELS[b.b]).toUpperCase().replace("OCT-", "OCT−");
    const hint = this.hints ? BTN_HINT[b.b] : "";
    const ic = b.b === B.PLAY || b.b === B.REC ? Math.max(7, Math.round(b.h * 0.17)) : 0;
    const hs = hint ? Math.max(7, b.font * 0.62) : 0;
    const total = ic + (ic ? 3 : 0) + b.font + (hint ? 2 + hs : 0);
    let y = b.y + dy + 2 + (b.h - 2 - total) / 2;
    const cx = b.x + b.w / 2;
    if (b.b === B.PLAY) {
      const on = ledLit(green);
      if (on) { g.shadowColor = css(p.play); g.shadowBlur = 4; }
      g.beginPath(); g.moveTo(cx - ic * 0.3, y + ic * 0.1); g.lineTo(cx + ic * 0.4, y + ic * 0.5); g.lineTo(cx - ic * 0.3, y + ic * 0.9); g.closePath();
      g.fillStyle = css(p.play, on || !green ? 1 : 0.6 + 0.4 * ledAlpha(green)); g.fill();
      g.shadowBlur = 0; g.shadowColor = "transparent";
    } else if (b.b === B.REC) circle(g, cx, y + ic / 2, ic * 0.4, css(p.rec));
    if (ic) y += ic + 3;
    legend(g, name, cx, y + b.font / 2, b.font, tint > 0 ? mixCss(p.keyText, p.led, tint) : css(p.keyText), b.w - 6, 0.06, lit ? css(p.led) : null);
    if (hint) legend(g, hint, cx, y + b.font + 2 + hs / 2, hs, css(p.led, 0.9), b.w - 4, 0.02);
  }

  drawKeybed(g, p) {
    const L = this.L, kb = L.keybed, kg = L.kg, ctl = this.controls, off = this.keyOff();
    g.save();
    g.translate(kb.x, kb.y);
    if (kg.scroll) {
      // the range strip: one cell per natural, its LED; the frame shows the part in view
      const first = Math.floor(off);
      for (let i = 0; i < 16; i++) {
        const c = kg.stripCell(i), lv = this.keyLevel(NATURALS[i]), inView = i >= first && i < off + kg.visible;
        fillRR(g, c.x, c.y, c.w, c.h, 4, css(inView ? p.hi : p.key));
        const a = ledAlpha(lv);
        if (ledLit(lv)) { g.shadowColor = css(p.led); g.shadowBlur = 3; }
        fillRR(g, c.x + c.w * 0.225, c.y + c.h - 7, c.w * 0.55, 3, 1.5, a > 0 ? css(p.led, a) : css(p.ledOff));
        g.shadowBlur = 0; g.shadowColor = "transparent";
      }
      const w = kg.stripWindow(off);
      rr(g, w.x, w.y, w.w, w.h, 7); g.lineWidth = 2; g.strokeStyle = css(p.keyText); g.stroke();
      g.beginPath(); g.rect(kg.stage.x, kg.stage.y - 2, kg.stage.w, kg.stage.h + 6); g.clip();
      g.translate(kg.stage.x + kg.shift(off), kg.stage.y);
    }
    for (const k of kg.keys) {
      const held = ctl.keyHeld(k.k), lv = this.keyLevel(k.k), a = ledAlpha(lv), lit = ledLit(lv), dy = held ? 1.5 : 0;
      cap(g, k.x, k.y + dy, k.w, k.h, k.r, css(held ? p.hi : p.key), css(p.hi), css(p.lo), 4 - dy);
      const hint = this.hints ? KEY_HINT[k.k] : "";
      const hs = hint ? Math.max(7, Math.min(12, k.w * 0.24)) : 0;
      const lx = k.x + (k.w - k.lw) / 2, ly = k.y + dy + 2 + (k.h - 2 - k.lh - (hint ? hs + 4 : 0)) / 2;
      fillRR(g, lx, ly, k.lw, k.lh, 3, css(p.ledOff));
      if (a > 0) {
        if (lit) { g.shadowColor = css(p.led); g.shadowBlur = 6; }
        fillRR(g, lx, ly, k.lw, k.lh, 3, css(p.led, a));
        if (lit) { g.shadowBlur = 2; fillRR(g, lx, ly, k.lw, k.lh, 3, css(p.led, a)); }
        g.shadowBlur = 0; g.shadowColor = "transparent";
      }
      if (hint) legend(g, hint, k.x + k.w / 2, ly + k.lh + 4 + hs / 2, hs, css(p.keyText, 0.85), k.w - 2, 0.02);
      if (ctl.keyLatched(k.k)) this.drawLatch(g, { x: k.x, y: k.y, w: k.w, h: k.h, r: k.r });
    }
    g.restore();
  }

  // a latch's mark, in the site's accent: a ring just outside the cap and a dot at its top right corner
  drawLatch(g, r) {
    const o = 3;
    rr(g, r.x - o, r.y - o, r.w + 2 * o, r.h + 2 * o + 4, (r.r || 6) + o);
    g.strokeStyle = css(this.accent); g.lineWidth = 2; g.stroke();
    circle(g, r.x + r.w - 1, r.y + 1, 5, css(this.pal.body));
    circle(g, r.x + r.w - 1, r.y + 1, 3.6, css(this.accent));
  }
}

// ============================================================ screen view ===
// The Screen view (the app's ScreenOnlyView): the display alone, as big as fits, on the faceplate's colour
export class ScreenView {
  constructor(canvas, lcd) {
    this.canvas = canvas; this.ctx = canvas.getContext("2d"); this.lcd = lcd;
    this.pal = resolve(PRESETS.find((p) => p.name === DEFAULT_THEME));
    this.dpr = 1; this.cssW = 0; this.cssH = 0;
  }
  setTheme(t) { this.pal = resolve(t); }
  layout(w, h, dpr) {
    const W = Math.max(1, Math.round(w * dpr)), H = Math.max(1, Math.round(h * dpr));
    if (this.canvas.width !== W || this.canvas.height !== H) { this.canvas.width = W; this.canvas.height = H; }
    this.dpr = dpr; this.cssW = w; this.cssH = h;
  }
  bezel() {   // CSS pixels
    const m = Math.min(this.cssW, this.cssH), size = Math.max(60, m - 2 * Math.max(16, m * 0.06));
    return { x: (this.cssW - size) / 2, y: (this.cssH - size) / 2, w: size, h: size };
  }
  anchor(name) {
    const b = this.bezel();
    if (name === "lcd") return { x: b.x + b.w / 2, y: b.y + b.h / 2 };
    return { x: this.cssW / 2, y: Math.max(4, b.y / 2), room: b.y };
  }
  draw() {
    const g = this.ctx, p = this.pal, d = this.dpr;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.fillStyle = css(p.body); g.fillRect(0, 0, this.canvas.width, this.canvas.height);
    g.setTransform(d, 0, 0, d, 0, 0);
    const b = this.bezel();
    drawDisplay(g, p, null, b.x, b.y, b.w, d, true);
    drawDisplay(g, p, this.lcd, b.x, b.y, b.w, d, false);
  }
}
