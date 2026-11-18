/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's Sequencer mode (Track 4): the Move's controls on the sequencer, its LEDs and its screen. The data and
 * the engine are seq.c's; host/src/move/README.md, "Sequencer", has the gestures and the decisions (and where they
 * follow Ableton Move's own sequencer). */
#include <stdio.h>
#include <string.h>

#include "move_int.h"

#define clampi rfmv_clampi
#define sc rfmv_sc
typedef rfmv_rgb_t rgb_t;

#define LAST_CC 3                                       /* (move.c's last-control kind for a CC knob) */
#define NUDGE 2                                         /* ticks a Left / Right with a step held (~10 % of a step) */
#define LEN_FINE 2                                      /* ticks a jog detent with a step held (~10 % of a step) */

/* the tracks' colours */
static const rgb_t TRACK_RGB[RFM_MOVE_SEQ_TRACKS] = {
    { 255, 42, 42 },  { 255, 128, 0 },  { 255, 208, 0 },   { 200, 255, 42 }, { 42, 255, 80 },   { 0, 255, 190 },
    { 26, 228, 255 }, { 40, 120, 255 }, { 120, 60, 255 },  { 196, 58, 255 }, { 255, 58, 200 },  { 255, 110, 140 },
    { 255, 170, 110 }, { 180, 255, 160 }, { 140, 220, 255 }, { 220, 200, 255 },
};
static const char *const CLOCK_NAMES[3] = { "AUTO", "INT", "EXT" };
/* Shift + step n (Move's own numbers where Move has the function) */
enum { SH_PROJECTS = 0, SH_CLOCK = 2, SH_FIXVEL = 9, SH_DOUBLE = 14, SH_QUANT = 15 };

static rfm_move_seq_t *Q(rfm_move_t *m) { return &m->seq; }
static int edit_clip(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    return SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
}
static int clip_len(const rfm_move_t *m) { return m->seq.len[edit_clip(m)]; }
static int vel_of(const rfm_move_t *m, int v) { return m->seq.fixed_vel ? 127 : clampi(v, 1, 127); }
static int running(const rfm_move_seq_t *q) { return q->playing || q->start_pending; }
static int root_note(const rfm_move_t *m) { return clampi(48 + m->s.root + 12 * m->s.octave, 0, 127); }

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
    q->mods = q->mods_used = 0, q->copy_src = 0, q->copy_pasted = 0, q->chord = 0;
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
/* notes went into the edited clip: it plays; and (Move) the first note into an empty clip starts the transport */
static void entered(rfm_move_t *m, int was_used)
{
    ensure_active(m);
    if (!was_used && !running(Q(m)) && rfmv_seq_clip_used(m, edit_clip(m)))
        rfmv_seq_start(m);
}

/* step entry's memory: the last notes (a step entered, a held step edited, a chord played), else the root */
static int last_notes(const rfm_move_t *m, uint8_t *note, uint8_t *vel)
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    if (!q->nlast) {
        note[0] = (uint8_t)root_note(m), vel[0] = 100;
        return 1;
    }
    for (i = 0; i < q->nlast; i++)
        note[i] = q->last_note[i], vel[i] = q->last_vel[i];
    return q->nlast;
}
static void last_add(rfm_move_seq_t *q, int note, int vel)
{
    int i;
    for (i = 0; i < q->nlast; i++)
        if (q->last_note[i] == note) {
            q->last_vel[i] = (uint8_t)vel;
            return;
        }
    if (q->nlast < RFM_MOVE_SEQ_POLY)
        q->last_note[q->nlast] = (uint8_t)note, q->last_vel[q->nlast] = (uint8_t)vel, q->nlast++;
}
static void last_from_step(rfm_move_t *m, int clip, int abs)
{
    rfm_move_seq_t *q = Q(m);
    int lo, hi = rfmv_seq_range(m, clip, abs, &lo), i;
    if (hi == lo)
        return;
    q->nlast = 0;
    for (i = lo; i < hi; i++)
        last_add(q, q->notes[i].note, q->notes[i].vel);
}
static void step_notes_clear(rfm_move_t *m, int clip, int abs)   /* the notes only (the locks stay) */
{
    int lo, hi = rfmv_seq_range(m, clip, abs, &lo);
    while (hi-- > lo)
        rfmv_seq_note_del(m, clip, abs, m->seq.notes[hi].note);
}

/* Copy released with nothing pressed: a waiting copy (or a cut) is dropped; else the edited clip is duplicated
 * into the track's next empty slot, which becomes the selected (and, if the source played, the playing) clip */
