/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: the host session (docs/PROTOCOL.md section 2). Portable C99, single-threaded, no OS calls: the
 * caller gives it links (a write function each), feeds it the bytes each link received, and calls poll() with a
 * millisecond clock. Everything happens inside those calls.
 *
 * Links: RFM_LINK_SX (a MIDI port pair: SysEx out; in: SysEx and the panel's channel messages) and RFM_LINK_BK
 * (the vendor bulk pipe). The back end adds every candidate; the session finds the unit by HELLO only.
 *
 *   DISCOVER  HELLO on every link; within hello_ms, every link that answered "RemoteFM1" proto 2 counts. The first
 *             SX link that answered carries the panel events; the frame link (the session transport) is a BK link
 *             that answered (unless transport = MIDI), else that SX link (unless transport = BULK). Nothing found:
 *             the rescan callback, HELLO again after retry_ms.
 *   STARTING  SESSION start=1 keepalive_ms on the frame link, until its ACK (retried; after 4 tries: DISCOVER).
 *   ACTIVE    the model is black, frames count from 0. Every 1000/fps ms, when tiles are dirty and fewer than
 *             max_frames_in_flight frames are unacknowledged: one frame (rfm_screen_encode) on the frame link.
 *             LEDS on a change and at least once a second; something at least every keepalive_ms / 3.
 *             FRAME_ACK: decode error -> every tile resent; an unexpected shadow reset -> SESSION start again
 *             (model black). A frame not acknowledged within ack_timeout_ms: every tile resent. PANEL_R reconciles
 *             the panel; a PANEL_R without the session flag (the unit ended it), or no PANEL_R for heartbeat_ms
 *             (it is the session's heartbeat, every 500 ms) -> SESSION start again. Nothing at all heard from the unit
 *             for lost_ms, or a link write failing: DISCOVER.
 *   STOPPING  stop(): SESSION start=0, until its ACK or stop_ms; then STOPPED. */
#ifndef RFM_SESSION_H
#define RFM_SESSION_H
#include <stdint.h>
#include "codec.h"
#include "panel.h"
#include "screen.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RFM_T_AUTO = 0, RFM_T_MIDI = 1, RFM_T_BULK = 2 };
enum { RFM_S_DISCOVER = 0, RFM_S_STARTING, RFM_S_ACTIVE, RFM_S_STOPPING, RFM_S_STOPPED };
#define RFM_MAX_LINKS 16

typedef struct {
    uint32_t fps;                                       /* 20 */
    uint32_t budget;                                    /* bytes per frame; 0: from the transport (screen.h) */
    uint32_t keepalive_ms;                              /* 3000 */
    uint32_t hello_ms;                                  /* 300: discovery window */
    uint32_t retry_ms;                                  /* 1000: between discoveries */
    uint32_t start_ms;                                  /* 400: SESSION's ACK */
    uint32_t ack_timeout_ms;                            /* 1000: a FRAME_ACK */
    uint32_t lost_ms;                                   /* 2000: nothing at all from the unit */
    uint32_t heartbeat_ms;                              /* 1600: no PANEL_R (it comes every 500 ms in a session) */
    uint32_t stop_ms;                                   /* 300 */
    uint32_t settle_ms;                                 /* 60: panel.h */
    uint32_t sx_body_max, bk_body_max;                  /* 0: the spec's (1024 / 16384), lowered by HELLO_R's */
    int transport;                                      /* RFM_T_* */
    const char *host_name;                              /* HELLO's body ("fm1remote") */
} rfm_session_cfg_t;
void rfm_session_cfg_default(rfm_session_cfg_t *c);

typedef int (*rfm_write_fn)(void *ctx, const uint8_t *p, uint32_t n);     /* 0 ok, else the link failed */

typedef struct {
    void *ctx;
    void (*event)(void *ctx, const rfm_event_t *ev);    /* a panel event */
    void (*state)(void *ctx, int state, const char *why);
    void (*rescan)(void *ctx);                          /* discovery found no unit: refresh the links */
    void (*log)(void *ctx, const char *msg);
} rfm_session_cb_t;

typedef struct {
    uint32_t frames, msgs, tiles, rects, body_bytes, wire_bytes;
    uint32_t acks, ack_ms_sum, ack_ms_max, ack_timeouts, decode_errs, shadow_resets;
    uint32_t leds_sent, keepalives, panel_r, events, sessions, discoveries, lost;
    uint32_t in_flight, pending;
    uint32_t last_ack_ms;
} rfm_session_stats_t;

typedef struct rfm_session rfm_session_t;
rfm_session_t *rfm_session_new(const rfm_session_cfg_t *cfg, const rfm_session_cb_t *cb);
void rfm_session_free(rfm_session_t *s);

int rfm_session_add_link(rfm_session_t *s, int kind, rfm_write_fn write, void *ctx, const char *name);  /* id, -1 */
void rfm_session_remove_link(rfm_session_t *s, int id, uint32_t now_ms);
void rfm_session_rx(rfm_session_t *s, int id, const uint8_t *p, uint32_t n, uint32_t now_ms);
void rfm_session_poll(rfm_session_t *s, uint32_t now_ms);

void rfm_session_set_frame(rfm_session_t *s, const uint16_t *lcd);     /* a new screen (copied) */
void rfm_session_set_leds(rfm_session_t *s, const rfm_leds_t *l);
void rfm_session_stop(rfm_session_t *s, uint32_t now_ms);               /* SESSION start=0, then STOPPED */
void rfm_session_restart(rfm_session_t *s, uint32_t now_ms);            /* back to DISCOVER (after stop too) */

int rfm_session_state(const rfm_session_t *s);
const char *rfm_session_state_name(int state);
const rfm_hello_t *rfm_session_hello(const rfm_session_t *s);           /* the unit's, once found */
int rfm_session_frame_link(const rfm_session_t *s);                     /* -1 none */
int rfm_session_midi_link(const rfm_session_t *s);
int rfm_session_link_kind(const rfm_session_t *s, int id);
uint32_t rfm_session_budget(const rfm_session_t *s);                    /* bytes per frame in use */
void rfm_session_stats(const rfm_session_t *s, rfm_session_stats_t *st);
const rfm_panel_t *rfm_session_panel(const rfm_session_t *s);
const rfm_screen_t *rfm_session_screen(const rfm_session_t *s);

#ifdef __cplusplus
}
#endif
#endif
