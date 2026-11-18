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
int rfmv_seq_clip_has(const rfm_move_t *m, int clip)
{
    return (m->seq.made[clip >> 3] >> (clip & 7) & 1) || rfmv_seq_clip_used(m, clip);
}
void rfmv_seq_clip_make(rfm_move_t *m, int clip, int on)
{
    const uint8_t b = (uint8_t)(1u << (clip & 7));
    if (on != !!(m->seq.made[clip >> 3] & b)) {
        m->seq.made[clip >> 3] = (uint8_t)(on ? m->seq.made[clip >> 3] | b : m->seq.made[clip >> 3] & ~b);
        rfmv_seq_dirty(m);
    }
}
int rfmv_seq_loop_lo(const rfm_move_t *m, int clip) { return m->seq.lstart[clip]; }
int rfmv_seq_loop_hi(const rfm_move_t *m, int clip)
{
    const int h = m->seq.lstart[clip] + m->seq.len[clip];
    return h > RFM_MOVE_SEQ_MAXLEN ? RFM_MOVE_SEQ_MAXLEN : h;
}
static const uint8_t RATE_TPS[RFM_MOVE_SEQ_RATES] = { 8, 12, 16, 24, 32, 48, 64, 96, 128, 192 };
static const char *const RATE_NAMES[RFM_MOVE_SEQ_RATES] = { "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T",
                                                            "1/4", "1/2T", "1/2" };
int rfmv_seq_tps(int rate) { return RATE_TPS[rate >= 0 && rate < RFM_MOVE_SEQ_RATES ? rate : RFM_MOVE_SEQ_RATE_16]; }
const char *rfm_move_seq_rate_name(int rate) { return rate >= 0 && rate < RFM_MOVE_SEQ_RATES ? RATE_NAMES[rate] : "?"; }
int rfmv_seq_trig(const rfm_move_t *m, int clip, int step, int id) { return rfmv_seq_lock_get(m, clip, step, id); }

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
    q->made[n->clip >> 3] = (uint8_t)(q->made[n->clip >> 3] | 1u << (n->clip & 7));   /* (a clip with notes is made) */
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
    if (clip < 0 || clip >= RFM_MOVE_SEQ_CLIPS || step < 0 || step >= RFM_MOVE_SEQ_MAXLEN || cc < 0 ||
        cc > RFM_MOVE_TRIG_LAST)
        return -1;
    val = val < 0 ? -1 : clampi(val, 0, cc < 128 ? 127 : 254);   /* (255: SEQ_NOLOCK) */
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
    len = clampi(len, 1, RFM_MOVE_SEQ_MAXLEN - m->seq.lstart[clip]);
    if (old == len)
        return;
    m->seq.len[clip] = (uint16_t)len;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip;
    jot(m, OP_LEN, &n, old, len);
}
static void lstart_set(rfm_move_t *m, int clip, int lo)
{
    rfm_move_note_t n;
    const int old = m->seq.lstart[clip];
    lo = clampi(lo, 0, RFM_MOVE_SEQ_MAXLEN - 1);
    if (old == lo)
        return;
    m->seq.lstart[clip] = (uint8_t)lo;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip;
    jot(m, OP_LSTART, &n, old, lo);
}
void rfmv_seq_loop_set(rfm_move_t *m, int clip, int lo, int len)
{
    lo = clampi(lo, 0, RFM_MOVE_SEQ_MAXLEN - 1);
    len = clampi(len, 1, RFM_MOVE_SEQ_MAXLEN - lo);
    if (lo + m->seq.len[clip] > RFM_MOVE_SEQ_MAXLEN)   /* (the order keeps lstart + len in range) */
        rfmv_seq_len_set(m, clip, len), lstart_set(m, clip, lo);
    else
        lstart_set(m, clip, lo), rfmv_seq_len_set(m, clip, len);
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
    rfmv_seq_loop_set(m, clip, 0, RFM_MOVE_SEQ_PAGE);
}
void rfmv_seq_clip_quantize(rfm_move_t *m, int clip)
{
    int lo, hi = rfmv_seq_clip_range(m, clip, &lo), i;
    for (i = lo; i < hi; i++) {                         /* (a replace keeps every index) */
        const rfm_move_note_t n = m->seq.notes[i];
        if (n.micro)                                    /* (Workflow Settings' amount) */
            rfmv_seq_note_put(m, clip, n.step, n.note, n.vel, n.len, n.micro * (100 - m->seq.qstrength) / 100);
    }
}
int rfmv_seq_clip_double(rfm_move_t *m, int clip)
{
    rfm_move_seq_t *q = &m->seq;
    const int L = q->len[clip], a = q->lstart[clip];
    int lo, hi, i, s;
    if (a + L * 2 > RFM_MOVE_SEQ_MAXLEN)
        return -1;
    for (s = a + L; s < a + 2 * L; s++)                 /* whatever was hidden past the end goes */
        rfmv_seq_step_clear(m, clip, s);
    hi = rfmv_seq_clip_range(m, clip, &lo);
    for (i = lo; i < hi && q->notes[i].step < a + L; i++) {   /* (the copies sort after: indices hold) */
        const rfm_move_note_t n = q->notes[i];
        if (n.step >= a)
            rfmv_seq_note_put(m, clip, n.step + L, n.note, n.vel, n.len, n.micro);
    }
    hi = rfmv_seq_clip_lock_range(m, clip, &lo);
    for (i = lo; i < hi && q->locks[i].step < a + L; i++) {
        const rfm_move_lock_t l = q->locks[i];
        if (l.step >= a)
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
    case OP_LSTART: q->lstart[o->n.clip] = (uint8_t)(forward ? o->nu : o->old); break;
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
    q->clip_lstart = q->lstart[clip];
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
    rfmv_seq_loop_set(m, clip, q->clip_lstart, q->clip_len);
    rfmv_seq_clip_make(m, clip, 1);
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
    rfmv_seq_loop_set(m, to, q->lstart[from], q->len[from]);
    rfmv_seq_clip_make(m, to, 1);
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
    q->bpm10 = 1200, q->clock_mode = RFM_MOVE_CLOCK_AUTO, q->sel_track = 0;
    q->rec_mode = RFM_MOVE_RM_CAPTURE, q->rec_style = RFM_MOVE_RS_OVERDUB;
    q->groove = 0, q->color = 0;
    q->root = q->in_seq ? m->seq.stash_root : m->s.root;   /* (a new project: MIDI mode's key and scale) */
    q->scale = q->in_seq ? m->seq.stash_scale : m->s.scale;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++) {
        memset(&q->tr[t], 0, sizeof q->tr[t]);
        q->tr[t].ch = (uint8_t)t, q->tr[t].active = 0, q->tr[t].sel = 0, q->tr[t].queued = SEQ_NONE;
        q->tr[t].rate = RFM_MOVE_SEQ_RATE_16, q->tr[t].pos = q->tr[t].next = -1, q->tr[t].dir = 1;
        rfmv_seq_arp_default(&q->tr[t].arp);
        q->tr[t].layout = RFM_MOVE_LAYOUT_SCALE;
        rfmv_seq_tdef_default(q->tr[t].tdef);
        {
            rfm_move_settings_t d;
            rfm_move_settings_default(&d);
            memcpy(q->tr[t].cc, d.cc, sizeof d.cc);
        }
    }
    for (c = 0; c < RFM_MOVE_SEQ_CLIPS; c++)
        q->len[c] = RFM_MOVE_SEQ_PAGE, q->lstart[c] = 0;
    memset(q->made, 0, sizeof q->made);
    q->nnotes = q->nlocks = 0;
    q->un = q->utop = 0, q->broken = 0;
    q->clip_kind = 0, q->cut = 0;
    q->page = 0, q->scene_off = 0, q->track_page = 0, q->bank = 0;
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
    q->loop_first = -1, q->loop_tap = -1, q->tstep_tap = -1, q->pad_tap = -1, q->proj_held = -1;
    q->rng = 0x9E3779B9u;
    q->qstrength = 100;
    q->group = 1;
}

