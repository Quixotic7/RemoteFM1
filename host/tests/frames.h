/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Test frames: 240 x 240 RGB565 in emu_hal_t.lcd's layout (each slot holds the pixel's bytes big-endian). */
#ifndef RFM_TEST_FRAMES_H
#define RFM_TEST_FRAMES_H
#include <stdint.h>

#define FR_W 240
#define FR_H 240
#define FR_N (FR_W * FR_H)

uint32_t fr_rand(uint32_t *seed);
uint16_t fr_slot(uint32_t rgb565);                      /* a pixel value -> the slot holding its BE bytes */
uint16_t fr_rgb(uint32_t r8, uint32_t g8, uint32_t b8);  /* -> slot */
void fr_fill(uint16_t *f, uint16_t slot);
void fr_rect(uint16_t *f, int x, int y, int w, int h, uint16_t slot);
void fr_noise(uint16_t *f, uint32_t *seed);
void fr_gradient(uint16_t *f, uint32_t phase);
/* a flat UI in the FM-1's manner: a header bar, four knob cells with anti-aliased text, a list with a moving
 * highlight, a meter; t animates the values (one step per frame) */
void fr_ui(uint16_t *f, uint32_t t);
void fr_text(uint16_t *f, int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* a binary PPM (P6 240 240 255) -> RGB565 slots; 0 ok */
int fr_load_ppm(const char *path, uint16_t *f);

#endif
