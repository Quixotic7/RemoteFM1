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

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const EMBED = params.get("embed") === "1";

// ------------------------------------------------------------------ prefs ---
const PREF_KEY = "remotefm1-player";
const prefs = (() => { try { return JSON.parse(localStorage.getItem(PREF_KEY)) || {}; } catch (_) { return {}; } })();
const savePrefs = () => { try { localStorage.setItem(PREF_KEY, JSON.stringify(prefs)); } catch (_) { /* (private mode) */ } };

// ------------------------------------------------------------------ state ---
const state = {
  entry: null, engine: null, wantEngine: null, engineState: "idle", info: null, running: false,
  theme: presetNamed(prefs.theme) || presetNamed(DEFAULT_THEME),
  view: params.get("view") === "screen" ? "screen" : params.get("view") === "device" ? "device" : prefs.view || "device",
  transpose: prefs.transpose | 0, notesPlayKeys: prefs.notesPlayKeys !== false, reachFirmware: !!prefs.reachFirmware,
  saves: [], selectedSave: null,
  pixels: null, frameSeq: 0, levels: new Uint8Array(44), playGreen: 0,
};
let catalog = null, player = null, sheet = null, site = null, siteEmit = () => {}, fm1 = null;
const lcd = new Lcd();
const controls = new Controls((p) => { if (player) player.panel(p); });
const device = new DeviceView($("device"), controls, lcd);
const screen = new ScreenView($("screen"), lcd);

// ------------------------------------------------------------------ drawing ---
let drawQueued = false;
const invalidate = () => { if (!drawQueued) { drawQueued = true; requestAnimationFrame(draw); } };
controls.onDraw = invalidate;
function draw() {
  drawQueued = false;
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

function engineFor(e, want) {
  if (want && e.engines.includes(want)) return want;
  const pref = (prefs.engine || {})[e.id];
  return pref && e.engines.includes(pref) ? pref : e.defaultEngine;
}

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
async function boot(fromSave = null) {
  const e = state.entry, eng = state.wantEngine;
  if (!e || !player) return;
  const ver = versionOf(e, eng);
  const rec = fromSave || (await saves.get(e.id, eng, ver, saves.AUTOSAVE));
  state.engine = eng; state.engineState = "loading"; state.info = null;
  setStatus(`Loading ${e.name}...`);
  if (sheet) sheet.render();
  try {
    await player.load(e, eng, rec ? rec.bytes : null);
  } catch (err) {
    const msg = err.message || String(err);
    await fallback(/worklet|module/i.test(msg) ? `The ${eng === "native" ? "native" : "emulated"} engine is not on this site yet.` : msg);
  }
}
async function fallback(why) {
  const e = state.entry;
  console.warn("engine:", why);
  state.engine = "mock"; state.engineState = "mock";
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
  const eng = state.engine === "mock" ? "test engine (the real one is not here yet)" : state.engine === "native" ? "native" : "emulated";
  return `${e.category} · ${e.tag} · ${e.author} · ${eng}`;
}
function engineNote() {
  const e = state.entry;
  if (!e) return "";
  const n = e.native ? `Native: FM1VST's ${e.name} core ${e.native.version}, compiled to WebAssembly; starts at once.` : null;
  const m = `Emulated: the firmware's own package ${e.version} on fm1-emu, the FM-1's chip in the browser; heavier on the CPU.`;
  const now = state.engine === "mock" ? `Now: a test engine, because the ${state.wantEngine === "native" ? "native" : "emulated"} engine could not start.` : "";
  const credit = state.engine === "emu" ? catalog.credits.fm1emu.line + "." : "";
  return [n, m, now, credit].filter(Boolean).join(" ");
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
    halted(code) { toast(`${state.entry ? state.entry.name : "The firmware"} restarted (code ${code}).`); boot(); },
    midiOut(bytes) { siteEmit("midiOut", bytes); },
    error(msg) {
      console.warn("worklet:", msg);
      if (/^load:/.test(msg) && state.engine !== "mock") fallback(msg.replace(/^load:\s*/, "The engine could not start: ") + ".");
      else if (/^render:/.test(msg)) toast(`The engine stopped: ${msg}`, "bad");
    },
  });
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
  versionOf, metaLine, engineNote,
  sheetChanged(open) { document.body.classList.toggle("sheet-open", open); if (open) document.body.classList.remove("chrome"); else $("stage").focus({ preventScroll: true }); },
  set(k, v) {
    if (k === "theme") { state.theme = presetNamed(v) || state.theme; prefs.theme = state.theme.name; applyTheme(); }
    else if (k === "view") { state.view = v; prefs.view = v; layout(); }
    else { state[k] = v; prefs[k] = v; }
    savePrefs();
    if (sheet) sheet.render();
  },
  openFirmware: (id) => { openFirmware(id); },
  setEngine(name) {
    const e = state.entry;
    if (!e || !e.engines.includes(name)) return;
    prefs.engine = { ...(prefs.engine || {}), [e.id]: name }; savePrefs();
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
  try {
    const r = await fetch("catalog.json", { cache: "no-cache" });
    catalog = await r.json();
  } catch (e) {
    toast("The firmware list could not be loaded. Reload the page.", "bad");
    return;
  }
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
  addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || !sheet.isOpen) return;
    e.stopImmediatePropagation(); e.preventDefault();
    if (sheet.inline) { sheet.inline = null; sheet.render(); } else if (sheet.page !== "main") sheet.show("main"); else sheet.close();
  }, true);
  addEventListener("resize", layout);
  new ResizeObserver(layout).observe($("stage"));
  matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener?.("change", layout);

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
