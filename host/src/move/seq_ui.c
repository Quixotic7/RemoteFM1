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

#define NUDGE 2                                         /* ticks a Left / Right with a step held (~10 % of a step) */
#define LEN_FINE 2                                      /* ticks a jog detent with a step held (~10 % of a step) */

/* the tracks' colours */
static const rgb_t TRACK_RGB[RFM_MOVE_SEQ_TRACKS] = {
    { 255, 42, 42 },  { 255, 128, 0 },  { 255, 208, 0 },   { 200, 255, 42 }, { 42, 255, 80 },   { 0, 255, 190 },
    { 26, 228, 255 }, { 40, 120, 255 }, { 120, 60, 255 },  { 196, 58, 255 }, { 255, 58, 200 },  { 255, 110, 140 },
    { 255, 170, 110 }, { 180, 255, 160 }, { 140, 220, 255 }, { 220, 200, 255 },
};
/* Shift + step n (0-based), as printed on the Move (its manual): 1 Set Overview (our projects), 2 Setup (our clock
 * source), 3 Workflow Settings, 5 Tempo, 6 Metronome, 7 Groove, 8 Layout, 9 Keys & Scales, 10 Full Velocity,
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
    if (!((m->s.mode == RFM_MOVE_MODE_SEQ && (q->mods & SEQ_M_SHIFT)) ||
          (m->s.mode == RFM_MOVE_MODE_FM1 && rfmv_track_held(m, 0))))
        return;
    for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
        const int job = i == SH_PROJECTS || i == SH_CLOCK || i == SH_FIXVEL || i == SH_DOUBLE || i == SH_QUANT ||
                        i == SH_METRO || i == SH_NEWCLIP || sh_menu(i);
        const int on = (i == SH_FIXVEL && q->fixed_vel) || (i == SH_METRO && q->metro) || (i == SH_CLOCK && q->following) ||
                       (i == SH_PROJECTS && q->projects) || (sh_menu(i) && sh_menu(i) == q->smenu);
        icon[i] = (uint8_t)(!job ? 0 : on ? 127 : 24);
    }
}

static int sh_step(int menu)
{
    static const int8_t S[] = { -1, SH_WORKFLOW, SH_TEMPO, SH_GROOVE, SH_LAYOUT, SH_KEYS, SH_ARP };
    return menu > 0 && menu < (int)sizeof S ? S[menu] : -1;
}

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
    if ((q->smenu == RFM_MOVE_SM_LAYOUT || q->smenu == RFM_MOVE_SM_KEYS) && !q->projects) {
        rfmv_pick_layout(m, p);                         /* (the Layout / Keys & Scales menus: the pads pick) */
        return;
    }
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
            rfmv_seq_launch_scene(m, scene);
            if (!running(q))
                rfmv_seq_start(m);
        } else if (dbl && rfmv_seq_clip_used(m, clip) && !t->grow && !(q->mods & (SEQ_M_MUTE | SEQ_M_COPY | SEQ_M_DELETE))) {
            rfmv_seq_select(m, track);                  /* a double tap: launched now, in phase with the bar */
            rfmv_seq_launch_now(m, track, scene);
            if (!running(q))
                rfmv_seq_start(m);
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
                if (q->rec) {                           /* armed: record into it */
                    if (!run) {
                        rfmv_seq_group(m);
                        q->rec_group = q->group;
                        rfmv_seq_start(m);
                    }
                    t->grow = 2;                        /* (starts with the launch, at the bar) */
                }
                rfmv_seq_launch(m, track, scene);
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
                    else
                        rfmv_seq_note_put(m, clip, abs, note, vel, RFM_MOVE_SEQ_TPS, 0), grow_to(m, clip, abs);
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
            rfmv_seq_note_put(m, clip, q->step_cur, note, vel, RFM_MOVE_SEQ_TPS, 0);
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
                        grow_to(m, clip, abs), entered(m, was);
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
        case SH_PROJECTS: q->projects = 1, q->smenu = 0, layer(q, LY_PROJ); break;
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
                    break;
                }
            }
            if (k > RFM_MOVE_SEQ_SCENES)
                rfmv_seq_msg(m, SEQ_MSG_NOCLIP);
            break;
        }
        case SH_FIXVEL:
            q->fixed_vel = (uint8_t)!q->fixed_vel, rfmv_seq_dirty(m);
            rfmv_seq_msg(m, q->fixed_vel ? SEQ_MSG_FULLVEL_ON : SEQ_MSG_FULLVEL_OFF);
            break;
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
        case SH_CLOCK:
            rfm_move_seq_set_clock(m, (q->clock_mode + 1) % 3);
            rfmv_seq_msg(m, SEQ_MSG_CLK_AUTO + q->clock_mode % 3);
            break;
        default: break;
        }
        return;
    }
    if (q->smenu == RFM_MOVE_SM_ARP && q->arp_page) {   /* the arp's lanes: a step of the lane */
        q->arp_sel = (uint8_t)s;
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
            step_notes_clear(m, clip, abs);
            q->nlast = 0;
            for (p = 0; p < RFM_MOVE_NPADS; p++)
                if ((q->chord >> p & 1) && m->pad[p].kind == H_NOTE) {
                    const int v = vel_of(m, q->pad_vel[p]);
                    put |= rfmv_seq_note_put(m, clip, abs, m->pad[p].b, v, RFM_MOVE_SEQ_TPS, 0) == 0;
                    last_add(q, m->pad[p].b, v);
                }
            q->step_add |= bit;
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
            if (cc == MR_CC_LOOP && (q->mods & SEQ_M_SHIFT)) {   /* Shift + Loop: double the loop (as Shift + 15) */
                q->mods_used |= SEQ_M_SHIFT;
                rfmv_seq_group(m);
                if (rfmv_seq_clip_double(m, edit_clip(m)) == 0)
                    rfmv_seq_msg(m, SEQ_MSG_DOUBLE);
                return 1;
            }
            if (mb == SEQ_M_SHIFT) {                    /* a double tap latches Shift (a tap, then a press within
                                                         * 300 ms); a press unlatches it */
                if (q->shift_lock)
                    q->shift_lock = 0, q->shift_ms = 0, q->mods_used |= SEQ_M_SHIFT;
                else if (q->shift_ms && now - q->shift_ms < 300)
                    q->shift_lock = 1, q->shift_ms = 0, layer(q, LY_SHIFT);
            }
            if (mb == SEQ_M_LOOP && q->loop_latch) {   /* latched: this press ends it (at its release) */
                q->loop_latch = 0;
                q->mods_used |= SEQ_M_LOOP;
                return 1;
            }
            q->mods |= (uint8_t)mb, q->mods_used &= (uint8_t)~mb;
            if (mb == SEQ_M_LOOP && !q->view && !q->projects)
                rfmv_pop(m, POP_LOOP, 0);               /* Loop held: the clip's length */
            if (mb == SEQ_M_COPY)
                q->copy_src = 0, q->copy_pasted = 0;
        } else if (mb == SEQ_M_SHIFT && q->shift_lock) {
            ;                                           /* (latched: it stays down) */
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
        else
            q->view = (uint8_t)!q->view;
        return 1;
    case MR_CC_BACK:                                    /* the universal escape: the latest layer still on closes
                                                         * (a Shift menu, the project grid, Loop's latch, Shift's
                                                         * lock, the clipboard's marks), one a press; none: FM-1 mode */
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
        rfm_move_set_mode(m, RFM_MOVE_MODE_FM1);        /* back to FM-1 mode; the sequencer keeps playing */
        return 1;
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
        else {                                          /* the loop's pages; Right past the last: a pending page
                                                         * (+1, +2 ..: the clip grows to it when a step goes in there;
                                                         * Left steps back) */
            const int clip = edit_clip(m), a = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE;
            const int pages = (rfmv_seq_loop_hi(m, clip) + RFM_MOVE_SEQ_PAGE - 1) / RFM_MOVE_SEQ_PAGE, was = q->page;
            q->page = (uint8_t)clampi(q->page + d, q->page < a ? 0 : a, RFM_MOVE_SEQ_PAGES - 1);
            if (q->page != was && (q->page >= pages || was >= pages))
                rfmv_pop(m, POP_BAR, q->page);          /* (a ghost page added or taken away: "BAR n", a second) */
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
static void smenu_turn(rfm_move_t *m, int k, int d)
{
    rfm_move_seq_t *q = Q(m);
    rfm_move_track_t *t = &q->tr[q->sel_track];
    rfm_move_arp_t *a = &t->arp;
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
        }
        rfmv_seq_sync(m);
        break;
    case RFM_MOVE_SM_ARP:                               /* FIZZ's ARP: the jog the page; page 1: PATTERN RATE RANGE
                                                         * CHANCE MODE GATE OCT DIST QUANT; pages 2, 3: the MOD and
                                                         * TRANSPOSE lanes (the steps pick a lane step; knob 1 its
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
        if (!q->step_held)
            rfmv_seq_group(m);                          /* (held steps: the hold's group) */
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
        } else if (q->mods & SEQ_M_LOOP) {              /* the pages: shown white, in the loop the track's colour,
                                                         * outside with notes dim white */
            const int a = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE, b = (rfmv_seq_loop_hi(m, clip) - 1) / RFM_MOVE_SEQ_PAGE;
            int lo, hi = rfmv_seq_clip_range(m, clip, &lo), k, notes = 0;
            for (k = lo; k < hi && !notes; k++)
                notes = q->notes[k].step / RFM_MOVE_SEQ_PAGE == i;
            v = i == q->page ? RFMV_WHITE : i >= a && i <= b ? tc : notes ? sc(RFMV_WHITE, 48) : sc(RFMV_WHITE, 6);
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
            if (i == SH_FIXVEL && q->fixed_vel)
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
    w[W_LOOP] = (q->mods & SEQ_M_LOOP) ? 127 : 40;
    w[W_COPY] = (q->mods & SEQ_M_COPY) ? 127 : q->paste ? (bl ? 127 : 30) : q->clip_kind && q->cut ? 70 : 30;
    w[W_MUTE] = (q->mods & SEQ_M_MUTE) ? 127 : q->tr[q->sel_track].mute ? 100 : 30;
    w[W_DELETE] = (q->mods & SEQ_M_DELETE) ? 127 : 30;
}

/* ================================================================ the screen === */
/* (the owner's design: Session.dc.html, Track.dc.html, Projects.dc.html; symbols only, the words in screen.c's
 * popups) */
static const char *const MSG[] = { "", "NO EMPTY CLIP", "FULL", "LOAD FAILED", "CAPTURED", "UNDO", "REDO", "COPIED",
                                   "PASTED", "CUT", "QUANTIZED", "DOUBLED", "EXT TEMPO", "DUPLICATED", "CLEARED",
                                   "CAPTURE CLEARED", "CLOCK AUTO", "CLOCK INT", "CLOCK EXT", "FULL VEL",
                                   "PAD VEL", "CLICK ON", "CLICK OFF" };
const char *rfmv_seq_msg_text(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    return q->msg && rfmv_now_ms(m) - q->msg_ms < RFM_MOVE_POPUP_MS && q->msg < sizeof MSG / sizeof MSG[0] ? MSG[q->msg]
                                                                                                         : NULL;
}

/* how full a group of notes looks: 0 none, 1 .. cap (a bar's height: the bits of the count) */
static int fullness(int n, int cap)
{
    int b = 0;
    while (n > 0)
        b++, n >>= 1;
    return b < cap ? b : cap;
}

/* Projects.dc.html: the 32 slots as the pads are (top-left = 1); the current one filled, a saved one outlined, each
 * with three bars (its tracks with notes, in three groups: the notes of each, as fullness()); an empty slot a dot.
 * A project loading (the host's answer pending) blinks. */
static void projects_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    for (i = 0; i < RFM_MOVE_PROJECTS; i++) {
        const int x = (i % 8) * 16 + 1, y = (i / 8) * 16 + 1, cur = i == q->proj_cur || (i == q->proj_pending && blink(q));
        rfm_move_proj_info_t in;
        int used[RFM_MOVE_SEQ_TRACKS], nu = 0, g, t;
        if (!cur && !(q->proj_used >> i & 1)) {
            mr_gfx_rect(cv, x + 6, y + 6, 2, 2, 1, 1);
            continue;
        }
        mr_gfx_rect(cv, x, y, 14, 14, 1, cur);
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
                mr_gfx_rect(cv, x + 3 + g * 3, y + 11 - h, 2, h, !cur, 1);
        }
    }
}

