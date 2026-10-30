/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's looper (STANDALONE-PLAN.md S3): 16 loops of events, recorded exactly as played (the keys' notes,
 * as played even with the arp on, and the knobs' CCs, at the millisecond), kept in RAM (the pool), saved to and loaded
 * from a computer over SysEx (SA_LOOP, docs/PROTOCOL.md section 8; tools/sa_loops.py), and (0.3.1) kept in the unit's
 * flash when SAVE is held, loaded at power-on (sa_store.c; sl_store_* below). Part of sa_main.c (its state, sa).
 *
 * What a loop holds (the owner, 2026-10-30): the notes the user plays, not the arp's; on playback they go through the
 * arp when it is on (sl_note), so the arp can be turned on and off while a loop plays (sl_arp_moved). With LATCH a
 * note is held until the latch lets it go, as heard.
 *
 * REC records into the current loop: a new take (the loop empty, or LAYER REPLACE) or an overdub (LAYER DUB: the loop
 * plays on and what is played joins it at each turn). Without a count-in a take is armed (SL_ARM): its time starts
 * with the first note played, and REC again calls it off; an overdub onto a playing loop starts at once (an armed one
 * starts the loop with the first note). A new take's length is free (from its first note to the REC or PLAY that ends
 * it) or 1, 2, 4 or 8 bars at the tempo (it ends by itself, then plays). REC or PLAY during a recording ends it at
 * once (on the press, not the release) and plays the loop. COUNT-IN 1, 2 or 4 bars counts in first and the recording
 * starts at the bar; METRONOME clicks each beat (a bar's first higher) through the count-in and the recording, on the
 * audio out only (sa_main.c sa_click_start); QUANT 1/8 or 1/16 moves each note to the grid at the tempo (its note off
 * with it: the length stays). PLAY plays / stops the current loop; the 16 loops' page picks another. A loop of bars
 * follows the tempo (SELECT, or a computer's MIDI clock: sa_arp.c), at the speed of its own tempo; a free loop keeps
 * its own time. A computer's MIDI start restarts the playing loop at its start; its stop stops it.
 *
 * Memory: one pool of events (SA_LPOOL), the loops one after the other in it (loop k from at[k], n[k] events, sorted
 * by time), and a take (SA_TAKE) where a recording goes, merged into its loop at the end of the recording, at each
 * turn of an overdub, and whenever the take is full. When the pool is full, what is played is not recorded (counted:
 * drops), and the Rec screen says FULL. */

#define SA_LOOPS 16u
#define SA_LPOOL 3584u                       /* events of all loops (8 B each: 28 KB of the pool) */
#define SA_TAKE 512u                         /* a recording's events before they join their loop (4 KB) */
enum { SL_STOP, SL_PLAY, SL_COUNT, SL_REC, SL_DUB, SL_ARM };
enum { SL_LAUNCH_NOW, SL_LAUNCH_BAR, SL_LAUNCH_END, SL_NLAUNCH };   /* a loop picked while one plays comes in at once,
                                                                    * at the playing one's next bar, or at its end */
static const uint8_t SA_REC_BARS[SA_REC_NLENS] = {0, 1, 2, 4, 8};
static const uint8_t SA_COUNT_BARS[SA_REC_NCOUNTS] = {0, 1, 2, 4};

typedef struct {
    uint32_t t;                              /* ms from the loop's start */
    uint8_t type, d1, d2, rsv;               /* SA_LE_*, the note / CC, the velocity / value */
} sa_lev_t;
_Static_assert(sizeof(sa_lev_t) == 8u, "8 bytes an event (the SysEx dump's too)");
static sa_lev_t sa_lpool[SA_LPOOL] PLAT_POOL;
static sa_lev_t sa_take[SA_TAKE] PLAT_POOL;

static struct {
    uint16_t at[SA_LOOPS], n[SA_LOOPS];
    uint32_t len[SA_LOOPS];                  /* ms; 0: empty */
    uint16_t bpm[SA_LOOPS];                  /* the tempo it was recorded at */
    uint8_t bars[SA_LOOPS];                  /* 0: free */
    uint32_t used;                           /* events in the pool */
    uint8_t cur, next, state;                /* the loop; the one queued (0xFF none); SL_* */
    uint32_t switch_at;                      /* where in the current loop the queued one comes in (ms; its length:
                                              * at its end) */
    uint8_t count_play;                      /* the loop plays on through the count (an overdub's) */
    uint32_t pos_q8, last_ms, idx;           /* where the loop is (ms x 256), when it moved, its next event */
    uint32_t take_n, rec_t0, rec_target, count_end;
    uint32_t lheld[4], ton[4];               /* notes the loop sounds; notes on in the take (not yet off) */
    uint32_t larp[4];                        /* ... of the loop's notes, those given to the arp */
    uint8_t lvel[128];                       /* the loop's notes' velocities (the arp turned on or off: sl_arp_moved) */
    uint32_t count_t0;                       /* the count-in's start */
    int32_t beat;                            /* the metronome's last beat (-1: none yet) */
    int16_t qsh[128];                        /* each note's quantize shift (its note off follows) */
    uint32_t drops, wraps, takes, clicks;    /* (the simulator's checks) */
} sl;

static int sl_clock_on(void);                /* (sa_arp.c: a computer's MIDI clock is running) */
static uint32_t sl_bpm_now(void);            /* (sa_arp.c: the tempo, the clock's or SELECT's) */
static uint32_t sl_bar_ms(void) { return 240000u / sl_bpm_now(); }

/* ------------------------------------------------------------------ the pool --- */
static void sl_move(uint32_t to, uint32_t from, uint32_t n)   /* (overlapping: no memmove in the USB-only build) */
{
    uint32_t i;
    if (to < from)
        for (i = 0; i < n; i++)
            sa_lpool[to + i] = sa_lpool[from + i];
    else if (to > from)
        for (i = n; i--;)
            sa_lpool[to + i] = sa_lpool[from + i];
}

static void sl_clear(uint32_t k)             /* loop k empty */
{
    uint32_t j, a = sl.at[k], n = sl.n[k];
    sl_move(a, a + n, sl.used - a - n);
    for (j = k + 1u; j < SA_LOOPS; j++)
        sl.at[j] = (uint16_t)(sl.at[j] - n);
    sl.used -= n;
    sl.n[k] = 0;
    sl.len[k] = 0;
    sl.bars[k] = 0;
}

/* the take into loop k, in time order (both sorted: merged from the end); what does not fit is dropped */
static void sl_merge(uint32_t k)
{
    uint32_t m = sl.take_n, j, a = sl.at[k], n = sl.n[k];
    int32_t i, t, o;
    if (m > SA_LPOOL - sl.used) {
        sl.drops += m - (SA_LPOOL - sl.used);
        m = SA_LPOOL - sl.used;
    }
    sl.take_n = 0;
    if (!m)
        return;
    sl_move(a + n + m, a + n, sl.used - a - n);
    for (i = (int32_t)n - 1, t = (int32_t)m - 1, o = (int32_t)(n + m) - 1; t >= 0; o--)
        if (i >= 0 && sa_lpool[a + (uint32_t)i].t > sa_take[t].t)
            sa_lpool[a + (uint32_t)o] = sa_lpool[a + (uint32_t)i--];
        else
            sa_lpool[a + (uint32_t)o] = sa_take[t--];
    sl.n[k] = (uint16_t)(n + m);
    for (j = k + 1u; j < SA_LOOPS; j++)
        sl.at[j] = (uint16_t)(sl.at[j] + m);
    sl.used += m;
}

static void sl_take_put(uint32_t t, uint32_t type, uint32_t d1, uint32_t d2)   /* in time order */
{
    uint32_t i;
    if (sl.take_n == SA_TAKE) {
        if (sl.state == SL_REC) {
            sl_merge(sl.cur);                /* (a long take: into its loop as it goes) */
        } else {
            sl.drops++;
            return;
        }
        if (sl.take_n) {
            sl.drops++;
            return;
        }
    }
    for (i = sl.take_n; i && sa_take[i - 1u].t > t; i--)
        sa_take[i] = sa_take[i - 1u];
    sa_take[i].t = t;
    sa_take[i].type = (uint8_t)type;
    sa_take[i].d1 = (uint8_t)d1;
    sa_take[i].d2 = (uint8_t)d2;
    sa_take[i].rsv = 0;
    sl.take_n++;
}

/* ------------------------------------------------------------------ the sound --- */
/* a loop's note (vel 0: off): the DX7 and MIDI out, or the arp when it is on (a loop holds the keys as played: the
 * arp steps them on playback as it would the keys) */
static void sl_note(uint32_t note, uint32_t vel)
{
    uint32_t *w = &sl.lheld[note >> 5], *a = &sl.larp[note >> 5], bit = 1u << (note & 31u);
    if (vel) {
        if (*w & bit)
            sl_note(note, 0);                /* (the same note again: off first) */
        *w |= bit;
        sl.lvel[note & 127u] = (uint8_t)vel;
        if (sar_is_on()) {
            *a |= bit;
            sar_key(note, vel);
        } else {
            sa_note(note, vel);
            sa_midi3(0x90u | (sa.ch - 1u), note, vel);
        }
    } else if (*w & bit) {
        *w &= ~bit;
        if (*a & bit) {
            *a &= ~bit;
            sar_key(note, 0);
        } else {
            sa_note(note, 0);
            sa_midi3(0x80u | (sa.ch - 1u), note, 0x40u);
        }
    }
}

static void sl_arp_moved(void)               /* ARP turned on or off while the loop's notes sound: they move over */
{
    uint32_t n;
    for (n = 0; n < 128u; n++) {
        uint32_t bit = 1u << (n & 31u), *a = &sl.larp[n >> 5];
        if (!(sl.lheld[n >> 5] & bit))
            continue;
        if (sar_is_on() && !(*a & bit)) {    /* sounding: into the arp */
            sa_note(n, 0);
            sa_midi3(0x80u | (sa.ch - 1u), n, 0x40u);
            *a |= bit;
            sar_key(n, sl.lvel[n]);
        } else if (!sar_is_on() && (*a & bit)) {   /* the arp's (it let them go): sounding again */
            *a &= ~bit;
            sa_note(n, sl.lvel[n]);
            sa_midi3(0x90u | (sa.ch - 1u), n, sl.lvel[n]);
        }
    }
}

static void sl_all_off(void)
{
    uint32_t n;
    for (n = 0; n < 128u; n++)
        if (sl.lheld[n >> 5] >> (n & 31u) & 1u)
            sl_note(n, 0);
}

static void sl_play_ev(const sa_lev_t *e)
{
    if (e->type == SA_LE_ON)
        sl_note(e->d1, e->d2);
    else if (e->type == SA_LE_OFF)
        sl_note(e->d1, 0);
    else if (e->type == SA_LE_CC) {
        sa_midi3(0xB0u | (sa.ch - 1u), e->d1, e->d2);
        sa_ev(SA_EV_CC, e->d1, e->d2);
    }
}

/* ------------------------------------------------------------------ recording --- */
static uint32_t sl_pos(void) { return sl.pos_q8 >> 8; }

static void sl_begin(void);
static void sa_rec_ev(uint32_t type, uint32_t d1, uint32_t d2)   /* what is played (sa_main.c's keys, sa_cc.c) */
{
    int32_t t, len;
    uint32_t *w = &sl.ton[(d1 & 127u) >> 5], bit = 1u << (d1 & 31u);
    if (sl.state == SL_ARM) {                /* armed: the first note played starts the take, at its time 0 */
        if (type != SA_LE_ON)
            return;
        sl_begin();
    }
    if (sl.state != SL_REC && sl.state != SL_DUB)
        return;
    len = (int32_t)(sl.state == SL_REC ? sl.rec_target : sl.len[sl.cur]);
    t = sl.state == SL_REC ? (int32_t)(plat_ms() - sl.rec_t0) : (int32_t)sl_pos();
    if (type == SA_LE_ON) {
        if (sa.rec_quant) {                  /* to the grid: 1/8 or 1/16 at the tempo */
            int32_t g = (int32_t)(60000u / sl_bpm_now() / (sa.rec_quant == 1u ? 2u : 4u)), q = (t + g / 2) / g * g;
            sl.qsh[d1 & 127u] = (int16_t)(q - t);
            t = q;
        } else {
            sl.qsh[d1 & 127u] = 0;
        }
        *w |= bit;
    } else if (type == SA_LE_OFF) {
        if (!(*w & bit))
            return;                          /* (its note on came before the recording) */
        *w &= ~bit;
        t += sl.qsh[d1 & 127u];
    }
    if (len > 0) {                           /* a known length: inside it (an overdub wraps) */
        if (sl.state == SL_DUB)
            t = (t % len + len) % len;
        else if (t >= len)
            t = len - 1;
    }
    sl_take_put((uint32_t)(t < 0 ? 0 : t), type, d1, d2);
}

static void sl_close_notes(uint32_t t)       /* the take's notes still on: off at t */
{
    uint32_t n;
    for (n = 0; n < 128u; n++)
        if (sl.ton[n >> 5] >> (n & 31u) & 1u)
            sl_take_put(t, SA_LE_OFF, n, 0x40u);
}

static void sl_restart(void)                 /* the current loop from its start */
{
    sl.pos_q8 = 0;
    sl.idx = 0;
    sl.last_ms = plat_ms();
}

static void sl_begin(void)                   /* the recording starts (after the count, if any) */
{
    uint32_t k = sl.cur, bars = SA_REC_BARS[sa.rec_len];
    sl.take_n = 0;
    sl.ton[0] = sl.ton[1] = sl.ton[2] = sl.ton[3] = 0;
    sl.beat = -1;
    if (sl.len[k] && sa.rec_layer == 1u) {   /* LAYER DUB onto a loop: it plays on */
        if (sl.state == SL_STOP || sl.state == SL_ARM || (sl.state == SL_COUNT && !sl.count_play))
            sl_restart();
        sl.state = SL_DUB;
    } else {                                 /* a new take */
        sl_all_off();
        sl_clear(k);
        sl.bpm[k] = (uint16_t)sl_bpm_now();
        sl.bars[k] = (uint8_t)bars;
        sl.rec_target = bars * sl_bar_ms();
        sl.rec_t0 = plat_ms();
        sl.state = SL_REC;
    }
    sl.takes++;
    sa.poke = 1;
}

/* REC tapped, nothing recording. With a count-in: its bars (the metronome's clicks if on), then the recording from
 * the bar. Without: an overdub onto the playing loop at once; else armed: the take (and its time) starts with the
 * first note played (the owner, 2026-10-30) */
static void sl_rec_start(void)
{
    uint32_t bars = SA_COUNT_BARS[sa.rec_count < SA_REC_NCOUNTS ? sa.rec_count : 0u];
    int dub_live = sl.state == SL_PLAY && sl.len[sl.cur] && sa.rec_layer == 1u;
    sl.beat = -1;
    if (bars) {
        sl.count_t0 = plat_ms();
        sl.count_end = sl.count_t0 + bars * sl_bar_ms();
        sl.count_play = (uint8_t)dub_live;
        if (!sl.count_play) {                /* (an overdub's loop plays on through the count: sl_step) */
            sl_all_off();
            sl.next = 0xFFu;
        }
        sl.state = SL_COUNT;
        sa.poke = 1;
        return;
    }
    if (dub_live) {
        sl_begin();
        return;
    }
    sl_all_off();
    sl.next = 0xFFu;
    sl.state = SL_ARM;
    sa.poke = 1;
}

static void sl_rec_stop(int play)            /* the recording ends; play: the loop plays (from its start, for a take) */
{
    uint32_t k = sl.cur;
    if (sl.state == SL_REC) {
        uint32_t len = sl.rec_target ? sl.rec_target : plat_ms() - sl.rec_t0;
        if (len < 50u || (!sl.take_n && !sl.n[k])) {   /* nothing played: the loop stays empty */
            sl.take_n = 0;
            sl.len[k] = 0;
            sl.state = SL_STOP;
            sa.poke = 1;
            return;
        }
        sl.rec_target = len;
        sl_close_notes(len - 1u);
        sl_merge(k);
        sl.len[k] = len;
        sl.state = play ? SL_PLAY : SL_STOP;
        sl_restart();
    } else if (sl.state == SL_DUB) {
        uint32_t p = sl_pos(), i;
        sl_close_notes(p < sl.len[k] ? p : sl.len[k] - 1u);
        sl_merge(k);
        for (i = 0; i < sl.n[k] && sa_lpool[sl.at[k] + i].t <= p; i++)
            ;
        sl.idx = i;                          /* (what was just played is not played again this turn) */
        sl.state = play ? SL_PLAY : SL_STOP;
        if (!play)
            sl_all_off();
    } else if (sl.state == SL_COUNT) {
        sl.state = sl.count_play ? SL_PLAY : SL_STOP;
        if (!sl.count_play)
            sl_all_off();
    } else if (sl.state == SL_ARM) {         /* nothing played: nothing recorded */
        sl.state = SL_STOP;
    }
    sa.poke = 1;
}

/* ------------------------------------------------------------------ playing --- */
static void sl_switch(void)                  /* the queued loop comes in now, from its start */
{
    sl_all_off();
    sl.cur = sl.next;
    sl.next = 0xFFu;
    sl.pos_q8 = 0;
    sl.idx = 0;
    if (!sl.len[sl.cur])
        sl.state = SL_STOP;
    else if (sa.base != SA_SCR_LOOPS)        /* (the 16 loops open: they show it) */
        sa_overlay(SA_SCR_LOOP);
    sa.poke = 1;
}

/* the loop's events up to its position; at its end, the next turn (an overdub's take joins it; a queued loop comes);
 * a loop queued for the next bar comes at it */
static void sl_fire(void)
{
    uint32_t guard = 0;
    for (;;) {
        uint32_t k = sl.cur, p = sl_pos(), a = sl.at[k];
        if (sl.next != 0xFFu && sl.state == SL_PLAY && sl.switch_at < sl.len[k] && p >= sl.switch_at) {
            sl_switch();
            if (sl.state != SL_PLAY || ++guard > 4u)
                return;
            continue;
        }
        while (sl.idx < sl.n[k] && sa_lpool[a + sl.idx].t <= p && sa_lpool[a + sl.idx].t < sl.len[k])
            sl_play_ev(&sa_lpool[a + sl.idx++]);
        if (p < sl.len[k] || ++guard > 4u)
            return;
        while (sl.idx < sl.n[k])             /* (an event at or past the end: played before the turn) */
            sl_play_ev(&sa_lpool[a + sl.idx++]);
        sl.wraps++;
        sl.pos_q8 -= sl.len[k] << 8;
        sl.idx = 0;
        if (sl.state == SL_DUB) {
            uint32_t on[4], n;
            memcpy(on, sl.ton, sizeof on);
            sl_close_notes(sl.len[k] - 1u);
            sl_merge(k);
            for (n = 0; n < 128u; n++)       /* keys still held go on into the next turn */
                if (on[n >> 5] >> (n & 31u) & 1u) {
                    sl.ton[n >> 5] |= 1u << (n & 31u);
                    sl_take_put(0, SA_LE_ON, n, 100u);
                }
        } else if (sl.next != 0xFFu) {       /* the queued loop, from its start */
            sl_switch();
            if (sl.state != SL_PLAY)
                return;
        }
        sa.poke = 1;
    }
}

/* the metronome (METRONOME on): a click a beat, the bar's first one higher, through the count-in and the recording
 * (from the take's start, or the overdubbed loop's) */
static void sl_metro(uint32_t now)
{
    uint32_t beat_ms = sl_bar_ms() / 4u, t;
    int32_t b;
    if (!sa.rec_metro || !beat_ms)
        return;
    if (sl.state == SL_COUNT)
        t = now - sl.count_t0;
    else if (sl.state == SL_REC)
        t = now - sl.rec_t0;
    else if (sl.state == SL_DUB)
        t = sl_pos() * sl_bpm_now() / (sl.bars[sl.cur] && sl.bpm[sl.cur] ? sl.bpm[sl.cur] : sl_bpm_now());
    else
        return;
    b = (int32_t)(t / beat_ms);
    if (b != sl.beat) {
        if (sl.beat >= 0 || t % beat_ms < 30u)   /* (a beat's click, not one late into it) */
            sa_ev(SA_EV_CLICK, (uint32_t)(b % 4 == 0), sa.rec_click);
        sl.beat = b;
        sl.clicks++;
    }
}

static void sl_step(uint32_t now)            /* sa_ms, every millisecond */
{
    uint32_t dt = now - sl.last_ms, k = sl.cur;
    sl.last_ms = now;
    if (dt > 100u)
        dt = 100u;
    if (sl.state == SL_COUNT && (int32_t)(now - sl.count_end) >= 0)
        sl_begin();
    sl_metro(now);
    if (sl.state == SL_ARM)
        return;
    if (sl.state == SL_REC) {
        if (sl.rec_target && now - sl.rec_t0 >= sl.rec_target)
            sl_rec_stop(1);                  /* its bars are done: it plays */
        return;
    }
    if (sl.state == SL_STOP || (sl.state == SL_COUNT && !sl.count_play))
        return;
    if (!sl.len[k]) {
        sl.state = SL_STOP;
        return;
    }
    if (sl.bars[k] && sl.bpm[k])             /* a loop of bars follows the tempo */
        sl.pos_q8 += dt * 256u * sl_bpm_now() / sl.bpm[k];
    else
        sl.pos_q8 += dt << 8;
    sl_fire();
}

/* ------------------------------------------------------------------ the buttons --- */
static int sl_recording(void)
{
    return sl.state == SL_REC || sl.state == SL_DUB || sl.state == SL_COUNT || sl.state == SL_ARM;
}

static void sl_play_tap(void)                /* PLAY: play / stop the current loop */
{
    if (sl_recording()) {
        sl_rec_stop(1);
    } else if (sl.state == SL_PLAY) {
        sl_all_off();
        sl.state = SL_STOP;
        sl.next = 0xFFu;
    } else if (sl.len[sl.cur]) {
        sl_restart();
        sl.state = SL_PLAY;
        if (sa.base != SA_SCR_LOOPS)
            sa_overlay(SA_SCR_LOOP);
        sl_fire();
    }
    sa.poke = 1;
}

/* a loop picked (a white key on the 16 loops' page): while one plays it comes in as LAUNCH says (at once, at the
 * next bar, at the end; the same loop again: the queue called off); else it is the loop, playing if it has notes */
static void sl_pick(uint32_t k)
{
    if (sl_recording() || k >= SA_LOOPS)
        return;
    if (sl.state == SL_PLAY) {
        uint32_t c = sl.cur, p = sl_pos(), bar;
        sl.next = (uint8_t)(k == c ? 0xFFu : k);
        sl.switch_at = sl.len[c];
        if (sl.next != 0xFFu && sa.loop_launch == SL_LAUNCH_NOW) {
            sl_switch();
            if (sl.state == SL_PLAY) {
                sl.last_ms = plat_ms();
                sl_fire();
            }
        } else if (sl.next != 0xFFu && sa.loop_launch == SL_LAUNCH_BAR) {
            bar = 240000u / (sl.bars[c] && sl.bpm[c] ? sl.bpm[c] : sl_bpm_now());   /* (in the loop's own time) */
            sl.switch_at = (p / bar + 1u) * bar;
            if (sl.switch_at > sl.len[c])
                sl.switch_at = sl.len[c];
        }
    } else {
        sl.cur = (uint8_t)k;
        sl.next = 0xFFu;
        if (sl.len[k]) {
            sl_restart();
            sl.state = SL_PLAY;
            sl_fire();
        }
    }
    sa.poke = 1;
}

static void sl_halt(void)                    /* panel mode, a computer's stop: recording ends, nothing plays */
{
    if (sl_recording())
        sl_rec_stop(0);
    sl_all_off();
    sl.state = SL_STOP;
    sl.next = 0xFFu;
}

static void sl_clock_start(void)             /* a computer's MIDI start: the loop from its start */
{
    if (sl.state == SL_PLAY || sl.state == SL_DUB) {
        sl_all_off();
        sl_restart();
        sl_fire();
    } else if (sl.state == SL_STOP && sl.len[sl.cur]) {
        sl_restart();
        sl.state = SL_PLAY;
        sl_fire();
    }
}

static void sl_init(void)
{
    memset(&sl, 0, sizeof sl);
    sl.next = 0xFFu;
}

/* ------------------------------------------------------------------ SysEx: SA_LOOP (docs/PROTOCOL.md section 8) --- */
#define SA_LOOP_REPLY 1024u
static void sl_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static uint32_t sl_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* a command's body -> its reply's (SA_LOOP_R): its length */
static uint32_t sa_loop_cmd(const uint8_t *b, uint32_t len, uint8_t *out)
{
    uint32_t op = len ? b[0] : 0u, k = len >= 2u ? b[1] : 0u, i;
    out[0] = (uint8_t)op;
    if (op == 0u) {                          /* INFO: the state, the current loop, each loop's events, ms, tempo, bars */
        out[1] = sl.state;
        out[2] = sl.cur;
        out[3] = SA_LOOPS;
        for (i = 0; i < SA_LOOPS; i++) {
            uint8_t *p = out + 4u + 9u * i;
            p[0] = (uint8_t)sl.n[i], p[1] = (uint8_t)(sl.n[i] >> 8);
            sl_le32(p + 2, sl.len[i]);
            p[6] = (uint8_t)sl.bpm[i], p[7] = (uint8_t)(sl.bpm[i] >> 8);
            p[8] = sl.bars[i];
        }
        return 4u + 9u * SA_LOOPS;
    }
    out[1] = (uint8_t)k;
    if (k >= SA_LOOPS || len < 2u) {
        out[2] = 2;                          /* (a bad request) */
        return 3;
    }
    if (op == 1u) {                          /* GET slot first16 max8: the events from first */
        uint32_t first = len >= 4u ? (uint32_t)b[2] | (uint32_t)b[3] << 8 : 0u, max = len >= 5u ? b[4] : 126u, c;
        if (max > (SA_LOOP_REPLY - 14u) / 8u)
            max = (SA_LOOP_REPLY - 14u) / 8u;
        c = first < sl.n[k] ? sl.n[k] - first : 0u;
        c = c < max ? c : max;
        out[2] = (uint8_t)first, out[3] = (uint8_t)(first >> 8);
        out[4] = (uint8_t)sl.n[k], out[5] = (uint8_t)(sl.n[k] >> 8);
        sl_le32(out + 6, sl.len[k]);
        out[10] = (uint8_t)sl.bpm[k], out[11] = (uint8_t)(sl.bpm[k] >> 8);
        out[12] = sl.bars[k];
        out[13] = (uint8_t)c;
        memcpy(out + 14, &sa_lpool[sl.at[k] + first], 8u * c);
        return 14u + 8u * c;
    }
    if (sl_recording()) {
        out[2] = 3;                          /* busy: a recording */
        return 3;
    }
    if (k == sl.cur && sl.state != SL_STOP && op >= 2u)
        sl_halt();                           /* (the loop being written stops) */
    if (op == 2u && len >= 9u) {             /* BEGIN slot len32 bpm16 bars8: the loop emptied, its length */
        uint32_t ms = sl_rd32(b + 2), bpm = (uint32_t)b[6] | (uint32_t)b[7] << 8;
        sl_clear(k);
        sl.len[k] = ms > 3600000u ? 0u : ms;
        sl.bpm[k] = (uint16_t)(bpm >= SA_BPM_MIN && bpm <= SA_BPM_MAX ? bpm : 120u);
        sl.bars[k] = b[8] <= 8u ? b[8] : 0u;
        out[2] = 0;
    } else if (op == 3u && len >= 5u) {      /* EVENTS slot first16 n8 events[n]: appended (first: the loop's count) */
        uint32_t first = (uint32_t)b[2] | (uint32_t)b[3] << 8, n = b[4], j;
        if (first != sl.n[k] || len < 5u + 8u * n) {
            out[2] = 2;
        } else if (n > SA_LPOOL - sl.used) {
            out[2] = 1;                      /* full */
        } else {
            uint32_t at = sl.at[k] + sl.n[k];
            sl_move(at + n, at, sl.used - at);
            for (i = 0; i < n; i++) {
                sa_lev_t *e = &sa_lpool[at + i];
                memcpy(e, b + 5u + 8u * i, 8u);
                e->d1 &= 0x7Fu;
                e->d2 &= 0x7Fu;
                e->rsv = 0;
                if (e->type > SA_LE_CC)
                    e->type = SA_LE_CC;
            }
            sl.n[k] = (uint16_t)(sl.n[k] + n);
            for (j = k + 1u; j < SA_LOOPS; j++)
                sl.at[j] = (uint16_t)(sl.at[j] + n);
            sl.used += n;
            out[2] = 0;
        }
    } else if (op == 4u) {                   /* END slot: sorted, inside its length (else the length grows) */
        uint32_t a = sl.at[k], n = sl.n[k], j;
        for (i = 1; i < n; i++) {
            sa_lev_t e = sa_lpool[a + i];
            for (j = i; j && sa_lpool[a + j - 1u].t > e.t; j--)
                sa_lpool[a + j] = sa_lpool[a + j - 1u];
            sa_lpool[a + j] = e;
        }
        if (n && sa_lpool[a + n - 1u].t >= sl.len[k])
            sl.len[k] = sa_lpool[a + n - 1u].t + 1u;
        out[2] = 0;
    } else if (op == 5u) {                   /* CLEAR slot */
        sl_clear(k);
        out[2] = 0;
    } else {
        out[2] = 2;
    }
    sa.poke = 1;
    return 3;
}

/* ------------------------------------------------------------------ the flash (sa_store.c, 0.3.1) --- */
_Static_assert(SA_LOOPS == SF_NL && SA_LPOOL <= SF_LSEC * SF_EVS && sizeof(sa_lev_t) * SF_EVS == 4096u,
               "the loops' places in the data area");

static uint32_t sl_store_meta(uint32_t k, sf_loop_t *e)   /* loop k as it is now (its sectors not filled): its count */
{
    memset(e, 0, sizeof *e);
    e->n = sl.n[k];
    e->len = sl.len[k];
    e->bpm = sl.len[k] ? sl.bpm[k] : 0u;
    e->bars = sl.len[k] ? sl.bars[k] : 0u;
    e->crc = sf_crc((const uint8_t *)&sa_lpool[sl.at[k]], 8u * sl.n[k], 0, 0xFFu);
    return sl.n[k];
}
static const uint8_t *sl_store_bytes(uint32_t k) { return (const uint8_t *)&sa_lpool[sl.at[k]]; }
static uint32_t sl_store_cur(void) { return sl.cur; }

static void sl_store_load(void)              /* (sa_init, after sl_init) the loops the directory lists, CRC good */
{
    uint32_t k, j, used = 0;
    for (k = 0; k < SA_LOOPS; k++) {
        const sf_loop_t *e = &sf.d.l[k];
        sl.at[k] = (uint16_t)used;
        if (!e->len || used + e->n > SA_LPOOL)
            continue;
        for (j = 0; j < e->nsec; j++) {
            uint32_t m = e->n - j * SF_EVS < SF_EVS ? e->n - j * SF_EVS : SF_EVS;
            const uint8_t *d = plat_data(sf_sec(e->sec[j]));
            if (!d)
                break;
            memcpy(&sa_lpool[used + j * SF_EVS], d, 8u * m);
        }
        if (j < e->nsec || sf_crc((const uint8_t *)&sa_lpool[used], 8u * e->n, 0, 0xFFu) != e->crc) {
            sf.lbad++;                       /* (overwritten by another firmware: the loop empty) */
            continue;
        }
        sl.n[k] = e->n;
        sl.len[k] = e->len;
        sl.bpm[k] = e->bpm;
        sl.bars[k] = e->bars;
        used += e->n;
        sf.loads++;
    }
    sl.used = used;
    sl.cur = (uint8_t)(sf.d.lcur < SA_LOOPS ? sf.d.lcur : 0u);
}
