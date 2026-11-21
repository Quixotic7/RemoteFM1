/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 Wi-Fi: the Wi-Fi page's screens and the settings' WI-FI row (sa_wifi.c), part of sa_screens.c in the
 * Wi-Fi build only. Drawn as design/wifi-settings/make_wifi_mockups.py draws them (its numbers, its sprites), with
 * sa_gfx.c's calls and the Jersey 10 font only. The lists are read from fm1-base's (plat_wifi_found, plat_wifi_saved)
 * for each band; the page's own state is the frame's snapshot (ui.wp). */

/* ------------------------------------------------------------------ sprites --- */
static const char *const WP_FAN[] = {"....aaaaa....", ".aaaaaaaaaaa.", "aaa.......aaa", "a...bbbbb...a",
                                     "..bbbbbbbbb..", "..bb.....bb..", ".....ccc.....", "....ccccc....",
                                     ".............", "......d......", ".....ddd....."};
static const char *const WP_LOCK[] = {".aaa.", "a...a", "a...a", "aaaaa", "aa.aa", "aa.aa", "aaaaa"};
static const char *const WP_STAR[] = {"...a...", "..aaa..", "aaaaaaa", ".aaaaa.", "..aaa..", ".aa.aa.", ".a...a."};
static const char *const WP_CHECK[] = {"......aa", ".....aa.", "aa..aa..", ".aaaa...", "..aa...."};
static const char *const WP_CROSS[] = {"aa...aa", ".aa.aa.", "..aaa..", ".aa.aa.", "aa...aa"};
static const char *const WP_EYE[] = {"..aaaaa..", "aa.....aa", "...aaa...", "..aaaaa..", "...aaa...", "aa.....aa",
                                     "..aaaaa.."};
static const char *const WP_EYE_SHUT[] = {".........", ".........", ".........", "aa.....aa", "..aaaaa..",
                                          ".a..a..a.", "a...a...a"};
static const char *const WP_KEY[] = {".aaa.......", "a...a......", "a...aaaaaaa", "a...a...a.a", ".aaa....a.a"};
static const char *const WP_TRASH[] = {"...aaa...", "aaaaaaaaa", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.",
                                       ".aaaaaaa."};
static const char *const WP_TRASH_OPEN[] = {".aaaaa...", "....aaaa.", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.",
                                            ".a.a.a.a.", ".aaaaaaa."};
static const char *const WP_JOINS[] = {"aaa......", "a.....a..", "a.....aa.", "a..aaaaaa", "a.....aa.", "a.....a..",
                                      "aaa......"};
static const char *const WP_NOTCH[] = {"a.", "aa", "aa", "a."};
static const char *const WP_LT[] = {"..a", ".aa", "aaa", ".aa", "..a"};
static const char *const WP_UP[] = {"..a..", ".aaa.", "aaaaa"};
#define WP_N(rows) ((uint32_t)(sizeof rows / sizeof rows[0]))

static void wp_spr(int32_t x, int32_t y, int32_t s, const char *const *rows, uint32_t n, uint16_t c)
{
    sa_arrow_c = c;
    sg_sprite(x, y, s, rows, n, sa_arrow_col);
}

static uint8_t wp_fan_lv;
static uint16_t wp_fan_on, wp_fan_off;
static int32_t wp_fan_col(char ch)
{
    uint32_t need = ch == 'd' ? 1u : ch == 'c' ? 2u : ch == 'b' ? 3u : 4u;
    return wp_fan_lv >= need ? wp_fan_on : wp_fan_off;
}
static void wp_fan(int32_t x, int32_t y, int32_t s, uint32_t lv, uint16_t on, uint16_t off)
{
    wp_fan_lv = (uint8_t)lv;
    wp_fan_on = on;
    wp_fan_off = off;
    sg_sprite(x, y, s, WP_FAN, WP_N(WP_FAN), wp_fan_col);
}

