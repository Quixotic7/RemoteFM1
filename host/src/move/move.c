/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move: an Ableton Move as a RemoteFM1 panel. See rfm/move.h and host/src/move/README.md (the tables, the
 * decisions). Portable C99: no OS calls, no allocation. */
#include <stdio.h>
#include <string.h>

#include "move_int.h"

#define clampi rfmv_clampi
#define SCALES rfmv_scales
#define NOTE_NAMES rfmv_note_names
#define SCALE_NAMES rfmv_scale_names
#define MODE_RGB rfmv_mode_rgb
#define BANK_RGB rfmv_bank_rgb
#define WHITE RFMV_WHITE
#define RED RFMV_RED
#define GREEN RFMV_GREEN
#define OFF RFMV_OFF
#define sc rfmv_sc
#define add rfmv_add
#define emit rfmv_emit
#define emit_midi rfmv_emit_midi
#define changed rfmv_changed
#define note_hold rfmv_note_hold
#define note_let rfmv_note_let
#define let_go rfmv_let_go
#define midi_pad rfmv_midi_pad
typedef rfmv_rgb_t rgb_t;

/* ================================================================ the mapping === */
/* pads 0..31 (bottom-left first, rows of 8): the FM-1 key each plays in FM-1 mode. The design's rows, top first:
 *   row 1  -  F# G# A# -  C# D# -        (keys 1 3 5, 8 10)
 *   row 2  F  G  A  B  C  D  E  F        (keys 0 2 4 6 7 9 11 12)
 *   row 3  F# G# A# -  C# D# -  F#       (keys 13 15 17, 20 22, 25)
 *   row 4  G  A  B  C  D  E  F  G        (keys 14 16 18 19 21 23 24 26) */
static const int8_t PAD_KEY[RFM_MOVE_NPADS] = {
    14, 16, 18, 19, 21, 23, 24, 26,   /* row 4 (bottom) */
    13, 15, 17, -1, 20, 22, -1, 25,   /* row 3 */
    0,  2,  4,  6,  7,  9,  11, 12,   /* row 2 */
    -1, 1,  3,  5,  -1, 8,  10, -1,   /* row 1 (top) */
};
/* steps 1..16: the 16 white keys, F3 .. G5 */
static const int8_t STEP_KEY[RFM_MOVE_NSTEPS] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
/* knobs 1..8 in FM-1 mode: 1 PRESETS, 2 ALGO, 3 4 none, 5..8 K1..K4 */
static const int8_t KNOB_ENC[RFM_MOVE_NKNOBS] = {
    RFM_MOVE_E_PRESETS, RFM_MOVE_E_ALGO, -1, -1, RFM_MOVE_E_K1, RFM_MOVE_E_K2, RFM_MOVE_E_K3, RFM_MOVE_E_K4,
};
/* Move button (CC) -> FM-1 button */
static const struct { uint8_t cc, b; } BUTTONS[] = {
    { MR_CC_CAPTURE, RFM_MOVE_B_FX }, { MR_CC_SAMPLE, RFM_MOVE_B_SEL }, { MR_CC_MUTE, RFM_MOVE_B_ENV },
    { MR_CC_DELETE, RFM_MOVE_B_LFO }, { MR_CC_COPY, RFM_MOVE_B_EDIT }, { MR_CC_SHIFT, RFM_MOVE_B_GLO },
    { MR_CC_MENU, RFM_MOVE_B_HOME }, { MR_CC_UNDO, RFM_MOVE_B_SAVE }, { MR_CC_LOOP, RFM_MOVE_B_ARP },
    { MR_CC_BACK, RFM_MOVE_B_SEQ }, { MR_CC_PLAY, RFM_MOVE_B_PLAY }, { MR_CC_REC, RFM_MOVE_B_REC },
    { MR_CC_LEFT, RFM_MOVE_B_OCTDN }, { MR_CC_RIGHT, RFM_MOVE_B_OCTUP },
};
#define NBUTTONS ((int)(sizeof BUTTONS / sizeof BUTTONS[0]))

/* FM1P_KEYMAP_INIT (FM1VST core-glue/common/panel.h; felucca-bsp's FM1_KEYMAP): id at [row][col] */
static const int8_t FM1_KEYMAP[6][11] = {
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 5, 11, 4, 10, 3, 9, 2, 8, -1, -1, -1 },
    { 34, 35, 36, 37, 38, 40, 39, 13, 7, 6, 12 },
    { 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33 },
    { 0, 1, 15, 14, 17, 16, 19, 18, 20, 21, 22 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};

const uint8_t SCALES[RFM_MOVE_NSCALES][8] = {          /* [0] = length */
    { 7, 0, 2, 4, 5, 7, 9, 11 }, { 7, 0, 2, 3, 5, 7, 8, 10 }, { 7, 0, 2, 3, 5, 7, 9, 10 },
    { 7, 0, 2, 4, 5, 7, 9, 10 }, { 5, 0, 2, 4, 7, 9 },       { 5, 0, 3, 5, 7, 10 },
    { 7, 0, 2, 3, 5, 7, 8, 11 }, { 6, 0, 3, 5, 6, 7, 10 },
};
const char *const SCALE_NAMES[RFM_MOVE_NSCALES] = { "MAJ", "MIN", "DOR", "MIX", "PMAJ", "PMIN", "HMIN", "BLUES" };
static const char *const MODE_NAMES[4] = { "FM-1", "MIDI", "CONTROL", "SEQ" };
static const char *const LAYOUT_NAMES[4] = { "SCALE", "CHROMA", "DRUM", "OCTAVE" };
static const char *const ENC_NAMES[8] = { "SELECT", "ALGO", "K1", "K2", "K3", "K4", "PRESETS", "MASTER" };
const char *const NOTE_NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const uint8_t DEFAULT_CC[RFM_MOVE_NBANKS] = { 16, 24, 102, 110 };

