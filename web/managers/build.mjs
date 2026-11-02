// SPDX-License-Identifier: GPL-3.0-only
// The firmwares' own preset managers / editors, hosted on the site so a visitor can use them on the firmware running in
// the browser (the native engine). Each is copied from the firmware's own repository at the commit FM1VST pins
// (web/vendor/FM1VST/cores/<tree>), or, for a firmware FM1VST has no tree of (or not at the commit the site's .rfm1 is
// built from), from its GitHub repository at a pinned commit (managers.json "git": { repo, commit }: the page, its files
// and its licence files fetched from raw.githubusercontent.com into --git-cache), at build time: nothing of theirs is
// committed here.
//
//   node web/managers/build.mjs [--cores DIR] [--out DIR] [--git-cache DIR] [--git-raw URL] [--spec FILE]
//     --cores      FM1VST's core trees (default web/vendor/FM1VST/cores)
//     --out        where the site's managers/ folder goes (default web/build/managers; the Pages job: _site/managers)
//     --git-cache  where the git-sourced managers' files are kept (default web/build/git-src/<id>-<commit>; ignored by git)
//     --git-raw    where they are fetched from (default https://raw.githubusercontent.com; tests)
//     --spec       another managers.json (tests)
//
// What goes out (managers.json lists them):
//   <out>/fm1-shim.js            the Web MIDI shim (this folder): a fake navigator.requestMIDIAccess wired to the player
//   <out>/live.html, live-editor.js   the pop-out page for an author's live editor (the Editor panel loads the author's own
//                                page from its site, in a sandbox: web/site/js/live-editor.js); nothing of theirs is copied
//   <out>/index.json             the managers, their sources and the native firmwares' USB port names (the player reads it)
//   <out>/<id>/index.html        the manager's page as its own site build makes it, plus one line: the shim's <script>,
//                                first in <head>. Nothing else of the page is changed, except what managers.json "strip"
//                                and "dropPacks" name (preset data that reaches this site only compiled into a firmware)
//   <out>/<id>/<files>           the files the page loads (its font, fm1backup.js ...), as its make_site.py copies them
//   <out>/<id>/<copy>            a manager that is a folder of its own (PIGEON's sample loader: modules, a worker, a
//                                service worker, vendor/): "copy" { from, to, skip } places the tree's files and folders
//                                where its site build puts them, and "at" is the page's folder under <out>/<id>/
//   <out>/<id>/LICENSE, LICENSING.md   the firmware repository's own ("licenceFiles" where they are named otherwise);
//                                NOTICE.txt: where it comes from, what was changed
//   index.json's path            <id>/ (and "open", a fragment: the part of a one-page site that is its editor,
//                                Mutable Dreams' #device-editor)
// A git-sourced manager's shim gets data-local, the files hosted here: a link of its page to a file of its own site that
// is not hosted (its installer, its guide) opens on its own site
// A manager whose tree is missing, or whose git source cannot be fetched, is skipped with a warning (the player then has
// no editor for that firmware); the build goes on.
import { readFileSync, writeFileSync, mkdirSync, copyFileSync, existsSync, statSync, readdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join, basename, relative } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const CORES = arg("--cores", join(HERE, "..", "vendor", "FM1VST", "cores"));
const OUT = arg("--out", join(HERE, "..", "build", "managers"));
const GIT_CACHE = arg("--git-cache", join(HERE, "..", "build", "git-src"));
const GIT_RAW = arg("--git-raw", "https://raw.githubusercontent.com").replace(/\/+$/, "");
const SPEC = JSON.parse(readFileSync(arg("--spec", join(HERE, "managers.json")), "utf8"));
const licenceFiles = (m) => m.licenceFiles || ["LICENSE", "LICENSING.md"];

