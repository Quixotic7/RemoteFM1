/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Saadet voice lifecycle. Allocation and mono key priority belong to the
 * common instrument; there is no parallel shadow keyboard or voice bank. */
#pragma once
static void fm6_sync(track_t *t, const voice_t *self);

static void fm6_note_on(track_t *t, voice_t *v)
{
    fm6_part_t *part=FM6P(t);
    fm6_voice_t *state=fm6_state(t,v);
    uint32_t index=(uint32_t)(v-t->v);
    int32_t previous_pitch[6];
    int glide=part->pt.lav && part->pt.pon && FM6F(t)[FN_PTIME]>0;
    if(glide)memcpy(previous_pitch,part->v[part->pt.lav-1u].porta,sizeof previous_pitch);
    fm6_sync(t,v);
    const uint8_t *patch=fm6_ed(t);
    uint32_t phase[6];
    memcpy(phase,state->ph,sizeof phase);
    memset(state,0,sizeof *state);
    if(!patch[FP_OKS] || voice_was)memcpy(state->ph,phase,sizeof phase);
    state->note=v->note;
    uint32_t velocity=v->mvel?v->mvel:v->vel;
    state->vel=(uint8_t)(FM6F(t)[FN_VNORM]?velocity*100u/127u:velocity);
    fm6_keyed(state,patch,fm6_note(t,v));
    for(uint32_t op=0;op<6u;op++){
        fm6_eg_go(&state->eg[op],patch+op*FP_OP,state->ol[op],state->rs[op],0);
        state->porta[op]=glide?previous_pitch[op]:state->base[op];
    }
    fm6_peg_set(&state->pe,patch);
    state->down=state->played=1;
    int first=1;
    for(uint32_t i=0;i<NVOICE;i++)if(i!=index && t->v[i].gate)first=0;
    if(first){
        if(patch[FP_LKS])part->lfo.ph=0x7FFFFFFFu;
        part->lfo.dly=0;part->pt.trig=1;
    }
    part->pt.lav=(uint8_t)(index+1u);
    part->pt.shared_valid=0;
}

/* A legato transition changes pitch targets without restarting the operator
 * or pitch envelopes. The instrument supplies the note selected by its stack. */
static void fm6_legato(track_t *t, voice_t *v)
{
    fm6_sync(t,v);
    fm6_voice_t *state=fm6_state(t,v);
    const uint8_t *patch=fm6_ed(t);
    uint32_t note=fm6_note(t,v);
    for(uint32_t op=0;op<6u;op++){
        state->base[op]=fm6_logfreq(patch+op*FP_OP,note);
        if(!FM6P(t)->pt.pon || !FM6F(t)[FN_PTIME])state->porta[op]=state->base[op];
    }
    state->note=v->note;state->freq_valid=0;state->still=state->quiet=0;
    state->down=1;
    FM6P(t)->pt.lav=(uint8_t)(v-t->v+1u);
    FM6P(t)->pt.shared_valid=0;
}