int rfm_move_pad_key(int pad) { return pad >= 0 && pad < RFM_MOVE_NPADS ? PAD_KEY[pad] : -1; }
int rfm_move_step_key(int step) { return step >= 0 && step < RFM_MOVE_NSTEPS ? STEP_KEY[step] : -1; }
int rfm_move_knob_enc(int knob) { return knob >= 0 && knob < RFM_MOVE_NKNOBS ? KNOB_ENC[knob] : -1; }
int rfm_move_button_of(int cc)
{
    int i;
    for (i = 0; i < NBUTTONS; i++)
        if (BUTTONS[i].cc == cc)
            return BUTTONS[i].b;
    return -1;
}
const char *rfm_move_mode_name(int mode) { return mode >= 0 && mode < 4 ? MODE_NAMES[mode] : "?"; }
const char *rfm_move_layout_name(int l) { return l >= 0 && l < 4 ? LAYOUT_NAMES[l] : "?"; }
const char *rfm_move_scale_name(int s) { return s >= 0 && s < RFM_MOVE_NSCALES ? SCALE_NAMES[s] : "?"; }
const char *rfm_move_enc_name(int e) { return e >= 0 && e < 8 ? ENC_NAMES[e] : "?"; }

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* scale degree d (0 = the root) in semitones above the root */
static int degree(int scale, int d)
{
    const uint8_t *sc = SCALES[scale < RFM_MOVE_NSCALES ? scale : 0];
    return 12 * (d / sc[0]) + sc[1 + d % sc[0]];
}

int rfm_move_pad_note(const rfm_move_settings_t *s, int pad)
{
    const int r = pad / 8, c = pad % 8, base = 48 + s->root + 12 * s->octave;
    int n;
    if (pad < 0 || pad >= RFM_MOVE_NPADS)
        return -1;
    switch (s->layout) {
    case RFM_MOVE_LAYOUT_SCALE: n = base + degree(s->scale, c + 3 * r); break;
    case RFM_MOVE_LAYOUT_CHROMATIC: n = base + c + 5 * r; break;
    case RFM_MOVE_LAYOUT_DRUM: n = s->drum[pad] + 12 * s->octave; break;
    default: n = base + 12 * r + degree(s->scale, c); break;
    }
    return n >= 0 && n <= 127 ? n : -1;
}

/* ================================================================ settings === */
void rfm_move_settings_default(rfm_move_settings_t *s)
{
    int b, k, p;
    memset(s, 0, sizeof *s);
    s->mode = RFM_MOVE_MODE_FM1;
    s->layout = RFM_MOVE_LAYOUT_SCALE;
    s->scale = RFM_MOVE_SCALE_MAJOR;
    for (b = 0; b < RFM_MOVE_NBANKS; b++)
        for (k = 0; k < RFM_MOVE_NKNOBS; k++)
            s->cc[b][k] = (uint8_t)(DEFAULT_CC[b] + k);
    for (p = 0; p < RFM_MOVE_NPADS; p++)   /* bottom-right = 36, leftwards, then the next row up from the right */
        s->drum[p] = (uint8_t)(RFM_MOVE_DRUM_BASE + (p / 8) * 8 + (7 - p % 8));
}

uint32_t rfm_move_save(const rfm_move_settings_t *s, uint8_t *o)
{
    uint8_t *p = o;
    *p++ = 'R', *p++ = 'M', *p++ = 'V', *p++ = RFM_MOVE_SETTINGS_VERSION;
    *p++ = (uint8_t)(RFM_MOVE_SETTINGS_BYTES & 0xFF), *p++ = (uint8_t)(RFM_MOVE_SETTINGS_BYTES >> 8);
    *p++ = s->mode, *p++ = s->layout, *p++ = s->channel, *p++ = (uint8_t)s->octave;
    *p++ = s->scale, *p++ = s->root, *p++ = s->bank;
    memcpy(p, s->cc, sizeof s->cc), p += sizeof s->cc;
    memcpy(p, s->drum, sizeof s->drum), p += sizeof s->drum;
    *p++ = s->midi_view;
    return (uint32_t)(p - o);
}

int rfm_move_load(rfm_move_settings_t *s, const uint8_t *b, uint32_t n)
{
    rfm_move_settings_t d;
    const uint8_t *p;
    int i, oct;
    rfm_move_settings_default(s);
    if (!b || n < 6 || b[0] != 'R' || b[1] != 'M' || b[2] != 'V' ||
        !((b[3] == RFM_MOVE_SETTINGS_VERSION && (b[4] | b[5] << 8) == RFM_MOVE_SETTINGS_BYTES && n >= RFM_MOVE_SETTINGS_BYTES) ||
          (b[3] == 1 && (b[4] | b[5] << 8) == RFM_MOVE_SETTINGS_BYTES - 1 && n >= RFM_MOVE_SETTINGS_BYTES - 1)))
        return -1;
    d = *s;
    p = b + 6;
    s->mode = p[0] < 4 ? p[0] : d.mode;
    s->layout = p[1] < 4 ? p[1] : d.layout;
    s->channel = p[2] < 16 ? p[2] : d.channel;
    oct = (int8_t)p[3];
    s->octave = (int8_t)(oct >= RFM_MOVE_OCT_MIN && oct <= RFM_MOVE_OCT_MAX ? oct : d.octave);
    s->scale = p[4] < RFM_MOVE_NSCALES ? p[4] : d.scale;
    s->root = p[5] < 12 ? p[5] : d.root;
    s->bank = p[6] < RFM_MOVE_NBANKS ? p[6] : d.bank;
    p += 7;
    for (i = 0; i < RFM_MOVE_NBANKS * RFM_MOVE_NKNOBS; i++)
        if (p[i] < 128)
            s->cc[i / RFM_MOVE_NKNOBS][i % RFM_MOVE_NKNOBS] = p[i];
    p += RFM_MOVE_NBANKS * RFM_MOVE_NKNOBS;
    for (i = 0; i < RFM_MOVE_NPADS; i++)
        if (p[i] < 128)
            s->drum[i] = p[i];
    p += RFM_MOVE_NPADS;
    s->midi_view = b[3] >= 2 && p[0] < RFM_MOVE_MV_N ? p[0] : RFM_MOVE_MV_PADS;
    return 0;
}

