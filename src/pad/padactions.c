// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "padactions.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gdesktopappinfo.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FIND_BURSTS   10
#define FIND_ON_MS    300
#define FIND_EVERY_MS 600

/* --------------------------------------------------------------- rumble */

/* 0 to 100 as the kernel's 16-bit magnitude, rounded up so drivers that
 * scale back to percent, such as hid-microsoft, get the same number. */
static guint16 magnitude(int percent)
{
    return (guint16)ceil(CLAMP(percent, 0, 100) * 0xFFFF / 100.0);
}

static int upload(int fd, int strong, int weak, int length_ms)
{
    struct ff_effect e = { 0 };
    e.type = FF_RUMBLE;
    e.id = -1;
    e.replay.length = (guint16)length_ms;
    e.u.rumble.strong_magnitude = magnitude(strong);
    e.u.rumble.weak_magnitude = magnitude(weak);
    return ioctl(fd, EVIOCSFF, &e) < 0 ? -1 : e.id;
}

static gboolean play(int fd, int id)
{
    struct input_event ev = { .type = EV_FF, .code = (guint16)id, .value = 1 };
    return write(fd, &ev, sizeof ev) == sizeof ev;
}

/* Closing the device removes the effect, so the fd stays open until the
 * rumble has played. */
static gboolean close_later(gpointer data)
{
    close(GPOINTER_TO_INT(data));
    return G_SOURCE_REMOVE;
}

