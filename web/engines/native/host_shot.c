/* SPDX-License-Identifier: GPL-3.0-only */
/* An author's build's reference picture (build.sh --ext ... --host): the same core, built with the host's C compiler
 * (the firmware's units, the glue, wasm_glue.c), powered on from an erased flash and run millisecond by millisecond
 * until its screen is not blank (at most boot_ms), as test.mjs runs the WebAssembly module; that first picture is
 * written as a P6 PPM (8-bit RGB, FM1VST's ppm.h conversion), which test.mjs compares with the module's, pixel for
 * pixel.
 *   host_shot OUT.ppm [boot_ms]  */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fm1core.h"

int fmw_open(void);
void fmw_boot(const uint8_t *bytes, uint32_t n);
void fmw_run_ms(uint32_t n);
uint32_t fmw_ms(void);
uint16_t *fmw_lcd(void);

static int blank(const uint16_t *px)
{
    for (int i = 1; i < FM1P_LCD_W * FM1P_LCD_H; i++)
        if (px[i] != px[0])
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "host_shot OUT.ppm [boot_ms]\n");
        return 2;
    }
    const uint32_t boot_ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 3000u;
    if (!fmw_open()) {
        fprintf(stderr, "host_shot: no ABI 6 core\n");
        return 1;
    }
    fmw_boot(NULL, 0);
    const uint16_t *px = fmw_lcd();
    while (blank(px) && fmw_ms() < boot_ms) {
        fmw_run_ms(1);
        px = fmw_lcd();
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", FM1P_LCD_W, FM1P_LCD_H);
    for (int i = 0; i < FM1P_LCD_W * FM1P_LCD_H; i++) {
        const uint16_t c = px[i];
        const uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                                (uint8_t)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("host_shot: %s at %u ms%s\n", argv[1], fmw_ms(), blank(px) ? " (blank)" : "");
    return 0;
}
