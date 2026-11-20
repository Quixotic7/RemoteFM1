/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The protocol v2 codec: framing on firmware/src/remote_proto.h's helpers, every message body, the link parsers. */
#include "rfm/codec.h"

#include <stdlib.h>
#include <string.h>

#include "remote_proto.h"

/* ------------------------------------------------------------------ framing out ---- */
uint32_t rfm_sx_size(uint32_t n) { return rf_packed_len(n) + RFM_SX_OVERHEAD; }

uint32_t rfm_sx_frame(uint8_t cmd, const uint8_t *body, uint32_t n, uint8_t *out)
{
    return rf_sx_encode(cmd, body, n, out);
}

uint32_t rfm_bk_frame(uint8_t cmd, const uint8_t *body, uint32_t n, uint32_t seq, uint8_t *out)
{
    rf_bk_header(out, cmd, n, seq);
    if (n)
        memcpy(out + RF_BK_HDR, body, n);
    return RF_BK_HDR + n;
}

/* ------------------------------------------------------------------ bodies ---- */
static void copy_str(char *dst, const uint8_t *src, uint32_t n)     /* a fixed field, NUL padding dropped */
{
    uint32_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = (char)src[i];
    while (i > 0 && dst[i - 1] == ' ')
        i--;
    dst[i] = 0;
}
static void put_str(uint8_t *dst, const char *s, uint32_t n)
{
    uint32_t i;
    memset(dst, 0, n);
    for (i = 0; s && s[i] && i < n; i++)
        dst[i] = (uint8_t)s[i];
}

uint32_t rfm_build_hello(uint8_t *body, const char *host_name)
{
    uint32_t n = 0;
    while (host_name && host_name[n] && n < 16u) {
        body[n] = (uint8_t)host_name[n];
        n++;
    }
    return n;
}

int rfm_parse_hello(const uint8_t *b, uint32_t n, rfm_hello_t *h)
{
    memset(h, 0, sizeof *h);
    if (n < RF_HELLO_LEN)
        return -1;
    copy_str(h->name, b, 16);
    h->proto = b[16];
    copy_str(h->ver, b + 17, 8);
    h->transports = b[25];
    h->usb_mode = b[26];
    h->lcd_baud = b[27];
    h->loader_state = (int8_t)b[28];
    h->sxq = (uint16_t)rf_rd16(b + 29);
    h->bk_rx_ring = rf_rd32(b + 31);
    h->bk_tx_ring = rf_rd32(b + 35);
    h->sx_body_max = (uint16_t)rf_rd16(b + 39);
    h->bk_body_max = (uint16_t)rf_rd16(b + 41);
    copy_str(h->identity, b + 43, 8);
    if (n >= RFM_HELLO_LEN) {
        h->v2 = 1;
        h->screen_w = (uint16_t)rf_rd16(b + RFM_HELLO_SCREEN_W);
        h->screen_h = (uint16_t)rf_rd16(b + RFM_HELLO_SCREEN_H);
        h->caps = (uint16_t)rf_rd16(b + RFM_HELLO_CAPS);
        h->max_in_flight = b[RFM_HELLO_INFLIGHT];
        h->session = b[RFM_HELLO_SESSION];
        copy_str(h->fw_ver, b + RFM_HELLO_FWVER, 8);
    }
    return 0;
}

uint32_t rfm_build_hello_r(uint8_t *b, const rfm_hello_t *h)
{
    memset(b, 0, RFM_HELLO_LEN);
    put_str(b, h->name, 16);
    b[16] = h->proto;
    put_str(b + 17, h->ver, 8);
    b[25] = h->transports;
    b[26] = h->usb_mode;
    b[27] = h->lcd_baud;
    b[28] = (uint8_t)h->loader_state;
    rf_wr16(b + 29, h->sxq);
    rf_wr32(b + 31, h->bk_rx_ring);
    rf_wr32(b + 35, h->bk_tx_ring);
    rf_wr16(b + 39, h->sx_body_max);
    rf_wr16(b + 41, h->bk_body_max);
    put_str(b + 43, h->identity, 8);
    rf_wr16(b + RFM_HELLO_SCREEN_W, h->screen_w);
    rf_wr16(b + RFM_HELLO_SCREEN_H, h->screen_h);
    rf_wr16(b + RFM_HELLO_CAPS, h->caps);
    b[RFM_HELLO_INFLIGHT] = h->max_in_flight;
    b[RFM_HELLO_SESSION] = h->session;
    put_str(b + RFM_HELLO_FWVER, h->fw_ver, 8);
    return RFM_HELLO_LEN;
}

