// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 RemoteFM1 contributors
// The SMK-37 emulator's page: an SMK-37 Pro's panel drawn in HTML, its controls sent to the worklet (app_worklet.js:
// the wasm's web_smk_* exports), the screen, the FM-1's key lights, the pads' colours and the button LEDs drawn back.
"use strict";
const $ = (id) => document.getElementById(id);
const stage = $("stage"), fit = $("fit");
const W = 1240, H = 640;
const STORE_KEY = "remotefm1-smk37-emu-store";

// ---------------------------------------------------------------- the model
// buttons in SMK_B_* order (smk_decode.h): the printed name, the FM-1 role, the LED's colour
const BTNS = [
  ["KNOB BANK", "CC 110"], ["FADER BANK", "CC 111"], ["<", "SEL −"], [">", "SEL +"],
  ["OCTAVE -", "OCT-"], ["OCTAVE +", "OCT+"],
  ["ARP", "FX"], ["NOTE REPEAT", "SEL"], ["SCALE", "ENV"], ["CHORD", "LFO"],
  ["GLOBE", "EDIT"], ["BT", "GLO"], ["PATCH", "HOME"], ["PARA", "SAVE"],
  ["FX", "ARP"], ["SEQ", "SEQ"], ["SEQ PLAY", "PLAY"], ["SEQ REC", "REC"],
];
const B_PLAY = 16, B_REC = 17, LED_PLAY_RED = 18;
const KNOBS = [["K1", "MASTER"], ["K2", "SELECT"], ["K3", "ALGORITHM"], ["K4", "PRESETS"],
               ["K5", "KNOB 1"], ["K6", "KNOB 2"], ["K7", "KNOB 3"], ["K8", "KNOB 4"]];
const KNOB_COL = ["#ffb23e", "#38e1ff", "#38e1ff", "#38e1ff", "#ff4fd8", "#ff4fd8", "#ff4fd8", "#ff4fd8"];
const PAD_NOTE = ["F3", "G3", "A3", "B3", "C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5", "D5", "E5", "F5", "G5"];
const NKEY = 37, FM1_LO = 5, FM1_HI = 31;          // keybed 0 = C3; the FM-1's 27 keys are 5..31 (F3..G5)
const BLACK = [1, 3, 6, 8, 10];
const isBlack = (k) => BLACK.includes(k % 12);
const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const keyName = (k) => NOTE[k % 12] + (3 + Math.floor(k / 12));

// ---------------------------------------------------------------- drawing
function el(tag, cls, parent = stage, style = {}) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  Object.assign(e.style, style);
  parent.appendChild(e);
  return e;
}
const px = (v) => v + "px";
function label(text, x, y, w, cls = "lbl", color) {
  const e = el("div", cls, stage, { left: px(x), top: px(y), width: px(w) });
  if (color) e.style.color = color;
  e.textContent = text;
  return e;
}

// the SMK-37 Pro's arrangement: the wheels top left, the screen beside them with six buttons under it, K1..K8 over
// the pads in the middle, the faders on the right with twelve buttons under them, the keybed along the bottom
// knobs over the pads' columns
const COL0 = 340, PITCH = 68;
const colX = (i) => COL0 + i * PITCH;
const knobs = KNOBS.map(([name, role], i) => {
  const x = colX(i), y = 44;
  label(name, x, 28, 60);
  const k = el("div", "knob ctl", stage, { left: px(x), top: px(y) });
  k.setAttribute("role", "slider");
  k.setAttribute("aria-label", `${name}, ${role}`);
  k.tabIndex = 0;
  const c = KNOB_COL[i];
  k.innerHTML = `<svg viewBox="0 0 60 60"><circle cx="30" cy="30" r="27" fill="none" stroke="#0b0c12" stroke-width="4"/>
    <path class="arc" fill="none" stroke="${c}" stroke-width="4" stroke-linecap="round" style="filter:drop-shadow(0 0 3px ${c})"/>
    <circle cx="30" cy="30" r="21" fill="url(#kg)" stroke="#4a4f62" stroke-width="1.5"/>
    <g class="ind"><line x1="30" y1="12" x2="30" y2="20" stroke="${c}" stroke-width="3" stroke-linecap="round"/></g></svg>`;
  label(role, x - 6, 110, 72, "role", c);
  return { el: k, i, angle: 0, arc: k.querySelector(".arc"), ind: k.querySelector(".ind") };
});
document.body.insertAdjacentHTML("beforeend", `<svg width="0" height="0" style="position:absolute"><defs>
  <radialGradient id="kg" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#4a4e5e"/><stop offset="1" stop-color="#1c1e27"/></radialGradient></defs></svg>`);
