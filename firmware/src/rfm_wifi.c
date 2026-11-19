/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 Wi-Fi (fm1-wifi.toml; docs/WIFI.md, docs/PROTOCOL.md section 10): the app's side of the network, part of
 * rfm_main.c (included at its end, in the Wi-Fi build only).
 *
 *   rf_big_init     the app's big buffers (app.h RF_BIG) from the Wi-Fi heap, zeroed, before anything uses them
 *   rf_net_*        the TCP transport's rings (net/rfm_net.c) and the socket task: made once Wi-Fi has an address,
 *                   given a turn whenever the main loop has queued bytes and every millisecond (the shim's semaphore,
 *                   rfn_wait); fm1-base's plat_yield (rf_run, every 250 us) runs the shim's tasks
 *   rf_wifi_cmd     WIFI (0x17): GET the state, SET a network (SSID and password: joined, and kept by fm1-base's
 *                   device/wifi.c in OBJ_WIFI once joined), FORGET one or all, SAVED lists them. SET and FORGET over USB
 *                   (SysEx or bulk) only. The password is never logged, shown or sent back, and its copies here are
 *                   wiped once handed to the library; SCAN asks the radio for the networks around, LIST pages them
 *                   (or the networks kept) small enough for the Move's USB-A
 *   rf_wifi_ms      once a millisecond: a SET waiting for the radio, the socket task's start, discovery's text
 *   rf_wifi_line    the waiting screen's line: Wi-Fi's state and address */

#ifdef PLAT_HOST
#include <stdlib.h>
#define RF_HEAP_USED 0u
#define RF_HEAP_PEAK 0u
#define RF_HEAP_SIZE 0u
#else
#include <stddef.h>
void *malloc(size_t n);                             /* (the shim's heap: ble/shim/misc.c, 248 + 24 KB with Wi-Fi) */
int os_sem_create(OS_SEM *s, int cnt);              /* (the shim's JieLi OS: ble/shim/os.c) */
int os_sem_post(OS_SEM *s);
int os_sem_pend(OS_SEM *s, int timeout);
extern uint32_t shim_heap_used, shim_heap_peak;
extern const uint32_t shim_heap_size;
#define RF_HEAP_USED shim_heap_used
#define RF_HEAP_PEAK shim_heap_peak
#define RF_HEAP_SIZE shim_heap_size
#endif

/* ------------------------------------------------------------------ the pool's layout --- */
/* fm1-base's app_wifi.ld: "keep the pool's layout as fm1-radio has it: the radio hardware writes somewhere in it
 * (unfound), and moving the stacks / heap / lwIP pools by even 4.6 KiB broke its reception". The pool before the heap
 * is made exactly as long as examples/wifi's (0x5A04 bytes: its canvas), so the heap and lwIP's pools start at the
 * same addresses as there; tools/wifi_layout.py checks the built image and says the pad if the app's pool changed */
#ifndef PLAT_HOST
#define RF_POOL_PAD 1512u
static uint8_t rf_pool_pad[RF_POOL_PAD] PLAT_POOL __attribute__((used));
#endif

