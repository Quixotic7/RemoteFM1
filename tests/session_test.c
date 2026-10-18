/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Host test of RemoteFM1's protocol layer (firmware/src/rfm_app.c with rfm_draw.c and rfm_push.c, the code the
 * device runs), driven from both transports' wire bytes through remote_proto.h's parsers (SysEx: rf_sx_encode ->
 * rf_sx_byte; bulk: rf_bk_header -> rf_bk_feed), as remote.c feeds them. The HAL is replaced by a log of what the
 * layer sends (messages, MIDI, LEDs) and a model panel. Every reply is checked byte for byte against
 * docs/PROTOCOL.md: HELLO_R (67 bytes, and its SysEx framing), SESSION -> ACK + PANEL_R + MASTER, the black screen,
 * LEDS mapped to three levels, frames over several TILES with END -> FRAME_ACK (shadow reset, decode error, two
 * ENDs while a push runs, an empty frame), the panel events as plain MIDI, PANEL_R on request and every 500 ms,
 * the keepalive ending the session, SESSION 0, a USB loss, TILES outside a session, ECHO, M0's raw TILE.
 *   cc -std=c99 -Ifirmware/src -o session_test tests/session_test.c && ./session_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/src/remote_proto.h"
#define RFA_FW_VER "0.1"
#define RFA_IDENTITY "FM-1_015"
#include "../firmware/src/rfm_draw.c"
#include "../firmware/src/rfm_push.c"
#include "../firmware/src/rfm_app.c"

static int fails, checks;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            fails++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                        \
    } while (0)

/* ------------------------------------------------------------ the fakes --- */
typedef struct {
    uint32_t t, cmd, len;
    uint8_t b[1100];
} msg_t;
static msg_t out[64];
static uint32_t nout;
static uint8_t wire_sx[4096];                           /* the last SysEx reply as it would go on the wire */
static uint32_t wire_sx_n;
static int rfa_tx(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    if (nout < 64) {
        out[nout].t = t;
        out[nout].cmd = cmd;
        out[nout].len = n;
        memcpy(out[nout].b, body, n < 1100 ? n : 1100);
    }
    nout++;
    if (t == RF_T_SX)
        wire_sx_n = rf_sx_encode(cmd, body, n, wire_sx);
    return 1;
}
static uint8_t midi[256][3];
static uint32_t nmidi;
static void rfa_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (nmidi < 256) {
        midi[nmidi][0] = (uint8_t)st;
        midi[nmidi][1] = (uint8_t)d1;
        midi[nmidi][2] = (uint8_t)d2;
    }
    nmidi++;
}
static uint8_t led_lit[11], led_glow[11], led_bg[11];
static uint32_t nleds;
static void rfa_leds(const uint8_t *lit, const uint8_t *glow, const uint8_t *bg)
{
    memcpy(led_lit, lit, 11);
    memcpy(led_glow, glow, 11);
    memcpy(led_bg, bg, 11);
    nleds++;
}
static void rfa_hello_dev(uint8_t *h, uint32_t t)
{
    uint32_t i;
    for (i = 25; i < 43; i++)
        h[i] = (uint8_t)(0xA0 + i + t);
}
static int32_t session_on = -1;
static void rfa_session_changed(uint32_t on) { session_on = (int32_t)on; }
static uint32_t now_us;
static uint32_t rfa_us(void) { return now_us += 7; }   /* every look at the clock: 7 us later */
static uint32_t diag_cmd, ndiag;
static void rfa_diag(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    (void)t, (void)body, (void)len;
    diag_cmd = cmd;
    ndiag++;
}

/* the model panel: a transfer completes on the 3rd look */
static uint16_t panel[240 * 240];
static struct {
    uint32_t x0, y0, x1, y1, cx, cy, looks, pending, windows, bad;
    const uint16_t *src;
    uint32_t n;
} L;
static int lcd_idle(void *c)
{
    (void)c;
    if (L.pending && ++L.looks >= 3) {
        uint32_t i;
        for (i = 0; i < L.n; i++) {
            if (L.cy > L.y1)
                L.bad++;
            else
                panel[L.cy * 240 + L.cx] = L.src[i];
            if (++L.cx > L.x1) {
                L.cx = L.x0;
                L.cy++;
            }
        }
        L.pending = 0;
    }
    return !L.pending;
}
static void lcd_window(void *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    (void)c;
    L.bad += L.pending;
    L.x0 = L.cx = x0, L.y0 = L.cy = y0, L.x1 = x1, L.y1 = y1;
    L.windows++;
}
static void lcd_data(void *c, const void *p, uint32_t n)
{
    (void)c;
    L.bad += L.pending || (n & 1);
    L.src = p;
    L.n = n / 2;
    L.pending = 1;
    L.looks = 0;
}
static const rfm_lcd_ops_t OPS = {lcd_idle, lcd_window, lcd_data, NULL};
static uint16_t shadow[240 * 240], stage[240 * 16];

