/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The screen encoder against the reference decoder (tiles_ref.c, written from the spec alone).
 *   test_screen                    the tests
 *   test_screen --report [a.ppm..] the compression report: bytes per frame and frames to converge per frame set, at
 *                                  the default bulk and SysEx budgets (20 Hz), plus real screens from PPM files */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "frames.h"
#include "rfm/codec.h"
#include "rfm/screen.h"
#include "rfm/tiles_ref.h"

typedef struct {
    uint8_t shadow[FR_N * 2];
    uint32_t body_max, bytes, msgs, max_msg;
    uint16_t frame;
    int end_seen;
} dec_t;

static void on_emit(void *ctx, const uint8_t *body, uint32_t n, int last)
{
    dec_t *d = (dec_t *)ctx;
    rfm_tiles_info_t info;
    int rc;
    CHECKF(n <= d->body_max, "body %u > %u", n, d->body_max);
    CHECK(!d->end_seen);
    rc = rfm_tiles_apply(d->shadow, body, n, 1, &info);
    CHECKF(rc == RFM_REF_OK, "decode error %d after %u rects", rc, info.applied);
    CHECK(info.frame == d->frame);
    CHECK(!!(info.flags & RFM_TILES_F_END) == !!last);
    CHECK(info.nrects >= 1u);
    if (last)
        d->end_seen = 1;
    d->bytes += n;
    d->msgs++;
    if (n > d->max_msg)
        d->max_msg = n;
}

static int same(const uint8_t *shadow, const uint16_t *img) { return !memcmp(shadow, img, FR_N * 2); }

/* one frame through the encoder and the reference decoder; the checks every frame must pass */
static rfm_enc_stats_t frame(rfm_screen_t *s, dec_t *d, uint32_t budget, uint32_t body_max)
{
    rfm_enc_stats_t st;
    uint32_t msgs;
    d->frame++;
    d->end_seen = 0;
    d->body_max = body_max < RFM_BODY_MIN ? RFM_BODY_MIN : body_max;
    msgs = rfm_screen_encode(s, d->frame, budget, body_max, on_emit, d, &st);
    CHECK(msgs == st.msgs);
    if (msgs)
        CHECK(d->end_seen);
    CHECKF(same(d->shadow, rfm_screen_model(s)), "shadow != model after frame %u", d->frame);
    CHECKF(st.rect_bytes <= budget || st.tiles == 1u, "%u rect bytes over budget %u with %u tiles", st.rect_bytes,
           budget, st.tiles);
    CHECK(st.body_bytes == st.rect_bytes + RFM_TILES_HDR * st.msgs);
    return st;
}

/* ---------------------------------------------------------------- frame sets ---- */
enum { SET_UI, SET_TEXT, SET_NOISE, SET_GRADIENT, SET_MIXED, NSETS };
static const char *set_names[NSETS] = {"ui", "ui-static", "noise", "gradient", "mixed"};

static void gen(int set, uint32_t t, uint16_t *f, uint32_t *seed)
{
    switch (set) {
    case SET_UI:
        fr_ui(f, t);
        break;
    case SET_TEXT:                                      /* the UI with only its value fields changing */
        fr_ui(f, 0);
        if (t) {
            char b[16];
            snprintf(b, sizeof b, "%3u", t % 128u);
            fr_text(f, 20, 60, b, fr_rgb(240, 240, 240), fr_rgb(36, 40, 52));
        }
        break;
    case SET_NOISE:
        fr_noise(f, seed);
        break;
    case SET_GRADIENT:
        fr_gradient(f, t * 3u);
        break;
    default: {
        uint32_t k = (t / 7u) % 4u, i;
        if (k == 0)
            fr_ui(f, t);
        else if (k == 1)
            fr_gradient(f, t);
        else if (k == 2)
            fr_noise(f, seed);
        else
            fr_fill(f, 0);
        for (i = 0; i < 5; i++)                         /* odd-sized patches across tile edges */
            fr_rect(f, (int)(fr_rand(seed) % 240u), (int)(fr_rand(seed) % 240u), (int)(fr_rand(seed) % 40u) + 1,
                    (int)(fr_rand(seed) % 40u) + 1, (uint16_t)fr_rand(seed));
        break;
    }
    }
}

