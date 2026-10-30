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

/* ------------------------------------------------------------------ S3: the looper's snapshot --- */
static void sa_ui_roll(uint32_t t0, uint32_t span, uint32_t now_t)   /* the notes of the loop and the take in
                                                                     * [t0, t0 + span): bars, rows by pitch */
{
    static int32_t start[128];
    static uint8_t vel[128];
    uint32_t k = sl.cur, i, pass, lo = 127, hi = 0, nb = 0;
    struct {
        uint32_t a, b;
        uint8_t note;
    } raw[32];
    for (pass = 0; pass < 2u; pass++) {
        const sa_lev_t *e = pass ? sa_take : &sa_lpool[sl.at[k]];
        uint32_t n = pass ? sl.take_n : (sl.state == SL_REC ? sl.n[k] : sl.n[k]);
        for (i = 0; i < 128u; i++)
            start[i] = -1;
        for (i = 0; i < n; i++) {
            uint32_t no = e[i].d1 & 127u;
            if (e[i].type == SA_LE_ON) {
                start[no] = (int32_t)e[i].t;
                vel[no] = e[i].d2;
            } else if (e[i].type == SA_LE_OFF && start[no] >= 0) {
                if (nb < 32u && e[i].t >= t0 && (uint32_t)start[no] < t0 + span) {
                    raw[nb].a = (uint32_t)start[no], raw[nb].b = e[i].t, raw[nb].note = (uint8_t)no;
                    nb++;
                }
                start[no] = -1;
            }
        }
        for (i = 0; i < 128u && nb < 32u; i++)   /* still held: up to now */
            if (start[i] >= 0 && now_t >= t0) {
                raw[nb].a = (uint32_t)start[i], raw[nb].b = now_t, raw[nb].note = (uint8_t)i;
                nb++;
            }
    }
    (void)vel;
    for (i = 0; i < nb; i++) {
        lo = raw[i].note < lo ? raw[i].note : lo;
        hi = raw[i].note > hi ? raw[i].note : hi;
    }
    ui.nbars = (uint8_t)nb;
    for (i = 0; i < nb; i++) {
        uint32_t a = raw[i].a < t0 ? 0u : raw[i].a - t0, b = raw[i].b - t0;
        uint32_t row = hi > lo ? (raw[i].note - lo) * 11u / (hi - lo) : 5u;
        b = b > span ? span : b;
        ui.bar[i].x0 = (uint8_t)(a * 216u / span);
        ui.bar[i].x1 = (uint8_t)(b * 216u / span);
        if (ui.bar[i].x1 < ui.bar[i].x0 + 3u)
            ui.bar[i].x1 = (uint8_t)(ui.bar[i].x0 + 3u);
        ui.bar[i].row = (uint8_t)row;
        ui.bar[i].hi = row > 7u;
    }
}

static void sa_ui_snap_loop(void)
{
    uint32_t i, k = sl.cur;
    ui.lstate = sl.state;
    ui.lcur = sl.cur;
    ui.lnext = sl.next;
    ui.lcursor = sl.cursor;
    ui.lfull = sl.used + sl.take_n >= SA_LPOOL || sl.drops != 0u;
    for (i = 0; i < SA_LOOPS; i++)
        ui.lfilled[i] = sl.len[i] != 0u;
    ui.llen = sl.state == SL_REC ? sl.rec_target : sl.len[k];
    ui.lpos = sl_pos();
    ui.lrec_ms = sl.state == SL_REC ? ui.now - sl.rec_t0 : ui.lpos;
    ui.count_left = (uint8_t)(sl.state == SL_COUNT ? (sl.count_end - ui.now) * 4u / sl_bar_ms() + 1u : 0u);
    ui.ropt[0] = sa.rec_len, ui.ropt[1] = sa.rec_quant, ui.ropt[2] = sa.rec_count, ui.ropt[3] = sa.rec_layer;
    ui.mfocus = sa.mfocus;
    ui.nbars = 0;
    if (ui.scr == SA_SCR_REC) {
        if (sl.state == SL_REC && !sl.rec_target) {   /* a free take: the last 4 s */
            uint32_t e = ui.lrec_ms, t0 = e > 4000u ? e - 4000u : 0u;
            sa_ui_roll(t0, 4000u, e);
        } else if (ui.llen) {
            sa_ui_roll(0, ui.llen, sl.state == SL_REC ? ui.lrec_ms : ui.lpos);
        }
    }
}

