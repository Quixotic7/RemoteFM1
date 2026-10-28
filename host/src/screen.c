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
    uint16_t model[RFM_NPIX];                           /* what the device shows */
    uint16_t basis[RFM_NPIX];                           /* the target each model tile was sent for (= model where
                                                         * the tile went exact): what "changed?" compares with */
    uint16_t target[RFM_NPIX];
    uint16_t send[RFM_NPIX];                            /* lossy: each pending tile as it would be sent */
    uint32_t age[RFM_NTILES];
    uint8_t dirty[RFM_NTILES], forced[RFM_NTILES];
    uint8_t refine[RFM_NTILES];                         /* queued for an exact resend (never while dirty) */
    uint8_t inexact[RFM_NTILES];                        /* the model holds a reduction of the basis */
    uint8_t prep[RFM_NTILES], prep_red[RFM_NTILES];     /* send[] holds the tile for this target; reduced */
    uint32_t seq, lossy;
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

/* ---------------------------------------------------------------- colour reduction ---- */
/* Median cut over the rect's distinct colours (weighted by pixel count, in 8-bit RGB with weights 2:4:3), a few
 * k-means rounds, then each cluster is represented by its member colour nearest the cluster's mean: the palette is
 * made of colours the rect already has (a background or a text colour that dominates its cluster stays exact).
 * Integer arithmetic only, ties broken by index: the same rect always gives the same result, on every platform. */
#define QMAX (T * T)
#define QW_R 2u
#define QW_G 4u
#define QW_B 3u

typedef struct {
    uint16_t slot, n;
    uint8_t c[3];                                       /* r, g, b, 8-bit */
} qcol_t;

static void slot_rgb(uint16_t slot, uint8_t *c)
{
    uint8_t m[2];
    uint32_t v, r, g, b;
    memcpy(m, &slot, 2);                                /* the slot's memory bytes = the BE pixel */
    v = (uint32_t)m[0] << 8 | m[1];
    r = v >> 11;
    g = (v >> 5) & 63u;
    b = v & 31u;
    c[0] = (uint8_t)(r << 3 | r >> 2);
    c[1] = (uint8_t)(g << 2 | g >> 4);
    c[2] = (uint8_t)(b << 3 | b >> 2);
}

static const uint32_t qw[3] = {QW_R, QW_G, QW_B};

static uint32_t qdist(const uint8_t *a, const uint32_t *m)  /* m: a mean, 8-bit x 16 */
{
    uint32_t k, d = 0;
    for (k = 0; k < 3; k++) {
        const int32_t e = (int32_t)a[k] * 16 - (int32_t)m[k];
        d += qw[k] * (uint32_t)(e * e >> 4);
    }
    return d;
}

typedef struct {
    uint32_t lo, hi;                                    /* uniq[lo, hi) */
    uint64_t sse;                                       /* weighted squared error x pixels */
} qbox_t;

/* the weighted SSE of uniq[lo, hi) times its pixel count, and the axis of the largest weighted variance */
static uint64_t box_sse(const qcol_t *u, uint32_t lo, uint32_t hi, uint32_t *axis)
{
    uint64_t sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0}, n = 0, tot = 0, best = 0;
    uint32_t i, k;
    for (i = lo; i < hi; i++) {
        n += u[i].n;
        for (k = 0; k < 3; k++) {
            sum[k] += (uint64_t)u[i].n * u[i].c[k];
            sq[k] += (uint64_t)u[i].n * u[i].c[k] * u[i].c[k];
        }
    }
    *axis = 0;
    for (k = 0; k < 3; k++) {
        const uint64_t v = qw[k] * (n * sq[k] - sum[k] * sum[k]);
        tot += v;
        if (v > best) {
            best = v;
            *axis = k;
        }
    }
    return n ? tot / n : 0;
}

static int qcol_less(const qcol_t *a, const qcol_t *b, uint32_t axis)
{
    if (a->c[axis] != b->c[axis])
        return a->c[axis] < b->c[axis];
    return a->slot < b->slot;
}

