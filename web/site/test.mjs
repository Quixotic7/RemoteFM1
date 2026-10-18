// SPDX-License-Identifier: GPL-3.0-only
// The site's logic in Node: the catalog, the panel's orders (label / role -> matrix), the worklet host's panel merge
// (taps), the computer-key map, and the mock engine against web/engine/API.md.
//   node web/site/test.mjs
import { readFileSync, existsSync } from "node:fs";
import { Controls, labelToMatrix, roleToMatrix, B, E, SRC, KEYMAP, KM, hintText, ledIndexOf, DEFAULT_KEYMAP } from "./js/panel.js";
import { PanelMerge } from "./worklet/host.js";
import { createEngine, ENGINE } from "./mock/engine.js";
import { derive, PRESETS, contrast } from "./js/theme.js";
import { Fm1Link, viewOf } from "./js/fm1-link.js";

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(76)} ${c ? "ok" : "FAIL"}`); };

// ---- the catalog
const cat = JSON.parse(readFileSync(new URL("./catalog.json", import.meta.url), "utf8"));
const natives = cat.firmwares.filter((f) => f.native).map((f) => f.id).sort();
ok(natives.join() === "amb,coralflower,felucca,fimba,fomni,frodhes,ghoulbox,jiant,obervave,purplemonkey", "catalog: the 10 un-gated cores are native, no other");
ok(cat.firmwares.every((f) => f.engines.includes("emu") && /^https:\/\/thefirmwarehub\.com\/fw\//.test(f.emu.pkg) && /^[0-9a-f]{64}$/.test(f.emu.sha256)),
   "catalog: every firmware runs emulated, its package from the Hub with a SHA-256");
ok(new Set(cat.firmwares.map((f) => f.id)).size === cat.firmwares.length, "catalog: ids unique");
ok(cat.firmwares.filter((f) => f.native).every((f) => f.defaultEngine === "native" && f.native.wasm.endsWith(`${f.native.core}.wasm`)), "catalog: native is the default where it exists");
ok(["choralroot", "melodee", "x0x", "orbit"].every((id) => cat.firmwares.find((f) => f.id === id).nativeGated), "catalog: gated cores say why they are not native");
ok(cat.firmwares.every((f) => cat.categories.includes(f.category)), "catalog: every category is a chip");

// ---- orders
ok([...Array(14).keys()].map(labelToMatrix).join() === "2,3,4,5,6,7,8,9,10,11,12,13,0,1", "buttons: label -> matrix (OCT- 0, OCT+ 1, FX 2 .. REC 13)");
ok([0, 1, 2, 3, 4, 5, 6].map(roleToMatrix).join() === "0,1,6,2,3,4,5", "encoders: role -> matrix (SELECT ALGO K1..K4 PRESETS)");
const led = ledIndexOf(DEFAULT_KEYMAP);
ok(led[14] === 3 * 4 + 3 && led[13] === 7 * 4 + 1 && led[0] === 3, "LEDs: key 0 at (column 3, line 3), REC at (7, 1), OCT- at (0, 3)");

// ---- controls -> the engine's panel
let msgs = [];
const c = new Controls((m) => msgs.push(m));
c.holdKey(0, SRC.KEY, true); c.holdKey(26, SRC.POINTER, true, 90);
c.holdBtn(B.FX, SRC.KEY, true); c.holdBtn(B.OCTDN, SRC.KEY, true);
c.turn(E.PRESETS, 3); c.turn(E.K1, -2);
const last = msgs.at(-1);
ok(last.keys === ((1 | (1 << 26)) >>> 0) && msgs[1].keyVel[26] === 90, "controls: keys as bits, a velocity kept");
ok(last.buttons === ((1 << 2) | 1), "controls: FX -> matrix 2, OCT- -> matrix 0");
ok(msgs.at(-2).enc[6] === 3 && last.enc[2] === -2 && last.enc[6] === 0, "controls: encoder deltas once each, matrix order");
c.holdKey(0, SRC.REMOTE, true); c.holdKey(0, SRC.KEY, false);
ok(msgs.at(-1).keys & 1, "controls: a key held by two sources stays down until both let go");
c.releaseSrc(SRC.REMOTE);
ok(!(msgs.at(-1).keys & 1), "controls: releasing a source lets its keys go");

// ---- host: down and up between two quanta is a tap
const pm = new PanelMerge();
pm.add({ keys: 4, buttons: 0, master: 700 }); pm.add({ keys: 0, buttons: 0 });
let p = pm.take();
ok(p.keys === 0 && p.taps && p.taps.keys === 4 && p.master === 700, "host: a key pressed and released within a quantum arrives as a tap");
pm.add({ keys: 4, buttons: 0 }); p = pm.take(); pm.add({ keys: 0, buttons: 0 }); p = pm.take();
ok(!p.taps, "host: a key the engine saw down is a plain release, not a tap");

// ---- keymap (FM1VST's)
ok(hintText(KM.KEY, 8) === "F4/W" && hintText(KM.BTN, B.FX) === "F5/C" && hintText(KM.KEY, 7) === "5", "keymap: the panel's hints as FM1VST prints them");
ok(new Set(KEYMAP.map((e) => e.code)).size === KEYMAP.length, "keymap: one action per physical key");

// ---- themes stay legible
ok(PRESETS.every((t) => { const p = derive(t); return contrast(p.capText, p.cap) >= 2.9 && contrast(p.hint, p.cap) >= 1.9; }), "themes: cap text and hints readable on every preset");

// ---- the mock engine, as API.md says
const eng = await createEngine({ sampleRate: 48000, files: {}, flash: null, options: { name: "Test" } });
const info = eng.info();
ok(ENGINE.name === "mock" && info.id && "latencyFrames" in info && "keymap" in info && "buttonNames" in info, "mock: info() has the API's fields");
const L = new Float32Array(128), R = new Float32Array(128);
let s0 = eng.screen().seq, peak = 0;
eng.setPanel({ keys: 1 << 7, buttons: 0, enc: new Int32Array(7), master: 900, keyVel: new Uint8Array(27) });
for (let i = 0; i < 400; i++) { eng.render(L, R); for (const v of L) peak = Math.max(peak, Math.abs(v)); }
ok(peak > 0.01, "mock: a held key sounds");
ok(eng.screen().pixels.length === 240 * 240 && eng.screen().seq !== s0 && eng.screen().pixels.some((v) => v !== 0), "mock: a 240x240 screen that is not blank and moves");
ok(eng.leds().levels.length === 44 && eng.leds().levels[led[14 + 7]] === 255, "mock: the held key's LED lit");
const f0 = eng.flash().seq;
eng.setPanel({ keys: 0, buttons: 1 << 9, enc: new Int32Array(7), master: 900 });
ok(eng.flash().seq !== f0 && eng.flash().bytes[4] === 1, "mock: SAVE changes the flash");
eng.midiIn(Uint8Array.from([0x90, 60, 100]));
ok(eng.midiOut().length === 3 && eng.midiOut().length === 0, "mock: MIDI in echoes notes out once");
ok(eng.halted() === 0, "mock: not halted");
eng.dispose();

// ---- the start overlay's key hint: the panel's own letters (FM1VST's map), every one of them a note key
const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
const hint = [...(/<div id="start"[\s\S]*?<\/div>\s*<div id="toast"/.exec(html) || [""])[0].matchAll(/<kbd>([^<]+)<\/kbd>/g)].map((m) => m[1]);
const notes = hint.slice(0, -2), octave = hint.slice(-2);
ok(notes.join("") === "SDFGHJKL" && notes.every((k) => KEYMAP.some((e) => e.code === "Key" + k && e.kind === KM.KEY && hintText(KM.KEY, e.idx) === k))
   && octave.join() === "Z,X", "start overlay: the key hint shows the panel's own letters (S D F G H J K L, Z X)");

// ---- my FM-1: what the header, the overlay and the sheet show (fm1-link.js)
const V = (st) => viewOf({ state: "idle", ...st });
ok(V({}).tone === "off" && V({}).label === "Connect FM-1", "fm1: idle -> grey, Connect FM-1");
ok(V({ state: "searching", quiet: true }).tone === "off", "fm1: a quiet search (auto-connect) stays grey");
ok(V({ state: "searching" }).tone === "busy" && V({ state: "starting" }).tone === "busy" && V({ state: "searching" }).live, "fm1: a click's search, starting -> amber, pulsing");
const vOn = V({ state: "connected", transport: "bulk", unit: { fwVer: "0.3" } });
ok(vOn.tone === "on" && vOn.label === "FM-1 connected" && vOn.via === "USB" && V({ state: "connected", transport: "midi" }).via === "MIDI", "fm1: connected -> green, FM-1 connected, USB / MIDI");
const vErr = V({ state: "error", why: "MIDI access (with SysEx) was refused" });
ok(vErr.tone === "bad" && /allow MIDI/.test(vErr.title) && /try again/.test(vErr.title), "fm1: an error -> red, the reason (and the retry) in the tooltip");
ok(viewOf({ state: "searching" }, true).tone === "bad", "fm1: nothing answered a click -> red");

{ // the link over a stand-in remote: a click's search that finds nothing turns red and goes on quietly
  const ls = new Set(), calls = [];
  let st = { state: "idle" }, timer = null;
  const fake = { get status() { return st; }, onStatus: (cb) => ls.add(cb), connect: async (o) => { calls.push(["connect", o]); push({ state: "searching" }); },
    disconnect: async () => { calls.push(["disconnect"]); push({ state: "idle" }); }, setQuiet: (q) => { calls.push(["quiet", q]); push({ ...st, quiet: q }); },
    autoConnect: async () => { calls.push(["auto"]); return { midi: "granted" }; } };
  const push = (s) => { st = s; for (const cb of ls) cb(s); };
  const link = new Fm1Link(fake, { searchMs: 10000, setTimer: (f) => (timer = f), clearTimer: () => { timer = null; } });
  const seen = [];
  link.on((v) => seen.push(v.tone));
  await link.auto();
  link.connect();
  ok(calls[0][0] === "auto" && calls[1][0] === "connect" && calls[1][1] === undefined && link.view().tone === "busy", "fm1 link: auto() at load; a click connects (with its gesture) -> amber");
  timer();
  ok(link.view().tone === "bad" && calls.some((c) => c[0] === "quiet" && c[1] === true), "fm1 link: no answer in 10 s -> red, the search goes on quietly");
  push({ state: "starting", quiet: true }); push({ state: "connected", transport: "midi", quiet: true });
  ok(link.view().tone === "on" && !link.notFound, "fm1 link: the unit answers later -> green");
  link.disconnect();
  ok(link.view().tone === "off" && seen.includes("busy") && seen.includes("bad") && seen.includes("on"), "fm1 link: disconnect -> grey; every state reached the subscribers");
}

// ---- my FM-1 with the real remote.js and libremotefm1 (WASM) against fake units: auto-connect at load
const BUILT = new URL("../build/remote/", import.meta.url);
if (!existsSync(new URL("remotefm1.mjs", BUILT)) || !existsSync(new URL("fakeunit.mjs", BUILT))) {
  console.log("fm1 auto-connect with remote.js: skipped (web/remote/build.sh not run)");
} else {
  const { default: createRemoteFM1 } = await import(new URL("remotefm1.mjs", BUILT).href);
  const { default: createFakeUnit } = await import(new URL("fakeunit.mjs", BUILT).href);
  const { RemoteFM1 } = await import("../remote/remote.js");
  const { createWorld } = await import("../remote/test/fakeworld.mjs");
  const settle = () => new Promise((r) => setImmediate(r));
  async function page(perm, build) {
    const w = await createWorld(createFakeUnit, { start: 3 });
    w.midiPerm = perm;
    w.keystep = w.addOther("Arturia KeyStep 37");
    w.iac = w.addOther("IAC Driver Bus 1");
    if (build) build(w);
    const remote = new RemoteFM1({ navigator: w.navigator, now: () => w.t, timers: false, module: createRemoteFM1 });
    const link = new Fm1Link(remote, { setTimer: () => 0, clearTimer: () => {} });
    const run = async (ms) => { for (let i = 0; i < ms; i++) { w.step(1); remote.tick(); if ((i & 3) === 3) await settle(); } await settle(); };
    await link.auto();
    return { w, remote, link, run };
  }
  const quietOthers = (w) => w.keystep.bytes === 0 && w.iac.bytes === 0;
  {
    const { w, link, run } = await page("granted", (w) => { w.unit = w.addUnit({ midiName: "FM-1", bulk: true }); });
    await run(2000);
    ok(link.view().tone === "on" && link.view().via === "MIDI" && w.requestDeviceFilters.length === 0 && quietOthers(w),
       "fm1 auto: MIDI granted -> connected at load (MIDI), no requestDevice, nothing to other ports");
    await Promise.all([link.disconnect(), run(400)]);
  }
  {
    const { w, link, run } = await page("granted", (w) => { w.unit = w.addUnit({ midiName: "FM-1", bulk: true }); w.permitted.add(w.unit.usb); });
    await run(2000);
    ok(link.view().tone === "on" && link.view().via === "USB" && w.requestDeviceFilters.length === 0, "fm1 auto: MIDI granted, USB paired before -> connected over USB at load");
    await Promise.all([link.disconnect(), run(400)]);
  }
  for (const perm of ["prompt", "denied"]) {
    const { w, link, run } = await page(perm, (w) => { w.unit = w.addUnit({ midiName: "FM-1", bulk: true }); });
    await run(3000);
    ok(link.view().tone === "off" && w.midiRequests.length === 0 && w.requestDeviceFilters.length === 0 && w.unit.sent.length === 0 && quietOthers(w),
       `fm1 auto: MIDI ${perm} -> grey Connect FM-1; nothing asked, nothing sent`);
    if (perm === "prompt") {
      link.connect();                                         // the header's click
      await run(1500);
      ok(w.midiRequests.length === 1 && w.requestDeviceFilters.length === 1 && link.view().tone === "on", "fm1 click: asks for MIDI and opens the WebUSB chooser, then connected");
      await Promise.all([link.disconnect(), run(400)]);
    }
  }
  {
    const { w, link, run } = await page("granted");
    await run(3000);
    ok(link.view().tone === "off" && quietOthers(w), "fm1 auto: no FM-1 plugged in -> grey, nothing sent to the KeyStep or IAC");
    const stock = w.addUnit({ midiName: "FM-1", name: "FM-1_015", proto: 1 });
    w.plugIn(stock);
    await run(8000);
    const n = stock.cmds.filter((c) => c === 1).length;
    ok(link.view().tone === "off" && n >= 3 && n <= 5 && stock.cmds.every((c) => c === 1), `fm1 auto: a stock FM-1 plugged in -> stays grey; ${n} HELLOs in 8 s, nothing else`);
    w.unplug(stock); await run(2500);
    const unit = w.addUnit({ midiName: "ChoralRoot FM-1" });
    w.plugIn(unit);
    await run(2500);
    ok(link.view().tone === "on" && quietOthers(w) && w.requestDeviceFilters.length === 0, "fm1 auto: a RemoteFM1 unit plugged in later -> connected, no requestDevice");
    await Promise.all([link.disconnect(), run(400)]);
  }
}

console.log(fails ? `site: ${fails} of ${checks} FAILED` : `site tests passed (${checks})`);
process.exit(fails ? 1 : 0);