/* ------------------------------------------------------------------ Rec --- */
static void sa_secs(char *s, uint32_t ms, int unit)   /* "4.2", "12.0S" */
{
    uint32_t d = (ms + 50u) / 100u;
    sa_num(s, (int32_t)(d / 10u), 0);
    while (*s)
        s++;
    *s++ = '.';
    *s++ = (char)('0' + d % 10u);
    if (unit)
        *s++ = 'S';
    *s = 0;
}

static void sa_loop_chip(int32_t y, uint32_t k)   /* "LOOP 3", right */
{
    char t[8];
    int32_t w;
    sa_cpy(t, "LOOP ");
    sa_txt_num(t + 5, k + 1u);
    w = sg_text_w(t, 1) + 14;
    sg_chip_text(230 - w, y, w, 22, 7, t, UC(PANEL), UC(INK));
}

static void sa_paint_rec(void)
{
    char big[12];
    int32_t i, w, blink = ui.now % 1000u < 500u;
    const char *lab = ui.lstate == SL_DUB ? "DUB" : ui.lstate == SL_COUNT ? "COUNT" : ui.lfull ? "FULL" : "REC";
    w = sg_text_w(lab, 1) + 14 + 13;
    sg_chip(10, 8, w, 22, UC(D1));
    if (blink)
        sg_rect(17, 15, 8, 8, UC(INK));
    sg_text(30, 14, 1, lab, UC(INK));
    sa_loop_chip(8, ui.lcur);
    if (ui.lstate == SL_COUNT) {
        big[0] = '-';
        sa_txt_num(big + 1, ui.count_left);
    } else {
        sa_secs(big, ui.lrec_ms, 0);
    }
    sg_text_sh((SG_W - sg_text_w(big, 7)) / 2, 50, 7, big, UC(INK), 4, UC(D1));
    /* the roll: 220 x 102 from (10, 126); each note a bar 6 high with its shadow, the playhead blinking */
    for (i = 0; i < ui.nbars; i++) {
        int32_t x = 10 + ui.bar[i].x0, y = 126 + 94 - 8 * ui.bar[i].row, bw = ui.bar[i].x1 - ui.bar[i].x0;
        sg_rect(x + 2, y + 2, bw, 6, UC(BG));
        sg_rect(x, y, bw, 6, ui.bar[i].hi ? UC(A1) : UC(A3));
    }
    if (blink) {
        int32_t x = ui.lstate == SL_REC && !ui.llen ? 226
                  : 10 + (int32_t)(ui.llen ? (ui.lrec_ms % ui.llen) * 216u / ui.llen : 0u);
        sg_rect(x, 126, 4, 102, UC(D1));
    }
}

/* ------------------------------------------------------------------ Loop-Play: the cassette --- */
static const char *const SA_REEL[] = {"....aaaa....", "..aaaaaaaa..", ".aabaaaaaaa.", ".abbaaaaaaa.", "aaaaabbaaaaa",
                                      "aaaabbbbaaaa", "aaaabbbbaaaa", "aaaaabbaaaaa", ".aaaaaaaaaa.", ".aaaaaaaaaa.",
                                      "..aaaaaaaa..", "....aaaa...."};
static uint8_t sa_reel_rot;
static int32_t sa_reel_col(char ch) { return ch == 'a' ? UC(INK) : UC(BG); }
static void sa_reel(int32_t x, int32_t y)    /* 12 x 12 at 3, turned a quarter at a time (the mockup's spin) */
{
    int32_t r, c;
    if (!sg_hits(y, 36))
        return;
    for (r = 0; r < 12; r++)
        for (c = 0; c < 12; c++) {
            int32_t sr = r, sc = c;
            char ch;
            switch (sa_reel_rot & 3u) {
            case 1: sr = 11 - c, sc = r; break;
            case 2: sr = 11 - r, sc = 11 - c; break;
            case 3: sr = c, sc = 11 - r; break;
            default: break;
            }
            ch = SA_REEL[sr][sc];
            if (ch != '.')
                sg_rect(x + 3 * c, y + 3 * r, 3, 3, (uint16_t)sa_reel_col(ch));
        }
}