static void copy_alone(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_track_t *t = &q->tr[q->sel_track];
    const int from = t->sel;
    int to = -1, s;
    if (q->paste || q->cut) {
        q->paste = 0, q->cut = 0, q->clip_kind = 0;
        rfmv_seq_msg(m, SEQ_MSG_CLEARED);
        return;
    }
    if (!rfmv_seq_clip_used(m, edit_clip(m)))
        return;
    for (s = from + 1; s < RFM_MOVE_SEQ_SCENES && to < 0; s++)
        if (!rfmv_seq_clip_used(m, SEQ_CLIP(q->sel_track, s)))
            to = s;
    if (to < 0) {
        rfmv_seq_msg(m, SEQ_MSG_NOCLIP);
        return;
    }
    rfmv_seq_group(m);
    rfmv_seq_clip_dup(m, SEQ_CLIP(q->sel_track, from), SEQ_CLIP(q->sel_track, to));
    if (t->active == from)                              /* (at the next bar; stopped: at once) */
        rfmv_seq_launch(m, q->sel_track, to);
    else
        t->sel = (int8_t)to;
    rfmv_seq_msg(m, SEQ_MSG_DUP);
}
/* Delete released with nothing pressed (Move): the selected track's clip is deleted (cut: Copy + clip pastes it) */
static void delete_alone(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    const int clip = edit_clip(m);
    if (!rfmv_seq_clip_used(m, clip))
        return;
    rfmv_seq_group(m);
    rfmv_seq_copy_clip(m, clip);
    q->cut = 1, q->paste = 0;
    if (q->tr[q->sel_track].active == q->tr[q->sel_track].sel)
        rfmv_seq_release(m, q->sel_track);
    rfmv_seq_clip_clear(m, clip);
    rfmv_seq_msg(m, SEQ_MSG_CUT);
}

/* ================================================================ pads === */
void rfmv_seq_pad(rfm_move_t *m, int p, int vel, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_hold_t *h = &m->pad[p];
    (void)now;
    if (!vel) {
        q->chord &= ~(1u << p);
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
                q->cut = 0, q->copy_pasted = 1;
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
            q->cut = 1, q->paste = 0;
            if (t->active == scene)
                rfmv_seq_release(m, track);
            rfmv_seq_clip_clear(m, clip);
            rfmv_seq_msg(m, SEQ_MSG_CUT);
        } else if (q->paste && q->clip_kind == 2) {     /* a copy waiting (Copy released): pasted here */
            rfmv_seq_group(m);
            rfmv_seq_paste_clip(m, clip);
            q->paste = 0;
            rfmv_seq_msg(m, SEQ_MSG_PASTED);
        } else {
            const int run = running(q), used = rfmv_seq_clip_used(m, clip);
            q->sel_track = (uint8_t)track;
            if (t->grow == 1 && t->active == scene)
                t->grow_stop = 1;                       /* a recording clip: done at the next bar */
            else if (!used) {                           /* an empty slot: selected; the track stops (Move) */
                t->sel = (int8_t)scene;
                if (q->rec) {                           /* armed: record into it */
                    if (!run) {
                        rfmv_seq_group(m);
                        q->rec_group = q->group;
                        rfmv_seq_start(m);
                    }
                    t->grow = 2;                        /* (starts with the launch, at the bar) */
                    rfmv_seq_launch(m, track, scene);
                } else if (t->active >= 0 || t->queued >= 0)
                    rfmv_seq_launch(m, track, -1);
            } else if (t->active == scene && t->queued == SEQ_NONE && run)
                t->sel = (int8_t)scene;                 /* the playing clip: it plays on, selected */
            else {
                rfmv_seq_launch(m, track, scene);       /* at the next bar; stopped: at once, and the transport starts */
                if (!run)
                    rfmv_seq_start(m);
            }
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
        h->kind = H_NOTE, h->a = (uint8_t)ch, h->b = (uint8_t)note;
        rfmv_note_hold(m, ch, note, vel);
        if (m->s.layout == RFM_MOVE_LAYOUT_DRUM)
            m->edit_pad = (int8_t)p;
        if (q->step_held) {                             /* held steps: the note in or out of them */
            const int was = rfmv_seq_clip_used(m, clip);
            for (s = 0; s < RFM_MOVE_NSTEPS; s++)
                if (q->step_held >> s & 1) {
                    const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
                    if (rfmv_seq_find(m, clip, abs, note) >= 0)
                        rfmv_seq_note_del(m, clip, abs, note);
                    else
                        rfmv_seq_note_put(m, clip, abs, note, vel, RFM_MOVE_SEQ_TPS, 0);
                    q->step_edit |= (uint16_t)(1u << s);
                }
            entered(m, was);
            return;                                     /* (it sounds while held; nothing recorded) */
        }
        if (!q->chord)                                  /* a new chord (or note): step entry's memory */
            q->nlast = 0;
        q->chord |= 1u << p, q->pad_vel[p] = (uint8_t)vel;
        last_add(q, note, vel);
        rfmv_seq_pad_on(m, p, note, vel);
    }
}

