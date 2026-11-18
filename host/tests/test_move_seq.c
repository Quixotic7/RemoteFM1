/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's sequencer (Track 4): the data model, steps and polyphony, lengths and pages, playback on the internal
 * clock (exact frames), the external MIDI clock, quantized launches, scenes, mutes and every path that must let go
 * of sounding notes, recording, Capture, copy / cut / paste, undo / redo, CC locks, scale and root on the Move, the
 * project grid and its actions, the project blob (round trip, and a fuzzed loader), the autosave and three golden
 * screens. Input goes in as the Move's raw USB-MIDI packets through mr_decode, as in test_move.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "move_int.h"
#include "moveremote/client.h"
#include "rfm/move.h"

/* ---- the actions, with absolute frames; the notes sounding; the host's project store */
typedef struct { rfm_move_act_t a; uint64_t at; } act_t;
static act_t acts[8192];
static int nact;
static uint64_t base;                                   /* the absolute frame of the current advance call */
static int sounding[16][128];
static uint8_t store[RFM_MOVE_PROJECTS][RFM_MOVE_PROJECT_MAX];
static uint32_t store_n[RFM_MOVE_PROJECTS];
static int answer_sync, nsave, nload, ndel, last_slot;
static rfm_move_t *answer_m;

static void on_act(void *ctx, const rfm_move_act_t *a)
{
    (void)ctx;
    if (nact < 8192)
        acts[nact].a = *a, acts[nact].at = base + a->frame, nact++;
    if (a->kind == RFM_MOVE_ACT_MIDI && a->n == 3) {
        const int st = a->bytes[0] & 0xF0, ch = a->bytes[0] & 15, n = a->bytes[1];
        if (st == 0x90 && a->bytes[2])
            sounding[ch][n]++;
        else if (st == 0x80 || st == 0x90)
            sounding[ch][n]--;
    }
    if (a->kind == RFM_MOVE_ACT_PROJECT_SAVE) {
        nsave++, last_slot = a->index;
        if (a->index < RFM_MOVE_PROJECTS && a->len <= RFM_MOVE_PROJECT_MAX && a->data) {
            memcpy(store[a->index], a->data, a->len);
            store_n[a->index] = a->len;
        }
    }
    if (a->kind == RFM_MOVE_ACT_PROJECT_LOAD) {
        nload++, last_slot = a->index;
        if (answer_sync && answer_m)
            rfm_move_project_load(answer_m, a->index, store[a->index], store_n[a->index]);
    }
    if (a->kind == RFM_MOVE_ACT_PROJECT_DELETE)
        ndel++, last_slot = a->index, store_n[a->index] = 0;
}
static void clear(void) { nact = 0; }
static int stuck(void)
{
    int c, n, s = 0;
    for (c = 0; c < 16; c++)
        for (n = 0; n < 128; n++)
            s += sounding[c][n] != 0;
    return s;
}
static void reset_sounding(void) { memset(sounding, 0, sizeof sounding); }

static uint32_t now_ms;
static void raw(rfm_move_t *m, uint8_t st, uint8_t d1, uint8_t d2)
{
    const uint8_t pkt[4] = { (uint8_t)(st >> 4), st, d1, d2 };
    mr_event_t ev;
    mr_decode(pkt, 0, &ev);
    rfm_move_input(m, &ev, now_ms += 5);
}
static void pad(rfm_move_t *m, int p, int vel) { raw(m, vel ? 0x90 : 0x80, (uint8_t)(MR_NOTE_PAD0 + p), (uint8_t)vel); }
static void step(rfm_move_t *m, int s, int down) { raw(m, down ? 0x90 : 0x80, (uint8_t)(MR_NOTE_STEP0 + s), down ? 127 : 0); }
static void tap_step(rfm_move_t *m, int s) { step(m, s, 1), step(m, s, 0); }
static void button(rfm_move_t *m, int cc, int down) { raw(m, 0xB0, (uint8_t)cc, down ? 127 : 0); }
static void press(rfm_move_t *m, int cc) { button(m, cc, 1), button(m, cc, 0); }
static void knob(rfm_move_t *m, int k, int d) { raw(m, 0xB0, (uint8_t)(MR_CC_KNOB1 + k), (uint8_t)(d & 127)); }
static void jog(rfm_move_t *m, int d) { raw(m, 0xB0, MR_CC_JOG, (uint8_t)(d & 127)); }
static void track(rfm_move_t *m, int t, int down) { button(m, MR_CC_TRACK1 - t, down); }

#define OFF_TEST_NOTE0 602                              /* the first note in a blob */
#define RATE 48000u
#define FPT 250u                                        /* frames a tick at 120 BPM, 48 kHz */
#define FPS (FPT * 24u)                                 /* a step: 6000 */
static void adv(rfm_move_t *m, uint32_t frames)        /* in blocks of 128, as an audio callback */
{
    while (frames) {
        const uint32_t n = frames < 128 ? frames : 128;
        rfm_move_seq_advance(m, n, RATE);
        base += n, frames -= n;
    }
}
static void clock_in(rfm_move_t *m, uint8_t b)
{
    const uint8_t msg[1] = { b };
    rfm_move_seq_midi_in(m, msg, 1);
}

static int find_midi(int st, int d1, int d2, uint64_t *at)   /* the first such action */
{
    int i;
    for (i = 0; i < nact; i++)
        if (acts[i].a.kind == RFM_MOVE_ACT_MIDI && acts[i].a.bytes[0] == st && acts[i].a.bytes[1] == d1 &&
            (d2 < 0 || acts[i].a.bytes[2] == d2)) {
            if (at)
                *at = acts[i].at;
            return 1;
        }
    return 0;
}
static int count_midi(int st, int d1)
{
    int i, n = 0;
    for (i = 0; i < nact; i++)
        n += acts[i].a.kind == RFM_MOVE_ACT_MIDI && acts[i].a.bytes[0] == st && (d1 < 0 || acts[i].a.bytes[1] == d1);
    return n;
}

static rfm_move_t M, M2;                                /* (150 KB each: not on the stack) */

/* a fresh core in Sequencer mode, track view, the clock at frame 0 */
static void fresh(rfm_move_t *m)
{
    rfm_move_init(m, NULL, on_act, NULL);
    track(m, 3, 1), track(m, 3, 0);
    base = 0;
    clear();
    reset_sounding();
    nsave = nload = ndel = 0;
}
static int clip_of(const rfm_move_t *m) { return SEQ_CLIP(m->seq.sel_track, m->seq.tr[m->seq.sel_track].sel); }
static int nnotes_clip(const rfm_move_t *m, int clip)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo);
    return hi - lo;
}

