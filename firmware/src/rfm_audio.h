/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The unit's side of Bluetooth audio (AUDIO_MODE / AUDIO, docs/PROTOCOL.md section 9): the host's sound arrives as
 * AUDIO blocks of mono IMA ADPCM at 44,100 / div Hz and plays through the FM-1's own output. No HAL: rfm_app.c and
 * rfm_main.c use it on the FM-1, host/tests/test_ble_audio.c on the computer.
 *
 *   rfm_au_start / rfm_au_stop    the main loop: the mode begins (the buffer empty) / ends
 *   rfm_au_put                    the main loop: an AUDIO body into the ring, as it came (still coded)
 *   rfm_au_mix                    the render (the audio interrupt): n frames at 44,100 Hz added to the output
 *
 * The ring holds the blocks still coded (RFM_AU_SLOTS x 134 bytes: 4.3 KB, ~370 ms at 22,050 Hz; RAM is short on the
 * FM-1, so they are decoded in the render, ~10 operations a sample). One writer each way: the main loop moves w and
 * q_in, the render r and q_out, so no lock. The render waits until the buffer holds `target` samples, then plays,
 * interpolating (4-point Hermite) up to 44,100 Hz. Its step follows how full the buffer is, at most +-0.3 % (5
 * cents), so a host's clock that runs a little fast or slow keeps the buffer near its target instead of running dry
 * or over. A host that falls behind empties it: the sound fades out (an underrun) and starts again once `target` is
 * back. A burst that leaves far more than the target (a host that stalled and caught up) drops a block at a block's
 * end (skipped) rather than keep the delay. A seq gap of up to RFM_AU_GAP_MAX blocks is filled with the last sample
 * fading, so the timing stays; a longer one is counted and closed up; a jump back restarts the count. */
#ifndef RFM_AUDIO_H
#define RFM_AUDIO_H
#include <stdint.h>
#include "../../proto/rfm1.h"
#include "rfm_adpcm.h"

#define RFM_AU_SLOTS 32u                                /* a power of two */
#define RFM_AU_GAP_MAX 8u
#define RFM_AU_TRIM 0.003f                              /* the step's largest correction */
#define RFM_AU_TRIM_RATE 0.002f                         /* the fill error's smoothing, a render call (~1.5 ms) */
#ifndef RFM_AU_PUBLISH
#define RFM_AU_PUBLISH() __asm__ volatile("" ::: "memory")   /* (rfm_main.c: RING_PUBLISH) */
#endif

typedef struct {
    int16_t pred;
    uint8_t index, gap;                                 /* gap: n samples of the last one fading (a lost block) */
    uint16_t n;                                         /* samples */
    uint8_t d[RFM_AUDIO_DATA_MAX];
} rfm_au_slot_t;

typedef struct {
    rfm_au_slot_t *slot;                                /* RFM_AU_SLOTS of them, the includer's */
    volatile uint8_t on;                                /* the render plays it (set last at a start, first at a stop) */
    volatile uint32_t w, r;                             /* blocks written (the main loop), read (the render) */
    volatile uint32_t q_in, q_out;                      /* samples written, read: the fill is q_in - q_out */
    uint8_t div, flags, have_seq;
    uint16_t seq_next, buf_ms;
    uint32_t target;                                    /* samples held before playing */
    /* the render's */
    uint8_t playing, skip;
    uint32_t pos;                                       /* the sample next in slot r */
    rfm_adpcm_t dec;
    float h[4], phase, base, step, err, last;
    /* counters (since the start) */
    uint32_t blocks, lost, underruns, skipped, bad;
} rfm_au_t;

static inline void rfm_au_stop(rfm_au_t *a)
{
    a->on = 0;
    RFM_AU_PUBLISH();
}

