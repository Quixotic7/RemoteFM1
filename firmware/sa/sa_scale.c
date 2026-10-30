/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The keys' layout (the 27 keys from F: which are white, which black) and the scale (GLO + PRESETS the root, GLO +
 * ALGORITHM the scale). Part of sa_main.c (its state, sa). */

/* The scales: CHROMATIC (off) and fourteen; bit i: i semitones over the root. SNAP (the owner, 2026-10-30): every key
 * plays, a note out of the scale moves to the nearest one in it (a tie: down); the layout stays. The keys' notes (and
 * so the arp's and what a loop records) in the instrument, and MIDI REMOTE's in MIDI KEYS. */
#define SA_NSCALES 15u
static const struct {
    const char *name;
    uint16_t mask;
} SA_SCALES[SA_NSCALES] = {{"CHROMATIC", 0xFFFu},  {"MAJOR", 0xAB5u},      {"MINOR", 0x5ADu},     {"DORIAN", 0x6ADu},
                           {"PHRYGIAN", 0x5ABu},   {"LYDIAN", 0xAD5u},     {"MIXOLYDIAN", 0x6B5u}, {"LOCRIAN", 0x56Bu},
                           {"HARM MINOR", 0x9ADu}, {"MEL MINOR", 0xAADu},  {"MAJ PENTA", 0x295u}, {"MIN PENTA", 0x4A9u},
                           {"BLUES", 0x4E9u},      {"WHOLE TONE", 0x555u}, {"DIMINISHED", 0xB6Du}};
static const char *const SA_ROOTS[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static int32_t sa_key_white(uint32_t k)      /* key k's place among the 16 white keys (0..15), -1 a black one */
{
    static const int8_t W[12] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6};   /* F F# G G# A A# B C C# D D# E */
    int32_t w = k < NKEYS ? W[k % 12u] : -1;
    return w < 0 ? -1 : (int32_t)(k / 12u) * 7 + w;
}

static int32_t sa_key_black(uint32_t k)      /* key k's place among the 11 black keys (0..10), -1 a white one */
{
    static const int8_t B[12] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1};
    int32_t b = k < NKEYS ? B[k % 12u] : -1;
    return b < 0 ? -1 : (int32_t)(k / 12u) * 5 + b;
}

static uint32_t sa_white_bit(uint32_t w)    /* the white key w's bit (0..15) among the 27 */
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        if (sa_key_white(k) == (int32_t)w)
            return 1u << k;
    return 0;
}

static uint32_t sa_black_bit(uint32_t b)     /* the black key b's (0..10) */
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        if (sa_key_black(k) == (int32_t)b)
            return 1u << k;
    return 0;
}

/* GLO held (the owner, 2026-10-30: usable with the screen off): the channel's white key (1..16 left to right) and the
 * octave's black key (the one GLO + it picks: the middle one 0; -3 / +3 the third / ninth) lit */
static uint32_t sa_glo_keys(uint32_t ch, int32_t oct)
{
    return sa_white_bit(ch - 1u) | sa_black_bit((uint32_t)clamp(oct + 5, 0, 10));
}

/* GLO + a black key: the octave, the middle black key (the sixth, F#4 at octave 0) 0, -1 / +1 a key left / right,
 * -3 / +3 at the ends (the first three, the last three) */
static int32_t sa_black_oct(uint32_t b) { return clamp((int32_t)b - 5, -3, 3); }

static int sa_in_scale(uint32_t n)           /* note n in the scale */
{
    uint32_t pc = (n + 12u * 11u - sa.scale_root) % 12u;
    return SA_SCALES[sa.scale < SA_NSCALES ? sa.scale : 0u].mask >> pc & 1u;
}

static uint32_t sa_scale_keys(int32_t oct)   /* the keys whose notes are in the scale (none when chromatic) */
{
    uint32_t k, m = 0;
    for (k = 0; sa.scale && k < NKEYS; k++) {
        int32_t n = (int32_t)SA_NOTE0 + (int32_t)k + 12 * oct;
        if (n >= 0 && n < 128 && sa_in_scale((uint32_t)n))
            m |= 1u << k;
    }
    return m;
}

static uint32_t sa_root_keys(int32_t oct)    /* the keys playing the scale's root (none when chromatic) */
{
    uint32_t k, m = 0;
    for (k = 0; sa.scale && k < NKEYS; k++) {
        int32_t n = (int32_t)SA_NOTE0 + (int32_t)k + 12 * oct;
        if (n >= 0 && n < 128 && ((uint32_t)n + 12u * 11u - sa.scale_root) % 12u == 0u)
            m |= 1u << k;
    }
    return m;
}

static uint32_t sa_snap(uint32_t n)          /* note n, onto the scale: the nearest note in it, a tie down */
{
    int32_t d;
    if (sa.scale == 0u || sa_in_scale(n))
        return n;
    for (d = 1; d <= 6; d++) {
        if ((int32_t)n - d >= 0 && sa_in_scale(n - (uint32_t)d))
            return n - (uint32_t)d;
        if (n + (uint32_t)d <= 127u && sa_in_scale(n + (uint32_t)d))
            return n + (uint32_t)d;
    }
    return n;
}

static void sa_scale_turn(uint32_t enc, int32_t d)   /* GLO + PRESETS: the root, round; GLO + ALGORITHM: the scale */
{
    if (enc == EN_PRESET)
        sa.scale_root = (uint8_t)(((int32_t)sa.scale_root + d % 12 + 12) % 12);
    else
        sa.scale = (uint8_t)(((int32_t)sa.scale + d % (int32_t)SA_NSCALES + (int32_t)SA_NSCALES) % (int32_t)SA_NSCALES);
    sa_changed();
}

/* MIDI REMOTE in MIDI KEYS, GLO held: PRESETS / ALGORITHM the scale (SCALES on in the settings): 1 taken */
static void sa_rm_info(uint32_t what);
static int sa_rm_glo_enc(uint32_t k, int32_t d)
{
    if (!sa.rm_scl || (k != EN_PRESET && k != EN_ALGO))
        return 0;
    sa_scale_turn(k, d);
    sa_rm_info(3u);                          /* (SA_RM_INFO_SCALE) */
    return 1;
}