/* ================================================================ the engine === */
/* Each track has a playhead (pos, sub ticks into the step, tps ticks a step at its rate) that walks the active
 * clip's loop [lo, hi). Half way through a step the next one is chosen (the step's function, if it fired: FIZZ's
 * RESTART, REVERSE, FORWARD, FLIP, RANDOM, RANDOM FUNCTION, LEAP n, JUMP n) and its condition and chance decided,
 * so that notes played early (negative micro) sound in the second half of the step before theirs. */
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
    q->snd[q->nsnd].off = q->tl + (len < 1 ? 1 : len);
    q->nsnd++;
}

static uint32_t rnd(rfm_move_seq_t *q)
{
    uint32_t x = q->rng ? q->rng : 0x9E3779B9u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return q->rng = x;
}

/* a note's micro (24ths of a step) in ticks of a tps-tick step */
static int micro_ticks(int micro, int tps)
{
    int t = (micro * tps + (micro >= 0 ? 12 : -12)) / 24;
    return clampi(t, -(tps / 2), tps - tps / 2 - 1);
}

/* the groove (Move's): an off step (the 2nd, 4th .. 16th of a beat) late; 100 % a triplet swing (a third of a step
 * late), up to 150 % */
int rfmv_seq_groove_of(const rfm_move_t *m, int track)   /* the track's own (Track) or the project's (Global) */
{
    const rfm_move_track_t *t = &m->seq.tr[track];
    return (t->own & 1) ? t->groove : m->seq.groove;
}
static int groove_at(const rfm_move_t *m, int tr, int step, int mt)
{
    const rfm_move_track_t *t = &m->seq.tr[tr];
    const int g = rfmv_seq_groove_of(m, tr);
    if (!g || !(step & 1))
        return mt;
    mt += g * t->tps / 300;
    return mt > t->tps - 1 ? t->tps - 1 : mt;
}

/* the track's loop now (a recording clip grows: to the end) */
static void loop_of(const rfm_move_seq_t *q, int track, int clip, int *lo, int *hi)
{
    *lo = q->lstart[clip];
    *hi = q->tr[track].grow == 1 ? RFM_MOVE_SEQ_MAXLEN : *lo + q->len[clip];
    if (*hi > RFM_MOVE_SEQ_MAXLEN)
        *hi = RFM_MOVE_SEQ_MAXLEN;
}

/* condition c (RFM_MOVE_COND_*) on loop pass `pass` */
static int cond_ok(const rfm_move_seq_t *q, int track, int c, int pass)
{
    if (c >= 1 && c <= RFM_MOVE_COND_AB_N) {
        int b = 2, a = c;
        while (a > b)
            a -= b, b++;
        return (pass - 1) % b == a - 1;
    }
    switch (c) {
    case RFM_MOVE_COND_FILL: return q->fill;
    case RFM_MOVE_COND_NFILL: return !q->fill;
    case RFM_MOVE_COND_PRE: return q->tr[track].pre;
    case RFM_MOVE_COND_NPRE: return !q->tr[track].pre;
    case RFM_MOVE_COND_NEI: return q->tr[(track + RFM_MOVE_SEQ_TRACKS - 1) % RFM_MOVE_SEQ_TRACKS].pre;
    case RFM_MOVE_COND_NNEI: return !q->tr[(track + RFM_MOVE_SEQ_TRACKS - 1) % RFM_MOVE_SEQ_TRACKS].pre;
    case RFM_MOVE_COND_1ST: return pass == 1;
    case RFM_MOVE_COND_N1ST: return pass != 1;
    default: return 1;
    }
}

/* the step plays (its condition, then its chance); PRE remembers the result (not PRE's own) */
static int decide(rfm_move_t *m, int track, int clip, int step, int pass)
{
    rfm_move_seq_t *q = &m->seq;
    const int c = rfmv_seq_lock_get(m, clip, step, RFM_MOVE_TRIG_COND);
    const int ch = rfmv_seq_lock_get(m, clip, step, RFM_MOVE_TRIG_CHANCE);
    int ok = 1, upd = 0;
    if (c > 0) {
        ok = cond_ok(q, track, c, pass);
        upd = c != RFM_MOVE_COND_PRE && c != RFM_MOVE_COND_NPRE;
    }
    if (ch >= 0) {
        const int r = (int)(rnd(q) % 100u);
        ok = ok && r < ch;
        upd = 1;
    }
    if (upd)
        q->tr[track].pre = (uint8_t)ok;
    return ok;
}

/* the next step after pos (its function, if it fired), with the pass it is in; it is decided now */
static void choose(rfm_move_t *m, int track)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[track];
    const int clip = SEQ_CLIP(track, t->active), pos = t->pos;
    int lo, hi, L, f = 0, next = -1, pass = t->pass;
    loop_of(q, track, clip, &lo, &hi);
    L = hi - lo;
    if (t->fire && t->grow != 1 && (f = rfmv_seq_lock_get(m, clip, pos, RFM_MOVE_TRIG_FUNC)) < 0)
        f = 0;
    if (f == RFM_MOVE_FN_RFUNC)
        f = (int)(rnd(q) % 5u);                         /* none, restart, reverse, forward, flip */
    switch (f) {
    case RFM_MOVE_FN_RESTART: next = lo, pass = 1; break;
    case RFM_MOVE_FN_REVERSE: t->dir = -1; break;
    case RFM_MOVE_FN_FORWARD: t->dir = 1; break;
    case RFM_MOVE_FN_FLIP: t->dir = (int8_t)-t->dir; break;
    case RFM_MOVE_FN_RANDOM: next = lo + (int)(rnd(q) % (uint32_t)L); break;
    case RFM_MOVE_FN_LEAP: {
        const int v = rfmv_seq_lock_get(m, clip, pos, RFM_MOVE_TRIG_LEAP), n = (v < 0 ? 2 : v - 15) * t->dir;
        const int r = pos - lo + n;
        if (pos >= lo && pos < hi) {
            next = lo + ((r % L) + L) % L;
            if (r >= L || r < 0)
                pass++;
        }
        break;
    }
    case RFM_MOVE_FN_JUMP: {
        const int v = rfmv_seq_lock_get(m, clip, pos, RFM_MOVE_TRIG_JUMP);
        next = v >= lo && v < hi ? v : lo;
        break;
    }
    default: break;
    }
    if (next < 0) {                                     /* the natural step */
        next = pos + t->dir;
        if (pos < lo || pos >= hi)                      /* (the loop moved away) */
            next = t->dir > 0 ? lo : hi - 1, pass++;
        else if (next >= hi)
            next = lo, pass++;
        else if (next < lo)
            next = hi - 1, pass++;
    }
    t->next = (int16_t)next, t->npass = (uint16_t)clampi(pass, 1, 60000);
    t->nfire = (uint8_t)decide(m, track, clip, next, t->npass);
}

/* the playhead at song tick T of a clip that started at t0 (launch, Capture, a recording joining in phase) */
static void seek(rfm_move_t *m, int track, int32_t T)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[track];
    const int clip = SEQ_CLIP(track, t->active);
    int lo, hi;
    int32_t d = T - t->t0, st;
    loop_of(q, track, clip, &lo, &hi);
    t->tps = (uint8_t)rfmv_seq_tps(t->rate);
    st = d / t->tps;
    t->pos = (int16_t)(lo + st % (hi - lo)), t->sub = (uint8_t)(d % t->tps);
    t->pass = (uint16_t)clampi(1 + st / (hi - lo), 1, 60000);
    t->dir = 1, t->next = -1;
    t->fire = (uint8_t)decide(m, track, clip, t->pos, t->pass);
}

