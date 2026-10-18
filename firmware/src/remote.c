/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 M0: the measuring build (PLAN.md section 7). It measures what the transports carry and what the panel
 * costs, with the bench protocol of remote_proto.h over USB-MIDI SysEx (EP1) and over a vendor bulk pair (EP3,
 * usb.c REMOTE_VENDOR), and forwards the panel as MIDI (keys ch 16, buttons ch 15, encoders CC 16..22 ch 15
 * relative, MASTER CC 7 / 39 ch 15). Everything TransitFM1 has for safety stays as it is, underneath (platform.c):
 * the boot stub, the staged own loader, step 1, the M-UPGRADE updater as FM-1_015 on the port "FM-1", the boot
 * guard.
 *
 * USB service (REMOTE_USB_MAINLOOP):
 *   0  TransitFM1's: usb_poll() from the TIMER5 ISR at 2 kHz, nothing else
 *   1  (default) the main loop polls as often as it can (rf_main_poll); the TIMER5 ISR polls at 2 kHz only while
 *      the main loop has not for 0.5 ms (an update session, a long LCD transfer, the blit bench)
 *   2  the TIMER5 ISR on every tick, 10 kHz, nothing from the main loop
 * One context touches the SIE at a time: in mode 1 the main loop raises rf_main_busy around its usb_poll() and the
 * ISR skips its poll while the flag is up; the ISR runs to its end before the main loop resumes (one core, no other
 * interrupt touches USB). usb_start (usb_retry) runs from the main loop while usb.up == 0, when usb_poll returns at
 * once, as in TransitFM1. The rings between usb_poll and the main loop have one producer and one consumer each,
 * whichever context usb_poll runs in.
 *
 * The updater: ota_service (handshakes) runs from platform_poll every millisecond; an upgrade request runs
 * ota_session() inside platform_poll, whose waits (ota_idle) poll nothing: there the ISR serves USB at 2 kHz, as in
 * TransitFM1. The bench never holds the updater's SysEx back for long: its RX ring takes only RemoteFM1 SysEx
 * (F0 7D 52 46), EP1 is held (NAK) for room only while the main loop drains it (20 ms), and the bench leaves
 * RF_SXQ_RESERVE events of the SysEx TX ring free for the updater's replies. */
#define APP_NAME "REMOTE FM1"
#ifndef REMOTE_USB_MAINLOOP
#define REMOTE_USB_MAINLOOP 1
#endif
#define REMOTE_VENDOR 1
#define SXQ 4096u                                       /* SysEx TX events: a whole 8 KB SOURCE message fits */
#define RF_SXQ_RESERVE 512u                             /* never used by the bench: the updater's replies */
#define USB_SYSEX_HOOK(b) rf_sx_hook(b)
#define USB_EP1_ROOM() rf_ep1_room()
#define USB_FRAME_DUE() rf_frame_due()
#define PLATFORM_USB_TICK(sub, t0) rf_usb_tick(sub, t0)
#include <stdint.h>
static void rf_sx_hook(uint32_t b);
static int rf_ep1_room(void);
static int rf_frame_due(void);
static void rf_usb_tick(uint32_t sub, uint32_t t0);
#include "platform.c"
#include "fm1_adc.h"
#include "remote_proto.h"

#define C_ACC RGB(120, 200, 255)
#define C_OFF RGB(110, 110, 110)
#define C_BAD RGB(255, 90, 60)
#define C_OK RGB(120, 230, 140)

/* ---------------------------------------------------- USB service --- */
#if REMOTE_USB_MAINLOOP == 1
static volatile uint8_t rf_main_busy;                   /* the main loop is inside usb_poll */
static volatile uint32_t rf_main_last;                  /* ticks: its last poll ended */
#endif
static uint32_t rf_polls_main, rf_polls_isr, rf_ticks_main, rf_ticks_isr;

static void rf_isr_poll(void)
{
    uint32_t t0 = fm1_ticks();
    usb_poll();
    rf_ticks_isr += fm1_ticks() - t0;
    rf_polls_isr++;
}

static void rf_usb_tick(uint32_t sub, uint32_t t0)      /* TIMER5 ISR, 10 kHz */
{
#if REMOTE_USB_MAINLOOP == 2
    (void)sub;
    (void)t0;
    rf_isr_poll();
#elif REMOTE_USB_MAINLOOP == 1
    if (sub % 5u == 0u && !rf_main_busy && (uint32_t)(t0 - rf_main_last) >= 500u * FM1_TICKS_PER_US)
        rf_isr_poll();
#else
    (void)t0;
    if (sub % 5u == 0u)
        rf_isr_poll();
#endif
}

