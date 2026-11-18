#include "devices.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UPOWER       "org.freedesktop.UPower"
#define UPOWER_PATH  "/org/freedesktop/UPower"
#define UPOWER_DEV   "org.freedesktop.UPower.Device"
#define SETTLE_MS    500    /* lets the burst of node changes from one plug settle */
#define WORD_BITS    (8 * sizeof(unsigned long))

typedef struct { DevNotify fn; gpointer user; } Watcher;

struct Devices {
    GDBusConnection *system;
    GDBusObjectManager *bluez;
    GDBusProxy *upower;
    GHashTable *batteries;     /* UPower path -> GDBusProxy */
    GPtrArray *usb;            /* Device*, the keyboards and mice found in sysfs */
    GFileMonitor *dev_monitor;
    guint scan_timer;
    GPtrArray *list;
    GArray *watchers;
    guint rebuild_idle;
    gboolean dry;
};

static void device_free(gpointer p)
{
    Device *x = p;
    g_free(x->id);
    g_free(x->name);
    g_free(x->address);
    g_free(x);
}

/* --------------------------------------------------------------- helpers */

static char *prop_string(GDBusProxy *p, const char *name)
{
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    char *s = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, NULL) : NULL;
    if (v) g_variant_unref(v);
    return s;
}

static gboolean prop_bool(GDBusProxy *p, const char *name)
{
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    gboolean b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return b;
}

static int bluez_kind(const char *icon)
{
    if (!icon) return -1;
    if (g_str_equal(icon, "input-gaming")) return DEV_CONTROLLER;
    if (g_str_has_prefix(icon, "audio-")) return DEV_AUDIO;
    if (g_str_equal(icon, "input-keyboard")) return DEV_KEYBOARD;
    if (g_str_equal(icon, "input-mouse")) return DEV_MOUSE;
    return -1;
}

/* UPower's device types: 5 mouse, 6 keyboard, 12 gaming input,
 * 17 headset, 18 speakers, 19 headphones. */
static int upower_kind(guint32 type)
{
    switch (type) {
    case 5: return DEV_MOUSE;
    case 6: return DEV_KEYBOARD;
    case 12: return DEV_CONTROLLER;
    case 17: case 18: case 19: return DEV_AUDIO;
    default: return -1;
    }
}

static int by_kind_then_name(gconstpointer a, gconstpointer b)
{
    const Device *x = *(Device *const *)a, *y = *(Device *const *)b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return g_utf8_collate(x->name, y->name);
}

/* ----------------------------------------------------------------- build */

static void add_bluez(Devices *d)
{
    if (!d->bluez) return;
    GList *objects = g_dbus_object_manager_get_objects(d->bluez);
    for (GList *o = objects; o; o = o->next) {
        GDBusObject *obj = o->data;
        GDBusInterface *dev = g_dbus_object_get_interface(obj, "org.bluez.Device1");
        if (!dev) continue;
        GDBusProxy *p = G_DBUS_PROXY(dev);
        char *icon = prop_string(p, "Icon");
        int kind = bluez_kind(icon);
        if (kind >= 0 && prop_bool(p, "Paired")) {
            Device *x = g_new0(Device, 1);
            x->id = g_strdup(g_dbus_object_get_object_path(obj));
            x->name = prop_string(p, "Alias");
            if (!x->name) x->name = g_strdup("Bluetooth device");
            x->address = prop_string(p, "Address");
            x->kind = kind;
            x->bluetooth = TRUE;
            x->connected = prop_bool(p, "Connected");
            x->battery = -1;
            GDBusInterface *bat = g_dbus_object_get_interface(obj, "org.bluez.Battery1");
            if (bat) {
                GVariant *v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bat), "Percentage");
                if (v) { x->battery = g_variant_get_byte(v); g_variant_unref(v); }
                g_object_unref(bat);
            }
            g_ptr_array_add(d->list, x);
        }
        g_free(icon);
        g_object_unref(dev);
    }
    g_list_free_full(objects, g_object_unref);
}

static Device *by_address(Devices *d, const char *serial)
{
    for (guint i = 0; serial && i < d->list->len; i++) {
        Device *x = d->list->pdata[i];
        if (x->address && g_ascii_strcasecmp(x->address, serial) == 0) return x;
    }
    return NULL;
}

/* The USB keyboard or mouse a UPower battery belongs to: the battery's
 * power supply sits under that device in sysfs. */
