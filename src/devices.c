#include "devices.h"
#include "hidpp.h"
#include "hyperx.h"
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UPOWER       "org.freedesktop.UPower"
#define UPOWER_PATH  "/org/freedesktop/UPower"
#define UPOWER_DEV   "org.freedesktop.UPower.Device"
#define SETTLE_MS    500    /* lets the burst of node changes from one plug settle */
#define WORD_BITS    (8 * sizeof(unsigned long))
#define BATTERY_EVERY 300   /* seconds between asks while a battery is on screen */
#define BATTERY_FRESH 60    /* seconds an answer stays fresh enough to show again */

typedef struct { DevNotify fn; gpointer user; } Watcher;

struct Devices {
    GDBusConnection *system;
    GDBusObjectManager *bluez;
    GDBusProxy *upower;
    GHashTable *batteries;     /* UPower path -> GDBusProxy */
    GPtrArray *usb;            /* Device*, the keyboards and mice found in sysfs */
    GFileMonitor *dev_monitor;
    guint scan_timer;
    GHashTable *hid;           /* USB device path -> HidState, for what only they report */
    GHashTable *pp;            /* HID device path -> PpState, keyboards that HID++ reports */
    int battery_wants, settings_wants, keys_wants;
    guint battery_timer;
    gboolean dry_allowed;      /* a dry run's pretend permission for those batteries */
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
    g_free(x->model);
    g_free(x->mouse);
    g_free(x->keyboard);
    g_free(x);
}

/* A device whose battery and settings only its own protocol reports
 * (src/hyperx.c). */
typedef struct {
    Devices *d;
    char *owner;         /* its USB device in sysfs, also its key in Devices.hid */
    char *node;          /* /dev/hidrawN */
    const HyperxModel *model;
    int level;           /* -1 until it answers */
    gboolean charging, asking;
    gint64 answered;     /* monotonic time of the last answer, 0 for never */
    HyperxState state;   /* its settings, once `known` */
    gboolean known, asleep, reading, writing;
    gboolean stale;      /* asked for while being read or written, so read it after */
    int watch_fd;        /* open while its settings are on screen, -1 otherwise */
    guint watch;
    GCancellable *show;  /* while its light's colour is streamed: see hyperx_shows */
} HidState;

static void stop_watch(HidState *s)
{
    if (s->watch) g_source_remove(s->watch);
    if (s->watch_fd >= 0) close(s->watch_fd);
    s->watch = 0;
    s->watch_fd = -1;
}

static void stop_show(HidState *s)
{
    if (!s->show) return;
    g_cancellable_cancel(s->show);
    g_clear_object(&s->show);
}

static void hid_state_free(gpointer p)
{
    HidState *s = p;
    stop_watch(s);
    stop_show(s);
    g_free(s->owner);
    g_free(s->node);
    g_free(s);
}

static gboolean hid_open(HidState *s)
{
    return access(s->node, R_OK | W_OK) == 0;
}

/* A keyboard paired to a Logitech receiver, whose settings HID++ reports
 * (src/hidpp.c) through the receiver's node. */
typedef struct {
    Devices *d;
    char *owner;         /* its HID device in sysfs, also its key in Devices.pp */
    char *node;          /* the receiver's HID++ node, /dev/hidrawN */
    guint8 index;        /* its place on the receiver */
    const HidppModel *model;
    HidppKeyboard state; /* its settings, once `known` */
    gboolean known, asleep, reading, writing, stale;
    int watch_fd;        /* open while Keyboard shows, -1 otherwise */
    guint watch;
} PpState;

static void pp_stop_watch(PpState *s)
{
    if (s->watch) g_source_remove(s->watch);
    if (s->watch_fd >= 0) close(s->watch_fd);
    s->watch = 0;
    s->watch_fd = -1;
}

static void pp_state_free(gpointer p)
{
    PpState *s = p;
    pp_stop_watch(s);
    g_free(s->owner);
    g_free(s->node);
    g_free(s);
}

