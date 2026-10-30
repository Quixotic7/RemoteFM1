/* SPDX-License-Identifier: MPL-2.0 AND Apache-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * Portions Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Portions derived from the Apache-2.0 MSFA component:
 * Copyright 2012-2013 Google Inc.
 * Copyright 2016-2025 Pascal Gauthier.
 * Copyright 2019 Jean Pierre Cimalando (portamento).
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use these portions except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. Source revisions: docs/BLE_MIGRATION.md. */

#include "performance_options.h"
/* FM6 core: DX7-compatible synthesis with native fp32 operator signals.
 * Scaling, envelopes, LFO and the 32 algorithms follow the Apache-2.0 MSFA
 * component (Google / Pascal Gauthier / Jean Pierre Cimalando); the native
 * operator implementation is fm6_msfa_ops.h. MARK I and OPL are retired.
 * See docs/BLE_MIGRATION.md for exact upstream revisions and audit status.
 * Pitches use Q24 log2 Hz, one cycle of phase is 2^24; float operator output
 * is measured in cycles of modulation. The patch is a 155-byte DX7 voice. */

#include "x0x/fastmath.h"


/* the 155-byte patch: 6 x 21 operator bytes (the sixth operator first), then the voice */
enum {
    FP_R1 = 0, FP_L1 = 4, FP_BP = 8, FP_LD, FP_RD, FP_LC, FP_RC, FP_RS, FP_AMS, FP_KVS, FP_OL, FP_MODE, FP_FC, FP_FF,
    FP_DET, FP_OP = 21,
    FP_PR1 = 126, FP_PL1 = 130, FP_ALG = 134, FP_FB, FP_OKS, FP_LFS, FP_LFD, FP_LPMD, FP_LAMD, FP_LKS, FP_LFW,
    FP_LPMS, FP_TRNSP, FP_NAME, FP_SIZE = 155
};

/* the FM6 function settings (a DX7's function mode; each track its own, as each Dexed instance has, saved with the
 * project and the template; project.c):
 * pitch bend range up / down and step (0 = smooth), portamento (PEDAL: while CC 65 is down, ON) and its time
 * (CC 5 sets it) and glissando, then wheel, foot, breath and aftertouch: range and target (bit 0 pitch, 1
 * amplitude, 2 EG bias), Dexed's velocity scaling to the DX7's range, and ENGINE */
enum { FN_PBUP, FN_PBDN, FN_PBSTEP, FN_PMODE, FN_PTIME, FN_GLISS, FN_MWR, FN_MWA, FN_FCR, FN_FCA, FN_BCR, FN_BCA,
       FN_ATR, FN_ATA, FN_VNORM, FN_ENGINE, FM6_NFN };
enum { FM6_MODERN, FM6_MARK1, FM6_OPL }; /* 1/2 are historical import IDs only */
static const uint8_t FM6_FNMAX[FM6_NFN] = {12, 12, 12, 1, 127, 1, 99, 7, 99, 7, 99, 7, 99, 7, 1, 2};
static const uint8_t FM6_FNDEF[FM6_NFN] = {3, 3, 0, 0, 0, 0, 99, 1, 0, 0, 0, 0, 0, 0, 0, FM6_MODERN};   /* MSFA renderer */
static uint8_t fm6_fn[NTRK][FM6_NFN];                    /* per track (fm6_fn_reset at power-on, a new project) */
static void fm6_fn_reset(void)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++)
        memcpy(fm6_fn[k], FM6_FNDEF, FM6_NFN);
}
static void fm6_fn_set(uint32_t tr, uint32_t k, int32_t v)
{
    if (tr < NTRK && k < FM6_NFN)
        fm6_fn[tr][k] = k == FN_ENGINE ? FM6_MODERN : (uint8_t)clamp(v, 0, FM6_FNMAX[k]);
}
static int fm6_fn_ok(const uint8_t *f)                    /* a stored set: every value in its range */
{
    uint32_t k;
    for (k = 0; k < FM6_NFN; k++)
        if (f[k] > FM6_FNMAX[k])
            return 0;
    return 1;
}

