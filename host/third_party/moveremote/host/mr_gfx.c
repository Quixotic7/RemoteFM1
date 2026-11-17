/* SPDX-License-Identifier: MPL-2.0 */
#include "../include/moveremote/gfx.h"

#include <string.h>

#include "../common/font5x7.h"

void mr_gfx_clear(mr_canvas_t *cv, int on) { memset(cv->px, on ? 1 : 0, sizeof cv->px); }

void mr_gfx_pixel(mr_canvas_t *cv, int x, int y, int on)
{
    if (x >= 0 && x < MR_SCREEN_W && y >= 0 && y < MR_SCREEN_H)
        cv->px[y][x] = (uint8_t)(on ? 1 : 0);
}

void mr_gfx_rect(mr_canvas_t *cv, int x, int y, int w, int h, int on, int fill)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (fill || j == y || j == y + h - 1 || i == x || i == x + w - 1)
                mr_gfx_pixel(cv, i, j, on);
}

int mr_gfx_text(mr_canvas_t *cv, int x, int y, const char *s, int on)
{
    for (; *s; s++, x += MR_FONT_ADV) {
        const uint8_t *g = mr_glyph(*s);
        int i, j;
        for (i = 0; i < MR_FONT_W; i++)
            for (j = 0; j < MR_FONT_H; j++)
                if (g[i] >> j & 1)
                    mr_gfx_pixel(cv, x + i, y + j, on);
    }
    return x;
}

void mr_gfx_pack(const mr_canvas_t *cv, uint8_t packed[MR_SCREEN_BYTES])
{
    int page, x, b;
    for (page = 0; page < MR_SCREEN_H / 8; page++)
        for (x = 0; x < MR_SCREEN_W; x++) {
            uint8_t v = 0;
            for (b = 0; b < 8; b++)
                v |= (uint8_t)((cv->px[page * 8 + b][x] ? 1 : 0) << b);
            packed[page * MR_SCREEN_W + x] = v;
        }
}

void mr_gfx_from_grey(mr_canvas_t *cv, const uint8_t *grey, int stride, int dither)
{
    static const uint8_t BAYER[4][4] = { { 8, 136, 40, 168 }, { 200, 72, 232, 104 }, { 56, 184, 24, 152 },
                                         { 248, 120, 216, 88 } };
    int x, y;
    for (y = 0; y < MR_SCREEN_H; y++)
        for (x = 0; x < MR_SCREEN_W; x++) {
            const uint8_t v = grey[y * stride + x];
            cv->px[y][x] = (uint8_t)(dither ? v > BAYER[y & 3][x & 3] : v >= 128);
        }
}