/* ------------------------------------------------------------------ helpers --- */
static uint32_t wp_slen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static uint32_t wp_level(int rssi)           /* dBm: 0 (not heard) .. 4 */
{
    return rssi == 0 ? 0u : rssi >= -55 ? 4u : rssi >= -65 ? 3u : rssi >= -75 ? 2u : 1u;
}
static uint16_t wp_lvl_col(uint32_t lv) { return lv >= 3u ? UC(A4) : lv == 2u ? UC(A3) : UC(A1); }

static void wp_bars(int32_t x, int32_t ybot, uint32_t lv, uint16_t dim)   /* four bars rising, lit to lv */
{
    int32_t i;
    for (i = 0; i < 4; i++) {
        int32_t h = 4 + 3 * i;
        sg_rect(x + i * 4, ybot - h, 3, h, (uint32_t)i < lv ? wp_lvl_col(lv) : dim);
    }
}

static int32_t wp_fit(const char *s, int32_t n, int32_t w)   /* the largest scale <= n at which s fits w */
{
    while (n > 1 && sg_text_w(s, n) > w)
        n--;
    return n;
}

static void wp_clip(char *d, const char *s, int32_t n, int32_t w)   /* s's start that fits w at scale n */
{
    uint32_t i;
    sa_cpy(d, s);
    for (i = (uint32_t)wp_slen(d); i && sg_text_w(d, n) > w; i--)
        d[i - 1u] = 0;
}

static void wp_head(uint16_t c) { sg_chip_text(10, 8, 0, 22, 7, "WI-FI", c, UC(ON)); }

static void wp_chip_r(const char *s)         /* a value chip on the right (the network's name) */
{
    char t[33];
    int32_t w;
    wp_clip(t, s, 1, 140);
    w = sg_text_w(t, 1) + 14;
    sg_chip_text(230 - w, 8, w, 22, 7, t, UC(PANEL), UC(INK));
}

static void wp_tile(int32_t x, int32_t y, int32_t w, int32_t h, const char *const *rows, uint32_t n, int32_t s,
                    uint16_t col, int picked)
{
    uint16_t fg = col;
    int32_t iw = (int32_t)wp_slen(rows[0]) * s, ih = (int32_t)n * s;
    if (picked) {
        sg_chip(x, y, w, h, col);
        fg = UC(ON);
    } else {
        sg_chip(x, y, w, h, UC(PANEL));
        sg_chip(x + 2, y + 2, w - 4, h - 4, UC(BG));
    }
    wp_spr(x + (w - iw) / 2, y + (h - ih) / 2, s, rows, n, fg);
}

/* ------------------------------------------------------------------ the snapshot --- */
static void sa_ui_snap_wifi(void)
{
    ui.wp = wp;
    ui.wp_st = (uint8_t)plat_wifi_state();
    ui.wp_glo = (uint8_t)(sa.btns >> B_GLO & 1u);
    ui.wp_scanning = (uint8_t)wp_scanning();
    ui.set[SA_SET_WIFI] = (uint8_t)(ui.wp_st != WIFI_OFF);
}

/* ------------------------------------------------------------------ the settings' WI-FI row --- */
/* its value (OFF, LOOKING, the network joined, NOT FOUND / COULD NOT JOIN), the stripes (OFF / ON; a light running
 * along four while it looks; the signal rising once joined), and OCT+'s arrow into the page, nudging */
