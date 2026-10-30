/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* MIDI REMOTE (the owner, 2026-10-30): the whole panel as a plain MIDI controller. GLO + FX (either first) goes in, and
 * GLO + FX again comes out; the mode is not kept across a power cycle. In it nothing else works: the keys, every
 * button, every encoder and MASTER only send MIDI (USB and Bluetooth at once); the DX7 is silent (the keys do not play
 * it, MIDI in and clock are ignored), the loops and the arp stopped. The map (docs/PROTOCOL.md section 8 and README.md;
 * the settings page, HOME held in the instrument, sets the channels, the encoders' encoding, the keys' mode, the
 * screen):
 *   keys           notes on KEY CH, F3..G5 shifted by the octave as it was when the mode came on, with velocity;
 *                  note off 64
 *   buttons        CC on CC CH, 127 pressed / 0 let go: FX 20, SEL 21, ENV 22, LFO 23, EDIT 24, GLO 25, HOME 26,
 *                  SAVE 27, ARP 28, SEQ 29, PLAY 30, REC 31, OCT- 85, OCT+ 86
 *   encoders       CC on CC CH: SELECT 102, ALGORITHM 103, PRESETS 104, KN1 105, KN2 106, KN3 107, KN4 108; absolute
 *                  0..127 (each starts at 64 when the mode comes on; x2 / x4 when turned fast), or relative: 1..63 up /
 *                  127..65 down (two's complement), or 65.. up / ..63 down (offset 64)
 *   MASTER         CC 109 on CC CH, 0..127 (sent when it moves)
 *   MIDI KEYS      (KEYS in the settings) GLO is a shift and sends nothing: GLO + a white key the keys' channel (1..16
 *                  left to right), GLO + a black key the octave (sa_black_oct); OCT- / OCT+ the octave when OCT KEYS
 *                  is on (else their CCs); the scale applies to the keys (SNAP, sa_scale.c), and with SCALES on GLO +
 *                  PRESETS / ALGORITHM set its root / scale (no CC; with SCALES off they send their CCs, GLO still
 *                  sends none)
 * All of these CCs are undefined in General MIDI (20..31, 85, 86, 102..119): free for a DAW's MIDI learn. Coming out
 * sends 0 for every button still held that sent 127, so nothing is left on at the other end. Part of sa_main.c (its
 * state, sa). */

static const uint8_t SA_RM_BTN_CC[NB] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 85, 86};
static const uint8_t SA_RM_ENC_CC[NE] = {102, 103, 104, 105, 106, 107, 108};
#define SA_RM_MASTER_CC 109u
#define SA_RM_KNOB_MS 1500u                  /* the screen KNOBS ONLY: an encoder's value shown this long */
#define SA_RM_INFO_MS 1200u                  /* a shortcut's screen (the octave, the keys' channel, the scale) */
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
    int8_t rel;                              /* the last encoder's step in a relative mode (the screen) */
    uint8_t info;                            /* the last shortcut (SA_RM_INFO_*), shown a moment */
    uint32_t info_ms;
    uint32_t last_ms;
    /* FX first: what its bank change undoes */
    uint32_t fx_ms;
    uint8_t fx_bank, fx_ov, fx_base, fx_menu;
    uint32_t fx_until;
} rm;
enum { SA_RM_SRC_NONE, SA_RM_SRC_KEY, SA_RM_SRC_BTN, SA_RM_SRC_ENC, SA_RM_SRC_MASTER };
enum { SA_RM_INFO_NONE, SA_RM_INFO_OCT, SA_RM_INFO_KEYCH, SA_RM_INFO_SCALE };
#define SA_RM_CC_CH() (sa.rm_cc_ch - 1u)       /* the buttons', encoders' and MASTER's channel (0..15) */
#define SA_RM_KEY_CH() (sa.rm_key_ch - 1u)     /* the keys' */
static int sa_rm_kbd(void) { return sa.rm_keys == SA_RMK_PLAY; }

static void sa_rm_info(uint32_t what)
{
    rm.info = (uint8_t)what;
    rm.info_ms = plat_ms();
    sa.poke = 1;
}

static void sa_rm_send(uint32_t st, uint32_t d1, uint32_t d2, uint32_t src, uint32_t i)   /* st with its channel */
{
    sa_midi3(st, d1, d2);
    rm.last[0] = (uint8_t)st;
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
    sa.ov = sa.base;                         /* (the screen open stays under MIDI REMOTE's: back to it after) */
    sa.led_b = sa.led_k = 0xFFFFFFFFu;
    sa.poke = 1;
}

static int sa_rm_btn_cc(uint32_t b);
static void sa_rm_exit(int send)             /* send: 0 for the buttons held, the keys' note offs */
{
    uint32_t b, k;
    if (!rm.on)
        return;
    if (send) {
        for (b = 0; b < NB; b++)
            if (((sa.btns & ~rm.mute) >> b & 1u) && sa_rm_btn_cc(b))
                sa_midi3(0xB0u | SA_RM_CC_CH(), SA_RM_BTN_CC[b], 0);
        for (k = 0; k < NKEYS; k++)
            if (sa.key_note[k] != 0xFFu) {
                sa_midi3(0x80u | sa.key_ch[k], sa.key_note[k], 0x40u);
                sa.key_note[k] = 0xFFu;
            }
    }
    for (k = 0; k < NKEYS; k++)
        sa.key_note[k] = 0xFFu;
    rm.on = 0;
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
        sa.base = rm.fx_base;
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
    rm.fx_base = sa.base;
    rm.fx_menu = sa.menu;
    rm.fx_until = sa.ov_until;
}

