// SPDX-License-Identifier: GPL-3.0-only
// Build web/site/catalog.json (web/engine/API.md section 5) from:
//   - the FM-1 Firmware Hub's catalog (a snapshot: data/hub-catalog.snapshot.json, from
//     https://thefirmwarehub.com/catalog.json; --hub <file|url> to use another);
//   - FM1VST's cores (data/cores.json: the 21, their pinned versions and licences, the ten un-gated ones built for the
//     web, the licensing gate of FM1VST CORES-PLAN.md section 6);
//   - RemoteFM1 itself (web/installer/releases.json, its first release): the one package this site hosts, fetched from
//     the installer's firmware/ folder on the same site, not from the Hub. It plays nothing; it waits for a host.
// Every Hub firmware gets the emulated engine (its package fetched at play time from where the Hub points); the ten
// un-gated cores also get the native one, which is then their default while it is the Hub's version (newest wins:
// web/site/js/live.js, which builds the entries; the page merges the Hub's live catalog over this one at load).
//   node web/site/tools/build_catalog.mjs [--layout dev|pages] [--hub FILE|URL] [--out FILE]
// layout: where the engines, the remote module and the installer are, seen from the page:
//   dev    the page at web/site/ served from web/ (python3 -m http.server in web/): ../engines/.., ../build/..
//   pages  the GitHub Pages artifact (.github/workflows/pages.yml): engines/, installer/, remote/ next to the page
import { readFileSync, writeFileSync, readdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { dirname, join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { siteEntry, CATEGORIES, HUB } from "../js/live.js";

const HERE = dirname(fileURLToPath(import.meta.url)), SITE = join(HERE, "..");
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const layout = arg("--layout", "dev"), out = arg("--out", join(SITE, "catalog.json"));
const hubSrc = arg("--hub", join(SITE, "data", "hub-catalog.snapshot.json"));

const PATHS = {
  dev: {
    engines: {
      native: { module: "../engines/native/engine.js", wasm: "../build/native/{core}.wasm" },
      emu: { module: "../engines/emu/engine.js", wasm: "../build/emu/fm1_web.wasm", hook: "../engines/emu/catalog.mjs" },
      mock: { module: "mock/engine.js" },
    },
    remote: "../remote/remote.js",
    installer: "../installer/",
  },
  pages: {
    engines: {
      native: { module: "engines/native/engine.js", wasm: "engines/native/{core}.wasm" },
      emu: { module: "engines/emu/engine.js", wasm: "engines/emu/fm1_web.wasm", hook: "engines/emu/catalog.mjs" },
      mock: { module: "mock/engine.js" },
    },
    remote: "remote/remote.js",
    installer: "installer/",
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
          const body = call.slice(0, call.indexOf(")") + 1 || call.length);
          const id = /^\s*(\w+)/.exec(body), ver = /\bVERSION\s+"?([^\s")]+)"?/.exec(body), src = /\bSOURCE\s+(\S+)/.exec(body);
          if (!id) continue;
          const path = src ? src[1].replace(/\$\{(\w+)\}/g, (m, v) => vars[v] ?? m) : "";
          const dirM = /\/cores\/([^/\s)]+)/.exec(path);
          found.set(id[1], { version: ver ? ver[1] : null, dir: dirM ? dirM[1] : null, glue: relative(glueRoot, dirname(p)).split(sep).join("/") });
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

// Hub firmwares known not to run in fm1-emu: W2's web/engines/emu/catalog.mjs EMU_SKIP when it is there
let EMU_SKIP = { bubba: "boots to a black screen, no sound (needs M-VAVE's own loader)" };
try { EMU_SKIP = (await import(new URL("../../engines/emu/catalog.mjs", import.meta.url).href)).EMU_SKIP || EMU_SKIP; } catch (_) { /* (W2's branch not merged) */ }

const firmwares = hub.firmwares.map((f) => {
  const core = coreByHub.get(f.id) || null;
  // the native core's own source: the commit FM1VST pins (its submodule), and FM1VST's glue folder for it
  const native = core && !core.gated ? { core: core.core, version: core.version, wasm: PATHS[layout].engines.native.wasm.replace("{core}", core.core),
    repo: core.repo || null, commit: core.commit || null, glue: core.glue || null } : null;
  return siteEntry(f, { core, native, emuSkip: EMU_SKIP });   // (newest wins: emu is the default where the core is behind)
});

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
    tag: "Waits for a host",
    what: "The firmware this site installs, running as itself. It turns an FM-1 into a USB panel for a computer, so it makes no sound of its own: it shows a screen and waits for a host. In a browser tab, no host is coming.",
    license: "GPL-3.0-only",
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
    blurb: "Plays nothing. Waits for a host.",
  };
}
firmwares.splice(0, firmwares.length, ...firmwares.filter((f) => f.id !== REMOTE_ID), remoteEntry());
firmwares.sort((a, b) => a.name.localeCompare(b.name, "en", { sensitivity: "base" }));

const catalog = {
  about: "Built by web/site/tools/build_catalog.mjs from the FM-1 Firmware Hub's catalog, FM1VST's cores and RemoteFM1's own release (web/installer/releases.json). Edit those, not this file.",
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
  firmwares,
};
writeFileSync(out, JSON.stringify(catalog, null, 1) + "\n");
const nNative = firmwares.filter((f) => f.native).length;
console.log(`catalog: ${firmwares.length} firmwares (${nNative} native + emulated, ${firmwares.length - nNative} emulated, RemoteFM1 among them), layout ${layout} -> ${out}`);
if (nNative !== 10) { console.error(`build_catalog: expected the 10 un-gated native cores, found ${nNative}`); process.exit(1); }
