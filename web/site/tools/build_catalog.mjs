// SPDX-License-Identifier: GPL-3.0-only
// Build web/site/catalog.json (web/engine/API.md section 5) from:
//   - the FM-1 Firmware Hub's catalog (a snapshot: data/hub-catalog.snapshot.json, from
//     https://thefirmwarehub.com/catalog.json; --hub <file|url> to use another);
//   - FM1VST's cores (data/cores.json: the 29, their pinned versions and licences, the twenty-one un-gated ones built
//     for the web, the licensing gate of FM1VST CORES-PLAN.md section 6); an un-gated core the Hub does not list
//     (LOOM-8, Speak37's FM-1 build) is a native-only entry of its own (offHub, as live.js mergeLive makes one that
//     left the Hub);
//   - RemoteFM1 itself (web/installer/releases.json, its first release): the one package this site hosts, fetched from
//     the installer's firmware/ folder on the same site, not from the Hub. It plays nothing; it waits for a host;
//   - the firmwares this site hosts as .rfm1 files (data/site-rfm1.json, web/site/fw/: FIZZ, built by tools/rfm1/), on
//     the rfm1 engine: each file's SHA-256 checked and its manifest read (the id, name, version, licence, source, glue);
//   - firmware authors' own native builds (--native-ext: web/site/tools/native_sync.mjs's index, the builds it checked
//     and published under engines/native/ext/): where FM1VST also builds the firmware, the newer version runs, the
//     author's at the same version (live.js preferNative); the other one is named in the entry (nativeAlt, nativeWhy).
// Every Hub firmware gets the emulated engine (its package fetched at play time from where the Hub points); the
// twenty-one un-gated cores also get the native one, which is then their default while it is the Hub's version
// (newest wins: web/site/js/live.js, which builds the entries; the page merges the Hub's live catalog over this one at
// load).
//   node web/site/tools/build_catalog.mjs [--layout dev|pages] [--hub FILE|URL] [--out FILE] [--native-ext FILE]
//                                         [--native-dir DIR]
// --native-dir: the folder the native cores' .wasm are published in (the Pages build: _site/engines/native). Only the
// cores whose <core>.wasm is there are listed as native (a warning names each one that is not): a Hub firmware whose
// core is missing runs emulated only (or on its author's native build), a native-only entry (LOOM-8, Speak37) is left
// out. Without it (the dev layout, the Pages check job) every un-gated core is listed, built or not.
// layout: where the engines, the remote module and the installer are, seen from the page:
//   dev    the page at web/site/ served from web/ (python3 -m http.server in web/): ../engines/.., ../build/..
//   pages  the GitHub Pages artifact (.github/workflows/pages.yml): engines/, installer/, remote/ next to the page
import { readFileSync, writeFileSync, readdirSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { dirname, join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { siteEntry, CATEGORIES, HUB, authorNative, preferNative, rfm1Entry } from "../js/live.js";
import { inspect } from "../../engines/rfm1/engine.js";
import { nativeAvailability, ungatedCores, annotate } from "./site_guard.mjs";

const HERE = dirname(fileURLToPath(import.meta.url)), SITE = join(HERE, "..");
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const layout = arg("--layout", "dev"), out = arg("--out", join(SITE, "catalog.json"));
const hubSrc = arg("--hub", join(SITE, "data", "hub-catalog.snapshot.json"));

const PATHS = {
  dev: {
    engines: {
      native: { module: "../engines/native/engine.js", wasm: "../build/native/{core}.wasm", ext: "../build/native-ext/ext/" },
      emu: { module: "../engines/emu/engine.js", wasm: "../build/emu/fm1_web.wasm", hook: "../engines/emu/catalog.mjs" },
      rfm1: { module: "../engines/rfm1/engine.js" },          // (the visitor's .rfm1 files: no files of its own)
      mock: { module: "mock/engine.js" },
    },
    remote: "../remote/remote.js",
    installer: "../installer/",
    managers: "../build/managers/",
  },
  pages: {
    engines: {
      native: { module: "engines/native/engine.js", wasm: "engines/native/{core}.wasm", ext: "engines/native/ext/" },
      emu: { module: "engines/emu/engine.js", wasm: "engines/emu/fm1_web.wasm", hook: "engines/emu/catalog.mjs" },
      rfm1: { module: "engines/rfm1/engine.js" },
      mock: { module: "mock/engine.js" },
    },
    remote: "remote/remote.js",
    installer: "installer/",
    managers: "managers/",
  },
};
if (!PATHS[layout]) { console.error(`build_catalog: --layout dev or pages, not ${layout}`); process.exit(2); }

const hubLive = /^https?:/.test(hubSrc);
const hub = hubLive ? await (await fetch(hubSrc, { signal: AbortSignal.timeout(30000) })).json() : JSON.parse(readFileSync(hubSrc, "utf8"));
if (!Array.isArray(hub.firmwares) || !hub.firmwares.length) { console.error(`build_catalog: ${hubSrc} has no firmwares`); process.exit(1); }
const cores = JSON.parse(readFileSync(join(SITE, "data", "cores.json"), "utf8")).cores;
// the native versions and the source each core builds come from the FM1VST the site builds (web/vendor/FM1VST): each
// fm1_add_core(<id> ... SOURCE .../cores/<dir> ... VERSION x) in core-glue/<glue>/, the submodule commit FM1VST pins
// for cores/<dir> (git ls-tree), its URL (.gitmodules). So a submodule bump moves them; data/cores.json's versions,
// commits, repos and glue folders are only the fallback when that tree is not checked out (the Pages "check" job), so
// keep them in step (this script says where they differ).
const FM1VST = join(SITE, "..", "vendor", "FM1VST");
let fm1vstCommit = JSON.parse(readFileSync(join(SITE, "data", "cores.json"), "utf8")).fm1vst || null;
{
  const glueRoot = join(FM1VST, "core-glue");
  const found = new Map();
  const walk = (dir) => {
    let names = [];
    try { names = readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
    for (const d of names) {
      const p = join(dir, d.name);
      if (d.isDirectory()) walk(p);
      else if (d.name.endsWith(".cmake")) {
        const text = readFileSync(p, "utf8"), vars = {};
        for (const m of text.matchAll(/^\s*set\((\w+)\s+([^\s)]+)/gm)) vars[m[1]] = m[2];
        for (const call of text.split("fm1_add_core(").slice(1)) {
          // (the call's arguments up to its own closing parenthesis: parentheses inside quoted arguments, a CREDITS
          // text or a VERSION_REGEX group, do not end it)
          let end = call.length;
          for (let i = 0, depth = 0, q = false; i < call.length; i++) {
            const ch = call[i];
            if (q) { if (ch === "\\") i++; else if (ch === '"') q = false; }
            else if (ch === '"') q = true;
            else if (ch === "(") depth++;
            else if (ch === ")" && depth-- === 0) { end = i + 1; break; }
          }
          const body = call.slice(0, end);
          const id = /^\s*(\w+)/.exec(body), ver = /\bVERSION\s+"?([^\s")]+)"?/.exec(body), src = /\bSOURCE\s+(\S+)/.exec(body);
          if (!id) continue;
          const path = src ? src[1].replace(/\$\{(\w+)\}/g, (m, v) => vars[v] ?? m) : "";
          const dirM = /\/cores\/([^/\s)]+)/.exec(path);
          // (VERSION_FILE <file> VERSION_REGEX "<re>" [VERSION_GROUP n]: the group of the file's first matching line, as
          // cmake/FM1Cores.cmake reads it; the file is in the core's tree, a submodule of FM1VST: checked out recursively)
          let fver = null;
          const vf = /\bVERSION_FILE\s+(\S+)/.exec(body), vr = /\bVERSION_REGEX\s+"((?:[^"\\]|\\.)*)"/.exec(body);
          if (!ver && vf && vr && dirM) {
            const group = Number((/\bVERSION_GROUP\s+(\d+)/.exec(body) || [0, 1])[1]);
            try {
              const re = new RegExp(vr[1].replace(/\\"/g, '"'));
              for (const line of readFileSync(join(FM1VST, "cores", dirM[1], vf[1]), "utf8").split("\n")) {
                const m = re.exec(line);
                if (m) { fver = m[group] ?? null; break; }
              }
            } catch (_) { /* (the tree not checked out: data/cores.json's version) */ }
          }
          // (a VERSION given through a variable, set(_x 0.6-beta) ... VERSION ${_x}: the variable's value)
          const sub = (t) => t.replace(/\$\{(\w+)\}/g, (m, v) => vars[v] ?? m);
          found.set(id[1], { version: ver ? sub(ver[1]) : fver, dir: dirM ? dirM[1] : null, glue: relative(glueRoot, dirname(p)).split(sep).join("/") });
        }
      }
    }
  };
  walk(glueRoot);
  const git = (...a) => { try { return execFileSync("git", ["-C", FM1VST, ...a], { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim(); } catch (_) { return ""; } };
  const urls = new Map();
  try {
    let cur = null;
    for (const line of readFileSync(join(FM1VST, ".gitmodules"), "utf8").split("\n")) {
      const pm = /^\s*path\s*=\s*(\S+)/.exec(line), um = /^\s*url\s*=\s*(\S+)/.exec(line);
      if (/^\s*\[submodule/.test(line)) cur = {};
      if (pm && cur) { cur.path = pm[1]; if (cur.url) urls.set(cur.path, cur.url); }
      if (um && cur) { cur.url = um[1].replace(/\.git$/, "").replace(/\/$/, ""); if (cur.path) urls.set(cur.path, cur.url); }
    }
  } catch (_) { /* (no checkout) */ }
  const head = found.size ? git("rev-parse", "HEAD") : "";
  if (head && head !== fm1vstCommit) { console.log(`build_catalog: FM1VST ${head} (web/vendor), was ${fm1vstCommit} in data/cores.json`); fm1vstCommit = head; }
  const was = (c, k, v) => { if (v && v !== c[k]) { console.log(`build_catalog: ${c.core} ${k} ${v} (FM1VST), was ${c[k] ?? "unset"} in data/cores.json`); c[k] = v; } };
  for (const c of cores) {
    const f = found.get(c.core);
    if (!f) continue;
    was(c, "version", f.version);
    was(c, "glue", f.glue);
    if (f.dir) {
      const m = /^160000 commit ([0-9a-f]{40})\t/.exec(git("ls-tree", "HEAD", `cores/${f.dir}`));
      was(c, "commit", m && m[1]);
      was(c, "repo", urls.get(`cores/${f.dir}`));
    }
  }
  if (!found.size) console.log("build_catalog: no FM1VST checkout under web/vendor: native versions and commits from data/cores.json");
}
const coreByHub = new Map(cores.map((c) => [c.hub || c.core, c]));

// the native cores the site has (--native-dir): a core whose .wasm is not there is not listed as native
const nativeDir = arg("--native-dir", null);
const nativeMissing = new Set(nativeDir ? nativeAvailability(ungatedCores(cores), (id) => existsSync(join(nativeDir, `${id}.wasm`))).missing : []);
if (nativeMissing.size) annotate({ level: "warning", title: `native cores not published: ${[...nativeMissing].join(" ")}`,
  message: `build_catalog: no .wasm in ${nativeDir} for ${[...nativeMissing].join(", ")}: not listed as native (their firmwares run emulated; a native-only entry is left out)` });

// the authors' own native builds (native_sync.mjs's index): by the site's id
const extFile = arg("--native-ext", null);
const ext = extFile ? JSON.parse(readFileSync(extFile, "utf8")) : null;
const extById = new Map((ext ? ext.accepted : []).map((b) => [b.id, b]));
// the native build an entry gets, and the other one; withNative(entry, pick) records the choice on the entry
const pickNative = (id, core) => preferNative(nativeOf(core), extById.has(id) ? authorNative(extById.get(id), PATHS[layout].engines.native.ext) : null);
const withNative = (e, pick) => {
  if (pick.alt) { e.nativeAlt = pick.alt; e.nativeWhy = pick.why; }
  if (e.native && e.native.by === "author") { delete e.nativeGated; if (!e.license) e.license = e.native.license; }
  return e;
};

// Hub firmwares known not to run in fm1-emu: W2's web/engines/emu/catalog.mjs EMU_SKIP when it is there
let EMU_SKIP = { bubba: "boots to a black screen, no sound (needs M-VAVE's own loader)",
  fizz: "shows FIZZ NEEDS ITS DATA, silent: its data blob is written over USB after the install, which the emulator can't do (this site runs its .rfm1)" };
try { EMU_SKIP = (await import(new URL("../../engines/emu/catalog.mjs", import.meta.url).href)).EMU_SKIP || EMU_SKIP; } catch (_) { /* (W2's branch not merged) */ }

// the native core's own source: the commit FM1VST pins (its submodule), and FM1VST's glue folder for it
// (nativeName: the name the native build goes by, where it differs from the Hub's; hubVersion: the Hub's name for the
// version it builds; nativeDefault: why it stays the default engine though the Hub's version is newer: live.js newestWins)
const nativeOf = (core) => core && !core.gated && !nativeMissing.has(core.core) ? { core: core.core, version: core.version, wasm: PATHS[layout].engines.native.wasm.replace("{core}", core.core),
  repo: core.repo || null, commit: core.commit || null, glue: core.glue || null,
  ...(core.nativeName ? { name: core.nativeName } : {}), ...(core.hubVersion ? { hubVersion: core.hubVersion } : {}),
  ...(core.nativeDefault ? { preferred: true, preferredWhy: core.nativeDefault } : {}) } : null;
const firmwares = hub.firmwares.map((f) => {
  const core = coreByHub.get(f.id) || null, pick = pickNative(f.id, core);
  return withNative(siteEntry(f, { core, native: pick.native, emuSkip: EMU_SKIP }), pick);   // (newest wins: emu is the default where the core is behind)
});
for (const id of extById.keys()) if (!hub.firmwares.some((f) => f.id === id) && !cores.some((c) => (c.hub || c.core) === id))
  console.log(`build_catalog: ${id}: an author's native build of a firmware neither the Hub nor FM1VST lists: left out`);
// an un-gated core the Hub does not list: native only, its entry from data/cores.json (name, what, release, authorFull)
const onHub = new Set(hub.firmwares.map((f) => f.id));
for (const core of cores) {
  const id = core.hub || core.core;
  if (core.gated || onHub.has(id)) continue;
  if (!core.name || !core.what) { console.error(`build_catalog: ${core.core} is not on the Hub: data/cores.json needs its name and what`); process.exit(1); }
  const gh = /^https:\/\/github\.com\/([^/]+\/[^/]+?)\/?$/.exec(core.repo || "");
  const pick = pickNative(id, core);
  if (!pick.native) { console.log(`build_catalog: ${core.core} (not on the Hub, native only) has no native build here: left out`); continue; }
  const e = withNative(siteEntry({ id, name: core.name, version: core.version, author: core.authorFull || core.author, what: core.what,
    source: core.repo || null, repo: gh ? gh[1] : null, release: core.release || null, beta: /\b(alpha|beta|rc)\b/i.test(core.version) },
    { core, native: pick.native, emuSkip: EMU_SKIP }), pick);
  firmwares.push({ ...e, engines: ["native"], defaultEngine: "native", nativeBehind: false, emu: null, offHub: true });
}

// RemoteFM1 as a firmware to play: its released package, from this site's installer (the same origin as the page)
const REMOTE_ID = "remotefm1";
function remoteEntry() {
  const rel = JSON.parse(readFileSync(join(SITE, "..", "installer", "releases.json"), "utf8"));
  const r = rel.releases[0], file = `firmware/remote-${r.version}.fwsc`;
  const pkg = readFileSync(join(SITE, "..", "installer", file));
  const sha = createHash("sha256").update(pkg).digest("hex");
  if (sha !== r.sha256) { console.error(`build_catalog: installer/${file} has SHA-256 ${sha}, releases.json says ${r.sha256}`); process.exit(1); }
  return {
    id: REMOTE_ID,
    name: "RemoteFM1",
    version: r.version,
    author: "Quixotic7",
    authorFull: "Quixotic7",
    category: "Other",
    categories: ["Other"],
    tag: "DX7 + MIDI controller",
    what: "The firmware this site installs, running as itself. On its own (0.3) the FM-1 is a DX7 with Yamaha's ROM voices, a MIDI controller, a looper and an arp; when a computer connects it becomes that computer's panel.",
    license: r.license || "GPL-3.0-only",     // (the firmware: MPL-2.0 from 0.2, GPL-3.0-only before)
    source: rel.repo,
    repo: "Quixotic7/RemoteFM1",
    release: r.tag ? `${rel.repo}/releases/tag/${r.tag}` : null,
    site: null,
    play: null,
    beta: false,
    engines: ["emu"],
    defaultEngine: "emu",
    native: null,
    emu: { pkg: PATHS[layout].installer + file, sha256: r.sha256, size: pkg.length, product: null, from: "site" },
    badge: "meta",
    blurb: "A DX7, a controller, a looper. Or a host's panel.",
  };
}
firmwares.splice(0, firmwares.length, ...firmwares.filter((f) => f.id !== REMOTE_ID), remoteEntry());

// the site's own .rfm1 files (the same path from the page in both layouts: fw/ is in web/site). One the Hub lists too
// keeps the Hub's package as its emulated engine; the .rfm1 is its default
for (const d of JSON.parse(readFileSync(join(SITE, "data", "site-rfm1.json"), "utf8")).firmwares) {
  const bytes = readFileSync(join(SITE, d.file));
  const sha = createHash("sha256").update(bytes).digest("hex");
  if (sha !== d.sha256) { console.error(`build_catalog: ${d.file} has SHA-256 ${sha}, data/site-rfm1.json says ${d.sha256}`); process.exit(1); }
  let r;
  try { r = rfm1Entry(d, inspect(new Uint8Array(bytes)).manifest, { sha256: sha, size: bytes.length }); }
  catch (e) { console.error(`build_catalog: ${d.file}: ${e.message || e}`); process.exit(1); }
  const i = firmwares.findIndex((f) => f.id === r.id);
  if (i < 0) firmwares.push(r);
  else {
    const h = firmwares[i];
    // (a package the emulator can't run, FIZZ's: the .rfm1 alone)
    console.log(`build_catalog: ${r.id} is on the Hub too: its .rfm1 ${r.version} is the default${EMU_SKIP[r.id] ? "; the Hub's package does not run emulated" : `, the Hub's ${h.version} runs emulated`}`);
    firmwares[i] = EMU_SKIP[r.id] ? r
      : { ...h, engines: ["rfm1", ...h.engines.filter((k) => k !== "rfm1")], defaultEngine: "rfm1", rfm1: r.rfm1, license: h.license || r.license };
  }
}
firmwares.sort((a, b) => a.name.localeCompare(b.name, "en", { sensitivity: "base" }));

const catalog = {
  about: "Built by web/site/tools/build_catalog.mjs from the FM-1 Firmware Hub's catalog, FM1VST's cores, RemoteFM1's own release (web/installer/releases.json) and the site's .rfm1 files (data/site-rfm1.json). Edit those, not this file.",
  generated: new Date().toISOString().slice(0, 10),
  hubUpdated: hub.updated || null,
  hubSource: hubLive ? "live" : "snapshot",
  hub: HUB,
  layout,
  paths: PATHS[layout],
  categories: CATEGORIES,
  credits: {
    fm1emu: { name: "fm1-emu", author: "Joakim Eriksson", license: "MIT", source: "https://github.com/joakimeriksson/fm1-emu",
              line: "Emulated by fm1-emu, the FM-1 emulator by Joakim Eriksson (MIT licence): https://github.com/joakimeriksson/fm1-emu" },
    fm1vst: { name: "FM1VST", license: "GPL-3.0-only", source: "https://github.com/Quixotic7/FM1VST", commit: fm1vstCommit },
    hub: { name: "The FM-1 Firmware Hub", source: HUB },
  },
  // the authors' own native builds: the workflow that signs them, and what the last sync accepted and turned down
  nativeAuthors: ext ? { workflow: ext.workflow, trustedRefs: ext.trustedRefs, docs: "native/",
    accepted: ext.accepted.map((b) => `${b.id} ${b.version}`), rejected: ext.rejected.map((r) => ({ repo: r.repo, tag: r.tag, why: r.why })) } : null,
  firmwares,
};
writeFileSync(out, JSON.stringify(catalog, null, 1) + "\n");
const nNative = firmwares.filter((f) => f.native).length;
const nOff = firmwares.filter((f) => f.offHub).length;
const nAuthor = firmwares.filter((f) => f.native && f.native.by === "author").length;
// FM1VST's own native cores: running, or named as the other build where the author's runs
const nFm1vst = firmwares.filter((f) => (f.native && f.native.by !== "author") || (f.nativeAlt && f.nativeAlt.by === "fm1vst")).length;
const nRfm1 = firmwares.filter((f) => f.rfm1).length;
console.log(`catalog: ${firmwares.length} firmwares (${nNative - nOff} native + emulated, ${nOff} native only, ${firmwares.length - nNative - nRfm1} emulated, RemoteFM1 among them; ${nAuthor} native built by the author; ${nRfm1} .rfm1 from this site), layout ${layout} -> ${out}`);
// (the 21 un-gated cores, less those --native-dir does not have)
const nUngated = ungatedCores(cores).length;
if (nUngated !== 21) { console.error(`build_catalog: data/cores.json has ${nUngated} un-gated cores, expected 21`); process.exit(1); }
if (nFm1vst !== nUngated - nativeMissing.size) { console.error(`build_catalog: expected ${nUngated - nativeMissing.size} native cores (${nUngated} un-gated, ${nativeMissing.size} missing), found ${nFm1vst}`); process.exit(1); }
if (nativeMissing.size) console.log(`build_catalog: ${nativeMissing.size} native cores left out (no .wasm in ${nativeDir}): ${[...nativeMissing].join(", ")}`);
