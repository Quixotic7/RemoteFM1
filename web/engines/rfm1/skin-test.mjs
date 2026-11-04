// SPDX-License-Identifier: GPL-3.0-only
// Skins (RemoteFM1IOS docs/RFM1-FORMAT.md section 8; web/site/js/skin.js): an .rfm1 with rfm1.skin sections, made in
// memory from fixtures/labels_core.rfm1 by web/site/tools/rfm1-skin.mjs (and by RemoteFM1IOS scripts/rfm1_skin.py
// when that repo is beside this one: both apps read the same file), still loads and plays; inspect() and info() hand
// the sections over; the page's parse, the skin it picks, the crop. No skinned binary is committed.
//   node web/engines/rfm1/skin-test.mjs
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import zlib from "node:zlib";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createEngine, inspect } from "./engine.js";
import { addSkin } from "../../site/tools/rfm1-skin.mjs";
import { parseSkin, pickSkin, coverRect, imageFor, clampDim, skinTheme, SKIN_THEME_NAME, MAX_DIM, DEFAULT_DIM } from "../../site/js/skin.js";
import { derived } from "../../site/js/theme.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
let fails = 0, n = 0;
const ok = (c, what) => { n++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return c; };
const same = (a, b) => a && b && a.length === b.length && a.every((v, i) => v === b[i]);
const plain = new Uint8Array(fs.readFileSync(path.join(HERE, "fixtures", "labels_core.rfm1")));

// a small PNG (w x h, one colour), made here
function png(w, h, rgb) {
  const crc = (buf) => { let c = ~0; for (const b of buf) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); } return ~c >>> 0; };
  const chunk = (type, data) => { const t = Buffer.from(type), len = Buffer.alloc(4), cr = Buffer.alloc(4); len.writeUInt32BE(data.length); cr.writeUInt32BE(crc(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, cr]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) raw.set(rgb, y * (w * 3 + 1) + 1 + x * 3);
  return new Uint8Array(Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]));
}
const bg = png(8, 12, [200, 40, 120]), wide = png(12, 8, [40, 120, 200]);
const five = { body: "#223344", key: "#AA5500", knob: "#111111", led: "#FFEE88", bezel: "#000000" };

console.log("== a skinned .rfm1 (rfm1-skin.mjs)");
{
  const skinned = addSkin(plain, { background: bg, landscape: wide, theme: five, dim: 0.3 });
  const f = inspect(skinned);
  ok(inspect(plain).skin === null, "inspect: a file without rfm1.skin has skin null");
  ok(f.skin && same(f.skin["rfm1.skin.background"], bg) && same(f.skin["rfm1.skin.background.landscape"], wide), "inspect: the three sections, the images byte for byte");
  const e = await createEngine({ sampleRate: 44100, files: { wasm: skinned }, flash: null, options: {} });
  const info = e.info(), L = new Float32Array(128), R = new Float32Array(128);
  for (let i = 0; i < 400; i++) e.render(L, R);
  ok(info.id === inspect(plain).manifest.id && info.skin && "rfm1.skin" in info.skin, `the skinned file boots and renders (${info.id}); info().skin has its sections`);
  const p = parseSkin(info.skin);
  ok(p && p.dim === 0.3 && same(p.background, bg) && same(p.landscape, wide) && p.theme && p.theme.body === "#223344" && p.theme.name === SKIN_THEME_NAME,
     "parseSkin: dim 0.3, both images, the five colours");
  const d = derived(five, SKIN_THEME_NAME);
  ok(Object.keys(d).every((k) => d[k] === p.theme[k]), "the five colours derive the rest as a visitor's theme (labels legible)");
  const again = addSkin(skinned, { background: wide });
  ok(!inspect(again).skin["rfm1.skin.background.landscape"] && same(inspect(again).skin["rfm1.skin.background"], wide) && again.length < skinned.length,
     "adding a skin again replaces the old one");
}

