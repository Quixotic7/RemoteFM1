/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The Notes view's chord names (firmware/sa/sa_chord.c) on the computer: tests/run_tests.sh. */
#include <stdio.h>
#include <string.h>
#include "../firmware/sa/sa_chord.c"

static int fails;
static void check(const char *want, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e)
{
    uint8_t n[5] = {a, b, c, d, e};
    uint32_t k = 0;
    char got[32];
    sa_chord_t ch;
    while (k < 5u && n[k])
        k++;
    sa_chord_name(n, k, &ch);
    snprintf(got, sizeof got, "%s%s%s", ch.main, ch.ext, ch.bass);
    if (strcmp(got, want)) {
        printf("FAIL %u %u %u %u %u: \"%s\", wanted \"%s\"\n", a, b, c, d, e, got, want);
        fails++;
    }
}

int main(void)
{
    sa_chord_t ch;
    uint8_t dm7[4] = {62, 65, 69, 72};
    char nm[8];
    check("Dm7", 62, 65, 69, 72, 0);
    check("C", 60, 64, 67, 0, 0);
    check("Cm", 60, 63, 67, 0, 0);
    check("Cmaj7", 60, 64, 67, 71, 0);
    check("C7", 60, 64, 67, 70, 0);
    check("C7", 60, 64, 70, 0, 0);                   /* no fifth */
    check("Bm7b5", 59, 62, 65, 69, 0);
    check("Cdim7", 60, 63, 66, 69, 0);
    check("Cdim", 60, 63, 66, 0, 0);
    check("Caug", 60, 64, 68, 0, 0);
    check("Csus2", 60, 62, 67, 0, 0);
    check("Csus4", 60, 65, 67, 0, 0);
    check("C7sus4", 60, 65, 67, 70, 0);
    check("C5", 48, 55, 60, 0, 0);
    check("C/E", 64, 67, 72, 0, 0);
    check("C/G", 55, 64, 72, 0, 0);
    check("Cadd9", 60, 64, 67, 74, 0);
    check("C9", 60, 64, 67, 70, 74);
    check("Cm9", 60, 63, 67, 70, 74);
    check("Cmaj9", 60, 64, 67, 71, 74);
    check("C6", 60, 64, 67, 69, 0);
    check("Am7", 57, 60, 64, 67, 0);
    check("Ebm", 63, 66, 70, 0, 0);
    check("F#", 66, 70, 73, 0, 0);
    check("C", 60, 0, 0, 0, 0);                      /* one note: its name */
    check("C", 60, 72, 0, 0, 0);
    check("C", 60, 61, 0, 0, 0);                     /* no chord: the bass */
    sa_chord_name(dm7, 4, &ch);                      /* D F A the core, C the extension */
    if (ch.core != (1u << 2 | 1u << 5 | 1u << 9)) {
        printf("FAIL Dm7's core %03X\n", ch.core);
        fails++;
    }
    sa_chord_name(dm7, 0, &ch);
    if (ch.main[0] || ch.found) {
        printf("FAIL nothing held: \"%s\"\n", ch.main);
        fails++;
    }
    sa_note_name(60, nm);
    if (strcmp(nm, "C4"))
        printf("FAIL 60 is %s\n", nm), fails++;
    sa_note_name(70, nm);
    if (strcmp(nm, "Bb4"))
        printf("FAIL 70 is %s\n", nm), fails++;
    sa_note_name(5, nm);
    if (strcmp(nm, "F-1"))
        printf("FAIL 5 is %s\n", nm), fails++;
    printf("sa_chord_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
