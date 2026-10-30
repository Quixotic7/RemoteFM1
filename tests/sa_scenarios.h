/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The DX7 voices and the performances the FM6 tests play, read the same way by RemoteFM1's side
 * (tests/sa_synth_test.c: firmware/sa/sa_synth.c) and saadet's (tests/fm6_ref.c: saadet's own instrument), so that
 * both render the same notes. Voices in the DX7's VCED order (155 bytes); events at block boundaries (32 frames). */
#pragma once
#include <stdint.h>

/* an operator, VCED order: R1-R4, L1-L4, BREAK POINT, L DEPTH, R DEPTH, L CURVE, R CURVE (0 -LIN, 1 -EXP, 2 +EXP,
 * 3 +LIN), RATE SCALING, AMS, KEY VELOCITY, OUTPUT, MODE (1 fixed), COARSE, FINE, DETUNE (7 = 0) */
#define SC_OP(r1, r2, r3, r4, l1, l2, l3, l4, bp, ld, rd, lc, rc, rs, ams, kvs, ol, mode, fc, ff, det)                \
    r1, r2, r3, r4, l1, l2, l3, l4, bp, ld, rd, lc, rc, rs, ams, kvs, ol, mode, fc, ff, det
/* the voice: pitch EG R1-R4, L1-L4, ALGORITHM (0..31), FEEDBACK, OSC KEY SYNC, LFO SPEED, DELAY, PMD, AMD, SYNC,
 * WAVE (0 tri, 1 saw down, 2 saw up, 3 square, 4 sine, 5 S&H), PMS, TRANSPOSE (24 = C3), NAME */
#define SC_VOICE(pr1, pr2, pr3, pr4, pl1, pl2, pl3, pl4, alg, fb, oks, lfs, lfd, lpmd, lamd, lks, lfw, lpms, trn)      \
    pr1, pr2, pr3, pr4, pl1, pl2, pl3, pl4, alg, fb, oks, lfs, lfd, lpmd, lamd, lks, lfw, lpms, trn

