/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Host test of SysEx over Bluetooth MIDI in fm1-base's ble/midi/le_midi.c as RemoteFM1 patches it
 * (firmware/patches/fm1-base): its parser (midi_parse: BLE MIDI packets -> USB-MIDI packets for ble_midi_receive) and
 * its encoder (ble_midi_send_many: USB-MIDI packets -> BLE MIDI packets), the code the FM-1 runs. The JieLi SDK is
 * not here, so tests/run_tests.sh cuts the MIDI part out of the patched file (from "---- MIDI in" to "---- ATT") into
 * build/host/le_midi_part.c, and this file gives it the few names it uses.
 *   - every SysEx length 0..400 bytes of body, alone and between notes and clocks, through the encoder at an ATT
 *     payload of 20 and of 64 bytes (a message over many notifications) and back through the parser: whole, in
 *     order, the notes and clocks too; a clock inside a SysEx;
 *   - messages written by another sender: SysEx continued in the next packet (its data right after the header), a
 *     timestamp before each F7, a real-time byte inside, a SysEx cut by a note;
 *   - 50,000 random packets: no crash, no read past the packet, no endless loop (ASan, UBSan). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t hci_con_handle_t;
#define BLE_MIDI_CENTRAL 0
#define H_MIDI 0x0006
#define H_CCC 0x0007
#define ATT_OP_AUTO_READ_CCC 0
static volatile hci_con_handle_t con_handle = 1;
#define CCC_GET(h, att) 1
static u8 sent[64];
static u32 sent_n, sends;
static int SEND_(const u8 *d, u32 n)
{
    if (n > sizeof sent)
        abort();
    memcpy(sent, d, n);
    sent_n = n;
    sends++;
    return 0;
}
#define SEND(h, att, d, n, op) SEND_((d), (n))
static void ble_op_att_get_remain(u32 *r) { *r = 512; }
static void ble_midi_receive(uint32_t pkt, uint32_t ts);
#include "../build/host/le_midi_part.c"

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
static uint32_t rs = 77u;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

/* what the parser hands on: the bytes of the SysEx packets (in order), the other messages as packets */
static uint8_t got[200000];
static uint32_t got_n, other[4096], other_n;
static void ble_midi_receive(uint32_t pkt, uint32_t ts)
{
    uint32_t cin = pkt & 15u, nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u, j;
    (void)ts;
    if (cin >= 4u && cin <= 7u && !(cin == 5u && (pkt >> 8 & 0xFFu) != 0xF7u)) {
        for (j = 0; j < nb; j++)
            got[got_n++] = (uint8_t)(pkt >> (8u * (j + 1u)));
        return;
    }
    other[other_n++ % 4096] = pkt & ~0xF0u;
}

/* the USB-MIDI packets of a message as a USB host sends a SysEx (CIN 4, then 5 / 6 / 7) */
static uint32_t q[100000], qn;
static uint16_t qms[100000];
static void q_sysex(const uint8_t *m, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i += 3u) {
        uint32_t k = n - i < 3u ? n - i : 3u, cin = i + k == n ? 4u + k : 4u;
        q[qn++] = cin | (uint32_t)m[i] << 8 | (k > 1u ? (uint32_t)m[i + 1u] << 16 : 0u) |
                  (k > 2u ? (uint32_t)m[i + 2u] << 24 : 0u);
    }
}
/* the encoder over the queue, every notification it makes through the parser (as the computer would get them) */
static void pump(void)
{
    uint32_t r = 0, i;
    for (i = 0; i < qn; i++)
        qms[i] = (uint16_t)(1000u + i / 40u);           /* (a few milliseconds apart) */
    while (r < qn) {
        int k = ble_midi_send_many(0, q + r, qms + r, (int)(qn - r));
        CHECK(k > 0);
        if (k <= 0)
            break;
        CHECK(sent_n <= send_mtu && sent_n >= 2u && (sent[0] & 0x80u));
        midi_parse(sent, (u16)sent_n, 0);
        r += (uint32_t)k;
    }
    qn = 0;
}

