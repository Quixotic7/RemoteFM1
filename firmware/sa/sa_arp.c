/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's arp and its clock (STANDALONE-PLAN.md S4). Part of sa_main.c (its state, sa).
 *
 * The clock: 24 ticks a beat, from the tempo (SELECT, 40..240 BPM) or, while a computer sends MIDI clock (a tick
 * within SA_CLK_LOST_MS), from its ticks; its tempo is then measured (SA_CLK_AVG ticks' average), and loops of bars
 * follow it (sa_loop.c). MIDI start (FA) restarts the arp's pattern and the playing loop at their starts; stop (FC)
 * stops the loop; continue (FB) goes on.
 *
 * The arp (ARP: on / off): the keys held (in the order pressed) are not played but stepped through, one note a step
 * at the rate (1/4 .. 1/32, triplets), over 1..4 octaves, up, down, up-down (the ends once), random, or as played,
 * each note sounding for its gate (10..100 % of the step). Its notes go out like the keys' (the DX7, MIDI out) and
 * are what a loop records. The first key down starts the pattern at once; the last key up ends it. */

#define SA_PPQ 24u
#define SA_CLK_LOST_MS 400u                  /* no MIDI clock for this long: the tempo's own again */
#define SA_CLK_AVG 24u
#define SA_ARP_CHORD_MS 12u                  /* the first step waits this long for the rest of a chord's keys */
static const uint8_t SA_ARP_TICKS[SA_ARP_NRATES] = {24, 12, 8, 6, 4, 3};   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 */

static struct {
    uint8_t on;
    uint8_t n, note[32], vel[32];            /* the keys held, in the order pressed */
    uint8_t playing;                         /* the pattern runs */
    uint8_t pending;                         /* ... from start_ms (its first step) */
    uint32_t start_ms;
    uint32_t idx, step;                      /* the pattern's position; steps since it started */
    uint32_t tick;                           /* ticks into the current step */
    uint8_t sounding;                        /* the note sounding (0xFF none) */
    uint8_t gate_off;                        /* it ended (its gate) */
    uint32_t rnd;
    uint32_t acc_q16, last_ms;               /* the tempo's ticks (fractions of a tick, x 65536) */
    /* MIDI clock */
    uint32_t clk_ms, clk_iv_q8, clk_n;       /* the last tick; the ticks' interval (ms x 256), averaged */
    uint8_t clk_run;                         /* started (FA / FB) and not stopped */
    uint32_t ticks_in, starts, stops;        /* (the simulator's checks) */
    uint32_t notes;
} sar;

static int sar_is_on(void) { return sar.on; }
static int sl_clock_on(void) { return sar.clk_n >= 2u && plat_ms() - sar.clk_ms < SA_CLK_LOST_MS; }

static uint32_t sl_bpm_now(void)             /* the tempo: a computer's clock's, or SELECT's */
{
    if (sl_clock_on() && sar.clk_iv_q8) {
        uint32_t b = (60000u * 256u / SA_PPQ + sar.clk_iv_q8 / 2u) / sar.clk_iv_q8;
        return b < 20u ? 20u : b > 300u ? 300u : b;
    }
    return sa.bpm;
}

static void sar_out(uint32_t note, uint32_t vel)   /* the arp's note on / off: the DX7, MIDI out, the loop */
{
    if (vel) {
        sa_note(note, vel);
        sa_midi3(0x90u | (sa.ch - 1u), note, vel);
        sa_rec_ev(SA_LE_ON, note, vel);
        sar.notes++;
    } else {
        sa_note(note, 0);
        sa_midi3(0x80u | (sa.ch - 1u), note, 0x40u);
        sa_rec_ev(SA_LE_OFF, note, 0x40u);
    }
}

static void sar_release(void)
{
    if (sar.sounding != 0xFFu) {
        sar_out(sar.sounding, 0);
        sar.sounding = 0xFFu;
    }
}

static uint32_t sar_pick(uint32_t *vel)      /* the pattern's note at idx */
{
    uint32_t n = sar.n, o = sa.arp_oct, len = n * o, i, k, sorted[32], tmp;
    for (i = 0; i < n; i++)
        sorted[i] = sar.note[i] | (uint32_t)sar.vel[i] << 8;
    if (sa.arp_mode != 4u)                   /* (all but "as played": low to high) */
        for (i = 1; i < n; i++)
            for (k = i; k && (sorted[k - 1u] & 0xFFu) > (sorted[k] & 0xFFu); k--)
                tmp = sorted[k], sorted[k] = sorted[k - 1u], sorted[k - 1u] = tmp;
    switch (sa.arp_mode) {
    case 1: k = len - 1u - sar.idx % len; break;                     /* down */
    case 2: {                                                        /* up-down, the ends once */
        uint32_t per = len > 1u ? 2u * len - 2u : 1u, p = sar.idx % per;
        k = p < len ? p : per - p;
        break;
    }
    case 3:                                                          /* random */
        sar.rnd = sar.rnd * 1664525u + 1013904223u;
        k = (sar.rnd >> 16) % len;
        break;
    default: k = sar.idx % len; break;                               /* up, as played */
    }
    *vel = sorted[k % n] >> 8;
    {
        int32_t note = (int32_t)(sorted[k % n] & 0xFFu) + 12 * (int32_t)(k / n);
        return (uint32_t)(note > 127 ? 127 : note);
    }
}

static void sar_step(void)                   /* a new step: the last note off, the next on */
{
    uint32_t vel, note;
    sar_release();
    if (!sar.n)
        return;
    note = sar_pick(&vel);
    sar_out(note, vel);
    sar.sounding = (uint8_t)note;
    sar.gate_off = 0;
    sar.idx++;
    sar.step++;
    sar.tick = 0;
}

static void sar_tick(void)                   /* a clock tick (24 a beat) */
{
    uint32_t rate = SA_ARP_TICKS[sa.arp_rate];
    if (!sar.on || !sar.playing || sar.pending)
        return;
    sar.tick++;
    if (!sar.gate_off && sar.tick * 10u >= rate * sa.arp_gate && sa.arp_gate < 10u) {
        sar_release();
        sar.gate_off = 1;
    }
    if (sar.tick >= rate)
        sar_step();
}

/* a key down (vel) or up (0) while the arp is on */
static void sar_key(uint32_t note, uint32_t vel)
{
    uint32_t i;
    if (vel) {
        if (sar.n < 32u) {
            sar.note[sar.n] = (uint8_t)note;
            sar.vel[sar.n] = (uint8_t)vel;
            sar.n++;
        }
        if (!sar.playing) {                  /* the first key: the pattern starts (when a chord's keys are all down) */
            sar.playing = 1;
            sar.idx = 0;
            sar.start_ms = plat_ms() + SA_ARP_CHORD_MS;
            sar.pending = 1;
        }
        return;
    }
    for (i = 0; i < sar.n; i++)
        if (sar.note[i] == note) {
            for (; i + 1u < sar.n; i++)
                sar.note[i] = sar.note[i + 1u], sar.vel[i] = sar.vel[i + 1u];
            sar.n--;
            break;
        }
    if (!sar.n) {                            /* the last key up: done */
        sar.playing = 0;
        sar.pending = 0;
        sar_release();
    }
}

static void sar_set(int on)                  /* ARP */
{
    sar.on = (uint8_t)(on != 0);
    sar.n = 0;
    sar.playing = 0;
    sar.pending = 0;
    sar_release();
}

/* every millisecond (sa_ms): the tempo's ticks while no computer sends clock */
static void sar_ms(uint32_t now)
{
    uint32_t dt = now - sar.last_ms;
    sar.last_ms = now;
    if (dt > 100u)
        dt = 100u;
    if (sar.pending && (int32_t)(now - sar.start_ms) >= 0) {
        sar.pending = 0;
        sar_step();
    }
    if (sl_clock_on())
        return;
    sar.acc_q16 += dt * (uint32_t)sa.bpm * (SA_PPQ * 65536u / 60u) / 1000u;
    while (sar.acc_q16 >= 65536u) {
        sar.acc_q16 -= 65536u;
        sar_tick();
    }
}

/* MIDI real time from a computer (sa_midi_in) */
static void sar_realtime(uint32_t st)
{
    uint32_t now = plat_ms();
    if (st == 0xF8u) {
        uint32_t iv = now - sar.clk_ms;
        if (sar.clk_n && iv < SA_CLK_LOST_MS) {
            uint32_t q = iv << 8;
            sar.clk_iv_q8 = sar.clk_n < 2u ? q : sar.clk_iv_q8 + ((int32_t)(q - sar.clk_iv_q8)) / (int32_t)SA_CLK_AVG;
        }
        sar.clk_n = sar.clk_n < 0xFFFFu ? sar.clk_n + 1u : sar.clk_n;
        if (iv >= SA_CLK_LOST_MS)
            sar.clk_n = 1;
        sar.clk_ms = now;
        sar.ticks_in++;
        sar_tick();
    } else if (st == 0xFAu) {                /* start: the pattern and the loop from their starts */
        sar.clk_run = 1;
        sar.starts++;
        if (sar.playing) {
            sar.idx = 0;
            sar.tick = SA_ARP_TICKS[sa.arp_rate] - 1u;   /* (the first tick steps) */
        }
        sl_clock_start();
    } else if (st == 0xFBu) {
        sar.clk_run = 1;
    } else if (st == 0xFCu) {
        sar.clk_run = 0;
        sar.stops++;
        if (sl.state == SL_PLAY) {
            sl_all_off();
            sl.state = SL_STOP;
        }
    }
}

static void sar_init(void)
{
    memset(&sar, 0, sizeof sar);
    sar.sounding = 0xFFu;
    sar.rnd = 0x2545F491u;
}
