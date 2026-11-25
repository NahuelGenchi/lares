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
/* s between /Discover re-checks on already-listed devices so peers switched
 * to Contacts Only or Receiving Off disappear instead of staying stale. */
#define DISCOVER_RECHECK 5
#define DISCOVER_MISS_RETRY 2
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
    guint missed;
    gint64 next_probe;
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
    ShareDiscoverMode prev_mode;
    guint receive_timer;

    GHashTable *candidates;   /* key -> Candidate */
    GCancellable *discover_cancel;
    guint retry;

    GCancellable *send_cancel;
    char *send_key;
    guint dry_timer;

    gboolean looking;         /* finding devices while files are being picked */
    GHashTable *awdl_peers;   /* MACs of the AWDL peers the link counts as valid now */
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

/* Looking for devices to send to: browsing on screen, files picked, or being picked. */
static gboolean finding(Share *s)
{
    return s->st.browsing || s->st.sending || s->looking;
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
    s->heard = g_hash_table_size(s->awdl_peers) > 0;
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

void share_contact_free(ShareContact *c)
{
    if (!c) return;
    g_free(c->name);
    g_free(c->device);
    g_free(c->email);
    g_free(c->phone);
    g_free(c->photo);
    g_free(c);
}

static void incoming_free(ShareIncoming *in)
{
    if (!in) return;
    g_free(in->sender);
    g_free(in->sender_model);
    g_free(in->matched_hash);
    if (in->names) g_ptr_array_unref(in->names);
    if (in->saved) g_ptr_array_unref(in->saved);
    if (in->file_icon) g_bytes_unref(in->file_icon);
    g_free(in->message);
    g_free(in);
}

/* ----------------------------------------------------- device & contacts */

static gboolean contains_ci(const char *hay, const char *needle)
{
    if (!hay || !*hay) return FALSE;
    char *lo = g_ascii_strdown(hay, -1);
    gboolean ok = strstr(lo, needle) != NULL;
    g_free(lo);
    return ok;
}

static ShareDeviceKind kind_from_str(const char *s)
{
    if (!s || !*s) return DEVICE_KIND_OTHER;
    if (contains_ci(s, "iphone") || contains_ci(s, "ipod")) return DEVICE_KIND_PHONE;
    if (contains_ci(s, "ipad")) return DEVICE_KIND_TABLET;
    if (contains_ci(s, "macbook")) return DEVICE_KIND_LAPTOP;
    if (contains_ci(s, "watch")) return DEVICE_KIND_WATCH;
    if (contains_ci(s, "imac") || contains_ci(s, "macmini") || contains_ci(s, "mac mini")
        || contains_ci(s, "macstudio") || contains_ci(s, "mac studio")
        || contains_ci(s, "macpro") || contains_ci(s, "mac pro")
        || contains_ci(s, "windows") || contains_ci(s, "linux"))
        return DEVICE_KIND_DESKTOP;
    if (g_ascii_strncasecmp(s, "mac", 3) == 0) return DEVICE_KIND_LAPTOP;
    return DEVICE_KIND_OTHER;
}

ShareDeviceKind share_device_kind(const char *model, const char *name)
{
    ShareDeviceKind k = kind_from_str(model);
    return k != DEVICE_KIND_OTHER ? k : kind_from_str(name);
}

static const char *label_from_str(const char *s)
{
    if (!s || !*s) return NULL;
    if (contains_ci(s, "iphone")) return "iPhone";
    if (contains_ci(s, "ipad")) return "iPad";
    if (contains_ci(s, "macbookair") || contains_ci(s, "macbook air")) return "MacBook Air";
    if (contains_ci(s, "macbookpro") || contains_ci(s, "macbook pro")) return "MacBook Pro";
    if (contains_ci(s, "macbook")) return "MacBook";
    if (contains_ci(s, "imac")) return "iMac";
    if (contains_ci(s, "macmini") || contains_ci(s, "mac mini")) return "Mac mini";
    if (contains_ci(s, "macstudio") || contains_ci(s, "mac studio")) return "Mac Studio";
    if (contains_ci(s, "macpro") || contains_ci(s, "mac pro")) return "Mac Pro";
    if (contains_ci(s, "watch")) return "Apple Watch";
    if (contains_ci(s, "realitydevice") || contains_ci(s, "vision")) return "Apple Vision Pro";
    if (contains_ci(s, "windows")) return "Windows";
    if (contains_ci(s, "linux")) return "Linux";
    if (g_ascii_strncasecmp(s, "mac", 3) == 0) return "Mac";
    return NULL;
}

const char *share_model_label(const char *model, const char *name)
{
    const char *l = label_from_str(model);
    if (l) return l;
    l = label_from_str(name);
    return l ? l : "Apple Device";
}

char *share_initials(const char *name)
{
    if (!name || !*name) return g_strdup("");
    const char *cut = strstr(name, "'s ");
    if (!cut) cut = strstr(name, "\xE2\x80\x99s ");
    char *base = cut && cut > name ? g_strndup(name, (gsize)(cut - name)) : g_strdup(name);
    if (contains_ci(base, "iphone") || contains_ci(base, "ipad") || contains_ci(base, "macbook")
        || contains_ci(base, "imac") || contains_ci(base, "apple device")) {
        g_free(base);
        return g_strdup("");
    }
    char out[3] = { 0 };
    int n = 0;
    gboolean in_word = FALSE;
    for (const char *p = base; *p && n < 2; p++) {
        if (g_ascii_isalnum(*p)) {
            if (!in_word) {
                out[n++] = g_ascii_toupper(*p);
                in_word = TRUE;
            }
        } else if (*p == ' ' || *p == '-' || *p == '_') {
            in_word = FALSE;
        }
    }
    g_free(base);
    return g_strdup(out);
}

char *share_hash_email(const char *email)
{
    if (!email) return NULL;
    char *copy = g_strstrip(g_strdup(email));
    char *lo = g_ascii_strdown(copy, -1);
    g_free(copy);
    if (!*lo || !strchr(lo, '@')) {
        g_free(lo);
        return NULL;
    }
    char *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA256, lo, -1);
    g_free(lo);
    return hex;
}

