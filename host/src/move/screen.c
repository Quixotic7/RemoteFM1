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
#include "v2.h"
#include "../../third_party/moveremote/common/font5x7.h"

#define clampi rfmv_clampi

/* ================================================================ drawing === */
static void fill(mr_canvas_t *cv, int x, int y, int w, int h, int on) { mr_gfx_rect(cv, x, y, w, h, on, 1); }

/* the CSS triangle (border-left w, top / bottom h / 2 transparent): row j is w x (1 - |centre - h/2| / (h/2)) long */
void rfmv_tri(mr_canvas_t *cv, int x, int y, int w, int h, int on)
{
    int j;
    for (j = 0; j < h; j++) {
        const int d = 2 * j + 1 - h, len = (w * (h - (d < 0 ? -d : d)) + h / 2) / h;
        fill(cv, x, y + j, len, 1, on);
    }
}

const char *rfmv_note_name(int n, char *b, int cap)
{
    snprintf(b, (size_t)cap, "%s%d", rfmv_note_names[n % 12], n / 12 - 2);   /* (Ableton's: MIDI 60 = C3) */
    return b;
}

/* ================================================================ popups === */
uint32_t rfmv_now_ms(const rfm_move_t *m) { return (uint32_t)(m->seq.now_us / 1000u); }

void rfmv_pop(rfm_move_t *m, int kind, int i)
{
    m->pop = (uint8_t)kind, m->pop_i = (uint8_t)i, m->pop_ms = rfmv_now_ms(m), m->pop_val = 0;
}

void rfmv_pop_let(rfm_move_t *m, int kind, int i)
{
    if (m->pop == kind && m->pop_i == i)
        m->pop_ms = rfmv_now_ms(m);
}

void rfmv_menu(rfm_move_t *m, int what, int i) { m->menu_what = (uint8_t)what, m->menu_i = (uint8_t)i, m->menu_ms = rfmv_now_ms(m); }

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

/* ---- v2 (the design canvas's V2Screen, see v2.h): full-screen popups with no frame, the left-half ones (FM-1
 * mode, MIDI's NOTES) the name and the value only; Title Case words */
static int g_half;
static int g_full;                                      /* (a popup over the whole screen: nothing drawn back) */

