/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's Sequencer mode (Track 4): the Move's controls on the sequencer, its LEDs and its screen. The data and
 * the engine are seq.c's; host/src/move/README.md, "Sequencer", has the gestures and the decisions. */
#include <stdio.h>
#include <string.h>

#include "move_int.h"

#define clampi rfmv_clampi
#define sc rfmv_sc
typedef rfmv_rgb_t rgb_t;

#define LAST_CC 3                                       /* (move.c's last-control kind for a CC knob) */

/* the tracks' colours */
static const rgb_t TRACK_RGB[RFM_MOVE_SEQ_TRACKS] = {
    { 255, 42, 42 },  { 255, 128, 0 },  { 255, 208, 0 },   { 200, 255, 42 }, { 42, 255, 80 },   { 0, 255, 190 },
    { 26, 228, 255 }, { 40, 120, 255 }, { 120, 60, 255 },  { 196, 58, 255 }, { 255, 58, 200 },  { 255, 110, 140 },
    { 255, 170, 110 }, { 180, 255, 160 }, { 140, 220, 255 }, { 220, 200, 255 },
};
static const char *const CLOCK_NAMES[3] = { "AUTO", "INT", "EXT" };

static rfm_move_seq_t *Q(rfm_move_t *m) { return &m->seq; }
static int edit_clip(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    return SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
}
static int clip_len(const rfm_move_t *m) { return m->seq.len[edit_clip(m)]; }
static int vel_of(const rfm_move_t *m, int v) { return m->seq.fixed_vel ? 127 : clampi(v, 1, 127); }

void rfmv_seq_msg(rfm_move_t *m, int msg)
{
    m->seq.msg = (uint8_t)msg;
    m->seq.msg_ms = (uint32_t)(m->seq.now_us / 1000u);
}

void rfmv_seq_ui_release(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    int p;
    for (p = 0; p < RFM_MOVE_NPADS; p++)
        if (q->recn[p].on)
            rfmv_seq_pad_off(m, p, q->recn[p].note);
    q->mods = q->mods_used = 0, q->copy_src = 0;
    q->step_held = q->step_add = q->step_edit = 0, q->step_first = -1;
}

/* the edited clip has notes but the track plays nothing: it plays (at once when stopped, else at the next bar) */
static void ensure_active(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    if (t->active < 0 && t->queued == SEQ_NONE && rfmv_seq_clip_used(m, edit_clip(m)))
        rfmv_seq_launch(m, q->sel_track, t->sel);
}

/* ================================================================ pads === */
void rfmv_seq_pad(rfm_move_t *m, int p, int vel, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_hold_t *h = &m->pad[p];
    (void)now;
    if (!vel) {
        if (h->kind == H_NOTE)
            rfmv_seq_pad_off(m, p, h->b);
        return;
    }
    h->kind = H_MENU;
    if (q->projects) {                                  /* the grid: top-left = project 1 */
        const int slot = (3 - p / 8) * 8 + p % 8;
        if (q->mods & SEQ_M_DELETE) {
            q->mods_used |= SEQ_M_DELETE;
            rfmv_proj_delete(m, slot);
        } else
            rfmv_proj_open(m, slot);
        return;
    }
    if (q->view) {                                      /* session: 8 tracks x 4 scenes */
        const int track = q->track_page * 8 + p % 8, scene = q->scene_off + (3 - p / 8);
        const int clip = SEQ_CLIP(track, scene);
        rfm_move_track_t *t = &q->tr[track];
        if (q->mods & SEQ_M_SHIFT) {
            q->mods_used |= SEQ_M_SHIFT;
            q->sel_track = (uint8_t)track, t->sel = (int8_t)scene;
        } else if (q->mods & SEQ_M_MUTE) {
            q->mods_used |= SEQ_M_MUTE;
            rfmv_seq_mute(m, track, !t->mute);
        } else if (q->mods & SEQ_M_COPY) {
            q->mods_used |= SEQ_M_COPY;
            if (q->copy_src || (q->cut && q->clip_kind == 2)) {
                rfmv_seq_group(m);
                rfmv_seq_paste_clip(m, clip);
                q->cut = 0;
                rfmv_seq_msg(m, SEQ_MSG_PASTED);
            } else {
                rfmv_seq_copy_clip(m, clip);
                q->copy_src = 1;
                rfmv_seq_msg(m, SEQ_MSG_COPIED);
            }
        } else if (q->mods & SEQ_M_DELETE) {
            q->mods_used |= SEQ_M_DELETE;
            rfmv_seq_group(m);
            rfmv_seq_copy_clip(m, clip);
            q->cut = 1;
            if (t->active == scene)
                rfmv_seq_release(m, track);
            rfmv_seq_clip_clear(m, clip);
            rfmv_seq_msg(m, SEQ_MSG_CUT);
        } else {
            const int running = q->playing || q->start_pending;
            q->sel_track = (uint8_t)track;
            t->sel = (int8_t)scene;
            if (t->active == scene && t->queued == SEQ_NONE && (running || rfmv_seq_clip_used(m, clip)) &&
                !(t->grow == 1)) {
                if (running)
                    rfmv_seq_launch(m, track, -1);      /* a playing clip: stop at the next bar */
            } else if (t->grow == 1 && t->active == scene)
                t->grow_stop = 1;                       /* a recording clip: done at the next bar */
            else if (!rfmv_seq_clip_used(m, clip)) {
                if (q->rec) {                           /* armed: record into it */
                    if (!running) {
                        rfmv_seq_group(m);
                        q->rec_group = q->group;
                        rfmv_seq_start(m);
                    }
                    t->grow = 2;                        /* (starts with the launch, at the bar) */
                    rfmv_seq_launch(m, track, scene);
                }
            } else
                rfmv_seq_launch(m, track, scene);
            rfmv_seq_dirty(m);
        }
        return;
    }
    /* track view: the MIDI mode's layout on the selected track's channel */
    {
        const int note = rfm_move_pad_note(&m->s, p), ch = q->tr[q->sel_track].ch, clip = edit_clip(m);
        int s;
        if (note < 0)
            return;
        if (q->mods & SEQ_M_DELETE) {
            q->mods_used |= SEQ_M_DELETE;
            rfmv_seq_group(m);
            rfmv_seq_note_remove_all(m, clip, note);
            return;
        }
        vel = vel_of(m, vel);
        q->last_vel = (uint8_t)vel, q->cur_note = (uint8_t)note;
        h->kind = H_NOTE, h->a = (uint8_t)ch, h->b = (uint8_t)note;
        rfmv_note_hold(m, ch, note, vel);
        if (m->s.layout == RFM_MOVE_LAYOUT_DRUM)
            m->edit_pad = (int8_t)p;
        if (q->step_held) {                             /* held steps: the note in or out of them */
            for (s = 0; s < RFM_MOVE_NSTEPS; s++)
                if (q->step_held >> s & 1) {
                    const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
                    if (rfmv_seq_find(m, clip, abs, note) >= 0)
                        rfmv_seq_note_del(m, clip, abs, note);
                    else
                        rfmv_seq_note_put(m, clip, abs, note, vel, RFM_MOVE_SEQ_TPS, 0);
                    q->step_edit |= (uint16_t)(1u << s);
                }
            ensure_active(m);
            return;                                     /* (it sounds while held; nothing recorded) */
        }
        rfmv_seq_pad_on(m, p, note, vel);
    }
}

