// SPDX-License-Identifier: MPL-2.0
// RemoteFM1 for Move (mockup): the Move drawn from the manual's layout, the route chooser, the architecture diagram,
// the controls explorer, the step checklists (kept in localStorage), the build picker. Placeholder data only.
"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const store = {
  get() { try { return JSON.parse(localStorage.getItem("rfm1move.v1")) || {}; } catch (_) { return {}; } },
  set(v) { try { localStorage.setItem("rfm1move.v1", JSON.stringify(v)); } catch (_) { /* private mode: no memory */ } },
};

// ---------------------------------------------------------------- segmented LED meters
function meter(m, n) {
  if (n != null) m.dataset.n = n;
  const of = +m.dataset.of || 5, lit = +m.dataset.n || 0;
  if (m.children.length !== of) m.replaceChildren(...Array.from({ length: of }, () => document.createElement("i")));
  [...m.children].forEach((c, i) => c.classList.toggle("on", i < lit));
}
$$(".meter").forEach((m) => meter(m));

// ---------------------------------------------------------------- the Move, in % of its body (the manual's figure:
// the body 1260 x 588 at 370,145; every centre and size below is that figure's)
const W = 1260, H = 588, X0 = 370, Y0 = 145;
const box = (cx, cy, w, h) => ({ left: (cx - w / 2 - X0) / W * 100 + "%", top: (cy - h / 2 - Y0) / H * 100 + "%", width: w / W * 100 + "%", height: h / H * 100 + "%" });
const KNOB_X = [654, 760, 867, 973, 1080, 1187, 1293, 1400];
const PAD_X = [654, 760, 867, 973, 1080, 1187, 1293, 1400], PAD_Y = [321, 402, 483, 564];
const RIGHT = [["capture", "sample"], ["loop", "mute"], ["delete", "copy"], ["undo", "shift"]];
// the FM-1 mode's keys (rfm1cs-abletonmove's design): white rows and the black keys above them, two halves
const BLACK = [1, 1, 1, 0, 1, 1, 0, 0];
const KEY_ROW = ["black", "white", "black", "white"];
const HERO_COLS = ["#3ccbff", "#5b93ff", "#b98cff", "#ff6fb5", "#ff8a4c", "#ffd84a", "#b9f53e", "#3cf0c0"];

