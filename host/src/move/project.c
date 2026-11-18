/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move's projects: the sequencer's whole state as a versioned, little-endian byte blob, and the 32 slots the
 * host stores them in (the core asks with actions; it never does IO). A blob may come from a file someone sent:
 * rfm_move_project_check reads nothing past n and accepts only a blob every field of which is in range.
 *
 *   0  "RMSQ"  4 version (1)  5 flags (0)  6 bpm10 u16  8 clock mode  9 fixed velocity  10 selected track
 *   11 tracks (16)  12 scenes (16)  13..23 zero
 *   24 16 tracks x { channel 0..15, mute 0/1, active clip 0..15 or 255, edited clip 0..15 }
 *   88 256 clip lengths u16 (1..256 steps), clip id = track * 16 + scene
 *  600 notes u16, then n x { clip, step, note 0..127, velocity 1..127, micro -12..11, length u16 1..6144 }
 *      strictly ascending by (clip, step, note)
 *      locks u16, then n x { clip, step, cc 0..127, value 0..127 } strictly ascending by (clip, step, cc)
 *      FNV-1a (32 bit) of everything before it */
#include <string.h>

#include "move_int.h"

#define HDR 24
#define OFF_TRACKS HDR
#define OFF_LENS (OFF_TRACKS + RFM_MOVE_SEQ_TRACKS * 4)
#define OFF_NOTES (OFF_LENS + RFM_MOVE_SEQ_CLIPS * 2)

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v & 0xFF), p[1] = (uint8_t)(v >> 8 & 0xFF); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v & 0xFFFF), w16(p + 2, v >> 16); }
static unsigned r16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) | (uint32_t)r16(p + 2) << 16; }

int rfm_move_project_save(rfm_move_t *m, uint8_t *out, uint32_t cap)
{
    const rfm_move_seq_t *q = &m->seq;
    const uint32_t need = RFM_MOVE_PROJECT_FIXED + (uint32_t)q->nnotes * 7u + (uint32_t)q->nlocks * 4u;
    uint8_t *p = out;
    int i;
    if (!out || cap < need)
        return -1;
    memset(p, 0, HDR);
    memcpy(p, "RMSQ", 4);
    p[4] = RFM_MOVE_PROJECT_VERSION, p[5] = 0;
    w16(p + 6, q->bpm10);
    p[8] = q->clock_mode, p[9] = q->fixed_vel, p[10] = q->sel_track;
    p[11] = RFM_MOVE_SEQ_TRACKS, p[12] = RFM_MOVE_SEQ_SCENES;
    p += HDR;
    for (i = 0; i < RFM_MOVE_SEQ_TRACKS; i++, p += 4)
        p[0] = q->tr[i].ch, p[1] = q->tr[i].mute, p[2] = (uint8_t)(q->tr[i].active < 0 ? 255 : q->tr[i].active),
        p[3] = (uint8_t)q->tr[i].sel;
    for (i = 0; i < RFM_MOVE_SEQ_CLIPS; i++, p += 2)
        w16(p, q->len[i]);
    w16(p, q->nnotes), p += 2;
    for (i = 0; i < q->nnotes; i++, p += 7) {
        const rfm_move_note_t *n = &q->notes[i];
        p[0] = n->clip, p[1] = n->step, p[2] = n->note, p[3] = n->vel, p[4] = (uint8_t)n->micro;
        w16(p + 5, n->len);
    }
    w16(p, q->nlocks), p += 2;
    for (i = 0; i < q->nlocks; i++, p += 4)
        p[0] = q->locks[i].clip, p[1] = q->locks[i].step, p[2] = q->locks[i].cc, p[3] = q->locks[i].val;
    w32(p, fnv(out, (uint32_t)(p - out)));
    p += 4;
    return (int)(p - out);
}

