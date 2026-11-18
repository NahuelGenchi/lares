#include "remote_model.h"
#include <gio/gunixsocketaddress.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MUTTER       "org.gnome.Mutter.DisplayConfig"
#define MUTTER_PATH  "/org/gnome/Mutter/DisplayConfig"
#define STATE_FMT    "(u@a((ssss)a(siiddada{sv})a{sv})@a(iiduba(ssss)a{sv})@a{sv})"
#define STATE_SIG    "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"
/* Sunshine's user service, as its packages name it. */
static const char *const SUNSHINE_UNITS[] = { "app-dev.lizardbyte.app.Sunshine.service", "sunshine.service", NULL };
#define METHOD_PERSISTENT 2u

#define TAILSCALE_SOCKET "/var/run/tailscale/tailscaled.sock"
#define POLL_SECONDS 3

typedef struct { Notify fn; gpointer user; } Watcher;

struct Remote {
    Config cfg;
    Live live;
    GDBusConnection *bus;
    char *unit_name, *unit_path;   /* Sunshine's systemd unit, found once; NULL without one */
    gint64 reloaded_at;            /* when systemd was last asked to look for new units */
    GArray *watchers;         /* Watcher */
    int wanted;
    guint poll;
    gboolean reading, again;   /* again: a refresh came in during a read */
    gboolean dry, dry_seeded, dry_deny, dry_stream;
};

static const char *ITEM_KEYS[N_ITEMS]  = { "screen", "sunshine", "tailscale" };
static const char *ITEM_NAMES[N_ITEMS] = { "Screen", "Sunshine", "Tailscale" };
/* Home: nothing listening, and the screen at the monitor's own size, filled
 * in at the first reading. Remote: ready to stream; the screen stays as it
 * is until a size that fits the other device is picked. */
static const char *DEFAULTS[N_MODES][N_ITEMS] = {
    { "leave", "off", "off" },
    { "leave", "on",  "on"  },
};

const char *item_name(Item i) { return ITEM_NAMES[i]; }
const char *mode_name(Mode m) { return m == MODE_HOME ? "Home" : "Remote"; }
static const char *mode_key(Mode m) { return m == MODE_HOME ? "home" : "remote"; }
static gboolean is_on(const char *v) { return v && g_str_equal(v, "on"); }
static gboolean is_leave(const char *v) { return !v || g_str_equal(v, "leave"); }

/* Home stops the stream first and drops Tailscale last; Remote does the
 * opposite, and sets the screen before Sunshine starts so it captures the
 * right size from the first frame. */
const Item *apply_order(Mode m, int *n)
{
    static const Item home[]   = { ITEM_SUNSHINE, ITEM_SCREEN, ITEM_TAILSCALE };
    static const Item remote[] = { ITEM_TAILSCALE, ITEM_SCREEN, ITEM_SUNSHINE };
    *n = N_ITEMS;
    return m == MODE_HOME ? home : remote;
}

gboolean parse_screen(const char *s, int *w, int *h, double *rate)
{
    int ww = 0, hh = 0;
    double r = 60;
    if (!s || sscanf(s, "%dx%d@%lf", &ww, &hh, &r) < 2 || ww <= 0 || hh <= 0)
        return FALSE;
    *w = ww; *h = hh; *rate = r;
    return TRUE;
}

static char *describe_screen(int w, int h, double r)
{
    return g_strdup_printf("%d × %d · %d Hz", w, h, (int)lround(r));
}

char *remote_describe(Item i, const char *v)
{
    if (is_leave(v)) return g_strdup("Not changed");
    if (i == ITEM_SCREEN) {
        int w, h; double r;
        return parse_screen(v, &w, &h, &r) ? describe_screen(w, h, r) : g_strdup(v);
    }
    return g_strdup(is_on(v) ? "On" : "Off");
}

char *remote_describe_live(Remote *b, Item i)
{
    Live *l = &b->live;
    if (i == ITEM_SCREEN) {
        if (!l->screen_known) return g_strdup("Reading the screen…");
        char *d = describe_screen(l->w, l->h, l->rate);
        char *s = g_strconcat("Now ", d, NULL);
        g_free(d);
        return s;
    }
    int v = i == ITEM_SUNSHINE ? l->sunshine : l->tailscale;
    return g_strdup(v < 0 ? "Checking…" : v ? "Now on" : "Now off");
}

