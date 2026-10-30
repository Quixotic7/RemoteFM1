/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The standalone instrument's voices: Yamaha's eight DX7 ROM cartridges, ROM1A..ROM4B, 32 voices each, packed as the
 * DX7 stores them (VMEM, 128 bytes), compiled in from build/gen/dx7_rom.h (firmware/tools/gen_dx7_rom.py reads the
 * owner's .syx files at build time; neither those nor the header are ever committed: the data is Yamaha's). A build
 * without them has one "bank" of one voice, the DX7's INIT VOICE. The user banks (DX7 SysEx in) come in S5. */
#include "dx7_rom.h"                     /* build/gen */

#if SA_ROM_NBANKS
#define SA_NBANKS SA_ROM_NBANKS
#define SA_BANK_VOICES 32u
#else
#define SA_NBANKS 1u
#define SA_BANK_VOICES 1u
#endif

static const uint8_t *sa_bank_vmem(uint32_t bank, uint32_t voice)   /* bank 0.., voice 0..; clamped */
{
#if SA_ROM_NBANKS
    return SA_ROM[bank < SA_NBANKS ? bank : 0u][voice < 32u ? voice : 0u];
#else
    (void)bank;
    (void)voice;
    return sa_synth_init_vmem();
#endif
}

static const char *sa_bank_name(uint32_t bank)   /* "ROM1A"; "INIT" without the cartridges */
{
#if SA_ROM_NBANKS
    return SA_ROM_BANK[bank < SA_NBANKS ? bank : 0u];
#else
    (void)bank;
    return "INIT";
#endif
}

/* a voice's name from its data (bytes 118..127), its runs of spaces made one and the ends trimmed: "E.PIANO 1" */
static void sa_voice_name(const uint8_t *vmem, char *out)
{
    uint32_t i, n = 0;
    for (i = 0; i < 10u; i++) {
        char c = (char)(vmem[118u + i] & 0x7Fu);
        if (c < 32 || c > 126)
            c = ' ';
        if (c == ' ' && (!n || out[n - 1u] == ' '))
            continue;
        out[n++] = c;
    }
    while (n && out[n - 1u] == ' ')
        n--;
    out[n] = 0;
}
