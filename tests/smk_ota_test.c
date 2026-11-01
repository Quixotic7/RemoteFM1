/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors
 * Its fake M-UPGRADE and simulated flash adapted from fm1-base's tests/ota_test.c (Charles Vestal; Leo Kuroshita,
 * Hügelton Instruments; MPL-2.0). */
/* The M-Vave SMK-37's update receiver (felucca-bsp chip/ota.c as firmware/patches/smk37 patch it, BOARD_SMK37):
 * a fake M-UPGRADE serves real packages over the update's SysEx and a simulated flash plays the unit's.
 *   cc -DBOARD_SMK37=1 -I build/fm1-base/bsp/chip -o smk_ota_test tests/smk_ota_test.c
 *   smk_ota_test SMK.fwsc MODEL FM1.fwsc [OTHER.fwsc...]
 * SMK.fwsc (an SMK-37 package of RemoteFM1's, of MODEL "SMK-37 Pro", "SMK-37 Elite" or "MKE-P37", as the build's
 * SMK_MODEL): staged at 0xBB000, its record at 0xBFF00 naming "ota-MODEL_016", nothing written outside the staging;
 * FM1.fwsc (RemoteFM1's FM-1 package) and each OTHER (any FM-1 package: M-VAVE's own, another firmware's): refused
 * (-15) before anything is erased; the SMK package with its USRTRIM region entry changed: refused too. The packages'
 * identity blocks are found as an installer finds them (20, or 36 as the SMK-37's). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SMK_MODEL
#define SMK_MODEL "SMK-37 Pro"
#endif
#define FELUCCA_ID SMK_MODEL "_5200"
#define FELUCCA_OTA_DRYRUN 0
#define FL_OTA_LO 0xBB000u                       /* (felucca-bsp fm1_flash.h's, SMK-37) */
#define FL_OTA_HI 0xC0000u
static int not_smk;                              /* the unit has no factory calibration record (unity.c smk_board_seen) */
#define OTA_BOARD_ANY() (not_smk)

static uint8_t nor[0x100000];
static uint8_t *logical;
static size_t logical_len;
static uint8_t rx[1024];
static uint32_t rx_len, rx_full;
static uint32_t now_ms, requests, bad_write, erases;
static int committed;
static uint8_t committed_parm[112];

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}
static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 7;
        if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
    return o;
}
static void host_reply(const uint8_t *msg, uint32_t n)
{
    rx_len = pack7(msg, n, rx);
    rx_full = 1;
}

/* ---- ota.c's hooks ---- */
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64], m[600];
    uint32_t addr, len, i, s = 0;
    unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11)
        return 0;
    addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24;
    len = u[11] | u[12] << 8 | u[13] << 16;
    requests++;
    memcpy(m, "\x00\x59\x30", 3);
    m[3] = (uint8_t)(len + 8); m[4] = (uint8_t)((len + 8) >> 8); m[5] = 0;
    m[6] = 0;
    memcpy(m + 7, &addr, 4);
    m[11] = (uint8_t)len; m[12] = (uint8_t)(len >> 8); m[13] = 0;
    if (addr == 0xE0000000u) {
        memset(m + 14, 0, len);
        memcpy(m + 14, "success", 8);
    } else {
        if (addr + len > logical_len) { printf("read past the package %#x\n", addr); exit(1); }
        memcpy(m + 14, logical + addr, len);
    }
    for (i = 6; i < 14 + len; i++) s += m[i];
    m[14 + len] = (uint8_t)~s;
    host_reply(m, 15 + len);
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!rx_full) return 0;
    *p = rx; *n = rx_len;
    return 1;
}
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms += 1; }
static uint32_t area_lo(void) { return not_smk ? 0xE0000u : 0xBB000u; }
static int ota_erase(uint32_t off)              /* (as unity.c's: the staging only) */
{
    if (off < area_lo() || off >= area_lo() + 0x5000u || (off & 0xFFFu)) { bad_write++; return -8; }
    memset(nor + off, 0xFF, 4096);
    erases++;
    return 0;
}
static int ota_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (off < area_lo() || off + n > area_lo() + 0x5000u) { bad_write++; return -8; }
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static int ota_fread(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step, (void)code; }
static void ota_commit(const uint8_t *parm) { committed = 1; memcpy(committed_parm, parm, 112); }

#include "ota.c"

static int check(const char *what, int ok)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

