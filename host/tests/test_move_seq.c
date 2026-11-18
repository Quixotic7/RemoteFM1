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
    int lo, hi, i, mst;
    fresh(m);
    CHECK(m->s.mode == RFM_MOVE_MODE_SEQ && m->seq.view == 0 && m->seq.sel_track == 0);
    for (i = 0; i < 16; i++)
        CHECK(m->seq.tr[i].ch == i && m->seq.tr[i].active == 0 && m->seq.tr[i].sel == 0);
    CHECK(SEQ_CLIP(3, 5) == 3 * 16 + 5 && m->seq.len[SEQ_CLIP(15, 15)] == 16);
    /* nothing played yet: an empty step gets the root (C3); the first note into an empty clip starts the
     * transport (Move); a tap on a step with notes clears it (Move) */
    tap_step(m, 2);
    CHECK(m->seq.nnotes == 1 && rfmv_seq_find(m, 0, 2, 48) >= 0 && m->seq.start_pending == 1);
    rfm_move_seq_play(m, 0);
    tap_step(m, 2);
    CHECK(m->seq.nnotes == 0 && !m->seq.start_pending);
    /* Move's pad, then step: a pad played (E3, pad 2 in C major) is what empty steps get */
    pad(m, 2, 90), pad(m, 2, 0);
    CHECK(m->seq.nlast == 1 && m->seq.last_note[0] == 52 && m->seq.last_vel[0] == 90);
    tap_step(m, 0);
    CHECK(m->seq.start_pending == 1);
    rfm_move_seq_play(m, 0);
    tap_step(m, 4), tap_step(m, 15);
    CHECK(m->seq.nnotes == 3 && rfmv_seq_find(m, 0, 0, 52) >= 0 && rfmv_seq_find(m, 0, 15, 52) >= 0);
    CHECK(m->seq.notes[0].vel == 90 && m->seq.notes[0].len == 24 && m->seq.notes[0].micro == 0);
    CHECK(!m->seq.start_pending);                       /* (the clip had notes: no start) */
    tap_step(m, 4);                                     /* notes: cleared */
    CHECK(m->seq.nnotes == 2 && rfmv_seq_find(m, 0, 4, 52) < 0);
    /* a chord held (C3 100, G3 70), then a step: exactly those notes (step 1's E3 replaced) */
    pad(m, 0, 100), pad(m, 4, 70);
    tap_step(m, 0);
    pad(m, 0, 0), pad(m, 4, 0);
    hi = rfmv_seq_range(m, 0, 0, &lo);
    CHECK(hi - lo == 2 && m->seq.notes[lo].note == 48 && m->seq.notes[lo].vel == 100 &&
          m->seq.notes[lo + 1].note == 55 && m->seq.notes[lo + 1].vel == 70);
    /* no pad held: an empty step gets the last step's notes, with their velocities */
    tap_step(m, 6);
    hi = rfmv_seq_range(m, 0, 6, &lo);
    CHECK(hi - lo == 2 && m->seq.notes[lo].note == 48 && m->seq.notes[lo + 1].vel == 70);
    tap_step(m, 6);
    CHECK(rfmv_seq_range(m, 0, 6, &lo) == lo);
    /* a chord held over two steps: both get it */
    pad(m, 1, 80), pad(m, 3, 81);                       /* D3 F3 */
    tap_step(m, 12), tap_step(m, 13);
    pad(m, 1, 0), pad(m, 3, 0);
    CHECK(rfmv_seq_find(m, 0, 12, 50) >= 0 && rfmv_seq_find(m, 0, 12, 53) >= 0 && rfmv_seq_find(m, 0, 13, 53) >= 0);
    CHECK(m->seq.nlast == 2 && m->seq.last_note[0] == 50 && m->seq.last_note[1] == 53);
    rfmv_seq_step_clear(m, 0, 12), rfmv_seq_step_clear(m, 0, 13);
    pad(m, 0, 100), pad(m, 0, 0);
    pad(m, 4, 70), pad(m, 4, 0);                        /* (one after the other: the last one, G3) */
    CHECK(m->seq.nlast == 1 && m->seq.last_note[0] == 55);
    /* hold an empty step: nothing goes in until pads are pressed (the owner: not the last notes); pads in and
     * out of it (not recorded, still sounding) */
    step(m, 8, 1);
    CHECK(rfmv_seq_range(m, 0, 8, &lo) == lo);
    pad(m, 0, 100), pad(m, 0, 0);                       /* C3 in */
    pad(m, 2, 100), pad(m, 2, 0);                       /* E3 in */
    pad(m, 4, 100), pad(m, 4, 0);                       /* G3 in */
    pad(m, 4, 100), pad(m, 4, 0);                       /* .. and out */
    step(m, 8, 0);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(hi - lo == 2 && m->seq.notes[lo].note == 48 && m->seq.notes[lo + 1].note == 52);
    CHECK(m->seq.nlast == 2 && m->seq.last_note[0] == 48 && m->seq.last_note[1] == 52);   /* (the edited step's) */
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
    /* hold + jog: the length by 2 ticks (Move: 10 % of a step); Shift + jog: whole steps */
    step(m, 8, 1), jog(m, -2);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(m->seq.notes[lo].len == 4 * 24 - 4);
    button(m, MR_CC_SHIFT, 1), jog(m, -1), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.notes[lo].len == 2 * 24);
    step(m, 8, 0);
    CHECK(rfmv_seq_find(m, 0, 8, 48) >= 0);             /* (edited while held: not cleared on release) */
    /* hold a step (Move): the volume knob = velocity, Left / Right = nudge (Shift: a tick), Up / Down = transpose
     * (Shift: an octave); none of them leaves the step's notes cleared on release */
    rfmv_seq_note_put(m, 0, 5, 60, 100, 24, 0), rfmv_seq_note_put(m, 0, 5, 64, 50, 24, 0);
    clear();
    mst = m->master;
    step(m, 5, 1);
    knob(m, 8, 5);
    hi = rfmv_seq_range(m, 0, 5, &lo);
    CHECK(m->seq.notes[lo].vel == 105 && m->seq.notes[lo + 1].vel == 55 && m->master == mst);   /* (not MASTER) */
    knob(m, 8, 40);
    CHECK(m->seq.notes[lo].vel == 127);
    press(m, MR_CC_RIGHT);
    CHECK(m->seq.notes[lo].micro == 2 && m->seq.page == 0);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_LEFT), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.notes[lo].micro == 1);
    for (i = 0; i < 10; i++)
        press(m, MR_CC_RIGHT);
    CHECK(m->seq.notes[lo].micro == 11);                /* (at most +11: the step's own) */
    press(m, MR_CC_UP);
    CHECK(rfmv_seq_find(m, 0, 5, 61) >= 0 && rfmv_seq_find(m, 0, 5, 65) >= 0 && rfmv_seq_find(m, 0, 5, 60) < 0);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_DOWN), button(m, MR_CC_SHIFT, 0);
    CHECK(rfmv_seq_find(m, 0, 5, 49) >= 0 && rfmv_seq_find(m, 0, 5, 53) >= 0 && m->s.octave == 0);
    CHECK(m->seq.notes[rfmv_seq_find(m, 0, 5, 49)].micro == 11 && m->seq.notes[rfmv_seq_find(m, 0, 5, 53)].vel == 95);
    step(m, 5, 0);
    CHECK(rfmv_seq_range(m, 0, 5, &lo) - lo == 2);
    rfmv_seq_step_clear(m, 0, 5);
    knob(m, 8, -2);
    CHECK(m->master == mst - 32);                         /* (no step held: MASTER again) */
    /* loops and pages (the owner's Loop): Loop + steps 1 and 4 held = pages 1..4; Left / Right page; Shift + Loop
     * doubles */
    button(m, MR_CC_LOOP, 1), step(m, 0, 1), step(m, 3, 1), step(m, 3, 0), step(m, 0, 0), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 64 && m->seq.lstart[0] == 0 && m->seq.page == 0);
    press(m, MR_CC_RIGHT), press(m, MR_CC_RIGHT);
    CHECK(m->seq.page == 2);
    pad(m, 0, 100), pad(m, 0, 0);
    tap_step(m, 1);                                     /* page 3, step 2 = step 34 */
    CHECK(rfmv_seq_find(m, 0, 33, 48) >= 0);
    press(m, MR_CC_RIGHT), press(m, MR_CC_RIGHT);
    CHECK(m->seq.page == 4 && m->seq.len[0] == 64);     /* (past the last page: a pending one, the clip unchanged) */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 14), button(m, MR_CC_SHIFT, 0);   /* Shift + step 15: double (Move) */
    CHECK(m->seq.len[0] == 128 && rfmv_seq_find(m, 0, 33 + 64, 48) >= 0 && rfmv_seq_find(m, 0, 8 + 64, 52) >= 0);
    button(m, MR_CC_LOOP, 1), step(m, 15, 1), step(m, 0, 1), step(m, 0, 0), step(m, 15, 0), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 256 && m->seq.lstart[0] == 0);   /* (either order) */
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_LOOP), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.len[0] == 256);                        /* (at most 16 pages) */
    button(m, MR_CC_LOOP, 1), tap_step(m, 2), button(m, MR_CC_LOOP, 0);   /* a tap: the page shown, the loop kept */
    CHECK(m->seq.len[0] == 256 && m->seq.page == 2);
    button(m, MR_CC_LOOP, 1), tap_step(m, 1), tap_step(m, 1), button(m, MR_CC_LOOP, 0);   /* a double tap: that page */
    CHECK(m->seq.len[0] == 16 && m->seq.lstart[0] == 16 && m->seq.page == 1);
    CHECK(rfmv_seq_find(m, 0, 33, 48) >= 0);            /* (outside the loop: kept, not played) */
    button(m, MR_CC_LOOP, 1), tap_step(m, 5), tap_step(m, 4), button(m, MR_CC_LOOP, 0);   /* (two pages: no double) */
    CHECK(m->seq.lstart[0] == 16 && m->seq.page == 4);
    /* Loop held + jog (Move): a step a detent; Shift: a page */
    button(m, MR_CC_LOOP, 1), button(m, MR_CC_SHIFT, 1), jog(m, 2), button(m, MR_CC_SHIFT, 0), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 48 && m->seq.lstart[0] == 16);
    button(m, MR_CC_LOOP, 1), jog(m, -3), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 45);
    press(m, MR_CC_UNDO);                               /* (one Loop hold, one undo) */
    CHECK(m->seq.len[0] == 48);
    button(m, MR_CC_LOOP, 1), button(m, MR_CC_SHIFT, 1), jog(m, -9), button(m, MR_CC_SHIFT, 0), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 16);
    press(m, MR_CC_UNDO), press(m, MR_CC_UNDO), press(m, MR_CC_UNDO);   /* (to before the double tap: pages 1..16) */
    CHECK(m->seq.lstart[0] == 0 && m->seq.len[0] == 256);
    button(m, MR_CC_LOOP, 1), tap_step(m, 0), tap_step(m, 0), button(m, MR_CC_LOOP, 0);
    CHECK(m->seq.len[0] == 16 && m->seq.lstart[0] == 0 && m->seq.page == 0);
    /* steps outside the loop can be edited (the owner: any page) */
    m->seq.len[0] = 8;
    clear();
    i = m->seq.nnotes;
    tap_step(m, 12);
    CHECK(m->seq.nnotes == i + 1);
    tap_step(m, 12);
    CHECK(m->seq.nnotes == i);
    m->seq.len[0] = 16;
    adv(m, 8 * FPS);                                    /* (held steps were heard: their previews end) */
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
    tap_step(m, 0);
    pad(m, 0, 100), pad(m, 2, 80);                      /* C3 and E3 (velocity 80) held: step 5 */
    tap_step(m, 4);
    pad(m, 0, 0), pad(m, 2, 0);
    m->seq.notes[rfmv_seq_find(m, 0, 4, 52)].micro = 6;   /* a quarter step late */
    clear();
    CHECK(rfm_move_seq_bpm10(m) == 1200 && !rfm_move_seq_following(m));
    CHECK(m->seq.start_pending == 1 && nact == 0);      /* (the first step started the transport, Move) */
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
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_SAMPLE), now_ms += 400, press(m, MR_CC_SAMPLE), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.bpm10 == 1200 && m->seq.msg == SEQ_MSG_EXTTAP && !m->seq.srec);
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
    /* Shift + step 2 (Move's Setup) cycles the clock source */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 1), button(m, MR_CC_SHIFT, 0);
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
    pad(m, 25, 100), pad(m, 25, 0);
    CHECK(m->seq.tr[0].queued == 1 && m->seq.tr[0].active == 0 && nact == 0);
    adv(m, 12 * FPS);
    CHECK(m->seq.tr[0].active == 1 && find_midi(0x90, 62, 100, &at) && at == 16 * FPS && !find_midi(0x90, 60, -1, NULL));
    /* press the playing clip: it plays on (Move); an empty slot in the track: a new (empty) clip there, selected
     * and launched at the next bar (the owner) */
    pad(m, 25, 100), pad(m, 25, 0);
    CHECK(m->seq.tr[0].queued == SEQ_NONE && m->seq.tr[0].active == 1);
    pad(m, 26, 100), pad(m, 26, 0);                       /* track 1, scene 3: empty */
    CHECK(m->seq.tr[0].queued == 2 && m->seq.tr[0].sel == 2 && rfmv_seq_clip_has(m, SEQ_CLIP(0, 2)));
    CHECK(!rfmv_seq_clip_used(m, SEQ_CLIP(0, 2)));
    button(m, MR_CC_DELETE, 1), pad(m, 26, 100), pad(m, 26, 0), button(m, MR_CC_DELETE, 0);   /* (the new clip cut: the
                                                         * track stops at the bar instead) */
    CHECK(!rfmv_seq_clip_has(m, SEQ_CLIP(0, 2)) && m->seq.tr[0].queued == SEQ_STOP);
    m->seq.clip_kind = 0, m->seq.cut = 0;
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
    /* stopped, a clip press makes it the track's clip at once and starts the transport (Move); Shift + clip only
     * selects; an empty slot, stopped: the track plays nothing */
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
    pad(m, 24, 100), pad(m, 24, 0);                     /* track 1, scene 1 */
    CHECK(m->seq.tr[0].active == 0 && m->seq.start_pending == 1);
    rfm_move_seq_play(m, 0);
    pad(m, 27, 100), pad(m, 27, 0);                       /* track 1, scene 4: empty: a new clip, no start */
    CHECK(m->seq.tr[0].active == 3 && m->seq.tr[0].sel == 3 && !m->seq.start_pending);
    CHECK(rfmv_seq_clip_has(m, SEQ_CLIP(0, 3)));
    button(m, MR_CC_SHIFT, 1), pad(m, 17, 100), pad(m, 17, 0), button(m, MR_CC_SHIFT, 0);   /* track 2 scene 2 */
    CHECK(m->seq.sel_track == 1 && m->seq.tr[1].sel == 1);
    /* Move's session: a row a track (the bank's 4, Track 1 on top), a column a scene; Down = the next bank (tracks
     * 5-8), Right = the scenes one on */
    press(m, MR_CC_DOWN), press(m, MR_CC_RIGHT);
    CHECK(m->seq.bank == 1 && m->seq.scene_off == 1 && m->seq.sel_track == 5);
    pad(m, 24, 100), pad(m, 24, 0);
    CHECK(m->seq.sel_track == 4 && m->seq.tr[4].sel == 1);
    for (i = 0; i < 20; i++)
        press(m, MR_CC_RIGHT), press(m, MR_CC_DOWN);
    CHECK(m->seq.scene_off == 8 && m->seq.bank == 3);
    for (i = 0; i < 20; i++)
        press(m, MR_CC_LEFT), press(m, MR_CC_UP);
    CHECK(m->seq.scene_off == 0 && m->seq.bank == 0);
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
    track(m, 0, 1), jog(m, 1), track(m, 0, 0);          /* 5 the track's channel changes (Track 1 held) */
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
    press(m, MR_CC_BACK);
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
    m->seq.rec_mode = RFM_MOVE_RM_RECORD;               /* (the record settings' Record: Move's own) */
    press(m, MR_CC_REC);                                /* armed, a bar's count-in (Move), then the transport */
    CHECK(m->seq.rec && m->seq.start_pending == 3);
    adv(m, 16 * FPS);
    CHECK(!m->seq.playing && m->seq.start_pending == 3 && m->seq.count_in == 1);
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
    /* quantize the clip: Shift + step 16 (Move) */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 15), button(m, MR_CC_SHIFT, 0);
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
    /* fixed velocity (Shift + step 10, Move's Full Velocity): steps and recordings at 127 */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 9), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.fixed_vel);
    pad(m, 7, 20), pad(m, 7, 0);
    tap_step(m, 12);
    CHECK(m->seq.notes[rfmv_seq_find(m, 0, 12, 60)].vel == 127);
    /* session: an empty clip with Rec armed records into it, growing a bar at a time until pressed again */
    fresh(m);
    m->seq.rec_mode = RFM_MOVE_RM_RECORD;
    press(m, MR_CC_MENU);
    press(m, MR_CC_REC);
    press(m, MR_CC_REC);                                /* (off in the count-in: no start) */
    CHECK(!m->seq.rec && !m->seq.start_pending);
    press(m, MR_CC_REC);
    pad(m, 17, 100), pad(m, 17, 0);                     /* track 2, scene 2: empty */
    CHECK(m->seq.tr[1].queued == 1 && m->seq.tr[1].grow == 2);
    adv(m, 16 * FPS + FPT);                             /* the count-in, then T 0: it starts */
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
    CHECK(m->seq.start_pending == 1);                   /* (Move: the transport starts with it) */
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
    /* Shift + Capture (Move): the capture forgotten */
    pad(m, 2, 100), adv(m, FPS), pad(m, 2, 0);
    CHECK(m->seq.ncap > 0);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_CAPTURE), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.ncap == 0 && m->seq.msg == SEQ_MSG_CAPCLEAR);
    press(m, MR_CC_CAPTURE);
    CHECK(m->seq.msg == SEQ_MSG_NOCLIP);
    /* Shift + Play (Move): every clip again from its start, at once */
    CHECK(m->seq.T > 24 * 16);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_PLAY), button(m, MR_CC_SHIFT, 0);
    CHECK(!m->seq.playing && m->seq.start_pending == 1 && m->seq.tr[0].active == 1 && stuck() == 0);
    adv(m, 5 * FPS + 1);
    CHECK(m->seq.playing && m->seq.T == 5 * 24 && rfmv_seq_clip_pos(m, 0) == 5);
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
    pad(m, 0, 100), pad(m, 2, 90);                      /* a chord, then step 1 */
    tap_step(m, 0);
    pad(m, 0, 0), pad(m, 2, 0);
    CHECK(nnotes_clip(m, 0) == 2);
    /* Copy + step 1, then step 9: pasted */
    button(m, MR_CC_COPY, 1), tap_step(m, 0), tap_step(m, 8), tap_step(m, 12), button(m, MR_CC_COPY, 0);
    hi = rfmv_seq_range(m, 0, 8, &lo);
    CHECK(hi - lo == 2 && rfmv_seq_range(m, 0, 12, &lo) - lo == 2 && nnotes_clip(m, 0) == 6);
    /* the owner's cut: Delete + step 13 (notes) = cut, an X on it; Delete + step 15 (empty) = its paste */
    button(m, MR_CC_DELETE, 1), tap_step(m, 12), button(m, MR_CC_DELETE, 0);
    CHECK(nnotes_clip(m, 0) == 4 && m->seq.cut && rfmv_seq_src_step(m, 0, 12) == 2 && m->seq.msg == SEQ_MSG_NONE);
    button(m, MR_CC_DELETE, 1), tap_step(m, 14), button(m, MR_CC_DELETE, 0);
    CHECK(rfmv_seq_range(m, 0, 14, &lo) - lo == 2 && nnotes_clip(m, 0) == 6 && !m->seq.cut);
    CHECK(rfmv_seq_src_step(m, 0, 12) == 0);
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
    /* session: Copy + clip, clip = a clip copy (the source dashed while Copy is held); Delete + clip = cut (an X),
     * Delete + an empty slot = its paste */
    press(m, MR_CC_MENU);
    button(m, MR_CC_COPY, 1), pad(m, 24, 100), pad(m, 24, 0);
    CHECK(rfmv_seq_src_clip(m, 0) == 1);
    pad(m, 8, 100), pad(m, 8, 0), button(m, MR_CC_COPY, 0);
    CHECK(nnotes_clip(m, SEQ_CLIP(2, 0)) == nnotes_clip(m, 0) && rfmv_seq_lock_get(m, SEQ_CLIP(2, 0), 8, 16) == 67);
    CHECK(rfmv_seq_src_clip(m, 0) == 0);
    button(m, MR_CC_DELETE, 1), pad(m, 8, 100), pad(m, 8, 0), button(m, MR_CC_DELETE, 0);
    CHECK(!rfmv_seq_clip_has(m, SEQ_CLIP(2, 0)) && rfmv_seq_src_clip(m, SEQ_CLIP(2, 0)) == 2);
    button(m, MR_CC_DELETE, 1), pad(m, 1, 100), pad(m, 1, 0), button(m, MR_CC_DELETE, 0);   /* track 4, scene 2 */
    CHECK(nnotes_clip(m, SEQ_CLIP(3, 1)) == nnotes_clip(m, 0) && !m->seq.cut);
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
    /* Move's copy: Copy + step, Copy released, then a step: pasted there (once) */
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 60, 100, 24, 0);
    button(m, MR_CC_COPY, 1), tap_step(m, 0), button(m, MR_CC_COPY, 0);
    CHECK(m->seq.paste == 1 && m->seq.clip_kind == 1 && m->seq.msg == SEQ_MSG_NONE && rfmv_seq_src_step(m, 0, 0) == 1);
    tap_step(m, 5);
    CHECK(rfmv_seq_find(m, 0, 5, 60) >= 0 && !m->seq.paste && m->seq.msg == SEQ_MSG_NONE);
    tap_step(m, 6);                                     /* (then step entry again: the root, C3) */
    CHECK(rfmv_seq_find(m, 0, 6, 60) < 0 && rfmv_seq_find(m, 0, 6, 48) >= 0);
    /* Copy alone with a copy waiting: dropped (Move: Copy again clears the clipboard) */
    button(m, MR_CC_COPY, 1), tap_step(m, 0), button(m, MR_CC_COPY, 0);
    press(m, MR_CC_COPY);
    CHECK(!m->seq.paste && m->seq.clip_kind == 0 && m->seq.msg == SEQ_MSG_NONE);
    tap_step(m, 7);
    CHECK(rfmv_seq_find(m, 0, 7, 60) < 0);
    /* Copy alone (Move): the clip duplicated into the track's next empty slot, selected and playing */
    press(m, MR_CC_COPY);
    CHECK(m->seq.msg == SEQ_MSG_DUP && nnotes_clip(m, SEQ_CLIP(0, 1)) == nnotes_clip(m, 0) && nnotes_clip(m, 0) == 4);
    CHECK(m->seq.tr[0].sel == 1 && m->seq.tr[0].active == 1 && m->seq.len[SEQ_CLIP(0, 1)] == m->seq.len[0]);
    press(m, MR_CC_UNDO);
    CHECK(nnotes_clip(m, SEQ_CLIP(0, 1)) == 0);
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_UNDO), button(m, MR_CC_SHIFT, 0);
    /* session: Copy + clip, released, then a clip: pasted */
    press(m, MR_CC_MENU);
    button(m, MR_CC_COPY, 1), pad(m, 24, 100), pad(m, 24, 0), button(m, MR_CC_COPY, 0);
    CHECK(m->seq.paste && m->seq.clip_kind == 2);
    pad(m, 8, 100), pad(m, 8, 0);
    CHECK(nnotes_clip(m, SEQ_CLIP(2, 0)) == 4 && !m->seq.paste && m->seq.tr[2].queued == SEQ_NONE);
    /* Delete alone (Move): the selected track's clip deleted (undoable) */
    press(m, MR_CC_MENU);
    CHECK(m->seq.sel_track == 0 && m->seq.tr[0].sel == 1);
    press(m, MR_CC_DELETE);
    CHECK(!rfmv_seq_clip_has(m, SEQ_CLIP(0, 1)) && m->seq.msg == SEQ_MSG_CLEARED);
    press(m, MR_CC_UNDO);
    CHECK(nnotes_clip(m, SEQ_CLIP(0, 1)) == 4);
    step(m, 3, 1), press(m, MR_CC_DELETE), step(m, 3, 0);   /* (with a step held: nothing) */
    CHECK(nnotes_clip(m, SEQ_CLIP(0, 1)) == 4 + 1);
    CHECK(stuck() == 0);
}