/* every check; q NULL: only check. 0 ok */
static int parse(const uint8_t *b, uint32_t n, rfm_move_seq_t *q)
{
    uint32_t nn, nl, offl, i, prev;
    const uint8_t *p;
    if (!b || n < RFM_MOVE_PROJECT_FIXED || n > RFM_MOVE_PROJECT_MAX)
        return -1;
    if (memcmp(b, "RMSQ", 4))
        return -1;
    if (b[4] != RFM_MOVE_PROJECT_VERSION)
        return -2;
    if (b[11] != RFM_MOVE_SEQ_TRACKS || b[12] != RFM_MOVE_SEQ_SCENES)
        return -3;
    nn = r16(b + OFF_NOTES);
    if (nn > RFM_MOVE_SEQ_NOTES)
        return -4;
    offl = OFF_NOTES + 2 + nn * 7;
    if (offl + 2 + 4 > n)
        return -4;
    nl = r16(b + offl);
    if (nl > RFM_MOVE_SEQ_LOCKS || offl + 2 + nl * 4 + 4 != n)
        return -4;
    if (r32(b + n - 4) != fnv(b, n - 4))
        return -5;
    if (r16(b + 6) < RFM_MOVE_SEQ_BPM10_MIN || r16(b + 6) > RFM_MOVE_SEQ_BPM10_MAX || b[8] > RFM_MOVE_CLOCK_EXTERNAL ||
        b[9] > 1 || b[10] >= RFM_MOVE_SEQ_TRACKS)
        return -6;
    for (i = 0, p = b + OFF_TRACKS; i < RFM_MOVE_SEQ_TRACKS; i++, p += 4)
        if (p[0] > 15 || p[1] > 1 || (p[2] != 255 && p[2] >= RFM_MOVE_SEQ_SCENES) || p[3] >= RFM_MOVE_SEQ_SCENES)
            return -6;
    for (i = 0, p = b + OFF_LENS; i < RFM_MOVE_SEQ_CLIPS; i++, p += 2)
        if (r16(p) < 1 || r16(p) > RFM_MOVE_SEQ_MAXLEN)
            return -6;
    for (i = 0, prev = 0, p = b + OFF_NOTES + 2; i < nn; i++, p += 7) {
        const uint32_t key = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        const int micro = (int8_t)p[4];
        const unsigned len = r16(p + 5);
        if (p[2] > 127 || p[3] < 1 || p[3] > 127 || micro < -RFM_MOVE_SEQ_TPS / 2 || micro >= RFM_MOVE_SEQ_TPS / 2 ||
            len < 1 || len > RFM_MOVE_SEQ_MAXLEN * RFM_MOVE_SEQ_TPS || (i && key <= prev))
            return -7;
        prev = key;
    }
    {   /* at most RFM_MOVE_SEQ_POLY notes a step */
        uint32_t run = 0;
        for (i = 0, p = b + OFF_NOTES + 2; i < nn; i++, p += 7) {
            run = i && p[0] == p[-7] && p[1] == p[-6] ? run + 1 : 1;
            if (run > RFM_MOVE_SEQ_POLY)
                return -7;
        }
    }
    for (i = 0, prev = 0, p = b + offl + 2; i < nl; i++, p += 4) {
        const uint32_t key = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        if (p[2] > 127 || p[3] > 127 || (i && key <= prev))
            return -8;
        prev = key;
    }
    if (!q)
        return 0;
    q->bpm10 = (uint16_t)r16(b + 6), q->clock_mode = b[8], q->fixed_vel = b[9], q->sel_track = b[10];
    for (i = 0, p = b + OFF_TRACKS; i < RFM_MOVE_SEQ_TRACKS; i++, p += 4) {
        q->tr[i].ch = p[0], q->tr[i].mute = p[1], q->tr[i].active = (int8_t)(p[2] == 255 ? -1 : p[2]);
        q->tr[i].sel = (int8_t)p[3];
    }
    for (i = 0, p = b + OFF_LENS; i < RFM_MOVE_SEQ_CLIPS; i++, p += 2)
        q->len[i] = (uint16_t)r16(p);
    q->nnotes = (uint16_t)nn;
    for (i = 0, p = b + OFF_NOTES + 2; i < nn; i++, p += 7) {
        rfm_move_note_t *o = &q->notes[i];
        memset(o, 0, sizeof *o);
        o->clip = p[0], o->step = p[1], o->note = p[2], o->vel = p[3], o->micro = (int8_t)p[4];
        o->len = (uint16_t)r16(p + 5);
    }
    q->nlocks = (uint16_t)nl;
    for (i = 0, p = b + offl + 2; i < nl; i++, p += 4)
        q->locks[i].clip = p[0], q->locks[i].step = p[1], q->locks[i].cc = p[2], q->locks[i].val = p[3];
    return 0;
}

int rfm_move_project_check(const uint8_t *b, uint32_t n) { return parse(b, n, NULL); }

/* ---- a project in brief, for the grid and its popup */
static void info_count(rfm_move_proj_info_t *o, uint8_t seen[RFM_MOVE_SEQ_CLIPS], int clip, int note)
{
    if (!seen[clip])
        seen[clip] = 1, o->clips++;
    if (note && o->notes[clip / RFM_MOVE_SEQ_SCENES] < 0xFFFF)
        o->notes[clip / RFM_MOVE_SEQ_SCENES]++;
}
static void info_live(const rfm_move_seq_t *q, rfm_move_proj_info_t *o)
{
    uint8_t seen[RFM_MOVE_SEQ_CLIPS];
    int i;
    memset(o, 0, sizeof *o);
    memset(seen, 0, sizeof seen);
    o->ok = 1, o->bpm10 = q->bpm10;
    for (i = 0; i < q->nnotes; i++)
        info_count(o, seen, q->notes[i].clip, 1);
    for (i = 0; i < q->nlocks; i++)
        info_count(o, seen, q->locks[i].clip, 0);
}
void rfmv_proj_info(const rfm_move_t *m, int slot, rfm_move_proj_info_t *o)
{
    const rfm_move_seq_t *q = &m->seq;
    if (slot == q->proj_cur && q->proj_pending < 0)
        info_live(q, o);
    else if (slot >= 0 && slot < RFM_MOVE_PROJECTS)
        *o = q->info[slot];
    else
        memset(o, 0, sizeof *o);
}
int rfm_move_project_info(rfm_move_t *m, int slot, const uint8_t *b, uint32_t n)
{
    rfm_move_proj_info_t *o;
    uint8_t seen[RFM_MOVE_SEQ_CLIPS];
    uint32_t nn, nl, i;
    const uint8_t *p;
    if (slot < 0 || slot >= RFM_MOVE_PROJECTS)
        return -1;
    o = &m->seq.info[slot];
    memset(o, 0, sizeof *o);
    if (parse(b, n, NULL) != 0)
        return -1;
    memset(seen, 0, sizeof seen);
    o->ok = 1, o->bpm10 = (uint16_t)r16(b + 6);
    nn = r16(b + OFF_NOTES);
    for (i = 0, p = b + OFF_NOTES + 2; i < nn; i++, p += 7)
        info_count(o, seen, p[0], 1);
    nl = r16(b + OFF_NOTES + 2 + nn * 7);
    for (i = 0, p = b + OFF_NOTES + 4 + nn * 7; i < nl; i++, p += 4)
        info_count(o, seen, p[0], 0);
    return 0;
}