function el(parent, cls, pos, attrs = {}) {
  const e = document.createElement("div");
  e.className = cls; Object.assign(e.style, pos);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  parent.appendChild(e); return e;
}
function drawMove(root, hero) {
  const ctl = (name) => hero ? {} : { "data-ctl": name, tabindex: "0", role: "button", "aria-label": CTL[name] ? CTL[name].t : name };
  el(root, "m-oled", box(467, 217, 135, 69), ctl("oled")).textContent = hero ? "FELUCCA" : "FM-1 MODE";
  KNOB_X.forEach((x, i) => { const k = el(root, "m-knob", box(x, 204, 44, 44), ctl("knob" + (i + 1))); k.style.setProperty("--rot", (i * 37 - 120) + "deg"); });
  const v = el(root, "m-vol", box(1571, 212, 60, 60), ctl("volume")); v.style.setProperty("--rot", "40deg");
  el(root, "m-jog", box(464, 362, 106, 106), ctl("jog"));
  el(root, "m-btn", box(428, 483, 54, 54), ctl("back")).innerHTML = "<i></i>";
  el(root, "m-btn", box(500, 483, 54, 54), ctl("menu")).innerHTML = "<i></i>";
  const TRACK = ["#b98cff", "#b9f53e", "#3ccbff", "#ff5f57"];
  PAD_Y.forEach((y, i) => { const t = el(root, "m-track", box(567, y, 30, 70), ctl("track" + (i + 1))); t.style.setProperty("--lc", i === 0 || hero ? TRACK[i] : "#4a4a4f"); });
  PAD_Y.forEach((y, r) => PAD_X.forEach((x, c) => {
    const p = el(root, "m-pad", box(x, y, 92, 66), ctl("pads"));
    p.dataset.col = c;
    if (hero) {
      const k = (c * 3 + r * 5) % 7;
      if (k === 0) p.classList.add("lit"), p.style.setProperty("--pc", ["var(--led-b)", "var(--led-a)", "var(--fm1)"][(c + r) % 3]);
      else if (k < 3) p.classList.add("white");
      else if (k < 5) p.classList.add("hatch");
      else if (k === 5) p.classList.add("dots");
    }
    else if (KEY_ROW[r] === "white") p.classList.add("white");
    else if (BLACK[c]) p.classList.add("hatch");
  }));
  RIGHT.forEach((pair, r) => pair.forEach((name, c) => {
    const b = el(root, "m-btn" + (name === "shift" ? " shift" : " round"), box([1499, 1571][c], PAD_Y[r], 54, 54), ctl(name));
    b.innerHTML = "<i></i>";
    if (!hero && FM1_LIT[name]) { b.classList.add("lit"); b.style.setProperty("--lc", FM1_LIT[name]); }
  }));
  el(root, "m-btn round", box(428, 660, 54, 54), ctl("play")).innerHTML = "<i></i>";
  el(root, "m-btn round", box(500, 660, 54, 54), ctl("rec")).innerHTML = "<i></i>";
  for (let i = 0; i < 16; i++) {
    const s = el(root, "m-step", box(627 + i * 53.3, 660, 34, 34), ctl("steps"));
    if (hero && i % 4 === 0) s.style.setProperty("--lc", "#ffffff");
  }
  el(root, "m-btn", box(1489, 660, 36, 36), ctl("left"));
  el(root, "m-btn", box(1581, 660, 36, 36), ctl("right"));
  el(root, "m-btn", box(1535, 635, 36, 36), ctl("updown"));
  el(root, "m-btn", box(1535, 685, 36, 36), ctl("updown"));
  el(root, "m-usb", {}, ctl("usba"));
}
const FM1_LIT = { capture: "#ff8a4c", sample: "#ff8a4c", mute: "#ff8a4c", delete: "#ff8a4c", copy: "#ff8a4c", loop: "#ff8a4c", undo: "#ff8a4c" };

