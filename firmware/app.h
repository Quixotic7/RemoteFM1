/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1 on fm1-base: the app's half of the contract with the platform (fm1-base device/unity.c on the FM-1,
 * host/plat_host.c in the host simulator). The app is app.c and src/; the protocol is docs/PROTOCOL.md. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "REMOTE FM1"
#define APP_SPLASH_RGB RGB(120, 200, 255)
#define APP_TAGLINE "DX7 VOICES AND A HOST'S PANEL"
#define APP_PALETTE 2u                   /* gfx.c's CYAN */
#define APP_BLOCK 256u                   /* the standalone DX7 (sa/sa_main.c sa_render): 8 of FM6's 32-frame blocks */
#define APP_CANVAS_PX (240u * 32u)       /* the standalone screens' two 16-row bands (sa/sa_gfx.c) */
#define FM1B_TRS 0                       /* no MIDI in on the TRS jack (yet): the standalone plays USB and Bluetooth MIDI */
#define APP_TOUCH                        /* the keys' velocity (fm1-base lib/touch): standalone only (sa_main.c) */

/* RFM_SHADOW 1: the 240 x 240 shadow frame buffer (115,200 B of the pool): TILES are decoded into it and pushed to
 * the panel at each END, COPY works, the panel is repainted from it after the update screen. 0 (a build short of
 * RAM: the WiFi one): each rectangle goes to the panel as it is decoded, no COPY; HELLO_R's caps say which */
#ifndef RFM_SHADOW
#define RFM_SHADOW 1
#endif

/* the SysEx send ring (felucca-bsp usb.c, in packets of 3 bytes: RAM): RemoteFM1's replies go in whole */
#ifndef SXQ
#define SXQ 1024u
#endif
/* Bluetooth MIDI's queues (fm1-base device/radio.c, in packets): SysEx comes and goes as packets of three bytes */
#define BLE_IN_Q 1024u
#define BLE_OUT_Q 128u

/* the store (src/rfm_store.c): RemoteFM1 writes only in 0xF2000..0xF3FFF, the one window that ChoralRoot, the stock
 * firmware and fm1-base's other firmwares leave free (ChoralRoot's map: its loop slots end at 0xE5FFF, its sound pool
 * fills 0xE6000..0xE9FFF and 0xEB000..0xF1FFF; the stock resources start at 0xF4000; FIZZ keeps its stores in
 * 0x97000..0xDFFFF and its Bluetooth at 0xE5000), and in the panel's calibration at 0xFC000 (the platform's, written
 * only by OCT- + OCT+ at power-on), so hopping between firmwares loses nobody's data. APP_STORE_OK keeps fm1-base's
 * store to exactly those (the device's st_erase / st_prog; the simulator refuses a save outside them). The two
 * sectors are one object, OBJ_RF (an A/B pair: cut-safe), and the objects RemoteFM1 keeps are records in it
 * (APP_STORE_MUX: OBJ_BLE, fm1-base's Bluetooth address and bondings, and OBJ_SA, the standalone's settings; they
 * have no sectors of their own). Numbers: OBJ_PANEL 0 and OBJ_BLE 1 as in 0.2 (whose OBJ_BLE at 0xF2000 was never
 * written: outside felucca-bsp's FL_STORE_OK, every save refused); OBJ_RF's record header says 2, so 0.2 reads none. */
enum { OBJ_PANEL, OBJ_BLE, OBJ_RF, OBJ_SA, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0u, 0xF2000u, 0u}
#define APP_STORE_OK(off, n) (plat_in((off), (n), 0xF2000u, 0xF4000u) || plat_in((off), (n), 0xFC000u, 0xFE000u))
#define APP_STORE_MUX
#define APP_STORE_VIRTUAL(obj) ((obj) == OBJ_BLE || (obj) == OBJ_SA)   /* records in OBJ_RF */
int plat_store_raw_load(uint32_t obj, void *dst, uint32_t max);   /* (fm1-base's, under APP_STORE_MUX) */
int plat_store_raw_head(uint32_t obj, void *dst, uint32_t n);
int plat_store_raw_size(uint32_t obj);
int plat_store_raw_save(uint32_t obj, const void *src, uint32_t len);

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#ifndef PLAT_HOST
/* felucca-bsp's usb.c with RemoteFM1's vendor interface and its hooks (firmware/patches/felucca-bsp): the bulk pair
 * on EP3, its rings in the pool; every SysEx byte from USB to src/rfm_main.c's tap (RemoteFM1's messages only, so the
 * updater's frame slot is never held by one); EP1 held while RemoteFM1's ring is full; the frame-number check and the
 * audio input's stall check by the clock (the main loop polls USB far more often than 2 kHz); macOS's odd CIN 0xF
 * byte inside a long SysEx taken */
#define FELUCCA_VENDOR 1
#define FELUCCA_VENDOR_IN_N 32768u
#define FELUCCA_VENDOR_OUT_N 32768u
#define FELUCCA_VENDOR_RING_ATTR PLAT_POOL
static int rf_sx_tap(uint32_t b);
static int rf_ep1_room(void);
static int rf_frame_due(void);
static uint32_t rf_usb_ms(void);
static uint32_t rf_cinf;
#define FELUCCA_SYSEX_TAP(b) rf_sx_tap(b)
#define FELUCCA_EP1_ROOM() rf_ep1_room()
#define FELUCCA_USB_FRAME_DUE() rf_frame_due()
#define FELUCCA_USB_MS() rf_usb_ms()
#define FELUCCA_CIN_F_SYSEX (rf_cinf++)
#else
/* the host simulator's checks and commands (src/rfm_host.c) */
#define APP_HOST_EXPECT
#define APP_HOST_CMD
#define APP_HOST_FAILS
int app_expect(const char *what, int *got);
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
int app_host_fails(void);
#endif