static void grow_finish(rfm_move_t *m, int track, int bars)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[track];
    const int clip = SEQ_CLIP(track, t->active < 0 ? t->sel : t->active), lo = q->lstart[clip];
    const uint16_t g = q->group;
    rfm_move_note_t n;
    int i, L;
    bars = clampi(bars, 1, (RFM_MOVE_SEQ_MAXLEN - lo) / RFM_MOVE_SEQ_PAGE);
    L = bars * RFM_MOVE_SEQ_PAGE;
    t->grow = 0, t->grow_stop = 0;
    q->group = q->rec_group;                            /* (part of the take) */
    q->len[clip] = RFM_MOVE_SEQ_PAGE;
    memset(&n, 0, sizeof n);
    n.clip = (uint8_t)clip;
    if (L != RFM_MOVE_SEQ_PAGE) {
        q->len[clip] = (uint16_t)L;
        jot(m, OP_LEN, &n, RFM_MOVE_SEQ_PAGE, L);
    }
    /* past the end: a note played just before the loop came round is the first step's; the rest go */
    for (;;) {
        int lo2, hi2 = rfmv_seq_clip_range(m, clip, &lo2);
        for (i = lo2; i < hi2 && q->notes[i].step < lo + L; i++) {}
        if (i >= hi2)
            break;
        n = q->notes[i];
        rfmv_seq_note_del(m, clip, n.step, n.note);
        if (n.step == lo + L && n.micro < 0 && rfmv_seq_find(m, clip, lo, n.note) < 0)
            rfmv_seq_note_put(m, clip, lo, n.note, n.vel, n.len, n.micro);
    }
    q->group = g;
}

static int rec_held(const rfm_move_seq_t *q, int clip, int note)   /* being recorded from a pad now (or the
                                                                   * arp's, this tick) */
{
    int p;
    for (p = 0; p < q->jr_n; p++)
        if (q->jr_clip[p] == clip && q->jr_note[p] == note)
            return 1;
    for (p = 0; p < RFM_MOVE_NPADS; p++)
        if (q->recn[p].on && q->recn[p].clip == clip && q->recn[p].note == note)
            return 1;
    return 0;
}

static void launch_now(rfm_move_t *m, int track, int scene, int32_t T)
{
    rfm_move_track_t *t = &m->seq.tr[track];
    rfmv_seq_release(m, track);
    t->queued = SEQ_NONE;
    if (scene < 0) {
        if (t->grow == 1 && t->pos >= 0)
            grow_finish(m, track, (t->pos - m->seq.lstart[SEQ_CLIP(track, t->active)]) / RFM_MOVE_SEQ_PAGE + 1);
        t->active = -1, t->grow = 0, t->pos = -1;
        return;
    }
    t->active = (int8_t)scene, t->sel = (int8_t)scene, t->t0 = T, t->pos = -1;
    if (t->grow == 2) {                                 /* recording into an empty clip starts here */
        t->grow = 1;
        m->seq.len[SEQ_CLIP(track, scene)] = RFM_MOVE_SEQ_PAGE;
    }
}

/* a track's tick: the playhead on, then what is due now */
static void track_tick(rfm_move_t *m, int tr, int32_t T)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[tr];
    int clip, i, lo, hi;
    if (t->active < 0 || T < t->t0)
        return;
    clip = SEQ_CLIP(tr, t->active);
    if (t->pos < 0)
        seek(m, tr, T);
    else if (++t->sub >= t->tps) {                      /* the next step */
        const int old = t->pos;
        int llo, lhi;
        if (t->next < 0)
            choose(m, tr);
        t->pos = t->next, t->pass = t->npass, t->fire = t->nfire, t->sub = 0, t->next = -1;
        t->tps = (uint8_t)rfmv_seq_tps(t->rate);
        loop_of(q, tr, clip, &llo, &lhi);
        if (t->grow == 1 && (old + 1 - llo) % RFM_MOVE_SEQ_PAGE == 0) {   /* a recording clip: a page more, or done */
            const int k = (old + 1 - llo) / RFM_MOVE_SEQ_PAGE;
            if (t->grow_stop || llo + k * RFM_MOVE_SEQ_PAGE >= RFM_MOVE_SEQ_MAXLEN) {
                grow_finish(m, tr, k);
                t->pos = (int16_t)llo, t->pass++;
                t->fire = (uint8_t)decide(m, tr, clip, t->pos, t->pass);
            } else
                q->len[clip] = (uint16_t)((k + 1) * RFM_MOVE_SEQ_PAGE);
        }
    }
    if (t->sub == 0 && q->rec && q->rec_style == RFM_MOVE_RS_REPLACE && tr == q->sel_track && t->grow != 1 &&
        q->srec != 2) {                                 /* Replace: what was there goes as the take passes over it */
        const uint16_t g = q->group;
        q->group = q->rec_group;
        hi = rfmv_seq_range(m, clip, t->pos, &lo);
        while (hi-- > lo)
            if (q->notes[hi].rsv != q->take && !rec_held(q, clip, q->notes[hi].note))
                rfmv_seq_note_del(m, clip, t->pos, q->notes[hi].note);
        q->group = g;
    }
    if (t->sub == t->tps / 2 && t->next < 0)
        choose(m, tr);
    if (!rfmv_seq_audible(m, tr))                       /* (muted, or another track soloed) */
        return;
    if (t->fire) {
        const int r = rfmv_seq_lock_get(m, clip, t->pos, RFM_MOVE_TRIG_RATCHET), nr = r < 2 ? 1 : r;
        hi = rfmv_seq_range(m, clip, t->pos, &lo);
        for (i = lo; i < hi; i++) {
            const rfm_move_note_t n = q->notes[i];      /* (note_on cannot change the notes) */
            const int mt = groove_at(m, tr, t->pos, micro_ticks(n.micro, t->tps));
            int k, len = n.len * t->tps / RFM_MOVE_SEQ_TPS;
            if (nr > 1 && len > t->tps / nr)
                len = t->tps / nr;
            for (k = 0; k < nr; k++)
                if (mt + k * t->tps / nr == t->sub && !rec_held(q, clip, n.note))
                    note_on(m, tr, n.note, n.vel, len);
        }
        if (t->sub == 0) {
            hi = rfmv_seq_lock_range(m, clip, t->pos, &lo);
            for (i = lo; i < hi && q->locks[i].cc < 128; i++)
                rfmv_emit_midi(m, 0xB0 | t->ch, q->locks[i].cc, q->locks[i].val);
        }
    }
    if (t->next >= 0 && t->nfire && t->sub >= t->tps / 2) {   /* the next step's early notes */
        hi = rfmv_seq_range(m, clip, t->next, &lo);
        for (i = lo; i < hi; i++) {
            const rfm_move_note_t n = q->notes[i];
            const int mt = groove_at(m, tr, t->next, micro_ticks(n.micro, t->tps));
            if (mt < 0 && t->tps + mt == t->sub && !rec_held(q, clip, n.note))
                note_on(m, tr, n.note, n.vel, n.len * t->tps / RFM_MOVE_SEQ_TPS);
        }
    }
}

