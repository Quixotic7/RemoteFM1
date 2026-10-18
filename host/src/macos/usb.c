/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libusb: RemoteFM1's vendor interface (class 0xFF on 1209:0001, as tools/m0_bulk.c finds it). Only that interface
 * is claimed: the MIDI interfaces stay with the system (no configuration change, no reset, no kernel driver
 * detached). Four IN transfers stay queued (asynchronous reads); every write is its own asynchronous OUT transfer,
 * so the session thread never waits on the pipe (the device answers in order: IOKit keeps a pipe's transfers in
 * order). An event thread runs libusb's callbacks. A transfer error other than a cancel marks the link dead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mac_priv.h"

#ifdef RFM_NO_LIBUSB
usbl_t *usb_open_unit(rxq_t *q, uint16_t tag, char *err, size_t errn)
{
    (void)q;
    (void)tag;
    snprintf(err, errn, "built without libusb");
    return NULL;
}
int usb_write(usbl_t *u, const uint8_t *p, uint32_t n)
{
    (void)u;
    (void)p;
    (void)n;
    return -1;
}
int usb_dead(const usbl_t *u) { return u != NULL; }
uint32_t usb_errors(const usbl_t *u) { return u ? 1u : 0u; }
void usb_close(usbl_t *u) { (void)u; }
const char *usb_name(const usbl_t *u) { return u ? "bulk" : ""; }
#else
#include <libusb.h>

#define RF_VID 0x1209u
#define RF_PID 0x0001u
#define NIN 4
#define INSZ (16384 + 64)

struct usbl {
    libusb_context *ctx;
    libusb_device_handle *h;
    int ifnum;
    uint8_t ep_in, ep_out;
    rxq_t *q;
    uint16_t tag;
    pthread_t th;
    int th_started;
    volatile int stop;
    int dead, in_busy, out_busy;                        /* __atomic */
    uint32_t errors;
    struct libusb_transfer *in[NIN];
    char name[64];
};

static void mark_dead(usbl_t *u)
{
    __atomic_store_n(&u->dead, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&u->errors, 1u, __ATOMIC_SEQ_CST);
}

static void LIBUSB_CALL in_cb(struct libusb_transfer *t)
{
    usbl_t *u = (usbl_t *)t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        if (t->actual_length > 0)
            rxq_push(u->q, u->tag, t->buffer, (uint32_t)t->actual_length);
        if (!u->stop && libusb_submit_transfer(t) == 0)
            return;
        if (!u->stop)
            mark_dead(u);
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        mark_dead(u);                                   /* NO_DEVICE (unplugged), STALL, ERROR */
    }
    __atomic_fetch_sub(&u->in_busy, 1, __ATOMIC_SEQ_CST);
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *t)
{
    usbl_t *u = (usbl_t *)t->user_data;
    if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED)
        mark_dead(u);
    else if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length != t->length)
        mark_dead(u);
    __atomic_fetch_sub(&u->out_busy, 1, __ATOMIC_SEQ_CST);
    libusb_free_transfer(t);                            /* LIBUSB_TRANSFER_FREE_BUFFER: the copy goes too */
}

static void *event_thread(void *arg)
{
    usbl_t *u = (usbl_t *)arg;
    int idle = 0;
    for (;;) {
        struct timeval tv = {0, 50000};
        int busy = __atomic_load_n(&u->in_busy, __ATOMIC_SEQ_CST) + __atomic_load_n(&u->out_busy, __ATOMIC_SEQ_CST);
        if (u->stop && (!busy || ++idle > 40))          /* stopping: until the transfers are back (<= 2 s) */
            break;
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
    }
    return NULL;
}

