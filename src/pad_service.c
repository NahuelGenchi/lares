// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Starts and stops the background helper, and switches the top-bar addon,
 * to match the controller settings. */
#include "pad_service.h"

#define HELPER_UNIT "lares-pad.service"
#define SYSTEMD     "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define MANAGER     "org.freedesktop.systemd1.Manager"
#define SHELL       "org.gnome.Shell"
#define SHELL_PATH  "/org/gnome/Shell"
#define EXTENSIONS  "org.gnome.Shell.Extensions"

static GDBusConnection *bus(void)
{
    static GDBusConnection *b;
    if (!b) b = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    return b;
}

static gboolean dry(void) { return g_getenv("LARES_DRY_RUN") != NULL; }

static void ignore(GObject *src, GAsyncResult *res, gpointer data)
{
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (v) g_variant_unref(v);
}

static void call(const char *dest, const char *path, const char *iface, const char *method, GVariant *params,
                 GAsyncReadyCallback done, gpointer user)
{
    g_dbus_connection_call(bus(), dest, path, iface, method, params, NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL,
                           done ? done : ignore, user);
}

/* -------------------------------------------------------------- helper */

static void start_helper(GObject *src, GAsyncResult *res, gpointer data)
{
    ignore(src, res, NULL);
    call(SYSTEMD, SYSTEMD_PATH, MANAGER, "StartUnit", g_variant_new("(ss)", HELPER_UNIT, "replace"), NULL, NULL);
}

/* systemd learns of a unit file installed since it started only on a
 * reload, so one comes between enabling and starting. */
static void reload_then_start(GObject *src, GAsyncResult *res, gpointer data)
{
    ignore(src, res, NULL);
    call(SYSTEMD, SYSTEMD_PATH, MANAGER, "Reload", NULL, start_helper, NULL);
}

static void sync_helper(gboolean wanted)
{
    const char *units[] = { HELPER_UNIT, NULL };
    if (wanted)
        call(SYSTEMD, SYSTEMD_PATH, MANAGER, "EnableUnitFiles", g_variant_new("(^asbb)", units, FALSE, TRUE),
             reload_then_start, NULL);
    else {
        call(SYSTEMD, SYSTEMD_PATH, MANAGER, "StopUnit", g_variant_new("(ss)", HELPER_UNIT, "replace"), NULL, NULL);
        call(SYSTEMD, SYSTEMD_PATH, MANAGER, "DisableUnitFiles", g_variant_new("(^asb)", units, FALSE), NULL, NULL);
    }
}

/* --------------------------------------------------------------- addon */

/* GNOME Shell loads a new addon only at log-in. Until it has, enabling it
 * means listing it in the shell's settings, so it starts at the next one. */
static void list_for_next_login(gboolean on)
{
    GSettingsSchema *schema = g_settings_schema_source_lookup(g_settings_schema_source_get_default(),
                                                             "org.gnome.shell", TRUE);
    if (!schema) return;
    g_settings_schema_unref(schema);
    GSettings *s = g_settings_new("org.gnome.shell");
    char **list = g_settings_get_strv(s, "enabled-extensions");
    GStrvBuilder *b = g_strv_builder_new();
    for (int i = 0; list[i]; i++)
        if (!g_str_equal(list[i], PAD_ADDON_UUID)) g_strv_builder_add(b, list[i]);
    if (on) g_strv_builder_add(b, PAD_ADDON_UUID);
    char **next = g_strv_builder_end(b);
    g_settings_set_strv(s, "enabled-extensions", (const char *const *)next);
    g_settings_sync();
    g_strfreev(next);
    g_strv_builder_unref(b);
    g_strfreev(list);
    g_object_unref(s);
}

typedef struct { gboolean on; PadAddonFn fn; gpointer user; } AddonCtx;

static PadAddonState state_of(GVariant *info)
{
    double state = 0;
    if (!info || !g_variant_lookup(info, "state", "d", &state)) return PAD_ADDON_NEEDS_LOGIN;
    return state == 1 ? PAD_ADDON_ON : PAD_ADDON_OFF;
}

static void got_info(GObject *src, GAsyncResult *res, gpointer data)
{
    AddonCtx *c = data;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    GVariant *info = v ? g_variant_get_child_value(v, 0) : NULL;
    PadAddonState state = v ? state_of(info) : PAD_ADDON_NO_SHELL;
    if (info) g_variant_unref(info);
    if (v) g_variant_unref(v);
    if (c->fn) c->fn(state, c->user);
    g_free(c);
}

void pad_service_addon_state(PadAddonFn fn, gpointer user)
{
    AddonCtx *c = g_new0(AddonCtx, 1);
    c->fn = fn;
    c->user = user;
    if (dry() || !bus()) { fn(PAD_ADDON_NO_SHELL, user); g_free(c); return; }
    g_dbus_connection_call(bus(), SHELL, SHELL_PATH, EXTENSIONS, "GetExtensionInfo",
                           g_variant_new("(s)", PAD_ADDON_UUID), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, got_info, c);
}

static void switch_addon(PadAddonState state, gpointer data)
{
    gboolean on = GPOINTER_TO_INT(data);
    if (state == PAD_ADDON_NEEDS_LOGIN) list_for_next_login(on);
    else if (state != PAD_ADDON_NO_SHELL)
        call(SHELL, SHELL_PATH, EXTENSIONS, on ? "EnableExtension" : "DisableExtension",
             g_variant_new("(s)", PAD_ADDON_UUID), NULL, NULL);
}

/* ---------------------------------------------------------------- public */

void pad_service_sync(const PadPrefs *p)
{
    if (dry() || !bus()) return;
    sync_helper(padconf_needs_helper(p));
    pad_service_addon_state(switch_addon, GINT_TO_POINTER(p->background && p->topbar));
}
