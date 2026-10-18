#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The host tests: an ASan + UBSan build in host/build-asan, then ctest. Nothing here opens a real device: the
# session tests use the fake unit, test_coremidi uses virtual endpoints of its own, fm1remote runs without --device.
# test_cross (ctest "cross") runs the session against the device's own protocol code (firmware/src/rfm_app.c,
# rfm_draw.c, rfm_push.c) compiled in, on a simulated clock and link.
set -e
cd "$(dirname "$0")"
cmake -S . -B build-asan -DRFM_SANITIZE=ON -DRFM_FM1REMOTE=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build-asan -j 8
cd build-asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ctest --output-on-failure "$@"
