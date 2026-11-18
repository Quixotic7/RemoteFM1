/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move: the Move as a RemoteFM1 panel. The FM-1 mode's mapping against the owner's design
 * (design/remotefm1move.movedesign.json), the MIDI modes' layouts against hand-computed notes, the Control mode's CC
 * banks, mode switching and release-all, settings round trips, LED diffing, and the screen's golden frame.
 * Events go in as the Move's raw USB-MIDI packets through libmoveremote's own mr_decode. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "moveremote/client.h"
#include "move_int.h"
#include "rfm/move.h"

static rfm_move_act_t acts[512];
static int nact;
static void on_act(void *ctx, const rfm_move_act_t *a)
{
    (void)ctx;
    if (nact < 512)
        acts[nact++] = *a;
}
static void clear(void) { nact = 0; }

static uint32_t now_ms;
static void raw(rfm_move_t *m, uint8_t st, uint8_t d1, uint8_t d2)
{
    const uint8_t pkt[4] = { (uint8_t)(st >> 4), st, d1, d2 };
    mr_event_t ev;
    mr_decode(pkt, 0, &ev);
    rfm_move_input(m, &ev, now_ms += 5);
}
static void pad(rfm_move_t *m, int p, int vel) { raw(m, vel ? 0x90 : 0x80, (uint8_t)(MR_NOTE_PAD0 + p), (uint8_t)vel); }
static void pad_at(rfm_move_t *m, int p, int v) { raw(m, 0xA0, (uint8_t)(MR_NOTE_PAD0 + p), (uint8_t)v); }
static void step(rfm_move_t *m, int s, int down) { raw(m, down ? 0x90 : 0x80, (uint8_t)(MR_NOTE_STEP0 + s), down ? 127 : 0); }
static void button(rfm_move_t *m, int cc, int down) { raw(m, 0xB0, (uint8_t)cc, down ? 127 : 0); }
static void knob(rfm_move_t *m, int k, int d) { raw(m, 0xB0, (uint8_t)(MR_CC_KNOB1 + k), (uint8_t)(d & 127)); }
static void jog(rfm_move_t *m, int d) { raw(m, 0xB0, MR_CC_JOG, (uint8_t)(d & 127)); }
static void touch(rfm_move_t *m, int k, int on) { raw(m, on ? 0x90 : 0x80, (uint8_t)k, on ? 127 : 0); }
static void track(rfm_move_t *m, int t, int down) { button(m, MR_CC_TRACK1 - t, down); }   /* t 0..3 */

static int has(uint8_t kind, int index, int down, int value)
{
    int i;
    for (i = 0; i < nact; i++)
        if (acts[i].kind == kind && acts[i].index == index && acts[i].down == down && acts[i].value == value)
            return 1;
    return 0;
}
static int has_midi(int s, int d1, int d2)
{
    int i;
    for (i = 0; i < nact; i++)
        if (acts[i].kind == RFM_MOVE_ACT_MIDI && acts[i].n == 3 && acts[i].bytes[0] == s && acts[i].bytes[1] == d1 &&
            acts[i].bytes[2] == d2)
            return 1;
    return 0;
}
static int count(uint8_t kind)
{
    int i, n = 0;
    for (i = 0; i < nact; i++)
        n += acts[i].kind == kind;
    return n;
}

/* ---- the design's labels (pad-<row>-<col>, row 1 the top), and the FM-1's keys' names (key 0 = F3) */
static const char *const DESIGN[4][8] = {
    { "", "F#", "G#", "A#", "", "C#", "D#", "" },
    { "F", "G", "A", "B", "C", "D", "E", "F" },
    { "F#", "G#", "A#", "", "C#", "D#", "", "F#" },
    { "G", "A", "B", "C", "D", "E", "F", "G" },
};
static const char *const NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *key_name(int k) { return NAMES[(53 + k) % 12]; }
static int key_black(int k) { return (0x54A >> ((k + 5) % 12)) & 1; }
static int pad_of(int row, int col) { return (4 - row) * 8 + (col - 1); }

static void test_fm1_pads(void)
{
    rfm_move_t m;
    int row, col, k, seen[27] = { 0 }, nkeys = 0;
    for (row = 1; row <= 4; row++)
        for (col = 1; col <= 8; col++) {
            const int p = pad_of(row, col), key = rfm_move_pad_key(p);
            if (!DESIGN[row - 1][col - 1][0]) {
                CHECKF(key < 0, "pad-%d-%d is unused in the design, plays %d", row, col, key);
                continue;
            }
            CHECKF(key >= 0 && key < 27 && !strcmp(key_name(key), DESIGN[row - 1][col - 1]),
                   "pad-%d-%d: key %d (%s), the design says %s", row, col, key, key >= 0 ? key_name(key) : "-",
                   DESIGN[row - 1][col - 1]);
            if (key >= 0 && key < 27)
                seen[key]++, nkeys++;
        }
    CHECK(nkeys == 27);
    for (k = 0; k < 27; k++)
        CHECKF(seen[k] == 1, "key %d on %d pads", k, seen[k]);
    /* the low octave is rows 1-2 (F3..F4), the high one rows 3-4 (F#4..G5); whites ascend along their rows */
    for (col = 1; col <= 8; col++) {
        CHECK(rfm_move_pad_key(pad_of(2, col)) <= 12 && !key_black(rfm_move_pad_key(pad_of(2, col))));
        CHECK(rfm_move_pad_key(pad_of(4, col)) >= 14 && !key_black(rfm_move_pad_key(pad_of(4, col))));
        if (col > 1) {
            CHECK(rfm_move_pad_key(pad_of(2, col)) > rfm_move_pad_key(pad_of(2, col - 1)));
            CHECK(rfm_move_pad_key(pad_of(4, col)) > rfm_move_pad_key(pad_of(4, col - 1)));
        }
    }
    CHECK(rfm_move_pad_key(pad_of(2, 1)) == 0 && rfm_move_pad_key(pad_of(4, 8)) == 26);
    CHECK(rfm_move_pad_key(pad_of(3, 1)) == 13 && rfm_move_pad_key(pad_of(3, 8)) == 25);

    /* through the input: velocity, pressure, release */
    rfm_move_init(&m, NULL, on_act, NULL);
    for (row = 1; row <= 4; row++)
        for (col = 1; col <= 8; col++) {
            const int p = pad_of(row, col), key = rfm_move_pad_key(p);
            clear();
            pad(&m, p, 90);
            if (key < 0) {
                CHECK(nact == 0);
                pad(&m, p, 0);
                CHECK(nact == 0);
                continue;
            }
            CHECK(nact == 1 && acts[0].kind == RFM_MOVE_ACT_KEY && acts[0].index == key && acts[0].down == 1 &&
                  acts[0].vel == 90);
            clear();
            pad_at(&m, p, 64);
            CHECK(nact == 1 && has(RFM_MOVE_ACT_PRESS, key, 0, 64));
            pad_at(&m, p, 64);                          /* the same pressure: nothing */
            CHECK(nact == 1);
            clear();
            pad(&m, p, 0);
            CHECK(nact == 2 && acts[0].kind == RFM_MOVE_ACT_PRESS && acts[0].value == 0 &&
                  acts[1].kind == RFM_MOVE_ACT_KEY && acts[1].index == key && acts[1].down == 0);
        }
    CHECK(rfm_move_lean(127) == 100 && rfm_move_lean(64) == 50 && rfm_move_lean(0) == 0);
}

static void test_steps(void)
{
    rfm_move_t m;
    int s;
    rfm_move_init(&m, NULL, on_act, NULL);
    for (s = 0; s < 16; s++) {
        const int k = rfm_move_step_key(s);
        CHECKF(k >= 0 && !key_black(k), "step %d: key %d", s + 1, k);
        if (s)
            CHECK(k > rfm_move_step_key(s - 1));
        clear();
        step(&m, s, 1);
        CHECK(nact == 1 && has(RFM_MOVE_ACT_KEY, k, 1, 0) && acts[0].vel == 100);
        clear();
        step(&m, s, 0);
        CHECK(nact == 1 && has(RFM_MOVE_ACT_KEY, k, 0, 0));
    }
    CHECK(rfm_move_step_key(0) == 0 && rfm_move_step_key(15) == 26);
    /* a white key held by a pad and a step: down with the first, up with the last */
    clear();
    pad(&m, pad_of(2, 1), 80);
    step(&m, 0, 1);
    pad(&m, pad_of(2, 1), 0);
    CHECK(nact == 1);
    step(&m, 0, 0);
    CHECK(nact == 2 && has(RFM_MOVE_ACT_KEY, 0, 0, 0));
}