/* Title Case ("CUTOFF" -> "Cutoff", "UP DN" -> "Up Dn"); words with a digit stay as they are (K1, P12) */
void rfmv_tcase(const char *s, char *o, int cap)
{
    int i = 0, start = 1, digit = 0;
    const char *w;
    for (; *s && i < cap - 1; s++) {
        if (start) {                                    /* (a word with a digit or a '/': kept) */
            for (digit = 0, w = s; *w && *w != ' '; w++)
                digit |= (*w >= '0' && *w <= '9') || *w == '/';
        }
        o[i++] = digit ? *s : start ? (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s)
                                    : (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
        start = *s == ' ' || *s == '-';
    }
    o[i] = 0;
}

/* the knob pages' strip: Sequencer mode TRIG and the 4 banks, else the 4 banks */
static void bank_strip(const rfm_move_t *m, mr_canvas_t *cv)
{
    if (m->s.mode == RFM_MOVE_MODE_SEQ)
        v2_strip(cv, 5, 1u << (m->seq.trig ? 0 : m->s.bank + 1), -1, 0, 128);
    else
        v2_strip(cv, 4, 1u << m->s.bank, -1, 0, 128);
}

/* a value popup: the left half (the name, the value), else the whole screen (the name, the value, a bar) */
static void value_pop(mr_canvas_t *cv, const char *label, const char *val, double frac, int bar)
{
    if (g_half) {
        v2_lpop(cv, label, val);
        return;
    }
    mr_gfx_clear(cv, 0);
    v2_titleBox(cv, label, val, 64, 124);
    if (bar)
        v2_vbar(cv, frac, bar == 2, 4, 120, 52);
}

static const char *enc_label(const rfm_move_t *m, int e)
{
    return e >= 0 && e <= RFM_MOVE_NENC && m->label[e][0] ? m->label[e] : rfm_move_enc_name(e);
}

/* the knob popups' contents, by kind */
static void value_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    char l[24], v[24];
    const int i = m->pop_i;
    switch (m->pop) {
    case POP_SELECT:
    case POP_ENC: {                                     /* an FM-1 encoder: this turn's detents */
        const int e = m->pop == POP_SELECT ? RFM_MOVE_E_SELECT : rfm_move_knob_enc(i), t = m->pop_val;
        rfmv_tcase(enc_label(m, e), l, sizeof l);
        snprintf(v, sizeof v, t ? "%+d" : "0", t);
        value_pop(cv, l, v, clampi(t, -32, 32) / 32.0, 2);
        return;
    }
    case POP_MASTER:
        rfmv_tcase(enc_label(m, RFM_MOVE_E_MASTER), l, sizeof l);
        snprintf(v, sizeof v, "%d%%", (m->master * 100 + 511) / 1023);
        value_pop(cv, l, v, m->master / 1023.0, 1);
        return;
    case POP_CC: {
        const int b = m->s.bank, cc = m->s.cc[b][i & 7];
        int val = m->cc_val[b][i & 7], lock = 0;
        snprintf(l, sizeof l, "CC %d", cc);
        if (m->s.mode == RFM_MOVE_MODE_SEQ && !q->view && q->step_first >= 0) {   /* a held step's lock */
            const int clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
            val = rfmv_seq_lock_get(m, clip, q->page * RFM_MOVE_SEQ_PAGE + q->step_first, cc);
            lock = 1;
        }
        if (val < 0)
            snprintf(v, sizeof v, "--"), val = 0;
        else
            snprintf(v, sizeof v, "%d", val);
        value_pop(cv, l, v, val / 127.0, 1);
        if (!g_half) {
            if (lock)
                v2_ic(cv, V2I_LOCK, 114, 4, 1);         /* (a padlock: the step's lock) */
            bank_strip(m, cv);
        }
        return;
    }
    case POP_DRUM: {
        const int n = m->s.drum[i & 31];
        rfmv_note_name(n, v, sizeof v);
        value_pop(cv, "Drum", v, n / 127.0, 1);
        return;
    }
    case POP_TEMPO: {
        const int b10 = rfm_move_seq_bpm10(m);
        snprintf(v, sizeof v, b10 % 10 ? "%d.%d" : "%d", b10 / 10, b10 % 10);
        value_pop(cv, "Tempo", v, 0, 0);
        return;
    }
    default:                                            /* POP_BANK (the sequencer: TRIG before bank 1) */
        if (m->s.mode == RFM_MOVE_MODE_SEQ && q->trig)
            value_pop(cv, "Knobs", "Trig", 0, 0);
        else {
            snprintf(v, sizeof v, "%d", m->s.bank + 1);
            value_pop(cv, "Bank", v, 0, 0);
        }
        if (!g_half)
            bank_strip(m, cv);
        return;
    }
}

/* ---- the TRIG page: a picture on the left, the name and the value on the right (no boxes); the knob pages as the
 * strip, TRIG first. A step held and the arrows, the jog or the volume knob (not on the TRIG page): Nudge, Length,
 * Velocity as edit popups over the page strip */
static const char *const FN_NAMES[RFM_MOVE_FN_N] = { "None", "Restart", "Reverse", "Forward", "Flip", "Random",
                                                      "Rnd Fn", "Leap", "Jump" };
