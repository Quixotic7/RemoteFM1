/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's Sequencer mode (Track 4): the Move's controls on the sequencer, its LEDs and its screen. The data and
 * the engine are seq.c's; host/src/move/README.md, "Sequencer", has the gestures and the decisions (and where they
 * follow Ableton Move's own sequencer). */
#include <stdio.h>
#include <string.h>

#include "move_int.h"
#include "v2.h"

#define clampi rfmv_clampi
#define sc rfmv_sc
typedef rfmv_rgb_t rgb_t;

#define NUDGE 2                                         /* ticks a Left / Right with a step held (~10 % of a step) */
#define LEN_FINE 2                                      /* ticks a jog detent with a step held (~10 % of a step) */

/* the tracks' colours */
static const rgb_t TRACK_RGB[RFM_MOVE_SEQ_TRACKS] = {
    { 255, 42, 42 },  { 255, 128, 0 },  { 255, 208, 0 },   { 200, 255, 42 }, { 42, 255, 80 },   { 0, 255, 190 },
    { 26, 228, 255 }, { 40, 120, 255 }, { 120, 60, 255 },  { 196, 58, 255 }, { 255, 58, 200 },  { 255, 110, 140 },
    { 255, 170, 110 }, { 180, 255, 160 }, { 140, 220, 255 }, { 220, 200, 255 },
};
/* Shift + step n (0-based), as printed on the Move (its manual): 1 Set Overview (our projects), 2 Setup (our clock
 * source), 3 Workflow Settings, 5 Tempo, 6 Metronome, 7 Groove, 8 Layout, 9 Keys & Scales, 10 Full Velocity (our Velocity menu),
 * 11 Repeat (our arp), 14 New Clip, 15 Double Loop, 16 Quantize */
enum { SH_PROJECTS = 0, SH_CLOCK = 1, SH_WORKFLOW = 2, SH_TEMPO = 4, SH_METRO = 5, SH_GROOVE = 6, SH_LAYOUT = 7,
       SH_KEYS = 8, SH_FIXVEL = 9, SH_ARP = 10, SH_NEWCLIP = 13, SH_DOUBLE = 14, SH_QUANT = 15 };
static int sh_menu(int s)                               /* the menu a Shift + step opens, 0 none */
{
    switch (s) {
    case SH_WORKFLOW: return RFM_MOVE_SM_WORKFLOW;
    case SH_TEMPO: return RFM_MOVE_SM_TEMPO;
    case SH_GROOVE: return RFM_MOVE_SM_GROOVE;
    case SH_LAYOUT: return RFM_MOVE_SM_LAYOUT;
    case SH_KEYS: return RFM_MOVE_SM_KEYS;
    case SH_ARP: return RFM_MOVE_SM_ARP;
    case SH_FIXVEL: return RFM_MOVE_SM_VEL;
    case SH_CLOCK: return RFM_MOVE_SM_GLOBAL;
    default: return 0;
    }
}
/* the icons under the steps while the Shift layer is on (Shift held or locked in the sequencer, Track 1 held in
 * FM-1 mode): every step with a shortcut dim, the ones on (a toggle, its menu open) full; off otherwise */
void rfmv_shift_icons(const rfm_move_t *m, uint8_t icon[16])
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    memset(icon, 0, 16);
    if (!((m->s.mode == RFM_MOVE_MODE_SEQ && (q->mods & SEQ_M_SHIFT)) || rfmv_track_held(m, 0)))
        return;
    for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
        const int job = i == SH_PROJECTS || i == SH_CLOCK || i == SH_FIXVEL || i == SH_DOUBLE || i == SH_QUANT ||
                        i == SH_METRO || i == SH_NEWCLIP || sh_menu(i);
        const int on = (i == SH_FIXVEL && m->s.vel_mode == RFM_MOVE_VEL_FIXED) || (i == SH_METRO && q->metro) || (i == SH_CLOCK && q->following) ||
                       (i == SH_PROJECTS && q->projects) || (sh_menu(i) && sh_menu(i) == q->smenu);
        icon[i] = (uint8_t)(!job ? 0 : on ? 127 : 24);
    }
}


static rfm_move_seq_t *Q(rfm_move_t *m) { return &m->seq; }
static int step_empty(const rfm_move_t *m, int clip, int abs)
{
    int lo;
    return rfmv_seq_range(m, clip, abs, &lo) == lo;
}
static int edit_clip(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    return SEQ_CLIP(q->sel_track, q->tr[q->sel_track].sel);
}
static int clip_len(const rfm_move_t *m) { return m->seq.len[edit_clip(m)]; }
static int vel_of(const rfm_move_t *m, int v) { (void)m; return clampi(v, 1, 127); }   /* (the pads' already through the
                                                         * velocity setting) */
static int running(const rfm_move_seq_t *q) { return q->playing || q->start_pending; }
static int root_note(const rfm_move_t *m) { return clampi(48 + m->s.root + 12 * m->s.octave, 0, 127); }

static void clip_delete(rfm_move_t *m, int clip);

/* the layers Back closes, the latest first */
enum { LY_MENU = 1, LY_PROJ, LY_LOOP, LY_SHIFT, LY_CLIP, LY_PAGE };
static void layer(rfm_move_seq_t *q, int id)
{
    int i, k = 0;
    for (i = 0; i < q->nlayer; i++)                     /* (once: to the top) */
        if (q->layer[i] != id)
            q->layer[k++] = q->layer[i];
    q->nlayer = (uint8_t)k;
    if (q->nlayer < sizeof q->layer)
        q->layer[q->nlayer++] = (uint8_t)id;
}
static int layer_on(const rfm_move_seq_t *q, int id)
{
    switch (id) {
    case LY_MENU: return q->smenu != 0;
    case LY_PROJ: return q->projects != 0;
    case LY_LOOP: return q->loop_latch != 0;
    case LY_SHIFT: return q->shift_lock != 0;
    case LY_CLIP: return q->clip_kind && (q->cut || q->paste);
    case LY_PAGE: return q->pg_kind != 0;
    default: return 0;
    }
}
/* a clip's last page: the loop's, or a later one with notes (past it: ghosts) */
static int clip_end_page(const rfm_move_t *m, int clip)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo), k, end = (rfmv_seq_loop_hi(m, clip) - 1) / RFM_MOVE_SEQ_PAGE;
    for (k = lo; k < hi; k++)
        if (m->seq.notes[k].step / RFM_MOVE_SEQ_PAGE > end)
            end = m->seq.notes[k].step / RFM_MOVE_SEQ_PAGE;
    return end;
}
static void layer_close(rfm_move_t *m, int id)
{
    rfm_move_seq_t *q = &m->seq;
    switch (id) {
    case LY_MENU: q->smenu = 0; break;
    case LY_PROJ: q->projects = 0; break;
    case LY_LOOP: q->loop_latch = 0, q->mods &= (uint8_t)~SEQ_M_LOOP, rfmv_pop_let(m, POP_LOOP, 0); break;
    case LY_SHIFT: q->shift_lock = 0, q->mods &= (uint8_t)~SEQ_M_SHIFT; break;
    case LY_CLIP: q->paste = 0, q->cut = 0, q->clip_kind = 0; break;
    case LY_PAGE: q->pg_kind = 0; break;
    default: break;
    }
}
static void grow_to(rfm_move_t *m, int clip, int abs);

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
    q->step_held = q->step_add = q->step_edit = 0, q->step_first = -1, q->step_popped = 0;
    q->loop_first = -1, q->fill = 0, q->steps_chord = 0, q->loop_latch = 0;
    rfmv_seq_arp_stop(m);
}

/* the edited clip has notes but the track plays nothing: it plays (at once when stopped, else at the next bar) */
static void ensure_active(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    if (t->active < 0 && t->queued == SEQ_NONE && rfmv_seq_clip_used(m, edit_clip(m)))
        rfmv_seq_launch(m, q->sel_track, t->sel);
}
/* notes went into the edited clip: it plays (when the transport does: only Play starts it, the owner's) */
static void entered(rfm_move_t *m, int was_used)
{
    (void)was_used;
    ensure_active(m);
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
        q->paste = 0, q->cut = 0, q->clip_kind = 0;    /* (the source's mark goes: no popup) */
        return;
    }
    if (!rfmv_seq_clip_used(m, edit_clip(m)))
        return;
    for (s = from + 1; s < RFM_MOVE_SEQ_SCENES && to < 0; s++)
        if (!rfmv_seq_clip_has(m, SEQ_CLIP(q->sel_track, s)))
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
/* Delete released with nothing pressed (Move): the selected track's clip is deleted */
static void delete_alone(rfm_move_t *m)
{
    const int clip = edit_clip(m);
    if (!rfmv_seq_clip_has(m, clip))
        return;
    rfmv_seq_group(m);
    clip_delete(m, clip);
    rfmv_seq_msg(m, SEQ_MSG_CLEARED);
}

/* a clip deleted: its notes and locks, and the clip itself (the slot empty again) */
static void clip_delete(rfm_move_t *m, int clip)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_track_t *t = &q->tr[clip / RFM_MOVE_SEQ_SCENES];
    if (t->queued == clip % RFM_MOVE_SEQ_SCENES)
        t->queued = SEQ_STOP;                           /* (its launch: the track stops instead) */
    if (t->active == clip % RFM_MOVE_SEQ_SCENES) {
        rfmv_seq_release(m, clip / RFM_MOVE_SEQ_SCENES);
        if (running(q))
            rfmv_seq_launch(m, clip / RFM_MOVE_SEQ_SCENES, -1);
        else
            t->active = -1;
    }
    rfmv_seq_clip_clear(m, clip);
    rfmv_seq_clip_make(m, clip, 0);
}
/* the clipboard's clip at clip; a cut (already gone from its slot) is pasted once */
static void paste_clip_at(rfm_move_t *m, int clip)
{
    rfm_move_seq_t *q = Q(m);
    rfmv_seq_group(m);
    rfmv_seq_paste_clip(m, clip);
    if (q->cut)
        q->cut = 0, q->clip_kind = 0;
}

/* Step record: the cursor a step on, round the loop (the steps are the track's rate) */
static void step_cursor_on(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    const int clip = edit_clip(m), lo = rfmv_seq_loop_lo(m, clip), hi = rfmv_seq_loop_hi(m, clip);
    q->step_cur = (int16_t)(q->step_cur + 1 >= hi || q->step_cur + 1 < lo ? lo : q->step_cur + 1);
    q->page = (uint8_t)(q->step_cur / RFM_MOVE_SEQ_PAGE);
}

/* ================================================================ tracks: 16, the Track buttons a bank of 4 === */
/* each track keeps its own pad layout and CC numbers: while in Sequencer mode the settings hold the selected
 * track's (MIDI / Control mode's are kept aside meanwhile) */
