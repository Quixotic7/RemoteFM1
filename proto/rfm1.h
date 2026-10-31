/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 protocol version 2: the constants of docs/PROTOCOL.md. Shared by the firmware (firmware/src) and every
 * host (host/). The framing helpers (rf_pack7 / rf_unpack7 / rf_sx_encode / rf_bk_header, RF_SX_ID*, RF_BK_HDR)
 * are firmware/src/remote_proto.h's, unchanged since M0. Owned by the orchestrator: report problems, don't edit. */
#ifndef RFM1_H
#define RFM1_H
#include <stdint.h>

#define RFM_NAME "RemoteFM1"
#define RFM_PROTO 2u

#define RFM_W 240u
#define RFM_H 240u
#define RFM_NCOL 11u                    /* LED / matrix columns */
#define RFM_NKEYS 27u                   /* note keys: matrix ids 14..40, key 0 = F3 (MIDI 53) */
#define RFM_NBTN 14u                    /* buttons: matrix ids 0..13 */
#define RFM_NENC 7u                     /* matrix encoders */

#define RFM_SX_BODY_MAX 1024u           /* a SysEx body before 7-in-8 packing */
#define RFM_BK_BODY_MAX 16384u          /* a bulk body */
#define RFM_FRAMES_IN_FLIGHT 2u
#define RFM_KEEPALIVE_MS_DEFAULT 3000u

/* commands (host -> device) and replies (device -> host); M0's numbers are kept (docs/PROTOCOL.md section 5) */
enum {
    RFM_HELLO = 0x01, RFM_SINK = 0x02, RFM_SOURCE = 0x03, RFM_ECHO = 0x04, RFM_STATS = 0x05, RFM_STATS_RESET = 0x06,
    RFM_BLIT_BENCH = 0x07, RFM_TILE_M0 = 0x08, RFM_SA_BENCH = 0x09,
    RFM_SESSION = 0x10, RFM_LEDS = 0x11, RFM_TILES = 0x12, RFM_PANEL_REQ = 0x13,
    RFM_TOUCH = 0x14,                   /* 0.3.1: the keys' velocity / pressure in the panel events (RFM_CAP_TOUCH) */
    RFM_HELLO_R = 0x41, RFM_DATA = 0x43, RFM_ECHO_R = 0x44, RFM_STATS_R = 0x45, RFM_ACK = 0x46, RFM_BLIT_R = 0x47,
    RFM_SA_BENCH_R = 0x49,
    RFM_SOURCE_DONE = 0x50, RFM_FRAME_ACK = 0x52, RFM_PANEL_R = 0x53,
    RFM_MIDI_IN = 0x54                  /* 0.3: the TRS jack's MIDI in, forwarded in a session (RFM_CAP_MIDI_IN) */
};

/* HELLO_R: M0's 51 bytes (firmware/src/remote_proto.h), of which: */
#define RFM_HELLO_TRANSPORTS 25u        /* u8, RFM_TR_* */
#define RFM_HELLO_IDENTITY 43u          /* char[8]: the update identity's first 8 characters */
enum {
    RFM_TR_USB = 1u << 0,               /* USB configured */
    RFM_TR_BULK = 1u << 1,              /* the vendor bulk interface built in */
    RFM_TR_SX_HEARD = 1u << 2,          /* RemoteFM1 SysEx heard over USB */
    RFM_TR_BK_HEARD = 1u << 3,          /* bulk heard */
    RFM_TR_BLE = 1u << 4,               /* 0.2: a Bluetooth computer connected */
    RFM_TR_BLE_HEARD = 1u << 5          /* 0.2: RemoteFM1 SysEx heard over Bluetooth */
};
/* then: */
#define RFM_HELLO_SCREEN_W 51u          /* u16 */
#define RFM_HELLO_SCREEN_H 53u          /* u16 */
#define RFM_HELLO_CAPS 55u              /* u16, RFM_CAP_* */
#define RFM_HELLO_INFLIGHT 57u          /* u8 */
#define RFM_HELLO_SESSION 58u           /* u8 */
#define RFM_HELLO_FWVER 59u             /* char[8] */
#define RFM_HELLO_LEN 67u
enum {
    RFM_CAP_SHADOW = 1u << 0, RFM_CAP_COPY = 1u << 1, RFM_CAP_RLE = 1u << 2, RFM_CAP_PAL = 1u << 3,
    RFM_CAP_BATTERY = 1u << 4, RFM_CAP_PLAY_GREEN = 1u << 5, RFM_CAP_LED5 = 1u << 6,
    RFM_CAP_HOLD = 1u << 7,             /* 0.2.2: TILES' HOLD flag (a frame drawn into the shadow, shown later) */
    RFM_CAP_BLE_FAST = 1u << 8,         /* 0.2.2: Bluetooth at 2M, long link packets, MTU 247: ~35 KB/s, ~9 KB in flight clean */
    RFM_CAP_MIDI_IN = 1u << 9,          /* 0.3: the TRS jack's MIDI in comes to the host in a session (MIDI_IN) */
    RFM_CAP_TOUCH = 1u << 10            /* 0.3.1: TOUCH: the keys' real velocity and their pressure as poly aftertouch */
};

