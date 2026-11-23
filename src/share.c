// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Sharing with Apple devices: which Wi-Fi card to lend, lares-awdl's life,
 * and the receiver, discovery and sends on top of its link. Nothing here
 * runs, and the card stays the user's, until receiving is turned on or
 * files are picked to send. With LARES_DRY_RUN set, the card, the link and
 * a nearby iPhone are pretended, so the section can be tried anywhere. */
#define _GNU_SOURCE
#include "share.h"
#include "airdrop/discovery.h"
#include "airdrop/session.h"
#include <grp.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>

/* s between /Discover tries while looking (DISC-5): a phone is reachable
 * about once a second over a fixed radio, and refuses until its Bluetooth
 * advert has woken it, so a slower retry only adds to the wait. */
#define DISCOVER_RETRY 1
/* s the link stays up after the last use, so the next send or receive
 * doesn't wait the 2.7 s lares-awdl takes to set the card up. */
#define LINK_LINGER 60
/* s for lares-awdl to give the card back before it's asked again: handing
 * a network back to NetworkManager can take about 45 s. */
#define LINK_STOP_GRACE 60

typedef struct {
    char *key, *address;
    guint16 port;
    guint flags;
    gboolean trying, listed;
} Candidate;

struct Share {
    ShareState st;
    ShareFn fn;
    gpointer user;
    gboolean dry;
    char *label;              /* "lares-<service ID>": AWDL's ARPA TLV and mDNS host */

    GSubprocess *proc;
    GDataInputStream *events;
    GCancellable *link_cancel;
    guint link_kill;
    char *iface, *address;

    GTlsCertificate *cert;
    gboolean cert_busy;
    char *service_id;         /* MDNS-3: our instance, and SenderID */

    AirdropMdns *mdns;
    AirdropBle *ble;
    AirdropReceiver *receiver;
    guint receive_timer;

    GHashTable *candidates;   /* key -> Candidate */
    GCancellable *discover_cancel;
    guint retry;

    GCancellable *send_cancel;
    char *send_key;
    guint dry_timer;

    gboolean looking;         /* finding devices while files are being picked */
    guint valid_peers;        /* AWDL peers the link counts as valid now */
    gboolean heard;           /* one was, since this use began */
    guint quiet_timer;
    guint linger;
    FILE *log;                /* ~/.cache/lares/share.log: when each step happened */
    gint64 log_start;
};

static void update_link(Share *s);
static void parts_apply(Share *s);

static void changed(Share *s)
{
    if (s->fn) s->fn(s->user);
}

/* Looking for devices to send to: files picked, or being picked. */
static gboolean finding(Share *s)
{
    return s->st.sending || s->looking;
}

/* s the link waits, while receiving or looking, before saying it has heard
 * no Apple device: an iPhone with its share sheet open is heard within a few
 * seconds even on channel 6, so silence this long means AirDrop is off or
 * not in Everyone mode on the phone, or the Wi-Fi adapter isn't receiving. */
#define QUIET_SECONDS 20

static gboolean quiet_over(gpointer data)
{
    Share *s = data;
    s->quiet_timer = 0;
    if (!s->heard && s->st.link == LINK_UP && (s->st.receiving || finding(s))) {
        s->st.quiet = TRUE;
        changed(s);
    }
    return G_SOURCE_REMOVE;
}

/* Starts the wait for a first Apple device when the link is up for a use,
 * and drops the hint once one is heard or the use ends. */
static void quiet_watch(Share *s)
{
    gboolean waiting = s->st.link == LINK_UP && (s->st.receiving || finding(s)) && !s->heard;
    if (waiting && !s->quiet_timer && !s->st.quiet)
        s->quiet_timer = g_timeout_add_seconds(QUIET_SECONDS, quiet_over, s);
    if (!waiting) {
        g_clear_handle_id(&s->quiet_timer, g_source_remove);
        if (s->st.quiet) {
            s->st.quiet = FALSE;
            changed(s);
        }
    }
}

/* A new use starts its wait afresh, unless a device is already in range. */
static void quiet_restart(Share *s)
{
    g_clear_handle_id(&s->quiet_timer, g_source_remove);
    s->heard = s->valid_peers > 0;
    s->st.quiet = FALSE;
}

/* When each step of finding and sending happened, in seconds from the
 * link's start, on this PC only and started afresh with each link: what
 * a slow discovery is measured with. No names or addresses. */
static void G_GNUC_PRINTF(2, 3) share_log(Share *s, const char *fmt, ...)
{
    if (!s->log || s->dry) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(s->log, "%7.2f  ", (g_get_monotonic_time() - s->log_start) / 1e6);
    vfprintf(s->log, fmt, ap);
    fputc('\n', s->log);
    fflush(s->log);
    va_end(ap);
}

static void log_open(Share *s);

/* A new log for a new session of use; one more line in the current one if
 * the link is still up from the last. */
static void log_session(Share *s, const char *what)
{
    if (!s->log || s->st.link == LINK_OFF || s->st.link == LINK_FAILED) log_open(s);
    share_log(s, "%s", what);
    quiet_restart(s);
}

static void log_open(Share *s)
{
    if (s->dry) return;
    if (s->log) fclose(s->log);
    char *dir = g_build_filename(g_get_user_cache_dir(), "lares", NULL), *path = g_build_filename(dir, "share.log", NULL);
    s->log = g_mkdir_with_parents(dir, 0700) == 0 ? fopen(path, "w") : NULL;
    s->log_start = g_get_monotonic_time();
    g_free(dir);
    g_free(path);
}

/* ------------------------------------------------------------ helpers */

static void device_free(gpointer data)
{
    ShareDevice *d = data;
    g_free(d->key);
    g_free(d->name);
    g_free(d->model);
    g_free(d->address);
    g_free(d->message);
    g_free(d);
}

static void incoming_free(ShareIncoming *in)
{
    if (!in) return;
    g_free(in->sender);
    if (in->names) g_ptr_array_unref(in->names);
    if (in->saved) g_ptr_array_unref(in->saved);
    g_free(in->message);
    g_free(in);
}

static void candidate_free(gpointer data)
{
    Candidate *c = data;
    g_free(c->key);
    g_free(c->address);
    g_free(c);
}

static ShareDevice *device_find(Share *s, const char *key)
{
    for (guint i = 0; i < s->st.devices->len; i++) {
        ShareDevice *d = s->st.devices->pdata[i];
        if (g_str_equal(d->key, key)) return d;
    }
    return NULL;
}

