// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "padpair.h"
#include <string.h>

#define BLUEZ          "org.bluez"
#define ADAPTER        "org.bluez.Adapter1"
#define DEVICE         "org.bluez.Device1"
#define SEARCH_SECONDS 30
#define PAIR_MS        30000
#define CONNECT_MS     15000

struct PadPair {
    PairState state;
    GPtrArray *found;        /* PairFound* */
    char *target, *reason, *adapter, *pairing_path;
    GHashTable *devices;     /* path -> GVariant a{sv} of Device1 */
    GDBusConnection *bus;
    GCancellable *cancel;
    guint timeout, subs[3];
    gboolean discovering;
    PadPairNotify changed;
    gpointer user;
};

static const struct { const char *error, *reason; } REASONS[] = {
    { "org.bluez.Error.AuthenticationFailed", "the controller turned it down" },
    { "org.bluez.Error.AuthenticationRejected", "the controller turned it down" },
    { "org.bluez.Error.AuthenticationCanceled", "it was canceled" },
    { "org.bluez.Error.AuthenticationTimeout", "the controller stopped answering" },
    { "org.bluez.Error.ConnectionAttemptFailed", "the controller could not be reached" },
    { "org.bluez.Error.InProgress", "another pairing is under way" },
};

static void found_free(gpointer p)
{
    PairFound *f = p;
    g_free(f->path);
    g_free(f->name);
    g_free(f);
}

static void set_state(PadPair *p, PairState s)
{
    p->state = s;
    p->changed(p->user);
}

static char *reason_of(GError *e)
{
    if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) return g_strdup("the controller took too long to answer");
    char *name = g_dbus_error_get_remote_error(e);
    for (guint i = 0; name && i < G_N_ELEMENTS(REASONS); i++)
        if (g_str_equal(name, REASONS[i].error)) { g_free(name); return g_strdup(REASONS[i].reason); }
    g_free(name);
    g_dbus_error_strip_remote_error(e);
    return g_strdup(e->message);
}

static gboolean is_error(GError *e, const char *name)
{
    char *remote = g_dbus_error_get_remote_error(e);
    gboolean same = remote && g_str_equal(remote, name);
    g_free(remote);
    return same;
}

/* Lets go of BlueZ: no search, no signals, no calls under way. */
static void reset(PadPair *p)
{
    if (p->timeout) { g_source_remove(p->timeout); p->timeout = 0; }
    if (p->discovering && p->adapter)
        g_dbus_connection_call(p->bus, BLUEZ, p->adapter, ADAPTER, "StopDiscovery", NULL, NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    p->discovering = FALSE;
    for (int i = 0; i < 3; i++) if (p->subs[i]) { g_dbus_connection_signal_unsubscribe(p->bus, p->subs[i]); p->subs[i] = 0; }
    if (p->cancel) { g_cancellable_cancel(p->cancel); g_clear_object(&p->cancel); }
    g_hash_table_remove_all(p->devices);
    g_clear_pointer(&p->pairing_path, g_free);
}

static void finish(PadPair *p, PairState s, char *reason)
{
    reset(p);
    g_free(p->reason);
    p->reason = reason;
    set_state(p, s);
}

static int by_name(gconstpointer a, gconstpointer b)
{
    const PairFound *x = *(PairFound *const *)a, *y = *(PairFound *const *)b;
    int c = g_utf8_collate(x->name, y->name);
    return c ? c : strcmp(x->path, y->path);
}

/* Controllers heard during this search (BlueZ gives those a signal
 * strength) that aren't paired yet. Same names get a number. */
static void update_found(PadPair *p)
{
    if (p->state != PAIR_SEARCHING) return;
    GPtrArray *found = g_ptr_array_new_with_free_func(found_free);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, p->devices);
    char *prefix = g_strconcat(p->adapter, "/", NULL);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const char *name = NULL;
        gboolean paired = FALSE;
        g_variant_lookup(v, "Name", "&s", &name);
        g_variant_lookup(v, "Paired", "b", &paired);
        GVariant *rssi = g_variant_lookup_value(v, "RSSI", NULL);
        if (g_str_has_prefix(k, prefix) && rssi && !paired && name
            && g_regex_match_simple("xbox", name, G_REGEX_CASELESS, 0)) {
            PairFound *f = g_new0(PairFound, 1);
            f->path = g_strdup(k);
            f->name = g_strdup(name);
            g_ptr_array_add(found, f);
        }
        if (rssi) g_variant_unref(rssi);
    }
    g_free(prefix);
    g_ptr_array_sort(found, by_name);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);   /* name -> count so far */
    GPtrArray *originals = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < found->len; i++) {
        PairFound *f = found->pdata[i];
        char *name = g_strdup(f->name);
        g_ptr_array_add(originals, name);
        int count = GPOINTER_TO_INT(g_hash_table_lookup(seen, name)) + 1;
        g_hash_table_insert(seen, name, GINT_TO_POINTER(count));
        if (count > 1) { char *n = g_strdup_printf("%s (%d)", f->name, count); g_free(f->name); f->name = n; }
    }
    g_hash_table_destroy(seen);
    g_ptr_array_unref(originals);
    gboolean same = found->len == p->found->len;
    for (guint i = 0; same && i < found->len; i++)
        same = g_str_equal(((PairFound *)found->pdata[i])->path, ((PairFound *)p->found->pdata[i])->path)
            && g_str_equal(((PairFound *)found->pdata[i])->name, ((PairFound *)p->found->pdata[i])->name);
    g_ptr_array_unref(p->found);
    p->found = found;
    if (!same) p->changed(p->user);
}

