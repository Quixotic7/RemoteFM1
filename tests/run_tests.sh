#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 RemoteFM1 contributors
# RemoteFM1's tests without a device (nothing here opens a MIDI or USB port):
#   - the app in fm1-base's host simulator (./build.sh --host): the scripted sessions in firmware/tests/ over USB SysEx,
#     the vendor bulk pair and Bluetooth MIDI, the update's SysEx beside RemoteFM1's, TILES' HOLD; again built USB-only (--no-ble)
#     and without the shadow frame buffer (RFM_SHADOW 0, the WiFi build's RAM);
#   - the protocol code alone: M0's framing, the TILES decoder (with and without the shadow) and the push, the
#     protocol layer in a scripted session;
#   - felucca-bsp's usb.c as RemoteFM1 patches it (its SysEx tap, macOS's CIN 0xF byte), and as everyone else builds it;
#   - fm1-base's Bluetooth MIDI SysEx as RemoteFM1 patches it (le_midi.c's parser and encoder);
#   - tools/fm1_install.py's package checks and identity names (the built packages too, when there);
#   - the standalone DX7 (firmware/sa/sa_synth.c on saadet's FM6): its own checks, its bench over SysEx in the
#     simulator, and with SAADET=dir (a saadet checkout) the same renders as saadet's own (tests/fm6_parity.sh);
#   - the standalone instrument (firmware/sa/sa_main.c, STANDALONE-PLAN.md S1) in the simulator: the keys, MIDI out and
#     in, the octave, channel and voices, the sound under full scale, the settings kept, the hand-over to a host and
#     back; its screens shot to build/host/sim/sa_*.png; built with and without Yamaha's ROM cartridges; the chord names.
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

SA_TESTS="firmware/tests/standalone_cc.txt firmware/tests/standalone_loop.txt firmware/tests/standalone_arp.txt firmware/tests/standalone_s5.txt firmware/tests/standalone_remote.txt firmware/tests/standalone_menus.txt firmware/tests/standalone_latch.txt firmware/tests/standalone_settings.txt firmware/tests/standalone_scales.txt firmware/tests/standalone_return.txt firmware/tests/standalone_rec.txt firmware/tests/standalone_loops.txt firmware/tests/standalone_leds.txt firmware/tests/standalone_scale_look.txt firmware/tests/standalone_kled.txt"   # S2.. (the standalone's milestones)
echo "== the app in fm1-base's host simulator: USB SysEx, bulk, Bluetooth, the update's SysEx beside (firmware/tests)"
env -u CC ./build.sh --host >/dev/null
sim "$SIM/remotefm1_host" firmware/tests/session.txt firmware/tests/bulk_ota.txt firmware/tests/ble.txt \
    firmware/tests/hold.txt firmware/tests/sa_bench.txt firmware/tests/standalone.txt firmware/tests/standalone_ble.txt firmware/tests/trs.txt \
    $SA_TESTS
if grep -q "SA_ROM_NBANKS 8u" firmware/build/gen/dx7_rom.h; then
    sim "$SIM/remotefm1_host" firmware/tests/standalone_rom.txt
else
    echo "(no ROM cartridges in this build: firmware/tests/standalone_rom.txt not run; DX7_ROM_DIR)"
fi
echo "== the same, the USB-only build (--no-ble)"
env -u CC ./build.sh --host --no-ble >/dev/null
sim "$SIM/remotefm1-usb_host" firmware/tests/session.txt firmware/tests/bulk_ota.txt firmware/tests/hold.txt \
    firmware/tests/sa_bench.txt firmware/tests/standalone.txt firmware/tests/trs.txt $SA_TESTS
echo "== the bench build (SA_BENCH=1): the standalone DX7's bench at boot, up to 16 voices"
SA_BENCH=1 env -u CC ./build.sh --host >/dev/null
sim "$SIM/remotefm1_host" firmware/tests/sa_bench_boot.txt
echo "== without the shadow frame buffer (RFM_SHADOW 0: rectangles straight to the panel, no COPY)"
CC="$PWD/firmware/tests/cc_noshadow.sh" ./build.sh --host >/dev/null
sim "$SIM/remotefm1_host" firmware/tests/noshadow.txt firmware/tests/standalone.txt
echo "== without Yamaha's ROM cartridges (DX7_ROM_DIR somewhere empty): the build goes on, the INIT VOICE only"
DX7_ROM_DIR=/nonexistent env -u CC ./build.sh --host | grep "dx7 rom"
grep -q "SA_ROM_NBANKS 0u" firmware/build/gen/dx7_rom.h
sim "$SIM/remotefm1_host" firmware/tests/standalone.txt
env -u CC ./build.sh --host >/dev/null                  # (the simulator as it should be left: with the shadow)

echo "== RemoteFM1's bench protocol (firmware/src/remote_proto.h; tools/m0_bench.py's codec against it)"
$TCC $SAN -Ifirmware/src -o "$H/proto_test" tests/proto_test.c
"$H/proto_test"
python3 tools/m0_bench.py --selftest
echo "== the TILES decoder, with and without the shadow, and the screen push (firmware/src/rfm_draw.c, rfm_push.c)"
$TCC $SAN -o "$H/rfm_draw_test" tests/rfm_draw_test.c
"$H/rfm_draw_test"
echo "== the store's window: RemoteFM1 writes only 0xF2000..0xF3FFF, the calibration's 0xFC000..0xFDFFF and the"
echo "   user DX7 banks' 0x93000..0x96FFF"
echo "   (firmware/app.h APP_STORE_OK; fm1-base's st_erase / st_prog keep to it: patches/fm1-base/0006)"
$TCC -DPLAT_HOST -Ibuild/fm1-base/include -Ifirmware -o "$H/store_window_test" tests/store_window_test.c
"$H/store_window_test"
[ "$(grep -c 'if (!APP_STORE_OK(off' build/fm1-base/device/unity.c)" = 2 ] ||
    { echo "FAILED: fm1-base's st_erase / st_prog do not keep to APP_STORE_OK"; exit 1; }