/* ------------------------------------------------------------------ the big buffers --- */
static uint32_t rf_big_bytes;                       /* what the app took from the heap (WIFI_R) */
static void *rf_take(uint32_t n)
{
    void *p = malloc(n);
    if (!p) {                                       /* (the heap is fresh at app_init: never; and nothing could run) */
        plat_log("RemoteFM1: no heap for its buffers");
        for (;;)
            ;
    }
    memset(p, 0, n);                                /* (as the pool is at boot) */
    rf_big_bytes += n;
    return p;
}
#define RF_TAKE(name, n) ((name) = rf_take((uint32_t)(sizeof *(name) * (n))))
static void rf_big_init(void)
{
#ifndef PLAT_HOST
    extern uint8_t _rb_start[], _rb_end[];
    memset(_rb_start, 0, (uint32_t)(_rb_end - _rb_start));   /* (RF_LOW: RAMTEXT's rest is not zeroed at boot) */
#endif
    rf_sx_buf = rf_take(2u * RF_SX_BODY_MAX);
    RF_TAKE(rf_reply_b, RF_REPLY_N);
    RF_TAKE(rf_evq, RF_EVQ);
    RF_TAKE(sa_evq, SA_EVQ);
    RF_TAKE(sa_mono, APP_BLOCK);
    RF_TAKE(sa_scope, SA_SCOPE_N);
    RF_TAKE(rf_mrx, RF_MRX_N);
    RF_TAKE(rf_gen, RF_GEN_MAX + 8u);
    RF_TAKE(rf_bk_buf, RF_BK_RX_MAX);
    RF_TAKE(rf_bk_net_buf, RF_BK_RX_MAX);
    RF_TAKE(rf_net_rx, RF_NET_RX_N);
    RF_TAKE(rf_net_tx, RF_NET_TX_N);
    RF_TAKE(rf_net_frame, RF_NET_FRAME_N);
    RF_TAKE(rf_stage, RF_STAGE_PX);
    RF_TAKE(sa_lpool, SA_LPOOL);
    RF_TAKE(sa_take, SA_TAKE);
    RF_TAKE(sa_ub_rx, SA_UB_LEN);
    RF_TAKE(sab_lr, 2u * SA_BENCH_FRAMES);
}

/* ------------------------------------------------------------------ the socket task --- */
#ifndef PLAT_HOST
static OS_SEM rf_net_sem;
void rfn_wait(void) { os_sem_pend(&rf_net_sem, 1); }   /* (net/rfm_net.c's task: 10 ms at most) */
static void rf_net_kick(void)
{
    if (((shim_sem_t *)(void *)&rf_net_sem)->count <= 0)
        os_sem_post(&rf_net_sem);
}
#else
void rfn_wait(void) {}
static void rf_net_kick(void) {}
#endif
static uint8_t rf_net_task_on;
static void rf_net_init(void)
{
    rfn_init(rf_net_rx, RF_NET_RX_N, rf_net_tx, RF_NET_TX_N, RFM_NET_TCP_PORT, RFM_NET_UDP_PORT);
#ifndef PLAT_HOST
    os_sem_create(&rf_net_sem, 0);
#endif
}

/* ------------------------------------------------------------------ WIFI --- */
static struct {
    uint8_t due;                                    /* a SET waits for the radio */
    char ssid[RFM_WIFI_SSID_MAX + 1u], pass[RFM_WIFI_PASS_MAX + 1u];
} rf_wf;
static uint32_t rf_wf_sets, rf_wf_ip_seen;
static uint32_t rf_wf_info_ms;

static void rf_wipe(void *p, uint32_t n)
{
    memset(p, 0, n);
    RING_PUBLISH();                                 /* (a compiler barrier: the stores are not left out as dead) */
}

static char *rf_put_ip(char *d, uint32_t ip)        /* "a.b.c.d" (plat_wifi_ip: its first byte lowest) */
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        d = s_int(d, ip >> (8u * i) & 0xFFu);
        if (i < 3u)
            *d++ = '.';
    }
    *d = 0;
    return d;
}

static void rf_net_name(char *n)                    /* "FM-1 XXXX": the unit's Bluetooth name (fm1-base radio.c) */
{
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t *u = plat_chip_uid();
    uint32_t h = 2166136261u, i = 1;
    while (u && i < 16u && u[i] == u[0])
        i++;
    if (u && i < 16u) {
        for (i = 0; i < 16u; i++)
            h = (h ^ u[i]) * 16777619u;
        h ^= h >> 16;
    } else {
        h = 0;
    }
    memcpy(n, "FM-1 ", 5);
    for (i = 0; i < 4u; i++)
        n[5 + i] = hex[h >> (12u - 4u * i) & 15u];
    n[9] = 0;
}

static void rf_wifi_info(void)                      /* discovery's answer (after "RFM1!\n") */
{
    static char t[192];
    char *p = t, n[10];
    rf_net_name(n);
    p = s_put(p, "name=");
    p = s_put(p, n);
    p = s_put(p, "\nid=");
    p = s_put(p, FELUCCA_ID);
    p = s_put(p, "\nver=");
    p = s_put(p, APP_VERSION);
    p = s_put(p, "\nport=");
    p = s_int(p, rfn_port());                     /* (RFM_NET_TCP_PORT; the simulator's rf serve: its own) */
    p = s_put(p, "\nip=");
    p = rf_put_ip(p, plat_wifi_ip());
    p = s_put(p, "\nbusy=");
    p = s_int(p, (uint32_t)rfn_up());
    p = s_put(p, "\nsession=");
    p = s_int(p, (uint32_t)(rfa.session != 0));
    s_put(p, "\n");
    rfn_set_info(t);
}

