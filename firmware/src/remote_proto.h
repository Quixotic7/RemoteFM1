/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 M0 bench protocol: one set of messages, carried two ways. Shared by the firmware (rfm_main.c), the host
 * test (tests/proto_test.c) and the libusb tool (tools/m0_bulk.c); tools/m0_bench.py and web/m0_bench.html mirror
 * it (keep them in step).
 *
 * A message is a command byte and a body (8-bit bytes). Multi-byte fields are little endian.
 *   SysEx (USB-MIDI, EP1):  F0 7D 52 46 <cmd> <body, 7-in-8 packed> F7
 *                           7D = non-commercial ID, 52 46 = "RF". 7-in-8: each group of up to 7 body bytes goes as
 *                           one byte holding their top bits (bit k = top bit of byte k) and then the bytes & 0x7F.
 *   bulk (vendor IF, EP3):  'R' 'F' <cmd> 00 <len16> <seq32> <body, raw>   (a 10-byte header, then len bytes)
 *                           seq counts the frames each side sends (a transport counter, checked as frame_gaps).
 * Commands (host -> device) and replies (cmd | 0x40, device -> host):
 *   HELLO       ()                         -> HELLO_R   (rf_hello_t, below); also stops a SOURCE
 *   SINK        seq32 data[] sum32         -> nothing: counted (bytes, messages, sequence gaps, checksum errors)
 *   SOURCE      n_bytes32 msg_len16        -> DATA (seq32 data[msg_len] sum32) until n_bytes, then SOURCE_DONE
 *   ECHO        data[]                     -> ECHO_R    the same bytes
 *   STATS       ()                         -> STATS_R   n8, n x u32 (RF_ST_* order)
 *   STATS_RESET ()                         -> ACK
 *   BLIT_BENCH  baud8[] (none: 4, 2)       -> BLIT_R    n8, n x (baud8 full_us32 tiles_us32 strips_us32)
 *   TILE        x8 y8 w8 h8 px[w*h*2]      -> nothing: drawn at once (px RGB565 big endian, as the panel takes it;
 *                                             M1: into the shadow, then pushed)
 *   SA_BENCH    (01: start)                -> SA_BENCH_R  the standalone DX7's bench (firmware/sa/sa_bench.c: its
 *                                             layout; firmware/tools/sa_bench.py reads it)
 *   SA_LOOP     op8 ...                    -> SA_LOOP_R   the standalone's loops (firmware/sa/sa_loop.c; docs/PROTOCOL.md
 *                                             section 8; tools/sa_loops.py)
 *   SA_INFO     op8 ...                    -> SA_INFO_R   0.3.1: the user DX7 banks kept (max, names), a name
 *                                             (firmware/sa/sa_ubank.c; docs/PROTOCOL.md section 8)
 * Protocol 2 (M1) keeps all of these as diagnostics and adds the session, LED, screen and panel messages:
 * docs/PROTOCOL.md, proto/rfm1.h.
 * DATA's data[i] = (uint8_t)(seq * 7 + i); sum32 = rf_sum over data[] only. */
#ifndef REMOTE_PROTO_H
#define REMOTE_PROTO_H
#include <stdint.h>

#define RF_FN static __attribute__((unused))     /* (not every includer uses every helper) */
#define RF_NAME "RemoteFM1"          /* M1 (proto 2, docs/PROTOCOL.md, proto/rfm1.h); M0 answered "RemoteFM1-M0", 1 */
#define RF_PROTO 2
#define RF_SX_ID0 0x7Du
#define RF_SX_ID1 0x52u
#define RF_SX_ID2 0x46u
#define RF_BK_HDR 10u
#define RF_VID 0x1209u
#define RF_PID 0x0001u
#define RF_SX_BODY_MAX 1024u        /* collected (non-SINK) body over SysEx: TILE 16 x 16, ECHO */
#define RF_BK_BODY_MAX 16384u       /* over bulk (M0: 8192; proto 2: RFM_BK_BODY_MAX) */
#define RF_SX_SRC_MAX 8192u         /* SOURCE msg_len limits */
#define RF_BK_SRC_MAX 16384u

