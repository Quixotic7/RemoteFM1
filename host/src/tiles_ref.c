/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The reference TILES decoder, from docs/PROTOCOL.md section 3 only. Deliberately plain: byte by byte, pixel by
 * pixel, no shared code with screen.c. */
#include "rfm/tiles_ref.h"

#include <stdlib.h>
#include <string.h>

#define W 240u
#define H 240u

static void put(uint8_t *shadow, uint32_t x, uint32_t y, uint8_t hi, uint8_t lo)
{
    uint8_t *p = shadow + 2u * (y * W + x);
    p[0] = hi;
    p[1] = lo;
}

int rfm_tiles_apply(uint8_t *shadow, const uint8_t *b, uint32_t n, int strict, rfm_tiles_info_t *info)
{
    uint32_t pos = 4, r;
    rfm_tiles_info_t dummy;
    if (!info)
        info = &dummy;
    memset(info, 0, sizeof *info);
    if (n < 4u)
        return RFM_REF_SHORT_HDR;
    info->frame = (uint16_t)(b[0] | b[1] << 8);
    info->flags = b[2];
    info->nrects = b[3];
    for (r = 0; r < info->nrects; r++) {
        uint32_t x, y, w, h, enc, i, j;
        if (n - pos < 5u) {
            info->used = pos;
            return RFM_REF_SHORT;
        }
        x = b[pos];
        y = b[pos + 1];
        w = b[pos + 2];
        h = b[pos + 3];
        enc = b[pos + 4];
        pos += 5;
        if (w < 1u || h < 1u || x + w > W || y + h > H) {
            info->used = pos;
            return RFM_REF_BOUNDS;
        }
        if (enc == 0x00u) {                             /* RAW: w*h BE pixels */
            if (n - pos < 2u * w * h) {
                info->used = pos;
                return RFM_REF_SHORT;
            }
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++) {
                    put(shadow, x + i, y + j, b[pos], b[pos + 1]);
                    pos += 2;
                }
        } else if (enc == 0x01u) {                      /* FILL: one BE colour */
            if (n - pos < 2u) {
                info->used = pos;
                return RFM_REF_SHORT;
            }
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++)
                    put(shadow, x + i, y + j, b[pos], b[pos + 1]);
            pos += 2;
        } else if (enc == 0x02u) {                      /* PAL: n8, n BE colours, rows of indices */
            uint32_t np, bpp, rowbytes;
            if (n - pos < 1u) {
                info->used = pos;
                return RFM_REF_SHORT;
            }
            np = b[pos++];
            if (np < 2u || np > 16u) {
                info->used = pos;
                return RFM_REF_PAL_N;
            }
            bpp = np <= 2u ? 1u : np <= 4u ? 2u : 4u;
            rowbytes = (w * bpp + 7u) / 8u;
            if (n - pos < 2u * np + rowbytes * h) {
                info->used = pos;
                return RFM_REF_SHORT;
            }
            {
                const uint8_t *pal = b + pos;
                const uint8_t *idx = b + pos + 2u * np;
                for (j = 0; j < h; j++) {
                    const uint8_t *row = idx + j * rowbytes;
                    for (i = 0; i < w; i++) {
                        uint32_t bit = i * bpp;                       /* MSB first within a byte */
                        uint32_t v = (row[bit / 8u] >> (8u - bpp - bit % 8u)) & ((1u << bpp) - 1u);
                        if (v >= np) {                                /* an index past the palette */
                            info->used = pos;
                            return RFM_REF_PAL_N;
                        }
                        put(shadow, x + i, y + j, pal[2u * v], pal[2u * v + 1u]);
                    }
                }
            }
            pos += 2u * np + rowbytes * h;
        } else if (enc == 0x03u) {                      /* RLE: len8 c16BE, len + 1 pixels, across rows */
            uint32_t done = 0, total = w * h;
            while (done < total) {
                uint32_t len, k;
                if (n - pos < 3u) {
                    info->used = pos;
                    return RFM_REF_SHORT;
                }
                len = (uint32_t)b[pos] + 1u;
                if (done + len > total) {
                    info->used = pos;
                    return RFM_REF_RLE_OVER;
                }
                for (k = 0; k < len; k++, done++)
                    put(shadow, x + done % w, y + done / w, b[pos + 1], b[pos + 2]);
                pos += 3;
            }
        } else if (enc == 0x04u) {                      /* COPY: sx8 sy8, memmove semantics */
            uint32_t sx, sy;
            uint8_t *tmp;
            if (n - pos < 2u) {
                info->used = pos;
                return RFM_REF_SHORT;
            }
            sx = b[pos];
            sy = b[pos + 1];
            pos += 2;
            if (sx + w > W || sy + h > H) {
                info->used = pos;
                return RFM_REF_BOUNDS;
            }
            tmp = (uint8_t *)malloc(2u * w * h);
            if (!tmp)
                return RFM_REF_SHORT;
            for (j = 0; j < h; j++)
                memcpy(tmp + 2u * w * j, shadow + 2u * ((sy + j) * W + sx), 2u * w);
            for (j = 0; j < h; j++)
                memcpy(shadow + 2u * ((y + j) * W + x), tmp + 2u * w * j, 2u * w);
            free(tmp);
        } else {
            info->used = pos;
            return RFM_REF_ENC;
        }
        info->applied++;
        info->px += w * h;
    }
    info->used = pos;
    if (strict && pos != n)
        return RFM_REF_TRAILING;
    return RFM_REF_OK;
}
