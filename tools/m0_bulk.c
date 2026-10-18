/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 M0: the vendor bulk half of the bench (EP3 of the vendor interface, class 0xFF), with libusb.
 *
 *   cc -std=c99 -O2 -Wall -Ifirmware/src -I/opt/homebrew/include/libusb-1.0 -o build/m0_bulk tools/m0_bulk.c \
 *      -L/opt/homebrew/lib -lusb-1.0
 *   build/m0_bulk [OUT.json] [--seconds 5] [--sizes 64,512,4096,16384,65536] [--depth 4] [--only sink,source,echo,tile]
 *
 * It finds 1209:0001 with an interface of class 0xFF (RemoteFM1's; another firmware has none: it says so and
 * exits with 2), claims that interface only (the MIDI interfaces stay with the system: no configuration change,
 * no reset, no kernel driver detached), sends HELLO and goes on only if the answer is "RemoteFM1-M0". Then:
 *   sink    SINK frames, one per transfer of each size (10-byte header + seq + data + sum), --depth transfers in
 *           flight, for --seconds; then STATS: what arrived, gaps, bad sums
 *   source  SOURCE (messages of min(size, 16402) - 18 data bytes) read with transfers of each size, --depth in
 *           flight, for --seconds, stopped with HELLO: the rate, sequence gaps, bad data
 *   echo    500 ECHO round trips of 16 bytes (one transfer each way): min / median / p99
 *   tile    TILE: 240 x 16 strips (one frame per transfer) and 16 x 16 tiles (a row of 15 per transfer), --seconds
 *           each, then STATS: tiles drawn per second, screens per second
 * The protocol is firmware/src/remote_proto.h (this file includes it). */
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "remote_proto.h"

static libusb_context *ctx;
static libusb_device_handle *dev;
static int ifnum = -1;
static uint8_t ep_out, ep_in;
static uint32_t tx_seq;
static double seconds = 5.0;
static int depth = 4;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- receive: one stream parser; replies land in a small mailbox, DATA is checked as it comes */
static rf_tstats_t rx_st;
static rf_bk_t rx;
static uint8_t rx_buf[70000];
static struct {
    uint32_t cmd, len;
    uint8_t body[1024];
    int full;
} mbox;
static struct {
    int on;
    uint64_t bytes, msgs, gaps, bad;
    uint32_t next;
    int have;
    double t_first, t_last;
} src;
static uint8_t src_done[13];
static int src_done_got;

static void on_msg(void *c, uint32_t cmd, const uint8_t *b, uint32_t len)
{
    (void)c;
    if (cmd == RF_DATA) {
        uint32_t seq, n, i, s = 0;
        double t = now();
        if (len < 8) {
            src.bad++;
            return;
        }
        seq = rf_rd32(b);
        n = len - 8;
        for (i = 0; i < n; i++) {
            if (b[4 + i] != (uint8_t)(seq * 7u + i)) {
                src.bad++;
                break;
            }
            s = rf_sum_byte(s, b[4 + i]);
        }
        if (i == n && s != rf_rd32(b + 4 + n))
            src.bad++;
        if (src.have && seq != src.next)
            src.gaps++;
        src.next = seq + 1;
        src.have = 1;
        if (!src.msgs++)
            src.t_first = t;
        src.t_last = t;
        src.bytes += n;
        return;
    }
    if (cmd == RF_SOURCE_DONE && len >= 13) {
        memcpy(src_done, b, 13);
        src_done_got = 1;
        return;
    }
    mbox.cmd = cmd;
    mbox.len = len < sizeof mbox.body ? len : (uint32_t)sizeof mbox.body;
    memcpy(mbox.body, b, mbox.len);
    mbox.full = 1;
}

/* ---- synchronous helpers */
static int send_frame(uint32_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t f[RF_BK_HDR + 70000];
    int done = 0, rc;
    rf_bk_header(f, cmd, n, tx_seq++);
    if (n)
        memcpy(f + RF_BK_HDR, body, n);
    rc = libusb_bulk_transfer(dev, ep_out, f, (int)(RF_BK_HDR + n), &done, 2000);
    return rc == 0 && done == (int)(RF_BK_HDR + n) ? 0 : -1;
}

