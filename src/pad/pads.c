// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pads.h"
#include <math.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#define UPOWER       "org.freedesktop.UPower"
#define SETTLE_MS    500    /* lets the burst of node changes from one plug settle */

typedef struct { PadsNotify fn; gpointer user; } Watcher;

typedef struct {
    char *native_path, *serial;
    PadBattery b;
} Reading;

struct Pads {
    PadConf *conf;
    GPtrArray *list;          /* Pad* */
    GPtrArray *readings;      /* Reading*, from UPower */
    GArray *watchers;
    GFileMonitor *dev_monitor;
    GDBusConnection *system;
    GCancellable *upower_call;
    guint scan_timer, upower_timer;
};

/* ------------------------------------------------------------- the model */

/* Model number on the controller's label, by product id. */
static const struct { const char *product, *model; } MODELS[] = {
    { "02d1", "1537" }, { "02dd", "1537 / 1697" }, { "02e3", "1698" },
    { "02ea", "1708" }, { "02e0", "1708" }, { "02fd", "1708" }, { "0b20", "1708" },
    { "0b00", "1797" }, { "0b05", "1797" }, { "0b22", "1797" },
    { "0b0a", "1836" }, { "0b0c", "1836" }, { "0b21", "1836" },
    { "0b12", "1914" }, { "0b13", "1914" },
};

/* The newest firmware seen for each Bluetooth LE product id (Padlight #30).
 * Over USB it isn't known yet whether the number follows the firmware. */
static const struct { const char *product, *version; } NEWEST[] = {
    { "0b13", "0523" }, { "0b20", "0523" }, { "0b22", "0523" },
};

/* Button codes as they appear in an input modalias, and the axes behind
 * the triggers and the d-pad. */
static const char *const KEY_CODES[PB_COUNT] = {
    [PB_A] = "130", [PB_B] = "131", [PB_X] = "133", [PB_Y] = "134", [PB_LB] = "136",
    [PB_RB] = "137", [PB_VIEW] = "13a", [PB_MENU] = "13b", [PB_XBOX] = "13c",
    [PB_LS] = "13d", [PB_RS] = "13e", [PB_SHARE] = "a7", [PB_UP] = "220",
    [PB_DOWN] = "221", [PB_LEFT] = "222", [PB_RIGHT] = "223",
};

static gboolean in_list(char **list, const char *code)
{
    for (int i = 0; list && list[i]; i++)
        if (g_ascii_strcasecmp(list[i], code) == 0) return TRUE;
    return FALSE;
}

static guint32 checklist_buttons(char **keys, char **axes)
{
    gboolean triggers = (in_list(axes, "9") && in_list(axes, "a")) || (in_list(axes, "2") && in_list(axes, "5"));
    gboolean hat = in_list(axes, "10") && in_list(axes, "11");
    guint32 bits = 0;
    for (int b = 0; b < PB_COUNT; b++) {
        gboolean has;
        if (b == PB_LT || b == PB_RT) has = triggers;
        else if (b >= PB_UP) has = hat || in_list(keys, KEY_CODES[b]);
        else has = in_list(keys, KEY_CODES[b]);
        if (has) bits |= PB_BIT(b);
    }
    return bits;
}

/* "0509" is binary-coded decimal for 5.9. */
static char *firmware_label(const char *v)
{
    if (strlen(v) != 4 || g_str_equal(v, "0000")) return NULL;
    int major = (v[0] - '0') * 10 + (v[1] - '0'), minor = (v[2] - '0') * 10 + (v[3] - '0');
    return g_strdup_printf("%d.%d", major, minor);
}

static char *newer_firmware(const char *product, const char *v)
{
    for (guint i = 0; i < G_N_ELEMENTS(NEWEST); i++) {
        if (!g_str_equal(NEWEST[i].product, product)) continue;
        const char *n = NEWEST[i].version;
        if (strspn(v, "0123456789") != 4) return NULL;
        int cur = atoi(v), newest = atoi(n);   /* "0509" and "0523" compare as numbers */
        if (cur >= newest) return NULL;
        return firmware_label(n);
    }
    return NULL;
}

