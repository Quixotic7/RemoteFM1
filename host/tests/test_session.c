/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The session against scripted fake units (fake.c), on a simulated millisecond clock: discovery among other
 * firmwares, frames and flow control, LEDs and keepalive, panel events and PANEL_R repair, shadow reset (a reboot),
 * keepalive loss (a deaf unit), a lost FRAME_ACK, a decode error, stop, transports; Bluetooth MIDI's pacing on a
 * slow link that loses what overflows the unit's input; Bluetooth's whole screen changes (HOLD): a moving source
 * shows only screens that were submitted, an older unit gets no HOLD, a stall, the option turned off mid-change. */
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
    rfm_session_stats(h.s, &st);
    CHECK(st.ble == 0 && st.wire_cap == 0 && st.frame_ms == 50u);    /* USB MIDI (sxq 960): as before */
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
    CHECKF(st.hold == 0 && st.held == 0 && fs.held_frames == 0, "USB: hold %u, held %u / %u", st.hold, st.held,
           fs.held_frames);                             /* (the fake has RFM_CAP_HOLD: USB never holds) */
    CHECK(!memcmp(rfm_fake_panel(h.f[0]), rfm_fake_shadow(h.f[0]), FR_N * 2));
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

/* ---------------------------------------------------------------- Bluetooth MIDI ---- */
/* The link as measured on a unit over Bluetooth MIDI (2026-10-27): ~6 KB/s host -> unit, and what is queued beyond
 * a few KB is lost (the unit's Bluetooth input, 256 packets, overflows: ble_rx_lost). Modelled: BT_RATE wire bytes a
 * second delivered in order, at most BT_QUEUE bytes undelivered (a message that would go beyond is lost whole), the
 * replies BT_BACK_MS late. */
#define BT_RATE 6000u
#define BT_QUEUE 3072u
#define BT_BACK_MS 40u
#define BT_Q 65536u
#define BT_NBACK 4096u

typedef struct {
    rfm_session_t *s;
    rfm_fake_t *f;
    int id, verbose;
    uint32_t now, credit;
    uint8_t q[BT_Q];
    uint32_t qr, qw;                                    /* host -> unit bytes in [qr, qw) */
    struct {
        uint32_t due, n;
        uint8_t b[512];
    } back[BT_NBACK];
    uint32_t br, bw;                                    /* unit -> host chunks, in order */
    uint32_t lost_msgs, max_queued, max_wire, max_in_flight, states[8];
    rfm_event_t ev[64];
    uint32_t nev;
} bt_t;
static bt_t BT;

static int bt_w(void *ctx, const uint8_t *p, uint32_t n)
{
    bt_t *b = (bt_t *)ctx;
    if (b->qw - b->qr + n > BT_QUEUE) {
        b->lost_msgs++;                                 /* the OS took it; the unit never sees it */
        return 0;
    }
    if (b->qw + n > BT_Q) {
        memmove(b->q, b->q + b->qr, b->qw - b->qr);
        b->qw -= b->qr;
        b->qr = 0;
    }
    memcpy(b->q + b->qw, p, n);
    b->qw += n;
    if (b->qw - b->qr > b->max_queued)
        b->max_queued = b->qw - b->qr;
    return 0;
}
static void bt_state(void *ctx, int st, const char *why)
{
    bt_t *b = (bt_t *)ctx;
    b->states[st]++;
    if (b->verbose)
        printf("  %6u ms  %s (%s)\n", b->now, rfm_session_state_name(st), why);
}
static void bt_log(void *ctx, const char *m)
{
    bt_t *b = (bt_t *)ctx;
    if (b->verbose)
        printf("  %6u ms  %s\n", b->now, m);
}
static void bt_event(void *ctx, const rfm_event_t *e)
{
    bt_t *b = (bt_t *)ctx;
    if (b->nev < 64)
        b->ev[b->nev++] = *e;
}

