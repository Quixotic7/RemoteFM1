/* SPDX-License-Identifier: GPL-3.0-only */
/* FIZZ's firmware unit for the .rfm1 (fm1_add_core UNIT): FIZZ's own glue, core/glue/core_quadrille.c, unchanged, plus
 * what the glue leaves to the device or does not give a host: the M-UPGRADE identity (below); the 49 live labels
 * (FM1VST docs/DYNAMIC-LABELS.md: qd_ctl_labels, the ones FIZZ's SysEx LABELS sends); built with rd1 (FM1_HOST_EXT),
 * readouts of the knobs' takeovers and the browser's words (FM1VST docs/RFM1.md "Readouts (rd1)"); and, built with
 * cpu2 (FM1CORE_RFM1_CPU2: the end of this file), core 1's half of the voices on the host's core-1 thread. FIZZ's web manager
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
#include "rfm1_ext.h"

static void fizz_rfm1_in_init(fm1sx_in_t *s, fm1sx_accept_fn accept);
static int fizz_rfm1_frame(fm1sx_in_t *s, const uint8_t **p, uint32_t *n);
#define fm1sx_in_init(s, a) fizz_rfm1_in_init((s), (a))
#define fm1sx_frame(s, p, n) fizz_rfm1_frame((s), (p), (n))
#ifdef FM1CORE_RFM1_CPU2
#define qd_app_audio fizz_rfm1_audio          /* (the glue's one call: the sound through the split below) */
#endif
#define fm1core_qd_frame fizz_rfm1_core_frame   /* (the descriptor's frame: below, the glue's and the readouts) */
#include "core_quadrille.c"
#undef fm1core_qd_frame
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

/* THE LIVE LABELS (FM1VST docs/DYNAMIC-LABELS.md): FIZZ's 49 (core/app/view_labels.c qd_ctl_labels, its SysEx LABELS),
 * in the same order as FM1CORE_LBL_*; the epoch moves when any of them changed (read at each poll: the host's, after a
 * frame) */
static qd_labels_t fizz_lbl;
static uint32_t fizz_lbl_epoch;
uint32_t fm1core_labels_epoch(void)
{
    qd_labels_t n;
    if (!fm1core_qd_up)
        return 0;
    qd_ctl_labels(&fm1core_qd_app, &n);
    if (!fizz_lbl_epoch || memcmp(&n, &fizz_lbl, sizeof n)) {
        fizz_lbl = n;
        if (++fizz_lbl_epoch == 0)
            fizz_lbl_epoch = 1;
    }
    return fizz_lbl_epoch;
}
const char *fm1core_label(int32_t which)
{
    return fm1core_qd_up && fizz_lbl_epoch && which >= 0 && which < QD_LBL_N ? fizz_lbl.s[which] : 0;
}
typedef char fizz_lbl_n_ok[QD_LBL_N == FM1CORE_LBL_N && QD_LBL_LEN == FM1CORE_LBL_LEN ? 1 : -1];

/* THE READOUTS (rd1): after each frame, a knob's takeover that moved (a new one, or a new detent: tk_ms) as its name
 * and value, as the screen draws them (qd_view_turn: FREQ, DECAY, HARM, FDBK, ALGO, TEMPO, SWING, PAGE, SCALE, ROOT,
 * OCTAVE, the track); a knob's band over a track's default or the held steps (QD_TK_DEFAULT, QD_TK_HELDKNOB) as the
 * knob's live label and the hold mode's value (qd_view_edit's model: the chord or the note that knob moves, VEL, LEN,
 * MICRO, RATCH, CHNC, COND, FUNC, SLIDE's time, the sound lock); the browser's LOADED / BACK / SAVED and an import as
 * messages. (The pool picker and the MIDI FX steps: not yet.) */
