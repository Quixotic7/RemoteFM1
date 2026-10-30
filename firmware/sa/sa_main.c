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
 * LATCH (LFO held; tapped it is CC bank D): the keys' notes stay after they are let go; a new chord (a key down while
 * none is held) replaces them; with the arp on, the arp keeps stepping the latched keys; LFO blinks while it is on;
 * off lets the notes go. Not kept across a power cycle.
 *
 * After S1 (STANDALONE-PLAN.md S2..S5): FX SEL ENV LFO EDIT the CC bank and KN1..KN4 its CCs (sa_cc.c); REC, PLAY
 * and their holds the looper (sa_loop.c); ARP and its hold the arp, SELECT the tempo, a computer's MIDI clock both
 * (sa_arp.c); HOME the next view (Notes, Scope, Keys); SAVE the theme menu, held: save; a DX7 bank dump the user
 * banks (sa_ubank.c); their screens in sa_screens.c. Menus (Loops, Rec options, Arp settings, Theme) stay up, locked
 * on, until HOME, their own button or another screen, and take the knobs, SELECT and PRESETS.
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
#define SA_BPM_MIN 40
#define SA_BPM_MAX 240
#define SA_ARP_NMODES 5u                     /* sa_arp.c: up, down, up-down, random, as played */
#define SA_ARP_NRATES 6u                     /* 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 */
#define SA_ARP_RATE_DEFAULT 3u               /* 1/16 */
#define SA_REC_NLENS 5u                      /* sa_loop.c: free, 1, 2, 4, 8 bars */

enum { SA_SCR_HOME, SA_SCR_PRESET, SA_SCR_CHANNEL, SA_SCR_OCTAVE, SA_SCR_HOST, SA_SCR_CCBANK, SA_SCR_CCTURN,
       SA_SCR_CCASSIGN, SA_SCR_SAVED, SA_SCR_REC, SA_SCR_LOOP, SA_SCR_LOOPS, SA_SCR_RECOPT, SA_SCR_ARP, SA_SCR_ARPSET,
       SA_SCR_TEMPO, SA_SCR_THEME, SA_SCR_REMOTE, SA_SCR_LATCH, SA_SCR_SETTINGS, SA_NSCR };
#define SA_HOLD_MS 500u                      /* SAVE, ARP, PLAY, REC, LFO, HOME: held this long is a hold, else a tap */
/* the settings (HOME held): MIDI REMOTE's display, knobs, channels, keys */
enum { SA_RMD_VOID, SA_RMD_SOLID, SA_RMD_ALL, SA_RMD_KNOBS, SA_RMD_N };   /* the screen in MIDI REMOTE */
enum { SA_RMR_ABS, SA_RMR_2C, SA_RMR_OFS, SA_RMR_N };   /* its encoders: absolute; relative 1..63 / 127..65; 65 / 63 */
enum { SA_RMK_FIXED, SA_RMK_PLAY, SA_RMK_N };           /* its keys: fixed (as 0.3); MIDI KEYBOARD */
#define SA_NSET 6u                           /* the settings' rows */
#define SA_MENU_MS 6000u                     /* (a menu's ov_until: unused since menus stay; kept for the touch) */
enum { SA_LE_ON, SA_LE_OFF, SA_LE_CC };      /* a loop's events (sa_loop.c) */
static void sa_rec_ev(uint32_t type, uint32_t d1, uint32_t d2);   /* (sa_loop.c: what is played, recorded) */
static void sl_all_off(void);

