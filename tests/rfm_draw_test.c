/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Host test of the TILES decoder (firmware/src/rfm_draw.c), with and without the shadow, and the screen push
 * (firmware/src/rfm_push.c), the code
 * the device runs. Built with -fsanitize=address,undefined (tests/run_tests.sh); the shadow and every body are
 * heap blocks of exactly their size, so a write or read past either end stops the test.
 *   - every encoding against an independent reference decoder (written from docs/PROTOCOL.md, COPY through a
 *     snapshot), on the shadow's bytes (big endian on the wire, in memory);
 *   - PAL at n = 2, 3, 4, 5, 16 with every width 1..33 and garbage in the row padding bits;
 *   - RLE runs across rows, of 1 and 256 pixels; COPY overlapping in all 8 directions and in place;
 *   - every malformed case: the error code, nothing drawn for it, the rectangles before it drawn;
 *   - 20,000 random messages, valid and mutated (cut short, bytes flipped), against the reference;
 *   - without the shadow (rows to the panel as decoded): 5,000 random messages against the shadow's decoding;
 *   - the push: a model panel whose DMA reads its source a little at a time while new TILES keep landing in the
 *     shadow, ENDs while a push runs, an abort: when the pushes run out the panel equals the shadow.
 *   cc -std=c99 -fsanitize=address,undefined -o rfm_draw_test tests/rfm_draw_test.c && ./rfm_draw_test */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/src/rfm_draw.c"
#include "../firmware/src/rfm_push.c"

#define W 240u
#define H 240u
#define NPX (W * H)

static int fails, checks;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            fails++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            if (fails > 30)                                                      \
                exit(1);                                                         \
        }                                                                        \
    } while (0)

static uint32_t rs = 12345u;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0u; }

/* ------------------------------------------------ the shadow as bytes --- */
static uint16_t *fb, *ref;                      /* the decoder's shadow, the reference's */
static uint32_t px_be(const uint16_t *b, uint32_t x, uint32_t y)   /* RGB565 from the bytes in memory */
{
    const uint8_t *p = (const uint8_t *)(b + y * W + x);
    return (uint32_t)p[0] << 8 | p[1];
}
static void ref_set(uint32_t x, uint32_t y, uint32_t c)             /* the reference writes bytes, big endian */
{
    uint8_t *p = (uint8_t *)(ref + y * W + x);
    p[0] = (uint8_t)(c >> 8);
    p[1] = (uint8_t)c;
}

/* ---------------------------------- the reference decoder (spec only) --- */
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* returns the rectangle's size, or 0 and *err */
static uint32_t ref_rect(const uint8_t *p, uint32_t n, uint32_t *err)
{
    uint32_t x, y, w, h, enc, i, j;
    if (n < 5) {
        *err = RFM_DRAW_E_RECT;
        return 0;
    }
    x = p[0], y = p[1], w = p[2], h = p[3], enc = p[4];
    p += 5;
    n -= 5;
    if (w < 1 || h < 1 || x + w > W || y + h > H) {
        *err = RFM_DRAW_E_BOUNDS;
        return 0;
    }
    if (enc == 0) {
        if (n < w * h * 2) goto sh;
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                ref_set(x + i, y + j, be16(p + 2 * (j * w + i)));
        *err = 0;
        return 5 + w * h * 2;
    }
    if (enc == 1) {
        if (n < 2) goto sh;
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                ref_set(x + i, y + j, be16(p));
        *err = 0;
        return 7;
    }
    if (enc == 2) {
        uint32_t np, bpp, rowb, need;
        if (n < 1) goto sh;
        np = p[0];
        if (np < 2 || np > 16) {
            *err = RFM_DRAW_E_PAL_N;
            return 0;
        }
        bpp = np <= 2 ? 1 : np <= 4 ? 2 : 4;
        rowb = (w * bpp + 7) / 8;
        need = 1 + 2 * np + h * rowb;
        if (n < need) goto sh;
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                uint32_t bit = i * bpp, byte = p[1 + 2 * np + j * rowb + bit / 8];
                uint32_t idx = (byte >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                if (idx >= np) {
                    *err = RFM_DRAW_E_PAL_INDEX;
                    return 0;
                }
            }
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                uint32_t bit = i * bpp, byte = p[1 + 2 * np + j * rowb + bit / 8];
                uint32_t idx = (byte >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                ref_set(x + i, y + j, be16(p + 1 + 2 * idx));
            }
        *err = 0;
        return 5 + need;
    }
    if (enc == 3) {
        uint32_t tot = 0, k = 0, q;
        while (tot < w * h) {
            if (k + 3 > n) goto sh;
            if (tot + p[k] + 1 > w * h) {
                *err = RFM_DRAW_E_RLE_OVERRUN;
                return 0;
            }
            tot += p[k] + 1u;
            k += 3;
        }
        tot = 0;
        for (q = 0; q < k; q += 3) {
            uint32_t r;
            for (r = 0; r <= p[q]; r++, tot++)
                ref_set(x + tot % w, y + tot / w, be16(p + q + 1));
        }
        *err = 0;
        return 5 + k;
    }
    if (enc == 4) {
        static uint16_t snap[NPX];
        uint32_t sx, sy;
        if (n < 2) goto sh;
        sx = p[0], sy = p[1];
        if (sx + w > W || sy + h > H) {
            *err = RFM_DRAW_E_COPY_SRC;
            return 0;
        }
        memcpy(snap, ref, sizeof snap);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                ref[(y + j) * W + x + i] = snap[(sy + j) * W + sx + i];
        *err = 0;
        return 7;
    }
    *err = RFM_DRAW_E_ENC;
    return 0;
sh:
    *err = RFM_DRAW_E_SHORT;
    return 0;
}

