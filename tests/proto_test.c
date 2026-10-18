/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Host test of firmware/src/remote_proto.h, the M0 bench protocol (the device parses with this very code):
 * 7-in-8 pack / unpack round trips (every length 0..300 and some long ones, all byte values), the SysEx parser fed
 * byte by byte with realtime bytes interleaved, foreign SysEx, truncated and cut-off messages, the bulk parser with
 * frames split at every offset and a resync, and the SINK counters with an injected sequence gap and a bad checksum.
 * The golden vector is m0_bench.py's encoding of the same message (tools/m0_bench.py sx_encode).
 *   cc -std=c99 -Wall -Ifirmware/src -o proto_test tests/proto_test.c && ./proto_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remote_proto.h"

static int fails, checks;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            fails++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                        \
    } while (0)

/* what the parsers delivered */
static struct {
    uint32_t n, cmd[64], len[64];
    uint8_t body[64][2048];
} got;
static void on_msg(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    (void)ctx;
    if (got.n < 64) {
        got.cmd[got.n] = cmd;
        got.len[got.n] = len;
        memcpy(got.body[got.n], body, len < 2048 ? len : 2048);
    }
    got.n++;
}

static uint32_t sink_body(uint8_t *b, uint32_t seq, uint32_t n, int bad_sum)
{
    uint32_t i;
    rf_wr32(b, seq);
    for (i = 0; i < n; i++)
        b[4 + i] = (uint8_t)(seq * 7u + i);
    rf_wr32(b + 4 + n, rf_sum(b + 4, n) + (bad_sum ? 1u : 0u));
    return n + 8u;
}

static void test_pack(void)
{
    static uint8_t in[70000], pk[90000], out[70000];
    uint32_t n, i, lens[] = {1000, 4095, 8192, 65535};
    for (i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)(i * 37u + (i >> 8));
    for (n = 0; n <= 300; n++) {
        uint32_t p = rf_pack7(in, n, pk), u, ok = 1;
        CHECK(p == rf_packed_len(n));
        for (i = 0; i < p; i++)
            ok &= pk[i] < 0x80u;
        CHECK(ok);
        u = rf_unpack7(pk, p, out);
        CHECK(u == n && !memcmp(in, out, n));
    }
    for (i = 0; i < 4; i++) {
        uint32_t p = rf_pack7(in, lens[i], pk);
        CHECK(rf_unpack7(pk, p, out) == lens[i] && !memcmp(in, out, lens[i]));
    }
    for (i = 0; i < 256; i++)                           /* every byte value at every place in a group */
        in[i] = (uint8_t)i;
    n = rf_pack7(in, 256, pk);
    CHECK(rf_unpack7(pk, n, out) == 256 && !memcmp(in, out, 256));
}

static void test_golden(void)
{
    /* m0_bench.py: sx_encode(RF_ECHO, bytes([0x00, 0x7F, 0x80, 0xFF, 1, 2, 3, 0xAA])) */
    static const uint8_t WANT[] = {0xF0, 0x7D, 0x52, 0x46, 0x04, 0x0C, 0x00, 0x7F, 0x00, 0x7F, 0x01, 0x02, 0x03,
                                   0x01, 0x2A, 0xF7};
    static const uint8_t BODY[] = {0x00, 0x7F, 0x80, 0xFF, 1, 2, 3, 0xAA};
    uint8_t m[64];
    uint8_t ff[100];
    uint32_t n = rf_sx_encode(RF_ECHO, BODY, sizeof BODY, m);
    CHECK(n == sizeof WANT && !memcmp(m, WANT, n));
    memset(ff, 0xFF, sizeof ff);
    CHECK(rf_sum(ff, sizeof ff) == 3801u);              /* m0_bench.py rf_sum, m0_bench.html rfSum: the same */
}

static void feed_sx(rf_sx_t *s, const uint8_t *p, uint32_t n, int realtime)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (realtime && i % 3u == 1u)
            rf_sx_byte(s, 0xF8);                        /* clock between any two bytes */
        if (realtime && i % 7u == 5u)
            rf_sx_byte(s, 0xFE);
        rf_sx_byte(s, p[i]);
    }
}