static void bt_step(bt_t *b, uint32_t ms)
{
    while (ms--) {
        rfm_session_stats_t st;
        uint32_t n;
        b->now++;
        b->credit += BT_RATE;                           /* milli-bytes */
        n = b->credit / 1000u;
        b->credit %= 1000u;
        if (n > b->qw - b->qr)
            n = b->qw - b->qr;
        if (n) {
            rfm_fake_rx_sx(b->f, b->q + b->qr, n, b->now);
            b->qr += n;
        }
        if (b->qr == b->qw) {
            b->qr = b->qw = 0;
            b->credit = 0;
        }
        rfm_fake_poll(b->f, b->now);
        while (b->bw - b->br < BT_NBACK &&
               (n = rfm_fake_take_sx(b->f, b->back[b->bw % BT_NBACK].b, sizeof b->back[0].b)) != 0) {
            b->back[b->bw % BT_NBACK].n = n;
            b->back[b->bw % BT_NBACK].due = b->now + BT_BACK_MS;
            b->bw++;
        }
        while (b->br != b->bw && (int32_t)(b->now - b->back[b->br % BT_NBACK].due) >= 0) {
            rfm_session_rx(b->s, b->id, b->back[b->br % BT_NBACK].b, b->back[b->br % BT_NBACK].n, b->now);
            b->br++;
        }
        rfm_session_poll(b->s, b->now);
        rfm_session_stats(b->s, &st);
        if (st.wire_in_flight > b->max_wire)
            b->max_wire = st.wire_in_flight;
        if (st.in_flight > b->max_in_flight)
            b->max_in_flight = st.in_flight;
    }
}

static int bt_converged(bt_t *b, const uint16_t *img, uint32_t max_ms)
{
    uint32_t t;
    for (t = 0; t < max_ms; t += 10) {
        rfm_session_stats_t st;
        bt_step(b, 10);
        rfm_session_stats(b->s, &st);
        if (rfm_session_state(b->s) == RFM_S_ACTIVE && rfm_fake_session(b->f) && !st.pending && !st.in_flight &&
            shadow_is(b->f, img))
            return 1;
    }
    return 0;
}

/* a busy, changing screen over Bluetooth for 20 s: an animated UI, page changes, LEDs; then still. ble = 1: the unit
 * answers as 0.2 does over Bluetooth (sxq 128): paced, nothing lost, nothing timed out, the screen converges. ble = 0:
 * the same link, but the HELLO_R says USB: the old pacing overruns it (the bug the owner saw). */
