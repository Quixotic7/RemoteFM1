/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's screen: the owner's design (host/src/move/README.md, "The screen"). The Move's screen is small, so at
 * rest every screen is symbols and pictures only; words come in large popups while a knob is touched or turned, a
 * step or a project pad is held (and RFM_MOVE_POPUP_MS after), or for a moment after an action (the sequencer's
 * messages). FM-1 mode is the owner's pixel drawing of the FM-1 (live: touched encoders, lit buttons, held keys) and
 * the FM-1's screen; MIDI mode the pad map; Control mode the banks and the 8 values; the sequencer's screens are
 * seq_ui.c's. Text is font5x7 at 2x (4x for the big numbers). */
#include <stdio.h>
#include <string.h>

#include "move_int.h"
#include "../../third_party/moveremote/common/font5x7.h"

#define clampi rfmv_clampi

/* ================================================================ drawing === */
static void fill(mr_canvas_t *cv, int x, int y, int w, int h, int on) { mr_gfx_rect(cv, x, y, w, h, on, 1); }
static void box(mr_canvas_t *cv, int x, int y, int w, int h) { mr_gfx_rect(cv, x, y, w, h, 1, 0); }

int rfmv_big_w(const char *s, int k)
{
    int w = 0;
    for (; *s; s++)
        w += *s == '.' ? 2 * k : MR_FONT_ADV * k;
    return w ? w - k : 0;
}

int rfmv_big(mr_canvas_t *cv, int x, int y, const char *s, int k, int on)
{
    for (; *s; s++) {
        const uint8_t *g = mr_glyph(*s);
        int i, j;
        if (*s == '.') {                                /* a narrow point: one font pixel, then the gap */
            fill(cv, x, y + 6 * k, k, k, on);
            x += 2 * k;
            continue;
        }
        for (i = 0; i < MR_FONT_W; i++)
            for (j = 0; j < MR_FONT_H; j++)
                if (g[i] >> j & 1)
                    fill(cv, x + i * k, y + j * k, k, k, on);
        x += MR_FONT_ADV * k;
    }
    return x - k;
}

/* text at k x, at most w pixels wide (the characters that fit) */
static int big_fit(mr_canvas_t *cv, int x, int y, const char *s, int k, int w, int on)
{
    char b[24];
    int n;
    snprintf(b, sizeof b, "%s", s);
    for (n = (int)strlen(b); n > 0 && rfmv_big_w(b, k) > w; n--)
        b[n - 1] = 0;
    return rfmv_big(cv, x, y, b, k, on);
}

static void big_mid(mr_canvas_t *cv, int cx, int y, const char *s, int k, int on)
{
    rfmv_big(cv, cx - rfmv_big_w(s, k) / 2, y, s, k, on);
}

void rfmv_dotted(mr_canvas_t *cv, int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < w; i += 2)
        mr_gfx_pixel(cv, x + i, y, 1), mr_gfx_pixel(cv, x + i, y + h - 1, 1);
    for (i = 0; i < h; i += 2)
        mr_gfx_pixel(cv, x, y + i, 1), mr_gfx_pixel(cv, x + w - 1, y + i, 1);
}

/* the CSS triangle (border-left w, top / bottom h / 2 transparent): row j is w x (1 - |centre - h/2| / (h/2)) long */
void rfmv_tri(mr_canvas_t *cv, int x, int y, int w, int h, int on)
{
    int j;
    for (j = 0; j < h; j++) {
        const int d = 2 * j + 1 - h, len = (w * (h - (d < 0 ? -d : d)) + h / 2) / h;
        fill(cv, x, y + j, len, 1, on);
    }
}

/* a circle (border-radius 50 %) in the d x d box at x, y: pixel centres within d / 2 */
void rfmv_disc(mr_canvas_t *cv, int x, int y, int d, int on)
{
    int i, j;
    for (j = 0; j < d; j++)
        for (i = 0; i < d; i++) {
            const int dx = 2 * i + 1 - d, dy = 2 * j + 1 - d;
            if (dx * dx + dy * dy <= d * d)
                mr_gfx_pixel(cv, x + i, y + j, on);
        }
}