static void test_buttons_knobs(void)
{
    static const struct { int cc, b; } B[] = {
        { MR_CC_CAPTURE, RFM_MOVE_B_FX }, { MR_CC_SAMPLE, RFM_MOVE_B_SEL }, { MR_CC_MUTE, RFM_MOVE_B_ENV },
        { MR_CC_DELETE, RFM_MOVE_B_LFO }, { MR_CC_COPY, RFM_MOVE_B_EDIT }, { MR_CC_SHIFT, RFM_MOVE_B_GLO },
        { MR_CC_MENU, RFM_MOVE_B_HOME }, { MR_CC_UNDO, RFM_MOVE_B_SAVE }, { MR_CC_LOOP, RFM_MOVE_B_ARP },
        { MR_CC_BACK, RFM_MOVE_B_SEQ }, { MR_CC_PLAY, RFM_MOVE_B_PLAY }, { MR_CC_REC, RFM_MOVE_B_REC },
        { MR_CC_LEFT, RFM_MOVE_B_OCTDN }, { MR_CC_RIGHT, RFM_MOVE_B_OCTUP },
    };
    static const int ENC[8] = { RFM_MOVE_E_PRESETS, RFM_MOVE_E_ALGO, -1, -1,
                                RFM_MOVE_E_K1, RFM_MOVE_E_K2, RFM_MOVE_E_K3, RFM_MOVE_E_K4 };
    rfm_move_t m;
    int i, mode, covered = 0;
    rfm_move_init(&m, NULL, on_act, NULL);
    /* the buttons: the same in modes 1..3 (only pads, steps, Up / Down and the knobs change); Sequencer mode takes
     * them over (test_move_seq.c) */
    for (mode = 0; mode < 3; mode++) {
        rfm_move_set_mode(&m, mode);
        for (i = 0; i < 14; i++) {
            clear();
            button(&m, B[i].cc, 1);
            CHECKF(nact == 1 && has(RFM_MOVE_ACT_BTN, B[i].b, 1, 0), "mode %d cc %d", mode, B[i].cc);
            button(&m, B[i].cc, 1);                     /* (a repeat: nothing) */
            CHECK(nact == 1);
            button(&m, B[i].cc, 0);
            CHECK(nact == 2 && has(RFM_MOVE_ACT_BTN, B[i].b, 0, 0));
            if (!mode)
                covered |= 1 << B[i].b;
        }
    }
    CHECK(covered == (1 << 14) - 1);
    CHECK(rfm_move_button_of(MR_CC_UP) < 0 && rfm_move_button_of(MR_CC_DOWN) < 0 &&
          rfm_move_button_of(MR_CC_TRACK1) < 0);
    rfm_move_set_mode(&m, 0);
    clear();
    button(&m, MR_CC_UP, 1), button(&m, MR_CC_UP, 0);   /* FM-1 mode: Up / Down do nothing */
    button(&m, MR_CC_DOWN, 1), button(&m, MR_CC_DOWN, 0);
    CHECK(nact == 0 && m.s.octave == 0);

    /* the knobs, the jog wheel, the volume knob */
    for (i = 0; i < 8; i++) {
        CHECK(rfm_move_knob_enc(i) == ENC[i]);
        clear();
        knob(&m, i, 6);                                 /* (the owner's: an FM-1 detent every 2 of the Move's ticks) */
        knob(&m, i, -4);
        if (ENC[i] < 0)
            CHECK(nact == 0);
        else
            CHECK(nact == 2 && has(RFM_MOVE_ACT_TURN, ENC[i], 0, 3) && has(RFM_MOVE_ACT_TURN, ENC[i], 0, -2));
    }
    clear();
    knob(&m, 4, 1);                                     /* a tick: nothing yet; the next: a detent */
    CHECK(nact == 0);
    knob(&m, 4, 1);
    CHECK(nact == 1 && has(RFM_MOVE_ACT_TURN, ENC[4], 0, 1));
    knob(&m, 4, 1), knob(&m, 4, -1), knob(&m, 4, -1);   /* (a turn back forgets the half: -1 needs two) */
    CHECK(nact == 2 && has(RFM_MOVE_ACT_TURN, ENC[4], 0, -1));
    knob(&m, 4, 3);                                     /* (3 ticks: 1 detent, 1 left over) */
    touch(&m, 4, 1), touch(&m, 4, 0);                   /* (the release forgets it) */
    clear();
    knob(&m, 4, 1);
    CHECK(nact == 0);
    knob(&m, 4, -1);
    clear();
    clear();
    jog(&m, -1);
    jog(&m, 5);
    CHECK(nact == 2 && has(RFM_MOVE_ACT_TURN, RFM_MOVE_E_SELECT, 0, -1) && has(RFM_MOVE_ACT_TURN, RFM_MOVE_E_SELECT, 0, 5));
    clear();
    raw(&m, 0xB0, MR_CC_JOG_CLICK, 127), raw(&m, 0xB0, MR_CC_JOG_CLICK, 0);   /* no SELECT push on an FM-1 */
    CHECK(nact == 0);
    /* MASTER: 16 a detent from the host's value, clamped */
    rfm_move_set_master(&m, 500);
    clear();
    knob(&m, 8, 2);
    CHECK(nact == 1 && has(RFM_MOVE_ACT_MASTER, 0, 0, 532));
    knob(&m, 8, 63);
    knob(&m, 8, 63);
    CHECK(acts[nact - 1].value == 1023 && m.master == 1023);
    clear();
    knob(&m, 8, 1);                                     /* at the top: no action */
    CHECK(nact == 0);
    knob(&m, 8, -63), knob(&m, 8, -63);
    CHECK(acts[nact - 1].kind == RFM_MOVE_ACT_MASTER && acts[nact - 1].value == 0);
    clear();
    knob(&m, 8, -1);
    CHECK(nact == 0);
    /* knob touch: remembered for the screen */
    {
        const int was = m.pop;
        touch(&m, 4, 1);
        CHECK(m.touched == 4 && m.pop == was);          /* (FM-1 mode: the drawing marks it; a turn brings a popup) */
    }
    touch(&m, 4, 0);
    CHECK(m.touched == -1);
}

static void test_modes(void)
{
    rfm_move_t m;
    rfm_move_init(&m, NULL, on_act, NULL);
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1);
    pad(&m, pad_of(2, 1), 100);
    pad_at(&m, pad_of(2, 1), 40);
    step(&m, 4, 1);
    button(&m, MR_CC_SHIFT, 1);
    clear();
    track(&m, 1, 1);                                    /* Track 2 down: no switch (the owner's: only a tap) */
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1 && (m.track_held & 2) && nact == 0);
    track(&m, 1, 0);                                    /* .. its tap: MIDI mode, everything let go */
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI && m.track_held == 0);
    CHECK(has(RFM_MOVE_ACT_KEY, 0, 0, 0) && has(RFM_MOVE_ACT_PRESS, 0, 0, 0) &&
          has(RFM_MOVE_ACT_KEY, rfm_move_step_key(4), 0, 0) && has(RFM_MOVE_ACT_BTN, RFM_MOVE_B_GLO, 0, 0));
    CHECK(count(RFM_MOVE_ACT_SETTINGS) == 1);
    clear();
    pad(&m, pad_of(2, 1), 0), step(&m, 4, 0), button(&m, MR_CC_SHIFT, 0);   /* the old presses' ends: nothing */
    CHECK(nact == 0);
    clear();
    track(&m, 1, 1), track(&m, 1, 0);                   /* the same mode, a tap: its next screen (NOTES), named */
    CHECK(count(RFM_MOVE_ACT_SETTINGS) == 1 && m.s.midi_view == RFM_MOVE_MV_NOTES && m.pop == POP_MODE && m.pop_i == 11);
    track(&m, 1, 1), track(&m, 1, 0);
    CHECK(m.s.midi_view == RFM_MOVE_MV_PADS);
    /* release-all: notes and buttons */
    pad(&m, 0, 100);
    button(&m, MR_CC_PLAY, 1);
    clear();
    rfm_move_release_all(&m);
    CHECK(nact == 2 && has_midi(0x80, 48, 0) && has(RFM_MOVE_ACT_BTN, RFM_MOVE_B_PLAY, 0, 0));
    clear();
    rfm_move_release_all(&m);
    CHECK(nact == 0);
    track(&m, 2, 1), track(&m, 2, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_CONTROL);
    track(&m, 3, 1), track(&m, 3, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_SEQ);
    clear();
    pad(&m, 0, 100);                                    /* the sequencer's placeholder: the MIDI mode's pads */
    CHECK(has_midi(0x90, 48, 100));
    track(&m, 0, 1), track(&m, 0, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1 && has_midi(0x80, 48, 0));
}

static void test_layouts(void)
{
    rfm_move_settings_t s;
    rfm_move_t m;
    int p;
    rfm_move_settings_default(&s);
    /* 1 isomorphic in scale (C major): +1 degree a column, +3 degrees (a 4th) a row */
    s.layout = RFM_MOVE_LAYOUT_SCALE;
    CHECK(rfm_move_pad_note(&s, 0) == 48 && rfm_move_pad_note(&s, 1) == 50 && rfm_move_pad_note(&s, 2) == 52);
    CHECK(rfm_move_pad_note(&s, 3) == 53 && rfm_move_pad_note(&s, 7) == 60);
    CHECK(rfm_move_pad_note(&s, 8) == 53 && rfm_move_pad_note(&s, 16) == 59 && rfm_move_pad_note(&s, 24) == 64);
    CHECK(rfm_move_pad_note(&s, 31) == 76);
    s.scale = RFM_MOVE_SCALE_MINOR, s.root = 9;         /* A minor */
    CHECK(rfm_move_pad_note(&s, 0) == 57 && rfm_move_pad_note(&s, 2) == 60 && rfm_move_pad_note(&s, 8) == 62);
    s.scale = RFM_MOVE_SCALE_MAJOR, s.root = 0;
    /* 2 chromatic: +1 a column, +5 a row */
    s.layout = RFM_MOVE_LAYOUT_CHROMATIC;
    CHECK(rfm_move_pad_note(&s, 0) == 48 && rfm_move_pad_note(&s, 7) == 55 && rfm_move_pad_note(&s, 8) == 53);
    CHECK(rfm_move_pad_note(&s, 31) == 70);
    /* 3 drum: C2 at the bottom right, leftwards, then the next row from the right */
    s.layout = RFM_MOVE_LAYOUT_DRUM;
    CHECK(rfm_move_pad_note(&s, 7) == 36 && rfm_move_pad_note(&s, 6) == 37 && rfm_move_pad_note(&s, 0) == 43);
    CHECK(rfm_move_pad_note(&s, 15) == 44 && rfm_move_pad_note(&s, 8) == 51 && rfm_move_pad_note(&s, 31) == 60);
    CHECK(rfm_move_pad_note(&s, 24) == 67);
    /* 4 octave: a row an octave, 7 degrees and the octave */
    s.layout = RFM_MOVE_LAYOUT_OCTAVE;
    CHECK(rfm_move_pad_note(&s, 0) == 48 && rfm_move_pad_note(&s, 4) == 55 && rfm_move_pad_note(&s, 6) == 59);
    CHECK(rfm_move_pad_note(&s, 7) == 60 && rfm_move_pad_note(&s, 8) == 60 && rfm_move_pad_note(&s, 31) == 96);
    /* the octave and the edges */
    s.octave = 1;
    CHECK(rfm_move_pad_note(&s, 0) == 60);
    s.octave = 4, s.layout = RFM_MOVE_LAYOUT_OCTAVE;
    CHECK(rfm_move_pad_note(&s, 0) == 96 && rfm_move_pad_note(&s, 31) == -1);
    s.octave = -4, s.layout = RFM_MOVE_LAYOUT_DRUM;
    CHECK(rfm_move_pad_note(&s, 7) == -1 && rfm_move_pad_note(&s, 24) == 19);

    /* through the input: Track 2 + top pads = the layout, Track 2 + steps = the channel, Up / Down the octave */
    rfm_move_init(&m, NULL, on_act, NULL);
    track(&m, 1, 1), track(&m, 1, 0);                   /* (a tap: MIDI mode) */
    track(&m, 1, 1);
    clear();
    pad(&m, pad_of(1, 2), 100), pad(&m, pad_of(1, 2), 0);
    CHECK(m.s.layout == RFM_MOVE_LAYOUT_CHROMATIC && count(RFM_MOVE_ACT_MIDI) == 0 && count(RFM_MOVE_ACT_SETTINGS) == 1);
    step(&m, 9, 1), step(&m, 9, 0);
    CHECK(m.s.channel == 9 && count(RFM_MOVE_ACT_KEY) == 0);
    pad(&m, pad_of(2, 6), 100), pad(&m, pad_of(2, 6), 0);   /* another pad while held: nothing */
    CHECK(count(RFM_MOVE_ACT_MIDI) == 0);
    track(&m, 1, 0);
    clear();
    pad(&m, 5, 70);                                     /* chromatic: pad 5 and pad 8 are both F3 (53) */
    pad(&m, 8, 90);
    CHECK(nact == 1 && has_midi(0x99, 53, 70));
    pad_at(&m, 8, 33);
    CHECK(has_midi(0xA9, 53, 33));
    pad(&m, 5, 0);
    CHECK(!has_midi(0x89, 53, 0));
    pad(&m, 8, 0);
    CHECK(has_midi(0x89, 53, 0));
    clear();
    button(&m, MR_CC_UP, 1), button(&m, MR_CC_UP, 0);
    CHECK(m.s.octave == 1 && count(RFM_MOVE_ACT_SETTINGS) == 1);
    for (p = 0; p < 10; p++)
        button(&m, MR_CC_DOWN, 1), button(&m, MR_CC_DOWN, 0);
    CHECK(m.s.octave == RFM_MOVE_OCT_MIN);
    for (p = 0; p < 10; p++)
        button(&m, MR_CC_UP, 1), button(&m, MR_CC_UP, 0);
    CHECK(m.s.octave == RFM_MOVE_OCT_MAX);
    /* a note held over an octave change ends where it began */
    m.s.octave = 0;
    clear();
    pad(&m, 0, 100);
    button(&m, MR_CC_UP, 1);
    pad(&m, 0, 0);
    CHECK(has_midi(0x99, 48, 100) && has_midi(0x89, 48, 0));
    /* in MIDI mode the knobs are still the FM-1's */
    clear();
    knob(&m, 0, 2);
    CHECK(has(RFM_MOVE_ACT_TURN, RFM_MOVE_E_PRESETS, 0, 1));
}

