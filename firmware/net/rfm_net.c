/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 Wi-Fi: the TCP transport's sockets and rings, and the discovery responder (src/rfm_net.h says what each
 * call does; docs/PROTOCOL.md section 10 the wire). On the FM-1 a unit of its own (fm1-wifi.toml net), built as the
 * SDK builds code that talks lwIP, its task made by the app with plat_net_task once Wi-Fi has an address; in the host
 * simulator included by app.c, on the computer's BSD sockets (the same calls: lwIP's are BSD's), polled by the
 * script's "rf serve" (src/rfm_host.c).
 *
 * Everything is non-blocking and polled: a turn (rfn_poll) accepts a host, reads what TCP has into rx while rx has
 * room (TCP's window holds the rest back: nothing is ever dropped), writes tx to TCP as far as TCP takes it, and
 * answers discovery datagrams. The task waits between turns on the app's semaphore (rfn_wait), which the main loop
 * posts when it has queued bytes and at least once a millisecond, so the sockets never wait on the main loop, and the
 * main loop never on a socket. */
#include <stdint.h>
#include <string.h>
#ifdef PLAT_HOST
/* (plat_host.c defines __attribute__ away before it includes the app: glibc's socket calls need theirs, a transparent
 * union, so the headers see the real one) */
#pragma push_macro("__attribute__")
#undef __attribute__
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#pragma pop_macro("__attribute__")
#define closesocket close
#define RFN_SA_LEN(a) ((void)0)
#else
#include "lwip/sockets.h"
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/priv/sockets_priv.h"
#ifndef TCP_SLOW_INTERVAL
#define TCP_SLOW_INTERVAL 500                        /* (lwip/priv/tcp_priv.h: 2 x TCP_TMR_INTERVAL, 250 ms) */
#endif
#define RFN_SA_LEN(a) ((a).sin_len = sizeof(a))
#endif
#include "rfm_net.h"

static uint8_t *rn_rx, *rn_tx;
static uint32_t rn_rxn, rn_txn;                   /* powers of two */
static volatile uint32_t rn_rx_w, rn_rx_r, rn_tx_w, rn_tx_r;
static volatile uint32_t rn_conn;                 /* bumped at every connect and close */
static volatile int rn_cs = -1;                   /* the host's socket */
static int rn_ls = -1, rn_us = -1;                /* listening, discovery */
static uint16_t rn_tcp_port, rn_udp_port;
static char rn_info[200];
static rfn_stats_t rn_st;
static rfn_diag_t rn_dg = {0, 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t rn_nrtx_was, rn_persist_was;
#ifdef PLAT_HOST
static int rn_fake;                               /* rfn_test_connect: a host without a socket */
#endif

void rfn_init(uint8_t *rx, uint32_t rx_n, uint8_t *tx, uint32_t tx_n, uint16_t tcp_port, uint16_t udp_port)
{
    rn_rx = rx;
    rn_rxn = rx_n;
    rn_tx = tx;
    rn_txn = tx_n;
    rn_rx_w = rn_rx_r = rn_tx_w = rn_tx_r = 0;
    rn_tcp_port = tcp_port;
    rn_udp_port = udp_port;
}

void rfn_set_info(const char *text)
{
    unsigned i;
    for (i = 0; i + 1u < sizeof rn_info && text[i]; i++)
        rn_info[i] = text[i];
    rn_info[i] = 0;
}

uint32_t rfn_rx_size(void) { return rn_rxn; }
uint32_t rfn_tx_size(void) { return rn_txn; }
uint32_t rfn_rx_avail(void) { return rn_rx_w - rn_rx_r; }
uint32_t rfn_tx_room(void) { return rn_txn - (rn_tx_w - rn_tx_r); }
uint32_t rfn_tx_pending(void) { return rn_tx_w - rn_tx_r; }
#ifdef PLAT_HOST
int rfn_up(void) { return rn_cs >= 0 || rn_fake; }
#else
int rfn_up(void) { return rn_cs >= 0; }
#endif
uint32_t rfn_conn_id(void) { return rn_conn; }
uint16_t rfn_port(void) { return rn_tcp_port; }
void rfn_stats(rfn_stats_t *s) { *s = rn_st; }

static void rn_sample(void)                       /* (every turn) the connection as lwIP keeps it */
{
    uint32_t p = rn_tx_w - rn_tx_r;
    if (p > rn_dg.tx_pending_max)
        rn_dg.tx_pending_max = p;
#ifndef PLAT_HOST
    if (rn_cs >= 0) {
        struct lwip_sock *k = lwip_socket_dbg_get_socket(rn_cs);
        struct tcp_pcb *t = k && k->conn && NETCONNTYPE_GROUP(k->conn->type) == NETCONN_TCP ? k->conn->pcb.tcp : 0;
        if (t) {                                  /* (read only, between the stack's turns: a diagnostic) */
            if (t->nrtx && !rn_nrtx_was)
                rn_dg.nrtx_events++;
            rn_nrtx_was = t->nrtx;
            if (t->nrtx > rn_dg.nrtx_max)
                rn_dg.nrtx_max = t->nrtx;
            if (t->rto > 0 && (uint32_t)t->rto * TCP_SLOW_INTERVAL > rn_dg.rto_max_ms)
                rn_dg.rto_max_ms = (uint32_t)t->rto * TCP_SLOW_INTERVAL;
            if (t->snd_wnd < rn_dg.snd_wnd_min)
                rn_dg.snd_wnd_min = t->snd_wnd;
            if (t->rcv_wnd < rn_dg.rcv_wnd_min)
                rn_dg.rcv_wnd_min = t->rcv_wnd;
            if (t->persist_backoff && !rn_persist_was)
                rn_dg.persist_events++;
            rn_persist_was = t->persist_backoff;
        }
    }
#endif
}

void rfn_diag(rfn_diag_t *d, int clear)
{
    *d = rn_dg;
#if !defined(PLAT_HOST) && LWIP_STATS
    d->tcp_xmit = lwip_stats.tcp.xmit;
    d->tcp_recv = lwip_stats.tcp.recv;
    d->tcp_drop = lwip_stats.tcp.drop;
    d->tcp_memerr = lwip_stats.tcp.memerr;
    d->link_recv = lwip_stats.link.recv;
    d->link_drop = lwip_stats.link.drop + lwip_stats.link.memerr;
#if MEMP_STATS
    d->pbuf_err = lwip_stats.memp[MEMP_PBUF_POOL] ? lwip_stats.memp[MEMP_PBUF_POOL]->err : 0u;
#endif
#endif
    if (d->snd_wnd_min == 0xFFFFFFFFu)
        d->snd_wnd_min = 0;
    if (d->rcv_wnd_min == 0xFFFFFFFFu)
        d->rcv_wnd_min = 0;
    if (clear) {
        rn_dg.nrtx_max = rn_dg.rto_max_ms = rn_dg.tx_pending_max = 0;
        rn_dg.snd_wnd_min = rn_dg.rcv_wnd_min = 0xFFFFFFFFu;
    }
}

uint32_t rfn_rx_read(uint8_t *dst, uint32_t n)
{
    uint32_t r = rn_rx_r, have = rn_rx_w - r, i;
    if (n > have)
        n = have;
    for (i = 0; i < n; i++)
        dst[i] = rn_rx[(r + i) & (rn_rxn - 1u)];
    rn_rx_r = r + n;                              /* (after the bytes: the task reads the index) */
    return n;
}

int rfn_tx_write(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb)
{
    uint32_t w = rn_tx_w, i;
    if (!rfn_up() || rfn_tx_room() < na + nb)
        return 0;
    for (i = 0; i < na; i++)
        rn_tx[(w + i) & (rn_txn - 1u)] = a[i];
    for (i = 0; i < nb; i++)
        rn_tx[(w + na + i) & (rn_txn - 1u)] = b[i];
    rn_tx_w = w + na + nb;
    return 1;
}

static void rn_nonblock(int s)
{
#ifdef PLAT_HOST
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, (f < 0 ? 0 : f) | O_NONBLOCK);
#else
    int one = 1;
    ioctlsocket(s, FIONBIO, &one);                /* (lwIP's own: no question of whose O_NONBLOCK) */
#endif
}

static void rn_drop_host(void)
{
    if (rn_cs >= 0) {
        closesocket(rn_cs);
        rn_cs = -1;
        rn_st.closes++;
    }
    rn_rx_w = rn_rx_r = 0;                        /* (the app sees the new rfn_conn_id and starts afresh) */
    rn_tx_r = rn_tx_w;
    rn_conn++;
}

void rfn_close(void)
{
#ifdef PLAT_HOST
    rn_fake = 0;
#endif
    rn_drop_host();
}

static int rn_open(void)
{
    struct sockaddr_in a;
    int one = 1;
    if (rn_ls < 0) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            rn_st.open_fail++;
            return -1;
        }
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&a, 0, sizeof a);
        RFN_SA_LEN(a);
        a.sin_family = AF_INET;
        a.sin_port = htons(rn_tcp_port);
