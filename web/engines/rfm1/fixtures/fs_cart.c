// SPDX-License-Identifier: GPL-3.0-only
// A tiny Fantasy CFW for engines/rfm1/fs-test.mjs: the .rfm1 exports (a blank panel, no sound, no flash) and
// rfm1_host_1's file API, reached through t_* exports the test calls between renders. fs-test.mjs adds the manifest
// (the rfm1 section) to the module it builds from this, per case. Rebuild (emsdk):
//   emcc -O2 fs_cart.c -o fs_cart.wasm --no-entry -sSTANDALONE_WASM -sINITIAL_MEMORY=4MB -sALLOW_MEMORY_GROWTH=0 \
//        -sSTACK_SIZE=65536 -sERROR_ON_UNDEFINED_SYMBOLS=0
//   and the same with -DNOFS -o fs_cart_plain.wasm (no file API: imports nothing)
#include <stdint.h>
#define EXPORT(n) __attribute__((export_name(n)))
#define HOST(n) __attribute__((import_module("rfm1_host_1"), import_name(n)))

#ifndef NOFS
HOST("fs_request") int32_t fs_request(int32_t op, const char *path, int32_t path_len, int64_t offset, void *buf, int32_t len);
HOST("fs_poll") int32_t fs_poll(int32_t id);
HOST("fs_cancel") void fs_cancel(int32_t id);
HOST("fs_changed") int32_t fs_changed(void);
#endif

static uint8_t panel[115384] __attribute__((aligned(8)));
static uint32_t desc[48];
static uint8_t arena[1 << 16] __attribute__((aligned(16)));
static uint32_t arena_at;
static uint8_t big[1 << 21] __attribute__((aligned(16)));   // the test's buffers (2 MiB)
static const int32_t LAYOUT[] = { 115384, 0, 4, 8, 12, 16, 44, 48, 75, 102, 146, 148, 115348, 115352, 115366, 115373, 115380,
    192, 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 92, 96, 100, 104, 108, 188 };
static volatile uint32_t ticks;

EXPORT("rfm1_version") int32_t rfm1_version(void) { return 1; }
EXPORT("rfm1_get") int32_t rfm1_get(int32_t abi) {
    if (abi != 6) return 0;
    desc[0] = 6; desc[1] = (uint32_t)"fscart"; desc[2] = (uint32_t)"FS Cart"; desc[3] = (uint32_t)"1.0";
    desc[5] = (uint32_t)"Other"; desc[6] = (uint32_t)"MIT";
    for (int i = 0; i < 14; i++) desc[9 + i] = (uint32_t)"B";
    desc[25] = (uint32_t)panel;
    return (int32_t)desc;
}
EXPORT("rfm1_layout") int32_t rfm1_layout(int32_t i) { return i >= 0 && i < (int32_t)(sizeof LAYOUT / 4) ? LAYOUT[i] : -1; }
EXPORT("rfm1_alloc") int32_t rfm1_alloc(int32_t n) { uint32_t p = (arena_at + 15) & ~15u; if (p + n > sizeof arena) return 0; arena_at = p + n; return (int32_t)(arena + p); }
EXPORT("rfm1_free") void rfm1_free(int32_t p) { (void)p; }
EXPORT("rfm1_options") void rfm1_options(int32_t a, int32_t b, int32_t c, int32_t d) { (void)a; (void)b; (void)c; (void)d; }
EXPORT("rfm1_boot_options") int32_t rfm1_boot_options(int32_t a, int32_t b, int32_t c) { (void)a; (void)b; (void)c; return 1; }
EXPORT("rfm1_init") void rfm1_init(int32_t demo) { (void)demo; }
#ifndef NOFS
static void demo_tick(int32_t ms);
static int demo_on = 1, demo_ok;
#endif
EXPORT("rfm1_tick") void rfm1_tick(int32_t ms) {
    ticks = ms;
#ifndef NOFS
    if (demo_on) demo_tick(ms);
#endif
}
EXPORT("rfm1_frame") void rfm1_frame(void) {}
EXPORT("rfm1_idle") void rfm1_idle(void) {}
static uint32_t phase;
EXPORT("rfm1_audio") void rfm1_audio(int32_t out, int32_t frames) {
    int16_t *o = (int16_t *)out;
    for (int i = 0; i < frames; i++) {                 // a quiet square once the demo read its file; else silence
        int16_t v = 0;
#ifndef NOFS
        if (demo_ok) v = (phase++ / 50) & 1 ? 3000 : -3000;
#endif
        o[2 * i] = o[2 * i + 1] = v;
    }
}
EXPORT("rfm1_midi_in") int32_t rfm1_midi_in(int32_t p) { (void)p; return 1; }
EXPORT("rfm1_midi_out_take") int32_t rfm1_midi_out_take(int32_t p) { (void)p; return 0; }
EXPORT("rfm1_halted") int32_t rfm1_halted(void) { return 0; }
EXPORT("rfm1_shutdown") void rfm1_shutdown(void) {}
EXPORT("rfm1_flash_stage") int32_t rfm1_flash_stage(int32_t p, int32_t n) { (void)p; (void)n; return 0; }
EXPORT("rfm1_flash") int32_t rfm1_flash(void) { return 0; }
EXPORT("rfm1_flash_dirty") int32_t rfm1_flash_dirty(void) { return 0; }
EXPORT("rfm1_flash_sync") int32_t rfm1_flash_sync(void) { return 0; }
EXPORT("rfm1_stats") void rfm1_stats(int32_t a, int32_t b) { (void)a; (void)b; }
EXPORT("rfm1_ui_info") void rfm1_ui_info(int32_t a, int32_t b) { (void)a; (void)b; }

