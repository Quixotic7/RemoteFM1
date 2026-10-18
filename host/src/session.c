/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The host session state machine. See rfm/session.h. */
#include "rfm/session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FLIGHT 8u

typedef struct link {
    int used, alive, kind, id;
    rfm_write_fn write;
    void *ctx;
    char name[64];
    rfm_rx_t *rx;
    uint32_t tx_seq;
    int hello_got, hello_ok;
    rfm_hello_t hello;
    struct rfm_session *s;
} link_t;

typedef struct {
    uint16_t frame;
    uint32_t t, body, msgs;
} flight_t;

struct rfm_session {
    rfm_session_cfg_t cfg;
    rfm_session_cb_t cb;
    link_t links[RFM_MAX_LINKS];
    int state, frame_link, midi_link, link_lost;
    rfm_hello_t hello;
    uint32_t now, t_state, t_sent;
    int tries, first_round;
    uint32_t last_tx, last_rx, last_led_tx, last_panel_r, next_frame, period;
    uint16_t frame_no;
    flight_t fl[MAX_FLIGHT];
    uint32_t nfl, max_in_flight;
    uint32_t cur_body, cur_msgs;                        /* the frame being emitted */
    int expect_reset;
    rfm_leds_t leds, leds_sent;
    int leds_valid;
    rfm_screen_t *screen;
    rfm_panel_t panel;
    uint32_t body_max, budget;
    rfm_session_stats_t st;
    uint8_t tx[RFM_BK_HDR_LEN + RFM_BK_BODY_MAX + 64];
};

static const char *state_names[] = {"discover", "starting", "active", "stopping", "stopped"};
const char *rfm_session_state_name(int st) { return st >= 0 && st <= RFM_S_STOPPED ? state_names[st] : "?"; }

void rfm_session_cfg_default(rfm_session_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->fps = 20;
    c->keepalive_ms = RFM_KEEPALIVE_MS_DEFAULT;
    c->hello_ms = 300;
    c->retry_ms = 1000;
    c->start_ms = 400;
    c->ack_timeout_ms = 1000;
    c->lost_ms = 2000;
    c->heartbeat_ms = 1600;
    c->stop_ms = 300;
    c->settle_ms = 60;
    c->transport = RFM_T_AUTO;
    c->host_name = "fm1remote";
}

static void logf_(rfm_session_t *s, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    if (!s->cb.log)
        return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    s->cb.log(s->cb.ctx, b);
}

static int after(uint32_t now, uint32_t t, uint32_t ms) { return (int32_t)(now - t) >= (int32_t)ms; }

static void set_state(rfm_session_t *s, int st, const char *why)
{
    s->state = st;
    s->t_state = s->now;
    if (s->cb.state)
        s->cb.state(s->cb.ctx, st, why);
}

/* ---------------------------------------------------------------- sending ---- */
static int send_msg(rfm_session_t *s, int id, uint8_t cmd, const uint8_t *body, uint32_t n)
{
    link_t *l;
    uint32_t len;
    if (id < 0 || id >= RFM_MAX_LINKS)
        return -1;
    l = &s->links[id];
    if (!l->used || !l->alive)
        return -1;
    if (l->kind == RFM_LINK_BK)
        len = rfm_bk_frame(cmd, body, n, l->tx_seq++, s->tx);
    else
        len = rfm_sx_frame(cmd, body, n, s->tx);
    if (l->write(l->ctx, s->tx, len)) {
        l->alive = 0;
        if (id == s->frame_link || id == s->midi_link)
            s->link_lost = 1;
        logf_(s, "link %s: write failed", l->name);
        return -1;
    }
    s->st.wire_bytes += len;
    if (id == s->frame_link)
        s->last_tx = s->now;
    return 0;
}

static void send_hellos(rfm_session_t *s)
{
    uint8_t b[16];
    uint32_t n = rfm_build_hello(b, s->cfg.host_name);
    int i;
    for (i = 0; i < RFM_MAX_LINKS; i++) {
        link_t *l = &s->links[i];
        if (!l->used || !l->alive)
            continue;
        l->hello_got = l->hello_ok = 0;
        rfm_rx_reset(l->rx);
        send_msg(s, i, RFM_HELLO, b, n);
    }
}

static void send_session(rfm_session_t *s, int start)
{
    uint8_t b[RFM_SESSION_LEN];
    rfm_build_session(b, start, start ? s->cfg.keepalive_ms : 0u);
    send_msg(s, s->frame_link, RFM_SESSION, b, RFM_SESSION_LEN);
    s->t_sent = s->now;
}

