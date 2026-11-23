// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* The card: what lares-awdl does to the Wi-Fi card, the TAP and the
 * kernel, by netlink written out by hand (generic netlink's family lookup,
 * nl80211, rtnetlink), sysfs and NetworkManager's nmcli. It follows
 * owly-radio's injection backend (injection/mod.rs and setup.rs, nl80211.rs,
 * rtnl.rs, netlink.rs, native.rs, journal.rs, probe.rs, sys.rs and the
 * drivers.toml quirks), in two of its modes and one of Lares' own:
 *
 * - exclusive, with an ad-hoc cell: the card's interface is taken down and
 *   an IBSS cell on AWDL's BSSID takes its place on the AWDL address, so
 *   the firmware ACKs the phone's unicast, with a monitor interface beside
 *   it for raw frames;
 * - active monitor, for cards without ad-hoc mode that advertise one: an
 *   active monitor on the AWDL address ACKs for itself;
 * - plain monitor, for rtl8xxxu's RTL8188EU, which has neither: its
 *   monitor was measured to ACK unicast to any address (docs/AIRDROP.md),
 *   so one monitor interface is all it takes.
 *
 * owly's shared mode (beside an associated station) and its hidden-AP
 * exclusive mode (hostapd) aren't ported. Every change is written to the
 * journal before it's made, and putting the card back replays it, so a run
 * that crashed is undone the same way by the next one. */
#define _GNU_SOURCE /* pipe2, the capability and prctl calls */
#include "awdl.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/filter.h>
#include <linux/genetlink.h>
#include <linux/if_addr.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/if_tun.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>

/* The interfaces a session adds; journal entries name only these. */
#define MONITOR_IF      "laresmon0"
#define CELL_IF         "laresibss0"
#define OUR_PREFIX      "lares"
#define IBSS_SSID       "lares"
#define JOURNAL_NAME    "lares-awdl.journal"
#define LOCK_NAME       "lares-awdl.lock"
#define JOURNAL_MAGIC   "lares-awdl journal 1"
#define FREQ_6          2437u
#define FREQ_44         5220u
#define FREQ_149        5745u
#define SOCKET_BUFFER   (4 << 20)       /* a burst of an iPhone's frames */
#define NM_TRIES        3

static void set_err(char *err, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

/* A plain sleep, for putting the card back: no stop request cuts it short. */
static void pause_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* ================================================================ netlink */

struct nl {
    int fd;
    uint32_t seq;
};

static uint8_t nl_rx[32768] __attribute__((aligned(8)));

typedef void (*nl_cb)(void *ctx, const struct nlmsghdr *h);

static int nl_open(struct nl *s, int proto)
{
    s->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, proto);
    if (s->fd < 0)
        return -errno;
    struct sockaddr_nl a = { .nl_family = AF_NETLINK };
    struct timeval tv = { AWDL_NETLINK_TIMEOUT_S, 0 };
    if (bind(s->fd, (struct sockaddr *)&a, sizeof a) < 0
        || setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) {
        int e = -errno;
        close(s->fd);
        s->fd = -1;
        return e;
    }
    s->seq = 0;
    return 0;
}

/* One request and its answer: every message of a dump, or the replies
 * before the acknowledgement. 0, or a negative errno from the kernel. */
static int nl_request(struct nl *s, uint16_t type, uint16_t flags, const void *payload, size_t len,
                      nl_cb cb, void *ctx)
{
    uint32_t seq = ++s->seq;
    bool dump = (flags & NLM_F_DUMP) == NLM_F_DUMP;
    struct nlmsghdr h = {
        .nlmsg_len = (uint32_t)(NLMSG_HDRLEN + len),
        .nlmsg_type = type,
        .nlmsg_flags = (uint16_t)(flags | NLM_F_REQUEST | (dump ? 0 : NLM_F_ACK)),
        .nlmsg_seq = seq,
    };
    struct iovec iov[2] = { { &h, NLMSG_HDRLEN }, { (void *)payload, len } };
    struct sockaddr_nl to = { .nl_family = AF_NETLINK };
    struct msghdr m = { .msg_name = &to, .msg_namelen = sizeof to, .msg_iov = iov, .msg_iovlen = 2 };
    if (sendmsg(s->fd, &m, 0) < 0)
        return -errno;
    for (;;) {
        struct iovec riov = { nl_rx, sizeof nl_rx };
        struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1 };
        ssize_t got = recvmsg(s->fd, &rm, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? -ETIMEDOUT : -errno;
        }
        if (rm.msg_flags & MSG_TRUNC)
            return -EMSGSIZE;
        size_t left = (size_t)got;
        for (const uint8_t *at = nl_rx; left >= NLMSG_HDRLEN;) {
            const struct nlmsghdr *r = (const struct nlmsghdr *)at;
            if (r->nlmsg_len < NLMSG_HDRLEN || r->nlmsg_len > left)
                return -EIO;
            size_t step = NLMSG_ALIGN(r->nlmsg_len) < left ? NLMSG_ALIGN(r->nlmsg_len) : left;
            at += step;
            left -= step;
            if (r->nlmsg_seq != seq)
                continue; /* a late answer to a request that timed out */
            if (r->nlmsg_type == NLMSG_ERROR) {
                if (r->nlmsg_len < NLMSG_LENGTH(sizeof(int)))
                    return -EIO;
                return ((struct nlmsgerr *)NLMSG_DATA(r))->error;
            }
            if (r->nlmsg_type == NLMSG_DONE)
                return 0;
            if (cb)
                cb(ctx, r);
        }
    }
}

/* A request's payload: a fixed header, then attributes. */
struct nlb {
    uint8_t buf[512] __attribute__((aligned(4)));
    size_t len;
    bool overflow;
};