/* ================================================================ steps === */
void rfmv_seq_step(rfm_move_t *m, int s, int down, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    const uint16_t bit = (uint16_t)(1u << s);
    const int clip = edit_clip(m), abs = q->page * RFM_MOVE_SEQ_PAGE + s;
    (void)now;
    if (!down) {
        if (q->step_held & bit) {
            if (abs < clip_len(m)) {
                if (!((q->step_add | q->step_edit) & bit))
                    step_notes_clear(m, clip, abs);     /* a tap on a step with notes: cleared (Move) */
                else
                    last_from_step(m, clip, abs);       /* entered or edited: its notes are the next empty step's */
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
                q->clip_kind = 3, q->copy_pasted = 1;   /* (paste again elsewhere while Copy is held) */
                rfmv_seq_msg(m, SEQ_MSG_PASTED);
            }
        } else if (q->mods & SEQ_M_DELETE) {
            q->mods_used |= SEQ_M_DELETE;
            rfmv_seq_group(m);
            rfmv_seq_track_clear(m, s);
        } else if (q->mods & SEQ_M_MUTE) {
            q->mods_used |= SEQ_M_MUTE;
            rfmv_seq_mute(m, s, !q->tr[s].mute);
        } else if (q->paste && q->clip_kind == 3) {
            rfmv_seq_group(m);
            rfmv_seq_track_copy(m, q->clip_src, s);
            q->paste = 0, q->clip_kind = 0;
            rfmv_seq_msg(m, SEQ_MSG_PASTED);
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
        case SH_PROJECTS: q->projects = 1; break;
        case SH_FIXVEL: q->fixed_vel = (uint8_t)!q->fixed_vel, rfmv_seq_dirty(m); break;
        case SH_QUANT:
            rfmv_seq_group(m);
            rfmv_seq_clip_quantize(m, clip);
            rfmv_seq_msg(m, SEQ_MSG_QUANT);
            break;
        case SH_DOUBLE:
            rfmv_seq_group(m);
            if (rfmv_seq_clip_double(m, clip) == 0)
                rfmv_seq_msg(m, SEQ_MSG_DOUBLE);
            break;
        case SH_CLOCK: rfm_move_seq_set_clock(m, (q->clock_mode + 1) % 3); break;
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
        if (!(q->mods_used & SEQ_M_LOOP))
            rfmv_seq_group(m);
        q->mods_used |= SEQ_M_LOOP;
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
            q->cut = 0, q->copy_pasted = 1;
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
        q->cut = 1, q->paste = 0;
        rfmv_seq_step_clear(m, clip, abs);
        rfmv_seq_msg(m, SEQ_MSG_CUT);
        return;
    }
    if (q->paste && q->clip_kind == 1 && !q->step_held) {   /* a copy waiting (Copy released): pasted here */
        const int was = rfmv_seq_clip_used(m, clip);
        rfmv_seq_group(m);
        rfmv_seq_paste_step(m, clip, abs);
        q->paste = 0;
        rfmv_seq_msg(m, SEQ_MSG_PASTED);
        entered(m, was);
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
        const int was = rfmv_seq_clip_used(m, clip);
        int lo, hi = rfmv_seq_range(m, clip, abs, &lo), p, i, put = 0;
        if (q->chord) {                                 /* pads held: the step gets exactly those notes */
            step_notes_clear(m, clip, abs);
            q->nlast = 0;
            for (p = 0; p < RFM_MOVE_NPADS; p++)
                if ((q->chord >> p & 1) && m->pad[p].kind == H_NOTE) {
                    const int v = vel_of(m, q->pad_vel[p]);
                    put |= rfmv_seq_note_put(m, clip, abs, m->pad[p].b, v, RFM_MOVE_SEQ_TPS, 0) == 0;
                    last_add(q, m->pad[p].b, v);
                }
            q->step_add |= bit;
        } else if (hi == lo) {                          /* empty: the last notes go in at once */
            uint8_t nn[RFM_MOVE_SEQ_POLY], vv[RFM_MOVE_SEQ_POLY];
            const int n = last_notes(m, nn, vv);
            for (i = 0; i < n; i++)
                put |= rfmv_seq_note_put(m, clip, abs, nn[i], vel_of(m, vv[i]), RFM_MOVE_SEQ_TPS, 0) == 0;
            q->step_add |= bit;
        }                                               /* (a step with notes: cleared on its release, if a tap) */
        if (put)
            entered(m, was);
    }
}

/* ================================================================ held steps: nudge, transpose, velocity === */
static int held_view(const rfm_move_t *m) { return !m->seq.view && !m->seq.projects && m->seq.step_held; }

static void held_nudge(rfm_move_t *m, int d)
{
    rfm_move_seq_t *q = Q(m);
    const int clip = edit_clip(m);
    int s, lo, hi, i;
    for (s = 0; s < RFM_MOVE_NSTEPS; s++)
        if (q->step_held >> s & 1) {
            const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
            hi = rfmv_seq_range(m, clip, abs, &lo);
            for (i = lo; i < hi; i++) {
                const rfm_move_note_t n = q->notes[i];
                rfmv_seq_note_put(m, clip, abs, n.note, n.vel, n.len, n.micro + d);   /* (clamped to -12..+11) */
            }
            q->step_edit |= (uint16_t)(1u << s);
        }
}
static void held_transpose(rfm_move_t *m, int d)
{
    rfm_move_seq_t *q = Q(m);
    const int clip = edit_clip(m);
    int s, lo, hi, i, k, ok;
    for (s = 0; s < RFM_MOVE_NSTEPS; s++)
        if (q->step_held >> s & 1) {
            const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
            rfm_move_note_t n[RFM_MOVE_SEQ_POLY];
            hi = rfmv_seq_range(m, clip, abs, &lo);
            for (i = lo, k = 0, ok = 1; i < hi && k < RFM_MOVE_SEQ_POLY; i++, k++) {
                n[k] = q->notes[i];
                ok &= n[k].note + d >= 0 && n[k].note + d <= 127;
            }
            if (!ok || !k)
                continue;                               /* (a note would leave 0..127: the step stays) */
            step_notes_clear(m, clip, abs);
            for (i = 0; i < k; i++)
                rfmv_seq_note_put(m, clip, abs, n[i].note + d, n[i].vel, n[i].len, n[i].micro);
            q->step_edit |= (uint16_t)(1u << s);
        }
}
int rfmv_seq_volume(rfm_move_t *m, int d)
{
    rfm_move_seq_t *q = Q(m);
    const int clip = edit_clip(m);
    int s, lo, hi, i;
    if (!held_view(m))
        return 0;
    for (s = 0; s < RFM_MOVE_NSTEPS; s++)
        if (q->step_held >> s & 1) {
            const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
            hi = rfmv_seq_range(m, clip, abs, &lo);
            for (i = lo; i < hi; i++) {
                const rfm_move_note_t n = q->notes[i];
                rfmv_seq_note_put(m, clip, abs, n.note, clampi(n.vel + d, 1, 127), n.len, n.micro);
            }
            q->step_edit |= (uint16_t)(1u << s);
        }
    return 1;
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
            if (cc == MR_CC_LOOP && (q->mods & SEQ_M_SHIFT)) {   /* Shift + Loop: double the loop (as Shift + 15) */
                q->mods_used |= SEQ_M_SHIFT;
                rfmv_seq_group(m);
                if (rfmv_seq_clip_double(m, edit_clip(m)) == 0)
                    rfmv_seq_msg(m, SEQ_MSG_DOUBLE);
                return 1;
            }
            q->mods |= (uint8_t)mb, q->mods_used &= (uint8_t)~mb;
            if (mb == SEQ_M_COPY)
                q->copy_src = 0, q->copy_pasted = 0;
        } else if (q->mods & mb) {
            const int used = q->mods_used & mb;
            const int alone = !used && !q->projects && !(q->mods & SEQ_M_SHIFT) && !q->step_held &&
                              !rfmv_track_held(m, 3);
            q->mods &= (uint8_t)~mb;
            if (mb == SEQ_M_MUTE && alone)
                rfmv_seq_mute(m, q->sel_track, !q->tr[q->sel_track].mute);   /* Mute alone: the selected track */
            if (mb == SEQ_M_COPY) {
                if (used)                               /* a source and no paste yet: the next press pastes (Move) */
                    q->paste = (uint8_t)(q->copy_src && !q->copy_pasted);
                else if (alone)
                    copy_alone(m);
                q->copy_src = 0, q->copy_pasted = 0;
            }
            if (mb == SEQ_M_DELETE && alone)
                delete_alone(m);
        }
        return 1;
    }
    if (!down)
        return 1;
    if (held_view(m)) {                                 /* a step held: the arrows edit its notes (Move) */
        switch (cc) {
        case MR_CC_LEFT:
        case MR_CC_RIGHT:
            held_nudge(m, (cc == MR_CC_RIGHT ? 1 : -1) * ((q->mods & SEQ_M_SHIFT) ? 1 : NUDGE));
            if (q->mods & SEQ_M_SHIFT)
                q->mods_used |= SEQ_M_SHIFT;
            return 1;
        case MR_CC_UP:
        case MR_CC_DOWN:
            held_transpose(m, (cc == MR_CC_UP ? 1 : -1) * ((q->mods & SEQ_M_SHIFT) ? 12 : 1));
            if (q->mods & SEQ_M_SHIFT)
                q->mods_used |= SEQ_M_SHIFT;
            return 1;
        default: break;
        }
    }
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
        q->ncap = 0;                                    /* (Move: the transport clears the capture) */
        if (q->mods & SEQ_M_SHIFT) {                    /* Shift + Play: every clip from its start (Move) */
            q->mods_used |= SEQ_M_SHIFT;
            rfmv_seq_restart(m);
        } else if (running(q))
            rfmv_seq_stop(m);
        else
            rfmv_seq_start(m);
        return 1;
    case MR_CC_REC:
        if (!q->rec) {
            q->rec = 1;
            rfmv_seq_group(m);
            q->rec_group = q->group;
            if (!running(q))
                rfmv_seq_count_in(m);                   /* stopped: a bar's count-in (Move) */
        } else {
            int t;
            q->rec = 0;
            if (q->start_pending == 3)
                q->start_pending = 0;                   /* (off during the count-in: no start) */
            for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
                if (q->tr[t].grow == 1)
                    q->tr[t].grow_stop = 1;
        }
        return 1;
    case MR_CC_CAPTURE:
        if (q->mods & SEQ_M_SHIFT) {                    /* Shift + Capture: the capture forgotten (Move) */
            q->mods_used |= SEQ_M_SHIFT;
            q->ncap = 0;
            rfmv_seq_msg(m, SEQ_MSG_CAPCLEAR);
        } else if (rfmv_seq_capture(m) >= 0) {
            rfmv_seq_msg(m, SEQ_MSG_CAPTURED);
            if (!running(q))
                rfmv_seq_start(m);                      /* (Move: the transport starts with the new clip) */
        } else
            rfmv_seq_msg(m, SEQ_MSG_NOCLIP);
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
    if (!q->view && q->step_held) {                     /* held steps: the notes' length (Move: ~10 % of a step a
                                                         * detent; Shift: whole steps) */
        const int clip = edit_clip(m), whole = (q->mods & SEQ_M_SHIFT) != 0;
        int s, lo, hi, i;
        if (whole)
            q->mods_used |= SEQ_M_SHIFT;
        for (s = 0; s < RFM_MOVE_NSTEPS; s++)
            if (q->step_held >> s & 1) {
                const int abs = q->page * RFM_MOVE_SEQ_PAGE + s;
                hi = rfmv_seq_range(m, clip, abs, &lo);
                for (i = lo; i < hi; i++) {
                    const rfm_move_note_t n = q->notes[i];
                    const int len = whole ? (n.len / RFM_MOVE_SEQ_TPS + d) * RFM_MOVE_SEQ_TPS : n.len + d * LEN_FINE;
                    rfmv_seq_note_put(m, clip, n.step, n.note, n.vel,
                                      clampi(len, whole ? RFM_MOVE_SEQ_TPS : 1, clip_len(m) * RFM_MOVE_SEQ_TPS), n.micro);
                }
                q->step_edit |= (uint16_t)(1u << s);
            }
        return;
    }
    if (q->mods & SEQ_M_LOOP) {                         /* Loop held: the clip's length (Move), a page a detent
                                                         * (Shift: a step) */
        const int clip = edit_clip(m), unit = (q->mods & SEQ_M_SHIFT) ? 1 : RFM_MOVE_SEQ_PAGE;
        const int L = q->len[clip], nl = unit == 1 ? L + d : ((L + unit - 1) / unit + d) * unit;
        if (!(q->mods_used & SEQ_M_LOOP))
            rfmv_seq_group(m);
        q->mods_used |= SEQ_M_LOOP | ((q->mods & SEQ_M_SHIFT) ? SEQ_M_SHIFT : 0);
        rfmv_seq_len_set(m, clip, clampi(nl, unit, RFM_MOVE_SEQ_MAXLEN));
        q->page = (uint8_t)clampi(q->page, 0, (q->len[clip] - 1) / RFM_MOVE_SEQ_PAGE);
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

/* a clip's state in session view (the LEDs and the screen's grid) */
enum { CS_EMPTY, CS_CONTENT, CS_PLAYING, CS_QUEUED, CS_STOPPING, CS_REC, CS_REC_WAIT };
static int clip_state(const rfm_move_t *m, int track, int scene)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[track];
    const int used = rfmv_seq_clip_used(m, SEQ_CLIP(track, scene));
    if (t->grow && (t->active == scene || t->queued == scene))
        return t->grow == 1 ? CS_REC : CS_REC_WAIT;
    if (t->queued == scene)
        return CS_QUEUED;
    if (t->active == scene && used && t->queued == SEQ_STOP && running(q))
        return CS_STOPPING;
    if (t->active == scene && used && q->playing)
        return CS_PLAYING;
    return used ? CS_CONTENT : CS_EMPTY;
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
        } else if (q->view) {                           /* Move's session colours */
            const int track = q->track_page * 8 + i % 8, scene = q->scene_off + (3 - i / 8);
            const rfm_move_track_t *t = &q->tr[track];
            const int col = t->mute ? 96 : 256, sel = track == q->sel_track && t->sel == scene;
            switch (clip_state(m, track, scene)) {
            case CS_REC: v = sc(RFMV_RED, pu); break;
            case CS_REC_WAIT: v = sc(RFMV_RED, bl ? 256 : 40); break;
            case CS_QUEUED: v = sc(RFMV_GREEN, bl ? 256 : 40); break;                  /* to launch: green */
            case CS_STOPPING: v = sc(TRACK_RGB[track], bl ? col : 24); break;          /* to stop: blinking */
            case CS_PLAYING: v = sel ? sc(RFMV_WHITE, pu) : sc(TRACK_RGB[track], pu * col / 256); break;
            case CS_CONTENT:                            /* (stopped, the track's clip to play: brighter) */
                v = sel ? sc(RFMV_WHITE, 150) : sc(TRACK_RGB[track], (t->active == scene ? 256 : 110) * col / 256);
                break;
            default: v = sel ? sc(RFMV_WHITE, 60) : RFMV_OFF; break;
            }
        } else {
            const int note = rfm_move_pad_note(&m->s, i);
            if (note >= 0 && q->step_first >= 0) {      /* a step held: its notes bright white, the rest dim */
                if (rfmv_seq_find(m, clip, q->page * RFM_MOVE_SEQ_PAGE + q->step_first, note) >= 0)
                    v = RFMV_WHITE;
                else
                    v = sc(rfmv_layout_pad(m, i, mode), 96);
            } else {
                v = rfmv_midi_pad(m, i, mode);
                if (note >= 0 && !m->pad_down[i]) {     /* the notes an empty step gets: the track's colour */
                    uint8_t nn[RFM_MOVE_SEQ_POLY], vv[RFM_MOVE_SEQ_POLY];
                    const int n = last_notes(m, nn, vv);
                    int k;
                    for (k = 0; k < n; k++)
                        if (nn[k] == note && q->nlast)
                            v = sc(tc, 200);
                }
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
        } else if (q->mods & SEQ_M_LOOP) {              /* the pages (Move: selected white, in the clip the track's
                                                         * colour, outside dim) */
            const int pages = (len + RFM_MOVE_SEQ_PAGE - 1) / RFM_MOVE_SEQ_PAGE;
            v = i == q->page ? RFMV_WHITE : i < pages ? tc : sc(RFMV_WHITE, 10);
        } else if (q->mods & SEQ_M_SHIFT) {             /* the shortcuts */
            if (i == SH_PROJECTS || i == SH_CLOCK || i == SH_FIXVEL || i == SH_DOUBLE || i == SH_QUANT)
                v = sc(RFMV_WHITE, 96);
            if (i == SH_FIXVEL && q->fixed_vel)
                v = RFMV_WHITE;
            if (i == SH_CLOCK && q->following)
                v = RFMV_GREEN;
        } else if (q->view) {                           /* the scenes */
            int used;
            const int st = scene_state(m, i, &used);
            v = st == 2 ? sc(RFMV_GREEN, pu) : st == 1 ? sc(RFMV_GREEN, bl ? 256 : 24) : used ? sc(RFMV_WHITE, 48) : RFMV_OFF;
        } else {                                        /* Move's step colours */
            const int abs = q->page * RFM_MOVE_SEQ_PAGE + i;
            if (abs < len) {
                int lo, hi = rfmv_seq_range(m, clip, abs, &lo);
                const int pos = q->tr[q->sel_track].active == q->tr[q->sel_track].sel ? rfmv_seq_clip_pos(m, q->sel_track) : -1;
                if (q->step_held >> i & 1)
                    v = tc;                             /* held: the track's colour, full */
                else if (pos == abs)
                    v = RFMV_GREEN;                     /* the playhead */
                else if (hi > lo)
                    v = RFMV_WHITE;                     /* notes */
                else
                    v = sc(tc, i % 4 ? 28 : 48);        /* empty: the track's colour, dim (beats a little more) */
            } else
                v = sc(RFMV_WHITE, 8);                  /* past the loop: dim grey */
        }
        c[L_STEP0 + i] = v;
    }
    /* Play, Rec, Sample (the beat) */
    c[L_PLAY] = q->playing ? RFMV_GREEN : q->start_pending ? sc(RFMV_GREEN, bl ? 256 : 24) : sc(RFMV_WHITE, 40);
    c[L_REC] = q->start_pending == 3 ? sc(RFMV_RED, bl ? 256 : 30) : q->rec ? RFMV_RED : sc(RFMV_RED, 30);
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
    } else if (q->step_held) {
        w[W_UP] = w[W_DOWN] = w[W_LEFT] = w[W_RIGHT] = 100;   /* (transpose, nudge) */
    } else {
        w[W_UP] = m->s.octave > 0 ? 127 : 24;
        w[W_DOWN] = m->s.octave < 0 ? 127 : 24;
        w[W_LEFT] = q->page > 0 ? 100 : 8;
        w[W_RIGHT] = (q->page + 1) * RFM_MOVE_SEQ_PAGE < len ? 100 : 8;
    }
    w[W_UNDO] = q->un ? 60 : 6;
    w[W_LOOP] = (q->mods & SEQ_M_LOOP) ? 127 : 40;
    w[W_COPY] = (q->mods & SEQ_M_COPY) ? 127 : q->paste ? (bl ? 127 : 30) : q->clip_kind && q->cut ? 70 : 30;
    w[W_MUTE] = (q->mods & SEQ_M_MUTE) ? 127 : q->tr[q->sel_track].mute ? 100 : 30;
    w[W_DELETE] = (q->mods & SEQ_M_DELETE) ? 127 : 30;
}

