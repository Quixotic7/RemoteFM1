// SPDX-License-Identifier: GPL-3.0-only
// The OMX-27 as the FM-1's controller (REMOTE mode, js/omx27.js): its views, what each key does in them, its LEDs and
// its screen's text. The owner's design: design/RemoteFM1---OMX-27-Remote.json (format: design/OMX-27-Designer-FORMAT.md).
// No DOM: web/site/test.mjs runs it.
//   keys      0 AUX, 1..10 the black keys, 11..26 the white keys (B3..C6: the design's keyMap)
//   K1        the view: Base, Seq, Keyboard, Quick Keys (four zones, a little hysteresis)
//   K2        the colour theme (six zones)
//   K3 / K4   Keyboard: the MIDI channel / the octave; Seq: K4 makes AUX the 11th black key or the button quick key
//   K5        MASTER, with pickup (midi-map.js MasterPot)
//   encoder   turns the selected knob (the last picked with a knob quick key, or the last turned); SELECT at first
// Base: 1 2 OCT- OCT+, 11 12 Range 1 / 2 (key 15 plays the FM-1's first F or its second), 13 the knob quick keys and 14
// the button quick keys (while held; a double click latches them, a click then closes them), the rest plays the FM-1's
// keys; AUX presses the selected button. Seq: the FM-1's keys 1:1. Keyboard: notes to the firmware's MIDI in. Quick
// Keys: every key a shortcut. Input -> actions (the page applies them):
//   { kind: "key", k, down }  FM-1 key 0..26     { kind: "btn", b, down }  a panel button (panel.js B, by label)
//   { kind: "turn", role, steps }  { kind: "select", role }  { kind: "midi", bytes }  { kind: "master", value }
//   { kind: "pickup", pot, at, first }  { kind: "view", view }  { kind: "theme", theme }
import { B, E, NKEY, PANEL_LABELS, keyBlack, ledAlpha, noteName } from "./panel.js";
import { MasterPot } from "./midi-map.js";
import { EV } from "./omx27.js";

export const VIEW = { BASE: 0, SEQ: 1, KEYS: 2, QUICK: 3 };
export const VIEW_NAMES = ["Base", "Seq", "Keyboard", "Quick Keys"];
// the OMX's keys' notes (the design's keyMap; AUX none)
export const OMX_NOTES = [null, 61, 63, 66, 68, 70, 73, 75, 78, 80, 82, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84];
// the FM-1's 27 keys (F3..G5): its 16 whites and 11 sharps, in order
export const FM1_WHITES = Array.from({ length: NKEY }, (_, k) => k).filter((k) => !keyBlack(k));
export const FM1_SHARPS = Array.from({ length: NKEY }, (_, k) => k).filter((k) => keyBlack(k));
export const KNOBS = [[E.SELECT, "SEL"], [E.PRESETS, "PRESETS"], [E.ALGO, "ALGO"], [E.K1, "KN1"], [E.K2, "KN2"], [E.K3, "KN3"], [E.K4, "KN4"]];
const KNOB_NAME = new Map(KNOBS);
const BTN6 = [B.FX, B.SEL, B.ENV, B.LFO, B.EDIT, B.GLO], BTN6B = [B.HOME, B.SAVE, B.ARP, B.SEQ, B.PLAY, B.REC];

// colours at full brightness (the design's hues); a shortcut shows at DIM and full while pressed or selected
const HUE = {
  aux: [255, 0, 0], r1: [128, 255, 0], r2: [255, 128, 0], kqk: [255, 0, 128], bqk: [255, 128, 128],
  knob: [0, 0, 255], kn: [255, 0, 0],
  btn: { [B.OCTDN]: [0, 255, 255], [B.OCTUP]: [0, 127, 255] },
};
[[255, 128, 0], [255, 255, 0], [255, 0, 255], [255, 0, 128], [0, 255, 128], [255, 0, 0]].forEach((c, i) => { HUE.btn[BTN6[i]] = c; HUE.btn[BTN6B[i]] = c; });
export const DIM = 0.5, DEAD = 0.16;   // DEAD: the dark red of a key past the FM-1's keyboard
// K2: the hue of the keys that mirror the FM-1's (and Night: dimmer shortcuts, for a dark room)
export const THEMES = [
  { name: "White", key: [255, 255, 255] },
  { name: "Ember", key: [255, 90, 0] },
  { name: "Ice", key: [0, 190, 255] },
  { name: "Violet", key: [150, 40, 255] },
  { name: "Lime", key: [110, 255, 0] },
  { name: "Night", key: [40, 60, 255], dim: 0.25 },
];
export const DOUBLE_MS = 400, NOTE_MS = 1500, VELOCITY = 100;

