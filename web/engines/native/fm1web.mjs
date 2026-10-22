// SPDX-License-Identifier: GPL-3.0-only
// An author's native build as one file, <id>-<version>.fm1web: a gzipped tar (ustar) of
//   manifest.json     what it is (below)
//   <core>.wasm       the module (web/engines/native/build.sh --ext): what the site's native engine runs
//   conformance.txt   the conformance script the build was tested with (test.mjs: the sound prelude, the save and
//                     reboot scripts), so the site's own smoke test runs the same one
// Made by the reusable workflow .github/workflows/fm1-native.yml (.github/scripts/fm1-native.sh), checked by the
// site's sync (web/site/tools/native_sync.mjs) before it is published. Node only.
//
//   node web/engines/native/fm1web.mjs pack --build DIR --report FILE --conformance FILE --out DIR
//        [--id ID] [--repo OWNER/REPO] [--tag TAG] [--run URL] [--usb-product NAME] [--editor URL] [--hub FILE|URL]
//        DIR is build.sh --ext's output (meta.json, <core>.wasm), the report test.mjs --json's; the Hub's catalog
//        gives the USB product name and the editor when they are not given. Prints the file's path.
//   node web/engines/native/fm1web.mjs verify FILE [--repo OWNER/REPO] [--id ID] [--max-bytes N] [--extract DIR]
//        the checks the site makes on the file itself (no attestation, no smoke test): the manifest, the module's
//        SHA-256, its imports against the engine's, its size. Prints the result as JSON; exit 1 when rejected.
//
// manifest.json (format "fm1web/1"):
//   id, core          the site's id for the firmware (the Hub's) and the core's own id (its descriptor's)
//   name, version     the firmware's name, and its version (the release's)
//   family, glue      the FM1VST glue family it is built on ("custom": the author's own glue) and the glue folder
//   abi               the core ABI (6)
//   fm1vst            { repo, commit }: the FM1VST the glue and the core ABI came from
//   source            { repo, url, tag, commit }: the author's repository at the release
//   wasm              { file, sha256, bytes }
//   asyncify          the module runs on the coroutine (Binaryen's Asyncify; engine.js's fm1w_co_* imports)
//   usbProduct        the USB product name the firmware answers with (the Hub's "product"), or null
//   editor            the firmware's own web editor, or null
//   license, credits  the descriptor's SPDX licence and its credits text
//   checks            { passed, lines, firstPicture, peak, realtime }: the conformance test's results
//   built             { by: "fm1-native", workflow, run, at }: the workflow run that made it (its log)
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { moduleProblems } from "./engine.js";

export const FORMAT = "fm1web/1";
export const FAMILIES = ["felucca_web", "fm1base", "sloop", "dinghy", "choralroot", "plat", "melodee", "custom"];
export const MAX_BYTES = 4 * 1024 * 1024;
export const WORKFLOW = "Quixotic7/RemoteFM1/.github/workflows/fm1-native.yml";
const ID_RE = /^[a-z0-9][a-z0-9_-]{0,31}$/;
const VER_RE = /^[0-9A-Za-z][0-9A-Za-z._+-]{0,39}$/;
const sha256 = (b) => createHash("sha256").update(b).digest("hex");

// ------------------------------------------------------------------------------------------------ the archive ---
// ustar, regular files only, flat names (no folders): what pack writes and all that read accepts
function header(name, size) {
    const h = Buffer.alloc(512);
    h.write(name, 0, 100, "utf8");
    h.write("0000644\0", 100); h.write("0000000\0", 108); h.write("0000000\0", 116);
    h.write(size.toString(8).padStart(11, "0") + "\0", 124);
    h.write("00000000000\0", 136);                             // (mtime 0: the same bytes for the same build)
    h.write("        ", 148);
    h[156] = 0x30;                                             // "0": a regular file
    h.write("ustar\0", 257); h.write("00", 263);
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += h[i];
    h.write(sum.toString(8).padStart(6, "0") + "\0 ", 148);
    return h;
}
export function writeArchive(files /* [[name, Buffer]] */) {
    const parts = [];
    for (const [name, data] of files) {
        parts.push(header(name, data.length), data, Buffer.alloc((512 - (data.length % 512)) % 512));
    }
    parts.push(Buffer.alloc(1024));
    return zlib.gzipSync(Buffer.concat(parts), { level: 9 });
}
// { name: Buffer } from an archive; throws on anything but flat regular files within the limits
export function readArchive(gz, { maxBytes = MAX_BYTES, maxFiles = 8 } = {}) {
    let tar;
    try { tar = zlib.gunzipSync(gz, { maxOutputLength: maxBytes + 64 * 1024 }); } catch (e) { throw new Error(`not a gzipped archive, or over ${maxBytes} bytes unpacked (${e.code || e.message})`); }
    const out = {};
    for (let off = 0; off + 512 <= tar.length;) {
        const h = tar.subarray(off, off + 512);
        if (h.every((b) => b === 0)) break;
        const name = h.subarray(0, 100).toString("utf8").replace(/\0.*$/s, "");
        const size = parseInt(h.subarray(124, 136).toString("ascii").replace(/\0.*$/s, "").trim() || "0", 8);
        const type = h[156];
        if (type !== 0x30 && type !== 0) throw new Error(`${name}: not a regular file (type ${String.fromCharCode(type)})`);
        if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(name)) throw new Error(`a file named ${JSON.stringify(name)} (flat names only)`);
        if (!Number.isFinite(size) || size < 0 || off + 512 + size > tar.length) throw new Error(`${name}: a broken header`);
        if (out[name]) throw new Error(`${name} twice`);
        out[name] = Buffer.from(tar.subarray(off + 512, off + 512 + size));
        if (Object.keys(out).length > maxFiles) throw new Error(`more than ${maxFiles} files`);
        off += 512 + Math.ceil(size / 512) * 512;
    }
    return out;
}

