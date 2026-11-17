/* SPDX-License-Identifier: MPL-2.0 */
/* libmoveremote: the host's end of the wire (include/moveremote/client.h, docs/PROTOCOL.md). */
#define _GNU_SOURCE
#include "../include/moveremote/client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RX_CAP (256 * 1024)

struct mr_client {
    int fd;
    mr_callbacks_t cb;
    mr_hello_t hello;
    int have_hello;
    uint8_t *rx;
    size_t rx_n;
};

uint64_t mr_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

/* ------------------------------------------------------------------ decoding --- */
static int rel(int v) { return v < 64 ? v : v - 128; }

void mr_decode(const uint8_t p[4], uint32_t frame, mr_event_t *ev)
{
    const int cable = p[0] >> 4, cin = p[0] & 0x0F, st = p[1] & 0xF0, d1 = p[2], d2 = p[3];
    memset(ev, 0, sizeof *ev);
    ev->frame = frame;
    memcpy(ev->raw, p, 4);
    ev->type = MR_EV_OTHER;
    if (cable == 2) {
        ev->type = MR_EV_MIDI;
        return;
    }
    if (cable != 0)
        return;
    if (cin >= 0x4 && cin <= 0x7) {
        ev->type = MR_EV_SYSEX;
        return;
    }
    switch (st) {
    case 0x90:
    case 0x80: {
        const int on = st == 0x90 && d2 > 0;
        if (d1 <= MR_NOTE_JOG_TOUCH)
            ev->type = MR_EV_TOUCH, ev->index = d1, ev->value = on;
        else if (d1 >= MR_NOTE_STEP0 && d1 < MR_NOTE_STEP0 + MR_NSTEPS)
            ev->type = MR_EV_STEP, ev->index = d1 - MR_NOTE_STEP0, ev->value = on;
        else if (d1 >= MR_NOTE_PAD0 && d1 < MR_NOTE_PAD0 + MR_NPADS)
            ev->type = MR_EV_PAD, ev->index = d1 - MR_NOTE_PAD0, ev->value = on ? d2 : 0;
        break;
    }
    case 0xA0:
        if (d1 >= MR_NOTE_PAD0 && d1 < MR_NOTE_PAD0 + MR_NPADS)
            ev->type = MR_EV_PAD_PRESSURE, ev->index = d1 - MR_NOTE_PAD0, ev->value = d2;
        break;
    case 0xD0:
        ev->type = MR_EV_PRESSURE, ev->value = d1;
        break;
    case 0xB0:
        if (d1 >= MR_CC_KNOB1 && d1 <= MR_CC_VOLUME)
            ev->type = MR_EV_KNOB, ev->index = d1 - MR_CC_KNOB1, ev->value = rel(d2);
        else if (d1 == MR_CC_JOG)
            ev->type = MR_EV_JOG, ev->value = rel(d2);
        else if (d1 == MR_CC_JOG_CLICK)
            ev->type = MR_EV_JOG_CLICK, ev->value = d2 > 0;
        else if (d1 == MR_CC_LINE_IN || d1 == MR_CC_LINE_OUT)
            ev->type = MR_EV_JACK, ev->index = d1 == MR_CC_LINE_OUT, ev->value = d2 >= 64;
        else
            ev->type = MR_EV_BUTTON, ev->index = d1, ev->value = d2 > 0;
        break;
    default:
        break;
    }
}

const char *mr_button_name(int cc)
{
    switch (cc) {
    case MR_CC_SHIFT: return "Shift";
    case MR_CC_MENU: return "Menu";
    case MR_CC_BACK: return "Back";
    case MR_CC_CAPTURE: return "Capture";
    case MR_CC_DOWN: return "Down";
    case MR_CC_UP: return "Up";
    case MR_CC_UNDO: return "Undo";
    case MR_CC_LOOP: return "Loop";
    case MR_CC_COPY: return "Copy";
    case MR_CC_LEFT: return "Left";
    case MR_CC_RIGHT: return "Right";
    case MR_CC_PLAY: return "Play";
    case MR_CC_REC: return "Rec";
    case MR_CC_MUTE: return "Mute";
    case MR_CC_SAMPLE: return "Sample";
    case MR_CC_DELETE: return "Delete";
    case 43: return "Track 1";
    case 42: return "Track 2";
    case 41: return "Track 3";
    case 40: return "Track 4";
    default: return NULL;
    }
}

