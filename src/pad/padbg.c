// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "padbg.h"
#include "padactions.h"
#include "padinput.h"
#include <math.h>
#include <string.h>

#define BATTERY_WAIT_MS   2000   /* a notice waits this long for a battery reading */
#define RECHARGE_MARGIN   10     /* an alert comes again only after charging past low + this */
#define HOLD_SECONDS      10     /* the idle inhibitor stays this long after the last input */
#define LOCK_IDLE_SECONDS 60     /* when the screen locks, pads idle this long disconnect */
#define INHIBIT_IDLE      8

typedef struct {
    char *path, *name, *key;     /* key: what a notice for it replaces */
    PadConnection connection;
} Known;

struct PadBg {
    Pads *pads;
    PadConf *conf;
    GDBusConnection *session;
    GHashTable *known;           /* path -> Known, the pads seen last time */
    gboolean scanned;            /* pads there at start get no notice */
    GHashTable *waiting;         /* path -> timeout id, notices waiting for a battery */
    GHashTable *told;            /* path -> alert stage already sent */
    GHashTable *sent;            /* notice key -> notification id */
    GHashTable *readers;         /* path -> PadReader, for activity */
    GHashTable *last_input;      /* path -> gint64 seconds (monotonic), idle disconnect */
    int idle_seconds;
    guint idle_timer, hold_timer;
    gboolean holding;
    guint32 cookie;
};

static gint64 now_s(void) { return g_get_monotonic_time() / G_USEC_PER_SEC; }

static void known_free(gpointer p)
{
    Known *k = p;
    g_free(k->path); g_free(k->name); g_free(k->key);
    g_free(k);
}

/* ---------------------------------------------------------- notifications */

typedef struct { PadBg *bg; char *key; } SentCtx;

static void notified(GObject *src, GAsyncResult *res, gpointer data)
{
    SentCtx *c = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (e) {
        g_printerr("lares-pad: couldn't show a notification: %s\n", e->message);
        g_error_free(e);
    }
    if (v) {
        guint32 id;
        g_variant_get(v, "(u)", &id);
        g_hash_table_insert(c->bg->sent, c->key, GUINT_TO_POINTER(id));
        g_variant_unref(v);
    } else {
        g_free(c->key);
    }
    g_free(c);
}

/* A pad's newest notice replaces the one before, so a quick reconnect
 * leaves one. Connection notices leave the list once seen. */
static void notify_user(PadBg *bg, const char *key, const char *title, const char *body,
                        guint8 urgency, gboolean transient)
{
    if (!bg->session) return;
    guint32 replaces = GPOINTER_TO_UINT(g_hash_table_lookup(bg->sent, key));
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("com.nahuelgenchi.Lares"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(urgency));
    if (transient) g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(TRUE));
    SentCtx *c = g_new0(SentCtx, 1);
    c->bg = bg;
    c->key = g_strdup(key);
    g_dbus_connection_call(bg->session, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", "Lares", replaces, "input-gaming-symbolic", title, body, NULL, &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, notified, c);
}

static char *notice_key(const Pad *pad)
{
    if (pad->address) return g_strconcat("connection ", pad->address, NULL);
    /* The input device's parent stays the same across reconnects on a cable. */
    char *dir = g_path_get_dirname(pad->path);
    char *key = g_strconcat("connection ", dir, NULL);
    g_free(dir);
    return key;
}

static void notice(PadBg *bg, const char *key, const char *name, PadConnection conn,
                   const PadBattery *battery, gboolean connected)
{
    if (!padconf_prefs(bg->conf)->notices) return;
    char *title = g_strdup_printf("%s %s", name, connected ? "connected" : "disconnected");
    char *body;
    if (connected && battery && battery->known) {
        char *b = pad_battery_text(battery);
        body = g_strdup_printf("%s · %s", pad_connection_name(conn), b);
        g_free(b);
    } else {
        body = g_strdup(pad_connection_name(conn));
    }
    notify_user(bg, key, title, body, 1, TRUE);
    g_free(title);
    g_free(body);
}

typedef struct { PadBg *bg; char *path; } WaitCtx;

static gboolean notice_without_battery(gpointer data)
{
    WaitCtx *w = data;
    const Pad *pad = pads_find(w->bg->pads, w->path);
    g_hash_table_remove(w->bg->waiting, w->path);
    if (pad) {
        char *key = notice_key(pad);
        notice(w->bg, key, pad_name(pad), pad->connection, NULL, TRUE);
        g_free(key);
    }
    return G_SOURCE_REMOVE;
}