// a git-sourced manager's files (managers.json "git": { repo: https://github.com/OWNER/NAME, commit: 40 hex }): its
// page, its files and its licence files, each fetched once from GIT_RAW/OWNER/NAME/<commit>/<path> into
// GIT_CACHE/<id>-<commit> (a file there is the one of that commit: kept). The tree's folder, or throws (a warning)
async function gitTree(m) {
  const g = m.git, gh = /^https:\/\/github\.com\/([\w.-]+)\/([\w.-]+?)(\.git)?\/?$/.exec(g.repo || "");
  if (!gh || !/^[0-9a-f]{40}$/.test(g.commit || "")) throw new Error(`git: ${JSON.stringify(g)}: not a GitHub repository and a full commit`);
  const dir = join(GIT_CACHE, `${m.id}-${g.commit}`);
  const want = [m.page, ...m.files, ...licenceFiles(m)];
  for (const f of want) {
    if (/(^|\/)\.\.(\/|$)/.test(f) || f.startsWith("/")) throw new Error(`${f}: not a path in the repository`);
    const to = join(dir, f);
    if (existsSync(to)) continue;
    const url = `${GIT_RAW}/${gh[1]}/${gh[2]}/${g.commit}/${f.split("/").map(encodeURIComponent).join("/")}`;
    let r;
    try { r = await fetch(url, { signal: AbortSignal.timeout(60000) }); }
    catch (e) { throw new Error(`${url}: ${e.cause ? e.cause.code || e.cause.message : e.message}`); }
    if (r.status === 404 && f !== m.page) continue;                 // (a file it does not have: "missing" in NOTICE.txt)
    if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
    mkdirSync(dirname(to), { recursive: true });
    writeFileSync(to, Buffer.from(await r.arrayBuffer()));
  }
  return dir;
}

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

// a file or a folder of the tree (copy: { from, to, skip: a regular expression of names to leave out }) into dir
function copyTree(tree, c, dir, copied) {
  const skip = c.skip ? new RegExp(c.skip) : null;
  const walk = (from, to) => {
    if (statSync(from).isDirectory()) {
      for (const n of readdirSync(from).sort()) if (!(skip && skip.test(n)) && n !== ".DS_Store") walk(join(from, n), join(to, n));
      return;
    }
    if (/\.syx$/i.test(from)) throw new Error(`${relative(tree, from)}: a raw preset file is never published`);
    mkdirSync(dirname(to), { recursive: true });
    copyFileSync(from, to);
    copied.push(relative(dir, to).split("\\").join("/"));
  };
  if (!existsSync(join(tree, c.from))) throw new Error(`${c.from}: not in the tree`);
  walk(join(tree, c.from), join(dir, c.to));
}