#ifndef NOFS
// the test's way in
EXPORT("t_buf") int32_t t_buf(void) { return (int32_t)big; }
EXPORT("t_request") int32_t t_request(int32_t op, int32_t path, int32_t n, int64_t off, int32_t buf, int32_t len) { return fs_request(op, (const char *)path, n, off, (void *)buf, len); }
EXPORT("t_poll") int32_t t_poll(int32_t id) { return fs_poll(id); }
EXPORT("t_cancel") void t_cancel(int32_t id) { fs_cancel(id); }
EXPORT("t_changed") int32_t t_changed(void) { return fs_changed(); }
// polled n times inside one call: never complete during a call
EXPORT("t_spin") int32_t t_spin(int32_t id, int32_t n) { int32_t r = -1; for (int i = 0; i < n && r == -1; i++) r = fs_poll(id); return r; }

// The demo (what the e2e test watches in a page): on boot, and whenever fs_changed moves, it READs /code/hello.txt
// and WRITEs "got:<its text>" to /data/out.txt; the screen is red while it has no file (FS_ENOFOLDER, FS_ENOENT: it
// tries again every 250 ms), green once written, and the sound a quiet square. t_demo(0) stops it (fs-test.mjs).
EXPORT("t_demo") void t_demo(int32_t on) { demo_on = on; }
static int32_t st, req_id, changed_seen = -1000, retry_at, got;
static char text[260];
static void fill(uint16_t rgb) {
    uint16_t be = (uint16_t)((rgb >> 8) | (rgb << 8)), *lcd = (uint16_t *)(panel + 148);
    if (lcd[0] == be) return;
    for (int i = 0; i < 240 * 240; i++) lcd[i] = be;
    (*(volatile uint32_t *)(panel + 115348))++;
}
static int32_t len_of(const char *s) { int32_t n = 0; while (s[n]) n++; return n; }
static void demo_tick(int32_t ms) {
    int32_t c = fs_changed();
    if (c != changed_seen) { changed_seen = c; if (st == 1 || st == 2) { fs_cancel(req_id); fs_poll(req_id); } st = 0; retry_at = ms; }
    if (st == 0 && ms >= retry_at) {
        static const char p[] = "/code/hello.txt";
        req_id = fs_request(3, p, len_of(p), 0, text + 4, 200);
        st = req_id > 0 ? 1 : 0;
        retry_at = ms + 250;
    } else if (st == 1) {
        int32_t r = fs_poll(req_id);
        if (r == -1) return;
        if (r < 0) { fill(0xF800); demo_ok = 0; st = 0; retry_at = ms + 250; return; }
        got = r;
        text[0] = 'g'; text[1] = 'o'; text[2] = 't'; text[3] = ':';
        static const char q[] = "/data/out.txt";
        req_id = fs_request(4, q, len_of(q), -1, text, 4 + got);
        st = req_id > 0 ? 2 : 0;
    } else if (st == 2) {
        int32_t r = fs_poll(req_id);
        if (r == -1) return;
        if (r == 4 + got) { fill(0x07E0); demo_ok = 1; st = 3; } else { fill(0xF800); st = 0; retry_at = ms + 250; }
    }
}
#endif
