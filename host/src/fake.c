/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* A fake RemoteFM1 unit (the device side of protocol v2) for tests and fm1remote's loopback. See rfm/fake.h. */
#include "rfm/fake.h"

#include <stdlib.h>
#include <string.h>

#include "rfm/tiles_ref.h"

#define OUTQ 262144u
#define MAXACK 16u

typedef struct {
    uint8_t *b;
    uint32_t r, w;                                      /* bytes in [r, w) of a linear buffer, compacted on take */
} outq_t;

typedef struct {
    uint16_t frame;
    uint8_t status;
    uint32_t bytes;
    uint64_t due_us;                                    /* the simulated wire's time, in us */
    int link;
} pend_ack_t;

struct rfm_fake {
    rfm_fake_cfg_t cfg;
    rfm_rx_t *rx[2];
    outq_t out[2];
    uint32_t bk_seq;
    uint32_t now;
    uint64_t busy_us[2];                                /* the simulated wire: busy until (us) */
    int session;                                        /* 0, 1 + link */
    uint32_t touch, touch_msgs;                         /* TOUCH's flags in this session; TOUCHes taken */
    uint32_t keepalive_ms, last_rx, last_panel_r;
    uint8_t shadow[RFM_W * RFM_H * 2];
    uint8_t panel[RFM_W * RFM_H * 2];                   /* what the unit shows */
    int held;                                           /* tiles in the shadow not on the panel (HOLD) */
    uint32_t tiles_ms;                                  /* the last TILES (RFM_HOLD_MS) */
    rfm_leds_t leds;
    /* the panel */
    uint32_t keys, buttons;
    int16_t enc[RFM_NENC];
    uint16_t master;
    /* the frame being received */
    uint32_t frame_bytes;
    uint8_t frame_err, reset_pending;
    pend_ack_t acks[MAXACK];
    uint32_t nacks;
    /* faults */
    int deaf, decode_err_next;
    uint32_t drop_acks, drop_midi;
    int cur_link;                                       /* the link the message being handled came on */
    rfm_fake_stats_t st;
};

void rfm_fake_cfg_default(rfm_fake_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->name = RFM_NAME;
    c->proto = RFM_PROTO;
    c->has_bulk = 1;
    c->sx_body_max = RFM_SX_BODY_MAX;
    c->bk_body_max = RFM_BK_BODY_MAX;
    c->tiles_outside_session = 1;
    c->hold = 1;
    c->midi_in = 1;
    c->touch = 1;
}

static void show(rfm_fake_t *f)
{
    memcpy(f->panel, f->shadow, sizeof f->panel);
    f->held = 0;
    f->st.panel_updates++;
}

/* ---------------------------------------------------------------- output ---- */
static void out_put(rfm_fake_t *f, int link, const uint8_t *p, uint32_t n)
{
    outq_t *q = &f->out[link];
    if (q->w + n > OUTQ) {
        if (q->r) {
            memmove(q->b, q->b + q->r, q->w - q->r);
            q->w -= q->r;
            q->r = 0;
        }
        if (q->w + n > OUTQ)
            return;                                     /* the host is not reading: dropped, as a full FIFO */
    }
    memcpy(q->b + q->w, p, n);
    q->w += n;
}

static uint32_t out_take(outq_t *q, uint8_t *out, uint32_t cap)
{
    uint32_t n = q->w - q->r;
    if (n > cap)
        n = cap;
    memcpy(out, q->b + q->r, n);
    q->r += n;
    if (q->r == q->w)
        q->r = q->w = 0;
    return n;
}

static void reply(rfm_fake_t *f, int link, uint8_t cmd, const uint8_t *body, uint32_t n)
{
    uint8_t buf[RFM_BK_HDR_LEN + 1200];
    uint32_t len;
    if (f->cfg.silent)
        return;
    if (link == RFM_LINK_BK) {
        if (!f->cfg.has_bulk)
            return;
        len = rfm_bk_frame(cmd, body, n, f->bk_seq++, buf);
    } else {
        len = rfm_sx_frame(cmd, body, n, buf);
    }
    out_put(f, link, buf, len);
}

static void midi3(rfm_fake_t *f, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m[3];
    m[0] = a;
    m[1] = b;
    m[2] = c;
    if (f->drop_midi) {
        f->drop_midi--;
        return;
    }
    out_put(f, RFM_LINK_SX, m, 3);
}