static void sa_paint_set_wifi(uint16_t c)
{
    int st = ui.wp_st;
    const char *v = "LOOKING", *why;
    uint16_t vc = c;
    int32_t n, i;
    if (st == WIFI_OFF) {
        v = "OFF";
        vc = UC(A1);
    } else if (st == WIFI_CONNECTED) {
        v = plat_wifi_ssid();
    } else if (st == WIFI_FAILED) {
        why = plat_wifi_why();
        v = why && sa_streq(why, "not found") ? "NOT FOUND" : "COULD NOT JOIN";
        vc = UC(A1);
    }
    n = wp_fit(v, 5, 180);
    sg_text_sh((SG_W - sg_text_w(v, n)) / 2, 100 + (5 - n) * 5, n, v, vc, n > 1 ? 4 : 2, UC(SHADOW));
    if (st == WIFI_OFF) {
        for (i = 0; i < 2; i++)
            sg_rect(20 + i * 101, 164, 98, 8, i == 0 ? UC(A1) : UC(D5));
    } else if (st == WIFI_CONNECTED) {
        uint32_t lv = wp_level(plat_wifi_rssi());
        for (i = 0; i < 4; i++)
            sg_rect(20 + i * 51, 172 - (2 + 2 * i), 47, 2 + 2 * i, (uint32_t)i < lv ? wp_lvl_col(lv) : UC(D5));
    } else {
        uint32_t run = st == WIFI_FAILED ? 9u : ui.now / 150u % 4u;
        for (i = 0; i < 4; i++)
            sg_rect(20 + i * 51, 164, 47, 8, (uint32_t)i == run ? c : UC(D5));
    }
    wp_spr(208 + (ui.now % 1000u < 150u ? 3 : 0), 100, 3, SA_ARROW, 9, UC(A1));
}

/* ------------------------------------------------------------------ LIST --- */
static void wp_tabs(uint32_t which, uint32_t n_around, uint32_t n_kept)
{
    char t[8];
    int on = which == 0u;
    int32_t x = 116;
    sg_chip(x, 8, 56, 22, on ? UC(A2) : UC(PANEL));
    wp_fan(x + 8, 14, 1, ui.wp_scanning ? ui.now / 120u % 4u + 1u : 4u, on ? UC(ON) : UC(A2), on ? UC(D2) : UC(MID));
    sa_txt_num(t, n_around);
    sg_text(x + 28, 14, 1, t, on ? UC(ON) : UC(SOFT));
    on = which == 1u;
    sg_chip(x + 60, 8, 54, 22, on ? UC(A3) : UC(PANEL));
    wp_spr(x + 68, 15, 1, WP_STAR, WP_N(WP_STAR), on ? UC(ON) : UC(A3));
    sa_txt_num(t, n_kept);
    sa_cpy(t + wp_slen(t), "/6");
    sg_text(x + 82, 14, 1, t, on ? UC(ON) : UC(SOFT));
}

/* a network: its bars, its name (scale 2; a long one cut, scrolling while picked), a star if kept, a lock if secured */
static void wp_row(int32_t y, const char *ssid, int rssi, int sec, int kept, int joined, int picked)
{
    char name[40];
    int32_t x, maxw = 146, cut = sg_text_w(ssid, 2) > maxw, m = 0;
    if (!sg_hits(y - 6, 30))
        return;
    if (picked) {
        sg_chip(6, y - 6, 216, 30, UC(PANEL));
        wp_spr(9, y + 1, 2, WP_NOTCH, WP_N(WP_NOTCH), UC(A2));
    }
    wp_bars(16, y + 16, wp_level(rssi), picked ? UC(MID) : UC(PANEL));
    if (cut && picked) {                     /* scrolling: 250 ms a character, a pause at each end */
        int32_t len = (int32_t)wp_slen(ssid), most = 0, step = (int32_t)((ui.now - ui.wp.t0) / 250u);
        while (most < len && sg_text_w(ssid + most, 2) > maxw)
            most++;
        m = step % (most + 8) - 4;
        m = m < 0 ? 0 : m > most ? most : m;
    }
    if (cut) {
        int32_t i;
        sa_cpy(name, ssid + m);
        for (i = (int32_t)wp_slen(name); i && sg_text_w(name, 2) + sg_text_w("..", 2) > maxw; i--)
            name[i - 1] = 0;
        cut = name[0] && wp_slen(name) < wp_slen(ssid + m);
    } else {
        sa_cpy(name, ssid);
    }
    x = sg_text(38, y, 2, name, joined ? UC(A4) : picked ? UC(INK) : UC(SOFT));
    if (cut)
        sg_text(x, y, 2, "..", UC(MID));
    if (kept)
        wp_spr(186, y + 3, 2, WP_STAR, WP_N(WP_STAR), UC(A3));
    if (sec)
        wp_spr(204, y + 2, 2, WP_LOCK, WP_N(WP_LOCK), picked ? UC(INK) : UC(MID));
}

