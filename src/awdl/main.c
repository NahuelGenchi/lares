// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-awdl's command line and event loop. It runs the AWDL node (node.c)
 * between the card's monitor interface and the TAP awdl0, as owly-awdl's
 * driver.rs and owly-radio's native.rs run it, on one epoll loop: a timerfd
 * armed at the node's next deadline with an absolute time, so PSFs, MIFs
 * and data line up with the availability windows; a signalfd; stdin, whose
 * end means Lares is gone; the monitor's packet socket and the TAP. It
 * wakes for nothing else. Lares reads one JSON object per line on stdout;
 * diagnostics go to stderr.
 *
 *   lares-awdl --wifi IFNAME [--channel 6|44|149] [--name HOSTNAME]
 *   lares-awdl --restore
 *
 * `--test-tap` (not for Lares) runs the node on awdl0 with no card, for a
 * check inside an unprivileged network namespace (`unshare -Urn`): it makes
 * the TAP as a session would, takes radiotap frames as hex lines on stdin
 * ("rx <hex>"), sends nothing, and keeps no journal. */
#define _GNU_SOURCE /* pipe2, the capability and prctl calls */
#include "awdl.h"
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/capability.h>

#define TAP_NAME        "awdl0"
#define EXIT_OK         0
#define EXIT_USAGE      2
#define EXIT_UNUSABLE   3
#define READS_PER_WAKE  64      /* frames or packets read for one wake-up, for fairness */
#define STDIN_LINE_MAX  4096    /* --test-tap: one hex frame */

static const char USAGE[] =
    "Usage: lares-awdl --wifi IFNAME [--channel 6|44|149] [--name HOSTNAME]\n"
    "       lares-awdl --restore\n"
    "Runs Apple Wireless Direct Link on a Wi-Fi card for Lares' AirDrop, with awdl0\n"
    "as its network interface, until stdin ends or a signal comes. --restore puts a\n"
    "card that a crashed run left back as it was.\n";

/* ---------------------------------------------------------- diagnostics and events */