// what each control does: t title, b both routes, a Route A only, B Route B only, o the OLED's word
const CTL = {
  pads: { t: "The 32 pads", o: "KEYS F..F", b: "FM-1 mode: the FM-1's 27 keys, two octaves as two piano rows (white keys white, black keys pink), with velocity and pressure. MIDI mode: scales, chromatic or drums. Sequencer mode: clips." },
  knob1: { t: "Knob 1", o: "PRESETS", b: "The FM-1's PRESETS encoder. Touch any knob to see its firmware's own label on the screen." },
  knob2: { t: "Knob 2", o: "ALGO", b: "The FM-1's ALGORITHM encoder." },
  knob3: { t: "Knob 3", o: "SPARE", b: "Free in FM-1 mode; a CC in Control mode." },
  knob4: { t: "Knob 4", o: "SPARE", b: "Free in FM-1 mode; a CC in Control mode." },
  knob5: { t: "Knob 5", o: "K1", b: "The FM-1's K1. Its name comes from the firmware: a filter, a sample start, whatever the author chose." },
  knob6: { t: "Knob 6", o: "K2", b: "The FM-1's K2." },
  knob7: { t: "Knob 7", o: "K3", b: "The FM-1's K3." },
  knob8: { t: "Knob 8", o: "K4", b: "The FM-1's K4." },
  volume: { t: "Volume knob", o: "MASTER", b: "The FM-1's MASTER. Touch it, with Shift and a jog click, to leave the tool." },
  jog: { t: "Jog wheel", o: "SELECT", b: "Turns the FM-1's SELECT encoder.", B: "In the projects view: picks a slot, a firmware or a state; a press downloads from the catalog." },
  oled: { t: "The screen", o: "128 x 64", b: "The FM-1's 240 x 240 colour screen, shrunk to the Move's: a thumbnail with the knob being touched, a zoom, or the labels.", A: "The full screen is on your computer or phone.", B: "The full screen is on the page at remotefm1.local:7730, or on an FM-1." },
  track1: { t: "Track 1", o: "FM-1 MODE", b: "FM-1 mode (violet): the pads are the FM-1's keys.", B: "Held with Step 1: the projects view." },
  track2: { t: "Track 2", o: "MIDI MODE", b: "MIDI mode (lime): the pads play notes into the firmware's MIDI in. Hold it to pick the layout and channel." },
  track3: { t: "Track 3", o: "CONTROL", b: "Control mode (cyan): MIDI mode plus the knobs as CCs, in four banks." },
  track4: { t: "Track 4", o: "SEQUENCER", b: "Sequencer mode (red): 16 tracks, one per MIDI channel, clips on the pads, steps below." },
  steps: { t: "The 16 step buttons", o: "STEPS", b: "Sequencer steps in Sequencer mode.", B: "Shift + Step 1: the projects view (three slots, states, the catalog).", A: "Shift + Step 13 is Schwung's Tools menu, where Move Remote starts." },
  capture: { t: "Capture", o: "FX", b: "The FM-1's FX button." }, sample: { t: "Sample", o: "SEL", b: "The FM-1's SEL button." },
  mute: { t: "Mute", o: "ENV", b: "The FM-1's ENV button.", B: "In the projects view: the catalog." },
  delete: { t: "Delete", o: "LFO", b: "The FM-1's LFO button." }, copy: { t: "Copy", o: "EDIT", b: "The FM-1's EDIT button." },
  loop: { t: "Loop", o: "ARP", b: "The FM-1's ARP button." }, undo: { t: "Undo", o: "SAVE", b: "The FM-1's SAVE button." },
  shift: { t: "Shift", o: "GLO", b: "The FM-1's GLO, and the Move's own Shift for its shortcuts." },
  menu: { t: "Menu", o: "HOME", b: "The FM-1's HOME button." }, back: { t: "Back", o: "BACK", b: "Back to FM-1 mode from the sequencer." },
  play: { t: "Play", o: "PLAY", b: "The FM-1's PLAY; its LED shows the FM-1's green." }, rec: { t: "Record", o: "REC", b: "The FM-1's REC." },
  left: { t: "Left", o: "OCT-", b: "The FM-1's OCT-." }, right: { t: "Right", o: "OCT+", b: "The FM-1's OCT+." },
  updown: { t: "Plus / minus", o: "OCTAVE", b: "The octave in MIDI mode; pages in the sequencer." },
  usba: { t: "USB-A (on the back)", o: "USB-A", A: "A USB MIDI keyboard into the Move plays the firmware.", B: "An FM-1 with RemoteFM1 0.3+ here becomes the panel: its screen shows the firmware, its keys play it. Also where the Wi-Fi setup goes over the cable." },
};

drawMove($('[data-move="hero"]'), true);
// the hero's playhead: one pad column inverted at a time, in steps
{
  let col = 0;
  const pads = $$('[data-move="hero"] .m-pad');
  const step = () => { pads.forEach((p) => p.classList.toggle("ph", +p.dataset.col === col)); col = (col + 1) % 8; };
  step();
  if (!matchMedia("(prefers-reduced-motion: reduce)").matches) setInterval(step, 375);
}
const big = $('[data-move="explore"]');
drawMove(big, false);
function pick(name, node) {
  const c = CTL[name]; if (!c) return;
  $$(".picked", big).forEach((e) => e.classList.remove("picked"));
  if (name === "pads" || name === "steps" || name === "updown") $$(`[data-ctl="${name}"]`, big).forEach((e) => e.classList.add("picked"));
  else node.classList.add("picked");
  $(".ei-kicker").textContent = "Control";
  $(".ei-title").textContent = c.t;
  $$(".ei-route").forEach((p) => p.remove());
  const body = $(".ei-body"); body.textContent = c.b || "";
  let after = body;
  for (const r of ["A", "B"]) if (c[r]) { const p = document.createElement("p"); p.className = "ei-route"; p.innerHTML = `<b>Route ${r}:</b> `; p.append(c[r]); after.after(p); after = p; }
  $(".ei-oled-txt").textContent = c.o;
}
big.addEventListener("click", (e) => { const t = e.target.closest("[data-ctl]"); if (t) pick(t.dataset.ctl, t); });
big.addEventListener("keydown", (e) => { const t = e.target.closest("[data-ctl]"); if (t && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); pick(t.dataset.ctl, t); } });

