/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Host test of usb.c's EP1 OUT parser (ep1_take -> midi_in_event -> sysex_byte -> the app's SysEx hook), the code
 * the device runs, fed the USB-MIDI event packets a long SysEx makes, 64-byte packets at a time, and the bytes the
 * hook saw then through remote_proto.h's parser (a SINK message and its checksum, as M0's bench sent them):
 *   - the class-compliant encoding (CIN 4, then 5 / 6 / 7 with 1 / 2 / 3 bytes) of every length 6..3000;
 *   - a SysEx split the way macOS splits a long one, the odd byte(s) at each split sent as CIN 0xF single bytes;
 *   - realtime bytes (CIN 0xF F8) inside a SysEx, a CIN 0xF data byte outside one, a SysEx cut by a note.
 * Built twice (tests/run_tests.sh): -DUSB_CIN_F_SYSEX as RemoteFM1 builds usb.c, where every message arrives whole,
 * and without it, as TransitFM1 / the update loader build it, where the test checks that exactly the CIN 0xF bytes
 * are lost: M0's "2048 / 8192-byte SINK one byte short, each fails its checksum". */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FELUCCA_OTA 1
static uint32_t hook_n, cinf_n;
static uint8_t hook_b[70000];
static void hook(uint32_t b)
{
    if (hook_n < sizeof hook_b)
        hook_b[hook_n] = (uint8_t)b;
    hook_n++;
}
#define USB_SYSEX_HOOK(b) hook(b)
#ifdef WITH_CIN_F
#define USB_CIN_F_SYSEX() (cinf_n++)
#endif
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../firmware/src/usb.c"
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
                    CHECK(hook_n == n - lone);          /* TransitFM1's parser: the CIN 0xF bytes are gone */
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
        uint32_t bad = usb.rx_bad;
        hook_n = 0;
        ev_put(0xF, 0x12, 0, 0);
        ev_flush();
        CHECK(hook_n == 0 && usb.rx_bad == bad + 1);
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
    printf("usb_midi_test (RemoteFM1, CIN 0xF taken): %d checks, %d failed\n", checks, fails);
#else
    CHECK(st.sink_bytes == 2047 && st.chk == 1);
    printf("usb_midi_test (TransitFM1's parser: M0's lost byte reproduced): %d checks, %d failed\n", checks, fails);
#endif
    return fails ? 1 : 0;
}