void rfmv_cond_name(int c, char *b, int cap)
{
    static const char *const N[] = { "Fill", "!Fill", "Pre", "!Pre", "Nei", "!Nei", "1st", "!1st" };
    if (c >= 1 && c <= RFM_MOVE_COND_AB_N) {
        int bb = 2, a = c;
        while (a > bb)
            a -= bb, bb++;
        snprintf(b, (size_t)cap, "%d:%d", a, bb);
    } else if (c >= RFM_MOVE_COND_FILL && c < RFM_MOVE_COND_N)
        snprintf(b, (size_t)cap, "%s", N[c - RFM_MOVE_COND_FILL]);
    else
        snprintf(b, (size_t)cap, "Always");
}
static void len_text(int v, char *b, int cap)
{
    if (rfmv_seq_len_inf(v))
        snprintf(b, (size_t)cap, "Inf");
    else if (v % RFM_MOVE_SEQ_TPS == 0)
        snprintf(b, (size_t)cap, "%d", v / RFM_MOVE_SEQ_TPS);
    else if (v < RFM_MOVE_SEQ_TPS)
        snprintf(b, (size_t)cap, "1/%d", (RFM_MOVE_SEQ_TPS + v / 2) / v);
    else
        snprintf(b, (size_t)cap, "%d.%d", v / RFM_MOVE_SEQ_TPS, v % RFM_MOVE_SEQ_TPS * 10 / RFM_MOVE_SEQ_TPS);
}
static void steps_base(mr_canvas_t *cv)               /* a base line and the step ticks under a picture */
{
    int i;
    v2_R(cv, 2, 45, 41, 1, 1);
    for (i = 0; i < 6; i++)
        v2_R(cv, 2 + i * 8, 40, 1, 6, 1);
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
    v = q->trig && q->step_first < 0 ? rfmv_seq_tdef_val(m, k) : rfmv_seq_trig_val(m, clip, abs, k);   /* (none held:
                                                         * the defaults for new steps) */
    mr_gfx_clear(cv, 0);
    g_full = 1;
    if (!q->trig) {                                     /* a held step's edit popup (V2Track's seq.micro / len / vel) */
        if (k == 0)
            snprintf(l, sizeof l, "Nudge"), snprintf(b, sizeof b, v ? "%+d%%" : "0%%", v * 100 / RFM_MOVE_SEQ_TPS);
        else if (k == 6)
            snprintf(l, sizeof l, "Length"), len_text(v, b, sizeof b);
        else
            snprintf(l, sizeof l, "Velocity"), snprintf(b, sizeof b, "%d", v);
        v2_titleBox(cv, l, b, 64, 124);
        if (k == 7)
            v2_vbar(cv, v / 127.0, 0, 4, 120, 52);
        rfmv_seq_page_strip(m, cv, abs);
        return;
    }
    l[0] = b[0] = 0;
    switch (k) {
    case 0:                                             /* the note between its step's ticks, pushed by the offset */
        snprintf(l, sizeof l, "Micro"), snprintf(b, sizeof b, v ? "%+d" : "0", v);
        v2_R(cv, 2, 47, 41, 1, 1), v2_R(cv, 2, 40, 1, 8, 1), v2_R(cv, 42, 40, 1, 8, 1);
        v2_R(cv, 7 + clampi(v, -5, 9), 26, 26, 4, 1);
        if (v > 0)
            v2_L(cv, 4, 36, 10, 36), v2_L(cv, 8, 34, 10, 36), v2_L(cv, 8, 38, 10, 36);
        else if (v < 0)
            v2_L(cv, 40, 36, 34, 36), v2_L(cv, 36, 34, 34, 36), v2_L(cv, 36, 38, 34, 36);
        break;
    case 1: {                                           /* the hits inside one step, fading */
        const int n = clampi(v, 1, 8), sp = 40 / n;
        snprintf(l, sizeof l, "Ratch"), snprintf(b, sizeof b, "x%d", v);
        for (i = 0; i < n; i++)
            v2_R(cv, 4 + i * sp, 12 + i * 16 / n, sp * 4 / 10 > 1 ? sp * 4 / 10 : 1, 34 - i * 16 / n, 1);
        v2_R(cv, 2, 47, 40, 1, 1);
        break;
    }
    case 2: {                                           /* a die */
        static const int P[5][2] = { { 8, 14 }, { 30, 14 }, { 19, 25 }, { 8, 36 }, { 30, 36 } };
        snprintf(l, sizeof l, "Chance"), snprintf(b, sizeof b, "%d%%", v);
        for (i = 0; i < 5; i++)
            v2_R(cv, P[i][0], P[i][1], 6, 6, 1);
        break;
    }
    case 3:                                             /* A:B as B passes, the A-th tall */
        snprintf(l, sizeof l, "Cond"), rfmv_cond_name(v, b, sizeof b);
        if (v >= 1 && v <= RFM_MOVE_COND_AB_N) {
            int bb = 2, a = v;
            while (a > bb)
                a -= bb, bb++;
            for (i = 0; i < bb; i++) {
                const int x = 2 + i * (40 / bb);
                if (i == a - 1)
                    v2_R(cv, x, 18, 40 / bb - 2, 20, 1);
                else
                    v2_R(cv, x, 36, 40 / bb - 2, 2, 1);
            }
        }
        break;
    case 4:                                             /* the playhead's path over the steps */
        snprintf(l, sizeof l, "Func"), snprintf(b, sizeof b, "%s", FN_NAMES[clampi(v, 0, RFM_MOVE_FN_N - 1)]);
        steps_base(cv);
        switch (v) {
        case RFM_MOVE_FN_RESTART: v2_L(cv, 37, 30, 5, 30), v2_L(cv, 5, 30, 9, 26), v2_L(cv, 5, 30, 9, 34), v2_R(cv, 3, 20, 1, 20, 1); break;
        case RFM_MOVE_FN_REVERSE: v2_L(cv, 37, 30, 5, 30), v2_L(cv, 5, 30, 9, 26), v2_L(cv, 5, 30, 9, 34); break;
        case RFM_MOVE_FN_FORWARD: v2_L(cv, 5, 30, 37, 30), v2_L(cv, 37, 30, 33, 26), v2_L(cv, 37, 30, 33, 34); break;
        case RFM_MOVE_FN_FLIP:
            v2_L(cv, 5, 24, 37, 24), v2_L(cv, 37, 24, 33, 20), v2_L(cv, 37, 24, 33, 28);
            v2_L(cv, 37, 34, 5, 34), v2_L(cv, 5, 34, 9, 30), v2_L(cv, 5, 34, 9, 38);
            break;
        case RFM_MOVE_FN_RANDOM: case RFM_MOVE_FN_RFUNC: v2_ic(cv, V2I_DICE, 16, 22, 1); break;
        case RFM_MOVE_FN_LEAP: case RFM_MOVE_FN_JUMP:
            v2_L(cv, 5, 38, 13, 20), v2_L(cv, 13, 20, 29, 20), v2_L(cv, 29, 20, 37, 38);
            v2_L(cv, 33, 36, 37, 38), v2_L(cv, 37, 34, 37, 38);
            break;
        default: break;
        }
        break;
    case 5: {                                           /* the function's amount */
        const int f = q->step_first < 0 ? rfmv_seq_tdef_val(m, 4) : rfmv_seq_trig_val(m, clip, abs, 4);
        steps_base(cv);
        if (f == RFM_MOVE_FN_LEAP || f == RFM_MOVE_FN_JUMP) {
            v2_L(cv, 5, 38, 13, 20), v2_L(cv, 13, 20, 29, 20), v2_L(cv, 29, 20, 37, 38);
            v2_L(cv, 33, 36, 37, 38), v2_L(cv, 37, 34, 37, 38);
        }
        if (f == RFM_MOVE_FN_LEAP)
            snprintf(l, sizeof l, "Leap"), snprintf(b, sizeof b, "%+d", v);
        else if (f == RFM_MOVE_FN_JUMP)
            snprintf(l, sizeof l, "Jump"), snprintf(b, sizeof b, "%d", v + 1);
        else
            snprintf(l, sizeof l, "Amount"), snprintf(b, sizeof b, "--");
        break;
    }
    case 6:                                             /* a gate bar over the steps it covers */
        snprintf(l, sizeof l, "Length"), len_text(v, b, sizeof b);
        steps_base(cv);
        v2_R(cv, 3, 26, rfmv_seq_len_inf(v) ? 40 : clampi(v * 8 / RFM_MOVE_SEQ_TPS - 2, 1, 40), 6, 1);
        break;
    case 7:                                             /* velocity */
        snprintf(l, sizeof l, "Velo"), snprintf(b, sizeof b, "%d", v);
        v2_ic(cv, V2I_VEL, 16, 22, 1);
        break;
    default: snprintf(l, sizeof l, "--"); break;
    }
    v2_titleBox(cv, l, b, 88, 80);
    v2_strip(cv, 5, 1, -1, 0, 128);
}

