/* SPDX-License-Identifier: MPL-2.0 */
/* The v2 screen's primitives: a line-for-line port of V2Screen.dc.html's (see v2.h). Rounding as JavaScript's
 * Math.round (floor(x + 0.5)), so every screen matches the mockup pixel for pixel. */
#include <string.h>

#include "v2.h"
#include "../../third_party/moveremote/common/font5x7.h"

static const uint16_t V2_ICONS[V2I_N][11] = {
    { 0x100, 0x180, 0x1c0, 0x1e0, 0x1f0, 0x1f8, 0x1f0, 0x1e0, 0x1c0, 0x180, 0x100 },   /* play */
    { 0x000, 0x0f8, 0x1fc, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x1fc, 0x0f8, 0x000 },   /* rec */
    { 0x000, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x3fe, 0x000 },   /* stop */
    { 0x000, 0x1f8, 0x204, 0x402, 0x407, 0x402, 0x400, 0x402, 0x204, 0x1f8, 0x000 },   /* loop */
    { 0x020, 0x052, 0x054, 0x088, 0x090, 0x124, 0x142, 0x201, 0x201, 0x7ff, 0x000 },   /* metro */
    { 0x007, 0x000, 0x01c, 0x000, 0x070, 0x000, 0x1c0, 0x000, 0x700, 0x000, 0x000 },   /* arp */
    { 0x104, 0x104, 0x102, 0x102, 0x102, 0x102, 0x70e, 0x70e, 0x000, 0x555, 0x000 },   /* groove */
    { 0x7ff, 0x555, 0x555, 0x555, 0x555, 0x555, 0x489, 0x489, 0x489, 0x489, 0x7ff },   /* keys */
    { 0x003, 0x003, 0x01b, 0x01b, 0x01b, 0x0db, 0x0db, 0x0db, 0x6db, 0x6db, 0x6db },   /* vel */
    { 0x070, 0x088, 0x124, 0x124, 0x134, 0x104, 0x088, 0x070, 0x000, 0x2aa, 0x000 },   /* tap */
    { 0x0f8, 0x104, 0x104, 0x104, 0x3fe, 0x3fe, 0x3de, 0x3de, 0x3fe, 0x3fe, 0x000 },   /* lock */
    { 0x020, 0x050, 0x088, 0x104, 0x202, 0x78f, 0x088, 0x088, 0x088, 0x0f8, 0x000 },   /* shift */
    { 0x040, 0x0c0, 0x751, 0x44a, 0x444, 0x44a, 0x751, 0x0c0, 0x040, 0x000, 0x000 },   /* mute */
    { 0x0f8, 0x104, 0x202, 0x202, 0x202, 0x707, 0x707, 0x707, 0x707, 0x000, 0x000 },   /* solo */
    { 0x7ff, 0x401, 0x5c1, 0x401, 0x439, 0x401, 0x4e1, 0x401, 0x40d, 0x401, 0x7ff },   /* clip */
    { 0x7f0, 0x418, 0x414, 0x41e, 0x582, 0x401, 0x5e1, 0x401, 0x5c1, 0x401, 0x7ff },   /* proj */
    { 0x020, 0x020, 0x124, 0x174, 0x575, 0x7fd, 0x575, 0x174, 0x124, 0x020, 0x020 },   /* wave */
    { 0x7ff, 0x401, 0x58d, 0x58d, 0x421, 0x471, 0x421, 0x58d, 0x58d, 0x401, 0x7ff },   /* dice */
    { 0x0f8, 0x124, 0x252, 0x489, 0x7ff, 0x489, 0x7ff, 0x489, 0x252, 0x124, 0x0f8 },   /* globe */
    { 0x000, 0x7ff, 0x000, 0x000, 0x7ff, 0x000, 0x000, 0x7ff, 0x000, 0x000, 0x000 },   /* track */
    { 0x030, 0x060, 0x0c0, 0x1f8, 0x030, 0x060, 0x0c0, 0x180, 0x000, 0x000, 0x000 },   /* bolt */
    { 0x555, 0x000, 0x4f9, 0x104, 0x575, 0x174, 0x575, 0x104, 0x4f9, 0x000, 0x555 },   /* capture */
    { 0x000, 0x780, 0x780, 0x780, 0x03c, 0x03c, 0x03c, 0x000, 0x000, 0x7ff, 0x000 },   /* step */
    { 0x000, 0x7fc, 0x404, 0x404, 0x404, 0x404, 0x404, 0x7fc, 0x000, 0x000, 0x000 },   /* replace */
    { 0x000, 0x7ff, 0x401, 0x5fd, 0x505, 0x5fd, 0x401, 0x7ff, 0x000, 0x000, 0x000 },   /* overdub */
    { 0x555, 0x000, 0x555, 0x000, 0x555, 0x000, 0x555, 0x000, 0x555, 0x000, 0x555 },   /* inkey */
    { 0x7ff, 0x555, 0x7ff, 0x555, 0x7ff, 0x555, 0x7ff, 0x555, 0x7ff, 0x555, 0x7ff },   /* chrom */
    { 0x78f, 0x78f, 0x78f, 0x7ff, 0x000, 0x78f, 0x78f, 0x78f, 0x000, 0x000, 0x000 },   /* drum */
    { 0x401, 0x401, 0x401, 0x7ff, 0x000, 0x401, 0x401, 0x401, 0x7ff, 0x000, 0x000 },   /* oct */
};