/* ================================================================ actions === */

void emit(rfm_move_t *m, uint8_t kind, int index, int down, int vel, int value)
{
    rfm_move_act_t a;
    memset(&a, 0, sizeof a);
    a.frame = m->seq.frame;
    a.kind = kind, a.index = (uint8_t)index, a.down = (uint8_t)down, a.vel = (uint8_t)vel, a.value = (int16_t)value;
    if (m->fn)
        m->fn(m->ctx, &a);
}
void emit_midi(rfm_move_t *m, int status, int d1, int d2)
{
    rfm_move_act_t a;
    memset(&a, 0, sizeof a);
    a.frame = m->seq.frame;
    a.kind = RFM_MOVE_ACT_MIDI, a.n = 3;
    a.bytes[0] = (uint8_t)status, a.bytes[1] = (uint8_t)(d1 & 127), a.bytes[2] = (uint8_t)(d2 & 127);
    if (m->fn)
        m->fn(m->ctx, &a);
}
void changed(rfm_move_t *m) { emit(m, RFM_MOVE_ACT_SETTINGS, 0, 0, 0, 0); }

/* a key or button held by several Move controls: down with the first, up with the last */
static void key_hold(rfm_move_t *m, int k, int vel)
{
    if (m->key_count[k]++ == 0)
        emit(m, RFM_MOVE_ACT_KEY, k, 1, clampi(vel, 1, 127), 0);
}
static void key_let(rfm_move_t *m, int k)
{
    if (!m->key_count[k] || --m->key_count[k])
        return;
    if (m->key_press[k]) {
        m->key_press[k] = 0;
        emit(m, RFM_MOVE_ACT_PRESS, k, 0, 0, 0);
    }
    emit(m, RFM_MOVE_ACT_KEY, k, 0, 0, 0);
}
static void btn_hold(rfm_move_t *m, int b)
{
    if (m->btn_count[b]++ == 0)
        emit(m, RFM_MOVE_ACT_BTN, b, 1, 0, 0);
}
static void btn_let(rfm_move_t *m, int b)
{
    if (m->btn_count[b] && --m->btn_count[b] == 0)
        emit(m, RFM_MOVE_ACT_BTN, b, 0, 0, 0);
}
void note_hold(rfm_move_t *m, int ch, int n, int vel)
{
    if (m->note_count[ch][n]++ == 0)
        emit_midi(m, 0x90 | ch, n, clampi(vel, 1, 127));
}
void note_let(rfm_move_t *m, int ch, int n)
{
    if (m->note_count[ch][n] && --m->note_count[ch][n] == 0)
        emit_midi(m, 0x80 | ch, n, 0);
}
void let_go(rfm_move_t *m, rfm_move_hold_t *h)
{
    if (h->kind == H_KEY)
        key_let(m, h->a);
    else if (h->kind == H_BTN)
        btn_let(m, h->a);
    else if (h->kind == H_NOTE)
        note_let(m, h->a, h->b);
    h->kind = H_NONE;
}

/* every pad, step and button let go (the track buttons stay as they are) */
static void release_holds(rfm_move_t *m)
{
    int i;
    for (i = 0; i < RFM_MOVE_NPADS; i++) {
        let_go(m, &m->pad[i]);
        m->pad_down[i] = 0, m->pad_press[i] = 0;
    }
    for (i = 0; i < RFM_MOVE_NSTEPS; i++)
        let_go(m, &m->step[i]);
    for (i = 0; i < 128; i++)
        let_go(m, &m->btn[i]);
    m->edit_pad = -1;
    rfmv_seq_ui_release(m);
}

void rfm_move_release_all(rfm_move_t *m)
{
    release_holds(m);
    rfmv_seq_stop(m);                                   /* the link is gone: nothing may keep sounding */
    m->track_held = 0;
    m->touched = -1;
    m->pop = POP_NONE;
}

void rfm_move_set_mode(rfm_move_t *m, int mode)
{
    if (mode < 0 || mode > 3 || mode == m->s.mode)
        return;
    release_holds(m);
    m->mmenu = 0;
    if (m->s.mode == RFM_MOVE_MODE_SEQ)
        rfmv_seq_leave(m);                              /* (the tracks' own layout and CC numbers out, MIDI mode's back) */
    m->s.mode = (uint8_t)mode;
    if (mode == RFM_MOVE_MODE_SEQ)
        rfmv_seq_enter(m);
    m->pop = POP_NONE;
    changed(m);
}

void rfm_move_set_scale(rfm_move_t *m, int scale, int root)
{
    if (scale < 0 || scale >= RFM_MOVE_NSCALES || root < 0 || root > 11)
        return;
    m->s.scale = (uint8_t)scale, m->s.root = (uint8_t)root;
    changed(m);
}

void rfm_move_set_master(rfm_move_t *m, int value) { m->master = (uint16_t)clampi(value, 0, 1023); }