GPtrArray *share_hash_phone_variants(const char *phone)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!phone) return out;
    GString *digits = g_string_new(NULL);
    for (const char *p = phone; *p; p++)
        if (g_ascii_isdigit(*p)) g_string_append_c(digits, *p);
    if (digits->len >= 4) {
        char *plus = g_strconcat("+", digits->str, NULL);
        g_ptr_array_add(out, g_compute_checksum_for_string(G_CHECKSUM_SHA256, plus, -1));
        g_ptr_array_add(out, g_compute_checksum_for_string(G_CHECKSUM_SHA256, digits->str, -1));
        g_free(plus);
    }
    g_string_free(digits, TRUE);
    return out;
}

static char *contacts_path(gboolean dry)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "lares", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *p = g_build_filename(dir, dry ? "airdrop-contacts-dry.ini" : "airdrop-contacts.ini", NULL);
    g_free(dir);
    return p;
}

static void load_contacts(Share *s)
{
    g_ptr_array_set_size(s->st.contacts, 0);
    char *path = contacts_path(s->dry);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **groups = g_key_file_get_groups(kf, &n);
        for (gsize i = 0; i < n; i++) {
            ShareContact *c = g_new0(ShareContact, 1);
            c->name = g_key_file_get_string(kf, groups[i], "name", NULL);
            c->device = g_key_file_get_string(kf, groups[i], "device", NULL);
            c->email = g_key_file_get_string(kf, groups[i], "email", NULL);
            c->phone = g_key_file_get_string(kf, groups[i], "phone", NULL);
            c->photo = g_key_file_get_string(kf, groups[i], "photo", NULL);
            if ((c->name && *c->name) || (c->device && *c->device) || (c->email && *c->email) || (c->phone && *c->phone))
                g_ptr_array_add(s->st.contacts, c);
            else
                share_contact_free(c);
        }
        g_strfreev(groups);
    }
    g_key_file_unref(kf);
    g_free(path);
}

static char **build_contact_hash_strv(GPtrArray *contacts)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; contacts && i < contacts->len; i++) {
        ShareContact *c = contacts->pdata[i];
        char *eh = share_hash_email(c->email);
        if (eh) g_ptr_array_add(arr, eh);
        GPtrArray *ph = share_hash_phone_variants(c->phone);
        for (guint j = 0; j < ph->len; j++) g_ptr_array_add(arr, g_strdup(ph->pdata[j]));
        g_ptr_array_unref(ph);
    }
    g_ptr_array_add(arr, NULL);
    return (char **)g_ptr_array_free(arr, FALSE);
}

static void sync_receiver_policy(Share *s)
{
    if (!s->receiver) return;
    char **hashes = build_contact_hash_strv(s->st.contacts);
    airdrop_receiver_set_policy(s->receiver, s->st.mode == SHARE_DISCOVER_CONTACTS, (const char *const *)hashes);
    g_strfreev(hashes);
}

