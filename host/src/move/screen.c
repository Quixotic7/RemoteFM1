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

void rfmv_xbox(mr_canvas_t *cv, int x, int y, int w, int h)
{
    int i;
    box(cv, x, y, w, h);
    for (i = 0; i < w; i++) {                           /* (the diagonals, scaled to the box) */
        const int j = i * (h - 1) / (w - 1 > 0 ? w - 1 : 1);
        mr_gfx_pixel(cv, x + i, y + j, 1), mr_gfx_pixel(cv, x + w - 1 - i, y + j, 1);
    }
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
    case POP_MASTER: return m->touched == i && !rfmv_half(m);   /* (FM-1 mode, MIDI's NOTES: turning only) */
    case POP_CC:
    case POP_TRIG: return m->touched == i;
    case POP_SELECT: return 0;                          /* (turning only: a moment after the last detent) */
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
/* FM-1 mode: popups keep to the left half, so the FM-1's screen (the right half) stays in view */
static int g_half;
static int g_full;                                      /* (a popup over the whole screen: nothing drawn back) */

/* the left-half knob popup: the label (2x when it fits, else 1x), the value at 2x, the bar */
static void knob_popup_half(mr_canvas_t *cv, const char *label, const char *value, const char *unit, int frac,
                            int centred)
{
    int w;
    rfmv_frame(cv, 0, 0, 64, 64);
    if (rfmv_big_w(label, 2) <= 58)
        rfmv_big(cv, 4, 5, label, 2, 1);
    else
        mr_gfx_text(cv, 4, 8, label, 1);
    w = big_fit(cv, 4, 24, value, 2, 56, 1) - 4;
    if (unit && *unit && w + 8 + 6 * (int)strlen(unit) <= 60)
        mr_gfx_text(cv, 4 + w + 4, 31, unit, 1);
    box(cv, 4, 46, 56, 10);
    if (!centred)
        fill(cv, 6, 48, clampi(frac, 0, 1000) * 52 / 1000, 6, 1);
    else {
        const int d = clampi(frac, -1000, 1000) * 26 / 1000;
        fill(cv, d >= 0 ? 32 : 32 + d, 48, d >= 0 ? d : -d, 6, 1);
        fill(cv, 32, 47, 1, 8, 1);                      /* (the middle) */
    }
}

static void knob_popup(mr_canvas_t *cv, const char *label, const char *value, const char *unit, int frac, int centred,
                       int deg)
{
    int w;
    if (g_half) {
        knob_popup_half(cv, label, value, unit, frac, centred);
        return;
    }
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
    case POP_SELECT:
    case POP_ENC: {
        const int e = m->pop == POP_SELECT ? RFM_MOVE_E_SELECT : rfm_move_knob_enc(i), t = m->pop_val;
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
    default:                                            /* POP_BANK (the sequencer: TRIG before bank 1) */
        if (m->s.mode == RFM_MOVE_MODE_SEQ && q->trig) {
            snprintf(l, sizeof l, "KNOBS");
            snprintf(v, sizeof v, "TRIG");
            frac = 0;
        } else {
            snprintf(l, sizeof l, "BANK");
            snprintf(v, sizeof v, "%d", m->s.bank + 1);
            frac = (m->s.bank + 1) * 1000 / RFM_MOVE_NBANKS;
        }
        break;
    }
    knob_popup(cv, l, v, u, frac, centred, -135 + frac * 270 / 1000);
}

/* ---- the TRIG page's popup: a picture on the left, the name and the value at 2x on the right */
static void hline(mr_canvas_t *cv, int x, int y, int w) { fill(cv, x, y, w, 2, 1); }
static void arrow(mr_canvas_t *cv, int x0, int x1, int y)   /* a 2 px arrow from x0 to x1 (its head at x1) */
{
    const int d = x1 > x0 ? 1 : -1;
    int i;
    hline(cv, x0 < x1 ? x0 : x1, y, (x1 - x0) * d + 1);
    for (i = 1; i <= 5; i++)
        fill(cv, x1 - d * i, y - i, 1, 2 * i + 2, 1);
}
static void die(mr_canvas_t *cv, int x, int y)        /* a die showing five */
{
    box(cv, x, y, 26, 26);
    box(cv, x + 1, y + 1, 24, 24);
    fill(cv, x + 5, y + 5, 4, 4, 1), fill(cv, x + 17, y + 5, 4, 4, 1), fill(cv, x + 11, y + 11, 4, 4, 1);
    fill(cv, x + 5, y + 17, 4, 4, 1), fill(cv, x + 17, y + 17, 4, 4, 1);
}
static void hop(mr_canvas_t *cv, int x, int y, int w)  /* an arc from (x, y) to (x + w, y), up */
{
    int i;
    for (i = 0; i <= w; i++) {
        const int u = 2 * i - w, h = (w * w - u * u) * 10 / (w * w > 0 ? w * w : 1);
        fill(cv, x + i, y - h, 1, 2, 1);
    }
}
static const char *const FN_NAMES[RFM_MOVE_FN_N] = { "NONE", "RESTART", "REVERSE", "FORWARD", "FLIP", "RANDOM",
                                                      "RND FN", "LEAP", "JUMP" };
void rfmv_cond_name(int c, char *b, int cap)
{
    static const char *const N[] = { "FILL", "!FILL", "PRE", "!PRE", "NEI", "!NEI", "1ST", "!1ST" };
    if (c >= 1 && c <= RFM_MOVE_COND_AB_N) {
        int bb = 2, a = c;
        while (a > bb)
            a -= bb, bb++;
        snprintf(b, (size_t)cap, "%d:%d", a, bb);
    } else if (c >= RFM_MOVE_COND_FILL && c < RFM_MOVE_COND_N)
        snprintf(b, (size_t)cap, "%s", N[c - RFM_MOVE_COND_FILL]);
    else
        snprintf(b, (size_t)cap, "ALWAYS");
}
static void trig_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int k = m->pop_i, clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
    int abs = q->page * RFM_MOVE_SEQ_PAGE + (q->step_first >= 0 ? q->step_first : 0), v, i;
    char l[16], b[16];
    if (q->step_first < 0) {                            /* none held: the clip's first step with notes */
        int lo, hi = rfmv_seq_clip_range(m, clip, &lo);
        abs = hi > lo ? q->notes[lo].step : abs;
    }
    v = rfmv_seq_trig_val(m, clip, abs, k);
    rfmv_frame(cv, 2, 2, 124, 60);
    l[0] = b[0] = 0;
    switch (k) {
    case 0:                                             /* a step, its note early or late */
        snprintf(l, sizeof l, "MICRO"), snprintf(b, sizeof b, v ? "%+d" : "0", v);
        box(cv, 12, 22, 24, 20);
        for (i = 22; i < 42; i += 3)
            mr_gfx_pixel(cv, 12, i, 1);
        fill(cv, 12 + v, 26, 4, 12, 1);
        break;
    case 1:                                             /* the hits */
        snprintf(l, sizeof l, "RATCH"), snprintf(b, sizeof b, "x%d", v);
        for (i = 0; i < v; i++)
            fill(cv, 8 + i * 32 / v, 14, 32 / v > 4 ? 3 : 2, 34, 1);
        break;
    case 2:
        snprintf(l, sizeof l, "CHANCE"), snprintf(b, sizeof b, "%d%%", v);
        die(cv, 10, 18);
        break;
    case 3:                                             /* A:B: B passes, the A-th filled */
        snprintf(l, sizeof l, "COND"), rfmv_cond_name(v, b, sizeof b);
        if (v >= 1 && v <= RFM_MOVE_COND_AB_N) {
            int bb = 2, a = v;
            while (a > bb)
                a -= bb, bb++;
            for (i = 0; i < bb; i++)
                mr_gfx_rect(cv, 6 + (i % 4) * 10, 18 + (i / 4) * 12, 8, 8, 1, i == a - 1);
        } else if (v)
            rfmv_big(cv, 8, 22, v == RFM_MOVE_COND_FILL || v == RFM_MOVE_COND_NFILL ? "F" : v <= RFM_MOVE_COND_NPRE ? "P" : v <= RFM_MOVE_COND_NNEI ? "N" : "1", 4, 1);
        break;
    case 4:
        snprintf(l, sizeof l, "FUNC"), snprintf(b, sizeof b, "%s", FN_NAMES[clampi(v, 0, RFM_MOVE_FN_N - 1)]);
        switch (v) {
        case RFM_MOVE_FN_RESTART: arrow(cv, 40, 10, 30), fill(cv, 6, 20, 2, 22, 1); break;
        case RFM_MOVE_FN_REVERSE: arrow(cv, 40, 8, 30); break;
        case RFM_MOVE_FN_FORWARD: arrow(cv, 8, 40, 30); break;
        case RFM_MOVE_FN_FLIP: arrow(cv, 8, 40, 22), arrow(cv, 40, 8, 38); break;
        case RFM_MOVE_FN_RANDOM: die(cv, 10, 18); break;
        case RFM_MOVE_FN_RFUNC: die(cv, 6, 18), rfmv_big(cv, 36, 22, "?", 2, 1); break;
        case RFM_MOVE_FN_LEAP: hop(cv, 6, 40, 14), hop(cv, 22, 40, 14), fill(cv, 38, 38, 4, 4, 1); break;
        case RFM_MOVE_FN_JUMP: hop(cv, 6, 42, 34), fill(cv, 38, 40, 4, 4, 1); break;
        default: break;
        }
        break;
    case 5: {
        const int f = rfmv_seq_trig_val(m, clip, abs, 4);
        if (f == RFM_MOVE_FN_LEAP)
            snprintf(l, sizeof l, "LEAP"), snprintf(b, sizeof b, "%+d", v), hop(cv, 6, 40, 30);
        else if (f == RFM_MOVE_FN_JUMP)
            snprintf(l, sizeof l, "TO STEP"), snprintf(b, sizeof b, "%d", v + 1), hop(cv, 6, 42, 34);
        else
            snprintf(l, sizeof l, "AMOUNT"), snprintf(b, sizeof b, "--");
        break;
    }
    case 6:                                             /* a gate bar over a step's width (a step: 8 px) */
        snprintf(l, sizeof l, "LENGTH");
        if (rfmv_seq_len_inf(v))
            snprintf(b, sizeof b, "INF");
        else if (v % RFM_MOVE_SEQ_TPS == 0)
            snprintf(b, sizeof b, "%d", v / RFM_MOVE_SEQ_TPS);
        else if (v < RFM_MOVE_SEQ_TPS)
            snprintf(b, sizeof b, "1/%d", RFM_MOVE_SEQ_TPS / v > 0 ? (RFM_MOVE_SEQ_TPS + v / 2) / v : 1);
        else
            snprintf(b, sizeof b, "%d.%d", v / RFM_MOVE_SEQ_TPS, v % RFM_MOVE_SEQ_TPS * 10 / RFM_MOVE_SEQ_TPS);
        for (i = 0; i < 5; i++)
            mr_gfx_pixel(cv, 6 + i * 8, 44, 1);         /* (the steps) */
        fill(cv, 6, 26, clampi(v * 8 / RFM_MOVE_SEQ_TPS, 1, 38), 12, 1);
        if (rfmv_seq_len_inf(v))
            fill(cv, 40, 26, 4, 12, 0), rfmv_big(cv, 40, 24, ">", 2, 1);
        break;
    case 7:                                             /* velocity: a level */
        snprintf(l, sizeof l, "VELO"), snprintf(b, sizeof b, "%d", v);
        box(cv, 14, 12, 16, 40);
        fill(cv, 14, 52 - v * 40 / 127, 16, v * 40 / 127, 1);
        break;
    default: snprintf(l, sizeof l, "--"); break;
    }
    big_fit(cv, 50, 10, l, 2, 74, 1);
    big_fit(cv, 50, 34, b, 2, 74, 1);
}

