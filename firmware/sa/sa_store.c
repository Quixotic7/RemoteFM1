/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's data in the flash's data area (0.3.1; STANDALONE-PLAN.md question 3, the owner's decision of
 * 2026-10-30: "I think it's fine to use the space choralroot or fizz stores data in. Both can backup and restore
 * data."): the 16 user DX7 banks (sa_ubank.c) and the 16 loops (sa_loop.c), in SA_STORE_LO..SA_STORE_HI
 * (firmware/app.h: 0x97000..0xD8FFF, 66 sectors of fm1-base's data area 0x97000..0xDFFFF, where ChoralRoot and FIZZ
 * keep theirs: installing one of them loses what RemoteFM1 kept there, and a save here loses theirs). Part of
 * sa_main.c.
 *
 * The layout: two directory sectors (0x97000, 0x98000: an A/B pair, the newer valid one in charge) and a pool of 64
 * sectors (0x99000..0xD8FFF). The directory lists each user bank (its pool sector, its arrival, the CRC-32 of its 4,096
 * voice bytes, a name) and each loop (its length, tempo, bars, event count, the CRC-32 of its events and the pool
 * sectors that hold them, 512 events of 8 bytes a sector). Nothing is ever written over: a save writes the changed
 * banks' and loops' data into pool sectors the directory in charge does not use, then the other directory sector with
 * a higher sequence number (its CRC last in it). A power cut anywhere leaves the old directory, and so the old data,
 * in charge. Only what changed is written (a loop as it already is costs nothing; a save where nothing changed writes
 * nothing), and the pool's sectors are taken in turn from a cursor (wear spread).
 *
 * The audio: the device's erase stops everything (felucca-bsp: IRQs off, the audio silenced: plat.h), so a save is
 * done in steps from the main loop, one flash operation (one 4 KiB erase, or SF_CHUNK bytes written) a step and one
 * step every SF_GAP_MS at most: the sound plays between them.
 *
 * The bytes of a bank are stored | 0x80 as in 0.3.0 (the loader skips the data area: bsp/loader/ldr_core.c, but kept
 * anyway: no "AT" can read at 0xF06). Every erase and write goes through sf_erase / sf_write, which keep to
 * APP_STORE_OK (tests/store_window_test.c, tests/run_tests.sh). */

#define SF_DIR0 SA_STORE_LO
#define SF_POOL (SA_STORE_LO + 0x2000u)
#define SF_NPOOL ((SA_STORE_HI - SF_POOL) / 0x1000u)
#define SF_MAGIC 0x31444652u                 /* "RFD1" */
#define SF_UB 16u                            /* user banks */
#define SF_NL 16u                            /* loops (sa_loop.c SA_LOOPS) */
#define SF_LSEC 7u                           /* a loop's sectors at most (sa_loop.c SA_LPOOL / 512) */
#define SF_EVS 512u                          /* a loop's events a sector */
#define SF_CHUNK 1024u                       /* bytes written a step */
#define SF_GAP_MS 10u                        /* a step at most this often */
#define SF_NONE 0xFFu
_Static_assert(SF_NPOOL == 64u, "64 pool sectors");
_Static_assert(SF_UB + 2u * (SF_LSEC + SF_NL) <= SF_NPOOL,
               "the pool holds every bank, the loops at their largest and a whole new copy of them");

typedef struct {
    uint8_t sec, rsv[3];                     /* the pool sector (SF_NONE: no bank) */
    uint32_t arr;                            /* its arrival (the directory's count) */
    uint32_t crc;                            /* CRC-32 (zlib's) of the 4,096 voice bytes as sent (7 bits) */
    char name[12];                           /* 10 characters, spaces after */
} sf_bank_t;
typedef struct {
    uint16_t n;                              /* events */
    uint8_t bars, nsec;
    uint32_t len;                            /* ms; 0: empty */
    uint16_t bpm, rsv;
    uint32_t crc;                            /* of its n x 8 event bytes */
    uint8_t sec[8];
} sf_loop_t;
typedef struct {
    uint32_t magic, seq, arr;
    uint8_t mig, lcur, rsv[2];               /* mig: 0.3.0's banks taken over (or none there) */
    sf_bank_t b[SF_UB];
    sf_loop_t l[SF_NL];
    uint32_t crc;                            /* of everything before it */
} sf_dir_t;
_Static_assert(sizeof(sf_bank_t) == 24u && sizeof(sf_loop_t) == 24u && sizeof(sf_dir_t) == 788u, "the directory");

enum { SF_IDLE, SF_BANK, SF_LOOPS, SF_DIRJOB };
enum { SF_P_DATA, SF_P_NEXT, SF_P_DERASE, SF_P_DPROG };
static struct {
    sf_dir_t d;                              /* the directory in charge (the one a job builds: sf_nd) */
    uint8_t at;                              /* the sector holding d (0, 1); the next goes to the other */
    uint8_t job, phase, slot, lk, li, from;  /* the job, its phase, its bank slot / loop, the sector, a bank's source
                                              * (1 arrived, 2 0.3.0's) */
    uint8_t rot;                             /* the pool's cursor */
    uint8_t loops_req;                       /* SAVE held: the loops to be saved */
    uint8_t mig_mask;                        /* 0.3.0's banks (XDATA0) still to take over */
    uint8_t erased;                          /* the current sector is erased */
    uint32_t off;                            /* bytes of it written */
    const uint8_t *src;                      /* a bank's 4,096 bytes (RAM) */
    sf_loop_t le;                            /* the loop being written */
    uint32_t last_ms;
    uint32_t ops, erases, progs, commits, same, full, refused, aborted, loads, lbad;   /* (the simulator's checks) */
    uint32_t pass_ops, pass_max, pass_bytes_max, pass_us_max;
    uint32_t cut, dead;                      /* (the simulator: a power cut after cut operations) */
} sf;
static sf_dir_t sf_nd PLAT_POOL;             /* the directory a job builds (the pool: .bss is nearly full) */

static uint32_t sf_crc(const uint8_t *p, uint32_t n, uint32_t c, uint32_t mask)   /* CRC-32, zlib's (p[i] & mask) */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    c = ~c;
    while (n--) {
        c ^= *p++ & mask;
        c = c >> 4 ^ T[c & 15u];
        c = c >> 4 ^ T[c & 15u];
    }
    return ~c;
}