static int sa_rm_btn_cc(uint32_t b)          /* the button sends its CC (in MIDI KEYBOARD: not GLO, not the OCT
                                              * buttons while they shift the octave) */
{
    if (!sa_rm_kbd())
        return 1;
    if (b == B_GLO)
        return 0;
    return !((b == B_OCTDN || b == B_OCTUP) && sa.rm_octk);
}

static void sa_rm_ms(uint32_t keys, uint32_t btns, const int32_t *steps)
{
    uint32_t ch, k, b, down = btns & ~sa.btns, up = sa.btns & ~btns, glo = sa_rm_kbd() && (btns >> B_GLO & 1u);
    for (ch = keys ^ sa.keys, k = 0; ch; ch >>= 1, k++) {
        if (!(ch & 1u))
            continue;
        if (keys >> k & 1u) {
            int32_t n, w = sa_key_white(k), bl = sa_key_black(k);
            uint32_t v = touch_vel(k);
            if (glo && w >= 0) {             /* MIDI KEYBOARD: GLO + a white key: the keys' channel 1..16 */
                sa.rm_key_ch = (uint8_t)(w + 1);
                sa_changed();
                sa_rm_info(SA_RM_INFO_KEYCH);
                continue;
            }
            if (glo && bl >= 0) {            /* ... GLO + a black key: the octave */
                rm.oct = (int8_t)sa_black_oct((uint32_t)bl);
                sa_rm_info(SA_RM_INFO_OCT);
                continue;
            }
            n = clamp((int32_t)SA_NOTE0 + (int32_t)k + 12 * rm.oct, 0, 127);
            if (sa_rm_kbd())
                n = (int32_t)sa_snap((uint32_t)n);
            sa.key_note[k] = (uint8_t)n;
            sa.key_ch[k] = (uint8_t)SA_RM_KEY_CH();
            sa_rm_send(0x90u | sa.key_ch[k], (uint32_t)n, v ? v : 1u, SA_RM_SRC_KEY, k);
        } else if (sa.key_note[k] != 0xFFu) {
            sa_rm_send(0x80u | sa.key_ch[k], sa.key_note[k], 0x40u, SA_RM_SRC_KEY, k);
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
            if (sa_rm_btn_cc(b)) {
                sa_rm_send(0xB0u | SA_RM_CC_CH(), SA_RM_BTN_CC[b], 127, SA_RM_SRC_BTN, b);
            } else if (b == B_OCTDN || b == B_OCTUP) {   /* MIDI KEYBOARD: the octave -3..+3 */
                rm.oct = (int8_t)clamp(rm.oct + (b == B_OCTUP ? 1 : -1), -3, 3);
                sa_rm_info(SA_RM_INFO_OCT);
            }
        }
        if (((up & ~rm.mute) >> b & 1u) && sa_rm_btn_cc(b))
            sa_rm_send(0xB0u | SA_RM_CC_CH(), SA_RM_BTN_CC[b], 0, SA_RM_SRC_BTN, b);
    }
    rm.mute &= btns;
    sa.btns = btns;
    for (k = 0; k < NE; k++) {
        if (steps[k]) {
            uint32_t now = plat_ms(), dt = now - rm.enc_ms[k];
            int32_t mul = dt < 15u ? 4 : dt < 40u ? 2 : 1, d = clamp(steps[k] * mul, -63, 63),
                    v = clamp((int32_t)rm.enc[k] + d, 0, 127);
            rm.enc_ms[k] = now;
            if (glo && sa_rm_glo_enc(k, steps[k]))
                continue;                    /* (item 5: GLO + PRESETS / ALGORITHM, the scale) */
            if (sa.rm_rel) {                 /* relative: every detent sent, 1/127 or 65/63 */
                rm.enc[k] = (uint8_t)v;
                rm.rel = (int8_t)d;
                sa_rm_send(0xB0u | SA_RM_CC_CH(), SA_RM_ENC_CC[k],
                           sa.rm_rel == SA_RMR_2C ? (uint32_t)(d & 0x7F) : (uint32_t)(64 + d), SA_RM_SRC_ENC, k);
            } else if (v != rm.enc[k]) {
                rm.enc[k] = (uint8_t)v;
                rm.rel = 0;
                sa_rm_send(0xB0u | SA_RM_CC_CH(), SA_RM_ENC_CC[k], (uint32_t)v, SA_RM_SRC_ENC, k);
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
            sa_rm_send(0xB0u | SA_RM_CC_CH(), SA_RM_MASTER_CC, (uint32_t)v, SA_RM_SRC_MASTER, 0);
        }
    }
}

static void sa_rm_leds(void)                 /* the buttons and keys held, lit; MIDI KEYS: the scale's keys dim */
{
    uint32_t b = sa.btns & ((1u << NB) - 1u), k = sa.keys, km = sa_rm_kbd() ? sa_scale_keys(rm.oct) & ~k : 0u;
    if (b == sa.led_b && k == sa.led_k && km == sa.led_km && plat_ms() - sa.led_ms < SA_LED_REFRESH_MS)
        return;
    sa.led_b = b;
    sa.led_k = k;
    sa.led_km = km;
    sa.led_ms = plat_ms();
    rf_sa_leds(b, k, km);
}
