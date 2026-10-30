/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's themes over every background (firmware/sa/sa_theme.h sa_theme_build): every theme on every
 * background it offers keeps its text readable, as the screens draw it (in RGB565, sa_lum's contrast):
 * ink on the ground 4.5:1; chip text (ON) on every accent and ink on the panel 4.5:1; on a light ground (the theme
 * turned) the accents 3:1 on the ground (the big numbers), SOFT 4.5:1, and ink on the deep shades 4.5:1. */
#include <stdio.h>
#include "../firmware/sa/sa_theme.h"

static int fails;
static void need(float c, float min, const char *what, uint32_t th, uint32_t bg)
{
    if (c + 0.005f < min) {
        printf("FAIL %s / %s: %s %.2f < %.1f\n", SA_THEMES[th].name, SA_BGS[bg].name, what, c, min);
        fails++;
    }
}

int main(void)
{
    uint32_t th, bg, k, n = 0;
    for (th = 0; th < SA_NTHEMES; th++)
        for (bg = 0; bg < SA_NBG; bg++) {
            uint16_t c[SA_NTK];
            uint32_t g;
            if (!sa_bg_ok(th, bg))
                continue;
            sa_theme_build(th, bg, c);
            g = sa_888(c[SA_TK_BG]);
            n++;
            need(sa_contrast(g, sa_888(c[SA_TK_INK])), 4.5f, "ink", th, bg);
            if (sa_lum(g) > 0.15f) {         /* a light ground */
                static const uint8_t A[5] = {SA_TK_A1, SA_TK_A2, SA_TK_A3, SA_TK_A4, SA_TK_A5};
                need(sa_contrast(sa_888(c[SA_TK_INK]), sa_888(c[SA_TK_PANEL])), 4.5f, "ink on panel", th, bg);
                for (k = 0; k < 5u; k++) {
                    need(sa_contrast(sa_888(c[SA_TK_ON]), sa_888(c[A[k]])), 4.5f, "chip text on accent", th, bg);
                    need(sa_contrast(g, sa_888(c[A[k]])), 3.0f, "accent on ground", th, bg);
                }
                need(sa_contrast(g, sa_888(c[SA_TK_SOFT])), 4.5f, "soft", th, bg);
                need(sa_contrast(g, sa_888(c[SA_TK_MID])), 3.0f, "mid", th, bg);
                need(sa_contrast(sa_888(c[SA_TK_INK]), sa_888(c[SA_TK_D1])), 4.5f, "ink on deep", th, bg);
            }
        }
    if (!sa_bg_ok(0, 7) || !sa_bg_ok(0, 8) || sa_bg_step(0, 6, 1) != 7u || sa_bg_step(0, 8, 1) != 0u ||
        sa_bg_step(1, 5, 1) != 7u)
        printf("FAIL the bright grounds' order\n"), fails++;
    if (fails)
        return 1;
    printf("sa_theme: %u theme / background pairs readable\n", n);
    return 0;
}
