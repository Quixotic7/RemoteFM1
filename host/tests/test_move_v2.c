/* SPDX-License-Identifier: MPL-2.0 */
/* The v2 screen's primitives against the design canvas: every screen of V2Screen.dc.html (its S table, the same
 * sample data) drawn with v2.c and compared with the mockup's own pixels (move_v2_ref.h, from move_v2_ref.mjs).
 * Writes v2_<name>.pbm for the contact sheet. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "move_int.h"
#include "moveremote/client.h"
#include "rfm/move.h"
#include "v2.h"
#include "move_v2_ref.h"

static int nfail, ncheck;
#define CHECKF(c, ...) do { ncheck++; if (!(c)) { nfail++; fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
                              fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

typedef mr_canvas_t B;
static void R(B *b, int x, int y, int w, int h) { v2_R(b, x, y, w, h, 1); }
static void R0(B *b, int x, int y, int w, int h) { v2_R(b, x, y, w, h, 0); }
static void O(B *b, int x, int y, int w, int h) { v2_O(b, x, y, w, h, 1); }
#define T(b, x, y, s, k) v2_T(b, x, y, s, k, 1)
#define TC(b, x, y, s, k) v2_TC(b, x, y, s, k, 1)
#define ic(b, n, x, y) v2_ic(b, n, x, y, 1)
#define BIT(i) (1u << (i))
#define NONE (-1.0)

/* the owner's 64 x 64 FM-1 drawing (rows 15..51), as V2Screen has it */
static const char *const ART[37] = {
    "0ffffffffffffff0", "1000000000000008", "2000000000000004", "20003fff00000004", "20003fff00000004", "230c30031830c184",
    "230c30031830c184", "200031c300000004", "200033e300000004", "2000366300000004", "200036333fffff84", "230c3c3344444444",
    "230c381f44444444", "2000300f44444444", "200030037fffffc4", "20003fff44444444", "23fe3fff44444444", "2222000044444444",
    "23fe00003fffff84", "2000000000000004", "2000000000000004", "21ffffffffffff04", "2200000000000084", "2236db6db6db6c84",
    "2236db6db6db6c84", "2236db6db6db6c84", "2236db6db6db6c84", "2200000000000084", "22db6db6db6db684", "22db6db6db6db684",
    "22db6db6db6db684", "22db6db6db6db684", "2200000000000084", "21ffffffffffff04", "2000000000000004", "1000000000000008",
    "0ffffffffffffff0" };