static void rf_main_poll(void)
{
#if REMOTE_USB_MAINLOOP == 1
    uint32_t t0, t1;
    rf_main_busy = 1;
    RING_PUBLISH();
    t0 = fm1_ticks();
    usb_poll();
    t1 = fm1_ticks();
    RING_PUBLISH();
    rf_main_last = t1;
    rf_main_busy = 0;
    rf_ticks_main += t1 - t0;
    rf_polls_main++;
#endif
}

static int rf_frame_due(void)                           /* usb_poll's context */
{
    static uint32_t last;
    if (fm1_ms - last < 10u)
        return 0;
    last = fm1_ms;
    return 1;
}

/* ---- SysEx in: usb_poll's context puts RemoteFM1 SysEx (only) into rf_mrx; the main loop parses it */
#define RF_MRX_N 16384u
static uint8_t rf_mrx[RF_MRX_N] __attribute__((section(".pool")));
static volatile uint32_t mrx_w, mrx_r;
static volatile uint32_t rf_drain_t;                    /* ticks: the main loop last drained the rings */
static uint32_t rf_mrx_ovf, rf_sx_held, rf_midi_drop;
static uint8_t rfh_st;                                  /* 0 idle, 1..3 the ID so far, 4 ours, 5 not ours */

static inline void rf_mrx_put(uint32_t b)
{
    uint32_t w = mrx_w;
    if (w - mrx_r >= RF_MRX_N) {
        rf_mrx_ovf++;
        return;
    }
    rf_mrx[w & (RF_MRX_N - 1u)] = (uint8_t)b;
    RING_PUBLISH();
    mrx_w = w + 1u;
}

static void rf_sx_hook(uint32_t b)                      /* usb_poll's context; never a realtime byte */
{
    static const uint8_t ID[3] = {RF_SX_ID0, RF_SX_ID1, RF_SX_ID2};
    if (b == 0xF0u) {
        if (rfh_st == 4u)
            rf_mrx_put(0xF0u);                          /* ours, cut off: the parser counts it */
        rfh_st = 1;
        return;
    }
    if (b & 0x80u) {                                    /* F7, or a status that ends the SysEx */
        if (rfh_st == 4u)
            rf_mrx_put(b);
        rfh_st = 0;
        return;
    }
    if (rfh_st >= 1u && rfh_st <= 3u) {
        if (b != ID[rfh_st - 1u]) {
            rfh_st = 5;
        } else if (++rfh_st == 4u) {
            rf_mrx_put(0xF0u);
            rf_mrx_put(RF_SX_ID0);
            rf_mrx_put(RF_SX_ID1);
            rf_mrx_put(RF_SX_ID2);
        }
        return;
    }
    if (rfh_st == 4u)
        rf_mrx_put(b);
}

/* before an EP1 OUT packet is taken (usb_poll's context): nobody here plays MIDI in, so the MIDI ring is emptied
 * (TransitFM1 never drained it: a DAW's clock would fill it and hold EP1, the updater's port, for good); room for
 * a packet's SysEx (48 bytes) in rf_mrx, unless the main loop has been away for 20 ms (an update session): then
 * what does not fit is dropped (counted), and the updater's SysEx goes on */
static int rf_ep1_room(void)
{
    if (mi_w != mi_r) {
        rf_midi_drop += mi_w - mi_r;
        mi_r = mi_w;
    }
    if (RF_MRX_N - (mrx_w - mrx_r) >= 64u)
        return 1;
    if ((uint32_t)(fm1_ticks() - rf_drain_t) > 20000u * FM1_TICKS_PER_US)
        return 1;
    rf_sx_held++;
    return 0;
}

/* ------------------------------------------------------- send --- */
static rf_tstats_t rf_st[2];
static uint32_t rf_raw_rx[2], rf_raw_tx[2], rf_tx_drop;
static uint32_t rf_bk_seq;

static int rf_room(uint32_t t, uint32_t n)              /* a message with an n-byte body fits now */
{
    if (t == RF_T_BK)
        return VB_OUT_N - (vo_w - vo_r) >= RF_BK_HDR + n;
    return SXQ - (so_w - so_r) >= (6u + rf_packed_len(n) + 2u) / 3u + RF_SXQ_RESERVE;
}

/* SysEx bytes into USB-MIDI event packets (CIN 4 continues, 5 / 6 / 7 end with 1 / 2 / 3 bytes) */
typedef struct {
    uint32_t w, k;
    uint8_t a[3];
} rf_em_t;
static inline void rf_em_put(rf_em_t *e, uint32_t b)
{
    if (e->k == 3u) {
        sx_out_q[e->w & (SXQ - 1u)] = 4u | (uint32_t)e->a[0] << 8 | (uint32_t)e->a[1] << 16 | (uint32_t)e->a[2] << 24;
        e->w++;
        e->k = 0;
    }
    e->a[e->k++] = (uint8_t)b;
}
static void rf_em_end(rf_em_t *e)
{
    uint32_t p = 4u + e->k;
    p |= (uint32_t)e->a[0] << 8;
    if (e->k > 1u)
        p |= (uint32_t)e->a[1] << 16;
    if (e->k > 2u)
        p |= (uint32_t)e->a[2] << 24;
    sx_out_q[e->w & (SXQ - 1u)] = p;
    e->w++;
}