static struct {
    uint8_t panel;                           /* panel mode: a host has the FM-1 */
    uint8_t host_t;                          /* the transport it came on (RF_T_*) */
    uint32_t host_ms;                        /* its last message */
    uint8_t ch;                              /* the MIDI channel, 1..16 */
    int8_t oct;                              /* -3..+3 */
    uint8_t bank, voice;                     /* 0..sa_nbanks() - 1 (the ROM's, then the user's), 0..31 */
    uint8_t theme, bg;                       /* the theme, its background (sa_theme.h SA_BGS) */
    uint8_t ccbank;                          /* 0..4: A..E */
    uint8_t cc[5][4], ccv[5][4];             /* each bank's CCs (sa_cc.c) and their values */
    uint8_t knob;                            /* the knob last turned */
    uint32_t knob_ms[4];
    uint32_t down_ms[16];                    /* when each button went down */
    uint32_t hold_done;                      /* buttons whose hold has fired (bit B_*) */
    uint8_t menu;                            /* the overlay is a menu (sa_overlay_menu) */
    uint8_t view;                            /* HOME's view: 0 Notes, 1 Scope, 2 Keys */
    uint8_t mfocus;                          /* a settings menu's option (Rec options, Arp settings: 0..3) */
    uint32_t sel_ms;                         /* SELECT's last detent */
    uint8_t arp_mode, arp_rate, arp_oct, arp_gate;   /* sa_arp.c's settings */
    uint16_t bpm;                            /* the tempo (SELECT), 40..240 */
    uint8_t rec_len, rec_quant, rec_count, rec_layer;   /* sa_loop.c's recording options */
    uint8_t ubank_seq;                       /* the user banks' count of arrivals (sa_ubank.c) */
    char name[12];                           /* the voice's */
    uint32_t keys;                           /* keys held (as played: lib/touch) */
    uint32_t btns;                           /* buttons held (plat.h's B_*) */
    uint8_t key_note[NKEYS], key_ch[NKEYS];  /* what each held key started (0xFF: nothing, after a panic) */
    uint8_t key_arp[NKEYS];                  /* ... given to the arp */
    uint8_t held[128];                       /* the notes sounding from the keys and MIDI in, counted: the screen */
    uint8_t latch;                           /* LATCH (LFO held): the keys' notes held after they are let go */
    uint8_t rm_disp, rm_rel, rm_cc_ch, rm_key_ch, rm_keys, rm_octk;   /* the settings (SA_RMD_*, SA_RMR_*, 1..16,
                                              * 1..16, SA_RMK_*, the OCT buttons shift MIDI KEYBOARD's octave) */
    uint8_t scale_root, scale, rm_scl;       /* the scale (sa_scale.c): root 0..11 (C..B), SA_SC_*; its GLO
                                              * shortcuts in MIDI KEYBOARD */
    uint8_t lat_n, lat_note[32], lat_ch[32], lat_arp[32];   /* the notes latched (let go, still held) */
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
    uint32_t notes_out, notes_in, ev_drop, panics, saves, loads, cc_out, save_holds;   /* (the simulator's checks) */
} sa;

#include "sa_ubank.c"

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
#define SA_SCOPE_N 512u                      /* the Scope view's samples: every other one of the synth's sum (before
                                              * the limiter and MASTER), Q15 of full scale, clamped */
static int16_t sa_scope[SA_SCOPE_N] PLAT_POOL;
static volatile uint32_t sa_scope_w;

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
            if (i & 1u) {
                float r = (float)sa_mono[k + i] * pg * 32767.0f;
                sa_scope[sa_scope_w++ % SA_SCOPE_N] = (int16_t)(r > 32767.0f ? 32767 : r < -32767.0f ? -32767 : r);
            }
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

static void sar_key(uint32_t note, uint32_t vel);   /* (sa_arp.c) */
static int sar_is_on(void);

static void sa_latch_off1(uint32_t i)       /* latched note i let go now */
{
    if (sa.lat_arp[i]) {
        sar_key(sa.lat_note[i], 0);
    } else {
        sa_note(sa.lat_note[i], 0);
        sa_midi3(0x80u | sa.lat_ch[i], sa.lat_note[i], 0x40u);
        sa_rec_ev(SA_LE_OFF, sa.lat_note[i], 0x40u);
    }
}

static void sa_latch_release(void)           /* every latched note let go */
{
    uint32_t i;
    for (i = 0; i < sa.lat_n; i++)
        sa_latch_off1(i);
    sa.lat_n = 0;
}