static int ifloor(double x)
{
    const int f = (int)x;
    return f > x ? f - 1 : f;
}
static int jround(double x) { return ifloor(x + 0.5); }   /* (JavaScript's Math.round) */

void v2_px(mr_canvas_t *b, int x, int y, int v)
{
    if (x >= 0 && x < 128 && y >= 0 && y < 64)
        b->px[y][x] = (uint8_t)(v != 0);
}
void v2_R(mr_canvas_t *b, int x, int y, int w, int h, int v)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            v2_px(b, i, j, v);
}
void v2_XR(mr_canvas_t *b, int x, int y, int w, int h)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (i >= 0 && i < 128 && j >= 0 && j < 64)
                b->px[j][i] ^= 1;
}
void v2_O(mr_canvas_t *b, int x, int y, int w, int h, int v)
{
    v2_R(b, x, y, w, 1, v), v2_R(b, x, y + h - 1, w, 1, v), v2_R(b, x, y, 1, h, v), v2_R(b, x + w - 1, y, 1, h, v);
}
void v2_dashO(mr_canvas_t *b, int x, int y, int w, int h)
{
    int i, j;
    for (i = 0; i < w; i++)
        if (i % 4 < 2)
            v2_px(b, x + i, y, 1), v2_px(b, x + i, y + h - 1, 1);
    for (j = 0; j < h; j++)
        if (j % 4 < 2)
            v2_px(b, x, y + j, 1), v2_px(b, x + w - 1, y + j, 1);
}
void v2_dotH(mr_canvas_t *b, int x, int y, int w)
{
    int i;
    for (i = 0; i < w; i++)
        if (i % 4 < 2)
            v2_px(b, x + i, y, 1);
}
void v2_dotV(mr_canvas_t *b, int x, int y, int h)
{
    int j;
    for (j = 0; j < h; j++)
        if (j % 4 < 2)
            v2_px(b, x, y + j, 1);
}
void v2_L(mr_canvas_t *b, int x0, int y0, int x1, int y1)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        int e2;
        v2_px(b, x0, y0, 1);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * e;
        if (e2 >= dy)
            e += dy, x0 += sx;
        if (e2 <= dx)
            e += dx, y0 += sy;
    }
}
void v2_X(mr_canvas_t *b, int x, int y, int w, int h)
{
    v2_L(b, x, y, x + w - 1, y + h - 1);
    v2_L(b, x + w - 1, y, x, y + h - 1);
}
int v2_tw(const char *s, int k)
{
    const int n = (int)strlen(s);
    return n ? n * 6 * k - k : 0;
}
int v2_T(mr_canvas_t *b, int x, int y, const char *s, int k, int v)
{
    for (; *s; s++) {
        const uint8_t *g = mr_glyph(*s);
        int i, j;
        for (i = 0; i < 5; i++)
            for (j = 0; j < 8; j++)
                if (g[i] >> j & 1)
                    v2_R(b, x + i * k, y + j * k, k, k, v);
        x += 6 * k;
    }
    return x;
}
int v2_TC(mr_canvas_t *b, double cx, int y, const char *s, int k, int v)
{
    return v2_T(b, jround(cx - v2_tw(s, k) / 2.0), y, s, k, v);
}
void v2_ic(mr_canvas_t *b, int icon, int x, int y, int v)
{
    int i, j;
    if (icon < 0 || icon >= V2I_N)
        return;
    for (j = 0; j < 11; j++)
        for (i = 0; i < 11; i++)
            if (V2_ICONS[icon][j] >> (10 - i) & 1)
                v2_px(b, x + i, y + j, v);
}
void v2_icSel(mr_canvas_t *b, int icon, int x, int y)
{
    v2_ic(b, icon, x, y, 1);
    v2_R(b, x, y + 13, 11, 2, 1);
}
void v2_ring(mr_canvas_t *b, int x, int y, int fill)
{
    static const char *const RG[7] = { "..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.." };
    int i, j;
    for (j = 0; j < 7; j++)
        for (i = 0; i < 7; i++)
            if (RG[j][i] == '#' || (fill && j > 0 && j < 6 && i > 0 && i < 6 && !((j == 1 || j == 5) && (i == 1 || i == 5))))
                v2_px(b, x + i, y + j, 1);
}