static void test_drum_settings(void)
{
    rfm_move_t m;
    rfm_move_settings_t s, t;
    uint8_t img[RFM_MOVE_SETTINGS_BYTES], bad[RFM_MOVE_SETTINGS_BYTES];
    rfm_move_init(&m, NULL, on_act, NULL);
    track(&m, 1, 1), track(&m, 1, 0);                   /* (a tap: MIDI mode) */
    track(&m, 1, 1);
    pad(&m, pad_of(1, 3), 100), pad(&m, pad_of(1, 3), 0);   /* layout 3: drums */
    track(&m, 1, 0);
    CHECK(m.s.layout == RFM_MOVE_LAYOUT_DRUM);
    clear();
    pad(&m, 7, 100);                                    /* bottom right: C2 */
    CHECK(has_midi(0x90, 36, 100));
    jog(&m, 2);                                         /* held + jog: its note */
    CHECK(m.s.drum[7] == 38 && count(RFM_MOVE_ACT_TURN) == 0 && count(RFM_MOVE_ACT_SETTINGS) == 1);
    jog(&m, -1);
    CHECK(m.s.drum[7] == 37);
    pad(&m, 7, 0);
    CHECK(has_midi(0x80, 36, 0));                       /* the note that sounded ends */
    clear();
    jog(&m, 1);                                         /* no pad held: SELECT */
    CHECK(has(RFM_MOVE_ACT_TURN, RFM_MOVE_E_SELECT, 0, 1));
    clear();
    pad(&m, 7, 90);
    CHECK(has_midi(0x90, 37, 90));
    pad(&m, 7, 0);
    /* the settings: a byte image, back again */
    m.s.channel = 5, m.s.octave = -2, m.s.scale = RFM_MOVE_SCALE_DORIAN, m.s.root = 7, m.s.bank = 2;
    m.s.cc[3][7] = 1;
    CHECK(rfm_move_save(&m.s, img) == RFM_MOVE_SETTINGS_BYTES);
    CHECK(rfm_move_load(&s, img, sizeof img) == 0 && !memcmp(&s, &m.s, sizeof s));
    rfm_move_init(&m, &s, on_act, NULL);                /* a new session from the saved settings */
    CHECK(m.s.drum[7] == 37 && m.s.mode == RFM_MOVE_MODE_MIDI && m.s.layout == RFM_MOVE_LAYOUT_DRUM);
    clear();
    pad(&m, 7, 90);
    CHECK(has_midi(0x95, 37 - 24, 90));                 /* channel 6, octave -2 */
    /* not an image: the defaults */
    memcpy(bad, img, sizeof bad);
    bad[3] = 99;
    rfm_move_settings_default(&t);
    CHECK(rfm_move_load(&s, bad, sizeof bad) == -1 && !memcmp(&s, &t, sizeof s));
    CHECK(rfm_move_load(&s, img, sizeof img - 1) == -1);
    CHECK(rfm_move_load(&s, NULL, 0) == -1);
    /* out-of-range fields: their defaults, the rest kept */
    memcpy(bad, img, sizeof bad);
    bad[6] = 9, bad[8] = 16, bad[9] = (uint8_t)(int8_t)-7, bad[13 + 2] = 200;
    CHECK(rfm_move_load(&s, bad, sizeof bad) == 0);
    CHECK(s.mode == 0 && s.channel == 0 && s.octave == 0 && s.cc[0][2] == 18 && s.root == 7 && s.drum[7] == 37);
}

static void test_control(void)
{
    rfm_move_t m;
    int b, k;
    rfm_move_settings_t d;
    rfm_move_settings_default(&d);
    for (k = 0; k < 8; k++)
        CHECK(d.cc[0][k] == 16 + k && d.cc[1][k] == 24 + k && d.cc[2][k] == 102 + k && d.cc[3][k] == 110 + k);
    rfm_move_init(&m, NULL, on_act, NULL);
    track(&m, 2, 1), track(&m, 2, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_CONTROL);
    clear();
    knob(&m, 0, 3);
    CHECK(nact == 1 && has_midi(0xB0, 16, 67));
    knob(&m, 7, -58);                                   /* a big turn: 64 - 58 */
    CHECK(has_midi(0xB0, 23, 6));
    knob(&m, 7, -10);
    CHECK(has_midi(0xB0, 23, 0));
    clear();
    knob(&m, 7, -1);                                    /* at 0: nothing sent */
    CHECK(nact == 0);
    /* the jog wheel: the bank (clamped 1..4) */
    for (b = 1; b < 4; b++) {
        clear();
        jog(&m, 1);
        CHECK(m.s.bank == b && count(RFM_MOVE_ACT_TURN) == 0);
        knob(&m, 2, 1);
        CHECK(has_midi(0xB0, d.cc[b][2], 65));
    }
    jog(&m, 1);
    CHECK(m.s.bank == 3);
    jog(&m, -9);
    CHECK(m.s.bank == 0);
    /* Track 3 held + a knob: its CC number, nothing sent */
    track(&m, 2, 1);
    clear();
    knob(&m, 1, 5);
    CHECK(m.s.cc[0][1] == 22 && count(RFM_MOVE_ACT_MIDI) == 0 && count(RFM_MOVE_ACT_SETTINGS) == 1);
    track(&m, 2, 0);
    clear();
    knob(&m, 1, 1);
    CHECK(has_midi(0xB0, 22, 65));
    /* the channel follows MIDI mode's; the volume knob is still MASTER; the pads still play notes */
    m.s.channel = 3;
    clear();
    knob(&m, 1, 1);
    knob(&m, 8, 1);
    pad(&m, 0, 50);
    CHECK(has_midi(0xB3, 22, 66) && has(RFM_MOVE_ACT_MASTER, 0, 0, 784) && has_midi(0x93, 48, 50));
}