/* ================================================================ the data model, steps === */
static void test_model(void)
{
    rfm_move_t *m = &M;
    int lo, hi, i;
    fresh(m);
    CHECK(m->s.mode == RFM_MOVE_MODE_SEQ && m->seq.view == 0 && m->seq.sel_track == 0);
    for (i = 0; i < 16; i++)
        CHECK(m->seq.tr[i].ch == i && m->seq.tr[i].active == 0 && m->seq.tr[i].sel == 0);
    CHECK(SEQ_CLIP(3, 5) == 3 * 16 + 5 && m->seq.len[SEQ_CLIP(15, 15)] == 16);
    /* pick a note (pad 2: E3 in C major), tap steps: they toggle that note */
    pad(m, 2, 90), pad(m, 2, 0);
    CHECK(m->seq.cur_note == 52 && m->seq.last_vel == 90);
    tap_step(m, 0), tap_step(m, 4), tap_step(m, 15);
    CHECK(m->seq.nnotes == 3 && rfmv_seq_find(m, 0, 0, 52) >= 0 && rfmv_seq_find(m, 0, 15, 52) >= 0);
    CHECK(m->seq.notes[0].vel == 90 && m->seq.notes[0].len == 24 && m->seq.notes[0].micro == 0);
    tap_step(m, 4);                                     /* again: off */
    CHECK(m->seq.nnotes == 2 && rfmv_seq_find(m, 0, 4, 52) < 0);
    /* polyphony: another note on step 1 by a tap (the step has notes: the tap adds the current one) */
    pad(m, 4, 70), pad(m, 4, 0);                        /* G3 */
    tap_step(m, 0);
    hi = rfmv_seq_range(m, 0, 0, &lo);
    CHECK(hi - lo == 2 && m->seq.notes[lo].note == 52 && m->seq.notes[lo + 1].note == 55);
    /* hold a step, pads in and out of it (not recorded, still sounding) */
    step(m, 8, 1);                                      /* empty: G3 goes in at once */
    CHECK(rfmv_seq_find(m, 0, 8, 55) >= 0);
    pad(m, 0, 100), pad(m, 0, 0);                       /* C3 in */
    pad(m, 2, 100), pad(m, 2, 0);                       /* E3 in */
    pad(m, 4, 100), pad(m, 4, 0);                       /* G3 out */
    step(m, 8, 0);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(hi - lo == 2 && m->seq.notes[lo].note == 48 && m->seq.notes[lo + 1].note == 52);
    CHECK(m->seq.cur_note == 55);                       /* (the last pad) */
    /* up to RFM_MOVE_SEQ_POLY notes a step */
    for (i = 0; i < 12; i++)
        rfmv_seq_note_put(m, 0, 10, 60 + i, 100, 24, 0);
    hi = rfmv_seq_range(m, 0, 10, &lo);
    CHECK(hi - lo == RFM_MOVE_SEQ_POLY);
    /* hold a step + a later step: the notes' length reaches it */
    step(m, 8, 1), step(m, 11, 1), step(m, 11, 0), step(m, 8, 0);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(m->seq.notes[lo].len == 4 * 24 && m->seq.notes[lo + 1].len == 4 * 24);
    CHECK(rfmv_seq_find(m, 0, 11, 55) < 0);              /* (the second step added nothing) */
    /* hold + jog: the length by steps; Shift + jog: a 6th of a step */
    step(m, 8, 1), jog(m, -2);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(m->seq.notes[lo].len == 2 * 24);
    button(m, MR_CC_SHIFT, 1), jog(m, 1), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.notes[lo].len == 2 * 24 + 4);
    step(m, 8, 0);
    CHECK(rfmv_seq_find(m, 0, 8, 48) >= 0);             /* (edited while held: no toggle on release) */
    /* lengths and pages: Loop + step 4 = 4 pages; Left / Right page; Shift + Loop doubles */
    button(m, MR_CC_LOOP, 1), tap_step(m, 3), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 64);
    press(m, MR_CC_RIGHT), press(m, MR_CC_RIGHT);
    CHECK(m->seq.page == 2);
    pad(m, 0, 100), pad(m, 0, 0);
    tap_step(m, 1);                                     /* page 3, step 2 = step 34 */
    CHECK(rfmv_seq_find(m, 0, 33, 48) >= 0);
    press(m, MR_CC_RIGHT), press(m, MR_CC_RIGHT);
    CHECK(m->seq.page == 3);                            /* (the clip's last page) */
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_LOOP), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.len[0] == 128 && rfmv_seq_find(m, 0, 33 + 64, 48) >= 0 && rfmv_seq_find(m, 0, 8 + 64, 52) >= 0);
    button(m, MR_CC_LOOP, 1), tap_step(m, 15), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 256);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_LOOP), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.len[0] == 256);                        /* (at most 16 pages) */
    button(m, MR_CC_LOOP, 1), tap_step(m, 0), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 16 && m->seq.page == 0);
    CHECK(rfmv_seq_find(m, 0, 33, 48) >= 0);            /* (past the end: kept, not played) */
    /* steps past the clip's end do nothing */
    m->seq.len[0] = 8;
    clear();
    tap_step(m, 12);
    CHECK(rfmv_seq_find(m, 0, 12, m->seq.cur_note) < 0);
    /* Track 4 held + step: the track; + jog: its channel */
    track(m, 3, 1);
    tap_step(m, 5);
    jog(m, 3);
    track(m, 3, 0);
    CHECK(m->seq.sel_track == 5 && m->seq.tr[5].ch == 8 && clip_of(m) == SEQ_CLIP(5, 0));
    pad(m, 0, 100);
    CHECK(find_midi(0x98, 48, 100, NULL));               /* the pads play the track's channel */
    pad(m, 0, 0);
    CHECK(find_midi(0x88, 48, 0, NULL) && stuck() == 0);
}

/* ================================================================ playback: exact frames === */
static void test_playback(void)
{
    rfm_move_t *m = &M;
    uint64_t at;
    int i, ons;
    fresh(m);
    pad(m, 0, 100), pad(m, 0, 0);                       /* C3 */
    tap_step(m, 0), tap_step(m, 4);
    pad(m, 2, 80), pad(m, 2, 0);                        /* E3 on step 5 too, velocity 80 */
    tap_step(m, 4);
    m->seq.notes[rfmv_seq_find(m, 0, 4, 52)].micro = 6;   /* a quarter step late */
    clear();
    CHECK(rfm_move_seq_bpm10(m) == 1200 && !rfm_move_seq_following(m));
    press(m, MR_CC_PLAY);
    CHECK(m->seq.start_pending == 1 && nact == 0);
    adv(m, 2 * 16 * FPS);                               /* two bars */
    CHECK(m->seq.playing);
    CHECK(find_midi(0x90, 48, 100, &at) && at == 0);
    CHECK(find_midi(0x80, 48, 0, &at) && at == FPS);      /* one step long */
    for (i = 0, ons = 0; i < nact; i++)
        if (acts[i].a.kind == RFM_MOVE_ACT_MIDI && acts[i].a.bytes[0] == 0x90 && acts[i].a.bytes[1] == 48) {
            static const uint64_t want[4] = { 0, 4 * FPS, 16 * FPS, 20 * FPS };
            CHECKF(ons < 4 && acts[i].at == want[ons], "C3 #%d at %llu", ons, (unsigned long long)acts[i].at);
            ons++;
        }
    CHECK(ons == 4);
    CHECK(find_midi(0x90, 52, 80, &at) && at == 4 * FPS + 6 * FPT);   /* the micro offset: 6 ticks */
    CHECK(count_midi(0x90, 52) == 2 && count_midi(0x80, 52) == 2);
    /* every action from advance carries its frame within the call; the frames grow */
    for (i = 1; i < nact; i++)
        CHECK(acts[i].at >= acts[i - 1].at);
    press(m, MR_CC_PLAY);                               /* stop */
    CHECK(!m->seq.playing && stuck() == 0);
    /* 44.1 kHz: a tick is 229.6875 frames; step 2 is due at frame ceil(5512.5) = 5513 */
    fresh(m);
    rfmv_seq_note_put(m, 0, 1, 60, 100, 24, 0);
    rfm_move_seq_play(m, 1);
    for (i = 0; i < 130; i++) {
        rfm_move_seq_advance(m, 100, 44100);
        base += 100;
    }
    CHECK(find_midi(0x90, 60, 100, &at) && at == 5513);
    CHECK(find_midi(0x80, 60, 0, &at) && at == 11025);  /* tick 48: exactly 11025 */
    rfm_move_seq_play(m, 0);
    /* microseconds as the unit (rate 1e6): 120 BPM = 5208.33 us a tick, step 2 at 125000 us */
    fresh(m);
    rfmv_seq_note_put(m, 0, 1, 61, 100, 24, 0);
    rfm_move_seq_play(m, 1);
    for (i = 0; i < 20; i++) {
        rfm_move_seq_advance(m, 10000, 1000000);
        base += 10000;
    }
    CHECK(find_midi(0x90, 61, 100, &at) && at == 125000);
    /* tempo: set, clamped */
    rfm_move_seq_set_bpm(m, 90000);
    CHECK(m->seq.bpm10 == RFM_MOVE_SEQ_BPM10_MAX);
    rfm_move_seq_set_bpm(m, 1);
    CHECK(m->seq.bpm10 == RFM_MOVE_SEQ_BPM10_MIN);
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
}

