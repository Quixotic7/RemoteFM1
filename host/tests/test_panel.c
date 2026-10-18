/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The panel decoder: keys, buttons, encoder deltas, the 14-bit MASTER, running status through the link parser,
 * PANEL_R reconciliation (settling, counter differences, wrap-around). */
#include <string.h>

#include "check.h"
#include "rfm/codec.h"
#include "rfm/panel.h"

static rfm_event_t evs[512];
static uint32_t nev;
static void on_ev(void *ctx, const rfm_event_t *e)
{
    (void)ctx;
    if (nev < 512)
        evs[nev++] = *e;
}

static void m3(rfm_panel_t *p, uint8_t a, uint8_t b, uint8_t c, uint32_t now)
{
    uint8_t m[3];
    m[0] = a;
    m[1] = b;
    m[2] = c;
    rfm_panel_midi(p, m, 3, now);
}

static int last_is(uint8_t type, uint8_t index, int16_t value, uint8_t source)
{
    return nev && evs[nev - 1].type == type && evs[nev - 1].index == index && evs[nev - 1].value == value &&
           evs[nev - 1].source == source;
}

static void test_events(void)
{
    rfm_panel_t p;
    uint32_t k, e, adc;
    int d;
    rfm_panel_init(&p, 60, on_ev, NULL);
    nev = 0;
    for (k = 0; k < RFM_NKEYS; k++) {
        m3(&p, 0x9F, (uint8_t)(53 + k), 100, 1);
        CHECK(last_is(RFM_EV_KEY, (uint8_t)k, 1, RFM_EVS_MIDI) && (p.keys >> k & 1u));
    }
    CHECK(p.keys == (1u << RFM_NKEYS) - 1u && nev == RFM_NKEYS);
    m3(&p, 0x8F, 53, 0, 2);
    CHECK(last_is(RFM_EV_KEY, 0, 0, RFM_EVS_MIDI));
    m3(&p, 0x9F, 54, 0, 2);                             /* Note On velocity 0 = off */
    CHECK(last_is(RFM_EV_KEY, 1, 0, RFM_EVS_MIDI));
    nev = 0;
    m3(&p, 0x9F, 52, 100, 2);                           /* outside the keys */
    m3(&p, 0x9F, 80, 100, 2);
    m3(&p, 0x90, 60, 100, 2);                           /* another channel */
    m3(&p, 0x9F, 55, 100, 2);                           /* already down: no event */
    CHECK(nev == 0);
    for (k = 0; k < RFM_NBTN; k++) {
        m3(&p, 0x9E, (uint8_t)k, 127, 3);
        CHECK(last_is(RFM_EV_BUTTON, (uint8_t)k, 1, RFM_EVS_MIDI));
        m3(&p, 0x8E, (uint8_t)k, 0, 3);
        CHECK(last_is(RFM_EV_BUTTON, (uint8_t)k, 0, RFM_EVS_MIDI));
    }
    nev = 0;
    m3(&p, 0x9E, 14, 127, 3);                           /* not a button */
    CHECK(nev == 0 && p.buttons == 0);
    /* encoders: 64 + d, d in -63..63 (0 = -64 accepted), 64 ignored */
    for (e = 0; e < RFM_NENC; e++) {
        int32_t sum = 0;
        for (d = -64; d <= 63; d++) {
            nev = 0;
            m3(&p, 0xBE, (uint8_t)(16 + e), (uint8_t)(64 + d), 4);
            if (!d) {
                CHECK(nev == 0);
                continue;
            }
            CHECK(last_is(RFM_EV_ENC, (uint8_t)e, (int16_t)d, RFM_EVS_MIDI));
            sum += d;
        }
        CHECK(p.enc_count[e] == (int16_t)sum);
    }
    /* MASTER: v14 = adc << 4 over CC 7 / CC 39, every value */
    for (adc = 0; adc < 1024; adc++) {
        uint32_t v14 = adc << 4;
        nev = 0;
        m3(&p, 0xBE, 7, (uint8_t)(v14 >> 7), 5);
        CHECK(nev == 0);
        m3(&p, 0xBE, 39, (uint8_t)(v14 & 0x7F), 5);
        CHECKF(p.master == adc && (adc == 0 ? 1 : last_is(RFM_EV_MASTER, 0, (int16_t)adc, RFM_EVS_MIDI)), "adc %u",
               adc);
    }
    nev = 0;
    m3(&p, 0xBE, 39, 0x10, 6);                          /* an LSB alone: ignored */
    CHECK(nev == 0 && p.master == 1023);
    m3(&p, 0xBE, 7, 127, 6);                            /* the same value again: no event */
    m3(&p, 0xBE, 39, 0x70, 6);
    CHECK(nev == 0);
    m3(&p, 0xBD, 16, 70, 6);                            /* CC on another channel */
    CHECK(nev == 0);
}

