/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone DX7 (firmware/sa/sa_synth.c, saadet's FM6) on the computer: it plays the performances of
 * tests/sa_scenarios.h and writes each as OUTDIR/NAME.raw (int32 little endian, mono, the 24-bit scale: what
 * tests/fm6_parity.sh compares with saadet's own instrument) and NAME.wav (16-bit, to listen to). Then the checks
 * that need no saadet: the INIT VOICE is a sine at the key's pitch, every render sounds and the voices differ, the
 * packed INIT VOICE is the DX7's, the sustain pedal, all notes off, all sound off, the stereo render.
 *   sa_synth_test OUTDIR        (tests/fm6_parity.sh builds it; SA_POLY from -D, 8 by default)
 * Ends "sa_synth: N checks, all passed" (exit 0) or with the failures (exit 1). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sa_synth.h"
#include "sa_scenarios.h"

static int bad, checks;
static void check(const char *what, int ok)
{
    checks++;
    if (getenv("VERBOSE"))
        printf("sa_synth: %-80s %s\n", what, ok ? "ok" : "FAIL");
    else if (!ok) {
        printf("sa_synth: %-80s FAIL\n", what);
    }
    bad += !ok;
}

static void put_wav(const char *path, const int32_t *x, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t v, i;
    uint16_t h[4] = {1, 1, 2, 16};       /* PCM, mono, 2 bytes a frame, 16 bits */
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    v = 36 + n * 2;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    fwrite(h, 2, 2, f);
    v = SA_FS;
    fwrite(&v, 4, 1, f);
    v = SA_FS * 2;
    fwrite(&v, 4, 1, f);
    fwrite(h + 2, 2, 2, f);
    fwrite("data", 1, 4, f);
    v = n * 2;
    fwrite(&v, 4, 1, f);
    for (i = 0; i < n; i++) {
        int32_t s = x[i] >> 8;
        int16_t y = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        fwrite(&y, 2, 1, f);
    }
    fclose(f);
}

/* the performance on sa_synth: events at their block, then the block */
static uint32_t play(const sc_scenario_t *sc, int32_t *out)
{
    uint32_t nb = sc_blocks(sc->ev), b;
    const sc_event_t *e = sc->ev;
    sa_synth_init();
    sa_synth_load_vced(SC_VOICES[sc->voice]);
    for (b = 0; b < nb; b++) {
        for (; e->type != E_END && e->block == b; e++) {
            uint8_t v[SA_VCED_LEN];
            switch (e->type) {
            case E_ON: sa_synth_note_on((uint32_t)e->a, (uint32_t)e->b); break;
            case E_OFF: sa_synth_note_off((uint32_t)e->a); break;
            case E_BEND: sa_synth_bend(e->a); break;
            case E_CC: sa_synth_cc((uint32_t)e->a, (uint32_t)e->b); break;
            case E_AT: sa_synth_aftertouch((uint32_t)e->a); break;
            case E_FN: sa_synth_function((uint32_t)e->a, e->b); break;
            case E_LOAD: sa_synth_load_vced(SC_VOICES[e->a]); break;
            case E_EDIT:
                sa_synth_voice(v);
                v[e->a] = (uint8_t)e->b;
                sa_synth_edit_vced(v);
                break;
            }
        }
        sa_synth_render_mono(out + b * SA_CTL, SA_CTL);
    }
    return nb * SA_CTL;
}

static double rms(const int32_t *x, uint32_t a, uint32_t b)
{
    double s = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        s += (double)x[i] * x[i];
    return b > a ? sqrt(s / (b - a)) : 0;
}

static double freq(const int32_t *w, uint32_t a, uint32_t b)   /* from the rising zero crossings */
{
    uint32_t i, n = 0;
    double f0 = 0, f1 = 0;
    for (i = a + 1; i < b; i++)
        if (w[i - 1] < 0 && w[i] >= 0) {
            double x = i - 1 + -(double)w[i - 1] / (w[i] - w[i - 1]);
            if (!n++)
                f0 = x;
            f1 = x;
        }
    return n > 1 ? (n - 1) * (double)SA_FS / (f1 - f0) : 0;
}

#define MAXN (SA_FS * 5u)
static int32_t wav[SC_N][MAXN];
static uint32_t len[SC_N];

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512], what[160];
    uint32_t k, j, i;
    for (k = 0; k < SC_N; k++) {
        len[k] = play(&SC_SCENARIOS[k], wav[k]);
        snprintf(path, sizeof path, "%s/%s.raw", dir, SC_SCENARIOS[k].name);
        FILE *f = fopen(path, "wb");
        if (!f) {
            printf("sa_synth: cannot write %s\n", path);
            return 1;
        }
        fwrite(wav[k], 4, len[k], f);
        fclose(f);
        snprintf(path, sizeof path, "%s/%s.wav", dir, SC_SCENARIOS[k].name);
        put_wav(path, wav[k], len[k]);
        snprintf(what, sizeof what, "%s sounds (rms %.0f) and stays in range", SC_SCENARIOS[k].name,
                 rms(wav[k], 0, len[k]));
        {
            int32_t pk = 0;
            for (i = 0; i < len[k]; i++)
                pk = abs(wav[k][i]) > pk ? abs(wav[k][i]) : pk;
            check(what, rms(wav[k], 0, len[k]) > 20000.0 && pk < 64 * 1024 * 1024);
        }
    }
    for (k = 0; k < 6u; k++)                 /* the *_play renders: the same notes, six different voices */
        for (j = k + 1; j < 6u; j++) {
            double d = 0;
            for (i = 0; i < len[k] && i < len[j]; i++)
                d += fabs((double)wav[k][i] - wav[j][i]);
            snprintf(what, sizeof what, "%s and %s differ", SC_SCENARIOS[k].name, SC_SCENARIOS[j].name);
            check(what, d / len[k] > 10000.0);
        }
    /* INIT VOICE: OP1 alone, a sine: C4 (261.63 Hz) from 50 ms into the first note, nothing before the note */
    {
        double f = freq(wav[0], SA_FS / 20u, SA_FS / 5u);
        snprintf(what, sizeof what, "INIT VOICE plays C4 at 261.63 Hz (%.3f)", f);
        check(what, fabs(f - 261.6256) < 0.05);
    }
    /* the packed INIT VOICE sa_synth starts with is the DX7's (the scenarios' VCED) */
    {
        uint8_t v[SA_VCED_LEN];
        sa_synth_init();
        sa_synth_voice(v);
        check("the INIT VOICE at start is the DX7's", !memcmp(v, SC_VOICES[SC_INIT], SA_VCED_LEN));
    }
    /* the sustain pedal holds released keys until it comes up; all notes off releases; all sound off silences */
    {
        int32_t b[SA_CTL * 64];
        sa_synth_init();
        sa_synth_load_vced(SC_VOICES[SC_ORGAN]);   /* (sustains at full level: the voices stay until released) */
        sa_synth_cc(64, 127);
        sa_synth_note_on(60, 100);
        sa_synth_note_on(64, 100);
        sa_synth_render_mono(b, SA_CTL * 4);
        sa_synth_note_off(60);
        sa_synth_note_off(64);
        sa_synth_render_mono(b, SA_CTL * 64);
        check("the pedal holds two released keys", sa_synth_voices() == 2 && rms(b, SA_CTL * 60, SA_CTL * 64) > 1e5);
        sa_synth_cc(64, 0);
        for (i = 0; i < 200u && sa_synth_voices(); i++)
            sa_synth_render_mono(b, SA_CTL * 64);
        check("the pedal up: they ring out and end", !sa_synth_voices());
        sa_synth_note_on(48, 100);
        sa_synth_note_on(55, 100);
        sa_synth_render_mono(b, SA_CTL * 8);
        sa_synth_cc(123, 0);
        for (i = 0; i < 200u && sa_synth_voices(); i++)
            sa_synth_render_mono(b, SA_CTL * 64);
        check("all notes off: they ring out and end", !sa_synth_voices());
        sa_synth_note_on(48, 100);
        sa_synth_render_mono(b, SA_CTL * 8);
        sa_synth_all_sound_off();
        sa_synth_render_mono(b, SA_CTL * 2);
        check("all sound off: silent within a block", !sa_synth_voices() && rms(b, SA_CTL, SA_CTL * 2) < 1.0);
    }
    /* polyphony: SA_POLY keys held sound at once; one more takes the oldest but never the lowest */
    {
        int32_t b[SA_CTL * 4];
        sa_synth_init();
        sa_synth_load_vced(SC_VOICES[SC_ORGAN]);
        for (i = 0; i < SA_POLY; i++)
            sa_synth_note_on(40 + i, 100);
        sa_synth_render_mono(b, SA_CTL * 4);
        check("SA_POLY keys held, all sounding", sa_synth_voices() == SA_POLY);
        sa_synth_note_on(100, 100);
        sa_synth_render_mono(b, SA_CTL * 4);
        check("one more: still SA_POLY", sa_synth_voices() == SA_POLY);
    }
    /* the stereo render: the mono render x gain / 4096 on both sides */
    {
        int32_t m[SA_CTL * 8], lr[SA_CTL * 16];
        int ok = 1;
        sa_synth_init();
        sa_synth_load_vced(SC_VOICES[SC_EP]);
        sa_synth_note_on(60, 100);
        sa_synth_render_mono(m, SA_CTL * 8);
        sa_synth_init();
        sa_synth_load_vced(SC_VOICES[SC_EP]);
        sa_synth_note_on(60, 100);
        sa_synth_render(lr, SA_CTL * 8, 2048);
        for (i = 0; i < SA_CTL * 8; i++)
            ok &= lr[2 * i] == (int32_t)((float)m[i] * 0.5f) && lr[2 * i + 1] == lr[2 * i];
        check("stereo: mono x gain on both sides", ok && rms(m, 0, SA_CTL * 8) > 1000);
    }
    if (bad)
        printf("sa_synth: %d of %d checks FAILED\n", bad, checks);
    else
        printf("sa_synth: %d checks, all passed (SA_POLY %d, renders in %s)\n", checks, SA_POLY, dir);
    return bad != 0;
}
