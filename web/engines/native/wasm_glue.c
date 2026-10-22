/* SPDX-License-Identifier: GPL-3.0-only */
/* The native engine's WebAssembly face of one core (core-api/fm1core.h): linked by build.sh with the core's static
 * library into web/build/native/<id>.wasm, a standalone module (no Emscripten JavaScript) that engine.js drives.
 *
 * The clock is FM1VST's engine/device.cpp run_ms, unchanged: tick(ms) once per device millisecond, frame() at ms 0
 * and every 15 ms or idle() otherwise, then audio() for each 128-frame block due at 44100 Hz. fmw_block() runs the
 * device until the next block exists and returns it (int16 stereo, 44.1 kHz): engine.js resamples to the context
 * rate. The input reaches the panel as Device::keys / buttons / enc / master write it.
 * The same file is compiled with the host's compiler for an author's build's reference picture (host_shot.c). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fm1core.h"

#ifdef __wasm__
#define FMW_EXPORT(name) __attribute__((used, export_name(#name)))
#else
#define FMW_EXPORT(name)               /* (the host build: host_shot.c calls these directly) */
#endif

static const fm1core_t *C;
static uint32_t dev_ms, last_frame;
static uint64_t frames_done;
static int16_t blk[FM1P_BLOCK * 2];
static int have_blk;
static uint16_t lcd_native[FM1P_LCD_W * FM1P_LCD_H];
static uint32_t lcd_seen = 0xFFFFFFFFu;

FMW_EXPORT(fmw_malloc) void *fmw_malloc(uint32_t n) { return malloc(n ? n : 1); }
FMW_EXPORT(fmw_free) void fmw_free(void *p) { free(p); }

/* the descriptor: 1 found (ABI 6), 0 not */
FMW_EXPORT(fmw_open) int fmw_open(void)
{
    C = fm1core_get(FM1CORE_ABI);
    return C != NULL && C->panel != NULL;
}

/* power-on from n bytes of flash image (NULL / 0: a fresh, erased flash): Device::boot_from */
FMW_EXPORT(fmw_boot) void fmw_boot(const uint8_t *bytes, uint32_t n)
{
    C->flash_stage(bytes, bytes ? n : 0u);
    C->options(FM1CORE_FLASH_STAGED, 0, 0, 1);
    if (C->boot_options)
        C->boot_options(-1, NULL, -1);
    C->panel->ready = 2;
    C->init(0);
    dev_ms = last_frame = 0;
    frames_done = 0;
}

/* one device millisecond (Device::run_ms); a block that fell due goes to blk */
static void run_ms(void)
{
    const uint32_t ms = dev_ms++;
    const int frame = ms == 0 || ms - last_frame >= 15u;
    C->tick(ms);
    if (frame) {
        last_frame = ms;
        C->frame();
    } else {
        C->idle();
    }
    while (frames_done + FM1P_BLOCK <= (uint64_t)ms * FM1P_FS / 1000u) {
        C->audio(blk, FM1P_BLOCK);          /* (at most one a ms: 44.1 frames < 128) */
        frames_done += FM1P_BLOCK;
        have_blk = 1;
    }
}

/* the next 128-frame block, int16 interleaved stereo at 44.1 kHz */
FMW_EXPORT(fmw_block) int16_t *fmw_block(void)
{
    have_blk = 0;
    while (!have_blk)
        run_ms();
    return blk;
}
FMW_EXPORT(fmw_ms) uint32_t fmw_ms(void) { return dev_ms; }
/* (tests) n device milliseconds, their audio dropped: the conformance test's run_ms loop */
FMW_EXPORT(fmw_run_ms) void fmw_run_ms(uint32_t n)
{
    while (n--)
        run_ms();
}