static void rf_wifi_ms(void)
{
    int st = plat_wifi_state();
    if (rf_wf.due && st != WIFI_OFF && st != WIFI_STARTING) {
        rf_wf.due = 0;
        plat_wifi_join(rf_wf.ssid, rf_wf.pass);    /* (device/wifi.c copies it; kept in OBJ_WIFI once joined) */
        rf_wipe(rf_wf.pass, sizeof rf_wf.pass);
    }
    if (!rf_net_task_on && st == WIFI_CONNECTED) {  /* the sockets once there is an address; they outlive a drop */
        rf_wifi_info();
        if (plat_net_task(rfn_task, 0, "rfm_net") == 0)
            rf_net_task_on = 1;
        else
            rf_net_task_on = 2;                     /* (the simulator: "rf serve" polls the sockets itself) */
    }
    if (plat_ms() - rf_wf_info_ms >= 500u || plat_wifi_ip() != rf_wf_ip_seen) {
        rf_wf_info_ms = plat_ms();
        rf_wf_ip_seen = plat_wifi_ip();
        rf_wifi_info();
    }
}

/* ------------------------------------------------------------------ WIFI DIAG (docs/WIFI.md "Diagnostics") --- */
/* what the main loop, the socket task, the session and lwIP did since the last DIAG (maxima, histograms) or since the
 * start (counters): a version byte, 28 x u32 (DG_*), then rfn_diag_t's 14 x u32, after WIFI_R's sockets' counters */