/* ================================================================ scale and root on the Move === */
static void test_scale_root(void)
{
    rfm_move_t *m = &M;
    int i;
    fresh(m);
    press(m, MR_CC_BACK);                               /* (FM-1 mode; the sequencer's Track buttons are its tracks) */
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
    memcpy(b, a, (size_t)n), b[4] = 3;
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

#define GOLD_TRACK 0x8085c9edu
#define GOLD_STEP 0xcbab8d53u
#define GOLD_SESSION 0x05524496u
#define GOLD_PROJECTS 0x5204a535u
#define GOLD_PROJECT 0x25ca21a2u
#define GOLD_TRACKS 0x32878560u
#define GOLD_COUNT 0xc387a502u
#define GOLD_MSG 0x1ad29fd4u
static int px_at(const uint8_t *packed, int x, int y) { return packed[(y >> 3) * 128 + x] >> (y & 7) & 1; }
static void gold(const char *name, const uint8_t *out, uint32_t want)
{
    char path[64];
    const uint32_t h = fnv(out, MR_SCREEN_BYTES);
    snprintf(path, sizeof path, "move_seq_%s.pbm", name);
    write_pbm(path, out);
    printf("screen seq %s: %08x\n", name, h);
    CHECKF(want == 0 || h == want, "%s %08x, golden %08x", name, h, want);
}

/* the pads as the Move shows them (8 x 4, top row first), for a look at the held step's colours */
static void write_ppm_pads(const char *path, int pads[32][3])
{
    FILE *f = fopen(path, "w");
    int x, y;
    if (!f)
        return;
    fprintf(f, "P3\n%d %d\n255\n", 8 * 24, 4 * 24);
    for (y = 0; y < 4 * 24; y++)
        for (x = 0; x < 8 * 24; x++) {
            const int p = (3 - y / 24) * 8 + x / 24, edge = x % 24 < 2 || y % 24 < 2;
            fprintf(f, "%d %d %d\n", edge ? 20 : pads[p][0], edge ? 20 : pads[p][1], edge ? 20 : pads[p][2]);
        }
    fclose(f);
}

static void test_screens_leds(void)
{
    rfm_move_t *m = &M;
    uint8_t out[MR_SCREEN_BYTES];
    rfm_move_led_t leds[RFM_MOVE_NLEDS];
    int n, i, steps_lit = 0;
    fresh(m);
    rfm_move_init(m, NULL, on_act, NULL);
    track(m, 3, 1), track(m, 3, 0);
    /* track view: a C-E-G chord on step 1, notes on 5 and 9, playing at bar 1 step 6, step 1 held */
    pad(m, 0, 100), pad(m, 0, 0);
    tap_step(m, 4), tap_step(m, 8);
    pad(m, 0, 100), pad(m, 2, 100), pad(m, 4, 100);
    tap_step(m, 0);
    pad(m, 0, 0), pad(m, 2, 0), pad(m, 4, 0);
    rfmv_seq_lock_set(m, 0, 0, 16, 99);
    rfm_move_seq_play(m, 1);
    adv(m, 5 * FPS + 10);
    m->pop = POP_NONE;                                  /* (the taps' popups dismissed: the view at rest) */
    rfm_move_screen(m, NULL, out);
    CHECK(px_at(out, 1, 0) && px_at(out, 30, 3) && px_at(out, 33, 0) && !px_at(out, 33, 1));   /* page 1, page 2 */
    CHECK(px_at(out, 0, 54) && px_at(out, 3, 60) && !px_at(out, 11, 60) && px_at(out, 43, 60)); /* steps 1, 2, 6 */
    CHECK(px_at(out, 40, 7) && px_at(out, 40, 50) && !px_at(out, 41, 7));    /* the playhead: step 6 (+ 10 frames) */
    CHECK(px_at(out, 0, 46) && px_at(out, 0, 34) && px_at(out, 32, 46));     /* C3 E3 G3 on step 1, C3 on step 5 */
    gold("track", out, GOLD_TRACK);
    step(m, 0, 1);
    rfm_move_screen(m, NULL, out);
    CHECK(!rfmv_pop_up(m, POP_STEP) && !px_at(out, 6, 6));   /* (a press alone: no popup) */
    adv(m, RATE * RFM_MOVE_SEQ_HOLD_MS / 1000 + 128);  /* held: its popup */
    rfm_move_screen(m, NULL, out);
    CHECK(m->pop == POP_STEP && m->pop_i == 0 && px_at(out, 6, 6) && px_at(out, 2, 2) && px_at(out, 0, 52));
    gold("step", out, GOLD_STEP);
    /* the step LEDs (Move's): the held step in the track's colour, the playhead green, notes white, empty steps
     * the track's colour dim */
    rfm_move_leds_invalidate(m);
    n = rfm_move_leds(m, leds, RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++)
        if (!leds[i].cc && leds[i].number >= MR_NOTE_STEP0 && leds[i].number < MR_NOTE_STEP0 + 16) {
            const int s = leds[i].number - MR_NOTE_STEP0;
            if (s == 0)
                CHECK(leds[i].r == 255 && leds[i].g == 42 && leds[i].b == 42);
            if (s == rfmv_seq_clip_pos(m, 0))
                CHECK(leds[i].r == 0 && leds[i].g == 255 && leds[i].b == 0);   /* the playhead */
            if (s == 4 || s == 8)
                CHECK(leds[i].r == 255 && leds[i].g == 255 && leds[i].b == 255);
            if (s == 1)
                CHECK(leds[i].r == 255 * 28 / 256 && leds[i].g == 42 * 28 / 256);
            steps_lit += leds[i].r || leds[i].g || leds[i].b;
        }
    CHECK(steps_lit == 16);
    /* the pads with step 1 held: C3 (the root), E3, G3 bright white; the rest their layout colours, dim (the
     * other roots dim red, in-scale notes dim white) */
    {
        int pads[32][3], k;
        memset(pads, 0, sizeof pads);
        for (i = 0; i < n; i++)
            if (!leds[i].cc && leds[i].number >= MR_NOTE_PAD0 && leds[i].number < MR_NOTE_PAD0 + 32)
                k = leds[i].number - MR_NOTE_PAD0, pads[k][0] = leds[i].r, pads[k][1] = leds[i].g, pads[k][2] = leds[i].b;
        CHECK(pads[0][0] == 255 && pads[0][1] == 255 && pads[0][2] == 255);   /* C3: the root, in the step */
        CHECK(pads[2][0] == 255 && pads[2][1] == 255 && pads[4][2] == 255);
        CHECK(pads[1][0] < 40 && pads[1][0] == pads[1][1]);                   /* D3: dim white */
        CHECK(pads[7][0] > pads[7][1] && pads[7][0] < 110);                    /* C4: a root not in the step, dim red */
        write_ppm_pads("move_seq_held_pads.ppm", pads);
    }
    step(m, 0, 0);
    /* session view: clips on tracks 1-3, one queued, one recording */
    rfmv_seq_note_put(m, SEQ_CLIP(1, 0), 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(1, 1), 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, SEQ_CLIP(2, 2), 0, 60, 100, 24, 0);
    rfmv_seq_launch(m, 2, 2);
    rfmv_seq_note_put(m, SEQ_CLIP(4, 5), 0, 60, 100, 24, 0);   /* track 5: muted, a clip in scene 6 */
    rfmv_seq_mute(m, 4, 1);
    m->seq.tr[5].grow = 2, rfmv_seq_launch(m, 5, 1);    /* track 6: to record into scene 2 at the bar */
    press(m, MR_CC_MENU);
    rfm_move_screen(m, NULL, out);
    CHECK(px_at(out, 0, 0) && px_at(out, 3, 1) && !px_at(out, 3, 5));        /* bank 1, track 1 selected (an arrow) */
    CHECK(px_at(out, 8, 19) && !px_at(out, 9, 19));                          /* track 5 muted: its row dotted */
    CHECK(px_at(out, 10, 1));                                                /* track 1 scene 1: playing, filled */
    CHECK(px_at(out, 15, 4) && !px_at(out, 17, 5));                          /* track 2 scene 2: content, outlined */
    CHECK(px_at(out, 11, 13) && !px_at(out, 10, 13));                        /* track 4 scene 1: empty, a dot */
    gold("session", out, GOLD_SESSION);
    rfm_move_leds_invalidate(m);
    n = rfm_move_leds(m, leds, RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++) {
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 17)       /* track 2 scene 2: has content */
            CHECK(leds[i].r == 255 * 110 / 256 && leds[i].g == 128 * 110 / 256);
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 24)       /* track 1 scene 1: playing, selected: white */
            CHECK(leds[i].r > 100 && leds[i].r == leds[i].g && leds[i].g == leds[i].b);
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 10)       /* track 3 scene 3: queued: green (blinking) */
            CHECK(leds[i].r == 0 && leds[i].g > 0);
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 16)       /* track 2 scene 1: playing: its colour */
            CHECK(leds[i].r > leds[i].b && leds[i].g > 0);
        if (leds[i].cc && leds[i].number == MR_CC_PLAY)
            CHECK(leds[i].g == 255 && leds[i].r == 0);
        if (leds[i].cc && leds[i].number == MR_CC_MENU)
            CHECK(!leds[i].rgb && leds[i].r == 127);
    }
    /* the project grid */
    rfm_move_seq_play(m, 0);
    {   /* projects 3 and 14 stored: this one (step 1's chord kept by its hold: 5 notes on track 1, 2 on track 2,
         * 5 clips) and a smaller one; their bars come from the blobs (rfm_move_project_info), the current one's live */
        static uint8_t blob[RFM_MOVE_PROJECT_MAX];
        int nb = rfm_move_project_save(m, blob, sizeof blob);
        CHECK(nb > 0 && rfm_move_project_info(m, 2, blob, (uint32_t)nb) == 0);
        CHECK(m->seq.info[2].ok && m->seq.info[2].notes[0] == 5 && m->seq.info[2].notes[1] == 2 && m->seq.info[2].clips == 5);
        CHECK(m->seq.info[2].bpm10 == 1200);
        rfmv_seq_clip_clear(m, SEQ_CLIP(4, 5));
        nb = rfm_move_project_save(m, blob, sizeof blob);
        blob[nb - 1] ^= 1;
        CHECK(rfm_move_project_info(m, 13, blob, (uint32_t)nb) < 0 && !m->seq.info[13].ok);   /* (a broken blob) */
        blob[nb - 1] ^= 1;
        CHECK(rfm_move_project_info(m, 13, blob, (uint32_t)nb) == 0);
        rfm_move_project_dir(m, 0x0000f00du);
        button(m, MR_CC_SHIFT, 1), tap_step(m, 0), button(m, MR_CC_SHIFT, 0);
        rfm_move_screen(m, NULL, out);
        CHECK(px_at(out, 1, 1) && px_at(out, 7, 7) && !px_at(out, 4, 10));      /* 1: current, filled, black bars */
        CHECK(px_at(out, 23, 7) && px_at(out, 24, 8) && !px_at(out, 25, 7));    /* 2: empty, a dot */
        CHECK(px_at(out, 33, 1) && !px_at(out, 34, 2) && px_at(out, 36, 11));    /* 3: saved, outlined, bars */
        CHECK(px_at(out, 49, 1) && !px_at(out, 52, 11));                        /* 4: saved, not known: no bars */
        gold("projects", out, GOLD_PROJECTS);
        /* project 3's pad held (pad 26: top row, third): its popup while it loads (the host has not answered) */
        pad(m, 26, 100);
        rfm_move_screen(m, NULL, out);
        CHECK(m->pop == POP_PROJ && m->pop_i == 2 && m->seq.proj_pending == 2 && px_at(out, 2, 2));
        gold("project", out, GOLD_PROJECT);
        CHECK(rfm_move_project_load(m, 2, blob, (uint32_t)nb) == 0);
        adv(m, RATE * 2);
        rfm_move_screen(m, NULL, out);
        CHECK(px_at(out, 2, 2));                        /* held: up */
        pad(m, 26, 0);
        adv(m, RATE * RFM_MOVE_POPUP_MS / 1000);
        rfm_move_screen(m, NULL, out);
        CHECK(!px_at(out, 2, 2) && !m->seq.projects);   /* gone a second after; the grid closed on the load */
    }
    /* Track 4 held: the track menu */
    track(m, 3, 1);
    tap_step(m, 2);
    rfm_move_screen(m, NULL, out);
    CHECK(m->menu_what == MENU_TRACK && m->seq.sel_track == 2 && px_at(out, 19, 59) && px_at(out, 19, 37));
    gold("tracks", out, GOLD_TRACKS);
    tap_step(m, 0);
    track(m, 3, 0);
    /* the count-in (Rec from stopped, Record mode): its beats at 4x */
    m->seq.rec_mode = RFM_MOVE_RM_RECORD;
    press(m, MR_CC_REC);
    rfm_move_screen(m, NULL, out);
    CHECK(m->seq.start_pending == 3 && px_at(out, 2, 2));
    gold("count", out, GOLD_COUNT);
    press(m, MR_CC_REC);
    /* a message (Shift + step 10: full velocity) at 2x, a second */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 9), button(m, MR_CC_SHIFT, 0);
    rfm_move_screen(m, NULL, out);
    CHECK(m->seq.msg == SEQ_MSG_FULLVEL_ON && m->seq.fixed_vel && px_at(out, 2, 2));
    gold("msg", out, GOLD_MSG);
    adv(m, RATE * RFM_MOVE_POPUP_MS / 1000);
    rfm_move_screen(m, NULL, out);
    CHECK(!px_at(out, 2, 2));
    /* other modes while it plays: a small triangle over the FM-1 drawing */
    rfm_move_seq_play(m, 1);
    adv(m, FPS);
    press(m, MR_CC_BACK);
    rfm_move_screen(m, NULL, out);
    CHECK(px_at(out, 3, 3) && px_at(out, 7, 7) && !px_at(out, 8, 7) && !px_at(out, 7, 3));
    CHECK(GOLD_TRACK && GOLD_STEP && GOLD_SESSION && GOLD_PROJECTS && GOLD_PROJECT && GOLD_TRACKS && GOLD_COUNT &&
          GOLD_MSG);
    rfm_move_release_all(m);
}

