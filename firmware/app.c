/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* RemoteFM1: an FM-1 that is only a panel for a firmware running on a computer (docs/PROTOCOL.md). Part of fm1-base's
 * unity build (device/unity.c on the FM-1, host/plat_host.c in the simulator), after the platform: gfx.c, the panel,
 * MIDI and the store are in scope, and on the FM-1 felucca-bsp's usb.c with the vendor interface (app.h).
 *
 *   src/remote_proto.h   the framing (7-in-8 SysEx, the bulk header) and the stream parsers, M0's
 *   src/rfm_draw.c       the TILES decoder, into the shadow (or straight to the panel: RFM_SHADOW 0)
 *   src/rfm_push.c       the push of the shadow's dirty tiles, a step at a time, never waiting for the panel
 *   src/rfm_app.c        the protocol layer: session, HELLO_R, LEDS, TILES, FRAME_ACK, PANEL_R, the panel as MIDI
 *   src/rfm_main.c       the app: its transports (USB SysEx, USB bulk, Bluetooth SysEx), the LEDs, the panel, the
 *                        waiting screen, M0's diagnostics, the contract with the platform
 *   src/rfm_dev.c        what only the FM-1 has (usb.c's rings, the panel's DMA), or
 *   src/rfm_host.c       the host simulator's stand-ins for those, and its commands and checks (tests/) */
#include "remote_proto.h"
#include "rfm_draw.c"
#include "rfm_push.c"
#include "rfm_main.c"