gboolean remote_matches(Remote *b, Item i, const char *v)
{
    if (is_leave(v)) return TRUE;
    Live *l = &b->live;
    if (i == ITEM_SCREEN) {
        int w, h; double r;
        return l->screen_known && parse_screen(v, &w, &h, &r)
            && l->w == w && l->h == h && fabs(l->rate - r) < 1.0;
    }
    int now = i == ITEM_SUNSHINE ? l->sunshine : l->tailscale;
    return now >= 0 && now == is_on(v);
}

/* ------------------------------------------------------------------ config */

static void config_load(Config *c, gboolean dry)
{
    /* Dry runs keep their own file, so trying the window changes nothing real. */
    c->path = g_build_filename(g_get_user_config_dir(), "lares",
                               dry ? "lares-dry-run.ini" : "lares.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    gboolean have = g_key_file_load_from_file(kf, c->path, G_KEY_FILE_NONE, NULL);
    for (int m = 0; m < N_MODES; m++)
        for (int i = 0; i < N_ITEMS; i++) {
            char *v = have ? g_key_file_get_string(kf, mode_key(m), ITEM_KEYS[i], NULL) : NULL;
            c->values[m][i] = v ? v : g_strdup(DEFAULTS[m][i]);
        }
    char *mode = have ? g_key_file_get_string(kf, "state", "mode", NULL) : NULL;
    c->mode_known = mode != NULL;
    c->mode = mode && g_str_equal(mode, "remote") ? MODE_REMOTE : MODE_HOME;
    c->fresh = !have;
    c->ready = have && g_key_file_get_boolean(kf, "state", "ready", NULL);
    c->sunshine_unit = have ? g_key_file_get_string(kf, "services", "sunshine_unit", NULL) : NULL;
    g_free(mode);
    g_key_file_free(kf);
}

void remote_save(Remote *b)
{
    Config *c = &b->cfg;
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "state", "mode", mode_key(c->mode));
    g_key_file_set_boolean(kf, "state", "ready", c->ready);
    for (int m = 0; m < N_MODES; m++)
        for (int i = 0; i < N_ITEMS; i++)
            g_key_file_set_string(kf, mode_key(m), ITEM_KEYS[i], c->values[m][i]);
    if (c->sunshine_unit) g_key_file_set_string(kf, "services", "sunshine_unit", c->sunshine_unit);
    char *dir = g_path_get_dirname(c->path);
    g_mkdir_with_parents(dir, 0700);
    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, c->path, &e)) {
        g_warning("Couldn't save %s: %s", c->path, e->message);
        g_error_free(e);
    }
    g_free(dir);
    g_key_file_free(kf);
}

/* --------------------------------------------------------------- commands */

typedef void (*CmdDone)(int status, const char *out, const char *err, gpointer user);
typedef struct { CmdDone done; gpointer user; } CmdCtx;

static void cmd_finished(GObject *src, GAsyncResult *res, gpointer data)
{
    CmdCtx *c = data;
    GSubprocess *p = G_SUBPROCESS(src);
    char *out = NULL, *err = NULL;
    GError *e = NULL;
    int status = -1;
    if (g_subprocess_communicate_utf8_finish(p, res, &out, &err, &e)) {
        if (g_subprocess_get_if_exited(p)) status = g_subprocess_get_exit_status(p);
    } else {
        g_free(err);
        err = g_strdup(e->message);
        g_error_free(e);
    }
    c->done(status, out ? out : "", err ? err : "", c->user);
    g_free(out);
    g_free(err);
    g_free(c);
    g_object_unref(p);
}

static void run_cmd(const char *const *argv, CmdDone done, gpointer user)
{
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
    if (!p) {
        done(-1, "", e->message, user);
        g_error_free(e);
        return;
    }
    CmdCtx *c = g_new0(CmdCtx, 1);
    c->done = done;
    c->user = user;
    g_subprocess_communicate_utf8_async(p, NULL, NULL, cmd_finished, c);
}

static char *first_line(const char *text)
{
    while (text && (*text == '\n' || *text == ' ')) text++;
    if (!text || !*text) return g_strdup("It stopped without saying why");
    const char *end = strchr(text, '\n');
    return end ? g_strndup(text, end - text) : g_strdup(text);
}

/* ----------------------------------------------------------------- screen */

static void screen_mode_free(gpointer p)
{
    ScreenMode *m = p;
    g_free(m->id);
    g_free(m->scales);
    g_free(m);
}