static void wp_dots(uint32_t n, uint32_t cur, int32_t y0)   /* the place in the list (up to 12) */
{
    uint32_t i;
    for (i = 0; i < n && i < 12u; i++)
        sg_rect(228, y0 + 10 * (int32_t)i, 6, 6, i == cur ? UC(A2) : UC(PANEL));
}

static void wp_paint_around(void)
{
    char s[33], ip[16];
    uint32_t n = wp_count(), nf = wp_nfound(), r;
    int rssi = 0, sec = 0, hero = wp_joined(0), st = ui.wp_st, e;
    wp_head(UC(A2));
    wp_tabs(0, nf, wp_nkept());
    if (!ui.wp_scanning && !nf && !hero && st != WIFI_OFF && st != WIFI_STARTING) {
        /* nothing heard: the fan dark, a cross, the radio's one band; SELECT up looks again */
        wp_fan(81, 50, 6, 0, UC(A1), UC(PANEL));
        wp_spr(150, 108, 4, WP_CROSS, WP_N(WP_CROSS), UC(A1));
        {
            int32_t w = sg_text_w("2.4 GHz", 2) + 24;
            sg_chip((SG_W - w) / 2, 132, w, 34, UC(A3));
            sg_text((SG_W - sg_text_w("2.4 GHz", 2)) / 2, 139, 2, "2.4 GHz", UC(ON));
        }
        wp_spr(112, 190 - (ui.now % 1000u < 150u ? 3 : 0), 3, WP_UP, WP_N(WP_UP), UC(A2));
        return;
    }
    if (ui.wp_scanning || st == WIFI_OFF || st == WIFI_STARTING) {
        /* a scan: the fan's arcs light outwards in turn, three bars sweep, the networks heard so far, and a place for
         * the next one breathing */
        static const int32_t SW[3] = {150, 110, 70};
        uint32_t ph = ui.now / 100u % 3u, k = 0;
        int32_t i, pf;
        char ps[33];
        pf = ui.wp.pick < n ? wp_entry(ui.wp.pick, ps, &rssi, &sec) : -1;
        wp_fan(16, 44, 3, ui.now / 150u % 4u + 1u, UC(A2), UC(D5));
        for (i = 0; i < 3; i++)
            sg_rect(62, 50 + 14 * i, SW[(i + ph) % 3u], 6, i == 0 ? UC(A2) : i == 1 ? UC(D2) : UC(D5));
        for (r = 0; r < WP_ROWS && plat_wifi_found((int)r, s, &rssi, &sec); r++, k++) {
            const char *j = plat_wifi_ssid();
            int jn = st == WIFI_CONNECTED && j && sa_streq(j, s);
            wp_row(108 + 32 * (int32_t)r, s, rssi, sec, plat_wifi_kept(s), jn, pf == (int)r && !jn);
        }
        if (k < WP_ROWS && ui.now % 1200u < 700u) {
            int32_t y = 108 + 32 * (int32_t)k;
            sg_rect(16, y + 4, 15, 12, UC(D5));
            sg_rect(38, y + 2, 120, 16, UC(D5));
        }
        return;
    }
    if (hero) {                              /* the network joined: the fan lit to its signal, the name, the address */
        int picked = ui.wp.pick == 0u;
        wp_joined(s);
        if (picked) {
            sg_chip(6, 36, 216, 62, UC(PANEL));
            sg_chip(8, 38, 212, 58, UC(BG));
        }
        wp_fan(16, 44, 3, wp_level(plat_wifi_rssi()), UC(A4), UC(D5));
        e = wp_fit(s, 3, 160);
        sg_text_sh(60, 44 + (3 - e) * 5, e, s, UC(INK), 2, UC(SHADOW));
        rf_put_ip(ip, plat_wifi_ip());
        sg_text(60, 76, sg_text_w(ip, 2) <= 158 ? 2 : 1, ip, UC(A4));
    } else {                                 /* not joined: the fan (lit while it joins), what it does */
        const char *j = plat_wifi_ssid();
        int busy = st == WIFI_JOINING || st == WIFI_ADDRESS;
        const char *v = busy && j && j[0] ? j : "NOT JOINED";
        wp_fan(16, 44, 3, busy ? ui.now / 150u % 4u + 1u : 0u, UC(A2), UC(D5));
        e = wp_fit(v, 3, 160);
        sg_text(60, 44 + (3 - e) * 5, e, v, busy ? UC(SOFT) : UC(MID));
    }
    {                                        /* the others: four rows, the pick in view */
        uint32_t off = hero ? 1u : 0u, top = ui.wp.top;
        if (ui.wp.pick >= off) {
            uint32_t pr = ui.wp.pick - off;
            if (pr < top)
                top = pr;
            if (pr >= top + WP_ROWS)
                top = pr - WP_ROWS + 1u;
        }
        wp.top = (uint8_t)top;               /* (kept for the next frame) */
        for (r = 0; r < WP_ROWS; r++) {
            e = wp_entry(off + top + r, s, &rssi, &sec);
            if (e == -1)
                break;
            wp_row(108 + 32 * (int32_t)r, s, rssi, sec, plat_wifi_kept(s), 0, ui.wp.pick == off + top + r);
        }
    }
    wp_dots(n, ui.wp.pick, 104);
}