static void nlb_raw(struct nlb *b, const void *p, size_t n)
{
    if (b->overflow || n > sizeof b->buf - b->len) {
        b->overflow = true;
        return;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void nlb_pad(struct nlb *b)
{
    static const uint8_t zero[4];
    nlb_raw(b, zero, NLA_ALIGN(b->len) - b->len);
}

static void nla_put(struct nlb *b, uint16_t type, const void *p, size_t n)
{
    struct nlattr a = { .nla_len = (uint16_t)(NLA_HDRLEN + n), .nla_type = type };
    nlb_raw(b, &a, sizeof a);
    nlb_raw(b, p, n);
    nlb_pad(b);
}

static void nla_u8(struct nlb *b, uint16_t type, uint8_t v)
{
    nla_put(b, type, &v, 1);
}

static void nla_u32(struct nlb *b, uint16_t type, uint32_t v)
{
    nla_put(b, type, &v, 4);
}

static void nla_flag(struct nlb *b, uint16_t type)
{
    nla_put(b, type, NULL, 0);
}

static void nla_str(struct nlb *b, uint16_t type, const char *s)
{
    nla_put(b, type, s, strlen(s) + 1);
}

static size_t nla_nest(struct nlb *b, uint16_t type)
{
    size_t at = b->len;
    struct nlattr a = { .nla_len = NLA_HDRLEN, .nla_type = (uint16_t)(type | NLA_F_NESTED) };
    nlb_raw(b, &a, sizeof a);
    return at;
}

static void nla_nest_end(struct nlb *b, size_t at)
{
    if (b->overflow)
        return;
    uint16_t len = (uint16_t)(b->len - at);
    memcpy(b->buf + at, &len, 2);
}

/* Walks attributes: the next one's type and value, false at the end or at
 * the first malformed one. */
static bool nla_next(const uint8_t **p, size_t *left, uint16_t *type, const uint8_t **val, size_t *vlen)
{
    if (*left < NLA_HDRLEN)
        return false;
    struct nlattr a;
    memcpy(&a, *p, sizeof a);
    if (a.nla_len < NLA_HDRLEN || a.nla_len > *left)
        return false;
    *type = a.nla_type & NLA_TYPE_MASK;
    *val = *p + NLA_HDRLEN;
    *vlen = a.nla_len - NLA_HDRLEN;
    size_t step = NLA_ALIGN(a.nla_len);
    if (step > *left)
        step = *left;
    *p += step;
    *left -= step;
    return true;
}

static const uint8_t *nla_find(const uint8_t *p, size_t len, uint16_t want, size_t *vlen)
{
    uint16_t type;
    const uint8_t *v;
    size_t n;
    while (nla_next(&p, &len, &type, &v, &n))
        if (type == want) {
            *vlen = n;
            return v;
        }
    return NULL;
}

static bool nla_get_u32(const uint8_t *p, size_t len, uint16_t want, uint32_t *out)
{
    size_t n;
    const uint8_t *v = nla_find(p, len, want, &n);
    if (!v || n < 4)
        return false;
    memcpy(out, v, 4);
    return true;
}

/* ---------------------------------------------------------- generic netlink */

struct genl {
    struct nl nl;
    uint16_t family;
};

static void genl_family_cb(void *ctx, const struct nlmsghdr *h)
{
    const uint8_t *p = (const uint8_t *)NLMSG_DATA(h) + GENL_HDRLEN;
    size_t len = h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    size_t n;
    const uint8_t *v = nla_find(p, len, CTRL_ATTR_FAMILY_ID, &n);
    if (v && n >= 2)
        memcpy(ctx, v, 2);
}

static void genl_header(struct nlb *b, uint8_t cmd)
{
    memset(b, 0, sizeof *b);
    struct genlmsghdr g = { .cmd = cmd, .version = 0 };
    nlb_raw(b, &g, sizeof g);
}

static int genl_open(struct genl *g, const char *name)
{
    int r = nl_open(&g->nl, NETLINK_GENERIC);
    if (r < 0)
        return r;
    struct nlb b;
    genl_header(&b, CTRL_CMD_GETFAMILY);
    nla_str(&b, CTRL_ATTR_FAMILY_NAME, name);
    uint16_t family = 0;
    r = nl_request(&g->nl, GENL_ID_CTRL, 0, b.buf, b.len, genl_family_cb, &family);
    if (r == 0 && family == 0)
        r = -ENOENT;
    if (r < 0) {
        close(g->nl.fd);
        g->nl.fd = -1;
        return r;
    }
    g->family = family;
    return 0;
}

/* The two sockets a session keeps: nl80211 and rtnetlink. */
static struct genl NL80211 = { { -1, 0 }, 0 };
static struct nl RTNL = { -1, 0 };

static int nl80211(void)
{
    return NL80211.nl.fd >= 0 ? 0 : genl_open(&NL80211, NL80211_GENL_NAME);
}

static int rtnl(void)
{
    return RTNL.fd >= 0 ? 0 : nl_open(&RTNL, NETLINK_ROUTE);
}

/* An nl80211 command with attributes `b` (begun by genl_header). */
static int nl80211_req(struct nlb *b, uint16_t flags, nl_cb cb, void *ctx)
{
    int r = nl80211();
    if (r < 0)
        return r;
    if (b->overflow)
        return -EMSGSIZE;
    return nl_request(&NL80211.nl, NL80211.family, flags, b->buf, b->len, cb, ctx);
}

/* An nl80211 message's attributes. */
static const uint8_t *genl_attrs(const struct nlmsghdr *h, size_t *len)
{
    *len = h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    return (const uint8_t *)NLMSG_DATA(h) + GENL_HDRLEN;
}

/* ================================================================ nl80211 */

/* What a wiphy can do, as far as the modes need: its interface types,
 * whether it has an active monitor, and AWDL's social channels. */
struct wiphy_info {
    uint32_t index;
    bool found, adhoc, monitor, active_monitor, self_managed_reg;
    struct {
        uint32_t freq;
        bool present, disabled, no_ir, radar;
    } ch[3];
};

static void wiphy_freq(struct wiphy_info *w, const uint8_t *p, size_t len)
{
    uint32_t freq;
    if (!nla_get_u32(p, len, NL80211_FREQUENCY_ATTR_FREQ, &freq))
        return;
    for (int i = 0; i < 3; i++) {
        if (w->ch[i].freq != freq)
            continue;
        size_t n;
        w->ch[i].present = true;
        w->ch[i].disabled = nla_find(p, len, NL80211_FREQUENCY_ATTR_DISABLED, &n) != NULL;
        w->ch[i].no_ir = nla_find(p, len, NL80211_FREQUENCY_ATTR_NO_IR, &n) != NULL;
        w->ch[i].radar = nla_find(p, len, NL80211_FREQUENCY_ATTR_RADAR, &n) != NULL;
    }
}

/* Merges one message of a split dump. */
static void wiphy_cb(void *ctx, const struct nlmsghdr *h)
{
    struct wiphy_info *w = ctx;
    size_t len;
    const uint8_t *p = genl_attrs(h, &len);
    uint32_t index;
    if (!nla_get_u32(p, len, NL80211_ATTR_WIPHY, &index) || index != w->index)
        return;
    w->found = true;
    uint16_t type;
    const uint8_t *v;
    size_t n;
    while (nla_next(&p, &len, &type, &v, &n)) {
        if (type == NL80211_ATTR_SUPPORTED_IFTYPES) {
            size_t tn;
            w->adhoc = nla_find(v, n, NL80211_IFTYPE_ADHOC, &tn) != NULL;
            w->monitor = nla_find(v, n, NL80211_IFTYPE_MONITOR, &tn) != NULL;
        } else if (type == NL80211_ATTR_FEATURE_FLAGS && n >= 4) {
            uint32_t f;
            memcpy(&f, v, 4);
            w->active_monitor = f & NL80211_FEATURE_ACTIVE_MONITOR;
        } else if (type == NL80211_ATTR_WIPHY_SELF_MANAGED_REG) {
            w->self_managed_reg = true;
        } else if (type == NL80211_ATTR_WIPHY_BANDS) {
            uint16_t bt, ft;
            const uint8_t *band, *fv;
            size_t bn, fn;
            const uint8_t *bp = v;
            size_t bl = n;
            while (nla_next(&bp, &bl, &bt, &band, &bn)) {
                size_t freqs_len;
                const uint8_t *freqs = nla_find(band, bn, NL80211_BAND_ATTR_FREQS, &freqs_len);
                while (freqs && nla_next(&freqs, &freqs_len, &ft, &fv, &fn))
                    wiphy_freq(w, fv, fn);
            }
        }
    }
}

static int wiphy_get(uint32_t index, struct wiphy_info *w)
{
    memset(w, 0, sizeof *w);
    w->index = index;
    w->ch[0].freq = FREQ_6;
    w->ch[1].freq = FREQ_44;
    w->ch[2].freq = FREQ_149;
    struct nlb b;
    genl_header(&b, NL80211_CMD_GET_WIPHY);
    nla_u32(&b, NL80211_ATTR_WIPHY, index);
    nla_flag(&b, NL80211_ATTR_SPLIT_WIPHY_DUMP);
    int r = nl80211_req(&b, NLM_F_DUMP, wiphy_cb, w);
    if (r == 0 && !w->found)
        r = -ENODEV;
    return r;
}

/* Whether the card may start a cell, beacon or inject on `freq`. */
static bool may_transmit(const struct wiphy_info *w, uint32_t freq)
{
    for (int i = 0; i < 3; i++)
        if (w->ch[i].freq == freq)
            return w->ch[i].present && !w->ch[i].disabled && !w->ch[i].no_ir && !w->ch[i].radar;
    return false;
}

struct iface_info {
    bool found, has_freq;
    uint32_t iftype, wiphy, freq;
};

static void iface_cb(void *ctx, const struct nlmsghdr *h)
{
    struct iface_info *i = ctx;
    size_t len;
    const uint8_t *p = genl_attrs(h, &len);
    if (!nla_get_u32(p, len, NL80211_ATTR_IFTYPE, &i->iftype))
        return;
    i->found = true;
    nla_get_u32(p, len, NL80211_ATTR_WIPHY, &i->wiphy);
    i->has_freq = nla_get_u32(p, len, NL80211_ATTR_WIPHY_FREQ, &i->freq);
    if (!i->has_freq) {
        /* A multi-link interface has its channels per link. */
        size_t ln;
        const uint8_t *links = nla_find(p, len, NL80211_ATTR_MLO_LINKS, &ln);
        uint16_t t;
        const uint8_t *lv;
        size_t lvn;
        while (links && !i->has_freq && nla_next(&links, &ln, &t, &lv, &lvn))
            i->has_freq = nla_get_u32(lv, lvn, NL80211_ATTR_WIPHY_FREQ, &i->freq);
    }
}

static int iface_get(int ifindex, struct iface_info *i)
{
    memset(i, 0, sizeof *i);
    struct nlb b;
    genl_header(&b, NL80211_CMD_GET_INTERFACE);
    nla_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    int r = nl80211_req(&b, 0, iface_cb, i);
    if (r == 0 && !i->found)
        r = -ENODEV;
    return r;
}

/* Monitor flags: MONITOR_DEFAULT leaves mac80211's default (control frames
 * and other BSSs' frames passed up), which a new passive monitor needs,
 * since with an empty set some drivers drop AWDL's frames by BSSID;
 * MONITOR_NONE is `iw set monitor none`, for a station made a monitor;
 * MONITOR_ACTIVE ACKs unicast to its own address and passes other BSSs'
 * frames (owly: without them an mt7921e drops a peer's data frames). */
enum monitor_flags { MONITOR_DEFAULT, MONITOR_NONE, MONITOR_ACTIVE };

static void put_iftype(struct nlb *b, uint32_t iftype, enum monitor_flags flags)
{
    nla_u32(b, NL80211_ATTR_IFTYPE, iftype);
    if (iftype != NL80211_IFTYPE_MONITOR || flags == MONITOR_DEFAULT)
        return;
    size_t at = nla_nest(b, NL80211_ATTR_MNTR_FLAGS);
    if (flags == MONITOR_ACTIVE) {
        nla_flag(b, NL80211_MNTR_FLAG_ACTIVE);
        nla_flag(b, NL80211_MNTR_FLAG_OTHER_BSS);
    }
    nla_nest_end(b, at);
}

static void new_iface_cb(void *ctx, const struct nlmsghdr *h)
{
    size_t len;
    const uint8_t *p = genl_attrs(h, &len);
    uint32_t idx;
    if (nla_get_u32(p, len, NL80211_ATTR_IFINDEX, &idx))
        *(int *)ctx = (int)idx;
}

/* Adds an interface on a wiphy; its index, or a negative errno. */
static int iface_new(uint32_t wiphy, const char *name, uint32_t iftype, enum monitor_flags flags)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_NEW_INTERFACE);
    nla_u32(&b, NL80211_ATTR_WIPHY, wiphy);
    nla_str(&b, NL80211_ATTR_IFNAME, name);
    put_iftype(&b, iftype, flags);
    int idx = -1;
    int r = nl80211_req(&b, 0, new_iface_cb, &idx);
    if (r < 0)
        return r;
    if (idx < 0)
        idx = (int)if_nametoindex(name);
    return idx > 0 ? idx : -ENODEV;
}