void rfm_move_set_label(rfm_move_t *m, int enc, const char *text)
{
    if (enc < 0 || enc > RFM_MOVE_NENC)
        return;
    snprintf(m->label[enc], sizeof m->label[enc], "%s", text ? text : "");
}

void rfm_move_set_keymap(rfm_move_t *m, const int8_t keymap[6][11])
{
    int r, c;
    memset(m->led_of, -1, sizeof m->led_of);
    for (r = 1; r <= 4; r++)
        for (c = 0; c < 11; c++) {
            const int id = (keymap ? keymap : FM1_KEYMAP)[r][c];
            if (id >= 0 && id < 41)
                m->led_of[id] = (int8_t)(c * 4 + (r - 1));
        }
}

void rfm_move_init(rfm_move_t *m, const rfm_move_settings_t *settings, rfm_move_act_fn fn, void *ctx)
{
    int b, k;
    memset(m, 0, sizeof *m);
    if (settings)
        m->s = *settings;
    else
        rfm_move_settings_default(&m->s);
    m->fn = fn, m->ctx = ctx;
    for (b = 0; b < RFM_MOVE_NBANKS; b++)
        for (k = 0; k < RFM_MOVE_NKNOBS; k++)
            m->cc_val[b][k] = 64;
    m->master = 768;
    m->edit_pad = -1;
    m->touched = -1;
    rfm_move_set_keymap(m, NULL);
    rfmv_seq_init(m);
    if (m->s.mode == RFM_MOVE_MODE_SEQ)
        rfmv_seq_enter(m);
}

/* ================================================================ input === */
/* the top row's first four pads pick the layout, row 2's eight the scale */
int rfmv_pick_layout(rfm_move_t *m, int p)
{
    if (p >= 24 && p < 28) {
        rfmv_menu(m, MENU_LAYOUT, p - 24);
        if (m->s.layout != p - 24) {
            m->s.layout = (uint8_t)(p - 24);
            changed(m);
        }
        return 1;
    }
    if (p >= 16 && p < 16 + RFM_MOVE_NSCALES) {
        rfmv_menu(m, MENU_SCALE, p - 16);
        if (m->s.scale != p - 16) {
            m->s.scale = (uint8_t)(p - 16);
            changed(m);
        }
        return 1;
    }
    return 0;
}

static int midi_mode(const rfm_move_t *m) { return m->s.mode != RFM_MOVE_MODE_FM1; }
int rfmv_track_held(const rfm_move_t *m, int t) { return (m->track_held >> t) & 1; }
int rfmv_half(const rfm_move_t *m)                     /* popups on the left half: the FM-1's screen on the right */
{
    return m->s.mode == RFM_MOVE_MODE_FM1 || (m->s.mode == RFM_MOVE_MODE_MIDI && !m->mmenu && m->s.midi_view == RFM_MOVE_MV_NOTES);
}
int rfmv_seq_trk_held(const rfm_move_t *m) { return m->s.mode == RFM_MOVE_MODE_SEQ && rfmv_track_held(m, 3); }
#define track_held(m, t) rfmv_track_held(m, t)
static int cc_mode(const rfm_move_t *m) { return m->s.mode == RFM_MOVE_MODE_CONTROL || m->s.mode == RFM_MOVE_MODE_SEQ; }

static void on_pad(rfm_move_t *m, int p, int vel, uint32_t now)
{
    rfm_move_hold_t *h = &m->pad[p];
    if (!vel) {                                         /* released */
        if (m->pad_down[p] && m->s.mode == RFM_MOVE_MODE_SEQ)
            rfmv_seq_pad(m, p, 0, now);
        let_go(m, h);
        m->pad_down[p] = 0, m->pad_press[p] = 0;
        if (m->edit_pad == p)
            m->edit_pad = -1;
        rfmv_pop_let(m, POP_DRUM, p);
        return;
    }
    if (m->pad_down[p])
        return;
    m->pad_down[p] = 1;
    h->kind = H_NONE;
    if (track_held(m, 1) || rfmv_seq_trk_held(m) || (m->mmenu && m->s.mode == RFM_MOVE_MODE_MIDI)) {   /* Track 2 held,
                                                         * MIDI mode's Keys & Scales (the sequencer: Track 4): the
                                                         * layout, the scale */
        h->kind = H_MENU;
        rfmv_pick_layout(m, p);
        return;
    }
    if (m->s.mode == RFM_MOVE_MODE_SEQ) {
        rfmv_seq_pad(m, p, vel, now);
        return;
    }
    if (!midi_mode(m)) {
        const int k = PAD_KEY[p];
        if (k >= 0) {
            h->kind = H_KEY, h->a = (uint8_t)k;
            key_hold(m, k, vel);
        }
        return;
    }
    {
        const int n = rfm_move_pad_note(&m->s, p);
        if (n < 0)
            return;
        h->kind = H_NOTE, h->a = m->s.channel, h->b = (uint8_t)n;
        note_hold(m, h->a, n, vel);
        if (m->s.layout == RFM_MOVE_LAYOUT_DRUM)
            m->edit_pad = (int8_t)p;
    }
}

static void on_pad_pressure(rfm_move_t *m, int p, int v)
{
    const rfm_move_hold_t *h = &m->pad[p];
    v = clampi(v, 0, 127);
    if (!m->pad_down[p] || m->pad_press[p] == v)
        return;
    m->pad_press[p] = (uint8_t)v;
    if (h->kind == H_KEY && m->key_press[h->a] != v) {
        m->key_press[h->a] = (uint8_t)v;
        emit(m, RFM_MOVE_ACT_PRESS, h->a, 0, 0, v);
    } else if (h->kind == H_NOTE)
        emit_midi(m, 0xA0 | h->a, h->b, v);
}