static void send_leds(rfm_session_t *s)
{
    uint8_t b[RFM_LEDS_LEN];
    rfm_build_leds(b, &s->leds);
    if (!send_msg(s, s->frame_link, RFM_LEDS, b, RFM_LEDS_LEN)) {
        s->leds_sent = s->leds;
        s->last_led_tx = s->now;
        s->st.leds_sent++;
    }
}

static void emit_tiles(void *ctx, const uint8_t *body, uint32_t n, int last)
{
    rfm_session_t *s = (rfm_session_t *)ctx;
    (void)last;
    send_msg(s, s->frame_link, RFM_TILES, body, n);
    s->cur_body += n;
    s->cur_msgs++;
    s->st.msgs++;
}

/* ---------------------------------------------------------------- states ---- */
static void enter_discover(rfm_session_t *s, const char *why)
{
    s->frame_link = s->midi_link = -1;
    s->nfl = 0;
    s->link_lost = 0;
    if (!s->first_round && s->cb.rescan)
        s->cb.rescan(s->cb.ctx);
    s->first_round = 0;
    s->st.discoveries++;
    set_state(s, RFM_S_DISCOVER, why);
    send_hellos(s);
    s->t_sent = s->now;
}

static void enter_starting(rfm_session_t *s, const char *why)
{
    s->tries = 1;
    s->nfl = 0;
    set_state(s, RFM_S_STARTING, why);
    send_session(s, 1);
}

static void enter_active(rfm_session_t *s)
{
    s->frame_no = 0;
    s->nfl = 0;
    s->expect_reset = 1;
    rfm_screen_reset_black(s->screen);                 /* the unit cleared its screen and shadow */
    rfm_panel_reset(&s->panel);
    memset(&s->leds_sent, 0, sizeof s->leds_sent);     /* .. and turned every LED off */
    s->last_led_tx = s->now - 1000u;
    s->next_frame = s->now;
    s->last_rx = s->now;
    s->last_tx = s->now;
    s->last_panel_r = s->now;
    s->st.sessions++;
    set_state(s, RFM_S_ACTIVE, "session started");
}

static uint32_t min_nz(uint32_t a, uint32_t b) { return !b ? a : !a ? b : a < b ? a : b; }

static int decide(rfm_session_t *s)
{
    int i, sx = -1, bk = -1, frame;
    for (i = 0; i < RFM_MAX_LINKS; i++) {
        const link_t *l = &s->links[i];
        if (!l->used || !l->alive || !l->hello_ok)
            continue;
        if (l->kind == RFM_LINK_SX && sx < 0)
            sx = i;
        if (l->kind == RFM_LINK_BK && bk < 0)
            bk = i;
    }
    frame = s->cfg.transport == RFM_T_MIDI ? sx : s->cfg.transport == RFM_T_BULK ? bk : bk >= 0 ? bk : sx;
    if (frame < 0)
        return 0;
    s->frame_link = frame;
    s->midi_link = sx;
    s->hello = s->links[frame].hello;
    if (s->links[frame].kind == RFM_LINK_BK) {
        s->body_max = min_nz(s->cfg.bk_body_max ? s->cfg.bk_body_max : RFM_BK_BODY_MAX, s->hello.bk_body_max);
        if (s->body_max > RFM_BK_BODY_MAX)
            s->body_max = RFM_BK_BODY_MAX;
    } else {
        s->body_max = min_nz(s->cfg.sx_body_max ? s->cfg.sx_body_max : RFM_SX_BODY_MAX, s->hello.sx_body_max);
        if (s->body_max > RFM_SX_BODY_MAX)
            s->body_max = RFM_SX_BODY_MAX;
    }
    s->budget = s->cfg.budget ? s->cfg.budget
                              : (s->links[frame].kind == RFM_LINK_BK ? RFM_BUDGET_BK_BPS : RFM_BUDGET_SX_BPS) /
                                    (s->cfg.fps ? s->cfg.fps : 20u);
    s->max_in_flight = s->hello.max_in_flight ? s->hello.max_in_flight : RFM_FRAMES_IN_FLIGHT;
    if (s->max_in_flight > MAX_FLIGHT)
        s->max_in_flight = MAX_FLIGHT;
    logf_(s, "found %s (fw %s, caps 0x%x): frames on %s, panel on %s, body max %u, budget %u B/frame, %u in flight",
          s->hello.name, s->hello.fw_ver, s->hello.caps, s->links[frame].name, sx >= 0 ? s->links[sx].name : "none",
          s->body_max, s->budget, s->max_in_flight);
    if (sx < 0)
        logf_(s, "no MIDI link answered: panel input comes from PANEL_R only (every 500 ms)");
    return 1;
}