static int hexv(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static void art(B *b)
{
    int r, x;
    for (r = 0; r < 37; r++)
        for (x = 0; x < 64; x++)
            if (hexv(ART[r][x >> 2]) >> (3 - (x & 3)) & 1)
                v2_px(b, x, 15 + r, 1);
}
static void thumb(B *b)
{
    int i;
    R0(b, 64, 0, 64, 64);
    R(b, 64, 0, 64, 10), v2_T(b, 66, 1, "P12 BELL", 1, 0);
    for (i = 0; i < 4; i++) {
        O(b, 68 + i * 14, 15, 10, 8);
        if (i)
            R(b, 64 + i * 14, 18, 4, 1);
    }
    v2_L(b, 66, 60, 74, 34), v2_L(b, 74, 34, 90, 46), R(b, 90, 46, 20, 1), v2_L(b, 110, 46, 125, 60), R(b, 66, 61, 60, 1);
}
static const int NOTES[8][3] = { { 0, 2, 9 }, { 2, 1, 9 }, { 4, 2, 6 }, { 6, 1, 9 }, { 8, 3, 4 }, { 11, 1, 6 }, { 12, 2, 2 }, { 14, 2, 6 } };
static void roll(B *b, int n, int head, int y0, int h)
{
    int s, i;
    for (s = 4; s < 16; s += 4)
        v2_dotV(b, s * 8, y0, h);
    for (i = 0; i < n; i++)
        R(b, NOTES[i][0] * 8 + 1, y0 + 2 + NOTES[i][2] * ((h - 4) / 12), NOTES[i][1] * 8 - 2, 3);
    if (head >= 0)
        R(b, head, y0, 1, h);
}
static void grid(B *b, const char *const rows[4], int sel)
{
    int r, c;
    if (sel >= 0)
        R(b, sel * 16 + 1, 0, 14, 2);
    for (r = 0; r < 4; r++)
        for (c = 0; rows[r][c]; c++)
            v2_cell(b, c, r, rows[r][c]);
}
static int has_mask(const int *a, int n)
{
    int m = 0, i;
    for (i = 0; i < n; i++)
        m |= 1 << a[i];
    return m;
}

static void draw(B *b, const char *n)
{
#define IS(s) (!strcmp(n, s))
    static const int HAS[8] = { 0, 2, 4, 6, 8, 11, 12, 14 };
    int i;
    if (IS("sys.type")) { T(b, 0, 0, "1x font5x7 Aa gjpqy 09%", 1); T(b, 0, 11, "2x Tempo", 2); T(b, 0, 32, "4x 12", 4); }
    else if (IS("sys.icons")) { for (i = 0; i < V2I_N; i++) ic(b, i, 1 + (i % 9) * 14, 1 + (i / 9) * 16); }
    else if (IS("sys.parts")) {
        v2_knobs(b, 0xB3, 1);
        T(b, 0, 14, "1/16", 2);
        v2_icSel(b, V2I_ARP, 54, 13); ic(b, V2I_KEYS, 70, 13);
        v2_dashO(b, 92, 13, 14, 13); O(b, 112, 13, 14, 13); v2_X(b, 115, 16, 8, 7);
        v2_vbar(b, 0.62, 0, 0, 60, 38); v2_vbar(b, -0.3, 1, 66, 60, 38);
        v2_strip(b, 8, BIT(2), 0.31, 0, 60); R(b, 66, 63, 40, 1); R(b, 122, 61, 5, 1); R(b, 124, 59, 1, 5);
    }
    else if (IS("sys.msg")) { v2_msg(b, "Step", "Copied"); v2_strip(b, 4, BIT(1), NONE, 0, 128); }
    else if (IS("fm1.rest")) { art(b); thumb(b); }
    else if (IS("fm1.touch")) { art(b); v2_XR(b, 40, 18, 6, 6); R(b, 39, 26, 3, 3); R0(b, 18, 54, 2, 4); thumb(b); }
    else if (IS("fm1.knob")) { thumb(b); v2_lpop(b, "Cutoff", "+3"); }
    else if (IS("fm1.jog")) { thumb(b); v2_lpop(b, "Select", "-2"); }
    else if (IS("fm1.master")) { thumb(b); v2_lpop(b, "Master", "50%"); }
    else if (IS("fm1.shift")) { art(b); thumb(b); ic(b, V2I_SHIFT, 2, 2); }
    else if (IS("mode.fm1")) v2_modeName(b, "FM-1", 4, 0);
    else if (IS("midi.pads")) {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 8; c++) {
                const int x = c * 16 + 1, y = (3 - r) * 16 + 1, deg = c + r * 3;
                if (deg % 7 == 0) R(b, x, y, 14, 14); else O(b, x, y, 14, 14);
                if ((r == 1 && c == 2) || (r == 0 && c == 4)) v2_R(b, x + 4, y + 4, 6, 6, deg % 7 != 0);
            }
    }
    else if (IS("midi.notes")) thumb(b);
    else if (IS("midi.chord")) { thumb(b); TC(b, 32, 4, "Cm7", 3); v2_kbd(b, 1, 34, 62, 28, -1, 0, BIT(0) | BIT(3) | BIT(7) | BIT(10)); }
    else if (IS("midi.knob")) { thumb(b); v2_lpop(b, "Algo", "+1"); }
    else if (IS("midi.hold2")) v2_keysMenu(b, "D MIN", 0, 2, 0, -1);
    else if (IS("midi.scale")) v2_keysMenu(b, "D DOR", 0, 2, 0, -1);
    else if (IS("mode.notes")) v2_modeName(b, "Notes", 2, 1);
    else if (IS("ctl.rest")) {
        static const double v[8] = { 0.7, 0.45, 0.9, 0.2, 0.6, 1, 0.35, 0.55 };
        for (i = 0; i < 8; i++) {
            const int x = i * 16 + 1, h = (int)(55 * v[i] + 0.5);
            R(b, x, 57, 14, 1); R(b, x, 57 - h, 14, h);
        }
        v2_strip(b, 4, BIT(1), NONE, 0, 128);
    }
    else if (IS("ctl.knob")) { v2_titleBox(b, "CC 74", "96", 64, 124); v2_vbar(b, 96 / 127.0, 0, 4, 120, 52); v2_strip(b, 4, BIT(1), NONE, 0, 128); }
    else if (IS("ctl.hold3")) { v2_knobs(b, 0xff, 3); T(b, 6, 18, "CC", 2); T(b, 40, 13, "20", 4); TC(b, 106, 18, "K4", 2); v2_strip(b, 4, BIT(1), NONE, 0, 128); }
    else if (IS("seq.track")) { roll(b, 8, 44, 0, 56); v2_strip(b, 4, BIT(1), 0.34, 0, 128); }
    else if (IS("seq.step")) {
        TC(b, 64, 3, "C3 Eb3 G3", 2);
        ic(b, V2I_VEL, 2, 26); T(b, 18, 24, "100", 2);
        R(b, 68, 31, 18, 1); R(b, 68, 28, 1, 7); R(b, 85, 28, 1, 7); T(b, 92, 24, "1.5", 2);
        v2_steps(b, has_mask(HAS, 8), -1, -1, 4);
    }
    else if (IS("seq.len")) { v2_titleBox(b, "Length", "1.5", 64, 124); v2_strip(b, 4, BIT(1), 0.3, 0, 128); }
    else if (IS("seq.micro")) { v2_titleBox(b, "Nudge", "+25%", 64, 124); v2_strip(b, 4, BIT(1), 0.33, 0, 128); }
    else if (IS("seq.vel")) { v2_titleBox(b, "Velocity", "91", 64, 124); v2_vbar(b, 91 / 127.0, 0, 4, 120, 52); v2_strip(b, 4, BIT(1), NONE, 0, 128); }
    else if (IS("seq.lock")) { v2_titleBox(b, "CC 74", "69", 64, 124); ic(b, V2I_LOCK, 114, 4); v2_vbar(b, 69 / 127.0, 0, 4, 120, 52); v2_strip(b, 4, BIT(1), NONE, 0, 128); }
    else if (IS("seq.clip")) { roll(b, 8, -1, 0, 46); v2_steps(b, has_mask(HAS, 8), 4, 8, -1); }
    else if (IS("seq.count")) { ic(b, V2I_REC, 14, 22); TC(b, 76, 14, "3", 4); v2_strip(b, 4, BIT(0) | BIT(1), NONE, 0, 128); }
    else if (IS("mode.seq")) v2_modeName(b, "Seq", 4, 3);
    else if (IS("trig.micro")) { R(b, 2, 47, 41, 1); R(b, 2, 40, 1, 8); R(b, 42, 40, 1, 8); R(b, 12, 26, 26, 4); v2_L(b, 4, 36, 10, 36); v2_L(b, 8, 34, 10, 36); v2_L(b, 8, 38, 10, 36); v2_titleBox(b, "Micro", "+5", 88, 80); v2_strip(b, 5, BIT(0), NONE, 0, 128); }
    else if (IS("trig.ratch")) { for (i = 0; i < 4; i++) R(b, 4 + i * 10, 12 + i * 4, 4, 34 - i * 4); R(b, 2, 47, 40, 1); v2_titleBox(b, "Ratch", "x4", 88, 80); v2_strip(b, 5, BIT(0), NONE, 0, 128); }
    else if (IS("trig.chance")) { static const int P[5][2] = { { 8, 14 }, { 30, 14 }, { 19, 25 }, { 8, 36 }, { 30, 36 } }; for (i = 0; i < 5; i++) R(b, P[i][0], P[i][1], 6, 6); v2_titleBox(b, "Chance", "75%", 88, 80); v2_strip(b, 5, BIT(0), NONE, 0, 128); }
    else if (IS("trig.cond")) { for (i = 0; i < 4; i++) { const int x = 2 + i * 10; if (i == 0) R(b, x, 18, 8, 20); else R(b, x, 36, 8, 2); } v2_titleBox(b, "Cond", "1:4", 88, 80); v2_strip(b, 5, BIT(0), NONE, 0, 128); }
    else if (IS("trig.func")) { R(b, 2, 45, 41, 1); for (i = 0; i < 6; i++) R(b, 2 + i * 8, 40, 1, 6); v2_L(b, 5, 38, 13, 20); v2_L(b, 13, 20, 29, 20); v2_L(b, 29, 20, 37, 38); v2_L(b, 33, 36, 37, 38); v2_L(b, 37, 34, 37, 38); v2_titleBox(b, "Jump", "+3", 88, 80); v2_strip(b, 5, BIT(0), NONE, 0, 128); }
    else if (IS("trig.len")) { R(b, 2, 45, 41, 1); for (i = 0; i < 6; i++) R(b, 2 + i * 8, 40, 1, 6); R(b, 3, 26, 26, 6); v2_titleBox(b, "Length", "3.5", 88, 80); v2_strip(b, 5, BIT(0), NONE, 0, 128); }
    else if (IS("loop.edit")) { ic(b, V2I_LOOP, 3, 3); TC(b, 76, 4, "Bars 3-6", 2); v2_pages(b, 8, 2, 5, 3, NULL); }
    else if (IS("loop.len")) { v2_titleBox(b, "Loop", "4 Bars", 64, 124); v2_strip(b, 16, BIT(2) | BIT(3) | BIT(4) | BIT(5), NONE, 0, 128); }
    else if (IS("loop.ghost")) { TC(b, 64, 22, "Bar 5", 2); v2_ghostStrip(b, 4, 6); }
    else if (IS("loop.marks")) { char mk[16] = { 0 }; mk[1] = 'X', mk[4] = 'D'; ic(b, V2I_LOOP, 3, 3); TC(b, 76, 4, "Bars 1-4", 2); v2_pages(b, 6, 0, 3, 1, mk); }
    else if (IS("loop.doubled")) { v2_msg(b, "Loop", "Doubled"); v2_strip(b, 16, 0xFF, NONE, 0, 128); }
    else if (IS("sess.rest")) { static const char *const G[4] = { "PCCC.P..", "C.QCC...", "C..R....", ".C......" }; grid(b, G, 1); }
    else if (IS("sess.marks")) { static const char *const G[4] = { "PCXC.P..", "C.QDC...", "C..R....", ".C......" }; grid(b, G, 1); }
    else if (IS("sess.hold4")) {
        TC(b, 64, 3, "Track 3", 2);
        for (i = 0; i < 16; i++) {
            const int x = i * 8, y = 28;
            if (i == 2) R(b, x, y, 7, 26);
            else if (i == 5) { O(b, x, y, 7, 26); v2_X(b, x + 1, y + 9, 5, 8); }
            else if (i == 9) { O(b, x, y, 7, 26); R(b, x + 2, y + 10, 3, 6); }
            else if (i < 7 || i == 8 || i == 10) O(b, x, y, 7, 26);
            else R(b, x + 3, y + 12, 1, 2);
        }
    }
    else if (IS("menu.global")) { v2_knobs(b, 7, -1); v2_battery(b, 60, 1, 87, 0); T(b, 90, 0, "87%", 2); T(b, 0, 22, "Clock Int", 2); T(b, 0, 42, "Vel Fixed", 2); }
    else if (IS("menu.charge")) { v2_battery(b, 52, 8, 64, 1); TC(b, 64, 28, "Charging", 2); TC(b, 64, 46, "64%", 2); }
    else if (IS("menu.keys")) v2_keysMenu(b, "D DOR", 0, 2, -1, 1);
    else if (IS("menu.layout")) { static const int I4[4] = { V2I_INKEY, V2I_CHROM, V2I_DRUM, V2I_OCT }; for (i = 0; i < 4; i++) { if (i == 0) v2_icSel(b, I4[i], 22 + i * 24, 6); else ic(b, I4[i], 22 + i * 24, 6); } TC(b, 64, 30, "In-Key", 2); v2_strip(b, 2, BIT(0), NONE, 0, 128); }
    else if (IS("menu.vel")) { v2_knobs(b, 1, -1); R(b, 2, 12, 1, 40); R(b, 2, 51, 40, 1); R(b, 3, 22, 38, 2); T(b, 50, 12, "Fixed", 2); T(b, 50, 30, "100", 2); v2_vbar(b, 100 / 127.0, 0, 50, 76, 48); }
    else if (IS("menu.arp")) { static const int P[6][2] = { { 2, 46 }, { 12, 36 }, { 22, 26 }, { 32, 16 }, { 42, 26 }, { 52, 36 } }; v2_knobs(b, 15, -1); for (i = 0; i < 6; i++) R(b, P[i][0], P[i][1], 8, 4); T(b, 66, 10, "Up", 2); T(b, 66, 27, "1/16", 2); T(b, 66, 44, "Oct 2", 2); v2_strip(b, 3, BIT(0), NONE, 0, 128); }
    else if (IS("menu.lane")) { static const int v[8] = { 6, -4, 10, 0, -8, 3, 12, -2 }; v2_knobs(b, 11, 0); T(b, 0, 11, "Mod", 2); R(b, 0, 40, 128, 1); for (i = 0; i < 8; i++) { const int x = 4 + i * 15, d = v[i]; R(b, x, d >= 0 ? 40 - d : 41, 10, d ? (d > 0 ? d : -d) : 1); if (i == 2) R(b, x, 55, 10, 2); } v2_strip(b, 3, BIT(1), NONE, 0, 128); }
    else if (IS("menu.tempo")) { ic(b, V2I_TAP, 2, 3); TC(b, 64, 3, "Tempo", 2); TC(b, 64, 22, "123", 4); v2_strip(b, 4, 7, NONE, 0, 128); }
    else if (IS("menu.locked")) { ic(b, V2I_LOCK, 2, 3); TC(b, 64, 3, "Tempo", 2); TC(b, 64, 22, "120", 4); }
    else if (IS("menu.metro")) { ic(b, V2I_METRO, 58, 4); v2_msg(b, "Click On", NULL); }
    else if (IS("menu.groove")) { v2_knobs(b, 3, 0); R(b, 4, 12, 3, 30); R(b, 34, 12, 3, 30); v2_dotV(b, 28, 12, 30); v2_titleBox(b, "Groove", "62%", 88, 50); v2_icSel(b, V2I_GLOBE, 2, 48); ic(b, V2I_TRACK, 18, 48); T(b, 40, 48, "Global", 2); }
    else if (IS("menu.work")) { v2_knobs(b, 7, -1); T(b, 0, 14, "Ch 3", 2); T(b, 0, 31, "Rate 1/16T", 2); T(b, 0, 48, "Quant 50%", 2); }
    else if (IS("menu.rec")) { static const int I3[3] = { V2I_CAPTURE, V2I_REC, V2I_STEP }; for (i = 0; i < 3; i++) { if (i == 0) v2_icSel(b, I3[i], 34 + i * 24, 2); else ic(b, I3[i], 34 + i * 24, 2); } TC(b, 64, 22, "Capture", 2); ic(b, V2I_OVERDUB, 22, 44); T(b, 40, 42, "Overdub", 2); }
    else if (IS("menu.quant")) { v2_msg(b, "Quantized", NULL); v2_strip(b, 4, BIT(1), NONE, 0, 128); }
    else if (IS("menu.newclip")) { ic(b, V2I_CLIP, 58, 4); v2_msg(b, "New Clip", NULL); }
    else if (IS("proj.grid")) {
        static const int U[33][3] = { [1] = { 6, 3, 8 }, [2] = { 4, 8, 5 }, [3] = { 2, 3, 6 }, [5] = { 8, 8, 8 }, [7] = { 5, 7, 3 }, [8] = { 3, 6, 2 }, [12] = { 7, 2, 4 }, [17] = { 2, 5, 8 } };
        for (i = 0; i < 32; i++) {
            const int nn = i + 1, x = (i % 8) * 16 + 1, y = (i / 8) * 16 + 1, h = U[nn][0] != 0;
            int k;
            if (nn == 7) R(b, x, y, 14, 14); else if (h) O(b, x, y, 14, 14); else R(b, x + 6, y + 6, 2, 2);
            if (h) for (k = 0; k < 3; k++) v2_R(b, x + 3 + k * 3, y + 11 - U[nn][k], 2, U[nn][k], nn == 7 ? 0 : 1);
        }
    }
    else if (IS("proj.marks")) {
        for (i = 0; i < 32; i++) {
            const int x = (i % 8) * 16 + 1, y = (i / 8) * 16 + 1;
            if (i == 6) R(b, x, y, 14, 14); else if (i == 1) { O(b, x, y, 14, 14); v2_X(b, x + 3, y + 3, 8, 8); } else if (i == 4) v2_dashO(b, x, y, 14, 14); else if (i == 0 || i == 2 || i == 7 || i == 11) O(b, x, y, 14, 14); else R(b, x + 6, y + 6, 2, 2);
        }
    }
    else if (IS("proj.held")) { static const int Hh[8] = { 20, 12, 26, 6, 0, 16, 9, 0 }; T(b, 2, 4, "07", 4); for (i = 0; i < 8; i++) { const int h = Hh[i] > 1 ? Hh[i] : 1; R(b, 64 + i * 8, 32 - h, 5, h); } ic(b, V2I_PLAY, 2, 44); T(b, 16, 43, "120", 2); ic(b, V2I_CLIP, 74, 44); T(b, 90, 43, "12", 2); }
    else if (IS("proj.color")) { v2_titleBox(b, "Color", "11", 64, 124); v2_strip(b, 16, BIT(10), NONE, 0, 128); }
    else if (IS("mode.proj")) { ic(b, V2I_PROJ, 58, 4); TC(b, 64, 26, "Projects", 2); }
    else if (IS("rec.capture")) { ic(b, V2I_REC, 0, 2); T(b, 16, 1, "Recording", 2); roll(b, 5, 92, 20, 36); v2_ghostStrip(b, 3, 4); R(b, 64, 61, 30, 3); }
    else if (IS("rec.sample")) {
        ic(b, V2I_WAVE, 0, 2); T(b, 16, 1, "Sampling", 2);
        for (i = 0; i < 60; i++) {
            const int a = (int)floor(fabs(sin(i * 0.9) * cos(i * 0.21)) * 16 + 0.5) + 1;
            R(b, i * 2, 38 - a, 1, a * 2);
        }
        for (i = 1; i < 4; i++) v2_dotV(b, i * 32, 20, 36);
        v2_strip(b, 4, BIT(1), 0.48, 0, 128);
    }
    else if (IS("rec.captured")) { v2_titleBox(b, "Captured", "85 BPM", 64, 124); v2_strip(b, 4, 3, NONE, 0, 128); }
    else T(b, 2, 2, n, 1);
#undef IS
}

