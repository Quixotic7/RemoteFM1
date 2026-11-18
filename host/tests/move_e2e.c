/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* rfm_move end to end: libmoveremote's client connected to Move Remote's simulator (moveremote-sim), presses
 * injected through the simulator's test FIFO (<dir>/mr-cmd), rfm_move turning them into panel actions, and its
 * LEDs and screen written back. tests/move_e2e.sh starts the simulator and runs this.
 *
 *   move_e2e --port N --dir DIR      (DIR: the simulator's --dir: mr-cmd, mr-state, mr-screen.pbm) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "moveremote/client.h"
#include "rfm/move.h"

static int g_fail, g_checks;
#define CHECK(c, ...)                                                                                               \
    do {                                                                                                            \
        g_checks++;                                                                                                 \
        if (c)                                                                                                      \
            printf("  ok    ");                                                                                     \
        else                                                                                                        \
            printf("  FAIL  "), g_fail++;                                                                           \
        printf(__VA_ARGS__);                                                                                        \
        putchar('\n');                                                                                              \
    } while (0)

static rfm_move_t g_m;
static rfm_move_act_t g_acts[256];
static int g_nact, g_inputs;
static uint32_t ms(void) { return (uint32_t)(mr_now_us() / 1000u); }

static void on_act(void *ctx, const rfm_move_act_t *a)
{
    (void)ctx;
    if (g_nact < 256)
        g_acts[g_nact++] = *a;
}
static void on_input(void *user, const mr_event_t *ev)
{
    (void)user;
    g_inputs++;
    rfm_move_input(&g_m, ev, ms());
}

static int find(uint8_t kind, int index, int down, int value)
{
    int i;
    for (i = 0; i < g_nact; i++)
        if (g_acts[i].kind == kind && g_acts[i].index == index && g_acts[i].down == down && g_acts[i].value == value)
            return 1;
    return 0;
}
static int find_midi(int s, int d1, int d2)
{
    int i;
    for (i = 0; i < g_nact; i++)
        if (g_acts[i].kind == RFM_MOVE_ACT_MIDI && g_acts[i].bytes[0] == s && g_acts[i].bytes[1] == d1 &&
            g_acts[i].bytes[2] == d2)
            return 1;
    return 0;
}

static int inject(const char *fifo, const char *lines)
{
    FILE *f = fopen(fifo, "w");
    if (!f)
        return -1;
    fputs(lines, f);
    fclose(f);
    return 0;
}

/* poll until n actions have come (or ms_max) */
static void pump(mr_client_t *c, int want, int ms_max)
{
    const uint32_t until = ms() + (uint32_t)ms_max;
    while (ms() < until && (want < 0 || g_nact < want))
        if (mr_poll(c, 10) < 0)
            break;
}

static int send_leds(mr_client_t *c)
{
    rfm_move_led_t out[RFM_MOVE_NLEDS];
    int n = rfm_move_leds(&g_m, out, RFM_MOVE_NLEDS), i, bad = 0;
    for (i = 0; i < n; i++)
        bad |= out[i].rgb ? mr_led_rgb(c, out[i].cc, out[i].number, out[i].r, out[i].g, out[i].b)
                          : mr_led(c, out[i].cc, out[i].number, out[i].r, 0);
    return bad ? -1 : n;
}

/* the simulator's screen shot (P1 128 64) against our packed frame */
static int shot_matches(const char *dir, const char *fifo, mr_client_t *c, const uint8_t *packed)
{
    char path[512], line[256];
    int x, y, ok = 1, tries;
    FILE *f = NULL;
    snprintf(path, sizeof path, "%s/mr-screen.pbm", dir);
    unlink(path);
    inject(fifo, "shot\n");
    for (tries = 0; tries < 50 && !(f = fopen(path, "r")); tries++)
        pump(c, -1, 20);
    if (!f)
        return 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "P1", 2) || !fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    for (y = 0; y < 64 && ok; y++) {
        if (!fgets(line, sizeof line, f) || strlen(line) < 128) {
            ok = 0;
            break;
        }
        for (x = 0; x < 128; x++)
            ok &= (line[x] == '1') == ((packed[(y >> 3) * 128 + x] >> (y & 7)) & 1);
    }
    fclose(f);
    return ok;
}

