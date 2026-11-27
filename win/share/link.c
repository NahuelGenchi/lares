// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The AWDL link on Windows (issue #109), in place of lares-awdl's main.c
 * and radio.c: one thread owns the adapter and the node. It waits on the
 * adapter's bulk transfers, a high-resolution waitable timer (Windows'
 * default tick, 15.6 ms, is five times AWDL's 3 TU guard) and packets from
 * the IP stack, and stamps each frame with the time the chip's TSF says it
 * arrived rather than when USB delivered it. */
#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <bcrypt.h>

#include "rtl8188eu.h"
#include "rtl_usb.h"

#define RX_XFERS        8
#define TX_XFERS        16
#define OUT_QUEUE       256
#define CALIBRATE_US    500000u     /* the TSF is mapped to the host clock this often */
#define STATS_US        10000000u
#define TSF_SLACK_US    50000u      /* a frame's TSF time is trusted this far before its arrival */

struct out_packet {
    awdl_mac dst, src;
    uint16_t len;
    uint8_t data[AWDL_PACKET_MAX];
};

struct link {
    struct link_config cfg;
    struct link_callbacks cb;
    HANDLE thread, stop, out_event, timer;
    SRWLOCK out_lock;
    struct out_packet *out;
    unsigned out_head, out_n;
    uint64_t out_dropped;

    struct rtl_usb *usb;
    struct rtl_chip chip;
    struct rtl_xfer rx[RX_XFERS], tx[TX_XFERS];
    struct awdl_node *node;
    bool failed;

    /* TSF to host time: a TSF reading and the host time it was taken at. */
    bool tsf_running;
    uint64_t cal_tsf;
    awdl_time cal_host, next_cal, next_stats;

    SRWLOCK counts_lock;
    struct link_counts counts;

    /* Frames for link_raw, sent from the link's thread. */
    uint8_t raw[8][512];
    size_t raw_len[8];
    unsigned raw_n;

    /* Pending ARPA TLV hostname update from another thread (under out_lock). */
    char pending_name[AWDL_NAME_MAX + 1];
    bool name_dirty;
};

/* ------------------------------------------------------------ time and log */

static LARGE_INTEGER qpf;

static awdl_time now_us(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    /* Whole seconds and the rest apart, so the product never overflows. */
    uint64_t s = (uint64_t)c.QuadPart / (uint64_t)qpf.QuadPart, r = (uint64_t)c.QuadPart % (uint64_t)qpf.QuadPart;
    return s * 1000000u + r * 1000000u / (uint64_t)qpf.QuadPart + 1u;
}

static FILE *log_file;
static awdl_time log_start;
static SRWLOCK log_lock = SRWLOCK_INIT;

void awdl_log(const char *fmt, ...)
{
    if (!log_file) return;
    AcquireSRWLockExclusive(&log_lock);
    awdl_time t = now_us() - log_start;
    fprintf(log_file, "%llu.%03llu ", (unsigned long long)(t / 1000000u), (unsigned long long)(t % 1000000u / 1000u));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
    ReleaseSRWLockExclusive(&log_lock);
}

/* %LOCALAPPDATA%\lares\lares-awdl.log, started afresh each session: what the
 * link did, with no names or addresses. */
static void open_log(void)
{
    if (log_file) return;
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 32) return;
    wcscat(dir, L"\\lares");
    CreateDirectoryW(dir, NULL);
    wcscat(dir, L"\\lares-awdl.log");
    log_file = _wfopen(dir, L"w");
    log_start = now_us();
}