/* ================================================================ the screen === */
static const char *const MSG[] = { "", "NO EMPTY CLIP", "FULL", "LOAD FAILED", "CAPTURED", "UNDO", "REDO", "COPIED",
                                   "PASTED", "CUT", "QUANTIZED", "DOUBLED", "EXT TEMPO", "DUPLICATED", "CLEARED",
                                   "CAPTURE CLEARED" };
static const char *msg_now(const rfm_move_seq_t *q)
{
    const uint32_t now_ms = (uint32_t)(q->now_us / 1000u);
    return q->msg && now_ms - q->msg_ms < 1500 && q->msg < sizeof MSG / sizeof MSG[0] ? MSG[q->msg] : NULL;
}

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
    int i;
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
}

/* the title bar's marks: play (a triangle), waiting / counting in (blinking box), recording (a dot) */
static void transport_marks(const rfm_move_seq_t *q, mr_canvas_t *cv, int x)
{
    int i;
    if (q->playing) {
        for (i = 0; i < 4; i++)
            mr_gfx_rect(cv, x + i, 1 + i, 1, 7 - 2 * i, 0, 1);
    } else if (q->start_pending && blink(q))
        mr_gfx_rect(cv, x, 2, 4, 5, 0, 0);
    if (q->rec)
        mr_gfx_rect(cv, x + 8, 2, 5, 5, 0, 1);
}