/* ---------------------------------------------------------------- receiving ---- */
static void on_event(void *ctx, const rfm_event_t *ev)
{
    rfm_session_t *s = (rfm_session_t *)ctx;
    s->st.events++;
    if (s->cb.event)
        s->cb.event(s->cb.ctx, ev);
}

static void on_midi(void *ctx, const uint8_t *m, uint32_t n)
{
    link_t *l = (link_t *)ctx;
    rfm_session_t *s = l->s;
    if (l->id != s->midi_link)
        return;
    if (rfm_panel_midi(&s->panel, m, n, s->now))
        s->last_rx = s->now;
}

static void on_frame_ack(rfm_session_t *s, const rfm_frame_ack_t *a)
{
    uint32_t i, k;
    for (i = 0; i < s->nfl && s->fl[i].frame != a->frame; i++)
        ;
    if (i == s->nfl) {
        logf_(s, "FRAME_ACK %u: not in flight", a->frame);
        return;
    }
    {
        const uint32_t lat = s->now - s->fl[i].t;
        s->st.acks++;
        s->st.ack_ms_sum += lat;
        if (lat > s->st.ack_ms_max)
            s->st.ack_ms_max = lat;
        s->st.last_ack_ms = lat;
    }
    if (i > 0) {                                        /* an older frame never acknowledged: lost somewhere */
        logf_(s, "FRAME_ACK %u before %u: resending the screen", a->frame, s->fl[0].frame);
        rfm_screen_invalidate(s->screen);
    } else if (a->bytes && a->bytes + 4u * s->fl[i].msgs < s->fl[i].body) {
        logf_(s, "FRAME_ACK %u: %u of %u body bytes arrived: resending the screen", a->frame, a->bytes,
              s->fl[i].body);
        rfm_screen_invalidate(s->screen);
    }
    for (k = i + 1u; k < s->nfl; k++)
        s->fl[k - i - 1u] = s->fl[k];
    s->nfl -= i + 1u;
    if (a->status & RFM_ACK_DECODE_ERR) {
        s->st.decode_errs++;
        logf_(s, "FRAME_ACK %u: decode error: resending the screen", a->frame);
        rfm_screen_invalidate(s->screen);
    }
    if ((a->status & RFM_ACK_SHADOW_RESET) && !s->expect_reset) {
        s->st.shadow_resets++;
        logf_(s, "FRAME_ACK %u: the unit's shadow was reset: new session", a->frame);
        enter_starting(s, "shadow reset");
        return;
    }
    s->expect_reset = 0;
}