uint32_t rfm_tile_reduce(const uint16_t *src, uint32_t stride, uint32_t w, uint32_t h, uint16_t *dst,
                         uint32_t dst_stride, uint32_t ncolors)
{
    qcol_t u[QMAX], tmp;
    qbox_t box[RFM_PAL_MAX];
    uint32_t mean[RFM_PAL_MAX][3], pal_idx[RFM_PAL_MAX];
    uint16_t pal[RFM_PAL_MAX];
    uint8_t asg[QMAX];
    uint32_t nu = 0, nb, i, j, k, it, npal = 0;
    if (ncolors < 1u)
        ncolors = 1u;
    if (ncolors > RFM_PAL_MAX)
        ncolors = RFM_PAL_MAX;
    if (!w || !h || w * h > QMAX)
        return 0;
    /* the distinct colours, in first-seen order */
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            const uint16_t c = src[j * stride + i];
            for (k = 0; k < nu && u[k].slot != c; k++)
                ;
            if (k == nu) {
                u[nu].slot = c;
                u[nu].n = 0;
                slot_rgb(c, u[nu].c);
                nu++;
            }
            u[k].n++;
        }
    if (nu <= ncolors) {
        for (j = 0; j < h; j++)
            memmove(dst + j * dst_stride, src + j * stride, w * 2u);
        return nu;
    }
    /* median cut: split the box of the largest error along its widest axis at the pixel median */
    box[0].lo = 0;
    box[0].hi = nu;
    box[0].sse = box_sse(u, 0, nu, &k);
    for (nb = 1; nb < ncolors; nb++) {
        uint32_t b = nb, axis, lo, hi, half, cum, cut;
        for (i = 0; i < nb; i++)
            if (box[i].hi - box[i].lo >= 2u && box[i].sse && (b == nb || box[i].sse > box[b].sse))
                b = i;
        if (b == nb)
            break;
        lo = box[b].lo;
        hi = box[b].hi;
        box_sse(u, lo, hi, &axis);
        for (i = lo + 1u; i < hi; i++) {                /* insertion sort on the axis (slot breaks ties) */
            tmp = u[i];
            for (j = i; j > lo && qcol_less(&tmp, &u[j - 1u], axis); j--)
                u[j] = u[j - 1u];
            u[j] = tmp;
        }
        for (i = lo, half = 0; i < hi; i++)
            half += u[i].n;
        cum = u[lo].n;                                  /* [lo, cut) holds half the pixels; lo < cut < hi */
        for (cut = lo + 1u; cut < hi - 1u && cum * 2u < half; cut++)
            cum += u[cut].n;
        box[nb].lo = cut;
        box[nb].hi = hi;
        box[b].hi = cut;
        box[b].sse = box_sse(u, lo, cut, &axis);
        box[nb].sse = box_sse(u, cut, hi, &axis);
    }
    /* the boxes' means, then k-means rounds over the distinct colours */
    for (i = 0; i < nu; i++)
        for (k = 0; k < nb; k++)
            if (i >= box[k].lo && i < box[k].hi)
                asg[i] = (uint8_t)k;
    for (it = 0; it < 4u; it++) {
        uint64_t sum[RFM_PAL_MAX][3], cnt[RFM_PAL_MAX];
        int moved = 0;
        memset(sum, 0, sizeof sum);
        memset(cnt, 0, sizeof cnt);
        for (i = 0; i < nu; i++) {
            cnt[asg[i]] += u[i].n;
            for (k = 0; k < 3; k++)
                sum[asg[i]][k] += (uint64_t)u[i].n * u[i].c[k];
        }
        for (j = 0; j < nb; j++)
            if (cnt[j])
                for (k = 0; k < 3; k++)
                    mean[j][k] = (uint32_t)((sum[j][k] * 16u + cnt[j] / 2u) / cnt[j]);
        if (it == 3u)
            break;
        for (i = 0; i < nu; i++) {
            uint32_t best = asg[i], bd = qdist(u[i].c, mean[asg[i]]);
            for (j = 0; j < nb; j++) {
                const uint32_t d = qdist(u[i].c, mean[j]);
                if (cnt[j] && d < bd) {
                    bd = d;
                    best = j;
                }
            }
            if (best != asg[i]) {
                asg[i] = (uint8_t)best;
                moved = 1;
            }
        }
        if (!moved) {
            it = 2u;                                    /* one last mean pass, then stop */
            continue;
        }
    }
    /* each cluster's representative: its member nearest the mean (then the most pixels, then the lowest slot) */
    for (j = 0; j < nb; j++) {
        uint32_t best = nu, bd = 0;
        for (i = 0; i < nu; i++) {
            uint32_t d;
            if (asg[i] != j)
                continue;
            d = qdist(u[i].c, mean[j]);
            if (best == nu || d < bd || (d == bd && (u[i].n > u[best].n ||
                                                      (u[i].n == u[best].n && u[i].slot < u[best].slot)))) {
                best = i;
                bd = d;
            }
        }
        if (best < nu)
            pal_idx[npal++] = best;
    }
    for (j = 0; j < npal; j++)
        pal[j] = u[pal_idx[j]].slot;
    /* every distinct colour to its nearest representative (a representative to itself) */
    for (i = 0; i < nu; i++) {
        uint32_t best = 0, bd = 0;
        for (j = 0; j < npal; j++) {
            uint32_t m[3], d;
            for (k = 0; k < 3; k++)
                m[k] = (uint32_t)u[pal_idx[j]].c[k] * 16u;
            d = qdist(u[i].c, m);
            if (j == 0 || d < bd) {
                bd = d;
                best = j;
            }
        }
        asg[i] = (uint8_t)best;
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            const uint16_t c = src[j * stride + i];
            for (k = 0; u[k].slot != c; k++)
                ;
            dst[j * dst_stride + i] = pal[asg[k]];
        }
    return npal;
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