static void sa_key(uint32_t k, uint32_t vel)       /* a key down (vel) or up (0) */
{
    if (vel) {
        int32_t n = (int32_t)SA_NOTE0 + (int32_t)k + 12 * sa.oct;
        n = clamp(n, 0, 127);
        if (sa.latch && sa.lat_n) {          /* LATCH: a new chord (no key held) replaces the one held; the same
                                              * note again is let go first */
            if (!sa.keys) {
                sa_latch_release();
            } else {
                uint32_t i, j;
                for (i = 0; i < sa.lat_n; i++)
                    if (sa.lat_note[i] == (uint32_t)n) {
                        sa_latch_off1(i);
                        for (j = i; j + 1u < sa.lat_n; j++)
                            sa.lat_note[j] = sa.lat_note[j + 1u], sa.lat_ch[j] = sa.lat_ch[j + 1u],
                            sa.lat_arp[j] = sa.lat_arp[j + 1u];
                        sa.lat_n--;
                        break;
                    }
            }
        }
        sa.key_note[k] = (uint8_t)n;
        sa.key_ch[k] = (uint8_t)(sa.ch - 1u);
        sa.key_arp[k] = (uint8_t)sar_is_on();
        sa.notes_out++;
        if (sa.key_arp[k]) {                 /* the arp plays it (sa_arp.c) */
            sar_key((uint32_t)n, vel);
            sa.poke = 1;
            return;
        }
        sa_note((uint32_t)n, vel);
        sa_midi3(0x90u | sa.key_ch[k], (uint32_t)n, vel);
        sa_rec_ev(SA_LE_ON, (uint32_t)n, vel);
    } else if (sa.key_note[k] != 0xFFu && sa.latch && sa.lat_n < 32u) {   /* LATCH: it stays */
        sa.lat_note[sa.lat_n] = sa.key_note[k];
        sa.lat_ch[sa.lat_n] = sa.key_ch[k];
        sa.lat_arp[sa.lat_n] = sa.key_arp[k];
        sa.lat_n++;
        sa.key_note[k] = 0xFFu;
    } else if (sa.key_note[k] != 0xFFu) {
        if (sa.key_arp[k]) {
            sar_key(sa.key_note[k], 0);
        } else {
            sa_note(sa.key_note[k], 0);
            sa_midi3(0x80u | sa.key_ch[k], sa.key_note[k], 0x40u);
            sa_rec_ev(SA_LE_OFF, sa.key_note[k], 0x40u);
        }
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
    sa_latch_release();
    sa_remote_off_all();
    for (k = 0; k < 128u; k++)
        sa.held[k] = 0;
    sa_ev(now ? SA_EV_SOUNDOFF : SA_EV_ALLOFF, 0, 0);
}

static void sa_latch_set(int on)             /* LFO held: LATCH on / off (off lets its notes go) */
{
    sa.latch = (uint8_t)(on != 0);
    if (!on)
        sa_latch_release();
    sa.poke = 1;
}

static void sa_panic(void)
{
    sl_all_off();
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

/* the settings (OBJ_SA, a record in OBJ_RF: src/rfm_store.c). Version 1 (S1) had the first 12 bytes; version 2 adds
 * the CC map and values (S2), the arp, the tempo and the recording options (S3, S4), the HOME view (S5) */
typedef struct {
    uint32_t magic;
    uint8_t ver, ch, bank, voice;
    int8_t oct;
    uint8_t theme, ccbank, view;
    uint8_t cc[5][4], ccv[5][4];
    uint8_t arp_mode, arp_rate, arp_oct, arp_gate;
    uint16_t bpm;
    uint8_t rec_len, rec_quant, rec_count, rec_layer;
    uint8_t ubank_seq, bg;                   /* (bg: 0, black, in records before it) */
    /* version 3 (2026-10-30): the settings page and the scale */
    uint8_t scale_root, scale, rm_disp, rm_rel, rm_cc_ch, rm_key_ch, rm_keys, rm_octk, rm_scl, rsv[7];
} sa_store_t;
#define SA_STORE_V2_HEAD ((int)__builtin_offsetof(sa_store_t, scale_root))   /* (version 2: the bytes before it) */

static void sa_settings_apply(const sa_store_t *s);
static void sa_settings_fill(sa_store_t *s);

static void sa_load(void)
{
    sa_store_t s;
    int n;
    memset(&s, 0, sizeof s);
    n = plat_store_load(OBJ_SA, &s, sizeof s);
    if (n < 12 || s.magic != SA_OBJ_MAGIC || s.ver < 1u || s.ver > 3u)
        return;
    if (s.ver != 3u || n != (int)sizeof s) {  /* (S1's: the CC map and the rest at their defaults; version 2's: the
                                              * settings page and the scale at theirs) */
        sa_store_t d;
        int keep = s.ver == 2u && n >= SA_STORE_V2_HEAD ? SA_STORE_V2_HEAD : 12;
        memset(&d, 0, sizeof d);
        sa_settings_fill(&d);
        memcpy(&d, &s, (uint32_t)keep);
        s = d;
    }
    sa_settings_apply(&s);
    sa.loads++;
}

static int sa_save_now(void)
{
    sa_store_t s;
    memset(&s, 0, sizeof s);
    sa_settings_fill(&s);
    s.magic = SA_OBJ_MAGIC;
    s.ver = 3;
    sa.store_dirty = 0;
    if (plat_store_save(OBJ_SA, &s, sizeof s) != 0)
        return -1;
    sa.saves++;
    return 0;
}

static void sa_store_step(void)
{
    uint32_t since = plat_ms() - sa.store_ms;
    if (!sa.store_dirty || since < SA_STORE_MS || sa_bench_busy())
        return;
    if (sa_synth_voices() && since < SA_STORE_FORCE_MS)
        return;                              /* (a save stops the sound a moment: wait for a quiet one) */
    sa_save_now();
}

static void sa_overlay(uint32_t scr)
{
    uint32_t now = plat_ms();
    if (sa.ov != scr || (int32_t)(now - sa.ov_until) >= 0)
        sa.ov_t0 = now;
    sa.ov = (uint8_t)scr;
    sa.ov_until = now + SA_OVERLAY_MS;
    sa.menu = 0;
    sa.poke = 1;
}

static void sa_overlay_menu(uint32_t scr)    /* a menu: stays (no timeout) until HOME, its button or another screen */
{
    sa_overlay(scr);
    sa.menu = 1;
    sa.ov_until = plat_ms() + SA_MENU_MS;
}

static void sa_close(void)                   /* back to the HOME view */
{
    sa.ov = SA_SCR_HOME;
    sa.menu = 0;
    sa.poke = 1;
}

#include "sa_scale.c"
#include "sa_loop.c"
#include "sa_arp.c"
#include "sa_cc.c"
#include "sa_remote.c"

static void sa_settings_fill(sa_store_t *s)
{
    s->ch = sa.ch;
    s->oct = sa.oct;
    s->bank = sa.bank;
    s->voice = sa.voice;
    s->theme = sa.theme;
    s->bg = sa.bg;
    s->ccbank = sa.ccbank;
    s->view = sa.view;
    memcpy(s->cc, sa.cc, sizeof s->cc);
    memcpy(s->ccv, sa.ccv, sizeof s->ccv);
    s->arp_mode = sa.arp_mode;
    s->arp_rate = sa.arp_rate;
    s->arp_oct = sa.arp_oct;
    s->arp_gate = sa.arp_gate;
    s->bpm = sa.bpm;
    s->rec_len = sa.rec_len;
    s->rec_quant = sa.rec_quant;
    s->rec_count = sa.rec_count;
    s->rec_layer = sa.rec_layer;
    s->ubank_seq = sa.ubank_seq;
    s->scale_root = sa.scale_root;
    s->scale = sa.scale;
    s->rm_disp = sa.rm_disp;
    s->rm_rel = sa.rm_rel;
    s->rm_cc_ch = sa.rm_cc_ch;
    s->rm_key_ch = sa.rm_key_ch;
    s->rm_keys = sa.rm_keys;
    s->rm_octk = sa.rm_octk;
    s->rm_scl = sa.rm_scl;
}

static void sa_settings_apply(const sa_store_t *s)   /* (each value checked: a record from another build) */
{
    uint32_t b, k;
    sa.ch = (uint8_t)clamp(s->ch, 1, 16);
    sa.oct = (int8_t)clamp(s->oct, -3, 3);
    sa.bank = (uint8_t)(s->bank < sa_nbanks() ? s->bank : 0u);
    sa.voice = (uint8_t)(s->voice < sa_bank_voices(sa.bank) ? s->voice : 0u);
    sa.theme = (uint8_t)(s->theme < SA_NTHEMES ? s->theme : SA_THEME_DEFAULT);
    sa.bg = (uint8_t)(s->bg < SA_NBG ? s->bg : 0u);   /* (CLASSIC kept: black where a theme has none) */
    sa.ccbank = (uint8_t)(s->ccbank < SA_NCCB ? s->ccbank : 0u);
    sa.view = (uint8_t)(s->view < 3u ? s->view : 0u);
    for (b = 0; b < SA_NCCB; b++)
        for (k = 0; k < 4u; k++) {
            sa.cc[b][k] = (uint8_t)(s->cc[b][k] <= SA_CC_MAX ? s->cc[b][k] : SA_CC_DEFAULT[b][k]);
            sa.ccv[b][k] = (uint8_t)(s->ccv[b][k] & 0x7Fu);
        }
    sa.arp_mode = (uint8_t)(s->arp_mode < SA_ARP_NMODES ? s->arp_mode : 0u);
    sa.arp_rate = (uint8_t)(s->arp_rate < SA_ARP_NRATES ? s->arp_rate : SA_ARP_RATE_DEFAULT);
    sa.arp_oct = (uint8_t)clamp(s->arp_oct, 1, 4);
    sa.arp_gate = (uint8_t)clamp(s->arp_gate, 1, 10);
    sa.bpm = (uint16_t)clamp(s->bpm, SA_BPM_MIN, SA_BPM_MAX);
    sa.rec_len = (uint8_t)(s->rec_len < SA_REC_NLENS ? s->rec_len : 0u);
    sa.rec_quant = (uint8_t)(s->rec_quant < 3u ? s->rec_quant : 0u);
    sa.rec_count = (uint8_t)(s->rec_count < 2u ? s->rec_count : 0u);
    sa.rec_layer = (uint8_t)(s->rec_layer < 2u ? s->rec_layer : 0u);
    sa.ubank_seq = s->ubank_seq;
    sa.scale_root = (uint8_t)(s->scale_root < 12u ? s->scale_root : 0u);
    sa.scale = (uint8_t)(s->scale < SA_NSCALES ? s->scale : 0u);
    sa.rm_disp = (uint8_t)(s->rm_disp < SA_RMD_N ? s->rm_disp : SA_RMD_ALL);
    sa.rm_rel = (uint8_t)(s->rm_rel < SA_RMR_N ? s->rm_rel : 0u);
    sa.rm_cc_ch = (uint8_t)clamp(s->rm_cc_ch, 1, 16);
    sa.rm_key_ch = (uint8_t)clamp(s->rm_key_ch, 1, 16);
    sa.rm_keys = (uint8_t)(s->rm_keys < SA_RMK_N ? s->rm_keys : 0u);
    sa.rm_octk = (uint8_t)(s->rm_octk != 0u);
    sa.rm_scl = (uint8_t)(s->rm_scl != 0u);
}

static void sa_settings_defaults(void)
{
    sa.ch = 1;
    sa.theme = SA_THEME_DEFAULT;
    sa_cc_defaults();
    sa.arp_rate = SA_ARP_RATE_DEFAULT;
    sa.arp_oct = 1;
    sa.arp_gate = 6;
    sa.bpm = 120;
    sa.rec_layer = 1;                        /* DUB: REC on a loop adds to it */
    sa.rm_disp = SA_RMD_ALL;
    sa.rm_cc_ch = sa.rm_key_ch = 1;
    sa.rm_octk = 1;
    sa.rm_scl = 1;
}

/* ------------------------------------------------------------------ the modes --- */
static void sa_enter_panel(uint32_t t)
{
    sa.host_t = (uint8_t)t;
    if (sa.panel)
        return;
    sa_rm_exit(1);                           /* (MIDI REMOTE ends: the host has the panel) */
    sl_halt();
    sa_all_off(1);
    sar_set(sar.on);                         /* (the arp stops: on again, as it was, when the host leaves) */
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
    if (!sa.panel && !rm.on && cin == 15u && st >= 0xF8u) {
        sar_realtime(st);                    /* MIDI clock, start, continue, stop (sa_arp.c) */
        return;
    }
    if (rm.on)
        return;                              /* MIDI REMOTE: the DX7 does not listen */
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
        if (d1 < sa_bank_voices(sa.bank) && d1 != sa.voice) {
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
    b |= 1u << (B_FX + sa.ccbank);           /* the CC bank's button */
    if (sl.state == SL_PLAY || sl.state == SL_DUB || (sl.state == SL_COUNT && sl.count_play))
        b |= 1u << B_PLAY;                   /* a loop plays */
    if (sl.state == SL_REC || sl.state == SL_DUB || (sl.state == SL_COUNT && plat_ms() % 500u < 250u))
        b |= 1u << B_REC;                    /* recording (blinking: the count) */
    if (sar.on)
        b |= 1u << B_ARP;
    if (sa.latch) {                          /* LATCH: LFO blinks (lit 600 ms of each second) */
        b &= ~(1u << B_LFO);
        if (plat_ms() % 1000u < 600u)
            b |= 1u << B_LFO;
    }
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

static void sa_press_early(uint32_t b)       /* SAVE, ARP, PLAY, REC down (most act when let go: sa_tap) */
{
    if ((b == B_PLAY || b == B_REC) && (sl.state == SL_REC || sl.state == SL_DUB)) {
        sl_rec_stop(1);                      /* a recording ends on the press, in time; the loop plays */
        sa.hold_done |= 1u << b;             /* (its release does nothing more) */
        sa_close();
    }
}

static void sa_menu_touch(void) { sa.ov_until = plat_ms() + SA_MENU_MS; sa.poke = 1; }

static void sa_recopt(uint32_t o, int32_t d) /* the recording options: LEN, QUANT, COUNT, LAYER */
{
    static const uint8_t N[4] = {SA_REC_NLENS, 3, 2, 2};
    uint8_t *v[4] = {&sa.rec_len, &sa.rec_quant, &sa.rec_count, &sa.rec_layer};
    int32_t x = clamp((int32_t)*v[o & 3u] + d, 0, N[o & 3u] - 1);
    sa.mfocus = (uint8_t)(o & 3u);
    if (x != *v[o & 3u]) {
        *v[o & 3u] = (uint8_t)x;
        sa_changed();
    }
}

static void sa_arpopt(uint32_t o, int32_t d) /* the arp's settings: MODE, RATE, OCT, GATE */
{
    static const uint8_t LO[4] = {0, 0, 1, 1}, HI[4] = {SA_ARP_NMODES - 1u, SA_ARP_NRATES - 1u, 4, 10};
    uint8_t *v[4] = {&sa.arp_mode, &sa.arp_rate, &sa.arp_oct, &sa.arp_gate};
    int32_t x = clamp((int32_t)*v[o & 3u] + d, LO[o & 3u], HI[o & 3u]);
    sa.mfocus = (uint8_t)(o & 3u);
    if (x != *v[o & 3u]) {
        *v[o & 3u] = (uint8_t)x;
        sa_changed();
    }
}

static void sa_setopt(uint32_t o, int32_t d) /* the settings' rows: SCREEN KNOBS CC-CH KEY-CH KEYS OCT-KEYS SCALES */
{
    static const uint8_t LO[SA_NSET + 1u] = {0, 0, 1, 1, 0, 0, 0},
                         HI[SA_NSET + 1u] = {SA_RMD_N - 1u, SA_RMR_N - 1u, 16, 16, SA_RMK_N - 1u, 1, 1};
    uint8_t *v[SA_NSET + 1u] = {&sa.rm_disp, &sa.rm_rel, &sa.rm_cc_ch, &sa.rm_key_ch, &sa.rm_keys, &sa.rm_octk,
                                &sa.rm_scl};
    int32_t x;
    o = o < SA_NSET ? o : SA_NSET - 1u;
    x = clamp((int32_t)*v[o] + d, LO[o], HI[o]);
    sa.mfocus = (uint8_t)o;
    if (x != *v[o]) {
        *v[o] = (uint8_t)x;
        sa_changed();
    }
}

static int sa_menu_knob(uint32_t k, int32_t d)   /* a menu takes KN1..KN4 (k 0..3) or PRESETS (4): 1 taken */
{
    if (!sa.menu)
        return 0;
    switch (sa.ov) {
    case SA_SCR_RECOPT:
        if (k < 4u)
            sa_recopt(k, d);
        else
            sa.mfocus = (uint8_t)clamp((int32_t)sa.mfocus + d, 0, 3);
        break;
    case SA_SCR_LOOPS:
        sl.cursor = (uint8_t)clamp((int32_t)sl.cursor + d, 0, (int32_t)SA_LOOPS - 1);
        break;
    case SA_SCR_ARPSET:
        if (k < 4u)
            sa_arpopt(k, d);
        else
            sa.mfocus = (uint8_t)clamp((int32_t)sa.mfocus + d, 0, 3);
        break;
    case SA_SCR_SETTINGS:
        if (k < 4u)
            sa_setopt(k, d);
        else
            sa.mfocus = (uint8_t)clamp((int32_t)sa.mfocus + d, 0, (int32_t)SA_NSET - 1);
        break;
    case SA_SCR_THEME:                       /* the theme, shown at once (the whole screen in it) */
        sa.theme = (uint8_t)(((int32_t)sa.theme + d % (int32_t)SA_NTHEMES + (int32_t)SA_NTHEMES) % (int32_t)SA_NTHEMES);
        sa_changed();
        break;
    default:
        return 0;
    }
    sa_menu_touch();
    return 1;
}

static void sa_select(int32_t d)             /* SELECT: the tempo; in a menu, its choice */
{
    if (sa.menu && sa.ov == SA_SCR_RECOPT) {
        sa_recopt(sa.mfocus, d);
        sa_menu_touch();
    } else if (sa.menu && sa.ov == SA_SCR_LOOPS) {
        sa_menu_knob(4u, d);
    } else if (sa.menu && sa.ov == SA_SCR_ARPSET) {
        sa_arpopt(sa.mfocus, d);
        sa_menu_touch();
    } else if (sa.menu && sa.ov == SA_SCR_THEME) {
        sa_menu_knob(4u, d);
    } else if (sa.menu && sa.ov == SA_SCR_SETTINGS) {
        sa_setopt(sa.mfocus, d);
        sa_menu_touch();
    } else {                                 /* the tempo (faster when turned fast) */
        uint32_t now = plat_ms(), dt = now - sa.sel_ms;
        int32_t b = clamp((int32_t)sa.bpm + d * (dt < 40u ? 4 : 1), SA_BPM_MIN, SA_BPM_MAX);
        sa.sel_ms = now;
        if (b != sa.bpm) {
            sa.bpm = (uint16_t)b;
            sa_changed();
        }
        sa_overlay(SA_SCR_TEMPO);
    }
}

static void sa_tap(uint32_t b)               /* SAVE, ARP, PLAY, REC let go before SA_HOLD_MS */
{
    if (b == B_SAVE) {                       /* the theme menu; SAVE again closes it (the theme kept) */
        if (sa.menu && sa.ov == SA_SCR_THEME)
            sa_close();
        else
            sa_overlay_menu(SA_SCR_THEME);
    } else if (b == B_PLAY) {
        if (sa.menu && sa.ov == SA_SCR_LOOPS) {   /* the loop under the cursor: next, or now */
            sl_pick(sl.cursor);
            sa_overlay(SA_SCR_LOOP);
        } else {
            sl_play_tap();
            if (sl.state != SL_PLAY && sa.ov == SA_SCR_LOOP)
                sa_close();
        }
    } else if (b == B_ARP) {
        if (sa.menu && sa.ov == SA_SCR_ARPSET) {
            sa_close();
        } else {
            sa_latch_release();              /* (the latched notes go: the arp's or not, as they were) */
            sar_set(!sar.on);
            sa_overlay(SA_SCR_ARP);
        }
    } else if (b == B_HOME) {                /* the HOME view at once; at it, the next one: Notes, Scope, Keys */
        if (sa.ov == SA_SCR_HOME && !sl_recording()) {
            sa.view = (uint8_t)((sa.view + 1u) % 3u);
            sa_changed();
        }
        sa_close();
    } else if (b == B_LFO) {                 /* (tapped: CC bank D, as the other bank buttons) */
        sa_cc_bank(B_LFO - B_FX);
    } else if (b == B_REC) {
        if (sa.menu && sa.ov == SA_SCR_RECOPT)
            sa_close();
        else if (sl.state == SL_COUNT)
            sl_rec_stop(1);                  /* (the count called off) */
        else {
            sl_rec_start();
            sa_close();
        }
    }
}

static void sa_hold(uint32_t b)              /* ... held SA_HOLD_MS */
{
    if (b == B_SAVE) {                       /* save everything now */
        sa.save_holds++;
        sa_save_now();
        sa_overlay(SA_SCR_SAVED);
    } else if (b == B_PLAY) {                /* the 16 loops */
        sl.cursor = sl.next != 0xFFu ? sl.next : sl.cur;
        sa_overlay_menu(SA_SCR_LOOPS);
    } else if (b == B_ARP) {                 /* the arp's settings */
        sa_overlay_menu(SA_SCR_ARPSET);
    } else if (b == B_REC && !sl_recording()) {   /* the recording options */
        sa_overlay_menu(SA_SCR_RECOPT);
    } else if (b == B_HOME) {                /* the settings page; held again: closed */
        if (sa.menu && sa.ov == SA_SCR_SETTINGS) {
            sa_close();
        } else {
            sa.mfocus = 0;
            sa_overlay_menu(SA_SCR_SETTINGS);
        }
    } else if (b == B_LFO) {                 /* LATCH on / off */
        sa_latch_set(!sa.latch);
        sa_overlay(SA_SCR_LATCH);
    }
}

static void sa_press(uint32_t b)             /* any other button down */
{
    if (b <= B_EDIT) {                       /* FX SEL ENV LFO EDIT: the CC bank */
        sa_cc_bank(b - B_FX);
    } else if (b == B_GLO) {
        sa.poke = 1;
    }
}

#define SA_HOLDABLE (1u << B_SAVE | 1u << B_ARP | 1u << B_PLAY | 1u << B_REC | 1u << B_LFO | 1u << B_HOME)
static void sa_buttons(uint32_t btns)
{
    const uint32_t OD = 1u << B_OCTDN, OU = 1u << B_OCTUP;
    uint32_t down = btns & ~sa.btns, up = sa.btns & ~btns, now = plat_ms(), b;
    if (sa_rm_combo(down, btns)) {           /* GLO + FX: MIDI REMOTE (sa_remote.c) */
        sa.btns = btns;
        return;
    }
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
    for (b = 0; b < (uint32_t)B_OCTDN; b++) {
        uint32_t m = 1u << b;
        if (down & m) {
            sa.down_ms[b] = now;
            sa.hold_done &= ~m;
            if (b == B_FX)
                sa_rm_fx_note();
            if (!(SA_HOLDABLE & m))
                sa_press(b);
            else
                sa_press_early(b);
        }
        if ((SA_HOLDABLE & m) && (btns & m) && !(sa.hold_done & m) && now - sa.down_ms[b] >= SA_HOLD_MS) {
            sa.hold_done |= m;
            sa_hold(b);
        }
        if ((up & m) && (SA_HOLDABLE & m) && !(sa.hold_done & m))
            sa_tap(b);
    }
}

static void sa_encoders(const int32_t *steps)   /* detents by plat.h's role (EN_*) */
{
    int32_t s = steps[EN_PRESET], c = steps[EN_ALGO], k;
    for (k = 0; k < 4; k++)
        if (steps[EN_K1 + k] && !sa_menu_knob((uint32_t)k, steps[EN_K1 + k]))
            sa_cc_knob((uint32_t)k, steps[EN_K1 + k]);
    if (steps[EN_SELECT])
        sa_select(steps[EN_SELECT]);
    if (s && sa_menu_knob(4u, s))
        s = 0;
    if (s) {
        if (sa.btns >> B_GLO & 1u) {
            int32_t b = clamp((int32_t)sa.bank + s, 0, (int32_t)sa_nbanks() - 1);
            if (b != sa.bank) {
                sa.bank = (uint8_t)b;
                if (sa.voice >= sa_bank_voices(sa.bank))
                    sa.voice = 0;
                sa_load_voice();
                sa_changed();
            }
        } else {
            int32_t v = clamp((int32_t)sa.voice + s, 0, (int32_t)sa_bank_voices(sa.bank) - 1);
            if (v != sa.voice) {
                sa.voice = (uint8_t)v;
                sa_load_voice();
                sa_changed();
            }
        }
        sa_overlay(SA_SCR_PRESET);
    }
    if (c && sa.menu && sa.ov == SA_SCR_THEME) {   /* the theme menu: ALGORITHM the background */
        sa.bg = (uint8_t)sa_bg_step(sa.theme, sa.bg, c);
        sa_changed();
        sa_menu_touch();
        c = 0;
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
    if (rm.on) {                             /* MIDI REMOTE: the panel only sends MIDI */
        sa_rm_ms(keys, btns & ((1u << NB) - 1u), steps);
        if (rm.on) {
            sa_rm_leds();
            sa_store_step();
            return;
        }
        sa.keys = keys;
        return;
    }
    for (ch = keys ^ sa.keys, k = 0; ch; ch >>= 1, k++) {
        if (!(ch & 1u))
            continue;
        if ((keys >> k & 1u) && (btns >> B_GLO & 1u) && sa_key_black(k) >= 0) {   /* GLO + a black key: the octave */
            int8_t o = (int8_t)sa_black_oct((uint32_t)sa_key_black(k));
            if (o != sa.oct) {
                sa.oct = o;
                sa_changed();
            }
            sa_overlay(SA_SCR_OCTAVE);
            continue;
        }
        sa_key(k, (keys >> k & 1u) ? touch_vel(k) : 0u);
    }
    sa.keys = keys;
    sa_buttons(btns & ((1u << NB) - 1u));
    sa_encoders(steps);
    sar_ms(now);
    sl_step(now);
    {
        int32_t u = sa_ub_take();            /* a DX7 bank arrived: kept, and its first voice played */
        if (u >= 0) {
            sa.bank = (uint8_t)sa_bank_of_slot((uint32_t)u);
            sa.voice = 0;
            sa_load_voice();
            sa_changed();
            sa_overlay(SA_SCR_PRESET);
        }
    }
    if (sa.ov != SA_SCR_HOME && sa.ov != SA_SCR_CCBANK && !sa.menu && (int32_t)(now - sa.ov_until) >= 0) {
        /* (CC-Bank and the menus stay: until HOME or another screen) */
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
    memset(&rm, 0, sizeof rm);               /* (MIDI REMOTE is never kept: off at power-on) */
    sa_settings_defaults();
    memset(&ub, 0, sizeof ub);
    sa_ub_scan();
    sl_init();
    sar_init();
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
