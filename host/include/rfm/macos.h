/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1's macOS back ends: CoreMIDI (SysEx + the panel's plain MIDI) and libusb (the vendor bulk interface,
 * class 0xFF on 1209:0001, EP 0x03 OUT / 0x83 IN). They find candidate links and add them to a session; received
 * bytes queue (CoreMIDI and libusb call back on their own threads) until pump() hands them to the session on the
 * session's thread.
 *
 * SAFETY: nothing here touches a real device unless cfg.allow_devices is set (fm1remote's --device). Without it the
 * CoreMIDI side only considers virtual endpoints named exactly cfg.virtual_name (the tests' loopback) and the USB
 * side refuses to open anything. */
#ifndef RFM_MACOS_H
#define RFM_MACOS_H
#include <stddef.h>
#include <stdint.h>
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int allow_devices;                                  /* 1: real MIDI devices and the USB unit may be opened */
    const char *virtual_name;                           /* also: virtual source + destination with this exact name */
    const char *port_filter;                            /* real devices: only names containing this (any case) */
    int use_midi, use_bulk;                             /* which back ends to run */
    int midi_restart;                                   /* MIDIRestart() first (a port CoreMIDI keeps offline) */
    void (*log)(void *ctx, const char *msg);
    void *log_ctx;
} rfm_mac_cfg_t;

typedef struct {
    uint32_t rx_bytes, rx_dropped, midi_links, bulk_links, usb_errors;
} rfm_mac_stats_t;

typedef struct rfm_mac rfm_mac_t;
rfm_mac_t *rfm_mac_new(const rfm_mac_cfg_t *cfg, char *err, size_t errn);
void rfm_mac_attach(rfm_mac_t *m, rfm_session_t *s);   /* the session the links go to; scans once */
void rfm_mac_rescan(rfm_mac_t *m);                      /* the session's rescan callback (its thread) */
void rfm_mac_pump(rfm_mac_t *m, uint32_t now_ms);       /* queued input -> the session; dead links removed */
void rfm_mac_wait(rfm_mac_t *m, uint32_t timeout_ms);   /* sleeps until input arrives or the timeout */
void rfm_mac_stats(rfm_mac_t *m, rfm_mac_stats_t *st);
void rfm_mac_free(rfm_mac_t *m);                        /* closes everything (call after the session stopped) */

#ifdef __cplusplus
}
#endif
#endif
