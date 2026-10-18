// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
#include "audio.h"

#include <CoreAudio/CoreAudio.h>

#include <cstring>

OSStatus AudioOut::cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n,
                      AudioBufferList *io)
{
    (void)flags;
    (void)ts;
    (void)bus;
    AudioOut *a = static_cast<AudioOut *>(ref);
    float *L = io->mNumberBuffers > 0 ? static_cast<float *>(io->mBuffers[0].mData) : nullptr;
    float *R = io->mNumberBuffers > 1 ? static_cast<float *>(io->mBuffers[1].mData) : nullptr;
    while (n) {                                         // (a mono or odd layout still runs the clock)
        const uint32_t c = n > 4096 ? 4096 : n;
        float *l = L ? L : a->scratch_[0], *r = R ? R : a->scratch_[1];
        a->fn_(a->ctx_, l, r, c);
        if (L)
            L += c;
        if (R)
            R += c;
        n -= c;
    }
    return noErr;
}

bool AudioOut::start(Fn fn, void *ctx, uint32_t frames, std::string &err)
{
    fn_ = fn;
    ctx_ = ctx;
    AudioComponentDescription d{};
    d.componentType = kAudioUnitType_Output;
    d.componentSubType = kAudioUnitSubType_DefaultOutput;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) {
        err = "no default audio output";
        unit_ = nullptr;
        return false;
    }
    AudioStreamBasicDescription f{};
    f.mSampleRate = 44100.0;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    AURenderCallbackStruct rc{cb, this};
    OSStatus st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (st == noErr)
        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rc,
                                  sizeof rc);
    if (st == noErr && frames) {                        // the output device's IO buffer (the latency)
        AudioDeviceID dev = 0;
        UInt32 sz = sizeof dev;
        if (AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev,
                                 &sz) == noErr && dev) {
            AudioObjectPropertyAddress pa{kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
            UInt32 fr = frames;
            AudioObjectSetPropertyData(dev, &pa, 0, nullptr, sizeof fr, &fr);
        }
    }
    if (st == noErr)
        st = AudioUnitInitialize(unit_);
    if (st == noErr)
        st = AudioOutputUnitStart(unit_);
    if (st != noErr) {
        err = "audio output: OSStatus " + std::to_string((int)st);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
        return false;
    }
    return true;
}

void AudioOut::stop()
{
    if (!unit_)
        return;
    AudioOutputUnitStop(unit_);                         // returns once the callback has finished
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
}
