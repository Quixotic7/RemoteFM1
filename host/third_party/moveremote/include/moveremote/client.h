/* SPDX-License-Identifier: MPL-2.0 */
/* libmoveremote: a host's end of Move Remote. Plain C99 over POSIX sockets (macOS, iOS, Linux).
 *
 *   mr_client_t *c = mr_connect(NULL, 0, "my app", &callbacks, 2000);   // NULL: USB first, then move.local
 *   while (mr_poll(c, 10) >= 0) { ... mr_led(c, ...); mr_display(c, packed); ... }
 *   mr_close(c);
 *
 * One thread per client: callbacks run inside mr_poll(), and the send functions must be called from that same
 * thread (or under the caller's own lock). Sends write straight to the socket (TCP_NODELAY) and block only if the
 * link is backed up. */
#ifndef MOVEREMOTE_CLIENT_H
#define MOVEREMOTE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- decoded input */
typedef enum {
    MR_EV_PAD = 1,         /* index 0..31 (bottom-left first, rows of 8), value: velocity 1..127, 0 = released */
    MR_EV_PAD_PRESSURE,    /* index 0..31, value: polyphonic aftertouch 0..127 */
    MR_EV_PRESSURE,        /* value: channel pressure 0..127 (the surface sends it too) */
    MR_EV_STEP,            /* index 0..15, value 1 down / 0 up */
    MR_EV_BUTTON,          /* index: the button's CC (MR_CC_SHIFT, MR_CC_PLAY, track buttons 40..43 ...), value 1/0 */
    MR_EV_KNOB,            /* index 0..7 knobs, 8 volume; value: signed detents (+ clockwise) */
    MR_EV_TOUCH,           /* index 0..7 knobs, 8 volume, 9 jog wheel; value 1 touched / 0 released */
    MR_EV_JOG,             /* value: signed detents */
    MR_EV_JOG_CLICK,       /* value 1 down / 0 up */
    MR_EV_JACK,            /* index 0 line in, 1 line out / headphones; value 1 plugged */
    MR_EV_MIDI,            /* a USB-MIDI packet from the device on USB-A (cable 2): raw[] */
    MR_EV_SYSEX,           /* a surface SysEx packet (the power button's reports): raw[] */
    MR_EV_OTHER,           /* anything else from the surface: raw[] */
} mr_ev_type_t;

typedef struct {
    mr_ev_type_t type;
    int index, value;
    uint32_t frame;        /* the device's sample clock at the block the event arrived in */
    uint8_t raw[4];        /* the USB-MIDI packet as the surface sent it */
} mr_event_t;

/* decodes one MR_D_INPUT record (also usable on its own) */
void mr_decode(const uint8_t pkt[4], uint32_t frame, mr_event_t *ev);
const char *mr_button_name(int cc);   /* "Shift", "Play", "Track 1" ... or NULL */

/* ---- what the device says */
typedef struct {
    uint16_t proto, caps;
    uint32_t sample_rate, out_capacity;
    uint16_t block;
    uint8_t screen_w, screen_h, via;   /* via: MR_VIA_USB / MR_VIA_WIFI / MR_VIA_LOCAL */
    char name[25];
} mr_hello_t;

typedef struct {
    uint32_t frame, underruns, overruns, late, dropped;
    uint8_t battery, power, jacks, flags, test_vol;
    uint16_t led_pending, out_fill;
} mr_status_t;

typedef struct {
    void (*on_hello)(void *user, const mr_hello_t *h);
    void (*on_input)(void *user, const mr_event_t *ev);
    /* the device's audio clock: `frames` frames went out of its speaker queue (and, if `in` is not NULL, came in
     * from its mic / line in, stereo int16). out_fill: what is still queued. Answer with mr_audio() as you like;
     * mr_pacer_t below says how much */
    void (*on_audio)(void *user, uint32_t frame, const int16_t *in, int frames, int out_fill, int flags);
    void (*on_status)(void *user, const mr_status_t *st);
    void (*on_pong)(void *user, uint32_t token, uint64_t host_time, uint32_t device_frame);
    void (*on_bye)(void *user, int reason);
    void *user;
} mr_callbacks_t;

typedef struct mr_client mr_client_t;

/* host NULL: try the USB link (MR_USB_ADDR), then move.local. port 0: MR_PORT. Blocks up to timeout_ms per address
 * for the connection and the device's HELLO. NULL on failure */
mr_client_t *mr_connect(const char *host, int port, const char *name, const mr_callbacks_t *cb, int timeout_ms);
/* after mr_connect() returned NULL: why, address by address ("172.16.254.1: No route to host; move.local: ...") */
const char *mr_last_error(void);
/* reads what has arrived (waiting up to timeout_ms) and runs the callbacks. < 0 once the link is gone */
int mr_poll(mr_client_t *c, int timeout_ms);
int mr_fd(const mr_client_t *c);      /* to wait on in your own loop */
const mr_hello_t *mr_hello(const mr_client_t *c);
void mr_close(mr_client_t *c);

/* ---- what the host sends (each returns 0, or < 0 if the link is gone) */
/* palette / brightness LED: cc 0 = a note LED (pads 68..99, steps 16..31), 1 = a CC LED (buttons, knob rings ...);
 * anim 0 = steady, 1..5 fade, 6..10 pulse, 11..15 blink (the device's tempo) */
int mr_led(mr_client_t *c, int cc, int number, int value, int anim);
int mr_led_rgb(mr_client_t *c, int cc, int number, int r, int g, int b);
int mr_pad_rgb(mr_client_t *c, int pad, int r, int g, int b);   /* pad 0..31 */
int mr_leds_raw(mr_client_t *c, const uint8_t *triples, int n); /* n x {status, number, value}: one message */
int mr_display(mr_client_t *c, const uint8_t packed[MR_SCREEN_BYTES]);
int mr_audio(mr_client_t *c, const int16_t *stereo, int frames);
int mr_midi(mr_client_t *c, const uint8_t *bytes, int n);      /* to the device on USB-A */
int mr_config(mr_client_t *c, int key, int32_t value);
int mr_ping(mr_client_t *c, uint32_t token);
int mr_quit(mr_client_t *c);                                    /* back to Move's own software */
int mr_raw(mr_client_t *c, const uint8_t *packets, int n);      /* n USB-MIDI packets to the surface (cable 0) */

uint64_t mr_now_us(void);

/* ---- keeping the device's speaker queue fed from on_audio: how many frames to send now */
typedef struct { int target, primed, cool; } mr_pacer_t;
void mr_pacer_init(mr_pacer_t *p, int target_frames);   /* 512 over USB is comfortable, 2048+ over Wi-Fi */
int mr_pacer_frames(mr_pacer_t *p, int consumed, int out_fill);

#ifdef __cplusplus
}
#endif

#endif