void rfmv_frame(mr_canvas_t *cv, int x, int y, int w, int h)
{
    fill(cv, x, y, w, h, 0);
    box(cv, x, y, w, h);
}

/* the 45-degree hatching the knob popup has behind it (1 px lines about 4 px apart) */
static void hatch(mr_canvas_t *cv, int x, int y, int w, int h)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            mr_gfx_pixel(cv, i, j, ((i - j) % 6 + 6) % 6 == 0);
}

/* sin of a multiple of 5 degrees, x 1024 */
static int isin5(int deg)
{
    static const int16_t S[19] = { 0, 89, 178, 265, 350, 433, 512, 587, 658, 724, 784, 839, 887, 928, 962, 989, 1008, 1020, 1024 };
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 90) return S[deg / 5];
    if (deg <= 180) return S[(180 - deg) / 5];
    if (deg <= 270) return -S[(deg - 180) / 5];
    return -S[(360 - deg) / 5];
}

/* the popup's dial (KnobPopup: a 22 px disc, a 2 x 9 px black pointer from its centre); deg 0 = up, clockwise */
static void dial(mr_canvas_t *cv, int deg)
{
    const int cx = 108, cy = 18;
    int t;
    deg = (deg >= 0 ? deg + 2 : deg - 2) / 5 * 5;
    rfmv_disc(cv, 97, 7, 22, 1);
    for (t = 0; t <= 18; t++) {                         /* half pixels out to 9 */
        const int X = cx * 1024 + t * isin5(deg) / 2, Y = cy * 1024 - t * isin5(deg + 90) / 2;
        fill(cv, (X >> 10) - 1, (Y >> 10) - 1, 2, 2, 0);
    }
}

const char *rfmv_note_name(int n, char *b, int cap)
{
    snprintf(b, (size_t)cap, "%s%d", rfmv_note_names[n % 12], n / 12 - 1);
    return b;
}

/* ================================================================ popups === */
uint32_t rfmv_now_ms(const rfm_move_t *m) { return (uint32_t)(m->seq.now_us / 1000u); }

void rfmv_pop(rfm_move_t *m, int kind, int i)
{
    m->pop = (uint8_t)kind, m->pop_i = (uint8_t)i, m->pop_ms = rfmv_now_ms(m);
}

void rfmv_pop_let(rfm_move_t *m, int kind, int i)
{
    if (m->pop == kind && m->pop_i == i)
        m->pop_ms = rfmv_now_ms(m);
}

void rfmv_menu(rfm_move_t *m, int what, int i) { m->menu_what = (uint8_t)what, m->menu_i = (uint8_t)i; }

static int pad_of_slot(int slot) { return (3 - slot / 8) * 8 + slot % 8; }

/* the popup's control is still held (touched): it stays up */
static int pop_held(const rfm_move_t *m)
{
    const int i = m->pop_i;
    switch (m->pop) {
    case POP_ENC:
    case POP_MASTER:
    case POP_CC: return m->touched == i;
    case POP_SELECT: return m->touched == 9;
    case POP_DRUM: return i < RFM_MOVE_NPADS && m->pad_down[i];
    case POP_LOOP: return (m->seq.mods & SEQ_M_LOOP) != 0;
    case POP_STEP: return i < RFM_MOVE_NSTEPS && (m->seq.step_held >> i & 1);
    case POP_PROJ: return i < RFM_MOVE_PROJECTS && m->pad_down[pad_of_slot(i)];
    default: return 0;
    }
}

int rfmv_pop_up(const rfm_move_t *m, int kind)
{
    return m->pop && m->pop == kind && (pop_held(m) || rfmv_now_ms(m) - m->pop_ms < RFM_MOVE_POPUP_MS);
}

/* KnobPopup: the label and the value at 2x (the unit small), a thick bar, a dial; the right half hatched behind.
 * frac 0..1000 fills the bar from the left; centred (an FM-1 encoder's relative turn): -1000..1000 from the middle */
