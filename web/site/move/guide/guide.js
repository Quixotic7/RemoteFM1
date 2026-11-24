// SPDX-License-Identifier: MPL-2.0
// remotefm1.com/move/guide: the keymaps. Each <figure class="km"> holds an empty .km-move and a list of keys; this
// draws the Move in it (the same figure as the Move page: ../js/move.js, the manual's layout), lights the controls
// each list item names in data-ctl, and puts the item's number on the first of them. Hovering or focusing an item
// shows its controls alone, in its colour; pressing a lit control marks its item. Nothing here talks to a device.
"use strict";
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

const ICON = {
  back: '<path d="M14 6l-6 6 6 6"/>',
  menu: '<path d="M6 8h12M6 12h12M6 16h12"/>',
  capture: '<path d="M6 10V6h4M14 6h4v4M18 14v4h-4M10 18H6v-4"/>',
  sample: '<circle cx="12" cy="12" r="3.5" class="f"/>',
  loop: '<path d="M7 14.5A5 5 0 1 1 12 17h-3"/><path d="M11 15l-2 2 2 2"/>',
  mute: '<path d="M7 17V7l5 6 5-6v10"/>',
  delete: '<path d="M7 7l10 10M17 7L7 17"/>',
  copy: '<path d="M9 9h8v8H9z"/><path d="M7 15V7h8"/>',
  undo: '<path d="M9 8L5 12l4 4"/><path d="M5 12h9a4 4 0 0 1 0 8h-2"/>',
  shift: '<circle cx="7" cy="12" r="1.6" class="f"/><circle cx="12" cy="12" r="1.6" class="f"/><circle cx="17" cy="12" r="1.6" class="f"/>',
  play: '<path d="M9 6.5v11l9-5.5z"/>',
  rec: '<circle cx="12" cy="12" r="5"/>',
  left: '<path d="M14 7l-5 5 5 5"/>',
  right: '<path d="M10 7l5 5-5 5"/>',
  plus: '<path d="M12 6v12M6 12h12"/>',
  minus: '<path d="M6 12h12"/>',
};
const icon = (name) => `<svg viewBox="0 0 24 24" aria-hidden="true">${ICON[name] || ""}</svg>`;

// the Move, in % of its body (the manual's figure: the body 1260 x 588 at 370,145)
const W = 1260, H = 588, X0 = 370, Y0 = 145;
const box = (cx, cy, w, h) => ({ left: (cx - w / 2 - X0) / W * 100 + "%", top: (cy - h / 2 - Y0) / H * 100 + "%", width: w / W * 100 + "%", height: h / H * 100 + "%" });
const COL_X = [654, 760, 867, 973, 1080, 1187, 1293, 1400], PAD_Y = [321, 402, 483, 564];
const RIGHT = [["capture", "sample"], ["loop", "mute"], ["delete", "copy"], ["undo", "shift"]];
// the lights of a keymap: ten LED colours, told apart by their numbers as well
const PAL = ["#3ccbff", "#b9f53e", "#ff8a4c", "#b98cff", "#ffd84a", "#ff6fb5", "#3cf0c0", "#5b93ff", "#ffffff", "#ff5f57"];

function drawMove(root) {
  const el = (cls, pos, name, html) => {
    const e = document.createElement("div");
    e.className = cls; Object.assign(e.style, pos); e.dataset.c = name;
    if (html) e.innerHTML = html;
    root.appendChild(e); return e;
  };
  el("m-oled", box(467, 217, 135, 69), "oled");
  COL_X.forEach((x, i) => el("m-knob", box(x, 204, 44, 44), "k" + (i + 1)));
  el("m-vol", box(1571, 212, 60, 60), "vol");
  el("m-jog", box(464, 362, 106, 106), "jog");
  el("m-btn", box(428, 483, 54, 54), "back", icon("back"));
  el("m-btn", box(500, 483, 54, 54), "menu", icon("menu"));
  PAD_Y.forEach((y, i) => el("m-track", box(567, y, 30, 70), "t" + (i + 1)));
  PAD_Y.forEach((y, r) => COL_X.forEach((x, c) => el("m-pad", box(x, y, 92, 66), "p" + (r + 1) + (c + 1))));
  RIGHT.forEach((pair, r) => pair.forEach((name, c) => el("m-btn", box([1499, 1571][c], PAD_Y[r], 54, 54), name, icon(name))));
  el("m-btn", box(428, 660, 54, 54), "play", icon("play"));
  el("m-btn", box(500, 660, 54, 54), "rec", icon("rec"));
  for (let i = 0; i < 16; i++) el("m-step", box(627 + i * 53.3, 660, 34, 34), "s" + (i + 1));
  el("m-btn sm", box(1489, 660, 36, 36), "left", icon("left"));
  el("m-btn sm", box(1581, 660, 36, 36), "right", icon("right"));
  el("m-btn sm", box(1535, 635, 36, 36), "up", icon("plus"));
  el("m-btn sm", box(1535, 685, 36, 36), "down", icon("minus"));
}