/* ----------------------------------------------------------------- sysfs */

static char *read_text(const char *path)
{
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) return NULL;
    return g_strstrip(text);
}

static char *link_target(const char *path)
{
    char *target = g_file_read_link(path, NULL);
    if (!target) return NULL;
    char *dir = g_path_get_dirname(path);
    char *full = g_canonicalize_filename(target, dir);
    g_free(target);
    g_free(dir);
    return full;
}

static GPtrArray *child_names(const char *dir)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d))) g_ptr_array_add(names, g_strdup(e));
    if (d) g_dir_close(d);
    return names;
}

/* The serial number of the USB device a pad hangs from. USB devices are
 * named by bus and port, such as 1-2 or 1-2.4; the root hub above them has
 * the same serial for every device. */
static char *usb_serial(const char *parent)
{
    char *dir = g_strdup(parent);
    for (int i = 0; i < 4 && dir && strlen(dir) > 1; i++) {
        char *base = g_path_get_basename(dir);
        gboolean usb = g_regex_match_simple("^\\d+-[\\d.]+$", base, 0, 0);
        g_free(base);
        if (usb) {
            char *path = g_build_filename(dir, "serial", NULL);
            char *serial = read_text(path);
            g_free(path);
            g_free(dir);
            if (serial && !*serial) { g_free(serial); return NULL; }
            return serial;
        }
        char *up = g_path_get_dirname(dir);
        g_free(dir);
        dir = up;
    }
    g_free(dir);
    return NULL;
}

static void pad_free(gpointer p)
{
    Pad *x = p;
    g_free(x->path); g_free(x->name); g_free(x->nickname); g_free(x->model);
    g_free(x->firmware); g_free(x->firmware_update); g_free(x->driver);
    g_free(x->address); g_free(x->serial); g_free(x->event_node); g_free(x->js_node);
    g_ptr_array_unref(x->hidraw_nodes);
    g_ptr_array_unref(x->power_supplies);
    g_free(x);
}

static int node_number(const Pad *p)
{
    const char *n = p->js_node ? p->js_node : p->event_node;
    const char *d = n + strlen(n);
    while (d > n && g_ascii_isdigit(d[-1])) d--;
    return atoi(d);
}

static int by_node(gconstpointer a, gconstpointer b)
{
    return node_number(*(Pad *const *)a) - node_number(*(Pad *const *)b);
}