/* ---------------------------------------------------------------- one rect ---- */
static uint32_t distinct(const uint16_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint16_t seen[17];
    uint32_t n = 0, i, j, k;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            uint16_t c = img[(y + j) * FR_W + x + i];
            for (k = 0; k < n && seen[k] != c; k++)
                ;
            if (k == n) {
                if (n == 17)
                    return 17;
                seen[n++] = c;
            }
        }
    return n;
}

static void test_rects(void)
{
    static uint16_t img[FR_N];
    static uint8_t shadow[FR_N * 2], before[FR_N * 2];
    static uint8_t body[RFM_BK_BODY_MAX];
    uint32_t seed = 12345, trial;
    for (trial = 0; trial < 4000; trial++) {
        uint32_t k = trial % 21u + 1u, i, x, y, w, h, nc, run = 0;
        uint16_t pal[24], c = 0;
        int enc;
        for (i = 0; i < k; i++)
            pal[i] = (uint16_t)fr_rand(&seed);
        for (i = 0; i < FR_N; i++) {                    /* runs of palette colours: RLE has something to do */
            if (!run) {
                c = pal[fr_rand(&seed) % k];
                run = trial & 1u ? fr_rand(&seed) % 300u + 1u : 1u;
            }
            img[i] = c;
            run--;
        }
        w = trial % 7u == 0 ? 240u : fr_rand(&seed) % 40u + 1u;
        h = trial % 11u == 0 ? 16u : fr_rand(&seed) % 24u + 1u;
        x = fr_rand(&seed) % (241u - w);
        y = fr_rand(&seed) % (241u - h);
        nc = distinct(img, x, y, w, h);
        for (enc = -1; enc <= RFM_ENC_RLE; enc++) {
            uint32_t n, j;
            rfm_tiles_info_t info;
            int rc, ok = 1;
            n = rfm_rect_encode(img, x, y, w, h, enc, body + 4, sizeof body - 4);
            if (enc == RFM_ENC_FILL && nc != 1u) {
                CHECK(n == 0);
                continue;
            }
            if (enc == RFM_ENC_PAL && nc > 16u) {
                CHECK(n == 0);
                continue;
            }
            CHECKF(n > 0, "enc %d of %ux%u (%u colours) not encoded", enc, w, h, nc);
            if (!n)
                continue;
            if (enc == RFM_ENC_PAL) {                   /* padding: n >= 2, row bytes as the spec says */
                uint32_t np = nc < 2u ? 2u : nc, bpp = np <= 2u ? 1u : np <= 4u ? 2u : 4u;
                CHECKF(n == 5u + 1u + 2u * np + h * ((w * bpp + 7u) / 8u), "PAL size %u for %ux%u n %u", n, w, h, np);
            }
            if (enc == RFM_ENC_RAW)
                CHECK(n == 5u + 2u * w * h);
            if (enc == -1) {
                int be;
                CHECK(n == rfm_rect_cost(img, x, y, w, h, &be));
                CHECK(body[4 + 4] == (uint8_t)be);
            }
            rfm_tiles_header(body, 7, RFM_TILES_F_END, 1);
            for (j = 0; j < sizeof shadow; j++)
                shadow[j] = (uint8_t)fr_rand(&seed);
            memcpy(before, shadow, sizeof shadow);
            rc = rfm_tiles_apply(shadow, body, n + 4u, 1, &info);
            CHECKF(rc == RFM_REF_OK, "enc %d %ux%u at %u,%u: decode error %d", enc, w, h, x, y, rc);
            for (j = 0; j < FR_H && ok; j++)
                for (i = 0; i < FR_W; i++) {
                    int in = i >= x && i < x + w && j >= y && j < y + h;
                    const uint8_t *want = in ? (const uint8_t *)&img[j * FR_W + i] : before + 2u * (j * FR_W + i);
                    if (memcmp(shadow + 2u * (j * FR_W + i), want, 2)) {
                        ok = 0;
                        break;
                    }
                }
            CHECKF(ok, "enc %d %ux%u at %u,%u (%u colours): pixels differ", enc, w, h, x, y, nc);
        }
    }
    /* long runs: a flat 240 x 8 strip as RLE is 8 runs of 256 */
    fr_fill(img, fr_slot(0x1234));
    CHECK(rfm_rect_encode(img, 0, 0, 240, 8, RFM_ENC_RLE, body, sizeof body) == 5u + 3u * 8u);
    CHECK(rfm_rect_encode(img, 0, 0, 240, 8, -1, body, sizeof body) == 7u);       /* FILL wins */
    CHECK(rfm_rect_encode(img, 0, 0, 240, 8, RFM_ENC_RAW, body, 100) == 0);       /* does not fit cap */
    CHECK(rfm_rect_encode(img, 230, 0, 11, 8, -1, body, sizeof body) == 0);       /* out of bounds */
}

