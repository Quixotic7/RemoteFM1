/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The FM6 bench (STANDALONE-PLAN.md S0): what a block of the DX7 costs on the FM-1, and whether the FM-1 renders the
 * computer's samples. Part of the unity build (app.c); the synth is sa_synth.c's (not hooked to the keys or the
 * sound yet: the bench renders into a buffer of its own).
 *
 * The bench: for each voice count (1, 8, 12, 16, up to SA_POLY), on one core and then, when the second core runs
 * (fm1.toml cpu2), on two (sa_synth_cores(2): about half the voices a control block on core 1, FIZZ's split), the
 * dense voice below is held on that many keys and rendered for SA_BENCH_SECONDS of audio, block by block (APP_BLOCK
 * frames: what fm1-base's audio interrupt asks of app_render), each block timed with plat_ticks24. It runs in the
 * main loop, a block at a time (app_frame and app_input_only), so USB, the watchdog and Bluetooth go on; the audio
 * interrupt, the 2 kHz USB poll and the 1 kHz tick may land inside a measured block, so the worst block includes
 * them. FM6's per-sample and per-voice code runs from RAM (firmware/patches/fm1-base/0005), its sine in RAM.
 *
 * The hash: sa_hash.h's performance rendered on one core, then on two, each hashed (firmware/tools/sa_bench.py
 * --hash compares them with the computer's: firmware/tools/sa_hash_host.c).
 *
 * The bench build (./build.sh --bench: SA_BENCH_BOOT, SA_POLY 16) starts the bench at boot; any build starts it on
 * SA_BENCH with body 01, the hash with 02. Each waits until 3 s after boot and until Bluetooth's start is over (the
 * radio's trim must not meet heavy flash reads). SA_BENCH's reply (firmware/tools/sa_bench.py reads it), little endian:
 *   0 layout8 (2)   1 state8 (the bench: 0 never asked, 1 waiting, 2 running, 3 done)   2 poly8 (SA_POLY)
 *   3 flags8 (bit 0 SA_BENCH_BOOT, bit 1 the second core built, bit 2 it runs now)   4 n8 (rows)   5 gave_up8
 *   (plat_cpu2_gave_up)   6 frames16 (a block)   8 budget32 (a block's real time in plat_ticks24 ticks)
 *   12 misreads32 (plat_cpu2_bad)   16 c1_stack32 (bytes)   20 hash_state8 (as state)   21 hash_n8 (hashes done)
 *   22 0   24 hash_one32 (one core)   28 hash_two32 (two cores)   32 hash_blocks32   36 handed32   40 kept32   44
 *   dropped32 (sa_synth_worker_stats since boot: blocks whose share core 1 took, did not take, dropped)
 *   48 + 16 k: voices8, cores8, sounding8 (the fewest voices sounding in a block), 0, blocks32, avg32, worst32 */
#include "sa_synth.h"
#include "sa_hash.h"

#ifndef SA_BENCH_BOOT
#define SA_BENCH_BOOT 0
#endif
#define SA_BENCH_SECONDS 3u
#define SA_BENCH_FRAMES APP_BLOCK
#define SA_BENCH_NC 4u
#define SA_BENCH_ROWS (2u * SA_BENCH_NC)
#define SA_BENCH_REPLY (48u + 16u * SA_BENCH_ROWS)
#define SA_HASH_STEP 16u                         /* the hash's blocks a main-loop turn (32 frames each) */

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
    uint8_t voices, cores, sounding;
    uint32_t blocks, sum, worst;                 /* ticks: all the blocks', the longest */
} sab_result_t;
static struct {
    uint8_t state, hstate;                       /* the bench's, the hash's: 0 never asked, 1 waiting, 2 running, 3 done */
    uint8_t cfg, n;                              /* the row running (voice count k + SA_BENCH_NC x (cores - 1)); done */
    uint8_t hcores, hn;                          /* the hash: the cores of the run, runs done */
    uint32_t hblock, hash[2];
    sab_result_t r[SA_BENCH_ROWS];
} sab;
static int32_t sab_lr[2u * SA_BENCH_FRAMES] PLAT_POOL;

static uint32_t sab_cores_max(void)
{
#if FM1B_CPU2
    return plat_cpu2_on() ? 2u : 1u;
#else
    return 1u;
#endif
}

static void sa_bench_idle(void)                  /* the synth silent, at the build's own split */
{
    sa_synth_cores(sab_cores_max());
    sa_synth_init();
}

static void sa_bench_init(void)
{
#if FM1B_CPU2
    sa_synth_worker(plat_cpu2_run, plat_cpu2_wait);   /* (core 1 started in app_init, before: rfm_main.c) */
#endif
    sa_bench_idle();
    sab.state = SA_BENCH_BOOT ? 1u : 0u;
}

static int sab_cfg_ok(uint32_t c)               /* row c: its voices fit SA_POLY, its cores run */
{
    return SA_BENCH_N[c % SA_BENCH_NC] <= SA_POLY && c / SA_BENCH_NC < sab_cores_max();
}

static void sa_bench_cfg(void)                  /* the row's run: the synth afresh, the keys down */
{
    uint32_t k, n = SA_BENCH_N[sab.cfg % SA_BENCH_NC], cores = 1u + sab.cfg / SA_BENCH_NC;
    sa_synth_cores(cores);
    sa_synth_init();
    sa_synth_load_vced(SA_BENCH_VOICE);
    for (k = 0; k < n; k++)
        sa_synth_note_on(36u + 5u * k, 100u);
    memset(&sab.r[sab.n], 0, sizeof sab.r[0]);
    sab.r[sab.n].voices = (uint8_t)n;
    sab.r[sab.n].cores = (uint8_t)cores;
    sab.r[sab.n].sounding = 255;
}

static int sab_may_start(void) { return plat_ms() >= 3000u && plat_ble_state() != BLE_STARTING; }

static void sa_hash_begin(void)
{
    sa_synth_cores(sab.hcores);
    sa_synth_init();
    sab.hblock = 0;
    sab.hash[sab.hcores - 1u] = SA_HASH_SEED;
}

/* a step of the hash, then a block of the bench (main loop) */
static void sa_bench_step(void)
{
    uint32_t t0, dt, s, k;
    if (sab.hstate == 1u && sab.state != 2u && sab_may_start()) {
        sab.hstate = 2;
        sab.hn = 0;
        sab.hcores = 1;
        sa_hash_begin();
    }
    if (sab.hstate == 2u) {
        for (k = 0; k < SA_HASH_STEP && sab.hblock < SA_HASH_BLOCKS; k++, sab.hblock++)
            sab.hash[sab.hcores - 1u] = sa_hash_block(sab.hblock, sab.hash[sab.hcores - 1u]);
        if (sab.hblock < SA_HASH_BLOCKS)
            return;
        sab.hn++;
        if (++sab.hcores <= sab_cores_max()) {
            sa_hash_begin();
        } else {
            sab.hstate = 3;
            sa_bench_idle();
        }
        return;
    }
    if (sab.state == 1u) {
        if (!sab_may_start())
            return;
        sab.state = 2;
        sab.cfg = sab.n = 0;
        while (sab.cfg < SA_BENCH_ROWS && !sab_cfg_ok(sab.cfg))
            sab.cfg++;
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
    while (++sab.cfg < SA_BENCH_ROWS && !sab_cfg_ok(sab.cfg))
        ;
    if (sab.cfg < SA_BENCH_ROWS) {
        sa_bench_cfg();
    } else {
        sab.state = 3;
        sa_bench_idle();
    }
}

/* SA_BENCH: body 01 starts (again) the bench, 02 the hash; the reply is where they are and what they found */
static uint32_t sa_bench_cmd(const uint8_t *body, uint32_t len, uint8_t *out)
{
    uint32_t k, h, kp, d;
    if (len >= 1u && body[0] == 1u && sab.state != 2u && sab.hstate != 2u) {
        sab.state = 1;
        sab.n = 0;
    }
    if (len >= 1u && body[0] == 2u && sab.state != 2u && sab.hstate != 2u) {
        sab.hstate = 1;
        sab.hn = 0;
    }
    memset(out, 0, SA_BENCH_REPLY);
    out[0] = 2;
    out[1] = sab.state;
    out[2] = SA_POLY;
#if FM1B_CPU2
    out[3] = (uint8_t)((SA_BENCH_BOOT ? 1u : 0u) | 2u | (plat_cpu2_on() ? 4u : 0u));
    out[5] = (uint8_t)plat_cpu2_gave_up();
    rf_wr32(out + 12, plat_cpu2_bad());
    rf_wr32(out + 16, plat_cpu2_stack());
#else
    out[3] = SA_BENCH_BOOT ? 1u : 0u;
#endif
    out[4] = sab.n;
    rf_wr16(out + 6, SA_BENCH_FRAMES);
    rf_wr32(out + 8, (uint32_t)((uint64_t)SA_BENCH_FRAMES * PLAT_TICKS_PER_US * 1000000u / SA_FS));
    out[20] = sab.hstate;
    out[21] = sab.hn;
    rf_wr32(out + 24, sab.hn >= 1u ? sab.hash[0] : 0u);
    rf_wr32(out + 28, sab.hn >= 2u ? sab.hash[1] : 0u);
    rf_wr32(out + 32, SA_HASH_BLOCKS);
    sa_synth_worker_stats(&h, &kp, &d);
    rf_wr32(out + 36, h);
    rf_wr32(out + 40, kp);
    rf_wr32(out + 44, d);
    for (k = 0; k < sab.n; k++) {
        uint8_t *o = out + 48u + 16u * k;
        const sab_result_t *r = &sab.r[k];
        o[0] = r->voices;
        o[1] = r->cores;
        o[2] = r->sounding;
        rf_wr32(o + 4, r->blocks);
        rf_wr32(o + 8, r->blocks ? r->sum / r->blocks : 0u);
        rf_wr32(o + 12, r->worst);
    }
    return 48u + 16u * sab.n;
}
