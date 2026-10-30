/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone screens' colour themes (design/standalone-mockup: Themes.dc.html's tokens, and the palette the screen
 * artboards are drawn in). A theme is the mockup's ten tokens, bg, border, panel, ink, a1..a4, shadow, on, and six
 * the screens need beside them: mid and soft (the in-between greys of the cartridge, the slots and the steps), a5 (the
 * octave's accent) and three deep shades under the big numbers (d1 under the voice number, d2 under the channel, d5
 * under the octave). The eight themes of the Themes artboard give only the ten: their six more are mixed from those
 * (SA_TH_DERIVE). "Pixel", the default, is the screen artboards' own palette: Sweetie-16's accents on solid black,
 * every token given. S1 uses Pixel; the SAVE menu that picks a theme is S5's. */
#pragma once
#include <stdint.h>
#include <stdlib.h>

enum { SA_TK_BG, SA_TK_BORDER, SA_TK_PANEL, SA_TK_INK, SA_TK_A1, SA_TK_A2, SA_TK_A3, SA_TK_A4, SA_TK_SHADOW, SA_TK_ON,
       SA_TK_MID, SA_TK_SOFT, SA_TK_A5, SA_TK_D1, SA_TK_D2, SA_TK_D5, SA_NTK };

typedef struct {
    const char *name;
    uint16_t c[SA_NTK];                  /* RGB565 */
} sa_theme_t;

#define SA_565(x) ((uint16_t)((((x) >> 19) & 0x1Fu) << 11 | (((x) >> 10) & 0x3Fu) << 5 | (((x) >> 3) & 0x1Fu)))
/* a + (b - a) * k / 8, channel by channel, on RGB888 */
#define SA_MIXC(a, b, k, s) ((((((a) >> (s)) & 0xFFu) * (8u - (k)) + (((b) >> (s)) & 0xFFu) * (k)) / 8u) << (s))
#define SA_MIX(a, b, k) (SA_MIXC(a, b, k, 16) | SA_MIXC(a, b, k, 8) | SA_MIXC(a, b, k, 0))
/* the ten tokens, then the six mixed: mid 3/8 of the way from panel to ink, soft 5/8, a5 = a2, the deep shades 5/8 of
 * the way from their accent (a1, a2, a2) to black */
#define SA_TH_DERIVE(n, bg, bo, pa, in, a1, a2, a3, a4, sh, on)                                                     \
    {n,                                                                                                            \
     {SA_565(bg), SA_565(bo), SA_565(pa), SA_565(in), SA_565(a1), SA_565(a2), SA_565(a3), SA_565(a4), SA_565(sh),   \
      SA_565(on), SA_565(SA_MIX(pa, in, 3u)), SA_565(SA_MIX(pa, in, 5u)), SA_565(a2), SA_565(SA_MIX(a1, 0u, 5u)),   \
      SA_565(SA_MIX(a2, 0u, 5u)), SA_565(SA_MIX(a2, 0u, 5u))}}
#define SA_TH_FULL(n, bg, bo, pa, in, a1, a2, a3, a4, sh, on, mi, so, a5, d1, d2, d5)                               \
    {n,                                                                                                            \
     {SA_565(bg), SA_565(bo), SA_565(pa), SA_565(in), SA_565(a1), SA_565(a2), SA_565(a3), SA_565(a4), SA_565(sh),   \
      SA_565(on), SA_565(mi), SA_565(so), SA_565(a5), SA_565(d1), SA_565(d2), SA_565(d5)}}