static int find(usbl_t *u, char *err, size_t errn)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(u->ctx, &list), i;
    int found_pid = 0;
    for (i = 0; i < n && !u->h; i++) {
        struct libusb_device_descriptor dd;
        struct libusb_config_descriptor *cd;
        int k, e, ifn = -1;
        uint8_t ein = 0, eout = 0;
        if (libusb_get_device_descriptor(list[i], &dd) || dd.idVendor != RF_VID || dd.idProduct != RF_PID)
            continue;
        found_pid = 1;
        if (libusb_get_active_config_descriptor(list[i], &cd))
            continue;
        for (k = 0; k < cd->bNumInterfaces && ifn < 0; k++) {
            const struct libusb_interface_descriptor *id = &cd->interface[k].altsetting[0];
            if (id->bInterfaceClass != 0xFF || id->bNumEndpoints < 2)
                continue;
            ein = eout = 0;
            for (e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                if ((ed->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (ed->bEndpointAddress & 0x80)
                    ein = ed->bEndpointAddress;
                else
                    eout = ed->bEndpointAddress;
            }
            if (ein && eout)
                ifn = id->bInterfaceNumber;
        }
        libusb_free_config_descriptor(cd);
        if (ifn >= 0) {
            if (libusb_open(list[i], &u->h)) {
                snprintf(err, errn, "1209:0001 has the vendor interface but cannot be opened");
                u->h = NULL;
                continue;
            }
            u->ifnum = ifn;
            u->ep_in = ein;
            u->ep_out = eout;
            snprintf(u->name, sizeof u->name, "bulk 1209:0001 if%d EP%02x/%02x", ifn, eout, ein);
        }
    }
    libusb_free_device_list(list, 1);
    if (!u->h && !err[0])
        snprintf(err, errn, found_pid ? "1209:0001 has no vendor interface (another firmware)" : "no 1209:0001 on USB");
    return u->h ? 0 : -1;
}

usbl_t *usb_open_unit(rxq_t *q, uint16_t tag, char *err, size_t errn)
{
    usbl_t *u = (usbl_t *)calloc(1, sizeof *u);
    int i;
    err[0] = 0;
    if (!u)
        return NULL;
    u->q = q;
    u->tag = tag;
    if (libusb_init(&u->ctx)) {
        snprintf(err, errn, "libusb_init failed");
        free(u);
        return NULL;
    }
    if (find(u, err, errn)) {
        libusb_exit(u->ctx);
        free(u);
        return NULL;
    }
    if (libusb_claim_interface(u->h, u->ifnum)) {
        snprintf(err, errn, "cannot claim interface %d", u->ifnum);
        libusb_close(u->h);
        libusb_exit(u->ctx);
        free(u);
        return NULL;
    }
    for (i = 0; i < NIN; i++) {
        u->in[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(u->in[i], u->h, u->ep_in, (unsigned char *)malloc(INSZ), INSZ, in_cb, u, 0);
        u->in[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        __atomic_fetch_add(&u->in_busy, 1, __ATOMIC_SEQ_CST);
        if (libusb_submit_transfer(u->in[i])) {
            __atomic_fetch_sub(&u->in_busy, 1, __ATOMIC_SEQ_CST);
            mark_dead(u);
        }
    }
    if (pthread_create(&u->th, NULL, event_thread, u) == 0)
        u->th_started = 1;
    else
        mark_dead(u);
    return u;
}

int usb_write(usbl_t *u, const uint8_t *p, uint32_t n)
{
    struct libusb_transfer *t;
    unsigned char *b;
    if (!u || __atomic_load_n(&u->dead, __ATOMIC_SEQ_CST))
        return -1;
    t = libusb_alloc_transfer(0);
    b = (unsigned char *)malloc(n ? n : 1u);
    if (!t || !b) {
        free(b);
        if (t)
            libusb_free_transfer(t);
        return -1;
    }
    memcpy(b, p, n);
    libusb_fill_bulk_transfer(t, u->h, u->ep_out, b, (int)n, out_cb, u, 2000);
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
    __atomic_fetch_add(&u->out_busy, 1, __ATOMIC_SEQ_CST);
    if (libusb_submit_transfer(t)) {
        __atomic_fetch_sub(&u->out_busy, 1, __ATOMIC_SEQ_CST);
        libusb_free_transfer(t);
        mark_dead(u);
        return -1;
    }
    return 0;
}

int usb_dead(const usbl_t *u) { return __atomic_load_n(&((usbl_t *)u)->dead, __ATOMIC_SEQ_CST); }
uint32_t usb_errors(const usbl_t *u) { return __atomic_load_n(&((usbl_t *)u)->errors, __ATOMIC_SEQ_CST); }
const char *usb_name(const usbl_t *u) { return u->name; }

void usb_close(usbl_t *u)
{
    int i, k;
    if (!u)
        return;
    for (k = 0; k < 20 && __atomic_load_n(&u->out_busy, __ATOMIC_SEQ_CST); k++) {   /* let the last writes go */
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    u->stop = 1;
    for (i = 0; i < NIN; i++)
        if (u->in[i])
            libusb_cancel_transfer(u->in[i]);
    if (u->th_started)
        pthread_join(u->th, NULL);
    for (i = 0; i < NIN; i++)
        if (u->in[i] && !__atomic_load_n(&u->in_busy, __ATOMIC_SEQ_CST))
            libusb_free_transfer(u->in[i]);
    libusb_release_interface(u->h, u->ifnum);
    libusb_close(u->h);
    libusb_exit(u->ctx);
    free(u);
}
#endif