static void on_step(rfm_move_t *m, int s, int down, uint32_t now)
{
    rfm_move_hold_t *h = &m->step[s];
    if (!down) {
        if (h->kind == H_SEQ)                           /* (Sequencer mode's step: its release) */
            rfmv_seq_step(m, s, 0, now);
        let_go(m, h);
        return;
    }
    if (h->kind != H_NONE)
        return;
    if (m->s.mode == RFM_MOVE_MODE_MIDI && m->shift_down) {   /* Shift + step 9: Keys & Scales (again: closed) */
        h->kind = H_MENU;
        if (s == 8)
            m->mmenu = (uint8_t)!m->mmenu;
        return;
    }
    if (track_held(m, 1)) {                             /* Track 2 held: steps 1..16 = the MIDI channel */
        h->kind = H_MENU;
        rfmv_menu(m, MENU_CHANNEL, s);
        if (m->s.channel != s) {
            m->s.channel = (uint8_t)s;
            changed(m);
        }
    } else if (!midi_mode(m)) {
        h->kind = H_KEY, h->a = (uint8_t)STEP_KEY[s];
        key_hold(m, h->a, 100);                         /* (steps have no velocity: the FM-1's own default, 100) */
    } else if (m->s.mode == RFM_MOVE_MODE_SEQ) {
        h->kind = H_SEQ;                                /* (a sequencer step press, for its release) */
        rfmv_seq_step(m, s, 1, now);
    }
}

static void on_button(rfm_move_t *m, int cc, int down, uint32_t now)
{
    rfm_move_hold_t *h;
    int b;
    if (cc >= MR_CC_TRACK4 && cc <= MR_CC_TRACK1) {
        const int t = MR_CC_TRACK1 - cc;
        if (down) {
            const int was = m->s.mode;
            rfm_move_set_mode(m, t);
            rfmv_menu(m, MENU_NONE, 0);
            m->track_held = (uint8_t)(m->track_held | 1u << t);
            m->tbtn_ms[t] = now, m->tbtn_sw = (uint8_t)(was != m->s.mode ? m->tbtn_sw | 1u << t : m->tbtn_sw & ~(1u << t));
        } else if (m->track_held >> t & 1) {
            m->track_held = (uint8_t)(m->track_held & ~(1u << t));
            if (now - m->tbtn_ms[t] < RFM_MOVE_TAP_MS && m->menu_what == MENU_NONE && m->s.mode == t) {   /* a tap (no
                                                         * menu used): the mode named; again in a mode: its next
                                                         * screen (Track 2: MIDI mode's) */
                if (m->tbtn_sw >> t & 1)
                    rfmv_pop(m, POP_MODE, t);
                else if (t == RFM_MOVE_MODE_MIDI) {
                    m->s.midi_view = (uint8_t)((m->s.midi_view + 1) % RFM_MOVE_MV_N);
                    changed(m);
                    rfmv_pop(m, POP_MODE, 10 + m->s.midi_view);
                }
            }
        }
        return;
    }
    if (m->s.mode == RFM_MOVE_MODE_SEQ && cc >= 0 && cc < 128 && (m->btn[cc].kind == H_NONE || m->btn[cc].kind == H_MENU)) {
        if (!down && m->btn[cc].kind == H_MENU) {
            m->btn[cc].kind = H_NONE;
            rfmv_seq_button(m, cc, 0, now);
            return;
        }
        if (down && m->btn[cc].kind == H_NONE && rfmv_seq_button(m, cc, 1, now)) {
            if (m->s.mode == RFM_MOVE_MODE_SEQ)         /* (Back leaves the mode) */
                m->btn[cc].kind = H_MENU;
            return;
        }
    }
    if (cc == MR_CC_SHIFT)
        m->shift_down = (uint8_t)down;
    if (m->s.mode == RFM_MOVE_MODE_MIDI) {              /* MIDI mode: Shift is the Move's (Shift + step 9: Keys &
                                                         * Scales), Back closes that menu */
        if (cc == MR_CC_SHIFT)
            return;
        if (cc == MR_CC_BACK && m->mmenu) {
            if (down)
                m->mmenu = 0;
            return;
        }
    }
    if (cc == MR_CC_UP || cc == MR_CC_DOWN) {
        const int o = clampi(m->s.octave + (cc == MR_CC_UP ? 1 : -1), RFM_MOVE_OCT_MIN, RFM_MOVE_OCT_MAX);
        if (down && midi_mode(m) && o != m->s.octave) {
            m->s.octave = (int8_t)o;
            changed(m);
        }
        return;
    }
    if (cc < 0 || cc > 127 || (b = rfm_move_button_of(cc)) < 0)
        return;
    h = &m->btn[cc];
    if (!down) {
        let_go(m, h);
        return;
    }
    if (h->kind != H_NONE)
        return;
    h->kind = H_BTN, h->a = (uint8_t)b;
    btn_hold(m, b);
}