void link_session_address(awdl_mac *m)
{
    if (BCryptGenRandom(NULL, m->a, 6, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        awdl_time t = now_us() ^ (awdl_time)GetCurrentProcessId() << 20;
        memcpy(m->a, &t, 6);
    }
    m->a[0] = (uint8_t)((m->a[0] & 0xfc) | 0x02);
}

/* ------------------------------------------------------------ TSF */

static void calibrate(struct link *l, awdl_time now)
{
    l->next_cal = now + CALIBRATE_US;
    awdl_time t0 = now_us();
    uint64_t tsf = 0;
    if (rtl_chip_tsf(&l->chip, &tsf) < 0) return;
    awdl_time t1 = now_us();
    if (t1 - t0 > 3000) return;     /* a slow read says little about when */
    l->tsf_running = l->cal_tsf && tsf != l->cal_tsf;
    l->cal_tsf = tsf;
    l->cal_host = t0 + (t1 - t0) / 2;
}

/* When a frame was received, from its descriptor's TSF; its arrival time if
 * the TSF can't say. */
static awdl_time frame_time(const struct link *l, uint32_t tsfl, awdl_time arrival)
{
    if (!l->tsf_running) return arrival;
    uint64_t base = l->cal_tsf, tsf = (base & ~0xffffffffull) | tsfl;
    int64_t d = (int64_t)(tsf - base);
    if (d > (int64_t)0x80000000) tsf -= 0x100000000ull;
    else if (d < -(int64_t)0x80000000) tsf += 0x100000000ull;
    awdl_time t = l->cal_host + (awdl_time)((int64_t)(tsf - base));
    if (t > arrival || arrival - t > TSF_SLACK_US) return arrival;
    return t;
}

/* ------------------------------------------------------------ the node's host */

static void count(struct link *l, void (*f)(struct link_counts *c, intptr_t v), intptr_t v)
{
    AcquireSRWLockExclusive(&l->counts_lock);
    f(&l->counts, v);
    ReleaseSRWLockExclusive(&l->counts_lock);
}

static void add_sent(struct link_counts *c, intptr_t v) { (void)v; c->tx_sent++; }
static void add_tx_failed(struct link_counts *c, intptr_t v) { (void)v; c->tx_failed++; }
static void add_raw(struct link_counts *c, intptr_t v) { (void)v; c->raw_sent++; }

static enum awdl_tx_result host_transmit(void *ctx, const struct awdl_tx *tx)
{
    struct link *l = (struct link *)ctx;
    if (l->failed) return AWDL_TX_DROPPED;
    struct rtl_xfer *x = NULL;
    for (unsigned i = 0; i < TX_XFERS && !x; i++)
        if (!l->tx[i].busy) x = &l->tx[i];
    if (!x) return AWDL_TX_FULL;
    struct rtl_tx_opts o = { 0 };
    o.ht = tx->rate.ht;
    o.sgi = tx->rate.sgi;
    o.rate = tx->rate.value;
    o.no_ack = tx->no_ack || awdl_mac_multicast(&tx->to);
    o.retries = 0;
    o.mgmt = tx->action;
    uint8_t ep = 0;
    x->len = rtl_tx_build(&l->chip, x->buf, x->cap, tx->bytes, tx->len, &o, &ep);
    if (!x->len || rtl_usb_write_start(l->usb, ep, x) < 0) {
        count(l, add_tx_failed, 0);
        return AWDL_TX_DROPPED;
    }
    return AWDL_TX_SENT;
}

static void host_peer_up(void *ctx, const struct awdl_peer *p)
{
    struct link *l = (struct link *)ctx;
    awdl_log("peer valid, device class %u, %u peers", p->devclass, (unsigned)l->node->npeers);
    if (l->cb.peer) l->cb.peer(l->cb.ctx, &p->mac, p->name, p->name_len, true);
}

static void host_peer_down(void *ctx, const struct awdl_peer *p)
{
    struct link *l = (struct link *)ctx;
    awdl_log("peer gone");
    if (l->cb.peer) l->cb.peer(l->cb.ctx, &p->mac, p->name, p->name_len, false);
}

static void host_deliver(void *ctx, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len)
{
    struct link *l = (struct link *)ctx;
    if (l->cb.deliver) l->cb.deliver(l->cb.ctx, dst, src, ip, len);
}

/* ------------------------------------------------------------ inputs */

struct rx_ctx {
    struct link *l;
    awdl_time arrival;
};

static void on_frame(void *ctx, const struct rtl_rx_frame *f)
{
    struct rx_ctx *r = (struct rx_ctx *)ctx;
    struct link *l = r->l;
    struct awdl_rx_frame af;
    int parsed = awdl_frame_parse(f->frame, f->len, &af);
    AcquireSRWLockExclusive(&l->counts_lock);
    l->counts.frames++;
    if (f->rate >= 4) l->counts.ofdm++;    /* DESC_RATE_6M and up */
    if (parsed < 0) l->counts.bad++;
    else if (af.kind == AWDL_RX_ACTION) l->counts.awdl_action++;
    else if (af.kind == AWDL_RX_DATA) l->counts.awdl_data++;
    if (f->has_rssi) l->counts.last_rssi = f->rssi;
    /* A probe response to our address: what --probe-test counts. */
    if (f->len >= 24 && f->frame[0] == 0x50 && memcmp(f->frame + 4, l->cfg.addr.a, 6) == 0) {
        l->counts.probe_resp++;
        if (f->frame[1] & 0x08) l->counts.probe_resp_retry++;
    }
    ReleaseSRWLockExclusive(&l->counts_lock);
    if (l->node && parsed == 0 && af.kind != AWDL_RX_OTHER)
        awdl_node_frame(l->node, frame_time(l, f->tsfl, r->arrival), f->has_rssi, f->rssi, f->frame, f->len);
}

static void reap(struct link *l)
{
    for (unsigned i = 0; i < RX_XFERS; i++) {
        struct rtl_xfer *x = &l->rx[i];
        if (!x->busy) {
            if (!l->failed) rtl_usb_read_start(l->usb, x);
            continue;
        }
        int r = rtl_usb_finish(l->usb, x);
        if (r == 0) continue;
        if (r < 0) {
            if (r == -2 && !l->failed) {
                l->failed = true;
                if (l->cb.failed) l->cb.failed(l->cb.ctx, "The Wi-Fi adapter was unplugged.");
            }
        } else if (x->len) {
            struct rx_ctx c = { l, now_us() };
            rtl_rx_parse(x->buf, x->len, on_frame, &c);
        }
        AcquireSRWLockExclusive(&l->counts_lock);
        l->counts.rx_xfers++;
        ReleaseSRWLockExclusive(&l->counts_lock);
        if (!l->failed && !x->busy) rtl_usb_read_start(l->usb, x);
    }
    for (unsigned i = 0; i < TX_XFERS; i++) {
        struct rtl_xfer *x = &l->tx[i];
        if (!x->busy) continue;
        int r = rtl_usb_finish(l->usb, x);
        if (r == 1) count(l, add_sent, 0);
        else if (r < 0) count(l, add_tx_failed, 0);
    }
}

/* Frames link_raw queued, sent as management frames at 6 Mbit/s, once. */
static void send_raw(struct link *l)
{
    for (;;) {
        uint8_t frame[512];
        size_t len = 0;
        AcquireSRWLockExclusive(&l->out_lock);
        if (l->raw_n) {
            len = l->raw_len[0];
            memcpy(frame, l->raw[0], len);
            l->raw_n--;
            memmove(l->raw[0], l->raw[1], sizeof l->raw[0] * l->raw_n);
            memmove(l->raw_len, l->raw_len + 1, sizeof l->raw_len[0] * l->raw_n);
        }
        ReleaseSRWLockExclusive(&l->out_lock);
        if (!len) return;
        struct awdl_tx tx = { frame, len, AWDL_BROADCAST, { false, false, 12 }, true, true };
        if (host_transmit(l, &tx) == AWDL_TX_SENT) count(l, add_raw, 0);
    }
}

void link_raw(struct link *l, const uint8_t *frame, size_t len)
{
    if (!l || len > sizeof l->raw[0]) return;
    AcquireSRWLockExclusive(&l->out_lock);
    if (l->raw_n < 8) {
        memcpy(l->raw[l->raw_n], frame, len);
        l->raw_len[l->raw_n++] = len;
    }
    ReleaseSRWLockExclusive(&l->out_lock);
    SetEvent(l->out_event);
}

static void drain_outbound(struct link *l)
{
    static struct out_packet p;     /* the link's thread's only */
    for (unsigned n = 0; n < AWDL_BURST && awdl_node_takes_outbound(l->node); n++) {
        AcquireSRWLockExclusive(&l->out_lock);
        bool have = l->out_n > 0;
        if (have) {
            const struct out_packet *q = &l->out[l->out_head];
            p.dst = q->dst;
            p.src = q->src;
            p.len = q->len;
            memcpy(p.data, q->data, q->len);
            l->out_head = (l->out_head + 1) % OUT_QUEUE;
            l->out_n--;
        }
        ReleaseSRWLockExclusive(&l->out_lock);
        if (!have) return;
        awdl_node_outbound(l->node, now_us(), &p.dst, &p.src, p.data, p.len);
    }
}

void link_send(struct link *l, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len)
{
    if (!l || len > AWDL_PACKET_MAX) return;
    AcquireSRWLockExclusive(&l->out_lock);
    if (l->out_n == OUT_QUEUE) {
        l->out_dropped++;
    } else {
        struct out_packet *p = &l->out[(l->out_head + l->out_n) % OUT_QUEUE];
        p->dst = *dst;
        p->src = *src;
        p->len = (uint16_t)len;
        memcpy(p->data, ip, len);
        l->out_n++;
    }
    ReleaseSRWLockExclusive(&l->out_lock);
    SetEvent(l->out_event);
}

static void apply_pending_name(struct link *l)
{
    char next[AWDL_NAME_MAX + 1];
    bool dirty = false;
    AcquireSRWLockExclusive(&l->out_lock);
    if (l->name_dirty) {
        memcpy(next, l->pending_name, sizeof next);
        l->name_dirty = false;
        dirty = true;
    }
    ReleaseSRWLockExclusive(&l->out_lock);
    if (!dirty) return;
    snprintf(l->cfg.name, sizeof l->cfg.name, "%s", next);
    if (l->node) awdl_node_set_name(l->node, l->cfg.name);
}

void link_set_name(struct link *l, const char *name)
{
    if (!l || !name) return;
    AcquireSRWLockExclusive(&l->out_lock);
    snprintf(l->pending_name, sizeof l->pending_name, "%s", name);
    l->name_dirty = true;
    ReleaseSRWLockExclusive(&l->out_lock);
    SetEvent(l->out_event);
}

static void log_stats(struct link *l)
{
    struct link_counts c;
    link_counts(l, &c);
    if (l->node) {
        const struct awdl_stats *s = &l->node->stats;
        awdl_log("STATS peers %zu | rx frames %llu action %llu data %llu | tx action %llu unicast %llu multicast %llu "
                 "sent %llu failed %llu full %llu | delivered %llu | dropped no peer %llu queue %llu expired %llu | "
                 "tsf %s",
                 l->node->npeers, (unsigned long long)c.frames, (unsigned long long)s->rx_action,
                 (unsigned long long)s->rx_data, (unsigned long long)s->tx_action,
                 (unsigned long long)s->tx_unicast, (unsigned long long)s->tx_multicast,
                 (unsigned long long)c.tx_sent, (unsigned long long)c.tx_failed,
                 (unsigned long long)s->radio_full, (unsigned long long)s->delivered,
                 (unsigned long long)s->dropped_no_peer, (unsigned long long)s->dropped_queue_full,
                 (unsigned long long)s->dropped_expired, l->tsf_running ? "on" : "off");
    } else {
        awdl_log("STATS rx frames %llu awdl action %llu data %llu | tsf %s", (unsigned long long)c.frames,
                 (unsigned long long)c.awdl_action, (unsigned long long)c.awdl_data, l->tsf_running ? "on" : "off");
    }
}

/* ------------------------------------------------------------ the thread */

static void arm(struct link *l, awdl_time now)
{
    awdl_time t = l->next_cal < l->next_stats ? l->next_cal : l->next_stats;
    if (l->node) {
        awdl_time d = awdl_node_deadline(l->node, now);
        if (d < t) t = d;
    }
    LARGE_INTEGER due;
    /* Relative, in 100 ns units; at least one. */
    due.QuadPart = t > now ? -(LONGLONG)((t - now) * 10u) : -1;
    SetWaitableTimerEx(l->timer, &due, 0, NULL, NULL, NULL, 0);
}

static int io_read(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
    return rtl_usb_read((struct rtl_usb *)ctx, addr, buf, len);
}

static int io_write(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
    return rtl_usb_write((struct rtl_usb *)ctx, addr, buf, len);
}

static bool bring_up(struct link *l, char *err, size_t errlen)
{
    l->usb = rtl_usb_open(err, errlen);
    if (!l->usb) return false;
    uint8_t eps[RTL_USB_MAX_OUT];
    unsigned n = rtl_usb_out_count(l->usb);
    for (unsigned i = 0; i < n; i++) eps[i] = rtl_usb_out_ep(l->usb, i);
    struct rtl_io io = { l->usb, io_read, io_write };
    if (rtl_chip_up(&l->chip, &io, eps, n, l->cfg.firmware, l->cfg.firmware_len, l->cfg.channel, l->cfg.addr.a, err,
                    errlen) < 0)
        return false;
    for (unsigned i = 0; i < RX_XFERS; i++)
        if (rtl_xfer_init(&l->rx[i], RTL_RX_XFER) < 0 || rtl_usb_read_start(l->usb, &l->rx[i]) < 0) {
            snprintf(err, errlen, "Couldn't start reading from the Wi-Fi adapter.");
            return false;
        }
    for (unsigned i = 0; i < TX_XFERS; i++)
        if (rtl_xfer_init(&l->tx[i], RTL_TX_MAX) < 0) {
            snprintf(err, errlen, "Out of memory.");
            return false;
        }
    return true;
}

static void start_node(struct link *l)
{
    struct awdl_config cfg;
    awdl_config_init(&cfg, &l->cfg.addr, l->cfg.name, l->cfg.channel, AWDL_PRESENCE_FIXED);
    cfg.accept_addba = false;
    struct awdl_host host = { l, host_transmit, host_peer_up, host_peer_down, host_deliver };
    awdl_node_init(l->node, &cfg, &host, now_us());
}

static void shut_down(struct link *l)
{
    if (l->usb) {
        rtl_usb_abort(l->usb);
        struct rtl_xfer *all[RX_XFERS + TX_XFERS];
        for (unsigned i = 0; i < RX_XFERS; i++) all[i] = &l->rx[i];
        for (unsigned i = 0; i < TX_XFERS; i++) all[RX_XFERS + i] = &l->tx[i];
        for (unsigned i = 0; i < RX_XFERS + TX_XFERS; i++) {
            if (all[i]->busy && all[i]->event) WaitForSingleObject((HANDLE)all[i]->event, 1000);
            if (all[i]->busy) rtl_usb_finish(l->usb, all[i]);
        }
        if (l->chip.up) rtl_chip_down(&l->chip);
    }
    for (unsigned i = 0; i < RX_XFERS; i++)
        if (l->rx[i].buf) rtl_xfer_free(&l->rx[i]);
    for (unsigned i = 0; i < TX_XFERS; i++)
        if (l->tx[i].buf) rtl_xfer_free(&l->tx[i]);
    rtl_usb_close(l->usb);
    l->usb = NULL;
}

static DWORD WINAPI link_main(LPVOID p)
{
    struct link *l = (struct link *)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    open_log();
    char err[512] = "";
    awdl_log("link: starting on channel %u", l->cfg.channel);
    if (!bring_up(l, err, sizeof err)) {
        awdl_log("link: %s", err);
        if (l->cb.failed) l->cb.failed(l->cb.ctx, err);
        shut_down(l);
        return 1;
    }
    awdl_time now = now_us();
    calibrate(l, now);
    l->next_stats = now + STATS_US;
    apply_pending_name(l);
    if (l->node) start_node(l);
    awdl_log("link: up");
    if (l->cb.ready) l->cb.ready(l->cb.ctx, l->cfg.channel, &l->cfg.addr);

    while (!l->failed) {
        now = now_us();
        apply_pending_name(l);
        send_raw(l);
        if (l->node) {
            drain_outbound(l);
            awdl_node_timer(l->node, now);
        }
        if (now >= l->next_cal) calibrate(l, now);
        if (now >= l->next_stats) {
            log_stats(l);
            l->next_stats = now + STATS_US;
        }
        arm(l, now_us());

        HANDLE h[3 + RX_XFERS + TX_XFERS];
        DWORD n = 0;
        h[n++] = l->stop;
        h[n++] = l->timer;
        h[n++] = l->out_event;
        for (unsigned i = 0; i < RX_XFERS; i++)
            if (l->rx[i].busy) h[n++] = (HANDLE)l->rx[i].event;
        for (unsigned i = 0; i < TX_XFERS; i++)
            if (l->tx[i].busy) h[n++] = (HANDLE)l->tx[i].event;
        DWORD w = WaitForMultipleObjects(n, h, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_FAILED) {
            snprintf(err, sizeof err, "The link's event loop failed.");
            if (l->cb.failed) l->cb.failed(l->cb.ctx, err);
            break;
        }
        reap(l);
    }
    log_stats(l);
    awdl_log("link: stopping");
    shut_down(l);
    return 0;
}

struct link *link_start(const struct link_config *cfg, const struct link_callbacks *cb)
{
    struct link *l = (struct link *)calloc(1, sizeof *l);
    if (!l) return NULL;
    l->cfg = *cfg;
    if (cb) l->cb = *cb;
    InitializeSRWLock(&l->out_lock);
    InitializeSRWLock(&l->counts_lock);
    l->out = (struct out_packet *)calloc(OUT_QUEUE, sizeof *l->out);
    if (!cfg->listen_only) l->node = (struct awdl_node *)calloc(1, sizeof *l->node);
    l->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    l->out_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    l->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!l->timer) l->timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    if (!l->out || (!cfg->listen_only && !l->node) || !l->stop || !l->out_event || !l->timer) {
        link_stop(l);
        return NULL;
    }
    l->thread = CreateThread(NULL, 0, link_main, l, 0, NULL);
    if (!l->thread) {
        link_stop(l);
        return NULL;
    }
    return l;
}

