#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The native engine's build: FM1VST's 12 un-gated cores compiled to WebAssembly with Emscripten.
#
#   web/engines/native/build.sh [id ...]        (default: all twelve)
#
# Environment:
#   FM1VST_DIR   FM1VST's tree, read only (default: the cores-all worktree here; in CI the submodule)
#   CR_PYTHON    python3 with Pillow (with libraqm) and fontTools, for the cores' generate steps
#   EMSDK        Emscripten's SDK (default ~/emsdk)
#   FM1WEB_OPT   the optimisation level (default -Os: 30-40 % smaller than -O3 and 0-8 % slower; every core still
#                runs 70x real time or more in Node with nine keys held)
#
# Steps:
#   1. a mirror of what the build reads from FM1VST, its tracked files only (git ls-files): cmake/, core-api/,
#      core-glue/ and each core's tree with its submodules, in web/build/native/src (FM1VST itself is never written:
#      the generate steps write into the mirror's trees, <tree>/build/gen, as FM1VST's build writes into its own);
#   2. FM1VST's own cmake/FM1Cores.cmake and core-glue/<family>/core_<id>.cmake under emcmake (cmake/CMakeLists.txt
#      here: the generate steps and the units exactly as FM1VST builds them; coro.c replaced by coro_wasm.c);
#   3. each core's static library linked with wasm_glue.c into a standalone module, web/build/native/<id>.wasm (no
#      Emscripten JavaScript: engine.js gives it its few imports); the cores whose main loop runs on the coroutine
#      (the Dinghy cores OberVave and Frodhes, Melodee: their module imports fm1w_co_yield) then get
#      Binaryen's Asyncify pass for their coroutine (coro_wasm.c);
#   4. web/build/native/cores.json: id, size, family, version, commit per core.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WEB="$(cd "$HERE/../.." && pwd)"
FM1VST_DIR="${FM1VST_DIR:-/Volumes/Q7Media-2025/Projects/Github/ChoralRootFM1/FM1VST/.claude/worktrees/cores-all}"
EMSDK="${EMSDK:-$HOME/emsdk}"
FM1WEB_OPT="${FM1WEB_OPT:--Os}"
if [ -z "${CR_PYTHON:-}" ]; then
    if [ -x /Library/Frameworks/Python.framework/Versions/3.11/bin/python3 ]; then
        CR_PYTHON=/Library/Frameworks/Python.framework/Versions/3.11/bin/python3
    else
        CR_PYTHON="$(command -v python3)"
    fi
fi
OUT="$WEB/build/native"
SRC="$OUT/src"
BLD="$OUT/cmake"

# id  glue cmake (core-glue relative)  tree (cores/)  family
CORES="
felucca      felucca_web/core_felucca.cmake       Felucca                     felucca_web
ghoulbox     felucca_web/core_ghoulbox.cmake      ghoulbox-fm1-dungeon-synth  felucca_web
jiant        felucca_web/core_jiant.cmake         JIANT                       felucca_web
purplemonkey choralroot/core_purplemonkey.cmake   PurpleMonkeyFM1             choralroot
coralflower  choralroot/core_coralflower.cmake    CoralFlower                 choralroot
choralroot   choralroot/core_choralroot.cmake     ChoralRootFM1               choralroot
melodee      melodee/core.cmake                   melodee                     melodee
fomni        plat/core_fomni.cmake                fm1-fomni                   plat
fimba        plat/core_fimba.cmake                fm1-fimba                   plat
amb          fm1base/core_amb.cmake               fm1-amb                     fm1base
obervave     dinghy/core_obervave.cmake           obervave                    dinghy
frodhes      dinghy/core_frodhes.cmake            frodhes                     dinghy
"

want="${*:-}"
sel=()
while read -r id cm tree fam; do
    [ -n "$id" ] || continue
    if [ -z "$want" ] || [[ " $want " == *" $id "* ]]; then
        sel+=("$id:$cm:$tree:$fam")
    fi