/* Loop held or latched (page edit): the loop icon, the range in words, the 16 pages as cells; the jog: Move's
 * "Loop Length" (the length at 4x, the looped pages thick on a strip of 16) */
static void loop_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
    const int a = rfmv_seq_loop_lo(m, clip), h = rfmv_seq_loop_hi(m, clip), pa = a / RFM_MOVE_SEQ_PAGE;
    const int pb = (h - 1) / RFM_MOVE_SEQ_PAGE;
    char b[24];
    int i;
    mr_gfx_clear(cv, 0);
    g_full = 1;
    if (m->pop_val == 1 && rfmv_now_ms(m) - m->pop_ms < RFM_MOVE_POPUP_MS) {   /* the jog: the length (a second) */
        const int L = h - a, P = L / RFM_MOVE_SEQ_PAGE, S = L % RFM_MOVE_SEQ_PAGE;
        uint32_t sel = 0;
        if (!P)
            snprintf(b, sizeof b, "%d St", S);
        else if (S)
            snprintf(b, sizeof b, "%d.%d", P, S * 10 / RFM_MOVE_SEQ_PAGE);
        else
            snprintf(b, sizeof b, "%d %s", P, P == 1 ? "Bar" : "Bars");
        for (i = pa; i <= pb && i < 16; i++)
            sel |= 1u << i;
        v2_titleBox(cv, "Loop", b, 64, 124);
        v2_strip(cv, 16, sel, -1, 0, 128);
        return;
    }
    rfmv_seq_loop_edit(m, cv);                          /* (else: page edit's own screen) */
}

