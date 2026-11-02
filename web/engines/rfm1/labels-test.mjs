// SPDX-License-Identifier: GPL-3.0-only
// Live panel labels in the .rfm1 engine (FM1VST docs/DYNAMIC-LABELS.md; API.md engine.labels()), end to end: a real
// .rfm1 built with FM1VST's rfm1_shim.c (fixtures/labels_core.c, build_labels.sh; committed) whose labels change while
// GLO is held, through render() and the worklet's LabelWatch into the page's merge; the same core without the
// symbols (the shim's exports, epoch 0) and an .rfm1 built before the exports: labels() null, the info as before.
//   node web/engines/rfm1/labels-test.mjs [--old FILE.rfm1 ...]   (more pre-label .rfm1 files to check)
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createEngine, inspect } from "./engine.js";
import { LabelWatch } from "../../site/worklet/host.js";
import { panelLabels } from "../../site/js/panel.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const fx = (n) => new Uint8Array(fs.readFileSync(path.join(HERE, "fixtures", n)));
const olds = [];
for (let i = process.argv.indexOf("--old"); i >= 0 && i + 1 < process.argv.length && !process.argv[i + 1].startsWith("--"); i++) olds.push(process.argv[i + 1]);
let fails = 0, n = 0;
const ok = (c, what) => { n++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return c; };
const mk = (bytes) => createEngine({ sampleRate: 44100, files: { wasm: bytes }, flash: null, options: {} });
const L = new Float32Array(128), R = new Float32Array(128);
const GLO = 7;   // the matrix button of the GLO label (panel.js labelToMatrix(5))
const panel = (buttons) => ({ keys: 0, buttons, enc: new Int32Array(7), master: 768, keyVel: new Uint8Array(27) });
const run = (e, ms) => { const t = e.deviceMs() + ms; while (e.deviceMs() < t) e.render(L, R); };

console.log("== labels_core.rfm1: live labels through the shim");
{
    const bytes = fx("labels_core.rfm1");
    ok(inspect(bytes).labels === true, "inspect: rfm1_labels_epoch and rfm1_label exported, (i32) signatures");
    const e = await mk(bytes), w = new LabelWatch(), info = e.info();
    ok(info.buttonNames[0] === "KICK" && info.buttonNames[1] === "TRK" && info.buttonNames[2] === "", "info().buttonNames: the static button_names as before");
    let l = e.labels();
    ok(l && l.seq === 1 && l.list.length === 49 && l.list[8] === "DRUMS" && l.list[9] === "" && l.list[13] === null && l.list[3] === "CUTOFF"
       && l.list[22] === "STEP 1" && l.list[38] === "MUTE" && l.list[0] === null, "after boot: epoch 1; FX DRUMS, SEL \"\" (no job), GLO not given, KNOB 1 CUTOFF, W1, B1");
    ok(l.buttons[0] === "DRUMS" && l.knobs[3] === "CUTOFF" && l.white[0] === "STEP 1" && l.black[0] === "MUTE", "the slices: knobs, buttons, white, black");
    const m1 = w.take(e);
    ok(m1 && m1.seq === 1 && w.take(e) === null, "the worklet's watch: one message, none while the epoch stays");
    let pl = panelLabels(info.buttonNames, m1.list);
    ok(pl.buttons[0] === "DRUMS" && pl.buttons[1] === "TRK" && pl.btnOff[1] && pl.buttons[5] === "GLO" && pl.keys[0] === "STEP 1" && pl.keys[1] === "MUTE",
       "the page's merge: FX DRUMS, SEL TRK dim (button_names), GLO printed, F3 STEP 1, F#3 MUTE");
    e.setPanel(panel(1 << GLO));
    run(e, 40);
    l = e.labels();
    const m2 = w.take(e);
    ok(l.seq === 2 && l.list[8] === "UNDO" && l.list[9] === "COPY" && l.list[13] === "SHIFT" && l.list[3] === "RES" && l.list[22] === "" && l.list[38] === null,
       "GLO held: epoch 2; FX UNDO, SEL COPY, GLO SHIFT, KNOB 1 RES, W1 \"\", B1 not given");
    pl = panelLabels(info.buttonNames, m2 && m2.list);
    ok(m2 && m2.seq === 2 && pl.buttons[0] === "UNDO" && pl.buttons[5] === "SHIFT" && !pl.btnOff[1] && pl.keys[0] === null && pl.knobs[3] === "RES",
       "... the message and the merge follow");
    ok(panelLabels(info.buttonNames, m2.list, false).buttons[0] === "FX", "... and the setting off still gives the printed names");
    e.setPanel(panel(0));
    run(e, 40);
    const m3 = w.take(e);
    ok(m3 && m3.seq === 3 && m3.list[8] === "DRUMS", "GLO up: epoch 3, DRUMS again");
    e.dispose();
    ok(e.labels() === null, "disposed: null");
}

console.log("== labels_core_nolabels.rfm1: the shim's exports, no symbols in the core");
{
    const bytes = fx("labels_core_nolabels.rfm1");
    ok(inspect(bytes).labels === true, "inspect: the exports are there (every shim build has them)");
    const e = await mk(bytes);
    run(e, 100);
    ok(e.labels() === null && new LabelWatch().take(e) === null && e.info().buttonNames[0] === "KICK", "epoch 0: labels() null, no message, button names as before");
    e.dispose();
}

console.log("== an .rfm1 from before the exports");
{
    // fs-test's plain module with a manifest: no rfm1_labels_* at all
    const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
    const custom = (name, payload) => { const nb = Buffer.from(name), body = [...leb(nb.length), ...nb, ...payload]; return [0, ...leb(body.length), ...body]; };
    const man = { rfm1: 1, abi: 6, id: "fscart", name: "FS Cart", version: "1.0", license: "MIT", source_url: "https://example.com/fscart@0" };
    const plain = Uint8Array.from([...fx("fs_cart_plain.wasm"), ...custom("rfm1", Buffer.from(JSON.stringify(man)))]);
    ok(inspect(plain).labels === false, "inspect: no label exports: labels false, the file accepted");
    const e = await mk(plain);
    run(e, 50);
    ok(e.labels() === null && new LabelWatch().take(e) === null, "boots; labels() null, no message");
    e.dispose();
    for (const f of olds) {
        const b = new Uint8Array(fs.readFileSync(f)), i = inspect(b);
        const o = await mk(b);
        run(o, 200);
        ok(o.labels() === null && Array.isArray(o.info().buttonNames), `${path.basename(f)}: labels ${i.labels ? "exported" : "not exported"}; boots, labels() null`);
        o.dispose();
    }
}

console.log(fails ? `labels: ${fails} of ${n} FAILED` : `labels tests passed (${n})`);
process.exit(fails ? 1 : 0);
