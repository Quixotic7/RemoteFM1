/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 writes the flash only in 0xF2000..0xF3FFF and the calibration's 0xFC000..0xFDFFF (firmware/app.h
 * APP_STORE_OK, which fm1-base's st_erase / st_prog keep to: firmware/patches/fm1-base/0006): every sector and range
 * checked against that window, and every object with sectors of its own inside it. tests/run_tests.sh. */
#include <stdio.h>
#include "app.h"

int main(void)
{
    static const uint32_t MAP[OBJ_NOBJ] = APP_STORE_MAP;
    uint32_t off, obj, fails = 0, n;
    for (off = 0; off < 0x100000u; off += 0x1000u) {
        int want = off == 0xF2000u || off == 0xF3000u || off == 0xFC000u || off == 0xFD000u;
        if (APP_STORE_OK(off, 0x1000u) != want) {
            printf("FAIL sector 0x%05X: %s\n", off, want ? "refused" : "allowed");
            fails++;
        }
    }
    for (off = 0xF0000u; off < 0x100000u; off += 0x80u)   /* writes across the window's edges */
        for (n = 0x80u; n <= 0x2000u; n *= 2u) {
            int want = (off >= 0xF2000u && off + n <= 0xF4000u) || (off >= 0xFC000u && off + n <= 0xFE000u);
            if (APP_STORE_OK(off, n) != want) {
                printf("FAIL 0x%05X + %u: %s\n", off, n, want ? "refused" : "allowed");
                fails++;
            }
        }
    for (obj = 0; obj < OBJ_NOBJ; obj++) {
        if (APP_STORE_VIRTUAL(obj))
            continue;                                     /* (records in OBJ_RF: no sectors) */
        if (!APP_STORE_OK(MAP[obj], 0x1000u) || !APP_STORE_OK(MAP[obj] + 0x1000u, 0x1000u)) {
            printf("FAIL object %u at 0x%05X: outside the window\n", obj, MAP[obj]);
            fails++;
        }
    }
    printf("store_window_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
