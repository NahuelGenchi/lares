// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The keyboard lighting the helper plays live (issue #57): an effect a
 * Logitech keyboard can't keep in its onboard profile, Off, Solid,
 * Breathing or Colour cycle, set under software control as Logitech G HUB
 * does. The keyboard then stops dimming itself: about a minute after its
 * last key it says it's idle, and the effect plays at half strength until
 * it's used again. A keyboard that reconnects, after sleeping or a power
 * cycle, is given the effect again. When the setting goes or the helper
 * stops, the lights go back to the keyboard, which plays what its profile
 * keeps.
 *
 * Nothing polls: the receiver's node is listened to only while an effect is
 * wanted, and /dev is watched for the receiver coming back. Requests run in
 * a worker thread, one at a time; a change that comes meanwhile follows. */
#include "livelight.h"
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>
#include "../hidpp.h"

#define PLUGGED_MS 1000   /* a new node gets its permissions from udev a moment after it appears */

struct LiveLight {
    PadConf *conf;
    gboolean dry;          /* a dry run leaves the real keyboard alone */
    char *node;            /* the receiver's HID++ node, while one is plugged in */
    guint8 index;
    HidppEffects fx;
    gboolean known;        /* `fx` was read from this node */
    int fd;                /* listened to while an effect is wanted, -1 otherwise */
    guint watch, plugged;
    GFileMonitor *dev;
    gboolean held;         /* software holds the lights now */
    HidppLive state;       /* what the keyboard was last told */
    gboolean busy, again;  /* a request is on its way; another is due after it */
};

typedef struct {
    LiveLight *l;
    char *node;
    guint8 index;
    HidppEffects fx;
    gboolean known, release;
    HidppLighting light;
    HidppLive state;
    gboolean ok, asleep;
} Job;

static void sync_light(LiveLight *l);

static const PadPrefs *prefs(LiveLight *l) { return padconf_prefs(l->conf); }

static gboolean wanted(LiveLight *l)
{
    return !l->dry && key_light_live(prefs(l)->key_light);
}

/* ------------------------------------------------------------ requests */

static void job_free(gpointer p)
{
    Job *j = p;
    g_free(j->node);
    g_free(j);
}

static void job_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    Job *j = data;
    GError *e = NULL;
    j->ok = j->known || hidpp_live_effects(j->node, j->index, &j->fx, &e);
    if (j->ok) {
        j->known = TRUE;
        j->ok = j->release ? hidpp_live_release(j->node, j->index, &j->fx, &e)
                           : hidpp_live_play(j->node, j->index, &j->fx, &j->light, j->state, &e);
    }
    j->asleep = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED)
             || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    if (e && !j->asleep) g_printerr("lares-pad: keyboard lighting: %s\n", e->message);
    g_clear_error(&e);
    g_task_return_boolean(task, TRUE);
}

static void job_done(GObject *src, GAsyncResult *res, gpointer user)
{
    Job *j = g_task_get_task_data(G_TASK(res));
    LiveLight *l = j->l;
    l->busy = FALSE;
    if (l->node && g_str_equal(l->node, j->node)) {
        if (j->known) {
            l->fx = j->fx;
            l->known = TRUE;
        }
        /* Asleep, it has no lights to hold; it takes them again as it wakes. */
        l->held = j->ok ? !j->release : j->asleep ? FALSE : l->held;
    }
    if (l->again) {
        l->again = FALSE;
        sync_light(l);
    }
}

static void request(LiveLight *l, gboolean release)
{
    if (l->busy) {
        l->again = TRUE;
        return;
    }
    Job *j = g_new0(Job, 1);
    j->l = l;
    j->node = g_strdup(l->node);
    j->index = l->index;
    j->fx = l->fx;
    j->known = l->known;
    j->release = release;
    j->light.effect = prefs(l)->key_light;
    memcpy(j->light.rgb, prefs(l)->key_rgb, 3);
    j->state = l->state;
    l->busy = TRUE;
    GTask *task = g_task_new(NULL, NULL, job_done, NULL);
    g_task_set_task_data(task, j, job_free);
    g_task_run_in_thread(task, job_thread);
    g_object_unref(task);
}

/* ------------------------------------------------------------- the node */

static void stop_listening(LiveLight *l)
{
    if (l->watch) g_source_remove(l->watch);
    if (l->fd >= 0) close(l->fd);
    l->watch = 0;
    l->fd = -1;
}

static void forget_node(LiveLight *l)
{
    stop_listening(l);
    g_clear_pointer(&l->node, g_free);
    l->known = l->held = FALSE;
}