static void on_knob(rfm_move_t *m, int k, int d, uint32_t now)
{
    if (!d)
        return;
    if (k == 8 && m->s.mode == RFM_MOVE_MODE_SEQ && rfmv_seq_volume(m, d))
        return;                                         /* (Sequencer mode, a step held: its velocity) */
    if (k == 8) {                                       /* volume: MASTER */
        const int v = clampi(m->master + d * RFM_MOVE_MASTER_STEP, 0, 1023);
        rfmv_pop(m, POP_MASTER, 8);
        if (v != m->master) {
            m->master = (uint16_t)v;
            emit(m, RFM_MOVE_ACT_MASTER, 0, 0, 0, v);
        }
        return;
    }
    if (k < 0 || k >= RFM_MOVE_NKNOBS)
        return;
    if (m->s.mode == RFM_MOVE_MODE_SEQ) {
        rfmv_seq_knob(m, k, d, now);
        return;
    }
    if (m->s.mode == RFM_MOVE_MODE_CONTROL) {
        const int b = m->s.bank;
        if (track_held(m, 2)) {                         /* Track 3 held: the knob's CC number */
            const int c = clampi(m->s.cc[b][k] + d, 0, 127);
            rfmv_menu(m, MENU_CC, k);
            if (c != m->s.cc[b][k]) {
                m->s.cc[b][k] = (uint8_t)c;
                changed(m);
            }
        } else {
            const int v = clampi(m->cc_val[b][k] + d, 0, 127);
            if (v != m->cc_val[b][k]) {
                m->cc_val[b][k] = (uint8_t)v;
                emit_midi(m, 0xB0 | m->s.channel, m->s.cc[b][k], v);
            }
            rfmv_pop(m, POP_CC, k);
        }
        return;
    }
    if (KNOB_ENC[k] >= 0) {                            /* (the popup counts this gesture's detents) */
        const int acc = m->pop == POP_ENC && m->pop_i == k && (m->touched == k || rfmv_pop_up(m, POP_ENC)) ? m->pop_val : 0;
        emit(m, RFM_MOVE_ACT_TURN, KNOB_ENC[k], 0, 0, d);
        rfmv_pop(m, POP_ENC, k);
        m->pop_val = (int16_t)clampi(acc + d, -9999, 9999);
    }
}

static void on_jog(rfm_move_t *m, int d, uint32_t now)
{
    if (!d)
        return;
    if (track_held(m, 1) || (m->mmenu && m->s.mode == RFM_MOVE_MODE_MIDI)) {   /* Track 2 held, Keys & Scales: the
                                                         * root */
        const int r = ((m->s.root + d) % 12 + 12) % 12;
        rfmv_menu(m, MENU_ROOT, r);
        if (r != m->s.root) {
            m->s.root = (uint8_t)r;
            changed(m);
        }
        return;
    }
    if (m->s.mode == RFM_MOVE_MODE_SEQ) {
        rfmv_seq_jog(m, d, now);
        return;
    }
    if (midi_mode(m) && m->s.layout == RFM_MOVE_LAYOUT_DRUM && m->edit_pad >= 0 && m->pad_down[m->edit_pad]) {
        const int p = m->edit_pad, n = clampi(m->s.drum[p] + d, 0, 127);
        if (n != m->s.drum[p]) {
            m->s.drum[p] = (uint8_t)n;
            changed(m);
        }
        rfmv_pop(m, POP_DRUM, p);
        return;
    }
    if (m->s.mode == RFM_MOVE_MODE_CONTROL) {
        const int b = clampi(m->s.bank + d, 0, RFM_MOVE_NBANKS - 1);
        if (b != m->s.bank) {
            m->s.bank = (uint8_t)b;
            changed(m);
        }
        return;
    }
    emit(m, RFM_MOVE_ACT_TURN, RFM_MOVE_E_SELECT, 0, 0, d);
    {                                                   /* the drawing's SELECT lights; FM-1 mode and MIDI's NOTES: a
                                                         * left-half popup while turning (this gesture's detents: from 0
                                                         * once it has timed out) */
        const int acc = rfmv_pop_up(m, POP_SELECT) ? m->pop_val : 0;
        rfmv_pop(m, POP_SELECT, 9);
        m->pop_val = (int16_t)clampi(acc + d, -9999, 9999);
    }
}

/* a touch: the knob's popup (in FM-1 mode the drawing marks the knob instead, and a turn brings the popup) */
static void on_touch(rfm_move_t *m, int k, int on)
{
    if (!on) {
        if (m->touched == k)
            m->touched = -1;
        if (rfmv_half(m) && (m->pop == POP_ENC || m->pop == POP_MASTER) && m->pop_i == k) {
            m->pop = POP_NONE, m->pop_val = 0;          /* (FM-1 mode, MIDI's NOTES: the release ends the gesture: the
                                                         * popup goes, its count starts again) */
            return;
        }
        if (m->pop == POP_ENC || m->pop == POP_MASTER || m->pop == POP_CC || m->pop == POP_TRIG)
            rfmv_pop_let(m, m->pop, k);
        return;
    }
    m->touched = (int8_t)k;
    if (rfmv_half(m) || rfmv_track_held(m, 2))         /* (FM-1 mode, MIDI's NOTES: a touch alone shows the drawing's
                                                         * highlight; the popup comes with turning) */
        return;
    if (k == 8)
        rfmv_pop(m, POP_MASTER, 8);
    else if (k < RFM_MOVE_NKNOBS && m->s.mode == RFM_MOVE_MODE_SEQ && m->seq.trig && !m->seq.view)
        rfmv_pop(m, POP_TRIG, k);
    else if (k < RFM_MOVE_NKNOBS && cc_mode(m))
        rfmv_pop(m, POP_CC, k);
    else if (k < RFM_MOVE_NKNOBS && KNOB_ENC[k] >= 0) {
        rfmv_pop(m, POP_ENC, k);
        m->pop_val = 0;
    }
}

