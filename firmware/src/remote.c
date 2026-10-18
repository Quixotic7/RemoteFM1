/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 M1: the FM-1 as a USB panel for a firmware that runs on a computer (PLAN.md). The protocol is
 * docs/PROTOCOL.md (version 2, proto/rfm1.h): rfm_app.c is its HAL-free layer (session, LEDS, TILES into a
 * 240 x 240 shadow with rfm_draw.c, the push with rfm_push.c, FRAME_ACK, PANEL_R, the panel as plain MIDI); this
 * file is the device under it: USB rings, the LCD, the scan, the ADC, the waiting screen, M0's diagnostics
 * (protocol section 5: SINK, SOURCE, ECHO, STATS, BLIT_BENCH, raw TILE). Everything TransitFM1 has for safety stays
 * as it is, underneath (platform.c): the boot stub, the staged own loader, step 1, the M-UPGRADE updater as
 * FM-1_015 on the port "FM-1", the boot guard.
 *
 * USB service (REMOTE_USB_MAINLOOP):
 *   0  TransitFM1's: usb_poll() from the TIMER5 ISR at 2 kHz, nothing else
 *   1  (default) the main loop polls as often as it can (rf_main_poll); the TIMER5 ISR polls at 2 kHz only while
 *      the main loop has not for 0.5 ms (an update session, the blit bench, a long decode)
 *   2  the TIMER5 ISR on every tick, 10 kHz, nothing from the main loop
 * One context touches the SIE at a time: in mode 1 the main loop raises rf_main_busy around its usb_poll() and the
 * ISR skips its poll while the flag is up; the ISR runs to its end before the main loop resumes (one core, no other
 * interrupt touches USB). usb_start (usb_retry) runs from the main loop while usb.up == 0, when usb_poll returns at
 * once, as in TransitFM1. The rings between usb_poll and the main loop have one producer and one consumer each,
 * whichever context usb_poll runs in.
 *
 * The screen push never waits for the panel: rfm_push_step starts the next SPI DMA only when the last one is over
 * (fm1_lcd_done) and returns at once otherwise, so the main loop goes on polling USB between the steps. A step costs
 * a CASET / RASET / RAMWR (a few us) and, for a rectangle narrower than the screen, a copy of at most 3,840 pixels
 * into the staging buffer. M0 measured a full frame at 51.4 ms at baud 2 (20 MHz) and per-tile windows nearly free.
 *
 * The updater: ota_service (handshakes) runs from platform_poll every millisecond; an upgrade request runs
 * ota_session() inside platform_poll, whose waits (ota_idle) poll nothing: there the ISR serves USB at 2 kHz, as in
 * TransitFM1. Its update screen is drawn over ours: afterwards the push starts again from the shadow
 * (rfm_app_lcd_lost). RemoteFM1's SysEx never holds the updater's back for long: its RX ring takes only RemoteFM1
 * SysEx (F0 7D 52 46), EP1 is held (NAK) for room only while the main loop drains it (20 ms), and RF_SXQ_RESERVE
 * events of the SysEx TX ring stay free for the updater's replies. The MIDI-in ring is drained (TransitFM1 never
 * did: a DAW's clock filled it and EP1 was NAKed for good). */
#define APP_NAME "REMOTE FM1"
#ifndef REMOTE_USB_MAINLOOP
#define REMOTE_USB_MAINLOOP 1
#endif
#define REMOTE_VENDOR 1
#define LCD_BAUD 2u                                     /* 20 MHz: 51 ms a full frame (M0) */
#define SXQ 4096u                                       /* SysEx TX events: a whole 8 KB SOURCE message fits */
#define RF_SXQ_RESERVE 512u                             /* never used by RemoteFM1's messages: the updater's replies */
#define USB_SYSEX_HOOK(b) rf_sx_hook(b)
#define USB_EP1_ROOM() rf_ep1_room()
#define USB_FRAME_DUE() rf_frame_due()
#define USB_CIN_F_SYSEX() (rf_cinf++)
#define PLATFORM_USB_TICK(sub, t0) rf_usb_tick(sub, t0)
#include <stdint.h>
static void rf_sx_hook(uint32_t b);
static int rf_ep1_room(void);
static int rf_frame_due(void);
static void rf_usb_tick(uint32_t sub, uint32_t t0);
static uint32_t rf_cinf;                                /* CIN 0xF bytes taken into a SysEx (usb.c) */
#include "platform.c"
#include "fm1_adc.h"
#include "remote_proto.h"
#include "rfm_draw.c"
#include "rfm_push.c"
#include "rfm_app.c"

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

