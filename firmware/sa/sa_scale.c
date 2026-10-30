/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The keys' layout (the 27 keys from F: which are white, which black) and the scale (GLO + PRESETS the root, GLO +
 * ALGORITHM the scale). Part of sa_main.c (its state, sa). */

#define SA_NSCALES 1u                        /* (the scales: next commit) */

static int32_t sa_key_white(uint32_t k)      /* key k's place among the 16 white keys (0..15), -1 a black one */
{
    static const int8_t W[12] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6};   /* F F# G G# A A# B C C# D D# E */
    int32_t w = k < NKEYS ? W[k % 12u] : -1;
    return w < 0 ? -1 : (int32_t)(k / 12u) * 7 + w;
}

static int32_t sa_key_black(uint32_t k)      /* key k's place among the 11 black keys (0..10), -1 a white one */
{
    static const int8_t B[12] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1};
    int32_t b = k < NKEYS ? B[k % 12u] : -1;
    return b < 0 ? -1 : (int32_t)(k / 12u) * 5 + b;
}

/* GLO + a black key: the octave, the middle black key (the sixth, F#4 at octave 0) 0, -1 / +1 a key left / right,
 * -3 / +3 at the ends (the first three, the last three) */
static int32_t sa_black_oct(uint32_t b) { return clamp((int32_t)b - 5, -3, 3); }

static uint32_t sa_snap(uint32_t n) { return n; }   /* (the scale: next commit) */
static int sa_rm_glo_enc(uint32_t k, int32_t d) { (void)k; (void)d; return 0; }
