/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The user's DX7 banks (STANDALONE-PLAN.md S5, question 3): a DX7 32-voice bulk dump (F0 43 0n 09 20 00, 4,096 bytes
 * of voices, the checksum, F7) sent to the unit over USB or Bluetooth MIDI, from any librarian, is kept in the flash's
 * data area (sa_store.c, 0.3.1): up to 16 banks, USER1..USER16, listed after the ROM cartridges (GLO + PRESETS). A 17th
 * arriving replaces the oldest. A host asks what is kept with SA_INFO (docs/PROTOCOL.md section 8: sa_info_cmd).
 * Part of sa_main.c (its state, sa).
 *
 * 0.3.0 kept up to three in XDATA0 (0x93000, 0x94000, 0x95000, the voices | 0x80; their headers in 0x96000: "RFUB",
 * the arrival's count, the CRC-32 of the sector's bytes). 0.3.1 reads them there and takes them over into the data
 * area, oldest first (sa_ub_poll: one at a time, written as an arriving bank is); it never writes XDATA0. */

#define SA_UB_N SF_UB
#define SA_UB_OLD_DATA 0x93000u              /* 0.3.0's bank k at SA_UB_OLD_DATA + 0x1000 k */
#define SA_UB_OLD_HDR 0x96000u
#define SA_UB_OLD_N 3u
#define SA_UB_MAGIC 0x42554652u              /* "RFUB" */
#define SA_UB_LEN 4104u                      /* the dump: 6 + 4096 + 2 */
#define SA_UB_TGT_MS 10000u                  /* SA_INFO TARGET: the dump must have ended within this */

/* SA_INFO statuses (docs/CART-UPLOAD.md) */
enum { SA_ST_OK, SA_ST_BADDUMP, SA_ST_BADREQ, SA_ST_BUSY, SA_ST_SESSION, SA_ST_FLASH };
#define SA_ACK_N 4u

typedef struct {
    uint32_t magic;
    uint8_t seq, rsv[3];
    uint32_t crc;
    uint8_t pad[20];
} sa_ubh_t;                                  /* (0.3.0's header) */
_Static_assert(sizeof(sa_ubh_t) == 32u, "32 bytes a header");

static uint8_t sa_ub_rx[SA_UB_LEN] PLAT_POOL;   /* a dump coming in (and the bank being written: its 4,096 bytes) */
static struct {
    uint32_t n;                              /* its bytes so far */
    uint8_t st;                              /* 0 none, 1 collecting, 2 too long: dropped */
    volatile uint8_t ready;                  /* 1: a whole one, for the main loop; 2: a 0.3.0 bank being taken over
                                              * (either: sa_ub_rx is in use, a dump meanwhile is dropped) */
    uint8_t valid[SA_UB_N];
    uint8_t vbuf[2][128];                    /* voices read back (|0x80 undone), two at a time */
    uint8_t vb;
    uint8_t name_slot;                       /* SA_INFO NAME: its slot (SF_NONE: none waiting) */
    char name[12];
    uint8_t hdr[4];                          /* a dump's first bytes (also while not collected) */
    uint8_t drop;                            /* the dump coming in is not kept: SA_ST_BUSY / SA_ST_SESSION; 0 kept */
    uint8_t rx_t, rx_slot;                   /* its transport; the slot it goes to (SF_NONE: the next free one) */
    volatile uint8_t tgt;                    /* SA_INFO TARGET: the slot armed (SF_NONE: none) */
    uint32_t tgt_ms;
    volatile uint8_t bx_st, bx_slot, bx_t;   /* a STORED from the byte collector (bx_st 0: none), for the main loop */
    uint8_t del_slot, del_t, del_run;        /* SA_INFO DELETE: waiting (SF_NONE: none); its transport; being written */
    uint8_t moved;                           /* a bank deleted: the bank played re-found (sa_main.c sa_ms) */
    uint8_t aq[SA_ACK_N][4], aq_n;           /* replies sent later: op slot status transport (sa_ub_ack) */
    uint8_t last_ack[3];
    uint32_t acks;
    uint32_t got, bad, writes, migrated;     /* (the simulator's checks) */
} ub;

