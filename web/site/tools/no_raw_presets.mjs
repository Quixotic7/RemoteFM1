// SPDX-License-Identifier: GPL-3.0-only
// No raw preset source in what the site publishes. ChoralRoot and Melodee compile Casio's CZ-1 preset tones and
// Sequential's Prophet-5 factory programs into their firmware (the owner's call, 2026-10-19: the compiled firmware is
// published, the raw SysEx is not). Fails (exit 1) when DIR holds:
//   - a .syx file, or anything under a cz1-factory/ or prophet5-factory/ folder;
//   - a file with the content of any .syx in FM1VST's core trees (SHA-256);
//   - a file holding a run of raw bytes from one of those .syx files (32-byte windows taken through each file: a raw
//     dump embedded in a .wasm, a .js or a .json; the firmware's own packed tables never match them).
//   node web/site/tools/no_raw_presets.mjs DIR [--cores web/vendor/FM1VST/cores] [--top]
// --top: only the files directly in DIR (web/build/native: the published .wasm, not the build's source mirror)
// Without a cores tree (or with no .syx in it) the content checks cannot run: it says so and fails unless
// --names-only is given.
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const dir = process.argv[2];
const coresDir = arg("--cores", join(HERE, "..", "..", "vendor", "FM1VST", "cores"));
const namesOnly = process.argv.includes("--names-only"), top = process.argv.includes("--top");
if (!dir || dir.startsWith("--") || !existsSync(dir)) { console.error("usage: no_raw_presets.mjs DIR [--cores DIR] [--names-only] [--top]"); process.exit(2); }

function* files(root, skip = new Set(), deep = true) {
  let ents = [];
  try { ents = readdirSync(root, { withFileTypes: true }); } catch (_) { return; }
  for (const e of ents) {
    const p = join(root, e.name);
    if (e.isDirectory()) { if (deep && !skip.has(e.name)) yield* files(p, skip); }
    else if (e.isFile()) yield p;
  }
}

const sha = (b) => createHash("sha256").update(b).digest("hex");
// the raw sources: every .syx in the core trees (not their build/ or .git)
const syx = [];
for (const p of files(coresDir, new Set([".git", "build", "node_modules"]))) if (/\.syx$/i.test(p)) syx.push(p);
const hashes = new Set(), windows = [];
for (const p of syx) {
  const b = readFileSync(p);
  hashes.add(sha(b));
  // 24 windows of 32 bytes spread through the file, each with 12 or more different byte values (a run of zeros or of
  // a few values would match anything: a .DS_Store, a table)
  const want = 24, w = 32, tries = 512;
  let got = 0;
  for (let k = 0; k < tries && got < want && b.length >= w; k++) {
    const at = Math.floor(((b.length - w) * (k + 0.5)) / tries);
    const bytes = b.subarray(at, at + w);
    if (new Set(bytes).size < 12 || windows.some((x) => x.from === relative(coresDir, p) && Math.abs(x.at - at) < b.length / (want * 2))) continue;
    windows.push({ from: relative(coresDir, p), at, bytes });
    got++;
  }
}

let bad = 0, n = 0;
const fail = (m) => { bad++; console.error(`no_raw_presets: FAIL: ${m}`); };
for (const p of files(dir, new Set(), !top)) {
  n++;
  const rel = relative(dir, p);
  if (/\.syx$/i.test(rel) || /(^|\/)(cz1-factory|prophet5-factory)\//.test(rel)) fail(`${rel}: a raw preset source`);
  if (namesOnly || !syx.length) continue;
  if (statSync(p).size > 64 * 1024 * 1024) continue;
  const b = readFileSync(p);
  if (hashes.has(sha(b))) { fail(`${rel}: the content of a core's .syx`); continue; }
  for (const w of windows) if (b.indexOf(w.bytes) >= 0) { fail(`${rel}: raw bytes of ${w.from} (offset ${w.at})`); break; }
}
if (!syx.length && !namesOnly) fail(`no .syx under ${coresDir}: the content checks need FM1VST's core trees (or --names-only)`);
if (bad) process.exit(1);
console.log(`no_raw_presets: ${n} files in ${dir}: no .syx, no cz1-factory / prophet5-factory file` +
  (namesOnly ? "" : `, none of the ${syx.length} .syx files' content or ${windows.length} byte runs from them`));
