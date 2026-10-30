/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 standalone (STANDALONE-PLAN.md, milestone S1): the FM-1 as an instrument of its own. Part of the unity
 * build (app.c), before src/rfm_main.c, which calls in here (sa_init, sa_ms, sa_frame, sa_render, the host's hooks).
 *
 * Modes. Standalone at boot. A host's HELLO (any transport) or SESSION start hands the FM-1 to it, panel mode, exactly
 * as RemoteFM1 0.2 is: every note off (here and over MIDI), the keys' velocity sensing off, the screen and LEDs the
 * host's (before its SESSION, the hand-over screen and the waiting LEDs), the panel's events as plain MIDI. The session's
 * end (SESSION 0, its keepalive, USB or Bluetooth gone), or a HELLO with no session and no message for SA_HOST_MS,
 * gives it back: standalone again, as it was left (voice, channel, octave).
 *
 * Standalone. The 27 keys play saadet's FM6 (sa_synth.c) with their velocity (fm1-base's lib/touch: the analog rows
 * sensed, the top row probed) and send their notes on the MIDI channel over USB and Bluetooth at once (src/rfm_main.c's
 * MIDI queue). Notes, pitch bend, the wheel, sustain, aftertouch and program changes coming in on that channel, over USB
 * or Bluetooth, play it too. The controls (STANDALONE-PLAN.md section 3; S1's part): MASTER the volume (fm1-base's
 * gain), PRESETS the voice 1..32, GLO held + PRESETS the cartridge (ROM1A..ROM4B), ALGORITHM the channel 1..16, OCT- /
 * OCT+ the octave -3..+3 (keys F3..G5 at 0), both at once a panic, HOME back to the Notes view at once. Each turn or
 * press shows its screen (Preset, Channel, Octave) for SA_OVERLAY_MS, then the HOME view: Notes (sa_ui.c). LEDs: the
 * keys whose notes sound from the keys or MIDI in lit, GLO lit while held, OCT- / OCT+ lit while the octave is below /
 * above 0, the other buttons at fm1-base's glow. The channel, octave, cartridge and voice are kept in the store
 * (OBJ_SA), a little after the last change, while nothing sounds.
 *
 * The sound. The main loop never calls FM6's note functions: it queues them (sa_ev), and the render (app_render, the
 * audio interrupt) takes the queue at the start of each block. Voices are loaded from the main loop (sa_synth_load_vmem
 * only publishes the voice). The render's output: FM6's sum x SA_PART_GAIN (one DX7 voice at full velocity peaks at
 * 1..2 x full scale, eight at up to ~10x: measured over the 256 ROM voices), a peak limiter (gain down at once to keep
 * each 32-frame block under SA_LIM_T, back up over ~150 ms), a soft knee above 0.8 for what a block's attack lets
 * through, then MASTER's gain. While the FM6 bench runs (sa_bench.c, over SysEx) the bench has the synth: the render
 * plays silence and drops the queue, and the voice is loaded again after it. */
#include "sa_theme.h"
#include "sa_chord.c"
#include "sa_bank.c"
#include "sa_gfx.c"

#ifndef RING_PUBLISH
#define RING_PUBLISH() ((void)0)                 /* (the simulator: one thread; the FM-1's is unity.c's barrier) */
#endif

/* from src/rfm_main.c */
static void rf_ev_put(uint32_t pkt);        /* a MIDI event packet out (USB, and Bluetooth when a computer listens) */
static int rf_panel_ours(void);             /* no session, no push, no hold: the screen is the standalone's */
static int rf_session_on(void);
static void rf_info(char out[4][32], uint8_t *warn);   /* the hand-over screen's lines (the waiting screen's) */
static void rf_sa_leds(uint32_t btns, uint32_t keys);   /* the LEDs, a host's mid level cleared */