static int wait_reply(uint32_t cmd, double timeout)        /* reads until a reply cmd lands in the mailbox */
{
    static uint8_t b[16384];
    double end = now() + timeout;
    while (now() < end) {
        int got = 0, rc;
        if (mbox.full) {
            mbox.full = 0;
            if (mbox.cmd == cmd)
                return 1;
            continue;
        }
        rc = libusb_bulk_transfer(dev, ep_in, b, sizeof b, &got, 200);
        if (got > 0)
            rf_bk_feed(&rx, b, (uint32_t)got);
        if (rc && rc != LIBUSB_ERROR_TIMEOUT)
            return 0;
    }
    return 0;
}

static uint32_t stv[RF_ST_N + 16];
static int get_stats(void)
{
    if (send_frame(RF_STATS, NULL, 0) || !wait_reply(RF_STATS_R, 3.0))
        return -1;
    memset(stv, 0, sizeof stv);
    for (uint32_t i = 0; i < mbox.body[0] && 1 + 4 * i + 4 <= mbox.len && i < RF_ST_N + 16; i++)
        stv[i] = rf_rd32(mbox.body + 1 + 4 * i);
    return 0;
}

static int reset_stats(void) { return send_frame(RF_STATS_RESET, NULL, 0) || !wait_reply(RF_ACK, 3.0) ? -1 : 0; }

/* ---- asynchronous pump: up to depth transfers in flight on one endpoint */
typedef struct {
    struct libusb_transfer *t;
    uint8_t *buf;
    int busy, status, len;
} xf_t;
static void LIBUSB_CALL xf_cb(struct libusb_transfer *t)
{
    xf_t *x = t->user_data;
    x->busy = 0;
    x->status = t->status;
    x->len = t->actual_length;
    if (t->endpoint & 0x80 && t->actual_length > 0)
        rf_bk_feed(&rx, t->buffer, (uint32_t)t->actual_length);   /* (completions come in submission order) */
}
static xf_t *xf_new(int n, int size)
{
    xf_t *x = calloc((size_t)n, sizeof *x);
    for (int i = 0; i < n; i++) {
        x[i].t = libusb_alloc_transfer(0);
        x[i].buf = malloc((size_t)size + RF_BK_HDR);
    }
    return x;
}
static void xf_wait_all(xf_t *x, int n, int cancel)
{
    double end = now() + 3.0;
    if (cancel)
        for (int i = 0; i < n; i++)
            if (x[i].busy)
                libusb_cancel_transfer(x[i].t);
    for (;;) {
        int busy = 0;
        struct timeval tv = {0, 20000};
        for (int i = 0; i < n; i++)
            busy |= x[i].busy;
        if (!busy || now() > end)
            break;
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }
}
static void xf_free(xf_t *x, int n)
{
    for (int i = 0; i < n; i++) {
        libusb_free_transfer(x[i].t);
        free(x[i].buf);
    }
    free(x);
}
static void pump(void)
{
    struct timeval tv = {0, 2000};
    libusb_handle_events_timeout_completed(ctx, &tv, NULL);
}

/* ---- the tests; results as JSON fragments */
static char js[65536];
static size_t jn;
#define J(...) (jn += (size_t)snprintf(js + jn, sizeof js - jn, __VA_ARGS__))