static void knob_popup(mr_canvas_t *cv, const char *label, const char *value, const char *unit, int frac, int centred,
                       int deg)
{
    int w;
    mr_gfx_clear(cv, 0);
    hatch(cv, 64, 0, 64, 64);
    rfmv_frame(cv, 2, 2, 124, 60);
    if (rfmv_big_w(label, 2) <= 88)
        rfmv_big(cv, 7, 6, label, 2, 1);
    else {                                              /* 8 characters: 11 px each; more: the first 8 */
        char b[9];
        int i;
        snprintf(b, sizeof b, "%s", label);
        for (i = 0; b[i]; i++) {
            const char c[2] = { b[i], 0 };
            rfmv_big(cv, 7 + 11 * i, 6, c, 2, 1);
        }
    }
    w = big_fit(cv, 7, 24, value, 2, 110, 1) - 7;
    if (unit && *unit)
        mr_gfx_text(cv, w <= 50 ? 60 : 7 + w + 4, 31, unit, 1);
    box(cv, 7, 45, 114, 10);
    if (!centred)
        fill(cv, 9, 47, clampi(frac, 0, 1000) * 110 / 1000, 6, 1);
    else {
        const int d = clampi(frac, -1000, 1000) * 55 / 1000;
        fill(cv, d >= 0 ? 64 : 64 + d, 47, d >= 0 ? d : -d, 6, 1);
        fill(cv, 64, 46, 1, 8, 1);                      /* (the middle) */
    }
    dial(cv, deg);
}

static const char *enc_label(const rfm_move_t *m, int e)
{
    return e >= 0 && e <= RFM_MOVE_NENC && m->label[e][0] ? m->label[e] : rfm_move_enc_name(e);
}

/* the knob popups' contents, by kind */
static void value_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    char l[24], v[24], u[8] = "";
    const int i = m->pop_i;
    int frac = 0, centred = 0, deg;
    switch (m->pop) {
    case POP_ENC: {
        const int e = rfm_move_knob_enc(i), t = m->pop_val;
        snprintf(l, sizeof l, "%s", enc_label(m, e));
        snprintf(v, sizeof v, t ? "%+d" : "%d", t);
        frac = clampi(t, -32, 32) * 1000 / 32, centred = 1;
        deg = t * 15;                                   /* (relative: the dial turns with the detents) */
        knob_popup(cv, l, v, u, frac, centred, ((deg % 360) + 540) % 360 - 180);
        return;
    }
    case POP_MASTER:
        snprintf(l, sizeof l, "%s", enc_label(m, RFM_MOVE_E_MASTER));
        snprintf(v, sizeof v, "%d", (m->master * 100 + 511) / 1023);
        snprintf(u, sizeof u, "%%");
        frac = m->master * 1000 / 1023;
        break;
    case POP_CC: {
        const int b = m->s.bank, cc = m->s.cc[b][i & 7];
        int val = m->cc_val[b][i & 7];
        snprintf(l, sizeof l, "CC %d", cc);
        if (m->s.mode == RFM_MOVE_MODE_SEQ && !q->view && q->step_first >= 0) {   /* a held step's lock */
            const int clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
            val = rfmv_seq_lock_get(m, clip, q->page * RFM_MOVE_SEQ_PAGE + q->step_first, cc);
            snprintf(u, sizeof u, "LOCK");
        }
        if (val < 0)
            snprintf(v, sizeof v, "--"), val = 0;
        else
            snprintf(v, sizeof v, "%d", val);
        frac = val * 1000 / 127;
        break;
    }
    case POP_DRUM: {
        const int n = m->s.drum[i & 31];
        snprintf(l, sizeof l, "DRUM");
        rfmv_note_name(n, v, sizeof v);
        snprintf(u, sizeof u, "%d", n);
        frac = n * 1000 / 127;
        break;
    }
    case POP_TEMPO: {
        const int b10 = rfm_move_seq_bpm10(m);
        snprintf(l, sizeof l, "TEMPO");
        snprintf(v, sizeof v, b10 % 10 ? "%d.%d" : "%d", b10 / 10, b10 % 10);
        snprintf(u, sizeof u, "BPM");
        frac = (b10 - RFM_MOVE_SEQ_BPM10_MIN) * 1000 / (RFM_MOVE_SEQ_BPM10_MAX - RFM_MOVE_SEQ_BPM10_MIN);
        break;
    }
    case POP_LOOP: {
        const int len = q->len[SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel)];
        snprintf(l, sizeof l, "LOOP");
        snprintf(v, sizeof v, "%d", len % RFM_MOVE_SEQ_PAGE ? len : len / RFM_MOVE_SEQ_PAGE);
        snprintf(u, sizeof u, "%s", len % RFM_MOVE_SEQ_PAGE ? "STEPS" : len == RFM_MOVE_SEQ_PAGE ? "BAR" : "BARS");
        frac = len * 1000 / RFM_MOVE_SEQ_MAXLEN;
        break;
    }
    default:                                            /* POP_BANK */
        snprintf(l, sizeof l, "BANK");
        snprintf(v, sizeof v, "%d", m->s.bank + 1);
        frac = (m->s.bank + 1) * 1000 / RFM_MOVE_NBANKS;
        break;
    }
    knob_popup(cv, l, v, u, frac, centred, -135 + frac * 270 / 1000);
}

