// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
// fm1remote: runs an .fm1core on the Mac with a RemoteFM1 unit as its panel.
//
// Threads: the core on the clock thread (the AUHAL render callback, or a timer thread with --no-audio: engine.h);
// the session (libremotefm1) on its own thread, with the transport back ends' callback threads feeding it through
// queues; the main thread waits for Ctrl-C, saves the flash image and shuts down.
//
// Without --device nothing is opened: the unit is libremotefm1's fake (fake.h) in this process, behind a loopback
// that runs at the measured rates (SysEx 300 KB/s, bulk 830 KB/s), so everything here can be tried and tested.
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "engine.h"
#include "patterns.h"
#include "rfm/fake.h"
#include "rfm/macos.h"
#include "rfm/screen.h"
#include "rfm/session.h"

namespace fs = std::filesystem;

#ifndef FM1VST_CORES
#define FM1VST_CORES ""
#endif

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

struct Options {
    std::string core = "choralroot", flash, port;
    int transport = RFM_T_AUTO;
    uint32_t fps = 20, budget = 0, sx_body = 0, seconds = 0, audio_frames = 0;
    bool stats = false, pattern = false, device = false, no_audio = false, fake_test = false, fresh = false;
    bool no_flash = false, verbose = false, midi_restart = false;
};

static void usage()
{
    std::fprintf(stderr,
        "usage: fm1remote [options]\n"
        "  --core PATH|NAME     the .fm1core to run (default choralroot; a NAME is looked up in FM1VST/build/cores\n"
        "                       and ~/Library/Application Support/fm1emu/cores)\n"
        "  --device             talk to the real unit (CoreMIDI + libusb). Without it: a built-in fake unit\n"
        "  --transport T        auto (bulk when it opens and answers, else SysEx) | midi | bulk\n"
        "  --fps N              frames per second to the unit (default 20)\n"
        "  --budget BYTES       bytes per frame (default: 800000/fps on bulk, 280000/fps on SysEx)\n"
        "  --sx-body N          the largest SysEx body (default 1024, lowered by the unit's HELLO_R)\n"
        "  --port NAME          only MIDI ports whose name contains NAME (with --device)\n"
        "  --midi-restart       MIDIRestart() first, when CoreMIDI keeps the unit's port offline (with --device)\n"
        "  --stats              one line per second: fps, bytes per frame, tiles pending, ACK latency, input\n"
        "  --pattern            no core: animated test screens, to measure the screen path\n"
        "  --no-audio           drive the core from a timer thread instead of the audio output\n"
        "  --audio-frames N     the output's IO buffer in frames (latency; default: the device's)\n"
        "  --flash PATH         the flash image (default ~/Library/Application Support/fm1remote/<core id>.bin)\n"
        "  --fresh              boot from an erased flash (the file is still written at exit)\n"
        "  --no-flash           RAM only: nothing loaded, nothing saved\n"
        "  --seconds N          stop after N seconds (default: at Ctrl-C)\n"
        "  --fake-test          with the fake unit: script input, check frames / input / LEDs / exit, PASS or FAIL\n"
        "  --verbose            the session's log\n");
}

static bool parse(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](std::string &v) {
            if (i + 1 >= argc)
                return false;
            v = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--core" && next(v)) o.core = v;
        else if (a == "--flash" && next(v)) o.flash = v;
        else if (a == "--port" && next(v)) o.port = v;
        else if (a == "--transport" && next(v)) {
            if (v == "auto") o.transport = RFM_T_AUTO;
            else if (v == "midi") o.transport = RFM_T_MIDI;
            else if (v == "bulk") o.transport = RFM_T_BULK;
            else return false;
        } else if (a == "--fps" && next(v)) o.fps = (uint32_t)std::strtoul(v.c_str(), nullptr, 0);
        else if (a == "--budget" && next(v)) o.budget = (uint32_t)std::strtoul(v.c_str(), nullptr, 0);
        else if (a == "--sx-body" && next(v)) o.sx_body = (uint32_t)std::strtoul(v.c_str(), nullptr, 0);
        else if (a == "--seconds" && next(v)) o.seconds = (uint32_t)std::strtoul(v.c_str(), nullptr, 0);
        else if (a == "--audio-frames" && next(v)) o.audio_frames = (uint32_t)std::strtoul(v.c_str(), nullptr, 0);
        else if (a == "--stats") o.stats = true;
        else if (a == "--pattern") o.pattern = true;
        else if (a == "--device") o.device = true;
        else if (a == "--no-audio") o.no_audio = true;
        else if (a == "--fake-test") o.fake_test = true;
        else if (a == "--fresh") o.fresh = true;
        else if (a == "--no-flash") o.no_flash = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--midi-restart") o.midi_restart = true;
        else return false;
    }
    if (o.fps < 1 || o.fps > 60)
        return false;
    if (o.fake_test && o.device) {
        std::fprintf(stderr, "fm1remote: --fake-test runs against the fake unit only\n");
        return false;
    }
    return true;
}

