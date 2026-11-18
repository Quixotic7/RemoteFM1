/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's sequencer: the data (sorted note and lock arrays, clip lengths, tracks), the undo journal, the
 * clipboard, the clock (internal, or the firmware's MIDI clock) and the engine that plays the clips. Portable C99,
 * no allocation: everything is in rfm_move_t.seq. host/src/move/README.md, "Sequencer", has the decisions.
 *
 * Time: a tick is 1/24 of a step (96 PPQN). `tl` counts every tick the clock gives (playing or not); `T` is the
 * song position while playing. A note sits at step*24 + micro in its clip; a clip of len steps loops every
 * len*24 ticks from its start t0. */
#include <string.h>

#include "move_int.h"

#define TPS RFM_MOVE_SEQ_TPS
#define BAR RFM_MOVE_SEQ_BAR
#define clampi rfmv_clampi

/* ================================================================ the data === */
static uint32_t nkey(int clip, int step, int note) { return (uint32_t)clip << 16 | (uint32_t)step << 8 | (uint32_t)note; }
static uint32_t note_key(const rfm_move_note_t *n) { return nkey(n->clip, n->step, n->note); }
static uint32_t lock_key(const rfm_move_lock_t *l) { return nkey(l->clip, l->step, l->cc); }

/* the first note whose key is >= key */
static int note_lower(const rfm_move_seq_t *q, uint32_t key)
{
    int lo = 0, hi = q->nnotes;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (note_key(&q->notes[mid]) < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static int lock_lower(const rfm_move_seq_t *q, uint32_t key)
{
    int lo = 0, hi = q->nlocks;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (lock_key(&q->locks[mid]) < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int rfmv_seq_range(const rfm_move_t *m, int clip, int step, int *lo)
{
    *lo = note_lower(&m->seq, nkey(clip, step, 0));
    return note_lower(&m->seq, nkey(clip, step, 0) + 256);
}
int rfmv_seq_clip_range(const rfm_move_t *m, int clip, int *lo)
{
    *lo = note_lower(&m->seq, nkey(clip, 0, 0));
    return note_lower(&m->seq, nkey(clip + 1, 0, 0));
}
int rfmv_seq_lock_range(const rfm_move_t *m, int clip, int step, int *lo)
{
    *lo = lock_lower(&m->seq, nkey(clip, step, 0));
    return lock_lower(&m->seq, nkey(clip, step, 0) + 256);
}
int rfmv_seq_clip_lock_range(const rfm_move_t *m, int clip, int *lo)
{
    *lo = lock_lower(&m->seq, nkey(clip, 0, 0));
    return lock_lower(&m->seq, nkey(clip + 1, 0, 0));
}
int rfmv_seq_find(const rfm_move_t *m, int clip, int step, int note)
{
    const uint32_t k = nkey(clip, step, note);
    const int i = note_lower(&m->seq, k);
    return i < m->seq.nnotes && note_key(&m->seq.notes[i]) == k ? i : -1;
}
int rfmv_seq_lock_get(const rfm_move_t *m, int clip, int step, int cc)
{
    const uint32_t k = nkey(clip, step, cc);
    const int i = lock_lower(&m->seq, k);
    return i < m->seq.nlocks && lock_key(&m->seq.locks[i]) == k ? m->seq.locks[i].val : -1;
}
int rfmv_seq_clip_used(const rfm_move_t *m, int clip)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo), llo, lhi = rfmv_seq_clip_lock_range(m, clip, &llo);
    return hi > lo || lhi > llo;
}
int rfmv_seq_track_used(const rfm_move_t *m, int track)
{
    int lo, hi, llo, lhi;
    lo = note_lower(&m->seq, nkey(SEQ_CLIP(track, 0), 0, 0));
    hi = note_lower(&m->seq, nkey(SEQ_CLIP(track + 1, 0), 0, 0));
    llo = lock_lower(&m->seq, nkey(SEQ_CLIP(track, 0), 0, 0));
    lhi = lock_lower(&m->seq, nkey(SEQ_CLIP(track + 1, 0), 0, 0));
    return hi > lo || lhi > llo;
}

/* raw edits (no journal): 0 done, -1 no room / nothing there */
static int raw_put(rfm_move_seq_t *q, const rfm_move_note_t *n)
{
    const uint32_t k = note_key(n);
    const int i = note_lower(q, k);
    if (i < q->nnotes && note_key(&q->notes[i]) == k) {
        q->notes[i] = *n;
        return 0;
    }
    if (q->nnotes >= RFM_MOVE_SEQ_NOTES)
        return -1;
    memmove(&q->notes[i + 1], &q->notes[i], (size_t)(q->nnotes - i) * sizeof q->notes[0]);
    q->notes[i] = *n;
    q->nnotes++;
    return 0;
}
static int raw_del(rfm_move_seq_t *q, int clip, int step, int note)
{
    const uint32_t k = nkey(clip, step, note);
    const int i = note_lower(q, k);
    if (i >= q->nnotes || note_key(&q->notes[i]) != k)
        return -1;
    memmove(&q->notes[i], &q->notes[i + 1], (size_t)(q->nnotes - i - 1) * sizeof q->notes[0]);
    q->nnotes--;
    return 0;
}
static int raw_lock(rfm_move_seq_t *q, int clip, int step, int cc, int val)
{
    rfm_move_lock_t l;
    const uint32_t k = nkey(clip, step, cc);
    const int i = lock_lower(q, k);
    const int there = i < q->nlocks && lock_key(&q->locks[i]) == k;
    if (val < 0) {
        if (!there)
            return -1;
        memmove(&q->locks[i], &q->locks[i + 1], (size_t)(q->nlocks - i - 1) * sizeof q->locks[0]);
        q->nlocks--;
        return 0;
    }
    l.clip = (uint8_t)clip, l.step = (uint8_t)step, l.cc = (uint8_t)cc, l.val = (uint8_t)val;
    if (there) {
        q->locks[i] = l;
        return 0;
    }
    if (q->nlocks >= RFM_MOVE_SEQ_LOCKS)
        return -1;
    memmove(&q->locks[i + 1], &q->locks[i], (size_t)(q->nlocks - i) * sizeof q->locks[0]);
    q->locks[i] = l;
    q->nlocks++;
    return 0;
}

/* ================================================================ the journal === */
void rfmv_seq_group(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    if (++q->group == 0)
        q->group = 1;
}
void rfmv_seq_dirty(rfm_move_t *m)
{
    m->seq.dirty = 1;
    m->seq.dirty_us = m->seq.now_us;
}

static void jot(rfm_move_t *m, int type, const rfm_move_note_t *n, int old, int nu)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_op_t *o;
    rfmv_seq_dirty(m);
    if (q->broken && q->broken == q->group)
        return;                                         /* this group overflowed the journal: not undoable */
    q->utop = q->un;                                    /* a new edit: no redo */
    if (q->un >= RFM_MOVE_SEQ_UNDO) {                   /* full: the oldest group goes */
        const uint16_t g = q->ops[0].group;
        int i = 0;
        while (i < q->un && q->ops[i].group == g)
            i++;
        if (i >= q->un || g == q->group) {
            q->un = q->utop = 0;
            q->broken = q->group;
            return;
        }
        memmove(&q->ops[0], &q->ops[i], (size_t)(q->un - i) * sizeof q->ops[0]);
        q->un = (uint16_t)(q->un - i);
    }
    o = &q->ops[q->un++];
    memset(o, 0, sizeof *o);
    o->group = q->group, o->type = (uint8_t)type, o->old = (uint16_t)old, o->nu = (uint16_t)nu;
    if (n)
        o->n = *n;
    q->utop = q->un;
}

int rfmv_seq_note_put(rfm_move_t *m, int clip, int step, int note, int vel, int len, int micro)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_note_t n;
    int i, lo, hi;
    if (clip < 0 || clip >= RFM_MOVE_SEQ_CLIPS || step < 0 || step >= RFM_MOVE_SEQ_MAXLEN || note < 0 || note > 127)
        return -1;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip, n.step = (uint8_t)step, n.note = (uint8_t)note, n.vel = (uint8_t)clampi(vel, 1, 127);
    n.micro = (int8_t)clampi(micro, -TPS / 2, TPS / 2 - 1);
    n.len = (uint16_t)clampi(len, 1, RFM_MOVE_SEQ_MAXLEN * TPS);
    i = rfmv_seq_find(m, clip, step, note);
    if (i >= 0) {
        if (!memcmp(&q->notes[i], &n, sizeof n))
            return 0;
        jot(m, OP_NOTE_DEL, &q->notes[i], 0, 0);
    } else {
        hi = rfmv_seq_range(m, clip, step, &lo);
        if (hi - lo >= RFM_MOVE_SEQ_POLY || q->nnotes >= RFM_MOVE_SEQ_NOTES)
            return -1;
    }
    raw_put(q, &n);
    jot(m, OP_NOTE_ADD, &n, 0, 0);
    return 0;
}
int rfmv_seq_note_del(rfm_move_t *m, int clip, int step, int note)
{
    const int i = rfmv_seq_find(m, clip, step, note);
    rfm_move_note_t n;
    if (i < 0)
        return -1;
    n = m->seq.notes[i];
    raw_del(&m->seq, clip, step, note);
    jot(m, OP_NOTE_DEL, &n, 0, 0);
    return 0;
}
int rfmv_seq_lock_set(rfm_move_t *m, int clip, int step, int cc, int val)
{
    const int old = rfmv_seq_lock_get(m, clip, step, cc);
    rfm_move_note_t n;
    if (clip < 0 || clip >= RFM_MOVE_SEQ_CLIPS || step < 0 || step >= RFM_MOVE_SEQ_MAXLEN || cc < 0 || cc > 127)
        return -1;
    val = val < 0 ? -1 : clampi(val, 0, 127);
    if (old == val)
        return 0;
    if (raw_lock(&m->seq, clip, step, cc, val) < 0)
        return -1;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip, n.step = (uint8_t)step, n.note = (uint8_t)cc;
    jot(m, OP_LOCK, &n, old < 0 ? SEQ_NOLOCK : old, val < 0 ? SEQ_NOLOCK : val);
    return 0;
}
void rfmv_seq_len_set(rfm_move_t *m, int clip, int len)
{
    rfm_move_note_t n;
    const int old = m->seq.len[clip];
    len = clampi(len, 1, RFM_MOVE_SEQ_MAXLEN);
    if (old == len)
        return;
    m->seq.len[clip] = (uint16_t)len;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip;
    jot(m, OP_LEN, &n, old, len);
}
void rfmv_seq_step_clear(rfm_move_t *m, int clip, int step)
{
    int lo, hi = rfmv_seq_range(m, clip, step, &lo);
    while (hi-- > lo)
        rfmv_seq_note_del(m, clip, step, m->seq.notes[hi].note);
    hi = rfmv_seq_lock_range(m, clip, step, &lo);
    while (hi-- > lo)
        rfmv_seq_lock_set(m, clip, step, m->seq.locks[hi].cc, -1);
}
void rfmv_seq_clip_clear(rfm_move_t *m, int clip)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo);
    while (hi-- > lo)
        rfmv_seq_note_del(m, clip, m->seq.notes[hi].step, m->seq.notes[hi].note);
    hi = rfmv_seq_clip_lock_range(m, clip, &lo);
    while (hi-- > lo)
        rfmv_seq_lock_set(m, clip, m->seq.locks[hi].step, m->seq.locks[hi].cc, -1);
    rfmv_seq_len_set(m, clip, RFM_MOVE_SEQ_PAGE);
}
void rfmv_seq_clip_quantize(rfm_move_t *m, int clip)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo), i;
    for (i = lo; i < hi; i++) {                         /* (a replace keeps every index) */
        const rfm_move_note_t n = m->seq.notes[i];
        if (n.micro)
            rfmv_seq_note_put(m, clip, n.step, n.note, n.vel, n.len, 0);
    }
}
int rfmv_seq_clip_double(rfm_move_t *m, int clip)
{
    rfm_move_seq_t *q = &m->seq;
    const int L = q->len[clip];
    int lo, hi, i, s;
    if (L * 2 > RFM_MOVE_SEQ_MAXLEN)
        return -1;
    for (s = L; s < 2 * L; s++)                         /* whatever was hidden past the end goes */
        rfmv_seq_step_clear(m, clip, s);
    hi = rfmv_seq_clip_range(m, clip, &lo);
    for (i = lo; i < hi && q->notes[i].step < L; i++) { /* (the copies sort after the range: indices hold) */
        const rfm_move_note_t n = q->notes[i];
        rfmv_seq_note_put(m, clip, n.step + L, n.note, n.vel, n.len, n.micro);
    }
    hi = rfmv_seq_clip_lock_range(m, clip, &lo);
    for (i = lo; i < hi && q->locks[i].step < L; i++) {
        const rfm_move_lock_t l = q->locks[i];
        rfmv_seq_lock_set(m, clip, l.step + L, l.cc, l.val);
    }
    rfmv_seq_len_set(m, clip, 2 * L);
    return 0;
}
void rfmv_seq_note_remove_all(rfm_move_t *m, int clip, int note)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo);
    while (hi-- > lo)
        if (m->seq.notes[hi].note == note)
            rfmv_seq_note_del(m, clip, m->seq.notes[hi].step, note);
}