/* StepPopup: the held step's notes as inverted chips (the first two and "+n" past three), its velocity (a level
 * symbol) and its length (a bracket) at 2x, over track view (its steps row stays, the step marked above it) */
static void step_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int s = m->pop_i, clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
    const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
    int lo, hi = rfmv_seq_range(m, clip, abs, &lo), n = hi - lo, i;
    char b[16];
    fill(cv, 0, 0, 128, 54, 0);                         /* (track view's steps row stays below) */
    rfmv_frame(cv, 2, 2, 124, 48);
    for (i = 0; i < n && i < 3; i++) {
        const int x = 6 + i * 40;
        fill(cv, x, 6, 36, 18, 1);
        if (i == 2 && n > 3)
            snprintf(b, sizeof b, "+%d", n - 2);
        else
            rfmv_note_name(q->notes[lo + i].note, b, sizeof b);
        if (rfmv_big_w(b, 2) <= 34)
            big_mid(cv, x + 18, 8, b, 2, 0);
        else
            mr_gfx_text(cv, x + 18 - 6 * (int)strlen(b) / 2, 11, b, 0);   /* (C#-1: too wide at 2x) */
    }
    if (n) {
        const rfm_move_note_t *n0 = &q->notes[lo];
        const int h = (n0->vel * 14 + 63) / 127, len = n0->len, st = len / RFM_MOVE_SEQ_TPS;
        int w;
        fill(cv, 7, 44 - h, 10, h, 1);                  /* velocity: a level, filled from the bottom */
        box(cv, 7, 30, 10, 14);
        snprintf(b, sizeof b, "%d", n0->vel);
        rfmv_big(cv, 21, 29, b, 2, 1);
        fill(cv, 66, 36, 26, 3, 1);                     /* length: a bracket */
        fill(cv, 66, 32, 1, 11, 1), fill(cv, 91, 32, 1, 11, 1);
        if (len % RFM_MOVE_SEQ_TPS && st < 10)
            snprintf(b, sizeof b, "%d.%d", st, len % RFM_MOVE_SEQ_TPS * 10 / RFM_MOVE_SEQ_TPS);
        else
            snprintf(b, sizeof b, "%d", (len + RFM_MOVE_SEQ_TPS / 2) / RFM_MOVE_SEQ_TPS);
        w = rfmv_big_w(b, 2);
        rfmv_big(cv, 98 + w <= 124 ? 98 : 124 - w, 29, b, 2, 1);
    }
    fill(cv, s * 8 - 2, 52, 11, 1, 1);                  /* the held step, over the row */
}

