// SPDX-License-Identifier: MPL-2.0
// MIDI learn: a controller's knobs, pots and buttons on the drawn FM-1's knobs, MASTER and buttons. No DOM:
// web/site/test.mjs runs it. main.js feeds every Web MIDI message here first; a learned one is consumed (it plays no
// key and does not reach the firmware's MIDI in), the rest flow as before.
//   sources   "cc:<channel>:<number>" or "note:<channel>:<number>", channel 1..16 (notes: buttons only)
//   targets   SELECT ALGORITHM PRESETS KN1..KN4 (encoders), MASTER, and the 14 buttons by their printed names
//   a CC on an encoder, by mode:
//     abs      a pot (0..127): turns by the change since its last value; 1 step = 1 detent at speed 1x
//     rel64    binary offset: 64 + n (65 = +1, 63 = -1)
//     rel2c    two's complement: 1..63 up, 127..65 down (127 = -1)
//     relsign  sign bit: 1..63 up, 65..127 down (the low six bits the size, bit 6 set = down)
//   the knob speed (1/2x, 1x, 2x) scales every mode; fractions carry over
//   a CC on MASTER: 0..127 -> 0..1023, with pickup: after a learn, a reload or MASTER moved by any other input, the
//     pot is ignored until it reaches MASTER's position (within 2 steps) or crosses it
//   a note or a CC on a button: note-on / CC >= 64 press, note-off / CC < 64 release
import { E, B, PANEL_LABELS } from "./panel.js";

export const TARGETS = [
  ["SELECT", E.SELECT], ["ALGORITHM", E.ALGO], ["PRESETS", E.PRESETS], ["KN1", E.K1], ["KN2", E.K2], ["KN3", E.K3], ["KN4", E.K4],
].map(([id, role]) => ({ id, kind: "enc", role }))
  .concat([{ id: "MASTER", kind: "master", role: E.MASTER }])
  .concat(PANEL_LABELS.map((id, b) => ({ id, kind: "btn", b })));
const BY_ID = new Map(TARGETS.map((t) => [t.id, t]));
export const targetOf = (id) => BY_ID.get(id) || null;
// the panel's control (input.js hit: KM.SELECT role, KM.BTN label index) -> its target id
export const knobTarget = (role) => (TARGETS.find((t) => t.role === role) || {}).id || null;
export const buttonTarget = (b) => (b >= 0 && b < PANEL_LABELS.length ? PANEL_LABELS[b] : null);

export const MODES = { abs: "absolute", rel64: "relative 64±", rel2c: "relative 2's comp.", relsign: "relative sign bit" };
export const SPEEDS = [0.5, 1, 2];
export const speedOf = (v) => (SPEEDS.includes(v) ? v : 1);

// a message -> { type: "cc" | "note", ch 1..16, num, value, on } (note-on at velocity 0 is a note-off); else null
export function parseMsg(d) {
  if (!d || d.length < 3) return null;
  const s = d[0] & 0xF0, ch = (d[0] & 15) + 1;
  if (s === 0xB0) return { type: "cc", ch, num: d[1] & 127, value: d[2] & 127 };
  if (s === 0x90 || s === 0x80) return { type: "note", ch, num: d[1] & 127, value: d[2] & 127, on: s === 0x90 && d[2] > 0 };
  return null;
}
export const srcKey = (m) => `${m.type}:${m.ch}:${m.num}`;
export function srcLabel(key, long = false) {
  const [type, ch, num] = String(key).split(":");
  return `${type === "cc" ? "CC" : "note"} ${num}${long ? ",\u00a0channel\u00a0" : " ch "}${ch}`;   // (no break before the channel)
}
const validSrc = (k) => /^(cc|note):([1-9]|1[0-6]):(\d|[1-9]\d|1[01]\d|12[0-7])$/.test(String(k));

// why a message can't drive a target ("" when it can)
export function canBind(targetId, m) {
  const t = targetOf(targetId);
  if (!t || !m) return "no such control";
  if (m.type === "note" && t.kind !== "btn") return `${t.id} takes a CC: move a knob or a pot`;
  return "";
}

// the mode, from the values a CC sent while it moved (learn). A pot sweeping (four or more values, each different,
// one way) is absolute; values hugging 64 are binary offset; values only in 1..15 or 113..127 two's complement
export function guessMode(values) {
  const v = (values || []).filter((x) => Number.isInteger(x));
  if (!v.length) return "abs";
  let sweep = v.length >= 4;
  for (let i = 2; i < v.length && sweep; i++) sweep = Math.sign(v[i] - v[i - 1]) === Math.sign(v[1] - v[0]);
  for (let i = 1; i < v.length && sweep; i++) sweep = v[i] !== v[i - 1];
  if (sweep) return "abs";
  if (v.some((x) => x !== 64) && v.every((x) => Math.abs(x - 64) <= 7)) return "rel64";
  if (v.every((x) => (x >= 1 && x <= 15) || (x >= 113 && x <= 127))) return "rel2c";
  return "abs";
}
// a relative CC's value -> its detents (+ = clockwise)
export function relDelta(mode, v) {
  if (mode === "rel64") return v - 64;
  if (mode === "rel2c") return v < 64 ? v : v - 128;
  if (mode === "relsign") return v & 64 ? -(v & 63) : v & 63;
  return 0;
}

