/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: the panel decoder (docs/PROTOCOL.md section 4). Portable C99.
 *
 * Plain MIDI channel messages from the unit become a panel state and events:
 *   9F/8F 53+k        note key k (0..26) down / up          (velocity 0 on 9F = up)
 *   9E/8E b           matrix button b (0..13) down / up
 *   BE 16+e 64+d      matrix encoder e: d detents (+ clockwise, as emu_hal_t.enc[e])
 *   BE 07 m, BE 27 l  MASTER: v14 = m << 7 | l, adc = v14 >> 4 (0..1023)
 * PANEL_R reconciles: PANEL_R wins. A control is corrected only if no MIDI event touched it in the last settle_ms
 * (PANEL_R travels on the session transport, the events on EP1: a PANEL_R may overtake an event still on its way);
 * an unsettled difference is left to the next PANEL_R. Encoders use the difference between counters (the first
 * PANEL_R after reset() only sets the baseline). MASTER is corrected by PANEL_R on any difference, 1 count too
 * (the unit sends MIDI only on a change of >= 2: the last count of a move reaches the host only by PANEL_R). */
#ifndef RFM_PANEL_H
#define RFM_PANEL_H
#include <stdint.h>
#include "rfm1.h"
#include "codec.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RFM_EV_KEY = 1, RFM_EV_BUTTON = 2, RFM_EV_ENC = 3, RFM_EV_MASTER = 4 };
enum { RFM_EVS_MIDI = 0, RFM_EVS_RESYNC = 1 };

typedef struct {
    uint8_t type;                                       /* RFM_EV_* */
    uint8_t index;                                      /* key 0..26, matrix button 0..13, matrix encoder 0..6 */
    uint8_t source;                                     /* RFM_EVS_*: a MIDI event, or a PANEL_R correction */
    int16_t value;                                      /* key / button: 1 down, 0 up; enc: detents; master 0..1023 */
} rfm_event_t;

typedef void (*rfm_event_fn)(void *ctx, const rfm_event_t *ev);

typedef struct {
    /* the panel as the host believes it is */
    uint32_t keys;                                      /* bit k: note key k held */
    uint32_t buttons;                                   /* bit b: matrix button b held */
    int16_t enc_count[RFM_NENC];                        /* the unit's absolute counters as the host tracks them */
    uint16_t master;                                    /* 0..1023 */
    uint16_t battery_mv;                                /* from PANEL_R, 0 unknown */
    uint8_t have_panel, have_master;
    /* counters */
    uint32_t events, resyncs, ignored;
    /* internal */
    uint32_t settle_ms;
    uint32_t t_key[RFM_NKEYS], t_btn[RFM_NBTN], t_enc[RFM_NENC], t_master;
    uint8_t master_msb, have_msb;
    rfm_event_fn fn;
    void *ctx;
} rfm_panel_t;

void rfm_panel_init(rfm_panel_t *p, uint32_t settle_ms, rfm_event_fn fn, void *ctx);
void rfm_panel_reset(rfm_panel_t *p);                   /* a new session: the next PANEL_R is the baseline */
/* one channel message (status byte first); returns 1 if it was a panel event */
int rfm_panel_midi(rfm_panel_t *p, const uint8_t *msg, uint32_t n, uint32_t now_ms);
void rfm_panel_reconcile(rfm_panel_t *p, const rfm_panel_r_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