void v2_knobs(mr_canvas_t *b, int mask, int touched)
{
    int i;
    for (i = 0; i < 8; i++)
        if (mask >> i & 1)
            v2_ring(b, 5 + i * 16, 0, i == touched);
}
void v2_strip(mr_canvas_t *b, int n, uint32_t sel, double tick, int x0, int w)
{
    const int gap = n > 12 ? 1 : 2;
    const double seg = (double)(w - gap * (n - 1)) / n;
    int i;
    for (i = 0; i < n; i++) {
        const int sx = jround(x0 + i * (seg + gap)), ex = jround(x0 + i * (seg + gap) + seg), on = (int)(sel >> i & 1);
        v2_R(b, sx, on ? 61 : 63, ex - sx, on ? 3 : 1, 1);
    }
    if (tick >= 0)
        v2_R(b, jround(x0 + tick * w), 59, 1, 5, 1);
}
void v2_ghostStrip(mr_canvas_t *b, int real, int total)
{
    v2_R(b, 0, 63, jround(128.0 * real / total) - 2, 1, 1);
    v2_R(b, 122, 61, 5, 1, 1);
    v2_R(b, 124, 59, 1, 5, 1);
}
void v2_titleBox(mr_canvas_t *b, const char *title, const char *val, int cx, int w)
{
    v2_TC(b, cx, 3, title, 2, 1);
    if (v2_tw(val, 4) <= w)
        v2_TC(b, cx, 21, val, 4, 1);
    else
        v2_TC(b, cx, 27, val, 2, 1);
}
void v2_vbar(mr_canvas_t *b, double frac, int bip, int x, int w, int y)
{
    v2_R(b, x, y + 2, w, 1, 1);
    if (!bip)
        v2_R(b, x, y, jround(w * (frac < 0 ? 0 : frac > 1 ? 1 : frac)), 5, 1);
    else {
        const int m = x + jround(w / 2.0), d = jround(w / 2.0 * (frac < -1 ? -1 : frac > 1 ? 1 : frac));
        v2_R(b, d >= 0 ? m : m + d, y, d >= 0 ? d : -d, 5, 1);
        v2_R(b, m, y - 2, 1, 9, 1);
    }
}
void v2_msg(mr_canvas_t *b, const char *a, const char *c)
{
    if (c && *c)
        v2_TC(b, 64, 12, a, 2, 1), v2_TC(b, 64, 32, c, 2, 1);
    else
        v2_TC(b, 64, 22, a, 2, 1);
}
void v2_modeName(mr_canvas_t *b, const char *s, int n, int cur)
{
    const int k = v2_tw(s, 4) <= 124 ? 4 : 3;
    v2_TC(b, 64, k == 4 ? 14 : 18, s, k, 1);
    v2_strip(b, n, 1u << cur, -1, 0, 128);
}
void v2_lpop(mr_canvas_t *b, const char *label, const char *val)
{
    int k;
    v2_R(b, 0, 0, 64, 64, 0);
    if (v2_tw(label, 2) <= 62)
        v2_TC(b, 32, 8, label, 2, 1);
    else
        v2_TC(b, 32, 12, label, 1, 1);
    k = v2_tw(val, 4) <= 62 ? 4 : 2;
    v2_TC(b, 32, k == 4 ? 28 : 34, val, k, 1);
}
void v2_battery(mr_canvas_t *b, int x, int y, int pct, int chg)
{
    v2_O(b, x, y, 22, 12, 1);
    v2_R(b, x + 22, y + 3, 2, 6, 1);
    v2_R(b, x + 2, y + 2, jround(18.0 * pct / 100), 8, 1);
    if (chg)
        v2_R(b, x + 2, y + 2, 18, 8, 0), v2_ic(b, V2I_BOLT, x + 6, y + 1, 1);
}
void v2_kbd(mr_canvas_t *b, int x, int y, int w, int h, int root, int scale, int held)
{
    static const int WH[7] = { 0, 2, 4, 5, 7, 9, 11 }, BL[5][2] = { { 1, 0 }, { 3, 1 }, { 6, 3 }, { 8, 4 }, { 10, 5 } };
    const int kw = w / 7;
    int i;
    for (i = 0; i < 7; i++) {
        const int kx = x + i * kw, n = WH[i];
        v2_O(b, kx, y, kw + 1, h, 1);
        if (n == root || (held >> n & 1))
            v2_R(b, kx + 1, y + 1, kw - 1, h - 2, 1);
        else if (scale >> n & 1)
            v2_R(b, kx + kw / 2 - 1, y + h - 6, 3, 3, 1);
    }
    for (i = 0; i < 5; i++) {
        const int n = BL[i][0], kx = x + (BL[i][1] + 1) * kw - kw / 3, bw = kw * 2 / 3, bh = ifloor(h * 0.6);
        v2_R(b, kx - 1, y, bw + 3, bh + 1, 0);
        v2_R(b, kx, y, bw + 1, bh, 1);
        if (n == root || (held >> n & 1) || (scale >> n & 1))
            v2_R(b, kx + bw / 2 - 1, y + bh - 6, 3, 3, 0);
    }
    v2_R(b, x, y, 7 * kw + 1, 1, 1);
}
void v2_keysMenu(mr_canvas_t *b, const char *name, int layout, int root, int ch, int glob)
{
    int i;
    for (i = 0; i < 4; i++)
        v2_R(b, i * 32 + 1, i == layout ? 0 : 2, 30, i == layout ? 3 : 1, 1);
    v2_TC(b, 64, 7, name, 4, 1);
    for (i = 0; i < 12; i++) {
        const int blk = (0x54A >> i) & 1, x = 4 + i * 10, y = blk ? 39 : 42, h = blk ? 5 : 6;
        if (i == root)
            v2_R(b, x, y, 8, h, 1);
        else
            v2_O(b, x, y, 8, h, 1);
    }
    if (glob < 0)
        for (i = 0; i < 16; i++) {
            if (i == ch)
                v2_R(b, i * 8, 53, 7, 10, 1);
            else
                v2_O(b, i * 8, 53, 7, 10, 1);
        }
    else {
        v2_ic(b, glob ? V2I_GLOBE : V2I_TRACK, 30, 52, 1);
        v2_T(b, 46, 50, glob ? "Global" : "Track", 2, 1);
    }
}
void v2_pages(mr_canvas_t *b, int n, int lo, int hi, int shown, const char *marks)
{
    int i;
    for (i = 0; i < 16; i++) {
        const int x = i * 8, y = 30, mk = marks ? marks[i] : 0;
        if (mk == 'X')
            v2_O(b, x, y, 7, 16, 1), v2_X(b, x + 1, y + 4, 5, 8);
        else if (mk == 'D')
            v2_dashO(b, x, y, 7, 16);
        else if (i >= n)
            v2_R(b, x + 3, y + 7, 1, 2, 1);
        else if (i >= lo && i <= hi)
            v2_R(b, x, y, 7, 16, 1);
        else
            v2_O(b, x, y, 7, 16, 1);
        if (i == shown)
            v2_R(b, x, y + 19, 7, 2, 1);
    }
}
void v2_steps(mr_canvas_t *b, int has, int copied, int cut, int held)
{
    int i;
    for (i = 0; i < 16; i++) {
        const int x = i * 8, y = 52, h = has >> i & 1;
        if (i == cut)
            v2_O(b, x, y, 7, 11, 1), v2_X(b, x + 1, y + 2, 5, 7);
        else if (i == copied) {
            if (h)
                v2_R(b, x + 2, y + 3, 3, 5, 1);
            v2_dashO(b, x, y, 7, 11);
        } else if (h)
            v2_R(b, x, y, 7, 11, 1);
        else
            v2_O(b, x, y, 7, 11, 1);
        if (i == held)
            v2_R(b, x, y - 3, 7, 2, 1);
    }
}
void v2_cell(mr_canvas_t *b, int c, int r, char s)
{
    const int x = c * 16 + 1, y = 4 + r * 15;
    if (s == '.')
        v2_R(b, x + 6, y + 5, 2, 2, 1);
    else if (s == 'P' || s == 'R') {
        v2_R(b, x, y, 14, 13, 1);
        if (s == 'P')
            v2_ic(b, V2I_PLAY, x + 2, y + 1, 0);
        else
            v2_R(b, x + 4, y + 3, 6, 7, 0);
    } else if (s == 'D')
        v2_dashO(b, x, y, 14, 13);
    else if (s == 'X')
        v2_O(b, x, y, 14, 13, 1), v2_X(b, x + 3, y + 3, 8, 7);
    else if (s == 'Q')
        v2_O(b, x, y, 14, 13, 1), v2_O(b, x + 2, y + 2, 10, 9, 1);
    else
        v2_O(b, x, y, 14, 13, 1);
}
