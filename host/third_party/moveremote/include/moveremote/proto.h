/* SPDX-License-Identifier: MPL-2.0 */
/* Move Remote wire protocol, version 1. Shared by the device (device/) and every host (host/).
 * docs/PROTOCOL.md is the prose version of this file; keep the two in step.
 *
 * One byte stream, the same whichever way it travels:
 *   - plain TCP to port MR_PORT (USB-C NCM link 172.16.254.1, or Wi-Fi), or
 *   - a WebSocket at ws://<move>:MR_PORT/ws, whose binary frames carry the same bytes in any chunking.
 *
 * Every message: u16 len (little-endian, counts type + payload, >= 1) | u8 type | payload[len - 1].
 * All multi-byte fields are little-endian. Types 0x01..0x7F go device -> host, 0x81..0xFF host -> device. */
#ifndef MOVEREMOTE_PROTO_H
#define MOVEREMOTE_PROTO_H

#include <stdint.h>

#define MR_PORT 7720
#define MR_PROTO 1
#define MR_USB_ADDR "172.16.254.1"            /* the Move's end of its USB-C network link (NCM gadget, usb0) */
#define MR_MAX_MSG 65535

#define MR_SAMPLE_RATE 44100
#define MR_BLOCK 128                          /* frames per SPI transfer (2.902 ms) */
#define MR_SCREEN_W 128
#define MR_SCREEN_H 64
#define MR_SCREEN_BYTES 1024                  /* 8 pages x 128 columns; byte = 8 vertical pixels, bit 0 at the top */

/* ---- device -> host */
enum {
    MR_D_HELLO = 0x01,   /* "MOVR" proto16 caps16 rate32 block16 w8 h8 via8 pad8 out_cap32 name[24] */
    MR_D_INPUT = 0x02,   /* n x { frame32, usb_midi[4] }: every raw event from the surface (cable 0) and USB-A (cable 2) */
    MR_D_AUDIO = 0x03,   /* frame32 nframes16 out_fill16 flags8 [int16 stereo pcm, nframes*2 if flags & MR_AUDIO_HAS_PCM] */
    MR_D_STATUS = 0x04,  /* see mr_status_t, every 500 ms: also the heartbeat */
    MR_D_PONG = 0x05,    /* token32 host_time64 frame32 */
    MR_D_BYE = 0x06,     /* reason8 */
};

/* ---- host -> device */
enum {
    MR_H_HELLO = 0x81,   /* proto16 name[32]: answered with MR_D_HELLO */
    MR_H_LED = 0x82,     /* n x { status8, number8, value8 }: status 0x90|anim (note) or 0xB0|anim (CC) */
    MR_H_LED_RGB = 0x83, /* n x { kind8 (0 note, 1 CC), number8, r8, g8, b8 } */
    MR_H_DISPLAY = 0x84, /* 1024 bytes, packed as MR_SCREEN_BYTES says */
    MR_H_AUDIO = 0x85,   /* int16 stereo interleaved, (len - 1) / 4 frames, appended to the output queue */
    MR_H_MIDI = 0x86,    /* complete MIDI messages (bytes, SysEx allowed) to the device on USB-A */
    MR_H_CONFIG = 0x87,  /* n x { key8, value32 (signed) } */
    MR_H_PING = 0x88,    /* token32 host_time64: answered with MR_D_PONG */
    MR_H_QUIT = 0x89,    /* leave Move Remote: Move's own software starts again */
    MR_H_RAW = 0x8A,     /* n x usb_midi[4] to the surface, cable 0 only, CIN 0x4..0xE (an escape hatch) */
};

/* MR_D_HELLO caps */
#define MR_CAP_AUDIO_OUT 0x0001
#define MR_CAP_AUDIO_IN 0x0002
#define MR_CAP_RGB 0x0004
#define MR_CAP_USB_A_MIDI 0x0008
#define MR_CAP_SIMULATOR 0x8000               /* the Mac simulator, not a Move */

