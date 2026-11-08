/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* libremotefm1: Bluetooth audio's encoder (docs/PROTOCOL.md section 9). A host's sound, mono floats at its own rate,
 * becomes AUDIO bodies: resampled to the stream's rate (44,100 / div: 22,050 Hz by default) by a 64-tap Kaiser-windowed
 * sinc low-pass at 0.45 x the lower rate, then IMA ADPCM (firmware/src/rfm_adpcm.h, the unit's own decoder) in blocks
 * of RFM_AUDIO_BLOCK samples (11.6 ms at 22,050 Hz; 133 body bytes, 158 on the wire as SysEx). Portable C99, no OS
 * calls; rfm_session_audio (session.h) uses it, a host may too. */
#ifndef RFM_AUDIO_ENC_H
#define RFM_AUDIO_ENC_H
#include <stdint.h>
#include "rfm1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rfm_audio_emit_fn)(void *ctx, const uint8_t *body, uint32_t n);   /* an AUDIO body */

typedef struct rfm_audio_enc rfm_audio_enc_t;
rfm_audio_enc_t *rfm_audio_enc_new(void);
void rfm_audio_enc_free(rfm_audio_enc_t *e);
/* a new stream at 44,100 / div Hz (RFM_AUDIO_DIV_MIN..MAX): the history, the coder and seq from 0 */
void rfm_audio_enc_reset(rfm_audio_enc_t *e, uint32_t div);
/* n mono samples (-1..1) at in_rate Hz (8000..192000; a new rate restarts the resampler); each block completed goes to
 * emit at once */
void rfm_audio_enc_push(rfm_audio_enc_t *e, const float *pcm, uint32_t n, uint32_t in_rate, rfm_audio_emit_fn emit,
                        void *ctx);
uint32_t rfm_audio_enc_rate(const rfm_audio_enc_t *e);   /* the stream's rate */

/* the unit's side, for tests and tools: an AUDIO body decoded (the samples, 2 x (n - RFM_AUDIO_HDR) of them); 0 if
 * malformed */
uint32_t rfm_audio_decode(const uint8_t *body, uint32_t n, int16_t *out);

#ifdef __cplusplus
}
#endif
#endif