// "t1-3 p17 row2 steps" -> the controls' names. A range runs over the last digit(s): k5-8, s3-7, p13-16, t1-3
function names(spec) {
  const out = [];
  const seq = (pre, a, b) => { for (let i = a; i <= b; i++) out.push(pre + i); };
  for (const tok of spec.trim().split(/\s+/)) {
    let m;
    if (tok === "pads") for (let r = 1; r <= 4; r++) seq("p" + r, 1, 8);
    else if (tok === "steps") seq("s", 1, 16);
    else if (tok === "knobs") seq("k", 1, 8);
    else if ((m = /^row(\d)$/.exec(tok))) seq("p" + m[1], 1, 8);
    else if ((m = /^col(\d)(?:-(\d))?$/.exec(tok))) for (let c = +m[1]; c <= +(m[2] || m[1]); c++) for (let r = 1; r <= 4; r++) out.push("p" + r + c);
    else if ((m = /^p(\d)(\d)-(\d)$/.exec(tok))) seq("p" + m[1], +m[2], +m[3]);
    else if ((m = /^([kst])(\d+)-(\d+)$/.exec(tok))) seq(m[1], +m[2], +m[3]);
    else out.push(tok);
  }
  return out;
}

for (const fig of $$(".km")) {
  const dev = fig.querySelector(".km-move");
  drawMove(dev);
  const at = (n) => dev.querySelector(`[data-c="${n}"]`);
  const oled = at("oled");
  const [big, small] = (fig.dataset.oled || "RemoteFM1").split("|");
  oled.innerHTML = `<b></b><small></small>`;
  oled.firstChild.textContent = big; oled.lastChild.textContent = small || "";
  const items = $$(".km-keys > li", fig);
  items.forEach((li, i) => {
    const col = PAL[(li.dataset.col != null ? +li.dataset.col : i) % PAL.length];
    const num = li.dataset.n || String(i + 1);
    li.style.setProperty("--hc", col);
    const tag = document.createElement("i");
    tag.className = "km-n"; tag.textContent = num; tag.setAttribute("aria-hidden", "true");
    li.prepend(tag);
    li.tabIndex = 0;
    const els = names(li.dataset.ctl || "").map(at).filter(Boolean);
    // a control two items use keeps the first one's colour at rest and takes each item's own while that item shows
    els.forEach((e) => { (e._in = e._in || []).push({ i, col }); if (e._in.length === 1) { e.classList.add("hl"); e.style.setProperty("--hc", col); } });
    const anchor = els.find((e) => !e._badge) || els[0];   // the number: on the first control with none yet
    if (anchor) {
      anchor._badge = true;
      const b = document.createElement("span");
      b.className = "km-badge"; b.textContent = num; b._in = [{ i, col }];
      b.style.setProperty("--hc", col);
      b.style.left = anchor.style.left; b.style.top = anchor.style.top;
      dev.appendChild(b);
    }
  });
  const lit = [...dev.children].filter((e) => e._in);
  const show = (i) => {   // one item's controls alone (null: all of them)
    for (const e of lit) {
      const mine = e._in.find((x) => x.i === i);
      e.classList.toggle("off", i != null && !mine);
      e.style.setProperty("--hc", (mine || e._in[0]).col);
    }
    items.forEach((li, k) => li.classList.toggle("on", k === i));
  };
  items.forEach((li, i) => {
    li.addEventListener("pointerenter", () => show(i));
    li.addEventListener("focus", () => show(i));
    li.addEventListener("pointerleave", () => show(null));
    li.addEventListener("blur", () => show(null));
  });
  let at_ = -1;   // a lit control pressed: its item; pressed again: its next item, if it has more than one
  dev.addEventListener("click", (e) => {
    const t = lit.find((x) => x === e.target || x.contains(e.target));
    if (!t) { show(null); at_ = -1; return; }
    const ids = t._in.map((x) => x.i);
    at_ = ids[(ids.indexOf(at_) + 1) % ids.length];
    show(at_);
    items[at_].scrollIntoView({ block: "nearest", behavior: "smooth" });
  });
}