static std::string resolve_core(const std::string &c)
{
    if (c.find('/') != std::string::npos || (c.size() > 8 && c.substr(c.size() - 8) == ".fm1core"))
        return c;
    for (const std::string &d : {std::string(FM1VST_CORES), CoreLoader::default_user_dir()}) {
        if (d.empty())
            continue;
        const fs::path p = fs::path(d) / (c + ".fm1core");
        std::error_code ec;
        if (fs::is_regular_file(p, ec))
            return p.string();
    }
    return c;
}

static std::vector<uint8_t> read_file(const std::string &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool write_file(const std::string &p, const std::vector<uint8_t> &b)   // atomically: a temp file renamed
{
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char *>(b.data()), (std::streamsize)b.size()))
            return false;
    }
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}

// ------------------------------------------------------------------------ the session thread ---
struct App;
struct Loop {                                           // a loopback link to the fake unit
    App *app;
    int kind, id;
};

struct App {
    Options o;
    Engine eng;
    rfm_session_t *s = nullptr;
    rfm_mac_t *mac = nullptr;
    rfm_fake_t *fake = nullptr;
    Loop loops[2];
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::atomic<bool> session_done{false};
    std::atomic<uint32_t> events{0};
    int result = 0;                                     // --fake-test: 0 PASS, 1 FAIL
    rfm_leds_t leds_submitted{};                        // the last LEDs given to the session
    std::mutex log_mu;

    uint32_t now() const
    {
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    }
    void log(const char *m)
    {
        std::lock_guard<std::mutex> g(log_mu);
        std::fprintf(stderr, "[%7.3f] %s\n", now() / 1000.0, m);
    }
};

static int loop_write(void *ctx, const uint8_t *p, uint32_t n)
{
    Loop *l = static_cast<Loop *>(ctx);
    if (l->kind == RFM_LINK_SX)
        rfm_fake_rx_sx(l->app->fake, p, n, l->app->now());
    else
        rfm_fake_rx_bk(l->app->fake, p, n, l->app->now());
    return 0;
}

static void loop_pump(App &a, uint32_t now)
{
    static uint8_t buf[1 << 18];
    rfm_fake_poll(a.fake, now);
    for (Loop &l : a.loops) {
        if (l.id < 0)
            continue;
        const uint32_t n = l.kind == RFM_LINK_SX ? rfm_fake_take_sx(a.fake, buf, sizeof buf)
                                                 : rfm_fake_take_bk(a.fake, buf, sizeof buf);
        if (n)
            rfm_session_rx(a.s, l.id, buf, n, now);
    }
}

static void cb_event(void *ctx, const rfm_event_t *e)
{
    App *a = static_cast<App *>(ctx);
    a->events++;
    if (!a->o.pattern && !a->eng.push_event(*e))
        a->eng.events_dropped++;
}
static void cb_state(void *ctx, int st, const char *why)
{
    App *a = static_cast<App *>(ctx);
    char b[160];
    std::snprintf(b, sizeof b, "session: %s (%s)", rfm_session_state_name(st), why);
    a->log(b);
}
static void cb_rescan(void *ctx)
{
    App *a = static_cast<App *>(ctx);
    if (a->mac)
        rfm_mac_rescan(a->mac);
}
static void cb_log(void *ctx, const char *m)
{
    App *a = static_cast<App *>(ctx);
    if (a->o.verbose || std::strncmp(m, "found", 5) == 0)
        a->log(m);
}

// --fake-test: a script run against the fake unit, on the session thread
struct FakeTest {
    enum { WAIT_ACTIVE, SETTLE, INPUT, CHECK_INPUT, RUN, FREEZE, DONE } phase = WAIT_ACTIVE;
    uint32_t t_phase = 0, t_active = 0;
    std::vector<uint8_t> before;
    bool frozen = false, input_ok = false, screen_changed = false;
    int fails = 0;
    void fail(App &a, const char *what)
    {
        char b[200];
        std::snprintf(b, sizeof b, "FAIL: %s", what);
        a.log(b);
        fails++;
    }
    void pass(App &a, const char *what)
    {
        char b[200];
        std::snprintf(b, sizeof b, "ok: %s", what);
        a.log(b);
    }
};

