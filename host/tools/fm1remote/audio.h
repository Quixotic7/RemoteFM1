// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
// The Mac's default output (an AUHAL output unit), 44.1 kHz stereo float: the render callback runs the core.
#pragma once
#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <string>

class AudioOut {
public:
    using Fn = void (*)(void *ctx, float *L, float *R, uint32_t n);
    ~AudioOut() { stop(); }
    bool start(Fn fn, void *ctx, uint32_t frames, std::string &err);   // frames: the IO buffer (0: the default)
    void stop();
    double rate() const { return 44100.0; }

private:
    static OSStatus cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 n, AudioBufferList *io);
    AudioComponentInstance unit_ = nullptr;
    Fn fn_ = nullptr;
    void *ctx_ = nullptr;
    float scratch_[2][4096];
};