static void rf_send_ms(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n, uint32_t ms)  /* polling USB */
{
    uint32_t t0 = fm1_ms;
    int r;
    while (!(r = rf_try(t, cmd, body, n))) {
        if (fm1_ms - t0 > ms) {
            rf_tx_drop++;
            return;
        }
        rf_main_poll();
        fm1_wdt_feed();
    }
}
static void rf_send(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n) { rf_send_ms(t, cmd, body, n, 300u); }


/* ------------------------------------------------- rfm_app.c on this device --- */
static uint16_t rf_shadow[RFM_W * RFM_H] __attribute__((section(".pool")));   /* the screen as the host believes */
#define RF_STAGE_PX (RFM_W * 16u)
static uint16_t rf_stage[RF_STAGE_PX] __attribute__((section(".pool")));    /* the push's rows of a narrow rect */
#define RF_BG_NS 1000u                                  /* the backlight level: SLOOP's LIGHTS MID (1 us a frame) */
static uint8_t rf_heard;                                /* bit t: a message came over transport t */
static uint32_t rf_tiles_m0, rf_tile_bytes, rf_src_us;
static uint32_t rf_ps[8];                               /* per second: polls main, isr, usb permille, loops, raw rx/tx */
static uint32_t rf_scr_hold;                            /* fm1_ms until which the waiting screen keeps off the panel */
static uint8_t rf_scr_full = 1;                         /* the waiting screen: draw all of it */
static uint8_t rf_pass_slow;                            /* this main loop pass ran the blit bench or a full repaint */
static uint32_t rf_loop_max;                            /* ticks: the longest main loop pass (the others) */
static uint32_t rf_blit_last[2][4];                     /* the screen's: baud, full, tiles, strips (us) */
static uint32_t rf_fb_keys, rf_fb_btns;                 /* outside a session: the keys lit as they are pressed */
static int32_t rf_batt_raw = -1;                        /* the battery ADC, smoothed */

/* rfm_app.c's messages are small; a host that stops reading a transport costs the loop at most 20 ms a message
 * (then they are dropped, counted in rf_tx_drop), never M0's 300 */
static int rfa_tx(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    rf_send_ms(t, cmd, body, n, 20u);
    return 1;
}

/* panel events: a queue in front of usb.c's MIDI ring (64 events), so a burst waits instead of being dropped */
#define RF_EVQ 256u
static uint32_t rf_evq[RF_EVQ], rf_ev_w, rf_ev_r, rf_ev_drop;
static void rfa_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (rf_ev_w - rf_ev_r >= RF_EVQ) {
        rf_ev_drop++;
        return;
    }
    rf_evq[rf_ev_w % RF_EVQ] = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;     /* cable 0, CIN = the status */
    rf_ev_w++;
}
static void rf_ev_flush(void)
{
    if (!usb.config) {
        rf_ev_r = rf_ev_w;                              /* nobody listens */
        return;
    }
    while (rf_ev_r != rf_ev_w && mo_w - mo_r < MQ) {
        midi_out_event(rf_evq[rf_ev_r % RF_EVQ]);
        rf_ev_r++;
    }
}

static void rfa_leds(const uint8_t *lit, const uint8_t *glow, const uint8_t *bg)
{
    uint32_t c;
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = lit[c];
        fm1_led_dim[c] = glow[c];
        fm1_led_bg[c] = bg[c];
    }
    fm1_led_bg_ns = RF_BG_NS;
}

/* waiting: every key and button LED at the glow, so the panel can be found */
static void rf_waiting_leds(void)
{
    uint32_t c, r, m;
    for (c = 0; c < FM1_NCOL; c++) {
        for (m = 0, r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][c] >= 0)
                m |= 1u << r;
        fm1_led[c] = 0;
        fm1_led_bg[c] = 0;
        fm1_led_dim[c] = (uint8_t)m;
    }
    rf_fb_keys = rf_fb_btns = 0;
}

static void rfa_session_changed(uint32_t on)
{
    if (!on) {
        rf_waiting_leds();
        rf_scr_full = 1;
        rf_scr_hold = fm1_ms;
    }
}

static void rfa_hello_dev(uint8_t *h, uint32_t t)
{
    (void)t;
    h[25] = (uint8_t)((usb.config ? 1u : 0u) | 2u | (uint32_t)rf_heard << 2);
    h[26] = REMOTE_USB_MAINLOOP;
    h[27] = LCD_BAUD;
    h[28] = (uint8_t)(int8_t)loader_state;
    rf_wr16(h + 29, SXQ - RF_SXQ_RESERVE);
    rf_wr32(h + 31, VB_IN_N);
    rf_wr32(h + 35, VB_OUT_N);
    rf_wr16(h + 39, RF_SX_BODY_MAX);
    rf_wr16(h + 41, RF_BK_BODY_MAX);
}