/* 1 sent, 0 no room now, -1 no host. A whole message is published at once: ep1_tx never sends part of one, so
 * nothing gets between its packets (TransitFM1's ota_wire_send, the only other producer, runs in the main loop
 * too, never in the middle of one of ours) */
static int rf_try(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    uint32_t i, k;
    if (!usb.config || !usb.up)
        return -1;
    if (!rf_room(t, n))
        return 0;
    if (t == RF_T_BK) {
        uint8_t h[RF_BK_HDR];
        uint32_t w = vo_w;
        rf_bk_header(h, cmd, n, rf_bk_seq++);
        for (i = 0; i < RF_BK_HDR; i++)
            vb_out[(w + i) & (VB_OUT_N - 1u)] = h[i];
        w += RF_BK_HDR;
        for (i = 0; i < n; i++)
            vb_out[(w + i) & (VB_OUT_N - 1u)] = body[i];
        RING_PUBLISH();
        vo_w = w + n;
        rf_raw_tx[t] += RF_BK_HDR + n;
    } else {
        rf_em_t e;
        e.w = so_w;
        e.k = 0;
        rf_em_put(&e, 0xF0u);
        rf_em_put(&e, RF_SX_ID0);
        rf_em_put(&e, RF_SX_ID1);
        rf_em_put(&e, RF_SX_ID2);
        rf_em_put(&e, cmd & 0x7Fu);
        for (i = 0; i < n; i += 7u) {
            uint32_t g = n - i < 7u ? n - i : 7u, msb = 0;
            for (k = 0; k < g; k++)
                msb |= (uint32_t)(body[i + k] >> 7) << k;
            rf_em_put(&e, msb);
            for (k = 0; k < g; k++)
                rf_em_put(&e, body[i + k] & 0x7Fu);
        }
        rf_em_put(&e, 0xF7u);
        rf_em_end(&e);
        RING_PUBLISH();
        so_w = e.w;
        rf_raw_tx[t] += 6u + rf_packed_len(n);
    }
    rf_st[t].tx_msgs++;
    return 1;
}

static void rf_send(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)   /* waits up to 300 ms, polling USB */
{
    uint32_t t0 = fm1_ms;
    int r;
    while (!(r = rf_try(t, cmd, body, n))) {
        if (fm1_ms - t0 > 300u) {
            rf_tx_drop++;
            return;
        }
        rf_main_poll();
        fm1_wdt_feed();
    }
}

/* ------------------------------------------------- the commands --- */
static uint8_t rf_heard;                                /* bit t: a message came over transport t */
static uint32_t rf_tiles, rf_tile_bytes, rf_src_us;
static uint32_t rf_ps[8];                               /* per second: polls main, isr, usb permille, loops, raw rx/tx */
static uint32_t rf_scr_hold;                            /* fm1_ms until which the status screen keeps off the panel */
static uint8_t rf_scr_full = 1;
static uint32_t rf_blit_last[2][4];                     /* the screen's: baud, full, tiles, strips (us) */

static void rf_hello(uint32_t t)
{
    static const char NAME[] = RF_NAME, VER[] = APP_VER, ID[] = FELUCCA_ID;
    uint8_t h[RF_HELLO_LEN];
    uint32_t i;
    for (i = 0; i < sizeof h; i++)
        h[i] = 0;
    for (i = 0; i < 16u && i < sizeof NAME - 1u; i++)
        h[i] = (uint8_t)NAME[i];
    h[16] = RF_PROTO;
    for (i = 0; i < 8u && i < sizeof VER - 1u; i++)
        h[17 + i] = (uint8_t)VER[i];
    h[25] = (uint8_t)((usb.config ? 1u : 0u) | 2u | (uint32_t)rf_heard << 2);
    h[26] = REMOTE_USB_MAINLOOP;
    h[27] = LCD_BAUD;
    h[28] = (uint8_t)(int8_t)loader_state;
    rf_wr16(h + 29, SXQ - RF_SXQ_RESERVE);
    rf_wr32(h + 31, VB_IN_N);
    rf_wr32(h + 35, VB_OUT_N);
    rf_wr16(h + 39, RF_SX_BODY_MAX);
    rf_wr16(h + 41, RF_BK_BODY_MAX);
    for (i = 0; i < 8u && i < sizeof ID - 1u; i++)
        h[43 + i] = (uint8_t)ID[i];
    rf_send(t, RF_HELLO_R, h, sizeof h);
}