/* the flash: only inside APP_STORE_OK */
static int sf_power_gone(void)               /* (the simulator: a power cut after sf.cut more operations) */
{
    if (sf.cut && !--sf.cut)
        sf.dead = 1;
    return sf.dead != 0;
}
static int sf_erase(uint32_t off)
{
    sf.ops++, sf.pass_ops++;
    if (sf_power_gone())
        return -9;
    if (!APP_STORE_OK(off, 0x1000u)) {
        sf.refused++;
        return -8;
    }
    sf.erases++;
    return plat_data_erase(off);
}
static int sf_write(uint32_t off, const void *src, uint32_t n)
{
    sf.ops++, sf.pass_ops++;
    if (n > sf.pass_bytes_max)
        sf.pass_bytes_max = n;
    if (sf_power_gone())
        return -9;
    if (!APP_STORE_OK(off, n)) {
        sf.refused++;
        return -8;
    }
    sf.progs++;
    return plat_data_write(off, src, n);
}

static uint32_t sf_sec(uint32_t s) { return SF_POOL + 0x1000u * s; }

static int sf_dir_ok(const sf_dir_t *d)
{
    uint32_t k, j;
    if (d->magic != SF_MAGIC || d->crc != sf_crc((const uint8_t *)d, sizeof *d - 4u, 0, 0xFFu))
        return 0;
    for (k = 0; k < SF_UB; k++)
        if (d->b[k].sec != SF_NONE && d->b[k].sec >= SF_NPOOL)
            return 0;
    for (k = 0; k < SF_NL; k++) {
        if (d->l[k].nsec > SF_LSEC || d->l[k].n > d->l[k].nsec * SF_EVS)
            return 0;
        for (j = 0; j < d->l[k].nsec; j++)
            if (d->l[k].sec[j] >= SF_NPOOL)
                return 0;
    }
    return 1;
}

static void sf_init(void)                    /* the directory in charge (none: an empty one) */
{
    const uint8_t *a = plat_data(SF_DIR0), *b = plat_data(SF_DIR0 + 0x1000u);
    sf_dir_t x, y;
    int va = 0, vb = 0;
    uint32_t k;
    memset(&sf, 0, sizeof sf);
    if (a) {
        memcpy(&x, a, sizeof x);
        va = sf_dir_ok(&x);
    }
    if (b) {
        memcpy(&y, b, sizeof y);
        vb = sf_dir_ok(&y);
    }
    if (va && (!vb || (int32_t)(x.seq - y.seq) > 0)) {
        sf.d = x;
        sf.at = 0;
    } else if (vb) {
        sf.d = y;
        sf.at = 1;
    } else {
        memset(&sf.d, 0, sizeof sf.d);
        for (k = 0; k < SF_UB; k++)
            sf.d.b[k].sec = SF_NONE;
        sf.at = 1;                           /* (the first goes to 0x97000) */
    }
    sf.rot = (uint8_t)(sf.d.seq * 7u % SF_NPOOL);
}