static void sa_ub_scan(void)                 /* which banks are there (the directory's, their CRC good) */
{
    uint32_t k;
    for (k = 0; k < SA_UB_N; k++) {
        const uint8_t *d = sf.d.b[k].sec != SF_NONE ? plat_data(sf_sec(sf.d.b[k].sec)) : 0;
        ub.valid[k] = d && sf.d.b[k].crc == sf_crc(d, 4096u, 0, 0x7Fu);
    }
}

static uint32_t sa_ub_count(void)
{
    uint32_t k, n = 0;
    for (k = 0; k < SA_UB_N; k++)
        n += ub.valid[k];
    return n;
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
    const uint8_t *d = plat_data(sf_sec(sf.d.b[slot].sec) + 128u * (voice & 31u));
    uint8_t *v = ub.vbuf[ub.vb ^= 1u];
    uint32_t i;
    for (i = 0; i < 128u; i++)
        v[i] = d ? (uint8_t)(d[i] & 0x7Fu) : 0u;
    return v;
}

/* a reply sent later (STORED, DELETE's): queued for the main loop (rfm_main.c sends it on its transport) */
static void sa_ub_ack_put(uint32_t op, uint32_t slot, uint32_t st, uint32_t t)
{
    uint8_t *q;
    ub.last_ack[0] = (uint8_t)op, ub.last_ack[1] = (uint8_t)slot, ub.last_ack[2] = (uint8_t)st;
    ub.acks++;
    if (ub.aq_n == SA_ACK_N)
        return;
    q = ub.aq[ub.aq_n++];
    q[0] = (uint8_t)op, q[1] = (uint8_t)slot, q[2] = (uint8_t)st, q[3] = (uint8_t)t;
}
static int sa_ub_ack(uint32_t *t, uint8_t *body)   /* (rfm_main.c) the next reply waiting: its transport, 3 bytes */
{
    if (!ub.aq_n)
        return 0;
    memcpy(body, ub.aq[0], 3);
    *t = ub.aq[0][3];
    {                                        /* (forward copy: no memmove in the USB-only build) */
        uint8_t *d = ub.aq[0], *s = ub.aq[1];
        uint32_t i, n = 4u * --ub.aq_n;
        for (i = 0; i < n; i++)
            d[i] = s[i];
    }
    return 1;
}

static uint32_t sa_ub_free_slot(void);

/* every SysEx byte from USB (t RF_T_SX) or Bluetooth MIDI (t 2), standalone (on) or not (src/rfm_main.c): a DX7 bulk
 * dump collected; one not kept (busy, a session, a bad size) answered with a STORED from the main loop */
static void sa_ub_byte(uint32_t b, uint32_t t, int on)
{
    if (b == 0xF0u) {
        ub.n = 0;
        ub.st = 1;
        ub.rx_t = (uint8_t)t;
        ub.drop = ub.ready ? SA_ST_BUSY : !on ? SA_ST_SESSION : 0u;   /* (one waits or is being written: dropped) */
    }
    if ((b & 0x80u) && b != 0xF0u && b != 0xF7u)
        ub.st = 0;                           /* (a status ends it) */
    if (ub.st == 3u && b == 0xF7u) {         /* a DX7 bank too long */
        ub.st = 0;
        b = SA_ST_BADDUMP;
        goto report;
    }
    if (ub.st != 1u)
        return;
    if (ub.n == SA_UB_LEN) {
        ub.st = 3;
        return;
    }
    if (ub.n < 4u)
        ub.hdr[ub.n] = (uint8_t)b;
    if (!ub.drop)
        sa_ub_rx[ub.n] = (uint8_t)b;
    ub.n++;
    if (b == 0xF7u) {
        ub.st = 0;
        b = ub.n != SA_UB_LEN || (ub.hdr[2] & 0xF0u) ? SA_ST_BADDUMP : ub.drop;
        if (!b) {
            uint8_t g = ub.tgt;
            ub.rx_slot = g != SF_NONE && plat_ms() - ub.tgt_ms < SA_UB_TGT_MS ? g : SF_NONE;
            ub.tgt = SF_NONE;
            ub.ready = 1;
            return;
        }
        goto report;
    } else if (ub.n == 4u && (ub.hdr[1] != 0x43u || ub.hdr[3] != 0x09u)) {
        ub.st = 2;                           /* (not a DX7 bank: not collected, not answered) */
    }
    return;
report:                                      /* a DX7 bank not kept: STORED says why (the TARGET used up) */
    if (!ub.bx_st) {
        uint8_t g = ub.tgt;
        ub.bx_slot = g != SF_NONE && plat_ms() - ub.tgt_ms < SA_UB_TGT_MS ? g : SF_NONE;
        ub.bx_t = (uint8_t)t;
        ub.bx_st = (uint8_t)b;
    }
    ub.tgt = SF_NONE;
}