/* the popups' life: up while their control is held, RFM_MOVE_POPUP_MS after (on the sequencer's clock) */
static void test_popups(void)
{
    rfm_move_t *m = &M;
    uint8_t out[MR_SCREEN_BYTES];
    const uint32_t pop_frames = RATE * RFM_MOVE_POPUP_MS / 1000;
    fresh(m);
    rfm_move_seq_set_clock(m, RFM_MOVE_CLOCK_INTERNAL);
    /* a held step: no popup for a tap, its popup after RFM_MOVE_SEQ_HOLD_MS held, a second after; a held empty
     * step stays empty (the owner: no last notes), a held step with notes is kept (never deleted) */
    {
        int lo;
        step(m, 2, 1);
        rfm_move_screen(m, NULL, out);
        CHECK(!rfmv_pop_up(m, POP_STEP) && !px_at(out, 6, 6));
        adv(m, RATE * RFM_MOVE_SEQ_HOLD_MS / 1000 + 128);
        rfm_move_screen(m, NULL, out);
        CHECK(rfmv_pop_up(m, POP_STEP) && m->pop_i == 2 && px_at(out, 2, 30) && !px_at(out, 6, 6));   /* (no notes) */
        adv(m, 3 * RATE);
        CHECK(rfmv_pop_up(m, POP_STEP));
        step(m, 2, 0);
        CHECK(rfmv_seq_range(m, 0, 2, &lo) == lo);      /* (held, nothing pressed: still empty) */
        adv(m, pop_frames - 128);
        CHECK(rfmv_pop_up(m, POP_STEP));
        adv(m, 256);
        CHECK(!rfmv_pop_up(m, POP_STEP));
        rfm_move_screen(m, NULL, out);
        CHECK(!px_at(out, 2, 30));
        tap_step(m, 2);                                 /* a tap on an empty step: the last notes, no popup */
        CHECK(rfmv_seq_range(m, 0, 2, &lo) > lo && !rfmv_pop_up(m, POP_STEP));
        clear();
        step(m, 2, 1);                                  /* held: heard (a preview), and kept */
        CHECK(find_midi(0x90, 48, -1, NULL));
        adv(m, RATE);
        step(m, 2, 0);
        CHECK(rfmv_seq_range(m, 0, 2, &lo) > lo);
        adv(m, pop_frames);
        tap_step(m, 2);                                 /* a tap on a step with notes: cleared */
        CHECK(rfmv_seq_range(m, 0, 2, &lo) == lo && !rfmv_pop_up(m, POP_STEP));
        adv(m, pop_frames);
    }
    /* a held step and a later one (the notes reach to it): the popup stays the held one's (its length 3) */
    tap_step(m, 4);
    step(m, 4, 1), step(m, 6, 1);
    adv(m, RATE / 2);
    CHECK(m->pop == POP_STEP && m->pop_i == 4 && m->seq.notes[0].len == 3 * RFM_MOVE_SEQ_TPS);
    step(m, 6, 0);
    CHECK(rfmv_pop_up(m, POP_STEP) && m->pop_i == 4);
    step(m, 4, 0);
    adv(m, pop_frames);
    /* Loop held: the clip's length (the jog: a page a detent) */
    button(m, MR_CC_LOOP, 1);
    jog(m, 1);
    CHECK(rfmv_pop_up(m, POP_LOOP) && m->seq.len[0] == 17);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_pop_loop.pbm", out);
    adv(m, 2 * pop_frames);
    CHECK(rfmv_pop_up(m, POP_LOOP));
    button(m, MR_CC_LOOP, 0);
    adv(m, pop_frames);
    CHECK(!rfmv_pop_up(m, POP_LOOP));
    /* Shift + jog: the tempo; a knob: its CC; the bank (jog); each a second */
    button(m, MR_CC_SHIFT, 1), jog(m, 2), button(m, MR_CC_SHIFT, 0);
    CHECK(rfmv_pop_up(m, POP_TEMPO) && m->seq.bpm10 == 1220);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_pop_tempo.pbm", out);
    knob(m, 3, 5);
    CHECK(rfmv_pop_up(m, POP_CC) && !rfmv_pop_up(m, POP_TEMPO) && m->pop_i == 3);
    jog(m, 1);
    CHECK(rfmv_pop_up(m, POP_BANK) && m->s.bank == 1);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_pop_bank.pbm", out);
    adv(m, pop_frames);
    CHECK(!m->pop || !rfmv_pop_up(m, m->pop));
    /* a message over a popup that came first; a popup over an older message */
    knob(m, 3, 1);
    adv(m, 128);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 9), button(m, MR_CC_SHIFT, 0);
    rfm_move_screen(m, NULL, out);
    CHECK(rfmv_seq_msg_text(m) && !px_at(out, 66, 0));  /* the message (no hatching) */
    adv(m, 128);
    knob(m, 3, 1);
    rfm_move_screen(m, NULL, out);
    CHECK(px_at(out, 66, 0));                           /* the knob's popup (hatched behind) */
    /* a held step and a knob: the step's CC lock */
    adv(m, pop_frames);
    step(m, 4, 1);
    knob(m, 3, 3);
    rfm_move_screen(m, NULL, out);
    CHECK(rfmv_pop_up(m, POP_CC) && rfmv_seq_lock_get(m, 0, 4, m->s.cc[1][3]) >= 0);
    write_pbm("move_seq_pop_lock.pbm", out);
    /* five notes on a step: two chips and "+3" */
    {
        int n;
        for (n = 0; n < 5; n++)
            rfmv_seq_note_put(m, 0, 4, 50 + 2 * n, 90, 36, 0);
    }
    knob(m, 3, 0);
    m->pop = POP_NONE;
    step(m, 4, 0), step(m, 4, 1);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_pop_step5.pbm", out);
    step(m, 4, 0);
    rfm_move_release_all(m);
}