static void op_apply(rfm_move_t *m, const rfm_move_op_t *o, int forward)
{
    rfm_move_seq_t *q = &m->seq;
    switch (o->type) {
    case OP_NOTE_ADD:
    case OP_NOTE_DEL:
        if ((o->type == OP_NOTE_ADD) == !!forward)
            raw_put(q, &o->n);
        else
            raw_del(q, o->n.clip, o->n.step, o->n.note);
        break;
    case OP_LOCK: {
        const int v = forward ? o->nu : o->old;
        raw_lock(q, o->n.clip, o->n.step, o->n.note, v == SEQ_NOLOCK ? -1 : v);
        break;
    }
    case OP_LEN: q->len[o->n.clip] = forward ? o->nu : o->old; break;
    default: break;
    }
}
int rfmv_seq_undo(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    uint16_t g;
    if (!q->un)
        return 0;
    g = q->ops[q->un - 1].group;
    while (q->un && q->ops[q->un - 1].group == g)
        op_apply(m, &q->ops[--q->un], 0);
    rfmv_seq_group(m);                                  /* (what follows is a new gesture) */
    rfmv_seq_dirty(m);
    return 1;
}
int rfmv_seq_redo(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    uint16_t g;
    if (q->un >= q->utop)
        return 0;
    g = q->ops[q->un].group;
    while (q->un < q->utop && q->ops[q->un].group == g)
        op_apply(m, &q->ops[q->un++], 1);
    rfmv_seq_group(m);
    rfmv_seq_dirty(m);
    return 1;
}