int rfm_hello_is_remotefm1(const rfm_hello_t *h)
{
    return h->v2 && h->proto == RFM_PROTO && !strcmp(h->name, RFM_NAME);
}

int rfm_hello_is_ble(const rfm_hello_t *h)
{
    return (h->transports & RFM_TR_BLE) && h->sxq && h->sxq <= RFM_HELLO_SXQ_BLE_MAX;
}

uint32_t rfm_build_session(uint8_t *b, int start, uint32_t keepalive_ms)
{
    b[0] = start ? 1u : 0u;
    rf_wr16(b + 1, keepalive_ms > 0xFFFFu ? 0xFFFFu : keepalive_ms);
    return RFM_SESSION_LEN;
}

uint32_t rfm_build_touch(uint8_t *b, uint32_t flags)
{
    b[0] = (uint8_t)(flags & (RFM_TOUCH_F_VEL | RFM_TOUCH_F_PRESS));
    return RFM_TOUCH_LEN;
}

uint32_t rfm_build_audio_mode(uint8_t *b, int on, uint32_t flags, uint32_t div, uint32_t buffer_ms)
{
    b[0] = on ? 1u : 0u;
    b[1] = (uint8_t)flags;
    b[2] = (uint8_t)div;
    rf_wr16(b + 3, buffer_ms > 0xFFFFu ? 0xFFFFu : buffer_ms);
    return RFM_AUDIO_MODE_LEN;
}

int rfm_parse_audio_mode(const uint8_t *b, uint32_t n, rfm_audio_r_t *m)
{
    if (n < RFM_AUDIO_MODE_LEN)
        return -1;
    memset(m, 0, sizeof *m);
    m->on = b[0] != 0;
    m->flags = b[1];
    m->div = b[2];
    m->buffer_ms = (uint16_t)rf_rd16(b + 3);
    return 0;
}

uint32_t rfm_build_audio_r(uint8_t *b, const rfm_audio_r_t *r)
{
    b[0] = r->on;
    b[1] = r->flags;
    b[2] = r->div;
    rf_wr16(b + 3, r->buffer_ms);
    rf_wr16(b + 5, r->fill_ms);
    rf_wr32(b + 7, r->blocks);
    rf_wr16(b + 11, r->lost);
    rf_wr16(b + 13, r->underruns);
    rf_wr16(b + 15, r->skipped);
    return RFM_AUDIO_R_LEN;
}

int rfm_parse_audio_r(const uint8_t *b, uint32_t n, rfm_audio_r_t *r)
{
    if (n < RFM_AUDIO_R_LEN)
        return -1;
    r->on = b[0] != 0;
    r->flags = b[1];
    r->div = b[2];
    r->buffer_ms = (uint16_t)rf_rd16(b + 3);
    r->fill_ms = (uint16_t)rf_rd16(b + 5);
    r->blocks = rf_rd32(b + 7);
    r->lost = (uint16_t)rf_rd16(b + 11);
    r->underruns = (uint16_t)rf_rd16(b + 13);
    r->skipped = (uint16_t)rf_rd16(b + 15);
    return 0;
}

int rfm_parse_session(const uint8_t *b, uint32_t n, int *start, uint32_t *keepalive_ms)
{
    if (n < RFM_SESSION_LEN)
        return -1;
    *start = b[0] != 0;
    *keepalive_ms = rf_rd16(b + 1);
    return 0;
}