// one line of the sheet's list: "KN1 ← CC 21 ch 1 · absolute"
export const describe = (mp) => `${mp.target} ← ${srcLabel(mp.src)}${mp.mode ? " · " + MODES[mp.mode] : ""}`;
const modeFor = (targetId, src) => (targetOf(targetId).kind === "enc" && src.startsWith("cc:") ? "abs" : undefined);

export class MidiMap {
  constructor(list = [], { speed = 1 } = {}) {
    this.speed = speedOf(speed);
    this.byKey = new Map();   // src -> { src, target, mode }
    this.state = new Map();   // src -> { last, acc, down, picked, hinted }
    this.btnOut = new Map();  // target -> pressed (what was sent)
    this.masterSet = null;    // the MASTER value this map set last (anything else moved it: pickup again)
    this.load(list);
  }
  // the stored list, checked (anything odd is left out)
  load(list) {
    this.byKey.clear(); this.state.clear(); this.btnOut.clear(); this.masterSet = null;
    for (const x of Array.isArray(list) ? list : []) {
      if (!x || !validSrc(x.src) || !targetOf(x.target)) continue;
      if (x.src.startsWith("note:") && targetOf(x.target).kind !== "btn") continue;
      const mode = modeFor(x.target, x.src) && MODES[x.mode] ? x.mode : modeFor(x.target, x.src);
      this.byKey.set(x.src, mode ? { src: x.src, target: x.target, mode } : { src: x.src, target: x.target });
    }
  }
  list() { return [...this.byKey.values()].map((x) => ({ ...x })); }
  has(src) { return this.byKey.has(src); }
  get(src) { const x = this.byKey.get(src); return x ? { ...x } : null; }
  boundTargets() { return new Set([...this.byKey.values()].map((x) => x.target)); }
  sourcesOf(target) { return this.list().filter((x) => x.target === target); }
  // a source on a target (a source already bound moves); the mode: an encoder's CC only
  bind(src, target, mode) {
    if (!validSrc(src) || !targetOf(target)) return null;
    if (src.startsWith("note:") && targetOf(target).kind !== "btn") return null;
    const m = modeFor(target, src), mp = m ? { src, target, mode: MODES[mode] ? mode : m } : { src, target };
    this.byKey.delete(src);   // (re-inserted: the list keeps the newest last)
    this.byKey.set(src, mp);
    this.state.delete(src);
    return { ...mp };
  }
  setMode(src, mode) { const x = this.byKey.get(src); if (x && x.mode && MODES[mode]) { x.mode = mode; const s = this.state.get(src); if (s) s.acc = 0; } }
  remove(src) { this.state.delete(src); return this.byKey.delete(src); }
  forget(target) { let n = 0; for (const x of this.list()) if (x.target === target) { this.remove(x.src); n++; } return n; }
  clear() { this.byKey.clear(); this.state.clear(); }
  setSpeed(v) { this.speed = speedOf(v); }
  // a source's last value, as learn saw it (an absolute pot turns from there)
  seed(src, value) { this.st(src).last = value; }
  // nothing held any more (main.js: after an edit, controls.releaseSrc(SRC.MAP))
  releaseAll() { for (const s of this.state.values()) s.down = false; this.btnOut.clear(); }
  st(src) { let s = this.state.get(src); if (!s) this.state.set(src, s = { last: undefined, acc: 0, down: false, picked: false, hinted: false }); return s; }

