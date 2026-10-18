/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The screen encoder: dirty 16x16 tiles, oldest first within a byte budget, the smallest of FILL / PAL / RLE / RAW
 * per rect, greedy horizontal merging, TILES bodies under the transport's limit. See rfm/screen.h. */
#include "rfm/screen.h"
#include "rfm/codec.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define T RFM_TILE
#define TX RFM_TILES_X
#define TY RFM_TILES_Y

typedef struct {                                        /* a planned rect */
    uint8_t x, y, w, h;
    int8_t enc;
    uint16_t cost;
} rect_t;

struct rfm_screen {
    uint16_t model[RFM_NPIX];
    uint16_t target[RFM_NPIX];
    uint32_t age[RFM_NTILES];
    uint8_t dirty[RFM_NTILES], forced[RFM_NTILES];
    uint32_t seq;
    int merge;
    rect_t tile_rects[RFM_NTILES][T];                   /* encode()'s per-tile plans (scratch) */
    uint8_t msg[RFM_BK_BODY_MAX];
};

/* ---------------------------------------------------------------- one rect ---- */
typedef struct {
    uint32_t ncol;                                      /* distinct colours, 17 = more than 16 */
    uint16_t pal[RFM_PAL_MAX];
    uint32_t runs;                                      /* RLE runs (each at most 256 pixels) */
} an_t;

static void analyze(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, an_t *a)
{
    uint32_t i, j, runlen = 0, last = 0, k;
    uint16_t prev = 0;
    a->ncol = 0;
    a->runs = 0;
    for (j = 0; j < h; j++) {
        const uint16_t *row = img + (y + j) * RFM_W + x;
        for (i = 0; i < w; i++) {
            const uint16_t c = row[i];
            if (a->ncol <= RFM_PAL_MAX && (a->ncol == 0 || a->pal[last] != c)) {
                for (k = 0; k < a->ncol && a->pal[k] != c; k++)
                    ;
                if (k == a->ncol) {
                    if (a->ncol < RFM_PAL_MAX)
                        a->pal[a->ncol] = c;
                    a->ncol++;
                }
                last = k < RFM_PAL_MAX ? k : 0;
            }
            if (!runlen || c != prev || runlen == 256u) {
                a->runs++;
                runlen = 1;
                prev = c;
            } else {
                runlen++;
            }
        }
    }
}

static uint32_t cost_of(const an_t *a, uint32_t w, uint32_t h, int enc)
{
    switch (enc) {
    case RFM_ENC_FILL:
        return a->ncol == 1u ? RFM_RECT_HDR + 2u : 0u;
    case RFM_ENC_PAL: {
        uint32_t n = a->ncol < RFM_PAL_MIN ? RFM_PAL_MIN : a->ncol;
        return n <= RFM_PAL_MAX ? RFM_RECT_HDR + 1u + 2u * n + h * rfm_pal_row_bytes(w, n) : 0u;
    }
    case RFM_ENC_RLE:
        return RFM_RECT_HDR + 3u * a->runs;
    case RFM_ENC_RAW:
        return RFM_RECT_HDR + 2u * w * h;
    default:
        return 0;
    }
}

static uint32_t best_of(const an_t *a, uint32_t w, uint32_t h, int *enc)
{
    static const int order[4] = {RFM_ENC_FILL, RFM_ENC_PAL, RFM_ENC_RLE, RFM_ENC_RAW};   /* ties: cheaper decode */
    uint32_t best = 0, k;
    for (k = 0; k < 4; k++) {
        uint32_t c = (k == 1 && a->ncol < RFM_PAL_MIN) ? 0u : cost_of(a, w, h, order[k]);
        if (c && (!best || c < best)) {
            best = c;
            *enc = order[k];
        }
    }
    return best;
}

static void px(uint8_t *out, const uint16_t *p) { memcpy(out, p, 2); }   /* the slot's memory bytes = BE pixel */