static ShareDevice *device_at(Share *s, const char *address)
{
    for (guint i = 0; i < s->st.devices->len; i++) {
        ShareDevice *d = s->st.devices->pdata[i];
        if (d->address && !g_ascii_strcasecmp(d->address, address)) return d;
    }
    return NULL;
}

GHashTable *share_parse_event(const char *line)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const char *p = line;
    while (g_ascii_isspace(*p)) p++;
    if (*p++ != '{') goto bad;
    for (;;) {
        while (g_ascii_isspace(*p)) p++;
        if (*p == '}') return t;
        GString *k = g_string_new(NULL), *v = g_string_new(NULL);
        GString *into = k;
        for (int part = 0; part < 2; part++) {
            while (g_ascii_isspace(*p)) p++;
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++) {
                    if (*p != '\\') { g_string_append_c(into, *p); continue; }
                    p++;
                    switch (*p) {
                    case 'n': g_string_append_c(into, '\n'); break;
                    case 't': g_string_append_c(into, '\t'); break;
                    case 'u': {
                        if (!g_ascii_isxdigit(p[1]) || !g_ascii_isxdigit(p[2]) || !g_ascii_isxdigit(p[3])
                            || !g_ascii_isxdigit(p[4])) goto bad_kv;
                        char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                        g_string_append_unichar(into, (gunichar)strtoul(hex, NULL, 16));
                        p += 4;
                        break;
                    }
                    case '\0': goto bad_kv;
                    default: g_string_append_c(into, *p);
                    }
                }
                if (*p++ != '"') goto bad_kv;
            } else if (part == 1) { /* a number, true, false or null */
                while (*p && *p != ',' && *p != '}' && !g_ascii_isspace(*p)) g_string_append_c(into, *p++);
                if (!into->len) goto bad_kv;
            } else {
                goto bad_kv;
            }
            while (g_ascii_isspace(*p)) p++;
            if (part == 0) {
                if (*p++ != ':') goto bad_kv;
                into = v;
            }
        }
        g_hash_table_replace(t, g_string_free(k, FALSE), g_string_free(v, FALSE));
        if (*p == ',') { p++; continue; }
        if (*p == '}') return t;
        goto bad;
    bad_kv:
        g_string_free(k, TRUE);
        g_string_free(v, TRUE);
        goto bad;
    }
bad:
    g_hash_table_unref(t);
    return NULL;
}

char *share_link_local(const char *mac, const char *ifname)
{
    unsigned b[6];
    if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return NULL;
    return g_strdup_printf("fe80::%x:%x:%x:%x%%%s", ((b[0] ^ 0x02) << 8) | b[1], (b[2] << 8) | 0xff,
                           0xfe00 | b[3], (b[4] << 8) | b[5], ifname);
}

/* How others see this PC: its pretty name if it has one. */
static char *computer_name(void)
{
    char *text = NULL;
    if (g_file_get_contents("/etc/machine-info", &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        g_free(text);
        text = NULL;
        for (char **l = lines; *l && !text; l++) {
            if (!g_str_has_prefix(*l, "PRETTY_HOSTNAME=")) continue;
            char *v = g_strstrip(g_strdup(*l + 16));
            if (*v == '"' || *v == '\'') {
                char *u = g_shell_unquote(v, NULL);
                g_free(v);
                v = u;
            }
            if (v && *v) text = v;
            else g_free(v);
        }
        g_strfreev(lines);
    }
    return text ? text : g_strdup(g_get_host_name());
}

static char *random_id(void)
{
    return g_strdup_printf("%06x%06x", g_random_int() & 0xffffff, g_random_int() & 0xffffff);
}

static char *download_folder(void)
{
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *folder = d ? g_strdup(d) : g_build_filename(g_get_home_dir(), "Downloads", NULL);
    g_mkdir_with_parents(folder, 0755);
    return folder;
}

/* ------------------------------------------------------------- the card */

static char *sysfs_link_name(const char *path)
{
    char *target = g_file_read_link(path, NULL);
    char *name = target ? g_path_get_basename(target) : NULL;
    g_free(target);
    return name;
}

gboolean share_has_card(void)
{
    if (g_getenv("LARES_DRY_RUN")) return TRUE;
    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (!d) return FALSE;
    gboolean found = FALSE;
    const char *name;
    while (!found && (name = g_dir_read_name(d))) {
        char *phy = g_build_filename("/sys/class/net", name, "phy80211", NULL);
        found = g_file_test(phy, G_FILE_TEST_EXISTS) && !g_str_equal(name, "awdl0");
        g_free(phy);
    }
    g_dir_close(d);
    return found;
}

static gboolean card_up(const char *iface)
{
    char *path = g_build_filename("/sys/class/net", iface, "operstate", NULL), *state = NULL;
    gboolean up = g_file_get_contents(path, &state, NULL, NULL) && g_str_has_prefix(state, "up");
    g_free(path);
    g_free(state);
    return up;
}

/* The card to lend: one this PC isn't using for its own Wi-Fi if there is
 * one, as a USB adapter beside the built-in card. */
static void find_card(Share *s)
{
    g_clear_pointer(&s->st.card, g_free);
    g_clear_pointer(&s->st.card_text, g_free);
    if (s->dry) {
        s->st.card = g_strdup("wlan0");
        s->st.card_text = g_strdup("USB Wi-Fi adapter, 2.4 GHz only");
        return;
    }
    /* For testing with another lares-awdl (LARES_AWDL), which may need no
     * card at all: the card it's handed, whatever is plugged in. */
    const char *forced = g_getenv("LARES_AWDL") ? g_getenv("LARES_WIFI") : NULL;
    if (forced && *forced) {
        s->st.card = g_strdup(forced);
        s->st.card_text = g_strdup("Wi-Fi card for testing");
        return;
    }
    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (!d) return;
    char *best = NULL;
    gboolean best_free = FALSE;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *phy = g_build_filename("/sys/class/net", name, "phy80211", NULL);
        gboolean wifi = g_file_test(phy, G_FILE_TEST_EXISTS) && !g_str_equal(name, "awdl0");
        g_free(phy);
        if (!wifi) continue;
        gboolean free_card = !card_up(name);
        if (!best || (free_card && !best_free)) {
            g_free(best);
            best = g_strdup(name);
            best_free = free_card;
        }
    }
    g_dir_close(d);
    if (!best) return;
    s->st.card = best;
    s->st.card_in_use = !best_free;
    /* The device's real path runs through the USB bus for an adapter. */
    char *dev = g_build_filename("/sys/class/net", best, "device", NULL);
    char *real = g_file_read_link(dev, NULL);
    gboolean usb = real && strstr(real, "/usb") != NULL;
    s->st.card_text = g_strdup(usb ? "USB Wi-Fi adapter" : "Built-in Wi-Fi card");
    g_free(dev);
    g_free(real);
}