  // a message: null when no mapping takes it (it flows on), else the actions (maybe none: consumed all the same)
  //   { kind: "turn", role, steps }  { kind: "master", value }  { kind: "btn", b, down }
  //   { kind: "pickup", pot, at, first }  MASTER waits for the pot (pot, at: 0..1023; first: once per wait)
  // ctx.master: MASTER's value now (pickup)
  feed(d, ctx = {}) {
    const m = parseMsg(d);
    if (!m) return null;
    const mp = this.byKey.get(srcKey(m));
    if (!mp) return null;
    const t = targetOf(mp.target), s = this.st(mp.src), out = [];
    if (t.kind === "btn") {
      const down = m.type === "note" ? m.on : m.value >= 64;
      s.down = down;
      const any = [...this.byKey.values()].some((x) => x.target === t.id && this.state.has(x.src) && this.state.get(x.src).down);
      if (any !== !!this.btnOut.get(t.id)) { this.btnOut.set(t.id, any); out.push({ kind: "btn", b: t.b, down: any }); }
      return out;
    }
    if (m.type !== "cc") return out;
    if (t.kind === "master") {
      const want = Math.round(m.value * 1023 / 127), cur = ctx.master;
      if (typeof cur !== "number") { s.last = m.value; out.push({ kind: "master", value: want }); return out; }
      if (cur !== this.masterSet) {   // moved by something else (or never set by a pot): every MASTER pot picks up again
        this.masterSet = cur;
        for (const x of this.byKey.values()) if (x.target === "MASTER" && this.state.has(x.src)) Object.assign(this.state.get(x.src), { picked: false, hinted: false });
      }
      if (!pickedUp(s, m.value, cur * 127 / 1023, 2)) { out.push({ kind: "pickup", pot: want, at: cur, first: !s.hinted }); s.hinted = true; return out; }
      this.masterSet = want;
      out.push({ kind: "master", value: want });
      return out;
    }
    let delta;
    if (mp.mode === "abs") { delta = s.last === undefined ? 0 : m.value - s.last; s.last = m.value; }
    else delta = relDelta(mp.mode, m.value);
    s.acc += delta * this.speed;
    const steps = Math.trunc(s.acc + (s.acc > 0 ? 1e-9 : -1e-9));
    s.acc -= steps;
    if (steps) out.push({ kind: "turn", role: t.role, steps });
    return out;
  }
}

// MASTER's pickup for one pot. s { last, picked }: the pot's state; v its value; at MASTER's position in the pot's units;
// tol how near is there. True once it is picked up: near MASTER, or it crossed MASTER since its last value
export function pickedUp(s, v, at, tol) {
  const last = s.last;
  s.last = v;
  if (!s.picked) s.picked = Math.abs(v - at) <= tol || (last !== undefined && (last - at) * (v - at) <= 0);
  return s.picked;
}
// one pot on MASTER with that pickup (the OMX-27's K5: js/omx27-views.js): v 0..max, cur MASTER now (0..1023) -> the
// actions as MidiMap.feed's (master, pickup). MASTER moved by anything else: it waits again
export class MasterPot {
  constructor() { this.s = { last: undefined, picked: false }; this.set = null; this.hinted = false; }
  feed(v, max, cur) {
    const want = Math.round(v * 1023 / max);
    if (cur !== this.set) { this.set = cur; this.s.picked = false; this.hinted = false; }
    if (!pickedUp(this.s, v, cur * max / 1023, 2 * max / 127)) { const first = !this.hinted; this.hinted = true; return [{ kind: "pickup", pot: want, at: cur, first }]; }
    this.set = want;
    return [{ kind: "master", value: want }];
  }
}

// learn mode: a control picked on the panel, then the next CC or note binds to it. take(d): null when learn leaves the
// message alone (nothing picked, or an unlearned note-off), else what happened:
//   { bound: mapping, fresh }  fresh: just bound (else the mode guess changed while the control kept moving)
//   { reject: why }            a note on a knob
//   {}                         consumed, nothing new
export class Learn {
  constructor(map) { this.map = map; this.active = false; this.selected = null; this.cur = null; }
  start() { this.active = true; this.selected = null; this.cur = null; }
  stop() { this.active = false; this.selected = null; this.cur = null; }
  select(target) { this.selected = target && targetOf(target) ? target : null; this.cur = null; }
  forgetSelected() { return this.selected ? this.map.forget(this.selected) : 0; }
  take(d) {
    if (!this.active || !this.selected) return null;
    const m = parseMsg(d);
    if (!m) return null;
    const key = srcKey(m);
    if (m.type === "note" && !m.on) return this.map.has(key) ? {} : null;
    const why = canBind(this.selected, m);
    if (why) return { reject: why };
    const c = this.cur;
    if (c && c.src === key && c.target === this.selected) {
      if (c.values.length < 24) c.values.push(m.value);
      if (m.type === "cc") this.map.seed(key, m.value);
      const mp = this.map.get(key);
      if (!mp || !mp.mode) return {};
      const g = guessMode(c.values);
      if (g === mp.mode) return {};
      this.map.setMode(key, g);
      return { bound: this.map.get(key), fresh: false };
    }
    const mode = targetOf(this.selected).kind === "enc" && m.type === "cc" ? guessMode([m.value]) : undefined;
    const mp = this.map.bind(key, this.selected, mode);
    if (m.type === "cc") this.map.seed(key, m.value);
    this.cur = { src: key, target: this.selected, values: [m.value] };
    return { bound: mp, fresh: true };
  }
}