/* ================================================================ the clipboard === */
void rfmv_seq_copy_step(rfm_move_t *m, int clip, int step)
{
    rfm_move_seq_t *q = &m->seq;
    int lo, hi = rfmv_seq_range(m, clip, step, &lo), i;
    q->clip_kind = 1, q->cut = 0, q->clip_nn = q->clip_nl = 0;
    for (i = lo; i < hi; i++)
        q->clip_notes[q->clip_nn++] = q->notes[i];
    hi = rfmv_seq_lock_range(m, clip, step, &lo);
    for (i = lo; i < hi; i++)
        q->clip_locks[q->clip_nl++] = q->locks[i];
}
void rfmv_seq_copy_clip(rfm_move_t *m, int clip)
{
    rfm_move_seq_t *q = &m->seq;
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo), i;
    q->clip_kind = 2, q->cut = 0, q->clip_nn = q->clip_nl = 0, q->clip_len = q->len[clip];
    for (i = lo; i < hi && q->clip_nn < RFM_MOVE_SEQ_POLY * RFM_MOVE_SEQ_MAXLEN; i++)
        q->clip_notes[q->clip_nn++] = q->notes[i];
    hi = rfmv_seq_clip_lock_range(m, clip, &lo);
    for (i = lo; i < hi && q->clip_nl < RFM_MOVE_SEQ_LOCKS; i++)
        q->clip_locks[q->clip_nl++] = q->locks[i];
}
void rfmv_seq_paste_step(rfm_move_t *m, int clip, int step)
{
    rfm_move_seq_t *q = &m->seq;
    int i;
    if (q->clip_kind != 1)
        return;
    rfmv_seq_step_clear(m, clip, step);
    for (i = 0; i < q->clip_nn; i++) {
        const rfm_move_note_t *n = &q->clip_notes[i];
        rfmv_seq_note_put(m, clip, step, n->note, n->vel, n->len, n->micro);
    }
    for (i = 0; i < q->clip_nl; i++)
        rfmv_seq_lock_set(m, clip, step, q->clip_locks[i].cc, q->clip_locks[i].val);
}
void rfmv_seq_paste_clip(rfm_move_t *m, int clip)
{
    rfm_move_seq_t *q = &m->seq;
    int i;
    if (q->clip_kind != 2)
        return;
    rfmv_seq_clip_clear(m, clip);
    rfmv_seq_len_set(m, clip, q->clip_len);
    for (i = 0; i < q->clip_nn; i++) {
        const rfm_move_note_t *n = &q->clip_notes[i];
        rfmv_seq_note_put(m, clip, n->step, n->note, n->vel, n->len, n->micro);
    }
    for (i = 0; i < q->clip_nl; i++)
        rfmv_seq_lock_set(m, clip, q->clip_locks[i].step, q->clip_locks[i].cc, q->clip_locks[i].val);
}
void rfmv_seq_track_copy(rfm_move_t *m, int from, int to)
{
    int s;
    if (from == to || from < 0 || to < 0 || from >= RFM_MOVE_SEQ_TRACKS || to >= RFM_MOVE_SEQ_TRACKS)
        return;
    rfmv_seq_release(m, to);
    for (s = 0; s < RFM_MOVE_SEQ_SCENES; s++) {         /* clip by clip through the clipboard */
        rfmv_seq_copy_clip(m, SEQ_CLIP(from, s));
        rfmv_seq_paste_clip(m, SEQ_CLIP(to, s));
    }
    m->seq.clip_kind = 0;
}
/* a clip's notes, locks and length onto another (cleared first), not through the clipboard */
void rfmv_seq_clip_dup(rfm_move_t *m, int from, int to)
{
    rfm_move_seq_t *q = &m->seq;
    int lo, hi, k, n;
    if (from == to || from < 0 || to < 0 || from >= RFM_MOVE_SEQ_CLIPS || to >= RFM_MOVE_SEQ_CLIPS)
        return;
    rfmv_seq_clip_clear(m, to);
    rfmv_seq_len_set(m, to, q->len[from]);
    n = rfmv_seq_clip_range(m, from, &lo) - lo;
    for (k = 0; k < n; k++) {                           /* (the range moves when `to` sorts before it) */
        rfm_move_note_t c;
        rfmv_seq_clip_range(m, from, &lo);
        c = q->notes[lo + k];
        rfmv_seq_note_put(m, to, c.step, c.note, c.vel, c.len, c.micro);
    }
    n = rfmv_seq_clip_lock_range(m, from, &lo) - lo;
    for (k = 0; k < n; k++) {
        rfm_move_lock_t c;
        rfmv_seq_clip_lock_range(m, from, &lo);
        c = q->locks[lo + k];
        rfmv_seq_lock_set(m, to, c.step, c.cc, c.val);
    }
    (void)hi;
}

