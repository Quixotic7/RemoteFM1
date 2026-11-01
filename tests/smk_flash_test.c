/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The M-Vave SMK-37's flash map (docs/SMK37.md "Flash map"; firmware/app.h under BOARD_SMK37, fm1-base's plat.h and
 * felucca-bsp's fm1_flash.h as firmware/patches/smk37 make them): every sector RemoteFM1 may erase or write (its
 * store's windows APP_STORE_OK, every object's A / B sectors, the standalone's directory and its 64 pool sectors, the
 * owner tag, the update's staging and its record, what a start clears) inside the SMK's allowed windows, none of them
 * sharing a sector, none touching the unit's own ranges (0x0..0x3FFF, BTIF and USRTRIM 0xC0000..0xC1FFF, the stock
 * resources 0xF4000..0xFDFFF, key_mac 0xFF000..) nor the stock firmware's USRFLASH end marker's sector (0xEA000, only
 * ever erased); and felucca-bsp's driver's second net refusing those ranges whatever a caller allows.
 *   cc -DBOARD_SMK37=1 -DPLAT_HOST -Ibuild/fm1-base/include -Ibuild/fm1-base/bsp/chip -Ibuild/app-smk37 tests/smk_flash_test.c
 * (build/fm1-base as BOARD=smk37 ./build.sh leaves it). tests/run_tests.sh. */
#include <stdio.h>
#include <string.h>
#include "app.h"
#define FL_MAP_ONLY
#ifndef BOARD_SMK37
#error "build with -DBOARD_SMK37=1"
#endif

/* the unit's own ranges (the guide's "Safety first"), written out again here rather than taken from the code */
static int protected_hit(uint32_t lo, uint32_t hi)  /* [lo, hi) touches one */
{
    static const uint32_t P[4][2] = {{0x0u, 0x4000u}, {0xC0000u, 0xC2000u}, {0xF4000u, 0xFE000u}, {0xFF000u, 0x100000u}};
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (lo < P[i][1] && hi > P[i][0])
            return 1;
    return 0;
}
/* the allowed windows (fm1_flash.h's SMK map): the VM (data and staging), USRFLASH */
static int allowed(uint32_t lo, uint32_t hi)
{
    return (lo >= 0x9D000u && hi <= 0xC0000u) || (lo >= 0xC2000u && hi <= 0xEA000u);
}

static uint8_t owner[256];                          /* each sector's user (0 none) */
static int fails;
static void claim(uint32_t off, uint8_t who, const char *what)
{
    uint32_t s = off >> 12;
    if (off & 0xFFFu || s >= 256u) {
        printf("FAIL %s at 0x%05X: not a sector\n", what, off);
        fails++;
        return;
    }
    if (!allowed(off, off + 0x1000u) || protected_hit(off, off + 0x1000u)) {
        printf("FAIL %s at 0x%05X: outside the SMK's windows\n", what, off);
        fails++;
    }
    if (owner[s] && owner[s] != who) {
        printf("FAIL %s at 0x%05X: shares its sector with another (%u)\n", what, off, owner[s]);
        fails++;
    }
    owner[s] = who;
}

#include "fm1_flash.h"

