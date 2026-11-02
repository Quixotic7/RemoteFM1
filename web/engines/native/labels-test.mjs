// SPDX-License-Identifier: GPL-3.0-only
// Live panel labels in the native engine (FM1VST docs/DYNAMIC-LABELS.md; API.md engine.labels()): the .rfm1 test's
// core (../rfm1/fixtures/labels_core.c) as a core library linked with wasm_glue.c the way build.sh links one (its
// LDFLAGS: -u pulls the symbols out of the archive), with and without the symbols; and every module already in
// web/build/native: labels() null unless its core has them.
//   FM1VST_DIR=<FM1VST with the FM1CORE_LBL_* defines> node web/engines/native/labels-test.mjs
// Needs emcc (on PATH, or ~/emsdk); without it, or without FM1VST_DIR's core-api, only the built modules are checked.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createEngine } from "./engine.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WEB = path.resolve(HERE, "../..");
const API = path.join(process.env.FM1VST_DIR || path.join(WEB, "vendor/FM1VST"), "core-api");
let fails = 0, n = 0;
const ok = (c, what) => { n++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return c; };
const L = new Float32Array(128), R = new Float32Array(128);
const mk = (wasm) => createEngine({ sampleRate: 44100, files: { wasm }, flash: null, options: {} });
const panel = (buttons) => ({ keys: 0, buttons, enc: new Int32Array(7), master: 768, keyVel: new Uint8Array(27) });
const run = (e, ms) => { const t = e.deviceMs() + ms; while (e.deviceMs() < t) e.render(L, R); };

const emcc = (() => {
    try { execFileSync("emcc", ["--version"], { stdio: "ignore" }); return "emcc"; } catch (_) {}
    const p = path.join(process.env.EMSDK || path.join(os.homedir(), "emsdk"), "upstream/emscripten/emcc");
    return fs.existsSync(p) ? p : null;
})();
const haveApi = fs.existsSync(path.join(API, "fm1core.h")) && fs.readFileSync(path.join(API, "fm1core.h"), "utf8").includes("FM1CORE_LBL_N");
if (emcc && haveApi) {
    console.log("== labels_core.c linked with wasm_glue.c (build.sh's flags)");
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "fm1-labels-"));
    const sh = fs.readFileSync(path.join(HERE, "build.sh"), "utf8");
    const ld = ["-Os", "-sSTANDALONE_WASM", "--no-entry", "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=33554432", "-sSTACK_SIZE=1048576",
        "-sERROR_ON_UNDEFINED_SYMBOLS=1", "-Wl,-u,fm1core_labels_epoch", "-Wl,-u,fm1core_label"];
    ok(sh.includes("-Wl,-u,fm1core_labels_epoch -Wl,-u,fm1core_label"), "build.sh links with -u for both symbols");
    const build = (name, defs) => {
        const o = path.join(tmp, `${name}.o`), a = path.join(tmp, `lib${name}.a`), w = path.join(tmp, `${name}.wasm`);
        execFileSync(emcc, ["-Os", ...defs, `-I${API}`, "-c", path.join(HERE, "../rfm1/fixtures/labels_core.c"), "-o", o]);
        execFileSync(emcc.replace(/emcc$/, "emar"), ["rcs", a, o]);
        execFileSync(emcc, [...ld, `-I${API}`, path.join(HERE, "wasm_glue.c"), a, "-o", w]);
        return fs.readFileSync(w);
    };
    const e = await mk(build("labels", []));
    let l = e.labels();
    ok(l && l.seq === 1 && l.list[8] === "DRUMS" && l.list[9] === "" && l.list[13] === null && l.list[3] === "CUTOFF" && l.list[22] === "STEP 1" && l.list[38] === "MUTE",
       "after boot: epoch 1; FX DRUMS, SEL \"\", GLO not given, KNOB 1 CUTOFF, W1 STEP 1, B1 MUTE");
    e.setPanel(panel(1 << 7));
    run(e, 40);
    l = e.labels();
    ok(l.seq === 2 && l.buttons[0] === "UNDO" && l.buttons[5] === "SHIFT" && l.white[0] === "" && l.black[0] === null, "GLO held: epoch 2, FX UNDO, GLO SHIFT, W1 \"\"");
    ok(e.info().buttonNames[0] === "KICK", "info().buttonNames as before");
    e.dispose();
    const z = await mk(build("nolabels", ["-DNOLABELS"]));
    run(z, 50);
    ok(z.labels() === null, "the core without the symbols (weak: links): labels() null");
    z.dispose();
    fs.renameSync(tmp, `${tmp}.done`);
} else console.log(`== (no ${emcc ? "FM1VST core-api with FM1CORE_LBL_N at " + API : "emcc"}: the link test skipped)`);

const dir = path.join(WEB, "build/native");
const built = fs.existsSync(dir) ? fs.readdirSync(dir).filter((f) => f.endsWith(".wasm")) : [];
if (built.length) console.log("== web/build/native");
for (const f of built) {
    const e = await mk(fs.readFileSync(path.join(dir, f)));
    run(e, 100);
    const l = e.labels();
    ok(l === null || (l.list.length === 49 && l.seq > 0), `${f}: ${l ? `live labels (epoch ${l.seq})` : "labels() null"}`);
    e.dispose();
}

console.log(fails ? `labels: ${fails} of ${n} FAILED` : `labels tests passed (${n})`);
process.exit(fails ? 1 : 0);