/* Looks at one input device and returns a Pad if it is an Xbox controller. */
static Pad *read_pad(const char *class_dir, const char *entry)
{
    static GRegex *modalias;
    if (!modalias)
        modalias = g_regex_new("^input:b([0-9A-F]{4})v([0-9A-F]{4})p([0-9A-F]{4})e([0-9A-F]{4})-"
                               "e[0-9A-F,]*k([0-9A-F,]*)r[0-9A-F,]*a([0-9A-F,]*)m[0-9A-F,]*l[0-9A-F,]*"
                               "s[0-9A-F,]*f([0-9A-F,]*)w", 0, 0, NULL);
    char *dir = g_build_filename(class_dir, entry, NULL);
    char *alias_path = g_build_filename(dir, "modalias", NULL);
    char *alias = read_text(alias_path);
    g_free(alias_path);
    GMatchInfo *m = NULL;
    Pad *pad = NULL;
    if (!alias || !g_regex_match(modalias, alias, 0, &m)) goto out;

    char *f[8];
    for (int i = 1; i <= 7; i++) f[i] = g_ascii_strdown(g_match_info_fetch(m, i), -1);
    const char *bus = f[1], *vendor = f[2], *product = f[3], *version = f[4];
    char **keys = g_strsplit(f[5], ",", -1), **axes = g_strsplit(f[6], ",", -1), **ff = g_strsplit(f[7], ",", -1);

    char *path = link_target(dir);
    if (!path) path = g_strdup(dir);
    gboolean bt = g_str_equal(bus, "0005");
    /* Steam Input and game streaming add made-up pads; a Bluetooth pad is
     * "virtual" to the kernel too, so those stay. */
    if (!in_list(keys, "130") || (strstr(path, "/devices/virtual/") && !bt)) goto skip;

    char *dev_link = g_build_filename(path, "device", NULL);
    char *parent = link_target(dev_link);
    g_free(dev_link);
    char *driver = NULL;
    if (parent) {
        char *drv_link = g_build_filename(parent, "driver", NULL);
        char *drv = link_target(drv_link);
        if (drv) driver = g_path_get_basename(drv);
        g_free(drv);
        g_free(drv_link);
    }
    char *name_path = g_build_filename(path, "name", NULL);
    char *kernel_name = read_text(name_path);
    g_free(name_path);
    if (!kernel_name) kernel_name = g_strdup("");
    gboolean microsoft = g_str_equal(vendor, "045e");
    gboolean xbox = microsoft || g_regex_match_simple("x-?box", kernel_name, G_REGEX_CASELESS, 0)
                 || (driver && (g_str_has_prefix(driver, "xpad") || g_str_has_prefix(driver, "xone")));

    GPtrArray *nodes = child_names(path);
    char *event = NULL, *js = NULL;
    for (guint i = 0; i < nodes->len; i++) {
        const char *n = nodes->pdata[i];
        if (!event && g_str_has_prefix(n, "event")) event = g_strdup(n);
        if (!js && g_str_has_prefix(n, "js")) js = g_strdup(n);
    }
    g_ptr_array_unref(nodes);

    if (xbox && event) {
        pad = g_new0(Pad, 1);
        pad->path = g_strdup(path);
        pad->name = g_strdup(*kernel_name ? (g_str_has_prefix(kernel_name, "Microsoft ") ? kernel_name + 10 : kernel_name)
                                          : "Xbox Controller");
        pad->connection = bt ? PAD_BLUETOOTH
                        : g_regex_match_simple("wireless receiver", kernel_name, G_REGEX_CASELESS, 0) ? PAD_ADAPTER : PAD_USB;
        for (guint i = 0; microsoft && i < G_N_ELEMENTS(MODELS); i++)
            if (g_str_equal(MODELS[i].product, product)) pad->model = g_strdup(MODELS[i].model);
        if (microsoft) {
            pad->firmware = firmware_label(version);
            pad->firmware_update = newer_firmware(product, version);
        }
        pad->driver = g_strdup(driver);
        char *uniq_path = g_build_filename(path, "uniq", NULL);
        pad->address = read_text(uniq_path);
        g_free(uniq_path);
        if (pad->address && !*pad->address) g_clear_pointer(&pad->address, g_free);
        /* The adapter's serial would be the same for each of its pads. */
        if (pad->connection == PAD_USB && parent) pad->serial = usb_serial(parent);
        pad->event_node = g_strconcat("/dev/input/", event, NULL);
        pad->js_node = js ? g_strconcat("/dev/input/", js, NULL) : NULL;
        pad->rumble = in_list(ff, "50");
        pad->checklist = checklist_buttons(keys, axes);
        GStatBuf st;
        pad->since = g_stat(pad->event_node, &st) == 0 ? st.st_mtime : 0;
        pad->power_supplies = g_ptr_array_new_with_free_func(g_free);
        pad->hidraw_nodes = g_ptr_array_new_with_free_func(g_free);
        if (parent) {
            char *ps = g_build_filename(parent, "power_supply", NULL);
            GPtrArray *names = child_names(ps);
            for (guint i = 0; i < names->len; i++) g_ptr_array_add(pad->power_supplies, g_strdup(names->pdata[i]));
            g_ptr_array_unref(names);
            g_free(ps);
            char *hr = g_build_filename(parent, "hidraw", NULL);
            names = child_names(hr);
            for (guint i = 0; i < names->len; i++)
                g_ptr_array_add(pad->hidraw_nodes, g_strconcat("/dev/", (char *)names->pdata[i], NULL));
            g_ptr_array_unref(names);
            g_free(hr);
        }
    }
    g_free(event); g_free(js); g_free(kernel_name); g_free(driver); g_free(parent);
skip:
    g_free(path);
    g_strfreev(keys); g_strfreev(axes); g_strfreev(ff);
    for (int i = 1; i <= 7; i++) g_free(f[i]);
out:
    if (m) g_match_info_free(m);
    g_free(alias);
    g_free(dir);
    return pad;
}

