// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 drawn on a canvas, and its input: FM1VST plugin/PanelComponent.cpp ported (itself ChoralRoot's
// tools/emu/emu.c: the geometry block, the 5x7 font, the drawing of each control, the computer-key map). Units are
// the drawing's (904 x 566, the VIEW 876 x 538 of it shown); the canvas maps them to device pixels.
//
// Orders. The panel draws buttons by LABEL (FM1VST EMU_B_*: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT-
// OCT+) and knobs by ROLE (EMU_E_*: SELECT ALGORITHM PRESETS KNOB1..4 MASTER). The engine takes MATRIX order
// (web/engine/API.md, RemoteFM1 proto/rfm1.h): buttons 0 OCT- 1 OCT+ 2 FX .. 13 REC; encoders 0 SELECT 1 ALGO
// 2..5 KNOB1..4 6 PRESETS. labelToMatrix / roleToMatrix convert (FM1VST core-glue/common/panel.h PANEL_DEFAULT).

import { css, derive, PRESETS, DEFAULT_THEME } from "./theme.js";

// ================================================================ geometry ===
const WHITE_X = [71, 119, 167, 215, 264, 312, 360, 408, 456, 504, 553, 601, 649, 697, 745, 794];
const BLACK_X = [95, 143, 191, 288, 336, 432, 480, 529, 625, 673, 769];
const KEY_W = 42, KEY_H = 76;
const BTN_X = [500, 554, 608, 661, 715, 768], BTN_Y = [177, 231], BTN_S = 36;
export const B = { FX: 0, SEL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11, OCTDN: 12, OCTUP: 13 };
export const E = { SELECT: 0, ALGO: 1, PRESETS: 2, K1: 3, K2: 4, K3: 5, K4: 6, MASTER: 7 };
export const NKEY = 27, NB = 14, NE = 8;
const ENC_CX = [186.5, 186.5, 95, 508, 604, 701, 797, 95];
const ENC_CY = [101, 191, 191, 101, 101, 101, 101, 101];
const ENC_LY = [66, 156, 156, 66, 66, 66, 66, 66];
const ENC_R = 21, ENC_CAP = 12;
const BODY = { x: 22, y: 22, w: 860, h: 522 }, SCREEN = { x: 247, y: 59, w: 201, h: 201 }, SCREEN_A = { x: 272, y: 84, w: 150, h: 150 };
export const VIEW = { x: 14, y: 14, w: 876, h: 538 };
const ENC_NAME = ["SELECT", "ALGORITHM", "PRESETS", "KNOB1", "KNOB2", "KNOB3", "KNOB4", "MASTER"];
export const PANEL_LABELS = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
const LCD = 240;

export const keyBlack = (k) => (0x54A >> ((k + 5) % 12)) & 1;
const KEYS = [], BTNS = [];
{
  let w = 0, b = 0;
  for (let k = 0; k < NKEY; k++) {
    const bl = keyBlack(k);
    KEYS.push({ x: bl ? BLACK_X[b++] : WHITE_X[w++], y: bl ? 326 : 408, w: KEY_W, h: KEY_H });
  }
  for (let i = 0; i < 12; i++) BTNS.push({ x: BTN_X[i % 6], y: BTN_Y[(i / 6) | 0], w: BTN_S, h: BTN_S });
  BTNS.push({ x: 87, y: 250, w: 42, h: 22 }, { x: 153, y: 250, w: 42, h: 22 });
}
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
// own FM-1 (web/remote). Every change calls onChange(msg) with the engine's panel in matrix order; enc holds the
// detents since the previous message.
export const SRC = { KEY: 1, ESC: 2, POINTER: 4, LATCH: 8, MIDI: 16, REMOTE: 32 };
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