static uint32_t rfa_us(void)                            /* (TIMER4 ticks, the remainder kept: no drift) */
{
    static uint32_t last, frac, us;
    uint32_t t = fm1_ticks(), d = t - last + frac;
    last = t;
    us += d / FM1_TICKS_PER_US;
    frac = d % FM1_TICKS_PER_US;
    return us;
}

/* the push's panel: lcd.c, never waiting for a transfer (rfm_push_step asks first) */
static int rf_lcd_idle(void *c)
{
    (void)c;
    return !lcd_busy || fm1_lcd_done();
}
static void rf_lcd_window(void *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    (void)c;
    lcd_window(x0, y0, x1, y1);
}
static void rf_lcd_data(void *c, const void *p, uint32_t n)
{
    (void)c;
    lcd_data(p, n);
}
static const rfm_lcd_ops_t RF_LCD = {rf_lcd_idle, rf_lcd_window, rf_lcd_data, 0};

/* ------------------------------------------------- M0's diagnostics --- */
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
    v[RF_ST_TILES] = rf_tiles_m0;
    v[RF_ST_TILE_BYTES] = rf_tile_bytes;
    v[RF_ST_MIDI_IN_DROPPED] = rf_midi_drop;
    v[RF_ST_LCD_TIMEOUTS] = fm1_lcd_timeouts;
    v[RF_ST_SRC_US] = rf_src_us;
    v[RF_ST_SX_RX_PS] = rf_ps[4];
    v[RF_ST_SX_TX_PS] = rf_ps[5];
    v[RF_ST_BK_RX_PS] = rf_ps[6];
    v[RF_ST_BK_TX_PS] = rf_ps[7];
    v[RF_ST_USB_CINF] = rf_cinf;
    v[RF_ST_USB_RX_BAD] = usb.rx_bad;
    v[RF_ST_LOOP_MAX_US] = rf_loop_max / FM1_TICKS_PER_US;
    v[RF_ST_FRAMES] = rfa.frames;
    v[RF_ST_DECODE_ERRS] = rfa.decode_errs;
    v[RF_ST_PUSH_US_LAST] = rfa.push_us;
    v[RF_ST_PUSH_US_MAX] = rfa.push_max_us;
    v[RF_ST_SESSIONS] = rfa.sessions;
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
    rf_tiles_m0 = rf_tile_bytes = rf_src_us = rf_tx_drop = 0;
    rf_cinf = usb.rx_bad = 0;
    rf_loop_max = 0;
    rfa.push_max_us = 0;
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


/* ---- BLIT_BENCH */
static uint16_t rf_tile2[2][256];

static void rf_lcd_wait_polling(void)                   /* the running transfer ends; USB is served meanwhile */
{
    uint32_t t0 = fm1_ticks();
    while (lcd_busy && !fm1_lcd_done() && (uint32_t)(fm1_ticks() - t0) < 100000u * FM1_TICKS_PER_US)
        rf_main_poll();
}

static uint32_t rf_us_since(uint32_t t0) { return (fm1_ticks() - t0) / FM1_TICKS_PER_US; }

/* the three patterns at each SPI1 baud asked for (lsb / (baud + 1): 4 = 12 MHz, 2 = 20 MHz), each timed to its
 * last byte: a full frame from RAM (the shadow as it is: one window, four 28,800-byte DMAs), 225 16 x 16 tiles
 * with a window each, 15 240 x 16 strips of the shadow with a window each (so the panel ends as the shadow).
 * USB: the ISR serves it (mode 1: the main loop is away). Then the push starts again from the shadow. */
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
    rf_pass_slow = 1;
    out[0] = (uint8_t)nb;
    for (k = 0; k < nb; k++) {
        uint32_t b = bauds[k] < 1u ? 1u : bauds[k] > 15u ? 15u : bauds[k], full, tiles, strips;
        fm1_wdt_feed();
        fm1_lcd_baud(b);
        t0 = fm1_ticks();
        lcd_window(0, 0, 239, 239);
        for (i = 0; i < 4u; i++)
            lcd_data((const uint8_t *)rf_shadow + i * 28800u, 28800u);
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
            lcd_data(rf_shadow + y * 16u * 240u, 7680u);
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
    rfm_app_lcd_lost();
    rf_scr_hold = fm1_ms + 3000u;
    rf_scr_full = 1;
    rf_send(t, RF_BLIT_R, out, 1u + 13u * nb);
}

