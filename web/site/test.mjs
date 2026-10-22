// SPDX-License-Identifier: GPL-3.0-only
// The site's logic in Node: the catalog, following the Hub's releases (live merge, versions, newest wins, saves
// carried to a new version), the emulated CPU clock, the browser's engine filter, the panel's orders (label / role -> matrix), the worklet host's panel merge
// (taps), the computer-key map, and the mock engine against web/engine/API.md.
//   node web/site/test.mjs
import { readFileSync, existsSync, unlinkSync } from "node:fs";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
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
ok(natives.join() === "amb,choralroot,coralflower,felucca,fimba,fomni,frodhes,ghoulbox,jiant,loom,melodee,obervave,purplemonkey", "catalog: the 13 un-gated cores are native, no other");
ok(cat.firmwares.filter((f) => !f.offHub).every((f) => f.engines.includes("emu") && /^[0-9a-f]{64}$/.test(f.emu.sha256)
   && (f.id === "remotefm1" ? f.emu.from === "site" : /^https:\/\/thefirmwarehub\.com\/fw\//.test(f.emu.pkg) && !f.emu.from)),
   "catalog: every Hub firmware runs emulated, its package from the Hub (RemoteFM1's from the site) with a SHA-256");
{
  const lm = cat.firmwares.find((f) => f.id === "loom"), hubIds = new Set(JSON.parse(readFileSync(new URL("./data/hub-catalog.snapshot.json", import.meta.url), "utf8")).firmwares.map((f) => f.id));
  ok(cat.firmwares.filter((f) => f.offHub).map((f) => f.id).join() === "loom" && !hubIds.has("loom"), "catalog: LOOM-8, not on the Hub, the one off-Hub entry");
  ok(lm && lm.name === "LOOM-8" && lm.engines.join() === "native" && lm.defaultEngine === "native" && !lm.nativeBehind && lm.emu === null && !lm.nativeGated
     && lm.native.version === "0.6-beta" && lm.native.wasm.endsWith("loom.wasm") && lm.native.glue === "fm1base" && lm.authorFull === "Charles Vestal"
     && lm.category === "Groovebox" && lm.what && lm.beta && lm.repo === "charlesvestal/fm1-loom",
     "catalog: LOOM-8 native only (no package to emulate), a beta, by Charles Vestal");
}
ok(new Set(cat.firmwares.map((f) => f.id)).size === cat.firmwares.length, "catalog: ids unique");
ok(cat.firmwares.filter((f) => f.native).every((f) => f.defaultEngine === (f.nativeBehind ? "emu" : "native") && f.native.wasm.endsWith(`${f.native.core}.wasm`)),
   "catalog: native is the default where it exists, unless the Hub's version is newer (newest wins)");
ok(["x0x", "orbit"].every((id) => cat.firmwares.find((f) => f.id === id).nativeGated), "catalog: gated cores say why they are not native");
ok(["choralroot", "melodee"].every((id) => { const f = cat.firmwares.find((x) => x.id === id); return f.native && !f.nativeGated && f.engines.join() === "native,emu" && f.defaultEngine === "native"; }),
   "catalog: ChoralRoot and Melodee play native by default, emulated (the Hub's package) one switch away");
ok(cat.firmwares.every((f) => cat.categories.includes(f.category)), "catalog: every category is a chip");

// ---- RemoteFM1 itself, a firmware to play: its released package, from this site (not the Hub), checked
{
  const rel = JSON.parse(readFileSync(new URL("../installer/releases.json", import.meta.url), "utf8")), r0 = rel.releases[0];
  const rf = cat.firmwares.find((f) => f.id === "remotefm1");
  ok(rf && rf.name === "RemoteFM1" && rf.version === r0.version && rf.author === "Quixotic7" && rf.license === "GPL-3.0-only"
     && rf.source === "https://github.com/Quixotic7/RemoteFM1" && rf.category === "Other", "remotefm1: in the catalog, the first release's version, author, licence, source, Other");
  ok(rf.engines.join() === "emu" && rf.defaultEngine === "emu" && !rf.native && rf.badge === "meta" && rf.blurb, "remotefm1: emulated only, a meta badge and a blurb");
  const pageUrl = "https://quixotic7.github.io/RemoteFM1/site/", pkgUrl = new URL(rf.emu.pkg, pageUrl);
  ok(!/^[a-z][\w+.-]*:|^\/\//i.test(rf.emu.pkg) && pkgUrl.origin === new URL(pageUrl).origin && rf.emu.pkg === cat.paths.installer + `firmware/remote-${r0.version}.fwsc`,
     `remotefm1: its package is same-origin, the installer's (${rf.emu.pkg})`);
  const file = new URL(`../installer/firmware/remote-${r0.version}.fwsc`, import.meta.url), bytes = readFileSync(file);
  const sha = createHash("sha256").update(bytes).digest("hex");
  ok(rf.emu.sha256 === r0.sha256 && sha === r0.sha256 && rf.emu.size === bytes.length, "remotefm1: SHA-256 and size are releases.json's and the package file's");
  // the published layout: the page at the root, the installer next to it
  const tmp = join(tmpdir(), `catalog-pages-${process.pid}.json`);
  execFileSync(process.execPath, [fileURLToPath(new URL("./tools/build_catalog.mjs", import.meta.url)), "--layout", "pages", "--out", tmp], { stdio: "ignore" });
  const pc = JSON.parse(readFileSync(tmp, "utf8")), prf = pc.firmwares.find((f) => f.id === "remotefm1");
  unlinkSync(tmp);
  ok(new URL(prf.emu.pkg, "https://quixotic7.github.io/RemoteFM1/").href === `https://quixotic7.github.io/RemoteFM1/installer/firmware/remote-${r0.version}.fwsc`
     && pc.firmwares.filter((f) => f.id !== "remotefm1" && !f.offHub).every((f) => f.emu.pkg.startsWith("https://thefirmwarehub.com/fw/")),
     "remotefm1: Pages layout -> <site>/installer/firmware/remote-X.fwsc; every other package still the Hub's");
  // the emu hook fetches a relative URL against the page and checks its SHA-256
  const { fetchPackage } = await import("../engines/emu/catalog.mjs");
  const asked = [];
  const fakeFetch = async (u) => { asked.push(u); return { ok: true, status: 200, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length) }; };
  const got = await fetchPackage({ id: rf.id, name: rf.name, package: { url: rf.emu.pkg, sha256: rf.emu.sha256 } }, fakeFetch, "http://127.0.0.1:8731/site/");
  let bad = null;
  try { await fetchPackage({ id: rf.id, name: rf.name, package: { url: rf.emu.pkg, sha256: "0".repeat(64) } }, fakeFetch, "http://127.0.0.1:8731/site/"); } catch (e) { bad = e.message; }
  ok(asked[0] === `http://127.0.0.1:8731/installer/firmware/remote-${r0.version}.fwsc` && got.bytes.length === bytes.length && /SHA-256/.test(bad || ""),
     "remotefm1: the emu hook fetches it same-origin and refuses a wrong SHA-256");
}

// ---- following the Hub's releases (js/live.js): the version comparator, newest wins, the live merge, gone packages
{
  const L = await import("./js/live.js");
  const C = L.compareVersions;
  const cases = [
    ["1.6.2", "1.6.1", 1], ["1.6.1", "1.6.1", 0], ["1.6", "1.6.0", 0], ["1.10", "1.9", 1], ["v1.2", "1.2", 0],
    ["0.2.20-beta", "0.2.20", -1], ["0.2.21-beta", "0.2.20", 1], ["0.2.20-beta", "0.2.20-beta2", -1], ["1.0.5RC", "1.0.5", -1], ["1.0.5RC", "1.0.5-beta", 1],
    ["Beta4", "Beta3", 1], ["Beta4", "Beta4", 0], ["1.1.5-rb", "1.1.5-rb", 0], ["1.1.6-rb", "1.1.5-rb", 1],
    ["0.8.5-c524e3c", "0.8.5-c524e3c", 0], ["0.9.10-53173f3", "0.8.5", 1], ["2026.10.17", "2026.10.9", 1], ["2026.10.17", "2026.10.17", 0],
    ["2.4.2+804c7785", "2.4.2", 0], ["1.5.1", "1.5", 1], ["0.9-salt14", "0.9-salt13", 1], ["2.5-b28-test", "2.5", -1],
  ];
  const bad = cases.filter(([a, b, w]) => C(a, b) !== w || C(b, a) !== -w || (w === 0 && C(b, a) !== 0));
  ok(!bad.length, `versions: ${cases.length} ordered pairs (dotted, -beta, RC, Beta4, -rb, a hash, dates, +build)${bad.length ? " " + JSON.stringify(bad) : ""}`);
  const unsure = [["0.8.5-c524e3c", "0.8.5"], ["1.1.5-rb", "1.1.5"], ["Beta4", "1.0"], ["1.0-fbc56d3", "1.0-b77b159"], ["1.5.1_and_sequences", "1.5.1"]];
  ok(unsure.every(([a, b]) => Number.isNaN(C(a, b))) && unsure.every(([a, b]) => L.hubNewer(a, b)) && !L.hubNewer("1.6.1", "1.6.1") && !L.hubNewer("1.6.0", "1.6.1"),
     "versions: unsure ones (a hash, a tag, Beta4 vs 1.0) are NaN and count as the Hub's newer; equal or older do not");

  // newest wins
  const nat = (v, hv) => L.newestWins({ id: "felucca", version: hv, engines: ["native", "emu"], native: { core: "felucca", version: v }, defaultEngine: "native" });
  ok(nat("1.6.1", "1.6.2").defaultEngine === "emu" && nat("1.6.1", "1.6.2").nativeBehind && nat("1.6.1", "1.6.1").defaultEngine === "native" && !nat("1.6.1", "1.6.1").nativeBehind,
     "default engine: the Hub's 1.6.2 over native 1.6.1 -> emu; equal -> native");
  const fe = nat("1.6.1", "1.6.2");
  ok(L.chooseEngine(fe, "native", {}) === "native" && L.chooseEngine(fe, null, {}) === "emu"
     && L.chooseEngine(fe, null, { engine: { felucca: "native" } }) === "emu"
     && L.chooseEngine(fe, null, { engine: { felucca: "native" }, engineAt: { felucca: "1.6.2" } }) === "native"
     && L.chooseEngine(fe, null, { engine: { felucca: "native" }, engineAt: { felucca: "1.6.1" } }) === "emu"
     && L.chooseEngine(nat("1.6.1", "1.6.1"), null, { engine: { felucca: "emu" } }) === "emu",
     "default engine: ?engine=native forces native; a choice made at an older release yields to the newest");

  // the merge
  const snap = cat, hubSnap = JSON.parse(readFileSync(new URL("./data/hub-catalog.snapshot.json", import.meta.url), "utf8"));
  const hub = JSON.parse(JSON.stringify(hubSnap));
  const hf = hub.firmwares.find((f) => f.id === "felucca");
  Object.assign(hf, { version: "1.6.2", pkg: "fw/felucca-1.6.2.fwsc", sha256: "a".repeat(64), size: 123 });
  hub.firmwares = hub.firmwares.filter((f) => f.id !== "sloop" && f.id !== "amb");     // left the Hub: an emulated one, a native one
  hub.firmwares.push({ id: "newfw", name: "Newfw", kind: ["synth"], author: "Someone (Somewhere)", what: "A new synth: very new.", version: "0.1", pkg: "fw/newfw-0.1.fwsc", sha256: "b".repeat(64), size: 9 });
  hub.firmwares.push({ id: "remotefm1", name: "Impostor", version: "9.9", pkg: "fw/remotefm1-9.9.fwsc", sha256: "c".repeat(64) });
  const m = L.mergeLive(snap, hub, {});
  const mf = m.firmwares.find((f) => f.id === "felucca"), old = snap.firmwares.find((f) => f.id === "felucca");
  ok(m.live && mf.version === "1.6.2" && mf.emu.pkg === "https://thefirmwarehub.com/fw/felucca-1.6.2.fwsc" && mf.emu.sha256 === "a".repeat(64)
     && mf.native.wasm === old.native.wasm && mf.native.version === old.native.version && mf.defaultEngine === "emu" && mf.nativeBehind
     && mf.snapshot.version === old.version && mf.snapshot.emu.pkg === old.emu.pkg && mf.category === old.category && mf.tag === old.tag,
     "merge: a newer Hub version -> its version, package and SHA-256; native data kept; default emu; the snapshot's kept for a 404");
  const n = m.firmwares.find((f) => f.id === "newfw");
  ok(n && n.engines.join() === "emu" && n.author === "Someone" && n.category === "Synth" && n.tag === "New synth" && n.emu.pkg.endsWith("fw/newfw-0.1.fwsc"), "merge: a firmware new on the Hub appears, emulated");
  const amb = m.firmwares.find((f) => f.id === "amb");
  ok(!m.firmwares.some((f) => f.id === "sloop") && amb && amb.engines.join() === "native" && amb.defaultEngine === "native" && amb.offHub,
     "merge: a firmware that left the Hub disappears, unless native (then native only)");
  const lm = m.firmwares.find((f) => f.id === "loom");
  ok(lm && lm.offHub && lm.engines.join() === "native" && lm.defaultEngine === "native" && !lm.emu, "merge: LOOM-8, never on the Hub, stays native only");
  const rf = m.firmwares.find((f) => f.id === "remotefm1");
  ok(rf === snap.firmwares.find((f) => f.id === "remotefm1") && m.firmwares.filter((f) => f.id === "remotefm1").length === 1, "merge: RemoteFM1's own entry stays as deployed (same-origin package)");
  const same = L.mergeLive(snap, hubSnap, {});
  ok(same.firmwares.length === snap.firmwares.length && same.firmwares.every((f) => { const o = snap.firmwares.find((x) => x.id === f.id); return o && o.version === f.version && (o.emu && o.emu.pkg) === (f.emu && f.emu.pkg) && o.defaultEngine === f.defaultEngine && !f.snapshot; }),
     "merge: the snapshot's own Hub catalog changes nothing");

  // the Hub down / slow -> the snapshot
  const down = await L.liveCatalog(snap, { fetchHub: async () => { throw new TypeError("Failed to fetch"); } });
  const slow = await L.liveCatalog(snap, { timeoutMs: 30, fetchHub: (signal) => new Promise((res, rej) => { signal.addEventListener("abort", () => rej(new Error("aborted"))); }) });
  const up = await L.liveCatalog(snap, { fetchHub: async () => hub });
  ok(!down.live && down.firmwares === snap.firmwares && !slow.live && /no answer/.test(slow.liveError) && up.live && up.firmwares.find((f) => f.id === "felucca").version === "1.6.2",
     "live catalog: the Hub down or past the timeout -> the deployed snapshot; up -> merged");

  // a gone package: the live catalog once more, then the snapshot's
  const pkgs = { "https://thefirmwarehub.com/fw/felucca-1.6.3.fwsc": "163", [old.emu.pkg]: "161" };
  const asked = [];
  const fetchPackage = async (p) => { asked.push(p.package.url); if (!pkgs[p.package.url]) throw new Error(`${p.package.url}: 404`); return { bytes: new TextEncoder().encode(pkgs[p.package.url]) }; };
  let rechecks = 0;
  const fresh163 = { ...mf, version: "1.6.3", emu: { ...mf.emu, pkg: "https://thefirmwarehub.com/fw/felucca-1.6.3.fwsc" } };
  const g1 = await L.packageFor(mf, { fetchPackage, recheck: async () => { rechecks++; return fresh163; } });
  ok(g1.entry.version === "1.6.3" && new TextDecoder().decode(g1.bytes) === "163" && rechecks === 1 && asked.join() === [mf.emu.pkg, fresh163.emu.pkg].join(),
     "404: the Hub's catalog once more -> its newer package");
  asked.length = 0; rechecks = 0;
  const g2 = await L.packageFor(mf, { fetchPackage, recheck: async () => { rechecks++; return mf; } });
  ok(g2.entry.version === old.version && g2.entry.fromSnapshot && new TextDecoder().decode(g2.bytes) === "161" && rechecks === 1 && asked.join() === [mf.emu.pkg, old.emu.pkg].join()
     && g2.entry.defaultEngine === "native",
     "404: nothing newer on the Hub -> the snapshot's package and version (native the default again)");
  let shaErr = null;
  try { await L.packageFor(mf, { fetchPackage: async () => { throw new Error("x: SHA-256 1, the catalog says 2"); }, recheck: async () => { rechecks = 99; return null; } }); } catch (e) { shaErr = e.message; }
  ok(/SHA-256/.test(shaErr) && rechecks !== 99 && L.isGone(new TypeError("Failed to fetch")) && L.isGone(new Error("u: 404")) && !L.isGone(new Error("u: 500")),
     "404: a wrong SHA-256 is reported, not fallen back from");

  // the browser's engine filter
  const all = L.browse(m.firmwares, {}), nativeOnly = L.browse(m.firmwares, { engine: "native" }), emuOnly = L.browse(m.firmwares, { engine: "emu" });
  ok(all.length === m.firmwares.length && nativeOnly.every((f) => f.native) && nativeOnly.length === m.firmwares.filter((f) => f.native).length
     && emuOnly.every((f) => f.engines.includes("emu") && !f.emuNote) && !emuOnly.some((f) => f.id === "bubba" || f.id === "amb") && emuOnly.some((f) => f.id === "felucca"),
     "filter: All everything; Native the cores; Emulated what fm1-emu runs (not bubba, not a native-only one)");
  ok(L.browse(m.firmwares, { engine: "native", cat: "Groovebox", q: "fel" }).map((f) => f.id).join() === "felucca,melodee" && L.browse(m.firmwares, { engine: "emu", q: "zzzz" }).length === 0,
     "filter: combines with a category chip and the search (Melodee's blurb names Felucca)");
  const cvN = L.cardView(mf, "native"), cvE = L.cardView(mf, "emu"), cvA = L.cardView(mf, "all");
  ok(cvN.version === mf.native.version && cvN.note === "Hub: 1.6.2" && cvN.engine === "native" && cvE.version === "1.6.2" && cvE.engine === "emu" && !cvE.note
     && cvA.version === "1.6.2" && cvA.note === `native: ${mf.native.version}` && cvA.engine === null && L.cardView(snap.firmwares.find((f) => f.id === "fimba"), "native").note === null,
     "filter: Native cards show the native version (a note when behind) and start native; Emulated the Hub's and start emu; All the default");
}

// ---- a save carried to a new version (js/saves.js Carry)
{
  const S = await import("./js/saves.js"), { compareVersions } = await import("./js/live.js");
  const store = [], decided = new Map();
  const io = { list: async (fw) => store.filter((r) => r.fw === fw),
    put: async (fw, engine, version, name, bytes) => { const i = store.findIndex((r) => r.fw === fw && r.engine === engine && r.version === version && r.name === name); const r = { fw, engine, version, name, bytes: new Uint8Array(bytes), saved: new Date(Date.now() + store.length).toISOString() }; if (i >= 0) store[i] = r; else store.push(r); return r; },
    decisions: { get: (k) => decided.get(k), set: (k, v) => decided.set(k, v) }, compare: compareVersions };
  await io.put("felucca", "emu", "1.6.1", S.AUTOSAVE, [1, 2, 3]);
  await io.put("felucca", "native", "1.6.1", S.AUTOSAVE, [9, 9]);
  await io.put("felucca", "emu", "1.5.0", "old", [5]);
  const c1 = new S.Carry(io);
  const o = await c1.check("felucca", "emu", "1.6.2");
  ok(o && o.from === "1.6.1" && o.to === "1.6.2" && o.engine === "emu" && o.rec.name === S.AUTOSAVE && (await c1.check("felucca", "emu", "1.6.2")) === null,
     "carry: 1.6.2 has no save, 1.6.1 has one (emu) -> offered once in a page");
  const n = await c1.answer(o, true);
  const at162 = store.filter((r) => r.version === "1.6.2"), at161 = store.find((r) => r.engine === "emu" && r.version === "1.6.1");
  ok(n && at162.length === 1 && at162[0].engine === "emu" && at162[0].name === S.AUTOSAVE && at162[0].bytes.join() === "1,2,3" && at161 && at161.bytes.join() === "1,2,3"
     && (await new S.Carry(io).check("felucca", "emu", "1.6.2")) === null,
     "carry: yes copies the flash to 1.6.2's Autosave, keeps 1.6.1's as the backup, and is not asked again");
  const c2 = new S.Carry(io);
  const on = await c2.check("felucca", "native", "1.6.3");
  ok(on && on.engine === "native" && on.rec.bytes.join() === "9,9", "carry: native offers the native save only (engine kinds not mixed)");
  await c2.answer(on, false);
  ok((await new S.Carry(io).check("felucca", "native", "1.6.3")) === null && !store.some((r) => r.version === "1.6.3"), "carry: no starts fresh and stops asking for that version");
  ok((await new S.Carry(io).check("felucca", "emu", "1.4.0")) === null && (await new S.Carry(io).check("felucca", "mock", "1.6.9")) === null
     && (await new S.Carry(io).check("fomni", "emu", "0.5")) === null, "carry: never from a newer version, never on the test engine, nothing to carry -> no offer");
  // shown, not answered (a reload): again; a version that had saves before any offer: never
  const c3 = new S.Carry(io);
  const o3 = await c3.check("felucca", "emu", "1.7");
  await io.put("felucca", "emu", "1.7", S.AUTOSAVE, [7]);                     // (the fresh boot's own autosave)
  ok(o3 && (await new S.Carry(io).check("felucca", "emu", "1.7")) && (await new S.Carry(io).check("felucca", "emu", "1.6.2")) === null,
     "carry: an unanswered offer comes back at the next load; a version with saves already is not asked about");
  const b = S.parseBackup(S.backupJson(store));
  ok(b.length === store.length && b.some((r) => r.version === "1.6.1" && r.engine === "emu"), "carry: the export holds the backup with the rest");
}

// ---- the emulated CPU clock (js/cpu.js)
{
  const P = await import("./js/cpu.js");
  ok(P.mhzFor("choralroot", {}).mhz === 360 && P.mhzFor("choralroot", {}).choice === "auto" && P.DEVICE_MHZ === 360
     && P.MHZ_CHOICES.join() === "auto,240,320,360,480,640" && P.inDeviceRange(320) && P.inDeviceRange(360) && P.isOverclock(480) && P.isOverclock(640) && !P.isOverclock(360),
     "cpu: the default is Auto at 360 MHz; choices Auto 240 320 360 480 640 (320-360 the device's, 480 640 overclock)");
  let prefs = {};
  Object.assign(prefs, P.pickMhz(prefs, "choralroot", 640));
  const saved = JSON.parse(JSON.stringify(prefs));                           // (through localStorage)
  ok(P.mhzFor("choralroot", saved).mhz === 640 && P.mhzFor("choralroot", saved).choice === 640 && P.mhzFor("felucca", saved).mhz === 360
     && P.mhzFor("x", { mhz: { x: 100 } }).mhz === 360, "cpu: a pick is kept per firmware, before the default (100 is no choice)");
  // Auto, too slow: load above 80 % for 2 s (after the 4 s grace) -> 320
  const run = (g, samples) => { for (const [t, load, late] of samples) { const r = g.feed({ load, lateMs: late || 0 }, t); if (r) return [t, r]; } return null; };
  const busy = (from, to, load, late = 0) => { const s = []; for (let t = from; t <= to; t += 500) s.push([t, load, late]); return s; };
  const g = new P.Governor({ choice: "auto", mhz: 360, now: 0 });
  const r = run(g, [...busy(500, 3500, 0.99), ...busy(4000, 5000, 0.5), ...busy(5500, 9000, 0.9)]);
  ok(r && r[1].step === 320 && r[0] === 7500, `cpu: Auto, load 90 % for 2 s after the boot's grace -> steps to 320 MHz (at ${r && r[0]} ms)`);
  prefs.autoMhz = P.stepDown({}, "x", 320);
  ok(P.mhzFor("x", prefs).mhz === 320 && [360, 320, 240, 180].map(P.nextStep).join() === "320,240,180,120" && P.nextStep(120) === null,
     "cpu: 360 -> 320 -> 240 -> 180 -> 120 (the floor), and the step is kept (never up by itself)");
  const gl = new P.Governor({ choice: "auto", mhz: 240, now: 0 });
  const rl = run(gl, busy(4000, 9000, 0.3, 70));
  ok(rl && rl[1].step === 180, "cpu: Auto, dropouts adding up (70 ms late a report) -> the next step down");
  ok(run(new P.Governor({ choice: "auto", mhz: 360, now: 0 }), [...busy(4000, 20000, 0.7), ...busy(20500, 21500, 0.95), ...busy(22000, 40000, 0.6, 10)]) === null,
     "cpu: Auto, 70 % load and short spikes -> stays");
  const gm = new P.Governor({ choice: 640, mhz: 640, now: 0 }), rm = run(gm, busy(4000, 9000, 0.95));
  ok(rm && rm[1].warn && !rm[1].step && gm.feed({ load: 0.99 }, 10000) === null, "cpu: a manual 640 never changes; it warns once");
  ok(run(new P.Governor({ choice: "auto", mhz: 120, now: 0 }), busy(4000, 9000, 0.95))[1].warn, "cpu: Auto at the 120 MHz floor, still too slow -> a warning, no lower step");
  // headroom before the audio breaks: the busiest stretch (peak) or the browser's glitch count
  const feed = (g, samples) => { for (const [t, m] of samples) { const r = g.feed(m, t); if (r) return [t, r]; } return null; };
  const peaky = []; for (let t = 4000; t <= 9000; t += 500) peaky.push([t, { load: 0.6, peak: 0.97 }]);
  const rp = feed(new P.Governor({ choice: "auto", mhz: 360, now: 0 }), peaky);
  ok(rp && rp[1].step === 320 && rp[0] === 5000, `cpu: Auto, mean 60 % but its busiest 21 ms above 95 % in 3 of 4 reports -> 320 (at ${rp && rp[0]} ms)`);
  const oneSpike = []; for (let t = 4000; t <= 20000; t += 500) oneSpike.push([t, { load: 0.7, peak: t % 2500 ? 0.8 : 1.2 }]);
  ok(feed(new P.Governor({ choice: "auto", mhz: 360, now: 0 }), oneSpike) === null, "cpu: Auto, a peak now and then (1 report in 5) -> stays");
  const gl3 = []; let n = 40; for (let t = 1000; t <= 9000; t += 500) { if (t >= 6000) n += 1; gl3.push([t, { load: 0.5, peak: 0.6, glitches: t < 4000 ? (n += 10) : n }]); }
  const rg = feed(new P.Governor({ choice: "auto", mhz: 360, now: 0 }), gl3);
  ok(rg && rg[1].step === 320 && rg[0] === 7000, `cpu: Auto, 3 glitches within 3 s (the browser's count; boot glitches don't count) -> 320 (at ${rg && rg[0]} ms)`);
  ok(feed(new P.Governor({ choice: "auto", mhz: 360, now: 0 }), busy(4000, 9000, 0.78).map(([t, l]) => [t, { load: l, peak: 0.9, glitches: 5 }])) === null,
     "cpu: Auto, 78 % with a 90 % peak and no new glitches -> stays");
  ok(P.mhzFor("melodee", {}, { cores: 4 }).mhz === 320 && P.mhzFor("felucca", {}, { cores: 4 }).mhz === 360 && P.mhzFor("melodee", {}, { cores: 8 }).mhz === 360
     && P.mhzFor("melodee", {}, {}).mhz === 360 && P.mhzFor("melodee", { mhz: { melodee: 360 } }, { cores: 2 }).mhz === 360,
     "cpu: a weak host (4 cores or fewer) starts Auto one step lower for a dual-core firmware; a pick or an unknown host does not");
  Object.assign(prefs, P.pickMhz(prefs, "x", "auto"));
  ok(P.mhzFor("x", prefs).mhz === 360, "cpu: picking Auto again starts at 360");
  ok(["choralroot", "melodee", "sloopdx", "fm106", "coralflower"].every((id) => P.DUAL_CORE.has(id) && cat.firmwares.some((f) => f.id === id)), "cpu: the dual-core firmwares (CPU-heavy) are in the catalog");
}

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


// ---- the running firmware's source link (live.js sourceOf): the version that runs, for every kind of firmware
{
  const { sourceOf } = await import("./js/live.js");
  const credits = cat.credits, F = (id) => cat.firmwares.find((f) => f.id === id);
  const fe = sourceOf(F("felucca"), "emu", credits), fn = sourceOf(F("felucca"), "native", credits);
  ok(fe.url === "https://github.com/hugelton/Felucca/tree/v1.6.1" && fe.ref === "v1.6.1" && !fe.glue
     && fe.title === "Felucca 1.6.1 by Leo Kuroshita (Hügelton Instruments) · GPL-3.0 (engines MIT) · source at v1.6.1", "source: emulated, the Hub's tree/<tag> URL as it is; the tooltip says author, licence, version");
  const bare = sourceOf({ id: "x", name: "X", version: "1.2", author: "a", source: "https://github.com/o/r", release: "https://github.com/o/r/releases/tag/v1.2" }, "emu");
  const noTag = sourceOf({ id: "x", name: "X", version: "1.2", author: "a", source: "https://github.com/o/r.git", release: "https://o.example/" }, "emu");
  ok(bare.url === "https://github.com/o/r/tree/v1.2" && bare.ref === "v1.2" && noTag.url === "https://github.com/o/r" && noTag.ref === null && /no release tag/.test(noTag.title),
     "source: a bare repo with a release tag -> /tree/<tag>; without one, the repo as it is");
  const jn = F("jiant").native;
  ok(fn.url === `${F("felucca").native.repo}/tree/${F("felucca").native.commit}` && /^[0-9a-f]{40}$/.test(F("felucca").native.commit) && fn.refKind === "commit"
     && fn.glue.url === `https://github.com/Quixotic7/FM1VST/tree/${credits.fm1vst.commit}/core-glue/felucca_web` && /source at commit [0-9a-f]{7}, the commit FM1VST builds/.test(fn.title)
     && sourceOf(F("jiant"), "native", credits).url === `https://github.com/juanjiant-bit/JIANT/tree/${jn.commit}`,
     "source: native, the core's pinned commit (FM1VST's submodule) and FM1VST's core-glue/<family>");
  const rf = sourceOf(F("remotefm1"), "emu", credits);
  ok(rf.url === "https://github.com/Quixotic7/RemoteFM1/tree/v" + F("remotefm1").version && / · GPL-3.0-only · source at v/.test(rf.title), "source: RemoteFM1, this repo at its release's tag");
  ok(sourceOf({ id: "local-FM-1.fwsc", name: "FM-1", local: true, source: null }, "emu") === null && sourceOf({ id: "y", name: "Y", source: null, repo: null }, "emu") === null && sourceOf(null, "emu") === null,
     "source: stock M-VAVE / a local .fwsc -> none");
  const shtml = readFileSync(new URL("./index.html", import.meta.url), "utf8");
  ok(/<a id="fw-src"[^>]*target="_blank" rel="noopener"/.test(shtml) && /<a id="site-src"[^>]*>Site source<\/a>/.test(shtml) && !/>Source<\/a>/.test(shtml),
     "source: the header's firmware link opens a new tab (noopener); the site's own is \"Site source\"");
}
// ---- no raw preset source in what the site publishes (tools/no_raw_presets.mjs; pages.yml runs it on _site): the
// page's own files and the native cores built here. The content checks need web/vendor/FM1VST's core trees
{
  const tool = fileURLToPath(new URL("./tools/no_raw_presets.mjs", import.meta.url));
  const trees = existsSync(fileURLToPath(new URL("../vendor/FM1VST/cores/melodee/assets", import.meta.url)));
  for (const d of ["./", "../build/native/"]) {
    const dir = fileURLToPath(new URL(d, import.meta.url));
    if (!existsSync(dir)) continue;
    let out = "", st = 0;
    try { out = execFileSync("node", [tool, dir, ...(trees ? [] : ["--names-only"]), ...(d === "./" ? [] : ["--top"])], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }); }
    catch (e) { st = e.status || 1; out = String(e.stderr || e.stdout); }
    ok(st === 0, `no raw presets: web/${d === "./" ? "site" : "build/native"}${trees ? "" : " (names only: no FM1VST core trees)"}${st ? ": " + out.trim().split("\n")[0] : ""}`);
  }
}
// ---- build_catalog: the native cores' commits from web/vendor/FM1VST (git ls-tree), data/cores.json's fallback in step
{
  const VST = fileURLToPath(new URL("../vendor/FM1VST/", import.meta.url));
  if (!existsSync(join(VST, ".gitmodules"))) console.log("build_catalog: no web/vendor/FM1VST checkout, native commits not checked");
  else {
    const pins = new Map(execFileSync("git", ["-C", VST, "ls-tree", "HEAD", "cores/"], { encoding: "utf8" }).trim().split("\n")
      .map((l) => /^160000 commit ([0-9a-f]{40})\t(\S+)$/.exec(l)).filter(Boolean).map((m) => [m[2], m[1]]));
    const gm = readFileSync(join(VST, ".gitmodules"), "utf8"), urls = new Map([...gm.matchAll(/path = (\S+)\s+url = (\S+)/g)].map((m) => [m[1], m[2].replace(/\.git$/, "")]));
    const nat = cat.firmwares.filter((f) => f.native);
    ok(nat.length === 13 && nat.every((f) => [...pins].some(([path, c]) => c === f.native.commit && urls.get(path) === f.native.repo)) && nat.every((f) => existsSync(join(VST, "core-glue", f.native.glue))),
       "build_catalog: each native core's commit is FM1VST's pin (ls-tree), its repo the submodule's, its glue folder there");
    const cj = JSON.parse(readFileSync(new URL("./data/cores.json", import.meta.url), "utf8"));
    const head = execFileSync("git", ["-C", VST, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    ok(cat.credits.fm1vst.commit === head && cj.fm1vst === head && nat.every((f) => { const c = cj.cores.find((x) => x.core === f.native.core); return c.commit === f.native.commit && c.repo === f.native.repo && c.glue === f.native.glue && c.version === f.native.version; }),
       "build_catalog: data/cores.json's fallback commits agree with web/vendor/FM1VST (the Pages check job builds without it)");
  }
}

// ---- authors' own native builds (docs/NATIVE-AUTHORS.md): the engine's import whitelist, the .fm1web checks, the
// site's sync with a recorded gh (tools/fixtures/gh-mock.mjs), the catalog's choice between two native builds
{
  const { preferNative, authorNative, nativeOrigin } = await import("./js/live.js");
  const { PROVIDED_IMPORTS, moduleProblems } = await import("../engines/native/engine.js");
  const F = await import("../engines/native/fm1web.mjs");
  const S = await import("./tools/native_sync.mjs");
  const { mkdtempSync, writeFileSync, rmSync, readdirSync } = await import("node:fs");
  // a wasm module by hand: function imports [module, name], memory imports, exported functions (all () -> ())
  const leb = (n) => { const o = []; do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; o.push(b); } while (n); return o; };
  const str = (s) => [...leb(Buffer.byteLength(s)), ...Buffer.from(s)];
  const sec = (id, body) => [id, ...leb(body.length), ...body];
  const vec = (items) => [...leb(items.length), ...items.flat()];
  function wasmOf({ imports = [], memImports = [], exports = [] }) {
    const nImp = imports.length;
    const b = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sec(1, vec([[0x60, 0, 0]])),
      ...sec(2, vec([...imports.map(([m, n]) => [...str(m), ...str(n), 0, 0]), ...memImports.map(([m, n]) => [...str(m), ...str(n), 2, 0, 1])])),
      ...sec(3, vec(exports.map(() => [0]))),
      ...(memImports.length ? [] : sec(5, vec([[0, 1]]))),
      ...sec(7, vec([...exports.map((n, i) => [...str(n), 0, ...leb(nImp + i)]), ...(memImports.length ? [] : [[...str("memory"), 2, 0]])])),
      ...sec(10, vec(exports.map(() => [2, 0, 0x0b])))];
    return Buffer.from(b);
  }
  const { REQUIRED_EXPORTS } = await import("../engines/native/engine.js");
  const fns = REQUIRED_EXPORTS.filter((e) => e !== "memory");
  const good = wasmOf({ imports: [["wasi_snapshot_preview1", "fd_write"], ["env", "emscripten_notify_memory_growth"]], exports: fns });
  ok(moduleProblems(new WebAssembly.Module(good)).length === 0 && PROVIDED_IMPORTS.env.includes("fm1w_co_yield") && PROVIDED_IMPORTS.wasi_snapshot_preview1.includes("fd_write")
     && Object.keys(PROVIDED_IMPORTS).sort().join() === "env,wasi_snapshot_preview1",
     "native authors: the import whitelist is engine.js's own import object (env + wasi), a module within it passes");
  const evil = moduleProblems(new WebAssembly.Module(wasmOf({ imports: [["env", "system"], ["wasi_snapshot_preview1", "path_open"], ["js", "eval"]], exports: fns })));
  const mem = moduleProblems(new WebAssembly.Module(wasmOf({ memImports: [["env", "memory"]], exports: fns })));
  const coro = moduleProblems(new WebAssembly.Module(wasmOf({ imports: [["env", "fm1w_co_yield"]], exports: fns })), { asyncify: false });
  ok(evil.length === 3 && evil.every((p) => /which engine\.js does not provide/.test(p)) && mem.some((p) => /imports a memory/.test(p)) && mem.some((p) => /export memory/.test(p))
     && coro.some((p) => /does not export asyncify_start_unwind/.test(p)) && coro.some((p) => /manifest says asyncify false/.test(p)),
     "native authors: any other import (env.system, path_open, js.eval, a memory) is rejected; the Asyncify flag must be true");

  // a .fm1web of that module, then tampered with
  const manifest = (over = {}) => ({ format: F.FORMAT, id: "testfw", core: "testfw", name: "Test FW", version: "1.0", family: "felucca_web", glue: "core-glue/felucca_web",
    abi: 6, fm1vst: { repo: "https://github.com/Quixotic7/FM1VST", commit: "7".repeat(40) }, source: { repo: "someone/testfw", url: "https://github.com/someone/testfw", tag: "v1.0", commit: "c".repeat(40) },
    wasm: { file: "testfw.wasm", sha256: createHash("sha256").update(good).digest("hex"), bytes: good.length }, asyncify: false, usbProduct: null, editor: null,
    license: "GPL-3.0-only", credits: null, gated: null, checks: { passed: true, lines: [] }, built: { by: "fm1-native" }, ...over });
  const pack = (m, files = [["testfw.wasm", good]]) => F.writeArchive([["manifest.json", Buffer.from(JSON.stringify(m))], ...files]);
  const v0 = F.verifyArchive(pack(manifest()), { repo: "someone/testfw", id: "testfw" });
  const vSha = F.verifyArchive(pack(manifest({ wasm: { file: "testfw.wasm", sha256: "0".repeat(64), bytes: good.length } })));
  const vRepo = F.verifyArchive(pack(manifest()), { repo: "other/repo" });
  const vId = F.verifyArchive(pack(manifest()), { id: "felucca" });
  const vBig = F.verifyArchive(pack(manifest()), { maxBytes: 100 });
  const vExtra = F.verifyArchive(pack(manifest(), [["testfw.wasm", good], ["payload.js", Buffer.from("alert(1)")]]));
  const vFail = F.verifyArchive(pack(manifest({ checks: { passed: false } })));
  let trav = null; try { F.readArchive(F.writeArchive([["../x", Buffer.from("x")]])); } catch (e) { trav = e.message; }
  ok(v0.ok && v0.manifest.id === "testfw" && !vSha.ok && vSha.problems.some((p) => /SHA-256/.test(p)) && !vRepo.ok && !vId.ok && !vBig.ok && vBig.problems.some((p) => /cap/.test(p))
     && !vExtra.ok && !vFail.ok && /flat names/.test(trav || ""),
     "native authors: a .fm1web: its manifest, SHA-256, repository, id, size cap, no other file, passed checks, no path in a name");

  // the attestation's fields (gh's JSON) and the licensing gate
  const tpl = readFileSync(new URL("./tools/fixtures/gh-attestation-verify.json", import.meta.url), "utf8");
  const ghJson = (o) => JSON.parse(tpl.replace(/\{\{(\w+)\}\}/g, (_, k) => ({ NAME: "x.fm1web", SHA256: "a".repeat(64), TAG: "v1.0", REPO: "someone/testfw", COMMIT: "c".repeat(40),
    SIGNER_WORKFLOW: F.WORKFLOW, SIGNER_REF: "refs/tags/native-v1", SOURCE_REPO: "someone/testfw", OWNER: "someone", ...o })[k]));
  const rules = { repo: "someone/testfw", trustedRefs: ["refs/tags/native-v1"] };
  const a0 = S.attestationOf(ghJson({}), rules), aRef = S.attestationOf(ghJson({ SIGNER_REF: "refs/heads/main" }), rules);
  const aWf = S.attestationOf(ghJson({ SIGNER_WORKFLOW: "zvenson/fm1-build/.github/workflows/build.yml" }), rules), aSrc = S.attestationOf(ghJson({ SOURCE_REPO: "evil/fork" }), rules);
  ok(a0.ok && a0.commit === "c".repeat(40) && a0.tag === "v1.0" && /actions\/runs\//.test(a0.run) && !aRef.ok && /native-v1/.test(aRef.why) && !aWf.ok && !aSrc.ok && !S.attestationOf([], rules).ok,
     "native authors: the attestation: only fm1-native.yml at native-v1, built from the listed repository at a tag");
  const coresJ = JSON.parse(readFileSync(new URL("./data/cores.json", import.meta.url), "utf8")).cores, cfgJ = JSON.parse(readFileSync(new URL("./data/native-authors.json", import.meta.url), "utf8"));
  ok(!S.gateOf({ id: "x0x", core: "x0x" }, coresJ, cfgJ).ok && !S.gateOf({ id: "nmsfm", core: "nmsfm" }, coresJ, cfgJ).ok && S.gateOf({ id: "duell", core: "duell" }, coresJ, cfgJ).ok
     && !S.gateOf({ id: "orbit", core: "orbit" }, coresJ, { ...cfgJ, ungated: { orbit: "no" } }).ok && S.gateOf({ id: "felucca", core: "felucca" }, coresJ, cfgJ).ok
     && cfgJ.authors.length === 0 && cfgJ.hubDiscovery === false && cfgJ.trustedRefs.join() === "refs/tags/native-v1",
     "native authors: gated firmwares stay out (x0x, NMS by its Hub id), DUELL un-gated, ORBIT never; the list starts empty, Hub discovery off");

  // the sync, end to end on a folder of files with the recorded gh
  const dir = mkdtempSync(join(tmpdir(), "native-sync-")), from = join(dir, "from"), mirror = join(dir, "ext");
  const { mkdirSync } = await import("node:fs");
  mkdirSync(from);
  writeFileSync(join(from, "testfw-1.0.fm1web"), pack(manifest()));
  writeFileSync(join(dir, "cfg.json"), JSON.stringify({ ...cfgJ, authors: [{ repo: "someone/testfw", id: "testfw" }] }));
  const ghMock = fileURLToPath(new URL("./tools/fixtures/gh-mock.mjs", import.meta.url));
  const runSync = async (mode, extra = {}) => { process.env.FM1_GH_MOCK = mode; return S.sync({ config: join(dir, "cfg.json"), from, gh: ghMock, work: join(dir, "work"), log: () => {}, noSmoke: true, ...extra }); };
  const sOk = await runSync("ok", { mirror });
  const sRef = await runSync("branch-ref"), sRepo = await runSync("other-repo"), sFile = await runSync("other-file"), sNone = await runSync("no-attest");
  const sSmoke = await runSync("ok", { noSmoke: false });
  delete process.env.FM1_GH_MOCK;
  ok(sOk.accepted.length === 1 && sOk.accepted[0].wasm === "testfw/1.0/testfw.wasm" && /native-v1$/.test(sOk.accepted[0].signer) && /actions\/runs\//.test(sOk.accepted[0].buildLog)
     && readdirSync(join(mirror, "testfw", "1.0")).sort().join() === "manifest.json,testfw.wasm" && sOk.candidates.length === 1,
     "native authors: sync: attested, checked, mirrored to ext/<id>/<version>/ with its manifest");
  ok([sRef, sRepo, sFile, sNone].every((s) => s.accepted.length === 0 && s.rejected.length === 1) && /native-v1/.test(sRef.rejected[0].why) && /someone\/else/.test(sRepo.rejected[0].why)
     && /not for this file/.test(sFile.rejected[0].why) && /gh attestation verify/.test(sNone.rejected[0].why) && sSmoke.accepted.length === 0 && /smoke test/.test(sSmoke.rejected[0].why),
     "native authors: sync rejects an untrusted signer ref, another repository, another file's attestation, none, a failed smoke test");
  rmSync(dir, { recursive: true, force: true });

  // the catalog's choice: newer wins, the author's at the same version; the other build named
  const fv = { core: "x", version: "1.6.1", wasm: "x.wasm" }, au = (v) => authorNative({ core: "x", version: v, wasm: `x/${v}/x.wasm`, glue: "core-glue/felucca_web", repo: "https://github.com/a/b", commit: "c".repeat(40), tag: `v${v}`, buildLog: "https://github.com/a/b/actions/runs/1" }, "engines/native/ext/");
  const pEq = preferNative(fv, au("1.6.1")), pNew = preferNative(fv, au("1.7")), pOld = preferNative(fv, au("1.6.0")), pOnly = preferNative(null, au("1.0"));
  ok(pEq.native.by === "author" && pEq.alt.by === "fm1vst" && /both 1\.6\.1/.test(pEq.why) && pNew.native.version === "1.7" && pOld.native === fv && pOld.alt.by === "author"
     && pOnly.native.by === "author" && !pOnly.alt && preferNative(fv, null).native === fv && pEq.native.wasm === "engines/native/ext/x/1.6.1/x.wasm" && pEq.native.glue === "felucca_web",
     "native authors: newer version wins, the author's build at the same version; the other is named");
  const idx = join(tmpdir(), `native-ext-${process.pid}.json`), outC = join(tmpdir(), `catalog-ext-${process.pid}.json`);
  const acc = (id, version, extra = {}) => ({ id, core: id, name: id, version, family: "felucca_web", glue: "core-glue/felucca_web", wasm: `${id}/${version}/${id}.wasm`, sha256: "a".repeat(64), bytes: 1,
    asyncify: false, abi: 6, license: "GPL-3.0-only", credits: null, usbProduct: null, editor: null, fm1vst: "7".repeat(40), repo: `https://github.com/a/${id}`, tag: `v${version}`, commit: "c".repeat(40), buildLog: "https://github.com/a/b/actions/runs/2", ...extra });
  writeFileSync(idx, JSON.stringify({ workflow: F.WORKFLOW, trustedRefs: ["refs/tags/native-v1"], candidates: [], rejected: [{ repo: "x/y", tag: "v1", why: "test" }],
    accepted: [acc("felucca", "1.6.1"), acc("amb", "1.2"), acc("duell", "0.2.20-beta")] }));
  execFileSync(process.execPath, [fileURLToPath(new URL("./tools/build_catalog.mjs", import.meta.url)), "--layout", "pages", "--out", outC, "--native-ext", idx], { stdio: "ignore" });
  const ec = JSON.parse(readFileSync(outC, "utf8")), by = (id) => ec.firmwares.find((f) => f.id === id);
  unlinkSync(idx); unlinkSync(outC);
  const fe = by("felucca"), am = by("amb"), du = by("duell");
  ok(fe.native.by === "author" && fe.native.wasm === "engines/native/ext/felucca/1.6.1/felucca.wasm" && fe.nativeAlt.by === "fm1vst" && fe.defaultEngine === "native"
     && am.native.by !== "author" && am.nativeAlt.by === "author" && am.nativeAlt.version === "1.2" && /newer/.test(am.nativeWhy)
     && du.native && du.native.by === "author" && !du.nativeGated && du.engines.join() === "native,emu" && !du.nativeBehind
     && ec.nativeAuthors.accepted.length === 3 && ec.nativeAuthors.rejected[0].why === "test" && nativeOrigin(fe).short === "native · built by the author"
     && ec.firmwares.filter((f) => f.native && f.native.by !== "author" || f.nativeAlt && f.nativeAlt.by === "fm1vst").length === 13,
     "native authors: the catalog: Felucca the author's (same version), AMB-1 FM1VST's (newer), DUELL the author's only; FM1VST's 13 still counted");
}