#define SA_NOTE0 53u                         /* key 0 at octave 0: F3 */
#define SA_HOST_MS 3000u                     /* a HELLO without a session: panel mode this long after its last message */
#define SA_OVERLAY_MS 1000u
#define SA_LED_REFRESH_MS 500u                /* the LEDs written again this often, changed or not */
#define SA_STORE_MS 2000u                    /* the settings saved this long after the last change, when silent */
#define SA_STORE_FORCE_MS 15000u             /* ... or this long after it anyway */
#define SA_PART_GAIN 0.3f
#define SA_LIM_T 0.89f                       /* the limiter's ceiling (of full scale) */
#define SA_LIM_REL 0.0045f                   /* its release a block (32 frames): ~160 ms */
#define SA_OBJ_MAGIC 0x31534652u             /* "RFS1" */

enum { SA_SCR_HOME, SA_SCR_PRESET, SA_SCR_CHANNEL, SA_SCR_OCTAVE, SA_SCR_HOST };

static struct {
    uint8_t panel;                           /* panel mode: a host has the FM-1 */
    uint8_t host_t;                          /* the transport it came on (RF_T_*) */
    uint32_t host_ms;                        /* its last message */
    uint8_t ch;                              /* the MIDI channel, 1..16 */
    int8_t oct;                              /* -3..+3 */
    uint8_t bank, voice;                     /* 0..SA_NBANKS - 1, 0..SA_BANK_VOICES - 1 */
    uint8_t theme;
    char name[12];                           /* the voice's */
    uint32_t keys;                           /* keys held (as played: lib/touch) */
    uint32_t btns;                           /* buttons held (plat.h's B_*) */
    uint8_t key_note[NKEYS], key_ch[NKEYS];  /* what each held key started (0xFF: nothing, after a panic) */
    uint8_t held[128];                       /* the notes sounding from the keys and MIDI in, counted: the screen */
    uint32_t rheld[4];                       /* MIDI in's notes held (bit n) */
    int8_t oct_before;                       /* the octave before the first of OCT- / OCT+ went down */
    uint8_t ov;                              /* the overlay shown (SA_SCR_*), SA_SCR_HOME none */
    uint32_t ov_t0, ov_until;
    uint8_t store_dirty;
    uint32_t store_ms;
    uint8_t bench_was;                       /* the FM6 bench had the synth */
    uint8_t poke;                            /* the picture changed: a frame soon */
    uint8_t lost;                            /* the panel was someone else's since the last frame */
    uint32_t frame_ms;
    uint32_t led_b, led_k, led_ms;
    uint32_t notes_out, notes_in, ev_drop, panics, saves, loads;   /* (the simulator's checks) */
} sa;

/* ------------------------------------------------------------------ the render's queue --- */
enum { SA_EV_ON, SA_EV_OFF, SA_EV_CC, SA_EV_BEND, SA_EV_AT, SA_EV_ALLOFF, SA_EV_SOUNDOFF };
#define SA_EVQ 256u
static uint32_t sa_evq[SA_EVQ] PLAT_POOL;
static volatile uint32_t sa_ev_w, sa_ev_r;
static void sa_ev(uint32_t type, uint32_t a, uint32_t b)
{
    uint32_t w = sa_ev_w;
    if (w - sa_ev_r >= SA_EVQ) {
        sa.ev_drop++;
        return;
    }
    sa_evq[w % SA_EVQ] = type | (a & 0xFFFFu) << 8 | (b & 0xFFu) << 24;
    RING_PUBLISH();
    sa_ev_w = w + 1u;
}

static int sa_bench_busy(void) { return sab.state == 1u || sab.state == 2u || sab.hstate == 1u || sab.hstate == 2u; }

static int32_t sa_mono[APP_BLOCK] PLAT_POOL;
static float sa_lim_g = 1.0f;
static volatile uint32_t sa_peak_q15;        /* the output's peak since the screen last read it (Q15) */
static uint32_t sa_clips;                    /* samples the soft knee had to bend (the simulator's checks) */

static void sa_ev_apply(void)                /* (the render) */
{
    uint32_t r = sa_ev_r;
    while (r != sa_ev_w) {
        uint32_t e = sa_evq[r % SA_EVQ], a = (e >> 8) & 0xFFFFu, b = e >> 24;
        RING_PUBLISH();
        r++;
        switch (e & 0xFFu) {
        case SA_EV_ON: sa_synth_note_on(a, b); break;
        case SA_EV_OFF: sa_synth_note_off(a); break;
        case SA_EV_CC: sa_synth_cc(a, b); break;
        case SA_EV_BEND: sa_synth_bend((int32_t)a - 8192); break;
        case SA_EV_AT: sa_synth_aftertouch(a); break;
        case SA_EV_ALLOFF: sa_synth_all_notes_off(); break;
        case SA_EV_SOUNDOFF: sa_synth_all_sound_off(); break;
        default: break;
        }
    }
    sa_ev_r = r;
}

