/* The Remote section: the Home | Remote switch and what each mode sets. */
#include <math.h>
#include <string.h>
#include "lares.h"

#define STEP_GAP_MS 140   /* a breath between steps, so each row's change reads */

typedef enum { RS_QUEUED, RS_BUSY, RS_OK, RS_DRIFT, RS_ERROR, RS_LEAVE } RowState;

typedef enum { NOTE_NONE, NOTE_CONFIRM, NOTE_PERMISSION, NOTE_ERROR, NOTE_DRIFT } Note;

typedef struct RemoteView RemoteView;

typedef struct {
    RemoteView *app;
    Item item;
    GtkWidget *box, *head, *value, *hint, *hint_rev, *chevron, *revealer, *chips;
    GPtrArray *toggles;
    char *chips_key;
    RowState state;
    char *error;
    gboolean perm, syncing;
} Row;

/* One step of the setup guide: a number that becomes a tick, what the app
 * does, where it stands, and what to press next. */
typedef struct {
    GtkWidget *mark, *status, *actions;
} Step;

enum { STEP_TAILSCALE, STEP_SUNSHINE, STEP_MOONLIGHT, N_STEPS };

struct RemoteView {
    Lares *app;
    Remote *m;
    GtkWidget *page, *mode_box, *guide, *guide_address, *guide_ip, *guide_or, *guide_host;
    Step steps[N_STEPS];
    char *guide_key;          /* what the guide shows; it's redrawn only when this changes */
    gboolean signing_in;
    GtkWidget *sw, *moonlight, *ip, *host;
    GtkWidget *note_rev, *note_icons, *note_text, *note_main, *note_alt;
    Row rows[N_ITEMS];
    gboolean ready, applying, restart;
    Item single[1];
    const Item *order;
    int n, step;
    Note note;
    Row *open;
};

static void start_apply(RemoteView *a);
static void on_live(gpointer user);

static Config *cfg(RemoteView *a) { return remote_config(a->m); }
static const char *want(RemoteView *a, Item i) { Config *c = cfg(a); return c->values[c->mode][i]; }
static gboolean leave(const char *v) { return !v || g_str_equal(v, "leave"); }

static const char *busy_text(Item i, const char *v)
{
    gboolean on = g_str_equal(v, "on");
    switch (i) {
    case ITEM_SCREEN:   return "Changing the resolution…";
    case ITEM_SUNSHINE: return on ? "Starting Sunshine…" : "Stopping Sunshine…";
    default:            return on ? "Connecting…" : "Disconnecting…";
    }
}

/* -------------------------------------------------------------------- rows */

static void sync_chips(Row *r)
{
    const char *v = want(r->app, r->item);
    r->syncing = TRUE;
    for (guint i = 0; i < r->toggles->len; i++) {
        GtkToggleButton *t = r->toggles->pdata[i];
        gtk_toggle_button_set_active(t, g_str_equal(g_object_get_data(G_OBJECT(t), "value"), v));
    }
    r->syncing = FALSE;
}

static void update_row(Row *r)
{
    RemoteView *a = r->app;
    const char *v = want(a, r->item);
    /* While it works, the row says so in its value's place and keeps its
     * height; drift and errors, which stay, get a line under the name. */
    gboolean busy = r->state == RS_BUSY;
    char *d = busy ? g_strdup(busy_text(r->item, v)) : remote_describe(r->item, v);
    gtk_label_set_text(GTK_LABEL(r->value), d);
    g_free(d);
    ui_class(r->value, "dim", busy || leave(v));

    char *hint = NULL;
    if (r->state == RS_DRIFT) hint = remote_describe_live(a->m, r->item);
    /* A permission error's own text is Tailscale's; the note explains it. */
    else if (r->state == RS_ERROR) hint = g_strdup(r->perm ? "Needs your password" : r->error);
    if (hint) {
        gtk_label_set_text(GTK_LABEL(r->hint), hint);
        ui_class(r->hint, "warn", r->state == RS_DRIFT);
        ui_class(r->hint, "bad", r->state == RS_ERROR);
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(r->hint_rev), hint != NULL);
    g_free(hint);
    sync_chips(r);
}

static void settle_rows(RemoteView *a)
{
    for (int i = 0; i < N_ITEMS; i++) {
        Row *r = &a->rows[i];
        const char *v = want(a, i);
        if (r->state != RS_BUSY && r->state != RS_QUEUED) {
            if (leave(v)) {
                r->state = RS_LEAVE;
            } else if (remote_matches(a->m, i, v)) {
                r->state = RS_OK;
                g_clear_pointer(&r->error, g_free);
                r->perm = FALSE;
            } else if (r->state != RS_ERROR) {
                r->state = RS_DRIFT;
            }
        }
        update_row(r);
    }
}