static void merge_props(PadPair *p, const char *path, GVariant *changed, const char *const *invalid)
{
    GVariantDict d;
    GVariant *old = g_hash_table_lookup(p->devices, path);
    g_variant_dict_init(&d, old);
    GVariantIter it;
    const char *key;
    GVariant *val;
    g_variant_iter_init(&it, changed);
    while (g_variant_iter_next(&it, "{&sv}", &key, &val)) { g_variant_dict_insert_value(&d, key, val); g_variant_unref(val); }
    for (int i = 0; invalid && invalid[i]; i++) g_variant_dict_remove(&d, invalid[i]);
    g_hash_table_insert(p->devices, g_strdup(path), g_variant_ref_sink(g_variant_dict_end(&d)));
}

static void on_added(GDBusConnection *c, const char *s, const char *o, const char *i, const char *sig,
                     GVariant *params, gpointer data)
{
    PadPair *p = data;
    const char *path;
    GVariant *ifaces;
    g_variant_get(params, "(&o@a{sa{sv}})", &path, &ifaces);
    GVariant *dev = g_variant_lookup_value(ifaces, DEVICE, NULL);
    if (dev) { merge_props(p, path, dev, NULL); g_variant_unref(dev); update_found(p); }
    g_variant_unref(ifaces);
}

static void on_removed(GDBusConnection *c, const char *s, const char *o, const char *i, const char *sig,
                       GVariant *params, gpointer data)
{
    PadPair *p = data;
    const char *path;
    const char **ifaces;
    g_variant_get(params, "(&o^a&s)", &path, &ifaces);
    if (g_strv_contains(ifaces, DEVICE)) { g_hash_table_remove(p->devices, path); update_found(p); }
    g_free(ifaces);
}

static void on_props(GDBusConnection *c, const char *s, const char *path, const char *i, const char *sig,
                     GVariant *params, gpointer data)
{
    PadPair *p = data;
    const char *iface;
    GVariant *changed;
    const char **invalid;
    g_variant_get(params, "(&s@a{sv}^a&s)", &iface, &changed, &invalid);
    if (g_str_equal(iface, DEVICE)) { merge_props(p, path, changed, invalid); update_found(p); }
    g_variant_unref(changed);
    g_free(invalid);
}

static gboolean search_over(gpointer data)
{
    PadPair *p = data;
    p->timeout = 0;
    finish(p, p->found->len ? PAIR_STOPPED : PAIR_NOT_FOUND, NULL);
    return G_SOURCE_REMOVE;
}

