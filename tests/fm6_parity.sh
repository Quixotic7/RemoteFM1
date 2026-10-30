#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
# RemoteFM1's FM6 (firmware/sa/sa_synth.c on saadet's FM6, firmware/third_party/saadet-fm6) against saadet's own:
#   1. the tables: firmware/tools/gen_fm6_tables.py's values against those of saadet's own generator, every one
#      (tests/fm6_tables_dump.c; the sine after its fill at start-up);
#   2. the sound: tests/sa_scenarios.h's ten performances (six voices: INIT and five made for this, every algorithm
#      family, feedback, the LFO's six waves, pitch EG, key and rate scaling, velocity, bend, the wheel, foot, breath,
#      aftertouch, portamento and glissando, more keys than voices, edits and loads while notes sound) rendered by
#      sa_synth (tests/sa_synth_test.c, SA_POLY 16) and by saadet's instrument itself (tests/fm6_ref.c: its voice.c,
#      its eng_fm6.c, its tables), compared sample for sample;
#   3. sa_synth's own checks at the default polyphony (tests/sa_synth_test.c, SA_POLY 8).
#   tests/fm6_parity.sh SAADET_DIR      a saadet checkout (https://github.com/keremimo/saadet, 2eff7f1); nothing is
#                                       written into it. Out: build/host/fm6/ (ours/, ref/: NAME.raw int32, NAME.wav)
# saadet's generators (GPL-3.0-only) are run, never read or copied: their output stays in build/host/fm6/saadet_gen.
# Ends "fm6 parity: PASS" (exit 0) or "fm6 parity: FAIL" (exit 1).
set -e
cd "$(dirname "$0")/.."
SAADET="${1:?usage: tests/fm6_parity.sh SAADET_DIR}"
[ -f "$SAADET/firmware/src/eng_fm6.c" ] || { echo "fm6 parity: $SAADET is not a saadet checkout"; exit 2; }
O=build/host/fm6
mkdir -p "$O/gen" "$O/saadet_gen" "$O/ours" "$O/ref" "$O/ours8"
CC="${HOST_CC:-cc}"
FL="-O2 -ffp-contract=off"            # (as fm1-base builds its host simulator: no fused multiply-add)
PY="${PYTHON:-python3}"
if [ -z "${DYLD_LIBRARY_PATH:-}" ] && [ -d /opt/homebrew/lib ]; then
    export DYLD_LIBRARY_PATH=/opt/homebrew/lib
fi
rev=$(git -C "$SAADET" rev-parse --short=7 HEAD 2>/dev/null || echo "?")
[ "$rev" = 2eff7f1 ] || echo "fm6 parity: note: saadet at $rev, RemoteFM1 vendored 2eff7f1"

echo "== the tables"
"$PY" firmware/tools/gen_fm6_tables.py --out "$O/gen/fm6_tables.h" >/dev/null
for g in gen_tables.py:melodee_tables.h gen_fm6_patches.py:melodee_fm6.h gen_cz1_factory.py:melodee_cz1.h \
         gen_j106_factory.py:melodee_j106.h gen_prophet_factory.py:melodee_prophet_factory.h; do
    "$PY" "$SAADET/tools/${g%%:*}" "$O/saadet_gen/${g#*:}" >/dev/null
done
$CC -std=c99 -Wall $FL -include "$O/gen/fm6_tables.h" -DTABLES_INIT=fm6_tables_init -o "$O/dump_ours" \
    tests/fm6_tables_dump.c
$CC -std=c99 -w $FL '-Dsection(x)=unused' -include "$O/saadet_gen/melodee_tables.h" -DTABLES_INIT=dsp_tables_init \
    -o "$O/dump_saadet" tests/fm6_tables_dump.c
"$O/dump_ours" > "$O/tables_ours.txt"
"$O/dump_saadet" > "$O/tables_saadet.txt"
tables=PASS
cmp -s "$O/tables_ours.txt" "$O/tables_saadet.txt" || tables=FAIL
echo "tables: $(wc -l < "$O/tables_ours.txt" | tr -d ' ') values, $tables"
[ $tables = PASS ] || diff "$O/tables_ours.txt" "$O/tables_saadet.txt" | head -20

echo "== the sound: sa_synth (SA_POLY 16) and saadet's instrument"
$CC -std=c99 -w $FL -DPLAT_HOST -DSA_POLY=16 -I"$O/gen" -Ifirmware/sa -Itests -o "$O/sa_synth_test16" \
    tests/sa_synth_test.c firmware/sa/sa_synth.c -lm
"$O/sa_synth_test16" "$O/ours"
$CC -std=gnu11 -w $FL '-Dsection(x)=unused' -I"$SAADET/firmware/src" -I"$O/saadet_gen" -Itests -o "$O/fm6_ref" \
    tests/fm6_ref.c -lm
"$O/fm6_ref" "$O/ref"
sound=PASS
"$PY" - "$O" <<'EOF' || sound=FAIL
import struct, sys
from pathlib import Path
o = Path(sys.argv[1])
bad = 0
for ref in sorted((o / "ref").glob("*.raw")):
    a, b = (o / "ours" / ref.name).read_bytes(), ref.read_bytes()
    x, y = struct.unpack(f"<{len(a) // 4}i", a), struct.unpack(f"<{len(b) // 4}i", b)
    diff = [abs(p - q) for p, q in zip(x, y) if p != q]
    peak = max(abs(v) for v in y)
    ok = len(x) == len(y) and not diff and peak > 1 << 20
    bad += not ok
    print(f"  {ref.stem:12} {len(y):7} samples, peak {peak / (1 << 23):5.2f} of 24-bit full scale: " +
          ("identical" if not diff and len(x) == len(y) else
           f"{len(diff)} differ (max {max(diff or [0])}), lengths {len(x)} / {len(y)}") + ("" if ok else "  FAIL"))
sys.exit(bad != 0)
EOF
echo "sound: $sound"

echo "== sa_synth's own checks (SA_POLY 8)"
own=PASS
$CC -std=c99 -Wall -Wno-unused-function $FL -DPLAT_HOST -I"$O/gen" -Ifirmware/sa -Itests -o "$O/sa_synth_test" \
    tests/sa_synth_test.c firmware/sa/sa_synth.c -lm 2>"$O/warnings.txt" || own=FAIL
"$O/sa_synth_test" "$O/ours8" || own=FAIL

if [ $tables = PASS ] && [ $sound = PASS ] && [ $own = PASS ]; then
    echo "fm6 parity: PASS"
else
    echo "fm6 parity: FAIL (tables $tables, sound $sound, sa_synth $own)"
    exit 1
fi