/* MR_D_HELLO via: the link the host came in on */
enum { MR_VIA_WIFI = 0, MR_VIA_USB = 1, MR_VIA_LOCAL = 2 };

/* MR_D_AUDIO flags */
#define MR_AUDIO_HAS_PCM 0x01                 /* the input (mic / line in) follows */
#define MR_AUDIO_UNDERRUN 0x02                /* the output queue ran dry during this batch */

/* MR_D_BYE reasons */
enum { MR_BYE_EXIT = 1, MR_BYE_REPLACED = 2, MR_BYE_SHUTDOWN = 3, MR_BYE_QUIT = 4, MR_BYE_PROTOCOL = 5 };

/* MR_H_CONFIG keys */
enum {
    MR_CFG_AUDIO_CLOCK = 1,   /* 0/1, default 1: send MR_D_AUDIO every batch (the host's audio clock) */
    MR_CFG_AUDIO_IN = 2,      /* 0/1, default 0: put the input pcm in MR_D_AUDIO */
    MR_CFG_AUDIO_BATCH = 3,   /* 1..8 blocks per MR_D_AUDIO, default 2 (256 frames, 5.8 ms) */
    MR_CFG_OUT_GAIN = 4,      /* 0..256 (256 = unity), default 256 */
    MR_CFG_LEDS_OFF = 5,      /* any value: every LED off */
    MR_CFG_FORWARD_EXIT = 6,  /* reserved */
};

/* MR_D_STATUS payload (packed, 32 bytes) */
#define MR_STATUS_LEN 32
/*  0 frame32       device sample clock
 *  4 battery8      as the surface reports it
 *  5 power8        the surface's power bits (bit 3: the power button held)
 *  6 jacks8        bit 0 line in plugged, bit 1 line out / headphones plugged
 *  7 flags8        bit 0 audio thread real-time, bit 1 test mode (output scaled down)
 *  8 underruns32   output blocks that ran dry since connect
 * 12 overruns32    output frames dropped (queue full) since connect
 * 16 late32        SPI transfers more than 400 us late since connect
 * 20 dropped32     MR_D_AUDIO messages not sent (the link was backed up) since connect
 * 24 led_pending16 LED writes waiting for the surface
 * 26 out_fill16    frames in the output queue
 * 28 test_vol8     percent of output in test mode (100 otherwise)
 * 29 pad8[3] */

/* ---- the Move's controls (cable 0), for decoding MR_D_INPUT */
enum {
    MR_NOTE_KNOB_TOUCH0 = 0,  /* 0..7 knob 1..8 touch, 8 volume touch, 9 jog touch: 0x90 vel>0 touch, 0x80 / vel 0 release */
    MR_NOTE_VOL_TOUCH = 8,
    MR_NOTE_JOG_TOUCH = 9,
    MR_NOTE_STEP0 = 16,       /* 16..31 step buttons 1..16 */
    MR_NOTE_PAD0 = 68,        /* 68..99 pads, bottom-left to top-right, rows of 8; 0xA0 = poly aftertouch */
    MR_NPADS = 32,
    MR_NSTEPS = 16,
};
enum {
    MR_CC_JOG_CLICK = 3, MR_CC_JOG = 14, MR_CC_STEP_ICON0 = 16, MR_CC_TRACK4 = 40, MR_CC_TRACK1 = 43,
    MR_CC_SHIFT = 49, MR_CC_MENU = 50, MR_CC_BACK = 51, MR_CC_CAPTURE = 52, MR_CC_DOWN = 54, MR_CC_UP = 55,
    MR_CC_UNDO = 56, MR_CC_LOOP = 58, MR_CC_COPY = 60, MR_CC_LEFT = 62, MR_CC_RIGHT = 63, MR_CC_KNOB1 = 71,
    MR_CC_VOLUME = 79, MR_CC_PLAY = 85, MR_CC_REC = 86, MR_CC_MUTE = 88, MR_CC_LINE_IN = 114, MR_CC_LINE_OUT = 115,
    MR_CC_SAMPLE = 118, MR_CC_DELETE = 119,
};

#endif
