/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Bluetooth audio's encoder. See rfm/audio.h. */
#include "rfm/audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rfm_adpcm.h"

#define TAPS 64                                         /* the low-pass's length (input samples) */
#define PH 128                                          /* its phases, interpolated between */
#define HN 256u                                         /* the input's history (a power of two, > TAPS) */
#define KAISER_BETA 6.0                                 /* ~63 dB stop band */

struct rfm_audio_enc {
    uint32_t div, out_rate, in_rate;
    double t, inc;                                      /* the next output's time, in input samples; the step */
    uint64_t n_in;                                      /* input samples so far */
    float hist[HN];
    float tab[(PH + 1) * TAPS];                         /* tab[p][j]: tap j at fraction p / PH */
    int16_t blk[RFM_AUDIO_BLOCK];
    uint32_t nblk;
    rfm_adpcm_t st;
    uint16_t seq;
};

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0, k;
    for (k = 1.0; k < 40.0; k += 1.0) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1e-12 * s)
            break;
    }
    return s;
}

/* the polyphase table for in_rate -> out_rate: cut-off 0.45 x the lower rate, each phase's taps summing to 1 */
static void build_table(rfm_audio_enc_t *e)
{
    const double lo = e->in_rate < e->out_rate ? e->in_rate : e->out_rate, fc = 0.45 * lo / e->in_rate;
    const double half = TAPS / 2.0, i0b = bessel_i0(KAISER_BETA);
    int p, j;
    for (p = 0; p <= PH; p++) {
        double sum = 0.0;
        float *row = &e->tab[p * TAPS];
        for (j = 0; j < TAPS; j++) {
            /* tap j is input sample floor(t) - (TAPS/2 - 1) + j; x its distance from t */
            const double x = (double)(j - (TAPS / 2 - 1)) - (double)p / PH, r = x / half;
            const double w = fabs(r) >= 1.0 ? 0.0 : bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / i0b;
            const double a = 2.0 * 3.14159265358979323846 * fc * x;
            const double h = (fabs(x) < 1e-9 ? 2.0 * fc : sin(a) / (3.14159265358979323846 * x)) * w;
            row[j] = (float)h;
            sum += h;
        }
        for (j = 0; j < TAPS; j++)
            row[j] = (float)(row[j] / sum);
    }
}

rfm_audio_enc_t *rfm_audio_enc_new(void)
{
    rfm_audio_enc_t *e = (rfm_audio_enc_t *)calloc(1, sizeof *e);
    if (e)
        rfm_audio_enc_reset(e, RFM_AUDIO_DIV_DEFAULT);
    return e;
}

void rfm_audio_enc_free(rfm_audio_enc_t *e) { free(e); }

void rfm_audio_enc_reset(rfm_audio_enc_t *e, uint32_t div)
{
    if (div < RFM_AUDIO_DIV_MIN || div > RFM_AUDIO_DIV_MAX)
        div = RFM_AUDIO_DIV_DEFAULT;
    e->div = div;
    e->out_rate = RFM_AUDIO_RATE_BASE / div;
    e->in_rate = 0;                                     /* (the next push builds the table) */
    e->nblk = 0;
    e->st.pred = 0;
    e->st.index = 0;
    e->seq = 0;
}

uint32_t rfm_audio_enc_rate(const rfm_audio_enc_t *e) { return e->out_rate; }

static void put_sample(rfm_audio_enc_t *e, float y, rfm_audio_emit_fn emit, void *ctx)
{
    int32_t v = (int32_t)lrintf(y * 32767.0f);
    e->blk[e->nblk++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    if (e->nblk == RFM_AUDIO_BLOCK) {
        uint8_t b[RFM_AUDIO_HDR + RFM_AUDIO_DATA_MAX];
        uint32_t i;
        b[0] = (uint8_t)e->seq;
        b[1] = (uint8_t)(e->seq >> 8);
        b[2] = (uint8_t)(uint16_t)e->st.pred;
        b[3] = (uint8_t)((uint16_t)e->st.pred >> 8);
        b[4] = (uint8_t)e->st.index;
        for (i = 0; i < RFM_AUDIO_DATA_MAX; i++) {
            uint32_t lo = rfm_adpcm_encode(&e->st, e->blk[2u * i]);
            uint32_t hi = rfm_adpcm_encode(&e->st, e->blk[2u * i + 1u]);
            b[RFM_AUDIO_HDR + i] = (uint8_t)(lo | hi << 4);
        }
        e->seq++;
        e->nblk = 0;
        if (emit)
            emit(ctx, b, sizeof b);
    }
}

void rfm_audio_enc_push(rfm_audio_enc_t *e, const float *pcm, uint32_t n, uint32_t in_rate, rfm_audio_emit_fn emit,
                        void *ctx)
{
    uint32_t i;
    if (in_rate < 8000u || in_rate > 192000u)
        return;
    if (in_rate != e->in_rate) {
        e->in_rate = in_rate;
        e->inc = (double)in_rate / e->out_rate;
        build_table(e);
        memset(e->hist, 0, sizeof e->hist);
        e->n_in = 0;
        e->t = TAPS / 2;                                /* (the first output once its window is all real input) */
    }
    for (i = 0; i < n; i++) {
        float x = pcm[i];
        e->hist[e->n_in++ & (HN - 1u)] = x != x ? 0.0f : x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;   /* (NaN: 0) */
        /* every output whose window ends at the newest input */
        while ((uint64_t)e->t + TAPS / 2 < e->n_in) {
            const uint64_t i0 = (uint64_t)e->t;
            const double fr = (e->t - (double)i0) * PH;
            const int p = (int)fr;
            const float f = (float)(fr - p), *r0 = &e->tab[p * TAPS], *r1 = r0 + TAPS;
            const uint64_t k0 = i0 - (TAPS / 2 - 1);
            float y = 0.0f;
            int j;
            for (j = 0; j < TAPS; j++)
                y += e->hist[(k0 + (uint64_t)j) & (HN - 1u)] * (r0[j] + f * (r1[j] - r0[j]));
            put_sample(e, y, emit, ctx);
            e->t += e->inc;
        }
    }
}

uint32_t rfm_audio_decode(const uint8_t *b, uint32_t n, int16_t *out)
{
    rfm_adpcm_t a;
    uint32_t i, k = 0;
    if (n <= RFM_AUDIO_HDR || n > RFM_AUDIO_HDR + RFM_AUDIO_DATA_MAX)
        return 0;
    a.pred = (int16_t)(uint16_t)(b[2] | (uint32_t)b[3] << 8);
    a.index = b[4] > 88u ? 88 : b[4];
    for (i = RFM_AUDIO_HDR; i < n; i++) {
        out[k++] = (int16_t)rfm_adpcm_decode(&a, b[i] & 15u);
        out[k++] = (int16_t)rfm_adpcm_decode(&a, b[i] >> 4);
    }
    return k;
}