// ---------------------------------------------------------------- the chooser
const ch = { i: 0, score: 0 };
const qs = $$(".ch-q"), dots = $$(".ch-dots i");
function chShow() {
  qs.forEach((q, i) => (q.hidden = i !== ch.i));
  dots.forEach((d, i) => d.classList.toggle("on", i < ch.i || (i === ch.i && ch.i < 3)));
  const res = $(".ch-result");
  res.hidden = ch.i < qs.length;
  const cnt = $(".ch-count");
  if (res.hidden) cnt.innerHTML = `${ch.i + 1}<small>/3</small>`;
  if (!res.hidden) {
    const r = ch.score > 0 ? "B" : ch.score < 0 ? "A" : "B";
    const say = r === "A"
      ? "<b style=\"--rc:var(--led-a)\">Route A, the Move as a controller.</b> Your computer or phone runs the firmware; nothing to flash."
      : ch.score === 0
        ? "<b style=\"--rc:var(--led-b)\">Either works.</b> Start with Route B: the Move on its own, and Route A is one tool away."
        : "<b style=\"--rc:var(--led-b)\">Route B, RemoteFM1 on the Move.</b> The Move becomes the instrument; a phone is its screen.";
    $(".ch-say").innerHTML = say;
    cnt.innerHTML = `${r}<small>!</small>`;
    $(".ch-go").dataset.go = r;
  }
}
$$(".ch-opts button").forEach((b) => b.addEventListener("click", () => { ch.score += b.dataset.v === "B" ? 1 : b.dataset.v === "A" ? -1 : 0; ch.i++; chShow(); }));
$("#ch-reset").addEventListener("click", () => { ch.i = 0; ch.score = 0; chShow(); });
chShow();

