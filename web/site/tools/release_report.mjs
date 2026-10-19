// SPDX-License-Identifier: GPL-3.0-only
// For .github/workflows/pages.yml: what a built catalog says about the Hub's releases, and whether to deploy it.
//   node web/site/tools/release_report.mjs --catalog FILE [--site URL] [--commit SHA] [--event NAME]
//       the report: every native core behind the Hub (FM1VST's pinned version older than the Hub's) as a table in
//       $GITHUB_STEP_SUMMARY and a ::warning annotation each (readable through the public check-runs API); then
//       deploy=true|false in $GITHUB_OUTPUT. Only a scheduled run (--event schedule) skips: when the Hub was not
//       reachable (the catalog is the snapshot: nothing new is known, the deployed site stays), or when the site's
//       deployed.json (--site) names the same commit and the same catalog.
//   node web/site/tools/release_report.mjs --catalog FILE --stamp OUT --commit SHA
//       writes deployed.json: { commit, catalog: <hash>, generated }, published next to the page.
// The catalog's hash leaves out what changes without a release (the build date, the Hub's refresh time).
import { readFileSync, writeFileSync, appendFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { hubNewer } from "../js/live.js";

const arg = (n, d = null) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const catFile = arg("--catalog");
if (!catFile) { console.error("release_report: --catalog FILE"); process.exit(2); }
const cat = JSON.parse(readFileSync(catFile, "utf8"));
const commit = arg("--commit", process.env.GITHUB_SHA || "");

export function catalogHash(c) {
  const { generated, hubUpdated, ...rest } = c;
  return createHash("sha256").update(JSON.stringify(rest)).digest("hex");
}
const hash = catalogHash(cat);

const stamp = arg("--stamp");
if (stamp) {
  writeFileSync(stamp, JSON.stringify({ commit, catalog: hash, generated: cat.generated }, null, 1) + "\n");
  console.log(`deployed.json: commit ${commit.slice(0, 7)}, catalog ${hash.slice(0, 12)}`);
  process.exit(0);
}

const summary = (s) => { if (process.env.GITHUB_STEP_SUMMARY) appendFileSync(process.env.GITHUB_STEP_SUMMARY, s + "\n"); console.log(s); };
const output = (k, v) => { if (process.env.GITHUB_OUTPUT) appendFileSync(process.env.GITHUB_OUTPUT, `${k}=${v}\n`); };
const esc = (s) => String(s).replace(/%/g, "%25").replace(/\r/g, "").replace(/\n/g, "%0A");

// ---- native cores behind the Hub
const behind = cat.firmwares.filter((f) => f.native && f.engines.includes("emu") && hubNewer(f.version, f.native.version));
const live = cat.hubSource !== "snapshot";
summary(`## Firmware releases`);
summary(`The Hub's catalog: ${live ? "live" : "**not reachable, the snapshot in the tree**"} (updated ${cat.hubUpdated || "?"}). ` +
  `${cat.firmwares.length} firmwares, ${cat.firmwares.filter((f) => f.native).length} with a native core.`);
summary("");
if (behind.length) {
  summary(`### ${behind.length} native core${behind.length === 1 ? "" : "s"} behind the Hub`);
  summary("The site plays these emulated by default (the Hub's latest); native stays one click away. Bump them in FM1VST (web/vendor/FM1VST) to make native the default again.");
  summary("");
  summary("| firmware | id | native (FM1VST) | the Hub |");
  summary("|---|---|---|---|");
  for (const f of behind) {
    summary(`| ${f.name} | \`${f.id}\` | ${f.native.version} | ${f.version} |`);
    console.log(`::warning title=Native core behind the Hub: ${f.id}::${esc(`${f.id}: native ${f.native.version}, the Hub ${f.version}. The site plays the Hub's ${f.version} emulated by default.`)}`);
  }
} else summary("Every native core is the Hub's version.");
summary("");

// ---- deploy?
const event = arg("--event", process.env.GITHUB_EVENT_NAME || "");
let deploy = true, why = `a ${event || "manual"} run always deploys`;
if (event === "schedule") {
  if (!live) { deploy = false; why = "the Hub did not answer: nothing new is known, the deployed site stays (its page merges the Hub's catalog live anyway)"; }
  else {
    const site = arg("--site");
    let was = null;
    if (site) {
      try {
        const r = await fetch(new URL("deployed.json", site), { cache: "no-store", signal: AbortSignal.timeout(20000) });
        if (r.ok) was = await r.json();
      } catch (_) { was = null; }
    }
    if (!was) why = "the deployed site has no deployed.json (or did not answer): deploying";
    else if (was.commit !== commit) why = `the deployed site is commit ${String(was.commit).slice(0, 7)}, this one ${commit.slice(0, 7)}: deploying`;
    else if (was.catalog !== hash) why = "the Hub's catalog changed since the last deploy: deploying";
    else { deploy = false; why = "same commit, same catalog as the deployed site: nothing to deploy"; }
  }
}
summary(`**Deploy: ${deploy ? "yes" : "no"}**, ${why}.`);
output("deploy", deploy ? "true" : "false");
output("behind", String(behind.length));