static void play_tick(rfm_move_t *m, int32_t T)
{
    rfm_move_seq_t *q = &m->seq;
    int tr;
    if (q->metro && T % (BAR / 4) == 0)
        rfmv_emit(m, RFM_MOVE_ACT_CLICK, 0, 0, 0, T % BAR == 0);
    if (T % BAR == 0 && q->arec == 1)                   /* Sample's loop: from this bar line .. */
        rfmv_emit(m, RFM_MOVE_ACT_AUDIO_REC, 0, 0, 0, 1), q->arec = 2, q->arec_T = T;
    else if (T % BAR == 0 && q->arec == 3 && T > q->arec_T) {   /* .. to this one: whole bars */
        rfmv_emit(m, RFM_MOVE_ACT_AUDIO_REC, (int)((T - q->arec_T) / BAR), 0, 0, 0);
        q->arec = 0;
    }
    if (T % BAR == 0)
        for (tr = 0; tr < RFM_MOVE_SEQ_TRACKS; tr++)
            if (q->tr[tr].queued != SEQ_NONE)
                launch_now(m, tr, q->tr[tr].queued, T);
    for (tr = 0; tr < RFM_MOVE_SEQ_TRACKS; tr++)
        track_tick(m, tr, T);
}

/* a held step's notes, once (the track's channel; the clock ends them) */
void rfmv_seq_preview(rfm_move_t *m, int clip, int step)
{
    rfm_move_seq_t *q = &m->seq;
    const int track = clip / RFM_MOVE_SEQ_SCENES, tps = rfmv_seq_tps(q->tr[track].rate);
    int lo, hi = rfmv_seq_range(m, clip, step, &lo), i;
    for (i = lo; i < hi; i++) {
        const rfm_move_note_t n = q->notes[i];
        note_on(m, track, n.note, n.vel, n.len * tps / RFM_MOVE_SEQ_TPS);
    }
}

static void begin(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    int t;
    q->playing = 1, q->start_pending = 0, q->T = -1;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++) {
        q->tr[t].t0 = 0, q->tr[t].pos = -1;
        if (q->tr[t].grow == 2 && q->tr[t].active >= 0 && q->tr[t].queued == SEQ_NONE)
            q->tr[t].queued = q->tr[t].active;          /* (so the launch at T 0 starts the recording) */
    }
}

/* ---- the arp (FIZZ's ARP: its 17 patterns, RATE, RANGE, CHANCE, MODE, GATE, OCT DIST; not its lanes): the
 * selected track's pads go into it instead of out; it runs on the clock, playing or not */
static const uint8_t ARP_RATES[RFM_MOVE_ARP_RATES] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64 };
int rfmv_seq_arp_rate_of(int i) { return ARP_RATES[clampi(i, 0, RFM_MOVE_ARP_RATES - 1)]; }
void rfmv_seq_arp_default(rfm_move_arp_t *a)
{
    memset(a, 0, sizeof *a);                            /* (FIZZ's defaults; OFF: it is every track's) */
    a->pat = RFM_MOVE_AP_UP, a->rate = 6, a->range = 2, a->chance = 100, a->mode = RFM_MOVE_AM_OFF, a->gate = 90;
    a->odist = 12, a->quant = 10, a->reset = RFM_MOVE_AR_NORM, a->mlen = 16, a->tlen = 16;
}
/* a pattern's walk over n notes (by pitch): FIZZ's qd_mfx_arp_order */
static int arp_order(int pat, int n, uint8_t *out)
{
    int i, k = 0;
#define PUT(x) (out[k++] = (uint8_t)(x))
    switch (pat) {
    case RFM_MOVE_AP_UP: case RFM_MOVE_AP_RAND: case RFM_MOVE_AP_ROTH: case RFM_MOVE_AP_ASP: case RFM_MOVE_AP_RONC:
        for (i = 0; i < n; i++) PUT(i);
        break;
    case RFM_MOVE_AP_DN:
        for (i = n - 1; i >= 0; i--) PUT(i);
        break;
    case RFM_MOVE_AP_UPDN:
        for (i = 0; i < n; i++) PUT(i);
        for (i = n - 2; i >= 1; i--) PUT(i);
        break;
    case RFM_MOVE_AP_DNUP:
        for (i = n - 1; i >= 0; i--) PUT(i);
        for (i = 1; i <= n - 2; i++) PUT(i);
        break;
    case RFM_MOVE_AP_UANDD:
        for (i = 0; i < n; i++) PUT(i);
        for (i = n - 1; i >= 0; i--) PUT(i);
        break;
    case RFM_MOVE_AP_DANDU:
        for (i = n - 1; i >= 0; i--) PUT(i);
        for (i = 0; i < n; i++) PUT(i);
        break;
    case RFM_MOVE_AP_CON: case RFM_MOVE_AP_DIV: case RFM_MOVE_AP_CONDIV: {
        uint8_t c[8];
        int lo = 0, hi = n - 1, nc = 0;
        while (lo <= hi) {
            c[nc++] = (uint8_t)lo++;
            if (lo <= hi)
                c[nc++] = (uint8_t)hi--;
        }
        if (pat == RFM_MOVE_AP_DIV)
            for (i = nc - 1; i >= 0; i--) PUT(c[i]);
        else
            for (i = 0; i < nc; i++) PUT(c[i]);
        if (pat == RFM_MOVE_AP_CONDIV)
            for (i = nc - 2; i >= 1; i--) PUT(c[i]);
        break;
    }
    case RFM_MOVE_AP_HI1: case RFM_MOVE_AP_HI2:
        if (n == 1) {
            PUT(0);
            break;
        }
        for (i = n - 2; i >= 0; i--) PUT(n - 1), PUT(i);
        if (pat == RFM_MOVE_AP_HI2)
            for (i = 1; i <= n - 3; i++) PUT(n - 1), PUT(i);
        break;
    case RFM_MOVE_AP_LO1: case RFM_MOVE_AP_LO2:
        if (n == 1) {
            PUT(0);
            break;
        }
        for (i = 1; i < n; i++) PUT(0), PUT(i);
        if (pat == RFM_MOVE_AP_LO2)
            for (i = n - 2; i >= 2; i--) PUT(0), PUT(i);
        break;
    default: break;
    }
#undef PUT
    return k;
}
static void arp_build(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    const int pat = q->tr[q->arp_track].arp.pat, n = q->arp_n;
    int i, j;
    for (i = 0; i < n; i++)
        q->arp_ord[i] = (uint8_t)i;
    for (i = 1; i < n; i++)                             /* by pitch */
        for (j = i; j > 0 && q->arp_pitch[q->arp_ord[j]] < q->arp_pitch[q->arp_ord[j - 1]]; j--) {
            const uint8_t x = q->arp_ord[j];
            q->arp_ord[j] = q->arp_ord[j - 1], q->arp_ord[j - 1] = x;
        }
    q->arp_nseq = (uint8_t)arp_order(pat, n, q->arp_seq);
    if (pat == RFM_MOVE_AP_RONC)
        for (i = n - 1; i > 0; i--) {                   /* (a shuffle: every note once) */
            const int x = (int)(rnd(q) % (uint32_t)(i + 1));
            const uint8_t t = q->arp_seq[i];
            q->arp_seq[i] = q->arp_seq[x], q->arp_seq[x] = t;
        }
}
int rfmv_seq_arp_on(rfm_move_t *m, int note, int vel)   /* 1: the arp took it */
{
    rfm_move_seq_t *q = &m->seq;
    const rfm_move_arp_t *a = &q->tr[q->sel_track].arp;
    int i;
    if (a->mode == RFM_MOVE_AM_OFF)
        return 0;
    if (q->arp_track != q->sel_track)
        q->arp_n = q->arp_down = q->arp_run = 0, q->arp_track = q->sel_track;
    if ((a->mode == RFM_MOVE_AM_HOLD || a->mode == RFM_MOVE_AM_1ST) && !q->arp_down && q->arp_n)
        q->arp_n = 0, q->arp_run = 0;                   /* (a latched set: the next note after every key is up) */
    for (i = 0; i < q->arp_n && q->arp_pitch[i] != note; i++) {}
    if (i == q->arp_n && q->arp_n < 8)
        q->arp_pitch[q->arp_n] = (uint8_t)note, q->arp_vel[q->arp_n++] = (uint8_t)vel;
    q->arp_down++;
    arp_build(m);
    if (a->reset == RFM_MOVE_AR_NOTE)                   /* RESET NOTE: a new note starts it again */
        q->arp_pos = q->arp_oct = q->arp_mpos = q->arp_tpos = 0;
    if (!q->arp_run) {                                  /* starts on QUANT's grid (0: the next tick; the song's bars
                                                         * while playing) */
        const int32_t now = q->tl + 1, ref = q->playing ? q->tl - q->T : 0;
        q->arp_run = 1, q->arp_pos = q->arp_oct = q->arp_steps = q->arp_mpos = q->arp_tpos = 0, q->arp_last = 0xFF;
        q->arp_lastn = 0;
        if (a->quant) {
            const int32_t Q = BAR / rfmv_seq_arp_rate_of(a->quant - 1), d = now - ref;
            q->arp_t0 = ref + (d >= 0 ? (d + Q - 1) / Q * Q : -((-d) / Q) * Q);
        } else
            q->arp_t0 = now;
        q->arp_k = 0, q->arp_next = q->arp_t0;
    }
    return 1;
}
void rfmv_seq_arp_off(rfm_move_t *m, int note)
{
    rfm_move_seq_t *q = &m->seq;
    const int mode = q->tr[q->arp_track].arp.mode;
    int i;
    if (q->arp_down)
        q->arp_down--;
    if (mode == RFM_MOVE_AM_ON || mode == RFM_MOVE_AM_ONCE || mode == RFM_MOVE_AM_OFF) {
        for (i = 0; i < q->arp_n && q->arp_pitch[i] != note; i++) {}
        if (i < q->arp_n) {
            memmove(&q->arp_pitch[i], &q->arp_pitch[i + 1], (size_t)(q->arp_n - i - 1));
            memmove(&q->arp_vel[i], &q->arp_vel[i + 1], (size_t)(q->arp_n - i - 1));
            q->arp_n--;
        }
        arp_build(m);
        if (!q->arp_n)
            q->arp_run = 0;
    }
}
void rfmv_seq_arp_stop(rfm_move_t *m)                  /* every held note let go, a latched arp too */
{
    m->seq.arp_n = m->seq.arp_down = m->seq.arp_run = 0;
}
static void arp_record(rfm_move_t *m, int note, int vel, int len);