/* The connector of the primary logical monitor, or of the first one. */
static char *main_connector(GVariant *logical)
{
    char *first = NULL, *primary = NULL;
    GVariantIter it;
    GVariant *lm;
    g_variant_iter_init(&it, logical);
    while ((lm = g_variant_iter_next_value(&it))) {
        gint32 x, y; double scale; guint32 tr; gboolean prim;
        GVariant *specs, *props;
        g_variant_get(lm, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &tr, &prim, &specs, &props);
        if (g_variant_n_children(specs) > 0) {
            const char *conn;
            g_variant_get_child(specs, 0, "(&ssss)", &conn, NULL, NULL, NULL);
            if (!first) first = g_strdup(conn);
            if (prim && !primary) primary = g_strdup(conn);
        }
        g_variant_unref(specs);
        g_variant_unref(props);
        g_variant_unref(lm);
    }
    if (primary) { g_free(first); return primary; }
    return first;
}

static GVariant *modes_of(GVariant *monitors, const char *conn)
{
    GVariantIter it;
    GVariant *mon, *found = NULL;
    g_variant_iter_init(&it, monitors);
    while (!found && (mon = g_variant_iter_next_value(&it))) {
        GVariant *spec = g_variant_get_child_value(mon, 0);
        const char *c;
        g_variant_get(spec, "(&ssss)", &c, NULL, NULL, NULL);
        if (g_str_equal(c, conn)) found = g_variant_get_child_value(mon, 1);
        g_variant_unref(spec);
        g_variant_unref(mon);
    }
    return found;
}

static GPtrArray *parse_modes(GVariant *modes)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(screen_mode_free);
    GVariantIter it;
    GVariant *mv;
    g_variant_iter_init(&it, modes);
    while ((mv = g_variant_iter_next_value(&it))) {
        const char *id; gint32 w, h; double rate, pref;
        GVariant *scales, *props;
        g_variant_get(mv, "(&siidd@ad@a{sv})", &id, &w, &h, &rate, &pref, &scales, &props);
        ScreenMode *m = g_new0(ScreenMode, 1);
        m->id = g_strdup(id);
        m->w = w; m->h = h; m->rate = rate;
        gsize n = 0;
        const double *s = g_variant_get_fixed_array(scales, &n, sizeof(double));
        m->scales = g_memdup2(s, n * sizeof(double));
        m->n_scales = n;
        g_variant_lookup(props, "is-current", "b", &m->current);
        g_variant_lookup(props, "is-preferred", "b", &m->preferred);
        g_ptr_array_add(arr, m);
        g_variant_unref(scales);
        g_variant_unref(props);
        g_variant_unref(mv);
    }
    return arr;
}

static char *current_mode_id(GVariant *monitors, const char *conn)
{
    GVariant *modes = modes_of(monitors, conn);
    if (!modes) return NULL;
    GPtrArray *arr = parse_modes(modes);
    char *id = NULL;
    for (guint i = 0; i < arr->len && !id; i++) {
        ScreenMode *m = arr->pdata[i];
        if (m->current) id = g_strdup(m->id);
    }
    g_ptr_array_unref(arr);
    g_variant_unref(modes);
    return id;
}

static void parse_state(Remote *b, GVariant *state)
{
    Live *l = &b->live;
    guint32 serial;
    GVariant *monitors, *logical, *props;
    g_variant_get(state, STATE_FMT, &serial, &monitors, &logical, &props);

    g_clear_pointer(&l->connector, g_free);
    g_clear_pointer(&l->modes, g_ptr_array_unref);
    l->screen_known = FALSE;
    l->connector = main_connector(logical);
    GVariant *modes = l->connector ? modes_of(monitors, l->connector) : NULL;
    if (modes) {
        l->modes = parse_modes(modes);
        for (guint i = 0; i < l->modes->len; i++) {
            ScreenMode *m = l->modes->pdata[i];
            if (m->current) {
                l->w = m->w; l->h = m->h; l->rate = m->rate;
                l->screen_known = TRUE;
            }
        }
        g_variant_unref(modes);
    }
    g_variant_unref(monitors);
    g_variant_unref(logical);
    g_variant_unref(props);
}

typedef struct {
    Remote *b;
    char *spec;
    Item item;
    StepDone done;
    gpointer user;
} StepCtx;

static StepCtx *step_new(Remote *b, Item item, const char *v, StepDone done, gpointer user)
{
    StepCtx *c = g_new0(StepCtx, 1);
    c->b = b; c->item = item; c->spec = g_strdup(v); c->done = done; c->user = user;
    return c;
}

static void step_finish(StepCtx *c, gboolean ok, gboolean perm, const char *msg)
{
    c->done(ok, perm, msg, c->user);
    g_free(c->spec);
    g_free(c);
}

static void screen_applied(GObject *src, GAsyncResult *res, gpointer data)
{
    StepCtx *c = data;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (r) {
        g_variant_unref(r);
        step_finish(c, TRUE, FALSE, NULL);
    } else {
        g_dbus_error_strip_remote_error(e);
        step_finish(c, FALSE, FALSE, e->message);
        g_error_free(e);
    }
}

