/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Host test of felucca-bsp's usb.c as RemoteFM1 builds it (firmware/patches/felucca-bsp applied: build/fm1-base, made
 * by ./build.sh): its EP1 OUT parser (ep1_take -> midi_in_event -> sysex_byte -> the app's SysEx tap), the code the
 * device runs, fed the USB-MIDI event packets a long SysEx makes, 64-byte packets at a time, and the bytes the tap saw
 * then through remote_proto.h's parser (a SINK message and its checksum, as M0's bench sent them):
 *   - the class-compliant encoding (CIN 4, then 5 / 6 / 7 with 1 / 2 / 3 bytes) of every length 6..3000;
 *   - a SysEx split the way macOS splits a long one, the odd byte(s) at each split sent as CIN 0xF single bytes;
 *   - realtime bytes (CIN 0xF F8) inside a SysEx, a CIN 0xF data byte outside one, a SysEx cut by a note;
 *   - the tap's claim: a message the app claims is never the updater's frame (sx_ready), one it leaves is, whole;
 *   - FELUCCA_EP1_ROOM: 0 leaves the packet (NAK), 1 takes it;
 *   - with FELUCCA_VENDOR (a third build): the descriptors are RemoteFM1 0.1's byte for byte, the vendor rings.
 * Built twice (tests/run_tests.sh): -DWITH_CIN_F as RemoteFM1 builds usb.c (FELUCCA_CIN_F_SYSEX), where every message
 * arrives whole, and without it, as felucca-bsp builds it for every other firmware and the update loader, where the
 * test checks that exactly the CIN 0xF bytes are lost: M0's "2048 / 8192-byte SINK one byte short, each fails its
 * checksum". */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FELUCCA_OTA 1
static uint32_t hook_n, cinf_n, claim_7d, ep1_hold;
static uint8_t hook_b[70000];
static int hook(uint32_t b)
{
    static int st;                                      /* (claim_7d: messages F0 7D .. are the app's) */
    if (hook_n < sizeof hook_b)
        hook_b[hook_n] = (uint8_t)b;
    hook_n++;
    if (b == 0xF0u) {
        st = 1;
        return 0;
    }
    if (st == 1)
        st = b == 0x7Du ? 2 : 3;
    if (b & 0x80u) {
        int r = st == 2;
        st = 0;
        return claim_7d && r;
    }
    return claim_7d && st == 2;
}
#define FELUCCA_SYSEX_TAP(b) hook(b)
#define FELUCCA_EP1_ROOM() (!ep1_hold)
#ifdef WITH_CIN_F
#define FELUCCA_CIN_F_SYSEX (cinf_n++)
#endif
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../build/fm1-base/bsp/chip/usb.c"
static uint32_t ota_now_ms(void) { return 0; }
static void ota_idle(void) {}
#include "../firmware/src/remote_proto.h"

static int fails, checks;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            fails++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            if (fails > 20)                                                      \
                exit(1);                                                         \
        }                                                                        \
    } while (0)

/* event packets -> 64-byte EP1 OUT packets -> ep1_take; the MIDI ring drained after each (remote.c does) */
static uint8_t ev[200000];
static uint32_t nev, realtime_seen;
static void ev_put(uint32_t cin, uint32_t a, uint32_t b, uint32_t c)
{
    ev[nev++] = (uint8_t)cin;
    ev[nev++] = (uint8_t)a;
    ev[nev++] = (uint8_t)b;
    ev[nev++] = (uint8_t)c;
}
static void ev_flush(void)
{
    uint32_t i;
    for (i = 0; i < nev; i += 64) {
        uint32_t n = nev - i < 64 ? nev - i : 64;
        CHECK(ep1_take(ev + i, n) == 1);
        while (mi_r != mi_w) {
            if (((midi_in_q[mi_r % MQ] >> 8) & 0xFFu) == 0xF8u)
                realtime_seen++;
            mi_r++;
        }
    }
    nev = 0;
}

