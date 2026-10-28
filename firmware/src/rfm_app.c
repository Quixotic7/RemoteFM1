/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1's protocol layer (docs/PROTOCOL.md, version 2), without the HAL: the session and its keepalive, HELLO_R,
 * LEDS, TILES into the shadow (rfm_draw.c), the push at END (rfm_push.c) and FRAME_ACK, PANEL_R, and the panel
 * events as plain MIDI. rfm_main.c includes it in the firmware (on fm1-base), tests/session_test.c and host/'s cross
 * test on the computer, each with its own versions of these (declared here, defined by the includer):
 *
 *   rfa_tx(t, cmd, body, n)       a message on transport t (RF_T_SX / RF_T_BK; the firmware's third, Bluetooth's
 *                                 SysEx); may wait for room, may drop
 *   rfa_midi(st, d1, d2)          a panel event as a plain MIDI message (queued by the includer: to the session's port)
 *   rfa_leds(lit, glow, bg)       11 bytes each, fm1_led's packed row bits: lit; mid and dim; faint
 *   rfa_hello_dev(h, t)           HELLO_R bytes 25..42 (M0's: transports, USB mode, LCD baud, loader, rings, maxima)
 *   rfa_session_changed(on)       the waiting screen goes (1) or comes back (0)
 *   rfa_us()                      microseconds, wrapping (FRAME_ACK draw_us)
 *   rfa_diag(t, cmd, body, len)   every other command: M0's diagnostics (section 5), or unknown
 *
 * Calls in: rfm_app_init, rfm_app_msg (a whole message), rfm_app_activity (bytes arrived: the keepalive),
 * rfm_app_panel (the panel once a millisecond), rfm_app_poll (often: the push, ACKs, timers), rfm_app_usb_lost,
 * rfm_app_lcd_lost (somebody else drew on the panel), rfm_app_tile_m0, rfm_app_pushing, rfm_app_rx_wait. The push
 * runs in a session (at every END without HOLD, and once at its start to blacken the panel) and, outside one, only
 * for M0's raw TILE.
 * HOLD (0.2.2, RFM_CAP_HOLD, docs/PROTOCOL.md section 3): a frame whose END carries HOLD is drawn into the shadow and
 * acknowledged, and its tiles stay in the push's dirty set, unshown; the next END without HOLD pushes them all with
 * its own (the host sends a screen change as held frames and a last one that shows it). So that a push never shows
 * tiles of the next held frames (the push reads the shadow while the DMA runs), once a session has used HOLD its
 * transport's input waits while a push runs or is due (rfm_app_rx_wait: the includer leaves the bytes in its rings).
 * Held tiles are shown anyway after RFM_HOLD_MS without TILES (a host gone quiet in the middle of a change).
 * Without a shadow (rfm_app_init's fb NULL: the includer sets rfa.draw.row and rowbuf, RFM_SHADOW 0) the rectangles
 * go to the panel as they are decoded (the includer draws them only in a session), each END is acknowledged at once,
 * and a panel drawn on by someone else is owed by the host: the next FRAME_ACK says "shadow reset".
 *
 * Choices where the spec leaves room (each is reported, and kept here):
 *  - TILES outside a session are decoded into the shadow and acknowledged at once, without a push: the waiting
 *    screen stays. LEDS outside a session are ignored (the waiting screen owns the LEDs).
 *  - A TILES body shorter than its 4-byte header has no frame number: it is counted, and the next FRAME_ACK
 *    carries the decode-error bit. Bytes after the last of nrects rectangles are an error too (all were drawn).
 *  - SESSION with fewer than 3 body bytes: keepalive 0 (never). An empty SESSION is ignored.
 *  - PANEL_R master16 is the ADC value 0..1023 (emu_hal_t.master), not v14.
 *  - An encoder turned more than 63 detents between two reports goes out as several messages of at most 63.
 *  - FRAME_ACK pending: the frames ENDed but not yet acknowledged after this one. One push may cover several
 *    ENDs (they arrived while a push ran): each gets its own FRAME_ACK, in order, when that push is over.
 *  - The first FRAME_ACK after a session start carries "shadow reset" (bit1), and so does the first after power-on,
 *    in a session or not (the spec: "a new session, or the device restarted"). */
#include "rfm_draw.h"
#include "rfm_push.h"

static int rfa_tx(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t n);
static void rfa_midi(uint32_t st, uint32_t d1, uint32_t d2);
static void rfa_leds(const uint8_t *lit, const uint8_t *glow, const uint8_t *bg);
static void rfa_hello_dev(uint8_t *h, uint32_t t);
static void rfa_session_changed(uint32_t on);
static uint32_t rfa_us(void);
static void rfa_diag(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len);

#ifndef RFA_FW_VER
#define RFA_FW_VER APP_VER
#endif
#ifndef RFA_IDENTITY
#define RFA_IDENTITY FELUCCA_ID
#endif
#define RFA_GREEN_COL 8u                    /* PLAY's green LED: column 8, row bit 1 (ChoralRoot ui_input.c) */
#define RFA_GREEN_BIT 0x02u
#ifndef RFA_CAPS
#define RFA_CAPS (RFM_CAP_SHADOW | RFM_CAP_COPY | RFM_CAP_RLE | RFM_CAP_PAL | RFM_CAP_PLAY_GREEN | RFM_CAP_HOLD)
#endif
#define RFA_QN 8u                           /* ENDs waiting for their FRAME_ACK */
#define RFA_PANEL_MS 500u
#define RFA_MASTER_MS 5u
#define RFA_MASTER_DEAD 2

typedef struct {
    uint16_t frame;
    uint8_t t, status, held;                /* held: an END with HOLD, acknowledged without a push */
    uint32_t bytes, us;
} rfa_end_t;

static struct {
    rfm_draw_t draw;
    rfm_push_t push;
    uint32_t now;
    uint8_t session, st;                    /* a session runs; its transport */
    uint16_t keepalive;
    uint32_t last_rx, panel_ms;
    uint8_t shadow_reset, flush;            /* the next ACK says so; push without an END */
    uint8_t f_err;                          /* this frame: a decode error so far */
    uint32_t f_bytes, f_us;                 /* this frame: body bytes, decode us */
    rfa_end_t q[RFA_QN];
    uint32_t qh, qn, covered;               /* ENDs queued, of which the running push covers `covered` */
    uint32_t push_t0, push_us, push_max_us;
    uint8_t hold_used, held;                /* this session sent HOLD; tiles drawn and not shown (HOLD) */
    uint32_t tiles_ms;                      /* the last TILES (RFM_HOLD_MS) */
    /* the panel as the events told it */
    uint32_t keys, buttons;
    int16_t enc[RFM_NENC];
    int32_t master, master_sent;            /* ADC 0..1023, -1 none yet */
    uint32_t master_ms;
    uint16_t batt_mv;
    /* counters */
    uint32_t frames, decode_errs, bad, sessions, lcd_lost, held_frames, hold_timeouts;
} rfa;

static void rfm_app_init(uint16_t *fb, uint16_t *stage, uint32_t stage_px, const rfm_lcd_ops_t *lcd)
{
    rfa.draw.fb = fb;                                   /* (NULL: no shadow; the includer gives row and rowbuf) */
    rfa.draw.dirty = fb ? rfm_push_dirty_cb : 0;
    rfa.draw.ctx = &rfa.push;
    rfm_push_init(&rfa.push, fb, stage, stage_px, lcd);
    rfa.master = rfa.master_sent = -1;
    rfa.shadow_reset = 1;                               /* the device restarted: a host's model is invalid */
}

static void rfa_wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void rfa_wr32(uint8_t *p, uint32_t v)
{
    rfa_wr16(p, v);
    rfa_wr16(p + 2, v >> 16);
}

/* ------------------------------------------------------------ replies --- */
static void rfa_hello(uint32_t t)
{
    static const char NAME[] = RFM_NAME, VER[] = RFA_FW_VER, ID[] = RFA_IDENTITY;
    uint8_t h[RFM_HELLO_LEN];
    uint32_t i;
    for (i = 0; i < sizeof h; i++)
        h[i] = 0;
    for (i = 0; i < 16u && i < sizeof NAME - 1u; i++)
        h[i] = (uint8_t)NAME[i];
    h[16] = RFM_PROTO;
    for (i = 0; i < 8u && i < sizeof VER - 1u; i++)
        h[17 + i] = (uint8_t)VER[i];
    rfa_hello_dev(h, t);                                /* 25..42 */
    for (i = 0; i < 8u && i < sizeof ID - 1u; i++)
        h[43 + i] = (uint8_t)ID[i];
    rfa_wr16(h + RFM_HELLO_SCREEN_W, RFM_W);
    rfa_wr16(h + RFM_HELLO_SCREEN_H, RFM_H);
    rfa_wr16(h + RFM_HELLO_CAPS, RFA_CAPS | (rfa.batt_mv ? RFM_CAP_BATTERY : 0u));
    h[RFM_HELLO_INFLIGHT] = RFM_FRAMES_IN_FLIGHT;
    h[RFM_HELLO_SESSION] = rfa.session;
    for (i = 0; i < 8u && i < sizeof VER - 1u; i++)
        h[RFM_HELLO_FWVER + i] = (uint8_t)VER[i];
    rfa_tx(t, RFM_HELLO_R, h, sizeof h);
}

static void rfa_panel_r(uint32_t t)
{
    uint8_t b[RFM_PANEL_LEN];
    uint32_t e;
    rfa_wr32(b + RFM_PANEL_KEYS, rfa.keys);
    rfa_wr16(b + RFM_PANEL_BUTTONS, rfa.buttons);
    for (e = 0; e < RFM_NENC; e++)
        rfa_wr16(b + RFM_PANEL_ENC + 2u * e, (uint16_t)rfa.enc[e]);
    rfa_wr16(b + RFM_PANEL_MASTER, rfa.master < 0 ? 0u : (uint32_t)rfa.master);
    rfa_wr16(b + RFM_PANEL_BATTERY, rfa.batt_mv);
    b[RFM_PANEL_FLAGS] = rfa.session ? RFM_PANEL_F_SESSION : 0u;
    rfa_tx(t, RFM_PANEL_R, b, sizeof b);
    rfa.panel_ms = rfa.now;
}

static void rfa_frame_ack(uint32_t t, uint32_t frame, uint32_t status, uint32_t us, uint32_t bytes, uint32_t pending)
{
    uint8_t b[RFM_ACK_LEN];
    if (rfa.shadow_reset) {
        status |= RFM_ACK_SHADOW_RESET;
        rfa.shadow_reset = 0;
    }
    rfa_wr16(b, frame);
    b[2] = (uint8_t)status;
    rfa_wr32(b + 3, us);
    rfa_wr32(b + 7, bytes);
    b[11] = (uint8_t)(pending > 255u ? 255u : pending);
    rfa_tx(t, RFM_FRAME_ACK, b, sizeof b);
    rfa.frames++;
}

static void rfa_master_send(void)
{
    uint32_t v14;
    if (rfa.master < 0)
        return;
    v14 = (uint32_t)rfa.master << 4;
    rfa_midi(0xB0u | RFM_MIDI_BTN_CH, RFM_MIDI_MASTER_MSB, v14 >> 7);
    rfa_midi(0xB0u | RFM_MIDI_BTN_CH, RFM_MIDI_MASTER_LSB, v14 & 0x7Fu);
    rfa.master_sent = rfa.master;
    rfa.master_ms = rfa.now;
}

/* ------------------------------------------------------------ session --- */
static void rfa_leds_off(void)
{
    static const uint8_t Z[RFM_NCOL];
    rfa_leds(Z, Z, Z);
}

/* stop the push; the dirty set: all of it (the panel must be repainted from the shadow) or none (the waiting
 * screen takes the panel: nothing of the shadow is owed to it) */
static void rfa_push_reset(int all)
{
    uint32_t b;
    rfm_push_abort(&rfa.push);
    rfa.covered = 0;
    if (!all)
        for (b = 0; b < RFM_TILES_Y; b++)
            rfa.push.dirty[b] = 0;
}

static void rfa_session_end(void)
{
    if (!rfa.session)
        return;
    rfa.session = 0;
    rfa.qn = 0;                                         /* nobody waits for those ACKs any more */
    rfa.flush = 0;
    rfa.hold_used = rfa.held = 0;                       /* (held tiles: never shown, the waiting screen comes) */
    rfa_push_reset(0);
    rfa_session_changed(0);
}

static void rfa_session_start(uint32_t t, uint32_t keepalive)
{
    rfa.session = 1;
    rfa.sessions++;
    rfa.st = (uint8_t)t;
    rfa.keepalive = (uint16_t)keepalive;
    rfa.last_rx = rfa.now;
    rfa.qn = 0;
    rfa.f_err = 0;
    rfa.f_bytes = rfa.f_us = 0;
    rfa.hold_used = rfa.held = 0;
    rfa_push_reset(0);
    rfm_draw_clear(&rfa.draw, 0x0000u);                 /* the shadow black, all of it dirty */
    rfa.flush = 1;                                      /* and the panel black, without an END */
    rfa.shadow_reset = 1;
    rfa_leds_off();
    rfa_session_changed(1);
}

static void rfa_cmd_session(uint32_t t, const uint8_t *b, uint32_t len)
{
    uint32_t ka = len >= 3u ? (uint32_t)b[1] | (uint32_t)b[2] << 8 : 0u;
    if (len < 1u) {
        rfa.bad++;
        return;
    }
    if (b[0]) {
        rfa_session_start(t, ka);
        rfa_tx(t, RFM_ACK, b, 0);
        rfa_panel_r(t);
        rfa_master_send();                              /* MASTER always when a session starts */
    } else {
        rfa_session_end();
        rfa_tx(t, RFM_ACK, b, 0);
    }
}

/* ------------------------------------------------------------ LEDs, screen --- */
static void rfa_cmd_leds(const uint8_t *b, uint32_t len)
{
    uint8_t lit[RFM_NCOL], glow[RFM_NCOL], bg[RFM_NCOL];
    uint32_t c;
    if (len < RFM_LEDS_LEN) {
        rfa.bad++;
        return;
    }
    if (!rfa.session)
        return;
    for (c = 0; c < RFM_NCOL; c++) {                    /* lit -> lit, mid and dim -> glow, faint -> backlight */
        lit[c] = b[RFM_LEDS_LIT + c];
        if (c == RFA_GREEN_COL && (b[RFM_LEDS_FLAGS] & RFM_LEDS_F_PLAY_GREEN))
            lit[c] |= RFA_GREEN_BIT;
        glow[c] = (uint8_t)((b[RFM_LEDS_MID + c] | b[RFM_LEDS_DIM + c]) & ~lit[c]);
        bg[c] = (uint8_t)(b[RFM_LEDS_FAINT + c] & ~lit[c] & ~glow[c]);
    }
    rfa_leds(lit, glow, bg);
}

/* an END: its FRAME_ACK at once (outside a session, without a shadow, or held with nothing queued before it), or
 * queued for the push (held: behind ENDs still queued, acknowledged in order without a push of its own) */
static void rfa_end_push(uint32_t t, uint32_t frame, uint32_t hold)
{
    uint32_t st = rfa.f_err ? RFM_ACK_DECODE_ERR : 0u;
    if (!rfa.session || !rfa.draw.fb || (hold && !rfa.qn)) {  /* no push outside a session (the waiting screen),
                                                         * nor without a shadow (drawn as it was decoded), nor for
                                                         * a held frame */
        rfa_frame_ack(t, frame, st, rfa.f_us, rfa.f_bytes, 0);
    } else {
        rfa_end_t *e;
        if (rfa.qn == RFA_QN) {                         /* (a host past its credit) the last one stands for it */
            e = &rfa.q[(rfa.qh + rfa.qn - 1u) % RFA_QN];
            e->status |= (uint8_t)st;
            e->bytes += rfa.f_bytes;
            e->us += rfa.f_us;
            e->frame = (uint16_t)frame;
            e->t = (uint8_t)t;
            e->held = (uint8_t)(e->held && hold);
        } else {
            e = &rfa.q[(rfa.qh + rfa.qn) % RFA_QN];
            e->frame = (uint16_t)frame;
            e->t = (uint8_t)t;
            e->status = (uint8_t)st;
            e->held = (uint8_t)hold;
            e->bytes = rfa.f_bytes;
            e->us = rfa.f_us;
            rfa.qn++;
        }
    }
    rfa.f_err = 0;
    rfa.f_bytes = rfa.f_us = 0;
}

static void rfa_cmd_tiles(uint32_t t, const uint8_t *b, uint32_t len)
{
    rfm_draw_res_t r;
    uint32_t t0 = rfa_us();
    rfm_draw_tiles(&rfa.draw, b, len, &r);
    rfa.f_us += rfa_us() - t0;
    rfa.f_bytes += len;
    if (r.err) {
        rfa.f_err = 1;
        rfa.decode_errs++;
    }
    rfa.tiles_ms = rfa.now;
    if (r.err != RFM_DRAW_E_HDR && (r.flags & RFM_TILES_F_END)) {
        uint32_t hold = (r.flags & RFM_TILES_F_HOLD) && rfa.session && rfa.draw.fb;
        if (hold) {
            rfa.hold_used = rfa.held = 1;
            rfa.held_frames++;
        }
        rfa_end_push(t, r.frame, hold);
    }
}

/* M0's raw TILE (x8 y8 w8 h8 px[w*h*2]): into the shadow and pushed at once, without a FRAME_ACK; 0 if bad */
static int rfm_app_tile_m0(const uint8_t *b, uint32_t len)
{
    uint32_t x, y, w, h;
    if (len < 4u)
        return 0;
    x = b[0];
    y = b[1];
    w = b[2];
    h = b[3];
    if (!w || !h || x + w > RFM_W || y + h > RFM_H || len != 4u + 2u * w * h)
        return 0;
    rfm_draw_raw(&rfa.draw, x, y, w, h, b + 4);
    rfm_push_mark(&rfa.push, x, y, w, h);
    rfa.flush = 1;
    return 1;
}

/* ------------------------------------------------------------ in --- */
static void rfm_app_activity(void) { rfa.last_rx = rfa.now; }

static void rfm_app_msg(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    rfa.last_rx = rfa.now;
    switch (cmd) {
    case RFM_HELLO:
        rfa_hello(t);
        break;
    case RFM_SESSION:
        rfa_cmd_session(t, body, len);
        break;
    case RFM_LEDS:
        rfa_cmd_leds(body, len);
        break;
    case RFM_TILES:
        rfa_cmd_tiles(t, body, len);
        break;
    case RFM_PANEL_REQ:
        rfa_panel_r(t);
        break;
    case RFM_ECHO:
        rfa_tx(t, RFM_ECHO_R, body, len);
        break;
    default:
        rfa_diag(t, cmd, body, len);
        break;
    }
}

/* the panel now (once a millisecond): keys / buttons debounced, encoder detents since the last call, the MASTER
 * ADC (-1: not read this time) */
static void rfm_app_panel(uint32_t keys, uint32_t buttons, const int32_t *steps, int32_t master)
{
    uint32_t ch, i, e;
    keys &= (1u << RFM_NKEYS) - 1u;
    buttons &= (1u << RFM_NBTN) - 1u;
    for (ch = keys ^ rfa.keys, i = 0; ch; ch >>= 1, i++)
        if (ch & 1u) {
            uint32_t on = (keys >> i) & 1u;
            rfa_midi((on ? 0x90u : 0x80u) | RFM_MIDI_KEY_CH, RFM_MIDI_KEY_NOTE0 + i, on ? RFM_MIDI_KEY_VEL : 0u);
        }
    rfa.keys = keys;
    for (ch = buttons ^ rfa.buttons, i = 0; ch; ch >>= 1, i++)
        if (ch & 1u) {
            uint32_t on = (buttons >> i) & 1u;
            rfa_midi((on ? 0x90u : 0x80u) | RFM_MIDI_BTN_CH, i, on ? RFM_MIDI_BTN_VEL : 0u);
        }
    rfa.buttons = buttons;
    for (e = 0; e < RFM_NENC; e++) {
        int32_t s = steps[e];
        rfa.enc[e] = (int16_t)(uint16_t)((uint32_t)(uint16_t)rfa.enc[e] + (uint32_t)s);
        while (s) {
            int32_t d = s > 63 ? 63 : s < -63 ? -63 : s;
            rfa_midi(0xB0u | RFM_MIDI_BTN_CH, RFM_MIDI_ENC_CC0 + e, (uint32_t)(64 + d));
            s -= d;
        }
    }
    if (master >= 0) {
        rfa.master = master;
        if (rfa.master_sent < 0 || ((master - rfa.master_sent >= RFA_MASTER_DEAD ||
                                     rfa.master_sent - master >= RFA_MASTER_DEAD) &&
                                    rfa.now - rfa.master_ms >= RFA_MASTER_MS))
            rfa_master_send();
    }
}

/* ------------------------------------------------------------ poll --- */
static void rfa_push_done(void)
{
    uint32_t k;
    rfa.push_us = rfa_us() - rfa.push_t0;
    if (rfa.push_us > rfa.push_max_us)
        rfa.push_max_us = rfa.push_us;
    for (k = 0; k < rfa.covered; k++) {
        rfa_end_t *e = &rfa.q[rfa.qh];
        rfa.qh = (rfa.qh + 1u) % RFA_QN;
        rfa.qn--;
        rfa_frame_ack(e->t, e->frame, e->status, e->us + rfa.push_us, e->bytes, rfa.qn);
    }
    rfa.covered = 0;
}

static void rfm_app_poll(uint32_t now_ms)
{
    rfa.now = now_ms;
    if (rfa.session && rfa.keepalive && now_ms - rfa.last_rx >= rfa.keepalive)
        rfa_session_end();
    if (rfa.push.active) {
        if (!rfm_push_step(&rfa.push))
            rfa_push_done();
    }
    while (!rfa.push.active && rfa.qn && rfa.q[rfa.qh].held) {   /* held ENDs first in line: no push for them */
        rfa_end_t *e = &rfa.q[rfa.qh];
        rfa.qh = (rfa.qh + 1u) % RFA_QN;
        rfa.qn--;
        rfa_frame_ack(e->t, e->frame, e->status, e->us, e->bytes, rfa.qn);
    }
    if (rfa.held && rfa.session && !rfa.push.active && !rfa.qn && now_ms - rfa.tiles_ms >= RFM_HOLD_MS) {
        rfa.flush = 1;                                  /* the host went quiet holding tiles: show them */
        rfa.hold_timeouts++;
    }
    if (!rfa.push.active && ((rfa.session && rfa.qn) || (rfa.flush && rfm_push_pending(&rfa.push)))) {
        rfa.flush = 0;
        rfa.held = 0;                                   /* the push takes every tile drawn so far */
        rfa.covered = rfa.qn;
        rfa.push_t0 = rfa_us();
        rfm_push_start(&rfa.push);
        if (!rfm_push_step(&rfa.push))
            rfa_push_done();
    }
    if (rfa.session && now_ms - rfa.panel_ms >= RFA_PANEL_MS)
        rfa_panel_r(rfa.st);
}

static void rfm_app_usb_lost(void) { rfa_session_end(); }

/* somebody else drew on the panel (the update screen, the blit bench): in a session, all of it again */
static void rfm_app_lcd_lost(void)
{
    rfa.lcd_lost++;
    if (!rfa.draw.fb) {                                 /* no shadow to repaint from: the host's model is gone */
        rfa.shadow_reset = 1;
        return;
    }
    rfa_push_reset(rfa.session);                        /* the ENDs stay queued: the repaint answers them */
    rfa.flush = rfa.session;
}

/* the push runs (the waiting screen keeps off the panel meanwhile) */
static int rfm_app_pushing(void) { return rfa.push.active; }

/* transport t's input waits (left in the includer's rings, nothing parsed): a session on t that uses HOLD has a push
 * running or due, which must not show what the next messages draw. Checked before every piece of input, so the
 * message after an END stays unread until its push is over (~22 ms for the whole screen). */
static int rfm_app_rx_wait(uint32_t t)
{
    return rfa.session && rfa.hold_used && rfa.st == t && (rfa.push.active || rfa.qn);
}
