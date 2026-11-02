#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# labels_core.c built as two .rfm1 files the way FM1VST's fm1_add_rfm1 builds one (cmake/FM1Rfm1.cmake: its feature
# and C flags, FM1VST's core-glue/common/rfm1_shim.c linked in, the module packed with tools/rfm1_pack.py and checked
# with tools/rfm1_check.py); committed, for engines/rfm1/labels-test.mjs:
#   labels_core.rfm1           live labels (fm1core_labels_epoch / fm1core_label)
#   labels_core_nolabels.rfm1  -DNOLABELS: the same shim, no symbols (its exports give epoch 0)
#
#   FM1VST_DIR=<an FM1VST tree whose rfm1_shim.c has rfm1_labels_epoch> web/engines/rfm1/fixtures/build_labels.sh
# emcc from ~/emsdk when it isn't on PATH.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FM1VST_DIR="${FM1VST_DIR:-$HERE/../../../vendor/FM1VST}"
command -v emcc >/dev/null || source "${EMSDK:-$HOME/emsdk}/emsdk_env.sh" >/dev/null 2>&1
grep -q rfm1_labels_epoch "$FM1VST_DIR/core-glue/common/rfm1_shim.c" || { echo "build_labels: $FM1VST_DIR's rfm1_shim.c has no rfm1_labels_epoch" >&2; exit 2; }
TMP="$(mktemp -d)"
FEAT=(-mcpu=mvp -msign-ext -mnontrapping-fptoint -mbulk-memory -mmutable-globals)
CFLAGS=("${FEAT[@]}" -O2 -std=gnu11 -ffp-contract=off -fno-common -DFM1CORE_RFM1=1 -I"$FM1VST_DIR/core-api")
printf '{"abi":6,"id":"labeltest","name":"Label Test","version":"1.0","license":"GPL-3.0-only","source_url":"https://github.com/Quixotic7/RemoteFM1@0000000"}' > "$TMP/describe.json"
emcc "${CFLAGS[@]}" -c "$FM1VST_DIR/core-glue/common/rfm1_shim.c" -o "$TMP/shim.o"
for v in labels_core:"" labels_core_nolabels:-DNOLABELS; do
    out="${v%%:*}" def="${v#*:}"
    emcc "${CFLAGS[@]}" $def -c "$HERE/labels_core.c" -o "$TMP/$out.o"
    emcc "${FEAT[@]}" -O2 "$TMP/$out.o" "$TMP/shim.o" --no-entry -sSTANDALONE_WASM -sFILESYSTEM=0 -sSTACK_SIZE=65536 \
         -sINITIAL_MEMORY=1MB -sALLOW_MEMORY_GROWTH=0 -sEXPORTED_FUNCTIONS=[] -Wl,--gc-sections -o "$TMP/$out.wasm"
    python3 "$FM1VST_DIR/tools/rfm1_pack.py" --wasm "$TMP/$out.wasm" --describe "$TMP/describe.json" \
            --out "$HERE/$out.rfm1" --author "RemoteFM1 tests" --license-file "GPL-3.0-only=$FM1VST_DIR/LICENSE"
    python3 "$FM1VST_DIR/tools/rfm1_check.py" --quiet "$HERE/$out.rfm1"
    echo "build_labels: $HERE/$out.rfm1"
done
mv "$TMP" "${TMPDIR:-/tmp}/build_labels.done.$$"
