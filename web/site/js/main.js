// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 player page: the catalog, the panel and its input, the audio host, saves, the sheet, deep links.
//   ?fw=<id>          open that firmware (a Hub id)
//   &engine=native|emu  pick the engine (where both can)
//   &view=screen      the screen view
//   &embed=1          only the device: no header, no settings
import { Controls, DeviceView, ScreenView, Lcd, VIEW, SRC, PANEL_LABELS } from "./panel.js";
import { PRESETS, DEFAULT_THEME, presetNamed, css, derive } from "./theme.js";
import { Keyboard, attachPointer, MidiInput } from "./input.js";
import { Player } from "./audio.js";
import * as saves from "./saves.js";
import { Sheet, icon } from "./sheet.js";
import { makeSite } from "./remote-hook.js";
import { createFm1, mountUseFm1, mountHeader } from "./use-fm1.js";
import { liveCatalog, mergeLive, packageFor, chooseEngine, compareVersions, sourceOf, nativeOrigin } from "./live.js";
import { loadPrefs, savePrefs as writePrefs, keyHintsOn } from "./prefs.js";
import { mhzFor, pickMhz, stepDown, Governor, DEVICE_MHZ } from "./cpu.js";
import { EditorPanel } from "./editor-panel.js";
import { RecUI } from "./rec-ui.js";

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const EMBED = params.get("embed") === "1";

// ------------------------------------------------------------------ prefs ---
const store = (() => { try { return window.localStorage; } catch (_) { return null; } })();   // (null: blocked; prefs.js copes)
const prefs = loadPrefs(store);
const HOST = { cores: typeof navigator !== "undefined" ? navigator.hardwareConcurrency : undefined };   // (cpu.js weakHost)
const savePrefs = () => { writePrefs(store, prefs); };

// ------------------------------------------------------------------ state ---
const state = {
  entry: null, engine: null, wantEngine: null, engineState: "idle", info: null, running: false,
  theme: presetNamed(prefs.theme) || presetNamed(DEFAULT_THEME),
  view: params.get("view") === "screen" ? "screen" : params.get("view") === "device" ? "device" : prefs.view || "device",
  transpose: prefs.transpose | 0, notesPlayKeys: prefs.notesPlayKeys !== false, reachFirmware: !!prefs.reachFirmware,
  keyHints: keyHintsOn(prefs),                              // the computer-key letters on the panel: off by default
  saves: [], selectedSave: null, carryOffer: null,
  mhz: DEVICE_MHZ, mhzChoice: "auto", mhzPending: null,     // the emulated CPU clock of the running firmware (cpu.js); mhzPending: Auto's step, waiting for a take to end
  browseEngine: ["all", "native", "emu"].includes(prefs.browseEngine) ? prefs.browseEngine : "all",   // the browser's filter
  pixels: null, frameSeq: 0, levels: new Uint8Array(44), playGreen: 0,
  volume: typeof prefs.volume === "number" ? Math.max(0, Math.min(1, prefs.volume)) : 1,   // the page's volume (not the recording's)
  recBits: prefs.recBits === 16 ? 16 : 24, recCountIn: prefs.recCountIn === true,          // the recorder (js/rec-ui.js)
};
let catalog = null, snapshot = null, emuHook = null, player = null, sheet = null, site = null, siteEmit = () => {}, fm1 = null;
let editor = null;   // the Editor button and panel: the firmware's own manager, hosted (editor-panel.js)
let rec = null;      // the recorder: the header's record button and Takes (rec-ui.js)
const editorChanged = () => { if (editor) editor.update(); };
const lcd = new Lcd();
const controls = new Controls((p) => { if (player) player.panel(p); });
const device = new DeviceView($("device"), controls, lcd);
const screen = new ScreenView($("screen"), lcd);
device.setHints(state.keyHints);

// ------------------------------------------------------------------ drawing ---
let drawQueued = false;
const invalidate = () => { if (!drawQueued) { drawQueued = true; requestAnimationFrame(draw); } };
controls.onDraw = invalidate;
let laidOut = false;   // (no draw before the first layout: the canvases' default 300 x 150 bitmaps, stretched over the stage)
function draw() {
  drawQueued = false;
  if (!laidOut) return;
  if (!$("device-wrap").hidden) device.draw();
  if (!$("screen-wrap").hidden) screen.draw();
}