/* ---------------------------------------------------------- DX7 data --- */
/* DX7 data, measured for MSFA (Copyright 2012 Google Inc.) and Dexed (Copyright 2013-2017 Pascal Gauthier),
 * Apache License 2.0: output levels below 20; the static times of the envelope (samples at 44.1 kHz, rates
 * 0..76); the velocity curve; the exponential key-scaling curve; pitch-modulation sensitivity; the pitch
 * envelope's rates and steps (1/32 octave). The sine, frequency, detune, LFO and
 * portamento tables are in melodee_tables.h (tools/gen_tables.py) */
static const uint8_t FM6_LEVELLUT[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
static const int32_t FM6_STATICS[77] = {
    1764000, 1764000, 1411200, 1411200, 1190700, 1014300, 992250, 882000, 705600, 705600,
    584325, 507150, 502740, 441000, 418950, 352800, 308700, 286650, 253575, 220500,
    220500, 176400, 145530, 145530, 125685, 110250, 110250, 88200, 88200, 74970,
    61740, 61740, 55125, 48510, 44100, 37485, 31311, 30870, 27562, 27562,
    22050, 18522, 17640, 15435, 14112, 13230, 11025, 9261, 9261, 7717,
    6615, 6615, 5512, 5512, 4410, 3969, 3969, 3439, 2866, 2690,
    2249, 1984, 1896, 1808, 1411, 1367, 1234, 1146, 926, 837,
    837, 705, 573, 573, 529, 441, 441};
static const uint8_t FM6_VELOCITY[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202, 205,
    206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232, 233, 235,
    237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251, 252, 253, 254};
static const uint8_t FM6_EXPSCALE[33] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33,
                                         39, 47, 56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250};
static const uint8_t FM6_PMS[8] = {0, 10, 20, 33, 55, 92, 153, 255};
static const uint32_t FM6_AMS[4] = {0, 4342338, 7171437, 16777216};   /* Q24 */
static const uint8_t FM6_PEG_RATE[100] = {
    1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
    12, 12, 13, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    27, 28, 30, 31, 33, 34, 36, 37, 38, 39, 41, 42, 44, 46, 47, 49, 51, 53, 54, 56,
    58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 79, 82, 85, 88, 91, 94, 98, 102, 106, 110,
    115, 120, 125, 130, 135, 141, 147, 153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255};
static const int8_t FM6_PEG_STEP[100] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43, -41, -39, -37, -35, -33, -32, -31,
    -30, -29, -28, -27, -26, -25, -24, -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11,
    -10, -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 38, 40, 43, 46, 49, 53, 58, 65, 73, 82, 92, 103, 115, 127};
_Static_assert(CTL * 2 == FM6_N, "FM6: a Dexed block is two control ticks");

/* ------------------------------------------------------- the algorithms --- */
/* The 32 algorithms as Dexed (MSFA fm_core.cc) has them, operators in the order they run (the sixth first):
 * bits 0-1 where the output goes (0 = the voice, 1 / 2 = bus A / B), bit 2 adds to it, bits 4-5 the bus that
 * modulates the operator, bits 6 + 7 its own feedback (bit 7 alone: the end of a feedback loop through OP6,
 * algorithms 4 and 6). tests/fm6_test.c checks the modulation graph of each against the DX7 diagrams */
