#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# rfm_move for the browser: web/build/move/rfm_move.mjs, built by rfm1cs-abletonmove's own web/build.sh from the
# submodule web/vendor/rfm1cs-abletonmove (the Ableton Move core, its flat wasm API, Move Remote's client; one
# single-file ES module that loads in a page, a Worker and Node). A build output like web/build/remote (never
# committed): the Pages workflow builds it with remote/build.sh's step. The page's js/move-core.js and js/move-seq.js
# re-export the submodule's web/move-core.js and web/move-seq.js (Pages copies those in their place).
#   web/move/build.sh            (Emscripten from ~/emsdk, or emcc on PATH as in CI)
set -e
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
CS=$ROOT/web/vendor/rfm1cs-abletonmove
if [ ! -f "$CS/web/build.sh" ]; then
    echo "web/move/build.sh: web/vendor/rfm1cs-abletonmove is not checked out (git submodule update --init web/vendor/rfm1cs-abletonmove)" >&2
    exit 1
fi
sh "$CS/web/build.sh" "$ROOT/web/build/move"