static void send_master(rfm_fake_t *f)
{
    uint32_t v14 = (uint32_t)f->master << 4;
    midi3(f, 0xB0 | RFM_MIDI_BTN_CH, RFM_MIDI_MASTER_MSB, (uint8_t)(v14 >> 7));
    midi3(f, 0xB0 | RFM_MIDI_BTN_CH, RFM_MIDI_MASTER_LSB, (uint8_t)(v14 & 0x7Fu));
}

static void send_panel_r(rfm_fake_t *f, int link)
{
    rfm_panel_r_t p;
    uint8_t b[RFM_PANEL_LEN];
    p.keys = f->keys;
    p.buttons = (uint16_t)f->buttons;
    memcpy(p.enc, f->enc, sizeof p.enc);
    p.master = f->master;
    p.battery_mv = 3900;
    p.flags = (uint8_t)(f->session ? RFM_PANEL_F_SESSION | (uint32_t)f->touch << 1 : 0u);
    rfm_build_panel_r(b, &p);
    reply(f, link, RFM_PANEL_R, b, RFM_PANEL_LEN);
    f->st.panel_r++;
    f->last_panel_r = f->now;
}

/* ---------------------------------------------------------------- input ---- */
static void on_msg(void *ctx, uint8_t cmd, const uint8_t *b, uint32_t n)
{
    rfm_fake_t *f = (rfm_fake_t *)ctx;
    const int link = f->cur_link;
    if (f->deaf || f->cfg.silent)
        return;
    f->st.msgs++;
    if (link == RFM_LINK_SX)
        f->st.sx_msgs++;
    else
        f->st.bk_msgs++;
    f->last_rx = f->now;
    switch (cmd) {
    case RFM_HELLO: {
        rfm_hello_t h;
        uint8_t body[RFM_HELLO_LEN];
        memset(&h, 0, sizeof h);
        strncpy(h.name, f->cfg.name, 16);
        h.proto = f->cfg.proto;
        strncpy(h.ver, "fake", 8);
        h.transports = (uint8_t)(1u | (f->cfg.has_bulk ? 2u : 0u) |
                                 (f->cfg.ble ? RFM_TR_BLE | RFM_TR_BLE_HEARD : 0u));
        h.sxq = (uint16_t)(f->cfg.ble == 1 && link == RFM_LINK_SX ? 128u : 1024u - 64u);  /* firmware/src/rfm_main.c */
        h.usb_mode = 1;
        h.lcd_baud = 2;
        h.sx_body_max = f->cfg.sx_body_max;
        h.bk_body_max = f->cfg.bk_body_max;
        strncpy(h.identity, "FM-1_015", 8);
        h.v2 = 1;
        h.screen_w = RFM_W;
        h.screen_h = RFM_H;
        h.caps = RFM_CAP_SHADOW | RFM_CAP_COPY | RFM_CAP_RLE | RFM_CAP_PAL | RFM_CAP_BATTERY | RFM_CAP_PLAY_GREEN |
                 RFM_CAP_LED5 | (f->cfg.hold ? RFM_CAP_HOLD : 0u) | (f->cfg.midi_in ? RFM_CAP_MIDI_IN : 0u) |
                 (f->cfg.touch ? RFM_CAP_TOUCH : 0u);
        h.max_in_flight = RFM_FRAMES_IN_FLIGHT;
        h.session = f->session ? 1 : 0;
        strncpy(h.fw_ver, "m1-fake", 8);
        f->st.hellos++;
        reply(f, link, RFM_HELLO_R, body, rfm_build_hello_r(body, &h));
        break;
    }
    case RFM_SESSION: {
        int start;
        uint32_t ka;
        if (rfm_parse_session(b, n, &start, &ka))
            break;
        if (start) {
            f->session = 1 + link;
            f->touch = 0;                               /* (TOUCH: every session starts without it) */
            f->keepalive_ms = ka;
            memset(f->shadow, 0, sizeof f->shadow);
            show(f);
            memset(&f->leds, 0, sizeof f->leds);
            f->nacks = 0;
            f->frame_bytes = 0;
            f->frame_err = 0;
            f->reset_pending = 1;                       /* a new session: the first FRAME_ACK says so */
            f->st.sessions_started++;
            reply(f, link, RFM_ACK, NULL, 0);
            send_panel_r(f, link);
            send_master(f);
        } else {
            if (f->session)
                f->st.sessions_ended++;
            f->session = 0;
            f->held = 0;                                /* (the waiting screen; held tiles are never shown) */
            reply(f, link, RFM_ACK, NULL, 0);
        }
        break;
    }
    case RFM_LEDS:
        if (!f->session && !f->cfg.tiles_outside_session)
            break;
        if (!rfm_parse_leds(b, n, &f->leds))
            f->st.leds++;
        break;
    case RFM_PANEL_REQ:
        f->st.panel_req++;
        send_panel_r(f, link);
        break;
    case RFM_TOUCH:                                     /* (0.3.1) in a session on its link: PANEL_R replies */
        if (!f->cfg.touch || !f->session || f->session != 1 + link || n < RFM_TOUCH_LEN)
            break;
        f->touch = b[0] & (RFM_TOUCH_F_VEL | RFM_TOUCH_F_PRESS);
        f->touch_msgs++;
        send_panel_r(f, link);
        break;
    case RFM_TILES: {
        rfm_tiles_info_t info;
        int rc;
        uint32_t rate = link == RFM_LINK_BK ? f->cfg.bk_rate_bps : f->cfg.sx_rate_bps;
        uint64_t now_us = (uint64_t)f->now * 1000u;
        if (!f->session && !f->cfg.tiles_outside_session)
            break;
        f->st.tiles_msgs++;
        f->st.tile_bytes += n;
        if (f->busy_us[link] < now_us)
            f->busy_us[link] = now_us;
        if (rate)
            f->busy_us[link] += (uint64_t)n * 1000000u / rate;
        if (f->decode_err_next) {
            f->decode_err_next = 0;
            rc = RFM_REF_ENC;
            if (n >= 4u) {
                info.frame = (uint16_t)(b[0] | b[1] << 8);
                info.flags = b[2];
            } else {
                info.flags = 0;
            }
        } else {
            rc = rfm_tiles_apply(f->shadow, b, n, 0, &info);
        }
        f->frame_bytes += n;
        f->tiles_ms = f->now;
        if (rc != RFM_REF_OK) {
            f->frame_err = 1;
            f->st.decode_errs++;
        }
        if (rc != RFM_REF_SHORT_HDR && (info.flags & RFM_TILES_F_END)) {
            pend_ack_t *a;
            f->st.frames++;
            if (f->nacks < MAXACK) {
                a = &f->acks[f->nacks++];
                a->frame = info.frame;
                a->status = (uint8_t)((f->frame_err ? RFM_ACK_DECODE_ERR : 0u) |
                                      (f->reset_pending ? RFM_ACK_SHADOW_RESET : 0u));
                a->bytes = f->frame_bytes;
                a->due_us = f->busy_us[link];
                a->link = link;
            }
            f->reset_pending = 0;
            f->frame_bytes = 0;
            f->frame_err = 0;
            if (info.flags & RFM_TILES_F_HOLD)
                f->st.held_frames++;
            if (f->cfg.hold && f->session && (info.flags & RFM_TILES_F_HOLD)) {
                f->held = 1;
            } else {
                show(f);
            }
        }
        break;
    }
    case RFM_ECHO:
        if (n <= RFM_SX_BODY_MAX)
            reply(f, link, RFM_ECHO_R, b, n);
        break;
    default:
        break;
    }
}