// ================================================================ the 5x7 font ===
// emu.c's 5x7 font, ASCII 32..95, columns, bit 0 = top
const FONT = [
  [0x00,0x00,0x00,0x00,0x00],[0x00,0x00,0x5F,0x00,0x00],[0x00,0x07,0x00,0x07,0x00],[0x14,0x7F,0x14,0x7F,0x14],
  [0x24,0x2A,0x7F,0x2A,0x12],[0x23,0x13,0x08,0x64,0x62],[0x36,0x49,0x55,0x22,0x50],[0x00,0x05,0x03,0x00,0x00],
  [0x00,0x1C,0x22,0x41,0x00],[0x00,0x41,0x22,0x1C,0x00],[0x08,0x2A,0x1C,0x2A,0x08],[0x08,0x08,0x3E,0x08,0x08],
  [0x00,0x50,0x30,0x00,0x00],[0x08,0x08,0x08,0x08,0x08],[0x00,0x60,0x60,0x00,0x00],[0x20,0x10,0x08,0x04,0x02],
  [0x3E,0x51,0x49,0x45,0x3E],[0x00,0x42,0x7F,0x40,0x00],[0x42,0x61,0x51,0x49,0x46],[0x21,0x41,0x45,0x4B,0x31],
  [0x18,0x14,0x12,0x7F,0x10],[0x27,0x45,0x45,0x45,0x39],[0x3C,0x4A,0x49,0x49,0x30],[0x01,0x71,0x09,0x05,0x03],
  [0x36,0x49,0x49,0x49,0x36],[0x06,0x49,0x49,0x29,0x1E],[0x00,0x36,0x36,0x00,0x00],[0x00,0x56,0x36,0x00,0x00],
  [0x08,0x14,0x22,0x41,0x00],[0x14,0x14,0x14,0x14,0x14],[0x00,0x41,0x22,0x14,0x08],[0x02,0x01,0x51,0x09,0x06],
  [0x32,0x49,0x79,0x41,0x3E],[0x7E,0x11,0x11,0x11,0x7E],[0x7F,0x49,0x49,0x49,0x36],[0x3E,0x41,0x41,0x41,0x22],
  [0x7F,0x41,0x41,0x22,0x1C],[0x7F,0x49,0x49,0x49,0x41],[0x7F,0x09,0x09,0x01,0x01],[0x3E,0x41,0x41,0x51,0x32],
  [0x7F,0x08,0x08,0x08,0x7F],[0x00,0x41,0x7F,0x41,0x00],[0x20,0x40,0x41,0x3F,0x01],[0x7F,0x08,0x14,0x22,0x41],
  [0x7F,0x40,0x40,0x40,0x40],[0x7F,0x02,0x04,0x02,0x7F],[0x7F,0x04,0x08,0x10,0x7F],[0x3E,0x41,0x41,0x41,0x3E],
  [0x7F,0x09,0x09,0x09,0x06],[0x3E,0x41,0x51,0x21,0x5E],[0x7F,0x09,0x19,0x29,0x46],[0x46,0x49,0x49,0x49,0x31],
  [0x01,0x01,0x7F,0x01,0x01],[0x3F,0x40,0x40,0x40,0x3F],[0x1F,0x20,0x40,0x20,0x1F],[0x7F,0x20,0x18,0x20,0x7F],
  [0x63,0x14,0x08,0x14,0x63],[0x03,0x04,0x78,0x04,0x03],[0x61,0x51,0x49,0x45,0x43],[0x00,0x7F,0x41,0x41,0x00],
  [0x02,0x04,0x08,0x10,0x20],[0x00,0x41,0x41,0x7F,0x00],[0x04,0x02,0x01,0x02,0x04],[0x40,0x40,0x40,0x40,0x40],
];
const textWidth = (s, size) => (s.length * 6 - 1) * size / 7;
const fitSize = (s, size, room) => { const w = textWidth(s, size); return w > room ? size * room / w : size; };

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

// ========================================================= drawing helpers ===
function rr(ctx, x, y, w, h, r) {
  r = Math.max(0, Math.min(r, w / 2, h / 2));
  ctx.beginPath();
  ctx.roundRect(x, y, w, h, r);
}
function fillRR(ctx, r, rad, c, a = 1) { rr(ctx, r.x, r.y, r.w, r.h, rad); ctx.fillStyle = css(c, a); ctx.fill(); }
// a line lw units wide inside the edge (at least 1 device pixel: px = units per pixel)
function lineRR(ctx, r, rad, c, a, lw, px) {
  const w = Math.max(lw, px);
  rr(ctx, r.x + w / 2, r.y + w / 2, r.w - w, r.h - w, Math.max(0, rad - w / 2));
  ctx.strokeStyle = css(c, a); ctx.lineWidth = w; ctx.stroke();
}
function circle(ctx, cx, cy, r, c, a = 1) { ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.fillStyle = css(c, a); ctx.fill(); }
function ring(ctx, cx, cy, r, c, a, lw, dash) {
  ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI);
  ctx.strokeStyle = css(c, a); ctx.lineWidth = lw;
  ctx.setLineDash(dash > 0 ? [dash, dash * 1.1] : []);
  ctx.stroke(); ctx.setLineDash([]);
}
function segment(ctx, x1, y1, x2, y2, lw, c) {
  ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2);
  ctx.strokeStyle = css(c); ctx.lineWidth = lw; ctx.lineCap = "round"; ctx.stroke(); ctx.lineCap = "butt";
}
// the 5x7 font, centred on cx, cap height `size` units, top at y; one path per string (no seams)
function text(ctx, cx, y, size, s, c, a = 1) {
  s = String(s);
  const f = size / 7, x0 = cx - textWidth(s, size) / 2;
  ctx.beginPath();
  for (let i = 0; i < s.length; i++) {
    let ch = s.charCodeAt(i);
    if (ch >= 97 && ch <= 122) ch -= 32;
    if (ch < 32 || ch > 95) continue;
    const g = FONT[ch - 32];
    for (let col = 0; col < 5; col++) for (let row = 0; row < 7; row++)
      if ((g[col] >> row) & 1) ctx.rect(x0 + (i * 6 + col) * f, y + row * f, f, f);
  }
  ctx.fillStyle = css(c, a);
  ctx.fill();
}