void rfmv_seq_track_clear(rfm_move_t *m, int track)
{
    int s;
    rfmv_seq_release(m, track);
    for (s = 0; s < RFM_MOVE_SEQ_SCENES; s++)
        rfmv_seq_clip_clear(m, SEQ_CLIP(track, s));
}

/* ================================================================ init === */
void rfmv_seq_project_clear(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    int t, c;
    q->bpm10 = 1200, q->clock_mode = RFM_MOVE_CLOCK_AUTO, q->fixed_vel = 0, q->sel_track = 0;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++) {
        memset(&q->tr[t], 0, sizeof q->tr[t]);
        q->tr[t].ch = (uint8_t)t, q->tr[t].active = 0, q->tr[t].sel = 0, q->tr[t].queued = SEQ_NONE;
    }
    for (c = 0; c < RFM_MOVE_SEQ_CLIPS; c++)
        q->len[c] = RFM_MOVE_SEQ_PAGE;
    q->nnotes = q->nlocks = 0;
    q->un = q->utop = 0, q->broken = 0;
    q->clip_kind = 0, q->cut = 0;
    q->page = 0, q->scene_off = 0, q->track_page = 0;
    q->dirty = 0;
}

void rfmv_seq_init(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    memset(q, 0, sizeof *q);
    rfmv_seq_project_clear(m);
    q->tl = -1, q->ext_clocks = -1, q->ext_win_n = -1;
    q->den_rate = 0;
    q->proj_cur = 0, q->proj_pending = -1;
    q->nlast = 0;
    q->step_first = -1;
    q->group = 1;
}

/* ================================================================ the engine === */
static void snd_off(rfm_move_t *m, int i)
{
    rfm_move_seq_t *q = &m->seq;
    rfmv_emit_midi(m, 0x80 | q->snd[i].ch, q->snd[i].note, 0);
    q->snd[i] = q->snd[--q->nsnd];
}
void rfmv_seq_release(rfm_move_t *m, int track)
{
    rfm_move_seq_t *q = &m->seq;
    int i = 0;
    while (i < q->nsnd)
        if (track < 0 || q->snd[i].track == track)
            snd_off(m, i);
        else
            i++;
}
static void note_on(rfm_move_t *m, int track, int note, int vel, int len)
{
    rfm_move_seq_t *q = &m->seq;
    const int ch = q->tr[track].ch;
    int i, old = 0;
    for (i = 0; i < q->nsnd; i++)
        if (q->snd[i].ch == ch && q->snd[i].note == note) {
            snd_off(m, i);                              /* the same note again: off first */
            break;
        }
    if (q->nsnd >= RFM_MOVE_SEQ_SOUNDING) {             /* full: the one due to end first ends now */
        for (i = 1; i < q->nsnd; i++)
            if (q->snd[i].off < q->snd[old].off)
                old = i;
        snd_off(m, old);
    }
    rfmv_emit_midi(m, 0x90 | ch, note, vel);
    q->snd[q->nsnd].ch = (uint8_t)ch, q->snd[q->nsnd].note = (uint8_t)note, q->snd[q->nsnd].track = (uint8_t)track;
    q->snd[q->nsnd].off = q->tl + len;
    q->nsnd++;
}