static void rf_stats_send(uint32_t t)
{
    uint32_t v[RF_ST_N], i, k;
    uint8_t b[1 + 4 * RF_ST_N];
    const rf_tstats_t *s;
    for (i = 0; i < RF_ST_N; i++)
        v[i] = 0;
    v[RF_ST_UPTIME_MS] = fm1_ms;
    for (k = 0; k < 2u; k++) {
        uint32_t o = k ? RF_ST_BK_RX_MSGS - RF_ST_SX_RX_MSGS : 0u;
        s = &rf_st[k];
        v[RF_ST_SX_RX_MSGS + o] = s->rx_msgs;
        v[RF_ST_SX_SINK_MSGS + o] = s->sink_msgs;
        v[RF_ST_SX_SINK_BYTES + o] = s->sink_bytes;
        v[RF_ST_SX_GAPS + o] = s->gaps;
        v[RF_ST_SX_LOST + o] = s->lost;
        v[RF_ST_SX_CHK + o] = s->chk;
        v[RF_ST_SX_BAD + o] = s->bad;
        v[RF_ST_SX_TX_MSGS + o] = s->tx_msgs;
        v[RF_ST_SX_TX_BYTES + o] = s->tx_bytes;
        v[RF_ST_SX_T_FIRST + o] = s->t_first;
        v[RF_ST_SX_T_LAST + o] = s->t_last;
    }
    v[RF_ST_SX_OVF] = rf_mrx_ovf;
    v[RF_ST_SX_HELD] = rf_sx_held;
    v[RF_ST_BK_OVF] = 0;                                /* bulk is held (NAK), never dropped */
    v[RF_ST_BK_HELD] = vb.held;
    v[RF_ST_BK_SYNC] = rf_st[1].sync;
    v[RF_ST_BK_FRAME_GAPS] = rf_st[1].frame_gaps;
    v[RF_ST_POLLS_MAIN_PS] = rf_ps[0];
    v[RF_ST_POLLS_ISR_PS] = rf_ps[1];
    v[RF_ST_USB_PERMILLE] = rf_ps[2];
    v[RF_ST_LOOPS_PS] = rf_ps[3];
    v[RF_ST_USB_RESETS] = usb.resets;
    v[RF_ST_USB_SETUPS] = usb.setups;
    v[RF_ST_EP1_RX] = usb.rx_pkts;
    v[RF_ST_EP1_TX] = usb.tx_pkts;
    v[RF_ST_EP3_RX] = vb.rx_pkts;
    v[RF_ST_EP3_TX] = vb.tx_pkts;
    v[RF_ST_TILES] = rf_tiles;
    v[RF_ST_TILE_BYTES] = rf_tile_bytes;
    v[RF_ST_MIDI_IN_DROPPED] = rf_midi_drop;
    v[RF_ST_LCD_TIMEOUTS] = fm1_lcd_timeouts;
    v[RF_ST_SRC_US] = rf_src_us;
    v[RF_ST_SX_RX_PS] = rf_ps[4];
    v[RF_ST_SX_TX_PS] = rf_ps[5];
    v[RF_ST_BK_RX_PS] = rf_ps[6];
    v[RF_ST_BK_TX_PS] = rf_ps[7];
    b[0] = RF_ST_N;
    for (i = 0; i < RF_ST_N; i++)
        rf_wr32(b + 1 + 4 * i, v[i]);
    rf_send(t, RF_STATS_R, b, sizeof b);
}

static rf_sx_t rf_sx;
static rf_bk_t rf_bk;

static void rf_stats_reset(void)
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        rf_tstats_t z = {0};
        rf_st[k] = z;
    }
    rf_body_reset_seq(&rf_sx.b);
    rf_body_reset_seq(&rf_bk.b);
    rf_bk.have_fseq = 0;
    rf_mrx_ovf = rf_sx_held = rf_midi_drop = 0;
    vb.held = 0;
    rf_tiles = rf_tile_bytes = rf_src_us = rf_tx_drop = 0;
}

/* ---- SOURCE: DATA messages as fast as the transport takes them */
static uint8_t rf_gen[RF_BK_SRC_MAX + 8u] __attribute__((section(".pool")));
static struct {
    uint8_t on, t;
    uint32_t left, msg_len, seq, msgs, bytes, t0, last_ok;
} rf_src;

static void rf_src_done(uint32_t aborted)
{
    uint8_t b[13];
    rf_src.on = 0;
    rf_src_us = (fm1_ticks() - rf_src.t0) / FM1_TICKS_PER_US;
    rf_wr32(b, rf_src.bytes);
    rf_wr32(b + 4, rf_src.msgs);
    rf_wr32(b + 8, rf_src_us);
    b[12] = (uint8_t)aborted;
    rf_send(rf_src.t, RF_SOURCE_DONE, b, sizeof b);
}