/* ---- input (engine/device.cpp) ---- */
FMW_EXPORT(fmw_input) void fmw_input(uint32_t keys, uint32_t buttons, uint32_t keys_tap, uint32_t buttons_tap,
                                     int32_t master)
{
    fm1panel_t *h = C->panel;
    keys &= (1u << FM1P_NKEY) - 1u;
    buttons &= (1u << FM1P_NBTN) - 1u;
    h->keys_tap |= (keys & ~h->keys) | (keys_tap & ((1u << FM1P_NKEY) - 1u));
    h->keys = keys;
    h->buttons_tap |= (buttons & ~h->buttons) | (buttons_tap & ((1u << FM1P_NBTN) - 1u));
    h->buttons = buttons;
    if (master >= 0)
        h->master = master > 1023 ? 1023 : master;
}
FMW_EXPORT(fmw_enc) void fmw_enc(uint32_t i, int32_t detents)
{
    if (i < FM1P_NENC)
        C->panel->enc[i] += detents;
}
FMW_EXPORT(fmw_key_vel) void fmw_key_vel(uint32_t k, uint32_t vel, uint32_t lean)
{
    if (k < FM1P_NKEY) {
        C->panel->key_vel[k] = (uint8_t)(vel > 127u ? 127u : vel);
        C->panel->key_lean[k] = (uint8_t)(lean > 100u ? 100u : lean);
    }
}
FMW_EXPORT(fmw_midi_in) int fmw_midi_in(uint32_t pkt) { return C->midi_in ? C->midi_in(pkt) : 1; }
/* one USB-MIDI packet the firmware sent into *pkt: 1, or 0 none */
FMW_EXPORT(fmw_midi_out) int fmw_midi_out(uint32_t *pkt) { return C->midi_out_take ? C->midi_out_take(pkt) : 0; }

/* ---- output ---- */
FMW_EXPORT(fmw_lcd_writes) uint32_t fmw_lcd_writes(void) { return C->panel->lcd_writes; }
/* the LCD as native-endian RGB565 (the panel's RAM is big-endian as sent: swapped when it changed) */
FMW_EXPORT(fmw_lcd) uint16_t *fmw_lcd(void)
{
    const fm1panel_t *h = C->panel;
    if (h->lcd_writes != lcd_seen) {
        uint32_t i;
        lcd_seen = h->lcd_writes;
        for (i = 0; i < FM1P_LCD_W * FM1P_LCD_H; i++)
            lcd_native[i] = (uint16_t)((h->lcd[i] >> 8) | (h->lcd[i] << 8));
    }
    return lcd_native;
}
FMW_EXPORT(fmw_leds) uint8_t *fmw_leds(void) { return &C->panel->led[0][0]; }   /* [col][line], 44 bytes */
FMW_EXPORT(fmw_play_green) uint32_t fmw_play_green(void) { return C->panel->led_play_green; }
FMW_EXPORT(fmw_halted) int fmw_halted(void) { return C->halted ? C->halted() : 0; }

/* ---- the flash image ---- */
FMW_EXPORT(fmw_flash) uint8_t *fmw_flash(void) { return C->flash(); }
FMW_EXPORT(fmw_flash_size) uint32_t fmw_flash_size(void) { return C->flash_size; }
FMW_EXPORT(fmw_flash_dirty) uint32_t fmw_flash_dirty(void) { return C->flash_dirty(); }
FMW_EXPORT(fmw_flash_sync) int fmw_flash_sync(void) { return C->flash_sync ? C->flash_sync() : 1; }
FMW_EXPORT(fmw_shutdown) void fmw_shutdown(void)
{
    if (C->shutdown)
        C->shutdown();
}

/* ---- the descriptor's strings and tables ---- */
FMW_EXPORT(fmw_str) const char *fmw_str(uint32_t k)
{
    switch (k) {
    case 0: return C->id;
    case 1: return C->name;
    case 2: return C->version;
    case 3: return C->source_url;
    case 4: return C->family;
    case 5: return C->license;
    case 6: return fm1core_credits(C);
    case 7: return C->gated;
    default: return k >= 100u && k < 100u + FM1P_NB ? C->button_names[k - 100u] : NULL;
    }
}
FMW_EXPORT(fmw_keymap) const int8_t *fmw_keymap(void) { return C->keymap; }   /* [6][11] or NULL */
/* the panel table after init: btn_id[14] (label -> matrix button), enc_id[7] (role -> matrix encoder), enc_dir[7] */
FMW_EXPORT(fmw_btn_id) const uint8_t *fmw_btn_id(void) { return C->panel->btn_id; }
FMW_EXPORT(fmw_enc_id) const uint8_t *fmw_enc_id(void) { return C->panel->enc_id; }
FMW_EXPORT(fmw_enc_dir) const int8_t *fmw_enc_dir(void) { return C->panel->enc_dir; }
FMW_EXPORT(fmw_ui_info) const char *fmw_ui_info(void)
{
    static char b[256];
    b[0] = 0;
    if (C->ui_info)
        C->ui_info(b, sizeof b);
    return b;
}