uint32_t rfm_build_leds(uint8_t *b, const rfm_leds_t *l)
{
    memcpy(b + RFM_LEDS_LIT, l->lit, RFM_NCOL);
    memcpy(b + RFM_LEDS_MID, l->mid, RFM_NCOL);
    memcpy(b + RFM_LEDS_DIM, l->dim, RFM_NCOL);
    memcpy(b + RFM_LEDS_FAINT, l->faint, RFM_NCOL);
    b[RFM_LEDS_FLAGS] = l->flags;
    return RFM_LEDS_LEN;
}

int rfm_parse_leds(const uint8_t *b, uint32_t n, rfm_leds_t *l)
{
    if (n < RFM_LEDS_LEN)
        return -1;
    memcpy(l->lit, b + RFM_LEDS_LIT, RFM_NCOL);
    memcpy(l->mid, b + RFM_LEDS_MID, RFM_NCOL);
    memcpy(l->dim, b + RFM_LEDS_DIM, RFM_NCOL);
    memcpy(l->faint, b + RFM_LEDS_FAINT, RFM_NCOL);
    l->flags = b[RFM_LEDS_FLAGS];
    return 0;
}

int rfm_leds_equal(const rfm_leds_t *a, const rfm_leds_t *b)
{
    return !memcmp(a->lit, b->lit, RFM_NCOL) && !memcmp(a->mid, b->mid, RFM_NCOL) &&
           !memcmp(a->dim, b->dim, RFM_NCOL) && !memcmp(a->faint, b->faint, RFM_NCOL) && a->flags == b->flags;
}

uint32_t rfm_build_frame_ack(uint8_t *b, const rfm_frame_ack_t *a)
{
    rf_wr16(b, a->frame);
    b[2] = a->status;
    rf_wr32(b + 3, a->draw_us);
    rf_wr32(b + 7, a->bytes);
    b[11] = a->pending;
    return RFM_ACK_LEN;
}

int rfm_parse_frame_ack(const uint8_t *b, uint32_t n, rfm_frame_ack_t *a)
{
    if (n < RFM_ACK_LEN)
        return -1;
    a->frame = (uint16_t)rf_rd16(b);
    a->status = b[2];
    a->draw_us = rf_rd32(b + 3);
    a->bytes = rf_rd32(b + 7);
    a->pending = b[11];
    return 0;
}

uint32_t rfm_build_panel_r(uint8_t *b, const rfm_panel_r_t *p)
{
    uint32_t e;
    rf_wr32(b + RFM_PANEL_KEYS, p->keys);
    rf_wr16(b + RFM_PANEL_BUTTONS, p->buttons);
    for (e = 0; e < RFM_NENC; e++)
        rf_wr16(b + RFM_PANEL_ENC + 2u * e, (uint16_t)p->enc[e]);
    rf_wr16(b + RFM_PANEL_MASTER, p->master);
    rf_wr16(b + RFM_PANEL_BATTERY, p->battery_mv);
    b[RFM_PANEL_FLAGS] = p->flags;
    return RFM_PANEL_LEN;
}

uint32_t rfm_midi_msg_len(uint8_t st)
{
    if (st >= 0xF8u)
        return 1;                                       /* real time */
    if (st < 0x80u || st >= 0xF0u)
        return 0;                                       /* data, SysEx, system common: never in MIDI_IN */
    return (st & 0xE0u) == 0xC0u ? 2u : 3u;             /* Cx Dx: 2, the other channel messages 3 */
}

int rfm_parse_panel_ev(const uint8_t *b, uint32_t n)
{
    uint32_t k = 0;
    if (!b || !n || n > RFM_PANEL_EV_MAX)
        return -1;
    while (k < n) {
        uint32_t m = b[k] >= 0xF0u ? 0u : rfm_midi_msg_len(b[k]), i;
        if (!m || k + m > n)
            return -1;
        for (i = 1; i < m; i++)
            if (b[k + i] & 0x80u)
                return -1;
        k += m;
    }
    return 0;
}

static void disc_copy(char *dst, uint32_t cap, const uint8_t *v, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && i + 1u < cap; i++)
        dst[i] = (char)(v[i] >= 0x20u && v[i] < 0x7Fu ? v[i] : '?');
    dst[i] = 0;
}