static gboolean pp_open(PpState *s)
{
    return access(s->node, R_OK | W_OK) == 0;
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
            /* Its own name, when the person gave it another. */
            x->model = prop_string(p, "Name");
            if (x->model && (!*x->model || g_str_equal(x->model, x->name))) g_clear_pointer(&x->model, g_free);
            x->link = LINK_BLUETOOTH;
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
        x->model = g_strdup(u->model);
        HidState *s = g_hash_table_lookup(d->hid, u->id);
        if (s) {
            x->battery = s->level;
            x->charging = s->charging;
            x->battery_locked = !hid_open(s) && !(d->dry && d->dry_allowed);
            x->has_settings = TRUE;
            if (s->known) {
                x->mouse = g_new(MouseSettings, 1);
                hyperx_decode(s->model, &s->state, x->mouse);
            }
            x->asleep = s->asleep;
        }
        PpState *k = g_hash_table_lookup(d->pp, u->id);
        if (k) {
            x->settings_locked = !pp_open(k) && !(d->dry && d->dry_allowed);
            x->has_settings = TRUE;
            if (k->known) {
                x->keyboard = g_new(KeyboardSettings, 1);
                hidpp_decode(&k->state, k->model, x->keyboard);
            }
            x->asleep = k->asleep;
        }
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
        GVariant *state = g_dbus_proxy_get_cached_property(p, "State");   /* 1 charging */
        gboolean charging = state && g_variant_is_of_type(state, G_VARIANT_TYPE_UINT32)
                            && g_variant_get_uint32(state) == 1;
        if (state) g_variant_unref(state);

        /* A Bluetooth controller's battery also shows up here, under its
         * address, and a USB keyboard's or mouse's under its sysfs device:
         * give it to that entry instead of listing the device twice. */
        char *serial = prop_string(p, "Serial");
        Device *same = by_address(d, serial);
        if (!same) same = by_power_supply(d, native);
        g_free(serial);
        g_free(native);
        if (same) {
            if (same->battery < 0) {
                same->battery = battery;
                same->charging = charging;
            }
            continue;
        }
        Device *x = g_new0(Device, 1);
        x->id = g_strdup(key);
        x->name = prop_string(p, "Model");
        if (!x->name || !*x->name) { g_free(x->name); x->name = g_strdup("Wireless device"); }
        x->kind = kind;
        x->connected = TRUE;
        x->battery = battery;
        x->charging = charging;
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

char *devices_usb_parent(const char *path)
{
    if (!path) return NULL;
    char **parts = g_strsplit(path, "/", -1);
    char *usb = NULL;
    for (guint i = g_strv_length(parts); i-- > 0 && !usb;) {
        if (!is_usb_device(parts[i])) continue;
        char *rest = parts[i + 1];
        parts[i + 1] = NULL;
        usb = g_strjoinv("/", parts);
        parts[i + 1] = rest;
    }
    g_strfreev(parts);
    return usb;
}

char *devices_input_owner(const char *device)
{
    if (!device) return NULL;
    char **parts = g_strsplit(device, "/", -1);
    guint n = g_strv_length(parts);
    gboolean paired = n >= 2 && is_hid_id(parts[n - 1]) && is_hid_id(parts[n - 2]);
    g_strfreev(parts);
    return paired ? g_strdup(device) : devices_usb_parent(device);
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

/* The model src/hyperx.c knows a USB device as, or NULL. */
static const HyperxModel *usb_model(const char *usb)
{
    char *vendor = sys_text(usb, "idVendor"), *product = sys_text(usb, "idProduct");
    const HyperxModel *m = vendor && product
        ? hyperx_model(g_ascii_strtoull(vendor, NULL, 16), g_ascii_strtoull(product, NULL, 16)) : NULL;
    g_free(vendor);
    g_free(product);
    return m;
}

/* The hidraw node of the interface a model's protocol answers on. */
static char *protocol_node(const char *owner, const HyperxModel *m)
{
    char *node = NULL;
    GDir *dir = g_dir_open("/sys/class/hidraw", 0, NULL);
    const char *entry;
    while (dir && !node && (entry = g_dir_read_name(dir))) {
        /* .../1-1/1-1:1.2/0003:03F0:0F98.0003: the HID device, its interface, its USB device */
        char *link = g_build_filename("/sys/class/hidraw", entry, "device", NULL);
        char *hid = realpath(link, NULL);
        char *iface = hid ? g_path_get_dirname(hid) : NULL;
        char *usb = iface ? g_path_get_dirname(iface) : NULL;
        char *number = iface ? sys_text(iface, "bInterfaceNumber") : NULL;
        if (usb && g_str_equal(usb, owner) && number && g_ascii_strtoll(number, NULL, 16) == m->interface)
            node = g_build_filename("/dev", entry, NULL);
        g_free(number);
        g_free(usb);
        g_free(iface);
        free(hid);
        g_free(link);
    }
    if (dir) g_dir_close(dir);
    return node;
}

/* Keeps what each device last said across scans, for the devices still
 * there. */
static void scan_protocols(Devices *d)
{
    GHashTable *hid = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, hid_state_free);
    for (guint i = 0; i < d->usb->len; i++) {
        Device *x = d->usb->pdata[i];
        const HyperxModel *m = usb_model(x->id);
        char *node = m ? protocol_node(x->id, m) : NULL;
        if (!node) continue;
        HidState *s = g_hash_table_lookup(d->hid, x->id);
        if (s) {
            g_hash_table_steal(d->hid, x->id);
            if (!g_str_equal(s->node, node)) stop_watch(s);   /* plugged in again */
            g_free(s->node);
        } else {
            s = g_new0(HidState, 1);
            s->d = d;
            s->owner = g_strdup(x->id);
            s->level = -1;
            s->watch_fd = -1;
        }
        s->node = node;
        s->model = m;
        g_hash_table_insert(hid, s->owner, s);
    }
    g_hash_table_destroy(d->hid);
    d->hid = hid;
}

/* For a keyboard paired to a receiver src/hidpp.c knows, such as
 * .../1-8.4:1.2/0003:046D:C547.0007/0003:046D:40A2.0009: the receiver's
 * HID++ node, and the keyboard's place on it. */
static char *pp_node(const char *owner, guint8 *index, const HidppModel **model)
{
    char *name = g_path_get_basename(owner), *receiver = g_path_get_dirname(owner);
    char *rname = g_path_get_basename(receiver), *node = NULL;
    const HidppModel *m = NULL;
    if (is_hid_id(name) && is_hid_id(rname) && g_str_has_prefix(name, "0003:046D:")
        && g_str_has_prefix(rname, "0003:046D:"))
        m = hidpp_model(g_ascii_strtoull(rname + 10, NULL, 16), g_ascii_strtoull(name + 10, NULL, 16));
    char *iface = m ? g_path_get_dirname(receiver) : NULL;
    char *number = iface ? sys_text(iface, "bInterfaceNumber") : NULL;
    char *uevent = number && g_ascii_strtoll(number, NULL, 16) == m->interface ? sys_text(owner, "uevent") : NULL;
    char *phys = uevent ? strstr(uevent, "HID_PHYS=") : NULL;
    int at = -1;
    if (phys) {
        phys += strlen("HID_PHYS=");
        char *nl = strchr(phys, '\n');
        if (nl) *nl = '\0';
        at = hidpp_phys_index(phys);
    }
    char *dir = at > 0 ? g_build_filename(receiver, "hidraw", NULL) : NULL;
    GDir *raw = dir ? g_dir_open(dir, 0, NULL) : NULL;
    const char *entry = raw ? g_dir_read_name(raw) : NULL;
    if (entry && g_str_has_prefix(entry, "hidraw")) {
        node = g_build_filename("/dev", entry, NULL);
        *index = (guint8)at;
        *model = m;
    }
    if (raw) g_dir_close(raw);
    g_free(dir);
    g_free(uevent);
    g_free(number);
    g_free(iface);
    g_free(rname);
    g_free(receiver);
    g_free(name);
    return node;
}

static void scan_hidpp(Devices *d)
{
    GHashTable *pp = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, pp_state_free);
    for (guint i = 0; i < d->usb->len; i++) {
        Device *x = d->usb->pdata[i];
        guint8 index = 0;
        const HidppModel *model = NULL;
        char *node = x->kind == DEV_KEYBOARD ? pp_node(x->id, &index, &model) : NULL;
        if (!node) continue;
        PpState *s = g_hash_table_lookup(d->pp, x->id);
        if (s) {
            g_hash_table_steal(d->pp, x->id);
            if (!g_str_equal(s->node, node)) pp_stop_watch(s);   /* plugged in again */
            g_free(s->node);
        } else {
            s = g_new0(PpState, 1);
            s->d = d;
            s->owner = g_strdup(x->id);
            s->watch_fd = -1;
        }
        s->node = node;
        s->index = index;
        s->model = model;
        g_hash_table_insert(pp, s->owner, s);
    }
    g_hash_table_destroy(d->pp);
    d->pp = pp;
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
    GPtrArray *found = g_ptr_array_new(), *models = g_ptr_array_new();   /* models: the HyperxModel of each, or NULL */
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
            gboolean direct = is_usb_device(last);
            char *product = direct ? sys_text(n->owner, "product") : NULL;
            x->name = product && *product ? g_steal_pointer(&product)
                    : g_strdup(n->name && *n->name ? n->name : "USB device");
            char *usb = direct ? g_strdup(n->owner) : devices_usb_parent(n->owner);
            const HyperxModel *m = direct ? usb_model(usb) : NULL;
            x->link = !direct || (m && m->receiver) ? LINK_RECEIVER : LINK_USB;
            g_free(usb);
            g_free(product);
            g_free(last);
            g_hash_table_insert(owners, x->id, x);
            g_ptr_array_add(found, x);
            g_ptr_array_add(models, (gpointer)m);
        }
        if (n->kind == DEV_CONTROLLER) x->kind = DEV_CONTROLLER;
    }
    /* A mouse on its cable leaves its receiver unlinked, so it gets one
     * entry, its own, rather than a second from the receiver saying it's
     * asleep. Its receiver comes back as the cable comes out. */
    g_ptr_array_set_size(d->usb, 0);
    for (guint i = 0; i < found->len; i++) {
        Device *x = found->pdata[i];
        if (x->kind == DEV_CONTROLLER
            || hyperx_on_cable(models->pdata[i], (const HyperxModel *const *)models->pdata, models->len))
            device_free(x);
        else
            g_ptr_array_add(d->usb, x);
    }
    g_ptr_array_free(models, TRUE);
    g_ptr_array_free(found, TRUE);
    g_hash_table_destroy(owners);
    g_ptr_array_free(nodes, TRUE);
    scan_protocols(d);
    scan_hidpp(d);
}

