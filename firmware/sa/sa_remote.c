/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* MIDI REMOTE (the owner, 2026-10-30): the whole panel as a plain MIDI controller. GLO + FX (either first) goes in, and
 * GLO + FX again comes out; the mode is not kept across a power cycle. In it nothing else works: the keys, every
 * button, every encoder and MASTER only send MIDI on the current channel (USB and Bluetooth at once); the DX7 is
 * silent (the keys do not play it, MIDI in and clock are ignored), the loops and the arp stopped. The map (fixed;
 * docs/PROTOCOL.md section 8 and README.md):
 *   keys           notes, F3..G5 shifted by the octave as it was when the mode came on, with velocity; note off 64
 *   buttons        CC, 127 pressed / 0 let go: FX 20, SEL 21, ENV 22, LFO 23, EDIT 24, GLO 25, HOME 26, SAVE 27,
 *                  ARP 28, SEQ 29, PLAY 30, REC 31, OCT- 85, OCT+ 86
 *   encoders       CC, absolute 0..127 (each starts at 64 when the mode comes on; x2 / x4 when turned fast):
 *                  SELECT 102, ALGORITHM 103, PRESETS 104, KN1 105, KN2 106, KN3 107, KN4 108
 *   MASTER         CC 109, 0..127 (sent when it moves)
 * All of these CCs are undefined in General MIDI (20..31, 85, 86, 102..119): free for a DAW's MIDI learn. Coming out
 * sends 0 for every button still held (GLO's own, at least), so nothing is left on at the other end. Part of
 * sa_main.c (its state, sa). */

static const uint8_t SA_RM_BTN_CC[NB] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 85, 86};
static const uint8_t SA_RM_ENC_CC[NE] = {102, 103, 104, 105, 106, 107, 108};
#define SA_RM_MASTER_CC 109u
#define SA_RM_FX_MS 400u                     /* FX first, then GLO this soon: the mode, FX's bank A undone */

static struct {
    uint8_t on;
    int8_t oct;                              /* the keys' octave in the mode */
    uint8_t enc[NE];                         /* the encoders' values */
    int32_t master;                          /* MASTER's last value sent, -1 none yet */
    uint32_t enc_ms[NE];
    uint8_t last[3];                         /* the last message sent: status, data 1, data 2 */
    uint8_t src;                             /* what sent it: SA_RM_SRC_* */
    uint8_t src_i;                           /* (the key, button or encoder) */
    uint32_t mute;                           /* buttons held as the mode came on (GLO, FX): their release sends nothing */
    uint32_t n;                              /* messages sent (the screen's flash, the simulator's checks) */
    uint32_t last_ms;
    /* FX first: what its bank change undoes */
    uint32_t fx_ms;
    uint8_t fx_bank, fx_ov, fx_menu;
    uint32_t fx_until;
} rm;
enum { SA_RM_SRC_NONE, SA_RM_SRC_KEY, SA_RM_SRC_BTN, SA_RM_SRC_ENC, SA_RM_SRC_MASTER };

static void sa_rm_send(uint32_t st, uint32_t d1, uint32_t d2, uint32_t src, uint32_t i)
{
    sa_midi3(st | (sa.ch - 1u), d1, d2);
    rm.last[0] = (uint8_t)(st | (sa.ch - 1u));
    rm.last[1] = (uint8_t)(d1 & 0x7Fu);
    rm.last[2] = (uint8_t)(d2 & 0x7Fu);
    rm.src = (uint8_t)src;
    rm.src_i = (uint8_t)i;
    rm.n++;
    rm.last_ms = plat_ms();
    sa.poke = 1;
}

static void sa_rm_enter(void)
{
    uint32_t i;
    sl_halt();
    sa_all_off(1);                           /* (the keys' notes off as they were sent; the DX7 silent) */
    sar_set(sar.on);                         /* (the arp's notes off; on again, as it was, after) */
    memset(&rm, 0, sizeof rm);
    rm.on = 1;
    rm.oct = sa.oct;
    rm.master = -1;
    for (i = 0; i < NE; i++)
        rm.enc[i] = 64;
    sa.ov = SA_SCR_HOME;
    sa.menu = 0;
    sa.led_b = sa.led_k = 0xFFFFFFFFu;
    sa.poke = 1;
}

static void sa_rm_exit(int send)             /* send: 0 for the buttons held, the keys' note offs */
{
    uint32_t b, k;
    if (!rm.on)
        return;
    if (send) {
        for (b = 0; b < NB; b++)
            if ((sa.btns & ~rm.mute) >> b & 1u)
                sa_midi3(0xB0u | (sa.ch - 1u), SA_RM_BTN_CC[b], 0);
        for (k = 0; k < NKEYS; k++)
            if (sa.key_note[k] != 0xFFu) {
                sa_midi3(0x80u | sa.key_ch[k], sa.key_note[k], 0x40u);
                sa.key_note[k] = 0xFFu;
            }
    }
    for (k = 0; k < NKEYS; k++)
        sa.key_note[k] = 0xFFu;
    rm.on = 0;
    sa.ov = SA_SCR_HOME;
    sa.led_b = sa.led_k = 0xFFFFFFFFu;
    sa.poke = 1;
}