int rfm_parse_discovery(const uint8_t *b, uint32_t n, rfm_net_info_t *o)
{
    uint32_t k = 6;
    memset(o, 0, sizeof *o);
    o->port = RFM_NET_TCP_PORT;
    if (!b || n < 6 || memcmp(b, "RFM1!\n", 6))
        return -1;
    while (k < n) {
        uint32_t e = k, eq = 0, kl, vl;
        const uint8_t *v;
        while (e < n && b[e] != '\n')
            e++;
        while (k + eq < e && b[k + eq] != '=')
            eq++;
        if (k + eq < e) {
            kl = eq;
            v = b + k + eq + 1u;
            vl = e - (k + eq + 1u);
            if (vl && v[vl - 1u] == '\r')
                vl--;
#define DK(s) (kl == sizeof s - 1u && !memcmp(b + k, s, kl))
            if (DK("name"))
                disc_copy(o->name, sizeof o->name, v, vl);
            else if (DK("id"))
                disc_copy(o->id, sizeof o->id, v, vl);
            else if (DK("ver"))
                disc_copy(o->ver, sizeof o->ver, v, vl);
            else if (DK("ip"))
                disc_copy(o->ip, sizeof o->ip, v, vl);
            else if (DK("port") || DK("busy") || DK("session")) {
                uint32_t x = 0, i;
                for (i = 0; i < vl && i < 6u && v[i] >= '0' && v[i] <= '9'; i++)
                    x = x * 10u + (uint32_t)(v[i] - '0');
                if (DK("port"))
                    o->port = x && x < 65536u ? (uint16_t)x : (uint16_t)RFM_NET_TCP_PORT;
                else if (DK("busy"))
                    o->busy = x ? 1 : 0;
                else
                    o->session = x ? 1 : 0;
            }
#undef DK
        }
        k = e + 1u;
    }
    return 0;
}

int rfm_parse_midi_in(const uint8_t *b, uint32_t n)
{
    uint32_t k = 0;
    if (!b || !n || n > RFM_MIDI_IN_MAX)
        return -1;
    while (k < n) {
        uint32_t m = rfm_midi_msg_len(b[k]), i;
        if (!m || k + m > n)
            return -1;
        for (i = 1; i < m; i++)
            if (b[k + i] & 0x80u)
                return -1;
        k += m;
    }
    return 0;
}

uint32_t rfm_build_midi_in(uint8_t *body, const uint8_t *msgs, uint32_t n)
{
    memcpy(body, msgs, n);
    return n;
}

int rfm_parse_panel_r(const uint8_t *b, uint32_t n, rfm_panel_r_t *p)
{
    uint32_t e;
    if (n < RFM_PANEL_LEN)
        return -1;
    p->keys = rf_rd32(b + RFM_PANEL_KEYS);
    p->buttons = (uint16_t)rf_rd16(b + RFM_PANEL_BUTTONS);
    for (e = 0; e < RFM_NENC; e++)
        p->enc[e] = (int16_t)(uint16_t)rf_rd16(b + RFM_PANEL_ENC + 2u * e);
    p->master = (uint16_t)rf_rd16(b + RFM_PANEL_MASTER);
    p->battery_mv = (uint16_t)rf_rd16(b + RFM_PANEL_BATTERY);
    p->flags = b[RFM_PANEL_FLAGS];
    return 0;
}

void rfm_tiles_header(uint8_t *b, uint16_t frame, uint8_t flags, uint8_t nrects)
{
    rf_wr16(b, frame);
    b[2] = flags;
    b[3] = nrects;
}

/* ------------------------------------------------------------------ receive ---- */
struct rfm_rx {
    int kind;
    rf_tstats_t st;
    rf_sx_t sx;
    rf_bk_t bk;
    uint8_t *buf;
    rfm_msg_fn on_msg;
    rfm_midi_fn on_midi;
    void *ctx;
    uint32_t msgs, midi;
    uint8_t in_sx, run, mn, need;
    uint8_t m[3];
};

static void rx_body(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    rfm_rx_t *r = (rfm_rx_t *)ctx;
    r->msgs++;
    if (r->on_msg)
        r->on_msg(r->ctx, (uint8_t)cmd, body, len);
}

