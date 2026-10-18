// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
#include "engine.h"

#include <chrono>
#include <cstdio>
#include <cstring>

// The LED levels a core's emu_hal_t has: FM1VST's core-api (ABI 4, the cores built 2026-10-08) has led and
// led_dim only; ChoralRoot's newer emu_hooks.h adds led_mid and led_faint. Copied when the header has them.
template <typename H> static auto copy_mid(const H *h, uint8_t *out, int) -> decltype(h->led_mid, void())
{
    std::memcpy(out, h->led_mid, RFM_NCOL);
}
template <typename H> static void copy_mid(const H *, uint8_t *out, long) { std::memset(out, 0, RFM_NCOL); }
template <typename H> static auto copy_faint(const H *h, uint8_t *out, int) -> decltype(h->led_faint, void())
{
    std::memcpy(out, h->led_faint, RFM_NCOL);
}
template <typename H> static void copy_faint(const H *, uint8_t *out, long) { std::memset(out, 0, RFM_NCOL); }

Engine::~Engine()
{
    stop_timer_clock();
    dev_.reset();
    lc_.reset();                                        // shutdown(), the module unloaded
}

bool Engine::load(const std::string &path, std::string &err)
{
    lc_ = CoreLoader::load(path, &err);
    if (!lc_)
        return false;
    dev_ = std::make_unique<Device>(lc_->core());
    return true;
}

void Engine::boot(const std::vector<uint8_t> &flash)
{
    dev_->boot_from(flash.empty() ? nullptr : flash.data(), (uint32_t)flash.size());
    const emu_hal_t *h = dev_->hal();
    // label -> matrix id (hal->btn_id) inverted: matrix id -> label; role -> matrix encoder likewise
    std::memset(btn_label_, -1, sizeof btn_label_);
    std::memset(enc_role_, -1, sizeof enc_role_);
    for (int l = 0; l < EMU_NB; l++)
        if (h->btn_id[l] < EMU_NBTN)
            btn_label_[h->btn_id[l]] = (int8_t)l;
    for (int r = 0; r < EMU_NE - 1; r++)
        if (h->enc_id[r] < EMU_NENC)
            enc_role_[h->enc_id[r]] = (int8_t)r;
    flash_copy_.resize(dev_->flash_size());
    flash_saved_dirty_ = dev_->flash_dirty();
    dev_->set_between([this](uint32_t ms, bool frame) { between(ms, frame); });
}

int Engine::matrix_encoder(int role) const
{
    return dev_ && role >= 0 && role < EMU_NE - 1 ? dev_->hal()->enc_id[role] : -1;
}

void Engine::apply(const rfm_event_t &e)
{
    emu_hal_t *h = dev_->hal();
    switch (e.type) {
    case RFM_EV_KEY:
        if (e.index < EMU_NKEY) {
            keys_ = e.value ? keys_ | 1u << e.index : keys_ & ~(1u << e.index);
            dev_->keys(keys_);                          // a key newly down also taps (a press shorter than 1 ms)
            if (e.value)
                keys_seen++;
        }
        break;
    case RFM_EV_BUTTON:
        if (e.index < EMU_NBTN && btn_label_[e.index] >= 0) {
            const uint32_t bit = 1u << btn_label_[e.index];
            labels_ = e.value ? labels_ | bit : labels_ & ~bit;
            dev_->buttons(labels_);
        }
        break;
    case RFM_EV_ENC:
        if (e.index < EMU_NENC) {
            const int role = enc_role_[e.index];
            if (role >= 0)                              // Device::enc multiplies by enc_dir: undo it, so the
                dev_->enc(role, e.value * h->enc_dir[role]);   // matrix encoder moves by exactly e.value
            else
                h->enc[e.index] += e.value;
        }
        break;
    case RFM_EV_MASTER:
        dev_->master(e.value);
        break;
    default:
        return;
    }
    events_applied++;
}

void Engine::between(uint32_t ms, bool frame)
{
    (void)frame;
    rfm_event_t e;
    while (q_.pop(e))
        apply(e);
    dev_ms.store(ms, std::memory_order_relaxed);
    if (dev_->halted())
        halted = true;
    const emu_hal_t *h = dev_->hal();
    if (ms - last_pub_ >= period_ms_ || !published_) {
        rfm_leds_t l;
        std::memcpy(l.lit, h->led, RFM_NCOL);
        copy_mid(h, l.mid, 0);
        std::memcpy(l.dim, h->led_dim, RFM_NCOL);
        copy_faint(h, l.faint, 0);
        l.flags = dev_->led_play_green() ? RFM_LEDS_F_PLAY_GREEN : 0;
        last_pub_ = ms;
        if (!published_ || h->lcd_writes != last_writes_ || !rfm_leds_equal(&l, &last_leds_)) {
            Snapshot &s = tb_.back();
            std::memcpy(s.lcd, h->lcd, sizeof s.lcd);
            s.leds = l;
            s.seq = ++seq_;
            s.dev_ms = ms;
            tb_.publish();
            last_writes_ = h->lcd_writes;
            last_leds_ = l;
            published_ = true;
            snapshots++;
        }
    }
    if (ms - last_flash_check_ >= 30000u) {
        last_flash_check_ = ms;
        const uint32_t d = dev_->flash_dirty();
        if (d != flash_saved_dirty_ && !flash_ready_.load(std::memory_order_acquire) && dev_->flash()) {
            std::memcpy(flash_copy_.data(), dev_->flash(), flash_copy_.size());
            flash_saved_dirty_ = d;
            flash_ready_.store(true, std::memory_order_release);
        }
    }
}

void Engine::render(float *L, float *R, uint32_t n) { dev_->render(L, R, n, (double)EMU_FS); }

void Engine::start_timer_clock()
{
    timer_run_ = true;
    timer_ = std::thread([this] {
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();
        std::vector<int16_t> sink(2 * 8192);
        while (timer_run_) {
            const uint32_t target = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0)
                                        .count();
            while (dev_->ms() <= target && timer_run_)
                dev_->run_ms();
            dev_->take_s16(sink.data(), 8192);         // nobody listens: the audio is dropped
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void Engine::stop_timer_clock()
{
    timer_run_ = false;
    if (timer_.joinable())
        timer_.join();
}

bool Engine::take_flash(std::vector<uint8_t> &out)
{
    if (!flash_ready_.load(std::memory_order_acquire))
        return false;
    out = flash_copy_;
    flash_ready_.store(false, std::memory_order_release);
    return true;
}

void Engine::final_flash(std::vector<uint8_t> &out)
{
    out.clear();
    if (!dev_ || !dev_->flash())
        return;
    dev_->flash_sync();
    out.assign(dev_->flash(), dev_->flash() + dev_->flash_size());
}
