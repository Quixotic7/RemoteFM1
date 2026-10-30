/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1's app on fm1-base (app.c): its transports, the panel, the LEDs, M0's diagnostics and the contract with the
 * platform, around the protocol layer (rfm_app.c, docs/PROTOCOL.md) and the standalone instrument (sa/sa_main.c:
 * standalone at boot, panel mode from a host's HELLO to its session's end). What only the FM-1 has
 * (felucca-bsp usb.c's rings, the panel's DMA) is rfm_dev.c; the host simulator's stand-ins are rfm_host.c.
 *
 * Transports. A message comes in on one of three, and its answer goes back on the same one:
 *   RF_T_SX   SysEx over USB-MIDI. usb.c hands every SysEx byte to rf_sx_tap (usb_poll's context); RemoteFM1's own
 *             (F0 7D 52 46 ...) go to the ring rf_mrx and are claimed, so the updater's frame slot (ota.c, 640 B, one
 *             at a time) never holds one. Everything else (the update's F0 00 59, the soft keys, a screen reader's)
 *             goes its way as before. Out: whole messages into usb.c's SysEx ring (sx_out_q), RF_SXQ_RESERVE
 *             packets of it always left to the updater.
 *   RF_T_BK   the vendor interface's bulk pair (EP3, felucca-bsp patched: firmware/patches/felucca-bsp).
 *   RF_T_BLE  SysEx over Bluetooth MIDI (fm1-base patched to carry SysEx: firmware/patches/fm1-base): in through
 *             plat_midi_in as USB-MIDI packets on PORT_BLE, reassembled by its own parser; out with plat_midi_send to
 *             the computer's link, a whole message only when plat_midi_out_room has room for it, else held in a short
 *             queue (rf_bp) and sent when there is, never waited for: a slow link never stops the loop.
 * A session lives on the transport it started on (rfm_app.c rfa.st): its PANEL_Rs go there, and the panel's events go
 * to that port as plain MIDI (USB-MIDI for SysEx and bulk sessions, Bluetooth for a Bluetooth one); in panel mode with
 * no session, to USB and to a Bluetooth computer that listens. Standalone, the panel's events are not sent: the queue
 * carries the instrument's notes instead (sa_main.c), to USB and Bluetooth, and MIDI coming in plays it. A USB reset or unplug ends a USB session, a Bluetooth disconnect a
 * Bluetooth one. Another transport is a transport like this one: its parser (rf_sx_t), a case in rf_try, a bit in
 * rfa_hello_dev (an RTP-MIDI one for the WiFi build: rf_sx_t on its SysEx, rf_try's encoder into its packets).
 *
 * USB service. fm1-base polls USB from its 2 kHz timer: one 64-byte packet per endpoint each time, 128 KB/s at most.
 * RemoteFM1 polls it from the main loop too, as often as it can (rp_service, interrupts off around it, so the
 * timer's poll and ours never meet): app_frame and app_input_only each spin for RF_SLICE_US, polling USB, draining
 * the rings into the parsers, stepping the push and Bluetooth's tasks, and return to fm1-base's loop (MASTER, the
 * updater, the watchdog) in between. The MIDI-in ring is emptied in usb_poll's context before every EP1 packet
 * (rf_ep1_room: nothing here plays USB MIDI in), so a DAW's clock can never fill it and shut the updater out, even
 * while an update session keeps the main loop away.
 *
 * Screen. The push (rfm_push.c) never waits: it blits a piece only when lcd_idle() says the last DMA is over. With
 * RFM_SHADOW 0 (no 115,200 B shadow) rectangles go to the panel row by row as they are decoded (rf_row). The standalone
 * screens and the hand-over screen (sa/sa_ui.c, a band at a time, never waiting either) draw only while no session or
 * push owns the panel (rf_panel_ours). A session that holds frames (TILES'
 * HOLD, 0.2.2) has its transport's input left in its ring while a push runs or is due (rfm_app_rx_wait): a byte at a
 * time on USB SysEx, a packet at a time on Bluetooth, a 256-byte piece at a time on bulk. */

#define RF_T_BLE 2u                         /* SysEx over Bluetooth MIDI (remote_proto.h: RF_T_SX 0, RF_T_BK 1) */
#define RF_NT 3u
#define RF_SXQ_RESERVE 64u                  /* USB SysEx send packets RemoteFM1 never takes: the updater's replies */
#define RF_SLICE_US 2000u                   /* a turn of the app's loop before fm1-base's main loop has its own */
/* the app's buffers in the pool (bytes), each smaller for a build short of RAM (the WiFi one), HELLO_R saying so */
#ifndef RF_MRX_N
#define RF_MRX_N 16384u                     /* USB SysEx in, RemoteFM1's (a power of two) */
#endif
#ifndef RF_BK_RX_MAX
#define RF_BK_RX_MAX RFM_BK_BODY_MAX        /* the longest bulk body taken (HELLO_R bk_body_max: the host keeps to it) */
#endif
#ifndef RF_GEN_MAX
#define RF_GEN_MAX RF_BK_SRC_MAX            /* M0's SOURCE: the longest DATA message made */
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_5200"              /* (the host simulator; tools/build.py gives the FM-1's build its own) */
#endif
#define RFA_FW_VER APP_VERSION
#define RFA_IDENTITY FELUCCA_ID
#if RFM_SHADOW
#define RFA_CAPS (RFM_CAP_SHADOW | RFM_CAP_COPY | RFM_CAP_RLE | RFM_CAP_PAL | RFM_CAP_PLAY_GREEN | RFM_CAP_HOLD | \
                  RFM_CAP_BLE_FAST)        /* (the radio's settings: fm1.toml ble_dle, ble_2m, ble_rx_bufs) */
#else
#define RFA_CAPS (RFM_CAP_RLE | RFM_CAP_PAL | RFM_CAP_PLAY_GREEN)
#endif
#include "rfm_app.c"

/* ------------------------------------------------------------------ the port (rfm_dev.c / rfm_host.c) --- */
static void rp_service(void);                       /* USB served now (main loop) */
static int rp_usb_cfg(void);                        /* a USB host configured the FM-1 */
static int rp_usb_live(void);                       /* ... and its bus runs (not suspended, not unplugged) */
static uint32_t rp_usb_resets(void);
static uint32_t rp_sx_room(void);                   /* free packets in USB's SysEx send ring */
static void rp_sx_begin(void);                      /* a whole message: begin, put each packet, end (published) */
static void rp_sx_put(uint32_t pkt);
static void rp_sx_end(void);
static uint32_t rp_bk_avail(void);                  /* the vendor bulk pair: bytes waiting, taken, room, a message */
static uint32_t rp_bk_read(uint8_t *dst, uint32_t n);
static uint32_t rp_bk_room(void);
static int rp_bk_send(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb);
static void rp_ble_put(uint32_t pkt);               /* a packet to the Bluetooth computer (room asked first) */
static void rp_lcd_baud(uint32_t b);                /* the panel's SPI clock (BLIT_BENCH) */
static void rp_wdt(void);                           /* the watchdog fed (inside the long waits: BLIT_BENCH, a send) */
static uint32_t rp_bk_rx_n(void), rp_bk_tx_n(void); /* the vendor rings' sizes (HELLO_R) */
enum { RC_RESETS, RC_SETUPS, RC_EP1_RX, RC_EP1_TX, RC_EP3_RX, RC_EP3_TX, RC_EP3_HELD, RC_LCD_TIMEOUTS, RC_N };
static void rp_counters(uint32_t *c);               /* USB's and the panel's counters (STATS_R) */
#ifdef PLAT_HOST
/* felucca-bsp boards/fm1/fm1_input.h FM1_KEYMAP (Leo Kuroshita, Hügelton Instruments, MPL-2.0): the matrix id at
 * (packed row bit, column), -1 none; the host simulator has no BSP */
static const int8_t RF_KEYMAP[6][RFM_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, {5, 11, 4, 10, 3, 9, 2, 8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13, 7, 6, 12},   {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    {0, 1, 15, 14, 17, 16, 19, 18, 20, 21, 22},     {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
#else
#define RF_KEYMAP FM1_KEYMAP
_Static_assert(RFM_NCOL == FM1_NCOL, "the LEDS arrays are the matrix's columns");
#endif

/* the panel by matrix numbers, as the protocol has it (emu_hal_t), from plat.h's names: fm1-base's default panel map
 * (device/panel.c PANEL_DEFAULT) turned round, so a unit calibrated otherwise still reports the standard numbers */
static const uint8_t RF_LABEL_OF_BTN[RFM_NBTN] = {B_OCTDN, B_OCTUP, B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO,
                                                  B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC};
static const uint8_t RF_ENC_OF_ROLE[NE] = {0, 1, 6, 2, 3, 4, 5};   /* EN_SELECT .. EN_K4 -> matrix encoder */
#define RF_GREEN_COL 8u                     /* PLAY's green LED: column 8, row bit 1 (rfm_app.c RFA_GREEN_*) */
#define RF_GREEN_BIT 0x02u

static uint32_t rf_ticks(void) { return plat_ticks24(); }
#ifndef RING_PUBLISH
#define RING_PUBLISH() ((void)0)                    /* (the simulator: one thread; the FM-1's is unity.c's barrier) */
#endif
#ifdef PLAT_HOST
static uint32_t rf_cinf;                            /* (the FM-1's: app.h, counted in usb.c) */
static void hs_init(void);
#endif

/* ------------------------------------------------------------------ USB SysEx in: the tap --- */
static uint8_t rf_mrx[RF_MRX_N] PLAT_POOL;          /* RemoteFM1's SysEx from USB, F0 .. F7, as it came */
static volatile uint32_t mrx_w, mrx_r;
static volatile uint32_t rf_drain_t;                /* ticks: the main loop last drained the rings */
static uint32_t rf_mrx_ovf, rf_sx_held, rf_midi_drop;
static uint8_t rfh_st;                              /* 0 idle, 1..3 the ID so far, 4 ours, 5 not ours */

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

/* every SysEx byte from USB-MIDI, and a status that ends one (usb.c, usb_poll's context; never a real-time byte):
 * 1 while the message is RemoteFM1's (F0 7D 52 46 ...), which usb.c then leaves out of the updater's frame */
static int rf_sx_tap(uint32_t b)
{
    static const uint8_t ID[3] = {RF_SX_ID0, RF_SX_ID1, RF_SX_ID2};
    if (sa_standalone())
        sa_ub_byte(b);                              /* a DX7 bank dump (sa/sa_ubank.c) */
    if (b == 0xF0u) {
        if (rfh_st == 4u)
            rf_mrx_put(0xF0u);                      /* ours, cut off: the parser counts it */
        rfh_st = 1;
        return 0;
    }
    if (b & 0x80u) {                                /* F7, or a status that ends the SysEx */
        int ours = rfh_st == 4u;
        if (ours)
            rf_mrx_put(b);
        rfh_st = 0;
        return ours;
    }
    if (rfh_st >= 1u && rfh_st <= 3u) {
        if (b != ID[rfh_st - 1u]) {
            rfh_st = 5;
        } else if (++rfh_st == 4u) {
            rf_mrx_put(0xF0u);
            rf_mrx_put(RF_SX_ID0);
            rf_mrx_put(RF_SX_ID1);
            rf_mrx_put(RF_SX_ID2);
            return 1;
        }
        return 0;
    }
    if (rfh_st == 4u)
        rf_mrx_put(b);
    return rfh_st == 4u;
}

#ifndef PLAT_HOST
/* before an EP1 OUT packet is taken (usb_poll's context): in panel mode the MIDI-in ring emptied (a panel plays no USB
 * MIDI in: a DAW's clock never fills it, so EP1, the updater's way in, is never held for it); standalone, the ring is
 * the instrument's (rf_rx_step reads it every turn of the main loop), emptied only when the main loop has been away for
 * 20 ms (an update session). Room for a packet's SysEx (48 bytes) in rf_mrx, unless the main loop has been away for
 * 20 ms: then what does not fit is dropped (counted) and the updater's SysEx goes on */
static int rf_ep1_room(void)
{
    if (mi_w != mi_r && (!sa_standalone() || (uint32_t)(rf_ticks() - rf_drain_t) > 20000u * PLAT_TICKS_PER_US)) {
        rf_midi_drop += mi_w - mi_r;
        mi_r = mi_w;
    }
    if (RF_MRX_N - (mrx_w - mrx_r) >= 64u)
        return 1;
    if ((uint32_t)(rf_ticks() - rf_drain_t) > 20000u * PLAT_TICKS_PER_US)
        return 1;
    rf_sx_held++;
    return 0;
}

static int rf_frame_due(void)                       /* usb_poll's context: the 10 ms frame-number check */
{
    static uint32_t last;
    uint32_t now = plat_ms();
    if (now - last < 10u)
        return 0;
    last = now;
    return 1;
}

static uint32_t rf_usb_ms(void) { return plat_ms(); }   /* usb_poll's context: the audio input's 20 ms stall check */
#endif

/* ------------------------------------------------------------------ out --- */
static rf_tstats_t rf_st[RF_NT];
static uint32_t rf_raw_rx[RF_NT], rf_raw_tx[RF_NT], rf_tx_drop[RF_NT];
static uint32_t rf_bk_seq;
static uint8_t rf_heard;                            /* bit t: a message came over transport t */

static uint32_t rf_sx_packets(uint32_t n) { return (6u + rf_packed_len(n) + 2u) / 3u; }
static uint32_t rf_ble_room(void) { return plat_midi_out_room(TO_BLE_HOST); }

static int rf_up(uint32_t t) { return t == RF_T_BLE ? plat_ble_connected() : rp_usb_cfg(); }

static int rf_room(uint32_t t, uint32_t n)          /* a message with an n-byte body fits now */
{
    if (t == RF_T_BK)
        return rp_bk_room() >= RF_BK_HDR + n;
    if (t == RF_T_BLE)
        return rf_ble_room() >= rf_sx_packets(n);
    return rp_sx_room() >= rf_sx_packets(n) + RF_SXQ_RESERVE;
}

/* the largest body a message on t can ever have (its send ring, whole) */
static uint32_t rf_body_cap(uint32_t t)
{
    uint32_t pk = t == RF_T_BLE ? BLE_OUT_Q : SXQ - RF_SXQ_RESERVE, wire;
    if (t == RF_T_BK)
        return rp_bk_tx_n() - RF_BK_HDR;
    wire = 3u * pk - 6u;                            /* F0 7D 52 46 cmd .. F7 */
    return wire / 8u * 7u + (wire % 8u ? wire % 8u - 1u : 0u);
}

/* SysEx bytes into USB-MIDI event packets (CIN 4 goes on, 5 / 6 / 7 ends with 1 / 2 / 3 bytes), to transport t */
typedef struct {
    uint32_t t, k;
    uint8_t a[3];
} rf_em_t;
static void rf_em_pkt(const rf_em_t *e, uint32_t pkt)
{
    if (e->t == RF_T_BLE)
        rp_ble_put(pkt);
    else
        rp_sx_put(pkt);
}
static void rf_em_put(rf_em_t *e, uint32_t b)
{
    if (e->k == 3u) {
        rf_em_pkt(e, 4u | (uint32_t)e->a[0] << 8 | (uint32_t)e->a[1] << 16 | (uint32_t)e->a[2] << 24);
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
    rf_em_pkt(e, p);
}

/* 1 sent, 0 no room now, -1 nobody there. A whole message goes in at once: usb.c sends the SysEx ring in order and
 * the updater's writer (ota_wire_send) runs in the main loop too, never inside one of ours; Bluetooth's queue is
 * drained by its tasks, which run between our messages */
static int rf_try(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    uint32_t i, k;
    if (!rf_up(t))
        return -1;
    if (!rf_room(t, n))
        return 0;
    if (t == RF_T_BK) {
        uint8_t h[RF_BK_HDR];
        rf_bk_header(h, cmd, n, rf_bk_seq);
        if (rp_bk_send(h, RF_BK_HDR, body, n) != 1)
            return 0;
        rf_bk_seq++;
        rf_raw_tx[t] += RF_BK_HDR + n;
    } else {
        rf_em_t e;
        e.t = t;
        e.k = 0;
        if (t != RF_T_BLE)
            rp_sx_begin();
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
        if (t != RF_T_BLE)
            rp_sx_end();
        rf_raw_tx[t] += 6u + rf_packed_len(n);
    }
    rf_st[t].tx_msgs++;
    return 1;
}

/* Bluetooth: what found no room waits here (in order) for the next turn, never for the link */
#define RF_BP_N 6u
#define RF_BP_MAX 72u                               /* HELLO_R (67) the longest that waits; longer ones are dropped */
static struct {
    uint8_t cmd, n, b[RF_BP_MAX];
} rf_bp[RF_BP_N];
static uint32_t rf_bp_h, rf_bp_n;
static void rf_bp_flush(void)
{
    while (rf_bp_n) {
        int r = rf_try(RF_T_BLE, rf_bp[rf_bp_h].cmd, rf_bp[rf_bp_h].b, rf_bp[rf_bp_h].n);
        if (!r)
            return;                                 /* no room yet */
        if (r < 0)
            rf_tx_drop[RF_T_BLE] += rf_bp_n;        /* the link is gone */
        rf_bp_h = (rf_bp_h + (r < 0 ? rf_bp_n : 1u)) % RF_BP_N;
        rf_bp_n = r < 0 ? 0u : rf_bp_n - 1u;
    }
}
static void rf_bp_add(uint32_t cmd, const uint8_t *body, uint32_t n)
{
    uint32_t i, k;
    if (rf_bp_n == RF_BP_N || n > RF_BP_MAX) {
        rf_tx_drop[RF_T_BLE]++;
        return;
    }
    k = (rf_bp_h + rf_bp_n++) % RF_BP_N;
    rf_bp[k].cmd = (uint8_t)cmd;
    rf_bp[k].n = (uint8_t)n;
    for (i = 0; i < n; i++)
        rf_bp[k].b[i] = body[i];
}

/* a message on t: over USB, waiting up to ms for room (USB served meanwhile) and then dropped (counted); over
 * Bluetooth, behind those still waiting, never waited for */
static void rf_send_ms(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n, uint32_t ms)
{
    uint32_t t0 = plat_ms();
    int r = 0;
    if (t == RF_T_BLE) {
        rf_bp_flush();
        if (rf_bp_n || !(r = rf_try(t, cmd, body, n)))
            rf_bp_add(cmd, body, n);
        else if (r < 0)
            rf_tx_drop[t]++;
        return;
    }
    while (!(r = rf_try(t, cmd, body, n))) {
        if (plat_ms() - t0 > ms) {
            rf_tx_drop[t]++;
            return;
        }
        rp_service();
        rp_wdt();
#ifdef PLAT_HOST
        rf_tx_drop[t]++;                            /* (the simulator's rings never fill) */
        return;
#endif
    }
    if (r < 0)
        rf_tx_drop[t]++;
}
static void rf_send(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n) { rf_send_ms(t, cmd, body, n, 300u); }

/* ------------------------------------------------------------------ rfm_app.c on fm1-base --- */
#if RFM_SHADOW
static uint16_t rf_shadow[RFM_W * RFM_H] PLAT_POOL;     /* the screen as the host believes it */
#endif
#define RF_STAGE_PX (RFM_W * 16u)
static uint16_t rf_stage[RF_STAGE_PX] PLAT_POOL;        /* the push's rows of a narrow rectangle */
static uint32_t rf_tiles_m0, rf_tile_bytes, rf_src_us;
static uint32_t rf_ps[8];                               /* per second: polls main, (isr), usb permille, loops, raw rx/tx */
static uint32_t rf_polls, rf_poll_ticks, rf_loops;
static uint32_t rf_scr_hold;                            /* plat_ms until which the waiting screen keeps off the panel */
static uint8_t rf_scr_full = 1;                         /* the waiting screen: draw all of it */
static uint8_t rf_pass_slow;                            /* this turn ran the blit bench or a full repaint */
static uint32_t rf_loop_max;                            /* ticks: the longest turn of the loop (the others) */
static uint32_t rf_wait_keys, rf_wait_btns = 0xFFFFFFFFu;   /* outside a session: the keys lit as they are pressed */

/* rfm_app.c's messages are small; a USB host that stops reading costs the loop at most 20 ms a message (then they
 * are dropped, counted in rf_tx_drop), never M0's 300 */
static int rfa_tx(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    rf_send_ms(t, cmd, body, n, 20u);
    return 1;
}

/* the panel's events: a queue in front of the MIDI rings, so a burst waits instead of being dropped; to the session's
 * port, or with no session to USB and a Bluetooth computer that listens */
#define RF_EVQ 256u
static uint32_t rf_evq[RF_EVQ], rf_ev_w, rf_ev_r, rf_ev_drop;
static void rf_ev_put(uint32_t pkt)                     /* (the standalone instrument's notes too: sa_main.c) */
{
    if (rf_ev_w - rf_ev_r >= RF_EVQ) {
        rf_ev_drop++;
        return;
    }
    rf_evq[rf_ev_w % RF_EVQ] = pkt;
    rf_ev_w++;
}
static void rfa_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (sa_standalone())
        return;                                         /* (standalone: the keys are the instrument's) */
    rf_ev_put((st >> 4) | st << 8 | d1 << 16 | d2 << 24);   /* cable 0, CIN = the status */
}
static void rf_ev_flush(void)
{
    uint32_t to;
    if (rfa.session)
        to = rfa.st == RF_T_BLE ? (plat_ble_connected() ? TO_BLE_HOST : 0u) : (rp_usb_cfg() ? TO_USB : 0u);
    else
        to = (rp_usb_cfg() ? TO_USB : 0u) | (plat_ble_connected() ? TO_BLE_HOST : 0u);
    if (!to) {
        rf_ev_r = rf_ev_w;                              /* nobody listens */
        return;
    }
    if ((to & TO_BLE_HOST) && rf_bp_n)
        return;                                         /* (Bluetooth's waiting replies first) */
    while (rf_ev_r != rf_ev_w && plat_midi_out_room(to) > 0u) {
        plat_midi_send(rf_evq[rf_ev_r % RF_EVQ], to);
        rf_ev_r++;
    }
}

/* LEDS (rfm_app.c: lit; mid and dim; faint, the matrix's packed rows) onto fm1-base's three levels: lit; mid, the
 * steady ~12 % (with the glow under it, as felucca-bsp wants it); faint, the glow (DIM HI, ~3 %). HELLO_R's caps say
 * "no five levels": a host's lit / dim / faint are lit / mid / glow here, its mid shown as its dim */
static void rf_leds_of(const uint8_t *m, uint32_t *btn, uint32_t *keys)   /* matrix rows -> plat.h's buttons, keys */
{
    uint32_t c, r;
    *btn = *keys = 0;
    for (c = 0; c < RFM_NCOL; c++)
        for (r = 1; r < 5u; r++) {
            int id;
            if (!(m[c] >> r & 1u))
                continue;
            id = RF_KEYMAP[r][c];
            if (id >= 14)
                *keys |= 1u << (id - 14);
            else if (id >= 0)
                *btn |= 1u << RF_LABEL_OF_BTN[id];
            else if (c == RF_GREEN_COL && (1u << r) == RF_GREEN_BIT)
                *btn |= LED_PLAY_GREEN;
        }
}
static uint32_t rf_led_lit_b, rf_led_lit_k, rf_led_mid_k, rf_led_glow_k, rf_led_root_k;   /* (the simulator's checks) */
static void rfa_leds(const uint8_t *lit, const uint8_t *glow, const uint8_t *bg)
{
    uint32_t lb, lk, mb, mk, fb, fk;
    rf_leds_of(lit, &lb, &lk);
    rf_leds_of(glow, &mb, &mk);
    rf_leds_of(bg, &fb, &fk);
    mb &= ~lb;
    mk &= ~lk;
    plat_lights(lb, lk, (mb | fb) & ~lb, (mk | fk) & ~lk, 2);
    plat_lights_mid(mb & ((1u << NB) - 1u), mk);
    plat_lights_levels(0, 0, 0, 0);                     /* (the standalone's levels off) */
    rf_led_lit_b = lb;
    rf_led_lit_k = lk;
    rf_led_mid_k = mk;
    rf_led_glow_k = fk & ~lk & ~mk;
}

/* waiting: every key and button at the glow, so the panel can be found; a key or button lit while it is held */
static void rf_waiting_leds(uint32_t btns, uint32_t keys)
{
    if (btns == rf_wait_btns && keys == rf_wait_keys)
        return;
    rf_wait_btns = btns;
    rf_wait_keys = keys;
    plat_lights(btns, keys, (1u << NB) - 1u, (1u << NKEYS) - 1u, 2);
    plat_lights_mid(0, 0);
    plat_lights_levels(0, 0, 0, 0);
    rf_led_lit_b = btns;
    rf_led_lit_k = keys;
    rf_led_mid_k = 0;
    rf_led_glow_k = ((1u << NKEYS) - 1u) & ~keys;
}

/* standalone (sa_main.c sa_leds): the lit buttons and keys (100 %), the unlit buttons at the glow; two steady levels
 * for keys (fm1-base patch 0007, felucca-bsp 0004): keys_root at SA_LED_ROOT_8 / 8 (a scale's root: the owner asked
 * 75 %), keys_mid at SA_LED_SCALE_8 / 8 (its notes: 40 % asked, 3/8 the nearest); the mid level a host's LEDS left
 * (plat_lights_mid holds it until it is called again) cleared, or its keys would stay lit at ~12 % */
#define SA_LED_ROOT_8 6u
#define SA_LED_SCALE_8 3u
static void rf_sa_leds(uint32_t btns, uint32_t keys, uint32_t keys_root, uint32_t keys_mid)
{
    plat_glow(1);
    plat_leds(btns, keys);
    plat_lights_mid(0, 0);
    plat_lights_levels(keys_root & ~keys, SA_LED_ROOT_8, keys_mid & ~keys & ~keys_root, SA_LED_SCALE_8);
    rf_led_lit_b = btns;
    rf_led_lit_k = keys;
    rf_led_root_k = keys_root & ~keys;
    rf_led_mid_k = keys_mid & ~keys & ~keys_root;
    rf_led_glow_k = 0;
}

static void rfa_session_changed(uint32_t on)
{
    if (!on) {
        rf_wait_btns = 0xFFFFFFFFu;                     /* the waiting LEDs again */
        rf_scr_full = 1;
        rf_scr_hold = plat_ms();
    }
    sa_session(on, rfa.st);                             /* panel mode, or standalone again (sa_main.c) */
}

static int rf_session_on(void) { return rfa.session; }

/* the screen is the standalone's (or the hand-over's): no session, no push, nobody holding it (M0's tiles, the blit
 * bench); after somebody else drew on it, all of it again */
static int rf_panel_ours(void)
{
    if (rfa.session || rfm_app_pushing() || (int32_t)(plat_ms() - rf_scr_hold) < 0)
        return 0;
    if (rf_scr_full) {
        rf_scr_full = 0;
        sa_ui_lost();
    }
    return 1;
}

static int rf_flash_ok(void) { return plat_data(PLAT_DATA_LO) != 0; }   /* the expected flash part: updates work */
static void rfa_hello_dev(uint8_t *h, uint32_t t)
{
    h[25] = (uint8_t)((rp_usb_cfg() ? 1u : 0u) | 2u | (uint32_t)(rf_heard & 3u) << 2 |
                      (plat_ble_connected() ? 0x10u : 0u) | (rf_heard & 4u ? 0x20u : 0u));
    h[26] = 1;                                      /* the main loop polls USB */
#ifdef LCD_BAUD
    h[27] = LCD_BAUD;
#else
    h[27] = 0;
#endif
    h[28] = (uint8_t)(int8_t)(rf_flash_ok() ? 0 : -2);
    rf_wr16(h + 29, t == RF_T_BLE ? BLE_OUT_Q : SXQ - RF_SXQ_RESERVE);
    rf_wr32(h + 31, rp_bk_rx_n());
    rf_wr32(h + 35, rp_bk_tx_n());
    rf_wr16(h + 39, RF_SX_BODY_MAX);
    rf_wr16(h + 41, RF_BK_RX_MAX);
}

static uint32_t rfa_us(void)                        /* (the tick counter, the remainder kept: no drift) */
{
    static uint32_t last, frac, us;
    uint32_t t = rf_ticks(), d = t - last + frac;
    last = t;
    us += d / PLAT_TICKS_PER_US;
    frac = d % PLAT_TICKS_PER_US;
    return us;
}

/* the push's panel: lcd_blit of whole rows of the window, only when the last transfer is over (rfm_push_step asks) */
static struct {
    uint32_t x, y, w, h, row;
} rf_win;
static int rf_lcd_idle(void *c)
{
    (void)c;
    return lcd_idle();
}
static void rf_lcd_window(void *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    (void)c;
    rf_win.x = x0;
    rf_win.y = y0;
    rf_win.w = x1 - x0 + 1u;
    rf_win.h = y1 - y0 + 1u;
    rf_win.row = 0;
}
static void rf_lcd_data(void *c, const void *p, uint32_t n)   /* (rfm_push.c sends whole rows of the window) */
{
    uint32_t rows = n / 2u / rf_win.w;
    (void)c;
    if (!rows)
        return;
    lcd_blit(rf_win.x, rf_win.y + rf_win.row, rf_win.w, rows, (const uint16_t *)p);
    rf_win.row += rows;
}
static const rfm_lcd_ops_t RF_LCD = {rf_lcd_idle, rf_lcd_window, rf_lcd_data, 0};

/* ------------------------------------------------------------------ no shadow: rows to the panel --- */
#if !RFM_SHADOW
static void rp_wait_idle(void);
static uint16_t rf_rowbuf[RFM_W];
/* rows of the same rectangle gathered into half the stage buffer while the other half goes out */
static struct {
    uint32_t x, y, w, n, half;
} rf_rows;
static void rf_rows_flush(void)
{
    if (!rf_rows.n)
        return;
    rp_wait_idle();
    lcd_blit(rf_rows.x, rf_rows.y, rf_rows.w, rf_rows.n, rf_stage + rf_rows.half * (RF_STAGE_PX / 2u));
    rf_rows.half ^= 1u;
    rf_rows.n = 0;
}
static void rf_row(void *c, uint32_t x, uint32_t y, uint32_t w, const uint16_t *px)
{
    uint16_t *d;
    uint32_t i;
    (void)c;
    if (!rfa.session)
        return;                                     /* the waiting screen owns the panel */
    if (rf_rows.n && (x != rf_rows.x || w != rf_rows.w || y != rf_rows.y + rf_rows.n ||
                      (rf_rows.n + 1u) * w > RF_STAGE_PX / 2u))
        rf_rows_flush();
    if (!rf_rows.n) {
        rf_rows.x = x;
        rf_rows.y = y;
        rf_rows.w = w;
    }
    d = rf_stage + rf_rows.half * (RF_STAGE_PX / 2u) + rf_rows.n * w;
    for (i = 0; i < w; i++)
        d[i] = px[i];
    rf_rows.n++;
}
#endif

/* ------------------------------------------------------------------ M0's diagnostics --- */
#if FM1B_BLE && !defined(PLAT_HOST)
/* a test readout (Bluetooth radio builds, firmware/patches/fm1-base 0004): what the computer's Bluetooth link
 * negotiated, 40 bytes after STATS_R's counters (fm1-base le_midi.c ble_midi_radio: the layout; PROTOCOL.md section 5) */
#define RF_RADIO_N 40u
extern void ble_midi_radio(uint8_t *b);
#else
#define RF_RADIO_N 0u
#endif
static void rf_stats_send(uint32_t t)
{
    uint32_t v[RF_ST_N], i, k, c[RC_N];
    uint8_t b[1 + 4 * RF_ST_N + RF_RADIO_N];
    const rf_tstats_t *s;
    for (i = 0; i < RF_ST_N; i++)
        v[i] = 0;
    rp_counters(c);
    v[RF_ST_UPTIME_MS] = plat_ms();
    for (k = 0; k < 2u; k++) {
        uint32_t o = k ? RF_ST_BK_RX_MSGS - RF_ST_SX_RX_MSGS : 0u;
        s = &rf_st[t == RF_T_BLE && !k ? RF_T_BLE : k];   /* (asked over Bluetooth: its SysEx counts) */
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
    v[RF_ST_BK_OVF] = 0;                            /* bulk is held (NAK), never dropped */
    v[RF_ST_BK_HELD] = c[RC_EP3_HELD];
    v[RF_ST_BK_SYNC] = rf_st[RF_T_BK].sync;
    v[RF_ST_BK_FRAME_GAPS] = rf_st[RF_T_BK].frame_gaps;
    v[RF_ST_POLLS_MAIN_PS] = rf_ps[0];
    v[RF_ST_POLLS_ISR_PS] = rf_ps[1];
    v[RF_ST_USB_PERMILLE] = rf_ps[2];
    v[RF_ST_LOOPS_PS] = rf_ps[3];
    v[RF_ST_USB_RESETS] = c[RC_RESETS];
    v[RF_ST_USB_SETUPS] = c[RC_SETUPS];
    v[RF_ST_EP1_RX] = c[RC_EP1_RX];
    v[RF_ST_EP1_TX] = c[RC_EP1_TX];
    v[RF_ST_EP3_RX] = c[RC_EP3_RX];
    v[RF_ST_EP3_TX] = c[RC_EP3_TX];
    v[RF_ST_TILES] = rf_tiles_m0;
    v[RF_ST_TILE_BYTES] = rf_tile_bytes;
    v[RF_ST_MIDI_IN_DROPPED] = rf_midi_drop;
    v[RF_ST_LCD_TIMEOUTS] = c[RC_LCD_TIMEOUTS];
    v[RF_ST_SRC_US] = rf_src_us;
    v[RF_ST_SX_RX_PS] = rf_ps[4];
    v[RF_ST_SX_TX_PS] = rf_ps[5];
    v[RF_ST_BK_RX_PS] = rf_ps[6];
    v[RF_ST_BK_TX_PS] = rf_ps[7];
    v[RF_ST_USB_CINF] = rf_cinf;
    v[RF_ST_USB_RX_BAD] = 0;                        /* (felucca-bsp's usb.c does not count them) */
    v[RF_ST_LOOP_MAX_US] = rf_loop_max / PLAT_TICKS_PER_US;
    v[RF_ST_FRAMES] = rfa.frames;
    v[RF_ST_DECODE_ERRS] = rfa.decode_errs;
    v[RF_ST_PUSH_US_LAST] = rfa.push_us;
    v[RF_ST_PUSH_US_MAX] = rfa.push_max_us;
    v[RF_ST_SESSIONS] = rfa.sessions;
    b[0] = RF_ST_N;
    for (i = 0; i < RF_ST_N; i++)
        rf_wr32(b + 1 + 4 * i, v[i]);
#if RF_RADIO_N
    ble_midi_radio(b + 1 + 4 * RF_ST_N);
#endif
    rf_send(t, RF_STATS_R, b, sizeof b);
}

static rf_sx_t rf_sx[2];                            /* the SysEx parsers: USB's (0), Bluetooth's (1) */
static rf_bk_t rf_bk;
#define RF_SXP(t) (&rf_sx[(t) == RF_T_BLE])

static void rf_stats_reset(void)
{
    uint32_t k;
    for (k = 0; k < RF_NT; k++) {
        rf_tstats_t z = {0};
        rf_st[k] = z;
        rf_tx_drop[k] = 0;
    }
    rf_body_reset_seq(&rf_sx[0].b);
    rf_body_reset_seq(&rf_sx[1].b);
    rf_body_reset_seq(&rf_bk.b);
    rf_bk.have_fseq = 0;
    rf_mrx_ovf = rf_sx_held = rf_midi_drop = 0;
    rf_tiles_m0 = rf_tile_bytes = rf_src_us = 0;
    rf_cinf = 0;
    rf_loop_max = 0;
    rfa.push_max_us = 0;
}

/* SOURCE: DATA messages as fast as the transport takes them */
static uint8_t rf_gen[RF_GEN_MAX + 8u] PLAT_POOL;
static struct {
    uint8_t on, t;
    uint32_t left, msg_len, seq, msgs, bytes, t0, last_ok;
} rf_src;

static void rf_src_done(uint32_t aborted)
{
    uint8_t b[13];
    rf_src.on = 0;
    rf_src_us = (rf_ticks() - rf_src.t0) / PLAT_TICKS_PER_US;
    rf_wr32(b, rf_src.bytes);
    rf_wr32(b + 4, rf_src.msgs);
    rf_wr32(b + 8, rf_src_us);
    b[12] = (uint8_t)aborted;
    rf_send(rf_src.t, RF_SOURCE_DONE, b, sizeof b);
}

static void rf_src_start(uint32_t t, const uint8_t *body, uint32_t len)
{
    uint32_t max = t == RF_T_BK ? RF_BK_SRC_MAX : RF_SX_SRC_MAX, cap = rf_body_cap(t), ml;
    if (max > RF_GEN_MAX)
        max = RF_GEN_MAX;
    if (len < 6u) {
        rf_st[t].bad++;
        return;
    }
    if (max + 8u > cap)
        max = cap - 8u;                             /* (a message must fit the send ring whole) */
    ml = rf_rd16(body + 4);
    rf_src.t = (uint8_t)t;
    rf_src.left = rf_rd32(body);
    rf_src.msg_len = ml < 1u ? 1u : ml > max ? max : ml;
    rf_src.seq = rf_src.msgs = rf_src.bytes = 0;
    rf_src.t0 = rf_ticks();
    rf_src.last_ok = plat_ms();
    rf_src.on = 1;
}

static void rf_src_step(void)                       /* at most ~2 KB generated a turn: USB is polled between */
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
        if (!rf_up(rf_src.t)) {
            rf_src_done(1);
            return;
        }
        if (!rf_room(rf_src.t, n + 8u)) {           /* (check first: a full ring costs no generating) */
            if (plat_ms() - rf_src.last_ok > 2000u)
                rf_src_done(1);                     /* nobody reads */
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
        rf_src.last_ok = plat_ms();
    }
}

/* BLIT_BENCH: at each SPI1 baud asked for (lsb / (baud + 1)), each timed to its last byte: a full frame from the
 * shadow (four 28,800-byte DMAs), 225 16 x 16 tiles with a window each, 15 240 x 16 strips of the shadow. The timer
 * serves USB meanwhile; then the push starts again from the shadow */
#if RFM_SHADOW
static uint16_t rf_tile2[2][256];
static void rf_lcd_wait_serving(void)               /* the running transfer ends; USB is served meanwhile */
{
    uint32_t t0 = rf_ticks();
    while (!lcd_idle() && (uint32_t)(rf_ticks() - t0) < 100000u * PLAT_TICKS_PER_US)
        rp_service();
    lcd_sync();
}
static uint32_t rf_us_since(uint32_t t0) { return (rf_ticks() - t0) / PLAT_TICKS_PER_US; }
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
    rf_lcd_wait_serving();
    rf_pass_slow = 1;
    out[0] = (uint8_t)nb;
    for (k = 0; k < nb; k++) {
        uint32_t b = bauds[k] > 15u ? 15u : bauds[k], full, tiles, strips;
        rp_wdt();
        rp_lcd_baud(b);
        t0 = rf_ticks();
        for (i = 0; i < 4u; i++)
            lcd_blit(0, 60u * i, 240, 60, rf_shadow + i * 14400u);
        lcd_sync();
        full = rf_us_since(t0);
        rp_service();
        rp_wdt();
        t0 = rf_ticks();
        for (y = 0; y < 15u; y++)
            for (x = 0; x < 15u; x++)
                lcd_blit(x * 16u, y * 16u, 16, 16, rf_tile2[(x + y) & 1u]);
        lcd_sync();
        tiles = rf_us_since(t0);
        rp_service();
        rp_wdt();
        t0 = rf_ticks();
        for (y = 0; y < 15u; y++)
            lcd_blit(0, y * 16u, 240, 16, rf_shadow + y * 16u * 240u);
        lcd_sync();
        strips = rf_us_since(t0);
        rp_service();
        out[1 + 13 * k] = (uint8_t)b;
        rf_wr32(out + 2 + 13 * k, full);
        rf_wr32(out + 6 + 13 * k, tiles);
        rf_wr32(out + 10 + 13 * k, strips);
    }
#ifdef LCD_BAUD
    rp_lcd_baud(LCD_BAUD);
#endif
    rfm_app_lcd_lost();
    rf_scr_hold = plat_ms() + 3000u;
    rf_scr_full = 1;
    rf_send(t, RF_BLIT_R, out, 1u + 13u * nb);
}
#endif

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
#if RFM_SHADOW
    case RF_BLIT_BENCH:
        rf_blit_bench(t, body, len);
        break;
#endif
    case RF_SA_BENCH: {                             /* the standalone DX7's bench (sa/sa_bench.c) */
        uint8_t b[SA_BENCH_REPLY];
        rf_send(t, RF_SA_BENCH_R, b, sa_bench_cmd(body, len, b));
        break;
    }
    case RF_SA_LOOP: {                              /* the standalone's loops (sa/sa_loop.c) */
        static uint8_t b[SA_LOOP_REPLY] PLAT_POOL;
        rf_send(t, RF_SA_LOOP_R, b, sa_loop_cmd(body, len, b));
        break;
    }
    case RF_TILE:
        if (!rfm_app_tile_m0(body, len)) {
            rf_st[t].bad++;
            break;
        }
        rf_tiles_m0++;
        rf_tile_bytes += len - 4u;
        rf_scr_hold = plat_ms() + 3000u;
        if (!rfa.session)
            rf_scr_full = 1;                        /* the waiting screen comes back whole after it */
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
    sa_host_msg(t, cmd, body, len);                 /* a HELLO hands the FM-1 to the host (sa_main.c) */
    if (cmd == RF_HELLO && rf_src.on)
        rf_src_done(1);                             /* HELLO stops a SOURCE (M0) */
    rfm_app_msg(t, cmd, body, len);
}

/* ------------------------------------------------------------------ in: the rings into the parsers --- */
static uint8_t rf_sx_buf[2][RF_SX_BODY_MAX] PLAT_POOL;
static uint8_t rf_bk_buf[RF_BK_RX_MAX] PLAT_POOL;
static uint32_t rf_ble_other;                       /* packets in that are not SysEx (Bluetooth MIDI): left */

static void rf_rx_step(void)
{
    uint32_t r, n, i, got = 0, pkt;
    int port, k;
    rf_drain_t = rf_ticks();
    rf_sx[0].b.now_ms = rf_sx[1].b.now_ms = rf_bk.b.now_ms = plat_ms();
    r = mrx_r;                                      /* USB SysEx (rf_sx_tap) */
    n = mrx_w - r;
    if (n > 512u)
        n = 512u;
    RING_PUBLISH();                                 /* the slots after the index */
    for (i = 0; i < n; i++) {
        if (rfm_app_rx_wait(RF_T_SX)) {             /* (HOLD: the push first) */
            n = i;
            break;
        }
        rf_sx_byte(&rf_sx[0], rf_mrx[(r + i) & (RF_MRX_N - 1u)]);
        RING_PUBLISH();
        mrx_r = r + i + 1u;                         /* (a command may take long: free its bytes as they go) */
    }
    rf_raw_rx[RF_T_SX] += n;
    got = n;
    for (k = 0; k < 4 && rp_bk_avail() && !rfm_app_rx_wait(RF_T_BK); k++) {   /* the vendor bulk pair */
        uint8_t b[256];
        n = rp_bk_read(b, sizeof b);
        rf_bk_feed(&rf_bk, b, n);
        rf_raw_rx[RF_T_BK] += n;
        got += n;
    }
    for (k = 0; k < 64 && !rfm_app_rx_wait(RF_T_BLE); k++) {   /* Bluetooth (and what is left of USB MIDI in) */
        uint32_t f = plat_lock(), cin;              /* (usb.c empties the USB part from its context: rf_ep1_room) */
        int have = plat_midi_in(&pkt, &port);
        plat_unlock(f);
        if (!have)
            break;
        cin = pkt & 15u;
        if (port != PORT_BLE) {                     /* USB MIDI in: the instrument's, standalone */
            if (sa_standalone())
                sa_midi_in(pkt);
            else
                rf_midi_drop++;
            continue;
        }
        if (cin >= 4u && cin <= 7u) {               /* SysEx: 3 bytes going on, or the last 1 / 2 / 3 */
            uint32_t nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u, j;
            for (j = 0; j < nb; j++) {
                rf_sx_byte(&rf_sx[1], (pkt >> (8u * (j + 1u))) & 0xFFu);
                if (sa_standalone())
                    sa_ub_byte((pkt >> (8u * (j + 1u))) & 0xFFu);   /* (a DX7 bank dump: sa/sa_ubank.c) */
            }
            rf_raw_rx[RF_T_BLE] += nb;
            got += nb;
        } else {
            uint32_t st = (pkt >> 8) & 0xFFu;
            if (st < 0xF8u)
                rf_sx_byte(&rf_sx[1], st);          /* (a status ends a SysEx cut short; real time does not) */
            if (sa_standalone())
                sa_midi_in(pkt);                    /* (Bluetooth MIDI in: the instrument's, standalone) */
            rf_ble_other++;
        }
    }
    if (got)
        rfm_app_activity();                         /* (SINK too: the keepalive counts every byte) */
}

/* ------------------------------------------------------------------ the panel, once a millisecond --- */
static uint32_t rf_usb_resets_seen;
static uint8_t rf_ble_was;
static void rf_ms_step(void)
{
    static uint32_t adc_ms;
    int32_t steps[RFM_NENC], roles[NE], master = -1;
    uint32_t e, keys = plat_keys(), lb = plat_buttons(), btns = 0, i;
    for (e = 0; e < RFM_NENC; e++)
        steps[e] = 0;
    for (e = 0; e < NE; e++) {
        roles[e] = plat_enc((int)e);
        steps[RF_ENC_OF_ROLE[e]] += roles[e];
    }
    sa_ms(lb, roles);                               /* standalone: the instrument; panel mode: its timeout */
    for (i = 0; i < RFM_NBTN; i++)
        if (lb >> RF_LABEL_OF_BTN[i] & 1u)
            btns |= 1u << i;
    if (plat_ms() - adc_ms >= RFA_MASTER_MS) {
        uint32_t m = plat_master() >> 2;            /* 0..4096 -> the ADC's 0..1023 */
        adc_ms = plat_ms();
        master = (int32_t)(m > 1023u ? 1023u : m);
    }
    if (!rfa.session && sa_standalone())
        rf_wait_btns = 0xFFFFFFFFu;                 /* (sa_main.c's LEDs; the waiting ones again in panel mode) */
    else if (!rfa.session)
        rf_waiting_leds(lb & ((1u << NB) - 1u), keys & ((1u << NKEYS) - 1u));
    rfm_app_panel(keys, btns, steps, master);
    if (rfa.session && rfa.st != RF_T_BLE && (rp_usb_resets() != rf_usb_resets_seen || !rp_usb_live()))
        rfm_app_usb_lost();                         /* a bus reset, or the host went away / asleep */
    rf_usb_resets_seen = rp_usb_resets();
    if (rf_ble_was && !plat_ble_connected()) {      /* a Bluetooth computer gone: its session, its half message */
        if (rfa.session && rfa.st == RF_T_BLE)
            rfm_app_usb_lost();
        rf_sx[1].st = 0;
        rf_bp_n = 0;
    }
    rf_ble_was = (uint8_t)plat_ble_connected();
}

/* ------------------------------------------------------------------ the waiting screen's lines --- */
/* (0.2's waiting screen: now the lines of sa_ui.c's hand-over screen, panel mode before the host's session) */
#define RF_NL 10u
static char rf_want[RF_NL][32];

static char *s_put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}
static char *s_int(char *d, uint32_t v)
{
    char t[10];
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) && n < 10);
    while (n)
        *d++ = t[--n];
    *d = 0;
    return d;
}
static char *s_kbs(char *d, uint32_t bps)            /* bytes / s -> "12.3" (KB/s) */
{
    uint32_t t = bps / 1024u * 10u + (bps % 1024u) * 10u / 1024u;
    d = s_int(d, t / 10u);
    *d++ = '.';
    return s_int(d, t % 10u);
}