function arcPath(a0, a1) {
  const r = 27, p = (a) => [30 + r * Math.sin(a * Math.PI / 180), 30 - r * Math.cos(a * Math.PI / 180)];
  const [x0, y0] = p(a0), [x1, y1] = p(a1);
  return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
let master = 2896;
function drawKnob(k) {
  if (k.i === 0) {
    const a = -135 + 270 * master / 4096;
    k.arc.setAttribute("d", master > 0 ? arcPath(-135, Math.max(a, -134)) : "");
    k.ind.setAttribute("transform", `rotate(${a} 30 30)`);
    k.el.setAttribute("aria-valuenow", Math.round(master * 100 / 4096));
  } else {
    k.arc.setAttribute("d", arcPath(k.angle - 20, k.angle + 20));
    k.ind.setAttribute("transform", `rotate(${k.angle} 30 30)`);
  }
}
knobs.forEach(drawKnob);

// pads: 2 rows of 8, in reading order
const pads = PAD_NOTE.map((n, p) => {
  const x = colX(p % 8), y = p < 8 ? 132 : 200;
  const d = el("div", "pad ctl", stage, { left: px(x), top: px(y) });
  d.setAttribute("role", "button");
  d.setAttribute("aria-label", `Pad ${p + 1}, white key ${n}`);
  const glow = el("i", "", d);
  el("span", "", d).textContent = n;
  return { el: d, glow };
});
label("PADS · THE FM-1'S WHITE KEYS", colX(0), 268, PITCH * 7 + 60, "role", "#8a8fa8");

// faders
const faders = [0, 1, 2, 3].map((f) => {
  const x = 898 + f * 90, y = 10;
  const d = el("div", "fader ctl", stage, { left: px(x), top: px(y) });
  d.setAttribute("role", "slider");
  d.setAttribute("aria-label", `Fader ${f + 1}, KNOB ${f + 1}`);
  d.tabIndex = 0;
  el("div", "slot", d);
  const fill = el("div", "fill", d), cap = el("div", "cap", d);
  label(`F${f + 1}`, x, y + 218, 56);
  label(`KNOB ${f + 1}`, x, y + 231, 56, "role", "#ff4fd8");
  return { el: d, fill, cap, v: 0 };
});
function drawFader(f) {
  const t = 14 + 192 * (1 - f.v / 127);
  f.cap.style.top = px(t);
  f.fill.style.top = px(t);
  f.fill.style.bottom = "14px";
  f.el.setAttribute("aria-valuenow", f.v);
}
faders.forEach(drawFader);

// buttons: KNOB BANK .. OCTAVE+ in two columns under the screen, ARP .. SEQ REC in two rows of six under the faders
const buttons = [];
const btnAt = (b) => b < 6 ? [164 + (b % 2) * 68, 210 + (b >> 1) * 56, 56] : [898 + ((b - 6) % 6) * 55, 268 + Math.floor((b - 6) / 6) * 58, 50];
BTNS.forEach(([name, role], b) => {
  const [x, y, w] = btnAt(b);
  const d = el("div", "btn ctl", stage, { left: px(x), top: px(y), width: px(w) });
  d.setAttribute("role", "button");
  d.setAttribute("aria-label", `${name}, ${role}`);
  d.innerHTML = name.replace(" ", "<br>");
  const led = b === B_REC ? "#ff3b3b" : b === B_PLAY ? "#5dff6a" : b < 4 ? "#38e1ff" : "#ffb23e";
  d.style.setProperty("--led", led);
  label(role, x - 4, y + 42, w + 8, "role", b < 2 ? "#8a8fa8" : "#ffc77a");
  buttons.push({ el: d, bit: b });
});

// wheels
function wheel(x, name, cls) {
  label(name, x, 214, 46);
  const d = el("div", "wheel ctl " + cls, stage, { left: px(x), top: px(20) });
  d.setAttribute("role", "slider");
  d.setAttribute("aria-label", name === "PITCH" ? "Pitch wheel" : "Mod wheel, CC 1");
  d.tabIndex = 0;
  el("div", "w", d);
  const mark = el("div", "mark", d);
  return { el: d, mark };
}
const pitch = wheel(20, "PITCH", "pitch"), mod = wheel(74, "MOD", "mod");
label("SUSTAIN", 20, 238, 100, "role", "#8a8fa8");
const pedalLamp = el("div", "", stage, { left: px(27), top: px(254), width: px(86), height: px(6), borderRadius: "3px", background: "#0b0c12" });
let bend = 0, modV = 0;
function drawWheels() {
  pitch.mark.style.top = px(95 - bend / 8192 * 85);
  mod.mark.style.top = px(180 - modV / 127 * 170);
}
drawWheels();

// the keybed: C3..C6, the FM-1's 27 keys and the 10 outer ones told apart
const bed = el("div", "keybed", stage);
const whites = [];
for (let k = 0; k < NKEY; k++) if (!isBlack(k)) whites.push(k);
const BED_X = 20, BED_W = 1200, KW = BED_W / whites.length;
const keys = [];
for (let k = 0; k < NKEY; k++) {
  const outer = k < FM1_LO || k > FM1_HI;
  let d;
  if (!isBlack(k)) {
    const w = whites.indexOf(k);
    d = el("div", "wkey ctl" + (outer ? " outer" : ""), bed, { left: px(w * KW + 1), width: px(KW - 2) });
    if (k % 12 === 0 || k === FM1_LO || k === FM1_HI) el("em", "", d).textContent = keyName(k);
  } else {
    const w = whites.indexOf(k - 1), bw = KW * 0.6;
    d = el("div", "bkey ctl" + (outer ? " outer" : ""), bed, { left: px((w + 1) * KW - bw / 2), width: px(bw) });
  }
  el("div", "led", d);
  d.setAttribute("role", "button");
  d.setAttribute("aria-label", `Key ${keyName(k)}${outer ? " (outer key: MIDI)" : ""}`);
  keys.push({ el: d });
}
{
  const xOf = (k) => BED_X + whites.indexOf(k) * KW;
  const fm1L = xOf(FM1_LO), fm1R = xOf(FM1_HI) + KW;
  const r = (cls, x0, x1, t) => { const e = el("div", "range " + cls, stage, { left: px(x0 + 2), width: px(x1 - x0 - 4) }); e.textContent = t; };
  r("out", BED_X, fm1L, "MIDI 48–52");
  r("fm1", fm1L, fm1R, "THE FM-1'S 27 KEYS · F3–G5");
  r("out", fm1R, BED_X + BED_W, "MIDI 80–84");
}

// scale the panel to the width it has (no sideways scroll at phone width)
function fitStage() {
  const s = Math.min(1, fit.clientWidth / W);
  stage.style.transform = `scale(${s})`;
  fit.style.height = px(H * s);
}
new ResizeObserver(fitStage).observe(fit);
fitStage();

// ---------------------------------------------------------------- the link to the worklet
let node = null, gain = null;
const send = (m) => { if (node) node.port.postMessage({ type: "app", ...m }); };
const latchMode = () => $("latch").getAttribute("aria-pressed") === "true";

// holders: a control is down while anything holds it (pointers, keys, a latch); a press lasts at least 50 ms (the
// firmware reads its panel every few ms, a quick tap must not fall between two reads)
function holdable(onChange) {
  const by = new Map(), at = new Map();
  return {
    down(id, x, v) { by.set(id, { x, v }); at.set(id, performance.now()); onChange(x); },
    up(id) {
      const h = by.get(id);
      if (!h) return;
      const t = at.get(id), wait = 50 - (performance.now() - t);
      const go = () => { if (at.get(id) !== t) return; by.delete(id); at.delete(id); onChange(h.x); };
      if (wait > 0) setTimeout(go, wait); else go();
    },
    has(id) { return by.has(id); },
    held(x) { for (const h of by.values()) if (h.x === x) return h; return null; },
    ids() { return [...by.keys()]; },
    move(id, x, v) {
      const h = by.get(id);
      if (!h || h.x === x) return;
      const old = h.x;
      by.set(id, { x, v }); at.set(id, performance.now());
      onChange(old); onChange(x);
    },
  };
}
const keyOn = new Array(NKEY).fill(0), padOn = new Array(16).fill(0);
const keyH = holdable((k) => {
  const h = keyH.held(k), v = h ? h.v : 0;
  keys[k].el.classList.toggle("held", !!h);
  if (!!keyOn[k] !== !!v) { keyOn[k] = v; send({ op: "key", k, vel: v }); }
});
const padH = holdable((p) => {
  const h = padH.held(p), v = h ? h.v : 0;
  pads[p].el.classList.toggle("held", !!h);
  if (!!padOn[p] !== !!v) { padOn[p] = v; send({ op: "pad", p, vel: v }); }
});
let btnMask = 0;
const btnH = holdable(() => {
  let m = 0;
  for (const b of buttons) {
    const h = btnH.held(b.bit);
    b.el.classList.toggle("held", !!h);
    b.el.classList.toggle("latched", !!(h && btnH.has("L" + b.bit)));
    if (h) m |= 1 << b.bit;
  }
  if (m !== btnMask) { btnMask = m; send({ op: "btns", mask: m }); }
});

// velocity from where the hit lands: the far end soft, the front edge hard
function velAt(e, elm) {
  const r = elm.getBoundingClientRect(), f = Math.min(1, Math.max(0, (e.clientY - r.top) / r.height));
  return Math.max(1, Math.min(127, Math.round(12 + 115 * f)));
}
function capture(elm, e) { try { elm.setPointerCapture(e.pointerId); } catch { /* not a live pointer */ } }
function latchToggle(h, id, x, v, e) {
  if (!(e.shiftKey || e.button === 2 || latchMode())) return false;
  if (h.has(id)) h.up(id); else h.down(id, x, v);
  return true;
}
function wirePress(elm, h, x, vel) {
  elm.addEventListener("contextmenu", (e) => e.preventDefault());
  elm.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    if (!node) return;
    const v = vel ? velAt(e, elm) : 1;
    if (latchToggle(h, "L" + x, x, v, e)) return;
    if (h.has("L" + x)) { h.up("L" + x); return; }
    capture(elm, e);
    h.down("p" + e.pointerId, x, v);
  });
}
keys.forEach((k, i) => wirePress(k.el, keyH, i, true));
pads.forEach((p, i) => wirePress(p.el, padH, i, true));
buttons.forEach((b) => wirePress(b.el, btnH, b.bit, false));
// a finger that went down on a key slides along the keybed
bed.addEventListener("pointermove", (e) => {
  const id = "p" + e.pointerId;
  if (!keyH.has(id)) return;
  const t = document.elementsFromPoint(e.clientX, e.clientY).find((x) => x.classList && (x.classList.contains("wkey") || x.classList.contains("bkey")));
  if (!t) return;
  const k = keys.findIndex((x) => x.el === t);
  if (k >= 0) keyH.move(id, k, velAt(e, t));
});
function pointerUp(e) {
  const id = "p" + e.pointerId;
  keyH.up(id); padH.up(id); btnH.up(id);
}
stage.addEventListener("pointerup", pointerUp);
stage.addEventListener("pointercancel", pointerUp);
stage.addEventListener("lostpointercapture", pointerUp);

