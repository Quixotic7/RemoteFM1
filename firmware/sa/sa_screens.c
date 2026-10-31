/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's screens after S1 (design/standalone-mockup, halved): the CC screens (CC-Bank, CC-Turn, CC-Assign)
 * and the save's note. Part of sa_ui.c (its snapshot, ui; sa_gfx.c's primitives). */

static uint32_t sa_slen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

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
    ui.launch = sa.loop_launch;
    ui.lfull = sl.used + sl.take_n >= SA_LPOOL || sl.drops != 0u;
    for (i = 0; i < SA_LOOPS; i++)
        ui.lfilled[i] = sl.len[i] != 0u;
    ui.llen = sl.state == SL_REC ? sl.rec_target : sl.len[k];
    ui.lpos = sl_pos();
    ui.lrec_ms = sl.state == SL_REC ? ui.now - sl.rec_t0 : sl.state == SL_ARM ? 0u : ui.lpos;
    ui.count_left = (uint8_t)(sl.state == SL_COUNT ? (sl.count_end - ui.now) * 4u / sl_bar_ms() + 1u : 0u);
    ui.ropt[0] = sa.rec_len, ui.ropt[1] = sa.rec_quant, ui.ropt[2] = sa.rec_layer, ui.ropt[3] = 0;
    ui.ropt[4] = sa.rec_count, ui.ropt[5] = sa.rec_metro, ui.ropt[6] = sa.rec_click, ui.ropt[7] = 0;
    ui.rpage = sa.rpage;
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
    const char *lab = ui.lstate == SL_DUB ? "DUB" : ui.lstate == SL_COUNT ? "COUNT" : ui.lstate == SL_ARM ? "ARMED" :
                      ui.lfull ? "FULL" : "REC";
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

/* ------------------------------------------------------------------ Loop-Pick: the 16 loops (SEQ) --- */
/* No cursor (the owner, 2026-10-30): the current loop lit (playing: filled, with its progress; stopped: outlined),
 * the one queued blinking; the white keys pick, the black keys or KN1 the launch (top right) */