/* ================================================================ the external clock === */
static void test_external(void)
{
    rfm_move_t *m = &M;
    uint64_t at;
    int i;
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, 0, 2, 62, 100, 6, 0);
    /* the firmware's clock at 120 BPM (a clock every 1000 frames at 48 kHz): Start, then clocks */
    adv(m, 480);                                        /* (the internal clock ran a little: no matter) */
    clock_in(m, 0xFA);
    CHECK(rfm_move_seq_following(m) && m->seq.start_pending == 2 && !m->seq.playing);
    clear();
    for (i = 0; i < 96; i++) {                          /* a bar */
        clock_in(m, 0xF8);
        if (i == 0)
            CHECK(m->seq.playing && find_midi(0x90, 60, 100, &at) && at == base);   /* bar 1 on the first clock */
        adv(m, 1000);
    }
    CHECK(find_midi(0x80, 60, 0, NULL) && find_midi(0x90, 62, 100, NULL) && find_midi(0x80, 62, 0, NULL));
    CHECKF(abs(rfm_move_seq_bpm10(m) - 1200) <= 2, "measured %d", rfm_move_seq_bpm10(m));
    CHECK(m->seq.T >= 4 * 96 - 1 && m->seq.T <= 4 * 96 + 3);   /* (between clocks: at most 3 ticks ahead) */
    /* the step-3 note (tick 48 = clock 12) sounded within the clock period before clock 12 */
    {
        uint64_t on;
        find_midi(0x90, 62, 100, &on);
        CHECKF(on >= 480 + 11 * 1000 && on <= 480 + 12 * 1000, "E at %llu", (unsigned long long)on);
    }
    /* tap tempo while following: shown, no effect */
    press(m, MR_CC_SAMPLE), now_ms += 400, press(m, MR_CC_SAMPLE);
    CHECK(m->seq.bpm10 == 1200 && m->seq.msg == SEQ_MSG_EXTTAP);
    /* Stop: the sequencer stops, nothing left sounding */
    rfmv_seq_note_put(m, 0, 4, 64, 100, 24 * 8, 0);     /* a long one */
    for (i = 0; i < 20; i++)
        clock_in(m, 0xF8), adv(m, 1000);
    clock_in(m, 0xFC);
    CHECK(!m->seq.playing && stuck() == 0);
    /* clocks keep coming (stopped): Play on the Move waits for the clock's next bar */
    for (i = 0; i < 10; i++)
        clock_in(m, 0xF8), adv(m, 1000);
    press(m, MR_CC_PLAY);
    CHECK(m->seq.start_pending == 2 && !m->seq.playing);
    for (i = 0; i < 200 && !m->seq.playing; i++)
        clock_in(m, 0xF8), adv(m, 1000);
    CHECK(m->seq.playing && (m->seq.tl - m->seq.T) % RFM_MOVE_SEQ_BAR == 0 && m->seq.T <= 3);
    /* Continue from a song position: at its next bar */
    clock_in(m, 0xFC);
    {
        const uint8_t spp[3] = { 0xF2, 8, 0 };          /* 16th 8: half a bar */
        rfm_move_seq_midi_in(m, spp, 3);
    }
    CHECK(m->seq.tl == 8 * 24 - 1);
    clock_in(m, 0xFB);
    for (i = 0; i < 48; i++)
        clock_in(m, 0xF8), adv(m, 1000);
    CHECK(!m->seq.playing);
    clock_in(m, 0xF8), adv(m, 1000);
    CHECK(m->seq.playing && m->seq.tl - m->seq.T == RFM_MOVE_SEQ_BAR);
    /* the clock goes away (AUTO): after 500 ms our own clock again, still playing */
    adv(m, RATE);
    CHECK(!rfm_move_seq_following(m) && m->seq.playing);
    {
        const int32_t T = m->seq.T;
        adv(m, 16 * FPS);
        CHECK(m->seq.T == T + 384);                     /* a bar on the internal clock */
    }
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
    /* INTERNAL ignores the clock; EXTERNAL waits for it */
    rfm_move_seq_set_clock(m, RFM_MOVE_CLOCK_INTERNAL);
    clock_in(m, 0xF8);
    CHECK(!rfm_move_seq_following(m));
    rfm_move_seq_set_clock(m, RFM_MOVE_CLOCK_EXTERNAL);
    CHECK(rfm_move_seq_following(m));
    {
        const int32_t tl = m->seq.tl;
        adv(m, RATE);
        CHECK(m->seq.tl <= tl + 3 && rfm_move_seq_following(m));   /* no clock: no ticks, no timeout */
    }
    /* Shift + step 5 cycles the clock source */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 4), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.clock_mode == RFM_MOVE_CLOCK_AUTO);
}

/* ================================================================ launching, scenes, mutes, note-offs === */
static void test_launch(void)
{
    rfm_move_t *m = &M;
    uint64_t at;
    int i;
    fresh(m);
    rfmv_seq_note_put(m, SEQ_CLIP(0, 0), 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(0, 1), 0, 62, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(1, 1), 2, 64, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(2, 0), 0, 67, 100, 16 * 24 * 4, 0);   /* track 3: a four-bar note */
    press(m, MR_CC_MENU);
    CHECK(m->seq.view == 1);
    rfm_move_seq_play(m, 1);
    adv(m, 5 * FPS);                                    /* into bar 1 */
    CHECK(sounding[2][67] == 1);
    /* track 1, scene 2 (pad: column 1, the second row from the top = 16): queued for the next bar */
    clear();
    pad(m, 16, 100), pad(m, 16, 0);
    CHECK(m->seq.tr[0].queued == 1 && m->seq.tr[0].active == 0 && nact == 0);
    adv(m, 12 * FPS);
    CHECK(m->seq.tr[0].active == 1 && find_midi(0x90, 62, 100, &at) && at == 16 * FPS && !find_midi(0x90, 60, -1, NULL));
    /* press the playing clip: it stops at the next bar */
    pad(m, 16, 100), pad(m, 16, 0);
    CHECK(m->seq.tr[0].queued == SEQ_STOP);
    adv(m, 16 * FPS);
    CHECK(m->seq.tr[0].active == -1);
    /* a scene: step 2 launches scene 2 (tracks without a clip there stop: track 3's long note ends) */
    clear();
    tap_step(m, 1);
    adv(m, 16 * FPS);
    CHECK(m->seq.tr[0].active == 1 && m->seq.tr[1].active == 1 && m->seq.tr[2].active == -1);
    CHECK(find_midi(0x82, 67, 0, &at) && at == 48 * FPS && sounding[2][67] == 0);   /* (the clip change let it go) */
    adv(m, 3 * FPS);
    CHECK(find_midi(0x91, 64, 100, &at) && at == 48 * FPS + 2 * FPS);           /* (track 2: channel 2) */
    /* mute: Mute + step 2 = track 2, its sounding notes let go at once */
    rfmv_seq_note_put(m, SEQ_CLIP(1, 1), 4, 65, 100, 24 * 10, 0);
    adv(m, 16 * FPS + 5 * FPS);
    CHECK(sounding[1][65] == 1);
    button(m, MR_CC_MUTE, 1), tap_step(m, 1), button(m, MR_CC_MUTE, 0);
    CHECK(m->seq.tr[1].mute && sounding[1][65] == 0);
    clear();
    adv(m, 16 * FPS);
    CHECK(!find_midi(0x91, 64, -1, NULL));
    press(m, MR_CC_MUTE);                               /* Mute alone: the selected track (track 1) */
    CHECK(m->seq.tr[0].mute);
    press(m, MR_CC_MUTE);
    button(m, MR_CC_MUTE, 1), tap_step(m, 1), button(m, MR_CC_MUTE, 0);
    CHECK(!m->seq.tr[0].mute && !m->seq.tr[1].mute);
    /* stopped, a clip press makes it the track's clip at once; Shift + clip only selects */
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
    pad(m, 24, 100), pad(m, 24, 0);                     /* track 1, scene 1 */
    CHECK(m->seq.tr[0].active == 0);
    button(m, MR_CC_SHIFT, 1), pad(m, 17, 100), pad(m, 17, 0), button(m, MR_CC_SHIFT, 0);   /* track 2 scene 2 */
    CHECK(m->seq.sel_track == 1 && m->seq.tr[1].sel == 1);
    /* paging: Right = tracks 9-16, Down = scenes 2-5 */
    press(m, MR_CC_RIGHT), press(m, MR_CC_DOWN);
    CHECK(m->seq.track_page == 1 && m->seq.scene_off == 1);
    pad(m, 24, 100), pad(m, 24, 0);
    CHECK(m->seq.sel_track == 8 && m->seq.tr[8].sel == 1);
    for (i = 0; i < 20; i++)
        press(m, MR_CC_DOWN);
    CHECK(m->seq.scene_off == 12);
    for (i = 0; i < 20; i++)
        press(m, MR_CC_UP);
    CHECK(m->seq.scene_off == 0);
}

