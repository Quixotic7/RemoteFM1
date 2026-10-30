/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone instrument's DX7 (sa_synth.h): saadet's FM6, unchanged in its maths, on a small instrument of its
 * own. FM6 is eng_fm6.c (with fm6_core.c, fm6_msfa_ops.h, fm6_voice_policy.h: firmware/third_party/saadet-fm6),
 * compiled here against fm6_shim.h, the part of saadet's instrument it reads. This file is the rest of that
 * instrument, as saadet's voice.c is for an engine with its own envelopes: the POLY allocation, the gates, the
 * amplitude ramp at a voice's start and end, and per block: fm6_block (the patch, the controllers, the LFO, the voices
 * that rang out), fm6_render for each sounding voice, fm6_post (the DC filter). Played the same, it renders the same
 * samples as saadet (tests/fm6_parity.sh). */
#include <stdint.h>
#if __has_include(<string.h>)
#include <string.h>                      /* (before the section names go, below: the system's headers as they are) */
#endif

#ifdef PLAT_HOST
#define SA_POOL
#else
#define SA_POOL __attribute__((section(".pool")))   /* fm1-base's pool: RAM, zeroed at boot */
#endif
#define FM6_SIN_ATTR SA_POOL             /* the sine is read every sample: RAM (filled by fm6_tables_init) */
#if defined(__APPLE__)
/* saadet places its DSP code in sections of its own (".dsp_text", ".audio_text": fm1-base's app.ld, patched, puts
 * them in .text); a Mac's Mach-O wants "segment,section" names, so the simulator and the tests drop them */
#define section(name) unused
#endif
#include "fm6_shim.h"

/* the DX7's INIT VOICE, packed (VMEM): every operator R 99 99 99 99, L 99 99 99 0, BREAK POINT C3 (39), no scaling,
 * ratio 1.00, detune 0, output 0 but OP1's 99; ALGORITHM 1, FEEDBACK 0, OSC KEY SYNC on; LFO speed 35, delay 0,
 * no depth, sync on, triangle, pitch sensitivity 3; pitch EG rates 99, levels 50; TRANSPOSE C3 (24) */
#define SA_INIT_OP(ol) 99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 7 << 3, 0, ol, 1 << 1, 0
static const uint8_t FM6_INIT[SA_VMEM_LEN] = {
    SA_INIT_OP(0), SA_INIT_OP(0), SA_INIT_OP(0), SA_INIT_OP(0), SA_INIT_OP(0), SA_INIT_OP(99),   /* OP6 .. OP1 */
    99, 99, 99, 99, 50, 50, 50, 50, 0, 1 << 3, 35, 0, 0, 0, 3 << 4 | 0 << 1 | 1, 24,
    'I', 'N', 'I', 'T', ' ', 'V', 'O', 'I', 'C', 'E'};
#undef SA_INIT_OP

#define FM6_NO_FACTORY                   /* (saadet's factory voices and engine table: not here) */
#include "../third_party/saadet-fm6/eng_fm6.c"
#if defined(__APPLE__)
#undef section
#endif
_Static_assert(SA_VCED_LEN == FP_SIZE && SA_VMEM_LEN == FM6_PACKED && SA_NFN == FN_ENGINE, "the API's sizes");

static fm6_part_t sa_part SA_POOL;      /* FM6's state of the part: its voices, LFO, controllers, DC filter */
static fm6_part_t *fm6_part(uint32_t part)
{
    (void)part;
    return &sa_part;
}

static struct {
    uint32_t age;                        /* the voices' starts, counted (the oldest is stolen first) */
    uint8_t rr;                          /* round robin: where the search for a free voice starts */
    uint8_t pedal;                       /* the sustain pedal is down */
} sa;

#define T (&trk[0])

/* ------------------------------------------------------------------ the voices --- */
/* the voice for a new note (saadet's POLY allocation): the same note sounding retriggers its voice; else a free
 * voice, round robin; else the oldest released one; else the oldest held one, but never the lowest held note */