static gboolean has_scale(ScreenMode *m, double s)
{
    for (gsize i = 0; i < m->n_scales; i++)
        if (fabs(m->scales[i] - s) < 0.001) return TRUE;
    return FALSE;
}

static void screen_got_state(GObject *src, GAsyncResult *res, gpointer data)
{
    StepCtx *c = data;
    GError *e = NULL;
    GVariant *state = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!state) {
        g_dbus_error_strip_remote_error(e);
        step_finish(c, FALSE, FALSE, e->message);
        g_error_free(e);
        return;
    }
    Remote *b = c->b;
    parse_state(b, state);

    int w, h; double rate;
    ScreenMode *best = NULL;
    if (parse_screen(c->spec, &w, &h, &rate) && b->live.modes)
        for (guint i = 0; i < b->live.modes->len; i++) {
            ScreenMode *m = b->live.modes->pdata[i];
            if (m->w != w || m->h != h) continue;
            double d = fabs(m->rate - rate), bd = best ? fabs(best->rate - rate) : 1e9;
            if (d < bd - 0.01 || (fabs(d - bd) <= 0.01 && m->preferred)) best = m;
        }
    if (!best) {
        char *msg = g_strdup_printf("This monitor has no %d × %d mode", w, h);
        step_finish(c, FALSE, FALSE, msg);
        g_free(msg);
        g_variant_unref(state);
        return;
    }
    if (best->current) {
        step_finish(c, TRUE, FALSE, NULL);
        g_variant_unref(state);
        return;
    }

    guint32 serial;
    GVariant *monitors, *logical, *props;
    g_variant_get(state, STATE_FMT, &serial, &monitors, &logical, &props);

    GVariantBuilder lb;
    g_variant_builder_init(&lb, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    GVariantIter it;
    GVariant *lm;
    g_variant_iter_init(&it, logical);
    while ((lm = g_variant_iter_next_value(&it))) {
        gint32 x, y; double scale; guint32 tr; gboolean prim;
        GVariant *specs, *lprops;
        g_variant_get(lm, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &tr, &prim, &specs, &lprops);

        GVariantBuilder sb;
        g_variant_builder_init(&sb, G_VARIANT_TYPE("a(ssa{sv})"));
        gboolean has_target = FALSE;
        int added = 0;
        GVariantIter si;
        GVariant *spec;
        g_variant_iter_init(&si, specs);
        while ((spec = g_variant_iter_next_value(&si))) {
            const char *conn;
            g_variant_get(spec, "(&ssss)", &conn, NULL, NULL, NULL);
            char *mode_id = g_str_equal(conn, b->live.connector)
                ? (has_target = TRUE, g_strdup(best->id))
                : current_mode_id(monitors, conn);
            if (mode_id) {
                g_variant_builder_add(&sb, "(ssa{sv})", conn, mode_id, NULL);
                added++;
            }
            g_free(mode_id);
            g_variant_unref(spec);
        }
        if (has_target && !has_scale(best, scale))
            scale = has_scale(best, 1.0) || best->n_scales == 0 ? 1.0 : best->scales[0];
        if (added)
            g_variant_builder_add(&lb, "(iiduba(ssa{sv}))", x, y, scale, tr, prim, &sb);
        else
            g_variant_builder_clear(&sb);
        g_variant_unref(specs);
        g_variant_unref(lprops);
        g_variant_unref(lm);
    }

    g_dbus_connection_call(b->bus, MUTTER, MUTTER_PATH, MUTTER, "ApplyMonitorsConfig",
        g_variant_new("(uua(iiduba(ssa{sv}))a{sv})", serial, METHOD_PERSISTENT, &lb, NULL),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, screen_applied, c);

    g_variant_unref(monitors);
    g_variant_unref(logical);
    g_variant_unref(props);
    g_variant_unref(state);
}

/* --------------------------------------------------------------- services */

static gboolean needs_permission(const char *err)
{
    return strstr(err, "ccess denied") || strstr(err, "ermission denied")
        || strstr(err, "sudo tailscale");
}

static void service_done(int status, const char *out, const char *err, gpointer data)
{
    StepCtx *c = data;
    if (status == 0) {
        step_finish(c, TRUE, FALSE, NULL);
    } else if (c->item == ITEM_TAILSCALE && status == 124) {
        step_finish(c, FALSE, FALSE, "Tailscale didn't connect in 30 seconds. It may need you to sign in again");
    } else {
        char *msg = first_line(*err ? err : out);
        step_finish(c, FALSE, c->item == ITEM_TAILSCALE && needs_permission(err), msg);
        g_free(msg);
    }
}