enum { SC_INIT, SC_EP, SC_BELL, SC_BRASS, SC_FB4, SC_ORGAN, SC_NVOICES };
static const uint8_t SC_VOICES[SC_NVOICES][155] = {
    {   /* the DX7's INIT VOICE */
        SC_OP(99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7),    /* OP6 */
        SC_OP(99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7),
        SC_OP(99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7),
        SC_OP(99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7),
        SC_OP(99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7),
        SC_OP(99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 99, 0, 1, 0, 7),    /* OP1 */
        SC_VOICE(99, 99, 99, 99, 50, 50, 50, 50, 0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24), 'I', 'N', 'I', 'T', ' ', 'V', 'O',
        'I', 'C', 'E'},
    {   /* ALGORITHM 5 (three pairs), OP6's feedback 6: velocity and key scaling on the modulators, a tine */
        SC_OP(95, 50, 35, 78, 99, 75, 0, 0, 60, 0, 50, 0, 1, 3, 0, 6, 72, 0, 14, 0, 7),
        SC_OP(95, 29, 20, 50, 99, 95, 0, 0, 39, 0, 0, 0, 0, 3, 0, 0, 99, 0, 1, 0, 4),
        SC_OP(95, 20, 20, 50, 99, 95, 0, 0, 48, 20, 30, 1, 1, 2, 0, 7, 78, 0, 1, 0, 10),
        SC_OP(95, 29, 20, 50, 99, 95, 0, 0, 39, 0, 0, 0, 0, 3, 0, 2, 99, 0, 1, 0, 7),
        SC_OP(95, 50, 35, 78, 99, 75, 0, 0, 60, 0, 70, 3, 1, 4, 0, 5, 80, 0, 1, 5, 7),
        SC_OP(96, 25, 25, 67, 99, 75, 0, 0, 39, 0, 0, 0, 0, 3, 0, 2, 99, 0, 1, 0, 6),
        SC_VOICE(94, 67, 95, 60, 50, 50, 50, 50, 4, 6, 1, 34, 33, 0, 0, 0, 4, 2, 24), 'T', 'E', 'S', 'T', ' ', 'E', 'P',
        ' ', ' ', ' '},
    {   /* ALGORITHM 3, a fixed-frequency modulator; the LFO a delayed sine on pitch and amplitude (AMS); OSC KEY
         * SYNC off: phases run on from note to note */
        SC_OP(99, 40, 30, 45, 99, 60, 30, 0, 50, 30, 0, 2, 0, 2, 2, 4, 85, 1, 2, 50, 9),
        SC_OP(98, 36, 25, 40, 99, 70, 20, 0, 39, 0, 0, 0, 0, 2, 0, 3, 90, 0, 3, 50, 7),
        SC_OP(99, 35, 22, 42, 99, 80, 50, 0, 39, 0, 0, 0, 0, 1, 3, 0, 99, 0, 1, 0, 8),
        SC_OP(99, 30, 25, 40, 99, 70, 30, 0, 39, 0, 0, 0, 0, 2, 0, 5, 82, 0, 7, 0, 6),
        SC_OP(99, 33, 28, 41, 99, 75, 35, 0, 39, 0, 0, 0, 0, 2, 1, 3, 88, 0, 2, 0, 7),
        SC_OP(99, 32, 21, 43, 99, 85, 60, 0, 39, 0, 0, 0, 0, 1, 2, 2, 99, 0, 1, 0, 7),
        SC_VOICE(99, 99, 99, 99, 50, 50, 50, 50, 2, 3, 0, 30, 40, 20, 30, 0, 4, 4, 24), 'T', 'E', 'S', 'T', ' ', 'B',
        'E', 'L', 'L', ' '},
    {   /* ALGORITHM 22, feedback 7; a pitch envelope at the attack, the LFO sample and hold on pitch; level scaling
         * on both sides, transposed up a tone */
        SC_OP(70, 45, 40, 60, 99, 90, 85, 0, 45, 40, 60, 3, 1, 4, 0, 4, 88, 0, 1, 0, 7),
        SC_OP(65, 40, 40, 55, 99, 92, 88, 0, 39, 0, 0, 0, 0, 2, 0, 0, 99, 0, 1, 0, 8),
        SC_OP(65, 40, 40, 55, 99, 92, 88, 0, 39, 0, 0, 0, 0, 2, 0, 0, 99, 0, 1, 0, 6),
        SC_OP(65, 40, 40, 55, 99, 92, 88, 0, 39, 0, 0, 0, 0, 2, 0, 0, 99, 0, 1, 0, 7),
        SC_OP(72, 50, 40, 60, 99, 88, 80, 0, 50, 50, 30, 1, 2, 3, 0, 6, 80, 0, 1, 0, 7),
        SC_OP(66, 40, 40, 56, 99, 93, 88, 0, 39, 0, 0, 0, 0, 2, 0, 1, 99, 0, 1, 0, 7),
        SC_VOICE(60, 50, 40, 70, 40, 52, 50, 50, 21, 7, 1, 45, 0, 10, 0, 1, 5, 6, 26), 'T', 'E', 'S', 'T', ' ', 'B',
        'R', 'A', 'S', 'S'},
    {   /* ALGORITHM 4: the feedback loop runs through OP6, OP5, OP4 (feedback 7); the LFO a square on pitch, high
         * rate scaling; OSC KEY SYNC off */
        SC_OP(90, 60, 50, 60, 99, 80, 60, 0, 39, 0, 0, 0, 0, 7, 0, 3, 85, 0, 2, 0, 7),
        SC_OP(90, 55, 50, 60, 99, 85, 60, 0, 39, 0, 0, 0, 0, 7, 0, 3, 80, 0, 1, 0, 9),
        SC_OP(92, 50, 45, 60, 99, 90, 70, 0, 39, 0, 0, 0, 0, 6, 0, 2, 99, 0, 1, 0, 7),
        SC_OP(88, 45, 40, 55, 99, 80, 50, 0, 39, 0, 0, 0, 0, 5, 0, 4, 82, 0, 3, 0, 5),
        SC_OP(93, 40, 40, 55, 99, 85, 50, 0, 39, 0, 0, 0, 0, 5, 0, 2, 78, 0, 1, 0, 7),
        SC_OP(91, 45, 40, 58, 99, 92, 75, 0, 39, 0, 0, 0, 0, 4, 0, 1, 99, 0, 0, 0, 7),
        SC_VOICE(99, 99, 99, 99, 50, 50, 50, 50, 3, 7, 0, 60, 10, 15, 0, 1, 3, 2, 24), 'T', 'E', 'S', 'T', ' ', 'F',
        'B', '4', ' ', ' '},
    {   /* ALGORITHM 32: six carriers (drawbars, one fixed), OP6's feedback 5; the LFO a triangle on pitch and
         * amplitude for the wheel, foot and breath */
        SC_OP(99, 80, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 1, 0, 80, 0, 8, 0, 7),
        SC_OP(99, 80, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 1, 0, 78, 0, 4, 0, 8),
        SC_OP(99, 80, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 2, 0, 84, 0, 3, 0, 6),
        SC_OP(99, 80, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 2, 0, 86, 1, 2, 30, 7),
        SC_OP(99, 80, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 90, 0, 2, 0, 7),
        SC_OP(99, 80, 99, 70, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 1, 92, 0, 0, 0, 7),
        SC_VOICE(99, 99, 99, 99, 50, 50, 50, 50, 31, 5, 1, 38, 0, 0, 0, 1, 0, 3, 24), 'T', 'E', 'S', 'T', ' ', 'O',
        'R', 'G', 'A', 'N'},
};