static voice_t *sa_alloc(uint32_t note)
{
    voice_t *v = T->v;
    uint32_t i, best = NVOICE, low = NVOICE;
    for (i = 0; i < NVOICE; i++)
        if (v[i].active && v[i].note == note)
            return &v[i];
    for (i = 0; i < NVOICE && best == NVOICE; i++) {
        uint32_t k = (sa.rr + i) % NVOICE;
        if (!v[k].active)
            best = k;
    }
    sa.rr = (uint8_t)(((best == NVOICE ? sa.rr : best) + 1u) % NVOICE);
    if (best < NVOICE)
        return &v[best];
    for (i = 0; i < NVOICE; i++)
        if (v[i].gate && (low == NVOICE || v[i].note < v[low].note))
            low = i;
    for (i = 0; i < NVOICE; i++)
        if (!v[i].gate && v[i].stage != 4u && (best == NVOICE || v[i].age < v[best].age))
            best = i;
    if (best == NVOICE)
        for (i = 0; i < NVOICE; i++)
            if (i != low && (best == NVOICE || v[i].age < v[best].age))
                best = i;
    return &v[best == NVOICE ? 0 : best];
}

/* a note starts on voice v (saadet's voice_start): a voice that was silent ramps up from 0 in its first block, one
 * that sounded goes on from its level (FM6 restarts its envelopes from where they are) */
static void sa_start(voice_t *v, uint32_t note, uint32_t vel)
{
    int sounding = v->active && v->stage != 0;
    voice_was = (uint8_t)(!v->active ? 0u : v->gate ? 2u : 1u);
    v->note = (uint8_t)note;
    v->vel = v->mvel = (uint8_t)vel;
    v->gate = v->active = 1;
    v->stage = 1;
    v->sus = 0;
    v->age = ++sa.age;
    if (!sounding)
        v->env_out = 0;
    fm6_note_on(T, v);
}

static void sa_release(voice_t *v)
{
    v->gate = 0;
    v->stage = 3;
    v->sus = 0;
}

/* one block of SA_CTL frames into out (saadet's track_render for FM6) */
static void sa_block(int32_t *out)
{
    track_t *t = T;
    uint32_t i, nr = 0, live = 0;
    for (i = 0; i < NVOICE; i++)
        live |= t->v[i].active;
    memset(out, 0, CTL * sizeof *out);
    fm6_block(t);                        /* the patch, the controllers, the LFO, the voices rung out (Dexed's) */
    if (!live) {
        fm6_post(t, out, CTL, 0);        /* (the DC filter starts afresh) */
        return;
    }
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &t->v[i];
        vmod_t m;
        int32_t env = 32767;
        if (!v->active)
            continue;
        if (v->stage == 4u || fm6_done(t, v)) {   /* given up (FM6's panic), or FM6's envelopes are over: this */
            v->active = v->gate = v->sus = 0;     /* block fades it to 0, and it is free */
            v->stage = 0;
            env = 0;
        }
        m.amp0 = v->env_out;
        m.amp1 = env;
        v->env_out = env;
        m.cutoff = 0;
        m.shape = 64 << 8;
        m.plog = 0;
        fm6_render(t, v, out, CTL, &m);
        nr++;
    }
    fm6_post(t, out, CTL, nr);
}

/* ------------------------------------------------------------------ the API --- */
void sa_synth_init(void)
{
    fm6_tables_init();
    memset(trk, 0, sizeof trk);
    memset(&sa_part, 0, sizeof sa_part);
    memset(fm6_eff, 0, sizeof fm6_eff);
    memset(&sa, 0, sizeof sa);
    fm6_init();                          /* Dexed's function settings; the INIT VOICE */
    T->p[P_VOICE] = V_POLY;
}

void sa_synth_load_vced(const uint8_t *v) { fm6_put_patch(0, v, 1); }
void sa_synth_edit_vced(const uint8_t *v) { fm6_put_patch(0, v, 0); }

void sa_synth_vced_of_vmem(const uint8_t *b, uint8_t *v)
{
    uint8_t s[FP_SIZE + 1u];
    fm6_unpack(b, s);
    memcpy(v, s, FP_SIZE);
}