static void test_bluetooth(int ble)
{
    static uint16_t img[FR_N];
    bt_t *b = &BT;
    rfm_session_cfg_t sc;
    rfm_session_cb_t cb;
    rfm_fake_cfg_t fc;
    rfm_session_stats_t st;
    rfm_fake_stats_t fs;
    rfm_leds_t leds;
    uint32_t t;
    int key = 0;
    memset(b, 0, sizeof *b);
    b->verbose = getenv("RFM_VERBOSE") != NULL;
    b->now = 1000;
    rfm_session_cfg_default(&sc);                       /* as the web player: auto, 20 fps, budget from the link */
    memset(&cb, 0, sizeof cb);
    cb.ctx = b;
    cb.state = bt_state;
    cb.log = bt_log;
    cb.event = bt_event;
    b->s = rfm_session_new(&sc, &cb);
    rfm_fake_cfg_default(&fc);
    fc.has_bulk = 0;
    fc.ble = ble;
    b->f = rfm_fake_new(&fc);
    b->id = rfm_session_add_link(b->s, RFM_LINK_SX, bt_w, b, "FM-1 7938");
    bt_step(b, 600);
    CHECK(rfm_session_state(b->s) == RFM_S_ACTIVE);
    rfm_session_stats(b->s, &st);
    if (ble)
        CHECKF(st.ble == 1 && rfm_session_budget(b->s) == RFM_BUDGET_BLE_BPS / 10u && st.frame_ms == 50u &&
                   st.wire_cap == 2048u && st.lossy == RFM_BLE_COLORS,
               "ble %u budget %u frame %u ms cap %u lossy %u", st.ble, rfm_session_budget(b->s), st.frame_ms,
               st.wire_cap, st.lossy);
    else
        CHECK(st.ble == 0 && rfm_session_budget(b->s) == 14000u && st.frame_ms == 50u && st.wire_cap == 0 &&
              st.lossy == 0);
    memset(&leds, 0, sizeof leds);
    for (t = 0; t < 400; t++) {                         /* 20 s at 20 Hz */
        if (t / 100u % 2u == 1u && t % 100u < 10u) {     /* a page change now and then */
            fr_fill(img, fr_rgb(60 + t % 7u, 20, 80));
            fr_rect(img, 10, 40 + (int)(t % 10u) * 16, 220, 12, fr_rgb(240, 200, 40));
            fr_text(img, 20, 20, "PAGE 2", fr_rgb(255, 255, 255), fr_rgb(60 + t % 7u, 20, 80));
        } else {
            fr_ui(img, t);
        }
        rfm_session_set_frame(b->s, img);
        if (t % 5u == 0) {
            leds.lit[t / 5u % 11u] = (uint8_t)(t * 3u);
            rfm_session_set_leds(b->s, &leds);
        }
        if (t == 200)
            rfm_fake_key(b->f, 7, 1);
        bt_step(b, 50);
        if (t == 202) {
            uint32_t i;
            for (i = 0; i < b->nev; i++)
                key |= b->ev[i].type == RFM_EV_KEY && b->ev[i].index == 7 && b->ev[i].value == 1;
        }
    }
    rfm_session_stats(b->s, &st);
    rfm_fake_stats(b->f, &fs);
    if (b->verbose)
        printf("  bluetooth %s: %u frames, %u acks, ack max %u ms, %u timeouts, %u msgs lost, queue max %u B, "
               "%u B in flight max, body %u B (%u B/s), %u tiles reduced\n", ble ? "paced" : "as USB", st.frames,
               st.acks, st.ack_ms_max, st.ack_timeouts, b->lost_msgs, b->max_queued, b->max_wire, st.body_bytes,
               st.body_bytes / 20u, st.reduced);
    if (!ble) {
        CHECKF(b->lost_msgs > 0 && st.ack_timeouts > 0, "USB pacing on the Bluetooth link: %u lost, %u timeouts",
               b->lost_msgs, st.ack_timeouts);
    } else {
        CHECK(key);                                     /* the panel still comes through */
        CHECK(bt_converged(b, img, 30000));
        rfm_session_stats(b->s, &st);
        rfm_fake_stats(b->f, &fs);
        CHECKF(b->lost_msgs == 0 && b->max_queued <= BT_QUEUE, "%u messages lost, queue max %u", b->lost_msgs,
               b->max_queued);
        CHECKF(st.ack_timeouts == 0 && st.decode_errs == 0 && st.shadow_resets == 0 && fs.decode_errs == 0,
               "ack timeouts %u, decode errors %u / %u, shadow resets %u", st.ack_timeouts, st.decode_errs,
               fs.decode_errs, st.shadow_resets);
        CHECKF(st.sessions == 1 && st.lost == 0 && b->states[RFM_S_STARTING] == 1 && b->states[RFM_S_DISCOVER] == 1,
               "sessions %u, lost %u, starting %u, discover %u", st.sessions, st.lost, b->states[RFM_S_STARTING],
               b->states[RFM_S_DISCOVER]);
        CHECKF(b->max_wire <= RFM_BLE_WIRE_CAP && b->max_in_flight <= 2, "%u B / %u frames in flight", b->max_wire,
               b->max_in_flight);
        CHECKF(st.frames >= 100 && st.acks == st.frames, "frames %u acks %u", st.frames, st.acks);
        CHECKF(st.body_bytes / 20u <= RFM_BUDGET_BLE_BPS + RFM_BUDGET_BLE_BPS / 4u, "%u B/s of body",
               st.body_bytes / 20u);
        CHECK(rfm_leds_equal(rfm_fake_leds(b->f), &leds));
        CHECK(fs.keepalive_expired == 0);
        /* idle for 10 s: LEDs and the heartbeat keep it */
        bt_step(b, 10000);
        rfm_session_stats(b->s, &st);
        CHECK(rfm_session_state(b->s) == RFM_S_ACTIVE && st.sessions == 1 && st.lost == 0);
    }
    rfm_session_free(b->s);
    rfm_fake_free(b->f);
}

