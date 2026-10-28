/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1 for the browser (PLAN.md M4, WEB-PLAN.md section 2 "Use my FM-1"): a small C API over the portable
 * session (host/src/session.c with codec, screen, panel), compiled with Emscripten. One session per module
 * instance. The JS back ends (web/remote/remote.js: Web MIDI, WebUSB) own the links; this file only turns the
 * session's callbacks into calls of the JS functions the module instance carries:
 *
 *   Module.rfmWrite(tag, Uint8Array) -> 0 ok, else the link failed      (a copy: the bytes may be kept)
 *   Module.rfmEvent(type, index, source, value)                          (rfm_event_t)
 *   Module.rfmState(state, why)                                          (RFM_S_*)
 *   Module.rfmRescan()
 *   Module.rfmLog(msg)
 *
 * Everything runs inside the calls JS makes (rw_rx, rw_poll, ..), single-threaded, on the page's main thread or a
 * Worker: never in the AudioWorklet. */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "rfm/session.h"

EM_JS(int, js_write, (int tag, const uint8_t *p, uint32_t n), {
    return Module.rfmWrite(tag, HEAPU8.slice(p, p + n)) | 0;
});
EM_JS(void, js_event, (int type, int index, int source, int value), {
    if (Module.rfmEvent) Module.rfmEvent(type, index, source, value);
});
EM_JS(void, js_state, (int state, const char *why), {
    if (Module.rfmState) Module.rfmState(state, UTF8ToString(why));
});
EM_JS(void, js_rescan, (void), {
    if (Module.rfmRescan) Module.rfmRescan();
});
EM_JS(void, js_log, (const char *msg), {
    if (Module.rfmLog) Module.rfmLog(UTF8ToString(msg));
});

static rfm_session_t *S;
static uint8_t *rxbuf;
static uint32_t rxcap;
static uint16_t frame_host[RFM_W * RFM_H], frame_be[RFM_W * RFM_H];
static uint8_t leds_levels[RFM_NCOL * 4u];
static uint32_t out32[64];
static char host_name[17] = "web";
static int ble_colors = -1;                             /* rw_set_ble_colors(); -1: the library's default */

static int w_link(void *ctx, const uint8_t *p, uint32_t n) { return js_write((int)(intptr_t)ctx, p, n); }
static void cb_event(void *ctx, const rfm_event_t *e)
{
    (void)ctx;
    js_event(e->type, e->index, e->source, e->value);
}
static void cb_state(void *ctx, int st, const char *why)
{
    (void)ctx;
    js_state(st, why ? why : "");
}
static void cb_rescan(void *ctx)
{
    (void)ctx;
    js_rescan();
}
static void cb_log(void *ctx, const char *m)
{
    (void)ctx;
    js_log(m);
}