// ---------------------------------------------------------------- the architecture
const LINKS = {
  hostmove: { k: "wifi", r: "A", lbl: [260, 290, "Wi-Fi · :7720"], t: "Move Remote over Wi-Fi", d: ["The player connects to Move Remote at move.local:7720: every pad, knob and button in; the LEDs and the screen out.", "About 5 ms each way. From remotefm1.com, Chrome asks once to reach local network devices."] },
  usbc: { k: "usb", r: "A", lbl: [255, 392, "USB-C · 172.16.254.1"], t: "Move Remote over USB-C", d: ["A cable from the Move's USB-C to the computer: the Move appears as a small network at 172.16.254.1.", "The fastest link, under a millisecond."] },
  ble: { k: "ble", r: "A", lbl: [400, 448, "Bluetooth or USB (optional)"], t: "An FM-1 beside it (optional)", d: ["An FM-1 with RemoteFM1's Bluetooth build can be a second panel for the same firmware, and play its sound on its own speaker.", "Not needed for the Move to work."] },
  router: { k: "net", r: "A", lbl: [205, 130, "remotefm1.com"], t: "The firmwares, from remotefm1.com", d: ["The player loads each firmware from the catalog when you pick it. The apps keep them offline."] },
  phonemove: { k: "wifi", r: "B", lbl: [260, 290, "Wi-Fi · :7730"], t: "The page on your phone", d: ["remotefm1.local:7730, served by the Move: the FM-1's screen, the faceplate, the firmware browser, Mix & FX, the files.", "Optional: the Move plays without it."] },
  usba: { k: "usb", r: "B", lbl: [540, 368, "USB-A"], t: "An FM-1 on the Move's USB-A", d: ["Any FM-1 with RemoteFM1 0.3 or later: its screen shows the firmware, its keys and knobs play the slot.", "Also how the FM-1 learns your Wi-Fi from the Move's page."] },
  wifi: { k: "wifi", r: "B", lbl: [540, 252, "Wi-Fi"], t: "An FM-1 over Wi-Fi", d: ["The FM-1 with RemoteFM1's Wi-Fi build joins the same network; the Move finds it by a broadcast and links (TCP 7740).", "Wi-Fi wins over USB-A when both are there; one FM-1 at a time."] },
  net: { k: "net", r: "B", lbl: [470, 112, "signed catalog"], t: "Downloads to the Move", d: ["The Move downloads firmwares itself, built for its processor, from a signed list: every file checked before it is installed.", "About 40 come bundled; the rest need Wi-Fi once."] },
};
const NODES = {
  A: {
    _: ["Route A at a glance", ["The firmware runs in your browser or the RemoteFM1 app.", "The Move, running Move Remote, sends every touch and shows the LEDs and a small screen.", "Sound comes from the computer or phone."]],
    host: ["Computer, phone or tablet", ["Runs the firmware: remotefm1.com in Chrome or Edge, the iOS or Android app, or FM1VST in a DAW.", "The sound comes out here, so it records straight into your DAW."]],
    move: ["Ableton Move", ["Runs Schwung's tool Move Remote: a raw remote, no sound of its own while it runs.", "Its pads, knobs and buttons become the FM-1's keys, encoders and buttons; four modes on the track buttons."]],
    fm1: ["FM-1 (optional)", ["Not needed in Route A. With RemoteFM1's Bluetooth build it can join in as a second panel."]],
    cloud: ["The internet", ["Only to load the player and the firmwares the first time."]],
  },
  B: {
    _: ["Route B at a glance", ["The firmware runs on the Move: three slots, mixed, with Schwung's MIDI and audio effects.", "A phone shows the FM-1's screen; an FM-1 can be the panel.", "Sound comes from the Move."]],
    host: ["Phone (or any browser)", ["Shows the page the Move serves: the screen, the knobs' labels, the firmware browser, Mix & FX, files.", "Nothing runs here; close it and the Move keeps playing."]],
    move: ["Ableton Move", ["Runs Schwung's tool RemoteFM1: the firmwares compiled for its processor, on its own cores.", "Its controls play the firmware; Shift + Step 1 opens the projects view."]],
    fm1: ["FM-1 (optional)", ["A panel you can hold: on the USB-A cable with any RemoteFM1 0.3+, or wireless with the Wi-Fi build.", "It shows the firmware's full colour screen; the sound stays on the Move."]],
    cloud: ["The internet", ["Firmware downloads to the Move from the signed catalog. After that, nothing needs the internet."]],
  },
};
const svg = $(".arch-svg");
// the small pad grid and FM-1 keys inside the nodes
{
  const g = $(".padgrid", svg);
  for (let r = 0; r < 4; r++) for (let c = 0; c < 6; c++) {
    const p = document.createElementNS("http://www.w3.org/2000/svg", "rect");
    Object.entries({ x: 352 + c * 17, y: 246 + r * 22, width: 13, height: 17, rx: 2.5, opacity: .85 }).forEach(([k, v]) => p.setAttribute(k, v));
    const k = (r * 6 + c) % 5;
    p.style.fill = k === 0 ? "#b9f53e" : k === 1 ? "#fff" : k === 2 ? "url(#pd-hatch)" : "none";
    p.style.stroke = "#fff"; p.style.strokeWidth = "1"; p.removeAttribute("opacity");
    g.appendChild(p);
  }
}
const labels = {};
for (const [id, l] of Object.entries(LINKS)) {
  const path = $(`[data-lk="${id}"]`, svg);
  path.classList.add(l.k);
  const t = document.createElementNS("http://www.w3.org/2000/svg", "text");
  t.setAttribute("class", "lbl"); t.setAttribute("x", l.lbl[0]); t.setAttribute("y", l.lbl[1]); t.textContent = l.lbl[2];
  svg.appendChild(t); labels[id] = t;
  path.addEventListener("click", () => info(l.t, l.d, path));
}
let route = "A", filter = "";
function info(title, lines, node) {
  $$(".picked", svg).forEach((e) => e.classList.remove("picked"));
  if (node) node.classList.add("picked");
  $(".ai-kicker").textContent = node ? "Route " + route : "Tap a box or a line";
  $(".ai-title").textContent = title;
  const ul = document.createElement("ul");
  for (const s of lines) { const li = document.createElement("li"); li.textContent = s; ul.appendChild(li); }
  $(".ai-body").replaceChildren(ul);
}
function setRoute(r) {
  route = r;
  $$(".arch .seg button").forEach((b) => b.classList.toggle("on", b.dataset.arch === r));
  for (const [id, l] of Object.entries(LINKS)) {
    const on = l.r === r;
    $(`[data-lk="${id}"]`, svg).classList.toggle("off", !on);
    labels[id].style.display = on ? "" : "none";
  }
  $(".node-fm1", svg).classList.add("opt");
  $("[data-node=host]", svg).classList.toggle("here", r === "A");
  $("[data-node=move]", svg).classList.toggle("here", r === "B");
  const runs = $(".runs", svg);
  runs.style.setProperty("--rc", r === "A" ? "var(--led-a)" : "var(--led-b)");
  runs.setAttribute("transform", r === "A" ? "translate(40 192)" : "translate(325 162)");
  $(".sound", svg).setAttribute("transform", r === "A" ? "translate(198 260)" : "translate(480 222)");
  $("[data-node=host] .nt", svg).textContent = r === "A" ? "Computer" : "Phone";
  $("[data-node=host] .ns", svg).textContent = r === "A" ? "phone · tablet" : "any browser";
  applyFilter();
  info(NODES[r]._[0], NODES[r]._[1], null);
}
function applyFilter() {
  $$(".legend button").forEach((b) => b.classList.toggle("on", b.dataset.filter === filter));
  for (const [id, l] of Object.entries(LINKS)) {
    const dim = filter && l.k !== filter && !(filter === "usb" && l.k === "usb");
    $(`[data-lk="${id}"]`, svg).classList.toggle("dim", !!dim);
    labels[id].classList.toggle("dim", !!dim);
  }
}
$$("[data-node]", svg).forEach((n) => {
  const go = () => { const d = NODES[route][n.dataset.node]; info(d[0], d[1], n); };
  n.addEventListener("click", go);
  n.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); go(); } });
});
$$(".arch .seg button").forEach((b) => b.addEventListener("click", () => setRoute(b.dataset.arch)));
$$(".legend button").forEach((b) => b.addEventListener("click", () => { filter = filter === b.dataset.filter ? "" : b.dataset.filter; applyFilter(); }));
setRoute("B");