enum { FM6_OB1 = 1, FM6_OB2 = 2, FM6_OADD = 4, FM6_IB1 = 16, FM6_IB2 = 32, FM6_FBIN = 64, FM6_FBOUT = 128 };
static const uint8_t FM6_ALG[32][6] = {
    {0xc1, 0x11, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x11, 0x14, 0xc1, 0x14}, {0xc1, 0x11, 0x14, 0x01, 0x11, 0x14},
    {0xc1, 0x11, 0x94, 0x01, 0x11, 0x14}, {0xc1, 0x14, 0x01, 0x14, 0x01, 0x14}, {0xc1, 0x94, 0x01, 0x14, 0x01, 0x14},
    {0xc1, 0x11, 0x05, 0x14, 0x01, 0x14}, {0x01, 0x11, 0xc5, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x05, 0x14, 0xc1, 0x14},
    {0x01, 0x05, 0x14, 0xc1, 0x11, 0x14}, {0xc1, 0x05, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x05, 0x14, 0xc1, 0x14},
    {0xc1, 0x05, 0x05, 0x14, 0x01, 0x14}, {0xc1, 0x05, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x05, 0x11, 0x14, 0xc1, 0x14},
    {0xc1, 0x11, 0x02, 0x25, 0x05, 0x14}, {0x01, 0x11, 0x02, 0x25, 0xc5, 0x14}, {0x01, 0x11, 0x11, 0xc5, 0x05, 0x14},
    {0xc1, 0x14, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x14}, {0x01, 0x14, 0x14, 0xc1, 0x14, 0x14},
    {0xc1, 0x14, 0x14, 0x14, 0x01, 0x14}, {0xc1, 0x14, 0x14, 0x01, 0x14, 0x04}, {0xc1, 0x14, 0x14, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x14, 0x04, 0x04, 0x04}, {0xc1, 0x05, 0x14, 0x01, 0x14, 0x04}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x04},
    {0x04, 0xc1, 0x11, 0x14, 0x01, 0x14}, {0xc1, 0x14, 0x01, 0x14, 0x04, 0x04}, {0x04, 0xc1, 0x11, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x04, 0x04, 0x04, 0x04}, {0xc4, 0x04, 0x04, 0x04, 0x04, 0x04}};
static inline __attribute__((always_inline)) int fm6_carrier(uint32_t alg, uint32_t n) { return !(FM6_ALG[alg & 31u][6u - n] & 3u); }   /* OP n: to the voice */
/* bit k: operator k (the sixth first) is a carrier of algorithm a: it writes the voice */
static uint32_t fm6_carriers(uint32_t a)
{
    uint32_t k, c = 0;
    for (k = 0; k < 6u; k++)
        c |= ((FM6_ALG[a & 31u][k] & 3u) ? 0u : 1u) << k;
    return c;
}

/* The chip's discrete envelopes and ROMs remain integer data. Continuous
 * gains, signal mixing and AMS use the native single-precision FPU. */
/* A Q24 log pitch has more significant bits than float. Preserve its exact
 * phase step with the DX7 frequency table, using two 7-bit partial products
 * instead of a wide multiply. Otherwise high-ratio notes drift against Dexed. */
static inline __attribute__((always_inline)) int32_t fm6_freq(int32_t lf)
{
    uint32_t i=((uint32_t)lf&0xFFFFFFu)>>14,f=(uint32_t)lf&0x3FFFu;
    uint32_t delta=(uint32_t)(FM6_FREQ[i+1u]-FM6_FREQ[i]);
    int32_t y=FM6_FREQ[i]+(int32_t)((delta*(f>>7)+((delta*(f&127u))>>7))>>7);
    int32_t sh=20-(lf>>24);
    return sh<=0?y:sh>31?0:y>>sh;
}
static inline __attribute__((always_inline)) uint32_t fm6_pow2(int32_t x)
{
    return (uint32_t)fm_minf(fm_exp2f((float)x * (1.0f / 65536.0f)) * 65536.0f, 4294967040.0f);
}
static int32_t fm6_f32(int32_t x) { return (int32_t)(float)x; }
static inline __attribute__((always_inline)) uint32_t fm6_ams_pt(uint32_t sa)
{
    return (uint32_t)fm_expf((float)sa * (0.07f / 262144.0f) + 12.2f);
}
/* Keep the exact wrapping phase counter: converting a slow oscillator's
 * accumulated phase to float would lose low bits on every sample. Modulation
 * is in cycles; reduce it before converting so large indices cannot overflow. */
static inline __attribute__((always_inline)) uint32_t fm6_phase(uint32_t ph, float mod)
{
    /* Truncate whole cycles in either direction; the counter's mask wraps
     * signed fractional cycles without a conditional floor operation. */
    mod -= (float)(int32_t)mod;
    int32_t offset=(int32_t)(mod*16777216.0f);
    return (ph+(uint32_t)offset)&0xFFFFFFu;
}
static inline __attribute__((always_inline)) float fm6_sin(uint32_t ph)
{
    uint32_t i = (ph >> 14) & 1023u;
    float f = (float)(ph & 0x3FFFu) * (1.0f / 16384.0f);
    return FM6_SIN[i] + (FM6_SIN[i+1u] - FM6_SIN[i]) * f;
}

