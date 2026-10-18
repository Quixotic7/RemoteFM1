/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The session against scripted fake units (fake.c), on a simulated millisecond clock: discovery among other
 * firmwares, frames and flow control, LEDs and keepalive, panel events and PANEL_R repair, shadow reset (a reboot),
 * keepalive loss (a deaf unit), a lost FRAME_ACK, a decode error, stop, transports. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "frames.h"
#include "rfm/fake.h"
#include "rfm/session.h"

#define MAXF 4

typedef struct h h_t;
typedef struct {
    h_t *h;
    int fake, kind, id;
} hl_t;

struct h {
    rfm_session_t *s;
    rfm_fake_t *f[MAXF];
    int nf;
    hl_t links[2 * MAXF];
    int nl;
    uint32_t now, max_in_flight, rescans, states[8];
    rfm_event_t ev[256];
    uint32_t nev;
    int verbose;
};

static int w(void *ctx, const uint8_t *p, uint32_t n)
{
    hl_t *l = (hl_t *)ctx;
    if (l->kind == RFM_LINK_SX)
        rfm_fake_rx_sx(l->h->f[l->fake], p, n, l->h->now);
    else
        rfm_fake_rx_bk(l->h->f[l->fake], p, n, l->h->now);
    return 0;
}
static void cb_event(void *ctx, const rfm_event_t *e)
{
    h_t *h = (h_t *)ctx;
    if (h->nev < 256)
        h->ev[h->nev++] = *e;
}
static void cb_state(void *ctx, int st, const char *why)
{
    h_t *h = (h_t *)ctx;
    h->states[st]++;
    if (h->verbose)
        printf("  %6u ms  %s (%s)\n", h->now, rfm_session_state_name(st), why);
}
static void cb_rescan(void *ctx) { ((h_t *)ctx)->rescans++; }
static void cb_log(void *ctx, const char *m)
{
    h_t *h = (h_t *)ctx;
    if (h->verbose)
        printf("  %6u ms  %s\n", h->now, m);
}

static void add_fake(h_t *h, const rfm_fake_cfg_t *fc)
{
    int k = h->nf++;
    h->f[k] = rfm_fake_new(fc);
    h->links[h->nl].h = h;
    h->links[h->nl].fake = k;
    h->links[h->nl].kind = RFM_LINK_SX;
    h->links[h->nl].id = rfm_session_add_link(h->s, RFM_LINK_SX, w, &h->links[h->nl], "midi");
    h->nl++;
    if (fc->has_bulk) {
        h->links[h->nl].h = h;
        h->links[h->nl].fake = k;
        h->links[h->nl].kind = RFM_LINK_BK;
        h->links[h->nl].id = rfm_session_add_link(h->s, RFM_LINK_BK, w, &h->links[h->nl], "bulk");
        h->nl++;
    }
}

static void setup(h_t *h, const rfm_session_cfg_t *sc)
{
    rfm_session_cb_t cb;
    memset(h, 0, sizeof *h);
    h->verbose = getenv("RFM_VERBOSE") != NULL;
    cb.ctx = h;
    cb.event = cb_event;
    cb.state = cb_state;
    cb.rescan = cb_rescan;
    cb.log = cb_log;
    h->s = rfm_session_new(sc, &cb);
    h->now = 1000;
}

static void teardown(h_t *h)
{
    int i;
    for (i = 0; i < h->nf; i++)
        rfm_fake_free(h->f[i]);
    rfm_session_free(h->s);
}

static void step(h_t *h, uint32_t ms)
{
    static uint8_t buf[300000];
    while (ms--) {
        int i;
        rfm_session_stats_t st;
        h->now++;
        for (i = 0; i < h->nl; i++) {
            hl_t *l = &h->links[i];
            uint32_t n;
            rfm_fake_poll(h->f[l->fake], h->now);
            n = l->kind == RFM_LINK_SX ? rfm_fake_take_sx(h->f[l->fake], buf, sizeof buf)
                                       : rfm_fake_take_bk(h->f[l->fake], buf, sizeof buf);
            if (n)
                rfm_session_rx(h->s, l->id, buf, n, h->now);
        }
        rfm_session_poll(h->s, h->now);
        rfm_session_stats(h->s, &st);
        if (st.in_flight > h->max_in_flight)
            h->max_in_flight = st.in_flight;
    }
}