/* one step (FIZZ's arp_step): the pattern's next note through the MOD lane (play, rest, tie, repeat, low, high,
 * power chord, chord, the n-th played) and the TRANSPOSE lane, each its own length; RESET MPAT / TPAT start the
 * pattern again as a lane comes round */
static void arp_step(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    const rfm_move_arp_t *a = &q->tr[q->arp_track].arp;
    const int n = q->arp_n, range = clampi(a->range, 1, 8), mlen = clampi(a->mlen, 1, 16), tlen = clampi(a->tlen, 1, 16);
    int idx, mod, tr, ties = 0, i, np = 0, pitch[8 + 2];
    if (!n || !q->arp_nseq) {
        q->arp_run = 0;
        return;
    }
    if ((a->mode == RFM_MOVE_AM_ONCE || a->mode == RFM_MOVE_AM_1ST) && q->arp_steps >= q->arp_nseq * range) {
        q->arp_run = 0;
        return;
    }
    q->arp_steps++;
    if (q->arp_pos >= q->arp_nseq) {                    /* a pass ends: the next octave */
        q->arp_pos = 0;
        if (++q->arp_oct >= range)
            q->arp_oct = 0;
        if (a->pat == RFM_MOVE_AP_RONC)
            arp_build(m);
    }
    mod = a->lane[0][q->arp_mpos % mlen], tr = a->lane[1][q->arp_tpos % tlen];
    for (i = 1; i < mlen && a->lane[0][(q->arp_mpos + i) % mlen] == RFM_MOVE_AL_TIE; i++)
        ties++;
    switch (a->pat) {
    case RFM_MOVE_AP_RAND: idx = (int)(rnd(q) % (uint32_t)n); break;
    case RFM_MOVE_AP_ROTH:
        idx = (int)(rnd(q) % (uint32_t)n);
        if (n > 1 && idx == q->arp_last)
            idx = (idx + 1 + (int)(rnd(q) % (uint32_t)(n - 1))) % n;
        break;
    case RFM_MOVE_AP_ASP: idx = -1 - (q->arp_pos % n); break;   /* (as played) */
    default: idx = q->arp_seq[q->arp_pos % q->arp_nseq]; break;
    }
    q->arp_last = (uint8_t)idx;
    {
        const int lo = q->arp_pitch[q->arp_ord[0]], hi = q->arp_pitch[q->arp_ord[n - 1]], hp = n == 1 ? hi + 12 : hi;
        const int e = idx < 0 ? q->arp_pitch[-1 - idx] : q->arp_pitch[q->arp_ord[idx]], on = q->arp_oct * a->odist;
        switch (mod) {
        case RFM_MOVE_AL_PLAY: pitch[np++] = e + on + tr; break;
        case RFM_MOVE_AL_REST: case RFM_MOVE_AL_TIE: break;
        case RFM_MOVE_AL_REPEAT:
            for (i = 0; i < q->arp_lastn; i++)
                pitch[np++] = q->arp_lastp[i];
            break;
        case RFM_MOVE_AL_LO: pitch[np++] = lo - 12 + tr; break;
        case RFM_MOVE_AL_HI: pitch[np++] = hp + 12 + tr; break;
        case RFM_MOVE_AL_PWR: pitch[np++] = lo + 12 * q->arp_oct + tr, pitch[np++] = hp + 12 * q->arp_oct + tr; break;
        case RFM_MOVE_AL_CHORD:
            for (i = 0; i < n; i++)
                pitch[np++] = q->arp_pitch[q->arp_ord[i]] + 12 * q->arp_oct + tr;
            break;
        default:                                        /* the n-th note played */
            if (mod - RFM_MOVE_AL_N1 < n)
                pitch[np++] = q->arp_pitch[mod - RFM_MOVE_AL_N1] + on + tr;
            break;
        }
    }
    q->arp_pos++;
    q->arp_mpos = (uint16_t)((q->arp_mpos + 1) % mlen);
    q->arp_tpos = (uint16_t)((q->arp_tpos + 1) % tlen);
    if ((a->reset == RFM_MOVE_AR_MPAT && q->arp_mpos == 0) || (a->reset == RFM_MOVE_AR_TPAT && q->arp_tpos == 0))
        q->arp_pos = 0, q->arp_oct = 0;
    if (!np || (a->chance < 100 && (int)(rnd(q) % 100u) >= a->chance))
        return;
    {
        const int steplen = BAR / rfmv_seq_arp_rate_of(a->rate);
        const int len = steplen * clampi(a->gate, 2, 200) / 100 * (1 + ties), vel = q->arp_vel[0];
        if (mod != RFM_MOVE_AL_REPEAT)
            q->arp_lastn = 0;
        for (i = 0; i < np; i++) {
            if (pitch[i] < 0 || pitch[i] > 127)
                continue;
            if (mod != RFM_MOVE_AL_REPEAT && q->arp_lastn < 8)
                q->arp_lastp[q->arp_lastn++] = (uint8_t)pitch[i];
            note_on(m, q->arp_track, pitch[i], vel, len < 1 ? 1 : len);
            arp_record(m, pitch[i], vel, len < 1 ? 1 : len);
        }
    }
}
static void arp_tick(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    if (!q->arp_run || q->tl < q->arp_next)
        return;
    arp_step(m);
    q->arp_k++;
    q->arp_next = q->arp_t0 + (int32_t)((int64_t)q->arp_k * BAR / rfmv_seq_arp_rate_of(q->tr[q->arp_track].arp.rate));
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
    if (q->metro && q->start_pending == 3 && q->count_in > 0 && (q->count_in - 1) % (BAR / 4) == 0)
        rfmv_emit(m, RFM_MOVE_ACT_CLICK, 0, 0, 0, q->count_in - 1 == BAR);   /* (the count-in's beats) */
    arp_tick(m);
    if (q->playing)
        play_tick(m, ++q->T);
    q->jr_n = 0;
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
        if (tr->grow == 1) {                            /* (the pages it reached; a page just begun counts) */
            const int lo = tr->active >= 0 ? q->lstart[SEQ_CLIP(t, tr->active)] : 0;
            grow_finish(m, t, q->playing && tr->pos >= 0 ? (tr->pos - lo) / RFM_MOVE_SEQ_PAGE + 1 : 1);
        }
        tr->grow = 0, tr->grow_stop = 0, tr->pos = -1;
        if (tr->queued != SEQ_NONE) {                   /* a pending launch happens now */
            tr->active = tr->queued == SEQ_STOP ? -1 : tr->queued;
            if (tr->active >= 0)
                tr->sel = tr->active;
            tr->queued = SEQ_NONE;
        }
    }
    if (q->arec >= 2)                                   /* (a loop being recorded ends with the transport) */
        rfmv_emit(m, RFM_MOVE_ACT_AUDIO_REC, (int)((q->T - q->arec_T + BAR - 1) / BAR), 0, 0, 0);
    q->playing = 0, q->start_pending = 0, q->rec = 0, q->srec = 0, q->arec = 0;
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
        t->active = (int8_t)scene, t->queued = SEQ_NONE, t->t0 = 0, t->pos = -1;
        if (scene < 0)
            t->grow = 0;
        return;
    }
    t->queued = (int8_t)(scene < 0 ? SEQ_STOP : scene);
}
/* a clip launched now, not at the bar: in phase with the bar the song is in (stopped: as a launch) */
void rfmv_seq_launch_now(rfm_move_t *m, int track, int scene)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[track];
    if (!q->playing) {
        rfmv_seq_launch(m, track, scene);
        return;
    }
    rfmv_seq_release(m, track);
    t->queued = SEQ_NONE, t->grow = 0;
    t->active = (int8_t)scene, t->sel = (int8_t)scene;
    t->t0 = q->T - q->T % BAR, t->pos = -1;
    seek(m, track, q->T);
}
void rfmv_seq_launch_scene(rfm_move_t *m, int scene)
{
    int t;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
        rfmv_seq_launch(m, t, rfmv_seq_clip_has(m, SEQ_CLIP(t, scene)) ? scene : -1);
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
        m->seq.tr[t].sel = (int8_t)scene;
}
/* solo: while any track is soloed only the soloed ones sound */
void rfmv_seq_solo(rfm_move_t *m, int track, int on)
{
    rfm_move_seq_t *q = &m->seq;
    int t;
    q->tr[track].solo = (uint8_t)!!on;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS; t++)
        if (!rfmv_seq_audible(m, t))
            rfmv_seq_release(m, t);
    rfmv_seq_dirty(m);
}
int rfmv_seq_audible(const rfm_move_t *m, int track)
{
    const rfm_move_seq_t *q = &m->seq;
    int t, any = 0;
    if (q->tr[track].mute)
        return 0;
    for (t = 0; t < RFM_MOVE_SEQ_TRACKS && !any; t++)
        any = q->tr[t].solo;
    return !any || q->tr[track].solo;
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
    const rfm_move_track_t *t = &q->tr[track];
    return q->playing && t->active >= 0 && t->pos >= 0 ? t->pos : -1;
}
int rfmv_seq_clip_tick(const rfm_move_t *m, int track)   /* (24 a step, whatever the rate) */
{
    const rfm_move_seq_t *q = &m->seq;
    const rfm_move_track_t *t = &q->tr[track];
    return q->playing && t->active >= 0 && t->pos >= 0 && t->tps ? t->pos * TPS + t->sub * TPS / t->tps : -1;
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
    rfmv_seq_ui_poll(m);
    rfmv_proj_poll(m);
}