// ---------------------------------------------------------------- the install paths (progress kept per browser)
const saved = store.get();
function setPath(p) {
  $$("#setup .seg button").forEach((b) => b.classList.toggle("on", b.dataset.path === p));
  $$(".path").forEach((d) => (d.hidden = d.dataset.path !== p));
  saved.path = p; store.set(saved);
}
function count(p) {
  const lis = $$(`[data-steps="${p}"] li`), done = lis.filter((li) => $("input", li).checked).length;
  lis.forEach((li) => li.classList.toggle("done", $("input", li).checked));
  const box = $(`[data-ring="${p}"]`);
  $("b", box).textContent = done;
  meter($(".meter", box), done);
}
for (const p of ["A", "B"]) {
  const done = new Set(saved[p] || []);
  $$(`[data-steps="${p}"] li`).forEach((li) => {
    const box = $("input", li);
    box.checked = done.has(li.dataset.step);
    box.addEventListener("change", () => {
      saved[p] = $$(`[data-steps="${p}"] li`).filter((x) => $("input", x).checked).map((x) => x.dataset.step);
      store.set(saved); count(p);
    });
  });
  count(p);
}
$$("#setup .seg button").forEach((b) => b.addEventListener("click", () => setPath(b.dataset.path)));
$$("[data-go]").forEach((a) => a.addEventListener("click", () => setPath(a.dataset.go)));
setPath(saved.path || "B");