/* every path that ends the sequencer's notes */
static void play_long(rfm_move_t *m)
{
    int t;
    for (t = 0; t < 4; t++)
        rfmv_seq_note_put(m, SEQ_CLIP(t, 0), 0, 60 + t, 100, 24 * 200, 0);
    rfm_move_seq_play(m, 1);
    adv(m, 3 * FPS);
}
static void test_note_offs(void)
{
    rfm_move_t *m = &M;
    int t;
    fresh(m);
    play_long(m);
    CHECK(stuck() == 4);
    press(m, MR_CC_PLAY);                               /* 1 Play (stop) */
    CHECK(stuck() == 0);
    play_long(m);
    rfm_move_release_all(m);                            /* 2 the link drops */
    CHECK(stuck() == 0 && !m->seq.playing);
    play_long(m);
    for (t = 0; t < 4; t++)                             /* 3 mutes */
        rfmv_seq_mute(m, t, 1);
    CHECK(stuck() == 0);
    for (t = 0; t < 4; t++)
        rfmv_seq_mute(m, t, 0);
    rfm_move_seq_play(m, 0);
    play_long(m);
    rfm_move_project_load(m, 3, NULL, 0);               /* 4 a project change */
    CHECK(stuck() == 0 && !m->seq.playing && rfm_move_project_slot(m) == 3);
    fresh(m);
    play_long(m);
    track(m, 3, 1), jog(m, 1), track(m, 3, 0);          /* 5 the track's channel changes */
    CHECK(sounding[0][60] == 0);
    for (t = 0; t < 4; t++)                             /* 6 clip changes (stop at the bar) */
        rfmv_seq_launch(m, t, -1);
    adv(m, 16 * FPS);
    CHECK(stuck() == 0);
    /* 7 a mode change: the sequencer plays on in FM-1 mode, and its notes still end on time */
    rfm_move_seq_play(m, 0);
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 72, 100, 24 * 3, 0);
    rfm_move_seq_play(m, 1);
    adv(m, FPS);
    track(m, 0, 1), track(m, 0, 0);
    CHECK(m->s.mode == RFM_MOVE_MODE_FM1 && m->seq.playing && sounding[0][72] == 1);
    adv(m, 3 * FPS);
    CHECK(sounding[0][72] == 0);
    track(m, 3, 1), track(m, 3, 0);
    press(m, MR_CC_BACK);                               /* Back: FM-1 mode, still playing */
    CHECK(m->s.mode == RFM_MOVE_MODE_FM1 && m->seq.playing);
    rfm_move_release_all(m);
    CHECK(stuck() == 0);
    /* 8 the same note twice: off before the second on; 64 sounding at most */
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 50, 100, 24 * 10, 0);
    rfmv_seq_note_put(m, 0, 2, 50, 100, 24 * 10, 0);
    for (t = 0; t < 16; t++) {
        int n;
        for (n = 0; n < 8; n++)
            rfmv_seq_note_put(m, SEQ_CLIP(t, 0), 4, 20 + n * 10, 100, 24 * 100, 0);
    }
    rfm_move_seq_play(m, 1);
    adv(m, 3 * FPS);
    CHECK(sounding[0][50] == 1 && count_midi(0x80, 50) == 1);
    adv(m, 3 * FPS);
    CHECK(m->seq.nsnd == RFM_MOVE_SEQ_SOUNDING && stuck() == RFM_MOVE_SEQ_SOUNDING);   /* (the rest were cut short) */
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
}

/* ================================================================ recording, capture === */
static void test_record(void)
{
    rfm_move_t *m = &M;
    int i, lo, hi;
    fresh(m);
    press(m, MR_CC_REC);                                /* armed, the transport starts */
    CHECK(m->seq.rec && m->seq.start_pending == 1);
    adv(m, (3 * 24 + 5) * FPT + 1);                    /* tick 77: step 4 + 5 ticks */
    CHECK(rfmv_seq_now_tick(m) == 77);
    pad(m, 1, 90);                                      /* D3 */
    adv(m, 2 * FPS);
    pad(m, 1, 0);
    i = rfmv_seq_find(m, 0, 3, 50);
    CHECK(i >= 0 && m->seq.notes[i].micro == 5 && m->seq.notes[i].len == 48 && m->seq.notes[i].vel == 90);
    adv(m, (4 * 24 + 20 - (77 + 48)) * FPT + 16 * FPS - 0);   /* to tick 116 of the next bar */
    CHECK(rfmv_seq_now_tick(m) % RFM_MOVE_SEQ_BAR == 116);
    pad(m, 3, 100);                                     /* F3: step 6, 4 ticks early */
    adv(m, FPT * 3);
    pad(m, 3, 0);
    i = rfmv_seq_find(m, 0, 5, 53);
    CHECK(i >= 0 && m->seq.notes[i].micro == -4 && m->seq.notes[i].len == 3);
    /* the take is one undo */
    press(m, MR_CC_UNDO);
    CHECK(m->seq.nnotes == 0);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_UNDO), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.nnotes == 2);
    /* quantize the clip: Shift + Capture */
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_CAPTURE), button(m, MR_CC_SHIFT, 0);
    hi = rfmv_seq_clip_range(m, 0, &lo);
    for (i = lo; i < hi; i++)
        CHECK(m->seq.notes[i].micro == 0);
    /* a note held over Stop is kept, to the stop */
    pad(m, 5, 100);
    adv(m, FPS);
    press(m, MR_CC_PLAY);
    CHECK(!m->seq.playing && !m->seq.rec && nnotes_clip(m, 0) == 3);
    pad(m, 5, 0);
    CHECK(stuck() == 0);
    /* fixed velocity (Shift + step 2): steps and recordings at 127 */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 1), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.fixed_vel);
    pad(m, 7, 20), pad(m, 7, 0);
    tap_step(m, 12);
    CHECK(m->seq.notes[rfmv_seq_find(m, 0, 12, 60)].vel == 127);
    /* session: an empty clip with Rec armed records into it, growing a bar at a time until pressed again */
    fresh(m);
    press(m, MR_CC_MENU);
    press(m, MR_CC_REC);
    rfm_move_seq_play(m, 0);                            /* (Rec started it: stop, then arm again) */
    press(m, MR_CC_REC);
    pad(m, 17, 100), pad(m, 17, 0);                     /* track 2, scene 2: empty */
    CHECK(m->seq.tr[1].queued == 1 && m->seq.tr[1].grow == 2);
    adv(m, FPT);                                        /* T 0: it starts */
    CHECK(m->seq.tr[1].active == 1 && m->seq.tr[1].grow == 1);
    press(m, MR_CC_MENU);                               /* track view, track 2 (selected by the press) */
    CHECK(m->seq.sel_track == 1);
    adv(m, 20 * FPS);                                   /* bar 2 */
    CHECK(m->seq.len[SEQ_CLIP(1, 1)] == 32);
    pad(m, 0, 100);
    adv(m, FPS);
    pad(m, 0, 0);
    CHECK(rfmv_seq_find(m, SEQ_CLIP(1, 1), 20, 48) >= 0);
    press(m, MR_CC_MENU);
    pad(m, 17, 100), pad(m, 17, 0);                     /* done: at the next bar it is 2 bars long */
    adv(m, 8 * FPS);
    CHECK(m->seq.tr[1].grow == 1 && m->seq.tr[1].grow_stop);
    adv(m, 4 * FPS);
    CHECK(m->seq.tr[1].grow == 0 && m->seq.len[SEQ_CLIP(1, 1)] == 32 && m->seq.tr[1].active == 1);
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);

    /* Capture: stopped, the last pad notes become a clip (first note on step 1) */
    fresh(m);
    adv(m, 10 * FPS + 77);
    pad(m, 7, 100), adv(m, FPS), pad(m, 7, 0);          /* C4 */
    adv(m, FPS);
    pad(m, 8, 80), adv(m, FPS / 2), pad(m, 8, 0);       /* (pad 8 = F3) two steps after */
    press(m, MR_CC_CAPTURE);
    CHECK(m->seq.msg == SEQ_MSG_CAPTURED && m->seq.tr[0].active == 0 && m->seq.len[0] == 16);
    CHECK(rfmv_seq_find(m, 0, 0, 60) >= 0 && rfmv_seq_find(m, 0, 2, 53) >= 0 && nnotes_clip(m, 0) == 2);
    CHECK(m->seq.notes[rfmv_seq_find(m, 0, 2, 53)].len == 12);
    press(m, MR_CC_CAPTURE);                            /* nothing new: nothing */
    CHECK(m->seq.msg == SEQ_MSG_NOCLIP && nnotes_clip(m, 1) == 0);
    /* playing: aligned to the song's bars, into the next empty clip, in phase */
    rfm_move_seq_play(m, 1);
    adv(m, 16 * FPS + 4 * FPS);                         /* bar 2, step 5 */
    pad(m, 2, 100), adv(m, FPS), pad(m, 2, 0);
    adv(m, 15 * FPS);                                   /* .. and bar 3, step 5 */
    pad(m, 4, 100), adv(m, FPS), pad(m, 4, 0);
    press(m, MR_CC_CAPTURE);
    CHECK(m->seq.tr[0].active == 1 && m->seq.len[SEQ_CLIP(0, 1)] == 32);
    CHECK(rfmv_seq_find(m, SEQ_CLIP(0, 1), 4, 52) >= 0 && rfmv_seq_find(m, SEQ_CLIP(0, 1), 20, 55) >= 0);
    CHECK(rfmv_seq_clip_pos(m, 0) == (int)((m->seq.T - 16 * 24) % (32 * 24)) / 24);
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
}