rfm_fake_t *rfm_fake_new(const rfm_fake_cfg_t *cfg)
{
    rfm_fake_t *f = (rfm_fake_t *)calloc(1, sizeof *f);
    if (!f)
        return NULL;
    if (cfg)
        f->cfg = *cfg;
    else
        rfm_fake_cfg_default(&f->cfg);
    if (!f->cfg.name)
        f->cfg.name = RFM_NAME;
    if (!f->cfg.sx_body_max)
        f->cfg.sx_body_max = RFM_SX_BODY_MAX;
    if (!f->cfg.bk_body_max)
        f->cfg.bk_body_max = RFM_BK_BODY_MAX;
    f->rx[RFM_LINK_SX] = rfm_rx_new(RFM_LINK_SX, f->cfg.sx_body_max, on_msg, NULL, f);
    f->rx[RFM_LINK_BK] = rfm_rx_new(RFM_LINK_BK, f->cfg.bk_body_max, on_msg, NULL, f);
    f->out[0].b = (uint8_t *)malloc(OUTQ);
    f->out[1].b = (uint8_t *)malloc(OUTQ);
    if (!f->rx[0] || !f->rx[1] || !f->out[0].b || !f->out[1].b) {
        rfm_fake_free(f);
        return NULL;
    }
    f->master = 512;
    f->reset_pending = 1;                               /* a power-on */
    return f;
}

