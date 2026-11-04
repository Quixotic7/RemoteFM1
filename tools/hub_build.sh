#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
# The signed release build (docs/RELEASING.md). The Firmware Hub's shared workflow (zvenson/fm1-build@v1) runs it as its
# build command on GitHub's Ubuntu runner (.github/workflows/release.yml); it runs the same way on a Mac (Docker).
#   tools/hub_build.sh [VERSION]          build VERSION's two packages: the version from the argument, else from the tag
#                                         (vX.Y.Z), else (a run from the Actions tab) the run's version input
#   tools/hub_build.sh --version [V]      print version=V (release.yml's output)
#   tools/hub_build.sh --check DIR [V]    check remote-V.fwsc and remote-usb-V.fwsc in DIR against
#                                         web/installer/releases.json and the committed web/installer/firmware/, no build
#   tools/hub_build.sh --site [V]         build, then copy both packages into web/installer/firmware/ and add them to its
#                                         SHA256SUMS (the release's first step on the Mac; releases.json is by hand)
# The two packages, as the site has always named them:
#   remote-V.fwsc       USB + Bluetooth MIDI (./build.sh --release V): the one The Firmware Hub lists
#   remote-usb-V.fwsc   USB only (./build.sh --release V --no-ble)
# Both install as FM-1_52XXYYZZ (firmware/fm1.toml id_prefix).
#
# Yamaha's DX7 ROM cartridges (the standalone's 256 voices) are never in this repository or a release: the build
# downloads the eight 32-voice dumps from Yamaha Black Boxes and stops unless each one's sha256 is the one pinned below
# (the same files the owner's Presets/DX7-ROM holds, so the site's packages and the signed ones are the same bytes).
# firmware/tools/gen_dx7_rom.py packs them into build/gen/dx7_rom.h; the .syx files stay in firmware/build/dx7-rom/.
#
# The version must be listed in web/installer/releases.json, with both sha256s, before the tag (on a tag it is an
# error otherwise): each package's sha256 is checked against releases.json and the committed web/installer/firmware/
# before anything is signed, so the signed files are byte for byte the ones the web installer serves.
set -euo pipefail
cd "$(dirname "$0")/.."

PILLOW_VERSION=11.1.0           # the Mac's (fm1-base draws the UI fonts with Pillow: another version may draw otherwise)
ROM_URL=https://yamahablackboxes.com/patches/dx7/factory
ROM_SHA256=(                    # rom1a.syx .. rom4b.syx, 4,104 bytes each (checked again by gen_dx7_rom.py)
    rom1a:91416e81d0fad931f6c7b5dc5bcfd7b7c48f5340b3753b7d3bf99f439fdd104d
    rom1b:15c817360f56f7bd0e269575291cbfac91a45f0ee6125479abfdc4d69a560b4b
    rom2a:5254f04b674aa872e8f09a8b5828cc5454f79495e4aa5bbba8a3cac3c606a699
    rom2b:1efb00f24e28df03470ebca19cadd63cb98b5f099b5e7502dd6f232ddb23af54
    rom3a:157b71a53a9881c8209bb5de57f6d9519d5f43cb7cae591c4606e7fb52ff9d86
    rom3b:0f21e5f28f0b6c77ef5cef839bb244d8946b2ee2243e88e1ba19f46c4aa85e30
    rom4a:b34f01b5ee8690e5b45351a960eed3dea2fdd72318b419303d23f07d5e411fda
    rom4b:d4eae13570569e27894a21593a9c591a474a1b945c13c3a0a933c42dee684c0b
)

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

mode=build
case "${1:-}" in
    --version) mode=version; shift ;;
    --check) mode=check; dir="${2:?--check DIR}"; shift 2 ;;
    --site) mode=site; shift ;;
esac

# the version: the argument, the tag, or the version input of a run from the Actions tab (read from the event file,
# never put into a command line)
if [ -n "${1:-}" ]; then
    v="$1"
elif [ "${GITHUB_REF_TYPE:-}" = tag ]; then
    v="${GITHUB_REF_NAME#v}"
