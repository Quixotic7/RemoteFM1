/* SPDX-License-Identifier: MPL-2.0 */
/* A 128x64 canvas (a byte a pixel, 0 = off) and its packing into the Move's screen format, for hosts that draw. */
#ifndef MOVEREMOTE_GFX_H
#define MOVEREMOTE_GFX_H

#include <stdint.h>

#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t px[MR_SCREEN_H][MR_SCREEN_W]; } mr_canvas_t;

void mr_gfx_clear(mr_canvas_t *cv, int on);
void mr_gfx_pixel(mr_canvas_t *cv, int x, int y, int on);
void mr_gfx_rect(mr_canvas_t *cv, int x, int y, int w, int h, int on, int fill);
int mr_gfx_text(mr_canvas_t *cv, int x, int y, const char *s, int on);   /* 5x7 font, 6 px a character; returns x */
void mr_gfx_pack(const mr_canvas_t *cv, uint8_t packed[MR_SCREEN_BYTES]);
/* any 8-bit grey image of the screen's size, by a threshold (dither 0) or ordered 4x4 dithering (dither 1) */
void mr_gfx_from_grey(mr_canvas_t *cv, const uint8_t *grey, int stride, int dither);

#ifdef __cplusplus
}
#endif

#endif