void awdl_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("lares-awdl: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void emit(const char *line)
{
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void event_error(const char *message)
{
    char quoted[1024], line[1100];
    if (!awdl_json_string(quoted, sizeof quoted, (const uint8_t *)message, strlen(message)))
        snprintf(quoted, sizeof quoted, "\"lares-awdl failed\"");
    snprintf(line, sizeof line, "{\"event\":\"error\",\"message\":%s}", quoted);
    emit(line);
    awdl_log("%s", message);
}

static void event_ready(uint8_t channel, const awdl_mac *addr)
{
    uint8_t ll[16];
    char text[INET6_ADDRSTRLEN], line[160];
    awdl_link_local(addr, ll);
    inet_ntop(AF_INET6, ll, text, sizeof text);
    snprintf(line, sizeof line, "{\"event\":\"ready\",\"interface\":\"%s\",\"channel\":%u,\"address\":\"%s\"}",
             TAP_NAME, channel, text);
    emit(line);
}

static void event_peer(const awdl_mac *mac, const uint8_t *name, size_t len, const char *state)
{
    char addr[18], quoted[AWDL_NAME_MAX * 6 + 4], line[sizeof quoted + 96];
    awdl_mac_format(mac, addr);
    if (!awdl_json_string(quoted, sizeof quoted, name, len))
        snprintf(quoted, sizeof quoted, "\"\"");
    snprintf(line, sizeof line, "{\"event\":\"peer\",\"address\":\"%s\",\"name\":%s,\"state\":\"%s\"}", addr,
             quoted, state);
    emit(line);
}

/* ---------------------------------------------------------- time */

static awdl_time now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (awdl_time)ts.tv_sec * 1000000u + (awdl_time)ts.tv_nsec / 1000u + 1u;
}

/* ---------------------------------------------------------- the loop's state */

static struct {
    struct awdl_node node;      /* static: its queues are touched only when used */
    int epoll_fd, sig_fd, timer_fd, mon_fd, tap_fd, tap_ifindex;
    bool tap_watched, stdin_watched, stop, failed, test;
    awdl_time armed, next_stats;
    uint64_t late_count, late_total, late_max, tx_errors;
    uint8_t rx[AWDL_RX_BUF];
    char line[STDIN_LINE_MAX];
    size_t line_len;
} L; /* zeroed (in .bss, not the file); main sets the descriptors to -1 */

static void read_stdin(void);

/* During setup: waits up to `ms`, cut short by a signal or by stdin
 * ending, either of which asks for a stop. */
bool awdl_wait_ms(unsigned ms)
{
    struct pollfd p[2] = { { L.sig_fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!L.stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long spent = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (spent >= (long)ms)
            return true;
        int n = poll(p, L.test ? 1 : 2, (int)((long)ms - spent));
        if (n < 0 && errno != EINTR)
            return true;
        if (n <= 0)
            continue;
        if (p[0].revents) {
            struct signalfd_siginfo si;
            if (read(L.sig_fd, &si, sizeof si) > 0)
                L.stop = true;
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR))
            read_stdin();
        if (p[1].revents & POLLNVAL)
            p[1].fd = -1;
    }
    return false;
}

/* ---------------------------------------------------------- the node's host */

static enum awdl_tx_result host_transmit(void *ctx, const struct awdl_tx *tx)
{
    if (L.test)
        return AWDL_TX_SENT;
    uint8_t rt[AWDL_RADIOTAP_MAX];
    size_t rtlen = awdl_radiotap_tx(rt, tx->rate, tx->no_ack);
    struct iovec iov[2] = { { rt, rtlen }, { (void *)tx->bytes, tx->len } };
    if (writev(L.mon_fd, iov, 2) >= 0)
        return AWDL_TX_SENT;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
        return AWDL_TX_FULL;
    if (errno == ENETDOWN || errno == ENXIO || errno == ENODEV) {
        /* A driver that reloads takes the monitor with it. */
        if (!L.failed)
            event_error("The Wi-Fi card's monitor interface went away.");
        L.failed = L.stop = true;
        return AWDL_TX_DROPPED;
    }
    L.tx_errors++;
    return AWDL_TX_DROPPED;
}

static void host_peer_up(void *ctx, const struct awdl_peer *p)
{
    int r = awdl_neigh_add(L.tap_ifindex, &p->mac);
    if (r < 0)
        awdl_log("no neighbour entry for a peer: %s", strerror(-r));
    event_peer(&p->mac, p->name, p->name_len, "valid");
}

static void host_peer_down(void *ctx, const struct awdl_peer *p)
{
    awdl_neigh_del(L.tap_ifindex, &p->mac);
    event_peer(&p->mac, p->name, p->name_len, "gone");
}

static void host_deliver(void *ctx, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len)
{
    uint8_t eth[14];
    memcpy(eth, dst->a, 6);
    memcpy(eth + 6, src->a, 6);
    eth[12] = AWDL_ETHERTYPE_IPV6 >> 8;
    eth[13] = AWDL_ETHERTYPE_IPV6 & 0xff;
    struct iovec iov[2] = { { eth, sizeof eth }, { (void *)ip, len } };
    /* One the host can't take now is dropped, as a full queue would. */
    if (writev(L.tap_fd, iov, 2) < 0 && errno != EAGAIN)
        L.tx_errors++;
}

/* ---------------------------------------------------------- inputs */

static void read_monitor(void)
{
    for (int i = 0; i < READS_PER_WAKE; i++) {
        ssize_t n = recv(L.mon_fd, L.rx, sizeof L.rx, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (!L.failed)
                    event_error("The Wi-Fi card's monitor interface went away.");
                L.failed = L.stop = true;
            }
            return;
        }
        struct awdl_rx rx;
        if (awdl_radiotap_parse(L.rx, (size_t)n, &rx) == 0)
            awdl_node_frame(&L.node, now_us(), rx.has_rssi, rx.rssi, rx.frame, rx.len);
    }
}

static void read_tap(void)
{
    for (int i = 0; i < READS_PER_WAKE && awdl_node_takes_outbound(&L.node); i++) {
        ssize_t n = read(L.tap_fd, L.rx, sizeof L.rx);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                event_error("awdl0 failed.");
                L.failed = L.stop = true;
            }
            return;
        }
        /* NET-3: IPv6 only. */
        if (n < 14 + (ssize_t)AWDL_IPV6_HEADER_LEN || L.rx[12] != 0x86 || L.rx[13] != 0xdd)
            continue;
        awdl_mac dst, src;
        memcpy(dst.a, L.rx, 6);
        memcpy(src.a, L.rx + 6, 6);
        awdl_node_outbound(&L.node, now_us(), &dst, &src, L.rx + 14, (size_t)n - 14);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* --test-tap: "rx <hex>" feeds a radiotap frame to the node as heard. */
