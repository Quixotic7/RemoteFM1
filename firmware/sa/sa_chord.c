/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The Notes view's chord name (design/standalone-mockup Home-Notes: "Dm7", the extension in the accent colour): the
 * held notes' pitch classes against a table of chords, the bass first as the root, then the others (a slash chord).
 * No platform here: tests/sa_ui_test.c checks it on the computer.
 *
 * A name is three parts: the root and the quality ("D", "m": ink), the extension ("7": the accent), and after a slash
 * the bass when it is not the root ("/F#": ink). A chord of four notes or more may leave out its fifth. Notes that
 * match no chord give the bass's name alone. Each note is also marked core (the root, third and fifth the quality
 * names; a sus or power chord's root and fifth) or extension, for the note chips' colours. */
#include <stdint.h>

static const char *const SA_PC_NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

#define SA_I(i) (1u << (i))
enum { SAQ_MAJ, SAQ_MIN, SAQ_DIM, SAQ_AUG, SAQ_SUS };
static const uint16_t SA_CORE[5] = {SA_I(0) | SA_I(4) | SA_I(7), SA_I(0) | SA_I(3) | SA_I(7),
                                    SA_I(0) | SA_I(3) | SA_I(6), SA_I(0) | SA_I(4) | SA_I(8), SA_I(0) | SA_I(7)};
static const char *const SA_QNAME[5] = {"", "m", "dim", "aug", ""};
static const struct {
    uint16_t set;                        /* intervals from the root */
    uint8_t q;                           /* SAQ_* */
    char ext[8];
} SA_CHORDS[] = {
    {SA_I(0) | SA_I(4) | SA_I(7), SAQ_MAJ, ""},
    {SA_I(0) | SA_I(3) | SA_I(7), SAQ_MIN, ""},
    {SA_I(0) | SA_I(4) | SA_I(7) | SA_I(10), SAQ_MAJ, "7"},
    {SA_I(0) | SA_I(3) | SA_I(7) | SA_I(10), SAQ_MIN, "7"},
    {SA_I(0) | SA_I(4) | SA_I(7) | SA_I(11), SAQ_MAJ, "maj7"},
    {SA_I(0) | SA_I(3) | SA_I(6), SAQ_DIM, ""},
    {SA_I(0) | SA_I(4) | SA_I(8), SAQ_AUG, ""},
    {SA_I(0) | SA_I(2) | SA_I(7), SAQ_SUS, "sus2"},
    {SA_I(0) | SA_I(5) | SA_I(7), SAQ_SUS, "sus4"},
    {SA_I(0) | SA_I(7), SAQ_SUS, "5"},
    {SA_I(0) | SA_I(3) | SA_I(7) | SA_I(11), SAQ_MIN, "maj7"},
    {SA_I(0) | SA_I(3) | SA_I(6) | SA_I(10), SAQ_MIN, "7b5"},
    {SA_I(0) | SA_I(3) | SA_I(6) | SA_I(9), SAQ_DIM, "7"},
    {SA_I(0) | SA_I(4) | SA_I(8) | SA_I(10), SAQ_AUG, "7"},
    {SA_I(0) | SA_I(5) | SA_I(7) | SA_I(10), SAQ_SUS, "7sus4"},
    {SA_I(0) | SA_I(4) | SA_I(7) | SA_I(9), SAQ_MAJ, "6"},
    {SA_I(0) | SA_I(3) | SA_I(7) | SA_I(9), SAQ_MIN, "6"},
    {SA_I(0) | SA_I(2) | SA_I(4) | SA_I(7), SAQ_MAJ, "add9"},
    {SA_I(0) | SA_I(2) | SA_I(3) | SA_I(7), SAQ_MIN, "add9"},
    {SA_I(0) | SA_I(2) | SA_I(4) | SA_I(7) | SA_I(10), SAQ_MAJ, "9"},
    {SA_I(0) | SA_I(2) | SA_I(4) | SA_I(7) | SA_I(11), SAQ_MAJ, "maj9"},
    {SA_I(0) | SA_I(2) | SA_I(3) | SA_I(7) | SA_I(10), SAQ_MIN, "9"},
    {SA_I(0) | SA_I(2) | SA_I(4) | SA_I(7) | SA_I(9), SAQ_MAJ, "6/9"},
    {SA_I(0) | SA_I(1) | SA_I(4) | SA_I(7) | SA_I(10), SAQ_MAJ, "7b9"},
    {SA_I(0) | SA_I(3) | SA_I(4) | SA_I(7) | SA_I(10), SAQ_MAJ, "7#9"},
    {SA_I(0) | SA_I(4) | SA_I(6) | SA_I(7) | SA_I(11), SAQ_MAJ, "maj7#11"},
    {SA_I(0) | SA_I(2) | SA_I(4) | SA_I(5) | SA_I(7) | SA_I(10), SAQ_MAJ, "11"},
    {SA_I(0) | SA_I(2) | SA_I(3) | SA_I(5) | SA_I(7) | SA_I(10), SAQ_MIN, "11"},
    {SA_I(0) | SA_I(3) | SA_I(5) | SA_I(7) | SA_I(10), SAQ_MIN, "11"},
    {SA_I(0) | SA_I(4) | SA_I(7) | SA_I(9) | SA_I(10), SAQ_MAJ, "13"},
    {SA_I(0) | SA_I(2) | SA_I(4) | SA_I(7) | SA_I(9) | SA_I(10), SAQ_MAJ, "13"},
};
#define SA_NCHORDS (sizeof SA_CHORDS / sizeof SA_CHORDS[0])