static uint64_t b0;                                     /* (the frame a play started at) */
static void rest(rfm_move_t *m) { adv(m, RATE * RFM_MOVE_POPUP_MS / 1000 + 256); }   /* (popups gone) */

/* ================================================================ the owner's Move test (2026-11-17) === */
static int count_on(int ch, int note)                   /* note ons in acts[] */
{
    int i, n = 0;
    for (i = 0; i < nact; i++)
        n += acts[i].a.kind == RFM_MOVE_ACT_MIDI && acts[i].a.bytes[0] == (0x90 | ch) && acts[i].a.bytes[1] == note &&
             acts[i].a.bytes[2];
    return n;
}
static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < nact; i++)
        n += acts[i].a.kind == kind;
    return n;
}

/* live recording while playing: the note goes in at its press (where it was heard, the host's lead taken off), its
 * length at the release; the take does not double the held note */
static void test_live_record(void)
{
    rfm_move_t *m = &M;
    int i;
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 60, 100, 24, 0);         /* (a clip playing) */
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 2 * FPS);
    press(m, MR_CC_REC);                                /* Capture mode, playing: records into the existing pages */
    CHECK(m->seq.rec && !m->seq.srec);
    adv(m, 2 * FPS + 7 * FPT);                          /* step 5 + 7 ticks (T 103) */
    pad(m, 1, 90);                                      /* D3 */
    i = rfmv_seq_find(m, 0, 4, 50);
    CHECK(i >= 0 && m->seq.notes[i].micro == 7);        /* (in at the press: visible while held) */
    clear();
    adv(m, 2 * FPS);
    pad(m, 1, 0);
    i = rfmv_seq_find(m, 0, 4, 50);
    CHECK(i >= 0 && m->seq.notes[i].len == 48 && count_on(0, 50) == 0);   /* (not played again while held) */
    /* the host's lead (the web player's 30 ms): what is played lands where it was heard */
    rfm_move_seq_set_lead_us(m, 31250);                 /* 6 ticks at 120 BPM */
    adv(m, 16 * FPS - (m->seq.T % 384) * FPT + 8 * FPS + 10 * FPT);   /* the next bar's step 9 + 10 ticks */
    pad(m, 2, 100), pad(m, 2, 0);                       /* (played at +10 ticks and a little: heard 6 earlier) */
    i = rfmv_seq_find(m, 0, 8, 52);
    CHECK(i >= 0 && m->seq.notes[i].micro >= 4 && m->seq.notes[i].micro <= 5);
    rfm_move_seq_set_lead_us(m, 0);
    /* Replace: the playhead clears what the take passes over (not the take's own notes) */
    press(m, MR_CC_REC);
    CHECK(!m->seq.rec);
    m->seq.rec_style = RFM_MOVE_RS_REPLACE;
    press(m, MR_CC_REC);
    adv(m, 16 * FPS + FPT);
    CHECK(rfmv_seq_find(m, 0, 0, 60) < 0 && rfmv_seq_find(m, 0, 4, 50) < 0);
    rfm_move_seq_play(m, 0);
    CHECK(stuck() == 0);
    /* Capture mode, stopped (the owner's intentional capture): Record arms; the first note clears the clip, starts
     * the transport and is the clip's first step; Record again ends it at the nearest bar */
    fresh(m);
    rfmv_seq_note_put(m, 0, 3, 72, 100, 24, 0);         /* (an old note: cleared) */
    press(m, MR_CC_REC);
    CHECK(m->seq.srec == 1 && !m->seq.start_pending);
    adv(m, 3 * FPS);
    pad(m, 0, 100);
    CHECK(m->seq.srec == 2 && m->seq.start_pending == 1 && rfmv_seq_find(m, 0, 0, 48) >= 0 && rfmv_seq_find(m, 0, 3, 72) < 0);
    adv(m, FPS), pad(m, 0, 0);
    adv(m, 16 * FPS + 2 * FPS);                         /* bar 2, step 4 */
    pad(m, 4, 100), adv(m, FPS / 2), pad(m, 4, 0);
    adv(m, 16 * FPS);                                   /* bar 3, step 4.5: */
    press(m, MR_CC_REC);                                /* .. nearest bar line: the 2nd: 2 bars, at once */
    CHECK(!m->seq.srec && !m->seq.rec && m->seq.len[0] == 32 && m->seq.tr[0].grow == 0);
    CHECK(rfmv_seq_find(m, 0, 19, 55) >= 0 && nnotes_clip(m, 0) == 2);
    CHECK(m->seq.notes[rfmv_seq_find(m, 0, 0, 48)].len == 24);
    CHECK(rfmv_seq_clip_pos(m, 0) == 2 || rfmv_seq_clip_pos(m, 0) == 3);   /* (the loop went on, in phase) */
    press(m, MR_CC_UNDO);                               /* (the take is one undo: the old note back) */
    CHECK(rfmv_seq_find(m, 0, 3, 72) >= 0 && nnotes_clip(m, 0) == 1);
    rfm_move_seq_play(m, 0);
    /* Step mode: each note or chord into the cursor's step, then a step on (the track's rate) */
    fresh(m);
    m->seq.rec_mode = RFM_MOVE_RM_STEP;
    press(m, MR_CC_REC);
    CHECK(m->seq.steprec && m->seq.step_cur == 0);
    pad(m, 0, 100), pad(m, 2, 90), pad(m, 0, 0), pad(m, 2, 0);   /* a chord */
    pad(m, 4, 80), pad(m, 4, 0);
    CHECK(rfmv_seq_find(m, 0, 0, 48) >= 0 && rfmv_seq_find(m, 0, 0, 52) >= 0 && rfmv_seq_find(m, 0, 1, 55) >= 0);
    CHECK(m->seq.step_cur == 2);
    press(m, MR_CC_REC);
    CHECK(!m->seq.steprec);
    rfm_move_seq_play(m, 0);
    adv(m, 2 * FPS);
    CHECK(stuck() == 0);
    /* the record settings: Shift + Record, the jog = the mode, knob 2 = the style; saved with the project */
    button(m, MR_CC_SHIFT, 1), press(m, MR_CC_REC), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.smenu == RFM_MOVE_SM_REC);
    jog(m, -2), knob(m, 1, 1);
    CHECK(m->seq.rec_mode == RFM_MOVE_RM_CAPTURE && m->seq.rec_style == RFM_MOVE_RS_REPLACE);
    press(m, MR_CC_BACK);
    CHECK(!m->seq.smenu && m->s.mode == RFM_MOVE_MODE_SEQ);
}