static void ask_batteries(Devices *d);
static void settings_on(Devices *d);
static void keys_on(Devices *d);

static gboolean scan_later(gpointer data)
{
    Devices *d = data;
    d->scan_timer = 0;
    scan_usb(d);
    if (d->battery_wants) ask_batteries(d);
    if (d->settings_wants) settings_on(d);
    if (d->keys_wants) keys_on(d);
    schedule(d);
    return G_SOURCE_REMOVE;
}

/* /dev/input changes as a keyboard or mouse comes or goes, a moment before
 * sysfs has all of it. */
static void on_input_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, Devices *d)
{
    if (!d->scan_timer) d->scan_timer = g_timeout_add(SETTLE_MS, scan_later, d);
}

/* ------------------------------------------------------------- batteries */

typedef struct {
    Devices *d;
    char *owner, *node;
    int level;
    gboolean charging;
} AskCtx;

static void ask_ctx_free(gpointer p)
{
    AskCtx *a = p;
    g_free(a->owner);
    g_free(a->node);
    g_free(a);
}

static void ask_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    AskCtx *a = data;
    g_task_return_boolean(task, hyperx_battery(a->node, &a->level, &a->charging, NULL));
}

static void asked(GObject *src, GAsyncResult *res, gpointer user)
{
    AskCtx *a = g_task_get_task_data(G_TASK(res));
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    HidState *s = g_hash_table_lookup(a->d->hid, a->owner);
    if (!s) return; /* unplugged meanwhile */
    s->asking = FALSE;
    if (!ok) return; /* asleep: keep what it said last */
    s->answered = g_get_monotonic_time();
    if (s->level == a->level && s->charging == a->charging) return;
    s->level = a->level;
    s->charging = a->charging;
    schedule(a->d);
}