/* -------------------------------------------------- scaling (DX7 rules) --- */
static inline __attribute__((always_inline)) int32_t fm6_scaleout(int32_t l) { return l >= 20 ? 28 + l : FM6_LEVELLUT[l < 0 ? 0 : l]; }

static int32_t fm6_curve(int32_t group, int32_t depth, int32_t curve)   /* key level scaling, 0.75 dB steps */
{
    int32_t s = curve == 0 || curve == 3 ? (group * depth * 329) >> 12
                                         : (FM6_EXPSCALE[group > 32 ? 32 : group] * depth * 329) >> 15;
    return curve < 2 ? -s : s;
}

/* an operator's output level for a note and velocity, in 0.023 dB steps (99 * 32 = full) */
static int32_t fm6_outlevel(const uint8_t *op, uint32_t note, uint32_t vel)
{
    int32_t off = (int32_t)note - op[FP_BP] - 17, l, v;
    l = fm6_scaleout(op[FP_OL]) + (off >= 0 ? fm6_curve((off + 1) / 3, op[FP_RD], op[FP_RC])
                                            : fm6_curve(-(off - 1) / 3, op[FP_LD], op[FP_LC]));
    v = FM6_VELOCITY[(vel > 127u ? 127u : vel) >> 1] - 239;
    l = (l > 127 ? 127 : l) * 32 + ((op[FP_KVS] * v + 7) >> 3) * 16;
    return l < 0 ? 0 : l;
}

static int32_t fm6_rscale(const uint8_t *op, uint32_t note)   /* key rate scaling, in qrate units */
{
    int32_t x = (int32_t)note / 3 - 7;
    return (op[FP_RS] * clamp(x, 0, 31)) >> 3;
}

static int32_t fm6_logfreq(const uint8_t *op, uint32_t note)   /* an operator's pitch for a note (osc_freq) */
{
    int32_t lf;
    if (op[FP_MODE]) {                                  /* fixed: 10 ^ (COARSE % 4 + FINE / 100) Hz */
        lf = (4458616 * ((op[FP_FC] & 3) * 100 + op[FP_FF])) >> 3;
        return lf + (op[FP_DET] > 7 ? 13457 * (op[FP_DET] - 7) : 0);
    }
    lf = 50857777 + ((1 << 24) / 12) * (int32_t)note;
    lf += (int32_t)((FM6_DETUNE[note] * (op[FP_DET] - 7)) >> 24) + FM6_COARSE[op[FP_FC] & 31];
    return lf + FM6_FINE[op[FP_FF]];
}

/* ------------------------------------------------------------ envelopes --- */
typedef struct {
    int32_t level, target, inc, hold;                    /* Q24 log2; samples a static segment has left */
    uint8_t ix, rising;                                  /* segment 0..3 (L1..L4), 4 = done */
} fm6_eg_t;

typedef struct {                                         /* a pitch envelope (PitchEnv) */
    int32_t pl, pt, pi;                                  /* level, target, step (Q24 octaves) */
    uint8_t pix, prise, pr[4], pv[4];                    /* segment, direction, rates and levels (at the key) */
} fm6_peg_t;

/* segment ix begins (Env::advance); where it would not move (or an attack to L1 0) the DX7 waits */
static __attribute__((noinline,section(".dsp_text"))) void fm6_eg_go(fm6_eg_t *e, const uint8_t *op, int32_t ol, int32_t rs, uint32_t ix)
{
    e->ix = (uint8_t)ix;
    if (ix < 4u) {
        int32_t lv = op[FP_L1 + ix], a = ((fm6_scaleout(lv) >> 1) << 6) + ol - 4256;
        int32_t q = clamp(((op[FP_R1 + ix] * 41) >> 6) + rs, 0, 63);
        e->target = (a < 16 ? 16 : a) << 16;
        e->rising = e->target > e->level;
        e->hold = 0;
        if (e->target == e->level || (!ix && !lv)) {
            int32_t r = clamp(op[FP_R1 + ix] + rs, 0, 99);
            e->hold = r < 77 ? FM6_STATICS[r] : 20 * (99 - r);
            if (r < 77 && !ix && !lv)
                e->hold /= 20;
        }
        e->inc = (4 + (q & 3)) << (2 + FM6_LG_N + (q >> 2));
    }
}