/* ------------------------------------------------------------ the host side --- */
static rf_tstats_t st[2];
static rf_sx_t sx;
static rf_bk_t bk;
static uint8_t sx_buf[RF_SX_BODY_MAX], bk_buf[RFM_BK_BODY_MAX];
static uint32_t now_ms = 1000;
static void on_msg(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    rfm_app_msg((uint32_t)(uintptr_t)ctx, cmd, body, len);
}
static void send_sx(uint32_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t w[3000];
    uint32_t k = rf_sx_encode(cmd, body, n, w), i;
    rfm_app_activity();
    for (i = 0; i < k; i++) {
        if (i == k / 2)
            rf_sx_byte(&sx, 0xF8);                      /* a clock in the middle: ignored */
        rf_sx_byte(&sx, w[i]);
    }
}
static uint32_t bk_seq;
static void send_bk(uint32_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t w[RF_BK_HDR + RFM_BK_BODY_MAX];
    uint32_t i;
    rf_bk_header(w, cmd, n, bk_seq++);
    memcpy(w + RF_BK_HDR, body, n);
    rfm_app_activity();
    for (i = 0; i < RF_BK_HDR + n; i += 64)              /* 64-byte USB packets */
        rf_bk_feed(&bk, w + i, RF_BK_HDR + n - i < 64 ? RF_BK_HDR + n - i : 64);
}
static void send(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    if (t == RF_T_SX)
        send_sx(cmd, body, n);
    else
        send_bk(cmd, body, n);
}
static void poll(uint32_t n)                            /* n main loop passes, a ms each */
{
    while (n--) {
        rfm_app_poll(now_ms);
        now_ms++;
    }
}
static void poll_same_ms(uint32_t n)
{
    while (n--)
        rfm_app_poll(now_ms);
}
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static const int32_t NO_STEPS[7];

static void check_panel_r(const msg_t *m, uint32_t t, uint32_t keys, uint32_t btns, const int16_t *enc,
                          uint32_t master, uint32_t session)
{
    uint8_t want[25];
    uint32_t e;
    memset(want, 0, sizeof want);
    want[0] = (uint8_t)keys, want[1] = (uint8_t)(keys >> 8), want[2] = (uint8_t)(keys >> 16), want[3] = (uint8_t)(keys >> 24);
    want[4] = (uint8_t)btns, want[5] = (uint8_t)(btns >> 8);
    for (e = 0; e < 7; e++)
        want[6 + 2 * e] = (uint8_t)enc[e], want[7 + 2 * e] = (uint8_t)((uint16_t)enc[e] >> 8);
    want[20] = (uint8_t)master, want[21] = (uint8_t)(master >> 8);
    want[22] = want[23] = 0;                            /* battery unknown */
    want[24] = (uint8_t)session;
    CHECK(m->cmd == RFM_PANEL_R && m->t == t && m->len == 25 && !memcmp(m->b, want, 25));
}

static void check_ack(const msg_t *m, uint32_t t, uint32_t frame, uint32_t status, uint32_t bytes, uint32_t pending)
{
    CHECK(m->cmd == RFM_FRAME_ACK && m->t == t && m->len == RFM_ACK_LEN);
    CHECK(rd16(m->b) == frame && m->b[2] == status && rd32(m->b + 7) == bytes && m->b[11] == pending);
    CHECK(rd32(m->b + 3) > 0 && rd32(m->b + 3) < 100000);   /* draw_us: decode + push, on the fake clock */
    if (rd16(m->b) != frame || m->b[2] != status || rd32(m->b + 7) != bytes || m->b[11] != pending)
        fprintf(stderr, "  ack: frame %u status %u bytes %u pending %u\n", rd16(m->b), m->b[2], rd32(m->b + 7),
                m->b[11]);
}

