/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The cross test: the HOST library (session, screen encoder, panel decoder, codec) against the DEVICE's own
 * protocol code (firmware/src/rfm_app.c with rfm_draw.c and rfm_push.c, compiled natively), on a simulated clock.
 * Both sides were written separately from docs/PROTOCOL.md; this proves they read it the same way.
 *
 * The device stand-in is remote.c's main loop in miniature: its rings drained through remote_proto.h's parsers
 * (SysEx: rf_sx_byte, at most 512 bytes a pass; bulk: rf_bk_feed, at most 1024 a pass in pieces of <= 256), the
 * replies framed as remote.c frames them (SysEx 7-in-8, bulk header + its own sequence), panel events queued and
 * flushed as plain MIDI between whole messages (usb.c sends SysEx first, never split by notes), rfm_app_panel once
 * a simulated millisecond with MASTER read every 5 ms, and an LCD whose DMA reads its source a pixel at a time at
 * 20 MHz SPI (1.25 px/us), so pushes overlap with incoming TILES. The links are byte pipes with a rate
 * (host -> device: SysEx 300 KB/s, bulk 850 KB/s), handed to the host's rfm_session_rx in uneven pieces, the way
 * the macOS back ends hand it CoreMIDI packet lists and libusb transfers.
 *
 * After every frame (once settled): host model == device shadow == panel, byte for byte; the host's panel state ==
 * the device's; LEDs as mapped by the device == the host's; no decode error, nothing malformed, nothing unknown;
 * never more than 2 frames in flight on either side. Also: a free-running animation, idling on keepalive and the
 * PANEL_R heartbeat, a lost FRAME_ACK, a lost SESSION ACK, a device-side session loss and a device reboot
 * mid-animation, the device's keepalive expiring while the host is frozen, stop.
 * Bluetooth (run_ble): the unit answers as over Bluetooth (sxq 128), the link carries 6 KB/s, and the device's input
 * waits while a push runs once the session holds (rfm_app_rx_wait, as rfm_main.c's rf_rx_step). With HOLD, after
 * every push the panel is exactly a screen the host committed (the image its TILES built when an END without HOLD
 * went out), and with lossless tiles exactly one of the screens submitted: a moving source shows only whole screens.
 * Without HOLD (ble_hold 0) the same moving source shows screens that never were.
 *   test_cross [a.ppm ...]        (RFM_VERBOSE=1: the session's log) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "frames.h"
#include "rfm/codec.h"
#include "rfm/session.h"
#include "rfm/tiles_ref.h"

/* ---------------------------------------------------------------- the device's own code ---- */
#include "remote_proto.h"
#define RFA_FW_VER "0.1-x"
#define RFA_IDENTITY "FM-1_015"
#include "rfm_draw.c"
#include "rfm_push.c"
#include "rfm_app.c"

#define PASS_US 25u                                     /* one device main loop pass */
#define PX_PER_4US 5u                                   /* 20 MHz SPI: 1.25 px / us */
#define RATE_H2D_SX 0.30                                /* bytes / us */
#define RATE_H2D_BK 0.85
#define RATE_D2H_SX 0.20
#define RATE_D2H_BK 0.85
#define RATE_BLE 0.006                                  /* Bluetooth MIDI, both ways: 6 KB/s */

static uint64_t now_us;
static uint32_t now_ms(void) { return (uint32_t)(now_us / 1000u); }
static int verbose;
static int ble_mode;                                    /* run_ble: the SysEx link is Bluetooth */

/* ---------------------------------------------------------------- pipes ---- */
typedef struct {
    uint8_t *b;
    uint32_t r, w, cap;
    double credit, rate;
    uint64_t total;
} pipe_t;
static void pipe_init(pipe_t *p, double rate)
{
    free(p->b);
    memset(p, 0, sizeof *p);
    p->rate = rate;
}
static void pipe_put(pipe_t *p, const uint8_t *d, uint32_t n)
{
    if (p->r && p->r == p->w)
        p->r = p->w = 0;
    if (p->w + n > p->cap) {
        if (p->r) {
            memmove(p->b, p->b + p->r, p->w - p->r);
            p->w -= p->r;
            p->r = 0;
        }
        if (p->w + n > p->cap) {
            p->cap = (p->w + n) * 2u + 4096u;
            p->b = (uint8_t *)realloc(p->b, p->cap);
        }
    }
    memcpy(p->b + p->w, d, n);
    p->w += n;
    p->total += n;
}
static uint32_t pipe_len(const pipe_t *p) { return p->w - p->r; }
static void pipe_tick(pipe_t *p, uint32_t us)
{
    p->credit += p->rate * us;
    if (p->credit > 8192.0)
        p->credit = 8192.0;
}
static uint32_t pipe_take(pipe_t *p, uint32_t max, const uint8_t **d)    /* what the wire has carried */
{
    uint32_t n = pipe_len(p);
    if (n > (uint32_t)p->credit)
        n = (uint32_t)p->credit;
    if (n > max)
        n = max;
    *d = p->b + p->r;
    p->r += n;
    p->credit -= n;
    return n;
}

static pipe_t h2d[2], d2h[2];                           /* [RF_T_SX], [RF_T_BK] */

/* ---------------------------------------------------------------- the device stand-in ---- */
static uint16_t shadow[RFM_W * RFM_H], stage[RFM_W * 16u];
static uint16_t panel[RFM_W * RFM_H];
static rf_tstats_t dst[2];
static rf_sx_t dsx;
static rf_bk_t dbk;
static uint8_t dsx_buf[RF_SX_BODY_MAX], dbk_buf[RFM_BK_BODY_MAX];
static uint32_t dbk_seq;
static uint8_t heard;
static struct {
    uint32_t tx[256], rx[256];                          /* messages by command */
    uint32_t diag, frame_acks, ack_err, ack_reset, tiles_in_push, max_qn, leds;
    uint32_t drop_frame_acks, drop_acks;                /* faults: replies lost on the way */
    uint32_t dropped_frame_acks, dropped_acks;
    uint64_t ack_bytes, ack_us;
    uint8_t lit[RFM_NCOL], glow[RFM_NCOL], bg[RFM_NCOL];
    int session_on;
    /* the panel as the hardware is */
    uint32_t keys, buttons;
    int32_t steps[RFM_NENC];
    int32_t master;
    uint32_t last_ms, master_ms;
    uint32_t evq[1024], ev_w, ev_r;
} D;

static int rfa_tx(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t w[RF_BK_HDR + RFM_BK_BODY_MAX + 4096];
    uint32_t k;
    D.tx[cmd & 0xFFu]++;
    if (cmd == RFM_FRAME_ACK) {
        D.frame_acks++;
        D.ack_err += body[2] & RFM_ACK_DECODE_ERR ? 1u : 0u;
        D.ack_reset += body[2] & RFM_ACK_SHADOW_RESET ? 1u : 0u;
        D.ack_us += rf_rd32(body + 3);
        D.ack_bytes += rf_rd32(body + 7);
        if (D.drop_frame_acks) {
            D.drop_frame_acks--;
            D.dropped_frame_acks++;
            return 1;
        }
    }
    if (cmd == RFM_ACK && D.drop_acks) {
        D.drop_acks--;
        D.dropped_acks++;
        return 1;
    }
    if (t == RF_T_SX) {
        k = rf_sx_encode(cmd, body, n, w);
    } else {
        rf_bk_header(w, cmd, n, dbk_seq++);
        memcpy(w + RF_BK_HDR, body, n);
        k = RF_BK_HDR + n;
    }
    pipe_put(&d2h[t], w, k);
    return 1;
}
static void rfa_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    CHECK(D.ev_w - D.ev_r < 1024u);
    D.evq[D.ev_w++ % 1024u] = st | d1 << 8 | d2 << 16;
}
static void ev_flush(void)                              /* remote.c rf_ev_flush: between whole messages */
{
    while (D.ev_r != D.ev_w) {
        uint32_t e = D.evq[D.ev_r++ % 1024u];
        uint8_t m[3];
        m[0] = (uint8_t)e, m[1] = (uint8_t)(e >> 8), m[2] = (uint8_t)(e >> 16);
        pipe_put(&d2h[RF_T_SX], m, 3);
    }
}
static void rfa_leds(const uint8_t *lit, const uint8_t *glow, const uint8_t *bg)
{
    memcpy(D.lit, lit, RFM_NCOL);
    memcpy(D.glow, glow, RFM_NCOL);
    memcpy(D.bg, bg, RFM_NCOL);
    D.leds++;
}
static void rfa_hello_dev(uint8_t *h, uint32_t t)      /* as remote.c fills it */
{
    (void)t;
    h[25] = (uint8_t)(1u | 2u | (uint32_t)heard << 2);
    h[26] = 1;
    h[27] = 2;
    h[28] = 0;
    rf_wr16(h + 29, ble_mode ? 128u : 4096u - 512u);  /* (Bluetooth: rfm_main.c's BLE_OUT_Q) */
    if (ble_mode)
        h[25] |= RFM_TR_BLE | RFM_TR_BLE_HEARD;
    rf_wr32(h + 31, 32768u);
    rf_wr32(h + 35, 8192u);
    rf_wr16(h + 39, RF_SX_BODY_MAX);
    rf_wr16(h + 41, RF_BK_BODY_MAX);
}
static void rfa_session_changed(uint32_t on) { D.session_on = (int)on; }
static uint32_t rfa_us(void) { return (uint32_t)now_us; }
static void rfa_diag(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    (void)t, (void)body, (void)len;
    D.diag++;
    fprintf(stderr, "device: unexpected command 0x%02x (%u bytes)\n", cmd, len);
}

/* the panel and its DMA */
static struct {
    int busy;
    uint64_t t0;
    const uint16_t *src;
    uint32_t n, done, x0, y0, x1, y1, cx, cy, bad, windows, xfers;
} L;
static void lcd_advance(void)
{
    uint32_t want, i;
    if (!L.busy)
        return;
    want = (uint32_t)((now_us - L.t0) * PX_PER_4US / 4u);
    if (want > L.n)
        want = L.n;
    for (i = L.done; i < want; i++) {                   /* the DMA reads its source as it goes */
        if (L.cy > L.y1)
            L.bad++;
        else
            panel[L.cy * RFM_W + L.cx] = L.src[i];
        if (++L.cx > L.x1) {
            L.cx = L.x0;
            L.cy++;
        }
    }
    L.done = want;
    if (L.done == L.n)
        L.busy = 0;
}
static int lcd_idle(void *c)
{
    (void)c;
    lcd_advance();
    return !L.busy;
}
static void lcd_window(void *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    (void)c;
    L.bad += L.busy || x1 >= RFM_W || y1 >= RFM_H || x0 > x1 || y0 > y1;
    L.x0 = L.cx = x0, L.y0 = L.cy = y0, L.x1 = x1, L.y1 = y1;
    L.windows++;
}
static void lcd_data(void *c, const void *p, uint32_t n)
{
    (void)c;
    L.bad += L.busy || (n & 1u);
    L.src = (const uint16_t *)p;
    L.n = n / 2u;
    L.done = 0;
    L.t0 = now_us;
    L.busy = 1;
    L.xfers++;
}
static const rfm_lcd_ops_t OPS = {lcd_idle, lcd_window, lcd_data, NULL};

static void dev_on_msg(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    uint32_t t = (uint32_t)(uintptr_t)ctx;
    heard |= (uint8_t)(1u << t);
    D.rx[cmd & 0xFFu]++;
    if (cmd == RFM_TILES && rfa.push.active)
        D.tiles_in_push++;
    rfm_app_msg(t, cmd, body, len);
}

/* power on (or a reboot: everything the device holds is gone, the link bytes in flight to it too) */
static void dev_boot(int power_on)
{
    uint32_t i;
    memset(&rfa, 0, sizeof rfa);
    memset(&dsx, 0, sizeof dsx);
    memset(&dbk, 0, sizeof dbk);
    memset(dst, 0, sizeof dst);
    {
        uint32_t xfers = L.xfers;
        memset(&L, 0, sizeof L);
        L.xfers = power_on ? 0u : xfers;
    }
    if (power_on) {                                     /* (a reboot keeps the test's counters) */
        memset(&D, 0, sizeof D);
        D.master = 512;
    }
    memset(D.steps, 0, sizeof D.steps);
    D.ev_r = D.ev_w = 0;
    D.master_ms = 0;
    D.last_ms = now_ms();
    D.session_on = -1;
    dbk_seq = 0;
    heard = 0;
    for (i = 0; i < RFM_W * RFM_H; i++) {
        shadow[i] = (uint16_t)(0xA55Au ^ i);            /* whatever the RAM held */
        panel[i] = (uint16_t)(0x5AA5u + i);
    }
    rf_body_init(&dsx.b, &dst[RF_T_SX], dsx_buf, sizeof dsx_buf, dev_on_msg, (void *)(uintptr_t)RF_T_SX);
    rf_body_init(&dbk.b, &dst[RF_T_BK], dbk_buf, sizeof dbk_buf, dev_on_msg, (void *)(uintptr_t)RF_T_BK);
    rfm_app_init(shadow, stage, RFM_W * 16u, &OPS);
    pipe_init(&h2d[RF_T_SX], ble_mode ? RATE_BLE : RATE_H2D_SX);
    pipe_init(&h2d[RF_T_BK], RATE_H2D_BK);
}

/* the panel after a push, against the screens it may show (run_ble): FNV-1a 64 of 240 x 240 wire pixels */
#define NHASH 8192u
typedef struct {
    uint64_t h[NHASH];
    uint32_t n;
} hset_t;
static hset_t committed, sources;                       /* the host's committed images; the screens submitted */
static int coherence;                                   /* 0 off, 1 committed, 2 sources */
static uint32_t pushes_seen, incoherent, last_xfers;
static uint64_t fnv(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static void hset_add(hset_t *s, uint64_t h) { s->h[s->n++ % NHASH] = h; }
static int hset_has(const hset_t *s, uint64_t h)
{
    uint32_t i, n = s->n < NHASH ? s->n : NHASH;
    for (i = 0; i < n; i++)
        if (s->h[i] == h)
            return 1;
    return 0;
}
static void check_coherent(void)
{
    uint64_t h;
    if (!coherence || L.xfers == last_xfers || L.busy || rfa.push.active)
        return;
    last_xfers = L.xfers;
    pushes_seen++;
    h = fnv(panel, sizeof panel);
    if (!hset_has(coherence == 1 ? &committed : &sources, h)) {
        if (verbose && incoherent < 5)
            printf("    %7u ms  the panel shows a screen that was never %s\n", now_ms(),
                   coherence == 1 ? "committed" : "submitted");
        incoherent++;
    }
}

/* what the host sends on the Bluetooth link, decoded by the reference decoder: the image it builds, committed at
 * every END without HOLD, black at every SESSION start */
static rfm_rx_t *hmon;
static uint8_t hsent[RFM_W * RFM_H * 2];
static uint32_t h_held, h_commits;
static void hmon_msg(void *ctx, uint8_t cmd, const uint8_t *b, uint32_t n)
{
    rfm_tiles_info_t info;
    (void)ctx;
    if (cmd == RFM_SESSION && n >= 1 && b[0]) {
        memset(hsent, 0, sizeof hsent);
        hset_add(&committed, fnv(hsent, sizeof hsent));
    } else if (cmd == RFM_TILES && rfm_tiles_apply(hsent, b, n, 0, &info) != RFM_REF_SHORT_HDR &&
               (info.flags & RFM_TILES_F_END)) {
        if (info.flags & RFM_TILES_F_HOLD) {
            h_held++;
        } else {
            h_commits++;
            hset_add(&committed, fnv(hsent, sizeof hsent));
        }
    }
}

/* once a millisecond (remote.c rf_ms_step): the panel to rfm_app.c, then the events out */
static void dev_ms(void)
{
    int32_t master = -1;
    uint32_t e;
    if (now_ms() - D.master_ms >= RFA_MASTER_MS) {
        D.master_ms = now_ms();
        master = D.master;
    }
    rfm_app_panel(D.keys, D.buttons, D.steps, master);
    for (e = 0; e < RFM_NENC; e++)
        D.steps[e] = 0;
    ev_flush();
}

static void dev_pass(void)
{
    const uint8_t *p;
    uint32_t n, i, got = 0;
    if (now_ms() != D.last_ms) {
        D.last_ms = now_ms();
        dev_ms();
    }
    dsx.b.now_ms = dbk.b.now_ms = now_ms();
    for (i = 0; i < 512u && !rfm_app_rx_wait(RF_T_SX); i++) {   /* remote.c rf_rx_step: a byte at a time, none
                                                                 * while a held session's push runs or is due */
        if (!pipe_take(&h2d[RF_T_SX], 1u, &p))
            break;
        rf_sx_byte(&dsx, p[0]);
        got++;
    }
    n = rfm_app_rx_wait(RF_T_BK) ? 0u : pipe_take(&h2d[RF_T_BK], 1024u, &p);
    for (i = 0; i < n; i += 256u)
        rf_bk_feed(&dbk, p + i, n - i < 256u ? n - i : 256u);
    got += n;
    if (got)
        rfm_app_activity();
    rfm_app_poll(now_ms());
    lcd_advance();
    check_coherent();
    if (rfa.qn > D.max_qn)
        D.max_qn = rfa.qn;
}

/* ---------------------------------------------------------------- the host ---- */
typedef struct {
    const char *name;
    int transport;                                      /* RFM_T_BULK (both links) or RFM_T_MIDI (MIDI only) */
    uint32_t sx_body_max, budget;
} cfg_t;

static rfm_session_t *S;
static int sx_id, bk_id, decoy_id;
static uint32_t host_frozen;
static uint32_t decoy_bytes;
static rfm_rx_t *mon[2];                                /* the device's output as a second parser sees it */
static uint32_t mon_cmd[256], mon_midi;
static uint32_t state_changes[8];
static uint32_t chunk_seed = 7;

static int w_dev(void *ctx, const uint8_t *p, uint32_t n)
{
    pipe_put(&h2d[(uintptr_t)ctx], p, n);
    if (hmon && (uintptr_t)ctx == RF_T_SX)
        rfm_rx_feed(hmon, p, n);
    return 0;
}
static int w_decoy(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx, (void)p;
    decoy_bytes += n;                                   /* another port: some synth that ignores us */
    return 0;
}
static void cb_state(void *ctx, int st, const char *why)
{
    (void)ctx;
    state_changes[st]++;
    if (verbose)
        printf("    %7u ms  %s (%s)\n", now_ms(), rfm_session_state_name(st), why);
}
static void cb_log(void *ctx, const char *m)
{
    (void)ctx;
    if (verbose)
        printf("    %7u ms  %s\n", now_ms(), m);
}
static void mon_msg(void *ctx, uint8_t cmd, const uint8_t *b, uint32_t n)
{
    (void)ctx, (void)b, (void)n;
    mon_cmd[cmd]++;
}
static void mon_midi_fn(void *ctx, const uint8_t *m, uint32_t n)
{
    (void)ctx, (void)m, (void)n;
    mon_midi++;
}

/* the device's bytes to the host, in uneven pieces (CoreMIDI packet lists, libusb transfers) */
static void host_rx(int t, int id)
{
    const uint8_t *p;
    uint32_t n = pipe_take(&d2h[t], 1u << 20, &p), o = 0;
    while (o < n) {
        uint32_t c = 1u + fr_rand(&chunk_seed) % (t == RF_T_SX ? 300u : 2000u);
        if (c > n - o)
            c = n - o;
        rfm_rx_feed(mon[t], p + o, c);
        if (id >= 0)
            rfm_session_rx(S, id, p + o, c, now_ms());
        o += c;
    }
}

static void host_ms(void)
{
    rfm_session_stats_t st;
    if (host_frozen || !S)
        return;
    host_rx(RF_T_SX, sx_id);
    host_rx(RF_T_BK, bk_id);
    rfm_session_poll(S, now_ms());
    rfm_session_stats(S, &st);
    CHECKF(st.in_flight <= RFM_FRAMES_IN_FLIGHT, "host: %u frames in flight", st.in_flight);
}

/* one simulated millisecond */
static void step_ms(void)
{
    uint32_t k, t;
    host_ms();
    for (k = 0; k < 1000u / PASS_US; k++) {
        now_us += PASS_US;
        for (t = 0; t < 2; t++) {
            pipe_tick(&h2d[t], PASS_US);
            pipe_tick(&d2h[t], PASS_US);
        }
        dev_pass();
    }
}
static void run_ms(uint32_t ms)
{
    while (ms--)
        step_ms();
}

/* ---------------------------------------------------------------- checks ---- */
static uint32_t errs_seen;
static void check_clean(const char *where)
{
    rfm_rx_stats_t a, b;
    uint32_t t;
    for (t = 0; t < 2; t++)
        CHECKF(!dst[t].bad && !dst[t].sync && !dst[t].frame_gaps && !dst[t].ovf, "%s: device %s: bad %u sync %u gaps %u",
               where, t ? "bulk" : "sysex", dst[t].bad, dst[t].sync, dst[t].frame_gaps);
    CHECKF(!rfa.bad, "%s: device: %u malformed commands", where, rfa.bad);
    CHECKF(!D.diag, "%s: device: %u unknown commands", where, D.diag);
    CHECKF(rfa.decode_errs == errs_seen, "%s: device: %u decode errors", where, rfa.decode_errs);
    CHECKF(!D.ack_err, "%s: %u FRAME_ACKs with the decode-error bit", where, D.ack_err);
    CHECKF(!L.bad, "%s: the LCD was driven wrong %u times", where, L.bad);
    CHECKF(D.max_qn <= RFM_FRAMES_IN_FLIGHT, "%s: device: %u frames queued", where, D.max_qn);
    rfm_rx_stats(mon[0], &a);
    rfm_rx_stats(mon[1], &b);
    CHECKF(!a.bad && !a.sync && !b.bad && !b.sync, "%s: host-side parse of the device's output: bad %u/%u sync %u/%u",
           where, a.bad, b.bad, a.sync, b.sync);
}

static int dev_idle(void)
{
    return !rfa.push.active && !rfa.qn && !rfa.flush && !rfm_push_pending(&rfa.push) && !L.busy &&
           D.ev_r == D.ev_w;
}
static int quiet(void)
{
    rfm_session_stats_t st;
    rfm_session_stats(S, &st);
    return rfm_session_state(S) == RFM_S_ACTIVE && !st.pending && !st.in_flight && !pipe_len(&h2d[0]) &&
           !pipe_len(&h2d[1]) && !pipe_len(&d2h[0]) && !pipe_len(&d2h[1]) && dev_idle();
}
static int settle(uint32_t max_ms)                      /* ms taken, or -1 */
{
    uint32_t ms = 0;
    while (!quiet()) {
        if (ms++ >= max_ms)
            return -1;
        step_ms();
    }
    return (int)ms;
}

static int screens_equal(const char *where, const uint16_t *target)
{
    const uint16_t *model = rfm_screen_model(rfm_session_screen(S));
    int ok = 1;
    if (target && memcmp(model, target, sizeof shadow)) {
        CHECKF(0, "%s: host model != the frame submitted", where);
        ok = 0;
    }
    if (memcmp(model, shadow, sizeof shadow)) {
        uint32_t i;
        for (i = 0; i < RFM_W * RFM_H && model[i] == shadow[i]; i++)
            ;
        CHECKF(0, "%s: host model != device shadow (first at x %u y %u: %04x vs %04x)", where, i % RFM_W, i / RFM_W,
               model[i], shadow[i]);
        ok = 0;
    }
    if (memcmp(shadow, panel, sizeof shadow)) {
        uint32_t i;
        for (i = 0; i < RFM_W * RFM_H && panel[i] == shadow[i]; i++)
            ;
        CHECKF(0, "%s: device shadow != panel (first at x %u y %u: %04x vs %04x)", where, i % RFM_W, i / RFM_W,
               shadow[i], panel[i]);
        ok = 0;
    }
    return ok;
}

static uint32_t last_master_change;
static void panel_equal(const char *where)
{
    const rfm_panel_t *p = rfm_session_panel(S);
    uint32_t e;
    CHECKF(p->keys == rfa.keys, "%s: keys host %07x device %07x", where, p->keys, rfa.keys);
    CHECKF(p->buttons == rfa.buttons, "%s: buttons host %04x device %04x", where, p->buttons, rfa.buttons);
    for (e = 0; e < RFM_NENC; e++)
        CHECKF(p->enc_count[e] == rfa.enc[e], "%s: encoder %u host %d device %d", where, e, p->enc_count[e],
               rfa.enc[e]);
    if (now_ms() - last_master_change > 1100u)          /* two PANEL_Rs since the last move */
        CHECKF(p->master == rfa.master, "%s: MASTER host %u device %d", where, p->master, rfa.master);
    else
        CHECKF(abs((int)p->master - (int)rfa.master) <= 1, "%s: MASTER host %u device %d", where, p->master,
               rfa.master);
}

static rfm_leds_t leds;
static void leds_equal(const char *where)
{
    uint32_t c;
    for (c = 0; c < RFM_NCOL; c++) {                    /* section 3: lit -> lit, mid and dim -> glow, faint -> bg */
        uint8_t lit = leds.lit[c] | (c == 8 && (leds.flags & RFM_LEDS_F_PLAY_GREEN) ? 0x02 : 0);
        uint8_t glow = (uint8_t)((leds.mid[c] | leds.dim[c]) & ~lit);
        uint8_t bg = (uint8_t)(leds.faint[c] & ~lit & ~glow);
        CHECKF(D.lit[c] == lit && D.glow[c] == glow && D.bg[c] == bg, "%s: LED column %u", where, c);
    }
}

/* ---------------------------------------------------------------- panel input ---- */
static uint32_t pin_seed = 12345;
static void inject(int big)
{
    uint32_t r = fr_rand(&pin_seed), k;
    switch (r % 5u) {
    case 0:
        D.keys ^= 1u << (fr_rand(&pin_seed) % RFM_NKEYS);
        break;
    case 1:
        D.buttons ^= 1u << (fr_rand(&pin_seed) % RFM_NBTN);
        break;
    case 2:
        k = fr_rand(&pin_seed) % RFM_NENC;
        D.steps[k] += big ? (r & 8u ? 150 : -100) : (int32_t)(fr_rand(&pin_seed) % 7u) - 3;
        break;
    case 3:
        D.master = (int32_t)(fr_rand(&pin_seed) % 1024u);
        last_master_change = now_ms();
        break;
    default:
        D.master += r & 16u ? 1 : -1;                   /* inside the dead band: only PANEL_R tells */
        if (D.master < 0)
            D.master = 1;
        if (D.master > 1023)
            D.master = 1022;
        last_master_change = now_ms();
        break;
    }
}
static void new_leds(void)
{
    uint32_t c;
    for (c = 0; c < RFM_NCOL; c++) {
        leds.lit[c] = (uint8_t)fr_rand(&pin_seed);
        leds.mid[c] = (uint8_t)fr_rand(&pin_seed);
        leds.dim[c] = (uint8_t)fr_rand(&pin_seed);
        leds.faint[c] = (uint8_t)fr_rand(&pin_seed);
    }
    leds.flags = (uint8_t)(fr_rand(&pin_seed) & 1u);
    rfm_session_set_leds(S, &leds);
}

/* ---------------------------------------------------------------- frame sets ---- */
enum { SET_UI, SET_TEXT, SET_NOISE, SET_GRADIENT, SET_MIXED, NSETS };
static const char *set_names[NSETS] = {"ui", "ui-static", "noise", "gradient", "mixed"};
static void gen(int set, uint32_t t, uint16_t *f, uint32_t *seed)    /* test_screen.c's sets */
{
    switch (set) {
    case SET_UI:
        fr_ui(f, t);
        break;
    case SET_TEXT:
        fr_ui(f, 0);
        if (t) {
            char b[16];
            snprintf(b, sizeof b, "%3u", t % 128u);
            fr_text(f, 20, 60, b, fr_rgb(240, 240, 240), fr_rgb(36, 40, 52));
        }
        break;
    case SET_NOISE:
        fr_noise(f, seed);
        break;
    case SET_GRADIENT:
        fr_gradient(f, t * 3u);
        break;
    default: {
        uint32_t k = (t / 7u) % 4u, i;
        if (k == 0)
            fr_ui(f, t);
        else if (k == 1)
            fr_gradient(f, t);
        else if (k == 2)
            fr_noise(f, seed);
        else
            fr_fill(f, 0);
        for (i = 0; i < 5; i++)
            fr_rect(f, (int)(fr_rand(seed) % 240u), (int)(fr_rand(seed) % 240u), (int)(fr_rand(seed) % 40u) + 1,
                    (int)(fr_rand(seed) % 40u) + 1, (uint16_t)fr_rand(seed));
        break;
    }
    }
}

typedef struct {
    uint32_t screens, frames, frames_max, bytes, first_bytes, first_frames, ms, ms_max;
    uint64_t us;
} meas_t;

/* one screen, then wait until everything has settled; frames and bytes as the device counted them */
static int show(const char *where, const uint16_t *f, meas_t *m)
{
    uint32_t ends0 = D.frame_acks + D.dropped_frame_acks;
    uint64_t bytes0 = D.ack_bytes, us0 = D.ack_us;
    int ms;
    rfm_session_set_frame(S, f);
    ms = settle(20000);
    CHECKF(ms >= 0, "%s: did not settle", where);
    if (ms < 0)
        return 0;
    if (m) {
        uint32_t fr = D.frame_acks - ends0;
        if (!m->screens) {
            m->first_bytes = (uint32_t)(D.ack_bytes - bytes0);
            m->first_frames = fr;
        } else {
            m->frames += fr;
            m->bytes += (uint32_t)(D.ack_bytes - bytes0);
            m->ms += (uint32_t)ms;
            if (fr > m->frames_max)
                m->frames_max = fr;
            if ((uint32_t)ms > m->ms_max)
                m->ms_max = (uint32_t)ms;
        }
        m->us += D.ack_us - us0;
        m->screens++;
    }
    return screens_equal(where, f);
}

/* ---------------------------------------------------------------- one configuration ---- */
static uint16_t fbuf[FR_N], gbuf[FR_N];

static void run_cfg(const cfg_t *c, int nppm, char **ppm)
{
    rfm_session_cfg_t sc;
    rfm_session_cb_t cb;
    rfm_session_stats_t st;
    const rfm_hello_t *h;
    uint32_t t, set, seed = 1, sessions, panel_r0, i;
    char where[96];

    printf("\n== %s: body max %u\n", c->name,
           c->transport == RFM_T_BULK ? RFM_BK_BODY_MAX : c->sx_body_max ? c->sx_body_max : RFM_SX_BODY_MAX);
    now_us = 5000000;                                   /* 5 s after power-on */
    dev_boot(1);
    pipe_init(&d2h[RF_T_SX], RATE_D2H_SX);
    pipe_init(&d2h[RF_T_BK], RATE_D2H_BK);
    memset(state_changes, 0, sizeof state_changes);
    memset(mon_cmd, 0, sizeof mon_cmd);
    mon_midi = decoy_bytes = 0;
    errs_seen = 0;
    host_frozen = 0;
    last_master_change = 0;
    memset(&leds, 0, sizeof leds);
    run_ms(20);

    rfm_session_cfg_default(&sc);
    sc.transport = c->transport;
    sc.sx_body_max = c->sx_body_max;
    sc.budget = c->budget;
    memset(&cb, 0, sizeof cb);
    cb.state = cb_state;
    cb.log = cb_log;
    S = rfm_session_new(&sc, &cb);
    for (t = 0; t < 2; t++) {
        rfm_rx_free(mon[t]);
        mon[t] = rfm_rx_new(t ? RFM_LINK_BK : RFM_LINK_SX, t ? RFM_BK_BODY_MAX + 64u : RFM_SX_BODY_MAX + 64u, mon_msg,
                            t ? NULL : mon_midi_fn, NULL);
    }
    decoy_id = rfm_session_add_link(S, RFM_LINK_SX, w_decoy, NULL, "decoy synth");
    sx_id = rfm_session_add_link(S, RFM_LINK_SX, w_dev, (void *)(uintptr_t)RF_T_SX, "FM-1");
    bk_id = c->transport == RFM_T_BULK ? rfm_session_add_link(S, RFM_LINK_BK, w_dev, (void *)(uintptr_t)RF_T_BK,
                                                              "FM-1 bulk")
                                       : -1;

    /* ---- discovery by HELLO, SESSION start */
    for (i = 0; i < 2000 && rfm_session_state(S) != RFM_S_ACTIVE; i++)
        step_ms();
    CHECKF(rfm_session_state(S) == RFM_S_ACTIVE, "%s: no session after %u ms", c->name, i);
    printf("  discovery + SESSION: active after %u ms\n", i);
    h = rfm_session_hello(S);
    CHECK(!strcmp(h->name, "RemoteFM1") && h->proto == 2 && h->v2 && h->screen_w == 240 && h->screen_h == 240);
    CHECK(h->caps == (RFM_CAP_SHADOW | RFM_CAP_COPY | RFM_CAP_RLE | RFM_CAP_PAL | RFM_CAP_PLAY_GREEN | RFM_CAP_HOLD));
    CHECK(h->max_in_flight == 2 && !strcmp(h->fw_ver, "0.1-x") && !strcmp(h->identity, "FM-1_015"));
    CHECK(h->sx_body_max == RF_SX_BODY_MAX && h->bk_body_max == RF_BK_BODY_MAX);
    CHECK(rfm_session_frame_link(S) == (c->transport == RFM_T_BULK ? bk_id : sx_id));
    CHECK(rfm_session_midi_link(S) == sx_id);
    CHECK(rfa.session && rfa.st == (c->transport == RFM_T_BULK ? RF_T_BK : RF_T_SX) && D.session_on == 1);
    CHECK(rfa.keepalive == RFM_KEEPALIVE_MS_DEFAULT);
    CHECK(decoy_bytes > 0);
    printf("  budget in use %u B/frame\n", rfm_session_budget(S));
    /* the first screen: black, and the panel black too (it held garbage) */
    CHECK(settle(2000) >= 0);
    fr_fill(fbuf, 0);
    CHECK(screens_equal("session start", fbuf));
    CHECK(rfm_session_panel(S)->master == 512 && rfm_session_panel(S)->have_master);

    /* ---- the frame sets, settled after every frame, with LEDs and panel input along the way */
    printf("  B/screen, frames, ms: per screen after the first (frames and bytes as the device's FRAME_ACKs count "
           "them); draw: the device's decode + push per frame\n");
    printf("  %-14s | %8s %7s | %9s %8s %8s | %8s %7s | %8s\n", "frame set", "first B", "frames", "B/screen", "frames",
           "max fr", "ms", "max ms", "draw ms");
    for (set = 0; set < NSETS + (uint32_t)nppm; set++) {
        meas_t m;
        const uint32_t n = set == SET_NOISE ? 6u : set < NSETS ? 16u : 1u;
        const char *name = set < NSETS ? set_names[set] : strrchr(ppm[set - NSETS], '/') ? strrchr(ppm[set - NSETS], '/') + 1
                                                                                     : ppm[set - NSETS];
        memset(&m, 0, sizeof m);
        if (set >= NSETS) {
            if (fr_load_ppm(ppm[set - NSETS], fbuf)) {
                printf("  %s: cannot read\n", name);
                continue;
            }
            fr_fill(gbuf, 0);                           /* from black, then to the next screenshot, and back */
            show("ppm black", gbuf, NULL);
            snprintf(where, sizeof where, "%s %s", c->name, name);
            show(where, fbuf, &m);
            if (set + 1u < NSETS + (uint32_t)nppm && !fr_load_ppm(ppm[set - NSETS + 1], gbuf)) {
                show(where, gbuf, &m);
                show(where, fbuf, &m);
            }
        } else {
            for (t = 0; t <= n; t++) {
                gen((int)set, t, fbuf, &seed);
                if (t % 3u == 1u)
                    new_leds();
                if (t % 2u == 0u)
                    inject(t % 8u == 4u);
                snprintf(where, sizeof where, "%s %s frame %u", c->name, name, t);
                if (!show(where, fbuf, &m))
                    break;
                panel_equal(where);
                leds_equal(where);
            }
        }
        check_clean(name);
        if (m.screens > 1)
            printf("  %-14s | %8u %7u | %9u %8.2f %8u | %8u %7u | %8.1f\n", name, m.first_bytes, m.first_frames,
                   m.bytes / (m.screens - 1u), (double)m.frames / (m.screens - 1u), m.frames_max,
                   m.ms / (m.screens - 1u), m.ms_max, m.us / 1000.0 / (m.frames + m.first_frames));
        else
            printf("  %-14s | %8u %7u |\n", name, m.first_bytes, m.first_frames);
    }

    /* ---- free-running: a new UI screen every 50 ms (pushes overlap incoming TILES), input all along */
    {
        uint32_t f0 = D.frame_acks, tip0 = D.tiles_in_push;
        int ms;
        for (t = 0; t < 60; t++) {
            fr_ui(fbuf, 100u + t);
            fr_rect(fbuf, (int)(t * 3u), (int)(t * 2u), 50, 37, fr_rgb(t * 4u, 255u - t * 4u, 128));
            rfm_session_set_frame(S, fbuf);
            inject(t == 30);
            if (t % 10u == 0u)
                new_leds();
            run_ms(50);
        }
        ms = settle(5000);
        CHECKF(ms >= 0, "%s free-running: did not settle", c->name);
        screens_equal("free-running", fbuf);
        run_ms(1200);
        panel_equal("free-running");
        leds_equal("free-running");
        check_clean("free-running");
        printf("  free-running 3 s at 20 Hz: %u frames acknowledged, %u TILES arrived during a push, settled %d ms "
               "after the last screen\n",
               D.frame_acks - f0, D.tiles_in_push - tip0, ms);
    }

    /* ---- idle: keepalive and the PANEL_R heartbeat keep the session */
    rfm_session_stats(S, &st);
    sessions = rfa.sessions;
    panel_r0 = D.tx[RFM_PANEL_R];
    {
        uint32_t ka0 = st.keepalives, leds0 = D.rx[RFM_LEDS];
        run_ms(7000);
        rfm_session_stats(S, &st);
        CHECKF(rfa.session && rfa.sessions == sessions && rfm_session_state(S) == RFM_S_ACTIVE,
               "%s idle: the session dropped", c->name);
        CHECKF(D.tx[RFM_PANEL_R] - panel_r0 >= 13u && D.tx[RFM_PANEL_R] - panel_r0 <= 15u, "PANEL_R x %u in 7 s",
               D.tx[RFM_PANEL_R] - panel_r0);
        CHECK(D.rx[RFM_LEDS] - leds0 >= 7u);
        printf("  idle 7 s: %u PANEL_R, %u LEDS (keepalive), host keepalives %u\n", D.tx[RFM_PANEL_R] - panel_r0,
               D.rx[RFM_LEDS] - leds0, st.keepalives - ka0);
        panel_equal("idle");
    }

    /* ---- a FRAME_ACK lost (mid-stream, then the last one of a burst) */
    {
        int ok = 1;
        D.drop_frame_acks = 1;
        for (t = 0; t < 4 && ok; t++) {
            fr_ui(fbuf, 300u + t);
            ok = show("lost FRAME_ACK", fbuf, NULL);
        }
        CHECK(D.dropped_frame_acks == 1);
        fr_gradient(fbuf, 77);
        rfm_session_set_frame(S, fbuf);
        run_ms(5);
        D.drop_frame_acks = 1;
        show("lost last FRAME_ACK", fbuf, NULL);
        CHECK(D.dropped_frame_acks == 2);
        rfm_session_stats(S, &st);
        printf("  lost FRAME_ACKs: recovered (host: %u ack timeouts)\n", st.ack_timeouts);
        check_clean("lost FRAME_ACK");
    }

    /* ---- the device loses its session mid-animation (a USB reset), and comes back to a new one */
    {
        uint32_t s0 = rfa.sessions;
        for (t = 0; t < 20; t++) {
            fr_ui(fbuf, 400u + t);
            rfm_session_set_frame(S, fbuf);
            if (t == 7)
                rfm_app_usb_lost();
            run_ms(50);
        }
        CHECKF(settle(6000) >= 0, "%s session loss: did not settle", c->name);
        CHECKF(rfa.sessions == s0 + 1u, "session loss: %u sessions after", rfa.sessions - s0);
        screens_equal("session loss", fbuf);
        run_ms(1200);
        panel_equal("session loss");
        check_clean("session loss");
        printf("  device-side session loss mid-animation: a new session, screen restored\n");
    }

    /* ---- the device reboots mid-animation (shadow garbage, no session, bytes in flight lost) */
    {
        uint32_t hs0, sr0, ms_to, t_boot = 0, resync;
        rfm_session_stats(S, &st);
        hs0 = st.sessions;
        sr0 = st.shadow_resets;
        for (t = 0; t < 10; t++) {
            fr_ui(fbuf, 500u + t);
            rfm_session_set_frame(S, fbuf);
            if (t == 4) {
                dev_boot(0);
                t_boot = now_ms();
            }
            run_ms(50);
        }
        for (ms_to = 0; ms_to < 5000 && !(rfa.session && quiet()); ms_to++)
            step_ms();
        resync = now_ms() - t_boot;
        rfm_session_stats(S, &st);
        CHECKF(rfa.session && st.sessions == hs0 + 1u, "reboot: device session %u, host sessions +%u", rfa.session,
               st.sessions - hs0);
        CHECKF(st.shadow_resets == sr0 + 1u, "reboot: the host saw %u shadow resets", st.shadow_resets - sr0);
        CHECKF(resync < 1000u, "reboot: %u ms to a new session and the screen", resync);
        CHECK(settle(3000) >= 0);
        screens_equal("reboot", fbuf);
        run_ms(1200);
        panel_equal("reboot");
        check_clean("reboot");
        printf("  device reboot mid-animation: FRAME_ACK says shadow reset, a new session, screen restored %u ms after "
               "the reboot\n", resync);
    }

    /* ---- the SESSION ACK lost: the host retries, the device starts over, all agree */
    {
        rfm_session_stats(S, &st);
        D.drop_acks = 1;
        rfm_session_restart(S, now_ms());
        for (i = 0; i < 3000 && rfm_session_state(S) != RFM_S_ACTIVE; i++)
            step_ms();
        CHECK(rfm_session_state(S) == RFM_S_ACTIVE && D.dropped_acks == 1);
        fr_ui(fbuf, 600);
        show("lost SESSION ACK", fbuf, NULL);
        run_ms(1200);
        panel_equal("lost SESSION ACK");
        check_clean("lost SESSION ACK");
        printf("  lost SESSION ACK: retried, active after %u ms\n", i);
    }

    /* ---- the host freezes past the keepalive: the device ends the session; the host comes back to a new one */
    {
        uint32_t s0 = rfa.sessions;
        host_frozen = 1;
        run_ms(3500);
        CHECK(!rfa.session && D.session_on == 0);
        host_frozen = 0;
        fr_gradient(fbuf, 5);
        rfm_session_set_frame(S, fbuf);
        for (i = 0; i < 5000 && !(rfa.session && rfa.sessions == s0 + 1u && quiet()); i++)
            step_ms();
        CHECKF(rfa.session && rfa.sessions == s0 + 1u, "keepalive: device session %u, sessions +%u", rfa.session,
               rfa.sessions - s0);
        screens_equal("keepalive expiry", fbuf);
        check_clean("keepalive expiry");
        printf("  host frozen 3.5 s: the device ended the session; a new one %u ms after the host woke\n", i);
    }

    /* ---- stop */
    rfm_session_stop(S, now_ms());
    for (i = 0; i < 1000 && rfm_session_state(S) != RFM_S_STOPPED; i++)
        step_ms();
    CHECK(rfm_session_state(S) == RFM_S_STOPPED && !rfa.session && D.session_on == 0);
    run_ms(100);
    check_clean("stop");
    rfm_session_stats(S, &st);
    printf("  totals: host %u frames, %u TILES msgs, %u body B, %u wire B; device %u FRAME_ACKs (%u shadow reset), "
           "%u PANEL_R, %u TILES arrived during a push, %u LCD transfers; panel events %u; most frames queued on the "
           "device %u\n",
           st.frames, st.msgs, st.body_bytes, st.wire_bytes, D.frame_acks, D.ack_reset, D.tx[RFM_PANEL_R],
           D.tiles_in_push, L.xfers, st.events, D.max_qn);
    rfm_session_free(S);
    S = NULL;
}

/* ---------------------------------------------------------------- Bluetooth ---- */
/* a moving source (a knob turned: big blocks and a number change every 50 ms), each change several frames' worth */
static void knob(uint16_t *f, uint32_t t)
{
    char b[16];
    uint32_t k;
    fr_ui(f, 0);
    for (k = 0; k < 10u; k++)
        fr_rect(f, 8 + (int)((t * 37u + k * 53u) % 170u), 56 + (int)((t * 19u + k * 29u) % 120u), 56, 40,
                fr_rgb(t * 40u + k * 30u, 200u - k * 20u, 100u + t * 10u));
    snprintf(b, sizeof b, "%3u", t % 1000u);
    fr_text(f, 20, 30, b, fr_rgb(240, 240, 240), fr_rgb(36, 40, 52));
}

/* the knob for n changes at 20 a second, then still; returns the pushes that showed a screen never submitted
 * (sources) or never committed (committed) */
static uint32_t turn_knob(const char *where, uint32_t t0, uint32_t n, int against)
{
    uint32_t t, bad0 = incoherent, p0 = pushes_seen, c0 = h_commits, hd0 = h_held;
    int ms;
    coherence = against;
    for (t = 0; t < n; t++) {
        knob(fbuf, t0 + t);
        hset_add(&sources, fnv(fbuf, sizeof fbuf));
        rfm_session_set_frame(S, fbuf);
        run_ms(50);
    }
    ms = settle(30000);
    CHECKF(ms >= 0, "%s: did not settle", where);
    coherence = 0;
    printf("  %s: %u changes in %u ms, then still %d ms later: %u pushes, %u commits, %u held frames; %u pushes "
           "showed a screen never %s\n", where, n, n * 50u, ms, pushes_seen - p0, h_commits - c0, h_held - hd0,
           incoherent - bad0, against == 1 ? "committed" : "submitted");
    return incoherent - bad0;
}

static void run_ble(int hold, int nppm, char **ppm)
{
    rfm_session_cfg_t sc;
    rfm_session_cb_t cb;
    rfm_session_stats_t st;
    uint32_t t, i, seed = 1, bad;
    char where[96];

    printf("\n== Bluetooth MIDI, %s\n", hold ? "HOLD (whole screen changes)" : "HOLD off (ble_hold 0)");
    ble_mode = 1;
    now_us = 5000000;
    dev_boot(1);
    pipe_init(&d2h[RF_T_SX], RATE_BLE);
    pipe_init(&d2h[RF_T_BK], RATE_D2H_BK);
    memset(state_changes, 0, sizeof state_changes);
    memset(mon_cmd, 0, sizeof mon_cmd);
    memset(&committed, 0, sizeof committed);
    memset(&sources, 0, sizeof sources);
    mon_midi = decoy_bytes = 0;
    errs_seen = 0;
    host_frozen = 0;
    last_master_change = 0;
    pushes_seen = incoherent = h_held = h_commits = 0;
    last_xfers = 0;
    memset(&leds, 0, sizeof leds);
    run_ms(20);
    rfm_session_cfg_default(&sc);                       /* as the web player: auto, 20 fps, the link's budget */
    sc.ble_hold = hold;
    memset(&cb, 0, sizeof cb);
    cb.state = cb_state;
    cb.log = cb_log;
    S = rfm_session_new(&sc, &cb);
    for (t = 0; t < 2; t++) {
        rfm_rx_free(mon[t]);
        mon[t] = rfm_rx_new(t ? RFM_LINK_BK : RFM_LINK_SX, t ? RFM_BK_BODY_MAX + 64u : RFM_SX_BODY_MAX + 64u, mon_msg,
                            t ? NULL : mon_midi_fn, NULL);
    }
    rfm_rx_free(hmon);
    hmon = rfm_rx_new(RFM_LINK_SX, RFM_SX_BODY_MAX + 64u, hmon_msg, NULL, NULL);
    sx_id = rfm_session_add_link(S, RFM_LINK_SX, w_dev, (void *)(uintptr_t)RF_T_SX, "FM-1 7938");
    bk_id = -1;
    for (i = 0; i < 4000 && rfm_session_state(S) != RFM_S_ACTIVE; i++)
        step_ms();
    CHECKF(rfm_session_state(S) == RFM_S_ACTIVE, "bluetooth: no session after %u ms", i);
    rfm_session_stats(S, &st);
    CHECKF(st.ble == 1 && st.hold == (uint32_t)hold && st.lossy == RFM_BLE_COLORS, "ble %u hold %u lossy %u", st.ble,
           st.hold, st.lossy);
    CHECK(rfm_session_hello(S)->caps & RFM_CAP_HOLD);
    CHECK(settle(5000) >= 0);
    fr_fill(fbuf, 0);
    CHECK(screens_equal("bluetooth session start", fbuf));

    /* still screens, lossy: model == shadow == panel after each; every push shows a committed screen */
    coherence = 1;
    for (t = 0; t <= 6; t++) {
        gen(t % 2u ? SET_TEXT : SET_UI, t, fbuf, &seed);
        if (t % 3u == 1u)
            new_leds();
        snprintf(where, sizeof where, "bluetooth screen %u", t);
        rfm_session_set_frame(S, fbuf);
        CHECKF(settle(30000) >= 0, "%s: did not settle", where);
        screens_equal(where, NULL);
        leds_equal(where);
    }
    for (i = 0; i < (uint32_t)nppm && i < 3u; i++)
        if (!fr_load_ppm(ppm[i], fbuf)) {
            rfm_session_set_frame(S, fbuf);
            CHECKF(settle(60000) >= 0, "bluetooth %s: did not settle", ppm[i]);
            screens_equal(ppm[i], NULL);
        }
    run_ms(1500);                                       /* the refinement: exact */
    CHECK(settle(30000) >= 0);
    screens_equal("bluetooth refined", fbuf);
    coherence = 0;
    CHECKF(!incoherent, "bluetooth: %u pushes showed a screen never committed", incoherent);
    check_clean("bluetooth screens");

    /* the knob, lossy: whole committed screens only (with HOLD) */
    bad = turn_knob("knob, lossy", 0, 60, 1);
    if (hold)
        CHECKF(!bad, "bluetooth knob: %u pushes showed a screen never committed", bad);
    /* the knob, lossless: whole submitted screens only (with HOLD); without HOLD, screens that never were */
    rfm_session_set_ble_colors(S, 0);
    CHECK(settle(30000) >= 0);
    bad = turn_knob("knob, lossless", 100, 60, 2);
    if (hold)
        CHECKF(!bad, "bluetooth knob: %u pushes showed a screen never submitted", bad);
    else
        CHECKF(bad > 0, "bluetooth knob without HOLD: every push showed a whole screen (the test cannot tell)");
    screens_equal("knob", fbuf);
    rfm_session_set_ble_colors(S, RFM_BLE_COLORS);
    CHECKF(rfa.hold_timeouts == 0, "bluetooth: %u held screens shown by the timeout", rfa.hold_timeouts);
    if (hold)
        CHECKF(D.tiles_in_push == 0, "bluetooth: %u TILES decoded during a push", D.tiles_in_push);
    rfm_session_stats(S, &st);
    CHECKF(hold ? st.held > 0 && rfa.held_frames == st.held : st.held == 0 && rfa.held_frames == 0,
           "held frames: host %u, device %u", st.held, rfa.held_frames);
    check_clean("bluetooth knob");

    /* a FRAME_ACK lost in the middle of a change: the host resends, all agree again */
    D.drop_frame_acks = 1;
    knob(fbuf, 500);
    rfm_session_set_frame(S, fbuf);
    CHECKF(settle(30000) >= 0, "bluetooth lost FRAME_ACK: did not settle");
    screens_equal("bluetooth lost FRAME_ACK", NULL);
    CHECK(D.dropped_frame_acks == 1);
    rfm_session_stats(S, &st);
    printf("  lost FRAME_ACK: recovered (%u ack timeouts; held screens shown by the device's timeout: %u)\n",
           st.ack_timeouts, rfa.hold_timeouts);
    check_clean("bluetooth lost FRAME_ACK");

    /* the device loses the session in the middle of a held change: a new one, the screen again */
    {
        uint32_t s0 = rfa.sessions;
        for (t = 0; t < 30; t++) {
            knob(fbuf, 600u + t);
            rfm_session_set_frame(S, fbuf);
            if (t == 7)
                rfm_app_usb_lost();
            run_ms(50);
        }
        CHECKF(settle(30000) >= 0, "bluetooth session loss: did not settle");
        CHECKF(rfa.sessions == s0 + 1u, "bluetooth session loss: %u sessions after", rfa.sessions - s0);
        screens_equal("bluetooth session loss", NULL);
        check_clean("bluetooth session loss");
    }

    rfm_session_stop(S, now_ms());
    for (i = 0; i < 3000 && rfm_session_state(S) != RFM_S_STOPPED; i++)
        step_ms();
    CHECK(rfm_session_state(S) == RFM_S_STOPPED && !rfa.session && !rfa.held && !rfa.hold_used);
    rfm_session_stats(S, &st);
    printf("  totals: host %u frames (%u held), %u body B; device %u FRAME_ACKs, %u pushes checked, %u TILES "
           "arrived during a push\n", st.frames, st.held, st.body_bytes, D.frame_acks, pushes_seen, D.tiles_in_push);
    rfm_session_free(S);
    S = NULL;
    rfm_rx_free(hmon);
    hmon = NULL;
    ble_mode = 0;
}

int main(int argc, char **argv)
{
    static const cfg_t cfgs[] = {
        {"bulk", RFM_T_BULK, 0, 0},
        {"bulk @ SysEx budget", RFM_T_BULK, 0, RFM_BUDGET_SX_BPS / 20u},
        {"sysex 1024", RFM_T_MIDI, 0, 0},
        {"sysex 1024 @ bulk budget", RFM_T_MIDI, 0, RFM_BUDGET_BK_BPS / 20u},
        {"sysex 512", RFM_T_MIDI, 512, 0},
        {"sysex 512 @ bulk budget", RFM_T_MIDI, 512, RFM_BUDGET_BK_BPS / 20u},
    };
    uint32_t i;
    verbose = getenv("RFM_VERBOSE") != NULL;
    for (i = 0; i < sizeof cfgs / sizeof cfgs[0]; i++)
        run_cfg(&cfgs[i], argc - 1, argv + 1);
    run_ble(1, argc - 1, argv + 1);
    run_ble(0, argc - 1, argv + 1);
    for (i = 0; i < 2; i++) {
        rfm_rx_free(mon[i]);
        free(h2d[i].b);
        free(d2h[i].b);
    }
    return check_done("test_cross");
}
