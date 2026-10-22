#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// A stand-in for `gh attestation verify <file> --repo R --signer-workflow W --format json` (the site's sync, tests):
// it prints gh's JSON (gh-attestation-verify.json, the shape gh 2.x prints: the in-toto statement and the signing
// certificate's GitHub extensions) for the file it is given, as a real attestation made by the reusable workflow
// would read: the file's SHA-256, the repository, tag and commit its manifest names. What it cannot do is the
// cryptography: a real tag's run is the only test of that. FM1_GH_MOCK picks a case:
//   ok          signed by W at refs/tags/native-v1, from R at the manifest's tag and commit (default)
//   branch-ref  signed by W at refs/heads/main (an untrusted ref of the workflow)
//   other-repo  built from another repository than R
//   other-file  an attestation for other bytes (its subject digest differs)
//   no-attest   gh fails as it does when there is none (exit 1)
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { readArchive } from "../../../engines/native/fm1web.mjs";

const a = process.argv.slice(2);
const opt = (k) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : null; };
if (a[0] !== "attestation" || a[1] !== "verify") { console.error("gh-mock: only `attestation verify`"); process.exit(2); }
const file = a[2], repo = opt("--repo"), workflow = opt("--signer-workflow");
const mode = process.env.FM1_GH_MOCK || "ok";
if (mode === "no-attest") {
    console.error(`Loaded digest sha256:... for file://${file}\nLoaded 0 attestations from GitHub API\n✗ No attestations found for subject`);
    process.exit(1);
}
const buf = fs.readFileSync(file);
const m = JSON.parse(readArchive(buf)["manifest.json"].toString("utf8"));
let sha = createHash("sha256").update(buf).digest("hex");
if (mode === "other-file") sha = "0".repeat(64);
const tpl = fs.readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), "gh-attestation-verify.json"), "utf8");
const fill = { NAME: path.basename(file), SHA256: sha, TAG: m.source.tag, REPO: repo, COMMIT: m.source.commit,
               SIGNER_WORKFLOW: workflow, SIGNER_REF: mode === "branch-ref" ? "refs/heads/main" : "refs/tags/native-v1",
               SOURCE_REPO: mode === "other-repo" ? "someone/else" : repo, OWNER: repo.split("/")[0] };
process.stdout.write(tpl.replace(/\{\{(\w+)\}\}/g, (_, k) => fill[k]));