static uint32_t ref_tiles(const uint8_t *b, uint32_t len, uint32_t *applied)
{
    uint32_t off = 4, k, err = 0;
    *applied = 0;
    if (len < 4)
        return RFM_DRAW_E_HDR;
    for (k = 0; k < b[3]; k++) {
        uint32_t sz = ref_rect(b + off, len - off, &err);
        if (!sz)
            return err;
        off += sz;
        (*applied)++;
    }
    return off == len ? 0u : RFM_DRAW_E_TRAILING;
}

/* ------------------------------------------------------------ encoders --- */
typedef struct {
    uint8_t *b;
    uint32_t n, cap;
} buf_t;
static void put(buf_t *o, uint32_t v)
{
    if (o->n < o->cap)
        o->b[o->n] = (uint8_t)v;
    o->n++;
}
static void put16be(buf_t *o, uint32_t v)
{
    put(o, v >> 8);
    put(o, v);
}
static void tiles_hdr(buf_t *o, uint32_t frame, uint32_t flags, uint32_t nrects)
{
    o->n = 0;
    put(o, frame);
    put(o, frame >> 8);
    put(o, flags);
    put(o, nrects);
}
static void rect_hdr(buf_t *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t enc)
{
    put(o, x);
    put(o, y);
    put(o, w);
    put(o, h);
    put(o, enc);
}
static void enc_raw(buf_t *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i;
    rect_hdr(o, x, y, w, h, RFM_ENC_RAW);
    for (i = 0; i < w * h; i++)
        put16be(o, px[i]);
}
static void enc_fill(buf_t *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    rect_hdr(o, x, y, w, h, RFM_ENC_FILL);
    put16be(o, c);
}
/* indices idx[w*h] < np; the padding bits of every row get garbage (pad_ones) */
static void enc_pal(buf_t *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t np, const uint16_t *pal,
                    const uint8_t *idx, int pad_ones)
{
    uint32_t bpp = np <= 2 ? 1 : np <= 4 ? 2 : 4, rowb = (w * bpp + 7) / 8, i, j, k;
    rect_hdr(o, x, y, w, h, RFM_ENC_PAL);
    put(o, np);
    for (i = 0; i < np; i++)
        put16be(o, pal[i]);
    for (j = 0; j < h; j++)
        for (k = 0; k < rowb; k++) {
            uint32_t v = 0, bit;
            for (bit = 0; bit < 8; bit += bpp) {
                uint32_t pi = (k * 8 + bit) / bpp, d = pi < w ? idx[j * w + pi] : (pad_ones ? 0xFu : 0u);
                v |= (d & ((1u << bpp) - 1)) << (8 - bpp - bit);
            }
            put(o, v);
        }
}
static void enc_rle(buf_t *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i = 0, n = w * h;
    rect_hdr(o, x, y, w, h, RFM_ENC_RLE);
    while (i < n) {
        uint32_t r = 1;
        while (i + r < n && r < 256 && px[i + r] == px[i])
            r++;
        put(o, r - 1);
        put16be(o, px[i]);
        i += r;
    }
}
static void enc_copy(buf_t *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy)
{
    rect_hdr(o, x, y, w, h, RFM_ENC_COPY);
    put(o, sx);
    put(o, sy);
}