let stacked = false;
function layout() {
  const stage = $("stage"), W = stage.clientWidth, H = stage.clientHeight, dpr = window.devicePixelRatio || 1;
  stacked = !EMBED && W < 720 && H > W * 1.1;   // a phone upright: the screen, and the panel as a strip to scroll
  document.body.classList.toggle("stacked", stacked);
  const showDevice = stacked || state.view === "device", showScreen = stacked || state.view === "screen";
  $("device-wrap").hidden = !showDevice;
  $("screen-wrap").hidden = !showScreen;
  if (stacked) {
    const sh = Math.min(W, Math.max(160, H - 230));
    $("screen-wrap").style.height = sh + "px";
    const dh = H - sh, dw = Math.round(dh * VIEW.w / VIEW.h);
    $("device").style.width = dw + "px"; $("device").style.height = dh + "px";
    device.layout(dw, dh, dpr);
    screen.layout(W, sh, dpr);
  } else {
    $("screen-wrap").style.height = "";
    $("device").style.width = ""; $("device").style.height = "";
    if (showDevice) device.layout(W, H, dpr);
    if (showScreen) screen.layout(W, H, dpr);
  }
  placeCorner();
  laidOut = true;
  invalidate();
}
// the corner button: on the body's empty top-right corner (units 860, 46), or the screen view's bezel corner
function placeCorner() {
  const b = $("corner"), stage = $("stage").getBoundingClientRect();
  let x, y, d;
  if (!$("screen-wrap").hidden && (state.view === "screen" || stacked)) {
    const r = $("screen").getBoundingClientRect(), w = Math.min(r.width, r.height);
    d = Math.max(22, Math.min(28, Math.round(w * 0.045)));
    const bz = screen.bezel();
    const c = Math.max(d / 2 + 3, bz.x - (r.width - w) / 2);
    x = r.left - stage.left + (r.width + w) / 2 - c; y = r.top - stage.top + (r.height - w) / 2 + c;
  } else {
    const r = $("device").getBoundingClientRect(), p = device.toCss(860, 46);
    d = Math.max(22, Math.min(28, Math.round(17.5 * device.unitPx())));
    x = r.left - stage.left + p.x; y = r.top - stage.top + p.y;
  }
  Object.assign(b.style, { left: x - d / 2 + "px", top: y - d / 2 + "px", width: d + "px", height: d + "px" });
  // the start overlay: over the panel's LCD in the device view
  const st = $("start").style;
  if (!stacked && state.view === "device") {
    const r = $("device").getBoundingClientRect(), p = device.toCss(347, 160);
    const half = 240, sx = r.left - stage.left + p.x;
    st.setProperty("--start-x", (stage.width > 2 * half + 32 ? Math.max(half + 16, Math.min(stage.width - half - 16, sx)) : stage.width / 2) + "px"); st.setProperty("--start-y", r.top - stage.top + p.y + "px");
  } else { st.removeProperty("--start-x"); st.removeProperty("--start-y"); }
}
let chromeTimer = 0;
function activity() {
  if (EMBED || (sheet && sheet.isOpen)) return;
  document.body.classList.add("chrome");
  clearTimeout(chromeTimer);
  chromeTimer = setTimeout(() => { if (!$("corner").matches(":hover")) document.body.classList.remove("chrome"); }, 1500);
}

// ------------------------------------------------------------------ toast ---
let toastTimer = 0;
function toast(text, kind = "", ms = 0) {
  const t = $("toast");
  t.textContent = text; t.className = "toast show " + kind;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = "toast " + kind; }, ms || (kind === "bad" ? 7000 : 4000));
}

// RemoteFM1 itself, played (the catalog's "meta" entry): it makes no sound and waits for a host. One line when it
// starts (the owner's), and when an FM-1 connects while it runs.
const SELF_ID = "remotefm1";
const playingSelf = () => !!state.entry && state.entry.id === SELF_ID && state.engine === "emu";
const selfLine = (tone = fm1Tone) => tone === "on" ? "Your FM-1 is now remotely displaying a remote."
  : "I heard you liked firmwares so I made a firmware for your firmware so you can run firmwares without installing firmwares";

// --------------------------------------------------------------- firmware ---
const versionOf = (e, engine) => (!e ? "" : engine === "native" && e.native ? e.native.version : e.version);
const findEntry = (id) => catalog.firmwares.find((f) => f.id === id) || null;

// the deep link's engine, the visitor's choice (until a new release), else the default (newest wins: live.js)
const engineFor = (e, want) => chooseEngine(e, want, prefs);

async function refreshSaves() {
  state.saves = state.entry ? await saves.list(state.entry.id) : [];
  if (sheet) sheet.render();
}

async function openFirmware(id, want = null) {
  const e = typeof id === "string" ? findEntry(id) : id;
  if (!e) { toast(`No firmware "${id}" in the catalog.`, "bad"); return; }
  // the running firmware's flash first (some cores write it only when asked: W1)
  if (state.running && state.entry && state.engine !== "mock") { try { await currentFlash(); } catch (_) { /* (gone already) */ } }
  if (e.emuNote && !e.native) toast(`${e.name} is known not to run in the emulator: ${e.emuNote}.`, "bad");
  state.entry = e;
  state.wantEngine = engineFor(e, want);
  state.engine = state.wantEngine;
  state.info = null; state.selectedSave = null;
  hideOffer();
  if (!e.local) { prefs.last = e.id; savePrefs(); }
  updateTitle();
  if (!EMBED && !e.local) {
    const u = new URL(location.href);
    u.searchParams.set("fw", e.id);
    if (e.engines.length > 1 && state.wantEngine !== e.defaultEngine) u.searchParams.set("engine", state.wantEngine); else u.searchParams.delete("engine");
    history.replaceState(null, "", u);
  }
  device.setButtonNames(null); device.setKeymap(null);
  state.levels = new Uint8Array(44); state.playGreen = 0; device.setLeds(state.levels, 0);
  lcd.clear(); invalidate();
  await refreshSaves();
  if (state.running) await boot();
  else $("start-name").textContent = e.name;
}

