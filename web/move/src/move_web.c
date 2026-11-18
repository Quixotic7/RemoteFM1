/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move for the browser (host/src/move/README.md, "The web player"): a flat C API over the portable core, compiled
 * with Emscripten (web/move/build.sh). web/site/js/move-core.js is its only caller. Each instance is one malloc'd
 * block: the core and the actions it gave during the last call, which JS reads from mw_acts() after the call:
 *   10 bytes an action: kind, index, down, vel, value (int16 LE), n, bytes[3]
 * and, beside them, each action's frame (uint32, mw_act_frames(): the offset in mw_seq_advance's frames; 0 for
 * every other call). A PROJECT_SAVE action's blob is copied out: value = its index for mw_blob() / mw_blob_len().
 * Nothing here keeps a pointer into JS memory; no filesystem, no sockets (mr_decode comes from mr_client.c, whose
 * socket code the linker leaves out). */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "rfm/move.h"

#define MW_MAX_ACTS 1024
#define MW_ACT_BYTES 10
#define MW_MAX_BLOBS 4                                  /* PROJECT_SAVEs in one call (a project switch makes 2) */

typedef struct {
    rfm_move_t m;
    int nacts;
    uint8_t acts[MW_MAX_ACTS * MW_ACT_BYTES];
    uint32_t frames[MW_MAX_ACTS];
    int nblobs;
    uint32_t blob_len[MW_MAX_BLOBS];
    uint8_t blob[MW_MAX_BLOBS][RFM_MOVE_PROJECT_MAX];
    rfm_move_led_t leds[RFM_MOVE_NLEDS];
    uint8_t screen[MR_SCREEN_BYTES];
    uint8_t settings[RFM_MOVE_SETTINGS_BYTES];
} mw_t;

static void on_act(void *ctx, const rfm_move_act_t *a)
{
    mw_t *w = ctx;
    uint8_t *o;
    int16_t value = a->value;
    if (w->nacts >= MW_MAX_ACTS)
        return;
    if (a->kind == RFM_MOVE_ACT_PROJECT_SAVE) {
        if (w->nblobs >= MW_MAX_BLOBS || !a->data || a->len > RFM_MOVE_PROJECT_MAX)
            return;
        memcpy(w->blob[w->nblobs], a->data, a->len);
        w->blob_len[w->nblobs] = a->len;
        value = (int16_t)w->nblobs++;
    }
    w->frames[w->nacts] = a->frame;
    o = w->acts + w->nacts++ * MW_ACT_BYTES;
    o[0] = a->kind, o[1] = a->index, o[2] = a->down, o[3] = a->vel;
    o[4] = (uint8_t)((uint16_t)value & 0xFF), o[5] = (uint8_t)((uint16_t)value >> 8);
    o[6] = a->n, o[7] = a->bytes[0], o[8] = a->bytes[1], o[9] = a->bytes[2];
}

/* a new core: settings (n bytes, an rfm_move_save image) or NULL / not an image: the defaults */
EMSCRIPTEN_KEEPALIVE mw_t *mw_new(const uint8_t *settings, uint32_t n)
{
    mw_t *w = calloc(1, sizeof *w);
    rfm_move_settings_t s;
    if (!w)
        return NULL;
    if (!settings || rfm_move_load(&s, settings, n) != 0)
        rfm_move_settings_default(&s);
    rfm_move_init(&w->m, &s, on_act, w);
    return w;
}
EMSCRIPTEN_KEEPALIVE void mw_free(mw_t *w) { free(w); }

/* the actions of the last call */
EMSCRIPTEN_KEEPALIVE int mw_nacts(const mw_t *w) { return w->nacts; }
EMSCRIPTEN_KEEPALIVE const uint8_t *mw_acts(const mw_t *w) { return w->acts; }
EMSCRIPTEN_KEEPALIVE const uint32_t *mw_act_frames(const mw_t *w) { return w->frames; }
EMSCRIPTEN_KEEPALIVE const uint8_t *mw_blob(const mw_t *w, int i) { return i >= 0 && i < w->nblobs ? w->blob[i] : NULL; }
EMSCRIPTEN_KEEPALIVE uint32_t mw_blob_len(const mw_t *w, int i) { return i >= 0 && i < w->nblobs ? w->blob_len[i] : 0; }
#define BEGIN(w) ((w)->nacts = 0, (w)->nblobs = 0)