/* a step held (300 ms): its notes in words, the velocity and the length; the steps row, the held step marked */
static void step_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int s = m->pop_i, clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
    const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
    int lo, hi = rfmv_seq_range(m, clip, abs, &lo), n = hi - lo, i;
    char b[48], nb[8];
    mr_gfx_clear(cv, 0);
    g_full = 1;
    b[0] = 0;
    for (i = 0; i < n; i++) {                           /* the names that fit (2x), "+n" for the rest */
        char t[48];
        snprintf(t, sizeof t, "%s%s%s", b, i ? " " : "", rfmv_note_name(q->notes[lo + i].note, nb, sizeof nb));
        if (v2_tw(t, 2) > 124 - (i + 1 < n ? 30 : 0) && i) {
            snprintf(t, sizeof t, "%s +%d", b, n - i);
            snprintf(b, sizeof b, "%s", t);
            break;
        }
        snprintf(b, sizeof b, "%s", t);
    }
    v2_TC(cv, 64, 3, b, 2, 1);
    if (n) {
        const rfm_move_note_t *n0 = &q->notes[lo];
        v2_ic(cv, V2I_VEL, 2, 26, 1);
        snprintf(b, sizeof b, "%d", n0->vel);
        v2_T(cv, 18, 24, b, 2, 1);
        v2_R(cv, 68, 31, 18, 1, 1), v2_R(cv, 68, 28, 1, 7, 1), v2_R(cv, 85, 28, 1, 7, 1);   /* length: a bracket */
        len_text(n0->len, b, sizeof b);
        v2_T(cv, 92, 24, b, 2, 1);
    }
    rfmv_seq_steps_cells(m, cv, 1u << s);
}

