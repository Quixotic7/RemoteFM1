/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: the screen encoder (PLAN.md section 3, docs/PROTOCOL.md section 3 TILES). Portable C99.
 *
 * Input: a 240x240 RGB565 frame exactly as emu_hal_t.lcd stores it (each uint16 slot holds the pixel's two bytes
 * big-endian in memory, i.e. the bytes as the panel takes them). The encoder never interprets a pixel: it compares
 * slots and copies their two memory bytes to the wire, so it is endian-neutral.
 *
 * The model is exact: it is what the device's shadow holds once it has applied everything sent. Lossless always.
 *
 *   submit(frame)    the new target. Every 16x16 tile that differs from the model (or is forced: invalidate) is
 *                    dirty; a tile keeps the age (submit counter) at which it first became dirty until it is sent.
 *   encode(...)      one frame: dirty tiles oldest-first (ties: raster order), each costed at its smallest encoding
 *                    (FILL / PAL 2..16 / RLE / RAW); the oldest always goes, the rest while they fit the byte budget
 *                    (a tile that does not fit is skipped, a smaller younger one may still go). Then horizontal
 *                    neighbours in a tile row are merged greedily while the merged rect is cheaper than the parts
 *                    (a row of same-colour FILL tiles becomes one FILL rect, a two-colour text line one PAL rect).
 *                    Rects are packed in raster order into TILES bodies of at most body_max bytes (and at most 255
 *                    rects); the last one carries END. The model takes the sent pixels.
 * The budget counts rect bytes (5-byte rect header + payload); each TILES body adds 4 bytes of header. */
#ifndef RFM_SCREEN_H
#define RFM_SCREEN_H
#include <stdint.h>
#include "rfm1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFM_TILE 16u
#define RFM_TILES_X (RFM_W / RFM_TILE)
#define RFM_TILES_Y (RFM_H / RFM_TILE)
#define RFM_NTILES (RFM_TILES_X * RFM_TILES_Y)
#define RFM_NPIX (RFM_W * RFM_H)

/* default byte budgets per second (M0, PLAN.md section 10: bulk ~830 KB/s, SysEx ~300 KB/s host -> device);
 * a frame's budget is rate / fps: 40000 and 14000 bytes at 20 Hz */
#define RFM_BUDGET_BK_BPS 800000u
#define RFM_BUDGET_SX_BPS 280000u
/* Bluetooth MIDI (RemoteFM1 0.2): ~6 KB/s of TILES body measured on a unit; 4000 leaves room for LEDS, keepalives,
 * PANEL_R and BLE MIDI's framing (session.h: Bluetooth pacing) */
#define RFM_BUDGET_BLE_BPS 4000u
#define RFM_BODY_MIN 64u                                /* the smallest body_max the encoder accepts */

typedef void (*rfm_emit_fn)(void *ctx, const uint8_t *body, uint32_t n, int last);

typedef struct {
    uint32_t tiles;                                     /* tiles sent */
    uint32_t rects;                                     /* rects sent (after merging and splitting) */
    uint32_t rect_bytes;                                /* what the budget counts */
    uint32_t body_bytes;                                /* rect bytes + TILES headers */
    uint32_t msgs;                                      /* TILES bodies */
    uint32_t by_enc[5];                                 /* rects per RFM_ENC_* */
    uint32_t pending;                                   /* dirty tiles left after this frame */
    uint32_t max_wait;                                  /* the oldest sent tile's wait, in submits */
} rfm_enc_stats_t;

typedef struct rfm_screen rfm_screen_t;
rfm_screen_t *rfm_screen_new(void);                     /* model all black */
void rfm_screen_free(rfm_screen_t *s);
void rfm_screen_reset_black(rfm_screen_t *s);           /* the device cleared its shadow: model = black, exact */
void rfm_screen_invalidate(rfm_screen_t *s);            /* the device shadow is unknown: every tile is resent */
void rfm_screen_submit(rfm_screen_t *s, const uint16_t *lcd);
uint32_t rfm_screen_pending(const rfm_screen_t *s);     /* dirty tiles */
int rfm_screen_tile_dirty(const rfm_screen_t *s, uint32_t tile);   /* tile = ty * RFM_TILES_X + tx */
const uint16_t *rfm_screen_model(const rfm_screen_t *s);
const uint16_t *rfm_screen_target(const rfm_screen_t *s);
void rfm_screen_set_merge(rfm_screen_t *s, int on);     /* default on (off: one rect per tile, for tests) */
/* one frame; returns the number of TILES bodies emitted (0: nothing was dirty, nothing emitted) */
uint32_t rfm_screen_encode(rfm_screen_t *s, uint16_t frame, uint32_t budget, uint32_t body_max, rfm_emit_fn emit,
                           void *ctx, rfm_enc_stats_t *st);

/* one rect of img (240x240, stride 240): enc = RFM_ENC_* or -1 for the smallest; returns the bytes written to out
 * (rect header included), 0 if it does not fit cap or enc cannot code it (FILL of 2 colours, PAL of > 16) */
uint32_t rfm_rect_encode(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int enc, uint8_t *out,
                         uint32_t cap);
/* the smallest encoding's size and its enc */
uint32_t rfm_rect_cost(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int *enc);

#ifdef __cplusplus
}
#endif
#endif