static Device *by_power_supply(Devices *d, const char *native)
{
    if (!native || !*native || strchr(native, '/')) return NULL;
    char *link = g_build_filename("/sys/class/power_supply", native, "device", NULL);
    char *path = realpath(link, NULL);
    g_free(link);
    Device *found = NULL;
    for (guint i = 0; path && !found && i < d->list->len; i++) {
        Device *x = d->list->pdata[i];
        size_t len = strlen(x->id);
        if (g_str_has_prefix(x->id, "/sys/") && strncmp(path, x->id, len) == 0
            && (path[len] == '\0' || path[len] == '/'))
            found = x;
    }
    free(path);
    return found;
}

static void add_usb(Devices *d)
{
    for (guint i = 0; i < d->usb->len; i++) {
        Device *u = d->usb->pdata[i], *x = g_new0(Device, 1);
        *x = *u;
        x->id = g_strdup(u->id);
        x->name = g_strdup(u->name);
        g_ptr_array_add(d->list, x);
    }
}

static void add_upower(Devices *d)
{
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, d->batteries);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        GDBusProxy *p = value;
        GVariant *type = g_dbus_proxy_get_cached_property(p, "Type");
        int kind = type ? upower_kind(g_variant_get_uint32(type)) : -1;
        if (type) g_variant_unref(type);
        char *native = prop_string(p, "NativePath");
        gboolean skip = kind < 0 || prop_bool(p, "PowerSupply") || !prop_bool(p, "IsPresent")
                     || (native && g_str_has_prefix(native, "/org/bluez"));
        if (skip) {
            g_free(native);
            continue;
        }

        GVariant *pct = g_dbus_proxy_get_cached_property(p, "Percentage");
        int battery = pct ? (int)lround(g_variant_get_double(pct)) : -1;
        if (pct) g_variant_unref(pct);

        /* A Bluetooth controller's battery also shows up here, under its
         * address, and a USB keyboard's or mouse's under its sysfs device:
         * give it to that entry instead of listing the device twice. */
        char *serial = prop_string(p, "Serial");
        Device *same = by_address(d, serial);
        if (!same) same = by_power_supply(d, native);
        g_free(serial);
        g_free(native);
        if (same) {
            if (same->battery < 0) same->battery = battery;
            continue;
        }
        Device *x = g_new0(Device, 1);
        x->id = g_strdup(key);
        x->name = prop_string(p, "Model");
        if (!x->name || !*x->name) { g_free(x->name); x->name = g_strdup("Wireless device"); }
        x->kind = kind;
        x->connected = TRUE;
        x->battery = battery;
        g_ptr_array_add(d->list, x);
    }
}

static gboolean rebuild(gpointer data)
{
    Devices *d = data;
    d->rebuild_idle = 0;
    g_ptr_array_set_size(d->list, 0);
    add_bluez(d);
    add_usb(d);
    add_upower(d);
    g_ptr_array_sort(d->list, by_kind_then_name);
    for (guint i = 0; i < d->watchers->len; i++) {
        Watcher *w = &g_array_index(d->watchers, Watcher, i);
        w->fn(w->user);
    }
    return G_SOURCE_REMOVE;
}

/* Many signals arrive together (a device connecting changes several
 * properties at once); they become one rebuild. */
static void schedule(Devices *d)
{
    if (!d->rebuild_idle) d->rebuild_idle = g_idle_add(rebuild, d);
}

static void on_bluez_object(GDBusObjectManager *m, GDBusObject *o, Devices *d) { schedule(d); }
static void on_bluez_interface(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, Devices *d) { schedule(d); }
static void on_bluez_props(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *p,
                           GVariant *changed, GStrv invalidated, Devices *d) { schedule(d); }
static void on_battery_props(GDBusProxy *p, GVariant *changed, GStrv invalidated, Devices *d) { schedule(d); }

static void bluez_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    Devices *d = data;
    d->bluez = g_dbus_object_manager_client_new_finish(res, NULL);
    if (!d->bluez) return; /* no Bluetooth on this PC */
    g_signal_connect(d->bluez, "object-added", G_CALLBACK(on_bluez_object), d);
    g_signal_connect(d->bluez, "object-removed", G_CALLBACK(on_bluez_object), d);
    g_signal_connect(d->bluez, "interface-added", G_CALLBACK(on_bluez_interface), d);
    g_signal_connect(d->bluez, "interface-removed", G_CALLBACK(on_bluez_interface), d);
    g_signal_connect(d->bluez, "interface-proxy-properties-changed", G_CALLBACK(on_bluez_props), d);
    schedule(d);
}