/* the TRIG page (jog left of bank 1): micro, ratchet, chance, condition, function, its amount, length */
static void test_trig(void)
{
    rfm_move_t *m = &M;
    int i, n1, n2;
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 60, 100, 24, 0);
    rfmv_seq_note_put(m, 0, 4, 62, 100, 24, 0);
    rfmv_seq_note_put(m, 0, 8, 64, 100, 24, 0);
    jog(m, -1);
    CHECK(m->seq.trig && m->pop == POP_BANK);
    step(m, 4, 1);
    knob(m, 1, 3);                                      /* ratchet x4 */
    knob(m, 0, -3);                                     /* micro -3 */
    knob(m, 6, 2);                                      /* length 1 step -> 2 steps (the table: 1, 1.5, 2) */
    step(m, 4, 0);
    i = rfmv_seq_find(m, 0, 4, 62);
    CHECK(rfmv_seq_trig(m, 0, 4, RFM_MOVE_TRIG_RATCHET) == 4 && m->seq.notes[i].micro == -3 && m->seq.notes[i].len == 48);
    CHECK(rfmv_seq_find(m, 0, 4, 62) >= 0);             /* (edited while held: kept) */
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 16 * FPS);
    CHECK(count_on(0, 62) == 4 && count_on(0, 60) == 1);
    {   /* the four hits a quarter step apart, the first 3 ticks early */
        uint64_t at;
        find_midi(0x90, 62, 100, &at);
        CHECK(at - b0 == (uint64_t)(4 * 24 - 3) * FPT);
    }
    rfm_move_seq_play(m, 0);
    adv(m, 2 * FPS);
    /* condition 1:2 on step 9: every other pass; FUNC JUMP on step 1 to step 9 (steps 2..8 skipped) */
    step(m, 8, 1), knob(m, 3, 1), step(m, 8, 0);       /* 1:2 */
    CHECK(rfmv_seq_trig(m, 0, 8, RFM_MOVE_TRIG_COND) == 1);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 4 * 16 * FPS);
    CHECK(count_on(0, 64) == 2 && count_on(0, 60) == 4);
    rfm_move_seq_play(m, 0);
    step(m, 0, 1), knob(m, 4, RFM_MOVE_FN_JUMP), knob(m, 5, 8), step(m, 0, 0);
    CHECK(rfmv_seq_trig(m, 0, 0, RFM_MOVE_TRIG_FUNC) == RFM_MOVE_FN_JUMP && rfmv_seq_trig(m, 0, 0, RFM_MOVE_TRIG_JUMP) == 8);
    step(m, 8, 1), knob(m, 3, -1), step(m, 8, 0);      /* (no condition) */
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 16 * FPS);
    CHECK(count_on(0, 62) == 0 && count_on(0, 64) >= 1);   /* (step 5 skipped: the jump lands on step 9) */
    {
        uint64_t at;
        find_midi(0x90, 64, 100, &at);
        CHECK(at - b0 == FPS);                           /* (step 9 right after step 1) */
    }
    rfm_move_seq_play(m, 0);
    /* REVERSE on step 1: then 16, 15 .. (the next step after step 1 is step 16) */
    step(m, 0, 1), knob(m, 4, -(RFM_MOVE_FN_JUMP - RFM_MOVE_FN_REVERSE)), step(m, 0, 0);
    rfmv_seq_note_put(m, 0, 15, 67, 100, 24, 0);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 3 * FPS);
    {
        uint64_t at;
        CHECK(find_midi(0x90, 67, 100, &at) && at - b0 == FPS);
    }
    rfm_move_seq_play(m, 0);
    /* chance: about half of 64 passes */
    step(m, 0, 1), knob(m, 4, -RFM_MOVE_FN_REVERSE), knob(m, 2, -50), step(m, 0, 0);
    CHECK(rfmv_seq_trig(m, 0, 0, RFM_MOVE_TRIG_CHANCE) == 50 && rfmv_seq_trig(m, 0, 0, RFM_MOVE_TRIG_FUNC) < 0);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 64 * 16 * FPS);
    n1 = count_on(0, 60);
    CHECKF(n1 > 16 && n1 < 48, "chance 50: %d of 64", n1);
    rfm_move_seq_play(m, 0);
    /* no step held: the whole clip's steps with notes */
    knob(m, 1, 1);
    CHECK(rfmv_seq_trig(m, 0, 8, RFM_MOVE_TRIG_RATCHET) == 2 && rfmv_seq_trig(m, 0, 15, RFM_MOVE_TRIG_RATCHET) == 2);
    CHECK(rfmv_seq_trig(m, 0, 3, RFM_MOVE_TRIG_RATCHET) < 0);
    /* the popup: its picture and words */
    {
        uint8_t out[MR_SCREEN_BYTES];
        m->touched = 1;
        rfmv_pop(m, POP_TRIG, 1);
        rfm_move_screen(m, NULL, out);
        CHECK(px_at(out, 2, 2) && px_at(out, 8, 30));
        write_pbm("move_seq_pop_trig.pbm", out);
        m->touched = -1;
    }
    jog(m, 1);
    CHECK(!m->seq.trig && m->s.bank == 0);
    (void)n2;
    adv(m, 2 * FPS);
    CHECK(stuck() == 0);
}

