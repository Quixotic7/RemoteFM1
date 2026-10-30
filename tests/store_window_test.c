/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 writes the flash only in 0xF2000..0xF3FFF, the calibration's 0xFC000..0xFDFFF and, from 0.3.1, the
 * standalone's user banks and loops in SA_STORE_LO..SA_STORE_HI, 0x97000..0xD8FFF of fm1-base's data area (firmware/app.h
 * APP_STORE_OK, which fm1-base's st_erase / st_prog keep to: firmware/patches/fm1-base/0006; and sa/sa_store.c's
 * sf_erase / sf_write). Every sector and range checked against that window; XDATA0 (0.3.0's banks, 0x93000..0x96FFF),
 * the update's staging (0xE0000..0xE4FFF), the stock resources (0xF4000..) and the app slot refused; every object
 * with sectors of its own inside the store's part of it. tests/run_tests.sh. */
#include <stdio.h>
#include "app.h"

_Static_assert(SA_STORE_LO == 0x97000u && SA_STORE_HI == 0xD9000u, "the standalone's data: 0x97000..0xD8FFF");
_Static_assert(SA_STORE_LO >= PLAT_DATA_LO && SA_STORE_HI <= PLAT_DATA_HI, "inside fm1-base's data area");

int main(void)
{
    static const uint32_t MAP[OBJ_NOBJ] = APP_STORE_MAP;
    uint32_t off, obj, fails = 0, n;
    for (off = 0; off < 0x100000u; off += 0x1000u) {
        int want = off == 0xF2000u || off == 0xF3000u || off == 0xFC000u || off == 0xFD000u ||
                   (off >= 0x97000u && off <= 0xD8000u);
        if (APP_STORE_OK(off, 0x1000u) != want) {
            printf("FAIL sector 0x%05X: %s\n", off, want ? "refused" : "allowed");
            fails++;
        }
    }
    for (off = 0x90000u; off < 0x100000u; off += 0x80u)   /* writes across the windows' edges */
        for (n = 0x80u; n <= 0x2000u; n *= 2u) {
            int want = (off >= 0xF2000u && off + n <= 0xF4000u) || (off >= 0xFC000u && off + n <= 0xFE000u) ||
                       (off >= 0x97000u && off + n <= 0xD9000u);
            if (APP_STORE_OK(off, n) != want) {
                printf("FAIL 0x%05X + %u: %s\n", off, n, want ? "refused" : "allowed");
                fails++;
            }
        }
    if (APP_STORE_OK(0x93000u, 1u) || APP_STORE_OK(0x96FFFu, 1u) || APP_STORE_OK(0xE0000u, 1u) ||
        APP_STORE_OK(0xF4000u, 1u) || APP_STORE_OK(0xD9000u, 1u) || APP_STORE_OK(0xD8000u, 0xFFFFFFFFu)) {
        printf("FAIL XDATA0, the update's staging, the stock resources, past the end or a wrapping range allowed\n");
        fails++;
    }
    for (obj = 0; obj < OBJ_NOBJ; obj++) {
        if (APP_STORE_VIRTUAL(obj))
            continue;                                     /* (records in OBJ_RF: no sectors) */
        if (!APP_STORE_OK(MAP[obj], 0x1000u) || !APP_STORE_OK(MAP[obj] + 0x1000u, 0x1000u) || MAP[obj] < 0xF2000u) {
            printf("FAIL object %u at 0x%05X: outside the window\n", obj, MAP[obj]);
            fails++;
        }
    }
    printf("store_window_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