/* Each question runs in a worker thread: it waits up to a second for the
 * mouse, and a USB write can stall. */
static void ask_batteries(Devices *d)
{
    gint64 now = g_get_monotonic_time();
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, d->hid);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        HidState *s = value;
        if (s->asking || (s->answered && now - s->answered < BATTERY_FRESH * G_USEC_PER_SEC)) continue;
        if (!hid_open(s)) {
            if (d->dry && d->dry_allowed) { /* pretend, as nothing was really allowed */
                s->level = 64;
                s->answered = now;
                schedule(d);
            }
            continue;
        }
        AskCtx *a = g_new0(AskCtx, 1);
        a->d = d;
        a->owner = g_strdup(key);
        a->node = g_strdup(s->node);
        s->asking = TRUE;
        GTask *task = g_task_new(NULL, NULL, asked, NULL);
        g_task_set_task_data(task, a, ask_ctx_free);
        g_task_run_in_thread(task, ask_thread);
        g_object_unref(task);
    }
}

static gboolean ask_batteries_later(gpointer data)
{
    ask_batteries(data);
    return G_SOURCE_CONTINUE;
}

void devices_want_batteries(Devices *d, gboolean on)
{
    d->battery_wants = MAX(0, d->battery_wants + (on ? 1 : -1));
    if (d->battery_wants && !d->battery_timer) {
        ask_batteries(d);
        d->battery_timer = g_timeout_add_seconds(BATTERY_EVERY, ask_batteries_later, d);
    } else if (!d->battery_wants && d->battery_timer) {
        g_source_remove(d->battery_timer);
        d->battery_timer = 0;
    }
}

typedef struct { Devices *d; DevDone done; gpointer user; } AllowCtx;

static void allow_finish(AllowCtx *c, gboolean ok, const char *message)
{
    if (ok) {
        GHashTableIter it;
        gpointer value;
        g_hash_table_iter_init(&it, c->d->hid);
        while (g_hash_table_iter_next(&it, NULL, &value)) ((HidState *)value)->answered = 0;
        ask_batteries(c->d);
        if (c->d->settings_wants) settings_on(c->d);
        if (c->d->keys_wants) keys_on(c->d);
        schedule(c->d);
    }
    c->done(ok, message, c->user);
    g_free(c);
}

static void allowed(GObject *src, GAsyncResult *res, gpointer data)
{
    GSubprocess *p = G_SUBPROCESS(src);
    char *err = NULL;
    GError *e = NULL;
    gboolean ran = g_subprocess_communicate_utf8_finish(p, res, NULL, &err, &e);
    int status = ran && g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    if (status == 0) {
        allow_finish(data, TRUE, NULL);
    } else if (status == 126 || status == 127) { /* pkexec: dismissed, or not allowed */
        allow_finish(data, FALSE, "Nothing changed: the password wasn't given.");
    } else {
        char *line = err && *err ? g_strdup(g_strstrip(err)) : g_strdup(e ? e->message : "It stopped without saying why.");
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        allow_finish(data, FALSE, line);
        g_free(line);
    }
    g_clear_error(&e);
    g_free(err);
    g_object_unref(p);
}