static char *normalize_contact_name(const char *s)
{
    if (!s || !*s) return g_strdup("");
    GString *out = g_string_sized_new(strlen(s));
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        /* Normalize UTF-8 curly apostrophes (U+2018 / U+2019) to ASCII '\''. */
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) {
            g_string_append_c(out, '\'');
            p += 2;
            continue;
        }
        g_string_append_c(out, g_ascii_tolower((char)*p));
    }
    char *res = g_string_free(out, FALSE);
    return g_strstrip(res);
}

static char *owner_prefix_norm(const char *norm)
{
    if (!norm) return NULL;
    const char *p = strstr(norm, "'s ");
    if (p && (size_t)(p - norm) >= 2) {
        char *sub = g_strndup(norm, (gsize)(p - norm));
        return g_strstrip(sub);
    }
    return NULL;
}

static gboolean contact_matches_exact(const ShareContact *c, const char *norm_name)
{
    if (!c || !norm_name || !*norm_name) return FALSE;
    if (c->device && *c->device) {
        char *cd = normalize_contact_name(c->device);
        gboolean ok = g_str_equal(cd, norm_name);
        g_free(cd);
        if (ok) return TRUE;
    }
    if (c->name && *c->name) {
        char *cn = normalize_contact_name(c->name);
        gboolean ok = g_str_equal(cn, norm_name);
        g_free(cn);
        if (ok) return TRUE;
    }
    return FALSE;
}

static gboolean contact_matches_owner(const ShareContact *c, const char *owner)
{
    if (!c || !owner || !*owner || !c->name || !*c->name) return FALSE;
    char *cn = normalize_contact_name(c->name);
    gboolean ok = g_str_equal(cn, owner);
    g_free(cn);
    return ok;
}

const ShareContact *share_find_contact(const ShareState *st, const char *name, const char *matched_hash)
{
    if (!st || !st->contacts) return NULL;
    if (matched_hash && *matched_hash) {
        for (guint i = 0; i < st->contacts->len; i++) {
            ShareContact *c = st->contacts->pdata[i];
            char *eh = share_hash_email(c->email);
            gboolean ok = eh && g_ascii_strcasecmp(eh, matched_hash) == 0;
            g_free(eh);
            if (ok) return c;
            GPtrArray *ph = share_hash_phone_variants(c->phone);
            for (guint j = 0; j < ph->len && !ok; j++)
                if (g_ascii_strcasecmp(ph->pdata[j], matched_hash) == 0) ok = TRUE;
            g_ptr_array_unref(ph);
            if (ok) return c;
        }
    }
    if (!name || !*name) return NULL;
    char *norm = normalize_contact_name(name);
    const ShareContact *found = NULL;
    for (guint i = 0; i < st->contacts->len; i++) {
        ShareContact *c = st->contacts->pdata[i];
        if (contact_matches_exact(c, norm)) {
            found = c;
            break;
        }
    }
    if (!found) {
        char *owner = owner_prefix_norm(norm);
        if (owner && *owner) {
            for (guint i = 0; i < st->contacts->len; i++) {
                ShareContact *c = st->contacts->pdata[i];
                if (contact_matches_owner(c, owner)) {
                    found = c;
                    break;
                }
            }
        }
        g_free(owner);
    }
    g_free(norm);
    return found;
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
    in->sender_model = g_strdup(o->sender_model);
    in->matched_hash = g_strdup(o->matched_hash);
    in->links = o->links;
    in->names = g_ptr_array_ref(o->names);
    if (o->file_icon) in->file_icon = g_bytes_ref(o->file_icon);
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
    /* The receiver's messages are its own fixed texts: no names. */
    share_log(s, "receiving failed: %s", message ? message : "?");
    g_free(in->message);
    in->message = g_strdup(message);
    changed(s);
    update_link(s);
}

/* The receiver's trace holds formats, sizes and counts, never a name. */
static void receiver_trace(guint id, const char *line, gpointer user)
{
    share_log(user, "%s", line);
}

static const AirdropReceiverCallbacks RECEIVER = {
    receiver_offer, receiver_withdrawn, receiver_progress, receiver_received, receiver_failed, receiver_trace,
};

static void discovered(GObject *src, GAsyncResult *res, gpointer data);

static gboolean transfer_active(Share *s)
{
    if (s->send_key) return TRUE;
    return s->st.incoming && (s->st.incoming->state == INCOMING_ASKING || s->st.incoming->state == INCOMING_RECEIVING);
}

static gboolean address_trying(Share *s, const char *address)
{
    if (!address || !*address) return FALSE;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, s->candidates);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        Candidate *cand = v;
        if (cand->trying && cand->address && g_ascii_strcasecmp(cand->address, address) == 0) return TRUE;
    }
    return FALSE;
}

