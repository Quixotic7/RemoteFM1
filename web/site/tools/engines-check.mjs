// SPDX-License-Identifier: GPL-3.0-only
// Are the engines the page loads actually there? A checkout's web/ served as it is (python3 -m http.server -d web)
// has the page and remote.js, but the engines are build outputs (web/build/native/<core>.wasm, web/build/emu/
// fm1_web.wasm), made by web/engines/native/build.sh and web/engines/emu/build.sh, or by the Pages workflow for the
// site. Without them every firmware falls back to the mock engine: its "TEST SCREEN" on the page and on a connected
// FM-1, a sine for a sound (2026-11-08: a fresh worktree served on :8787 looked like a broken page).
//   node web/site/tools/engines-check.mjs [webDir]          exit 1 and the list when something the catalog names is missing
//   node web/site/tools/engines-check.mjs --url http://127.0.0.1:8787/     the same over HTTP, against a running server
// web/serve.sh runs it before serving.
import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

// what the catalog makes the page fetch: { path (relative to web/), why }[]; catalog: site/catalog.json's object
export function enginePaths(catalog) {
  const out = [], seen = new Set();
  const add = (rel, why) => {
    const p = rel.replace(/^\.\.\//, "");            // (the catalog's paths are relative to web/site/)
    if (!seen.has(p)) { seen.add(p); out.push({ path: p, why }); }
  };
  const fws = Array.isArray(catalog.firmwares) ? catalog.firmwares : [];
  for (const f of fws) {
    if (f && f.native && f.native.wasm && Array.isArray(f.engines) && f.engines.includes("native"))
      add(f.native.wasm, `${f.id}'s native core`);
  }
  const emu = ((catalog.paths && catalog.paths.engines) || {}).emu;
  if (emu && emu.wasm && fws.some((f) => f && Array.isArray(f.engines) && f.engines.includes("emu")))
    add(emu.wasm, "the emulator (every .fwsc firmware)");
  if (catalog.paths && catalog.paths.remote)            /* (remote.js is a source; its module is built: web/remote/build.sh) */
    add("build/remote/remotefm1.mjs", "Use my FM-1 (libremotefm1 in WebAssembly)");
  return out;
}

// the missing ones: exists(path) -> bool | Promise<bool>
export async function missingEngines(catalog, exists) {
  const miss = [];
  for (const e of enginePaths(catalog)) if (!(await exists(e.path))) miss.push(e);
  return miss;
}

async function main(argv) {
  const ui = argv.indexOf("--url");
  let catalog, exists, where;
  if (ui >= 0) {
    const base = new URL(argv[ui + 1].replace(/\/?$/, "/"));
    where = base.href;
    catalog = await (await fetch(new URL("site/catalog.json", base))).json();
    exists = async (p) => { try { const r = await fetch(new URL(p, base), { method: "HEAD" }); return r.ok; } catch (_) { return false; } };
  } else {
    const web = resolve(argv.find((a) => !a.startsWith("--")) || join(fileURLToPath(new URL(".", import.meta.url)), "../.."));
    where = web;
    catalog = JSON.parse(readFileSync(join(web, "site/catalog.json"), "utf8"));
    exists = (p) => existsSync(join(web, p));
  }
  const all = enginePaths(catalog), miss = await missingEngines(catalog, exists);
  if (!miss.length) { console.log(`engines: all ${all.length} there (${where})`); return 0; }
  console.log(`engines: ${miss.length} of ${all.length} missing in ${where}: the page plays its mock engine ("TEST SCREEN") instead`);
  for (const m of miss.slice(0, 12)) console.log(`  ${m.path}  (${m.why})`);
  if (miss.length > 12) console.log(`  ... and ${miss.length - 12} more`);
  console.log("build them: web/engines/native/build.sh (FM1VST_DIR=web/vendor/FM1VST), web/engines/emu/build.sh, web/remote/build.sh");
  return 1;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