static inline __attribute__((always_inline)) void fm6_eg_go_run(fm6_eg_t *e, const uint8_t *op, int32_t ol, int32_t rs, uint32_t ix)
{
    void (*volatile run)(fm6_eg_t *, const uint8_t *, int32_t, int32_t, uint32_t) = fm6_eg_go;
    run(e, op, ol, rs, ix);
}
#define fm6_eg_go(e,op,ol,rs,ix) fm6_eg_go_run((e),(op),(ol),(rs),(ix))

/* one block of an operator envelope (Env::getsample); returns its level. Segments 0..2 run
 * while the key is down, 3 after the release */
static __attribute__((noinline,section(".dsp_text"))) int32_t fm6_eg_step(fm6_eg_t *e, const uint8_t *op, int32_t ol, int32_t rs, int down)
{
    if (e->hold) {
        e->hold -= FM6_N;
        if (e->hold <= 0) {
            e->hold = 0;
            fm6_eg_go(e, op, ol, rs, e->ix + 1u);
        }
    }
    if ((e->ix < 3u || (e->ix < 4u && !down)) && !e->hold) {
        if (e->rising) {                                 /* the DX7 attack: from -48 dB, faster when low */
            if (e->level < (1716 << 16))
                e->level = 1716 << 16;
            e->level += (((17 << 24) - e->level) >> 24) * e->inc;
            if (e->level >= e->target) {
                e->level = e->target;
                fm6_eg_go(e, op, ol, rs, e->ix + 1u);
            }
        } else {
            e->level -= e->inc;
            if (e->level <= e->target) {
                e->level = e->target;
                fm6_eg_go(e, op, ol, rs, e->ix + 1u);
            }
        }
    }
    return e->level;
}

static inline __attribute__((always_inline)) int32_t fm6_eg_step_run(fm6_eg_t *e, const uint8_t *op, int32_t ol, int32_t rs, int down)
{
#if MELODEE_FM6_FAST
    /* Sustained and finished operators have no state transition. Avoid the
     * far RAM call, while static holds still consume their exact samples. */
    if (e->ix >= 3u && !e->hold && (e->ix >= 4u || down))
        return e->level;
#endif
    int32_t (*volatile run)(fm6_eg_t *, const uint8_t *, int32_t, int32_t, int) = fm6_eg_step;
    return run(e, op, ol, rs, down);
}
#define fm6_eg_step(e,op,ol,rs,down) fm6_eg_step_run((e),(op),(ol),(rs),(down))

static __attribute__((noinline,section(".dsp_text"))) void fm6_peg_go(fm6_peg_t *e, uint32_t ix)        /* pitch EG segment ix (PitchEnv::advance) */
{
    e->pix = (uint8_t)ix;
    if (ix < 4u) {
        e->pt = FM6_PEG_STEP[e->pv[ix]] * (1 << 19);
        e->prise = e->pt > e->pl;
        e->pi = FM6_PEG_RATE[e->pr[ix]] * FM6_PEG_UNIT;
    }
}

static inline __attribute__((always_inline)) void fm6_peg_go_run(fm6_peg_t *e, uint32_t ix)
{
    void (*volatile run)(fm6_peg_t *, uint32_t) = fm6_peg_go;
    run(e, ix);
}
#define fm6_peg_go(e,ix) fm6_peg_go_run((e),(ix))

static void fm6_peg_set(fm6_peg_t *e, const uint8_t *p)  /* a key: the voice's pitch EG starts from L4 (set) */
{
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        e->pr[k] = p[FP_PR1 + k];
        e->pv[k] = p[FP_PL1 + k];
    }
    e->pl = FM6_PEG_STEP[e->pv[3]] * (1 << 19);
    fm6_peg_go(e, 0);
}