/* ProjectPopup: the number at 4x, a bar a track (its notes), the tempo, the clips */
static int bits_of(int n)
{
    int b = 0;
    while (n > 0)
        b++, n >>= 1;
    return b;
}
static void project_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    rfm_move_proj_info_t in;
    char b[16];
    int i, nt = 8;
    rfmv_proj_info(m, m->pop_i, &in);
    rfmv_frame(cv, 2, 2, 124, 60);
    snprintf(b, sizeof b, "%02d", m->pop_i + 1);
    rfmv_big(cv, 7, 8, b, 4, 1);
    if (!in.ok)
        return;
    for (i = 8; i < RFM_MOVE_SEQ_TRACKS; i++)
        if (in.notes[i])
            nt = 16;                                    /* (tracks 9..16 in use: 16 thinner bars) */
    for (i = 0; i < nt; i++) {
        const int h = in.notes[i] ? clampi(5 * bits_of(in.notes[i]) + 1, 1, 26) : 1;   /* 1, 2, 4 .. notes: 6, 11, 16 .. px */
        if (nt == 8)
            fill(cv, 58 + i * 8, 36 - h, 5, h, 1);
        else
            fill(cv, 58 + i * 4, 36 - h, 3, h, 1);
    }
    rfmv_tri(cv, 7, 45, 9, 12, 1);
    snprintf(b, sizeof b, "%d", (in.bpm10 + 5) / 10);
    rfmv_big(cv, 21, 43, b, 2, 1);
    box(cv, 76, 45, 12, 12);
    fill(cv, 79, 48, 6, 6, 1);
    snprintf(b, sizeof b, "%d", in.clips);
    rfmv_big(cv, 92, 43, b, 2, 1);
}

/* a message (the sequencer's: UNDO, COPIED ..) at 2x, a line or two */
static void message_popup(mr_canvas_t *cv, const char *msg)
{
    char a[24], b[24];
    const char *sp = strchr(msg, ' ');
    rfmv_frame(cv, 2, 2, 124, 60);
    if (rfmv_big_w(msg, 2) <= 118 || !sp) {
        big_mid(cv, 64, 25, msg, 2, 1);
        return;
    }
    snprintf(a, sizeof a, "%.*s", (int)(sp - msg), msg);
    snprintf(b, sizeof b, "%s", sp + 1);
    big_mid(cv, 64, 15, a, 2, 1);
    big_mid(cv, 64, 35, b, 2, 1);
}

/* the count-in: its beats to go at 4x, a recording dot */
static void count_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    char b[4];
    snprintf(b, sizeof b, "%d", clampi((m->seq.count_in - 2) / 96 + 1, 1, 4));   /* (a bar: 4 .. 1) */
    rfmv_frame(cv, 2, 2, 124, 60);
    rfmv_disc(cv, 20, 26, 12, 1);
    rfmv_big(cv, 54, 18, b, 4, 1);
}

/* whichever popup is up now (the newer of a control's popup and a message; the count-in under both) */
static void popups(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const char *msg = rfmv_seq_msg_text(m);
    int up = m->pop && m->pop != POP_SELECT && rfmv_pop_up(m, m->pop);
    if (up && m->pop == POP_STEP && (q->view || q->projects || m->s.mode != RFM_MOVE_MODE_SEQ))
        up = 0;
    if (msg && (!up || (int32_t)(q->msg_ms - m->pop_ms) >= 0)) {
        message_popup(cv, msg);
        return;
    }
    if (up) {
        if (m->pop == POP_STEP)
            step_popup(m, cv);
        else if (m->pop == POP_PROJ)
            project_popup(m, cv);
        else
            value_popup(m, cv);
        return;
    }
    if (m->s.mode == RFM_MOVE_MODE_SEQ && q->start_pending == 3)
        count_popup(m, cv);
}

/* ================================================================ FM-1 mode === */
/* the owner's 64 x 64 drawing of the FM-1 (white = on; bit 63 = x 0): the frame, MASTER SELECT PRESETS ALGO (2 x 2
 * dots), the screen, K1..K4 and the 6 x 2 buttons, OCT- / OCT+, the keybed (11 + 4 gaps over 16 naturals) */
