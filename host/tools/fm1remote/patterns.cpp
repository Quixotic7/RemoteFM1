// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
#include "patterns.h"

#include <cstring>

static const int W = 240, H = 240;

static uint16_t slot(uint32_t rgb565)                   // the slot holding the pixel's bytes big-endian
{
    uint16_t s;
    uint8_t *p = reinterpret_cast<uint8_t *>(&s);
    p[0] = (uint8_t)(rgb565 >> 8);
    p[1] = (uint8_t)rgb565;
    return s;
}
static uint16_t rgb(uint32_t r, uint32_t g, uint32_t b) { return slot((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

static void rect(uint16_t *f, int x, int y, int w, int h, uint16_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            f[j * W + i] = c;
}

static void digits(uint16_t *f, int x, int y, uint32_t v, uint16_t c)   // a 7-segment-ish counter
{
    static const uint8_t seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    for (int d = 4; d >= 0; d--, v /= 10) {
        const int ox = x + d * 14;
        const uint8_t s = seg[v % 10];
        if (s & 1) rect(f, ox + 2, y, 8, 2, c);
        if (s & 2) rect(f, ox + 10, y + 2, 2, 8, c);
        if (s & 4) rect(f, ox + 10, y + 12, 2, 8, c);
        if (s & 8) rect(f, ox + 2, y + 20, 8, 2, c);
        if (s & 16) rect(f, ox, y + 12, 2, 8, c);
        if (s & 32) rect(f, ox, y + 2, 2, 8, c);
        if (s & 64) rect(f, ox + 2, y + 10, 8, 2, c);
    }
}

const char *pattern_name(uint32_t t)
{
    static const char *n[4] = {"ui", "gradient", "boxes", "noise"};
    return n[(t / 4000u) % 4u];
}

void pattern_frame(uint16_t *f, uint32_t t, uint32_t frame)
{
    static uint32_t seed = 1;
    switch ((t / 4000u) % 4u) {
    case 0: {
        const uint16_t bg = rgb(16, 18, 24), panel = rgb(36, 40, 52), accent = rgb(255, 140, 40);
        rect(f, 0, 0, W, H, bg);
        rect(f, 0, 0, W, 28, accent);
        for (int i = 0; i < 4; i++) {
            rect(f, 6 + i * 58, 36, 52, 52, panel);
            rect(f, 10 + i * 58, 80, (int)((frame * (uint32_t)(i + 1) * 3u) % 44u), 4, rgb(40, 200, 190));
        }
        const int sel = (int)(frame / 10u) % 6;
        for (int i = 0; i < 6; i++)
            rect(f, 6, 98 + i * 20, 228, 18, i == sel ? rgb(40, 200, 190) : panel);
        digits(f, 80, 3, frame, rgb(20, 20, 20));
        break;
    }
    case 1:
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                f[y * W + x] = rgb((uint32_t)(x + (int)frame * 2) & 255u, (uint32_t)(y + (int)frame) & 255u,
                                   (uint32_t)(x ^ y) & 255u);
        break;
    case 2:
        rect(f, 0, 0, W, H, 0);
        for (int k = 0; k < 5; k++) {
            const int p = (int)frame * (3 + k), x = p % 400 < 200 ? p % 200 : 200 - p % 200;
            const int q = (int)frame * (2 + k), y = q % 400 < 200 ? q % 200 : 200 - q % 200;
            rect(f, x, y, 40, 40, rgb(60u * (uint32_t)k, 255u - 50u * (uint32_t)k, 128));
        }
        break;
    default:
        for (int i = 0; i < W * H; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            f[i] = (uint16_t)seed;
        }
        break;
    }
}

void pattern_leds(rfm_leds_t *l, uint32_t t)
{
    std::memset(l, 0, sizeof *l);
    const uint32_t step = t / 100u;
    for (uint32_t c = 0; c < RFM_NCOL; c++) {
        l->faint[c] = 0x1E;
        if (c == step % RFM_NCOL)
            l->lit[c] = 0x1E;
        if (c == (step + 1) % RFM_NCOL)
            l->mid[c] = 0x1E;
        if (c == (step + 2) % RFM_NCOL)
            l->dim[c] = 0x1E;
    }
    l->flags = (step / 5) & 1 ? RFM_LEDS_F_PLAY_GREEN : 0;
}
