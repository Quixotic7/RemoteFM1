/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Bluetooth audio end to end (docs/PROTOCOL.md section 9): the HOST library's session (rfm_session_audio, its
 * resampler and IMA ADPCM encoder) against the DEVICE's own protocol layer and player (firmware/src/rfm_app.c with
 * rfm_audio.h), over a simulated Bluetooth link at the fast radio's measured rate (35 KB/s each way), on a simulated
 * clock. The host plays a 1 kHz sine at 48 kHz in 1024-sample chunks (an AudioWorklet's posts), its clock 100 ppm
 * fast; the device renders at 44.1 kHz in 64-frame blocks as the FM-1's audio interrupt does.
 *
 * Checks: the unit offers the mode, the session turns it on, no TILES go while it is on (a changing screen is
 * submitted all along); the stream's wire rate; after a settling second the buffer neither runs dry nor overflows,
 * nothing is lost, and the sound is the sine (SNR over a 4 s window, the clock difference followed); a 300 ms stall of
 * the host fades out, then recovers (an underrun, a catch-up skip, the sine back); turned off, the device's shadow is
 * black and the host sends the whole screen again, so the shadow ends equal to the host's screen. The ring is the
 * shadow's last rows, as on the FM-1.
 *   test_ble_audio            (RFM_VERBOSE=1: the session's log) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "rfm/codec.h"
#include "rfm/session.h"

/* ---------------------------------------------------------------- the device's own code ---- */
#include "remote_proto.h"
#define RFA_FW_VER "0.3.6-x"
#define RFA_IDENTITY "FM-1_520"
#include "rfm_draw.c"
#include "rfm_push.c"
#include "rfm_app.c"

#define STEP_US 100u
#define RATE_BLE 0.035                                  /* bytes / us: the fast radio, measured with a Mac */
#define HOST_RATE 48000u
#define HOST_PPM 100.0                                  /* the host's audio clock, fast */
#define CHUNK 1024u
#define TONE_HZ 1000.0
#define TONE_AMP 0.5f

static uint64_t now_us;
static uint32_t now_ms(void) { return (uint32_t)(now_us / 1000u); }
static int verbose;

/* ---------------------------------------------------------------- pipes ---- */
typedef struct {
    uint8_t *b;
    uint32_t r, w, cap;
    double credit;
} pipe_t;
static pipe_t h2d, d2h;
static void pipe_put(pipe_t *p, const uint8_t *d, uint32_t n)
{
    if (p->r == p->w)
        p->r = p->w = 0;
    if (p->w + n > p->cap) {
        memmove(p->b, p->b + p->r, p->w - p->r);
        p->w -= p->r;
        p->r = 0;
        if (p->w + n > p->cap) {
            p->cap = (p->w + n) * 2u + 4096u;
            p->b = (uint8_t *)realloc(p->b, p->cap);
        }
    }
    memcpy(p->b + p->w, d, n);
    p->w += n;
}
static uint32_t pipe_take(pipe_t *p, uint32_t max, const uint8_t **d)
{
    uint32_t n = p->w - p->r;
    p->credit += RATE_BLE * STEP_US;
    if (p->credit > 4096.0)
        p->credit = 4096.0;
    if (n > (uint32_t)p->credit)
        n = (uint32_t)p->credit;
    if (n > max)
        n = max;
    *d = p->b + p->r;
    p->r += n;
    p->credit -= n;
    return n;
}

/* ---------------------------------------------------------------- the device stand-in ---- */
static uint16_t shadow[RFM_W * RFM_H], stage[RFM_W * 16u];
static rf_tstats_t dst;
static rf_sx_t dsx;
static uint8_t dsx_buf[RF_SX_BODY_MAX];
static rfm_au_t au;
static uint32_t rx_cmd[256], tx_cmd[256];
static uint64_t frames_out;                             /* 44.1 kHz frames rendered */

static int rfa_tx(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t w[RF_SX_BODY_MAX * 2u];
    (void)t;
    tx_cmd[cmd & 0xFFu]++;
    pipe_put(&d2h, w, rf_sx_encode(cmd, body, n, w));
    return 1;
}
static void rfa_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint8_t m[3] = {(uint8_t)st, (uint8_t)d1, (uint8_t)d2};
    pipe_put(&d2h, m, 3);
}
static void rfa_leds(const uint8_t *lit, const uint8_t *glow, const uint8_t *bg) { (void)lit, (void)glow, (void)bg; }
static void rfa_hello_dev(uint8_t *h, uint32_t t)      /* as rfm_main.c fills it, over Bluetooth */
{
    (void)t;
    h[25] = (uint8_t)(1u | RFM_TR_BLE | RFM_TR_BLE_HEARD);
    h[26] = 1;
    rf_wr16(h + 29, 128u);
    rf_wr16(h + 39, RFM_SX_BODY_MAX);
    rf_wr16(h + 41, RFM_BK_BODY_MAX);
}
static void rfa_session_changed(uint32_t on) { (void)on; }
static void rfa_touch_changed(uint32_t flags) { (void)flags; }
static uint32_t rfa_us(void) { return (uint32_t)now_us; }
static void rfa_diag(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len) { (void)t, (void)cmd, (void)body, (void)len; }
static int lcd_idle(void *c) { (void)c; return 1; }
static void lcd_window(void *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) { (void)c, (void)x0, (void)y0, (void)x1, (void)y1; }
static void lcd_data(void *c, const void *p, uint32_t n) { (void)c, (void)p, (void)n; }
static const rfm_lcd_ops_t LCD = {lcd_idle, lcd_window, lcd_data, 0};

static void dev_on_msg(void *ctx, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    rx_cmd[cmd & 0xFFu]++;
    rfm_app_msg((uint32_t)(uintptr_t)ctx, cmd, body, len);
}

static void dev_init(void)
{
    au.slot = (rfm_au_slot_t *)(void *)(shadow + RFM_W * RFM_H) - RFM_AU_SLOTS;   /* (as rfm_main.c: the shadow's
                                                                                  * last rows) */
    rfm_app_init(shadow, stage, RFM_W * 16u, &LCD);
    rfm_app_audio_init(&au);
    rf_body_init(&dsx.b, &dst, dsx_buf, sizeof dsx_buf, dev_on_msg, (void *)(uintptr_t)RF_T_SX);
}

/* the output, as the FM-1's render adds it: kept from `keep_from` frames on */
#define OUT_MAX (44100u * 16u)
static float *out;
static uint64_t keep_from;

static void dev_step(void)
{
    const uint8_t *p;
    uint32_t n, i;
    static uint32_t last_ms = 0xFFFFFFFFu;
    const uint32_t panel0[RFM_NENC] = {0};
    dsx.b.now_ms = now_ms();
    n = pipe_take(&h2d, 256u, &p);
    for (i = 0; i < n; i++)
        rf_sx_byte(&dsx, p[i]);
    if (n)
        rfm_app_activity();
    if (now_ms() != last_ms) {
        last_ms = now_ms();
        rfm_app_panel(0, 0, (const int32_t *)panel0, 512, 0, 0);
    }
    rfm_app_poll(now_ms());
    while (frames_out + 64u <= now_us * 44100u / 1000000u) {   /* the audio interrupt, 64 frames a block */
        int32_t lr[128] = {0};
        rfm_au_mix(&au, lr, 64, 32767.0f);
        for (i = 0; i < 64u; i++) {
            const uint64_t k = frames_out + i;
            if (k >= keep_from && k - keep_from < OUT_MAX)
                out[k - keep_from] = (float)lr[2u * i] / 32767.0f;
        }
        frames_out += 64u;
    }
}

/* ---------------------------------------------------------------- the host ---- */
static rfm_session_t *S;
static int link_id;
static uint16_t screen[RFM_W * RFM_H];
static uint64_t host_samples;                           /* samples the host has played */
static double tone_ph;
static uint32_t tiles_while_on;

static int host_write(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    pipe_put(&h2d, p, n);
    return 0;
}
static void host_log(void *ctx, const char *m)
{
    (void)ctx;
    if (verbose)
        printf("[%6u ms] %s\n", now_ms(), m);
}

static void paint(uint32_t seed)                        /* a screen that changes now and then */
{
    uint32_t i;
    for (i = 0; i < RFM_W * RFM_H; i++)
        screen[i] = (uint16_t)(((i / RFM_W) / 24u + (i % RFM_W) / 24u + seed) & 1u ? 0xF800u + seed * 0x41u : 0x001Fu);
    rfm_session_set_frame(S, screen);
}

/* the host's audio: chunks as its clock (HOST_PPM fast) makes them; `stall_until` ms: none, then all at once */
static void host_audio(uint64_t stall_from, uint64_t stall_until)
{
    const double t_host = (double)now_us * 1e-6 * (1.0 + HOST_PPM * 1e-6);
    float buf[CHUNK];
    uint32_t i;
    if (now_us >= stall_from * 1000u && now_us < stall_until * 1000u)
        return;
    while ((double)(host_samples + CHUNK) <= t_host * HOST_RATE) {
        for (i = 0; i < CHUNK; i++) {
            buf[i] = TONE_AMP * (float)sin(tone_ph);
            tone_ph += 2.0 * 3.14159265358979323846 * TONE_HZ / HOST_RATE;
        }
        rfm_session_audio(S, buf, CHUNK, HOST_RATE);
        host_samples += CHUNK;
    }
}

static void run_until(uint32_t ms, uint64_t stall_from, uint64_t stall_until)
{
    while (now_us < (uint64_t)ms * 1000u) {
        const uint8_t *p;
        uint32_t n;
        const uint32_t tiles0 = rx_cmd[RFM_TILES];
        now_us += STEP_US;
        dev_step();
        n = pipe_take(&d2h, 4096u, &p);
        if (n)
            rfm_session_rx(S, link_id, p, n, now_ms());
        if (now_us % 1000u == 0u) {
            host_audio(stall_from, stall_until);
            rfm_session_poll(S, now_ms());
        }
        if (au.on && rx_cmd[RFM_TILES] != tiles0)
            tiles_while_on += rx_cmd[RFM_TILES] - tiles0;
    }
}

/* the sine's SNR in out[0..n): its frequency from the rising zero crossings (the clocks differ, and the unit trims
 * its rate by up to 0.3 %), then the best fit near it */
static double snr_db(const float *y, uint32_t n, double *f_best)
{
    double best = -1e9, t0 = -1, t1 = 0, f0;
    uint32_t i, nc = 0;
    int k;
    for (i = 1; i < n; i++)
        if (y[i - 1] < 0.0f && y[i] >= 0.0f) {
            const double t = i - 1 + (double)y[i - 1] / ((double)y[i - 1] - (double)y[i]);
            if (t0 < 0)
                t0 = t;
            t1 = t;
            nc++;
        }
    f0 = nc > 1 ? (nc - 1) * 44100.0 / (t1 - t0) : TONE_HZ;
    *f_best = f0;
    for (k = -10; k <= 10; k++) {
        const double f = f0 * (1.0 + 5e-6 * k), w = 2.0 * 3.14159265358979323846 * f / 44100.0;
        double s = 0, c = 0, m = 0, sig = 0, res = 0;
        uint32_t i;
        for (i = 0; i < n; i++)
            m += y[i];
        m /= n;
        for (i = 0; i < n; i++) {
            s += (y[i] - m) * sin(w * i);
            c += (y[i] - m) * cos(w * i);
        }
        s *= 2.0 / n;
        c *= 2.0 / n;
        for (i = 0; i < n; i++) {
            const double fit = s * sin(w * i) + c * cos(w * i), e = y[i] - m - fit;
            sig += fit * fit;
            res += e * e;
        }
        if (res > 0 && 10.0 * log10(sig / res) > best) {
            best = 10.0 * log10(sig / res);
            *f_best = f;
        }
    }
    return best;
}

static double rms(const float *y, uint32_t n)
{
    double a = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        a += (double)y[i] * y[i];
    return sqrt(a / n);
}

int main(void)
{
    rfm_session_cfg_t cfg;
    rfm_session_cb_t cb;
    rfm_session_stats_t st;
    uint32_t wire0, blocks0, under0, lost0;
    double f, snr, kbps;
    verbose = getenv("RFM_VERBOSE") != NULL;
    out = (float *)calloc(OUT_MAX, sizeof *out);
    dev_init();

    rfm_session_cfg_default(&cfg);
    cfg.transport = RFM_T_MIDI;
    cfg.ble_audio = 1;
    memset(&cb, 0, sizeof cb);
    cb.log = host_log;
    S = rfm_session_new(&cfg, &cb);
    link_id = rfm_session_add_link(S, RFM_LINK_SX, host_write, NULL, "FM-1 7938");
    paint(0);

    /* the mode comes on; nothing of the screen goes while it is */
    run_until(1000, 0, 0);
    CHECK(rfm_session_state(S) == RFM_S_ACTIVE);
    CHECK(rfm_session_hello(S)->caps & RFM_CAP_BLE_AUDIO);
    CHECK(rfm_session_audio_on(S));
    CHECK(au.on && rfm_app_audio_on());
    CHECK(rx_cmd[RFM_AUDIO_MODE] == 1);
    paint(1);                                           /* (a new screen: not sent while the mode is on) */

    /* settled: 2 s .. 6 s, the sine, no losses, the buffer near its target */
    run_until(2000, 0, 0);
    rfm_session_stats(S, &st);
    wire0 = st.audio_wire;
    blocks0 = au.blocks;
    under0 = au.underruns;
    lost0 = au.lost;
    keep_from = frames_out;
    run_until(6000, 0, 0);
    rfm_session_stats(S, &st);
    kbps = (st.audio_wire - wire0) / 4.0 / 1000.0;
    snr = snr_db(out, (uint32_t)(frames_out - keep_from), &f);
    printf("stream: %u Hz, %.2f KB/s on the wire (%.0f %% of %.0f KB/s), %u blocks in 4 s; unit buffer %u ms "
           "(target %u); SNR %.1f dB at %.2f Hz\n",
           st.audio_rate, kbps, kbps * 100.0 / (RATE_BLE * 1000.0), RATE_BLE * 1000.0, au.blocks - blocks0,
           rfm_au_fill_ms(&au), au.buf_ms, snr, f);
    CHECK(st.audio_rate == 22050u);
    CHECKF(kbps > 13.0 && kbps < 14.5, "%.2f KB/s", kbps);
    CHECKF(au.underruns == under0 && au.lost == lost0 && au.bad == 0, "underruns %u lost %u bad %u", au.underruns,
           au.lost, au.bad);
    CHECKF(rfm_au_fill_ms(&au) > au.buf_ms / 2u && rfm_au_fill_ms(&au) < 2u * au.buf_ms, "fill %u ms",
           rfm_au_fill_ms(&au));
    CHECKF(snr > 25.0, "SNR %.1f dB", snr);
    CHECKF(fabs(f / TONE_HZ - 1.0) < RFM_AU_TRIM, "%.4f Hz", f);   /* (the trim's range: at most 5 cents) */
    CHECK(tiles_while_on == 0 && st.audio && st.audio_unit_blocks > 300u && st.audio_buffer_ms == 120u);

    /* the host stalls 300 ms and catches up: a fade, an underrun, then the sine again (a block skipped to catch up) */
    run_until(7000, 0, 0);
    under0 = au.underruns;
    run_until(9500, 7000, 7300);
    CHECKF(au.underruns > under0, "underruns %u", au.underruns);
    CHECKF(au.skipped >= 1u, "skipped %u", au.skipped);
    keep_from = frames_out;
    run_until(10500, 0, 0);
    {
        const double r = rms(out, (uint32_t)(frames_out - keep_from));
        CHECKF(fabs(r - TONE_AMP / sqrt(2.0)) < 0.03, "rms %.3f", r);
        CHECKF(rfm_au_fill_ms(&au) < 2u * au.buf_ms, "fill %u ms after the burst", rfm_au_fill_ms(&au));
    }

    /* off: the device's screen black, the host sends all of it again */
    rfm_session_set_ble_audio(S, 0);
    run_until(14000, 0, 0);
    CHECK(!rfm_session_audio_on(S) && !au.on && !rfm_app_audio_on());
    CHECK(rfm_session_state(S) == RFM_S_ACTIVE);
    CHECK(rx_cmd[RFM_TILES] > 0);
    {
        const uint16_t *model = rfm_screen_model(rfm_session_screen(S));
        CHECK(!memcmp(model, screen, sizeof screen));     /* the host sent the whole screen again ... */
        CHECK(!memcmp(model, shadow, sizeof shadow));     /* ... and the device has it */
    }
    rfm_session_stats(S, &st);
    CHECK(st.decode_errs == 0 && st.audio == 0);
    rfm_session_free(S);
    free(out);
    free(h2d.b);
    free(d2h.b);
    return check_done("ble_audio");
}