#define SA_NTHEMES 9u
static const sa_theme_t SA_THEMES[SA_NTHEMES] = {
    /* the screen artboards' palette (Main.dc.html P, Home-Notes, Preset, Channel, Octave) */
    SA_TH_FULL("Pixel", 0x000000u, 0x000000u, 0x333c57u, 0xf4f4f4u, 0xef7d57u, 0x73eff7u, 0xffcd75u, 0xa7f070u,
               0x5d275du, 0x000000u, 0x566c86u, 0x94b0c2u, 0x41a6f6u, 0xb13e53u, 0x257179u, 0x29366fu),
    /* Themes.dc.html, in its order */
    SA_TH_DERIVE("ChoralRoot", 0x000000u, 0x000000u, 0x1c1c1cu, 0xf4f1eau, 0xff8a1fu, 0x4d7cffu, 0xffd60au, 0x2fd27fu,
                 0x2a2a2au, 0x000000u),
    SA_TH_DERIVE("FIZZ", 0x000000u, 0x000000u, 0x26262du, 0xf1f0eau, 0xff6a2cu, 0x3f7dffu, 0xffd23fu, 0x1fd47eu,
                 0xf03cffu, 0x000000u),
    SA_TH_DERIVE("NES", 0x000000u, 0x000000u, 0x0000bcu, 0xf8f8f8u, 0xf83800u, 0x3cbcfcu, 0xf8b800u, 0x58d854u,
                 0x0000bcu, 0x000000u),
    SA_TH_DERIVE("C64", 0x000000u, 0x6c5eb5u, 0x352879u, 0xffffffu, 0x9ad284u, 0x70a4b2u, 0xb8c76fu, 0x9a6759u,
                 0x352879u, 0x000000u),
    SA_TH_DERIVE("C64 Impossible Mission", 0x000000u, 0x000000u, 0xa1683cu, 0xffffffu, 0xedf171u, 0x75cec8u,
                 0xc46c71u, 0xa9ff9fu, 0x8e3c97u, 0x000000u),
    SA_TH_DERIVE("SNES", 0x000000u, 0x9a97b5u, 0x4f43aeu, 0xe6e4f0u, 0xd8343cu, 0x4a6cd8u, 0xf4c20du, 0x2fa35cu,
                 0x4f43aeu, 0x000000u),
    SA_TH_DERIVE("Game Boy", 0x000000u, 0x306230u, 0x306230u, 0x9bbc0fu, 0x8bac0fu, 0x9bbc0fu, 0x8bac0fu, 0x306230u,
                 0x0f380fu, 0x000000u),
    SA_TH_DERIVE("SynthWave", 0x000000u, 0x000000u, 0x2e1a5eu, 0xffffffu, 0xff2e97u, 0x00e5ffu, 0xffe45eu, 0xb84dffu,
                 0xff2e97u, 0x000000u),
};
#define SA_THEME_DEFAULT 0u

/* The background (the theme menu's ALGORITHM; kept with the theme). 0 the themes' own black, then dark grounds every
 * theme's ink, accents and chips read on, then SA_BG_CLASSIC: the theme's classic screen colour (C64 blue, Game Boy
 * green, SynthWave purple, SNES grey...), offered only where the theme has one and its ink keeps a contrast of 4.5:1
 * (WCAG AA) on it; the choice is kept through the other themes (black where one has none). Over a background not
 * black, a theme's border drawn in black follows the background, and a panel colour too near it is lifted 2/8 of the
 * way to the ink (the chips stay chips). */
#define SA_NBG 7u
#define SA_BG_CLASSIC 6u
static const struct {
    const char *name;
    uint32_t rgb;
} SA_BGS[SA_NBG - 1u] = {{"BLACK", 0x000000u},  {"MIDNIGHT", 0x0a1026u}, {"CHARCOAL", 0x18181cu},
                         {"PLUM", 0x1e0c26u},   {"FOREST", 0x08190fu},   {"OXBLOOD", 0x220909u}};
static const uint32_t SA_BG_CLASSIC_RGB[SA_NTHEMES] = {
    0x1a1c2cu,                               /* Pixel: Sweetie-16's ground */
    0,                                       /* ChoralRoot: black is its own */
    0,                                       /* FIZZ */
    0x0000a8u,                               /* NES: the title screens' blue */
    0x352879u,                               /* C64: the blue of the boot screen */
    0,                                       /* C64 Impossible Mission: black */
    0x4a4760u,                               /* SNES: the console's grey, deepened for the light ink */
    0x0f380fu,                               /* Game Boy: the darkest green */
    0x241046u,                               /* SynthWave: the night sky's purple */
};