/* a free pool sector (used by neither the directory in charge nor the one being built), from the cursor */
static int sf_used(const sf_dir_t *d, uint32_t s)
{
    uint32_t k, j;
    for (k = 0; k < SF_UB; k++)
        if (d->b[k].sec == s)
            return 1;
    for (k = 0; k < SF_NL; k++)
        for (j = 0; j < d->l[k].nsec; j++)
            if (d->l[k].sec[j] == s)
                return 1;
    return 0;
}
static int32_t sf_alloc(const uint8_t *taken, uint32_t ntaken)
{
    uint32_t i, s, j;
    for (i = 0; i < SF_NPOOL; i++) {
        s = (sf.rot + i) % SF_NPOOL;
        if (sf_used(&sf.d, s) || sf_used(&sf_nd, s))
            continue;
        for (j = 0; j < ntaken && taken[j] != s; j++)
            ;
        if (j < ntaken)
            continue;
        sf.rot = (uint8_t)((s + 1u) % SF_NPOOL);
        return (int32_t)s;
    }
    return -1;
}

static int sf_busy(void) { return sf.job != SF_IDLE; }

static void sf_begin(uint32_t job)
{
    sf_nd = sf.d;
    sf.job = (uint8_t)job;
    sf.off = 0;
    sf.erased = 0;
    sf.li = 0;
}

/* a bank (4,096 bytes in RAM, stored as they are) into user slot `slot` */
static int sf_bank_begin(uint32_t slot, const uint8_t *src, uint32_t crc, const char *name, uint32_t from)
{
    int32_t s;
    if (sf_busy() || slot >= SF_UB)
        return -1;
    sf_begin(SF_BANK);
    if ((s = sf_alloc(0, 0)) < 0) {
        sf.job = SF_IDLE;
        sf.full++;
        return -1;
    }
    sf.slot = (uint8_t)slot;
    sf.src = src;
    sf.from = (uint8_t)from;
    sf_nd.b[slot].sec = (uint8_t)s;          /* (and its old sector free once this is in charge) */
    sf_nd.b[slot].arr = ++sf_nd.arr;
    sf_nd.b[slot].crc = crc;
    memcpy(sf_nd.b[slot].name, name, 12);
    sf.phase = SF_P_DATA;
    return 0;
}

static int sf_loops_begin(void)              /* every changed loop, then the directory */
{
    if (sf_busy())
        return -1;
    sf_begin(SF_LOOPS);
    sf.lk = 0;
    sf.phase = SF_P_NEXT;
    return 0;
}

static int sf_dir_begin(void)                /* the directory alone (a bank's name) */
{
    if (sf_busy())
        return -1;
    sf_begin(SF_DIRJOB);
    sf.phase = SF_P_DERASE;
    return 0;
}

/* (sa_loop.c, sa_ubank.c) */
static uint32_t sl_store_meta(uint32_t k, sf_loop_t *e);   /* loop k's length, tempo, bars, count, CRC: its events */
static const uint8_t *sl_store_bytes(uint32_t k);          /* loop k's events as they are now */
static uint32_t sl_store_cur(void);
static void sa_ub_done(uint32_t from, int ok);
static void sa_ub_dir_done(int ok);
static int sf_mig_left(void);

static int sf_loop_same(const sf_loop_t *a, const sf_loop_t *b)
{
    return a->n == b->n && a->len == b->len && a->bpm == b->bpm && a->bars == b->bars && a->crc == b->crc;
}

static void sf_abort(void)
{
    uint32_t from = sf.job == SF_BANK ? sf.from : 0u, dir = sf.job == SF_DIRJOB;
    sf.aborted++;
    sf.job = SF_IDLE;
    if (from)
        sa_ub_done(from, 0);
    if (dir)
        sa_ub_dir_done(0);
}