// a pot (0..127) in n zones; prev the zone now: it changes only past the zone's edge by a margin
export function zoneOf(v, n, prev) {
  const w = 128 / n, z = Math.max(0, Math.min(n - 1, Math.floor(v / w)));
  if (prev == null || prev < 0 || prev >= n || z === prev) return z;
  const m = Math.min(3, w / 4);
  return v < prev * w - m || v >= (prev + 1) * w + m ? z : prev;
}
const c7 = (c, f) => c.map((x) => Math.max(0, Math.min(127, Math.round(x * f / 2))));

export class OmxViews {
  // saved: prefs.omx ({ theme, knob, button, auxBlack, ch, oct })
  constructor(saved = {}) {
    const s = saved || {};
    this.view = VIEW.BASE;
    this.theme = Number.isInteger(s.theme) && s.theme >= 0 && s.theme < THEMES.length ? s.theme : 0;
    this.knob = KNOB_NAME.has(s.knob) ? s.knob : E.SELECT;
    this.button = Number.isInteger(s.button) && s.button >= 0 && s.button < PANEL_LABELS.length ? s.button : B.PLAY;
    this.auxBlack = s.auxBlack === true;
    this.ch = Number.isInteger(s.ch) && s.ch >= 1 && s.ch <= 16 ? s.ch : 1;
    this.oct = Number.isInteger(s.oct) && Math.abs(s.oct) <= 3 ? s.oct : 0;
    this.range = 0;
    this.held = new Map();                          // OMX key -> its role when it went down
    this.qk = { knob: { latched: false, last: -1e9 }, btn: { latched: false, last: -1e9 } };
    this.qkHeld = [];                               // "knob" / "btn", in the order held
    this.fm1 = new Uint8Array(NKEY);                // how many OMX keys hold each FM-1 key
    this.btns = new Uint8Array(PANEL_LABELS.length);
    this.master = new MasterPot();
    this.note = null;                               // { text, until }: the theme's name, MASTER's %
  }
  saved() { return { theme: this.theme, knob: this.knob, button: this.button, auxBlack: this.auxBlack, ch: this.ch, oct: this.oct }; }

  // the quick keys showing in Base: "knob", "btn" or null
  overlay() {
    if (this.view !== VIEW.BASE) return null;
    if (this.qkHeld.length) return this.qkHeld[this.qkHeld.length - 1];
    return this.qk.knob.latched ? "knob" : this.qk.btn.latched ? "btn" : null;
  }
  // the FM-1 key an OMX key plays in Base (-1: past the FM-1's keys)
  baseKey(k) { const n = OMX_NOTES[k], f = n == null ? -1 : n - 65 + 12 * this.range; return f >= 0 && f < NKEY ? f : -1; }

  // what key k does now: null nothing, else { kind: fm1 (k) | note (note, ch) | oct (b) | btn (b) | knob (role) | range (r)
  //   | qk (which) | aux (b) }
  roleOf(k) {
    const fm1 = (f) => (f >= 0 && f < NKEY ? { kind: "fm1", k: f } : null);
    const btn = (b) => ({ kind: "btn", b }), aux = { kind: "aux", b: this.button };
    if (this.view === VIEW.SEQ) {
      if (k === 0) return this.auxBlack ? fm1(FM1_SHARPS[10]) : aux;
      return fm1(k <= 10 ? FM1_SHARPS[k - 1] : FM1_WHITES[k - 11]);
    }
    if (this.view === VIEW.KEYS) {
      if (k === 0) return aux;
      const n = OMX_NOTES[k] + 12 * this.oct;
      return n >= 0 && n <= 127 ? { kind: "note", note: n, ch: this.ch } : null;
    }
    if (this.view === VIEW.QUICK) {
      if (k === 0) return aux;
      if (k === 1 || k === 2) return btn(k === 1 ? B.OCTDN : B.OCTUP);
      if (k >= 3 && k <= 9) return { kind: "knob", role: KNOBS[k - 3][0] };
      if (k >= 11 && k <= 16) return btn(BTN6[k - 11]);
      if (k >= 19 && k <= 24) return btn(BTN6B[k - 19]);
      return null;
    }
    const ov = this.overlay();
    if (k === 0) return aux;
    if (k === 1 || k === 2) { const b = k === 1 ? B.OCTDN : B.OCTUP; return ov === "btn" ? btn(b) : { kind: "oct", b }; }
    if (k === 11 || k === 12) return { kind: "range", r: k - 11 };
    if (k === 13 || k === 14) return { kind: "qk", which: k === 13 ? "knob" : "btn" };
    if (k <= 10) {
      if (ov === "knob") return k <= 9 ? { kind: "knob", role: KNOBS[k - 3][0] } : null;
      if (ov === "btn") return k >= 5 ? btn(BTN6[k - 5]) : null;
    } else if (ov === "btn") return k >= 21 ? btn(BTN6B[k - 21]) : null;
    return fm1(this.baseKey(k));
  }