static int iface_del(int ifindex)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_DEL_INTERFACE);
    nla_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    return nl80211_req(&b, 0, NULL, NULL);
}

/* Changes an interface's type; it has to be down. */
static int iface_set_type(int ifindex, uint32_t iftype, enum monitor_flags flags)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_SET_INTERFACE);
    nla_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    put_iftype(&b, iftype, flags);
    return nl80211_req(&b, 0, NULL, NULL);
}

/* Tunes a monitor interface, 20 MHz wide (HT20). */
static int set_freq(int ifindex, uint32_t freq)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_SET_WIPHY);
    nla_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    nla_u32(&b, NL80211_ATTR_WIPHY_FREQ, freq);
    nla_u32(&b, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT20);
    return nl80211_req(&b, 0, NULL, NULL);
}

/* Joins (or starts) an HT20 ad-hoc cell on AWDL's BSSID on a fixed
 * channel, beaconing as rarely as cfg80211 allows: an iPhone restarted
 * beside a cell beaconing every 100 TU didn't list owly where one beside a
 * cell beaconing every 10 s did. */
static int ibss_join(int ifindex, uint32_t freq)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_JOIN_IBSS);
    nla_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    nla_put(&b, NL80211_ATTR_SSID, IBSS_SSID, strlen(IBSS_SSID));
    nla_u32(&b, NL80211_ATTR_WIPHY_FREQ, freq);
    nla_u32(&b, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT20);
    nla_flag(&b, NL80211_ATTR_FREQ_FIXED);
    nla_put(&b, NL80211_ATTR_MAC, AWDL_BSSID.a, 6);
    nla_u32(&b, NL80211_ATTR_BEACON_INTERVAL, AWDL_IBSS_BEACON_TU);
    return nl80211_req(&b, 0, NULL, NULL);
}

static int ibss_leave(int ifindex)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_LEAVE_IBSS);
    nla_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    return nl80211_req(&b, 0, NULL, NULL);
}

static void regdom_cb(void *ctx, const struct nlmsghdr *h)
{
    size_t len, n;
    const uint8_t *p = genl_attrs(h, &len);
    const uint8_t *v = nla_find(p, len, NL80211_ATTR_REG_ALPHA2, &n);
    if (v && n >= 2) {
        char *out = ctx;
        out[0] = (char)v[0];
        out[1] = (char)v[1];
        out[2] = 0;
    }
}

/* The global regulatory domain's country, "00" for the world. */
static int regdom_get(char out[3])
{
    out[0] = 0;
    struct nlb b;
    genl_header(&b, NL80211_CMD_GET_REG);
    int r = nl80211_req(&b, 0, regdom_cb, out);
    return r < 0 ? r : out[0] ? 0 : -ENOENT;
}

/* Asks for a regulatory domain; the kernel applies it a moment later. */
static int regdom_set(const char *alpha2)
{
    struct nlb b;
    genl_header(&b, NL80211_CMD_REQ_SET_REG);
    nla_str(&b, NL80211_ATTR_REG_ALPHA2, alpha2);
    return nl80211_req(&b, 0, NULL, NULL);
}

static bool country_code(const char *c)
{
    return c[0] >= 'A' && c[0] <= 'Z' && c[1] >= 'A' && c[1] <= 'Z' && c[2] == 0;
}

/* ================================================================ rtnetlink */

static int rtnl_req(uint16_t type, uint16_t flags, const struct nlb *b)
{
    int r = rtnl();
    if (r < 0)
        return r;
    if (b->overflow)
        return -EMSGSIZE;
    return nl_request(&RTNL, type, flags, b->buf, b->len, NULL, NULL);
}

static void link_header(struct nlb *b, int ifindex, unsigned flags, unsigned change)
{
    memset(b, 0, sizeof *b);
    struct ifinfomsg m = { .ifi_family = AF_UNSPEC, .ifi_index = ifindex, .ifi_flags = flags,
                           .ifi_change = change };
    nlb_raw(b, &m, sizeof m);
}

static int link_set_up(int ifindex, bool up)
{
    struct nlb b;
    link_header(&b, ifindex, up ? IFF_UP : 0, IFF_UP);
    return rtnl_req(RTM_NEWLINK, 0, &b);
}

static int link_set_mac(int ifindex, const awdl_mac *m)
{
    struct nlb b;
    link_header(&b, ifindex, 0, 0);
    nla_put(&b, IFLA_ADDRESS, m->a, 6);
    return rtnl_req(RTM_NEWLINK, 0, &b);
}

static int link_set_mtu(int ifindex, uint32_t mtu)
{
    struct nlb b;
    link_header(&b, ifindex, 0, 0);
    nla_u32(&b, IFLA_MTU, mtu);
    return rtnl_req(RTM_NEWLINK, 0, &b);
}

/* No IPv6 address of the kernel's own beside ours. */
static int link_set_addr_gen_none(int ifindex)
{
    struct nlb b;
    link_header(&b, ifindex, 0, 0);
    size_t spec = nla_nest(&b, IFLA_AF_SPEC);
    size_t inet6 = nla_nest(&b, AF_INET6);
    nla_u8(&b, IFLA_INET6_ADDR_GEN_MODE, IN6_ADDR_GEN_MODE_NONE);
    nla_nest_end(&b, inet6);
    nla_nest_end(&b, spec);
    return rtnl_req(RTM_NEWLINK, 0, &b);
}

