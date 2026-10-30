/* SPDX-License-Identifier: MPL-2.0 AND Apache-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * Portions Copyright 2012 Google Inc.
 * Licensed portions: http://www.apache.org/licenses/LICENSE-2.0
 * Provided AS IS, without warranties; full terms in LICENSES/Apache-2.0-msfa.txt.
 * MSFA routing and operator model: fm_core.cc / fm_op_kernel.cc, Apache-2.0.
 * Native fp32 implementation for Saadet. See docs/BLE_MIGRATION.md. */
#pragma once

static float fm6_bus[2][CTL], fm6_sum[CTL];

/* The compatibility argument accepts old renderer IDs; all render with MSFA. */
static __attribute__((section(".dsp_text"))) void fm6_op(fm6_voice_t *s, uint32_t k,
    float *out, const float *in, int add, uint32_t legacy_engine, uint32_t n)
{
    (void)legacy_engine;
    uint32_t phase=s->ph[k], step=(uint32_t)s->fq[k];
    float gain=s->g[k], delta=s->dg[k];
    for(uint32_t i=0;i<n;i++){
        gain+=delta;
        float value=fm6_sin(in?fm6_phase(phase,in[i]):phase&0xFFFFFFu)*gain;
        if(add)out[i]+=value;else out[i]=value;
        phase+=step;
    }
    s->ph[k]=phase;s->g[k]=gain;
}

static __attribute__((section(".dsp_text"))) void fm6_op_fb(fm6_voice_t *s, uint32_t k,
    float *out, int add, uint32_t legacy_engine, uint32_t n)
{
    (void)legacy_engine;
    uint32_t phase=s->ph[k], step=(uint32_t)s->fq[k];
    float gain=s->g[k], delta=s->dg[k], older=s->fb[0], newer=s->fb[1];
    float amount=1.0f/(float)(1u<<(s->fbs+1u));
    for(uint32_t i=0;i<n;i++){
        gain+=delta;
        float modulation=(older+newer)*amount;
        older=newer;
        newer=fm6_sin(fm6_phase(phase,modulation))*gain;
        if(add)out[i]+=newer;else out[i]=newer;
        phase+=step;
    }
    s->ph[k]=phase;s->g[k]=gain;s->fb[0]=older;s->fb[1]=newer;
}

static inline __attribute__((always_inline)) void fm6_plan(fm6_voice_t *s,
    const int32_t *levels, uint32_t algorithm, uint32_t legacy_engine, uint32_t feedback_shift)
{
    (void)legacy_engine;
    if(s->eng!=FM6_MODERN){
        memset(s->gout,0,sizeof s->gout);
        s->fb[0]=s->fb[1]=0.0f;
    }
    s->eng=FM6_MODERN;
    uint32_t available=1u;
    for(uint32_t k=0;k<6u;k++){
        uint32_t flags=FM6_ALG[algorithm&31u][k];
        uint32_t output=flags&3u, input=(flags>>4)&3u, add=flags&FM6_P_ADD;
        float start=s->gout[k];
        float target=levels[k]==0?1.0f/16384.0f:
            fm_exp2f((float)levels[k]*(1.0f/16777216.0f)-14.0f);
        s->gout[k]=target;s->plan[k]=0;
        if(start<1120.0f/16777216.0f && target<1120.0f/16777216.0f){
            if(!add)available&=~(1u<<output);
            s->ph[k]+=(uint32_t)s->fq[k]<<FM6_LG_N;
            continue;
        }
        if(!(available&(1u<<output)))add=0;
        if(input && !(available&(1u<<input)))input=0;
        s->g[k]=start;s->dg[k]=(target-start)*(1.0f/FM6_N);
        s->plan[k]=(uint8_t)(FM6_P_RUN|output|add|(input<<4));
        if(!input && (flags&0xC0u)==0xC0u && feedback_shift<16u){
            s->plan[k]|=FM6_P_FB;s->fbs=(uint8_t)feedback_shift;
        }
        available|=1u<<output;
    }
}
