/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The render hash on the computer (firmware/sa/sa_hash.h): what the FM-1 must answer to SA_BENCH 02.
 *   cc -O2 -ffp-contract=off -DPLAT_HOST -DSA_POLY=N -I GEN -I firmware/sa firmware/tools/sa_hash_host.c \
 *      firmware/sa/sa_synth.c -lm        (GEN: gen_fm6_tables.py's fm6_tables.h; firmware/tools/sa_bench.py does it)
 *   sa_hash_host [CORES]                 prints "SA_POLY hash blocks": one core, or the batch over two (1, 2) */
#include <stdio.h>
#include <stdlib.h>
#include "sa_synth.h"
#include "sa_hash.h"

static int w_now(void (*fn)(uint32_t), uint32_t arg) { fn(arg); return 1; }
static int w_wait(void) { return 0; }

int main(int argc, char **argv)
{
    uint32_t b, h = SA_HASH_SEED;
    if (argc > 1 && atoi(argv[1]) == 2) {
        sa_synth_cores(2);
        sa_synth_worker(w_now, w_wait);
    }
    sa_synth_init();
    for (b = 0; b < SA_HASH_BLOCKS; b++)
        h = sa_hash_block(b, h);
    printf("%d %08x %u\n", SA_POLY, h, (unsigned)SA_HASH_BLOCKS);
    return 0;
}