/* NET-1: a /64 link-scope address with no duplicate address detection,
 * replacing it if it's there already. */
static int addr_add_link_local(int ifindex, const uint8_t a[16])
{
    struct nlb b;
    memset(&b, 0, sizeof b);
    struct ifaddrmsg m = { .ifa_family = AF_INET6, .ifa_prefixlen = 64, .ifa_flags = IFA_F_NODAD,
                           .ifa_scope = RT_SCOPE_LINK, .ifa_index = (uint32_t)ifindex };
    nlb_raw(&b, &m, sizeof m);
    nla_put(&b, IFA_LOCAL, a, 16);
    nla_put(&b, IFA_ADDRESS, a, 16);
    nla_u32(&b, IFA_FLAGS, IFA_F_NODAD);
    return rtnl_req(RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE, &b);
}

static void neigh_msg(struct nlb *b, int ifindex, uint16_t state, const awdl_mac *mac)
{
    memset(b, 0, sizeof *b);
    struct ndmsg m = { .ndm_family = AF_INET6, .ndm_ifindex = ifindex, .ndm_state = state };
    nlb_raw(b, &m, sizeof m);
    uint8_t ll[16];
    awdl_link_local(mac, ll);
    nla_put(b, NDA_DST, ll, 16);
}

/* NET-2: a permanent neighbour entry for an AWDL peer, so nothing waits on
 * neighbour discovery over the air. */
int awdl_neigh_add(int ifindex, const awdl_mac *mac)
{
    struct nlb b;
    neigh_msg(&b, ifindex, NUD_PERMANENT, mac);
    nla_put(&b, NDA_LLADDR, mac->a, 6);
    return rtnl_req(RTM_NEWNEIGH, NLM_F_CREATE | NLM_F_REPLACE, &b);
}

int awdl_neigh_del(int ifindex, const awdl_mac *mac)
{
    struct nlb b;
    neigh_msg(&b, ifindex, 0, mac);
    return rtnl_req(RTM_DELNEIGH, 0, &b);
}

/* ================================================================ sysfs and procfs */

/* A small file's first line, without its newline; its length, or -1. */
static int read_line(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    char *nl = strchr(out, '\n');
    if (nl)
        *nl = 0;
    return (int)strlen(out);
}

static int write_str(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ssize_t n = write(fd, value, strlen(value));
    int e = n < 0 ? -errno : 0;
    close(fd);
    return e;
}

/* A name the kernel would take for an interface, and safe in a path. */
static bool ifname_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= IFNAMSIZ || strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '/' || s[i] <= ' ' || s[i] == ':' || (unsigned char)s[i] >= 0x7f)
            return false;
    return true;
}

/* Interfaces are looked up through the kernel, not sysfs, which shows
 * another network namespace's in a container. */
static bool if_exists(const char *ifname)
{
    return if_nametoindex(ifname) != 0;
}

static bool parse_mac(const char *s, awdl_mac *m)
{
    unsigned v[6];
    char end;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &end) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff)
            return false;
        m->a[i] = (uint8_t)v[i];
    }
    return true;
}

/* An interface ioctl (SIOCGIFHWADDR, SIOCGIFFLAGS) on a throwaway socket. */
static bool if_ioctl(const char *ifname, unsigned long request, struct ifreq *ifr)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    memset(ifr, 0, sizeof *ifr);
    snprintf(ifr->ifr_name, IFNAMSIZ, "%s", ifname);
    bool ok = ioctl(fd, request, ifr) == 0;
    close(fd);
    return ok;
}

static bool if_mac(const char *ifname, awdl_mac *m)
{
    struct ifreq ifr;
    if (!if_ioctl(ifname, SIOCGIFHWADDR, &ifr))
        return false;
    memcpy(m->a, ifr.ifr_hwaddr.sa_data, 6);
    return true;
}

static bool if_up(const char *ifname)
{
    struct ifreq ifr;
    return if_ioctl(ifname, SIOCGIFFLAGS, &ifr) && (ifr.ifr_flags & IFF_UP);
}

/* The kernel driver's name, such as rtl8192cu. */
static void sys_driver(const char *ifname, char *out, size_t cap)
{
    char path[96], link[PATH_MAX];
    out[0] = 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", ifname);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0)
        return;
    link[n] = 0;
    const char *base = strrchr(link, '/');
    base = base ? base + 1 : link;
    size_t len = strlen(base) < cap - 1 ? strlen(base) : cap - 1;
    memcpy(out, base, len);
    out[len] = 0;
}

static int ipv6_conf_path(char *path, size_t cap, const char *ifname, const char *key)
{
    return snprintf(path, cap, "/proc/sys/net/ipv6/conf/%s/%s", ifname, key);
}

static int ipv6_conf_set(const char *ifname, const char *key, const char *value)
{
    char path[128];
    ipv6_conf_path(path, sizeof path, ifname, key);
    return write_str(path, value);
}

void awdl_host_name(char *out)
{
    char name[AWDL_NAME_MAX + 1];
    if (read_line("/proc/sys/kernel/hostname", name, sizeof name) > 0)
        snprintf(out, AWDL_NAME_MAX + 1, "%s", name);
    else
        snprintf(out, AWDL_NAME_MAX + 1, "lares");
}

/* ================================================================ NetworkManager */

static const char *nmcli_path(void)
{
    static const char *const where[] = { "/usr/bin/nmcli", "/bin/nmcli", "/usr/local/bin/nmcli" };
    for (size_t i = 0; i < sizeof where / sizeof where[0]; i++)
        if (access(where[i], X_OK) == 0)
            return where[i];
    return NULL;
}

/* Runs nmcli with `args`, its output in `out` (when given), with signals
 * as a fresh process has them and a plain environment. Its exit status,
 * or -1 when it's missing, failed or took too long. */
static int nmcli(const char *const *args, char *out, size_t cap)
{
    const char *bin = nmcli_path();
    if (!bin)
        return -1;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return -1;
    const char *argv[16];
    size_t argc = 0;
    argv[argc++] = "nmcli";
    for (size_t i = 0; args[i] && argc < 15; i++)
        argv[argc++] = args[i];
    argv[argc] = NULL;
    char *const envp[] = { (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin", (char *)"LC_ALL=C", NULL };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int r = posix_spawn(&pid, bin, &fa, &at, (char *const *)argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(pipefd[1]);
    if (r != 0) {
        close(pipefd[0]);
        return -1;
    }
    size_t got = 0;
    bool timed_out = false;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long spent = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (spent >= (long)AWDL_NMCLI_TIMEOUT_MS) {
            timed_out = true;
            break;
        }
        struct pollfd p = { pipefd[0], POLLIN, 0 };
        int pr = poll(&p, 1, (int)(AWDL_NMCLI_TIMEOUT_MS - (unsigned long)spent));
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0) {
            timed_out = pr == 0;
            break;
        }
        char buf[512];
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (out && got + 1 < cap) {
            size_t take = (size_t)n < cap - 1 - got ? (size_t)n : cap - 1 - got;
            memcpy(out + got, buf, take);
            got += take;
        }
    }
    close(pipefd[0]);
    if (out)
        out[got] = 0;
    if (timed_out)
        kill(pid, SIGKILL);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (timed_out || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void nm_set_managed(const char *ifname, bool managed)
{
    const char *args[] = { "-w", "5", "device", "set", ifname, "managed", managed ? "yes" : "no", NULL };
    nmcli(args, NULL, 0);
}

/* 1 managed, 0 unmanaged, -1 unknown (no NetworkManager, or no device). */
static int nm_managed(const char *ifname)
{
    char out[128];
    const char *args[] = { "-g", "GENERAL.STATE", "device", "show", ifname, NULL };
    if (nmcli(args, out, sizeof out) != 0)
        return -1;
    char *end;
    long state = strtol(out, &end, 10);
    if (end == out)
        return -1;
    return state == 10 ? 0 : 1; /* NM_DEVICE_STATE_UNMANAGED */
}

static bool uuid_ok(const char *s)
{
    if (strlen(s) != 36)
        return false;
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex && c != '-')
            return false;
    }
    return true;
}

