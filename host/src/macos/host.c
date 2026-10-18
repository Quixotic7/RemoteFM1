/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 RemoteFM1 contributors */
/* The macOS host glue: the receive queue, candidate links from CoreMIDI and libusb into a session. See rfm/macos.h. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "mac_priv.h"
#include "rfm/macos.h"

/* ---------------------------------------------------------------- the receive queue ---- */
int rxq_init(rxq_t *q, uint32_t cap)
{
    memset(q, 0, sizeof *q);
    q->buf = (uint8_t *)malloc(cap);
    if (!q->buf)
        return -1;
    q->cap = cap;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    return 0;
}

void rxq_free(rxq_t *q)
{
    if (!q->buf)
        return;
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
    free(q->buf);
    q->buf = NULL;
}

void rxq_push(rxq_t *q, uint16_t tag, const uint8_t *p, uint32_t n)
{
    pthread_mutex_lock(&q->mu);
    while (n) {
        uint32_t c = n > 0xFFFFu ? 0xFFFFu : n;
        if (q->len + 4u + c > q->cap) {
            q->dropped += n;                            /* nobody is pumping: lost, as a full FIFO would */
            break;
        }
        q->buf[q->len] = (uint8_t)tag;
        q->buf[q->len + 1] = (uint8_t)(tag >> 8);
        q->buf[q->len + 2] = (uint8_t)c;
        q->buf[q->len + 3] = (uint8_t)(c >> 8);
        memcpy(q->buf + q->len + 4, p, c);
        q->len += 4u + c;
        q->total += c;
        p += c;
        n -= c;
    }
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

uint32_t rxq_take(rxq_t *q, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    pthread_mutex_lock(&q->mu);
    while (n < q->len) {                                /* whole records only */
        uint32_t len = 4u + (q->buf[n + 2] | (uint32_t)q->buf[n + 3] << 8);
        if (n + len > cap)
            break;
        n += len;
    }
    memcpy(out, q->buf, n);
    memmove(q->buf, q->buf + n, q->len - n);
    q->len -= n;
    pthread_mutex_unlock(&q->mu);
    return n;
}

void rxq_wait(rxq_t *q, uint32_t ms)
{
    struct timeval tv;
    struct timespec ts;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + (time_t)(ms / 1000u);
    ts.tv_nsec = (long)tv.tv_usec * 1000L + (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&q->mu);
    if (!q->len)
        pthread_cond_timedwait(&q->cv, &q->mu, &ts);
    pthread_mutex_unlock(&q->mu);
}

/* ---------------------------------------------------------------- links ---- */
typedef struct ml {
    int used, kind, sid, seen;
    uint16_t gen;
    uint32_t src, dst;
    int32_t uid;
    usbl_t *usb;
    struct rfm_mac *m;
    char name[64];
} ml_t;

struct rfm_mac {
    rfm_mac_cfg_t cfg;
    char vname[64], filter[64];
    rfm_session_t *s;
    cm_t *cm;
    rxq_t q;
    ml_t l[RFM_MAX_LINKS];
    uint8_t *tmp;
    uint32_t now, usb_tries;
    rfm_mac_stats_t st;
    char last_usb_err[160];
};
#define TMP_CAP (1u << 20)

static void mlog(rfm_mac_t *m, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    if (!m->cfg.log)
        return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    m->cfg.log(m->cfg.log_ctx, b);
}

static uint16_t tag_of(const ml_t *l, int slot) { return (uint16_t)((unsigned)slot | (unsigned)l->gen << 4); }

static int w_link(void *ctx, const uint8_t *p, uint32_t n)
{
    ml_t *l = (ml_t *)ctx;
    if (l->kind == RFM_LINK_SX)
        return cm_send(l->m->cm, l->dst, p, n);
    return usb_write(l->usb, p, n);
}

static int free_slot(rfm_mac_t *m)
{
    int i;
    for (i = 0; i < RFM_MAX_LINKS; i++)
        if (!m->l[i].used)
            return i;
    return -1;
}

static void drop_link(rfm_mac_t *m, int i)
{
    ml_t *l = &m->l[i];
    if (!l->used)
        return;
    mlog(m, "link gone: %s", l->name);
    if (m->s)
        rfm_session_remove_link(m->s, l->sid, m->now);
    if (l->kind == RFM_LINK_SX) {
        cm_disconnect(m->cm, l->src);
        m->st.midi_links--;
    } else {
        usb_close(l->usb);
        l->usb = NULL;
        m->st.bulk_links--;
    }
    l->used = 0;
    l->gen++;
}

rfm_mac_t *rfm_mac_new(const rfm_mac_cfg_t *cfg, char *err, size_t errn)
{
    rfm_mac_t *m = (rfm_mac_t *)calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->cfg = *cfg;
    snprintf(m->vname, sizeof m->vname, "%s", cfg->virtual_name ? cfg->virtual_name : "");
    snprintf(m->filter, sizeof m->filter, "%s", cfg->port_filter ? cfg->port_filter : "");
    m->cfg.virtual_name = m->vname;
    m->cfg.port_filter = m->filter;
    if (!m->cfg.allow_devices && !m->vname[0])
        m->cfg.use_bulk = 0;
    m->tmp = (uint8_t *)malloc(TMP_CAP);
    if (!m->tmp || rxq_init(&m->q, TMP_CAP)) {
        snprintf(err, errn, "out of memory");
        rfm_mac_free(m);
        return NULL;
    }
    if (m->cfg.use_midi) {
        m->cm = cm_open(&m->q, m->cfg.allow_devices && m->cfg.midi_restart, err, errn);
        if (!m->cm) {
            rfm_mac_free(m);
            return NULL;
        }
    }
    return m;
}

void rfm_mac_rescan(rfm_mac_t *m)
{
    int i, k, n;
    cm_cand_t c[RFM_MAX_LINKS];
    if (!m->s)
        return;
    if (m->cm) {
        n = cm_scan(m->cm, c, RFM_MAX_LINKS, m->cfg.allow_devices, m->vname, m->filter);
        for (i = 0; i < RFM_MAX_LINKS; i++)
            m->l[i].seen = 0;
        for (k = 0; k < n; k++) {
            int found = 0, slot;
            ml_t *l;
            for (i = 0; i < RFM_MAX_LINKS && !found; i++)
                if (m->l[i].used && m->l[i].kind == RFM_LINK_SX && m->l[i].uid == c[k].uid &&
                    m->l[i].src == c[k].src && m->l[i].dst == c[k].dst) {
                    m->l[i].seen = 1;
                    found = 1;
                }
            if (found || (slot = free_slot(m)) < 0)
                continue;
            l = &m->l[slot];
            l->used = l->seen = 1;
            l->kind = RFM_LINK_SX;
            l->src = c[k].src;
            l->dst = c[k].dst;
            l->uid = c[k].uid;
            l->m = m;
            snprintf(l->name, sizeof l->name, "midi \"%s\"", c[k].name);
            if (cm_connect(m->cm, l->src, tag_of(l, slot))) {
                l->used = 0;
                continue;
            }
            l->sid = rfm_session_add_link(m->s, RFM_LINK_SX, w_link, l, l->name);
            if (l->sid < 0) {
                cm_disconnect(m->cm, l->src);
                l->used = 0;
                continue;
            }
            m->st.midi_links++;
            mlog(m, "candidate %s", l->name);
        }
        for (i = 0; i < RFM_MAX_LINKS; i++)
            if (m->l[i].used && m->l[i].kind == RFM_LINK_SX && !m->l[i].seen)
                drop_link(m, i);
    }
    if (m->cfg.use_bulk && m->cfg.allow_devices && !m->st.bulk_links) {
        char err[160];
        int slot = free_slot(m);
        if (slot >= 0) {
            ml_t *l = &m->l[slot];
            l->usb = usb_open_unit(&m->q, tag_of(l, slot), err, sizeof err);
            if (l->usb) {
                l->used = 1;
                l->kind = RFM_LINK_BK;
                l->m = m;
                snprintf(l->name, sizeof l->name, "%s", usb_name(l->usb));
                l->sid = rfm_session_add_link(m->s, RFM_LINK_BK, w_link, l, l->name);
                m->st.bulk_links++;
                mlog(m, "candidate %s", l->name);
            } else if (strcmp(err, m->last_usb_err)) {
                mlog(m, "vendor bulk: %s", err);
                snprintf(m->last_usb_err, sizeof m->last_usb_err, "%s", err);
            }
        }
    }
}

void rfm_mac_attach(rfm_mac_t *m, rfm_session_t *s)
{
    m->s = s;
    rfm_mac_rescan(m);
}

void rfm_mac_pump(rfm_mac_t *m, uint32_t now)
{
    uint32_t n, o = 0;
    int i;
    m->now = now;
    n = rxq_take(&m->q, m->tmp, TMP_CAP);
    while (o + 4u <= n) {
        uint16_t tag = (uint16_t)(m->tmp[o] | m->tmp[o + 1] << 8);
        uint32_t len = m->tmp[o + 2] | (uint32_t)m->tmp[o + 3] << 8;
        int slot = tag & 15;
        const ml_t *l = &m->l[slot];
        if (l->used && tag_of(l, slot) == tag && m->s)
            rfm_session_rx(m->s, l->sid, m->tmp + o + 4, len, now);
        m->st.rx_bytes += len;
        o += 4u + len;
    }
    for (i = 0; i < RFM_MAX_LINKS; i++)
        if (m->l[i].used && m->l[i].kind == RFM_LINK_BK && usb_dead(m->l[i].usb)) {
            m->st.usb_errors += usb_errors(m->l[i].usb);
            drop_link(m, i);
        }
}

void rfm_mac_wait(rfm_mac_t *m, uint32_t ms) { rxq_wait(&m->q, ms); }

void rfm_mac_stats(rfm_mac_t *m, rfm_mac_stats_t *st)
{
    *st = m->st;
    pthread_mutex_lock(&m->q.mu);
    st->rx_dropped = m->q.dropped;
    pthread_mutex_unlock(&m->q.mu);
}

void rfm_mac_free(rfm_mac_t *m)
{
    int i;
    if (!m)
        return;
    for (i = 0; i < RFM_MAX_LINKS; i++)
        if (m->l[i].used) {
            m->s = NULL;                                /* (the session may be gone already) */
            drop_link(m, i);
        }
    cm_close(m->cm);
    rxq_free(&m->q);
    free(m->tmp);
    free(m);
}