typedef struct {
    char main[12];                       /* root and quality ("Dm"): ink */
    char ext[10];                        /* the extension ("7"): the accent */
    char bass[5];                        /* "/F#" when the bass is not the root, else "" */
    uint16_t core;                       /* pitch classes of the core (bit pc): the rest are extensions */
    uint8_t found;                       /* 1 a chord, 0 the bass's name only (or nothing held: all empty) */
} sa_chord_t;

static char *sa_cpy(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static uint16_t sa_rot(uint16_t pcs, uint32_t root)     /* the pitch classes as intervals above root */
{
    uint32_t i;
    uint16_t m = 0;
    for (i = 0; i < 12u; i++)
        if (pcs >> i & 1u)
            m |= (uint16_t)(1u << ((i + 12u - root) % 12u));
    return m;
}

static int sa_match(uint16_t iv)                         /* the chord these intervals are, -1 none */
{
    uint32_t k, n = 0, i;
    for (i = 0; i < 12u; i++)
        n += iv >> i & 1u;
    for (k = 0; k < SA_NCHORDS; k++)
        if (iv == SA_CHORDS[k].set)
            return (int)k;
    for (k = 0; n >= 3u && k < SA_NCHORDS; k++) {        /* a four-note chord or more without its fifth */
        uint16_t s = SA_CHORDS[k].set;
        if ((s & SA_I(7)) && iv == (s & ~SA_I(7)) && (SA_CHORDS[k].q == SAQ_MAJ || SA_CHORDS[k].q == SAQ_MIN) &&
            SA_CHORDS[k].ext[0])
            return (int)k;
    }
    return -1;
}

/* the chord of the held notes (MIDI numbers, any order, n of them) */
static void sa_chord_name(const uint8_t *notes, uint32_t n, sa_chord_t *c)
{
    uint32_t i, bass = 128u, best_root = 0, r;
    uint16_t pcs = 0;
    int best = -1;
    c->main[0] = c->ext[0] = c->bass[0] = 0;
    c->core = 0;
    c->found = 0;
    for (i = 0; i < n; i++) {
        pcs |= (uint16_t)(1u << (notes[i] % 12u));
        if (notes[i] < bass)
            bass = notes[i];
    }
    if (!n)
        return;
    bass %= 12u;
    best = sa_match(sa_rot(pcs, bass));                  /* the bass as the root first */
    best_root = bass;
    for (r = 0; best < 0 && r < 12u; r++) {             /* then the others: the first chord of the table wins */
        int k;
        if (r == bass || !(pcs >> r & 1u))
            continue;
        k = sa_match(sa_rot(pcs, r));
        if (k >= 0)
            best = k, best_root = r;
    }
    if (best < 0 || (pcs == (1u << bass))) {
        sa_cpy(c->main, SA_PC_NAME[bass]);
        c->core = pcs;
        return;
    }
    c->found = 1;
    sa_cpy(sa_cpy(c->main, SA_PC_NAME[best_root]), SA_QNAME[SA_CHORDS[best].q]);
    sa_cpy(c->ext, SA_CHORDS[best].ext);
    if (best_root != bass)
        sa_cpy(sa_cpy(c->bass, "/"), SA_PC_NAME[bass]);
    for (i = 0; i < 12u; i++)
        if (SA_CORE[SA_CHORDS[best].q] >> i & 1u)
            c->core |= (uint16_t)(1u << ((best_root + i) % 12u));
    c->core &= pcs;
}

/* a note's name with its octave (MIDI 60 = C4, as the panel's F3..G5): "C#4" */
static void sa_note_name(uint32_t note, char *s)
{
    int32_t o = (int32_t)(note / 12u) - 1;
    s = sa_cpy(s, SA_PC_NAME[note % 12u]);
    if (o < 0) {
        *s++ = '-';
        o = -o;
    }
    *s++ = (char)('0' + o % 10);
    *s = 0;
}