/* The connection NetworkManager has up on the interface, if any. */
static bool nm_connection(const char *ifname, char out[40])
{
    char buf[128];
    const char *args[] = { "-g", "GENERAL.CON-UUID", "device", "show", ifname, NULL };
    out[0] = 0;
    if (nmcli(args, buf, sizeof buf) != 0)
        return false;
    buf[strcspn(buf, "\n")] = 0;
    if (!uuid_ok(buf))
        return false;
    memcpy(out, buf, 37); /* 36 characters and the NUL */
    return true;
}

/* NetworkManager reconnects the card by itself once it has it back, if the
 * connection has autoconnect on; otherwise the connection it had is
 * brought up, at once for a card whose type was changed, as owly does. */
static void nm_give_back(const char *ifname, const char *uuid, bool converted)
{
    if (!uuid_ok(uuid))
        return;
    char now[40];
    if (!converted)
        for (unsigned waited = 0; waited < AWDL_AUTOCONNECT_MS; waited += 1000) {
            if (nm_connection(ifname, now))
                return;
            pause_ms(1000);
        }
    const char *args[] = { "-w", "10", "connection", "up", "uuid", uuid, "ifname", ifname, NULL };
    for (int i = 0; i < NM_TRIES; i++) {
        if (nmcli(args, NULL, 0) == 0)
            return;
        pause_ms(1000);
    }
    awdl_log("%s is back, but didn't reconnect to its network", ifname);
}

/* ================================================================ the journal */

/* The card as it was, and each change made to it since, in the order
 * made. Written whole on every change, in the run directory, which
 * outlives the process but not a reboot, as the changes themselves don't. */
struct journal {
    char ifname[IFNAMSIZ];
    uint32_t wiphy, iftype;
    awdl_mac mac;
    bool up;
    int nm;                     /* 1 managed, 0 unmanaged, -1 unknown */
    char connection[40];
    char ifaces[4][IFNAMSIZ];   /* interfaces added, oldest first */
    int nifaces;
    bool type_changed, mac_changed, has_ipv6, has_regdom;
    char ipv6[4];               /* the card's disable_ipv6 before */
    char regdom[3];             /* the regulatory domain before */
};

static struct journal J;
static char journal_path[PATH_MAX];
static int lock_fd = -1;

static int journal_dir(char *out, size_t cap)
{
    /* getenv, not secure_getenv: with file capabilities the process runs
     * in secure mode, and the desktop user's run directory is the place. */
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    if (xdg && xdg[0] == '/' && stat(xdg, &st) == 0 && S_ISDIR(st.st_mode))
        return snprintf(out, cap, "%s", xdg) < (int)cap ? 0 : -1;
    return snprintf(out, cap, "/run") < (int)cap ? 0 : -1;
}

static int journal_save(void)
{
    char tmp[PATH_MAX + 8], buf[1024], mac[18];
    awdl_mac_format(&J.mac, mac);
    int n = snprintf(buf, sizeof buf,
                     JOURNAL_MAGIC "\nifname %s\nwiphy %u\niftype %u\nmac %s\nup %d\nnm %d\nconnection %s\n",
                     J.ifname, J.wiphy, J.iftype, mac, J.up, J.nm, J.connection[0] ? J.connection : "-");
    for (int i = 0; i < J.nifaces; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "iface %s\n", J.ifaces[i]);
    if (J.has_regdom)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "regdom %s\n", J.regdom);
    if (J.has_ipv6)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "ipv6 %s\n", J.ipv6);
    if (J.type_changed)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "type\n");
    if (J.mac_changed)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "mac-changed\n");
    if (n <= 0 || (size_t)n >= sizeof buf)
        return -EOVERFLOW;
    snprintf(tmp, sizeof tmp, "%s.tmp", journal_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -errno;
    bool ok = write(fd, buf, (size_t)n) == n && fsync(fd) == 0;
    int e = errno;
    close(fd);
    if (!ok || rename(tmp, journal_path) < 0) {
        e = ok ? errno : e;
        unlink(tmp);
        return -e;
    }
    return 0;
}

/* 1 with a journal read into `j`, 0 with none, -1 for one that can't be
 * read or doesn't make sense. Every name and value is checked, since the
 * file sits in the user's run directory. */
static int journal_load(struct journal *j)
{
    memset(j, 0, sizeof *j);
    int fd = open(journal_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    char *save = NULL, *line = strtok_r(buf, "\n", &save);
    if (!line || strcmp(line, JOURNAL_MAGIC) != 0)
        return -1;
    bool has_mac = false;
    j->nm = -1;
    while ((line = strtok_r(NULL, "\n", &save))) {
        char *sp = strchr(line, ' ');
        const char *v = sp ? sp + 1 : "";
        if (sp)
            *sp = 0;
        if (strcmp(line, "ifname") == 0 && ifname_ok(v)) {
            snprintf(j->ifname, sizeof j->ifname, "%s", v);
        } else if (strcmp(line, "wiphy") == 0) {
            j->wiphy = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(line, "iftype") == 0) {
            j->iftype = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(line, "mac") == 0) {
            has_mac = parse_mac(v, &j->mac);
        } else if (strcmp(line, "up") == 0) {
            j->up = strcmp(v, "1") == 0;
        } else if (strcmp(line, "nm") == 0) {
            j->nm = (int)strtol(v, NULL, 10);
        } else if (strcmp(line, "connection") == 0 && uuid_ok(v)) {
            snprintf(j->connection, sizeof j->connection, "%s", v);
        } else if (strcmp(line, "iface") == 0 && ifname_ok(v)
                   && strncmp(v, OUR_PREFIX, strlen(OUR_PREFIX)) == 0 && j->nifaces < 4) {
            snprintf(j->ifaces[j->nifaces++], IFNAMSIZ, "%s", v);
        } else if (strcmp(line, "regdom") == 0 && (country_code(v) || strcmp(v, "00") == 0)) {
            snprintf(j->regdom, sizeof j->regdom, "%s", v);
            j->has_regdom = true;
        } else if (strcmp(line, "ipv6") == 0 && (strcmp(v, "0") == 0 || strcmp(v, "1") == 0)) {
            snprintf(j->ipv6, sizeof j->ipv6, "%s", v);
            j->has_ipv6 = true;
        } else if (strcmp(line, "type") == 0) {
            j->type_changed = true;
        } else if (strcmp(line, "mac-changed") == 0) {
            j->mac_changed = true;
        }
    }
    bool type_ok = j->iftype == NL80211_IFTYPE_ADHOC || j->iftype == NL80211_IFTYPE_STATION
                   || j->iftype == NL80211_IFTYPE_AP || j->iftype == NL80211_IFTYPE_MONITOR;
    return j->ifname[0] && has_mac && type_ok ? 1 : -1;
}

static int journal_add_iface(const char *name)
{
    if (J.nifaces == 4)
        return -EOVERFLOW;
    snprintf(J.ifaces[J.nifaces++], IFNAMSIZ, "%s", name);
    return journal_save();
}

/* For a change that was recorded but not made. */
static void journal_drop_iface(void)
{
    if (J.nifaces > 0) {
        J.nifaces--;
        journal_save();
    }
}

/* Deletes an interface a session added, leaving its cell first. */
static void remove_iface(const char *name)
{
    if (!if_exists(name))
        return;
    int idx = (int)if_nametoindex(name);
    struct iface_info info;
    if (iface_get(idx, &info) == 0 && info.iftype == NL80211_IFTYPE_ADHOC)
        ibss_leave(idx);
    link_set_up(idx, false);
    int r = iface_del(idx);
    if (r < 0)
        awdl_log("deleting %s: %s", name, strerror(-r));
}

/* Puts everything the journal records back, newest first: the added
 * interfaces, the regulatory domain, then the card's type, IPv6, MAC, link
 * and NetworkManager. Stops only where the card can't be given its type
 * back, keeping the journal for the next try. */
static int replay(const struct journal *j, char *err, size_t errlen)
{
    for (int i = j->nifaces - 1; i >= 0; i--)
        remove_iface(j->ifaces[i]);
    if (j->has_regdom) {
        int r = regdom_set(j->regdom);
        if (r < 0)
            awdl_log("restoring the regulatory domain %s: %s", j->regdom, strerror(-r));
    }
    if (!if_exists(j->ifname)) {
        awdl_log("%s is gone, so there's nothing of it to put back", j->ifname);
        return 0;
    }
    int idx = (int)if_nametoindex(j->ifname);
    bool converted = false;
    struct iface_info now;
    if (iface_get(idx, &now) == 0 && now.iftype != j->iftype) {
        if (now.iftype == NL80211_IFTYPE_ADHOC)
            ibss_leave(idx);
        link_set_up(idx, false);
        int r = iface_set_type(idx, j->iftype, MONITOR_DEFAULT);
        if (r < 0) {
            set_err(err, errlen,
                    "Wi-Fi (%s) is still set up for AirDrop and couldn't be given its type back (%s). "
                    "Restarting the computer fixes it.",
                    j->ifname, strerror(-r));
            return -1;
        }
        converted = true;
    }
    converted |= j->type_changed;
    if (j->has_ipv6)
        ipv6_conf_set(j->ifname, "disable_ipv6", j->ipv6);
    awdl_mac mac;
    if (if_mac(j->ifname, &mac) && !awdl_mac_eq(&mac, &j->mac)) {
        link_set_up(idx, false);
        int r = link_set_mac(idx, &j->mac);
        if (r < 0)
            awdl_log("giving %s its address back: %s", j->ifname, strerror(-r));
    }
    link_set_up(idx, j->up);
    if (j->nm == 1) {
        nm_set_managed(j->ifname, true);
        nm_give_back(j->ifname, j->connection, converted);
    }
    return 0;
}

static int journal_replay(char *err, size_t errlen)
{
    struct journal j;
    int r = journal_load(&j);
    if (r == 0)
        return 0;
    if (r < 0) {
        awdl_log("%s can't be read; removing it", journal_path);
        unlink(journal_path);
        return 0;
    }
    if (replay(&j, err, errlen) < 0)
        return -1;
    unlink(journal_path);
    return 0;
}

int awdl_radio_recover(char *err, size_t errlen)
{
    char dir[PATH_MAX - 32], lock[PATH_MAX];
    if (journal_dir(dir, sizeof dir) < 0) {
        set_err(err, errlen, "The run directory's path is too long.");
        return -1;
    }
    snprintf(journal_path, sizeof journal_path, "%s/%s", dir, JOURNAL_NAME);
    snprintf(lock, sizeof lock, "%s/%s", dir, LOCK_NAME);
    if (lock_fd < 0) {
        lock_fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd < 0) {
            set_err(err, errlen, "Can't open %s: %s", lock, strerror(errno));
            return -1;
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            set_err(err, errlen, "Another lares-awdl has the Wi-Fi card.");
            return -1;
        }
    }
    if (journal_replay(err, errlen) < 0)
        return -1;
    memset(&J, 0, sizeof J);
    return 0;
}