/* a project pad held: its number at 4x, a bar a track, the tempo, the clips; knob 1 turned: its colour */
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
    mr_gfx_clear(cv, 0);
    g_full = 1;
    if (m->pop_val == 1) {                              /* the colour, once */
        const int c = in.color ? in.color : m->pop_i % RFM_MOVE_PROJ_COLORS + 1;
        snprintf(b, sizeof b, "%d", c);
        v2_titleBox(cv, "Color", b, 64, 124);
        v2_strip(cv, 16, 1u << (c - 1), -1, 0, 128);
        return;
    }
    snprintf(b, sizeof b, "%02d", m->pop_i + 1);
    v2_T(cv, 2, 4, b, 4, 1);
    if (!in.ok)
        return;
    for (i = 8; i < RFM_MOVE_SEQ_TRACKS; i++)
        if (in.notes[i])
            nt = 16;                                    /* (tracks 9..16 in use: 16 thinner bars) */
    for (i = 0; i < nt; i++) {
        const int h = in.notes[i] ? clampi(5 * bits_of(in.notes[i]) + 1, 1, 26) : 1;
        if (nt == 8)
            v2_R(cv, 64 + i * 8, 32 - h, 5, h, 1);
        else
            v2_R(cv, 64 + i * 4, 32 - h, 3, h, 1);
    }
    v2_ic(cv, V2I_PLAY, 2, 44, 1);
    snprintf(b, sizeof b, "%d", (in.bpm10 + 5) / 10);
    v2_T(cv, 16, 43, b, 2, 1);
    v2_ic(cv, V2I_CLIP, 74, 44, 1);
    snprintf(b, sizeof b, "%d", in.clips);
    v2_T(cv, 90, 43, b, 2, 1);
}

/* a message (Title Case, past tense naming the thing): one or two rows, an icon over it where one says more, the
 * page strip kept under it in track view; the left half: the rows at 2x where they fit */
static void message_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    const char *a, *b;
    int icon, id = rfmv_seq_msg_info(m, &a, &b, &icon);
    if (g_half) {
        v2_R(cv, 0, 0, 64, 64, 0);
        if (*b) {
            v2_TC(cv, 32, 18, a, v2_tw(a, 2) <= 62 ? 2 : 1, 1);
            v2_TC(cv, 32, 38, b, v2_tw(b, 2) <= 62 ? 2 : 1, 1);
        } else
            v2_TC(cv, 32, 26, a, v2_tw(a, 2) <= 62 ? 2 : 1, 1);
        return;
    }
    mr_gfx_clear(cv, 0);
    g_full = 1;
    if (id == SEQ_MSG_CAPTURED) {                       /* Move's message with the tempo under it */
        char t[16];
        const int b10 = rfm_move_seq_bpm10(m);
        snprintf(t, sizeof t, "%d BPM", (b10 + 5) / 10);
        v2_titleBox(cv, a, t, 64, 124);
    } else if (icon >= 0) {                             /* (a toggle or a one-shot with its icon: no strip) */
        v2_ic(cv, icon, 58, 4, 1);
        v2_msg(cv, a, b);
        return;
    } else
        v2_msg(cv, a, b);
    if (id == SEQ_MSG_DOUBLE) {                         /* the loop's pages on a strip of 16 */
        const rfm_move_seq_t *q = &m->seq;
        const int clip = SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
        const int pa = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE, pb = (rfmv_seq_loop_hi(m, clip) - 1) / RFM_MOVE_SEQ_PAGE;
        uint32_t sel = 0;
        int i;
        for (i = pa; i <= pb && i < 16; i++)
            sel |= 1u << i;
        v2_strip(cv, 16, sel, -1, 0, 128);
    } else if (m->s.mode == RFM_MOVE_MODE_SEQ && !m->seq.view && !m->seq.projects)
        rfmv_seq_page_strip(m, cv, -1);
}