static uint32_t rf_errs(uint32_t t)
{
    const rf_tstats_t *s = &rf_st[t];
    return s->gaps + s->chk + s->bad + rf_tx_drop[t] + (t == RF_T_BK ? s->sync + s->frame_gaps :
                                                       t == RF_T_SX ? rf_mrx_ovf : 0u);
}

static void rf_lines(void)
{
    char *p;
    int b = plat_battery();
    s_put(rf_want[0], APP_NAME);
    p = s_put(rf_want[1], APP_VERSION[0] >= '0' && APP_VERSION[0] <= '9' ? "V" : "");
    p = s_put(p, APP_VERSION);
    p = s_put(p, "   ");
    s_put(p, FELUCCA_ID);
    s_put(rf_want[2], "WAITING FOR HOST");
    p = s_put(rf_want[3], "USB ");
    s_put(p, !rp_usb_cfg() ? "NO HOST" : !rp_usb_live() ? "IDLE" : "HOST CONNECTED");
    rf_want[4][0] = 0;
    if (FM1B_BLE) {
        char n[12];
        int st = plat_ble_state();
        plat_ble_name(n);
        p = s_put(rf_want[4], "BT ");
        p = s_put(p, n);
        p = s_put(p, " ");
        s_put(p, plat_ble_connected() ? "CONNECTED" : st == BLE_READY ? "READY" : st == BLE_STARTING ? "STARTING" :
                 st == BLE_FAILED ? "FAILED" : "OFF");
    }
    if (b < 0) {
        rf_want[5][0] = 0;
    } else if (b > 3) {
        s_put(rf_want[5], "POWERED OVER USB");
    } else {
        p = s_put(rf_want[5], "BATTERY ");
        p = s_int(p, (uint32_t)b);
        s_put(p, "/3");
    }
    p = s_put(rf_want[6], "MIDI RX ");
    p = s_kbs(p, rf_ps[4]);
    p = s_put(p, " TX ");
    p = s_kbs(p, rf_ps[5]);
    s_put(p, " KB/S");
    p = s_put(rf_want[7], "BULK RX ");
    p = s_kbs(p, rf_ps[6]);
    p = s_put(p, " TX ");
    p = s_kbs(p, rf_ps[7]);
    s_put(p, " KB/S");
    p = s_put(rf_want[8], "ERRORS MIDI ");
    p = s_int(p, rf_errs(RF_T_SX));
    p = s_put(p, " BULK ");
    p = s_int(p, rf_errs(RF_T_BK));
    if (FM1B_BLE) {
        p = s_put(p, " BT ");
        s_int(p, rf_errs(RF_T_BLE));
    }
    s_put(rf_want[9], rf_flash_ok() ? "" : "UNKNOWN FLASH: NO UPDATES");
}