// (re)start the current firmware on its engine, from a save (or the autosave)
let bootSeq = 0;
async function boot(fromSave = null) {
  let e = state.entry;
  const eng = state.wantEngine, seq = ++bootSeq;
  if (!e || !player) return;
  state.engine = eng; state.engineState = "loading"; state.info = null;
  setStatus(`Loading ${e.name}...`);
  if (sheet) sheet.render();
  editorChanged();
  try {
    // an emulated firmware's package from where the Hub points now (a gone one: the Hub once more, then the snapshot's)
    if (eng === "emu" && !e.localBytes && e.emu && e.emu.pkg && emuHook && emuHook.fetchPackage) {
      let got;
      try { got = await packageFor(e, { fetchPackage: (pkg) => emuHook.fetchPackage(pkg), recheck: recheckHub }); }
      catch (err) { throw new Error(`The package of ${e.name} could not be downloaded from ${e.emu.from === "site" ? "this site" : "the FM-1 Firmware Hub"} (${err.message || err}).`); }
      if (seq !== bootSeq || state.entry !== e) return;          // (another firmware was picked meanwhile)
      if (got.entry !== e) { replaceEntry(got.entry); e = got.entry; }
      e = { ...e, localBytes: got.bytes };
    }
    const ver = versionOf(state.entry, eng);
    const rec = fromSave || (await saves.get(state.entry.id, eng, ver, saves.AUTOSAVE));
    const clk = mhzFor(state.entry.id, prefs, HOST);
    state.mhz = clk.mhz; state.mhzChoice = clk.choice;
    governor = eng === "emu" ? new Governor({ ...clk, now: performance.now() }) : null;
    state.mhzPending = null;
    await player.load(e, eng, rec ? rec.bytes : null, eng === "emu" ? { mhz: clk.mhz } : {});
    if (!fromSave && !state.entry.local) offerCarry(state.entry, eng, ver);
  } catch (err) {
    if (seq !== bootSeq) return;
    const msg = err.message || String(err);
    await fallback(/worklet|module/i.test(msg) ? `The ${eng === "native" ? "native" : "emulated"} engine is not on this site yet.` : msg);
  }
}
// ---------------------------------------------------------- the CPU clock ---
// the emulated clock: a step down (Auto) or a new pick reloads the firmware from its running flash (fm1-emu takes the
// clock at boot only); the save is the same one (saves are per firmware, engine and version, not per clock)
let governor = null;
async function rebootKeepingFlash() {
  if (!state.running || !state.entry) return;
  let bytes = null;
  if (state.engine !== "mock" && state.info) { try { bytes = await currentFlash(); } catch (_) { bytes = null; } }
  await boot(bytes ? { name: saves.AUTOSAVE, bytes } : null);
}
// Auto never restarts the engine while a take records (as FM1VST in a DAW): the step waits for the take's end
// (state.mhzPending: the sheet's clock line says so), then applies (stepAuto). The take itself stays clean meanwhile:
// it is the rendered PCM, whole even when the device's output drops out.
function onLoad(m) {
  if (!governor || state.engine !== "emu" || !state.entry) return;
  const r = governor.feed(m, performance.now());
  if (!r) return;
  const e = state.entry;
  if (r.step && rec && rec.busy) {
    state.mhzPending = { id: e.id, mhz: r.step };
    toast(`Your computer can't run ${e.name} at full speed: Auto steps down to ${r.step} MHz after this take (expect dropouts until then; the take stays clean).`, "bad", 9000);
  } else if (r.step) stepAuto(e, r.step);
  else if (r.warn) {
    toast(state.mhzChoice === "auto" ? `Your computer can't keep up with ${e.name} even at ${state.mhz} MHz: expect dropouts.`
      : `Your computer can't keep up with ${e.name} at ${state.mhz} MHz: try Auto or a lower clock.`, "bad", 8000);
  }
  if (sheet) sheet.render();
}

function stepAuto(e, mhz) {
  state.mhzPending = null;
  prefs.autoMhz = stepDown(prefs, e.id, mhz); savePrefs();
  toast(`Your computer can't run this firmware at full speed: emulating at ${mhz} MHz.`, "bad", 7000);
  rebootKeepingFlash();
}
// a take ended: a step that waited for it applies now (if the same firmware still runs emulated on Auto)
function recIdle() {
  const p = state.mhzPending;
  if (!p) return;
  state.mhzPending = null;
  if (state.entry && state.entry.id === p.id && state.engine === "emu" && state.mhzChoice === "auto") stepAuto(state.entry, p.mhz);
  else if (sheet) sheet.render();
}