/* which track of the 16: a 4 x 4 of cells (a column a bank, Track 1 on top), the selected one filled, the
 * bank's column bigger; k the cell's size */
void rfmv_seq_track_glyph(const rfm_move_t *m, mr_canvas_t *cv, int x, int y, int k)
{
    const rfm_move_seq_t *q = &m->seq;
    int b, t;
    for (b = 0; b < 4; b++)
        for (t = 0; t < 4; t++) {
            const int tr = b * 4 + t, cx = x + b * (k + 1), cy = y + t * (k + 1);
            if (tr == q->sel_track)
                mr_gfx_rect(cv, cx, cy, k, k, 1, 1);
            else if (b == q->bank)
                mr_gfx_rect(cv, cx, cy, k, k, 1, 0);
            else
                mr_gfx_pixel(cv, cx + k / 2, cy + k / 2, 1);
        }
}

/* Session.dc.html (the owner's mockup): the 8 tracks in view across, their 4 clip slots down (14 x 13 cells, 16 px
 * apart, from y 4); a 14 x 2 bar over the selected track (muted: a dotted line there); a clip empty (a 2 x 2 dot), with
 * content (an outline), playing (filled, a black play triangle), queued (a blinking outline), stopping (blinking
 * between playing and an outline), recording (filled, a black dot; blinking while it waits); the clipboard's source
 * dashed (copied) or crossed (cut) */
