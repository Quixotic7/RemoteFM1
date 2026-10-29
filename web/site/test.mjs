// SPDX-License-Identifier: GPL-3.0-only
// The site's logic in Node: the catalog, following the Hub's releases (live merge, versions, newest wins, saves
// carried to a new version), the emulated CPU clock, the browser's engine filter, the panel's orders (label / role -> matrix), the worklet host's panel merge
// (taps), the computer-key map, the mock engine against web/engine/API.md, and the OMX-27's REMOTE mode.
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
import { PRESETS, contrast, hex, derived, presetNamed, CUSTOM_NAME, accentOf } from "./js/theme.js";
import { Fm1Link, viewOf, errorText, MIDI_PROMPT_TEXT, MIDI_LATE_TEXT, MIDI_DENIED_TEXT } from "./js/fm1-link.js";

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(76)} ${c ? "ok" : "FAIL"}`); };

// ---- the catalog
const cat = JSON.parse(readFileSync(new URL("./catalog.json", import.meta.url), "utf8"));
const natives = cat.firmwares.filter((f) => f.native).map((f) => f.id).sort();
ok(natives.join() === "amb,choralroot,coralflower,duell,felucca,fimba,fmini,fomni,frodhes,ghoulbox,grimm,jiant,loom,melodee,minitone,obervave,pigeon,purplemonkey,skiff,speak37,tonitor",
   "catalog: the 21 un-gated cores are native, no other");
ok(cat.firmwares.filter((f) => !f.offHub && !f.rfm1).every((f) => f.engines.includes("emu") && /^[0-9a-f]{64}$/.test(f.emu.sha256)
   && (f.id === "remotefm1" ? f.emu.from === "site" : /^https:\/\/thefirmwarehub\.com\/fw\//.test(f.emu.pkg) && !f.emu.from)),
   "catalog: every Hub firmware runs emulated, its package from the Hub (RemoteFM1's from the site) with a SHA-256");
{
  const lm = cat.firmwares.find((f) => f.id === "loom"), hubIds = new Set(JSON.parse(readFileSync(new URL("./data/hub-catalog.snapshot.json", import.meta.url), "utf8")).firmwares.map((f) => f.id));
  // (the snapshot is the Hub's of 2026-10-18: Mini-Tone and FMini-1 came later; the live merge gives them the emulated engine)
  const off = cat.firmwares.filter((f) => f.offHub).map((f) => f.id).sort();
  ok(off.join() === "fmini,loom,minitone,speak37" && off.every((id) => !hubIds.has(id)), "catalog: LOOM-8, Speak37, Mini-Tone, FMini-1, not in the Hub snapshot, the off-Hub entries");
  const sp = cat.firmwares.find((f) => f.id === "speak37");
  ok(sp && sp.name === "Speak37" && sp.engines.join() === "native" && sp.defaultEngine === "native" && sp.emu === null && sp.native.version === "1.0"
     && sp.native.glue === "speak37" && sp.category === "Other" && sp.what && !sp.beta && sp.repo === "genkigenki/Speak37",
     "catalog: Speak37's FM-1 build native only (the Hub has its SMK-37 build only)");
  ok(lm && lm.name === "LOOM-8" && lm.engines.join() === "native" && lm.defaultEngine === "native" && !lm.nativeBehind && lm.emu === null && !lm.nativeGated
     && lm.native.version === "0.6.1" && lm.native.wasm.endsWith("loom.wasm") && lm.native.glue === "fm1base" && lm.authorFull === "Charles Vestal"
     && lm.category === "Groovebox" && lm.what && !lm.beta && lm.repo === "charlesvestal/fm1-loom",
     "catalog: LOOM-8 native only (no package to emulate in the snapshot), 0.6.1 (no longer a beta), by Charles Vestal");
}
ok(new Set(cat.firmwares.map((f) => f.id)).size === cat.firmwares.length, "catalog: ids unique");
ok(cat.firmwares.filter((f) => f.native).every((f) => f.defaultEngine === (f.nativeBehind && !f.native.preferred ? "emu" : "native") && f.native.wasm.endsWith(`${f.native.core}.wasm`)),
   "catalog: native is the default where it exists, unless the Hub's version is newer (newest wins)");
{
  const L = await import("./js/live.js"), by = (id) => cat.firmwares.find((f) => f.id === id);
  const du = by("duell"), pg = by("pigeon"), tn = by("tonitor");
  ok(du.native && !du.nativeGated && du.defaultEngine === "native" && du.name === "DUELL" && L.nameOf(du, "native") === "DUELL (unofficial build)" && L.nameOf(du, "emu") === "DUELL"
     && /DUELL \(unofficial build\) core/.test(L.nativeOrigin(du).long),
     "catalog: DUELL public; its native core goes by \"DUELL (unofficial build)\", the Hub's package by DUELL");
  // the Hub's newer PIGEON (a beta) and Tonitor's own version names, as the live merge meets them
  const hub = JSON.parse(readFileSync(new URL("./data/hub-catalog.snapshot.json", import.meta.url), "utf8"));
  Object.assign(hub.firmwares.find((f) => f.id === "pigeon"), { version: "0.5.0-beta.1", pkg: "fw/pigeon-0.5.0-beta.1.fwsc", sha256: "d".repeat(64) });
  Object.assign(hub.firmwares.find((f) => f.id === "tonitor"), { version: "2026.10.22", pkg: "fw/tonitor-2026.10.22.fwsc", sha256: "e".repeat(64) });
  const m = L.mergeLive(cat, hub, {}), mp = m.firmwares.find((f) => f.id === "pigeon"), mt = m.firmwares.find((f) => f.id === "tonitor");
  ok(pg.native.version === "0.4.1" && pg.native.preferred && mp.nativeBehind && mp.defaultEngine === "native" && L.chooseEngine(mp, null, {}) === "native" && L.chooseEngine(mp, "emu", {}) === "emu",
     "catalog: PIGEON native 0.4.1 stays the default under the Hub's 0.5.0-beta.1 (nativeDefault), the beta one switch away");
  ok(tn.native.version === "0.55" && tn.native.hubVersion === "2026.10.22" && mt.version === "2026.10.22" && !mt.nativeBehind && mt.defaultEngine === "native",
     "catalog: Tonitor 0.55 is the Hub's 2026.10.22 (hubVersion): native, not behind");
  ok(["minitone", "fmini"].every((id) => { const f = m.firmwares.find((x) => x.id === id); return f && f.offHub && f.engines.join() === "native"; }),
     "catalog: Mini-Tone and FMini-1 stay native only until the Hub lists them");
}
ok(["x0x", "orbit"].every((id) => cat.firmwares.find((f) => f.id === id).nativeGated), "catalog: gated cores say why they are not native");
ok(["choralroot", "melodee"].every((id) => { const f = cat.firmwares.find((x) => x.id === id); return f.native && !f.nativeGated && f.engines.join() === "native,emu" && f.defaultEngine === "native"; }),
   "catalog: ChoralRoot and Melodee play native by default, emulated (the Hub's package) one switch away");
ok(cat.firmwares.every((f) => cat.categories.includes(f.category)), "catalog: every category is a chip");

// ---- RemoteFM1 itself, a firmware to play: its released package, from this site (not the Hub), checked
{
  const rel = JSON.parse(readFileSync(new URL("../installer/releases.json", import.meta.url), "utf8")), r0 = rel.releases[0];
  const rf = cat.firmwares.find((f) => f.id === "remotefm1");
  ok(rf && rf.name === "RemoteFM1" && rf.version === r0.version && rf.author === "Quixotic7" && rf.license === (r0.license || "GPL-3.0-only")
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
     && pc.firmwares.filter((f) => f.id !== "remotefm1" && !f.offHub && !f.rfm1).every((f) => f.emu.pkg.startsWith("https://thefirmwarehub.com/fw/"))
     && pc.firmwares.find((f) => f.id === "fizz").rfm1.file === cat.firmwares.find((f) => f.id === "fizz").rfm1.file,
     "remotefm1: Pages layout -> <site>/installer/firmware/remote-X.fwsc; every other package still the Hub's; FIZZ's .rfm1 at the same fw/ path");
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

// ---- FIZZ, this site's own .rfm1 (web/site/fw/, data/site-rfm1.json, tools/rfm1/build_fizz.sh): the entry, the file
{
  const L = await import("./js/live.js");
  const { inspect } = await import("../engines/rfm1/engine.js");
  const fz = cat.firmwares.find((f) => f.id === "fizz"), d = JSON.parse(readFileSync(new URL("./data/site-rfm1.json", import.meta.url), "utf8")).firmwares.find((x) => /\/fizz-/.test(x.file));
  ok(fz && fz.name === "FIZZ" && fz.author === "Quixotic7" && fz.authorFull === "Michael P Jones (Quixotic7)" && fz.version === "0.12.0-beta.12" && fz.beta
     && fz.category === "Groovebox" && fz.tag && fz.what && fz.source === "https://github.com/Quixotic7/FIZZ/tree/v0.12.0-beta.12" && fz.repo === "Quixotic7/FIZZ"
     && fz.release === "https://github.com/Quixotic7/FIZZ/releases/tag/v0.12.0-beta.12", "fizz: in the catalog: FIZZ 0.12.0-beta.12 by Quixotic7, beta, Groovebox, its source and release at the tag");
  ok(fz.engines.join() === "rfm1" && fz.defaultEngine === "rfm1" && !fz.native && !fz.emu && !fz.local && L.siteRfm1(fz) && !L.localRfm1(fz)
     && fz.rfm1.file === "fw/fizz-0.12.0-beta.12.rfm1" && /^[0-9a-f]{64}$/.test(fz.rfm1.sha256) && fz.rfm1.size > 0,
     "fizz: the rfm1 engine only, its file this site's (rfm1: file, sha256, size; not a visitor's)");
  const bytes = readFileSync(new URL(fz.rfm1.file, import.meta.url)), sha = createHash("sha256").update(bytes).digest("hex");
  const sums = readFileSync(new URL("./fw/SHA256SUMS", import.meta.url), "utf8");
  ok(sha === fz.rfm1.sha256 && sha === d.sha256 && bytes.length === fz.rfm1.size && sums.includes(`${sha}  fizz-0.12.0-beta.12.rfm1`),
     `fizz: the file is there; its SHA-256 is the catalog's, data/site-rfm1.json's and fw/SHA256SUMS's (${sha.slice(0, 12)}...)`);
  const m = inspect(new Uint8Array(bytes)).manifest;
  ok(m.id === "fizz" && m.version === fz.version && m.license === "MPL-2.0 AND GPL-3.0-only AND MIT AND FTL AND OFL-1.1" && fz.license === m.license
     && /^https:\/\/github\.com\/Quixotic7\/FIZZ@[0-9a-f]{40}$/.test(m.source_url) && fz.rfm1.sourceUrl === m.source_url
     && /^https:\/\/github\.com\/Quixotic7\/FM1VST\/tree\/[0-9a-f]{40}$/.test(m.glue_url) && fz.rfm1.glueUrl === m.glue_url && m.memory_mib <= 32,
     `fizz: the manifest (inspected): ${m.license}, FIZZ at its commit, FM1VST's glue at its commit, ${m.memory_mib} MiB`);
  // the browser, the source link, the merge with the Hub (which lists FIZZ's device package: it needs its data blob, so not emulated)
  ok(L.browse(cat.firmwares, { engine: "native" }).some((f) => f.id === "fizz") && !L.browse(cat.firmwares, { engine: "emu" }).some((f) => f.id === "fizz")
     && L.cardView(fz, "native").engine === "rfm1" && L.cardView(fz, "all").engine === null, "fizz: the Native filter lists it (started on rfm1), the Emulated one does not");
  const s = L.sourceOf(fz, "rfm1", cat.credits);
  ok(s.url === `https://github.com/Quixotic7/FIZZ/tree/${m.source_url.split("@")[1]}` && s.refKind === "commit" && s.glue && /^Quixotic7\/FM1VST @ [0-9a-f]{7}$/.test(s.glue.label)
     && s.title.includes(` · ${m.license} · source at commit`), "fizz: the header's source link: FIZZ at its commit, FM1VST's glue at its commit");
  const hubFizz = { id: "fizz", name: "FIZZ", kind: "groove", author: "Quixotic7", version: "0.12.0-beta.12", pkg: "fw/fizz-0.12.0-beta.12.fwsc", sha256: "d".repeat(64), size: 611496, what: "The Hub's text." };
  const hub0 = JSON.parse(readFileSync(new URL("./data/hub-catalog.snapshot.json", import.meta.url), "utf8"));
  const without = L.mergeLive(cat, hub0, {}), skip = L.mergeLive(cat, { ...hub0, firmwares: [...hub0.firmwares, hubFizz] }, { emuSkip: { fizz: "needs its data" } });
  const withEmu = L.mergeLive(cat, { ...hub0, firmwares: [...hub0.firmwares, hubFizz] }, {});
  const one = (c) => c.firmwares.filter((f) => f.id === "fizz");
  const we = one(withEmu)[0];
  ok(one(without).length === 1 && one(without)[0].engines.join() === "rfm1" && one(skip).length === 1 && one(skip)[0].engines.join() === "rfm1" && !one(skip)[0].emu
     && one(withEmu).length === 1 && we.engines.join() === "rfm1,emu" && we.defaultEngine === "rfm1" && we.emu.pkg === "https://thefirmwarehub.com/fw/fizz-0.12.0-beta.12.fwsc" && L.siteRfm1(we),
     "fizz: the live merge keeps it off the Hub; on the Hub, one entry: the .rfm1 alone where the package can't be emulated, else rfm1 + the Hub's package");
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
  ok(all.length === m.firmwares.length && nativeOnly.every((f) => f.native || f.engines.includes("rfm1")) && nativeOnly.length === m.firmwares.filter((f) => f.native || f.engines.includes("rfm1")).length
     && nativeOnly.some((f) => f.id === "fizz")
     && emuOnly.every((f) => f.engines.includes("emu") && !f.emuNote) && !emuOnly.some((f) => f.id === "bubba" || f.id === "amb") && emuOnly.some((f) => f.id === "felucca"),
     "filter: All everything; Native the cores and the site's .rfm1 (FIZZ); Emulated what fm1-emu runs (not bubba, not a native-only one)");
  ok(L.browse(m.firmwares, { engine: "native", cat: "Groovebox", q: "fel" }).map((f) => f.id).join() === "felucca,melodee,pigeon,skiff" && L.browse(m.firmwares, { engine: "emu", q: "zzzz" }).length === 0,
     "filter: combines with a category chip and the search (Melodee's, PIGEON's, Skiff's blurbs name Felucca)");
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
ok(PRESETS.every((t) => contrast(hex(t.keyText), hex(t.key)) >= 4.5 && contrast(hex(t.label), hex(t.body)) >= 4.5), "themes: legends and labels readable (4.5:1) on every preset");
{ // My theme: the app's derivation keeps it legible whatever the five colours
  const picks = [{ body: "#FFFFFF", key: "#FFFF00", knob: "#FFFFFF", led: "#00FF00", bezel: "#000000" }, { body: "#000000", key: "#101010", knob: "#000000", led: "#FF0000", bezel: "#333333" },
    { body: "#7F7F7F", key: "#808080", knob: "#777777", led: "#FFFFFF", bezel: "#111111" }];
  ok(picks.every((c) => { const t = derived(c); return contrast(hex(t.keyText), hex(t.key)) >= 4.5 && contrast(hex(t.label), hex(t.body)) >= 4.4 && t.name === CUSTOM_NAME; }),
     "themes: My theme's legends and labels stay readable, from any five colours");
  ok(presetNamed("Emulator") === null && presetNamed("tangerine").name === "Tangerine" && presetNamed(CUSTOM_NAME, picks[0]).body === "#FFFFFF",
     "themes: presets by name (an old FM1VST name is unknown: the default applies); My theme from the stored colours");
  ok(PRESETS.every((t) => contrast(accentOf(t), hex("#211E19")) >= 3), "themes: the sheet's accent reads on the sheet for every preset");
}

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
ok(V({ state: "connected", transport: "midi", ports: { name: 'midi "FM-1 1F2D"' } }).via === "Bluetooth" &&
   V({ state: "connected", transport: "midi", ports: { name: 'midi "FM-1"' } }).via === "MIDI" &&
   V({ state: "connected", transport: "midi", ports: { name: 'midi "ChoralRoot FM-1"' } }).via === "MIDI" &&
   / over Bluetooth, RemoteFM1 0\.2\.0/.test(V({ state: "connected", transport: "midi", ports: { name: 'midi "FM-1 00AF"' }, unit: { fwVer: "0.2.0" } }).detail),
   "fm1: RemoteFM1 0.2 over Bluetooth MIDI (its port \"FM-1 1F2D\") says Bluetooth");
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
{ // the browser's MIDI prompt pending (remote.js status.midiPrompt): said, not mistaken for "no FM-1 answered"
  const vp = V({ state: "searching", midiPrompt: true });
  ok(vp.tone === "busy" && vp.prompt && vp.detail === MIDI_PROMPT_TEXT && /Allow MIDI: your browser is asking/.test(vp.title), "fm1: MIDI prompt pending -> amber, \"Allow MIDI: your browser is asking\"");
  ok(viewOf({ state: "searching", midiPrompt: true }, true).detail === MIDI_PROMPT_TEXT, "fm1: the prompt wins over \"nothing answered\"");
  ok(errorText("MIDI is blocked for this site (its permission is denied)") === MIDI_DENIED_TEXT && /site-settings icon/.test(MIDI_DENIED_TEXT),
     "fm1: a site with MIDI denied -> how to allow it again (site settings)");
  ok(/allow MIDI/.test(errorText("MIDI access (with SysEx) was refused")), "fm1: a refusal at the prompt keeps its text");
  const ls = new Set(), timers = [];
  let st = { state: "idle" }, connects = 0;
  const fake = { get status() { return st; }, onStatus: (cb) => ls.add(cb), connect: async () => { connects++; }, disconnect: async () => { push({ state: "idle" }); }, setQuiet: (q) => push({ ...st, quiet: q }) };
  const push = (x) => { st = x; for (const cb of ls) cb(x); };
  const link = new Fm1Link(fake, { setTimer: (f, ms) => { const t = { f, ms, live: true }; timers.push(t); return t; }, clearTimer: (t) => { t.live = false; } });
  const fire = (ms) => { for (const t of timers.filter((t) => t.live && t.ms === ms)) { t.live = false; t.f(); } };
  link.connect(); push({ state: "searching" }); push({ state: "searching", midiPrompt: true });
  fire(10000);
  ok(link.view().tone === "busy" && link.view().prompt && !link.notFound, "fm1 link: 10 s on the prompt -> still \"Allow MIDI\", not red");
  fire(30000);
  const late = link.view();
  ok(late.tone === "bad" && late.label === "Try again" && late.detail === MIDI_LATE_TEXT && /try again/.test(late.detail), "fm1 link: 30 s on the prompt -> the hint stays, red, Try again");
  link.connect();
  ok(connects === 2 && link.view().tone === "busy" && link.view().prompt, "fm1 link: Try again -> asks again, amber");
  push({ state: "searching" });
  ok(link.view().tone === "busy" && !link.view().prompt, "fm1 link: allowed -> searching for the FM-1");
  fire(10000);
  ok(link.view().tone === "bad" && link.notFound, "fm1 link: then 10 s with no FM-1 -> red, as before");
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
  { // a click with the browser's prompt left unanswered, then allowed
    const { w, link, run } = await page("prompt", (w) => { w.unit = w.addUnit({ midiName: "FM-1" }); });
    const ask = w.navigator.requestMIDIAccess;
    let allow = null;
    w.navigator.requestMIDIAccess = (o) => new Promise((r) => { allow = () => r(ask(o)); });
    link.connect();
    await run(1500);
    ok(link.view().tone === "busy" && link.view().prompt && link.view().detail === MIDI_PROMPT_TEXT, "fm1 click, MIDI prompt pending: \"Allow MIDI: your browser is asking\"");
    allow();
    await run(2000);
    ok(link.view().tone === "on" && !link.status.midiPrompt, "fm1 click: allowed -> the hint goes, connected");
    await Promise.all([link.disconnect(), run(400)]);
  }
  { // a click on a site whose MIDI is denied, no USB unit: how to allow it again, nothing asked
    const { w, link, run } = await page("denied", (w) => { w.unit = w.addUnit({ midiName: "FM-1" }); });
    link.connect();
    await run(1500);
    ok(link.view().tone === "bad" && link.view().detail === MIDI_DENIED_TEXT && w.midiRequests.length === 1, "fm1 click, MIDI denied for the site: how to re-allow it (site settings)");
    await Promise.all([link.disconnect(), run(400)]);
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
  ok(rf.url === "https://github.com/Quixotic7/RemoteFM1/tree/v" + F("remotefm1").version && rf.title.includes(` · ${F("remotefm1").license} · source at v`), "source: RemoteFM1, this repo at its release's tag");
  ok(sourceOf({ id: "local-FM-1.fwsc", name: "FM-1", local: true, source: null }, "emu") === null && sourceOf({ id: "y", name: "Y", source: null, repo: null }, "emu") === null && sourceOf(null, "emu") === null,
     "source: stock M-VAVE / a local .fwsc -> none");
  // the visitor's .rfm1 file (js/library.js): its manifest's source_url <repo>@<commit>, and its glue_url
  const { entryOf } = await import("./js/library.js");
  const re = entryOf({ sha256: "ab".repeat(32), fileName: "felucca-1.6.1.rfm1", size: 1072881, manifest: { rfm1: 1, abi: 6, id: "felucca", name: "Felucca", version: "1.6.1",
    license: "GPL-3.0-only", source_url: "https://github.com/hugelton/Felucca@a53ecf428c29c780108fd70a31b71a32a6b5c313", authors: ["Leo Kuroshita (Hügelton Instruments)"],
    glue_url: "https://github.com/Quixotic7/FM1VST/tree/0da04482a3af", built_by: "FM1VST 0.6.2" } });
  const rs = sourceOf(re, "rfm1", credits);
  ok(re.local && re.engines.join() === "rfm1" && re.defaultEngine === "rfm1" && re.license === "GPL-3.0-only" && re.author === "Leo Kuroshita (Hügelton Instruments)"
     && rs.url === "https://github.com/hugelton/Felucca/tree/a53ecf428c29c780108fd70a31b71a32a6b5c313" && rs.refKind === "commit" && rs.repo === "hugelton/Felucca"
     && rs.glue.url === "https://github.com/Quixotic7/FM1VST/tree/0da04482a3af" && rs.glue.label === "Quixotic7/FM1VST @ 0da0448"
     && /Felucca 1\.6\.1 by Leo Kuroshita .* · GPL-3\.0-only · source at commit a53ecf4, the commit this \.rfm1 was built from/.test(rs.title),
     "source: an .rfm1 file, its manifest's <repo>@<commit> as the tree at that commit, its glue_url as the glue");
  const L2 = await import("./js/live.js");
  ok(L2.chooseEngine(re, null, { engine: { felucca: "native" } }) === "rfm1" && L2.nameOf(re, "rfm1") === "Felucca",
     "source: an .rfm1 entry runs on the rfm1 engine, whatever engine the catalog's Felucca was picked on");
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
    ok(nat.length === 21 && nat.every((f) => [...pins].some(([path, c]) => c === f.native.commit && urls.get(path) === f.native.repo)) && nat.every((f) => existsSync(join(VST, "core-glue", f.native.glue))),
       "build_catalog: each native core's commit is FM1VST's pin (ls-tree), its repo the submodule's, its glue folder there");
    const cj = JSON.parse(readFileSync(new URL("./data/cores.json", import.meta.url), "utf8"));
    const head = execFileSync("git", ["-C", VST, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    ok(cat.credits.fm1vst.commit === head && cj.fm1vst === head && nat.every((f) => { const c = cj.cores.find((x) => x.core === f.native.core); return c.commit === f.native.commit && c.repo === f.native.repo && c.glue === f.native.glue && c.version === f.native.version; }),
       "build_catalog: data/cores.json's fallback commits agree with web/vendor/FM1VST (the Pages check job builds without it)");
  }
}

// ---- what may be deployed (tools/site_guard.mjs, pages.yml): the catalog lists only the native cores the site has
// (build_catalog.mjs --native-dir), and the deploy guard refuses a site that lacks what its catalog names
{
  const G = await import("./tools/site_guard.mjs");
  const fs = await import("node:fs");
  const exp = G.ungatedCores(JSON.parse(readFileSync(new URL("./data/cores.json", import.meta.url), "utf8")).cores);
  ok(exp.length === 21 && exp.includes("speak37") && !exp.includes("x0x"), "guard: the expected native cores are the 21 un-gated ones");
  // the catalog filter: a published folder without felucca's (a Hub firmware) and speak37's (native only off the Hub)
  const dir = fs.mkdtempSync(join(tmpdir(), "native-dir-")), outF = join(dir, "catalog.json"), outA = join(dir, "catalog-all.json");
  for (const id of exp) if (id !== "felucca" && id !== "speak37") fs.writeFileSync(join(dir, `${id}.wasm`), "");
  const BC = fileURLToPath(new URL("./tools/build_catalog.mjs", import.meta.url));
  const log = execFileSync(process.execPath, [BC, "--layout", "pages", "--out", outF, "--native-dir", dir], { encoding: "utf8", env: { ...process.env, GITHUB_ACTIONS: "" } });
  execFileSync(process.execPath, [BC, "--layout", "pages", "--out", outA], { stdio: "ignore" });
  const cf = JSON.parse(readFileSync(outF, "utf8")), ca = JSON.parse(readFileSync(outA, "utf8"));
  const byF = new Map(cf.firmwares.map((f) => [f.id, f])), fe = byF.get("felucca");
  ok(cf.firmwares.filter((f) => f.native).map((f) => f.id).sort().join() === ca.firmwares.filter((f) => f.native && !["felucca", "speak37"].includes(f.id)).map((f) => f.id).sort().join()
     && cf.firmwares.filter((f) => f.native).length === 19,
     "catalog filter: --native-dir lists as native only the 19 cores whose .wasm is there");
  ok(fe && fe.native === null && fe.engines.join() === "emu" && fe.defaultEngine === "emu" && fe.emu.pkg.startsWith("https://thefirmwarehub.com/fw/"),
     "catalog filter: Felucca, its core missing, falls back to the emulated engine alone");
  ok(!byF.has("speak37") && cf.firmwares.length === ca.firmwares.length - 1 && ca.firmwares.every((f) => f.id === "speak37" || byF.has(f.id)),
     "catalog filter: Speak37 (native only, off the Hub) is left out; every other entry stays");
  ok(/warning: native cores not published: felucca speak37:/.test(log) && /2 native cores left out/.test(log),
     "catalog filter: a warning names the missing cores");
  // the guard on a fake site: what the filtered catalog names, and the engines, managers, installer
  const mgr = { managers: [{ id: "choralroot", path: "choralroot/" }, { id: "pigeon", path: "pigeon/webapp/pigeon/" }] };
  const siteOf = (c) => new Set([...c.firmwares.filter((f) => f.native).map((f) => f.native.wasm), "engines/native/engine.js", "engines/rfm1/engine.js", "engines/common/shared.js",
    "engines/emu/engine.js", "engines/emu/fm1_web.wasm", "engines/emu/catalog.mjs", ...c.firmwares.filter((f) => f.emu && f.emu.from === "site").map((f) => f.emu.pkg),
    ...c.firmwares.filter((f) => f.rfm1 && f.rfm1.from === "site").map((f) => f.rfm1.file),
    "installer/index.html", "managers/fm1-shim.js", "managers/choralroot/index.html", "managers/pigeon/webapp/pigeon/index.html", "remote/remote.js", "build/remote/remotefm1.mjs"]);
  const guard = (c, files, managers = mgr) => G.guardSite({ catalog: c, has: (p) => files.has(p), expected: exp, managers });
  const g0 = guard(cf, siteOf(cf));
  ok(!g0.errors.length && g0.missing.join() === "felucca,speak37" && g0.warnings.some((w) => /2 of 21 native cores are missing/.test(w)),
     "guard: two cores missing, the catalog without them: deployed, with a warning");
  const g1 = guard(ca, siteOf(cf));
  ok(g1.errors.length === 2 && g1.errors.every((e) => /^(felucca|speak37): the catalog's native engine/.test(e)),
     "guard: a catalog naming a native .wasm the site lacks is not deployed");
  const drop = (n) => { const s = siteOf(ca); exp.slice(0, n).forEach((id) => s.delete(`engines/native/${id}.wasm`)); return s; };
  const c7 = { ...ca, firmwares: ca.firmwares.map((f) => (f.native && exp.slice(0, 7).includes(f.native.core) ? { ...f, native: null, engines: ["emu"] } : f)) };
  const c8 = { ...ca, firmwares: ca.firmwares.map((f) => (f.native && exp.slice(0, 8).includes(f.native.core) ? { ...f, native: null, engines: ["emu"] } : f)) };
  const g7 = guard(c7, drop(7)), g8 = guard(c8, drop(8)), gAll = guard({ ...ca, firmwares: ca.firmwares.filter((f) => !f.offHub).map((f) => ({ ...f, native: null, engines: ["emu"] })) }, drop(21));
  ok(!g7.errors.length && g8.errors.length === 1 && /8 of 21 native cores are missing \(more than a third/.test(g8.errors[0]) && gAll.errors.length === 1,
     "guard: 7 of 21 cores missing deploys; 8 (more than a third), or all (no emcc), fail the job");
  const noEmu = siteOf(cf); noEmu.delete("engines/emu/fm1_web.wasm");
  const noPkg = siteOf(cf); [...noPkg].filter((p) => p.startsWith("installer/firmware/")).forEach((p) => noPkg.delete(p));
  ok(guard(cf, noEmu).errors.some((e) => /emulated engine's wasm engines\/emu\/fm1_web\.wasm/.test(e)) && guard(cf, noPkg).errors.some((e) => /^remotefm1: its package/.test(e)),
     "guard: the emulated engine's fm1_web.wasm, or RemoteFM1's own package, missing: not deployed");
  const noMgr = siteOf(cf); noMgr.delete("managers/pigeon/webapp/pigeon/index.html");
  const noShim = siteOf(cf); noShim.delete("managers/fm1-shim.js");
  const noMod = siteOf(cf); noMod.delete("build/remote/remotefm1.mjs");
  const noRemote = siteOf(cf); noRemote.delete("build/remote/remotefm1.mjs"); noRemote.delete("remote/remote.js");
  ok(guard(cf, noMgr).errors.some((e) => /manager pigeon: managers\/pigeon\/webapp\/pigeon\/index\.html/.test(e)) && guard(cf, noShim).errors.length === 1
     && guard(cf, noMod).errors.length === 1 && !guard(cf, noRemote).errors.length && !guard(cf, siteOf(cf), null).errors.length,
     "guard: a listed manager's page, the shim, remote.js's module missing: not deployed (no managers, no remote: deployed)");
  const noShared = siteOf(cf); noShared.delete("engines/common/shared.js");
  const noRfm1 = siteOf(cf); noRfm1.delete("engines/rfm1/engine.js");
  ok(guard(cf, noShared).errors.length === 2 && guard(cf, noShared).errors.every((e) => /without engines\/common\/shared\.js/.test(e))
     && guard(cf, noRfm1).errors.length === 1 && /the \.rfm1 engine engines\/rfm1\/engine\.js/.test(guard(cf, noRfm1).errors[0]),
     "guard: the .rfm1 engine, or the module it and the native engine import, missing: not deployed");
  const noFizz = siteOf(cf); noFizz.delete(byF.get("fizz").rfm1.file);
  const shaOf = (bad) => (p) => (p === byF.get("fizz").rfm1.file && !bad ? byF.get("fizz").rfm1.sha256 : "0".repeat(64));
  ok(guard(cf, noFizz).errors.length === 1 && /^fizz: its \.rfm1 fw\/fizz-[\w.-]+\.rfm1 \(hosted here\) is not in the site/.test(guard(cf, noFizz).errors[0])
     && !G.guardSite({ catalog: cf, has: (p) => siteOf(cf).has(p), expected: exp, managers: mgr, sha: shaOf(false) }).errors.length
     && /^fizz: .* SHA-256 differs/.test(G.guardSite({ catalog: cf, has: (p) => siteOf(cf).has(p), expected: exp, managers: mgr, sha: shaOf(true) }).errors.join()),
     "guard: the site's own .rfm1 (FIZZ) missing, or not the catalog's SHA-256: not deployed");
  const ann = G.nativeAnnotations({ built: ["felucca"], failed: [{ id: "speak37", stage: "configure", log: "ModuleNotFoundError: No module named 'numpy'" }] });
  const none = G.nativeAnnotations(null);
  ok(ann.length === 1 && ann[0].level === "warning" && /speak37/.test(ann[0].title) && /numpy/.test(ann[0].message) && none.length === 1 && none[0].level === "error",
     "guard: build.sh's result.json -> a warning per failed core with its log; none -> an error");
  for (const f of fs.readdirSync(dir)) fs.unlinkSync(join(dir, f));
  fs.rmdirSync(dir);
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
    accepted: [acc("felucca", "1.6.1"), acc("amb", "1.2"), acc("duell", "1.0")] }));
  execFileSync(process.execPath, [fileURLToPath(new URL("./tools/build_catalog.mjs", import.meta.url)), "--layout", "pages", "--out", outC, "--native-ext", idx], { stdio: "ignore" });
  const ec = JSON.parse(readFileSync(outC, "utf8")), by = (id) => ec.firmwares.find((f) => f.id === id);
  unlinkSync(idx); unlinkSync(outC);
  const fe = by("felucca"), am = by("amb"), du = by("duell");
  ok(fe.native.by === "author" && fe.native.wasm === "engines/native/ext/felucca/1.6.1/felucca.wasm" && fe.nativeAlt.by === "fm1vst" && fe.defaultEngine === "native"
     && am.native.by !== "author" && am.nativeAlt.by === "author" && am.nativeAlt.version === "1.2" && /newer/.test(am.nativeWhy)
     && du.native && du.native.by === "author" && !du.nativeGated && du.engines.join() === "native,emu" && !du.nativeBehind
     && ec.nativeAuthors.accepted.length === 3 && ec.nativeAuthors.rejected[0].why === "test" && nativeOrigin(fe).short === "native · built by the author"
     && du.nativeAlt && du.nativeAlt.by === "fm1vst"
     && ec.firmwares.filter((f) => f.native && f.native.by !== "author" || f.nativeAlt && f.nativeAlt.by === "fm1vst").length === 21,
     "native authors: the catalog: Felucca and DUELL the author's (same version), AMB-1 FM1VST's (newer); FM1VST's 21 still counted");
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
  // the drawing: the hint letters are drawn only when on (fillText calls on a stand-in canvas)
  const { DeviceView, pickLayout, keybedGeometry } = await import("./js/panel.js");
  let texts = [];
  const g = new Proxy({}, { get: (_, k) => (k === "fillText" ? (t) => texts.push(t) : k === "measureText" ? () => ({ width: 10 }) : () => {}), set: () => true });
  const dv = new DeviceView({ getContext: () => g, width: 0, height: 0 }, new Controls(() => {}), null);
  dv.layout(1194, 768, 1);
  const draw = (on) => { dv.setHints(on); texts = []; dv.drawKeybed(g, dv.pal); for (const b of dv.L.btns) dv.drawButton(g, dv.pal, b, false, 0, 0); return texts.slice(); };
  const off = draw(false), on = draw(true);
  ok(dv.hints === true && off.length === 14 && on.includes("S") && on.includes("Z") && on.length > off.length, "hints: the computer-key letters drawn only when on");
  ok(pickLayout(390, 760) === "phonePortrait" && pickLayout(844, 390) === "phoneLandscape" && pickLayout(1440, 848) === "padLandscape" && pickLayout(834, 1100) === "padPortrait",
     "faceplate: the layout follows the stage's shape (phone upright / on its side, tablet or desktop)");
  const kg = keybedGeometry(358, 272, 40), row = keybedGeometry(1098, 362);
  ok(kg.scroll && kg.keys.length === 27 && kg.maxOffset > 0 && row.keys.filter((k) => k.natural).length === 16 && row.maxOffset === 0,
     "faceplate: 27 keys, 16 naturals; a phone's keybed scrolls, a wide one shows them all");
  dv.layout(390, 760, 1);
  const kb = dv.L.keybed, hitKey = (k) => { const c = dv.L.kg, key = c.keys.find((x) => x.k === k); return dv.hit(kb.x + c.stage.x + c.shift(dv.keyOff()) + key.x + key.w / 2, kb.y + c.stage.y + key.y + key.h / 2); };
  ok(hitKey(9) && hitKey(9).kind === KM.KEY && hitKey(9).idx === 9 && dv.hit(kb.x + 30, kb.y + 20).kind === "strip", "faceplate: a phone's keybed: a key hits its note; the range strip above is the strip");
  dv.stripTo(kb.x + kb.w - 20, false);
  ok(dv.keyOff() === dv.L.kg.maxOffset, "faceplate: a tap at the strip's right end scrolls the keys to the end");
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

// ---- MIDI learn (js/midi-map.js): sources, modes, speed, pickup, buttons, consumption, rebinding
{
  const MM = await import("./js/midi-map.js");
  const cc = (n, v, ch = 1) => [0xB0 | (ch - 1), n, v], on = (n, v = 100, ch = 1) => [0x90 | (ch - 1), n, v], off = (n, ch = 1) => [0x80 | (ch - 1), n, 0];
  const steps = (acts) => (acts || []).filter((a) => a.kind === "turn").reduce((s, a) => s + a.steps, 0);
  ok(MM.TARGETS.length === 22 && MM.TARGETS.filter((t) => t.kind === "enc").map((t) => t.id).join() === "SELECT,ALGORITHM,PRESETS,KN1,KN2,KN3,KN4"
     && MM.targetOf("KN1").role === E.K1 && MM.targetOf("HOME").b === B.HOME && MM.targetOf("OCT+").b === B.OCTUP && MM.knobTarget(E.MASTER) === "MASTER",
     "midi map: 7 encoders, MASTER and the 14 buttons");
  const p = MM.parseMsg(on(60, 0, 3));
  ok(MM.srcKey(MM.parseMsg(cc(21, 5))) === "cc:1:21" && p.type === "note" && !p.on && p.ch === 3 && MM.srcKey(p) === "note:3:60" && MM.parseMsg([0xE0, 0, 64]) === null,
     "midi map: a CC and a note keyed by channel and number; note-on at 0 is a note-off; pitch bend is none");
  // absolute, at each speed
  for (const [sp, want] of [[1, [0, 1, 3, 2]], [2, [0, 2, 6, 4]], [0.5, [0, 0, 1, 1]]]) {
    const m = new MM.MidiMap([{ src: "cc:1:21", target: "KN1", mode: "abs" }], { speed: sp });
    const got = [cc(21, 40), cc(21, 41), cc(21, 43), cc(21, 42)].map((d) => steps(m.feed(d)));
    const acc = got.reduce((a, x, i) => (a.push((a[i - 1] || 0) + x), a), []);
    ok(acc.join() === want.join(), `midi map: absolute at ${sp}x: 40 41 43 42 -> ${acc.join(" ")} detents (the first value only sets the start)`);
  }
  {
    const m = new MM.MidiMap([{ src: "cc:1:21", target: "KN1", mode: "abs" }], { speed: 0.5 });
    m.seed("cc:1:21", 10);
    const s1 = steps(m.feed(cc(21, 11))), s2 = steps(m.feed(cc(21, 12))), s3 = steps(m.feed(cc(21, 13))), s4 = steps(m.feed(cc(21, 10)));
    ok(s1 === 0 && s2 === 1 && s3 === 0 && s4 === -1, "midi map: absolute at 1/2x carries the half step over (11 12 13: 0 1 0; back to 10: -1)");
  }
  // relative
  const rel = (mode, vals) => { const m = new MM.MidiMap([{ src: "cc:1:30", target: "SELECT", mode }]); return vals.map((v) => steps(m.feed(cc(30, v)))); };
  ok(rel("rel64", [65, 66, 63, 60, 64]).join() === "1,2,-1,-4,0", "midi map: relative binary offset: 65 +1, 66 +2, 63 -1, 60 -4");
  ok(rel("rel2c", [1, 3, 127, 125, 64]).join() === "1,3,-1,-3,-64", "midi map: relative two's complement: 1 +1, 3 +3, 127 -1, 125 -3");
  ok(rel("relsign", [1, 3, 65, 67]).join() === "1,3,-1,-3", "midi map: relative sign bit: 1 +1, 3 +3, 65 -1, 67 -3");
  { const m = new MM.MidiMap([{ src: "cc:1:30", target: "SELECT", mode: "rel64" }], { speed: 2 }); ok(steps(m.feed(cc(30, 65))) === 2, "midi map: the speed scales a relative encoder too (2x: 65 -> +2)"); }
  // the guess
  const G = MM.guessMode;
  ok(G([65, 65, 66]) === "rel64" && G([63, 62, 63]) === "rel64" && G([65]) === "rel64", "midi map guess: values hugging 64 -> binary offset");
  ok(G([1, 1, 2]) === "rel2c" && G([127, 126, 127]) === "rel2c" && G([1, 127, 1]) === "rel2c", "midi map guess: values in 1..15 / 113..127 -> two's complement");
  ok(G([40, 42, 45]) === "abs" && G([10, 11, 12, 13]) === "abs" && G([60, 61, 62, 63, 64, 65]) === "abs" && G([0, 127]) === "abs" && G([]) === "abs",
     "midi map guess: a pot sweeping (even at the bottom, or through 64) -> absolute");
  // buttons: notes and CCs, several sources on one button (SRC.MAP holds)
  {
    const m = new MM.MidiMap([{ src: "note:1:60", target: "HOME" }, { src: "cc:1:64", target: "HOME" }]);
    const a1 = m.feed(on(60)), a2 = m.feed(cc(64, 127)), a3 = m.feed(off(60)), a4 = m.feed(cc(64, 0)), a5 = m.feed(cc(64, 63));
    ok(a1.length === 1 && a1[0].kind === "btn" && a1[0].b === B.HOME && a1[0].down && !a2.length && !a3.length && a4.length === 1 && !a4[0].down && !a5.length,
       "midi map: a note presses and releases a button, a CC >= 64 / < 64 too; two sources hold it until both let go");
    const c = new Controls(() => {});
    c.holdBtn(B.HOME, SRC.POINTER, true); c.holdBtn(B.HOME, SRC.MAP, true); c.holdBtn(B.HOME, SRC.MAP, false);
    ok(SRC.MAP === 64 && c.btnHeld(B.HOME) && Object.values(SRC).filter((v) => v === 64).length === 1, "midi map: SRC.MAP is its own bit, composing with the pointer's hold");
  }
  // consumption: learned messages give actions (maybe none), unlearned ones null; notes can't go on knobs
  {
    const m = new MM.MidiMap([{ src: "note:1:60", target: "HOME" }, { src: "cc:1:21", target: "KN1" }, { src: "note:1:61", target: "KN1" }]);
    ok(Array.isArray(m.feed(on(60))) && Array.isArray(m.feed(cc(21, 3))) && m.feed(on(62)) === null && m.feed(cc(21, 3, 2)) === null && m.feed(on(61)) === null,
       "midi map: a learned CC or note is consumed, another number or channel flows on; a stored note on a knob is dropped");
    ok(m.list()[1].mode === "abs" && MM.describe(m.list()[1]) === "KN1 ← CC 21 ch 1 · absolute" && MM.describe(m.list()[0]) === "HOME ← note 60 ch 1", "midi map: the list's lines");
  }
  // MASTER and its pickup
  {
    const m = new MM.MidiMap([{ src: "cc:1:22", target: "MASTER" }]);
    let master = 768;   // (96.0 in CC steps)
    const feed = (v) => { const a = m.feed(cc(22, v), { master }); for (const x of a) if (x.kind === "master") master = x.value; return a; };
    const a1 = feed(20), a2 = feed(40), a3 = feed(60);
    ok(a1.length === 1 && a1[0].kind === "pickup" && a1[0].first && a2[0].kind === "pickup" && !a2[0].first && a3[0].kind === "pickup" && master === 768,
       "midi map pickup: a pot below MASTER is ignored (MASTER stays 768; the first ignore says so once)");
    const a4 = feed(100);
    ok(a4.length === 1 && a4[0].kind === "master" && master === Math.round(100 * 1023 / 127), `midi map pickup: crossing MASTER's position (60 -> 100) picks it up: MASTER ${master}`);
    feed(127); feed(0);
    ok(master === 0, "midi map pickup: then it follows directly, 0..127 -> 0..1023 (127 -> 1023, 0 -> 0)");
    master = 512;   // the mouse moved MASTER
    const a5 = feed(10);
    ok(a5[0].kind === "pickup" && a5[0].first && master === 512, "midi map pickup: after MASTER moves by the mouse, the pot waits again");
    const a6 = feed(63);
    ok(a6[0].kind === "master" && master === Math.round(63 * 1023 / 127), "midi map pickup: within 2 steps of MASTER (63 ~ 512) picks it up");
    const n = new MM.MidiMap([{ src: "cc:1:22", target: "MASTER" }]);
    ok(n.feed(cc(22, 95), { master: 768 })[0].kind === "master", "midi map pickup: a fresh map (a reload) picks up at once when the pot is within 2 steps");
  }
  // rebinding and forgetting; learn
  {
    const m = new MM.MidiMap([]), L = new MM.Learn(m);
    ok(L.take(cc(21, 5)) === null, "learn: off, nothing taken");
    L.start(); L.select("KN1");
    const r1 = L.take(cc(21, 40));
    ok(r1.fresh && r1.bound.target === "KN1" && r1.bound.mode === "abs" && m.list().length === 1, "learn: KN1 picked, CC 21 binds to it (absolute)");
    L.take(cc(21, 41)); L.take(cc(21, 42)); L.take(cc(21, 43));
    ok(m.get("cc:1:21").mode === "abs", "learn: a pot sweeping stays absolute");
    L.stop(); const t44 = steps(m.feed(cc(21, 44))); L.start(); L.select("KN1"); L.take(cc(21, 44));
    ok(t44 === 1, "learn: after learn, the pot turns from its last learned value (43 -> 44: 1 detent)");
    L.select("KN2");
    const r2 = L.take(cc(30, 65)); L.take(cc(30, 65)); L.take(cc(30, 66));
    ok(r2.fresh && m.get("cc:1:30").mode === "rel64", "learn: an encoder sending 65s is binary offset");
    L.select("KN3");
    L.take(cc(21, 44));
    ok(m.get("cc:1:21").target === "KN3" && m.list().length === 2, "learn: binding a source already bound moves it (CC 21: KN1 -> KN3)");
    L.take(cc(22, 50));
    ok(m.sourcesOf("KN3").length === 2, "learn: one target takes several sources");
    L.select("KN4");
    const rj = L.take(on(60));
    ok(rj.reject && /CC/.test(rj.reject) && !m.has("note:1:60"), "learn: a note on a knob is refused");
    L.select("HOME");
    const rb = L.take(on(60)), ro = L.take(off(60)), rx = L.take(off(61));
    ok(rb.fresh && rb.bound.target === "HOME" && !rb.bound.mode && ro && !ro.bound && rx === null, "learn: a note binds to a button; its note-off is consumed, another note-off flows on");
    L.select("KN3");
    ok(L.forgetSelected() === 2 && !m.has("cc:1:21") && !m.has("cc:1:22") && m.has("cc:1:30"), "learn: Forget removes the picked control's bindings");
    const back = new MM.MidiMap(JSON.parse(JSON.stringify(m.list())));
    ok(back.list().length === 2 && back.get("cc:1:30").mode === "rel64" && new MM.MidiMap([{ src: "cc:0:1", target: "KN1" }, { src: "cc:1:1", target: "NOPE" }, null]).list().length === 0,
       "learn: the list round-trips through JSON (prefs); bad records are left out");
  }
}

// ---- the OMX-27 in REMOTE mode: the wire (js/omx27.js), its views (js/omx27-views.js), K5's pickup (midi-map.js)
{
  const X = await import("./js/omx27.js"), V = await import("./js/omx27-views.js"), MM = await import("./js/midi-map.js");
  const hexOf = (b) => Buffer.from(b).toString("hex");
  ok(X.MODE_REMOTE === 9 && hexOf(X.enterRemote()) === "f07d00005105" + "09f7" && hexOf(X.sysex(0x5B)) === "f07d00005bf7",
     "omx: MODE_REMOTE is 9 (config.h OMXMode); the MODE command F0 7D 00 00 51 05 09 F7; the LED latch 5B");
  const pk = X.parse(X.keyMsg(13, 3)), pe = X.parse(Uint8Array.of(0xF0, 0x7D, 0, 0, 0x51, 1, 0, 4, 0xF7)), pp = X.parse(X.potMsg(4, 100, 12345));
  ok(pk.type === "key" && pk.key === 13 && pk.ev === 3 && pe.type === "enc" && pe.delta === -4 && X.parse(X.encMsg(3)).delta === 3
     && pp.type === "pot" && pp.pot === 4 && pp.v === 100 && pp.hires === 12345 && X.parse(Uint8Array.of(0xF0, 0x7D, 0, 0, 0x51, 2, 1, 0xF7)).z === 1
     && X.parse(X.statusMsg(4)).s === 4 && X.parse([0xF0, 0x7E, 0, 0, 0x51, 0, 1, 1, 0xF7]) === null && X.parse([0x90, 60, 100]) === null,
     "omx: key, encoder (dir 0 CCW, 2 CW, amt), button, pot (v7 + 14-bit) and status decode; others are none");
  ok(hexOf(X.enc7([0x80, 0x7F, 0xFF, 0, 0, 0, 0, 0x81])) === "05007f7f00000000" + "0101" && X.enc7(new Uint8Array(32)).length === 37,
     "omx: enc7 (a high-bits byte, then 7 low parts): 32 bytes are 37");
  // remote_test.py's _pack and _enc7 (PIL, rotate 180) on this image gave these bytes
  const bits = new Uint8Array(128 * 32);
  for (let y = 0; y < 32; y++) for (let x = 0; x < 128; x++) if ((x * 7 + y * 13) % 11 < 3 || x === y || (x < 9 && y < 4)) bits[y * 128 + x] = 1;
  const fb = X.pack(bits);
  ok(createHash("sha256").update(fb).digest("hex") === "dab809b51637adc3ea0bdc5dfe376069213bb6c3a8f3349e3141860eb666a25f"
     && hexOf(fb.subarray(0, 16)) === "8410430c30c208218618618410430c30" && hexOf(fb.subarray(496)) === "10430c30c20821f6f9f3f4f8f3fcf0f2"
     && hexOf(X.enc7(fb.subarray(160, 192))) === "240821061861041048430c30420821060418610410430c3049420821061861040010430c30",
     "omx: the screen packs as remote_test.py does (SSD1306 pages, 180 deg; chunk 5 enc7'd the same)");
  ok(X.unpack(fb).every((b, i) => b === bits[i]) && X.dec7(X.enc7(fb.subarray(0, 32)), 32).every((b, i) => b === fb[i]), "omx: unpack and dec7 undo them");
  ok(X.isOmxPortName("OMX-27") && X.isOmxPortName("omx-27-v3 MIDI 1") && !X.isOmxPortName("FM-1"), "omx: its port is any name with OMX in it");
  // the session: changed LEDs only, a batch past 8, one frame in flight (ACK or 250 ms), 15 fps, only changed chunks
  {
    let t = 0;
    const sent = [], S = new X.OmxSession({ send: (b) => sent.push(b), now: () => t });
    S.enter();
    ok(sent.length === 1 && hexOf(sent[0]) === hexOf(X.enterRemote()) && !S.tick(), "session: Connect sends the MODE command; nothing more before 52 02");
    S.feed(X.statusMsg(2)); sent.length = 0;
    S.tick();
    const cmds = () => sent.map((b) => b[4]);
    ok(S.active && cmds().filter((c) => c === 0x5A).length === 1 && cmds().filter((c) => c === 0x5C).length === 16 && cmds().filter((c) => c === 0x5D).length === 1,
       "session: in REMOTE mode, every LED (one batch) and the whole screen (16 chunks, latched) go out");
    sent.length = 0;
    const leds = Array.from({ length: 27 }, () => [0, 0, 0]); leds[5] = [127, 0, 0]; S.setLeds(leds);
    const f2 = new Uint8Array(512); f2[100] = 1; S.setFrame(f2);
    t = 10; S.tick();
    ok(sent.length === 0, "session: within 30 ms nothing more goes; the frame waits for its ACK");
    t = 40; S.tick();
    ok(cmds().join() === "89,91", "session: one changed LED: 59 5 r g b, then 5B");
    S.feed(X.statusMsg(4)); t = 80; sent.length = 0; S.tick();
    ok(cmds().join() === "92,93" && sent[0][5] === 3, "session: after the ACK, only the changed chunk (3) and 5D");
    sent.length = 0; f2[0] = 9; t = 100; S.tick();
    ok(sent.length === 0, "session: no ACK yet: no frame");
    t = 340; S.tick();
    ok(cmds().join() === "92,93", "session: a lost ACK: the next frame after 250 ms");
    S.feed(X.statusMsg(4)); sent.length = 0; t = 360; S.tick();
    ok(sent.length === 0, "session: the same frame and LEDs: nothing");
    S.clear();
    ok(cmds().filter((c) => c === 0x5A).length === 1 && cmds().filter((c) => c === 0x5C).length === 16 && sent.filter((b) => b[4] === 0x5A)[0].slice(7, -1).every((v) => v === 0),
       "session: clear() turns every LED off and blanks the screen");
    S.feed(X.statusMsg(3)); sent.length = 0; t = 1000; S.setFrame(f2); S.tick();
    ok(!S.active && sent.length === 0, "session: after 52 03 (REMOTE left) nothing goes");
    S.feed(X.statusMsg(2)); t = 1100; S.tick();
    ok(S.active && sent.filter((b) => b[4] === 0x5C).length === 16, "session: 52 02 again (MODE while in REMOTE mode answers 52 03, 52 02): everything sent again");
  }
  // ---- the views
  const acts = (a, kind) => a.filter((x) => x.kind === kind);
  const D = 1, U = 0, Q = 3;
  const down = (v, k, t = 0) => v.key(k, D, t), up = (v, k, t = 0) => v.key(k, U, t);
  const click = (v, k, t) => [...v.key(k, D, t), ...v.key(k, U, t + 40), ...v.key(k, Q, t + 40)];
  {
    const v = new V.OmxViews();
    ok(V.FM1_WHITES.length === 16 && V.FM1_SHARPS.length === 11 && V.FM1_SHARPS.map((k) => 53 + k).join() === "54,56,58,61,63,66,68,70,73,75,78",
       "views: the FM-1's 27 keys are 16 whites (F3..G5) and 11 sharps (F#3 .. F#5)");
    ok(v.view === V.VIEW.BASE && v.knob === E.SELECT && v.range === 0, "views: Base, SELECT selected, Range 1 at first");
    let a = down(v, 15);
    ok(a.length === 1 && a[0].kind === "key" && a[0].k === 0 && a[0].down, "Base R1: key 15 (F4) plays the FM-1's first F (key 0, F3)");
    a = up(v, 15);
    ok(a.length === 1 && a[0].k === 0 && !a[0].down, "Base: its release lets it go");
    ok(down(v, 3)[0].k === 1 && up(v, 3)[0].k === 1 && down(v, 26)[0].k === 19, "Base R1: black key 3 (F#4) plays F#3; key 26 (C6) plays C5");
    up(v, 26);
    down(v, 12); up(v, 12);
    ok(v.range === 1 && down(v, 15)[0].k === 12 && down(v, 23)[0].k === 26, "Base R2: key 15 plays the second F (key 12), key 23 (G5) the FM-1's top G5");
    up(v, 15); up(v, 23);
    ok(down(v, 24).length === 0 && down(v, 9).length === 0 && v.roleOf(26) === null, "Base R2: keys past the FM-1's (24, 9, 26) do nothing");
    up(v, 24); up(v, 9);
    a = down(v, 1);
    ok(a.length === 1 && a[0].kind === "btn" && a[0].b === B.OCTDN && a[0].down && up(v, 1)[0].down === false && down(v, 2)[0].b === B.OCTUP,
       "Base: keys 1 and 2 hold OCT- and OCT+");
    up(v, 2);
    ok(v.button === B.PLAY, "Base: OCT- and OCT+ pressed are not selected (AUX's button stays PLAY)");
    // the knob quick keys: held, KN2 with key 7, the whites still play
    down(v, 13);
    ok(v.overlay() === "knob" && v.screen().title === "KNOBS", "knob quick keys: key 13 held shows them (KNOBS)");
    a = down(v, 7);
    ok(a.length === 1 && a[0].kind === "select" && a[0].role === E.K2 && v.knob === E.K2, "knob quick keys: key 7 selects KN2");
    up(v, 7);
    ok(acts(v.enc(3), "turn")[0].role === E.K2 && v.enc(-2)[0].steps === -2, "the encoder turns the selected knob (KN2), its amt as detents");
    ok(down(v, 20)[0].kind === "key" && down(v, 10).length === 0, "knob quick keys: the whites still play; key 10 does nothing");
    up(v, 20); up(v, 10); up(v, 13);
    ok(v.overlay() === null, "knob quick keys: let go, they close");
    // double click latches, a click closes
    click(v, 13, 1000);
    ok(v.overlay() === null, "a single click on 13 does not latch");
    click(v, 13, 3000); click(v, 13, 3200);
    ok(v.overlay() === "knob" && v.qk.knob.latched, "a double click on 13 latches the knob quick keys");
    ok(down(v, 3)[0].role === E.SELECT, "latched: key 3 selects SEL");
    up(v, 3);
    click(v, 13, 5000);
    ok(v.overlay() === null, "latched: a single click closes them");
    // the button quick keys: PLAY pressed and selected, the whites mute, OCT selectable, AUX presses the selected
    down(v, 14);
    ok(v.overlay() === "btn" && v.screen().title === "BUTTONS", "button quick keys: key 14 held shows them (BUTTONS)");
    a = down(v, 25);
    ok(a.length === 1 && a[0].kind === "btn" && a[0].b === B.PLAY && a[0].down, "button quick keys: key 25 presses PLAY");
    a = up(v, 25);
    ok(a[0].b === B.PLAY && !a[0].down, "button quick keys: and lets it go");
    ok(down(v, 21)[0].b === B.HOME && v.button === B.HOME && down(v, 5)[0].b === B.FX && v.button === B.FX, "button quick keys: HOME (21), FX (5): pressed and selected");
    up(v, 21); up(v, 5);
    ok(down(v, 16).length === 0 && down(v, 3).length === 0, "button quick keys: the white keys (16) and keys 3, 4 do not play");
    up(v, 16); up(v, 3);
    down(v, 1); up(v, 1);
    ok(v.button === B.OCTDN, "button quick keys: OCT- is selectable here");
    up(v, 14);
    ok(down(v, 0)[0].b === B.OCTDN && up(v, 0)[0].down === false, "AUX presses the selected button (OCT-) while held");
    // ranges and knob selects in Seq: the 1:1 map, AUX toggled by K4
    v.pot(0, 40);
    ok(v.view === V.VIEW.SEQ && v.screen().title === "SEQ", "K1 at 40 (second zone): the Seq view");
    const seqKeys = [];
    for (let k = 1; k <= 26; k++) { const r = down(v, k); seqKeys.push(r[0].k); up(v, k); }
    ok(seqKeys.slice(0, 10).join() === V.FM1_SHARPS.slice(0, 10).join() && seqKeys.slice(10).join() === V.FM1_WHITES.join(),
       "Seq: blacks 1..10 are the first 10 FM-1 sharps, whites 11..26 its 16 whites, in order");
    ok(down(v, 0)[0].kind === "btn" && (up(v, 0), true), "Seq: AUX is the button quick key at first");
    v.pot(3, 100);
    ok(v.auxBlack && down(v, 0)[0].k === V.FM1_SHARPS[10] && /AUX F#5/.test(v.screen().line), "Seq: K4 up, AUX is the 11th sharp (F#5)");
    up(v, 0); v.pot(3, 10);
    ok(!v.auxBlack, "Seq: K4 down, the button quick key again");
    // Keyboard: notes, channel K3, octave K4
    v.pot(0, 70);
    ok(v.view === V.VIEW.KEYS, "K1 at 70: the Keyboard view");
    v.pot(2, 20); v.pot(3, 95);
    a = down(v, 12);
    ok(a[0].kind === "midi" && a[0].bytes.join() === [0x92, 60 + 24, 100].join() && v.screen().line.startsWith("CH 3  OCT +2"),
       "Keyboard: K3 at 20 is channel 3, K4 at 95 octave +2: key 12 (C4) sends note-on C6 at 100 on channel 3");
    v.pot(3, 64);
    ok(up(v, 12)[0].bytes.join() === [0x82, 84, 0].join(), "Keyboard: the note-off is the note that went down, after an octave change");
    ok(down(v, 0)[0].kind === "btn", "Keyboard: AUX is the button quick key"); up(v, 0);
    // Quick Keys: knob shortcuts select, button shortcuts select and press
    v.pot(0, 120);
    ok(v.view === V.VIEW.QUICK && v.screen().title === "QUICK KEYS", "K1 at 120: the Quick Keys view");
    ok(down(v, 9)[0].role === E.K4 && v.knob === E.K4 && (up(v, 9), true), "Quick Keys: key 9 selects KN4");
    a = down(v, 23);
    ok(a[0].kind === "btn" && a[0].b === B.PLAY && a[0].down && v.button === B.PLAY && !up(v, 23)[0].down, "Quick Keys: key 23 selects PLAY and presses it");
    ok(down(v, 11)[0].b === B.FX && down(v, 19)[0].b === B.HOME && down(v, 17).length === 0, "Quick Keys: 11 FX, 19 HOME, 17 nothing");
    const rel = v.releaseAll();
    ok(rel.length === 2 && rel.every((x) => !x.down) && v.held.size === 0, "views: releaseAll lets every held key and button go");
  }
  // K1 / K2 zones, with hysteresis
  {
    ok(V.zoneOf(0, 4, null) === 0 && V.zoneOf(31, 4, null) === 0 && V.zoneOf(32, 4, null) === 1 && V.zoneOf(127, 4, null) === 3, "zones: K1's four (0..31, 32..63, 64..95, 96..127)");
    ok(V.zoneOf(33, 4, 0) === 0 && V.zoneOf(35, 4, 0) === 1 && V.zoneOf(30, 4, 1) === 1 && V.zoneOf(28, 4, 1) === 0, "zones: a zone changes only 3 past its edge (hysteresis)");
    const v = new V.OmxViews();
    ok(v.pot(0, 33).length === 0 && v.pot(0, 36)[0].view === 1 && v.pot(0, 30).length === 0, "K1: no view flicker around an edge");
    const t = v.pot(1, 127, 0, 1000);
    ok(t[0].kind === "theme" && t[0].theme === 5 && v.screen(1500).line === "THEME NIGHT" && v.screen(3000).line !== "THEME NIGHT" && V.THEMES.length === 6,
       "K2: six themes; the top one (Night), its name on the screen a moment");
    ok(new V.OmxViews(v.saved()).theme === 5 && new V.OmxViews({ theme: 99, knob: 42 }).theme === 0, "views: the theme (and selections) kept in prefs; bad values ignored");
  }
  // Range 2: the playing keys past the FM-1's G5 (G#5 A#5 A5 B5 C6) lit dark red; Range 1 has none
  {
    const v = new V.OmxViews(), red = (l, k) => l[k][0] > 0 && l[k][1] === 0 && l[k][2] === 0;
    const dead = (l) => [...Array(27).keys()].filter((k) => red(l, k) && k !== 0);
    ok(dead(v.leds()).length === 0, "Range 1: no dead keys");
    v.range = 1;
    const l = v.leds();
    ok(dead(l).join() === "9,10,24,25,26" && l[24][0] < 64, "Range 2: G#5 A#5 A5 B5 C6 dark red, dimmer than a shortcut");
    v.key(14, 1, 0);
    ok(!red(v.leds(), 24), "Range 2: the button quick keys cover the dead keys (no red under HOME..REC)");
  }
  // the LEDs: shortcuts at 50% and full when pressed or selected; mirrored keys dark unless the FM-1 lights them
  {
    const v = new V.OmxViews(), lv = new Uint8Array(27);
    const L = () => v.leds((k) => lv[k]);
    let l = L();
    ok(l[0].join() === "64,0,0" && l[1].join() === "0,64,64" && l[11].join() === "64,127,0" && l[12].join() === "64,32,0",
       "LEDs: AUX, OCT- at 50%; Range 1 selected (full lime), Range 2 at 50%");
    ok(l[15].join() === "0,0,0" && l[3].join() === "0,0,0", "LEDs: keys mirroring the FM-1's are dark while its LEDs are");
    lv[0] = 255; lv[1] = 40;
    l = L();
    ok(l[15].join() === "127,127,127" && l[3].every((c) => c > 0 && c < 60) && l[3][0] === l[3][2], "LEDs: lit on the FM-1, the mirror lights in the theme's hue (White), its level setting the brightness");
    v.theme = 1; l = L();
    ok(l[15].join() === V.THEMES[1].key.map((c) => Math.min(127, Math.round(c / 2))).join(), "LEDs: another theme, another hue (Ember)");
    v.theme = 0;
    v.key(0, 1); l = L();
    ok(l[0].join() === "127,0,0", "LEDs: AUX full while pressed"); v.key(0, 0);
    v.key(13, 1); l = L();
    ok(l[13].join() === "127,0,64" && l[3].join() === "0,0,127" && l[4].join() === "0,0,64" && l[6].join() === "64,0,0" && l[15].join() === "127,127,127",
       "LEDs: knob quick keys: the toggle full, the selected knob (SEL) full, the others at 50%; F still mirrors");
    v.key(13, 0); v.key(14, 1); l = L();
    ok(l[25].join() === "0,127,64" && l[21].join() === "64,32,0" && l[15].join() === "0,0,0" && l[3].join() === "0,0,0",
       "LEDs: button quick keys: PLAY (selected) full, HOME at 50%; the muted whites dark");
    v.key(14, 0);
    v.pot(0, 70); v.key(12, 1); l = L();
    ok(l[12].join() === "127,127,127" && l[13].join() === "0,0,0", "LEDs: Keyboard: a held key lights in the theme's hue, the rest dark");
    v.key(12, 0);
    v.pot(0, 40); lv.fill(0); lv[V.FM1_WHITES[3]] = 255; lv[V.FM1_SHARPS[0]] = 96; l = L();
    ok(l[14].join() === "127,127,127" && l[1].join() === "79,79,79" && l.filter((c) => c.some((x) => x)).length === 3,
       "LEDs: Seq mirrors each FM-1 key's LED (and AUX the button quick key)");
  }
  // K5: MASTER with pickup (midi-map.js MasterPot, as a learned MASTER pot)
  {
    const v = new V.OmxViews();
    let a = v.pot(4, 10, 10 << 7, 0, 768);
    ok(a.length === 1 && a[0].kind === "pickup" && a[0].first && a[0].at === 768 && /^K5 \d+% TO 75%$/.test(v.screen(10).line), "K5 far from MASTER: a pickup, not a change (the screen says where)");
    a = v.pot(4, 50, 50 << 7, 0, 768);
    ok(a[0].kind === "pickup" && !a[0].first, "K5: still away, still waiting");
    a = v.pot(4, 100, 100 << 7, 0, 768);
    ok(a[0].kind === "master" && a[0].value === Math.round(12800 * 1023 / 16383), "K5 crossing MASTER picks it up");
    a = v.pot(4, 64, 64 << 7, 0, a[0].value);
    ok(a[0].kind === "master" && Math.abs(a[0].value - 512) <= 1 && v.screen(10).line === "MASTER 50%", "K5 then follows (MASTER 50%)");
    a = v.pot(4, 66, 66 << 7, 0, 300);
    ok(a[0].kind === "pickup", "K5: MASTER moved by something else: pickup again");
    const mp = new MM.MasterPot();
    ok(mp.feed(767, 1023, 768)[0].kind === "master", "MasterPot: a pot already at MASTER takes it at once");
  }
}

console.log(fails ? `site: ${fails} of ${checks} FAILED` : `site tests passed (${checks})`);
process.exit(fails ? 1 : 0);