// the shim's <script>, first in <head> (after <meta charset>, which must stay in the first 1024 bytes): before any
// script of the page, so navigator.requestMIDIAccess is ours when the page asks for it
function injectShim(html, m, what, local) {
  const up = "../".repeat(1 + (m.at || "").split("/").filter(Boolean).length);   // (the page's folder: <id>/<at>)
  const esc = (t) => t.replace(/&/g, "&amp;").replace(/"/g, "&quot;");
  const tag = `<script src="${up}fm1-shim.js" data-manager="${m.id}" data-upstream="${esc(m.upstream)}"${local ? ` data-local="${esc(local.join(" "))}"` : ""}></script>`;
  const cs = /<meta\s+charset=["']?utf-8["']?\s*\/?>/i.exec(html), hd = /<head(\s[^>]*)?>/i.exec(html);
  const at = cs ? cs.index + cs[0].length : hd ? hd.index + hd[0].length : -1;
  if (at < 0) throw new Error(`${what}: no <head> to put the shim in`);
  if (/<script/i.test(html.slice(0, at))) throw new Error(`${what}: a script comes before <head>'s start: the shim would not be first`);
  return html.slice(0, at) + "\n" + tag + html.slice(at);
}

mkdirSync(OUT, { recursive: true });
copyFileSync(join(HERE, "fm1-shim.js"), join(OUT, "fm1-shim.js"));
copyFileSync(join(HERE, "live.html"), join(OUT, "live.html"));
copyFileSync(join(HERE, "..", "site", "js", "live-editor.js"), join(OUT, "live-editor.js"));
const fm1vst = commitOf(join(CORES, ".."));
const index = { about: "The firmwares' own managers hosted by the FM-1 Player (web/managers/build.mjs).", fm1vst,
                ports: SPEC.ports, managers: [] };
let bad = 0;
for (const m of SPEC.managers) {
  let tree;
  if (m.git) {
    try { tree = await gitTree(m); }
    catch (e) { console.warn(`managers: ${m.id}: its source ${m.git.repo}@${String(m.git.commit).slice(0, 7)} could not be fetched (${e.message}): skipped`); continue; }
  } else tree = join(CORES, m.tree);
  if (!existsSync(join(tree, m.page))) { console.warn(`managers: ${m.id}: no ${m.page} in ${tree} (the submodule is not checked out?): skipped`); continue; }
  try {
    const commit = m.git ? m.git.commit : commitOf(tree);
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
    const local = m.git ? ["index.html", ...m.files.map((f) => basename(f)), ...licenceFiles(m).filter((f) => existsSync(join(tree, f))).map((f) => basename(f)), "NOTICE.txt"] : null;
    html = injectShim(html, m, m.page, local);
    const dir = join(OUT, m.id);
    mkdirSync(dir, { recursive: true });
    const copied = [];
    for (const c of m.copy || []) copyTree(tree, c, dir, copied);
    mkdirSync(join(dir, m.at || ""), { recursive: true });
    writeFileSync(join(dir, m.at || "", "index.html"), html);
    for (const f of m.files) {
      if (/\.syx$/i.test(f)) throw new Error(`${f}: a raw preset file is never published`);
      if (!existsSync(join(tree, f))) { notes.push(`missing in the tree: ${f}`); continue; }
      copyFileSync(join(tree, f), join(dir, basename(f)));
      copied.push(basename(f));
    }
    for (const f of licenceFiles(m)) if (existsSync(join(tree, f))) { copyFileSync(join(tree, f), join(dir, basename(f))); copied.push(basename(f)); }
    const source = commit ? `${m.repo}/tree/${commit}` : m.repo;
    writeFileSync(join(dir, "NOTICE.txt"), [
      `${m.title}, hosted by the FM-1 Player (RemoteFM1, https://github.com/Quixotic7/RemoteFM1).`,
      ``,
      `Author: ${m.author}. Licence: ${m.licence} (${licenceFiles(m).map((f) => basename(f)).join(", ")} here: the repository's own).`,
      `Source: ${source} (${m.page}${m.lib ? ", " + m.lib.join(", ") : ""}${m.files.length ? ", " + m.files.join(", ") : ""}${(m.copy || []).length ? ", " + m.copy.map((c) => c.from).join(", ") : ""}).`,
      `Its own site: ${m.upstream}`,
      ``,
      ...((m.copy || []).length ? [`Laid out as its own site build does: ${m.copy.map((c) => `${c.from} -> ${c.to}`).join(", ")}${m.at ? `; the page is ${m.at}index.html` : ""}.`, ``] : []),
      `What the FM-1 Player changed: ${m.at || ""}index.html is ${m.page}${m.kind === "choralroot-manager" ? ", assembled as the repository's web/make_site.py does (its /*LIB*/ and /*PACKS*/ filled in)" : ""},`,
      `with one line added first in <head>: <script src="${"../".repeat(1 + (m.at || "").split("/").filter(Boolean).length)}fm1-shim.js">, the player's Web MIDI shim (GPL-3.0-only,`,
      `https://github.com/Quixotic7/RemoteFM1/tree/main/web/managers). The shim gives the page one MIDI input and output named as`,
      `the firmware's USB port, wired to the firmware running in the player, and opens the page's links outside this folder on`,
      `its own site${m.git ? " (here also a link to a file of its folder that is not hosted: data-local lists the hosted ones)" : ""}.`,
      ...notes.map((n) => `- ${n}`),
      ``,
    ].join("\n"));
    index.managers.push({ id: m.id, title: m.title, firmwares: m.firmwares, path: `${m.id}/${m.at || ""}${m.open || ""}`, author: m.author, license: m.licence,
                          source, commit, ...(m.git ? { git: m.git.repo } : {}), upstream: m.upstream, ...("live" in m ? { live: m.live } : {}), files: [...new Set([`${m.at || ""}index.html`, ...copied, "NOTICE.txt"])] });
    console.log(`managers: ${m.id}: ${m.git ? m.git.repo : m.tree}@${(commit || "?").slice(0, 7)} -> ${dir} (${html.length} B${notes.length ? "; " + notes.join("; ") : ""})`);
  } catch (e) { bad++; console.error(`managers: ${m.id}: ${e.message}`); }
}
writeFileSync(join(OUT, "index.json"), JSON.stringify(index, null, 1) + "\n");
console.log(`managers: ${index.managers.length} of ${SPEC.managers.length} hosted in ${OUT}`);
process.exit(bad ? 1 : 0);