/* ---------------------------------------------------------------- UPower */

typedef struct { Devices *d; char *path; } AddCtx;

static void battery_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    AddCtx *c = data;
    GDBusProxy *p = g_dbus_proxy_new_finish(res, NULL);
    if (p) {
        g_signal_connect(p, "g-properties-changed", G_CALLBACK(on_battery_props), c->d);
        g_hash_table_replace(c->d->batteries, g_strdup(c->path), p);
        schedule(c->d);
    }
    g_free(c->path);
    g_free(c);
}

static void add_battery(Devices *d, const char *path)
{
    if (g_str_has_suffix(path, "/DisplayDevice")) return;
    AddCtx *c = g_new0(AddCtx, 1);
    c->d = d;
    c->path = g_strdup(path);
    g_dbus_proxy_new(d->system, G_DBUS_PROXY_FLAGS_NONE, NULL, UPOWER, path, UPOWER_DEV, NULL, battery_ready, c);
}

static void on_upower_signal(GDBusProxy *p, const char *sender, const char *signal, GVariant *params, Devices *d)
{
    const char *path;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(o)"))) return;
    g_variant_get(params, "(&o)", &path);
    if (g_str_equal(signal, "DeviceAdded")) add_battery(d, path);
    else if (g_str_equal(signal, "DeviceRemoved") && g_hash_table_remove(d->batteries, path)) schedule(d);
}

static void upower_enumerated(GObject *src, GAsyncResult *res, gpointer data)
{
    Devices *d = data;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, NULL);
    if (!v) return;
    GVariantIter *it;
    const char *path;
    g_variant_get(v, "(ao)", &it);
    while (g_variant_iter_loop(it, "&o", &path)) add_battery(d, path);
    g_variant_iter_free(it);
    g_variant_unref(v);
}

static void upower_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    Devices *d = data;
    d->upower = g_dbus_proxy_new_finish(res, NULL);
    if (!d->upower) return;
    g_signal_connect(d->upower, "g-signal", G_CALLBACK(on_upower_signal), d);
    g_dbus_proxy_call(d->upower, "EnumerateDevices", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      upower_enumerated, d);
}

/* ------------------------------------------------------------------- USB */

/* Keyboards and mice plugged in by USB, wired or on their own receiver, are
 * read from sysfs: many have no battery reading, so BlueZ and UPower never
 * mention them. Bluetooth ones are left to BlueZ, which also knows the
 * paired ones that aren't connected. */

gboolean devices_bitmap_has(const char *bitmap, guint bit)
{
    if (!bitmap) return FALSE;
    char *copy = g_strstrip(g_strdup(bitmap));
    char **words = g_strsplit(copy, " ", -1);
    guint n = g_strv_length(words), index = bit / WORD_BITS;
    gboolean set = index < n && (g_ascii_strtoull(words[n - 1 - index], NULL, 16) >> (bit % WORD_BITS)) & 1;
    g_strfreev(words);
    g_free(copy);
    return set;
}

int devices_input_kind(const char *keys, const char *rel)
{
    if (devices_bitmap_has(keys, BTN_GAMEPAD) || devices_bitmap_has(keys, BTN_JOYSTICK)) return DEV_CONTROLLER;
    if (devices_bitmap_has(rel, REL_X) && devices_bitmap_has(rel, REL_Y) && devices_bitmap_has(keys, BTN_LEFT))
        return DEV_MOUSE;
    for (guint key = KEY_ESC; key <= KEY_S; key++)
        if (!devices_bitmap_has(keys, key)) return -1;
    return DEV_KEYBOARD;
}

static gboolean is_hid_id(const char *s)      /* such as 0003:046D:C547.0007 */
{
    if (strlen(s) != 19 || s[4] != ':' || s[9] != ':' || s[14] != '.') return FALSE;
    for (int i = 0; i < 19; i++)
        if (i != 4 && i != 9 && i != 14 && !g_ascii_isxdigit(s[i])) return FALSE;
    return TRUE;
}

static gboolean is_usb_device(const char *s)  /* bus and ports, such as 1-2 or 1-2.4 */
{
    const char *dash = strchr(s, '-');
    if (!dash || dash == s || !dash[1]) return FALSE;
    for (const char *c = s; *c; c++)
        if (c != dash && !g_ascii_isdigit(*c) && *c != '.') return FALSE;
    return TRUE;
}