/* Loop held: the 16 pages (in the loop filled, the shown one marked under it) and the loop's pages at 2x */
static void loop_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
    const int a = rfmv_seq_loop_lo(m, clip), h = rfmv_seq_loop_hi(m, clip), pa = a / RFM_MOVE_SEQ_PAGE;
    const int pb = (h - 1) / RFM_MOVE_SEQ_PAGE;
    char b[24];
    int i;
    rfmv_frame(cv, 2, 2, 124, 60);
    {                                                   /* the length: pages (+ steps) */
        const int L = h - a, P = L / RFM_MOVE_SEQ_PAGE, S = L % RFM_MOVE_SEQ_PAGE;
        if (!P)
            snprintf(b, sizeof b, "%d ST", S);
        else if (S)
            snprintf(b, sizeof b, "%dP+%d", P, S);
        else
            snprintf(b, sizeof b, "%d %s", P, P == 1 ? "BAR" : "BARS");
    }
    big_mid(cv, 64, 8, b, 2, 1);
    {                                                   /* the loop bar: its span over the 16 pages, end marks */
        const int x0 = 5 + a * 112 / RFM_MOVE_SEQ_MAXLEN, x1 = 5 + h * 112 / RFM_MOVE_SEQ_MAXLEN;
        fill(cv, x0, 26, x1 - x0 > 1 ? x1 - x0 : 1, 3, 1);
        fill(cv, x0, 24, 1, 7, 1), fill(cv, x1 - 1 > x0 ? x1 - 1 : x0, 24, 1, 7, 1);
    }
    for (i = 0; i < RFM_MOVE_SEQ_PAGES; i++) {
        if (rfmv_seq_src_page(m, SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel), i) == 2)
            rfmv_xbox(cv, 4 + i * 7 + 1, 34, 6, 12);    /* (the page cut) */
        else if (rfmv_seq_src_page(m, SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel), i) == 1)
            rfmv_dotted(cv, 4 + i * 7 + 1, 34, 6, 12);  /* (copied) */
        else
            mr_gfx_rect(cv, 4 + i * 7 + 1, 34, 6, 12, 1, i >= pa && i <= pb);
        if (i == q->page)
            fill(cv, 4 + i * 7 + 1, 49, 6, 2, 1);
    }
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
    {                                                   /* its colour (1..16, a dot and the number), as knob 1 turns
                                                         * it */
        char cb[8];
        snprintf(cb, sizeof cb, "%d", in.color ? in.color : m->pop_i % RFM_MOVE_PROJ_COLORS + 1);
        rfmv_big(cv, 123 - rfmv_big_w(cb, 2), 6, cb, 2, 1);
        rfmv_disc(cv, 123 - rfmv_big_w(cb, 2) - 13, 8, 10, 1);
    }
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
    if (g_half) {                                       /* the left half: a word a line, 2x when it fits */
        rfmv_frame(cv, 0, 0, 64, 64);
        snprintf(a, sizeof a, "%.*s", sp ? (int)(sp - msg) : (int)strlen(msg), msg);
        snprintf(b, sizeof b, "%s", sp ? sp + 1 : "");
        if (rfmv_big_w(a, 2) <= 58 && rfmv_big_w(b, 2) <= 58) {
            big_mid(cv, 32, *b ? 15 : 25, a, 2, 1);
            if (*b)
                big_mid(cv, 32, 35, b, 2, 1);
        } else {
            mr_gfx_text(cv, 32 - 3 * (int)strlen(a), *b ? 22 : 28, a, 1);
            if (*b)
                mr_gfx_text(cv, 32 - 3 * (int)strlen(b), 34, b, 1);
        }
        return;
    }
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
    int up = m->pop && (m->pop != POP_SELECT || g_half) && rfmv_pop_up(m, m->pop) &&
             !(m->pop == POP_MODE && m->pop_i < 4 && rfmv_hold_shown(m, m->pop_i));   /* (its button held: its menu) */
    if (up && m->pop == POP_STEP && (q->view || q->projects || m->s.mode != RFM_MOVE_MODE_SEQ))
        up = 0;
    if (msg && (!up || (int32_t)(q->msg_ms - m->pop_ms) >= 0)) {
        message_popup(cv, msg);
        return;
    }
    if (up) {
        if (m->pop == POP_STEP)
            step_popup(m, cv);
        else if (m->pop == POP_TRIG)
            trig_popup(m, cv);
        else if (m->pop == POP_LOOP)
            loop_popup(m, cv);
        else if (m->pop == POP_TRACK) {                 /* a track / bank change: "TRACK n" and the 4 x 4 */
            char b[12];
            snprintf(b, sizeof b, "TR %d", m->pop_i + 1);
            rfmv_frame(cv, 2, 2, 124, 60);
            rfmv_seq_track_glyph(m, cv, 10, 12, 8);
            rfmv_big(cv, 56, 22, b, 2, 1);
        } else if (m->pop == POP_MODE) {                /* a Track button tapped: the mode (or its screen) named, the
                                                         * whole screen, no frame: as big as it fits, centred */
            static const char *const MODES[4] = { "FM-1", "MIDI", "CONTROL", "SEQUENCER" };
            static const char *const VIEWS[RFM_MOVE_MV_N] = { "PADS", "NOTES" };
            const char *w = m->pop_i < 4 ? MODES[m->pop_i] : VIEWS[(m->pop_i - 10) % RFM_MOVE_MV_N];
            int k;
            mr_gfx_clear(cv, 0);
            for (k = 4; k > 1 && rfmv_big_w(w, k) > 126; k--) {}
            if (rfmv_big_w(w, k) <= 126)
                rfmv_big(cv, 64 - rfmv_big_w(w, k) / 2, 32 - 7 * k / 2, w, k, 1);
            else {                                      /* (two lines) */
                char a[16];
                const char *sp = strchr(w, ' ');
                snprintf(a, sizeof a, "%.*s", sp ? (int)(sp - w) : (int)strlen(w) / 2, w);
                big_mid(cv, 64, 14, a, 2, 1);
                big_mid(cv, 64, 36, sp ? sp + 1 : w + strlen(a), 2, 1);
            }
            g_full = 1;
        } else if (m->pop == POP_BAR) {                   /* Move's "Bar 2" (a ghost page), over the page strip */
            char b[12];
            snprintf(b, sizeof b, "BAR %d", m->pop_i + 1);
            fill(cv, 0, 12, 128, 38, 0);
            big_mid(cv, 64, 24, b, 2, 1);
        }
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

/* ---- MIDI mode's NOTES screen: the notes held (names, 2x when they fit) and the chord they make (Cmaj7, Am, Dsus4,
 * G7; inverted: a slash chord, C/E) on the left half */
static const struct { uint16_t set; const char *name; } CHORDS[] = {
    { 0x091, "" }, { 0x089, "m" }, { 0x049, "dim" }, { 0x111, "aug" }, { 0x085, "sus2" }, { 0x0A1, "sus4" },
    { 0x891, "maj7" }, { 0x491, "7" }, { 0x489, "m7" }, { 0x449, "m7b5" }, { 0x249, "dim7" }, { 0x889, "mmaj7" },
    { 0x291, "6" }, { 0x289, "m6" }, { 0x095, "add9" }, { 0x08D, "madd9" }, { 0x495, "9" }, { 0x895, "maj9" },
    { 0x48D, "m9" }, { 0x4A1, "7sus4" }, { 0x081, "5" },
};
/* the chord of notes n[0..k) (any order): its name, or "" */
const char *rfmv_chord_name(const uint8_t *n, int k, char *out, int cap)
{
    uint16_t pcs = 0;
    int i, r, c, bass = 128, best = -1, best_r = 0;
    out[0] = 0;
    for (i = 0; i < k; i++) {
        pcs = (uint16_t)(pcs | 1u << (n[i] % 12));
        if (n[i] < bass)
            bass = n[i];
    }
    if (k < 2)
        return out;
    for (r = 0; r < 12; r++) {                          /* (the bass as the root first: C6 over Am7/C) */
        const int root = (bass % 12 + r) % 12;
        const uint16_t rel = (uint16_t)(((pcs >> root) | (pcs << (12 - root))) & 0xFFF);
        if (!(rel & 1))
            continue;
        for (c = 0; c < (int)(sizeof CHORDS / sizeof CHORDS[0]); c++)
            if (CHORDS[c].set == rel && (best < 0 || c < best))
                best = c, best_r = root;
        if (best >= 0)
            break;
    }
    if (best < 0)
        return out;
    snprintf(out, (size_t)cap, "%s%s", rfmv_note_names[best_r], CHORDS[best].name);
    if (bass % 12 != best_r) {
        const size_t l = strlen(out);
        snprintf(out + l, (size_t)cap - l, "/%s", rfmv_note_names[bass % 12]);
    }
    return out;
}
static void notes_half(const rfm_move_t *m, mr_canvas_t *cv)
{
    uint8_t n[16];
    char b[24], c[16];
    int k = 0, i, y;
    for (i = 0; i < 128 && k < 16; i++)
        if (m->note_count[m->s.channel][i])
            n[k++] = (uint8_t)i;
    if (!k)
        return;                                         /* (at rest: black) */
    rfmv_chord_name(n, k, c, sizeof c);
    y = 2;
    if (*c) {                                           /* the chord: 2x when it fits, else 1x */
        if (rfmv_big_w(c, 2) <= 62)
            rfmv_big(cv, 1, y, c, 2, 1), y += 18;
        else
            mr_gfx_text(cv, 1, y + 4, c, 1), y += 14;
        mr_gfx_rect(cv, 1, y - 3, 60, 1, 1, 1);
    }
    if (k <= (*c ? 2 : 3)) {                            /* a few: one a line at 2x */
        for (i = 0; i < k; i++, y += 16)
            rfmv_big(cv, 1, y, rfmv_note_name(n[i], b, sizeof b), 2, 1);
        return;
    }
    {                                                   /* more: 1x, as many a line as fit */
        int x = 1;
        for (i = 0; i < k; i++) {
            rfmv_note_name(n[i], b, sizeof b);
            if (x + 6 * (int)strlen(b) > 63)
                x = 1, y += 9;
            mr_gfx_text(cv, x, y, b, 1);
            x += 6 * (int)strlen(b) + 4;
        }
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
    rfmv_scale_tabs(m, cv, 5, m->menu_what == MENU_SCALE);
    if (m->menu_what == MENU_LAYOUT)
        snprintf(b, sizeof b, "%s", rfm_move_layout_name(m->s.layout));
    else if (m->menu_what == MENU_CHANNEL)
        snprintf(b, sizeof b, "CH %d", m->s.channel + 1);
    else if (m->menu_what == MENU_SCALE) {              /* a scale pad: the scale's name alone, its tab tall */
        snprintf(b, sizeof b, "%s", rfm_move_scale_name(m->s.scale));
    } else
        snprintf(b, sizeof b, "%s %s", rfmv_note_names[m->s.root % 12], rfm_move_scale_name(m->s.scale));
    big_mid(cv, 64, 15, b, 2, 1);
    for (i = 0; i < 12; i++) {                          /* the root: C..B, sharps short */
        const int sharp = (0x54A >> i) & 1, x = 4 + i * 10;
        mr_gfx_rect(cv, x, 34, 8, sharp ? 5 : 9, 1, i == m->s.root);
    }
    for (i = 0; i < RFM_MOVE_NSTEPS; i++)
        mr_gfx_rect(cv, i * 8, 54, 7, 10, 1, i == m->s.channel);
}

/* the 24 scales as 3 rows of 8 tabs (as the pads: rows 2..4), the current one filled (tall: a scale pad pressed) */
void rfmv_scale_tabs(const rfm_move_t *m, mr_canvas_t *cv, int y, int tall)
{
    int i;
    for (i = 0; i < RFM_MOVE_NSCALES; i++)
        if (i == m->s.scale)                            /* (the current: a block; the others: a line) */
            mr_gfx_rect(cv, (i % 8) * 16 + 1, y + (i / 8) * 3 - (tall ? 1 : 0), 14, tall ? 4 : 2, 1, 1);
        else
            mr_gfx_rect(cv, (i % 8) * 16 + 3, y + (i / 8) * 3 + 1, 10, 1, 1, 1);
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
    int t;
    for (t = 0; t < 4; t++)                             /* (a Track button held, its screen not yet due) */
        if (rfmv_track_held(m, t) && !rfmv_hold_shown(m, t))
            return 1;
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
    for (t = 1; t < 4 && !rfmv_hold_shown(m, t); t++) {}   /* (Track 1 has no menu: FM-1 mode stays in view; a tap:
                                                         * nothing of a hold screen) */
    if (t < 4) {
        held_menu(m, &cv, t);
        mr_gfx_pack(&cv, out);
        return;
    }
    if (m->s.mode != RFM_MOVE_MODE_SEQ && (m->seq.smenu || m->seq.projects)) {   /* a sequencer menu opened with
                                                         * Track 1 held (the Shift layer): full width until Back */
        rfmv_seq_screen(m, &cv);
        popups(m, &cv);
        mr_gfx_pack(&cv, out);
        return;
    }
    switch (m->s.mode) {
    case RFM_MOVE_MODE_SEQ: rfmv_seq_screen(m, &cv); break;
    case RFM_MOVE_MODE_MIDI:
        if (m->mmenu)
            midi_menu(m, &cv);                          /* (Shift + step 9: Keys & Scales) */
        else if (m->s.midi_view == RFM_MOVE_MV_NOTES) {
            notes_half(m, &cv);                         /* the notes and the chord; the FM-1's screen on the right */
            for (y = 0; y < 64; y++)
                for (x = 0; x < 64; x++)
                    cv.px[y][64 + x] = m->thumb[y][x];
        } else
            midi_screen(m, &cv);
        break;
    case RFM_MOVE_MODE_CONTROL: control_screen(m, &cv); break;
    default:
        rfmv_fm1_art(m, &cv);
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                cv.px[y][64 + x] = m->thumb[y][x];
        break;
    }
    if (rfmv_hold_shown(m, 0)) {                        /* Track 1 held (any mode): the Shift layer, a badge top left */
        mr_gfx_rect(&cv, 0, 0, 40, 11, 1, 1);
        fill(&cv, 3, 6, 7, 1, 0), fill(&cv, 4, 5, 5, 1, 0), fill(&cv, 5, 4, 3, 1, 0), fill(&cv, 6, 3, 1, 1, 0);
        fill(&cv, 5, 7, 3, 2, 0);                       /* (an up arrow) */
        mr_gfx_text(&cv, 12, 2, "SHIFT", 0);
    }
    g_half = rfmv_half(m), g_full = 0;
    popups(m, &cv);
    if (g_half && !g_full) {                                       /* (any wider popup: the FM-1's screen drawn back over it) */
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                cv.px[y][64 + x] = m->thumb[y][x];
        g_half = 0;
    }
    mr_gfx_pack(&cv, out);
}