static void wp_paint_kept(void)              /* the networks kept: joined (a check), heard (a star), not heard (dim);
                                              * their six places along the bottom */
{
    char s[33], j[33];
    uint32_t nk = wp_nkept(), r, top = ui.wp.top;
    int hero = wp_joined(j);
    wp_head(UC(A2));
    wp_tabs(1, wp_nfound(), nk);
    if (ui.wp.pick < top)
        top = ui.wp.pick;
    if (ui.wp.pick >= top + WP_ROWS)
        top = ui.wp.pick - WP_ROWS + 1u;
    wp.top = (uint8_t)top;
    for (r = 0; r < WP_ROWS && top + r < nk && plat_wifi_saved((int)(top + r), s); r++) {
        int32_t y = 48 + 34 * (int32_t)r, n;
        int rssi = 0, heard = wp_heard(s, &rssi, 0, 0) >= 0, jn = hero && sa_streq(s, j);
        if (!sg_hits(y - 7, 32))
            continue;
        if (ui.wp.pick == top + r) {
            sg_chip(6, y - 7, 216, 32, UC(PANEL));
            wp_spr(9, y + 1, 2, WP_NOTCH, WP_N(WP_NOTCH), UC(A3));
        }
        if (jn)
            wp_spr(16, y + 4, 2, WP_CHECK, WP_N(WP_CHECK), UC(A4));
        else
            wp_spr(18, y + 2, 2, WP_STAR, WP_N(WP_STAR), heard ? UC(A3) : UC(MID));
        n = wp_fit(s, 2, 150);
        sg_text(40, y + (2 - n) * 5, n, s, jn ? UC(A4) : heard ? UC(INK) : UC(MID));
        wp_bars(196, y + 16, heard ? wp_level(rssi) : 0u, heard ? UC(D5) : UC(PANEL));
    }
    for (r = 0; r < 6u; r++) {
        sg_chip(24 + 33 * (int32_t)r, 190, 27, 30, r < nk ? UC(A3) : UC(PANEL));
        if (r < nk)
            wp_spr(24 + 33 * (int32_t)r + 7, 198, 2, WP_STAR, WP_N(WP_STAR), UC(ON));
    }
}