int main(void)
{
    static uint8_t m[500], want[200000];
    uint32_t len, mtu, w, i, n, k;
    /* 1: the encoder and the parser, every length, ATT payload 20 and 64; notes and clocks between the messages */
    for (mtu = 20; mtu <= 64; mtu += 44) {
        send_mtu = (u16)mtu;
        got_n = other_n = w = 0;
        for (len = 0; len <= 400; len++) {
            m[0] = 0xF0;
            for (i = 0; i < len; i++)
                m[1 + i] = (uint8_t)(rnd() & 0x7Fu);
            m[1 + len] = 0xF7;
            q_sysex(m, len + 2u);
            memcpy(want + w, m, len + 2u);
            w += len + 2u;
            if (len % 3u == 0u)
                q[qn++] = 0x09u | 0x90u << 8 | (len & 0x7Fu) << 16 | 100u << 24;   /* a note between */
            if (len % 5u == 0u)
                q[qn++] = 0x0Fu | 0xF8u << 8;                                      /* a clock */
            if (qn > 3000u)
                pump();
        }
        pump();
        CHECK(got_n == w && !memcmp(got, want, w));
        CHECK(other_n == 134u + 81u);
        CHECK(!sx_in[0].on);
    }
    /* 2: a clock inside a SysEx (a sequencer's tick between two of its packets): the SysEx whole, the clock seen */
    send_mtu = 64;
    got_n = other_n = 0;
    for (i = 0; i < 100u; i++)
        m[i] = (uint8_t)(i == 0 ? 0xF0 : i == 99 ? 0xF7 : i & 0x7F);
    for (i = 0; i < 48u; i += 3u)                       /* 48 bytes as CIN 4, a clock, the rest */
        q[qn++] = 4u | (uint32_t)m[i] << 8 | (uint32_t)m[i + 1u] << 16 | (uint32_t)m[i + 2u] << 24;
    q[qn++] = 0x0Fu | 0xF8u << 8;
    q_sysex(m + 48, 52);
    pump();
    CHECK(got_n == 100u && !memcmp(got, m, 100));
    CHECK(other_n == 1u && other[0] == (0x0Fu | 0xF8u << 8));
    /* 3: another sender's packets */
    {
        static const u8 A[] = {0x80, 0x81, 0xF0, 0x7D, 0x52, 0x46, 0x01};          /* a SysEx begun ... */
        static const u8 B[] = {0x80, 0x10, 0x20, 0x30, 0x82, 0xF7};                 /* ... continued, its F7 */
        static const u8 C[] = {0x80, 0x81, 0xF0, 0x01, 0x83, 0xF8, 0x02, 0x84, 0xF7}; /* a clock inside */
        static const u8 D[] = {0x80, 0x81, 0xF0, 0x01, 0x02, 0x82, 0x90, 0x3C, 0x64}; /* cut by a note */
        static const u8 E[] = {0x80, 0x81, 0x90, 0x3C, 0x64, 0x3E, 0x64, 0x82, 0x40, 0x00};   /* running status */
        static const u8 WA[] = {0xF0, 0x7D, 0x52, 0x46, 0x01, 0x10, 0x20, 0x30, 0xF7}, WC[] = {0xF0, 0x01, 0x02, 0xF7};
        got_n = other_n = 0;
        midi_parse(A, sizeof A, 0);
        CHECK(sx_in[0].on);
        midi_parse(B, sizeof B, 0);
        CHECK(!sx_in[0].on && got_n == sizeof WA && !memcmp(got, WA, sizeof WA));
        got_n = 0;
        midi_parse(C, sizeof C, 0);
        CHECK(got_n == sizeof WC && !memcmp(got, WC, sizeof WC) && other_n == 1u);
        got_n = other_n = 0;
        midi_parse(D, sizeof D, 0);
        CHECK(!sx_in[0].on && other_n == 1u && other[0] == (0x09u | 0x90u << 8 | 0x3Cu << 16 | 0x64u << 24));
        CHECK(got_n <= 3u && got[0] == 0xF0);           /* (what went before the note: never an F7) */
        other_n = 0;
        midi_parse(E, sizeof E, 0);
        CHECK(other_n == 3u && other[1] == (0x09u | 0x90u << 8 | 0x3Eu << 16 | 0x64u << 24) &&
              other[2] == (0x09u | 0x90u << 8 | 0x40u << 16));
        midi_parse(A, sizeof A, 0x10u);                 /* the device's link: its own SysEx, apart */
        CHECK(sx_in[1].on && !sx_in[0].on);
        sx_reset(1);
    }
    /* 4: random packets: nothing past the packet, no endless loop */
    for (k = 0; k < 50000u; k++) {
        u8 *p;
        n = 1u + rnd() % 64u;
        p = malloc(n);
        for (i = 0; i < n; i++)
            p[i] = (u8)(rnd() % 3u == 0u ? 0x80u | rnd() : rnd() & 0x7Fu);
        p[0] |= 0x80u;
        got_n = 0;
        midi_parse(p, (u16)n, (rnd() & 1u) ? 0x10u : 0u);
        CHECK(got_n <= 3u * n);
        free(p);
        if (got_n > 100000u)
            got_n = 0;
    }
    printf("ble_sysex_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