char *devices_input_owner(const char *device)
{
    if (!device) return NULL;
    char **parts = g_strsplit(device, "/", -1);
    guint n = g_strv_length(parts);
    char *owner = NULL;
    if (n >= 2 && is_hid_id(parts[n - 1]) && is_hid_id(parts[n - 2])) {
        owner = g_strdup(device);
    } else {
        for (guint i = n; i-- > 0 && !owner;) {
            if (!is_usb_device(parts[i])) continue;
            char *rest = parts[i + 1];
            parts[i + 1] = NULL;
            owner = g_strjoinv("/", parts);
            parts[i + 1] = rest;
        }
    }
    g_strfreev(parts);
    return owner;
}

static char *sys_text(const char *dir, const char *file)
{
    char *path = g_build_filename(dir, file, NULL), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) g_strstrip(text);
    g_free(path);
    return text;
}

typedef struct { char *device, *owner, *name; int kind; } InputNode;

static void input_node_free(gpointer p)
{
    InputNode *n = p;
    g_free(n->device);
    g_free(n->owner);
    g_free(n->name);
    g_free(n);
}

static int by_device_path(gconstpointer a, gconstpointer b)
{
    return strcmp((*(InputNode *const *)a)->device, (*(InputNode *const *)b)->device);
}

/* The keyboard and mouse nodes on USB, with the physical device each
 * belongs to. */
static GPtrArray *usb_input_nodes(void)
{
    GPtrArray *nodes = g_ptr_array_new_with_free_func(input_node_free);
    GDir *dir = g_dir_open("/sys/class/input", 0, NULL);
    const char *entry;
    while (dir && (entry = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(entry, "input")) continue;
        char *base = g_build_filename("/sys/class/input", entry, NULL);
        char *bus = sys_text(base, "id/bustype");
        if (g_strcmp0(bus, "0003") == 0) {
            char *keys = sys_text(base, "capabilities/key"), *rel = sys_text(base, "capabilities/rel");
            char *link = g_build_filename(base, "device", NULL);
            char *device = realpath(link, NULL);
            char *owner = devices_input_owner(device);
            int kind = devices_input_kind(keys, rel);
            if (owner && kind >= 0) {
                InputNode *n = g_new0(InputNode, 1);
                n->device = g_strdup(device);
                n->owner = g_steal_pointer(&owner);
                n->name = sys_text(base, "name");
                n->kind = kind;
                g_ptr_array_add(nodes, n);
            }
            g_free(owner);
            free(device);
            g_free(link);
            g_free(keys);
            g_free(rel);
        }
        g_free(bus);
        g_free(base);
    }
    if (dir) g_dir_close(dir);
    return nodes;
}

/* One entry per physical device. Its first node, in sysfs order, says what
 * it is: a gaming mouse's extra keyboard node, for its programmable buttons,
 * comes after the mouse's. A device with a gamepad node is a controller,
 * which the Controller section lists. */
static void scan_usb(Devices *d)
{
    GPtrArray *nodes = usb_input_nodes();
    g_ptr_array_sort(nodes, by_device_path);
    GHashTable *owners = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *found = g_ptr_array_new();
    for (guint i = 0; i < nodes->len; i++) {
        InputNode *n = nodes->pdata[i];
        Device *x = g_hash_table_lookup(owners, n->owner);
        if (!x) {
            x = g_new0(Device, 1);
            x->id = g_strdup(n->owner);
            x->kind = n->kind;
            x->connected = TRUE;
            x->battery = -1;
            /* A USB device's own product name reads better than its node's,
             * which starts with the maker's company. A device paired to a
             * receiver has only its node's name. */
            char *last = g_path_get_basename(n->owner);
            char *product = is_usb_device(last) ? sys_text(n->owner, "product") : NULL;
            x->name = product && *product ? g_steal_pointer(&product)
                    : g_strdup(n->name && *n->name ? n->name : "USB device");
            g_free(product);
            g_free(last);
            g_hash_table_insert(owners, x->id, x);
            g_ptr_array_add(found, x);
        }
        if (n->kind == DEV_CONTROLLER) x->kind = DEV_CONTROLLER;
    }
    g_ptr_array_set_size(d->usb, 0);
    for (guint i = 0; i < found->len; i++) {
        Device *x = found->pdata[i];
        if (x->kind == DEV_CONTROLLER) device_free(x);
        else g_ptr_array_add(d->usb, x);
    }
    g_ptr_array_free(found, TRUE);
    g_hash_table_destroy(owners);
    g_ptr_array_free(nodes, TRUE);
}