/* ------------------------------------------------------------------ sending --- */
static int send_all(mr_client_t *c, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n > 0) {
        const ssize_t w = send(c->fd, b, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { c->fd, POLLOUT, 0 };
                poll(&pf, 1, 100);
                continue;
            }
            return -1;
        }
        b += w, n -= (size_t)w;
    }
    return 0;
}

static int send_msg(mr_client_t *c, uint8_t type, const void *payload, size_t n)
{
    uint8_t hdr[3];
    if (!c || c->fd < 0 || n + 1 > MR_MAX_MSG)
        return -1;
    put16(hdr, (uint32_t)(n + 1));
    hdr[2] = type;
    if (n + 3 <= 512) {   /* small ones in one write: one TCP segment */
        uint8_t buf[512];
        memcpy(buf, hdr, 3);
        if (n)
            memcpy(buf + 3, payload, n);
        return send_all(c, buf, n + 3);
    }
    return send_all(c, hdr, 3) || send_all(c, payload, n) ? -1 : 0;
}

int mr_led(mr_client_t *c, int cc, int number, int value, int anim)
{
    const uint8_t t[3] = { (uint8_t)((cc ? 0xB0 : 0x90) | (anim & 15)), (uint8_t)(number & 127), (uint8_t)(value & 127) };
    return send_msg(c, MR_H_LED, t, 3);
}

int mr_leds_raw(mr_client_t *c, const uint8_t *triples, int n) { return send_msg(c, MR_H_LED, triples, (size_t)n * 3); }

int mr_led_rgb(mr_client_t *c, int cc, int number, int r, int g, int b)
{
    const uint8_t t[5] = { (uint8_t)(cc ? 1 : 0), (uint8_t)(number & 127), (uint8_t)r, (uint8_t)g, (uint8_t)b };
    return send_msg(c, MR_H_LED_RGB, t, 5);
}

int mr_pad_rgb(mr_client_t *c, int pad, int r, int g, int b)
{
    return mr_led_rgb(c, 0, MR_NOTE_PAD0 + (pad & 31), r, g, b);
}

int mr_display(mr_client_t *c, const uint8_t packed[MR_SCREEN_BYTES])
{
    return send_msg(c, MR_H_DISPLAY, packed, MR_SCREEN_BYTES);
}

int mr_audio(mr_client_t *c, const int16_t *stereo, int frames)
{
    while (frames > 0) {   /* little-endian on every host this builds for; one message holds up to 16383 frames */
        const int n = frames > 8192 ? 8192 : frames;
        if (send_msg(c, MR_H_AUDIO, stereo, (size_t)n * 4) < 0)
            return -1;
        stereo += n * 2, frames -= n;
    }
    return 0;
}

int mr_midi(mr_client_t *c, const uint8_t *bytes, int n) { return send_msg(c, MR_H_MIDI, bytes, (size_t)n); }

int mr_config(mr_client_t *c, int key, int32_t value)
{
    uint8_t p[5];
    p[0] = (uint8_t)key;
    put32(p + 1, (uint32_t)value);
    return send_msg(c, MR_H_CONFIG, p, 5);
}

int mr_ping(mr_client_t *c, uint32_t token)
{
    uint8_t p[12];
    const uint64_t t = mr_now_us();
    put32(p, token);
    put32(p + 4, (uint32_t)t);
    put32(p + 8, (uint32_t)(t >> 32));
    return send_msg(c, MR_H_PING, p, 12);
}

int mr_quit(mr_client_t *c) { return send_msg(c, MR_H_QUIT, NULL, 0); }

int mr_raw(mr_client_t *c, const uint8_t *packets, int n) { return send_msg(c, MR_H_RAW, packets, (size_t)n * 4); }

