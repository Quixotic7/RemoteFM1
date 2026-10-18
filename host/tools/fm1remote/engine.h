// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
// fm1remote's engine: one .fm1core run by FM1VST's Device on ONE clock thread (the audio callback, or a timer
// thread with --no-audio), as core-api/fm1core.h requires. Nothing else touches the core while the clock runs.
//
//   session thread -> clock thread   panel events, through a lock-free single-producer queue; applied in Device's
//                                    "between" hook (after tick, before frame / idle) in the order they came
//   clock thread -> session thread   the LCD (115 KB), the four LED arrays and PLAY green, copied into a lock-free
//                                    triple buffer at most every 1000 / fps device ms, and only when lcd_writes
//                                    moved or an LED changed
//   clock thread -> main thread      the flash image (1 MiB), copied every 30 device s if flash_dirty moved; the
//                                    main thread writes the file. At exit (clock stopped): flash_sync, then a copy.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cores.h"
#include "device.h"
#include "rfm/codec.h"
#include "rfm/panel.h"

struct Snapshot {
    uint16_t lcd[EMU_LCD_W * EMU_LCD_H];
    rfm_leds_t leds;
    uint32_t seq = 0, dev_ms = 0;
};

// single producer (the session thread), single consumer (the clock thread)
template <uint32_t N> class EventQueue {
public:
    bool push(const rfm_event_t &e)
    {
        const uint32_t w = w_.load(std::memory_order_relaxed);
        if (w - r_.load(std::memory_order_acquire) >= N)
            return false;
        buf_[w % N] = e;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(rfm_event_t &e)
    {
        const uint32_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire))
            return false;
        e = buf_[r % N];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<rfm_event_t, N> buf_{};
    std::atomic<uint32_t> w_{0}, r_{0};
};

// one writer, one reader; the reader always gets the newest published buffer
class TripleBuffer {
public:
    Snapshot &back() { return *s_[back_]; }
    void publish() { back_ = mid_.exchange(back_ | FRESH, std::memory_order_acq_rel) & 3; }
    const Snapshot *take()
    {
        if (!(mid_.load(std::memory_order_acquire) & FRESH))
            return nullptr;
        front_ = mid_.exchange(front_, std::memory_order_acq_rel) & 3;
        return s_[front_].get();
    }
    TripleBuffer()
    {
        for (auto &p : s_)
            p = std::make_unique<Snapshot>();
    }

private:
    static constexpr int FRESH = 4;
    std::unique_ptr<Snapshot> s_[3];
    int back_ = 0, front_ = 2;
    std::atomic<int> mid_{1};
};

class Engine {
public:
    ~Engine();
    bool load(const std::string &path, std::string &err);
    void boot(const std::vector<uint8_t> &flash);       // empty: a fresh flash
    const fm1core_t *core() const { return lc_ ? lc_->core() : nullptr; }
    Device *device() { return dev_.get(); }
    void set_fps(uint32_t fps) { period_ms_ = fps ? 1000u / fps : 50u; }
    int matrix_encoder(int role) const;                 // the matrix encoder of a knob role (hal->enc_id)

    // the clock
    void render(float *L, float *R, uint32_t n);        // the audio callback
    void start_timer_clock();                           // --no-audio
    void stop_timer_clock();

    // the session thread
    bool push_event(const rfm_event_t &e) { return q_.push(e); }
    const Snapshot *take_snapshot() { return tb_.take(); }

    // the main thread
    bool take_flash(std::vector<uint8_t> &out);         // a periodic copy, when one is ready
    void final_flash(std::vector<uint8_t> &out);        // the clock must be stopped
    uint32_t flash_size() const { return dev_ ? dev_->flash_size() : 0; }

    std::atomic<uint32_t> events_applied{0}, events_dropped{0}, snapshots{0}, dev_ms{0}, keys_seen{0};
    std::atomic<bool> halted{false};

private:
    void between(uint32_t ms, bool frame);
    void apply(const rfm_event_t &e);

    std::unique_ptr<LoadedCore> lc_;
    std::unique_ptr<Device> dev_;
    EventQueue<4096> q_;
    TripleBuffer tb_;
    uint32_t keys_ = 0, labels_ = 0, seq_ = 0;
    int8_t btn_label_[EMU_NBTN];
    int8_t enc_role_[EMU_NENC];
    uint32_t period_ms_ = 50, last_pub_ = 0, last_writes_ = ~0u;
    rfm_leds_t last_leds_{};
    bool published_ = false;
    uint32_t last_flash_check_ = 0, flash_saved_dirty_ = 0;
    std::vector<uint8_t> flash_copy_;
    std::atomic<bool> flash_ready_{false};
    std::thread timer_;
    std::atomic<bool> timer_run_{false};
};