static void rf_src_start(uint32_t t, const uint8_t *body, uint32_t len)
{
    uint32_t max = t == RF_T_BK ? RF_BK_SRC_MAX : RF_SX_SRC_MAX, ml;
    if (len < 6u) {
        rf_st[t].bad++;
        return;
    }
    ml = rf_rd16(body + 4);
    rf_src.t = (uint8_t)t;
    rf_src.left = rf_rd32(body);
    rf_src.msg_len = ml < 1u ? 1u : ml > max ? max : ml;
    rf_src.seq = rf_src.msgs = rf_src.bytes = 0;
    rf_src.t0 = fm1_ticks();
    rf_src.last_ok = fm1_ms;
    rf_src.on = 1;
}

static void rf_src_step(void)                          /* at most ~2 KB generated a call: USB is polled between */
{
    uint32_t made = 0;
    while (rf_src.on && made < 2048u) {
        uint32_t n = rf_src.left < rf_src.msg_len ? rf_src.left : rf_src.msg_len, i, s = 0, seq = rf_src.seq;
        uint8_t *d = rf_gen + 4;
        int r;
        if (!n) {
            rf_src_done(0);
            return;
        }
        if (!usb.config) {
            rf_src_done(1);
            return;
        }
        if (!rf_room(rf_src.t, n + 8u)) {               /* (check first: a full ring costs no generating) */
            if (fm1_ms - rf_src.last_ok > 2000u)
                rf_src_done(1);                         /* nobody reads */
            return;
        }
        rf_wr32(rf_gen, seq);
        for (i = 0; i < n; i++) {
            uint32_t b = (seq * 7u + i) & 0xFFu;
            d[i] = (uint8_t)b;
            s = rf_sum_byte(s, b);
        }
        rf_wr32(d + n, s);
        r = rf_try(rf_src.t, RF_DATA, rf_gen, n + 8u);
        if (r <= 0) {
            rf_src_done(1);
            return;
        }
        made += n;
        rf_src.seq++;
        rf_src.msgs++;
        rf_src.bytes += n;
        rf_src.left -= n;
        rf_st[rf_src.t].tx_bytes += n;
        rf_src.last_ok = fm1_ms;
    }
}

/* ---- the panel: TILE, BLIT_BENCH */
static uint16_t rf_fb[240u * 240u] __attribute__((section(".pool")));      /* the blit bench's full frame */
static uint16_t rf_tbuf[2][240u * 16u] __attribute__((section(".pool")));  /* TILE: two, so a copy overlaps a blit */
static uint16_t rf_tile2[2][256];
static uint8_t rf_tsel;

static void rf_lcd_wait_polling(void)                   /* the running transfer ends; USB is served meanwhile */
{
    uint32_t t0 = fm1_ticks();
    while (lcd_busy && !fm1_lcd_done() && (uint32_t)(fm1_ticks() - t0) < 100000u * FM1_TICKS_PER_US)
        rf_main_poll();
}

static void rf_tile(uint32_t t, const uint8_t *body, uint32_t len)
{
    uint32_t x, y, w, h, n, i;
    uint8_t *d;
    if (len < 4u) {
        rf_st[t].bad++;
        return;
    }
    x = body[0];
    y = body[1];
    w = body[2];
    h = body[3];
    n = w * h * 2u;
    if (!w || !h || x + w > 240u || y + h > 240u || w * h > 240u * 16u || len != 4u + n) {
        rf_st[t].bad++;
        return;
    }
    d = (uint8_t *)rf_tbuf[rf_tsel];                    /* (its last blit ended before the one after it began) */
    for (i = 0; i < n; i++)
        d[i] = body[4 + i];
    rf_lcd_wait_polling();
    lcd_blit(x, y, w, h, rf_tbuf[rf_tsel]);             /* left running */
    rf_tsel ^= 1u;
    rf_tiles++;
    rf_tile_bytes += n;
    rf_scr_hold = fm1_ms + 3000u;
}

static uint32_t rf_us_since(uint32_t t0) { return (fm1_ticks() - t0) / FM1_TICKS_PER_US; }

/* the three patterns at each SPI1 baud asked for (lsb / (baud + 1): 4 = 12 MHz, 2 = 20 MHz), each timed to its
 * last byte: a full frame from RAM (one window, four 28,800-byte DMAs), 225 16 x 16 tiles with a window each, 15
 * 240 x 16 strips with a window each. USB: the ISR serves it (mode 1: the main loop is away) */