enum {
    RF_HELLO = 0x01, RF_SINK = 0x02, RF_SOURCE = 0x03, RF_ECHO = 0x04, RF_STATS = 0x05, RF_STATS_RESET = 0x06,
    RF_BLIT_BENCH = 0x07, RF_TILE = 0x08, RF_SA_BENCH = 0x09, RF_SA_LOOP = 0x0A, RF_SA_INFO = 0x0B,
    RF_R = 0x40,                    /* reply bit */
    RF_HELLO_R = 0x41, RF_DATA = 0x43, RF_ECHO_R = 0x44, RF_STATS_R = 0x45, RF_ACK = 0x46, RF_BLIT_R = 0x47,
    RF_SA_BENCH_R = 0x49, RF_SA_LOOP_R = 0x4A, RF_SA_INFO_R = 0x4B,
    RF_SOURCE_DONE = 0x50           /* bytes32 msgs32 dev_us32 aborted8 */
};
enum { RF_T_SX = 0, RF_T_BK = 1 };

/* HELLO_R body, M0's 51 bytes (proto 2 adds 16 more: proto/rfm1.h RFM_HELLO_*): */
#define RF_HELLO_LEN 51u
/*  0 name[16] "RemoteFM1"          16 proto8         17 ver[8] (APP_VERSION)   25 transports8 (bit0 USB configured,
 *  bit1 vendor bulk built in, bit2 SysEx heard, bit3 bulk heard; 0.2: bit4 a Bluetooth central connected, bit5 SysEx
 *  heard over Bluetooth)   26 usb_mode8 (1: the main loop polls USB)   27 lcd_baud8   28 loader_state (int8: 0 the
 *  updates work, -2 an unknown flash chip; 0.1: its own staged loader)   29 sxq16 (SysEx TX packets free for
 *  RemoteFM1 on the transport asked: USB's ring, or Bluetooth's)   31 bk_rx_ring32   35 bk_tx_ring32
 * 39 sx_body_max16   41 bk_body_max16   43 identity[8] (the first 8 characters: "FM-1_520" for FM-1_52XXYYZZ;
 *  0.1 answered "FM-1_015") */

/* STATS_R, in this order (n x u32) */
enum {
    RF_ST_UPTIME_MS,
    RF_ST_SX_RX_MSGS, RF_ST_SX_SINK_MSGS, RF_ST_SX_SINK_BYTES, RF_ST_SX_GAPS, RF_ST_SX_LOST, RF_ST_SX_CHK, RF_ST_SX_BAD,
    RF_ST_SX_OVF, RF_ST_SX_HELD, RF_ST_SX_TX_MSGS, RF_ST_SX_TX_BYTES, RF_ST_SX_T_FIRST, RF_ST_SX_T_LAST,
    RF_ST_BK_RX_MSGS, RF_ST_BK_SINK_MSGS, RF_ST_BK_SINK_BYTES, RF_ST_BK_GAPS, RF_ST_BK_LOST, RF_ST_BK_CHK, RF_ST_BK_BAD,
    RF_ST_BK_OVF, RF_ST_BK_HELD, RF_ST_BK_TX_MSGS, RF_ST_BK_TX_BYTES, RF_ST_BK_T_FIRST, RF_ST_BK_T_LAST,
    RF_ST_BK_SYNC, RF_ST_BK_FRAME_GAPS,
    RF_ST_POLLS_MAIN_PS, RF_ST_POLLS_ISR_PS, RF_ST_USB_PERMILLE, RF_ST_LOOPS_PS,
    RF_ST_USB_RESETS, RF_ST_USB_SETUPS, RF_ST_EP1_RX, RF_ST_EP1_TX, RF_ST_EP3_RX, RF_ST_EP3_TX,
    RF_ST_TILES, RF_ST_TILE_BYTES, RF_ST_MIDI_IN_DROPPED, RF_ST_LCD_TIMEOUTS, RF_ST_SRC_US,
    RF_ST_SX_RX_PS, RF_ST_SX_TX_PS, RF_ST_BK_RX_PS, RF_ST_BK_TX_PS,
    /* M1 (appended: STATS_R's n8 says how many a device sends) */
    RF_ST_USB_CINF,                 /* CIN 0xF bytes taken into a SysEx (usb.c USB_CIN_F_SYSEX) */
    RF_ST_USB_RX_BAD,               /* malformed USB-MIDI events ignored */
    RF_ST_LOOP_MAX_US,              /* the longest main loop pass (outside an update session) */
    RF_ST_FRAMES, RF_ST_DECODE_ERRS, RF_ST_PUSH_US_LAST, RF_ST_PUSH_US_MAX, RF_ST_SESSIONS,
    RF_ST_N
};

/* order-sensitive checksum, cheap per byte */
static inline uint32_t rf_sum_byte(uint32_t s, uint32_t b) { return ((s << 1) | (s >> 31)) + b; }
RF_FN uint32_t rf_sum(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0;
    while (n--)
        s = rf_sum_byte(s, *p++);
    return s;
}
static inline void rf_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void rf_wr32(uint8_t *p, uint32_t v) { rf_wr16(p, v); rf_wr16(p + 2, v >> 16); }
static inline uint32_t rf_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rf_rd32(const uint8_t *p) { return rf_rd16(p) | rf_rd16(p + 2) << 16; }

