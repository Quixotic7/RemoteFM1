// SPDX-License-Identifier: GPL-3.0-only
// Firmware authors' own native builds (docs/NATIVE-AUTHORS.md), fetched and checked for the site
// (.github/workflows/pages.yml, before the catalog is built):
//
//   node web/site/tools/native_sync.mjs [--index FILE] [--mirror DIR] [--from DIR] [--gh PATH] [--hub FILE|URL]
//        [--work DIR] [--no-smoke]
//   --index   the result, read by build_catalog.mjs --native-ext (default web/build/native-ext/index.json)
//   --mirror  where the accepted builds go: DIR/<id>/<version>/<core>.wasm + manifest.json (the site's
//             engines/native/ext/; none: nothing copied)
//   --from    .fm1web files from a folder instead of the repositories' releases (tests; each file's repository is
//             its manifest's source.repo, which must still be one the list names)
//   --gh      the gh that verifies the attestations (default: gh on PATH; tests give a recorded one)
//   --hub     the Hub's catalog, for hubDiscovery (default its live catalog, else the snapshot in the tree)
//
// Candidates: each repository of data/native-authors.json "authors" ({ repo, id }), and with "hubDiscovery" on, each
// Hub firmware's repository; the .fm1web asset of its latest GitHub release. Each is accepted only when all hold:
//   1. its attestation: gh attestation verify <file> --repo <repo> --signer-workflow <workflow> passes, and the
//      signer is the workflow at one of "trustedRefs" (refs/tags/native-v1), the source is <repo> at a tag;
//   2. its manifest (web/engines/native/fm1web.mjs verifyArchive): the id the list gives, the repository, a commit
//      and tag equal to the attestation's, the module's SHA-256 and size as the manifest says;
//   3. its module's imports: exactly the functions web/engines/native/engine.js provides (PROVIDED_IMPORTS), nothing
//      else; the exports the engine calls; its Asyncify flag true to the module;
//   4. the size: the file and the module at most "maxBytes" (4 MiB);
//   5. the licensing gate: not a firmware data/cores.json marks gated (unless "ungated" names it), never one of
//      "never";
//   6. the smoke test: web/engines/native/test.mjs on the module with the build's own conformance script (it boots,
//      draws, sounds, survives 10 s of rendering), in a child process with a time limit.
// The index lists every candidate (so a new release changes it), the accepted builds and the rejected ones, why.
import fs from "node:fs";
import path from "node:path";
import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { verifyArchive, MAX_BYTES, WORKFLOW } from "../../engines/native/fm1web.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url)), SITE = path.join(HERE, ".."), WEB = path.join(SITE, "..");
const sha256 = (b) => createHash("sha256").update(b).digest("hex");