elif [ "${GITHUB_EVENT_NAME:-}" = workflow_dispatch ]; then
    v="$(python3 -c 'import json, os; print(json.load(open(os.environ["GITHUB_EVENT_PATH"]))["inputs"]["version"])')"
else
    echo "hub_build.sh: no version (an argument X.Y.Z, a tag vX.Y.Z or a run's version input)" >&2
    exit 1
fi
# X.Y.Z, two digits at most each: the identity is FM-1_52 + XXYYZZ
if ! [[ "$v" =~ ^[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}$ ]]; then
    echo "::error::version '$v' is not X.Y.Z (tags are vX.Y.Z)"
    exit 1
fi
if [ "$mode" = version ]; then
    echo "version=$v"
    exit 0
fi

# both packages in $1 (remote-V.fwsc, remote-usb-V.fwsc) against web/installer/releases.json and the committed
# web/installer/firmware/: a version not listed is an error on a tag, a notice otherwise
check() {
    python3 - "$v" "$1" <<'PY'
import hashlib, json, os, sys
from pathlib import Path
v, d = sys.argv[1], Path(sys.argv[2])
names = [f"remote-{v}.fwsc", f"remote-usb-{v}.fwsc"]
got = {}
for n in names:
    if not (d / n).is_file():
        sys.exit(f"::error::{d} has no {n}")
    got[n] = hashlib.sha256((d / n).read_bytes()).hexdigest()
entry = next((r for r in json.loads(Path("web/installer/releases.json").read_text())["releases"]
              if r.get("version") == v), None)
if entry is None:
    if os.environ.get("GITHUB_REF_TYPE") == "tag":
        sys.exit(f"::error::{v} is not in web/installer/releases.json: list it (docs/RELEASING.md), then tag")
    print(f"::notice::{v} is not in web/installer/releases.json: built, not a release (the installer will not offer it)")
    for n in names:
        print(f"  {got[n]}  {n}")
    sys.exit(0)
want = {names[0]: entry.get("sha256"), names[1]: (entry.get("usbOnly") or {}).get("sha256")}
if (entry.get("usbOnly") or {}).get("file") != names[1]:
    sys.exit(f"::error::releases.json's {v}: usbOnly.file is not {names[1]}")
site = Path("web/installer/firmware")
sums = (site / "SHA256SUMS").read_text()
bad = [n for n in names if got[n] != want[n]]
bad += [f"{n} (web/installer/firmware/)" for n in names
        if not (site / n).is_file() or hashlib.sha256((site / n).read_bytes()).hexdigest() != got[n]]
bad += [f"{n} (web/installer/firmware/SHA256SUMS)" for n in names if f"{got[n]}  {n}" not in sums]
if bad:
    sys.exit(f"::error::{', '.join(bad)}: not the sha256 releases.json and web/installer/firmware/ hold for {v} "
             "(or missing): this build is not the release the web installer serves")
for n in names:
    print(f"  {got[n]}  {n}")
print(f"web/installer/releases.json and web/installer/firmware/: {v}'s two sha256s match")
PY
}
if [ "$mode" = check ]; then
    check "$dir"
    exit 0
fi

PY="${PYTHON:-python3}"
# GitHub's runner: Raqm's libraries (Pillow's wheels load them at run time), the pinned Pillow (the shared workflow
# installs the newest) and fm1-base with felucca-bsp (the shared workflow's checkout fetches no submodules; the site's
# submodules under web/vendor are not needed)
if [ "${GITHUB_ACTIONS:-}" = true ]; then
    sudo apt-get update -q
    sudo apt-get install -y -q --no-install-recommends libfribidi0 libraqm0 rsync
    "$PY" -m pip install --disable-pip-version-check -q "Pillow==$PILLOW_VERSION"
    git submodule update --init --recursive base
elif [ ! -f base/tools/build.py ] || [ ! -f base/bsp/chip/usb.c ]; then
    git submodule update --init --recursive base