/* ================================================================ steps === */
static int step_has(const rfm_move_t *m, int clip, int abs, int note) { return rfmv_seq_find(m, clip, abs, note) >= 0; }

void rfmv_seq_step(rfm_move_t *m, int s, int down, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    const uint16_t bit = (uint16_t)(1u << s);
    const int clip = edit_clip(m), abs = q->page * RFM_MOVE_SEQ_PAGE + s;
    (void)now;
    if (!down) {
        if (q->step_held & bit) {
            if (!((q->step_add | q->step_edit) & bit) && abs < clip_len(m)) {   /* a tap: the note toggles */
                if (step_has(m, clip, abs, q->cur_note))
                    rfmv_seq_note_del(m, clip, abs, q->cur_note);
                else {
                    rfmv_seq_note_put(m, clip, abs, q->cur_note, vel_of(m, q->last_vel), RFM_MOVE_SEQ_TPS, 0);
                    ensure_active(m);
                }
            }
            q->step_held &= (uint16_t)~bit, q->step_add &= (uint16_t)~bit, q->step_edit &= (uint16_t)~bit;
            if (q->step_first == s) {
                int i;
                q->step_first = -1;
                for (i = 0; i < RFM_MOVE_NSTEPS && q->step_first < 0; i++)
                    if (q->step_held >> i & 1)
                        q->step_first = (int8_t)i;
            }
        }
        return;
    }
    if (q->projects) {
        if ((q->mods & SEQ_M_SHIFT) && s == 0)
            q->projects = 0, q->mods_used |= SEQ_M_SHIFT;
        return;
    }
    if (rfmv_track_held(m, 3)) {                        /* Track 4 held: steps are the tracks */
        if (q->mods & SEQ_M_COPY) {
            q->mods_used |= SEQ_M_COPY;
            if (!q->copy_src || q->clip_kind != 3) {
                q->clip_kind = 3, q->clip_src = (uint8_t)s, q->copy_src = 1, q->cut = 0;
                rfmv_seq_msg(m, SEQ_MSG_COPIED);
            } else {
                rfmv_seq_group(m);
                rfmv_seq_track_copy(m, q->clip_src, s);
                q->clip_kind = 3;                       /* (paste again elsewhere while Copy is held) */
                rfmv_seq_msg(m, SEQ_MSG_PASTED);
            }
        } else if (q->mods & SEQ_M_DELETE) {
            q->mods_used |= SEQ_M_DELETE;
            rfmv_seq_group(m);
            rfmv_seq_track_clear(m, s);
        } else if (q->mods & SEQ_M_MUTE) {
            q->mods_used |= SEQ_M_MUTE;
            rfmv_seq_mute(m, s, !q->tr[s].mute);
        } else if (q->sel_track != s) {
            q->sel_track = (uint8_t)s, q->page = 0;
            q->track_page = (uint8_t)(s / 8);
            rfmv_seq_dirty(m);
        }
        return;
    }
    if (q->mods & SEQ_M_SHIFT) {                        /* the shortcuts */
        q->mods_used |= SEQ_M_SHIFT;
        switch (s) {
        case 0: q->projects = 1; break;
        case 1: q->fixed_vel = (uint8_t)!q->fixed_vel, rfmv_seq_dirty(m); break;
        case 2:
            rfmv_seq_group(m);
            rfmv_seq_clip_quantize(m, clip);
            rfmv_seq_msg(m, SEQ_MSG_QUANT);
            break;
        case 3:
            rfmv_seq_group(m);
            if (rfmv_seq_clip_double(m, clip) == 0)
                rfmv_seq_msg(m, SEQ_MSG_DOUBLE);
            break;
        case 4: rfm_move_seq_set_clock(m, (q->clock_mode + 1) % 3); break;
        default: break;
        }
        return;
    }
    if (q->mods & SEQ_M_MUTE) {                         /* Mute + step: track 1..16 */
        q->mods_used |= SEQ_M_MUTE;
        rfmv_seq_mute(m, s, !q->tr[s].mute);
        return;
    }
    if (q->mods & SEQ_M_LOOP) {                         /* Loop + step: the clip's length in pages */
        q->mods_used |= SEQ_M_LOOP;
        rfmv_seq_group(m);
        rfmv_seq_len_set(m, clip, (s + 1) * RFM_MOVE_SEQ_PAGE);
        if (q->page > s)
            q->page = (uint8_t)s;
        return;
    }
    if (q->view) {                                      /* session: step n launches scene n */
        rfmv_seq_launch_scene(m, s);
        rfmv_seq_dirty(m);
        return;
    }
    if (abs >= clip_len(m))
        return;
    if (q->mods & SEQ_M_COPY) {
        q->mods_used |= SEQ_M_COPY;
        if (q->copy_src || (q->cut && q->clip_kind == 1)) {
            rfmv_seq_group(m);
            rfmv_seq_paste_step(m, clip, abs);
            q->cut = 0;
            rfmv_seq_msg(m, SEQ_MSG_PASTED);
        } else {
            rfmv_seq_copy_step(m, clip, abs);
            q->copy_src = 1;
            rfmv_seq_msg(m, SEQ_MSG_COPIED);
        }
        return;
    }
    if (q->mods & SEQ_M_DELETE) {
        q->mods_used |= SEQ_M_DELETE;
        rfmv_seq_group(m);
        rfmv_seq_copy_step(m, clip, abs);
        q->cut = 1;
        rfmv_seq_step_clear(m, clip, abs);
        rfmv_seq_msg(m, SEQ_MSG_CUT);
        return;
    }
    if (q->step_first >= 0 && !(q->step_held & bit)) { /* a second step: the first's notes reach to it */
        const int first = q->page * RFM_MOVE_SEQ_PAGE + q->step_first;
        if (abs > first) {
            int lo, hi = rfmv_seq_range(m, clip, first, &lo), i;
            for (i = lo; i < hi; i++) {
                const rfm_move_note_t n = m->seq.notes[i];
                rfmv_seq_note_put(m, clip, n.step, n.note, n.vel, (abs - first + 1) * RFM_MOVE_SEQ_TPS, n.micro);
            }
            q->step_edit |= (uint16_t)(1u << q->step_first);
        }
        return;
    }
    if (!q->step_held)
        rfmv_seq_group(m);                              /* one hold, one undo */
    q->step_held |= bit;
    if (q->step_first < 0)
        q->step_first = (int8_t)s;
    {
        int lo, hi = rfmv_seq_range(m, clip, abs, &lo);
        if (hi == lo && rfmv_seq_note_put(m, clip, abs, q->cur_note, vel_of(m, q->last_vel), RFM_MOVE_SEQ_TPS, 0) == 0) {
            q->step_add |= bit;                         /* an empty step: the note goes in at once */
            ensure_active(m);
        }
    }
}