static void sa_ub_name_of(const uint8_t *v, char *name)   /* a bank's first name: its first voice's (10 characters) */
{
    uint32_t i;
    memset(name, 0, 12);
    for (i = 0; i < 10u; i++) {
        uint32_t c = v[118u + i] & 0x7Fu;
        name[i] = (char)(c >= 0x20u && c < 0x7Fu ? c : 0x20u);
    }
}

static uint32_t sa_ub_free_slot(void)        /* the first slot with no bank, else the oldest bank's */
{
    uint32_t k, oldest = 0;
    for (k = 0; k < SA_UB_N; k++)
        if (!ub.valid[k])
            return k;
    for (k = 1; k < SA_UB_N; k++)
        if ((int32_t)(sf.d.b[k].arr - sf.d.b[oldest].arr) < 0)
            oldest = k;
    return oldest;
}

/* 0.3.0's banks in XDATA0 (header and CRC good) not among the banks kept: bit k */
static uint32_t sa_ub_old(sa_ubh_t *h)
{
    uint32_t k, j, m = 0;
    for (k = 0; k < SA_UB_OLD_N; k++) {
        const uint8_t *p = plat_data(SA_UB_OLD_HDR + 32u * k), *d = plat_data(SA_UB_OLD_DATA + 0x1000u * k);
        uint32_t c7;
        if (!p || !d)
            continue;
        memcpy(&h[k], p, sizeof h[k]);
        if (h[k].magic != SA_UB_MAGIC || h[k].crc != sf_crc(d, 4096u, 0, 0xFFu))
            continue;
        c7 = sf_crc(d, 4096u, 0, 0x7Fu);
        for (j = 0; j < SA_UB_N && !(ub.valid[j] && sf.d.b[j].crc == c7); j++)
            ;
        if (j == SA_UB_N)
            m |= 1u << k;
    }
    return m;
}

static int sf_mig_left(void)                 /* (sa_store.c: the directory's mig) */
{
    return !sf.d.mig && (sf.mig_mask || ub.ready == 2u);
}

static void sa_ub_mig_scan(void)             /* (at boot, after sa_ub_scan) */
{
    sa_ubh_t h[SA_UB_OLD_N];
    sf.mig_mask = sf.d.mig ? 0u : (uint8_t)sa_ub_old(h);
}

/* the main loop: a dump that came in checked and its writing started; or a bank's new name; or one of 0.3.0's banks
 * taken over (one at a time, while the store is free, the oldest first) */