  // ---- input
  key(k, ev, now = 0) {
    if (k < 0 || k >= 27) return [];
    if (ev === EV.DOWN) {
      if (this.held.has(k)) return [];
      const r = this.roleOf(k);
      this.held.set(k, r);
      return this.press(r, true);
    }
    if (ev === EV.UP) {
      if (!this.held.has(k)) return [];
      const r = this.held.get(k);
      this.held.delete(k);
      return this.press(r, false);
    }
    if (ev === EV.QUICK && this.view === VIEW.BASE && (k === 13 || k === 14)) this.click(k === 13 ? "knob" : "btn", now);
    return [];
  }
  // a quick click on a quick-key toggle: a second one soon after latches it; when latched, one closes it
  click(which, now) {
    const q = this.qk[which];
    if (q.latched) { q.latched = false; q.last = -1e9; return; }
    if (now - q.last <= DOUBLE_MS) {
      q.latched = true; q.last = -1e9;
      this.qk[which === "knob" ? "btn" : "knob"].latched = false;
    } else q.last = now;
  }
  press(r, down) {
    if (!r) return [];
    switch (r.kind) {
      case "fm1": return this.count(this.fm1, r.k, down, (k, d) => ({ kind: "key", k, down: d }));
      case "note": return [{ kind: "midi", bytes: [(down ? 0x90 : 0x80) | (r.ch - 1), r.note, down ? VELOCITY : 0] }];
      case "oct": case "aux": return this.count(this.btns, r.b, down, (b, d) => ({ kind: "btn", b, down: d }));
      case "btn":
        if (down) this.button = r.b;
        return this.count(this.btns, r.b, down, (b, d) => ({ kind: "btn", b, down: d }));
      case "knob": if (!down) return []; this.knob = r.role; return [{ kind: "select", role: r.role }];
      case "range": if (down) this.range = r.r; return [];
      case "qk":
        if (down) this.qkHeld.push(r.which); else { const i = this.qkHeld.lastIndexOf(r.which); if (i >= 0) this.qkHeld.splice(i, 1); }
        return [];
    }
    return [];
  }
  // a key or button held by several OMX keys: down with the first, up with the last
  count(arr, i, down, act) {
    const was = arr[i];
    arr[i] = down ? was + 1 : Math.max(0, was - 1);
    return (was === 0) !== (arr[i] === 0) ? [act(i, down)] : [];
  }
  enc(delta) { return delta ? [{ kind: "turn", role: this.knob, steps: delta }] : []; }
  // a knob turned elsewhere (the page, a MIDI controller, the FM-1): the encoder turns it next
  knobUsed(role) { if (KNOB_NAME.has(role)) this.knob = role; }
  // a pot: v 0..127, hires 0..16383; master MASTER now (0..1023)
  pot(p, v, hires, now = 0, master = 768) {
    if (p === 0) {
      const z = zoneOf(v, 4, this.view);
      if (z === this.view) return [];
      this.view = z;
      return [{ kind: "view", view: z }];
    }
    if (p === 1) {
      const z = zoneOf(v, THEMES.length, this.theme);
      this.note = { text: "THEME " + THEMES[z].name.toUpperCase(), until: now + NOTE_MS };
      if (z === this.theme) return [];
      this.theme = z;
      return [{ kind: "theme", theme: z }];
    }
    if (p === 2 && this.view === VIEW.KEYS) this.ch = zoneOf(v, 16, this.ch - 1) + 1;
    if (p === 3 && this.view === VIEW.KEYS) this.oct = zoneOf(v, 7, this.oct + 3) - 3;
    if (p === 3 && this.view === VIEW.SEQ) this.auxBlack = zoneOf(v, 2, this.auxBlack ? 1 : 0) === 1;
    if (p === 4) {
      const acts = this.master.feed(hires, 16383, master);
      const a = acts[0], pct = (x) => Math.round(x / 1023 * 100) + "%";
      this.note = { text: a.kind === "master" ? "MASTER " + pct(a.value) : `K5 ${pct(a.pot)} TO ${pct(a.at)}`, until: now + NOTE_MS };
      return acts;
    }
    return [];
  }
  // everything the OMX holds, let go (it left REMOTE mode, went away, or was disconnected)
  releaseAll() {
    const out = [];
    for (const r of this.held.values()) out.push(...this.press(r, false));
    this.held.clear(); this.qkHeld = []; this.fm1.fill(0); this.btns.fill(0);
    return out;
  }

