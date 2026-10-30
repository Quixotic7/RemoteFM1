/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone screens' drawing: the pixel font (Jersey 10 at any whole scale: build/gen/sa_font.h), the mockup's
 * chips with stepped corners, pixel sprites, and the band renderer that puts them on the panel.
 *
 * No frame buffer: a screen is painted 16 rows at a time (a band) into one of two band buffers (fm1-base's canvas,
 * cv_px: 240 x 32 pixels, the two halves), each primitive clipped to the band, and the band goes to the panel only if
 * its pixels changed (a hash a band, kept): a still screen costs no transfer, a moving one only its moving bands. A
 * band's DMA runs while the next band is painted into the other half; nothing waits for the panel (lcd_idle: a band
 * painted while the last one is still going out waits, painted, for the next turn). sg_frame paints a frame a few
 * bands a call, from the caller's paint function, which must draw the same picture for every band of the frame.
 * Pixels are byte-swapped RGB565, as the panel takes them (gfx.c). */
#include "sa_font.h"                     /* build/gen: firmware/tools/gen_pixfont.py */

#define SG_W 240
#define SG_H 240
#define SG_BH 16                         /* rows a band */
#define SG_NB (SG_H / SG_BH)
_Static_assert(APP_CANVAS_PX >= 2u * SG_W * SG_BH, "two bands in fm1-base's canvas");

static uint16_t *sg_px;                  /* the band being painted */
static int32_t sg_y0;                    /* its first row */
static struct {
    uint32_t hash[SG_NB];
    uint32_t force;                      /* bands sent whatever their hash (bit b) */
    uint8_t band;                        /* the next band of the frame running; SG_NB: none runs */
    uint8_t pend;                        /* that band is painted, waiting for the panel */
    int8_t inflight;                     /* the half of the last band sent (its DMA may still read it), -1 none */
    uint32_t pend_hash;
    uint32_t bands_sent, frames;         /* (the simulator's checks) */
} sg = {{0}, 0xFFFFFFFFu, SG_NB, 0, -1, 0, 0, 0};

static inline uint16_t sg_sw(uint16_t c) { return (uint16_t)(c >> 8 | c << 8); }

/* ------------------------------------------------------------------ primitives (clipped to the band) --- */
static void sg_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h, i;
    uint16_t s = sg_sw(c);
    if (x < 0)
        x = 0;
    if (x1 > SG_W)
        x1 = SG_W;
    if (y < sg_y0)
        y = sg_y0;
    if (y1 > sg_y0 + SG_BH)
        y1 = sg_y0 + SG_BH;
    for (; y < y1; y++) {
        uint16_t *p = sg_px + (y - sg_y0) * SG_W;
        for (i = x; i < x1; i++)
            p[i] = s;
    }
}

static int sg_hits(int32_t y, int32_t h) { return y < sg_y0 + SG_BH && y + h > sg_y0; }

/* the mockup's chip: a box whose corners step in, 4 then 2 pixels over the first and last two rows each (its .px
 * clip-path at half size) */
static void sg_chip(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (!sg_hits(y, h))
        return;
    sg_rect(x + 4, y, w - 8, 2, c);
    sg_rect(x + 2, y + 2, w - 4, 2, c);
    sg_rect(x, y + 4, w, h - 8, c);
    sg_rect(x + 2, y + h - 4, w - 4, 2, c);
    sg_rect(x + 4, y + h - 2, w - 8, 2, c);
}

/* ------------------------------------------------------------------ the pixel font --- */
static uint32_t sg_gi(uint8_t ch)        /* a character's glyph: ASCII, the degree sign, o-slash; else '?' */
{
    if (ch >= 32u && ch < 127u)
        return ch - 32u;
    if (ch == 0xB0u)
        return 95u;
    if (ch == 0xF8u)
        return 96u;
    return '?' - 32u;
}

static int32_t sg_text_w(const char *s, int32_t n)   /* the advances (CSS's width) */
{
    int32_t w = 0;
    for (; *s; s++)
        w += SA_PF_G[sg_gi((uint8_t)*s)].adv;
    return w * n;
}

/* s at scale n (each grid pixel n x n), its capitals' top at y (the baseline at y + 10 n; descenders to y + 12 n);
 * returns the pen's end */
static int32_t sg_text(int32_t x, int32_t y, int32_t n, const char *s, uint16_t c)
{
    int32_t top = y - 2 * n;             /* the glyphs' row 0 */
    if (!sg_hits(top, (int32_t)SA_PF_ROWS * n))
        return x + sg_text_w(s, n);
    for (; *s; s++) {
        const sa_pf_glyph_t *g = &SA_PF_G[sg_gi((uint8_t)*s)];
        const uint16_t *b = SA_PF_BITS[sg_gi((uint8_t)*s)];
        int32_t gx = x + g->x0 * n, r;
        for (r = 0; r < (int32_t)SA_PF_ROWS; r++) {
            uint32_t bits = b[r], col = 0;
            int32_t ry = top + r * n;
            if (!bits || !sg_hits(ry, n))
                continue;
            while (bits) {                   /* runs of set bits: a rectangle each */
                uint32_t run = 0;
                while (!(bits & 1u)) {
                    bits >>= 1;
                    col++;
                }
                while (bits & 1u) {
                    bits >>= 1;
                    run++;
                }
                sg_rect(gx + (int32_t)col * n, ry, (int32_t)run * n, n, c);
                col += run;
            }
        }
        x += g->adv * n;
    }
    return x;
}

