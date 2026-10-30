/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1's store (app.h): everything it keeps lives in two flash sectors, 0xF2000..0xF3FFF, the one window that
 * ChoralRoot, the stock firmware and fm1-base's other firmwares leave free, plus the panel's calibration at 0xFC000 as
 * every fm1-base firmware has it. fm1-base's storage keeps an object as an A/B pair of sectors (the new copy written
 * whole into the other sector, its header last: a cut leaves the old one in charge), so the two sectors hold one
 * object, OBJ_RF, and the objects RemoteFM1 keeps are records inside it (APP_STORE_MUX: plat_store_* are these, on
 * fm1-base's plat_store_raw_*): OBJ_BLE (fm1-base's Bluetooth: its address, the radio's trim, the bondings; up to
 * 1,536 bytes) and OBJ_SA (the standalone's settings: sa_main.c). A record saved as it already is costs no write, so
 * the sectors see a save only when something changed.
 *
 * OBJ_RF's payload: "RFS0", then records: id (the object), 0, length (16 bits, little endian), the bytes. */

#define RS_MAGIC 0x30534652u                  /* "RFS0" */
#define RS_N 2048u                            /* the payload at most (OBJ_BLE's 1,536 and the rest) */
static uint8_t rs_buf[RS_N] PLAT_POOL;
static uint32_t rs_saves, rs_same;            /* (the simulator's checks) */

static uint32_t rs_get(void)                  /* OBJ_RF into rs_buf: its length (4: an empty one) */
{
    int n = plat_store_raw_load(OBJ_RF, rs_buf, RS_N);
    if (n < 4 || ((uint32_t)rs_buf[0] | (uint32_t)rs_buf[1] << 8 | (uint32_t)rs_buf[2] << 16 |
                  (uint32_t)rs_buf[3] << 24) != RS_MAGIC) {
        rs_buf[0] = (uint8_t)RS_MAGIC;
        rs_buf[1] = (uint8_t)(RS_MAGIC >> 8);
        rs_buf[2] = (uint8_t)(RS_MAGIC >> 16);
        rs_buf[3] = (uint8_t)(RS_MAGIC >> 24);
        return 4u;
    }
    return (uint32_t)n;
}

static int rs_find(uint32_t n, uint32_t obj, uint32_t *at, uint32_t *len)   /* obj's record in rs_buf[0..n) */
{
    uint32_t p = 4u;
    while (p + 4u <= n) {
        uint32_t l = (uint32_t)rs_buf[p + 2u] | (uint32_t)rs_buf[p + 3u] << 8;
        if (p + 4u + l > n)
            return 0;                             /* (cut short: the rest is not trusted) */
        if (rs_buf[p] == obj) {
            *at = p;
            *len = l;
            return 1;
        }
        p += 4u + l;
    }
    return 0;
}

int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t n, at, len;
    if (!APP_STORE_VIRTUAL(obj))
        return plat_store_raw_load(obj, dst, max);
    n = rs_get();
    if (!rs_find(n, obj, &at, &len) || !len)
        return -1;
    if (len > max)
        len = max;
    memcpy(dst, rs_buf + at + 4u, len);
    return (int)len;
}

int plat_store_head(uint32_t obj, void *dst, uint32_t want)
{
    uint32_t n, at, len;
    if (!APP_STORE_VIRTUAL(obj))
        return plat_store_raw_head(obj, dst, want);
    n = rs_get();
    if (!rs_find(n, obj, &at, &len) || !len || len < want)
        return -1;
    memcpy(dst, rs_buf + at + 4u, want);
    return (int)want;
}

int plat_store_size(uint32_t obj)
{
    uint32_t n, at, len;
    if (!APP_STORE_VIRTUAL(obj))
        return plat_store_raw_size(obj);
    n = rs_get();
    return rs_find(n, obj, &at, &len) && len ? (int)len : -1;
}

int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t n, at, old;
    if (!APP_STORE_VIRTUAL(obj))
        return plat_store_raw_save(obj, src, len);
    n = rs_get();
    if (rs_find(n, obj, &at, &old)) {
        if (old == len && !memcmp(rs_buf + at + 4u, src, len)) {
            rs_same++;
            return 0;                             /* as it is: no write */
        }
        {                                         /* the old record out (the rest moved down) */
            uint32_t i;
            for (i = at; i + 4u + old < n; i++)
                rs_buf[i] = rs_buf[i + 4u + old];
        }
        n -= 4u + old;
    }
    if (n + 4u + len > RS_N)
        return -1;
    rs_buf[n] = (uint8_t)obj;
    rs_buf[n + 1u] = 0;
    rs_buf[n + 2u] = (uint8_t)len;
    rs_buf[n + 3u] = (uint8_t)(len >> 8);
    memcpy(rs_buf + n + 4u, src, len);
    n += 4u + len;
    rs_saves++;
    return plat_store_raw_save(OBJ_RF, rs_buf, n);
}