static void fake_test_step(App &a, FakeTest &ft, uint32_t now)
{
    const uint8_t *shadow = rfm_fake_shadow(a.fake);
    rfm_session_stats_t st;
    rfm_session_stats(a.s, &st);
    const uint32_t end = (a.o.seconds ? a.o.seconds : 5) * 1000u;
    switch (ft.phase) {
    case FakeTest::WAIT_ACTIVE:
        if (rfm_session_state(a.s) == RFM_S_ACTIVE) {
            ft.t_active = now;
            ft.phase = FakeTest::SETTLE;
        } else if (now > 3000) {
            ft.fail(a, "no session within 3 s");
            ft.phase = FakeTest::DONE;
        }
        break;
    case FakeTest::SETTLE:                              // the boot screens settle; the screen as it is then
        if (now - ft.t_active >= 1500) {
            ft.before.assign(shadow, shadow + RFM_W * RFM_H * 2);
            if (!a.o.pattern) {
                rfm_fake_key(a.fake, 9, 1);             // note key 9 (D4) down ...
                const int e = a.eng.matrix_encoder(EMU_E_PRESETS);
                if (e >= 0)
                    rfm_fake_enc(a.fake, (uint32_t)e, 1);   // ... and the PRESETS knob a detent on
            }
            ft.phase = FakeTest::INPUT;
            ft.t_phase = now;
        }
        break;
    case FakeTest::INPUT:
        if (now - ft.t_phase >= 300) {
            if (!a.o.pattern)
                rfm_fake_key(a.fake, 9, 0);
            ft.phase = FakeTest::CHECK_INPUT;
            ft.t_phase = now;
        }
        break;
    case FakeTest::CHECK_INPUT:
        if (now - ft.t_phase >= 900) {
            ft.screen_changed = std::memcmp(ft.before.data(), shadow, ft.before.size()) != 0;
            if (a.o.pattern) {
                if (ft.screen_changed) ft.pass(a, "the pattern moves on the unit's screen");
                else ft.fail(a, "the unit's screen did not change");
            } else {
                char b[160];
                std::snprintf(b, sizeof b, "input reached the core: %u events applied, key down seen %u time(s)",
                              a.eng.events_applied.load(), a.eng.keys_seen.load());
                if (a.eng.keys_seen.load() >= 1 && a.eng.events_applied.load() >= 3) ft.pass(a, b);
                else ft.fail(a, b);
                if (ft.screen_changed) ft.pass(a, "the screen changed after the key and the knob");
                else ft.fail(a, "the screen did not change after the key and the knob");
            }
            ft.phase = FakeTest::RUN;
        }
        break;
    case FakeTest::RUN:
        if (now >= end) {                               // stop taking snapshots: the unit must catch up exactly
            ft.frozen = true;
            ft.phase = FakeTest::FREEZE;
            ft.t_phase = now;
        }
        break;
    case FakeTest::FREEZE: {
        const uint8_t *target = reinterpret_cast<const uint8_t *>(rfm_screen_target(rfm_session_screen(a.s)));
        if (!st.pending && !st.in_flight && !std::memcmp(target, shadow, RFM_W * RFM_H * 2)) {
            rfm_fake_stats_t fs;
            char b[200];
            rfm_fake_stats(a.fake, &fs);
            std::snprintf(b, sizeof b, "the unit's screen equals the frame (%u frames, %u acked, %u body bytes, %u "
                          "decode errors)", st.frames, st.acks, st.body_bytes, fs.decode_errs);
            if (st.frames >= 10 && st.acks == st.frames && !fs.decode_errs) ft.pass(a, b);
            else ft.fail(a, b);
            const bool same = rfm_leds_equal(rfm_fake_leds(a.fake), &a.leds_submitted) != 0;
            std::snprintf(b, sizeof b, "LEDs: %u LEDS messages, the unit's %s the core's", fs.leds,
                          same ? "equal" : "DIFFER FROM");
            if (fs.leds >= 3 && same) ft.pass(a, b);
            else ft.fail(a, b);
            ft.phase = FakeTest::DONE;
            g_stop = true;
        } else if (now - ft.t_phase > 3000) {
            ft.fail(a, "the unit's screen never equalled the frame");
            ft.phase = FakeTest::DONE;
            g_stop = true;
        }
        break;
    }
    case FakeTest::DONE:
        break;
    }
}