// the entry that played in place of the catalog's (a newer release found on a re-check, or the snapshot's version)
function replaceEntry(n) {
  const i = catalog.firmwares.findIndex((f) => f.id === n.id);
  if (i >= 0) catalog.firmwares[i] = n;
  state.entry = n;
  updateTitle();
  refreshSaves();
}
// the Hub's catalog once more (once per page), merged over the deployed one: the entry for id, or null
let recheckP = null;
function recheckHub(id) {
  if (!recheckP) recheckP = emuHook.fetchHubCatalog((u) => fetch(u, { cache: "no-cache" }))
    .then((hub) => mergeLive(snapshot, hub, { emuSkip: emuHook.EMU_SKIP || {} })).catch(() => null);
  return recheckP.then((c) => (c && c.firmwares.find((f) => f.id === id)) || null);
}
async function fallback(why) {
  const e = state.entry;
  console.warn("engine:", why);
  // the newest-wins default (emulated) could not start: the firmware's native core, if it has one, before a test engine
  if (state.wantEngine === "emu" && e.native && e.engines.includes("native") && !params.get("engine")) {
    toast(`${why} Playing the native core ${e.native.version} instead.`, "bad");
    state.wantEngine = "native";
    updateTitle();
    return boot();
  }
  state.engine = "mock"; state.engineState = "mock";
  editorChanged();
  toast(`${why} Playing a test engine instead.`, "bad");
  setStatus("");
  try { await player.load({ ...e, name: e.name }, "mock", null); } catch (err) { toast(String(err.message || err), "bad"); }
  if (sheet) sheet.render();
}

function updateTitle() {
  const e = state.entry;
  document.title = e ? `${e.name} · FM-1 Player` : "FM-1 Player";
  $("fw-name").textContent = e ? e.name : "";
  $("fw-ver").textContent = e ? versionOf(e, state.wantEngine) : "";
  updateSource();
  editorChanged();
}
// the header's link to the running firmware's own source, at the version that runs (live.js sourceOf); a file from the
// visitor's computer (M-VAVE's stock firmware, any .fwsc) has none: the link shows so, disabled
const sourceNow = () => sourceOf(state.entry, state.wantEngine, catalog && catalog.credits);   // (the engine picked: a test engine stands in for it)
function updateSource() {
  const a = $("fw-src"), e = state.entry, s = e ? sourceNow() : null;
  a.hidden = !e;
  if (!e) return;
  a.classList.toggle("off", !s);
  if (s) { a.href = s.url; a.title = s.title; a.setAttribute("aria-label", `${e.name}'s source code (opens GitHub)`); a.removeAttribute("aria-disabled"); }
  else {
    a.removeAttribute("href"); a.setAttribute("aria-disabled", "true");
    a.title = `${e.name}: a package from your computer, no public source`; a.setAttribute("aria-label", a.title);
  }
  a.querySelector(".src-text").textContent = s ? "Source" : "No public source";
}
function setStatus(text) { $("status").textContent = text; }
// every emulated run credits fm1-emu (its MIT licence asks for the notice; W2)
function creditStatus() {
  const c = catalog.credits.fm1emu;
  $("status").replaceChildren("Emulated by ", Object.assign(document.createElement("a"), { href: c.source, target: "_blank", rel: "noopener", textContent: "fm1-emu" }),
    `, the FM-1 emulator by ${c.author} (${c.license} licence)`);
}

function metaLine() {
  const e = state.entry;
  if (!e) return "";
  const eng = state.engine === "mock" ? "test engine (the real one is not here yet)" : state.engine === "native" ? (e.native && e.native.by === "author" ? "native, built by the author" : "native") : `emulated, ${state.mhz} MHz`;
  return `${e.category} · ${e.tag} · ${e.author} · ${eng}`;
}
function engineNote() {
  const e = state.entry;
  if (!e) return "";
  const o = nativeOrigin(e);
  const n = !e.native ? null : (e.nativeBehind
    ? `Native: ${o.long}, older than the Hub's ${e.version}, compiled to WebAssembly; starts at once and is lighter on the CPU.`
    : `Native: ${o.long}, compiled to WebAssembly; starts at once.`) + (e.nativeWhy ? ` Of the two native builds, ${e.nativeWhy}.` : "");
  const m = e.engines.includes("emu") ? `Emulated: the firmware's own package ${e.version}${e.nativeBehind ? " (the latest)" : ""} on fm1-emu, the FM-1's chip in the browser; heavier on the CPU.` : null;
  const now = state.engine === "mock" ? `Now: a test engine, because the ${state.wantEngine === "native" ? "native" : "emulated"} engine could not start.` : "";
  const credit = state.engine === "emu" ? catalog.credits.fm1emu.line + "." : "";
  return [n, m, now, credit].filter(Boolean).join(" ");
}