// knobs: vertical drag and the mouse wheel; K1 is MASTER (the firmware keeps its level: drawn from its reply)
function turn(k, n) {
  if (!n || !node) return;
  if (k.i === 0) master = Math.max(0, Math.min(4096, master + n * 96));
  else k.angle = (k.angle + n * 15) % 360;
  drawKnob(k);
  send({ op: "enc", e: k.i, n });
}
function wireDrag(elm, onDy, onWheel, onEnd) {
  let last = null;
  elm.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    if (!node) return;
    capture(elm, e);
    last = { y: e.clientY, x0: e.clientY, r: elm.getBoundingClientRect(), e };
    onDy(0, last, e);
  });
  elm.addEventListener("pointermove", (e) => {
    if (!last) return;
    const s = stage.getBoundingClientRect().width / W;
    onDy((last.y - e.clientY) / s, last, e);
    last.y = e.clientY;
  });
  const end = () => { if (last) { last = null; if (onEnd) onEnd(); } };
  elm.addEventListener("pointerup", end);
  elm.addEventListener("pointercancel", end);
  elm.addEventListener("wheel", (e) => {
    e.preventDefault();
    if (!node) return;
    onWheel(e.deltaMode === 1 ? e.deltaY * 33 : e.deltaY);
  }, { passive: false });
}
for (const k of knobs) {
  let acc = 0, wacc = 0;
  wireDrag(k.el, (dy) => {
    acc += dy;
    const n = Math.trunc(acc / 6);
    if (n) { acc -= n * 6; turn(k, n); }
  }, (dy) => {
    if (Math.abs(dy) >= 50) { turn(k, dy < 0 ? 1 : -1); wacc = 0; return; }   // a wheel notch: one detent
    wacc -= dy;
    const n = Math.trunc(wacc / 25);
    if (n) { wacc -= n * 25; turn(k, n); }
  });
  k.el.addEventListener("keydown", (e) => {
    const n = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: 5, PageDown: -5 }[e.key];
    if (n) { turn(k, n); e.preventDefault(); e.stopPropagation(); }
  });
}
// faders: the cap follows the pointer (a click jumps there)
function setFader(f, v) {
  v = Math.max(0, Math.min(127, Math.round(v)));
  if (v === f.v) return;
  f.v = v;
  drawFader(f);
  send({ op: "fader", f: faders.indexOf(f), v });
}
for (const f of faders) {
  const at = (e) => { const r = f.el.getBoundingClientRect(), s = r.height / 220; return 127 * (1 - ((e.clientY - r.top) / s - 14) / 192); };
  wireDrag(f.el, (dy, st, e) => setFader(f, at(e)), (dy) => setFader(f, f.v - dy / 12));
  f.el.addEventListener("keydown", (e) => {
    const n = { ArrowUp: 1, ArrowDown: -1, PageUp: 16, PageDown: -16 }[e.key];
    if (n) { setFader(f, f.v + n); e.preventDefault(); e.stopPropagation(); }
  });
}
// the pitch wheel springs back to the middle when let go; the mod wheel stays
let spring = 0;
function setBend(v) {
  v = Math.max(-8192, Math.min(8191, Math.round(v)));
  if (v === bend) return;
  bend = v; drawWheels(); send({ op: "bend", v });
}
function springBack() {
  cancelAnimationFrame(spring);
  const from = bend, t0 = performance.now();
  const step = () => {
    const t = Math.min(1, (performance.now() - t0) / 140);
    setBend(from * (1 - t) * (1 - t));
    if (t < 1) spring = requestAnimationFrame(step); else setBend(0);
  };
  spring = requestAnimationFrame(step);
}
wireDrag(pitch.el, (dy) => { cancelAnimationFrame(spring); setBend(bend + dy * 96); }, (dy) => { setBend(bend - dy * 24); clearTimeout(pitch.t); pitch.t = setTimeout(springBack, 250); }, springBack);
function setMod(v) {
  v = Math.max(0, Math.min(127, Math.round(v)));
  if (v === modV) return;
  modV = v; drawWheels(); send({ op: "mod", v });
}
wireDrag(mod.el, (dy) => setMod(modV + dy * 0.75), (dy) => setMod(modV - dy / 12));
pitch.el.addEventListener("keydown", (e) => {
  const n = { ArrowUp: 4096, ArrowDown: -4096 }[e.key];
  if (n) { setBend(n); clearTimeout(pitch.t); pitch.t = setTimeout(springBack, 300); e.preventDefault(); e.stopPropagation(); }
});
mod.el.addEventListener("keydown", (e) => {
  const n = { ArrowUp: 8, ArrowDown: -8 }[e.key];
  if (n) { setMod(modV + n); e.preventDefault(); e.stopPropagation(); }
});