static void discovery_started(GObject *src, GAsyncResult *res, gpointer data)
{
    PadPair *p = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); return; }
    if (v) { g_variant_unref(v); p->discovering = TRUE; }
    /* Someone else's search finds the controllers just as well. */
    else if (!is_error(e, "org.bluez.Error.InProgress")) { finish(p, PAIR_FAILED, reason_of(e)); g_error_free(e); return; }
    g_clear_error(&e);
    p->timeout = g_timeout_add_seconds(SEARCH_SECONDS, search_over, p);
    p->state = PAIR_SEARCHING;
    update_found(p);
    p->changed(p->user);
}

static void got_objects(GObject *src, GAsyncResult *res, gpointer data)
{
    PadPair *p = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!v) {
        gboolean cancelled = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_error_free(e);
        if (!cancelled) set_state(p, PAIR_UNAVAILABLE);
        return;
    }
    GVariantIter *objects;
    const char *path;
    GVariant *ifaces;
    gboolean any_adapter = FALSE;
    g_variant_get(v, "(a{oa{sa{sv}}})", &objects);
    while (g_variant_iter_loop(objects, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *ad = g_variant_lookup_value(ifaces, ADAPTER, NULL);
        if (ad) {
            gboolean powered = FALSE;
            g_variant_lookup(ad, "Powered", "b", &powered);
            any_adapter = TRUE;
            if (powered && !p->adapter) p->adapter = g_strdup(path);
            g_variant_unref(ad);
        }
        GVariant *dev = g_variant_lookup_value(ifaces, DEVICE, NULL);
        if (dev) g_hash_table_insert(p->devices, g_strdup(path), dev);
    }
    g_variant_iter_free(objects);
    g_variant_unref(v);
    if (!p->adapter) { set_state(p, any_adapter ? PAIR_OFF : PAIR_UNAVAILABLE); return; }

    p->subs[0] = g_dbus_connection_signal_subscribe(p->bus, BLUEZ, "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded", "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_added, p, NULL);
    p->subs[1] = g_dbus_connection_signal_subscribe(p->bus, BLUEZ, "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved", "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_removed, p, NULL);
    p->subs[2] = g_dbus_connection_signal_subscribe(p->bus, BLUEZ, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, DEVICE, G_DBUS_SIGNAL_FLAGS_NONE, on_props, p, NULL);
    g_dbus_connection_call(p->bus, BLUEZ, p->adapter, ADAPTER, "StartDiscovery", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, p->cancel, discovery_started, p);
}

void padpair_search(PadPair *p)
{
    reset(p);
    g_clear_pointer(&p->adapter, g_free);
    g_clear_pointer(&p->target, g_free);
    g_clear_pointer(&p->reason, g_free);
    g_ptr_array_set_size(p->found, 0);
    if (!p->bus) { set_state(p, PAIR_UNAVAILABLE); return; }
    p->cancel = g_cancellable_new();
    set_state(p, PAIR_STARTING);
    g_dbus_connection_call(p->bus, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                           G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, p->cancel,
                           got_objects, p);
}

/* ---------------------------------------------------------------- pairing */

static void connected(GObject *src, GAsyncResult *res, gpointer data)
{
    PadPair *p = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v) g_variant_unref(v);
    if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); return; }
    if (e && !is_error(e, "org.bluez.Error.AlreadyConnected")) { finish(p, PAIR_FAILED, reason_of(e)); g_error_free(e); return; }
    g_clear_error(&e);
    finish(p, PAIR_CONNECTED, NULL);
}

/* Trusted lets the controller connect again by itself later. */
static void trusted(GObject *src, GAsyncResult *res, gpointer data)
{
    PadPair *p = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v) g_variant_unref(v);
    if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); return; }
    if (e) { finish(p, PAIR_FAILED, reason_of(e)); g_error_free(e); return; }
    g_dbus_connection_call(p->bus, BLUEZ, p->pairing_path, DEVICE, "Connect", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, CONNECT_MS, p->cancel, connected, p);
}