static int tile_differs(const uint16_t *a, const uint16_t *b, uint32_t t)
{
    uint32_t x = (t % TX) * T, y = (t / TX) * T, j;
    for (j = 0; j < T; j++)
        if (memcmp(a + (y + j) * RFM_W + x, b + (y + j) * RFM_W + x, T * 2u))
            return 1;
    return 0;
}

static void copy_tile(uint16_t *dst, const uint16_t *src, uint32_t t)
{
    uint32_t x = (t % TX) * T, y = (t / TX) * T, j;
    for (j = 0; j < T; j++)
        memcpy(dst + (y + j) * RFM_W + x, src + (y + j) * RFM_W + x, T * 2u);
}

/* a tile is dirty when its target is not what it was last sent for (the basis): a reduced tile is not dirty while
 * its target stays the same, however it differs from the model */
static void refresh(rfm_screen_t *s)
{
    uint32_t t;
    for (t = 0; t < RFM_NTILES; t++) {
        if (s->forced[t] || tile_differs(s->basis, s->target, t)) {
            if (!s->dirty[t]) {
                s->dirty[t] = 1;
                s->age[t] = s->seq;
            }
            s->refine[t] = 0;
        } else {
            s->dirty[t] = 0;
        }
    }
}

void rfm_screen_reset_black(rfm_screen_t *s)
{
    memset(s->model, 0, sizeof s->model);
    memset(s->basis, 0, sizeof s->basis);
    memset(s->forced, 0, sizeof s->forced);
    memset(s->inexact, 0, sizeof s->inexact);
    memset(s->refine, 0, sizeof s->refine);
    refresh(s);
}

void rfm_screen_set_lossy(rfm_screen_t *s, uint32_t colors)
{
    if (colors > RFM_PAL_MAX)
        colors = RFM_PAL_MAX;
    if (colors && colors < RFM_PAL_MIN)
        colors = RFM_PAL_MIN;
    if (colors != s->lossy)
        memset(s->prep, 0, sizeof s->prep);
    s->lossy = colors;
}

uint32_t rfm_screen_lossy(const rfm_screen_t *s) { return s->lossy; }

uint32_t rfm_screen_refine(rfm_screen_t *s, int on)
{
    uint32_t t, n = 0;
    for (t = 0; t < RFM_NTILES; t++) {
        if (!on)
            s->refine[t] = 0;
        else if (s->inexact[t] && !s->dirty[t] && !s->refine[t]) {
            s->refine[t] = 1;
            n++;
        }
    }
    return n;
}

uint32_t rfm_screen_refining(const rfm_screen_t *s)
{
    uint32_t t, n = 0;
    for (t = 0; t < RFM_NTILES; t++)
        n += s->refine[t];
    return n;
}

uint32_t rfm_screen_inexact(const rfm_screen_t *s)
{
    uint32_t t, n = 0;
    for (t = 0; t < RFM_NTILES; t++)
        n += s->inexact[t];
    return n;
}

void rfm_screen_invalidate(rfm_screen_t *s)
{
    memset(s->forced, 1, sizeof s->forced);
    refresh(s);
}

void rfm_screen_submit(rfm_screen_t *s, const uint16_t *lcd)
{
    uint32_t t;
    for (t = 0; t < RFM_NTILES; t++)                    /* a prepared (reduced) tile is kept while its target is */
        if (s->prep[t] && tile_differs(s->target, lcd, t))
            s->prep[t] = 0;
    memcpy(s->target, lcd, sizeof s->target);
    s->seq++;
    refresh(s);
}

int rfm_screen_tile_dirty(const rfm_screen_t *s, uint32_t t)
{
    return t < RFM_NTILES && (s->dirty[t] || s->refine[t]);
}