/* ------------------------------------------------------ connection & note */

/* A label's text is set only when it changes, so a selection made to copy an
 * address survives the next reading. */
static void set_field(GtkWidget *label, const char *text, gboolean known)
{
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0)
        gtk_label_set_text(GTK_LABEL(label), text);
    ui_class(label, "dim", !known);
}

/* The card under the switch: whether Moonlight is connected, and the two
 * ways to add this PC in it. */
static void update_connection(RemoteView *a)
{
    Live *l = remote_live(a->m);
    gboolean on = l->sunshine == 1, live = on && l->streaming;
    const char *moon = l->sunshine < 0 ? "Unknown"
                     : !on ? "Off"
                     : live ? "live"
                     : l->paired ? "Ready" : "No device paired";
    set_field(a->moonlight, moon, live || (on && l->paired));
    ui_class(a->moonlight, "live", live); /* a badge that breathes */
    set_field(a->ip, l->tailscale_ip ? l->tailscale_ip : "Unknown", l->tailscale_ip != NULL);
    set_field(a->host, l->tailscale_host ? l->tailscale_host : "Unknown", l->tailscale_host != NULL);
}

static void update_note(RemoteView *a)
{
    Note n = a->note == NOTE_CONFIRM ? NOTE_CONFIRM : NOTE_NONE;
    Row *bad = NULL;
    if (n == NOTE_NONE) {
        for (int i = 0; i < N_ITEMS; i++)
            if (a->rows[i].state == RS_ERROR && (!bad || a->rows[i].perm)) bad = &a->rows[i];
        if (bad) n = bad->perm ? NOTE_PERMISSION : NOTE_ERROR;
        else if (!a->applying)
            for (int i = 0; i < N_ITEMS; i++)
                if (a->rows[i].state == RS_DRIFT) n = NOTE_DRIFT;
    }
    a->note = n;

    char *text = NULL;
    const char *main = NULL, *alt = NULL, *art = "warn";
    /* Each note says only what the line above and the rows don't. */
    switch (n) {
    case NOTE_CONFIRM: {
        gboolean ts_off = g_str_equal(cfg(a)->values[MODE_HOME][ITEM_TAILSCALE], "off");
        text = g_strdup(ts_off
            ? "Home ends the stream and turns off Tailscale, so this PC can't be reached from away."
            : "Home ends the stream.");
        main = "Switch to Home";
        alt = "Stay on Remote";
        break;
    }
    case NOTE_PERMISSION:
        text = g_strdup("Tailscale asks for your password once, so Lares can switch it from now on.");
        main = "Allow";
        art = "key";
        break;
    case NOTE_ERROR:
        text = g_strdup_printf("%s didn't change.", item_name(bad->item));
        main = "Try again";
        break;
    case NOTE_DRIFT:
        text = g_strdup("Changed outside Lares.");
        main = "Set it again";
        break;
    case NOTE_NONE:
        break;
    }
    if (text) {
        gtk_label_set_text(GTK_LABEL(a->note_text), text);
        gtk_stack_set_visible_child_name(GTK_STACK(a->note_icons), art);
        gtk_button_set_label(GTK_BUTTON(a->note_main), main);
        gtk_widget_set_visible(a->note_alt, alt != NULL);
        if (alt) gtk_button_set_label(GTK_BUTTON(a->note_alt), alt);
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->note_rev), text != NULL);
    g_free(text);
}

/* ----------------------------------------------------------------- applying */

static void run_step(RemoteView *a);

static gboolean run_step_later(gpointer data)
{
    run_step(data);
    return G_SOURCE_REMOVE;
}

static void step_done(gboolean ok, gboolean perm, const char *msg, gpointer user)
{
    RemoteView *a = user;
    Row *r = &a->rows[a->order[a->step]];
    if (ok) {
        r->state = RS_OK;
    } else {
        r->state = RS_ERROR;
        g_free(r->error);
        r->error = g_strdup(msg);
        r->perm = perm;
    }
    update_row(r);
    update_note(a);
    a->step++;
    g_timeout_add(STEP_GAP_MS, run_step_later, a);
}

static void run_step(RemoteView *a)
{
    if (a->restart) {
        a->applying = FALSE;
        start_apply(a);
        return;
    }
    while (a->step < a->n && leave(want(a, a->order[a->step]))) a->step++;
    if (a->step >= a->n) {
        a->applying = FALSE;
        remote_refresh(a->m);
        return;
    }
    Row *r = &a->rows[a->order[a->step]];
    r->state = RS_BUSY;
    update_row(r);
    remote_apply(a->m, r->item, want(a, r->item), step_done, a);
}