void rfmv_seq_sync(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    if (!q->in_seq)
        return;
    q->tr[q->sel_track].layout = m->s.layout;
    memcpy(q->tr[q->sel_track].cc, m->s.cc, sizeof m->s.cc);
    if (q->tr[q->sel_track].own & 2)                    /* the key and scale: the track's own, or the project's */
        q->tr[q->sel_track].root = m->s.root, q->tr[q->sel_track].scale = m->s.scale;
    else
        q->root = m->s.root, q->scale = m->s.scale;
}
static void load_track(rfm_move_t *m);
void rfmv_seq_load_track(rfm_move_t *m) { load_track(m); }
static void load_track(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    if (!q->in_seq)
        return;
    {
        const rfm_move_track_t *t = &q->tr[q->sel_track];
        m->s.layout = t->layout;
        memcpy(m->s.cc, t->cc, sizeof m->s.cc);
        m->s.root = (t->own & 2) ? t->root : q->root;
        m->s.scale = (t->own & 2) ? t->scale : q->scale;
    }
}
void rfmv_seq_enter(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    if (q->in_seq)
        return;
    q->stash_layout = m->s.layout, q->stash_root = m->s.root, q->stash_scale = m->s.scale;
    memcpy(q->stash_cc, m->s.cc, sizeof m->s.cc);
    q->in_seq = 1;
    load_track(m);
}
void rfmv_seq_leave(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    if (!q->in_seq)
        return;
    rfmv_seq_sync(m);
    q->in_seq = 0;
    m->s.layout = q->stash_layout, m->s.root = q->stash_root, m->s.scale = q->stash_scale;
    memcpy(m->s.cc, q->stash_cc, sizeof m->s.cc);
}
void rfmv_seq_select(rfm_move_t *m, int track)
{
    rfm_move_seq_t *q = Q(m);
    track = clampi(track, 0, RFM_MOVE_SEQ_TRACKS - 1);
    if (track == q->sel_track)
        return;
    rfmv_seq_sync(m);
    q->sel_track = (uint8_t)track, q->bank = (uint8_t)(track / 4);
    q->page = (uint8_t)(q->lstart[edit_clip(m)] / RFM_MOVE_SEQ_PAGE);
    q->steprec = 0;
    load_track(m);
    rfmv_seq_dirty(m);
}
/* ================================================================ pads === */
void rfmv_seq_pad(rfm_move_t *m, int p, int vel, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_hold_t *h = &m->pad[p];
    (void)now;
    if (!vel) {
        q->chord &= ~(1u << p);
        rfmv_pop_let(m, POP_PROJ, (3 - p / 8) * 8 + p % 8);
        if (h->kind == H_ARP)
            rfmv_seq_arp_off(m, h->b), h->kind = H_NONE;
        if (h->kind == H_NOTE)
            rfmv_seq_pad_off(m, p, h->b);
        if (q->steps_chord & (1u << p)) {              /* Step record: the chord let go, the cursor a step on */
            q->steps_chord &= ~(1u << p);
            if (!q->steps_chord)
                step_cursor_on(m);
        }
        return;
    }
    h->kind = H_MENU;
    if (q->projects) {                                  /* the grid: top-left = project 1 */
        const int slot = (3 - p / 8) * 8 + p % 8;
        if (q->mods & SEQ_M_DELETE) {
            q->mods_used |= SEQ_M_DELETE;
            rfmv_proj_delete(m, slot);
        } else {
            rfmv_pop(m, POP_PROJ, slot);                /* (its popup while the pad is held, also once it is open) */
            rfmv_proj_open(m, slot);
        }
        return;
    }
    if (q->view) {                                      /* session (the owner's mockup): a column a track (8 in view,
                                                         * Left / Right one track), a row a clip slot (4, Up / Down) */
        const int track = q->track_page + p % 8, scene = q->scene_off + (3 - p / 8);
        const int clip = SEQ_CLIP(track, scene);
        rfm_move_track_t *t = &q->tr[track];
        const int dbl = q->pad_tap == p && now - q->pad_tap_ms < 300;
        q->pad_tap = (int8_t)p, q->pad_tap_ms = now;
        if (q->mods & SEQ_M_SHIFT) {                    /* Shift + a clip: its whole row, a scene across the 16 */
            q->mods_used |= SEQ_M_SHIFT;
            rfmv_seq_launch_scene(m, scene);            /* (stopped: launched, it plays with Play) */
        } else if (dbl && t->active == scene && t->queued == SEQ_NONE && running(q) && rfmv_seq_clip_used(m, clip) &&
                   !t->grow && !(q->mods & (SEQ_M_MUTE | SEQ_M_COPY | SEQ_M_DELETE))) {
            rfmv_seq_launch(m, track, -1);              /* a double tap on the playing clip: it stops at the next bar
                                                         * (the owner's; on any other clip a double tap is a tap:
                                                         * everything launches at the next bar) */
            q->pad_tap = -1;
        } else if (q->mods & SEQ_M_MUTE) {
            q->mods_used |= SEQ_M_MUTE;
            rfmv_seq_mute(m, track, !t->mute);
        } else if (q->mods & SEQ_M_COPY) {              /* Copy + clip (no popups: the source is dashed) */
            q->mods_used |= SEQ_M_COPY;                 /* (a clip: copied; an empty slot: the clipboard pasted) */
            if (rfmv_seq_clip_has(m, clip)) {
                rfmv_seq_copy_clip(m, clip);
                q->clip_src = (uint8_t)clip, q->copy_src = 1, q->paste = 0, layer(q, LY_CLIP);
            } else if (q->clip_kind == 2) {
                paste_clip_at(m, clip);
                q->copy_pasted = 1, q->paste = 0;
            }
        } else if (q->mods & SEQ_M_DELETE) {            /* Delete + clip (cut): a clip is cut (an X in its slot); an
                                                         * empty slot gets the clipboard's clip */
            q->mods_used |= SEQ_M_DELETE;
            if (rfmv_seq_clip_has(m, clip)) {
                rfmv_seq_group(m);
                rfmv_seq_copy_clip(m, clip);
                q->cut = 1, q->paste = 0, q->clip_src = (uint8_t)clip, layer(q, LY_CLIP);
                clip_delete(m, clip);
            } else if (q->clip_kind == 2)
                paste_clip_at(m, clip);
        } else if (q->paste && q->clip_kind == 2) {     /* a copy waiting (Copy released): pasted here */
            paste_clip_at(m, clip);
            q->paste = 0;
        } else {
            const int run = running(q), used = rfmv_seq_clip_has(m, clip);
            rfmv_seq_select(m, track);
            if (t->grow == 1 && t->active == scene)
                t->grow_stop = 1;                       /* a recording clip: done at the next bar */
            else if (!used) {                           /* an empty slot: a new clip there, selected (and the track's) */
                t->sel = (int8_t)scene;
                rfmv_seq_clip_make(m, clip, 1);
                if (q->rec) {                           /* armed: record into it (stopped: once Play starts it) */
                    if (!run) {
                        rfmv_seq_group(m);
                        q->rec_group = q->group;
                    }
                    t->grow = 2;                        /* (starts with the launch, at the bar) */
                }
                rfmv_seq_launch(m, track, scene);
            } else if (t->active == scene && t->queued == SEQ_NONE && run)
                t->sel = (int8_t)scene;                 /* the playing clip: it plays on, selected */
            else {
                rfmv_seq_launch(m, track, scene);       /* at the next bar; stopped: at once, playing with Play */
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
        if (!q->step_held && !q->steprec && rfmv_seq_arp_on(m, note, vel)) {   /* the track's arp plays it */
            h->kind = H_ARP, h->b = (uint8_t)note;
            return;
        }
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
                    else {
                        const int e = step_empty(m, clip, abs);
                        rfmv_seq_note_put(m, clip, abs, note, vel, RFM_MOVE_SEQ_TPS, 0), grow_to(m, clip, abs);
                        if (e)
                            rfmv_seq_tdef_apply(m, clip, abs, 1);
                    }
                    q->step_edit |= (uint16_t)(1u << s);
                }
            entered(m, was);
            return;                                     /* (it sounds while held; nothing recorded) */
        }
        if (!q->chord)                                  /* a new chord (or note): step entry's memory */
            q->nlast = 0;
        q->chord |= 1u << p, q->pad_vel[p] = (uint8_t)vel;
        last_add(q, note, vel);
        if (q->steprec) {                               /* Step record: into the cursor's step (a new chord replaces
                                                         * what was there) */
            const int was = rfmv_seq_clip_used(m, clip);
            if (!q->steps_chord) {
                rfmv_seq_group(m);
                step_notes_clear(m, clip, q->step_cur);
            }
            q->steps_chord |= 1u << p;
            {
                const int e = step_empty(m, clip, q->step_cur);
                rfmv_seq_note_put(m, clip, q->step_cur, note, vel, RFM_MOVE_SEQ_TPS, 0);
                if (e)
                    rfmv_seq_tdef_apply(m, clip, q->step_cur, 1);
            }
            entered(m, was);
            return;
        }
        rfmv_seq_pad_on(m, p, note, vel);
    }
}

/* a step entered past the loop's end (a pending page, Right past the last): the loop grows to that page's end */
static void grow_to(rfm_move_t *m, int clip, int abs)
{
    const int lo = rfmv_seq_loop_lo(m, clip), hi = rfmv_seq_loop_hi(m, clip);
    if (abs >= hi && rfmv_seq_clip_used(m, clip) && rfmv_seq_range(m, clip, abs, &(int){ 0 }) > 0)
        rfmv_seq_loop_set(m, clip, lo, (abs / RFM_MOVE_SEQ_PAGE + 1) * RFM_MOVE_SEQ_PAGE - lo);
}

/* ---- Loop mode's pages: Delete + a page with notes cuts it (an X), Copy + one copies it (dashed), either + an
 * empty page pastes the page clipboard there (a pasted cut empties it) */
static int page_full(const rfm_move_t *m, int clip, int pg)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo), i;
    for (i = lo; i < hi; i++)
        if (m->seq.notes[i].step / RFM_MOVE_SEQ_PAGE == pg)
            return 1;
    hi = rfmv_seq_clip_lock_range(m, clip, &lo);
    for (i = lo; i < hi; i++)
        if (m->seq.locks[i].step / RFM_MOVE_SEQ_PAGE == pg)
            return 1;
    return 0;
}
static void page_edit(rfm_move_t *m, int clip, int pg, int del)
{
    rfm_move_seq_t *q = Q(m);
    const int a = pg * RFM_MOVE_SEQ_PAGE;
    int lo, hi, i;
    if (page_full(m, clip, pg)) {                       /* into the page clipboard (Delete: and gone) */
        q->pg_nn = q->pg_nl = 0;
        hi = rfmv_seq_clip_range(m, clip, &lo);
        for (i = lo; i < hi; i++)
            if (q->notes[i].step / RFM_MOVE_SEQ_PAGE == pg && q->pg_nn < sizeof q->pg_notes / sizeof q->pg_notes[0])
                q->pg_notes[q->pg_nn] = q->notes[i], q->pg_notes[q->pg_nn++].step -= (uint8_t)a;
        hi = rfmv_seq_clip_lock_range(m, clip, &lo);
        for (i = lo; i < hi; i++)
            if (q->locks[i].step / RFM_MOVE_SEQ_PAGE == pg && q->pg_nl < sizeof q->pg_locks / sizeof q->pg_locks[0])
                q->pg_locks[q->pg_nl] = q->locks[i], q->pg_locks[q->pg_nl++].step -= (uint8_t)a;
        q->pg_kind = (uint8_t)(del ? 2 : 1), q->pg_src = (uint8_t)pg, q->pg_clip = (uint8_t)clip;
        if (del) {
            rfmv_seq_group(m);
            for (i = a; i < a + RFM_MOVE_SEQ_PAGE; i++)
                rfmv_seq_step_clear(m, clip, i);
        }
        layer(q, LY_PAGE);
        return;
    }
    if (!q->pg_kind)
        return;
    rfmv_seq_group(m);                                  /* an empty page: the clipboard pasted */
    for (i = 0; i < q->pg_nn; i++) {
        const rfm_move_note_t *n = &q->pg_notes[i];
        rfmv_seq_note_put(m, clip, a + n->step, n->note, n->vel, n->len, n->micro);
    }
    for (i = 0; i < q->pg_nl; i++)
        rfmv_seq_lock_set(m, clip, a + q->pg_locks[i].step, q->pg_locks[i].cc, q->pg_locks[i].val);
    if (q->pg_kind == 2)
        q->pg_kind = 0;
    ensure_active(m);
}
int rfmv_seq_src_page(const rfm_move_t *m, int clip, int pg)   /* 1 copied, 2 cut, 0 */
{
    const rfm_move_seq_t *q = &m->seq;
    return q->pg_kind && q->pg_clip == clip && q->pg_src == pg ? q->pg_kind : 0;
}

/* the clipboard's source is marked (a step or a clip: dashed when copied, an X when cut) */
static int src_shown(const rfm_move_seq_t *q, int kind)
{
    return q->clip_kind == kind && (q->cut || q->paste || ((q->mods & SEQ_M_COPY) && q->copy_src));
}
int rfmv_seq_src_step(const rfm_move_t *m, int clip, int abs)   /* 1 copied, 2 cut, 0 */
{
    const rfm_move_seq_t *q = &m->seq;
    return src_shown(q, 1) && q->clip_src == clip && q->clip_step == abs ? 1 + q->cut : 0;
}
int rfmv_seq_src_clip(const rfm_move_t *m, int clip)
{
    const rfm_move_seq_t *q = &m->seq;
    return src_shown(q, 2) && q->clip_src == clip ? 1 + q->cut : 0;
}

/* the clipboard's step at (clip, abs); a cut (already gone from its source) is pasted once */
static void paste_step_at(rfm_move_t *m, int clip, int abs)
{
    rfm_move_seq_t *q = Q(m);
    const int was = rfmv_seq_clip_used(m, clip);
    rfmv_seq_group(m);
    rfmv_seq_paste_step(m, clip, abs);
    if (q->cut)
        q->cut = 0, q->clip_kind = 0;
    grow_to(m, clip, abs);
    entered(m, was);
}

