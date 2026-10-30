/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors
 * Portions Copyright (C) 2026 Kerem Kilic (Ellic Studio): clamp, amp_at, audio_dc_decay, VOICE_FS and AUDIO_ONE
 * restate saadet's dsp.c and audio_pcm.h (MPL-2.0) exactly, so that FM6 computes as it does in saadet */
/* What saadet's eng_fm6.c expects of saadet's instrument (its core.h: track_t, voice_t, vmod_t, the song, the audio
 * batch), reduced to what FM6 reads: one part (track 0), SA_POLY voices, no macros (P_E0..P_E6 stay 0: FM6 plays
 * the patch as it is), no audio batch, one core. sa_synth.c includes this, then eng_fm6.c, and is the instrument:
 * it allocates the voices, keeps their gates, and calls fm6_note_on, fm6_block, fm6_render, fm6_done, fm6_post the
 * way saadet's voice.c does for an engine with its own envelopes (engine_t.ownenv). */
#pragma once
#include <stdint.h>
#if __has_include(<string.h>)
#include <string.h>
#else                                    /* (the FM-1's toolchain has no C library: felucca-bsp chip/libc.c's) */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#endif
#include "sa_synth.h"
#include "fm6_tables.h"                  /* firmware/tools/gen_fm6_tables.py (build/gen) */

#define CTL 32                           /* saadet's block (SA_CTL) */
#define FS FM6_FS
#define NTRK 1u                          /* one part */
#define NVOICE SA_POLY
#define MELODEE_DUAL_CORE 0              /* one core: FM6's serial kernel */
_Static_assert(CTL == (int)SA_CTL, "SA_CTL is saadet's CTL");

/* a track's parameters as FM6 reads them: the eight macros (0: the patch as it is) and the voice mode */
enum { P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_VOICE, P_COUNT };
enum { V_POLY, V_MONO, V_LEGATO, V_UNISON };

typedef struct {                         /* a voice (saadet's voice_t: what FM6 reads, and the allocation's) */
    uint8_t note, vel, mvel, gate, active;
    uint8_t stage;                       /* 0 off, 1 sounding, 3 released, 4 given up (FM6's panic: one block's fade) */
    uint8_t sus;                         /* released while the sustain pedal is down: released when it comes up */
    int32_t env_out;                     /* the amplitude the last block ended at, Q15 */
    uint32_t age;
} voice_t;

typedef struct {                         /* a voice's modulation for a block (saadet's vmod_t: FM6's part) */
    int32_t amp0, amp1;                  /* Q15 ramp over the block */
    int32_t cutoff, shape;               /* the instrument's filter and shape (FM6: modulator level, feedback) */
    int32_t plog;                        /* the voice's own pitch offset, Q24 octaves (glide, LFO: 0 here) */
} vmod_t;

typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
    int32_t bend_raw;                    /* -8192..8191 */
    uint8_t mw, foot, breath, at, porta; /* CC 1, 4, 2, channel pressure, CC 65 (on / off) */
} track_t;

static track_t trk[NTRK];
static struct { int16_t g[1]; } song;    /* saadet's global TUNE in cents: 0 */
#define G_TUNE 0
static uint8_t voice_was;                /* what the voice a note starts did before: 0 free, 1 released, 2 held */
static inline int32_t tuning_log(void) { return 0; }   /* A4 = 440 Hz */

/* saadet's dsp.c and audio_pcm.h, as there (audio Q23 in int32: AUDIO_FRAC_BITS 8) */
#define VOICE_FS 24000                   /* a voice's level: a full-scale operator at amplitude 1.0 */
#define AUDIO_ONE (1 << 8)
typedef int64_t audio_dc_t;
static inline __attribute__((always_inline)) int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
/* (out of line, unlike saadet's: its 64-bit multiplies must not sit beside the DC filter's loads, felucca-bsp
 * docs/CPU.md; the same result) */
static __attribute__((noinline)) audio_dc_t audio_dc_decay(audio_dc_t y, int32_t coefficient)
{
    return (audio_dc_t)((y >> 30) * coefficient + (((int64_t)(y & 1073741823) * coefficient) >> 30));
}
static inline __attribute__((always_inline)) int32_t amp_at(const vmod_t *m, uint32_t i)   /* (CTL = 2^5) */
{
    int32_t x = (m->amp1 - m->amp0) * (int32_t)i;
    return m->amp0 + ((x + ((x >> 31) & (CTL - 1))) >> 5);
}

/* one core, no audio batch: FM6's batch and profiling paths compile away */
static const int audio_batch_active = 0;
#define AUDIO_BATCH_BYTES 128
#define audio_batch_push(kernel, job, size, out, n, ap) ((void)(kernel), (void)(job), (void)(out))
#define audio_batch_push_cost(kernel, job, size, out, n, ap, cost) ((void)(kernel), (void)(job), (void)(out), (void)(cost))
#define AP_START(t)
#define AP_END(what, t)
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* the slot's stores before the index's (a compiler barrier) */