// ============================================================ device view ===
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
    this.k = 1; this.ox = 0; this.oy = 0;
    this.base = null;
    this.hints = false;
  }
  setTheme(t) { this.theme = t; this.pal = derive(t); this.base = null; }
  // the computer-key hints (the green letters: the sheet's "Show computer-key hints", off by default); off, the keys'
  // LED and note name and the buttons' names sit centred in the space the letters took
  setHints(on) { this.hints = !!on; }
  setButtonNames(n) { this.names = (n && n.length === NB ? n : PANEL_LABELS).map((s) => String(s || "")); this.base = null; }
  setKeymap(km) { this.ledIdx = ledIndexOf(km); }
  setLeds(levels, playGreen) { this.levels = levels; this.playGreen = playGreen | 0; }
  keyLevel(k) { const i = this.ledIdx[14 + k]; return i >= 0 ? this.levels[i] : 0; }
  btnLevel(b) { const i = this.ledIdx[labelToMatrix(b)]; return i >= 0 ? this.levels[i] : 0; }

  // fit VIEW into the canvas's CSS box (w x h) at dpr; snap to an integer scale of the LCD when one is within 8 %
  layout(w, h, dpr) {
    const W = Math.max(1, Math.round(w * dpr)), H = Math.max(1, Math.round(h * dpr));
    if (this.canvas.width !== W || this.canvas.height !== H) { this.canvas.width = W; this.canvas.height = H; }
    let k = Math.min(W / VIEW.w, H / VIEW.h);
    const n = Math.floor(k * SCREEN_A.w / LCD), ks = n * LCD / SCREEN_A.w;
    if (n >= 1 && ks >= 0.92 * k) k = ks;
    this.k = k; this.dpr = dpr; this.cssW = w; this.cssH = h;
    this.ox = Math.floor((W - VIEW.w * k) / 2); this.oy = Math.floor((H - VIEW.h * k) / 2);
    this.base = null;
  }
  // units <-> CSS pixels of the canvas
  toUnits(cx, cy) { return { x: (cx * this.dpr - this.ox) / this.k + VIEW.x, y: (cy * this.dpr - this.oy) / this.k + VIEW.y }; }
  toCss(ux, uy) { return { x: ((ux - VIEW.x) * this.k + this.ox) / this.dpr, y: ((uy - VIEW.y) * this.k + this.oy) / this.dpr }; }
  unitPx() { return this.k / this.dpr; }   // CSS pixels per unit

  hit(ux, uy) {
    for (let i = 0; i < NKEY; i++) { const r = KEYS[i]; if (ux >= r.x && ux < r.x + r.w && uy >= r.y && uy < r.y + r.h) return { kind: KM.KEY, idx: i }; }
    for (let i = 0; i < NB; i++) { const r = BTNS[i]; if (ux >= r.x && ux < r.x + r.w && uy >= r.y && uy < r.y + r.h) return { kind: KM.BTN, idx: i }; }
    for (let i = 0; i < NE; i++) { const dx = ux - ENC_CX[i], dy = uy - ENC_CY[i]; if (dx * dx + dy * dy <= (ENC_R + 5) ** 2) return { kind: KM.SELECT, idx: i }; }
    if (ux >= SCREEN.x && ux < SCREEN.x + SCREEN.w && uy >= SCREEN.y && uy < SCREEN.y + SCREEN.h) return { kind: "screen" };
    return null;
  }

  unitsTransform(ctx) { ctx.setTransform(this.k, 0, 0, this.k, this.ox - VIEW.x * this.k, this.oy - VIEW.y * this.k); }

  drawBase() {
    const W = this.canvas.width, H = this.canvas.height;
    const c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(W, H) : Object.assign(document.createElement("canvas"), { width: W, height: H });
    const g = c.getContext("2d"), p = this.pal, px = 1 / this.k;
    g.fillStyle = css(p.backdrop); g.fillRect(0, 0, W, H);
    this.unitsTransform(g);
    fillRR(g, BODY, 38, p.plate);
    lineRR(g, BODY, 38, p.outline, 1, 1.5, px);
    lineRR(g, { x: BODY.x + 7, y: BODY.y + 7, w: BODY.w - 14, h: BODY.h - 14 }, 32, p.edge, 1, 1, px);
    for (const b of [{ x: 52, y: 306, w: 800, h: 198 }, { x: 484, y: 162, w: 338, h: 124 }, { x: 77, y: 243, w: 127, h: 36 }]) {
      const rad = b.w > 700 ? 20 : b.w > 300 ? 8 : 6;
      fillRR(g, b, rad, p.bed);
      lineRR(g, b, rad, p.bedEdge, 1, 1, px);
    }
    for (const dx of [262, 406, 599, 743]) circle(g, dx, 404, 2, p.bedEdge);
    for (let i = 0; i < NE; i++) text(g, ENC_CX[i], ENC_LY[i] - 6, 7, ENC_NAME[i], p.label);
    for (let i = 0; i < 12; i++) {   // the printed labels where the firmware names a button differently
      const r = BTNS[i], lbl = PANEL_LABELS[i];
      if (this.names[i].toUpperCase() === lbl) continue;
      text(g, r.x + r.w / 2, i < 6 ? r.y - 10.5 : r.y + r.h + 4.5, 5.5, lbl, p.bedLabel, 0.85);
    }
    fillRR(g, SCREEN, 10, p.screenBezel);
    lineRR(g, SCREEN, 10, p.outline, 1, 1, px);
    this.base = c;
  }

  draw() {
    if (!this.base) this.drawBase();
    const g = this.ctx, p = this.pal, ctl = this.controls, px = 1 / this.k;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.drawImage(this.base, 0, 0);
    this.unitsTransform(g);
    for (let k = 0; k < NKEY; k++) this.drawKey(g, p, k, ctl.keyHeld(k), this.keyLevel(k), px);
    for (let b = 0; b < NB; b++) this.drawButton(g, p, b, ctl.btnHeld(b), this.btnLevel(b), b === B.PLAY ? this.playGreen : 0, px);
    for (let i = 0; i < NE; i++) this.drawKnob(g, p, i, ctl.angle[i], ctl.selKnob === i);
    // the LCD, nearest-neighbour (exactly n x 240 device pixels when the scale snapped)
    g.setTransform(1, 0, 0, 1, 0, 0);
    const x0 = Math.round((SCREEN_A.x - VIEW.x) * this.k + this.ox), y0 = Math.round((SCREEN_A.y - VIEW.y) * this.k + this.oy);
    const s = Math.round(SCREEN_A.w * this.k);
    g.imageSmoothingEnabled = s % LCD !== 0 && s < LCD;
    g.drawImage(this.lcd.canvas, x0, y0, s, s);
    g.imageSmoothingEnabled = true;
  }

  drawKey(g, p, k, held, lv, px) {
    const r = KEYS[k], cx = r.x + KEY_W / 2;
    fillRR(g, r, 21, held ? p.pressed : p.cap);
    lineRR(g, r, 21, held ? p.hint : p.capOutline, 1, held ? 1.5 : 1, px);
    const hint = this.hints && KEY_HINT[k], y = r.y + (hint ? 0 : 3);   // (no hint: the LED and the name 3 units lower, centred)
    if (ledLit(lv)) fillRR(g, { x: cx - 9, y: y + 9, w: 18, h: 40 }, 9, p.white, 0.16 * ledAlpha(lv));
    const slot = { x: cx - 4, y: y + 14, w: 8, h: 30 };
    fillRR(g, slot, 4, p.ledOff);
    if (lv) fillRR(g, slot, 4, p.white, ledAlpha(lv));
    text(g, cx, y + 50, 6.5, noteName(k), p.noteText);
    if (hint) text(g, cx, r.y + 61, 6.5, hint, p.hint, 0.9);
  }

  drawButton(g, p, i, held, lv, green, px) {
    const r = BTNS[i], oct = i >= B.OCTDN;
    const col = i === B.REC ? p.red : i === B.PLAY ? p.orange : p.white;
    const rad = oct ? 6 : 5;
    fillRR(g, r, rad, p.cap);
    if (lv) fillRR(g, r, rad, col, ledAlpha(lv));
    lineRR(g, r, rad, held ? p.hint : p.capOutline, 1, held ? 2 : 1, px);
    const n = (this.names[i] || PANEL_LABELS[i]).toUpperCase();
    const base = oct ? 6.5 : 7.5, size = fitSize(n, base, r.w - 5);
    const h = this.hints ? BTN_HINT[i] : "";
    const ny = oct ? 7.5 : h ? 11 : (r.h - base) / 2;            // (no hint: the name centred on the cap)
    text(g, r.x + r.w / 2, r.y + ny + (base - size) / 2, size, n, ledLit(lv) ? p.litText : lv ? p.capTextDim : p.capText);
    if (h && !oct) text(g, r.x + r.w / 2, r.y + 25, 5, h, ledLit(lv) ? p.hintLit : lv ? p.hintDim : p.hint, 0.9);
    else if (h) text(g, r.x + r.w / 2, r.y + r.h + 4, 5, h, p.hintBed, 0.9);
    if (i === B.PLAY) {
      circle(g, r.x + 6, r.y + r.h - 6, 3.2, p.ledOff);
      if (green) circle(g, r.x + 6, r.y + r.h - 6, 3.2, p.green, ledAlpha(green));
    }
  }

  drawKnob(g, p, i, a, sel) {
    const cx = ENC_CX[i], cy = ENC_CY[i];
    circle(g, cx, cy, ENC_R, p.knobBody);
    ring(g, cx, cy, ENC_R - 3, p.knobRing, 1, 2.5, 2);
    circle(g, cx, cy, ENC_CAP, p.knobCap);
    segment(g, cx + 4 * Math.sin(a), cy - 4 * Math.cos(a), cx + (ENC_CAP - 1) * Math.sin(a), cy - (ENC_CAP - 1) * Math.cos(a), 2,
            i === E.MASTER ? p.masterPointer : p.pointer);
    if (sel) ring(g, cx, cy, ENC_R + 3, p.hint, 1, 1.5, 0);
  }
}

