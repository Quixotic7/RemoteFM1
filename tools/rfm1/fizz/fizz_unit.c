/* SPDX-License-Identifier: GPL-3.0-only */
/* FIZZ's firmware unit for the .rfm1 (fm1_add_core UNIT): FIZZ's own glue, core/glue/core_quadrille.c, unchanged, plus
 * the one thing a FIZZ unit answers that the glue leaves to the device: the M-UPGRADE identity. FIZZ's web manager
 * finds the unit by it first (site/manager/fizzlink.js findFizz: fm1lib's Updater, the handshake
 * F0 00 32 45 00 00 00 40 7F F7) and only then talks FIZZ's own SysEx (7D 46 5A). On the device fm1-base's main loop
 * runs felucca-bsp's chip/ota.c ota_service() (device/main_fm1.c), which answers with the package identity
 * (FELUCCA_ID, fm1-base tools/build.py --release: fm1.toml's id_prefix FM-1_46 + XXYYZZ, FM-1_46001200 for 0.12.0).
 * Here the same ota.c, at the commit FIZZ's fm1-base pins, through FM1VST's common/sysex_ota.h, on the glue's own
 * SysEx transport: the handshake frame is accepted beside FIZZ's 7D 46 5A ones and answered, nothing else of ota.c
 * runs (the update itself is never started: its flash hooks refuse). FIZZ's other frames in that framing (its store's
 * backup: FZBI, FZBR, FZSR; firmware/app/app.c app_usb_cmd) are not part of the core and get no answer.
 * build_fizz.sh writes fizz_package_id.h (FELUCCA_ID, checked against FIZZ's releases.json) and puts felucca-bsp's
 * chip/ on the include path. GPL-3.0-only: it compiles FM1VST's sysex_ota.h (GPL-3.0-only) with FIZZ's glue and
 * felucca-bsp's ota.c (both MPL-2.0, unchanged), as the .rfm1's licence expression already says. */
#include "sysex.h"
#include "fizz_package_id.h"

static void fizz_rfm1_in_init(fm1sx_in_t *s, fm1sx_accept_fn accept);
static int fizz_rfm1_frame(fm1sx_in_t *s, const uint8_t **p, uint32_t *n);
#define fm1sx_in_init(s, a) fizz_rfm1_in_init((s), (a))
#define fm1sx_frame(s, p, n) fizz_rfm1_frame((s), (p), (n))
#include "core_quadrille.c"
#undef fm1sx_in_init
#undef fm1sx_frame

#define FM1SX_OTA_C "ota.c"                       /* felucca-bsp chip/ota.c */
#define FM1SX_OTA_IN fm1core_qd_sx_in
#define FM1SX_OTA_OUT fm1core_qd_sx_out
#define FM1SX_OTA_MS() (fm1core_qd_app.ms)
#include "sysex_ota.h"

static fm1sx_accept_fn fizz_rfm1_accept_fizz;
static int fizz_rfm1_accept(const uint8_t *f, uint32_t n) { return fm1sx_is_mupg(f, n) || fizz_rfm1_accept_fizz(f, n); }
static void fizz_rfm1_in_init(fm1sx_in_t *s, fm1sx_accept_fn accept)
{
    fizz_rfm1_accept_fizz = accept;
    fm1sx_in_init(s, fizz_rfm1_accept);
}
/* the waiting frame for FIZZ's app, or 0: an M-UPGRADE frame is ota.c's (the handshake answered, the frame freed) */
static int fizz_rfm1_frame(fm1sx_in_t *s, const uint8_t **p, uint32_t *n)
{
    if (!fm1sx_frame(s, p, n))
        return 0;
    if (!fm1sx_is_mupg(*p, *n))
        return 1;
    (void)ota_service();
    return 0;
}
