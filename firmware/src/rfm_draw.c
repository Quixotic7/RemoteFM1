/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The TILES decoder: see rfm_draw.h. No libc (the device has only a byte-wise memcpy): pixel loops. */
#include "rfm_draw.h"

#define RD_W RFM_W
#define RD_H RFM_H

/* the 16-bit value whose memory bytes are p[0], p[1] (little-endian CPU): a wire pixel as the shadow keeps it */
static inline uint16_t rd_px(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8); }

static void rd_dirty(const rfm_draw_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (d->dirty)
        d->dirty(d->ctx, x, y, w, h);
}

static void rd_fill(const rfm_draw_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h; j++) {
        uint16_t *row = d->fb + (y + j) * RD_W + x;
        for (i = 0; i < w; i++)
            row[i] = c;
    }
}

void rfm_draw_raw(const rfm_draw_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++) {
        uint16_t *row = d->fb + (y + j) * RD_W + x;
        const uint8_t *s = px + 2u * j * w;
        for (i = 0; i < w; i++)
            row[i] = rd_px(s + 2u * i);
    }
}

void rfm_draw_clear(const rfm_draw_t *d, uint32_t c)
{
    uint8_t b[2];
    b[0] = (uint8_t)(c >> 8);                           /* big endian on the wire */
    b[1] = (uint8_t)c;
    rd_fill(d, 0, 0, RD_W, RD_H, rd_px(b));
    rd_dirty(d, 0, 0, RD_W, RD_H);
}

static inline uint32_t rd_pal_index(const uint8_t *row, uint32_t i, uint32_t bpp)
{
    uint32_t bit = i * bpp;
    return (uint32_t)(row[bit >> 3] >> (8u - bpp - (bit & 7u))) & ((1u << bpp) - 1u);
}

/* COPY as memmove: rows in the order that reads each source row before it is overwritten, each row likewise */
static void rd_copy(const rfm_draw_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy)
{
    uint32_t k, i;
    int up = y > sy;                                    /* the block moves down: bottom row first */
    for (k = 0; k < h; k++) {
        uint32_t j = up ? h - 1u - k : k;
        uint16_t *dst = d->fb + (y + j) * RD_W + x;
        const uint16_t *src = d->fb + (sy + j) * RD_W + sx;
        if (dst > src)
            for (i = w; i--;)
                dst[i] = src[i];
        else if (dst < src)
            for (i = 0; i < w; i++)
                dst[i] = src[i];
    }
}

uint32_t rfm_draw_rect(const rfm_draw_t *d, const uint8_t *p, uint32_t n, uint32_t *err)
{
    uint32_t x, y, w, h, left, size, i, j;
    const uint8_t *q;
    if (n < RFM_RECT_HDR) {
        *err = RFM_DRAW_E_RECT;
        return 0;
    }
    x = p[0];
    y = p[1];
    w = p[2];
    h = p[3];
    q = p + RFM_RECT_HDR;
    left = n - RFM_RECT_HDR;
    if (!w || !h || x + w > RD_W || y + h > RD_H) {
        *err = RFM_DRAW_E_BOUNDS;
        return 0;
    }
    switch (p[4]) {
    case RFM_ENC_RAW:
        size = 2u * w * h;
        if (left < size)
            goto short_;
        rfm_draw_raw(d, x, y, w, h, q);
        break;
    case RFM_ENC_FILL:
        size = 2;
        if (left < size)
            goto short_;
        rd_fill(d, x, y, w, h, rd_px(q));
        break;
    case RFM_ENC_PAL: {
        uint32_t np, bpp, rb;
        uint16_t pal[RFM_PAL_MAX];
        const uint8_t *ix;
        if (left < 1u)
            goto short_;
        np = q[0];
        if (np < RFM_PAL_MIN || np > RFM_PAL_MAX) {
            *err = RFM_DRAW_E_PAL_N;
            return 0;
        }
        bpp = rfm_pal_bpp(np);
        rb = rfm_pal_row_bytes(w, np);
        size = 1u + 2u * np + h * rb;
        if (left < size)
            goto short_;
        for (i = 0; i < np; i++)
            pal[i] = rd_px(q + 1u + 2u * i);
        ix = q + 1u + 2u * np;
        if (np != 1u << bpp)                            /* 3, 5..15: an index may be >= n */
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++)
                    if (rd_pal_index(ix + j * rb, i, bpp) >= np) {
                        *err = RFM_DRAW_E_PAL_INDEX;
                        return 0;
                    }
        for (j = 0; j < h; j++) {
            uint16_t *row = d->fb + (y + j) * RD_W + x;
            const uint8_t *r = ix + j * rb;
            for (i = 0; i < w; i++)
                row[i] = pal[rd_pal_index(r, i, bpp)];
        }
        break;
    }
    case RFM_ENC_RLE: {
        uint32_t total = 0, npx = w * h, k = 0, c = 0, rr = 0;
        while (total < npx) {                           /* the runs first: exactly w * h pixels */
            uint32_t run;
            if (left - k < 3u)
                goto short_;
            run = (uint32_t)q[k] + 1u;
            if (total + run > npx) {
                *err = RFM_DRAW_E_RLE_OVERRUN;
                return 0;
            }
            total += run;
            k += 3u;
        }
        size = k;
        for (k = 0; k < size; k += 3u) {                /* then the pixels: across rows, left to right, top down */
            uint32_t run = (uint32_t)q[k] + 1u;
            uint16_t v = rd_px(q + k + 1u);
            while (run) {
                uint16_t *row = d->fb + (y + rr) * RD_W + x;
                uint32_t m = w - c < run ? w - c : run;
                for (i = 0; i < m; i++)
                    row[c + i] = v;
                run -= m;
                c += m;
                if (c == w) {
                    c = 0;
                    rr++;
                }
            }
        }
        break;
    }
    case RFM_ENC_COPY: {
        uint32_t sx, sy;
        size = 2;
        if (left < size)
            goto short_;
        sx = q[0];
        sy = q[1];
        if (sx + w > RD_W || sy + h > RD_H) {
            *err = RFM_DRAW_E_COPY_SRC;
            return 0;
        }
        rd_copy(d, x, y, w, h, sx, sy);
        break;
    }
    default:
        *err = RFM_DRAW_E_ENC;
        return 0;
    }
    rd_dirty(d, x, y, w, h);
    *err = RFM_DRAW_OK;
    return RFM_RECT_HDR + size;
short_:
    *err = RFM_DRAW_E_SHORT;
    return 0;
}

uint32_t rfm_draw_tiles(const rfm_draw_t *d, const uint8_t *body, uint32_t len, rfm_draw_res_t *r)
{
    uint32_t off = RFM_TILES_HDR, k;
    r->frame = r->flags = r->nrects = r->applied = r->err_off = 0;
    r->err = RFM_DRAW_OK;
    if (len < RFM_TILES_HDR) {
        r->err = RFM_DRAW_E_HDR;
        return r->err;
    }
    r->frame = (uint32_t)body[0] | (uint32_t)body[1] << 8;
    r->flags = body[2];
    r->nrects = body[3];
    for (k = 0; k < r->nrects; k++) {
        uint32_t e, sz = rfm_draw_rect(d, body + off, len - off, &e);
        if (!sz) {
            r->err = e;
            r->err_off = off;
            return r->err;
        }
        off += sz;
        r->applied++;
    }
    if (off != len) {
        r->err = RFM_DRAW_E_TRAILING;
        r->err_off = off;
    }
    return r->err;
}