static void test_sink(int size)
{
    xf_t *x = xf_new(depth, size);
    uint32_t data = (uint32_t)size - RF_BK_HDR - 8u, seq = 0, i;
    uint64_t sent = 0;
    uint8_t pat[70000];
    uint32_t sum;
    double t0, t1, end;
    int err = 0;
    for (i = 0; i < data; i++)
        pat[i] = (uint8_t)(rand() >> 7);
    sum = rf_sum(pat, data);
    reset_stats();
    t0 = now();
    end = t0 + seconds;
    while (now() < end && !err) {
        for (int k = 0; k < depth; k++) {
            if (x[k].busy)
                continue;
            if (x[k].status && sent) {
                err = x[k].status;
                break;
            }
            rf_bk_header(x[k].buf, RF_SINK, (uint32_t)size - RF_BK_HDR, tx_seq++);
            rf_wr32(x[k].buf + RF_BK_HDR, seq++);
            memcpy(x[k].buf + RF_BK_HDR + 4, pat, data);
            rf_wr32(x[k].buf + RF_BK_HDR + 4 + data, sum);
            libusb_fill_bulk_transfer(x[k].t, dev, ep_out, x[k].buf, size, xf_cb, &x[k], 5000);
            x[k].busy = 1;
            x[k].status = 0;
            if (libusb_submit_transfer(x[k].t)) {
                x[k].busy = 0;
                err = -1;
                break;
            }
            sent += data;
        }
        pump();
    }
    xf_wait_all(x, depth, 0);
    t1 = now();
    xf_free(x, depth);
    if (get_stats()) {
        J("{\"size\": %d, \"error\": \"no STATS\"}", size);
        printf("  %6d  no STATS\n", size);
        return;
    }
    {
        double dev_ms = stv[RF_ST_BK_T_LAST] > stv[RF_ST_BK_T_FIRST] ? stv[RF_ST_BK_T_LAST] - stv[RF_ST_BK_T_FIRST] : 1;
        double host = sent / (t1 - t0) / 1024, devr = stv[RF_ST_BK_SINK_BYTES] / dev_ms * 1000 / 1024;
        printf("  %6d  %9u  %9.1f  %9.1f  %12u  %5u  %5u  %5u  %8u  %9u  %6u  %s\n", size, seq, host, devr,
               stv[RF_ST_BK_SINK_BYTES], stv[RF_ST_BK_GAPS], stv[RF_ST_BK_CHK], stv[RF_ST_BK_BAD],
               stv[RF_ST_BK_HELD], stv[RF_ST_POLLS_MAIN_PS], stv[RF_ST_USB_PERMILLE], err ? "ERROR" : "");
        J("{\"size\": %d, \"transfers\": %u, \"host_kBps\": %.1f, \"dev_kBps\": %.1f, \"arrived_bytes\": %u, "
          "\"gaps\": %u, \"chk_err\": %u, \"bad\": %u, \"frame_gaps\": %u, \"held\": %u, \"polls_main_ps\": %u, "
          "\"polls_isr_ps\": %u, \"usb_permille\": %u, \"error\": %d}",
          size, seq, host, devr, stv[RF_ST_BK_SINK_BYTES], stv[RF_ST_BK_GAPS], stv[RF_ST_BK_CHK], stv[RF_ST_BK_BAD],
          stv[RF_ST_BK_FRAME_GAPS], stv[RF_ST_BK_HELD], stv[RF_ST_POLLS_MAIN_PS], stv[RF_ST_POLLS_ISR_PS],
          stv[RF_ST_USB_PERMILLE], err);
    }
}

