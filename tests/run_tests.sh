#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 RemoteFM1 contributors
# RemoteFM1's tests without a device (nothing here opens a MIDI or USB port):
#   - the app in fm1-base's host simulator (./build.sh --host): the scripted sessions in firmware/tests/ over USB SysEx,
#     the vendor bulk pair and Bluetooth MIDI, the update's SysEx beside RemoteFM1's; again built USB-only (--no-ble)
#     and without the shadow frame buffer (RFM_SHADOW 0, the WiFi build's RAM);
#   - the protocol code alone: M0's framing, the TILES decoder (with and without the shadow) and the push, the
#     protocol layer in a scripted session;
#   - felucca-bsp's usb.c as RemoteFM1 patches it (its SysEx tap, macOS's CIN 0xF byte), and as everyone else builds it;
#   - fm1-base's Bluetooth MIDI SysEx as RemoteFM1 patches it (le_midi.c's parser and encoder);
#   - tools/fm1_install.py's package checks and identity names (the built packages too, when there).
#   tests/run_tests.sh          (host/run_tests.sh and the web/ tests are their own: README.md "Tests")
# Ends "ALL TESTS PASSED" (exit 0), or stops at the first failure.
set -e
cd "$(dirname "$0")/.."
H=build/host
mkdir -p "$H"
TCC="cc -std=c99 -Wall -Wextra -Wno-unused-function -Wno-sign-compare"
SAN="-O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
SIM=firmware/build/host
OUT="$H/sim"
mkdir -p "$OUT"
sim() {   # sim BINARY SCRIPT...
    b="$1"
    shift
    for t in "$@"; do
        echo "-- $t"
        "$b" "$t" "$OUT" > "$OUT/log.txt" 2>&1 || { cat "$OUT/log.txt"; echo "FAILED: $t"; exit 1; }
        if grep -q "FAIL\|EXPECTATIONS FAILED" "$OUT/log.txt"; then
            grep "FAIL" "$OUT/log.txt"
            echo "FAILED: $t"
            exit 1
        fi
        grep "^host:" "$OUT/log.txt"
    done
}

echo "== the app in fm1-base's host simulator: USB SysEx, bulk, Bluetooth, the update's SysEx beside (firmware/tests)"
env -u CC ./build.sh --host >/dev/null
sim "$SIM/remotefm1_host" firmware/tests/session.txt firmware/tests/bulk_ota.txt firmware/tests/ble.txt
echo "== the same, the USB-only build (--no-ble)"
env -u CC ./build.sh --host --no-ble >/dev/null
sim "$SIM/remotefm1-usb_host" firmware/tests/session.txt firmware/tests/bulk_ota.txt
echo "== without the shadow frame buffer (RFM_SHADOW 0: rectangles straight to the panel, no COPY)"
CC="$PWD/firmware/tests/cc_noshadow.sh" ./build.sh --host >/dev/null
sim "$SIM/remotefm1_host" firmware/tests/noshadow.txt
env -u CC ./build.sh --host >/dev/null                  # (the simulator as it should be left: with the shadow)

echo "== RemoteFM1's bench protocol (firmware/src/remote_proto.h; tools/m0_bench.py's codec against it)"
$TCC $SAN -Ifirmware/src -o "$H/proto_test" tests/proto_test.c
"$H/proto_test"
python3 tools/m0_bench.py --selftest
echo "== the TILES decoder, with and without the shadow, and the screen push (firmware/src/rfm_draw.c, rfm_push.c)"
$TCC $SAN -o "$H/rfm_draw_test" tests/rfm_draw_test.c
"$H/rfm_draw_test"
echo "== the protocol layer: a scripted session from both USB transports (firmware/src/rfm_app.c)"
$TCC $SAN -o "$H/session_test" tests/session_test.c
"$H/session_test"
echo "== felucca-bsp's usb.c, patched (build/fm1-base): M0's lost byte (CIN 0xF), the SysEx tap's claim, EP1 held"
B="-Ibuild/fm1-base/bsp/chip -Ibuild/fm1-base/bsp/boards/fm1 -Wno-int-to-pointer-cast"
$TCC $SAN $B -o "$H/usb_midi_test" tests/usb_midi_test.c
"$H/usb_midi_test"
$TCC $SAN $B -DWITH_CIN_F -o "$H/usb_midi_test_rf" tests/usb_midi_test.c
"$H/usb_midi_test_rf"
$TCC $SAN $B -DWITH_CIN_F -DFELUCCA_VENDOR=1 '-DFELUCCA_USB_PRODUCT="FM-1"' -o "$H/usb_midi_test_vendor" tests/usb_midi_test.c
"$H/usb_midi_test_vendor"
echo "== fm1-base's Bluetooth MIDI SysEx, patched (le_midi.c's parser and encoder)"
python3 - <<'EOF'
src = open("build/fm1-base/ble/midi/le_midi.c").read()
open("build/host/le_midi_part.c", "w").write(src[src.index("/* ---- MIDI in:"):src.index("/* ---- ATT */")])
EOF
$TCC $SAN -o "$H/ble_sysex_test" tests/ble_sysex_test.c
"$H/ble_sysex_test"
echo "== tools/fm1_install.py: packages, identities (M-VAVE's V15 / V16 too when found: V15_PKG, V16_PKG)"
python3 tests/install_test.py
if [ -f /opt/homebrew/include/libusb-1.0/libusb.h ]; then
    echo "== tools/m0_bulk.c builds (not run: it would open the unit)"
    cc -std=c99 -O2 -Wall -Wextra -Ifirmware/src -I/opt/homebrew/include/libusb-1.0 -o "$H/m0_bulk" tools/m0_bulk.c \
        -L/opt/homebrew/lib -lusb-1.0
else
    echo "(no libusb in /opt/homebrew: tools/m0_bulk.c not built)"
fi
echo "ALL TESTS PASSED"