/* ---- LEDs */
static void test_leds(void)
{
    rfm_move_t m;
    rfm_move_led_t out[RFM_MOVE_NLEDS + 4];
    uint8_t lv[RFM_MOVE_NLEVELS];
    int n, i, j, dup = 0;
    rfm_move_init(&m, NULL, on_act, NULL);
    memset(lv, 0, sizeof lv);
    rfm_move_panel(&m, lv, 0);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS + 4);
    CHECK(n == RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            dup += out[i].cc == out[j].cc && out[i].number == out[j].number;
    CHECK(dup == 0);
    for (i = 0; i < n; i++)                             /* Track 1 in the FM-1 mode's colour */
        if (out[i].cc && out[i].number == MR_CC_TRACK1)
            CHECK(out[i].rgb && out[i].r == 0xc4 && out[i].g == 0x3a && out[i].b == 0xff);
    CHECK(rfm_move_leds(&m, out, RFM_MOVE_NLEDS) == 0);
    /* key 0's LED (F3: the keymap's id 14 at row 4 column 3 -> levels[3 * 4 + 3]) lights pad-2-1 and step 1 */
    lv[3 * 4 + 3] = 255;
    rfm_move_panel(&m, lv, 0);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS);
    CHECK(n == 2);
    CHECK(n == 2 && !out[0].cc && out[0].number == MR_NOTE_PAD0 + pad_of(2, 1) && out[0].r == 255 && out[0].g == 255);
    CHECK(n == 2 && !out[1].cc && out[1].number == MR_NOTE_STEP0 + 0 && out[1].r == 255);
    rfm_move_panel(&m, lv, 0);                          /* the same again: nothing */
    CHECK(rfm_move_leds(&m, out, RFM_MOVE_NLEDS) == 0);
    /* a black key lights pink: key 1 (F#3, id 15 at row 4 column 2) */
    lv[2 * 4 + 3] = 255;
    rfm_move_panel(&m, lv, 0);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS);
    CHECK(n == 1 && out[0].number == MR_NOTE_PAD0 + pad_of(1, 2) && out[0].r == 0xff && out[0].g == 0x7a && out[0].b == 0xa8);
    /* PLAY's green (and PLAY id 12 at row 2 column 10) */
    rfm_move_panel(&m, lv, 255);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS);
    CHECK(n == 1 && out[0].cc && out[0].number == MR_CC_PLAY && out[0].r == 0 && out[0].g == 255 && out[0].b == 0);
    /* SEQ's LED (id 11 at row 1 column 1) on Back, a white LED */
    lv[1 * 4 + 0] = 255;
    rfm_move_panel(&m, lv, 255);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS);
    CHECK(n == 1 && !out[0].rgb && out[0].cc && out[0].number == MR_CC_BACK && out[0].r == 127);
    /* a mode change repaints what changes, once; at most max a call, the rest next time */
    track(&m, 1, 1), track(&m, 1, 0);
    n = rfm_move_leds(&m, out, 5);
    CHECK(n == 5);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS);
    CHECK(n > 0);
    CHECK(rfm_move_leds(&m, out, RFM_MOVE_NLEDS) == 0);
    /* a pressed pad lights, and only it */
    pad(&m, 3, 100);
    n = rfm_move_leds(&m, out, RFM_MOVE_NLEDS);
    CHECK(n == 1 && out[0].number == MR_NOTE_PAD0 + 3 && out[0].r == 255 && out[0].g == 255 && out[0].b == 255);
    rfm_move_leds_invalidate(&m);
    CHECK(rfm_move_leds(&m, out, RFM_MOVE_NLEDS + 4) == RFM_MOVE_NLEDS);
}

/* ---- the screen */
static uint32_t fnv(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
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

/* golden frames (FNV-1a of the 1024 packed bytes); a change in the drawing is a change here, made on purpose:
 * look at the PBMs this test writes into the build directory first (move_*.pbm) */
#define GOLD_FM1 0xca84fa11u
#define GOLD_FM1_LIVE 0x714e834du
#define GOLD_KNOB 0x5eab3fb5u
#define GOLD_MASTER 0x08fb6a96u
#define GOLD_MIDI 0x3de84831u
#define GOLD_MIDI_HIT 0x570d8447u
#define GOLD_CONTROL 0xdaa9dc11u
#define GOLD_CC 0xcd509862u
#define GOLD_MENU_MIDI 0x27622f6eu
#define GOLD_MENU_CC 0x35fff5ddu

static int px_at(const uint8_t *packed, int x, int y) { return packed[(y >> 3) * 128 + x] >> (y & 7) & 1; }
static uint32_t gold(const char *name, const uint8_t *out, uint32_t want)
{
    char path[64];
    const uint32_t h = fnv(out, MR_SCREEN_BYTES);
    snprintf(path, sizeof path, "move_%s.pbm", name);
    write_pbm(path, out);
    printf("screen %s: %08x\n", name, h);
    CHECKF(want == 0 || h == want, "%s %08x, golden %08x", name, h, want);
    return h;
}
static void ms(rfm_move_t *m, uint32_t n) { rfm_move_seq_advance(m, n * 1000u, 1000000u); }   /* the popups' clock */

/* the owner's FM-1 drawing, read from the PNG the owner drew (64 x 64, white = on) independently of screen.c's table:
 * rows 15..51; the rest is black */
static const char *const ART[37] = {
    "....########################################################....",   /* 15 */
    "...#........................................................#...",   /* 16 */
    "..#..........................................................#..",   /* 17 */
    "..#...............##############.............................#..",   /* 18 */
    "..#...............##############.............................#..",   /* 19 */
    "..#...##....##....##..........##...##.....##....##.....##....#..",   /* 20 */
    "..#...##....##....##..........##...##.....##....##.....##....#..",   /* 21 */
    "..#...............##...###....##.............................#..",   /* 22 */
    "..#...............##..#####...##.............................#..",   /* 23 */
    "..#...............##.##..##...##.............................#..",   /* 24 */
    "..#...............##.##...##..##..#######################....#..",   /* 25 */
    "..#...##....##....####....##..##.#...#...#...#...#...#...#...#..",   /* 26 */
    "..#...##....##....###......#####.#...#...#...#...#...#...#...#..",   /* 27 */
    "..#...............##........####.#...#...#...#...#...#...#...#..",   /* 28 */
    "..#...............##..........##.#########################...#..",   /* 29 */
    "..#...............##############.#...#...#...#...#...#...#...#..",   /* 30 */
    "..#...#########...##############.#...#...#...#...#...#...#...#..",   /* 31 */
    "..#...#...#...#..................#...#...#...#...#...#...#...#..",   /* 32 */
    "..#...#########...................#######################....#..",   /* 33 */
    "..#..........................................................#..",   /* 34 */
    "..#..........................................................#..",   /* 35 */
    "..#....#################################################.....#..",   /* 36 */
    "..#...#.................................................#....#..",   /* 37 */
    "..#...#...##.##.##.##.##.##.##.##.##.##.##.##.##.##.##..#....#..",   /* 38 */
    "..#...#...##.##.##.##.##.##.##.##.##.##.##.##.##.##.##..#....#..",   /* 39 */
    "..#...#...##.##.##.##.##.##.##.##.##.##.##.##.##.##.##..#....#..",   /* 40 */
    "..#...#...##.##.##.##.##.##.##.##.##.##.##.##.##.##.##..#....#..",   /* 41 */
    "..#...#.................................................#....#..",   /* 42 */
    "..#...#.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.#....#..",   /* 43 */
    "..#...#.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.#....#..",   /* 44 */
    "..#...#.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.#....#..",   /* 45 */
    "..#...#.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.##.#....#..",   /* 46 */
    "..#...#.................................................#....#..",   /* 47 */
    "..#....#################################################.....#..",   /* 48 */
    "..#..........................................................#..",   /* 49 */
    "...#........................................................#...",   /* 50 */
    "....########################################################....",   /* 51 */
};
static int art_px(int x, int y) { return y >= 15 && y < 52 && ART[y - 15][x] == '#'; }
/* the left half against the drawing: the number of pixels that differ */
static int art_diff(const uint8_t *out)
{
    int x, y, d = 0;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, x, y) != art_px(x, y);
    return d;
}