/* Mute held, Track 4 held (CC numbers, the layout, the rate), FILL, the metronome, groove, the arp, the Shift
 * menus and Shift's lock */
static void test_controls(void)
{
    rfm_move_t *m = &M;
    uint8_t out[MR_SCREEN_BYTES];
    int i, lo;
    fresh(m);
    rfmv_seq_note_put(m, SEQ_CLIP(1, 0), 0, 60, 100, 24, 0);
    button(m, MR_CC_MUTE, 1);
    rfm_move_screen(m, NULL, out);
    CHECK(px_at(out, 17 + 7, 2 + 13) && !m->seq.tr[1].mute);   /* (the tracks: track 2 has notes: filled) */
    tap_step(m, 1);
    rfm_move_screen(m, NULL, out);
    button(m, MR_CC_MUTE, 0);
    CHECK(m->seq.tr[1].mute && px_at(out, 17, 2) && !px_at(out, 17 + 4, 2 + 13));   /* (crossed out) */
    write_pbm("move_seq_mute.pbm", out);
    /* Track 1 held (the selected track's button): knob 3 = its CC number, the top row's pads the layout, Up / Down
     * the rate */
    track(m, 0, 1);
    knob(m, 2, 4);
    i = m->s.cc[0][2];
    knob(m, 2, -1);
    CHECK(m->s.cc[0][2] == i - 1 && m->menu_what == MENU_CC);
    pad(m, 25, 100), pad(m, 25, 0);                     /* chromatic */
    CHECK(m->s.layout == RFM_MOVE_LAYOUT_CHROMATIC);
    press(m, MR_CC_DOWN);                               /* 1/16 -> 1/8T: slower */
    CHECK(m->seq.tr[0].rate == RFM_MOVE_SEQ_RATE_16 + 1 && m->menu_what == MENU_RATE);
    press(m, MR_CC_DOWN), press(m, MR_CC_UP), press(m, MR_CC_DOWN);   /* 1/8 */
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_trackmenu_rate.pbm", out);
    track(m, 0, 0);
    CHECK(m->seq.tr[0].rate == 5 && m->s.mode == RFM_MOVE_MODE_SEQ);
    m->s.layout = RFM_MOVE_LAYOUT_SCALE;
    rfmv_seq_note_put(m, 0, 1, 64, 100, 24, 0);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 3 * FPS);
    {
        uint64_t at;
        CHECK(find_midi(0x90, 64, 100, &at) && at - b0 == 2 * FPS);   /* (step 2 at 1/8: two 16ths in) */
    }
    rfm_move_seq_play(m, 0);
    m->seq.tr[0].rate = RFM_MOVE_SEQ_RATE_16;
    /* FILL (the jog wheel held): a FILL step plays only then */
    rfmv_seq_lock_set(m, 0, 1, RFM_MOVE_TRIG_COND, RFM_MOVE_COND_FILL);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 16 * FPS);
    CHECK(count_on(0, 64) == 0);
    raw(m, 0xB0, MR_CC_JOG_CLICK, 127);
    CHECK(m->seq.fill);
    adv(m, 16 * FPS);
    raw(m, 0xB0, MR_CC_JOG_CLICK, 0);
    CHECK(count_on(0, 64) == 1 && !m->seq.fill);
    rfm_move_seq_play(m, 0);
    /* Shift + step 6: the metronome (a click a beat, the bar's first marked) */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 5), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.metro);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 16 * FPS + 1);
    CHECK(count_kind(RFM_MOVE_ACT_CLICK) == 5);
    for (i = 0; i < nact && acts[i].a.kind != RFM_MOVE_ACT_CLICK; i++) {}
    CHECK(i < nact && acts[i].a.value == 1 && acts[i].at == b0);
    rfm_move_seq_play(m, 0);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 5), button(m, MR_CC_SHIFT, 0);
    CHECK(!m->seq.metro);
    /* Shift + step 7: groove (the jog): an off 16th late */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 6), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.smenu == RFM_MOVE_SM_GROOVE);
    jog(m, 15);                                         /* (5 % a detent: 75 %) */
    CHECK(m->seq.tr[0].groove == 75);
    rest(m);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_groove.pbm", out);
    press(m, MR_CC_BACK);
    rfmv_seq_lock_set(m, 0, 1, RFM_MOVE_TRIG_COND, -1);
    clear();
    rfm_move_seq_play(m, 1), b0 = base;
    adv(m, 3 * FPS);
    {
        uint64_t at;
        CHECK(find_midi(0x90, 64, 100, &at) && at - b0 == FPS + 6 * FPT);   /* (step 2: 75 % of a third of a step late) */
    }
    rfm_move_seq_play(m, 0);
    m->seq.tr[0].groove = 0;
    /* Shift + step 3: Workflow Settings: the jog the channel, knob 2 the rate, knob 3 Quantize's amount */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 2), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.smenu == RFM_MOVE_SM_WORKFLOW);
    jog(m, 2), knob(m, 1, -1), knob(m, 2, -10);
    CHECK(m->seq.tr[0].ch == 2 && m->seq.tr[0].rate == 2 && m->seq.qstrength == 50);
    rest(m);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_workflow.pbm", out);
    press(m, MR_CC_BACK);
    m->seq.tr[0].ch = 0, m->seq.tr[0].rate = RFM_MOVE_SEQ_RATE_16;
    rfmv_seq_note_put(m, 0, 2, 66, 100, 24, 10);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 15), button(m, MR_CC_SHIFT, 0);   /* Shift + 16: quantize, 50 % */
    CHECK(m->seq.notes[rfmv_seq_find(m, 0, 2, 66)].micro == 5);
    /* Shift + step 5: the tempo (the jog); following the firmware's clock: locked */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 4), button(m, MR_CC_SHIFT, 0);
    jog(m, 3);
    CHECK(m->seq.smenu == RFM_MOVE_SM_TEMPO && m->seq.bpm10 == 1230);
    rest(m);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_tempo.pbm", out);
    rfm_move_seq_set_clock(m, RFM_MOVE_CLOCK_EXTERNAL);
    jog(m, 3);
    CHECK(m->seq.bpm10 == 1230);
    rfm_move_seq_set_clock(m, RFM_MOVE_CLOCK_AUTO);
    m->seq.following = 0;
    press(m, MR_CC_BACK);
    /* Shift + step 8 / 9: Layout, Keys & Scales (the pads pick; the jog the root) */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 7), button(m, MR_CC_SHIFT, 0);
    clear();
    pad(m, 27, 100), pad(m, 27, 0);
    CHECK(m->seq.smenu == RFM_MOVE_SM_LAYOUT && m->s.layout == RFM_MOVE_LAYOUT_OCTAVE && count_kind(RFM_MOVE_ACT_MIDI) == 0);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 8), button(m, MR_CC_SHIFT, 0);
    pad(m, 17, 100), pad(m, 17, 0), jog(m, 2);
    CHECK(m->seq.smenu == RFM_MOVE_SM_KEYS && m->s.scale == 1 && m->s.root == 2);
    rest(m);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_keys.pbm", out);
    button(m, MR_CC_SHIFT, 1), tap_step(m, 8), button(m, MR_CC_SHIFT, 0);   /* (again: closed) */
    CHECK(!m->seq.smenu);
    rfm_move_set_scale(m, 0, 0), m->s.layout = RFM_MOVE_LAYOUT_SCALE;
    /* Shift + step 11: the arp (FIZZ's): MODE ON, UP over two octaves at 1/16: C3 E3, C4 E4, C3 .. */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 10), button(m, MR_CC_SHIFT, 0);
    knob(m, 4, 1), knob(m, 2, 1);                       /* MODE ON, RANGE 2 */
    CHECK(m->seq.smenu == RFM_MOVE_SM_ARP && m->seq.tr[0].arp.mode == RFM_MOVE_AM_ON && m->seq.tr[0].arp.range == 2);
    rest(m);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_arp.pbm", out);
    press(m, MR_CC_BACK);
    clear();
    pad(m, 0, 100), pad(m, 2, 90);
    CHECK(count_on(0, 48) == 0);                        /* (the arp plays them, on the next 16th) */
    adv(m, 4 * FPS + FPT);
    pad(m, 0, 0), pad(m, 2, 0);
    CHECK(count_on(0, 48) == 1 && count_on(0, 52) == 1 && count_on(0, 60) == 1 && count_on(0, 64) == 1);
    adv(m, 4 * FPS);
    CHECK(count_on(0, 48) == 1 && !m->seq.arp_run && stuck() == 0);   /* (released: it stops) */
    m->seq.tr[0].arp.mode = RFM_MOVE_AM_OFF;
    /* Shift + step 14: a new clip in the track's next empty slot, selected */
    button(m, MR_CC_SHIFT, 1), tap_step(m, 13), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.tr[0].sel == 1 && rfmv_seq_clip_has(m, SEQ_CLIP(0, 1)) && !rfmv_seq_clip_used(m, SEQ_CLIP(0, 1)));
    /* Loop tapped: latched, the jog sets the length (a step a detent, growing into new pages); Loop again ends it */
    m->seq.len[clip_of(m)] = 16;
    press(m, MR_CC_LOOP);
    CHECK(m->seq.loop_latch && (m->seq.mods & SEQ_M_LOOP));
    jog(m, 5);
    CHECK(m->seq.len[clip_of(m)] == 21 && rfmv_pop_up(m, POP_LOOP));
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_loop_latch.pbm", out);
    button(m, MR_CC_SHIFT, 1), jog(m, 1), button(m, MR_CC_SHIFT, 0);
    CHECK(m->seq.len[clip_of(m)] == 37);
    tap_step(m, 3);                                     /* (the steps: still the pages) */
    CHECK(m->seq.page == 3);
    press(m, MR_CC_LOOP);
    CHECK(!m->seq.loop_latch && !(m->seq.mods & SEQ_M_LOOP));
    jog(m, 1);
    CHECK(m->seq.len[clip_of(m)] == 37);                         /* (the jog: the bank again) */
    press(m, MR_CC_LOOP), press(m, MR_CC_BACK);         /* (Back ends it too) */
    CHECK(!m->seq.loop_latch && !(m->seq.mods & SEQ_M_LOOP) && m->s.mode == RFM_MOVE_MODE_SEQ);
    m->seq.page = 0, m->s.bank = 0;
    /* Right past the clip's last page: pending pages (+1, +2: the clip unchanged), Left back; a step on one grows the
     * clip to it */
    {
        const int c = clip_of(m);
        rfmv_seq_loop_set(m, c, 0, 16);
        m->seq.page = 0;
        press(m, MR_CC_RIGHT), press(m, MR_CC_RIGHT), press(m, MR_CC_RIGHT);
        CHECK(m->seq.page == 3 && m->seq.len[c] == 16);
        rest(m);
        rfm_move_screen(m, NULL, out);
        write_pbm("move_seq_pending.pbm", out);
        CHECK(m->pop == POP_BAR && m->pop_i == 3);      /* ("BAR 4", a second) */
        CHECK(px_at(out, 0, 2) && px_at(out, 20, 2) && !px_at(out, 40, 2));   /* (the real page: a line, a ghost blank) */
        CHECK(px_at(out, 100 / 4 * 4 - 100 / 8, 0));   /* (the "+" at the far end) */
        rest(m);
        rfm_move_screen(m, NULL, out);
        write_pbm("move_seq_pending_rest.pbm", out);
        CHECK(!rfmv_pop_up(m, POP_BAR) && px_at(out, 127 - 6 * 4 - 3, 0));   /* (the box: "16/1") */
        press(m, MR_CC_LEFT);
        CHECK(m->seq.page == 2 && m->seq.len[c] == 16);
        tap_step(m, 4);                                 /* page 3, step 5: the clip is 3 pages now */
        CHECK(m->seq.len[c] == 48 && rfmv_seq_find(m, c, 36, 48) >= 0);
        press(m, MR_CC_RIGHT);
        CHECK(m->seq.page == 3 && m->seq.len[c] == 48);
        press(m, MR_CC_LEFT), press(m, MR_CC_LEFT), press(m, MR_CC_LEFT), press(m, MR_CC_LEFT);
        CHECK(m->seq.page == 0);
        adv(m, 2 * FPS);
    }
    /* Shift's lock: a double tap latches it (step 2 = the clock without holding); a press unlatches it */
    press(m, MR_CC_SHIFT), press(m, MR_CC_SHIFT);
    CHECK(m->seq.shift_lock && (m->seq.mods & SEQ_M_SHIFT));
    i = m->seq.clock_mode;
    tap_step(m, 1);
    CHECK(m->seq.clock_mode == (i + 1) % 3);
    press(m, MR_CC_SHIFT);
    CHECK(!m->seq.shift_lock && !(m->seq.mods & SEQ_M_SHIFT));
    now_ms += 1000;
    tap_step(m, 1);
    CHECK(m->seq.clock_mode == (i + 1) % 3);            /* (a step again) */
    rfm_move_seq_set_clock(m, RFM_MOVE_CLOCK_AUTO);
    (void)lo;
}

