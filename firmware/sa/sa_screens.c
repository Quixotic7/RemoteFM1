/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's screens after S1 (design/standalone-mockup, halved): the CC screens (CC-Bank, CC-Turn, CC-Assign)
 * and the save's note. Part of sa_ui.c (its snapshot, ui; sa_gfx.c's primitives). */

static uint16_t sa_kcol(uint32_t k)          /* the knobs' colours: KN1 blue, KN2 orange, KN3 yellow, KN4 green */
{
    static const uint8_t T[4] = {SA_TK_A5, SA_TK_A1, SA_TK_A3, SA_TK_A4};
    return ui.c[T[k & 3u]];
}

static void sa_txt_num(char *s, uint32_t v)  /* v in decimal */
{
    sa_num(s, (int32_t)v, 0);
}

/* ------------------------------------------------------------------ CC-Bank --- */
static void sa_paint_ccbank(void)
{
    static const char *const L[SA_NCCB] = {"A", "B", "C", "D", "E"};
    int32_t i, s;
    for (i = 0; i < (int32_t)SA_NCCB; i++) {   /* the five banks: 38 wide, 6 apart, from x 13 */
        int32_t x = 13 + 44 * i, on = i == ui.ccbank;
        sg_chip(x, 9, 38, 28, on ? UC(INK) : UC(PANEL));
        sg_chip(x + 2, 11, 34, 24, on ? UC(INK) : UC(BG));
        sg_text(x + (38 - sg_text_w(L[i], 2)) / 2, 13, 2, L[i], on ? UC(ON) : UC(SOFT));
    }
    for (i = 0; i < 4; i++) {                /* the four knobs: the value, a meter of 14, the CC */
        int32_t x = 18 + 54 * i, lit = (ui.ccv[i] * 14 + 63) / 127;
        char v[4], cc[8];
        sa_txt_num(v, ui.ccv[i]);
        sg_text(x + (42 - sg_text_w(v, 2)) / 2, 48, 2, v, UC(INK));
        for (s = 0; s < 14; s++)
            sg_rect(x, 200 - 10 * s, 42, 8, s < lit ? sa_kcol((uint32_t)i) : UC(D5));
        sa_cpy(cc, "CC");
        sa_txt_num(cc + 2, ui.cc[i]);
        sg_chip_text(x + (42 - (sg_text_w(cc, 1) + 8)) / 2, 213, sg_text_w(cc, 1) + 8, 20, 4, cc,
                     sa_kcol((uint32_t)i), UC(ON));
    }
}

/* ------------------------------------------------------------------ CC-Turn --- */
static void sa_cc_chips(int32_t y)           /* the bank (left) and the knob (right) */
{
    static const char *const L[SA_NCCB] = {"A", "B", "C", "D", "E"};
    char kn[4] = {'K', 'N', (char)('1' + ui.knob), 0};
    int32_t w = sg_text_w(kn, 1) + 14;
    sg_chip_text(10, y, 0, 22, 7, L[ui.ccbank], UC(INK), UC(ON));
    sg_chip_text(230 - w, y, w, 22, 7, kn, sa_kcol(ui.knob), UC(ON));
}

static void sa_paint_ccturn(void)
{
    char cc[8], v[4];
    int32_t i, w, lit = (ui.ccv[ui.knob] * 16 + 63) / 127;
    sa_cc_chips(8);
    sa_cpy(cc, "CC ");
    sa_txt_num(cc + 3, ui.cc[ui.knob]);
    w = sg_text_w(cc, 3) + 18;
    sg_chip((SG_W - w) / 2, 38, w, 38, sa_kcol(ui.knob));
    sg_text((SG_W - sg_text_w(cc, 3)) / 2, 42, 3, cc, UC(ON));
    sa_txt_num(v, ui.ccv[ui.knob]);
    sg_text_sh((SG_W - sg_text_w(v, 9)) / 2, 98, 9, v, UC(INK), 4, UC(D1));
    for (i = 0; i < 16; i++)                 /* 16 blocks, the last lit one in the highlight */
        sg_rect(9 + 14 * i, 202, 12, 22, i < lit ? (i == lit - 1 ? UC(A3) : sa_kcol(ui.knob)) : UC(D5));
}

/* ------------------------------------------------------------------ CC-Assign --- */
static void sa_paint_ccassign(void)
{
    char v[4];
    int32_t i, off = (int32_t)(ui.now % 800u / 200u) * 2, cc = ui.cc[ui.knob];
    const char *name = sa_cc_name((uint32_t)cc);
    for (i = -8; i < SG_W + 8; i += 4) {     /* the hazard stripes, crawling right in four steps */
        int32_t x = i + off, odd = ((i + 8) / 4) & 1;
        sg_rect(x, 0, 4, 4, odd ? UC(BG) : UC(A3));
        sg_rect(x, 4, 4, 4, odd ? UC(A3) : UC(BG));
        sg_rect(x, SG_H - 8, 4, 4, odd ? UC(BG) : UC(A3));
        sg_rect(x, SG_H - 4, 4, 4, odd ? UC(A3) : UC(BG));
    }
    sa_cc_chips(16);
    sg_chip_text(96, 16, 48, 22, 0, "GLO", UC(A3), UC(ON));
    sg_text_c(48, 3, "CC", sa_kcol(ui.knob));
    if (cc > 0) {                            /* the neighbours, dim, half off the edges */
        sa_txt_num(v, (uint32_t)cc - 1u);
        sg_text(-28 + (52 - sg_text_w(v, 4)) / 2, 106, 4, v, UC(PANEL));
    }
    if (cc < (int32_t)SA_CC_MAX) {
        sa_txt_num(v, (uint32_t)cc + 1u);
        sg_text(216 + (52 - sg_text_w(v, 4)) / 2, 106, 4, v, UC(PANEL));
    }
    sa_txt_num(v, (uint32_t)cc);
    sg_text_sh((SG_W - sg_text_w(v, 8)) / 2, 92, 8, v, UC(INK), 4, UC(D1));
    sg_text_c(190, 2, name, UC(INK));
}

/* ------------------------------------------------------------------ saved (SAVE held) --- */
static void sa_paint_saved(void)
{
    sg_chip_text((SG_W - 80) / 2, 8, 80, 22, 0, "SAVE", UC(A4), UC(ON));
    sg_text_sh((SG_W - sg_text_w("SAVED", 5)) / 2, 90, 5, "SAVED", UC(A4), 4, UC(D2));
    sg_text_c(170, 1, "CC MAP  ARP  TEMPO  THEME", UC(SOFT));
}

/* ------------------------------------------------------------------ the dispatch --- */
static void sa_ui_snap_more(void) {}

static int sa_paint_more(void)               /* 1: painted (a screen after S1) */
{
    switch (ui.scr) {
    case SA_SCR_CCBANK: sa_paint_ccbank(); return 1;
    case SA_SCR_CCTURN: sa_paint_ccturn(); return 1;
    case SA_SCR_CCASSIGN: sa_paint_ccassign(); return 1;
    case SA_SCR_SAVED: sa_paint_saved(); return 1;
    default: return 0;
    }
}
