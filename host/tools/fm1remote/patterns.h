// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
// --pattern: animated test screens (no core) to measure the screen path. Every 4 s the next of: a flat UI with a
// moving highlight and counters, a scrolling gradient, bouncing boxes on black, full-screen noise (the worst
// case). LEDs chase along the columns.
#pragma once
#include <cstdint>

#include "rfm/codec.h"

const char *pattern_name(uint32_t t_ms);
void pattern_frame(uint16_t *lcd, uint32_t t_ms, uint32_t frame);
void pattern_leds(rfm_leds_t *l, uint32_t t_ms);