static void test_line(const char *s)
{
    if (strncmp(s, "rx ", 3) != 0)
        return;
    s += 3;
    size_t n = 0;
    while (s[0] && s[1] && n < sizeof L.rx) {
        int hi = hex_value(s[0]), lo = hex_value(s[1]);
        if (hi < 0 || lo < 0)
            return;
        L.rx[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    struct awdl_rx rx;
    if (awdl_radiotap_parse(L.rx, n, &rx) == 0)
        awdl_node_frame(&L.node, now_us(), rx.has_rssi, rx.rssi, rx.frame, rx.len);
}

/* Stdin carries nothing for lares-awdl; its end means Lares is gone. */
static void read_stdin(void)
{
    char buf[1024];
    ssize_t n = read(0, buf, sizeof buf);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        L.stop = true;
        return;
    }
    if (!L.test)
        return;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            L.line[L.line_len] = 0;
            test_line(L.line);
            L.line_len = 0;
        } else if (L.line_len + 1 < sizeof L.line) {
            L.line[L.line_len++] = buf[i];
        }
    }
}

static void log_stats(void)
{
    const struct awdl_stats *s = &L.node.stats;
    awdl_log("STATS peers %zu | tx action %llu unicast %llu multicast %llu | radio full %llu gave up %llu "
             "errors %llu | rx action %llu data %llu malformed %llu delivered %llu | dropped no peer %llu "
             "queue %llu expired %llu | syn held %llu released %llu | timer late avg %llu max %llu us",
             L.node.npeers, (unsigned long long)s->tx_action, (unsigned long long)s->tx_unicast,
             (unsigned long long)s->tx_multicast, (unsigned long long)s->radio_full,
             (unsigned long long)s->radio_gave_up, (unsigned long long)L.tx_errors,
             (unsigned long long)s->rx_action, (unsigned long long)s->rx_data,
             (unsigned long long)s->rx_malformed, (unsigned long long)s->delivered,
             (unsigned long long)s->dropped_no_peer, (unsigned long long)s->dropped_queue_full,
             (unsigned long long)s->dropped_expired, (unsigned long long)s->syn_held,
             (unsigned long long)s->syn_released,
             (unsigned long long)(L.late_count ? L.late_total / L.late_count : 0),
             (unsigned long long)L.late_max);
    L.late_count = L.late_total = L.late_max = 0;
}

static void on_timer(void)
{
    uint64_t expirations;
    if (read(L.timer_fd, &expirations, sizeof expirations) < 0 && errno == EAGAIN)
        return;
    awdl_time now = now_us();
    if (now >= L.armed) {
        uint64_t late = now - L.armed;
        L.late_count++;
        L.late_total += late;
        if (late > L.late_max)
            L.late_max = late;
    }
    awdl_node_timer(&L.node, now);
    if (now >= L.next_stats) {
        log_stats();
        L.next_stats = now + AWDL_STATS_US;
    }
}