/* Session view: the 8 tracks of the page as columns, 8 scenes as rows with the 4 on the pads framed; each clip
 * empty (a dot), with content (an outline), playing (filled, the playhead a gap moving through it), queued
 * (blinking), stopping (blinking), recording (a checker); the selected track's number inverted, the selected clip
 * marked at its left; muted tracks' numbers struck through; the bar's 16ths at the bottom */
#define SG_Y0 18                                        /* the first row */
#define SG_PITCH 5
#define SG_ROWS 8
static void session_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int bpm = rfm_move_seq_bpm10(m), bl = blink(q);
    const int win = clampi(q->scene_off - 2, 0, RFM_MOVE_SEQ_SCENES - SG_ROWS);   /* the rows shown */
    const char *msg = msg_now(q);
    char b[24];
    int c, r, x, y;
    rfmv_title(cv, 128, "");
    snprintf(b, sizeof b, "P%02d", q->proj_cur + 1);
    inv_text(cv, 1, 1, b);
    transport_marks(q, cv, 22);
    if (msg)
        snprintf(b, sizeof b, "%.13s", msg);
    else
        snprintf(b, sizeof b, "%s%d.%d", q->following ? "EXT " : "", bpm / 10, bpm % 10);
    inv_text(cv, 127 - 6 * (int)strlen(b), 1, b);
    for (c = 0; c < 8; c++) {                           /* the track numbers */
        const int track = q->track_page * 8 + c, x0 = c * 16;
        const int sel = track == q->sel_track;
        snprintf(b, sizeof b, "%d", track + 1);
        x = x0 + 9 - 3 * (int)strlen(b);
        if (sel)
            mr_gfx_rect(cv, x0 + 3, 9, 13, 8, 1, 1);
        mr_gfx_text(cv, x, 10, b, !sel);
        if (q->tr[track].mute)
            mr_gfx_rect(cv, x0 + 4, 13, 11, 1, !sel, 1);
    }
    for (r = 0; r < SG_ROWS; r++) {                     /* the clips */
        const int scene = win + r;
        y = SG_Y0 + r * SG_PITCH;
        for (c = 0; c < 8; c++) {
            const int track = q->track_page * 8 + c, st = clip_state(m, track, scene), w = 10, h = SG_PITCH - 1;
            const rfm_move_track_t *t = &q->tr[track];
            int i, j;
            x = c * 16 + 4;
            switch (st) {
            case CS_EMPTY: mr_gfx_pixel(cv, x + 4, y + 1, 1), mr_gfx_pixel(cv, x + 4, y + 2, 1); break;
            case CS_CONTENT: mr_gfx_rect(cv, x, y, w, h, 1, 0); break;
            case CS_QUEUED: mr_gfx_rect(cv, x, y, w, h, 1, bl); break;
            case CS_REC_WAIT:                           /* (blinking: the checker on the 8ths) */
            case CS_REC:
                mr_gfx_rect(cv, x, y, w, h, 1, 0);
                for (j = 1; j < h - 1; j++)
                    for (i = 1; i < w - 1; i++)
                        if (st == CS_REC ? ((i + j + (bl ? 1 : 0)) & 1) : (bl && ((i + j) & 1)))
                            mr_gfx_pixel(cv, x + i, y + j, 1);
                break;
            default: {                                  /* playing, stopping: filled, the playhead a gap */
                const int L = t->grow == 1 ? RFM_MOVE_SEQ_MAXLEN : q->len[SEQ_CLIP(track, scene)];
                const int pos = rfmv_seq_clip_pos(m, track);
                mr_gfx_rect(cv, x, y, w, h, 1, !(st == CS_STOPPING && bl));
                if (pos >= 0 && L > 0 && !(st == CS_STOPPING && bl))
                    mr_gfx_rect(cv, x + 1 + pos * (w - 2) / L, y, 1, h, 0, 1);
                break;
            }
            }
            if (track == q->sel_track && t->sel == scene)   /* the selected clip: a bar at its left */
                mr_gfx_rect(cv, x - 2, y, 1, h, 1, 1);
        }
    }
    {                                                   /* the 4 rows on the pads: brackets at the sides */
        const int y0 = SG_Y0 + (q->scene_off - win) * SG_PITCH - 1, y1 = y0 + 4 * SG_PITCH;
        mr_gfx_rect(cv, 0, y0, 1, y1 - y0 + 1, 1, 1);
        mr_gfx_rect(cv, 127, y0, 1, y1 - y0 + 1, 1, 1);
        mr_gfx_rect(cv, 1, y0, 2, 1, 1, 1), mr_gfx_rect(cv, 1, y1, 2, 1, 1, 1);
        mr_gfx_rect(cv, 125, y0, 2, 1, 1, 1), mr_gfx_rect(cv, 125, y1, 2, 1, 1, 1);
    }
    {                                                   /* the bar's 16ths */
        const int pos = q->playing && q->T >= 0 ? (int)(q->T % RFM_MOVE_SEQ_BAR / RFM_MOVE_SEQ_TPS) : -1;
        for (c = 0; c < RFM_MOVE_NSTEPS; c++) {
            x = c * 8 + 1;
            if (c == pos)
                mr_gfx_rect(cv, x, 60, 6, 3, 1, 1);
            else
                mr_gfx_rect(cv, x + 2, 61, c % 4 ? 1 : 2, 1, 1, 1);
        }
    }
}

