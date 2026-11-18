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
    RFM_MOVE_SCALE_PENTA_MAJOR, RFM_MOVE_SCALE_PENTA_MINOR, RFM_MOVE_SCALE_HARM_MINOR, RFM_MOVE_SCALE_BLUES,
    RFM_MOVE_NSCALES                                    /* 8: Track 2 held, row 2's pads */
};
#define RFM_MOVE_OCT_MIN (-4)                           /* the octave: the lowest pad of layouts 1, 2, 4 is */
#define RFM_MOVE_OCT_MAX 4                              /* 48 + root + 12 * octave (C3 = 48 at octave 0) */
#define RFM_MOVE_DRUM_BASE 36                           /* the drum layout's bottom-right pad (C2) */
#define RFM_MOVE_THUMB_LUMA 80                          /* screen thumbnail: an FM-1 pixel above this luma (0..255) is white */
#define RFM_MOVE_POPUP_MS 1000                          /* a popup stays this long after its control's release / last turn */
#define RFM_MOVE_SCREEN_LIT 64                          /* FM-1 mode's drawing: a button is drawn lit from this LED level */
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
    /* the sequencer's projects (32 slots the host stores; host/src/move/README.md, "Projects") */
    RFM_MOVE_ACT_PROJECT_SAVE = 8,                      /* index slot 0..31: store data/len (a project blob) */
    RFM_MOVE_ACT_PROJECT_LOAD = 9,                      /* index slot: answer with rfm_move_project_load(m, slot, ..) */
    RFM_MOVE_ACT_PROJECT_DELETE = 10,                   /* index slot: forget it */
};
typedef struct {
    uint8_t kind;                                       /* RFM_MOVE_ACT_* */
    uint8_t index, down, vel;
    int16_t value;
    uint8_t n, bytes[3];
    /* when: actions from rfm_move_seq_advance carry the frame (in that call's units, 0..frames-1, from the start
     * of the call) the event is due at; every other action has 0 = now */
    uint32_t frame;
    const uint8_t *data;                                /* PROJECT_SAVE: the blob, valid during the callback only */
    uint32_t len;
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

/* ---- the sequencer (Track 4; host/src/move/README.md, "Sequencer") */
#define RFM_MOVE_SEQ_TRACKS 16                          /* track n plays MIDI channel n by default */
#define RFM_MOVE_SEQ_SCENES 16
#define RFM_MOVE_SEQ_CLIPS (RFM_MOVE_SEQ_TRACKS * RFM_MOVE_SEQ_SCENES)   /* clip id = track * 16 + scene */
#define RFM_MOVE_SEQ_PAGE 16                            /* steps a page: a bar of 16ths */
#define RFM_MOVE_SEQ_PAGES 16
#define RFM_MOVE_SEQ_MAXLEN (RFM_MOVE_SEQ_PAGE * RFM_MOVE_SEQ_PAGES)     /* 256 steps */
#define RFM_MOVE_SEQ_TPS 24                             /* ticks a step (96 PPQN); a MIDI clock = 4 ticks */
#define RFM_MOVE_SEQ_BAR (RFM_MOVE_SEQ_TPS * RFM_MOVE_SEQ_PAGE)          /* 384 ticks, 4/4 */
#define RFM_MOVE_SEQ_POLY 8                             /* notes a step */
#define RFM_MOVE_SEQ_NOTES 4096                         /* notes a project */
#define RFM_MOVE_SEQ_LOCKS 2048                         /* CC locks a project */
#define RFM_MOVE_SEQ_UNDO 2048                          /* undo journal entries */
#define RFM_MOVE_SEQ_SOUNDING 64                        /* sequencer notes sounding at once */
#define RFM_MOVE_SEQ_CAPTURE 256                        /* Capture: pad events kept */
#define RFM_MOVE_SEQ_CAPTURE_BARS 8                     /* .. from the last 8 bars */
#define RFM_MOVE_SEQ_BPM10_MIN 200                      /* 20.0 .. 300.0 BPM */
#define RFM_MOVE_SEQ_BPM10_MAX 3000
#define RFM_MOVE_PROJECTS 32
#define RFM_MOVE_PROJECT_VERSION 1
#define RFM_MOVE_AUTOSAVE_MS 2000                       /* an edited project is saved this long after the last edit */
/* the blob: header 24, tracks 16 x 4, clip lengths 256 x 2, notes 2 + n x 7, locks 2 + n x 4, FNV-1a 4 */
#define RFM_MOVE_PROJECT_FIXED (24 + RFM_MOVE_SEQ_TRACKS * 4 + RFM_MOVE_SEQ_CLIPS * 2 + 2 + 2 + 4)
#define RFM_MOVE_PROJECT_MAX (RFM_MOVE_PROJECT_FIXED + RFM_MOVE_SEQ_NOTES * 7 + RFM_MOVE_SEQ_LOCKS * 4)
enum { RFM_MOVE_CLOCK_AUTO = 0, RFM_MOVE_CLOCK_INTERNAL = 1, RFM_MOVE_CLOCK_EXTERNAL = 2 };

typedef struct {                                        /* a note in a clip; sorted by (clip, step, note) */
    uint8_t clip, step, note, vel;
    int8_t micro;                                       /* ticks off the step, -12..+11 */
    uint8_t rsv;
    uint16_t len;                                       /* ticks, 1..256 x 24 */
} rfm_move_note_t;
typedef struct { uint8_t clip, step, cc, val; } rfm_move_lock_t;   /* a CC lock; sorted by (clip, step, cc) */
typedef struct {                                        /* an undo journal entry */
    uint16_t group;
    uint8_t type, rsv;
    uint16_t old, nu;
    rfm_move_note_t n;
} rfm_move_op_t;
typedef struct {
    uint8_t ch, mute;                                   /* (saved) */
    int8_t active, sel;                                 /* (saved) the clip that plays (-1 none), the one edited */
    int8_t queued;                                      /* -2 nothing, -1 stop, 0..15: launch at the next bar */
    uint8_t grow, grow_stop;                            /* recording into an empty clip: it grows a bar at a time */
    uint8_t rsv;
    int32_t t0;                                         /* the song tick the active clip started at */
} rfm_move_track_t;
typedef struct { uint8_t ch, note, track, rsv; int32_t off; } rfm_move_sounding_t;
typedef struct { int32_t t; uint8_t note, vel, rsv[2]; } rfm_move_cap_t;   /* t: timeline tick, vel 0 = off */
typedef struct { int32_t t; uint8_t clip, note, vel, on; } rfm_move_recnote_t;
typedef struct {                                        /* a stored project in brief (rfm_move_project_info) */
    uint8_t ok;                                         /* 1: known */
    uint8_t clips;                                      /* clips with notes or locks */
    uint16_t bpm10;
    uint16_t notes[RFM_MOVE_SEQ_TRACKS];                /* notes on each track */
} rfm_move_proj_info_t;

typedef struct {
    /* ---- the project (what a blob holds) */
    uint16_t bpm10;                                     /* the internal clock, BPM x 10 */
    uint8_t clock_mode, fixed_vel, sel_track;
    rfm_move_track_t tr[RFM_MOVE_SEQ_TRACKS];
    uint16_t len[RFM_MOVE_SEQ_CLIPS];                   /* clip lengths, steps 1..256 */
    uint16_t nnotes, nlocks;
    rfm_move_note_t notes[RFM_MOVE_SEQ_NOTES];
    rfm_move_lock_t locks[RFM_MOVE_SEQ_LOCKS];
    /* ---- the clock */
    uint64_t now_us, us_rem;                            /* time advanced so far */
    uint64_t ph;                                        /* progress to the next tick, of den */
    uint32_t den_rate;                                  /* the rate den was made for */
    int32_t tl;                                         /* timeline tick (counts every tick; the clock's position) */
    int32_t T;                                          /* song tick while playing */
    uint8_t playing, start_pending, following, ext_run;   /* start_pending 1 now, 2 at the timeline's next bar, 3 count-in */
    int16_t count_in;                                   /* start_pending 3: ticks to go (Rec from stopped: a bar) */
    int32_t ext_clocks;                                 /* MIDI clocks since the reference (-1 before the first) */
    uint64_t ext_last_us, ext_win_us;
    int32_t ext_win_n;
    uint32_t ext_period_us;                             /* one MIDI clock, measured (0 unknown) */
    uint16_t spp;                                       /* the last song position (16ths) */
    uint32_t frame;                                     /* the frame actions are stamped with now */
    rfm_move_sounding_t snd[RFM_MOVE_SEQ_SOUNDING];
    uint8_t nsnd;
    /* ---- recording, capture */
    uint8_t rec;                                        /* armed */
    uint16_t rec_group;
    rfm_move_recnote_t recn[RFM_MOVE_NPADS];            /* notes being recorded, by pad */
    rfm_move_cap_t cap[RFM_MOVE_SEQ_CAPTURE];
    uint16_t ncap, cap_head;
    /* ---- undo */
    rfm_move_op_t ops[RFM_MOVE_SEQ_UNDO];
    uint16_t un, utop, group, broken;
    /* ---- the clipboard */
    uint8_t clip_kind, cut;                             /* 0 empty, 1 a step, 2 a clip, 3 a track; cut: Delete made it */
    uint8_t clip_src, clip_step;
    uint16_t clip_len, clip_nn, clip_nl;
    rfm_move_note_t clip_notes[RFM_MOVE_SEQ_POLY * RFM_MOVE_SEQ_MAXLEN];
    rfm_move_lock_t clip_locks[RFM_MOVE_SEQ_LOCKS];
    /* ---- projects */
    int8_t proj_cur, proj_pending;
    uint32_t proj_used;
    uint8_t dirty;
    uint64_t dirty_us;
    uint8_t blob[RFM_MOVE_PROJECT_MAX];
    /* ---- the controls */
    uint8_t view;                                       /* 0 track, 1 session */
    uint8_t projects;                                   /* the project grid is up */
    uint8_t mods;                                       /* SEQ_M_* held */
    uint8_t mods_used;                                  /* .. and used with something (no release action) */
    uint8_t copy_src;                                   /* this Copy hold has a source */
    uint8_t page, track_page, scene_off;
    uint8_t nlast;                                      /* step entry's last notes (0: the root note) */
    uint8_t last_note[RFM_MOVE_SEQ_POLY], last_vel[RFM_MOVE_SEQ_POLY];
    uint32_t chord;                                     /* bits: pads held as a chord (track view, no step held) */
    uint8_t pad_vel[RFM_MOVE_NPADS];                    /* .. their velocities */
    uint8_t paste;                                      /* a copy waits for its target (Copy released) */
    uint8_t copy_pasted;                                /* this Copy hold pasted */
    uint16_t step_held;                                 /* bits: steps held (track view) */
    uint16_t step_add, step_edit;                       /* .. pressed onto an empty note / edited while held */
    int8_t step_first;                                  /* the first step held, -1 none */
    uint32_t tap_ms[4];
    uint8_t ntap;
    uint8_t msg;                                        /* a short message on the screen (SEQ_MSG_*) */
    uint32_t msg_ms, ui_ms;
    rfm_move_proj_info_t info[RFM_MOVE_PROJECTS];       /* what the project grid draws of each slot */
} rfm_move_seq_t;

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
    /* the screen's popup (host/src/move/README.md, "The screen"): what it shows, for which control, and since when */
    uint8_t pop;                                        /* what (move_int.h POP_*), 0 none */
    uint8_t pop_i;                                      /* its knob (0..7, 8 volume), step, pad or project slot */
    int16_t pop_val;                                    /* an FM-1 encoder's detents in this gesture */
    uint32_t pop_ms;                                    /* the sequencer clock (ms) at its last touch, turn or release */
    uint8_t menu_what, menu_i;                          /* a held Track button's menu: the setting changed last */
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
    /* the sequencer (about 150 KB: keep rfm_move_t static or on the heap, not on a small stack) */
    rfm_move_seq_t seq;
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
 * last one. Popups time out on the sequencer's clock: call rfm_move_seq_advance in every mode */
void rfm_move_screen(rfm_move_t *m, const uint16_t *rgb565, uint8_t out[MR_SCREEN_BYTES]);
/* 1: the screen changes with the clock now (a popup or a message up, the sequencer playing or counting in): draw it
 * again as rfm_move_seq_advance runs, not only after input or a new FM-1 frame */
int rfm_move_screen_timed(const rfm_move_t *m);

/* ---- the sequencer's clock and transport
 * The clock: call rfm_move_seq_advance for every block of time, at the start of the block (playing or not; it
 * also runs blinking, tap tempo's display and autosave). Actions it gives carry `frame`: the offset in this call's
 * units at which the host plays them (sample accurate when frames are audio frames). rate 1000000 = microseconds.
 * The external clock: the firmware's MIDI out realtime bytes, one message a call: F8 clock (24 PPQN), FA start,
 * FB continue, FC stop, F2 lsb msb song position. In RFM_MOVE_CLOCK_AUTO the external clock wins while clocks
 * arrive (and for 500 ms after the last); its events are stamped frame 0 (now). */
void rfm_move_seq_advance(rfm_move_t *m, uint32_t frames, uint32_t rate);
void rfm_move_seq_midi_in(rfm_move_t *m, const uint8_t *msg, int n);
void rfm_move_seq_play(rfm_move_t *m, int on);         /* as Play in Sequencer mode (1 start, 0 stop) */
void rfm_move_seq_set_bpm(rfm_move_t *m, int bpm10);   /* the internal clock, BPM x 10 (200..3000) */
int rfm_move_seq_bpm10(const rfm_move_t *m);           /* the tempo in use (measured when following) */
void rfm_move_seq_set_clock(rfm_move_t *m, int mode);  /* RFM_MOVE_CLOCK_* */
int rfm_move_seq_following(const rfm_move_t *m);       /* 1: the external clock drives the sequencer now */

/* ---- projects: 32 slots the host stores (the core never does IO)
 * A blob: "RMSQ", version, then the project; little-endian, every field checked on load, an FNV-1a check at the
 * end; at most RFM_MOVE_PROJECT_MAX bytes. */
int rfm_move_project_save(rfm_move_t *m, uint8_t *out, uint32_t cap);   /* bytes, or -1 (cap too small) */
int rfm_move_project_check(const uint8_t *b, uint32_t n);                /* 0 a valid blob, else < 0 */
/* the host's answer to PROJECT_LOAD (or at start, the last project): slot 0..31 becomes the current project; b NULL
 * or n 0: an empty project. 0 ok; < 0 an invalid blob: nothing changes. Stops the sequencer (notes released).
 * May be called from inside the action callback. */
int rfm_move_project_load(rfm_move_t *m, int slot, const uint8_t *b, uint32_t n);
void rfm_move_project_dir(rfm_move_t *m, uint32_t used);   /* bit n: slot n holds a project */
/* what the project grid and its popup draw of a stored project (its tempo, clips, notes a track): the host passes
 * each stored blob once (at start, after an import); saves, loads and deletes keep it up to date. 0 ok; < 0 an
 * invalid blob (the slot is drawn without its content) */
int rfm_move_project_info(rfm_move_t *m, int slot, const uint8_t *b, uint32_t n);
void rfm_move_project_flush(rfm_move_t *m);            /* the autosave now, if the project changed */
int rfm_move_project_slot(const rfm_move_t *m);        /* the current slot */

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
