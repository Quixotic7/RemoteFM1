/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The user's DX7 banks (STANDALONE-PLAN.md S5, question 3 (b)): a DX7 32-voice bulk dump (F0 43 0n 09 20 00, 4,096
 * bytes of voices, the checksum, F7) sent to the unit over USB or Bluetooth MIDI, from any librarian, is kept in the
 * flash's XDATA0 window: up to three banks, USER1..USER3, listed after the ROM cartridges (GLO + PRESETS). A fourth
 * arriving replaces the oldest. Part of sa_main.c (its state, sa).
 *
 * The flash: one sector a bank (0x93000, 0x94000, 0x95000: the voices, each byte | 0x80, so that bytes 0xF00..0xF07 of
 * a sector never read as an update record: the loader's "AT" is 0x41 0x54, and every stored byte is 0x80 or more),
 * and their headers in 0x96000 (32 bytes each: "RFUB", the arrival's count, the voices' CRC-32). A bank is written
 * only when one arrives: the headers' sector erased and the others' headers written back, the bank's sector erased
 * and written, its header last (a cut leaves it missing, never half there). ChoralRoot's or the stock firmware's code
 * overwrites this window when they are installed: the banks are then missing (their CRC fails) and are sent again.
 * The only flash RemoteFM1 writes beside its store, and only through sa_ub_erase / sa_ub_write, which keep to
 * APP_STORE_OK (firmware/app.h; tests/store_window_test.c, tests/run_tests.sh). */

#define SA_UB_N 3u
#define SA_UB_DATA 0x93000u                  /* bank k at SA_UB_DATA + 0x1000 k */
#define SA_UB_HDR 0x96000u
#define SA_UB_MAGIC 0x42554652u              /* "RFUB" */
#define SA_UB_LEN 4104u                      /* the dump: 6 + 4096 + 2 */

typedef struct {
    uint32_t magic;
    uint8_t seq, rsv[3];
    uint32_t crc;
    uint8_t pad[20];
} sa_ubh_t;
_Static_assert(sizeof(sa_ubh_t) == 32u, "32 bytes a header");

static uint8_t sa_ub_rx[SA_UB_LEN] PLAT_POOL;   /* a dump coming in */
static struct {
    uint32_t n;                              /* its bytes so far */
    uint8_t st;                              /* 0 none, 1 collecting, 2 too long: dropped */
    volatile uint8_t ready;                  /* a whole one, for the main loop */
    uint8_t valid[SA_UB_N];
    sa_ubh_t h[SA_UB_N];
    uint8_t vbuf[2][128];                    /* voices read back (|0x80 undone), two at a time */
    uint8_t vb;
    uint32_t got, bad, writes, refused;      /* (the simulator's checks) */
} ub;

static uint32_t sa_crc32(const uint8_t *p, uint32_t n, uint32_t x)   /* (the data as stored) */
{
    uint32_t i, k;
    x = ~x;
    for (i = 0; i < n; i++) {
        x ^= p[i];
        for (k = 0; k < 8u; k++)
            x = x >> 1 ^ (0xEDB88320u & (0u - (x & 1u)));
    }
    return ~x;
}

/* the flash: only inside APP_STORE_OK (the user banks' four sectors and the store's) */
static int sa_ub_erase(uint32_t off)
{
    if (!APP_STORE_OK(off, 0x1000u)) {
        ub.refused++;
        return -8;
    }
    return plat_data_erase(off);
}
static int sa_ub_write(uint32_t off, const void *src, uint32_t n)
{
    if (!APP_STORE_OK(off, n)) {
        ub.refused++;
        return -8;
    }
    return plat_data_write(off, src, n);
}

static void sa_ub_scan(void)                 /* which banks are there */
{
    uint32_t k;
    for (k = 0; k < SA_UB_N; k++) {
        const uint8_t *h = plat_data(SA_UB_HDR + 32u * k), *d = plat_data(SA_UB_DATA + 0x1000u * k);
        ub.valid[k] = 0;
        if (!h || !d)
            continue;
        memcpy(&ub.h[k], h, sizeof ub.h[k]);
        ub.valid[k] = ub.h[k].magic == SA_UB_MAGIC && ub.h[k].crc == sa_crc32(d, 4096u, 0);
    }
}

static uint32_t sa_ub_count(void)
{
    return (uint32_t)ub.valid[0] + ub.valid[1] + ub.valid[2];
}

static int32_t sa_ub_slot(uint32_t j)        /* the j-th bank there (in slot order), -1 none */
{
    uint32_t k;
    for (k = 0; k < SA_UB_N; k++)
        if (ub.valid[k] && !j--)
            return (int32_t)k;
    return -1;
}

static const uint8_t *sa_ub_vmem(uint32_t slot, uint32_t voice)
{
    const uint8_t *d = plat_data(SA_UB_DATA + 0x1000u * slot + 128u * (voice & 31u));
    uint8_t *v = ub.vbuf[ub.vb ^= 1u];
    uint32_t i;
    for (i = 0; i < 128u; i++)
        v[i] = d ? (uint8_t)(d[i] & 0x7Fu) : 0u;
    return v;
}

/* every SysEx byte from USB or Bluetooth (src/rfm_main.c): a DX7 bulk dump collected */
static void sa_ub_byte(uint32_t b)
{
    if (b == 0xF0u) {
        ub.n = 0;
        ub.st = ub.ready ? 2u : 1u;          /* (one waits for the main loop: this one dropped) */
    }
    if ((b & 0x80u) && b != 0xF0u && b != 0xF7u)
        ub.st = 0;                           /* (a status ends it) */
    if (ub.st != 1u)
        return;
    if (ub.n == SA_UB_LEN) {
        ub.st = 2;
        return;
    }
    sa_ub_rx[ub.n++] = (uint8_t)b;
    if (b == 0xF7u) {
        ub.st = 0;
        if (ub.n == SA_UB_LEN && sa_ub_rx[1] == 0x43u && (sa_ub_rx[2] & 0xF0u) == 0u && sa_ub_rx[3] == 0x09u)
            ub.ready = 1;
    } else if (ub.n == 4u && (sa_ub_rx[1] != 0x43u || sa_ub_rx[3] != 0x09u)) {
        ub.st = 2;                           /* (not a DX7 bank: not collected) */
    }
}

/* the main loop: a dump that came in, checked and kept; the slot it went to, -1 none */
static int32_t sa_ub_take(void)
{
    uint32_t i, sum = 0, k, oldest = 0, crc;
    sa_ubh_t h;
    if (!ub.ready)
        return -1;
    for (i = 0; i < 4096u; i++)
        sum += sa_ub_rx[6u + i];
    if (sa_ub_rx[4] != 0x20u || sa_ub_rx[5] != 0x00u || ((sum + sa_ub_rx[4102]) & 0x7Fu) != 0u) {
        ub.bad++;
        ub.ready = 0;
        return -1;
    }
    for (i = 0; i < 4096u; i++)              /* (stored | 0x80: sa_ub_vmem undoes it) */
        sa_ub_rx[6u + i] |= 0x80u;
    crc = sa_crc32(sa_ub_rx + 6, 4096u, 0);
    for (k = 0; k < SA_UB_N && ub.valid[k]; k++)
        ;
    if (k == SA_UB_N) {                      /* all three there: the oldest goes */
        for (i = 1, k = 0; i < SA_UB_N; i++)
            if ((uint8_t)(ub.h[i].seq - ub.h[oldest].seq) >= 0x80u)
                oldest = i;
        k = oldest;
    }
    ub.valid[k] = 0;
    if (sa_ub_erase(SA_UB_HDR) == 0) {
        for (i = 0; i < SA_UB_N; i++)
            if (ub.valid[i])
                sa_ub_write(SA_UB_HDR + 32u * i, &ub.h[i], 32u);
        if (sa_ub_erase(SA_UB_DATA + 0x1000u * k) == 0 && sa_ub_write(SA_UB_DATA + 0x1000u * k, sa_ub_rx + 6, 4096u) == 0) {
            memset(&h, 0xFF, sizeof h);
            h.magic = SA_UB_MAGIC;
            h.seq = ++sa.ubank_seq;
            h.crc = crc;
            sa_ub_write(SA_UB_HDR + 32u * k, &h, 32u);
            ub.writes++;
        }
    }
    ub.ready = 0;
    ub.got++;
    sa_ub_scan();
    return ub.valid[k] ? (int32_t)k : -1;
}

/* ------------------------------------------------------------------ every bank: the ROM's, then the user's --- */
static uint32_t sa_nbanks(void) { return SA_NBANKS + sa_ub_count(); }

static uint32_t sa_bank_voices(uint32_t bank) { return bank < SA_NBANKS ? SA_BANK_VOICES : 32u; }

static const uint8_t *sa_bank_vmem(uint32_t bank, uint32_t voice)
{
    int32_t k = bank >= SA_NBANKS ? sa_ub_slot(bank - SA_NBANKS) : -1;
    return k < 0 ? sa_rom_vmem(bank, voice) : sa_ub_vmem((uint32_t)k, voice);
}

static const char *sa_bank_name(uint32_t bank)   /* "ROM1A", "USER2" */
{
    static const char *const U[SA_UB_N] = {"USER1", "USER2", "USER3"};
    int32_t k = bank >= SA_NBANKS ? sa_ub_slot(bank - SA_NBANKS) : -1;
    return k < 0 ? sa_rom_name(bank) : U[k];
}

static uint32_t sa_bank_of_slot(uint32_t slot)   /* the bank number of user slot k (it is there) */
{
    uint32_t j, b = SA_NBANKS;
    for (j = 0; j < slot; j++)
        b += ub.valid[j];
    return b;
}
