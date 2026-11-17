/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move: an Ableton Move (running Move Remote) as a RemoteFM1 host's panel. Portable C99, no OS calls, no
 * allocation: the state is a plain struct the host owns. The mapping is the owner's design
 * (design/remotefm1move.movedesign.json); host/src/move/README.md has the tables and the decisions.
 *
 *   rfm_move_t m;  rfm_move_init(&m, &settings_or_NULL, on_action, ctx);
 *   Move input  : rfm_move_input(&m, &mr_event, now_ms)        -> actions through the callback
 *   FM-1 LEDs   : rfm_move_panel(&m, levels44, play_green)     (levels[col * 4 + line], 0..255, as hosts have them)
 *   Move LEDs   : n = rfm_move_leds(&m, out, max)              only the LEDs that changed since the last call
 *   Move screen : rfm_move_screen(&m, rgb565_240x240_or_NULL, out1024)
 *   link lost / mode changed: rfm_move_release_all(&m)        (every held key, button and note let go)
 *
 * Actions use the protocol's MATRIX order (proto/rfm1.h, libremotefm1's rfm_event_t): buttons 0 OCT- 1 OCT+ 2 FX ..
 * 13 REC, encoders 0 SELECT 1 ALGO 2..5 K1..K4 6 PRESETS; keys 0..26 (key 0 = F3). The web player's panel.js names
 * them by label / role: labelToMatrix / roleToMatrix convert. */
#ifndef RFM_MOVE_H
#define RFM_MOVE_H
#include <stdint.h>

#include "moveremote/client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFM_MOVE_NKEYS 27
#define RFM_MOVE_NBTN 14
#define RFM_MOVE_NENC 7
#define RFM_MOVE_NPADS 32
#define RFM_MOVE_NSTEPS 16
#define RFM_MOVE_NKNOBS 8
#define RFM_MOVE_NBANKS 4
#define RFM_MOVE_NLEVELS 44                             /* the FM-1's LEDs: levels[col * 4 + line], 11 columns */
#define RFM_MOVE_LABEL_MAX 16

/* the FM-1's matrix buttons and encoders (proto/rfm1.h order) */
enum {
    RFM_MOVE_B_OCTDN = 0, RFM_MOVE_B_OCTUP = 1, RFM_MOVE_B_FX = 2, RFM_MOVE_B_SEL = 3, RFM_MOVE_B_ENV = 4,
    RFM_MOVE_B_LFO = 5, RFM_MOVE_B_EDIT = 6, RFM_MOVE_B_GLO = 7, RFM_MOVE_B_HOME = 8, RFM_MOVE_B_SAVE = 9,
    RFM_MOVE_B_ARP = 10, RFM_MOVE_B_SEQ = 11, RFM_MOVE_B_PLAY = 12, RFM_MOVE_B_REC = 13,
};
enum {
    RFM_MOVE_E_SELECT = 0, RFM_MOVE_E_ALGO = 1, RFM_MOVE_E_K1 = 2, RFM_MOVE_E_K2 = 3, RFM_MOVE_E_K3 = 4,
    RFM_MOVE_E_K4 = 5, RFM_MOVE_E_PRESETS = 6,
    RFM_MOVE_E_MASTER = 7,                              /* (labels only: MASTER is not an encoder) */
};

/* the four modes (Track 1..4) and the MIDI modes' pad layouts */
enum { RFM_MOVE_MODE_FM1 = 0, RFM_MOVE_MODE_MIDI = 1, RFM_MOVE_MODE_CONTROL = 2, RFM_MOVE_MODE_SEQ = 3 };
enum { RFM_MOVE_LAYOUT_SCALE = 0, RFM_MOVE_LAYOUT_CHROMATIC = 1, RFM_MOVE_LAYOUT_DRUM = 2, RFM_MOVE_LAYOUT_OCTAVE = 3 };
enum {
    RFM_MOVE_SCALE_MAJOR = 0, RFM_MOVE_SCALE_MINOR, RFM_MOVE_SCALE_DORIAN, RFM_MOVE_SCALE_MIXOLYDIAN,
    RFM_MOVE_SCALE_PENTA_MAJOR, RFM_MOVE_SCALE_PENTA_MINOR, RFM_MOVE_NSCALES
};
#define RFM_MOVE_OCT_MIN (-4)                           /* the octave: the lowest pad of layouts 1, 2, 4 is */
#define RFM_MOVE_OCT_MAX 4                              /* 48 + root + 12 * octave (C3 = 48 at octave 0) */
#define RFM_MOVE_DRUM_BASE 36                           /* the drum layout's bottom-right pad (C2) */
#define RFM_MOVE_THUMB_LUMA 80                          /* screen thumbnail: an FM-1 pixel above this luma (0..255) is white */
#define RFM_MOVE_MASTER_STEP 16                        /* MASTER per volume knob detent */