static void begin(RemoteView *a, const Item *order, int n)
{
    a->applying = TRUE;
    a->restart = FALSE;
    a->order = order;
    a->n = n;
    a->step = 0;
    for (int i = 0; i < n; i++) {
        Row *r = &a->rows[order[i]];
        g_clear_pointer(&r->error, g_free);
        r->perm = FALSE;
        r->state = leave(want(a, r->item)) ? RS_LEAVE : RS_QUEUED;
        update_row(r);
    }
    update_note(a);
    run_step(a);
}

static void start_apply(RemoteView *a)
{
    if (a->applying) { a->restart = TRUE; return; }
    int n;
    const Item *order = apply_order(cfg(a)->mode, &n);
    begin(a, order, n);
}

static void apply_one(RemoteView *a, Row *r)
{
    if (a->applying) { a->restart = TRUE; update_row(r); return; }
    a->single[0] = r->item;
    begin(a, a->single, 1);
}

static void set_mode(RemoteView *a, Mode m)
{
    Config *c = cfg(a);
    c->mode = m;
    c->mode_known = TRUE;
    remote_save(a->m);
    lares_mode_switch_set_active(LARES_MODE_SWITCH(a->sw), m, TRUE);
    lares_set_mode_colors(a->app);
    start_apply(a);
}

static void on_picked(LaresModeSwitch *sw, int side, RemoteView *a)
{
    if (!a->ready) return;
    Mode m = side ? MODE_REMOTE : MODE_HOME;
    if (m == cfg(a)->mode) {
        if (a->note == NOTE_CONFIRM) {
            a->note = NOTE_NONE;
            update_note(a);
            return;
        }
        gboolean all_set = TRUE;
        for (int i = 0; i < N_ITEMS; i++)
            all_set &= a->rows[i].state == RS_OK || a->rows[i].state == RS_LEAVE;
        if (!all_set && !a->applying) start_apply(a);
        return;
    }
    if (m == MODE_HOME && remote_live(a->m)->streaming) {
        a->note = NOTE_CONFIRM;
        update_note(a);
        return;
    }
    a->note = NOTE_NONE;
    set_mode(a, m);
}

static void allowed(gboolean ok, gboolean perm, const char *msg, gpointer user)
{
    RemoteView *a = user;
    gtk_widget_set_sensitive(a->note_main, TRUE);
    if (ok) {
        start_apply(a);
        return;
    }
    Row *r = &a->rows[ITEM_TAILSCALE];
    g_free(r->error);
    r->error = g_strdup(msg);
    r->perm = TRUE;
    update_row(r);
    update_note(a);
}

static void on_note_main(GtkButton *b, RemoteView *a)
{
    switch (a->note) {
    case NOTE_CONFIRM:
        a->note = NOTE_NONE;
        set_mode(a, MODE_HOME);
        break;
    case NOTE_PERMISSION:
        gtk_widget_set_sensitive(a->note_main, FALSE);
        remote_allow_tailscale(a->m, allowed, a);
        break;
    default:
        start_apply(a);
        break;
    }
}

static void on_note_alt(GtkButton *b, RemoteView *a)
{
    a->note = NOTE_NONE;
    update_note(a);
}

/* -------------------------------------------------------------------- chips */

static void on_chip(GtkToggleButton *t, Row *r)
{
    if (r->syncing || !gtk_toggle_button_get_active(t)) return;
    RemoteView *a = r->app;
    Config *c = cfg(a);
    const char *v = g_object_get_data(G_OBJECT(t), "value");
    char **slot = &c->values[c->mode][r->item];
    if (g_str_equal(*slot, v)) return;
    g_free(*slot);
    *slot = g_strdup(v);
    remote_save(a->m);
    apply_one(a, r);
}

static GtkWidget *new_flow(void)
{
    return lares_wrap_new(9);
}

static void add_chip(Row *r, GtkWidget *flow, const char *text, const char *value)
{
    GtkWidget *t = gtk_toggle_button_new_with_label(text);
    gtk_widget_add_css_class(t, "chip");
    gtk_widget_set_cursor_from_name(t, "pointer");
    g_object_set_data_full(G_OBJECT(t), "value", g_strdup(value), g_free);
    if (r->toggles->len)
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(t), r->toggles->pdata[0]);
    g_signal_connect(t, "toggled", G_CALLBACK(on_chip), r);
    lares_wrap_append(LARES_WRAP(flow), t);
    g_ptr_array_add(r->toggles, t);
}

typedef struct { int w, h, hz; gboolean tall; } Res;

