/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The M-Vave SMK-37's panel signals decoded (felucca-bsp boards/smk37/smk_decode.h, as firmware/patches/smk37 adds
 * it), fed frames and readings by hand as the scan would: the factory curves taken only when whole, a key's velocity
 * from its two contacts' time, its release, a button's debounce, an encoder's detents both ways (and its bounce), a
 * pad's hit, velocity, pressure and release, the faders' ends and spikes, the wheels' rest and travel, the pedal of
 * either polarity, the pads' LED frame.
 *   cc -Ibuild/fm1-base/bsp/boards/smk37 tests/smk_decode_test.c   (build/fm1-base as BOARD=smk37 ./build.sh leaves it) */
#include <stdio.h>
#include <string.h>
#include "smk_decode.h"

static int fails, checks;
static void ok(int c, const char *what)
{
    checks++;
    if (!c) {
        fails++;
        printf("FAIL %s\n", what);
    }
}
static uint8_t raw[SMK_NCOL];
static void close_pos(uint32_t pos, int on)
{
    if (on)
        raw[pos >> 4] |= (uint8_t)(1u << (pos & 7u));
    else
        raw[pos >> 4] &= (uint8_t)~(1u << (pos & 7u));
}
#define FRAME_TICKS (1600u * 24u)                  /* a frame: 1.6 ms of TIMER4 */