static void test_screen(void)
{
    static uint16_t lcd[240 * 240];
    static rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES], out2[MR_SCREEN_BYTES], lv[RFM_MOVE_NLEVELS];
    int x, y, d;
    /* a test card: a horizontal grey ramp, a white box, a black disc */
    for (y = 0; y < 240; y++)
        for (x = 0; x < 240; x++) {
            const int g = x * 31 / 239, dx = x - 160, dy = y - 160;
            uint16_t v = (uint16_t)(g << 11 | (g * 2) << 5 | g);
            if (x >= 20 && x < 100 && y >= 20 && y < 100)
                v = 0xFFFF;
            if (dx * dx + dy * dy < 40 * 40)
                v = 0;
            lcd[y * 240 + x] = v;
        }
    rfm_move_init(&m, NULL, on_act, NULL);
    rfm_move_set_label(&m, RFM_MOVE_E_K1, "CUTOFF");
    /* at rest: the left half is the owner's drawing, pixel for pixel; the right half the FM-1's screen */
    rfm_move_screen(&m, lcd, out);
    CHECKF(art_diff(out) == 0, "FM-1 mode at rest: the drawing exactly (%d pixels differ)", art_diff(out));
    CHECK(out[(1 * 128) + 64 + 15] == 0xFF);            /* x 79, y 8..15: inside the box */
    CHECK(out[(5 * 128) + 64 + 43] == 0x00);            /* x 107, y 40..47: inside the disc */
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, 64 + x, y) != m.thumb[y][x];
    CHECK(d == 0);
    gold("fm1", out, GOLD_FM1);
    rfm_move_screen(&m, NULL, out2);                    /* NULL: the last screen kept */
    CHECK(!memcmp(out, out2, sizeof out));
    /* LEDs dim (under RFM_MOVE_SCREEN_LIT) change nothing */
    memset(lv, RFM_MOVE_SCREEN_LIT - 1, sizeof lv);
    rfm_move_panel(&m, lv, 0);
    rfm_move_screen(&m, NULL, out);
    CHECK(art_diff(out) == 0);

    /* live: a touched encoder's dot inverted in a 6 x 6 block (K1: knob 5, its dot at 35, 20) */
    touch(&m, 4, 1);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 33, 18) && px_at(out, 38, 23) && !px_at(out, 35, 20) && !px_at(out, 36, 21));
    CHECK(art_diff(out) == 6 * 6);                      /* (32 lit around it, the 4 of the dot dark) */
    CHECK(m.pop == POP_NONE);                           /* a touch alone: no popup in FM-1 mode */
    touch(&m, 4, 0);
    touch(&m, 8, 1);                                    /* the volume knob: MASTER, top left */
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 4, 18) && !px_at(out, 6, 20) && px_at(out, 9, 23));
    touch(&m, 8, 0);
    touch(&m, 0, 1);                                    /* knob 1: PRESETS, bottom left */
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 4, 24) && !px_at(out, 6, 26));
    touch(&m, 0, 0);
    jog(&m, 1), jog(&m, 2);                             /* the jog wheel: SELECT's popup on the left half while turning
                                                         * (its detents), the mirror stays */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.pop == POP_SELECT && m.pop_val == 3 && px_at(out, 0, 0) && px_at(out, 63, 63) && px_at(out, 64 + 15, 8));
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(art_diff(out) == 0);                          /* timed out: the drawing at rest */
    jog(&m, -1);                                        /* (a new gesture: from 0) */
    CHECK(m.pop_val == -1);
    ms(&m, RFM_MOVE_POPUP_MS);
    /* buttons: lit (the FM-1's LED from RFM_MOVE_SCREEN_LIT) or held, their cells filled; FX lit, REC held (Rec),
     * OCT+ held (Right) */
    memset(lv, 0, sizeof lv);
    lv[m.led_of[RFM_MOVE_B_FX]] = RFM_MOVE_SCREEN_LIT;
    rfm_move_panel(&m, lv, 0);
    button(&m, MR_CC_REC, 1);
    button(&m, MR_CC_RIGHT, 1);
    /* keys: F3 (key 0, the first natural) and F#3 (key 1, the first gap's block) and G5 (key 26, the last) held */
    pad(&m, 16, 100), pad(&m, 25, 100), pad(&m, 7, 100);
    rfm_move_screen(&m, NULL, out);
    for (y = 26; y < 29; y++)
        for (x = 34; x < 37; x++)
            CHECK(px_at(out, x, y));                    /* FX: the grid's first cell */
    CHECK(px_at(out, 55, 31) && px_at(out, 54, 30) && px_at(out, 56, 32));   /* REC: the last */
    CHECK(px_at(out, 11, 32) && px_at(out, 13, 32) && !px_at(out, 8, 32));   /* OCT+ yes, OCT- no */
    CHECK(!px_at(out, 8, 43) && !px_at(out, 9, 46) && !px_at(out, 10, 38) && !px_at(out, 11, 41));
    CHECK(!px_at(out, 53, 43) && !px_at(out, 54, 46) && px_at(out, 50, 43));
    CHECK(art_diff(out) == 9 + 9 + 3 + 3 * 8);
    gold("fm1_live", out, GOLD_FM1_LIVE);
    pad(&m, 16, 0), pad(&m, 25, 0), pad(&m, 7, 0);
    button(&m, MR_CC_REC, 0), button(&m, MR_CC_RIGHT, 0);
    memset(lv, 0, sizeof lv);
    rfm_move_panel(&m, lv, 0);
    rfm_move_screen(&m, NULL, out);
    CHECK(art_diff(out) == 0);

    /* the knob popup: a turn (the firmware's label, this gesture's detents) */
    touch(&m, 4, 1);
    knob(&m, 4, 4), knob(&m, 4, 2);                     /* (6 ticks: 3 detents) */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.pop == POP_ENC && m.pop_val == 3);
    CHECK(px_at(out, 0, 0) && px_at(out, 63, 63) && !px_at(out, 1, 1));       /* the frame: the left half only */
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, 64 + x, y) != m.thumb[y][x];
    CHECK(d == 0);                                      /* (the FM-1's screen stays in view) */
    gold("knob", out, GOLD_KNOB);
    ms(&m, RFM_MOVE_POPUP_MS - 10);                     /* (the owner's: up while turning, a moment after the last
                                                         * detent; a touch alone does not hold it) */
    rfm_move_screen(&m, NULL, out2);
    CHECK(!memcmp(out, out2, sizeof out) && rfm_move_screen_timed(&m));
    ms(&m, 20);
    rfm_move_screen(&m, NULL, out2);
    CHECK(!rfmv_pop_up(&m, POP_ENC) && art_diff(out2) == 6 * 6);   /* still touched: the drawing's highlight only */
    knob(&m, 4, 2);                                     /* (turning again in the same touch: the count goes on) */
    CHECK(m.pop_val == 4);
    touch(&m, 4, 0);                                    /* the release: gone at once, the count back to 0 */
    rfm_move_screen(&m, NULL, out);
    CHECK(art_diff(out) == 0 && !rfm_move_screen_timed(&m) && m.pop_val == 0 && m.pop == POP_NONE);
    touch(&m, 4, 1);                                    /* a later touch: the highlight, no popup */
    rfm_move_screen(&m, NULL, out);
    CHECK(art_diff(out) == 6 * 6 && !rfmv_pop_up(&m, POP_ENC) && m.pop_val == 0);
    knob(&m, 4, -2);                                    /* a new gesture: the count starts again */
    CHECK(m.pop_val == -1 && rfmv_pop_up(&m, POP_ENC));
    touch(&m, 4, 0);
    touch(&m, 8, 1), knob(&m, 8, 1), knob(&m, 8, -1);   /* MASTER the same */
    CHECK(rfmv_pop_up(&m, POP_MASTER));
    touch(&m, 8, 0);
    CHECK(!rfmv_pop_up(&m, POP_MASTER) && m.pop == POP_NONE);
    ms(&m, RFM_MOVE_POPUP_MS);
    knob(&m, 8, -16);                                   /* the volume knob: MASTER (768 - 256 = 512: 50 %) */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.pop == POP_MASTER);
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, 64 + x, y) != m.thumb[y][x];
    CHECK(d == 0);
    gold("master", out, GOLD_MASTER);
    ms(&m, RFM_MOVE_POPUP_MS);

    /* MIDI mode: the pad map (C major from C3: the bottom-left pad the root) */
    track(&m, 1, 1), track(&m, 1, 0);
    CHECK(m.pop == POP_MODE && m.pop_i == RFM_MOVE_MODE_MIDI);   /* (a tap: "MIDI" named) */
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 1, 49) && px_at(out, 7, 55) && px_at(out, 14, 62));      /* pad 0: the root, filled */
    CHECK(px_at(out, 17, 49) && !px_at(out, 23, 55));                         /* pad 1: D, outlined */
    gold("midi", out, GOLD_MIDI);
    pad(&m, 1, 90);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 18, 50) && !px_at(out, 21, 53) && px_at(out, 27, 62));   /* pressed: filled, a black square */
    gold("midi_hit", out, GOLD_MIDI_HIT);
    pad(&m, 1, 0);
    touch(&m, 4, 1);                                    /* MIDI mode: a touch alone brings the popup */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.pop == POP_ENC && px_at(out, 2, 2));
    touch(&m, 4, 0);
    ms(&m, RFM_MOVE_POPUP_MS);
    /* Track 2 held: the menu (layout, scale, root, channel); then the scale changed: its name at 2x */
    track(&m, 1, 1);
    pad(&m, 17, 100), pad(&m, 17, 0);                   /* row 2, pad 2: minor */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.menu_what == MENU_SCALE && m.s.scale == RFM_MOVE_SCALE_MINOR);
    CHECK(px_at(out, 17, 6) && px_at(out, 3, 6) && !px_at(out, 1, 6));        /* scale 2 a block, 1 a line (24: 3 rows) */
    gold("menu_midi", out, GOLD_MENU_MIDI);
    pad(&m, 16, 100), pad(&m, 16, 0);
    pad(&m, 26, 100), pad(&m, 26, 0);                   /* the drum layout: a pad held + the jog wheel, its note */
    track(&m, 1, 0);
    pad(&m, 7, 100);
    jog(&m, -2);
    rfm_move_screen(&m, NULL, out);
    CHECK(m.pop == POP_DRUM && m.pop_i == 7 && m.s.drum[7] == 34 && px_at(out, 2, 2));
    write_pbm("move_pop_drum.pbm", out);
    pad(&m, 7, 0);
    ms(&m, RFM_MOVE_POPUP_MS - 1);
    CHECK(rfmv_pop_up(&m, POP_DRUM));
    ms(&m, 1);
    CHECK(!rfmv_pop_up(&m, POP_DRUM));
    track(&m, 1, 1), pad(&m, 24, 100), pad(&m, 24, 0), track(&m, 1, 0);

    /* Control mode: the banks, the 8 values (64 at first: half full) */
    track(&m, 2, 1), track(&m, 2, 0);
    knob(&m, 1, 36);                                    /* knob 2: 100 */
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 1, 1) && px_at(out, 30, 4) && px_at(out, 33, 1) && !px_at(out, 40, 3));
    CHECK(px_at(out, 7, 63) && px_at(out, 7, 37) && !px_at(out, 7, 34) && px_at(out, 23, 21));
    gold("control", out, GOLD_CONTROL);
    knob(&m, 0, 1);
    rfm_move_screen(&m, NULL, out);
    CHECK(m.pop == POP_CC && m.pop_i == 0);
    gold("cc", out, GOLD_CC);
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out2);
    CHECK(px_at(out, 66, 0) && !px_at(out2, 66, 0));    /* dropped (the hatching gone) */
    track(&m, 2, 1);                                    /* Track 3 held + knob 3: its CC number */
    knob(&m, 2, 2);
    rfm_move_screen(&m, NULL, out);
    CHECK(m.menu_what == MENU_CC && m.menu_i == 2 && m.s.cc[0][2] == 20 && px_at(out, 40, 50));
    gold("menu_cc", out, GOLD_MENU_CC);
    track(&m, 2, 0);
    CHECK(GOLD_FM1 && GOLD_FM1_LIVE && GOLD_KNOB && GOLD_MASTER && GOLD_MIDI && GOLD_MIDI_HIT && GOLD_CONTROL &&
          GOLD_CC && GOLD_MENU_MIDI && GOLD_MENU_CC);
}

/* the thumbnail is a luminance threshold, no dither: flat colours come out solid, 1 px strokes survive */
static void test_threshold(void)
{
    static uint16_t lcd[240 * 240];
    rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES];
    int x, y, solid_white = 1, solid_black = 1, line = 0;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 240; x++)
            lcd[y * 240 + x] = x < 120 ? 0x4208 /* dark grey, luma ~66: black */ : 0xF800 /* red, luma ~76+: see below */;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 120; x++)
            if (y < 120)
                lcd[y * 240 + x] = 0x8410;              /* mid grey, luma ~132: white */
    for (y = 120; y < 240; y++)
        lcd[y * 240 + 60] = 0xFFFF;                     /* a 1 px white line in the dark grey */
    rfm_move_init(&m, NULL, on_act, NULL);
    rfm_move_screen(&m, lcd, out);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            const int v = m.thumb[y][x];
            if (y < 31 && x < 31 && !v) solid_white = 0;      /* mid grey quarter */
            if (y >= 33 && x < 31 && x != 16 && v) solid_black = 0;  /* dark grey quarter but the line's column */
            if (y >= 33 && x == 16 && v) line++;
        }
    CHECK(solid_white);
    CHECK(solid_black);
    CHECKF(line >= 30, "the 1 px line survives (%d of 31 rows)", line);
    /* pure red (luma 76) is under the threshold (80): black */
    CHECK(m.thumb[10][50] == (((77 * 255) >> 8) > RFM_MOVE_THUMB_LUMA));
}

