/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Inside the macOS back ends: the receive queue the callback threads fill, the CoreMIDI and libusb pieces. */
#ifndef RFM_MAC_PRIV_H
#define RFM_MAC_PRIV_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the receive queue: records [tag16][len16][bytes], filled by CoreMIDI / libusb threads */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint8_t *buf;
    uint32_t cap, len, dropped, total;
} rxq_t;
int rxq_init(rxq_t *q, uint32_t cap);
void rxq_free(rxq_t *q);
void rxq_push(rxq_t *q, uint16_t tag, const uint8_t *p, uint32_t n);
uint32_t rxq_take(rxq_t *q, uint8_t *out, uint32_t cap);      /* moves every whole record out */
void rxq_wait(rxq_t *q, uint32_t timeout_ms);

/* ---- CoreMIDI */
typedef struct cm cm_t;
typedef struct {
    uint32_t src, dst;                                  /* MIDIEndpointRef */
    int32_t uid;                                        /* the destination's unique ID (identity across scans) */
    char name[64];
} cm_cand_t;
cm_t *cm_open(rxq_t *q, int midi_restart, char *err, size_t errn);
void cm_close(cm_t *c);
/* candidates: real device entities with a source and a destination (allow_devices, filter, "update" ports
 * skipped), and virtual endpoint pairs named exactly virtual_name */
int cm_scan(cm_t *c, cm_cand_t *out, int max, int allow_devices, const char *virtual_name, const char *filter);
int cm_connect(cm_t *c, uint32_t src, uint16_t tag);
void cm_disconnect(cm_t *c, uint32_t src);
int cm_send(cm_t *c, uint32_t dst, const uint8_t *p, uint32_t n);

/* ---- libusb (vendor bulk) */
typedef struct usbl usbl_t;
usbl_t *usb_open_unit(rxq_t *q, uint16_t tag, char *err, size_t errn);   /* 1209:0001 with a class 0xFF interface */
int usb_write(usbl_t *u, const uint8_t *p, uint32_t n);
int usb_dead(const usbl_t *u);
uint32_t usb_errors(const usbl_t *u);
void usb_close(usbl_t *u);
const char *usb_name(const usbl_t *u);

#endif
