/* SPDX-License-Identifier: MPL-2.0 */
/* A 5x7 font (in an 8-row cell, so g, j, p, q and y can descend) for ASCII 32..126. Each glyph is 5 column bytes,
 * bit 0 at the top: the same layout as the Move's packed screen, so text on a row that is a multiple of 8 is a copy.
 * The glyphs are the classic HD44780-style 5x7 set. */
#ifndef MR_FONT5X7_H
#define MR_FONT5X7_H

#include <stdint.h>

#define MR_FONT_W 5
#define MR_FONT_ADV 6                         /* advance: one blank column between characters */
#define MR_FONT_H 8

extern const uint8_t mr_font5x7[95][5];

/* the glyph for c (a '?' for anything outside 32..126) */
static inline const uint8_t *mr_glyph(char c)
{
    const unsigned u = (unsigned char)c;
    return mr_font5x7[(u < 32 || u > 126) ? ('?' - 32) : (u - 32)];
}

#endif
