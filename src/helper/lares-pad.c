// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-pad: Lares' background helper. It runs connection notices, low
 * battery alerts, keeping the screen awake and idle disconnect while the
 * Lares window is closed, plays a keyboard's live lighting, and gives the
 * top-bar addon everything it shows over D-Bus, so the addon holds no logic
 * of its own. GLib only, no GTK. It ends by itself when the settings no
 * longer need it. */
#include <gio/gio.h>
#include <glib-unix.h>
#include "../pad/padactions.h"
#include "../pad/padbg.h"
#include "../pad/padconf.h"
#include "../pad/padinput.h"
#include "../pad/pads.h"
#include "livelight.h"

#define BUS_NAME "com.nahuelgenchi.Lares.Pad"
#define OBJ_PATH "/com/nahuelgenchi/Lares/Pad"
#define IFACE    "com.nahuelgenchi.Lares.Pad"

static const char INTROSPECTION[] =
    "<node>"
    " <interface name='" IFACE "'>"
    "  <method name='GetState'>"
    "   <arg type='aa{sv}' name='pads' direction='out'/>"
    "   <arg type='a{sv}' name='prefs' direction='out'/>"
    "  </method>"
    "  <method name='Watch'><arg type='s' name='path' direction='in'/></method>"
    "  <method name='Unwatch'><arg type='s' name='path' direction='in'/></method>"
    "  <method name='Rumble'><arg type='s' name='path' direction='in'/></method>"
    "  <method name='Find'><arg type='s' name='path' direction='in'/></method>"
    "  <method name='Disconnect'><arg type='s' name='path' direction='in'/></method>"
    "  <signal name='Changed'/>"
    "  <signal name='Input'><arg type='s' name='path'/><arg type='(udddddd)' name='state'/></signal>"
    " </interface>"
    "</node>";

typedef struct {
    GMainLoop *loop;
    GDBusConnection *bus;
    PadConf *conf;
    Pads *pads;
    PadBg *bg;
    LiveLight *light;
    GHashTable *live;        /* pad path -> PadReader, while the addon's menu is open */
    GHashTable *watchers;    /* sender -> name watch id */
} Helper;

static Helper H;

static void emit_changed(void)
{
    if (H.bus)
        g_dbus_connection_emit_signal(H.bus, NULL, OBJ_PATH, IFACE, "Changed", NULL, NULL);
}

/* ----------------------------------------------------------------- state */

static GVariant *pad_dict(const Pad *pad, int low_level)
{
    GVariantDict d;
    g_variant_dict_init(&d, NULL);
    g_variant_dict_insert(&d, "path", "s", pad->path);
    g_variant_dict_insert(&d, "name", "s", pad_name(pad));
    g_variant_dict_insert(&d, "model-name", "s", pad->name);
    g_variant_dict_insert(&d, "connection", "s", pad_connection_name(pad->connection));
    g_variant_dict_insert(&d, "since", "x", pad->since);
    g_variant_dict_insert(&d, "rumble", "b", pad->rumble);
    g_variant_dict_insert(&d, "wireless", "b", pad->connection != PAD_USB);
    g_variant_dict_insert(&d, "bluetooth", "b", pad->connection == PAD_BLUETOOTH && pad->address);
    g_variant_dict_insert(&d, "finding", "b", pad_finding(pad->path));
    if (pad->battery.known) {
        char *text = pad_battery_text(&pad->battery), *charge = pad_battery_charge(&pad->battery);
        g_variant_dict_insert(&d, "battery", "d", pad_battery_fraction(&pad->battery));
        g_variant_dict_insert(&d, "battery-text", "s", text);
        g_variant_dict_insert(&d, "battery-charge", "s", charge);
        g_variant_dict_insert(&d, "charging", "b", pad_battery_charging(&pad->battery));
        g_variant_dict_insert(&d, "low", "b", !pad_battery_charging(&pad->battery)
                                              && pad_battery_low_stage(&pad->battery, low_level) > 0);
        g_free(text);
        g_free(charge);
    } else {
        g_variant_dict_insert(&d, "battery", "d", -1.0);
        g_variant_dict_insert(&d, "battery-text", "s", pad->connection == PAD_USB ? "On USB power" : "");
    }
    return g_variant_dict_end(&d);
}

static GVariant *get_state(void)
{
    PadPrefs *p = padconf_prefs(H.conf);
    GVariantBuilder pads;
    g_variant_builder_init(&pads, G_VARIANT_TYPE("aa{sv}"));
    GPtrArray *list = pads_list(H.pads);
    for (guint i = 0; i < list->len; i++) g_variant_builder_add_value(&pads, pad_dict(list->pdata[i], p->low_level));
    GVariantDict prefs;
    g_variant_dict_init(&prefs, NULL);
    g_variant_dict_insert(&prefs, "battery-level", "b", p->topbar_level);
    g_variant_dict_insert(&prefs, "hide-without-controller", "b", p->topbar_hide);
    return g_variant_new("(aa{sv}@a{sv})", &pads, g_variant_dict_end(&prefs));
}