/* ---- actions (the OMX-27 link's vocabulary: web/site/js/omx27-views.js) */
enum {
    RFM_MOVE_ACT_KEY = 1,                               /* index key 0..26, down 1/0, vel 1..127 on a down */
    RFM_MOVE_ACT_PRESS = 2,                             /* index key, value pressure 0..127 (lean: rfm_move_lean) */
    RFM_MOVE_ACT_BTN = 3,                               /* index matrix button 0..13, down 1/0 */
    RFM_MOVE_ACT_TURN = 4,                              /* index matrix encoder 0..6, value detents (+ clockwise) */
    RFM_MOVE_ACT_MASTER = 5,                            /* value 0..1023 */
    RFM_MOVE_ACT_MIDI = 6,                              /* n bytes: one whole message into the firmware's MIDI in */
    RFM_MOVE_ACT_SETTINGS = 7,                          /* the settings changed: save them when convenient */
};
typedef struct {
    uint8_t kind;                                       /* RFM_MOVE_ACT_* */
    uint8_t index, down, vel;
    int16_t value;
    uint8_t n, bytes[3];
} rfm_move_act_t;
typedef void (*rfm_move_act_fn)(void *ctx, const rfm_move_act_t *a);
/* pressure 0..127 as emu_hal_t / fm1panel_t key_lean 0..100 (as rfm_panel_lean) */
static inline uint8_t rfm_move_lean(int p) { return (uint8_t)(((p < 0 ? 0 : p > 127 ? 127 : p) * 100 + 63) / 127); }

/* ---- the settings a host keeps (rfm_move_save / rfm_move_load: a versioned byte image) */
typedef struct {
    uint8_t mode;                                       /* RFM_MOVE_MODE_* */
    uint8_t layout;                                     /* RFM_MOVE_LAYOUT_* */
    uint8_t channel;                                    /* MIDI channel 0..15 (shown 1..16) */
    int8_t octave;                                      /* RFM_MOVE_OCT_MIN..MAX */
    uint8_t scale, root;                                /* RFM_MOVE_SCALE_*, 0..11 (0 = C) */
    uint8_t bank;                                       /* Control mode's CC bank 0..3 */
    uint8_t cc[RFM_MOVE_NBANKS][RFM_MOVE_NKNOBS];       /* each bank's CC numbers, knob 1..8 */
    uint8_t drum[RFM_MOVE_NPADS];                       /* the drum layout's notes, by pad (0 = bottom-left) */
} rfm_move_settings_t;
#define RFM_MOVE_SETTINGS_VERSION 1
#define RFM_MOVE_SETTINGS_BYTES (6 + 7 + RFM_MOVE_NBANKS * RFM_MOVE_NKNOBS + RFM_MOVE_NPADS)
void rfm_move_settings_default(rfm_move_settings_t *s);
/* "RMV" version8 len16, then the fields; returns RFM_MOVE_SETTINGS_BYTES */
uint32_t rfm_move_save(const rfm_move_settings_t *s, uint8_t out[RFM_MOVE_SETTINGS_BYTES]);
/* 0 ok (every field checked and clamped); -1 not a settings image (s is left as the defaults) */
int rfm_move_load(rfm_move_settings_t *s, const uint8_t *b, uint32_t n);

/* ---- a Move LED write (only the changed ones come out of rfm_move_leds) */
typedef struct {
    uint8_t rgb;                                        /* 1: LED_RGB (r g b); 0: LED (r = palette / brightness) */
    uint8_t cc;                                         /* 0 a note LED (pads 68..99, steps 16..31), 1 a CC LED */
    uint8_t number;
    uint8_t r, g, b;
} rfm_move_led_t;
/* every LED rfm_move drives: 32 pads and 16 steps (RGB note LEDs), 15 RGB CC LEDs (Play, Rec, Sample, Track 1..4,
 * knob rings 1..8), 13 white CC LEDs (Shift, Menu, Back, Capture, Down, Up, Undo, Loop, Copy, Left, Right, Mute,
 * Delete) */
#define RFM_MOVE_NLEDS 76

/* ---- the state */
typedef struct {
    uint8_t kind;                                       /* what a pressed control did, for its release */
    uint8_t a, b;                                       /* key / button / (channel, note) */
} rfm_move_hold_t;