static void wait_free(gpointer data)
{
    WaitCtx *w = data;
    g_free(w->path);
    g_free(w);
}

static void connection_notices(PadBg *bg, GPtrArray *list)
{
    /* Left: in known, not in the list. */
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, bg->known);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (pads_find(bg->pads, k)) continue;
        Known *gone = v;
        guint wait = GPOINTER_TO_UINT(g_hash_table_lookup(bg->waiting, k));
        if (wait) { g_source_remove(wait); g_hash_table_remove(bg->waiting, k); continue; }   /* gone before its notice */
        notice(bg, gone->key, gone->name, gone->connection, NULL, FALSE);
    }
    /* Arrived: in the list, not known. Or one whose battery just came. */
    for (guint i = 0; i < list->len; i++) {
        Pad *pad = list->pdata[i];
        guint wait = GPOINTER_TO_UINT(g_hash_table_lookup(bg->waiting, pad->path));
        if (wait && pad->battery.known) {
            g_source_remove(wait);
            g_hash_table_remove(bg->waiting, pad->path);
        } else if (wait || g_hash_table_contains(bg->known, pad->path) || !bg->scanned) {
            continue;
        } else if (!pad->battery.known && pad->connection != PAD_USB) {
            /* The battery reading can come a moment after the pad. */
            WaitCtx *w = g_new0(WaitCtx, 1);
            w->bg = bg;
            w->path = g_strdup(pad->path);
            guint id = g_timeout_add_full(G_PRIORITY_DEFAULT, BATTERY_WAIT_MS, notice_without_battery, w, wait_free);
            g_hash_table_insert(bg->waiting, g_strdup(pad->path), GUINT_TO_POINTER(id));
            continue;
        }
        char *key = notice_key(pad);
        notice(bg, key, pad_name(pad), pad->connection, pad->connection == PAD_USB ? NULL : &pad->battery, TRUE);
        g_free(key);
    }
    g_hash_table_remove_all(bg->known);
    for (guint i = 0; i < list->len; i++) {
        Pad *pad = list->pdata[i];
        Known *kn = g_new0(Known, 1);
        kn->path = g_strdup(pad->path);
        kn->name = g_strdup(pad_name(pad));
        kn->key = notice_key(pad);
        kn->connection = pad->connection;
        g_hash_table_insert(bg->known, kn->path, kn);
    }
    bg->scanned = TRUE;
}

/* --------------------------------------------------------- battery alerts */

/* Each stage comes once, until the battery charges back above the level.
 * None for a pad on a cable. */
static void battery_alerts(PadBg *bg, GPtrArray *list)
{
    PadPrefs *prefs = padconf_prefs(bg->conf);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, bg->told);
    while (g_hash_table_iter_next(&it, &k, NULL))
        if (!pads_find(bg->pads, k)) g_hash_table_iter_remove(&it);

    for (guint i = 0; i < list->len; i++) {
        Pad *pad = list->pdata[i];
        const PadBattery *b = &pad->battery;
        if (!b->known || pad->connection == PAD_USB) continue;
        int stage = pad_battery_low_stage(b, prefs->low_level);
        gboolean charging = pad_battery_charging(b);
        if (stage == 0 && (charging || pad_battery_low_stage(b, prefs->low_level + RECHARGE_MARGIN) == 0)) {
            g_hash_table_remove(bg->told, pad->path);
            continue;
        }
        int told = GPOINTER_TO_INT(g_hash_table_lookup(bg->told, pad->path));
        if (!prefs->alerts || charging || stage <= told) continue;
        g_hash_table_insert(bg->told, g_strdup(pad->path), GINT_TO_POINTER(stage));
        const char *level = pad_battery_level_name(b);
        char *lower = level ? g_utf8_strdown(level, -1) : NULL;
        char *left = level ? g_strdup_printf("Its charge is %s.", lower)
                           : g_strdup_printf("%d%% left.", (int)lround(b->percentage));
        char *title = g_strdup_printf("%s battery %s", pad_name(pad), stage > 1 ? "very low" : "low");
        char *body = g_strdup_printf("%s %s", left, stage > 1 ? "Charge it or change its batteries before it switches off."
                                                              : "Charge it or change its batteries soon.");
        char *key = g_strconcat("battery ", pad->path, NULL);
        notify_user(bg, key, title, body, stage > 1 ? 2 : 1, FALSE);
        g_free(key); g_free(title); g_free(body); g_free(lower); g_free(left);
    }
}