/* the active clip's local tick at song tick T (-1 none) */
static int clip_lt(const rfm_move_seq_t *q, int track, int32_t T)
{
    const rfm_move_track_t *t = &q->tr[track];
    int32_t d;
    if (t->active < 0)
        return -1;
    d = T - t->t0;
    if (d < 0)
        return -1;
    if (t->grow == 1)
        return d < RFM_MOVE_SEQ_MAXLEN * TPS ? (int)d : (int)(d % (RFM_MOVE_SEQ_MAXLEN * TPS));
    return (int)(d % ((int32_t)q->len[SEQ_CLIP(track, t->active)] * TPS));
}
/* a local tick as (step, micro) in a clip of len steps */
static void to_step(int lt, int len, int *step, int *micro)
{
    int s = (lt + TPS / 2) / TPS;
    int d = lt - s * TPS;
    if (s >= len)
        s -= len;
    *step = s, *micro = d;
}

static void grow_finish(rfm_move_t *m, int track, int bars)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[track];
    const int clip = SEQ_CLIP(track, t->active < 0 ? t->sel : t->active);
    const uint16_t g = q->group;
    rfm_move_note_t n;
    bars = clampi(bars, 1, RFM_MOVE_SEQ_PAGES);
    t->grow = 0, t->grow_stop = 0;
    q->group = q->rec_group;                            /* (part of the take) */
    q->len[clip] = RFM_MOVE_SEQ_PAGE;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip;
    if (bars * RFM_MOVE_SEQ_PAGE != RFM_MOVE_SEQ_PAGE) {
        q->len[clip] = (uint16_t)(bars * RFM_MOVE_SEQ_PAGE);
        jot(m, OP_LEN, &n, RFM_MOVE_SEQ_PAGE, bars * RFM_MOVE_SEQ_PAGE);
    }
    q->group = g;
}

static void launch_now(rfm_move_t *m, int track, int scene, int32_t T)
{
    rfm_move_track_t *t = &m->seq.tr[track];
    rfmv_seq_release(m, track);
    t->queued = SEQ_NONE;
    if (scene < 0) {
        if (t->grow == 1)
            grow_finish(m, track, (T - t->t0 + BAR - 1) / BAR);
        t->active = -1, t->grow = 0;
        return;
    }
    t->active = (int8_t)scene, t->sel = (int8_t)scene, t->t0 = T;
    if (t->grow == 2) {                                 /* recording into an empty clip starts here */
        t->grow = 1;
        m->seq.len[SEQ_CLIP(track, scene)] = RFM_MOVE_SEQ_PAGE;
    }
}

static void play_tick(rfm_move_t *m, int32_t T)
{
    rfm_move_seq_t *q = &m->seq;
    int tr, i, lo, hi;
    if (T % BAR == 0)
        for (tr = 0; tr < RFM_MOVE_SEQ_TRACKS; tr++) {
            rfm_move_track_t *t = &q->tr[tr];
            if (t->grow == 1 && T > t->t0) {            /* a recording clip: a bar more, or done */
                const int k = (int)((T - t->t0) / BAR);
                if (t->grow_stop || k >= RFM_MOVE_SEQ_PAGES)
                    grow_finish(m, tr, k);
                else
                    q->len[SEQ_CLIP(tr, t->active)] = (uint16_t)((k + 1) * RFM_MOVE_SEQ_PAGE);
                if (t->grow == 0)
                    t->t0 = T - (T - t->t0) % ((int32_t)q->len[SEQ_CLIP(tr, t->active)] * TPS);
            }
            if (t->queued != SEQ_NONE)
                launch_now(m, tr, t->queued, T);
        }
    for (tr = 0; tr < RFM_MOVE_SEQ_TRACKS; tr++) {
        const rfm_move_track_t *t = &q->tr[tr];
        int lt, step, micro, clip;
        if (t->active < 0 || t->mute || (lt = clip_lt(q, tr, T)) < 0)
            continue;
        clip = SEQ_CLIP(tr, t->active);
        to_step(lt, t->grow == 1 ? RFM_MOVE_SEQ_MAXLEN : q->len[clip], &step, &micro);
        if (step >= RFM_MOVE_SEQ_MAXLEN)
            continue;
        hi = rfmv_seq_range(m, clip, step, &lo);
        for (i = lo; i < hi; i++) {
            const rfm_move_note_t n = q->notes[i];      /* (note_on cannot change the notes) */
            if (n.micro == micro)
                note_on(m, tr, n.note, n.vel, n.len);
        }
        if (micro == 0) {
            hi = rfmv_seq_lock_range(m, clip, step, &lo);
            for (i = lo; i < hi; i++)
                rfmv_emit_midi(m, 0xB0 | t->ch, q->locks[i].cc, q->locks[i].val);
        }
    }
}

static void begin(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    int t;
    q->playing = 1, q->start_pending = 0, q->T = -1;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++) {
        q->tr[t].t0 = 0;
        if (q->tr[t].grow == 2 && q->tr[t].active >= 0 && q->tr[t].queued == SEQ_NONE)
            q->tr[t].queued = q->tr[t].active;          /* (so the launch at T 0 starts the recording) */
    }
}

static void tick(rfm_move_t *m, uint32_t frame)
{
    rfm_move_seq_t *q = &m->seq;
    int i = 0;
    q->frame = frame;
    q->tl++;
    if (q->start_pending == 1 || (q->start_pending == 2 && q->tl % BAR == 0) ||
        (q->start_pending == 3 && --q->count_in <= 0))
        begin(m);
    while (i < q->nsnd)                                 /* note offs before note ons */
        if (q->snd[i].off <= q->tl)
            snd_off(m, i);
        else
            i++;
    if (q->playing)
        play_tick(m, ++q->T);
}