#ifdef PLAT_HOST
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* (the simulator: this computer only) */
#else
        a.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
        if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) {
            closesocket(s);
            rn_st.open_fail++;
            return -1;
        }
        rn_nonblock(s);
        rn_ls = s;
    }
    if (rn_us < 0) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
            memset(&a, 0, sizeof a);
            RFN_SA_LEN(a);
            a.sin_family = AF_INET;
            a.sin_port = htons(rn_udp_port);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
                closesocket(s);
                s = -1;
            } else {
                rn_nonblock(s);
            }
        }
        rn_us = s;                                /* (no discovery is no reason to stop: TCP by address works) */
    }
    return 0;
}

static void rn_accept(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int s = accept(rn_ls, (struct sockaddr *)&a, &al), one = 1;
    if (s < 0)
        return;
    if (rn_cs >= 0) {
        rn_st.replaced++;
        rn_drop_host();
    }
    rn_nonblock(s);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* (replies and acknowledgements go at once) */
    rn_rx_w = rn_rx_r = 0;
    rn_tx_r = rn_tx_w;
    rn_cs = s;
    rn_conn++;
    rn_st.accepts++;
}

static int rn_udp(void)
{
    static uint8_t q[64];
    static char out[208];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = (int)recvfrom(rn_us, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
    unsigned i, k;
    if (n <= 0)
        return 0;
    rn_st.udp_in++;
    if (n < 5 || memcmp(q, "RFM1?", 5) != 0)
        return 1;
    memcpy(out, "RFM1!\n", 6);
    for (k = 6, i = 0; rn_info[i] && k + 1u < sizeof out; i++, k++)
        out[k] = rn_info[i];
    if (sendto(rn_us, out, k, 0, (struct sockaddr *)&from, fl) > 0)
        rn_st.udp_out++;
    return 1;
}

int rfn_poll(void)
{
    fd_set rd, wr;
    struct timeval tv = {0, 0};
    int mx = -1, did = 0, n;
    rn_st.polls++;
    rn_sample();
    if (rn_ls < 0 && rn_open() < 0)
        return -1;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_SET(rn_ls, &rd);
    mx = rn_ls;
    if (rn_us >= 0) {
        FD_SET(rn_us, &rd);
        mx = rn_us > mx ? rn_us : mx;
    }
    if (rn_cs >= 0) {
        if (rfn_rx_avail() < rn_rxn)
            FD_SET(rn_cs, &rd);                   /* (rx full: TCP's window holds the host back) */
        else
            rn_st.rx_full++;
        if (rfn_tx_pending())
            FD_SET(rn_cs, &wr);
        mx = rn_cs > mx ? rn_cs : mx;
    }
    if (select(mx + 1, &rd, &wr, 0, &tv) <= 0)
        return 0;
    if (FD_ISSET(rn_ls, &rd)) {
        rn_accept();
        did = 1;
    }
    if (rn_us >= 0 && FD_ISSET(rn_us, &rd))
        did |= rn_udp();
    if (rn_cs >= 0 && FD_ISSET(rn_cs, &rd)) {
        uint32_t w = rn_rx_w, free_ = rn_rxn - (w - rn_rx_r), at = w & (rn_rxn - 1u),
                 c = rn_rxn - at < free_ ? rn_rxn - at : free_;
        n = (int)recv(rn_cs, rn_rx + at, c, 0);
        if (n > 0) {
            rn_rx_w = w + (uint32_t)n;            /* (after the bytes) */
            rn_st.rx_bytes += (uint32_t)n;
            did = 1;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            rn_drop_host();                       /* the host closed, or the link is gone */
            return 1;
        }
    }
    if (rn_cs >= 0 && FD_ISSET(rn_cs, &wr)) {
        uint32_t r = rn_tx_r, have = rn_tx_w - r, at = r & (rn_txn - 1u), c = rn_txn - at < have ? rn_txn - at : have;
        n = (int)send(rn_cs, rn_tx + at, c, 0);
        if (n > 0) {
            rn_tx_r = r + (uint32_t)n;
            rn_st.tx_bytes += (uint32_t)n;
            did = 1;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            rn_st.tx_eagain++;
        } else {
            rn_drop_host();
            return 1;
        }
    }
    return did;
}

#ifndef PLAT_HOST
/* the task: a turn, then the app's semaphore (posted by the main loop when it queued bytes, and every millisecond);
 * after a busy turn another at once (a yield) */
void rfn_task(void *arg)
{
    (void)arg;
    for (;;) {
        int r = rfn_poll(), k;
        for (k = 0; r > 0 && k < 4; k++)          /* (a burst: a few turns before the main loop has its own) */
            r = rfn_poll();
        rfn_wait();
    }
}
#else
void rfn_task(void *arg) { (void)arg; }
void rfn_test_connect(int on)
{
    if (on) {
        rn_fake = 1;
        rn_rx_w = rn_rx_r = 0;
        rn_tx_r = rn_tx_w;
        rn_conn++;
        rn_st.accepts++;
    } else {
        rfn_close();
    }
}
uint32_t rfn_test_inject(const uint8_t *p, uint32_t n)
{
    uint32_t w = rn_rx_w, i, room = rn_rxn - (w - rn_rx_r);
    if (n > room)
        n = room;
    for (i = 0; i < n; i++)
        rn_rx[(w + i) & (rn_rxn - 1u)] = p[i];
    rn_rx_w = w + n;
    rn_st.rx_bytes += n;
    return n;
}
uint32_t rfn_test_take(uint8_t *dst, uint32_t max)
{
    uint32_t r = rn_tx_r, n = rn_tx_w - r, i;
    if (n > max)
        n = max;
    for (i = 0; i < n; i++)
        dst[i] = rn_tx[(r + i) & (rn_txn - 1u)];
    rn_tx_r = r + n;
    rn_st.tx_bytes += n;
    return n;
}
#endif