// ============================================================ screen view ===
// FM1VST ScreenView: the body's colour around a black bezel, the LCD pixel for pixel when a whole multiple fits
export class ScreenView {
  constructor(canvas, lcd) {
    this.canvas = canvas; this.ctx = canvas.getContext("2d"); this.lcd = lcd;
    this.pal = derive(PRESETS.find((p) => p.name === DEFAULT_THEME));
  }
  setTheme(t) { this.pal = derive(t); }
  layout(w, h, dpr) {
    const W = Math.max(1, Math.round(w * dpr)), H = Math.max(1, Math.round(h * dpr));
    if (this.canvas.width !== W || this.canvas.height !== H) { this.canvas.width = W; this.canvas.height = H; }
    this.dpr = dpr; this.cssW = w; this.cssH = h;
  }
  bezel() {   // CSS pixels
    const w = Math.min(this.cssW, this.cssH), inset = Math.max(6, w * 0.028);
    return { x: (this.cssW - w) / 2 + inset, y: (this.cssH - w) / 2 + inset, w: w - 2 * inset, h: w - 2 * inset };
  }
  draw() {
    const g = this.ctx, p = this.pal, d = this.dpr, W = this.canvas.width, H = this.canvas.height;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.fillStyle = css(p.plate); g.fillRect(0, 0, W, H);
    g.setTransform(d, 0, 0, d, 0, 0);
    const bz = this.bezel(), rad = Math.max(8, bz.w * 0.035);
    rr(g, 1.5, 1.5, this.cssW - 3, this.cssH - 3, rad + 3); g.strokeStyle = css(p.edge); g.lineWidth = 1; g.stroke();
    g.save();
    g.shadowColor = "rgba(0,0,0,0.35)"; g.shadowBlur = rad; g.shadowOffsetY = 2;
    rr(g, bz.x, bz.y, bz.w, bz.h, rad); g.fillStyle = css(p.screenBezel); g.fill();
    g.restore();
    rr(g, bz.x + 0.5, bz.y + 0.5, bz.w - 1, bz.h - 1, rad); g.strokeStyle = css(p.outline); g.stroke();
    const w = Math.min(this.cssW, this.cssH), avail = bz.w - 2 * Math.max(8, w * 0.032);
    const phys = Math.floor(avail * d), n = Math.floor(phys / LCD);
    const size = n >= 1 && n * LCD >= 0.94 * phys ? n * LCD : phys;
    const cx = (bz.x + bz.w / 2) * d, cy = (bz.y + bz.h / 2) * d;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.imageSmoothingEnabled = size % LCD !== 0 && size < LCD;
    g.drawImage(this.lcd.canvas, Math.round(cx - size / 2), Math.round(cy - size / 2), size, size);
    g.imageSmoothingEnabled = true;
  }
}
