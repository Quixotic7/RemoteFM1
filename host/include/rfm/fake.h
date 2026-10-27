/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: a fake RemoteFM1 unit, the device side of docs/PROTOCOL.md v2 as far as a host can see it.
 * Portable C99. The tests and fm1remote's loopback mode (no --device) talk to it instead of the FM-1.
 *
 * It has a MIDI side (SysEx in, SysEx replies and the panel's plain MIDI out) and, optionally, a bulk side. The
 * host writes with rx_sx / rx_bk; the replies queue until the host takes them (take_sx / take_bk). poll() runs its
 * timers: keepalive expiry, PANEL_R every 500 ms in a session, FRAME_ACKs when the simulated wire has carried the
 * frame (rate_bps per side, 0: at once). It draws TILES with the reference decoder (tiles_ref.h).
 * Outside a session it sends no PANEL_R; whether it still draws and acknowledges TILES is cfg.tiles_outside_session
 * (the spec leaves that open).
 * Faults for the tests: reboot (shadow black, session over, the next FRAME_ACK says shadow reset), deafness (host
 * messages lost), dropped FRAME_ACKs, dropped panel events (PANEL_R must repair them), a decode error. */
#ifndef RFM_FAKE_H
#define RFM_FAKE_H
#include <stdint.h>
#include "codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;                                   /* "RemoteFM1"; another name / proto plays another firmware */
    uint8_t proto;                                      /* 2 */
    int silent;                                         /* never answers anything (a firmware that ignores us) */
    int has_bulk;
    uint32_t sx_rate_bps, bk_rate_bps;                  /* the simulated wire for TILES; 0: instant */
    uint16_t sx_body_max, bk_body_max;                  /* advertised and enforced (1024 / 16384) */
    int ble;                                            /* 1: the MIDI side is Bluetooth MIDI (0.2): its HELLO_R
                                                         * says sxq 128 and the Bluetooth bits; 2: USB while a
                                                         * Bluetooth computer is connected (the bits, sxq 960) */
    int tiles_outside_session;                          /* 1 (default): TILES / LEDS outside a session are still
                                                         * drawn and acknowledged (FRAME_ACK says shadow reset after
                                                         * a reboot); 0: ignored, no FRAME_ACK */
} rfm_fake_cfg_t;
void rfm_fake_cfg_default(rfm_fake_cfg_t *c);

typedef struct {
    uint32_t hellos, sessions_started, sessions_ended, keepalive_expired;
    uint32_t leds, tiles_msgs, frames, acks, acks_dropped, tile_bytes, decode_errs, panel_r, panel_req, msgs;
    uint32_t sx_msgs, bk_msgs;
} rfm_fake_stats_t;

typedef struct rfm_fake rfm_fake_t;
rfm_fake_t *rfm_fake_new(const rfm_fake_cfg_t *cfg);
void rfm_fake_free(rfm_fake_t *f);

void rfm_fake_rx_sx(rfm_fake_t *f, const uint8_t *p, uint32_t n, uint32_t now_ms);
void rfm_fake_rx_bk(rfm_fake_t *f, const uint8_t *p, uint32_t n, uint32_t now_ms);
uint32_t rfm_fake_take_sx(rfm_fake_t *f, uint8_t *out, uint32_t cap);  /* device -> host MIDI bytes */
uint32_t rfm_fake_take_bk(rfm_fake_t *f, uint8_t *out, uint32_t cap);
void rfm_fake_poll(rfm_fake_t *f, uint32_t now_ms);

/* the panel (sent as plain MIDI at once, and kept for PANEL_R) */
void rfm_fake_key(rfm_fake_t *f, uint32_t k, int down);
void rfm_fake_button(rfm_fake_t *f, uint32_t b, int down);
void rfm_fake_enc(rfm_fake_t *f, uint32_t e, int32_t detents);
void rfm_fake_master(rfm_fake_t *f, uint32_t adc);

/* faults */
void rfm_fake_reboot(rfm_fake_t *f);
void rfm_fake_deaf(rfm_fake_t *f, int on);              /* host -> device messages vanish */
void rfm_fake_drop_acks(rfm_fake_t *f, uint32_t n);     /* the next n FRAME_ACKs are not sent */
void rfm_fake_drop_midi(rfm_fake_t *f, uint32_t n);     /* the next n panel events are not sent */
void rfm_fake_decode_error(rfm_fake_t *f);              /* the next TILES body is rejected */

int rfm_fake_session(const rfm_fake_t *f);              /* 0 none, 1 on SysEx, 2 on bulk */
const uint8_t *rfm_fake_shadow(const rfm_fake_t *f);    /* 240 x 240 x 2 bytes, BE: emu_hal_t.lcd's layout */
const rfm_leds_t *rfm_fake_leds(const rfm_fake_t *f);
void rfm_fake_stats(const rfm_fake_t *f, rfm_fake_stats_t *st);

#ifdef __cplusplus
}
#endif
#endif
