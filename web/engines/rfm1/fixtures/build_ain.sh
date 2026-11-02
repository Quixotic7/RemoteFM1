#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# ain_cart.c (the audio-input test cart) built as a module without a manifest (ain_cart.wasm: engines/rfm1/ain-test.mjs
# adds one per case) and as ain_cart.rfm1 (with "wants": ["ain1"]: what web/site/tools/ain-e2e.mjs loads, and a cart to
# open by hand from the firmware browser's "Open a file..."); both committed. A test fixture only: not in the catalog.
#   web/engines/rfm1/fixtures/build_ain.sh          (emcc from ~/emsdk when it isn't on PATH)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
command -v emcc >/dev/null || source "${EMSDK:-$HOME/emsdk}/emsdk_env.sh" >/dev/null 2>&1
emcc -O2 "$HERE/ain_cart.c" -o "$HERE/ain_cart.wasm" --no-entry -sSTANDALONE_WASM -sINITIAL_MEMORY=2MB -sALLOW_MEMORY_GROWTH=0 \
     -sSTACK_SIZE=65536 -sERROR_ON_UNDEFINED_SYMBOLS=0
node -e '
const fs = require("fs"), [wasm, out] = process.argv.slice(1);
const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
const m = { rfm1: 1, abi: 6, id: "aintest", name: "Audio In Test", version: "1.0", license: "GPL-3.0-only",
  source_url: "https://github.com/Quixotic7/RemoteFM1@0000000000000000000000000000000000000000", authors: ["RemoteFM1 tests"], wants: ["ain1"] };
const nm = Buffer.from("rfm1"), js = Buffer.from(JSON.stringify(m)), body = [...leb(nm.length), ...nm, ...js];
fs.writeFileSync(out, Buffer.concat([fs.readFileSync(wasm), Buffer.from([0, ...leb(body.length), ...body])]));
' "$HERE/ain_cart.wasm" "$HERE/ain_cart.rfm1"
echo "build_ain: $HERE/ain_cart.wasm $HERE/ain_cart.rfm1"