/* the logical image of a package whose identity takes nb blocks */
static uint8_t *unmark(const uint8_t *raw, size_t n, uint32_t nb, size_t *len)
{
    uint8_t *l = malloc(n);
    uint32_t i;
    for (i = 0; i < nb; i++)
        memcpy(l + i * 47u, raw + i * 48u, 47);
    memcpy(l + nb * 47u, raw + nb * 48u, n - nb * 48u);
    *len = n - nb;
    return l;
}
static int ufw_ok(const uint8_t *l, size_t len)  /* its UFW header's CRCs hold, and its files' heads where it says */
{
    uint8_t h[0x400], e32[32];
    uint32_t nent, i;
    memcpy(h, l, sizeof h);
    ota_jl_enc(h, 0x40);
    nent = ota_rd16(h + 8);
    if (ota_crc16(h + 2, 0x3E, 0) != ota_rd16(h) || !nent || nent > 11u ||
        ota_crc16(h + 0x40, nent * 0x50u, 0) != ota_rd16(h + 2))
        return 0;
    for (i = 0; i < nent; i++) {                 /* flash.bin's flash header, ota.bin's JLFS head: a CRC each */
        uint8_t *e = h + 0x40 + i * 0x50u;
        uint32_t off;
        ota_jl_enc(e, 0x50);
        off = ota_rd32(e + 8);
        if (off + 32u > len)
            return 0;
        memcpy(e32, l + off, 32);
        if (ota_rd16(e) == 0)
            ota_jl_enc(e32, 32);
        if ((ota_rd16(e) == 0 || ota_rd16(e) == 100) && ota_crc16(e32 + 2, 30, 0) != ota_rd16(e32))
            return 0;
    }
    return 1;
}
static int load(const char *path, char *ident)   /* -> logical; the identity from its marker bytes */
{
    static const uint32_t NB[2] = {36, 20};
    FILE *f = fopen(path, "rb");
    uint8_t *raw;
    size_t n, len;
    uint32_t k, i, j;
    if (!f) { perror(path); return -1; }
    raw = malloc(0x200000);
    n = fread(raw, 1, 0x200000, f);
    fclose(f);
    for (k = 0; k < 2u; k++) {
        uint8_t *l = unmark(raw, n, NB[k], &len);
        if (!ufw_ok(l, len)) {
            free(l);
            continue;
        }
        for (i = j = 0; i < NB[k]; i++)
            if (raw[i * 48u + 47u] != 0x7D)
                ident[j++] = (char)(raw[i * 48u + 47u] - i - 1u);
        ident[j] = 0;
        free(logical);
        logical = l;
        logical_len = len;
        free(raw);
        return (int)NB[k];
    }
    free(raw);
    printf("%s: not a package (no UFW header after 20 or 36 blocks)\n", path);
    return -1;
}
static void reset_dev(const uint8_t *head)       /* the unit's flash head: an SMK-37's (the SPL, isd_config) */
{
    memset(nor, 0xFF, sizeof nor);
    memcpy(nor, head, 0x4000);
    rx_full = 0; requests = 0; committed = 0; bad_write = 0; erases = 0;
}
static int staging_erased(void)
{
    uint32_t i;
    for (i = 0xBB000u; i < 0xC0000u; i++)
        if (nor[i] != 0xFFu) return 0;
    return 1;
}
/* the flash image's app_area_head entry named name: its offset field changed (and its CRC made good again) */
static int region_move(const char *name, uint32_t to)
{
    uint8_t h[0x400], key_blk[512];
    uint32_t fl_off = 0, i, j, nent;
    memcpy(h, logical, sizeof h);
    ota_jl_enc(h, 0x40);
    nent = ota_rd16(h + 8);
    for (i = 0; i < nent; i++) {
        uint8_t *e = h + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 0) fl_off = ota_rd32(e + 8);
    }
    memcpy(key_blk, logical + fl_off + 0x4000u, 512);
    for (i = 0; i < 512u; i += 32u) {                /* SFC with the key 0x980F (our packages') */
        uint32_t k = 0x980Fu ^ (i >> 2);
        for (j = 0; j < 32u; j++) { key_blk[i + j] ^= (uint8_t)k; k = ((k << 1) ^ (k & 0x8000u ? 0x1021u : 0u)) & 0xFFFFu; }
    }
    for (i = 32; i < 512u; i += 32u)
        if (!memcmp(key_blk + i + 16, name, strlen(name) + 1)) {
            ota_wr32(key_blk + i + 4, to);
            ota_wr16(key_blk + i, ota_crc16(key_blk + i + 2, 30, 0));
            for (i = 0; i < 512u; i += 32u) {
                uint32_t k = 0x980Fu ^ (i >> 2);
                for (j = 0; j < 32u; j++) { key_blk[i + j] ^= (uint8_t)k; k = ((k << 1) ^ (k & 0x8000u ? 0x1021u : 0u)) & 0xFFFFu; }
            }
            memcpy(logical + fl_off + 0x4000u, key_blk, 512);
            return 0;
        }
    return -1;
}