static int shadow_is(const rfm_fake_t *f, const uint16_t *img) { return !memcmp(rfm_fake_shadow(f), img, FR_N * 2); }

static int run_until_converged(h_t *h, rfm_fake_t *f, const uint16_t *img, uint32_t max_ms)
{
    uint32_t t;
    for (t = 0; t < max_ms; t += 10) {
        rfm_session_stats_t st;
        step(h, 10);
        rfm_session_stats(h->s, &st);
        if (rfm_session_state(h->s) == RFM_S_ACTIVE && rfm_fake_session(f) && !st.pending && !st.in_flight &&
            shadow_is(f, img))
            return 1;
    }
    return 0;
}

static void default_fake(rfm_fake_cfg_t *c) { rfm_fake_cfg_default(c); }

/* discovery among a silent firmware, the M0 bench (proto 1) and the unit; bulk chosen for frames */
static void test_discovery_and_frames(void)
{
    static uint16_t img[FR_N];
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_fake_stats_t fs;
    rfm_session_stats_t st;
    rfm_leds_t leds;
    rfm_fake_t *unit;
    uint32_t t;
    rfm_session_cfg_default(&sc);
    setup(&h, &sc);
    default_fake(&fc);
    fc.silent = 1;
    fc.has_bulk = 0;
    add_fake(&h, &fc);                                  /* stock / ChoralRoot: ignores HELLO */
    default_fake(&fc);
    fc.name = "RemoteFM1-M0";
    fc.proto = 1;
    add_fake(&h, &fc);                                  /* the M0 bench: answers, but it is not v2 */
    default_fake(&fc);
    fc.bk_rate_bps = 830000;
    fc.sx_rate_bps = 300000;
    add_fake(&h, &fc);                                  /* the unit */
    unit = h.f[2];
    step(&h, 50);
    CHECK(rfm_session_state(h.s) == RFM_S_ACTIVE);
    CHECK(rfm_session_link_kind(h.s, rfm_session_frame_link(h.s)) == RFM_LINK_BK);
    CHECK(rfm_session_frame_link(h.s) == h.links[4].id && rfm_session_midi_link(h.s) == h.links[3].id);
    CHECK(rfm_session_budget(h.s) == 40000u);
    CHECK(rfm_fake_session(unit) == 2 && rfm_fake_session(h.f[1]) == 0);
    /* a UI, animated for 3 s at 20 Hz, then still */
    memset(&leds, 0, sizeof leds);
    for (t = 0; t < 60; t++) {
        fr_ui(img, t);
        rfm_session_set_frame(h.s, img);
        leds.lit[t % 11] = (uint8_t)t;
        rfm_session_set_leds(h.s, &leds);
        step(&h, 50);
    }
    CHECK(run_until_converged(&h, unit, img, 1000));
    CHECK(rfm_leds_equal(rfm_fake_leds(unit), &leds));
    rfm_session_stats(h.s, &st);
    rfm_fake_stats(unit, &fs);
    CHECKF(st.frames >= 55 && st.acks == st.frames, "frames %u acks %u", st.frames, st.acks);
    CHECK(fs.decode_errs == 0 && fs.frames == st.frames && h.max_in_flight <= 2);
    CHECK(fs.bk_msgs > 0 && fs.tiles_msgs > 0);
    CHECK(st.decode_errs == 0 && st.ack_timeouts == 0 && st.shadow_resets == 0);
    /* idle for 10 s: LEDs at 1 Hz keep the session alive */
    {
        uint32_t leds_before = st.leds_sent;
        step(&h, 10000);
        rfm_session_stats(h.s, &st);
        rfm_fake_stats(unit, &fs);
        CHECKF(st.leds_sent - leds_before >= 9 && st.leds_sent - leds_before <= 12, "%u LEDS in 10 s",
               st.leds_sent - leds_before);
        CHECK(fs.keepalive_expired == 0 && rfm_fake_session(unit) == 2 && rfm_session_state(h.s) == RFM_S_ACTIVE);
        CHECK(st.panel_r >= 19);                        /* every 500 ms */
    }
    /* stop: SESSION 0, ACK */
    rfm_session_stop(h.s, h.now);
    step(&h, 20);
    CHECK(rfm_session_state(h.s) == RFM_S_STOPPED && rfm_fake_session(unit) == 0);
    teardown(&h);
}