void rfm_move_input(rfm_move_t *m, const mr_event_t *ev, uint32_t now_ms)
{
    switch (ev->type) {
    case MR_EV_PAD:
        if (ev->index >= 0 && ev->index < RFM_MOVE_NPADS)
            on_pad(m, ev->index, clampi(ev->value, 0, 127), now_ms);
        break;
    case MR_EV_PAD_PRESSURE:
        if (ev->index >= 0 && ev->index < RFM_MOVE_NPADS)
            on_pad_pressure(m, ev->index, ev->value);
        break;
    case MR_EV_STEP:
        if (ev->index >= 0 && ev->index < RFM_MOVE_NSTEPS)
            on_step(m, ev->index, ev->value != 0, now_ms);
        break;
    case MR_EV_BUTTON: on_button(m, ev->index, ev->value != 0, now_ms); break;
    case MR_EV_KNOB: on_knob(m, ev->index, ev->value, now_ms); break;
    case MR_EV_JOG: on_jog(m, ev->value, now_ms); break;
    case MR_EV_JOG_CLICK:                               /* the sequencer: FILL while the wheel is held down */
        m->seq.fill = (uint8_t)(m->s.mode == RFM_MOVE_MODE_SEQ && ev->value != 0);
        break;
    case MR_EV_TOUCH:
        if (ev->index >= 0 && ev->index <= 9)
            on_touch(m, ev->index, ev->value != 0);
        break;
    default: break;                                     /* jog click (the FM-1 has no encoder push), jacks, MIDI .. */
    }
}

/* ================================================================ LEDs === */
void rfm_move_panel(rfm_move_t *m, const uint8_t levels[RFM_MOVE_NLEVELS], uint8_t play_green)
{
    memcpy(m->levels, levels, RFM_MOVE_NLEVELS);
    m->play_green = play_green;
}

/* the FM-1's LED level 0..255 as a brightness 0..256 (panel.js ledAlpha: the 5 levels' curve) */
static int alpha(int level)
{
    static const int LX[5] = { 0, 12, 40, 96, 255 }, LY[5] = { 0, 51, 97, 159, 256 };
    int i;
    level = clampi(level, 0, 255);
    for (i = 1; i < 5; i++)
        if (level <= LX[i])
            return LY[i - 1] + (LY[i] - LY[i - 1]) * (level - LX[i - 1]) / (LX[i] - LX[i - 1]);
    return 256;
}
static int level_of(const rfm_move_t *m, int id)
{
    const int i = id >= 0 && id < 41 ? m->led_of[id] : -1;
    return i >= 0 ? m->levels[i] : 0;
}
int rfmv_level(const rfm_move_t *m, int id) { return level_of(m, id); }

const rgb_t MODE_RGB[4] = { { 0xc4, 0x3a, 0xff }, { 0xc8, 0xff, 0x2a }, { 0x1a, 0xe4, 0xff }, { 0xff, 0x2a, 0x2a } };
const rgb_t BANK_RGB[4] = { { 0x1a, 0xe4, 0xff }, { 0xff, 0xb0, 0x2a }, { 0xc4, 0x3a, 0xff }, { 0xc8, 0xff, 0x2a } };
const rgb_t WHITE = { 255, 255, 255 }, GREEN = { 0, 255, 0 }, RED = { 255, 0, 0 };
const rgb_t OFF = { 0, 0, 0 };
static const rgb_t PINK = { 0xff, 0x7a, 0xa8 };
#define FLOOR 24                                        /* a mapped key's glow while its FM-1 LED is off (/256) */
#define DIM 32                                          /* an unselected choice (/256) */

rgb_t sc(rgb_t c, int f)
{
    rgb_t o;
    f = clampi(f, 0, 256);
    o.r = (uint8_t)(c.r * f / 256), o.g = (uint8_t)(c.g * f / 256), o.b = (uint8_t)(c.b * f / 256);
    return o;
}
rgb_t add(rgb_t a, rgb_t b)
{
    rgb_t o;
    o.r = (uint8_t)clampi(a.r + b.r, 0, 255), o.g = (uint8_t)clampi(a.g + b.g, 0, 255);
    o.b = (uint8_t)clampi(a.b + b.b, 0, 255);
    return o;
}

/* the LED list: index -> (kind, number) */
static const uint8_t WHITE_CC[13] = { MR_CC_SHIFT, MR_CC_MENU, MR_CC_BACK, MR_CC_CAPTURE, MR_CC_DOWN, MR_CC_UP,
                                      MR_CC_UNDO, MR_CC_LOOP, MR_CC_COPY, MR_CC_LEFT, MR_CC_RIGHT, MR_CC_MUTE,
                                      MR_CC_DELETE };

static void led_id(int i, rfm_move_led_t *l)
{
    memset(l, 0, sizeof *l);
    l->rgb = 1;
    if (i < L_STEP0)
        l->cc = 0, l->number = (uint8_t)(MR_NOTE_PAD0 + i);
    else if (i < L_PLAY)
        l->cc = 0, l->number = (uint8_t)(MR_NOTE_STEP0 + i - L_STEP0);
    else if (i == L_PLAY)
        l->cc = 1, l->number = MR_CC_PLAY;
    else if (i == L_REC)
        l->cc = 1, l->number = MR_CC_REC;
    else if (i == L_SAMPLE)
        l->cc = 1, l->number = MR_CC_SAMPLE;
    else if (i < L_RING0)
        l->cc = 1, l->number = (uint8_t)(MR_CC_TRACK1 - (i - L_TRACK0));
    else if (i < L_WHITE0)
        l->cc = 1, l->number = (uint8_t)(MR_CC_KNOB1 + i - L_RING0);
    else
        l->rgb = 0, l->cc = 1, l->number = WHITE_CC[i - L_WHITE0];
}