/* TOUCH body (host -> device, in a session; no reply): flags8. A session starts with 0 (velocity 100, no
 * aftertouch, as before); PANEL_R's flags carry them back (RFM_PANEL_F_VEL / _PRESS) */
#define RFM_TOUCH_LEN 1u
enum { RFM_TOUCH_F_VEL = 1u << 0, RFM_TOUCH_F_PRESS = 1u << 1 };
#define RFM_MIDI_KEY_AT_DEAD 2u         /* a key's pressure goes out on a change of >= 2 (and on reaching 0) */
#define RFM_MIDI_KEY_AT_MS 10u          /* ... at most every 10 ms a key */

/* SESSION body: start8 keepalive_ms16 */
#define RFM_SESSION_LEN 3u

/* LEDS body: lit[11] mid[11] dim[11] faint[11] flags8 */
#define RFM_LEDS_LIT 0u
#define RFM_LEDS_MID 11u
#define RFM_LEDS_DIM 22u
#define RFM_LEDS_FAINT 33u
#define RFM_LEDS_FLAGS 44u
#define RFM_LEDS_LEN 45u
#define RFM_LEDS_F_PLAY_GREEN 1u

/* TILES body: frame16 flags8 nrects8, then rects: x8 y8 w8 h8 enc8 payload */
#define RFM_TILES_HDR 4u
#define RFM_RECT_HDR 5u
#define RFM_TILES_F_END 1u
#define RFM_TILES_F_HOLD 2u             /* 0.2.2 (RFM_CAP_HOLD), with END: acknowledged, into the shadow, not shown
                                         * until an END without HOLD (or RFM_HOLD_MS without TILES) */
#define RFM_HOLD_MS 1500u               /* the device shows held tiles by itself after this long without TILES */
enum { RFM_ENC_RAW = 0x00, RFM_ENC_FILL = 0x01, RFM_ENC_PAL = 0x02, RFM_ENC_RLE = 0x03, RFM_ENC_COPY = 0x04 };
#define RFM_PAL_MIN 2u
#define RFM_PAL_MAX 16u
static inline uint32_t rfm_pal_bpp(uint32_t n) { return n <= 2u ? 1u : n <= 4u ? 2u : 4u; }
static inline uint32_t rfm_pal_row_bytes(uint32_t w, uint32_t n) { return (w * rfm_pal_bpp(n) + 7u) / 8u; }

/* FRAME_ACK body: frame16 status8 draw_us32 bytes32 pending8 */
#define RFM_ACK_LEN 12u
enum { RFM_ACK_DECODE_ERR = 1u << 0, RFM_ACK_SHADOW_RESET = 1u << 1 };

/* PANEL_R body: keys32 buttons16 enc[7] int16 master16 battery_mv16 flags8 */
#define RFM_PANEL_KEYS 0u
#define RFM_PANEL_BUTTONS 4u
#define RFM_PANEL_ENC 6u
#define RFM_PANEL_MASTER 20u
#define RFM_PANEL_BATTERY 22u
#define RFM_PANEL_FLAGS 24u
#define RFM_PANEL_LEN 25u
#define RFM_PANEL_F_SESSION 1u
#define RFM_PANEL_F_VEL 2u              /* 0.3.1: TOUCH's velocity on in this session */
#define RFM_PANEL_F_PRESS 4u            /* 0.3.1: TOUCH's pressure (poly aftertouch) on */

/* MIDI_IN body (device -> host, in a session): whole MIDI messages, each with its status byte (no running status, no
 * SysEx): channel messages (2 or 3 bytes) and real time (F8 FA FB FC: 1 byte), in the order they came; 1..64 bytes */
#define RFM_MIDI_IN_MAX 64u

/* panel events as plain MIDI (docs/PROTOCOL.md section 4) */
#define RFM_MIDI_KEY_CH 15u             /* channel 16, zero-based */
#define RFM_MIDI_KEY_NOTE0 53u          /* key k -> note 53 + k */
#define RFM_MIDI_KEY_VEL 100u          /* (TOUCH velocity off; on: the key's own, 1..127) */
#define RFM_MIDI_KEY_AT 0xA0u           /* TOUCH pressure on: AF 53+k p, 0..127 */
#define RFM_MIDI_BTN_CH 14u             /* channel 15: note = matrix button id */
#define RFM_MIDI_BTN_VEL 127u
#define RFM_MIDI_ENC_CC0 16u            /* encoder e -> CC 16 + e on channel 15, value 64 + detents */
#define RFM_MIDI_MASTER_MSB 7u          /* v14 = adc << 4: CC 7 = v14 >> 7, then CC 39 = v14 & 0x7F */
#define RFM_MIDI_MASTER_LSB 39u

#endif