/* ------------------------------------------------------------------ NET: a network kept picked --- */
static void wp_paint_net(void)
{
    char t[12];
    int rssi = 0, sec = 1, ch = 0, heard = wp_heard(wp_net, &rssi, &sec, &ch) >= 0, n;
    uint32_t armed = ui.wp.armed;
    wp_head(UC(A2));
    if (heard && ch) {
        sa_cpy(t, "CH ");
        sa_txt_num(t + 3, (uint32_t)ch);
        wp_chip_r(t);
    }
    if (!heard && ui.wp_st == WIFI_CONNECTED && sa_streq(plat_wifi_ssid(), wp_net))
        rssi = plat_wifi_rssi(), heard = 1;
    wp_fan(94, 38, 4, wp_level(heard ? rssi : 0), UC(A3), UC(D5));
    n = wp_fit(wp_net, 4, 200);
    sg_text_sh((SG_W - sg_text_w(wp_net, n)) / 2, 92, n, wp_net, UC(INK), 3, UC(SHADOW));
    if (plat_wifi_kept(wp_net))
        wp_spr(96, 140, 2, WP_STAR, WP_N(WP_STAR), UC(A3));
    if (sec)
        wp_spr(132, 140, 2, WP_LOCK, WP_N(WP_LOCK), UC(MID));
    wp_tile(12, 168, 66, 58, WP_JOINS, WP_N(WP_JOINS), 4, UC(A4), ui.wp.tile == 0u);
    wp_tile(87, 168, 66, 58, WP_KEY, WP_N(WP_KEY), 4, UC(A3), ui.wp.tile == 1u);
    if (armed) {                             /* FORGET armed: red, the lid lifted, shaking */
        int32_t dx = (ui.now - ui.wp.armed_ms) / 100u % 4u == 1u ? 2 : (ui.now - ui.wp.armed_ms) / 100u % 4u == 3u ? -2
                                                                                                             : 0;
        wp_tile(162, 168, 66, 58, WP_TRASH_OPEN, WP_N(WP_TRASH_OPEN), 4, UC(D1), 1);
        sg_chip(160 + dx, 166, 70, 4, UC(A1));
    } else {
        wp_tile(162, 168, 66, 58, WP_TRASH, WP_N(WP_TRASH), 4, UC(A1), ui.wp.tile == 2u);
    }
}

/* ------------------------------------------------------------------ PASS: the password on the keys --- */
static const uint8_t WP_BK[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};   /* the white key each black key follows */