/* a Bluetooth session on the modelled link (BT_*); colors: cfg.ble_colors; unit_hold: the fake has RFM_CAP_HOLD;
 * hold: cfg.ble_hold */
static void bt_open2(bt_t *b, uint32_t colors, int unit_hold, int hold)
{
    rfm_session_cfg_t sc;
    rfm_session_cb_t cb;
    rfm_fake_cfg_t fc;
    memset(b, 0, sizeof *b);
    b->verbose = getenv("RFM_VERBOSE") != NULL;
    b->now = 1000;
    rfm_session_cfg_default(&sc);
    sc.ble_colors = colors;
    sc.ble_hold = hold;
    memset(&cb, 0, sizeof cb);
    cb.ctx = b;
    cb.state = bt_state;
    cb.log = bt_log;
    cb.event = bt_event;
    b->s = rfm_session_new(&sc, &cb);
    rfm_fake_cfg_default(&fc);
    fc.has_bulk = 0;
    fc.ble = 1;
    fc.hold = unit_hold;
    b->f = rfm_fake_new(&fc);
    b->id = rfm_session_add_link(b->s, RFM_LINK_SX, bt_w, b, "FM-1 7938");
    bt_step(b, 600);
    CHECK(rfm_session_state(b->s) == RFM_S_ACTIVE);
}
static void bt_open(bt_t *b, uint32_t colors) { bt_open2(b, colors, 1, 1); }

static void bt_close(bt_t *b)
{
    rfm_session_free(b->s);
    rfm_fake_free(b->f);
}

/* steps until nothing is pending or in flight (a refinement not yet queued); the ms it took */
static uint32_t bt_settle(bt_t *b, uint32_t max_ms)
{
    uint32_t t;
    for (t = 0; t < max_ms; t += 5) {
        rfm_session_stats_t st;
        rfm_session_stats(b->s, &st);
        if (!st.pending && !st.in_flight)
            return t;
        bt_step(b, 5);
    }
    return max_ms;
}

/* a page with anti-aliased text and a gradient band: tiles of many colours */
static void lossy_page(uint16_t *img, uint32_t t)
{
    static uint16_t g[FR_N];
    fr_ui(img, t);
    fr_gradient(g, t);
    memcpy(img + 168 * FR_W, g + 168 * FR_W, (FR_H - 168) * FR_W * 2u);
}

static int shadow_is_model(bt_t *b)
{
    return !memcmp(rfm_fake_shadow(b->f), rfm_screen_model(rfm_session_screen(b->s)), FR_N * 2);
}

/* Bluetooth's lossy tiles: a page change costs fewer bytes than lossless; the unit shows the reduction, which is not
 * sent again while the screen is still (no resends, no flicker); after ble_refine_ms still, the reduced tiles go
 * again exactly and the unit converges to the exact image; a change during the refinement goes first (the
 * refinement is dropped, then done again); turning lossy off sends the reduced tiles exactly at once */
