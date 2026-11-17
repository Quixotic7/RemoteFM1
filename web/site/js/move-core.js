// SPDX-License-Identifier: MPL-2.0
// rfm_move in the browser: the Ableton Move core (host/src/move/move.c, host/src/move/README.md has the mapping) as
// WebAssembly (web/move/build.sh -> web/build/move/rfm_move.mjs), wrapped for js/move-link.js. No DOM: Node runs it
// (web/site/test.mjs).
//   const core = await MoveCore.load(settingsBytes?)          (the module once per page, a core per call)
//   core.input(packet, frame, nowMs) -> actions                packet: the INPUT record's 4 bytes as a LE uint32
//   core.panel(levels44, playGreen); core.leds() -> [{ rgb, cc, number, r, g, b }]   (only the changed ones)
//   core.screen(rgb565 | null) -> Uint8Array(1024)             (the Move's packed 128 x 64)
//   core.save() -> Uint8Array(77); core.load(bytes)             (load: a fresh core with those settings)
//   core.setLabel(enc, text); core.setMaster(v); core.setKeymap(km | null); core.setMode(m) / releaseAll() -> actions
// Actions (move.h rfm_move_act_t, matrix order):
//   { kind: "key", k, down, vel }  { kind: "press", k, value }  { kind: "btn", m, down }  { kind: "turn", enc, steps }
//   { kind: "master", value }  { kind: "midi", bytes }  { kind: "settings" }
export const ACT = { KEY: 1, PRESS: 2, BTN: 3, TURN: 4, MASTER: 5, MIDI: 6, SETTINGS: 7 };
export const MODE = { FM1: 0, MIDI: 1, CONTROL: 2, SEQ: 3 };
export const MODE_NAMES = ["FM-1", "MIDI", "Control", "Sequencer"];
export const SCREEN_BYTES = 1024, SETTINGS_BYTES = 77, NLEDS = 76;
const ACT_BYTES = 10, LED_BYTES = 6;
// pressure 0..127 as key_lean 0..100 (move.h rfm_move_lean)
export const lean = (p) => Math.floor((Math.max(0, Math.min(127, p | 0)) * 100 + 63) / 127);

let modP = null;
// the module (once): url the built rfm_move.mjs (default: web/build/move/ beside the site)
export function loadModule(url) {
  if (!modP) {
    const u = url || new URL("../../build/move/rfm_move.mjs", import.meta.url).href;
    modP = import(/* @vite-ignore */ u).then((m) => (m.default || m.createRfmMove)()).catch((e) => { modP = null; throw e; });
  }
  return modP;
}

