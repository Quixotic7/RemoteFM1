// SPDX-License-Identifier: GPL-3.0-only
// The compiled .rfm1 firmwares of Quixotic7/RemoteFM1FW (the release the iOS app reads too), for the site to host:
// the latest release's (or --tag's) .rfm1 assets and its SHA256SUMS, every file checked against SHA256SUMS, written to
// --out (default web/site/fw/rfm1/, ignored by git: the binaries are never committed, the Pages build fetches them at
// deploy) with SHA256SUMS and release.json ({ repo, tag, url, files }). build_catalog.mjs --fw-catalog <dir> lists them.
// Also the repository's catalog/catalog.json (catalog/SCHEMA.md: per firmware id its card, its author's editor page and
// its USB MIDI port name, checked against the firmware's USB settings), from --meta-ref (default main), as catalog.json in
// --out: build_catalog.mjs takes each firmware's editor and port from it. Not fetched: a warning, the rest stands.
//   node web/site/tools/fetch_fw_catalog.mjs [--repo OWNER/NAME] [--tag TAG] [--meta-ref REF] [--out DIR]
//   node web/site/tools/fetch_fw_catalog.mjs --from DIR [--out DIR]     a local copy (offline, a fixture): DIR has the
//                                                                       .rfm1 files and SHA256SUMS (and catalog.json)
// GITHUB_TOKEN / GH_TOKEN is used for the API when set (the release is public: it is not needed). Any file that is not
// its SHA256SUMS line, or a .rfm1 SHA256SUMS does not list (or the other way round), fails it (exit 1), writing nothing.
import { readFileSync, writeFileSync, mkdirSync, readdirSync, unlinkSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
// the site's own copies (the last deploy): a file whose SHA-256 is the release's comes from there, not GitHub
const deployed = arg("--deployed", "https://remotefm1.com/fw/rfm1/");
const repo = arg("--repo", "Quixotic7/RemoteFM1FW"), tag = arg("--tag", null), from = arg("--from", null), metaRef = arg("--meta-ref", "main");
const out = arg("--out", join(HERE, "..", "fw", "rfm1"));
const die = (m) => { console.error(`fetch_fw_catalog: ${m}`); process.exit(1); };
const sha = (b) => createHash("sha256").update(b).digest("hex");

// SHA256SUMS ("<hex>  <name>" or "<hex> *<name>") -> Map(name, hex)
export function parseSums(text) {
  const m = new Map();
  for (const line of String(text).split("\n")) {
    const r = /^([0-9a-f]{64})\s+\*?(\S+)\s*$/i.exec(line.trim());
    if (r) m.set(r[2], r[1].toLowerCase());
  }
  return m;
}

async function main() {
  let sumsText, files = new Map(), skipped = new Set(), rel = { repo, tag: null, url: null }, meta = null;
  if (from) {
    try { meta = readFileSync(join(from, "catalog.json"), "utf8"); } catch (_) { meta = null; }
    sumsText = readFileSync(join(from, "SHA256SUMS"), "utf8");
    for (const n of readdirSync(from)) if (n.endsWith(".rfm1")) files.set(n, readFileSync(join(from, n)));
    try { rel = { ...rel, ...JSON.parse(readFileSync(join(from, "release.json"), "utf8")) }; } catch (_) { rel.tag = "local"; }
  } else {
    const token = process.env.GITHUB_TOKEN || process.env.GH_TOKEN;
    const hdr = { Accept: "application/vnd.github+json", "User-Agent": "remotefm1-site", ...(token ? { Authorization: `Bearer ${token}` } : {}) };
    const api = `https://api.github.com/repos/${repo}/releases/${tag ? `tags/${encodeURIComponent(tag)}` : "latest"}`;
    // GitHub's download host answers 5xx or hangs now and then (2026-11-09: a fifth of the files for hours; one
    // such file made a deploy without any .rfm1, and long retries a deploy that ran past 30 min). So: tries with a
    // short timeout each, and for each .rfm1 first the copy this site already serves (when its SHA-256 is the
    // release's), then the download link, then the same file through api.github.com.
    const retry = async (url, opts, { ms = 30000, tries = 1, soft = false } = {}) => {
      let last = "";
      for (let i = 0; i < tries; i++) {
        if (i) await new Promise((res) => setTimeout(res, 2000 * i));
        try {
          const res = await fetch(url, { ...opts, signal: AbortSignal.timeout(ms) });
          if (res.ok) return Buffer.from(await res.arrayBuffer());
          last = String(res.status);
          if (res.status < 500 && res.status !== 429) break;    // (a 404 stays one)
        } catch (e) { last = e.message || String(e); }
      }
      if (!soft) die(`${url}: ${last}`);
      console.warn(`fetch_fw_catalog: ${url}: ${last}`);
      return null;
    };
    const ua = { "User-Agent": "remotefm1-site" };
    const j = JSON.parse((await retry(api, { headers: hdr }, { tries: 3 })).toString("utf8"));
    rel = { repo, tag: j.tag_name, url: j.html_url };
    const viaApi = (a, soft) => retry(a.url, { headers: { ...hdr, Accept: "application/octet-stream" } }, { ms: 60000, tries: 3, soft });
    const sumsAsset = j.assets.find((a) => a.name === "SHA256SUMS");
    if (!sumsAsset) die(`${repo} ${j.tag_name} has no SHA256SUMS`);
    sumsText = ((await retry(sumsAsset.browser_download_url, { headers: ua }, { soft: true })) || (await viaApi(sumsAsset, false))).toString("utf8");
    const want = parseSums(sumsText);
    let fromSite = 0;
    const one = async (a) => {
      const hash = want.get(a.name);
      const kept = hash && (await retry(new URL(a.name, deployed).href, { headers: ua }, { ms: 20000, soft: true }));
      if (kept && sha(kept) === hash) { fromSite++; return kept; }
      return (await retry(a.browser_download_url, { headers: ua }, { soft: true })) || viaApi(a, true);
    };
    const todo = j.assets.filter((a) => a.name.endsWith(".rfm1"));
    const work = async () => {
      for (let a; (a = todo.shift()); ) {
        const b = await one(a);
        if (b) files.set(a.name, b);
        else { skipped.add(a.name); console.log(`::warning title=RemoteFM1FW::${a.name} could not be downloaded: left out of this deploy`); }
      }
    };
    await Promise.all(Array.from({ length: 8 }, work));
    console.log(`fetch_fw_catalog: ${fromSite} of ${files.size} from this site's last deploy (${deployed}), the rest from GitHub`);
    if (skipped.size * 2 > skipped.size + files.size) die(`${skipped.size} of ${skipped.size + files.size} .rfm1 files could not be downloaded`);
    const mu = `https://raw.githubusercontent.com/${repo}/${metaRef}/catalog/catalog.json`;
    try {
      const res = await fetch(mu, { headers: { "User-Agent": "remotefm1-site" }, signal: AbortSignal.timeout(30000) });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      meta = await res.text();
    } catch (e) { console.warn(`fetch_fw_catalog: ${mu}: ${e.message || e} (no editors / ports from it)`); meta = null; }
  }
  if (meta !== null) {
    try { const m = JSON.parse(meta); if (!m || typeof m.firmwares !== "object") throw new Error("no firmwares"); }
    catch (e) { console.warn(`fetch_fw_catalog: its catalog.json is not one (${e.message}): left out`); meta = null; }
  }
  const sums = parseSums(sumsText);
  const listed = [...sums.keys()].filter((n) => n.endsWith(".rfm1"));
  if (!listed.length) die("SHA256SUMS lists no .rfm1");
  for (const n of listed) if (!files.has(n) && !skipped.has(n)) die(`${n}: in SHA256SUMS, not in the release`);
  for (const [n, b] of files) {
    if (!sums.has(n)) die(`${n}: not in SHA256SUMS`);
    if (sha(b) !== sums.get(n)) die(`${n}: SHA-256 ${sha(b)}, SHA256SUMS says ${sums.get(n)}`);
  }
  // (all checked: replace what --out had)
  mkdirSync(out, { recursive: true });
  for (const n of readdirSync(out)) if (n.endsWith(".rfm1") || n === "SHA256SUMS" || n === "release.json" || n === "catalog.json") unlinkSync(join(out, n));
  const names = [...files.keys()].sort();
  for (const n of names) writeFileSync(join(out, n), files.get(n));
  writeFileSync(join(out, "SHA256SUMS"), names.map((n) => `${sums.get(n)}  ${n}\n`).join(""));
  writeFileSync(join(out, "release.json"), JSON.stringify({ ...rel, files: names, ...(meta !== null ? { catalog: `catalog/catalog.json@${from ? "local" : metaRef}` } : {}) }, null, 1) + "\n");
  if (meta !== null) writeFileSync(join(out, "catalog.json"), meta);
  console.log(`fetch_fw_catalog: ${names.length} .rfm1 files of ${rel.repo} ${rel.tag}, checked against SHA256SUMS${skipped.size ? ` (${skipped.size} left out: ${[...skipped].join(", ")})` : ""} -> ${out}`);
}
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) await main();