static uint32_t emit_rect(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int enc,
                          const an_t *a, uint8_t *out)
{
    uint32_t o = RFM_RECT_HDR, i, j;
    out[0] = (uint8_t)x;
    out[1] = (uint8_t)y;
    out[2] = (uint8_t)w;
    out[3] = (uint8_t)h;
    out[4] = (uint8_t)enc;
    switch (enc) {
    case RFM_ENC_RAW:
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++, o += 2)
                px(out + o, img + (y + j) * RFM_W + x + i);
        break;
    case RFM_ENC_FILL:
        px(out + o, &a->pal[0]);
        o += 2;
        break;
    case RFM_ENC_PAL: {
        uint32_t n = a->ncol < RFM_PAL_MIN ? RFM_PAL_MIN : a->ncol, bpp = rfm_pal_bpp(n);
        uint32_t rb = rfm_pal_row_bytes(w, n), k, last = 0;
        out[o++] = (uint8_t)n;
        for (k = 0; k < n; k++, o += 2) {
            if (k < a->ncol)
                px(out + o, &a->pal[k]);
            else
                out[o] = out[o + 1] = 0;               /* padding to the 2-colour minimum: never indexed */
        }
        for (j = 0; j < h; j++) {
            uint8_t *row = out + o;
            const uint16_t *src = img + (y + j) * RFM_W + x;
            memset(row, 0, rb);
            for (i = 0; i < w; i++) {
                uint32_t bit = i * bpp;
                if (a->pal[last] != src[i])
                    for (last = 0; last < a->ncol && a->pal[last] != src[i]; last++)
                        ;
                row[bit >> 3] |= (uint8_t)(last << (8u - bpp - (bit & 7u)));
            }
            o += rb;
        }
        break;
    }
    case RFM_ENC_RLE: {
        uint32_t runlen = 0;
        uint16_t prev = 0;
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                const uint16_t *c = img + (y + j) * RFM_W + x + i;
                if (runlen && *c == prev && runlen < 256u) {
                    runlen++;
                    out[o - 3] = (uint8_t)(runlen - 1u);
                } else {
                    runlen = 1;
                    prev = *c;
                    out[o] = 0;
                    px(out + o + 1, c);
                    o += 3;
                }
            }
        break;
    }
    default:
        return 0;
    }
    return o;
}

uint32_t rfm_rect_cost(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int *enc)
{
    an_t a;
    int e = RFM_ENC_RAW;
    uint32_t c;
    analyze(img, x, y, w, h, &a);
    c = best_of(&a, w, h, &e);
    if (enc)
        *enc = e;
    return c;
}

uint32_t rfm_rect_encode(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int enc, uint8_t *out,
                         uint32_t cap)
{
    an_t a;
    uint32_t c;
    if (!w || !h || x + w > RFM_W || y + h > RFM_H || w > 255u || h > 255u)
        return 0;
    analyze(img, x, y, w, h, &a);
    if (enc < 0)
        c = best_of(&a, w, h, &enc);
    else
        c = cost_of(&a, w, h, enc);
    if (!c || c > cap)
        return 0;
    return emit_rect(img, x, y, w, h, enc, &a, out);
}

/* ---------------------------------------------------------------- the screen ---- */
rfm_screen_t *rfm_screen_new(void)
{
    rfm_screen_t *s = (rfm_screen_t *)calloc(1, sizeof *s);
    if (s)
        s->merge = 1;
    return s;
}

void rfm_screen_free(rfm_screen_t *s) { free(s); }
void rfm_screen_set_merge(rfm_screen_t *s, int on) { s->merge = on; }
const uint16_t *rfm_screen_model(const rfm_screen_t *s) { return s->model; }
const uint16_t *rfm_screen_target(const rfm_screen_t *s) { return s->target; }

static int tile_differs(const rfm_screen_t *s, uint32_t t)
{
    uint32_t x = (t % TX) * T, y = (t / TX) * T, j;
    for (j = 0; j < T; j++)
        if (memcmp(s->model + (y + j) * RFM_W + x, s->target + (y + j) * RFM_W + x, T * 2u))
            return 1;
    return 0;
}

static void refresh(rfm_screen_t *s)
{
    uint32_t t;
    for (t = 0; t < RFM_NTILES; t++) {
        if (s->forced[t] || tile_differs(s, t)) {
            if (!s->dirty[t]) {
                s->dirty[t] = 1;
                s->age[t] = s->seq;
            }
        } else {
            s->dirty[t] = 0;
        }
    }
}

void rfm_screen_reset_black(rfm_screen_t *s)
{
    memset(s->model, 0, sizeof s->model);
    memset(s->forced, 0, sizeof s->forced);
    refresh(s);
}