/* SysEx only (transport MIDI) at the measured rate: flow control holds 2 frames, the screen catches up */
static void test_sysex_flow(void)
{
    static uint16_t img[FR_N];
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_session_stats_t st;
    rfm_fake_stats_t fs;
    uint32_t t, seed = 3;
    rfm_session_cfg_default(&sc);
    sc.transport = RFM_T_MIDI;
    setup(&h, &sc);
    default_fake(&fc);
    fc.sx_rate_bps = 300000;
    add_fake(&h, &fc);
    step(&h, 30);
    CHECK(rfm_session_state(h.s) == RFM_S_ACTIVE && rfm_session_link_kind(h.s, rfm_session_frame_link(h.s)) ==
                                                       RFM_LINK_SX);
    CHECK(rfm_session_budget(h.s) == 14000u);
    for (t = 0; t < 40; t++) {                          /* full-screen noise: over budget every frame */
        fr_noise(img, &seed);
        rfm_session_set_frame(h.s, img);
        step(&h, 50);
    }
    rfm_session_stats(h.s, &st);
    CHECK(st.pending > 0);
    CHECK(run_until_converged(&h, h.f[0], img, 3000));
    rfm_session_stats(h.s, &st);
    rfm_fake_stats(h.f[0], &fs);
    CHECKF(h.max_in_flight == 2 && fs.bk_msgs <= 1 /* HELLO */ && fs.decode_errs == 0 && st.ack_timeouts == 0,
           "in flight %u, bulk msgs %u, decode errors %u, ack timeouts %u", h.max_in_flight, fs.bk_msgs,
           fs.decode_errs, st.ack_timeouts);
    CHECKF(st.ack_ms_max >= 40, "ACK latency max %u ms: the simulated wire should show", st.ack_ms_max);
    teardown(&h);
}

/* panel events, a dropped Note On repaired by PANEL_R */
static void test_panel_events(void)
{
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_fake_t *u;
    uint32_t i;
    int got_key = 0, got_btn = 0, got_enc = 0, got_master = 0, repaired = 0;
    rfm_session_cfg_default(&sc);
    setup(&h, &sc);
    default_fake(&fc);
    add_fake(&h, &fc);
    u = h.f[0];
    step(&h, 30);
    CHECK(rfm_session_state(h.s) == RFM_S_ACTIVE);
    step(&h, 600);
    h.nev = 0;
    rfm_fake_key(u, 5, 1);
    rfm_fake_button(u, 9, 1);
    rfm_fake_enc(u, 3, -2);
    rfm_fake_master(u, 777);
    step(&h, 2);
    for (i = 0; i < h.nev; i++) {
        const rfm_event_t *e = &h.ev[i];
        got_key |= e->type == RFM_EV_KEY && e->index == 5 && e->value == 1 && e->source == RFM_EVS_MIDI;
        got_btn |= e->type == RFM_EV_BUTTON && e->index == 9 && e->value == 1;
        got_enc |= e->type == RFM_EV_ENC && e->index == 3 && e->value == -2;
        got_master |= e->type == RFM_EV_MASTER && e->value == 777;
    }
    CHECK(got_key && got_btn && got_enc && got_master);
    h.nev = 0;
    rfm_fake_drop_midi(u, 1);
    rfm_fake_key(u, 5, 0);                              /* the Note Off is lost on the way */
    step(&h, 600);
    for (i = 0; i < h.nev; i++)
        repaired |= h.ev[i].type == RFM_EV_KEY && h.ev[i].index == 5 && h.ev[i].value == 0 &&
                    h.ev[i].source == RFM_EVS_RESYNC;
    CHECK(repaired && !(rfm_session_panel(h.s)->keys & (1u << 5)));
    h.nev = 0;
    rfm_fake_drop_midi(u, 1);
    rfm_fake_enc(u, 0, 3);                              /* lost detents */
    step(&h, 600);
    CHECK(h.nev == 1 && h.ev[0].type == RFM_EV_ENC && h.ev[0].index == 0 && h.ev[0].value == 3 &&
          h.ev[0].source == RFM_EVS_RESYNC);
    teardown(&h);
}