// --------------------------------------------------------------------------------------------- verification ---
// The checks on the file itself. expect: { repo, id, maxBytes }. Returns { ok, problems, manifest, files }.
export function verifyArchive(buf, { repo = null, id = null, maxBytes = MAX_BYTES } = {}) {
    const problems = [];
    const no = (what) => { problems.push(what); };
    if (buf.length > maxBytes) no(`the file is ${buf.length} bytes, over the ${maxBytes} cap`);
    let files = {};
    try { files = readArchive(buf, { maxBytes }); } catch (e) { no(`the archive: ${e.message}`); return { ok: false, problems, manifest: null, files }; }
    let m = null;
    try { m = JSON.parse(files["manifest.json"].toString("utf8")); } catch (_) { no("no readable manifest.json"); return { ok: false, problems, manifest: null, files }; }
    if (m.format !== FORMAT) no(`format ${JSON.stringify(m.format)}, not ${FORMAT}`);
    if (!ID_RE.test(String(m.id))) no(`id ${JSON.stringify(m.id)}: a-z 0-9 _ -, 1 to 32`);
    if (!ID_RE.test(String(m.core))) no(`core ${JSON.stringify(m.core)}: a-z 0-9 _ -, 1 to 32`);
    if (id && m.id !== id) no(`id ${m.id}, the list says ${id}`);
    if (!VER_RE.test(String(m.version))) no(`version ${JSON.stringify(m.version)}`);
    if (!FAMILIES.includes(m.family)) no(`family ${JSON.stringify(m.family)}`);
    if (m.abi !== 6) no(`ABI ${m.abi}, not 6`);
    if (typeof m.name !== "string" || !m.name || m.name.length > 64) no("no name (or over 64 characters)");
    if (typeof m.asyncify !== "boolean") no("asyncify is not true or false");
    const src = m.source || {};
    if (!/^[A-Za-z0-9-]+\/[A-Za-z0-9._-]+$/.test(String(src.repo))) no(`source.repo ${JSON.stringify(src.repo)}`);
    if (repo && String(src.repo).toLowerCase() !== repo.toLowerCase()) no(`source.repo ${src.repo}, not ${repo}`);
    if (!/^[0-9a-f]{40}$/.test(String(src.commit))) no(`source.commit ${JSON.stringify(src.commit)}`);
    if (!m.checks || m.checks.passed !== true) no("its conformance checks did not pass");
    for (const k of ["editor"]) if (m[k] != null && !/^https:\/\/[^\s"<>]+$/.test(String(m[k]))) no(`${k} is not an https URL`);
    const w = m.wasm || {};
    const wasm = files[w.file];
    if (!wasm || w.file !== `${m.core}.wasm`) no(`no ${m.core}.wasm in the archive (manifest: ${w.file})`);
    else {
        const h = sha256(wasm);
        if (h !== w.sha256) no(`${w.file}: SHA-256 ${h}, the manifest says ${w.sha256}`);
        if (wasm.length !== w.bytes) no(`${w.file}: ${wasm.length} bytes, the manifest says ${w.bytes}`);
        if (wasm.length > maxBytes) no(`${w.file} is ${wasm.length} bytes, over the ${maxBytes} cap`);
        let mod = null;
        try { mod = new WebAssembly.Module(wasm); } catch (e) { no(`${w.file} is not a valid module: ${e.message}`); }
        if (mod) for (const p of moduleProblems(mod, { asyncify: m.asyncify })) no(`${w.file} ${p}`);
    }
    for (const n of Object.keys(files)) if (!["manifest.json", "conformance.txt", `${m.core}.wasm`].includes(n)) no(`an unexpected file, ${n}`);
    return { ok: problems.length === 0, problems, manifest: m, files };
}

// ------------------------------------------------------------------------------------------------------ pack ---
async function hubEntry(hub, id) {
    if (!hub) return null;
    try {
        const cat = /^https?:/.test(hub) ? await (await fetch(hub, { signal: AbortSignal.timeout(20000) })).json() : JSON.parse(fs.readFileSync(hub, "utf8"));
        return (cat.firmwares || []).find((f) => f.id === id) || null;
    } catch (e) { console.error(`fm1web: the Hub's catalog (${hub}): ${e.message}; no USB product name or editor from it`); return null; }
}
export async function pack(o) {
    const meta = JSON.parse(fs.readFileSync(path.join(o.build, "meta.json"), "utf8"));
    const report = JSON.parse(fs.readFileSync(o.report, "utf8"));
    const r = report.cores[0], info = r.info || {};
    const wasm = fs.readFileSync(path.join(o.build, meta.wasm));
    const id = o.id || meta.id;
    const hub = await hubEntry(o.hub, id);
    const version = meta.version;
    const m = {
        format: FORMAT,
        id,
        core: meta.id,
        name: info.name || hub?.name || id,
        version,
        family: meta.family,
        glue: meta.glue.startsWith("core-glue/ext_") ? "author" : meta.glue,
        abi: 6,
        fm1vst: { repo: "https://github.com/Quixotic7/FM1VST", commit: meta.fm1vst },
        source: { repo: o.repo || null, url: o.repo ? `https://github.com/${o.repo}` : null, tag: o.tag || null, commit: meta.commit },
        wasm: { file: meta.wasm, sha256: sha256(wasm), bytes: wasm.length },
        asyncify: !!meta.asyncify,
        usbProduct: o.usbProduct || hub?.product || null,
        editor: o.editor || hub?.editor || null,
        license: info.license || null,
        credits: info.credits || null,
        gated: null,
        checks: { passed: report.fails === 0, lines: r.checks, firstPicture: r.ref || null, peak: r.peak ?? null,
                  realtime: r.rtf ? Math.round(r.rtf) : null },
        built: { by: "fm1-native", workflow: WORKFLOW, run: o.run || null, at: new Date().toISOString() },
    };
    const files = [["manifest.json", Buffer.from(JSON.stringify(m, null, 1) + "\n")], [meta.wasm, wasm]];
    if (o.conformance && fs.existsSync(o.conformance)) files.push(["conformance.txt", fs.readFileSync(o.conformance)]);
    fs.mkdirSync(o.out, { recursive: true });
    const file = path.join(o.out, `${id}-${version}.fm1web`);
    fs.writeFileSync(file, writeArchive(files));
    return { file, manifest: m };
}

// ------------------------------------------------------------------------------------------------------- CLI ---
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const [cmd, ...rest] = process.argv.slice(2);
    const arg = (k, d = null) => { const i = rest.indexOf(k); return i >= 0 ? rest[i + 1] : d; };
    if (cmd === "pack") {
        const o = { build: arg("--build"), report: arg("--report"), conformance: arg("--conformance"), out: arg("--out"),
                    id: arg("--id"), repo: arg("--repo"), tag: arg("--tag"), run: arg("--run"),
                    usbProduct: arg("--usb-product"), editor: arg("--editor"), hub: arg("--hub") };
        if (!o.build || !o.report || !o.out) { console.error("fm1web pack --build DIR --report FILE --out DIR ..."); process.exit(2); }
        const { file, manifest } = await pack(o);
        const v = verifyArchive(fs.readFileSync(file), { repo: o.repo, id: manifest.id });
        if (!v.ok) { console.error(`fm1web: the package fails its own checks:\n  ${v.problems.join("\n  ")}`); process.exit(1); }
        console.log(file);
    } else if (cmd === "verify") {
        const file = rest[0];
        const v = verifyArchive(fs.readFileSync(file), { repo: arg("--repo"), id: arg("--id"), maxBytes: Number(arg("--max-bytes", MAX_BYTES)) });
        const dir = arg("--extract");
        if (dir && v.ok) { fs.mkdirSync(dir, { recursive: true }); for (const [n, b] of Object.entries(v.files)) fs.writeFileSync(path.join(dir, n), b); }
        console.log(JSON.stringify({ ok: v.ok, problems: v.problems, manifest: v.manifest }, null, 1));
        process.exit(v.ok ? 0 : 1);
    } else { console.error("fm1web.mjs pack|verify (see its header)"); process.exit(2); }
}