// the sustain pedal (a switch on the jack)
let pedal = false;
function setPedal(on) {
  pedal = on;
  $("pedal").setAttribute("aria-pressed", on ? "true" : "false");
  pedalLamp.style.background = on ? "var(--lime)" : "#0b0c12";
  pedalLamp.style.boxShadow = on ? "0 0 8px var(--lime)" : "none";
  send({ op: "pedal", on });
}
$("pedal").addEventListener("click", (e) => { setPedal(!pedal); e.currentTarget.blur(); });
$("latch").addEventListener("click", (e) => {
  const on = $("latch").getAttribute("aria-pressed") !== "true";
  $("latch").setAttribute("aria-pressed", on ? "true" : "false");
  if (!on) for (const h of [keyH, padH, btnH]) for (const id of h.ids()) if (id[0] === "L") h.up(id);
  e.currentTarget.blur();
});

// ---------------------------------------------------------------- the computer keyboard
let typeBase = 12;                                  // the Z row's C: keybed 12 = C4
const PIANO = { KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyV: 5, KeyG: 6, KeyB: 7, KeyH: 8, KeyN: 9, KeyJ: 10, KeyM: 11, Comma: 12,
                KeyQ: 12, Digit2: 13, KeyW: 14, Digit3: 15, KeyE: 16, KeyR: 17, Digit5: 18, KeyT: 19, Digit6: 20, KeyY: 21, Digit7: 22, KeyU: 23, KeyI: 24 };