// ------------------------------------------------------------- carry a save ---
// a version with no save, an older one with a save on the same engine kind: offered once (saves.js Carry)
const carry = new saves.Carry({
  compare: compareVersions,
  decisions: { get: (k) => (prefs.carry || {})[k], set: (k, v) => { prefs.carry = { ...(prefs.carry || {}), [k]: v }; savePrefs(); } },
});
const carryText = (o) => `Bring your ${state.entry ? state.entry.name : o.fw} ${o.from} save to ${o.to}? (a backup of it is kept)`;
async function offerCarry(e, eng, ver) {
  let o = null;
  try { o = await carry.check(e.id, eng, ver); } catch (_) { return; }
  if (!o || state.entry !== e || state.wantEngine !== eng || state.engine === "mock") return;
  state.carryOffer = o;
  const box = $("offer");
  $("offer-text").textContent = carryText(o);
  box.hidden = false;
  requestAnimationFrame(() => box.classList.add("show"));
  if (sheet) sheet.render();
}
function hideOffer() {
  state.carryOffer = null;
  const box = $("offer");
  if (box) { box.classList.remove("show"); box.hidden = true; }
}
async function answerCarry(yes) {
  const o = state.carryOffer;
  if (!o) return;
  hideOffer();
  try {
    const rec = await carry.answer(o, yes);
    await refreshSaves();
    if (rec) { toast(`Your ${o.from} save is now ${o.to}'s; the ${o.from} one is kept as it was.`, "ok"); if (state.running && state.entry && state.entry.id === o.fw) await boot(rec); }
  } catch (err) { toast(err.message || String(err), "bad"); }
  if (sheet) sheet.render();
}

// ------------------------------------------------------------------ audio ---
function makePlayer() {
  player = new Player(catalog, {
    frame(px, seq) { state.pixels = px; state.frameSeq = seq; lcd.put(px); invalidate(); siteEmit("frame", px, seq); },
    leds(levels, green) { state.levels = levels; state.playGreen = green; device.setLeds(levels, green); invalidate(); siteEmit("leds", levels, green); },
    info(info) {
      state.info = info;
      if (state.engine !== "mock") state.engineState = "running";
      device.setButtonNames(info.buttonNames && info.buttonNames.length === 14 ? info.buttonNames : PANEL_LABELS);
      device.setKeymap(info.keymap || null);
      setStatus("");
      if (state.engine === "emu") {
        setStatus("Warming up the emulator...");
        setTimeout(() => { if (state.engine === "emu") creditStatus(); }, 1500);
      }
      if (playingSelf()) toast(selfLine(), "", 6500);   // (the long one: time to read it)
      controls.emit();   // the panel as it stands, to the new engine
      invalidate();
      if (sheet) sheet.render();
      siteEmit("info", info); siteEmit("firmware", { entry: state.entry, engine: state.engine });
      editorChanged();
    },
    flash(m) {
      if (state.engine === "mock" || !state.entry) return;
      if (!m.bytes) { clearTimeout(flashTimer); flashTimer = setTimeout(() => player.flashReq(), 1500); return; }
      const e = state.entry, key = `${e.id}/${state.engine}`, hsh = quickHash(m.bytes);
      if (lastSaved.get(key) !== hsh) {
        lastSaved.set(key, hsh);
        saves.put(e.id, state.engine, versionOf(e, state.engine), saves.AUTOSAVE, m.bytes).then(refreshSaves).catch(() => {});
      }
      if (pendingSave) { const f = pendingSave; pendingSave = null; f(m.bytes); }
    },
    // a reboot (a manager's RESTART, the firmware's own reset): it comes back from its flash as it stands, as an FM-1 does
    // (the autosave follows: the flash read is saved, flash() below); the port goes and comes back (boot -> info)
    halted(code) {
      toast(`${state.entry ? state.entry.name : "The firmware"} restarted (code ${code}).`);
      const was = state.entry;
      currentFlash().catch(() => null).then((bytes) => { if (state.entry === was) boot(bytes ? { name: saves.AUTOSAVE, bytes } : null); });
    },
    midiOut(bytes) { siteEmit("midiOut", bytes); if (editor) editor.bridge.fromEngine(bytes); },
    load: onLoad,
    rec(m) { if (rec) rec.onRec(m); else if (m.type === "recChunk") player.recBuf(m.l, m.r); },
    beforeLoad() { if (rec) rec.beforeLoad(); },
    error(msg) {
      console.warn("worklet:", msg);
      if (/^load:/.test(msg) && state.engine !== "mock") fallback(msg.replace(/^load:\s*/, "The engine could not start: ") + ".");
      else if (/^render:/.test(msg)) toast(`The engine stopped: ${msg}`, "bad");
    },
  });
  player.setVolume(state.volume);
}
let flashTimer = 0, pendingSave = null;
const lastSaved = new Map();
function quickHash(b) { let h = 2166136261; for (let i = 0; i < b.length; i += 7) h = Math.imul(h ^ b[i], 16777619); return (h >>> 0) + ":" + b.length; }
// the flash on request too: some cores only write it when asked (W1), so ask now and then, and when the page hides
setInterval(() => { if (state.running && !document.hidden && state.engine !== "mock" && state.info) player.flashReq(); }, 30000);
const flushFlash = () => { if (state.running && state.engine !== "mock" && player) player.flashReq(); };
document.addEventListener("visibilitychange", () => { if (document.hidden) flushFlash(); });
addEventListener("pagehide", flushFlash);
// the running flash, fresh (a request to the worklet)
const currentFlash = () => new Promise((resolve, reject) => {
  if (!player || state.engine === "mock") return reject(new Error("No firmware is running."));
  pendingSave = resolve;
  player.flashReq();
  setTimeout(() => { if (pendingSave === resolve) { pendingSave = null; reject(new Error("The engine did not answer.")); } }, 3000);
});