enum { E_END, E_ON, E_OFF, E_BEND, E_CC, E_AT, E_FN, E_LOAD, E_EDIT };
typedef struct {
    uint32_t block;                      /* the block it comes before (32 frames each) */
    uint8_t type;
    int32_t a, b;                        /* ON note vel, OFF note, BEND value, CC number value, AT value, FN (saadet's
                                          * FN_*) value, LOAD voice, EDIT byte value (the voice now, edited) */
} sc_event_t;

#define MS(ms) ((uint32_t)((ms) * 44100u / 1000u / 32u))
/* every voice: a chord, velocities from 30 to 127, the same key again while held (its voice retriggered), a bend
 * up and back, the wheel and aftertouch, the release and the tails */
static const sc_event_t SC_PLAY[] = {
    {MS(0), E_ON, 60, 100}, {MS(250), E_ON, 64, 80}, {MS(260), E_ON, 67, 60}, {MS(600), E_OFF, 64, 0},
    {MS(1000), E_OFF, 60, 0}, {MS(1000), E_OFF, 67, 0}, {MS(1200), E_ON, 48, 127}, {MS(1200), E_ON, 84, 30},
    {MS(1400), E_ON, 84, 90}, {MS(1600), E_BEND, 3000, 0}, {MS(1700), E_BEND, 8191, 0}, {MS(1800), E_BEND, -4000, 0},
    {MS(1900), E_BEND, 0, 0}, {MS(1600), E_CC, 1, 40}, {MS(1800), E_CC, 1, 127}, {MS(2000), E_AT, 90, 0},
    {MS(2200), E_CC, 1, 0}, {MS(2200), E_AT, 0, 0}, {MS(2400), E_OFF, 48, 0}, {MS(2400), E_OFF, 84, 0},
    {MS(2500), E_ON, 36, 110}, {MS(2700), E_OFF, 36, 0}, {MS(4000), E_END, 0, 0}};
/* more keys than voices: 20 notes held in turn, released in a different order, others coming in on their tails */
static const sc_event_t SC_STEAL[] = {
    {MS(0), E_ON, 40, 100}, {MS(50), E_ON, 43, 90}, {MS(100), E_ON, 47, 80}, {MS(150), E_ON, 50, 70},
    {MS(200), E_ON, 52, 100}, {MS(250), E_ON, 55, 60}, {MS(300), E_ON, 59, 110}, {MS(350), E_ON, 62, 120},
    {MS(400), E_OFF, 47, 0}, {MS(400), E_ON, 64, 90}, {MS(450), E_ON, 67, 80}, {MS(500), E_ON, 71, 70},
    {MS(550), E_ON, 74, 60}, {MS(600), E_ON, 76, 50}, {MS(650), E_ON, 79, 100}, {MS(700), E_ON, 83, 100},
    {MS(750), E_ON, 86, 100}, {MS(800), E_ON, 88, 100}, {MS(850), E_ON, 91, 100}, {MS(900), E_ON, 95, 100},
    {MS(950), E_ON, 98, 127}, {MS(1000), E_OFF, 40, 0}, {MS(1050), E_OFF, 62, 0}, {MS(1100), E_ON, 41, 100},
    {MS(1150), E_ON, 45, 100}, {MS(1200), E_OFF, 43, 0}, {MS(1200), E_OFF, 50, 0}, {MS(1200), E_OFF, 52, 0},
    {MS(1300), E_OFF, 55, 0}, {MS(1300), E_OFF, 59, 0}, {MS(1300), E_OFF, 64, 0}, {MS(1300), E_OFF, 67, 0},
    {MS(1400), E_OFF, 71, 0}, {MS(1400), E_OFF, 74, 0}, {MS(1400), E_OFF, 76, 0}, {MS(1400), E_OFF, 79, 0},
    {MS(1500), E_OFF, 83, 0}, {MS(1500), E_OFF, 86, 0}, {MS(1500), E_OFF, 88, 0}, {MS(1500), E_OFF, 91, 0},
    {MS(1500), E_OFF, 95, 0}, {MS(1500), E_OFF, 98, 0}, {MS(1500), E_OFF, 41, 0}, {MS(1500), E_OFF, 45, 0},
    {MS(3000), E_END, 0, 0}};
/* portamento: always on (FUNCTION mode), then glissando, then by CC 65 with its time from CC 5; a bend range of
 * 12 and stepped bend */