/* ================================================================ buttons, knobs, jog === */
static int mod_bit(int cc)
{
    switch (cc) {
    case MR_CC_SHIFT: return SEQ_M_SHIFT;
    case MR_CC_COPY: return SEQ_M_COPY;
    case MR_CC_DELETE: return SEQ_M_DELETE;
    case MR_CC_MUTE: return SEQ_M_MUTE;
    case MR_CC_LOOP: return SEQ_M_LOOP;
    default: return 0;
    }
}

int rfmv_seq_button(rfm_move_t *m, int cc, int down, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    const int mb = mod_bit(cc);
    if (mb) {
        if (down) {
            if (cc == MR_CC_LOOP && (q->mods & SEQ_M_SHIFT)) {   /* Shift + Loop: double the loop */
                q->mods_used |= SEQ_M_SHIFT;
                rfmv_seq_group(m);
                if (rfmv_seq_clip_double(m, edit_clip(m)) == 0)
                    rfmv_seq_msg(m, SEQ_MSG_DOUBLE);
                return 1;
            }
            q->mods |= (uint8_t)mb, q->mods_used &= (uint8_t)~mb;
            if (mb == SEQ_M_COPY)
                q->copy_src = 0;
        } else {
            if (mb == SEQ_M_MUTE && (q->mods & SEQ_M_MUTE) && !(q->mods_used & SEQ_M_MUTE))
                rfmv_seq_mute(m, q->sel_track, !q->tr[q->sel_track].mute);   /* Mute alone: the selected track */
            q->mods &= (uint8_t)~mb;
            if (mb == SEQ_M_COPY)
                q->copy_src = 0;
        }
        return 1;
    }
    if (!down)
        return 1;
    switch (cc) {
    case MR_CC_MENU:
        if (q->projects)
            q->projects = 0;
        else
            q->view = (uint8_t)!q->view;
        return 1;
    case MR_CC_BACK:
        if (q->projects)
            q->projects = 0;
        else
            rfm_move_set_mode(m, RFM_MOVE_MODE_FM1);    /* back to FM-1 mode; the sequencer keeps playing */
        return 1;
    case MR_CC_PLAY:
        if (q->mods & SEQ_M_SHIFT) {                    /* Shift + Play: every clip stops (at the next bar) */
            int t;
            q->mods_used |= SEQ_M_SHIFT;
            for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
                if (q->tr[t].active >= 0)
                    rfmv_seq_launch(m, t, -1);
        } else if (q->playing || q->start_pending)
            rfmv_seq_stop(m);
        else
            rfmv_seq_start(m);
        return 1;
    case MR_CC_REC:
        if (!q->rec) {
            q->rec = 1;
            rfmv_seq_group(m);
            q->rec_group = q->group;
            if (!q->playing && !q->start_pending)
                rfmv_seq_start(m);
        } else {
            int t;
            q->rec = 0;
            for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
                if (q->tr[t].grow == 1)
                    q->tr[t].grow_stop = 1;
        }
        return 1;
    case MR_CC_CAPTURE:
        if (q->mods & SEQ_M_SHIFT) {                    /* Shift + Capture: quantize the clip */
            q->mods_used |= SEQ_M_SHIFT;
            rfmv_seq_group(m);
            rfmv_seq_clip_quantize(m, edit_clip(m));
            rfmv_seq_msg(m, SEQ_MSG_QUANT);
        } else
            rfmv_seq_msg(m, rfmv_seq_capture(m) >= 0 ? SEQ_MSG_CAPTURED : SEQ_MSG_NOCLIP);
        return 1;
    case MR_CC_SAMPLE: rfmv_seq_tap(m, now); return 1;
    case MR_CC_UNDO:
        if (q->mods & SEQ_M_SHIFT) {
            q->mods_used |= SEQ_M_SHIFT;
            if (rfmv_seq_redo(m))
                rfmv_seq_msg(m, SEQ_MSG_REDO);
        } else if (rfmv_seq_undo(m))
            rfmv_seq_msg(m, SEQ_MSG_UNDO);
        return 1;
    case MR_CC_LEFT:
    case MR_CC_RIGHT: {
        const int d = cc == MR_CC_RIGHT ? 1 : -1;
        if (q->view)
            q->track_page = (uint8_t)clampi(q->track_page + d, 0, 1);
        else
            q->page = (uint8_t)clampi(q->page + d, 0, (clip_len(m) - 1) / RFM_MOVE_SEQ_PAGE);
        return 1;
    }
    case MR_CC_UP:
    case MR_CC_DOWN: {
        const int d = cc == MR_CC_UP ? 1 : -1;
        if (q->view)
            q->scene_off = (uint8_t)clampi(q->scene_off - d, 0, RFM_MOVE_SEQ_SCENES - 4);
        else {
            const int o = clampi(m->s.octave + d, RFM_MOVE_OCT_MIN, RFM_MOVE_OCT_MAX);
            if (o != m->s.octave) {
                m->s.octave = (int8_t)o;
                rfmv_changed(m);
            }
        }
        return 1;
    }
    default: return 0;
    }
}