/* GLO + FX: 1 when it toggled the mode (the press then does nothing else). FX first: its bank A undone */
static int sa_rm_combo(uint32_t down, uint32_t btns)
{
    const uint32_t G = 1u << B_GLO, F = 1u << B_FX;
    if ((btns & (G | F)) != (G | F) || !(down & (G | F)))
        return 0;
    if (!(down & F) && !rm.on) {             /* GLO second: FX's press made bank A the bank: undone */
        if (plat_ms() - rm.fx_ms > SA_RM_FX_MS)
            return 0;
        sa.ccbank = rm.fx_bank;
        sa.ov = rm.fx_ov;
        sa.menu = rm.fx_menu;
        sa.ov_until = rm.fx_until;
    }
    if (rm.on)
        sa_rm_exit(1);
    else {
        sa_rm_enter();
        rm.mute = btns;
    }
    return 1;
}

static void sa_rm_fx_note(void)              /* (FX down in the instrument: what GLO second would undo) */
{
    rm.fx_ms = plat_ms();
    rm.fx_bank = sa.ccbank;
    rm.fx_ov = sa.ov;
    rm.fx_menu = sa.menu;
    rm.fx_until = sa.ov_until;
}

static void sa_rm_ms(uint32_t keys, uint32_t btns, const int32_t *steps)
{
    uint32_t ch, k, b, down = btns & ~sa.btns, up = sa.btns & ~btns;
    for (ch = keys ^ sa.keys, k = 0; ch; ch >>= 1, k++) {
        if (!(ch & 1u))
            continue;
        if (keys >> k & 1u) {
            int32_t n = clamp((int32_t)SA_NOTE0 + (int32_t)k + 12 * rm.oct, 0, 127);
            uint32_t v = touch_vel(k);
            sa.key_note[k] = (uint8_t)n;
            sa.key_ch[k] = (uint8_t)(sa.ch - 1u);
            sa_rm_send(0x90u, (uint32_t)n, v ? v : 1u, SA_RM_SRC_KEY, k);
        } else if (sa.key_note[k] != 0xFFu) {
            sa_rm_send(0x80u, sa.key_note[k], 0x40u, SA_RM_SRC_KEY, k);
            sa.key_note[k] = 0xFFu;
        }
    }
    sa.keys = keys;
    if (sa_rm_combo(down, btns)) {
        sa.btns = btns;
        return;                              /* (out: the panel is the instrument's again from the next ms) */
    }
    for (b = 0; b < NB; b++) {
        if (down >> b & 1u) {
            rm.mute &= ~(1u << b);
            sa_rm_send(0xB0u, SA_RM_BTN_CC[b], 127, SA_RM_SRC_BTN, b);
        }
        if ((up & ~rm.mute) >> b & 1u)
            sa_rm_send(0xB0u, SA_RM_BTN_CC[b], 0, SA_RM_SRC_BTN, b);
    }
    rm.mute &= btns;
    sa.btns = btns;
    for (k = 0; k < NE; k++) {
        if (steps[k]) {
            uint32_t now = plat_ms(), dt = now - rm.enc_ms[k];
            int32_t mul = dt < 15u ? 4 : dt < 40u ? 2 : 1, v = clamp((int32_t)rm.enc[k] + steps[k] * mul, 0, 127);
            rm.enc_ms[k] = now;
            if (v != rm.enc[k]) {
                rm.enc[k] = (uint8_t)v;
                sa_rm_send(0xB0u, SA_RM_ENC_CC[k], (uint32_t)v, SA_RM_SRC_ENC, k);
            }
        }
    }
    {
        int32_t m = (int32_t)(plat_master() > 4096u ? 4096u : plat_master()), v = (m * 127 + 2048) / 4096;
        /* a new value once MASTER is 3/4 of a step from the last one (no jitter on a boundary) */
        if (rm.master < 0) {
            rm.master = v;                   /* (where it stands when the mode comes on: nothing sent) */
        } else if (v != rm.master && (m * 127 - rm.master * 4096 > 3072 || rm.master * 4096 - m * 127 > 3072)) {
            rm.master = v;
            sa_rm_send(0xB0u, SA_RM_MASTER_CC, (uint32_t)v, SA_RM_SRC_MASTER, 0);
        }
    }
}

static void sa_rm_leds(void)                 /* the buttons and keys held, lit */
{
    uint32_t b = sa.btns & ((1u << NB) - 1u), k = sa.keys;
    if (b == sa.led_b && k == sa.led_k && plat_ms() - sa.led_ms < SA_LED_REFRESH_MS)
        return;
    sa.led_b = b;
    sa.led_k = k;
    sa.led_ms = plat_ms();
    rf_sa_leds(b, k);
}
