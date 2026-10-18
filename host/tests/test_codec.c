/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The codec: both framings (golden vector, random bodies in random chunks among other MIDI traffic, resync), every
 * v2 body against the offsets of proto/rfm1.h, HELLO_R acceptance, the body size limits. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "frames.h"
#include "rfm/codec.h"

typedef struct {
    uint8_t cmd[64];
    uint32_t len[64];
    uint8_t body[64][RFM_BK_BODY_MAX];
    uint32_t n;
    uint8_t midi[256][3];
    uint32_t midi_len[256], nmidi;
} sink_t;

static void on_msg(void *ctx, uint8_t cmd, const uint8_t *b, uint32_t n)
{
    sink_t *s = (sink_t *)ctx;
    if (s->n < 64) {
        s->cmd[s->n] = cmd;
        s->len[s->n] = n;
        memcpy(s->body[s->n], b, n);
        s->n++;
    }
}
static void on_midi(void *ctx, const uint8_t *m, uint32_t n)
{
    sink_t *s = (sink_t *)ctx;
    if (s->nmidi < 256) {
        memcpy(s->midi[s->nmidi], m, n);
        s->midi_len[s->nmidi++] = n;
    }
}

static void feed_chunks(rfm_rx_t *r, const uint8_t *p, uint32_t n, uint32_t *seed)
{
    while (n) {
        uint32_t c = fr_rand(seed) % 70u + 1u;
        if (c > n)
            c = n;
        rfm_rx_feed(r, p, c);
        p += c;
        n -= c;
    }
}

static void test_golden(void)
{
    static const uint8_t body[8] = {0x00, 0x7F, 0x80, 0xFF, 1, 2, 3, 0xAA};
    static const uint8_t want[] = {0xF0, 0x7D, 0x52, 0x46, 0x04, 0x0C, 0x00, 0x7F, 0x00,
                                   0x7F, 0x01, 0x02, 0x03, 0x01, 0x2A, 0xF7};     /* tests/proto_test.c */
    uint8_t out[32], bk[32];
    CHECK(rfm_sx_frame(RFM_ECHO, body, 8, out) == sizeof want);
    CHECK(!memcmp(out, want, sizeof want));
    CHECK(rfm_sx_size(8) == sizeof want);
    CHECK(rfm_bk_frame(RFM_ECHO, body, 8, 0x01020304u, bk) == 18u);
    CHECK(bk[0] == 'R' && bk[1] == 'F' && bk[2] == RFM_ECHO && bk[3] == 0 && bk[4] == 8 && bk[5] == 0);
    CHECK(bk[6] == 4 && bk[7] == 3 && bk[8] == 2 && bk[9] == 1 && !memcmp(bk + 10, body, 8));
}

/* random messages over SysEx, between channel messages (with running status), realtime bytes and foreign SysEx */
static void test_sx_stream(void)
{
    static sink_t s;
    static uint8_t stream[200000], bodies[20][RFM_SX_BODY_MAX];
    static uint32_t blen[20];
    uint32_t seed = 42, o = 0, i, k;
    rfm_rx_t *r;
    rfm_rx_stats_t st;
    memset(&s, 0, sizeof s);
    r = rfm_rx_new(RFM_LINK_SX, RFM_SX_BODY_MAX, on_msg, on_midi, &s);
    for (i = 0; i < 20; i++) {
        blen[i] = i == 0 ? 0u : i == 1 ? RFM_SX_BODY_MAX : fr_rand(&seed) % RFM_SX_BODY_MAX;
        for (k = 0; k < blen[i]; k++)
            bodies[i][k] = (uint8_t)fr_rand(&seed);
        stream[o++] = 0x9F;                             /* a key, then a running-status key off */
        stream[o++] = (uint8_t)(53 + i);
        stream[o++] = 100;
        stream[o++] = (uint8_t)(53 + i);
        stream[o++] = 0;
        stream[o++] = 0xF8;                             /* realtime */
        stream[o++] = 0xF0;                             /* foreign SysEx: ignored */
        stream[o++] = 0x43;
        stream[o++] = 0x10;
        stream[o++] = 0xF7;
        o += rfm_sx_frame((uint8_t)(0x40 + i), bodies[i], blen[i], stream + o);
        if (i == 5) {                                   /* a realtime byte inside one of ours */
            memmove(stream + o - 40, stream + o - 41, 41);
            stream[o - 41] = 0xFE;
            o++;
        }
    }
    feed_chunks(r, stream, o, &seed);
    CHECK(s.n == 20);
    for (i = 0; i < 20 && i < s.n; i++) {
        CHECKF(s.cmd[i] == 0x40 + i && s.len[i] == blen[i] && !memcmp(s.body[i], bodies[i], blen[i]),
               "message %u: cmd %02x len %u (want %u)", i, s.cmd[i], s.len[i], blen[i]);
    }
    CHECK(s.nmidi == 40);
    for (i = 0; i < 20 && 2 * i + 1 < s.nmidi; i++) {
        CHECK(s.midi_len[2 * i] == 3 && s.midi[2 * i][0] == 0x9F && s.midi[2 * i][1] == 53 + i &&
              s.midi[2 * i][2] == 100);
        CHECK(s.midi[2 * i + 1][0] == 0x9F && s.midi[2 * i + 1][2] == 0);  /* running status */
    }
    rfm_rx_stats(r, &st);
    CHECK(st.bad == 0);
    /* one of ours cut off by a status byte: counted bad, not delivered; the next one arrives */
    s.n = 0;
    {
        uint8_t m[64];
        uint32_t n = rfm_sx_frame(0x41, bodies[3], 20, m);
        rfm_rx_feed(r, m, 10);
        rfm_rx_feed(r, (const uint8_t *)"\x90\x40\x40", 3);
        rfm_rx_feed(r, m, n);
    }
    rfm_rx_stats(r, &st);
    CHECK(st.bad == 1 && s.n == 1 && s.len[0] == 20);
    /* a body over the cap: counted bad, dropped */
    s.n = 0;
    {
        static uint8_t big[RFM_SX_BODY_MAX + 1], m[1300];
        uint32_t n = rfm_sx_frame(0x42, big, sizeof big, m);
        rfm_rx_feed(r, m, n);
        rfm_rx_stats(r, &st);
        CHECK(st.bad == 2 && s.n == 0);
    }
    rfm_rx_free(r);
}