/* ------------------------------------------------------------ helpers --- */
static rfm_draw_t D;
static uint32_t dirty_n, dirty_last[4];
static void on_dirty(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)ctx;
    CHECK(w >= 1 && h >= 1 && x + w <= W && y + h <= H);
    dirty_n++;
    dirty_last[0] = x, dirty_last[1] = y, dirty_last[2] = w, dirty_last[3] = h;
}

static void both_random(void)
{
    uint32_t i;
    for (i = 0; i < NPX; i++)
        fb[i] = ref[i] = (uint16_t)rnd();
}
static int same(void) { return !memcmp(fb, ref, NPX * 2); }

/* run a body through both (as an exact-size heap copy) and compare */
static uint32_t run_both(const uint8_t *b, uint32_t len, uint32_t *err_out)
{
    uint8_t *c = malloc(len ? len : 1);
    rfm_draw_res_t r;
    uint32_t app, e;
    memcpy(c, b, len);
    e = ref_tiles(c, len, &app);
    rfm_draw_tiles(&D, c, len, &r);
    free(c);
    CHECK(r.err == e);
    CHECK(r.applied == app);
    CHECK(same());
    if (r.err != e || r.applied != app)
        fprintf(stderr, "  len %u: err %u (ref %u) applied %u (ref %u)\n", len, r.err, e, r.applied, app);
    if (err_out)
        *err_out = r.err;
    return r.applied;
}

static uint8_t mb[70000];
static buf_t M = {mb, 0, sizeof mb};

/* ------------------------------------------------------------ the tests --- */
static void test_fill_raw(void)
{
    uint16_t px[7 * 3];
    uint32_t i;
    both_random();
    tiles_hdr(&M, 0x1234, 1, 2);
    enc_fill(&M, 10, 20, 16, 16, 0xF800);
    for (i = 0; i < 21; i++)
        px[i] = (uint16_t)(i * 0x0841u + 0x1234u);
    enc_raw(&M, 233, 237, 7, 3, px);
    dirty_n = 0;
    run_both(mb, M.n, NULL);
    CHECK(dirty_n == 2 && dirty_last[0] == 233 && dirty_last[1] == 237 && dirty_last[2] == 7 && dirty_last[3] == 3);
    CHECK(px_be(fb, 10, 20) == 0xF800 && px_be(fb, 25, 35) == 0xF800);
    CHECK(((const uint8_t *)fb)[2 * (20 * W + 10)] == 0xF8);           /* big endian in memory */
    CHECK(px_be(fb, 239, 239) == px[20] && px_be(fb, 233, 237) == px[0]);
    {
        rfm_draw_res_t r;
        uint8_t b[4] = {0x34, 0x12, 1, 0};
        rfm_draw_tiles(&D, b, 4, &r);
        CHECK(r.err == 0 && r.frame == 0x1234 && r.flags == 1 && r.nrects == 0 && r.applied == 0);
    }
}

static void test_pal(void)
{
    static const uint32_t NS[] = {2, 3, 4, 5, 16};
    uint32_t k, w, h, i;
    uint16_t pal[16];
    uint8_t idx[33 * 5];
    for (k = 0; k < 5; k++)
        for (w = 1; w <= 33; w++)
            for (h = 1; h <= 5; h += 2) {
                uint32_t np = NS[k];
                both_random();
                for (i = 0; i < np; i++)
                    pal[i] = (uint16_t)rnd();
                for (i = 0; i < w * h; i++)
                    idx[i] = (uint8_t)rn(np);
                tiles_hdr(&M, w, 0, 1);
                enc_pal(&M, 240 - w, 3, w, h, np, pal, idx, (int)(w & 1));
                CHECK(M.n == 4 + 5 + 1 + 2 * np + h * rfm_pal_row_bytes(w, np));
                run_both(mb, M.n, NULL);
                for (i = 0; i < w * h; i++)
                    if (px_be(fb, 240 - w + i % w, 3 + i / w) != pal[idx[i]])
                        break;
                CHECK(i == w * h);
            }
    /* rfm1.h's helpers: 1 / 2 / 4 bits, rows rounded up to bytes */
    CHECK(rfm_pal_bpp(2) == 1 && rfm_pal_bpp(3) == 2 && rfm_pal_bpp(4) == 2 && rfm_pal_bpp(5) == 4 &&
          rfm_pal_bpp(16) == 4);
    CHECK(rfm_pal_row_bytes(9, 2) == 2 && rfm_pal_row_bytes(5, 3) == 2 && rfm_pal_row_bytes(3, 5) == 2);
}