/* ---------------------------------------------------------------- round trips ---- */
static void roundtrip(int set, uint32_t budget, uint32_t body_max, int merge, uint32_t anim_frames)
{
    static uint16_t f[FR_N];
    static dec_t d;
    rfm_screen_t *s = rfm_screen_new();
    uint32_t seed = 777u + (uint32_t)set, t, n;
    memset(&d, 0, sizeof d);
    rfm_screen_set_merge(s, merge);
    for (t = 0; t < anim_frames; t++) {
        gen(set, t, f, &seed);
        rfm_screen_submit(s, f);
        frame(s, &d, budget, body_max);
    }
    for (n = 0; n < 2000 && rfm_screen_pending(s); n++)
        frame(s, &d, budget, body_max);
    CHECKF(!rfm_screen_pending(s), "%s budget %u body %u: did not converge", set_names[set], budget, body_max);
    CHECKF(same(d.shadow, f), "%s budget %u body %u merge %d: shadow != frame", set_names[set], budget, body_max,
           merge);
    CHECK(!memcmp(rfm_screen_model(s), f, sizeof f));
    CHECK(rfm_screen_encode(s, 1, budget, body_max, on_emit, &d, NULL) == 0);     /* nothing dirty: nothing sent */
    rfm_screen_free(s);
}

static void test_roundtrips(void)
{
    static const uint32_t cfg[][2] = {{40000, 16384}, {14000, 1024}, {3000, 600}, {1500, 520}, {800, 64}, {0, 1024}};
    int set;
    uint32_t c;
    for (set = 0; set < NSETS; set++)
        for (c = 0; c < sizeof cfg / sizeof cfg[0]; c++)
            roundtrip(set, cfg[c][0], cfg[c][1], 1, cfg[c][0] < 1000u ? 8u : 30u);
    for (set = 0; set < NSETS; set++)
        roundtrip(set, 14000, 1024, 0, 20);
}

/* oldest first: under full-screen noise every frame, no tile waits longer than one round of the screen */
static void test_fairness(void)
{
    static uint16_t f[FR_N];
    static dec_t d;
    static uint32_t last_sent[RFM_NTILES];
    rfm_screen_t *s = rfm_screen_new();
    uint32_t seed = 99, t, max_wait = 0, min_tiles = 1000, k, gap = 0;
    memset(&d, 0, sizeof d);
    memset(last_sent, 0, sizeof last_sent);
    for (t = 1; t <= 300; t++) {
        rfm_enc_stats_t st;
        fr_noise(f, &seed);
        rfm_screen_submit(s, f);
        st = frame(s, &d, 3000, 1024);
        if (st.max_wait > max_wait)
            max_wait = st.max_wait;
        if (st.tiles < min_tiles)
            min_tiles = st.tiles;
        for (k = 0; k < RFM_NTILES; k++) {              /* every tile is dirty after a noise submit: the clean */
            if (!rfm_screen_tile_dirty(s, k))           /* ones were sent in this frame */
                last_sent[k] = t;
            if (t - last_sent[k] > gap)
                gap = t - last_sent[k];
        }
    }
    CHECK(min_tiles >= 1u);
    CHECKF(gap <= (RFM_NTILES + min_tiles - 1u) / min_tiles + 1u, "a tile went %u frames unsent (>= %u tiles/frame)",
           gap, min_tiles);
    CHECK(max_wait <= gap + 1u);
    printf("fairness: noise at 3000 B/frame: >= %u tiles per frame, the longest a tile waited %u frames\n", min_tiles,
           gap);
    /* a tile that keeps changing does not shut out the rest, even at one tile per frame (budget 0) */
    fr_ui(f, 0);
    rfm_screen_submit(s, f);
    for (t = 0; t < 600 && rfm_screen_pending(s) > 1u; t++) {
        fr_rect(f, 0, 0, 16, 16, (uint16_t)fr_rand(&seed));
        rfm_screen_submit(s, f);
        frame(s, &d, 0, 1024);
    }
    CHECKF(rfm_screen_pending(s) <= 1u, "the screen did not catch up beside a busy tile");
    rfm_screen_free(s);
}