static void rf_blit_bench(uint32_t t, const uint8_t *body, uint32_t len)
{
    static const uint8_t DEF[2] = {4, 2};
    uint8_t out[1 + 13 * 6];
    uint32_t nb = len ? (len > 6u ? 6u : len) : 2u, k, i, x, y, t0;
    const uint8_t *bauds = len ? body : DEF;
    for (i = 0; i < 256u; i++) {
        rf_tile2[0][i] = swap16(RGB(255, 40 + (i & 15u) * 12u, 0));
        rf_tile2[1][i] = swap16(RGB(0, 40 + (i >> 4) * 12u, 255));
    }
    rf_lcd_wait_polling();
    lcd_sync();
    out[0] = (uint8_t)nb;
    for (k = 0; k < nb; k++) {
        uint32_t b = bauds[k] < 1u ? 1u : bauds[k] > 15u ? 15u : bauds[k], full, tiles, strips;
        for (y = 0; y < 240u; y++)
            for (x = 0; x < 240u; x++)
                rf_fb[y * 240u + x] = swap16(RGB(x, y, (k * 80u) & 255u));
        fm1_wdt_feed();
        fm1_lcd_baud(b);
        t0 = fm1_ticks();
        lcd_window(0, 0, 239, 239);
        for (i = 0; i < 4u; i++)
            lcd_data((const uint8_t *)rf_fb + i * 28800u, 28800u);
        lcd_sync();
        full = rf_us_since(t0);
        fm1_wdt_feed();
        rf_main_poll();
        t0 = fm1_ticks();
        for (y = 0; y < 15u; y++)
            for (x = 0; x < 15u; x++) {
                lcd_window(x * 16u, y * 16u, x * 16u + 15u, y * 16u + 15u);
                lcd_data(rf_tile2[(x + y) & 1u], 512u);
            }
        lcd_sync();
        tiles = rf_us_since(t0);
        fm1_wdt_feed();
        rf_main_poll();
        t0 = fm1_ticks();
        for (y = 0; y < 15u; y++) {
            lcd_window(0, y * 16u, 239, y * 16u + 15u);
            lcd_data(rf_fb + y * 16u * 240u, 7680u);
        }
        lcd_sync();
        strips = rf_us_since(t0);
        fm1_wdt_feed();
        rf_main_poll();
        out[1 + 13 * k] = (uint8_t)b;
        rf_wr32(out + 2 + 13 * k, full);
        rf_wr32(out + 6 + 13 * k, tiles);
        rf_wr32(out + 10 + 13 * k, strips);
        if (k < 2u) {
            rf_blit_last[k][0] = b;
            rf_blit_last[k][1] = full;
            rf_blit_last[k][2] = tiles;
            rf_blit_last[k][3] = strips;
        }
    }
    fm1_lcd_baud(LCD_BAUD);
    rf_scr_hold = fm1_ms + 3000u;
    rf_send(t, RF_BLIT_R, out, 1u + 13u * nb);
}

static void rf_on_msg(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    uint32_t t = (uint32_t)(uintptr_t)ctx;
    rf_heard |= (uint8_t)(1u << t);
    switch (cmd) {
    case RF_HELLO:
        if (rf_src.on)
            rf_src_done(1);
        rf_hello(t);
        break;
    case RF_SOURCE:
        rf_src_start(t, body, len);
        break;
    case RF_ECHO:
        rf_send(t, RF_ECHO_R, body, len);
        break;
    case RF_STATS:
        rf_stats_send(t);
        break;
    case RF_STATS_RESET:
        rf_stats_reset();
        rf_send(t, RF_ACK, body, 0);
        break;
    case RF_BLIT_BENCH:
        rf_blit_bench(t, body, len);
        break;
    case RF_TILE:
        rf_tile(t, body, len);
        break;
    default:
        rf_st[t].bad++;
        break;
    }
}

/* ---- the main loop drains both rings through the parsers (a bounded piece each time) */
static uint8_t rf_sx_buf[RF_SX_BODY_MAX];
static uint8_t rf_bk_buf[RF_BK_BODY_MAX] __attribute__((section(".pool")));

static void rf_rx_step(void)
{
    uint32_t r, n, i;
    rf_drain_t = fm1_ticks();
    rf_sx.b.now_ms = rf_bk.b.now_ms = fm1_ms;
    r = mrx_r;
    n = mrx_w - r;
    if (n > 512u)
        n = 512u;
    RING_PUBLISH();                                     /* the slots after the index */
    for (i = 0; i < n; i++) {
        rf_sx_byte(&rf_sx, rf_mrx[(r + i) & (RF_MRX_N - 1u)]);
        RING_PUBLISH();
        mrx_r = r + i + 1u;                             /* (a command may take long: free its bytes as they go) */
    }
    rf_raw_rx[RF_T_SX] += n;
    r = vi_r;
    n = vi_w - r;
    if (n > 1024u)
        n = 1024u;
    RING_PUBLISH();
    while (n) {
        uint32_t o = r & (VB_IN_N - 1u), seg = VB_IN_N - o;
        if (seg > n)
            seg = n;
        if (seg > 256u)
            seg = 256u;
        rf_bk_feed(&rf_bk, vb_in + o, seg);
        RING_PUBLISH();
        r += seg;
        vi_r = r;
        n -= seg;
        rf_raw_rx[RF_T_BK] += seg;
    }
}