/* the owner's MIDI mode round: Shift + step 9 (Keys & Scales), the scale pads coloured, a Track 2 tap turning MIDI
 * mode's screens (NOTES: the notes and the chord), the mode named on a tap (not a hold), the settings' version 2 */
static void test_midi_round(void)
{
    static rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES], img[RFM_MOVE_SETTINGS_BYTES];
    rfm_move_settings_t st;
    rfm_move_led_t leds[RFM_MOVE_NLEDS];
    char c[16];
    int i, n, x, y, d;
    rfm_move_init(&m, NULL, on_act, NULL);
    /* the chords */
    {
        static const struct { uint8_t n[5]; int k; const char *name; } T[] = {
            { { 48, 52, 55, 59 }, 4, "Cmaj7" }, { { 57, 60, 64 }, 3, "Am" }, { { 50, 55, 57 }, 3, "Dsus4" },
            { { 55, 59, 62, 65 }, 4, "G7" }, { { 52, 55, 60 }, 3, "C/E" }, { { 48, 51, 55 }, 3, "Cm" },
            { { 48, 55 }, 2, "C5" }, { { 48, 49 }, 2, "" }, { { 60 }, 1, "" }, { { 47, 50, 53, 57 }, 4, "Bm7b5" },
        };
        for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++)
            CHECKF(!strcmp(rfmv_chord_name(T[i].n, T[i].k, c, sizeof c), T[i].name), "%s: %s", T[i].name, c);
    }
    /* a tap on Track 2: MIDI mode, named; a hold (its menu) names nothing */
    track(&m, 1, 1), track(&m, 1, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI && m.pop == POP_MODE && m.pop_i == RFM_MOVE_MODE_MIDI);
    rfm_move_screen(&m, NULL, out);
    write_pbm("move_mode_midi.pbm", out);
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 128; x++)
            d += px_at(out, x, y) && (y < 14 || y > 50);
    CHECK(!px_at(out, 2, 2) && !px_at(out, 0, 0) && d == 0 && px_at(out, 64 - 46, 18 + 1));   /* (the whole screen, no
                                                         * frame: "MIDI" at 4x, centred) */
    ms(&m, RFM_MOVE_POPUP_MS);
    track(&m, 2, 1), now_ms += 500, track(&m, 2, 0);    /* Track 3 held: its menu over MIDI mode, no switch */
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI && !rfmv_pop_up(&m, POP_MODE));
    track(&m, 2, 1), track(&m, 2, 0);                   /* (a tap: Control mode) */
    ms(&m, RFM_MOVE_POPUP_MS);
    m.thumb[10][10] = 1;                                /* (a lit pixel on the FM-1's screen: covered by the popup) */
    track(&m, 0, 1), track(&m, 0, 0);                   /* FM-1: "FM-1" over the whole screen too */
    CHECK(m.pop == POP_MODE && m.pop_i == 0);
    rfm_move_screen(&m, NULL, out);
    CHECK(!px_at(out, 64 + 10, 10) && !px_at(out, 0, 0));
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 64 + 10, 10));                     /* (then the FM-1's screen again) */
    m.thumb[10][10] = 0;
    track(&m, 1, 1), track(&m, 1, 0);
    ms(&m, RFM_MOVE_POPUP_MS);
    /* MIDI mode has no Shift shortcuts (the owner's): Shift + step 9 is the FM-1's GLO and nothing else */
    clear();
    button(&m, MR_CC_SHIFT, 1), step(&m, 8, 1), step(&m, 8, 0), button(&m, MR_CC_SHIFT, 0);
    CHECK(!m.mmenu && has(RFM_MOVE_ACT_BTN, RFM_MOVE_B_GLO, 1, 0));
    /* the scale: Track 2 held, row 2's pads (cyan); its hold screen only once held 300 ms, or used */
    clear();
    track(&m, 1, 1);
    rfm_move_screen(&m, NULL, out);
    CHECK(!px_at(out, 1, 0) && rfm_move_screen_timed(&m));   /* (not yet: the pad map; the host draws again) */
    ms(&m, RFM_MOVE_TAP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 1, 0));                            /* (held: the menu's layout tabs) */
    pad(&m, 17, 100), pad(&m, 17, 0);                   /* minor */
    rfm_move_screen(&m, NULL, out);
    write_pbm("move_midi_keys.pbm", out);
    CHECK(px_at(out, 17, 4) && px_at(out, 17, 7) && !px_at(out, 17, 9));   /* (minor's tab, tall: a scale pad pressed) */
    jog(&m, 2);                                         /* D */
    CHECK(m.s.scale == RFM_MOVE_SCALE_MINOR && m.s.root == 2 && count(RFM_MOVE_ACT_MIDI) == 0);
    rfm_move_leds_invalidate(&m);
    n = rfm_move_leds(&m, leds, RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++)
        if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 17)
            CHECK(leds[i].r == 0 && leds[i].b == 255);  /* (the current scale's pad: cyan, full) */
        else if (!leds[i].cc && leds[i].number == MR_NOTE_PAD0 + 16)
            CHECK(leds[i].r == 0 && leds[i].b > 20 && leds[i].b < 100);
    clear();
    knob(&m, 0, RFM_MOVE_LIST_TICKS - 1);               /* knob 1: the root (one a few ticks) */
    CHECK(m.s.root == 2);
    knob(&m, 0, 1);
    CHECK(m.s.root == 3 && m.menu_what == MENU_ROOT && count(RFM_MOVE_ACT_TURN) == 0);
    knob(&m, 1, -RFM_MOVE_LIST_TICKS);                  /* knob 2: the scale (minor -> major) */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.s.scale == RFM_MOVE_SCALE_MAJOR && m.menu_what == MENU_SCALE && px_at(out, 1, 4) && px_at(out, 1, 7));   /* (its tab tall) */
    knob(&m, 1, RFM_MOVE_LIST_TICKS), knob(&m, 0, -RFM_MOVE_LIST_TICKS);
    CHECK(m.s.scale == RFM_MOVE_SCALE_MINOR && m.s.root == 2);
    /* 24 scales: rows 2, 3, 4 of the pads (the first 8 as before); knob 2 walks all of them; short names */
    pad(&m, 8, 100), pad(&m, 8, 0);                     /* row 3, pad 1: scale 9, phrygian */
    CHECK(m.s.scale == RFM_MOVE_SCALE_PHRYGIAN);
    pad(&m, 7, 100), pad(&m, 7, 0);                     /* row 4, pad 8: scale 24, bebop */
    CHECK(m.s.scale == RFM_MOVE_SCALE_BEBOP && RFM_MOVE_NSCALES == 24);
    knob(&m, 1, 3 * RFM_MOVE_LIST_TICKS);
    CHECK(m.s.scale == RFM_MOVE_NSCALES - 1);           /* (the last) */
    for (i = 0; i < RFM_MOVE_NSCALES; i++) {
        int k, prev = -1, ok = rfmv_scales[i][0] >= 5 && rfmv_scales[i][0] <= 8 && rfmv_scales[i][1] == 0;
        for (k = 1; k <= rfmv_scales[i][0]; k++)
            ok &= rfmv_scales[i][k] > prev && rfmv_scales[i][k] < 12, prev = rfmv_scales[i][k];
        CHECKF(ok && strlen(rfm_move_scale_name(i)) <= 6, "scale %d %s", i, rfm_move_scale_name(i));
    }
    {   /* bebop's 8 notes on the in-scale layout; the settings keep scale 24 */
        rfm_move_settings_t st2 = m.s;
        uint8_t im[RFM_MOVE_SETTINGS_BYTES];
        st2.layout = RFM_MOVE_LAYOUT_SCALE, st2.root = 0, st2.octave = 0;
        CHECK(rfm_move_pad_note(&st2, 7) == 48 + 11 && rfm_move_pad_note(&st2, 8) == 48 + 5);   /* (8th degree B; +3 a row: F) */
        rfm_move_save(&st2, im);
        CHECK(rfm_move_load(&st2, im, sizeof im) == 0 && st2.scale == RFM_MOVE_SCALE_BEBOP);
    }
    pad(&m, 17, 100), pad(&m, 17, 0);                   /* (minor again) */
    track(&m, 1, 0);                                    /* (used: no tap on its release) */
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI && m.s.midi_view == RFM_MOVE_MV_PADS);
    rfm_move_set_scale(&m, RFM_MOVE_SCALE_MAJOR, 0);
    track(&m, 2, 1), track(&m, 2, 0);                   /* a quick click on Track 3: its hold screen never drawn */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.s.mode == RFM_MOVE_MODE_CONTROL && m.pop == POP_MODE && !px_at(out, 1, 1));
    track(&m, 1, 1), track(&m, 1, 0);
    ms(&m, RFM_MOVE_POPUP_MS);
    /* Track 2 tapped in MIDI mode: NOTES (named); the notes held and their chord on the left, the FM-1's screen
     * right; a knob's popup on the left half while turning only */
    track(&m, 1, 1), track(&m, 1, 0);
    CHECK(m.s.midi_view == RFM_MOVE_MV_NOTES && m.pop == POP_MODE && m.pop_i == 10 + RFM_MOVE_MV_NOTES);
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out);
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, x, y);
    CHECK(d == 0);                                      /* (at rest: black) */
    pad(&m, 0, 100), pad(&m, 2, 100), pad(&m, 4, 100); /* C3 E3 G3: "C" */
    rfm_move_screen(&m, NULL, out);
    write_pbm("move_midi_notes.pbm", out);
    for (d = 0, y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            d += px_at(out, x, y);
    CHECK(d > 10);
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, 64 + x, y) != m.thumb[y][x];
    CHECK(d == 0);
    pad(&m, 0, 0), pad(&m, 2, 0), pad(&m, 4, 0);
    touch(&m, 4, 1);
    CHECK(!rfmv_pop_up(&m, POP_ENC));                   /* (a touch: nothing) */
    knob(&m, 4, 4);
    rfm_move_screen(&m, NULL, out);
    CHECK(rfmv_pop_up(&m, POP_ENC) && m.pop_val == 2 && px_at(out, 0, 0) && px_at(out, 63, 63));
    touch(&m, 4, 0);
    CHECK(!rfmv_pop_up(&m, POP_ENC) && m.pop_val == 0);
    /* the settings keep the screen; a version 1 image (77 bytes) still loads */
    rfm_move_save(&m.s, img);
    CHECK(rfm_move_load(&st, img, sizeof img) == 0 && st.midi_view == RFM_MOVE_MV_NOTES);
    img[3] = 2, img[4] = (uint8_t)(RFM_MOVE_SETTINGS_BYTES - 2);   /* (version 2: no velocity setting) */
    CHECK(rfm_move_load(&st, img, RFM_MOVE_SETTINGS_BYTES - 2) == 0 && st.midi_view == RFM_MOVE_MV_NOTES &&
          st.vel_mode == RFM_MOVE_VEL_DYNAMIC && st.vel_level == 127);
    img[3] = 1, img[4] = (uint8_t)(RFM_MOVE_SETTINGS_BYTES - 3);
    CHECK(rfm_move_load(&st, img, RFM_MOVE_SETTINGS_BYTES - 3) == 0 && st.midi_view == RFM_MOVE_MV_PADS &&
          st.mode == RFM_MOVE_MODE_MIDI);
    track(&m, 1, 1), track(&m, 1, 0);                   /* (round: PADS again) */
    CHECK(m.s.midi_view == RFM_MOVE_MV_PADS && m.pop_i == 10);
}