/* Whether the card has 5 GHz, from `iw`: AirDrop is several times faster
 * there (docs/AIRDROP.md). Unknown, it says nothing. */
static void bands_read(GObject *src, GAsyncResult *res, gpointer data)
{
    Share *s = data;
    char *out = NULL;
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL) && out) {
        s->st.card_5ghz = strstr(out, "5180") || strstr(out, "5745") || strstr(out, "5220");
        gboolean known = strstr(out, "Frequencies") != NULL;
        if (known && s->st.card_text) {
            char *t = g_strdup_printf("%s, %s", s->st.card_text, s->st.card_5ghz ? "2.4 and 5 GHz" : "2.4 GHz only");
            g_free(s->st.card_text);
            s->st.card_text = t;
        }
        changed(s);
    }
    g_free(out);
    g_object_unref(src);
}

static void read_bands(Share *s)
{
    if (s->dry || !s->st.card) return;
    char *path = g_build_filename("/sys/class/net", s->st.card, "phy80211", NULL);
    char *phy = sysfs_link_name(path);
    g_free(path);
    if (!phy) return;
    const char *argv[] = { "iw", "phy", phy, "info", NULL };
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_free(phy);
    if (p) g_subprocess_communicate_utf8_async(p, NULL, NULL, bands_read, s);
}

/* ------------------------------------------------------- lares-awdl's install */

/* The lares-awdl built with this Lares: beside it in a build folder, or in
 * ../libexec once installed. */
static char *bundled_awdl(void)
{
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return NULL;
    char *dir = g_path_get_dirname(exe);
    char *candidates[] = { g_build_filename(dir, "lares-awdl", NULL),
                           g_build_filename(dir, "..", "libexec", "lares-awdl", NULL) };
    char *found = NULL;
    for (int i = 0; i < 2; i++) {
        if (!found && g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE)) found = g_canonicalize_filename(candidates[i], NULL);
        g_free(candidates[i]);
    }
    g_free(dir);
    g_free(exe);
    return found;
}

static const char *awdl_path(void)
{
    const char *p = g_getenv("LARES_AWDL"); /* another copy, for testing */
    return p ? p : SHARE_AWDL_PATH;
}

/* Installed with its capabilities, and the same program as the bundled one. */
static gboolean awdl_ready(void)
{
    const char *path = awdl_path();
    if (g_getenv("LARES_AWDL")) return g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
    if (getxattr(path, "security.capability", NULL, 0) <= 0) return FALSE;
    char *bundled = bundled_awdl();
    if (!bundled) return TRUE;
    char *a = NULL, *b = NULL;
    gsize la = 0, lb = 0;
    gboolean same = g_file_get_contents(path, &a, &la, NULL) && g_file_get_contents(bundled, &b, &lb, NULL)
                    && la == lb && memcmp(a, b, la) == 0;
    g_free(a);
    g_free(b);
    g_free(bundled);
    return same;
}

static void check_setup(Share *s)
{
    if (!s->st.card) s->st.setup = SHARE_NO_CARD;
    else if (s->dry) s->st.setup = s->st.setup == SHARE_READY ? SHARE_READY : SHARE_NEEDS_ALLOW;
    else s->st.setup = awdl_ready() ? SHARE_READY : SHARE_NEEDS_ALLOW;
}

static void allow_done(Share *s, gboolean ok, const char *message)
{
    s->st.allow_busy = FALSE;
    g_clear_pointer(&s->st.allow_error, g_free);
    if (ok) s->st.setup = s->dry ? SHARE_READY : (awdl_ready() ? SHARE_READY : SHARE_NEEDS_ALLOW);
    if (ok && s->st.setup != SHARE_READY) s->st.allow_error = g_strdup("It was installed, but can't be used yet.");
    if (!ok) s->st.allow_error = g_strdup(message);
    changed(s);
    update_link(s);
}

static void allowed(GObject *src, GAsyncResult *res, gpointer data)
{
    GSubprocess *p = G_SUBPROCESS(src);
    char *err = NULL;
    GError *e = NULL;
    gboolean ran = g_subprocess_communicate_utf8_finish(p, res, NULL, &err, &e);
    int status = ran && g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    if (status == 0) {
        allow_done(data, TRUE, NULL);
    } else if (status == 126 || status == 127) { /* pkexec: dismissed, or not allowed */
        allow_done(data, FALSE, "Nothing changed: the password wasn't given.");
    } else {
        char *line = err && *err ? g_strdup(g_strstrip(err)) : g_strdup(e ? e->message : "It stopped without saying why.");
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        allow_done(data, FALSE, line);
        g_free(line);
    }
    g_clear_error(&e);
    g_free(err);
    g_object_unref(p);
}

static gboolean pretend_allowed(gpointer data)
{
    Share *s = data;
    s->st.setup = SHARE_READY;
    allow_done(s, TRUE, NULL);
    return G_SOURCE_REMOVE;
}