static void sa_ub_poll(void)
{
    uint32_t i, sum = 0, k;
    char name[12];
    if (ub.bx_st) {                          /* a dump not kept (sa_ub_byte): STORED */
        sa_ub_ack_put(4, ub.bx_slot != SF_NONE ? ub.bx_slot : sa_ub_free_slot(), ub.bx_st, ub.bx_t);
        ub.bx_st = 0;
    }
    if (sf_busy())
        return;
    if (ub.ready == 1u) {
        uint32_t slot = ub.rx_slot != SF_NONE ? ub.rx_slot : sa_ub_free_slot();
        for (i = 0; i < 4096u; i++)
            sum += sa_ub_rx[6u + i];
        if (sa_ub_rx[4] != 0x20u || sa_ub_rx[5] != 0x00u || ((sum + sa_ub_rx[4102]) & 0x7Fu) != 0u) {
            ub.bad++;
            ub.ready = 0;
            sa_ub_ack_put(4, slot, SA_ST_BADDUMP, ub.rx_t);
            return;
        }
        for (i = 0; i < 4096u; i++)          /* (stored | 0x80: sa_ub_vmem undoes it) */
            sa_ub_rx[6u + i] |= 0x80u;
        sa_ub_name_of(sa_ub_rx + 6, name);
        ub.got++;
        if (sf_bank_begin(slot, sa_ub_rx + 6, sf_crc(sa_ub_rx + 6, 4096u, 0, 0x7Fu), name, 1) != 0) {
            ub.ready = 0;
            sa_ub_ack_put(4, slot, SA_ST_FLASH, ub.rx_t);
        }
        return;
    }
    if (ub.name_slot != SF_NONE) {           /* SA_INFO NAME */
        if (ub.valid[ub.name_slot] && sf_dir_begin() == 0)
            memcpy(sf_nd.b[ub.name_slot].name, ub.name, 12);
        ub.name_slot = SF_NONE;
        return;
    }
    if (ub.del_slot != SF_NONE) {            /* SA_INFO DELETE: the directory without the bank (its sector free once
                                              * that directory is in charge: a power cut before leaves the bank) */
        k = ub.del_slot;
        if (!ub.valid[k] || sf_dir_begin() != 0) {
            sa_ub_ack_put(3, k, ub.valid[k] ? SA_ST_FLASH : SA_ST_BADREQ, ub.del_t);
            ub.del_slot = SF_NONE;
            return;
        }
        memset(&sf_nd.b[k], 0, sizeof sf_nd.b[k]);
        sf_nd.b[k].sec = SF_NONE;
        ub.del_run = 1;
        return;
    }
    if (!ub.ready && ub.st != 1u && sf.mig_mask && !sf.d.mig) {
        sa_ubh_t h[SA_UB_OLD_N];
        uint32_t m = sa_ub_old(h), oldest = SA_UB_OLD_N;
        for (k = 0; k < SA_UB_OLD_N; k++)    /* the oldest first: they keep their order */
            if ((m >> k & 1u) && (oldest == SA_UB_OLD_N || (uint8_t)(h[k].seq - h[oldest].seq) >= 0x80u))
                oldest = k;
        sf.mig_mask = (uint8_t)m;
        if (oldest == SA_UB_OLD_N)
            return;
        ub.ready = 2;
        memcpy(sa_ub_rx + 6, plat_data(SA_UB_OLD_DATA + 0x1000u * oldest), 4096u);
        sf.mig_mask = (uint8_t)(m & ~(1u << oldest));
        sa_ub_name_of(sa_ub_rx + 6, name);
        if (sf_bank_begin(sa_ub_free_slot(), sa_ub_rx + 6, sf_crc(sa_ub_rx + 6, 4096u, 0, 0x7Fu), name, 2) != 0)
            ub.ready = 0;
    }
}

static void sa_ub_done(uint32_t from, int ok)   /* (sa_store.c) the bank's writing ended: in charge, or not */
{
    if (ok) {
        ub.writes++;
        ub.migrated += from == 2u;
    }
    if (from == 1u)                          /* STORED: in charge (the bank is there), or the flash failed */
        sa_ub_ack_put(4, sf.slot, ok ? SA_ST_OK : SA_ST_FLASH, ub.rx_t);
    ub.ready = 0;
    sa_ub_scan();
}

