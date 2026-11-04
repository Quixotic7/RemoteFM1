// SPDX-License-Identifier: GPL-3.0-only
// Add a skin to an .rfm1 firmware (RemoteFM1IOS docs/RFM1-FORMAT.md section 8), as RemoteFM1IOS scripts/rfm1_skin.py:
// the same arguments, the same sections, for this repo's tests (web/site/tools/skin-e2e.mjs) and for authors.
//   node web/site/tools/rfm1-skin.mjs add firmware.rfm1 -o skinned.rfm1 [--background bg.png] [--landscape wide.png]
//        [--theme theme.json] [--dim 0.2]
// The skin goes in custom sections at the end of the file (the code is untouched; its SHA-256 changes); an existing
// skin is replaced. theme.json: {"body": "#RRGGBB", "key", "knob", "led", "bezel"} or all fourteen roles.
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const leb = (n) => { const o = []; do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; o.push(b); } while (n); return o; };
function readLeb(b, at) { let v = 0, sh = 0, c; do { c = b[at++]; v |= (c & 0x7f) << sh; sh += 7; } while (c & 0x80); return [v >>> 0, at]; }
const custom = (name, payload) => {
  const nm = new TextEncoder().encode(name), body = [...leb(nm.length), ...nm];
  const out = new Uint8Array(1 + leb(body.length + payload.length).length + body.length + payload.length);
  const head = [0, ...leb(body.length + payload.length), ...body];
  out.set(head, 0); out.set(payload, head.length);
  return out;
};

// bytes: the .rfm1; skin: { background, landscape (Uint8Array), theme (object), dim (number) } -> the skinned file
export function addSkin(bytes, { background = null, landscape = null, theme = null, dim = null } = {}) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (b.length < 8 || b[0] !== 0 || b[1] !== 0x61 || b[2] !== 0x73 || b[3] !== 0x6d) throw new Error("not a WebAssembly file");
  for (const im of [background, landscape]) if (im && im.length > 4 << 20) throw new Error("an image over 4 MiB");
  const keep = [b.subarray(0, 8)];
  for (let at = 8; at < b.length;) {
    const start = at, id = b[at++];
    let size; [size, at] = readLeb(b, at);
    const end = at + size;
    let skin = false;
    if (id === 0) { const [n, p] = readLeb(b, at); skin = new TextDecoder().decode(b.subarray(p, p + n)).startsWith("rfm1.skin"); }
    if (!skin) keep.push(b.subarray(start, end));
    at = end;
  }
  const j = { skin: 1 };
  if (dim !== null && dim !== undefined) j.dim = dim;
  if (theme) j.theme = theme;
  keep.push(custom("rfm1.skin", new TextEncoder().encode(JSON.stringify(j))));
  if (background) keep.push(custom("rfm1.skin.background", background));
  if (landscape) keep.push(custom("rfm1.skin.background.landscape", landscape));
  const out = new Uint8Array(keep.reduce((n, x) => n + x.length, 0));
  let o = 0; for (const x of keep) { out.set(x, o); o += x.length; }
  return out;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const a = process.argv.slice(2), opt = (k) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : null; };
  if (a[0] !== "add" || !a[1] || !opt("-o")) { console.error("usage: rfm1-skin.mjs add firmware.rfm1 -o out.rfm1 [--background bg.png] [--landscape wide.png] [--theme t.json] [--dim 0.2]"); process.exit(2); }
  const read = (k) => (opt(k) ? new Uint8Array(readFileSync(opt(k))) : null);
  const out = addSkin(readFileSync(a[1]), { background: read("--background"), landscape: read("--landscape"),
    theme: opt("--theme") ? JSON.parse(readFileSync(opt("--theme"), "utf8")) : null, dim: opt("--dim") !== null ? +opt("--dim") : null });
  writeFileSync(opt("-o"), out);
  console.log(`${opt("-o")}: ${out.length} bytes`);
}
