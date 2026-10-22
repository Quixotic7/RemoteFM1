#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The native engine's build: firmware cores compiled to WebAssembly with Emscripten, on FM1VST's core machinery.
#
#   web/engines/native/build.sh [id ...]        FM1VST's 21 un-gated cores (default: all twenty-one)
#   web/engines/native/build.sh --ext TREE --id ID --family FAMILY [--core FILE] [--glue DIR] [--version V]
#                               [--out DIR] [--host]
#                                               one firmware from an external tree: an author's own checkout, built
#                                               on FM1VST's glue (docs/NATIVE-AUTHORS.md, .github/workflows/fm1-native.yml)
#
# Environment:
#   FM1VST_DIR   FM1VST's tree, read only (default: the cores-all worktree here; in CI the submodule or a checkout)
#   CR_PYTHON    python3 with Pillow (with libraqm) and fontTools, for the cores' generate steps
#   EMSDK        Emscripten's SDK (default ~/emsdk; when emcc is on PATH already, that one)
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
#
# --ext (an author's build): the same four steps for one core whose tree is TREE (the author's checkout at its
# release tag, with its submodules) instead of FM1VST's submodule. The core's description, one of:
#   --glue DIR with a core.cmake   the author's own glue folder (files copied to core-glue/ext_<id>/): its core.cmake
#                                  calls fm1_add_core(<id> ${CMAKE_CURRENT_LIST_DIR} SOURCE ${FM1WEB_TREE} ...)
#   --core FILE                    the fm1_add_core arguments after SOURCE (UNIT, DESC, GEN, VERSION...), as text; the
#                                  units named are in --glue DIR (or, with no --glue, the family's own folder)
#   neither                        FM1VST's own core-glue/<family>/core_<id>.cmake (or core.cmake): a firmware FM1VST
#                                  already builds, from the author's tree in place of FM1VST's submodule
# The tree goes where the glue expects it: cores/<the name FM1VST's cmake gives> for FM1VST's own glue, else
# cores/<id> (a glue unit reaches it as "../../cores/<id>/...", a cmake as ${FM1WEB_TREE}). With a family other than
# custom, core-glue/<family> is on the include path of the author's glue units (#include "fw_core.h").
# --version: the version the descriptor reports (default: what the glue says). --host also builds the same core with
# the host's C compiler (host_shot.c: the first screen as a PPM, the conformance test's reference picture).
# Output (--out, default web/build/native-ext/<id>): <id>.wasm, meta.json, and with --host <id>-host.ppm.
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
FAMILIES="felucca_web fm1base sloop dinghy choralroot plat melodee tonitor speak37 custom"

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
loom         fm1base/core_loom.cmake              fm1-loom                    fm1base
duell        felucca_web/core_duell.cmake         DUELL                       felucca_web
minitone     felucca_web/core_minitone.cmake      Mini-Tone                   felucca_web
fmini        felucca_web/core_fmini.cmake         fmini-1                     felucca_web
skiff        felucca_web/core_skiff.cmake         skiff                       felucca_web
grimm        dinghy/core_grimm.cmake              grimm                       dinghy
pigeon       felucca_web/core_pigeon.cmake        pigeon                      felucca_web
tonitor      tonitor/core.cmake                   tonitor                     tonitor
speak37      speak37/core_speak37.cmake           Speak37                     speak37
"

# ---- the arguments
EXT="" X_ID="" X_FAM="" X_CORE="" X_GLUE="" X_VER="" X_OUT="" X_HOST=0
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ext) EXT="$2"; shift 2 ;;
        --id) X_ID="$2"; shift 2 ;;
        --family) X_FAM="$2"; shift 2 ;;
        --core) X_CORE="$2"; shift 2 ;;
        --glue) X_GLUE="$2"; shift 2 ;;
        --version) X_VER="$2"; shift 2 ;;
        --out) X_OUT="$2"; shift 2 ;;
        --host) X_HOST=1; shift ;;
        -*) echo "build.sh: unknown option $1" >&2; exit 2 ;;
        *) args+=("$1"); shift ;;
    esac
done

[ -f "$FM1VST_DIR/core-api/fm1core.h" ] || { echo "build.sh: no FM1VST at $FM1VST_DIR (FM1VST_DIR)" >&2; exit 2; }
if ! command -v emcc > /dev/null; then
    # shellcheck disable=SC1091
    source "$EMSDK/emsdk_env.sh" > /dev/null 2>&1 || true