static void test_sysex(void)
{
    static uint8_t buf[RF_SX_BODY_MAX], m[20000], body[10000];
    rf_tstats_t st;
    rf_sx_t s;
    uint32_t n, k, seq;
    static const uint8_t FOREIGN[] = {0xF0, 0x00, 0x59, 0x11, 0x22, 0xF7};             /* not ours: no count */
    static const uint8_t UPGRADE[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    memset(&got, 0, sizeof got);
    rf_body_init(&s.b, &st, buf, sizeof buf, on_msg, NULL);

    /* ECHO, every length 0..600 bytes (ours collected up to RF_SX_BODY_MAX), realtime interleaved */
    for (k = 0; k <= 600; k += 7) {
        uint32_t i;
        for (i = 0; i < k; i++)
            body[i] = (uint8_t)(i ^ 0xA5u ^ k);
        n = rf_sx_encode(RF_ECHO, body, k, m);
        got.n = 0;
        feed_sx(&s, m, n, 1);
        CHECK(got.n == 1 && got.cmd[0] == RF_ECHO && got.len[0] == k && !memcmp(got.body[0], body, k));
    }
    CHECK(st.rx_msgs == 86 && st.bad == 0);
    got.n = 0;
    feed_sx(&s, FOREIGN, sizeof FOREIGN, 1);
    feed_sx(&s, UPGRADE, sizeof UPGRADE, 0);
    CHECK(got.n == 0 && st.bad == 0);

    /* too long to collect: counted bad, not delivered */
    for (k = 0; k < RF_SX_BODY_MAX + 10; k++)
        body[k] = (uint8_t)k;
    n = rf_sx_encode(RF_ECHO, body, RF_SX_BODY_MAX + 10, m);
    feed_sx(&s, m, n, 0);
    CHECK(got.n == 0 && st.bad == 1);

    /* truncated: a message cut by a new F0 (the first is counted bad, the second arrives) */
    n = rf_sx_encode(RF_HELLO, NULL, 0, m);
    feed_sx(&s, (const uint8_t *)"\xF0\x7D\x52\x46\x04\x01\x02\x03", 8, 0);
    feed_sx(&s, m, n, 1);
    CHECK(got.n == 1 && got.cmd[0] == RF_HELLO && got.len[0] == 0 && st.bad == 2);
    /* ... and by a channel status byte */
    feed_sx(&s, (const uint8_t *)"\xF0\x7D\x52\x46\x04\x01\x02\x90", 8, 0);
    CHECK(got.n == 1 && st.bad == 3);
    /* an F7 right after the ID: no command */
    feed_sx(&s, (const uint8_t *)"\xF0\x7D\x52\x46\xF7", 5, 0);
    CHECK(got.n == 1 && st.bad == 4);

    /* SINK: counted, never delivered; seq 0..9, then 11 (a gap of one), a bad checksum, sizes 0..8192 */
    memset(&st, 0, sizeof st);
    got.n = 0;
    for (seq = 0; seq < 10; seq++) {
        n = rf_sx_encode(RF_SINK, body, sink_body(body, seq, seq * 100u, 0), m);
        feed_sx(&s, m, n, 1);
    }
    CHECK(st.sink_msgs == 10 && st.gaps == 0 && st.chk == 0 && st.sink_bytes == 4500 && got.n == 0);
    n = rf_sx_encode(RF_SINK, body, sink_body(body, 11, 8192, 0), m);
    feed_sx(&s, m, n, 0);
    CHECK(st.sink_msgs == 11 && st.gaps == 1 && st.lost == 1 && st.chk == 0 && st.sink_bytes == 4500 + 8192);
    n = rf_sx_encode(RF_SINK, body, sink_body(body, 12, 33, 1), m);
    feed_sx(&s, m, n, 1);
    CHECK(st.sink_msgs == 12 && st.gaps == 1 && st.chk == 1);
    n = rf_sx_encode(RF_SINK, body, sink_body(body, 13, 0, 0), m);   /* no data at all */
    feed_sx(&s, m, n, 0);
    CHECK(st.sink_msgs == 13 && st.gaps == 1 && st.chk == 1 && st.bad == 0);
    n = rf_sx_encode(RF_SINK, body, 5, m);                           /* shorter than seq + sum */
    feed_sx(&s, m, n, 0);
    CHECK(st.sink_msgs == 13 && st.bad == 1);
    rf_body_reset_seq(&s.b);                                         /* STATS_RESET: any seq starts a run */
    n = rf_sx_encode(RF_SINK, body, sink_body(body, 500, 10, 0), m);
    feed_sx(&s, m, n, 0);
    CHECK(st.gaps == 1);
    n = rf_sx_encode(RF_SINK, body, sink_body(body, 499, 10, 0), m); /* backwards: a gap, nothing lost */
    feed_sx(&s, m, n, 0);
    CHECK(st.gaps == 2 && st.lost == 1);
}

static void test_bulk(void)
{
    static uint8_t buf[RF_BK_BODY_MAX], stream[200000], body[70000];
    rf_tstats_t st;
    rf_bk_t k;
    uint32_t n = 0, i, seq = 0, cut;
    memset(&st, 0, sizeof st);
    memset(&k, 0, sizeof k);
    memset(&got, 0, sizeof got);
    rf_body_init(&k.b, &st, buf, sizeof buf, on_msg, NULL);
    /* a stream: HELLO, ECHO(300), SINK x 3 (the second 65535-byte body), STATS, TILE 16 x 16 */
#define FRAME(cmd, b, len)                          \
    do {                                            \
        rf_bk_header(stream + n, cmd, len, seq++);  \
        memcpy(stream + n + RF_BK_HDR, b, len);     \
        n += RF_BK_HDR + (len);                     \
    } while (0)
    FRAME(RF_HELLO, body, 0);
    for (i = 0; i < 300; i++)
        body[i] = (uint8_t)(i * 3u);
    FRAME(RF_ECHO, body, 300);
    FRAME(RF_SINK, body, sink_body(body, 0, 100, 0));
    FRAME(RF_SINK, body, sink_body(body, 1, 65535 - 8, 0));
    FRAME(RF_SINK, body, sink_body(body, 2, 46, 0));
    FRAME(RF_STATS, body, 0);
    body[0] = 16; body[1] = 32; body[2] = 16; body[3] = 16;
    for (i = 0; i < 512; i++)
        body[4 + i] = (uint8_t)i;
    FRAME(RF_TILE, body, 516);
    /* fed whole, then split at many offsets (frames span chunks) */
    for (cut = 0; cut < 40; cut++) {
        uint32_t p = 0, step = cut ? cut * 37u + 1u : n;
        memset(&st, 0, sizeof st);
        got.n = 0;
        k.have_fseq = 0;
        rf_body_reset_seq(&k.b);
        while (p < n) {
            uint32_t m = n - p < step ? n - p : step;
            rf_bk_feed(&k, stream + p, m);
            p += m;
        }
        CHECK(got.n == 4 && got.cmd[0] == RF_HELLO && got.cmd[1] == RF_ECHO && got.len[1] == 300 &&
              got.body[1][299] == (uint8_t)(299 * 3) && got.cmd[2] == RF_STATS && got.cmd[3] == RF_TILE &&
              got.len[3] == 516 && got.body[3][4 + 511] == 0xFF);
        CHECK(st.sink_msgs == 3 && st.sink_bytes == 100 + 65527 + 46 && st.gaps == 0 && st.chk == 0 &&
              st.sync == 0 && st.frame_gaps == 0 && st.bad == 0 && st.rx_msgs == 7);
    }
    /* garbage, then 'R' 'R' 'F' ... : resync; a frame seq jump; a SINK gap */
    memset(&st, 0, sizeof st);
    got.n = 0;
    rf_bk_feed(&k, (const uint8_t *)"xyzR", 4);
    n = 0;
    seq = 100;
    FRAME(RF_ECHO, body, 3);
    rf_bk_feed(&k, stream, n);
    CHECK(got.n == 1 && got.cmd[0] == RF_ECHO && st.sync == 4 && st.frame_gaps == 1);
    n = 0;
    seq = 102;                                           /* 101 lost */
    FRAME(RF_SINK, body, sink_body(body, 3, 10, 0));
    FRAME(RF_SINK, body, sink_body(body, 5, 10, 0));     /* SINK seq 4 lost */
    FRAME(RF_SINK, body, sink_body(body, 6, 10, 1));     /* bad sum */
    rf_bk_feed(&k, stream, n);
    CHECK(st.frame_gaps == 2 && st.sink_msgs == 3 && st.gaps == 1 && st.lost == 1 && st.chk == 1);
    /* a collected body longer than the buffer: bad, nothing delivered, the stream goes on */
    n = 0;
    FRAME(RF_ECHO, body, RF_BK_BODY_MAX + 1);
    FRAME(RF_HELLO, body, 0);
    got.n = 0;
    rf_bk_feed(&k, stream, n);
    CHECK(st.bad == 1 && got.n == 1 && got.cmd[0] == RF_HELLO);
}

int main(void)
{
    test_pack();
    test_golden();
    test_sysex();
    test_bulk();
    printf("proto_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