/* ================================================================ steps === */
void rfmv_seq_step(rfm_move_t *m, int s, int down, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    const uint16_t bit = (uint16_t)(1u << s);
    const int clip = edit_clip(m), abs = q->page * RFM_MOVE_SEQ_PAGE + s;
    if (!down) {
        if (q->loop_first == s)
            q->loop_first = -1;
        if (q->tstep_held & (1u << s)) {                /* Track 4 held, a track's step: tap select, double tap
                                                         * solo, hold mute */
            q->tstep_held &= ~(1u << s);
            if (rfmv_now_ms(m) - q->step_t[s] >= RFM_MOVE_SEQ_HOLD_MS)
                rfmv_seq_mute(m, s, !q->tr[s].mute), q->tstep_tap = -1;
            else if (q->tstep_tap == s && now - q->tstep_tap_ms < 400)
                rfmv_seq_solo(m, s, !q->tr[s].solo), q->tstep_tap = -1;
            else {
                rfmv_menu(m, MENU_TRACK, s);
                rfmv_seq_select(m, s);
                q->tstep_tap = (int8_t)s, q->tstep_tap_ms = now;
            }
            return;
        }
        if (q->step_held & bit) {
            int lo;
            const int has = rfmv_seq_range(m, clip, abs, &lo) > lo;
            const int held = (q->step_popped && q->step_first == s) || rfmv_now_ms(m) - q->step_t[s] >= RFM_MOVE_SEQ_HOLD_MS;
            if (!((q->step_add | q->step_edit) & bit) && !held) {   /* a tap: on (the last notes) or off (Move) */
                if (has)
                    step_notes_clear(m, clip, abs);
                else {
                    uint8_t nn[RFM_MOVE_SEQ_POLY], vv[RFM_MOVE_SEQ_POLY];
                    const int n = last_notes(m, nn, vv), was = rfmv_seq_clip_used(m, clip);
                    int i, put = 0;
                    for (i = 0; i < n; i++)
                        put |= rfmv_seq_note_put(m, clip, abs, nn[i], vel_of(m, vv[i]), RFM_MOVE_SEQ_TPS, 0) == 0;
                    if (put)
                        rfmv_seq_tdef_apply(m, clip, abs, 1), grow_to(m, clip, abs), entered(m, was);
                }
            } else if (has)
                last_from_step(m, clip, abs);           /* entered or edited: its notes are the next empty step's */
            q->step_held &= (uint16_t)~bit, q->step_add &= (uint16_t)~bit, q->step_edit &= (uint16_t)~bit;
            if (q->step_first == s) {
                int i;
                q->step_first = -1;
                for (i = 0; i < RFM_MOVE_NSTEPS && q->step_first < 0; i++)
                    if (q->step_held >> i & 1)
                        q->step_first = (int8_t)i;
            }
            if (m->pop == POP_STEP && m->pop_i == s && q->step_popped) {   /* its popup (a hold's): the next
                                                         * held step's, or it drops */
                if (q->step_first >= 0)
                    rfmv_pop(m, POP_STEP, q->step_first);
                else
                    rfmv_pop_let(m, POP_STEP, s);
            }
            if (!q->step_held)
                q->step_popped = 0;
        }
        return;
    }
    if (q->projects) {
        if ((q->mods & SEQ_M_SHIFT) && s == 0)
            q->projects = 0, q->mods_used |= SEQ_M_SHIFT;
        return;
    }
    if (rfmv_seq_trk_held(m)) {                         /* a Track button held: steps are the 16 tracks */
        rfmv_menu(m, MENU_TRACK, s);                    /* (Track 4 used: its release is no tap) */
        if (q->mods & SEQ_M_COPY) {
            q->mods_used |= SEQ_M_COPY;
            if (!q->copy_src || q->clip_kind != 3) {
                q->clip_kind = 3, q->clip_src = (uint8_t)s, q->copy_src = 1, q->cut = 0;
            } else {
                rfmv_seq_group(m);
                rfmv_seq_track_copy(m, q->clip_src, s);
                q->clip_kind = 3, q->copy_pasted = 1;   /* (paste again elsewhere while Copy is held) */
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
        } else {                                        /* (its release decides: a tap selects, a double tap
                                                         * solos, a hold mutes) */
            q->tstep_held |= 1u << s;
            q->step_t[s] = rfmv_now_ms(m);
        }
        return;
    }
    if ((q->mods & SEQ_M_LOOP) && (q->mods & (SEQ_M_DELETE | SEQ_M_COPY)) && !q->view) {   /* Loop on: Delete / Copy
                                                         * work on pages as on steps (their own clipboard) */
        page_edit(m, clip, s, (q->mods & SEQ_M_DELETE) != 0);
        q->mods_used |= (uint8_t)(SEQ_M_LOOP | (q->mods & (SEQ_M_DELETE | SEQ_M_COPY)));
        return;
    }
    if ((q->mods & SEQ_M_COPY) && !q->view) {           /* Copy + step (the owner's, as Delete): a step with notes
                                                         * or locks is copied (dashed); an empty one gets the
                                                         * clipboard (a copy, or a Delete's cut) pasted */
        int lo, llo;
        q->mods_used |= SEQ_M_COPY;
        if (rfmv_seq_range(m, clip, abs, &lo) > lo || rfmv_seq_lock_range(m, clip, abs, &llo) > llo) {
            rfmv_seq_copy_step(m, clip, abs);
            q->clip_src = (uint8_t)clip, q->clip_step = (uint8_t)abs, q->copy_src = 1, q->paste = 0, layer(q, LY_CLIP);
        } else if (q->clip_kind == 1) {
            paste_step_at(m, clip, abs);
            q->copy_pasted = 1, q->paste = 0;
        }
        return;
    }
    if (q->mods & SEQ_M_SHIFT) {                        /* the shortcuts */
        q->mods_used |= SEQ_M_SHIFT;
        if (s == SH_TEMPO) {                            /* Shift + step 5: the tempo menu, and each press a tap (tap
                                                         * tempo) */
            rfmv_seq_tap(m, now);
            q->smenu = RFM_MOVE_SM_TEMPO;
            layer(q, LY_MENU);
            return;
        }
        if (sh_menu(s)) {                               /* a menu: open (again: closed) */
            q->smenu = (uint8_t)(q->smenu == sh_menu(s) ? 0 : sh_menu(s));
            q->arp_page = 0;
            if (q->smenu)
                layer(q, LY_MENU);
            rfmv_menu(m, MENU_NONE, 0);
            return;
        }
        switch (s) {
        case SH_PROJECTS: rfmv_proj_enter(m); break;    /* (projects mode: a mode of its own) */
        case SH_METRO:
            q->metro = (uint8_t)!q->metro;
            rfmv_seq_msg(m, q->metro ? SEQ_MSG_METRO_ON : SEQ_MSG_METRO_OFF);
            break;
        case SH_NEWCLIP: {                              /* the track's next empty slot: a new clip, selected */
            rfm_move_track_t *t = &q->tr[q->sel_track];
            int k;
            for (k = 1; k <= RFM_MOVE_SEQ_SCENES; k++) {
                const int sc2 = (t->sel + k) % RFM_MOVE_SEQ_SCENES;
                if (!rfmv_seq_clip_has(m, SEQ_CLIP(q->sel_track, sc2))) {
                    rfmv_seq_clip_make(m, SEQ_CLIP(q->sel_track, sc2), 1);
                    t->sel = (int8_t)sc2, q->page = 0;
                    rfmv_seq_msg(m, SEQ_MSG_NEWCLIP);
                    break;
                }
            }
            if (k > RFM_MOVE_SEQ_SCENES)
                rfmv_seq_msg(m, SEQ_MSG_NOCLIP);
            break;
        }
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
        default: break;
        }
        return;
    }
    if (q->mods & SEQ_M_MUTE) {                         /* Mute + step: track 1..16 */
        q->mods_used |= SEQ_M_MUTE;
        rfmv_seq_mute(m, s, !q->tr[s].mute);
        return;
    }
    if (q->mods & SEQ_M_LOOP) {                         /* Loop + step: the pages. A tap shows the page; two held
                                                         * loop the pages from one to the other; a double tap loops
                                                         * that page alone */
        q->mods_used |= SEQ_M_LOOP;
        if (q->loop_first >= 0 && q->loop_first != s) {
            const int a = q->loop_first < s ? q->loop_first : s, b = q->loop_first < s ? s : q->loop_first;
            rfmv_seq_group(m);
            rfmv_seq_loop_set(m, clip, a * RFM_MOVE_SEQ_PAGE, (b - a + 1) * RFM_MOVE_SEQ_PAGE);
            if (q->page < a || q->page > b)
                q->page = (uint8_t)a;                   /* (the page shown: in the new loop) */
            q->loop_tap = -1;
        } else {
            if (q->loop_tap == s && now - q->loop_tap_ms < 400) {
                rfmv_seq_group(m);
                rfmv_seq_loop_set(m, clip, s * RFM_MOVE_SEQ_PAGE, RFM_MOVE_SEQ_PAGE);
                q->loop_tap = -1;
            } else
                q->loop_tap = (int8_t)s, q->loop_tap_ms = now;
            q->page = (uint8_t)s, q->loop_first = (int8_t)s;
        }
        rfmv_pop(m, POP_LOOP, 0);
        return;
    }
    if (q->view) {                                      /* session: the 2 steps under a track's column: stop its
                                                         * clips, mute it */
        const int track = q->track_page + s / 2;
        if (s & 1)
            rfmv_seq_mute(m, track, !q->tr[track].mute);
        else if (q->tr[track].active >= 0 || q->tr[track].queued >= 0)
            rfmv_seq_launch(m, track, -1);              /* (at the next bar; stopped: at once) */
        rfmv_seq_dirty(m);
        return;
    }
    if (q->mods & SEQ_M_DELETE) {                       /* Delete + step (the owner's cut): a step with notes or
                                                         * locks is cut (gone, kept in the clipboard, an X on it); an
                                                         * empty one gets the clipboard pasted */
        int lo, llo;
        const int full = rfmv_seq_range(m, clip, abs, &lo) > lo || rfmv_seq_lock_range(m, clip, abs, &llo) > llo;
        q->mods_used |= SEQ_M_DELETE;
        if (full) {
            rfmv_seq_group(m);
            rfmv_seq_copy_step(m, clip, abs);
            q->cut = 1, q->paste = 0, q->clip_src = (uint8_t)clip, q->clip_step = (uint8_t)abs, layer(q, LY_CLIP);
            rfmv_seq_step_clear(m, clip, abs);
        } else if (q->clip_kind == 1)
            paste_step_at(m, clip, abs);
        return;
    }
    if (q->paste && q->clip_kind == 1 && !q->step_held) {   /* a copy waiting (Copy released): pasted here */
        paste_step_at(m, clip, abs);
        q->paste = 0;
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
    q->step_t[s] = rfmv_now_ms(m);
    if (q->step_first < 0)
        q->step_first = (int8_t)s, q->step_popped = 0;
    {
        const int was = rfmv_seq_clip_used(m, clip);
        int lo, hi = rfmv_seq_range(m, clip, abs, &lo), p, put = 0;
        if (q->chord) {                                 /* pads held: the step gets exactly those notes */
            const int e = hi == lo;
            step_notes_clear(m, clip, abs);
            q->nlast = 0;
            for (p = 0; p < RFM_MOVE_NPADS; p++)
                if ((q->chord >> p & 1) && m->pad[p].kind == H_NOTE) {
                    const int v = vel_of(m, q->pad_vel[p]);
                    put |= rfmv_seq_note_put(m, clip, abs, m->pad[p].b, v, RFM_MOVE_SEQ_TPS, 0) == 0;
                    last_add(q, m->pad[p].b, v);
                }
            q->step_add |= bit;
            if (put && e)
                rfmv_seq_tdef_apply(m, clip, abs, 1);
            if (put)
                grow_to(m, clip, abs), entered(m, was);
        } else if (hi > lo)
            rfmv_seq_preview(m, clip, abs);             /* a step with notes: heard (a tap clears it at the release,
                                                         * an empty one is filled then: holding it leaves it for the
                                                         * pads) */
    }
}

/* (advance) the first step held long enough: its popup */
void rfmv_seq_ui_poll(rfm_move_t *m)
{
    rfm_move_seq_t *q = Q(m);
    if (q->step_first >= 0 && !q->step_popped && m->s.mode == RFM_MOVE_MODE_SEQ && !q->view && !q->projects &&
        rfmv_now_ms(m) - q->step_t[q->step_first] >= RFM_MOVE_SEQ_HOLD_MS) {
        q->step_popped = 1;
        rfmv_pop(m, POP_STEP, q->step_first);
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
    rfmv_pop(m, POP_TRIG, 7);                           /* (a VELOCITY popup) */
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
            if (cc == MR_CC_LOOP && (q->mods & SEQ_M_SHIFT)) {   /* Shift + Loop held: FILL (the owner's; Double
                                                         * Loop is Shift + step 15) */
                q->mods_used |= SEQ_M_SHIFT;
                q->fill = 1, q->loop_end = 1;           /* (its release: FILL off, nothing else) */
                return 1;
            }
            if (mb == SEQ_M_SHIFT) {                    /* a double tap latches Shift (a tap, then a press within
                                                         * 300 ms); a press unlatches it */
                if (q->shift_lock)
                    q->shift_lock = 0, q->shift_ms = 0, q->mods_used |= SEQ_M_SHIFT;
                else if (q->shift_ms && now - q->shift_ms < 300)
                    q->shift_lock = 1, q->shift_ms = 0, layer(q, LY_SHIFT);
            }
            if (mb == SEQ_M_LOOP && q->loop_latch) {   /* latched: this press ends it, at once (as Back) */
                layer_close(m, LY_LOOP);
                q->loop_end = 1;                        /* (its release: nothing) */
                return 1;
            }
            q->mods |= (uint8_t)mb, q->mods_used &= (uint8_t)~mb;
            if (mb == SEQ_M_LOOP && !q->view && !q->projects)
                rfmv_pop(m, POP_LOOP, 0);               /* Loop held: the clip's length */
            if (mb == SEQ_M_COPY)
                q->copy_src = 0, q->copy_pasted = 0;
        } else if (mb == SEQ_M_SHIFT && q->shift_lock) {
            ;                                           /* (latched: it stays down) */
        } else if (mb == SEQ_M_LOOP && q->loop_end) {
            q->loop_end = 0, q->fill = 0;               /* (the press that ended the latch, or FILL's) */
        } else if (mb == SEQ_M_LOOP && !(q->mods_used & SEQ_M_LOOP) && !q->loop_latch && !q->view && !q->projects) {
            q->loop_latch = 1, layer(q, LY_LOOP);       /* a tap on Loop: latched (the jog sets the length) until
                                                         * Loop or Back again */
            rfmv_pop(m, POP_LOOP, 0);
        } else if (q->mods & mb) {
            const int used = q->mods_used & mb;
            if (mb == SEQ_M_SHIFT)
                q->shift_ms = used ? 0 : (now ? now : 1);   /* (a tap: the first of a double tap) */
            if (mb == SEQ_M_LOOP)
                rfmv_pop_let(m, POP_LOOP, 0);
            const int alone = !used && !q->projects && !(q->mods & SEQ_M_SHIFT) && !q->step_held &&
                              !rfmv_seq_trk_held(m);
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
    if (rfmv_seq_trk_held(m) && (cc == MR_CC_UP || cc == MR_CC_DOWN)) {   /* a Track button held: the track's rate
                                                         * (Up: faster) */
        rfm_move_track_t *t = &q->tr[q->sel_track];
        const int r = clampi(t->rate + (cc == MR_CC_UP ? -1 : 1), 0, RFM_MOVE_SEQ_RATES - 1);
        rfmv_menu(m, MENU_RATE, r);
        if (r != t->rate) {
            t->rate = (uint8_t)r;                       /* (from the next step) */
            rfmv_seq_dirty(m);
        }
        return 1;
    }
    if (held_view(m)) {                                 /* a step held: the arrows edit its notes (Move) */
        switch (cc) {
        case MR_CC_LEFT:
        case MR_CC_RIGHT:
            held_nudge(m, (cc == MR_CC_RIGHT ? 1 : -1) * ((q->mods & SEQ_M_SHIFT) ? 1 : NUDGE));
            rfmv_pop(m, POP_TRIG, 0);                   /* (the TRIG page's MICRO popup) */
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
        else if (q->smenu)
            layer_close(m, LY_MENU);                    /* (a Shift menu: Session closes it, as Back) */
        else
            q->view = (uint8_t)!q->view;
        return 1;
    case MR_CC_BACK:                                    /* the universal escape: the latest layer still on closes
                                                         * (a Shift menu, the project grid, Loop's latch, Shift's
                                                         * lock, the clipboard's marks), one a press; none: nothing */
        while (q->nlayer) {
            const int id = q->layer[--q->nlayer];
            if (layer_on(q, id)) {
                layer_close(m, id);
                return 1;
            }
        }
        {
            int id;
            for (id = LY_MENU; id <= LY_PAGE; id++)        /* (one opened some other way) */
                if (layer_on(q, id)) {
                    layer_close(m, id);
                    return 1;
                }
        }
        return 1;                                       /* (nothing to close: nothing. Only a Track tap changes the
                                                         * mode, the owner's) */
    case MR_CC_PLAY:
        q->ncap = 0;                                    /* (Move: the transport clears the capture) */
        if ((q->mods & SEQ_M_SHIFT) && q->view) {       /* session: Shift + Play: every clip stops (at the bar) */
            int t;
            q->mods_used |= SEQ_M_SHIFT;
            for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
                if (q->tr[t].active >= 0 || q->tr[t].queued >= 0)
                    rfmv_seq_launch(m, t, -1);
        } else if (q->mods & SEQ_M_SHIFT) {             /* Shift + Play: every clip from its start (Move) */
            q->mods_used |= SEQ_M_SHIFT;
            rfmv_seq_restart(m);
        } else if (running(q))
            rfmv_seq_stop(m);
        else if (q->rec && q->rec_mode == RFM_MOVE_RM_RECORD)
            rfmv_seq_count_in(m);                       /* (Rec armed while stopped: Play gives the bar's count-in) */
        else
            rfmv_seq_start(m);
        return 1;
    case MR_CC_REC:
        if (q->mods & SEQ_M_SHIFT) {                    /* Shift + Record: the record settings */
            q->mods_used |= SEQ_M_SHIFT;
            q->smenu = (uint8_t)(q->smenu == RFM_MOVE_SM_REC ? 0 : RFM_MOVE_SM_REC);
            if (q->smenu)
                layer(q, LY_MENU);
            return 1;
        }
        if (q->rec_mode == RFM_MOVE_RM_STEP) {          /* Step: the cursor on (at the page's first step) or off */
            q->steprec = (uint8_t)!q->steprec;
            if (q->steprec) {
                const int clip = edit_clip(m), lo = rfmv_seq_loop_lo(m, clip), hi = rfmv_seq_loop_hi(m, clip);
                const int a = q->page * RFM_MOVE_SEQ_PAGE;
                q->step_cur = (int16_t)(a >= lo && a < hi ? a : lo);
                q->steps_chord = 0;
            }
            return 1;
        }
        if (q->rec_mode == RFM_MOVE_RM_CAPTURE && (q->srec || (!q->rec && !running(q)))) {
            rfmv_seq_sample(m);                         /* Capture, stopped: armed; recording: done at the bar */
            return 1;
        }
        if (!q->rec) {
            if (!++q->take)
                q->take = 1;
            q->rec = 1;
            rfmv_seq_group(m);
            q->rec_group = q->group;
            /* (stopped: armed; Play starts it with a bar's count-in. Only Play starts the transport, the owner's) */
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
            rfmv_seq_msg(m, SEQ_MSG_CAPTURED);          /* (stopped: the new clip plays with Play) */
        } else
            rfmv_seq_msg(m, SEQ_MSG_NOCLIP);
        return 1;
    case MR_CC_SAMPLE:
        if (q->mods & SEQ_M_SHIFT)
            q->mods_used |= SEQ_M_SHIFT;                /* (Shift + Sample: nothing; the Move has none) */
        else
            rfmv_seq_audio_rec(m);                      /* the audio loop: armed, recording, done at the next bar */
        return 1;
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
        if (q->view)                                    /* the tracks, one a press (8 of 16 in view) */
            q->track_page = (uint8_t)clampi(q->track_page + d, 0, RFM_MOVE_SEQ_TRACKS - 8);
        else {
            const int clip = edit_clip(m), a = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE;
            const int pages = (rfmv_seq_loop_hi(m, clip) + RFM_MOVE_SEQ_PAGE - 1) / RFM_MOVE_SEQ_PAGE, was = q->page;
            if (q->mods & SEQ_M_LOOP) {                 /* Loop's page edit (held or latched): the pages; Right
                                                         * past the last a pending (ghost) page (+1, +2 ..: the clip
                                                         * grows to it when a step goes in there; Left steps back) */
                q->mods_used |= SEQ_M_LOOP;
                q->page = (uint8_t)clampi(q->page + d, q->page < a ? 0 : a, RFM_MOVE_SEQ_PAGES - 1);
                if (q->page != was && (q->page >= pages || was >= pages))
                    rfmv_pop(m, POP_BAR, q->page);      /* (a ghost page added or taken away: "BAR n", a second) */
            } else {                                    /* otherwise (the owner's): the loop's pages, round, never
                                                         * a ghost */
                const int n = pages - a;
                q->page = (uint8_t)(n > 0 ? a + ((q->page < a || q->page >= pages ? (d > 0 ? n - 1 : 0) : q->page - a) + d + n) % n
                                          : a);
            }
        }
        return 1;
    }
    case MR_CC_UP:
    case MR_CC_DOWN: {
        const int d = cc == MR_CC_UP ? 1 : -1;
        if (q->view)                                    /* the clip slots (4 of 16 in view; Up: the ones above) */
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

/* the TRIG page: knob k's value of a step (shown 0 none: micro 0, ratchet 1, chance 100, cond / func none) */
int rfmv_seq_trig_val(const rfm_move_t *m, int clip, int abs, int k)
{
    int v, lo, hi;
    switch (k) {
    case 0:                                             /* micro: the step's first note's */
        hi = rfmv_seq_range(m, clip, abs, &lo);
        return hi > lo ? m->seq.notes[lo].micro : 0;
    case 1: return (v = rfmv_seq_trig(m, clip, abs, RFM_MOVE_TRIG_RATCHET)) < 0 ? 1 : v;
    case 2: return (v = rfmv_seq_trig(m, clip, abs, RFM_MOVE_TRIG_CHANCE)) < 0 ? 100 : v;
    case 3: return (v = rfmv_seq_trig(m, clip, abs, RFM_MOVE_TRIG_COND)) < 0 ? 0 : v;
    case 4: return (v = rfmv_seq_trig(m, clip, abs, RFM_MOVE_TRIG_FUNC)) < 0 ? 0 : v;
    case 5: {
        const int f = rfmv_seq_trig_val(m, clip, abs, 4);
        if (f == RFM_MOVE_FN_LEAP)
            return (v = rfmv_seq_trig(m, clip, abs, RFM_MOVE_TRIG_LEAP)) < 0 ? 2 : v - 15;
        if (f == RFM_MOVE_FN_JUMP)
            return (v = rfmv_seq_trig(m, clip, abs, RFM_MOVE_TRIG_JUMP)) < 0 ? rfmv_seq_loop_lo(m, clip) : v;
        return 0;
    }
    case 6:                                             /* length: the step's first note's, ticks (24 a step) */
        hi = rfmv_seq_range(m, clip, abs, &lo);
        return hi > lo ? m->seq.notes[lo].len : RFM_MOVE_SEQ_TPS;
    case 7:                                             /* (held steps' volume knob) velocity */
        hi = rfmv_seq_range(m, clip, abs, &lo);
        return hi > lo ? m->seq.notes[lo].vel : 0;
    default: return 0;
    }
}
/* the TRIG page's lengths (Elektron's): 1/8 .. 64 steps, then INF (the longest a note can be) */
static const uint16_t LENS[] = { 3, 6, 12, 18, 24, 36, 48, 72, 96, 144, 192, 288, 384, 576, 768, 1536,
                                 RFM_MOVE_SEQ_MAXLEN * RFM_MOVE_SEQ_TPS };
#define NLENS ((int)(sizeof LENS / sizeof LENS[0]))
int rfmv_seq_len_index(int len)                         /* the nearest entry */
{
    int i, b = 0;
    for (i = 1; i < NLENS; i++)
        if ((len > LENS[i] ? len - LENS[i] : LENS[i] - len) < (len > LENS[b] ? len - LENS[b] : LENS[b] - len))
            b = i;
    return b;
}
int rfmv_seq_len_of(int i) { return LENS[clampi(i, 0, NLENS - 1)]; }
typedef char lens_n_check[NLENS == RFM_MOVE_TRIG_NLENS ? 1 : -1];

/* ---- the TRIG page's defaults (the owner's): with no step held its knobs set what new steps get, per track (saved
 * with the project); steps already there keep theirs */
void rfmv_seq_tdef_default(uint8_t d[8])
{
    d[0] = 12, d[1] = 1, d[2] = 100, d[3] = 0, d[4] = 0, d[5] = 15 + 2, d[6] = 0, d[7] = 4;   /* (length: 24, a step) */
}
int rfmv_seq_tdef_val(const rfm_move_t *m, int k)
{
    const uint8_t *d = m->seq.tr[m->seq.sel_track].tdef;
    switch (k) {
    case 0: return d[0] - 12;
    case 1: return d[1];
    case 2: return d[2];
    case 3: return d[3];
    case 4: return d[4];
    case 5: return d[4] == RFM_MOVE_FN_LEAP ? d[5] - 15 : d[4] == RFM_MOVE_FN_JUMP ? d[6] : 0;
    case 6: return LENS[d[7] < NLENS ? d[7] : 4];
    default: return 0;
    }
}
static void tdef_turn(rfm_move_t *m, int k, int d)
{
    uint8_t *t = m->seq.tr[m->seq.sel_track].tdef;
    switch (k) {
    case 0: t[0] = (uint8_t)(clampi(t[0] - 12 + d, -RFM_MOVE_SEQ_TPS / 2, RFM_MOVE_SEQ_TPS / 2 - 1) + 12); break;
    case 1: t[1] = (uint8_t)clampi(t[1] + d, 1, 8); break;
    case 2: t[2] = (uint8_t)clampi(t[2] + d, 1, 100); break;
    case 3: t[3] = (uint8_t)clampi(t[3] + d, 0, RFM_MOVE_COND_N - 1); break;
    case 4: t[4] = (uint8_t)clampi(t[4] + d, 0, RFM_MOVE_FN_N - 1); break;
    case 5:
        if (t[4] == RFM_MOVE_FN_LEAP) {
            int n = clampi(t[5] - 15 + d, -15, 15);
            if (!n)
                n = d > 0 ? 1 : -1;                     /* (no LEAP 0) */
            t[5] = (uint8_t)(n + 15);
        } else if (t[4] == RFM_MOVE_FN_JUMP)
            t[6] = (uint8_t)clampi(t[6] + d, 0, RFM_MOVE_SEQ_MAXLEN - 1);
        break;
    case 6: t[7] = (uint8_t)clampi(t[7] + d, 0, NLENS - 1); break;
    default: break;
    }
    rfmv_seq_dirty(m);
}
/* a step that was empty got its first note: the track's default trigs as its locks (entry: its notes' micro and
 * length too; a recording keeps the played ones) */
void rfmv_seq_tdef_apply(rfm_move_t *m, int clip, int abs, int entry)
{
    const uint8_t *t = m->seq.tr[clip / RFM_MOVE_SEQ_SCENES].tdef;
    if (t[1] > 1)
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_RATCHET, t[1]);
    if (t[2] < 100)
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_CHANCE, t[2]);
    if (t[3])
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_COND, t[3]);
    if (t[4]) {
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_FUNC, t[4]);
        if (t[4] == RFM_MOVE_FN_LEAP)
            rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_LEAP, t[5]);
        else if (t[4] == RFM_MOVE_FN_JUMP)
            rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_JUMP, t[6]);
    }
    if (entry && (t[0] != 12 || t[7] != 4)) {
        int lo, hi = rfmv_seq_range(m, clip, abs, &lo), i, n = hi - lo;
        rfm_move_note_t nn[RFM_MOVE_SEQ_POLY];
        for (i = 0; i < n && i < RFM_MOVE_SEQ_POLY; i++)
            nn[i] = m->seq.notes[lo + i];
        for (i = 0; i < n && i < RFM_MOVE_SEQ_POLY; i++)
            rfmv_seq_note_put(m, clip, abs, nn[i].note, nn[i].vel, LENS[t[7] < NLENS ? t[7] : 4], t[0] - 12);
    }
}