/* ================================================================ recording, capture, tap === */
void rfm_move_seq_set_lead_us(rfm_move_t *m, uint32_t us) { m->seq.lead_us = us > 1000000u ? 1000000u : us; }

/* the host's lead in 16ths of a tick at the tempo in use */
static int32_t lead16(const rfm_move_t *m)
{
    return (int32_t)((uint64_t)m->seq.lead_us * (uint64_t)rfm_move_seq_bpm10(m) * 96u * 16u / 600000000u);
}
/* the fraction of the tick in progress, in 16ths */
static int32_t frac16(const rfm_move_seq_t *q)
{
    const uint64_t den = (uint64_t)q->den_rate * 600u;
    return den ? (int32_t)(q->ph * 16u / den) : 0;
}
/* the timeline tick of what is heard now (the host's lead taken off), rounded */
static int32_t tl_now(const rfm_move_t *m)
{
    return (int32_t)(((int64_t)m->seq.tl * 16 + frac16(&m->seq) - lead16(m) + 8) >> 4);
}
/* .. the song tick (-1 not playing) */
static int32_t T_now(const rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    int32_t T;
    if (!q->playing)
        return -1;
    T = (int32_t)(((int64_t)q->T * 16 + frac16(q) - lead16(m) + 8) >> 4);
    return T < 0 ? 0 : T;
}

static void cap_push(rfm_move_t *m, int note, int vel)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_cap_t *c = &q->cap[q->cap_head];
    c->t = tl_now(m), c->note = (uint8_t)note, c->vel = (uint8_t)vel;
    q->cap_head = (uint16_t)((q->cap_head + 1) % RFM_MOVE_SEQ_CAPTURE);
    if (q->ncap < RFM_MOVE_SEQ_CAPTURE)
        q->ncap++;
}

/* where a note played now goes in the track's playing clip: the step nearest what was heard and its micro */
static void rec_pos_r(rfm_move_t *m, int tr, int arp, int *step, int *micro)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[tr];
    int32_t r;
    if (t->pos < 0)
        seek(m, tr, q->T < t->t0 ? t->t0 : q->T);
    r = arp ? ((int32_t)t->sub + 1) * 16                /* (the arp's note: the tick about to play, exactly) */
            : (int32_t)t->sub * 16 + frac16(q) - lead16(m);
    if (r * 2 >= (int32_t)t->tps * 16) {                /* nearer the next step: early for it */
        if (t->next < 0)
            choose(m, tr);
        *step = t->next;
        r -= (int32_t)t->tps * 16;
    } else
        *step = t->pos;
    *micro = clampi((int)((r * 24 + (r >= 0 ? 1 : -1) * t->tps * 8) / ((int32_t)t->tps * 16)), -TPS / 2, TPS / 2 - 1);
}

static void rec_pos(rfm_move_t *m, int tr, int *step, int *micro) { rec_pos_r(m, tr, 0, step, micro); }

