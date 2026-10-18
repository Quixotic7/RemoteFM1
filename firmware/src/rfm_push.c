/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The screen push: see rfm_push.h. */
#include "rfm_push.h"

#define RP_ALL ((uint16_t)((1u << RFM_TILES_X) - 1u))

void rfm_push_init(rfm_push_t *p, const uint16_t *fb, uint16_t *stage, uint32_t stage_px, const rfm_lcd_ops_t *lcd)
{
    uint32_t i;
    p->fb = fb;
    p->stage = stage;
    p->stage_px = stage_px;
    p->lcd = *lcd;
    for (i = 0; i < RFM_TILES_Y; i++)
        p->dirty[i] = p->plan[i] = 0;
    p->active = 0;
    p->band = 0;
    p->rx = p->ry = p->rw = p->rh = p->row = 0;
    p->rects = p->xfers = p->bytes = 0;
}

void rfm_push_mark(rfm_push_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t t0, t1, b0, b1, b;
    uint16_t m;
    if (!w || !h || x >= RFM_W || y >= RFM_H)
        return;
    if (x + w > RFM_W)
        w = RFM_W - x;
    if (y + h > RFM_H)
        h = RFM_H - y;
    t0 = x / RFM_TILE;
    t1 = (x + w - 1u) / RFM_TILE;
    b0 = y / RFM_TILE;
    b1 = (y + h - 1u) / RFM_TILE;
    m = (uint16_t)(((1u << (t1 + 1u)) - 1u) & ~((1u << t0) - 1u));
    for (b = b0; b <= b1; b++)
        p->dirty[b] |= m;
}

void rfm_push_dirty_cb(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    rfm_push_mark((rfm_push_t *)ctx, x, y, w, h);
}

void rfm_push_mark_all(rfm_push_t *p)
{
    uint32_t b;
    for (b = 0; b < RFM_TILES_Y; b++)
        p->dirty[b] = RP_ALL;
}

int rfm_push_pending(const rfm_push_t *p)
{
    uint32_t b;
    for (b = 0; b < RFM_TILES_Y; b++)
        if (p->dirty[b])
            return 1;
    return 0;
}

void rfm_push_start(rfm_push_t *p)
{
    uint32_t b;
    for (b = 0; b < RFM_TILES_Y; b++) {
        p->plan[b] |= p->dirty[b];                      /* (|=: an aborted plan's rest is in dirty too) */
        p->dirty[b] = 0;
    }
    p->active = 1;
    p->band = 0;
    p->rh = p->row = 0;
    p->rects = p->xfers = p->bytes = 0;
}

void rfm_push_abort(rfm_push_t *p)
{
    uint32_t b;
    for (b = 0; b < RFM_TILES_Y; b++)
        p->plan[b] = 0;
    p->active = 0;
    p->rh = p->row = 0;
    rfm_push_mark_all(p);
}

/* the plan's next rectangle into rx..rh (tiles taken out of the plan); 0: the plan is empty */
static int rp_next_rect(rfm_push_t *p)
{
    uint32_t b, t0, t1, b1;
    uint16_t m, run;
    for (b = p->band; b < RFM_TILES_Y && !p->plan[b]; b++)
        ;
    p->band = (uint8_t)b;
    if (b == RFM_TILES_Y)
        return 0;
    m = p->plan[b];
    for (t0 = 0; !(m & (1u << t0)); t0++)
        ;
    for (t1 = t0; t1 + 1u < RFM_TILES_X && (m & (1u << (t1 + 1u))); t1++)
        ;
    run = (uint16_t)(((1u << (t1 + 1u)) - 1u) & ~((1u << t0) - 1u));
    for (b1 = b; b1 + 1u < RFM_TILES_Y && (p->plan[b1 + 1u] & run) == run; b1++)
        ;
    for (; b <= b1; b++)
        p->plan[b] &= (uint16_t)~run;
    p->rx = t0 * RFM_TILE;
    p->rw = (t1 - t0 + 1u) * RFM_TILE;
    p->ry = (uint32_t)p->band * RFM_TILE;
    p->rh = (b1 - p->band + 1u) * RFM_TILE;
    p->row = 0;
    p->rects++;
    return 1;
}

int rfm_push_step(rfm_push_t *p)
{
    uint32_t rows, n;
    if (!p->active)
        return 0;
    if (!p->lcd.idle(p->lcd.ctx))
        return 1;                                       /* the DMA runs: come back */
    if (p->row == p->rh) {
        if (!rp_next_rect(p)) {
            p->active = 0;                              /* the last transfer is over */
            return 0;
        }
        p->lcd.window(p->lcd.ctx, p->rx, p->ry, p->rx + p->rw - 1u, p->ry + p->rh - 1u);
    }
    if (p->rw == RFM_W) {                               /* full rows: straight from the shadow */
        rows = RFM_PUSH_DMA_MAX / (2u * RFM_W);
        if (rows > p->rh - p->row)
            rows = p->rh - p->row;
        n = rows * RFM_W;
        p->lcd.data(p->lcd.ctx, p->fb + (p->ry + p->row) * RFM_W, 2u * n);
    } else {                                            /* through the staging buffer, whole rows */
        uint32_t r, i;
        uint16_t *d = p->stage;
        rows = p->stage_px / p->rw;
        if (rows > p->rh - p->row)
            rows = p->rh - p->row;
        for (r = 0; r < rows; r++) {
            const uint16_t *s = p->fb + (p->ry + p->row + r) * RFM_W + p->rx;
            for (i = 0; i < p->rw; i++)
                *d++ = s[i];
        }
        n = rows * p->rw;
        p->lcd.data(p->lcd.ctx, p->stage, 2u * n);
    }
    p->row += rows;
    p->xfers++;
    p->bytes += 2u * n;
    return 1;
}
