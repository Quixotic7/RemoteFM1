/* SPDX-License-Identifier: GPL-3.0-only */
/* FIZZ's firmware unit for the .rfm1 (fm1_add_core UNIT): FIZZ's own glue, core/glue/core_quadrille.c, unchanged, plus
 * two things the glue leaves to the device: the M-UPGRADE identity (below) and, built with cpu2 (fm1_add_rfm1 WANTS
 * cpu2, FM1CORE_RFM1_CPU2: the end of this file), core 1's half of the voices on the host's core-1 thread. FIZZ's web manager
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
#include "rfm1_cpu2.h"

static void fizz_rfm1_in_init(fm1sx_in_t *s, fm1sx_accept_fn accept);
static int fizz_rfm1_frame(fm1sx_in_t *s, const uint8_t **p, uint32_t *n);
#define fm1sx_in_init(s, a) fizz_rfm1_in_init((s), (a))
#define fm1sx_frame(s, p, n) fizz_rfm1_frame((s), (p), (n))
#ifdef FM1CORE_RFM1_CPU2
#define qd_app_audio fizz_rfm1_audio          /* (the glue's one call: the sound through the split below) */
#endif
#include "core_quadrille.c"
#undef fm1sx_in_init
#undef fm1sx_frame
#ifdef FM1CORE_RFM1_CPU2
#undef qd_app_audio
#endif

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

#ifdef FM1CORE_RFM1_CPU2
/* CORE 1 (FM1VST docs/RFM1.md "cpu2", core-glue/common/rfm1_cpu2.h): each control block as FIZZ's firmware renders it on
 * two cores (firmware/app/fz_port.c fz_audio, P2.14b, without the pipeline of P2.15d: no block of latency more, so the
 * samples are qd_app_audio's): qd_app_block_begin; the larger half of the sounding voices (the top of the pool down to
 * ceil(act / 2) of them) into bus B as core 1's job; the rest into A on core 0; the join; qd_app_block_end. Any split
 * gives qd_app_audio's samples bit for bit (app.h: qd_app_block_voices is pure, the sums are integer). A host without a
 * thread for core 1 answers "run it yourself": the job runs after core 0's half, the same samples. */
static struct {
    qd_app_t *a;
    qd_bus_t *bus;
    int v0, v1;
} fizz_c1j;
static void fizz_c1_job(uint32_t arg)
{
    (void)arg;
    qd_app_block_voices(fizz_c1j.a, fizz_c1j.v0, fizz_c1j.v1, fizz_c1j.bus);
}
static void fizz_rfm1_block(qd_app_t *a, int16_t *out)
{
    qd_bus_t A, B;
    const int nv = a->pool.nvoice;
    int i, act = 0, k = 0, sp = nv, posted = 0;
    qd_app_block_begin(a);
    for (i = 0; i < nv; i++)
        act += a->pool.v[i].q.active != 0;
    for (i = nv - 1; i >= 0 && k < (act + 1) / 2; i--) {
        k += a->pool.v[i].q.active != 0;
        sp = i;
    }
    if (act >= 2) {                                     /* (one voice or none: not worth a hand-over) */
        fizz_c1j.a = a, fizz_c1j.bus = &B, fizz_c1j.v0 = sp, fizz_c1j.v1 = nv;
        posted = rfm1_cpu2_run(fizz_c1_job, 0);
    }
    qd_app_block_voices(a, 0, sp, &A);
    if (!posted || rfm1_cpu2_wait() != 0)
        qd_app_block_voices(a, sp, nv, &B);
    qd_app_block_end(a, &A, &B, out);
}
/* qd_app_audio (core/app/app.c) with fizz_rfm1_block for its qd_app_block */
void fizz_rfm1_audio(qd_app_t *a, int16_t *stereo, uint32_t frames)
{
    a->proj.nopage++;
    while (frames) {
        uint32_t n;
        if (a->ififo >= a->nfifo) {
            fizz_rfm1_block(a, a->fifo);
            a->nfifo = QD_MIX_CTL;
            a->ififo = 0;
        }
        n = (uint32_t)(a->nfifo - a->ififo);
        n = n < frames ? n : frames;
        memcpy(stereo, a->fifo + 2 * a->ififo, n * 2u * sizeof *stereo);
        stereo += 2u * n;
        a->ififo += (int)n;
        frames -= n;
    }
    a->proj.nopage--;
}
#endif