/* one INPUT record: a USB-MIDI packet (4 bytes) and its frame. Returns the number of actions */
EMSCRIPTEN_KEEPALIVE int mw_input(mw_t *w, uint32_t packet, uint32_t frame, uint32_t now_ms)
{
    const uint8_t p[4] = { (uint8_t)packet, (uint8_t)(packet >> 8), (uint8_t)(packet >> 16), (uint8_t)(packet >> 24) };
    mr_event_t ev;
    BEGIN(w);
    mr_decode(p, frame, &ev);
    rfm_move_input(&w->m, &ev, now_ms);
    return w->nacts;
}
/* the decoded event alone (tests): type, index, value into out[3] */
EMSCRIPTEN_KEEPALIVE void mw_decode(uint32_t packet, int32_t *out)
{
    const uint8_t p[4] = { (uint8_t)packet, (uint8_t)(packet >> 8), (uint8_t)(packet >> 16), (uint8_t)(packet >> 24) };
    mr_event_t ev;
    mr_decode(p, 0, &ev);
    out[0] = (int32_t)ev.type, out[1] = ev.index, out[2] = ev.value;
}

EMSCRIPTEN_KEEPALIVE int mw_release_all(mw_t *w)
{
    BEGIN(w);
    rfm_move_release_all(&w->m);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE int mw_set_mode(mw_t *w, int mode)
{
    BEGIN(w);
    rfm_move_set_mode(&w->m, mode);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE int mw_set_scale(mw_t *w, int scale, int root)
{
    BEGIN(w);
    rfm_move_set_scale(&w->m, scale, root);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE void mw_set_master(mw_t *w, int v) { rfm_move_set_master(&w->m, v); }
EMSCRIPTEN_KEEPALIVE int mw_master(const mw_t *w) { return w->m.master; }
EMSCRIPTEN_KEEPALIVE int mw_set_battery(mw_t *w, int battery, int power) { return rfm_move_set_battery(&w->m, battery, power); }
EMSCRIPTEN_KEEPALIVE int mw_battery(const mw_t *w) { return w->m.battery; }
EMSCRIPTEN_KEEPALIVE int mw_sample_levels(mw_t *w, const uint8_t *peaks, int n) { return rfm_move_sample_levels(&w->m, peaks, n); }
EMSCRIPTEN_KEEPALIVE void mw_set_label(mw_t *w, int enc, const char *text) { rfm_move_set_label(&w->m, enc, text); }
/* keymap: 66 int8 (6 rows x 11), or NULL for the FM-1's */
EMSCRIPTEN_KEEPALIVE void mw_set_keymap(mw_t *w, const int8_t *km)
{
    rfm_move_set_keymap(&w->m, km ? (const int8_t(*)[11])km : NULL);
}

EMSCRIPTEN_KEEPALIVE void mw_panel(mw_t *w, const uint8_t *levels44, int play_green)
{
    rfm_move_panel(&w->m, levels44, (uint8_t)play_green);
}
/* the changed LEDs: 6 bytes each (rgb, cc, number, r, g, b) at mw_leds_buf() */
EMSCRIPTEN_KEEPALIVE int mw_leds(mw_t *w) { return rfm_move_leds(&w->m, w->leds, RFM_MOVE_NLEDS); }
EMSCRIPTEN_KEEPALIVE const rfm_move_led_t *mw_leds_buf(const mw_t *w) { return w->leds; }
EMSCRIPTEN_KEEPALIVE void mw_leds_invalidate(mw_t *w) { rfm_move_leds_invalidate(&w->m); }

/* the Move's screen (1024 bytes at mw_screen_buf()); rgb565: 240 x 240, or NULL to keep the last */
EMSCRIPTEN_KEEPALIVE const uint8_t *mw_screen(mw_t *w, const uint16_t *rgb565)
{
    rfm_move_screen(&w->m, rgb565, w->screen);
    return w->screen;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *mw_save(mw_t *w)
{
    rfm_move_save(&w->m.s, w->settings);
    return w->settings;
}
EMSCRIPTEN_KEEPALIVE int mw_settings_bytes(void) { return RFM_MOVE_SETTINGS_BYTES; }

/* the sequencer: the clock (call every audio block, playing or not), the firmware's MIDI clock in, the transport */
EMSCRIPTEN_KEEPALIVE int mw_seq_advance(mw_t *w, uint32_t frames, uint32_t rate)
{
    BEGIN(w);
    rfm_move_seq_advance(&w->m, frames, rate);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE int mw_seq_midi_in(mw_t *w, const uint8_t *msg, int n)
{
    BEGIN(w);
    rfm_move_seq_midi_in(&w->m, msg, n);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE int mw_seq_play(mw_t *w, int on)
{
    BEGIN(w);
    rfm_move_seq_play(&w->m, on);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE void mw_seq_set_bpm(mw_t *w, int bpm10) { rfm_move_seq_set_bpm(&w->m, bpm10); }
EMSCRIPTEN_KEEPALIVE void mw_seq_set_lead(mw_t *w, uint32_t us) { rfm_move_seq_set_lead_us(&w->m, us); }
EMSCRIPTEN_KEEPALIVE int mw_seq_bpm10(const mw_t *w) { return rfm_move_seq_bpm10(&w->m); }
EMSCRIPTEN_KEEPALIVE void mw_seq_set_clock(mw_t *w, int mode) { rfm_move_seq_set_clock(&w->m, mode); }
EMSCRIPTEN_KEEPALIVE int mw_seq_following(const mw_t *w) { return rfm_move_seq_following(&w->m); }
EMSCRIPTEN_KEEPALIVE int mw_seq_playing(const mw_t *w) { return w->m.seq.playing; }
EMSCRIPTEN_KEEPALIVE int mw_screen_timed(const mw_t *w) { return rfm_move_screen_timed(&w->m); }

/* projects: the host's store answers PROJECT_LOAD with mw_project_load; slot used bits; a blob for export */
EMSCRIPTEN_KEEPALIVE int mw_project_load(mw_t *w, int slot, const uint8_t *b, uint32_t n)
{
    int r;
    BEGIN(w);
    r = rfm_move_project_load(&w->m, slot, b, n);
    return r < 0 ? r : w->nacts;
}
EMSCRIPTEN_KEEPALIVE int mw_project_check(const uint8_t *b, uint32_t n) { return rfm_move_project_check(b, n); }
EMSCRIPTEN_KEEPALIVE void mw_project_dir(mw_t *w, uint32_t used) { rfm_move_project_dir(&w->m, used); }
/* a stored project's content for the grid's bars and its popup (rfm_move_project_info) */
EMSCRIPTEN_KEEPALIVE int mw_project_info(mw_t *w, int slot, const uint8_t *b, uint32_t n) { return rfm_move_project_info(&w->m, slot, b, n); }
EMSCRIPTEN_KEEPALIVE int mw_project_flush(mw_t *w)
{
    BEGIN(w);
    rfm_move_project_flush(&w->m);
    return w->nacts;
}
EMSCRIPTEN_KEEPALIVE int mw_project_slot(const mw_t *w) { return rfm_move_project_slot(&w->m); }
/* the current project as a blob (export) into mw_blob(w, 0); its length, or -1 */
EMSCRIPTEN_KEEPALIVE int mw_project_save(mw_t *w)
{
    const int n = rfm_move_project_save(&w->m, w->blob[0], RFM_MOVE_PROJECT_MAX);
    w->nblobs = n > 0, w->blob_len[0] = n > 0 ? (uint32_t)n : 0;
    return n;
}
EMSCRIPTEN_KEEPALIVE int mw_project_max(void) { return RFM_MOVE_PROJECT_MAX; }

/* the state the page shows: mode, layout, channel, octave, scale, root, bank, track_held */
EMSCRIPTEN_KEEPALIVE void mw_state(const mw_t *w, int32_t *out)
{
    out[0] = w->m.s.mode, out[1] = w->m.s.layout, out[2] = w->m.s.channel, out[3] = w->m.s.octave;
    out[4] = w->m.s.scale, out[5] = w->m.s.root, out[6] = w->m.s.bank, out[7] = w->m.track_held;
}

/* the mapping (tests, help) */
EMSCRIPTEN_KEEPALIVE int mw_pad_key(int pad) { return rfm_move_pad_key(pad); }
EMSCRIPTEN_KEEPALIVE int mw_step_key(int step) { return rfm_move_step_key(step); }
EMSCRIPTEN_KEEPALIVE int mw_button_of(int cc) { return rfm_move_button_of(cc); }
EMSCRIPTEN_KEEPALIVE int mw_knob_enc(int knob) { return rfm_move_knob_enc(knob); }
EMSCRIPTEN_KEEPALIVE int mw_pad_note(const mw_t *w, int pad) { return rfm_move_pad_note(&w->m.s, pad); }