void rfmv_seq_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    const int clip = edit_clip(m), len = q->len[clip], bpm = rfm_move_seq_bpm10(m);
    const char *msg = msg_now(q);
    char b[40], nb[8];
    int i, y;
    if (q->projects) {
        projects_screen(m, cv);
        return;
    }
    if (q->view) {
        session_screen(m, cv);
        return;
    }
    rfmv_title(cv, 128, "TRACK");
    snprintf(b, sizeof b, "P%02d", q->proj_cur + 1);
    inv_text(cv, 128 - 6 * 3 - 1, 1, b);
    transport_marks(q, cv, 70);
    /* line 1: the track and the clock */
    snprintf(b, sizeof b, "T%02d CH%02d", q->sel_track + 1, t->ch + 1);
    rfmv_text(cv, 0, 11, b, 9);
    snprintf(b, sizeof b, "%3d.%d %s", bpm / 10, bpm % 10, q->following ? "EXT" : "INT");
    rfmv_text(cv, 128 - 6 * (int)strlen(b), 11, b, 21);
    /* line 2: where, and the notes an empty step gets */
    {
        uint8_t nn[RFM_MOVE_SEQ_POLY], vv[RFM_MOVE_SEQ_POLY];
        const int n = last_notes(m, nn, vv);
        char more[6] = "";
        if (n > 1)
            snprintf(more, sizeof more, "+%d", n - 1);
        snprintf(b, sizeof b, "S%d PG %d/%d %s%s%s", t->sel + 1, q->page + 1, (len + 15) / 16,
                 rfmv_note_name(nn[0], nb, sizeof nb), more, q->fixed_vel ? " FIX" : "");
        rfmv_text(cv, 0, 20, b, 21);
    }
    /* line 3: the transport (the count-in's beats) */
    bbt(q, nb, sizeof nb);
    if (q->start_pending == 3)
        snprintf(b, sizeof b, "COUNT IN %d%s", (q->count_in - 1) / 96 + 1, t->mute ? " MUTE" : "");
    else
        snprintf(b, sizeof b, "%s %s%s%s", q->playing ? "PLAY" : q->start_pending ? "WAIT" : "STOP", nb,
                 q->rec ? " REC" : "", t->mute ? " MUTE" : "");
    rfmv_text(cv, 0, 29, b, 21);
    /* lines 4, 5: the held step, else a message, else the knob */
    y = 38;
    if (q->step_first >= 0) {
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
    } else if (msg)
        rfmv_text(cv, 0, y, msg, 21);
    else if (q->paste)
        rfmv_text(cv, 0, y, "PASTE", 21);
    else if (m->last_kind == LAST_CC && m->last_index < RFM_MOVE_NKNOBS) {
        snprintf(b, sizeof b, "K%d CC%d %d  BANK %d", m->last_index + 1, m->s.cc[m->s.bank][m->last_index],
                 m->cc_val[m->s.bank][m->last_index], m->s.bank + 1);
        rfmv_text(cv, 0, y, b, 21);
    }
    /* the bar at the bottom: the page's 16 steps, the playhead above */
    {
        const int pos = t->active == t->sel ? rfmv_seq_clip_pos(m, q->sel_track) - q->page * RFM_MOVE_SEQ_PAGE : -1;
        for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
            const int x = i * 8, abs = q->page * RFM_MOVE_SEQ_PAGE + i;
            int lo, filled;
            if (abs >= len)
                continue;
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
