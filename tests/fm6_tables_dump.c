/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* Every value of the FM6 tables a header holds, one per line (floats as their bits), for tests/fm6_parity.sh to
 * compare ours (firmware/tools/gen_fm6_tables.py) with saadet's own generator's: the header comes in with -include,
 * TABLES_INIT names its start-up fill (ours fm6_tables_init, saadet's dsp_tables_init: FM6_SIN). */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define DUMP(name, fmt, cast)                                                                                       \
    for (i = 0; i < sizeof name / sizeof name[0]; i++)                                                              \
        printf(#name "[%u] " fmt "\n", i, (cast)name[i]);

int main(void)
{
    unsigned i;
    TABLES_INIT();
    printf("FM6_N %d\nFM6_LG_N %d\nFM6_LFO_UNIT %d\nFM6_PEG_UNIT %d\n", FM6_N, FM6_LG_N, FM6_LFO_UNIT, FM6_PEG_UNIT);
    DUMP(FM6_FREQ, "%" PRId32, int32_t)
    DUMP(FM6_COARSE, "%" PRId32, int32_t)
    DUMP(FM6_FINE, "%" PRId32, int32_t)
    DUMP(FM6_DETUNE, "%" PRId64, int64_t)
    DUMP(FM6_LFO_INC, "%" PRIu32, uint32_t)
    DUMP(FM6_PORTA, "%" PRId32, int32_t)
    DUMP(FM6_GLISS, "%" PRId32, int32_t)
    for (i = 0; i < sizeof FM6_SIN / sizeof FM6_SIN[0]; i++) {
        uint32_t bits;
        memcpy(&bits, &FM6_SIN[i], 4);
        printf("FM6_SIN[%u] %08" PRIx32 "\n", i, bits);
    }
    return 0;
}