/* Capture: the last phrase (a silence of more than 2 bars ends it), its own tempo in an empty project */
static void test_capture2(void)
{
    rfm_move_t *m = &M;
    int i;
    fresh(m);
    /* an old phrase, 3 bars of silence, then the one to keep */
    pad(m, 6, 100), adv(m, FPS), pad(m, 6, 0);
    adv(m, 3 * 16 * FPS);
    /* played at 100 BPM (7200 frames a 16th at 48 kHz), 8ths: C D E F G A B C (a bar) */
    for (i = 0; i < 8; i++) {
        pad(m, i, 100), adv(m, 7200), pad(m, i, 0), adv(m, 7200);
    }
    adv(m, 7200 * 2);
    press(m, MR_CC_CAPTURE);
    CHECKF(abs(m->seq.bpm10 - 1000) <= 5, "the tempo found: %d", m->seq.bpm10);
    CHECK(m->seq.len[0] == 16 && nnotes_clip(m, 0) == 8 && rfmv_seq_find(m, 0, 6, 9 + 48) < 0);
    CHECK(rfmv_seq_find(m, 0, 0, 48) >= 0 && rfmv_seq_find(m, 0, 2, 50) >= 0 && rfmv_seq_find(m, 0, 14, 60) >= 0);
    CHECK(abs(m->seq.notes[rfmv_seq_find(m, 0, 14, 60)].micro) <= 1);
    rfm_move_seq_play(m, 0);
}

/* Sample: the audio loop, from a bar line to a bar line (whole bars) */
static void test_audio_loop(void)
{
    rfm_move_t *m = &M;
    int i, a = -1, b = -1;
    fresh(m);
    rfmv_seq_note_put(m, 0, 0, 60, 100, 24, 0);
    press(m, MR_CC_SAMPLE);
    CHECK(m->seq.arec == 1 && !m->seq.start_pending);
    adv(m, FPS);
    clear();
    pad(m, 0, 100), pad(m, 0, 0);                       /* stopped: the first note starts the transport */
    CHECK(m->seq.start_pending == 1);
    adv(m, 16 * FPS + 5 * FPS);
    CHECK(m->seq.arec == 2);
    {
        uint8_t out[MR_SCREEN_BYTES];
        rfm_move_screen(m, NULL, out);
        CHECK(px_at(out, 100, 6));                      /* (the waveform, top right) */
    }
    press(m, MR_CC_SAMPLE);                             /* done at the next bar line: 2 bars */
    CHECK(m->seq.arec == 3);
    adv(m, 16 * FPS);
    for (i = 0; i < nact; i++)
        if (acts[i].a.kind == RFM_MOVE_ACT_AUDIO_REC)
            *(acts[i].a.value ? &a : &b) = (int)acts[i].at;
    CHECKF(b - a == 2 * 16 * (int)FPS && m->seq.arec == 0, "loop %d..%d", a, b);
    for (i = 0; i < nact; i++)
        if (acts[i].a.kind == RFM_MOVE_ACT_AUDIO_REC && !acts[i].a.value)
            CHECK(acts[i].a.index == 2);
    rfm_move_seq_play(m, 0);
}