void rfmv_seq_knob(rfm_move_t *m, int k, int d, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    const int b = m->s.bank, cc = m->s.cc[b][k];
    m->last_kind = LAST_CC, m->last_index = (uint8_t)k, m->last_ms = now;
    if (!q->view && q->step_held) {                     /* held steps: a CC lock (Delete held: removed) */
        const int clip = edit_clip(m);
        int s;
        for (s = 0; s < RFM_MOVE_NSTEPS; s++)
            if (q->step_held >> s & 1) {
                const int abs = q->page * RFM_MOVE_SEQ_PAGE + s, old = rfmv_seq_lock_get(m, clip, abs, cc);
                const int v = clampi((old >= 0 ? old : m->cc_val[b][k]) + d, 0, 127);
                rfmv_seq_lock_set(m, clip, abs, cc, (q->mods & SEQ_M_DELETE) ? -1 : v);
                q->step_edit |= (uint16_t)(1u << s);
            }
        if (q->mods & SEQ_M_DELETE)
            q->mods_used |= SEQ_M_DELETE;
        return;
    }
    {
        const int v = clampi(m->cc_val[b][k] + d, 0, 127);
        if (v != m->cc_val[b][k]) {
            m->cc_val[b][k] = (uint8_t)v;
            rfmv_emit_midi(m, 0xB0 | q->tr[q->sel_track].ch, cc, v);
        }
        m->last_value = (int16_t)m->cc_val[b][k];
    }
}