async function start() {
  if (state.running) return true;
  try {
    if (!player) makePlayer();
    const ok = await player.start();
    if (!ok) return false;
  } catch (e) { toast(e.message || String(e), "bad"); return false; }
  state.running = true;
  document.body.classList.add("running");
  $("start").hidden = true;
  if (state.entry) await boot();
  return true;
}

// ------------------------------------------------------------- my FM-1 ---
// the start overlay mirrors the link: "Play on my FM-1" while not connected, a line once it is
let fm1Tone = null;
function onFm1(v) {
  const b = $("start-fm1"), note = $("start-fm1-note");
  b.hidden = v.tone === "on";
  b.disabled = v.tone === "busy";
  b.querySelector(".fm1-dot").className = "fm1-dot " + v.tone + (v.live && v.tone !== "off" ? " live" : "");
  b.querySelector("span:last-child").textContent = v.tone === "busy" ? "Connecting your FM-1..." : "Play on my FM-1";
  b.title = v.title;
  note.hidden = v.tone !== "on" && v.tone !== "bad";
  note.className = "start-note " + v.tone;
  note.textContent = v.tone === "on" ? `Your FM-1 is connected${v.via ? " over " + v.via : ""}: press Play and it plays this firmware.` : v.detail;
  if (fm1Tone !== null && v.tone !== fm1Tone) {
    if (v.tone === "on") toast(playingSelf() ? selfLine("on") : `FM-1 connected${v.via ? " over " + v.via : ""}: its keys, knobs and screen are the panel.`, "ok", playingSelf() ? 6500 : 0);
    else if (v.tone === "bad") toast(v.detail, "bad");
  }
  fm1Tone = v.tone;
}
// one click: the sound and the unit (audio start, Web MIDI, the WebUSB chooser: the same user gesture)
function playOnFm1() {
  if (fm1 && !fm1.connected) fm1.connect();
  start();
}

// ------------------------------------------------------------------- MIDI ---
const midi = new MidiInput(onMidi, () => { if (sheet) sheet.render(); });
function onMidi(d) {
  if (!d || !d.length) return;
  const s = d[0] & 0xF0;
  if (state.notesPlayKeys && (s === 0x90 || s === 0x80) && d.length >= 3) {
    const k = d[1] - 53 - 12 * state.transpose, on = s === 0x90 && d[2] > 0;
    if (k >= 0 && k < 27) controls.holdKey(k, SRC.MIDI, on, on ? d[2] : 0);
    if (!state.reachFirmware) return;
  }
  if (state.reachFirmware || !state.notesPlayKeys) { if (player) player.midi(d); }
}