/* invalidate: the device's shadow unknown (garbage) -> every tile resent; reset_black: the device cleared */
static void test_invalidate(void)
{
    static uint16_t f[FR_N];
    static dec_t d;
    rfm_screen_t *s = rfm_screen_new();
    uint32_t seed = 5, i, n;
    memset(&d, 0, sizeof d);
    fr_ui(f, 3);
    rfm_screen_submit(s, f);
    for (n = 0; n < 50 && rfm_screen_pending(s); n++)
        frame(s, &d, 14000, 1024);
    CHECK(same(d.shadow, f));
    for (i = 0; i < sizeof d.shadow; i++)               /* the unit lost its shadow: garbage */
        d.shadow[i] = (uint8_t)fr_rand(&seed);
    rfm_screen_invalidate(s);
    CHECK(rfm_screen_pending(s) == RFM_NTILES);
    for (n = 0; n < 50 && rfm_screen_pending(s); n++) {
        d.frame++;
        d.end_seen = 0;
        d.body_max = 1024;
        rfm_screen_encode(s, d.frame, 14000, 1024, on_emit, &d, NULL);
    }
    CHECK(same(d.shadow, f));
    memset(d.shadow, 0, sizeof d.shadow);              /* a new session: the unit is black */
    rfm_screen_reset_black(s);
    CHECK(rfm_screen_pending(s) > 0u);
    for (n = 0; n < 50 && rfm_screen_pending(s); n++)
        frame(s, &d, 14000, 1024);
    CHECK(same(d.shadow, f));
    /* an all-black target after reset_black: nothing to send */
    fr_fill(f, 0);
    rfm_screen_submit(s, f);
    for (n = 0; n < 50 && rfm_screen_pending(s); n++)
        frame(s, &d, 14000, 1024);
    rfm_screen_reset_black(s);
    CHECK(rfm_screen_pending(s) == 0u);
    rfm_screen_free(s);
}

/* merging: a flat screen is one FILL rect per tile row; on vs off give the same pixels, on never costs more */
static void test_merge(void)
{
    static uint16_t f[FR_N];
    static dec_t d1, d2;
    rfm_screen_t *a = rfm_screen_new(), *b = rfm_screen_new();
    rfm_enc_stats_t st;
    memset(&d1, 0, sizeof d1);
    memset(&d2, 0, sizeof d2);
    fr_fill(f, fr_rgb(10, 200, 30));
    rfm_screen_submit(a, f);
    st = frame(a, &d1, 40000, 16384);
    CHECKF(st.rects == RFM_TILES_Y && st.by_enc[RFM_ENC_FILL] == RFM_TILES_Y, "%u rects", st.rects);
    CHECK(st.body_bytes == RFM_TILES_HDR + RFM_TILES_Y * 7u);
    fr_ui(f, 1);
    rfm_screen_submit(a, f);
    rfm_screen_set_merge(b, 0);
    fr_fill(f, fr_rgb(10, 200, 30));
    rfm_screen_submit(b, f);
    frame(b, &d2, 40000, 16384);
    fr_ui(f, 1);
    rfm_screen_submit(b, f);
    {
        rfm_enc_stats_t sa = frame(a, &d1, 40000, 16384), sb = frame(b, &d2, 40000, 16384);
        CHECK(sa.tiles == sb.tiles);
        CHECKF(sa.rect_bytes <= sb.rect_bytes, "merged %u > unmerged %u", sa.rect_bytes, sb.rect_bytes);
        CHECK(sa.rects < sb.rects);
    }
    CHECK(same(d1.shadow, f) && same(d2.shadow, f));
    rfm_screen_free(a);
    rfm_screen_free(b);
}