static void test_bluetooth_lossy(void)
{
    static uint16_t img[FR_N], img2[FR_N], shown[FR_N];
    bt_t *b = &BT;
    rfm_session_stats_t st, st0;
    uint32_t exact_bytes, lossy_bytes, ms, t;

    /* lossless over Bluetooth (ble_colors 0): the page's bytes */
    bt_open(b, 0);
    lossy_page(img, 5);
    rfm_session_set_frame(b->s, img);
    bt_settle(b, 30000);
    rfm_session_stats(b->s, &st);
    exact_bytes = st.body_bytes;
    CHECKF(shadow_is(b->f, img) && st.reduced == 0 && st.inexact == 0 && st.lossy == 0, "reduced %u inexact %u",
           st.reduced, st.inexact);
    bt_close(b);

    /* lossy (the default) */
    bt_open(b, RFM_BLE_COLORS);
    rfm_session_set_frame(b->s, img);
    ms = bt_settle(b, 30000);
    rfm_session_stats(b->s, &st);
    lossy_bytes = st.body_bytes;
    if (b->verbose)
        printf("  lossy page: %u B (lossless %u B), %u tiles reduced, %u ms\n", lossy_bytes, exact_bytes, st.reduced,
               ms);
    CHECKF(st.reduced > 20u && st.inexact == st.reduced && lossy_bytes * 10u < exact_bytes * 8u,
           "reduced %u inexact %u, %u B lossy vs %u B exact", st.reduced, st.inexact, lossy_bytes, exact_bytes);
    CHECK(shadow_is_model(b) && !shadow_is(b->f, img));
    /* still, under ble_refine_ms: nothing is sent, the unit's image stays as it is */
    memcpy(shown, rfm_fake_shadow(b->f), sizeof shown);
    st0 = st;
    for (t = 0; t < 16; t++) {
        rfm_session_set_frame(b->s, img);
        bt_step(b, 50);
        rfm_session_stats(b->s, &st);
        CHECKF(st.frames == st0.frames && !memcmp(rfm_fake_shadow(b->f), shown, sizeof shown), "%u frames, %u",
               st.frames, st0.frames);
    }
    /* then the refinement: exact */
    CHECK(bt_converged(b, img, 10000));
    rfm_session_stats(b->s, &st);
    CHECKF(st.inexact == 0 && st.refined == st0.reduced, "inexact %u, refined %u of %u", st.inexact, st.refined,
           st0.reduced);
    st0 = st;
    for (t = 0; t < 40; t++) {                          /* exact and still: nothing more */
        rfm_session_set_frame(b->s, img);
        bt_step(b, 50);
    }
    rfm_session_stats(b->s, &st);
    CHECK(st.frames == st0.frames && shadow_is(b->f, img));

    /* a new page, still until its refinement starts, then a change: the change goes first */
    lossy_page(img2, 9);
    rfm_session_set_frame(b->s, img2);
    bt_settle(b, 30000);
    for (t = 0; t < 2000u && !rfm_screen_refining(rfm_session_screen(b->s)); t += 5) {
        rfm_session_set_frame(b->s, img2);
        bt_step(b, 5);
    }
    CHECK(rfm_screen_refining(rfm_session_screen(b->s)) > 2u);
    bt_step(b, 5);
    fr_rect(img2, 0, 0, 64, 32, fr_rgb(255, 0, 0));    /* the change */
    rfm_session_set_frame(b->s, img2);
    bt_step(b, 1);
    CHECK(rfm_screen_refining(rfm_session_screen(b->s)) == 0);   /* dropped while it goes */
    CHECK(bt_converged(b, img2, 10000));                /* and done again once still */
    rfm_session_stats(b->s, &st);
    CHECK(st.inexact == 0 && st.ack_timeouts == 0 && b->lost_msgs == 0 && st.sessions == 1);

    /* lossy off at once: the reduced tiles are sent exactly without waiting */
    rfm_session_set_frame(b->s, img);
    bt_settle(b, 30000);
    rfm_session_stats(b->s, &st);
    CHECK(st.inexact > 0);
    rfm_session_set_ble_colors(b->s, 0);
    bt_settle(b, 30000);
    rfm_session_stats(b->s, &st);
    CHECKF(st.inexact == 0 && st.lossy == 0 && shadow_is(b->f, img), "inexact %u lossy %u", st.inexact, st.lossy);
    bt_close(b);
}

/* ---------------------------------------------------------------- Bluetooth: whole screen changes ---- */
/* a knob turned: blocks and a number, each change a few frames' worth over Bluetooth (~2 KB lossless) */
static void knob(uint16_t *f, uint32_t t)
{
    char b[16];
    uint32_t k;
    fr_ui(f, 0);
    for (k = 0; k < 3u; k++)
        fr_rect(f, 8 + (int)((t * 37u + k * 53u) % 170u), 56 + (int)((t * 19u + k * 29u) % 120u), 40, 24,
                fr_rgb(t * 40u + k * 30u, 200u - k * 20u, 100u + t * 10u));
    snprintf(b, sizeof b, "%3u", t % 1000u);
    fr_text(f, 20, 30, b, fr_rgb(240, 240, 240), fr_rgb(36, 40, 52));
}
static uint64_t fnv(const void *p, uint32_t n)
{
    const uint8_t *q = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i < n; i++)
        h = (h ^ q[i]) * 1099511628211ull;
    return h;
}
static int panel_is(bt_t *b, const uint16_t *img) { return !memcmp(rfm_fake_panel(b->f), img, FR_N * 2); }
static int panel_is_shadow(bt_t *b) { return !memcmp(rfm_fake_panel(b->f), rfm_fake_shadow(b->f), FR_N * 2); }