/* running status through the link parser: BE 10 41 11 42 = two encoder messages */
static rfm_panel_t g_p;
static void on_midi(void *ctx, const uint8_t *m, uint32_t n)
{
    (void)ctx;
    rfm_panel_midi(&g_p, m, n, 10);
}
static void test_running_status(void)
{
    static const uint8_t s[] = {0xBE, 0x10, 0x41, 0x11, 0x3E, 0xF8, 0x12, 0x45, 0x9F, 53, 100, 54, 100};
    rfm_rx_t *r = rfm_rx_new(RFM_LINK_SX, 64, NULL, on_midi, NULL);
    rfm_panel_init(&g_p, 60, on_ev, NULL);
    nev = 0;
    rfm_rx_feed(r, s, sizeof s);
    CHECK(nev == 5);
    CHECK(evs[0].type == RFM_EV_ENC && evs[0].index == 0 && evs[0].value == 1);
    CHECK(evs[1].type == RFM_EV_ENC && evs[1].index == 1 && evs[1].value == -2);
    CHECK(evs[2].type == RFM_EV_ENC && evs[2].index == 2 && evs[2].value == 5);
    CHECK(evs[3].type == RFM_EV_KEY && evs[3].index == 0 && evs[4].index == 1);
    rfm_rx_free(r);
}

static void test_reconcile(void)
{
    rfm_panel_t p;
    rfm_panel_r_t r;
    memset(&r, 0, sizeof r);
    r.flags = 1;
    r.master = 500;
    r.enc[2] = 100;
    r.keys = 1u << 4;                                   /* a key held when the session starts */
    rfm_panel_init(&p, 60, on_ev, NULL);
    nev = 0;
    rfm_panel_reconcile(&p, &r, 1000);                  /* the first: keys / buttons / master; encoders baseline */
    CHECK(nev == 2);
    CHECK(evs[0].type == RFM_EV_KEY && evs[0].index == 4 && evs[0].value == 1 && evs[0].source == RFM_EVS_RESYNC);
    CHECK(evs[1].type == RFM_EV_MASTER && evs[1].value == 500);
    CHECK(p.enc_count[2] == 100);
    /* a lost Note Off: PANEL_R says up */
    nev = 0;
    r.keys = 0;
    rfm_panel_reconcile(&p, &r, 1500);
    CHECK(nev == 1 && last_is(RFM_EV_KEY, 4, 0, RFM_EVS_RESYNC));
    /* a PANEL_R overtaking an event: the button went down 10 ms ago by MIDI, PANEL_R (older) says up: kept */
    nev = 0;
    m3(&p, 0x9E, 3, 127, 1990);
    rfm_panel_reconcile(&p, &r, 2000);
    CHECK(nev == 1 && (p.buttons & 8u));
    rfm_panel_reconcile(&p, &r, 2100);                  /* settled and still up: corrected */
    CHECK(nev == 2 && last_is(RFM_EV_BUTTON, 3, 0, RFM_EVS_RESYNC));
    /* lost encoder detents: the counter difference */
    nev = 0;
    m3(&p, 0xBE, 18, 64 + 3, 2200);                     /* 3 arrive ... */
    r.enc[2] = 100 + 3 + 4;                             /* ... 4 more were lost */
    rfm_panel_reconcile(&p, &r, 2205);                  /* not settled: wait */
    CHECK(nev == 1);
    rfm_panel_reconcile(&p, &r, 2700);
    CHECK(nev == 2 && last_is(RFM_EV_ENC, 2, 4, RFM_EVS_RESYNC) && p.enc_count[2] == 107);
    /* wrap-around: 32767 -> -32766 is +3 */
    r.enc[2] = 32767;
    rfm_panel_reconcile(&p, &r, 3200);
    nev = 0;
    r.enc[2] = -32766;
    rfm_panel_reconcile(&p, &r, 3700);
    CHECK(nev == 1 && last_is(RFM_EV_ENC, 2, 3, RFM_EVS_RESYNC));
    /* MASTER: a 1-count difference is jitter (the unit sends >= 2), 2 is a change */
    nev = 0;
    r.master = 501;
    rfm_panel_reconcile(&p, &r, 4200);
    CHECK(nev == 0);
    r.master = 502;
    rfm_panel_reconcile(&p, &r, 4700);
    CHECK(nev == 1 && last_is(RFM_EV_MASTER, 0, 502, RFM_EVS_RESYNC));
    /* reset (a new session): the next PANEL_R is a baseline for the encoders */
    rfm_panel_reset(&p);
    nev = 0;
    r.enc[0] = 555;
    rfm_panel_reconcile(&p, &r, 5200);
    CHECK(nev == 0 && p.enc_count[0] == 555);
    CHECK(p.battery_mv == r.battery_mv);
}

int main(void)
{
    test_events();
    test_running_status();
    test_reconcile();
    return check_done("test_panel");
}