if grep -rn "plat_data_erase\|plat_data_write\|fl_erase4k\|fl_write" firmware/src firmware/sa firmware/app.c |
    grep -v "^firmware/sa/sa_ubank.c:[0-9]*:    return plat_data_\(erase(off)\|write(off, src, n)\);$"; then
    echo "FAILED: the app writes the flash beside the store (and sa_ubank.c's two checked writers)"; exit 1
fi
[ "$(grep -c 'if (!APP_STORE_OK(off, ' firmware/sa/sa_ubank.c)" = 2 ] ||
    { echo "FAILED: sa_ubank.c's flash writes do not keep to APP_STORE_OK"; exit 1; }
echo "== the standalone's themes over every background readable (firmware/sa/sa_theme.h)"
$TCC $SAN -o "$H/sa_theme_test" tests/sa_theme_test.c
"$H/sa_theme_test"
echo "== the standalone's chord names (firmware/sa/sa_chord.c)"
$TCC $SAN -o "$H/sa_chord_test" tests/sa_chord_test.c
"$H/sa_chord_test"
echo "== the protocol layer: a scripted session from both USB transports (firmware/src/rfm_app.c)"
$TCC $SAN -o "$H/session_test" tests/session_test.c
"$H/session_test"
echo "== felucca-bsp's usb.c, patched (build/fm1-base): M0's lost byte (CIN 0xF), the SysEx tap's claim, EP1 held, the"
echo "   descriptors (0.1's; the vendor interface after USB audio in + out)"
B="-Ibuild/fm1-base/bsp/chip -Ibuild/fm1-base/bsp/boards/fm1 -Wno-int-to-pointer-cast"
$TCC $SAN $B -o "$H/usb_midi_test" tests/usb_midi_test.c
"$H/usb_midi_test"
$TCC $SAN $B -DWITH_CIN_F -o "$H/usb_midi_test_rf" tests/usb_midi_test.c
"$H/usb_midi_test_rf"
$TCC $SAN $B -DWITH_CIN_F -DFELUCCA_VENDOR=1 '-DFELUCCA_USB_PRODUCT="FM-1"' -o "$H/usb_midi_test_vendor" tests/usb_midi_test.c
"$H/usb_midi_test_vendor"
$TCC $SAN $B -DWITH_CIN_F -DFELUCCA_VENDOR=1 -DFELUCCA_UAC=1 -DFELUCCA_UAC_OUT=1 -DFELUCCA_UAC_48K=0 \
    '-DFELUCCA_USB_PRODUCT="FM-1"' -o "$H/usb_midi_test_vendor_uac" tests/usb_midi_test.c
"$H/usb_midi_test_vendor_uac"
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
echo "== the standalone DX7: sa_synth's checks (firmware/sa/sa_synth.c, saadet's FM6; the tables of the build above)"
$TCC $SAN -Wno-unused-but-set-variable -ffp-contract=off -DPLAT_HOST -Ifirmware/build/gen -Ifirmware/sa -Itests \
    -o "$H/sa_synth_test" tests/sa_synth_test.c firmware/sa/sa_synth.c -lm
mkdir -p "$H/sa"
for m in serial now late drop; do "$H/sa_synth_test" "$H/sa" $m; done
python3 firmware/tools/sa_bench.py --selftest | tail -2
python3 tools/sa_loops.py --selftest
echo "== the render hash: the simulator's (SA_BENCH 02, one core and two) against firmware/tools/sa_hash_host.c's"
cc -std=c99 -O2 -ffp-contract=off -w -DPLAT_HOST -DSA_POLY=8 -Ifirmware/build/gen -Ifirmware/sa -o "$H/sa_hash_host" \
    firmware/tools/sa_hash_host.c firmware/sa/sa_synth.c -lm
python3 - "$("$H/sa_hash_host" 1)" "$("$H/sa_hash_host" 2)" > "$H/sa_hash.txt" <<'PYEOF'
import sys
s = lambda line: int(line.split()[1], 16) - (1 << 32) * (int(line.split()[1], 16) >> 31)
print("# (made by tests/run_tests.sh: the host's hashes as the simulator's expect reads them)")
print("wait 3100\nrf send usb 09 02\nwait 3000\nexpect sa_hash_state 3")
print(f"expect sa_hash_one {s(sys.argv[1])}\nexpect sa_hash_two {s(sys.argv[2])}")
PYEOF
sim "$SIM/remotefm1_host" "$H/sa_hash.txt"
if [ -n "${SAADET:-}" ]; then
    echo "== the standalone DX7 against saadet's own (SAADET=$SAADET)"
    tests/fm6_parity.sh "$SAADET"
else
    echo "(no SAADET=dir: the FM6 parity with saadet's own instrument not run: tests/fm6_parity.sh)"
fi
echo "ALL TESTS PASSED"
