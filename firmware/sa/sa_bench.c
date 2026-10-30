/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The FM6 bench (STANDALONE-PLAN.md S0): what a block of the DX7 costs on the FM-1. Part of the unity build (app.c);
 * the synth is sa_synth.c's (not hooked to the keys or the sound yet: the bench renders into a buffer of its own).
 *
 * For each voice count (1, 8, 12, 16, up to SA_POLY), the dense voice below is held on that many keys and rendered
 * for SA_BENCH_SECONDS of audio, block by block (APP_BLOCK frames: what fm1-base's audio interrupt asks of
 * app_render), each block timed with plat_ticks24. The bench runs in the main loop, a block at a time (app_frame
 * and app_input_only), so USB, the watchdog and Bluetooth go on; the audio interrupt, the 2 kHz USB poll and the
 * 1 kHz tick may land inside a measured block, so the worst block includes them. The FM6 code runs from the flash
 * (XIP: firmware/patches/fm1-base/0005), its sine in RAM.
 *
 * The bench build (./build.sh --bench: SA_BENCH_BOOT, SA_POLY 16) starts it at boot; any build starts it on
 * SA_BENCH with body 01. It waits until 3 s after boot and until Bluetooth's start is over (the radio's trim must
 * not meet heavy flash reads). SA_BENCH's reply (firmware/src/remote_proto.h; firmware/tools/sa_bench.py reads it):
 *   0 state8 (0 never asked, 1 waiting to start, 2 running, 3 done)   1 poly8 (SA_POLY)   2 flags8 (bit 0:
 *   SA_BENCH_BOOT)   3 n8 (voice counts measured)   4 frames16 (a block)   6 budget32 (a block's real time in
 *   plat_ticks24 ticks)   10 + 12 k: voices8, sounding8 (the fewest voices sounding in a block), blocks16,
 *   avg32, worst32 (ticks a block) */
#include "sa_synth.h"

#ifndef SA_BENCH_BOOT
#define SA_BENCH_BOOT 0
#endif
#define SA_BENCH_SECONDS 3u
#define SA_BENCH_FRAMES APP_BLOCK
#define SA_BENCH_NC 4u
#define SA_BENCH_REPLY (10u + 12u * SA_BENCH_NC)

/* the dense voice: ALGORITHM 1 (six operators, five modulating), every operator held at full level, OP6's feedback
 * 7, every operator on the LFO's amplitude (AMS 1), the LFO on pitch too: every part of FM6 works each block */
#define SAB_OP(ol, fc, det) 99, 99, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 1, 0, ol, 0, fc, 0, det
static const uint8_t SA_BENCH_VOICE[SA_VCED_LEN] = {
    SAB_OP(99, 1, 7), SAB_OP(95, 3, 8), SAB_OP(90, 1, 6), SAB_OP(90, 1, 7), SAB_OP(85, 2, 9), SAB_OP(90, 1, 7),
    99, 99, 99, 99, 50, 50, 50, 50, 0, 7, 1, 30, 0, 20, 20, 1, 0, 3, 24,
    'B', 'E', 'N', 'C', 'H', ' ', 'F', 'M', '6', ' '};
#undef SAB_OP
static const uint8_t SA_BENCH_N[SA_BENCH_NC] = {1, 8, 12, 16};

typedef struct {
    uint8_t voices, sounding;
    uint16_t blocks;
    uint32_t sum, worst;                         /* ticks: all the blocks', the longest */
} sab_result_t;
static struct {
    uint8_t state, cfg, n;                       /* n: the voice counts done */
    sab_result_t r[SA_BENCH_NC];
} sab;
static int32_t sab_lr[2u * SA_BENCH_FRAMES] PLAT_POOL;

static void sa_bench_cfg(void)                  /* the next voice count: the synth afresh, the keys down */
{
    uint32_t k, n = SA_BENCH_N[sab.cfg];
    sa_synth_init();
    sa_synth_load_vced(SA_BENCH_VOICE);
    for (k = 0; k < n; k++)
        sa_synth_note_on(36u + 5u * k, 100u);
    memset(&sab.r[sab.n], 0, sizeof sab.r[0]);
    sab.r[sab.n].voices = (uint8_t)n;
    sab.r[sab.n].sounding = 255;
}

static void sa_bench_init(void) { sab.state = SA_BENCH_BOOT ? 1u : 0u; }

/* a block of the bench (main loop) */
static void sa_bench_step(void)
{
    uint32_t t0, dt, s;
    if (sab.state == 1u) {
        if (plat_ms() < 3000u || plat_ble_state() == BLE_STARTING)
            return;
        sab.state = 2;
        sab.cfg = sab.n = 0;
        sa_bench_cfg();
    }
    if (sab.state != 2u)
        return;
    t0 = plat_ticks24();
    sa_synth_render(sab_lr, SA_BENCH_FRAMES, 4096u);
    dt = plat_ticks24() - t0;
    s = sa_synth_voices();
    {
        sab_result_t *r = &sab.r[sab.n];
        r->sum += dt;
        r->worst = dt > r->worst ? dt : r->worst;
        r->sounding = (uint8_t)(s < r->sounding ? s : r->sounding);
        if (++r->blocks < SA_BENCH_SECONDS * SA_FS / SA_BENCH_FRAMES)
            return;
    }
    sab.n++;
    while (++sab.cfg < SA_BENCH_NC && SA_BENCH_N[sab.cfg] > SA_POLY)
        ;
    if (sab.cfg < SA_BENCH_NC) {
        sa_bench_cfg();
    } else {
        sab.state = 3;
        sa_synth_init();                         /* (silent and idle again) */
    }
}

/* SA_BENCH: body 01 starts (again) a run; the reply is the state and what was measured so far */
static uint32_t sa_bench_cmd(const uint8_t *body, uint32_t len, uint8_t *out)
{
    uint32_t k;
    if (len >= 1u && body[0] == 1u && sab.state != 2u) {
        sab.state = 1;
        sab.n = 0;
    }
    out[0] = sab.state;
    out[1] = SA_POLY;
    out[2] = SA_BENCH_BOOT ? 1u : 0u;
    out[3] = sab.n;
    rf_wr16(out + 4, SA_BENCH_FRAMES);
    rf_wr32(out + 6, (uint32_t)((uint64_t)SA_BENCH_FRAMES * PLAT_TICKS_PER_US * 1000000u / SA_FS));
    for (k = 0; k < SA_BENCH_NC; k++) {
        uint8_t *o = out + 10u + 12u * k;
        uint32_t b = k < sab.n ? sab.r[k].blocks : 0u;
        o[0] = k < sab.n ? sab.r[k].voices : 0u;
        o[1] = k < sab.n ? sab.r[k].sounding : 0u;
        rf_wr16(o + 2, b);
        rf_wr32(o + 4, b ? sab.r[k].sum / b : 0u);
        rf_wr32(o + 8, k < sab.n ? sab.r[k].worst : 0u);
    }
    return 10u + 12u * sab.n;
}