rfm_rx_t *rfm_rx_new(int kind, uint32_t body_cap, rfm_msg_fn on_msg, rfm_midi_fn on_midi, void *ctx)
{
    rfm_rx_t *r = (rfm_rx_t *)calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->buf = (uint8_t *)malloc(body_cap ? body_cap : 1u);
    if (!r->buf) {
        free(r);
        return NULL;
    }
    r->kind = kind;
    r->on_msg = on_msg;
    r->on_midi = on_midi;
    r->ctx = ctx;
    rf_body_init(&r->sx.b, &r->st, r->buf, body_cap, rx_body, r);
    rf_body_init(&r->bk.b, &r->st, r->buf, body_cap, rx_body, r);
    return r;
}

void rfm_rx_free(rfm_rx_t *r)
{
    if (!r)
        return;
    free(r->buf);
    free(r);
}

void rfm_rx_reset(rfm_rx_t *r)
{
    r->sx.st = 0;
    r->bk.hn = 0;
    r->bk.in_body = 0;
    r->bk.have_fseq = 0;
    r->in_sx = 0;
    r->run = 0;
    r->mn = 0;
}

static uint8_t midi_need(uint8_t status)              /* data bytes after a status */
{
    switch (status & 0xF0u) {
    case 0xC0:
    case 0xD0:
        return 1;
    case 0xF0:
        return status == 0xF1u || status == 0xF3u ? 1 : status == 0xF2u ? 2 : 0;
    default:
        return 2;
    }
}

static void midi_byte(rfm_rx_t *r, uint8_t c)
{
    if (c >= 0xF8u)                                    /* realtime: anywhere, ignored */
        return;
    if (c == 0xF0u) {                                  /* (rf_sx_byte counts one of ours cut off by it) */
        r->in_sx = 1;
        r->run = 0;
        r->mn = 0;
        rf_sx_byte(&r->sx, c);
        return;
    }
    if (c == 0xF7u) {
        if (r->in_sx)
            rf_sx_byte(&r->sx, c);
        r->in_sx = 0;
        return;
    }
    if (c & 0x80u) {
        if (r->in_sx) {
            rf_sx_byte(&r->sx, c);                      /* a status ends a SysEx (counted as cut off if ours) */
            r->in_sx = 0;
        }
        r->m[0] = c;
        r->mn = 1;
        r->need = midi_need(c);
        r->run = c < 0xF0u ? c : 0;
        if (!r->need) {
            r->mn = 0;
            if (c < 0xF0u && r->on_midi) {
                r->midi++;
                r->on_midi(r->ctx, r->m, 1);
            }
        }
        return;
    }
    if (r->in_sx) {
        rf_sx_byte(&r->sx, c);
        return;
    }
    if (!r->mn) {                                      /* running status */
        if (!r->run)
            return;
        r->m[0] = r->run;
        r->need = midi_need(r->run);
        r->mn = 1;
    }
    r->m[r->mn++] = c;
    if (r->mn == 1u + r->need) {
        r->mn = 0;
        if (r->m[0] < 0xF0u && r->on_midi) {          /* system common messages are not ours */
            r->midi++;
            r->on_midi(r->ctx, r->m, 1u + r->need);
        }
    }
}

void rfm_rx_feed(rfm_rx_t *r, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (r->kind != RFM_LINK_SX) {                       /* bulk, or the bulk framing over TCP */
        rf_bk_feed(&r->bk, p, n);
        return;
    }
    for (i = 0; i < n; i++)
        midi_byte(r, p[i]);
}

void rfm_rx_stats(const rfm_rx_t *r, rfm_rx_stats_t *st)
{
    st->msgs = r->msgs;
    st->bad = r->st.bad;
    st->sync = r->st.sync;
    st->frame_gaps = r->st.frame_gaps;
    st->midi = r->midi;
}

/* ---------------------------------------------------------------- WIFI (the Wi-Fi build) ---- */
uint32_t rfm_build_wifi_get(uint8_t *body)
{
    body[0] = RFM_WIFI_GET;
    return 1;
}