static void wp_paint_pass(void)
{
    const sa_wp_t *p = &ui.wp;
    uint32_t n = p->len, lay = ui.wp_glo ? 1u : p->layer, i, s0, lit = 0;
    uint32_t since = ui.now - p->typed_ms;
    int ok = n >= 8u, cursor = since < 500u || since / 500u % 2u == 0u, litw = -1, litb = -1;
    int32_t x, w, k;
    char c2[2] = {0, 0}, t[4];
    /* the network: a chip with its lock and name; EDIT's eye */
    w = sg_text_w(wp_net, 1) + 34;
    sg_chip(10, 8, w < 176 ? w : 176, 22, UC(A3));
    wp_spr(18, 12, 2, WP_LOCK, WP_N(WP_LOCK), UC(ON));
    for (i = 0, x = 34; wp_net[i] && x + SA_PF_G[sg_gi((uint8_t)wp_net[i])].adv <= 180; i++) {
        c2[0] = wp_net[i];
        x = sg_text(x, 14, 1, c2, UC(ON));
    }
    sg_chip(190, 8, 40, 22, p->shown ? UC(A2) : UC(PANEL));
    wp_spr(199, 12, 2, p->shown ? WP_EYE : WP_EYE_SHUT, 7, p->shown ? UC(ON) : UC(SOFT));
    /* the field: a window on the password with the cursor in it; shown, or blocks (the last character typed shown a
     * second); a dim '<' when it starts further on */
    if (p->shown) {
        for (s0 = 0; s0 < n && sg_text_w(wp_pw + s0, 2) > 200; s0++)
            ;
        if (p->cur < s0)
            s0 = p->cur;
    } else {
        s0 = n > 13u ? n - 13u : 0u;
        if (p->cur < s0)
            s0 = p->cur;
    }
    if (s0)
        wp_spr(4, 44, 2, WP_LT, WP_N(WP_LT), UC(MID));
    x = 14;
    for (i = s0; i <= n; i++) {
        int32_t xi = x;
        if (i < n) {
            c2[0] = wp_pw[i];
            if (p->shown) {
                if (x + sg_text_w(c2, 2) > 214)
                    break;
                x = sg_text(x, 42, 2, c2, UC(INK));
            } else {
                if (i - s0 >= 13u)
                    break;
                if (i == p->last && since < 1000u) {
                    x = sg_text(x, 42, 2, c2, UC(A1)) + 2;
                } else {
                    sg_rect(x + 2, 46, 10, 12, UC(SOFT));
                    x += 15;
                }
            }
        }
        if (i == p->cur && cursor)
            sg_rect(xi + 1, 64, 12, 3, UC(A1));
    }
    /* the length: 63 ticks, the 8th taller (WPA's least), orange under 8, green from 8; the count */
    for (k = 0; k < 63; k++) {
        int32_t h = k == 7 ? 12 : 8;
        sg_rect(12 + 3 * k, 84 - h, 2, h, (uint32_t)k < n ? (ok ? UC(A4) : UC(A1)) : UC(D5));
    }
    sa_txt_num(t, n);
    sg_text(206, 72, n < 10u ? 2 : 1, t, ok ? UC(A4) : UC(A1));
    /* the layers, as FX SEL ENV LFO lie */
    for (i = 0; i < 4u; i++) {
        static const char *const LAB[4] = {"abc", "ABC", "123", "#+="};
        int32_t lx = 12 + 56 * (int32_t)i;
        int on = i == lay;
        sg_chip(lx, 94, 50, 24, on ? UC(A2) : UC(PANEL));
        sg_text(lx + (50 - sg_text_w(LAB[i], 1)) / 2, 101, 1, LAB[i], on ? UC(ON) : UC(SOFT));
    }
    /* the keys as they lie: 11 black over 16 white, a character on each; the key typed glowing 250 ms */
    if (p->lit_k && ui.now - p->lit_ms < 250u) {
        lit = p->lit_k - 1u;
        litw = sa_key_white(lit);
        litb = sa_key_black(lit);
    }
    if (!sg_hits(128, 112))
        return;
    for (k = 0; k < 16; k++) {
        int32_t kx = 1 + 15 * k;
        int on = k == litw;
        sg_rect(kx, 174, 13, 64, on ? UC(A1) : UC(INK));
        c2[0] = WP_LAY[lay][1][k];
        if (c2[0] == ' ')
            sg_rect(kx + 3, 226, 7, 2, UC(BG));
        else
            sg_text(kx + (13 - sg_text_w(c2, 1)) / 2, 220, 1, c2, UC(ON));
    }
    for (k = 0; k < 11; k++) {
        int32_t kx = 1 + 15 * (WP_BK[k] + 1) - 8;
        int on = k == litb;
        sg_rect(kx, 128, 14, 42, on ? UC(A1) : UC(PANEL));
        c2[0] = WP_LAY[lay][0][k];
        sg_text(kx + (14 - sg_text_w(c2, 1)) / 2, 154, 1, c2, on ? UC(ON) : UC(INK));
    }
}