// ---- the computer-key hints on the panel: off by default, remembered (prefs.js), drawn only when on
{
  const P = await import("./js/prefs.js");
  const mem = () => { const m = new Map(); return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)) }; };
  const st = mem();
  ok(P.keyHintsOn(P.loadPrefs(st)) === false && P.keyHintsOn(P.loadPrefs(null)) === false, "hints: off by default (no record, or no storage at all)");
  const pr = P.loadPrefs(st); pr.keyHints = true; P.savePrefs(st, pr);
  const back = P.loadPrefs(st);
  const throwing = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("quota"); } };
  ok(P.keyHintsOn(back) === true && P.savePrefs(throwing, back) === false && P.keyHintsOn(P.loadPrefs(throwing)) === false, "hints: turned on, remembered; a blocked storage gives the default and throws nothing");
  back.keyHints = false; P.savePrefs(st, back);
  ok(P.keyHintsOn(P.loadPrefs(st)) === false, "hints: turned off again, remembered");
  // the drawing: count the 5x7 font's cells (rect calls) and where the note name sits
  const { DeviceView } = await import("./js/panel.js");
  let rects = [];
  const g = new Proxy({}, { get: (_, k) => (k === "rect" ? (x, y) => rects.push(y) : () => {}), set: () => true });
  const dv = new DeviceView({ getContext: () => g }, new Controls(() => {}), null);
  const draw = (on) => { dv.setHints(on); rects = []; dv.drawKey(g, dv.pal, 7, false, 0, 1); dv.drawButton(g, dv.pal, B.FX, false, 0, 0, 1); return rects.slice(); };
  const off = draw(false), on = draw(true);
  ok(dv.hints === true && on.length > off.length && Math.min(...off) > Math.min(...on), "hints: drawn only when on; off, the key's name and the button's name move to the middle (no gap)");
}