/* a new session (any old one is freed). transport: RFM_T_AUTO / MIDI / BULK; 0 for a value: the default */
EMSCRIPTEN_KEEPALIVE int rw_init(int fps, int transport, int keepalive_ms, int budget, const char *name)
{
    rfm_session_cfg_t c;
    rfm_session_cb_t cb;
    if (S)
        rfm_session_free(S);
    rfm_session_cfg_default(&c);
    if (fps > 0)
        c.fps = (uint32_t)fps;
    c.transport = transport;
    if (keepalive_ms >= 0)
        c.keepalive_ms = (uint32_t)keepalive_ms;
    if (budget > 0)
        c.budget = (uint32_t)budget;
    if (ble_colors >= 0)
        c.ble_colors = (uint32_t)ble_colors;
    if (name && *name) {
        strncpy(host_name, name, 16);
        host_name[16] = 0;
    }
    c.host_name = host_name;
    cb.ctx = NULL;
    cb.event = cb_event;
    cb.state = cb_state;
    cb.rescan = cb_rescan;
    cb.log = cb_log;
    S = rfm_session_new(&c, &cb);
    return S ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE void rw_free(void)
{
    rfm_session_free(S);
    S = NULL;
}

/* kind RFM_LINK_SX (0) or RFM_LINK_BK (1); tag: what rfmWrite gets. Returns the session's link id or -1 */
EMSCRIPTEN_KEEPALIVE int rw_add_link(int kind, int tag, const char *name)
{
    return S ? rfm_session_add_link(S, kind, w_link, (void *)(intptr_t)tag, name) : -1;
}
EMSCRIPTEN_KEEPALIVE void rw_remove_link(int id, uint32_t now)
{
    if (S)
        rfm_session_remove_link(S, id, now);
}

/* the input buffer: JS writes n bytes there, then rw_rx(id, n, now) */
EMSCRIPTEN_KEEPALIVE uint8_t *rw_buf(uint32_t n)
{
    if (n > rxcap) {
        uint8_t *b = (uint8_t *)realloc(rxbuf, n);
        if (!b)
            return NULL;
        rxbuf = b;
        rxcap = n;
    }
    return rxbuf;
}
EMSCRIPTEN_KEEPALIVE void rw_rx(int id, uint32_t n, uint32_t now)
{
    if (S && rxbuf && n <= rxcap)
        rfm_session_rx(S, id, rxbuf, n, now);
}
EMSCRIPTEN_KEEPALIVE void rw_poll(uint32_t now)
{
    if (S)
        rfm_session_poll(S, now);
}

/* the screen: JS writes 240 x 240 RGB565 values in host order (engine.screen()'s pixels) to rw_frame_buf(), then
 * rw_set_frame(). The encoder wants emu_hal_t.lcd's layout (each slot holds the pixel's bytes big-endian in
 * memory), so every value is byte-swapped here (WASM is little endian). */
EMSCRIPTEN_KEEPALIVE uint16_t *rw_frame_buf(void) { return frame_host; }
EMSCRIPTEN_KEEPALIVE void rw_set_frame(void)
{
    uint32_t i;
    if (!S)
        return;
    for (i = 0; i < RFM_W * RFM_H; i++)
        frame_be[i] = (uint16_t)((frame_host[i] >> 8) | (frame_host[i] << 8));
    rfm_session_set_frame(S, frame_be);
}

/* the LEDs: 44 levels levels[col * 4 + line] (engine.leds(), fm1panel_t's layout) and PLAY green 0..255. Each level
 * becomes its plane by FM1VST's fm1p_led_class (lit >= 176, mid >= 96, dim >= 40, faint > 0); line 0..3 is the
 * column's row bit 1..4 (core-glue/common/panel.h); PLAY green is on when lit (FM1VST Device::led_play_green). */
EMSCRIPTEN_KEEPALIVE uint8_t *rw_leds_buf(void) { return leds_levels; }
EMSCRIPTEN_KEEPALIVE void rw_set_leds(int play_green)
{
    rfm_leds_t l;
    uint32_t col, line;
    if (!S)
        return;
    memset(&l, 0, sizeof l);
    for (col = 0; col < RFM_NCOL; col++)
        for (line = 0; line < 4u; line++) {
            const uint8_t v = leds_levels[col * 4u + line], bit = (uint8_t)(1u << (line + 1u));
            if (v >= 176u)
                l.lit[col] |= bit;
            else if (v >= 96u)
                l.mid[col] |= bit;
            else if (v >= 40u)
                l.dim[col] |= bit;
            else if (v)
                l.faint[col] |= bit;
        }
    l.flags = play_green >= 176 ? RFM_LEDS_F_PLAY_GREEN : 0;
    rfm_session_set_leds(S, &l);
}

/* Bluetooth's lossy tiles: 0 lossless (the reduced tiles shown are then sent exactly at once), 2..16 colours a tile,
 * -1 the library's default (RFM_BLE_COLORS); now and for later sessions (rw_init) */
EMSCRIPTEN_KEEPALIVE void rw_set_ble_colors(int n)
{
    ble_colors = n;
    if (S)
        rfm_session_set_ble_colors(S, n >= 0 ? (uint32_t)n : RFM_BLE_COLORS);
}

EMSCRIPTEN_KEEPALIVE void rw_stop(uint32_t now)
{
    if (S)
        rfm_session_stop(S, now);
}
EMSCRIPTEN_KEEPALIVE void rw_restart(uint32_t now)
{
    if (S)
        rfm_session_restart(S, now);
}
EMSCRIPTEN_KEEPALIVE int rw_state(void) { return S ? rfm_session_state(S) : RFM_S_STOPPED; }
EMSCRIPTEN_KEEPALIVE int rw_frame_link(void) { return S ? rfm_session_frame_link(S) : -1; }
EMSCRIPTEN_KEEPALIVE int rw_midi_link(void) { return S ? rfm_session_midi_link(S) : -1; }
EMSCRIPTEN_KEEPALIVE int rw_link_kind(int id) { return S ? rfm_session_link_kind(S, id) : -1; }
EMSCRIPTEN_KEEPALIVE uint32_t rw_budget(void) { return S ? rfm_session_budget(S) : 0u; }

/* the stats, as uint32s in rfm_session_stats_t's order (remote.js STAT_NAMES) */
EMSCRIPTEN_KEEPALIVE uint32_t *rw_stats(void)
{
    rfm_session_stats_t st;
    memset(out32, 0, sizeof out32);
    if (S) {
        rfm_session_stats(S, &st);
        memcpy(out32, &st, sizeof st < sizeof out32 ? sizeof st : sizeof out32);
    }
    return out32;
}
EMSCRIPTEN_KEEPALIVE uint32_t rw_stats_count(void) { return (uint32_t)(sizeof(rfm_session_stats_t) / 4u); }

/* the panel as the host believes it is: keys, buttons, master, have_master, battery_mv, enc_count[7] */
EMSCRIPTEN_KEEPALIVE uint32_t *rw_panel(void)
{
    uint32_t i;
    memset(out32, 0, sizeof out32);
    if (S) {
        const rfm_panel_t *p = rfm_session_panel(S);
        out32[0] = p->keys;
        out32[1] = p->buttons;
        out32[2] = p->master;
        out32[3] = p->have_master;
        out32[4] = p->battery_mv;
        for (i = 0; i < RFM_NENC; i++)
            out32[5 + i] = (uint32_t)(int32_t)p->enc_count[i];
    }
    return out32;
}

/* the unit's HELLO_R once found: [caps, max_in_flight, proto, screen_w, screen_h], then rw_hello_str(0 name, 1 fw) */
EMSCRIPTEN_KEEPALIVE uint32_t *rw_hello(void)
{
    memset(out32, 0, sizeof out32);
    if (S) {
        const rfm_hello_t *h = rfm_session_hello(S);
        out32[0] = h->caps;
        out32[1] = h->max_in_flight;
        out32[2] = h->proto;
        out32[3] = h->screen_w;
        out32[4] = h->screen_h;
    }
    return out32;
}
EMSCRIPTEN_KEEPALIVE const char *rw_hello_str(int which)
{
    const rfm_hello_t *h;
    if (!S)
        return "";
    h = rfm_session_hello(S);
    return which == 0 ? h->name : which == 1 ? h->fw_ver : which == 2 ? h->identity : h->ver;
}