static void test_rle(void)
{
    uint16_t px[5 * 3];
    uint32_t i;
    both_random();
    for (i = 0; i < 15; i++)                            /* runs 4, 3, 7, 1: across the row ends */
        px[i] = i < 4 ? 0x1111 : i < 7 ? 0x2222 : i < 14 ? 0x3333 : 0x4444;
    tiles_hdr(&M, 1, 0, 1);
    enc_rle(&M, 100, 200, 5, 3, px);
    CHECK(M.n == 4 + 5 + 4 * 3);
    run_both(mb, M.n, NULL);
    CHECK(px_be(fb, 104, 200) == 0x2222 && px_be(fb, 101, 201) == 0x2222 && px_be(fb, 102, 201) == 0x3333 &&
          px_be(fb, 103, 202) == 0x3333 && px_be(fb, 104, 202) == 0x4444);
    {                                                   /* 256-pixel runs (len 255) over a 240 x 3 rect */
        static uint16_t big[240 * 3];
        for (i = 0; i < 720; i++)
            big[i] = i < 256 ? 0xAAAA : i < 512 ? 0x5555 : 0x0F0F;
        both_random();
        tiles_hdr(&M, 2, 0, 1);
        enc_rle(&M, 0, 237, 240, 3, big);
        CHECK(M.n == 4 + 5 + 3 * 3 && mb[9] == 255 && mb[12] == 255 && mb[15] == 207);
        run_both(mb, M.n, NULL);
        CHECK(px_be(fb, 15, 238) == 0xAAAA && px_be(fb, 16, 238) == 0x5555 && px_be(fb, 239, 239) == 0x0F0F);
    }
}

static void test_copy(void)
{
    int dx, dy;
    for (dy = -3; dy <= 3; dy += 3)
        for (dx = -3; dx <= 3; dx += 3) {
            both_random();
            tiles_hdr(&M, 3, 0, 1);
            enc_copy(&M, (uint32_t)(50 + dx), (uint32_t)(60 + dy), 20, 17, 50, 60);
            run_both(mb, M.n, NULL);
        }
    both_random();                                      /* the whole screen one pixel up and left, and edges */
    tiles_hdr(&M, 3, 0, 3);
    enc_copy(&M, 0, 0, 239, 239, 1, 1);
    enc_copy(&M, 1, 1, 239, 239, 0, 0);
    enc_copy(&M, 0, 0, 240, 240, 0, 0);
    run_both(mb, M.n, NULL);
}