static void paired(GObject *src, GAsyncResult *res, gpointer data)
{
    PadPair *p = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v) g_variant_unref(v);
    if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); return; }
    if (e && !is_error(e, "org.bluez.Error.AlreadyExists")) {
        /* BlueZ may still be trying after the wait is over. */
        if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            g_dbus_connection_call(p->bus, BLUEZ, p->pairing_path, DEVICE, "CancelPairing", NULL, NULL,
                                   G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        finish(p, PAIR_FAILED, reason_of(e));
        g_error_free(e);
        return;
    }
    g_clear_error(&e);
    g_dbus_connection_call(p->bus, BLUEZ, p->pairing_path, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", DEVICE, "Trusted", g_variant_new_boolean(TRUE)), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, p->cancel, trusted, p);
}

void padpair_pair(PadPair *p, const char *path)
{
    PairFound *f = NULL;
    for (guint i = 0; i < p->found->len; i++)
        if (g_str_equal(((PairFound *)p->found->pdata[i])->path, path)) f = p->found->pdata[i];
    if (p->state != PAIR_SEARCHING || !f) return;
    if (p->timeout) { g_source_remove(p->timeout); p->timeout = 0; }
    g_free(p->target);
    p->target = g_strdup(f->name);
    p->pairing_path = g_strdup(path);
    /* Searching while pairing slows Bluetooth down. */
    if (p->discovering)
        g_dbus_connection_call(p->bus, BLUEZ, p->adapter, ADAPTER, "StopDiscovery", NULL, NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    p->discovering = FALSE;
    set_state(p, PAIR_PAIRING);
    g_dbus_connection_call(p->bus, BLUEZ, path, DEVICE, "Pair", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, PAIR_MS,
                           p->cancel, paired, p);
}

void padpair_cancel(PadPair *p)
{
    if (p->pairing_path)
        g_dbus_connection_call(p->bus, BLUEZ, p->pairing_path, DEVICE, "CancelPairing", NULL, NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    reset(p);
    if (p->state != PAIR_IDLE) set_state(p, PAIR_IDLE);
}

void padpair_hidden(PadPair *p)
{
    if (p->state == PAIR_STARTING || p->state == PAIR_SEARCHING) padpair_cancel(p);
}

/* ------------------------------------------------------------------ text */

const char *padpair_action(PadPair *p)
{
    switch (p->state) {
    case PAIR_STARTING: case PAIR_SEARCHING: return "Stop searching";
    case PAIR_NOT_FOUND: case PAIR_STOPPED: return "Search again";
    case PAIR_PAIRING: return NULL;
    case PAIR_CONNECTED: return "Pair another controller";
    case PAIR_FAILED: return "Try again";
    default: return "Pair a controller";
    }
}

char *padpair_status(PadPair *p)
{
    switch (p->state) {
    case PAIR_STARTING: return g_strdup("Looking for Bluetooth…");
    case PAIR_UNAVAILABLE: return g_strdup("Bluetooth isn't available on this PC.");
    case PAIR_OFF: return g_strdup("Bluetooth is off. Turn it on in the system menu, then try again.");
    case PAIR_SEARCHING:
        return g_strdup("Searching… Hold the pair button on top of the controller until the Xbox button flashes quickly.");
    case PAIR_NOT_FOUND:
        return g_strdup("No controller found. Hold the pair button until the Xbox button flashes quickly.");
    case PAIR_STOPPED:
        return g_strdup_printf("The search stopped after %d seconds. Pick the controller while its Xbox button flashes quickly.", SEARCH_SECONDS);
    case PAIR_PAIRING: return g_strdup_printf("Pairing with %s…", p->target);
    case PAIR_CONNECTED: return g_strdup_printf("%s is paired and connected.", p->target);
    case PAIR_FAILED: return g_strdup_printf("Couldn't pair: %s. Bluetooth in Settings can pair it too.", p->reason);
    default: return NULL;
    }
}

PairState padpair_state(PadPair *p) { return p->state; }
GPtrArray *padpair_found(PadPair *p) { return p->found; }
const char *padpair_target(PadPair *p) { return p->target; }
const char *padpair_reason(PadPair *p) { return p->reason; }

PadPair *padpair_new(PadPairNotify changed, gpointer user)
{
    PadPair *p = g_new0(PadPair, 1);
    p->changed = changed;
    p->user = user;
    p->found = g_ptr_array_new_with_free_func(found_free);
    p->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    p->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    return p;
}
