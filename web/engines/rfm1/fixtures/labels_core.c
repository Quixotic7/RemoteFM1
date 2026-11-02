// SPDX-License-Identifier: GPL-3.0-only
// A tiny core with live panel labels (FM1VST docs/DYNAMIC-LABELS.md) for engines/rfm1/labels-test.mjs: the fm1core
// ABI, a blank screen, silence, and FM1VST's core-glue/common/rfm1_shim.c linked in by build_labels.sh (its
// rfm1_labels_epoch / rfm1_label call fm1core_labels_epoch / fm1core_label below through weak references).
//   GLO up:   FX "DRUMS", SEL "" (no job), KNOB 1 "CUTOFF", W1 "STEP 1", B1 "MUTE"; the rest not given
//   GLO held: FX "UNDO", SEL "COPY", GLO "SHIFT", KNOB 1 "RES", W1 "" ; the rest not given
// The epoch: 0 before init, 1 after it, + 1 on every change. -DNOLABELS: the same core without the two symbols
// (the shim's exports then give epoch 0).
#include <stdint.h>
#include <string.h>

#include "fm1core.h"

static fm1panel_t panel;
static uint8_t flash[4096];
static uint32_t flash_seq;

static void options(const char *p, int nf, int s, int h) { (void)p; (void)nf; (void)s; (void)h; }
static int flash_stage(const uint8_t *b, uint32_t n)
{
    memset(flash, 0xFF, sizeof flash);
    if (b)
        memcpy(flash, b, n < sizeof flash ? n : sizeof flash);
    return 1;
}
static uint8_t *flash_img(void) { return flash; }
static uint32_t flash_dirty(void) { return flash_seq; }
static void audio(int16_t *out, uint32_t frames) { memset(out, 0, frames * 4u); }
static void idle(void) {}
static void tick(uint32_t ms) { (void)ms; }

#ifndef NOLABELS
static uint32_t epoch;
static int shift;
#endif

static void init(int demo)
{
    (void)demo;
    for (int b = 0; b < FM1P_NB; b++)
        panel.btn_id[b] = (uint8_t)(b < 12 ? b + 2 : b - 12);
    for (int e = 0; e < FM1P_NE - 1; e++) {
        panel.enc_id[e] = (uint8_t)e;
        panel.enc_dir[e] = 1;
    }
#ifndef NOLABELS
    epoch = 1;
#endif
}

static void frame(void)
{
#ifndef NOLABELS
    const int s = (panel.buttons >> panel.btn_id[FM1P_B_GLO]) & 1u;
    if (s != shift) {
        shift = s;
        epoch++;
    }
#endif
}

static const fm1core_t core = {
    .abi_version = FM1CORE_ABI,
    .id = "labeltest",
    .name = "Label Test",
    .version = "1.0",
    .source_url = "https://github.com/Quixotic7/RemoteFM1@0000000",
    .family = "custom",
    .license = "GPL-3.0-only",
    .flash_size = sizeof flash,
    .button_names = { "KICK", "TRK" },
    .panel = &panel,
    .options = options,
    .init = init,
    .tick = tick,
    .frame = frame,
    .idle = idle,
    .audio = audio,
    .flash_stage = flash_stage,
    .flash = flash_img,
    .flash_dirty = flash_dirty,
};

FM1CORE_EXPORT const fm1core_t *fm1core_get(uint32_t abi) { return abi == FM1CORE_ABI ? &core : NULL; }

#ifndef NOLABELS
FM1CORE_EXPORT uint32_t fm1core_labels_epoch(void) { return epoch; }
FM1CORE_EXPORT const char *fm1core_label(int32_t which)
{
    switch (which) {
    case FM1CORE_LBL_BTN0 + FM1P_B_FX: return shift ? "UNDO" : "DRUMS";
    case FM1CORE_LBL_BTN0 + FM1P_B_SEL: return shift ? "COPY" : "";
    case FM1CORE_LBL_BTN0 + FM1P_B_GLO: return shift ? "SHIFT" : NULL;
    case FM1CORE_LBL_KNOB0 + FM1P_E_K1: return shift ? "RES" : "CUTOFF";
    case FM1CORE_LBL_WHITE0: return shift ? "" : "STEP 1";
    case FM1CORE_LBL_BLACK0: return shift ? NULL : "MUTE";
    default: return NULL;
    }
}
#endif