static void session_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int bl = blink(q);
    int c, r, i;
    for (c = 0; c < 8; c++) {
        const int track = q->track_page + c, x = c * 16 + 1;
        if (track == q->sel_track)
            mr_gfx_rect(cv, x, 0, 14, q->tr[track].mute ? 1 : 2, 1, 1);
        if (q->tr[track].mute)
            for (i = 0; i < 14; i += 2)
                mr_gfx_pixel(cv, x + i, 1, 1);
    }
    for (r = 0; r < 4; r++)
        for (c = 0; c < 8; c++) {
            const int track = q->track_page + c, scene = q->scene_off + r, x = c * 16 + 1, y = 4 + r * 15;
            int st = clip_state(m, track, scene);
            const int src = rfmv_seq_src_clip(m, SEQ_CLIP(track, scene));
            if (src) {
                if (src == 2)
                    rfmv_xbox(cv, x, y, 14, 13);
                else
                    rfmv_dotted(cv, x, y, 14, 13);
                continue;
            }
            if (st == CS_STOPPING)
                st = bl ? CS_CONTENT : CS_PLAYING;
            if (st == CS_REC_WAIT)
                st = bl ? CS_CONTENT : CS_REC;
            switch (st) {
            case CS_EMPTY: mr_gfx_rect(cv, x + 6, y + 5, 2, 2, 1, 1); break;
            case CS_QUEUED:
                if (!bl)
                    mr_gfx_rect(cv, x, y, 14, 13, 1, 0);
                break;
            case CS_PLAYING:
                mr_gfx_rect(cv, x, y, 14, 13, 1, 1);
                rfmv_tri(cv, x + 4, y + 3, 6, 8, 0);
                break;
            case CS_REC:
                mr_gfx_rect(cv, x, y, 14, 13, 1, 1);
                rfmv_disc(cv, x + 4, y + 3, 7, 0);
                break;
            default: mr_gfx_rect(cv, x, y, 14, 13, 1, 0); break;
            }
        }
}