// ---- Shift-click latches on the drawn panel (input.js attachPointer, Keyboard): the engine sees held keys and buttons
{
  const { attachPointer, Keyboard } = await import("./js/input.js");
  const P = await import("./js/prefs.js");
  const on = () => { const L = {}; return { L, addEventListener: (t, f) => { (L[t] = L[t] || []).push(f); }, fire: (t, e = {}) => (L[t] || []).forEach((f) => f(e)) }; };
  const canvas = Object.assign(on(), { getBoundingClientRect: () => ({ left: 0, top: 0 }), setPointerCapture() {} });
  const doc = Object.assign(on(), { focused: false, hidden: false, hasFocus() { return this.focused; } });
  const win = Object.assign(on(), { document: doc });
  // the stand-in view: x picks the control (y is the pointer's height, for the knob's drag)
  const AT = { 1: { kind: KM.BTN, idx: B.HOME }, 2: { kind: KM.BTN, idx: B.SEL }, 3: { kind: KM.KEY, idx: 5 }, 4: { kind: KM.SELECT, idx: E.K1 }, 5: null };
  const view = { toUnits: (x, y) => ({ x, y }), hit: (x) => AT[x] };
  let last = null, hints = 0, turned = 0;
  const c = new Controls((s) => { last = s; turned += s.enc[roleToMatrix(E.K1)]; });
  attachPointer(canvas, view, c, { win, onShiftHint: () => hints++ });
  let pid = 0;
  const click = (x, { shift = false, button = 0, y = 100, hold = null } = {}) => {
    const id = ++pid, base = { pointerId: id, clientX: x, clientY: y, shiftKey: shift, ctrlKey: false, button, pointerType: "mouse", preventDefault() {} };
    canvas.fire("pointerdown", base);
    if (hold) hold(id);
    canvas.fire("pointerup", base);
  };
  const btnBit = (b) => (last.buttons >>> labelToMatrix(b)) & 1;
  click(1, { shift: true });
  ok(c.btnLatched(B.HOME) && btnBit(B.HOME) === 1, "latch: Shift-click HOME holds it after the mouse lets go (the engine sees HOME held)");
  click(2, { shift: true });
  ok(c.btnLatched(B.SEL) && btnBit(B.HOME) === 1 && btnBit(B.SEL) === 1, "latch: Shift-click SEL as well: both held");
  let during = 0;
  click(3, { hold: () => { during = (last.keys >>> 5) & 1; } });
  ok(during === 1 && ((last.keys >>> 5) & 1) === 0 && !c.keyLatched(5) && btnBit(B.HOME) && btnBit(B.SEL), "latch: a plain click on a key while latched is momentary; the latches stay");
  click(3, { shift: true });
  ok(c.keyLatched(5) && ((last.keys >>> 5) & 1) === 1, "latch: Shift-click a key latches it (a held note)");
  click(3, { shift: true });
  ok(!c.keyLatched(5) && ((last.keys >>> 5) & 1) === 0, "latch: Shift-click it again lets it go");
  click(1);
  ok(!c.btnLatched(B.HOME) && btnBit(B.HOME) === 0 && c.btnLatched(B.SEL), "latch: a plain click on latched HOME lets it go (no momentary press left)");
  click(1, { button: 2 });
  ok(c.btnLatched(B.HOME), "latch: a right-click still latches");
  click(5, { shift: true });
  ok(!c.anyLatched() && last.buttons === 0, "latch: Shift-click on an empty spot of the panel lets every latch go");
  // Escape: lets the latches go (not OCT-); with none held it is OCT- as before; taken by a popover, it does nothing
  const kl = {}, saved = { a: globalThis.addEventListener, d: globalThis.document };
  globalThis.addEventListener = (t, f) => { kl[t] = f; };
  globalThis.document = { addEventListener() {} };
  try { new Keyboard({ controls: c, enabled: () => true }); } finally { globalThis.addEventListener = saved.a; globalThis.document = saved.d; }
  const key = (type, code, extra = {}) => kl[type]({ code, key: code, repeat: false, metaKey: false, ctrlKey: false, altKey: false, shiftKey: false, target: {}, preventDefault() {}, ...extra });
  click(1, { shift: true }); click(3, { shift: true });
  key("keydown", "Escape");
  ok(!c.anyLatched() && !c.btnHeld(B.OCTDN) && last.keys === 0 && last.buttons === 0, "latch: Escape lets them all go, and does not press OCT-");
  key("keyup", "Escape");
  key("keydown", "Escape"); const octHeld = c.btnHeld(B.OCTDN); key("keyup", "Escape");
  ok(octHeld && !c.btnHeld(B.OCTDN), "latch: Escape with nothing latched is OCT- as before");
  click(1, { shift: true });
  key("keydown", "Escape", { cancelBubble: true });
  ok(c.btnLatched(B.HOME) && !c.btnHeld(B.OCTDN), "latch: an Escape a popover took (stopPropagation) leaves the latches and OCT- alone");
  // computer keys with Shift held play as before; latches stay
  key("keydown", "KeyA", { shiftKey: true });
  ok(c.keyHeld(7) && c.btnLatched(B.HOME), "latch: computer keys with Shift held still play (Shift+A holds its key)");
  key("keyup", "KeyA");
  // focus: to a frame in the page (the Editor panel) keeps them; the page losing focus or hidden lets them go
  const tick = () => new Promise((r) => setTimeout(r, 5));
  doc.focused = true; win.fire("blur"); await tick();
  ok(c.btnLatched(B.HOME), "latch: focus moving into a frame of the page keeps the latches");
  doc.focused = false; win.fire("blur"); await tick();
  ok(!c.anyLatched() && last.buttons === 0, "latch: the page losing focus lets them go");
  click(2, { shift: true }); doc.hidden = true; doc.fire("visibilitychange"); doc.hidden = false;
  ok(!c.anyLatched(), "latch: the tab hidden lets them go");
  click(2, { shift: true }); c.releaseLatches();
  ok(!c.anyLatched() && last.buttons === 0, "latch: releaseLatches (main.js on a firmware switch or reboot) lets them go");
  // a knob: Shift-click and drag turns it exactly as a plain one, and latches nothing
  const drag = (shift) => {
    turned = 0;
    click(4, { shift, y: 100, hold: (id) => canvas.fire("pointermove", { pointerId: id, clientY: 60, shiftKey: shift, pointerType: "mouse" }) });
    return turned;
  };
  const plain = drag(false), shifted = drag(true);
  ok(plain === 5 && shifted === 5 && c.selKnob === E.K1 && !c.anyLatched(), "latch: Shift-click and drag on a knob turns it as a plain drag (5 detents), no latch");
  // the hint: Shift with the mouse over the panel, once per page; shown once ever (prefs)
  const hintsBefore = hints;
  canvas.fire("pointermove", { pointerId: 99, clientY: 0, shiftKey: false, pointerType: "mouse" });
  win.fire("keydown", { key: "Shift" }); win.fire("keydown", { key: "Shift" });
  ok(hints - hintsBefore <= 1 && hints === 1, "latch: the Shift hint fires once (Shift over the panel)");
  const mem = new Map(), st = { getItem: (k) => (mem.has(k) ? mem.get(k) : null), setItem: (k, v) => mem.set(k, String(v)) };
  const pr = P.loadPrefs(st), due0 = P.shiftHintDue(pr); pr.shiftHintShown = true; P.savePrefs(st, pr);
  ok(due0 && !P.shiftHintDue(P.loadPrefs(st)) && P.shiftHintDue(P.loadPrefs(null)), "latch: \"Shift-click to hold\" is due once, then remembered");
}

console.log(fails ? `site: ${fails} of ${checks} FAILED` : `site tests passed (${checks})`);
process.exit(fails ? 1 : 0);