/* ---- 7-in-8 */
static inline uint32_t rf_packed_len(uint32_t n) { return n + (n + 6u) / 7u; }
RF_FN uint32_t rf_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t o = 0, i, k, g;
    for (i = 0; i < n; i += g) {
        uint32_t msb = 0;
        g = n - i < 7u ? n - i : 7u;
        for (k = 0; k < g; k++)
            msb |= (uint32_t)(in[i + k] >> 7) << k;
        out[o++] = (uint8_t)msb;
        for (k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7Fu;
    }
    return o;
}
RF_FN uint32_t rf_unpack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t o = 0, i, g = 0, msb = 0;
    for (i = 0; i < n; i++) {
        if (!g) {
            msb = in[i];
            g = 1;
            continue;
        }
        out[o++] = (uint8_t)((in[i] & 0x7Fu) | ((msb >> (g - 1u)) & 1u) << 7);
        g = g == 7u ? 0u : g + 1u;
    }
    return o;
}
/* a whole SysEx message into out (rf_packed_len(n) + 6 bytes) */
RF_FN uint32_t rf_sx_encode(uint32_t cmd, const uint8_t *body, uint32_t n, uint8_t *out)
{
    uint32_t o;
    out[0] = 0xF0;
    out[1] = RF_SX_ID0;
    out[2] = RF_SX_ID1;
    out[3] = RF_SX_ID2;
    out[4] = (uint8_t)(cmd & 0x7Fu);
    o = 5u + rf_pack7(body, n, out + 5);
    out[o++] = 0xF7;
    return o;
}
RF_FN void rf_bk_header(uint8_t *h, uint32_t cmd, uint32_t len, uint32_t seq)
{
    h[0] = 'R';
    h[1] = 'F';
    h[2] = (uint8_t)cmd;
    h[3] = 0;
    rf_wr16(h + 4, len);
    rf_wr32(h + 6, seq);
}

/* ---- receive: per-transport counters and the body consumer both framings feed */
typedef struct {
    uint32_t rx_msgs, sink_msgs, sink_bytes, gaps, lost, chk, bad, ovf, held, tx_msgs, tx_bytes, t_first, t_last;
    uint32_t sync, frame_gaps;
} rf_tstats_t;

typedef void (*rf_msg_fn)(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len);

typedef struct {
    rf_tstats_t *st;
    rf_msg_fn fn;
    void *ctx;
    uint8_t *buf;                   /* collected body (not SINK) */
    uint32_t cap;
    uint32_t cmd, pos, seq, sum, expect;
    uint8_t dl[4];                  /* SINK: the last 4 bytes are the checksum; a delay line keeps them out of the sum */
    uint8_t over, have_seq;
    uint32_t now_ms;                /* set by the caller: SINK first / last times */
} rf_body_t;

RF_FN void rf_body_init(rf_body_t *m, rf_tstats_t *st, uint8_t *buf, uint32_t cap, rf_msg_fn fn, void *ctx)
{
    m->st = st;
    m->buf = buf;
    m->cap = cap;
    m->fn = fn;
    m->ctx = ctx;
    m->have_seq = 0;
    m->pos = 0;
}
RF_FN void rf_body_begin(rf_body_t *m, uint32_t cmd)
{
    m->cmd = cmd;
    m->pos = m->seq = m->sum = 0;
    m->over = 0;
}
static inline void rf_body_byte(rf_body_t *m, uint32_t b)
{
    uint32_t p = m->pos++;
    if (m->cmd == RF_SINK) {
        if (p < 4u) {
            m->seq |= b << (8u * p);
        } else {
            p -= 4u;
            if (p >= 4u)
                m->sum = rf_sum_byte(m->sum, m->dl[p & 3u]);
            m->dl[p & 3u] = (uint8_t)b;
        }
    } else if (p < m->cap) {
        m->buf[p] = (uint8_t)b;
    } else {
        m->over = 1;
    }
}
RF_FN void rf_body_end(rf_body_t *m)
{
    rf_tstats_t *st = m->st;
    if (m->cmd == RF_SINK) {
        uint32_t k, chk;
        if (m->pos < 8u) {
            st->bad++;
            return;
        }
        k = m->pos - 8u;                            /* the checksum's first byte, counted after seq */
        chk = (uint32_t)m->dl[k & 3u] | (uint32_t)m->dl[(k + 1u) & 3u] << 8 | (uint32_t)m->dl[(k + 2u) & 3u] << 16 |
              (uint32_t)m->dl[(k + 3u) & 3u] << 24;
        st->rx_msgs++;
        if (!st->sink_msgs++)
            st->t_first = m->now_ms;
        st->t_last = m->now_ms;
        st->sink_bytes += m->pos - 8u;
        if (chk != m->sum)
            st->chk++;
        if (m->have_seq && m->seq != m->expect) {
            st->gaps++;
            if ((int32_t)(m->seq - m->expect) > 0)
                st->lost += m->seq - m->expect;
        }
        m->expect = m->seq + 1u;
        m->have_seq = 1;
        return;
    }
    if (m->over) {
        st->bad++;
        return;
    }
    st->rx_msgs++;
    m->fn(m->ctx, m->cmd, m->buf, m->pos);
}
RF_FN void rf_body_reset_seq(rf_body_t *m) { m->have_seq = 0; }