const KEYBTN = { Escape: 12, Backspace: 13, Space: B_PLAY, Enter: B_REC, ArrowLeft: 4, ArrowRight: 5,   // by FM-1 role
                 KeyA: 6, KeyF: 7, KeyK: 14, KeyL: 15, KeyO: 11, KeyP: 10 };
function showOct() { $("oct").textContent = `Typing keys: Z = ${keyName(typeBase)}`; }
showOct();
const typing = (e) => e.target.closest && e.target.closest("input, select, textarea, button, .bar");
addEventListener("keydown", (e) => {
  if (!node || e.metaKey || e.ctrlKey || e.altKey || typing(e)) return;
  if (e.target.getAttribute && e.target.getAttribute("role") === "slider" && e.code.startsWith("Arrow")) return;
  const id = "k" + e.code;
  if (e.code === "BracketLeft" || e.code === "BracketRight") {
    if (!e.repeat) { typeBase = Math.max(0, Math.min(12, typeBase + (e.code === "BracketLeft" ? -12 : 12))); showOct(); }
  } else if (e.code === "Backquote") {
    if (!e.repeat) setPedal(!pedal);
  } else if (e.code === "ArrowUp" || e.code === "ArrowDown") {
    turn(knobs[1], e.code === "ArrowUp" ? 1 : -1);
  } else if (e.code in PIANO) {
    const k = typeBase + PIANO[e.code];
    if (!e.repeat && k < NKEY) keyH.down(id, k, e.shiftKey ? 50 : 100);
  } else if (e.code in KEYBTN) {
    if (!e.repeat) btnH.down(id, KEYBTN[e.code]);
  } else return;
  e.preventDefault();
});
addEventListener("keyup", (e) => { const id = "k" + e.code; keyH.up(id); btnH.up(id); });
addEventListener("blur", () => { for (const h of [keyH, btnH]) for (const id of h.ids()) if (id[0] === "k") h.up(id); });