static const uint64_t FM1_ART[64] = {
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u,
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u,
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u,
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0ffffffffffffff0u,
    0x1000000000000008u, 0x2000000000000004u, 0x20003fff00000004u, 0x20003fff00000004u,
    0x230c30031830c184u, 0x230c30031830c184u, 0x200031c300000004u, 0x200033e300000004u,
    0x2000366300000004u, 0x200036333fffff84u, 0x230c3c3344444444u, 0x230c381f44444444u,
    0x2000300f44444444u, 0x200030037fffffc4u, 0x20003fff44444444u, 0x23fe3fff44444444u,
    0x2222000044444444u, 0x23fe00003fffff84u, 0x2000000000000004u, 0x2000000000000004u,
    0x21ffffffffffff04u, 0x2200000000000084u, 0x2236db6db6db6c84u, 0x2236db6db6db6c84u,
    0x2236db6db6db6c84u, 0x2236db6db6db6c84u, 0x2200000000000084u, 0x22db6db6db6db684u,
    0x22db6db6db6db684u, 0x22db6db6db6db684u, 0x22db6db6db6db684u, 0x2200000000000084u,
    0x21ffffffffffff04u, 0x2000000000000004u, 0x1000000000000008u, 0x0ffffffffffffff0u,
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u,
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u,
    0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u, 0x0000000000000000u,
};
/* the drawn encoders' 2 x 2 dots, by matrix encoder (0 SELECT 1 ALGO 2..5 K1..K4 6 PRESETS 7 MASTER): the left 2 x 2
 * as the FM-1 has them (MASTER SELECT / PRESETS ALGO), K1..K4 in the row over the buttons */
static const uint8_t ENC_XY[8][2] = { { 12, 20 }, { 12, 26 }, { 35, 20 }, { 42, 20 }, { 48, 20 }, { 55, 20 },
                                      { 6, 26 }, { 6, 20 } };
static const int8_t NATURAL_OF[RFM_MOVE_NKEYS] = {   /* key -> its natural's index, or (sharps) -1 - the one below */
    0, -1, 1, -2, 2, -3, 3, 4, -5, 5, -6, 6, 7, -8, 8, -9, 9, -10, 10, 11, -12, 12, -13, 13, 14, -15, 15,
};

void rfmv_fm1_art(const rfm_move_t *m, mr_canvas_t *cv)
{
    int x, y, k, e = -1, b;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            cv->px[y][x] = (uint8_t)(FM1_ART[y] >> (63 - x) & 1);
    /* the encoder touched (or SELECT, the jog wheel, turned a moment ago): its dot inverted in a 6 x 6 block */
    if (m->touched >= 0 && m->touched < RFM_MOVE_NKNOBS)
        e = rfm_move_knob_enc(m->touched);
    else if (m->touched == 8)
        e = RFM_MOVE_E_MASTER;
    else if (m->touched == 9 || rfmv_pop_up(m, POP_SELECT))
        e = RFM_MOVE_E_SELECT;
    if (e >= 0) {
        fill(cv, ENC_XY[e][0] - 2, ENC_XY[e][1] - 2, 6, 6, 1);
        fill(cv, ENC_XY[e][0], ENC_XY[e][1], 2, 2, 0);
    }
    /* the buttons lit (RFM_MOVE_SCREEN_LIT) or held: their cells filled. The 6 x 2 grid in the panel's order
     * (FX SEL ENV LFO EDIT GLO / HOME SAVE ARP SEQ PLAY REC = matrix 2..13), OCT- OCT+ in the box at the left */
    for (b = 0; b < RFM_MOVE_NBTN; b++) {
        if (!m->btn_count[b] && rfmv_level(m, b) < RFM_MOVE_SCREEN_LIT)
            continue;
        if (b < 2)
            fill(cv, 7 + 4 * b, 32, 3, 1, 1);
        else
            fill(cv, 34 + 4 * ((b - 2) % 6), 26 + 4 * ((b - 2) / 6), 3, 3, 1);
    }
    /* the keys held: their 2 x 4 blocks dark. Naturals: the bottom row's 16; sharps: the top row's block in the gap
     * after their natural (the gaps B-C and E-F stay as drawn) */
    for (k = 0; k < RFM_MOVE_NKEYS; k++) {
        const int n = NATURAL_OF[k];
        if (!m->key_count[k])
            continue;
        if (n >= 0)
            fill(cv, 8 + 3 * n, 43, 2, 4, 0);
        else
            fill(cv, 10 + 3 * (-1 - n), 38, 2, 4, 0);
    }
    if (m->seq.playing)                                 /* the sequencer plays: a small triangle over the drawing */
        rfmv_tri(cv, 3, 3, 5, 9, 1);
}

