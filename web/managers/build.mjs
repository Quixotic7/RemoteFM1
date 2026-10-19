// SPDX-License-Identifier: GPL-3.0-only
// The firmwares' own preset managers / editors, hosted on the site so a visitor can use them on the firmware running in
// the browser (the native engine). Each is copied from the firmware's own repository at the commit FM1VST pins
// (web/vendor/FM1VST/cores/<tree>), at build time: nothing of theirs is committed here.
//
//   node web/managers/build.mjs [--cores DIR] [--out DIR]
//     --cores  FM1VST's core trees (default web/vendor/FM1VST/cores)
//     --out    where the site's managers/ folder goes (default web/build/managers; the Pages job: _site/managers)
//
// What goes out (managers.json lists them):
//   <out>/fm1-shim.js            the Web MIDI shim (this folder): a fake navigator.requestMIDIAccess wired to the player
//   <out>/index.json             the managers, their sources and the native firmwares' USB port names (the player reads it)
//   <out>/<id>/index.html        the manager's page as its own site build makes it, plus one line: the shim's <script>,
//                                first in <head>. Nothing else of the page is changed, except what managers.json "strip"
//                                and "dropPacks" name (preset data that reaches this site only compiled into a firmware)
//   <out>/<id>/<files>           the files the page loads (its font, fm1backup.js ...), as its make_site.py copies them
//   <out>/<id>/LICENSE, LICENSING.md   the firmware repository's own; NOTICE.txt: where it comes from, what was changed
// A manager whose tree is missing is skipped with a warning (the player then has no editor for that firmware).
import { readFileSync, writeFileSync, mkdirSync, copyFileSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join, basename } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const CORES = arg("--cores", join(HERE, "..", "vendor", "FM1VST", "cores"));
const OUT = arg("--out", join(HERE, "..", "build", "managers"));
const SPEC = JSON.parse(readFileSync(join(HERE, "managers.json"), "utf8"));

// make_site.py strip_module: an ES module's text inlined into the page's own <script type="module">
const stripModule = (src) => src.replace(/^export\s+/gm, "").replace(/^import .*?;\n/gm, "");
const commitOf = (dir) => { try { return execFileSync("git", ["-C", dir, "rev-parse", "HEAD"], { encoding: "utf8" }).trim(); } catch (_) { return null; } };
const once = (text, mark, what) => {
  const n = text.split(mark).length - 1;
  if (n !== 1) throw new Error(`${what}: "${mark}" found ${n} times (expected once): the manager changed; update web/managers/managers.json`);
};