/* the main loop, every millisecond: one step of the job, when one runs; 1: a bank was just put in charge */
static int sf_step(uint32_t now)
{
    if (sf.job == SF_IDLE || now - sf.last_ms < SF_GAP_MS)
        return 0;
    for (;;) {
        if (sf.phase == SF_P_NEXT) {         /* the next loop that changed (one loop looked at a step) */
            sf_loop_t e;
            uint32_t n, k = sf.lk, j;
            if (k >= SF_NL) {
                sf_nd.lcur = (uint8_t)sl_store_cur();
                sf_nd.mig = (uint8_t)!sf_mig_left();
                if (!memcmp(&sf_nd, &sf.d, sizeof sf.d - 4u)) {
                    sf.same++;               /* nothing changed: nothing written */
                    sf.job = SF_IDLE;
                    return 0;
                }
                sf.phase = SF_P_DERASE;
                continue;
            }
            n = sl_store_meta(k, &e);
            if (sf_loop_same(&e, &sf.d.l[k])) {
                sf.lk++;
                return 0;
            }
            e.nsec = (uint8_t)((n + SF_EVS - 1u) / SF_EVS);
            for (j = 0; j < e.nsec; j++) {
                int32_t s = sf_alloc(e.sec, j);
                if (s < 0)
                    break;
                e.sec[j] = (uint8_t)s;
            }
            if (j < e.nsec) {                /* (no room: cannot be with SF_NPOOL; the old copy stays) */
                sf.full++;
                sf.lk++;
                return 0;
            }
            sf.le = e;
            sf.li = 0;
            sf.off = 0;
            sf.erased = 0;
            if (!e.nsec) {                   /* empty, or a length without events: the directory only */
                sf_nd.l[k] = e;
                sf.lk++;
                return 0;
            }
            sf.phase = SF_P_DATA;
            continue;
        }
        break;
    }
    sf.last_ms = now;
    if (sf.phase == SF_P_DATA) {
        uint32_t sec, total, n;
        const uint8_t *src;
        if (sf.job == SF_BANK) {
            sec = sf_nd.b[sf.slot].sec;
            total = 4096u;
            src = sf.src;
        } else {
            sf_loop_t now_e;
            sec = sf.le.sec[sf.li];
            total = (uint32_t)sf.le.n * 8u - (uint32_t)sf.li * SF_EVS * 8u;
            if (total > 4096u)
                total = 4096u;
            if (sl_store_meta(sf.lk, &now_e) != sf.le.n || !sf_loop_same(&now_e, &sf.le)) {
                sf.lk++;                     /* (it changed while being written: kept as it was; a SAVE again) */
                sf.phase = SF_P_NEXT;
                return 0;
            }
            src = sl_store_bytes(sf.lk) + (uint32_t)sf.li * SF_EVS * 8u;
        }
        if (!sf.erased) {                    /* the sector erased: one step */
            sf.erased = 1;
            if (sf_erase(sf_sec(sec)) != 0)
                sf_abort();
            return 0;
        }
        n = total - sf.off < SF_CHUNK ? total - sf.off : SF_CHUNK;
        if (sf_write(sf_sec(sec) + sf.off, src + sf.off, n) != 0) {   /* SF_CHUNK bytes of it: a step */
            sf_abort();
            return 0;
        }
        sf.off += n;
        if (sf.off < total)
            return 0;
        sf.off = 0;
        sf.erased = 0;
        if (sf.job == SF_BANK) {
            sf.phase = SF_P_DERASE;
        } else if (++sf.li == sf.le.nsec) {
            sf_nd.l[sf.lk++] = sf.le;
            sf.phase = SF_P_NEXT;
        }
        return 0;
    }
    if (sf.phase == SF_P_DERASE) {
        if (sf.job != SF_LOOPS)
            sf_nd.mig = (uint8_t)!sf_mig_left();
        sf.phase = SF_P_DPROG;
        if (sf_erase(SF_DIR0 + 0x1000u * (sf.at ^ 1u)) != 0)
            sf_abort();
        return 0;
    }
    /* SF_P_DPROG: the new directory, its CRC last in it: it is in charge */
    sf_nd.magic = SF_MAGIC;
    sf_nd.seq = sf.d.seq + 1u;
    sf_nd.crc = sf_crc((const uint8_t *)&sf_nd, sizeof sf_nd - 4u, 0, 0xFFu);
    if (sf_write(SF_DIR0 + 0x1000u * (sf.at ^ 1u), &sf_nd, sizeof sf_nd) != 0) {
        sf_abort();
        return 0;
    }
    sf.d = sf_nd;
    sf.at ^= 1u;
    sf.commits++;
    {
        uint32_t job = sf.job, from = sf.from;
        sf.job = SF_IDLE;
        if (job == SF_BANK) {
            sa_ub_done(from, 1);
            return from == 1u;
        }
        if (job == SF_DIRJOB)
            sa_ub_dir_done(1);
    }
    return 0;
}

/* the main loop's call: the step, and (the simulator's measure) the most flash operations in one pass and the
 * longest pass's time on the host */
#ifdef PLAT_HOST
#include <time.h>
#endif
static int sf_ms(uint32_t now)
{
    int r;
#ifdef PLAT_HOST
    struct timespec t0, t1;
    uint32_t us;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    sf.pass_ops = 0;
    r = sf_step(now);
    if (sf.pass_ops > sf.pass_max)
        sf.pass_max = sf.pass_ops;
#ifdef PLAT_HOST
    clock_gettime(CLOCK_MONOTONIC, &t1);
    us = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L);
    if (us > sf.pass_us_max)
        sf.pass_us_max = us;
#endif
    return r;
}