typedef struct {
    rfm_move_settings_t s;
    rfm_move_act_fn fn;
    void *ctx;
    /* input */
    rfm_move_hold_t pad[RFM_MOVE_NPADS], step[RFM_MOVE_NSTEPS], btn[128];   /* btn: by Move CC */
    uint8_t pad_down[RFM_MOVE_NPADS], pad_press[RFM_MOVE_NPADS];
    uint8_t key_count[RFM_MOVE_NKEYS], btn_count[RFM_MOVE_NBTN], key_press[RFM_MOVE_NKEYS];
    uint8_t note_count[16][128];
    uint8_t track_held;                                 /* bit t: Track t + 1 held */
    uint8_t cc_val[RFM_MOVE_NBANKS][RFM_MOVE_NKNOBS];   /* Control mode's values (not saved), 64 at first */
    uint16_t master;                                    /* MASTER as the host last said or the volume knob set */
    int8_t edit_pad;                                    /* the drum pad the jog wheel is editing, -1 none */
    /* what the screen says about the last control touched or changed */
    uint8_t last_kind;                                  /* 0 none, 1 FM-1 encoder, 2 MASTER, 3 CC knob, 4 drum note */
    uint8_t last_index;
    int16_t last_value;
    uint32_t last_ms;
    int8_t touched;                                     /* the knob (0..7, 8 volume, 9 jog) touched now, -1 none */
    char label[RFM_MOVE_NENC + 1][RFM_MOVE_LABEL_MAX + 1];   /* the firmware's knob labels ("" = the default) */
    /* the FM-1's LEDs */
    uint8_t levels[RFM_MOVE_NLEVELS], play_green;
    int8_t led_of[41];                                  /* panel id (0..13 button, 14 + k key) -> levels index */
    /* the Move's LEDs as last written (rfm_move_leds) */
    uint8_t sent[RFM_MOVE_NLEDS][3];
    uint8_t sent_ok[RFM_MOVE_NLEDS];
    /* the screen */
    uint8_t thumb[64][64];                              /* the last FM-1 screen, 1 bit a pixel (thresholded) */
} rfm_move_t;

/* settings NULL: the defaults. fn gets every action (it may be NULL). */
void rfm_move_init(rfm_move_t *m, const rfm_move_settings_t *settings, rfm_move_act_fn fn, void *ctx);
void rfm_move_input(rfm_move_t *m, const mr_event_t *ev, uint32_t now_ms);
/* every held FM-1 key, button and MIDI note let go (ups, pressure 0, note offs); the track buttons forgotten */
void rfm_move_release_all(rfm_move_t *m);
void rfm_move_set_mode(rfm_move_t *m, int mode);       /* as a Track button does (releases all on a change) */
void rfm_move_set_scale(rfm_move_t *m, int scale, int root);
void rfm_move_set_master(rfm_move_t *m, int value);    /* MASTER as the host has it (the volume knob moves from it) */
/* a knob's label from the firmware (fm1core labels: by encoder, RFM_MOVE_E_MASTER for MASTER); NULL or "" = default */
void rfm_move_set_label(rfm_move_t *m, int enc, const char *text);
/* the panel's wiring (FM1P_KEYMAP_INIT's shape: id at [row][col], rows 1..4 = LED lines 0..3); NULL = the FM-1's */
void rfm_move_set_keymap(rfm_move_t *m, const int8_t keymap[6][11]);
/* the FM-1's LEDs: levels[col * 4 + line] 0..255 and PLAY's green level 0..255 */
void rfm_move_panel(rfm_move_t *m, const uint8_t levels[RFM_MOVE_NLEVELS], uint8_t play_green);
/* the Move LED writes now due (at most max; the rest on the next call). Returns how many */
int rfm_move_leds(rfm_move_t *m, rfm_move_led_t *out, int max);
void rfm_move_leds_invalidate(rfm_move_t *m);          /* a new connection: every LED is written again */
/* the Move's 128 x 64 screen, packed (MR_SCREEN_BYTES). rgb565: the FM-1's 240 x 240 screen, or NULL to keep the
 * last one */
void rfm_move_screen(rfm_move_t *m, const uint16_t *rgb565, uint8_t out[MR_SCREEN_BYTES]);

/* ---- the mapping, for hosts' help pages and the tests */
int rfm_move_pad_key(int pad);                          /* FM-1 mode: the key a pad plays, -1 none */
int rfm_move_step_key(int step);                        /* FM-1 mode: the white key a step plays */
int rfm_move_button_of(int move_cc);                    /* FM-1 button a Move button presses, -1 none */
int rfm_move_knob_enc(int knob);                        /* FM-1 mode: the encoder knob 0..7 turns, -1 none */
/* the MIDI modes: the note a pad plays now (-1 none: out of range) with these settings */
int rfm_move_pad_note(const rfm_move_settings_t *s, int pad);
const char *rfm_move_mode_name(int mode);
const char *rfm_move_layout_name(int layout);
const char *rfm_move_scale_name(int scale);
const char *rfm_move_enc_name(int enc);                 /* "SELECT" .. "PRESETS", "MASTER" */

#ifdef __cplusplus
}
#endif
#endif
