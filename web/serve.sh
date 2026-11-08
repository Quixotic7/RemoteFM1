#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The site from this checkout on a local server: http://127.0.0.1:PORT/site/ (default 8787). It first checks that the
# engines the page loads are built (web/site/tools/engines-check.mjs): a checkout has the page but not its engines
# (web/build/native, web/build/emu, web/build/remote are build outputs), and without them every firmware plays the mock
# engine's TEST SCREEN. --build builds what is missing first (FM1VST from web/vendor/FM1VST; Emscripten and rustup).
#   web/serve.sh [--build] [PORT]
set -e
cd "$(dirname "$0")"
BUILD=0
[ "${1:-}" = --build ] && { BUILD=1; shift; }
PORT="${1:-8787}"
if ! node site/tools/engines-check.mjs .; then
    if [ $BUILD = 1 ]; then
        [ -f build/emu/fm1_web.wasm ] || bash engines/emu/build.sh
        FM1VST_DIR="${FM1VST_DIR:-$PWD/vendor/FM1VST}" bash engines/native/build.sh || true
        [ -f build/remote/remotefm1.mjs ] || sh remote/build.sh
        node site/tools/engines-check.mjs . || echo "serve.sh: still missing some (above): those firmwares play the test engine"
    else
        echo "serve.sh: serving anyway; web/serve.sh --build builds them"
    fi
fi
echo "serving $(pwd) on http://127.0.0.1:$PORT/site/"
exec python3 -m http.server "$PORT" --bind 127.0.0.1
