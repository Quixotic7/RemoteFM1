/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The M-Vave SMK-37's controls the FM-1 has not (docs/SMK37.md; fm1-base's plat_smk_take, BOARD_SMK37), standalone:
 * the instrument's. Part of sa_main.c (its state, sa; MIDI REMOTE's rm).
 *
 *   outer keys     C3..E3 and G#5..C6 play as the 27 keys do (keys 27..36 of sa_key: the octave, the scale's SNAP,
 *                  LATCH, the arp, the loop, the keys' velocity or VELOCITY's fixed one); MIDI REMOTE: notes on KEY CH
 *   pitch wheel    pitch bend: the DX7's, and out on the channel
 *   mod wheel      CC 1 (the DX7 hears it: its mod wheel), and out
 *   pedal          CC 64, sustain (the DX7 holds its notes), and out
 *   faders 1..4    CC 16..19 (General Purpose 1..4) out, and to the DX7 (which does not use them)
 *   KNOB BANK, FADER BANK, <, >   CC 110..113 out, 127 down / 0 up
 * In MIDI REMOTE the wheels, the pedal, the faders and the buttons go on CC CH, the outer keys on KEY CH (rm.oct). The
 * session's side (a host's firmware gets the same as MIDI in) is src/rfm_main.c's rf_smk_ms. */

#define SA_SMK_FADER_CC 16u                  /* fader f: CC 16 + f */
#define SA_SMK_BTN_CC 110u                   /* PLAT_SMK_KNOB_BANK .. RIGHT: CC 110 .. 113 */

static uint32_t sa_smk_slot(uint32_t key)   /* keybed key 0..36 (an outer one) -> sa_key's 27..36 */
{
    return key < SMK_KEY_F3 ? NKEYS + key : key;   /* (32..36 are G#5..C6 as they are) */
}

static void sa_smk_cc(uint32_t cc, uint32_t v)
{
    if (rm.on)
        sa_rm_send(0xB0u | SA_RM_CC_CH(), cc, v, SA_RM_SRC_BTN, 0);
    else
        sa_cc_send(cc, v);                   /* out, the DX7, the loop being recorded */
}

static void sa_smk(const plat_smk_ev_t *e)
{
    if (sa.panel)
        return;
    switch (e->type) {
    case PLAT_SMK_KEY: {
        uint32_t k = sa_smk_slot(e->id), bit = 1u << (k - NKEYS), v = (uint32_t)e->v;
        if (k < NKEYS || k >= SA_NKEYS)
            return;
        if (rm.on) {                         /* MIDI REMOTE: a note on KEY CH, the mode's octave */
            if (v) {
                int32_t n = clamp((int32_t)SA_NOTE0 + sa_key_off(k) + 12 * rm.oct, 0, 127);
                if (sa_rm_kbd())
                    n = (int32_t)sa_snap((uint32_t)n);
                sa.key_note[k] = (uint8_t)n;
                sa.key_ch[k] = (uint8_t)SA_RM_KEY_CH();
                sa_rm_send(0x90u | sa.key_ch[k], (uint32_t)n, v, SA_RM_SRC_KEY, k);
            } else if (sa.key_note[k] != 0xFFu) {
                sa_rm_send(0x80u | sa.key_ch[k], sa.key_note[k], 0x40u, SA_RM_SRC_KEY, k);
                sa.key_note[k] = 0xFFu;
            }
            return;
        }
        if (v && sa.base == SA_SCR_LOOPS)
            return;                          /* (the loops' page: the 16 white keys pick; the outer keys nothing) */
        if (v) {
            if (sa.okeys & bit)
                return;
            sa.okeys |= bit;
            sa_key(k, sa.vel_on ? v : SA_VEL_FIXED);
        } else if (sa.okeys & bit) {
            sa.okeys &= ~bit;                /* (before sa_key: a LATCH's next chord sees it let go) */
            sa_key(k, 0);
        }
        return;
    }
    case PLAT_SMK_BEND: {
        uint32_t b = (uint32_t)(e->v + 8192);
        if (rm.on) {
            sa_rm_send(0xE0u | SA_RM_CC_CH(), b & 0x7Fu, b >> 7, SA_RM_SRC_BTN, 0);
            return;
        }
        sa_ev(SA_EV_BEND, b, 0);
        sa_midi3(0xE0u | (sa.ch - 1u), b & 0x7Fu, b >> 7);
        return;
    }
    case PLAT_SMK_MOD: sa_smk_cc(1u, (uint32_t)e->v); return;
    case PLAT_SMK_PEDAL: sa_smk_cc(64u, e->v ? 127u : 0u); return;
    case PLAT_SMK_FADER: sa_smk_cc(SA_SMK_FADER_CC + e->id, (uint32_t)e->v); return;
    case PLAT_SMK_BTN: sa_smk_cc(SA_SMK_BTN_CC + e->id, e->v ? 127u : 0u); return;
    default: return;
    }
}