void rfmv_seq_jog(rfm_move_t *m, int d, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    (void)now;
    if (rfmv_track_held(m, 3)) {                        /* Track 4 held: the selected track's channel */
        rfm_move_track_t *t = &q->tr[q->sel_track];
        const int ch = clampi(t->ch + d, 0, 15);
        if (ch != t->ch) {
            rfmv_seq_release(m, q->sel_track);
            t->ch = (uint8_t)ch;
            rfmv_seq_dirty(m);
        }
        return;
    }
    if (!q->view && q->step_held) {                     /* held steps: the notes' length (Shift: a 6th of a step) */
        const int clip = edit_clip(m), unit = (q->mods & SEQ_M_SHIFT) ? RFM_MOVE_SEQ_TPS / 6 : RFM_MOVE_SEQ_TPS;
        int s, lo, hi, i;
        if (q->mods & SEQ_M_SHIFT)
            q->mods_used |= SEQ_M_SHIFT;
        for (s = 0; s < RFM_MOVE_NSTEPS; s++)
            if (q->step_held >> s & 1) {
                const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
                hi = rfmv_seq_range(m, clip, abs, &lo);
                for (i = lo; i < hi; i++) {
                    const rfm_move_note_t n = q->notes[i];
                    const int len = unit == RFM_MOVE_SEQ_TPS ? (n.len / RFM_MOVE_SEQ_TPS + d) * RFM_MOVE_SEQ_TPS
                                                             : n.len + d * unit;
                    rfmv_seq_note_put(m, clip, n.step, n.note, n.vel, clampi(len, unit, clip_len(m) * RFM_MOVE_SEQ_TPS),
                                      n.micro);
                }
                q->step_edit |= (uint16_t)(1u << s);
            }
        return;
    }
    if (q->mods & SEQ_M_SHIFT) {                        /* Shift + jog: the tempo, 1 BPM a detent */
        q->mods_used |= SEQ_M_SHIFT;
        if (q->following)
            rfmv_seq_msg(m, SEQ_MSG_EXTTAP);
        else
            rfm_move_seq_set_bpm(m, q->bpm10 + d * 10);
        return;
    }
    if (m->s.layout == RFM_MOVE_LAYOUT_DRUM && m->edit_pad >= 0 && m->pad_down[m->edit_pad] && !q->view) {
        const int p = m->edit_pad, n = clampi(m->s.drum[p] + d, 0, 127);
        if (n != m->s.drum[p]) {
            m->s.drum[p] = (uint8_t)n;
            rfmv_changed(m);
        }
        return;
    }
    {
        const int b = clampi(m->s.bank + d, 0, RFM_MOVE_NBANKS - 1);
        if (b != m->s.bank) {
            m->s.bank = (uint8_t)b;
            rfmv_changed(m);
        }
    }
}

/* ================================================================ LEDs === */
static int blink(const rfm_move_seq_t *q) { return q->tl >= 0 && (q->tl / (RFM_MOVE_SEQ_TPS * 2)) & 1; }   /* 8ths */
static int pulse(const rfm_move_seq_t *q)                                  /* a beat: bright, fading */
{
    return q->playing && q->T >= 0 ? 256 - (int)(q->T % 96) * 160 / 96 : 256;
}

static int scene_state(const rfm_move_t *m, int scene, int *used)   /* 2 playing, 1 queued, 0 */
{
    const rfm_move_seq_t *q = &m->seq;
    int t, st = 0;
    *used = 0;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++) {
        if (rfmv_seq_clip_used(m, SEQ_CLIP(t, scene)))
            *used = 1;
        if (q->tr[t].queued == scene)
            st = st ? st : 1;
        if (q->tr[t].active == scene && q->playing && rfmv_seq_clip_used(m, SEQ_CLIP(t, scene)))
            st = 2;
    }
    return st;
}

