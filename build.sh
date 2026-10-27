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
#   JIELI_TOOLCHAIN  JieLi's Linux toolchain (default ~/.jieli/toolchain; on a Mac it runs in Docker)
#   AC79_SDK         JieLi's AC79 SDK, for its Bluetooth libraries (default ~/fw-AC79_AIoT_SDK; never committed)
#   PYTHON           a Python whose Pillow has libraqm (fm1-base's fonts); on a Mac DYLD_LIBRARY_PATH=/opt/homebrew/lib
set -e
cd "$(dirname "$0")"
ROOT="$PWD"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
if [ -z "${DYLD_LIBRARY_PATH:-}" ] && [ -d /opt/homebrew/lib ]; then
    export DYLD_LIBRARY_PATH=/opt/homebrew/lib          # (Homebrew's libraqm for Pillow)
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
exec "$PY" "$B/tools/build.py" firmware "$@"