static gboolean scan_later(gpointer data)
{
    Devices *d = data;
    d->scan_timer = 0;
    scan_usb(d);
    schedule(d);
    return G_SOURCE_REMOVE;
}

/* /dev/input changes as a keyboard or mouse comes or goes, a moment before
 * sysfs has all of it. */
static void on_input_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, Devices *d)
{
    if (!d->scan_timer) d->scan_timer = g_timeout_add(SETTLE_MS, scan_later, d);
}

/* --------------------------------------------------------------- actions */

typedef struct { Devices *d; char *id; gboolean on; DevDone done; gpointer user; } ConnCtx;

static void conn_free(ConnCtx *c)
{
    g_free(c->id);
    g_free(c);
}

/* BlueZ answers in its own words; say what they mean. */
static const char *plain_error(const char *msg)
{
    if (strstr(msg, "page-timeout") || strstr(msg, "Page Timeout") || strstr(msg, "Host is down"))
        return "It didn't answer. Turn it on, bring it closer, and try again.";
    if (strstr(msg, "InProgress") || strstr(msg, "In Progress"))
        return "It's already connecting.";
    if (strstr(msg, "NotReady") || strstr(msg, "Not Ready"))
        return "Bluetooth is off on this PC.";
    return msg;
}

static void connected_done(GObject *src, GAsyncResult *res, gpointer data)
{
    ConnCtx *c = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v) {
        g_variant_unref(v);
        c->done(TRUE, NULL, c->user);
    } else {
        g_dbus_error_strip_remote_error(e);
        c->done(FALSE, plain_error(e->message), c->user);
        g_error_free(e);
    }
    conn_free(c);
}

static gboolean dry_connected(gpointer data)
{
    ConnCtx *c = data;
    for (guint i = 0; i < c->d->list->len; i++) {
        Device *x = c->d->list->pdata[i];
        if (g_str_equal(x->id, c->id)) {
            x->connected = c->on;
            if (c->on && x->battery < 0) x->battery = 70;
        }
    }
    c->done(TRUE, NULL, c->user);
    for (guint i = 0; i < c->d->watchers->len; i++) {
        Watcher *w = &g_array_index(c->d->watchers, Watcher, i);
        w->fn(w->user);
    }
    conn_free(c);
    return G_SOURCE_REMOVE;
}

void devices_set_connected(Devices *d, const char *id, gboolean on, DevDone done, gpointer user)
{
    ConnCtx *c = g_new0(ConnCtx, 1);
    c->d = d; c->id = g_strdup(id); c->on = on; c->done = done; c->user = user;
    if (d->dry) {
        g_timeout_add(1200, dry_connected, c);
        return;
    }
    g_dbus_connection_call(d->system, "org.bluez", id, "org.bluez.Device1", on ? "Connect" : "Disconnect",
                           NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, connected_done, c);
}

/* ------------------------------------------------------------------ public */

GPtrArray *devices_list(Devices *d) { return d->list; }

gboolean devices_any(Devices *d, DevKind kind)
{
    for (guint i = 0; i < d->list->len; i++)
        if (((Device *)d->list->pdata[i])->kind == kind) return TRUE;
    return FALSE;
}

void devices_watch(Devices *d, DevNotify fn, gpointer user)
{
    Watcher w = { fn, user };
    g_array_append_val(d->watchers, w);
}

Devices *devices_new(void)
{
    Devices *d = g_new0(Devices, 1);
    d->list = g_ptr_array_new_with_free_func(device_free);
    d->watchers = g_array_new(FALSE, FALSE, sizeof(Watcher));
    d->batteries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    d->usb = g_ptr_array_new_with_free_func(device_free);
    d->dry = g_getenv("LARES_DRY_RUN") != NULL;
    GFile *dev = g_file_new_for_path("/dev/input");
    d->dev_monitor = g_file_monitor_directory(dev, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(dev);
    if (d->dev_monitor) g_signal_connect(d->dev_monitor, "changed", G_CALLBACK(on_input_changed), d);
    scan_usb(d);
    schedule(d);
    d->system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!d->system) return d; /* no system bus: no devices, nothing breaks */
    g_dbus_object_manager_client_new(d->system, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                     "org.bluez", "/", NULL, NULL, NULL, NULL, bluez_ready, d);
    g_dbus_proxy_new(d->system, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL, UPOWER, UPOWER_PATH,
                     UPOWER, NULL, upower_ready, d);
    return d;
}
