// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "padconf.h"

const int PAD_IDLE_MINUTES[PAD_IDLE_CHOICES] = { 5, 10, 15, 30 };

typedef struct { PadConfNotify fn; gpointer user; } Watcher;

struct PadConf {
    PadPrefs prefs;
    char *path;
    GFileMonitor *monitor;
    GArray *watchers;
    guint reload;
};

static int get_int(GKeyFile *kf, const char *key, int fallback, int lo, int hi)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, "pad", key, &e);
    if (e) { g_error_free(e); return fallback; }
    return CLAMP(v, lo, hi);
}

static gboolean get_bool(GKeyFile *kf, const char *key, gboolean fallback)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, "pad", key, &e);
    if (e) { g_error_free(e); return fallback; }
    return v;
}

/* The defaults are Padlight's: alerts and notices on, the rest off. */
static void load(PadConf *c)
{
    PadPrefs *p = &c->prefs;
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, c->path, G_KEY_FILE_NONE, NULL);
    p->low_level = get_int(kf, "low-battery-level", 20, 5, 50);
    p->alerts = get_bool(kf, "low-battery-alerts", TRUE);
    p->notices = get_bool(kf, "connection-notices", TRUE);
    p->keep_awake = get_bool(kf, "keep-awake", FALSE);
    p->idle_disconnect = get_bool(kf, "idle-disconnect", FALSE);
    p->idle_minutes = get_int(kf, "idle-disconnect-minutes", 15, 1, 120);
    p->topbar = get_bool(kf, "topbar", FALSE);
    p->topbar_level = get_bool(kf, "topbar-battery-level", FALSE);
    p->topbar_hide = get_bool(kf, "topbar-hide-without-controller", FALSE);
    p->background = get_bool(kf, "background", TRUE);
    g_free(p->salt);
    p->salt = g_key_file_get_string(kf, "pad", "nickname-salt", NULL);

    g_hash_table_remove_all(p->nicknames);
    char **keys = g_key_file_get_keys(kf, "nicknames", NULL, NULL);
    for (int i = 0; keys && keys[i]; i++) {
        char *v = g_key_file_get_string(kf, "nicknames", keys[i], NULL);
        if (v) g_hash_table_insert(p->nicknames, g_strdup(keys[i]), v);
    }
    g_strfreev(keys);
    g_key_file_free(kf);
}

void padconf_save(PadConf *c)
{
    PadPrefs *p = &c->prefs;
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "pad", "low-battery-level", p->low_level);
    g_key_file_set_boolean(kf, "pad", "low-battery-alerts", p->alerts);
    g_key_file_set_boolean(kf, "pad", "connection-notices", p->notices);
    g_key_file_set_boolean(kf, "pad", "keep-awake", p->keep_awake);
    g_key_file_set_boolean(kf, "pad", "idle-disconnect", p->idle_disconnect);
    g_key_file_set_integer(kf, "pad", "idle-disconnect-minutes", p->idle_minutes);
    g_key_file_set_boolean(kf, "pad", "topbar", p->topbar);
    g_key_file_set_boolean(kf, "pad", "topbar-battery-level", p->topbar_level);
    g_key_file_set_boolean(kf, "pad", "topbar-hide-without-controller", p->topbar_hide);
    g_key_file_set_boolean(kf, "pad", "background", p->background);
    if (p->salt) g_key_file_set_string(kf, "pad", "nickname-salt", p->salt);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, p->nicknames);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_key_file_set_string(kf, "nicknames", k, v);

    char *dir = g_path_get_dirname(c->path);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(kf, c->path, NULL);
    g_free(dir);
    g_key_file_free(kf);
}

static gboolean reload_now(gpointer data)
{
    PadConf *c = data;
    c->reload = 0;
    load(c);
    for (guint i = 0; i < c->watchers->len; i++) {
        Watcher *w = &g_array_index(c->watchers, Watcher, i);
        w->fn(w->user);
    }
    return G_SOURCE_REMOVE;
}

/* The file is replaced whole when saved, so its folder is what's watched. */
static void on_dir_changed(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, PadConf *c)
{
    GFile *target = ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN ? other : file;
    if (!target) return;
    char *name = g_file_get_basename(target), *own = g_path_get_basename(c->path); /* pad-dry-run.ini in a dry run */
    gboolean ours = g_str_equal(name, own);
    g_free(name);
    g_free(own);
    if (ours && !c->reload) c->reload = g_timeout_add(100, reload_now, c);
}

void padconf_watch(PadConf *c, PadConfNotify fn, gpointer user)
{
    Watcher w = { fn, user };
    g_array_append_val(c->watchers, w);
    if (c->monitor) return;
    char *dir = g_path_get_dirname(c->path);
    g_mkdir_with_parents(dir, 0700);
    GFile *f = g_file_new_for_path(dir);
    c->monitor = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (c->monitor) g_signal_connect(c->monitor, "changed", G_CALLBACK(on_dir_changed), c);
    g_object_unref(f);
    g_free(dir);
}

gboolean padconf_needs_helper(const PadPrefs *p)
{
    return p->background && (p->topbar || p->alerts || p->notices || p->keep_awake || p->idle_disconnect);
}

PadConf *padconf_new(void)
{
    PadConf *c = g_new0(PadConf, 1);
    c->path = g_build_filename(g_get_user_config_dir(), "lares",
                               g_getenv("LARES_DRY_RUN") ? "pad-dry-run.ini" : "pad.ini", NULL);
    c->watchers = g_array_new(FALSE, FALSE, sizeof(Watcher));
    c->prefs.nicknames = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    load(c);
    return c;
}

PadPrefs *padconf_prefs(PadConf *c) { return &c->prefs; }