static void test_bk_stream(void)
{
    static sink_t s;
    static uint8_t stream[600000], bodies[12][RFM_BK_BODY_MAX];
    static uint32_t blen[12];
    uint32_t seed = 7, o = 0, i, k;
    rfm_rx_t *r;
    rfm_rx_stats_t st;
    memset(&s, 0, sizeof s);
    r = rfm_rx_new(RFM_LINK_BK, RFM_BK_BODY_MAX, on_msg, NULL, &s);
    memcpy(stream, "junkRjunkRR", 11);                  /* leftovers of an earlier run: resynced */
    o = 11;
    for (i = 0; i < 12; i++) {
        blen[i] = i == 0 ? 0u : i == 1 ? RFM_BK_BODY_MAX : fr_rand(&seed) % 3000u;
        for (k = 0; k < blen[i]; k++)
            bodies[i][k] = (uint8_t)fr_rand(&seed);
        o += rfm_bk_frame((uint8_t)(0x50 + i), bodies[i], blen[i], 100u + i, stream + o);
    }
    feed_chunks(r, stream, o, &seed);
    CHECK(s.n == 12);
    for (i = 0; i < 12 && i < s.n; i++)
        CHECK(s.cmd[i] == 0x50 + i && s.len[i] == blen[i] && !memcmp(s.body[i], bodies[i], blen[i]));
    rfm_rx_stats(r, &st);
    CHECK(st.sync > 0 && st.bad == 0 && st.frame_gaps == 0);
    rfm_rx_free(r);
}