/* ================================================================ MIDI, Control === */
/* a pad's note in the layout: 2 the root, 1 in the scale (every drum pad), 0 out, -1 no note */
static int pad_class(const rfm_move_t *m, int p)
{
    const int n = rfm_move_pad_note(&m->s, p);
    const uint8_t *scl = rfmv_scales[m->s.scale < RFM_MOVE_NSCALES ? m->s.scale : 0];
    int rel, i;
    if (n < 0)
        return -1;
    rel = ((n - m->s.root) % 12 + 12) % 12;
    if (rel == 0)
        return 2;
    if (m->s.layout == RFM_MOVE_LAYOUT_DRUM)
        return 1;
    for (i = 0; i < scl[0]; i++)
        if (scl[1 + i] == rel)
            return 1;
    return 0;
}

/* Midi.dc.html: 14 x 14 cells, 16 px apart, top row = the top pads; root filled, in the scale outlined, out a dot,
 * a pressed pad filled with a 6 x 6 black square */
static void midi_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    int p;
    for (p = 0; p < RFM_MOVE_NPADS; p++) {
        const int x = (p % 8) * 16 + 1, y = (3 - p / 8) * 16 + 1, c = pad_class(m, p);
        if (c < 0)
            continue;
        if (m->pad_down[p]) {
            fill(cv, x, y, 14, 14, 1);
            fill(cv, x + 4, y + 4, 6, 6, 0);
        } else if (c == 2)
            fill(cv, x, y, 14, 14, 1);
        else if (c == 1)
            box(cv, x, y, 14, 14);
        else
            fill(cv, x + 6, y + 6, 2, 2, 1);
    }
}

/* Control.dc.html: the 4 banks as tabs (the current one filled), the 8 values as full-height bars */
static void bank_tabs(const rfm_move_t *m, mr_canvas_t *cv, int y)
{
    int i;
    for (i = 0; i < RFM_MOVE_NBANKS; i++)
        mr_gfx_rect(cv, i * 32 + 1, y, 30, 4, 1, i == m->s.bank);
}
static void control_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    int k;
    bank_tabs(m, cv, 1);
    for (k = 0; k < RFM_MOVE_NKNOBS; k++) {
        const int h = (m->cc_val[m->s.bank][k] * 54 + 63) / 127, x = k * 16 + 2;
        box(cv, x, 8, 12, 56);
        fill(cv, x, 9 + 54 - h, 12, h, 1);
    }
}

/* ================================================================ held Track buttons === */
/* Track 2: the layout (tabs: the top row's pads 1..4), the scale (row 2's 8 pads), the root (a keyboard octave: the
 * jog wheel), the channel (the 16 steps); the setting changed last in words at 2x */
static void midi_menu(const rfm_move_t *m, mr_canvas_t *cv)
{
    char b[24];
    int i;
    for (i = 0; i < 4; i++)
        mr_gfx_rect(cv, i * 32 + 1, 0, 30, 4, 1, i == m->s.layout);
    for (i = 0; i < RFM_MOVE_NSCALES; i++)
        mr_gfx_rect(cv, i * 16 + 1, 6, 14, 4, 1, i == m->s.scale);
    if (m->menu_what == MENU_LAYOUT)
        snprintf(b, sizeof b, "%s", rfm_move_layout_name(m->s.layout));
    else if (m->menu_what == MENU_CHANNEL)
        snprintf(b, sizeof b, "CH %d", m->s.channel + 1);
    else
        snprintf(b, sizeof b, "%s %s", rfmv_note_names[m->s.root % 12], rfm_move_scale_name(m->s.scale));
    big_mid(cv, 64, 15, b, 2, 1);
    for (i = 0; i < 12; i++) {                          /* the root: C..B, sharps short */
        const int sharp = (0x54A >> i) & 1, x = 4 + i * 10;
        mr_gfx_rect(cv, x, 34, 8, sharp ? 5 : 9, 1, i == m->s.root);
    }
    for (i = 0; i < RFM_MOVE_NSTEPS; i++)
        mr_gfx_rect(cv, i * 8, 54, 7, 10, 1, i == m->s.channel);
}