static void sa_ub_dir_done(int ok)           /* (sa_store.c) a directory alone written (a NAME, a DELETE), or not */
{
    if (ub.del_run) {
        sa_ub_ack_put(3, ub.del_slot, ok ? SA_ST_OK : SA_ST_FLASH, ub.del_t);
        ub.del_run = 0;
        ub.del_slot = SF_NONE;
        ub.moved = 1;
    }
    sa_ub_scan();
}

/* SA_INFO (docs/PROTOCOL.md section 8): a command's body -> its reply's; its length */
#define SA_INFO_REPLY (6u + 19u * SA_UB_N)
static void sa_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
/* (t: the transport it came on; on: standalone. A reply of 0 bytes: none now, it comes later: DELETE's) */
static uint32_t sa_info_cmd(const uint8_t *b, uint32_t len, uint8_t *out, uint32_t t, int on)
{
    uint32_t op = len ? b[0] : 0u, k, n = 0;
    out[0] = (uint8_t)op;
    if (op == 0u) {                          /* BANKS: the most, how many, busy, the slot the next goes to, the loops;
                                              * each bank kept */
        out[1] = SA_UB_N;
        out[3] = (uint8_t)(sf_busy() || ub.ready || sf.loops_req || ub.name_slot != SF_NONE || ub.del_slot != SF_NONE);
        out[4] = (uint8_t)sa_ub_free_slot();
        out[5] = SF_NL;
        for (k = 0; k < SA_UB_N; k++) {
            uint8_t *p = out + 6u + 19u * n;
            if (!ub.valid[k])
                continue;
            p[0] = (uint8_t)k;
            sa_le32(p + 1, sf.d.b[k].arr);
            sa_le32(p + 5, sf.d.b[k].crc);
            memcpy(p + 9, sf.d.b[k].name, 10);
            n++;
        }
        out[2] = (uint8_t)n;
        return 6u + 19u * n;
    }
    if (op == 1u) {                          /* NAME slot name[1..10]: a bank's name, written a moment after */
        k = len >= 2u ? b[1] : SA_UB_N;
        out[1] = (uint8_t)k;
        if (k >= SA_UB_N || !ub.valid[k] || len < 3u || len > 12u) {
            out[2] = 2;
        } else if (ub.name_slot != SF_NONE) {
            out[2] = 3;
        } else {
            memset(ub.name, 0x20, 10);
            ub.name[10] = ub.name[11] = 0;
            for (n = 0; n + 2u < len; n++)
                ub.name[n] = (char)(b[2u + n] >= 0x20u && b[2u + n] < 0x7Fu ? b[2u + n] : 0x20u);
            ub.name_slot = (uint8_t)k;
            out[2] = 0;
        }
        return 3;
    }
    if (op == 2u || op == 3u) {              /* TARGET slot: the next dump goes to it (replacing what is there);
                                              * DELETE slot: the bank removed (its reply when written) */
        k = len >= 2u ? b[1] : SF_NONE;
        out[1] = (uint8_t)k;
        if (!on)
            out[2] = SA_ST_SESSION;
        else if (k >= SA_UB_N || (op == 3u && !ub.valid[k]))
            out[2] = SA_ST_BADREQ;
        else if (op == 2u) {
            ub.tgt_ms = plat_ms();
            ub.tgt = (uint8_t)k;
            out[2] = SA_ST_OK;
        } else if (ub.del_slot != SF_NONE) {
            out[2] = SA_ST_BUSY;
        } else {
            ub.del_slot = (uint8_t)k;
            ub.del_t = (uint8_t)t;
            return 0;
        }
        return 3;
    }
    out[1] = 2;                              /* (no such op) */
    return 2;
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
    static const char *const U[SA_UB_N] = {"USER1",  "USER2",  "USER3",  "USER4",  "USER5",  "USER6",
                                           "USER7",  "USER8",  "USER9",  "USER10", "USER11", "USER12",
                                           "USER13", "USER14", "USER15", "USER16"};
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