int awdl_radio_down(char *err, size_t errlen)
{
    return journal_replay(err, errlen);
}

/* ================================================================ setup */

/* What owly's drivers.toml knows about drivers it runs in the modes ported
 * here, beyond what nl80211 says. */
static const struct quirk {
    const char *driver;
    enum awdl_mode mode;
    uint8_t channels[3];
    uint8_t nchannels;
    bool rx_ampdu;              /* the hardware answers A-MPDUs: accept Block Ack, AWDL-15 */
} QUIRKS[] = {
    /* Intel: no phone has used their ad-hoc cell on 5 GHz. */
    { "iwlwifi", AWDL_MODE_IBSS, { 6 }, 1, false },
    /* 2.4 GHz only. */
    { "rtl8xxxu", AWDL_MODE_IBSS, { 6 }, 1, false },
    /* MediaTek MT7921 and MT7922: no ad-hoc mode, and no unicast on 5 GHz. */
    { "mt7921e", AWDL_MODE_ACTIVE_MONITOR, { 6 }, 1, false },
    /* MediaTek MT7612U: unicast reaches the air on 5 GHz, and its active
     * monitor answers A-MPDUs itself. */
    { "mt76x2u", AWDL_MODE_ACTIVE_MONITOR, { 149, 44, 6 }, 3, true },
};

/* owly's default order: an iPhone gives channel 6 a slot or two in sixteen,
 * and 44 or 149 several. */
static const uint8_t DEFAULT_CHANNELS[3] = { 44, 149, 6 };

struct setup {
    const char *ifname;
    int ifindex;
    uint32_t wiphy;
    struct wiphy_info caps;
    const struct quirk *quirk;
    const awdl_mac *addr;
    char *err;
    size_t errlen;
};

/* The channels to try, in order, of those the card may transmit on. */
static size_t tx_freqs(const struct setup *s, uint8_t channel, uint32_t out[3])
{
    const uint8_t *order = DEFAULT_CHANNELS;
    size_t n = 3, count = 0;
    if (channel) {
        order = &channel;
        n = 1;
    } else if (s->quirk) {
        order = s->quirk->channels;
        n = s->quirk->nchannels;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t f = awdl_channel_freq(order[i]);
        if (awdl_channel_social(order[i]) && may_transmit(&s->caps, f))
            out[count++] = f;
    }
    return count;
}

/* Polls `done` every AWDL_SETUP_POLL_MS for up to `ms`; false when it never
 * came or a stop was asked for. */
static bool cell_started(int ifindex, uint32_t freq, unsigned ms)
{
    for (unsigned waited = 0;; waited += AWDL_SETUP_POLL_MS) {
        struct iface_info i;
        if (iface_get(ifindex, &i) == 0 && i.has_freq && i.freq == freq)
            return true;
        if (waited >= ms || !awdl_wait_ms(AWDL_SETUP_POLL_MS))
            return false;
    }
}

/* The monitor interface AWDL injects through, on the AWDL address. Its
 * index, or -1. */
static int add_monitor(struct setup *s, enum monitor_flags flags)
{
    if (if_exists(MONITOR_IF))
        remove_iface(MONITOR_IF);
    if (journal_add_iface(MONITOR_IF) < 0) {
        set_err(s->err, s->errlen, "Can't record the card's changes in %s.", journal_path);
        return -1;
    }
    int idx = iface_new(s->wiphy, MONITOR_IF, NL80211_IFTYPE_MONITOR, flags);
    if (idx < 0) {
        /* Not there, so putting the card back mustn't look for it. */
        journal_drop_iface();
        set_err(s->err, s->errlen, "Couldn't add a monitor interface to %s: %s", s->ifname,
                strerror(-idx));
        return -1;
    }
    nm_set_managed(MONITOR_IF, false);
    /* So A-MSDUs fit; a monitor that refuses keeps its MTU. */
    link_set_mtu(idx, AWDL_MONITOR_MTU);
    int r = link_set_mac(idx, s->addr);
    if (r == 0)
        r = link_set_up(idx, true);
    if (r < 0) {
        set_err(s->err, s->errlen, "Couldn't bring up the monitor interface: %s", strerror(-r));
        return -1;
    }
    return idx;
}

/* An ad-hoc cell on AWDL's BSSID in the card's place, on the AWDL address.
 * Joining that "IBSS" gives the firmware an address to ACK for and a BSSID
 * filter that passes AWDL, and mac80211 a station entry per peer, so
 * unicast is sent, retried and ACKed the ordinary way. The cell is HT,
 * since an iPhone sends its unicast at HT rates. A card that refuses an
 * ad-hoc interface beside its own (some Intel AX211s) gets its own made the
 * cell. Returns the frequency it started on, or 0. */
