/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1's fake unit (host/src/fake.c) for the browser tests: a handle API over rfm_fake_t. Test-only: the
 * site never ships it. */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "rfm/fake.h"

static uint8_t *inbuf, outbuf[1u << 19];
static uint32_t incap;
static uint32_t out32[32];

/* name NULL / "": "RemoteFM1"; proto 0: 2; rates in bytes/s (0: instant); ble: rfm_fake_cfg_t's (1: on Bluetooth) */
EMSCRIPTEN_KEEPALIVE rfm_fake_t *fk_new(int has_bulk, int silent, const char *name, int proto, int sx_rate,
                                         int bk_rate, int ble)
{
    rfm_fake_cfg_t c;
    rfm_fake_cfg_default(&c);
    c.has_bulk = has_bulk;
    c.silent = silent;
    if (name && *name) {
        char *n = (char *)malloc(strlen(name) + 1u);   /* kept for the fake's life (cfg holds the pointer) */
        strcpy(n, name);
        c.name = n;
    }
    if (proto > 0)
        c.proto = (uint8_t)proto;
    c.sx_rate_bps = (uint32_t)sx_rate;
    c.bk_rate_bps = (uint32_t)bk_rate;
    c.ble = ble;
    return rfm_fake_new(&c);
}
EMSCRIPTEN_KEEPALIVE void fk_free(rfm_fake_t *f) { rfm_fake_free(f); }

EMSCRIPTEN_KEEPALIVE uint8_t *fk_inbuf(uint32_t n)
{
    if (n > incap) {
        uint8_t *b = (uint8_t *)realloc(inbuf, n);
        if (!b)
            return NULL;
        inbuf = b;
        incap = n;
    }
    return inbuf;
}
EMSCRIPTEN_KEEPALIVE void fk_rx_sx(rfm_fake_t *f, uint32_t n, uint32_t now) { rfm_fake_rx_sx(f, inbuf, n, now); }
EMSCRIPTEN_KEEPALIVE void fk_rx_bk(rfm_fake_t *f, uint32_t n, uint32_t now) { rfm_fake_rx_bk(f, inbuf, n, now); }
EMSCRIPTEN_KEEPALIVE uint8_t *fk_outbuf(void) { return outbuf; }
EMSCRIPTEN_KEEPALIVE uint32_t fk_take_sx(rfm_fake_t *f) { return rfm_fake_take_sx(f, outbuf, sizeof outbuf); }
EMSCRIPTEN_KEEPALIVE uint32_t fk_take_bk(rfm_fake_t *f) { return rfm_fake_take_bk(f, outbuf, sizeof outbuf); }
EMSCRIPTEN_KEEPALIVE void fk_poll(rfm_fake_t *f, uint32_t now) { rfm_fake_poll(f, now); }

EMSCRIPTEN_KEEPALIVE void fk_key(rfm_fake_t *f, uint32_t k, int down) { rfm_fake_key(f, k, down); }
EMSCRIPTEN_KEEPALIVE void fk_button(rfm_fake_t *f, uint32_t b, int down) { rfm_fake_button(f, b, down); }
EMSCRIPTEN_KEEPALIVE void fk_enc(rfm_fake_t *f, uint32_t e, int d) { rfm_fake_enc(f, e, d); }
EMSCRIPTEN_KEEPALIVE void fk_master(rfm_fake_t *f, uint32_t adc) { rfm_fake_master(f, adc); }
EMSCRIPTEN_KEEPALIVE void fk_reboot(rfm_fake_t *f) { rfm_fake_reboot(f); }
EMSCRIPTEN_KEEPALIVE void fk_deaf(rfm_fake_t *f, int on) { rfm_fake_deaf(f, on); }
EMSCRIPTEN_KEEPALIVE void fk_drop_acks(rfm_fake_t *f, uint32_t n) { rfm_fake_drop_acks(f, n); }
EMSCRIPTEN_KEEPALIVE void fk_drop_midi(rfm_fake_t *f, uint32_t n) { rfm_fake_drop_midi(f, n); }

EMSCRIPTEN_KEEPALIVE int fk_session(rfm_fake_t *f) { return rfm_fake_session(f); }
EMSCRIPTEN_KEEPALIVE const uint8_t *fk_shadow(rfm_fake_t *f) { return rfm_fake_shadow(f); }   /* 240*240*2, BE */
EMSCRIPTEN_KEEPALIVE const uint8_t *fk_leds(rfm_fake_t *f) { return (const uint8_t *)rfm_fake_leds(f); }  /* 45 */
EMSCRIPTEN_KEEPALIVE uint32_t *fk_stats(rfm_fake_t *f)
{
    rfm_fake_stats_t st;
    rfm_fake_stats(f, &st);
    memset(out32, 0, sizeof out32);
    memcpy(out32, &st, sizeof st < sizeof out32 ? sizeof st : sizeof out32);
    return out32;
}