static uint32_t hash_of(const B *b)
{
    uint32_t h = 0x811c9dc5u;
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 128; x++)
            h = (h ^ b->px[y][x]) * 0x01000193u;
    return h;
}
static void write_pbm(const char *name, const B *b)
{
    char path[96];
    FILE *f;
    int x, y;
    snprintf(path, sizeof path, "v2_%s.pbm", name);
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++, fputc('\n', f))
        for (x = 0; x < 128; x++)
            fprintf(f, "%d ", b->px[y][x]);
    fclose(f);
}

/* ================================================================ the core, the same screens === */
/* each mockup's situation set up on a live core (as close as the core's own data allows), its screen written as core_<name>.pbm for the contact sheet; the mockup's FM-1 screen stand-in
 * given as the core's thumbnail */
static rfm_move_t C;
static uint32_t now_ms;
static void raw(uint8_t st, uint8_t d1, uint8_t d2)
{
    const uint8_t pkt[4] = { (uint8_t)(st >> 4), st, d1, d2 };
    mr_event_t ev;
    mr_decode(pkt, 0, &ev);
    rfm_move_input(&C, &ev, now_ms += 5);
}
static void pad(int p, int vel) { raw(vel ? 0x90 : 0x80, (uint8_t)(MR_NOTE_PAD0 + p), (uint8_t)vel); }
static void step(int s, int down) { raw(down ? 0x90 : 0x80, (uint8_t)(MR_NOTE_STEP0 + s), down ? 127 : 0); }
static void button(int cc, int down) { raw(0xB0, (uint8_t)cc, down ? 127 : 0); }
static void press(int cc) { button(cc, 1), button(cc, 0); }
static void knob(int k, int d) { raw(0xB0, (uint8_t)(MR_CC_KNOB1 + k), (uint8_t)(d & 127)); }
static void touch(int k, int on) { raw(on ? 0x90 : 0x80, (uint8_t)k, on ? 127 : 0); }
static void jog(int d) { raw(0xB0, MR_CC_JOG, (uint8_t)(d & 127)); }
static void track(int t, int down) { button(MR_CC_TRACK1 - t, down); }
static void tap_track(int t) { track(t, 1), track(t, 0); }
static void adv_ms(uint32_t ms) { rfm_move_seq_advance(&C, ms * 48, 48000); }
static void rest(void) { adv_ms(RFM_MOVE_POPUP_MS + 5); }
static void hold_track(int t) { track(t, 1), adv_ms(RFM_MOVE_TAP_MS + 5); }