static const char *const SA_LAUNCH[3] = {"INSTANT", "AT BAR", "AT END"};
static void sa_paint_loops(void)
{
    int32_t i, w, blink = ui.now % 1000u < 500u;
    const char *ln = SA_LAUNCH[ui.launch % 3u];
    sg_chip_text(10, 8, 0, 22, 7, "LOOPS", UC(A4), UC(ON));
    w = sg_text_w(ln, 1) + 14;
    sg_chip_text(230 - w, 8, w, 22, 7, ln, UC(A3), UC(ON));
    for (i = 0; i < 16; i++) {               /* 4 x 4 from (15, 36): 48 x 43, 6 apart */
        int32_t x = 15 + 54 * (i % 4), y = 36 + 49 * (i / 4);
        int cur = i == ui.lcur, playing = cur && (ui.lstate == SL_PLAY || ui.lstate == SL_DUB), queued = i == ui.lnext;
        uint16_t line = cur ? UC(A4) : queued ? UC(A3) : ui.lfilled[i] ? UC(PANEL) : UC(D5);
        uint16_t bg = playing ? UC(A4) : queued || cur ? UC(BG) : ui.lfilled[i] ? UC(PANEL) : UC(BG);
        uint16_t fg = playing ? UC(ON) : queued ? (blink ? UC(A3) : UC(MID)) : cur ? UC(A4) :
                      ui.lfilled[i] ? UC(INK) : UC(MID);
        char t[4];
        sg_chip(x, y, 48, 43, line);
        sg_chip(x + (cur ? 4 : 2), y + (cur ? 4 : 2), cur ? 40 : 44, cur ? 35 : 39, bg);
        sa_txt_num(t, (uint32_t)i + 1u);
        sg_text(x + (48 - sg_text_w(t, 2)) / 2, y + 8, 2, t, fg);
        if (playing) {
            sg_rect(x + 6, y + 34, 36, 4, UC(D2));
            sg_rect(x + 6, y + 34, ui.llen ? (int32_t)(ui.lpos * 36u / ui.llen) : 0, 4, UC(BG));
        }
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
static const char *const SA_RCOUNT[SA_REC_NCOUNTS] = {"NONE", "1 BAR", "2 BARS", "4 BARS"};
static const char *const SA_RCOUNT_S[SA_REC_NCOUNTS] = {"NONE", "1 BAR", "2 BAR", "4 BAR"};
static const char *const SA_CLICK[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};

/* the recording options: two pages (SELECT), TAKE (LENGTH, QUANTIZE, LAYER) and COUNT (COUNT-IN, METRONOME, its
 * volume), a knob each; the knob last turned big in the middle */
static void sa_paint_recopt(void)
{
    static const char *const *const BIG[8] = {SA_RLEN, SA_RQ, SA_LAYER, 0, SA_RCOUNT, SA_ONOFF, SA_CLICK, 0};
    static const char *const *const SMALL[8] = {SA_RLEN_S, SA_RQ, SA_LAYER_S, 0, SA_RCOUNT_S, SA_ONOFF, SA_CLICK, 0};
    static const uint8_t N[8] = {SA_REC_NLENS, 3, 2, 0, SA_REC_NCOUNTS, 2, 10, 0}, LO[8] = {0, 0, 0, 0, 0, 0, 1, 0};
    static const char *const NAME[8] = {"LENGTH", "QUANTIZE", "LAYER", 0, "COUNT-IN", "METRONOME", "CLICK VOL", 0};
    static const char *const LAB[8] = {"LEN", "QUANT", "LAYER", "", "COUNT", "METRO", "CLICK", ""};
    uint32_t p = ui.rpage & 1u, f = ui.mfocus < 3u ? ui.mfocus : 0u, o = p * 4u + f, v = ui.ropt[o] - LO[o], i;
    sa_optmenu_t m = {"REC", p ? "COUNT 2/2" : "TAKE 1/2", 0, 0, {0}, {0}, {0}, 0, 0,
                      {SA_TK_A5, SA_TK_A1, SA_TK_A3, SA_TK_PANEL}, (uint8_t)f};
    m.title_bg = UC(D1);
    m.title_fg = UC(INK);
    for (i = 0; i < 4u; i++) {
        uint32_t q = p * 4u + i;
        m.name[i] = NAME[q] ? NAME[q] : "";
        m.label[i] = LAB[q];
        m.val[i] = SMALL[q] ? SMALL[q][ui.ropt[q] - LO[q]] : "";
    }
    m.prev = v > 0u ? BIG[o][v - 1u] : 0;
    m.next = v + 1u < N[o] ? BIG[o][v + 1u] : 0;
    {
        sa_optmenu_t big = m;                /* (the centre shows the long name: "2 BARS", "REPLACE") */
        big.val[f] = BIG[o][v];
        sa_paint_optmenu(&big);
        if (sg_hits(186, 46)) {              /* (the cells keep the short ones; the fourth, empty, dark) */
            int32_t x = 8 + 57 * (int32_t)f;
            uint16_t c = ui.c[m.col[f]];
            sg_chip(x + 2, 188, 49, 42, c);
            sg_text(x + (53 - sg_text_w(m.label[f], 1)) / 2, 195, 1, m.label[f], UC(ON));
            sg_text(x + (53 - sg_text_w(m.val[f], 1)) / 2, 213, 1, m.val[f], UC(ON));
            sg_chip(8 + 57 * 3, 186, 53, 46, UC(D5));
            sg_chip(10 + 57 * 3, 188, 49, 42, UC(BG));
        }
    }
}

/* ------------------------------------------------------------------ S4: Arp, Arp settings, Tempo --- */
static const char *const SA_AMODE[SA_ARP_NMODES] = {"UP", "DOWN", "UP DOWN", "RANDOM", "PLAYED"};
static const char *const SA_AMODE_S[SA_ARP_NMODES] = {"UP", "DOWN", "UP/DN", "RAND", "ORDER"};
static const char *const SA_ARATE[SA_ARP_NRATES] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const SA_AOCT[4] = {"1", "2", "3", "4"};
static const char *const SA_AGATE[10] = {"10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};

static void sa_bpm_chip(int32_t y, int bpm_word)
{
    char t[12];
    int32_t w;
    sa_txt_num(t, ui.bpm);
    if (bpm_word)
        sa_cpy(t + (ui.bpm >= 100u ? 3 : 2), ui.clk ? " EXT" : " BPM");
    w = sg_text_w(t, 1) + 14;
    sg_chip_text(230 - w, y, w, 22, 7, t, ui.clk ? UC(A2) : UC(PANEL), ui.clk ? UC(ON) : UC(INK));
}

/* eight steps (16, 28, 40, 52 high, twice) with the ball hopping on the one that sounds */
static void sa_steps(uint32_t step, int lit)
{
    static const uint8_t H[4] = {16, 28, 40, 52};
    static const char *const BALL[] = {"..aaaaaa..", ".aaaaaaaa.", "aaaaaaaaaa", "aaaaaaaaaa", "aaaaaaaaaa",
                                       "aaaaaaaaaa", "aaaaaaaaaa", "aaaaaaaaaa", ".aaaaaaaa.", "..aaaaaa.."};
    int32_t i, s = (int32_t)(step % 8u);
    for (i = 0; i < 8; i++)
        sg_rect(18 + 26 * i, 218 - H[i & 3], 22, H[i & 3], lit && i == s ? UC(A4) : UC(D2));
    if (lit) {
        sa_arrow_c = UC(A3);
        sg_sprite(23 + 26 * s, 218 - H[s & 3] - 14, 1, BALL, 10, sa_arrow_col);
    }
}

static void sa_paint_arp(void)
{
    const char *m = ui.arp_on ? "ON" : "OFF";   /* (the owner, 2026-10-30: on / off only, not the pattern) */
    int32_t n = 7;
    sg_chip_text(10, 8, 0, 22, 7, "ARP", ui.arp_on ? UC(A4) : UC(PANEL), ui.arp_on ? UC(ON) : UC(INK));
    sa_bpm_chip(8, 0);
    while (n > 3 && sg_text_w(m, n) > 224)
        n--;
    sg_text_sh((SG_W - sg_text_w(m, n)) / 2, 52 + (9 - n) * 4, n, m, ui.arp_on ? UC(INK) : UC(MID), 4, UC(D2));
    sa_steps(ui.arp_step, ui.arp_on);
}

static void sa_paint_arpset(void)
{
    static const char *const *const BIG[4] = {SA_AMODE, SA_ARATE, SA_AOCT, SA_AGATE};
    static const uint8_t LO[4] = {0, 0, 1, 1}, N[4] = {SA_ARP_NMODES, SA_ARP_NRATES, 4, 10};
    char bpm[12];
    sa_optmenu_t m = {"ARP", 0, 0, 0, {"MODE", "RATE", "OCTAVES", "GATE"}, {"MODE", "RATE", "OCT", "GATE"},
                      {0}, 0, 0, {SA_TK_A5, SA_TK_A1, SA_TK_A3, SA_TK_A4}, ui.mfocus & 3u};
    uint32_t f = m.focus, v = ui.aopt[f] - LO[f];
    sa_txt_num(bpm, ui.bpm);
    sa_cpy(bpm + (ui.bpm >= 100u ? 3 : 2), ui.clk ? " EXT" : " BPM");
    m.right = bpm;
    m.title_bg = UC(A4);
    m.title_fg = UC(ON);
    m.val[0] = SA_AMODE_S[ui.aopt[0]];
    m.val[1] = SA_ARATE[ui.aopt[1]];
    m.val[2] = SA_AOCT[ui.aopt[2] - 1u];
    m.val[3] = SA_AGATE[ui.aopt[3] - 1u];
    m.prev = v > 0u ? BIG[f][v - 1u] : 0;
    m.next = v + 1u < N[f] ? BIG[f][v + 1u] : 0;
    if (f == 0u) {                           /* (the centre: the mode's long name; its cell the short one) */
        sa_optmenu_t big = m;
        big.val[0] = SA_AMODE[ui.aopt[0]];
        sa_paint_optmenu(&big);
        if (sg_hits(186, 46)) {
            sg_chip(10, 188, 49, 42, UC(A5));
            sg_text(8 + (53 - sg_text_w("MODE", 1)) / 2, 195, 1, "MODE", UC(ON));
            sg_text(8 + (53 - sg_text_w(m.val[0], 1)) / 2, 213, 1, m.val[0], UC(ON));
        }
    } else {
        sa_paint_optmenu(&m);
    }
}

static void sa_paint_tempo(void)
{
    char t[4];
    uint32_t beat = (uint32_t)((uint64_t)ui.now * ui.bpm / 60000u);
    sg_chip_text(10, 8, 0, 22, 7, "TEMPO", UC(A3), UC(ON));
    sg_chip_text(ui.clk ? 167 : 175, 8, 0, 22, 7, ui.clk ? "MIDI CLK" : "BPM", ui.clk ? UC(A2) : UC(PANEL),
                 ui.clk ? UC(ON) : UC(INK));
    sa_txt_num(t, ui.bpm);
    sg_text_sh((SG_W - sg_text_w(t, 9)) / 2, 52, 9, t, UC(INK), 4, UC(D2));
    sa_steps(beat, 1);
}

/* ------------------------------------------------------------------ S5: HOME's Scope and Keys views --- */
/* the Scope: the last SA_SCOPE_N samples (2 output samples apart), from a rising zero crossing (with hysteresis: the
 * wave below -1/8 of its peak before it), the first that leaves the window: the picture stands still. Auto gain: the
 * window's peak fills SA_SCOPE_AMP pixels, the gain falling at once and rising ~25 % a frame, at most SA_SCOPE_GMAX
 * (quieter than that: drawn as it is; silence: flat) */
#define SA_SCOPE_AMP 80
#define SA_SCOPE_GMAX (80.0f / 300.0f)       /* (a peak of ~1 % of full scale fills the screen) */
static void sa_ui_snap_views(void)
{
    static int16_t buf[SA_SCOPE_N];
    uint32_t i, w = sa_scope_w, trig = 0, span = 2u * SA_WAVE_N;
    int32_t pk = 0, thr, armed = 0;
    float g;
    for (i = 0; i < SA_SCOPE_N; i++) {        /* the ring, oldest first */
        int32_t v = buf[i] = sa_scope[(w + i) % SA_SCOPE_N];
        v = v < 0 ? -v : v;
        pk = v > pk ? v : pk;
    }
    thr = pk / 8;
    for (i = 1; i + span <= SA_SCOPE_N; i++) {
        if (buf[i - 1u] < -thr)
            armed = 1;
        if (armed && buf[i - 1u] < 0 && buf[i] >= 0) {
            trig = i;
            break;
        }
    }
    if (i + span > SA_SCOPE_N)
        trig = SA_SCOPE_N - span;            /* (no crossing: the newest) */
    pk = 0;
    for (i = 0; i < span; i++) {
        int32_t v = buf[trig + i];
        v = v < 0 ? -v : v;
        pk = v > pk ? v : pk;
    }
    ui.wave_on = pk >= 8;
    g = pk ? (float)SA_SCOPE_AMP / (float)pk : SA_SCOPE_GMAX;
    if (g > SA_SCOPE_GMAX)
        g = SA_SCOPE_GMAX;
    if (uis.sgain <= 0.0f || g < uis.sgain)
        uis.sgain = g;                       /* louder: at once */
    else
        uis.sgain = uis.sgain * 1.25f < g ? uis.sgain * 1.25f : g;
    for (i = 0; i < SA_WAVE_N; i++) {
        int32_t y = (int32_t)((float)buf[trig + 2u * i] * uis.sgain);
        ui.wave[i] = (int16_t)(y > SA_SCOPE_AMP ? SA_SCOPE_AMP : y < -SA_SCOPE_AMP ? -SA_SCOPE_AMP : y);
    }
    ui.view = sa.view;
    ui.theme = sa.theme;
    ui.keyheld = 0;
    for (i = 0; i < NKEYS; i++) {
        int32_t n = (int32_t)SA_NOTE0 + (int32_t)i + 12 * sa.oct;
        if ((sa.keys >> i & 1u) || (n >= 0 && n < 128 && (sa.held[n] || sa.arp_show == n + 1)))
            ui.keyheld |= 1u << i;
    }
    ui.lvl8 = (uint8_t)(uis.lvl * 8.0f + 0.5f);
}

static void sa_home_chips(void)              /* the channel, LATCH when on, the voice */
{
    int32_t w = sg_text_w(ui.name, 1) + 14;
    sg_chip_text(10, 8, 0, 22, 7, ui.ch_txt, UC(A2), UC(ON));
    if (ui.latch)
        sg_chip_text(16 + sg_text_w(ui.ch_txt, 1) + 14, 8, 0, 22, 7, "LATCH", UC(A3), UC(ON));
    sg_chip_text(230 - w, 8, w, 22, 7, ui.name, UC(PANEL), UC(INK));
}

static void sa_chord_txt(int32_t y, int32_t n, uint16_t sh)   /* the chord (its extension in the accent), centred */
{
    int32_t w = sg_text_w(ui.chord.main, n) + sg_text_w(ui.chord.ext, n) + sg_text_w(ui.chord.bass, n), x;
    while (n > 2 && w > 224) {
        n--;
        w = sg_text_w(ui.chord.main, n) + sg_text_w(ui.chord.ext, n) + sg_text_w(ui.chord.bass, n);
    }
    x = (SG_W - w) / 2;
    if (!ui.ghost) {
        int32_t xs = sg_text(x + 4, y + 4, n, ui.chord.main, sh);
        xs = sg_text(xs, y + 4, n, ui.chord.ext, sh);
        sg_text(xs, y + 4, n, ui.chord.bass, sh);
    }
    x = sg_text(x, y, n, ui.chord.main, ui.ghost ? UC(MID) : UC(INK));
    x = sg_text(x, y, n, ui.chord.ext, ui.ghost ? UC(MID) : UC(A1));
    sg_text(x, y, n, ui.chord.bass, ui.ghost ? UC(MID) : UC(INK));
}

static void sa_paint_scope(void)
{
    int32_t i, prev = -1;
    char ch[16];
    for (i = 0; i < SG_W; i += 8)            /* the grid: the zero line, three verticals, dashed */
        sg_rect(i, 119, 4, 2, UC(PANEL));
    for (i = 36; i < 204; i += 8) {
        sg_rect(59, i, 2, 4, UC(D5));
        sg_rect(119, i, 2, 4, UC(D5));
        sg_rect(179, i, 2, 4, UC(D5));
    }
    for (int32_t pass = 0; pass < 2; pass++) {   /* the wave, 3 pixels thick, 2 a point; its shadow first */
        prev = -1;
        for (i = 0; i < (int32_t)SA_WAVE_N; i++) {
            int32_t y = 119 - (ui.wave_on ? ui.wave[i] : 0), top = prev < 0 ? y : (prev < y ? prev : y),
                    bot = (prev < 0 ? y : (prev > y ? prev : y)) + 3, o = pass ? 0 : 3;
            if (sg_hits(top - 1 + o, bot - top + 2))
                sg_rect(2 * i + o, top - 1 + o, 2, bot - top, pass ? UC(A4) : UC(D2));
            prev = y;
        }
    }
    sa_home_chips();
    sa_cpy(ch, ui.chord.main);
    sa_cpy(ch + sa_slen(ch), ui.chord.ext);
    if (ui.ghost && !uis.n && !ui.chord.main[0])
        ch[0] = 0;
    if (ch[0])
        sg_chip_text(10, 210, 0, 22, 7, ch, ui.ghost ? UC(PANEL) : UC(INK), ui.ghost ? UC(MID) : UC(ON));
    for (i = 0; i < 8; i++)                  /* the level, 8 segments */
        sg_rect(166 + 8 * i, 217, 6, 14, i < ui.lvl8 ? (i < 4 ? UC(A4) : i < 6 ? UC(A3) : UC(A1)) : UC(PANEL));
}

static void sa_paint_keys(void)
{
    static const uint8_t BLACK_AFTER[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};
    static const uint8_t WKEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* F G A B C ..*/
    static const uint8_t BKEY[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
    char lo[6], hi[6], oc[8];
    int32_t i, w;
    sa_home_chips();
    sa_chord_txt(42, 5, UC(SHADOW));
    for (i = 0; i < 16; i++) {               /* 16 white keys, 12 wide, 2 apart; a held one drops and lights */
        int on = ui.keyheld >> WKEY[i] & 1u;
        sg_rect(9 + 14 * i, 106 + (on ? 4 : 0), 12, 90, on ? UC(A1) : UC(INK));
    }
    for (i = 0; i < 11; i++) {               /* 11 black keys, 8 wide, outlined */
        int on = ui.keyheld >> BKEY[i] & 1u;
        int32_t x = 9 + 14 * BLACK_AFTER[i] + 9;
        sg_rect(x - 2, 106, 12, 56, UC(BG));
        sg_rect(x, 106, 8, 54, on ? UC(A1) : UC(PANEL));
    }
    sa_note_name((uint32_t)clamp((int32_t)SA_NOTE0 + 12 * ui.oct, 0, 127), lo);
    sa_note_name((uint32_t)clamp((int32_t)SA_NOTE0 + 26 + 12 * ui.oct, 0, 127), hi);
    sa_cpy(oc, "OCT ");
    sa_num(oc + 4, ui.oct, 1);
    sg_chip_text(10, 210, 0, 22, 6, lo, UC(PANEL), UC(INK));
    w = sg_text_w(oc, 1) + 12;
    sg_chip_text((SG_W - w) / 2, 210, w, 22, 6, oc, UC(A5), UC(ON));
    w = sg_text_w(hi, 1) + 12;
    sg_chip_text(230 - w, 210, w, 22, 6, hi, UC(PANEL), UC(INK));
}

/* ------------------------------------------------------------------ S5: the theme menu (SAVE) --- */
static void sa_paint_theme(void)
{
    static const uint8_t SW[6] = {SA_TK_PANEL, SA_TK_A1, SA_TK_A2, SA_TK_A3, SA_TK_A4, SA_TK_INK};
    const char *name = SA_THEMES[ui.theme].name;
    char pos[8];
    int32_t i, w, n = 5, nudge = ui.now % 600u < 300u ? 0 : 2;
    sa_txt_num(pos, ui.theme + 1u);
    sa_cpy(pos + sa_slen(pos), "/");
    sa_txt_num(pos + sa_slen(pos), SA_NTHEMES);
    if (UC(BORDER) != UC(BG)) {              /* the theme's border, 8 wide (the screens' is 4) */
        sg_rect(0, 0, SG_W, 8, UC(BORDER));
        sg_rect(0, SG_H - 8, SG_W, 8, UC(BORDER));
        sg_rect(0, 0, 8, SG_H, UC(BORDER));
        sg_rect(SG_W - 8, 0, 8, SG_H, UC(BORDER));
    }
    sg_chip_text(14, 14, 0, 22, 7, "THEME", UC(A1), UC(ON));
    w = sg_text_w(pos, 1) + 14;
    sg_chip_text(226 - w, 14, w, 22, 7, pos, UC(PANEL), UC(INK));
    sa_arrow_c = UC(A1);
    sg_sprite(12 - nudge, 66, 3, SA_ARROW_L, 9, sa_arrow_col);
    sg_sprite(213 + nudge, 66, 3, SA_ARROW, 9, sa_arrow_col);
    n = 5;                                   /* the name: one line at 5..3 if it fits, else wrapped at 3..1 */
    while (n > 3 && sg_text_w(name, n) > 180)
        n--;
    if (sg_text_w(name, n) > 180)
        n = 3;
    for (; n >= 1; n--) {
        const char *ln[4];
        int32_t nl = 0, lw[4], k, y;
        char buf[4][24];
        const char *p = name;
        while (*p && nl < 4) {               /* greedy: words onto a line while it fits */
            int32_t m = 0;
            buf[nl][0] = 0;
            while (*p) {
                char word[24];
                int32_t wl = 0, cw;
                while (p[wl] && p[wl] != ' ' && wl < 23)
                    word[wl] = p[wl], wl++;
                word[wl] = 0;
                if (m) {
                    char t[24];
                    sa_cpy(sa_cpy(sa_cpy(t, buf[nl]), " "), word);
                    cw = sg_text_w(t, n);
                    if (cw > 180)
                        break;
                    sa_cpy(buf[nl], t);
                } else {
                    sa_cpy(buf[nl], word);
                }
                m++;
                p += wl;
                while (*p == ' ')
                    p++;
            }
            ln[nl] = buf[nl];
            lw[nl] = sg_text_w(buf[nl], n);
            nl++;
        }
        for (k = 0; k < nl && lw[k] <= 180; k++)
            ;
        if ((k < nl || *p || nl * (12 * n + 2) > 80) && n > 1)
            continue;
        y = 40 + (80 - nl * (12 * n + 2)) / 2 + 2 * n;
        for (k = 0; k < nl; k++)
            sg_text_sh((SG_W - lw[k]) / 2, y + k * (12 * n + 2), n, ln[k], UC(INK), 3, UC(SHADOW));
        break;
    }
    for (i = 0; i < 6; i++) {                /* the swatches */
        sg_chip(27 + 32 * i, 118, 26, 26, UC(BG));
        sg_chip(29 + 32 * i, 120, 22, 22, ui.c[SW[i]]);
    }
    {                                        /* a chord in the theme */
        int32_t x = (SG_W - sg_text_w("Dm7", 4)) / 2;
        sg_text(x + 3, 150 + 3, 4, "Dm", UC(SHADOW));
        sg_text(x + 3 + sg_text_w("Dm", 4), 153, 4, "7", UC(SHADOW));
        x = sg_text(x, 150, 4, "Dm", UC(INK));
        sg_text(x, 150, 4, "7", UC(A1));
    }
    {                                        /* the background (ALGORITHM): its name in a chip, a swatch of it */
        char bg[16];
        sa_cpy(sa_cpy(bg, "BG "), sa_bg_name(ui.theme, ui.bg));
        int32_t x;
        w = sg_text_w(bg, 1) + 14;
        x = (SG_W - w - 20) / 2;
        sg_chip_text(x, 195, w, 22, 7, bg, UC(PANEL), UC(INK));
        sg_rect(x + w + 6, 199, 14, 14, UC(INK));
        sg_rect(x + w + 8, 201, 10, 10, UC(BG));
    }
    for (i = 0; i < (int32_t)SA_NTHEMES; i++)   /* where it is among them */
        sg_rect(48 + 16 * i, 224, 10, 6, i == ui.theme ? UC(A1) : UC(PANEL));
}

/* ------------------------------------------------------------------ MIDI REMOTE (sa_remote.c) --- */
static void sa_paint_remote(void)
{
    static const char *const BN[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY",
                                       "REC", "OCT-", "OCT+"};
    static const char *const EN[NE] = {"SELECT", "ALGORITHM", "PRESETS", "KN1", "KN2", "KN3", "KN4"};
    static const char HEX[] = "0123456789ABCDEF";
    char src[12], what[12], big[8], raw[12];
    uint32_t st = ui.rm_last[0] & 0xF0u, v = ui.rm_last[2];
    int32_t i, w, lit;
    uint16_t acc = UC(A2);
    /* (the owner, 2026-10-30: no header, no channels chip, no source chip, no raw bytes: the CC and its value) */
    (void)BN, (void)EN, (void)HEX, (void)raw, (void)src;
    if ((ui.rm_info == SA_RM_INFO_OCT || ui.rm_info == SA_RM_INFO_KEYCH) && ui.rm_info_age < SA_RM_INFO_MS) {
        int oct = ui.rm_info == SA_RM_INFO_OCT;   /* MIDI KEYBOARD's shortcuts: the octave, the keys' channel */
        sa_num(big, oct ? (int32_t)ui.rm_oct - 3 : (int32_t)ui.rm_keych, oct);
        w = sg_text_w(oct ? "OCTAVE" : "KEY CHANNEL", 1) + 16;
        sg_chip_text((SG_W - w) / 2, 44, w, 22, 7, oct ? "OCTAVE" : "KEY CHANNEL", oct ? UC(A5) : UC(A2), UC(ON));
        sg_text_sh((SG_W - sg_text_w(big, 7)) / 2, 90, 7, big, oct ? UC(A5) : UC(A2), 4, UC(SHADOW));
        for (i = 0; i < (oct ? 7 : 16); i++) {   /* where it is: 7 octaves, or 16 channels */
            int32_t n = oct ? 7 : 16, cw = oct ? 26 : 11, x = (SG_W - n * (cw + 2)) / 2 + i * (cw + 2);
            int on = oct ? i == (int32_t)ui.rm_oct : i + 1 == (int32_t)ui.rm_keych;
            sg_rect(x, 200, cw, 14, on ? (oct ? UC(A5) : UC(A2)) : UC(PANEL));
        }
        return;
    }
    if (!ui.rm_any) {                        /* nothing sent yet */
        sg_text_sh((SG_W - sg_text_w("MIDI", 5)) / 2, 90, 5, "MIDI", UC(MID), 4, UC(SHADOW));
        return;
    }
    switch (ui.rm_src) {
    case SA_RM_SRC_KEY: acc = UC(A1); break;
    case SA_RM_SRC_BTN: acc = UC(A3); break;
    case SA_RM_SRC_ENC: acc = ui.rm_src_i >= EN_K1 ? sa_kcol(ui.rm_src_i - EN_K1) : UC(A2); break;
    default: acc = UC(A4); break;
    }
    if (st == 0x90u || st == 0x80u) {        /* a note: its name big, the velocity */
        sa_cpy(what, st == 0x90u ? "NOTE ON" : "NOTE OFF");
        sa_note_name(ui.rm_last[1], big);
    } else {                                 /* a CC: its number, the value big (relative: the step, signed) */
        sa_cpy(what, "CC ");
        sa_txt_num(what + 3, ui.rm_last[1]);
        if (ui.rm_src == SA_RM_SRC_ENC && ui.set[1] != SA_RMR_ABS) {
            sa_num(big, ui.rm_rel, 1);
            v = (uint32_t)clamp(64 + 8 * ui.rm_rel, 0, 127);
        } else {
            sa_txt_num(big, v);
        }
    }
    w = sg_text_w(what, 3);                  /* "CC 74" (or NOTE ON): big enough to read */
    sg_text_sh((SG_W - w) / 2, 34, 3, what, UC(INK), 3, UC(SHADOW));
    sg_text_sh((SG_W - sg_text_w(big, 6)) / 2, 92, 6, big, acc, 4, UC(SHADOW));
    lit = (int32_t)(v * 16u + 63u) / 127;
    for (i = 0; i < 16; i++)                 /* the value (the velocity), 16 segments */
        sg_rect(17 + 13 * i, 196, 11, 16, i < lit ? acc : UC(PANEL));
}

/* ------------------------------------------------------------------ LATCH (LFO held) --- */
static void sa_paint_latch(void)
{
    static const char *const LOCK[] = {"..aaaa..", ".a....a.", ".a....a.", "aaaaaaaa", "aaaabaaa", "aaabbbaa",
                                       "aaaabaaa", "aaaaaaaa"};
    const char *v = ui.latch ? "ON" : "OFF";
    int32_t i;
    sg_chip_text(10, 8, 0, 22, 7, "LFO HOLD", UC(PANEL), UC(INK));
    sa_arrow_c = ui.latch ? UC(A3) : UC(MID);
    sg_sprite(96, 36, 6, LOCK, 8, sa_arrow_col);
    if (!ui.latch)                           /* (open: the shackle lifted) */
        sg_rect(96 + 6 * 6, 36 + 6, 6, 12, UC(BG));
    sg_text_sh((SG_W - sg_text_w("LATCH", 4)) / 2, 100, 4, "LATCH", UC(INK), 4, UC(SHADOW));
    sg_text_sh((SG_W - sg_text_w(v, 5)) / 2, 150, 5, v, ui.latch ? UC(A3) : UC(MID), 4, UC(D1));
    for (i = 0; i < 8; i++)
        sg_rect(18 + 26 * i, 224, 22, 6, ui.latch ? UC(A3) : UC(PANEL));
}

/* ------------------------------------------------------------------ the scale (GLO + PRESETS / ALGORITHM) --- */
/* Redrawn in the mockup's style (the owner, 2026-10-30, did not like the old keyboard): flat pixel keys as the Keys
 * view's, no bevels; the root in the theme's accent, the scale's notes lit, the others dark; the root and the scale's
 * name big: one octave, C..B, 7 white keys 30 wide and 5 black, under the root and the name (the owner chose this
 * look, 2026-10-30). */

static uint16_t sa_sc_col(uint32_t pc, int black)   /* a key's colour: the root, in the scale, out */
{
    uint32_t d = (pc + 12u - ui.sc_root % 12u) % 12u, mask = SA_SCALES[ui.sc % SA_NSCALES].mask;
    if (d == 0u && ui.sc)
        return UC(A1);
    if (mask >> d & 1u)
        return ui.sc ? (black ? UC(A4) : UC(INK)) : (black ? UC(MID) : UC(SOFT));
    return black ? UC(BG) : UC(D5);
}

static void sa_paint_scale(void)
{
    const char *root = SA_ROOTS[ui.sc_root % 12u], *name = SA_SCALES[ui.sc % SA_NSCALES].name;
    static const uint8_t WPC[7] = {0, 2, 4, 5, 7, 9, 11}, BPC[5] = {1, 3, 6, 8, 10}, BX[5] = {1, 2, 4, 5, 6};
    int32_t i, n, w;
    sg_chip_text(10, 8, 0, 22, 7, "SCALE", UC(A4), UC(ON));
    sg_chip_text(186, 8, 44, 22, 0, "GLO", UC(A3), UC(ON));
    w = sg_text_w(root, 7);
    sg_text_sh((SG_W - w) / 2, 38, 7, root, ui.sc ? UC(A1) : UC(MID), 4, UC(SHADOW));
    for (n = 3; n > 1 && sg_text_w(name, n) > 224; n--)
        ;
    sg_text_sh((SG_W - sg_text_w(name, n)) / 2, 116, n, name, UC(INK), 3, UC(SHADOW));
    for (i = 0; i < 7; i++)              /* 7 white keys, 30 wide, 2 apart, from x 14 */
        sg_rect(14 + 32 * i, 156, 30, 72, sa_sc_col(WPC[i], 0));
    for (i = 0; i < 5; i++) {            /* 5 black keys, 18 wide, a 2-pixel gap of the ground round them */
        int32_t x = 14 + 32 * BX[i] - 10;
        sg_rect(x - 2, 156, 22, 44, UC(BG));
        sg_rect(x, 156, 18, 42, sa_sc_col(BPC[i], 1));
        if (sa_sc_col(BPC[i], 1) == UC(BG))   /* (a dark black key: its outline) */
            sg_rect(x + 2, 156, 14, 40, UC(PANEL)), sg_rect(x + 4, 156, 10, 38, UC(BG));
    }
}

/* ------------------------------------------------------------------ the settings (HOME held, GLO + HOME) --- */
/* "Big options like ChoralRoot" (the owner, 2026-10-30): one setting big in the middle, its value bigger in its
 * colour, the value's place among its choices as stripes, the rows before and after peeking dim above and below, where
 * the row is on the right. SELECT the row, KN1 the value. */
static const char *const SA_SET_LAB[SA_NSET] = {"SCREEN", "KNOBS", "CC CHANNEL", "KEY CHANNEL", "KEYS", "OCT KEYS",
                                                "SCALE SHORTCUTS", "KNOB LEDS", "VELOCITY"};
static uint32_t sa_set_val(uint32_t i, char *num, const char **v)   /* row i's value as text; its choices' count */
{
    static const char *const DISP[SA_RMD_N] = {"VOID", "SOLID", "ALL", "KNOBS ONLY"};
    static const char *const REL[SA_RMR_N] = {"ABSOLUTE", "REL 1/127", "REL 65/63"};
    static const char *const KEYS[SA_RMK_N] = {"FIXED", "MIDI KEYS"};
    static const char *const OCTK[2] = {"CC 85/86", "OCTAVE"};
    switch (i) {
    case 0: *v = DISP[ui.set[0] % SA_RMD_N]; return SA_RMD_N;
    case 1: *v = REL[ui.set[1] % SA_RMR_N]; return SA_RMR_N;
    case 4: *v = KEYS[ui.set[4] % SA_RMK_N]; return SA_RMK_N;
    case 5: *v = OCTK[ui.set[5] & 1u]; return 2;
    case 6: *v = SA_ONOFF[ui.set[6] & 1u]; return 2;
    case 7: *v = SA_ONOFF[ui.set[7] & 1u]; return 2;
    case 8: *v = SA_ONOFF[ui.set[8] & 1u]; return 2;
    default: sa_txt_num(num, ui.set[i]); *v = num; return 16;
    }
}

static void sa_paint_settings(void)
{
    static const uint8_t COL[SA_NSET] = {SA_TK_A5, SA_TK_A1, SA_TK_A3, SA_TK_A4, SA_TK_A2, SA_TK_A5, SA_TK_A1, SA_TK_A3,
                                          SA_TK_A4};
    int32_t f = ui.mfocus < SA_NSET ? ui.mfocus : 0, n, i, w, nv, cur, sw, x0;
    uint16_t c = ui.c[COL[f]];
    char num[4];
    const char *v;
    sg_chip_text(10, 8, 220, 22, 0, "SETTINGS", UC(A2), UC(ON));   /* (was REMOTE MODE SETTINGS: VELOCITY is the
                                                                     * instrument's too) */
    if (f > 0)                               /* the row before, peeking above */
        sg_text_c(38, 2, SA_SET_LAB[f - 1], UC(PANEL));
    for (n = 3; n > 1 && sg_text_w(SA_SET_LAB[f], n) > 210; n--)
        ;
    sg_text_sh((SG_W - sg_text_w(SA_SET_LAB[f], n)) / 2, 64, n, SA_SET_LAB[f], UC(INK), 3, UC(SHADOW));
    nv = (int32_t)sa_set_val((uint32_t)f, num, &v);
    for (n = 5; n > 2 && sg_text_w(v, n) > 214; n--)
        ;
    sg_text_sh((SG_W - sg_text_w(v, n)) / 2, 100 + (5 - n) * 5, n, v, c, 4, UC(SHADOW));
    /* the value's place: a stripe a choice, the value's lit */
    cur = f == 2 || f == 3 ? ui.set[f] - 1 : ui.set[f];
    sw = (200 - 3 * (nv - 1)) / nv;
    x0 = (SG_W - (nv * sw + 3 * (nv - 1))) / 2;
    for (i = 0; i < nv; i++)
        sg_rect(x0 + i * (sw + 3), 164, sw, 8, i == cur ? c : UC(D5));
    if (f + 1 < (int32_t)SA_NSET)            /* the row after, peeking below */
        sg_text_c(182, 2, SA_SET_LAB[f + 1], UC(PANEL));
    for (i = 0; i < (int32_t)SA_NSET; i++)   /* where the row is: a dot each, on the right */
        sg_rect(228, 66 + 12 * i, 6, 8, i == f ? c : UC(PANEL));
    w = sg_text_w("GLO + FX FOR REMOTE", 1) + 16;
    sg_chip_text((SG_W - w) / 2, 210, w, 22, 7, "GLO + FX FOR REMOTE", UC(PANEL), UC(INK));
}

/* ------------------------------------------------------------------ the dispatch --- */
static void sa_ui_snap_more(void)
{
    ui.blank = 0;
    ui.set[0] = sa.rm_disp, ui.set[1] = sa.rm_rel, ui.set[2] = sa.rm_cc_ch, ui.set[3] = sa.rm_key_ch;
    ui.set[4] = sa.rm_keys, ui.set[5] = sa.rm_octk, ui.set[6] = sa.rm_scl, ui.set[7] = sa.rm_kled;
    ui.set[8] = sa.vel_on;
    ui.rm_info = rm.info;
    ui.rm_info_age = ui.now - rm.info_ms;
    ui.rm_oct = (uint8_t)(rm.oct + 3);
    ui.rm_keych = sa.rm_key_ch;
    ui.rm_kbd = sa.rm_keys == SA_RMK_PLAY;
    ui.rm_rel = rm.rel;
    if (rm.on && !sa.panel) {
        ui.scr = SA_SCR_REMOTE;
        if (sa.rm_disp == SA_RMD_VOID) {      /* nothing: black, whatever the theme */
            ui.cbuf[SA_TK_BG] = 0;
            ui.blank = 1;
        } else if (sa.rm_disp == SA_RMD_SOLID) {
            ui.blank = 1;
        } else if (sa.rm_disp == SA_RMD_KNOBS) {   /* the encoders' and MASTER's values a moment, else black */
            int knob = rm.n && (rm.src == SA_RM_SRC_ENC || rm.src == SA_RM_SRC_MASTER) &&
                       ui.now - rm.last_ms < SA_RM_KNOB_MS;
            int info = rm.info == SA_RM_INFO_SCALE && ui.rm_info_age < SA_RM_KNOB_MS;
            (void)knob;                      /* (the owner, 2026-10-30: the last knob stays shown, not black) */
            if (!rm.kn_n && !info) {
                ui.cbuf[SA_TK_BG] = 0;       /* (before any knob was turned: black) */
                ui.blank = 1;
            }
        }
        if (!ui.blank && rm.info == SA_RM_INFO_SCALE && ui.rm_info_age < SA_RM_INFO_MS)
            ui.scr = SA_SCR_SCALE;           /* (GLO + PRESETS / ALGORITHM in MIDI KEYS: the scale a moment) */
    }
    ui.sc_root = sa.scale_root;
    ui.sc = sa.scale;
    memcpy(ui.rm_last, rm.last, 3);
    ui.rm_src = rm.src;
    ui.rm_src_i = rm.src_i;
    ui.rm_any = rm.n != 0u;
    if (rm.on && sa.rm_disp == SA_RMD_KNOBS && rm.kn_n) {   /* KNOBS ONLY: the last knob's, whatever came after */
        memcpy(ui.rm_last, rm.kn_last, 3);
        ui.rm_src = rm.kn_src;
        ui.rm_src_i = rm.kn_src_i;
    }
    ui.rm_age = ui.now - rm.last_ms;
    if (ui.scr == SA_SCR_HOME && sl_recording())
        ui.scr = SA_SCR_REC;                 /* (a recording: the Rec screen is HOME's) */
    sa_ui_snap_loop();
    ui.arp_on = sar.on;
    ui.aopt[0] = sa.arp_mode, ui.aopt[1] = sa.arp_rate, ui.aopt[2] = sa.arp_oct, ui.aopt[3] = sa.arp_gate;
    ui.arp_step = (uint8_t)(sar.step ? sar.step - 1u : 0u);
    ui.clk = (uint8_t)sl_clock_on();
    ui.bpm = sl_bpm_now();
    sa_ui_snap_views();
}

static int sa_paint_more(void)               /* 1: painted (a screen after S1) */
{
    switch (ui.scr) {
    case SA_SCR_HOME:
        if (ui.view == 1u)
            sa_paint_scope();
        else if (ui.view == 2u)
            sa_paint_keys();
        return ui.view == 1u || ui.view == 2u;
    case SA_SCR_THEME: sa_paint_theme(); return 1;
    case SA_SCR_REMOTE: sa_paint_remote(); return 1;
    case SA_SCR_CCBANK: sa_paint_ccbank(); return 1;
    case SA_SCR_CCTURN: sa_paint_ccturn(); return 1;
    case SA_SCR_CCASSIGN: sa_paint_ccassign(); return 1;
    case SA_SCR_SAVED: sa_paint_saved(); return 1;
    case SA_SCR_REC: sa_paint_rec(); return 1;
    case SA_SCR_LOOP: sa_paint_loopplay(); return 1;
    case SA_SCR_LOOPS: sa_paint_loops(); return 1;
    case SA_SCR_RECOPT: sa_paint_recopt(); return 1;
    case SA_SCR_ARP: sa_paint_arp(); return 1;
    case SA_SCR_ARPSET: sa_paint_arpset(); return 1;
    case SA_SCR_TEMPO: sa_paint_tempo(); return 1;
    case SA_SCR_LATCH: sa_paint_latch(); return 1;
    case SA_SCR_SETTINGS: sa_paint_settings(); return 1;
    case SA_SCR_SCALE: sa_paint_scale(); return 1;
    default: return 0;
    }
}