/* Track 3: the bank, the knob turned (filled) and its CC number at 2x */
static void cc_menu(const rfm_move_t *m, mr_canvas_t *cv)
{
    char b[16];
    const int k = m->menu_what == MENU_CC ? m->menu_i & 7 : 0;
    int i;
    bank_tabs(m, cv, 1);
    snprintf(b, sizeof b, "CC %d", m->s.cc[m->s.bank][k]);
    big_mid(cv, 64, 15, b, 2, 1);
    for (i = 0; i < RFM_MOVE_NKNOBS; i++)
        mr_gfx_rect(cv, i * 16 + 2, 40, 12, 24, 1, i == k);
}

static void held_menu(const rfm_move_t *m, mr_canvas_t *cv, int t)
{
    if (t == RFM_MOVE_MODE_MIDI)
        midi_menu(m, cv);
    else if (t == RFM_MOVE_MODE_CONTROL)
        cc_menu(m, cv);
    else
        rfmv_seq_track_menu(m, cv);
}

/* ================================================================ the screen === */
/* The thumbnail is plain black and white, no dither (the owner's call: it reads more clearly). Each FM-1 pixel is
 * white when its luminance is above RFM_MOVE_THUMB_LUMA; a thumbnail pixel (a 3..4 px box of them) is white when at
 * least a quarter of its box is, so 1 px lines and text strokes survive the 3.75x reduction. */
static void thumb_from(rfm_move_t *m, const uint16_t *px)
{
    int x, y, i, j;
    for (y = 0; y < 64; y++) {
        const int y0 = y * 240 / 64, y1 = (y + 1) * 240 / 64;
        for (x = 0; x < 64; x++) {
            const int x0 = x * 240 / 64, x1 = (x + 1) * 240 / 64;
            uint32_t lit = 0, cnt = 0;
            for (j = y0; j < y1; j++)
                for (i = x0; i < x1; i++) {
                    const uint16_t v = px[j * 240 + i];
                    const uint32_t r = (v >> 11 & 31) * 255 / 31, g = (v >> 5 & 63) * 255 / 63, b = (v & 31) * 255 / 31;
                    lit += ((77 * r + 150 * g + 29 * b) >> 8) > RFM_MOVE_THUMB_LUMA;
                    cnt++;
                }
            m->thumb[y][x] = (uint8_t)(lit * 4 >= cnt);
        }
    }
}

int rfm_move_screen_timed(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    return q->playing || q->start_pending || rfmv_seq_msg_text(m) != NULL || (m->pop && rfmv_pop_up(m, m->pop)) ||
           (q->projects && q->proj_pending >= 0);
}

void rfm_move_screen(rfm_move_t *m, const uint16_t *rgb565, uint8_t out[MR_SCREEN_BYTES])
{
    mr_canvas_t cv;                                     /* (8 KB of stack) */
    int t, x, y;
    if (rgb565)
        thumb_from(m, rgb565);
    mr_gfx_clear(&cv, 0);
    for (t = 1; t < 4 && !rfmv_track_held(m, t); t++) {}   /* (Track 1 has no menu: FM-1 mode stays in view) */
    if (t < 4) {
        held_menu(m, &cv, t);
        mr_gfx_pack(&cv, out);
        return;
    }
    switch (m->s.mode) {
    case RFM_MOVE_MODE_SEQ: rfmv_seq_screen(m, &cv); break;
    case RFM_MOVE_MODE_MIDI: midi_screen(m, &cv); break;
    case RFM_MOVE_MODE_CONTROL: control_screen(m, &cv); break;
    default:
        rfmv_fm1_art(m, &cv);
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                cv.px[y][64 + x] = m->thumb[y][x];
        break;
    }
    popups(m, &cv);
    mr_gfx_pack(&cv, out);
}