int rfm_move_project_load(rfm_move_t *m, int slot, const uint8_t *b, uint32_t n)
{
    rfm_move_seq_t *q = &m->seq;
    if (slot < 0 || slot >= RFM_MOVE_PROJECTS)
        return -1;
    if (b && n && parse(b, n, NULL) != 0) {
        if (q->proj_pending == slot)
            q->proj_pending = -1;
        rfmv_seq_msg(m, SEQ_MSG_LOADFAIL);
        return -1;
    }
    rfmv_seq_stop(m);
    rfmv_seq_ui_release(m);
    rfmv_seq_project_clear(m);
    if (b && n) {
        parse(b, n, q);
        q->proj_used |= 1u << slot;
    }
    if (q->clock_mode == RFM_MOVE_CLOCK_INTERNAL)
        q->following = 0;
    q->proj_cur = (int8_t)slot, q->proj_pending = -1, q->projects = 0;
    q->dirty = 0;
    info_live(q, &q->info[slot]);
    return 0;
}

void rfm_move_project_dir(rfm_move_t *m, uint32_t used) { m->seq.proj_used = used; }
int rfm_move_project_slot(const rfm_move_t *m) { return m->seq.proj_cur; }

static void emit_slot(rfm_move_t *m, int kind, int slot, const uint8_t *data, uint32_t len)
{
    rfm_move_act_t a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind, a.index = (uint8_t)slot, a.data = data, a.len = len;
    if (m->fn)
        m->fn(m->ctx, &a);
}

void rfmv_proj_save_now(rfm_move_t *m)
{
    rfm_move_seq_t *q = &m->seq;
    const int n = rfm_move_project_save(m, q->blob, sizeof q->blob);
    if (n <= 0)
        return;
    q->dirty = 0;
    q->proj_used |= 1u << q->proj_cur;
    info_live(q, &q->info[q->proj_cur]);
    emit_slot(m, RFM_MOVE_ACT_PROJECT_SAVE, q->proj_cur, q->blob, (uint32_t)n);
}

void rfm_move_project_flush(rfm_move_t *m)
{
    if (m->seq.dirty)
        rfmv_proj_save_now(m);
}

void rfmv_proj_poll(rfm_move_t *m)
{
    const rfm_move_seq_t *q = &m->seq;
    if (q->dirty && q->now_us - q->dirty_us >= (uint64_t)RFM_MOVE_AUTOSAVE_MS * 1000u)
        rfmv_proj_save_now(m);
}

/* a pad in the project grid: the current project is saved, then the slot loads (the host answers) or starts empty */
void rfmv_proj_open(rfm_move_t *m, int slot)
{
    rfm_move_seq_t *q = &m->seq;
    if (slot < 0 || slot >= RFM_MOVE_PROJECTS || q->proj_pending >= 0)
        return;
    if (slot == q->proj_cur) {
        q->projects = 0;
        return;
    }
    if (q->dirty || (q->proj_used >> q->proj_cur & 1))
        rfmv_proj_save_now(m);
    if (q->proj_used >> slot & 1) {
        q->proj_pending = (int8_t)slot;
        emit_slot(m, RFM_MOVE_ACT_PROJECT_LOAD, slot, NULL, 0);   /* (last: the host may answer at once) */
        return;
    }
    rfm_move_project_load(m, slot, NULL, 0);           /* a new, empty project, kept at once */
    rfmv_proj_save_now(m);
}

void rfmv_proj_delete(rfm_move_t *m, int slot)
{
    rfm_move_seq_t *q = &m->seq;
    if (slot < 0 || slot >= RFM_MOVE_PROJECTS || q->proj_pending >= 0)
        return;
    q->proj_used &= ~(1u << slot);
    q->info[slot].ok = 0;
    if (slot == q->proj_cur) {                          /* the open one: it empties (and is not saved again) */
        rfmv_seq_stop(m);
        rfmv_seq_ui_release(m);
        rfmv_seq_project_clear(m);
    }
    emit_slot(m, RFM_MOVE_ACT_PROJECT_DELETE, slot, NULL, 0);
}