static inline __attribute__((always_inline)) int32_t fm6_peg_step(fm6_peg_t *e, int down)      /* pitch envelope, Q24 octaves */
{
    if (e->pix < 3u || (e->pix < 4u && !down)) {
        if (e->prise) {
            e->pl += e->pi;
            if (e->pl >= e->pt) {
                e->pl = e->pt;
                fm6_peg_go(e, e->pix + 1u);
            }
        } else {
            e->pl -= e->pi;
            if (e->pl <= e->pt) {
                e->pl = e->pt;
                fm6_peg_go(e, e->pix + 1u);
            }
        }
    }
    return e->pl;
}

/* ------------------------------------------------------------ the voice --- */
typedef struct fm6_voice {                               /* a voice; operators in Dexed's order: 0 = OP6 */
    fm6_eg_t eg[6];
    uint32_t ph[6];                                      /* phase, Q24 = a cycle */
    int32_t fq[6], freq_log[6];
    uint8_t freq_valid;
    float gout[6], g[6], dg[6];                 /* step; gain at the block's end, now, per sample */
    int32_t base[6], porta[6];                           /* the note's log frequency; portamento's */
    int16_t ol[6];                                       /* output level (note, velocity), microsteps */
    int8_t rs[6];                                        /* rate scaling */
    uint8_t plan[6];                                     /* this block's routing (FM6_P_*) */
    float fb[2];                                       /* the feedback operator's last two outputs */
    fm6_peg_t pe;                                        /* the pitch envelope */
    uint8_t down, sub, note, vel, eng, fbs, quiet, played;
    uint8_t frozen;                                      /* stopped as by Dexed's panic: not kept running */
    uint8_t still;                                       /* over and at rest: 1 only time goes on, 2 nothing matters */
    int32_t spb;                                         /* the bend and tune it was at rest with */
} fm6_voice_t;
#define FM6_SILENT 1638400                               /* an envelope level no engine renders (MARK I: -96 dB) */
enum { FM6_P_ADD = 4, FM6_P_FB = 0x40, FM6_P_RUN = 0x80 };   /* plan: bits 0-1 out bus, 4-5 in bus */

#include "fm6_msfa_ops.h"

/* Scratch belongs to the caller: paired voices must never share operator buses.
 * Keep the operator kernel separate: surrounding engine additions otherwise
 * change this old compiler's register allocation and loop layout. */
#if MELODEE_DUAL_CORE
static __attribute__((noinline, section(".dsp_text"))) void fm6_run_into(fm6_voice_t *s, uint32_t n, float bus[2][CTL], float *sum)
#else
static __attribute__((noinline, section(".dsp_text"))) void fm6_run(fm6_voice_t *s, uint32_t n)
#endif
{
    uint32_t k, eng = s->eng;
#if !MELODEE_DUAL_CORE
    float (*bus)[CTL] = fm6_bus, *sum = fm6_sum;
#endif
    for (k = 0; k < n; k++)
        sum[k] = 0;
    for (k = 0; k < 6u; k++) {
        uint32_t pl = s->plan[k], o = pl & 3u, in = (pl >> 4) & 3u;
        float *out = o ? bus[o - 1u] : sum;
        if (!(pl & FM6_P_RUN))
            continue;
        if (pl & FM6_P_FB)
            fm6_op_fb(s, k, out, pl & FM6_P_ADD, eng, n);
        else
            fm6_op(s, k, out, in ? bus[in - 1u] : 0, pl & FM6_P_ADD, eng, n);
    }
}

#if MELODEE_DUAL_CORE
static void fm6_run(fm6_voice_t *s, uint32_t n)
{
    /* XIP to internal RAM exceeds the target's direct-call range. Both cores
     * use the same RAM kernel with separate scratch, avoiding flash fetch
     * contention without changing operator arithmetic or addition order. */
    void (*volatile run)(fm6_voice_t *, uint32_t, float [2][CTL], float *) = fm6_run_into;
    run(s, n, fm6_bus, fm6_sum);
}
#endif
