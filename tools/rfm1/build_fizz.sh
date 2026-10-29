#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# FIZZ (https://github.com/Quixotic7/FIZZ, MPL-2.0) as the site's own .rfm1: web/site/fw/fizz-<version>.rfm1, built
# with FM1VST's fm1_add_core and fm1_add_rfm1 (tools/rfm1/fizz/CMakeLists.txt) and checked (rfm1_check.py, then
# booted and played in wasm3 by rfm1_run). tools/rfm1/README.md says how to move it to a new FIZZ release.
#
#   tools/rfm1/build_fizz.sh
#
#   FIZZ_SRC=<dir>     a FIZZ checkout at the release's commit (RemoteFM1IOS's vendor/FIZZ); default: a clone of
#                      the tag in build/rfm1-fizz/
#   FM1VST_DIR=<dir>   FM1VST at FM1VST_COMMIT; default: a clone in build/rfm1-fizz/
#   FM1_EMCC=<emcc>    default: emcc on PATH, else ~/emsdk's
#
# Nothing GPL of FIZZ is built: its site/ and core/tests/ are left out of the copy compiled (build/rfm1-fizz/src).
set -euo pipefail

FIZZ_VERSION=0.12.0-beta.12
FIZZ_COMMIT=1b7f8efbe6276a7f78c61da39ff45aee4d9b805e      # (tag v0.12.0-beta.12)
FM1VST_COMMIT=d5f8f64101b401abf6fd9aaa3f20c3c507e34d60    # (branch rfm1-target: fm1_add_rfm1, the 15 published .rfm1)
# the file's licence: FIZZ's, the glue's (FM1VST: core-glue/common, core-api), plutovg's and Emscripten's, FreeType's
# rasterizer in plutovg, the font
LICENSE="MPL-2.0 AND GPL-3.0-only AND MIT AND FTL AND OFL-1.1"
MEMORY_MIB=16

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$ROOT/build/rfm1-fizz
OUT=$ROOT/web/site/fw/fizz-$FIZZ_VERSION.rfm1
mkdir -p "$B"
say() { printf '== %s\n' "$*"; }

# a checkout at the commit, clean where the build reads it
at_commit() {   # dir commit what paths..
    local d=$1 c=$2 what=$3
    shift 3
    [ "$(git -C "$d" rev-parse HEAD)" = "$c" ] || { echo "$what at $d is $(git -C "$d" rev-parse --short HEAD), not ${c:0:12}"; exit 1; }
    [ -z "$(git -C "$d" status --porcelain -- "$@")" ] || { echo "$what at $d has local changes in $*"; exit 1; }
}
clone_at() {    # url commit dir [tag]
    if [ ! -d "$3/.git" ]; then
        if [ -n "${4:-}" ]; then git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$4" "$1" "$3"
        else git clone --quiet --filter=blob:none --no-checkout "$1" "$3"; fi
    fi
    git -C "$3" fetch --quiet --depth 1 origin "$2" 2>/dev/null || true
    git -C "$3" -c advice.detachedHead=false checkout --quiet "$2"
}

FIZZ_SRC=${FIZZ_SRC:-}
if [ -z "$FIZZ_SRC" ]; then FIZZ_SRC=$B/FIZZ-git; clone_at https://github.com/Quixotic7/FIZZ "$FIZZ_COMMIT" "$FIZZ_SRC" "v$FIZZ_VERSION"; fi
at_commit "$FIZZ_SRC" "$FIZZ_COMMIT" FIZZ core third_party assets LICENSE LICENSING.md
FM1VST_DIR=${FM1VST_DIR:-}
if [ -z "$FM1VST_DIR" ]; then FM1VST_DIR=$B/FM1VST-git; clone_at https://github.com/Quixotic7/FM1VST "$FM1VST_COMMIT" "$FM1VST_DIR"; fi
at_commit "$FM1VST_DIR" "$FM1VST_COMMIT" FM1VST CMakeLists.txt cmake core-api core-glue/common tools third_party/wasm3

# FIZZ's firmware sources and their licences only (as RemoteFM1IOS's scripts/build_cores.sh), with a .git file
# pointing at the checkout's git dir, so fm1_add_core reads the commit
SRC=$B/src/FIZZ
mkdir -p "$SRC"
rsync -a --delete --exclude=.git --exclude=/core/tests/ --include=/LICENSE --include=/LICENSING.md \
    --include='/core/***' --include='/third_party/***' --include=/assets/ --include=/assets/fonts/ \
    --include=/assets/fonts/OFL.txt --exclude='*' "$FIZZ_SRC/" "$SRC/"
echo "gitdir: $(git -C "$FIZZ_SRC" rev-parse --absolute-git-dir)" >"$SRC/.git"
[ ! -e "$SRC/site" ] && [ ! -e "$SRC/core/tests" ] || { echo "GPL parts of FIZZ in $SRC"; exit 1; }
# plutovg's rasterizer without setjmp in Wasm (the patch says why; the native build is unchanged)
patch -s -p1 -d "$SRC" <"$ROOT/tools/rfm1/fizz/plutovg-ft-raster-wasm.patch"

EMCC=${FM1_EMCC:-$(command -v emcc || true)}
if [ -z "$EMCC" ] && [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    # shellcheck disable=SC1091
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
    EMCC=$(command -v emcc || true)
fi
[ -n "$EMCC" ] || { echo "no emcc (Emscripten): install it, or FM1_EMCC=/path/to/emcc"; exit 1; }
CMAKE=${CMAKE:-$(command -v cmake || echo /opt/homebrew/bin/cmake)}

say "FIZZ $FIZZ_VERSION (${FIZZ_COMMIT:0:12}), FM1VST ${FM1VST_COMMIT:0:12}, $("$EMCC" --version | head -1 | sed 's/.*) //')"
"$CMAKE" -S "$ROOT/tools/rfm1/fizz" -B "$B/build" -DCMAKE_BUILD_TYPE=Release -DFM1VST_DIR="$FM1VST_DIR" \
    -DFIZZ_DIR="$SRC" -DFIZZ_VERSION="$FIZZ_VERSION" -DFIZZ_MEMORY_MIB=$MEMORY_MIB -DFM1_EMCC="$EMCC" \
    -DCR_PYTHON="$(command -v python3)" >"$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
"$CMAKE" --build "$B/build" --target rfm1_fizz rfm1_run -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >"$B/build.log" 2>&1 \
    || { tail -40 "$B/build.log"; exit 1; }

say "the manifest: the licence expression and glue_url"
mkdir -p "$(dirname "$OUT")"
python3 "$ROOT/tools/rfm1/fizz/manifest.py" --in "$B/build/rfm1/fizz-$FIZZ_VERSION.rfm1" --out "$OUT" \
    --license "$LICENSE" --glue-url "https://github.com/Quixotic7/FM1VST/tree/$FM1VST_COMMIT" --source-commit "$FIZZ_COMMIT"
say "checked"
python3 "$FM1VST_DIR/tools/rfm1_check.py" "$OUT"
"$B/build/rfm1_run" "$OUT"
say "$OUT: $(wc -c <"$OUT" | tr -d ' ') bytes, SHA-256 $(shasum -a 256 "$OUT" | cut -d' ' -f1)"