// ChoralRoot's device manager as its web/make_site.py manager_page() makes it: /*LIB*/ = the four modules, /*PACKS*/null =
// the packs of web/packs inlined; the packs managers.json drops are left out of both the index and the inlined set
function choralrootManager(tree, m) {
  let html = readFileSync(join(tree, m.page), "utf8");
  const lib = m.lib.map((f) => stripModule(readFileSync(join(tree, f), "utf8"))).join("\n");
  const packsDir = join(tree, m.packs);
  const drop = m.dropPacks || {};
  const index = existsSync(join(packsDir, "index.json")) ? JSON.parse(readFileSync(join(packsDir, "index.json"), "utf8")) : { packs: [] };
  const kept = index.packs.filter((p) => !(p.file in drop));
  const embed = { index: kept.map((p) => ({ ...p, url: "packs/" + p.file })),
                  packs: Object.fromEntries(kept.map((p) => [p.file, JSON.parse(readFileSync(join(packsDir, p.file), "utf8"))])) };
  for (const mark of ["/*LIB*/", "/*PACKS*/null"]) once(html, mark, m.page);
  html = html.replace("/*LIB*/", () => lib).replace("/*PACKS*/null", () => JSON.stringify(embed).replace(/<\//g, "<\\/"));
  const dropped = index.packs.filter((p) => p.file in drop).map((p) => `${m.packs}/${p.file} (${drop[p.file]})`);
  return { html, notes: dropped.map((d) => `left out: ${d}`) };
}

// the shim's <script>, first in <head> (after <meta charset>, which must stay in the first 1024 bytes): before any
// script of the page, so navigator.requestMIDIAccess is ours when the page asks for it
function injectShim(html, m, what) {
  const tag = `<script src="../fm1-shim.js" data-manager="${m.id}" data-upstream="${m.upstream}"></script>`;
  const cs = /<meta\s+charset=["']?utf-8["']?\s*\/?>/i.exec(html), hd = /<head(\s[^>]*)?>/i.exec(html);
  const at = cs ? cs.index + cs[0].length : hd ? hd.index + hd[0].length : -1;
  if (at < 0) throw new Error(`${what}: no <head> to put the shim in`);
  if (/<script/i.test(html.slice(0, at))) throw new Error(`${what}: a script comes before <head>'s start: the shim would not be first`);
  return html.slice(0, at) + "\n" + tag + html.slice(at);
}

mkdirSync(OUT, { recursive: true });
copyFileSync(join(HERE, "fm1-shim.js"), join(OUT, "fm1-shim.js"));
const fm1vst = commitOf(join(CORES, ".."));
const index = { about: "The firmwares' own managers hosted by the FM-1 Player (web/managers/build.mjs).", fm1vst,
                ports: SPEC.ports, managers: [] };
let bad = 0;
for (const m of SPEC.managers) {
  const tree = join(CORES, m.tree);
  if (!existsSync(join(tree, m.page))) { console.warn(`managers: ${m.id}: no ${m.page} in ${tree} (the submodule is not checked out?): skipped`); continue; }
  try {
    const commit = commitOf(tree);
    const notes = [];
    let html;
    if (m.kind === "choralroot-manager") { const r = choralrootManager(tree, m); html = r.html; notes.push(...r.notes); }
    else html = readFileSync(join(tree, m.page), "utf8");
    for (const s of m.strip || []) {
      once(html, s.begin, m.page); once(html, s.end, m.page);
      const a = html.indexOf(s.begin), b = html.indexOf(s.end) + s.end.length;
      if (b < a) throw new Error(`${m.page}: ${s.end} before ${s.begin}`);
      html = html.slice(0, a) + s.with + html.slice(b);
      notes.push(`changed: the text between ${s.begin} and ${s.end} is "${s.with}" (${s.why})`);
    }
    html = injectShim(html, m, m.page);
    const dir = join(OUT, m.id);
    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, "index.html"), html);
    const copied = [];
    for (const f of m.files) {
      if (/\.syx$/i.test(f)) throw new Error(`${f}: a raw preset file is never published`);
      if (!existsSync(join(tree, f))) { notes.push(`missing in the tree: ${f}`); continue; }
      copyFileSync(join(tree, f), join(dir, basename(f)));
      copied.push(basename(f));
    }
    for (const f of ["LICENSE", "LICENSING.md"]) if (existsSync(join(tree, f))) { copyFileSync(join(tree, f), join(dir, f)); copied.push(f); }
    const source = commit ? `${m.repo}/tree/${commit}` : m.repo;
    writeFileSync(join(dir, "NOTICE.txt"), [
      `${m.title}, hosted by the FM-1 Player (RemoteFM1, https://github.com/Quixotic7/RemoteFM1).`,
      ``,
      `Author: ${m.author}. Licence: ${m.licence} (LICENSE, LICENSING.md here: the repository's own).`,
      `Source: ${source} (${m.page}${m.lib ? ", " + m.lib.join(", ") : ""}${m.files.length ? ", " + m.files.join(", ") : ""}).`,
      `Its own site: ${m.upstream}`,
      ``,
      `What the FM-1 Player changed: index.html is ${m.page}${m.kind === "choralroot-manager" ? ", assembled as the repository's web/make_site.py does (its /*LIB*/ and /*PACKS*/ filled in)" : ""},`,
      `with one line added first in <head>: <script src="../fm1-shim.js">, the player's Web MIDI shim (GPL-3.0-only,`,
      `https://github.com/Quixotic7/RemoteFM1/tree/main/web/managers). The shim gives the page one MIDI input and output named as`,
      `the firmware's USB port, wired to the firmware running in the player, and opens the page's links outside this folder on`,
      `its own site.`,
      ...notes.map((n) => `- ${n}`),
      ``,
    ].join("\n"));
    index.managers.push({ id: m.id, title: m.title, firmwares: m.firmwares, path: `${m.id}/`, author: m.author, license: m.licence,
                          source, commit, upstream: m.upstream, files: ["index.html", ...copied, "NOTICE.txt"] });
    console.log(`managers: ${m.id}: ${m.tree}@${(commit || "?").slice(0, 7)} -> ${dir} (${html.length} B${notes.length ? "; " + notes.join("; ") : ""})`);
  } catch (e) { bad++; console.error(`managers: ${m.id}: ${e.message}`); }
}
writeFileSync(join(OUT, "index.json"), JSON.stringify(index, null, 1) + "\n");
console.log(`managers: ${index.managers.length} of ${SPEC.managers.length} hosted in ${OUT}`);
process.exit(bad ? 1 : 0);