// ---------------------------------------------------------------- what comes back
const screen = $("screen"), ctx = screen.getContext("2d"), img = ctx.createImageData(240, 240);
function drawScreen(fb) {
  const d = img.data;
  for (let i = 0; i < 240 * 240; i++) {
    const v = fb[i], p = ((v >> 8) | (v << 8)) & 0xFFFF;
    d[i * 4] = ((p >> 11) & 31) * 255 / 31;
    d[i * 4 + 1] = ((p >> 5) & 63) * 255 / 63;
    d[i * 4 + 2] = (p & 31) * 255 / 31;
    d[i * 4 + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
}
// the FM-1's key LEDs (the 27 keys) on the keybed
function drawKeyLights(m) {
  for (let k = FM1_LO; k <= FM1_HI; k++) keys[k].el.classList.toggle("lit", !!(m >>> (k - FM1_LO) & 1));
}
// the SMK's own lights: the button LEDs (SEQ PLAY green, its red half, or both) and the pads' RGB as the LED chain
// gets it, shown at its hue and at the pad's level (the chain's values are dim: a bright LED behind a pad)
function drawSmkLights(m) {
  for (const b of buttons) {
    let lit = !!(m.leds >>> b.bit & 1);
    if (b.bit === B_PLAY) {
      const red = !!(m.leds >>> LED_PLAY_RED & 1);
      b.el.style.setProperty("--led", lit && red ? "#ffd23b" : red ? "#ff3b3b" : "#5dff6a");
      lit = lit || red;
    }
    b.el.classList.toggle("lit", lit);
  }
  m.rgb.forEach((c, p) => {
    const r = c >> 16 & 255, g = c >> 8 & 255, bl = c & 255, mx = Math.max(r, g, bl), lvl = m.lvl[p];
    const pad = pads[p];
    if (!mx || !lvl) { pad.glow.style.opacity = 0; pad.el.style.boxShadow = ""; return; }
    const s = 255 / mx, col = `rgb(${Math.round(r * s)},${Math.round(g * s)},${Math.round(bl * s)})`;
    const a = Math.sqrt(lvl / 255);
    pad.glow.style.background = `radial-gradient(circle at 50% 45%, #fff 0%, ${col} 45%, ${col} 100%)`;
    pad.glow.style.opacity = a.toFixed(3);
    pad.el.style.boxShadow = `0 0 ${Math.round(24 * a)}px ${Math.round(4 * a)}px ${col}`;
  });
  if (m.master !== undefined && m.master !== master) { master = m.master; drawKnob(knobs[0]); }
}

// ---------------------------------------------------------------- storage (the settings the firmware saves)
function loadStore() {
  try {
    const raw = JSON.parse(localStorage.getItem(STORE_KEY) || "{}"), out = {};
    for (const [o, b64] of Object.entries(raw)) out[o] = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0)).buffer;
    return out;
  } catch { return {}; }
}
function saveStore(store) {
  try {
    const raw = {};
    for (const [o, u8] of Object.entries(store)) { let s = ""; for (const c of u8) s += String.fromCharCode(c); raw[o] = btoa(s); }
    localStorage.setItem(STORE_KEY, JSON.stringify(raw));
  } catch { /* no storage: this visit only */ }
}

