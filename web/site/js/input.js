// SPDX-License-Identifier: GPL-3.0-only
// Input: the computer keyboard (FM1VST's map, panel.js KEYMAP), the pointer on the drawn panel (mouse and multi-touch:
// keys and buttons held while pressed; Shift-click, right-click or ctrl-click latches one; knobs turned by a vertical
// drag or the wheel) and Web MIDI keyboards.
//
// Latches (the owner's "hold shift to click and hold multiple buttons"): Shift-click (or right-click, ctrl-click) on a
// key or button holds it down after the mouse lets go; the engine sees an ordinary held key or button (SRC.LATCH).
// Any click on a latched one lets it go. Plain clicks on the others stay momentary, Shift-clicks latch more. All of them
// let go on Escape (when nothing else took it: the sheet, a popover), Shift-click on an empty spot of the panel, the
// page losing focus or being hidden, and a firmware (re)start (main.js boot). Knobs ignore Shift as before.
import { KEYMAP, KM, B, E, SRC } from "./panel.js";

// ---------------------------------------------------------------- keyboard ---
export class Keyboard {
  // opts: { controls, enabled() -> bool, onLcdView(), onActivity() }
  constructor(opts) {
    this.o = opts;
    this.held = new Set();
    this.byCode = new Map();
    for (const e of KEYMAP) { if (!this.byCode.has(e.code)) this.byCode.set(e.code, []); this.byCode.get(e.code).push(e); }
    this.down = (e) => this.onDown(e);
    this.up = (e) => this.onUp(e);
    this.blur = () => this.releaseAll();
    addEventListener("keydown", this.down);
    addEventListener("keyup", this.up);
    addEventListener("blur", this.blur);
    document.addEventListener("visibilitychange", () => { if (document.hidden) this.releaseAll(); });
  }
  typing(e) { const t = e.target; return t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName)); }
  onDown(e) {
    // (cancelBubble: a handler before this one took the key, e.g. Escape closing the recorder's Takes popover)
    if (e.metaKey || e.ctrlKey || e.altKey || e.cancelBubble || this.typing(e) || !this.o.enabled(e)) return;
    const c = this.o.controls;
    if (e.code === "Escape" && c.anyLatched()) {   // Escape lets the latches go (and is OCT- only when none is held)
      e.preventDefault();
      if (!e.repeat) c.releaseLatches();
      return;
    }
    const list = this.byCode.get(e.code);
    if (!list) return;
    e.preventDefault();
    if (this.o.onActivity) this.o.onActivity(e);
    if (list[0].kind === KM.TURN) { c.turn(c.selKnob, c.selKnob === E.MASTER ? 2 * list[0].idx : list[0].idx); return; }
    if (e.repeat || this.held.has(e.code)) return;
    this.held.add(e.code);
    for (const m of list) this.action(m, true);
  }
  onUp(e) {
    if (!this.held.has(e.code)) return;
    this.held.delete(e.code);
    for (const m of this.byCode.get(e.code)) {
      let other = false;   // (Z and Esc both hold OCT-: the control stays down while either is)
      for (const h of this.held) for (const o of this.byCode.get(h) || []) other = other || (o.kind === m.kind && o.idx === m.idx);
      if (!other) this.action(m, false);
    }
  }
  action(m, down) {
    const c = this.o.controls;
    switch (m.kind) {
      case KM.KEY: c.holdKey(m.idx, SRC.KEY, down); break;
      case KM.BTN: c.holdBtn(m.idx, SRC.KEY, down); break;
      case KM.OCTBOTH: c.holdBtn(B.OCTDN, SRC.ESC, down); c.holdBtn(B.OCTUP, SRC.ESC, down); break;
      case KM.CYCLE:
        if (down) {
          const CYC = [E.SELECT, E.K1, E.K2, E.K3, E.K4];
          let at = CYC.indexOf(c.selKnob);
          if (at < 0) at = m.idx > 0 ? -1 : 0;
          c.select(CYC[(((at + m.idx) % 5) + 5) % 5]);
        }
        break;
      case KM.LCDVIEW: if (down && this.o.onLcdView) this.o.onLcdView(); break;
    }
  }
  releaseAll() {
    this.held.clear();
    this.o.controls.releaseSrc(SRC.KEY | SRC.ESC | SRC.POINTER);
  }
}

