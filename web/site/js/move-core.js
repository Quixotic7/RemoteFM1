// SPDX-License-Identifier: MPL-2.0
// rfm_move in the browser: the Ableton Move core (host/src/move/move.c, host/src/move/README.md has the mapping) as
// WebAssembly (web/move/build.sh -> web/build/move/rfm_move.mjs), wrapped for js/move-link.js. No DOM: Node runs it
// (web/site/test.mjs).
//   const core = await MoveCore.load(settingsBytes?)          (the module once per page, a core per call)
//   core.input(packet, frame, nowMs) -> actions                packet: the INPUT record's 4 bytes as a LE uint32
//   core.panel(levels44, playGreen); core.leds() -> [{ rgb, cc, number, r, g, b }]   (only the changed ones)
//   core.screen(rgb565 | null) -> Uint8Array(1024)             (the Move's packed 128 x 64)
//   core.save() -> Uint8Array(80); core.load(bytes)             (load: a fresh core with those settings)
//   core.setLabel(enc, text); core.setMaster(v); core.setKeymap(km | null); core.setMode(m) / releaseAll() -> actions
// Actions (move.h rfm_move_act_t, matrix order):
//   { kind: "key", k, down, vel }  { kind: "press", k, value }  { kind: "btn", m, down }  { kind: "turn", enc, steps }
//   { kind: "master", value }  { kind: "midi", bytes }  { kind: "settings" }
//   { kind: "projectSave", slot, bytes }  { kind: "projectLoad", slot }  { kind: "projectDelete", slot }
// and every action has `frame`: for seqAdvance's, the offset from the call's start it is due at (0 otherwise).
// The sequencer (host/src/move/README.md, "Sequencer"):
//   core.seqAdvance(frames, rate) -> actions (every block, playing or not); core.seqMidiIn(bytes) -> actions
//   core.seqPlay(on); core.setBpm(bpm10); core.setLead(us); core.setClock(mode); core.bpm10(); core.following(); core.playing()
//   actions also: { kind: "click", accent } (the metronome), { kind: "audioRec", on, bars } (Sample's loop), frame-stamped
//   core.projectLoad(slot, bytes | null) -> actions, or null (an invalid blob); core.projectDir(bits);
//   core.projectFlush() -> actions; core.projectSlot(); core.projectSave() -> the current blob; core.projectCheck(bytes)
//   core.projectInfo(slot, bytes): a stored project's content for the screen's project grid (0 ok)
export const ACT = { KEY: 1, PRESS: 2, BTN: 3, TURN: 4, MASTER: 5, MIDI: 6, SETTINGS: 7, PROJECT_SAVE: 8, PROJECT_LOAD: 9, PROJECT_DELETE: 10,
  CLICK: 11, AUDIO_REC: 12, PROJECT_CUT: 13, PROJECT_COPY: 14, PROJECT_PASTE: 15, PROJECT_COLOR: 16 };