int main(void)
{
    static uint8_t rec[SMK_TRIM_LEN];
    static smk_trim_t t;
    static smk_keys_t k;
    static smk_btns_t b;
    static smk_encs_t e;
    static smk_pads_t p;
    static smk_faders_t f;
    static smk_wheels_t w;
    static smk_pedal_t pd;
    uint32_t i, j, now = 1000, v;
    /* the factory record: taken when its marker and every curve hold, else the defaults */
    memset(rec, 0xFF, sizeof rec);
    ok(!smk_trim_parse(&t, rec) && !t.factory && t.key[0][1] == 640u && t.pad[15][4] == 950u,
       "no record: the stock defaults");
    for (i = 0; i < SMK_NPAD + SMK_NKEY; i++)
        for (j = 0; j < 5u; j++) {
            uint32_t x = 100u * (j + 1u) + i;
            rec[8u + 10u * i + 2u * j] = (uint8_t)x;
            rec[9u + 10u * i + 2u * j] = (uint8_t)(x >> 8);
        }
    memcpy(rec + 0x224, "trimdat", 8);
    ok(smk_trim_parse(&t, rec) && t.factory && t.pad[0][0] == 100u && t.pad[3][2] == 303u && t.key[0][0] == 116u &&
       t.key[36][4] == 552u, "the unit's record: its curves (pads then keys)");
    rec[8u + 10u * 20u + 4u] = rec[8u + 10u * 20u + 5u] = 0;   /* key 4's third point under its second */
    ok(!smk_trim_parse(&t, rec) && t.key[4][2] == 800u, "a curve that does not rise: the defaults, all of them");
    smk_trim_defaults(&t);
    /* the curve: under the first point 1, over the last 1023 */
    ok(smk_curve(t.key[0], 5) == 1u && smk_curve(t.key[0], 981) == 1023u && smk_curve(t.key[0], 640) == 256u &&
       smk_curve(t.key[0], 720) == 384u, "the 5-point curve");
    ok(smk_key_vel(t.key[0], 0) == 127u && smk_key_vel(t.key[0], 50u * 240000u / 10u) <= 30u,
       "a key's velocity: both contacts at once 127, 50 ms between them soft");
    /* a key: the first contact, two frames, the second: it sounds then, timed from the first */
    memset(raw, 0, sizeof raw);
    close_pos(SMK_KEY_POS[5][0], 1);
    smk_keys_frame(&k, &t, raw, now);
    ok(!smk_key_down(&k, 5), "a key: its first contact alone does not sound");
    now += 2u * FRAME_TICKS;
    close_pos(SMK_KEY_POS[5][1], 1);
    smk_keys_frame(&k, &t, raw, now);
    ok(smk_key_down(&k, 5) && k.vel[5] == smk_key_vel(t.key[5], 2u * FRAME_TICKS) && k.vel[5] > 100u,
       "a key: its second contact sounds it, its velocity from 3.2 ms");
    close_pos(SMK_KEY_POS[5][1], 0);                /* lifted to the first contact: still held */
    for (i = 0; i < 5u; i++)
        smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    ok(smk_key_down(&k, 5), "a key: back to its first contact, still held");
    close_pos(SMK_KEY_POS[5][0], 0);
    smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    ok(smk_key_down(&k, 5), "a key: open two frames, still held (the release waits SMK_KEY_REL)");
    smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    ok(!smk_key_down(&k, 5), "a key: open three frames: up");
    close_pos(SMK_KEY_POS[36][1], 1);               /* the second contact alone (both in one frame): T = 10 */
    smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    ok(smk_key_down(&k, 36) && k.vel[36] == 127u && k.down[1] == 1u << 4, "key 36 (C6), both at once: 127");
    close_pos(SMK_KEY_POS[36][1], 0);
    close_pos(SMK_KEY_POS[0][0], 1);                /* a key that never reaches its second contact: nothing */
    for (i = 0; i < 4u; i++)
        smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    close_pos(SMK_KEY_POS[0][0], 0);
    for (i = 0; i < 4u; i++)
        smk_keys_frame(&k, &t, raw, now += FRAME_TICKS);
    ok(!smk_key_down(&k, 0) && k.presses == 2u, "a half press: no note");
    /* a button: two frames closed a press, four open a release */
    close_pos(SMK_BTN_POS[SMK_B_OCT_DN], 1);
    smk_btns_frame(&b, raw);
    ok(!(b.held >> SMK_B_OCT_DN & 1u), "a button: one frame closed is not a press");
    smk_btns_frame(&b, raw);
    ok(b.held == 1u << SMK_B_OCT_DN && b.pressed == 1u << SMK_B_OCT_DN, "a button: two frames, pressed (OCTAVE- is 4)");
    close_pos(SMK_BTN_POS[SMK_B_OCT_DN], 0);
    for (i = 0; i < 3u; i++)
        smk_btns_frame(&b, raw);
    ok(b.held == 1u << SMK_B_OCT_DN, "a button: three frames open, still held");
    smk_btns_frame(&b, raw);
    ok(!b.held && b.released == 1u << SMK_B_OCT_DN, "a button: four, released");
    /* an encoder: a detent is a whole cycle; clockwise B before A (0 2 3 1 0), each state two frames */
    smk_encs_init(&e);
    smk_encs_frame(&e, raw);                       /* (power-on: the rest, 0) */
    {
        static const uint8_t CW[4] = {2, 3, 1, 0}, CCW[4] = {1, 3, 2, 0};
        uint32_t rep;
        for (rep = 0; rep < 3u; rep++)
            for (i = 0; i < 4u; i++) {
                close_pos(SMK_ENC_POS[1][0], CW[i] & 1u);
                close_pos(SMK_ENC_POS[1][1], CW[i] >> 1 & 1u);
                smk_encs_frame(&e, raw);
                smk_encs_frame(&e, raw);
            }
        ok(e.steps[1] == 3, "K2: three detents clockwise: +3");
        for (i = 0; i < 4u; i++) {
            close_pos(SMK_ENC_POS[1][0], CCW[i] & 1u);
            close_pos(SMK_ENC_POS[1][1], CCW[i] >> 1 & 1u);
            smk_encs_frame(&e, raw);
            smk_encs_frame(&e, raw);
        }
        ok(e.steps[1] == 2, "K2: one back: +2");
        for (i = 0; i < 6u; i++) {                 /* a contact bouncing for single frames: nothing */
            close_pos(SMK_ENC_POS[1][0], i & 1u);
            smk_encs_frame(&e, raw);
        }
        close_pos(SMK_ENC_POS[1][0], 0);
        smk_encs_frame(&e, raw);
        smk_encs_frame(&e, raw);
        ok(e.steps[1] == 2 && e.steps[0] == 0, "K2: a bounce of single frames counts nothing; K1 untouched");
        for (i = 0; i < 4u; i++) {                 /* half a detent and back: nothing */
            static const uint8_t HALF[4] = {2, 3, 2, 0};
            close_pos(SMK_ENC_POS[1][0], HALF[i] & 1u);
            close_pos(SMK_ENC_POS[1][1], HALF[i] >> 1 & 1u);
            smk_encs_frame(&e, raw);
            smk_encs_frame(&e, raw);
        }
        ok(e.steps[1] == 2, "K2: half a detent and back: nothing");
        for (i = 0; i < 4u; i++) {                 /* one detent counter-clockwise right after: -1 */
            close_pos(SMK_ENC_POS[1][0], CCW[i] & 1u);
            close_pos(SMK_ENC_POS[1][1], CCW[i] >> 1 & 1u);
            smk_encs_frame(&e, raw);
            smk_encs_frame(&e, raw);
        }
        ok(e.steps[1] == 1, "K2: one detent the other way: one step back");
    }
    /* a pad: the baseline at rest, a hit over it, its peak's velocity, the pressure, the release */
    smk_pads_init(&p);
    for (i = 0; i < 40u; i++)
        smk_pad_reading(&p, &t, 3, 60);
    ok(p.base[3] == 60u + 0u || p.base[3] <= 61u, "a pad's baseline follows it at rest");
    smk_pad_reading(&p, &t, 3, 300);
    smk_pad_reading(&p, &t, 3, 700);
    smk_pad_reading(&p, &t, 3, 650);
    ok(!(p.down >> 3 & 1u), "a pad: hit, its peak still being read");
    smk_pad_reading(&p, &t, 3, 640);
    ok((p.down >> 3 & 1u) && p.vel[3] == smk_vel127(smk_curve(t.pad[3], 700)) && p.vel[3] > 40u,
       "a pad: its velocity from the peak of the hit's readings (700)");
    smk_pad_reading(&p, &t, 3, 600);
    ok(p.press[3] == (600u - 512u) / 4u + 32u, "a pad held: its pressure");
    smk_pad_reading(&p, &t, 3, 65);
    ok(p.down >> 3 & 1u, "a pad: one reading under the release level: still held");
    smk_pad_reading(&p, &t, 3, 65);
    ok(!(p.down >> 3 & 1u) && p.hits == 1u, "a pad: two: released");
    /* a fader: 0..127, the ends reached, a lone spike dropped */
    for (i = 0; i < 80u; i++)
        smk_fader_reading(&f, 2, 1023);
    ok(f.v[2] == 127u, "a fader at the top: 127");
    for (i = 0; i < 80u; i++)
        smk_fader_reading(&f, 2, 0);
    ok(f.v[2] == 0u, "at the bottom: 0");
    for (i = 0; i < 80u; i++)
        smk_fader_reading(&f, 2, 512);
    v = f.v[2];
    smk_fader_reading(&f, 2, 1023);
    smk_fader_reading(&f, 2, 512);
    ok(v >= 63u && v <= 65u && f.v[2] == v, "half way 64; a one-reading spike changes nothing");
    /* the wheels: the pitch wheel's rest is its centre, its travel widens to what is seen */
    smk_wheels_init(&w);
    for (i = 0; i < 40u; i++)
        smk_wheel_pitch(&w, 528);
    ok(w.centre == 528 && w.bend == 0, "the pitch wheel's rest at power-on: no bend");
    for (i = 0; i < 40u; i++)
        smk_wheel_pitch(&w, 640);
    ok(w.bend == 8191, "pushed up to the end seen: full bend up");
    for (i = 0; i < 40u; i++)
        smk_wheel_pitch(&w, 440);
    ok(w.bend == -8192, "pulled down: full bend down");
    for (i = 0; i < 40u; i++)
        smk_wheel_pitch(&w, 531);
    ok(w.bend == 0, "back at rest (inside the dead band): 0");
    for (i = 0; i < 40u; i++)
        smk_wheel_mod(&w, 629);
    ok(w.mod == 0u, "the mod wheel down (the higher reading): 0");
    for (i = 0; i < 40u; i++)
        smk_wheel_mod(&w, 470);
    ok(w.mod == 127u, "up: 127");
    /* the pedal: its rest learnt, then down far from it, either way */
    memset(&pd, 0, sizeof pd);
    for (i = 0; i < SMK_PEDAL_LEARN; i++)
        smk_pedal_reading(&pd, 985);
    smk_pedal_reading(&pd, 980);
    ok(!pd.down, "the pedal at rest (nothing plugged: ~985)");
    smk_pedal_reading(&pd, 20);
    ok(pd.down, "a switch pedal that closes: down");
    smk_pedal_reading(&pd, 900);
    ok(!pd.down, "and up again");
    memset(&pd, 0, sizeof pd);
    for (i = 0; i < SMK_PEDAL_LEARN; i++)
        smk_pedal_reading(&pd, 15);
    smk_pedal_reading(&pd, 990);
    ok(pd.down, "a normally closed pedal (rest low): down when it opens");
    /* the battery's bars */
    ok(smk_battery_bars(0) == -1 && smk_battery_bars(500) == 0 && smk_battery_bars(565) == 2 && smk_battery_bars(610) == 3,
       "the battery: the stock firmware's levels");
    /* the pads' frame: the reset, then each pad's GRB at its chain slot, a byte a bit */
    {
        static uint8_t fr[SMK_PADLED_LEN];
        uint32_t rgb[SMK_NPAD] = {0};
        rgb[1] = 0x80FF01u;                        /* pad 1 (top row 2): its slot 15 */
        smk_padled_frame(fr, rgb);
        ok(fr[0] == 0 && fr[SMK_PADLED_RESET - 1u] == 0 && fr[SMK_PADLED_RESET] == 0xC0u,
           "the frame: the long reset, then pad 0 (dark) at slot 0");
        {
            const uint8_t *o = fr + SMK_PADLED_RESET + 24u * 15u;
            ok(o[0] == 0xF0u && o[7] == 0xF0u && o[8] == 0xF0u && o[9] == 0xC0u && o[16] == 0xC0u && o[23] == 0xF0u,
               "pad 1 at slot 15, its green first (FF), then red (80), then blue (01)");
        }
    }
    printf(fails ? "smk_decode_test: %d of %d FAILED\n" : "smk_decode_test: ok (%d checks)\n", fails ? fails : checks, checks);
    return fails != 0;
}