/* ------------------------------------------------------------- batteries */

static void reading_free(gpointer p)
{
    Reading *r = p;
    g_free(r->native_path);
    g_free(r->serial);
    g_free(r);
}

static void match_batteries(Pads *p)
{
    for (guint i = 0; i < p->list->len; i++) {
        Pad *pad = p->list->pdata[i];
        memset(&pad->battery, 0, sizeof pad->battery);
        Reading *found = NULL;
        for (guint j = 0; !found && j < p->readings->len; j++) {
            Reading *r = p->readings->pdata[j];
            for (guint k = 0; k < pad->power_supplies->len; k++)
                if (r->native_path && g_str_equal(r->native_path, pad->power_supplies->pdata[k])) found = r;
        }
        for (guint j = 0; !found && pad->address && j < p->readings->len; j++) {
            Reading *r = p->readings->pdata[j];
            if (r->serial && g_ascii_strcasecmp(r->serial, pad->address) == 0) found = r;
        }
        if (found) pad->battery = found->b;
    }
}

static void notify(Pads *p)
{
    for (guint i = 0; i < p->watchers->len; i++) {
        Watcher *w = &g_array_index(p->watchers, Watcher, i);
        w->fn(w->user);
    }
}

typedef struct { Pads *p; GPtrArray *readings; int left; } BatCtx;

static void bat_done(BatCtx *c)
{
    if (--c->left > 0) return;
    Pads *p = c->p;
    if (!g_cancellable_is_cancelled(p->upower_call)) {
        g_ptr_array_unref(p->readings);
        p->readings = c->readings;
        match_batteries(p);
        notify(p);
    } else {
        g_ptr_array_unref(c->readings);
    }
    g_free(c);
}

static void got_props(GObject *src, GAsyncResult *res, gpointer data)
{
    BatCtx *c = data;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (v) {
        GVariant *props = g_variant_get_child_value(v, 0);
        gboolean present = FALSE;
        g_variant_lookup(props, "IsPresent", "b", &present);
        if (present) {
            Reading *r = g_new0(Reading, 1);
            g_variant_lookup(props, "NativePath", "s", &r->native_path);
            g_variant_lookup(props, "Serial", "s", &r->serial);
            g_variant_lookup(props, "Percentage", "d", &r->b.percentage);
            g_variant_lookup(props, "State", "u", &r->b.state);
            g_variant_lookup(props, "BatteryLevel", "u", &r->b.level);
            r->b.known = TRUE;
            g_ptr_array_add(c->readings, r);
        }
        g_variant_unref(props);
        g_variant_unref(v);
    }
    bat_done(c);
}

static void got_devices(GObject *src, GAsyncResult *res, gpointer data)
{
    BatCtx *c = data;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (v) {
        GVariantIter *it;
        const char *path;
        g_variant_get(v, "(ao)", &it);
        while (g_variant_iter_loop(it, "&o", &path)) {
            c->left++;
            g_dbus_connection_call(c->p->system, UPOWER, path, "org.freedesktop.DBus.Properties", "GetAll",
                g_variant_new("(s)", "org.freedesktop.UPower.Device"), G_VARIANT_TYPE("(a{sv})"),
                G_DBUS_CALL_FLAGS_NONE, 3000, c->p->upower_call, got_props, c);
        }
        g_variant_iter_free(it);
        g_variant_unref(v);
    }
    bat_done(c);
}

/* Battery readings, asked for only while a pad is connected. */
static void refresh_batteries(Pads *p)
{
    if (!p->system || !p->list->len) {
        if (p->readings->len) { g_ptr_array_set_size(p->readings, 0); match_batteries(p); }
        return;
    }
    if (p->upower_call) { g_cancellable_cancel(p->upower_call); g_object_unref(p->upower_call); }
    p->upower_call = g_cancellable_new();
    BatCtx *c = g_new0(BatCtx, 1);
    c->p = p;
    c->left = 1;
    c->readings = g_ptr_array_new_with_free_func(reading_free);
    g_dbus_connection_call(p->system, UPOWER, "/org/freedesktop/UPower", UPOWER, "EnumerateDevices", NULL,
        G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, 3000, p->upower_call, got_devices, c);
}

