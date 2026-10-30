#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
# Portions derived from MSFA (Apache-2.0): Copyright 2012-2013 Google Inc.; Copyright 2016-2025 Pascal Gauthier;
# Copyright 2019 Jean Pierre Cimalando. Licence: firmware/third_party/saadet-fm6/LICENSES/Apache-2.0-msfa.txt
"""The tables saadet's FM6 (firmware/third_party/saadet-fm6) reads, as one C header.

  firmware/tools/gen_fm6_tables.py --out build/gen/fm6_tables.h      (fm1.toml `gen`: every build)

MSFA (the DX7 core of Google's music-synthesizer-for-android, carried on in Dexed's Source/msfa) computes these
tables at start-up; this script computes the same ones, the same way, once. Each table names the Apache-2.0 file it
follows (the files and revisions: firmware/third_party/saadet-fm6/PROVENANCE.md):

  FM6_FREQ     freqlut.cc Freqlut::init: phase step of a log frequency, 1024 steps an octave (Google, f67d41d)
  FM6_SIN      sin.cc Sin::init: the integer rotation, as float cycles of modulation; filled by
               fm6_tables_init() at boot (RAM: it is read per sample), not stored in the flash (Google, f67d41d)
  FM6_COARSE   dx7note.cc coarsemul: log2 of the coarse ratios 0.5, 1 .. 31 (Google, f67d41d)
  FM6_FINE     dx7note.cc osc_freq: log2(1 + FINE / 100) (Google, f67d41d)
  FM6_DETUNE   dx7note.cc osc_freq: Dexed's note-dependent detune, per note, x 2^24 (Dexed fea2f0f)
  FM6_LFO_INC  lfo.cc Lfo::reset: Dexed's measured LFO rates, phase per 64-sample block (Dexed fea2f0f)
  FM6_LFO_UNIT lfo.cc Lfo::init: the delay ramp's unit (Google, f67d41d)
  FM6_PEG_UNIT pitchenv.cc PitchEnv::init: the pitch envelope's unit (Google, f67d41d)
  FM6_PORTA    porta.cpp Porta::init_sr: portamento per block, and FM6_GLISS with glissando (Dexed fea2f0f)

The values equal those of saadet's own generator (tests/fm6_parity.sh compares every value, SIN after its fill).
Python's floats are IEEE doubles and math.* is the C library's, as in MSFA's C++.
"""
import argparse
import math
import struct
from pathlib import Path

FS = 44100.0                            # the FM-1's rate
LG_N = 6                                # MSFA's block: 64 samples (synth.h LG_N)
N = 1 << LG_N

# Dexed lfo.cc lfoSource: the DX7's LFO rates 0..99 in Hz, measured (Apache-2.0, Copyright 2013 Google Inc.)
LFO_SOURCE = [
    0.062541, 0.125031, 0.312393, 0.437120, 0.624610, 0.750694, 0.936330, 1.125302, 1.249609, 1.436782,
    1.560915, 1.752081, 1.875117, 2.062494, 2.247191, 2.374451, 2.560492, 2.686728, 2.873976, 2.998950,
    3.188013, 3.369840, 3.500175, 3.682224, 3.812065, 4.000800, 4.186202, 4.310716, 4.501260, 4.623209,
    4.814636, 4.930480, 5.121901, 5.315191, 5.434783, 5.617346, 5.750431, 5.946717, 6.062811, 6.248438,
    6.431695, 6.564264, 6.749460, 6.868132, 7.052186, 7.250580, 7.375719, 7.556294, 7.687577, 7.877738,
    7.993605, 8.181967, 8.372405, 8.504848, 8.685079, 8.810573, 8.986341, 9.122423, 9.300595, 9.500285,
    9.607994, 9.798158, 9.950249, 10.117361, 11.251125, 11.384335, 12.562814, 13.676149, 13.904338, 15.092062,
    16.366612, 16.638935, 17.869907, 19.193858, 19.425019, 20.833333, 21.034918, 22.502250, 24.003841, 24.260068,
    25.746653, 27.173913, 27.578599, 29.052876, 30.693677, 31.191516, 32.658393, 34.317090, 34.674064, 36.416606,
    38.197097, 38.550501, 40.387722, 40.749796, 42.625746, 44.326241, 44.883303, 46.772685, 48.590865, 49.261084,
]


def f32(x):
    """x rounded to a C float"""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def freq():
    """freqlut.cc: lut[i] = floor(y + 0.5), y from 2^44 / FS times 2^(1/1024) per step (MAX_LOGFREQ_INT 20)"""
    y, inc, out = float(1 << (24 + 20)) / FS, math.pow(2.0, 1.0 / 1024), []
    for _ in range(1025):
        out.append(int(math.floor(y + 0.5)))
        y *= inc
    return out


def sin_consts():
    """sin.cc: the rotation by 2 pi / 1024 in Q30"""
    dphase = 2 * math.pi / 1024
    return int(math.floor(math.cos(dphase) * (1 << 30) + 0.5)), int(math.floor(math.sin(dphase) * (1 << 30) + 0.5))


def coarse():
    """dx7note.cc coarsemul: (1 << 24) * log2(ratio), ratio 0.5 for COARSE 0"""
    return [int(math.floor(math.log2(0.5 if k == 0 else k) * (1 << 24) + 0.5)) for k in range(32)]