enum {
    DG_MS, DG_TURN_MAX_US, DG_H0, DG_H1, DG_H2, DG_H3, DG_H4, DG_H5, DG_OUTSIDE_MAX_US, DG_NET_GAP_MAX_MS,
    DG_FRAME_US_MAX, DG_FRAME_US_LAST, DG_FRAME_TURNS_MAX, DG_FRAMES_WHOLE, DG_FRAMES_OVER,
    DG_PANEL_R, DG_PANEL_R_GAP_MAX_MS, DG_TX_DROP, DG_RX_FULL_MS_MAX, DG_RX_FULL_EVENTS,
    DG_SESS_STARTS, DG_END_HOST, DG_END_KEEPALIVE, DG_END_CONN, DG_LAST_RESET, DG_MODE, DG_REFUSED, DG_SESS_T, DG_N
};
#define DG_HIST_N 6u                                /* turn lengths: < 0.25, 1, 4, 16, 64 ms, more */
static uint32_t rf_dg[DG_N];
static uint32_t rf_dg_run_end, rf_dg_pr_ms, rf_dg_polls, rf_dg_polls_ms, rf_dg_full_t0;
static uint8_t rf_dg_full;
static void rf_dg_turn(uint32_t us)
{
    static const uint32_t B[DG_HIST_N - 1u] = {250u, 1000u, 4000u, 16000u, 64000u};
    uint32_t k = 0;
    if (us > rf_dg[DG_TURN_MAX_US])
        rf_dg[DG_TURN_MAX_US] = us;
    while (k < DG_HIST_N - 1u && us >= B[k])
        k++;
    rf_dg[DG_H0 + k]++;
}
static void rf_dg_run(uint32_t ticks)               /* (rf_run's start: since the last one ended, fm1-base's turn) */
{
    static uint32_t last;
    if (last) {
        uint32_t us = (ticks - last) / PLAT_TICKS_PER_US;
        if (us > rf_dg[DG_OUTSIDE_MAX_US])
            rf_dg[DG_OUTSIDE_MAX_US] = us;
    }
    last = ticks;
    (void)rf_dg_run_end;
}
static void rf_dg_panel_r(void)
{
    const uint32_t now = plat_ms();
    rf_dg[DG_PANEL_R]++;
    if (rf_dg_pr_ms && rfa.session && now - rf_dg_pr_ms > rf_dg[DG_PANEL_R_GAP_MAX_MS])
        rf_dg[DG_PANEL_R_GAP_MAX_MS] = now - rf_dg_pr_ms;
    rf_dg_pr_ms = now;
}
static void rf_dg_ms(void)                          /* once a millisecond */
{
    rfn_stats_t ns;
    const uint32_t now = plat_ms();
    rfn_stats(&ns);
    if (ns.polls != rf_dg_polls) {                  /* the socket task's turns: the longest gap between them */
        rf_dg_polls = ns.polls;
        rf_dg_polls_ms = now;
    } else if (rf_net_task_on == 1 && now - rf_dg_polls_ms > rf_dg[DG_NET_GAP_MAX_MS]) {
        rf_dg[DG_NET_GAP_MAX_MS] = now - rf_dg_polls_ms;
    }
    if (rfn_up() && rfn_rx_avail() + 256u > rfn_rx_size()) {   /* TCP's input (nearly) full: how long */
        if (!rf_dg_full) {
            rf_dg_full = 1;
            rf_dg_full_t0 = now;
            rf_dg[DG_RX_FULL_EVENTS]++;
        } else if (now - rf_dg_full_t0 > rf_dg[DG_RX_FULL_MS_MAX]) {
            rf_dg[DG_RX_FULL_MS_MAX] = now - rf_dg_full_t0;
        }
    } else {
        rf_dg_full = 0;
    }
}
static uint32_t rf_dg_put(uint8_t *b)               /* the DIAG block; maxima and histograms start again */
{
    uint32_t i;
    rf_dg[DG_MS] = plat_ms();
    rf_dg[DG_FRAME_US_MAX] = rf_nf_us_max;
    rf_dg[DG_FRAME_US_LAST] = rf_nf_us_last;
    rf_dg[DG_FRAME_TURNS_MAX] = rf_nf_turns_max;
    rf_dg[DG_FRAMES_WHOLE] = rf_nf_frames;
    rf_dg[DG_FRAMES_OVER] = rf_nf_over;
    rf_dg[DG_TX_DROP] = rf_tx_drop[RF_T_NET];
    rf_dg[DG_SESS_STARTS] = rfa.sessions;
    rf_dg[DG_END_HOST] = rfa.end_host;
    rf_dg[DG_END_KEEPALIVE] = rfa.end_keepalive;
    rf_dg[DG_END_CONN] = rf_dg_conn_ends;
    rf_dg[DG_LAST_RESET] = rfa.last_reset;
    rf_dg[DG_MODE] = rf_nf_on;
    rf_dg[DG_REFUSED] = rf_dg_refused;
    rf_dg[DG_SESS_T] = rfa.session ? rfa.st : 0xFFu;   /* the session's transport now (0 USB SysEx, 1 bulk, 3 TCP) */
    for (i = 0; i < DG_N; i++)
        rf_wr32(b + 4u * i, rf_dg[i]);
    rf_dg[DG_TURN_MAX_US] = rf_dg[DG_OUTSIDE_MAX_US] = rf_dg[DG_NET_GAP_MAX_MS] = 0;
    rf_dg[DG_PANEL_R_GAP_MAX_MS] = rf_dg[DG_RX_FULL_MS_MAX] = 0;
    for (i = 0; i < DG_HIST_N; i++)
        rf_dg[DG_H0 + i] = 0;
    rf_nf_us_max = rf_nf_turns_max = 0;
    return 4u * DG_N;
}

static void rf_net_turn(void)                       /* every turn of the loop */
{
    static uint32_t ms;
    if (rfn_tx_pending())
        rf_net_kick();
    if (plat_ms() != ms) {
        ms = plat_ms();
        rf_net_kick();                              /* (a turn a millisecond: what TCP brought, discovery) */
        rf_wifi_ms();
        rf_dg_ms();
    }
#ifdef PLAT_HOST
    hs_net_pump();
#endif
}