/* 0 started; -1 a bad div (the mode refused) */
static inline int rfm_au_start(rfm_au_t *a, uint32_t div, uint32_t buf_ms, uint32_t flags)
{
    uint32_t i, rate;
    rfm_au_stop(a);
    if (div < RFM_AUDIO_DIV_MIN || div > RFM_AUDIO_DIV_MAX)
        return -1;
    buf_ms = buf_ms < RFM_AUDIO_BUF_MS_MIN ? RFM_AUDIO_BUF_MS_MIN : buf_ms > RFM_AUDIO_BUF_MS_MAX ? RFM_AUDIO_BUF_MS_MAX
                                                                                                    : buf_ms;
    rate = RFM_AUDIO_RATE_BASE / div;
    a->div = (uint8_t)div;
    a->flags = (uint8_t)flags;
    a->buf_ms = (uint16_t)buf_ms;
    a->target = rate * buf_ms / 1000u;
    a->w = a->r = a->q_in = a->q_out = 0;
    a->have_seq = 0;
    a->seq_next = 0;
    a->playing = a->skip = 0;
    a->pos = 0;
    a->dec.pred = 0;
    a->dec.index = 0;
    for (i = 0; i < 4u; i++)
        a->h[i] = 0.0f;
    a->phase = a->err = a->last = 0.0f;
    a->base = a->step = 1.0f / (float)div;              /* stream samples an output frame */
    a->blocks = a->lost = a->underruns = a->skipped = a->bad = 0;
    RFM_AU_PUBLISH();
    a->on = 1;
    RFM_AU_PUBLISH();
    return 0;
}

static inline uint32_t rfm_au_fill(const rfm_au_t *a) { return a->q_in - a->q_out; }
static inline uint32_t rfm_au_fill_ms(const rfm_au_t *a)
{
    return a->div ? (uint32_t)((uint64_t)rfm_au_fill(a) * 1000u * a->div / RFM_AUDIO_RATE_BASE) : 0u;
}

/* a slot written: n samples, coded (b: pred16 index8 data) or a gap (b NULL); 0 the ring is full */
static inline int rfm_au_slot(rfm_au_t *a, const uint8_t *b, uint32_t n)
{
    rfm_au_slot_t *s;
    uint32_t i;
    if (a->w - a->r >= RFM_AU_SLOTS)
        return 0;
    s = &a->slot[a->w & (RFM_AU_SLOTS - 1u)];
    s->n = (uint16_t)n;
    s->gap = (uint8_t)!b;
    if (b) {
        s->pred = (int16_t)(uint16_t)(b[0] | (uint32_t)b[1] << 8);
        s->index = b[2] > 88u ? 88u : b[2];
        for (i = 0; i < n / 2u; i++)
            s->d[i] = b[3 + i];
    }
    RFM_AU_PUBLISH();                                   /* the slot, then its index */
    a->w = a->w + 1u;
    a->q_in = a->q_in + n;
    RFM_AU_PUBLISH();
    return 1;
}

/* an AUDIO body (the main loop): 0 taken, -1 not now (the mode off) or malformed */
static inline int rfm_au_put(rfm_au_t *a, const uint8_t *b, uint32_t len)
{
    uint32_t seq, n;
    if (!a->on)
        return -1;
    if (len <= RFM_AUDIO_HDR || len > RFM_AUDIO_HDR + RFM_AUDIO_DATA_MAX) {
        a->bad++;
        return -1;
    }
    seq = (uint32_t)b[0] | (uint32_t)b[1] << 8;
    n = 2u * (len - RFM_AUDIO_HDR);
    if (a->have_seq && seq != a->seq_next) {
        uint32_t ahead = (seq - a->seq_next) & 0xFFFFu, back = (a->seq_next - seq) & 0xFFFFu;
        if (back <= RFM_AU_SLOTS)
            return 0;                                   /* (an old one again: dropped) */
        if (ahead <= RFM_AU_GAP_MAX) {
            uint32_t g;
            for (g = 0; g < ahead; g++)                 /* lost on the way: the time kept, the sound fading */
                rfm_au_slot(a, 0, RFM_AUDIO_BLOCK);
        }
        a->lost += ahead < 0x8000u ? ahead : 0u;        /* (a jump back: a new stream, nothing lost) */
    }
    a->have_seq = 1;
    a->seq_next = (uint16_t)(seq + 1u);
    a->blocks++;
    if (!rfm_au_slot(a, b + 2, n))
        a->lost++;                                      /* the ring full */
    return 0;
}

