/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 Wi-Fi (docs/WIFI.md, docs/PROTOCOL.md section 10): the network side of the TCP transport, between the
 * app's main loop (src/rfm_main.c, transport RF_T_NET) and the sockets (net/rfm_net.c: on the FM-1 a unit of its own
 * built with lwIP's headers, fm1.toml net; in the host simulator part of the app, on the computer's sockets).
 *
 * Two byte rings carry the bulk framing as it is on the wire: rx (TCP -> the app's parser) and tx (the app's whole
 * messages -> TCP). On the FM-1 the socket code runs in a task of the shim's (plat_net_task), which runs only inside
 * the main loop's shim_run (cooperative, one core): the rings need no lock, only each side writing its own index
 * after its bytes. One host at a time: a new connection replaces the old one (a host that comes back after a lost
 * link is not shut out by a dead socket), the rings emptied, rfn_conn_id() changed so the app starts its parser and
 * any session on the network afresh. A UDP datagram that starts "RFM1?" on the discovery port is answered with the
 * text rfn_set_info gave (to the sender's address and port). */
#ifndef RFM_NET_H
#define RFM_NET_H
#include <stdint.h>

void rfn_init(uint8_t *rx, uint32_t rx_n, uint8_t *tx, uint32_t tx_n, uint16_t tcp_port, uint16_t udp_port);
void rfn_set_info(const char *text);            /* the discovery answer after "RFM1!\n" (copied, <= 191 bytes) */
uint32_t rfn_rx_avail(void);
uint32_t rfn_rx_read(uint8_t *dst, uint32_t n);
uint32_t rfn_tx_room(void);
int rfn_tx_write(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb);   /* whole or nothing: 1 / 0 */
uint32_t rfn_tx_pending(void);                  /* bytes not yet given to TCP */
int rfn_up(void);                               /* a host is connected */
uint32_t rfn_conn_id(void);                     /* changes with every connection and every close */
uint16_t rfn_port(void);                        /* the TCP port served (RFM_NET_TCP_PORT; the simulator's own) */
uint32_t rfn_rx_size(void), rfn_tx_size(void);
int rfn_poll(void);                             /* the sockets' turn: > 0 something moved, 0 idle, < 0 not open */
void rfn_close(void);                           /* the host let go (the app's wish) */
typedef struct {
    uint32_t accepts, closes, replaced, rx_bytes, tx_bytes, rx_full, tx_eagain, udp_in, udp_out, polls, open_fail;
} rfn_stats_t;
void rfn_stats(rfn_stats_t *s);
/* the FM-1: the task (plat_net_task(rfn_task, 0, "rfm_net")); it waits in rfn_wait (the app's, src/rfm_main.c: the
 * shim's semaphore, which the main loop posts) between turns */
void rfn_task(void *arg);
void rfn_wait(void);
#ifdef PLAT_HOST
/* the simulator's scripted host (src/rfm_host.c): a connection without a socket, its bytes by hand */
void rfn_test_connect(int on);
uint32_t rfn_test_inject(const uint8_t *p, uint32_t n);   /* host -> device: into rx */
uint32_t rfn_test_take(uint8_t *dst, uint32_t max);       /* device -> host: out of tx */
#endif
#endif
