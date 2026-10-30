/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone's MIDI controller (STANDALONE-PLAN.md S2): five banks, A..E (FX SEL ENV LFO EDIT), of four CCs, one
 * for each of KN1..KN4. A knob's detents move its CC's value 0..127 (faster when turned fast) and send it on the MIDI
 * channel, to USB and Bluetooth, and to the DX7 itself (which hears 1, 2, 4, 5, 64, 65). GLO held + a knob picks
 * which CC that knob sends in the bank shown (0..119: the channel mode messages 120..127 are never a knob's). The
 * map and the values are kept with the settings (sa_main.c's store record). The CC-Bank screen a bank button opens stays
 * (no timeout) until HOME or another screen: the knobs turned there (and GLO + a knob) change it in place; turned
 * from elsewhere they show CC-Turn / CC-Assign for a second. Part of sa_main.c (its state, sa). */

#define SA_NCCB 5u                           /* banks A..E */
#define SA_CC_MAX 119u                       /* the highest CC a knob may send */

static const uint8_t SA_CC_DEFAULT[SA_NCCB][4] = {
    {1, 11, 7, 10},                          /* A: the wheel, expression, volume, pan */
    {71, 74, 73, 72},                        /* B: resonance, brightness, attack, release (the mockup's) */
    {91, 93, 94, 95},                        /* C: reverb, chorus, detune, phaser */
    {70, 75, 76, 77},                        /* D: sound controllers: variation, decay, vibrato rate, depth */
    {16, 17, 18, 19},                        /* E: general purpose 1..4 */
};

/* the CC's name, as the CC-Assign screen shows it (General MIDI's names, short) */
static const char *sa_cc_name(uint32_t cc)
{
    static const struct {
        uint8_t cc;
        const char *name;
    } N[] = {{0, "BANK MSB"},    {1, "MOD WHEEL"},  {2, "BREATH"},     {4, "FOOT"},        {5, "PORTA TIME"},
             {6, "DATA ENTRY"},  {7, "VOLUME"},     {8, "BALANCE"},    {10, "PAN"},        {11, "EXPRESSION"},
             {12, "EFFECT 1"},   {13, "EFFECT 2"},  {16, "GENERAL 1"}, {17, "GENERAL 2"},  {18, "GENERAL 3"},
             {19, "GENERAL 4"},  {32, "BANK LSB"},  {64, "SUSTAIN"},   {65, "PORTAMENTO"}, {66, "SOSTENUTO"},
             {67, "SOFT PEDAL"}, {68, "LEGATO"},    {69, "HOLD 2"},    {70, "VARIATION"},  {71, "RESONANCE"},
             {72, "RELEASE"},    {73, "ATTACK"},    {74, "BRIGHTNESS"}, {75, "DECAY"},      {76, "VIB RATE"},
             {77, "VIB DEPTH"},  {78, "VIB DELAY"}, {79, "SOUND 10"},  {80, "GENERAL 5"},  {81, "GENERAL 6"},
             {82, "GENERAL 7"},  {83, "GENERAL 8"}, {84, "PORTA CTRL"}, {88, "HIRES VEL"},  {91, "REVERB"},
             {92, "TREMOLO"},    {93, "CHORUS"},    {94, "DETUNE"},    {95, "PHASER"},     {96, "DATA +1"},
             {97, "DATA -1"},    {98, "NRPN LSB"},  {99, "NRPN MSB"},  {100, "RPN LSB"},   {101, "RPN MSB"}};
    uint32_t i;
    for (i = 0; i < sizeof N / sizeof N[0]; i++)
        if (N[i].cc == cc)
            return N[i].name;
    if (cc >= 33u && cc <= 63u)
        return "LSB";                        /* (the low halves of 1..31) */
    return "UNDEFINED";
}

static void sa_cc_defaults(void)
{
    uint32_t b, k;
    for (b = 0; b < SA_NCCB; b++)
        for (k = 0; k < 4u; k++) {
            sa.cc[b][k] = SA_CC_DEFAULT[b][k];
            sa.ccv[b][k] = sa.cc[b][k] == 7u ? 100u : sa.cc[b][k] == 10u ? 64u : 0u;
        }
}

static void sa_cc_send(uint32_t cc, uint32_t v)   /* out (USB + Bluetooth), the DX7, the loop being recorded */
{
    sa_midi3(0xB0u | (sa.ch - 1u), cc, v);
    sa_ev(SA_EV_CC, cc, v);
    sa_rec_ev(SA_LE_CC, cc, v);
    sa.cc_out++;
}

/* a knob turned d detents: its CC's value (two at once: faster than 40 ms a detent x 2, faster than 15 ms x 4) */
static void sa_cc_knob(uint32_t k, int32_t d)
{
    uint32_t now = plat_ms(), dt = now - sa.knob_ms[k], b = sa.ccbank;
    int32_t mul = dt < 15u ? 4 : dt < 40u ? 2 : 1, v;
    sa.knob_ms[k] = now;
    sa.knob = (uint8_t)k;
    if (sa.btns >> B_GLO & 1u) {             /* GLO: the knob's CC */
        int32_t c = clamp((int32_t)sa.cc[b][k] + d, 0, (int32_t)SA_CC_MAX);
        if (c != sa.cc[b][k]) {
            sa.cc[b][k] = (uint8_t)c;
            sa_changed();
        }
        if (sa.ov == SA_SCR_CCBANK)
            sa.poke = 1;                     /* (the CC-Bank screen stays: in place) */
        else
            sa_overlay(SA_SCR_CCASSIGN);
        return;
    }
    v = clamp((int32_t)sa.ccv[b][k] + d * mul, 0, 127);
    if (v != sa.ccv[b][k]) {
        sa.ccv[b][k] = (uint8_t)v;
        sa_cc_send(sa.cc[b][k], (uint32_t)v);
        sa_changed();
    }
    if (sa.ov == SA_SCR_CCBANK)
        sa.poke = 1;
    else
        sa_overlay(SA_SCR_CCTURN);
}

static void sa_cc_bank(uint32_t b)           /* FX SEL ENV LFO EDIT */
{
    if (b != sa.ccbank) {
        sa.ccbank = (uint8_t)b;
        sa_changed();
    }
    sa_overlay(SA_SCR_CCBANK);
}