/* the render's next stream sample, -1..1 (the ring empty: an underrun, the last sample fading) */
static inline float rfm_au_next(rfm_au_t *a)
{
    const rfm_au_slot_t *s;
    int32_t v;
    for (;;) {
        if (a->r == a->w) {
            a->playing = 0;
            a->underruns++;
            a->last *= 0.9f;
            return a->last;
        }
        RFM_AU_PUBLISH();
        s = &a->slot[a->r & (RFM_AU_SLOTS - 1u)];
        if (a->pos < s->n)
            break;
        a->r = a->r + 1u;                               /* (the slot free for the main loop) */
        a->pos = 0;
        if (a->skip && a->r != a->w) {                  /* catching up: the next block dropped */
            a->q_out = a->q_out + a->slot[a->r & (RFM_AU_SLOTS - 1u)].n;
            a->r = a->r + 1u;
            a->skip = 0;
            a->skipped++;
        }
    }
    if (s->gap) {
        a->last *= 0.995f;
    } else {
        if (!a->pos) {
            a->dec.pred = s->pred;
            a->dec.index = s->index;
        }
        v = rfm_adpcm_decode(&a->dec, (uint32_t)(s->d[a->pos >> 1] >> ((a->pos & 1u) * 4u)) & 15u);
        a->last = (float)v * (1.0f / 32768.0f);
    }
    a->pos++;
    a->q_out = a->q_out + 1u;
    return a->last;
}

/* the render: n frames at 44,100 Hz, each the stream's sound x gain added to both channels; the peak (0..1) */
static inline float rfm_au_mix(rfm_au_t *a, int32_t *lr, uint32_t n, float gain)
{
    uint32_t i, fill;
    float e, peak = 0.0f;
    if (!a->on)
        return 0.0f;
    fill = rfm_au_fill(a);
    e = ((float)fill - (float)a->target) / (float)a->target;
    e = e > 1.0f ? 1.0f : e < -1.0f ? -1.0f : e;
    if (a->playing) {
        a->err += RFM_AU_TRIM_RATE * (e - a->err);
        a->step = a->base * (1.0f + RFM_AU_TRIM * a->err);
        if (fill > 2u * a->target + RFM_AUDIO_BLOCK)
            a->skip = 1;
    } else if (fill >= a->target) {
        a->playing = 1;
    }
    for (i = 0; i < n; i++) {
        float y, c1, c2, c3, t;
        a->phase += a->step;
        while (a->phase >= 1.0f) {
            a->phase -= 1.0f;
            a->h[0] = a->h[1];
            a->h[1] = a->h[2];
            a->h[2] = a->h[3];
            a->h[3] = a->playing ? rfm_au_next(a) : (a->last *= 0.9f);
        }
        t = a->phase;
        c1 = 0.5f * (a->h[2] - a->h[0]);
        c2 = a->h[0] - 2.5f * a->h[1] + 2.0f * a->h[2] - 0.5f * a->h[3];
        c3 = 0.5f * (a->h[3] - a->h[0]) + 1.5f * (a->h[1] - a->h[2]);
        y = ((c3 * t + c2) * t + c1) * t + a->h[1];
        y = y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y;
        lr[2u * i] += (int32_t)(y * gain);
        lr[2u * i + 1u] += (int32_t)(y * gain);
        y = y < 0.0f ? -y : y;
        peak = y > peak ? y : peak;
    }
    return peak;
}

#endif