/* the 16 steps of the page at the bottom (7 x 10, 8 px apart): notes or the playhead filled, empty outlined, past the
 * clip's end dotted. Returns the step the playhead is on (-1 none) */
static int playhead_step(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    return t->active == t->sel ? rfmv_seq_clip_pos(m, q->sel_track) - q->page * RFM_MOVE_SEQ_PAGE : -1;
}
int rfmv_seq_steps_row(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = edit_clip(m), pos = playhead_step(m), a = rfmv_seq_loop_lo(m, clip), b = rfmv_seq_loop_hi(m, clip);
    int i;
    if (q->steprec && q->step_cur / RFM_MOVE_SEQ_PAGE == q->page)   /* Step record's cursor: a bar over its step */
        mr_gfx_rect(cv, (q->step_cur % RFM_MOVE_SEQ_PAGE) * 8, 51, 7, 2, 1, 1);
    for (i = 0; i < RFM_MOVE_NSTEPS; i++) {
        const int abs = q->page * RFM_MOVE_SEQ_PAGE + i, src = rfmv_seq_src_step(m, clip, abs);
        int lo;
        const int notes = rfmv_seq_range(m, clip, abs, &lo) > lo;
        if (src == 2)                                   /* cut: an X */
            rfmv_xbox(cv, i * 8, 54, 7, 10);
        else if (src == 1)                              /* copied: dashed */
            rfmv_dotted(cv, i * 8, 54, 7, 10);
        else if (abs < a || abs >= b) {                 /* outside the loop: a line (with notes: half filled) */
            mr_gfx_rect(cv, i * 8, 63, 7, 1, 1, 1);
            if (notes)
                mr_gfx_rect(cv, i * 8, 59, 7, 5, 1, 1);
        } else
            mr_gfx_rect(cv, i * 8, 54, 7, 10, 1, i == pos || notes);
    }
    return pos;
}