  // a Base-view playing key past the end of the FM-1's keyboard (Range 2's top): lit dark red, so it reads as not there
  deadKey(k, ov) {
    return this.view === VIEW.BASE && ov !== "btn" && ((k >= 3 && k <= 10) || k >= 15) && this.baseKey(k) < 0;
  }

  // ---- what the OMX shows
  // the 27 LEDs (7-bit [r, g, b]); keyLevel(k): the FM-1's LED of key k (0..255)
  leds(keyLevel = () => 0) {
    const th = THEMES[this.theme], dim = th.dim || DIM, ov = this.overlay(), out = [];
    for (let k = 0; k < 27; k++) {
      const r = this.roleOf(k), pressed = this.held.has(k);
      const sc = (hue, on) => c7(hue, on ? 1 : dim);
      if (!r) out.push(this.deadKey(k, ov) ? c7(HUE.aux, DEAD * dim / DIM) : [0, 0, 0]);
      else if (r.kind === "fm1") out.push(c7(th.key, ledAlpha(keyLevel(r.k))));
      else if (r.kind === "note") out.push(pressed ? c7(th.key, 1) : [0, 0, 0]);
      else if (r.kind === "aux") out.push(sc(HUE.aux, pressed));
      else if (r.kind === "oct") out.push(sc(HUE.btn[r.b], pressed));
      else if (r.kind === "btn") out.push(sc(HUE.btn[r.b], pressed || this.button === r.b));
      else if (r.kind === "knob") out.push(sc(r.role === E.SELECT || r.role === E.PRESETS || r.role === E.ALGO ? HUE.knob : HUE.kn, this.knob === r.role));
      else if (r.kind === "range") out.push(sc(r.r ? HUE.r2 : HUE.r1, this.range === r.r));
      else if (r.kind === "qk") out.push(sc(HUE[r.which === "knob" ? "kqk" : "bqk"], pressed || ov === r.which));
      else out.push([0, 0, 0]);
    }
    return out;
  }
  // the screen: { title, line } (line: a moment's note, else the view's context)
  screen(now = 0) {
    const ov = this.overlay(), kn = KNOB_NAME.get(this.knob), aux = "AUX " + PANEL_LABELS[this.button];
    const title = this.view === VIEW.SEQ ? "SEQ" : this.view === VIEW.KEYS ? "KEYBOARD" : this.view === VIEW.QUICK ? "QUICK KEYS"
      : ov === "knob" ? "KNOBS" : ov === "btn" ? "BUTTONS" : "FM-1";
    let line;
    if (this.note && now < this.note.until) line = this.note.text;
    else if (this.view === VIEW.SEQ) line = `${kn}  ${this.auxBlack ? "AUX " + noteName(FM1_SHARPS[10]) : aux}`;
    else if (this.view === VIEW.KEYS) line = `CH ${this.ch}  OCT ${this.oct > 0 ? "+" : ""}${this.oct}  ${kn}`;
    else if (this.view === VIEW.QUICK) line = `${kn}  ${aux}`;
    else line = `R${this.range + 1}  ${kn}  ${aux}`;
    return { title, line, view: this.view };
  }
  // a short name for each key now (the tests' pictures of the OMX)
  labels() {
    return Array.from({ length: 27 }, (_, k) => {
      const r = this.roleOf(k);
      if (!r) return "";
      switch (r.kind) {
        case "fm1": return noteName(r.k);
        case "note": return noteName(r.note - 53);
        case "aux": return "AUX";
        case "oct": case "btn": return PANEL_LABELS[r.b];
        case "knob": return KNOB_NAME.get(r.role);
        case "range": return "R" + (r.r + 1);
        case "qk": return r.which === "knob" ? "KNOBS" : "BTNS";
      }
      return "";
    });
  }
}