fi
command -v emcc > /dev/null || { echo "build.sh: no emcc (EMSDK=$EMSDK)" >&2; exit 2; }
WASM_OPT="$(dirname "$(command -v emcc)")/../bin/wasm-opt"
[ -x "$WASM_OPT" ] || WASM_OPT="$(command -v wasm-opt || true)"
[ -x "$WASM_OPT" ] || WASM_OPT="$EMSDK/upstream/bin/wasm-opt"

# a tree's tracked files (with its submodules') into a mirror folder; rsync keeps the times, so an unchanged file does
# not rebuild. A tree that is not a git checkout (an unpacked archive) is copied whole, without .git and build/.
mirror_tree() {
    local from="$1" to="$2"
    mkdir -p "$to"
    if git -C "$from" rev-parse --is-inside-work-tree > /dev/null 2>&1 &&
       [ "$(cd "$from" && git rev-parse --show-toplevel)" = "$(cd "$from" && pwd -P)" ]; then
        git -C "$from" ls-files -z --recurse-submodules | rsync -a --from0 --files-from=- "$from/" "$to/"
    else
        rsync -a --exclude .git --exclude /build "$from/" "$to/"
    fi
}

# one core: configure, build its library, link it; $1 id, $2 build dir, $3 output wasm, then extra cmake -D options
build_core_wasm() {
    local id="$1" bld="$2" wasm="$3"
    shift 3
    mkdir -p "$bld"
    echo "== configure (emcmake, $FM1WEB_OPT)"
    emcmake cmake -S "$SRC" -B "$bld" -DCMAKE_BUILD_TYPE=Release -DCR_PYTHON="$CR_PYTHON" \
        -DFM1WEB_CORO="$HERE/coro_wasm.c" -DFM1WEB_OPT="$FM1WEB_OPT" "$@" > "$bld/configure.log" 2>&1 ||
        { cat "$bld/configure.log"; exit 1; }
    grep -E "core: version" "$bld/configure.log" || true
    echo "== build core_$id"
    cmake --build "$bld" -j8 --target "core_$id" > "$bld/build.log" 2>&1 || { tail -60 "$bld/build.log"; exit 1; }
    link_core "$id" "$bld/libcore_$id.a" "$wasm"
}

LDFLAGS=("$FM1WEB_OPT" -sSTANDALONE_WASM --no-entry -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432
         -sSTACK_SIZE=1048576 -sERROR_ON_UNDEFINED_SYMBOLS=1)
# link one core's library with wasm_glue.c; Asyncify when it runs on the coroutine (it imports fm1w_co_yield)
link_core() {
    local id="$1" lib="$2" wasm="$3"
    echo "== link $id"
    emcc "${LDFLAGS[@]}" -I"$SRC/core-api" -Wall -Wextra "$HERE/wasm_glue.c" "$lib" -o "$wasm"
    if node -e 'const m=new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));process.exit(WebAssembly.Module.imports(m).some(i=>i.name==="fm1w_co_yield")?0:1)' "$wasm"; then
        # the coroutine (coro_wasm.c): Asyncify, with fm1w_co_yield the only import that unwinds
        "$WASM_OPT" "$wasm" --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext \
            --enable-mutable-globals --asyncify --pass-arg=asyncify-imports@env.fm1w_co_yield \
            "$FM1WEB_OPT" -o "$wasm"
    fi
}

