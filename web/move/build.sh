#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# rfm_move for the browser: web/build/move/rfm_move.mjs, the Ableton Move core (host/src/move/move.c, the vendored
# Move Remote client's mr_decode, its 1-bit graphics and font) and web/move/src/move_web.c's flat API, one
# single-file ES module (the .wasm inlined) that loads in a page, a Worker and Node. A build output like
# web/build/remote (never committed): the Pages workflow builds it with remote/build.sh's step.
#   web/move/build.sh            (Emscripten from ~/emsdk, or emcc on PATH as in CI)
set -e
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
if ! command -v emcc >/dev/null 2>&1; then
    . "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
OUT=$ROOT/web/build/move
mkdir -p "$OUT"
MR=host/third_party/moveremote
emcc -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/host/include" -I"$ROOT/$MR/include" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node -sSINGLE_FILE=1 -sFILESYSTEM=0 \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=1MB -sSTACK_SIZE=128KB -sEXPORT_NAME=createRfmMove \
    -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,stringToUTF8,lengthBytesUTF8 \
    host/src/move/move.c host/src/move/screen.c host/src/move/seq.c host/src/move/seq_ui.c host/src/move/project.c "$MR/host/mr_client.c" "$MR/host/mr_gfx.c" "$MR/common/font5x7.c" web/move/src/move_web.c \
    -o "$OUT/rfm_move.mjs"
ls -l "$OUT"