static gboolean pretend_allowed(gpointer data)
{
    AllowCtx *c = data;
    c->d->dry_allowed = TRUE;
    allow_finish(c, TRUE, NULL);
    return G_SOURCE_REMOVE;
}

char *devices_udev_rules(void)
{
    char *hyperx = hyperx_udev_rules(), *hidpp = hidpp_udev_rules();
    char *rules = g_strconcat(hyperx, hidpp, NULL);
    g_free(hyperx);
    g_free(hidpp);
    return rules;
}

void devices_allow_batteries(Devices *d, DevDone done, gpointer user)
{
    AllowCtx *c = g_new0(AllowCtx, 1);
    c->d = d;
    c->done = done;
    c->user = user;
    if (d->dry) {
        g_timeout_add(1200, pretend_allowed, c);
        return;
    }
    /* The rules travel as an argument, so root reads nothing the user could
     * change. udevadm applies them to what's plugged in now and waits until
     * it has. */
    char *rules = devices_udev_rules();
    const char *script = "umask 022 && mkdir -p /etc/udev/rules.d && printf '%s' \"$1\" > " HYPERX_RULES
                         " && udevadm control --reload"
                         " && udevadm trigger --action=change --subsystem-match=hidraw && udevadm settle";
    const char *argv[] = { "pkexec", "/bin/sh", "-c", script, "lares", rules, NULL };
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
    g_free(rules);
    if (!p) {
        allow_finish(c, FALSE, e->message);
        g_error_free(e);
        return;
    }
    g_subprocess_communicate_utf8_async(p, NULL, NULL, allowed, c);
}

/* -------------------------------------------------------------- settings */

typedef struct {
    Devices *d;
    char *owner, *node;
    const HyperxModel *model;
    HyperxState state;
    MouseSettings base, want;
    DevDone done;
    gpointer user;
} SettingsCtx;

static void settings_ctx_free(gpointer p)
{
    SettingsCtx *c = p;
    g_free(c->owner);
    g_free(c->node);
    g_free(c);
}

static SettingsCtx *settings_ctx(Devices *d, HidState *s)
{
    SettingsCtx *c = g_new0(SettingsCtx, 1);
    c->d = d;
    c->owner = g_strdup(s->owner);
    c->node = g_strdup(s->node);
    c->model = s->model;
    return c;
}

static void ask_settings(Devices *d, HidState *s);

static void read_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    SettingsCtx *c = data;
    GError *e = NULL;
    if (hyperx_read(c->node, c->model, &c->state, &e)) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, e);
}

static void settings_read(GObject *src, GAsyncResult *res, gpointer user)
{
    SettingsCtx *c = g_task_get_task_data(G_TASK(res));
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    HidState *s = g_hash_table_lookup(c->d->hid, c->owner);
    if (!s) return; /* unplugged meanwhile */
    s->reading = FALSE;
    if (ok) {
        s->state = c->state;
        s->known = TRUE;
    }
    s->asleep = !ok; /* asleep, or fell asleep while asked */
    if (s->stale) {
        s->stale = FALSE;
        ask_settings(c->d, s);
    }
    schedule(c->d);
}

static void ask_settings(Devices *d, HidState *s)
{
    /* A wake or a rescan while it's being read or written may have changed
     * it: read it once that's done, rather than not at all. */
    if (s->reading || s->writing) {
        s->stale = TRUE;
        return;
    }
    if (!hid_open(s)) {
        if (d->dry && d->dry_allowed && !s->known) { /* pretend, as nothing was really allowed */
            hyperx_sample(&s->state);
            s->known = TRUE;
            schedule(d);
        }
        return;
    }
    s->reading = TRUE;
    GTask *task = g_task_new(NULL, NULL, settings_read, NULL);
    g_task_set_task_data(task, settings_ctx(d, s), settings_ctx_free);
    g_task_run_in_thread(task, read_thread);
    g_object_unref(task);
}

/* What the mouse says by itself while its settings are on screen: its DPI
 * button, waking or falling asleep, and its battery after waking. A wake,
 * or a DPI event that doesn't match the table, means reading it again. Every
 * open hidraw node gets every report, so answers to the worker threads'
 * questions come here too; those aren't events and are skipped. */
