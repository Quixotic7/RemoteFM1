/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The render hash: a fixed performance on sa_synth, its samples hashed, the same on the FM-1 (sa_bench.c, SA_BENCH
 * 02) and on the computer (firmware/tools/sa_hash_host.c): equal hashes say the FM-1's FPU and compiler give the
 * computer's samples, bit for bit. Three seconds: two voices of every kind of work (algorithms 5 and 22, feedback,
 * the LFO's sine and sample and hold on pitch and amplitude, the pitch EG, velocity and key scaling, OSC KEY SYNC off,
 * a transpose), chords, bend, the wheel and aftertouch, portamento, a voice loaded mid-way, more keys than SA_POLY.
 * FNV-1a over each sample's four bytes, little endian (sa_synth_render_mono's int32, before any gain). Include after
 * sa_synth.h; sa_hash_block is called with b = 0, 1, .. SA_HASH_BLOCKS - 1 after sa_synth_init. */
#pragma once
#include <stdint.h>
#include "sa_synth.h"

#define SA_HASH_BLOCKS (3u * SA_FS / SA_CTL)
#define SA_HASH_SEED 2166136261u

#define SAH_OP(r1, r2, r3, r4, l1, l2, l3, l4, bp, ld, rd, lc, rc, rs, ams, kvs, ol, mode, fc, ff, det)               \
    r1, r2, r3, r4, l1, l2, l3, l4, bp, ld, rd, lc, rc, rs, ams, kvs, ol, mode, fc, ff, det
static const uint8_t SA_HASH_VOICES[2][SA_VCED_LEN] = {
    {SAH_OP(95, 50, 35, 78, 99, 75, 0, 0, 60, 0, 50, 0, 1, 3, 2, 6, 72, 0, 14, 0, 7),
     SAH_OP(95, 29, 20, 50, 99, 95, 0, 0, 39, 0, 0, 0, 0, 3, 0, 0, 99, 0, 1, 0, 4),
     SAH_OP(95, 20, 20, 50, 99, 95, 0, 0, 48, 20, 30, 1, 1, 2, 1, 7, 78, 1, 2, 35, 10),
     SAH_OP(95, 29, 20, 50, 99, 95, 0, 0, 39, 0, 0, 0, 0, 3, 3, 2, 99, 0, 1, 0, 7),
     SAH_OP(95, 50, 35, 78, 99, 75, 0, 0, 60, 0, 70, 3, 1, 4, 0, 5, 80, 0, 1, 5, 7),
     SAH_OP(96, 25, 25, 67, 99, 75, 0, 0, 39, 0, 0, 0, 0, 3, 2, 2, 99, 0, 1, 0, 6),
     94, 67, 95, 60, 50, 50, 50, 50, 4, 6, 0, 34, 33, 30, 25, 0, 4, 3, 24, 'H', 'A', 'S', 'H', ' ', 'E', 'P', ' ', ' ', ' '},
    {SAH_OP(70, 45, 40, 60, 99, 90, 85, 0, 45, 40, 60, 3, 1, 4, 0, 4, 88, 0, 1, 0, 7),
     SAH_OP(65, 40, 40, 55, 99, 92, 88, 0, 39, 0, 0, 0, 0, 2, 1, 0, 99, 0, 1, 0, 8),
     SAH_OP(65, 40, 40, 55, 99, 92, 88, 0, 39, 0, 0, 0, 0, 2, 0, 0, 99, 0, 1, 0, 6),
     SAH_OP(65, 40, 40, 55, 99, 92, 88, 0, 39, 0, 0, 0, 0, 2, 0, 0, 99, 0, 1, 0, 7),
     SAH_OP(72, 50, 40, 60, 99, 88, 80, 0, 50, 50, 30, 1, 2, 3, 0, 6, 80, 0, 1, 0, 7),
     SAH_OP(66, 40, 40, 56, 99, 93, 88, 0, 39, 0, 0, 0, 0, 2, 0, 1, 99, 0, 1, 0, 7),
     60, 50, 40, 70, 40, 52, 50, 50, 21, 7, 1, 45, 0, 10, 20, 1, 5, 6, 26, 'H', 'A', 'S', 'H', ' ', 'B', 'R', 'A', 'S',
     'S'},
};
#undef SAH_OP

/* the performance: at block b (32 frames), what happens before it is rendered */
static void sa_hash_events(uint32_t b)
{
    static const uint8_t CHORD[12] = {36, 43, 48, 52, 55, 60, 64, 67, 71, 74, 79, 84};
    uint32_t k;
    if (b == 0u) {
        sa_synth_load_vced(SA_HASH_VOICES[0]);
        sa_synth_note_on(60, 100);
        sa_synth_note_on(64, 70);
        sa_synth_note_on(67, 40);
    }
    if (b == 300u)
        sa_synth_note_on(48, 127);
    if (b == 350u)
        sa_synth_note_off(64);
    if (b >= 400u && b < 600u && !(b & 15u))
        sa_synth_bend((int32_t)(b - 400u) * 40 - 4000);
    if (b == 450u)
        sa_synth_cc(1, 100);
    if (b == 500u)
        sa_synth_aftertouch(90);
    if (b == 600u) {
        sa_synth_bend(0);
        sa_synth_cc(1, 0);
        sa_synth_note_on(60, 90);                /* (sounding: its voice again) */
    }
    if (b == 800u)
        sa_synth_all_notes_off();
    if (b == 1300u) {
        sa_synth_load_vced(SA_HASH_VOICES[1]);
        sa_synth_function(SA_FN_PMODE, 1);
        sa_synth_function(SA_FN_PTIME, 60);
        sa_synth_note_on(50, 110);
    }
    if (b == 1500u) {
        sa_synth_note_on(57, 90);
        sa_synth_note_off(50);
    }
    if (b == 1800u) {
        sa_synth_note_off(57);
        sa_synth_function(SA_FN_PMODE, 0);
    }
    for (k = 0; k < 12u; k++) {                 /* more keys than voices */
        if (b == 2200u + 20u * k)
            sa_synth_note_on(CHORD[k], 60u + 5u * k);
        if (b == 3200u + 30u * k)
            sa_synth_note_off(CHORD[k]);
    }
}

static uint32_t sa_hash_block(uint32_t b, uint32_t h)
{
    int32_t x[SA_CTL];
    uint32_t i, k;
    sa_hash_events(b);
    sa_synth_render_mono(x, SA_CTL);
    for (i = 0; i < SA_CTL; i++)
        for (k = 0; k < 4u; k++)
            h = (h ^ (((uint32_t)x[i] >> (8u * k)) & 255u)) * 16777619u;
    return h;
}