gboolean pad_rumble(const Pad *pad, int strong, int weak, int length_ms)
{
    if (!pad->rumble) return FALSE;
    length_ms = CLAMP(length_ms, 50, 2000);
    int fd = open(pad->event_node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return FALSE;
    int id = upload(fd, strong, weak, length_ms);
    if (id < 0 || !play(fd, id)) { close(fd); return FALSE; }
    g_timeout_add(length_ms + 100, close_later, GINT_TO_POINTER(fd));
    return TRUE;
}

/* ----------------------------------------------------------------- find */

typedef struct {
    char *path;
    int fd, id, left;
    guint timer;
    PadFindDone done;
    gpointer user;
} Finding;

static GHashTable *findings;   /* pad path -> Finding */

static void finding_end(Finding *f)
{
    if (f->timer) g_source_remove(f->timer);
    close(f->fd);
    PadFindDone done = f->done;
    gpointer user = f->user;
    g_hash_table_remove(findings, f->path);   /* frees f */
    if (done) done(user);
}

static void finding_free(gpointer p)
{
    Finding *f = p;
    g_free(f->path);
    g_free(f);
}

static gboolean burst(gpointer data)
{
    Finding *f = data;
    if (f->left-- <= 0 || !play(f->fd, f->id)) {
        f->timer = 0;
        finding_end(f);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean pad_find_start(const Pad *pad, PadFindDone done, gpointer user)
{
    if (!findings) findings = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, finding_free);
    if (!pad->rumble || g_hash_table_contains(findings, pad->path)) return FALSE;
    int fd = open(pad->event_node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return FALSE;
    int id = upload(fd, 100, 100, FIND_ON_MS);
    if (id < 0) { close(fd); return FALSE; }
    Finding *f = g_new0(Finding, 1);
    f->path = g_strdup(pad->path);
    f->fd = fd;
    f->id = id;
    f->left = FIND_BURSTS - 1;
    f->done = done;
    f->user = user;
    g_hash_table_insert(findings, f->path, f);
    play(fd, id);
    f->timer = g_timeout_add(FIND_EVERY_MS, burst, f);
    return TRUE;
}

void pad_find_stop(const char *pad_path)
{
    Finding *f = findings ? g_hash_table_lookup(findings, pad_path) : NULL;
    if (f) finding_end(f);
}

gboolean pad_finding(const char *pad_path)
{
    return findings && g_hash_table_contains(findings, pad_path);
}

/* ------------------------------------------------------------ disconnect */

static void disconnected(GObject *src, GAsyncResult *res, gpointer data)
{
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (v) g_variant_unref(v);
}

static void got_objects(GObject *src, GAsyncResult *res, gpointer data)
{
    char *address = data;
    GDBusConnection *bus = G_DBUS_CONNECTION(src);
    GVariant *v = g_dbus_connection_call_finish(bus, res, NULL);
    if (v) {
        GVariantIter *objects;
        const char *path;
        GVariant *ifaces;
        g_variant_get(v, "(a{oa{sa{sv}}})", &objects);
        while (g_variant_iter_loop(objects, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
            const char *addr = NULL;
            if (dev && g_variant_lookup(dev, "Address", "&s", &addr) && g_ascii_strcasecmp(addr, address) == 0)
                g_dbus_connection_call(bus, "org.bluez", path, "org.bluez.Device1", "Disconnect", NULL, NULL,
                                       G_DBUS_CALL_FLAGS_NONE, 10000, NULL, disconnected, NULL);
            if (dev) g_variant_unref(dev);
        }
        g_variant_iter_free(objects);
        g_variant_unref(v);
    }
    g_free(address);
}

void pad_disconnect(const Pad *pad)
{
    if (pad->connection != PAD_BLUETOOTH || !pad->address) return;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!bus) return;
    g_dbus_connection_call(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                           NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000,
                           NULL, got_objects, g_strdup(pad->address));
    g_object_unref(bus);
}

/* ---------------------------------------------------------------- in use */

typedef struct {
    char *pad_path;
    GPtrArray *nodes;     /* device files to look for */
    PadUsersFn fn;
    gpointer user;
} UsersJob;

static void job_free(gpointer p)
{
    UsersJob *j = p;
    g_free(j->pad_path);
    g_ptr_array_unref(j->nodes);
    g_free(j);
}

static char *read_line(const char *path)
{
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) return NULL;
    return g_strstrip(text);
}

/* A readable name: the app GNOME or Flatpak started it as, or its process
 * name. Only the user's own processes can be seen; nothing is read from the
 * devices themselves. GNOME Shell, which may read a pad's keys, Lares and
 * its helper are left out. */
static char *program_name(const char *pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/comm", pid);
    char *comm = read_line(path);
    if (!comm || g_str_equal(comm, "gnome-shell") || g_str_has_prefix(comm, "lares")) { g_free(comm); return NULL; }
    snprintf(path, sizeof path, "/proc/%s/cgroup", pid);
    char *cgroup = read_line(path);
    char *name = NULL;
    static GRegex *scope;
    if (!scope) scope = g_regex_new("/app-(?:gnome|flatpak)-(.+?)-\\d+\\.scope", 0, 0, NULL);
    GMatchInfo *m;
    if (cgroup && g_regex_match(scope, cgroup, 0, &m)) {
        char *app = g_match_info_fetch(m, 1);
        GString *id = g_string_new(app);
        g_string_replace(id, "\\x2d", "-", 0);
        g_string_append(id, ".desktop");
        GDesktopAppInfo *info = g_desktop_app_info_new(id->str);
        if (info) { name = g_strdup(g_app_info_get_name(G_APP_INFO(info))); g_object_unref(info); }
        g_string_free(id, TRUE);
        g_free(app);
    }
    if (cgroup) g_match_info_free(m);
    g_free(cgroup);
    if (!name) name = comm; else g_free(comm);
    return name;
}

static void users_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    UsersJob *j = data;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    char self[16];
    snprintf(self, sizeof self, "%d", getpid());
    GDir *proc = g_dir_open("/proc", 0, NULL);
    const char *pid;
    while (proc && (pid = g_dir_read_name(proc))) {
        if (!g_ascii_isdigit(pid[0]) || g_str_equal(pid, self)) continue;
        char fd_dir[64];
        snprintf(fd_dir, sizeof fd_dir, "/proc/%s/fd", pid);
        GDir *fds = g_dir_open(fd_dir, 0, NULL);   /* fails for other users' processes */
        if (!fds) continue;
        gboolean holds = FALSE;
        const char *fd;
        while (!holds && (fd = g_dir_read_name(fds))) {
            char link[96], target[256];
            snprintf(link, sizeof link, "%s/%s", fd_dir, fd);
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            for (guint i = 0; i < j->nodes->len && !holds; i++)
                holds = g_str_equal(target, j->nodes->pdata[i]);
        }
        g_dir_close(fds);
        if (!holds) continue;
        char *name = program_name(pid);
        gboolean seen = FALSE;
        for (guint i = 0; name && i < names->len && !seen; i++) seen = g_str_equal(names->pdata[i], name);
        if (name && !seen) g_ptr_array_add(names, name);
        else g_free(name);
    }
    if (proc) g_dir_close(proc);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
    g_task_return_pointer(task, names, (GDestroyNotify)g_ptr_array_unref);
}

static void users_done(GObject *src, GAsyncResult *res, gpointer data)
{
    UsersJob *j = g_task_get_task_data(G_TASK(res));
    GPtrArray *names = g_task_propagate_pointer(G_TASK(res), NULL);
    if (names) {
        j->fn(j->pad_path, names, j->user);
        g_ptr_array_unref(names);
    }
}

void pad_find_users(const Pad *pad, PadUsersFn fn, gpointer user)
{
    UsersJob *j = g_new0(UsersJob, 1);
    j->pad_path = g_strdup(pad->path);
    j->fn = fn;
    j->user = user;
    j->nodes = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(j->nodes, g_strdup(pad->event_node));
    if (pad->js_node) g_ptr_array_add(j->nodes, g_strdup(pad->js_node));
    for (guint i = 0; i < pad->hidraw_nodes->len; i++) g_ptr_array_add(j->nodes, g_strdup(pad->hidraw_nodes->pdata[i]));
    GTask *task = g_task_new(NULL, NULL, users_done, NULL);
    g_task_set_task_data(task, j, job_free);
    g_task_run_in_thread(task, users_thread);
    g_object_unref(task);
}