void rfmv_seq_leds(const rfm_move_t *m, rgb_t c[L_WHITE0], uint8_t w[13])
{
    const rfm_move_seq_t *q = &m->seq;
    const rgb_t mode = rfmv_mode_rgb[RFM_MOVE_MODE_SEQ], tc = TRACK_RGB[q->sel_track];
    const int clip = edit_clip(m), len = q->len[clip], bl = blink(q), pu = pulse(q);
    const int t4 = rfmv_track_held(m, 3);
    int i;
    /* pads */
    for (i = 0; i < RFM_MOVE_NPADS; i++) {
        rgb_t v = RFMV_OFF;
        if (q->projects) {
            const int slot = (3 - i / 8) * 8 + i % 8;
            if (slot == q->proj_pending)
                v = bl ? mode : RFMV_OFF;
            else if (slot == q->proj_cur)
                v = mode;
            else if (q->proj_used >> slot & 1)
                v = sc(RFMV_WHITE, 72);
            else
                v = sc(RFMV_WHITE, 8);
        } else if (q->view) {
            const int track = q->track_page * 8 + i % 8, scene = q->scene_off + (3 - i / 8);
            const rfm_move_track_t *t = &q->tr[track];
            const int used = rfmv_seq_clip_used(m, SEQ_CLIP(track, scene)), col = t->mute ? 96 : 256;
            if (t->active == scene && t->grow)
                v = sc(RFMV_RED, t->grow == 1 ? pu : (bl ? 256 : 40));
            else if (t->queued == scene || (t->queued == SEQ_STOP && t->active == scene))
                v = sc(TRACK_RGB[track], bl ? col : 24);
            else if (t->active == scene && used)
                v = sc(TRACK_RGB[track], q->playing ? pu * col / 256 : col);
            else if (used)
                v = sc(TRACK_RGB[track], 48);
            else if (track == q->sel_track && t->sel == scene)
                v = sc(RFMV_WHITE, 20);
        } else {
            const int note = rfm_move_pad_note(&m->s, i);
            v = rfmv_midi_pad(m, i, mode);
            if (note >= 0 && !m->pad_down[i]) {
                if (q->step_first >= 0) {
                    if (rfmv_seq_find(m, clip, q->page * RFM_MOVE_SEQ_PAGE + q->step_first, note) >= 0)
                        v = tc;
                } else if (note == q->cur_note)
                    v = sc(tc, 200);
            }
        }
        c[L_PAD0 + i] = v;
    }
    /* steps */
    for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
        rgb_t v = RFMV_OFF;
        if (q->projects)
            v = i == 0 ? sc(RFMV_WHITE, 96) : RFMV_OFF;
        else if (t4 || (q->mods & SEQ_M_MUTE)) {        /* the tracks */
            const int used = rfmv_seq_track_used(m, i);
            if (q->tr[i].mute)
                v = sc(RFMV_RED, t4 && i == q->sel_track ? 256 : 48);
            else if (i == q->sel_track)
                v = TRACK_RGB[i];
            else
                v = sc(TRACK_RGB[i], used ? 64 : 10);
        } else if (q->mods & SEQ_M_LOOP) {              /* the pages */
            const int pages = (len + RFM_MOVE_SEQ_PAGE - 1) / RFM_MOVE_SEQ_PAGE;
            v = i == q->page ? tc : i < pages ? sc(tc, 64) : RFMV_OFF;
        } else if (q->mods & SEQ_M_SHIFT) {             /* the shortcuts */
            static const uint8_t lit[5] = { 96, 96, 96, 96, 96 };
            v = i < 5 ? sc(RFMV_WHITE, lit[i]) : RFMV_OFF;
            if (i == 1 && q->fixed_vel)
                v = RFMV_WHITE;
            if (i == 4 && q->following)
                v = RFMV_GREEN;
        } else if (q->view) {                           /* the scenes */
            int used;
            const int st = scene_state(m, i, &used);
            v = st == 2 ? sc(RFMV_GREEN, pu) : st == 1 ? sc(RFMV_GREEN, bl ? 256 : 24) : used ? sc(RFMV_WHITE, 48) : RFMV_OFF;
        } else {
            const int abs = q->page * RFM_MOVE_SEQ_PAGE + i;
            if (abs < len) {
                int lo, hi = rfmv_seq_range(m, clip, abs, &lo);
                const int pos = q->tr[q->sel_track].active == q->tr[q->sel_track].sel ? rfmv_seq_clip_pos(m, q->sel_track) : -1;
                if (q->step_held >> i & 1)
                    v = RFMV_WHITE;
                else if (pos == abs)
                    v = sc(RFMV_WHITE, 200);
                else if (rfmv_seq_find(m, clip, abs, q->cur_note) >= 0)
                    v = tc;
                else if (hi > lo)
                    v = sc(tc, 56);
                else
                    v = sc(RFMV_WHITE, i % 4 ? 4 : 14);
            }
        }
        c[L_STEP0 + i] = v;
    }
    /* Play, Rec, Sample (the beat) */
    c[L_PLAY] = q->playing ? RFMV_GREEN : q->start_pending ? sc(RFMV_GREEN, bl ? 256 : 24) : sc(RFMV_WHITE, 40);
    c[L_REC] = q->rec ? RFMV_RED : sc(RFMV_RED, 30);
    c[L_SAMPLE] = q->tl >= 0 && q->tl % 96 < 24 ? (q->following ? RFMV_GREEN : RFMV_WHITE) : sc(RFMV_WHITE, 16);
    /* knob rings: the held step's locks, else the CC values */
    for (i = 0; i < RFM_MOVE_NKNOBS; i++) {
        const rgb_t bc = rfmv_bank_rgb[m->s.bank & 3];
        rgb_t v;
        if (!q->view && q->step_first >= 0) {
            const int l = rfmv_seq_lock_get(m, clip, q->page * RFM_MOVE_SEQ_PAGE + q->step_first, m->s.cc[m->s.bank][i]);
            v = l < 0 ? RFMV_OFF : sc(bc, 32 + l * 224 / 127);
        } else
            v = m->touched == i ? bc : sc(bc, 16 + m->cc_val[m->s.bank][i] * 240 / 127);
        c[L_RING0 + i] = v;
    }
    /* the white LEDs */
    w[W_SHIFT] = (q->mods & SEQ_M_SHIFT) ? 127 : 24;
    w[W_MENU] = q->view ? 127 : 40;
    w[W_BACK] = 40;
    w[W_CAPTURE] = q->ncap ? 100 : 16;
    if (q->view) {
        w[W_UP] = q->scene_off > 0 ? 100 : 8;
        w[W_DOWN] = q->scene_off < RFM_MOVE_SEQ_SCENES - 4 ? 100 : 8;
        w[W_LEFT] = q->track_page > 0 ? 100 : 8;
        w[W_RIGHT] = q->track_page < 1 ? 100 : 8;
    } else {
        w[W_UP] = m->s.octave > 0 ? 127 : 24;
        w[W_DOWN] = m->s.octave < 0 ? 127 : 24;
        w[W_LEFT] = q->page > 0 ? 100 : 8;
        w[W_RIGHT] = (q->page + 1) * RFM_MOVE_SEQ_PAGE < len ? 100 : 8;
    }
    w[W_UNDO] = q->un ? 60 : 6;
    w[W_LOOP] = (q->mods & SEQ_M_LOOP) ? 127 : 40;
    w[W_COPY] = (q->mods & SEQ_M_COPY) ? 127 : q->clip_kind && q->cut ? 70 : 30;
    w[W_MUTE] = (q->mods & SEQ_M_MUTE) ? 127 : q->tr[q->sel_track].mute ? 100 : 30;
    w[W_DELETE] = (q->mods & SEQ_M_DELETE) ? 127 : 30;
}