/* the project blob, version 2's fields; a version 1 blob still loads */
static void test_blob_v2(void)
{
    static uint8_t a[RFM_MOVE_PROJECT_MAX], b[RFM_MOVE_PROJECT_MAX];
    rfm_move_t *m = &M, *m2 = &M2;
    int n, n1;
    fresh(m);
    rfmv_seq_note_put(m, 0, 20, 60, 100, 24, 0);
    rfmv_seq_loop_set(m, 0, 16, 32);
    rfmv_seq_clip_make(m, SEQ_CLIP(2, 3), 1);
    rfmv_seq_lock_set(m, 0, 20, RFM_MOVE_TRIG_COND, RFM_MOVE_COND_NFILL);
    rfmv_seq_lock_set(m, 0, 20, RFM_MOVE_TRIG_JUMP, 200);
    m->seq.tr[3].rate = 7, m->seq.tr[3].groove = 33, m->seq.tr[3].arp.pat = RFM_MOVE_AP_CONDIV, m->seq.tr[3].arp.odist = -7;
    m->seq.rec_mode = RFM_MOVE_RM_STEP, m->seq.rec_style = RFM_MOVE_RS_REPLACE;
    n = rfm_move_project_save(m, a, sizeof a);
    CHECK(n == RFM_MOVE_PROJECT_FIXED + 7 + 2 * 4 && a[4] == 2);
    rfm_move_init(m2, NULL, on_act, NULL);
    CHECK(rfm_move_project_load(m2, 1, a, (uint32_t)n) == 0);
    CHECK(m2->seq.lstart[0] == 16 && m2->seq.len[0] == 32 && rfmv_seq_clip_has(m2, SEQ_CLIP(2, 3)));
    CHECK(rfmv_seq_trig(m2, 0, 20, RFM_MOVE_TRIG_JUMP) == 200 && m2->seq.tr[3].rate == 7 && m2->seq.tr[3].groove == 33);
    CHECK(m2->seq.tr[3].arp.pat == RFM_MOVE_AP_CONDIV && m2->seq.tr[3].arp.odist == -7);
    CHECK(m2->seq.rec_mode == RFM_MOVE_RM_STEP && m2->seq.rec_style == RFM_MOVE_RS_REPLACE);
    /* a bad trig value, a loop past the end: refused */
    memcpy(b, a, (size_t)n), b[n - 4 - RFM_MOVE_PROJECT_EXT + 0] = 250, refnv(b, (uint32_t)n);
    CHECK(rfm_move_project_check(b, (uint32_t)n) == -9);
    /* version 1: no extension, plain CC locks only */
    fresh(m);
    rfmv_seq_note_put(m, SEQ_CLIP(1, 2), 3, 60, 100, 24, 0);
    n = rfm_move_project_save(m, a, sizeof a);
    n1 = n - RFM_MOVE_PROJECT_EXT;
    memcpy(b, a, (size_t)n1 - 4), b[4] = 1;
    refnv(b, (uint32_t)n1);
    rfm_move_init(m2, NULL, on_act, NULL);
    CHECK(rfm_move_project_check(b, (uint32_t)n1) == 0 && rfm_move_project_load(m2, 1, b, (uint32_t)n1) == 0);
    CHECK(rfmv_seq_find(m2, SEQ_CLIP(1, 2), 3, 60) >= 0 && rfmv_seq_clip_has(m2, SEQ_CLIP(1, 2)) && m2->seq.tr[0].rate == RFM_MOVE_SEQ_RATE_16);
    CHECK(!rfmv_seq_clip_has(m2, SEQ_CLIP(1, 3)) && m2->seq.lstart[SEQ_CLIP(1, 2)] == 0);
}

/* 16 tracks, the Track buttons a bank of 4 (Shift + Track n: bank n), each track its own layout and CC numbers;
 * Back closes the latest layer; a held step's jog, Left / Right and volume knob with their popups */
static void test_tracks16(void)
{
    rfm_move_t *m = &M, *m2 = &M2;
    static uint8_t blob[RFM_MOVE_PROJECT_MAX];
    uint8_t out[MR_SCREEN_BYTES];
    int n, i;
    fresh(m);
    CHECK(m->seq.sel_track == 0 && m->seq.bank == 0);
    track(m, 2, 1), track(m, 2, 0);                     /* Track 3: track 3 */
    CHECK(m->seq.sel_track == 2 && m->s.mode == RFM_MOVE_MODE_SEQ);
    button(m, MR_CC_SHIFT, 1), track(m, 3, 1), track(m, 3, 0), button(m, MR_CC_SHIFT, 0);   /* bank 4: track 15 */
    CHECK(m->seq.bank == 3 && m->seq.sel_track == 14 && m->pop == POP_TRACK);
    rfm_move_screen(m, NULL, out);
    write_pbm("move_seq_pop_track.pbm", out);
    track(m, 0, 1), track(m, 0, 0);
    CHECK(m->seq.sel_track == 12);
    clear();
    pad(m, 0, 100), pad(m, 0, 0);
    CHECK(find_midi(0x9C, 48, 100, NULL));              /* (track 13 plays channel 13) */
    {   /* the Track LEDs: the bank's tracks, the selected one full */
        rfm_move_led_t leds[RFM_MOVE_NLEDS];
        int k, ok = 0;
        rfm_move_leds_invalidate(m);
        k = rfm_move_leds(m, leds, RFM_MOVE_NLEDS);
        for (i = 0; i < k; i++)
            if (leds[i].cc && leds[i].number == MR_CC_TRACK1)
                ok = leds[i].r > 100 || leds[i].g > 100 || leds[i].b > 100;
        CHECK(ok);
    }
    button(m, MR_CC_MUTE, 1), track(m, 1, 1), track(m, 1, 0), button(m, MR_CC_MUTE, 0);   /* Mute + Track 2: track 14 */
    CHECK(m->seq.tr[13].mute && m->seq.sel_track == 12);
    /* each track its own layout and CC numbers (Track held: the top row's pads, the knobs) */
    track(m, 0, 1), pad(m, 26, 100), pad(m, 26, 0), knob(m, 0, 5), track(m, 0, 0);
    CHECK(m->s.layout == RFM_MOVE_LAYOUT_DRUM && m->s.cc[0][0] == 21);
    track(m, 2, 1), track(m, 2, 0);                     /* track 15: its own (the defaults) */
    CHECK(m->seq.sel_track == 14 && m->s.layout == RFM_MOVE_LAYOUT_SCALE && m->s.cc[0][0] == 16);
    track(m, 0, 1), track(m, 0, 0);
    CHECK(m->s.layout == RFM_MOVE_LAYOUT_DRUM && m->s.cc[0][0] == 21);
    press(m, MR_CC_BACK);                               /* FM-1 mode: MIDI mode's settings back */
    CHECK(m->s.mode == RFM_MOVE_MODE_FM1 && m->s.layout == RFM_MOVE_LAYOUT_SCALE && m->s.cc[0][0] == 16);
    track(m, 3, 1), track(m, 3, 0);
    CHECK(m->s.mode == RFM_MOVE_MODE_SEQ && m->s.layout == RFM_MOVE_LAYOUT_DRUM);
    n = rfm_move_project_save(m, blob, sizeof blob);
    rfm_move_init(m2, NULL, on_act, NULL);
    track(m2, 3, 1), track(m2, 3, 0);
    CHECK(rfm_move_project_load(m2, 2, blob, (uint32_t)n) == 0);
    CHECK(m2->seq.sel_track == 12 && m2->seq.bank == 3 && m2->s.layout == RFM_MOVE_LAYOUT_DRUM && m2->s.cc[0][0] == 21);
    CHECK(m2->seq.tr[14].layout == RFM_MOVE_LAYOUT_SCALE && m2->seq.tr[13].mute);
    /* Back: the latest layer first (Shift locked, then a Shift menu: Back closes the menu, then the lock) */
    m->s.layout = RFM_MOVE_LAYOUT_SCALE;
    now_ms += 1000;
    press(m, MR_CC_SHIFT), press(m, MR_CC_SHIFT);
    CHECK(m->seq.shift_lock);
    tap_step(m, 8);                                     /* (Shift locked: Keys & Scales) */
    CHECK(m->seq.smenu == RFM_MOVE_SM_KEYS);
    press(m, MR_CC_LOOP);                               /* Loop latched too */
    CHECK(m->seq.loop_latch);
    press(m, MR_CC_BACK);
    CHECK(!m->seq.loop_latch && m->seq.smenu == RFM_MOVE_SM_KEYS && m->seq.shift_lock);
    press(m, MR_CC_BACK);
    CHECK(!m->seq.smenu && m->seq.shift_lock);
    press(m, MR_CC_BACK);
    CHECK(!m->seq.shift_lock && !(m->seq.mods & SEQ_M_SHIFT) && m->s.mode == RFM_MOVE_MODE_SEQ);
    rfmv_seq_note_put(m, clip_of(m), 3, 60, 100, 24, 0);
    button(m, MR_CC_COPY, 1), tap_step(m, 3), button(m, MR_CC_COPY, 0);   /* the clipboard's mark */
    CHECK(rfmv_seq_src_step(m, clip_of(m), 3) == 1);
    press(m, MR_CC_BACK);
    CHECK(rfmv_seq_src_step(m, clip_of(m), 3) == 0 && m->s.mode == RFM_MOVE_MODE_SEQ);
    press(m, MR_CC_BACK);                               /* nothing on: FM-1 mode */
    CHECK(m->s.mode == RFM_MOVE_MODE_FM1);
    track(m, 3, 1), track(m, 3, 0);
    /* a held step: the jog the length (LENGTH's popup), Left / Right the micro timing (MICRO's), the volume knob the
     * velocity (VELO's; MASTER untouched) */
    {
        const int c = clip_of(m), mst = m->master;
        int k;
        m->seq.page = 0;
        step(m, 3, 1);
        jog(m, 6);
        k = rfmv_seq_find(m, c, 3, 60);
        CHECK(m->seq.notes[k].len == 36 && m->pop == POP_TRIG && m->pop_i == 6);
        press(m, MR_CC_RIGHT);
        CHECK(m->seq.notes[k].micro == 2 && m->seq.page == 0 && m->pop == POP_TRIG && m->pop_i == 0);
        knob(m, 8, -20);
        CHECK(m->seq.notes[k].vel == 80 && m->master == mst && m->pop == POP_TRIG && m->pop_i == 7);
        rfm_move_screen(m, NULL, out);
        write_pbm("move_seq_pop_velo.pbm", out);
        step(m, 3, 0);
        knob(m, 8, -2);
        CHECK(m->master == mst - 32);                   /* (released: MASTER again) */
        CHECK(rfmv_seq_find(m, c, 3, 60) >= 0);
        adv(m, 4 * FPS);
    }
    CHECK(stuck() == 0);
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
    test_popups();
    test_live_record();
    test_trig();
    test_controls();
    test_capture2();
    test_audio_loop();
    test_blob_v2();
    test_tracks16();
    return check_done("move_seq");
}
