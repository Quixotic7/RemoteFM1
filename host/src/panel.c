/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The panel decoder: plain MIDI panel events -> state + events; PANEL_R reconciles. See rfm/panel.h. */
#include "rfm/panel.h"

#include <string.h>

static void ev(rfm_panel_t *p, uint8_t type, uint8_t index, int16_t value, uint8_t source)
{
    rfm_event_t e;
    e.type = type;
    e.index = index;
    e.value = value;
    e.source = source;
    p->events++;
    if (source == RFM_EVS_RESYNC)
        p->resyncs++;
    if (p->fn)
        p->fn(p->ctx, &e);
}

void rfm_panel_init(rfm_panel_t *p, uint32_t settle_ms, rfm_event_fn fn, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->settle_ms = settle_ms;
    p->fn = fn;
    p->ctx = ctx;
}

void rfm_panel_reset(rfm_panel_t *p)
{
    p->have_panel = 0;
    p->have_msb = 0;
}

/* the last event's time; 0 = never (a stamp is never 0) */
static uint32_t stamp(uint32_t now) { return now ? now : 1u; }
static int settled(const rfm_panel_t *p, uint32_t t, uint32_t now)
{
    return !t || (int32_t)(now - t) >= (int32_t)p->settle_ms;
}

int rfm_panel_midi(rfm_panel_t *p, const uint8_t *m, uint32_t n, uint32_t now)
{
    uint32_t st, ch, a, v;
    if (n < 3u)
        return 0;
    st = m[0] & 0xF0u;
    ch = m[0] & 0x0Fu;
    a = m[1];
    v = m[2];
    if ((st == 0x90u || st == 0x80u) && ch == RFM_MIDI_KEY_CH && a >= RFM_MIDI_KEY_NOTE0 &&
        a < RFM_MIDI_KEY_NOTE0 + RFM_NKEYS) {
        uint32_t k = a - RFM_MIDI_KEY_NOTE0, down = st == 0x90u && v != 0u;
        p->t_key[k] = stamp(now);
        if (((p->keys >> k) & 1u) != down) {
            p->keys ^= 1u << k;
            ev(p, RFM_EV_KEY, (uint8_t)k, (int16_t)down, RFM_EVS_MIDI);
        }
        return 1;
    }
    if ((st == 0x90u || st == 0x80u) && ch == RFM_MIDI_BTN_CH && a < RFM_NBTN) {
        uint32_t down = st == 0x90u && v != 0u;
        p->t_btn[a] = stamp(now);
        if (((p->buttons >> a) & 1u) != down) {
            p->buttons ^= 1u << a;
            ev(p, RFM_EV_BUTTON, (uint8_t)a, (int16_t)down, RFM_EVS_MIDI);
        }
        return 1;
    }
    if (st == 0xB0u && ch == RFM_MIDI_BTN_CH) {
        if (a >= RFM_MIDI_ENC_CC0 && a < RFM_MIDI_ENC_CC0 + RFM_NENC) {
            uint32_t e = a - RFM_MIDI_ENC_CC0;
            int32_t d = (int32_t)v - 64;
            p->t_enc[e] = stamp(now);
            if (!d) {
                p->ignored++;
                return 1;
            }
            p->enc_count[e] = (int16_t)(uint16_t)((uint16_t)p->enc_count[e] + (uint16_t)d);
            ev(p, RFM_EV_ENC, (uint8_t)e, (int16_t)d, RFM_EVS_MIDI);
            return 1;
        }
        if (a == RFM_MIDI_MASTER_MSB) {
            p->master_msb = (uint8_t)v;
            p->have_msb = 1;
            p->t_master = stamp(now);
            return 1;
        }
        if (a == RFM_MIDI_MASTER_LSB) {
            uint32_t adc;
            p->t_master = stamp(now);
            if (!p->have_msb) {                         /* an LSB without its MSB: nothing to pair it with */
                p->ignored++;
                return 1;
            }
            p->have_msb = 0;
            adc = (((uint32_t)p->master_msb << 7) | v) >> 4;
            if (!p->have_master || adc != p->master) {
                p->master = (uint16_t)adc;
                p->have_master = 1;
                ev(p, RFM_EV_MASTER, 0, (int16_t)adc, RFM_EVS_MIDI);
            }
            return 1;
        }
    }
    return 0;
}

void rfm_panel_reconcile(rfm_panel_t *p, const rfm_panel_r_t *r, uint32_t now)
{
    uint32_t k, first = !p->have_panel;
    uint32_t keys = r->keys & ((1u << RFM_NKEYS) - 1u), btns = r->buttons & ((1u << RFM_NBTN) - 1u);
    p->battery_mv = r->battery_mv;
    for (k = 0; k < RFM_NKEYS; k++) {
        uint32_t want = (keys >> k) & 1u;
        if (((p->keys >> k) & 1u) != want && settled(p, p->t_key[k], now)) {
            p->keys ^= 1u << k;
            ev(p, RFM_EV_KEY, (uint8_t)k, (int16_t)want, RFM_EVS_RESYNC);
        }
    }
    for (k = 0; k < RFM_NBTN; k++) {
        uint32_t want = (btns >> k) & 1u;
        if (((p->buttons >> k) & 1u) != want && settled(p, p->t_btn[k], now)) {
            p->buttons ^= 1u << k;
            ev(p, RFM_EV_BUTTON, (uint8_t)k, (int16_t)want, RFM_EVS_RESYNC);
        }
    }
    for (k = 0; k < RFM_NENC; k++) {
        int16_t d = (int16_t)(uint16_t)((uint16_t)r->enc[k] - (uint16_t)p->enc_count[k]);
        if (first) {
            p->enc_count[k] = r->enc[k];
        } else if (d && settled(p, p->t_enc[k], now)) {
            p->enc_count[k] = r->enc[k];
            ev(p, RFM_EV_ENC, (uint8_t)k, d, RFM_EVS_RESYNC);
        }
    }
    {
        uint32_t m = r->master > 1023u ? 1023u : r->master;
        /* PANEL_R wins (docs/PROTOCOL.md section 4), a 1-count move too: the unit's MIDI skips those (dead band
         * of 2), so only PANEL_R can tell the host about them */
        if (m != p->master && settled(p, p->t_master, now)) {
            p->master = (uint16_t)m;
            ev(p, RFM_EV_MASTER, 0, (int16_t)m, RFM_EVS_RESYNC);
        }
        if (!p->have_master)
            p->have_master = 1;
    }
    p->have_panel = 1;
}