static float sa_knee(float y)                /* 1:1 below 0.8, then a tanh-like bend into +-1 */
{
    float a = y < 0.0f ? -y : y, t;
    if (a <= 0.8f)
        return y;
    sa_clips++;
    t = (a - 0.8f) * 5.0f;
    a = t >= 3.0f ? 1.0f : 0.8f + 0.2f * (t * (27.0f + t * t) / (27.0f + 9.0f * t * t));
    return y < 0.0f ? -a : a;
}

/* fm1-base's app_render: n stereo frames (a multiple of SA_CTL), MASTER's gain */
static void sa_render(int32_t *lr, uint32_t n, uint32_t gain_q12)
{
    const float pg = SA_PART_GAIN / 8388608.0f, mg = (float)gain_q12 * (8388607.0f / 4096.0f);
    float peak_out = 0.0f;
    uint32_t k, i;
    if (sa_bench_busy() || n > APP_BLOCK) {
        sa_ev_r = sa_ev_w;                   /* (the bench has the synth) */
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    sa_ev_apply();
    sa_synth_render_mono(sa_mono, n);
    for (k = 0; k < n; k += SA_CTL) {
        float pk = 0.0f, g0 = sa_lim_g, g1, target;
        for (i = 0; i < SA_CTL; i++) {
            float a = (float)sa_mono[k + i];
            a = a < 0.0f ? -a : a;
            pk = a > pk ? a : pk;
        }
        pk *= pg;
        target = pk > SA_LIM_T ? SA_LIM_T / pk : 1.0f;
        g1 = target < g0 ? target : g0 + (target - g0) * SA_LIM_REL;
        for (i = 0; i < SA_CTL; i++) {
            float g = g0 + (g1 - g0) * (float)(i + 1u) * (1.0f / (float)SA_CTL);
            float y = sa_knee((float)sa_mono[k + i] * pg * g), ay = y < 0.0f ? -y : y;
            int32_t v = (int32_t)(y * mg);
            peak_out = ay > peak_out ? ay : peak_out;
            lr[2u * (k + i)] = lr[2u * (k + i) + 1u] = v;
        }
        sa_lim_g = g1;
    }
    {
        uint32_t q = (uint32_t)(peak_out * 32767.0f);
        if (q > sa_peak_q15)
            sa_peak_q15 = q;
    }
}

/* ------------------------------------------------------------------ notes --- */
static void sa_midi3(uint32_t st, uint32_t d1, uint32_t d2)   /* a channel message out (USB + Bluetooth) */
{
    rf_ev_put((st >> 4) | st << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24);
}

static void sa_note(uint32_t note, uint32_t vel)   /* the screen's count and the synth (vel 0: off) */
{
    if (vel) {
        if (sa.held[note] < 255u)
            sa.held[note]++;
        sa_ev(SA_EV_ON, note, vel);
    } else {
        if (sa.held[note])
            sa.held[note]--;
        sa_ev(SA_EV_OFF, note, 0);
    }
    sa.poke = 1;
}

static void sa_key(uint32_t k, uint32_t vel)       /* a key down (vel) or up (0) */
{
    if (vel) {
        int32_t n = (int32_t)SA_NOTE0 + (int32_t)k + 12 * sa.oct;
        n = clamp(n, 0, 127);
        sa.key_note[k] = (uint8_t)n;
        sa.key_ch[k] = (uint8_t)(sa.ch - 1u);
        sa_note((uint32_t)n, vel);
        sa_midi3(0x90u | sa.key_ch[k], (uint32_t)n, vel);
        sa.notes_out++;
    } else if (sa.key_note[k] != 0xFFu) {
        sa_note(sa.key_note[k], 0);
        sa_midi3(0x80u | sa.key_ch[k], sa.key_note[k], 0x40u);
        sa.key_note[k] = 0xFFu;
    }
}

static void sa_remote_off_all(void)        /* MIDI in's notes released (a channel change, a panic) */
{
    uint32_t n;
    for (n = 0; n < 128u; n++)
        if (sa.rheld[n >> 5] >> (n & 31u) & 1u)
            sa_note(n, 0);
    sa.rheld[0] = sa.rheld[1] = sa.rheld[2] = sa.rheld[3] = 0;
}

/* every note off: the keys' (MIDI note offs, as they were sent) and MIDI in's; the synth silent at once (panic, panel
 * mode) or ringing out */
static void sa_all_off(int now)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        if (sa.keys >> k & 1u)
            sa_key(k, 0);
    sa_remote_off_all();
    for (k = 0; k < 128u; k++)
        sa.held[k] = 0;
    sa_ev(now ? SA_EV_SOUNDOFF : SA_EV_ALLOFF, 0, 0);
}