/* ------------------------------------------------------------------ receiving --- */
static void on_msg(mr_client_t *c, uint8_t type, const uint8_t *p, size_t n)
{
    switch (type) {
    case MR_D_HELLO:
        if (n >= 46 && !memcmp(p, "MOVR", 4)) {
            mr_hello_t *h = &c->hello;
            memset(h, 0, sizeof *h);
            h->proto = get16(p + 4), h->caps = get16(p + 6), h->sample_rate = get32(p + 8), h->block = get16(p + 12);
            h->screen_w = p[14], h->screen_h = p[15], h->via = p[16], h->out_capacity = get32(p + 18);
            memcpy(h->name, p + 22, 24);
            h->name[24] = 0;
            c->have_hello = 1;
            if (c->cb.on_hello)
                c->cb.on_hello(c->cb.user, h);
        }
        break;
    case MR_D_INPUT: {
        size_t i;
        for (i = 0; i + 8 <= n; i += 8) {
            mr_event_t ev;
            mr_decode(p + i + 4, get32(p + i), &ev);
            if (c->cb.on_input)
                c->cb.on_input(c->cb.user, &ev);
        }
        break;
    }
    case MR_D_AUDIO:
        if (n >= 9 && c->cb.on_audio) {
            const int frames = get16(p + 4), flags = p[8];
            const int16_t *in = NULL;
            if ((flags & MR_AUDIO_HAS_PCM) && n >= 9 + (size_t)frames * 4)
                in = (const int16_t *)(const void *)(p + 9);   /* (the rx buffer is copied aligned, below) */
            c->cb.on_audio(c->cb.user, get32(p), in, frames, get16(p + 6), flags);
        }
        break;
    case MR_D_STATUS:
        if (n >= MR_STATUS_LEN && c->cb.on_status) {
            mr_status_t s;
            s.frame = get32(p), s.battery = p[4], s.power = p[5], s.jacks = p[6], s.flags = p[7];
            s.underruns = get32(p + 8), s.overruns = get32(p + 12), s.late = get32(p + 16), s.dropped = get32(p + 20);
            s.led_pending = get16(p + 24), s.out_fill = get16(p + 26), s.test_vol = p[28];
            c->cb.on_status(c->cb.user, &s);
        }
        break;
    case MR_D_PONG:
        if (n >= 16 && c->cb.on_pong)
            c->cb.on_pong(c->cb.user, get32(p), (uint64_t)get32(p + 4) | (uint64_t)get32(p + 8) << 32, get32(p + 12));
        break;
    case MR_D_BYE:
        if (c->cb.on_bye)
            c->cb.on_bye(c->cb.user, n ? p[0] : 0);
        break;
    default:
        break;
    }
}

static int rx_parse(mr_client_t *c)
{
    size_t off = 0;
    while (c->rx_n - off >= 2) {
        const size_t len = get16(c->rx + off);
        if (len == 0)
            return -1;
        if (c->rx_n - off < 2 + len)
            break;
        {
            /* the payload starts at an odd offset; audio is read as int16, so it gets an aligned copy */
            static __thread uint8_t *al;
            static __thread size_t al_cap;
            const uint8_t *pl = c->rx + off + 3;
            if (len - 1 > 0 && ((uintptr_t)pl & 1)) {
                if (al_cap < len) {
                    free(al);
                    al = malloc(len);
                    al_cap = al ? len : 0;
                }
                if (al) {
                    memcpy(al, pl, len - 1);
                    pl = al;
                }
            }
            on_msg(c, c->rx[off + 2], pl, len - 1);
        }
        off += 2 + len;
        if (c->fd < 0)
            return -1;
    }
    if (off) {
        memmove(c->rx, c->rx + off, c->rx_n - off);
        c->rx_n -= off;
    }
    return 0;
}

int mr_poll(mr_client_t *c, int timeout_ms)
{
    struct pollfd pf;
    int first = 1;
    if (!c || c->fd < 0)
        return -1;
    pf.fd = c->fd, pf.events = POLLIN, pf.revents = 0;
    if (poll(&pf, 1, timeout_ms) < 0 && errno != EINTR)
        return -1;
    if (!(pf.revents & (POLLIN | POLLHUP | POLLERR)))
        return 0;
    for (;;) {
        const ssize_t r = recv(c->fd, c->rx + c->rx_n, RX_CAP - c->rx_n, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            if (first || r == 0) {
                close(c->fd);
                c->fd = -1;
                return -1;
            }
            break;
        }
        if (r < 0)
            break;
        first = 0;
        c->rx_n += (size_t)r;
        if (rx_parse(c) < 0) {
            if (c->fd >= 0)
                close(c->fd);
            c->fd = -1;
            return -1;
        }
        if (c->rx_n == RX_CAP) {   /* a message bigger than the buffer: not ours */
            close(c->fd);
            c->fd = -1;
            return -1;
        }
    }
    return 1;
}

int mr_fd(const mr_client_t *c) { return c ? c->fd : -1; }
const mr_hello_t *mr_hello(const mr_client_t *c) { return c && c->have_hello ? &c->hello : NULL; }

void mr_close(mr_client_t *c)
{
    if (!c)
        return;
    if (c->fd >= 0)
        close(c->fd);
    free(c->rx);
    free(c);
}