/* ================================================================ edits: copy, cut, paste, undo, locks === */
static void test_edits(void)
{
    rfm_move_t *m = &M;
    uint64_t at;
    int lo, hi;
    fresh(m);
    pad(m, 0, 100), pad(m, 0, 0);
    tap_step(m, 0);
    pad(m, 2, 90), pad(m, 2, 0);
    tap_step(m, 0);
    CHECK(nnotes_clip(m, 0) == 2);
    /* Copy + step 1, then step 9: pasted */
    button(m, MR_CC_COPY, 1), tap_step(m, 0), tap_step(m, 8), tap_step(m, 12), button(m, MR_CC_COPY, 0);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(hi - lo == 2 && rfmv_seq_range(m, 0, 12, &lo) - lo == 2 && nnotes_clip(m, 0) == 6);
    /* Delete + step 13 = cut; Copy + step 15 = its paste */
    button(m, MR_CC_DELETE, 1), tap_step(m, 12), button(m, MR_CC_DELETE, 0);
    CHECK(nnotes_clip(m, 0) == 4 && m->seq.cut);
    button(m, MR_CC_COPY, 1), tap_step(m, 14), button(m, MR_CC_COPY, 0);
    CHECK(rfmv_seq_range(m, 0, 14, &lo) - lo == 2 && nnotes_clip(m, 0) == 6 && !m->seq.cut);
    /* undo: the paste, the cut, the second paste; redo the lot */
    press(m, MR_CC_UNDO);
    CHECK(nnotes_clip(m, 0) == 4 && rfmv_seq_range(m, 0, 14, &lo) == lo);
    press(m, MR_CC_UNDO);
    CHECK(nnotes_clip(m, 0) == 6 && rfmv_seq_range(m, 0, 12, &lo) - lo == 2);
    press(m, MR_CC_UNDO), press(m, MR_CC_UNDO);
    CHECK(nnotes_clip(m, 0) == 2);
    button(m, MR_CC_SHIFT, 1);
    press(m, MR_CC_UNDO), press(m, MR_CC_UNDO), press(m, MR_CC_UNDO), press(m, MR_CC_UNDO), press(m, MR_CC_UNDO);
    button(m, MR_CC_SHIFT, 0);
    CHECK(nnotes_clip(m, 0) == 6 && rfmv_seq_range(m, 0, 14, &lo) - lo == 2);
    /* a new edit after an undo: no redo */
    press(m, MR_CC_UNDO);
    tap_step(m, 3);
    CHECK(m->seq.un == m->seq.utop);
    /* Delete + pad: that note out of the whole clip */
    button(m, MR_CC_DELETE, 1), pad(m, 0, 100), pad(m, 0, 0), button(m, MR_CC_DELETE, 0);
    lo = 0;
    for (hi = 0; hi < m->seq.nnotes; hi++)
        lo += m->seq.notes[hi].note == 48;
    CHECK(lo == 0);
    /* CC locks: hold step 9, turn knob 1 (CC 16): a lock from the knob's value; the ring shows it */
    step(m, 8, 1);
    knob(m, 0, 5);
    CHECK(rfmv_seq_lock_get(m, 0, 8, 16) == 69);
    knob(m, 0, -2);
    CHECK(rfmv_seq_lock_get(m, 0, 8, 16) == 67);
    {
        rfm_move_led_t out[RFM_MOVE_NLEDS];
        int n, i, ok = 0;
        rfm_move_leds_invalidate(m);
        n = rfm_move_leds(m, out, RFM_MOVE_NLEDS);
        for (i = 0; i < n; i++)
            if (out[i].cc && out[i].number == MR_CC_KNOB1)
                ok = out[i].r || out[i].g || out[i].b;
        CHECK(ok);
    }
    step(m, 8, 0);
    CHECK(rfmv_seq_range(m, 0, 8, &lo) - lo == 1);      /* (a lock is an edit: the step kept its notes) */
    m->s.bank = 1;                                      /* bank 2: knob 3 = CC 26 */
    step(m, 2, 1), knob(m, 2, 1), step(m, 2, 0);
    CHECK(rfmv_seq_lock_get(m, 0, 2, 26) == 65);
    clear();
    rfm_move_seq_play(m, 1);
    adv(m, 16 * FPS);
    CHECK(find_midi(0xB0, 16, 67, &at) && at == 8 * FPS && find_midi(0xB0, 26, 65, &at) && at == 2 * FPS);
    rfm_move_seq_play(m, 0);
    step(m, 2, 1), button(m, MR_CC_DELETE, 1), knob(m, 2, 1), button(m, MR_CC_DELETE, 0), step(m, 2, 0);
    CHECK(rfmv_seq_lock_get(m, 0, 2, 26) == -1);
    press(m, MR_CC_UNDO);
    CHECK(rfmv_seq_lock_get(m, 0, 2, 26) == 65);
    /* knobs without a held step: live CCs on the track's channel */
    clear();
    knob(m, 1, 2);
    CHECK(find_midi(0xB0, 25, 66, NULL));
    /* session: Copy + clip, clip = a clip copy; Delete + clip = cut, Copy + clip = paste */
    press(m, MR_CC_MENU);
    button(m, MR_CC_COPY, 1), pad(m, 24, 100), pad(m, 24, 0), pad(m, 26, 100), pad(m, 26, 0), button(m, MR_CC_COPY, 0);
    CHECK(nnotes_clip(m, SEQ_CLIP(2, 0)) == nnotes_clip(m, 0) && rfmv_seq_lock_get(m, SEQ_CLIP(2, 0), 8, 16) == 67);
    button(m, MR_CC_DELETE, 1), pad(m, 26, 100), pad(m, 26, 0), button(m, MR_CC_DELETE, 0);
    CHECK(!rfmv_seq_clip_used(m, SEQ_CLIP(2, 0)));
    button(m, MR_CC_COPY, 1), pad(m, 19, 100), pad(m, 19, 0), button(m, MR_CC_COPY, 0);   /* track 4, scene 2 */
    CHECK(nnotes_clip(m, SEQ_CLIP(3, 1)) == nnotes_clip(m, 0));
    /* Track 4 held: Copy + steps = a track copy; Delete + step = the track cleared */
    track(m, 3, 1);
    button(m, MR_CC_COPY, 1), tap_step(m, 3), tap_step(m, 9), button(m, MR_CC_COPY, 0);
    CHECK(nnotes_clip(m, SEQ_CLIP(9, 1)) == nnotes_clip(m, 0) && rfmv_seq_track_used(m, 9));
    button(m, MR_CC_DELETE, 1), tap_step(m, 9), button(m, MR_CC_DELETE, 0);
    track(m, 3, 0);
    CHECK(!rfmv_seq_track_used(m, 9));
    press(m, MR_CC_UNDO);
    CHECK(rfmv_seq_track_used(m, 9));
    /* the journal keeps going past its size (oldest groups dropped), and a group bigger than it is not undoable */
    {
        int i;
        for (i = 0; i < 3000; i++) {
            rfmv_seq_group(m);
            rfmv_seq_note_put(m, SEQ_CLIP(5, 0), i % 16, 30 + (i / 16) % 8, 1 + i % 127, 24, 0);
        }
        CHECK(m->seq.un >= RFM_MOVE_SEQ_UNDO - 2);
        rfmv_seq_group(m);
        for (i = 0; i < 3000; i++)
            rfmv_seq_lock_set(m, SEQ_CLIP(6, i / 256 % 2), i % 256, 1 + i / 512, i % 100);
        CHECK(m->seq.un <= RFM_MOVE_SEQ_UNDO);
        while (rfmv_seq_undo(m)) {}
        CHECK(m->seq.un == 0);
    }
}

