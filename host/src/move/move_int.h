/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's insides, shared by move.c (the mapping, the MIDI modes), seq.c (the sequencer's data, clock and
 * engine), seq_ui.c (Sequencer mode's controls, LEDs and screen) and project.c (the project blob). Not an API:
 * the tests include it to look at the data model. */
#ifndef RFM_MOVE_INT_H
#define RFM_MOVE_INT_H
#include "moveremote/gfx.h"
#include "rfm/move.h"

typedef struct { uint8_t r, g, b; } rfmv_rgb_t;

enum { H_NONE = 0, H_KEY, H_BTN, H_NOTE, H_MENU, H_SEQ, H_ARP };
/* the LED list (rfm_move_leds): pads, steps, the RGB CC LEDs, then the 13 white ones (WHITE_CC order) */
enum { L_PAD0 = 0, L_STEP0 = 32, L_PLAY = 48, L_REC, L_SAMPLE, L_TRACK0, L_RING0 = 55, L_WHITE0 = 63, L_ICON0 = 76 };
enum { W_SHIFT, W_MENU, W_BACK, W_CAPTURE, W_DOWN, W_UP, W_UNDO, W_LOOP, W_COPY, W_LEFT, W_RIGHT, W_MUTE, W_DELETE };

extern const uint8_t rfmv_scales[RFM_MOVE_NSCALES][8];   /* [0] = length */
extern const char *const rfmv_note_names[12];
extern const char *const rfmv_scale_names[RFM_MOVE_NSCALES];
extern const rfmv_rgb_t rfmv_mode_rgb[4], rfmv_bank_rgb[4];
extern const rfmv_rgb_t RFMV_WHITE, RFMV_RED, RFMV_GREEN, RFMV_OFF;

int rfmv_clampi(int v, int lo, int hi);
rfmv_rgb_t rfmv_sc(rfmv_rgb_t c, int f);               /* c x f / 256 */
rfmv_rgb_t rfmv_add(rfmv_rgb_t a, rfmv_rgb_t b);
rfmv_rgb_t rfmv_midi_pad(const rfm_move_t *m, int p, rfmv_rgb_t mode);
rfmv_rgb_t rfmv_layout_pad(const rfm_move_t *m, int p, rfmv_rgb_t mode);   /* .. as if not pressed */
int rfmv_track_held(const rfm_move_t *m, int t);
int rfmv_half(const rfm_move_t *m);
int rfmv_hold_shown(const rfm_move_t *m, int t);       /* Track t's hold screen is due (held long enough, or used) */                    /* FM-1 mode, MIDI's NOTES: left-half popups, turn-only knob ones */
int rfmv_seq_trk_held(const rfm_move_t *m);            /* the sequencer: Track 4 held (the tracks, the track's settings) */

/* actions (stamped with m->seq.frame) */
void rfmv_emit(rfm_move_t *m, uint8_t kind, int index, int down, int vel, int value);
void rfmv_emit_midi(rfm_move_t *m, int status, int d1, int d2);
void rfmv_changed(rfm_move_t *m);
void rfmv_note_hold(rfm_move_t *m, int ch, int n, int vel);
void rfmv_note_let(rfm_move_t *m, int ch, int n);
void rfmv_let_go(rfm_move_t *m, rfm_move_hold_t *h);

int rfmv_level(const rfm_move_t *m, int id);           /* the FM-1 LED level 0..255 of a panel id (0..13, 14 + key) */

/* ---- screen.c: the screen (every mode's picture, the popups, the held Track buttons' menus) */
enum { POP_NONE = 0, POP_ENC, POP_MASTER, POP_CC, POP_SELECT, POP_DRUM, POP_TEMPO, POP_LOOP, POP_BANK, POP_STEP,
       POP_PROJ, POP_TRIG, POP_BAR, POP_TRACK, POP_MODE };
#define RFM_MOVE_TAP_MS 300                             /* a Track button released this soon: a tap (not its menu) */
enum { MENU_NONE = 0, MENU_LAYOUT, MENU_SCALE, MENU_ROOT, MENU_CHANNEL, MENU_CC, MENU_TRACK, MENU_TCH, MENU_RATE, MENU_SHIFT };
uint32_t rfmv_now_ms(const rfm_move_t *m);             /* the sequencer clock's ms (what popups time out on) */
void rfmv_pop(rfm_move_t *m, int kind, int i);          /* a popup up (again) now */
void rfmv_pop_let(rfm_move_t *m, int kind, int i);      /* its control let go: it drops RFM_MOVE_POPUP_MS from now */
int rfmv_pop_up(const rfm_move_t *m, int kind);         /* that popup is up now */
void rfmv_menu(rfm_move_t *m, int what, int i);         /* a held Track button's menu: what changed */
int rfmv_big(mr_canvas_t *cv, int x, int y, const char *s, int k, int on);   /* font5x7 at k x; returns the end x */
int rfmv_big_w(const char *s, int k);                   /* .. its width */
void rfmv_dotted(mr_canvas_t *cv, int x, int y, int w, int h);   /* a dotted outline */
void rfmv_cond_name(int c, char *b, int cap);
const char *rfmv_chord_name(const uint8_t *n, int k, char *out, int cap);   /* "Cmaj7", "C/E", "" */           /* "1:2", "FILL" .. */
void rfmv_seq_track_glyph(const rfm_move_t *m, mr_canvas_t *cv, int x, int y, int k);   /* which of the 16 tracks */
void rfmv_xbox(mr_canvas_t *cv, int x, int y, int w, int h);     /* an outline crossed out (a cut) */
void rfmv_tri(mr_canvas_t *cv, int x, int y, int w, int h, int on);   /* a play triangle, pointing right */
void rfmv_disc(mr_canvas_t *cv, int x, int y, int d, int on);         /* a filled circle in a d x d box */
void rfmv_frame(mr_canvas_t *cv, int x, int y, int w, int h);         /* a popup's box: black, a 1 px white frame */
const char *rfmv_note_name(int n, char *b, int cap);
void rfmv_fm1_art(const rfm_move_t *m, mr_canvas_t *cv);   /* FM-1 mode's left half: the owner's drawing, live */

/* ---- seq.c: the data model (sorted arrays), the journal, the clock, the engine */
#define SEQ_CLIP(track, scene) ((track) * RFM_MOVE_SEQ_SCENES + (scene))
enum { SEQ_NONE = -2, SEQ_STOP = -1 };
enum { OP_NOTE_ADD = 1, OP_NOTE_DEL, OP_LOCK, OP_LEN, OP_LSTART };
#define SEQ_NOLOCK 0xFF

void rfmv_seq_init(rfm_move_t *m);                     /* an empty project, the clock reset */
void rfmv_seq_project_clear(rfm_move_t *m);            /* the project's data only */
int rfmv_seq_range(const rfm_move_t *m, int clip, int step, int *lo);   /* notes at (clip, step): [lo, ret) */
int rfmv_seq_clip_range(const rfm_move_t *m, int clip, int *lo);       /* notes of the clip */
int rfmv_seq_lock_range(const rfm_move_t *m, int clip, int step, int *lo);
int rfmv_seq_clip_lock_range(const rfm_move_t *m, int clip, int *lo);
int rfmv_seq_find(const rfm_move_t *m, int clip, int step, int note);  /* index, -1 */
int rfmv_seq_lock_get(const rfm_move_t *m, int clip, int step, int cc); /* value, -1 */
int rfmv_seq_clip_used(const rfm_move_t *m, int clip);                 /* notes or locks */
int rfmv_seq_track_used(const rfm_move_t *m, int track);
int rfmv_seq_clip_has(const rfm_move_t *m, int clip);  /* used, or made (an empty clip shown as a clip) */
void rfmv_seq_clip_make(rfm_move_t *m, int clip, int on);
int rfmv_seq_loop_lo(const rfm_move_t *m, int clip);   /* the loop: [lo, hi) */
int rfmv_seq_loop_hi(const rfm_move_t *m, int clip);
int rfmv_seq_tps(int rate);                            /* ticks a step at a rate */
/* edits (journaled into the current undo group, the project marked changed) */
void rfmv_seq_group(rfm_move_t *m);                    /* a new undo group: one gesture */
int rfmv_seq_note_put(rfm_move_t *m, int clip, int step, int note, int vel, int len, int micro);   /* add/replace */
int rfmv_seq_note_del(rfm_move_t *m, int clip, int step, int note);
void rfmv_seq_step_clear(rfm_move_t *m, int clip, int step);
int rfmv_seq_lock_set(rfm_move_t *m, int clip, int step, int cc, int val);   /* val -1: remove */
void rfmv_seq_len_set(rfm_move_t *m, int clip, int len);
void rfmv_seq_loop_set(rfm_move_t *m, int clip, int lo, int len);   /* the loop's start and length (steps) */
void rfmv_seq_preview(rfm_move_t *m, int clip, int step);   /* a step's notes sound once (a held step) */
int rfmv_seq_trig(const rfm_move_t *m, int clip, int step, int id);   /* a trig parameter (RFM_MOVE_TRIG_*), -1 none */
void rfmv_seq_clip_clear(rfm_move_t *m, int clip);
void rfmv_seq_clip_quantize(rfm_move_t *m, int clip);
int rfmv_seq_clip_double(rfm_move_t *m, int clip);
void rfmv_seq_note_remove_all(rfm_move_t *m, int clip, int note);
int rfmv_seq_undo(rfm_move_t *m);                      /* 1 something undone */
int rfmv_seq_redo(rfm_move_t *m);
/* the clipboard: copy (kind 1 step, 2 clip) into it, paste from it */
void rfmv_seq_copy_step(rfm_move_t *m, int clip, int step);
void rfmv_seq_copy_clip(rfm_move_t *m, int clip);
void rfmv_seq_paste_step(rfm_move_t *m, int clip, int step);
void rfmv_seq_paste_clip(rfm_move_t *m, int clip);
void rfmv_seq_track_copy(rfm_move_t *m, int from, int to);
void rfmv_seq_track_clear(rfm_move_t *m, int track);
void rfmv_seq_clip_dup(rfm_move_t *m, int from, int to);   /* Copy alone: the clip duplicated */
/* the transport */
void rfmv_seq_start(rfm_move_t *m);                    /* Play: now, or at the external clock's next bar */
void rfmv_seq_stop(rfm_move_t *m);                     /* everything sounding let go, recordings finished */
void rfmv_seq_count_in(rfm_move_t *m);                 /* Rec from stopped: start a bar from now */
void rfmv_seq_restart(rfm_move_t *m);                  /* Shift + Play: every clip from its start */
void rfmv_seq_release(rfm_move_t *m, int track);       /* the track's sounding notes off (-1 all) */
void rfmv_seq_launch(rfm_move_t *m, int track, int scene);   /* scene -1: stop the track */
void rfmv_seq_launch_scene(rfm_move_t *m, int scene);
void rfmv_seq_mute(rfm_move_t *m, int track, int on);
void rfmv_seq_solo(rfm_move_t *m, int track, int on);
int rfmv_seq_groove_of(const rfm_move_t *m, int track);   /* Global (the project's) or the track's own */
int rfmv_seq_audible(const rfm_move_t *m, int track);  /* not muted, and soloed if any track is */
int rfmv_seq_now_tick(const rfm_move_t *m);            /* the song tick now (rounded to the nearest) */
int rfmv_seq_clip_pos(const rfm_move_t *m, int track); /* the active clip's step now, -1 not playing */
int rfmv_seq_clip_tick(const rfm_move_t *m, int track);   /* .. its tick in the clip now, -1 */
/* live pad notes in Sequencer mode: recording and capture */
void rfmv_seq_pad_on(rfm_move_t *m, int pad, int note, int vel);
void rfmv_seq_pad_off(rfm_move_t *m, int pad, int note);
int rfmv_seq_capture(rfm_move_t *m);                   /* the clip's scene, -1 nothing captured */
void rfmv_seq_tap(rfm_move_t *m, uint32_t now_ms);
void rfmv_seq_sample(rfm_move_t *m);                   /* Record (Capture mode, stopped): arm / done at the nearest bar */
void rfmv_seq_audio_rec(rfm_move_t *m);                /* Sample: the audio loop armed / ended at the next bar */
void rfmv_seq_ui_poll(rfm_move_t *m);
void rfmv_seq_select(rfm_move_t *m, int track);        /* a track selected: its layout and CC numbers in */
void rfmv_seq_enter(rfm_move_t *m);                    /* Sequencer mode entered / left: the settings swapped */
void rfmv_seq_leave(rfm_move_t *m);
void rfmv_seq_sync(rfm_move_t *m);
void rfmv_seq_load_track(rfm_move_t *m);              /* .. and back (the selected track's, or Global's) */                     /* the selected track's layout and CC numbers from the settings */
void rfmv_seq_arp_default(rfm_move_arp_t *a);
int rfmv_seq_arp_rate_of(int i);                       /* the arp's RATE: notes a bar */
int rfmv_seq_arp_on(rfm_move_t *m, int note, int vel); /* the selected track's arp takes a pad note: 1 */
void rfmv_seq_arp_off(rfm_move_t *m, int note);
void rfmv_seq_arp_stop(rfm_move_t *m);                  /* (advance) a step held long enough: its popup */
void rfmv_seq_dirty(rfm_move_t *m);

/* ---- project.c */
void rfmv_proj_open(rfm_move_t *m, int slot);          /* grid: load (current saved first) or a new empty one */
void rfmv_proj_delete(rfm_move_t *m, int slot);
void rfmv_proj_save_now(rfm_move_t *m);                /* PROJECT_SAVE for the current slot */
void rfmv_proj_poll(rfm_move_t *m);                    /* the autosave's debounce (from advance) */
void rfmv_proj_info(const rfm_move_t *m, int slot, rfm_move_proj_info_t *o);   /* the current one: live */

/* ---- seq_ui.c: Sequencer mode */
enum { SEQ_M_SHIFT = 1, SEQ_M_COPY = 2, SEQ_M_DELETE = 4, SEQ_M_MUTE = 8, SEQ_M_LOOP = 16 };
enum { SEQ_MSG_NONE = 0, SEQ_MSG_NOCLIP, SEQ_MSG_FULL, SEQ_MSG_LOADFAIL, SEQ_MSG_CAPTURED, SEQ_MSG_UNDO,
       SEQ_MSG_REDO, SEQ_MSG_COPIED, SEQ_MSG_PASTED, SEQ_MSG_CUT, SEQ_MSG_QUANT, SEQ_MSG_DOUBLE, SEQ_MSG_EXTTAP,
       SEQ_MSG_DUP, SEQ_MSG_CLEARED, SEQ_MSG_CAPCLEAR, SEQ_MSG_CLK_AUTO, SEQ_MSG_CLK_INT, SEQ_MSG_CLK_EXT,
       SEQ_MSG_FULLVEL_ON, SEQ_MSG_FULLVEL_OFF, SEQ_MSG_METRO_ON, SEQ_MSG_METRO_OFF };
void rfmv_seq_ui_release(rfm_move_t *m);
int rfmv_seq_src_step(const rfm_move_t *m, int clip, int abs);   /* the clipboard's source: 1 copied, 2 cut, 0 */
int rfmv_seq_src_clip(const rfm_move_t *m, int clip);
int rfmv_seq_src_page(const rfm_move_t *m, int clip, int pg);   /* Loop mode's page clipboard: 1 copied, 2 cut */
int rfmv_pick_layout(rfm_move_t *m, int p);
int rfmv_seq_len_index(int len);                       /* the TRIG page's lengths: the nearest, the length of */
int rfmv_seq_len_of(int i);
int rfmv_seq_len_inf(int len);
int rfmv_seq_trig_val(const rfm_move_t *m, int clip, int abs, int k);   /* the TRIG page's knob k on a step */            /* a held Track button's pad: layout (top row), scale (row 2) */               /* leaving the mode / release-all: held things let go */
void rfmv_seq_pad(rfm_move_t *m, int p, int vel, uint32_t now);
void rfmv_seq_step(rfm_move_t *m, int s, int down, uint32_t now);
int rfmv_seq_button(rfm_move_t *m, int cc, int down, uint32_t now);   /* 1 taken */
int rfmv_seq_volume(rfm_move_t *m, int d);             /* the volume knob: 1 taken (a step held: velocity) */
void rfmv_seq_knob(rfm_move_t *m, int k, int d, uint32_t now);
void rfmv_seq_jog(rfm_move_t *m, int d, uint32_t now);
void rfmv_shift_icons(const rfm_move_t *m, uint8_t icon[16]);   /* the Shift layer's icon LEDs (0 off) */
void rfmv_seq_leds(const rfm_move_t *m, rfmv_rgb_t c[L_WHITE0], uint8_t white[13]);
void rfmv_seq_screen(const rfm_move_t *m, mr_canvas_t *cv);
void rfmv_seq_track_menu(const rfm_move_t *m, mr_canvas_t *cv);   /* Track 4 held */
void rfmv_seq_msg(rfm_move_t *m, int msg);
const char *rfmv_seq_msg_text(const rfm_move_t *m);    /* the message up now, NULL none */
int rfmv_seq_steps_row(const rfm_move_t *m, mr_canvas_t *cv);   /* track view's 16 steps at the bottom; the page's first step */

#endif