static uint32_t sa_888(uint16_t c)
{
    uint32_t r = c >> 11 & 31u, g = c >> 5 & 63u, b = c & 31u;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static float sa_lum(uint32_t rgb)            /* the relative luminance, sRGB's curve approximated by a square */
{
    float r = (float)(rgb >> 16 & 255u) / 255.0f, g = (float)(rgb >> 8 & 255u) / 255.0f, b = (float)(rgb & 255u) / 255.0f;
    return 0.2126f * r * r + 0.7152f * g * g + 0.0722f * b * b;
}

static float sa_contrast(uint32_t a, uint32_t b)
{
    float la = sa_lum(a) + 0.05f, lb = sa_lum(b) + 0.05f;
    return la > lb ? la / lb : lb / la;
}

static int sa_bg_ok(uint32_t theme, uint32_t bg)   /* the background offered with the theme */
{
    if (theme >= SA_NTHEMES || bg >= SA_NBG)
        return 0;
    if (bg != SA_BG_CLASSIC)
        return 1;
    return SA_BG_CLASSIC_RGB[theme] != 0u &&
           sa_contrast(SA_BG_CLASSIC_RGB[theme], sa_888(SA_THEMES[theme].c[SA_TK_INK])) >= 4.5f;
}

static uint32_t sa_bg_rgb(uint32_t theme, uint32_t bg)
{
    if (!sa_bg_ok(theme, bg))
        return 0;
    return bg == SA_BG_CLASSIC ? SA_BG_CLASSIC_RGB[theme] : SA_BGS[bg].rgb;
}

static const char *sa_bg_name(uint32_t theme, uint32_t bg)   /* (CLASSIC where the theme has none: black) */
{
    return bg == SA_BG_CLASSIC ? (sa_bg_ok(theme, bg) ? "CLASSIC" : "BLACK") : SA_BGS[bg % (SA_NBG - 1u)].name;
}

static uint32_t sa_bg_step(uint32_t theme, uint32_t bg, int32_t d)   /* ALGORITHM: the next one offered, round */
{
    int32_t b = (int32_t)(bg < SA_NBG ? bg : 0u), k;
    for (k = 0; k < (int32_t)SA_NBG; k++) {
        b = (b + (d > 0 ? 1 : -1) + (int32_t)SA_NBG) % (int32_t)SA_NBG;
        if (sa_bg_ok(theme, (uint32_t)b) && (d > 0 ? --d : ++d) == 0)
            break;
    }
    return (uint32_t)b;
}

static void sa_theme_build(uint32_t theme, uint32_t bg, uint16_t out[SA_NTK])   /* the theme over the background */
{
    const uint16_t *t = SA_THEMES[theme < SA_NTHEMES ? theme : 0u].c;
    uint32_t rgb = sa_bg_rgb(theme, bg), k, p, d;
    for (k = 0; k < SA_NTK; k++)
        out[k] = t[k];
    if (!rgb)
        return;
    out[SA_TK_BG] = SA_565(rgb);
    if (t[SA_TK_BORDER] == t[SA_TK_BG])
        out[SA_TK_BORDER] = out[SA_TK_BG];
    p = sa_888(t[SA_TK_PANEL]);
    d = (uint32_t)abs((int)(p >> 16 & 255u) - (int)(rgb >> 16 & 255u)) +
        (uint32_t)abs((int)(p >> 8 & 255u) - (int)(rgb >> 8 & 255u)) + (uint32_t)abs((int)(p & 255u) - (int)(rgb & 255u));
    if (d < 72u)
        out[SA_TK_PANEL] = SA_565(SA_MIX(p, sa_888(t[SA_TK_INK]), 2u));
}