console.log("== the format's rules (parseSkin)");
{
  const sec = (j, extra = {}) => ({ "rfm1.skin": new TextEncoder().encode(typeof j === "string" ? j : JSON.stringify(j)), ...extra });
  ok(parseSkin(null) === null && parseSkin({}) === null, "no rfm1.skin: none");
  ok(parseSkin(sec({ skin: 2 }, { "rfm1.skin.background": bg })) === null, "skin != 1: none");
  ok(parseSkin(sec("{not json", { "rfm1.skin.background": bg })) === null, "bad JSON: none");
  ok(parseSkin(sec({ skin: 1 })) === null, "neither an image nor a theme: none");
  ok(parseSkin(sec({ skin: 1, theme: { body: "#112233" } })) === null, "a theme of one colour (not five, not fourteen): none");
  const big = new Uint8Array((4 << 20) + 1);
  ok(parseSkin(sec({ skin: 1 }, { "rfm1.skin.background": big })) === null, "an image over 4 MiB is left out (and with nothing else, no skin)");
  const t = parseSkin(sec({ skin: 1, theme: five }));
  ok(t && !t.background && t.theme && t.dim === 0, "colours alone are a skin; dim defaults to 0");
  ok(parseSkin(sec({ skin: 1, dim: 5 }, { "rfm1.skin.background": bg })).dim === MAX_DIM && clampDim(-1) === 0 && clampDim("x") === 0, "dim is kept within 0..0.8");
  const full = { body: "#010203", label: "#FFFFFF", bed: "#111111", key: "#222222", hi: "#333333", lo: "#000000", keyText: "#EEEEEE",
    knob: "#444444", mark: "#DDDDDD", bezel: "#050505", lcd: "#C0C0C0", ink: "#101010", led: "#FF00FF", ledOff: "#555555" };
  const ft = skinTheme(full);
  ok(ft && ft.label === "#FFFFFF" && ft.ledOff === "#555555", "all fourteen roles: used as given");
}

console.log("== which skin shows (pickSkin), the crop");
{
  const fw = { id: "rfm1:abc", portrait: {}, landscape: null, dim: 0.1, theme: null }, user = { id: "user:1", portrait: {}, landscape: null };
  ok(pickSkin({ fw, user, dim: 0.4 }) === fw, "the firmware's skin wins over the visitor's image");
  const u = pickSkin({ firmwareSkins: false, fw, user, dim: 0.4 });
  ok(u && u.id === "user:1" && u.dim === 0.4 && u.theme === null, "Firmware skins off: the visitor's image, with the Dim setting");
  ok(pickSkin({ user, dim: 9 }).dim === MAX_DIM && pickSkin({}) === null && DEFAULT_DIM === 0.25, "no skin at all: none; the default dim 0.25");
  const P = { tag: "p" }, Ls = { tag: "l" };
  ok(imageFor({ portrait: P, landscape: Ls }, true) === Ls && imageFor({ portrait: P, landscape: Ls }, false) === P && imageFor({ portrait: P }, true) === P
     && imageFor({ landscape: Ls }, false) === Ls, "a wide canvas gets the landscape image, if there is one");
  const [sx, sy, sw, sh] = coverRect(1000, 500, 400, 400);
  ok(sx === 250 && sy === 0 && sw === 500 && sh === 500, "cover: a wide image on a square canvas is cropped at its sides");
}

// both apps read the same file: the iOS repo's script, when it is beside this one
const PY = path.resolve(HERE, "../../../../RemoteFM1IOS/scripts/rfm1_skin.py");
const PY2 = path.resolve(HERE, "../../../../../../../RemoteFM1IOS/scripts/rfm1_skin.py");   // (from a .claude/worktrees checkout)
const py = [PY, PY2].find((p) => fs.existsSync(p));
console.log("== RemoteFM1IOS scripts/rfm1_skin.py");
if (!py) console.log("  (skipped: no RemoteFM1IOS beside this repo)");
else {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "rfm1-skin-"));
  const w = (nm, b) => { const f = path.join(tmp, nm); fs.writeFileSync(f, b); return f; };
  const inF = w("in.rfm1", plain), out = path.join(tmp, "out.rfm1");
  execFileSync("python3", [py, "add", inF, "-o", out, "--background", w("bg.png", bg), "--theme", w("t.json", JSON.stringify(five)), "--dim", "0.2"]);
  const p = parseSkin(inspect(new Uint8Array(fs.readFileSync(out))).skin);
  ok(p && p.dim === 0.2 && same(p.background, bg) && p.theme && p.theme.key === "#AA5500", "a file skinned by the iOS app's script: the same skin here");
  const mine = addSkin(plain, { background: bg, theme: five, dim: 0.2 });
  const shown = execFileSync("python3", [py, "show", w("mine.rfm1", mine)], { encoding: "utf8" });
  ok(/0\.2/.test(shown) && /#223344/i.test(shown), `and this tool's file read by it: ${shown.trim().split("\n").join(" | ").slice(0, 120)}`);
  fs.rmSync(tmp, { recursive: true, force: true });
}

console.log(fails ? `skin: ${fails} of ${n} FAILED` : `skin tests passed (${n})`);
process.exit(fails ? 1 : 0);