/* steps until the unit holds tiles (a change part sent); 0: it never did */
static int bt_until_holding(bt_t *b, uint32_t max_ms)
{
    uint32_t t;
    for (t = 0; t < max_ms; t++) {
        if (rfm_fake_holding(b->f))
            return 1;
        bt_step(b, 1);
    }
    return 0;
}

/* a moving source over Bluetooth, lossless (the unit's panel can then be compared with the screens submitted):
 * with HOLD every screen the panel shows was submitted, the old one stays until the new one is all there; without
 * it (ble_hold 0) the panel shows screens that never were. An older unit (no RFM_CAP_HOLD) gets no HOLD. A stall
 * mid-change: the unit shows what it holds after RFM_HOLD_MS, then everything converges. ble_hold off mid-change:
 * the change goes on unheld, nothing stays held. */
static void test_bluetooth_hold(void)
{
    static uint16_t img[FR_N], prev[FR_N];
    static uint64_t src[256];
    bt_t *b = &BT;
    rfm_session_stats_t st;
    rfm_fake_stats_t fs;
    uint32_t t, k, i, nsrc, shown, bad, last, pass;

    for (pass = 0; pass < 2; pass++) {
        const int hold = pass == 0;
        bt_open2(b, 0, 1, hold);
        rfm_session_stats(b->s, &st);
        CHECKF(st.ble == 1 && st.hold == (uint32_t)hold && st.lossy == 0, "ble %u hold %u lossy %u", st.ble,
               st.hold, st.lossy);
        memset(img, 0, sizeof img);
        nsrc = 0;
        src[nsrc++] = fnv(img, sizeof img);             /* black: the session's start */
        shown = bad = 0;
        rfm_fake_stats(b->f, &fs);
        last = fs.panel_updates;
        for (t = 0; t < 100; t++) {                     /* 20 changes a second for 5 s */
            knob(img, t);
            src[nsrc++] = fnv(img, sizeof img);
            rfm_session_set_frame(b->s, img);
            for (k = 0; k < 50; k++) {
                bt_step(b, 1);
                rfm_fake_stats(b->f, &fs);
                if (fs.panel_updates != last) {
                    const uint64_t h = fnv(rfm_fake_panel(b->f), FR_N * 2);
                    last = fs.panel_updates;
                    shown++;
                    for (i = 0; i < nsrc && src[i] != h; i++)
                        ;
                    bad += i == nsrc;
                }
            }
        }
        CHECK(bt_converged(b, img, 30000));
        CHECK(panel_is(b, img));
        rfm_session_stats(b->s, &st);
        rfm_fake_stats(b->f, &fs);
        if (b->verbose)
            printf("  knob over Bluetooth, %s: 100 changes in 5 s, the panel changed %u times, %u of them to a screen "
                   "never submitted; %u frames, %u held\n", hold ? "HOLD" : "no HOLD", shown, bad, st.frames, st.held);
        if (hold)
            CHECKF(bad == 0 && shown >= 5 && st.held > 0 && fs.held_frames == st.held && fs.hold_timeouts == 0 &&
                       st.ack_timeouts == 0 && b->lost_msgs == 0,
                   "HOLD: %u of %u shown never submitted, held %u / %u, hold timeouts %u, ack timeouts %u, lost %u",
                   bad, shown, st.held, fs.held_frames, fs.hold_timeouts, st.ack_timeouts, b->lost_msgs);
        else
            CHECKF(bad > 0 && st.held == 0 && fs.held_frames == 0, "no HOLD: %u of %u shown never submitted, held %u",
                   bad, shown, st.held);
        bt_close(b);
    }

    /* an older unit (0.2.1: no RFM_CAP_HOLD): no HOLD goes out, every frame shows */
    bt_open2(b, RFM_BLE_COLORS, 0, 1);
    for (t = 0; t < 20; t++) {
        knob(img, t);
        rfm_session_set_frame(b->s, img);
        bt_step(b, 50);
    }
    bt_settle(b, 30000);
    rfm_session_stats(b->s, &st);
    rfm_fake_stats(b->f, &fs);
    CHECKF(st.hold == 0 && st.held == 0 && fs.held_frames == 0 && shadow_is_model(b) && panel_is_shadow(b),
           "older unit: hold %u, held %u / %u", st.hold, st.held, fs.held_frames);
    bt_close(b);

    /* a stall in the middle of a change: the unit hears nothing; it keeps the old screen for RFM_HOLD_MS, then shows
     * what it holds; when it hears again, the screen is sent again and all agree */
    bt_open2(b, 0, 1, 1);
    knob(prev, 1);
    rfm_session_set_frame(b->s, prev);
    CHECK(bt_converged(b, prev, 30000) && panel_is(b, prev));
    knob(img, 2);
    rfm_session_set_frame(b->s, img);
    CHECK(bt_until_holding(b, 2000));
    rfm_fake_deaf(b->f, 1);
    bt_step(b, RFM_HOLD_MS - 100u);
    rfm_fake_stats(b->f, &fs);
    CHECKF(rfm_fake_holding(b->f) && panel_is(b, prev) && fs.hold_timeouts == 0, "stall: holding %d, old screen %d",
           rfm_fake_holding(b->f), panel_is(b, prev));
    bt_step(b, 200);
    rfm_fake_stats(b->f, &fs);
    CHECKF(!rfm_fake_holding(b->f) && fs.hold_timeouts == 1 && panel_is_shadow(b), "stall: holding %d, timeouts %u",
           rfm_fake_holding(b->f), fs.hold_timeouts);
    rfm_fake_deaf(b->f, 0);
    CHECK(bt_converged(b, img, 30000));
    rfm_session_stats(b->s, &st);
    CHECKF(panel_is(b, img) && st.sessions == 1 && st.ack_timeouts >= 1, "after the stall: sessions %u, timeouts %u",
           st.sessions, st.ack_timeouts);

    /* ble_hold off in the middle of a change: the rest goes unheld and shows; on again */
    knob(img, 3);
    rfm_session_set_frame(b->s, img);
    CHECK(bt_until_holding(b, 2000));
    rfm_session_set_ble_hold(b->s, 0);
    CHECK(bt_converged(b, img, 30000));
    rfm_session_stats(b->s, &st);
    CHECK(st.hold == 0 && !rfm_fake_holding(b->f) && panel_is(b, img));
    rfm_session_set_ble_hold(b->s, 1);
    rfm_session_stats(b->s, &st);
    CHECK(st.hold == 1);
    bt_close(b);
}