def fine():
    """dx7note.cc osc_freq: floor(24204406.323123 * log(1 + 0.01 * FINE) + 0.5), (1 << 24) / ln 2 the constant"""
    return [int(math.floor(24204406.323123 * math.log(1 + 0.01 * f) + 0.5)) for f in range(128)]


def detune():
    """Dexed dx7note.cc osc_freq: logfreq += detuneRatio * logfreq * (DETUNE - 7), with
    detuneRatio = 0.0209 * exp(-0.396 * ((float)logfreq / (1 << 24))) / 7 and logfreq the note's
    (midinote_to_logfreq: 50857777 + ((1 << 24) / 12) * note). Per note: detuneRatio * logfreq * 2^24, rounded,
    which FM6 multiplies by (DETUNE - 7) and shifts down by 24"""
    out = []
    for note in range(128):
        lf = 50857777 + ((1 << 24) // 12) * note
        ratio = 0.0209 * math.exp(-0.396 * f32(f32(float(lf)) / (1 << 24))) / 7
        out.append(int(math.floor(ratio * lf * (1 << 24) + 0.5)))
    return out


def lfo_inc():
    """Dexed lfo.cc: delta_ = lfoSource[rate] * lforatio_ (uint32_t), lforatio_ = 4437500000.0 * N / FS (uint32_t)"""
    ratio = int(4437500000.0 * N / FS)
    return [int(src * ratio) for src in LFO_SOURCE]


def porta(semitones_per_s):
    """porta.cpp Porta::init_sr: per 64-sample block, in Q24 octaves: (int32_t)(0.5f + step * spp),
    step = (1 << 24) / 12, spp = sps / FS * N, sps = rate * 2^(-0.062 * CC)"""
    step, out = (1 << 24) // 12, []
    for i in range(128):
        spp = semitones_per_s * math.pow(2.0, -0.062 * i) / FS * N
        out.append(int(0.5 + step * spp))
    return out


def c_array(ctype, name, values, per_line=8):
    lines = [f"static const {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    return lines + ["};"]


def header():
    c, s = sin_consts()
    lfo_unit = int(N * 25190424 / FS + 0.5)                  # lfo.cc Lfo::init
    peg_unit = int(N * (1 << 24) / (21.3 * FS) + 0.5)         # pitchenv.cc PitchEnv::init
    out = [
        "/* SPDX-License-Identifier: MPL-2.0",
        " * Copyright (C) 2026 RemoteFM1 contributors",
        " * Generated by firmware/tools/gen_fm6_tables.py from MSFA's formulas (Apache-2.0: Copyright 2012-2013",
        " * Google Inc., 2016-2025 Pascal Gauthier, 2019 Jean Pierre Cimalando). Do not edit. */",
        "#pragma once",
        "#include <stdint.h>",
        f"#define FM6_FS {int(FS)}",
        f"#define FM6_N {N}                         /* MSFA's block (the controls run once per FM6_N samples) */",
        f"#define FM6_LG_N {LG_N}",
        f"#define FM6_LFO_UNIT {lfo_unit}",
        f"#define FM6_PEG_UNIT {peg_unit}",
        f"#define FM6_SIN_C {c}               /* cos and sin of 2 pi / 1024, Q30 (sin.cc) */",
        f"#define FM6_SIN_S {s}",
        *c_array("int32_t", "FM6_FREQ", freq()),
        *c_array("int32_t", "FM6_COARSE", coarse()),
        *c_array("int32_t", "FM6_FINE", fine()),
        *c_array("int64_t", "FM6_DETUNE", [f"{v}ll" for v in detune()], 6),
        *c_array("uint32_t", "FM6_LFO_INC", [f"{v}u" for v in lfo_inc()]),
        *c_array("int32_t", "FM6_PORTA", porta(2100.0)),
        *c_array("int32_t", "FM6_GLISS", porta(1300.0)),
        "/* the sine, 1024 points and the closing 0, in cycles of modulation: filled by fm6_tables_init() before the",
        " * first render (sin.cc Sin::init's integer rotation; FM6_SIN_ATTR places it, e.g. in RAM) */",
        "#ifndef FM6_SIN_ATTR",
        "#define FM6_SIN_ATTR",
        "#endif",
        "static float FM6_SIN[1025] FM6_SIN_ATTR;",
        "static void fm6_tables_init(void)",
        "{",
        "    int32_t u = 1 << 30, v = 0, x;",
        "    uint32_t i;",
        "    for (i = 0; i < 512u; i++) {",
        "        x = (v + 32) >> 6;",
        "        FM6_SIN[i] = (float)x * (1.0f / 16777216.0f);",
        "        FM6_SIN[i + 512u] = (float)-x * (1.0f / 16777216.0f);",
        "        x = (int32_t)(((int64_t)u * FM6_SIN_S + (int64_t)v * FM6_SIN_C + (1 << 29)) >> 30);",
        "        u = (int32_t)(((int64_t)u * FM6_SIN_C - (int64_t)v * FM6_SIN_S + (1 << 29)) >> 30);",
        "        v = x;",
        "    }",
        "    FM6_SIN[1024] = 0.0f;",
        "}",
    ]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    text = header()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    if not a.out.exists() or a.out.read_text() != text:   # (unchanged: the build's dependants stay as they are)
        a.out.write_text(text)
    print(f"fm6 tables: {a.out}")


if __name__ == "__main__":
    main()
