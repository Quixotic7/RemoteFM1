/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 in fm1-base's host simulator (tools/build.py firmware --host; base/host/plat_host.c, which includes app.c
 * at its end: its screen fb, its MIDI queue and its script are in scope). The port's stand-ins (rp_*: a USB host that
 * reads everything at once, the vendor rings in memory), a host on the other end of each transport (what the device
 * sends, parsed back with remote_proto.h's parsers), a reference screen the TILES commands also draw into, and the
 * script's commands and checks:
 *
 *   rf usb unplug | plug | reset | suspend | resume     the USB host goes, comes back, resets the bus, sleeps, wakes
 *   rf send T CMD [HEX...]       a message from the host on T (usb: SysEx through usb.c's tap, as the FM-1 has it;
 *                                bk: the vendor pair; ble: Bluetooth SysEx as packets through plat_midi_in), CMD hex
 *   rf hello T | rf panelreq T | rf session T 1|0 [KEEPALIVE_MS]
 *   rf led T WHAT LEVEL          one LED in the host's LED picture (WHAT: a key 0..26, bN a button by matrix number,
 *                                green; LEVEL lit, mid, dim, faint, off), and the whole picture sent (LEDS)
 *   rf tiles T FRAME end|hold|more RECT...   one TILES message (hold: END with HOLD), each RECT drawn into the
 *                                reference screen too:
 *                                fill:X,Y,W,H,C (C: RGB565, hex)  raw:X,Y,W,H,SEED  pal:X,Y,W,H,N,SEED
 *                                rle:X,Y,W,H,SEED  copy:X,Y,W,H,SX,SY  badpal:X,Y,W,H (n = 1: malformed)
 *   rf raw T HEX...              bytes as they are on T (usb: SysEx bytes through the tap; bk: into the ring)
 *   rf ota                       an update frame (F0 00 59 ... F7) from USB, between RemoteFM1's
 *   rf snap                      the reference screen kept as it is now (rf_screen_snap: the panel is that one)
 * expect (an int; "<N" / ">N" bounds): rf_session, rf_session_t (0 usb, 1 bk, 2 ble), rf_n_T_CC (replies of command
 * CC, hex, on T), rf_ack_frame, rf_ack_status, rf_ack_t, rf_ack_pending, rf_hello_caps, rf_hello_session,
 * rf_hello_transports, rf_hello_sxq, rf_hello_ident (1: the identity's first 8 characters), rf_panel_flags,
 * rf_panel_keys, rf_panel_buttons, rf_panel_master, rf_panel_encN, rf_shadow_ok (the shadow is the reference),
 * rf_screen_ok (the panel is), rf_screen_snap (the panel is the screen of rf snap), rf_screen_black, rf_frames,
 * rf_decode_errs, rf_ota_frames, rf_bad_T, rf_drop_T, rf_lit_keys, rf_lit_btns (plat.h's labels), rf_mid_keys,
 * rf_glow_keys, rf_held (tiles drawn, not shown: HOLD), rf_held_frames, rf_hold_timeouts */
#include <stdlib.h>
#include <string.h>
static void run_ms(uint32_t ms);                    /* (plat_host.c's, below) */

/* ------------------------------------------------------------------ the port --- */
static int hp_cfg = 1, hp_live = 1;
static uint32_t hp_resets;
static void rp_service(void) {}
static int rp_usb_cfg(void) { return hp_cfg; }
static int rp_usb_live(void) { return hp_cfg && hp_live; }
static uint32_t rp_usb_resets(void) { return hp_resets; }

/* the host on the other end: what the device sends, parsed per transport */
static rf_tstats_t hs_st[RF_NT];
static rf_sx_t hs_sx[2];
static rf_bk_t hs_bk;
static uint8_t hs_buf[RF_NT][RFM_BK_BODY_MAX];
static uint32_t hs_n[RF_NT][128];
static struct {
    uint32_t frame, status, pending, t;
} hs_ack;
static uint8_t hs_hello[RFM_HELLO_LEN], hs_panel[RFM_PANEL_LEN];
static void hs_msg(void *ctx, uint32_t cmd, const uint8_t *b, uint32_t len)
{
    uint32_t t = (uint32_t)(uintptr_t)ctx, i;
    hs_n[t][cmd & 127u]++;
    if (cmd == RFM_FRAME_ACK && len >= RFM_ACK_LEN) {
        hs_ack.frame = rf_rd16(b);
        hs_ack.status = b[2];
        hs_ack.pending = b[11];
        hs_ack.t = t;
    } else if (cmd == RFM_HELLO_R && len >= RFM_HELLO_LEN) {
        for (i = 0; i < RFM_HELLO_LEN; i++)
            hs_hello[i] = b[i];
    } else if (cmd == RFM_PANEL_R && len >= RFM_PANEL_LEN) {
        for (i = 0; i < RFM_PANEL_LEN; i++)
            hs_panel[i] = b[i];
    }
}
static void hs_pkt(rf_sx_t *s, uint32_t pkt)        /* a USB-MIDI SysEx packet's bytes to a parser */
{
    uint32_t cin = pkt & 15u, nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u, j;
    if (cin < 4u || cin > 7u)
        return;
    for (j = 0; j < nb; j++)
        rf_sx_byte(s, (pkt >> (8u * (j + 1u))) & 0xFFu);
}

static uint32_t rp_sx_room(void) { return SXQ; }
static void rp_sx_begin(void) {}
static void rp_sx_put(uint32_t pkt) { hs_pkt(&hs_sx[0], pkt); }
static void rp_sx_end(void) {}

#define HB_N 65536u
static uint8_t hb_in[HB_N];                          /* host -> device, the vendor pair */
static uint32_t hb_w, hb_r;
static uint32_t rp_bk_avail(void) { return hb_w - hb_r; }
static uint32_t rp_bk_read(uint8_t *dst, uint32_t n)
{
    uint32_t i;
    if (n > hb_w - hb_r)
        n = hb_w - hb_r;
    for (i = 0; i < n; i++)
        dst[i] = hb_in[hb_r++ % HB_N];
    return n;
}
static uint32_t rp_bk_room(void) { return hp_cfg ? 32768u : 0u; }
static int rp_bk_send(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb)
{
    if (!hp_cfg)
        return -1;
    rf_bk_feed(&hs_bk, a, na);
    rf_bk_feed(&hs_bk, b, nb);
    return 1;
}
static uint32_t rp_bk_rx_n(void) { return 32768u; }
static uint32_t rp_bk_tx_n(void) { return 32768u; }
static void rp_ble_put(uint32_t pkt)
{
    plat_midi_send(pkt, TO_BLE_HOST);                /* (counted and logged by the simulator) */
    hs_pkt(&hs_sx[1], pkt);
}
static void rp_lcd_baud(uint32_t b) { (void)b; }
static void rp_wdt(void) {}
#if !RFM_SHADOW
static void rp_wait_idle(void) {}
#endif
static void rp_counters(uint32_t *c)
{
    uint32_t i;
    for (i = 0; i < RC_N; i++)
        c[i] = 0;
    c[RC_RESETS] = hp_resets;
}

/* ------------------------------------------------------------------ the host's side: in --- */
/* USB SysEx, byte by byte as felucca-bsp's sysex_byte has it: the tap first; what it does not claim is collected for
 * the updater (one frame of 640 bytes at most), as ota.c would get it */
static uint8_t hu_on, hu_collect;
static uint32_t hu_pos, hu_ota_frames;
static void hu_byte(uint32_t b)
{
    if (b >= 0xF8u)
        return;
    if ((b & 0x80u) && b != 0xF0u && b != 0xF7u) {
        (void)rf_sx_tap(b);
        hu_on = hu_collect = 0;
        return;
    }
    if (b == 0xF0u) {
        hu_on = hu_collect = 1;
        hu_pos = 0;
    }
    if (rf_sx_tap(b))
        hu_collect = 0;
    if (!hu_on)
        return;
    if (hu_collect && b != 0xF0u && b != 0xF7u && ++hu_pos > 640u)
        hu_collect = 0;
    if (b == 0xF7u) {
        if (hu_collect && hu_pos)
            hu_ota_frames++;
        hu_on = hu_collect = 0;
    }
}
static int hb_ble_in(const uint8_t *m, uint32_t n)   /* Bluetooth: packets into the simulator's MIDI queue */
{
    uint32_t i;
    if (MQ - (mi_w - mi_r) < (n + 2u) / 3u)
        return -1;
    for (i = 0; i < n; i += 3u) {
        uint32_t k = n - i < 3u ? n - i : 3u, cin = i + k == n ? 4u + k : 4u;
        min_q[mi_w % MQ] = cin | (uint32_t)m[i] << 8 | (k > 1u ? (uint32_t)m[i + 1u] << 16 : 0u) |
                           (k > 2u ? (uint32_t)m[i + 2u] << 24 : 0u);
        min_port[mi_w++ % MQ] = PORT_BLE;
    }
    return 0;
}
static int hb_t(const char *s)                       /* a transport's name */
{
    return !strcmp(s, "usb") ? (int)RF_T_SX : !strcmp(s, "bk") ? (int)RF_T_BK : !strcmp(s, "ble") ? (int)RF_T_BLE : -1;
}
static uint8_t hb_m[3 * RFM_BK_BODY_MAX];
static int hb_send(int t, uint32_t cmd, const uint8_t *body, uint32_t n)   /* a message from the host on t */
{
    uint32_t i, m;
    if (t == (int)RF_T_BK) {
        rf_bk_header(hb_m, cmd, n, hb_w);
        for (i = 0; i < n; i++)
            hb_m[RF_BK_HDR + i] = body[i];
        for (i = 0; i < RF_BK_HDR + n; i++)
            hb_in[hb_w++ % HB_N] = hb_m[i];
        return 0;
    }
    if (n > RFM_SX_BODY_MAX + 64u)
        return -1;
    m = rf_sx_encode(cmd, body, n, hb_m);
    if (t == (int)RF_T_BLE)
        return hb_ble_in(hb_m, m);
    for (i = 0; i < m; i++)
        hu_byte(hb_m[i]);
    return 0;
}

/* ------------------------------------------------------------------ the reference screen --- */
static uint16_t ref[RFM_W * RFM_H];                  /* RGB565 values (the shadow keeps them byte-swapped) */
static uint16_t snap[RFM_W * RFM_H];                 /* ref as rf snap found it */
static uint16_t hb_px(uint32_t x, uint32_t y, uint32_t seed) { return (uint16_t)(x * 2654435761u ^ y * 40503u ^ seed * 977u); }
static uint16_t hb_pal(uint32_t k, uint32_t seed) { return (uint16_t)(seed * 9973u + k * 4099u + 0x1111u); }
static uint8_t hb_body[RFM_BK_BODY_MAX];
static uint32_t hb_len;
static void hb_put(uint32_t b) { if (hb_len < sizeof hb_body) hb_body[hb_len++] = (uint8_t)b; }
static void hb_put16be(uint32_t v) { hb_put(v >> 8); hb_put(v & 0xFFu); }
static int hb_rect(const char *s)                    /* one RECT token into the body and the reference */
{
    char kind[8] = {0};
    unsigned v[6] = {0};
    int n = sscanf(s, "%7[a-z]:%u,%u,%u,%u,%x,%x", kind, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    uint32_t x = v[0], y = v[1], w = v[2], h = v[3], i, j;
    if (n < 5)
        return -1;
    if (!strcmp(kind, "pal") || !strcmp(kind, "copy"))   /* (decimal) */
        n = sscanf(s, "%7[a-z]:%u,%u,%u,%u,%u,%u", kind, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    hb_put(x);
    hb_put(y);
    hb_put(w);
    hb_put(h);
    if (!strcmp(kind, "fill")) {
        hb_put(RFM_ENC_FILL);
        hb_put16be(v[4]);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                ref[(y + j) * RFM_W + x + i] = (uint16_t)v[4];
    } else if (!strcmp(kind, "raw")) {
        hb_put(RFM_ENC_RAW);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                uint16_t c = hb_px(x + i, y + j, v[4]);
                hb_put16be(c);
                ref[(y + j) * RFM_W + x + i] = c;
            }
    } else if (!strcmp(kind, "pal")) {
        uint32_t np = v[4], bpp = rfm_pal_bpp(np);
        hb_put(RFM_ENC_PAL);
        hb_put(np);
        for (i = 0; i < np; i++)
            hb_put16be(hb_pal(i, v[5]));
        for (j = 0; j < h; j++) {
            uint32_t acc = 0, nbits = 0;
            for (i = 0; i < w; i++) {
                uint32_t k = (i + 3u * j + v[5]) % np;
                ref[(y + j) * RFM_W + x + i] = hb_pal(k, v[5]);
                acc = acc << bpp | k;
                nbits += bpp;
                if (nbits == 8u) {
                    hb_put(acc);
                    acc = nbits = 0;
                }
            }
            if (nbits)
                hb_put(acc << (8u - nbits));         /* each row on a byte boundary, MSB first */
        }
    } else if (!strcmp(kind, "rle")) {
        uint32_t k, run = 0, total = w * h;
        uint16_t c = 0;
        hb_put(RFM_ENC_RLE);
        for (k = 0; k <= total; k++) {
            uint16_t p = k < total ? (uint16_t)(((k / 7u + v[4]) % 3u) * 0x2945u + 0x0821u) : 0;
            if (k < total)
                ref[(y + k / w) * RFM_W + x + k % w] = p;
            if (run && (k == total || p != c || run == 256u)) {
                hb_put(run - 1u);
                hb_put16be(c);
                run = 0;
            }
            c = p;
            run++;
        }
    } else if (!strcmp(kind, "copy")) {
        static uint16_t t[RFM_W * RFM_H];
        uint32_t sx = v[4], sy = v[5];
        hb_put(RFM_ENC_COPY);
        hb_put(sx);
        hb_put(sy);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                t[j * w + i] = ref[(sy + j) * RFM_W + sx + i];
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                ref[(y + j) * RFM_W + x + i] = t[j * w + i];
    } else if (!strcmp(kind, "badpal")) {
        hb_put(RFM_ENC_PAL);
        hb_put(1);                                   /* n = 1: out of range */
        hb_put16be(0xFFFFu);
    } else {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ the script --- */
static uint8_t hl[4][RFM_NCOL];                      /* the host's LED picture: lit, mid, dim, faint */
static uint8_t hl_green;
static int hb_hex(const char *q, uint8_t *out, uint32_t max)   /* hex bytes from q */
{
    uint32_t n = 0;
    char *e;
    for (;;) {
        unsigned long v = strtoul(q, &e, 16);
        if (e == q || n == max)
            break;
        out[n++] = (uint8_t)v;
        q = e;
    }
    return (int)n;
}
static const char *hb_word(const char *line, int k)  /* the k-th word of the line */
{
    const char *p = line;
    while (k--) {
        p += strspn(p, " \t");
        p += strcspn(p, " \t\r\n");
    }
    return p + strspn(p, " \t");
}
static void hb_run(void) { run_ms(2); }

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    int t = hb_t(b);
    uint8_t body[64];
    if (strcmp(cmd, "rf"))
        return 0;
    if (!strcmp(a, "usb")) {
        if (!strcmp(b, "unplug"))
            hp_cfg = 0;
        else if (!strcmp(b, "plug"))
            hp_cfg = hp_live = 1;
        else if (!strcmp(b, "reset"))
            hp_resets++;
        else if (!strcmp(b, "suspend"))
            hp_live = 0;
        else if (!strcmp(b, "resume"))
            hp_live = 1;
        hb_run();
        return 1;
    }
    if (!strcmp(a, "snap")) {
        memcpy(snap, ref, sizeof snap);
        return 1;
    }
    if (!strcmp(a, "ota")) {                         /* an update frame: F0 00 59 + its packed body + F7 */
        static const uint8_t F[] = {0xF0, 0x00, 0x59, 0x11, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
        uint32_t i;
        for (i = 0; i < sizeof F; i++)
            hu_byte(F[i]);
        hb_run();
        return 1;
    }
    if (t < 0) {
        printf("rf %s: a transport (usb, bk, ble)\n", a);
        exit(2);
    }
    if (!strcmp(a, "send")) {
        static uint8_t m[RFM_SX_BODY_MAX];
        int n = hb_hex(hb_word(plat_host_line, 4), m, sizeof m);
        if (hb_send(t, (uint32_t)strtoul(c, 0, 16), m, (uint32_t)n) < 0)
            printf("rf send: did not fit\n");
    } else if (!strcmp(a, "hello")) {
        hb_send(t, RFM_HELLO, body, 0);
    } else if (!strcmp(a, "panelreq")) {
        hb_send(t, RFM_PANEL_REQ, body, 0);
    } else if (!strcmp(a, "session")) {
        uint32_t ka = d[0] ? (uint32_t)atoi(d) : 0u;
        body[0] = (uint8_t)atoi(c);
        body[1] = (uint8_t)ka;
        body[2] = (uint8_t)(ka >> 8);
        hb_send(t, RFM_SESSION, body, 3);
        if (body[0])
            memset(ref, 0, sizeof ref);              /* the host's model: all black */
    } else if (!strcmp(a, "led")) {
        static const char *const LV[5] = {"lit", "mid", "dim", "faint", "off"};
        const char *what = c, *lv = d;
        uint32_t k, col = 0, bit = 0, r, cc;
        int level = -1, id = -1;
        for (k = 0; k < 5u; k++)
            if (!strcmp(lv, LV[k]))
                level = (int)k;
        if (!strcmp(what, "green"))
            col = RF_GREEN_COL, bit = RF_GREEN_BIT;
        else
            id = what[0] == 'b' ? atoi(what + 1) : 14 + atoi(what);
        for (r = 1; id >= 0 && r < 5u; r++)
            for (cc = 0; cc < RFM_NCOL; cc++)
                if (RF_KEYMAP[r][cc] == id)
                    col = cc, bit = 1u << r;
        if (level < 0 || !bit) {
            printf("rf led %s %s: no such LED or level\n", what, lv);
            exit(2);
        }
        for (k = 0; k < 4u; k++)
            hl[k][col] &= (uint8_t)~bit;
        if (col == RF_GREEN_COL && bit == RF_GREEN_BIT) {
            hl_green = level == 0;
        } else if (level < 4) {
            hl[level][col] |= (uint8_t)bit;
        }
        {
            uint8_t L[RFM_LEDS_LEN];
            for (k = 0; k < 4u; k++)
                memcpy(L + 11u * k, hl[k], RFM_NCOL);
            L[RFM_LEDS_FLAGS] = hl_green ? RFM_LEDS_F_PLAY_GREEN : 0u;
            hb_send(t, RFM_LEDS, L, RFM_LEDS_LEN);
        }
    } else if (!strcmp(a, "tiles")) {
        uint32_t frame = (uint32_t)atoi(c), k;
        hb_len = 0;
        hb_put(frame & 0xFFu);
        hb_put(frame >> 8);
        hb_put(!strcmp(d, "end") ? RFM_TILES_F_END : !strcmp(d, "hold") ? RFM_TILES_F_END | RFM_TILES_F_HOLD : 0u);
        hb_put(0);
        for (k = 0;; k++) {
            const char *w = hb_word(plat_host_line, 5 + (int)k);
            char tok[64] = {0};
            sscanf(w, "%63s", tok);
            if (!tok[0])
                break;
            if (hb_rect(tok) < 0) {
                printf("rf tiles: bad rectangle %s\n", tok);
                exit(2);
            }
        }
        if (hb_len == sizeof hb_body) {
            printf("rf tiles: more than a bulk body (%u bytes)\n", RFM_BK_BODY_MAX);
            exit(2);
        }
        hb_body[3] = (uint8_t)k;
        if (hb_send(t, RFM_TILES, hb_body, hb_len) < 0)
            printf("rf tiles: %u bytes do not fit a message\n", hb_len);
    } else if (!strcmp(a, "raw")) {
        static uint8_t m[4096];
        int n = hb_hex(hb_word(plat_host_line, 3), m, sizeof m), i;
        if (t == (int)RF_T_BK)
            for (i = 0; i < n; i++)
                hb_in[hb_w++ % HB_N] = m[i];
        else if (t == (int)RF_T_BLE)
            hb_ble_in(m, (uint32_t)n);
        else
            for (i = 0; i < n; i++)
                hu_byte(m[i]);
    } else {
        printf("rf %s: no such command\n", a);
        exit(2);
    }
    hb_run();
    return 1;
}

static int hb_screen_eq(const uint16_t *s, const uint16_t *r)   /* a screen (byte-swapped, as the panel takes it)
                                                                 * is r */
{
    uint32_t i;
    for (i = 0; i < RFM_W * RFM_H; i++)
        if (s[i] != swap16(r[i]))
            return 0;
    return 1;
}
static int hb_screen_is(const uint16_t *s) { return hb_screen_eq(s, ref); }

int app_expect(const char *what, int *got)
{
    int t;
    if (!strncmp(what, "rf_n_", 5)) {                /* rf_n_T_CC */
        char tn[4] = {0};
        unsigned cc = 0;
        if (sscanf(what + 5, "%3[a-z]_%x", tn, &cc) != 2 || (t = hb_t(tn)) < 0)
            return 0;
        *got = (int)hs_n[t][cc & 127u];
        return 1;
    }
    if (!strncmp(what, "rf_bad_", 7) && (t = hb_t(what + 7)) >= 0) {
        *got = (int)rf_st[t].bad;
        return 1;
    }
    if (!strncmp(what, "rf_drop_", 8) && (t = hb_t(what + 8)) >= 0) {
        *got = (int)rf_tx_drop[t];
        return 1;
    }
    if (!strncmp(what, "rf_panel_enc", 12)) {
        *got = (int16_t)rf_rd16(hs_panel + RFM_PANEL_ENC + 2u * ((uint32_t)atoi(what + 12) % RFM_NENC));
        return 1;
    }
    if (!strcmp(what, "rf_session"))
        *got = rfa.session;
    else if (!strcmp(what, "rf_session_t"))
        *got = rfa.st;
    else if (!strcmp(what, "rf_ack_frame"))
        *got = (int)hs_ack.frame;
    else if (!strcmp(what, "rf_ack_status"))
        *got = (int)hs_ack.status;
    else if (!strcmp(what, "rf_ack_pending"))
        *got = (int)hs_ack.pending;
    else if (!strcmp(what, "rf_ack_t"))
        *got = (int)hs_ack.t;
    else if (!strcmp(what, "rf_hello_caps"))
        *got = (int)rf_rd16(hs_hello + RFM_HELLO_CAPS);
    else if (!strcmp(what, "rf_hello_session"))
        *got = hs_hello[RFM_HELLO_SESSION];
    else if (!strcmp(what, "rf_hello_transports"))
        *got = hs_hello[25];
    else if (!strcmp(what, "rf_hello_sxq"))
        *got = (int)rf_rd16(hs_hello + 29);
    else if (!strcmp(what, "rf_hello_ident"))
        *got = !memcmp(hs_hello + 43, FELUCCA_ID, 8) && !memcmp(hs_hello, RFM_NAME, sizeof RFM_NAME - 1u) &&
               hs_hello[16] == RFM_PROTO;
    else if (!strcmp(what, "rf_panel_flags"))
        *got = hs_panel[RFM_PANEL_FLAGS];
    else if (!strcmp(what, "rf_panel_keys"))
        *got = (int)rf_rd32(hs_panel + RFM_PANEL_KEYS);
    else if (!strcmp(what, "rf_panel_buttons"))
        *got = (int)rf_rd16(hs_panel + RFM_PANEL_BUTTONS);
    else if (!strcmp(what, "rf_panel_master"))
        *got = (int)rf_rd16(hs_panel + RFM_PANEL_MASTER);
#if RFM_SHADOW
    else if (!strcmp(what, "rf_shadow_ok"))
        *got = hb_screen_is(rf_shadow);
#endif
    else if (!strcmp(what, "rf_screen_ok"))
        *got = hb_screen_is(fb);
    else if (!strcmp(what, "rf_screen_snap"))
        *got = hb_screen_eq(fb, snap);
    else if (!strcmp(what, "rf_held"))
        *got = rfa.held;
    else if (!strcmp(what, "rf_held_frames"))
        *got = (int)rfa.held_frames;
    else if (!strcmp(what, "rf_hold_timeouts"))
        *got = (int)rfa.hold_timeouts;
    else if (!strcmp(what, "rf_screen_diff")) {     /* the first pixel that differs, -1 none (debugging) */
        uint32_t i;
        *got = -1;
        for (i = 0; i < RFM_W * RFM_H && *got < 0; i++)
            if (fb[i] != swap16(ref[i]))
                *got = (int)i;
    }
    else if (!strcmp(what, "rf_screen_black")) {
        uint32_t i;
        *got = 1;
        for (i = 0; i < RFM_W * RFM_H; i++)
            if (fb[i])
                *got = 0;
    } else if (!strcmp(what, "rf_frames"))
        *got = (int)rfa.frames;
    else if (!strcmp(what, "rf_decode_errs"))
        *got = (int)rfa.decode_errs;
    else if (!strcmp(what, "rf_ota_frames"))
        *got = (int)hu_ota_frames;
    else if (!strcmp(what, "rf_lit_keys"))
        *got = (int)rf_led_lit_k;
    else if (!strcmp(what, "rf_lit_btns"))
        *got = (int)rf_led_lit_b;
    else if (!strcmp(what, "rf_mid_keys"))
        *got = (int)rf_led_mid_k;
    else if (!strcmp(what, "rf_glow_keys"))
        *got = (int)rf_led_glow_k;
    else
        return 0;
    return 1;
}

/* the host's parsers (from app_init) */
static void hs_init(void)
{
    rf_body_init(&hs_sx[0].b, &hs_st[RF_T_SX], hs_buf[RF_T_SX], RFM_BK_BODY_MAX, hs_msg, (void *)(uintptr_t)RF_T_SX);
    rf_body_init(&hs_bk.b, &hs_st[RF_T_BK], hs_buf[RF_T_BK], RFM_BK_BODY_MAX, hs_msg, (void *)(uintptr_t)RF_T_BK);
    rf_body_init(&hs_sx[1].b, &hs_st[RF_T_BLE], hs_buf[RF_T_BLE], RFM_BK_BODY_MAX, hs_msg, (void *)(uintptr_t)RF_T_BLE);
}