/* Track.dc.html: the clip's pages on top (the shown one filled, in the loop outlined, past it dotted: 4 segments, or
 * 8 / 16 thinner ones for a longer clip or page), the page's notes as a piano roll (8 px a step, 3 px a semitone
 * up from the page's lowest note; squeezed past 13 semitones) with the beats dotted and the playhead a 1 px line, the
 * 16 steps at the bottom */
static void track_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const int clip = edit_clip(m), pa = rfmv_seq_loop_lo(m, clip) / RFM_MOVE_SEQ_PAGE;
    const int pages = (rfmv_seq_loop_hi(m, clip) + RFM_MOVE_SEQ_PAGE - 1) / RFM_MOVE_SEQ_PAGE;
    const int need = pages > q->page + 1 ? pages : q->page + 1, nseg = need <= 4 ? 4 : need <= 8 ? 8 : 16;
    const int pitch = 128 / nseg, p0 = q->page * RFM_MOVE_SEQ_PAGE;
    int i, lo, hi, nlo = 127, nhi = 0, y;
    if (q->page >= pages) {                             /* ghost pages (Move's): a line under the real pages, blank
                                                         * ghosts, a "+" at the far end; the clip's steps / pages boxed */
        char b[16];
        const int seg = 100 / (q->page + 1), xe = seg * (q->page + 1);
        mr_gfx_rect(cv, 0, 2, seg * pages - 2 > 1 ? seg * pages - 2 : 1, 1, 1, 1);
        mr_gfx_rect(cv, xe - seg / 2 - 2, 2, 5, 1, 1, 1), mr_gfx_rect(cv, xe - seg / 2, 0, 1, 5, 1, 1);
        snprintf(b, sizeof b, "%d/%d", rfmv_seq_loop_hi(m, clip) - rfmv_seq_loop_lo(m, clip), pages);
        mr_gfx_rect(cv, 127 - 6 * (int)strlen(b) - 3, 0, 6 * (int)strlen(b) + 3, 9, 1, 0);
        mr_gfx_text(cv, 127 - 6 * (int)strlen(b) - 1, 1, b, 1);
    }
    for (i = 0; i < nseg && q->page < pages; i++) {
        const int x = i * pitch + 1, w = pitch - 2;
        if (i == q->page)
            mr_gfx_rect(cv, x, 0, w, 4, 1, 1);
        else if (i >= pa && i < pages)
            mr_gfx_rect(cv, x, 0, w, 4, 1, 0);
        else
            rfmv_dotted(cv, x, 0, w, 4);
    }
    for (i = 1; i <= 4; i++)                            /* the beats */
        for (y = 7; y < 51; y += 4)
            mr_gfx_pixel(cv, i * 32 - 1, y, 1);
    hi = rfmv_seq_clip_range(m, clip, &lo);
    for (i = lo; i < hi; i++)
        if (q->notes[i].step >= p0 && q->notes[i].step < p0 + RFM_MOVE_SEQ_PAGE) {
            nlo = q->notes[i].note < nlo ? q->notes[i].note : nlo;
            nhi = q->notes[i].note > nhi ? q->notes[i].note : nhi;
        }
    for (i = lo; i < hi; i++) {
        const rfm_move_note_t *n = &q->notes[i];
        int x, w, dy;
        if (n->step < p0 || n->step >= p0 + RFM_MOVE_SEQ_PAGE)
            continue;
        x = (n->step - p0) * 8 + n->micro / 3;
        w = n->len * 8 / RFM_MOVE_SEQ_TPS - 1;
        dy = nhi - nlo <= 13 ? (n->note - nlo) * 3 : (n->note - nlo) * 39 / (nhi - nlo);
        mr_gfx_rect(cv, x, 46 - dy, w < 1 ? 1 : w, 4, 1, 1);
    }
    {
        const rfm_move_track_t *t = &q->tr[q->sel_track];
        const int tick = t->active == t->sel ? rfmv_seq_clip_tick(m, q->sel_track) : -1;
        if (tick >= 0 && tick / RFM_MOVE_SEQ_TPS >= p0 && tick / RFM_MOVE_SEQ_TPS < p0 + RFM_MOVE_SEQ_PAGE)
            mr_gfx_rect(cv, (tick - p0 * RFM_MOVE_SEQ_TPS) * 8 / RFM_MOVE_SEQ_TPS, 7, 1, 44, 1, 1);
    }

    mr_gfx_rect(cv, 116, 6, 12, 12, 0, 1);             /* which track of the 16 */
    rfmv_seq_track_glyph(m, cv, 117, 7, 2);
    rfmv_seq_steps_row(m, cv);
}