void share_allow(Share *s)
{
    if (s->st.allow_busy) return;
    s->st.allow_busy = TRUE;
    g_clear_pointer(&s->st.allow_error, g_free);
    changed(s);
    if (s->dry) {
        g_timeout_add(1200, pretend_allowed, s);
        return;
    }
    char *bundled = bundled_awdl();
    if (!bundled) {
        allow_done(s, FALSE, "lares-awdl isn't installed with Lares: run make install.");
        return;
    }
    /* Root-owned, so nothing can change it after; run only by the user's
     * group; and given only the two capabilities AWDL needs. Beside it, two
     * small files: NetworkManager leaves AirDrop's interfaces alone, and
     * udev keeps the address lares-awdl gives awdl0 (AWDL-2). Their text
     * travels as arguments, so root reads nothing the user could change. */
    struct group *g = getgrgid(getgid());
    const char *script = "install -D -o root -g \"$2\" -m 0750 \"$1\" " SHARE_AWDL_PATH
                         " && setcap cap_net_admin,cap_net_raw+ep " SHARE_AWDL_PATH
                         " && umask 022 && mkdir -p /etc/NetworkManager/conf.d /etc/systemd/network"
                         " && printf '%s' \"$3\" > " SHARE_NM_CONF
                         " && printf '%s' \"$4\" > " SHARE_LINK_FILE
                         " && { nmcli general reload conf >/dev/null 2>&1 || true; }"
                         /* With ufw on, nearby devices couldn't reach the receiver:
                          * open its ports and mDNS, on AirDrop's interface only. */
                         " && if command -v ufw >/dev/null && ufw status | grep -q 'Status: active'; then"
                         " ufw allow in on awdl0 proto tcp to any port 8770:8779 comment 'Lares AirDrop' >/dev/null"
                         " && ufw allow in on awdl0 proto udp to any port 5353 comment 'Lares AirDrop' >/dev/null; fi";
    const char *nm = "# Lares' Share: AirDrop's interfaces belong to lares-awdl.\n[keyfile]\n"
                     "unmanaged-devices+=interface-name:awdl0;interface-name:laresmon0;interface-name:laresibss0\n";
    const char *link = "# Lares' Share: keep the address lares-awdl gives awdl0.\n[Match]\nOriginalName=awdl0\n\n"
                       "[Link]\nMACAddressPolicy=none\n";
    const char *argv[] = { "pkexec", "/bin/sh", "-c", script, "lares", bundled, g ? g->gr_name : "root", nm, link, NULL };
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
    g_free(bundled);
    if (!p) {
        allow_done(s, FALSE, e->message);
        g_error_free(e);
        return;
    }
    g_subprocess_communicate_utf8_async(p, NULL, NULL, allowed, s);
}

/* ------------------------------------------------------------- the parts */

static void receiver_offer(const AirdropOffer *o, gpointer user)
{
    Share *s = user;
    ShareIncoming *cur = s->st.incoming;
    /* One at a time: a second sender is declined while the first is on. */
    if (cur && (cur->state == INCOMING_ASKING || cur->state == INCOMING_RECEIVING)) {
        airdrop_receiver_answer(s->receiver, o->id, FALSE);
        return;
    }
    incoming_free(cur);
    share_log(s, "a device asks to send");
    ShareIncoming *in = g_new0(ShareIncoming, 1);
    in->id = o->id;
    in->state = INCOMING_ASKING;
    in->sender = g_strdup(o->sender);
    in->links = o->links;
    in->names = g_ptr_array_ref(o->names);
    in->total = o->total;
    s->st.incoming = in;
    changed(s);
}

static ShareIncoming *incoming_for(Share *s, guint id)
{
    return s->st.incoming && s->st.incoming->id == id ? s->st.incoming : NULL;
}

static void receiver_withdrawn(guint id, gpointer user)
{
    Share *s = user;
    ShareIncoming *in = incoming_for(s, id);
    if (!in || in->state != INCOMING_ASKING) return;
    incoming_free(in);
    s->st.incoming = NULL;
    changed(s);
    update_link(s);
}

static void receiver_progress(guint id, guint64 done, guint64 total, gpointer user)
{
    Share *s = user;
    ShareIncoming *in = incoming_for(s, id);
    if (!in) return;
    in->done = done;
    if (total) in->total = total;
    changed(s);
}

static void receiver_received(guint id, gboolean links, GPtrArray *items, gpointer user)
{
    Share *s = user;
    ShareIncoming *in = incoming_for(s, id);
    if (!in) return;
    in->state = INCOMING_DONE;
    share_log(s, "received");
    in->saved = g_ptr_array_ref(items);
    changed(s);
    update_link(s);
}

static void receiver_failed(guint id, const char *message, gpointer user)
{
    Share *s = user;
    ShareIncoming *in = incoming_for(s, id);
    if (!in) return;
    in->state = INCOMING_FAILED;
    share_log(s, "receiving failed");
    g_free(in->message);
    in->message = g_strdup(message);
    changed(s);
    update_link(s);
}

static const AirdropReceiverCallbacks RECEIVER = {
    receiver_offer, receiver_withdrawn, receiver_progress, receiver_received, receiver_failed,
};

static void discovered(GObject *src, GAsyncResult *res, gpointer data);

static void try_discover(Share *s, Candidate *c)
{
    if (c->trying || c->listed || !s->discover_cancel) return;
    c->trying = TRUE;
    share_log(s, "asking a %s", g_str_has_prefix(c->key, "awdl:") ? "nearby AWDL device directly" : "service found over mDNS");
    g_object_set_data_full(G_OBJECT(s->discover_cancel), "share", s, NULL);
    airdrop_discover_async(c->address, c->port, s->discover_cancel, discovered, g_strdup(c->key));
}

static void discovered(GObject *src, GAsyncResult *res, gpointer data)
{
    char *key = data;
    GError *e = NULL;
    AirdropPeer *peer = airdrop_discover_finish(res, &e);
    GCancellable *cancel = g_task_get_cancellable(G_TASK(res));
    gboolean cancelled = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED) || g_cancellable_is_cancelled(cancel);
    Share *s = cancelled ? NULL : g_object_get_data(G_OBJECT(cancel), "share");
    Candidate *c = s ? g_hash_table_lookup(s->candidates, key) : NULL;
    if (c) {
        c->trying = FALSE;
        if (!peer) share_log(s, "not listed yet: %s", e ? e->message : "it didn't list us");
        if (peer) {
            share_log(s, "listed a device");
            c->listed = TRUE;
            ShareDevice *d = device_find(s, key);
            ShareDevice *same = device_at(s, c->address);
            if (!d && same) {
                d = same; /* found both over mDNS and on AWDL alone: one device */
                if (c->flags) d->flags = c->flags;
            }
            if (!d) {
                d = g_new0(ShareDevice, 1);
                d->key = g_strdup(key);
                d->address = g_strdup(c->address);
                d->port = c->port;
                d->flags = c->flags;
                d->state = DEVICE_FOUND;
                g_ptr_array_add(s->st.devices, d);
            }
            g_free(d->name);
            g_free(d->model);
            d->name = g_strdup(peer->name);
            d->model = g_strdup(peer->model);
            changed(s);
        }
    }
    airdrop_peer_free(peer);
    g_clear_error(&e);
    g_free(key);
}