int rfmv_seq_len_inf(int len) { return len >= LENS[NLENS - 1]; }

static void trig_turn(rfm_move_t *m, int clip, int abs, int k, int d)
{
    const int v = rfmv_seq_trig_val(m, clip, abs, k);
    int lo, hi, i;
    switch (k) {
    case 0:                                             /* micro: every note of the step, a tick a detent */
        hi = rfmv_seq_range(m, clip, abs, &lo);
        for (i = lo; i < hi; i++) {
            const rfm_move_note_t n = m->seq.notes[i];
            rfmv_seq_note_put(m, clip, abs, n.note, n.vel, n.len, clampi(n.micro + d, -RFM_MOVE_SEQ_TPS / 2, RFM_MOVE_SEQ_TPS / 2 - 1));
        }
        break;
    case 1: {
        const int r = clampi(v + d, 1, 8);
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_RATCHET, r == 1 ? -1 : r);
        break;
    }
    case 2: {
        const int c = clampi(v + d, 1, 100);
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_CHANCE, c == 100 ? -1 : c);
        break;
    }
    case 3: {
        const int c = clampi(v + d, 0, RFM_MOVE_COND_N - 1);
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_COND, c ? c : -1);
        break;
    }
    case 4: {
        const int f = clampi(v + d, 0, RFM_MOVE_FN_N - 1);
        rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_FUNC, f ? f : -1);
        break;
    }
    case 6: {                                           /* length: every note of the step, through the table */
        const int L = rfmv_seq_len_of(rfmv_seq_len_index(v) + d);
        hi = rfmv_seq_range(m, clip, abs, &lo);
        for (i = lo; i < hi; i++) {
            const rfm_move_note_t n = m->seq.notes[i];
            rfmv_seq_note_put(m, clip, abs, n.note, n.vel, L, n.micro);
        }
        break;
    }
    case 5: {
        const int f = rfmv_seq_trig_val(m, clip, abs, 4);
        if (f == RFM_MOVE_FN_LEAP) {
            int n = clampi(v + d, -15, 15);
            if (!n)
                n = d > 0 ? 1 : -1;                     /* (no LEAP 0) */
            rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_LEAP, n + 15);
        } else if (f == RFM_MOVE_FN_JUMP)
            rfmv_seq_lock_set(m, clip, abs, RFM_MOVE_TRIG_JUMP, clampi(v + d, 0, RFM_MOVE_SEQ_MAXLEN - 1));
        break;
    }
    default: break;
    }
}