/* Mute held: the 16 tracks, two rows of 8 (with notes filled, empty outlined), a muted one crossed out; the
 * selected one underlined */
static void mute_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    int i;
    for (i = 0; i < RFM_MOVE_SEQ_TRACKS; i++) {
        const int x = (i % 8) * 16 + 1, y = (i / 8) * 32 + 2;
        if (q->tr[i].mute)
            rfmv_xbox(cv, x, y, 14, 26);
        else
            mr_gfx_rect(cv, x, y, 14, 26, 1, rfmv_seq_track_used(m, i));
        if (i == q->sel_track)
            mr_gfx_rect(cv, x, y + 28, 14, 2, 1, 1);
    }
}

/* a Shift menu (Shift + a step, Shift + Record): its values at 2x (4x the tempo), a picture where one says more */
static const char *const AP_NAMES[RFM_MOVE_AP_N] = { "UP", "DOWN", "UP DN", "DN UP", "U+D", "D+U", "CONV", "DIV",
                                                     "CON DIV", "HI UP", "HI UPDN", "LO UP", "LO UPDN", "RAND",
                                                     "RAND OTH", "SHUFFLE", "AS PLAYED" };
static const char *const AM_NAMES[RFM_MOVE_AM_N] = { "OFF", "ON", "1-ST", "ONCE", "HOLD" };
static void row2(mr_canvas_t *cv, int y, const char *s) { rfmv_big(cv, 4, y, s, 2, 1); }
static void smenu_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    char b[32];
    int i;
    mr_gfx_rect(cv, sh_step(q->smenu) * 8, 62, 7, 2, 1, 1);   /* (its step, under the screen) */
    switch (q->smenu) {
    case RFM_MOVE_SM_WORKFLOW:
        snprintf(b, sizeof b, "CH %d", t->ch + 1), row2(cv, 2, b);
        snprintf(b, sizeof b, "RATE %s", rfm_move_seq_rate_name(t->rate)), row2(cv, 21, b);
        snprintf(b, sizeof b, "QUANT %d%%", q->qstrength), row2(cv, 40, b);
        break;
    case RFM_MOVE_SM_TEMPO: {
        const int b10 = rfm_move_seq_bpm10(m);
        snprintf(b, sizeof b, "%d", (b10 + 5) / 10);
        rfmv_big(cv, 64 - rfmv_big_w(b, 4) / 2, 8, b, 4, 1);
        if (q->following) {                             /* locked to the firmware's clock: a padlock */
            mr_gfx_rect(cv, 6, 44, 14, 12, 1, 1);
            mr_gfx_rect(cv, 9, 36, 8, 9, 1, 0);
            rfmv_big(cv, 28, 44, "EXT", 2, 1);
        } else
            rfmv_tri(cv, 6, 44, 10, 12, 1), rfmv_big(cv, 22, 44, "BPM", 2, 1);
        break;
    }
    case RFM_MOVE_SM_GROOVE:                            /* two 16ths, the second late by the groove */
        mr_gfx_rect(cv, 8, 10, 4, 26, 1, 1);
        mr_gfx_rect(cv, 40 + rfmv_seq_groove_of(m, q->sel_track) * 32 / 300, 10, 4, 26, 1, 1);
        mr_gfx_rect(cv, 40, 38, 1, 3, 1, 1);
        rfmv_big(cv, 80, 14, (t->own & 1) ? "TRK" : "GLB", 2, 1);   /* (whose: the track's own or the project's) */
        snprintf(b, sizeof b, "GROOVE %d%%", rfmv_seq_groove_of(m, q->sel_track)), row2(cv, 44, b);
        break;
    case RFM_MOVE_SM_LAYOUT:
        for (i = 0; i < 4; i++)
            mr_gfx_rect(cv, i * 32 + 1, 2, 30, 8, 1, i == m->s.layout);
        row2(cv, 24, rfm_move_layout_name(m->s.layout));
        break;
    case RFM_MOVE_SM_KEYS:
        for (i = 0; i < RFM_MOVE_NSCALES; i++)
            mr_gfx_rect(cv, i * 16 + 1, 2, 14, 6, 1, i == m->s.scale);
        snprintf(b, sizeof b, "%s %s", rfmv_note_names[m->s.root % 12], rfm_move_scale_name(m->s.scale));
        rfmv_big(cv, 64 - rfmv_big_w(b, 2) / 2 > 0 ? 64 - rfmv_big_w(b, 2) / 2 : 0, 14, b, 2, 1);
        for (i = 0; i < 12; i++) {
            const int sharp = (0x54A >> i) & 1;
            mr_gfx_rect(cv, 4 + i * 10, 34, 8, sharp ? 6 : 12, 1, i == m->s.root);
        }
        row2(cv, 48, (t->own & 2) ? "TRACK" : "GLOBAL");
        break;
    case RFM_MOVE_SM_ARP:
        if (q->arp_page) {                              /* a lane: its 16 steps (past its length dotted, the picked
                                                         * one underlined), RESET and the length */
            static const char *const RS[RFM_MOVE_AR_N] = { "NORM", "NOTE", "MPAT", "TPAT" };
            static const char MODC[RFM_MOVE_AL_N1 + 1] = "x.-R<>\"#";
            const rfm_move_arp_t *a = &t->arp;
            const int L = q->arp_page - 1, len = L ? a->tlen : a->mlen;
            rfmv_big(cv, 2, 2, L ? "TRANS" : "MOD", 2, 1);
            snprintf(b, sizeof b, "%s %d", RS[a->reset % RFM_MOVE_AR_N], len), rfmv_big(cv, 68, 2, b, 2, 1);
            for (i = 0; i < RFM_MOVE_ARP_LANE; i++) {
                const int x = i * 8, v = a->lane[L][i];
                if (i >= len) {
                    rfmv_dotted(cv, x, 24, 7, 28);
                    continue;
                }
                if (L) {                                /* transpose: a bar up or down from the middle */
                    const int h = clampi(v, -48, 48) * 13 / 48;
                    mr_gfx_rect(cv, x, 38 - (h > 0 ? h : 0), 7, h > 0 ? h : h < 0 ? -h : 1, 1, 1);
                } else {
                    char c[2] = { v < RFM_MOVE_AL_N1 ? MODC[v] : (char)('1' + v - RFM_MOVE_AL_N1), 0 };
                    mr_gfx_text(cv, x + 1, 34, c, 1);
                }
                if (i == q->arp_sel)
                    mr_gfx_rect(cv, x, 54, 7, 2, 1, 1);
            }
            break;
        }
    {                             /* the pattern's walk over C E G, the mode, the rate */
        const rfm_move_arp_t *a = &t->arp;
        uint8_t walk[32];
        int n = 0, x;
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
        for (i = 0, x = 4; i < n; i++, x += 40 / n)
            mr_gfx_rect(cv, x, 22 - walk[i] * 8, 40 / n - 1 > 2 ? 40 / n - 1 : 2, 4, 1, a->mode != RFM_MOVE_AM_OFF);
        if (a->mode == RFM_MOVE_AM_OFF)
            for (i = 0; i < 48; i += 3)
                mr_gfx_pixel(cv, i, 30, 1);
        rfmv_big(cv, 50, 2, AP_NAMES[a->pat % RFM_MOVE_AP_N], 2, 1);
        snprintf(b, sizeof b, "1/%d", rfmv_seq_arp_rate_of(a->rate)), rfmv_big(cv, 50, 21, b, 2, 1);
        snprintf(b, sizeof b, "%s x%d", AM_NAMES[a->mode % RFM_MOVE_AM_N], a->range), row2(cv, 40, b);
        if (a->quant)
            snprintf(b, sizeof b, "Q1/%d", rfmv_seq_arp_rate_of(a->quant - 1)), mr_gfx_text(cv, 92, 56, b, 1);
        break;
    }
    case RFM_MOVE_SM_REC: {                             /* the mode's picture, its name, the style */
        static const char *const RM[RFM_MOVE_RM_N] = { "CAPTURE", "RECORD", "STEP" };
        if (q->rec_mode == RFM_MOVE_RM_RECORD)
            rfmv_disc(cv, 4, 4, 14, 1);
        else if (q->rec_mode == RFM_MOVE_RM_STEP)
            for (i = 0; i < 3; i++)
                mr_gfx_rect(cv, 2 + i * 6, 8, 5, 8, 1, i == 1);
        else
            rfmv_disc(cv, 4, 4, 14, 1), rfmv_disc(cv, 8, 8, 6, 0);
        rfmv_big(cv, 24, 4, RM[q->rec_mode % RFM_MOVE_RM_N], 2, 1);
        row2(cv, 32, q->rec_style == RFM_MOVE_RS_REPLACE ? "REPLACE" : "OVERDUB");
        break;
    }
    default: break;
    }
}

