/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The TILES decoder (docs/PROTOCOL.md section 3): a TILES body's rectangles, in order, into a 240 x 240 shadow
 * frame buffer. Plain C99 that depends only on proto/rfm1.h: the device builds it into remote.c, the host tests
 * (tests/rfm_draw_test.c) and the host library's cross test compile it natively.
 *
 * The shadow holds each pixel as its two wire bytes in memory order (RGB565 big endian, as the panel takes it and
 * as emu_hal_t.lcd stores it), so a row of the shadow is DMA'd to the panel as it is. The code assumes a
 * little-endian CPU (the FM-1's pi32v2, x86-64, arm64); the tests check the shadow's bytes, so a big-endian host
 * would fail them rather than pass wrongly.
 *
 * Every rectangle is checked whole (bounds, enc, payload length, PAL n and indices, RLE runs) before a pixel is
 * written: a malformed rectangle writes nothing, and it and the rest of the message are dropped. Nothing outside
 * fb[0 .. 240 * 240) is ever written and nothing outside body[0 .. len) is ever read. */
#ifndef RFM_DRAW_H
#define RFM_DRAW_H
#include <stdint.h>
#include "../../proto/rfm1.h"

/* a rectangle of the shadow was written (x, y, w, h: inside the screen, w, h >= 1) */
typedef void (*rfm_dirty_fn)(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

typedef struct {
    uint16_t *fb;                       /* RFM_W * RFM_H pixels, row-major */
    rfm_dirty_fn dirty;                 /* NULL: nobody tracks */
    void *ctx;
} rfm_draw_t;

enum {
    RFM_DRAW_OK = 0,
    RFM_DRAW_E_HDR,                     /* the body is shorter than the 4-byte TILES header */
    RFM_DRAW_E_RECT,                    /* a rectangle header cut off (fewer than 5 bytes left) */
    RFM_DRAW_E_BOUNDS,                  /* w or h 0, or x + w > 240, or y + h > 240 */
    RFM_DRAW_E_ENC,                     /* an unknown enc */
    RFM_DRAW_E_SHORT,                   /* the payload is cut off (RAW, FILL, PAL, RLE short of w * h, COPY) */
    RFM_DRAW_E_PAL_N,                   /* PAL n outside 2..16 */
    RFM_DRAW_E_PAL_INDEX,               /* a PAL index >= n */
    RFM_DRAW_E_RLE_OVERRUN,             /* a run goes past w * h */
    RFM_DRAW_E_COPY_SRC,                /* COPY: sx + w > 240 or sy + h > 240 */
    RFM_DRAW_E_TRAILING                 /* bytes after the last of nrects rectangles (all of them were drawn) */
};

typedef struct {
    uint32_t frame, flags, nrects;      /* the header (0 when err is RFM_DRAW_E_HDR) */
    uint32_t applied;                   /* rectangles drawn */
    uint32_t err;                       /* RFM_DRAW_* */
    uint32_t err_off;                   /* body offset of the bad rectangle, or of the trailing bytes */
} rfm_draw_res_t;

/* a whole TILES body; returns r->err */
uint32_t rfm_draw_tiles(const rfm_draw_t *d, const uint8_t *body, uint32_t len, rfm_draw_res_t *r);
/* one rectangle (x8 y8 w8 h8 enc8 payload) at p[0 .. n): its size in bytes, or 0 and *err (nothing drawn) */
uint32_t rfm_draw_rect(const rfm_draw_t *d, const uint8_t *p, uint32_t n, uint32_t *err);
/* M0's raw TILE: w * h big-endian pixels at px (bounds checked by the caller) */
void rfm_draw_raw(const rfm_draw_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px);
/* the shadow all one colour (c: RGB565 as a number), and all of it dirty */
void rfm_draw_clear(const rfm_draw_t *d, uint32_t c);
#endif