/* ------------------------------------------------------------ live input */

static void on_live(const PadState *s, gpointer data)
{
    const char *path = data;
    if (!H.bus) return;
    g_dbus_connection_emit_signal(H.bus, NULL, OBJ_PATH, IFACE, "Input",
        g_variant_new("(s(udddddd))", path, s->buttons, s->lx, s->ly, s->rx, s->ry, s->lt, s->rt), NULL);
}

static void stop_live(void)
{
    g_hash_table_remove_all(H.live);
}

/* The addon's menu went away with its process: stop reading for it. */
static void watcher_vanished(GDBusConnection *c, const char *name, gpointer data)
{
    g_hash_table_remove(H.watchers, name);
    if (!g_hash_table_size(H.watchers)) stop_live();
}

static void watch(const char *sender, const char *path)
{
    const Pad *pad = pads_find(H.pads, path);
    if (!pad || g_hash_table_contains(H.live, path)) return;
    char *key = g_strdup(path);
    PadReader *r = pad_reader_live(pad->event_node, FALSE, on_live, key);
    if (!r) { g_free(key); return; }
    g_hash_table_insert(H.live, key, r);
    if (!g_hash_table_contains(H.watchers, sender))
        g_hash_table_insert(H.watchers, g_strdup(sender),
            GUINT_TO_POINTER(g_bus_watch_name_on_connection(H.bus, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                            NULL, watcher_vanished, NULL, NULL)));
}

/* --------------------------------------------------------------- methods */

static void find_done(gpointer data) { emit_changed(); }

static void on_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                    const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    if (g_str_equal(method, "GetState")) {
        g_dbus_method_invocation_return_value(inv, get_state());
        return;
    }
    const char *pad_path;
    g_variant_get(params, "(&s)", &pad_path);
    const Pad *pad = pads_find(H.pads, pad_path);
    if (g_str_equal(method, "Watch")) watch(sender, pad_path);
    else if (g_str_equal(method, "Unwatch")) g_hash_table_remove(H.live, pad_path);
    else if (pad && g_str_equal(method, "Rumble")) pad_rumble(pad, PAD_RUMBLE_STRENGTH, PAD_RUMBLE_STRENGTH, PAD_RUMBLE_MS);
    else if (pad && g_str_equal(method, "Find")) {
        if (pad_finding(pad_path)) pad_find_stop(pad_path);
        else pad_find_start(pad, find_done, NULL);
        emit_changed();
    } else if (pad && g_str_equal(method, "Disconnect")) pad_disconnect(pad);
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable VTABLE = { on_call, NULL, NULL, { 0 } };

/* --------------------------------------------------------------- set up */

static void on_pads(gpointer data)
{
    /* Readers for pads that left go with them. */
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, H.live);
    while (g_hash_table_iter_next(&it, &k, NULL)) if (!pads_find(H.pads, k)) g_hash_table_iter_remove(&it);
    emit_changed();
}

static void on_conf(gpointer data)
{
    if (!padconf_needs_helper(padconf_prefs(H.conf))) g_main_loop_quit(H.loop);
    else emit_changed();
}

static void on_bus(GDBusConnection *bus, const char *name, gpointer data)
{
    H.bus = bus;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(INTROSPECTION, NULL);
    g_dbus_connection_register_object(bus, OBJ_PATH, info->interfaces[0], &VTABLE, NULL, NULL, NULL);
    g_dbus_node_info_unref(info);
}

static void on_name_lost(GDBusConnection *bus, const char *name, gpointer data)
{
    g_printerr("lares-pad: another helper is running\n");
    g_main_loop_quit(H.loop);
}

static gboolean on_term(gpointer data)
{
    g_main_loop_quit(H.loop);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    H.loop = g_main_loop_new(NULL, FALSE);
    H.conf = padconf_new();
    if (!padconf_needs_helper(padconf_prefs(H.conf))) return 0;
    H.live = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)pad_reader_free);
    H.watchers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    H.pads = pads_new(H.conf);
    H.bg = padbg_new(H.pads, H.conf);
    pads_watch(H.pads, on_pads, NULL);
    padconf_watch(H.conf, on_conf, NULL);
    H.light = livelight_new(H.conf);
    g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus, NULL, on_name_lost, NULL, NULL);
    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);
    g_main_loop_run(H.loop);
    livelight_free(H.light);   /* the keyboard plays its own lighting again */
    return 0;
}