/* Receivers refuse until a Bluetooth advert has woken them, which can take
 * many seconds: keep asking while looking (DISC-5). */
static gboolean retry_discover(gpointer data)
{
    Share *s = data;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, s->candidates);
    while (g_hash_table_iter_next(&it, NULL, &v)) try_discover(s, v);
    return G_SOURCE_CONTINUE;
}

static void add_candidate(Share *s, const char *key, const char *address, guint16 port, guint flags)
{
    Candidate *c = g_hash_table_lookup(s->candidates, key);
    if (!c) {
        c = g_new0(Candidate, 1);
        c->key = g_strdup(key);
        g_hash_table_insert(s->candidates, c->key, c);
    }
    if (!c->address || g_strcmp0(c->address, address) || c->port != port) {
        g_free(c->address);
        c->address = g_strdup(address);
        c->port = port;
        c->listed = FALSE;
    }
    if (flags) c->flags = flags;
    ShareDevice *d = device_find(s, key);
    if (d && flags) d->flags = flags;
    try_discover(s, c);
}

static void drop_device(Share *s, const char *key)
{
    g_hash_table_remove(s->candidates, key);
    ShareDevice *d = device_find(s, key);
    if (d && d->state != DEVICE_ASKING && d->state != DEVICE_SENDING) {
        g_ptr_array_remove(s->st.devices, d);
        changed(s);
    }
}

static void mdns_found(const AirdropService *svc, gpointer user)
{
    Share *s = user;
    if (!finding(s) || !g_strcmp0(svc->instance, s->service_id)) return;
    share_log(s, "service found over mDNS");
    add_candidate(s, svc->instance, svc->address, svc->port, svc->flags);
}

static void mdns_lost(const char *instance, gpointer user)
{
    drop_device(user, instance);
}

static const MdnsCallbacks MDNS = { mdns_found, mdns_lost };

/* What runs on the link follows what's on: the receiver and its
 * announcement while receiving, browsing and the Bluetooth advert while
 * sending. */
static void parts_apply(Share *s)
{
    if (s->st.link != LINK_UP) return;
    if (s->dry) {
        changed(s);
        return;
    }
    if (!s->mdns) {
        GError *e = NULL;
        s->mdns = airdrop_mdns_new(s->iface, &MDNS, s, &e);
        if (!s->mdns) {
            g_free(s->st.link_error);
            s->st.link_error = g_strdup_printf("mDNS: %s", e->message);
            g_error_free(e);
        }
    }
    gboolean incoming_on = s->st.incoming && (s->st.incoming->state == INCOMING_ASKING
                                              || s->st.incoming->state == INCOMING_RECEIVING);
    if ((s->st.receiving || incoming_on) && !s->receiver && s->cert) {
        char *folder = download_folder();
        GError *e = NULL;
        s->receiver = airdrop_receiver_new(s->address, s->st.name, folder, s->cert, &RECEIVER, s, &e);
        g_free(folder);
        if (!s->receiver) {
            g_free(s->st.link_error);
            s->st.link_error = g_strdup(e->message);
            g_error_free(e);
        }
    } else if (!s->st.receiving && !incoming_on && s->receiver) {
        g_clear_pointer(&s->receiver, airdrop_receiver_free);
    }
    if (s->mdns) {
        if (s->st.receiving && s->receiver)
            airdrop_mdns_publish(s->mdns, s->service_id, s->label, s->address, airdrop_receiver_port(s->receiver),
                                 AIRDROP_RECEIVER_FLAGS);
        else
            airdrop_mdns_publish(s->mdns, NULL, NULL, NULL, 0, 0);
        airdrop_mdns_browse(s->mdns, finding(s));
    }
    if (finding(s)) {
        if (!s->discover_cancel) s->discover_cancel = g_cancellable_new();
        if (!s->retry) s->retry = g_timeout_add_seconds(DISCOVER_RETRY, retry_discover, s);
    } else {
        if (s->discover_cancel) {
            g_cancellable_cancel(s->discover_cancel);
            g_clear_object(&s->discover_cancel);
        }
        g_clear_handle_id(&s->retry, g_source_remove);
        g_hash_table_remove_all(s->candidates);
    }
    changed(s);
}

static void parts_stop(Share *s)
{
    g_clear_pointer(&s->receiver, airdrop_receiver_free);
    if (s->mdns) airdrop_mdns_publish(s->mdns, NULL, NULL, NULL, 0, 0);
    g_clear_pointer(&s->mdns, airdrop_mdns_free);
    if (s->discover_cancel) {
        g_cancellable_cancel(s->discover_cancel);
        g_clear_object(&s->discover_cancel);
    }
    g_clear_handle_id(&s->retry, g_source_remove);
    g_hash_table_remove_all(s->candidates);
}

/* ----------------------------------------------------------------- link */

static gboolean need_link(Share *s)
{
    ShareIncoming *in = s->st.incoming;
    return s->st.setup == SHARE_READY
        && (s->st.receiving || finding(s)
            || (in && (in->state == INCOMING_ASKING || in->state == INCOMING_RECEIVING)));
}

static void link_event(Share *s, GHashTable *ev)
{
    const char *kind = g_hash_table_lookup(ev, "event");
    if (!kind) return;
    if (g_str_equal(kind, "ready")) {
        g_free(s->iface);
        g_free(s->address);
        s->iface = g_strdup(g_hash_table_lookup(ev, "interface"));
        const char *addr = g_hash_table_lookup(ev, "address");
        s->address = addr && s->iface ? (strchr(addr, '%') ? g_strdup(addr) : g_strdup_printf("%s%%%s", addr, s->iface))
                                      : NULL;
        if (!s->iface || !s->address) return;
        s->st.link = LINK_UP;
        share_log(s, "link up on channel %s", (const char *)g_hash_table_lookup(ev, "channel"));
        g_clear_pointer(&s->st.link_error, g_free);
        s->valid_peers = 0;
        s->heard = FALSE;
        parts_apply(s);
        quiet_watch(s);
    } else if (g_str_equal(kind, "peer")) {
        const char *state = g_hash_table_lookup(ev, "state");
        share_log(s, "AWDL peer %s", state ? state : "?");
        if (state && g_str_equal(state, "valid")) {
            s->valid_peers++;
            s->heard = TRUE;
            quiet_watch(s);
        } else if (state && g_str_equal(state, "gone") && s->valid_peers > 0) {
            s->valid_peers--;
        }
        if (!s->iface || !finding(s)) return;
        const char *mac = g_hash_table_lookup(ev, "address");
        if (!mac || !state) return;
        char *key = g_strconcat("awdl:", mac, NULL);
        if (g_str_equal(state, "valid")) {
            char *ll = share_link_local(mac, s->iface);
            if (ll) add_candidate(s, key, ll, AIRDROP_PORT, 0); /* DISC-5 */
            g_free(ll);
        } else if (g_str_equal(state, "gone")) {
            drop_device(s, key);
        }
        g_free(key);
    } else if (g_str_equal(kind, "error")) {
        g_free(s->st.link_error);
        s->st.link_error = g_strdup(g_hash_table_lookup(ev, "message"));
        changed(s);
    }
}