static int res_order(gconstpointer x, gconstpointer y)
{
    const Res *a = x, *b = y;
    if (a->tall != b->tall) return a->tall ? -1 : 1;
    if (a->w * a->h != b->w * b->h) return b->w * b->h - a->w * a->h;
    return b->hz - a->hz;
}

/* Only sizes that fill a common screen shape: 16:10, as on MacBooks and many
 * laptops, or 16:9, as on most TVs and monitors.
 * 16:9 fills this monitor. Anything else would add bars somewhere. */
static void build_screen_chips(Row *r)
{
    Live *l = remote_live(r->app->m);
    if (!l->modes) return;
    /* The monitor's own size is what a mode sets until another is picked, so
     * it always has a chip, in its own group when it isn't a common shape. */
    char *own = remote_own_size(l);
    Res mine = { 0 };
    double rate = 0;
    if (own) parse_screen(own, &mine.w, &mine.h, &rate);
    mine.hz = (int)lround(rate);
    gboolean own_listed = !own;
    GArray *list = g_array_new(FALSE, FALSE, sizeof(Res));
    for (guint i = 0; i < l->modes->len; i++) {
        ScreenMode *m = l->modes->pdata[i];
        Res res = { m->w, m->h, (int)lround(m->rate), m->w * 10 == m->h * 16 };
        if (m->w < 1280 || res.hz < 59 || (!res.tall && m->w * 9 != m->h * 16)) continue;
        own_listed |= res.w == mine.w && res.h == mine.h && res.hz == mine.hz;
        gboolean seen = FALSE;
        for (guint j = 0; j < list->len && !seen; j++) {
            Res *o = &g_array_index(list, Res, j);
            seen = o->w == res.w && o->h == res.h && o->hz == res.hz;
        }
        if (!seen) g_array_append_val(list, res);
    }
    g_array_sort(list, res_order);

    GString *key = g_string_new(NULL);
    for (guint j = 0; j < list->len; j++) {
        Res *o = &g_array_index(list, Res, j);
        g_string_append_printf(key, "%dx%d@%d;", o->w, o->h, o->hz);
    }
    if (!own_listed) g_string_append_printf(key, "own %s", own);
    if (r->chips_key && g_str_equal(r->chips_key, key->str)) {
        g_string_free(key, TRUE);
        g_array_unref(list);
        g_free(own);
        return;
    }
    g_free(r->chips_key);
    r->chips_key = g_string_free(key, FALSE);

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(r->chips)))
        gtk_box_remove(GTK_BOX(r->chips), child);
    g_ptr_array_set_size(r->toggles, 0);

    if (!own_listed) {
        mine.tall = FALSE;
        g_array_append_val(list, mine);
    }
    static const char *const GROUPS[] = {
        "16:10, as on MacBooks and many laptops", "16:9, as on most TVs and monitors", "This monitor's own size",
    };
    for (int group = 0; group < 3; group++) {
        GtkWidget *flow = NULL;
        for (guint j = 0; j < list->len; j++) {
            Res *o = &g_array_index(list, Res, j);
            gboolean is_own = !own_listed && j == list->len - 1;
            if (group == 2 ? !is_own : is_own || o->tall != (group == 0)) continue;
            if (!flow) {
                gtk_box_append(GTK_BOX(r->chips), ui_label(GROUPS[group], "group"));
                flow = new_flow();
                gtk_box_append(GTK_BOX(r->chips), flow);
            }
            char *text = o->hz == 60 ? g_strdup_printf("%d×%d", o->w, o->h)
                                     : g_strdup_printf("%d×%d · %d Hz", o->w, o->h, o->hz);
            char *value = g_strdup_printf("%dx%d@%d", o->w, o->h, o->hz);
            add_chip(r, flow, text, value);
            g_free(text);
            g_free(value);
        }
    }
    g_array_unref(list);
    g_free(own);
    sync_chips(r);
}

/* ------------------------------------------------------------------- build */

static void on_head(GtkButton *b, Row *r)
{
    RemoteView *a = r->app;
    gboolean open = !gtk_revealer_get_reveal_child(GTK_REVEALER(r->revealer));
    if (a->open && a->open != r) {
        Row *o = a->open;
        gtk_revealer_set_reveal_child(GTK_REVEALER(o->revealer), FALSE);
        ui_class(o->chevron, "open", FALSE);
        ui_class(o->box, "open", FALSE);
        gtk_accessible_update_state(GTK_ACCESSIBLE(o->head), GTK_ACCESSIBLE_STATE_EXPANDED, FALSE, -1);
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(r->revealer), open);
    ui_class(r->chevron, "open", open);
    ui_class(r->box, "open", open);
    gtk_accessible_update_state(GTK_ACCESSIBLE(r->head), GTK_ACCESSIBLE_STATE_EXPANDED, open, -1);
    a->open = open ? r : NULL;
}

