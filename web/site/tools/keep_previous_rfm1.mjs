// SPDX-License-Identifier: GPL-3.0-only
// The previous deploy's .rfm1 files, kept one more deploy. A deploy that changes a firmware's version drops its old
// file (fw/rfm1/<id>-<old>.rfm1), but GitHub Pages' CDN serves the old catalog.json for up to 10 minutes (max-age 600)
// after it: a visitor's player then asks for the old file and gets 404 ("could not be downloaded", a test engine).
// So the build carries over every .rfm1 the live site's catalog names that the new site lacks, each fetched from the
// live site and checked against that catalog's SHA-256 (a mismatch or a failed fetch: left out, a warning). The new
// catalog does not list them; the next deploy drops them. Never fails the deploy.
//   node web/site/tools/keep_previous_rfm1.mjs [--site _site] [--live https://remotefm1.com/]
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const SAFE = /^fw\/(?:[A-Za-z0-9_.+-]+\/)?[A-Za-z0-9_.+-]+\.rfm1$/;   // fw/<name>.rfm1 or fw/<dir>/<name>.rfm1, no ".."
const sha = (b) => createHash("sha256").update(b).digest("hex");

// the live catalog's .rfm1 files: [{ file, sha256 }]
export function rfm1Files(catalog) {
  const out = new Map();
  for (const f of (catalog && catalog.firmwares) || []) {
    const r = f && f.rfm1;
    if (r && typeof r.file === "string" && SAFE.test(r.file) && !r.file.includes("..") && /^[0-9a-f]{64}$/.test(r.sha256 || ""))
      out.set(r.file, r.sha256);
  }
  return [...out].map(([file, sha256]) => ({ file, sha256 }));
}

// -> { kept: [file], present: n, failed: [{ file, why }] }
export async function keepPrevious({ site, live, fetchFn = fetch, log = console.log }) {
  const res = { kept: [], present: 0, failed: [] };
  let cat;
  try {
    const r = await fetchFn(new URL("catalog.json", live).href, { cache: "no-store" });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    cat = await r.json();
  } catch (e) {
    res.failed.push({ file: "catalog.json", why: e.message });
    return res;
  }
  for (const { file, sha256 } of rfm1Files(cat)) {
    const dst = join(site, file);
    if (existsSync(dst)) { res.present++; continue; }
    try {
      const r = await fetchFn(new URL(file, live).href);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const b = Buffer.from(await r.arrayBuffer());
      if (sha(b) !== sha256) throw new Error("its SHA-256 is not the live catalog's");
      mkdirSync(dirname(dst), { recursive: true });
      writeFileSync(dst, b);
      res.kept.push(file);
      log(`keep_previous_rfm1: ${file} kept from the live site (the CDN's old catalog may still name it)`);
    } catch (e) {
      res.failed.push({ file, why: e.message });
    }
  }
  return res;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
  const r = await keepPrevious({ site: arg("--site", "_site"), live: arg("--live", "https://remotefm1.com/") });
  for (const f of r.failed) console.log(`::warning title=keep_previous_rfm1::${f.file}: ${f.why}`);
  console.log(`keep_previous_rfm1: ${r.kept.length} kept, ${r.present} already in the new site, ${r.failed.length} not kept`);
}