/* Sample's audio loop: a waveform in the top-right corner (armed: blinking) */
static void arec_glyph(const rfm_move_t *m, mr_canvas_t *cv)
{
    static const uint8_t H[9] = { 2, 5, 8, 4, 9, 6, 3, 7, 2 };
    int i;
    if (!m->seq.arec || (m->seq.arec == 1 && blink(&m->seq)))
        return;
    mr_gfx_rect(cv, 98, 0, 30, 12, 0, 1);
    for (i = 0; i < 9; i++)
        mr_gfx_rect(cv, 100 + i * 3, 6 - H[i] / 2, 2, H[i] < 1 ? 1 : H[i], 1, 1);
}

void rfmv_seq_screen(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    if (q->smenu && !q->projects)
        smenu_screen(m, cv);
    else if (q->projects)
        projects_screen(m, cv);
    else if (q->mods & SEQ_M_MUTE)
        mute_screen(m, cv);
    else if (q->view)
        session_screen(m, cv);
    else
        track_screen(m, cv);
    arec_glyph(m, cv);
}

/* Track 4 held: the layout (4 tabs: the top row's pads) and the scale (8: row 2's) on top, what changed last at 2x
 * (the track, its channel: the jog wheel, a knob's CC: the knobs, the rate: Up / Down, the layout, the scale), the
 * channel (a strip of 16), the rate (a strip of 10), and the tracks on the steps (the selected one filled, with
 * content outlined, empty dotted, a muted one flagged above) */