static void build_row(RemoteView *a, Item i, const char *const *art, GtkWidget *card)
{
    Row *r = &a->rows[i];
    r->app = a;
    r->item = i;
    r->toggles = g_ptr_array_new();
    r->state = RS_QUEUED;

    r->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(r->box, "row");

    r->head = gtk_button_new();
    gtk_widget_add_css_class(r->head, "row-head");
    gtk_widget_set_cursor_from_name(r->head, "pointer");
    g_signal_connect(r->head, "clicked", G_CALLBACK(on_head), r);
    gtk_accessible_update_state(GTK_ACCESSIBLE(r->head), GTK_ACCESSIBLE_STATE_EXPANDED, FALSE, -1);

    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_box_append(GTK_BOX(line), ui_icon(art, 2, "row-icon"));

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(texts), ui_label(item_name(i), "name"));
    r->hint = ui_paragraph("", "hint");
    gtk_label_set_wrap_mode(GTK_LABEL(r->hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(r->hint), 24);
    r->hint_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(r->hint_rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(r->hint_rev), 200);
    gtk_revealer_set_child(GTK_REVEALER(r->hint_rev), r->hint);
    gtk_box_append(GTK_BOX(texts), r->hint_rev);
    gtk_box_append(GTK_BOX(line), texts);

    r->value = ui_label("", "value");
    gtk_label_set_xalign(GTK_LABEL(r->value), 1);
    gtk_widget_set_valign(r->value, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(line), r->value);

    /* The value tells the state: what the mode sets, or what's happening;
     * drift and errors get a line under the name and the note. */
    r->chevron = ui_icon(ART_CHEVRON, 2, "chevron");
    gtk_box_append(GTK_BOX(line), r->chevron);
    gtk_button_set_child(GTK_BUTTON(r->head), line);

    r->chips = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_add_css_class(r->chips, "chips");
    r->revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(r->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(r->revealer), 260);
    gtk_revealer_set_child(GTK_REVEALER(r->revealer), r->chips);

    if (i != ITEM_SCREEN) {
        GtkWidget *flow = new_flow();
        gtk_box_append(GTK_BOX(r->chips), flow);
        add_chip(r, flow, "On", "on");
        add_chip(r, flow, "Off", "off");
    }

    gtk_box_append(GTK_BOX(r->box), r->head);
    gtk_box_append(GTK_BOX(r->box), r->revealer);
    if (gtk_widget_get_first_child(card)) {
        GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(sep, "line");
        gtk_box_append(GTK_BOX(card), sep);
    }
    gtk_box_append(GTK_BOX(card), r->box);
}

/* A field of the connection card: its name, and its value on the right. */
static GtkWidget *add_field(GtkWidget *card, const char *name, gboolean selectable)
{
    if (gtk_widget_get_first_child(card)) {
        GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(sep, "line");
        gtk_box_append(GTK_BOX(card), sep);
    }
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_add_css_class(row, "field");
    GtkWidget *n = ui_label(name, NULL);
    gtk_widget_set_hexpand(n, TRUE);
    gtk_box_append(GTK_BOX(row), n);
    GtkWidget *v = ui_label("", "value");
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_label_set_selectable(GTK_LABEL(v), selectable);
    gtk_box_append(GTK_BOX(row), v);
    gtk_box_append(GTK_BOX(card), row);
    return v;
}