/* class compliant: CIN 4 triples, the end as CIN 5 / 6 / 7 */
static void enc_std(const uint8_t *m, uint32_t n)
{
    uint32_t i = 0;
    while (n - i > 3) {
        ev_put(4, m[i], m[i + 1], m[i + 2]);
        i += 3;
    }
    ev_put(4 + (n - i), m[i], n - i > 1 ? m[i + 1] : 0, n - i > 2 ? m[i + 2] : 0);
}
/* split into chunks of s bytes, each encoded on its own: the 1 or 2 bytes left at a split go as CIN 0xF; a
 * realtime clock between chunks if rt */
static uint32_t enc_split(const uint8_t *m, uint32_t n, uint32_t s, int rt)
{
    uint32_t o = 0, lone = 0;
    while (o < n) {
        uint32_t k = n - o < s ? n - o : s, i = 0;
        if (o + k == n) {
            enc_std(m + o, k);
            break;
        }
        while (k - i >= 3) {
            ev_put(4, m[o + i], m[o + i + 1], m[o + i + 2]);
            i += 3;
        }
        for (; i < k; i++, lone++)
            ev_put(0xF, m[o + i], 0, 0);
        if (rt)
            ev_put(0xF, 0xF8, 0, 0);
        o += k;
    }
    return lone;
}

/* a SINK message as m0_bench.py sends it: seq32 data[size] sum32, 7-in-8, framed */
static uint32_t sink_msg(uint8_t *out, uint32_t seq, uint32_t size)
{
    static uint8_t body[9000];
    uint32_t i;
    rf_wr32(body, seq);
    for (i = 0; i < size; i++)
        body[4 + i] = (uint8_t)(seq * 31u + i * 7u + (i >> 5));
    rf_wr32(body + 4 + size, rf_sum(body + 4, size));
    return rf_sx_encode(RF_SINK, body, size + 8u, out);
}

static rf_tstats_t st;
static void on_msg(void *ctx, uint32_t cmd, const uint8_t *b, uint32_t len) { (void)ctx, (void)cmd, (void)b, (void)len; }