static void unmark_address(Share *s, const char *address, gint64 next_probe)
{
    if (!address || !*address) return;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, s->candidates);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        Candidate *cand = v;
        if (cand->address && g_ascii_strcasecmp(cand->address, address) == 0) {
            cand->listed = FALSE;
            cand->missed = 0;
            cand->next_probe = next_probe;
        }
    }
}

static gboolean remove_idle_device_by(Share *s, const char *key, const char *address)
{
    gboolean removed = FALSE;
    for (guint i = s->st.devices->len; i > 0; i--) {
        ShareDevice *d = s->st.devices->pdata[i - 1];
        gboolean match = (key && *key && g_str_equal(d->key, key))
                         || (address && *address && d->address && g_ascii_strcasecmp(d->address, address) == 0);
        if (match && d->state == DEVICE_FOUND) {
            g_ptr_array_remove_index(s->st.devices, i - 1);
            removed = TRUE;
        }
    }
    return removed;
}

static void try_discover(Share *s, Candidate *c)
{
    if (c->trying || !s->discover_cancel) return;
    gint64 now = g_get_monotonic_time();
    if (c->next_probe && now < c->next_probe) return;
    if (address_trying(s, c->address)) return;
    if (c->listed) {
        if (transfer_active(s)) return;
        ShareDevice *d = device_at(s, c->address);
        if (d && !g_str_equal(d->key, c->key)) {
            Candidate *primary = g_hash_table_lookup(s->candidates, d->key);
            if (primary && primary->listed) return;
        }
    }
    c->trying = TRUE;
    if (!c->listed)
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
        gint64 now = g_get_monotonic_time();
        if (!peer) {
            gboolean explicit_refusal = (e == NULL);
            if (c->listed || device_at(s, c->address)) {
                if (explicit_refusal) {
                    share_log(s, "device is no longer discoverable");
                    char *addr = g_strdup(c->address);
                    unmark_address(s, addr, now + (gint64)DISCOVER_MISS_RETRY * G_USEC_PER_SEC);
                    if (remove_idle_device_by(s, key, addr)) changed(s);
                    g_free(addr);
                } else {
                    c->missed++;
                    gboolean refused = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED);
                    guint limit = refused ? 2 : 3;
                    if (c->missed >= limit) {
                        share_log(s, "device stopped answering: %s", e->message);
                        char *addr = g_strdup(c->address);
                        unmark_address(s, addr, now + (gint64)DISCOVER_MISS_RETRY * G_USEC_PER_SEC);
                        if (remove_idle_device_by(s, key, addr)) changed(s);
                        g_free(addr);
                    } else {
                        c->next_probe = now + (gint64)(refused ? DISCOVER_RETRY : DISCOVER_MISS_RETRY) * G_USEC_PER_SEC;
                    }
                }
            } else {
                if (explicit_refusal) c->next_probe = now + (gint64)DISCOVER_MISS_RETRY * G_USEC_PER_SEC;
                share_log(s, "not listed yet: %s", e ? e->message : "it didn't list us");
            }
        } else {
            gboolean was_listed = c->listed;
            gint64 next = now + (gint64)DISCOVER_RECHECK * G_USEC_PER_SEC;
            c->listed = TRUE;
            c->missed = 0;
            c->next_probe = next;
            GHashTableIter it;
            gpointer v;
            g_hash_table_iter_init(&it, s->candidates);
            while (g_hash_table_iter_next(&it, NULL, &v)) {
                Candidate *sib = v;
                if (sib->address && c->address && g_ascii_strcasecmp(sib->address, c->address) == 0) {
                    sib->listed = TRUE;
                    sib->missed = 0;
                    sib->next_probe = next;
                }
            }
            ShareDevice *d = device_find(s, key);
            ShareDevice *same = device_at(s, c->address);
            gboolean dirty = FALSE;
            if (!d && same) {
                d = same; /* found both over mDNS and on AWDL alone: one device */
                if (c->flags && d->flags != c->flags) { d->flags = c->flags; dirty = TRUE; }
            }
            if (!d) {
                d = g_new0(ShareDevice, 1);
                d->key = g_strdup(key);
                d->address = g_strdup(c->address);
                d->port = c->port;
                d->flags = c->flags;
                d->state = DEVICE_FOUND;
                g_ptr_array_add(s->st.devices, d);
                dirty = TRUE;
            }
            if (g_strcmp0(d->name, peer->name) != 0 || g_strcmp0(d->model, peer->model) != 0) {
                g_free(d->name);
                g_free(d->model);
                d->name = g_strdup(peer->name);
                d->model = g_strdup(peer->model);
                dirty = TRUE;
            }
            if (!was_listed) share_log(s, "listed a device");
            if (dirty) changed(s);
        }
    }
    airdrop_peer_free(peer);
    g_clear_error(&e);
    g_free(key);
}