/* FM-1 mode: Track 1 held is the Move's Shift layer (Shift itself is the FM-1's GLO): Shift + step and Shift +
 * Record work on the sequencer, their menus stay open until Back; the pads stay the FM-1's keys at once */
static void test_fm1_shift_layer(void)
{
    static rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES];
    rfm_move_led_t leds[RFM_MOVE_NLEDS];
    int i, n, x, y, d;
    rfm_move_init(&m, NULL, on_act, NULL);
    ms(&m, RFM_MOVE_POPUP_MS);
    clear();
    button(&m, MR_CC_SHIFT, 1), button(&m, MR_CC_SHIFT, 0);   /* Shift: the FM-1's */
    CHECK(has(RFM_MOVE_ACT_BTN, RFM_MOVE_B_GLO, 1, 0));
    track(&m, 0, 1);
    clear();
    pad(&m, 16, 100);                                   /* (a key while Track 1 is held: at once) */
    CHECK(nact == 1 && has(RFM_MOVE_ACT_KEY, 0, 1, 0));
    pad(&m, 16, 0);
    rfm_move_screen(&m, NULL, out);
    CHECK(!px_at(out, 0, 0));                           /* (the badge: not before 300 ms, or a use) */
    ms(&m, RFM_MOVE_TAP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 0, 0) && px_at(out, 39, 10));      /* the Shift layer's badge */
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            d += px_at(out, 64 + x, y) != m.thumb[y][x];
    CHECK(d == 0);
    write_pbm("move_fm1_shift.pbm", out);
    rfm_move_leds_invalidate(&m);
    n = rfm_move_leds(&m, leds, RFM_MOVE_NLEDS);
    for (i = 0; i < n; i++)
        if (!leds[i].cc && leds[i].number == MR_NOTE_STEP0 + 5)
            CHECK(leds[i].r > 0);                       /* (step 6: Metronome, lit) */
        else if (!leds[i].cc && leds[i].number == MR_NOTE_STEP0 + 3)
            CHECK(leds[i].r == 0);                      /* (step 4: nothing) */
    for (i = 0; i < n; i++)                             /* the icons under the steps: a shortcut dim, none off */
        if (leds[i].cc && leds[i].number == MR_CC_STEP_ICON0 + 5)
            CHECK(!leds[i].rgb && leds[i].r == 24);
        else if (leds[i].cc && leds[i].number == MR_CC_STEP_ICON0 + 3)
            CHECK(leds[i].r == 0);
    clear();
    step(&m, 5, 1), step(&m, 5, 0);                     /* Shift + step 6: the metronome */
    CHECK(m.seq.metro && count(RFM_MOVE_ACT_KEY) == 0);
    {   /* written on change only: the metronome's icon full now, nothing else */
        int k = 0;
        n = rfm_move_leds(&m, leds, RFM_MOVE_NLEDS);
        for (i = 0; i < n; i++)
            if (leds[i].cc && leds[i].number == MR_CC_STEP_ICON0 + 5) {
                k++;
                CHECK(leds[i].r == 127);
            } else if (leds[i].cc && leds[i].number >= MR_CC_STEP_ICON0 && leds[i].number < MR_CC_STEP_ICON0 + 16)
                k += 100;
        CHECK(k == 1);
    }
    step(&m, 6, 1), step(&m, 6, 0);                     /* Shift + step 7: Groove's menu */
    rfm_move_leds(&m, leds, RFM_MOVE_NLEDS);
    track(&m, 0, 0);
    {   /* Track 1 let go: every icon off (and the step pads' own LEDs, notes 16..31, untouched by them) */
        int k = 0, on = 0;
        n = rfm_move_leds(&m, leds, RFM_MOVE_NLEDS);
        for (i = 0; i < n; i++)
            if (leds[i].cc && leds[i].number >= MR_CC_STEP_ICON0 && leds[i].number < MR_CC_STEP_ICON0 + 16)
                k++, on += leds[i].r != 0;
        CHECK(k == 13 && on == 0);
    }
    CHECK(m.seq.smenu == RFM_MOVE_SM_GROOVE && m.s.mode == RFM_MOVE_MODE_FM1 && m.pop != POP_MODE);
    clear();
    jog(&m, 2);                                         /* (the menu's: the groove, not SELECT) */
    CHECK(m.seq.groove == 10 && count(RFM_MOVE_ACT_TURN) == 0);
    rfm_move_screen(&m, NULL, out);
    write_pbm("move_fm1_groove.pbm", out);
    clear();
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);   /* Back: the menu closes (not the FM-1's SEQ) */
    CHECK(!m.seq.smenu && nact == 0);
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);   /* (nothing open: the FM-1's SEQ again) */
    CHECK(has(RFM_MOVE_ACT_BTN, RFM_MOVE_B_SEQ, 1, 0));
    track(&m, 0, 1), button(&m, MR_CC_REC, 1), button(&m, MR_CC_REC, 0), track(&m, 0, 0);   /* Shift + Record */
    CHECK(m.seq.smenu == RFM_MOVE_SM_REC);
    clear();
    jog(&m, 1);
    CHECK(m.seq.rec_mode == RFM_MOVE_RM_RECORD && count(RFM_MOVE_ACT_TURN) == 0);
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
    track(&m, 0, 1), step(&m, 4, 1), step(&m, 4, 0), now_ms += 400, step(&m, 4, 1), step(&m, 4, 0), track(&m, 0, 0);
    CHECKF(m.seq.bpm10 >= 1450 && m.seq.bpm10 <= 1500 && m.seq.smenu == RFM_MOVE_SM_TEMPO, "tapped %d", m.seq.bpm10);   /* (Shift +
                                                         * step 5 twice, ~410 ms apart: tap tempo) */
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
    track(&m, 0, 1), step(&m, 0, 1), step(&m, 0, 0), track(&m, 0, 0);   /* Shift + step 1: the projects */
    CHECK(m.seq.projects);
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
    CHECK(!m.seq.projects);
    /* a quick tap on Track 1 from another mode: FM-1 mode, named */
    track(&m, 1, 1), track(&m, 1, 0);
    button(&m, MR_CC_SHIFT, 1);                         /* (MIDI mode: no Shift layer, no icons) */
    {
        uint8_t ic[16];
        rfmv_shift_icons(&m, ic);
        for (i = 0; i < 16; i++)
            CHECK(ic[i] == 0);
    }
    button(&m, MR_CC_SHIFT, 0);
    track(&m, 3, 1), track(&m, 3, 0);                   /* the sequencer: Shift held, or locked: the icons */
    button(&m, MR_CC_SHIFT, 1);
    {
        uint8_t ic[16];
        rfmv_shift_icons(&m, ic);
        CHECK(ic[0] == 24 && ic[3] == 0 && ic[5] == 127 && ic[15] == 24);   /* (the metronome on: full) */
        button(&m, MR_CC_SHIFT, 0);
        rfmv_shift_icons(&m, ic);
        CHECK(ic[0] == 0 && ic[5] == 0);
        now_ms += 1000;
        button(&m, MR_CC_SHIFT, 1), button(&m, MR_CC_SHIFT, 0), button(&m, MR_CC_SHIFT, 1), button(&m, MR_CC_SHIFT, 0);
        rfmv_shift_icons(&m, ic);
        CHECK(m.seq.shift_lock && ic[0] == 24);
        button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
    }
    track(&m, 0, 1), track(&m, 0, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1 && m.pop == POP_MODE && m.pop_i == 0);
    clear();
    step(&m, 4, 1), step(&m, 4, 0);                     /* (Track 1 let go: the steps are keys again) */
    CHECK(count(RFM_MOVE_ACT_KEY) == 2);
}

/* a Track button switches only on a tap: its name at the release, first (the new mode's screen once the name has
 * gone); a press shows nothing new; a hold is its menu over the mode that is on, which stays */