/* ---- SysEx framing, fed byte by byte (realtime bytes may come anywhere and are ignored) */
typedef struct {
    rf_body_t b;
    uint8_t st, grp, msb;           /* st: 0 idle, 1..3 the ID, 4 the command, 5 the body, 6 not ours */
} rf_sx_t;

RF_FN void rf_sx_byte(rf_sx_t *s, uint32_t c)
{
    if (c >= 0xF8u)
        return;
    if (c == 0xF0u) {
        if (s->st >= 4u && s->st <= 5u)
            s->b.st->bad++;                         /* ours, cut off by a new message */
        s->st = 1;
        return;
    }
    if (c == 0xF7u) {
        if (s->st == 5u)
            rf_body_end(&s->b);
        else if (s->st == 4u)
            s->b.st->bad++;
        s->st = 0;
        return;
    }
    if (c & 0x80u) {                                /* another status ends a SysEx */
        if (s->st >= 4u && s->st <= 5u)
            s->b.st->bad++;
        s->st = 0;
        return;
    }
    switch (s->st) {
    case 1:
        s->st = c == RF_SX_ID0 ? 2 : 6;
        break;
    case 2:
        s->st = c == RF_SX_ID1 ? 3 : 6;
        break;
    case 3:
        s->st = c == RF_SX_ID2 ? 4 : 6;
        break;
    case 4:
        rf_body_begin(&s->b, c);
        s->grp = 0;
        s->st = 5;
        break;
    case 5:
        if (!s->grp) {
            s->msb = (uint8_t)c;
            s->grp = 1;
        } else {
            rf_body_byte(&s->b, c | ((uint32_t)(s->msb >> (s->grp - 1u)) & 1u) << 7);
            s->grp = s->grp == 7u ? 0 : (uint8_t)(s->grp + 1u);
        }
        break;
    default:
        break;
    }
}

/* ---- bulk framing, fed in chunks (frames may span chunks; a bad magic resyncs byte by byte) */
typedef struct {
    rf_body_t b;
    uint8_t hdr[RF_BK_HDR];
    uint32_t hn, left, fexpect;
    uint8_t in_body, have_fseq;
} rf_bk_t;

RF_FN void rf_bk_feed(rf_bk_t *k, const uint8_t *p, uint32_t n)
{
    while (n) {
        if (k->in_body) {
            uint32_t m = n < k->left ? n : k->left, i;
            for (i = 0; i < m; i++)
                rf_body_byte(&k->b, p[i]);
            p += m;
            n -= m;
            k->left -= m;
            if (!k->left) {
                k->in_body = 0;
                rf_body_end(&k->b);
            }
            continue;
        }
        k->hdr[k->hn++] = *p++;
        n--;
        if (k->hn == 1u && k->hdr[0] != 'R') {
            k->b.st->sync++;
            k->hn = 0;
        } else if (k->hn == 2u && k->hdr[1] != 'F') {
            k->b.st->sync++;
            k->hn = k->hdr[1] == 'R' ? 1u : 0u;
            k->hdr[0] = 'R';
        } else if (k->hn == RF_BK_HDR) {
            uint32_t seq = rf_rd32(k->hdr + 6);
            k->hn = 0;
            if (k->have_fseq && seq != k->fexpect)
                k->b.st->frame_gaps++;
            k->fexpect = seq + 1u;
            k->have_fseq = 1;
            rf_body_begin(&k->b, k->hdr[2]);
            k->left = rf_rd16(k->hdr + 4);
            if (k->left)
                k->in_body = 1;
            else
                rf_body_end(&k->b);
        }
    }
}
#endif