/* text with the mockup's text-shadow (sx, sy pixels down and right, in colour sh) */
static int32_t sg_text_sh(int32_t x, int32_t y, int32_t n, const char *s, uint16_t c, int32_t d, uint16_t sh)
{
    sg_text(x + d, y + d, n, s, sh);
    return sg_text(x, y, n, s, c);
}

/* text centred on the screen's width */
static void sg_text_c(int32_t y, int32_t n, const char *s, uint16_t c)
{
    sg_text((SG_W - sg_text_w(s, n)) / 2, y, n, s, c);
}

/* text in a chip: padding px left and right, the capitals centred in h; w 0: as wide as the text */
static int32_t sg_chip_text(int32_t x, int32_t y, int32_t w, int32_t h, int32_t px, const char *s, uint16_t bg,
                            uint16_t fg)
{
    int32_t tw = sg_text_w(s, 1);
    if (!w)
        w = tw + 2 * px;
    sg_chip(x, y, w, h, bg);
    sg_text(x + (w - tw) / 2, y + (h - (int32_t)SA_PF_CAP) / 2, 1, s, fg);
    return w;
}

/* ------------------------------------------------------------------ sprites --- */
/* a pixel map: rows of characters, '.' clear, any other a colour from col(ch) (0xFFFF0000u-free: < 0 skips), each
 * pixel s x s */
static void sg_sprite(int32_t x, int32_t y, int32_t s, const char *const *rows, uint32_t nrows,
                      int32_t (*col)(char ch))
{
    uint32_t r;
    if (!sg_hits(y, (int32_t)nrows * s))
        return;
    for (r = 0; r < nrows; r++) {
        int32_t i;
        if (!sg_hits(y + (int32_t)r * s, s))
            continue;
        for (i = 0; rows[r][i]; i++) {
            int32_t c;
            if (rows[r][i] == '.' || (c = col(rows[r][i])) < 0)
                continue;
            sg_rect(x + i * s, y + (int32_t)r * s, s, s, (uint16_t)c);
        }
    }
}

/* ------------------------------------------------------------------ the band renderer --- */
static uint32_t sg_band_hash(const uint16_t *p)   /* FNV-1a over the band's pixels, two at a time */
{
    const uint32_t *w = (const uint32_t *)(const void *)p;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < SG_W * SG_BH / 2u; i++)
        h = (h ^ w[i]) * 16777619u;
    return h | 1u;                       /* (never 0: a band never sent) */
}

static void sg_invalidate(void)          /* the panel was drawn on by someone else: every band again */
{
    sg.force = 0xFFFFFFFFu;
}

static int sg_busy(void) { return sg.band < SG_NB; }   /* a frame is under way */

static void sg_start(void)               /* a new frame (the caller's picture as it is now) */
{
    sg.band = 0;
    sg.pend = 0;
    sg.frames++;
}

static void sg_abort(void)               /* the frame dropped (the panel is someone else's now) */
{
    sg.band = SG_NB;
    sg.pend = 0;
}

/* up to max bands of the running frame: painted by paint(), sent if they changed; returns 1 while the frame runs */
static int sg_step(void (*paint)(void), uint32_t max, uint16_t bg)
{
    while (max-- && sg.band < SG_NB) {
        uint32_t half = sg.band & 1u, i;
        uint16_t *buf = cv_px + half * (uint32_t)(SG_W * SG_BH);
        if (!sg.pend) {
            if (sg.inflight == (int8_t)half && !lcd_idle())
                return 1;                /* that half is still going out */
            sg_px = buf;
            sg_y0 = (int32_t)sg.band * SG_BH;
            {
                uint16_t s = sg_sw(bg);
                for (i = 0; i < (uint32_t)(SG_W * SG_BH); i++)
                    buf[i] = s;
            }
            paint();
            sg.pend_hash = sg_band_hash(buf);
            if (sg.pend_hash == sg.hash[sg.band] && !(sg.force >> sg.band & 1u)) {
                sg.band++;
                continue;
            }
            sg.pend = 1;
        }
        if (!lcd_idle())
            return 1;                    /* the other half's DMA: next turn */
        lcd_blit(0, (uint32_t)sg.band * SG_BH, SG_W, SG_BH, buf);
        sg.inflight = (int8_t)half;
        sg.hash[sg.band] = sg.pend_hash;
        sg.force &= ~(1u << sg.band);
        sg.bands_sent++;
        sg.pend = 0;
        sg.band++;
    }
    return sg.band < SG_NB;
}