// ----------------------------------------------------------------- pointer ---
// view: the DeviceView of the canvas (toUnits, hit, stripTo); multi-touch: each pointer holds what it went down on.
// onShiftHint(): Shift held with the mouse over the panel (once per page; main.js shows "Shift-click to hold" once ever).
// win: the window (the page's; a stand-in in the Node tests)
// learn: MIDI learn (main.js) { active() -> bool, pick(hit) }: while active a click picks a control and operates nothing
export function attachPointer(canvas, view, controls, { onActivity, onScreenTap, onShiftHint, learn, win = globalThis } = {}) {
  const ptrs = new Map();   // pointerId -> { kind, idx, y, acc }
  const pos = (e) => { const r = canvas.getBoundingClientRect(); return view.toUnits(e.clientX - r.left, e.clientY - r.top); };
  let over = false, hinted = !onShiftHint;   // (over: a mouse is over the panel)
  const hint = () => { if (!hinted) { hinted = true; onShiftHint(); } };
  canvas.addEventListener("pointerdown", (e) => {
    if (onActivity) onActivity(e);
    const u = pos(e), h = view.hit(u.x, u.y);
    if (learn && learn.active()) { if (h && h.kind !== "screen") e.preventDefault(); learn.pick(h); return; }
    if (!h) { if (e.shiftKey && controls.anyLatched()) controls.releaseLatches(); return; }
    if (h.kind === "screen") { if (onScreenTap) onScreenTap(e); return; }
    e.preventDefault();
    try { canvas.setPointerCapture(e.pointerId); } catch (_) { /* (a synthetic event) */ }
    // the scrolling keybed's range strip (a phone upright): a tap centres the keys there, a drag scrolls them
    if (h.kind === "strip") { ptrs.set(e.pointerId, { kind: "strip", x0: e.clientX, moved: false }); return; }
    if (h.kind === KM.SELECT) {
      controls.select(h.idx);
      ptrs.set(e.pointerId, { kind: KM.SELECT, idx: h.idx, y: e.clientY, acc: 0 });
      return;
    }
    // a latched one: any click lets it go; Shift-click, right-click or ctrl-click latches one
    const on = h.kind === KM.KEY ? controls.keyLatched(h.idx) : controls.btnLatched(h.idx);
    if (on || e.shiftKey || e.button === 2 || e.ctrlKey) {
      if (h.kind === KM.KEY) controls.holdKey(h.idx, SRC.LATCH, !on); else controls.holdBtn(h.idx, SRC.LATCH, !on);
      return;
    }
    ptrs.set(e.pointerId, { kind: h.kind, idx: h.idx });
    if (h.kind === KM.KEY) controls.holdKey(h.idx, SRC.POINTER, true); else controls.holdBtn(h.idx, SRC.POINTER, true);
  });
  canvas.addEventListener("pointermove", (e) => {
    if (e.pointerType === "mouse") { over = true; if (e.shiftKey) hint(); }
    const p = ptrs.get(e.pointerId);
    if (p && p.kind === "strip") {
      if (!p.moved && Math.abs(e.clientX - p.x0) > 4) p.moved = true;
      if (p.moved && view.stripTo) { view.stripTo(pos(e).x, true); controls.dirty(); }
      return;
    }
    if (!p || p.kind !== KM.SELECT) return;
    p.acc += (p.y - e.clientY) / 8;   // 8 CSS pixels a detent, up = clockwise
    p.y = e.clientY;
    const s = Math.trunc(p.acc);
    if (s) { p.acc -= s; controls.turn(p.idx, s); }
  });
  const end = (e) => {
    const p = ptrs.get(e.pointerId);
    if (!p) return;
    ptrs.delete(e.pointerId);
    if (p.kind === "strip") { if (!p.moved && view.stripTo && e.clientX !== undefined) { view.stripTo(pos(e).x, false); controls.dirty(); } return; }
    if (p.kind === KM.SELECT) return;
    const still = [...ptrs.values()].some((q) => q.kind === p.kind && q.idx === p.idx);
    if (still) return;
    if (p.kind === KM.KEY) controls.holdKey(p.idx, SRC.POINTER, false); else controls.holdBtn(p.idx, SRC.POINTER, false);
  };
  canvas.addEventListener("pointerup", end);
  canvas.addEventListener("pointercancel", end);
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  canvas.addEventListener("pointerleave", (e) => { if (e.pointerType === "mouse") over = false; });
  if (win && win.addEventListener) {
    win.addEventListener("keydown", (e) => { if (e.key === "Shift" && over) hint(); });
    // the page loses focus (not to a frame in it, e.g. the Editor panel's manager) or is hidden: the latches let go
    win.addEventListener("blur", () => setTimeout(() => { if (!win.document || !win.document.hasFocus()) controls.releaseLatches(); }, 0));
    if (win.document) win.document.addEventListener("visibilitychange", () => { if (win.document.hidden) controls.releaseLatches(); });
  }
  let wheelAcc = 0;
  canvas.addEventListener("wheel", (e) => {
    const u = pos(e), h = view.hit(u.x, u.y);
    if (!h || h.kind !== KM.SELECT || (learn && learn.active())) return;
    e.preventDefault();
    const px = e.deltaMode === 1 ? e.deltaY * 16 : e.deltaMode === 2 ? e.deltaY * 400 : e.deltaY;
    wheelAcc += -px / 40;
    const s = Math.trunc(wheelAcc);
    if (s) { wheelAcc -= s; controls.turn(h.idx, s); }
  }, { passive: false });
}