/* the arp's note, recorded with its timing and length while recording (not played again by the clip this tick) */
static void arp_record(rfm_move_t *m, int note, int vel, int len)
{
    rfm_move_seq_t *q = &m->seq;
    const int tr = q->arp_track;
    rfm_move_track_t *t = &q->tr[tr];
    int step, micro, clip, i;
    uint16_t g;
    if (!q->rec || !q->playing || tr != q->sel_track || q->steprec)
        return;
    if (t->active < 0) {
        const int32_t T = q->T + 1;
        t->active = t->sel, t->t0 = T - T % BAR, t->pos = -1, t->queued = SEQ_NONE;
    }
    clip = SEQ_CLIP(tr, t->active);
    rec_pos_r(m, tr, 1, &step, &micro);
    g = q->group, q->group = q->rec_group;
    rfmv_seq_note_put(m, clip, step, note, vel, clampi(len * TPS / (t->tps ? t->tps : TPS), 1, RFM_MOVE_SEQ_MAXLEN * TPS), micro);
    q->group = g;
    if ((i = rfmv_seq_find(m, clip, step, note)) >= 0)
        q->notes[i].rsv = q->take;
    if (q->jr_n < 8)
        q->jr_clip[q->jr_n] = (uint8_t)clip, q->jr_note[q->jr_n++] = (uint8_t)note;
}

static void rec_start_note(rfm_move_t *m, int pad, int clip, int step, int micro, int note, int vel, int32_t T)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_recnote_t *r = &q->recn[pad];
    const uint16_t g = q->group;
    q->group = q->rec_group;
    {
        int lo;
        const int e = rfmv_seq_range(m, clip, step, &lo) == lo;
        rfmv_seq_note_put(m, clip, step, note, vel, TPS, micro);   /* (its length at the release) */
        if (e)
            rfmv_seq_tdef_apply(m, clip, step, 0);     /* (a new step: the track's default trigs) */
    }
    q->group = g;
    {
        const int i = rfmv_seq_find(m, clip, step, note);
        if (i >= 0)
            q->notes[i].rsv = q->take;                  /* (this take's: Replace keeps it) */
    }
    r->t = T, r->clip = (uint8_t)clip, r->step = (uint8_t)step, r->micro = (int8_t)micro;
    r->note = (uint8_t)note, r->vel = (uint8_t)vel, r->on = 1;
}

/* Sample armed: the first note starts the take (the selected clip cleared; stopped: the transport from it) */
static void srec_start(rfm_move_t *m, int pad, int note, int vel)
{
    rfm_move_seq_t *q = &m->seq;
    const int tr = q->sel_track;
    rfm_move_track_t *t = &q->tr[tr];
    const int clip = SEQ_CLIP(tr, t->sel);
    const uint16_t g = (rfmv_seq_group(m), q->group);
    q->rec_group = g;
    rfmv_seq_release(m, tr);
    rfmv_seq_clip_clear(m, clip);
    rfmv_seq_clip_make(m, clip, 1);
    q->rec = 1, q->srec = 2;
    t->queued = SEQ_NONE, t->grow = 1, t->grow_stop = 0, t->active = t->sel, t->pos = -1, t->next = -1;
    if (!q->playing) {                                  /* the note is the clip's first step, the transport starts */
        t->t0 = 0;
        rfmv_seq_start(m);
        rec_start_note(m, pad, clip, 0, 0, note, vel, 0);
        return;
    }
    {
        const int32_t T = T_now(m);
        int step, micro;
        t->t0 = T - T % BAR;                            /* playing: from this bar */
        rec_pos(m, tr, &step, &micro);
        rec_start_note(m, pad, clip, step, micro, note, vel, T);
    }
}

void rfmv_seq_pad_on(rfm_move_t *m, int pad, int note, int vel)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_track_t *t = &q->tr[q->sel_track];
    int step, micro, clip;
    cap_push(m, note, vel);
    if (pad < 0 || pad >= RFM_MOVE_NPADS)
        return;
    if (q->srec == 1) {
        srec_start(m, pad, note, vel);
        return;
    }
    if (!q->rec || !q->playing)
        return;
    if (t->active < 0) {                                /* no clip playing: the edited one starts, on the bar */
        const int32_t T = T_now(m);
        t->active = t->sel, t->t0 = T - T % BAR, t->pos = -1, t->queued = SEQ_NONE;
    }
    clip = SEQ_CLIP(q->sel_track, t->active);
    rec_pos(m, q->sel_track, &step, &micro);
    rec_start_note(m, pad, clip, step, micro, note, vel, T_now(m));
}

/* the note's length, at its release (it went in at the press) */
static void rec_finish(rfm_move_t *m, int pad)
{
    rfm_move_seq_t *q = &m->seq;
    rfm_move_recnote_t *r = &q->recn[pad];
    const int track = r->clip / RFM_MOVE_SEQ_SCENES, tps = rfmv_seq_tps(q->tr[track].rate);
    int32_t now = T_now(m);
    int i;
    uint16_t g;
    r->on = 0;
    if (now < 0)
        now = q->T + 1;
    if ((i = rfmv_seq_find(m, r->clip, r->step, r->note)) < 0)
        return;                                         /* (deleted meanwhile) */
    g = q->group;
    q->group = q->rec_group;
    rfmv_seq_note_put(m, r->clip, r->step, r->note, q->notes[i].vel, clampi((now - r->t) * TPS / tps, 1, RFM_MOVE_SEQ_MAXLEN * TPS),
                      q->notes[i].micro);
    q->group = g;
    if ((i = rfmv_seq_find(m, r->clip, r->step, r->note)) >= 0)
        q->notes[i].rsv = q->take;
}

void rfmv_seq_pad_off(rfm_move_t *m, int pad, int note)
{
    rfm_move_seq_t *q = &m->seq;
    cap_push(m, note, 0);
    if (pad >= 0 && pad < RFM_MOVE_NPADS && q->recn[pad].on)
        rec_finish(m, pad);
}

/* Sample: the audio loop. A press arms it: it records from the next bar line (stopped: the first note played starts
 * the transport, the loop from its first bar); a press while recording ends it at the next bar line (whole bars);
 * a press while armed disarms it */
void rfmv_seq_audio_rec(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    q->arec = (uint8_t)(q->arec == 0 ? 1 : q->arec == 1 ? 0 : 3);
    if (q->arec == 1)
        q->samp_n = 0, q->samp_head = 0;                /* (a new take: its waveform from none) */
}

int rfm_move_sample_levels(rfm_move_t *m, const uint8_t *peaks, int n)
{
    rfm_move_seq_t *q = &m->seq;
    int i;
    if (q->arec < 2 || !peaks || n <= 0)
        return 0;                                       /* (armed, waiting for the bar: flat) */
    for (i = 0; i < n; i++) {
        q->samp[q->samp_head] = (uint8_t)((peaks[i] * 16 + 127) / 255 + 1);
        q->samp_head = (uint8_t)((q->samp_head + 1) % RFM_MOVE_SAMPLE_BARS);
        if (q->samp_n < RFM_MOVE_SAMPLE_BARS)
            q->samp_n++;
    }
    return n;
}

/* Record in Capture mode, stopped (the owner's intentional capture): a press arms it (again: disarmed); the first
 * note played clears the selected clip and records into it from there (the transport starts with that note); a press
 * while recording ends the take at the nearest bar, the clip as long as the bars played */
void rfmv_seq_sample(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    const int tr = q->sel_track;
    rfm_move_track_t *t = &q->tr[tr];
    if (q->srec < 2) {
        q->srec = (uint8_t)!q->srec;
        if (q->srec && !++q->take)
            q->take = 1;
        return;
    }
    q->srec = 0, q->rec = 0;
    if (t->grow == 1 && t->pos >= 0 && t->active >= 0) {
        const int lo = q->lstart[SEQ_CLIP(tr, t->active)], page = RFM_MOVE_SEQ_PAGE * t->tps;
        const int32_t passed = (int32_t)(t->pos - lo) * t->tps + t->sub;
        const int k = (int)(passed / page);
        if (k >= 1 && (passed % page) * 2 < page) {     /* just past a bar line: done at it, the loop goes on */
            grow_finish(m, tr, k);
            t->pos = (int16_t)(t->pos - k * RFM_MOVE_SEQ_PAGE), t->next = -1;
            if (t->sub >= t->tps / 2)
                choose(m, tr);
        } else
            t->grow_stop = 1;                           /* else at the next one */
    }
}