/* ---- the panel as MIDI: keys Note On / Off ch 16 (53 + key), buttons ch 15 (note = id), encoders CC 16..22 ch 15
 * (64 + steps: 65 = +1, 63 = -1), MASTER CC 7 / 39 ch 15 (14 bit) */
static void rf_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_out_event((st >> 4) | st << 8 | d1 << 16 | d2 << 24);
}

static void rf_input_step(void)
{
    static uint32_t notes, btns, last_ms, adc_ms;
    static int32_t master = -100;
    uint32_t n, ch, i, e;
    if (fm1_ms == last_ms)
        return;
    last_ms = fm1_ms;
    n = fm1_in.notes & 0x7FFFFFFu;
    for (ch = n ^ notes, i = 0; ch; ch >>= 1, i++)
        if (ch & 1u) {
            uint32_t on = (n >> i) & 1u;
            rf_midi(on ? 0x9Fu : 0x8Fu, 53u + i, on ? 100u : 0u);
            fm1_led_key(14u + i, (int)on);
        }
    notes = n;
    n = fm1_in.buttons & 0x3FFFu;
    for (ch = n ^ btns, i = 0; ch; ch >>= 1, i++)
        if (ch & 1u) {
            uint32_t on = (n >> i) & 1u;
            rf_midi(on ? 0x9Eu : 0x8Eu, i, on ? 100u : 0u);
            fm1_led_key(i, (int)on);
        }
    btns = n;
    for (e = 0; e < FM1_NENC; e++) {
        int32_t s = fm1_enc_take(e);
        if (s)
            rf_midi(0xBEu, 16u + e, (uint32_t)(64 + (s > 63 ? 63 : s < -63 ? -63 : s)));
    }
    if (last_ms - adc_ms >= 10u) {
        int32_t v;
        adc_ms = last_ms;
        v = fm1_adc_read(FM1_ADC_MASTER);
        if (v >= 0 && (v - master > 3 || master - v > 3)) {
            master = v;
            rf_midi(0xBEu, 7u, (uint32_t)v >> 3);
            rf_midi(0xBEu, 39u, ((uint32_t)v & 7u) << 4);
        }
    }
}

/* ---- the status screen: a line at a time, only when the panel is idle, left running (USB goes on) */
#define RF_NL 10u
static char rf_line[RF_NL][32], rf_want[RF_NL][32];
static const uint8_t RF_LINE_Y[RF_NL] = {4, 40, 66, 86, 106, 126, 146, 170, 190, 218};

static char *s_put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}
static char *s_int(char *d, uint32_t v)
{
    fmt_int(d, (int32_t)v);
    while (*d)
        d++;
    return d;
}
static char *s_kbs(char *d, uint32_t bps)                /* bytes / s -> "12.3" (KB/s) */
{
    fmt_fix(d, (int32_t)(bps / 1024u * 10u + (bps % 1024u) * 10u / 1024u), 1);
    while (*d)
        d++;
    return d;
}
static char *s_ms(char *d, uint32_t us)                  /* us -> "77.1" (ms) */
{
    fmt_fix(d, (int32_t)(us / 100u), 1);
    while (*d)
        d++;
    return d;
}

static uint32_t rf_errs(uint32_t t)
{
    const rf_tstats_t *s = &rf_st[t];
    return s->gaps + s->chk + s->bad + (t ? s->sync + s->frame_gaps : rf_mrx_ovf);
}

static void rf_lines(void)
{
    char *p;
    uint32_t k;
    s_put(rf_want[0], "REMOTE FM1");
    s_put(rf_want[1], "M0 BENCH  v" APP_VER "  AS V15");
    p = s_put(rf_want[2], "USB ");
    p = s_put(p, !usb.up ? "OFF" : !usb.config ? "NO HOST" : usb.suspended ? "IDLE" : "HOST");
    p = s_put(p, REMOTE_USB_MAINLOOP == 1 ? "  POLL MAIN" : REMOTE_USB_MAINLOOP == 2 ? "  POLL 10K" : "  POLL 2K");
    p = s_put(rf_want[3], "POLL ");
    p = s_int(p, rf_ps[0] + rf_ps[1]);
    p = s_put(p, "/S  USB ");
    p = s_int(p, rf_ps[2] / 10u);
    s_put(p, "%");
    p = s_put(rf_want[4], "MIDI RX ");
    p = s_kbs(p, rf_ps[4]);
    p = s_put(p, " TX ");
    p = s_kbs(p, rf_ps[5]);
    s_put(p, " KB/S");
    p = s_put(rf_want[5], "BULK RX ");
    p = s_kbs(p, rf_ps[6]);
    p = s_put(p, " TX ");
    p = s_kbs(p, rf_ps[7]);
    s_put(p, " KB/S");
    p = s_put(rf_want[6], "ERRORS MIDI ");
    p = s_int(p, rf_errs(0));
    p = s_put(p, "  BULK ");
    s_int(p, rf_errs(1));
    for (k = 0; k < 2u; k++) {
        const uint32_t *b = rf_blit_last[k];
        p = s_put(rf_want[7 + k], "BLIT ");
        if (!b[0]) {
            s_put(p, "-");
            continue;
        }
        p = s_int(p, 60u / (b[0] + 1u));
        p = s_put(p, "M F");
        p = s_ms(p, b[1]);
        p = s_put(p, " T");
        p = s_ms(p, b[2]);
        p = s_put(p, " S");
        s_ms(p, b[3]);
    }
    s_put(rf_want[9], loader_state >= 0 ? "FM-1_015  LOADER OK" :
                      loader_state == -2 ? "UNKNOWN FLASH CHIP" : "LOADER NOT STAGED");
}