/* -------------------------------------------------------------- dry runs */

static gboolean dry_fire(gpointer data)
{
    StepCtx *c = data;
    Live *l = &c->b->live;
    if (c->item == ITEM_TAILSCALE && c->b->dry_deny) {
        step_finish(c, FALSE, TRUE, "Access denied: prefs write access denied");
        return G_SOURCE_REMOVE;
    }
    if (c->item == ITEM_SCREEN) {
        int w, h; double r;
        if (parse_screen(c->spec, &w, &h, &r)) { l->w = w; l->h = h; l->rate = r; l->screen_known = TRUE; }
    } else if (c->item == ITEM_SUNSHINE) {
        l->sunshine = is_on(c->spec);
        l->streaming = l->sunshine && c->b->dry_stream;
    } else {
        l->tailscale = is_on(c->spec);
    }
    step_finish(c, TRUE, FALSE, NULL);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ apply */

void remote_apply(Remote *b, Item i, const char *v, StepDone done, gpointer user)
{
    if (is_leave(v)) { done(TRUE, FALSE, NULL, user); return; }
    StepCtx *c = step_new(b, i, v, done, user);
    if (b->dry) {
        g_timeout_add(i == ITEM_SUNSHINE && is_on(v) ? 1500 : 750, dry_fire, c);
        return;
    }
    if (i == ITEM_SCREEN) {
        g_dbus_connection_call(b->bus, MUTTER, MUTTER_PATH, MUTTER, "GetCurrentState", NULL,
            G_VARIANT_TYPE(STATE_SIG), G_DBUS_CALL_FLAGS_NONE, -1, NULL, screen_got_state, c);
    } else if (i == ITEM_SUNSHINE) {
        if (!b->unit_name) { step_finish(c, FALSE, FALSE, "Sunshine isn't installed"); return; }
        /* enable/disable too, so the mode survives a restart */
        const char *argv[] = { "systemctl", "--user", is_on(v) ? "enable" : "disable", "--now",
                               b->unit_name, NULL };
        run_cmd(argv, service_done, c);
    } else {
        const char *argv[] = { "timeout", "30", "tailscale", is_on(v) ? "up" : "down", NULL };
        run_cmd(argv, service_done, c);
    }
}

void remote_allow_tailscale(Remote *b, StepDone done, gpointer user)
{
    StepCtx *c = step_new(b, ITEM_TAILSCALE, "on", done, user);
    if (b->dry) {
        b->dry_deny = FALSE;
        g_timeout_add(600, dry_fire, c);
        return;
    }
    char *op = g_strdup_printf("--operator=%s", g_get_user_name());
    const char *argv[] = { "pkexec", "tailscale", "set", op, NULL };
    run_cmd(argv, service_done, c);
    g_free(op);
}


/* ------------------------------------------------------------- live state */

/* A read asks three things at once, all without starting a program: Mutter
 * for the screen, systemd for Sunshine and Tailscale's own socket for its
 * state. Running `tailscale status` every few seconds would cost a 30 MB
 * process each time. */
typedef struct {
    Remote *r;
    int left;
    Notify done;
    gpointer user;
} LiveCtx;

static void notify_watchers(Remote *r)
{
    for (guint i = 0; i < r->watchers->len; i++) {
        Watcher *w = &g_array_index(r->watchers, Watcher, i);
        w->fn(w->user);
    }
}

static void live_part(LiveCtx *c)
{
    if (--c->left > 0) return;
    Remote *r = c->r;
    r->reading = FALSE;
    if (r->dry) r->dry_seeded = TRUE;
    Live *live = &r->live;
    if (r->cfg.fresh && live->screen_known && live->modes) {
        /* First run: Home puts the monitor back to its own size. */
        for (guint i = 0; i < live->modes->len; i++) {
            ScreenMode *m = live->modes->pdata[i];
            if (!m->preferred) continue;
            g_free(r->cfg.values[MODE_HOME][ITEM_SCREEN]);
            r->cfg.values[MODE_HOME][ITEM_SCREEN] = g_strdup_printf("%dx%d@%d", m->w, m->h, (int)lround(m->rate));
            break;
        }
        r->cfg.fresh = FALSE;
        remote_save(r);
    }
    if (!r->cfg.ready && remote_setup_done(r)) {
        r->cfg.ready = TRUE;
        remote_save(r);
    }
    if (!r->cfg.mode_known) {
        /* First run: the mode is whichever one the PC already looks like. */
        Live *l = &r->live;
        r->cfg.mode = l->sunshine == 1 && remote_matches(r, ITEM_SCREEN, r->cfg.values[MODE_REMOTE][ITEM_SCREEN])
                    ? MODE_REMOTE : MODE_HOME;
        r->cfg.mode_known = TRUE;
        remote_save(r);
    }
    if (c->done) c->done(c->user);
    notify_watchers(r);
    g_free(c);
    if (r->again) {
        r->again = FALSE;
        remote_read_live(r, NULL, NULL);
    }
}

static gboolean sunshine_streaming(void)
{
    char *path = g_build_filename(g_get_user_config_dir(), "sunshine", "sunshine.log", NULL);
    FILE *f = fopen(path, "rb");
    g_free(path);
    if (!f) return FALSE;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = size > 262144 ? size - 262144 : 0;
    fseek(f, start, SEEK_SET);
    char *buf = g_malloc(size - start + 1);
    size_t n = fread(buf, 1, size - start, f);
    buf[n] = 0;
    fclose(f);
    const char *on = g_strrstr(buf, "CLIENT CONNECTED");
    const char *off = g_strrstr(buf, "CLIENT DISCONNECTED");
    gboolean live = on && (!off || on > off);
    g_free(buf);
    return live;
}

/* Whether a Moonlight device is paired. Sunshine's state file also keeps its
 * own login, so only whether the device list is empty is looked at; nothing
 * from the file is kept, shown or logged, and the copy read is wiped. */
static gboolean sunshine_paired(void)
{
    char *path = g_build_filename(g_get_user_config_dir(), "sunshine", "sunshine_state.json", NULL);
    char *text = NULL;
    gsize len = 0;
    gboolean paired = FALSE;
    if (g_file_get_contents(path, &text, &len, NULL)) {
        const char *p = strstr(text, "\"named_devices\"");
        if (p && (p = strchr(p, '['))) {
            p++;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
            paired = *p && *p != ']';
        }
        memset(text, 0, len);
        g_free(text);
    }
    g_free(path);
    return paired;
}

static void sunshine_state(GObject *src, GAsyncResult *res, gpointer data)
{
    LiveCtx *c = data;
    Live *l = &c->r->live;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    l->sunshine = 0;
    if (v) {
        GVariant *inner;
        g_variant_get(v, "(v)", &inner);
        const char *state = g_variant_get_string(inner, NULL);
        l->sunshine = g_str_equal(state, "active") || g_str_equal(state, "activating")
                   || g_str_equal(state, "reloading");
        g_variant_unref(inner);
        g_variant_unref(v);
    }
    l->streaming = l->sunshine && (sunshine_streaming() || c->r->dry_stream);
    l->paired = sunshine_paired();
    live_part(c);
}

/* Sunshine's packages name its user service differently; the first one
 * systemd knows is it. Asked again on each reading until one turns up, so
 * installing Sunshine with Lares open is noticed. */
static void find_sunshine(Remote *r)
{
    const char *names[G_N_ELEMENTS(SUNSHINE_UNITS) + 1];
    int n = 0;
    if (r->cfg.sunshine_unit) names[n++] = r->cfg.sunshine_unit;
    for (int i = 0; SUNSHINE_UNITS[i]; i++) names[n++] = SUNSHINE_UNITS[i];
    for (int i = 0; i < n && !r->unit_path; i++) {
        GVariant *v = g_dbus_connection_call_sync(r->bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "LoadUnit", g_variant_new("(s)", names[i]),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
        if (!v) continue;
        char *path;
        g_variant_get(v, "(o)", &path);
        g_variant_unref(v);
        GVariant *st = g_dbus_connection_call_sync(r->bus, "org.freedesktop.systemd1", path,
            "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.freedesktop.systemd1.Unit", "LoadState"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
        gboolean loaded = FALSE;
        if (st) {
            GVariant *inner;
            g_variant_get(st, "(v)", &inner);
            loaded = g_str_equal(g_variant_get_string(inner, NULL), "loaded");
            g_variant_unref(inner);
            g_variant_unref(st);
        }
        if (loaded) { r->unit_path = path; r->unit_name = g_strdup(names[i]); }
        else g_free(path);
    }
    r->live.sunshine_installed = r->unit_path != NULL;
}

static void read_sunshine(LiveCtx *c)
{
    if (!c->r->unit_path) find_sunshine(c->r);
    /* systemd remembers a unit it didn't find, so a Sunshine installed since
     * is seen only after it looks again: at most once a minute, and only
     * while something reads (the guide is open). */
    gint64 now = g_get_monotonic_time();
    if (!c->r->unit_path && now - c->r->reloaded_at > 60 * G_USEC_PER_SEC) {
        c->r->reloaded_at = now;
        g_dbus_connection_call(c->r->bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "Reload", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    if (!c->r->unit_path) {
        c->r->live.sunshine = 0;
        c->r->live.streaming = FALSE;
        c->r->live.paired = sunshine_paired();
        live_part(c);
        return;
    }
    g_dbus_connection_call(c->r->bus, "org.freedesktop.systemd1", c->r->unit_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.systemd1.Unit", "ActiveState"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, sunshine_state, c);
}

/* Tailscale's local API answers plain HTTP on its socket. */
typedef struct {
    LiveCtx *live;
    GSocketConnection *conn;
    GString *body;
    char chunk[4096];
} TsCtx;

/* The string value of "key" at or after `from` in a JSON text. Enough for the
 * few plain fields read from Tailscale's status. */
static char *json_string(const char *from, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = from ? strstr(from, pattern) : NULL;
    if (!p || !(p = strchr(p + strlen(pattern), ':'))) return NULL;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p != '"') return NULL;
    const char *end = strchr(p + 1, '"');
    return end ? g_strndup(p + 1, end - p - 1) : NULL;
}

/* This PC's first IPv4 address on the tailnet. */
static char *tailnet_ip(const char *self)
{
    const char *p = self ? strstr(self, "\"TailscaleIPs\"") : NULL;
    const char *end = p ? strchr(p, ']') : NULL;
    for (p = p ? strchr(p, '[') : NULL; p && end && p < end; ) {
        const char *open = strchr(p, '"');
        if (!open || open > end) break;
        const char *close = strchr(open + 1, '"');
        if (!close) break;
        if (memchr(open + 1, '.', close - open - 1)) return g_strndup(open + 1, close - open - 1);
        p = close + 1;
    }
    return NULL;
}

static void ts_finish(TsCtx *t)
{
    Live *l = &t->live->r->live;
    const char *json = t->body->str;
    char *state = json_string(json, "BackendState");
    l->tailscale_installed = state || g_file_test(TAILSCALE_SOCKET, G_FILE_TEST_EXISTS);
    if (!l->tailscale_installed) {
        char *program = g_find_program_in_path("tailscale");
        l->tailscale_installed = program != NULL;
        g_free(program);
    }
    l->tailscale = state && g_str_equal(state, "Running");
    l->tailscale_signed_in = state && (g_str_equal(state, "Running") || g_str_equal(state, "Stopped")
                                       || g_str_equal(state, "Starting"));
    const char *self = strstr(json, "\"Self\"");
    g_free(l->tailscale_host);
    g_free(l->tailscale_ip);
    l->tailscale_host = l->tailscale_signed_in ? json_string(self, "HostName") : NULL;
    l->tailscale_ip = l->tailscale_signed_in ? tailnet_ip(self) : NULL;
    g_free(state);
    if (t->conn) g_object_unref(t->conn);
    g_string_free(t->body, TRUE);
    LiveCtx *c = t->live;
    g_free(t);
    live_part(c);
}

static void ts_read(GObject *src, GAsyncResult *res, gpointer data)
{
    TsCtx *t = data;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, NULL);
    if (n > 0 && t->body->len < 262144) {
        g_string_append_len(t->body, t->chunk, n);
        g_input_stream_read_async(G_INPUT_STREAM(src), t->chunk, sizeof t->chunk,
                                  G_PRIORITY_DEFAULT, NULL, ts_read, t);
        return;
    }
    ts_finish(t);
}

static void ts_written(GObject *src, GAsyncResult *res, gpointer data)
{
    TsCtx *t = data;
    if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res, NULL, NULL)) {
        ts_finish(t);
        return;
    }
    g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(t->conn)), t->chunk,
                              sizeof t->chunk, G_PRIORITY_DEFAULT, NULL, ts_read, t);
}

