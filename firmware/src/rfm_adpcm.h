/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* IMA ADPCM (the IMA's 1992 recommendation, as in WAV's 0x11 and Apple's ima4): 4 bits a sample, a step that adapts.
 * The AUDIO message's coder (docs/PROTOCOL.md section 9): the firmware decodes with it (rfm_audio.h), libremotefm1
 * encodes with it (host/src/audio.c). One state: the predicted sample and the step's index (0..88). Header only. */
#ifndef RFM_ADPCM_H
#define RFM_ADPCM_H
#include <stdint.h>

static const int16_t RFM_ADPCM_STEP[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t RFM_ADPCM_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct {
    int32_t pred;                                       /* -32768..32767 */
    int32_t index;                                      /* 0..88 */
} rfm_adpcm_t;

/* one nibble (0..15) -> the next sample */
static inline int32_t rfm_adpcm_decode(rfm_adpcm_t *a, uint32_t nib)
{
    int32_t step = RFM_ADPCM_STEP[a->index], d = step >> 3;
    if (nib & 4u)
        d += step;
    if (nib & 2u)
        d += step >> 1;
    if (nib & 1u)
        d += step >> 2;
    a->pred += (nib & 8u) ? -d : d;
    a->pred = a->pred > 32767 ? 32767 : a->pred < -32768 ? -32768 : a->pred;
    a->index += RFM_ADPCM_INDEX[nib & 7u];
    a->index = a->index < 0 ? 0 : a->index > 88 ? 88 : a->index;
    return a->pred;
}

/* one sample -> its nibble; the state follows exactly as the decoder's will */
static inline uint32_t rfm_adpcm_encode(rfm_adpcm_t *a, int32_t x)
{
    int32_t step = RFM_ADPCM_STEP[a->index], diff = x - a->pred;
    uint32_t nib = 0;
    if (diff < 0) {
        nib = 8u;
        diff = -diff;
    }
    if (diff >= step) {
        nib |= 4u;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        nib |= 2u;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step)
        nib |= 1u;
    rfm_adpcm_decode(a, nib);
    return nib;
}

#endif