// -------------------------------------------------------------------- app ---
const app = {
  get state() { return state; },
  get catalog() { return catalog; },
  midi,
  installerUrl: "",
  remoteMounted: false,
  remoteStatus: "",
  versionOf, metaLine, engineNote, carryText, answerCarry, sourceNow,
  sheetChanged(open) { document.body.classList.toggle("sheet-open", open); if (open) document.body.classList.remove("chrome"); else $("stage").focus({ preventScroll: true }); },
  set(k, v) {
    if (k === "theme") { state.theme = presetNamed(v) || state.theme; prefs.theme = state.theme.name; applyTheme(); }
    else if (k === "view") { state.view = v; prefs.view = v; layout(); }
    else if (k === "keyHints") { state.keyHints = !!v; prefs.keyHints = !!v; device.setHints(state.keyHints); invalidate(); }
    else { state[k] = v; prefs[k] = v; }
    savePrefs();
    if (sheet) sheet.render();
  },
  // from the browser; engine: its filter's (Native / Emulated), picked as the sheet's engine choice is
  openFirmware: (id, engine = null) => {
    const e = findEntry(id);
    if (e && engine && e.engines.includes(engine)) {
      prefs.engine = { ...(prefs.engine || {}), [e.id]: engine }; prefs.engineAt = { ...(prefs.engineAt || {}), [e.id]: e.version }; savePrefs();
      return openFirmware(e, engine);
    }
    openFirmware(id);
  },
  // the page's volume: while the slider moves (no re-render), and kept when it is let go
  setVolume(v, keep) {
    state.volume = Math.max(0, Math.min(1, +v || 0));
    if (player) player.setVolume(state.volume);
    if (keep) { prefs.volume = state.volume; savePrefs(); }
  },
  setMhz(choice) {
    const e = state.entry;
    if (!e) return;
    Object.assign(prefs, pickMhz(prefs, e.id, choice)); savePrefs();
    const clk = mhzFor(e.id, prefs, HOST);
    const reload = clk.mhz !== state.mhz;
    state.mhzChoice = clk.choice;
    if (sheet) sheet.render();
    if (reload && state.engine === "emu") rebootKeepingFlash();
    else state.mhz = clk.mhz;
  },
  setEngine(name) {
    const e = state.entry;
    if (!e || !e.engines.includes(name)) return;
    prefs.engine = { ...(prefs.engine || {}), [e.id]: name }; prefs.engineAt = { ...(prefs.engineAt || {}), [e.id]: e.version }; savePrefs();
    openFirmware(e, name);
  },
  async enableMidi() { if (await midi.enable()) { if (!midi.selected) midi.select(prefs.midi || "*"); } },
  selectMidi(id) { midi.select(id); prefs.midi = id; savePrefs(); },
  async loadSave(rec) { state.selectedSave = rec.name; if (sheet) sheet.render(); if (state.running) await boot(rec); toast(`Started from "${rec.name}".`); },
  async saveAs(name) {
    try { const b = await currentFlash(); const e = state.entry; await saves.put(e.id, state.engine, versionOf(e, state.engine), name, b); state.selectedSave = name; await refreshSaves(); toast(`Saved "${name}".`, "ok"); }
    catch (err) { toast(err.message, "bad"); if (sheet) sheet.render(); }
  },
  async saveOver(rec) { if (rec) return app.saveAs(rec.name); },
  async renameSave(rec, to) { await saves.rename(rec.fw, rec.engine, rec.version, rec.name, to); state.selectedSave = to; await refreshSaves(); },
  async deleteSave(rec) { await saves.remove(rec.fw, rec.engine, rec.version, rec.name); state.selectedSave = null; await refreshSaves(); toast(`Deleted "${rec.name}".`); },
  exportSaves() {
    const e = state.entry, json = saves.backupJson(state.saves);
    const a = Object.assign(document.createElement("a"), { href: URL.createObjectURL(new Blob([json], { type: "application/json" })),
      download: `${e.id}-saves-${new Date().toISOString().slice(0, 10)}.json` });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  },
  async importSaves(file) {
    try {
      const buf = new Uint8Array(await file.arrayBuffer());
      if (buf[0] === 0x7B) { const n = await saves.importBackup(new TextDecoder().decode(buf)); toast(`Imported ${n} save${n === 1 ? "" : "s"}.`, "ok"); }
      else {   // a raw flash image: one save of the current firmware and engine
        const e = state.entry, name = file.name.replace(/\.[^.]+$/, "").slice(0, 40) || "Imported";
        await saves.put(e.id, state.wantEngine, versionOf(e, state.wantEngine), name, buf);
        state.selectedSave = name;
        toast(`Imported "${name}" as a save of ${e.name} (${state.wantEngine === "native" ? "native" : "emulated"}).`, "ok");
      }
      await refreshSaves();
    } catch (err) { toast(err.message || String(err), "bad"); }
  },
  async openLocalFile(file) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const id = "local-" + file.name.replace(/[^\w.-]+/g, "_");
    openFirmware({ id, name: file.name.replace(/\.fwsc$/i, ""), version: "file", author: "your file", authorFull: "a file from your computer",
      category: "Other", categories: ["Other"], tag: "your package", what: `${file.name}, ${bytes.length} bytes, from your computer.`,
      license: null, source: null, engines: ["emu"], defaultEngine: "emu", native: null, emu: { pkg: null }, local: true, localBytes: bytes }, "emu");
  },
};

function applyTheme() {
  device.setTheme(state.theme); screen.setTheme(state.theme);
  const p = derive(state.theme);
  document.documentElement.style.setProperty("--plate", css(p.plate));
  document.documentElement.style.setProperty("--backdrop", css(p.backdrop));
  invalidate();
}