fi
git submodule status --recursive base
# (the Mac: Pillow finds Homebrew's libraqm, as build.sh's DYLD_LIBRARY_PATH does for the build)
if [ "$(uname -s)" = Darwin ] && [ -z "${DYLD_LIBRARY_PATH:-}" ] && [ -d /opt/homebrew/lib ]; then
    export DYLD_LIBRARY_PATH=/opt/homebrew/lib
fi
"$PY" - "$PILLOW_VERSION" <<'PY'
import sys
import PIL
from PIL import features
print(f"Python {sys.version.split()[0]}, Pillow {PIL.__version__} (raqm {features.version('raqm')})")
if not features.check("raqm"):
    sys.exit("hub_build.sh: Pillow has no Raqm layout (libfribidi or libraqm missing?): the fonts could differ")
if PIL.__version__ != sys.argv[1]:
    sys.exit(f"hub_build.sh: the release builds use Pillow {sys.argv[1]}, not {PIL.__version__} (PYTHON=... for "
             "another python3)")
PY

# Yamaha's ROM cartridges: a file already here or in DX7_ROM_DIR (the owner's Presets/DX7-ROM) is used when its
# sha256 is the pinned one, else it is downloaded; any other file stops the build
rom=firmware/build/dx7-rom
mkdir -p "$rom"
for e in "${ROM_SHA256[@]}"; do
    b="${e%%:*}" want="${e#*:}"
    f="$rom/$b.syx"
    if [ ! -f "$f" ] || [ "$(sha256 "$f")" != "$want" ]; then
        if [ -n "${DX7_ROM_DIR:-}" ] && [ -f "$DX7_ROM_DIR/$b.syx" ] && [ "$(sha256 "$DX7_ROM_DIR/$b.syx")" = "$want" ]; then
            cp "$DX7_ROM_DIR/$b.syx" "$f"
        else
            curl -fsSL --retry 3 -o "$f" "$ROM_URL/$b.syx"
        fi
    fi
    if [ "$(sha256 "$f")" != "$want" ]; then
        echo "::error::$b.syx from $ROM_URL is not the pinned file (sha256 $(sha256 "$f"))"
        exit 1
    fi
done
export DX7_ROM_DIR="$PWD/$rom"
echo "DX7 ROM: the eight cartridges, sha256s as pinned"

ulimit -n 8192 || true          # (the JieLi linker opens many files)
out="firmware/build/hub-$v"
mkdir -p "$out"
rm -f "$out"/*
./build.sh --release "$v"
cp firmware/build/remotefm1-"$v".fwsc "$out/remote-$v.fwsc"
./build.sh --release "$v" --no-ble
cp firmware/build/remotefm1-usb-"$v".fwsc "$out/remote-usb-$v.fwsc"
grep -q "define SA_ROM_NBANKS 8u" firmware/build/gen/dx7_rom.h ||
    { echo "::error::the build does not have the eight DX7 ROM cartridges (firmware/build/gen/dx7_rom.h)"; exit 1; }
(cd "$out" && for f in remote-*.fwsc; do echo "$(sha256 "$f")  $f"; done > SHA256SUMS && cat SHA256SUMS)

if [ "$mode" = site ]; then
    for n in "remote-$v.fwsc" "remote-usb-$v.fwsc"; do
        cp "$out/$n" web/installer/firmware/
        grep -v "  $n\$" web/installer/firmware/SHA256SUMS > "$out/sums.tmp" || true
        echo "$(sha256 "$out/$n")  $n" >> "$out/sums.tmp"
        mv "$out/sums.tmp" web/installer/firmware/SHA256SUMS
    done
    echo "web/installer/firmware/: remote-$v.fwsc and remote-usb-$v.fwsc copied, SHA256SUMS updated;"
    echo "now list $v in web/installer/releases.json with these sha256s (docs/RELEASING.md)"
    exit 0
fi
check "$out"

# what the shared workflow signs and releases (its "packages": remote-*.fwsc), in the root; on GitHub's runner only
if [ "${GITHUB_ACTIONS:-}" = true ]; then
    rm -f remote-*.fwsc
    cp "$out/remote-$v.fwsc" "$out/remote-usb-$v.fwsc" .
    ls -l remote-*.fwsc
fi