static void read_event(Share *s);

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, &e);
    if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(e);
        return;
    }
    Share *s = data;
    g_clear_error(&e);
    if (!line) return; /* the end: on_link_exit says how it went */
    GHashTable *ev = share_parse_event(line);
    if (ev) {
        link_event(s, ev);
        g_hash_table_unref(ev);
    }
    g_free(line);
    read_event(s);
}

static void read_event(Share *s)
{
    g_data_input_stream_read_line_async(s->events, G_PRIORITY_DEFAULT, s->link_cancel, on_line, s);
}

static void on_link_exit(GObject *src, GAsyncResult *res, gpointer data)
{
    GSubprocess *p = G_SUBPROCESS(src);
    GError *e = NULL;
    g_subprocess_wait_finish(p, res, &e);
    if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(e);
        g_object_unref(p);
        return;
    }
    g_clear_error(&e);
    Share *s = data;
    gboolean expected = s->st.link == LINK_STOPPING;
    int status = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    if (s->proc == p) {
        g_clear_object(&s->proc);
        g_clear_object(&s->events);
        g_clear_object(&s->link_cancel);
        g_clear_handle_id(&s->link_kill, g_source_remove);
    }
    parts_stop(s);
    g_clear_pointer(&s->iface, g_free);
    g_clear_pointer(&s->address, g_free);
    s->valid_peers = 0;
    s->st.link = expected || status == 0 ? LINK_OFF : LINK_FAILED;
    quiet_watch(s);
    if (s->st.link == LINK_FAILED && !s->st.link_error)
        s->st.link_error = g_strdup(status == 3 ? "The Wi-Fi card can't run AirDrop." : "The AirDrop link stopped.");
    if (s->st.link == LINK_FAILED) {
        /* Nothing can work without the link: turn both off. */
        s->st.receiving = FALSE;
        g_clear_handle_id(&s->receive_timer, g_source_remove);
    }
    g_object_unref(p);
    changed(s);
    if (s->st.link == LINK_OFF) update_link(s);
}

static void cert_made(GObject *src, GAsyncResult *res, gpointer data)
{
    Share *s = data;
    GError *e = NULL;
    GTlsCertificate *c = g_task_propagate_pointer(G_TASK(res), &e);
    s->cert_busy = FALSE;
    if (!c) {
        s->st.link = LINK_FAILED;
        g_free(s->st.link_error);
        s->st.link_error = g_strdup(e->message);
        g_error_free(e);
        changed(s);
        return;
    }
    s->cert = c;
    /* "Starting" covered making it; now the link itself starts. */
    s->st.link = LINK_OFF;
    update_link(s);
}

static void cert_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    GError *e = NULL;
    GTlsCertificate *c = airdrop_identity(&e);
    if (c) g_task_return_pointer(task, c, g_object_unref);
    else g_task_return_error(task, e);
}

static gboolean pretend_link(gpointer data);

static void link_start(Share *s)
{
    if (!s->cert && !s->dry) {
        if (s->cert_busy) return;
        s->cert_busy = TRUE;
        s->st.link = LINK_STARTING;
        changed(s);
        GTask *t = g_task_new(NULL, NULL, cert_made, s);
        g_task_run_in_thread(t, cert_thread);
        g_object_unref(t);
        return;
    }
    s->st.link = LINK_STARTING;
    g_clear_pointer(&s->st.link_error, g_free);
    changed(s);
    if (s->dry) {
        g_timeout_add(700, pretend_link, s);
        return;
    }
    const char *argv[] = { awdl_path(), "--wifi", s->st.card, "--name", s->label, NULL };
    GError *e = NULL;
    /* lares-awdl ends, giving the card back, when its stdin closes: Lares
     * ending in any way ends it. What it says about the link goes to a log
     * on this PC only, started afresh each time, for when a card misbehaves. */
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    char *logdir = g_build_filename(g_get_user_cache_dir(), "lares", NULL);
    char *log = g_build_filename(logdir, "lares-awdl.log", NULL);
    if (g_mkdir_with_parents(logdir, 0700) == 0) g_subprocess_launcher_set_stderr_file_path(launcher, log);
    s->proc = g_subprocess_launcher_spawnv(launcher, argv, &e);
    g_object_unref(launcher);
    g_free(logdir);
    g_free(log);
    if (!s->proc) {
        s->st.link = LINK_FAILED;
        s->st.link_error = g_strdup(e->message);
        g_error_free(e);
        changed(s);
        return;
    }
    s->link_cancel = g_cancellable_new();
    s->events = g_data_input_stream_new(g_subprocess_get_stdout_pipe(s->proc));
    read_event(s);
    g_subprocess_wait_async(g_object_ref(s->proc), NULL, on_link_exit, s);
}

static gboolean kill_link(gpointer data)
{
    Share *s = data;
    s->link_kill = 0;
    if (s->proc) g_subprocess_send_signal(s->proc, SIGTERM);
    return G_SOURCE_REMOVE;
}

static void link_stop(Share *s)
{
    share_log(s, "link stopping");
    parts_stop(s);
    if (s->dry) {
        s->st.link = LINK_OFF;
        changed(s);
        return;
    }
    if (!s->proc) {
        s->st.link = LINK_OFF;
        changed(s);
        return;
    }
    s->st.link = LINK_STOPPING;
    changed(s);
    g_output_stream_close(g_subprocess_get_stdin_pipe(s->proc), NULL, NULL);
    if (!s->link_kill) s->link_kill = g_timeout_add_seconds(LINK_STOP_GRACE, kill_link, s);
}