void rfmv_seq_start(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    if (q->playing || q->start_pending)
        return;
    q->start_pending = q->following ? 2 : 1;            /* following: at the external clock's next bar */
}

/* Rec from stopped (Move's count-in): the transport starts a bar from now; following an external clock, at its
 * next bar (as Play) */
void rfmv_seq_count_in(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    if (q->playing || q->start_pending)
        return;
    if (q->following) {
        rfmv_seq_start(m);
        return;
    }
    q->start_pending = 3, q->count_in = BAR + 1;       /* (the first tick is due at once: a bar of ticks to T 0) */
}

static void rec_finish(rfm_move_t *m, int pad);

/* Shift + Play (Move's retrigger): every playing clip from its start, at once (following: the clock's next bar) */
void rfmv_seq_restart(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    const uint8_t rec = q->rec;
    if (!q->playing) {
        rfmv_seq_start(m);
        return;
    }
    rfmv_seq_stop(m);
    q->rec = rec;
    rfmv_seq_start(m);
}

void rfmv_seq_stop(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    int t, p;
    for (p = 0; p < RFM_MOVE_NPADS; p++)
        if (q->recn[p].on)
            rec_finish(m, p);
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++) {
        rfm_move_track_t *tr = &q->tr[t];
        if (tr->grow == 1)
            grow_finish(m, t, q->playing ? (q->T - tr->t0) / BAR + 1 : 1);
        tr->grow = 0, tr->grow_stop = 0;
        if (tr->queued != SEQ_NONE) {                   /* a pending launch happens now */
            tr->active = tr->queued == SEQ_STOP ? -1 : tr->queued;
            if (tr->active >= 0)
                tr->sel = tr->active;
            tr->queued = SEQ_NONE;
        }
    }
    q->playing = 0, q->start_pending = 0, q->rec = 0;
    rfmv_seq_release(m, -1);
}

void rfmv_seq_launch(rfm_move_t *m, int track, int scene)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[track];
    if (scene >= 0)
        t->sel = (int8_t)scene;
    if (!q->playing && !q->start_pending) {
        rfmv_seq_release(m, track);
        t->active = (int8_t)scene, t->queued = SEQ_NONE, t->t0 = 0;
        if (scene < 0)
            t->grow = 0;
        return;
    }
    t->queued = (int8_t)(scene < 0 ? SEQ_STOP : scene);
}
void rfmv_seq_launch_scene(rfm_move_t *m, int scene)
{
    int t;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
        rfmv_seq_launch(m, t, rfmv_seq_clip_used(m, SEQ_CLIP(t, scene)) ? scene : -1);
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
        m->seq.tr[t].sel = (int8_t)scene;
}
void rfmv_seq_mute(rfm_move_t *m, int track, int on)
{
    m->seq.tr[track].mute = (uint8_t)!!on;
    if (on)
        rfmv_seq_release(m, track);
    rfmv_seq_dirty(m);
}

int rfmv_seq_now_tick(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    const uint64_t den = (uint64_t)q->den_rate * 600u;
    if (!q->playing)
        return -1;
    return q->T + (den && q->ph * 2 >= den ? 1 : 0);
}
int rfmv_seq_clip_pos(const rfm_move_t *m, int track)
{
    const rfm_move_seq_t *q = &m->seq;
    const int lt = q->playing ? clip_lt(q, track, q->T < 0 ? 0 : q->T) : -1;
    return lt < 0 ? -1 : lt / TPS;
}

/* ================================================================ the clock === */
static int ext_bpm10(const rfm_move_seq_t *q)
{
    return q->ext_period_us ? (int)(25000000u / q->ext_period_us) : q->bpm10;
}
int rfm_move_seq_bpm10(const rfm_move_t *m) { return m->seq.following ? ext_bpm10(&m->seq) : m->seq.bpm10; }
int rfm_move_seq_following(const rfm_move_t *m) { return m->seq.following; }

void rfm_move_seq_set_bpm(rfm_move_t *m, int bpm10)
{
    bpm10 = clampi(bpm10, RFM_MOVE_SEQ_BPM10_MIN, RFM_MOVE_SEQ_BPM10_MAX);
    if (bpm10 != m->seq.bpm10) {
        m->seq.bpm10 = (uint16_t)bpm10;
        rfmv_seq_dirty(m);
    }
}

static void follow(rfm_move_seq_t *q)
{
    if (q->following)
        return;
    q->following = 1;
    q->ext_clocks = q->tl >= 0 ? q->tl / 4 - 1 : -1;    /* the next clock is the next 4 ticks */
    q->ext_win_n = -1;
}

void rfm_move_seq_set_clock(rfm_move_t *m, int mode)
{
    rfm_move_seq_t *q = &m->seq;
    if (mode < 0 || mode > RFM_MOVE_CLOCK_EXTERNAL)
        return;
    q->clock_mode = (uint8_t)mode;
    if (mode == RFM_MOVE_CLOCK_INTERNAL)
        q->following = 0;
    else if (mode == RFM_MOVE_CLOCK_EXTERNAL)
        follow(q);
    rfmv_seq_dirty(m);
}

void rfm_move_seq_play(rfm_move_t *m, int on)
{
    if (on)
        rfmv_seq_start(m);
    else
        rfmv_seq_stop(m);
}