/* a Shift menu's values: knob k (jog: k -1, the menu's first value) */
/* a Shift menu's knob that picks from a list (a scale, a root, a layout, a rate, a pattern, a mode ..): turned a
 * slower one item a few ticks, the same for every menu (rfmv_list_ticks); numbers (tempo, groove, ..) as before */
static int smenu_list(const rfm_move_seq_t *q, int k)
{
    switch (q->smenu) {
    case RFM_MOVE_SM_WORKFLOW: return k == 0 || k == 1;
    case RFM_MOVE_SM_GROOVE: return k == 1;
    case RFM_MOVE_SM_LAYOUT: return k == 0;
    case RFM_MOVE_SM_KEYS: return k <= 3;
    case RFM_MOVE_SM_ARP:
        if (q->arp_page)
            return (k == 0 && q->arp_page == 1) || k == 1 || k == 2;
        return k == 0 || k == 1 || k == 2 || k == 4 || k == 7;
    case RFM_MOVE_SM_REC: return k <= 1;
    case RFM_MOVE_SM_GLOBAL: return k <= 1;
    default: return 0;
    }
}

/* the knobs a Shift menu uses (bit k: knob k + 1; their ring LEDs lit in the menu colour, the others off) */
int rfmv_seq_menu_knobs(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    switch (q->smenu) {
    case RFM_MOVE_SM_WORKFLOW: return 0x07;
    case RFM_MOVE_SM_TEMPO: return q->following ? 0 : 0x03;
    case RFM_MOVE_SM_GROOVE: return 0x03;
    case RFM_MOVE_SM_LAYOUT: return 0x01;
    case RFM_MOVE_SM_KEYS: return 0x0F;
    case RFM_MOVE_SM_ARP: return q->arp_page ? 0x0F : 0xFF;
    case RFM_MOVE_SM_REC: return 0x03;
    case RFM_MOVE_SM_VEL: return 0x01;
    case RFM_MOVE_SM_GLOBAL: return 0x07;
    default: return 0;
    }
}

static void smenu_turn(rfm_move_t *m, int k, int d)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_track_t *t = &q->tr[q->sel_track];
    rfm_move_arp_t *a = &t->arp;
    if (k >= 0 && smenu_list(q, k) && !(d = rfmv_list_ticks(m, k, d)))
        return;                                         /* (not a whole item yet) */
    rfmv_menu(m, MENU_NONE, k < 0 ? 0 : k);
    switch (q->smenu) {
    case RFM_MOVE_SM_WORKFLOW:                          /* channel, rate, quantize amount */
        if (k <= 0) {
            const int ch = clampi(t->ch + d, 0, 15);
            if (ch != t->ch)
                rfmv_seq_release(m, q->sel_track), t->ch = (uint8_t)ch;
        } else if (k == 1)
            t->rate = (uint8_t)clampi(t->rate + d, 0, RFM_MOVE_SEQ_RATES - 1);
        else if (k == 2)
            q->qstrength = (uint8_t)clampi(q->qstrength + d * 5, 0, 100);
        break;
    case RFM_MOVE_SM_TEMPO:                             /* (following the firmware's clock: locked) */
        if (!q->following && k <= 1)                    /* (Move: Shift + the wheel, or knob 2: fine) */
            rfm_move_seq_set_bpm(m, q->bpm10 + d * (k == 1 || (q->mods & SEQ_M_SHIFT) ? 1 : 10));
        break;
    case RFM_MOVE_SM_GROOVE:                            /* the jog: the amount; knob 2: GLOBAL (the project's) or
                                                         * TRACK (its own) */
        if (k <= 0) {                                   /* (Move: Shift fine; ours: 5 % a detent, Shift 1 %) */
            uint8_t *g = (t->own & 1) ? &t->groove : &q->groove;
            *g = (uint8_t)clampi(*g + d * ((q->mods & SEQ_M_SHIFT) ? 1 : 5), 0, 150);
        } else if (k == 1 && (d > 0) != !!(t->own & 1)) {
            if (d > 0)
                t->groove = q->groove;                  /* (its own starts from the global one) */
            t->own = (uint8_t)(d > 0 ? t->own | 1 : t->own & ~1);
        }
        break;
    case RFM_MOVE_SM_LAYOUT:
        if (k <= 0) {
            const int l = clampi(m->s.layout + d, 0, 3);
            if (l != m->s.layout)
                m->s.layout = (uint8_t)l, rfmv_changed(m);
        }
        break;
    case RFM_MOVE_SM_KEYS:                              /* the root (jog, knob 1), the scale (knob 2) */
        if (k <= 0) {
            m->s.root = (uint8_t)(((m->s.root + d) % 12 + 12) % 12);
            rfmv_changed(m);
        } else if (k == 1) {
            const int sc2 = clampi(m->s.scale + d, 0, RFM_MOVE_NSCALES - 1);
            if (sc2 != m->s.scale)
                m->s.scale = (uint8_t)sc2, rfmv_changed(m);
        } else if (k == 2 && (d > 0) != !!(t->own & 2)) {   /* knob 3: GLOBAL or TRACK (its own key and scale) */
            rfmv_seq_sync(m);
            if (d > 0)
                t->root = q->root, t->scale = q->scale;
            t->own = (uint8_t)(d > 0 ? t->own | 2 : t->own & ~2);
            load_track(m);
            rfmv_changed(m);
        } else if (k == 3) {                            /* knob 4: the layout (the pads play, the owner's) */
            const int l = clampi(m->s.layout + d, 0, 3);
            if (l != m->s.layout)
                m->s.layout = (uint8_t)l, rfmv_changed(m);
        }
        rfmv_seq_sync(m);
        break;
    case RFM_MOVE_SM_ARP:                               /* FIZZ's ARP: the jog the page; page 1: PATTERN RATE RANGE
                                                         * CHANCE MODE GATE OCT DIST QUANT; pages 2, 3: the MOD and
                                                         * TRANSPOSE lanes (knob 3 picks a lane step; knob 1 its
                                                         * value, knob 2 RESET, knob 4 the lane's length) */
        if (k < 0) {
            q->arp_page = (uint8_t)clampi(q->arp_page + d, 0, 2);
            break;
        }
        if (q->arp_page) {
            const int L = q->arp_page - 1;
            if (k == 0)
                a->lane[L][q->arp_sel & 15] = (int8_t)(L ? clampi(a->lane[L][q->arp_sel & 15] + d, -48, 48)
                                                         : clampi(a->lane[L][q->arp_sel & 15] + d, 0, RFM_MOVE_AL_N - 1));
            else if (k == 1)
                a->reset = (uint8_t)clampi(a->reset + d, 0, RFM_MOVE_AR_N - 1);
            else if (k == 2)                            /* knob 3: the lane step (the steps play, the owner's) */
                q->arp_sel = (uint8_t)clampi(q->arp_sel + d, 0, RFM_MOVE_ARP_LANE - 1);
            else if (k == 3) {
                uint8_t *len = L ? &a->tlen : &a->mlen;
                *len = (uint8_t)clampi(*len + d, 1, 16);
            }
            break;
        }
        switch (k) {
        case 0: a->pat = (uint8_t)clampi(a->pat + d, 0, RFM_MOVE_AP_N - 1); break;
        case 1: a->rate = (uint8_t)clampi(a->rate + d, 0, RFM_MOVE_ARP_RATES - 1); break;
        case 2: a->range = (uint8_t)clampi(a->range + d, 1, 8); break;
        case 3: a->chance = (uint8_t)clampi(a->chance + d * 5, 0, 100); break;
        case 4:
            a->mode = (uint8_t)clampi(a->mode + d, 0, RFM_MOVE_AM_N - 1);
            if (a->mode == RFM_MOVE_AM_OFF)
                rfmv_seq_arp_stop(m);
            break;
        case 5: a->gate = (uint8_t)clampi(a->gate + d * 5, 2, 200); break;
        case 6: a->odist = (int8_t)clampi(a->odist + d, -24, 24); break;
        case 7: a->quant = (uint8_t)clampi(a->quant + d, 0, 16); break;
        default: break;
        }
        break;
    case RFM_MOVE_SM_GLOBAL:                            /* the jog or knob 1: the clock source; knob 2: the velocity
                                                         * option; knob 3: its level */
        if (k <= 0)
            rfm_move_seq_set_clock(m, clampi(q->clock_mode + d, 0, 2));
        else if (k == 1)
            rfmv_vel_step(m, d), rfmv_changed(m);
        else if (k == 2)
            m->s.vel_level = (uint8_t)clampi(m->s.vel_level + d, 1, 127), rfmv_changed(m);
        break;
    case RFM_MOVE_SM_VEL:                               /* the jog: the option (1:1, five); knob 1: the level */
        if (k < 0)
            rfmv_vel_step(m, d);
        else if (k == 0)
            m->s.vel_level = (uint8_t)clampi(m->s.vel_level + d, 1, 127);
        rfmv_changed(m);
        break;
    case RFM_MOVE_SM_REC:                               /* mode, style */
        if (k <= 0)
            q->rec_mode = (uint8_t)clampi(q->rec_mode + d, 0, RFM_MOVE_RM_N - 1), q->steprec = 0;
        else if (k == 1)
            q->rec_style = (uint8_t)clampi(q->rec_style + d, 0, RFM_MOVE_RS_N - 1);
        break;
    default: break;
    }
    rfmv_seq_dirty(m);
}

void rfmv_seq_knob(rfm_move_t *m, int k, int d, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    const int b = m->s.bank, cc = m->s.cc[b][k];
    (void)now;
    if (q->view && !rfmv_seq_trk_held(m))
        return;                                         /* (session: the knobs do nothing; a touch selects a track) */
    if (rfmv_seq_trk_held(m)) {                         /* a Track button held: the knob's CC number (this bank) */
        const int c = clampi(cc + d, 0, 127);
        rfmv_menu(m, MENU_CC, k);
        if (c != cc) {
            m->s.cc[b][k] = (uint8_t)c;
            rfmv_changed(m);
        }
        return;
    }
    if (q->smenu) {                                     /* a Shift menu open: its values */
        smenu_turn(m, k, d);
        return;
    }
    if (q->trig && !q->view && !q->projects) {          /* the TRIG page: the held steps' (none held: every step
                                                         * with notes in the clip) */
        const int clip = edit_clip(m);
        int s;
        rfmv_pop(m, POP_TRIG, k);
        if (k > 6)
            return;
        if ((k == 3 || k == 4) && !(d = rfmv_list_ticks(m, k, d)))
            return;                                     /* (COND, FUNC: lists, one item a few ticks) */
        if (!q->step_held) {                            /* none held: the track's defaults for new steps (the
                                                         * owner's: steps already there keep theirs) */
            tdef_turn(m, k, d);
            return;
        }
        if (q->step_held) {
            for (s = 0; s < RFM_MOVE_NSTEPS; s++)
                if (q->step_held >> s & 1) {
                    trig_turn(m, clip, q->page * RFM_MOVE_SEQ_PAGE + s, k, d);
                    q->step_edit |= (uint16_t)(1u << s);
                }
        } else {
            int lo, hi = rfmv_seq_clip_range(m, clip, &lo), prev = -1, i;
            uint8_t steps[RFM_MOVE_SEQ_MAXLEN];
            int ns = 0;
            for (i = lo; i < hi; i++)
                if (q->notes[i].step != prev)
                    steps[ns++] = (uint8_t)(prev = q->notes[i].step);
            for (i = 0; i < ns; i++)
                trig_turn(m, clip, steps[i], k, d);
        }
        return;
    }
    rfmv_pop(m, POP_CC, k);
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
    }
}

