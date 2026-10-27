/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1's port on the FM-1 (rfm_main.c's rp_*): felucca-bsp's usb.c, patched with the vendor interface and the
 * hooks (firmware/patches/felucca-bsp: app.h turns them on), and the panel's DMA. Part of fm1-base's unity build, after
 * usb.c and lcd.c: their rings and counters are in scope. Nothing here touches a register: usb_poll, the rings and
 * fm1_lcd_baud are felucca-bsp's. */

/* USB served now, from the main loop: interrupts off around it, so the timer's own 2 kHz poll never meets it (one
 * context in the SIE at a time); a poll is a few microseconds */
static void rp_service(void)
{
    uint32_t f = plat_lock();
    usb_poll();
    plat_unlock(f);
}

static int rp_usb_cfg(void) { return usb.up && usb.config; }
static int rp_usb_live(void) { return usb.up && usb.config && !usb.suspended; }
static uint32_t rp_usb_resets(void) { return usb.resets; }

/* usb.c's SysEx send ring (sx_out_q, SXQ packets): a whole message written, then published at once (ep1_tx sends
 * it in order, before any note) */
static uint32_t rp_sx_room(void) { return SXQ - (so_w - so_r); }
static uint32_t rp_sx_w;
static void rp_sx_begin(void) { rp_sx_w = so_w; }
static void rp_sx_put(uint32_t pkt) { sx_out_q[rp_sx_w++ % SXQ] = pkt; }
static void rp_sx_end(void)
{
    RING_PUBLISH();                                 /* the packets before the index */
    so_w = rp_sx_w;
}

/* the vendor bulk pair (usb.c FELUCCA_VENDOR) */
static uint32_t rp_bk_avail(void) { return usb_vendor_avail(); }
static uint32_t rp_bk_read(uint8_t *dst, uint32_t n) { return usb_vendor_read(dst, n); }
static uint32_t rp_bk_room(void) { return usb_vendor_room(); }
static int rp_bk_send(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb)
{
    return usb_vendor_send(a, na, b, nb);
}
static uint32_t rp_bk_rx_n(void) { return FELUCCA_VENDOR_IN_N; }
static uint32_t rp_bk_tx_n(void) { return FELUCCA_VENDOR_OUT_N; }

/* Bluetooth: the computer's link (room asked first, a whole message at once) */
static void rp_ble_put(uint32_t pkt) { plat_midi_send(pkt, TO_BLE_HOST); }

static void rp_lcd_baud(uint32_t b) { fm1_lcd_baud(b); }
static void rp_wdt(void) { fm1_wdt_feed(); }

#if !RFM_SHADOW
static void rp_wait_idle(void)                      /* the panel's transfer ends (100 ms at most: lcd.c's own
                                                     * wait then times out); USB is served meanwhile */
{
    uint32_t t0 = plat_ticks24();
    while (!lcd_idle() && plat_ticks24() - t0 < 100000u * PLAT_TICKS_PER_US)
        rp_service();
}
#endif

static void rp_counters(uint32_t *c)
{
    c[RC_RESETS] = usb.resets;
    c[RC_SETUPS] = usb.setups;
    c[RC_EP1_RX] = usb.rx_pkts;
    c[RC_EP1_TX] = usb.tx_pkts;
    c[RC_EP3_RX] = vb.rx_pkts;
    c[RC_EP3_TX] = vb.tx_pkts;
    c[RC_EP3_HELD] = vb.held;
    c[RC_LCD_TIMEOUTS] = fm1_lcd_timeouts;
}