/* a TILES body: frame, flags, one FILL rect */
static uint32_t tiles_fill(uint8_t *b, uint32_t frame, uint32_t flags, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t c)
{
    b[0] = (uint8_t)frame, b[1] = (uint8_t)(frame >> 8), b[2] = (uint8_t)flags, b[3] = 1;
    b[4] = (uint8_t)x, b[5] = (uint8_t)y, b[6] = (uint8_t)w, b[7] = (uint8_t)h, b[8] = RFM_ENC_FILL;
    b[9] = (uint8_t)(c >> 8), b[10] = (uint8_t)c;
    return 11;
}
static uint32_t px(uint32_t x, uint32_t y, const uint16_t *b)          /* RGB565 from the bytes */
{
    const uint8_t *p = (const uint8_t *)(b + y * 240 + x);
    return (uint32_t)p[0] << 8 | p[1];
}

int main(void)
{
    uint8_t b[2048];
    uint32_t n, i;
    int16_t enc[7] = {0};
    rf_body_init(&sx.b, &st[0], sx_buf, sizeof sx_buf, on_msg, (void *)(uintptr_t)RF_T_SX);
    rf_body_init(&bk.b, &st[1], bk_buf, sizeof bk_buf, on_msg, (void *)(uintptr_t)RF_T_BK);
    rfm_app_init(shadow, stage, 240 * 16, &OPS);
    for (i = 0; i < 240 * 240; i++)
        panel[i] = 0x1234;                              /* the waiting screen */
    poll(1);
    rfm_app_panel(0, 0, NO_STEPS, 512);                 /* the MASTER at 512 (first reading: sent) */
    CHECK(nmidi == 2 && midi[0][0] == 0xBE && midi[0][1] == 7 && midi[0][2] == (512u << 4) >> 7 &&
          midi[1][0] == 0xBE && midi[1][1] == 39 && midi[1][2] == ((512u << 4) & 0x7F));

    /* ---- HELLO over SysEx: HELLO_R, 67 bytes */
    nout = 0;
    send_sx(RFM_HELLO, (const uint8_t *)"host", 4);
    CHECK(nout == 1 && out[0].t == RF_T_SX && out[0].cmd == RFM_HELLO_R && out[0].len == RFM_HELLO_LEN);
    {
        uint8_t want[67];
        memset(want, 0, sizeof want);
        memcpy(want, "RemoteFM1", 9);
        want[16] = 2;
        memcpy(want + 17, "0.1", 3);
        for (i = 25; i < 43; i++)
            want[i] = (uint8_t)(0xA0 + i);
        memcpy(want + 43, "FM-1_015", 8);
        want[51] = 240, want[53] = 240;
        want[55] = 0x2F;                                /* shadow, COPY, RLE, PAL, PLAY green; no battery, 3 levels */
        want[57] = 2;
        want[58] = 0;
        memcpy(want + 59, "0.1", 3);
        CHECK(!memcmp(out[0].b, want, 67));
        /* on the wire: F0 7D 52 46 41, 7-in-8 (77 bytes for 67), F7 */
        CHECK(wire_sx_n == 6 + 67 + 10 && wire_sx[0] == 0xF0 && wire_sx[1] == 0x7D && wire_sx[2] == 0x52 &&
              wire_sx[3] == 0x46 && wire_sx[4] == 0x41 && wire_sx[wire_sx_n - 1] == 0xF7);
        CHECK(wire_sx[5] == 0x00 && wire_sx[6] == 'R' && wire_sx[12] == 'F' && wire_sx[13] == 0x00 && wire_sx[14] == 'M');
        for (i = 5; i + 1 < wire_sx_n; i++)
            if (wire_sx[i] & 0x80)
                break;
        CHECK(i + 1 == wire_sx_n);
    }
    /* ---- HELLO over bulk: the answer on bulk */
    nout = 0;
    send_bk(RFM_HELLO, NULL, 0);
    CHECK(nout == 1 && out[0].t == RF_T_BK && out[0].cmd == RFM_HELLO_R && out[0].b[25] == 0xA0 + 25 + 1);

    /* ---- outside a session: LEDS ignored, TILES decoded and acknowledged at once, nothing pushed */
    nout = nleds = 0;
    memset(b, 0xFF, 45);
    send_sx(RFM_LEDS, b, 45);
    CHECK(nleds == 0 && nout == 0);
    n = tiles_fill(b, 7, 1, 0, 0, 16, 16, 0xF800);
    send_sx(RFM_TILES, b, n);
    CHECK(nout == 1);
    check_ack(&out[0], RF_T_SX, 7, 0, n, 0);
    poll(10);
    CHECK(L.windows == 0 && panel[0] == 0x1234);

    /* ---- SESSION start over bulk, keepalive 3000: ACK, PANEL_R, the MASTER; LEDs off; the panel black */
    nout = nmidi = nleds = 0;
    b[0] = 1, b[1] = 3000 & 0xFF, b[2] = 3000 >> 8;
    send_bk(RFM_SESSION, b, 3);
    CHECK(nout == 2 && out[0].t == RF_T_BK && out[0].cmd == RFM_ACK && out[0].len == 0);
    check_panel_r(&out[1], RF_T_BK, 0, 0, enc, 512, 1);
    CHECK(nmidi == 2 && midi[0][1] == 7 && midi[1][1] == 39);
    CHECK(session_on == 1 && nleds == 1);
    for (i = 0; i < 11; i++)
        CHECK(!led_lit[i] && !led_glow[i] && !led_bg[i]);
    CHECK(px(0, 0, shadow) == 0);                       /* the FILL of before is gone: black */
    nout = 0;
    poll(40);
    CHECK(nout == 0);                                   /* no FRAME_ACK for the black screen */
    for (i = 0; i < 240 * 240 && panel[i] == 0; i++)
        ;
    CHECK(i == 240 * 240 && L.bad == 0);
    nout = 0;
    send_sx(RFM_HELLO, NULL, 0);
    CHECK(nout == 1 && out[0].b[58] == 1);              /* HELLO_R: session 1 */

    /* ---- LEDS over SysEx: lit > mid = dim (glow) > faint (backlight); PLAY green */
    for (i = 0; i < 45; i++)
        b[i] = 0;
    b[0] = 0x02;                                        /* lit col 0 row 1 */
    b[11 + 0] = 0x06;                                   /* mid col 0 rows 1, 2: row 1 stays lit */
    b[22 + 1] = 0x10;                                   /* dim col 1 row 4 */
    b[33 + 1] = 0x18;                                   /* faint col 1 rows 3, 4: row 4 is glow */
    b[33 + 10] = 0x1E;
    b[44] = 1;                                          /* PLAY green: col 8 row bit 1 */
    nleds = 0;
    send_sx(RFM_LEDS, b, 45);
    CHECK(nleds == 1);
    CHECK(led_lit[0] == 0x02 && led_glow[0] == 0x04 && led_bg[0] == 0);
    CHECK(led_lit[1] == 0 && led_glow[1] == 0x10 && led_bg[1] == 0x08);
    CHECK(led_lit[8] == 0x02 && led_bg[10] == 0x1E);
    send_sx(RFM_LEDS, b, 44);                           /* short: ignored */
    CHECK(nleds == 1);

    /* ---- frame 1 in two TILES messages over SysEx, END on the second: the first ACK says shadow reset */
    nout = 0;
    n = tiles_fill(b, 1, 0, 16, 16, 32, 16, 0x07E0);
    send_sx(RFM_TILES, b, n);
    poll(20);
    CHECK(nout == 0);                                   /* no END yet: nothing pushed, nothing acked */
    CHECK(panel[16 * 240 + 16] == 0);
    {
        uint32_t n2 = tiles_fill(b, 1, 1, 200, 220, 40, 20, 0x001F);
        send_sx(RFM_TILES, b, n2);
        poll(40);
        CHECK(nout == 1);
        check_ack(&out[0], RF_T_SX, 1, RFM_ACK_SHADOW_RESET, n + n2, 0);
    }
    CHECK(!memcmp(panel, shadow, sizeof panel) && px(16, 16, panel) == 0x07E0 && px(239, 239, panel) == 0x001F);

    /* ---- frames 2 and 3 ENDed back to back (3 while 2's push runs): two ACKs in order, over bulk */
    nout = 0;
    n = tiles_fill(b, 2, 1, 0, 0, 240, 240, 0xFFFF);
    send_bk(RFM_TILES, b, n);
    poll_same_ms(4);                                    /* frame 2's push has begun */
    CHECK(rfm_app_pushing());
    send_bk(RFM_TILES, b + 0, tiles_fill(b, 3, 1, 5, 5, 3, 3, 0x1111));
    poll(200);
    CHECK(nout == 2);
    check_ack(&out[0], RF_T_BK, 2, 0, 11, 1);           /* pending: frame 3 still queued */
    check_ack(&out[1], RF_T_BK, 3, 0, 11, 0);
    CHECK(!memcmp(panel, shadow, sizeof panel));
    /* ---- two ENDs before any poll: one push for both, two ACKs */
    nout = 0;
    send_sx(RFM_TILES, b, tiles_fill(b, 4, 1, 0, 0, 1, 1, 0x2222));
    send_sx(RFM_TILES, b, tiles_fill(b, 5, 1, 1, 0, 1, 1, 0x3333));
    poll(30);
    CHECK(nout == 2);
    check_ack(&out[0], RF_T_SX, 4, 0, 11, 1);
    check_ack(&out[1], RF_T_SX, 5, 0, 11, 0);
    /* ---- a decode error (an unknown enc after a good rect), and an empty END frame */
    nout = 0;
    tiles_fill(b, 6, 1, 0, 0, 2, 2, 0x4444);
    b[3] = 2;
    b[11] = 0, b[12] = 0, b[13] = 1, b[14] = 1, b[15] = 9;
    send_sx(RFM_TILES, b, 16);
    b[0] = 8, b[1] = 0, b[2] = 1, b[3] = 0;
    send_sx(RFM_TILES, b, 4);
    poll(30);
    CHECK(nout == 2);
    check_ack(&out[0], RF_T_SX, 6, RFM_ACK_DECODE_ERR, 16, 1);
    check_ack(&out[1], RF_T_SX, 8, 0, 4, 0);
    CHECK(px(1, 1, shadow) == 0x4444 && !memcmp(panel, shadow, sizeof panel));
    /* a TILES body shorter than its header: counted; the next ACK has the error bit */
    nout = 0;
    send_sx(RFM_TILES, b, 2);
    send_sx(RFM_TILES, b, tiles_fill(b, 9, 1, 0, 0, 1, 1, 0));
    poll(30);
    CHECK(nout == 1);
    check_ack(&out[0], RF_T_SX, 9, RFM_ACK_DECODE_ERR, 2 + 11, 0);

    /* ---- the panel as plain MIDI */
    nmidi = 0;
    {
        int32_t steps[7] = {0, 0, 0, 2, 0, 0, -70};
        rfm_app_panel(1u << 0 | 1u << 26, 1u << 12, steps, -1);
        enc[3] = 2, enc[6] = -70;
    }
    CHECK(nmidi == 6);
    CHECK(midi[0][0] == 0x9F && midi[0][1] == 53 && midi[0][2] == 100);
    CHECK(midi[1][0] == 0x9F && midi[1][1] == 79 && midi[1][2] == 100);
    CHECK(midi[2][0] == 0x9E && midi[2][1] == 12 && midi[2][2] == 127);
    CHECK(midi[3][0] == 0xBE && midi[3][1] == 19 && midi[3][2] == 66);
    CHECK(midi[4][0] == 0xBE && midi[4][1] == 22 && midi[4][2] == 1);       /* -63 */
    CHECK(midi[5][0] == 0xBE && midi[5][1] == 22 && midi[5][2] == 57);      /* then -7 */
    nmidi = 0;
    poll(1);
    rfm_app_panel(1u << 26, 0, NO_STEPS, 513);          /* key 0 off, button off; MASTER +1: inside the deadband */
    CHECK(nmidi == 2 && midi[0][0] == 0x8F && midi[0][1] == 53 && midi[0][2] == 0 && midi[1][0] == 0x8E &&
          midi[1][1] == 12 && midi[1][2] == 0);
    nmidi = 0;
    rfm_app_panel(1u << 26, 0, NO_STEPS, 520);          /* +8: sent (5 ms since the last) */
    CHECK(nmidi == 2 && midi[0][2] == (520u << 4) >> 7 && midi[1][2] == ((520u << 4) & 0x7F));
    nmidi = 0;
    rfm_app_panel(1u << 26, 0, NO_STEPS, 530);          /* +10 in the same ms: held back (5 ms) */
    CHECK(nmidi == 0);
    poll(5);
    rfm_app_panel(1u << 26, 0, NO_STEPS, 530);
    CHECK(nmidi == 2);
    /* ---- PANEL_REQ on bulk, and the 500 ms PANEL_R on the session's transport (bulk) */
    nout = 0;
    send_bk(RFM_PANEL_REQ, NULL, 0);
    CHECK(nout == 1);
    check_panel_r(&out[0], RF_T_BK, 1u << 26, 0, enc, 530, 1);
    nout = 0;
    for (i = 0; i < 1100; i++) {
        if (i % 400 == 0)
            send_sx(RFM_ECHO, (const uint8_t *)"k", 1); /* traffic: the keepalive holds */
        poll(1);
    }
    {
        uint32_t k, panels = 0, echos = 0;
        for (k = 0; k < nout && k < 64; k++) {
            if (out[k].cmd == RFM_PANEL_R) {
                panels++;
                CHECK(out[k].t == RF_T_BK);
            }
            if (out[k].cmd == RFM_ECHO_R) {
                echos++;
                CHECK(out[k].t == RF_T_SX && out[k].len == 1 && out[k].b[0] == 'k');
            }
        }
        CHECK(panels == 2 && echos == 3);
    }
    /* ---- diagnostics go to the includer */
    ndiag = 0;
    send_sx(RF_STATS, NULL, 0);
    send_bk(0x33, NULL, 0);
    CHECK(ndiag == 2 && diag_cmd == 0x33);

    /* ---- the keepalive: 3000 ms without a message ends the session */
    poll(2990);
    CHECK(session_on == 1);
    poll(20);
    CHECK(session_on == 0);
    nout = 0;
    poll(1000);
    CHECK(nout == 0);                                   /* no PANEL_R outside a session */
    send_sx(RFM_HELLO, NULL, 0);
    CHECK(nout == 1 && out[0].b[58] == 0);
    nout = 0;
    send_sx(RFM_PANEL_REQ, NULL, 0);
    check_panel_r(&out[0], RF_T_SX, 1u << 26, 0, enc, 530, 0);

    /* ---- SESSION over SysEx without a keepalive (0: never), then SESSION 0 */
    nout = 0;
    b[0] = 1, b[1] = 0, b[2] = 0;
    send_sx(RFM_SESSION, b, 3);
    CHECK(session_on == 1 && nout == 2 && out[0].cmd == RFM_ACK && out[0].t == RF_T_SX);
    poll(10000);
    CHECK(session_on == 1);
    nout = 0;
    send_sx(RFM_TILES, b, tiles_fill(b, 1, 1, 0, 0, 8, 8, 0x5555));
    poll(40);
    CHECK(nout >= 1 && out[0].cmd == RFM_FRAME_ACK && (out[0].b[2] & RFM_ACK_SHADOW_RESET));
    nout = 0;
    b[0] = 0;
    send_sx(RFM_SESSION, b, 3);
    CHECK(session_on == 0 && nout == 1 && out[0].cmd == RFM_ACK && out[0].len == 0);
    /* ---- a USB loss ends a session; an END queued then is never acked */
    b[0] = 1;
    send_bk(RFM_SESSION, b, 1);                         /* (start8 alone: keepalive 0) */
    CHECK(session_on == 1);
    nout = 0;
    send_bk(RFM_TILES, b, tiles_fill(b, 2, 1, 0, 0, 240, 240, 0x6666));
    rfm_app_usb_lost();
    poll(100);
    CHECK(session_on == 0 && nout == 0);

    /* ---- M0's raw TILE outside a session: into the shadow and onto the panel, no ACK */
    {
        uint8_t t[4 + 2 * 4 * 2];
        t[0] = 10, t[1] = 20, t[2] = 4, t[3] = 2;
        for (i = 0; i < 8; i++)
            t[4 + 2 * i] = 0xAB, t[5 + 2 * i] = (uint8_t)i;
        CHECK(rfm_app_tile_m0(t, sizeof t));
        CHECK(!rfm_app_tile_m0(t, sizeof t - 1));
        nout = 0;
        poll(30);
        CHECK(nout == 0 && px(13, 21, panel) == 0xAB07 && px(10, 20, panel) == 0xAB00);
    }
    CHECK(L.bad == 0);
    printf("session_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