/* ------------------------------------------------------------- scanning */

static void name_pads(Pads *p)
{
    PadPrefs *prefs = padconf_prefs(p->conf);
    for (guint i = 0; i < p->list->len; i++) {
        Pad *pad = p->list->pdata[i];
        g_clear_pointer(&pad->nickname, g_free);
        char *key = pad_nickname_key(pad, prefs->salt);
        const char *v = key ? g_hash_table_lookup(prefs->nicknames, key) : NULL;
        if (v) {
            const char *tab = strchr(v, '\t');
            pad->nickname = tab ? g_strndup(v, tab - v) : g_strdup(v);
        }
        g_free(key);
    }
}

static void scan(Pads *p)
{
    GPtrArray *names = child_names("/sys/class/input");
    GPtrArray *found = g_ptr_array_new_with_free_func(pad_free);
    for (guint i = 0; i < names->len; i++) {
        const char *n = names->pdata[i];
        if (!g_str_has_prefix(n, "input")) continue;
        Pad *pad = read_pad("/sys/class/input", n);
        if (pad) g_ptr_array_add(found, pad);
    }
    g_ptr_array_unref(names);
    g_ptr_array_sort(found, by_node);
    g_ptr_array_unref(p->list);
    p->list = found;
    name_pads(p);
    match_batteries(p);   /* the last readings, until fresh ones arrive */
    notify(p);
    refresh_batteries(p);
}

static gboolean scan_later_cb(gpointer data)
{
    Pads *p = data;
    p->scan_timer = 0;
    scan(p);
    return G_SOURCE_REMOVE;
}

static void on_dev_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, Pads *p)
{
    if (!p->scan_timer) p->scan_timer = g_timeout_add(SETTLE_MS, scan_later_cb, p);
}

static gboolean batteries_later_cb(gpointer data)
{
    Pads *p = data;
    p->upower_timer = 0;
    refresh_batteries(p);
    return G_SOURCE_REMOVE;
}

/* Any news from UPower may be a new reading; many come together. */
static void on_upower(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *signal, GVariant *params, gpointer data)
{
    Pads *p = data;
    if (p->list->len && !p->upower_timer) p->upower_timer = g_timeout_add(300, batteries_later_cb, p);
}

static void on_conf(gpointer data)
{
    Pads *p = data;
    name_pads(p);
    notify(p);
}

/* -------------------------------------------------------------- names */

static char *identity(const Pad *pad)
{
    if (pad->address) {
        char *low = g_ascii_strdown(pad->address, -1);
        char *id = g_strconcat("address ", low, NULL);
        g_free(low);
        return id;
    }
    if (pad->serial) return g_strconcat("serial ", pad->serial, NULL);
    return NULL;
}

gboolean pad_can_nickname(const Pad *pad) { return pad->address || pad->serial; }

/* An HMAC with a random salt of this computer's, so the settings never hold
 * the address or the serial number. */
char *pad_nickname_key(const Pad *pad, const char *salt)
{
    char *id = identity(pad);
    if (!id || !salt || !*salt) { g_free(id); return NULL; }
    char *key = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)salt, strlen(salt), id, -1);
    g_free(id);
    return key;
}

char *pad_clean_nickname(const char *text)
{
    char *t = g_strstrip(g_strdup(text ? text : ""));
    GString *s = g_string_new(NULL);
    gboolean space = FALSE;
    for (const char *c = t; *c; c = g_utf8_next_char(c)) {
        gunichar u = g_utf8_get_char(c);
        if (g_unichar_isspace(u)) { space = TRUE; continue; }
        if (space && s->len) g_string_append_c(s, ' ');
        space = FALSE;
        g_string_append_unichar(s, u);
    }
    g_free(t);
    char *out = g_string_free(s, FALSE);
    if (g_utf8_strlen(out, -1) > PAD_NICKNAME_LENGTH) {
        char *cut = g_utf8_substring(out, 0, PAD_NICKNAME_LENGTH);
        g_free(out);
        out = g_strstrip(cut);
    }
    return out;
}