/* What the keyboard or its receiver says by itself: the keyboard came back,
 * so it's given the effect again; it went; or, while software holds its
 * lights, it was left alone or used again, which is answered with the
 * effect at half or full strength. The answers to anyone's requests aren't
 * events. */
static gboolean on_report(int fd, GIOCondition cond, gpointer data)
{
    LiveLight *l = data;
    HidppKeyboard idx = { .status_index = l->fx.status, .rgb_index = l->known ? l->fx.feature : 0 };
    guint8 r[64];
    ssize_t got;
    gboolean again = FALSE;
    while ((got = read(fd, r, sizeof r)) > 0) {
        HidppEvent ev;
        if (!hidpp_event(r, (gsize)got, l->index, &idx, &ev)) continue;
        switch (ev.kind) {
        case HIDPP_EVENT_LINK:
            l->held = FALSE;
            if (ev.linked) {
                l->state = HIDPP_LIVE_TAKE;
                again = TRUE;
            }
            break;
        case HIDPP_EVENT_IDLE:
        case HIDPP_EVENT_ACTIVE:
            if (!l->held) break;
            l->state = ev.kind == HIDPP_EVENT_IDLE ? HIDPP_LIVE_IDLE : HIDPP_LIVE_ACTIVE;
            again = TRUE;
            break;
        default:
            break;
        }
    }
    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
        l->watch = 0;
        forget_node(l);   /* unplugged: /dev says when it's back */
        return G_SOURCE_REMOVE;
    }
    if (again) sync_light(l);
    return G_SOURCE_CONTINUE;
}

static gboolean find_node(LiveLight *l)
{
    if (l->node) return TRUE;
    const HidppModel *model;
    l->node = hidpp_find(&l->index, &model);
    l->known = l->held = FALSE;
    return l->node != NULL;
}

static void listen_to(LiveLight *l)
{
    if (l->fd >= 0) return;
    l->fd = open(l->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (l->fd >= 0) l->watch = g_unix_fd_add(l->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_report, l);
}

/* -------------------------------------------------------------- choices */

/* Plays what the settings ask, or gives the lights back. */
static void sync_light(LiveLight *l)
{
    if (wanted(l)) {
        if (!find_node(l) || access(l->node, R_OK | W_OK) != 0) return;
        listen_to(l);
        request(l, FALSE);
    } else if (l->node && l->held) {
        request(l, TRUE);
        stop_listening(l);
    } else if (!l->busy) {
        stop_listening(l);
    }
}

static void on_conf(gpointer data)
{
    LiveLight *l = data;
    if (!l->held) l->state = HIDPP_LIVE_TAKE;
    sync_light(l);
}

static gboolean plugged_now(gpointer data)
{
    LiveLight *l = data;
    l->plugged = 0;
    l->state = HIDPP_LIVE_TAKE;
    sync_light(l);
    return G_SOURCE_REMOVE;
}

static void on_dev(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer data)
{
    LiveLight *l = data;
    char *name = g_file_get_basename(file);
    if (ev == G_FILE_MONITOR_EVENT_CREATED && g_str_has_prefix(name, "hidraw") && wanted(l) && !l->node
        && !l->plugged)
        l->plugged = g_timeout_add(PLUGGED_MS, plugged_now, l);
    g_free(name);
}

LiveLight *livelight_new(PadConf *conf)
{
    LiveLight *l = g_new0(LiveLight, 1);
    l->conf = conf;
    l->dry = g_getenv("LARES_DRY_RUN") != NULL;
    l->fd = -1;
    l->state = HIDPP_LIVE_TAKE;
    padconf_watch(conf, on_conf, l);
    GFile *dev = g_file_new_for_path("/dev");
    l->dev = g_file_monitor_directory(dev, G_FILE_MONITOR_NONE, NULL, NULL);
    if (l->dev) g_signal_connect(l->dev, "changed", G_CALLBACK(on_dev), l);
    g_object_unref(dev);
    sync_light(l);
    return l;
}

/* The lights go back to the keyboard before the helper ends, so it plays
 * what its profile keeps; a request on its way is left to finish first. */
void livelight_free(LiveLight *l)
{
    if (!l) return;
    while (l->busy) g_main_context_iteration(NULL, TRUE);
    if (l->node && l->held) hidpp_live_release(l->node, l->index, &l->fx, NULL);
    if (l->plugged) g_source_remove(l->plugged);
    if (l->dev) g_object_unref(l->dev);
    forget_node(l);
    g_free(l);
}