/* ------------------------------------------------------------ keep awake */

static void uninhibit(PadBg *bg, guint32 cookie)
{
    g_dbus_connection_call(bg->session, "org.gnome.SessionManager", "/org/gnome/SessionManager",
                           "org.gnome.SessionManager", "Uninhibit", g_variant_new("(u)", cookie), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void inhibited(GObject *src, GAsyncResult *res, gpointer data)
{
    PadBg *bg = data;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!v) { bg->holding = FALSE; return; }
    guint32 cookie;
    g_variant_get(v, "(u)", &cookie);
    g_variant_unref(v);
    /* Granted after letting go, or one too many: give it back at once. */
    if (bg->holding && !bg->cookie) bg->cookie = cookie;
    else uninhibit(bg, cookie);
}

static void release(PadBg *bg)
{
    if (bg->hold_timer) { g_source_remove(bg->hold_timer); bg->hold_timer = 0; }
    if (!bg->holding) return;
    bg->holding = FALSE;
    if (bg->cookie) uninhibit(bg, bg->cookie);
    bg->cookie = 0;
}

static gboolean hold_over(gpointer data)
{
    PadBg *bg = data;
    bg->hold_timer = 0;
    release(bg);
    return G_SOURCE_REMOVE;
}

/* As a video player does while it plays. GNOME counts idle time again once
 * the inhibitor goes, so the screen then dims after Settings › Power's
 * delays, as it would after the last key press. */
static void keep_awake(PadBg *bg)
{
    if (!bg->session) return;
    if (!bg->holding) {
        bg->holding = TRUE;
        g_dbus_connection_call(bg->session, "org.gnome.SessionManager", "/org/gnome/SessionManager",
            "org.gnome.SessionManager", "Inhibit",
            g_variant_new("(susu)", "Lares", 0, "A game controller is in use", INHIBIT_IDLE),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, inhibited, bg);
    }
    if (bg->hold_timer) g_source_remove(bg->hold_timer);
    bg->hold_timer = g_timeout_add_seconds(HOLD_SECONDS, hold_over, bg);
}

/* --------------------------------------------------------- idle disconnect */

static gboolean can_idle_disconnect(const Pad *pad)
{
    return pad->connection == PAD_BLUETOOTH && pad->address;
}

static void disconnect_idle(PadBg *bg, int seconds)
{
    gint64 now = now_s();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, bg->last_input);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (now - (gint64)GPOINTER_TO_SIZE(v) < seconds) continue;
        const Pad *pad = pads_find(bg->pads, k);
        if (pad) pad_disconnect(pad);
        g_hash_table_iter_replace(&it, GSIZE_TO_POINTER(now));
    }
}

static gboolean idle_due(gpointer data);

static void schedule_idle(PadBg *bg)
{
    if (bg->idle_timer) { g_source_remove(bg->idle_timer); bg->idle_timer = 0; }
    if (!g_hash_table_size(bg->last_input) || !bg->idle_seconds) return;
    gint64 next = G_MAXINT64;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, bg->last_input);
    while (g_hash_table_iter_next(&it, NULL, &v)) next = MIN(next, (gint64)GPOINTER_TO_SIZE(v) + bg->idle_seconds);
    gint64 wait = MAX(1, next - now_s());
    bg->idle_timer = g_timeout_add_seconds((guint)wait, idle_due, bg);
}

/* When it comes, a pad used meanwhile isn't idle yet; the timer waits again. */
static gboolean idle_due(gpointer data)
{
    PadBg *bg = data;
    bg->idle_timer = 0;
    disconnect_idle(bg, bg->idle_seconds);
    schedule_idle(bg);
    return G_SOURCE_REMOVE;
}

/* --------------------------------------------------------------- readers */

typedef struct { PadBg *bg; char *path; PadReader *reader; } ReaderCtx;