static uint32_t ibss_cell(struct setup *s, const uint32_t *freqs, size_t nfreqs)
{
    if (if_exists(CELL_IF))
        remove_iface(CELL_IF);
    if (journal_add_iface(CELL_IF) < 0) {
        set_err(s->err, s->errlen, "Can't record the card's changes in %s.", journal_path);
        return 0;
    }
    const char *cell = CELL_IF;
    int idx = iface_new(s->wiphy, CELL_IF, NL80211_IFTYPE_ADHOC, MONITOR_DEFAULT);
    if (idx >= 0) {
        nm_set_managed(CELL_IF, false);
    } else {
        journal_drop_iface();
        awdl_log("%s refuses an ad-hoc interface beside it (%s); making it the cell", s->ifname,
                 strerror(-idx));
        char path[128], was[8];
        ipv6_conf_path(path, sizeof path, s->ifname, "disable_ipv6");
        J.has_ipv6 = true;
        snprintf(J.ipv6, sizeof J.ipv6, "%s", read_line(path, was, sizeof was) > 0 && was[0] == '1' ? "1" : "0");
        J.type_changed = true;
        J.mac_changed = true;
        if (journal_save() < 0) {
            set_err(s->err, s->errlen, "Can't record the card's changes in %s.", journal_path);
            return 0;
        }
        int r = iface_set_type(s->ifindex, NL80211_IFTYPE_ADHOC, MONITOR_DEFAULT);
        if (r < 0) {
            set_err(s->err, s->errlen, "Couldn't make %s an ad-hoc cell: %s", s->ifname, strerror(-r));
            return 0;
        }
        cell = s->ifname;
        idx = s->ifindex;
    }
    /* The cell ACKs the phone's frames only on the AWDL address. owly only
     * warns here, since its cell keeps the card's own address, which is
     * its AWDL address; ours is random per session, so a cell on any other
     * would leave the phone unanswered: say so rather than run unheard. */
    int r = link_set_mac(idx, s->addr);
    if (r < 0) {
        set_err(s->err, s->errlen, "Couldn't give %s the AWDL address: %s", cell, strerror(-r));
        return 0;
    }
    /* IP runs on awdl0. Left alone, the kernel and avahi take the cell for
     * a network too and send plain IPv6 on AWDL's BSSID. */
    ipv6_conf_set(cell, "disable_ipv6", "1");
    r = link_set_up(idx, true);
    if (r < 0) {
        set_err(s->err, s->errlen, "Couldn't bring %s up: %s", cell, strerror(-r));
        return 0;
    }
    for (size_t i = 0; i < nfreqs; i++) {
        r = ibss_join(idx, freqs[i]);
        if (r < 0) {
            awdl_log("no ad-hoc cell on %u MHz: %s", freqs[i], strerror(-r));
            continue;
        }
        /* mac80211 scans the channel once before it starts the cell. */
        if (cell_started(idx, freqs[i], AWDL_IBSS_START_MS))
            return freqs[i];
        awdl_log("the ad-hoc cell on %u MHz didn't start", freqs[i]);
        ibss_leave(idx);
    }
    set_err(s->err, s->errlen, "Couldn't start AWDL's ad-hoc cell on any of AirDrop's channels.");
    return 0;
}

/* For cards with neither ad-hoc mode nor injection while associated: an
 * active monitor, which ACKs unicast to its own address, on the AWDL
 * address. owly found an active monitor alone keeps its old channel
 * (MT7612U), so the card's interface becomes a passive monitor first, for
 * the channel context that makes changes take effect, and is tuned too. */
static uint32_t active_monitor(struct setup *s, const uint32_t *freqs, size_t nfreqs, int *mon)
{
    J.type_changed = true;
    if (journal_save() < 0) {
        set_err(s->err, s->errlen, "Can't record the card's changes in %s.", journal_path);
        return 0;
    }
    int r = iface_set_type(s->ifindex, NL80211_IFTYPE_MONITOR, MONITOR_NONE);
    if (r == 0)
        r = link_set_up(s->ifindex, true);
    if (r < 0) {
        set_err(s->err, s->errlen, "Couldn't make %s a monitor: %s", s->ifname, strerror(-r));
        return 0;
    }
    *mon = add_monitor(s, MONITOR_ACTIVE);
    if (*mon < 0)
        return 0;
    for (size_t i = 0; i < nfreqs; i++) {
        set_freq(s->ifindex, freqs[i]);
        r = set_freq(*mon, freqs[i]);
        if (r == 0)
            return freqs[i];
        awdl_log("the active monitor can't tune to %u MHz: %s", freqs[i], strerror(-r));
    }
    set_err(s->err, s->errlen, "Couldn't tune the active monitor to any of AirDrop's channels.");
    return 0;
}

/* For a card whose plain monitor ACKs: a monitor interface beside the
 * card's own, which stays down. Nothing else is started or stopped, since
 * some of these cards hear nothing once they've been stopped and started
 * again (the RTL8188EU lent to WSL, issue #98). */
static uint32_t plain_monitor(struct setup *s, const uint32_t *freqs, size_t nfreqs, int *mon)
{
    *mon = add_monitor(s, MONITOR_DEFAULT);
    if (*mon < 0)
        return 0;
    for (size_t i = 0; i < nfreqs; i++) {
        int r = set_freq(*mon, freqs[i]);
        if (r == 0)
            return freqs[i];
        awdl_log("the monitor can't tune to %u MHz: %s", freqs[i], strerror(-r));
    }
    set_err(s->err, s->errlen, "Couldn't tune the monitor to any of AirDrop's channels.");
    return 0;
}

/* Asks for `country`'s regulatory domain where the card lost it, and waits
 * for the kernel to apply it; a driver that manages its own (Intel's LAR)
 * ignores the request, so it isn't made. */
static void keep_country(struct setup *s, const char *country)
{
    char current[3] = "";
    if (!country[0] || s->caps.self_managed_reg)
        return;
    regdom_get(current);
    if (strcmp(current, country) == 0)
        return;
    J.has_regdom = true;
    snprintf(J.regdom, sizeof J.regdom, "%s", current[0] ? current : "00");
    if (journal_save() < 0)
        return;
    int r = regdom_set(country);
    if (r < 0) {
        awdl_log("couldn't set the regulatory domain to %s: %s", country, strerror(-r));
        return;
    }
    for (unsigned waited = 0; waited < AWDL_REGDOM_APPLY_MS; waited += AWDL_REGDOM_POLL_MS) {
        if (regdom_get(current) == 0 && strcmp(current, country) == 0)
            break;
        if (!awdl_wait_ms(AWDL_REGDOM_POLL_MS))
            break;
    }
    wiphy_get(s->wiphy, &s->caps);
}