/* faults: a reboot mid-animation, a reboot while idle, a deaf unit (keepalive loss), a lost ACK, a decode error;
 * once with a unit that draws and acknowledges TILES outside a session (the FRAME_ACK after a reboot says shadow
 * reset), once with one that ignores them (only the PANEL_R heartbeat and the ACK timeout tell) */
static void test_faults(int tiles_outside)
{
    static uint16_t img[FR_N];
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_fake_t *u;
    rfm_session_stats_t st, st0;
    rfm_fake_stats_t fs;
    rfm_leds_t leds;
    uint32_t t;
    rfm_session_cfg_default(&sc);
    setup(&h, &sc);
    default_fake(&fc);
    fc.bk_rate_bps = 830000;
    fc.tiles_outside_session = tiles_outside;
    add_fake(&h, &fc);
    u = h.f[0];
    memset(&leds, 0, sizeof leds);
    leds.lit[2] = 0x1E;
    leds.flags = RFM_LEDS_F_PLAY_GREEN;
    rfm_session_set_leds(h.s, &leds);
    fr_ui(img, 0);
    rfm_session_set_frame(h.s, img);
    CHECK(run_until_converged(&h, u, img, 1000));

    /* 1. reboot during an animation */
    rfm_session_stats(h.s, &st0);
    for (t = 1; t < 20; t++) {
        fr_ui(img, t);
        rfm_session_set_frame(h.s, img);
        if (t == 8)
            rfm_fake_reboot(u);
        step(&h, 50);
    }
    CHECK(run_until_converged(&h, u, img, 4000));
    rfm_session_stats(h.s, &st);
    CHECKF(st.sessions == st0.sessions + 1, "sessions %u -> %u", st0.sessions, st.sessions);
    if (tiles_outside)
        CHECKF(st.shadow_resets == st0.shadow_resets + 1, "shadow resets %u", st.shadow_resets);
    CHECK(rfm_leds_equal(rfm_fake_leds(u), &leds));

    /* 2. reboot while idle (no frame to acknowledge): the heartbeat stops -> a new session */
    step(&h, 500);
    rfm_session_stats(h.s, &st0);
    rfm_fake_reboot(u);
    step(&h, 2500);
    CHECK(run_until_converged(&h, u, img, 2000));
    rfm_session_stats(h.s, &st);
    CHECKF(st.sessions == st0.sessions + 1 && st.lost == st0.lost + 1, "sessions %u lost %u", st.sessions,
           st.lost);
    CHECK(rfm_leds_equal(rfm_fake_leds(u), &leds));

    /* 3. keepalive loss: the unit hears nothing for 4 s (it ends the session after 3 s), then hears again */
    rfm_session_stats(h.s, &st0);
    rfm_fake_deaf(u, 1);
    for (t = 0; t < 80; t++) {
        fr_ui(img, 100 + t);
        rfm_session_set_frame(h.s, img);
        step(&h, 50);
    }
    rfm_fake_deaf(u, 0);
    rfm_fake_stats(u, &fs);
    CHECK(fs.keepalive_expired == 1);
    CHECK(run_until_converged(&h, u, img, 6000));
    rfm_session_stats(h.s, &st);
    CHECKF(st.sessions == st0.sessions + 1, "sessions %u -> %u", st0.sessions, st.sessions);
    CHECK(rfm_leds_equal(rfm_fake_leds(u), &leds));

    /* 4. a lost FRAME_ACK: timed out, the whole screen resent */
    rfm_session_stats(h.s, &st0);
    rfm_fake_drop_acks(u, 1);
    fr_ui(img, 300);
    rfm_session_set_frame(h.s, img);
    CHECK(run_until_converged(&h, u, img, 3000));
    rfm_session_stats(h.s, &st);
    CHECK(st.ack_timeouts == st0.ack_timeouts + 1);

    /* 5. a decode error: the screen resent */
    rfm_session_stats(h.s, &st0);
    rfm_fake_decode_error(u);
    fr_ui(img, 301);
    rfm_session_set_frame(h.s, img);
    CHECK(run_until_converged(&h, u, img, 2000));
    rfm_session_stats(h.s, &st);
    CHECK(st.decode_errs == st0.decode_errs + 1);
    CHECK(rfm_leds_equal(rfm_fake_leds(u), &leds));
    CHECK(h.max_in_flight <= 2);
    teardown(&h);
}