/* the clock jumps (start, song position): what sounds ends, the capture's times are void */
static void relocate(rfm_move_t *m, int32_t tl, int32_t clocks)
{
    rfm_move_seq_t *q = &m->seq;
    rfmv_seq_release(m, -1);
    q->tl = tl, q->ext_clocks = clocks, q->ext_win_n = -1;
    q->ncap = 0;
}

void rfm_move_seq_midi_in(rfm_move_t *m, const uint8_t *msg, int n)
{
    rfm_move_seq_t *q = &m->seq;
    if (!msg || n < 1 || q->clock_mode == RFM_MOVE_CLOCK_INTERNAL)
        return;
    q->frame = 0;
    switch (msg[0]) {
    case 0xF8: {
        int32_t target;
        follow(q);
        q->ext_last_us = q->now_us;
        q->ext_clocks++;
        if (q->ext_win_n < 0 || q->ext_clocks < q->ext_win_n)
            q->ext_win_n = q->ext_clocks, q->ext_win_us = q->now_us;
        else if (q->ext_clocks - q->ext_win_n >= (q->ext_period_us ? 24 : 6) && q->now_us > q->ext_win_us) {
            q->ext_period_us = (uint32_t)((q->now_us - q->ext_win_us) / (uint64_t)(q->ext_clocks - q->ext_win_n));
            q->ext_win_n = q->ext_clocks, q->ext_win_us = q->now_us;
        }
        target = q->ext_clocks * 4;
        while (q->tl < target)                          /* catch up: this clock is due now */
            tick(m, 0);
        q->ph = 0;
        break;
    }
    case 0xFA:                                          /* start: bar 1 on the next clock */
        follow(q);
        q->ext_run = 1;
        rfmv_seq_stop(m);
        relocate(m, -1, -1);
        q->start_pending = 2;
        break;
    case 0xFB:                                          /* continue: from the song position, at its next bar */
        follow(q);
        q->ext_run = 1;
        if (!q->playing) {
            relocate(m, (int32_t)q->spp * TPS - 1, (int32_t)q->spp * 6 - 1);
            q->start_pending = 2;
        }
        break;
    case 0xFC:
        q->ext_run = 0;
        if (q->following)
            rfmv_seq_stop(m);
        break;
    case 0xF2:
        if (n >= 3) {
            q->spp = (uint16_t)((msg[1] & 127) | (msg[2] & 127) << 7);
            if (q->following && !q->playing)
                relocate(m, (int32_t)q->spp * TPS - 1, (int32_t)q->spp * 6 - 1);
        }
        break;
    default: break;
    }
}

void rfm_move_seq_advance(rfm_move_t *m, uint32_t frames, uint32_t rate)
{
    rfm_move_seq_t *q = &m->seq;
    uint64_t den, num, cur = 0;
    int32_t cap;
    if (!rate)
        return;
    if (q->den_rate != rate) {                          /* a new rate: the progress kept in proportion */
        if (q->den_rate)
            q->ph = q->ph * rate / q->den_rate;
        q->den_rate = rate;
    }
    den = (uint64_t)rate * 600u;
    num = (uint64_t)(q->following ? clampi(ext_bpm10(q), 10, 30000) : q->bpm10) * 96u;
    cap = q->following ? q->ext_clocks * 4 + 3 : INT32_MAX;
    if (q->start_pending == 1 || (q->start_pending == 3 && q->count_in > BAR))
        q->ph = den;                                    /* Play (or a count-in): the first tick is due at once */
    for (;;) {
        uint64_t need;
        if (q->ph >= den) {
            if (q->tl >= cap) {                         /* following: wait for the next clock */
                q->ph = den;
                break;
            }
            tick(m, (uint32_t)cur);
            q->ph -= den;
            continue;
        }
        need = (den - q->ph + num - 1) / num;
        if (cur + need >= frames) {                     /* (a tick at `frames` is the next call's frame 0) */
            q->ph += (frames - cur) * num;
            break;
        }
        cur += need;
        q->ph += need * num;
    }
    q->frame = 0;
    q->us_rem += (uint64_t)frames * 1000000u;
    q->now_us += q->us_rem / rate;
    q->us_rem %= rate;
    if (q->following && q->clock_mode == RFM_MOVE_CLOCK_AUTO && q->now_us - q->ext_last_us > 500000u)
        q->following = 0;                               /* the external clock is gone: ours again */
    rfmv_proj_poll(m);
}

/* ================================================================ recording, capture, tap === */
static int32_t tl_now(const rfm_move_seq_t *q)
{
    const uint64_t den = (uint64_t)q->den_rate * 600u;
    return q->tl + (den && q->ph * 2 >= den ? 1 : 0);
}

static void cap_push(rfm_move_seq_t *q, int note, int vel)
{
    rfm_move_cap_t *c = &q->cap[q->cap_head];
    c->t = tl_now(q), c->note = (uint8_t)note, c->vel = (uint8_t)vel;
    q->cap_head = (uint16_t)((q->cap_head + 1) % RFM_MOVE_SEQ_CAPTURE);
    if (q->ncap < RFM_MOVE_SEQ_CAPTURE)
        q->ncap++;
}

void rfmv_seq_pad_on(rfm_move_t *m, int pad, int note, int vel)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[q->sel_track];
    cap_push(q, note, vel);
    if (!q->rec || !q->playing || pad < 0 || pad >= RFM_MOVE_NPADS)
        return;
    if (t->active < 0) {                                /* no clip playing: the edited one starts, on the bar */
        const int32_t T = rfmv_seq_now_tick(m);
        t->active = t->sel, t->t0 = T - T % BAR;
    }
    q->recn[pad].t = rfmv_seq_now_tick(m), q->recn[pad].clip = (uint8_t)SEQ_CLIP(q->sel_track, t->active);
    q->recn[pad].note = (uint8_t)note, q->recn[pad].vel = (uint8_t)vel, q->recn[pad].on = 1;
}

