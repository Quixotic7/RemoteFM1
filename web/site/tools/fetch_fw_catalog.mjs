// SPDX-License-Identifier: GPL-3.0-only
// The compiled .rfm1 firmwares of Quixotic7/RemoteFM1FW (the release the iOS app reads too), for the site to host:
// the latest release's (or --tag's) .rfm1 assets and its SHA256SUMS, every file checked against SHA256SUMS, written to
// --out (default web/site/fw/rfm1/, ignored by git: the binaries are never committed, the Pages build fetches them at
// deploy) with SHA256SUMS and release.json ({ repo, tag, url, files }). build_catalog.mjs --fw-catalog <dir> lists them.
//   node web/site/tools/fetch_fw_catalog.mjs [--repo OWNER/NAME] [--tag TAG] [--out DIR]
//   node web/site/tools/fetch_fw_catalog.mjs --from DIR [--out DIR]     a local copy (offline, a fixture): DIR has the
//                                                                       .rfm1 files and SHA256SUMS
// GITHUB_TOKEN / GH_TOKEN is used for the API when set (the release is public: it is not needed). Any file that is not
// its SHA256SUMS line, or a .rfm1 SHA256SUMS does not list (or the other way round), fails it (exit 1), writing nothing.
import { readFileSync, writeFileSync, mkdirSync, readdirSync, unlinkSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const repo = arg("--repo", "Quixotic7/RemoteFM1FW"), tag = arg("--tag", null), from = arg("--from", null);
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
  let sumsText, files = new Map(), rel = { repo, tag: null, url: null };
  if (from) {
    sumsText = readFileSync(join(from, "SHA256SUMS"), "utf8");
    for (const n of readdirSync(from)) if (n.endsWith(".rfm1")) files.set(n, readFileSync(join(from, n)));
    try { rel = { ...rel, ...JSON.parse(readFileSync(join(from, "release.json"), "utf8")) }; } catch (_) { rel.tag = "local"; }
  } else {
    const token = process.env.GITHUB_TOKEN || process.env.GH_TOKEN;
    const hdr = { Accept: "application/vnd.github+json", "User-Agent": "remotefm1-site", ...(token ? { Authorization: `Bearer ${token}` } : {}) };
    const api = `https://api.github.com/repos/${repo}/releases/${tag ? `tags/${encodeURIComponent(tag)}` : "latest"}`;
    const r = await fetch(api, { headers: hdr, signal: AbortSignal.timeout(30000) });
    if (!r.ok) die(`${api}: ${r.status}`);
    const j = await r.json();
    rel = { repo, tag: j.tag_name, url: j.html_url };
    const get = async (a) => {
      const res = await fetch(a.browser_download_url, { headers: { "User-Agent": "remotefm1-site" }, signal: AbortSignal.timeout(120000) });
      if (!res.ok) die(`${a.browser_download_url}: ${res.status}`);
      return Buffer.from(await res.arrayBuffer());
    };
    const sumsAsset = j.assets.find((a) => a.name === "SHA256SUMS");
    if (!sumsAsset) die(`${repo} ${j.tag_name} has no SHA256SUMS`);
    sumsText = (await get(sumsAsset)).toString("utf8");
    for (const a of j.assets) if (a.name.endsWith(".rfm1")) files.set(a.name, await get(a));
  }
  const sums = parseSums(sumsText);
  const listed = [...sums.keys()].filter((n) => n.endsWith(".rfm1"));
  if (!listed.length) die("SHA256SUMS lists no .rfm1");
  for (const n of listed) if (!files.has(n)) die(`${n}: in SHA256SUMS, not in the release`);
  for (const [n, b] of files) {
    if (!sums.has(n)) die(`${n}: not in SHA256SUMS`);
    if (sha(b) !== sums.get(n)) die(`${n}: SHA-256 ${sha(b)}, SHA256SUMS says ${sums.get(n)}`);
  }
  // (all checked: replace what --out had)
  mkdirSync(out, { recursive: true });
  for (const n of readdirSync(out)) if (n.endsWith(".rfm1") || n === "SHA256SUMS" || n === "release.json") unlinkSync(join(out, n));
  const names = [...files.keys()].sort();
  for (const n of names) writeFileSync(join(out, n), files.get(n));
  writeFileSync(join(out, "SHA256SUMS"), names.map((n) => `${sums.get(n)}  ${n}\n`).join(""));
  writeFileSync(join(out, "release.json"), JSON.stringify({ ...rel, files: names }, null, 1) + "\n");
  console.log(`fetch_fw_catalog: ${names.length} .rfm1 files of ${rel.repo} ${rel.tag}, checked against SHA256SUMS -> ${out}`);
}
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) await main();
