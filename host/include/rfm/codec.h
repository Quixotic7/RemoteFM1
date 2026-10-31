/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: the protocol v2 codec (docs/PROTOCOL.md, proto/rfm1.h). Portable C99, no OS calls.
 *
 * Framing out: rfm_sx_frame (F0 7D 52 46 cmd <7-in-8 body> F7) and rfm_bk_frame (the 10-byte bulk header + body),
 * both built on firmware/src/remote_proto.h's helpers. Framing in: rfm_rx, a stream parser per link. A SysEx link's
 * stream is a whole MIDI byte stream: RemoteFM1 SysEx messages go to the message callback, channel messages (the
 * panel events) to the MIDI callback, realtime bytes and other SysEx are dropped. A bulk link's stream is the bulk
 * framing (resynchronised on a bad magic). Messages: one build / parse pair for every v2 message body. */
#ifndef RFM_CODEC_H
#define RFM_CODEC_H
#include <stddef.h>
#include <stdint.h>
#include "rfm1.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RFM_LINK_SX = 0, RFM_LINK_BK = 1 };

#define RFM_BK_HDR_LEN 10u
#define RFM_SX_OVERHEAD 6u                              /* F0 7D 52 46 cmd .. F7 */
uint32_t rfm_sx_size(uint32_t body_len);                /* the framed SysEx length */
uint32_t rfm_sx_frame(uint8_t cmd, const uint8_t *body, uint32_t n, uint8_t *out);
uint32_t rfm_bk_frame(uint8_t cmd, const uint8_t *body, uint32_t n, uint32_t seq, uint8_t *out);

/* ---- message bodies */
typedef struct {
    char name[17];                                      /* NUL-terminated copies of the fixed fields */
    uint8_t proto;
    char ver[9];
    uint8_t transports, usb_mode, lcd_baud;
    int8_t loader_state;
    uint16_t sxq;
    uint32_t bk_rx_ring, bk_tx_ring;
    uint16_t sx_body_max, bk_body_max;
    char identity[9];
    /* v2 (len >= RFM_HELLO_LEN) */
    int v2;
    uint16_t screen_w, screen_h, caps;
    uint8_t max_in_flight, session;
    char fw_ver[9];
} rfm_hello_t;

uint32_t rfm_build_hello(uint8_t *body, const char *host_name);           /* <= 16 bytes of name */
int rfm_parse_hello(const uint8_t *b, uint32_t n, rfm_hello_t *h);        /* 0 ok (>= 51 bytes) */
uint32_t rfm_build_hello_r(uint8_t *body, const rfm_hello_t *h);          /* RFM_HELLO_LEN bytes (the fake) */
/* the unit we look for: name exactly "RemoteFM1", proto 2, the v2 fields present */
int rfm_hello_is_remotefm1(const rfm_hello_t *h);
/* this HELLO_R came over Bluetooth MIDI (0.2+): sxq is the free send packets on the transport asked, Bluetooth's
 * queue (128) there, USB's ring less 64 (960) on USB, 4096 - 512 in M0 / 0.1; the Bluetooth bit says a computer is
 * connected (it is set on USB replies too then, so sxq decides). An sxq of 0 (unknown) is never Bluetooth. */
#define RFM_HELLO_SXQ_BLE_MAX 256u
int rfm_hello_is_ble(const rfm_hello_t *h);

uint32_t rfm_build_session(uint8_t *body, int start, uint32_t keepalive_ms);
uint32_t rfm_build_touch(uint8_t *body, uint32_t flags);   /* TOUCH (0.3.1): RFM_TOUCH_F_*; RFM_TOUCH_LEN */
int rfm_parse_session(const uint8_t *b, uint32_t n, int *start, uint32_t *keepalive_ms);

typedef struct {
    uint8_t lit[RFM_NCOL], mid[RFM_NCOL], dim[RFM_NCOL], faint[RFM_NCOL];
    uint8_t flags;                                      /* RFM_LEDS_F_PLAY_GREEN */
} rfm_leds_t;
uint32_t rfm_build_leds(uint8_t *body, const rfm_leds_t *l);
int rfm_parse_leds(const uint8_t *b, uint32_t n, rfm_leds_t *l);
int rfm_leds_equal(const rfm_leds_t *a, const rfm_leds_t *b);

typedef struct {
    uint16_t frame;
    uint8_t status;                                     /* RFM_ACK_DECODE_ERR, RFM_ACK_SHADOW_RESET */
    uint32_t draw_us, bytes;
    uint8_t pending;
} rfm_frame_ack_t;
uint32_t rfm_build_frame_ack(uint8_t *body, const rfm_frame_ack_t *a);
int rfm_parse_frame_ack(const uint8_t *b, uint32_t n, rfm_frame_ack_t *a);

typedef struct {
    uint32_t keys;
    uint16_t buttons;
    int16_t enc[RFM_NENC];
    uint16_t master, battery_mv;
    uint8_t flags;                                      /* RFM_PANEL_F_SESSION */
} rfm_panel_r_t;
uint32_t rfm_build_panel_r(uint8_t *body, const rfm_panel_r_t *p);
int rfm_parse_panel_r(const uint8_t *b, uint32_t n, rfm_panel_r_t *p);

/* MIDI_IN (RFM_CAP_MIDI_IN, docs/PROTOCOL.md section 4): the TRS jack's MIDI, whole messages with their status */
uint32_t rfm_midi_msg_len(uint8_t status);              /* 1..3; 0 for a byte no MIDI_IN message starts with */
int rfm_parse_midi_in(const uint8_t *b, uint32_t n);    /* 0: 1..RFM_MIDI_IN_MAX bytes of whole messages; else -1 */
uint32_t rfm_build_midi_in(uint8_t *body, const uint8_t *msgs, uint32_t n);   /* (the fake): n bytes, as they are */

/* TILES header: frame16 flags8 nrects8 */
void rfm_tiles_header(uint8_t *body, uint16_t frame, uint8_t flags, uint8_t nrects);

/* ---- receive */
typedef void (*rfm_msg_fn)(void *ctx, uint8_t cmd, const uint8_t *body, uint32_t n);
typedef void (*rfm_midi_fn)(void *ctx, const uint8_t *msg, uint32_t n);  /* one channel message, status first */

typedef struct {
    uint32_t msgs, bad, sync, frame_gaps, midi;
} rfm_rx_stats_t;

typedef struct rfm_rx rfm_rx_t;
/* kind RFM_LINK_SX or RFM_LINK_BK; body_cap: the longest body kept (longer: counted as bad and dropped) */
rfm_rx_t *rfm_rx_new(int kind, uint32_t body_cap, rfm_msg_fn on_msg, rfm_midi_fn on_midi, void *ctx);
void rfm_rx_free(rfm_rx_t *r);
void rfm_rx_feed(rfm_rx_t *r, const uint8_t *p, uint32_t n);
void rfm_rx_reset(rfm_rx_t *r);                         /* forget a partial message */
void rfm_rx_stats(const rfm_rx_t *r, rfm_rx_stats_t *st);

#ifdef __cplusplus
}
#endif
#endif