// ---------------------------------------------------------------- Web MIDI in (the app's USB MIDI in)
let midiAccess = null, midiIn = null;
function midiPacket(d) {
  const s = d[0];
  if (s >= 0x80 && s < 0xF0) return (s >> 4) | s << 8 | (d[1] || 0) << 16 | (d[2] || 0) << 24;
  if (s === 0xF8 || s === 0xFA || s === 0xFB || s === 0xFC) return 0xF | s << 8;
  return null;
}
function useMidi(id) {
  if (midiIn) midiIn.onmidimessage = null;
  midiIn = id && midiAccess ? midiAccess.inputs.get(id) : null;
  if (midiIn) midiIn.onmidimessage = (e) => { const p = midiPacket(e.data); if (p !== null) send({ op: "midi", pkt: p >>> 0 }); };
}
function listMidi() {
  const sel = $("midiport"), was = sel.value;
  sel.innerHTML = "<option value=''>No MIDI input</option>";
  for (const i of midiAccess.inputs.values()) {
    const o = document.createElement("option");
    o.value = i.id; o.textContent = i.name;
    sel.appendChild(o);
  }
  sel.value = [...sel.options].some((o) => o.value === was) ? was : "";
  if (!sel.value && sel.options.length > 1) sel.value = sel.options[1].value;
  useMidi(sel.value);
}
$("midi").addEventListener("click", async () => {
  if (!navigator.requestMIDIAccess) { $("alert").textContent = "This browser has no Web MIDI (Chrome, Edge or Firefox do)."; return; }
  try {
    midiAccess = await navigator.requestMIDIAccess();
    $("midi").hidden = true;
    $("midiport").hidden = false;
    midiAccess.onstatechange = listMidi;
    listMidi();
  } catch (err) { $("alert").textContent = "No MIDI: " + (err && err.message || err); }
});
$("midiport").addEventListener("change", (e) => useMidi(e.target.value));