/* ------------------------------------------------------------------ connecting --- */
static __thread char g_err[256];

const char *mr_last_error(void) { return g_err; }

/* why a try failed, one line per address tried */
static void note_err(const char *host, const char *why)
{
    const size_t n = strlen(g_err);
    snprintf(g_err + n, sizeof g_err - n, "%s%s: %s", n ? "; " : "", host, why);
}

static int connect_to(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char ps[16];
    int fd = -1, gai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if ((gai = getaddrinfo(host, ps, &hints, &res)) != 0) {
        note_err(host, gai_strerror(gai));
        return -1;
    }
    for (int pass = 0; pass < 2 && fd < 0; pass++)   /* IPv4 first: the device's addresses are IPv4 */
        for (ai = res; ai && fd < 0; ai = ai->ai_next) {
            int one = 1, err = 0;
            socklen_t el = sizeof err;
            struct pollfd pf;
            if ((pass == 0) != (ai->ai_family == AF_INET))
                continue;
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
                note_err(host, strerror(errno));
                close(fd), fd = -1;
                continue;
            }
            pf.fd = fd, pf.events = POLLOUT, pf.revents = 0;
            if (poll(&pf, 1, timeout_ms) != 1) {
                note_err(host, "timed out");
                close(fd), fd = -1;
                continue;
            }
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
                note_err(host, strerror(err ? err : errno));
                close(fd), fd = -1;
                continue;
            }
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        }
    freeaddrinfo(res);
    return fd;
}

static mr_client_t *try_host(const char *host, int port, const char *name, const mr_callbacks_t *cb, int timeout_ms)
{
    mr_client_t *c;
    uint8_t hello[34];
    const uint64_t until = mr_now_us() + (uint64_t)timeout_ms * 1000u;
    const int fd = connect_to(host, port, timeout_ms);
    if (fd < 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c || !(c->rx = malloc(RX_CAP))) {
        free(c);
        close(fd);
        return NULL;
    }
    c->fd = fd;
    if (cb)
        c->cb = *cb;
    memset(hello, 0, sizeof hello);
    put16(hello, MR_PROTO);
    snprintf((char *)hello + 2, 32, "%s", name ? name : "host");
    if (send_msg(c, MR_H_HELLO, hello, 2 + strlen((char *)hello + 2)) < 0) {
        mr_close(c);
        return NULL;
    }
    while (!c->have_hello) {
        const uint64_t t = mr_now_us();
        if (t >= until || mr_poll(c, (int)((until - t) / 1000u) + 1) < 0) {
            note_err(host, t >= until ? "connected, but no Move Remote HELLO" : "closed before HELLO");
            mr_close(c);
            return NULL;
        }
    }
    if (c->hello.proto != MR_PROTO) {
        char why[64];
        snprintf(why, sizeof why, "the device speaks protocol %u, this host %u", c->hello.proto, MR_PROTO);
        note_err(host, why);
        mr_close(c);
        return NULL;
    }
    return c;
}

mr_client_t *mr_connect(const char *host, int port, const char *name, const mr_callbacks_t *cb, int timeout_ms)
{
    mr_client_t *c;
    g_err[0] = 0;
    if (!port)
        port = MR_PORT;
    if (timeout_ms <= 0)
        timeout_ms = 2000;
    if (host)
        return try_host(host, port, name, cb, timeout_ms);
    if ((c = try_host(MR_USB_ADDR, port, name, cb, timeout_ms < 500 ? timeout_ms : 500)))
        return c;
    return try_host("move.local", port, name, cb, timeout_ms);
}

/* ------------------------------------------------------------------ pacing --- */
void mr_pacer_init(mr_pacer_t *p, int target_frames)
{
    p->target = target_frames < MR_BLOCK ? MR_BLOCK : target_frames;
    p->primed = 0;
    p->cool = 0;
}

/* steady state sends exactly what was consumed, so the queue holds still; it is nudged halfway back to the target
 * when it strays more than a block, then left alone for a few ticks while what is in flight lands */
int mr_pacer_frames(mr_pacer_t *p, int consumed, int out_fill)
{
    int want = consumed;
    if (!p->primed) {
        p->primed = 1;
        p->cool = 8;
        return consumed + p->target;
    }
    if (p->cool > 0)
        p->cool--;
    else if (out_fill < p->target - MR_BLOCK || out_fill > p->target + 2 * MR_BLOCK) {
        want += (p->target - out_fill) / 2;
        p->cool = 8;
    }
    return want < 0 ? 0 : want;
}
