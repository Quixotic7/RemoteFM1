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
        knob(&m, i, 3);
        knob(&m, i, -2);
        if (ENC[i] < 0)
            CHECK(nact == 0);
        else
            CHECK(nact == 2 && has(RFM_MOVE_ACT_TURN, ENC[i], 0, 3) && has(RFM_MOVE_ACT_TURN, ENC[i], 0, -2));
    }
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
    touch(&m, 4, 1);
    CHECK(m.touched == 4 && m.last_kind == 1 && m.last_index == RFM_MOVE_E_K1);
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
    track(&m, 1, 1);                                    /* Track 2: MIDI mode, everything let go */
    CHECK(m.s.mode == RFM_MOVE_MODE_MIDI && (m.track_held & 2));
    CHECK(has(RFM_MOVE_ACT_KEY, 0, 0, 0) && has(RFM_MOVE_ACT_PRESS, 0, 0, 0) &&
          has(RFM_MOVE_ACT_KEY, rfm_move_step_key(4), 0, 0) && has(RFM_MOVE_ACT_BTN, RFM_MOVE_B_GLO, 0, 0));
    CHECK(count(RFM_MOVE_ACT_SETTINGS) == 1);
    clear();
    pad(&m, pad_of(2, 1), 0), step(&m, 4, 0), button(&m, MR_CC_SHIFT, 0);   /* the old presses' ends: nothing */
    track(&m, 1, 0);
    CHECK(nact == 0 && m.track_held == 0);
    clear();
    track(&m, 1, 1), track(&m, 1, 0);                   /* the same mode: nothing */
    CHECK(nact == 0);
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
    knob(&m, 0, 1);
    CHECK(has(RFM_MOVE_ACT_TURN, RFM_MOVE_E_PRESETS, 0, 1));
}

static void test_drum_settings(void)
{
    rfm_move_t m;
    rfm_move_settings_t s, t;
    uint8_t img[RFM_MOVE_SETTINGS_BYTES], bad[RFM_MOVE_SETTINGS_BYTES];
    rfm_move_init(&m, NULL, on_act, NULL);
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
 * look at the PBMs this test writes into the build directory first */
#define GOLD_DEFAULT 0x8931f095u
#define GOLD_MIDI 0x04969954u
#define GOLD_HELD 0xeb71336du

static void test_screen(void)
{
    static uint16_t lcd[240 * 240];
    rfm_move_t m;
    uint8_t out[MR_SCREEN_BYTES], out2[MR_SCREEN_BYTES];
    int x, y, set = 0, i;
    uint32_t h;
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
    touch(&m, 4, 1);
    knob(&m, 4, 3);
    touch(&m, 4, 0);
    rfm_move_screen(&m, lcd, out);
    for (i = 0; i < MR_SCREEN_BYTES; i++)
        set += out[i] != 0;
    CHECK(set > 100);
    /* the thumbnail: the white box top left of the right half, the disc dark, the ramp dark at its left */
    CHECK(out[(1 * 128) + 64 + 15] == 0xFF);            /* x 79, y 8..15: inside the box */
    CHECK(out[(5 * 128) + 64 + 43] == 0x00);            /* x 107, y 40..47: inside the disc */
    h = fnv(out, MR_SCREEN_BYTES);
    write_pbm("move_default.pbm", out);
    printf("screen default: %08x\n", h);
    CHECKF(GOLD_DEFAULT == 0 || h == GOLD_DEFAULT, "default view %08x, golden %08x", h, GOLD_DEFAULT);
    rfm_move_screen(&m, NULL, out2);                    /* NULL: the last screen kept */
    CHECK(!memcmp(out, out2, sizeof out));
    track(&m, 1, 1);
    track(&m, 1, 0);
    rfm_move_screen(&m, NULL, out);
    h = fnv(out, MR_SCREEN_BYTES);
    write_pbm("move_midi.pbm", out);
    printf("screen midi: %08x\n", h);
    CHECKF(GOLD_MIDI == 0 || h == GOLD_MIDI, "MIDI view %08x, golden %08x", h, GOLD_MIDI);
    track(&m, 2, 1);
    rfm_move_screen(&m, NULL, out);
    h = fnv(out, MR_SCREEN_BYTES);
    write_pbm("move_held.pbm", out);
    printf("screen held: %08x\n", h);
    CHECKF(GOLD_HELD == 0 || h == GOLD_HELD, "held menu %08x, golden %08x", h, GOLD_HELD);
    CHECK(GOLD_DEFAULT && GOLD_MIDI && GOLD_HELD);
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
    return check_done("move");
}