static void watch(int fd, bool on, bool *watched)
{
    if (on == *watched)
        return;
    struct epoll_event ev = { .events = on ? EPOLLIN : 0, .data.fd = fd };
    epoll_ctl(L.epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    *watched = on;
}

/* Arms the timer at the node's next deadline, an absolute time. */
static void arm(void)
{
    awdl_time now = now_us();
    awdl_time t = awdl_node_deadline(&L.node, now);
    if (L.next_stats && L.next_stats < t)
        t = L.next_stats;
    L.armed = t;
    awdl_time at = t - 1; /* now_us counts from 1 */
    struct itimerspec its = { .it_value = { (time_t)(at / 1000000u), (long)(at % 1000000u) * 1000L } };
    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
        its.it_value.tv_nsec = 1;
    timerfd_settime(L.timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static int add_fd(int fd)
{
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    return epoll_ctl(L.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static void loop(void)
{
    L.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    L.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    add_fd(L.sig_fd);
    add_fd(L.timer_fd);
    if (L.mon_fd >= 0)
        add_fd(L.mon_fd);
    add_fd(L.tap_fd);
    L.tap_watched = true;
    /* Stdin that epoll can't watch (a file, /dev/null) is read through now. */
    L.stdin_watched = add_fd(0) == 0;
    while (!L.stdin_watched && !L.stop)
        read_stdin();
    awdl_node_timer(&L.node, now_us());
    L.next_stats = now_us() + AWDL_STATS_US;
    while (!L.stop) {
        watch(L.tap_fd, awdl_node_takes_outbound(&L.node), &L.tap_watched);
        arm();
        struct epoll_event ev[8];
        int n = epoll_wait(L.epoll_fd, ev, 8, -1);
        if (n < 0 && errno != EINTR) {
            event_error("The event loop failed.");
            L.failed = true;
            break;
        }
        for (int i = 0; i < n && !L.stop; i++) {
            int fd = ev[i].data.fd;
            if (fd == L.sig_fd) {
                struct signalfd_siginfo si;
                if (read(L.sig_fd, &si, sizeof si) > 0)
                    L.stop = true;
            } else if (fd == 0) {
                read_stdin();
            } else if (fd == L.timer_fd) {
                on_timer();
            } else if (fd == L.mon_fd) {
                read_monitor();
            } else if (fd == L.tap_fd) {
                read_tap();
            }
        }
    }
}

/* ---------------------------------------------------------- privileges */

/* Keeps CAP_NET_ADMIN and CAP_NET_RAW, and nothing else: not in the
 * bounding set (when run as root, which may change it), not inheritable,
 * not ambient, so nmcli runs with none. */
static int drop_capabilities(char *err, size_t errlen)
{
    struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d) < 0) {
        snprintf(err, errlen, "Can't read lares-awdl's capabilities: %s", strerror(errno));
        return -1;
    }
    const uint32_t keep = 1u << CAP_NET_ADMIN | 1u << CAP_NET_RAW;
    if ((d[0].permitted & keep) != keep) {
        snprintf(err, errlen, "lares-awdl needs CAP_NET_ADMIN and CAP_NET_RAW (install it with "
                              "cap_net_admin,cap_net_raw+ep).");
        return -1;
    }
    if (d[0].effective & (1u << CAP_SETPCAP))
        for (int cap = 0; cap <= 63; cap++)
            if (cap != CAP_NET_ADMIN && cap != CAP_NET_RAW && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0
                && errno == EINVAL)
                break;
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
    d[0].effective = d[0].permitted = keep;
    d[0].inheritable = 0;
    d[1].effective = d[1].permitted = d[1].inheritable = 0;
    if (syscall(SYS_capset, &h, d) < 0) {
        snprintf(err, errlen, "Can't drop lares-awdl's other capabilities: %s", strerror(errno));
        return -1;
    }
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    return 0;
}

/* ---------------------------------------------------------- start and end */

/* AWDL-2: a random, locally administered unicast address, kept for the
 * whole session. */
static void session_address(awdl_mac *m)
{
    if (getrandom(m->a, 6, 0) != 6) {
        awdl_time t = now_us() ^ (awdl_time)getpid() << 20;
        memcpy(m->a, &t, 6);
    }
    m->a[0] = (uint8_t)((m->a[0] & 0xfc) | 0x02);
}

static int block_signals(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigprocmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);
    L.sig_fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    return L.sig_fd;
}

static void start_node(const awdl_mac *addr, const char *name, uint8_t channel, bool accept_addba)
{
    struct awdl_config cfg;
    awdl_config_init(&cfg, addr, name, channel, AWDL_PRESENCE_FIXED);
    cfg.accept_addba = accept_addba;
    struct awdl_host host = {
        .transmit = host_transmit,
        .peer_up = host_peer_up,
        .peer_down = host_peer_down,
        .deliver = host_deliver,
    };
    awdl_node_init(&L.node, &cfg, &host, now_us());
}

static int run_test_tap(const char *name)
{
    char err[256];
    L.test = true;
    if (drop_capabilities(err, sizeof err) < 0) {
        event_error(err);
        return EXIT_UNUSABLE;
    }
    awdl_mac addr;
    session_address(&addr);
    L.tap_fd = awdl_tap_open(TAP_NAME, &addr, &L.tap_ifindex, err, sizeof err);
    if (L.tap_fd < 0) {
        event_error(err);
        return EXIT_UNUSABLE;
    }
    start_node(&addr, name, 6, false);
    event_ready(6, &addr);
    loop();
    close(L.tap_fd);
    emit("{\"event\":\"stopped\"}");
    return L.failed ? EXIT_UNUSABLE : EXIT_OK;
}

static int run(const char *ifname, uint8_t channel, const char *name)
{
    char err[512];
    if (drop_capabilities(err, sizeof err) < 0 || awdl_radio_recover(err, sizeof err) < 0) {
        event_error(err);
        return EXIT_UNUSABLE;
    }
    awdl_mac addr;
    session_address(&addr);
    /* The TAP first: it costs the card nothing if it fails, and it goes
     * with the process, so the journal needn't know it. */
    L.tap_fd = awdl_tap_open(TAP_NAME, &addr, &L.tap_ifindex, err, sizeof err);
    if (L.tap_fd < 0) {
        event_error(err);
        return EXIT_UNUSABLE;
    }
    struct awdl_radio radio;
    if (awdl_radio_up(ifname, channel, &addr, &radio, err, sizeof err) < 0) {
        close(L.tap_fd);
        if (L.stop) {
            /* Asked to stop during setup, which has put the card back. */
            emit("{\"event\":\"stopped\"}");
            return EXIT_OK;
        }
        event_error(err);
        return EXIT_UNUSABLE;
    }
    if (L.stop) {
        /* Asked to stop while the card was being set up. */
        close(L.tap_fd);
        if (awdl_radio_down(err, sizeof err) < 0)
            event_error(err);
        emit("{\"event\":\"stopped\"}");
        return EXIT_OK;
    }
    /* AWDL-2: peers address us by the AWDL address; something that
     * rewrote awdl0's (udev's MACAddressPolicy) breaks that. */
    awdl_radio_settle_tap(TAP_NAME, L.tap_ifindex, &addr);
    L.mon_fd = awdl_monitor_open(radio.monitor_ifindex, err, sizeof err);
    if (L.mon_fd < 0) {
        event_error(err);
        close(L.tap_fd);
        awdl_radio_down(err, sizeof err);
        return EXIT_UNUSABLE;
    }
    start_node(&addr, name, radio.channel, radio.accept_addba);
    event_ready(radio.channel, &addr);
    loop();
    /* awdl0 goes with its descriptor, and its neighbour entries with it. */
    close(L.tap_fd);
    close(L.mon_fd);
    int code = L.failed ? EXIT_UNUSABLE : EXIT_OK;
    if (awdl_radio_down(err, sizeof err) < 0) {
        event_error(err);
        code = EXIT_UNUSABLE;
    }
    emit("{\"event\":\"stopped\"}");
    return code;
}

int main(int argc, char **argv)
{
    L.epoll_fd = L.sig_fd = L.timer_fd = L.mon_fd = L.tap_fd = -1;
    const char *ifname = NULL, *name_arg = NULL;
    unsigned channel = 0;
    bool restore = false, test_tap = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fputs(USAGE, stdout);
            return EXIT_OK;
        } else if (strcmp(a, "--wifi") == 0 && more) {
            ifname = argv[++i];
        } else if (strcmp(a, "--channel") == 0 && more) {
            char *end;
            channel = (unsigned)strtoul(argv[++i], &end, 10);
            if (*end || !awdl_channel_social((uint8_t)channel) || channel > 255) {
                fputs(USAGE, stderr);
                return EXIT_USAGE;
            }
        } else if (strcmp(a, "--name") == 0 && more) {
            name_arg = argv[++i];
        } else if (strcmp(a, "--restore") == 0) {
            restore = true;
        } else if (strcmp(a, "--test-tap") == 0) {
            test_tap = true;
        } else {
            fputs(USAGE, stderr);
            return EXIT_USAGE;
        }
    }
    if ((restore || test_tap) == (ifname != NULL) || (restore && test_tap)
        || ((restore || test_tap) && channel)) {
        fputs(USAGE, stderr);
        return EXIT_USAGE;
    }
    if (block_signals() < 0) {
        event_error("Can't watch for signals.");
        return EXIT_UNUSABLE;
    }
    char name[AWDL_NAME_MAX + 1];
    if (name_arg && name_arg[0]) {
        /* At most AWDL_NAME_MAX bytes, cut on a UTF-8 character boundary. */
        size_t len = strlen(name_arg);
        if (len > AWDL_NAME_MAX) {
            len = AWDL_NAME_MAX;
            while (len > 0 && ((uint8_t)name_arg[len] & 0xc0) == 0x80)
                len--;
        }
        memcpy(name, name_arg, len);
        name[len] = 0;
    } else {
        awdl_host_name(name);
    }

    if (test_tap)
        return run_test_tap(name);
    if (restore) {
        char err[512];
        if (drop_capabilities(err, sizeof err) < 0 || awdl_radio_recover(err, sizeof err) < 0) {
            event_error(err);
            return EXIT_UNUSABLE;
        }
        return EXIT_OK;
    }
    return run(ifname, (uint8_t)channel, name);
}