void sa_synth_load_vmem(const uint8_t *b)
{
    uint8_t s[FP_SIZE + 1u];
    fm6_unpack(b, s);
    fm6_put_patch(0, s, 1);
}

void sa_synth_voice(uint8_t *v) { memcpy(v, fm6_patch[0], FP_SIZE); }
void sa_synth_function(uint32_t fn, int32_t value) { fm6_fn_set(0, fn, value); }

void sa_synth_note_on(uint32_t note, uint32_t vel)
{
    if (!vel) {
        sa_synth_note_off(note);
        return;
    }
    if (note > 127u)
        return;
    sa_start(sa_alloc(note), note, vel > 127u ? 127u : vel);
}

void sa_synth_note_off(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &T->v[i];
        if (v->gate && v->note == note) {
            if (sa.pedal)
                v->sus = 1;              /* held by the pedal */
            else
                sa_release(v);
        }
    }
}

static void sa_pedal(int down)
{
    uint32_t i;
    sa.pedal = (uint8_t)down;
    if (!down)
        for (i = 0; i < NVOICE; i++)
            if (T->v[i].gate && T->v[i].sus)
                sa_release(&T->v[i]);
}

void sa_synth_bend(int32_t bend) { T->bend_raw = clamp(bend, -8192, 8191); }
void sa_synth_aftertouch(uint32_t value) { T->at = (uint8_t)(value & 127u); }

void sa_synth_cc(uint32_t cc, uint32_t value)
{
    value &= 127u;
    switch (cc) {
    case 1: T->mw = (uint8_t)value; break;
    case 2: T->breath = (uint8_t)value; break;
    case 4: T->foot = (uint8_t)value; break;
    case 5: fm6_fn_set(0, FN_PTIME, (int32_t)value); break;   /* portamento time (saadet's mod.c) */
    case 64: sa_pedal(value >= 64u); break;
    case 65: T->porta = (uint8_t)(value >= 64u); break;
    case 120: sa_synth_all_sound_off(); break;
    case 121:                                                 /* reset: the wheel, the controllers, bend, pedal */
        T->mw = T->breath = T->foot = T->at = T->porta = 0;
        T->bend_raw = 0;
        sa_pedal(0);
        break;
    case 123: sa_synth_all_notes_off(); break;
    default: break;
    }
}

void sa_synth_all_notes_off(void)
{
    uint32_t i;
    sa.pedal = 0;
    for (i = 0; i < NVOICE; i++)
        if (T->v[i].gate)
            sa_release(&T->v[i]);
}

void sa_synth_all_sound_off(void)
{
    uint32_t i;
    sa.pedal = 0;
    for (i = 0; i < NVOICE; i++)
        if (T->v[i].active) {
            T->v[i].gate = T->v[i].sus = 0;
            T->v[i].stage = 4;           /* the next block fades it out */
        }
}

void sa_synth_render_mono(int32_t *out, uint32_t n)
{
    uint32_t k;
    for (k = 0; k + CTL <= n; k += CTL)
        sa_block(out + k);
    for (; k < n; k++)
        out[k] = 0;                      /* (n not a multiple of SA_CTL: the rest silent) */
}

void sa_synth_render(int32_t *lr, uint32_t n, uint32_t gain_q12)
{
    int32_t b[CTL];
    uint32_t k, i;
    float g = (float)gain_q12 * (1.0f / 4096.0f);   /* (a float multiply: no 64-bit one beside the loads) */
    for (k = 0; k < n; k += CTL) {
        uint32_t m = n - k < (uint32_t)CTL ? n - k : (uint32_t)CTL;
        if (m < (uint32_t)CTL)
            memset(b, 0, sizeof b);
        else
            sa_block(b);
        for (i = 0; i < m; i++) {
            float f = (float)b[i] * g;
            int32_t y = f > 8388607.0f ? 8388607 : f < -8388607.0f ? -8388607 : (int32_t)f;
            lr[2u * (k + i)] = lr[2u * (k + i) + 1u] = y;
        }
    }
}

uint32_t sa_synth_voices(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += T->v[i].active;
    return n;
}