static void build_page(RemoteView *a)
{
    a->mode_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 36);

    /* The switch names the mode; under it, how the stream is doing and the
     * address to add this PC by in Moonlight, then what the mode sets. */
    a->sw = lares_mode_switch_new(ART_HOME, "Home", ART_LAPTOP, "Remote");
    g_signal_connect(a->sw, "picked", G_CALLBACK(on_picked), a);
    gtk_box_append(GTK_BOX(a->mode_box), a->sw);

    GtkWidget *conn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(conn, "card");
    a->moonlight = add_field(conn, "Moonlight", FALSE);
    a->ip = add_field(conn, "IP address", TRUE);
    a->host = add_field(conn, "Host name", TRUE);
    gtk_box_append(GTK_BOX(a->mode_box), conn);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    build_row(a, ITEM_SCREEN, ART_SCREEN, card);
    build_row(a, ITEM_SUNSHINE, ART_SUN, card);
    build_row(a, ITEM_TAILSCALE, ART_NET, card);
    gtk_box_append(GTK_BOX(a->mode_box), card);

    GtkWidget *note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(note, "note");
    a->note_icons = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(a->note_icons), ui_icon(ART_BANG, 2, "warn"), "warn");
    gtk_stack_add_named(GTK_STACK(a->note_icons), ui_icon(ART_KEY, 2, "key"), "key");
    gtk_widget_set_valign(a->note_icons, GTK_ALIGN_START);
    gtk_widget_set_size_request(a->note_icons, -1, 27); /* one line of the text, so the icon sits level with it */
    gtk_widget_add_css_class(a->note_icons, "note-icon");
    a->note_text = ui_paragraph("", "note-text");
    gtk_label_set_wrap_mode(GTK_LABEL(a->note_text), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(a->note_text, TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->note_text), 30);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_box_append(GTK_BOX(texts), a->note_text);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    a->note_alt = gtk_button_new_with_label("");
    gtk_widget_add_css_class(a->note_alt, "quiet");
    a->note_main = gtk_button_new_with_label("");
    gtk_widget_add_css_class(a->note_main, "primary");
    gtk_widget_set_cursor_from_name(a->note_alt, "pointer");
    gtk_widget_set_cursor_from_name(a->note_main, "pointer");
    g_signal_connect(a->note_main, "clicked", G_CALLBACK(on_note_main), a);
    g_signal_connect(a->note_alt, "clicked", G_CALLBACK(on_note_alt), a);
    gtk_box_append(GTK_BOX(buttons), a->note_alt);
    gtk_box_append(GTK_BOX(buttons), a->note_main);
    gtk_box_append(GTK_BOX(texts), buttons);
    gtk_box_append(GTK_BOX(note), a->note_icons);
    gtk_box_append(GTK_BOX(note), texts);
    a->note_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(a->note_rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(a->note_rev), 240);
    gtk_revealer_set_child(GTK_REVEALER(a->note_rev), note);
    gtk_box_append(GTK_BOX(a->mode_box), a->note_rev);

    /* With a saved mode the window opens in its colors, with no flash. */
    if (cfg(a)->mode_known) {
        lares_mode_switch_set_active(LARES_MODE_SWITCH(a->sw), cfg(a)->mode, FALSE);
        lares_set_mode_colors(a->app);
    }
    for (int i = 0; i < N_ITEMS; i++) update_row(&a->rows[i]);
}

/* ------------------------------------------------------------ setup guide */

static void open_uri(RemoteView *a, const char *uri)
{
    GtkUriLauncher *l = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(l, GTK_WINDOW(gtk_widget_get_root(a->page)), NULL, NULL, NULL);
    g_object_unref(l);
}

static void on_link(GtkButton *b, RemoteView *a)
{
    open_uri(a, g_object_get_data(G_OBJECT(b), "uri"));
}

/* Signing in: `tailscale up` prints a link to sign in with and waits until
 * that's done. It runs once through pkexec, which asks for the password and
 * also lets Lares switch Tailscale from then on (--operator). */
typedef struct { RemoteView *a; GDataInputStream *err; gboolean opened; } SignIn;

static void sign_in_line(GObject *src, GAsyncResult *res, gpointer data)
{
    SignIn *s = data;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
    if (!line) {
        g_object_unref(s->err);
        s->a->signing_in = FALSE;
        remote_refresh(s->a->m);
        g_free(s);
        return;
    }
    char *url = strstr(line, "https://login.tailscale.com/");
    if (url && !s->opened) {
        s->opened = TRUE;
        char *end = url;
        while (*end && !g_ascii_isspace(*end)) end++;
        *end = 0;
        open_uri(s->a, url);
    }
    g_free(line);
    g_data_input_stream_read_line_async(s->err, G_PRIORITY_DEFAULT, NULL, sign_in_line, s);
}

static void on_sign_in(GtkButton *b, RemoteView *a)
{
    if (a->signing_in || g_getenv("LARES_DRY_RUN")) return;
    char *op = g_strdup_printf("--operator=%s", g_get_user_name());
    const char *argv[] = { "pkexec", "tailscale", "up", op, NULL };
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE, NULL);
    g_free(op);
    if (!p) return;
    SignIn *s = g_new0(SignIn, 1);
    s->a = a;
    s->err = g_data_input_stream_new(g_subprocess_get_stderr_pipe(p));
    a->signing_in = TRUE;
    g_data_input_stream_read_line_async(s->err, G_PRIORITY_DEFAULT, NULL, sign_in_line, s);
    g_object_unref(p);
    remote_refresh(a->m);
}

/* Moonlight pairs with a PIN typed into Sunshine's own page, which needs
 * Sunshine running. */
static void sunshine_started(gboolean ok, gboolean perm, const char *msg, gpointer data)
{
    RemoteView *a = data;
    open_uri(a, "https://localhost:47990");
    remote_refresh(a->m);
}

