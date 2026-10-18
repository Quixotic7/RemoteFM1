#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# libremotefm1 for the browser: web/build/remote/remotefm1.mjs (the site's) and fakeunit.mjs (the tests' fake unit).
# Both are single-file ES modules (the .wasm inlined), loadable in a page, a Worker and Node.
#   web/remote/build.sh            (Emscripten from ~/emsdk, or emcc on PATH as in CI)
set -e
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
if ! command -v emcc >/dev/null 2>&1; then
    . "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
OUT=$ROOT/web/build/remote
mkdir -p "$OUT"
INC="-I$ROOT/host/include -I$ROOT/proto -I$ROOT/firmware/src"
COMMON="-O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node
 -sSINGLE_FILE=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=8MB -sFILESYSTEM=0
 -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16,HEAPU32,UTF8ToString,stringToUTF8"

# the site's module: the portable session, codec, screen encoder and panel decoder (no fake, no reference decoder)
emcc $COMMON $INC -sEXPORT_NAME=createRemoteFM1 \
    host/src/codec.c host/src/screen.c host/src/panel.c host/src/session.c web/remote/src/rfm_web.c \
    -o "$OUT/remotefm1.mjs"

# the tests' fake unit: host/src/fake.c with the reference TILES decoder
emcc $COMMON $INC -sEXPORT_NAME=createFakeUnit \
    host/src/codec.c host/src/tiles_ref.c host/src/fake.c web/remote/src/fake_web.c \
    -o "$OUT/fakeunit.mjs"

ls -l "$OUT"