static void sa_paint_loopplay(void)
{
    char t[12];
    int32_t w, i, prog = ui.llen ? (int32_t)((uint64_t)ui.lpos * 214u / ui.llen) : 0;
    sg_chip_text(10, 8, 0, 22, 7, ui.lstate == SL_PLAY || ui.lstate == SL_DUB ? "PLAY" : "STOP", UC(A4), UC(ON));
    sa_secs(t, ui.llen, 1);
    w = sg_text_w(t, 1) + 14;
    sg_chip_text(230 - w, 8, w, 22, 7, t, UC(PANEL), UC(INK));
    sg_chip(28, 40, 184, 128, UC(MID));      /* the cassette */
    sg_chip(40, 50, 160, 30, UC(INK));
    sa_cpy(t, "LOOP ");
    sa_txt_num(t + 5, ui.lcur + 1u);
    sg_text((SG_W - sg_text_w(t, 2)) / 2, 55, 2, t, UC(ON));
    sg_rect(40, 82, 160, 4, UC(D1));
    sg_chip(64, 92, 112, 44, UC(BG));
    sa_reel_rot = (uint8_t)(ui.lstate == SL_PLAY || ui.lstate == SL_DUB ? ui.now / 150u : 0u);
    sa_reel(70, 96);
    sa_reel(134, 96);
    sg_rect(106, 110, 28, 8, UC(SHADOW));
    sg_chip(68, 144, 104, 24, UC(PANEL));
    sg_rect(86, 152, 8, 8, UC(BG));
    sg_rect(146, 152, 8, 8, UC(BG));
    sg_rect(13, 190, 214, 16, UC(D5));       /* where the loop is */
    sg_rect(13, 190, prog, 16, UC(A4));
    for (i = 7; i < 214; i += 9)
        sg_rect(13 + i, 190, 2, 16, UC(BG));
    if (ui.lnext != 0xFFu) {
        sa_cpy(t, "NEXT ");
        sa_txt_num(t + 5, ui.lnext + 1u);
        sg_text_c(216, 1, t, UC(A3));
    }
}

/* ------------------------------------------------------------------ Loop-Pick: the 16 loops --- */
static const char *const SA_CURSOR[] = {"a......", "aa.....", "aba....", "abba...", "abbba..", "abbbba.", "abbbbba",
                                        "abbaaa.", "aa.aba.", "....aa."};
static int32_t sa_cursor_col(char ch) { return ch == 'a' ? UC(BG) : UC(INK); }

static void sa_paint_loops(void)
{
    int32_t i, blink = ui.now % 1000u < 500u;
    sg_chip_text(10, 8, 0, 22, 7, "LOOPS", UC(A4), UC(ON));
    if (ui.lnext != 0xFFu && (blink || 1)) {
        char t[8];
        int32_t w;
        sa_cpy(t, "NEXT ");
        sa_txt_num(t + 5, ui.lnext + 1u);
        w = sg_text_w(t, 1) + 14;
        sg_chip_text(230 - w, 8, w, 22, 7, t, blink ? UC(A3) : UC(D1), UC(ON));
    }
    for (i = 0; i < 16; i++) {               /* 4 x 4 from (15, 36): 48 x 43, 6 apart */
        int32_t x = 15 + 54 * (i % 4), y = 36 + 49 * (i / 4);
        int playing = i == ui.lcur && (ui.lstate == SL_PLAY || ui.lstate == SL_DUB), queued = i == ui.lnext;
        uint16_t line = playing ? UC(A4) : queued ? UC(A3) : ui.lfilled[i] ? UC(PANEL) : UC(D5);
        uint16_t bg = playing ? UC(A4) : queued ? UC(BG) : ui.lfilled[i] ? UC(PANEL) : UC(BG);
        uint16_t fg = playing ? UC(ON) : queued ? (blink ? UC(A3) : UC(MID)) : ui.lfilled[i] ? UC(INK) : UC(MID);
        char t[4];
        sg_chip(x, y, 48, 43, line);
        sg_chip(x + 2, y + 2, 44, 39, bg);
        sa_txt_num(t, (uint32_t)i + 1u);
        sg_text(x + (48 - sg_text_w(t, 2)) / 2, y + 8, 2, t, fg);
        if (playing) {
            sg_rect(x + 6, y + 34, 36, 4, UC(D2));
            sg_rect(x + 6, y + 34, ui.llen ? (int32_t)(ui.lpos * 36u / ui.llen) : 0, 4, UC(BG));
        }
    }
    {                                        /* the cursor, poking at its loop's corner */
        int32_t c = ui.lcursor, x = 15 + 54 * (c % 4) + 32, y = 36 + 49 * (c / 4) + 22, p = ui.now % 600u < 300u;
        sg_sprite(x - 2 * p, y - 2 * p, 2, SA_CURSOR, 10, sa_cursor_col);
    }
}