/* ================================================================ the screen === */
static const char *const MSG[] = { "", "NO EMPTY CLIP", "FULL", "LOAD FAILED", "CAPTURED", "UNDO", "REDO", "COPIED",
                                   "PASTED", "CUT", "QUANTIZED", "DOUBLED", "EXT TEMPO" };

static void inv_text(mr_canvas_t *cv, int x, int y, const char *s) { mr_gfx_text(cv, x, y, s, 0); }

static void bbt(const rfm_move_seq_t *q, char *b, int cap)
{
    const int32_t T = q->playing && q->T >= 0 ? q->T : 0;
    snprintf(b, (size_t)cap, "%d.%d.%d", (int)(T / RFM_MOVE_SEQ_BAR + 1) % 1000, (int)(T % RFM_MOVE_SEQ_BAR / 96 + 1),
             (int)(T % 96 / RFM_MOVE_SEQ_TPS + 1));
}

static void projects_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    char b[24];
    int i, n = 0;
    for (i = 0; i < RFM_MOVE_PROJECTS; i++)
        n += q->proj_used >> i & 1;
    rfmv_title(cv, 128, "PROJECTS");
    snprintf(b, sizeof b, "P%02d", q->proj_cur + 1);
    inv_text(cv, 128 - 6 * 3 - 1, 1, b);
    for (i = 0; i < RFM_MOVE_PROJECTS; i++) {           /* the pads, as they are laid out */
        const int x = 4 + (i % 8) * 15, y = 13 + (i / 8) * 12;
        if (i == q->proj_cur || (i == q->proj_pending && blink(q)))
            mr_gfx_rect(cv, x, y, 12, 9, 1, 1);
        else if (q->proj_used >> i & 1) {
            mr_gfx_rect(cv, x, y, 12, 9, 1, 0);
            mr_gfx_rect(cv, x + 4, y + 3, 4, 3, 1, 1);
        } else
            mr_gfx_pixel(cv, x + 6, y + 4, 1);
    }
    (void)n;
}