/* the count-in: a record dot, the beats to go at 4x; the strip fills beat by beat */
static void count_popup(const rfm_move_t *m, mr_canvas_t *cv)
{
    char b[4];
    const int go = clampi((m->seq.count_in - 2) / 96 + 1, 1, 4);   /* (a bar: 4 .. 1) */
    snprintf(b, sizeof b, "%d", go);
    mr_gfx_clear(cv, 0);
    v2_ic(cv, V2I_REC, 14, 22, 1);
    v2_TC(cv, 76, 14, b, 4, 1);
    v2_strip(cv, 4, (1u << (5 - go)) - 1, -1, 0, 128);
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
        message_popup(m, cv);
        return;
    }
    if (up) {
        if (m->pop == POP_STEP)
            step_popup(m, cv);
        else if (m->pop == POP_TRIG)
            trig_popup(m, cv);
        else if (m->pop == POP_LOOP)
            loop_popup(m, cv);
        else if (m->pop == POP_TRACK) {                 /* a track change: its name */
            char b[12];
            snprintf(b, sizeof b, "Track %d", m->pop_i + 1);
            mr_gfx_clear(cv, 0);
            v2_msg(cv, b, NULL);
            g_full = 1;
        } else if (m->pop == POP_MODE) {                /* a Track button tapped: the mode (or its view) named, the
                                                         * whole screen, the strip the four modes (the two views) */
            static const char *const MODES[4] = { "FM-1", "MIDI", "Control", "Seq" };
            static const char *const VIEWS[RFM_MOVE_MV_N] = { "Pads", "Notes" };
            mr_gfx_clear(cv, 0);
            if (m->pop_i == 20) {                       /* projects mode entered: Move's set icon over the name */
                v2_ic(cv, V2I_PROJ, 58, 4, 1);
                v2_TC(cv, 64, 26, "Projects", 2, 1);
            } else if (m->pop_i < 4)
                v2_modeName(cv, MODES[m->pop_i], 4, m->pop_i);
            else
                v2_modeName(cv, VIEWS[(m->pop_i - 10) % RFM_MOVE_MV_N], RFM_MOVE_MV_N, (m->pop_i - 10) % RFM_MOVE_MV_N);
            g_full = 1;
        } else if (m->pop == POP_BAR) {                 /* Move's "Bar 2" (a ghost page) over the ghost strip */
            char b[12];
            snprintf(b, sizeof b, "Bar %d", m->pop_i + 1);
            mr_gfx_clear(cv, 0);
            v2_TC(cv, 64, 22, b, 2, 1);
            rfmv_seq_page_strip(m, cv, -1);
            g_full = 1;
        } else if (m->pop == POP_PROJ)
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
    if (e >= 0)
        v2_XR(cv, ENC_XY[e][0] - 2, ENC_XY[e][1] - 2, 6, 6);
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

/* V2Midi midi.pads: 14 x 14 cells, 16 px apart, top row = the top pads; root filled, in the scale outlined, out a
 * dot; a pressed pad carries a 6 x 6 square in the opposite colour */
static void midi_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    int p;
    for (p = 0; p < RFM_MOVE_NPADS; p++) {
        const int x = (p % 8) * 16 + 1, y = (3 - p / 8) * 16 + 1, c = pad_class(m, p);
        if (c < 0)
            continue;
        if (c == 2)
            v2_R(cv, x, y, 14, 14, 1);
        else if (c == 1)
            v2_O(cv, x, y, 14, 14, 1);
        else
            v2_R(cv, x + 6, y + 6, 2, 2, 1);
        if (m->pad_down[p])                             /* pressed: a square in the opposite colour */
            v2_R(cv, x + 4, y + 4, 6, 6, c != 2);
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
/* V2Midi midi.chord: the chord's name at 3x (2x, 1x when it is wider), the held notes on a one-octave keyboard */
static void notes_half(const rfm_move_t *m, mr_canvas_t *cv)
{
    uint8_t n[16];
    char c[16];
    int k = 0, i, held = 0;
    for (i = 0; i < 128 && k < 16; i++)
        if (m->note_count[m->s.channel][i])
            n[k++] = (uint8_t)i, held |= 1 << (i % 12);
    if (!k)
        return;                                         /* (at rest: black) */
    rfmv_chord_name(n, k, c, sizeof c);
    if (*c) {
        const int s = v2_tw(c, 3) <= 62 ? 3 : v2_tw(c, 2) <= 62 ? 2 : 1;
        v2_TC(cv, 32, s == 3 ? 4 : s == 2 ? 8 : 12, c, s, 1);
    }
    v2_kbd(cv, 1, 34, 62, 28, -1, 0, held);
}

/* V2Control ctl.rest: the 8 values as filled bars over their knobs on a base line; the strip the bank */
static void control_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    int k;
    for (k = 0; k < RFM_MOVE_NKNOBS; k++) {
        const int x = k * 16 + 1, h = (m->cc_val[m->s.bank][k] * 55 + 63) / 127;
        v2_R(cv, x, 57, 14, 1, 1);
        v2_R(cv, x, 57 - h, 14, h, 1);
    }
    bank_strip(m, cv);
}

/* ================================================================ held Track buttons === */
/* the build's key / scale screen (kept, the owner's): the layouts as tabs, the key and the scale at 4x (3x, 2x when
 * wider), the root on a 12-key row, the channels (MIDI) or Global / Track (the sequencer's Keys & Scales) */
void rfmv_keys_menu(const rfm_move_t *m, mr_canvas_t *cv, int glob)
{
    char b[24];
    snprintf(b, sizeof b, "%s %s", rfmv_note_names[m->s.root % 12], rfm_move_scale_name(m->s.scale));
    if (v2_tw(b, 4) <= 128)
        v2_keysMenu(cv, b, m->s.layout, m->s.root % 12, m->s.channel, glob);
    else {                                              /* (a long scale name: the same screen, the name smaller) */
        v2_keysMenu(cv, "", m->s.layout, m->s.root % 12, m->s.channel, glob);
        v2_TC(cv, 64, v2_tw(b, 3) <= 128 ? 10 : 13, b, v2_tw(b, 3) <= 128 ? 3 : 2, 1);
    }
}

/* Track 3: the knob legend (the turned one filled), "CC", the number it sends at 4x and which knob; the bank strip */
static void cc_menu(const rfm_move_t *m, mr_canvas_t *cv)
{
    char b[16];
    const int k = m->menu_what == MENU_CC ? m->menu_i & 7 : 0, cc = m->s.cc[m->s.bank][k];
    v2_knobs(cv, 0xff, k);
    v2_T(cv, 6, 18, "CC", 2, 1);
    snprintf(b, sizeof b, "%d", cc);
    v2_T(cv, cc >= 100 ? 26 : 40, 13, b, 4, 1);
    snprintf(b, sizeof b, "K%d", k + 1);
    v2_TC(cv, 106, 18, b, 2, 1);
    bank_strip(m, cv);
}

static void held_menu(const rfm_move_t *m, mr_canvas_t *cv, int t)
{
    if (t == RFM_MOVE_MODE_MIDI)
        rfmv_keys_menu(m, cv, -1);
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
           (m->menu_what && rfmv_now_ms(m) - m->menu_ms < RFM_MOVE_POPUP_MS) ||
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
            rfmv_keys_menu(m, &cv, -1);                 /* (Shift + step 9: Keys & Scales) */
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
    if (rfmv_hold_shown(m, 0)) {                        /* Track 1 held (any mode): the Shift layer, the shift icon
                                                         * in the top-left corner (the drawing's empty one) */
        if (m->s.mode != RFM_MOVE_MODE_FM1)
            v2_R(&cv, 0, 0, 15, 15, 0);
        v2_ic(&cv, V2I_SHIFT, 2, 2, 1);
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