int main(int argc, char **argv)
{
    static uint8_t head[0x4000];
    char ident[64], want[40];
    int bad = 0, rc, nb, i;
    if (argc < 4) { printf("usage: smk_ota_test SMK.fwsc MODEL FM1.fwsc [OTHER.fwsc...]\n"); return 2; }
    if ((nb = load(argv[1], ident)) < 0)
        return 2;
    if (strcmp(argv[2], SMK_MODEL)) { printf("built for %s, the package is %s's\n", SMK_MODEL, argv[2]); return 2; }
    snprintf(want, sizeof want, "%s_", SMK_MODEL);
    bad += check("the SMK package: 36 identity blocks", nb == 36);
    bad += check("its identity names the model and RemoteFM1's number", !strncmp(ident, want, strlen(want)) &&
                 !strncmp(ident + strlen(want), "52", 2));
    printf("  identity %s (%zu characters)\n", ident, strlen(ident));
    memcpy(head, logical + 0x400, sizeof head);         /* (flash.bin's head: the SPL and the chip key, as a unit has) */

    reset_dev(head);
    rc = ota_session();
    bad += check("the SMK package is staged and committed", rc == 0 && committed);
    bad += check("staging 0xBB000: LOADER.BIN head", !memcmp(nor + 0xBB000u + 16, "LOADER.BIN", 10));
    bad += check("its record at 0xBFF00 == the RAM record", !memcmp(nor + 0xBFF00u, committed_parm, 112));
    snprintf(want, sizeof want, "ota-%s_016", SMK_MODEL);
    bad += check("the record names the stock loader of the model (ota-MODEL_016)",
                 !memcmp(committed_parm + 8, want, strlen(want) + 1) && committed_parm[8 + strlen(want)] == 0 &&
                 ota_rd32(committed_parm + 72) == 0xBB000u);
    bad += check("nothing erased or written outside 0xBB000..0xBFFFF", bad_write == 0);
    bad += check("boot cleanup erases it", ota_boot_cleanup() == 1 && staging_erased());

    if (!region_move("USRTRIM", 0xE9000u)) {
        reset_dev(head);
        rc = ota_session();
        bad += check("its USRTRIM region elsewhere: refused (-15), nothing staged", rc == -15 && !committed && staging_erased() && !bad_write);
        if (rc != -15)
            printf("  rc %d\n", rc);
    } else {
        bad += check("the SMK package has a USRTRIM region entry", 0);
    }

    /* a unit with no factory record (an FM-1 that was given this build): the FM-1 build's update, an FM-1 package
     * taken (staged at 0xE0000, the record at 0xE4F00 naming the FM-1's loader) so the unit can be put right */
    if ((nb = load(argv[3], ident)) < 0)
        return 2;
    not_smk = 1;
    reset_dev(head);
    rc = ota_session();
    bad += check("no factory record: an FM-1 package is taken, as the FM-1 build takes it", rc == 0 && committed &&
                 !memcmp(committed_parm + 8, "ota-FM-1_015", 13) && ota_rd32(committed_parm + 72) == 0xE0000u &&
                 !memcmp(nor + 0xE4F00u, committed_parm, 112) && !bad_write);
    not_smk = 0;

    for (i = 3; i < argc; i++) {
        char what[160];
        const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if ((nb = load(argv[i], ident)) < 0)
            return 2;
        reset_dev(head);
        rc = ota_session();
        snprintf(what, sizeof what, "%s (%s): refused (-15), nothing staged", base, ident);
        bad += check(what, rc == -15 && !committed && staging_erased() && !bad_write);
        if (rc != -15)
            printf("  rc %d\n", rc);
    }
    printf(bad ? "SMK OTA: %d FAILED\n" : "SMK OTA: all passed\n", bad);
    return bad ? 1 : 0;
}