export class MoveCore {
  static async load(settings, url) { return new MoveCore(await loadModule(url), settings); }
  constructor(M, settings) {
    this.M = M;
    this.w = 0;
    this.load(settings);
  }
  // a fresh core with these settings (an rfm_move_save image; anything else: the defaults)
  load(settings) {
    const M = this.M;
    if (this.w) M._mw_free(this.w);
    let p = 0, n = 0;
    if (settings && settings.length) { n = settings.length; p = M._malloc(n); M.HEAPU8.set(settings, p); }
    this.w = M._mw_new(p, n);
    if (p) M._free(p);
    if (!this.w) throw new Error("rfm_move: out of memory");
    if (!this.px) this.px = M._malloc(240 * 240 * 2);
    if (!this.lv) this.lv = M._malloc(44);
    if (!this.km) this.km = M._malloc(66);
    if (!this.st) this.st = M._malloc(32);
    return this;
  }
  _acts(n) {
    const M = this.M, base = M._mw_acts(this.w), out = [];
    for (let i = 0; i < n; i++) {
      const o = base + i * ACT_BYTES, b = M.HEAPU8, kind = b[o], index = b[o + 1], down = !!b[o + 2], vel = b[o + 3];
      const value = (b[o + 4] | (b[o + 5] << 8)) << 16 >> 16;
      if (kind === ACT.KEY) out.push({ kind: "key", k: index, down, vel });
      else if (kind === ACT.PRESS) out.push({ kind: "press", k: index, value });
      else if (kind === ACT.BTN) out.push({ kind: "btn", m: index, down });
      else if (kind === ACT.TURN) out.push({ kind: "turn", enc: index, steps: value });
      else if (kind === ACT.MASTER) out.push({ kind: "master", value });
      else if (kind === ACT.MIDI) out.push({ kind: "midi", bytes: b.slice(o + 7, o + 7 + Math.min(3, b[o + 6])) });
      else if (kind === ACT.SETTINGS) out.push({ kind: "settings" });
    }
    return out;
  }
  input(packet, frame = 0, nowMs = 0) { return this._acts(this.M._mw_input(this.w, packet >>> 0, frame >>> 0, nowMs >>> 0)); }
  releaseAll() { return this._acts(this.M._mw_release_all(this.w)); }
  setMode(mode) { return this._acts(this.M._mw_set_mode(this.w, mode | 0)); }
  setScale(scale, root) { return this._acts(this.M._mw_set_scale(this.w, scale | 0, root | 0)); }
  setMaster(v) { this.M._mw_set_master(this.w, v | 0); }
  get master() { return this.M._mw_master(this.w); }
  // enc: matrix encoder 0..6, 7 MASTER; text "" or null: the default name
  setLabel(enc, text) {
    const M = this.M, s = String(text || "").slice(0, 16), n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n);
    M.stringToUTF8(s, p, n);
    M._mw_set_label(this.w, enc | 0, p);
    M._free(p);
  }
  // km: the panel's wiring (6 rows x 11, id or -1: engine info().keymap), or null for the FM-1's
  setKeymap(km) {
    if (!Array.isArray(km) || km.length !== 6) { this.M._mw_set_keymap(this.w, 0); return; }
    const b = new Int8Array(66);
    for (let r = 0; r < 6; r++) for (let c = 0; c < 11; c++) { const v = km[r] && km[r][c]; b[r * 11 + c] = Number.isInteger(v) ? v : -1; }
    this.M.HEAPU8.set(new Uint8Array(b.buffer), this.km);
    this.M._mw_set_keymap(this.w, this.km);
  }
  panel(levels, playGreen = 0) {
    const b = new Uint8Array(44);
    if (levels) b.set(levels.subarray ? levels.subarray(0, 44) : levels.slice(0, 44));
    this.M.HEAPU8.set(b, this.lv);
    this.M._mw_panel(this.w, this.lv, playGreen | 0);
  }
  leds() {
    const M = this.M, n = M._mw_leds(this.w), base = M._mw_leds_buf(this.w), out = [];
    for (let i = 0; i < n; i++) {
      const o = base + i * LED_BYTES, b = M.HEAPU8;
      out.push({ rgb: b[o], cc: b[o + 1], number: b[o + 2], r: b[o + 3], g: b[o + 4], b: b[o + 5] });
    }
    return out;
  }
  ledsInvalidate() { this.M._mw_leds_invalidate(this.w); }
  screen(rgb565) {
    const M = this.M;
    let p = 0;
    if (rgb565 && rgb565.length >= 240 * 240) { new Uint16Array(M.HEAPU8.buffer, this.px, 240 * 240).set(rgb565.subarray(0, 240 * 240)); p = this.px; }
    const o = M._mw_screen(this.w, p);
    return M.HEAPU8.slice(o, o + SCREEN_BYTES);
  }
  save() { const o = this.M._mw_save(this.w); return this.M.HEAPU8.slice(o, o + SETTINGS_BYTES); }
  state() {
    this.M._mw_state(this.w, this.st);
    const s = new Int32Array(this.M.HEAPU8.buffer, this.st, 8);
    return { mode: s[0], layout: s[1], channel: s[2], octave: s[3], scale: s[4], root: s[5], bank: s[6], trackHeld: s[7] };
  }
  padNote(pad) { return this.M._mw_pad_note(this.w, pad | 0); }
  // the FM-1 mode's mapping (tests, help)
  padKey(pad) { return this.M._mw_pad_key(pad | 0); }
  stepKey(step) { return this.M._mw_step_key(step | 0); }
  buttonOf(cc) { return this.M._mw_button_of(cc | 0); }
  knobEnc(knob) { return this.M._mw_knob_enc(knob | 0); }
  decode(packet) {
    const p = this.st; this.M._mw_decode(packet >>> 0, p);
    const s = new Int32Array(this.M.HEAPU8.buffer, p, 3);
    return { type: s[0], index: s[1], value: s[2] };
  }
  free() { const M = this.M; if (this.w) M._mw_free(this.w); for (const k of ["px", "lv", "km", "st"]) if (this[k]) { M._free(this[k]); this[k] = 0; } this.w = 0; }
}

// a USB-MIDI packet (the surface's, cable 0) as INPUT carries it, as a LE uint32: bytes cin|status|d1|d2
export function packet(status, d1, d2, cable = 0) {
  const cin = (status >> 4) & 15;
  return ((cable << 4 | cin) | (status & 255) << 8 | (d1 & 127) << 16 | (d2 & 127) << 24) >>> 0;
}