// -------------------------------------------------------------------- MIDI ---
// Web MIDI input: one picked input, or all. handler(bytes) gets every message (Uint8Array).
export class MidiInput {
  constructor(handler, onChange) {
    this.handler = handler; this.onChange = onChange || (() => {});
    this.access = null; this.selected = ""; this.error = "";
    // the user's FM-1 running RemoteFM1 is never a MIDI keyboard here (its panel notes would play twice): W4's rule
    this.isUnitPort = (name) => /fm-1|remotefm1/i.test(name || "") && !/update/i.test(name || "");
    this.unitPorts = null;
    this.skipPort = null;   // (an OMX-27 in REMOTE mode: js/omx-link.js owns its port)
    this.supported = typeof navigator !== "undefined" && !!navigator.requestMIDIAccess;
  }
  skip(i) { return this.isUnitPort(i.name) || (this.unitPorts && this.unitPorts.inputId === i.id) || (!!this.skipPort && this.skipPort(i)); }
  inputs() { return this.access ? [...this.access.inputs.values()].filter((i) => !this.skip(i)).map((i) => ({ id: i.id, name: i.name, state: i.state })) : []; }
  async enable() {
    if (!this.supported) { this.error = "This browser has no Web MIDI (Chrome, Edge, Firefox and Opera have it)."; this.onChange(); return false; }
    try {
      this.access = await navigator.requestMIDIAccess({ sysex: false });
      this.access.onstatechange = () => { this.bind(); this.onChange(); };
      this.error = "";
    } catch (e) {
      this.error = "MIDI access was refused: allow it in the address bar and try again.";
    }
    this.bind();
    this.onChange();
    return !!this.access;
  }
  select(id) { this.selected = id; this.bind(); this.onChange(); }
  // MIDI already allowed on this site (no prompt if enable() runs now)
  async granted() {
    try { return (await navigator.permissions.query({ name: "midi", sysex: false })).state === "granted"; } catch (_) { return false; }
  }
  bind() {
    if (!this.access) return;
    for (const i of this.access.inputs.values()) {
      const on = !this.skip(i) && (this.selected === "*" || i.id === this.selected);
      i.onmidimessage = on ? (e) => this.handler(e.data) : null;
    }
  }
}
