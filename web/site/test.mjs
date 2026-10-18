// SPDX-License-Identifier: GPL-3.0-only
// The site's logic in Node: the catalog, the panel's orders (label / role -> matrix), the worklet host's panel merge
// (taps), the computer-key map, and the mock engine against web/engine/API.md.
//   node web/site/test.mjs
import { readFileSync } from "node:fs";
import { Controls, labelToMatrix, roleToMatrix, B, E, SRC, KEYMAP, KM, hintText, ledIndexOf, DEFAULT_KEYMAP } from "./js/panel.js";
import { PanelMerge } from "./worklet/host.js";
import { createEngine, ENGINE } from "./mock/engine.js";
import { derive, PRESETS, contrast } from "./js/theme.js";

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

console.log(fails ? `site: ${fails} of ${checks} FAILED` : `site tests passed (${checks})`);
process.exit(fails ? 1 : 0);
