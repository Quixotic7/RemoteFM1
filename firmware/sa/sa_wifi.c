/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 Wi-Fi: Wi-Fi on the FM-1 itself (design/wifi-settings: README.md, its mockups; docs/WIFI.md "On the FM-1
 * itself"). Part of sa_main.c in the Wi-Fi build only (FM1B_WIFI); its screens are sa_wifi_draw.c.
 *
 * The settings' last row, WI-FI: KN1 turns Wi-Fi off or on (kept with the settings, sa.wifi_off: at power-on the radio
 * starts only when it was left on), OCT+ opens the Wi-Fi page (and turns Wi-Fi on). The page (sa.base SA_SCR_WIFI):
 *   LIST   the network joined (pick 0) and the networks around, or (ALGORITHM) the networks kept; SELECT the pick,
 *          turned up past the top a scan; OCT+ opens the pick: a kept one NET, a secured one PASS, an open one joins
 *   NET    a network kept: JOIN, a new password, FORGET (twice: OCT+ arms, OCT+ again forgets; anything else, or 3 s,
 *          disarms); SELECT the tile, OCT+ does it
 *   PASS   the password typed on the keys (four layers on FX / SEL / ENV / LFO, GLO held: ABC for the next key), EDIT
 *          shows / hides it, ALGORITHM the cursor, SELECT / KN1 the character under it, OCT- deletes (empty: back),
 *          GLO + OCT- clears, OCT+ joins (0 or 8..63 characters), HOME cancels
 *   JOIN   the join under way (OCT- cancels); DONE joined (2 s or any control: LIST); FAIL why (OCT+ the password
 *          again, as typed; OCT- LIST)
 * OCT- goes back a level, HOME closes the settings (in PASS it cancels). The lists are fm1-base's own (plat_wifi_found,
 * plat_wifi_saved), read when drawn; only the network picked's name is kept (wp_net). The password (wp_pw) is held
 * while it is typed and until its join has an answer, handed to fm1-base's join the way WIFI SET does (rf_wifi_hand,
 * src/rfm_wifi.c: kept by device/wifi.c in OBJ_WIFI once joined), wiped when the join is done or given up, and
 * whenever the page closes (another screen, a host's session); never logged or sent. The page never scans while a
 * join is under way (or about to be handed over). */

enum { WP_LIST, WP_NET, WP_PASS, WP_JOIN, WP_DONE, WP_FAIL };
enum { WP_WHY_JOIN, WP_WHY_PASS, WP_WHY_NOTFOUND, WP_WHY_ADDR };
#define WP_PW_MAX 63u                        /* (RFM_WIFI_PASS_MAX: WPA's longest) */
#define WP_ROWS 4u                           /* rows a list shows */
#define WP_ARM_MS 3000u                      /* FORGET armed this long */
#define WP_DONE_MS 2000u
#define WP_SCAN_MS 12000u                    /* a scan not done after this long: given up (RFM_WIFI_SCAN_MS) */

typedef struct {
    uint8_t open;                            /* the page is up (its state to be wiped when it is not) */
    uint8_t page, tab;                       /* WP_*; LIST's: 0 the networks around, 1 the ones kept */
    uint8_t pick, top;                       /* LIST: the pick (around: 0 the network joined, when one is), the first
                                              * row shown */
    uint8_t tile, armed;                     /* NET: JOIN, PASSWORD, FORGET; FORGET armed */
    uint8_t layer, shown, len, cur, last;    /* PASS: abc ABC 123 #+=; shown; the length, the cursor, the character
                                              * last typed (its place) */
    uint8_t lit_k, typed;                    /* the key last typed (+1, 0 none); a password was typed for this join */
    uint8_t scan_due, seen, why, from_net;   /* a scan to ask; JOIN: fm1-base took the join; FAIL: WP_WHY_*; PASS
                                              * came from NET (HOME / OCT- go back there) */
    uint32_t t0, armed_ms, lit_ms, typed_ms, join_ms, scan_at, scan_base;
} sa_wp_t;
static sa_wp_t wp;
static char wp_net[33];                      /* the network picked: its name */
static char wp_pw[WP_PW_MAX + 1u];           /* the password typed (wiped: wp_wipe) */
static uint32_t wp_scans_asked, wp_joins, wp_forgets;   /* (the simulator's checks) */

static int sa_streq(const char *a, const char *b);   /* (sa_ui.c) */
/* from src/rfm_wifi.c (below in the unity build) */
static void rf_wipe(void *p, uint32_t n);
static void rf_wifi_hand(const char *ssid, const char *pass);   /* a join, as WIFI SET (pass 0: a kept one's) */
static int rf_wifi_due(void);                /* a join handed over and not yet given to fm1-base */
static char *rf_put_ip(char *d, uint32_t ip);

/* the four layers: black keys B1..B11, white keys W1..W16 (every printable ASCII character on one at least) */
static const char *const WP_LAY[4][2] = {{"abcdefghijk", "lmnopqrstuvwxyz "},
                                         {"ABCDEFGHIJK", "LMNOPQRSTUVWXYZ "},
                                         {"0123456789-", ".,!?@#$%&*()_+= "},
                                         {"0123456789-", "'\"/\\:;<>[]{}|^`~"}};

static void wp_wipe(void)
{
    rf_wipe(wp_pw, sizeof wp_pw);
    wp.len = wp.cur = wp.last = 0;
    wp.typed = 0;
}

static void wp_page(uint32_t p)
{
    wp.page = (uint8_t)p;
    wp.t0 = plat_ms();
    wp.armed = 0;
    sa.poke = 1;
}

static void wp_close(void)                   /* the page gone: nothing of it kept */
{
    wp_wipe();
    memset(&wp, 0, sizeof wp);
    memset(wp_net, 0, sizeof wp_net);
}

/* ------------------------------------------------------------------ the lists (fm1-base's) --- */
static int wp_st(void) { return plat_wifi_state(); }

static int wp_joined(char *s)                /* connected: 1, the network's name in s (33) */
{
    const char *j = plat_wifi_ssid();
    if (wp_st() != WIFI_CONNECTED || !j || !j[0])
        return 0;
    if (s)
        sa_cpy(s, j);
    return 1;
}

static uint32_t wp_nfound(void)
{
    uint32_t n = 0;
    while (n < 16u && plat_wifi_found((int)n, 0, 0, 0))
        n++;
    return n;
}

static uint32_t wp_nkept(void)
{
    uint32_t n = 0;
    while (n < 6u && plat_wifi_saved((int)n, 0))
        n++;
    return n;
}

static int wp_heard(const char *ssid, int *rssi, int *sec, int *ch)   /* in the scan's list: its index, else -1 */
{
    char s[33];
    int i, r, c;
    for (i = 0; i < 16 && plat_wifi_found(i, s, &r, &c); i++)
        if (sa_streq(s, ssid)) {
            if (rssi)
                *rssi = r;
            if (sec)
                *sec = c;
            if (ch)
                *ch = plat_wifi_found_channel(i);
            return i;
        }
    return -1;
}

/* the networks around as LIST shows them: the network joined first (when there is one), then the scan's others; entry
 * v: its name (s, 33), signal and lock; returns the scan's index, -2 the network joined, -1 none */
static int wp_entry(uint32_t v, char *s, int *rssi, int *sec)
{
    char j[33];
    int hero = wp_joined(j), i;
    if (hero) {
        if (v == 0u) {
            sa_cpy(s, j);
            if (wp_heard(j, 0, sec, 0) < 0)
                *sec = 1;
            *rssi = plat_wifi_rssi();
            return -2;
        }
        v--;
    }
    for (i = 0; i < 16 && plat_wifi_found(i, s, rssi, sec); i++) {
        if (hero && sa_streq(s, j))
            continue;
        if (v-- == 0u)
            return i;
    }
    return -1;
}

static uint32_t wp_count(void)               /* LIST's entries (the tab's) */
{
    uint32_t n;
    if (wp.tab)
        return wp_nkept();
    n = wp_nfound();
    if (wp_joined(0) && wp_heard(plat_wifi_ssid(), 0, 0, 0) < 0)
        n++;                                 /* (joined, not in the scan's list: shown on top all the same) */
    return n;
}

static int wp_scanning(void)
{
    if (wp.scan_at && (plat_wifi_scans() != wp.scan_base || plat_ms() - wp.scan_at >= WP_SCAN_MS))
        wp.scan_at = 0;
    return wp.scan_at != 0;
}

static int wp_joining(void)                  /* a join under way or about to be handed over: no scan now */
{
    int st = wp_st();
    return st == WIFI_JOINING || st == WIFI_ADDRESS || rf_wifi_due();
}

static void wp_scan(void)                    /* a scan asked (the radio up, no join under way) */
{
    int st = wp_st();
    if (wp_joining() || st == WIFI_OFF || st == WIFI_STARTING) {
        wp.scan_due = (uint8_t)(st == WIFI_OFF || st == WIFI_STARTING);   /* (when the radio is up) */
        return;
    }
    wp.scan_due = 0;
    if (wp_scanning())
        return;
    if (plat_wifi_scan() == 0) {
        wp.scan_base = plat_wifi_scans();
        wp.scan_at = plat_ms() | 1u;
        wp_scans_asked++;
    }
    sa.poke = 1;
}

/* ------------------------------------------------------------------ on / off, the page --- */
static void sa_wifi_set_on(int on)           /* KN1 on the WI-FI row; kept with the settings */
{
    int was = wp_st() != WIFI_OFF;
    sa.mfocus = SA_SET_WIFI;
    sa.poke = 1;
    if (on == was)
        return;
    sa.wifi_off = (uint8_t)!on;
    sa_changed();
    plat_wifi_on(on);                        /* (on: the radio calibrates first, up to 15 s: fm1-base holds the
                                              * input and the sound meanwhile) */
}

static void sa_wifi_turned_on(void)          /* WIFI SET / SCAN over USB turned the radio on: kept so */
{
    if (sa.wifi_off) {
        sa.wifi_off = 0;
        sa_changed();
    }
}

static void wp_open(void)                    /* OCT+ on the WI-FI row */
{
    wp_close();
    wp.open = 1;
    wp_page(WP_LIST);
    if (wp_st() == WIFI_OFF)
        sa_wifi_set_on(1);
    if (!wp_joining())                       /* (on opening, unless a join is under way) */
        wp.scan_due = 1;
    sa_overlay_menu(SA_SCR_WIFI);
}

static void wp_back_settings(void)
{
    wp_close();
    sa.mfocus = SA_SET_WIFI;
    sa_overlay_menu(SA_SCR_SETTINGS);
}

static void wp_list(void)                    /* back to the list (the password wiped) */
{
    wp_wipe();
    wp_page(WP_LIST);
}

static void wp_pass(int keep)                /* the password page: kept as typed (the cursor at its end), or new */
{
    if (!keep) {
        wp_wipe();
        wp.layer = wp.shown = 0;             /* (a new one: abc, hidden) */
    }
    wp.cur = wp.len;
    wp.typed_ms = plat_ms();
    wp_page(WP_PASS);
}

static void wp_join(int kept)                /* the network picked joined: its password kept (fm1-base's), or the one
                                              * typed (none: an open network) */
{
    rf_wifi_hand(wp_net, kept ? 0 : wp_pw);
    wp.typed = (uint8_t)(!kept && wp.len);
    wp.seen = 0;
    wp.join_ms = plat_ms();
    wp_joins++;
    wp_page(WP_JOIN);
}

static void wp_cancel(void)                  /* JOIN, OCT-: the networks kept again (the most recent other one), else
                                              * the join stopped (a scan stops it: device/wifi.c) */
{
    char s[33];
    int i;
    for (i = 0; i < 6 && plat_wifi_saved(i, s); i++)
        if (!sa_streq(s, wp_net)) {
            rf_wifi_hand(s, 0);
            wp_list();
            return;
        }
    rf_wifi_hand(0, 0);                      /* (one not handed over yet: dropped) */
    if (wp_st() == WIFI_JOINING || wp_st() == WIFI_ADDRESS)
        plat_wifi_scan();
    wp_list();
}

static void wp_forget(void)
{
    char s[33];
    int i;
    for (i = 0; i < 6 && plat_wifi_saved(i, s); i++)
        if (sa_streq(s, wp_net)) {
            plat_wifi_forget(i);
            wp_forgets++;
            break;
        }
    wp.tab = (uint8_t)(wp.tab && wp_nkept());
    wp.pick = wp.top = 0;
    wp_list();
}

static void wp_open_pick(void)               /* LIST, OCT+ */
{
    int rssi = 0, sec = 0, e;
    if (wp.pick >= wp_count())
        return;
    if (wp.tab) {
        plat_wifi_saved(wp.pick, wp_net);
        wp.tile = 0;
        wp_page(WP_NET);
        return;
    }
    e = wp_entry(wp.pick, wp_net, &rssi, &sec);
    if (e == -1)
        return;
    wp.from_net = 0;
    if (e == -2 || plat_wifi_kept(wp_net)) {
        wp.tile = 0;
        wp_page(WP_NET);
    } else if (sec) {
        wp_pass(0);
    } else {
        wp_wipe();
        wp_join(0);                          /* (open: at once) */
    }
}

/* ------------------------------------------------------------------ the password --- */
static void wp_typed(void)
{
    wp.typed_ms = plat_ms();
    sa.poke = 1;
}

static void wp_insert(char c)
{
    uint32_t i;
    if (wp.len >= WP_PW_MAX)
        return;
    for (i = wp.len; i > wp.cur; i--)
        wp_pw[i] = wp_pw[i - 1u];
    wp_pw[wp.cur] = c;
    wp.len++;
    wp_pw[wp.len] = 0;
    wp.last = wp.cur;
    wp.cur++;
    wp_typed();
}

static void wp_delete(void)                  /* the character before the cursor (at the start: the one under it) */
{
    uint32_t i, at;
    if (!wp.len)
        return;
    at = wp.cur ? wp.cur - 1u : 0u;
    for (i = at; i + 1u < wp.len; i++)
        wp_pw[i] = wp_pw[i + 1u];
    wp.len--;
    wp_pw[wp.len] = 0;
    wp.cur = (uint8_t)at;
    wp.last = 0xFFu;                         /* (nothing just typed shows) */
    wp_typed();
}

static void wp_turn_char(int32_t d)          /* SELECT / KN1: the character under the cursor, through all 95; at the end
                                              * a new one (from 'a') */
{
    int32_t c;
    if (wp.cur >= wp.len) {
        if (wp.len >= WP_PW_MAX)
            return;
        wp_insert('a');
        wp.cur--;
        d += d > 0 ? -1 : 0;
    }
    c = ((int32_t)wp_pw[wp.cur] - 32 + d % 95 + 95) % 95 + 32;
    wp_pw[wp.cur] = (char)c;
    wp.last = wp.cur;
    wp_typed();
}

static int wp_can_join(void) { return wp.len == 0u || wp.len >= 8u; }   /* (WPA: 8..63) */

static int wp_glo(void) { return (int)(sa.btns >> B_GLO & 1u); }

/* a key down while the password is up: its character typed (1: taken) */
static int sa_wifi_key(uint32_t k)
{
    int32_t w = sa_key_white(k), b = sa_key_black(k);
    uint32_t lay;
    if (sa.base != SA_SCR_WIFI || wp.page != WP_PASS)
        return 0;
    lay = wp_glo() ? 1u : wp.layer;
    if (w >= 0)
        wp_insert(WP_LAY[lay][1][w]);
    else if (b >= 0)
        wp_insert(WP_LAY[lay][0][b]);
    wp.lit_k = (uint8_t)(k + 1u);
    wp.lit_ms = plat_ms();
    return 1;
}

/* ------------------------------------------------------------------ the controls --- */
/* buttons down (sa_buttons, before anything else sees them): the ones the page takes */
static uint32_t sa_wifi_buttons(uint32_t down, uint32_t btns)
{
    const uint32_t OD = 1u << B_OCTDN, OU = 1u << B_OCTUP;
    uint32_t eat = 0;
    (void)btns;
    if (sa.panel || !down)
        return 0;
    if (sa.base == SA_SCR_SETTINGS && sa.menu && sa.mfocus == SA_SET_WIFI && (down & OU)) {
        wp_open();                           /* OCT+ on the WI-FI row: the page */
        return OU;
    }
    if (sa.base != SA_SCR_WIFI)
        return 0;
    if (wp.armed && (down & ~OU))            /* anything but OCT+: FORGET disarmed */
        wp.armed = 0;
    switch (wp.page) {
    case WP_LIST:
        if (down & OU)
            wp_open_pick();
        if (down & OD)
            wp_back_settings();
        return down & (OU | OD);
    case WP_NET:
        if (down & OU) {
            if (wp.tile == 0u) {
                wp.typed = 0;
                wp_join(1);
            } else if (wp.tile == 1u) {
                wp.from_net = 1;
                wp_pass(0);
            } else if (!wp.armed) {
                wp.armed = 1;
                wp.armed_ms = plat_ms();
                sa.poke = 1;
            } else {
                wp_forget();
            }
        }
        if (down & OD)
            wp_list();
        return down & (OU | OD);
    case WP_PASS:
        eat = down & (OU | OD | 1u << B_FX | 1u << B_SEL | 1u << B_ENV | 1u << B_LFO | 1u << B_EDIT | 1u << B_HOME);
        if (down & (1u << B_FX | 1u << B_SEL | 1u << B_ENV | 1u << B_LFO)) {
            uint32_t b;
            for (b = B_FX; b <= B_LFO; b++)
                if (down >> b & 1u)
                    wp.layer = (uint8_t)(b - B_FX);
            sa.poke = 1;
        }
        if (down >> B_EDIT & 1u) {
            wp.shown = (uint8_t)!wp.shown;
            sa.poke = 1;
        }
        if (down >> B_HOME & 1u) {           /* cancel: back where it came from, nothing kept */
            if (wp.from_net) {
                wp_wipe();
                wp_page(WP_NET);
            } else {
                wp_list();
            }
            return eat;
        }
        if (down & OD) {
            if (wp_glo()) {                  /* GLO + OCT-: all of it */
                wp_wipe();
                wp_typed();
            } else if (!wp.len) {            /* empty: back */
                if (wp.from_net)
                    wp_page(WP_NET);
                else
                    wp_list();
                return eat;
            } else {
                wp_delete();
            }
        }
        if ((down & OU) && wp_can_join())
            wp_join(0);
        return eat;
    case WP_JOIN:
        if (down & OD)
            wp_cancel();
        return down & (OU | OD);
    case WP_DONE:
        wp_list();                           /* (any control) */
        return down & (OU | OD);
    case WP_FAIL:
        if (down & OU)
            wp_pass(1);
        else if (down & OD)
            wp_list();
        return down & (OU | OD);
    default:
        return 0;
    }
}

static void sa_wifi_select(int32_t d)        /* SELECT on the page */
{
    if (wp.armed)
        wp.armed = 0;
    switch (wp.page) {
    case WP_LIST: {
        int32_t n = (int32_t)wp_count(), p = (int32_t)wp.pick + d;
        if (p < 0) {                         /* up past the top: a scan */
            if (!wp.tab)
                wp_scan();
            p = 0;
        }
        wp.pick = (uint8_t)clamp(p, 0, n > 0 ? n - 1 : 0);
        break;
    }
    case WP_NET: wp.tile = (uint8_t)clamp((int32_t)wp.tile + d, 0, 2); break;
    case WP_PASS: wp_turn_char(d); break;
    case WP_DONE: wp_list(); break;
    default: break;
    }
    sa.poke = 1;
}

static void sa_wifi_algo(int32_t c)          /* ALGORITHM: LIST's tab; PASS's cursor */
{
    if (wp.armed)
        wp.armed = 0;
    if (wp.page == WP_LIST) {
        wp.tab = (uint8_t)!wp.tab;
        wp.pick = wp.top = 0;
    } else if (wp.page == WP_PASS) {
        wp.cur = (uint8_t)clamp((int32_t)wp.cur + c, 0, wp.len);
        wp.last = 0xFFu;
        wp_typed();
    } else if (wp.page == WP_DONE) {
        wp_list();
    }
    sa.poke = 1;
}

static void sa_wifi_knob(uint32_t k, int32_t d)   /* KN1..KN4 on the page: KN1 the password's character */
{
    if (wp.armed)
        wp.armed = 0;
    if (wp.page == WP_PASS && k == 0u)
        wp_turn_char(d);
    else if (wp.page == WP_DONE)
        wp_list();
}

/* ------------------------------------------------------------------ every millisecond --- */
static void sa_wifi_ms(void)
{
    uint32_t now = plat_ms();
    int st;
    if (sa.panel || sa.base != SA_SCR_WIFI) {
        if (wp.open)
            wp_close();                      /* (another screen, a host's session: nothing of the page kept) */
        return;
    }
    if (!wp.open) {                          /* (opened some other way: as by OCT+) */
        wp.open = 1;
        wp_page(WP_LIST);
    }
    st = wp_st();
    if (wp.scan_due && st != WIFI_OFF && st != WIFI_STARTING)
        wp_scan();
    if (wp.armed && now - wp.armed_ms >= WP_ARM_MS) {
        wp.armed = 0;
        sa.poke = 1;
    }
    if (wp.page == WP_LIST || wp.page == WP_NET) {   /* (the list moved: the pick kept inside it) */
        uint32_t n = wp_count();
        if (wp.page == WP_LIST && wp.pick >= n)
            wp.pick = (uint8_t)(n ? n - 1u : 0u);
    }
    if (wp.page == WP_JOIN && !rf_wifi_due()) {
        const char *ss = plat_wifi_ssid();
        int ours = ss && sa_streq(ss, wp_net);
        if ((st == WIFI_JOINING || st == WIFI_ADDRESS) && ours)
            wp.seen = 1;
        if (!ours && (st == WIFI_JOINING || st == WIFI_ADDRESS || st == WIFI_CONNECTED) &&
            now - wp.join_ms > 100u) {
            wp_list();                       /* (another join took over: WIFI SET over USB) */
        } else if (wp.seen && st == WIFI_CONNECTED) {
            wp_wipe();                       /* joined: kept by fm1-base, ours gone */
            wp_page(WP_DONE);
        } else if (wp.seen && st == WIFI_FAILED) {
            const char *why = plat_wifi_why();
            if (why && sa_streq(why, "no address"))
                wp.why = WP_WHY_ADDR;
            else if (wp.typed && wp_heard(wp_net, 0, 0, 0) >= 0)
                wp.why = WP_WHY_PASS;        /* (fm1-base says only "could not join": the owner's best guess) */
            else if (why && sa_streq(why, "not found"))
                wp.why = WP_WHY_NOTFOUND;
            else
                wp.why = WP_WHY_JOIN;
            wp_page(WP_FAIL);
        }
    }
    if (wp.page == WP_DONE && now - wp.t0 >= WP_DONE_MS)
        wp_list();
}

/* the LEDs while the page is up: OCT- / OCT+ lit while they do something; the layer's button and EDIT (shown) in the
 * password; the key last typed a moment */
static void sa_wifi_leds(uint32_t *b, uint32_t *k)
{
    uint32_t B = *b & (1u << B_GLO);
    if (sa.base == SA_SCR_SETTINGS && sa.menu && sa.mfocus == SA_SET_WIFI) {
        *b |= 1u << B_OCTUP;                 /* (OCT+ opens the page) */
        return;
    }
    if (sa.base != SA_SCR_WIFI)
        return;
    B |= 1u << B_OCTDN;
    if ((wp.page == WP_LIST && wp_count()) || wp.page == WP_NET || wp.page == WP_FAIL ||
        (wp.page == WP_PASS && wp_can_join()))
        B |= 1u << B_OCTUP;
    if (wp.page == WP_PASS) {
        B |= 1u << (B_FX + (wp_glo() ? 1u : wp.layer));
        if (wp.shown)
            B |= 1u << B_EDIT;
        *k = wp.lit_k && plat_ms() - wp.lit_ms < 250u ? 1u << (wp.lit_k - 1u) : 0u;
    }
    *b = B;
}
