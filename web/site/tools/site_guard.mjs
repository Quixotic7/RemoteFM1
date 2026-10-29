// SPDX-License-Identifier: GPL-3.0-only
// What may be deployed (.github/workflows/pages.yml, the build job): the site's catalog may name only what the site
// has, and a site that lost most of its native cores is not deployed at all (the deployed one stays).
//
//   node web/site/tools/site_guard.mjs check SITE [--cores FILE]
//       the deploy guard on an assembled site (_site). Fails (exit 1) when:
//         - a catalog entry's native engine has no .wasm in SITE, or the native engine.js is missing;
//         - more than a third of FM1VST's un-gated cores (data/cores.json, or --cores) have no .wasm in SITE;
//         - the .rfm1 engine is missing, or engines/common/shared.js (the native and .rfm1 engines import it);
//         - the catalog has emulated entries and the emulated engine (engine.js, fm1_web.wasm, catalog.mjs) is missing,
//           or a package the site itself hosts (RemoteFM1's) is missing;
//         - the hosted managers' index.json names a manager whose page is missing, or the Web MIDI shim is missing;
//         - remote/remote.js is published without the module it loads (build/remote/remotefm1.mjs);
//         - the installer's page is missing.
//       A native core missing below the threshold, or no managers at all, is a warning.
//   node web/site/tools/site_guard.mjs native-result FILE
//       web/engines/native/build.sh's result.json as annotations: one warning per core that failed, with its log's
//       tail (an error when there is no result: the build stopped before any core).
// In GitHub Actions the messages are ::warning / ::error annotations (readable through the public check-runs API).
import { readFileSync, existsSync, appendFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
export const MAX_MISSING = 1 / 3;              // more native cores missing than this share: do not deploy

// FM1VST's cores the site builds natively: the un-gated ones of data/cores.json
export const ungatedCores = (cores) => cores.filter((c) => !c.gated).map((c) => c.core);

// the native cores whose .wasm is there (has(core) -> bool): { present, missing }
export function nativeAvailability(coreIds, has) {
  const present = coreIds.filter((id) => has(id));
  return { present, missing: coreIds.filter((id) => !present.includes(id)) };
}

// the guard. catalog: the site's catalog.json; has(path) -> bool, path relative to the site; expected: the un-gated
// core ids; managers: the site's managers/index.json (null: none). -> { errors: [], warnings: [], missing: [] }
export function guardSite({ catalog, has, expected, managers = null }) {
  const errors = [], warnings = [], P = catalog.paths || {}, E = P.engines || {};
  const fws = catalog.firmwares || [];
  // native: every entry's .wasm, and the engine
  const natives = fws.filter((f) => f.native);
  for (const f of natives) if (!has(f.native.wasm)) errors.push(`${f.id}: the catalog's native engine ${f.native.wasm} is not in the site`);
  if (natives.length && !(E.native && has(E.native.module))) errors.push(`the native engine ${E.native ? E.native.module : "(no path)"} is not in the site, ${natives.length} entries use it`);
  // FM1VST's cores: a few may be missing (their firmwares run emulated), not most of them
  const tmpl = E.native ? E.native.wasm : "engines/native/{core}.wasm";
  const { missing } = nativeAvailability(expected, (id) => has(tmpl.replace("{core}", id)));
  if (missing.length > expected.length * MAX_MISSING)
    errors.push(`${missing.length} of ${expected.length} native cores are missing (more than a third: ${missing.join(", ")}); the deployed site stays`);
  else if (missing.length) warnings.push(`${missing.length} of ${expected.length} native cores are missing (${missing.join(", ")}): their firmwares run emulated, or are left out`);
  // the .rfm1 engine (the visitor's own files: JavaScript only, always published), and the module the native and the
  // .rfm1 engines import (engines/common/shared.js, next to their folders)
  if (E.rfm1 && !has(E.rfm1.module)) errors.push(`the .rfm1 engine ${E.rfm1.module} is not in the site`);
  for (const k of ["native", "rfm1"]) {
    const mod = E[k] && E[k].module, shared = mod ? mod.replace(/[^/]+\/[^/]+$/, "common/shared.js") : null;
    if (mod && has(mod) && !has(shared)) errors.push(`the ${k} engine ${mod} is in the site without ${shared}, which it imports`);
  }
  // emulated: the engine every Hub firmware runs on, and the packages the site hosts itself
  const emus = fws.filter((f) => f.engines && f.engines.includes("emu"));
  if (emus.length) for (const k of ["module", "wasm", "hook"]) {
    if (!(E.emu && E.emu[k] && has(E.emu[k]))) errors.push(`the emulated engine's ${k} ${E.emu ? E.emu[k] : "(no path)"} is not in the site, ${emus.length} entries use it`);
  }
  for (const f of emus) if (f.emu && f.emu.from === "site" && !has(f.emu.pkg)) errors.push(`${f.id}: its package ${f.emu.pkg} (hosted here) is not in the site`);
  // the hosted managers: what index.json lists must be there
  const mbase = P.managers || "managers/";
  if (!managers) warnings.push(`no ${mbase}index.json: the Editor button shows only links`);
  else {
    if (!has(`${mbase}fm1-shim.js`)) errors.push(`${mbase}fm1-shim.js (the managers' Web MIDI shim) is not in the site`);
    for (const m of managers.managers || []) {
      const page = `${mbase}${String(m.path || `${m.id}/`).replace(/\/?$/, "/")}index.html`;
      if (!has(page)) errors.push(`manager ${m.id}: ${page} is not in the site`);
    }
  }
  // Use my FM-1: optional (the page keeps its stub), but never remote.js without its module
  if (P.remote && has(P.remote) && !has("build/remote/remotefm1.mjs")) errors.push(`${P.remote} is in the site without build/remote/remotefm1.mjs`);
  if (P.installer && !has(`${P.installer}index.html`)) errors.push(`the installer ${P.installer}index.html is not in the site`);
  return { errors, warnings, missing };
}

// build.sh's result.json -> [{ level, title, message }]
export function nativeAnnotations(result) {
  if (!result) return [{ level: "error", title: "native cores not built", message: "web/engines/native/build.sh stopped before building any core (no result.json): no native engine is published" }];
  return (result.failed || []).map((f) => ({ level: "warning", title: `native core ${f.id} not built (${f.stage})`,
    message: `${f.id} failed at ${f.stage}; it is left out and its firmware runs emulated (or is left out).\n${f.log || ""}` }));
}

// one message: an annotation in GitHub Actions, a line otherwise
export function annotate({ level, title, message }) {
  if (process.env.GITHUB_ACTIONS) {
    const esc = (s) => String(s).replace(/%/g, "%25").replace(/\r/g, "").replace(/\n/g, "%0A");
    const t = String(title).replace(/%/g, "%25").replace(/[,:\n\r]/g, " ");
    console.log(`::${level} title=${t}::${esc(message)}`);
  } else console.log(`${level}: ${title}: ${message}`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const summary = (line) => { console.log(line); if (process.env.GITHUB_STEP_SUMMARY) appendFileSync(process.env.GITHUB_STEP_SUMMARY, line + "\n"); };
  const [cmd, target] = process.argv.slice(2);
  const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
  if (cmd === "native-result") {
    const r = target && existsSync(target) ? JSON.parse(readFileSync(target, "utf8")) : null;
    const notes = nativeAnnotations(r);
    notes.forEach(annotate);
    if (r) summary(`native cores: ${r.built.length} built, ${r.failed.length} failed${r.failed.length ? ": " + r.failed.map((f) => `${f.id} (${f.stage})`).join(", ") : ""}`);
  } else if (cmd === "check" && target) {
    const site = resolve(target);
    const read = (p) => JSON.parse(readFileSync(join(site, p), "utf8"));
    if (!existsSync(join(site, "catalog.json"))) { annotate({ level: "error", title: "site guard", message: `no ${site}/catalog.json` }); process.exit(1); }
    const catalog = read("catalog.json");
    const cores = JSON.parse(readFileSync(arg("--cores", join(HERE, "..", "data", "cores.json")), "utf8")).cores;
    const mbase = (catalog.paths && catalog.paths.managers) || "managers/";
    const managers = existsSync(join(site, mbase, "index.json")) ? read(join(mbase, "index.json")) : null;
    const g = guardSite({ catalog, has: (p) => existsSync(join(site, p)), expected: ungatedCores(cores), managers });
    g.warnings.forEach((m) => annotate({ level: "warning", title: "site guard", message: m }));
    g.errors.forEach((m) => annotate({ level: "error", title: "not deployed (site guard)", message: m }));
    const nat = catalog.firmwares.filter((f) => f.native).length;
    summary(`site guard: ${g.errors.length ? "**not deployed**: " : ""}${catalog.firmwares.length} firmwares, ${nat} native; ${g.missing.length} native cores missing; ${g.errors.length} errors, ${g.warnings.length} warnings`);
    process.exit(g.errors.length ? 1 : 0);
  } else { console.error("site_guard: check SITE [--cores FILE] | native-result FILE"); process.exit(2); }
}