void rfm_screen_invalidate(rfm_screen_t *s)
{
    memset(s->forced, 1, sizeof s->forced);
    refresh(s);
}

void rfm_screen_submit(rfm_screen_t *s, const uint16_t *lcd)
{
    memcpy(s->target, lcd, sizeof s->target);
    s->seq++;
    refresh(s);
}

int rfm_screen_tile_dirty(const rfm_screen_t *s, uint32_t t) { return t < RFM_NTILES && s->dirty[t]; }

uint32_t rfm_screen_pending(const rfm_screen_t *s)
{
    uint32_t t, n = 0;
    for (t = 0; t < RFM_NTILES; t++)
        n += s->dirty[t];
    return n;
}

typedef struct {
    rfm_screen_t *s;
    uint32_t body_max, len, nr, msgs, body_bytes;
    uint16_t frame;
    rfm_emit_fn emit;
    void *ctx;
} packer_t;

static void pk_flush(packer_t *p, int last)
{
    rfm_tiles_header(p->s->msg, p->frame, last ? RFM_TILES_F_END : 0u, (uint8_t)p->nr);
    p->emit(p->ctx, p->s->msg, p->len, last);
    p->msgs++;
    p->body_bytes += p->len;
    p->len = RFM_TILES_HDR;
    p->nr = 0;
}

static void pk_rect(packer_t *p, const rect_t *r)
{
    an_t a;
    uint32_t n;
    if (p->len + r->cost > p->body_max || p->nr == 255u)
        pk_flush(p, 0);
    analyze(p->s->target, r->x, r->y, r->w, r->h, &a);
    n = emit_rect(p->s->target, r->x, r->y, r->w, r->h, r->enc, &a, p->s->msg + p->len);
    assert(n == r->cost);                               /* the plan's size is the encoding's */
    (void)n;
    p->len += r->cost;
    p->nr++;
}

static int cmp_age(const void *pa, const void *pb, const rfm_screen_t *s)
{
    uint32_t a = *(const uint16_t *)pa, b = *(const uint16_t *)pb;
    if (s->age[a] != s->age[b])
        return (int32_t)(s->age[a] - s->age[b]) < 0 ? -1 : 1;
    return a < b ? -1 : a > b;
}

/* plans tile t alone: one rect, or (if its best encoding is too big for a message) strips of hs rows */
static uint32_t plan_tile(const rfm_screen_t *s, uint32_t t, uint32_t max_rect, rect_t *out, uint32_t *nout)
{
    uint32_t x = (t % TX) * T, y = (t / TX) * T, c, hs, j, total = 0;
    int enc = RFM_ENC_RAW;
    c = rfm_rect_cost(s->target, x, y, T, T, &enc);
    if (c <= max_rect) {
        out[0].x = (uint8_t)x;
        out[0].y = (uint8_t)y;
        out[0].w = T;
        out[0].h = T;
        out[0].enc = (int8_t)enc;
        out[0].cost = (uint16_t)c;
        *nout = 1;
        return c;
    }
    hs = (max_rect - RFM_RECT_HDR) / (2u * T);           /* RAW of hs rows always fits */
    if (hs < 1u)
        hs = 1u;
    *nout = 0;
    for (j = 0; j < T; j += hs) {
        uint32_t h = T - j < hs ? T - j : hs;
        rect_t *r = &out[(*nout)++];
        c = rfm_rect_cost(s->target, x, y + j, T, h, &enc);
        r->x = (uint8_t)x;
        r->y = (uint8_t)(y + j);
        r->w = T;
        r->h = (uint8_t)h;
        r->enc = (int8_t)enc;
        r->cost = (uint16_t)c;
        total += c;
    }
    return total;
}