static void on_input(gpointer data)
{
    ReaderCtx *c = data;
    PadPrefs *prefs = padconf_prefs(c->bg->conf);
    if (prefs->keep_awake) keep_awake(c->bg);
    if (g_hash_table_contains(c->bg->last_input, c->path))
        g_hash_table_insert(c->bg->last_input, g_strdup(c->path), GSIZE_TO_POINTER(now_s()));
}

static void reader_free(gpointer data)
{
    ReaderCtx *c = data;
    pad_reader_free(c->reader);
    g_free(c->path);
    g_free(c);
}

/* A reader for every pad while keeping the screen awake, or for each
 * Bluetooth pad while disconnecting idle ones. A new connection is a new
 * path, so a pad that comes back gets a new reader. */
static void sync_activity(PadBg *bg, GPtrArray *list)
{
    PadPrefs *prefs = padconf_prefs(bg->conf);
    gboolean awake = prefs->keep_awake, idle = prefs->idle_disconnect;
    GHashTable *want = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < list->len; i++) {
        Pad *pad = list->pdata[i];
        if (awake || (idle && can_idle_disconnect(pad))) g_hash_table_add(want, pad->path);
    }
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, bg->readers);
    while (g_hash_table_iter_next(&it, &k, NULL))
        if (!g_hash_table_contains(want, k)) g_hash_table_iter_remove(&it);
    for (guint i = 0; i < list->len; i++) {
        Pad *pad = list->pdata[i];
        if (!g_hash_table_contains(want, pad->path) || g_hash_table_contains(bg->readers, pad->path)) continue;
        ReaderCtx *c = g_new0(ReaderCtx, 1);
        c->bg = bg;
        c->path = g_strdup(pad->path);
        c->reader = pad_reader_activity(pad->event_node, on_input, c);
        if (c->reader) g_hash_table_insert(bg->readers, g_strdup(pad->path), c);
        else { g_free(c->path); g_free(c); }
    }
    g_hash_table_unref(want);
    if (!awake || !list->len) release(bg);

    /* Idle disconnect: a new time counts every pad from now. */
    int seconds = idle ? prefs->idle_minutes * 60 : 0;
    if (seconds != bg->idle_seconds) g_hash_table_remove_all(bg->last_input);
    bg->idle_seconds = seconds;
    g_hash_table_iter_init(&it, bg->last_input);
    while (g_hash_table_iter_next(&it, &k, NULL)) {
        const Pad *pad = pads_find(bg->pads, k);
        if (!pad || !idle || !can_idle_disconnect(pad)) g_hash_table_iter_remove(&it);
    }
    for (guint i = 0; idle && i < list->len; i++) {
        Pad *pad = list->pdata[i];
        if (can_idle_disconnect(pad) && !g_hash_table_contains(bg->last_input, pad->path))
            g_hash_table_insert(bg->last_input, g_strdup(pad->path), GSIZE_TO_POINTER(now_s()));
    }
    schedule_idle(bg);
}

/* When the screen locks or blanks, pads with no input for a minute
 * disconnect at once, whatever the chosen time. */
static void on_screensaver(GDBusConnection *c, const char *s, const char *path, const char *iface,
                           const char *signal, GVariant *params, gpointer data)
{
    PadBg *bg = data;
    gboolean active = FALSE;
    g_variant_get(params, "(b)", &active);
    if (active && padconf_prefs(bg->conf)->idle_disconnect) disconnect_idle(bg, LOCK_IDLE_SECONDS);
}

/* ---------------------------------------------------------------- set up */

static void on_pads(gpointer data)
{
    PadBg *bg = data;
    GPtrArray *list = pads_list(bg->pads);
    connection_notices(bg, list);
    battery_alerts(bg, list);
    sync_activity(bg, list);
}

PadBg *padbg_new(Pads *pads, PadConf *conf)
{
    PadBg *bg = g_new0(PadBg, 1);
    bg->pads = pads;
    bg->conf = conf;
    bg->session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    bg->known = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, known_free);
    bg->waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    bg->told = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    bg->sent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    bg->readers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, reader_free);
    bg->last_input = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (bg->session)
        g_dbus_connection_signal_subscribe(bg->session, NULL, "org.gnome.ScreenSaver", "ActiveChanged",
            "/org/gnome/ScreenSaver", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_screensaver, bg, NULL);
    pads_watch(pads, on_pads, bg);
    padconf_watch(conf, on_pads, bg);
    on_pads(bg);
    return bg;
}