/* the hand-over screen's lines: the version and identity, USB's state, Bluetooth's, the errors (or the flash's
 * warning); warn bit i: line i in the warning colour */
static void rf_info(char out[4][32], uint8_t *warn)
{
    static const uint8_t L[4] = {1, 3, 4, 8};
    uint32_t i;
    rf_lines();
    *warn = 0;
    for (i = 0; i < 4u; i++)
        memcpy(out[i], rf_want[L[i]], 32);
    if (rf_errs(RF_T_SX) || rf_errs(RF_T_BK) || rf_errs(RF_T_BLE))
        *warn |= 8u;
    if (!rf_flash_ok()) {
        memcpy(out[3], rf_want[9], 32);
        *warn |= 8u;
    }
}

static void rf_second_step(void)
{
    static uint32_t sec, pp, pt, lp, rx[RF_NT], tx[RF_NT];
    uint32_t k;
    if (plat_ms() - sec < 1000u)
        return;
    sec = plat_ms();
    rf_ps[0] = rf_polls - pp;
    rf_ps[1] = 0;                                   /* (the timer's own polls are fm1-base's) */
    rf_ps[2] = (rf_poll_ticks - pt) / (1000u * PLAT_TICKS_PER_US);   /* permille of a second */
    rf_ps[3] = rf_loops - lp;
    pp = rf_polls;
    pt = rf_poll_ticks;
    lp = rf_loops;
    rf_ps[4] = rf_raw_rx[RF_T_SX] + rf_raw_rx[RF_T_BLE] - rx[RF_T_SX] - rx[RF_T_BLE];
    rf_ps[5] = rf_raw_tx[RF_T_SX] + rf_raw_tx[RF_T_BLE] - tx[RF_T_SX] - tx[RF_T_BLE];
    rf_ps[6] = rf_raw_rx[RF_T_BK] - rx[RF_T_BK];
    rf_ps[7] = rf_raw_tx[RF_T_BK] - tx[RF_T_BK];
    for (k = 0; k < RF_NT; k++) {
        rx[k] = rf_raw_rx[k];
        tx[k] = rf_raw_tx[k];
    }
}