export const CLOCK = { AUTO: 0, INT: 1, EXT: 2 };
export const PROJECTS = 32, PROJECT_MAX = 39072;   // (RFM_MOVE_PROJECT_MAX, version 3 blobs)
export const MODE = { FM1: 0, MIDI: 1, CONTROL: 2, SEQ: 3 };
export const MODE_NAMES = ["FM-1", "MIDI", "Control", "Sequencer"];
export const SCREEN_BYTES = 1024, SETTINGS_BYTES = 80, NLEDS = 92, POPUP_MS = 1000;   // (RFM_MOVE_POPUP_MS: a popup stays this long)
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
    const M = this.M, base = M._mw_acts(this.w), fr = M._mw_act_frames(this.w), out = [];
    for (let i = 0; i < n; i++) {
      const o = base + i * ACT_BYTES, b = M.HEAPU8, kind = b[o], index = b[o + 1], down = !!b[o + 2], vel = b[o + 3];
      const value = (b[o + 4] | (b[o + 5] << 8)) << 16 >> 16, len = out.length;
      if (kind === ACT.KEY) out.push({ kind: "key", k: index, down, vel });
      else if (kind === ACT.PRESS) out.push({ kind: "press", k: index, value });
      else if (kind === ACT.BTN) out.push({ kind: "btn", m: index, down });
      else if (kind === ACT.TURN) out.push({ kind: "turn", enc: index, steps: value });
      else if (kind === ACT.MASTER) out.push({ kind: "master", value });
      else if (kind === ACT.MIDI) out.push({ kind: "midi", bytes: b.slice(o + 7, o + 7 + Math.min(3, b[o + 6])) });
      else if (kind === ACT.SETTINGS) out.push({ kind: "settings" });
      else if (kind === ACT.PROJECT_SAVE) {
        const p = M._mw_blob(this.w, value), nb = M._mw_blob_len(this.w, value);
        if (p && nb) out.push({ kind: "projectSave", slot: index, bytes: M.HEAPU8.slice(p, p + nb) });
      } else if (kind === ACT.PROJECT_LOAD) out.push({ kind: "projectLoad", slot: index });
      else if (kind === ACT.PROJECT_DELETE) out.push({ kind: "projectDelete", slot: index });
      else if (kind === ACT.CLICK) out.push({ kind: "click", accent: !!value });          // the metronome
      else if (kind === ACT.AUDIO_REC) out.push({ kind: "audioRec", on: !!value, bars: index });   // Sample's loop
      else if (kind === ACT.PROJECT_CUT) out.push({ kind: "projectCut", slot: index });            // projects mode
      else if (kind === ACT.PROJECT_COPY) out.push({ kind: "projectCopy", slot: index });
      else if (kind === ACT.PROJECT_PASTE) out.push({ kind: "projectPaste", slot: index });
      else if (kind === ACT.PROJECT_COLOR) out.push({ kind: "projectColor", slot: index, color: value });
      if (out.length > len) out[len].frame = (b[fr + 4 * i] | (b[fr + 4 * i + 1] << 8) | (b[fr + 4 * i + 2] << 16) | (b[fr + 4 * i + 3] << 24)) >>> 0;
    }
    return out;
  }
  // a blob into the module's memory for one call: fn(ptr, n)
  _withBytes(bytes, fn) {
    const M = this.M;
    if (!bytes || !bytes.length) return fn(0, 0);
    const p = M._malloc(bytes.length);
    try { M.HEAPU8.set(bytes, p); return fn(p, bytes.length); } finally { M._free(p); }
  }
  // ---- the sequencer
  seqAdvance(frames, rate) { return this._acts(this.M._mw_seq_advance(this.w, frames >>> 0, rate >>> 0)); }
  seqMidiIn(bytes) { return this._withBytes(bytes, (p, n) => this._acts(this.M._mw_seq_midi_in(this.w, p, n))); }
  seqPlay(on) { return this._acts(this.M._mw_seq_play(this.w, on ? 1 : 0)); }
  setBpm(bpm10) { this.M._mw_seq_set_bpm(this.w, bpm10 | 0); }
  setLead(us) { this.M._mw_seq_set_lead(this.w, Math.max(0, us | 0)); }   // (the clock's lead over what sounds)
  bpm10() { return this.M._mw_seq_bpm10(this.w); }
  setClock(mode) { this.M._mw_seq_set_clock(this.w, mode | 0); }
  following() { return !!this.M._mw_seq_following(this.w); }
  playing() { return !!this.M._mw_seq_playing(this.w); }
  screenTimed() { return !!this.M._mw_screen_timed(this.w); }   // the screen moves with the clock (a popup, playing)
  // ---- projects
  projectCheck(bytes) { return bytes && bytes.length && bytes.length <= PROJECT_MAX ? this._withBytes(bytes, (p, n) => this.M._mw_project_check(p, n)) : -1; }
  projectLoad(slot, bytes) {
    const r = this._withBytes(bytes, (p, n) => this.M._mw_project_load(this.w, slot | 0, p, n));
    return r < 0 ? null : this._acts(r);
  }
  projectDir(bits) { this.M._mw_project_dir(this.w, bits >>> 0); }
  // a stored project, for the grid's bars and the project popup: 0 ok, < 0 not a valid blob
  projectInfo(slot, bytes) { return bytes && bytes.length && bytes.length <= PROJECT_MAX ? this._withBytes(bytes, (p, n) => this.M._mw_project_info(this.w, slot | 0, p, n)) : -1; }
  projectFlush() { return this._acts(this.M._mw_project_flush(this.w)); }
  projectSlot() { return this.M._mw_project_slot(this.w); }
  projectSave() { const n = this.M._mw_project_save(this.w); if (n <= 0) return null; const p = this.M._mw_blob(this.w, 0); return this.M.HEAPU8.slice(p, p + n); }
  input(packet, frame = 0, nowMs = 0) { return this._acts(this.M._mw_input(this.w, packet >>> 0, frame >>> 0, nowMs >>> 0)); }
  releaseAll() { return this._acts(this.M._mw_release_all(this.w)); }
  setMode(mode) { return this._acts(this.M._mw_set_mode(this.w, mode | 0)); }
  setScale(scale, root) { return this._acts(this.M._mw_set_scale(this.w, scale | 0, root | 0)); }
  setMaster(v) { this.M._mw_set_master(this.w, v | 0); }
  get master() { return this.M._mw_master(this.w); }
  // the Move's battery from its STATUS (byte 4, byte 5 the power bits): true when it changed (redraw); -1 unknown
  setBattery(battery, power) { return !!this.M._mw_set_battery(this.w, battery | 0, power | 0); }
  get battery() { return this.M._mw_battery(this.w); }
  // Sample's loop: the recorded audio's window peaks (0..255 each) for the Sampling waveform; how many were taken
  sampleLevels(peaks) {
    const n = Math.min(peaks.length | 0, 256);
    if (!n) return 0;
    const M = this.M, p = M._malloc(n);
    M.HEAPU8.set(Array.from(peaks).slice(0, n).map((v) => Math.max(0, Math.min(255, v | 0))), p);
    const k = M._mw_sample_levels(this.w, p, n);
    M._free(p);
    return k;
  }
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