void rfmv_seq_track_menu(const rfm_move_t *m, mr_canvas_t *cv)
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[q->sel_track];
    char b[24];
    int i;
    for (i = 0; i < 4; i++)
        mr_gfx_rect(cv, i * 32 + 1, 0, 30, 3, 1, i == m->s.layout);
    for (i = 0; i < RFM_MOVE_NSCALES; i++)
        mr_gfx_rect(cv, i * 16 + 1, 5, 14, 3, 1, i == m->s.scale);
    switch (m->menu_what) {
    case MENU_TCH: snprintf(b, sizeof b, "CH %d", t->ch + 1); break;
    case MENU_CC: snprintf(b, sizeof b, "K%d CC%d", (m->menu_i & 7) + 1, m->s.cc[m->s.bank][m->menu_i & 7]); break;
    case MENU_RATE: snprintf(b, sizeof b, "%s", rfm_move_seq_rate_name(t->rate)); break;
    case MENU_LAYOUT: snprintf(b, sizeof b, "%s", rfm_move_layout_name(m->s.layout)); break;
    case MENU_SCALE: snprintf(b, sizeof b, "%s", rfm_move_scale_name(m->s.scale)); break;
    default: snprintf(b, sizeof b, "TR %d", q->sel_track + 1); break;
    }
    rfmv_big(cv, 64 - rfmv_big_w(b, 2) / 2, 13, b, 2, 1);
    for (i = 0; i < 16; i++)
        mr_gfx_rect(cv, i * 8, 33, 7, 5, 1, i == t->ch);
    for (i = 0; i < RFM_MOVE_SEQ_RATES; i++)
        mr_gfx_rect(cv, i * 13 + 1, 41, 11, 5, 1, i == t->rate);
    for (i = 0; i < RFM_MOVE_SEQ_TRACKS; i++) {
        if (i == q->sel_track)
            mr_gfx_rect(cv, i * 8, 54, 7, 10, 1, 1);
        else if (rfmv_seq_track_used(m, i))
            mr_gfx_rect(cv, i * 8, 54, 7, 10, 1, 0);
        else
            rfmv_dotted(cv, i * 8, 54, 7, 10);
        if (q->tr[i].mute)
            mr_gfx_rect(cv, i * 8 + 2, 50, 3, 2, 1, 1);
    }
}