static void test_source(int size)
{
    xf_t *x = xf_new(depth, size);
    uint8_t req[6];
    uint32_t ml = (uint32_t)(size < (int)RF_BK_SRC_MAX + 18 ? size : (int)RF_BK_SRC_MAX + 18) - 18u;
    double end, stop_end;
    int stopped = 0;
    reset_stats();
    memset(&src, 0, sizeof src);
    src_done_got = 0;
    rf_wr32(req, 0xFFFFFFFFu);
    rf_wr16(req + 4, ml);
    send_frame(RF_SOURCE, req, 6);
    end = now() + seconds;
    stop_end = end + 3.0;
    while (now() < stop_end) {
        for (int k = 0; k < depth; k++) {
            if (x[k].busy)
                continue;
            libusb_fill_bulk_transfer(x[k].t, dev, ep_in, x[k].buf, size, xf_cb, &x[k], 0);
            x[k].busy = 1;
            if (libusb_submit_transfer(x[k].t))
                x[k].busy = 0;
        }
        pump();
        if (!stopped && now() >= end) {
            stopped = 1;
            send_frame(RF_HELLO, NULL, 0);              /* stops it: SOURCE_DONE, then HELLO_R */
        }
        if (stopped && src_done_got && mbox.full && mbox.cmd == RF_HELLO_R)
            break;
    }
    mbox.full = 0;
    xf_wait_all(x, depth, 1);
    xf_free(x, depth);
    {
        double span = src.msgs > 1 ? src.t_last - src.t_first : 0, host = span > 0 ? src.bytes / span / 1024 : 0;
        uint32_t db = rf_rd32(src_done), dus = rf_rd32(src_done + 8);
        double devr = src_done_got && dus ? db / (dus * 1e-6) / 1024 : 0;
        printf("  %6d  %6u  %9llu  %10llu  %9.1f  %9.1f  %5llu  %5llu  %s\n", size, ml,
               (unsigned long long)src.msgs, (unsigned long long)src.bytes, host, devr,
               (unsigned long long)src.gaps, (unsigned long long)src.bad, src_done_got ? "" : "no SOURCE_DONE");
        J("{\"size\": %d, \"msg_len\": %u, \"msgs\": %llu, \"bytes\": %llu, \"host_kBps\": %.1f, \"dev_kBps\": %.1f, "
          "\"gaps\": %llu, \"bad\": %llu, \"done\": %d}", size, ml, (unsigned long long)src.msgs,
          (unsigned long long)src.bytes, host, devr, (unsigned long long)src.gaps, (unsigned long long)src.bad,
          src_done_got);
    }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void test_echo(void)
{
    enum { N = 500 };
    static double lat[N];
    int n = 0, lost = 0;
    uint8_t body[16];
    for (int i = 0; i < N; i++) {
        double t0;
        memset(body, 0xA5, sizeof body);
        rf_wr32(body, (uint32_t)i);
        t0 = now();
        if (send_frame(RF_ECHO, body, sizeof body) || !wait_reply(RF_ECHO_R, 1.0) || mbox.len != sizeof body ||
            memcmp(mbox.body, body, sizeof body)) {
            lost++;
            continue;
        }
        lat[n++] = (now() - t0) * 1000;
    }
    if (!n) {
        printf("  no ECHO_R\n");
        J("{\"error\": \"no ECHO_R\"}");
        return;
    }
    qsort(lat, (size_t)n, sizeof lat[0], cmp_d);
    printf("  %5d  %4d  %7.3f  %9.3f  %7.3f  %7.3f\n", N, lost, lat[0], lat[n / 2], lat[n * 99 / 100], lat[n - 1]);
    J("{\"n\": %d, \"lost\": %d, \"min_ms\": %.3f, \"median_ms\": %.3f, \"p99_ms\": %.3f, \"max_ms\": %.3f}", N, lost,
      lat[0], lat[n / 2], lat[n * 99 / 100], lat[n - 1]);
}

/* TILE: kind 0 = 240 x 16 strips, one frame per transfer; kind 1 = 16 x 16 tiles, a row of 15 frames per transfer */
static void test_tile(int kind)
{
    int per = kind ? 15 : 1, w = kind ? 16 : 240, fsz = RF_BK_HDR + 4 + w * 16 * 2, size = per * fsz;
    xf_t *x = xf_new(depth, size);
    uint32_t sent = 0, row = 0;
    double t0, t1, end;
    reset_stats();
    t0 = now();
    end = t0 + seconds;
    while (now() < end) {
        for (int k = 0; k < depth; k++) {
            uint8_t *p = x[k].buf;
            if (x[k].busy)
                continue;
            for (int j = 0; j < per; j++, p += fsz) {
                uint32_t y = row % 15u, c = (row / 15u) * 40u + (kind ? (uint32_t)j * 9u : 0u);
                uint16_t px = (uint16_t)((((c * 3u) & 0xF8u) << 8) | (((y * 17u) & 0xFCu) << 3) | ((c & 0xF8u) >> 3));
                rf_bk_header(p, RF_TILE, 4u + (uint32_t)w * 32u, tx_seq++);
                p[RF_BK_HDR] = (uint8_t)(kind ? j * 16 : 0);
                p[RF_BK_HDR + 1] = (uint8_t)(y * 16u);
                p[RF_BK_HDR + 2] = (uint8_t)w;
                p[RF_BK_HDR + 3] = 16;
                for (int i = 0; i < w * 16; i++) {
                    p[RF_BK_HDR + 4 + 2 * i] = (uint8_t)(px >> 8);
                    p[RF_BK_HDR + 5 + 2 * i] = (uint8_t)px;
                }
                sent++;
            }
            row++;
            libusb_fill_bulk_transfer(x[k].t, dev, ep_out, x[k].buf, size, xf_cb, &x[k], 5000);
            x[k].busy = 1;
            if (libusb_submit_transfer(x[k].t))
                x[k].busy = 0;
        }
        pump();
    }
    xf_wait_all(x, depth, 0);
    xf_free(x, depth);
    t1 = now();
    if (get_stats()) {
        printf("  no STATS\n");
        J("{\"kind\": \"%s\", \"error\": \"no STATS\"}", kind ? "16x16" : "240x16");
        return;
    }
    {
        double per_screen = kind ? 225.0 : 15.0, tps = stv[RF_ST_TILES] / (t1 - t0);
        printf("  %-7s  %6u  %6u  %9.1f  %8.1f  %9.1f  %5u\n", kind ? "16x16" : "240x16", sent, stv[RF_ST_TILES], tps,
               tps / per_screen, stv[RF_ST_TILE_BYTES] / (t1 - t0) / 1024, stv[RF_ST_BK_BAD]);
        J("{\"kind\": \"%s\", \"sent\": %u, \"drawn\": %u, \"tiles_ps\": %.1f, \"screens_ps\": %.2f, "
          "\"pixel_kBps\": %.1f, \"bad\": %u}", kind ? "16x16" : "240x16", sent, stv[RF_ST_TILES], tps,
          tps / per_screen, stv[RF_ST_TILE_BYTES] / (t1 - t0) / 1024, stv[RF_ST_BK_BAD]);
    }
}

static int find_device(void)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int found_pid = 0;
    for (ssize_t i = 0; i < n && !dev; i++) {
        struct libusb_device_descriptor dd;
        struct libusb_config_descriptor *cd;
        if (libusb_get_device_descriptor(list[i], &dd) || dd.idVendor != RF_VID || dd.idProduct != RF_PID)
            continue;
        found_pid = 1;
        if (libusb_get_active_config_descriptor(list[i], &cd))
            continue;
        for (int k = 0; k < cd->bNumInterfaces && ifnum < 0; k++) {
            const struct libusb_interface_descriptor *id = &cd->interface[k].altsetting[0];
            if (id->bInterfaceClass != 0xFF || id->bNumEndpoints < 2)
                continue;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                if ((ed->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (ed->bEndpointAddress & 0x80)
                    ep_in = ed->bEndpointAddress;
                else
                    ep_out = ed->bEndpointAddress;
            }
            if (ep_in && ep_out)
                ifnum = id->bInterfaceNumber;
        }
        libusb_free_config_descriptor(cd);
        if (ifnum >= 0 && libusb_open(list[i], &dev)) {
            fprintf(stderr, "m0_bulk: found the vendor interface but cannot open the device\n");
            ifnum = -1;
        }
    }
    libusb_free_device_list(list, 1);
    if (!dev)
        fprintf(stderr, found_pid ? "m0_bulk: 1209:0001 has no vendor interface: the unit runs another firmware\n"
                                  : "m0_bulk: no 1209:0001 device\n");
    return dev ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *out = NULL, *only = "sink,source,echo,tile";
    char sizes_s[256] = "64,512,4096,16384,65536";
    int sizes[16], ns = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sizes") && i + 1 < argc)
            snprintf(sizes_s, sizeof sizes_s, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc)
            depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            only = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: m0_bulk [OUT.json] [--seconds S] [--sizes a,b,..] [--depth N] [--only "
                            "sink,source,echo,tile]\n");
            return 1;
        } else
            out = argv[i];
    }
    for (char *p = strtok(sizes_s, ","); p && ns < 16; p = strtok(NULL, ","))
        if (atoi(p) >= 64 && atoi(p) <= 65536)
            sizes[ns++] = atoi(p);
    if (depth < 1 || depth > 32)
        depth = 4;
    if (libusb_init(&ctx))
        return 1;
    if (find_device()) {
        libusb_exit(ctx);
        return 2;
    }
    if (libusb_claim_interface(dev, ifnum)) {
        fprintf(stderr, "m0_bulk: cannot claim interface %d\n", ifnum);
        libusb_close(dev);
        libusb_exit(ctx);
        return 1;
    }
    rf_body_init(&rx.b, &rx_st, rx_buf, sizeof rx_buf, on_msg, NULL);
    printf("interface %d, EP OUT %02x, EP IN %02x\n", ifnum, ep_out, ep_in);
    {
        static uint8_t junk[16384];
        int got;
        while (!libusb_bulk_transfer(dev, ep_in, junk, sizeof junk, &got, 100) && got > 0)
            ;                                           /* whatever an earlier run left */
    }
    if (send_frame(RF_HELLO, NULL, 0) || !wait_reply(RF_HELLO_R, 1.5) || mbox.len < RF_HELLO_LEN ||
        memcmp(mbox.body, RF_NAME, sizeof RF_NAME - 1)) {
        fprintf(stderr, "m0_bulk: no RemoteFM1-M0 answer over bulk\n");
        rc = 2;
        goto done;
    }
    {
        const uint8_t *h = mbox.body;
        printf("HELLO  %.16s v%.8s  proto %u  identity %.8s  usb mode %u  lcd baud %u  loader %d\n", h, h + 17, h[16],
               h + 43, h[26], h[27], (int8_t)h[28]);
        J("{\"hello\": {\"name\": \"%.16s\", \"version\": \"%.8s\", \"usb_mode\": %u, \"lcd_baud\": %u, "
          "\"loader_state\": %d, \"identity\": \"%.8s\"}, \"depth\": %d, \"seconds\": %.1f", h, h + 17, h[26], h[27],
          (int8_t)h[28], h + 43, depth, seconds);
    }
    if (strstr(only, "sink")) {
        printf("\nSINK host -> device, %.0f s each, %d transfers in flight (KB = 1024 B of payload)\n", seconds, depth);
        printf("  %6s  %9s  %9s  %9s  %12s  %5s  %5s  %5s  %8s  %9s  %6s\n", "size", "transfers", "host_kBps",
               "dev_kBps", "arrived", "gaps", "chk", "bad", "held", "polls/s", "usb%o");
        J(", \"sink\": [");
        for (int i = 0; i < ns; i++) {
            if (i)
                J(", ");
            test_sink(sizes[i]);
        }
        J("]");
    }
    if (strstr(only, "source")) {
        printf("\nSOURCE device -> host, %.0f s each, read transfers of size, %d in flight\n", seconds, depth);
        printf("  %6s  %6s  %9s  %10s  %9s  %9s  %5s  %5s\n", "size", "msglen", "msgs", "bytes", "host_kBps", "dev_kBps",
               "gaps", "bad");
        J(", \"source\": [");
        for (int i = 0; i < ns; i++) {
            if (i)
                J(", ");
            test_source(sizes[i]);
        }
        J("]");
    }
    if (strstr(only, "echo")) {
        printf("\nECHO 16 bytes, 500 round trips\n  %5s  %4s  %7s  %9s  %7s  %7s\n", "n", "lost", "min_ms", "median_ms",
               "p99_ms", "max_ms");
        J(", \"echo\": ");
        test_echo();
    }
    if (strstr(only, "tile")) {
        printf("\nTILE over bulk, %.0f s each\n  %-7s  %6s  %6s  %9s  %8s  %9s  %5s\n", seconds, "kind", "sent", "drawn",
               "tiles/s", "screens/s", "px_kBps", "bad");
        J(", \"tile\": [");
        test_tile(0);
        J(", ");
        test_tile(1);
        J("]");
    }
    if (!get_stats()) {
        J(", \"stats\": [");
        for (uint32_t i = 0; i < RF_ST_N; i++)
            J("%s%u", i ? ", " : "", stv[i]);
        J("]");
    }
    J("}\n");
    if (out) {
        FILE *f = fopen(out, "w");
        if (f) {
            fputs(js, f);
            fclose(f);
            printf("\nwrote %s\n", out);
        }
    }
done:
    libusb_release_interface(dev, ifnum);
    libusb_close(dev);
    libusb_exit(ctx);
    return rc;
}