void rfmv_seq_jog(rfm_move_t *m, int d, uint32_t now)
{
    rfm_move_seq_t *q = Q(m);
    (void)now;
    if (rfmv_seq_trk_held(m)) {                         /* a Track button held: the selected track's channel */
        rfm_move_track_t *t = &q->tr[q->sel_track];
        const int ch = clampi(t->ch + d, 0, 15);
        rfmv_menu(m, MENU_TCH, ch);
        if (ch != t->ch) {
            rfmv_seq_release(m, q->sel_track);
            t->ch = (uint8_t)ch;
            rfmv_seq_dirty(m);
        }
        return;
    }
    if (q->smenu) {                                     /* a Shift menu: its first value */
        smenu_turn(m, -1, d);
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
        rfmv_pop(m, POP_TRIG, 6);                       /* (the TRIG page's LENGTH popup) */
        return;
    }
    if (q->mods & SEQ_M_LOOP) {                         /* Loop held (or latched): the clip's length (Move's), a
                                                         * step a detent (Shift: a page) */
        const int clip = edit_clip(m), unit = (q->mods & SEQ_M_SHIFT) ? RFM_MOVE_SEQ_PAGE : 1;
        const int L = q->len[clip], nl = L + d * unit;
        if (!(q->mods_used & SEQ_M_LOOP))
            rfmv_seq_group(m);
        q->mods_used |= SEQ_M_LOOP | ((q->mods & SEQ_M_SHIFT) ? SEQ_M_SHIFT : 0);
        rfmv_seq_len_set(m, clip, clampi(nl, unit, RFM_MOVE_SEQ_MAXLEN));
        rfmv_pop(m, POP_LOOP, 0);
        m->pop_val = 1;                                 /* (the jog: Move's "Loop Length") */
        return;
    }
    if (q->mods & SEQ_M_SHIFT) {                        /* Shift + jog: the tempo, 1 BPM a detent */
        q->mods_used |= SEQ_M_SHIFT;
        if (q->following)
            rfmv_seq_msg(m, SEQ_MSG_EXTTAP);
        else {
            rfm_move_seq_set_bpm(m, q->bpm10 + d * 10);
            rfmv_pop(m, POP_TEMPO, 0);
        }
        return;
    }
    if (m->s.layout == RFM_MOVE_LAYOUT_DRUM && m->edit_pad >= 0 && m->pad_down[m->edit_pad] && !q->view) {
        const int p = m->edit_pad, n = clampi(m->s.drum[p] + d, 0, 127);
        if (n != m->s.drum[p]) {
            m->s.drum[p] = (uint8_t)n;
            rfmv_changed(m);
        }
        rfmv_pop(m, POP_DRUM, p);
        return;
    }
    {                                                   /* the knob pages: TRIG, then banks 1..4 */
        const int pg = clampi((q->trig ? 0 : m->s.bank + 1) + d, 0, RFM_MOVE_NBANKS);
        q->trig = (uint8_t)(pg == 0);
        if (pg && pg - 1 != m->s.bank) {
            m->s.bank = (uint8_t)(pg - 1);
            rfmv_changed(m);
        }
        rfmv_pop(m, POP_BANK, 0);
    }
}

/* ================================================================ LEDs === */
static int blink(const rfm_move_seq_t *q) { return q->tl >= 0 && (q->tl / (RFM_MOVE_SEQ_TPS * 2)) & 1; }   /* 8ths */
static int pulse(const rfm_move_seq_t *q)                                  /* a beat: bright, fading */
{
    return q->playing && q->T >= 0 ? 256 - (int)(q->T % 96) * 160 / 96 : 256;
}


/* a clip's state in session view (the LEDs and the screen's grid) */
enum { CS_EMPTY, CS_CONTENT, CS_PLAYING, CS_QUEUED, CS_STOPPING, CS_REC, CS_REC_WAIT };
static int clip_state(const rfm_move_t *m, int track, int scene)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[track];
    const int used = rfmv_seq_clip_has(m, SEQ_CLIP(track, scene));
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
    const int clip = edit_clip(m), bl = blink(q), pu = pulse(q);
    const int t4 = rfmv_seq_trk_held(m);
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
            const int track = q->track_page + i % 8, scene = q->scene_off + (3 - i / 8);
            const rfm_move_track_t *t = &q->tr[track];
            const int col = t->mute ? 96 : 256, sel = track == q->sel_track && t->sel == scene;
            const int src = rfmv_seq_src_clip(m, SEQ_CLIP(track, scene));
            switch (src ? -1 : clip_state(m, track, scene)) {
            case -1: v = src == 2 ? sc(RFMV_RED, bl ? 200 : 60) : sc(RFMV_WHITE, bl ? 256 : 40); break;   /* cut, copied */
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
            if (q->tr[i].solo)
                v = sc((rgb_t){ 40, 90, 255 }, t4 && i == q->sel_track ? 256 : 140);   /* soloed: blue (Move's) */
            else if (q->tr[i].mute)
                v = sc(RFMV_RED, t4 && i == q->sel_track ? 256 : 48);
            else if (i == q->sel_track)
                v = TRACK_RGB[i];
            else
                v = sc(TRACK_RGB[i], used ? 64 : 10);
        } else if (q->mods & SEQ_M_LOOP) {              /* the pages: the loop's in the track's colour, the one
                                                         * shown white, the playing one pulsing; the clip's others
                                                         * (outside the loop) dim; past the clip off */
            const int a = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE, b = (rfmv_seq_loop_hi(m, clip) - 1) / RFM_MOVE_SEQ_PAGE;
            const int pos = q->tr[q->sel_track].active == q->tr[q->sel_track].sel ? rfmv_seq_clip_pos(m, q->sel_track) : -1;
            const int end = clip_end_page(m, clip);
            int lo, hi = rfmv_seq_clip_range(m, clip, &lo), k, notes = 0;
            for (k = lo; k < hi && !notes; k++)
                notes = q->notes[k].step / RFM_MOVE_SEQ_PAGE == i;
            if (i == q->page)
                v = RFMV_WHITE;
            else if (pos >= 0 && pos / RFM_MOVE_SEQ_PAGE == i)
                v = sc(i >= a && i <= b ? tc : RFMV_WHITE, pu);   /* the playhead's page */
            else if (i >= a && i <= b)
                v = tc;
            else if (i <= end)
                v = sc(RFMV_WHITE, notes ? 48 : 14);
            else
                v = RFMV_OFF;
            if (rfmv_seq_src_page(m, clip, i))           /* the page clipboard's source: cut red, copied blinking */
                v = rfmv_seq_src_page(m, clip, i) == 2 ? sc(RFMV_RED, bl ? 220 : 60) : sc(RFMV_WHITE, bl ? 256 : 30);
        } else if (q->mods & SEQ_M_SHIFT) {             /* the shortcuts */
            if (i == SH_PROJECTS || i == SH_CLOCK || i == SH_FIXVEL || i == SH_DOUBLE || i == SH_QUANT || sh_menu(i) ||
                i == SH_METRO || i == SH_NEWCLIP)
                v = sc(RFMV_WHITE, 96);
            if (sh_menu(i) && sh_menu(i) == q->smenu)
                v = rfmv_mode_rgb[RFM_MOVE_MODE_SEQ];
            if (i == SH_METRO && q->metro)
                v = RFMV_WHITE;
            if (i == SH_FIXVEL && m->s.vel_mode == RFM_MOVE_VEL_FIXED)
                v = RFMV_WHITE;
            if (i == SH_CLOCK && q->following)
                v = RFMV_GREEN;
        } else if (q->view) {                           /* the 2 steps under each track's column: stop (lit while it
                                                         * plays something to stop), mute (the track's colour; muted:
                                                         * red, dim) */
            const int tr = q->track_page + i / 2;
            const rfm_move_track_t *t = &q->tr[tr];
            if (i & 1)
                v = t->mute ? sc(RFMV_RED, 60) : sc(TRACK_RGB[tr], 160);
            else
                v = (t->active >= 0 && running(q) && rfmv_seq_clip_has(m, SEQ_CLIP(tr, t->active))) || t->queued >= 0
                        ? (t->queued == SEQ_STOP ? sc(RFMV_WHITE, bl ? 256 : 30)
                                                                                                : RFMV_WHITE)
                                                                     : sc(RFMV_WHITE, 8);
        } else {                                        /* Move's step colours */
            const int abs = q->page * RFM_MOVE_SEQ_PAGE + i, src = rfmv_seq_src_step(m, clip, abs);
            int lo, hi = rfmv_seq_range(m, clip, abs, &lo);
            if (q->steprec && abs == q->step_cur)
                v = RFMV_RED;                           /* Step record's cursor */
            else if (src)
                v = src == 2 ? sc(RFMV_RED, bl ? 220 : 60) : sc(RFMV_WHITE, bl ? 256 : 30);   /* cut, copied */
            else if (abs >= rfmv_seq_loop_lo(m, clip) && abs < rfmv_seq_loop_hi(m, clip)) {
                const int pos = q->tr[q->sel_track].active == q->tr[q->sel_track].sel ? rfmv_seq_clip_pos(m, q->sel_track) : -1;
                if (q->step_held >> i & 1)
                    v = tc;                             /* held: the track's colour, full */
                else if (pos == abs)
                    v = RFMV_GREEN;                     /* the playhead */
                else if (hi > lo)
                    v = RFMV_WHITE;                     /* notes */
                else
                    v = sc(tc, 28);                     /* empty: the track's colour, dim (the Move's LED shapes
                                                         * mark the beats) */
            } else if (q->step_held >> i & 1)
                v = tc;
            else
                v = hi > lo ? sc(RFMV_WHITE, 48) : sc(RFMV_WHITE, 6);   /* outside the loop: dim */
        }
        c[L_STEP0 + i] = v;
    }
    /* Play, Rec, Sample (the beat) */
    c[L_PLAY] = q->playing ? RFMV_GREEN : q->start_pending ? sc(RFMV_GREEN, bl ? 256 : 24) : sc(RFMV_WHITE, 40);
    c[L_REC] = q->start_pending == 3 || q->srec == 1 ? sc(RFMV_RED, bl ? 256 : 30)   /* (count-in, Capture armed) */
             : q->rec || q->srec == 2 || q->steprec ? RFMV_RED : sc(RFMV_RED, 30);
    if (q->arec)                                        /* Sample's loop: armed blinking, recording red */
        c[L_SAMPLE] = q->arec >= 2 ? RFMV_RED : sc(RFMV_RED, bl ? 256 : 30);
    else if (q->mods & SEQ_M_SHIFT)                     /* (Shift: tap tempo, the beat) */
        c[L_SAMPLE] = q->tl >= 0 && q->tl % 96 < 24 ? (q->following ? RFMV_GREEN : RFMV_WHITE) : sc(RFMV_WHITE, 16);
    else
        c[L_SAMPLE] = sc(RFMV_WHITE, 40);
    /* knob rings: the held step's locks, else the CC values */
    for (i = 0; i < RFM_MOVE_NKNOBS; i++) {
        const rgb_t bc = rfmv_bank_rgb[m->s.bank & 3];
        rgb_t v;
        if (q->trig && !q->view) {                      /* the TRIG page: the held step's values (lit: not the default) */
            const int ab = q->page * RFM_MOVE_SEQ_PAGE + (q->step_first >= 0 ? q->step_first : 0);
            const int tv = q->step_first >= 0 && i < 7 ? rfmv_seq_trig_val(m, clip, ab, i) : 0;
            const int def = i == 1 ? 1 : i == 2 ? 100 : i == 6 ? (q->step_first >= 0 ? RFM_MOVE_SEQ_TPS : 0) : 0;
            v = i >= 7 ? RFMV_OFF : m->touched == i ? RFMV_WHITE : tv != def ? sc(RFMV_WHITE, 200) : sc(RFMV_WHITE, 24);
        } else if (!q->view && q->step_first >= 0) {
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
        w[W_UP] = q->scene_off > 0 ? 100 : 8;           /* (the clip slots, the tracks) */
        w[W_DOWN] = q->scene_off < RFM_MOVE_SEQ_SCENES - 4 ? 100 : 8;
        w[W_LEFT] = q->track_page > 0 ? 100 : 8;
        w[W_RIGHT] = q->track_page < RFM_MOVE_SEQ_TRACKS - 8 ? 100 : 8;
    } else if (q->step_held) {
        w[W_UP] = w[W_DOWN] = w[W_LEFT] = w[W_RIGHT] = 100;   /* (transpose, nudge) */
    } else {
        w[W_UP] = m->s.octave > 0 ? 127 : 24;
        w[W_DOWN] = m->s.octave < 0 ? 127 : 24;
        w[W_LEFT] = q->page > 0 ? 100 : 8;
        w[W_RIGHT] = q->page + 1 < RFM_MOVE_SEQ_PAGES ? ((q->page + 1) * RFM_MOVE_SEQ_PAGE < rfmv_seq_loop_hi(m, clip) ? 100 : 40) : 8;
    }
    w[W_UNDO] = q->un ? 60 : 6;
    if (q->loop_latch) {                                /* latched (page edit on): glowing, a second round */
        const int ph = (int)(q->now_us / 1000u % 1000u);
        w[W_LOOP] = (uint8_t)(60 + (ph < 500 ? ph : 1000 - ph) * 67 / 500);
    } else
        w[W_LOOP] = (q->mods & SEQ_M_LOOP) ? 127 : 40;
    w[W_COPY] = (q->mods & SEQ_M_COPY) ? 127 : q->paste ? (bl ? 127 : 30) : q->clip_kind && q->cut ? 70 : 30;
    w[W_MUTE] = (q->mods & SEQ_M_MUTE) ? 127 : q->tr[q->sel_track].mute ? 100 : 30;
    w[W_DELETE] = (q->mods & SEQ_M_DELETE) ? 127 : 30;
}

/* ================================================================ the screen === */
/* v2 (the design canvas's V2Track, V2Loop, V2Session, V2Menus, V2Arp, V2Projects, V2Record; drawn with v2.c):
 * symbols at rest, words only while something is touched or turned; Move's bottom strip says where you are */