static void on_msg(void *ctx, uint8_t cmd, const uint8_t *b, uint32_t n)
{
    link_t *l = (link_t *)ctx;
    rfm_session_t *s = l->s;
    const int ours = l->id == s->frame_link || l->id == s->midi_link;
    if (ours)
        s->last_rx = s->now;
    switch (cmd) {
    case RFM_HELLO_R:
        if (rfm_parse_hello(b, n, &l->hello))
            return;
        l->hello_got = 1;
        l->hello_ok = rfm_hello_is_remotefm1(&l->hello);
        if (!l->hello_ok && s->state == RFM_S_DISCOVER)
            logf_(s, "link %s: \"%s\" proto %u answered: not RemoteFM1 v2", l->name, l->hello.name, l->hello.proto);
        break;
    case RFM_ACK:
        if (l->id != s->frame_link)
            return;
        if (s->state == RFM_S_STARTING)
            enter_active(s);
        else if (s->state == RFM_S_STOPPING)
            set_state(s, RFM_S_STOPPED, "session ended");
        break;
    case RFM_FRAME_ACK: {
        rfm_frame_ack_t a;
        if (l->id == s->frame_link && s->state == RFM_S_ACTIVE && !rfm_parse_frame_ack(b, n, &a))
            on_frame_ack(s, &a);
        break;
    }
    case RFM_PANEL_R: {
        rfm_panel_r_t p;
        if (!ours || rfm_parse_panel_r(b, n, &p))
            return;
        s->st.panel_r++;
        if (p.flags & RFM_PANEL_F_SESSION)
            s->last_panel_r = s->now;
        if (!(p.flags & RFM_PANEL_F_SESSION)) {
            if (s->state == RFM_S_ACTIVE) {
                logf_(s, "PANEL_R: the unit has no session: starting again");
                enter_starting(s, "the unit ended the session");
            }
            return;
        }
        if (s->state == RFM_S_ACTIVE || s->state == RFM_S_STARTING)
            rfm_panel_reconcile(&s->panel, &p, s->now);
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- API ---- */
rfm_session_t *rfm_session_new(const rfm_session_cfg_t *cfg, const rfm_session_cb_t *cb)
{
    rfm_session_t *s = (rfm_session_t *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (cfg)
        s->cfg = *cfg;
    else
        rfm_session_cfg_default(&s->cfg);
    if (!s->cfg.fps)
        s->cfg.fps = 20;
    if (cb)
        s->cb = *cb;
    s->screen = rfm_screen_new();
    if (!s->screen) {
        free(s);
        return NULL;
    }
    rfm_panel_init(&s->panel, s->cfg.settle_ms, on_event, s);
    s->period = 1000u / s->cfg.fps;
    if (!s->period)
        s->period = 1;
    s->frame_link = s->midi_link = -1;
    s->first_round = 1;
    s->state = RFM_S_STOPPED;
    s->t_sent = 0;
    s->state = -1;                                      /* the first poll enters DISCOVER */
    return s;
}

void rfm_session_free(rfm_session_t *s)
{
    int i;
    if (!s)
        return;
    for (i = 0; i < RFM_MAX_LINKS; i++)
        rfm_rx_free(s->links[i].rx);
    rfm_screen_free(s->screen);
    free(s);
}

int rfm_session_add_link(rfm_session_t *s, int kind, rfm_write_fn write, void *ctx, const char *name)
{
    int i;
    for (i = 0; i < RFM_MAX_LINKS; i++) {
        link_t *l = &s->links[i];
        if (l->used)
            continue;
        memset(l, 0, sizeof *l);
        l->rx = rfm_rx_new(kind, kind == RFM_LINK_BK ? RFM_BK_BODY_MAX + 64u : RFM_SX_BODY_MAX + 64u, on_msg,
                           kind == RFM_LINK_SX ? on_midi : NULL, l);
        if (!l->rx)
            return -1;
        l->used = l->alive = 1;
        l->kind = kind;
        l->id = i;
        l->write = write;
        l->ctx = ctx;
        l->s = s;
        snprintf(l->name, sizeof l->name, "%s", name ? name : (kind == RFM_LINK_BK ? "bulk" : "midi"));
        return i;
    }
    return -1;
}

void rfm_session_remove_link(rfm_session_t *s, int id, uint32_t now)
{
    link_t *l;
    if (id < 0 || id >= RFM_MAX_LINKS || !s->links[id].used)
        return;
    s->now = now;
    l = &s->links[id];
    rfm_rx_free(l->rx);
    l->rx = NULL;
    l->used = l->alive = 0;
    if (id == s->frame_link || id == s->midi_link) {
        s->link_lost = 1;
        if (id == s->midi_link)
            s->midi_link = -1;
    }
}

void rfm_session_rx(rfm_session_t *s, int id, const uint8_t *p, uint32_t n, uint32_t now)
{
    if (id < 0 || id >= RFM_MAX_LINKS || !s->links[id].used)
        return;
    s->now = now;
    rfm_rx_feed(s->links[id].rx, p, n);
}

void rfm_session_poll(rfm_session_t *s, uint32_t now)
{
    s->now = now;
    if (s->state < 0) {
        enter_discover(s, "start");
        return;
    }
    if (s->link_lost && (s->state == RFM_S_STARTING || s->state == RFM_S_ACTIVE)) {
        s->st.lost++;
        enter_discover(s, "link lost");
        return;
    }
    if (s->link_lost && s->state == RFM_S_STOPPING) {
        set_state(s, RFM_S_STOPPED, "link lost");
        return;
    }
    switch (s->state) {
    case RFM_S_DISCOVER: {
        int i, sx = 0, bk = 0, bk_links = 0, early;
        for (i = 0; i < RFM_MAX_LINKS; i++) {
            const link_t *l = &s->links[i];
            if (!l->used || !l->alive)
                continue;
            bk_links += l->kind == RFM_LINK_BK;
            if (l->hello_ok) {
                sx += l->kind == RFM_LINK_SX;
                bk += l->kind == RFM_LINK_BK;
            }
        }
        early = sx && (bk || !bk_links || s->cfg.transport == RFM_T_MIDI);
        if ((early || after(now, s->t_sent, s->cfg.hello_ms)) && decide(s)) {
            enter_starting(s, "unit found");
            return;
        }
        if (after(now, s->t_sent, s->cfg.retry_ms))
            enter_discover(s, "no unit answered");
        break;
    }
    case RFM_S_STARTING:
        if (after(now, s->t_sent, s->cfg.start_ms)) {
            if (++s->tries > 4) {
                enter_discover(s, "SESSION not acknowledged");
                return;
            }
            send_session(s, 1);
        }
        break;
    case RFM_S_ACTIVE: {
        if (after(now, s->last_rx, s->cfg.lost_ms)) {
            s->st.lost++;
            logf_(s, "nothing from the unit for %u ms", now - s->last_rx);
            enter_discover(s, "unit silent");
            return;
        }
        if (s->cfg.heartbeat_ms && after(now, s->last_panel_r, s->cfg.heartbeat_ms)) {   /* PANEL_R is the session's heartbeat (500 ms) */
            s->st.lost++;
            logf_(s, "no PANEL_R for %u ms: the unit's session is gone", now - s->last_panel_r);
            enter_starting(s, "no PANEL_R");
            return;
        }
        if (s->nfl && after(now, s->fl[0].t, s->cfg.ack_timeout_ms)) {
            s->st.ack_timeouts++;
            logf_(s, "FRAME_ACK %u timed out: resending the screen", s->fl[0].frame);
            s->nfl = 0;
            rfm_screen_invalidate(s->screen);
        }
        if (s->leds_valid && (!rfm_leds_equal(&s->leds, &s->leds_sent) || after(now, s->last_led_tx, 1000u)))
            send_leds(s);
        if (after(now, s->next_frame, 0) && s->nfl < s->max_in_flight && rfm_screen_pending(s->screen)) {
            rfm_enc_stats_t es;
            const uint16_t f = s->frame_no++;
            s->cur_body = s->cur_msgs = 0;
            rfm_screen_encode(s->screen, f, s->budget, s->body_max, emit_tiles, s, &es);
            if (s->state != RFM_S_ACTIVE)
                return;
            s->fl[s->nfl].frame = f;
            s->fl[s->nfl].t = now;
            s->fl[s->nfl].body = s->cur_body;
            s->fl[s->nfl].msgs = s->cur_msgs;
            s->nfl++;
            s->st.frames++;
            s->st.tiles += es.tiles;
            s->st.rects += es.rects;
            s->st.body_bytes += es.body_bytes;
            s->next_frame += s->period;
            if (after(now, s->next_frame, 0))
                s->next_frame = now + s->period;
        }
        if (s->cfg.keepalive_ms && after(now, s->last_tx, s->cfg.keepalive_ms / 3u)) {
            s->st.keepalives++;
            send_leds(s);
        }
        break;
    }
    case RFM_S_STOPPING:
        if (after(now, s->t_state, s->cfg.stop_ms))
            set_state(s, RFM_S_STOPPED, "no ACK to SESSION 0");
        break;
    default:
        break;
    }
}

void rfm_session_set_frame(rfm_session_t *s, const uint16_t *lcd) { rfm_screen_submit(s->screen, lcd); }

void rfm_session_set_leds(rfm_session_t *s, const rfm_leds_t *l)
{
    s->leds = *l;
    s->leds_valid = 1;
}

void rfm_session_stop(rfm_session_t *s, uint32_t now)
{
    s->now = now;
    if ((s->state == RFM_S_ACTIVE || s->state == RFM_S_STARTING) && s->frame_link >= 0) {
        set_state(s, RFM_S_STOPPING, "stop");
        send_session(s, 0);
        return;
    }
    if (s->state != RFM_S_STOPPING)
        set_state(s, RFM_S_STOPPED, "stop");
}

void rfm_session_restart(rfm_session_t *s, uint32_t now)
{
    s->now = now;
    enter_discover(s, "restart");
}

int rfm_session_state(const rfm_session_t *s) { return s->state < 0 ? RFM_S_DISCOVER : s->state; }
const rfm_hello_t *rfm_session_hello(const rfm_session_t *s) { return &s->hello; }
int rfm_session_frame_link(const rfm_session_t *s) { return s->frame_link; }
int rfm_session_midi_link(const rfm_session_t *s) { return s->midi_link; }
int rfm_session_link_kind(const rfm_session_t *s, int id)
{
    return id >= 0 && id < RFM_MAX_LINKS && s->links[id].used ? s->links[id].kind : -1;
}
uint32_t rfm_session_budget(const rfm_session_t *s) { return s->budget; }
const rfm_panel_t *rfm_session_panel(const rfm_session_t *s) { return &s->panel; }
const rfm_screen_t *rfm_session_screen(const rfm_session_t *s) { return s->screen; }

void rfm_session_stats(const rfm_session_t *s, rfm_session_stats_t *st)
{
    *st = s->st;
    st->in_flight = s->nfl;
    st->pending = rfm_screen_pending(s->screen);
}