static void ts_connected(GObject *src, GAsyncResult *res, gpointer data)
{
    TsCtx *t = data;
    t->conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, NULL);
    g_object_unref(src);
    if (!t->conn) {
        ts_finish(t);
        return;
    }
    static const char request[] =
        "GET /localapi/v0/status?peers=false HTTP/1.0\r\nHost: local-tailscaled.sock\r\n\r\n";
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(t->conn)), request,
                                    sizeof request - 1, G_PRIORITY_DEFAULT, NULL, ts_written, t);
}

static void read_tailscale(LiveCtx *c)
{
    TsCtx *t = g_new0(TsCtx, 1);
    t->live = c;
    t->body = g_string_new(NULL);
    GSocketAddress *addr = g_unix_socket_address_new(TAILSCALE_SOCKET);
    GSocketClient *client = g_socket_client_new();
    /* A local socket needs no proxy; asking for one would load GIO's network
     * modules, TLS included, for nothing. */
    g_socket_client_set_enable_proxy(client, FALSE);
    g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(addr), NULL, ts_connected, t);
    g_object_unref(addr);
}

static void screen_read(GObject *src, GAsyncResult *res, gpointer data)
{
    LiveCtx *c = data;
    GVariant *state = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (state) {
        parse_state(c->r, state);
        g_variant_unref(state);
    }
    live_part(c);
}