uint32_t rfm_build_wifi_scan(uint8_t *body, int force)
{
    body[0] = RFM_WIFI_SCAN;
    body[1] = force ? 1u : 0u;
    return 2;
}

uint32_t rfm_build_wifi_list(uint8_t *body, uint32_t which, uint32_t first)
{
    body[0] = RFM_WIFI_LIST;
    body[1] = (uint8_t)which;
    body[2] = (uint8_t)first;
    return 3;
}

uint32_t rfm_build_wifi_set(uint8_t *body, const char *ssid, const char *pass)
{
    const uint32_t sl = ssid ? (uint32_t)strlen(ssid) : 0u, pl = pass ? (uint32_t)strlen(pass) : 0u;
    if (!sl || sl > RFM_WIFI_SSID_MAX || pl > RFM_WIFI_PASS_MAX)
        return 0;
    body[0] = RFM_WIFI_SET;
    body[1] = (uint8_t)sl;
    memcpy(body + 2, ssid, sl);
    body[2u + sl] = (uint8_t)pl;
    if (pl)
        memcpy(body + 3u + sl, pass, pl);
    return 3u + sl + pl;
}

uint32_t rfm_build_wifi_forget(uint8_t *body, uint32_t k)
{
    body[0] = RFM_WIFI_FORGET;
    body[1] = (uint8_t)k;
    return 2;
}

static int wifi_str(const uint8_t *b, uint32_t n, uint32_t *k, char *out, uint32_t cap)   /* len8 bytes */
{
    uint32_t l;
    if (*k >= n)
        return -1;
    l = b[*k];
    if (*k + 1u + l > n)
        return -1;
    if (l >= cap)
        return -1;
    memcpy(out, b + *k + 1u, l);
    out[l] = 0;
    *k += 1u + l;
    return 0;
}

int rfm_parse_wifi_state(const uint8_t *b, uint32_t n, rfm_wifi_state_t *o)
{
    uint32_t k = RFM_WIFI_R_SSID, i;
    memset(o, 0, sizeof *o);
    if (!b || n < RFM_WIFI_R_SSID + 2u || b[0] > RFM_WIFI_MODE)
        return -1;
    o->op = b[0];
    o->status = b[1];
    o->state = b[2];
    o->rssi = (int8_t)b[3];
    memcpy(o->ip, b + 4, 4);
    o->tcp_port = (uint16_t)(b[8] | b[9] << 8);
    o->udp_port = (uint16_t)(b[10] | b[11] << 8);
    o->host = b[12];
    o->saved = b[13];
    if (wifi_str(b, n, &k, o->ssid, sizeof o->ssid) || wifi_str(b, n, &k, o->why, sizeof o->why))
        return -1;
    if (o->op == RFM_WIFI_SAVED && k < n) {
        const uint32_t m = b[k++];
        for (i = 0; i < m; i++) {
            char s[33];
            if (wifi_str(b, n, &k, s, sizeof s))
                return -1;
            if (o->nkept < 6u)
                memcpy(o->kept[o->nkept++], s, sizeof s);
        }
    }
    return 0;
}

int rfm_parse_wifi_list(const uint8_t *b, uint32_t n, rfm_wifi_list_t *o)
{
    uint32_t k = RFM_WIFI_LIST_HDR, i;
    memset(o, 0, sizeof *o);
    if (!b || n < RFM_WIFI_LIST_HDR || (b[0] != RFM_WIFI_SCAN && b[0] != RFM_WIFI_LIST))
        return -1;
    o->op = b[0];
    o->status = b[1];
    o->state = b[2];
    o->which = b[3];
    o->scanning = b[4];
    o->scans = b[5];
    o->total = b[6];
    o->first = b[7];
    for (i = 0; i < b[8]; i++) {
        rfm_wifi_net_t e;
        if (k + 4u > n)
            return -1;
        e.rssi = (int8_t)b[k];
        e.flags = b[k + 1u];
        e.channel = b[k + 2u];
        k += 3u;
        if (wifi_str(b, n, &k, e.ssid, sizeof e.ssid))
            return -1;
        if (o->count < 16u)
            o->net[o->count++] = e;
    }
    return 0;
}