/* a MIDI mode's pad colour */
rgb_t midi_pad(const rfm_move_t *m, int p, rgb_t mode)
{
    if (rfm_move_pad_note(&m->s, p) >= 0 && m->pad_down[p])
        return WHITE;
    return rfmv_layout_pad(m, p, mode);
}
rgb_t rfmv_layout_pad(const rfm_move_t *m, int p, rgb_t mode)
{
    const int n = rfm_move_pad_note(&m->s, p);
    int rel, i;
    const uint8_t *scl = SCALES[m->s.scale < RFM_MOVE_NSCALES ? m->s.scale : 0];
    if (n < 0)
        return OFF;
    rel = ((n - m->s.root) % 12 + 12) % 12;
    if (rel == 0)
        return mode;
    if (m->s.layout == RFM_MOVE_LAYOUT_DRUM)
        return sc(WHITE, 90);
    for (i = 0; i < scl[0]; i++)
        if (scl[1 + i] == rel)
            return sc(WHITE, 90);
    return sc(WHITE, 12);
}

static void wanted(const rfm_move_t *m, uint8_t w[RFM_MOVE_NLEDS][3])
{
    const rgb_t mode = MODE_RGB[m->s.mode & 3];
    const int t2 = track_held(m, 1) || (m->mmenu && m->s.mode == RFM_MOVE_MODE_MIDI), fm1 = !midi_mode(m);
    const rgb_t SCALE_RGB = { 0, 190, 255 };            /* (the scale pads: their own colour, cyan) */
    rgb_t c[L_WHITE0];
    uint8_t wv[13];
    int i;
    for (i = 0; i < RFM_MOVE_NPADS; i++) {
        if (t2)
            c[i] = i >= 24 && i < 28 ? sc(mode, m->s.layout == i - 24 ? 256 : 64)
                 : i >= 16 && i < 16 + RFM_MOVE_NSCALES ? sc(SCALE_RGB, m->s.scale == i - 16 ? 256 : 56) : OFF;
        else if (fm1) {
            const int k = PAD_KEY[i];
            const int lv = k >= 0 ? alpha(level_of(m, 14 + k)) : 0;
            c[i] = k < 0 ? OFF : m->pad_down[i] ? mode : sc((0x54A >> ((k + 5) % 12)) & 1 ? PINK : WHITE, lv > FLOOR ? lv : FLOOR);
        } else
            c[i] = midi_pad(m, i, mode);
    }
    for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
        rgb_t s = OFF;
        if (t2 && m->mmenu && !track_held(m, 1))
            s = i == 8 ? WHITE : OFF;                   /* (Keys & Scales: step 9) */
        else if (t2)
            s = m->s.channel == i ? mode : sc(WHITE, 16);
        else if (fm1) {
            const int lv = alpha(level_of(m, 14 + STEP_KEY[i]));
            s = m->step[i].kind == H_KEY ? mode : sc(WHITE, lv > FLOOR ? lv : FLOOR);
        }
        c[L_STEP0 + i] = s;
    }
    c[L_PLAY] = add(sc(WHITE, alpha(level_of(m, RFM_MOVE_B_PLAY))), sc(GREEN, alpha(m->play_green)));
    c[L_REC] = sc(RED, alpha(level_of(m, RFM_MOVE_B_REC)));
    c[L_SAMPLE] = sc(WHITE, alpha(level_of(m, RFM_MOVE_B_SEL)));
    for (i = 0; i < 4; i++)
        c[L_TRACK0 + i] = sc(MODE_RGB[i], m->s.mode == i ? 256 : DIM / 2);
    for (i = 0; i < RFM_MOVE_NKNOBS; i++) {
        rgb_t r = OFF;
        if (m->s.mode == RFM_MOVE_MODE_CONTROL) {
            const rgb_t bc = BANK_RGB[m->s.bank & 3];
            r = m->touched == i ? bc : sc(bc, 16 + m->cc_val[m->s.bank][i] * 240 / 127);
        } else if (KNOB_ENC[i] >= 0)
            r = sc(mode, m->touched == i ? 256 : 64);
        c[L_RING0 + i] = r;
    }
    for (i = 0; i < 13; i++) {
        const int cc = WHITE_CC[i], b = rfm_move_button_of(cc);
        int v = 0;
        if (cc == MR_CC_UP || cc == MR_CC_DOWN)
            v = !midi_mode(m) ? 0 : (cc == MR_CC_UP ? m->s.octave > 0 : m->s.octave < 0) ? 127 : 24;
        else if (b >= 0)
            v = clampi(alpha(level_of(m, b)) * 127 / 256, 6, 127);
        wv[i] = (uint8_t)v;
    }
    if (m->s.mode == RFM_MOVE_MODE_SEQ && !t2)         /* Sequencer mode takes over all but the Track LEDs */
        rfmv_seq_leds(m, c, wv);
    for (i = 0; i < L_WHITE0; i++)
        w[i][0] = c[i].r, w[i][1] = c[i].g, w[i][2] = c[i].b;
    for (i = 0; i < 13; i++)
        w[L_WHITE0 + i][0] = wv[i], w[L_WHITE0 + i][1] = 0, w[L_WHITE0 + i][2] = 0;
}

int rfm_move_leds(rfm_move_t *m, rfm_move_led_t *out, int max)
{
    uint8_t w[RFM_MOVE_NLEDS][3];
    int i, n = 0;
    wanted(m, w);
    for (i = 0; i < RFM_MOVE_NLEDS && n < max; i++) {
        if (m->sent_ok[i] && !memcmp(m->sent[i], w[i], 3))
            continue;
        memcpy(m->sent[i], w[i], 3);
        m->sent_ok[i] = 1;
        led_id(i, &out[n]);
        out[n].r = w[i][0], out[n].g = w[i][1], out[n].b = w[i][2];
        n++;
    }
    return n;
}

void rfm_move_leds_invalidate(rfm_move_t *m) { memset(m->sent_ok, 0, sizeof m->sent_ok); }