/* ------------------------------------------------------------------ Rec options, Arp settings: one layout --- */
static const char *const SA_ARROW[] = {"a....", "aa...", "aaa..", "aaaa.", "aaaaa", "aaaa.", "aaa..", "aa...", "a...."};
static const char *const SA_ARROW_L[] = {"....a", "...aa", "..aaa", ".aaaa", "aaaaa", ".aaaa", "..aaa", "...aa",
                                         "....a"};
static uint16_t sa_arrow_c;
static int32_t sa_arrow_col(char ch) { return ch == 'a' ? sa_arrow_c : -1; }

typedef struct {
    const char *title, *right;               /* the chips */
    uint16_t title_bg, title_fg;
    const char *name[4], *label[4];          /* each option's name (the centre chip) and its cell's label */
    const char *val[4], *prev, *next;        /* the values; the focused one's neighbours */
    uint8_t col[4];                          /* SA_TK_* */
    uint8_t focus;
} sa_optmenu_t;

static void sa_paint_optmenu(const sa_optmenu_t *m)
{
    int32_t i, w, nudge = ui.now % 600u < 300u ? 0 : 2, f = m->focus;
    uint16_t fc = ui.c[m->col[f]];
    sg_chip_text(10, 8, 0, 22, 7, m->title, m->title_bg, m->title_fg);
    if (m->right) {
        w = sg_text_w(m->right, 1) + 14;
        sg_chip_text(230 - w, 8, w, 22, 7, m->right, UC(PANEL), UC(INK));
    }
    w = sg_text_w(m->name[f], 1) + 20;
    sg_chip_text((SG_W - w) / 2, 40, w, 22, 0, m->name[f], fc, UC(ON));
    sa_arrow_c = fc;
    if (m->prev)
        sg_sprite(10 - nudge, 80, 3, SA_ARROW_L, 9, sa_arrow_col);
    if (m->next)
        sg_sprite(215 + nudge, 80, 3, SA_ARROW, 9, sa_arrow_col);
    {
        int32_t n = 5;
        while (n > 2 && sg_text_w(m->val[f], n) > 176)
            n--;
        sg_text_sh((SG_W - sg_text_w(m->val[f], n)) / 2, 75, n, m->val[f], UC(INK), 4, UC(D5));
    }
    if (m->prev || m->next) {                /* the neighbours: the one before left, after right */
        const char *a = m->prev ? m->prev : "", *b = m->next ? m->next : "";
        int32_t wa = sg_text_w(a, 1), wb = sg_text_w(b, 1);
        sg_text(SG_W / 2 - 10 - wa, 134, 1, a, UC(MID));
        sg_text(SG_W / 2 + 10, 134, 1, b, UC(MID));
        (void)wb;
    }
    for (i = 0; i < 4; i++) {                /* the four cells: 53 x 46 from (8, 186), 4 apart */
        int32_t x = 8 + 57 * i, on = i == f;
        uint16_t c = ui.c[m->col[i]];
        sg_chip(x, 186, 53, 46, c);
        sg_chip(x + 2, 188, 49, 42, on ? c : UC(BG));
        sg_text(x + (53 - sg_text_w(m->label[i], 1)) / 2, 195, 1, m->label[i], on ? UC(ON) : c);
        sg_text(x + (53 - sg_text_w(m->val[i], 1)) / 2, 213, 1, m->val[i], on ? UC(ON) : UC(INK));
    }
}