static gboolean on_report(int fd, GIOCondition cond, gpointer data)
{
    HidState *s = data;
    guint8 report[HYPERX_REPORT];
    ssize_t got;
    gboolean changed = FALSE, again = FALSE;
    while ((got = read(fd, report, sizeof report)) > 0) {
        HyperxEvent ev;
        if (!hyperx_event(report, (gsize)got, &ev)) continue;
        switch (ev.kind) {
        case HYPERX_STAGE: {
            guint8 was = s->state.dpi[4];
            if (!s->known) break;
            if (hyperx_take_stage(&s->state, &ev)) changed |= s->state.dpi[4] != was;
            else again = TRUE;
            break;
        }
        case HYPERX_LINK:
            changed |= s->asleep == ev.linked;
            s->asleep = !ev.linked;
            again |= ev.linked;
            break;
        case HYPERX_BATTERY:
            changed |= s->level != ev.level || s->charging != ev.charging;
            s->level = ev.level;
            s->charging = ev.charging;
            s->answered = g_get_monotonic_time();
            break;
        }
    }
    gboolean gone = got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR);
    if (again) ask_settings(s->d, s);
    if (changed) schedule(s->d);
    if (!gone) return G_SOURCE_CONTINUE;
    close(fd); /* unplugged: the next scan finds it again, or not */
    s->watch_fd = -1;
    s->watch = 0;
    return G_SOURCE_REMOVE;
}

static void start_watch(HidState *s)
{
    if (s->watch || !hid_open(s)) return;
    s->watch_fd = open(s->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s->watch_fd >= 0) s->watch = g_unix_fd_add(s->watch_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_report, s);
}

static void settings_on(Devices *d)
{
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, d->hid);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        start_watch(value);
        ask_settings(d, value);
    }
}

static void keys_want(Devices *d, gboolean on);

void devices_want_settings(Devices *d, DevKind kind, gboolean on)
{
    if (kind == DEV_KEYBOARD) keys_want(d, on);
    if (kind != DEV_MOUSE) return;
    int was = d->settings_wants;
    d->settings_wants = MAX(0, was + (on ? 1 : -1));
    if (!was && d->settings_wants) {
        settings_on(d);
    } else if (was && !d->settings_wants) {
        GHashTableIter it;
        gpointer value;
        g_hash_table_iter_init(&it, d->hid);
        while (g_hash_table_iter_next(&it, NULL, &value)) stop_watch(value);
    }
}

static void write_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    SettingsCtx *c = data;
    GError *e = NULL;
    if (hyperx_write(c->node, c->model, &c->base, &c->want, &c->state, &e)) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, e);
}

typedef struct {
    char *node;
    guint8 rgb[3];
} ShowCtx;

static void show_ctx_free(gpointer p)
{
    ShowCtx *c = p;
    g_free(c->node);
    g_free(c);
}

static void show_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    ShowCtx *c = data;
    gint64 end = g_get_monotonic_time() + HYPERX_SHOW_MS * G_TIME_SPAN_MILLISECOND;
    GPollFD stop;
    gboolean pollable = g_cancellable_make_pollfd(cancel, &stop);
    for (;;) {
        if (pollable) g_poll(&stop, 1, HYPERX_SHOW_EVERY_MS);
        else g_usleep(HYPERX_SHOW_EVERY_MS * G_TIME_SPAN_MILLISECOND);
        if (g_cancellable_is_cancelled(cancel) || g_get_monotonic_time() >= end) break;
        hyperx_show(c->node, c->rgb, NULL);
    }
    if (pollable) g_cancellable_release_fd(cancel);
    g_task_return_boolean(task, TRUE);
}

/* Goes on streaming the colour hyperx_write streamed once, until the
 * mouse's own effect shows the stored brightness. */
static void start_show(HidState *s, const guint8 rgb[3])
{
    stop_show(s);
    s->show = g_cancellable_new();
    ShowCtx *c = g_new0(ShowCtx, 1);
    c->node = g_strdup(s->node);
    memcpy(c->rgb, rgb, 3);
    GTask *task = g_task_new(NULL, s->show, NULL, NULL);
    g_task_set_task_data(task, c, show_ctx_free);
    g_task_run_in_thread(task, show_thread);
    g_object_unref(task);
}

static void settings_written(GObject *src, GAsyncResult *res, gpointer user)
{
    SettingsCtx *c = g_task_get_task_data(G_TASK(res));
    GError *e = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), &e);
    HidState *s = g_hash_table_lookup(c->d->hid, c->owner);
    if (s) {
        s->writing = FALSE;
        if (ok) {
            MouseSettings was, now;
            hyperx_decode(NULL, &s->state, &was);
            hyperx_decode(NULL, &c->state, &now);
            if (hyperx_shows(&was, &now)) start_show(s, now.rgb);
            s->state = c->state;
            s->asleep = FALSE;
        } else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED)) {
            s->asleep = TRUE;
        } else {
            s->stale = TRUE; /* to show what it holds after all */
        }
        if (s->stale) {
            s->stale = FALSE;
            ask_settings(c->d, s);
        }
        schedule(c->d);
    }
    c->done(ok, e ? e->message : NULL, c->user);
    g_clear_error(&e);
}