int main(void)
{
    static const uint32_t MAP[OBJ_NOBJ] = APP_STORE_MAP;
    static const uint32_t CLR[] = APP_SMK_CLEAR;
    uint32_t off, obj, n, s, i;
    /* the store's window: exactly the SMK's data windows less the tag's sector */
    for (off = 0; off < 0x100000u; off += 0x1000u) {
        int want = (off >= 0x9D000u && off < 0xAF000u) || (off >= 0xB0000u && off < 0xBA000u) ||
                   (off >= 0xC2000u && off < 0xEA000u);
        if (APP_STORE_OK(off, 0x1000u) != want) {
            printf("FAIL APP_STORE_OK sector 0x%05X: %s\n", off, want ? "refused" : "allowed");
            fails++;
        }
        if (APP_STORE_OK(off, 0x1000u) && (protected_hit(off, off + 0x1000u) || off == PLAT_SMK_TAG ||
                                           off == PLAT_SMK_STOCKTAG || (off >= 0xBB000u && off < 0xC0000u))) {
            printf("FAIL APP_STORE_OK allows 0x%05X (protected, the tag, the marker or the staging)\n", off);
            fails++;
        }
        if (PLAT_DATA_OK(off, 0x1000u) && (protected_hit(off, off + 0x1000u) || off == PLAT_SMK_TAG ||
                                           off == PLAT_SMK_STOCKTAG || (off >= 0xBB000u && off < 0xC0000u))) {
            printf("FAIL PLAT_DATA_OK allows 0x%05X\n", off);
            fails++;
        }
    }
    for (off = 0x9C000u; off < 0x100000u; off += 0x80u)   /* across the edges, and ranges that would wrap */
        for (n = 0x80u; n <= 0x4000u; n *= 2u)
            if (APP_STORE_OK(off, n) && protected_hit(off, off + n)) {
                printf("FAIL APP_STORE_OK 0x%05X + %u\n", off, n);
                fails++;
            }
    if (APP_STORE_OK(0xE9000u, 0xFFFFFFFFu) || APP_STORE_OK(0xBF000u, 0x4000u) || PLAT_DATA_OK(0xE9F00u, 0x1000u)) {
        printf("FAIL a wrapping or edge-crossing range allowed\n");
        fails++;
    }
    /* every object with sectors (A / B), the directory and the pool, the tag, the staging: one user a sector */
    for (obj = 0; obj < OBJ_NOBJ; obj++) {
        if (APP_STORE_VIRTUAL(obj) || obj == OBJ_PANEL)
            continue;                                     /* (records in OBJ_RF; no calibration on the SMK) */
        claim(MAP[obj], (uint8_t)(10u + obj), "an object's A");
        claim(MAP[obj] + 0x1000u, (uint8_t)(10u + obj), "its B");
        if (!APP_STORE_OK(MAP[obj], 0x2000u)) {
            printf("FAIL object %u: outside APP_STORE_OK\n", obj);
            fails++;
        }
    }
    if (MAP[OBJ_PANEL] != 0u || APP_STORE_OK(MAP[OBJ_PANEL], 0x1000u)) {
        printf("FAIL OBJ_PANEL has sectors on the SMK-37\n");
        fails++;
    }
    claim(SA_STORE_LO, 2, "the standalone's directory A");
    claim(SA_STORE_LO + 0x1000u, 2, "its B");
    for (s = 0; s < SA_POOL_N; s++) {
        claim(SA_POOL_SEC(s), 3, "a pool sector");
        if (!APP_STORE_OK(SA_POOL_SEC(s), 0x1000u)) {
            printf("FAIL pool sector %u: outside APP_STORE_OK\n", s);
            fails++;
        }
    }
    claim(PLAT_SMK_TAG, 4, "the owner tag");
    for (off = FL_OTA_LO; off < FL_OTA_HI; off += 0x1000u)
        claim(off, 5, "the update's staging");
    if (FL_OTA_LO != 0xBB000u || FL_OTA_HI != 0xC0000u) {
        printf("FAIL the staging is not 0xBB000..0xBFFFF\n");
        fails++;
    }
    /* a start's clearing: inside the data windows, never the tag, the staging or USRFLASH */
    for (i = 0; i < sizeof CLR / sizeof CLR[0]; i += 2u)
        for (off = CLR[i]; off < CLR[i + 1u]; off += 0x1000u)
            if (PLAT_DATA_OK(off, 0x1000u) && (off >= PLAT_XDATA1_LO || off == PLAT_SMK_TAG || off >= 0xBB000u)) {
                printf("FAIL APP_SMK_CLEAR would clear 0x%05X\n", off);
                fails++;
            }
    /* felucca-bsp's driver: its window (FL_RANGE_OK) and its second net (FL_SMK_PROTECTED, whatever FL_RANGE_OK) */
    for (off = 0; off < 0x100000u; off += 0x1000u) {
        int prot = protected_hit(off, off + 0x1000u);
        if (FL_SMK_PROTECTED(off, 0x1000u) != prot) {
            printf("FAIL FL_SMK_PROTECTED 0x%05X: %d\n", off, !prot);
            fails++;
        }
        if (FL_GUARD(off, 0x1000u) && (prot || !(allowed(off, off + 0x1000u) || off == 0xEA000u))) {
            printf("FAIL the driver would erase 0x%05X\n", off);
            fails++;
        }
    }
    if (!FL_GUARD(0xEA000u, 0x1000u) || FL_GUARD(0xEA000u, 0x100u) || FL_GUARD(0xEAF00u, 8u)) {
        printf("FAIL the stock marker's sector: erased whole only, never programmed\n");
        fails++;
    }
    if (!FL_SMK_PROTECTED(0xBF000u, 0x2000u) || !FL_SMK_PROTECTED(0xFE000u, 0x1001u) || !FL_SMK_PROTECTED(0xF3F00u, 0x200u) ||
        !FL_SMK_PROTECTED(0x3FFFu, 1u) || !FL_SMK_PROTECTED(0xEA000u, 0xFFFFFFFFu) || FL_SMK_PROTECTED(0xBF000u, 0x1000u)) {
        printf("FAIL FL_SMK_PROTECTED at an edge\n");
        fails++;
    }
    printf("smk_flash_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