/* ================================================================ scale and root on the Move === */
static void test_scale_root(void)
{
    rfm_move_t *m = &M;
    int i;
    fresh(m);
    track(m, 1, 1);                                     /* Track 2 held (MIDI mode) */
    jog(m, 2);                                          /* root D */
    pad(m, 17, 100), pad(m, 17, 0);                     /* row 2, column 2: minor */
    CHECK(m->s.root == 2 && m->s.scale == RFM_MOVE_SCALE_MINOR && count_midi(0x90, -1) == 0);
    jog(m, -3);
    CHECK(m->s.root == 11);
    jog(m, 1);
    pad(m, 22, 100), pad(m, 22, 0);                     /* column 7: harmonic minor */
    CHECK(m->s.root == 0 && m->s.scale == RFM_MOVE_SCALE_HARM_MINOR);
    pad(m, 25, 100), pad(m, 25, 0);                     /* row 1 still picks the layout */
    CHECK(m->s.layout == RFM_MOVE_LAYOUT_CHROMATIC);
    pad(m, 24, 100), pad(m, 24, 0);
    track(m, 1, 0);
    CHECK(rfm_move_pad_note(&m->s, 0) == 48 && rfm_move_pad_note(&m->s, 2) == 51 && rfm_move_pad_note(&m->s, 6) == 59);
    for (i = 0; i < RFM_MOVE_NSCALES; i++)
        CHECK(strcmp(rfm_move_scale_name(i), "?") != 0);
    CHECK(!strcmp(rfm_move_scale_name(RFM_MOVE_SCALE_BLUES), "BLUES"));
    /* the Sequencer's pads use them (the MIDI mode's settings, shared) */
    track(m, 3, 1), track(m, 3, 0);
    clear();
    pad(m, 2, 100), pad(m, 2, 0);
    CHECK(find_midi(0x90, 51, 100, NULL));
    /* the settings image keeps scales 6 and 7 */
    {
        uint8_t img[RFM_MOVE_SETTINGS_BYTES];
        rfm_move_settings_t s;
        m->s.scale = RFM_MOVE_SCALE_BLUES;
        rfm_move_save(&m->s, img);
        CHECK(rfm_move_load(&s, img, sizeof img) == 0 && s.scale == RFM_MOVE_SCALE_BLUES);
    }
}