int main(void)
{
    static uint8_t msg[12000];
    static uint8_t pbuf[64];
    uint32_t n, i, size, lone;
    rf_sx_t sx;
    usb.config = 1;
    /* 1: class-compliant packets, every length, whole */
    for (n = 6; n <= 3000; n++) {
        msg[0] = 0xF0;
        for (i = 1; i + 1 < n; i++)
            msg[i] = (uint8_t)((i * 13u + n) & 0x7Fu);
        msg[n - 1] = 0xF7;
        hook_n = 0;
        enc_std(msg, n);
        ev_flush();
        CHECK(hook_n == n && !memcmp(hook_b, msg, n));
    }
    /* 2: SINK messages split as macOS does; 3: with a clock between the pieces */
    {
        static const uint32_t SIZES[] = {32, 128, 512, 2048, 8192}, SPLIT[] = {256, 1024, 2048, 128};
        uint32_t s, k, rt;
        for (rt = 0; rt < 2; rt++)
            for (k = 0; k < 4; k++)
                for (s = 0; s < 5; s++) {
                    size = SIZES[s];
                    n = sink_msg(msg, 7, size);
                    hook_n = cinf_n = realtime_seen = 0;
                    lone = enc_split(msg, n, SPLIT[k], (int)rt);
                    ev_flush();
                    memset(&st, 0, sizeof st);
                    rf_body_init(&sx.b, &st, pbuf, sizeof pbuf, on_msg, NULL);
                    sx.st = 0;
                    for (i = 0; i < hook_n; i++)
                        rf_sx_byte(&sx, hook_b[i]);
#ifdef WITH_CIN_F
                    CHECK(hook_n == n && !memcmp(hook_b, msg, n));
                    CHECK(cinf_n == lone);
                    CHECK(st.sink_msgs == 1 && st.sink_bytes == size && st.chk == 0);
#else
                    CHECK(hook_n == n - lone);          /* felucca-bsp's parser: the CIN 0xF bytes are gone */
                    CHECK(st.sink_msgs == 1 && st.sink_bytes <= size);
                    if (lone)
                        CHECK(st.chk == 1);             /* ... and the message fails its checksum */
                    if (lone == 1)
                        CHECK(st.sink_bytes == size - 1u);   /* M0's symptom: exactly one byte short */
#endif
                    CHECK(realtime_seen == (rt ? (n - 1u) / SPLIT[k] : 0u));
                }
    }
    /* 4: a CIN 0xF data byte outside a SysEx is still ignored; a note ends a SysEx */
    {
        hook_n = 0;
        ev_put(0xF, 0x12, 0, 0);
        ev_flush();
        CHECK(hook_n == 0);
        ev_put(4, 0xF0, 0x7D, 0x52);
        ev_put(0x9, 0x90, 60, 100);                     /* a note in the middle: the SysEx ends */
        ev_put(0xF, 0x33, 0, 0);                        /* so this byte is not a SysEx byte */
        ev_flush();
        CHECK(hook_n == 4 && hook_b[3] == 0x90 && !usb.sx_on);
    }
    /* 5: M0's case, reproduced: a 2048-byte SINK with a single odd byte at one split */
    size = 2048;
    n = sink_msg(msg, 1, size);
    hook_n = cinf_n = 0;
    nev = 0;
    for (i = 0; i + 3 <= 1024; i += 3)                  /* 1024 = 341 * 3 + 1: one byte left at the split */
        ev_put(4, msg[i], msg[i + 1], msg[i + 2]);
    ev_put(0xF, msg[1023], 0, 0);
    enc_std(msg + 1024, n - 1024);
    ev_flush();
    memset(&st, 0, sizeof st);
    rf_body_init(&sx.b, &st, pbuf, sizeof pbuf, on_msg, NULL);
    sx.st = 0;
    for (i = 0; i < hook_n; i++)
        rf_sx_byte(&sx, hook_b[i]);
#ifdef WITH_CIN_F
    CHECK(st.sink_bytes == 2048 && st.chk == 0 && cinf_n == 1);
#else
    CHECK(st.sink_bytes == 2047 && st.chk == 1);
#endif
    /* 6: the tap's claim: the app's message (F0 7D ..) never reaches the updater's frame, the update's (F0 00 59 ..)
     * does, whole, before and after it, and one the app claims while the updater's frame is still taken */
    claim_7d = 1;
    sx_ready = 0;
    {
        static const uint8_t OTA[] = {0xF0, 0x00, 0x59, 0x11, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
        n = sink_msg(msg, 3, 700);                      /* longer than the frame (640): taken whole by the app */
        hook_n = 0;
        enc_std(msg, n);
        ev_flush();
        CHECK(!sx_ready && hook_n == n);
        n = sink_msg(msg, 4, 20);                       /* short: the frame would take it but for the claim */
        enc_std(msg, n);
        ev_flush();
        CHECK(!sx_ready);
        enc_std(OTA, sizeof OTA);
        ev_flush();
        CHECK(sx_ready && sx_frame_len == sizeof OTA - 2u && !memcmp(sx_frame, OTA + 1, sizeof OTA - 2u));
        enc_std(msg, n);                                /* the app's, while the frame is taken: the app gets it */
        hook_n = 0;
        ev_flush();
        CHECK(sx_ready && hook_n == n && sx_frame[1] == 0x59);
        sx_ready = 0;
        enc_std(OTA, sizeof OTA);
        ev_flush();
        CHECK(sx_ready && !memcmp(sx_frame, OTA + 1, sizeof OTA - 2u));
        sx_ready = 0;
    }
    claim_7d = 0;
    /* 7: FELUCCA_EP1_ROOM: 0 leaves the packet for later (NAKed), 1 takes it */
    {
        uint8_t pkt[4] = {0x09, 0x90, 60, 100};
        ep1_hold = 1;
        CHECK(ep1_take(pkt, 4) == 0 && mi_w == mi_r);
        ep1_hold = 0;
        CHECK(ep1_take(pkt, 4) == 1 && mi_w == mi_r + 1u);
        mi_r = mi_w;
    }
#if FELUCCA_VENDOR
    /* 8: the descriptors with the vendor interface: RemoteFM1 0.1's, byte for byte (its usb.c, REMOTE_VENDOR, and the
     * product name "FM-1"), so hosts see the same device; the vendor rings: a message whole or not at all */
    {
        static const uint8_t DEV01[18] = {0x12, 0x01, 0x10, 0x02, 0, 0, 0, 0x40, 0x09, 0x12, 0x01, 0x00, 0x50, 0x03, 1, 2, 0, 1};
        static const uint8_t CFG01[124] = {
            0x09, 0x02, 0x7C, 0x00, 0x03, 0x01, 0x00, 0x80, 0x32, 0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
            0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01, 0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
            0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00, 0x06, 0x24, 0x02, 0x01, 0x01, 0x00, 0x06, 0x24, 0x02, 0x02, 0x02,
            0x00, 0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00, 0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01,
            0x00, 0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x01, 0x09, 0x05, 0x81,
            0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x03, 0x09, 0x04, 0x02, 0x00, 0x02, 0xFF, 0x00,
            0x00, 0x00, 0x07, 0x05, 0x03, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00};
        static const uint8_t BOS01[29] = {0x05, 0x0F, 0x1D, 0x00, 0x01, 0x18, 0x10, 0x05, 0x00, 0x38, 0xB6, 0x08, 0x34,
                                          0xA9, 0x09, 0xA0, 0x47, 0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65, 0x00,
                                          0x01, 0x01, 0x00};
        static const uint8_t STR2_01[10] = {0x0A, 0x03, 'F', 0, 'M', 0, '-', 0, '1', 0};
        static uint8_t big[FELUCCA_VENDOR_OUT_N], rd[64];
        const uint8_t *d;
        uint16_t l;
        CHECK(get_desc(0x0100, &d, &l) && l == 18 && !memcmp(d, DEV01, 18));
        CHECK(get_desc(0x0200, &d, &l) && l == sizeof CFG01 && !memcmp(d, CFG01, sizeof CFG01));
        CHECK(get_desc(0x0F00, &d, &l) && l == sizeof BOS01 && !memcmp(d, BOS01, sizeof BOS01));
        CHECK(get_desc(0x0302, &d, &l) && l == sizeof STR2_01 && !memcmp(d, STR2_01, sizeof STR2_01));
        usb.up = 1;
        CHECK(usb_vendor_room() == FELUCCA_VENDOR_OUT_N);
        CHECK(usb_vendor_send(rd, 10, big, FELUCCA_VENDOR_OUT_N - 9u) == 0 && vo_w == vo_r);   /* one byte too many */
        CHECK(usb_vendor_send(rd, 10, big, 100) == 1 && vo_w - vo_r == 110u);
        usb.config = 0;
        CHECK(usb_vendor_send(rd, 1, big, 1) == -1 && usb_vendor_room() == 0);
        usb.config = 1;
        for (i = 0; i < 70u; i++)
            vb_in[(vi_w + i) & (FELUCCA_VENDOR_IN_N - 1u)] = (uint8_t)i;
        vi_w += 70u;
        CHECK(usb_vendor_avail() == 70u && usb_vendor_read(rd, 64) == 64u && rd[63] == 63 && usb_vendor_avail() == 6u);
    }
#endif
#ifdef WITH_CIN_F
    printf("usb_midi_test (RemoteFM1's usb.c, CIN 0xF taken%s): %d checks, %d failed\n",
           FELUCCA_VENDOR ? ", the vendor interface" : "", checks, fails);
#else
    printf("usb_midi_test (felucca-bsp's parser: M0's lost byte reproduced): %d checks, %d failed\n", checks, fails);
#endif
    return fails ? 1 : 0;
}