static const char *const SA_RLEN[SA_REC_NLENS] = {"FREE", "1 BAR", "2 BARS", "4 BARS", "8 BARS"};
static const char *const SA_RLEN_S[SA_REC_NLENS] = {"FREE", "1 BAR", "2 BAR", "4 BAR", "8 BAR"};
static const char *const SA_RQ[3] = {"OFF", "1/8", "1/16"};
static const char *const SA_ONOFF[2] = {"OFF", "ON"};
static const char *const SA_LAYER[2] = {"REPLACE", "DUB"};
static const char *const SA_LAYER_S[2] = {"NEW", "DUB"};

static void sa_paint_recopt(void)
{
    static const char *const *const BIG[4] = {SA_RLEN, SA_RQ, SA_ONOFF, SA_LAYER};
    static const uint8_t N[4] = {SA_REC_NLENS, 3, 2, 2};
    sa_optmenu_t m = {"REC", 0, 0, 0, {"LENGTH", "QUANTIZE", "COUNT-IN", "LAYER"}, {"LEN", "QUANT", "COUNT", "LAYER"},
                      {0}, 0, 0, {SA_TK_A5, SA_TK_A1, SA_TK_A3, SA_TK_A4}, ui.mfocus & 3u};
    uint32_t f = m.focus, v = ui.ropt[f];
    m.title_bg = UC(D1);
    m.title_fg = UC(INK);
    m.val[0] = SA_RLEN_S[ui.ropt[0]];
    m.val[1] = SA_RQ[ui.ropt[1]];
    m.val[2] = SA_ONOFF[ui.ropt[2]];
    m.val[3] = SA_LAYER_S[ui.ropt[3]];
    m.prev = v > 0u ? BIG[f][v - 1u] : 0;
    m.next = v + 1u < N[f] ? BIG[f][v + 1u] : 0;
    {
        sa_optmenu_t big = m;               /* (the centre shows the long name: "2 BARS", "REPLACE") */
        const char *vals[4] = {m.val[0], m.val[1], m.val[2], m.val[3]};
        vals[f] = BIG[f][v];
        big.val[f] = vals[f];
        sa_paint_optmenu(&big);
        if (sg_hits(186, 46)) {              /* (the cells keep the short ones) */
            int32_t x = 8 + 57 * (int32_t)f;
            uint16_t c = ui.c[m.col[f]];
            sg_chip(x + 2, 188, 49, 42, c);
            sg_text(x + (53 - sg_text_w(m.label[f], 1)) / 2, 195, 1, m.label[f], UC(ON));
            sg_text(x + (53 - sg_text_w(m.val[f], 1)) / 2, 213, 1, m.val[f], UC(ON));
        }
    }
}

/* ------------------------------------------------------------------ the dispatch --- */
static void sa_ui_snap_more(void)
{
    if (ui.scr == SA_SCR_HOME && sl_recording())
        ui.scr = SA_SCR_REC;                 /* (a recording: the Rec screen is HOME's) */
    sa_ui_snap_loop();
}

static int sa_paint_more(void)               /* 1: painted (a screen after S1) */
{
    switch (ui.scr) {
    case SA_SCR_CCBANK: sa_paint_ccbank(); return 1;
    case SA_SCR_CCTURN: sa_paint_ccturn(); return 1;
    case SA_SCR_CCASSIGN: sa_paint_ccassign(); return 1;
    case SA_SCR_SAVED: sa_paint_saved(); return 1;
    case SA_SCR_REC: sa_paint_rec(); return 1;
    case SA_SCR_LOOP: sa_paint_loopplay(); return 1;
    case SA_SCR_LOOPS: sa_paint_loops(); return 1;
    case SA_SCR_RECOPT: sa_paint_recopt(); return 1;
    default: return 0;
    }
}