/* every command rfm_app.c does not take: M0's diagnostics (PROTOCOL.md section 5) */
static void rfa_diag(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    switch (cmd) {
    case RF_SOURCE:
        rf_src_start(t, body, len);
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
        if (!rfm_app_tile_m0(body, len)) {
            rf_st[t].bad++;
            break;
        }
        rf_tiles_m0++;
        rf_tile_bytes += len - 4u;
        rf_scr_hold = fm1_ms + 3000u;
        if (!rfa.session)
            rf_scr_full = 1;                            /* the waiting screen comes back whole after it */
        break;
    default:
        rf_st[t].bad++;
        break;
    }
}

static void rf_on_msg(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    uint32_t t = (uint32_t)(uintptr_t)ctx;
    rf_heard |= (uint8_t)(1u << t);
    if (cmd == RF_HELLO && rf_src.on)
        rf_src_done(1);                                 /* HELLO stops a SOURCE (M0) */
    rfm_app_msg(t, cmd, body, len);
}

/* ---- the main loop drains both rings through the parsers (a bounded piece each time) */
static uint8_t rf_sx_buf[RF_SX_BODY_MAX];
static uint8_t rf_bk_buf[RFM_BK_BODY_MAX] __attribute__((section(".pool")));

static void rf_rx_step(void)
{
    uint32_t r, n, i, got = 0;
    rf_drain_t = fm1_ticks();
    rf_sx.b.now_ms = rf_bk.b.now_ms = fm1_ms;
    r = mrx_r;
    n = mrx_w - r;
    if (n > 512u)
        n = 512u;
    got += n;
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
    got += n;
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
    if (got)
        rfm_app_activity();                             /* (SINK too: the keepalive counts every byte) */
}

/* ---- once a millisecond: the panel to rfm_app.c (keys, buttons, encoders; MASTER every 5 ms), the battery,
 * the USB host gone */
static void rf_ms_step(void)
{
    static uint32_t adc_ms, batt_ms, resets;
    int32_t steps[RFM_NENC], master = -1;
    uint32_t e, keys = fm1_in.notes, btns = fm1_in.buttons, ch, i;
    for (e = 0; e < RFM_NENC; e++)
        steps[e] = fm1_enc_take(e);
    if (fm1_ms - adc_ms >= RFA_MASTER_MS) {
        adc_ms = fm1_ms;
        master = fm1_adc_read(FM1_ADC_MASTER);
    }
    if (!rfa.session) {                                 /* waiting: a pressed key lights */
        keys &= 0x7FFFFFFu;
        btns &= 0x3FFFu;
        for (ch = keys ^ rf_fb_keys, i = 0; ch; ch >>= 1, i++)
            if (ch & 1u)
                fm1_led_key(14u + i, (int)((keys >> i) & 1u));
        for (ch = btns ^ rf_fb_btns, i = 0; ch; ch >>= 1, i++)
            if (ch & 1u)
                fm1_led_key(i, (int)((btns >> i) & 1u));
        rf_fb_keys = keys;
        rf_fb_btns = btns;
    }
    rfm_app_panel(keys, btns, steps, master);
    rf_ev_flush();
    if (fm1_ms - batt_ms >= 250u) {
        int32_t b;
        batt_ms = fm1_ms;
        b = fm1_adc_read(FM1_ADC_BATT);
        if (b > 0)
            rf_batt_raw = rf_batt_raw < 0 ? b : rf_batt_raw + (b - rf_batt_raw) / 8;
    }
    if (rfa.session && (usb.resets != resets || !usb.config || usb.suspended))
        rfm_app_usb_lost();                             /* a bus reset, or the host went away / asleep */
    resets = usb.resets;
}

/* ---- the waiting screen: a line at a time, only when the panel is free, left running (USB goes on) */
#define RF_NL 9u
static char rf_line[RF_NL][32], rf_want[RF_NL][32];
static const uint8_t RF_LINE_Y[RF_NL] = {6, 44, 76, 100, 120, 156, 176, 196, 220};

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

static uint32_t rf_errs(uint32_t t)
{
    const rf_tstats_t *s = &rf_st[t];
    return s->gaps + s->chk + s->bad + (t ? s->sync + s->frame_gaps : rf_mrx_ovf);
}

