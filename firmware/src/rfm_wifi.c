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
 *                   wiped once handed to the library
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

static void rf_net_turn(void)                       /* every turn of the loop */
{
    static uint32_t ms;
    if (rfn_tx_pending())
        rf_net_kick();
    if (plat_ms() != ms) {
        ms = plat_ms();
        rf_net_kick();                              /* (a turn a millisecond: what TCP brought, discovery) */
        rf_wifi_ms();
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
    }
    return n;
}

static void rf_wifi_cmd(uint32_t t, const uint8_t *body, uint32_t len)
{
    static uint8_t b[RFM_WIFI_R_SSID + 2u + 32u + 40u + 1u + 6u * 33u + 44u];
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
    } else if (op != RFM_WIFI_GET && op != RFM_WIFI_SAVED) {
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