static void test_malformed(void)
{
    struct {
        const char *what;
        uint8_t b[64];
        uint32_t n, err, applied;
    } C[] = {
        {"header 3 bytes", {0, 0, 1}, 3, RFM_DRAW_E_HDR, 0},
        {"empty", {0}, 0, RFM_DRAW_E_HDR, 0},
        {"rect header cut", {0, 0, 1, 1, 0, 0, 16}, 7, RFM_DRAW_E_RECT, 0},
        {"no rect at all", {0, 0, 1, 1}, 4, RFM_DRAW_E_RECT, 0},
        {"w 0", {0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0}, 11, RFM_DRAW_E_BOUNDS, 0},
        {"h 0", {0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0}, 11, RFM_DRAW_E_BOUNDS, 0},
        {"x + w 241", {0, 0, 1, 1, 230, 0, 11, 1, 1, 0, 0}, 11, RFM_DRAW_E_BOUNDS, 0},
        {"y + h 241", {0, 0, 1, 1, 0, 239, 1, 2, 1, 0, 0}, 11, RFM_DRAW_E_BOUNDS, 0},
        {"x 255 w 255", {0, 0, 1, 1, 255, 255, 255, 255, 1, 0, 0}, 11, RFM_DRAW_E_BOUNDS, 0},
        {"enc 5", {0, 0, 1, 1, 0, 0, 1, 1, 5, 0, 0}, 11, RFM_DRAW_E_ENC, 0},
        {"enc 0xFF", {0, 0, 1, 1, 0, 0, 1, 1, 0xFF, 0, 0}, 11, RFM_DRAW_E_ENC, 0},
        {"RAW short by 1", {0, 0, 1, 1, 0, 0, 2, 1, 0, 1, 2, 3}, 12, RFM_DRAW_E_SHORT, 0},
        {"FILL short", {0, 0, 1, 1, 0, 0, 2, 1, 1, 0xFF}, 10, RFM_DRAW_E_SHORT, 0},
        {"PAL no n", {0, 0, 1, 1, 0, 0, 2, 1, 2}, 9, RFM_DRAW_E_SHORT, 0},
        {"PAL n 0", {0, 0, 1, 1, 0, 0, 2, 1, 2, 0}, 10, RFM_DRAW_E_PAL_N, 0},
        {"PAL n 1", {0, 0, 1, 1, 0, 0, 2, 1, 2, 1, 0, 0, 0}, 13, RFM_DRAW_E_PAL_N, 0},
        {"PAL n 17", {0, 0, 1, 1, 0, 0, 2, 1, 2, 17}, 10, RFM_DRAW_E_PAL_N, 0},
        {"PAL palette short", {0, 0, 1, 1, 0, 0, 2, 1, 2, 2, 1, 2, 3}, 13, RFM_DRAW_E_SHORT, 0},
        {"PAL indices short", {0, 0, 1, 1, 0, 0, 9, 2, 2, 2, 1, 2, 3, 4, 0xFF, 0xFF, 0xFF}, 17, RFM_DRAW_E_SHORT, 0},
        {"PAL n 3 index 3", {0, 0, 1, 1, 0, 0, 3, 1, 2, 3, 1, 1, 2, 2, 3, 3, 0x1C}, 17, RFM_DRAW_E_PAL_INDEX, 0},
        {"PAL n 5 index 15", {0, 0, 1, 1, 0, 0, 2, 1, 2, 5, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 0x0F}, 21,
         RFM_DRAW_E_PAL_INDEX, 0},
        {"RLE no runs", {0, 0, 1, 1, 0, 0, 2, 2, 3}, 9, RFM_DRAW_E_SHORT, 0},
        {"RLE runs end early", {0, 0, 1, 1, 0, 0, 2, 2, 3, 2, 0xAB, 0xCD}, 12, RFM_DRAW_E_SHORT, 0},
        {"RLE run cut", {0, 0, 1, 1, 0, 0, 2, 2, 3, 1, 0xAB, 0xCD, 1, 0xAB}, 14, RFM_DRAW_E_SHORT, 0},
        {"RLE overrun", {0, 0, 1, 1, 0, 0, 2, 2, 3, 4, 0xAB, 0xCD}, 12, RFM_DRAW_E_RLE_OVERRUN, 0},
        {"RLE overrun 2nd run", {0, 0, 1, 1, 0, 0, 2, 2, 3, 1, 1, 1, 2, 2, 2}, 15, RFM_DRAW_E_RLE_OVERRUN, 0},
        {"COPY short", {0, 0, 1, 1, 0, 0, 2, 2, 4, 9}, 10, RFM_DRAW_E_SHORT, 0},
        {"COPY sx out", {0, 0, 1, 1, 0, 0, 2, 2, 4, 239, 0}, 11, RFM_DRAW_E_COPY_SRC, 0},
        {"COPY sy out", {0, 0, 1, 1, 0, 0, 2, 2, 4, 0, 255}, 11, RFM_DRAW_E_COPY_SRC, 0},
        {"trailing", {0, 0, 1, 1, 0, 0, 1, 1, 1, 0xAB, 0xCD, 0}, 12, RFM_DRAW_E_TRAILING, 1},
        {"2nd bad", {0, 0, 1, 2, 0, 0, 1, 1, 1, 0xAB, 0xCD, 0, 0, 1, 1, 7}, 16, RFM_DRAW_E_ENC, 1},
        {"nrects 3, 2 there", {0, 0, 1, 3, 0, 0, 1, 1, 1, 0xAB, 0xCD, 5, 5, 1, 1, 1, 1, 2}, 18, RFM_DRAW_E_RECT, 2},
    };
    uint32_t k;
    for (k = 0; k < sizeof C / sizeof C[0]; k++) {
        uint32_t e, a;
        static uint16_t before[NPX];
        both_random();
        memcpy(before, fb, sizeof before);
        a = run_both(C[k].b, C[k].n, &e);
        if (e != C[k].err || a != C[k].applied)
            fprintf(stderr, "  malformed \"%s\": err %u applied %u, want %u %u\n", C[k].what, e, a, C[k].err,
                    C[k].applied);
        CHECK(e == C[k].err && a == C[k].applied);
        if (!a)
            CHECK(!memcmp(before, fb, sizeof before));  /* nothing drawn */
    }
}