uint32_t rfm_screen_pending(const rfm_screen_t *s)
{
    uint32_t t, n = 0;
    for (t = 0; t < RFM_NTILES; t++)
        n += s->dirty[t] | s->refine[t];
    return n;
}

/* lossy: send[] gets tile t as it goes this time: its reduction to s->lossy colours when that encodes smaller
 * (cached while the target tile stays the same), else the target's pixels; returns 1 if reduced */
static int prepare(rfm_screen_t *s, uint32_t t)
{
    uint32_t x = (t % TX) * T, y = (t / TX) * T;
    an_t a;
    if (s->refine[t] && !s->dirty[t]) {                 /* a refinement: exact */
        copy_tile(s->send, s->target, t);
        s->prep[t] = 0;
        return 0;
    }
    if (s->prep[t])
        return s->prep_red[t];
    s->prep[t] = 1;
    s->prep_red[t] = 0;
    analyze(s->target, x, y, T, T, &a);
    if (a.ncol > s->lossy) {
        int e;
        const uint32_t exact = rfm_rect_cost(s->target, x, y, T, T, &e);
        uint16_t *d = s->send + y * RFM_W + x;
        rfm_tile_reduce(s->target + y * RFM_W + x, RFM_W, T, T, d, RFM_W, s->lossy);
        if (rfm_rect_cost(s->send, x, y, T, T, &e) < exact) {
            s->prep_red[t] = 1;
            return 1;
        }
    }
    copy_tile(s->send, s->target, t);
    return 0;
}

typedef struct {
    rfm_screen_t *s;
    const uint16_t *img;                                /* what is sent: the target, or send[] when lossy */
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
    analyze(p->img, r->x, r->y, r->w, r->h, &a);
    n = emit_rect(p->img, r->x, r->y, r->w, r->h, r->enc, &a, p->s->msg + p->len);
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
static uint32_t plan_tile(const uint16_t *img, uint32_t t, uint32_t max_rect, rect_t *out, uint32_t *nout)
{
    uint32_t x = (t % TX) * T, y = (t / TX) * T, c, hs, j, total = 0;
    int enc = RFM_ENC_RAW;
    c = rfm_rect_cost(img, x, y, T, T, &enc);
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
        c = rfm_rect_cost(img, x, y + j, T, h, &enc);
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
    uint8_t sel[RFM_NTILES], red[RFM_NTILES], fine[RFM_NTILES];
    const uint16_t *img = s->lossy ? s->send : s->target;
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
    if (!nd && !rfm_screen_refining(s))
        return 0;
    /* oldest first, raster order among equals (insertion sort: <= 225 entries); refinements after them all */
    for (k = 1; k < nd; k++) {
        uint16_t v = order[k];
        uint32_t j = k;
        while (j > 0 && cmp_age(&order[j - 1], &v, s) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    for (t = 0; t < RFM_NTILES; t++)
        if (s->refine[t] && !s->dirty[t])
            order[nd++] = (uint16_t)t;
    memset(sel, 0, sizeof sel);
    memset(red, 0, sizeof red);
    memset(fine, 0, sizeof fine);
    for (k = 0; k < nd; k++) {
        t = order[k];
        fine[t] = (uint8_t)(s->refine[t] && !s->dirty[t]);
        if (s->lossy)
            red[t] = (uint8_t)prepare(s, t);
        tcost[t] = plan_tile(img, t, max_rect, tile_rects[t], &ntr[t]);
        if (k == 0 || tcost[t] <= remaining) {
            sel[t] = 1;
            remaining = tcost[t] >= remaining ? 0u : remaining - tcost[t];
            st->tiles++;
            st->reduced += red[t];
            st->refined += fine[t];
            if (s->seq - s->age[t] > st->max_wait)
                st->max_wait = s->seq - s->age[t];
        }
        if (remaining < RFM_RECT_HDR + 2u)
            break;
    }

    pk.s = s;
    pk.img = img;
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
                    analyze(img, cur.x, cur.y, mw, T, &a);
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

    /* the model takes what was sent, the basis the target it was sent for */
    for (t = 0; t < RFM_NTILES; t++) {
        if (!sel[t])
            continue;
        copy_tile(s->model, img, t);
        copy_tile(s->basis, s->target, t);
        s->inexact[t] = red[t];
        s->dirty[t] = 0;
        s->forced[t] = 0;
        s->refine[t] = 0;
    }
    st->rect_bytes = rect_bytes;
    st->body_bytes = pk.body_bytes;
    st->msgs = pk.msgs;
    st->pending = rfm_screen_pending(s);
    return pk.msgs;
}