/* the battery as ChoralRoot's Options page shows it (smoothed ADC, thresholds 531 / 561 / 591): 0..3, -1 unknown */
static int32_t rf_batt_level(void)
{
    if (rf_batt_raw < 0)
        return -1;
    return rf_batt_raw >= 591 ? 3 : rf_batt_raw >= 561 ? 2 : rf_batt_raw >= 531 ? 1 : 0;
}

static void rf_lines(void)
{
    char *p;
    int32_t bl = rf_batt_level();
    s_put(rf_want[0], "REMOTE FM1");
    s_put(rf_want[1], "v" APP_VER "   AS V15 (" FELUCCA_ID ")");
    s_put(rf_want[2], "WAITING FOR HOST");
    p = s_put(rf_want[3], "USB ");
    s_put(p, !usb.up ? "OFF" : !usb.config ? "NO HOST" : usb.suspended ? "IDLE" : "HOST CONNECTED");
    if (bl < 0) {
        rf_want[4][0] = 0;
    } else {
        p = s_put(rf_want[4], "BATTERY ");
        p = s_int(p, (uint32_t)bl);
        s_put(p, "/3");
    }
    p = s_put(rf_want[5], "MIDI RX ");
    p = s_kbs(p, rf_ps[4]);
    p = s_put(p, " TX ");
    p = s_kbs(p, rf_ps[5]);
    s_put(p, " KB/S");
    p = s_put(rf_want[6], "BULK RX ");
    p = s_kbs(p, rf_ps[6]);
    p = s_put(p, " TX ");
    p = s_kbs(p, rf_ps[7]);
    s_put(p, " KB/S");
    p = s_put(rf_want[7], "ERRORS MIDI ");
    p = s_int(p, rf_errs(0));
    p = s_put(p, "  BULK ");
    s_int(p, rf_errs(1));
    s_put(rf_want[8], loader_state >= 0 ? "UPDATES OK (LOADER STAGED)" :
                      loader_state == -2 ? "UNKNOWN FLASH CHIP" : "LOADER NOT STAGED");
}

static void rf_screen_step(void)
{
    static uint32_t next_ms;
    uint32_t i;
    if (rfa.session || rfm_app_pushing() || (int32_t)(fm1_ms - rf_scr_hold) < 0)
        return;                                         /* the host's screen, M0 tiles or the blit bench */
    if (lcd_busy && !fm1_lcd_done())
        return;
    if (rf_scr_full) {
        rf_scr_full = 0;
        rf_pass_slow = 1;
        lcd_fill(0, 0, 240, 240, C_BLACK);              /* (~50 ms, rare: the ISR serves USB meanwhile) */
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
        uint16_t c = i == 0u ? C_ACC : i == 1u || i >= 5u ? C_OFF : C_WHITE;
        if (str_eq(rf_line[i], rf_want[i]))
            continue;
        if (i == 7u && (rf_errs(0) || rf_errs(1)))
            c = C_BAD;
        if (i == 8u)
            c = loader_state >= 0 ? C_OFF : C_BAD;
        if (i == 3u && usb.config && !usb.suspended)
            c = C_OK;
        if (i == 4u && rf_batt_level() == 0)
            c = C_BAD;
        str_cpy(rf_line[i], rf_want[i], sizeof rf_line[i]);
        cv_begin(240, i ? 16u : 32u, C_BLACK);
        cv_text((240 - text_w(i ? &FONT_S : &FONT_L, rf_line[i])) / 2, 0, i ? &FONT_S : &FONT_L, rf_line[i], c);
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
    rfm_app_init(rf_shadow, rf_stage, RF_STAGE_PX, &RF_LCD);
    rf_waiting_leds();
    rf_drain_t = fm1_ticks();
    for (;;) {
        uint32_t t0;
        if (fm1_ms != pp) {                             /* TransitFM1's duties, once a millisecond */
            uint32_t dl = ota_deadline;
            pp = fm1_ms;
            platform_poll();                            /* (an upgrade request runs the whole session in here) */
            if (ota_deadline != dl) {
                rfm_app_lcd_lost();                     /* the update screen was drawn over ours */
                rf_scr_full = 1;
                rf_scr_hold = fm1_ms;
            }
            rf_ms_step();
        }
        t0 = fm1_ticks();
        rf_pass_slow = 0;
        rf_main_poll();
        rf_rx_step();
        rf_main_poll();
        rfm_app_poll(fm1_ms);
        rf_main_poll();
        rf_src_step();
        rf_main_poll();
        rf_screen_step();
        rf_second_step();
        t0 = fm1_ticks() - t0;
        if (!rf_pass_slow && t0 > rf_loop_max)
            rf_loop_max = t0;
    }
}