static void reset(int mode)
{
    static B tb;
    int x, y;
    rfm_move_init(&C, NULL, NULL, NULL);
    memset(&tb, 0, sizeof tb);
    thumb(&tb);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            C.thumb[y][x] = tb.px[y][64 + x];
    if (mode)
        tap_track(mode);
    rest();
}
static void seq_notes(int page)                        /* the mockup's NOTES on a page (row 2 the highest) */
{
    int i;
    for (i = 0; i < 8; i++)
        rfmv_seq_note_put(&C, 0, page * 16 + NOTES[i][0], 72 - NOTES[i][2], 100, NOTES[i][1] * RFM_MOVE_SEQ_TPS, 0);
}
static void shot(const char *name)
{
    uint8_t out[MR_SCREEN_BYTES];
    char path[96];
    FILE *f;
    int x, y;
    rfm_move_screen(&C, NULL, out);
    snprintf(path, sizeof path, "core_%s.pbm", name);
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++, fputc('\n', f))
        for (x = 0; x < 128; x++)
            fprintf(f, "%d ", out[(y >> 3) * 128 + x] >> (y & 7) & 1);
    fclose(f);
}
static void shift_step(int s) { button(MR_CC_SHIFT, 1), step(s, 1), step(s, 0), button(MR_CC_SHIFT, 0); }