/* ------------------------------------------------------------------ JOIN, DONE, FAIL --- */
static void wp_paint_join(void)
{
    int st = ui.wp_st;
    uint32_t stage = st == WIFI_ADDRESS ? 1u : st == WIFI_CONNECTED ? 2u : 0u, el = ui.now - ui.wp.join_ms, i;
    wp_head(UC(A2));
    wp_chip_r(wp_net);
    wp_fan(81, 44, 6, ui.now / 150u % 5u, UC(A2), UC(D5));   /* the arcs light from the dot out, again and again */
    for (i = 0; i < 3u; i++) {               /* the handshake, an address, done; the one under way breathing */
        int32_t x = 28 + 64 * (int32_t)i;
        uint16_t c = i < stage ? UC(A2) : i == stage ? (ui.now % 1200u < 700u ? UC(D2) : UC(D5)) : UC(D5);
        sg_rect(x, 140, 56, 10, c);
        c = i < stage ? UC(A2) : i == stage ? UC(INK) : UC(PANEL);
        if (i == 1u)
            sg_text(x + (56 - sg_text_w("IP", 2)) / 2, 162, 2, "IP", c);
        else if (i == 0u)
            wp_spr(x + (56 - 11 * 3) / 2, 164, 3, WP_KEY, WP_N(WP_KEY), c);
        else
            wp_spr(x + (56 - 8 * 3) / 2, 164, 3, WP_CHECK, WP_N(WP_CHECK), c);
    }
    for (i = 0; i < 8u; i++)                 /* the time waited, over 15 s */
        sg_rect(18 + 26 * (int32_t)i, 224, 22, 6, i <= el * 8u / 15000u ? UC(A2) : UC(PANEL));
}

static void wp_paint_done(void)
{
    char ip[16];
    uint32_t t = ui.now - ui.wp.t0, i;
    int32_t dy = t < 200u ? -(int32_t)(200u - t) / 8 : t < 300u ? -4 : 0, n;   /* the check drops in, bounces */
    wp_head(UC(A4));
    wp_spr(84, 40 + dy, 9, WP_CHECK, WP_N(WP_CHECK), UC(A4));
    n = wp_fit(wp_net, 3, 220);
    sg_text_sh((SG_W - sg_text_w(wp_net, n)) / 2, 106, n, wp_net, UC(INK), 3, UC(SHADOW));
    rf_put_ip(ip, plat_wifi_ip());
    n = wp_fit(ip, 3, 220);
    sg_text_sh((SG_W - sg_text_w(ip, n)) / 2, 152, n, ip, UC(A4), 3, UC(D2));
    for (i = 0; i < 8u; i++)
        sg_rect(18 + 26 * (int32_t)i, 224, 22, 6, UC(A4));
}

static void wp_paint_fail(void)
{
    static const char *const WHY[4] = {"COULD NOT JOIN", "WRONG PASSWORD", "NOT FOUND", "NO ADDRESS"};
    const char *why = WHY[ui.wp.why & 3u];
    uint32_t t = ui.now - ui.wp.t0;
    int32_t dx = t < 300u ? (t / 100u % 2u ? 4 : -4) : 0, n;   /* the cross shakes once */
    wp_head(UC(A1));
    wp_chip_r(wp_net);
    wp_fan(81, 38, 6, 1, UC(A1), UC(PANEL));
    wp_spr(144 + dx, 92, 4, WP_CROSS, WP_N(WP_CROSS), UC(A1));
    n = wp_fit(why, 3, 220);
    sg_text_sh((SG_W - sg_text_w(why, n)) / 2, 132 + (3 - n) * 5, n, why, UC(A1), n, UC(D1));
    wp_tile(50, 172, 66, 46, WP_KEY, WP_N(WP_KEY), 4, UC(A3), 1);
    wp_tile(124, 172, 66, 46, SA_ARROW_L, 9, 3, UC(A2), 0);
}

static void sa_paint_wifi(void)
{
    switch (ui.wp.page) {
    case WP_NET: wp_paint_net(); break;
    case WP_PASS: wp_paint_pass(); break;
    case WP_JOIN: wp_paint_join(); break;
    case WP_DONE: wp_paint_done(); break;
    case WP_FAIL: wp_paint_fail(); break;
    default:
        if (ui.wp.tab)
            wp_paint_kept();
        else
            wp_paint_around();
        break;
    }
}