static int state_has(const char *dir, const char *what)
{
    char path[512], line[512];
    FILE *f;
    int ok = 0;
    snprintf(path, sizeof path, "%s/mr-state", dir);
    if (!(f = fopen(path, "r")))
        return 0;
    if (fgets(line, sizeof line, f))
        ok = strstr(line, what) != NULL;
    fclose(f);
    return ok;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    char fifo[512];
    int port = 0, i, n, tries;
    mr_callbacks_t cb;
    mr_client_t *c;
    uint8_t levels[RFM_MOVE_NLEVELS], frame[MR_SCREEN_BYTES];
    static uint16_t lcd[240 * 240];
    for (i = 1; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--port"))
            port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--dir"))
            dir = argv[i + 1];
    if (!port || !dir) {
        fprintf(stderr, "usage: move_e2e --port N --dir DIR\n");
        return 2;
    }
    snprintf(fifo, sizeof fifo, "%s/mr-cmd", dir);
    memset(&cb, 0, sizeof cb);
    cb.on_input = on_input;
    rfm_move_init(&g_m, NULL, on_act, NULL);
    printf("move_e2e\n");
    c = mr_connect("127.0.0.1", port, "rfm_move e2e", &cb, 3000);
    CHECK(c != NULL, "connected to the simulator (%s)", c ? mr_hello(c)->name : mr_last_error());
    if (!c)
        return 1;
    CHECK(mr_hello(c)->caps & MR_CAP_SIMULATOR, "it is the simulator");

    /* FM-1 mode: pad-2-1 (pad 16, note 84) = key 0 with velocity and pressure; Play; knob 1 = PRESETS; the
     * volume knob = MASTER; the jog wheel = SELECT; step 16 = key 26 */
    inject(fifo, "note 84 100\nat 84 50\nnote 84 0\ncc 85 127\ncc 85 0\ncc 71 6\ncc 71 124\ncc 79 2\ncc 14 1\n"
                 "note 31 127\nnote 31 0\n");
    pump(c, 12, 3000);
    CHECK(find(RFM_MOVE_ACT_KEY, 0, 1, 0) && g_acts[0].vel == 100, "pad-2-1 pressed: FM-1 key 0 down, velocity 100");
    CHECK(find(RFM_MOVE_ACT_PRESS, 0, 0, 50) && find(RFM_MOVE_ACT_PRESS, 0, 0, 0), "its pressure 50, then 0");
    CHECK(find(RFM_MOVE_ACT_KEY, 0, 0, 0), "released: key 0 up");
    CHECK(find(RFM_MOVE_ACT_BTN, RFM_MOVE_B_PLAY, 1, 0) && find(RFM_MOVE_ACT_BTN, RFM_MOVE_B_PLAY, 0, 0), "Play: PLAY");
    CHECK(find(RFM_MOVE_ACT_TURN, RFM_MOVE_E_PRESETS, 0, 3) && find(RFM_MOVE_ACT_TURN, RFM_MOVE_E_PRESETS, 0, -2),
          "knob 1: PRESETS +3, -2 (6 ticks, -4: a detent every 2)");
    CHECK(find(RFM_MOVE_ACT_MASTER, 0, 0, 768 + 32), "volume knob: MASTER 800");
    CHECK(find(RFM_MOVE_ACT_TURN, RFM_MOVE_E_SELECT, 0, 1), "jog wheel: SELECT +1");
    CHECK(find(RFM_MOVE_ACT_KEY, 26, 1, 0) && find(RFM_MOVE_ACT_KEY, 26, 0, 0), "step 16: key 26 (G5)");
    CHECK(g_nact == 12, "%d actions from %d input events (12 expected)", g_nact, g_inputs);

    /* the LEDs and the screen back to the Move */
    memset(levels, 0, sizeof levels);
    levels[3 * 4 + 3] = 255;                            /* key 0 lit */
    rfm_move_panel(&g_m, levels, 255);
    n = send_leds(c);
    CHECK(n == RFM_MOVE_NLEDS, "first LED write: all %d LEDs (%d)", RFM_MOVE_NLEDS, n);
    CHECK(send_leds(c) == 0, "nothing changed: no LED writes");
    for (i = 0; i < 240 * 240; i++)
        lcd[i] = (uint16_t)((i % 240) < 120 ? 0xFFFF : 0);
    rfm_move_screen(&g_m, lcd, frame);
    CHECK(mr_display(c, frame) == 0, "screen sent");
    CHECK(shot_matches(dir, fifo, c, frame), "the simulator shows our frame, pixel for pixel");
    for (tries = 0; tries < 160 && !(state_has(dir, "led_pending=0 ") && state_has(dir, "drew=1")); tries++)
        pump(c, -1, 25);
    CHECK(state_has(dir, "drew=1") && state_has(dir, "led_pending=0 ") && state_has(dir, "host=rfm_move e2e"),
          "simulator state: our host, the screen drawn, every LED write taken");

    /* Track 2: MIDI mode; pad 1 (bottom left) = C3 on channel 1 */
    g_nact = 0;
    inject(fifo, "cc 42 127\ncc 42 0\nnote 68 90\nnote 68 0\n");
    pump(c, 3, 3000);
    CHECK(g_m.s.mode == RFM_MOVE_MODE_MIDI && find(RFM_MOVE_ACT_SETTINGS, 0, 0, 0), "Track 2: MIDI mode");
    CHECK(find_midi(0x90, 48, 90) && find_midi(0x80, 48, 0), "the bottom-left pad: C3 on and off, channel 1");
    n = send_leds(c);
    CHECK(n > 0 && n < RFM_MOVE_NLEDS, "the mode change rewrote %d LEDs, not all", n);
    rfm_move_screen(&g_m, NULL, frame);
    mr_display(c, frame);
    CHECK(shot_matches(dir, fifo, c, frame), "the MIDI mode's screen shown");

    /* Track 4: the sequencer. Pad 1 picks C3, steps 1 and 5 get it, then Play starts the transport (only Play does,
     * the owner's); the internal clock (driven here as an audio callback would, 48 kHz in blocks of
     * 128) plays them as MIDI actions and the step LEDs show them */
    {
        rfm_move_led_t out[RFM_MOVE_NLEDS];
        const int before = g_inputs;
        int ons = 0, offs = 0, lit1 = 0, lit5 = 0, lit2 = 1, frames;
        uint32_t at5 = 0;
        g_nact = 0;
        inject(fifo, "cc 40 127\ncc 40 0\nnote 68 100\nnote 68 0\nnote 16 127\nnote 16 0\nnote 20 127\nnote 20 0\ncc 85 127\ncc 85 0\n");
        for (tries = 0; tries < 300 && g_inputs < before + 10; tries++)
            pump(c, -1, 10);
        CHECK(g_m.s.mode == RFM_MOVE_MODE_SEQ && g_m.seq.nnotes == 2 && g_m.seq.start_pending == 1,
              "Track 4: two steps set, Play started the transport (%d notes)", g_m.seq.nnotes);
        g_nact = 0;
        for (frames = 0; frames < 48000; frames += 128) {
            const int n0 = g_nact;
            rfm_move_seq_advance(&g_m, 128, 48000);
            for (i = n0; i < g_nact; i++)
                if (g_acts[i].kind == RFM_MOVE_ACT_MIDI && g_acts[i].bytes[1] == 48) {
                    if (g_acts[i].bytes[0] == 0x90) {
                        ons++;
                        if (ons == 2)
                            at5 = (uint32_t)frames + g_acts[i].frame;
                    } else if (g_acts[i].bytes[0] == 0x80)
                        offs++;
                }
        }
        CHECK(ons == 2 && offs == 2, "one second at 120 BPM: C3 on steps 1 and 5 (%d ons, %d offs)", ons, offs);
        CHECK(at5 == 4 * 6000, "step 5 at frame %u (24000 expected)", at5);
        rfm_move_leds_invalidate(&g_m);
        n = rfm_move_leds(&g_m, out, RFM_MOVE_NLEDS);
        for (i = 0; i < n; i++)
            if (!out[i].cc && out[i].number == MR_NOTE_STEP0 + 0)
                lit1 = out[i].r > 100;
            else if (!out[i].cc && out[i].number == MR_NOTE_STEP0 + 4)
                lit5 = out[i].r > 100;
            else if (!out[i].cc && out[i].number == MR_NOTE_STEP0 + 1)
                lit2 = out[i].r > 100;
        CHECK(lit1 && lit5 && !lit2, "step LEDs: 1 and 5 lit (white: notes), 2 not");
        rfm_move_leds_invalidate(&g_m);
        n = send_leds(c);
        CHECK(n == RFM_MOVE_NLEDS, "the sequencer's LEDs written (%d)", n);
        rfm_move_screen(&g_m, NULL, frame);
        mr_display(c, frame);
        CHECK(shot_matches(dir, fifo, c, frame), "the sequencer's screen shown");
        for (tries = 0; tries < 160 && !(state_has(dir, "led_pending=0 ") && state_has(dir, "drew=1")); tries++)
            pump(c, -1, 25);
        CHECK(state_has(dir, "led_pending=0 "), "every LED write taken");
        g_nact = 0;
        rfm_move_seq_play(&g_m, 0);
        inject(fifo, "cc 42 127\ncc 42 0\n");          /* a Track 2 tap: MIDI mode for the last part */
        pump(c, 1, 3000);
    }

    /* a held pad, then the link goes: everything let go */
    g_nact = 0;
    inject(fifo, "note 70 100\n");
    pump(c, 1, 3000);
    mr_close(c);
    rfm_move_release_all(&g_m);
    CHECK(find_midi(0x90, 52, 100) && find_midi(0x80, 52, 0), "link closed with a pad held: its note let go");
    printf("move_e2e: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