// ------------------------------------------------------------------- boot ---
async function main() {
  $("corner").innerHTML = icon("dots");
  if (EMBED) document.body.classList.add("embed");
  applyTheme();
  // the FM-1 at its size from the first frame on (before any await: the catalog can take seconds on a slow connection)
  layout();
  addEventListener("resize", layout);
  new ResizeObserver(layout).observe($("stage"));
  matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener?.("change", layout);
  try {
    const r = await fetch("catalog.json", { cache: "no-cache" });
    snapshot = catalog = await r.json();
  } catch (e) {
    toast("The firmware list could not be loaded. Reload the page.", "bad");
    return;
  }
  // the Hub's catalog, live (its current versions and packages), merged over the deployed one; the deployed one if the
  // Hub does not answer within 4 s (web/engines/emu/catalog.mjs fetches it; without the emulated engine, no merge)
  try { emuHook = catalog.paths.engines.emu && catalog.paths.engines.emu.hook ? await import(new URL(catalog.paths.engines.emu.hook, document.baseURI).href) : null; }
  catch (_) { emuHook = null; }
  if (emuHook && typeof emuHook.fetchHubCatalog === "function") {
    catalog = await liveCatalog(snapshot, { emuSkip: emuHook.EMU_SKIP || {},
      fetchHub: (signal) => emuHook.fetchHubCatalog((u) => fetch(u, { signal })) });
    if (!catalog.live) console.warn("the Hub's catalog:", catalog.liveError, "- using the deployed snapshot");
  }
  // the hosted managers (web/managers/build.mjs: managers/index.json); none: the Editor button shows only links
  const mbase = new URL(catalog.paths.managers || "managers/", document.baseURI);
  let mindex = null;
  try { const r = await fetch(new URL("index.json", mbase), { cache: "no-cache" }); if (r.ok) mindex = await r.json(); } catch (_) { mindex = null; }
  editor = new EditorPanel({ getState: () => state, index: mindex, base: mbase.href, onLayout: layout,
    toEngine: (b) => { if (player && state.engine === "native") player.midi(b); }, setEngine: (n) => app.setEngine(n) });
  window.FM1Editor = editor;
  app.installerUrl = catalog.paths.installer;
  $("install-link").href = catalog.paths.installer;
  sheet = new Sheet(app);
  const made = makeSite({ controls, getState: () => state, installerUrl: catalog.paths.installer,
    setStatus: (t) => { app.remoteStatus = t; const el = sheet.remoteSlot.querySelector(".remote-status"); if (el) el.textContent = t; },
    midiIn: (b) => player && player.midi(b) });
  site = made.site; siteEmit = made.emit;
  window.FM1Site = site;
  import(new URL(catalog.paths.remote, document.baseURI).href).then((m) => {
    if (m && typeof m.RemoteFM1 === "function") {          // W4's class: the section is ours (use-fm1.js)
      app.remoteMounted = true;
      if (typeof m.isCandidatePortName === "function") midi.isUnitPort = m.isCandidatePortName;
      fm1 = createFm1(site, m, { onUnitPorts: (ports) => { midi.unitPorts = ports; midi.bind(); } });
      mountUseFm1(sheet.remoteSlot, fm1, site);
      mountHeader($("fm1-btn"), fm1, { openSheet: () => sheet.open() });
      fm1.on(onFm1);
      onFm1(fm1.view());
      sheet.render();
      fm1.auto();                                          // connects now if MIDI was allowed before (no prompt)
    } else if (m && typeof m.mount === "function") { app.remoteMounted = true; sheet.remoteSlot.replaceChildren(); m.mount(sheet.remoteSlot, site); sheet.render(); }
  }).catch(() => { /* (web/remote is W4's: the stub stays) */ });

  // the recorder
  if (!EMBED) {
    rec = new RecUI({
      getPlayer: () => player,
      ensureRunning: async () => state.running || start(),
      nameParts: () => ({ id: state.entry ? state.entry.id : "fm1", version: state.entry ? versionOf(state.entry, state.engine === "mock" ? state.wantEngine : state.engine) : "" }),
      settings: () => ({ bits: state.recBits, countIn: state.recCountIn }),
      onIdle: recIdle,
      toast,
    });
    window.FM1Rec = rec;
  }

  // input
  new Keyboard({
    controls,
    enabled: (e) => { if (sheet.isOpen && sheet.el.contains(e.target)) return false; if (!state.running) start(); return true; },
    onLcdView: () => app.set("view", state.view === "screen" ? "device" : "screen"),
    onActivity: activity,
  });
  attachPointer($("device"), device, controls, { onActivity: (e) => { activity(); if (!state.running) start(); }, onScreenTap: () => {} });
  $("screen").addEventListener("pointerdown", () => { activity(); if (!state.running) start(); });
  $("stage").addEventListener("pointermove", activity);
  $("corner").addEventListener("click", () => sheet.open());
  $("corner").addEventListener("mouseleave", activity);
  $("start-btn").addEventListener("click", () => start());
  $("start-fm1").addEventListener("click", playOnFm1);
  $("menu-btn").addEventListener("click", () => sheet.open());
  $("fw-btn").addEventListener("click", () => sheet.open("firmware"));
  $("offer-yes").addEventListener("click", () => answerCarry(true));
  $("offer-no").addEventListener("click", () => answerCarry(false));
  addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || !sheet.isOpen) return;
    e.stopImmediatePropagation(); e.preventDefault();
    if (sheet.inline) { sheet.inline = null; sheet.render(); } else if (sheet.page !== "main") sheet.show("main"); else sheet.close();
  }, true);

  // the firmware: the deep link, the last one, or the first native one
  const want = params.get("fw") || prefs.last;
  const first = (want && findEntry(want)) || findEntry("felucca") || catalog.firmwares[0];
  if (params.get("fw") && !findEntry(params.get("fw"))) toast(`No firmware "${params.get("fw")}": showing ${first.name}.`, "bad");
  layout();
  await openFirmware(first, params.get("engine"));
  if (await player_autostart()) return;
  $("start").hidden = false;
}
// an AudioContext that may run without a gesture (an embed in an active page, a test browser) starts at once
async function player_autostart() {
  if (!player) makePlayer();
  if (await player.tryAutostart()) return start();
  return false;
}
main();
