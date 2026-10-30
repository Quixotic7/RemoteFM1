/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone screens (design/standalone-mockup, the 480 x 480 artboards at half size: the panel's 240 x 240):
 * HOME's Notes view, the Preset, Channel and Octave overlays, and the hand-over screen of panel mode before the host's
 * session. Part of sa_main.c (its state, sa). A frame is a snapshot of what to show (sa_ui_snap: the state, the chord,
 * the bars, the time) painted band by band (sa_gfx.c).
 *
 * Sizes from the mockup, halved: chips 22 high with 4/2 stepped corners, their text the pixel font at scale 1 (capitals
 * 10 high: the mockup's 16 px Jersey 10 is 8.6, the nearest whole scale is 1); the chord at scale 4 (capitals 40: the
 * mockup's 68 px is 36), 3 or 2 when it is too wide; the big numbers at scale 5 (capitals 50: 88 px is 47); the
 * text-shadows 4 pixels down and right. Motion as the mockup's steps(): the chord's pop (scale + 1 for its first
 * 100 ms), the cartridge sliding in, the DIN plug's pins blinking, the furball hopping and blinking, the bars moving in
 * five steps. The bars follow the sound: the output's peak (sa_render) with each bar's own stepped wave. */

#define SA_UI_FRAME_MS 33u                   /* ~30 frames a second while anything moves */
#define SA_NBARS 14u
#define SA_CHIPS_MAX 8u                      /* note chips at most (as many as fit the width: sa_ui_chips) */

static struct {
    uint8_t scr;
    uint32_t now, t;                         /* ms; since the overlay came */
    const uint16_t *c;                       /* the theme's colours (SA_TK_*) */
    /* HOME: Notes */
    char ch_txt[8], name[12];
    sa_chord_t chord;
    uint8_t ghost;                           /* nothing held: the last chord, dim */
    int32_t chord_n;                         /* its scale */
    uint8_t nn, more, notes[SA_CHIPS_MAX];   /* the chips: the lowest notes held, and how many more */
    char nm[SA_CHIPS_MAX][6];
    int32_t cw[SA_CHIPS_MAX + 1u], cx0;      /* their widths ("+N" last), the first's x */
    uint8_t eq[SA_NBARS];
    /* overlays */
    uint8_t bank, voice, ch;
    int8_t oct;
    /* S2: the CC screens */
    uint8_t ccbank, cc[4], ccv[4], knob;
    /* S3: the looper's */
    uint8_t lstate, lcur, lnext, lcursor, lfull, count_left, mfocus;
    uint8_t lfilled[16];
    uint32_t lpos, llen, lrec_ms;
    uint8_t ropt[4];
    uint8_t nbars;
    struct {
        uint8_t x0, x1, row, hi;
    } bar[32];
    /* the hand-over */
    uint8_t host_t, warn;
    char info[4][32];
} ui;

static struct {                              /* across frames */
    sa_chord_t last;                         /* the chord of the last frame with notes held */
    sa_chord_t peak, ghost;                  /* the chord when the most notes were held since the last silence; the
                                              * one shown dim after it */
    uint32_t n, peak_n;                      /* notes held at the last frame; at the peak */
    uint32_t pop_ms;
    float lvl;
    uint32_t lvl_ms;
} uis;

#define UC(t) (ui.c[SA_TK_##t])

/* ------------------------------------------------------------------ the snapshot --- */
static int sa_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void sa_ui_chord(void)
{
    uint8_t held[24];
    uint32_t n = 0, i;
    sa_chord_t c;
    for (i = 0; i < 128u && n < sizeof held; i++)
        if (sa.held[i])
            held[n++] = (uint8_t)i;
    sa_chord_name(held, n, &c);
    if (n) {
        if (n >= uis.n && (!sa_streq(c.main, uis.last.main) || !sa_streq(c.ext, uis.last.ext) ||
                           !sa_streq(c.bass, uis.last.bass)))
            uis.pop_ms = ui.now;             /* a new chord pops (not one left by lifted fingers) */
        uis.last = c;
        if (n >= uis.peak_n) {
            uis.peak = c;
            uis.peak_n = n;
        }
        ui.chord = c;
    } else {
        if (uis.n)
            uis.ghost = uis.peak;            /* all lifted: the chord as it was at its fullest stays, dim */
        uis.peak_n = 0;
        ui.chord = uis.ghost;
    }
    uis.n = n;
    ui.ghost = n == 0u;
    if (!ui.chord.main[0]) {
        sa_cpy(ui.chord.main, "--");         /* (nothing played since the start) */
        ui.chord.ext[0] = ui.chord.bass[0] = 0;
    }
    {
        int32_t w;
        ui.chord_n = 4;
        while (ui.chord_n > 2 && (w = sg_text_w(ui.chord.main, ui.chord_n) + sg_text_w(ui.chord.ext, ui.chord_n) +
                                      sg_text_w(ui.chord.bass, ui.chord_n)) > 228)
            ui.chord_n--;
    }
    /* the chips, lowest first, 6 apart, as many as fit 236 pixels; the rest a "+N" chip */
    for (ui.nn = (uint8_t)(n < SA_CHIPS_MAX ? n : SA_CHIPS_MAX);; ui.nn--) {
        int32_t tot = 0;
        ui.more = (uint8_t)(n - ui.nn);
        for (i = 0; i < ui.nn; i++) {
            ui.notes[i] = held[i];
            sa_note_name(held[i], ui.nm[i]);
            ui.cw[i] = sg_text_w(ui.nm[i], 1) + 12;
            tot += ui.cw[i] + (i ? 6 : 0);
        }
        if (ui.more) {
            ui.cw[ui.nn] = sg_text_w("+9", 1) + 12;
            tot += ui.cw[ui.nn] + 6;
        }
        ui.cx0 = (SG_W - tot) / 2;
        if (tot <= 236 || ui.nn <= 1u)
            break;
    }
}

static void sa_ui_bars(void)
{
    uint32_t i, pk = sa_peak_q15, dt = ui.now - uis.lvl_ms;
    float p;
    sa_peak_q15 = 0;
    uis.lvl_ms = ui.now;
    p = (float)pk * (1.0f / 32767.0f) * 1.6f;
    p = p > 1.0f ? 1.0f : p;
    uis.lvl -= uis.lvl * (dt > 200u ? 1.0f : (float)dt * 0.005f);   /* falls back over ~200 ms */
    if (p > uis.lvl)
        uis.lvl = p;
    for (i = 0; i < SA_NBARS; i++) {
        uint32_t period = 800u + ((i * 13u) % 6u) * 100u, ph = ((i * 37u) % 10u) * 100u;
        uint32_t q = ((ui.now + ph) % period) * 5u / period;   /* steps(5) of the bar's wave */
        float tri = q <= 2u ? (float)q * 0.5f : (float)(5u - q) * 0.5f, v = uis.lvl * (0.3f + 0.7f * tri);
        uint32_t s = (uint32_t)(v * 5.0f);
        ui.eq[i] = (uint8_t)(8u + 9u * (s > 4u ? 4u : s));
    }
}

static void sa_ui_snap_more(void);
static void sa_ui_snap(void)
{
    ui.now = plat_ms();
    ui.c = SA_THEMES[sa.theme < SA_NTHEMES ? sa.theme : 0u].c;
    ui.scr = sa.panel ? SA_SCR_HOST : sa.ov;
    ui.t = ui.now - sa.ov_t0;
    ui.ch_txt[0] = 'C', ui.ch_txt[1] = 'H', ui.ch_txt[2] = ' ';
    ui.ch_txt[3] = (char)('0' + sa.ch / 10u), ui.ch_txt[4] = (char)('0' + sa.ch % 10u), ui.ch_txt[5] = 0;
    sa_cpy(ui.name, sa.name);
    sa_ui_chord();
    sa_ui_bars();
    ui.bank = sa.bank;
    ui.voice = sa.voice;
    ui.ch = sa.ch;
    ui.oct = sa.oct;
    ui.ccbank = sa.ccbank;
    memcpy(ui.cc, sa.cc[sa.ccbank], 4);
    memcpy(ui.ccv, sa.ccv[sa.ccbank], 4);
    ui.knob = sa.knob;
    sa_ui_snap_more();
    ui.host_t = sa.host_t;
    if (sa.panel)
        rf_info(ui.info, &ui.warn);
}

/* ------------------------------------------------------------------ the sprites (the mockup's pixel maps) --- */
static const char *const SA_CART[] = {"aaaaaaaaaaaa", "abbbbbbbbbba", "abccccccccba", "abcddddddcba", "abccccccccba",
                                      "abbbbbbbbbba", "abbbbbbbbbba", "abbbbbbbbbba", "aaaaaaaaaaaa", "f.f.f.f.f.f."};
static int32_t sa_cart_col(char ch)
{
    return ch == 'a' ? UC(MID) : ch == 'b' ? UC(SOFT) : ch == 'c' ? UC(A1) : ch == 'd' ? UC(BG) : UC(A3);
}
static const char *const SA_DIN[] = {"....aaaaaa....", "..aa......aa..", ".a..........a.", ".a....cc....a.",
                                     "a..cc....cc..a", "a............a", "a.cc......cc.a", "a............a",
                                     "a............a", ".a..........a.", ".a....aa....a.", "..aa..aa..aa..",
                                     "....aaaaaa...."};
static uint8_t sa_din_pins;
static int32_t sa_din_col(char ch) { return ch == 'a' ? UC(A2) : sa_din_pins ? UC(INK) : -1; }
static const char *const SA_FUZZ[] = {"..a..aa..a..", ".aaa.aa.aaa.", ".aaaaaaaaaa.", "aaaaaaaaaaaa",
                                      "aawwwaawwwaa", "aawkwaawkwaa", "aawwwaawwwaa", "aaaaaaaaaaaa",
                                      "aaammmmmmaaa", "aaaawmmwaaaa", ".aaaaaaaaaa.", ".bb......bb."};
static const char *const SA_LID[] = {"............", "............", "............", "............",
                                     "..lll..lll..", "..LLL..LLL..", "..lll..lll.."};
static int32_t sa_fuzz_col(char ch)
{
    return ch == 'a' || ch == 'l' ? UC(A5) : ch == 'b' ? UC(D5) : ch == 'w' ? UC(INK) : ch == 'm' ? UC(SHADOW) :
                                                                                                   UC(BG);
}

/* ------------------------------------------------------------------ the screens --- */
static void sa_paint_home(void)
{
    int32_t w, x, n = ui.chord_n, y, d, i;
    sg_chip_text(10, 8, 0, 22, 7, ui.ch_txt, UC(A2), UC(ON));
    w = sg_text_w(ui.name, 1) + 14;
    sg_chip_text(230 - w, 8, w, 22, 7, ui.name, UC(PANEL), UC(INK));
    /* the chord: its baseline at 98 (the mockup's 68 px line at 46), the pop one scale up for 100 ms */
    if (!ui.ghost && ui.now - uis.pop_ms < 100u)
        n++;
    d = n >= 4 ? 4 : n;
    y = 98 - 10 * n;
    if (sg_hits(y - 2 * n, 14 * n + d)) {
        w = sg_text_w(ui.chord.main, n) + sg_text_w(ui.chord.ext, n) + sg_text_w(ui.chord.bass, n);
        x = (SG_W - w) / 2;
        if (!ui.ghost) {
            int32_t xs = sg_text(x + d, y + d, n, ui.chord.main, UC(SHADOW));
            xs = sg_text(xs, y + d, n, ui.chord.ext, UC(SHADOW));
            sg_text(xs, y + d, n, ui.chord.bass, UC(SHADOW));
        }
        x = sg_text(x, y, n, ui.chord.main, ui.ghost ? UC(MID) : UC(INK));
        x = sg_text(x, y, n, ui.chord.ext, ui.ghost ? UC(MID) : UC(A1));
        sg_text(x, y, n, ui.chord.bass, ui.ghost ? UC(MID) : UC(INK));
    }
    /* the note chips (the extensions in the accent), centred, 6 apart */
    if (!ui.ghost && sg_hits(131, 22)) {
        x = ui.cx0;
        for (i = 0; i < ui.nn; i++) {
            int ext = !(ui.chord.core >> (ui.notes[i] % 12u) & 1u);
            sg_chip_text(x, 131, ui.cw[i], 22, 6, ui.nm[i], ext ? UC(A1) : UC(PANEL), ext ? UC(ON) : UC(INK));
            x += ui.cw[i] + 6;
        }
        if (ui.more) {
            char m[4] = {'+', (char)('0' + (ui.more > 9u ? 9u : ui.more)), 0, 0};
            sg_chip_text(x, 131, ui.cw[ui.nn], 22, 6, m, UC(PANEL), UC(INK));
        }
    }
    /* the bars: 14 from x 10 to 230, 12 wide, 4 apart, standing on y 230 */
    for (i = 0; i < (int32_t)SA_NBARS; i++)
        sg_rect(10 + 16 * i, 230 - ui.eq[i], 12, ui.eq[i], (i & 1) ? UC(A3) : UC(A1));
}

static void sa_num(char *s, int32_t v, int sign)   /* "11", "+1", "0", "-2" */
{
    char t[4];
    int32_t k = 0;
    if (v < 0) {
        *s++ = '-';
        v = -v;
    } else if (sign && v > 0) {
        *s++ = '+';
    }
    do
        t[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < 3);
    while (k)
        *s++ = t[--k];
    *s = 0;
}

static void sa_paint_preset(void)
{
    char num[4];
    int32_t i, dy = ui.t < 400u ? -8 + 2 * (int32_t)(ui.t / 100u) : 0;   /* the cartridge slides in, steps(4) */
    const char *bank = sa_bank_name(ui.bank);
    char name[12];
    sg_sprite(10, 7 + dy, 3, SA_CART, 10, sa_cart_col);
    {
        int32_t w = sg_text_w(bank, 1) + 14;
        sg_chip_text(230 - w, 10, w, 22, 7, bank, UC(A1), UC(ON));
    }
    sa_num(num, ui.voice + 1, 0);
    sg_text_sh((SG_W - sg_text_w(num, 5)) / 2, 66, 5, num, UC(A3), 4, UC(D1));
    sa_voice_name(sa_bank_vmem(ui.bank, ui.voice), name);
    sg_text_c(155, 1, name, UC(INK));
    for (i = 0; i < 32; i++) {               /* 16 x 2 slots: the voice, those before it, those after */
        uint16_t c = i == ui.voice ? UC(A3) : i < ui.voice ? UC(MID) : UC(PANEL);
        sg_rect(17 + (i % 16) * 13, 204 + (i / 16) * 13, 11, 11, c);
    }
}

static void sa_paint_channel(void)
{
    char num[4];
    int32_t i;
    sa_din_pins = (uint8_t)(ui.now % 1200u < 720u);   /* the pins blink (60 % of 1.2 s) */
    sg_sprite(99, 8, 3, SA_DIN, 13, sa_din_col);
    sa_num(num, ui.ch, 0);
    sg_text_sh((SG_W - sg_text_w(num, 5)) / 2, 74, 5, num, UC(A2), 4, UC(D2));
    for (i = 0; i < 16; i++) {               /* 8 x 2 cells: an outline, filled for the channel */
        int32_t x = 18 + (i % 8) * 26, y = 182 + (i / 8) * 24;
        int on = i + 1 == ui.ch;
        sg_chip(x, y, 22, 20, on ? UC(A2) : UC(PANEL));
        sg_chip(x + 2, y + 2, 18, 16, on ? UC(A2) : UC(BG));
    }
}

static void sa_paint_octave(void)
{
    char num[4], lo[6], hi[6], rng[14];
    int32_t i, cx, top, y;
    uint32_t hop = ui.now % 1200u, blink = ui.now % 3200u;
    sg_chip_text(95, 8, 50, 22, 0, "OCT", UC(A5), UC(ON));
    sa_num(num, ui.oct, 1);
    sg_text_sh((SG_W - sg_text_w(num, 5)) / 2, 54, 5, num, UC(A5), 4, UC(D5));
    for (i = 0; i < 7; i++) {                /* the seven steps, the octave's lit, 0's paler */
        int32_t o = i - 3, h = 8 + 8 * i;
        sg_rect(17 + 30 * i, 206 - h, 26, h, o == ui.oct ? UC(A5) : o == 0 ? UC(SOFT) : UC(PANEL));
    }
    /* the furball stands on the octave's step; it hops (71..85 % of 1.2 s) and blinks (91..96 % of 3.2 s) */
    cx = 17 + 30 * (ui.oct + 3) + 13;
    top = 206 - (8 + 8 * (ui.oct + 3)) - 36 - (hop >= 852u && hop < 1020u ? 8 : 0);
    sg_sprite(cx - 18, top, 3, SA_FUZZ, 12, sa_fuzz_col);
    if (blink >= 2912u && blink < 3072u)
        sg_sprite(cx - 18, top, 3, SA_LID, 7, sa_fuzz_col);
    sa_note_name((uint32_t)clamp((int32_t)SA_NOTE0 + 12 * ui.oct, 0, 127), lo);
    sa_note_name((uint32_t)clamp((int32_t)SA_NOTE0 + 26 + 12 * ui.oct, 0, 127), hi);
    sa_cpy(sa_cpy(sa_cpy(rng, lo), "-"), hi);
    y = 210;
    sg_chip_text((SG_W - 76) / 2, y, 76, 22, 0, rng, UC(PANEL), UC(INK));
}

static void sa_paint_host(void)
{
    static const char *const TR[3] = {"USB", "USB BULK", "BLUETOOTH"};
    const char *tr = TR[ui.host_t < 3u ? ui.host_t : 0u];
    int32_t i, w;
    sg_chip_text((SG_W - 100) / 2, 8, 100, 22, 0, "PANEL MODE", UC(A4), UC(ON));
    sg_text_sh((SG_W - sg_text_w("HOST", 5)) / 2, 52, 5, "HOST", UC(A2), 4, UC(D2));
    w = sg_text_w(tr, 1) + 14;
    sg_chip_text((SG_W - w) / 2, 120, w, 22, 7, tr, UC(PANEL), UC(INK));
    for (i = 0; i < 4; i++)
        sg_text_c(160 + 18 * i, 1, ui.info[i], (ui.warn >> i & 1u) ? UC(A1) : UC(SOFT));
}

#include "sa_screens.c"

static void sa_paint(void)
{
    if (!sa_paint_more())
        switch (ui.scr) {
    case SA_SCR_PRESET: sa_paint_preset(); break;
    case SA_SCR_CHANNEL: sa_paint_channel(); break;
    case SA_SCR_OCTAVE: sa_paint_octave(); break;
    case SA_SCR_HOST: sa_paint_host(); break;
    default: sa_paint_home(); break;
    }
    if (UC(BORDER) != UC(BG)) {              /* a theme's border (C64, SNES, Game Boy): 4 pixels inside the edge */
        sg_rect(0, 0, SG_W, 4, UC(BORDER));
        sg_rect(0, SG_H - 4, SG_W, 4, UC(BORDER));
        sg_rect(0, 0, 4, SG_H, UC(BORDER));
        sg_rect(SG_W - 4, 0, 4, SG_H, UC(BORDER));
    }
}

/* ------------------------------------------------------------------ the frames --- */
static void sa_ui_lost(void)                 /* somebody else drew on the panel (fm1-base's screens, the host) */
{
    sg_abort();
    sg_invalidate();
    sa.poke = 1;
}

/* the main loop (app_frame, app_input_only): a frame every SA_UI_FRAME_MS, or soon after a change, painted a few
 * bands a call, while the panel is ours */
static void sa_frame(void)
{
    uint32_t now = plat_ms();
    if (!rf_panel_ours()) {
        sa.lost = 1;
        sg_abort();
        return;
    }
    if (sa.lost) {
        sa.lost = 0;
        sa_ui_lost();
    }
    if (!sg_busy()) {
        uint32_t since = now - sa.frame_ms;
        if (since < (sa.poke ? 16u : SA_UI_FRAME_MS))
            return;
        sa.poke = 0;
        sa.frame_ms = now;
        sa_ui_snap();
        sg_start();
    }
    sg_step(sa_paint, 4u, ui.c[SA_TK_BG]);
}