/* ================================================================ projects === */
static void rich_project(rfm_move_t *m, uint32_t seed)
{
    int i;
    for (i = 0; i < 600; i++) {
        seed = seed * 1103515245u + 12345u;
        rfmv_seq_note_put(m, (int)(seed >> 8) % 256, (int)(seed >> 3) % 256, (int)(seed >> 16) % 128,
                          1 + (int)(seed >> 20) % 127, 1 + (int)(seed >> 9) % 400, (int)(seed >> 24) % 24 - 12);
        if (i % 3 == 0)
            rfmv_seq_lock_set(m, (int)(seed >> 11) % 256, (int)(seed >> 5) % 256, (int)(seed >> 17) % 128, (int)(seed >> 1) % 128);
    }
    for (i = 0; i < 16; i++) {
        m->seq.tr[i].ch = (uint8_t)(15 - i), m->seq.tr[i].mute = (uint8_t)(i & 1);
        m->seq.tr[i].active = (int8_t)(i % 3 == 0 ? -1 : i), m->seq.tr[i].sel = (int8_t)(i / 2);
        m->seq.len[i * 16 + 3] = (uint16_t)(i * 16 + 7);
    }
    m->seq.bpm10 = 1375, m->seq.clock_mode = RFM_MOVE_CLOCK_EXTERNAL, m->seq.fixed_vel = 1, m->seq.sel_track = 9;
}
static int same_project(const rfm_move_seq_t *a, const rfm_move_seq_t *b)
{
    int i;
    if (a->bpm10 != b->bpm10 || a->clock_mode != b->clock_mode || a->fixed_vel != b->fixed_vel ||
        a->sel_track != b->sel_track || a->nnotes != b->nnotes || a->nlocks != b->nlocks ||
        memcmp(a->len, b->len, sizeof a->len) || memcmp(a->notes, b->notes, a->nnotes * sizeof a->notes[0]) ||
        memcmp(a->locks, b->locks, a->nlocks * sizeof a->locks[0]))
        return 0;
    for (i = 0; i < 16; i++)
        if (a->tr[i].ch != b->tr[i].ch || a->tr[i].mute != b->tr[i].mute || a->tr[i].active != b->tr[i].active ||
            a->tr[i].sel != b->tr[i].sel)
            return 0;
    return 1;
}
/* the invariants a loaded project keeps */
static int sane(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    if (q->nnotes > RFM_MOVE_SEQ_NOTES || q->nlocks > RFM_MOVE_SEQ_LOCKS || q->bpm10 < RFM_MOVE_SEQ_BPM10_MIN ||
        q->bpm10 > RFM_MOVE_SEQ_BPM10_MAX || q->sel_track > 15)
        return 0;
    for (i = 0; i < q->nnotes; i++) {
        const rfm_move_note_t *n = &q->notes[i];
        if (n->note > 127 || !n->vel || n->vel > 127 || n->micro < -12 || n->micro > 11 || !n->len ||
            n->len > 256 * 24)
            return 0;
        if (i && ((uint32_t)n[-1].clip << 16 | n[-1].step << 8 | n[-1].note) >= ((uint32_t)n->clip << 16 | n->step << 8 | n->note))
            return 0;
    }
    for (i = 0; i < 256; i++)
        if (q->len[i] < 1 || q->len[i] > 256)
            return 0;
    for (i = 0; i < 16; i++)
        if (q->tr[i].ch > 15 || q->tr[i].active < -1 || q->tr[i].active > 15 || q->tr[i].sel < 0 || q->tr[i].sel > 15)
            return 0;
    return 1;
}
static uint32_t fnv32(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static void refnv(uint8_t *b, uint32_t n)
{
    const uint32_t h = fnv32(b, n - 4);
    b[n - 4] = (uint8_t)h, b[n - 3] = (uint8_t)(h >> 8), b[n - 2] = (uint8_t)(h >> 16), b[n - 1] = (uint8_t)(h >> 24);
}
static uint32_t rng = 2463534242u;
static uint32_t xs(void) { rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5; return rng; }

static void test_blob(void)
{
    static uint8_t a[RFM_MOVE_PROJECT_MAX], b[RFM_MOVE_PROJECT_MAX], junk[RFM_MOVE_PROJECT_MAX + 64];
    rfm_move_t *m = &M, *m2 = &M2;
    int n, n2, i, ok = 0, bad = 0, iter;
    fresh(m);
    CHECK(rfm_move_project_save(m, a, sizeof a) == RFM_MOVE_PROJECT_FIXED);   /* an empty project: 608 bytes */
    rich_project(m, 7);
    n = rfm_move_project_save(m, a, sizeof a);
    CHECK(n == RFM_MOVE_PROJECT_FIXED + m->seq.nnotes * 7 + m->seq.nlocks * 4 && n < 8000);
    printf("blob: empty %d bytes, %d notes + %d locks %d bytes, worst case %d\n", RFM_MOVE_PROJECT_FIXED,
           m->seq.nnotes, m->seq.nlocks, n, RFM_MOVE_PROJECT_MAX);
    CHECK(rfm_move_project_save(m, a, (uint32_t)n - 1) == -1);
    CHECK(memcmp(a, "RMSQ", 4) == 0 && a[4] == RFM_MOVE_PROJECT_VERSION);
    CHECK(rfm_move_project_check(a, (uint32_t)n) == 0);
    rfm_move_init(m2, NULL, on_act, NULL);
    CHECK(rfm_move_project_load(m2, 4, a, (uint32_t)n) == 0 && same_project(&m->seq, &m2->seq) && sane(m2));
    CHECK(rfm_move_project_slot(m2) == 4 && (m2->seq.proj_used >> 4 & 1));
    n2 = rfm_move_project_save(m2, b, sizeof b);
    CHECK(n2 == n && !memcmp(a, b, (size_t)n));
    /* the worst case fits RFM_MOVE_PROJECT_MAX and stays under 64 KB */
    CHECK(RFM_MOVE_PROJECT_MAX < 65536);
    /* invalid blobs: nothing changes */
    CHECK(rfm_move_project_load(m2, 5, a, (uint32_t)n - 1) < 0 && same_project(&m->seq, &m2->seq) &&
          rfm_move_project_slot(m2) == 4);
    memcpy(b, a, (size_t)n), b[4] = 2;
    CHECK(rfm_move_project_check(b, (uint32_t)n) == -2);
    memcpy(b, a, (size_t)n), b[100] ^= 1;
    CHECK(rfm_move_project_check(b, (uint32_t)n) == -5);
    memcpy(b, a, (size_t)n), b[OFF_TEST_NOTE0 + 3] = 0, refnv(b, (uint32_t)n);   /* velocity 0 */
    CHECK(rfm_move_project_check(b, (uint32_t)n) == -7);
    memcpy(b, a, (size_t)n), b[24] = 16, refnv(b, (uint32_t)n);                 /* channel 17 */
    CHECK(rfm_move_project_check(b, (uint32_t)n) == -6);
    memcpy(b, a, (size_t)n), b[OFF_TEST_NOTE0 + 7] = b[OFF_TEST_NOTE0], b[OFF_TEST_NOTE0 + 8] = b[OFF_TEST_NOTE0 + 1],
        b[OFF_TEST_NOTE0 + 9] = b[OFF_TEST_NOTE0 + 2], refnv(b, (uint32_t)n);   /* a duplicate key */
    CHECK(rfm_move_project_check(b, (uint32_t)n) == -7);
    CHECK(rfm_move_project_check(NULL, 100) < 0 && rfm_move_project_check(a, 0) < 0);
    /* truncated at every length: never valid, never read past n (ASan watches a copy of exactly n bytes) */
    for (i = 0; i < n; i++) {
        uint8_t *t = malloc((size_t)i + 1);
        memcpy(t, a, (size_t)i);
        bad += rfm_move_project_check(t, (uint32_t)i) == 0;
        bad += rfm_move_project_load(m2, 1, t, (uint32_t)i) == 0 && i > 0;
        free(t);
    }
    CHECK(bad == 0);
    /* random bytes, random lengths */
    for (iter = 0; iter < 3000; iter++) {
        const uint32_t len = xs() % (sizeof junk);
        uint8_t *t = malloc(len ? len : 1);
        for (i = 0; i < (int)len; i++)
            t[i] = (uint8_t)xs();
        if (len >= 5 && (iter & 1))
            memcpy(t, "RMSQ\001", 5);
        if (rfm_move_project_load(m2, 2, t, len) == 0)
            ok++;
        free(t);
    }
    CHECK(sane(m2));
    /* mutations of a real blob with the check fixed up: whatever loads is sane and saves back valid */
    for (iter = 0; iter < 20000; iter++) {
        const int k = 1 + (int)(xs() % 4);
        uint32_t len = (uint32_t)n;
        uint8_t *t;
        memcpy(b, a, (size_t)n);
        for (i = 0; i < k; i++) {
            const uint32_t pos = xs() % (uint32_t)n;
            switch (xs() % 4) {
            case 0: b[pos] = (uint8_t)xs(); break;
            case 1: b[pos] ^= (uint8_t)(1u << (xs() % 8)); break;
            case 2: b[pos] = (uint8_t)(b[pos] + 1); break;
            default: len = 600 + xs() % ((uint32_t)n - 600); break;   /* (counts disagree with the length) */
            }
        }
        if (len >= 8)
            refnv(b, len);
        t = malloc(len);
        memcpy(t, b, len);
        if (rfm_move_project_load(m2, 3, t, len) == 0) {
            const int sn = rfm_move_project_save(m2, junk, sizeof junk);
            ok++;
            if (!sane(m2) || sn < 0 || rfm_move_project_check(junk, (uint32_t)sn) != 0)
                bad++;
        }
        free(t);
    }
    printf("fuzz: %d loads accepted, %d bad\n", ok, bad);
    CHECK(bad == 0 && ok > 0);
}

static void test_project_grid(void)
{
    rfm_move_t *m = &M;
    int i;
    fresh(m);
    memset(store_n, 0, sizeof store_n);
    answer_m = m, answer_sync = 1;
    CHECK(rfm_move_project_slot(m) == 0 && m->seq.proj_used == 0);
    /* Shift + step 1: the grid; the current one again: out */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.projects == 1);
    pad(m, 24, 100), pad(m, 24, 0);                     /* top left = project 1 = current */
    CHECK(m->seq.projects == 0 && nsave == 0);
    /* some notes; then project 2 (empty): project 1 saved, project 2 new and saved */
    pad(m, 0, 100), pad(m, 0, 0);
    tap_step(m, 0), tap_step(m, 6);
    CHECK(m->seq.dirty);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
    pad(m, 25, 100), pad(m, 25, 0);
    CHECK(nsave == 2 && store_n[0] > 0 && store_n[1] == RFM_MOVE_PROJECT_FIXED && rfm_move_project_slot(m) == 1);
    CHECK(m->seq.nnotes == 0 && m->seq.projects == 0 && m->seq.proj_used == 3 && !m->seq.dirty);
    /* project 1 again: saved, then a LOAD the host answers at once (inside the callback) */
    rfmv_seq_note_put(m, 0, 3, 70, 100, 24, 0);
    nsave = 0;
    button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
    pad(m, 24, 100), pad(m, 24, 0);
    CHECK(nsave == 1 && nload == 1 && last_slot == 0 && rfm_move_project_slot(m) == 0);
    CHECK(m->seq.nnotes == 2 && rfmv_seq_find(m, 0, 6, 48) >= 0 && m->seq.projects == 0);
    /* a later answer (async): the pad blinks meanwhile, other pads wait */
    answer_sync = 0;
    button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
    pad(m, 25, 100), pad(m, 25, 0);
    CHECK(m->seq.proj_pending == 1 && rfm_move_project_slot(m) == 0 && nload == 2);
    pad(m, 26, 100), pad(m, 26, 0);
    CHECK(m->seq.proj_pending == 1 && nload == 2);
    CHECK(rfm_move_project_load(m, 1, store[1], store_n[1]) == 0 && rfm_move_project_slot(m) == 1);
    CHECK(rfmv_seq_find(m, 0, 3, 70) >= 0);
    /* a load that fails: nothing changes, the message says so */
    {
        static uint8_t bogus[700];
        memset(bogus, 0xAB, sizeof bogus);
        CHECK(rfm_move_project_load(m, 5, bogus, sizeof bogus) < 0 && rfm_move_project_slot(m) == 1 &&
              m->seq.msg == SEQ_MSG_LOADFAIL && rfmv_seq_find(m, 0, 3, 70) >= 0);
    }
    /* Delete + pad: project 1 is deleted; Delete + the current one: it empties */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
    button(m, MR_CC_DELETE, 1);
    pad(m, 24, 100), pad(m, 24, 0);
    CHECK(ndel == 1 && last_slot == 0 && !(m->seq.proj_used & 1));
    pad(m, 25, 100), pad(m, 25, 0);
    CHECK(ndel == 2 && m->seq.nnotes == 0 && m->seq.proj_used == 0 && !m->seq.dirty);
    button(m, MR_CC_DELETE, 0);
    press(m, MR_CC_BACK);                               /* Back leaves the grid, not the mode */
    CHECK(m->seq.projects == 0 && m->s.mode == RFM_MOVE_MODE_SEQ);
    /* the host's directory */
    rfm_move_project_dir(m, 0x80000001u);
    CHECK(m->seq.proj_used == 0x80000001u);
    /* autosave: 2 s after the last edit, once */
    nsave = 0;
    rfmv_seq_note_put(m, 0, 0, 60, 100, 24, 0);
    adv(m, RATE * 19 / 10);
    CHECK(nsave == 0);
    rfmv_seq_note_put(m, 0, 1, 60, 100, 24, 0);
    adv(m, RATE * 19 / 10);
    CHECK(nsave == 0);
    adv(m, RATE * 2 / 10);
    CHECK(nsave == 1 && last_slot == 1 && !m->seq.dirty);
    adv(m, RATE * 5);
    CHECK(nsave == 1);
    /* flush: now, if changed */
    rfm_move_project_flush(m);
    CHECK(nsave == 1);
    rfm_move_seq_set_bpm(m, 1000);
    rfm_move_project_flush(m);
    CHECK(nsave == 2);
    /* LEDs of the grid: current, used, empty */
    for (i = 0; i < 1; i++) {
        rfm_move_led_t out[RFM_MOVE_NLEDS];
        int n, j, cur = 0, used = 0;
        button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
        rfm_move_leds_invalidate(m);
        n = rfm_move_leds(m, out, RFM_MOVE_NLEDS);
        for (j = 0; j < n; j++) {
            if (!out[j].cc && out[j].number == MR_NOTE_PAD0 + 25)
                cur = out[j].r == 0xff && out[j].g == 0x2a;
            if (!out[j].cc && out[j].number == MR_NOTE_PAD0 + 7)   /* slot 32 (used, bottom right) */
                used = out[j].r == 71 && out[j].g == 71;
        }
        CHECK(cur && used);
        press(m, MR_CC_BACK);
    }
    answer_m = NULL;
}