/* a random valid rectangle into o; px: scratch */
static void rand_rect(buf_t *o, int small)
{
    static uint16_t px[240 * 240];
    static uint8_t idx[240 * 240];
    uint32_t w = 1 + rn(small ? 40 : 240), h = 1 + rn(small ? 40 : 64), x, y, i, enc = rn(5);
    if (w * h > 4000)
        enc = enc == 0 ? 1 : enc;                       /* keep RAW messages modest */
    x = rn(W - w + 1);
    y = rn(H - h + 1);
    switch (enc) {
    case 0:
        for (i = 0; i < w * h; i++)
            px[i] = (uint16_t)rnd();
        enc_raw(o, x, y, w, h, px);
        break;
    case 1:
        enc_fill(o, x, y, w, h, rnd() & 0xFFFF);
        break;
    case 2: {
        uint32_t np = 2 + rn(15);
        uint16_t pal[16];
        for (i = 0; i < np; i++)
            pal[i] = (uint16_t)rnd();
        for (i = 0; i < w * h; i++)
            idx[i] = (uint8_t)rn(np);
        enc_pal(o, x, y, w, h, np, pal, idx, (int)(rnd() & 1));
        break;
    }
    case 3: {
        uint32_t c = rnd() & 0xFFFF;
        for (i = 0; i < w * h; i++) {
            if (!rn(9))
                c = rnd() & 0xFFFF;
            px[i] = (uint16_t)c;
        }
        enc_rle(o, x, y, w, h, px);
        break;
    }
    default:
        enc_copy(o, x, y, w, h, rn(W - w + 1), rn(H - h + 1));
        break;
    }
}

static void test_fuzz(void)
{
    uint32_t k, errs = 0;
    both_random();
    for (k = 0; k < 20000; k++) {
        uint32_t nr = rn(6), i, e;
        tiles_hdr(&M, k, rnd() & 1, nr);
        for (i = 0; i < nr; i++)
            rand_rect(&M, k & 1);
        if (M.n > M.cap)
            continue;
        if (k % 3 == 1)                                 /* cut short */
            M.n = rn(M.n + 1);
        else if (k % 3 == 2) {                          /* a few bytes changed (after the header, mostly) */
            uint32_t f = 1 + rn(3);
            while (f--)
                mb[rn(M.n)] = (uint8_t)rnd();
            if (!rn(8))
                M.n += rn(4);                           /* trailing garbage */
        }
        run_both(mb, M.n, &e);
        errs += e != 0;
        if (fails)
            break;
    }
    CHECK(errs > 3000 && errs < 17000);                 /* both kinds were exercised */
}

/* ------------------------------------------------------------ no shadow: rows --- */
/* the decoder without a shadow (fb NULL: RFM_SHADOW 0, the WiFi build's): the same random messages, valid and
 * mutated, drawn row by row into a second screen, against the shadow's decoding of them; COPY is refused there
 * (E_ENC), the rectangles before it drawn, nothing of it (checked against the shadow decoding the message cut there) */
static uint16_t *rowscr, *snap, rowbuf[W];
static void on_row(void *ctx, uint32_t x, uint32_t y, uint32_t w, const uint16_t *px)
{
    (void)ctx;
    CHECK(px == rowbuf && w >= 1 && x + w <= W && y < H);
    memcpy(rowscr + y * W + x, px, 2u * w);
}
static void test_rows(void)
{
    rfm_draw_t R = {NULL, NULL, NULL, on_row, rowbuf}, S = {NULL, NULL, NULL, NULL, NULL};
    uint32_t k, copies = 0, errs = 0;
    rowscr = malloc(NPX * 2);
    snap = malloc(NPX * 2);
    S.fb = fb;
    both_random();
    for (k = 0; k < 5000; k++) {
        uint32_t nr = rn(6), i;
        rfm_draw_res_t a, b;
        tiles_hdr(&M, k, rnd() & 1, nr);
        for (i = 0; i < nr; i++)
            rand_rect(&M, k & 1);
        if (M.n > M.cap)
            continue;
        if (k % 4 == 1)
            M.n = rn(M.n + 1);
        else if (k % 4 == 2)
            mb[rn(M.n)] = (uint8_t)rnd();
        memcpy(snap, fb, NPX * 2);
        memcpy(rowscr, fb, NPX * 2);
        rfm_draw_tiles(&R, mb, M.n, &b);
        if (b.err == RFM_DRAW_E_ENC && b.err_off + 4u < M.n && mb[b.err_off + 4u] == RFM_ENC_COPY) {
            copies++;
            mb[3] = (uint8_t)b.applied;                 /* the message cut before the COPY */
            rfm_draw_tiles(&S, mb, b.err_off, &a);
            CHECK(a.err == RFM_DRAW_OK && a.applied == b.applied);
        } else {
            rfm_draw_tiles(&S, mb, M.n, &a);
            CHECK(a.err == b.err && a.applied == b.applied && a.frame == b.frame);
            errs += b.err != 0;
        }
        CHECK(!memcmp(fb, rowscr, NPX * 2));
        if (fails)
            break;
    }
    CHECK(copies > 200 && errs > 200);
    {                                                   /* rfm_draw_clear without a shadow: every row */
        memset(rowscr, 0, NPX * 2);
        rfm_draw_clear(&R, 0x07E0u);
        CHECK(px_be(rowscr, 0, 0) == 0x07E0 && px_be(rowscr, 239, 239) == 0x07E0 && px_be(rowscr, 17, 133) == 0x07E0);
    }
    free(rowscr);
    free(snap);
}

