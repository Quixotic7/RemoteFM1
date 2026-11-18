/* SPDX-License-Identifier: MPL-2.0 */
/* The v2 Move screen's drawing (the owner's design canvas, V2Screen.dc.html: "one grammar, Move's conventions,
 * symbols at rest"), ported pixel for pixel: the same primitives, the same rounding, font5x7 in its 8-row cell.
 * No boxes around values or icons; a chosen icon underlined; dashes 2 on 2 off; Move's bottom strip. */
#ifndef RFM_MOVE_V2_H
#define RFM_MOVE_V2_H

#include <stdint.h>
#include "moveremote/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { V2I_PLAY, V2I_REC, V2I_STOP, V2I_LOOP, V2I_METRO, V2I_ARP, V2I_GROOVE, V2I_KEYS, V2I_VEL, V2I_TAP, V2I_LOCK, V2I_SHIFT, V2I_MUTE, V2I_SOLO, V2I_CLIP, V2I_PROJ, V2I_WAVE, V2I_DICE, V2I_GLOBE, V2I_TRACK, V2I_BOLT, V2I_CAPTURE, V2I_STEP, V2I_REPLACE, V2I_OVERDUB, V2I_INKEY, V2I_CHROM, V2I_DRUM, V2I_OCT, V2I_N };

/* the bitmap primitives (v: 1 white, 0 black) */
void v2_px(mr_canvas_t *b, int x, int y, int v);
void v2_R(mr_canvas_t *b, int x, int y, int w, int h, int v);        /* a filled rectangle */
void v2_XR(mr_canvas_t *b, int x, int y, int w, int h);              /* inverted */
void v2_O(mr_canvas_t *b, int x, int y, int w, int h, int v);        /* an outline */
void v2_dashO(mr_canvas_t *b, int x, int y, int w, int h);           /* a dashed outline (2 on, 2 off) */
void v2_dotH(mr_canvas_t *b, int x, int y, int w);
void v2_dotV(mr_canvas_t *b, int x, int y, int h);
void v2_L(mr_canvas_t *b, int x0, int y0, int x1, int y1);           /* a line (Bresenham) */
void v2_X(mr_canvas_t *b, int x, int y, int w, int h);               /* a cross */
int v2_tw(const char *s, int k);                                      /* text width at k x */
int v2_T(mr_canvas_t *b, int x, int y, const char *s, int k, int v); /* text at k x; returns the next x */
int v2_TC(mr_canvas_t *b, double cx, int y, const char *s, int k, int v);   /* centred on cx */
void v2_ic(mr_canvas_t *b, int icon, int x, int y, int v);           /* an 11 x 11 icon */
void v2_icSel(mr_canvas_t *b, int icon, int x, int y);               /* the chosen one: underlined */
void v2_ring(mr_canvas_t *b, int x, int y, int fill);
/* the shared parts */
void v2_knobs(mr_canvas_t *b, int mask, int touched);                /* the knob legend: rings over the live knobs */
/* Move's bottom strip: n segments (the ones in sel thick); a tick at x0 + tick * w (tick < 0: none) */
void v2_strip(mr_canvas_t *b, int n, uint32_t sel, double tick, int x0, int w);
void v2_ghostStrip(mr_canvas_t *b, int real, int total);             /* the real pages a line, a + at the far end */
void v2_titleBox(mr_canvas_t *b, const char *title, const char *val, int cx, int w);   /* the edit popup, no box */
void v2_vbar(mr_canvas_t *b, double frac, int bip, int x, int w, int y);   /* a value bar (bipolar: from the middle) */
void v2_msg(mr_canvas_t *b, const char *a, const char *c);           /* a message: one or two centred rows at 2x */
void v2_modeName(mr_canvas_t *b, const char *s, int n, int cur);     /* the full-screen mode name and the strip */
void v2_lpop(mr_canvas_t *b, const char *label, const char *val);    /* the left-half popup: the label, the value */
void v2_battery(mr_canvas_t *b, int x, int y, int pct, int chg);
/* one octave: root -1 none; scale and held: bit n for pitch class n */
void v2_kbd(mr_canvas_t *b, int x, int y, int w, int h, int root, int scale, int held);
/* the build's key / scale menu (kept): glob -1 the 16 channels (ch), 0 Track, 1 Global */
void v2_keysMenu(mr_canvas_t *b, const char *name, int layout, int root, int ch, int glob);
/* the pages as cells (Loop page edit): n pages in the clip, lo..hi looped, shown underlined; mark[i] 'X' / 'D' / 0 */
void v2_pages(mr_canvas_t *b, int n, int lo, int hi, int shown, const char *marks);
/* the 16 steps as cells: has (a bit a step), copied, cut, held (-1 none) */
void v2_steps(mr_canvas_t *b, int has, int copied, int cut, int held);
/* a session cell: P playing, C clip, Q queued, R recording, . empty, D copied, X cut */
void v2_cell(mr_canvas_t *b, int c, int r, char s);

#ifdef __cplusplus
}
#endif
#endif