// ---------------------------------------------------------------- the flasher: this page offers only the Wi-Fi build
// (the owner's decision); the other two cards link to remotefm1.com/installer, which keeps USB + Bluetooth as its default

// ---------------------------------------------------------------- Wi-Fi: two ways
$$("#wifi .seg button").forEach((b) => b.addEventListener("click", () => {
  $$("#wifi .seg button").forEach((x) => x.classList.toggle("on", x === b));
  $$(".wifi-way").forEach((w) => (w.hidden = w.dataset.wifi !== b.dataset.wifi));
}));

// review shots: ?arch=A shows Route A in the diagram, ?path=A the Route A steps
if (/arch=A/.test(location.search)) setRoute("A");
if (/path=A/.test(location.search)) setPath("A");

// ---------------------------------------------------------------- 1-bit dither: an ordered (Bayer 8 x 8) dither of a
// gradient, drawn white-on-black at one canvas pixel per 2 CSS pixels and scaled up crisp (image-rendering: pixelated),
// as the Move's screen would draw it. data-dither: "fade" (white at the top, gone at the bottom) or "rule" (a short
// band that thins out to the right, under each section's title)
const BAYER = [0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
  3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21];
function dither(cv) {
  const kind = cv.dataset.dither, w = Math.max(8, Math.round(cv.clientWidth / 2)), h = Math.max(4, Math.round(cv.clientHeight / 2));
  cv.width = w; cv.height = h;
  const ctx = cv.getContext("2d"), img = ctx.createImageData(w, h), d = img.data;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const v = kind === "fade" ? 1 - y / h : Math.max(0, 1 - x / (w * .9)) * (1 - Math.abs(y / h - .5));
    if (v * 64 > BAYER[(y % 8) * 8 + (x % 8)] + .5) { const i = (y * w + x) * 4; d[i] = d[i + 1] = d[i + 2] = 255; d[i + 3] = 255; }
  }
  ctx.putImageData(img, 0, 0);
}
for (const h of $$(".sec-head")) { const cv = document.createElement("canvas"); cv.className = "dither dither-rule"; cv.dataset.dither = "rule"; h.appendChild(cv); }
const drawDithers = () => $$("canvas.dither").forEach(dither);
drawDithers();
addEventListener("resize", drawDithers);


// ---------------------------------------------------------------- the glossary as a list + detail (the cards stay
// as the content without script)
{
  const words = $("[data-lex]");
  const items = $$(".word", words).map((d) => ({ t: $("summary b", d).textContent, k: $("summary span", d).textContent, p: $("p", d).innerHTML }));
  const lex = document.createElement("div"); lex.className = "lex";
  lex.innerHTML = '<div class="lex-list panel" data-tab="WORDS" role="listbox" aria-label="Words"></div><div class="lex-detail panel" data-tab="MEANS" aria-live="polite"><p class="lex-k"></p><h3 class="lex-t"></h3><p class="lex-p"></p><b class="meter lex-m" data-of="8"></b></div>';
  const list = $(".lex-list", lex);
  const pick = (i) => {
    $$("button", list).forEach((b, j) => { b.classList.toggle("on", i === j); b.setAttribute("aria-selected", i === j); });
    $(".lex-k", lex).textContent = items[i].k; $(".lex-t", lex).textContent = items[i].t; $(".lex-p", lex).innerHTML = items[i].p;
    meter($(".lex-m", lex), i + 1);
  };
  items.forEach((it, i) => {
    const b = document.createElement("button"); b.type = "button"; b.setAttribute("role", "option");
    b.innerHTML = `<b></b><span></span>`; $("b", b).textContent = it.t; $("span", b).textContent = it.k;
    b.addEventListener("click", () => pick(i)); list.appendChild(b);
  });
  words.replaceWith(lex);
  pick(0);
}