static void on_open_sunshine(GtkButton *b, RemoteView *a)
{
    if (remote_live(a->m)->sunshine == 1) open_uri(a, "https://localhost:47990");
    else remote_apply(a->m, ITEM_SUNSHINE, "on", sunshine_started, a);
}

static GtkWidget *step_button(RemoteView *a, const char *label, const char *css, const char *uri, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, css);
    gtk_widget_set_cursor_from_name(b, "pointer");
    if (uri) g_object_set_data(G_OBJECT(b), "uri", (gpointer)uri);
    g_signal_connect(b, "clicked", cb ? cb : G_CALLBACK(on_link), a);
    return b;
}

static void set_step(RemoteView *a, int i, gboolean done, const char *status, GtkWidget *b1, GtkWidget *b2)
{
    Step *st = &a->steps[i];
    gtk_stack_set_visible_child_name(GTK_STACK(st->mark), done ? "done" : "todo");
    gtk_label_set_text(GTK_LABEL(st->status), status);
    ui_class(st->status, "value", done);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(st->actions))) gtk_widget_unparent(c);
    if (b1) lares_wrap_append(LARES_WRAP(st->actions), b1);
    if (b2) lares_wrap_append(LARES_WRAP(st->actions), b2);
    gtk_widget_set_visible(st->actions, b1 != NULL);
}

/* Each step ticks itself once Lares sees it done; the buttons open the
 * apps' own pages, as installing them differs on every Linux. */
static void update_guide(RemoteView *a)
{
    Live *l = remote_live(a->m);
    /* Readings come every few seconds; buttons are made again only when
     * something they depend on changed, so one is never swapped mid-click. */
    char *key = g_strdup_printf("%d%d%d%d%d%d %s %s", l->tailscale_installed, l->tailscale_signed_in, a->signing_in,
                                l->sunshine_installed, l->sunshine == 1, l->paired,
                                l->tailscale_host ? l->tailscale_host : "", l->tailscale_ip ? l->tailscale_ip : "");
    if (a->guide_key && g_str_equal(key, a->guide_key)) { g_free(key); return; }
    g_free(a->guide_key);
    a->guide_key = key;
    if (!l->tailscale_installed)
        set_step(a, STEP_TAILSCALE, FALSE, "Not installed yet.",
                 step_button(a, "Install", "primary", "https://tailscale.com/download/linux", NULL), NULL);
    else if (!l->tailscale_signed_in)
        set_step(a, STEP_TAILSCALE, FALSE, a->signing_in ? "Finish signing in in your browser…"
                                                         : "Installed. Sign in to link this PC with your devices.",
                 a->signing_in ? NULL : step_button(a, "Sign in", "primary", NULL, G_CALLBACK(on_sign_in)), NULL);
    else
        set_step(a, STEP_TAILSCALE, TRUE, "Linked.", NULL, NULL); /* the address below names it */

    if (!l->sunshine_installed)
        set_step(a, STEP_SUNSHINE, FALSE, "Not installed yet.",
                 step_button(a, "Install", "primary", "https://github.com/LizardByte/Sunshine", NULL), NULL);
    else
        set_step(a, STEP_SUNSHINE, TRUE, "Installed.", NULL, NULL);

    if (l->paired)
        set_step(a, STEP_MOONLIGHT, TRUE, "A device is paired.", NULL, NULL);
    else
        set_step(a, STEP_MOONLIGHT, FALSE, "Install Moonlight on the device you'll play from and add this PC by "
                 "its address. Moonlight then shows a PIN: type it on Sunshine's page.",
                 step_button(a, "Get Moonlight", "primary", "https://moonlight-stream.org", NULL),
                 l->sunshine_installed ? step_button(a, "Open Sunshine", "quiet", NULL, G_CALLBACK(on_open_sunshine)) : NULL);

    gtk_label_set_text(GTK_LABEL(a->guide_ip), l->tailscale_ip ? l->tailscale_ip : "");
    gtk_label_set_text(GTK_LABEL(a->guide_host), l->tailscale_host ? l->tailscale_host : "");
    gtk_widget_set_visible(a->guide_or, l->tailscale_host != NULL);
    gtk_widget_set_visible(a->guide_host, l->tailscale_host != NULL);
    gtk_widget_set_visible(a->guide_address, l->tailscale_ip != NULL);
}