void rfmv_seq_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    const int clip = edit_clip(m), len = q->len[clip], bpm = rfm_move_seq_bpm10(m);
    const uint32_t now_ms = (uint32_t)(q->now_us / 1000u);
    char b[40], nb[8];
    int i, y;
    if (q->projects) {
        projects_screen(m, cv);
        return;
    }
    rfmv_title(cv, 128, q->view ? "SESSION" : "TRACK");
    snprintf(b, sizeof b, "P%02d", q->proj_cur + 1);
    inv_text(cv, 128 - 6 * 3 - 1, 1, b);
    if (q->playing) {                                   /* a play triangle in the title */
        for (i = 0; i < 4; i++)
            mr_gfx_rect(cv, 70 + i, 1 + i, 1, 7 - 2 * i, 0, 1);
    } else if (q->start_pending && blink(q))
        mr_gfx_rect(cv, 70, 2, 4, 5, 0, 0);
    if (q->rec)
        mr_gfx_rect(cv, 78, 2, 5, 5, 0, 1);
    /* line 1: the track and the clock */
    snprintf(b, sizeof b, "T%02d CH%02d", q->sel_track + 1, t->ch + 1);
    rfmv_text(cv, 0, 11, b, 9);
    snprintf(b, sizeof b, "%3d.%d %s", bpm / 10, bpm % 10, q->following ? "EXT" : "INT");
    rfmv_text(cv, 128 - 6 * (int)strlen(b), 11, b, 21);
    /* line 2: where */
    if (q->view)
        snprintf(b, sizeof b, "SCN %d-%d  TRK %d-%d", q->scene_off + 1, q->scene_off + 4, q->track_page * 8 + 1,
                 q->track_page * 8 + 8);
    else
        snprintf(b, sizeof b, "S%d PG %d/%d %s%s", t->sel + 1, q->page + 1, (len + 15) / 16,
                 rfmv_note_name(q->cur_note, nb, sizeof nb), q->fixed_vel ? " FIX" : "");
    rfmv_text(cv, 0, 20, b, 21);
    /* line 3: the transport */
    bbt(q, nb, sizeof nb);
    snprintf(b, sizeof b, "%s %s%s%s", q->playing ? "PLAY" : q->start_pending ? "WAIT" : "STOP", nb,
             q->rec ? " REC" : "", t->mute ? " MUTE" : "");
    rfmv_text(cv, 0, 29, b, 21);
    /* lines 4, 5: the held step, else a message, else the knob */
    y = 38;
    if (!q->view && q->step_first >= 0) {
        const int abs = q->page * RFM_MOVE_SEQ_PAGE + q->step_first;
        int lo, hi = rfmv_seq_range(m, clip, abs, &lo), n = 0, llo, lhi = rfmv_seq_lock_range(m, clip, abs, &llo);
        int x;
        snprintf(b, sizeof b, "S%d", abs + 1);
        x = mr_gfx_text(cv, 0, y, b, 1) + 6;
        for (i = lo; i < hi && n < 3; i++, n++)
            x = mr_gfx_text(cv, x, y, rfmv_note_name(q->notes[i].note, nb, sizeof nb), 1) + 6;
        if (hi - lo > 3) {
            snprintf(b, sizeof b, "+%d", hi - lo - 3);
            mr_gfx_text(cv, x, y, b, 1);
        }
        if (hi > lo) {
            const rfm_move_note_t *n0 = &q->notes[lo];
            snprintf(b, sizeof b, "L%d.%d V%d", n0->len / RFM_MOVE_SEQ_TPS, n0->len % RFM_MOVE_SEQ_TPS * 10 / RFM_MOVE_SEQ_TPS,
                     n0->vel);
            if (n0->micro)
                snprintf(b + strlen(b), sizeof b - strlen(b), " %+d", n0->micro);
            rfmv_text(cv, 0, y + 9, b, 21);
        } else if (lhi > llo) {
            snprintf(b, sizeof b, "CC%d=%d", q->locks[llo].cc, q->locks[llo].val);
            rfmv_text(cv, 0, y + 9, b, 21);
        }
        if (hi > lo && lhi > llo) {
            snprintf(b, sizeof b, "CC%d=%d", q->locks[llo].cc, q->locks[llo].val);
            rfmv_text(cv, 128 - 6 * (int)strlen(b), y + 9, b, 21);
        }
    } else if (q->mods & SEQ_M_SHIFT) {               /* Shift held: the shortcuts' states */
        snprintf(b, sizeof b, "CLK %s  VEL %s", CLOCK_NAMES[q->clock_mode % 3], q->fixed_vel ? "FIX" : "PAD");
        rfmv_text(cv, 0, y, b, 21);
    } else if (q->msg && now_ms - q->msg_ms < 1500 && q->msg < sizeof MSG / sizeof MSG[0])
        rfmv_text(cv, 0, y, MSG[q->msg], 21);
    else if (q->view) {                                 /* session: the selected clip */
        const int sc_ = t->sel, used = rfmv_seq_clip_used(m, clip);
        const char *st = t->grow && t->active == sc_ ? "REC" : t->queued == sc_ ? "QUEUED"
                         : t->active == sc_ && used ? (q->playing ? "PLAYING" : "ON") : used ? "" : "EMPTY";
        snprintf(b, sizeof b, "T%02d S%d %dST %s", q->sel_track + 1, sc_ + 1, len, st);
        rfmv_text(cv, 0, y, b, 21);
    } else if (m->last_kind == LAST_CC && m->last_index < RFM_MOVE_NKNOBS) {
        snprintf(b, sizeof b, "K%d CC%d %d  BANK %d", m->last_index + 1, m->s.cc[m->s.bank][m->last_index],
                 m->cc_val[m->s.bank][m->last_index], m->s.bank + 1);
        rfmv_text(cv, 0, y, b, 21);
    }
    /* the bar at the bottom: the page's 16 steps (track) or the bar's 16ths (session), the playhead above */
    {
        const int pos = q->view ? (q->playing && q->T >= 0 ? (int)(q->T % RFM_MOVE_SEQ_BAR / RFM_MOVE_SEQ_TPS) : -1)
                                : (t->active == t->sel ? rfmv_seq_clip_pos(m, q->sel_track) - q->page * RFM_MOVE_SEQ_PAGE : -1);
        for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
            const int x = i * 8, abs = q->page * RFM_MOVE_SEQ_PAGE + i;
            int lo, filled = 0;
            if (!q->view && abs >= len)
                continue;
            if (!q->view)
                filled = rfmv_seq_range(m, clip, abs, &lo) > lo;
            mr_gfx_rect(cv, x, 58, 7, 6, 1, filled);
            if (i == pos)
                mr_gfx_rect(cv, x, 55, 7, 2, 1, 1);
        }
    }
}

void rfmv_seq_track_menu(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    char b[24];
    int i;
    rfmv_title(cv, 128, "TRACK 4  SEQ");
    snprintf(b, sizeof b, "T%02d CH%02d%s", q->sel_track + 1, q->tr[q->sel_track].ch + 1,
             q->tr[q->sel_track].mute ? " MUTE" : "");
    rfmv_text(cv, 0, 12, b, 21);
    for (i = 0; i < RFM_MOVE_SEQ_TRACKS; i++) {         /* the 16 tracks, as the steps */
        const int x = (i % 8) * 16, y = 24 + (i / 8) * 14, sel = i == q->sel_track;
        snprintf(b, sizeof b, "%d", i + 1);
        if (sel)
            mr_gfx_rect(cv, x, y, 15, 11, 1, 1);
        else if (rfmv_seq_track_used(m, i))
            mr_gfx_rect(cv, x, y, 15, 11, 1, 0);
        mr_gfx_text(cv, x + (i < 9 ? 5 : 2), y + 2, b, !sel);
        if (q->tr[i].mute)
            mr_gfx_rect(cv, x + 1, y + 5, 13, 1, !sel, 1);
    }
}

void rfmv_seq_badge(const rfm_move_t *m, mr_canvas_t *cv)
{
    int i;
    if (!m->seq.playing)
        return;
    for (i = 0; i < 4; i++)                             /* a small play triangle at the end of the title */
        mr_gfx_rect(cv, 55 + i, 1 + i, 1, 7 - 2 * i, 0, 1);
}