static uint32_t rf_wifi_reply(uint32_t op, uint32_t status, uint8_t *b)
{
    const char *ssid = plat_wifi_ssid(), *why = plat_wifi_why();
    uint32_t n, i, k, saved = 0;
    uint32_t rxf = 0;
    int rxr = 0;
    rfn_stats_t ns;
    while (saved < 16u && plat_wifi_saved((int)saved, 0))
        saved++;
    plat_wifi_rx(&rxf, &rxr);
    b[0] = (uint8_t)op;
    b[1] = (uint8_t)status;
    b[2] = (uint8_t)plat_wifi_state();
    b[3] = (uint8_t)(int8_t)plat_wifi_rssi();
    rf_wr32(b + 4, plat_wifi_ip());
    rf_wr16(b + 8, RFM_NET_TCP_PORT);
    rf_wr16(b + 10, RFM_NET_UDP_PORT);
    b[12] = (uint8_t)rfn_up();
    b[13] = (uint8_t)saved;
    rf_wr32(b + 14, RF_HEAP_USED);
    rf_wr32(b + 18, RF_HEAP_PEAK);
    rf_wr32(b + 22, RF_HEAP_SIZE);
    rf_wr16(b + 26, rf_big_bytes >> 4);             /* (16-byte units: the app's own share of the heap) */
    rf_wr16(b + 28, rxf > 0xFFFFu ? 0xFFFFu : rxf);   /* frames heard in the last second (anyone's) */
    n = RFM_WIFI_R_SSID;
    for (k = 0; k < 32u && ssid && ssid[k]; k++)
        b[n + 1u + k] = (uint8_t)ssid[k];
    b[n] = (uint8_t)k;
    n += 1u + k;
    for (k = 0; k < 40u && why && why[k]; k++)
        b[n + 1u + k] = (uint8_t)why[k];
    b[n] = (uint8_t)k;
    n += 1u + k;
    if (op == RFM_WIFI_SAVED) {                     /* the networks kept, the most recent first */
        char s[33];
        b[n++] = (uint8_t)saved;
        for (i = 0; i < saved; i++) {
            plat_wifi_saved((int)i, s);
            for (k = 0; k < 32u && s[k]; k++)
                b[n + 1u + k] = (uint8_t)s[k];
            b[n] = (uint8_t)k;
            n += 1u + k;
        }
    } else {                                        /* the sockets' counters (rfn_stats_t, 11 x u32) */
        rfn_diag_t d;
        rfn_stats(&ns);
        rf_wr32(b + n, ns.accepts);
        rf_wr32(b + n + 4u, ns.closes);
        rf_wr32(b + n + 8u, ns.replaced);
        rf_wr32(b + n + 12u, ns.rx_bytes);
        rf_wr32(b + n + 16u, ns.tx_bytes);
        rf_wr32(b + n + 20u, ns.rx_full);
        rf_wr32(b + n + 24u, ns.tx_eagain);
        rf_wr32(b + n + 28u, ns.udp_in);
        rf_wr32(b + n + 32u, ns.udp_out);
        rf_wr32(b + n + 36u, ns.polls);
        rf_wr32(b + n + 40u, ns.open_fail);
        n += 44u;
        if (op == RFM_WIFI_DIAG || op == RFM_WIFI_MODE) {   /* the diagnostics (WIFI DIAG): the loop's, then lwIP's */
            b[n++] = 1u;                            /* (the block's version) */
            n += rf_dg_put(b + n);
            rfn_diag(&d, 1);
            rf_wr32(b + n, d.nrtx_events);
            rf_wr32(b + n + 4u, d.nrtx_max);
            rf_wr32(b + n + 8u, d.rto_max_ms);
            rf_wr32(b + n + 12u, d.snd_wnd_min);
            rf_wr32(b + n + 16u, d.rcv_wnd_min);
            rf_wr32(b + n + 20u, d.persist_events);
            rf_wr32(b + n + 24u, d.tx_pending_max);
            rf_wr32(b + n + 28u, d.tcp_xmit);
            rf_wr32(b + n + 32u, d.tcp_recv);
            rf_wr32(b + n + 36u, d.tcp_drop);
            rf_wr32(b + n + 40u, d.tcp_memerr);
            rf_wr32(b + n + 44u, d.link_recv);
            rf_wr32(b + n + 48u, d.link_drop);
            rf_wr32(b + n + 52u, d.pbuf_err);
            n += 56u;
        }
    }
    return n;
}