/* ------------------------------------------------------------------ the loop --- */
static void rf_poll(void)                           /* USB served, timed */
{
    uint32_t t0 = rf_ticks();
    rp_service();
    rf_poll_ticks += rf_ticks() - t0;
    rf_polls++;
}

static uint32_t rf_last_ms;
static void rf_turn(void)                           /* one turn: USB, the rings, the push, the replies */
{
    uint32_t t0 = rf_ticks();
    rf_pass_slow = 0;
    if (plat_ms() != rf_last_ms) {
        rf_last_ms = plat_ms();
        rf_ms_step();
    }
    rf_poll();
    rf_rx_step();
    rf_poll();
    rfm_app_poll(plat_ms());
#if !RFM_SHADOW
    rf_rows_flush();                                /* (what a message's rectangles left) */
#endif
    rf_poll();
    rf_src_step();
    rf_bp_flush();
    rf_ev_flush();
    rf_loops++;
    t0 = rf_ticks() - t0;
    if (!rf_pass_slow && t0 > rf_loop_max)
        rf_loop_max = t0;
}

/* turns for RF_SLICE_US (one in the simulator), Bluetooth's tasks every quarter of a millisecond */
static void rf_run(void)
{
    uint32_t t0 = rf_ticks(), y = t0;
    do {
        rf_turn();
        if (rf_ticks() - y >= 250u * PLAT_TICKS_PER_US) {
            y = rf_ticks();
            plat_yield();                           /* (fm1-base: Bluetooth's tasks) */
        }
#ifdef PLAT_HOST
        break;
#endif
    } while (rf_ticks() - t0 < RF_SLICE_US * PLAT_TICKS_PER_US);
    rf_second_step();
}

