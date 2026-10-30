/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
#include "frames.h"

#include <stdio.h>
#include <string.h>

uint32_t fr_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

uint16_t fr_slot(uint32_t v)
{
    uint16_t s;
    uint8_t *p = (uint8_t *)&s;
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return s;
}

uint16_t fr_rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return fr_slot(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void fr_fill(uint16_t *f, uint16_t s)
{
    int i;
    for (i = 0; i < FR_N; i++)
        f[i] = s;
}

void fr_rect(uint16_t *f, int x, int y, int w, int h, uint16_t s)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (i >= 0 && i < FR_W && j >= 0 && j < FR_H)
                f[j * FR_W + i] = s;
}

void fr_noise(uint16_t *f, uint32_t *seed)
{
    int i;
    for (i = 0; i < FR_N; i++)
        f[i] = (uint16_t)fr_rand(seed);
}

void fr_gradient(uint16_t *f, uint32_t ph)
{
    int x, y;
    for (y = 0; y < FR_H; y++)
        for (x = 0; x < FR_W; x++)
            f[y * FR_W + x] = fr_rgb((uint32_t)(x + (int)ph) & 255u, (uint32_t)(y * 2 + (int)ph * 3) & 255u,
                                     (uint32_t)(x + y) & 255u);
}

/* a 6x10 pseudo font: each character's bits come from its code (a stand-in for Inter Tight); edges get two
 * anti-aliasing shades, so a text cell holds 4 colours as the real UI's do */
static uint32_t glyph_bits(int c, int row)
{
    uint32_t s = (uint32_t)c * 2654435761u + (uint32_t)row * 40503u + 7u;
    if (c == ' ')
        return 0;
    s = fr_rand(&s);
    return (s >> 5) & 0x3Fu;
}

static uint16_t mix(uint16_t fg, uint16_t bg, int k)   /* k / 4 of fg over bg, on the slots' RGB565 */
{
    const uint8_t *a = (const uint8_t *)&fg, *b = (const uint8_t *)&bg;
    uint32_t va = (uint32_t)a[0] << 8 | a[1], vb = (uint32_t)b[0] << 8 | b[1];
    uint32_t r = (((va >> 11) & 31u) * (uint32_t)k + ((vb >> 11) & 31u) * (4u - (uint32_t)k)) / 4u;
    uint32_t g = (((va >> 5) & 63u) * (uint32_t)k + ((vb >> 5) & 63u) * (4u - (uint32_t)k)) / 4u;
    uint32_t bl = ((va & 31u) * (uint32_t)k + (vb & 31u) * (4u - (uint32_t)k)) / 4u;
    return fr_slot(r << 11 | g << 5 | bl);
}

void fr_text(uint16_t *f, int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t m1 = mix(fg, bg, 1), m2 = mix(fg, bg, 3);
    for (; *s; s++, x += 7) {
        int row, col;
        for (row = 0; row < 10; row++) {
            uint32_t bits = glyph_bits(*s, row), nb = glyph_bits(*s, row + 1) | glyph_bits(*s, row - 1);
            for (col = 0; col < 6; col++) {
                int on = (bits >> col) & 1u, near = (nb >> col) & 1u;
                if (x + col >= FR_W || y + row >= FR_H)
                    continue;
                f[(y + row) * FR_W + x + col] = on ? fg : near ? (col & 1 ? m1 : m2) : bg;
            }
        }
    }
}

void fr_ui(uint16_t *f, uint32_t t)
{
    static const char *names[4] = {"STRUM", "SPREAD", "CHORD", "LEVEL"};
    static const char *items[6] = {"MAJ7 PAD", "FOLK STRUM", "DARK KEYS", "GLASS ARP", "WIDE CHOIR", "LOW BRASS"};
    const uint16_t bg = fr_rgb(16, 18, 24), panel = fr_rgb(36, 40, 52), white = fr_rgb(240, 240, 240);
    const uint16_t accent = fr_rgb(255, 140, 40), teal = fr_rgb(40, 200, 190), dim = fr_rgb(120, 124, 140);
    char buf[32];
    int i, sel = (int)(t / 10u) % 6;
    fr_fill(f, bg);
    fr_rect(f, 0, 0, 240, 28, accent);
    fr_text(f, 8, 9, "CHORALROOT", fr_rgb(20, 20, 20), accent);
    snprintf(buf, sizeof buf, "BPM %3u", 90u + (t / 25u) % 40u);
    fr_text(f, 170, 9, buf, fr_rgb(20, 20, 20), accent);
    for (i = 0; i < 4; i++) {
        int x = 6 + i * 58, v = (int)((t * (uint32_t)(i + 1) * 3u) % 128u);
        fr_rect(f, x, 36, 52, 52, panel);
        fr_text(f, x + 4, 42, names[i], dim, panel);
        snprintf(buf, sizeof buf, "%3d", v);
        fr_text(f, x + 14, 60, buf, white, panel);
        fr_rect(f, x + 4, 80, 44, 4, bg);
        fr_rect(f, x + 4, 80, v * 44 / 127, 4, teal);
    }
    for (i = 0; i < 6; i++) {
        int y = 98 + i * 20;
        uint16_t b = i == sel ? teal : bg, fg = i == sel ? fr_rgb(10, 10, 10) : white;
        fr_rect(f, 6, y, 228, 18, b);
        fr_text(f, 12, y + 4, items[i], fg, b);
    }
    fr_rect(f, 6, 222, 228, 12, panel);
    fr_rect(f, 6, 222, (int)(t * 7u % 228u), 12, accent);
}

int fr_load_ppm(const char *path, uint16_t *f)
{
    FILE *fp = fopen(path, "rb");
    int w, h, mx, i;
    if (!fp)
        return -1;
    if (fscanf(fp, "P6 %d %d %d", &w, &h, &mx) != 3 || w != FR_W || h != FR_H || mx != 255 || fgetc(fp) == EOF) {
        fclose(fp);
        return -1;
    }
    for (i = 0; i < FR_N; i++) {
        unsigned char c[3];
        if (fread(c, 1, 3, fp) != 3) {
            fclose(fp);
            return -1;
        }
        /* the screenshots were expanded from RGB565 as v * 255 / max: back with rounding */
        f[i] = fr_slot(((uint32_t)(c[0] * 31 + 127) / 255u) << 11 | ((uint32_t)(c[1] * 63 + 127) / 255u) << 5 |
                       (uint32_t)(c[2] * 31 + 127) / 255u);
    }
    fclose(fp);
    return 0;
}