static const sc_event_t SC_PORTA[] = {
    {MS(0), E_FN, 3, 1}, {MS(0), E_FN, 4, 70}, {MS(0), E_ON, 48, 100}, {MS(300), E_ON, 60, 100},
    {MS(300), E_OFF, 48, 0}, {MS(700), E_ON, 55, 100}, {MS(700), E_OFF, 60, 0}, {MS(1000), E_FN, 5, 1},
    {MS(1000), E_ON, 67, 100}, {MS(1000), E_OFF, 55, 0}, {MS(1400), E_OFF, 67, 0}, {MS(1500), E_FN, 3, 0},
    {MS(1500), E_FN, 5, 0}, {MS(1500), E_CC, 65, 127}, {MS(1500), E_CC, 5, 40}, {MS(1500), E_ON, 43, 90},
    {MS(1800), E_ON, 62, 90}, {MS(1800), E_OFF, 43, 0}, {MS(2100), E_CC, 65, 0}, {MS(2100), E_ON, 50, 90},
    {MS(2100), E_OFF, 62, 0}, {MS(2300), E_FN, 0, 12}, {MS(2300), E_FN, 1, 12}, {MS(2400), E_BEND, 8191, 0},
    {MS(2500), E_BEND, -8192, 0}, {MS(2600), E_FN, 2, 2}, {MS(2600), E_BEND, 5000, 0}, {MS(2700), E_BEND, -2500, 0},
    {MS(2800), E_BEND, 0, 0}, {MS(2900), E_OFF, 50, 0}, {MS(3800), E_END, 0, 0}};
/* edits while notes sound (they follow), then another voice loaded (they stop: a DX7 program change) */
static const sc_event_t SC_EDITS[] = {
    {MS(0), E_ON, 57, 100}, {MS(0), E_ON, 64, 90}, {MS(400), E_EDIT, 126 + 9, 6},
    {MS(500), E_EDIT, 5 * 21 + 16, 70}, {MS(600), E_EDIT, 4 * 21 + 18, 3}, {MS(700), E_EDIT, 134, 7},
    {MS(900), E_LOAD, SC_FB4, 0}, {MS(1000), E_ON, 52, 100}, {MS(1200), E_ON, 59, 100},
    {MS(1500), E_EDIT, 126 + 18, 31}, {MS(1600), E_OFF, 52, 0}, {MS(1600), E_OFF, 59, 0},
    {MS(1700), E_LOAD, SC_EP, 0}, {MS(1800), E_ON, 69, 100}, {MS(2300), E_OFF, 69, 0}, {MS(3300), E_END, 0, 0}};
/* the wheel, foot, breath and aftertouch on pitch, amplitude and EG bias (FUNCTION mode targets), velocity scaled */
static const sc_event_t SC_CTRL[] = {
    {MS(0), E_FN, 7, 3}, {MS(0), E_FN, 8, 80}, {MS(0), E_FN, 9, 2}, {MS(0), E_FN, 10, 99}, {MS(0), E_FN, 11, 4},
    {MS(0), E_FN, 12, 60}, {MS(0), E_FN, 13, 7}, {MS(0), E_FN, 14, 1}, {MS(0), E_ON, 53, 127}, {MS(0), E_ON, 65, 64},
    {MS(200), E_CC, 1, 64}, {MS(400), E_CC, 1, 127}, {MS(600), E_CC, 4, 100}, {MS(800), E_CC, 2, 50},
    {MS(1000), E_CC, 2, 127}, {MS(1200), E_AT, 127, 0}, {MS(1400), E_CC, 121, 0}, {MS(1600), E_CC, 4, 30},
    {MS(1800), E_OFF, 53, 0}, {MS(1800), E_OFF, 65, 0}, {MS(2800), E_END, 0, 0}};

typedef struct {
    const char *name;
    uint32_t voice;
    const sc_event_t *ev;
} sc_scenario_t;
static const sc_scenario_t SC_SCENARIOS[] = {
    {"init_play", SC_INIT, SC_PLAY},     {"ep_play", SC_EP, SC_PLAY},         {"bell_play", SC_BELL, SC_PLAY},
    {"brass_play", SC_BRASS, SC_PLAY},   {"fb4_play", SC_FB4, SC_PLAY},       {"organ_play", SC_ORGAN, SC_PLAY},
    {"ep_steal", SC_EP, SC_STEAL},       {"brass_porta", SC_BRASS, SC_PORTA}, {"bell_edits", SC_BELL, SC_EDITS},
    {"organ_ctrl", SC_ORGAN, SC_CTRL},
};
#define SC_N (sizeof SC_SCENARIOS / sizeof SC_SCENARIOS[0])

static uint32_t sc_blocks(const sc_event_t *e)          /* the performance's length in blocks (E_END's) */
{
    while (e->type != E_END)
        e++;
    return e->block;
}