#ifdef FM1_HOST_EXT
static void fizz_rd_note(const ui_held_t *h, int knob, char *b, size_t n)   /* a NOTE slot's note, else the chord */
{
    int i;
    if (knob >= QD_K_SELECT && knob <= QD_K_4 && h->slot_note[knob == QD_K_SELECT ? 0 : knob - QD_K_1 + 1])
        for (i = 0; i < h->nnote && i < 6; i++)
            if (h->note[i] == h->slot_note[knob == QD_K_SELECT ? 0 : knob - QD_K_1 + 1]) {
                snprintf(b, n, "%s", h->note_name[i]);
                return;
            }
    if (h->root[0])
        snprintf(b, n, "%s%s", h->root, h->qual);
    else if (h->names[0])
        snprintf(b, n, "%s", h->names);
    else
        snprintf(b, n, "%s", h->nnote ? h->note_name[0] : "-");
}
static void fizz_rd_held(int knob)
{
    static ui_held_t h;
    static ui_lane_t l;
    char v[24];
    const char *t;
    v[0] = 0;
    memset(&h, 0, sizeof h);
    qd_view_edit(&fm1core_qd_app, &h, &l);
    switch (h.field) {
    case UI_F_NOTE: fizz_rd_note(&h, knob, v, sizeof v); break;
    case UI_F_VEL: snprintf(v, sizeof v, "%d", h.vel); break;
    case UI_F_LEN: snprintf(v, sizeof v, "%s", h.len_name ? h.len_name : ""); break;
    case UI_F_MICRO: snprintf(v, sizeof v, "%d", h.micro); break;
    case UI_F_RATCH: snprintf(v, sizeof v, "%d", h.count); break;
    case UI_F_CHNC: snprintf(v, sizeof v, "%d%%", h.chance); break;
    case UI_F_COND: snprintf(v, sizeof v, "%s", h.cond_name); break;
    case UI_F_FUNC: snprintf(v, sizeof v, "%s", h.func_name); break;
    case UI_F_SLIDE: snprintf(v, sizeof v, "%d", h.ptime); break;
    case UI_F_SOUND: snprintf(v, sizeof v, "%s", h.sound_cur ? h.sound_cur : ""); break;
    default: break;
    }
    (void)fm1core_labels_epoch();                       /* (the labels as of now: the knob's name) */
    t = knob >= 0 && knob < QD_NKNOB ? fizz_lbl.s[QD_LBL_KNOB0 + knob] : "";
    if (t[0] || v[0])
        RFM1_READOUT(-1, t, v);
}

static void fizz_rd_poll(void)
{
    static int last_tk = QD_TK_NONE;
    static uint32_t last_ms;
    const qd_ui_t *u = &fm1core_qd_app.ui;
    if (!fm1core_qd_up)
        return;
    if (u->tk == last_tk && (u->tk == QD_TK_NONE || u->tk_ms == last_ms))
        return;
    last_tk = u->tk, last_ms = u->tk_ms;
    if (u->tk >= 0 && u->tk < UI_NTK) {
        ui_turn_t v;
        memset(&v, 0, sizeof v);
        qd_view_turn(&fm1core_qd_app, &v);
        if (v.name && v.name[0])
            RFM1_READOUT_BAR(-1, v.name, v.val, (int32_t)(v.v * 1000.0f), 0, 1000);
    } else if (u->tk == QD_TK_DEFAULT || u->tk == QD_TK_HELDKNOB)
        fizz_rd_held(u->tk_src);
    else if (u->tk == QD_TK_LOADED)
        RFM1_MESSAGE("LOADED");
    else if (u->tk == QD_TK_BACK)
        RFM1_MESSAGE("BACK");
    else if (u->tk == QD_TK_SAVED)
        RFM1_MESSAGE("SAVED");
    else if (u->tk == QD_TK_IMPORT)
        RFM1_MESSAGE("IMPORTED");
}
#endif
void fm1core_qd_frame(void)
{
    fizz_rfm1_core_frame();
#ifdef FM1_HOST_EXT
    fizz_rd_poll();
#endif
}