static void sa_panic(void)
{
    sa_all_off(1);
    sa_midi3(0xB0u | (sa.ch - 1u), 64, 0);  /* the pedal up, every note off: the DAW too */
    sa_midi3(0xB0u | (sa.ch - 1u), 123, 0);
    sa.panics++;
}

/* ------------------------------------------------------------------ voices, settings --- */
static void sa_load_voice(void)
{
    const uint8_t *v = sa_bank_vmem(sa.bank, sa.voice);
    sa_synth_load_vmem(v);
    sa_voice_name(v, sa.name);
    sa.poke = 1;
}

static void sa_changed(void)
{
    sa.store_dirty = 1;
    sa.store_ms = plat_ms();
}

typedef struct {
    uint32_t magic;
    uint8_t ver, ch, bank, voice;
    int8_t oct;
    uint8_t theme, rsv[2];
} sa_store_t;

static void sa_load(void)
{
    sa_store_t s;
    if (plat_store_load(OBJ_SA, &s, sizeof s) != (int)sizeof s || s.magic != SA_OBJ_MAGIC || s.ver != 1u)
        return;
    sa.ch = (uint8_t)clamp(s.ch, 1, 16);
    sa.oct = (int8_t)clamp(s.oct, -3, 3);
    sa.bank = (uint8_t)(s.bank < SA_NBANKS ? s.bank : 0u);
    sa.voice = (uint8_t)(s.voice < SA_BANK_VOICES ? s.voice : 0u);
    sa.theme = (uint8_t)(s.theme < SA_NTHEMES ? s.theme : SA_THEME_DEFAULT);
    sa.loads++;
}

static void sa_store_step(void)
{
    sa_store_t s;
    uint32_t since = plat_ms() - sa.store_ms;
    if (!sa.store_dirty || since < SA_STORE_MS || sa_bench_busy())
        return;
    if (sa_synth_voices() && since < SA_STORE_FORCE_MS)
        return;                              /* (a save stops the sound a moment: wait for a quiet one) */
    memset(&s, 0, sizeof s);
    s.magic = SA_OBJ_MAGIC;
    s.ver = 1;
    s.ch = sa.ch;
    s.oct = sa.oct;
    s.bank = sa.bank;
    s.voice = sa.voice;
    s.theme = sa.theme;
    sa.store_dirty = 0;
    if (plat_store_save(OBJ_SA, &s, sizeof s) == 0)
        sa.saves++;
}

static void sa_overlay(uint32_t scr)
{
    uint32_t now = plat_ms();
    if (sa.ov != scr || (int32_t)(now - sa.ov_until) >= 0)
        sa.ov_t0 = now;
    sa.ov = (uint8_t)scr;
    sa.ov_until = now + SA_OVERLAY_MS;
    sa.poke = 1;
}

/* ------------------------------------------------------------------ the modes --- */
static void sa_enter_panel(uint32_t t)
{
    sa.host_t = (uint8_t)t;
    if (sa.panel)
        return;
    sa_all_off(1);
    sa.keys = 0;
    touch_on(0);                             /* (the panel's keys are plat_keys again, as RemoteFM1 0.2 has them) */
    sa.panel = 1;
    sa.ov = SA_SCR_HOME;
    sg_abort();
    sg_invalidate();
    sa.poke = 1;
}

