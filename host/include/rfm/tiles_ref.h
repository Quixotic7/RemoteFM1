/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: a reference TILES decoder, written from docs/PROTOCOL.md section 3 alone (it shares no code with
 * the encoder). The tests check the encoder against it; the fake device draws with it. Portable C99.
 *
 * shadow: 240 x 240 pixels, 2 bytes each, big-endian (byte 0 = the high byte), i.e. emu_hal_t.lcd's memory. */
#ifndef RFM_TILES_REF_H
#define RFM_TILES_REF_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RFM_REF_OK = 0, RFM_REF_SHORT_HDR, RFM_REF_BOUNDS, RFM_REF_ENC, RFM_REF_SHORT, RFM_REF_PAL_N, RFM_REF_RLE_OVER,
    RFM_REF_TRAILING
};

typedef struct {
    uint16_t frame;
    uint8_t flags, nrects;
    uint32_t applied;                                   /* rects applied before an error */
    uint32_t used;                                      /* body bytes consumed */
    uint32_t px;                                        /* pixels written */
} rfm_tiles_info_t;

/* applies one TILES body in order; on a malformed rect it stops there (what came before stays applied) and returns
 * the error. strict: bytes left over after nrects rects are an error too (RFM_REF_TRAILING). */
int rfm_tiles_apply(uint8_t *shadow, const uint8_t *body, uint32_t n, int strict, rfm_tiles_info_t *info);

#ifdef __cplusplus
}
#endif
#endif