static void rf_screen_step(void)
{
    static uint32_t next_ms;
    uint32_t i;
    if ((int32_t)(fm1_ms - rf_scr_hold) < 0)
        return;                                         /* a host's tiles or the blit bench are on the panel */
    if (lcd_busy && !fm1_lcd_done())
        return;
    if (rf_scr_full) {
        rf_scr_full = 0;
        lcd_fill(0, 0, 240, 240, C_BLACK);              /* (~80 ms, rare: the ISR serves USB meanwhile) */
        for (i = 0; i < RF_NL; i++)
            rf_line[i][0] = 0;
        rf_lines();
        return;
    }
    if (fm1_ms - next_ms < 250u && next_ms)
        goto draw;
    next_ms = fm1_ms;
    rf_lines();
draw:
    for (i = 0; i < RF_NL; i++) {
        uint16_t c = i == 0u ? C_ACC : i == 1u ? C_OFF : C_WHITE;
        if (str_eq(rf_line[i], rf_want[i]))
            continue;
        if (i == 6u && (rf_errs(0) || rf_errs(1)))
            c = C_BAD;
        if (i == 9u)
            c = loader_state >= 0 ? C_OFF : C_BAD;
        if (i == 2u && usb.config)
            c = C_OK;
        str_cpy(rf_line[i], rf_want[i], sizeof rf_line[i]);
        cv_begin(240, i ? 16u : 32u, C_BLACK);
        cv_text(i ? 4 : (240 - text_w(&FONT_L, rf_line[i])) / 2, 0, i ? &FONT_S : &FONT_L, rf_line[i], c);
        cv_blit(0, RF_LINE_Y[i]);                       /* left running */
        return;                                         /* one line per pass */
    }
}

static void rf_second_step(void)
{
    static uint32_t sec, pm, pi, tm, ti, lp, rx[2], tx[2];
    static uint32_t loops;
    uint32_t a, b;
    loops++;
    if (fm1_ms - sec < 1000u)
        return;
    sec = fm1_ms;
    a = rf_polls_main;
    b = rf_polls_isr;
    rf_ps[0] = a - pm;
    rf_ps[1] = b - pi;
    pm = a;
    pi = b;
    a = rf_ticks_main;
    b = rf_ticks_isr;
    rf_ps[2] = ((a - tm) + (b - ti)) / (1000u * FM1_TICKS_PER_US);   /* permille of a second */
    tm = a;
    ti = b;
    rf_ps[3] = loops - lp;
    lp = loops;
    rf_ps[4] = rf_raw_rx[0] - rx[0];
    rf_ps[5] = rf_raw_tx[0] - tx[0];
    rf_ps[6] = rf_raw_rx[1] - rx[1];
    rf_ps[7] = rf_raw_tx[1] - tx[1];
    rx[0] = rf_raw_rx[0];
    rx[1] = rf_raw_rx[1];
    tx[0] = rf_raw_tx[0];
    tx[1] = rf_raw_tx[1];
}

static void app_main(void)
{
    uint32_t pp = 0xFFFFFFFFu;
    fm1_adc_init();
    rf_body_init(&rf_sx.b, &rf_st[RF_T_SX], rf_sx_buf, sizeof rf_sx_buf, rf_on_msg, (void *)(uintptr_t)RF_T_SX);
    rf_body_init(&rf_bk.b, &rf_st[RF_T_BK], rf_bk_buf, sizeof rf_bk_buf, rf_on_msg, (void *)(uintptr_t)RF_T_BK);
    rf_drain_t = fm1_ticks();
    for (;;) {
        if (fm1_ms != pp) {                             /* TransitFM1's duties, once a millisecond */
            uint32_t dl = ota_deadline;
            pp = fm1_ms;
            platform_poll();                            /* (an upgrade request runs the whole session in here) */
            if (ota_deadline != dl) {
                rf_scr_full = 1;                        /* the update screen was drawn over ours */
                rf_scr_hold = fm1_ms;
            }
        }
        rf_main_poll();
        rf_rx_step();
        rf_main_poll();
        rf_src_step();
        rf_main_poll();
        rf_input_step();
        rf_screen_step();
        rf_second_step();
    }
}