/* The Bluetooth advert wakes phones while devices are looked for, from
 * the moment files are being picked: it needs no link, and a phone takes
 * seconds to wake, which can pass while the link comes up. */
static void apply_ble(Share *s)
{
    gboolean on = finding(s) && s->st.setup == SHARE_READY && !s->dry;
    if (on && !s->ble) s->ble = airdrop_ble_new();
    if (s->ble && airdrop_ble_is_active(s->ble) != on) {
        airdrop_ble_set_active(s->ble, on);
        share_log(s, "Bluetooth advert %s", on ? "on" : "off");
    }
}

static gboolean linger_over(gpointer data)
{
    Share *s = data;
    s->linger = 0;
    if (!need_link(s) && (s->st.link == LINK_UP || s->st.link == LINK_STARTING) && !s->cert_busy) link_stop(s);
    return G_SOURCE_REMOVE;
}

static void update_link(Share *s)
{
    gboolean need = need_link(s);
    apply_ble(s);
    if (need) g_clear_handle_id(&s->linger, g_source_remove);
    if (need && (s->st.link == LINK_OFF || s->st.link == LINK_FAILED)) {
        if (s->st.link == LINK_FAILED && !s->st.receiving && !finding(s)) return;
        link_start(s);
    } else if (!need && (s->st.link == LINK_UP || s->st.link == LINK_STARTING) && !s->cert_busy) {
        /* What used it stops at once (the announcement, the receiver,
         * browsing); the link itself waits a minute for the next use. */
        if (s->st.link == LINK_UP) parts_apply(s);
        if (!s->linger) s->linger = g_timeout_add_seconds(LINK_LINGER, linger_over, s);
    } else if (s->st.link == LINK_UP) {
        parts_apply(s);
    }
    quiet_watch(s);
}

/* -------------------------------------------------------------- dry run */

static const char *const DRY_FILES[] = { "IMG_0412.HEIC", "IMG_0413.HEIC" };

static gboolean pretend_link(gpointer data)
{
    Share *s = data;
    if (s->st.link != LINK_STARTING) return G_SOURCE_REMOVE;
    s->st.link = LINK_UP;
    changed(s);
    quiet_watch(s);
    return G_SOURCE_REMOVE;
}

static gboolean pretend_device(gpointer data)
{
    Share *s = data;
    s->dry_timer = 0;
    if (!finding(s) || device_find(s, "dry") || g_getenv("LARES_DRY_QUIET")) return G_SOURCE_REMOVE;
    s->heard = TRUE;
    quiet_watch(s);
    ShareDevice *d = g_new0(ShareDevice, 1);
    d->key = g_strdup("dry");
    d->name = g_strdup("iPhone");
    d->model = g_strdup("iPhone");
    d->state = DEVICE_FOUND;
    g_ptr_array_add(s->st.devices, d);
    changed(s);
    return G_SOURCE_REMOVE;
}

static gboolean pretend_offer(gpointer data)
{
    Share *s = data;
    if (!s->st.receiving || s->st.incoming || g_getenv("LARES_DRY_QUIET")) return G_SOURCE_REMOVE;
    s->heard = TRUE;
    quiet_watch(s);
    ShareIncoming *in = g_new0(ShareIncoming, 1);
    in->id = 1;
    in->state = INCOMING_ASKING;
    in->sender = g_strdup("iPhone");
    in->names = g_ptr_array_new_with_free_func(g_free);
    for (gsize i = 0; i < G_N_ELEMENTS(DRY_FILES); i++) g_ptr_array_add(in->names, g_strdup(DRY_FILES[i]));
    in->total = 4200000;
    s->st.incoming = in;
    changed(s);
    return G_SOURCE_REMOVE;
}