/* ------------------------------------------------------------ the push --- */
static struct {
    uint16_t panel[NPX];
    uint32_t x0, y0, x1, y1, cx, cy;
    const uint8_t *src;
    uint32_t n, done, step;                             /* the running transfer: bytes, done; step: bytes a poll */
    uint32_t windows, xfers, bytes;
    int bad;
} L;

static void lcd_px(uint16_t v)                          /* one pixel into the window, wrapping as RAMWR does */
{
    if (L.cy <= L.y1 && L.cx <= L.x1)
        L.panel[L.cy * W + L.cx] = v;
    else
        L.bad = 1;
    if (++L.cx > L.x1) {
        L.cx = L.x0;
        L.cy++;
    }
}
static int lcd_idle(void *ctx)
{
    (void)ctx;
    if (L.done < L.n) {                                 /* the DMA moves on: reads the source as it is now */
        uint32_t k = L.step ? L.step : L.n;
        if (k > L.n - L.done)
            k = L.n - L.done;
        k &= ~1u;
        for (uint32_t i = 0; i < k; i += 2) {
            uint16_t v;
            memcpy(&v, L.src + L.done + i, 2);
            lcd_px(v);
        }
        L.done += k;
        return 0;
    }
    return 1;
}
static void lcd_window(void *ctx, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    (void)ctx;
    if (L.done != L.n || x0 > x1 || y0 > y1 || x1 >= W || y1 >= H)
        L.bad = 1;
    L.x0 = L.cx = x0, L.y0 = L.cy = y0, L.x1 = x1, L.y1 = y1;
    L.windows++;
}
static void lcd_data(void *ctx, const void *p, uint32_t n)
{
    (void)ctx;
    if (L.done != L.n || !n || (n & 1) || n > RFM_PUSH_DMA_MAX)
        L.bad = 1;
    L.src = p;
    L.n = n;
    L.done = 0;
    L.xfers++;
    L.bytes += n;
}

static rfm_push_t P;
static uint16_t *stage;

static void push_until_idle(void)
{
    uint32_t guard = 0;
    while (rfm_push_step(&P) && ++guard < 10000000u)
        ;
    CHECK(guard < 10000000u);
}

static void test_push_basic(void)
{
    static const rfm_lcd_ops_t OPS = {lcd_idle, lcd_window, lcd_data, NULL};
    uint32_t i;
    rfm_push_init(&P, fb, stage, 240 * 16, &OPS);
    D.dirty = rfm_push_dirty_cb;
    D.ctx = &P;
    for (i = 0; i < NPX; i++)
        fb[i] = (uint16_t)rnd();
    memset(L.panel, 0, sizeof L.panel);
    L.step = 0;
    rfm_push_mark_all(&P);                              /* a whole frame: 1 window, straight from the shadow */
    rfm_push_start(&P);
    L.windows = L.xfers = L.bytes = 0;
    push_until_idle();
    CHECK(!L.bad && !memcmp(L.panel, fb, sizeof L.panel));
    CHECK(L.windows == 1 && L.xfers == 4 && L.bytes == NPX * 2 && P.rects == 1);
    /* one tile, then two separate runs in a band, then an L shape: windows and staged sizes */
    tiles_hdr(&M, 1, 1, 1);
    enc_fill(&M, 17, 33, 3, 3, 0x1234);                 /* inside tile (1, 2) */
    rfm_draw_tiles(&D, mb, M.n, &(rfm_draw_res_t){0});
    rfm_push_start(&P);
    L.windows = L.xfers = L.bytes = 0;
    push_until_idle();
    CHECK(!L.bad && !memcmp(L.panel, fb, sizeof L.panel) && L.windows == 1 && L.bytes == 512);
    rfm_push_mark(&P, 0, 0, 32, 16);
    rfm_push_mark(&P, 200, 0, 40, 40);                  /* tiles 12..14 in bands 0..2 */
    rfm_push_mark(&P, 0, 16, 240, 1);                   /* band 1 full */
    for (i = 0; i < NPX; i++)
        fb[i] = (uint16_t)rnd();
    rfm_push_start(&P);
    L.windows = 0;
    push_until_idle();
    CHECK(!L.bad && L.windows == 3);                    /* tiles 0-1 b0-1, 12-14 b0-2, 2-11 b1 */
    {
        uint32_t wrong = 0, kept = 0;                   /* exactly the dirty tiles went to the panel */
        for (i = 0; i < NPX; i++) {
            uint32_t tx = i % W / 16, ty = i / W / 16;
            int d = (ty == 0 && tx < 2) || (tx >= 12 && ty <= 2) || ty == 1;
            if (d)
                wrong += L.panel[i] != fb[i];
            else
                kept += L.panel[i] != fb[i];
        }
        CHECK(wrong == 0 && kept > NPX / 2);
    }
}