/* a unit on Bluetooth MIDI and USB MIDI at once (two SysEx ports, no bulk; the Bluetooth bit set on both): frames
 * and the panel go over USB, paced as before */
static void test_usb_before_bluetooth(void)
{
    h_t h;
    rfm_session_cfg_t sc;
    rfm_fake_cfg_t fc;
    rfm_session_stats_t st;
    rfm_session_cfg_default(&sc);
    setup(&h, &sc);
    default_fake(&fc);
    fc.has_bulk = 0;
    fc.ble = 1;
    add_fake(&h, &fc);                                  /* the Bluetooth port, added first */
    default_fake(&fc);
    fc.has_bulk = 0;
    fc.ble = 2;
    add_fake(&h, &fc);                                  /* USB */
    step(&h, 30);
    CHECK(rfm_session_state(h.s) == RFM_S_ACTIVE);
    CHECK(rfm_session_frame_link(h.s) == h.links[1].id && rfm_session_midi_link(h.s) == h.links[1].id);
    rfm_session_stats(h.s, &st);
    CHECK(st.ble == 0 && st.wire_cap == 0 && st.frame_ms == 50u && rfm_session_budget(h.s) == 14000u);
    CHECK(rfm_fake_session(h.f[1]) == 1 && rfm_fake_session(h.f[0]) == 0);
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
    test_bluetooth(1);
    test_bluetooth(0);
    test_bluetooth_lossy();
    test_bluetooth_hold();
    test_usb_before_bluetooth();
    return check_done("test_session");
}
