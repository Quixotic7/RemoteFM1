/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone instrument's DX7: saadet's FM6 (firmware/third_party/saadet-fm6) behind a small API. sa_synth.c is
 * its own unit (fm1.toml o2: -O2); everything else calls these.
 *
 * One part, SA_POLY voices (POLY: a new note takes a free voice round-robin, else the oldest released one, else the
 * oldest held one but the lowest; the same note again retriggers its voice), on FM6's own envelopes. Audio is
 * rendered in blocks of SA_CTL (32) frames; FM6's controls (envelopes, LFO, pitch) run every other block, at MSFA's
 * 64 samples.
 *
 * Context: the note, controller and render calls touch the voices: call them from one context (the render's), or
 * with the render locked out. sa_synth_load_* and sa_synth_edit_vced only publish the voice (FM6's own generation
 * counters): the render takes it at its next block, so the main loop may call them while the render runs. */
#pragma once
#include <stdint.h>
#if __has_include("sa_config.h")
#include "sa_config.h"                   /* the build's options (firmware/tools/gen_sa_config.py: build/gen) */
#endif

#ifndef SA_POLY
#define SA_POLY 8                        /* voices (sa_config.h: 16 in the bench build) */
#endif
#define SA_CTL 32u                       /* frames a block */
#define SA_FS 44100u
#define SA_VCED_LEN 155u                 /* a DX7 voice, unpacked (VCED: OP6 first, 21 bytes each, then the voice) */
#define SA_VMEM_LEN 128u                 /* a DX7 voice, packed (VMEM: one of a 32-voice bank's records) */

/* the DX7's function settings (saadet fm6_core.c FN_*; Dexed's defaults at sa_synth_init): bend range up and down
 * (0..12 semitones), bend step (0 smooth, else 12 / step semitones), portamento mode (1: always; else while CC 65 is
 * down), its time (0..127, CC 5), glissando, then range (0..99) and target (bit 0 pitch, 1 amplitude, 2 EG bias) of
 * the wheel, the foot, breath and aftertouch, velocity scaled to the DX7's range (0 / 1) */
enum { SA_FN_PBUP, SA_FN_PBDN, SA_FN_PBSTEP, SA_FN_PMODE, SA_FN_PTIME, SA_FN_GLISS, SA_FN_MWR, SA_FN_MWA, SA_FN_FCR,
       SA_FN_FCA, SA_FN_BCR, SA_FN_BCA, SA_FN_ATR, SA_FN_ATA, SA_FN_VNORM, SA_NFN };

void sa_synth_init(void);                              /* the tables, the voices silent, the INIT voice, Dexed's
                                                        * function settings */
void sa_synth_load_vced(const uint8_t *v);             /* a new voice (155 bytes): the notes stop, as on a DX7
                                                        * program change */
void sa_synth_load_vmem(const uint8_t *b);             /* the same from a packed record (128 bytes) */
void sa_synth_edit_vced(const uint8_t *v);             /* an edit of the voice: the sounding notes follow it */
void sa_synth_voice(uint8_t *v);                       /* the voice now (155 bytes, sanitized) */
void sa_synth_vced_of_vmem(const uint8_t *b, uint8_t *v);   /* unpack a record (VMEM 128 -> VCED 155) */
const uint8_t *sa_synth_init_vmem(void);               /* the DX7's INIT VOICE, packed (128 bytes) */
void sa_synth_function(uint32_t fn, int32_t value);    /* SA_FN_*, clamped to its range */

void sa_synth_note_on(uint32_t note, uint32_t vel);    /* vel 0: a note off */
void sa_synth_note_off(uint32_t note);
void sa_synth_bend(int32_t bend);                      /* -8192..8191 (the 14-bit value - 8192) */
void sa_synth_cc(uint32_t cc, uint32_t value);         /* 1 wheel, 2 breath, 4 foot, 5 portamento time,
                                                        * 64 sustain, 65 portamento, 120 all sound off,
                                                        * 121 reset controllers, 123 all notes off */
void sa_synth_aftertouch(uint32_t value);              /* channel pressure 0..127 */
void sa_synth_all_notes_off(void);                     /* every key released (the voices ring out), pedal up */
void sa_synth_all_sound_off(void);                     /* silent now: every voice fades out in one block */

void sa_synth_render_mono(int32_t *out, uint32_t n);   /* n frames (a multiple of SA_CTL), 24-bit scale (Q23):
                                                        * FM6's sum after its DC filter, before any gain */
void sa_synth_render(int32_t *lr, uint32_t n, uint32_t gain_q12);   /* fm1-base's app_render: n stereo frames,
                                                        * interleaved, 24-bit, x gain / 4096, clipped */
uint32_t sa_synth_voices(void);                        /* voices sounding (held or ringing out) */

/* two cores: sa_synth_cores(2) renders FM6's voices as saadet's batch (each voice a job: its controls and operators,
 * into a buffer of its own), about half of them handed to the worker, which runs fn(arg) elsewhere (fm1-base's
 * plat_cpu2_run: 1 = taken, 0 = do it yourself) while the render does the rest, then waits for it (plat_cpu2_wait:
 * 0 = done, -1 = dropped: done here). The same samples either way (tests/fm6_parity.sh). sa_synth_cores(1), the
 * default: every voice at once, on the render's core. The worker's counts: blocks it took, did not take, dropped */
void sa_synth_cores(uint32_t n);
void sa_synth_worker(int (*run)(void (*fn)(uint32_t), uint32_t arg), int (*wait)(void));
void sa_synth_worker_stats(uint32_t *handed, uint32_t *kept, uint32_t *dropped);