static void sa_leave_panel(void)
{
    if (!sa.panel)
        return;
    sa.panel = 0;
    sa.keys = 0;
    sa.btns = plat_buttons();                /* (what is held now starts nothing) */
    touch_on(1);
    sg_abort();
    sg_invalidate();
    sa.led_b = sa.led_k = 0xFFFFFFFFu;
    sa.poke = 1;
}

static int sa_standalone(void) { return !sa.panel; }

/* every RemoteFM1 message from a host (rfm_main.c rf_on_msg): a HELLO or a SESSION start hands the FM-1 over */
static void sa_host_msg(uint32_t t, uint32_t cmd, const uint8_t *body, uint32_t len)
{
    sa.host_ms = plat_ms();
    if (cmd == RFM_HELLO || (cmd == RFM_SESSION && len >= 1u && body[0]))
        sa_enter_panel(t);
}

static void sa_session(uint32_t on, uint32_t t)   /* (rfm_main.c rfa_session_changed) */
{
    if (on)
        sa_enter_panel(t);
    else
        sa_leave_panel();
}

/* ------------------------------------------------------------------ MIDI in --- */
static void sa_midi_in(uint32_t pkt)
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    if (sa.panel || cin < 8u || cin > 14u || st < 0x80u || st >= 0xF0u || (st & 15u) != sa.ch - 1u)
        return;
    sa.notes_in++;
    switch (st & 0xF0u) {
    case 0x90u:
        if (d2) {
            uint32_t *w = &sa.rheld[d1 >> 5], bit = 1u << (d1 & 31u);
            if (*w & bit)
                sa_note(d1, 0);              /* (the same note again: one count) */
            *w |= bit;
            sa_note(d1, d2);
            break;
        }
        /* fall through: velocity 0 is a note off */
    case 0x80u:
        if (sa.rheld[d1 >> 5] >> (d1 & 31u) & 1u) {
            sa.rheld[d1 >> 5] &= ~(1u << (d1 & 31u));
            sa_note(d1, 0);
        }
        break;
    case 0xB0u:
        if (d1 == 120u || d1 == 123u) {
            sa_remote_off_all();
            sa_ev(d1 == 120u ? SA_EV_SOUNDOFF : SA_EV_ALLOFF, 0, 0);
        } else {
            sa_ev(SA_EV_CC, d1, d2);
        }
        break;
    case 0xE0u: sa_ev(SA_EV_BEND, d1 | d2 << 7, 0); break;
    case 0xD0u: sa_ev(SA_EV_AT, d1, 0); break;
    case 0xC0u:                              /* a program change: the voice of the cartridge */
        if (d1 < SA_BANK_VOICES && d1 != sa.voice) {
            sa.voice = (uint8_t)d1;
            sa_load_voice();
            sa_changed();
            sa_overlay(SA_SCR_PRESET);
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ the panel, once a millisecond --- */
static void sa_leds(void)
{
    uint32_t b = 0, k = 0, i;
    if (sa.btns >> B_GLO & 1u)
        b |= 1u << B_GLO;
    if (sa.oct < 0)
        b |= 1u << B_OCTDN;
    if (sa.oct > 0)
        b |= 1u << B_OCTUP;
    for (i = 0; i < NKEYS; i++) {
        int32_t n = (int32_t)SA_NOTE0 + (int32_t)i + 12 * sa.oct;
        if ((sa.keys >> i & 1u) || (n >= 0 && n < 128 && sa.held[n]))
            k |= 1u << i;
    }
    if (b == sa.led_b && k == sa.led_k && plat_ms() - sa.led_ms < SA_LED_REFRESH_MS)
        return;
    sa.led_b = b;
    sa.led_k = k;
    sa.led_ms = plat_ms();
    rf_sa_leds(b, k);                        /* (rewritten now and then too: nothing else's LEDs stay) */
}

static void sa_buttons(uint32_t btns)
{
    const uint32_t OD = 1u << B_OCTDN, OU = 1u << B_OCTUP;
    uint32_t down = btns & ~sa.btns;
    sa.btns = btns;
    if (down & (OD | OU)) {
        if ((btns & (OD | OU)) == (OD | OU)) {  /* both: a panic, and the octave as before the first went down */
            sa_panic();
            sa.oct = sa.oct_before;
        } else {
            sa.oct_before = sa.oct;
            sa.oct = (int8_t)clamp(sa.oct + ((down & OU) ? 1 : -1), -3, 3);
        }
        if (sa.oct != sa.oct_before)
            sa_changed();
        sa_overlay(SA_SCR_OCTAVE);
    }
    if (down & (1u << B_HOME)) {
        sa.ov = SA_SCR_HOME;                 /* the HOME view at once (Notes: S1's only view) */
        sa.poke = 1;
    }
    if (down & (1u << B_GLO))
        sa.poke = 1;
}

static void sa_encoders(const int32_t *steps)   /* detents by plat.h's role (EN_*) */
{
    int32_t s = steps[EN_PRESET], c = steps[EN_ALGO];
    if (s) {
        if (sa.btns >> B_GLO & 1u) {
            int32_t b = clamp((int32_t)sa.bank + s, 0, (int32_t)SA_NBANKS - 1);
            if (b != sa.bank) {
                sa.bank = (uint8_t)b;
                sa_load_voice();
                sa_changed();
            }
        } else {
            int32_t v = clamp((int32_t)sa.voice + s, 0, (int32_t)SA_BANK_VOICES - 1);
            if (v != sa.voice) {
                sa.voice = (uint8_t)v;
                sa_load_voice();
                sa_changed();
            }
        }
        sa_overlay(SA_SCR_PRESET);
    }
    if (c) {
        int32_t ch = clamp((int32_t)sa.ch + c, 1, 16);
        if (ch != sa.ch) {
            sa_remote_off_all();             /* (their note offs would come on the old channel) */
            sa.ch = (uint8_t)ch;
            sa_changed();
        }
        sa_overlay(SA_SCR_CHANNEL);
    }
}

static void sa_ms(uint32_t btns, const int32_t *steps)
{
    uint32_t now = plat_ms(), keys, ch, k;
    int busy = sa_bench_busy();
    if (sa.bench_was && !busy) {             /* the bench gave the synth back (sa_synth_init'ed): our voice again */
        sa_synth_cores(sab_cores_max());
        sa_load_voice();
    }
    sa.bench_was = (uint8_t)busy;
    if (sa.panel) {
        if (!rf_session_on() && now - sa.host_ms >= SA_HOST_MS)
            sa_leave_panel();                /* a HELLO and nothing since */
        return;
    }
    touch_poll();
    keys = touch_keys() & ((1u << NKEYS) - 1u);
    for (ch = keys ^ sa.keys, k = 0; ch; ch >>= 1, k++)
        if (ch & 1u)
            sa_key(k, (keys >> k & 1u) ? touch_vel(k) : 0u);
    sa.keys = keys;
    sa_buttons(btns & ((1u << NB) - 1u));
    sa_encoders(steps);
    if (sa.ov != SA_SCR_HOME && (int32_t)(now - sa.ov_until) >= 0) {
        sa.ov = SA_SCR_HOME;
        sa.poke = 1;
    }
    sa_leds();
    sa_store_step();
}

/* ------------------------------------------------------------------ start --- */
static void sa_init(void)
{
    uint32_t k;
    memset(&sa, 0, sizeof sa);               /* (the simulator's reboot runs this again) */
    sa.ch = 1;
    sa.theme = SA_THEME_DEFAULT;
    for (k = 0; k < NKEYS; k++)
        sa.key_note[k] = 0xFFu;
    sa.led_b = sa.led_k = 0xFFFFFFFFu;
    sa_load();
    sa_synth_cores(sab_cores_max());
    sa_load_voice();
    sa.bench_was = (uint8_t)sa_bench_busy();
    sa.poke = 1;
}

static void sa_ui_init(void)                 /* (app_ui_init: the platform is up) the keys' sensing on, standalone */
{
    if (!sa.panel)
        touch_on(1);
}

#include "sa_ui.c"