void rfm_fake_free(rfm_fake_t *f)
{
    if (!f)
        return;
    rfm_rx_free(f->rx[0]);
    rfm_rx_free(f->rx[1]);
    free(f->out[0].b);
    free(f->out[1].b);
    free(f);
}

void rfm_fake_rx_sx(rfm_fake_t *f, const uint8_t *p, uint32_t n, uint32_t now)
{
    f->now = now;
    f->cur_link = RFM_LINK_SX;
    rfm_rx_feed(f->rx[RFM_LINK_SX], p, n);
}

void rfm_fake_rx_bk(rfm_fake_t *f, const uint8_t *p, uint32_t n, uint32_t now)
{
    if (!f->cfg.has_bulk)
        return;
    f->now = now;
    f->cur_link = RFM_LINK_BK;
    rfm_rx_feed(f->rx[RFM_LINK_BK], p, n);
}

uint32_t rfm_fake_take_sx(rfm_fake_t *f, uint8_t *out, uint32_t cap) { return out_take(&f->out[0], out, cap); }
uint32_t rfm_fake_take_bk(rfm_fake_t *f, uint8_t *out, uint32_t cap) { return out_take(&f->out[1], out, cap); }

void rfm_fake_poll(rfm_fake_t *f, uint32_t now)
{
    uint32_t i = 0;
    f->now = now;
    while (i < f->nacks) {                              /* in order: an ACK waits for the ones before it */
        pend_ack_t *a = &f->acks[i];
        if (a->due_us > (uint64_t)now * 1000u + 999u)
            break;
        if (f->drop_acks) {
            f->drop_acks--;
            f->st.acks_dropped++;
        } else {
            rfm_frame_ack_t ack;
            uint8_t b[RFM_ACK_LEN];
            ack.frame = a->frame;
            ack.status = a->status;
            ack.draw_us = 900;
            ack.bytes = a->bytes;
            ack.pending = (uint8_t)(f->nacks - 1u);
            rfm_build_frame_ack(b, &ack);
            reply(f, a->link, RFM_FRAME_ACK, b, RFM_ACK_LEN);
            f->st.acks++;
        }
        i++;
    }
    if (i) {
        memmove(f->acks, f->acks + i, (f->nacks - i) * sizeof f->acks[0]);
        f->nacks -= i;
    }
    if (f->held && (int32_t)(now - f->tiles_ms) >= (int32_t)RFM_HOLD_MS) {
        show(f);                                        /* the host went quiet holding tiles */
        f->st.hold_timeouts++;
    }
    if (f->session) {
        if (f->keepalive_ms && (int32_t)(now - f->last_rx) >= (int32_t)f->keepalive_ms) {
            f->session = 0;                             /* back to the waiting screen */
            f->held = 0;
            f->st.keepalive_expired++;
            f->st.sessions_ended++;
        } else if ((int32_t)(now - f->last_panel_r) >= 500) {
            send_panel_r(f, f->session - 1);
        }
    }
}

/* ---------------------------------------------------------------- the panel ---- */
void rfm_fake_key_vel(rfm_fake_t *f, uint32_t k, int down, uint32_t vel)
{
    if (k >= RFM_NKEYS)
        return;
    f->keys = down ? f->keys | 1u << k : f->keys & ~(1u << k);
    if (down)
        midi3(f, 0x90 | RFM_MIDI_KEY_CH, (uint8_t)(RFM_MIDI_KEY_NOTE0 + k),
              (uint8_t)((f->touch & RFM_TOUCH_F_VEL) && vel >= 1u && vel <= 127u ? vel : RFM_MIDI_KEY_VEL));
    else
        midi3(f, 0x80 | RFM_MIDI_KEY_CH, (uint8_t)(RFM_MIDI_KEY_NOTE0 + k), 0);
}

void rfm_fake_press(rfm_fake_t *f, uint32_t k, uint32_t p)
{
    if (k < RFM_NKEYS && (f->touch & RFM_TOUCH_F_PRESS) && (f->keys >> k & 1u))
        midi3(f, RFM_MIDI_KEY_AT | RFM_MIDI_KEY_CH, (uint8_t)(RFM_MIDI_KEY_NOTE0 + k), (uint8_t)(p > 127u ? 127u : p));
}