static gboolean pretend_written(gpointer data)
{
    SettingsCtx *c = data;
    HidState *s = g_hash_table_lookup(c->d->hid, c->owner);
    guint8 packets[HYPERX_BLOCKS][HYPERX_REPORT];
    MouseSettings merged;
    if (s) hyperx_merge(&s->state, &c->base, &c->want, &merged);
    int n = s ? hyperx_packets(&s->state, &merged, 0, packets) : -1;
    if (s) {
        s->writing = FALSE;
        for (int i = 0; i < n; i++) hyperx_apply(&s->state, packets[i]);
        if (s->stale) {
            s->stale = FALSE;
            ask_settings(c->d, s);
        }
        schedule(c->d);
    }
    c->done(n >= 0, n >= 0 ? NULL : "The mouse can't hold that setting.", c->user);
    settings_ctx_free(c);
    return G_SOURCE_REMOVE;
}

void devices_set_mouse(Devices *d, const char *id, const MouseSettings *base, const MouseSettings *want,
                       DevDone done, gpointer user)
{
    HidState *s = g_hash_table_lookup(d->hid, id);
    if (!s || !s->known || s->writing) {
        done(FALSE, "Its settings can't be changed right now.", user);
        return;
    }
    SettingsCtx *c = settings_ctx(d, s);
    c->base = *base;
    c->want = *want;
    c->done = done;
    c->user = user;
    s->writing = TRUE;
    stop_show(s);   /* or its colour would cover this change */
    schedule(d);
    if (d->dry) {
        g_timeout_add(600, pretend_written, c);
        return;
    }
    GTask *task = g_task_new(NULL, NULL, settings_written, NULL);
    g_task_set_task_data(task, c, settings_ctx_free);
    g_task_run_in_thread(task, write_thread);
    g_object_unref(task);
}

/* ------------------------------------------------------------- keyboards */

typedef struct {
    Devices *d;
    char *owner, *node;
    guint8 index;
    HidppKeyboard state;   /* what it reads, or holds after a lighting change */
    gboolean set_bright, set_light;
    int value, now;        /* the brightness to set, and what it reads back */
    HidppLighting light;   /* the lighting to set */
    DevDone done;
    gpointer user;
} KeysCtx;

static void keys_ctx_free(gpointer p)
{
    KeysCtx *c = p;
    g_free(c->owner);
    g_free(c->node);
    g_free(c);
}

static KeysCtx *keys_ctx(Devices *d, PpState *s)
{
    KeysCtx *c = g_new0(KeysCtx, 1);
    c->d = d;
    c->owner = g_strdup(s->owner);
    c->node = g_strdup(s->node);
    c->index = s->index;
    return c;
}

static void ask_keys(Devices *d, PpState *s);

static void keys_read_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    KeysCtx *c = data;
    GError *e = NULL;
    if (hidpp_read(c->node, c->index, &c->state, &e)) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, e);
}

static void keys_read(GObject *src, GAsyncResult *res, gpointer user)
{
    KeysCtx *c = g_task_get_task_data(G_TASK(res));
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    PpState *s = g_hash_table_lookup(c->d->pp, c->owner);
    if (!s) return; /* unplugged meanwhile */
    s->reading = FALSE;
    if (ok) {
        s->state = c->state;
        s->known = TRUE;
    }
    s->asleep = !ok; /* asleep, or fell asleep while asked */
    if (s->stale) {
        s->stale = FALSE;
        ask_keys(c->d, s);
    }
    schedule(c->d);
}

static void ask_keys(Devices *d, PpState *s)
{
    if (s->reading || s->writing) {
        s->stale = TRUE;
        return;
    }
    if (!pp_open(s)) {
        if (d->dry && d->dry_allowed && !s->known) { /* pretend, as nothing was really allowed */
            hidpp_sample(&s->state);
            s->known = TRUE;
            schedule(d);
        }
        return;
    }
    s->reading = TRUE;
    GTask *task = g_task_new(NULL, NULL, keys_read, NULL);
    g_task_set_task_data(task, keys_ctx(d, s), keys_ctx_free);
    g_task_run_in_thread(task, keys_read_thread);
    g_object_unref(task);
}

/* What the keyboard or its receiver says by itself while Keyboard shows:
 * the receiver lost or found it, it reconnected, its brightness changed, or
 * another setting did. A new link or another setting means reading it
 * again. The receiver's node also carries the answers to everyone's
 * requests; those aren't events. */
static gboolean on_keys_report(int fd, GIOCondition cond, gpointer data)
{
    PpState *s = data;
    guint8 report[64];
    ssize_t got;
    gboolean changed = FALSE, again = FALSE;
    while ((got = read(fd, report, sizeof report)) > 0) {
        HidppEvent ev;
        if (!hidpp_event(report, (gsize)got, s->index, &s->state, &ev)) continue;
        switch (ev.kind) {
        case HIDPP_EVENT_LINK:
            changed |= s->asleep == ev.linked;
            s->asleep = !ev.linked;
            again |= ev.linked;
            break;
        case HIDPP_EVENT_BRIGHTNESS:
            if (!s->known) break;
            changed |= s->state.brightness != ev.brightness || s->asleep;
            s->state.brightness = CLAMP(ev.brightness, 0, s->state.brightness_max);
            s->asleep = FALSE;
            break;
        case HIDPP_EVENT_CHANGED:
            again = TRUE;
            break;
        }
    }
    gboolean gone = got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR);
    if (again) ask_keys(s->d, s);
    if (changed) schedule(s->d);
    if (!gone) return G_SOURCE_CONTINUE;
    close(fd); /* unplugged: the next scan finds it again, or not */
    s->watch_fd = -1;
    s->watch = 0;
    return G_SOURCE_REMOVE;
}