static void test_mode_name_first(void)
{
    static rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES];
    int x, y, d;
    rfm_move_init(&m, NULL, on_act, NULL);
    track(&m, 2, 1);                                    /* Track 3 down: still FM-1 mode, no name */
    rfm_move_screen(&m, NULL, out);
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1 && m.pop != POP_MODE);
    track(&m, 2, 0);                                    /* the tap's release: Control mode, its name only */
    rfm_move_screen(&m, NULL, out);
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 128; x++)
            d += px_at(out, x, y) && (y < 14 || y > 50);
    CHECK(m.s.mode == RFM_MOVE_MODE_CONTROL && m.pop == POP_MODE && d == 0 && !px_at(out, 2, 8) &&
          rfm_move_screen_timed(&m));                   /* (no bank tabs, no bars; the host draws again) */
    ms(&m, RFM_MOVE_POPUP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 2, 8) && !rfm_move_screen_timed(&m));   /* (then Control mode's screen) */
    track(&m, 1, 1);                                    /* Track 2 held 300 ms: its menu over Control mode */
    ms(&m, RFM_MOVE_TAP_MS);
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 1, 0) && m.s.mode == RFM_MOVE_MODE_CONTROL);   /* (the menu's layout tabs) */
    now_ms += 400;
    track(&m, 1, 0);
    CHECK(!rfmv_pop_up(&m, POP_MODE) && m.s.mode == RFM_MOVE_MODE_CONTROL);   /* (no switch, no name) */
    track(&m, 0, 1), now_ms += 400, track(&m, 0, 0);   /* (Track 1 held, nothing done: no switch either) */
    CHECK(m.s.mode == RFM_MOVE_MODE_CONTROL);
    track(&m, 0, 1), track(&m, 0, 0), ms(&m, RFM_MOVE_POPUP_MS);   /* (a tap: FM-1 mode) */
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1);
    track(&m, 1, 1), ms(&m, RFM_MOVE_TAP_MS);          /* Track 2 held from FM-1 mode: its menu, its pads */
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 1, 0) && m.s.mode == RFM_MOVE_MODE_FM1);
    pad(&m, 0, 100), pad(&m, 0, 0);                     /* (the top-left scale pad) */
    CHECK(m.s.scale == rfmv_scale_of_pad(0));
    now_ms += 400, track(&m, 1, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1 && !rfmv_pop_up(&m, POP_MODE));   /* (back to FM-1 mode, as it was) */
}

/* Track 1 in every mode (the owner's): a tap switches to FM-1 mode (its name at the release, first); held, it is the
 * Shift layer and switches nothing (MIDI mode: the pads stay notes) */
static void test_track1_anywhere(void)
{
    static rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES];
    uint8_t ic[16];
    int x, y, d;
    rfm_move_init(&m, NULL, on_act, NULL);
    track(&m, 1, 1), track(&m, 1, 0);                   /* MIDI mode */
    ms(&m, RFM_MOVE_POPUP_MS);
    track(&m, 0, 1);                                    /* Track 1 down: still MIDI mode, the Shift layer */
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI);
    rfmv_shift_icons(&m, ic);
    CHECK(ic[5] == 24 && ic[3] == 0);                   /* (the icons under the steps) */
    clear();
    pad(&m, 0, 100), pad(&m, 0, 0);                     /* (the pads: notes) */
    CHECK(has_midi(0x90, 48, 100));
    step(&m, 5, 1), step(&m, 5, 0);                     /* Shift + step 6: the metronome */
    CHECK(m.seq.metro);
    ms(&m, RFM_MOVE_POPUP_MS);                          /* (the CLICK ON message gone) */
    rfm_move_screen(&m, NULL, out);
    CHECK(px_at(out, 0, 0) && px_at(out, 39, 10));      /* (held: the SHIFT badge over MIDI mode's screen) */
    step(&m, 8, 1), step(&m, 8, 0);                     /* Shift + step 9: Keys & Scales, open past the release */
    track(&m, 0, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI && m.seq.smenu == RFM_MOVE_SM_KEYS);   /* (used: no switch) */
    clear();
    knob(&m, 1, RFM_MOVE_LIST_TICKS);                   /* (the menu's: the scale, not an FM-1 encoder) */
    CHECK(m.s.scale == 1 && count(RFM_MOVE_ACT_TURN) == 0);
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
    CHECK(!m.seq.smenu && m.s.mode == RFM_MOVE_MODE_MIDI);
    /* a tap: FM-1 mode, the name drawn first */
    track(&m, 0, 1);
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI);
    track(&m, 0, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_FM1 && m.pop == POP_MODE && m.pop_i == 0);
    rfm_move_screen(&m, NULL, out);
    for (d = 0, y = 0; y < 64; y++)
        for (x = 0; x < 128; x++)
            d += px_at(out, x, y) && (y < 14 || y > 50);
    CHECK(d == 0);                                      /* (the name only, no drawing) */
    /* the sequencer: Track 1 held is its Shift layer too (no step entered) */
    track(&m, 3, 1), track(&m, 3, 0);
    ms(&m, RFM_MOVE_POPUP_MS);
    track(&m, 0, 1), step(&m, 5, 1), step(&m, 5, 0), track(&m, 0, 0);
    CHECK(m.s.mode == RFM_MOVE_MODE_SEQ && !m.seq.metro && m.seq.nnotes == 0);
}

/* the Velocity menu (Track 1 held + step 10 in FM-1 / MIDI mode, Shift + step 10 in the sequencer): sticky; the jog
 * the option, knob 1 the level; every pad that plays goes through it */
static void test_velocity(void)
{
    static rfm_move_t m;
    rfm_move_settings_t s;
    uint8_t img[RFM_MOVE_SETTINGS_BYTES];
    rfm_move_settings_default(&s);
    CHECK(rfm_move_vel(&s, 40) == 40 && rfm_move_vel(&s, 0) == 1);
    s.vel_mode = RFM_MOVE_VEL_SOFT;
    CHECK(rfm_move_vel(&s, 40) > 40 && rfm_move_vel(&s, 127) == 127 && rfm_move_vel(&s, 1) >= 1);
    s.vel_mode = RFM_MOVE_VEL_HARD;
    CHECK(rfm_move_vel(&s, 40) < 40 && rfm_move_vel(&s, 127) == 127 && rfm_move_vel(&s, 1) == 1);
    s.vel_mode = RFM_MOVE_VEL_FIXED, s.vel_level = 77;
    CHECK(rfm_move_vel(&s, 5) == 77 && !strcmp(rfm_move_vel_name(s.vel_mode), "FIXED"));
    rfm_move_save(&s, img);
    rfm_move_settings_default(&s);
    CHECK(rfm_move_load(&s, img, sizeof img) == 0 && s.vel_mode == RFM_MOVE_VEL_FIXED && s.vel_level == 77);

    rfm_move_init(&m, NULL, on_act, NULL);
    track(&m, 0, 1), step(&m, 9, 1), step(&m, 9, 0), track(&m, 0, 0);   /* FM-1 mode: the menu, over FM-1 mode */
    CHECK(m.seq.smenu == RFM_MOVE_SM_VEL && m.s.mode == RFM_MOVE_MODE_FM1);
    jog(&m, 1), knob(&m, 0, -27);                       /* FIXED, 100 */
    CHECK(m.s.vel_mode == RFM_MOVE_VEL_FIXED && m.s.vel_level == 100 && m.seq.smenu == RFM_MOVE_SM_VEL);
    button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
    CHECK(!m.seq.smenu);
    track(&m, 1, 1), track(&m, 1, 0);                   /* MIDI mode: a pad at the level */
    clear();
    pad(&m, 0, 20);
    CHECK(has_midi(0x90, 48, 100));
    pad(&m, 0, 0);
}

/* the owner's: only a quick Track tap changes the mode. Every other button (alone, with Shift, with Back after), the
 * jog and the knobs leave it as it was, in every mode */
static void test_mode_only_by_tap(void)
{
    static const int CC[] = { MR_CC_CAPTURE, MR_CC_SAMPLE, MR_CC_MUTE, MR_CC_DELETE, MR_CC_COPY, MR_CC_SHIFT,
                              MR_CC_MENU, MR_CC_UNDO, MR_CC_LOOP, MR_CC_BACK, MR_CC_PLAY, MR_CC_REC, MR_CC_LEFT,
                              MR_CC_RIGHT, MR_CC_UP, MR_CC_DOWN };
    static rfm_move_t m;
    int mode, i, k, ok = 1;
    rfm_move_init(&m, NULL, on_act, NULL);
    for (mode = 0; mode < 4; mode++) {
        if (m.s.mode != mode)
            track(&m, mode, 1), track(&m, mode, 0);
        CHECK(m.s.mode == mode);
        for (i = 0; i < (int)(sizeof CC / sizeof CC[0]); i++) {
            button(&m, CC[i], 1), button(&m, CC[i], 0);
            button(&m, MR_CC_SHIFT, 1), button(&m, CC[i], 1), button(&m, CC[i], 0), button(&m, MR_CC_SHIFT, 0);
            track(&m, 0, 1), button(&m, CC[i], 1), button(&m, CC[i], 0), now_ms += 400, track(&m, 0, 0);
            for (k = 0; k < 4; k++)
                button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
            jog(&m, 1), knob(&m, 0, 2);
            if (m.s.mode != mode) {
                CHECKF(0, "mode %d changed to %d by CC %d", mode, m.s.mode, CC[i]);
                ok = 0;
                track(&m, mode, 1), track(&m, mode, 0);
            }
        }
        for (i = 0; i < 16; i++) {                      /* (the Shift layer's shortcuts, Back after each) */
            if (mode == RFM_MOVE_MODE_SEQ)
                button(&m, MR_CC_SHIFT, 1), step(&m, i, 1), step(&m, i, 0), button(&m, MR_CC_SHIFT, 0);
            else
                track(&m, 0, 1), step(&m, i, 1), step(&m, i, 0), now_ms += 400, track(&m, 0, 0);
            for (k = 0; k < 4; k++)
                button(&m, MR_CC_BACK, 1), button(&m, MR_CC_BACK, 0);
            if (m.s.mode != mode) {
                CHECKF(0, "mode %d changed to %d by shortcut %d", mode, m.s.mode, i + 1);
                ok = 0;
                track(&m, mode, 1), track(&m, mode, 0);
            }
        }
        rfm_move_seq_play(&m, 0);
    }
    CHECK(ok);
}

int main(void)
{
    test_fm1_pads();
    test_steps();
    test_buttons_knobs();
    test_modes();
    test_layouts();
    test_drum_settings();
    test_control();
    test_leds();
    test_screen();
    test_threshold();
    test_midi_round();
    test_fm1_shift_layer();
    test_mode_name_first();
    test_track1_anywhere();
    test_velocity();
    test_mode_only_by_tap();
    return check_done("move");
}