int awdl_radio_up(const char *ifname, uint8_t channel, const awdl_mac *addr, struct awdl_radio *r,
                  char *err, size_t errlen)
{
    struct setup s = { .ifname = ifname, .addr = addr, .err = err, .errlen = errlen };
    memset(r, 0, sizeof *r);
    r->addr = *addr;
    if (!ifname_ok(ifname) || !if_exists(ifname)) {
        set_err(err, errlen, "There's no network interface called %s.", ifname);
        return -1;
    }
    if (strncmp(ifname, OUR_PREFIX, strlen(OUR_PREFIX)) == 0 || strcmp(ifname, "awdl0") == 0) {
        set_err(err, errlen, "%s is lares-awdl's own interface.", ifname);
        return -1;
    }
    s.ifindex = (int)if_nametoindex(ifname);
    struct iface_info info;
    int rc = iface_get(s.ifindex, &info);
    if (rc == -ENODEV || rc == -EOPNOTSUPP || rc == -EINVAL) {
        set_err(err, errlen, "%s isn't a Wi-Fi card.", ifname);
        return -1;
    }
    s.wiphy = info.wiphy;
    if (rc == 0)
        rc = wiphy_get(s.wiphy, &s.caps);
    if (rc < 0) {
        set_err(err, errlen, "Can't read what %s can do: %s", ifname, strerror(-rc));
        return -1;
    }
    char driver[64];
    sys_driver(ifname, driver, sizeof driver);
    for (size_t i = 0; i < sizeof QUIRKS / sizeof QUIRKS[0]; i++)
        if (strcmp(driver, QUIRKS[i].driver) == 0)
            s.quirk = &QUIRKS[i];

    /* The mode: owly's for the driver where it knows one, else an ad-hoc
     * cell where the card has ad-hoc mode, else an active monitor. */
    bool want_active = s.quirk && s.quirk->mode == AWDL_MODE_ACTIVE_MONITOR;
    if (!s.caps.monitor) {
        set_err(err, errlen, "This Wi-Fi card (%s) has no monitor mode, which AirDrop needs.",
                driver[0] ? driver : ifname);
        return -1;
    }
    if (s.caps.active_monitor && (want_active || !s.caps.adhoc)) {
        r->mode = AWDL_MODE_ACTIVE_MONITOR;
    } else if (s.caps.adhoc) {
        r->mode = AWDL_MODE_IBSS;
    } else if (strcmp(driver, "rtl8xxxu") == 0) {
        /* rtl8xxxu's chips without ad-hoc mode (the RTL8188EU): a plain
         * monitor ACKed unicast to any address when it was measured, with
         * probe responses that came once and were never retried (issue #98). */
        r->mode = AWDL_MODE_MONITOR;
    } else {
        set_err(err, errlen,
                "This Wi-Fi card (%s) has neither ad-hoc mode nor an active monitor, so it can't "
                "acknowledge an iPhone's frames.",
                driver[0] ? driver : ifname);
        return -1;
    }
    r->accept_addba = s.quirk && s.quirk->rx_ampdu;

    /* The country the kernel has now: it can forget it once the card is
     * down, and the world's domain left in its place allows no cell on
     * 5 GHz. Without one, the channels the card may use are known now:
     * don't take Wi-Fi down for none. */
    char country[3] = "";
    if (regdom_get(country) < 0 || !country_code(country))
        country[0] = 0;
    uint32_t freqs[3];
    if (!country[0] && tx_freqs(&s, channel, freqs) == 0) {
        set_err(err, errlen, "This Wi-Fi card may not transmit on %s here.",
                channel ? "that channel" : "any of AirDrop's channels (6, 44, 149)");
        return -1;
    }

    /* The card as it is, before anything changes. */
    memset(&J, 0, sizeof J);
    snprintf(J.ifname, sizeof J.ifname, "%s", ifname);
    J.wiphy = s.wiphy;
    J.iftype = info.iftype;
    if (!if_mac(ifname, &J.mac)) {
        set_err(err, errlen, "%s has no MAC address.", ifname);
        return -1;
    }
    J.up = if_up(ifname);
    J.nm = nm_managed(ifname);
    if (J.nm == 1)
        nm_connection(ifname, J.connection);
    int saved = journal_save();
    if (saved < 0) {
        set_err(err, errlen, "Can't write the recovery journal %s: %s", journal_path, strerror(-saved));
        return -1;
    }

    /* NetworkManager lets go of the card, or it brings it straight back. */
    if (J.nm == 1)
        nm_set_managed(ifname, false);
    rc = link_set_up(s.ifindex, false);
    if (rc < 0) {
        set_err(err, errlen, "Couldn't take %s down: %s", ifname, strerror(-rc));
        goto fail;
    }
    keep_country(&s, country);
    size_t nfreqs = tx_freqs(&s, channel, freqs);
    if (nfreqs == 0) {
        set_err(err, errlen, "This Wi-Fi card may not transmit on %s here.",
                channel ? "that channel" : "any of AirDrop's channels (6, 44, 149)");
        goto fail;
    }
    uint32_t freq;
    if (r->mode == AWDL_MODE_ACTIVE_MONITOR) {
        freq = active_monitor(&s, freqs, nfreqs, &r->monitor_ifindex);
    } else if (r->mode == AWDL_MODE_MONITOR) {
        freq = plain_monitor(&s, freqs, nfreqs, &r->monitor_ifindex);
    } else {
        freq = ibss_cell(&s, freqs, nfreqs);
        if (freq)
            r->monitor_ifindex = add_monitor(&s, MONITOR_DEFAULT);
        if (r->monitor_ifindex < 0)
            freq = 0;
    }
    if (!freq)
        goto fail;
    r->channel = awdl_channel_from_freq(freq);
    awdl_log("%s mode on %s, channel %u",
             r->mode == AWDL_MODE_IBSS ? "ad-hoc cell" : r->mode == AWDL_MODE_MONITOR ? "plain monitor" : "active monitor",
             ifname, r->channel);
    return 0;

fail:;
    char again[256];
    if (journal_replay(again, sizeof again) < 0)
        awdl_log("%s", again);
    return -1;
}

/* ================================================================ the TAP and the monitor socket */

int awdl_tap_open(const char *name, const awdl_mac *addr, int *ifindex, char *err, size_t errlen)
{
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, errlen, "Can't open /dev/net/tun: %s", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        set_err(err, errlen, "Can't make %s: %s", name, strerror(errno));
        close(fd);
        return -1;
    }
    /* No offloads: frames come and go whole. */
    ioctl(fd, TUNSETOFFLOAD, 0UL);
    int idx = (int)if_nametoindex(name);
    int r = idx > 0 ? 0 : -ENODEV;
    if (r == 0 && link_set_addr_gen_none(idx) < 0)
        ipv6_conf_set(name, "addr_gen_mode", "1");
    if (r == 0) {
        ipv6_conf_set(name, "accept_dad", "0");
        r = link_set_mac(idx, addr);
    }
    if (r == 0)
        r = link_set_mtu(idx, AWDL_TAP_MTU);
    uint8_t ll[16];
    awdl_link_local(addr, ll);
    if (r == 0)
        r = addr_add_link_local(idx, ll);
    if (r == 0)
        r = link_set_up(idx, true);
    if (r < 0) {
        set_err(err, errlen, "Can't set %s up: %s", name, strerror(-r));
        close(fd);
        return -1;
    }
    *ifindex = idx;
    return fd;
}

void awdl_radio_settle_tap(const char *name, int ifindex, const awdl_mac *addr)
{
    nm_set_managed(name, false);
    awdl_mac now;
    if (if_mac(name, &now) && !awdl_mac_eq(&now, addr)) {
        awdl_log("%s's MAC was changed by something else; setting it back", name);
        link_set_up(ifindex, false);
        link_set_mac(ifindex, addr);
        uint8_t ll[16];
        awdl_link_local(addr, ll);
        addr_add_link_local(ifindex, ll);
        link_set_up(ifindex, true);
    }
}

int awdl_monitor_open(int ifindex, char *err, size_t errlen)
{
    /* AWDL-1: only frames on AWDL's BSSID reach us. The filter reads the
     * radiotap length (little-endian), skips control and extension frames,
     * which have no address 3, and compares address 3. */
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 3),
        BPF_STMT(BPF_ALU | BPF_LSH | BPF_K, 8),
        BPF_STMT(BPF_MISC | BPF_TAX, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 2),
        BPF_STMT(BPF_ALU | BPF_OR | BPF_X, 0),
        BPF_STMT(BPF_MISC | BPF_TAX, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0c),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x04, 6, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0c, 5, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 16),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x002500ff, 0, 3),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 20),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x9473, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0x40000),
        BPF_STMT(BPF_RET | BPF_K, 0),
    };
    struct sock_fprog prog = { .len = sizeof code / sizeof code[0], .filter = code };
    /* Protocol 0 until bound, so nothing from another interface queues up
     * before the filter is on. */
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        set_err(err, errlen, "Can't open a packet socket: %s", strerror(errno));
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) < 0) {
        set_err(err, errlen, "Can't filter the monitor's frames: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                             .sll_ifindex = ifindex };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        set_err(err, errlen, "Can't listen on the monitor interface: %s", strerror(errno));
        close(fd);
        return -1;
    }
    /* Our own frames aren't wanted back, and a burst of an iPhone's
     * shouldn't overflow the buffer. */
    int one = 1, size = SOCKET_BUFFER;
    setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    return fd;
}