static void keys_start_watch(PpState *s)
{
    if (s->watch || !pp_open(s)) return;
    s->watch_fd = open(s->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s->watch_fd >= 0) s->watch = g_unix_fd_add(s->watch_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_keys_report, s);
}

static void keys_on(Devices *d)
{
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, d->pp);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        keys_start_watch(value);
        ask_keys(d, value);
    }
}

static void keys_want(Devices *d, gboolean on)
{
    int was = d->keys_wants;
    d->keys_wants = MAX(0, was + (on ? 1 : -1));
    if (!was && d->keys_wants) {
        keys_on(d);
    } else if (was && !d->keys_wants) {
        GHashTableIter it;
        gpointer value;
        g_hash_table_iter_init(&it, d->pp);
        while (g_hash_table_iter_next(&it, NULL, &value)) pp_stop_watch(value);
    }
}

static void keys_write_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    KeysCtx *c = data;
    GError *e = NULL;
    gboolean ok = !c->set_bright || hidpp_set_brightness(c->node, c->index, c->value, &c->now, &e);
    if (ok && c->set_light) ok = hidpp_set_lighting(c->node, c->index, &c->light, &c->state, &e);
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, e);
}

static void keys_written(GObject *src, GAsyncResult *res, gpointer user)
{
    KeysCtx *c = g_task_get_task_data(G_TASK(res));
    GError *e = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), &e);
    PpState *s = g_hash_table_lookup(c->d->pp, c->owner);
    if (s) {
        s->writing = FALSE;
        if (ok) {
            if (c->set_light) s->state = c->state;   /* read back whole, the brightness too */
            else s->state.brightness = c->now;
            s->asleep = FALSE;
        } else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED)
                   || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
            s->asleep = TRUE;
        } else {
            s->stale = TRUE; /* to show what it holds after all */
        }
        if (s->stale) {
            s->stale = FALSE;
            ask_keys(c->d, s);
        }
        schedule(c->d);
    }
    c->done(ok, e ? e->message : NULL, c->user);
    g_clear_error(&e);
}

static gboolean pretend_keys_written(gpointer data)
{
    KeysCtx *c = data;
    PpState *s = g_hash_table_lookup(c->d->pp, c->owner);
    if (s) {
        s->writing = FALSE;
        if (c->set_bright) s->state.brightness = c->value;
        if (c->set_light) s->state.light = c->light;
        if (s->stale) {
            s->stale = FALSE;
            ask_keys(c->d, s);
        }
        schedule(c->d);
    }
    c->done(TRUE, NULL, c->user);
    keys_ctx_free(c);
    return G_SOURCE_REMOVE;
}

void devices_set_keyboard(Devices *d, const char *id, const KeyboardSettings *base, const KeyboardSettings *want,
                          DevDone done, gpointer user)
{
    PpState *s = g_hash_table_lookup(d->pp, id);
    if (!s || !s->known || s->writing) {
        done(FALSE, "Its settings can't be changed right now.", user);
        return;
    }
    gboolean bright = want->brightness != base->brightness;
    gboolean light = want->light != base->light
                     || (key_light_coloured(want->light) && memcmp(want->rgb, base->rgb, 3) != 0);
    if ((bright && !s->state.has_brightness) || (light && !s->state.has_profile)) {
        done(FALSE, "Its settings can't be changed right now.", user);
        return;
    }
    if (!bright && !light) {
        done(TRUE, NULL, user);
        return;
    }
    KeysCtx *c = keys_ctx(d, s);
    c->set_bright = bright;
    c->value = hidpp_brightness_value(&s->state, want->brightness);
    c->set_light = light;
    c->light.effect = want->light;
    memcpy(c->light.rgb, want->rgb, 3);
    c->done = done;
    c->user = user;
    s->writing = TRUE;
    schedule(d);
    if (d->dry) {
        g_timeout_add(600, pretend_keys_written, c);
        return;
    }
    GTask *task = g_task_new(NULL, NULL, keys_written, NULL);
    g_task_set_task_data(task, c, keys_ctx_free);
    g_task_run_in_thread(task, keys_write_thread);
    g_object_unref(task);
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
        if (c->done) c->done(TRUE, NULL, c->user);
    } else {
        g_dbus_error_strip_remote_error(e);
        if (c->done) c->done(FALSE, plain_error(e->message), c->user);
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
    if (c->done) c->done(TRUE, NULL, c->user);
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
    d->hid = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, hid_state_free);
    d->pp = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, pp_state_free);
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