void pads_set_nickname(Pads *p, const Pad *pad, const char *nickname)
{
    PadPrefs *prefs = padconf_prefs(p->conf);
    if (!prefs->salt) prefs->salt = g_uuid_string_random();
    char *key = pad_nickname_key(pad, prefs->salt);
    if (!key) return;
    char *clean = pad_clean_nickname(nickname);
    if (*clean) g_hash_table_insert(prefs->nicknames, key, g_strdup_printf("%s\t%s", clean, pad->name));
    else { g_hash_table_remove(prefs->nicknames, key); g_free(key); }
    g_free(clean);
    padconf_save(p->conf);
    name_pads(p);
    notify(p);
}

const char *pad_name(const Pad *pad) { return pad->nickname ? pad->nickname : pad->name; }

const char *pad_connection_name(PadConnection c)
{
    return c == PAD_BLUETOOTH ? "Bluetooth" : c == PAD_ADAPTER ? "Xbox adapter" : "Wired (USB)";
}

/* -------------------------------------------------------------- battery */

const char *pad_battery_level_name(const PadBattery *b)
{
    switch (b->level) {
    case 3: return "Low";
    case 4: return "Critical";
    case 6: return "Normal";
    case 7: return "High";
    case 8: return "Full";
    default: return NULL;
    }
}

gboolean pad_battery_charging(const PadBattery *b) { return b->state == 1 || b->state == 4; }

int pad_battery_low_stage(const PadBattery *b, int low)
{
    if (pad_battery_level_name(b)) return b->level == 3 ? 1 : b->level == 4 ? 2 : 0;
    if (b->percentage <= MIN(PAD_VERY_LOW, low)) return 2;
    return b->percentage <= low ? 1 : 0;
}

char *pad_battery_charge(const PadBattery *b)
{
    const char *name = pad_battery_level_name(b);
    return name ? g_strdup(name) : g_strdup_printf("%d%%", (int)lround(b->percentage));
}

char *pad_battery_text(const PadBattery *b)
{
    char *charge = pad_battery_charge(b);
    char *text = b->state == 1 ? g_strdup_printf("%s · Charging", charge)
               : b->state == 4 ? g_strdup_printf("%s · Charged", charge) : g_strdup(charge);
    g_free(charge);
    return text;
}

double pad_battery_fraction(const PadBattery *b)
{
    switch (b->level) {
    case 3: return 0.10;
    case 4: return 0.05;
    case 6: return 0.55;
    case 7: return 0.80;
    case 8: return 1.0;
    default: return CLAMP(b->percentage / 100.0, 0, 1);
    }
}

/* ---------------------------------------------------------------- public */

GPtrArray *pads_list(Pads *p) { return p->list; }

const Pad *pads_find(Pads *p, const char *path)
{
    for (guint i = 0; path && i < p->list->len; i++) {
        Pad *pad = p->list->pdata[i];
        if (g_str_equal(pad->path, path)) return pad;
    }
    return NULL;
}

void pads_watch(Pads *p, PadsNotify fn, gpointer user)
{
    Watcher w = { fn, user };
    g_array_append_val(p->watchers, w);
}

/* /dev/input changes for every controller, wired or not, about half a
 * second before it's ready to read. */
Pads *pads_new(PadConf *conf)
{
    Pads *p = g_new0(Pads, 1);
    p->conf = conf;
    p->list = g_ptr_array_new_with_free_func(pad_free);
    p->readings = g_ptr_array_new_with_free_func(reading_free);
    p->watchers = g_array_new(FALSE, FALSE, sizeof(Watcher));
    GFile *dev = g_file_new_for_path("/dev/input");
    p->dev_monitor = g_file_monitor_directory(dev, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(dev);
    if (p->dev_monitor) g_signal_connect(p->dev_monitor, "changed", G_CALLBACK(on_dev_changed), p);
    p->system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (p->system)
        g_dbus_connection_signal_subscribe(p->system, UPOWER, NULL, NULL, NULL, NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE, on_upower, p, NULL);
    padconf_watch(conf, on_conf, p);
    scan(p);
    return p;
}
