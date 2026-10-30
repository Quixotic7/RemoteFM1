/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The reference for RemoteFM1's FM6: the performances of tests/sa_scenarios.h played on saadet's own instrument
 * (its firmware sources, voice.c's allocation and track_render, eng_fm6.c with its own tables), each written as
 * OUTDIR/NAME.raw (int32, mono, part 1's dry output), for tests/fm6_parity.sh to compare with tests/sa_synth_test.c's.
 * Built against a saadet checkout (-I SAADET/firmware/src -I SAADET/build/gen, its generated headers made by its own
 * tools/generate_assets.py); nothing of saadet's is copied here. The part as saadet's FM6 test sets one up: FM6 in
 * POLY, the macros at 0, the parameters' defaults; FM6's voices weighted one budget point each (GLO > WEIGHTS) so
 * its budget never cuts a voice (sa_synth has none) and the 16 voices allocate as RemoteFM1's with SA_POLY 16.
 *   fm6_ref OUTDIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "melodee_tables.h"
#define memset saadet_memset             /* (saadet's own memset, memcpy, memcmp beside the C library's) */
#define memcpy saadet_memcpy
#define memcmp saadet_memcmp
#include "libc.c"                        /* its string helpers and rng */
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;   /* (the panel's input, which core.h's users read) */
#include "core.h"
#include "engines.c"
#include "params.c"
#include "mod.c"
#include "voice.c"
#include "mix.c"                         /* (the LFO's tempo sync: div_samples) */
#include "sa_scenarios.h"

static track_t *const T = &trk[0];

static void setup(const uint8_t *voice)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    eng_state_reset();
    memset(fm6_eff, 0, sizeof fm6_eff);
    fm6_fn_reset();
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (i = 0; i < P_E0; i++)
        T->p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        T->p[P_E0 + i] = 0;
    T->p[P_VOICE] = V_POLY;
    T->eng_req = T->engine = (uint8_t)ENGI_FM6;
    settings_vpoints[2] = 1;             /* FM6: one budget point a voice */
    fm6_put_patch(0, voice, 1);
}

static uint32_t play(const sc_scenario_t *sc, int32_t *out)
{
    uint32_t nb = sc_blocks(sc->ev), b;
    const sc_event_t *e = sc->ev;
    setup(SC_VOICES[sc->voice]);
    for (b = 0; b < nb; b++) {
        for (; e->type != E_END && e->block == b; e++) {
            uint8_t v[FP_SIZE + 1u];
            switch (e->type) {
            case E_ON: trk_note_on(T, (uint32_t)e->a, (uint32_t)e->b); break;
            case E_OFF: trk_note_off(T, (uint32_t)e->a); break;
            case E_BEND: T->bend_raw = e->a; break;
            case E_CC: mod_midi(T, 0xB0u, (uint32_t)e->a, (uint32_t)e->b); break;
            case E_AT: mod_midi(T, 0xD0u, (uint32_t)e->a, 0); break;
            case E_FN: fm6_fn_set(0, (uint32_t)e->a, e->b); break;
            case E_LOAD: fm6_put_patch(0, SC_VOICES[e->a], 1); break;
            case E_EDIT:
                memcpy(v, fm6_patch[0], sizeof v);
                v[e->a] = (uint8_t)e->b;
                fm6_put_patch(0, v, 0);
                break;
            }
        }
        track_render(T, out + b * CTL, CTL);
    }
    return nb * CTL;
}

static int32_t wav[44100 * 5];

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    uint32_t k;
    if (!audio_batch_active && NVOICE == 16)
        for (k = 0; k < SC_N; k++) {
            char path[512];
            uint32_t n = play(&SC_SCENARIOS[k], wav);
            FILE *f;
            snprintf(path, sizeof path, "%s/%s.raw", dir, SC_SCENARIOS[k].name);
            if (!(f = fopen(path, "wb"))) {
                printf("fm6_ref: cannot write %s\n", path);
                return 1;
            }
            fwrite(wav, 4, n, f);
            fclose(f);
        }
    else {
        printf("fm6_ref: saadet's instrument is not as expected (batch %d, NVOICE %d)\n", audio_batch_active, NVOICE);
        return 1;
    }
    printf("fm6_ref: %u renders on saadet's instrument in %s\n", (unsigned)SC_N, dir);
    return 0;
}