void link_stop(struct link *l)
{
    if (!l) return;
    if (l->thread) {
        SetEvent(l->stop);
        WaitForSingleObject(l->thread, INFINITE);
        CloseHandle(l->thread);
    }
    if (l->stop) CloseHandle(l->stop);
    if (l->out_event) CloseHandle(l->out_event);
    if (l->timer) CloseHandle(l->timer);
    free(l->node);
    free(l->out);
    free(l);
}

/* The MAC's receive counters (REG_RXERR_RPT 0x0664, a type in bits 28-31)
 * and a few registers, for --listen: whether the radio hears and where
 * frames stop. */
void link_dump(struct link *l, FILE *out)
{
    static const char *const names[16] = { "ofdm ppdu", "ofdm false alarm", "ofdm ok", "ofdm fail", "cck ppdu",
                                           "cck false alarm", "cck ok", "cck fail", "ht ppdu", "ht false alarm",
                                           "ht total", "ht ok", "ht fail", "13", "14", "rx full drop" };
    if (!l->usb) return;
    for (uint32_t t = 0; t < 16; t++) {
        if (t == 13 || t == 14) continue;
        uint32_t sel = t << 28, v = 0;
        rtl_usb_write(l->usb, 0x0664, &sel, 4);
        rtl_usb_read(l->usb, 0x0664, &v, 4);
        fprintf(out, "  %-16s %u\n", names[t], v & 0xffff);
    }
    fprintf(out, "  rf:");
    for (uint8_t r = 0; r < 0x60; r++) fprintf(out, "%s%02x=%05x", r % 8 ? " " : "\n   ", r, rtl_chip_rf_read(&l->chip, r));
    fprintf(out, "\n");
    static const uint16_t regs[] = { 0x0002, 0x001c, 0x0040, 0x0044, 0x0064, 0x0100, 0x0104, 0x010c, 0x0280,
                                     0x0284, 0x0288, 0x0608, 0x0610, 0x06a0, 0x0800, 0x0820, 0x0870, 0x0c50 };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        uint32_t v = 0;
        rtl_usb_read(l->usb, regs[i], &v, 4);
        fprintf(out, "  reg %04x %08x\n", regs[i], v);
    }
}

void link_counts(struct link *l, struct link_counts *out)
{
    AcquireSRWLockExclusive(&l->counts_lock);
    *out = l->counts;
    out->tsf_running = l->tsf_running;
    ReleaseSRWLockExclusive(&l->counts_lock);
}