/* ---------------------------------------------------------------- the report ---- */
typedef struct {
    const char *name;
    uint32_t budget, body_max;
} tcfg_t;
static const tcfg_t report_cfg[2] = {{"bulk", RFM_BUDGET_BK_BPS / 20u, RFM_BK_BODY_MAX},
                                     {"sysex", RFM_BUDGET_SX_BPS / 20u, RFM_SX_BODY_MAX}};

static uint32_t converge(rfm_screen_t *s, dec_t *d, const tcfg_t *c, uint32_t *bytes)
{
    uint32_t n = 0;
    *bytes = 0;
    while (rfm_screen_pending(s) && n < 1000) {
        rfm_enc_stats_t st = frame(s, d, c->budget, c->body_max);
        *bytes += st.body_bytes;
        n++;
    }
    return n;
}

static void report(int nppm, char **ppm)
{
    static uint16_t f[FR_N], g[FR_N];
    static dec_t d;
    int set, k, i;
    printf("\ncompression at 20 Hz: bulk budget %u B/frame (body <= %u), SysEx %u B/frame (body <= %u)\n",
           report_cfg[0].budget, report_cfg[0].body_max, report_cfg[1].budget, report_cfg[1].body_max);
    printf("  full B / frames: the first screen from black; anim: 100 frames, a new screen every frame (mean bytes,\n"
           "  tiles sent and tiles left pending per frame); catch-up: frames to converge once the animation stops\n");
    printf("  %-12s %-6s | %9s %7s | %10s %11s %8s | %9s\n", "frame set", "link", "full B", "frames", "anim B/fr",
           "anim tiles", "pending", "catch-up");
    for (set = 0; set < NSETS; set++)
        for (k = 0; k < 2; k++) {
            rfm_screen_t *s = rfm_screen_new();
            uint32_t seed = 1, bytes, full, n, t, sum = 0, tiles = 0, pend = 0, cu;
            memset(&d, 0, sizeof d);
            gen(set, 0, f, &seed);
            rfm_screen_submit(s, f);
            n = converge(s, &d, &report_cfg[k], &full);            /* the first full screen, from black */
            for (t = 1; t <= 100; t++) {                           /* then 100 frames of animation at 20 Hz */
                rfm_enc_stats_t st;
                gen(set, t, f, &seed);
                rfm_screen_submit(s, f);
                st = frame(s, &d, report_cfg[k].budget, report_cfg[k].body_max);
                sum += st.body_bytes;
                tiles += st.tiles;
                pend += st.pending;
            }
            cu = converge(s, &d, &report_cfg[k], &bytes);           /* the animation stops: frames to settle */
            CHECK(same(d.shadow, f));
            printf("  %-12s %-6s | %9u %7u | %10u %11.1f %8.1f | %9u\n", set_names[set], report_cfg[k].name,
                   full, n, sum / 100u, tiles / 100.0, pend / 100.0, cu);
            rfm_screen_free(s);
        }
    for (i = 0; i < nppm; i++) {
        if (fr_load_ppm(ppm[i], f)) {
            printf("  %s: cannot read\n", ppm[i]);
            continue;
        }
        for (k = 0; k < 2; k++) {
            rfm_screen_t *s = rfm_screen_new();
            uint32_t bytes, n, n2 = 0, bytes2 = 0;
            const char *base = strrchr(ppm[i], '/') ? strrchr(ppm[i], '/') + 1 : ppm[i];
            memset(&d, 0, sizeof d);
            rfm_screen_submit(s, f);
            n = converge(s, &d, &report_cfg[k], &bytes);
            CHECK(same(d.shadow, f));
            if (i + 1 < nppm && !fr_load_ppm(ppm[i + 1], g)) {   /* a screen change to the next screenshot */
                rfm_screen_submit(s, g);
                n2 = converge(s, &d, &report_cfg[k], &bytes2);
                CHECK(same(d.shadow, g));
            }
            printf("  real screen %-16s %-6s from black %6u B in %u frame(s); to the next screenshot %6u B in %u\n",
                   base, report_cfg[k].name, bytes, n, bytes2, n2);
            rfm_screen_free(s);
        }
    }
    (void)g;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--report")) {
        report(argc - 2, argv + 2);
        return check_done("test_screen --report");
    }
    test_rects();
    test_roundtrips();
    test_fairness();
    test_invalidate();
    test_merge();
    return check_done("test_screen");
}