/* ---- Capture (Move's): the last phrase played on the pads becomes a clip on the selected track */
typedef struct { int32_t on, off; uint8_t note, vel; } cnote_t;

/* the tempo a phrase was played at (BPM x 10, 70..140: Move's range, so no octave errors), from its onsets
 * (timeline ticks at bpm10) */
static int detect_bpm10(const cnote_t *c, int n, int bpm10)
{
    double ms[RFM_MOVE_SEQ_CAPTURE], best = 1e18, ioi = 0;
    int i, b, best_b = 0;
    if (n < 3)
        return 0;
    for (i = 0; i < n; i++)
        ms[i] = (double)(c[i].on - c[0].on) * 600000.0 / (bpm10 * 96.0);
    {                                                   /* the typical gap between onsets (the median) */
        double g[64];
        int k = 0, j, x;
        for (i = 1; i < n && k < 64; i++)
            if (ms[i] - ms[i - 1] > 40)
                g[k++] = ms[i] - ms[i - 1];
        for (j = 1; j < k; j++)                         /* (insertion sort: a few) */
            for (x = j; x > 0 && g[x - 1] > g[x]; x--) {
                const double tmp = g[x];
                g[x] = g[x - 1], g[x - 1] = tmp;
            }
        ioi = k ? g[k / 2] : 0;
    }
    for (b = 700; b <= 1400; b += 5) {
        const double s16 = 150000.0 / b;
        double e = 0, nb, d;
        for (i = 0; i < n; i++) {
            const double x = ms[i] / s16;
            d = x - (double)(long)(x + 0.5);
            e += d * d;
        }
        e /= n;
        nb = (ms[n - 1] + ioi) / (16 * s16);            /* the phrase, to the next onset it implies: whole bars */
        d = nb - (double)(long)(nb + 0.5);
        e = e * 4 + (nb < 0.5 ? 1 : d * d) + 0.05 * (b > 1200 ? b - 1200 : 1200 - b) / 1200.0;
        if (e < best)
            best = e, best_b = b;
    }
    {                                                   /* the grid's spacing, least squares */
        const double s16 = 150000.0 / best_b;
        double sk = 0, skk = 0;
        for (i = 0; i < n; i++) {
            const double k = (double)(long)(ms[i] / s16 + 0.5);
            sk += ms[i] * k, skk += k * k;
        }
        if (skk > 0 && sk > 0)
            best_b = (int)(150000.0 / (sk / skk) + 0.5);
    }
    while (best_b > 1400)
        best_b /= 2;                                    /* (the least squares past 140: half) */
    return clampi(best_b, 700, 1400);
}

int rfmv_seq_capture(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    const int tr = q->sel_track;
    rfm_move_track_t *t = &q->tr[tr];
    static cnote_t c[RFM_MOVE_SEQ_CAPTURE];
    const int32_t now = tl_now(m), tl0 = q->tl - q->T;
    int i, j, n = 0, first, scene = -1, clip, L, pages, tps = rfmv_seq_tps(t->rate), any_clip = 0;
    int32_t origin, oT = 0, start_t;
    const int start = (q->cap_head - q->ncap + RFM_MOVE_SEQ_CAPTURE) % RFM_MOVE_SEQ_CAPTURE;
    for (i = 0; i < q->ncap; i++) {                     /* the notes, on .. off */
        const rfm_move_cap_t *e = &q->cap[(start + i) % RFM_MOVE_SEQ_CAPTURE];
        if (!e->vel)
            continue;
        c[n].on = e->t, c[n].off = now, c[n].note = e->note, c[n].vel = e->vel;
        for (j = i + 1; j < q->ncap; j++) {
            const rfm_move_cap_t *f = &q->cap[(start + j) % RFM_MOVE_SEQ_CAPTURE];
            if (f->note == e->note) {
                if (!f->vel)
                    c[n].off = f->t;
                break;
            }
        }
        n++;
    }
    if (!n)
        return -1;
    /* the last phrase: back from the last note, until a silence of more than RFM_MOVE_SEQ_CAPTURE_GAP bars (or
     * the phrase would pass 16 bars) */
    first = n - 1, start_t = c[n - 1].on;
    for (i = n - 2; i >= 0; i--) {
        int32_t end = c[i].off;
        for (j = 0; j < i; j++)                         /* (a note held across: the silence is shorter) */
            if (c[j].off > end)
                end = c[j].off;
        if (start_t - end > RFM_MOVE_SEQ_CAPTURE_GAP * BAR || c[n - 1].on - c[i].on >= RFM_MOVE_SEQ_CAPTURE_BARS * BAR)
            break;
        first = i, start_t = c[i].on;
    }
    for (i = 0; i < RFM_MOVE_SEQ_CLIPS && !any_clip; i++)
        any_clip = rfmv_seq_clip_has(m, i);
    if (!q->playing && !any_clip && !q->following) {    /* an empty project: the phrase's own tempo */
        const int b = detect_bpm10(c + first, n - first, q->bpm10);
        if (b && b != q->bpm10) {
            for (i = first; i < n; i++) {
                c[i].on = c[first].on + (int32_t)((int64_t)(c[i].on - c[first].on) * b / q->bpm10);
                c[i].off = c[first].on + (int32_t)((int64_t)(c[i].off - c[first].on) * b / q->bpm10);
            }
            rfm_move_seq_set_bpm(m, b);
        }
    }
    if (q->playing && c[first].on - tl0 >= 0) {         /* played along: the song's bars */
        oT = (c[first].on - tl0) - (c[first].on - tl0) % BAR;
        origin = oT + tl0;
    } else
        origin = c[first].on;
    pages = clampi((c[n - 1].on - origin) / (RFM_MOVE_SEQ_PAGE * tps) + 1, 1, RFM_MOVE_SEQ_PAGES);
    if (!rfmv_seq_clip_has(m, SEQ_CLIP(tr, t->sel)))
        scene = t->sel;
    for (i = 0; i < RFM_MOVE_SEQ_SCENES && scene < 0; i++)
        if (!rfmv_seq_clip_has(m, SEQ_CLIP(tr, i)))
            scene = i;
    if (scene < 0)
        return -1;
    clip = SEQ_CLIP(tr, scene);
    L = pages * RFM_MOVE_SEQ_PAGE;
    rfmv_seq_group(m);
    rfmv_seq_loop_set(m, clip, 0, L);
    rfmv_seq_clip_make(m, clip, 1);
    for (i = first; i < n; i++) {
        const int32_t rel = c[i].on - origin;
        int s, micro;
        if (rel < 0)
            continue;
        s = (int)((rel + tps / 2) / tps);
        micro = (int)((rel - (int32_t)s * tps) * TPS / tps);
        if (s >= L)
            s -= L;                                     /* (early for the loop's first step) */
        if (s >= L)
            continue;
        rfmv_seq_note_put(m, clip, s, c[i].note, c[i].vel,
                          clampi((c[i].off - c[i].on) * TPS / tps, 1, L * TPS), micro);
    }
    rfmv_seq_release(m, tr);
    t->queued = SEQ_NONE, t->grow = 0;
    t->active = (int8_t)scene, t->sel = (int8_t)scene;
    t->t0 = q->playing ? oT : 0, t->pos = -1;
    if (q->playing && q->T >= t->t0)
        seek(m, tr, q->T);                              /* (in phase now) */
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