static void build_step(RemoteView *a, GtkWidget *card, int i, const char *number, const char *name, const char *what)
{
    Step *st = &a->steps[i];
    if (i) {
        GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(sep, "line");
        gtk_box_append(GTK_BOX(card), sep);
    }
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(row, "step");
    st->mark = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(st->mark), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    GtkWidget *num = ui_label(number, "step-number");
    gtk_label_set_xalign(GTK_LABEL(num), 0.5);
    gtk_widget_set_size_request(num, 27, 27);
    gtk_stack_add_named(GTK_STACK(st->mark), num, "todo");
    gtk_stack_add_named(GTK_STACK(st->mark), ui_icon(ART_CHECK, 2, "ok"), "done");
    gtk_widget_set_valign(st->mark, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), st->mark);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    GtkWidget *title = ui_label(name, NULL);
    gtk_widget_set_size_request(title, -1, 27); /* as tall as the mark beside it, so their middles meet */
    gtk_box_append(GTK_BOX(texts), title);
    GtkWidget *w = ui_paragraph(what, "muted");
    gtk_box_append(GTK_BOX(texts), w);
    st->status = ui_paragraph("", NULL);
    gtk_box_append(GTK_BOX(texts), st->status);
    st->actions = lares_wrap_new(9);
    gtk_box_append(GTK_BOX(texts), st->actions);
    gtk_box_append(GTK_BOX(row), texts);
    gtk_box_append(GTK_BOX(card), row);
}

static void build_guide(RemoteView *a)
{
    a->guide = gtk_box_new(GTK_ORIENTATION_VERTICAL, 36);
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_box_append(GTK_BOX(hero), ui_label("Remote play", "title"));
    GtkWidget *sub = ui_paragraph("Play this PC's games and desktop from a laptop, tablet or phone, at home or away. "
                                  "Three free apps do it.", "sub");
    gtk_box_append(GTK_BOX(hero), sub);
    gtk_box_append(GTK_BOX(a->guide), hero);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "steps");
    build_step(a, card, STEP_TAILSCALE, "1", "Tailscale", "Links your devices privately, wherever they are.");
    build_step(a, card, STEP_SUNSHINE, "2", "Sunshine", "Shares this PC's screen, sound and controllers.");
    build_step(a, card, STEP_MOONLIGHT, "3", "Moonlight", "Plays it on your other device.");
    gtk_box_append(GTK_BOX(a->guide), card);

    /* The address and the name wrap as whole words: a name never breaks at
     * its hyphen. */
    a->guide_address = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_box_append(GTK_BOX(a->guide_address), ui_label("This PC's address", "muted"));
    GtkWidget *values = lares_wrap_new(9);
    a->guide_ip = ui_label("", "value");
    gtk_label_set_selectable(GTK_LABEL(a->guide_ip), TRUE);
    lares_wrap_append(LARES_WRAP(values), a->guide_ip);
    a->guide_or = ui_label("or", "muted");
    lares_wrap_append(LARES_WRAP(values), a->guide_or);
    a->guide_host = ui_label("", "value");
    gtk_label_set_selectable(GTK_LABEL(a->guide_host), TRUE);
    lares_wrap_append(LARES_WRAP(values), a->guide_host);
    gtk_box_append(GTK_BOX(a->guide_address), values);
    gtk_box_append(GTK_BOX(a->guide), a->guide_address);
}

/* -------------------------------------------------------------- live state */

static void on_live(gpointer user)
{
    RemoteView *a = user;
    /* Until remote play is set up, the section is the guide. */
    gboolean set_up = cfg(a)->ready;
    gtk_widget_set_visible(a->guide, !set_up);
    gtk_widget_set_visible(a->mode_box, set_up);
    update_guide(a);
    if (!a->ready) {
        Config *c = cfg(a);
        lares_mode_switch_set_active(LARES_MODE_SWITCH(a->sw), c->mode, FALSE);
        for (int i = 0; i < N_ITEMS; i++) a->rows[i].state = RS_OK;
        lares_set_mode_colors(a->app);
        a->ready = TRUE;
    }
    build_screen_chips(&a->rows[ITEM_SCREEN]);
    if (!a->applying) settle_rows(a);
    update_connection(a);
    update_note(a);
}

GtkWidget *remote_build(Lares *app, Section *self)
{
    RemoteView *a = g_new0(RemoteView, 1);
    a->app = app;
    a->m = app->remote;
    self->view = a;
    build_page(a);
    build_guide(a);
    a->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(a->page), a->guide);
    gtk_box_append(GTK_BOX(a->page), a->mode_box);
    gtk_widget_set_visible(a->guide, FALSE);
    gtk_widget_set_visible(a->mode_box, FALSE);
    remote_watch(a->m, on_live, a);
    if (remote_live(a->m)->sunshine >= 0) on_live(a); /* the Overview already read it */
    return a->page;
}

void remote_shown(Lares *app, Section *self, gboolean on)
{
    remote_want(app->remote, on);
}