static void core_screens(void)
{
    int i;
    /* FM-1 mode */
    reset(0), shot("fm1.rest");
    reset(0), touch(5, 1), shot("fm1.touch");
    reset(0), rfm_move_set_label(&C, RFM_MOVE_E_K1, "CUTOFF"), touch(4, 1), knob(4, 6), shot("fm1.knob");
    reset(0), jog(-1), jog(-1), shot("fm1.jog");
    reset(0), rfm_move_set_master(&C, 511), touch(8, 1), knob(8, 1), knob(8, 127), shot("fm1.master");
    reset(0), hold_track(0), shot("fm1.shift");
    reset(1), tap_track(0), shot("mode.fm1");
    /* MIDI mode */
    reset(1), pad(10, 100), pad(4, 100), shot("midi.pads");
    reset(1), tap_track(1), rest(), shot("midi.notes");
    reset(1), tap_track(1), rest();
    C.note_count[0][60] = C.note_count[0][63] = C.note_count[0][67] = C.note_count[0][70] = 1, shot("midi.chord");
    reset(1), tap_track(1), rest(), knob(1, 2), shot("midi.knob");
    reset(1), C.s.root = 2, C.s.scale = RFM_MOVE_SCALE_MINOR, hold_track(1), shot("midi.hold2");
    reset(1), C.s.root = 2, C.s.scale = RFM_MOVE_SCALE_DORIAN, hold_track(1), shot("midi.scale");
    reset(1), tap_track(1), shot("mode.notes");
    /* Control mode */
    reset(2), C.s.bank = 1;
    {
        static const double v[8] = { 0.7, 0.45, 0.9, 0.2, 0.6, 1, 0.35, 0.55 };
        for (i = 0; i < 8; i++)
            C.cc_val[1][i] = (uint8_t)(v[i] * 127 + 0.5);
    }
    shot("ctl.rest");
    C.s.cc[1][0] = 74, C.cc_val[1][0] = 95, knob(0, 1), shot("ctl.knob");
    rest(), C.s.cc[1][3] = 19, hold_track(2), knob(3, 1), shot("ctl.hold3");
    /* the sequencer: track view */
    reset(3), rfmv_seq_loop_set(&C, 0, 0, 64), seq_notes(1), C.seq.page = 1;
    rfm_move_seq_play(&C, 1), adv_ms(500 * 21 / 4 + 62), shot("seq.track");
    reset(3), rfmv_seq_loop_set(&C, 0, 0, 64), C.seq.page = 1;
    rfmv_seq_note_put(&C, 0, 20, 60, 100, 36, 0), rfmv_seq_note_put(&C, 0, 20, 63, 100, 36, 0), rfmv_seq_note_put(&C, 0, 20, 67, 100, 36, 0);
    for (i = 0; i < 8; i++)
        if (NOTES[i][0] != 4)
            rfmv_seq_note_put(&C, 0, 16 + NOTES[i][0], 60, 100, 24, 0);
    step(4, 1), adv_ms(400), shot("seq.step");
    jog(1), jog(-1), shot("seq.len");
    press(MR_CC_RIGHT), shot("seq.micro");
    knob(8, 1), shot("seq.vel");
    C.s.cc[C.s.bank][2] = 74, knob(2, 1), shot("seq.lock");
    step(4, 0), rest();
    button(MR_CC_COPY, 1), step(4, 1), step(4, 0), shot("seq.clip"), button(MR_CC_COPY, 0);
    reset(3), C.seq.rec_mode = RFM_MOVE_RM_RECORD, press(MR_CC_REC), press(MR_CC_PLAY), adv_ms(600), shot("seq.count");
    reset(0), tap_track(3), shot("mode.seq");
    /* the TRIG page */
    {
        static const char *const TN[6] = { "trig.micro", "trig.ratch", "trig.chance", "trig.cond", "trig.func", "trig.len" };
        static const int TK[6] = { 0, 1, 2, 3, 4, 6 }, TD[6] = { 5, 3, -5, 2, 8, 60 };
        for (i = 0; i < 6; i++) {
            reset(3), rfmv_seq_note_put(&C, 0, 0, 60, 100, 24, 0);
            while (!C.seq.trig)
                jog(-1);
            rest(), step(0, 1), knob(TK[i], TD[i]), shot(TN[i]), step(0, 0);
        }
    }
    /* loop and pages */
    reset(3), rfmv_seq_loop_set(&C, 0, 32, 64), rfmv_seq_note_put(&C, 0, 7 * 16, 60, 100, 24, 0), C.seq.page = 3;
    press(MR_CC_LOOP), shot("loop.edit");
    jog(1), jog(-1), shot("loop.len");
    reset(3), rfmv_seq_loop_set(&C, 0, 0, 64), C.seq.page = 3, press(MR_CC_LOOP), press(MR_CC_RIGHT);
    shot("loop.ghost");
    reset(3), rfmv_seq_loop_set(&C, 0, 0, 64), rfmv_seq_note_put(&C, 0, 16, 60, 100, 24, 0), rfmv_seq_note_put(&C, 0, 5 * 16, 60, 100, 24, 0);
    C.seq.page = 1, press(MR_CC_LOOP), button(MR_CC_DELETE, 1), step(1, 1), step(1, 0), button(MR_CC_DELETE, 0), rest(), shot("loop.marks");
    reset(3), rfmv_seq_loop_set(&C, 0, 0, 64), shift_step(14), shot("loop.doubled");
    /* session */
    reset(3), press(MR_CC_MENU);
    {
        static const char *const G[4] = { "PCCC.P..", "C.QCC...", "C..R....", ".C......" };
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 8; c++) {
                C.seq.tr[c].active = -1;
                if (G[r][c] != '.')
                    rfmv_seq_note_put(&C, SEQ_CLIP(c, r), 0, 60, 100, 24, 0);
            }
        for (r = 0; r < 4; r++)
            for (c = 0; c < 8; c++)
                if (G[r][c] == 'P')
                    C.seq.tr[c].active = (int8_t)r;
        C.seq.tr[2].queued = 1, C.seq.tr[3].active = 2, C.seq.tr[3].grow = 1, C.seq.sel_track = 1;
        rfm_move_seq_play(&C, 1), adv_ms(10);
    }
    shot("sess.rest");
    button(MR_CC_COPY, 1), pad(16 + 3, 100), pad(16 + 3, 0), button(MR_CC_COPY, 0), shot("sess.marks");
    reset(3);
    C.seq.sel_track = 2, C.seq.tr[5].mute = 1, C.seq.tr[9].solo = 1;
    for (i = 0; i < 11; i++)
        if (i < 7 || i == 8 || i == 10)
            rfmv_seq_note_put(&C, SEQ_CLIP(i, 0), 0, 60, 100, 24, 0);
    hold_track(3), shot("sess.hold4"), track(3, 0);
    /* the Shift menus */
    reset(3), rfm_move_set_battery(&C, 87, 0), C.s.vel_mode = RFM_MOVE_VEL_FIXED, rfm_move_seq_set_clock(&C, RFM_MOVE_CLOCK_INTERNAL);
    shift_step(1), shot("menu.global");
    reset(3), C.s.root = 2, C.s.scale = RFM_MOVE_SCALE_DORIAN, shift_step(8), shot("menu.keys");
    reset(3), shift_step(7), shot("menu.layout");
    reset(3), C.s.vel_mode = RFM_MOVE_VEL_FIXED, C.s.vel_level = 100, shift_step(9), shot("menu.vel");
    reset(3), shift_step(10), knob(1, 7 * RFM_MOVE_LIST_TICKS), shot("menu.arp");
    reset(3), shift_step(10), jog(1), shot("menu.lane");
    reset(3), shift_step(4), shift_step(4), rfm_move_seq_set_bpm(&C, 1230), shot("menu.tempo");
    reset(3), rfm_move_seq_set_clock(&C, RFM_MOVE_CLOCK_EXTERNAL), shift_step(4), shot("menu.locked");
    reset(3), shift_step(5), shot("menu.metro");
    reset(3), C.seq.groove = 62, shift_step(6), touch(0, 1), shot("menu.groove");
    reset(3), C.seq.tr[0].ch = 2, C.seq.qstrength = 50, shift_step(2), shot("menu.work");
    reset(3), button(MR_CC_SHIFT, 1), press(MR_CC_REC), button(MR_CC_SHIFT, 0), shot("menu.rec");
    reset(3), shift_step(15), shot("menu.quant");
    reset(3), shift_step(13), shot("menu.newclip");
    /* projects mode */
    reset(3);
    {
        static const int U[33][3] = { [1] = { 6, 3, 8 }, [2] = { 4, 8, 5 }, [3] = { 2, 3, 6 }, [5] = { 8, 8, 8 }, [7] = { 5, 7, 3 }, [8] = { 3, 6, 2 }, [12] = { 7, 2, 4 }, [17] = { 2, 5, 8 } };
        int n;
        for (n = 1; n <= 32; n++)
            if (U[n][0]) {
                rfm_move_proj_info_t *o = &C.seq.info[n - 1];
                memset(o, 0, sizeof *o);
                o->ok = 1, o->bpm10 = 1200, o->clips = 12;
                for (i = 0; i < 3; i++)
                    o->notes[i] = (uint16_t)(1u << (U[n][i] - 1));
                C.seq.proj_used |= 1u << (n - 1);
            }
        C.seq.proj_cur = 6, C.seq.proj_used |= 1u << 6;
    }
    shift_step(0), shot("mode.proj"), rest(), shot("proj.grid");
    C.seq.proj_cur = 30, pad(24 + 6, 100), adv_ms(10), shot("proj.held"), knob(0, RFM_MOVE_LIST_TICKS), shot("proj.color"), pad(24 + 6, 0), rest();
    button(MR_CC_DELETE, 1), pad(24 + 1, 100), pad(24 + 1, 0), button(MR_CC_DELETE, 0), rest(), shot("proj.marks");
    /* recording */
    reset(3), C.seq.rec_mode = RFM_MOVE_RM_RECORD, seq_notes(0), press(MR_CC_REC), press(MR_CC_PLAY), adv_ms(2000 + 500 * 23 / 2), shot("rec.capture");
    reset(3), press(MR_CC_SAMPLE), press(MR_CC_PLAY), adv_ms(2000 + 1000), shot("rec.sample");
    reset(3);
    for (i = 0; i < 4; i++)
        pad(i, 100), adv_ms(250), pad(i, 0), adv_ms(100);
    press(MR_CC_CAPTURE), shot("rec.captured");
}

int main(void)
{
    int i;
    for (i = 0; i < (int)(sizeof V2_REF / sizeof V2_REF[0]); i++) {
        static B b;
        uint32_t h;
        memset(&b, 0, sizeof b);
        draw(&b, V2_REF[i].name);
        h = hash_of(&b);
        write_pbm(V2_REF[i].name, &b);
        CHECKF(h == V2_REF[i].hash, "%s: %08x, the mockup %08x", V2_REF[i].name, h, V2_REF[i].hash);
    }
    core_screens();
    printf("move_v2: %d checks, %d failed\n", ncheck, nfail);
    return nfail != 0;
}