/* ------------------------------------------------------------------ the contract --- */
#if FM1B_CPU2
static int rf_c1_start = -2;                        /* plat_cpu2_set(1) at app_init: 0 it runs, -1 one core */
#endif
void app_init(void)
{
    rf_body_init(&rf_sx[0].b, &rf_st[RF_T_SX], rf_sx_buf[0], RF_SX_BODY_MAX, rf_on_msg, (void *)(uintptr_t)RF_T_SX);
    rf_body_init(&rf_sx[1].b, &rf_st[RF_T_BLE], rf_sx_buf[1], RF_SX_BODY_MAX, rf_on_msg, (void *)(uintptr_t)RF_T_BLE);
    rf_body_init(&rf_bk.b, &rf_st[RF_T_BK], rf_bk_buf, sizeof rf_bk_buf, rf_on_msg, (void *)(uintptr_t)RF_T_BK);
#if RFM_SHADOW
    rfm_app_init(rf_shadow, rf_stage, RF_STAGE_PX, &RF_LCD);
#else
    rfm_app_init(0, rf_stage, RF_STAGE_PX, &RF_LCD);
    rfa.draw.row = rf_row;
    rfa.draw.rowbuf = rf_rowbuf;
#endif
    rf_drain_t = rf_ticks();
#if FM1B_CPU2
    rf_c1_start = plat_cpu2_set(1);                 /* the second core (fm1.toml cpu2), as FIZZ starts it: here or never,
                                                     * before the sound; one core after a failed start or while barred */
#endif
    sa_bench_init();
    sa_init();                                      /* the standalone instrument (sa_main.c), after the bench's synth */
#ifdef PLAT_HOST
    hs_init();                                      /* (the simulated host's parsers: rfm_host.c) */
#endif
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12) { sa_render(lr, n, master_q12); }

void app_panic(void) { sa_ev(SA_EV_SOUNDOFF, 0, 0); }   /* (before an update: the render runs on) */

/* the first screen, and again after the platform drew its own (the update's): the standalone's (or the hand-over's)
 * whole, and in a session the host's screen repainted from the shadow */
void app_ui_init(void)
{
    rfm_app_lcd_lost();
    rf_scr_full = 1;
    rf_scr_hold = plat_ms();
    rf_wait_btns = 0xFFFFFFFFu;
    sa_ui_init();                                   /* the keys' velocity sensing on, standalone (sa_main.c) */
    sa_ui_lost();
}

void app_frame(void)
{
    rf_run();
    sa_frame();                                     /* the standalone screens, or the hand-over (sa_ui.c) */
    sa_bench_step();
}

void app_input_only(void)
{
    rf_run();
    sa_frame();
    sa_bench_step();
}

void app_say(const char *a, const char *b) { (void)a, (void)b; }   /* (no UI of its own to speak) */

#ifdef PLAT_HOST
#include "rfm_host.c"
#else
#include "rfm_dev.c"
#endif