// ---------------------------------------------------------------- the RemoteFM1 player (docs/SMK37.md "Emulator")
// The player on this site (its page with ?unit=smk37-emu: web/site/js/emu-port.js) sees this emulator as a unit named
// "FM-1 (SMK-37 emulator)" over a BroadcastChannel: what it sends comes in as the app's USB MIDI (its SysEx through
// RemoteFM1's tap), what the app sends on USB goes back, whole messages each. "here" both ways, twice a second.
const LINK = "remotefm1-smk37-emu";
let chan = null, playerAt = 0, sxOut = [];
// the player: the site's root (this page published three folders down, beside the SMK-37 test installer), else "/"
function playerUrl() {
  const seg = location.pathname.split("/");
  const published = seg.length >= 5 && seg.slice(-4, -1).join(" ") === "installer smk37 emu";
  const u = new URL(published ? "../../../" : "/", location.href);
  u.searchParams.set("unit", "smk37-emu");
  return u.href;
}
function showLink() {
  const on = !!chan && performance.now() - playerAt < 2500;
  $("link").textContent = !node ? "" : on ? "Player connected." : "Waiting for the player.";
  $("link").classList.toggle("on", on);
}
// what the player sent (one or more whole messages): SysEx to the tap, the rest as USB-MIDI packets
function fromPlayer(d) {
  for (let i = 0; i < d.length;) {
    const s = d[i];
    if (s === 0xF0) {
      let j = i + 1;
      while (j < d.length && d[j] !== 0xF7) j++;
      send({ op: "usx", bytes: d.slice(i, j + 1) });
      i = j + 1;
    } else {
      const n = s >= 0xF8 ? 1 : (s & 0xE0) === 0xC0 ? 2 : s >= 0x80 && s < 0xF0 ? 3 : 1;
      const p = midiPacket(d.subarray(i, i + n));
      if (p !== null) send({ op: "midi", pkt: p >>> 0 });
      i += n;
    }
  }
}
// what the app sent on USB: its SysEx (whole F0 .. F7 messages) and its USB-MIDI packets, each message to the player
function toPlayer(sx, pk) {
  const post = (b) => chan.postMessage({ from: "emu", type: "midi", data: b });
  for (let i = 0, s = -1; i < sx.length; i++) {
    if (sx[i] === 0xF0) s = i;
    else if (sx[i] === 0xF7 && s >= 0) { post(sx.slice(s, i + 1)); s = -1; }
  }
  for (const p of pk) {
    const cin = p & 15, b = [(p >>> 8) & 255, (p >>> 16) & 255, p >>> 24];
    if (cin >= 4 && cin <= 7) {                     // SysEx in packets (CIN 4 goes on; 5, 6, 7 end it)
      sxOut.push(...b.slice(0, cin === 5 ? 1 : cin === 6 ? 2 : 3));
      if (cin > 4) { post(Uint8Array.from(sxOut)); sxOut = []; }
    } else if (cin === 0xF) {
      if (b[0] !== 0xF8 && b[0] !== 0xFE) post(Uint8Array.of(b[0]));   // (not the clock or active sensing)
    } else if (cin >= 8) {
      post(Uint8Array.from(cin === 0xC || cin === 0xD ? b.slice(0, 2) : b));
    }
  }
}
function startLink() {
  if (chan || typeof BroadcastChannel === "undefined") return;
  chan = new BroadcastChannel(LINK);
  chan.onmessage = (e) => {
    const m = e.data;
    if (!m || m.from !== "player") return;
    if (m.type === "here") { if (performance.now() - playerAt > 2500) chan.postMessage({ from: "emu", type: "here" }); playerAt = performance.now(); showLink(); }
    else if (m.type === "midi" && m.data) fromPlayer(new Uint8Array(m.data));
  };
  send({ op: "link", on: true });
  chan.postMessage({ from: "emu", type: "here" });
  setInterval(() => { chan.postMessage({ from: "emu", type: "here" }); showLink(); }, 500);
  addEventListener("pagehide", () => chan.postMessage({ from: "emu", type: "bye" }));
  showLink();
}
$("player").addEventListener("click", (e) => { window.open(playerUrl(), "_blank", "noopener"); e.currentTarget.blur(); });

// ---------------------------------------------------------------- starting
const status = $("status"), cpu = $("cpu");
async function start() {
  const go = $("go");
  go.disabled = true;
  go.textContent = "Starting…";
  try {
    const ac = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
    const [wasm] = await Promise.all([
      fetch("app.wasm").then((r) => { if (!r.ok) throw new Error(`app.wasm: ${r.status}`); return r.arrayBuffer(); }),
      ac.audioWorklet.addModule("app_worklet.js").then(() => ac.audioWorklet.addModule("worklet.js"))]);
    const n = new AudioWorkletNode(ac, "fm1", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
    gain = ac.createGain();
    gain.gain.value = $("mute").getAttribute("aria-pressed") === "true" ? 0 : 1;
    n.connect(gain).connect(ac.destination);
    const ready = new Promise((res) => {
      n.port.onmessage = (e) => {
        const m = e.data;
        if (m.type === "ready") res();
        else if (m.type === "frame") {
          if (m.fb) drawScreen(m.fb);
          drawKeyLights(m.keys);
          if (m.load !== undefined) cpu.textContent = `${m.load}% of this computer's time`;
          if (m.store) saveStore(m.store);
        } else if (m.type === "app" && m.op === "lights") drawSmkLights(m);
        else if (m.type === "app" && m.op === "usb") { if (chan) toPlayer(m.sx, m.pk); }
      };
    });
    n.port.postMessage({ type: "load", wasm, store: loadStore(), master }, [wasm]);
    await ready;
    await ac.resume();
    node = n;
    window.emu = { node, ac };
    $("start").hidden = true;
    startLink();
    status.textContent = ac.sampleRate === 44100 ? "On." : `On (audio at ${ac.sampleRate} Hz).`;
  } catch (err) {
    go.disabled = false;
    go.textContent = "Start";
    $("alert").textContent = "Could not start: " + (err && err.message || err);
  }
}
$("go").addEventListener("click", start);
$("mute").addEventListener("click", (e) => {
  const on = $("mute").getAttribute("aria-pressed") !== "true";
  $("mute").setAttribute("aria-pressed", on ? "true" : "false");
  if (gain) gain.gain.value = on ? 0 : 1;
  e.currentTarget.blur();
});
if (location.hash === "#start") start();          // (a headless check: the browser must allow audio without a click)