uint32_t rfm_screen_encode(rfm_screen_t *s, uint16_t frame, uint32_t budget, uint32_t body_max, rfm_emit_fn emit,
                           void *ctx, rfm_enc_stats_t *st)
{
    rect_t(*tile_rects)[T] = s->tile_rects;
    uint16_t order[RFM_NTILES];
    uint32_t nd = 0, t, k, remaining = budget, max_rect, rect_bytes = 0;
    uint32_t ntr[RFM_NTILES], tcost[RFM_NTILES];
    uint8_t sel[RFM_NTILES];
    rfm_enc_stats_t dummy;
    packer_t pk;
    if (!st)
        st = &dummy;
    memset(st, 0, sizeof *st);
    if (body_max > RFM_BK_BODY_MAX)
        body_max = RFM_BK_BODY_MAX;
    if (body_max < RFM_BODY_MIN)
        body_max = RFM_BODY_MIN;
    max_rect = body_max - RFM_TILES_HDR;

    for (t = 0; t < RFM_NTILES; t++)
        if (s->dirty[t])
            order[nd++] = (uint16_t)t;
    if (!nd)
        return 0;
    /* oldest first, raster order among equals (insertion sort: <= 225 entries) */
    for (k = 1; k < nd; k++) {
        uint16_t v = order[k];
        uint32_t j = k;
        while (j > 0 && cmp_age(&order[j - 1], &v, s) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    memset(sel, 0, sizeof sel);
    for (k = 0; k < nd; k++) {
        t = order[k];
        tcost[t] = plan_tile(s, t, max_rect, tile_rects[t], &ntr[t]);
        if (k == 0 || tcost[t] <= remaining) {
            sel[t] = 1;
            remaining = tcost[t] >= remaining ? 0u : remaining - tcost[t];
            st->tiles++;
            if (s->seq - s->age[t] > st->max_wait)
                st->max_wait = s->seq - s->age[t];
        }
        if (remaining < RFM_RECT_HDR + 2u)
            break;
    }

    pk.s = s;
    pk.body_max = body_max;
    pk.len = RFM_TILES_HDR;
    pk.nr = 0;
    pk.msgs = 0;
    pk.body_bytes = 0;
    pk.frame = frame;
    pk.emit = emit;
    pk.ctx = ctx;

    /* tile rows: greedy merge of selected neighbours, then pack */
    for (k = 0; k < TY; k++) {
        rect_t cur;
        uint32_t tx, have = 0, curparts = 0;
        for (tx = 0; tx <= TX; tx++) {
            t = k * TX + tx;
            if (tx < TX && sel[t] && ntr[t] == 1u) {
                if (have && s->merge) {
                    an_t a;
                    int enc = RFM_ENC_RAW;
                    uint32_t mw = (uint32_t)cur.w + T, mc;
                    analyze(s->target, cur.x, cur.y, mw, T, &a);
                    mc = best_of(&a, mw, T, &enc);
                    if (mc <= max_rect && mc < (uint32_t)cur.cost + tcost[t]) {
                        cur.w = (uint8_t)mw;
                        cur.enc = (int8_t)enc;
                        cur.cost = (uint16_t)mc;
                        curparts++;
                        continue;
                    }
                }
                if (have) {
                    pk_rect(&pk, &cur);
                    st->rects++;
                    st->by_enc[cur.enc]++;
                    rect_bytes += cur.cost;
                }
                cur = tile_rects[t][0];
                have = 1;
                curparts = 1;
                continue;
            }
            if (have) {
                pk_rect(&pk, &cur);
                st->rects++;
                st->by_enc[cur.enc]++;
                rect_bytes += cur.cost;
                have = 0;
            }
            if (tx < TX && sel[t]) {                    /* a split tile: its strips as they are */
                uint32_t r;
                for (r = 0; r < ntr[t]; r++) {
                    pk_rect(&pk, &tile_rects[t][r]);
                    st->rects++;
                    st->by_enc[tile_rects[t][r].enc]++;
                    rect_bytes += tile_rects[t][r].cost;
                }
            }
        }
        (void)curparts;
    }
    pk_flush(&pk, 1);

    /* the model takes what was sent */
    for (t = 0; t < RFM_NTILES; t++) {
        uint32_t x, y, j;
        if (!sel[t])
            continue;
        x = (t % TX) * T;
        y = (t / TX) * T;
        for (j = 0; j < T; j++)
            memcpy(s->model + (y + j) * RFM_W + x, s->target + (y + j) * RFM_W + x, T * 2u);
        s->dirty[t] = 0;
        s->forced[t] = 0;
    }
    st->rect_bytes = rect_bytes;
    st->body_bytes = pk.body_bytes;
    st->msgs = pk.msgs;
    st->pending = rfm_screen_pending(s);
    return pk.msgs;
}