/* new TILES land in the shadow while the push's DMA reads it; ENDs come while pushes run */
static void test_push_race(void)
{
    uint32_t f, k, want = 0, frames_pushed = 0, ends = 0;
    for (k = 0; k < NPX; k++)
        fb[k] = 0;
    memset(L.panel, 0, sizeof L.panel);
    rfm_push_abort(&P);
    for (k = 0; k < RFM_TILES_Y; k++)
        P.dirty[k] = 0;
    for (f = 0; f < 400; f++) {
        uint32_t msgs = 1 + rn(4), m;
        L.step = 2 + rn(3000);
        for (m = 0; m < msgs; m++) {
            uint32_t nr = 1 + rn(4), i, polls;
            tiles_hdr(&M, f, m + 1 == msgs ? 1 : 0, nr);
            for (i = 0; i < nr; i++)
                rand_rect(&M, (int)(rnd() & 1));
            if (M.n > M.cap)
                continue;
            {
                rfm_draw_res_t r;
                rfm_draw_tiles(&D, mb, M.n, &r);
                CHECK(r.err == 0);
            }
            if (mb[2] & 1) {                            /* END */
                ends++;
                want = 1;
            }
            polls = rn(40);
            for (i = 0; i < polls; i++) {               /* the main loop between messages */
                if (!rfm_push_step(&P) && want) {
                    want = 0;
                    frames_pushed++;
                    rfm_push_start(&P);
                }
            }
            if (f == 200 && m == 0) {                   /* the panel drawn on by someone else */
                for (i = 0; i < NPX; i += 7)
                    L.panel[i] ^= 0x5555;
                L.done = L.n;
                rfm_push_abort(&P);
                want = 1;
            }
        }
    }
    for (k = 0; k < 4; k++) {                           /* the last ENDs: push until nothing is left */
        push_until_idle();
        if (want || rfm_push_pending(&P)) {
            want = 0;
            rfm_push_start(&P);
        }
    }
    push_until_idle();
    CHECK(!rfm_push_pending(&P) && !P.active);
    CHECK(!L.bad);
    CHECK(!memcmp(L.panel, fb, sizeof L.panel));
    CHECK(frames_pushed > 50 && frames_pushed < ends);  /* some ENDs came while a push ran (they merged) */
    {
        uint32_t i, diff = 0;
        for (i = 0; i < NPX; i++)
            diff += L.panel[i] != fb[i];
        if (diff)
            fprintf(stderr, "  push race: %u pixels differ\n", diff);
    }
}

int main(void)
{
    uint32_t i;
    {
        uint16_t one = 1;                               /* the decoder's assumption */
        CHECK(*(uint8_t *)&one == 1);
    }
    fb = malloc(NPX * 2);
    ref = malloc(NPX * 2);
    stage = malloc(240 * 16 * 2);
    D.fb = fb;
    D.dirty = on_dirty;
    D.ctx = NULL;
    for (i = 0; i < NPX; i++)
        fb[i] = ref[i] = 0;
    test_fill_raw();
    test_pal();
    test_rle();
    test_copy();
    test_malformed();
    test_fuzz();
    test_rows();
    test_push_basic();
    test_push_race();
    {                                                   /* rfm_draw_clear: black, big endian */
        rfm_draw_t d = {fb, NULL, NULL, NULL, NULL};
        rfm_draw_clear(&d, 0xF81Fu);
        CHECK(px_be(fb, 0, 0) == 0xF81F && px_be(fb, 239, 239) == 0xF81F);
    }
    free(fb);
    free(ref);
    free(stage);
    printf("rfm_draw_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