/* SCAN (6) and LIST (7): a page of the scan's networks (the strongest first) or of the networks kept (the most
 * recent first), entries of at most RFM_WIFI_PAGE_MAX bytes, so a reply stays under the Move's USB-A burst limit */
static uint32_t rf_wf_scan_at, rf_wf_scan_base;     /* a scan asked (ms | 1) and plat_wifi_scans() then */
static int rf_wf_scanning(void)
{
    if (rf_wf_scan_at && (plat_wifi_scans() != rf_wf_scan_base || plat_ms() - rf_wf_scan_at >= RFM_WIFI_SCAN_MS))
        rf_wf_scan_at = 0;
    return rf_wf_scan_at != 0;
}
static int rf_wf_heard(const char *ssid, int *rssi, int *sec, int *ch)   /* in the scan's list: its index, or -1 */
{
    char s[33];
    int i;
    for (i = 0; i < 16 && plat_wifi_found(i, s, rssi, sec); i++)
        if (!strcmp(s, ssid)) {
            *ch = plat_wifi_found_channel(i);
            return i;
        }
    return -1;
}
static uint32_t rf_wifi_list(uint32_t op, uint32_t status, uint32_t which, uint32_t first, uint8_t *b)
{
    const char *now = plat_wifi_ssid();
    uint32_t n = RFM_WIFI_LIST_HDR, total = 0, count = 0, k;
    char s[33];
    while (total < 16u && (which == RFM_WIFI_LIST_KEPT ? plat_wifi_saved((int)total, 0)
                                                     : plat_wifi_found((int)total, 0, 0, 0)))
        total++;
    for (k = first; k < total; k++) {
        int rssi = 0, sec = 0, ch = 0, flags = 0;
        uint32_t len;
        if (which == RFM_WIFI_LIST_KEPT) {
            plat_wifi_saved((int)k, s);
            flags = RFM_WIFI_F_KEPT;
            if (rf_wf_heard(s, &rssi, &sec, &ch) >= 0)
                flags |= RFM_WIFI_F_HEARD;
            else
                rssi = sec = ch = 0;
        } else {
            plat_wifi_found((int)k, s, &rssi, &sec);
            ch = plat_wifi_found_channel((int)k);
            flags = RFM_WIFI_F_HEARD | (plat_wifi_kept(s) ? RFM_WIFI_F_KEPT : 0);
        }
        if (sec)
            flags |= RFM_WIFI_F_SECURED;
        if (now && now[0] && !strcmp(now, s))
            flags |= RFM_WIFI_F_JOINED;
        len = (uint32_t)strlen(s);
        if (len > 32u)
            len = 32u;
        if (n + 4u + len > RFM_WIFI_LIST_HDR + RFM_WIFI_PAGE_MAX)
            break;                                  /* (the rest on the next page: first + count) */
        b[n] = (uint8_t)(int8_t)rssi;
        b[n + 1u] = (uint8_t)flags;
        b[n + 2u] = (uint8_t)ch;
        b[n + 3u] = (uint8_t)len;
        memcpy(b + n + 4u, s, len);
        n += 4u + len;
        count++;
    }
    b[0] = (uint8_t)op;
    b[1] = (uint8_t)status;
    b[2] = (uint8_t)plat_wifi_state();
    b[3] = (uint8_t)which;
    b[4] = (uint8_t)rf_wf_scanning();
    b[5] = (uint8_t)plat_wifi_scans();
    b[6] = (uint8_t)total;
    b[7] = (uint8_t)first;
    b[8] = (uint8_t)count;
    return n;
}