/* transport BULK with a unit without bulk: nothing found, rescans; and a unit that never answers */
static void test_not_found(void)
{
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_session_cfg_default(&sc);
    sc.transport = RFM_T_BULK;
    setup(&h, &sc);
    default_fake(&fc);
    fc.has_bulk = 0;
    add_fake(&h, &fc);
    step(&h, 3500);
    CHECK(rfm_session_state(h.s) == RFM_S_DISCOVER && h.rescans >= 3 && rfm_session_frame_link(h.s) < 0);
    teardown(&h);
    rfm_session_cfg_default(&sc);
    setup(&h, &sc);
    default_fake(&fc);
    fc.silent = 1;
    add_fake(&h, &fc);
    step(&h, 2500);
    CHECK(rfm_session_state(h.s) == RFM_S_DISCOVER && h.rescans >= 2);
    rfm_session_stop(h.s, h.now);
    CHECK(rfm_session_state(h.s) == RFM_S_STOPPED);
    teardown(&h);
}

/* a link failing under the session: back to discovery, which finds the unit's other link */
static int fail_writes;
static int w_fail(void *ctx, const uint8_t *p, uint32_t n)
{
    if (fail_writes)
        return -1;
    return w(ctx, p, n);
}
static void test_link_failure(void)
{
    static uint16_t img[FR_N];
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_session_cfg_default(&sc);
    setup(&h, &sc);
    default_fake(&fc);
    add_fake(&h, &fc);
    /* replace the bulk link's writer by one that can fail */
    rfm_session_remove_link(h.s, h.links[1].id, h.now);
    h.links[1].id = rfm_session_add_link(h.s, RFM_LINK_BK, w_fail, &h.links[1], "bulk");
    step(&h, 50);
    CHECK(rfm_session_link_kind(h.s, rfm_session_frame_link(h.s)) == RFM_LINK_BK);
    fail_writes = 1;
    fr_ui(img, 9);
    rfm_session_set_frame(h.s, img);
    step(&h, 600);
    CHECK(rfm_session_link_kind(h.s, rfm_session_frame_link(h.s)) == RFM_LINK_SX);   /* SysEx carries on */
    CHECK(run_until_converged(&h, h.f[0], img, 2000));
    fail_writes = 0;
    teardown(&h);
}

int main(void)
{
    test_discovery_and_frames();
    test_sysex_flow();
    test_panel_events();
    test_faults(1);
    test_faults(0);
    test_not_found();
    test_link_failure();
    return check_done("test_session");
}