static void read_screen(LiveCtx *c)
{
    g_dbus_connection_call(c->r->bus, MUTTER, MUTTER_PATH, MUTTER, "GetCurrentState", NULL,
        G_VARIANT_TYPE(STATE_SIG), G_DBUS_CALL_FLAGS_NONE, -1, NULL, screen_read, c);
}

static gboolean live_later(gpointer data)
{
    LiveCtx *c = data;
    c->left = 1;
    live_part(c);
    return G_SOURCE_REMOVE;
}

void remote_read_live(Remote *r, Notify done, gpointer user)
{
    LiveCtx *c = g_new0(LiveCtx, 1);
    c->r = r; c->done = done; c->user = user;
    r->reading = TRUE;
    if (r->dry && r->dry_seeded) { /* dry runs keep their own pretend state */
        g_idle_add(live_later, c);
        return;
    }
    c->left = 3;
    read_sunshine(c);
    read_tailscale(c);
    read_screen(c);
}

/* A read that started before a change may miss it, so a refresh during a
 * read queues one more. */
void remote_refresh(Remote *r)
{
    if (r->reading) r->again = TRUE;
    else remote_read_live(r, NULL, NULL);
}

static gboolean poll_live(gpointer data)
{
    remote_refresh(data);
    return G_SOURCE_CONTINUE;
}