static const struct { const char *a, *b; int8_t icon; } MSG[] = {
    { "", "", -1 }, { "No Empty", "Clip", -1 }, { "Full", "", -1 }, { "Load", "Failed", -1 }, { "Captured", "", -1 },
    { "Undone", "", -1 }, { "Redone", "", -1 }, { "Copied", "", -1 }, { "Pasted", "", -1 }, { "Cut", "", -1 },
    { "Quantized", "", -1 }, { "Loop", "Doubled", -1 }, { "Ext Tempo", "", V2I_LOCK }, { "Duplicated", "", -1 },
    { "Cleared", "", -1 }, { "Capture", "Cleared", -1 }, { "Clock Auto", "", -1 }, { "Clock Int", "", -1 },
    { "Clock Ext", "", -1 }, { "Full Vel", "", V2I_VEL }, { "Pad Vel", "", V2I_VEL }, { "Click On", "", V2I_METRO },
    { "Click Off", "", V2I_METRO }, { "New Clip", "", V2I_CLIP },
};
const char *rfmv_seq_msg_text(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    return q->msg && rfmv_now_ms(m) - q->msg_ms < RFM_MOVE_POPUP_MS && q->msg < sizeof MSG / sizeof MSG[0] ? MSG[q->msg].a
                                                                                                         : NULL;
}
int rfmv_seq_msg_info(const rfm_move_t *m, const char **a, const char **b, int *icon)
{
    const rfm_move_seq_t *q = &m->seq;
    const int id = rfmv_seq_msg_text(m) ? q->msg : 0;
    *a = MSG[id].a, *b = MSG[id].b, *icon = MSG[id].icon;
    return id;
}

/* how full a group of notes looks: 0 none, 1 .. cap (a bar's height: the bits of the count) */
static int fullness(int n, int cap)
{
    int b = 0;
    while (n > 0)
        b++, n >>= 1;
    return b < cap ? b : cap;
}

/* a clip's last page: the loop's, or a later one with notes */
int rfmv_seq_end_page(const rfm_move_t *m, int clip) { return clip_end_page(m, clip); }

/* Move's strip in track view: the clip's pages (the shown one thick; a ghost page shown: the real pages a line and a
 * + at the far end), the playhead (or, at >= 0, that step) a tick over the whole clip */
void rfmv_seq_page_strip(const rfm_move_t *m, mr_canvas_t *cv, int at)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = edit_clip(m), end = clip_end_page(m, clip), n = end + 1;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    int pos = at;
    if (q->page > end) {
        v2_ghostStrip(cv, n, q->page + 2);
        return;
    }
    if (pos < 0 && t->active == t->sel && running(q))
        pos = rfmv_seq_clip_pos(m, q->sel_track);
    v2_strip(cv, n, 1u << q->page, pos >= 0 ? (double)pos / (n * RFM_MOVE_SEQ_PAGE) : -1, 0, 128);
}

/* the 16 steps of the shown page as cells (only while a step, Copy or Delete is held, or Step record is on): notes
 * filled, empty outlined, copied dashed, cut crossed; the held ones (marks) underlined above */
void rfmv_seq_steps_cells(const rfm_move_t *m, mr_canvas_t *cv, uint32_t marks)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = edit_clip(m);
    int i;
    for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
        const int abs = q->page * RFM_MOVE_SEQ_PAGE + i, src = rfmv_seq_src_step(m, clip, abs), x = i * 8, y = 52;
        int lo;
        const int notes = rfmv_seq_range(m, clip, abs, &lo) > lo;
        if (src == 2)
            v2_O(cv, x, y, 7, 11, 1), v2_X(cv, x + 1, y + 2, 5, 7);
        else if (src == 1) {
            if (notes)
                v2_R(cv, x + 2, y + 3, 3, 5, 1);
            v2_dashO(cv, x, y, 7, 11);
        } else if (notes)
            v2_R(cv, x, y, 7, 11, 1);
        else
            v2_O(cv, x, y, 7, 11, 1);
        if (marks >> i & 1)
            v2_R(cv, x, y - 3, 7, 2, 1);
    }
}

/* V2Projects proj.grid: the 32 slots as the pads are (top-left = 1); the open one filled, a saved one outlined, each
 * with three bars (its tracks with notes in three groups, as fullness()); an empty slot a dot; the clipboard's
 * source crossed (cut) or dashed (copied). A project loading (the host's answer pending) blinks. */
static void projects_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    for (i = 0; i < RFM_MOVE_PROJECTS; i++) {
        const int x = (i % 8) * 16 + 1, y = (i / 8) * 16 + 1, cur = i == q->proj_cur || (i == q->proj_pending && blink(q));
        rfm_move_proj_info_t in;
        int used[RFM_MOVE_SEQ_TRACKS], nu = 0, g, t;
        if (q->pbuf_kind && q->pbuf_src == i && !cur) {  /* the buffer's source: cut crossed, copied dashed */
            if (q->pbuf_kind == 2 && !(q->proj_used >> i & 1)) {
                v2_O(cv, x, y, 14, 14, 1), v2_X(cv, x + 3, y + 3, 8, 8);
                continue;
            }
            if (q->pbuf_kind == 1) {
                v2_dashO(cv, x, y, 14, 14);
                continue;
            }
        }
        if (!cur && !(q->proj_used >> i & 1)) {
            v2_R(cv, x + 6, y + 6, 2, 2, 1);
            continue;
        }
        if (cur)
            v2_R(cv, x, y, 14, 14, 1);
        else
            v2_O(cv, x, y, 14, 14, 1);
        rfmv_proj_info(m, i, &in);
        if (!in.ok)
            continue;
        for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
            if (in.notes[t])
                used[nu++] = in.notes[t];
        for (g = 0; g < 3; g++) {                       /* the used tracks in thirds (1 track: one bar, 2: two) */
            const int a = nu * g / 3, b = nu * (g + 1) / 3;
            int n = 0, h;
            for (t = a; t < b; t++)
                n += used[t];
            h = fullness(n, 8);
            if (h)
                v2_R(cv, x + 3 + g * 3, y + 11 - h, 2, h, !cur);
        }
    }
}

/* V2Session sess.rest (the owner's mockup): the 8 tracks in view across, their 4 clip slots down; a 14 x 2 bar over
 * the selected track (muted tracks: a dashed line on top); playing filled with a black play icon, recording filled
 * with a black square, queued doubled (blinking), stopping blinking, a clip outlined, empty a dot; the clipboard's
 * source dashed (copied) or crossed (cut) */
static void session_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int bl = blink(q);
    int c, r;
    for (c = 0; c < 8; c++) {
        const int track = q->track_page + c, x = c * 16 + 1;
        if (track == q->sel_track)
            v2_R(cv, x, 0, 14, 2, 1);
        else if (q->tr[track].mute)
            v2_dotH(cv, x, 1, 14);
    }
    for (r = 0; r < 4; r++)
        for (c = 0; c < 8; c++) {
            const int track = q->track_page + c, scene = q->scene_off + r;
            const int src = rfmv_seq_src_clip(m, SEQ_CLIP(track, scene));
            char s = '.';
            if (src)
                s = src == 2 ? 'X' : 'D';
            else
                switch (clip_state(m, track, scene)) {
                case CS_CONTENT: s = 'C'; break;
                case CS_QUEUED: s = bl ? 'C' : 'Q'; break;
                case CS_PLAYING: s = 'P'; break;
                case CS_STOPPING: s = bl ? 'C' : 'P'; break;
                case CS_REC: s = 'R'; break;
                case CS_REC_WAIT: s = bl ? 'C' : 'R'; break;
                default: break;
                }
            v2_cell(cv, c, r, s);
        }
}

/* the shown page's notes as a piano roll: 8 px a step, 13 rows of 4 px (the page's highest note on top; squeezed past
 * 12 semitones), 3 px tall, the beats dashed, the playhead a line */
static void roll(const rfm_move_t *m, mr_canvas_t *cv, int y0, int h)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = edit_clip(m), p0 = q->page * RFM_MOVE_SEQ_PAGE, row = (h - 4) / 12;
    int i, lo, hi, nlo = 127, nhi = 0, s;
    for (s = 4; s < 16; s += 4)
        v2_dotV(cv, s * 8, y0, h);
    hi = rfmv_seq_clip_range(m, clip, &lo);
    for (i = lo; i < hi; i++)
        if (q->notes[i].step >= p0 && q->notes[i].step < p0 + RFM_MOVE_SEQ_PAGE) {
            nlo = q->notes[i].note < nlo ? q->notes[i].note : nlo;
            nhi = q->notes[i].note > nhi ? q->notes[i].note : nhi;
        }
    for (i = lo; i < hi; i++) {
        const rfm_move_note_t *n = &q->notes[i];
        int x, w, r;
        if (n->step < p0 || n->step >= p0 + RFM_MOVE_SEQ_PAGE)
            continue;
        x = (n->step - p0) * 8 + 1 + n->micro / 3;
        w = n->len * 8 / RFM_MOVE_SEQ_TPS - 2;
        r = nhi - nlo <= 12 ? nhi - n->note + (12 - (nhi - nlo)) / 2 : (nhi - n->note) * 12 / (nhi - nlo);   /* (centred) */
        v2_R(cv, x, y0 + 2 + r * row, w < 1 ? 1 : w, 3, 1);
    }
    {
        const rfm_move_track_t *t = &q->tr[q->sel_track];
        const int tick = t->active == t->sel ? rfmv_seq_clip_tick(m, q->sel_track) : -1;
        if (tick >= 0 && tick / RFM_MOVE_SEQ_TPS >= p0 && tick / RFM_MOVE_SEQ_TPS < p0 + RFM_MOVE_SEQ_PAGE)
            v2_R(cv, (tick - p0 * RFM_MOVE_SEQ_TPS) * 8 / RFM_MOVE_SEQ_TPS, y0, 1, h, 1);
    }
}

/* V2Track seq.track: the shown page as a piano roll, full height; Move's strip the pages and the playhead. A step,
 * Copy or Delete held, or Step record: the steps row (seq.clip). Recording: Move's "Recording" row on top
 * (rec.capture); the audio loop: "Sampling" (rec.sample) */
static void track_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int cells = q->step_held || (q->mods & (SEQ_M_COPY | SEQ_M_DELETE)) || q->steprec;
    int y0 = 0, h = cells ? 46 : 56;
    if (q->arec >= 2 || (q->arec == 1 && !blink(q))) {  /* the audio loop: armed (blinking), recording */
        v2_ic(cv, V2I_WAVE, 0, 2, 1);
        v2_T(cv, 16, 1, q->arec == 1 ? "Sample" : "Sampling", 2, 1);
        y0 = 20, h = cells ? 26 : 36;
    } else if (q->rec && running(q) && q->rec_mode != RFM_MOVE_RM_STEP) {
        v2_ic(cv, V2I_REC, 0, 2, 1);
        v2_T(cv, 16, 1, "Recording", 2, 1);
        y0 = 20, h = cells ? 26 : 36;
    }
    roll(m, cv, y0, h);
    if (cells) {
        uint32_t marks = q->step_held;
        if (q->steprec && q->step_cur / RFM_MOVE_SEQ_PAGE == q->page)
            marks |= 1u << (q->step_cur % RFM_MOVE_SEQ_PAGE);   /* (Step record's cursor) */
        rfmv_seq_steps_cells(m, cv, marks);
    } else
        rfmv_seq_page_strip(m, cv, -1);
}

/* the 16 tracks on one row, as the 16 step keys: the selected one filled, muted crossed, soloed a square inside,
 * with notes outlined, empty a dot; a word on top */
static void tracks_row(const rfm_move_t *m, mr_canvas_t *cv, const char *title)
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    v2_TC(cv, 64, 3, title, 2, 1);
    for (i = 0; i < RFM_MOVE_SEQ_TRACKS; i++) {
        const int x = i * 8, y = 28;
        if (i == q->sel_track) {
            v2_R(cv, x, y, 7, 26, 1);
            if (q->tr[i].mute)
                v2_R(cv, x + 1, y + 9, 5, 8, 0), v2_X(cv, x + 1, y + 9, 5, 8), v2_XR(cv, x + 1, y + 9, 5, 8);
            else if (q->tr[i].solo)
                v2_R(cv, x + 2, y + 10, 3, 6, 0);
        } else if (q->tr[i].mute)
            v2_O(cv, x, y, 7, 26, 1), v2_X(cv, x + 1, y + 9, 5, 8);
        else if (q->tr[i].solo)
            v2_O(cv, x, y, 7, 26, 1), v2_R(cv, x + 2, y + 10, 3, 6, 1);
        else if (rfmv_seq_track_used(m, i))
            v2_O(cv, x, y, 7, 26, 1);
        else
            v2_R(cv, x + 3, y + 12, 1, 2, 1);
    }
}

/* Mute held: the 16 tracks, the muted ones crossed */
static void mute_screen(const rfm_move_t *m, mr_canvas_t *cv) { tracks_row(m, cv, "Mute"); }

/* the Shift menus (V2Menus, V2Arp, V2Global): knobs and the jog only; the knob legend where the design has it */
static const char *const AP_NAMES[RFM_MOVE_AP_N] = { "Up", "Down", "Up Dn", "Dn Up", "U+D", "D+U", "Conv", "Div",
                                                     "Con Div", "Hi Up", "Hi UpDn", "Lo Up", "Lo UpDn", "Rand",
                                                     "Rand Oth", "Shuffle", "Played" };