static void test_bodies(void)
{
    uint8_t b[128];
    rfm_hello_t h, g;
    uint32_t n, e;
    /* HELLO */
    CHECK(rfm_build_hello(b, "fm1remote") == 9 && !memcmp(b, "fm1remote", 9));
    CHECK(rfm_build_hello(b, "a-very-long-host-name-indeed") == 16);
    CHECK(rfm_build_hello(b, NULL) == 0);
    memset(&h, 0, sizeof h);
    strcpy(h.name, "RemoteFM1");
    h.proto = 2;
    strcpy(h.ver, "0.1");
    h.sx_body_max = 1024;
    h.bk_body_max = 16384;
    strcpy(h.identity, "FM-1_015");
    h.screen_w = h.screen_h = 240;
    h.caps = RFM_CAP_SHADOW | RFM_CAP_PAL;
    h.max_in_flight = 2;
    h.session = 1;
    strcpy(h.fw_ver, "m1");
    n = rfm_build_hello_r(b, &h);
    CHECK(n == RFM_HELLO_LEN);
    CHECK(!memcmp(b, "RemoteFM1\0\0\0\0\0\0\0", 16) && b[16] == 2 && b[39] == 0 && b[40] == 4);
    CHECK(b[RFM_HELLO_SCREEN_W] == 240 && b[RFM_HELLO_SCREEN_H] == 240 && b[RFM_HELLO_CAPS] == 9);
    CHECK(b[RFM_HELLO_INFLIGHT] == 2 && b[RFM_HELLO_SESSION] == 1 && b[RFM_HELLO_FWVER] == 'm');
    CHECK(!rfm_parse_hello(b, n, &g) && g.v2 && rfm_hello_is_remotefm1(&g));
    CHECK(g.sx_body_max == 1024 && g.bk_body_max == 16384 && !strcmp(g.identity, "FM-1_015") && g.caps == 9);
    CHECK(!rfm_parse_hello(b, 51, &g) && !g.v2 && !rfm_hello_is_remotefm1(&g));   /* M0's 51 bytes only */
    CHECK(rfm_parse_hello(b, 50, &g) != 0);
    b[16] = 1;
    CHECK(!rfm_parse_hello(b, n, &g) && !rfm_hello_is_remotefm1(&g));             /* proto 1 */
    b[16] = 2;
    memcpy(b, "RemoteFM1-M0", 12);
    CHECK(!rfm_parse_hello(b, n, &g) && !rfm_hello_is_remotefm1(&g));             /* M0's name */
    /* SESSION */
    {
        int start;
        uint32_t ka;
        CHECK(rfm_build_session(b, 1, 3000) == 3 && b[0] == 1 && b[1] == 0xB8 && b[2] == 0x0B);
        CHECK(!rfm_parse_session(b, 3, &start, &ka) && start == 1 && ka == 3000);
        CHECK(rfm_parse_session(b, 2, &start, &ka) != 0);
    }
    /* LEDS */
    {
        rfm_leds_t l, m;
        for (e = 0; e < RFM_NCOL; e++) {
            l.lit[e] = (uint8_t)e;
            l.mid[e] = (uint8_t)(0x10 + e);
            l.dim[e] = (uint8_t)(0x20 + e);
            l.faint[e] = (uint8_t)(0x30 + e);
        }
        l.flags = RFM_LEDS_F_PLAY_GREEN;
        CHECK(rfm_build_leds(b, &l) == RFM_LEDS_LEN);
        CHECK(b[RFM_LEDS_LIT + 3] == 3 && b[RFM_LEDS_MID + 3] == 0x13 && b[RFM_LEDS_DIM + 10] == 0x2A);
        CHECK(b[RFM_LEDS_FAINT] == 0x30 && b[RFM_LEDS_FLAGS] == 1);
        CHECK(!rfm_parse_leds(b, RFM_LEDS_LEN, &m) && rfm_leds_equal(&l, &m));
        m.faint[10] ^= 1;
        CHECK(!rfm_leds_equal(&l, &m));
    }
    /* FRAME_ACK */
    {
        rfm_frame_ack_t a = {0xBEEF, 3, 123456, 0x01020304, 1}, c;
        CHECK(rfm_build_frame_ack(b, &a) == RFM_ACK_LEN);
        CHECK(b[0] == 0xEF && b[1] == 0xBE && b[2] == 3 && b[7] == 4 && b[10] == 1 && b[11] == 1);
        CHECK(!rfm_parse_frame_ack(b, RFM_ACK_LEN, &c) && c.frame == 0xBEEF && c.status == 3 &&
              c.draw_us == 123456 && c.bytes == 0x01020304 && c.pending == 1);
        CHECK(rfm_parse_frame_ack(b, RFM_ACK_LEN - 1, &c) != 0);
    }
    /* PANEL_R */
    {
        rfm_panel_r_t p, q;
        p.keys = 0x07654321;
        p.buttons = 0x2001;
        for (e = 0; e < RFM_NENC; e++)
            p.enc[e] = (int16_t)(e * 1000 - 3000);
        p.master = 1023;
        p.battery_mv = 3901;
        p.flags = 1;
        CHECK(rfm_build_panel_r(b, &p) == RFM_PANEL_LEN);
        CHECK(b[RFM_PANEL_KEYS] == 0x21 && b[RFM_PANEL_BUTTONS] == 0x01 && b[RFM_PANEL_BUTTONS + 1] == 0x20);
        CHECK(b[RFM_PANEL_ENC] == 0x48 && b[RFM_PANEL_ENC + 1] == 0xF4);            /* -3000 LE */
        CHECK(b[RFM_PANEL_MASTER] == 0xFF && b[RFM_PANEL_MASTER + 1] == 3 && b[RFM_PANEL_FLAGS] == 1);
        CHECK(!rfm_parse_panel_r(b, RFM_PANEL_LEN, &q) && !memcmp(&p.enc, &q.enc, sizeof p.enc) &&
              q.keys == p.keys && q.buttons == p.buttons && q.master == 1023 && q.battery_mv == 3901 &&
              q.flags == 1);
        CHECK(rfm_parse_panel_r(b, RFM_PANEL_LEN - 1, &q) != 0);
    }
    /* TILES header */
    rfm_tiles_header(b, 0x1234, RFM_TILES_F_END, 7);
    CHECK(b[0] == 0x34 && b[1] == 0x12 && b[2] == 1 && b[3] == 7);
}

int main(void)
{
    test_golden();
    test_sx_stream();
    test_bk_stream();
    test_bodies();
    return check_done("test_codec");
}