void remote_want(Remote *r, gboolean on)
{
    r->wanted += on ? 1 : -1;
    if (r->wanted > 0 && !r->poll) {
        remote_refresh(r);
        r->poll = g_timeout_add_seconds(POLL_SECONDS, poll_live, r);
    } else if (r->wanted <= 0 && r->poll) {
        g_source_remove(r->poll);
        r->poll = 0;
        r->wanted = 0;
    }
}

void remote_watch(Remote *r, Notify fn, gpointer user)
{
    Watcher w = { fn, user };
    g_array_append_val(r->watchers, w);
}

static void screen_changed_read(GObject *src, GAsyncResult *res, gpointer data)
{
    Remote *r = data;
    GVariant *state = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!state) return;
    parse_state(r, state);
    g_variant_unref(state);
    notify_watchers(r);
}

static void monitors_changed(GDBusConnection *bus, const char *sender, const char *path,
                             const char *iface, const char *signal, GVariant *params, gpointer data)
{
    Remote *r = data;
    if (r->dry || !r->watchers->len) return;
    g_dbus_connection_call(r->bus, MUTTER, MUTTER_PATH, MUTTER, "GetCurrentState", NULL,
        G_VARIANT_TYPE(STATE_SIG), G_DBUS_CALL_FLAGS_NONE, -1, NULL, screen_changed_read, r);
}

/* ----------------------------------------------------------------- set up */

gboolean remote_setup_done(Remote *r)
{
    Live *l = &r->live;
    return l->tailscale_signed_in && l->sunshine_installed && l->paired;
}

Remote *remote_new(void)
{
    Remote *r = g_new0(Remote, 1);
    GError *e = NULL;
    r->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (!r->bus) g_error("No session bus: %s", e->message);
    r->dry = g_getenv("LARES_DRY_RUN") != NULL;
    r->dry_deny = r->dry && g_getenv("LARES_DRY_DENY") != NULL;
    r->dry_stream = r->dry && g_getenv("LARES_DRY_STREAM") != NULL;
    r->live.sunshine = r->live.tailscale = -1;
    r->watchers = g_array_new(FALSE, FALSE, sizeof(Watcher));
    config_load(&r->cfg, r->dry);
    find_sunshine(r);
    r->reloaded_at = g_get_monotonic_time();   /* systemd's view is fresh at start */
    g_dbus_connection_signal_subscribe(r->bus, MUTTER, MUTTER, "MonitorsChanged", MUTTER_PATH,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, monitors_changed, r, NULL);
    return r;
}

Config *remote_config(Remote *r) { return &r->cfg; }
Live *remote_live(Remote *r) { return &r->live; }