static void session_thread(App &a)
{
    FakeTest ft;
    uint32_t next_pattern = 0, pattern_frame_no = 0, t_stop = 0, last_stats = 0;
    bool stopping = false;
    rfm_session_stats_t prev{};
    static uint16_t pat[RFM_W * RFM_H];
    rfm_leds_t leds_now{};
    const uint32_t period = 1000u / a.o.fps;
    for (;;) {
        const uint32_t now = a.now();
        if (g_stop && !stopping) {
            rfm_session_stop(a.s, now);
            stopping = true;
            t_stop = now;
        }
        if (stopping && (rfm_session_state(a.s) == RFM_S_STOPPED || now - t_stop > 800))
            break;
        if (a.fake)
            loop_pump(a, now);
        else
            rfm_mac_pump(a.mac, now);
        if (a.o.pattern) {
            if (now >= next_pattern) {
                pattern_frame(pat, now, pattern_frame_no++);
                pattern_leds(&leds_now, now);
                if (!ft.frozen) {
                    rfm_session_set_frame(a.s, pat);
                    rfm_session_set_leds(a.s, &leds_now);
                    a.leds_submitted = leds_now;
                }
                next_pattern = now + period;
            }
        } else if (const Snapshot *sn = a.eng.take_snapshot()) {
            if (!ft.frozen) {
                rfm_session_set_frame(a.s, sn->lcd);
                rfm_session_set_leds(a.s, &sn->leds);
                a.leds_submitted = sn->leds;
            }
            leds_now = sn->leds;
        }
        rfm_session_poll(a.s, now);
        if (a.fake)
            loop_pump(a, now);
        if (a.o.fake_test && !stopping) {
            fake_test_step(a, ft, now);
            if (ft.phase == FakeTest::DONE && !g_stop)
                g_stop = true;
        }
        if (a.o.stats && now - last_stats >= 1000) {
            rfm_session_stats_t st;
            rfm_session_stats(a.s, &st);
            const uint32_t fr = st.frames - prev.frames, acks = st.acks - prev.acks;
            const int fl = rfm_session_frame_link(a.s);
            char b[300];
            std::snprintf(b, sizeof b,
                          "%-8s %-4s fps %4.1f  B/frame %6u  tiles/frame %5.1f  pending %3u  in flight %u  ACK %5.1f ms "
                          "(max %u)  input %u  LEDS %u%s%s",
                          rfm_session_state_name(rfm_session_state(a.s)),
                          fl < 0 ? "-" : rfm_session_link_kind(a.s, fl) == RFM_LINK_BK ? "bulk" : "midi",
                          fr * 1000.0 / (double)(now - last_stats), fr ? (st.body_bytes - prev.body_bytes) / fr : 0u,
                          fr ? (double)(st.tiles - prev.tiles) / fr : 0.0, st.pending, st.in_flight,
                          acks ? (double)(st.ack_ms_sum - prev.ack_ms_sum) / acks : 0.0, st.ack_ms_max,
                          st.events - prev.events, st.leds_sent - prev.leds_sent, a.o.pattern ? "  pattern " : "",
                          a.o.pattern ? pattern_name(now) : "");
            a.log(b);
            prev = st;
            last_stats = now;
        }
        if (a.mac)
            rfm_mac_wait(a.mac, 1);
        else
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (a.o.fake_test) {
        if (ft.phase != FakeTest::DONE)
            ft.fail(a, "the script did not finish");
        if (rfm_session_state(a.s) == RFM_S_STOPPED && !rfm_fake_session(a.fake))
            ft.pass(a, "clean stop: SESSION 0 acknowledged, the unit is back on its waiting screen");
        else
            ft.fail(a, "the session did not stop cleanly");
        a.result = ft.fails ? 1 : 0;
    }
    a.session_done = true;
}

static void audio_cb(void *ctx, float *L, float *R, uint32_t n) { static_cast<Engine *>(ctx)->render(L, R, n); }

int main(int argc, char **argv)
{
    App a;
    if (!parse(argc, argv, a.o)) {
        usage();
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // ---- the core
    std::string flash_path;
    if (!a.o.pattern) {
        std::string err;
        const std::string path = resolve_core(a.o.core);
        if (!a.eng.load(path, err)) {
            std::fprintf(stderr, "fm1remote: %s\n", err.c_str());
            return 1;
        }
        const fm1core_t *c = a.eng.core();
        flash_path = a.o.flash.empty() ? CoreLoader::app_support_dir() + "/fm1remote/" + c->id + ".bin" : a.o.flash;
        std::vector<uint8_t> img;
        if (!a.o.no_flash && !a.o.fresh) {
            img = read_file(flash_path);
            if (!img.empty() && img.size() != c->flash_size) {
                std::fprintf(stderr, "fm1remote: %s is %zu bytes, not %u: booting fresh\n", flash_path.c_str(),
                             img.size(), c->flash_size);
                img.clear();
            }
        }
        a.eng.set_fps(a.o.fps);
        a.eng.boot(img);
        std::fprintf(stderr, "fm1remote: %s %s (%s), flash %s\n", c->name, c->version, path.c_str(),
                     a.o.no_flash ? "RAM only" : img.empty() ? (flash_path + " (fresh)").c_str() : flash_path.c_str());
    }

    // ---- the session and its links
    rfm_session_cfg_t sc;
    rfm_session_cfg_default(&sc);
    sc.fps = a.o.fps;
    sc.budget = a.o.budget;
    sc.sx_body_max = a.o.sx_body;
    sc.transport = a.o.transport;
    rfm_session_cb_t cb{&a, cb_event, cb_state, cb_rescan, cb_log};
    a.s = rfm_session_new(&sc, &cb);
    a.loops[0] = {&a, RFM_LINK_SX, -1};
    a.loops[1] = {&a, RFM_LINK_BK, -1};
    if (a.o.device) {
        rfm_mac_cfg_t mc{};
        char err[200] = "";
        mc.allow_devices = 1;
        mc.port_filter = a.o.port.c_str();
        mc.use_midi = 1;
        mc.use_bulk = a.o.transport != RFM_T_MIDI;
        mc.midi_restart = a.o.midi_restart;
        mc.log = [](void *ctx, const char *m) { static_cast<App *>(ctx)->log(m); };
        mc.log_ctx = &a;
        a.mac = rfm_mac_new(&mc, err, sizeof err);
        if (!a.mac) {
            std::fprintf(stderr, "fm1remote: %s\n", err);
            return 1;
        }
        rfm_mac_attach(a.mac, a.s);
    } else {
        rfm_fake_cfg_t fc;
        rfm_fake_cfg_default(&fc);
        fc.sx_rate_bps = 300000;                        // the M0 numbers (PLAN.md section 10)
        fc.bk_rate_bps = 830000;
        a.fake = rfm_fake_new(&fc);
        a.loops[0].id = rfm_session_add_link(a.s, RFM_LINK_SX, loop_write, &a.loops[0], "fake midi");
        a.loops[1].id = rfm_session_add_link(a.s, RFM_LINK_BK, loop_write, &a.loops[1], "fake bulk");
        std::fprintf(stderr, "fm1remote: no --device: the unit is a built-in fake (loopback)\n");
    }

    // ---- the clock, then the session thread
    AudioOut audio;
    bool audio_on = false;
    if (!a.o.pattern) {
        std::string err;
        if (!a.o.no_audio && audio.start(audio_cb, &a.eng, a.o.audio_frames, err))
            audio_on = true;
        else {
            if (!a.o.no_audio)
                std::fprintf(stderr, "fm1remote: %s: running the core from a timer instead\n", err.c_str());
            a.eng.start_timer_clock();
        }
    }
    std::thread st([&a] { session_thread(a); });

    // ---- the main thread: Ctrl-C, --seconds, the flash every 30 s, a halted core
    std::vector<uint8_t> fl;
    while (!a.session_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (a.o.seconds && !a.o.fake_test && a.now() >= a.o.seconds * 1000u)
            g_stop = true;
        if (a.o.fake_test && a.now() > (a.o.seconds + 15u) * 1000u)
            g_stop = true;                              // (the script's own deadline is earlier)
        if (!a.o.pattern && !a.o.no_flash && a.eng.take_flash(fl) && !write_file(flash_path, fl))
            std::fprintf(stderr, "fm1remote: cannot write %s\n", flash_path.c_str());
        if (!a.o.pattern && a.eng.halted && !g_stop) {
            std::fprintf(stderr, "fm1remote: the core halted (it rebooted or exited): stopping\n");
            g_stop = true;
        }
    }
    st.join();

    // ---- shutdown: the session is stopped; the clock next, then the flash, then the core
    if (audio_on)
        audio.stop();
    else
        a.eng.stop_timer_clock();
    if (!a.o.pattern && !a.o.no_flash) {
        a.eng.final_flash(fl);
        if (!fl.empty()) {
            if (write_file(flash_path, fl))
                std::fprintf(stderr, "fm1remote: flash saved to %s\n", flash_path.c_str());
            else
                std::fprintf(stderr, "fm1remote: cannot write %s\n", flash_path.c_str());
        }
    }
    rfm_mac_free(a.mac);
    rfm_session_free(a.s);
    rfm_fake_free(a.fake);
    if (a.o.fake_test)
        std::fprintf(stderr, "fm1remote --fake-test: %s\n", a.result ? "FAIL" : "PASS");
    return a.o.fake_test ? a.result : 0;
}
