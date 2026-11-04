#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
# RemoteFM1's firmware build (README.md "Build"): fm1-base's tools/build.py on the app in firmware/, run from a patched
# copy of fm1-base. base/ is fm1-base, a git submodule pinned at a commit (felucca-bsp inside it, base/bsp), and stays
# as it is: the copy, build/fm1-base, is base/ with firmware/patches/fm1-base/*.patch and
# firmware/patches/felucca-bsp/*.patch applied (git apply: exactly, or not at all), made again at every build.
#   ./build.sh [--release X.Y.Z]    firmware/build/remotefm1[-X.Y.Z].fwsc (USB and Bluetooth MIDI)
#   ./build.sh --no-ble [...]       firmware/build/remotefm1-usb[-X.Y.Z].fwsc (USB only: nothing of JieLi's linked)
#   ./build.sh --host               the host simulator: firmware/build/host/remotefm1_host (tests/run_tests.sh)
#   ./build.sh --bench [--no-ble]   the FM6 bench build: firmware/build/remotefm1[-usb]-bench.fwsc, which measures the
#                                   standalone DX7 at boot (firmware/sa/sa_bench.c; firmware/tools/sa_bench.py reads it)
#   BOARD=smk37 [MODEL=pro|elite|mke] ./build.sh [--no-ble] [--host] [--release X.Y.Z]   the M-Vave SMK-37
#                                   (docs/SMK37.md): firmware/build/remotefm1-smk37[-elite|-mke][-usb][-X.Y.Z].fwsc, or
#                                   the simulator build/host/remotefm1-smk37[...]_host; the SMK's own patch series
#                                   (firmware/patches/smk37/) applied after the others, so the FM-1's inputs stay as
#                                   they are (BOARD unset or fm1: the FM-1)
#   BOARD=smk37 [MODEL=...] ./build.sh --web   the SMK-37's browser emulator: firmware/build/emu-smk37[-elite|-mke]/
#                                   (the page from web/emu-smk37/; docs/SMK37.md "Emulator")
#   JIELI_TOOLCHAIN  JieLi's Linux toolchain (default ~/.jieli/toolchain; on a Mac it runs in Docker)
#   AC79_SDK         JieLi's AC79 SDK, for its Bluetooth libraries (default ~/fw-AC79_AIoT_SDK; never committed)
#   PYTHON           a Python whose Pillow has libraqm (fm1-base's fonts); on a Mac DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib
set -e
cd "$(dirname "$0")"
ROOT="$PWD"
BENCH=0
for a in "$@"; do
    shift
    if [ "$a" = --bench ]; then BENCH=1; else set -- "$@" "$a"; fi
done
[ $BENCH = 0 ] || export SA_BENCH=1                     # (firmware/tools/gen_sa_config.py; SA_BENCH=1 ./build.sh
                                                        # --host: the simulator with the bench at boot)
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
if [ -z "${DYLD_LIBRARY_PATH:-}${DYLD_FALLBACK_LIBRARY_PATH:-}" ] && [ -d /opt/homebrew/lib ]; then
    # Homebrew's libraqm for Pillow, as a fallback only: Pillow keeps its own FreeType and HarfBuzz (Homebrew's newer
    # FreeType draws the fonts' edges differently, so a build would depend on the Mac's Homebrew; with Pillow's own,
    # the Mac and GitHub's runner draw the same fonts: docs/RELEASING.md)
    export DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib
fi
PY="${PYTHON:-python3}"

[ -f base/tools/build.py ] && [ -f base/bsp/chip/usb.c ] ||
    { echo "build.sh: base/ is empty: git submodule update --init --recursive"; exit 1; }
"$PY" -c 'import PIL' 2>/dev/null || { echo "build.sh: $PY has no Pillow (pip3 install Pillow)"; exit 1; }
case " $* " in
*" --host "* | *" --web "*) ;;
*)
    [ -x "$JIELI_TOOLCHAIN/pi32v2/bin/clang" ] || { echo "build.sh: no toolchain in $JIELI_TOOLCHAIN"; exit 1; }
    [ -f "$AC79_SDK/cpu/wl82/tools/uboot.boot" ] || { echo "build.sh: no JieLi AC79 SDK in $AC79_SDK"; exit 1; }
    if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
        docker info >/dev/null 2>&1 || { echo "build.sh: Docker is not running (open -g -a Docker)"; exit 1; }
    fi ;;
esac