static gboolean pretend_progress(gpointer data)
{
    Share *s = data;
    gboolean more = FALSE;
    for (guint i = 0; i < s->st.devices->len; i++) {
        ShareDevice *d = s->st.devices->pdata[i];
        if (d->state == DEVICE_ASKING) {
            d->state = DEVICE_SENDING;
            d->total = 3000000;
            more = TRUE;
        } else if (d->state == DEVICE_SENDING) {
            d->sent = MIN(d->total, d->sent + 600000);
            if (d->sent == d->total) d->state = DEVICE_SENT;
            else more = TRUE;
        }
    }
    ShareIncoming *in = s->st.incoming;
    if (in && in->state == INCOMING_RECEIVING) {
        in->done = MIN(in->total, in->done + 900000);
        if (in->done == in->total) {
            in->state = INCOMING_DONE;
            in->saved = g_ptr_array_new_with_free_func(g_free);
            char *folder = download_folder();
            for (guint i = 0; i < in->names->len; i++)
                g_ptr_array_add(in->saved, g_build_filename(folder, in->names->pdata[i], NULL));
            g_free(folder);
        } else {
            more = TRUE;
        }
    }
    changed(s);
    if (!more) s->dry_timer = 0;
    return more ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void pretend_tick(Share *s)
{
    if (!s->dry_timer) s->dry_timer = g_timeout_add(500, pretend_progress, s);
}

/* The pretend iPhone's user takes a moment to accept. */
static gboolean pretend_accept(gpointer data)
{
    pretend_tick(data);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ API */

Share *share_new(void)
{
    Share *s = g_new0(Share, 1);
    s->dry = g_getenv("LARES_DRY_RUN") != NULL;
    s->st.devices = g_ptr_array_new_with_free_func(device_free);
    s->candidates = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, candidate_free);
    s->st.name = computer_name();
    s->service_id = random_id();
    /* Not the machine's host name: that would tell strangers nearby, and
     * avahi, which also answers for it, could rename the machine over the
     * clash. As owly does, a label from the session's service ID. */
    s->label = g_strconcat("lares-", s->service_id, NULL);
    find_card(s);
    check_setup(s);
    read_bands(s);
    return s;
}

const ShareState *share_state(Share *s) { return &s->st; }

void share_watch(Share *s, ShareFn fn, gpointer user)
{
    s->fn = fn;
    s->user = user;
}

void share_refresh(Share *s)
{
    if (s->st.link != LINK_OFF && s->st.link != LINK_FAILED) return; /* the card is lent out */
    gboolean had = s->st.card != NULL;
    find_card(s);
    check_setup(s);
    if (!had && s->st.card) read_bands(s);
    changed(s);
}

static gboolean receive_over(gpointer data)
{
    Share *s = data;
    s->receive_timer = 0;
    share_set_receiving(s, FALSE);
    return G_SOURCE_REMOVE;
}

void share_set_receiving(Share *s, gboolean on)
{
    if (on && s->st.setup != SHARE_READY) return;
    s->st.receiving = on;
    g_clear_handle_id(&s->receive_timer, g_source_remove);
    if (on) {
        log_session(s, "receiving on");
        s->st.receive_until = g_get_monotonic_time() + SHARE_RECEIVE_SECONDS * G_USEC_PER_SEC;
        s->receive_timer = g_timeout_add_seconds(SHARE_RECEIVE_SECONDS, receive_over, s);
        if (s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
        if (s->dry) g_timeout_add(4000, pretend_offer, s);
    }
    changed(s);
    update_link(s);
}

void share_answer(Share *s, gboolean accept)
{
    ShareIncoming *in = s->st.incoming;
    if (!in || in->state != INCOMING_ASKING) return;
    if (s->receiver) airdrop_receiver_answer(s->receiver, in->id, accept);
    if (accept) {
        in->state = INCOMING_RECEIVING;
        if (s->dry) pretend_tick(s);
    } else {
        incoming_free(in);
        s->st.incoming = NULL;
    }
    changed(s);
    update_link(s);
}

void share_dismiss_incoming(Share *s)
{
    ShareIncoming *in = s->st.incoming;
    if (!in || in->state == INCOMING_ASKING || in->state == INCOMING_RECEIVING) return;
    incoming_free(in);
    s->st.incoming = NULL;
    changed(s);
}

void share_set_looking(Share *s, gboolean on)
{
    if (on == s->looking || (on && s->st.setup != SHARE_READY)) return;
    s->looking = on;
    if (on) {
        log_session(s, "looking while files are picked");
        if (s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
        if (s->dry && !s->dry_timer) s->dry_timer = g_timeout_add(1500, pretend_device, s);
    } else if (!s->st.sending) {
        g_ptr_array_set_size(s->st.devices, 0);
        g_hash_table_remove_all(s->candidates);
    }
    changed(s);
    update_link(s);
}

void share_set_sending(Share *s, const char *const *paths)
{
    gboolean on = paths && paths[0];
    if (on && s->st.setup != SHARE_READY) return;
    if (!on) share_cancel_send(s);
    g_strfreev(s->st.paths);
    s->st.paths = on ? g_strdupv((char **)paths) : NULL;
    /* What was found while the files were picked stays. */
    gboolean keep = on && s->looking;
    s->looking = FALSE;
    s->st.sending = on;
    if (on) log_session(s, keep ? "files picked" : "sending");
    if (!keep) {
        g_ptr_array_set_size(s->st.devices, 0);
        g_hash_table_remove_all(s->candidates);
    }
    if (on && s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
    if (on && s->dry && !s->dry_timer) s->dry_timer = g_timeout_add(1500, pretend_device, s);
    changed(s);
    update_link(s);
}

typedef struct {
    Share *s;
    char *key;
    GCancellable *cancel;
} SendCtx;

static void send_progress(gboolean accepted, guint64 sent, guint64 total, gpointer user)
{
    SendCtx *c = user;
    if (g_cancellable_is_cancelled(c->cancel)) return;
    ShareDevice *d = device_find(c->s, c->key);
    if (!d) return;
    if (accepted && d->state == DEVICE_ASKING) {
        d->state = DEVICE_SENDING;
        share_log(c->s, "they accepted");
    }
    d->sent = sent;
    d->total = total;
    changed(c->s);
}

static void send_done(GObject *src, GAsyncResult *res, gpointer user)
{
    SendCtx *c = user;
    GError *e = NULL;
    gboolean ok = airdrop_send_finish(res, &e);
    if (!g_cancellable_is_cancelled(c->cancel)) {
        Share *s = c->s;
        ShareDevice *d = device_find(s, c->key);
        share_log(s, ok ? "sent" : "send ended without sending");
        if (d) {
            d->state = ok ? DEVICE_SENT : g_error_matches(e, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)
                                          ? DEVICE_DECLINED : DEVICE_FAILED;
            g_free(d->message);
            d->message = e ? g_strdup(e->message) : NULL;
        }
        if (s->send_cancel == c->cancel) {
            g_clear_object(&s->send_cancel);
            g_clear_pointer(&s->send_key, g_free);
        }
        changed(s);
    }
    g_clear_error(&e);
    g_object_unref(c->cancel);
    g_free(c->key);
    g_free(c);
}

void share_send_to(Share *s, const char *key)
{
    ShareDevice *d = device_find(s, key);
    if (!d || !s->st.paths || s->send_key) return;
    d->state = DEVICE_ASKING;
    share_log(s, "asking to send");
    d->sent = d->total = 0;
    g_clear_pointer(&d->message, g_free);
    s->send_key = g_strdup(key);
    changed(s);
    if (s->dry) {
        g_timeout_add(2000, pretend_accept, s);
        g_clear_pointer(&s->send_key, g_free);
        return;
    }
    s->send_cancel = g_cancellable_new();
    SendCtx *c = g_new0(SendCtx, 1);
    c->s = s;
    c->key = g_strdup(key);
    c->cancel = g_object_ref(s->send_cancel);
    AirdropSendRequest req = {
        .address = d->address, .port = d->port, .flags = d->flags, .name = s->st.name,
        .id = s->service_id, .paths = s->st.paths,
    };
    airdrop_send_async(&req, c->cancel, send_progress, c, send_done, c);
}

void share_cancel_send(Share *s)
{
    if (s->send_cancel) g_cancellable_cancel(s->send_cancel);
    g_clear_object(&s->send_cancel);
    g_clear_pointer(&s->send_key, g_free);
    for (guint i = 0; i < s->st.devices->len; i++) {
        ShareDevice *d = s->st.devices->pdata[i];
        if (d->state == DEVICE_ASKING || d->state == DEVICE_SENDING) d->state = DEVICE_FOUND;
    }
    changed(s);
}

/* Ends the link now, without the minute it otherwise waits for a next use:
 * for a caller that is ending itself and must see the card given back. */
void share_stop_link(Share *s)
{
    g_clear_handle_id(&s->linger, g_source_remove);
    if (s->st.link == LINK_UP || (s->st.link == LINK_STARTING && !s->cert_busy)) link_stop(s);
}