done <<< "$CORES"
[ ${#sel[@]} -gt 0 ] || { echo "build.sh: no such core: $want" >&2; exit 2; }

[ -f "$FM1VST_DIR/core-api/fm1core.h" ] || { echo "build.sh: no FM1VST at $FM1VST_DIR (FM1VST_DIR)" >&2; exit 2; }
# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" > /dev/null 2>&1
command -v emcc > /dev/null || { echo "build.sh: no emcc (EMSDK=$EMSDK)" >&2; exit 2; }
WASM_OPT="$(dirname "$(command -v emcc)")/../bin/wasm-opt"
[ -x "$WASM_OPT" ] || WASM_OPT="$EMSDK/upstream/bin/wasm-opt"

mkdir -p "$SRC" "$BLD"

# ---- 1. the mirror (tracked files only; rsync keeps the times, so an unchanged file does not rebuild)
echo "== mirror of $FM1VST_DIR"
git -C "$FM1VST_DIR" ls-files -z cmake core-api core-glue |
    rsync -a --from0 --files-from=- "$FM1VST_DIR/" "$SRC/"
cp "$HERE/cmake/CMakeLists.txt" "$SRC/CMakeLists.txt"
defs=()
list=""
for e in "${sel[@]}"; do
    IFS=: read -r id cm tree fam <<< "$e"
    t="$FM1VST_DIR/cores/$tree"
    mkdir -p "$SRC/cores/$tree"
    git -C "$t" ls-files -z --recurse-submodules | rsync -a --from0 --files-from=- "$t/" "$SRC/cores/$tree/"
    commit="$(git -C "$t" rev-parse HEAD)"
    defs+=("-DFM1WEB_COMMIT_$id=$commit")
    list="$list${list:+;}$id=core-glue/$cm=$tree"
done

# ---- 2. FM1VST's core build under Emscripten
echo "== configure (emcmake, $FM1WEB_OPT)"
emcmake cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release -DCR_PYTHON="$CR_PYTHON" \
    -DFM1WEB_CORES="$list" -DFM1WEB_CORO="$HERE/coro_wasm.c" -DFM1WEB_OPT="$FM1WEB_OPT" "${defs[@]}" > "$BLD/configure.log" 2>&1 ||
    { cat "$BLD/configure.log"; exit 1; }
grep -E "core: version" "$BLD/configure.log" || true
targets=()
for e in "${sel[@]}"; do
    IFS=: read -r id _ <<< "$e"
    targets+=(--target "core_$id")
done
echo "== build ${#sel[@]} core libraries"
cmake --build "$BLD" -j8 "${targets[@]}" > "$BLD/build.log" 2>&1 || { tail -60 "$BLD/build.log"; exit 1; }

# ---- 3. link each core
LDFLAGS=("$FM1WEB_OPT" -sSTANDALONE_WASM --no-entry -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432
         -sSTACK_SIZE=1048576 -sERROR_ON_UNDEFINED_SYMBOLS=1)
mkdir -p "$OUT/meta"
for e in "${sel[@]}"; do
    IFS=: read -r id cm tree fam <<< "$e"
    echo "== link $id"
    lib="$BLD/libcore_$id.a"
    emcc "${LDFLAGS[@]}" -I"$SRC/core-api" -Wall -Wextra "$HERE/wasm_glue.c" "$lib" -o "$OUT/$id.wasm"
    # (a core that runs its main loop on the coroutine imports fm1w_co_yield: the Dinghy cores, Melodee)
    if node -e 'const m=new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));process.exit(WebAssembly.Module.imports(m).some(i=>i.name==="fm1w_co_yield")?0:1)' "$OUT/$id.wasm"; then
        # the coroutine (coro_wasm.c): Asyncify, with fm1w_co_yield the only import that unwinds
        "$WASM_OPT" "$OUT/$id.wasm" --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext \
            --enable-mutable-globals --asyncify --pass-arg=asyncify-imports@env.fm1w_co_yield \
            "$FM1WEB_OPT" -o "$OUT/$id.wasm"
    fi
    size=$(wc -c < "$OUT/$id.wasm" | tr -d ' ')     # (GNU stat -f is the filesystem's status, not a format)
    ver=$(grep -E "^-- $id core: version" "$BLD/configure.log" | sed -E 's/.*version ([^,]*),.*/\1/' || true)
    commit=$(git -C "$FM1VST_DIR/cores/$tree" rev-parse HEAD)
    printf '  %s.wasm  %d bytes\n' "$id" "$size"
    echo "{\"id\":\"$id\",\"wasm\":\"$id.wasm\",\"bytes\":$size,\"family\":\"$fam\",\"version\":\"$ver\",\"commit\":\"$commit\"}" > "$OUT/meta/$id.json"
done
# every core built so far (this run or an earlier one), in the table's order
json="["
while read -r id _; do
    [ -n "$id" ] && [ -f "$OUT/meta/$id.json" ] && [ -f "$OUT/$id.wasm" ] || continue
    [ "$json" = "[" ] || json="$json,"
    json="$json$(cat "$OUT/meta/$id.json")"
done <<< "$CORES"
echo "$json]" > "$OUT/cores.json"
echo "== done: $OUT"