# the patched copy: base/ as it is (only what changed is copied again), then the patches
B="$ROOT/build/fm1-base"
mkdir -p "$B"
rsync -a --delete --checksum --exclude .git base/ "$B/"
for p in firmware/patches/fm1-base/*.patch; do
    GIT_CEILING_DIRECTORIES="$ROOT/build" git -C "$B" apply "$ROOT/$p" ||
        { echo "build.sh: $p does not apply to base/ (fm1-base moved on?)"; exit 1; }
done
for p in firmware/patches/felucca-bsp/*.patch; do
    GIT_CEILING_DIRECTORIES="$ROOT/build" git -C "$B/bsp" apply "$ROOT/$p" ||
        { echo "build.sh: $p does not apply to base/bsp (felucca-bsp moved on?)"; exit 1; }
done
# the M-Vave SMK-37 (docs/SMK37.md): its own series on top, for fm1-base and felucca-bsp (firmware/patches/smk37/), and
# for the app: firmware/ is copied to build/app-smk37 and firmware/patches/smk37/app/*.patch applied there, so that
# firmware/'s sources stay as they are (fm1-base's build hashes the app's sources into the image, FM1B_BUILD: an edit
# of firmware/, even under #if BOARD_SMK37, would change the FM-1's packages). The SMK's packages and simulator are
# copied to firmware/build/. SMK_SOURCES=1 (BOARD unset): the FM-1 built from those same patched sources, into
# build/app-smk37/build/ (tests/run_tests.sh: its HELLO_R against the SMK-37's)
SMK=0
case "${BOARD:-fm1}" in
fm1) [ "${SMK_SOURCES:-0}" != 1 ] || SMK=1 ;;
smk37) SMK=1 ;;
*) echo "build.sh: BOARD=$BOARD: fm1 or smk37"; exit 1 ;;
esac
APPDIR=firmware
if [ $SMK = 1 ]; then
    [ $BENCH = 0 ] || { echo "build.sh: --bench is the FM-1's"; exit 1; }
    for p in firmware/patches/smk37/fm1-base/*.patch; do
        GIT_CEILING_DIRECTORIES="$ROOT/build" git -C "$B" apply "$ROOT/$p" ||
            { echo "build.sh: $p does not apply"; exit 1; }
    done
    for p in firmware/patches/smk37/felucca-bsp/*.patch; do
        GIT_CEILING_DIRECTORIES="$ROOT/build" git -C "$B/bsp" apply "$ROOT/$p" ||
            { echo "build.sh: $p does not apply"; exit 1; }
    done
    APPDIR="$ROOT/build/app-smk37"
    mkdir -p "$APPDIR" "$ROOT/build/proto"
    rsync -a --delete --checksum proto/ "$ROOT/build/proto/"   # (the app's "../../proto/rfm1.h", beside the copy)
    rsync -a --delete --checksum --exclude /build/ --exclude /patches/ firmware/ "$APPDIR/"
    for p in firmware/patches/smk37/app/*.patch; do
        GIT_CEILING_DIRECTORIES="$ROOT/build" git -C "$APPDIR" apply "$ROOT/$p" ||
            { echo "build.sh: $p does not apply to firmware/"; exit 1; }
    done
    if [ "${BOARD:-fm1}" = smk37 ]; then
        "$PY" "$B/tools/build.py" "$APPDIR" "$@"
        case " $* " in
        *" --web "*)                                    # the emulator: the wasm and fm1-base's worklet, with the
            E="firmware/build/emu-smk37"                # SMK-37's own page (web/emu-smk37/) over fm1-base's
            case "${MODEL:-pro}" in elite | mke) E="$E-$MODEL" ;; esac
            mkdir -p "$E"
            cp -p "$APPDIR/build/emu/app.wasm" "$APPDIR/build/emu/worklet.js" "$E/"
            for f in web/emu-smk37/*.html web/emu-smk37/*.js web/emu-smk37/*.css; do
                [ ! -f "$f" ] || cp -p "$f" "$E/"
            done
            echo "(the SMK-37's emulator in $E: serve that folder, e.g. python3 -m http.server -d $E 8037)"
            exit 0 ;;
        esac
        mkdir -p firmware/build/host
        for f in "$APPDIR"/build/remotefm1-smk37*.fwsc; do
            [ ! -f "$f" ] || cp -p "$f" firmware/build/
        done
        for f in "$APPDIR"/build/host/remotefm1-smk37*_host; do   # (a new file, not over the old one: macOS kills
            [ ! -f "$f" ] || { cp -p "$f" "firmware/build/host/.$(basename "$f")" &&  # a binary rewritten in place)
                               mv -f "firmware/build/host/.$(basename "$f")" "firmware/build/host/$(basename "$f")"; }
        done
        echo "(the SMK-37's packages and simulator copied to firmware/build/)"
        exit 0
    fi
    exec "$PY" "$B/tools/build.py" "$APPDIR" "$@"
fi
if [ $BENCH = 0 ]; then
    exec "$PY" "$B/tools/build.py" firmware "$@"
fi
case " $* " in *" --host "* | *" --web "* | *" --release "*) echo "build.sh: --bench is a device build only"; exit 1 ;; esac
stem=remotefm1
case " $* " in *" --no-ble "*) stem=remotefm1-usb ;; esac
[ ! -f firmware/build/$stem.fwsc ] || mv firmware/build/$stem.fwsc firmware/build/$stem.fwsc.keep
"$PY" "$B/tools/build.py" firmware "$@" || { [ ! -f firmware/build/$stem.fwsc.keep ] ||
    mv firmware/build/$stem.fwsc.keep firmware/build/$stem.fwsc; exit 1; }
mv firmware/build/$stem.fwsc firmware/build/$stem-bench.fwsc
[ ! -f firmware/build/$stem.fwsc.keep ] || mv firmware/build/$stem.fwsc.keep firmware/build/$stem.fwsc   # (the
                                                        # ordinary package of the last ordinary build stays)
echo "bench    $ROOT/firmware/build/$stem-bench.fwsc (SA_BENCH: the FM6 bench at boot)"