# =============================================================================================== an author's build ===
if [ -n "$EXT" ]; then
    [ -d "$EXT" ] || { echo "build.sh: --ext $EXT is not a folder" >&2; exit 2; }
    [[ "$X_ID" =~ ^[a-z0-9][a-z0-9_-]{0,31}$ ]] || { echo "build.sh: --id must be 1-32 of a-z 0-9 _ - (got '$X_ID')" >&2; exit 2; }
    [[ " $FAMILIES " == *" $X_FAM "* ]] || { echo "build.sh: --family is one of: $FAMILIES (got '$X_FAM')" >&2; exit 2; }
    EXT="$(cd "$EXT" && pwd)"
    [ -z "$X_GLUE" ] || X_GLUE="$(cd "$X_GLUE" && pwd)"
    OUT="${X_OUT:-$WEB/build/native-ext/$X_ID}"
    mkdir -p "$OUT"
    OUT="$(cd "$OUT" && pwd)"
    SRC="$OUT/src"
    BLD="$OUT/cmake"
    rm -rf "$SRC/core-glue"                     # (the glue as this run gives it: no author file left from an earlier run)
    echo "== mirror of $FM1VST_DIR (cmake, core-api, core-glue)"
    mkdir -p "$SRC"
    git -C "$FM1VST_DIR" ls-files -z cmake core-api core-glue | rsync -a --from0 --files-from=- "$FM1VST_DIR/" "$SRC/"
    cp "$HERE/cmake/CMakeLists.txt" "$SRC/CMakeLists.txt"

    # the core's description
    mode="" cm="" tree=""
    if [ -n "$X_GLUE" ] && [ -f "$X_GLUE/core.cmake" ]; then
        mode="the author's glue ($X_GLUE/core.cmake)"
    elif [ -n "$X_CORE" ]; then
        [ -f "$X_CORE" ] || { echo "build.sh: --core $X_CORE: no such file" >&2; exit 2; }
        mode="the author's fm1_add_core arguments ($X_CORE)"
    elif [ "$X_FAM" != custom ]; then
        for f in "$SRC/core-glue/$X_FAM/core_$X_ID.cmake" "$SRC/core-glue/$X_FAM/core.cmake"; do
            if [ -f "$f" ] && grep -Eq "fm1_add_core\(${X_ID}[[:space:]]" "$f"; then cm="${f#"$SRC/"}"; break; fi
        done
        [ -n "$cm" ] || { echo "build.sh: FM1VST has no $X_ID core in core-glue/$X_FAM: give --core (the fm1_add_core arguments) or --glue" >&2; exit 2; }
        # the tree's name, from the cmake's SOURCE (directly, or through a set() variable): .../cores/<tree>
        tree="$(grep -Eo '/cores/[A-Za-z0-9._-]+' "$SRC/$cm" | head -1 | sed 's#/cores/##')"
        [ -n "$tree" ] || { echo "build.sh: no cores/<tree> in $cm" >&2; exit 2; }
        mode="FM1VST's own glue ($cm)"
    else
        echo "build.sh: --family custom needs --glue DIR (with a core.cmake) or --core FILE" >&2; exit 2
    fi
    if [ -z "$cm" ]; then
        tree="$X_ID"
        gd="$SRC/core-glue/ext_$X_ID"
        mkdir -p "$gd"
        [ -z "$X_GLUE" ] || rsync -a --exclude .git "$X_GLUE/" "$gd/"
        if [ -f "$gd/core.cmake" ] && [ -n "$X_GLUE" ] && [ -f "$X_GLUE/core.cmake" ]; then
            cm="core-glue/ext_$X_ID/core.cmake"
        else
            # (the arguments' units are in the author's glue folder; with none, in the family's own)
            # shellcheck disable=SC2016
            gdir='${CMAKE_CURRENT_LIST_DIR}'
            [ -n "$X_GLUE" ] || gdir="\${CMAKE_SOURCE_DIR}/core-glue/$X_FAM"
            fam=""
            grep -Eq '(^|[[:space:]])FAMILY[[:space:]]' "$X_CORE" || fam="FAMILY $([ "$X_FAM" = custom ] && echo "$X_ID" || echo "$X_FAM")"
            {
                echo "# generated by web/engines/native/build.sh --ext: the author's fm1_add_core arguments ($X_CORE)"
                echo "fm1_add_core($X_ID $gdir SOURCE \${FM1WEB_TREE} $fam"
                cat "$X_CORE"                   # (CMake reads its own comments; a "#" in a quoted regex stays)
                echo ")"
            } > "$gd/fm1web_core.cmake"
            cm="core-glue/ext_$X_ID/fm1web_core.cmake"
        fi
    fi
    echo "== $X_ID ($X_FAM): $mode; the tree at cores/$tree"
    mirror_tree "$EXT" "$SRC/cores/$tree"
    commit="$(git -C "$EXT" rev-parse HEAD 2>/dev/null || echo unknown)"
    famdir=""
    [ "$X_FAM" = custom ] || famdir="$SRC/core-glue/$X_FAM"
    defs=(-DFM1WEB_CORES="$X_ID=$cm=$tree" -DFM1WEB_TREE="$SRC/cores/$tree" -DFM1WEB_FAMILY_DIR="$famdir"
          "-DFM1WEB_COMMIT_$X_ID=$commit")
    [ -z "$X_VER" ] || defs+=("-DFM1WEB_VERSION_$X_ID=$X_VER")
    build_core_wasm "$X_ID" "$BLD" "$OUT/$X_ID.wasm" "${defs[@]}"
    ver=$(grep -E "^-- $X_ID core: version" "$BLD/configure.log" | sed -E 's/.*version ([^,]*),.*/\1/' || true)
    [ -z "$X_VER" ] || ver="$X_VER"
    size=$(wc -c < "$OUT/$X_ID.wasm" | tr -d ' ')
    asy=false
    node -e 'const m=new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));process.exit(WebAssembly.Module.exports(m).some(e=>e.name==="asyncify_start_unwind")?0:1)' "$OUT/$X_ID.wasm" && asy=true
    printf '  %s.wasm  %d bytes%s\n' "$X_ID" "$size" "$([ $asy = true ] && echo ', Asyncify')"

    # the same core built with the host's compiler: its first screen, the conformance test's reference
    host=""
    if [ "$X_HOST" = 1 ]; then
        HB="$OUT/cmake-host"
        mkdir -p "$HB"
        echo "== the host build (the reference picture)"
        if cmake -S "$SRC" -B "$HB" -DCMAKE_BUILD_TYPE=Release -DCR_PYTHON="$CR_PYTHON" -DFM1WEB_HOST=ON \
                -DFM1WEB_CORO="$SRC/core-glue/common/coro.c" -DFM1WEB_OPT=-O2 "${defs[@]}" > "$HB/configure.log" 2>&1 &&
           cmake --build "$HB" -j8 --target "core_$X_ID" > "$HB/build.log" 2>&1 &&
           cc -O2 -I"$SRC/core-api" -DFMW_HOST "$HERE/host_shot.c" "$HERE/wasm_glue.c" "$HB/libcore_$X_ID.a" \
                -lm -lpthread -o "$HB/host_shot" > "$HB/link.log" 2>&1 &&
           "$HB/host_shot" "$OUT/$X_ID-host.ppm" 3000; then
            host="$X_ID-host.ppm"
        else
            echo "build.sh: the host build failed (no reference picture; logs in $HB)" >&2
            tail -20 "$HB/build.log" "$HB/link.log" 2>/dev/null >&2 || true
        fi
    fi
    fv="$(git -C "$FM1VST_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
    cat > "$OUT/meta.json" <<EOF
{"id":"$X_ID","wasm":"$X_ID.wasm","bytes":$size,"family":"$X_FAM","version":"$ver","commit":"$commit","tree":"$tree","glue":"${cm%/*}","fm1vst":"$fv","asyncify":$asy,"hostPicture":$([ -n "$host" ] && echo "\"$host\"" || echo null),"conformance":$(f="$SRC/${cm%/*}/conformance_$X_ID.txt"; [ -f "$f" ] || f="$SRC/${cm%/*}/conformance.txt"; [ -f "$f" ] && echo "\"${f#"$SRC/"}\"" || echo null)}
EOF
    echo "== done: $OUT"
    exit 0
fi

# ================================================================================================= FM1VST's cores ===
OUT="$WEB/build/native"
SRC="$OUT/src"
BLD="$OUT/cmake"
want="${args[*]:-}"
sel=()
while read -r id cm tree fam; do
    [ -n "$id" ] || continue
    if [ -z "$want" ] || [[ " $want " == *" $id "* ]]; then
        sel+=("$id:$cm:$tree:$fam")
    fi
done <<< "$CORES"
[ ${#sel[@]} -gt 0 ] || { echo "build.sh: no such core: $want" >&2; exit 2; }
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
    mirror_tree "$t" "$SRC/cores/$tree"
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
mkdir -p "$OUT/meta"
for e in "${sel[@]}"; do
    IFS=: read -r id cm tree fam <<< "$e"
    link_core "$id" "$BLD/libcore_$id.a" "$OUT/$id.wasm"
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