// ------------------------------------------------------------------------------------------------- the checks ---
// gh attestation verify, its JSON: the signer and the source of the first result that passes our rules
export function attestationOf(results, { repo, workflow = WORKFLOW, trustedRefs }) {
    const want = `https://github.com/${repo}`.toLowerCase();
    const why = [];
    for (const r of Array.isArray(results) ? results : []) {
        const c = r?.verificationResult?.signature?.certificate || {};
        const signer = String(c.buildSignerURI || "");
        const ref = trustedRefs.find((t) => signer === `https://github.com/${workflow}@${t}`);
        if (!ref) { why.push(`signed by ${signer || "an unknown workflow"}, not ${workflow} at ${trustedRefs.join(" or ")}`); continue; }
        if (String(c.sourceRepositoryURI || "").toLowerCase() !== want) { why.push(`built from ${c.sourceRepositoryURI}, not ${want}`); continue; }
        if (!/^refs\/tags\/./.test(String(c.sourceRepositoryRef || ""))) { why.push(`built from ${c.sourceRepositoryRef}, not a tag`); continue; }
        if (!/^[0-9a-f]{40}$/.test(String(c.sourceRepositoryDigest || ""))) { why.push("no source commit"); continue; }
        return { ok: true, signer, signerRef: ref, commit: c.sourceRepositoryDigest, tag: c.sourceRepositoryRef.replace(/^refs\/tags\//, ""),
                 run: c.runInvocationURI || null, subject: r?.verificationResult?.statement?.subject || null };
    }
    return { ok: false, why: why.length ? why.join("; ") : "no attestation result" };
}

// the gate: { ok, why }; cores = data/cores.json's list, cfg = data/native-authors.json
export function gateOf(m, cores, cfg) {
    for (const k of [m.id, m.core]) if (cfg.never && cfg.never[k]) return { ok: false, why: `${k} never runs native here: ${cfg.never[k]}` };
    for (const c of cores) {
        if (!c.gated) continue;
        const ids = [c.core, c.hub].filter(Boolean);
        const hit = [m.id, m.core].find((k) => ids.includes(k));
        if (hit && !(cfg.ungated && (cfg.ungated[c.core] || (c.hub && cfg.ungated[c.hub]))))
            return { ok: false, why: `${hit} is gated (${c.gated}): FM1VST CORES-PLAN section 6` };
    }
    return { ok: true };
}

const run = (cmd, args, { timeout = 120000 } = {}) => new Promise((res) => {
    execFile(cmd, args, { timeout, maxBuffer: 32 * 1024 * 1024, env: process.env }, (err, stdout, stderr) =>
        res({ code: err ? (typeof err.code === "number" ? err.code : 1) : 0, stdout: String(stdout), stderr: String(stderr), killed: !!(err && err.killed) }));
});

// ---------------------------------------------------------------------------------------------- the candidates ---
const token = () => process.env.GH_TOKEN || process.env.GITHUB_TOKEN || "";
async function gh(url) {
    const r = await fetch(url, { headers: { accept: "application/vnd.github+json", ...(token() ? { authorization: `Bearer ${token()}` } : {}) },
                               signal: AbortSignal.timeout(30000) });
    if (r.status === 404) return null;
    if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
    return r.json();
}
async function latestRelease(repo) {
    const rel = await gh(`https://api.github.com/repos/${repo}/releases/latest`);
    if (!rel) return [];
    return (rel.assets || []).filter((a) => a.name.endsWith(".fm1web"))
        .map((a) => ({ tag: rel.tag_name, asset: a.name, url: a.browser_download_url, size: a.size, updated: a.updated_at,
                       digest: a.digest || null }));
}

export async function sync(o) {
    const cfg = JSON.parse(fs.readFileSync(o.config || path.join(SITE, "data", "native-authors.json"), "utf8"));
    const cores = JSON.parse(fs.readFileSync(path.join(SITE, "data", "cores.json"), "utf8")).cores;
    const maxBytes = cfg.maxBytes || MAX_BYTES, workflow = cfg.workflow || WORKFLOW, trustedRefs = cfg.trustedRefs || [];
    const work = o.work || path.join(WEB, "build", "native-ext", "work");
    fs.mkdirSync(work, { recursive: true });
    const log = o.log || console.log;

    // who to look for: repo (lower case) -> { repo, id, from }
    const wanted = new Map();
    const fine = (w) => /^[a-z0-9][a-z0-9_-]{0,31}$/.test(String(w.id)) && /^[A-Za-z0-9-]+\/[A-Za-z0-9._-]+$/.test(String(w.repo));
    for (const a of cfg.authors || []) {
        if (fine(a)) wanted.set(a.repo.toLowerCase(), { repo: a.repo, id: a.id, from: "list" });
        else log(`native_sync: the list's entry ${JSON.stringify(a)}: an id is a-z 0-9 _ -, a repo owner/name`);
    }
    if (cfg.hubDiscovery) {
        let hub = null;
        try {
            hub = /^https?:/.test(o.hub || "") ? await (await fetch(o.hub, { signal: AbortSignal.timeout(30000) })).json()
                : JSON.parse(fs.readFileSync(o.hub || path.join(SITE, "data", "hub-catalog.snapshot.json"), "utf8"));
        } catch (e) { log(`native_sync: the Hub's catalog: ${e.message}`); }
        for (const f of hub?.firmwares || [])
            if (f.repo && fine(f) && !wanted.has(f.repo.toLowerCase())) wanted.set(f.repo.toLowerCase(), { repo: f.repo, id: f.id, from: "hub" });
    }

    // the candidates: their release assets (or local files)
    const cands = [];
    if (o.from) {
        for (const n of fs.readdirSync(o.from).filter((n) => n.endsWith(".fm1web")).sort()) {
            const file = path.join(o.from, n), buf = fs.readFileSync(file);
            const v = verifyArchive(buf, { maxBytes });
            const repo = v.manifest?.source?.repo || "";
            const w = wanted.get(repo.toLowerCase());
            if (!w) { log(`native_sync: ${n}: ${repo || "no repository"} is not on the list: skipped`); continue; }
            cands.push({ ...w, tag: v.manifest?.source?.tag || null, asset: n, file, size: buf.length, sha256: sha256(buf) });
        }
    } else {
        for (const w of wanted.values()) {
            try {
                const assets = await latestRelease(w.repo);
                const a = assets.find((x) => x.asset.startsWith(`${w.id}-`)) || assets[0];
                if (a) cands.push({ ...w, ...a });
                else if (w.from === "list") log(`native_sync: ${w.repo}: no .fm1web in its latest release`);
            } catch (e) { log(`native_sync: ${w.repo}: ${e.message}`); }
        }
    }

    const accepted = [], rejected = [];
    const reject = (c, why) => { rejected.push({ repo: c.repo, id: c.id, tag: c.tag, asset: c.asset, why }); log(`  REJECTED ${c.repo} ${c.asset}: ${why}`); };
    for (const c of cands) {
        log(`== ${c.repo} ${c.tag || ""} ${c.asset} (${c.id}, ${c.from})`);
        // 0. the file
        let file = c.file, buf;
        if (!file) {
            if (c.size > maxBytes) { reject(c, `${c.size} bytes, over the ${maxBytes} cap`); continue; }
            try {
                const r = await fetch(c.url, { signal: AbortSignal.timeout(60000) });
                if (!r.ok) throw new Error(`HTTP ${r.status}`);
                buf = Buffer.from(await r.arrayBuffer());
            } catch (e) { reject(c, `the download: ${e.message}`); continue; }
            file = path.join(work, `${c.id}.fm1web`);
            fs.writeFileSync(file, buf);
        } else buf = fs.readFileSync(file);
        c.sha256 = sha256(buf);
        if (c.digest && c.digest !== `sha256:${c.sha256}`) { reject(c, `its SHA-256 is not the release's (${c.digest})`); continue; }
        if (buf.length > maxBytes) { reject(c, `${buf.length} bytes, over the ${maxBytes} cap`); continue; }
        // 1. the attestation
        const r = await run(o.gh || "gh", ["attestation", "verify", file, "--repo", c.repo, "--signer-workflow", workflow, "--format", "json"]);
        if (r.code !== 0) { reject(c, `gh attestation verify: ${(r.stderr || r.stdout).trim().split("\n").slice(-3).join(" ")}`); continue; }
        let att;
        try { att = attestationOf(JSON.parse(r.stdout), { repo: c.repo, workflow, trustedRefs }); } catch (e) { att = { ok: false, why: `gh's output: ${e.message}` }; }
        if (!att.ok) { reject(c, `the attestation: ${att.why}`); continue; }
        if (att.subject && !att.subject.some((s) => s?.digest?.sha256 === c.sha256)) { reject(c, "the attestation is not for this file"); continue; }
        // 2-4. the manifest, the module, the size
        const v = verifyArchive(buf, { repo: c.repo, id: c.id, maxBytes });
        if (!v.ok) { reject(c, v.problems.join("; ")); continue; }
        const m = v.manifest;
        if (m.source.commit !== att.commit) { reject(c, `the manifest's commit ${m.source.commit} is not the attested ${att.commit}`); continue; }
        if (m.source.tag !== att.tag) { reject(c, `the manifest's tag ${m.source.tag} is not the attested ${att.tag}`); continue; }
        if (c.tag && c.tag !== att.tag) { reject(c, `built from ${att.tag}, attached to the release ${c.tag}`); continue; }
        // 5. the gate
        const g = gateOf(m, cores, cfg);
        if (!g.ok) { reject(c, g.why); continue; }
        // 6. the smoke test
        const dir = path.join(work, c.id);
        fs.rmSync(dir, { recursive: true, force: true });
        fs.mkdirSync(dir, { recursive: true });
        for (const [n, b] of Object.entries(v.files)) fs.writeFileSync(path.join(dir, n), b);
        let smoke = "skipped";
        if (!o.noSmoke) {
            const args = [path.join(WEB, "engines", "native", "test.mjs"), "--wasm", path.join(dir, m.wasm.file), "--id", m.core,
                          "--out", path.join(dir, "test"), "--json", path.join(dir, "test.json")];
            if (v.files["conformance.txt"]) args.push("--conformance", path.join(dir, "conformance.txt"));
            const t = await run(process.execPath, args, { timeout: 180000 });
            let rep = null;
            try { rep = JSON.parse(fs.readFileSync(path.join(dir, "test.json"), "utf8")); } catch (_) { /* (none: it failed early) */ }
            if (t.code !== 0 || !rep || rep.fails) {
                const lines = rep ? rep.cores[0].checks.filter((l) => l.startsWith("FAIL")) : [t.killed ? "over 180 s" : (t.stderr || t.stdout).trim().split("\n").slice(-2).join(" ")];
                reject(c, `the smoke test: ${lines.join("; ")}`);
                continue;
            }
            smoke = `passed (${rep.cores[0].checks.filter((l) => l.startsWith("ok")).length} checks)`;
        }
        log(`  accepted: ${m.name} ${m.version}, ${m.family}, ${m.source.repo}@${m.source.commit.slice(0, 7)}, signed by ${att.signer.replace(/^https:\/\/github.com\//, "")}, smoke test ${smoke}`);
        const rel = `${m.id}/${m.version}/${m.wasm.file}`;
        if (o.mirror) {
            const to = path.join(o.mirror, m.id, m.version);
            fs.mkdirSync(to, { recursive: true });
            fs.writeFileSync(path.join(to, m.wasm.file), v.files[m.wasm.file]);
            fs.writeFileSync(path.join(to, "manifest.json"), v.files["manifest.json"]);
        }
        accepted.push({
            id: m.id, core: m.core, name: m.name, version: m.version, family: m.family, glue: m.glue, wasm: rel,
            sha256: m.wasm.sha256, bytes: m.wasm.bytes, asyncify: m.asyncify, abi: m.abi, license: m.license,
            credits: m.credits, usbProduct: m.usbProduct, editor: m.editor, fm1vst: m.fm1vst.commit,
            repo: `https://github.com/${m.source.repo}`, tag: m.source.tag, commit: m.source.commit,
            buildLog: att.run || m.built?.run || null, signer: att.signer, from: c.from, smoke,
        });
    }
    const index = {
        about: "web/site/tools/native_sync.mjs: firmware authors' own native builds, checked (docs/NATIVE-AUTHORS.md). build_catalog.mjs --native-ext reads it.",
        workflow, trustedRefs,
        candidates: cands.map((c) => ({ repo: c.repo, id: c.id, tag: c.tag, asset: c.asset, sha256: c.sha256 || null })),
        accepted, rejected,
    };
    if (o.index) { fs.mkdirSync(path.dirname(o.index), { recursive: true }); fs.writeFileSync(o.index, JSON.stringify(index, null, 1) + "\n"); }
    log(`native_sync: ${cands.length} candidate(s), ${accepted.length} accepted, ${rejected.length} rejected${o.index ? ` -> ${o.index}` : ""}`);
    return index;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const arg = (k, d = null) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
    const mirror = arg("--mirror");
    await sync({ index: arg("--index", path.join(WEB, "build", "native-ext", "index.json")), mirror: mirror === "none" ? null : mirror,
                 from: arg("--from"), gh: arg("--gh"), hub: arg("--hub", "https://thefirmwarehub.com/catalog.json"), work: arg("--work"),
                 config: arg("--config"), noSmoke: process.argv.includes("--no-smoke") });
}