static int touched_knob(const rfm_move_t *m) { return m->touched >= 0 && m->touched < RFM_MOVE_NKNOBS ? m->touched : -1; }
static void fit2(mr_canvas_t *cv, int x, int y, const char *s, int w)   /* 2x where it fits in w, else 1x */
{
    if (v2_tw(s, 2) <= w)
        v2_T(cv, x, y, s, 2, 1);
    else
        v2_T(cv, x, y + 4, s, 1, 1);
}
static void smenu_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    const int tk = touched_knob(m);
    char b[32], c[32];
    int i;
    switch (q->smenu) {
    case RFM_MOVE_SM_WORKFLOW:                          /* the track's channel, rate and quantize: a row a knob */
        v2_knobs(cv, 7, tk);
        snprintf(b, sizeof b, "Ch %d", t->ch + 1), v2_T(cv, 0, 14, b, 2, 1);
        snprintf(b, sizeof b, "Rate %s", rfm_move_seq_rate_name(t->rate)), v2_T(cv, 0, 31, b, 2, 1);
        snprintf(b, sizeof b, "Quant %d%%", q->qstrength), v2_T(cv, 0, 48, b, 2, 1);
        break;
    case RFM_MOVE_SM_TEMPO: {                           /* the tap icon (following: a padlock), the BPM at 4x; the
                                                         * taps counted on the strip */
        const int b10 = rfm_move_seq_bpm10(m);
        v2_ic(cv, q->following ? V2I_LOCK : V2I_TAP, 2, 3, 1);
        v2_TC(cv, 64, 3, "Tempo", 2, 1);
        snprintf(b, sizeof b, b10 % 10 && b10 < 1000 ? "%d.%d" : "%d", b10 % 10 && b10 < 1000 ? b10 / 10 : (b10 + 5) / 10, b10 % 10);
        v2_TC(cv, 64, 22, b, v2_tw(b, 4) <= 124 ? 4 : 3, 1);
        if (!q->following)
            v2_strip(cv, 4, (1u << clampi(q->ntap, 0, 4)) - 1, -1, 0, 128);
        break;
    }
    case RFM_MOVE_SM_GROOVE: {                          /* two 16ths, the second late by the groove; Global / Track */
        const int g = rfmv_seq_groove_of(m, q->sel_track), own = t->own & 1;
        v2_knobs(cv, 3, tk);
        v2_R(cv, 4, 12, 3, 30, 1);
        v2_R(cv, 28 + (g * 10 + 50) / 100, 12, 3, 30, 1);
        v2_dotV(cv, 28, 12, 30);
        snprintf(b, sizeof b, "%d%%", g);
        v2_titleBox(cv, "Groove", b, 88, 50);
        if (own)
            v2_ic(cv, V2I_GLOBE, 2, 48, 1), v2_icSel(cv, V2I_TRACK, 18, 48);
        else
            v2_icSel(cv, V2I_GLOBE, 2, 48), v2_ic(cv, V2I_TRACK, 18, 48, 1);
        v2_T(cv, 40, 48, own ? "Track" : "Global", 2, 1);
        break;
    }
    case RFM_MOVE_SM_LAYOUT: {                          /* Move's icon row: the chosen one underlined and named */
        static const int IC[4] = { V2I_INKEY, V2I_CHROM, V2I_DRUM, V2I_OCT };
        static const char *const NM[4] = { "In-Key", "Chromatic", "Drum", "Octaves" };
        for (i = 0; i < 4; i++)
            if (i == m->s.layout)
                v2_icSel(cv, IC[i], 22 + i * 24, 6);
            else
                v2_ic(cv, IC[i], 22 + i * 24, 6, 1);
        v2_TC(cv, 64, 30, NM[m->s.layout & 3], 2, 1);
        v2_strip(cv, 2, 1, -1, 0, 128);
        break;
    }
    case RFM_MOVE_SM_KEYS:                              /* the build's key / scale screen, Global / Track below */
        rfmv_keys_menu(m, cv, !(t->own & 2));
        break;
    case RFM_MOVE_SM_ARP:
        if (q->arp_page) {                              /* a lane: its 16 steps as bars (the edited one underlined;
                                                         * past its length a dot) */
            const rfm_move_arp_t *a = &t->arp;
            const int L = q->arp_page - 1, len = L ? a->tlen : a->mlen;
            v2_knobs(cv, 0x0F, tk);
            v2_T(cv, 0, 11, L ? "Trans" : "Mod", 2, 1);
            v2_R(cv, 0, 40, 128, 1, 1);
            for (i = 0; i < RFM_MOVE_ARP_LANE; i++) {
                const int x = i * 8 + 1, v = a->lane[L][i];
                if (i >= len) {
                    v2_R(cv, x + 2, 44, 2, 2, 1);
                    continue;
                }
                if (L) {                                /* transpose: up or down from the line */
                    const int d = clampi(v, -48, 48) * 14 / 48;
                    v2_R(cv, x, d >= 0 ? 40 - d : 41, 6, d ? (d > 0 ? d : -d) : 1, 1);
                } else                                  /* mod: the kind as a height */
                    v2_R(cv, x, 40 - 2 * v - 1, 6, 2 * v + 1, 1);
                if (i == q->arp_sel)
                    v2_R(cv, x, 55, 6, 2, 1);
            }
            v2_strip(cv, 3, 1u << q->arp_page, -1, 0, 128);
            break;
        }
        {                                               /* the pattern's walk as a picture, its name, rate, range */
            const rfm_move_arp_t *a = &t->arp;
            uint8_t walk[8];
            int n = 0;
            switch (a->pat) {
            case RFM_MOVE_AP_DN: walk[0] = 2, walk[1] = 1, walk[2] = 0, n = 3; break;
            case RFM_MOVE_AP_UPDN: walk[0] = 0, walk[1] = 1, walk[2] = 2, walk[3] = 1, n = 4; break;
            case RFM_MOVE_AP_DNUP: walk[0] = 2, walk[1] = 1, walk[2] = 0, walk[3] = 1, n = 4; break;
            case RFM_MOVE_AP_UANDD: walk[0] = 0, walk[1] = 1, walk[2] = 2, walk[3] = 2, walk[4] = 1, walk[5] = 0, n = 6; break;
            case RFM_MOVE_AP_DANDU: walk[0] = 2, walk[1] = 1, walk[2] = 0, walk[3] = 0, walk[4] = 1, walk[5] = 2, n = 6; break;
            case RFM_MOVE_AP_CON: case RFM_MOVE_AP_CONDIV: walk[0] = 0, walk[1] = 2, walk[2] = 1, n = 3; break;
            case RFM_MOVE_AP_DIV: walk[0] = 1, walk[1] = 2, walk[2] = 0, n = 3; break;
            case RFM_MOVE_AP_HI1: case RFM_MOVE_AP_HI2: walk[0] = 2, walk[1] = 1, walk[2] = 2, walk[3] = 0, n = 4; break;
            case RFM_MOVE_AP_LO1: case RFM_MOVE_AP_LO2: walk[0] = 0, walk[1] = 1, walk[2] = 0, walk[3] = 2, n = 4; break;
            case RFM_MOVE_AP_RAND: case RFM_MOVE_AP_ROTH: case RFM_MOVE_AP_RONC:
                walk[0] = 1, walk[1] = 0, walk[2] = 2, walk[3] = 0, walk[4] = 1, n = 5; break;
            default: walk[0] = 0, walk[1] = 1, walk[2] = 2, n = 3; break;
            }
            v2_knobs(cv, 0xFF, tk);
            for (i = 0; i < n; i++) {                   /* (off: outlined) */
                if (a->mode == RFM_MOVE_AM_OFF)
                    v2_O(cv, 2 + i * 10, 46 - walk[i] * 15, 8, 4, 1);
                else
                    v2_R(cv, 2 + i * 10, 46 - walk[i] * 15, 8, 4, 1);
            }
            fit2(cv, 66, 10, AP_NAMES[a->pat % RFM_MOVE_AP_N], 62);
            snprintf(b, sizeof b, "1/%d", rfmv_seq_arp_rate_of(a->rate)), fit2(cv, 66, 27, b, 62);
            snprintf(b, sizeof b, "Oct %d", a->range), fit2(cv, 66, 44, b, 62);
            v2_strip(cv, 3, 1, -1, 0, 128);
        }
        break;
    case RFM_MOVE_SM_REC: {                             /* the three modes as icons, the chosen one underlined and
                                                         * named; the style below with its icon */
        static const int IC[3] = { V2I_CAPTURE, V2I_REC, V2I_STEP };
        static const char *const NM[3] = { "Capture", "Record", "Step" };
        for (i = 0; i < 3; i++)
            if (i == q->rec_mode)
                v2_icSel(cv, IC[i], 34 + i * 24, 2);
            else
                v2_ic(cv, IC[i], 34 + i * 24, 2, 1);
        v2_TC(cv, 64, 22, NM[q->rec_mode % 3], 2, 1);
        v2_ic(cv, q->rec_style == RFM_MOVE_RS_REPLACE ? V2I_REPLACE : V2I_OVERDUB, 22, 44, 1);
        v2_T(cv, 40, 42, q->rec_style == RFM_MOVE_RS_REPLACE ? "Replace" : "Overdub", 2, 1);
        break;
    }
    case RFM_MOVE_SM_VEL: {                             /* the curve as a graph (Fixed: a flat line at the level), the
                                                         * option (the jog) and the level (knob 1) with a bar */
        v2_knobs(cv, 1, tk);
        v2_R(cv, 2, 12, 1, 40, 1), v2_R(cv, 2, 51, 40, 1, 1);
        for (i = 0; i < 38; i++) {
            const int v = rfm_move_vel(&m->s, 1 + i * 126 / 37);
            v2_R(cv, 3 + i, 51 - (v * 37 + 63) / 127, 1, 2, 1);
        }
        rfmv_tcase(rfm_move_vel_name(m->s.vel_mode), c, sizeof c);
        v2_T(cv, 50 + v2_tw(c, 2) <= 128 ? 50 : 128 - v2_tw(c, 2), 12, c, 2, 1);
        snprintf(b, sizeof b, "%d", m->s.vel_level), v2_T(cv, 50, 30, b, 2, 1);
        v2_vbar(cv, m->s.vel_level / 127.0, 0, 50, 76, 48);
        break;
    }
    case RFM_MOVE_SM_GLOBAL: {                          /* the Move's battery (as Move draws it) and the settings;
                                                         * the velocity level only in its popup (knob 3 touched) */
        static const char *const CK[3] = { "Auto", "Int", "Ext" };
        const int bt = m->battery;
        if (tk == 2) {
            snprintf(b, sizeof b, "%d", m->s.vel_level);
            v2_titleBox(cv, "Level", b, 64, 124);
            v2_vbar(cv, m->s.vel_level / 127.0, 0, 4, 120, 52);
            break;
        }
        v2_knobs(cv, 7, tk);
        v2_battery(cv, 60, 1, bt > 0 ? bt : 0, 0);
        if (bt >= 0)
            snprintf(b, sizeof b, "%d%%", bt);
        else
            snprintf(b, sizeof b, "--");
        v2_T(cv, 90, 0, b, 2, 1);
        snprintf(b, sizeof b, "Clock %s", CK[q->clock_mode % 3]), v2_T(cv, 0, 22, b, 2, 1);
        rfmv_tcase(rfm_move_vel_name(m->s.vel_mode), c, sizeof c);
        snprintf(b, sizeof b, "Vel %s", c), v2_T(cv, 0, 42, b, 2, 1);
        break;
    }
    default: break;
    }
}

/* V2Loop loop.edit: Loop held or latched (page edit): the loop icon, the range in words, the 16 pages as cells (in
 * the loop filled, the clip's others outlined, past the clip a dot, cut crossed, copied dashed), the shown one
 * underlined */
void rfmv_seq_loop_edit(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = edit_clip(m), pa = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE;
    const int pb = (rfmv_seq_loop_hi(m, clip) - 1) / RFM_MOVE_SEQ_PAGE;
    char b[24], mk[16];
    int i;
    v2_ic(cv, V2I_LOOP, 3, 3, 1);
    if (pa == pb)
        snprintf(b, sizeof b, "Bar %d", pa + 1);
    else
        snprintf(b, sizeof b, "Bars %d-%d", pa + 1, pb + 1);
    v2_TC(cv, 76, 4, b, 2, 1);
    for (i = 0; i < 16; i++) {
        const int src = rfmv_seq_src_page(m, clip, i);
        mk[i] = (char)(src == 2 ? 'X' : src == 1 ? 'D' : 0);
    }
    v2_pages(cv, clip_end_page(m, clip) + 1, pa, pb, q->page, mk);
}

void rfmv_seq_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    if (!q->projects && !q->smenu && !q->view && (q->mods & SEQ_M_LOOP) && !(q->mods & SEQ_M_MUTE))
        rfmv_seq_loop_edit(m, cv);
    else if (q->projects)
        projects_screen(m, cv);
    else if (q->smenu)
        smenu_screen(m, cv);
    else if (q->mods & SEQ_M_MUTE)
        mute_screen(m, cv);
    else if (q->view)
        session_screen(m, cv);
    else
        track_screen(m, cv);
}

/* Track 4 held (V2Session sess.hold4): the 16 tracks on one row, the selected one named on top (or what changed last:
 * the channel, a knob's CC, the rate, the layout, the scale) */
void rfmv_seq_track_menu(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    char b[24], c[24];
    switch (m->menu_what) {
    case MENU_TCH: snprintf(b, sizeof b, "Ch %d", t->ch + 1); break;
    case MENU_CC: snprintf(b, sizeof b, "K%d CC %d", (m->menu_i & 7) + 1, m->s.cc[m->s.bank][m->menu_i & 7]); break;
    case MENU_RATE: snprintf(b, sizeof b, "Rate %s", rfm_move_seq_rate_name(t->rate)); break;
    case MENU_LAYOUT: rfmv_tcase(rfm_move_layout_name(m->s.layout), b, sizeof b); break;
    case MENU_SCALE: rfmv_tcase(rfm_move_scale_name(m->s.scale), c, sizeof c), snprintf(b, sizeof b, "%s", c); break;
    default: snprintf(b, sizeof b, "Track %d", q->sel_track + 1); break;
    }
    tracks_row(m, cv, b);
}