/* ================================================================ LEDs, screens === */
static uint32_t fnv(const uint8_t *p, int n) { return fnv32(p, (uint32_t)n); }
static void write_pbm(const char *path, const uint8_t *packed)
{
    FILE *f = fopen(path, "w");
    int x, y;
    if (!f)
        return;
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(packed[(y >> 3) * 128 + x] >> (y & 7) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

#define GOLD_TRACK 0x478a5380u
#define GOLD_SESSION 0x82f0c587u
#define GOLD_PROJECTS 0x1304e3e5u

static void test_screens_leds(void)
{
    rfm_move_t *m = &M;
    uint8_t out[MR_SCREEN_BYTES];
    rfm_move_led_t leds[RFM_MOVE_NLEDS];
    uint32_t h;
    int n, i, steps_lit = 0;
    fresh(m);
    rfm_move_init(m, NULL, on_act, NULL);
    track(m, 3, 1), track(m, 3, 0);
    /* track view: a C-E-G chord on step 1, notes on 5 and 9, playing at bar 1 step 6, step 1 held */
    pad(m, 0, 100), pad(m, 0, 0);
    tap_step(m, 0), tap_step(m, 4), tap_step(m, 8);
    pad(m, 2, 100), pad(m, 2, 0);
    tap_step(m, 0);
    pad(m, 4, 100), pad(m, 4, 0);
    tap_step(m, 0);
    rfmv_seq_lock_set(m, 0, 0, 16, 99);
    rfm_move_seq_play(m, 1);
    adv(m, 5 * FPS + 10);
    step(m, 0, 1);
    rfm_move_screen(m, NULL, out);
    h = fnv(out, MR_SCREEN_BYTES);
    write_pbm("move_seq_track.pbm", out);
    printf("screen seq track: %08x\n", h);
    CHECKF(GOLD_TRACK == 0 || h == GOLD_TRACK, "track view %08x, golden %08x", h, GOLD_TRACK);
    /* the step LEDs: the held step white, the playhead, the notes */
    rfm_move_leds_invalidate(m);
    n = rfm_move_leds(m, leds, RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++)
        if (!leds[i].cc && leds[i].number >= MR_NOTE_STEP0 && leds[i].number < MR_NOTE_STEP0 + 16) {
            const int s = leds[i].number - MR_NOTE_STEP0;
            if (s == 0)
                CHECK(leds[i].r == 255 && leds[i].g == 255 && leds[i].b == 255);
            if (s == 5)
                CHECK(leds[i].r == 199 && leds[i].g == 199);   /* the playhead */
            if (s == 4 || s == 8)                       /* notes, not the current one (G3): track 1's colour, dim */
                CHECK(leds[i].r == 255 * 56 / 256 && leds[i].g == 42 * 56 / 256);
            steps_lit += leds[i].r || leds[i].g || leds[i].b;
        }
    CHECK(steps_lit == 16);
    step(m, 0, 0);
    /* session view: clips on tracks 1-3, one queued, one recording */
    rfmv_seq_note_put(m, SEQ_CLIP(1, 0), 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(1, 1), 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(2, 2), 0, 60, 100, 24, 0);
    rfmv_seq_launch(m, 2, 2);
    press(m, MR_CC_MENU);
    rfm_move_screen(m, NULL, out);
    h = fnv(out, MR_SCREEN_BYTES);
    write_pbm("move_seq_session.pbm", out);
    printf("screen seq session: %08x\n", h);
    CHECKF(GOLD_SESSION == 0 || h == GOLD_SESSION, "session view %08x, golden %08x", h, GOLD_SESSION);
    rfm_move_leds_invalidate(m);
    n = rfm_move_leds(m, leds, RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++) {
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 17)       /* track 2 scene 2: has content, dim */
            CHECK(leds[i].r == 255 * 48 / 256 && leds[i].g == 128 * 48 / 256);
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 24)       /* track 1 scene 1: playing */
            CHECK(leds[i].r > 100);
        if (leds[i].cc && leds[i].number == MR_CC_PLAY)
            CHECK(leds[i].g == 255 && leds[i].r == 0);
        if (leds[i].cc && leds[i].number == MR_CC_MENU)
            CHECK(!leds[i].rgb && leds[i].r == 127);
    }
    /* the project grid */
    rfm_move_seq_play(m, 0);
    rfm_move_project_dir(m, 0x0000f00du);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
    rfm_move_screen(m, NULL, out);
    h = fnv(out, MR_SCREEN_BYTES);
    write_pbm("move_seq_projects.pbm", out);
    printf("screen seq projects: %08x\n", h);
    CHECKF(GOLD_PROJECTS == 0 || h == GOLD_PROJECTS, "project grid %08x, golden %08x", h, GOLD_PROJECTS);
    press(m, MR_CC_BACK);
    /* Track 4 held: the track menu; other modes while playing: the badge */
    track(m, 3, 1);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_tracks.pbm", out);
    track(m, 3, 0);
    rfm_move_seq_play(m, 1);
    adv(m, FPS);
    track(m, 1, 1), track(m, 1, 0);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_badge.pbm", out);
    CHECK(((out[0 * 128 + 56] >> 3) & 1) == 0 && ((out[0 * 128 + 60] >> 3) & 1) == 1);   /* a dark triangle */
    CHECK(GOLD_TRACK && GOLD_SESSION && GOLD_PROJECTS);
    rfm_move_release_all(m);
}

int main(void)
{
    test_model();
    test_playback();
    test_external();
    test_launch();
    test_note_offs();
    test_record();
    test_edits();
    test_scale_root();
    test_blob();
    test_project_grid();
    test_screens_leds();
    return check_done("move_seq");
}