static void rec_finish(rfm_move_t *m, int pad)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_recnote_t *r = &q->recn[pad];
    const int track = r->clip / RFM_MOVE_SEQ_SCENES;
    const rfm_move_track_t *t = &q->tr[track];
    int32_t now = rfmv_seq_now_tick(m), d;
    int L, step, micro;
    uint16_t g;
    r->on = 0;
    if (now < 0)
        now = q->T + 1;
    if (t->active < 0 || SEQ_CLIP(track, t->active) != r->clip || r->t < t->t0)
        return;                                         /* the clip changed under the note */
    L = t->grow == 1 ? RFM_MOVE_SEQ_MAXLEN : q->len[r->clip];
    d = r->t - t->t0;
    to_step((int)(d % ((int32_t)L * TPS)), L, &step, &micro);
    g = q->group;
    q->group = q->rec_group;
    rfmv_seq_note_put(m, r->clip, step, r->note, q->fixed_vel ? 127 : r->vel, clampi(now - r->t, 1, L * TPS), micro);
    q->group = g;
}

void rfmv_seq_pad_off(rfm_move_t *m, int pad, int note)
{
    rfm_move_seq_t *q = &m->seq;
    cap_push(q, note, 0);
    if (pad >= 0 && pad < RFM_MOVE_NPADS && q->recn[pad].on)
        rec_finish(m, pad);
}

/* Capture (Move's): the pad notes of the last bars become a clip on the selected track */

int rfmv_seq_capture(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[q->sel_track];
    const int32_t now = tl_now(q), window = RFM_MOVE_SEQ_CAPTURE_BARS * BAR;
    int32_t first = INT32_MAX, last = INT32_MIN, origin, tl0 = q->tl - q->T, oT = 0;
    int i, j, start = (q->cap_head - q->ncap + RFM_MOVE_SEQ_CAPTURE) % RFM_MOVE_SEQ_CAPTURE, scene = -1, nbars, L;
    int clip, any = 0;
    /* the notes (on .. off) in the window */
    for (i = 0; i < q->ncap; i++) {
        const rfm_move_cap_t *c = &q->cap[(start + i) % RFM_MOVE_SEQ_CAPTURE];
        int32_t off = now;
        if (!c->vel || c->t < now - window)
            continue;
        for (j = i + 1; j < q->ncap; j++) {
            const rfm_move_cap_t *e = &q->cap[(start + j) % RFM_MOVE_SEQ_CAPTURE];
            if (e->note == c->note && !e->vel) {
                off = e->t;
                break;
            }
        }
        first = c->t < first ? c->t : first;
        last = off > last ? off : last;
        any = 1;
    }
    if (!any)
        return -1;
    if (q->playing && first - tl0 >= 0) {              /* played along: the song's bars */
        oT = (first - tl0) - (first - tl0) % BAR;
        origin = oT + tl0;
    } else
        origin = first;
    nbars = clampi((last - origin + BAR - 1) / BAR, 1, RFM_MOVE_SEQ_CAPTURE_BARS);
    if (!rfmv_seq_clip_used(m, SEQ_CLIP(q->sel_track, t->sel)))
        scene = t->sel;
    for (i = 0; i < RFM_MOVE_SEQ_SCENES && scene < 0; i++)
        if (!rfmv_seq_clip_used(m, SEQ_CLIP(q->sel_track, i)))
            scene = i;
    if (scene < 0)
        return -1;
    clip = SEQ_CLIP(q->sel_track, scene);
    L = nbars * RFM_MOVE_SEQ_PAGE;
    rfmv_seq_group(m);
    rfmv_seq_len_set(m, clip, L);
    for (i = 0; i < q->ncap; i++) {
        const rfm_move_cap_t *c = &q->cap[(start + i) % RFM_MOVE_SEQ_CAPTURE];
        int32_t off = now, rel;
        int step, micro;
        if (!c->vel || c->t < origin || (rel = c->t - origin) >= L * TPS)
            continue;
        for (j = i + 1; j < q->ncap; j++) {
            const rfm_move_cap_t *e = &q->cap[(start + j) % RFM_MOVE_SEQ_CAPTURE];
            if (e->note == c->note && !e->vel) {
                off = e->t;
                break;
            }
        }
        to_step((int)rel, L, &step, &micro);
        rfmv_seq_note_put(m, clip, step, c->note, q->fixed_vel ? 127 : c->vel, clampi(off - c->t, 1, L * TPS), micro);
    }
    rfmv_seq_release(m, q->sel_track);
    t->queued = SEQ_NONE, t->grow = 0;
    t->active = (int8_t)scene, t->sel = (int8_t)scene;
    t->t0 = q->playing ? oT : 0;
    q->ncap = 0;
    return scene;
}

void rfmv_seq_tap(rfm_move_t *m, uint32_t now_ms)
{
    rfm_move_seq_t *q = &m->seq;
    if (q->ntap && now_ms - q->tap_ms[q->ntap - 1] > 2000)
        q->ntap = 0;
    if (q->ntap == 4) {
        memmove(q->tap_ms, q->tap_ms + 1, 3 * sizeof q->tap_ms[0]);
        q->ntap = 3;
    }
    q->tap_ms[q->ntap++] = now_ms;
    if (q->ntap >= 2) {
        const uint32_t avg = (q->tap_ms[q->ntap - 1] - q->tap_ms[0]) / (uint32_t)(q->ntap - 1);
        if (q->following)
            rfmv_seq_msg(m, SEQ_MSG_EXTTAP);
        else if (avg)
            rfm_move_seq_set_bpm(m, (int)(600000u / avg));
    }
}
