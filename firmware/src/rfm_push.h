/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The screen push: which parts of the shadow (rfm_draw.h) the panel has not seen yet, and a step-at-a-time engine
 * that sends them, so the caller's loop keeps servicing USB while the SPI DMA runs. Plain C99, host-compilable
 * (tests/rfm_draw_test.c drives it against a model panel whose DMA reads the shadow a little at a time).
 *
 * Dirty set: one bit per 16 x 16 tile (15 bands of 15 tiles). A push takes a snapshot of it (the plan) and clears
 * it; whatever is drawn while the push runs is marked again, for the next push. The plan goes out as rectangles:
 * a run of dirty tiles in a band, grown down over the bands below where the same tiles are dirty. A rectangle as
 * wide as the screen is DMA'd from the shadow itself (its rows are contiguous), in pieces of at most
 * RFM_PUSH_DMA_MAX bytes; a narrower one is copied a few rows at a time into a staging buffer and DMA'd from there.
 * The copy happens only when the previous transfer is over, so one staging buffer is enough.
 *
 * Correctness while TILES keep arriving: the decoder may write a part of the shadow that the running push has
 * still to send (the panel then gets the newer pixels early) or has sent already (the panel is stale there). Both
 * parts were marked dirty again by that write, so the push after the next END sends them: when the pushes run
 * out, the panel equals the shadow. */
#ifndef RFM_PUSH_H
#define RFM_PUSH_H
#include <stdint.h>
#include "../../proto/rfm1.h"

#define RFM_TILE 16u
#define RFM_TILES_X (RFM_W / RFM_TILE)          /* 15 */
#define RFM_TILES_Y (RFM_H / RFM_TILE)          /* 15 */
#define RFM_PUSH_DMA_MAX 28800u                 /* bytes per transfer from the shadow (60 full rows; M0's size) */

typedef struct {
    int (*idle)(void *ctx);                     /* no transfer is running: the next may start */
    void (*window)(void *ctx, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);   /* CASET / RASET / RAMWR */
    void (*data)(void *ctx, const void *p, uint32_t n);                               /* start n bytes; at once */
    void *ctx;
} rfm_lcd_ops_t;

typedef struct {
    const uint16_t *fb;                         /* the shadow, RFM_W x RFM_H */
    uint16_t *stage;                            /* staging buffer, stage_px pixels (>= RFM_W) */
    uint32_t stage_px;
    rfm_lcd_ops_t lcd;
    uint16_t dirty[RFM_TILES_Y];                /* bit tx: tile (tx, ty) changed since the last snapshot */
    uint16_t plan[RFM_TILES_Y];                 /* the running push's tiles not yet sent */
    uint8_t active;                             /* a push runs */
    uint8_t band;                               /* the plan's next band to look at */
    uint32_t rx, ry, rw, rh, row;               /* the rectangle being sent; row: rows of it started */
    uint32_t rects, xfers, bytes;               /* this push's counts (diagnostics) */
} rfm_push_t;

void rfm_push_init(rfm_push_t *p, const uint16_t *fb, uint16_t *stage, uint32_t stage_px, const rfm_lcd_ops_t *lcd);
void rfm_push_mark(rfm_push_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void rfm_push_mark_all(rfm_push_t *p);
int rfm_push_pending(const rfm_push_t *p);      /* anything dirty (not counting a running push's plan) */
void rfm_push_start(rfm_push_t *p);             /* snapshot the dirty set; a push runs (perhaps an empty one) */
/* one step, never waiting: 1 while the push runs, 0 when it is over (the last transfer finished) or none runs */
int rfm_push_step(rfm_push_t *p);
/* the panel was drawn on by someone else (or a transfer was cut): stop, everything dirty */
void rfm_push_abort(rfm_push_t *p);
/* rfm_dirty_fn for rfm_draw_t: ctx is the rfm_push_t */
void rfm_push_dirty_cb(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#endif