/* Receivers refuse until a Bluetooth advert has woken them, which can take
 * many seconds: keep asking while looking (DISC-5), and re-verify listed devices. */
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
        c->missed = 0;
        c->next_probe = 0;
    }
    if (flags) c->flags = flags;
    ShareDevice *d = device_find(s, key);
    if (d && flags) d->flags = flags;
    try_discover(s, c);
}

/* DISC-5: asks an AWDL peer directly, at its link-local address. */
static void ask_peer(Share *s, const char *mac)
{
    char *key = g_strconcat("awdl:", mac, NULL);
    char *ll = share_link_local(mac, s->iface);
    if (ll) add_candidate(s, key, ll, AIRDROP_PORT, 0);
    g_free(ll);
    g_free(key);
}

static void drop_device(Share *s, const char *key)
{
    Candidate *c = g_hash_table_lookup(s->candidates, key);
    char *addr = c && c->address ? g_strdup(c->address) : NULL;
    g_hash_table_remove(s->candidates, key);
    if (!addr) {
        ShareDevice *d = device_find(s, key);
        if (d && d->address) addr = g_strdup(d->address);
    }
    unmark_address(s, addr, g_get_monotonic_time() + (gint64)DISCOVER_MISS_RETRY * G_USEC_PER_SEC);
    if (remove_idle_device_by(s, key, addr)) changed(s);
    g_free(addr);
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
        } else {
            sync_receiver_policy(s);
        }
    } else if ((s->st.receiving || incoming_on) && s->receiver) {
        sync_receiver_policy(s);
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
        /* Peers already met while only receiving are asked too (issue #107). */
        GHashTableIter it;
        gpointer mac;
        g_hash_table_iter_init(&it, s->awdl_peers);
        while (g_hash_table_iter_next(&it, &mac, NULL)) ask_peer(s, mac);
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
        g_hash_table_remove_all(s->awdl_peers);
        s->heard = FALSE;
        parts_apply(s);
        quiet_watch(s);
    } else if (g_str_equal(kind, "peer")) {
        const char *state = g_hash_table_lookup(ev, "state");
        const char *mac = g_hash_table_lookup(ev, "address");
        share_log(s, "AWDL peer %s", state ? state : "?");
        if (!state || !mac) return;
        if (g_str_equal(state, "valid")) {
            g_hash_table_add(s->awdl_peers, g_strdup(mac));
            s->heard = TRUE;
            quiet_watch(s);
            if (s->iface && finding(s)) ask_peer(s, mac);
        } else if (g_str_equal(state, "gone")) {
            g_hash_table_remove(s->awdl_peers, mac);
            if (!s->iface || !finding(s)) return;
            char *key = g_strconcat("awdl:", mac, NULL);
            drop_device(s, key);
            g_free(key);
        }
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
    g_hash_table_remove_all(s->awdl_peers);
    s->st.link = expected || status == 0 ? LINK_OFF : LINK_FAILED;
    quiet_watch(s);
    if (s->st.link == LINK_FAILED && !s->st.link_error)
        s->st.link_error = g_strdup(status == 3 ? "The Wi-Fi card can't run AirDrop." : "The AirDrop link stopped.");
    if (s->st.link == LINK_FAILED) {
        /* Nothing can work without the link: turn both off. */
        s->st.mode = SHARE_DISCOVER_OFF;
        s->st.receiving = FALSE;
        s->st.browsing = FALSE;
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

static void add_dry_device(Share *s, const char *key, const char *name, const char *model, DeviceState state,
                           guint64 sent, guint64 total)
{
    ShareDevice *d = g_new0(ShareDevice, 1);
    d->key = g_strdup(key);
    d->name = g_strdup(name);
    d->model = g_strdup(model);
    d->state = state;
    d->sent = sent;
    d->total = total;
    g_ptr_array_add(s->st.devices, d);
}

static void add_sample_dry_devices(Share *s)
{
    g_ptr_array_set_size(s->st.devices, 0);
    add_dry_device(s, "susuin", "Susuin", "iPhone17,1", DEVICE_FOUND, 0, 0);
    add_dry_device(s, "ana", "Ana's iPhone", "iPhone16,2", DEVICE_FOUND, 0, 0);
    add_dry_device(s, "bruno", "Bruno's MacBook Air", "MacBookAir10,1", DEVICE_FOUND, 0, 0);
}

static gboolean pretend_device(gpointer data)
{
    Share *s = data;
    s->dry_timer = 0;
    if (!finding(s) || s->st.devices->len > 0 || g_getenv("LARES_DRY_QUIET")) return G_SOURCE_REMOVE;
    s->heard = TRUE;
    quiet_watch(s);
    add_sample_dry_devices(s);
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
    in->sender = g_strdup("Susuin");
    in->sender_model = g_strdup("iPhone17,1");
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
            s->st.xfer_start_us = g_get_monotonic_time();
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

static void init_dry_scene(Share *s)
{
    const char *want = g_getenv("LARES_DRY_SHARE");
    if (!want) want = "";
    s->st.setup = SHARE_READY;
    if (g_str_equal(want, "setup")) {
        s->st.setup = SHARE_NEEDS_ALLOW;
        return;
    }
    if (g_str_equal(want, "off")) {
        s->st.link = LINK_UP;
        s->st.mode = SHARE_DISCOVER_OFF;
        s->st.browsing = TRUE;
        add_sample_dry_devices(s);
        return;
    }
    if (g_str_equal(want, "contacts")) {
        s->st.link = LINK_UP;
        s->st.mode = SHARE_DISCOVER_CONTACTS;
        s->st.receiving = TRUE;
        s->st.browsing = TRUE;
        add_sample_dry_devices(s);
        return;
    }
    if (g_str_equal(want, "incoming") || g_str_equal(want, "incoming-receiving")
        || g_str_equal(want, "incoming-done") || g_str_equal(want, "incoming-failed")) {
        s->st.link = LINK_UP;
        s->st.mode = SHARE_DISCOVER_EVERYONE;
        s->st.receiving = TRUE;
        s->st.browsing = TRUE;
        s->st.receive_until = g_get_monotonic_time() + 540 * G_USEC_PER_SEC;
        s->st.xfer_start_us = g_get_monotonic_time() - 12 * G_USEC_PER_SEC;
        ShareIncoming *in = g_new0(ShareIncoming, 1);
        in->id = 1;
        in->sender = g_strdup("Susuin");
        in->sender_model = g_strdup("iPhone17,1");
        in->names = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(in->names, g_strdup("61541ab060fa3.PNG"));
        in->total = 13200000;
        if (g_str_equal(want, "incoming")) {
            in->state = INCOMING_ASKING;
        } else if (g_str_equal(want, "incoming-receiving")) {
            in->state = INCOMING_RECEIVING;
            in->done = 9900000;
        } else if (g_str_equal(want, "incoming-done")) {
            in->state = INCOMING_DONE;
            in->done = in->total;
            in->saved = g_ptr_array_new_with_free_func(g_free);
            char *folder = download_folder();
            g_ptr_array_add(in->saved, g_build_filename(folder, "61541ab060fa3.PNG", NULL));
            g_free(folder);
        } else {
            in->state = INCOMING_FAILED;
            in->message = g_strdup("Susuin canceled the copy.");
        }
        s->st.incoming = in;
        add_dry_device(s, "susuin", "Susuin", "iPhone17,1", DEVICE_FOUND, 0, 0);
        return;
    }
    if (g_str_equal(want, "sending")) {
        s->st.link = LINK_UP;
        s->st.mode = SHARE_DISCOVER_EVERYONE;
        s->st.receiving = TRUE;
        s->st.browsing = TRUE;
        s->st.sending = TRUE;
        s->st.receive_until = g_get_monotonic_time() + 540 * G_USEC_PER_SEC;
        s->st.xfer_start_us = g_get_monotonic_time() - 8 * G_USEC_PER_SEC;
        const char *paths[] = { "Holiday.mov", "Notes.pdf", NULL };
        s->st.paths = g_strdupv((char **)paths);
        add_dry_device(s, "a", "Ana's iPhone", "iPhone16,2", DEVICE_SENDING, 41000000, 96000000);
        add_dry_device(s, "b", "Living room iPad", "iPad14,5", DEVICE_FOUND, 0, 0);
        add_dry_device(s, "c", "Bruno's MacBook Air", "MacBookAir10,1", DEVICE_DECLINED, 0, 0);
        return;
    }
    s->st.link = LINK_UP;
    s->st.mode = SHARE_DISCOVER_EVERYONE;
    s->st.receiving = TRUE;
    s->st.browsing = TRUE;
    s->st.receive_until = g_get_monotonic_time() + 540 * G_USEC_PER_SEC;
    if (!g_getenv("LARES_DRY_QUIET")) add_sample_dry_devices(s);
    else s->st.quiet = TRUE;
}

/* ------------------------------------------------------------------ API */

Share *share_new(void)
{
    Share *s = g_new0(Share, 1);
    s->dry = g_getenv("LARES_DRY_RUN") != NULL;
    s->st.devices = g_ptr_array_new_with_free_func(device_free);
    s->st.contacts = g_ptr_array_new_with_free_func((GDestroyNotify)share_contact_free);
    s->candidates = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, candidate_free);
    s->awdl_peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->st.name = computer_name();
    s->service_id = random_id();
    /* Not the machine's host name: that would tell strangers nearby, and
     * avahi, which also answers for it, could rename the machine over the
     * clash. As owly does, a label from the session's service ID. */
    s->label = g_strconcat("lares-", s->service_id, NULL);
    load_contacts(s);
    find_card(s);
    check_setup(s);
    read_bands(s);
    if (s->dry) init_dry_scene(s);
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
    share_set_discover_mode(s, s->prev_mode);
    return G_SOURCE_REMOVE;
}

void share_set_discover_mode(Share *s, ShareDiscoverMode mode)
{
    if (mode != SHARE_DISCOVER_OFF && s->st.setup != SHARE_READY) return;
    if (mode == SHARE_DISCOVER_EVERYONE && s->st.mode != SHARE_DISCOVER_EVERYONE) s->prev_mode = s->st.mode;
    s->st.mode = mode;
    s->st.receiving = mode != SHARE_DISCOVER_OFF;
    g_clear_handle_id(&s->receive_timer, g_source_remove);
    if (mode == SHARE_DISCOVER_EVERYONE) {
        log_session(s, "discoverable: everyone for 10 minutes");
        s->st.receive_until = g_get_monotonic_time() + SHARE_RECEIVE_SECONDS * G_USEC_PER_SEC;
        s->receive_timer = g_timeout_add_seconds(SHARE_RECEIVE_SECONDS, receive_over, s);
        if (s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
        if (s->dry && !s->st.incoming) g_timeout_add(4000, pretend_offer, s);
    } else if (mode == SHARE_DISCOVER_CONTACTS) {
        log_session(s, "discoverable: contacts only");
        s->st.receive_until = 0;
        if (s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
    } else {
        s->st.receive_until = 0;
    }
    sync_receiver_policy(s);
    changed(s);
    update_link(s);
}

void share_set_receiving(Share *s, gboolean on)
{
    share_set_discover_mode(s, on ? SHARE_DISCOVER_EVERYONE : SHARE_DISCOVER_OFF);
}

void share_set_browsing(Share *s, gboolean on)
{
    if (on == s->st.browsing || (on && s->st.setup != SHARE_READY)) return;
    s->st.browsing = on;
    if (on) {
        log_session(s, "browsing nearby devices");
        if (s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
        if (s->dry && !s->st.devices->len && !s->dry_timer) s->dry_timer = g_timeout_add(1200, pretend_device, s);
    } else if (!s->st.sending && !s->looking) {
        g_ptr_array_set_size(s->st.devices, 0);
        g_hash_table_remove_all(s->candidates);
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
        s->st.xfer_start_us = g_get_monotonic_time();
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
    } else if (!s->st.sending && !s->st.browsing) {
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
    /* What was found while browsing or picking stays. */
    gboolean keep = (on && (s->looking || s->st.browsing)) || s->st.browsing;
    s->looking = FALSE;
    s->st.sending = on;
    if (on) log_session(s, keep ? "files picked" : "sending");
    if (!keep) {
        g_ptr_array_set_size(s->st.devices, 0);
        g_hash_table_remove_all(s->candidates);
    }
    if (on && s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
    if (on && s->dry && !s->st.devices->len && !s->dry_timer) s->dry_timer = g_timeout_add(1500, pretend_device, s);
    changed(s);
    update_link(s);
}

void share_send_files_to(Share *s, const char *key, const char *const *paths)
{
    if (!paths || !paths[0] || s->st.setup != SHARE_READY || !device_find(s, key)) return;
    if (s->send_key) share_cancel_send(s);
    g_strfreev(s->st.paths);
    s->st.paths = g_strdupv((char **)paths);
    s->looking = FALSE;
    s->st.sending = TRUE;
    log_session(s, "sending to device");
    if (s->st.link == LINK_FAILED) s->st.link = LINK_OFF;
    update_link(s);
    share_send_to(s, key);
}

void share_dismiss_device(Share *s, const char *key)
{
    ShareDevice *d = device_find(s, key);
    if (!d || d->state == DEVICE_ASKING || d->state == DEVICE_SENDING) return;
    char *addr = g_strdup(d->address);
    d->state = DEVICE_FOUND;
    d->sent = d->total = 0;
    g_clear_pointer(&d->message, g_free);
    if (!s->dry && addr && *addr) {
        gboolean still_listed = FALSE;
        GHashTableIter it;
        gpointer v;
        g_hash_table_iter_init(&it, s->candidates);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            Candidate *cand = v;
            if (cand->listed && cand->address && g_ascii_strcasecmp(cand->address, addr) == 0) {
                still_listed = TRUE;
                break;
            }
        }
        if (!still_listed) remove_idle_device_by(s, key, addr);
    }
    g_free(addr);
    gboolean any = FALSE;
    for (guint i = 0; i < s->st.devices->len; i++) {
        ShareDevice *x = s->st.devices->pdata[i];
        if (x->state != DEVICE_FOUND) any = TRUE;
    }
    if (!any) {
        s->st.sending = FALSE;
        g_clear_pointer(&s->st.paths, g_strfreev);
    }
    changed(s);
    update_link(s);
}

void share_save_contacts(Share *s, GPtrArray *contacts)
{
    GKeyFile *kf = g_key_file_new();
    g_ptr_array_set_size(s->st.contacts, 0);
    for (guint i = 0; contacts && i < contacts->len; i++) {
        ShareContact *src = contacts->pdata[i];
        ShareContact *c = g_new0(ShareContact, 1);
        c->name = g_strdup(src->name);
        c->device = g_strdup(src->device);
        c->email = g_strdup(src->email);
        c->phone = g_strdup(src->phone);
        c->photo = g_strdup(src->photo);
        g_ptr_array_add(s->st.contacts, c);
        char *grp = g_strdup_printf("contact-%u", i + 1);
        if (c->name && *c->name) g_key_file_set_string(kf, grp, "name", c->name);
        if (c->device && *c->device) g_key_file_set_string(kf, grp, "device", c->device);
        if (c->email && *c->email) g_key_file_set_string(kf, grp, "email", c->email);
        if (c->phone && *c->phone) g_key_file_set_string(kf, grp, "phone", c->phone);
        if (c->photo && *c->photo) g_key_file_set_string(kf, grp, "photo", c->photo);
        g_free(grp);
    }
    char *path = contacts_path(s->dry);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_unref(kf);
    sync_receiver_policy(s);
    changed(s);
}

void share_set_device_photo(Share *s, const char *device_name, const char *photo_path)
{
    if (!device_name || !*device_name) return;
    char *norm = normalize_contact_name(device_name);
    char *owner = owner_prefix_norm(norm);
    int match = -1;
    for (guint i = 0; i < s->st.contacts->len; i++) {
        if (contact_matches_exact(s->st.contacts->pdata[i], norm)) {
            match = (int)i;
            break;
        }
    }
    if (match < 0 && owner && *owner) {
        for (guint i = 0; i < s->st.contacts->len; i++) {
            if (contact_matches_owner(s->st.contacts->pdata[i], owner)) {
                match = (int)i;
                break;
            }
        }
    }
    g_free(owner);
    g_free(norm);

    GPtrArray *next = g_ptr_array_new_with_free_func((GDestroyNotify)share_contact_free);
    for (guint i = 0; i < s->st.contacts->len; i++) {
        ShareContact *src = s->st.contacts->pdata[i];
        ShareContact *c = g_new0(ShareContact, 1);
        c->name = g_strdup(src->name);
        c->device = g_strdup(src->device);
        c->email = g_strdup(src->email);
        c->phone = g_strdup(src->phone);
        c->photo = g_strdup(src->photo);
        if ((int)i == match) {
            g_free(c->photo);
            c->photo = g_strdup(photo_path);
            if (!c->device || !*c->device) {
                g_free(c->device);
                c->device = g_strdup(device_name);
            }
        }
        g_ptr_array_add(next, c);
    }
    if (match < 0 && photo_path && *photo_path) {
        ShareContact *c = g_new0(ShareContact, 1);
        c->name = g_strdup(device_name);
        c->device = g_strdup(device_name);
        c->photo = g_strdup(photo_path);
        g_ptr_array_add(next, c);
    }
    share_save_contacts(s, next);
    g_ptr_array_unref(next);
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
        c->s->st.xfer_start_us = g_get_monotonic_time();
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
            if (!ok && d->address && *d->address) {
                GHashTableIter it;
                gpointer v;
                g_hash_table_iter_init(&it, s->candidates);
                while (g_hash_table_iter_next(&it, NULL, &v)) {
                    Candidate *cand = v;
                    if (cand->address && g_ascii_strcasecmp(cand->address, d->address) == 0) cand->next_probe = 0;
                }
            }
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
    s->st.xfer_start_us = g_get_monotonic_time();
    g_clear_pointer(&d->message, g_free);
    s->send_key = g_strdup(key);
    changed(s);
    if (s->dry) {
        g_timeout_add(1200, pretend_accept, s);
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
