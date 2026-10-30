/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The CoreMIDI back end end to end, without hardware: the fake unit sits behind a virtual MIDI source and
 * destination of this process (named "rfm-test-<pid>"); the host back end runs with allow_devices = 0, so it sees
 * only those two endpoints and never a real device. The fake's replies go out in packets of at most 61 bytes, so a
 * long SysEx reaches the host split over many packets (and several messages may share one). Checks: discovery over
 * CoreMIDI, frames over SysEx until the screen is exact, LEDs, a panel event. Exit 77 (skipped) without CoreMIDI. */
#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rfm/fake.h"
#include "rfm/macos.h"
#include "rfm/session.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t inbox[1 << 20];
static uint32_t inbox_n;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void slot(uint16_t *f, uint32_t i, uint32_t v)
{
    uint8_t *p = (uint8_t *)&f[i];
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int fails;
#define EXPECT(c)                                                                                                   \
    do {                                                                                                            \
        if (!(c)) {                                                                                                 \
            fails++;                                                                                                \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c);                                        \
        }                                                                                                           \
    } while (0)

static int got_key;
static void on_event(void *ctx, const rfm_event_t *e)
{
    (void)ctx;
    if (e->type == RFM_EV_KEY && e->index == 7 && e->value == 1)
        got_key = 1;
}
static void on_log(void *ctx, const char *m)
{
    (void)ctx;
    if (getenv("RFM_VERBOSE"))
        printf("  %s\n", m);
}

int main(void)
{
    static uint16_t img[240 * 240];
    static uint8_t buf[1 << 20];
    MIDIClientRef client = 0;
    MIDIEndpointRef vsrc = 0, vdst = 0;
    char name[64], err[200];
    rfm_fake_cfg_t fc;
    rfm_fake_t *fake;
    rfm_session_cfg_t sc;
    rfm_session_cb_t cb;
    rfm_session_t *s;
    rfm_mac_cfg_t mc;
    rfm_mac_t *mac;
    rfm_leds_t leds;
    uint32_t t0, i, phase = 0, converged = 0;
    CFStringRef cname;

    snprintf(name, sizeof name, "rfm-test-%d", (int)getpid());
    cname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (MIDIClientCreate(CFSTR("rfm fake unit"), NULL, NULL, &client) != noErr ||
        MIDISourceCreate(client, cname, &vsrc) != noErr ||
        MIDIDestinationCreateWithBlock(client, cname, &vdst, ^(const MIDIPacketList *pl, void *rc) {
            const MIDIPacket *p = &pl->packet[0];
            UInt32 k;
            (void)rc;
            pthread_mutex_lock(&mu);
            for (k = 0; k < pl->numPackets; k++) {
                if (inbox_n + p->length <= sizeof inbox) {
                    memcpy(inbox + inbox_n, p->data, p->length);
                    inbox_n += p->length;
                }
                p = MIDIPacketNext(p);
            }
            pthread_mutex_unlock(&mu);
        }) != noErr) {
        printf("test_coremidi: CoreMIDI unavailable: skipped\n");
        return 77;
    }

    rfm_fake_cfg_default(&fc);
    fc.has_bulk = 0;
    fake = rfm_fake_new(&fc);
    memset(&mc, 0, sizeof mc);
    mc.allow_devices = 0;                               /* never a real device */
    mc.virtual_name = name;
    mc.use_midi = 1;
    mc.log = on_log;
    mac = rfm_mac_new(&mc, err, sizeof err);
    if (!mac) {
        printf("test_coremidi: %s: skipped\n", err);
        return 77;
    }
    rfm_session_cfg_default(&sc);
    sc.transport = RFM_T_MIDI;
    memset(&cb, 0, sizeof cb);
    cb.ctx = mac;
    cb.event = on_event;
    cb.log = on_log;
    cb.rescan = (void (*)(void *))rfm_mac_rescan;
    s = rfm_session_new(&sc, &cb);
    rfm_mac_attach(mac, s);

    for (i = 0; i < 240 * 240; i++)                     /* a UI-ish frame: bands, a gradient strip, specks */
        slot(img, i, i / 240 < 120 ? ((i / 240) / 15) * 0x0841u : i / 240 < 160 ? (i % 240) * 0x0101u : i * 2654435761u >> 16);
    memset(&leds, 0, sizeof leds);
    leds.lit[4] = 0x0A;
    leds.dim[1] = 0x10;
    rfm_session_set_leds(s, &leds);
    rfm_session_set_frame(s, img);

    t0 = now_ms();
    while (now_ms() - t0 < 8000u) {
        uint32_t now = now_ms(), n;
        pthread_mutex_lock(&mu);                        /* host -> fake */
        n = inbox_n;
        memcpy(buf, inbox, n);
        inbox_n = 0;
        pthread_mutex_unlock(&mu);
        if (n)
            rfm_fake_rx_sx(fake, buf, n, now);
        rfm_fake_poll(fake, now);
        n = rfm_fake_take_sx(fake, buf, sizeof buf);    /* fake -> host, in packets of <= 61 bytes */
        for (i = 0; i < n;) {
            uint8_t plb[1024];
            MIDIPacketList *pl = (MIDIPacketList *)plb;
            MIDIPacket *p = MIDIPacketListInit(pl);
            uint32_t c = n - i > 61u ? 61u : n - i;
            p = MIDIPacketListAdd(pl, sizeof plb, p, 0, c, buf + i);
            MIDIReceived(vsrc, pl);
            i += c;
        }
        rfm_mac_pump(mac, now);
        rfm_session_poll(s, now);
        if (phase == 0 && rfm_session_state(s) == RFM_S_ACTIVE) {
            rfm_session_stats_t st;
            rfm_session_stats(s, &st);
            if (!st.pending && !st.in_flight && !memcmp(rfm_fake_shadow(fake), img, sizeof img)) {
                converged = now - t0;
                rfm_fake_key(fake, 7, 1);
                phase = 1;
            }
        } else if (phase == 1 && got_key) {
            rfm_fake_key(fake, 7, 0);                   /* (no note left hanging in whatever else listens) */
            phase = 2;
        } else if (phase == 2) {
            break;
        }
        usleep(500);
    }
    {
        rfm_session_stats_t st;
        rfm_mac_stats_t ms;
        rfm_session_stats(s, &st);
        rfm_mac_stats(mac, &ms);
        printf("test_coremidi: state %s, screen exact after %u ms, %u frames, %u acks, %u body bytes, %u MIDI bytes "
               "in\n", rfm_session_state_name(rfm_session_state(s)), converged, st.frames, st.acks, st.body_bytes,
               ms.rx_bytes);
        EXPECT(rfm_session_state(s) == RFM_S_ACTIVE);
        EXPECT(rfm_session_link_kind(s, rfm_session_frame_link(s)) == RFM_LINK_SX);
        EXPECT(converged > 0);
        EXPECT(rfm_leds_equal(rfm_fake_leds(fake), &leds));
        EXPECT(got_key);
        EXPECT(st.decode_errs == 0 && st.ack_timeouts == 0);
    }
    rfm_session_stop(s, now_ms());
    rfm_mac_free(mac);
    rfm_session_free(s);
    rfm_fake_free(fake);
    MIDIEndpointDispose(vsrc);
    MIDIEndpointDispose(vdst);
    MIDIClientDispose(client);
    CFRelease(cname);
    printf("test_coremidi: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