uint32_t rfm_fake_touch(const rfm_fake_t *f) { return f->touch | f->touch_msgs << 8; }

void rfm_fake_key(rfm_fake_t *f, uint32_t k, int down)
{
    if (k >= RFM_NKEYS)
        return;
    f->keys = down ? f->keys | 1u << k : f->keys & ~(1u << k);
    if (down)
        midi3(f, 0x90 | RFM_MIDI_KEY_CH, (uint8_t)(RFM_MIDI_KEY_NOTE0 + k), RFM_MIDI_KEY_VEL);
    else
        midi3(f, 0x80 | RFM_MIDI_KEY_CH, (uint8_t)(RFM_MIDI_KEY_NOTE0 + k), 0);
}

void rfm_fake_button(rfm_fake_t *f, uint32_t b, int down)
{
    if (b >= RFM_NBTN)
        return;
    f->buttons = down ? f->buttons | 1u << b : f->buttons & ~(1u << b);
    if (down)
        midi3(f, 0x90 | RFM_MIDI_BTN_CH, (uint8_t)b, RFM_MIDI_BTN_VEL);
    else
        midi3(f, 0x80 | RFM_MIDI_BTN_CH, (uint8_t)b, 0);
}

void rfm_fake_enc(rfm_fake_t *f, uint32_t e, int32_t d)
{
    if (e >= RFM_NENC)
        return;
    f->enc[e] = (int16_t)(uint16_t)((uint16_t)f->enc[e] + (uint16_t)d);
    while (d) {                                         /* clamped to -63..+63 per message */
        int32_t c = d > 63 ? 63 : d < -63 ? -63 : d;
        midi3(f, 0xB0 | RFM_MIDI_BTN_CH, (uint8_t)(RFM_MIDI_ENC_CC0 + e), (uint8_t)(64 + c));
        d -= c;
    }
}

void rfm_fake_master(rfm_fake_t *f, uint32_t adc)
{
    f->master = (uint16_t)(adc > 1023u ? 1023u : adc);
    send_master(f);
}

/* ---------------------------------------------------------------- faults and state ---- */
void rfm_fake_reboot(rfm_fake_t *f)
{
    memset(f->shadow, 0, sizeof f->shadow);
    show(f);
    memset(&f->leds, 0, sizeof f->leds);
    if (f->session)
        f->st.sessions_ended++;
    f->session = 0;
    f->nacks = 0;
    f->frame_bytes = 0;
    f->frame_err = 0;
    f->reset_pending = 1;
    memset(f->enc, 0, sizeof f->enc);
    rfm_rx_reset(f->rx[0]);
    rfm_rx_reset(f->rx[1]);
}

void rfm_fake_deaf(rfm_fake_t *f, int on) { f->deaf = on; }
void rfm_fake_drop_acks(rfm_fake_t *f, uint32_t n) { f->drop_acks = n; }
void rfm_fake_drop_midi(rfm_fake_t *f, uint32_t n) { f->drop_midi = n; }
void rfm_fake_decode_error(rfm_fake_t *f) { f->decode_err_next = 1; }

void rfm_fake_trs(rfm_fake_t *f, const uint8_t *msgs, uint32_t n)
{
    uint8_t b[RFM_MIDI_IN_MAX];
    uint32_t k = 0, len = 0;
    if (!f->session || !f->cfg.midi_in)
        return;
    while (k < n) {
        uint32_t m = rfm_midi_msg_len(msgs[k]);
        if (!m || k + m > n)
            break;
        if (len + m > RFM_MIDI_IN_MAX) {
            reply(f, f->session - 1, RFM_MIDI_IN, b, len);
            len = 0;
        }
        memcpy(b + len, msgs + k, m);
        len += m;
        k += m;
    }
    if (len)
        reply(f, f->session - 1, RFM_MIDI_IN, b, len);
}
int rfm_fake_session(const rfm_fake_t *f) { return f->session; }
const uint8_t *rfm_fake_shadow(const rfm_fake_t *f) { return f->shadow; }
const uint8_t *rfm_fake_panel(const rfm_fake_t *f) { return f->panel; }
int rfm_fake_holding(const rfm_fake_t *f) { return f->held; }
const rfm_leds_t *rfm_fake_leds(const rfm_fake_t *f) { return &f->leds; }
void rfm_fake_stats(const rfm_fake_t *f, rfm_fake_stats_t *st) { *st = f->st; }