static void rf_wifi_cmd(uint32_t t, const uint8_t *body, uint32_t len)
{
    static uint8_t b[RFM_WIFI_R_SSID + 2u + 32u + 40u + 1u + 6u * 33u + 44u + 1u + 4u * DG_N + 56u];
    uint32_t op = len ? body[0] : RFM_WIFI_GET, status = RFM_WIFI_OK;
    int usb = t == RF_T_SX || t == RF_T_BK;         /* (SET and FORGET: not from the network itself) */
    if (op == RFM_WIFI_SET) {
        uint32_t sl = len > 1u ? body[1] : 0u, pl = len > 2u + sl ? body[2u + sl] : 0u, i;
        if (!usb) {
            status = RFM_WIFI_NOT_HERE;
        } else if (!sl || sl > RFM_WIFI_SSID_MAX || pl > RFM_WIFI_PASS_MAX || len != 3u + sl + pl) {
            status = RFM_WIFI_BAD;
        } else {
            for (i = 0; i < sl; i++)
                rf_wf.ssid[i] = (char)body[2u + i];
            rf_wf.ssid[sl] = 0;
            for (i = 0; i < pl; i++)
                rf_wf.pass[i] = (char)body[3u + sl + i];
            rf_wf.pass[pl] = 0;
            rf_wf.due = 1;
            rf_wf_sets++;
            if (plat_wifi_state() == WIFI_OFF)
                plat_wifi_on(1);
        }
        rf_wipe((void *)(uintptr_t)body, len);   /* (the message's copy of the password) */
    } else if (op == RFM_WIFI_FORGET) {
        uint32_t k = len > 1u ? body[1] : 0xFFu;
        if (!usb) {
            status = RFM_WIFI_NOT_HERE;
        } else if (k == 0xFFu) {
            while (plat_wifi_saved(0, 0))
                plat_wifi_forget(0);                /* all of them */
        } else if (!plat_wifi_saved((int)k, 0)) {
            status = RFM_WIFI_BAD;
        } else {
            plat_wifi_forget((int)k);
        }
    } else if (op == RFM_WIFI_MODE) {               /* 05 m: 1 whole frames over TCP, 0 as they come (an A/B) */
        if (len < 2u || body[1] > 1u) {
            status = RFM_WIFI_BAD;
        } else {
            if (!body[1] && rf_nf_n && !rf_nf_ready)
                rf_nf_flush();                      /* (what was kept goes now) */
            rf_nf_on = body[1];
        }
    } else if (op == RFM_WIFI_SCAN || op == RFM_WIFI_LIST) {   /* 06: a scan, then its first page; 07 which first */
        uint32_t which = op == RFM_WIFI_LIST && len > 1u ? body[1] : RFM_WIFI_LIST_SCAN;
        uint32_t first = op == RFM_WIFI_LIST && len > 2u ? body[2] : 0u;
        if (which > RFM_WIFI_LIST_KEPT) {
            status = RFM_WIFI_BAD;
        } else if (op == RFM_WIFI_SCAN) {
            if (plat_wifi_state() == WIFI_OFF)
                plat_wifi_on(1);                    /* (on in a second or so: asked again) */
            if (rf_wf_scanning()) {
                /* (one at a time: this one's results are coming) */
            } else if (plat_wifi_state() == WIFI_ADDRESS ||
                       (plat_wifi_state() == WIFI_JOINING && !(len > 1u && body[1] == 1u))) {
                status = RFM_WIFI_BUSY;             /* (a scan now stops the join: `06 01` asks for it anyway) */
            } else if (plat_wifi_scan() == 0) {
                rf_wf_scan_base = plat_wifi_scans();
                rf_wf_scan_at = plat_ms() | 1u;
            } else {
                status = RFM_WIFI_BUSY;
            }
        }
        rf_send(t, RFM_WIFI_R, b, rf_wifi_list(op, status, which, first, b));
        return;
    } else if (op != RFM_WIFI_GET && op != RFM_WIFI_SAVED && op != RFM_WIFI_DIAG) {
        status = RFM_WIFI_BAD;
    }
    rf_send(t, RFM_WIFI_R, b, rf_wifi_reply(op, status, b));
}

static void rf_wifi_line(char *d)                   /* "WIFI CONNECTED 192.168.1.42" */
{
    static const char *const ST[7] = {"OFF", "STARTING", "ON", "JOINING", "ADDRESS", "", "FAILED"};
    int st = plat_wifi_state();
    char *p = s_put(d, "WIFI ");
    if (st == WIFI_CONNECTED) {
        p = rf_put_ip(p, plat_wifi_ip());
        if (rfn_up())
            s_put(p, " HOST");
    } else {
        s_put(p, st >= 0 && st <= WIFI_FAILED ? ST[st] : "?");
    }
}
