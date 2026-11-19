// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Controller section: a card for each Xbox controller, with Padlight's
 * live picture on its Buttons tab, next to Sticks and Vibration, the battery,
 * connection, model and firmware, who has it open, and Test vibration, Find
 * and Disconnect. Under the cards: paired controllers that are away, Pair a
 * controller, and the controller settings (the background helper and the
 * top-bar icon are in Settings). Ported from Padlight's controllerCard.js,
 * controllerCheck.js, buttonCheck.js, checklist.js, vibrationLab.js and
 * prefs.js. Nothing reads a controller unless its card is on screen. */
#include <math.h>
#include <stdlib.h>
#include "lares.h"
#include "pad_picture.h"
#include "pad_service.h"
#include "pad/padactions.h"
#include "pad/padinput.h"
#include "pad/padpair.h"

#define GAUGE_SIZE     126
#define DEAD_ZONE      0.10    /* games commonly ignore about this much around the centre */
#define REST_STILL     0.02    /* a stick rests once it holds this still */
#define REST_MS        600     /* for this long */
#define REST_REACH     0.4     /* this close to the centre */
#define TRIGGER_PULL   0.9     /* a trigger counts for the button test pulled this far */
#define STRENGTH_STEP  5

enum { TAB_BUTTONS, TAB_STICKS, TAB_VIBRATION, N_TABS };
static const char *const TAB_NAMES[N_TABS] = { "Buttons", "Sticks", "Vibration" };

/* What buttons whose text changes can say; each keeps the width of its
 * widest (ui_button_labels), so a click doesn't move it. */
static const char *const PLAY_LABELS[] = { "Play", "Playing…", NULL };
static const char *const FIND_LABELS[] = { "Find controller", "Stop finding", NULL };
static const char *const CONNECT_LABELS[] = { "Connect", "Connecting…", NULL };
/* padpair_action's, and "Pairing…" while it pairs, with nothing to click. */
static const char *const PAIR_LABELS[] = { "Pair a controller", "Stop searching", "Search again",
                                           "Pair another controller", "Try again", "Pairing…", NULL };

typedef struct PadView PadView;

/* What a controller's card remembers while it stays connected, so a card
 * made again keeps its tab, its button test and its vibration settings. */
typedef struct {
    int tab;
    guint32 seen;
    int strong, weak, length_ms;
} Memory;

typedef struct {
    double x, y;              /* where it is now, before any dead zone */
    double ax, ay;            /* where it last held still */
    gint64 since;
    gboolean anchored, has_rest;
    double rx, ry;            /* where it rests */
} Stick;

typedef struct {
    PadView *v;
    char *path;
    GtkWidget *name, *sub, *entry, *stack, *tabs[N_TABS];
    GtkWidget *picture, *count;
    GtkWidget *gauges[2], *stick_text[2], *trig_meter[2], *trig_text[2];
    GtkWidget *strong, *weak, *length, *strong_text, *weak_text, *length_text, *play;
    GtkWidget *users, *find, *test;
    GtkWidget *battery, *battery_text;  /* turn orange at the low battery level */
    PadReader *reader;
    gboolean reader_raw;
    Stick sticks[2];
    PadState last;            /* the newest reading, for the rest check */
    guint rest_tick;          /* while the Sticks tab shows */
} Card;

struct PadView {
    Lares *app;
    GtkWidget *page, *cards, *empty, *away, *pair_card, *pair_status, *pair_action, *pair_found;
    GtkWidget *level_text, *idle_chips, *depends, *helper_off, *nicknames;
    GtkWidget *sw_alerts, *sw_notices, *sw_awake, *sw_idle;
    GPtrArray *card_list;     /* Card* */
    GHashTable *memory;       /* pad path -> Memory */
    GHashTable *users;        /* pad path -> "In use by …" */
    PadPair *pair;
    char *shape;              /* what the cards were built for */
    char *away_busy;          /* the away controller being connected */
    char *away_error_id, *away_error; /* why its last try failed */
    gboolean on_screen, syncing;
};

static void update_readers(PadView *v);
static void rebuild(gpointer user);

static PadPrefs *prefs(PadView *v) { return padconf_prefs(v->app->padconf); }

static Memory *memory_of(PadView *v, const char *path)
{
    Memory *m = g_hash_table_lookup(v->memory, path);
    if (!m) {
        m = g_new0(Memory, 1);
        m->strong = m->weak = PAD_RUMBLE_STRENGTH;
        m->length_ms = PAD_RUMBLE_MS;
        g_hash_table_insert(v->memory, g_strdup(path), m);
    }
    return m;
}

static GtkWidget *button(const char *label, const char *css, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, css);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static char *percent(double v)
{
    int r = (int)lround(v * 100);
    return r < 0 ? g_strdup_printf("−%d%%", -r) : g_strdup_printf("%d%%", r);
}

/* --------------------------------------------------------------- sticks */

/* Follows one stick and works out where it rests: wherever it holds still
 * for a moment, close to the centre. */
static void stick_feed(Stick *s, double x, double y)
{
    s->x = x;
    s->y = y;
    gint64 now = g_get_monotonic_time() / 1000;
    if (hypot(x, y) > REST_REACH) { s->anchored = FALSE; return; }
    if (!s->anchored || hypot(x - s->ax, y - s->ay) > REST_STILL) {
        s->ax = x; s->ay = y; s->since = now; s->anchored = TRUE;
    } else if (now - s->since >= REST_MS) {
        s->rx = x; s->ry = y; s->has_rest = TRUE;
    }
}

static gboolean drifting(const Stick *s) { return s->has_rest && hypot(s->rx, s->ry) > DEAD_ZONE; }

/* One stick's whole travel as a circle, with the dead zone, where the stick
 * rests and where it is now. */
static void draw_gauge(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    Stick *s = data;
    GdkRGBA fg;
    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    gboolean on_dark = 0.299 * fg.red + 0.587 * fg.green + 0.114 * fg.blue > 0.5;
    double radius = MIN(w, h) / 2.0 - 2;
    cairo_translate(cr, w / 2.0, h / 2.0);
    cairo_set_line_width(cr, 1);
    cairo_arc(cr, 0, 0, radius, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.07);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.22);
    cairo_stroke(cr);
    cairo_move_to(cr, -radius, 0); cairo_line_to(cr, radius, 0);
    cairo_move_to(cr, 0, -radius); cairo_line_to(cr, 0, radius);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.1);
    cairo_stroke(cr);
    cairo_arc(cr, 0, 0, radius * DEAD_ZONE, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.16);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.4);
    cairo_stroke(cr);
    if (s->has_rest) {
        double rx = s->rx, ry = s->ry, reach = hypot(rx, ry);
        if (reach > 1) { rx /= reach; ry /= reach; }
        cairo_arc(cr, rx * radius, ry * radius, 6, 0, 2 * G_PI);
        if (drifting(s)) {
            if (on_dark) cairo_set_source_rgb(cr, 1, 0.69, 0.18);
            else cairo_set_source_rgb(cr, 0.76, 0.37, 0);
        } else {
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.55);
        }
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }
    double x = s->x, y = s->y, reach = hypot(x, y);
    if (reach > 1) { x /= reach; y /= reach; }
    cairo_arc(cr, x * radius, y * radius, 3.5, 0, 2 * G_PI);
    if (on_dark) cairo_set_source_rgb(cr, 0.25, 0.83, 0.25);
    else cairo_set_source_rgb(cr, 0.06, 0.49, 0.06);
    cairo_fill(cr);
}

static void set_text(GtkWidget *label, const char *text)
{
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0) gtk_label_set_text(GTK_LABEL(label), text);
}

static void show_sticks(Card *c, const PadState *s)
{
    stick_feed(&c->sticks[0], s->lx, s->ly);
    stick_feed(&c->sticks[1], s->rx, s->ry);
    for (int i = 0; i < 2; i++) {
        Stick *st = &c->sticks[i];
        char *x = percent(st->x), *y = percent(-st->y);
        char *text;
        if (drifting(st)) {
            char *off = percent(hypot(st->rx, st->ry));
            text = g_strdup_printf("%s, %s · Rests %s off centre: it may move a game by itself", x, y, off);
            g_free(off);
        } else if (st->has_rest) {
            text = g_strdup_printf("%s, %s · Rests in the centre", x, y);
        } else {
            text = g_strdup_printf("%s, %s · Let go to see where it rests", x, y);
        }
        set_text(c->stick_text[i], text);
        ui_class(c->stick_text[i], "warn", drifting(st));
        g_free(x); g_free(y); g_free(text);
        gtk_widget_queue_draw(c->gauges[i]);
        double t = i == 0 ? s->lt : s->rt;
        lares_meter_set(LARES_METER(c->trig_meter[i]), t);
        char *tp = percent(t);
        set_text(c->trig_text[i], tp);
        g_free(tp);
    }
}

/* ---------------------------------------------------------- button test */

static void show_count(Card *c, const Pad *pad, Memory *m)
{
    int total = __builtin_popcount(pad->checklist), count = __builtin_popcount(m->seen & pad->checklist);
    char *text = count == total && total ? g_strdup("Every button responded")
                                          : g_strdup_printf("Press every button: %d of %d", count, total);
    set_text(c->count, text);
    ui_class(c->count, "value", count == total && total);
    g_free(text);
}

static void feed_test(Card *c, const PadState *s)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    if (!pad) return;
    Memory *m = memory_of(c->v, c->path);
    guint32 pressed = s->buttons;
    if (s->lt >= TRIGGER_PULL) pressed |= PB_BIT(PB_LT);
    if (s->rt >= TRIGGER_PULL) pressed |= PB_BIT(PB_RT);
    guint32 before = m->seen;
    m->seen |= pressed & pad->checklist;
    lares_pad_picture_set_state(LARES_PAD_PICTURE(c->picture), s);
    if (m->seen != before) {
        lares_pad_picture_set_seen(LARES_PAD_PICTURE(c->picture), m->seen);
        show_count(c, pad, m);
    }
}

static void on_reset(GtkButton *b, Card *c)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    Memory *m = memory_of(c->v, c->path);
    m->seen = 0;
    lares_pad_picture_set_seen(LARES_PAD_PICTURE(c->picture), 0);
    if (pad) show_count(c, pad, m);
}

/* ---------------------------------------------------------- live reading */

static void on_state(const PadState *s, gpointer data)
{
    Card *c = data;
    c->last = *s;
    switch (memory_of(c->v, c->path)->tab) {
    case TAB_STICKS: show_sticks(c, s); break;
    case TAB_BUTTONS: feed_test(c, s); break;
    default: break;
    }
}

/* A stick held still sends nothing, so the rest check looks again on its
 * own, five times a second, while the Sticks tab shows. */
static gboolean rest_tick(gpointer data)
{
    Card *c = data;
    show_sticks(c, &c->last);
    return G_SOURCE_CONTINUE;
}

static void stop_reader(Card *c)
{
    if (c->rest_tick) { g_source_remove(c->rest_tick); c->rest_tick = 0; }
    g_clear_pointer(&c->reader, pad_reader_free);
    lares_pad_picture_set_state(LARES_PAD_PICTURE(c->picture), NULL);
}

/* One reader per card, only while it's on screen and its tab moves: the
 * Sticks tab reads without the dead zone, the others with it. */
static void update_readers(PadView *v)
{
    for (guint i = 0; i < v->card_list->len; i++) {
        Card *c = v->card_list->pdata[i];
        const Pad *pad = pads_find(v->app->pads, c->path);
        int tab = memory_of(v, c->path)->tab;
        gboolean need = v->on_screen && pad && tab != TAB_VIBRATION;
        gboolean raw = tab == TAB_STICKS;
        if (c->reader && (!need || c->reader_raw != raw)) stop_reader(c);
        if (need && !c->reader) {
            c->reader = pad_reader_live(pad->event_node, raw, on_state, c);
            c->reader_raw = raw;
            if (c->reader && raw) c->rest_tick = g_timeout_add(200, rest_tick, c);
        }
    }
}

static void on_tab(GtkToggleButton *t, Card *c)
{
    if (!gtk_toggle_button_get_active(t) || c->v->syncing) return;
    int tab = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "tab"));
    memory_of(c->v, c->path)->tab = tab;
    gtk_stack_set_visible_child_name(GTK_STACK(c->stack), TAB_NAMES[tab]);
    /* Play does what Test vibration does on the Vibration tab. */
    if (c->test) gtk_widget_set_visible(c->test, tab != TAB_VIBRATION);
    update_readers(c->v);
}

/* ------------------------------------------------------------- vibration */

static void show_vibration(Card *c)
{
    Memory *m = memory_of(c->v, c->path);
    char buf[32];
    snprintf(buf, sizeof buf, "%d%%", m->strong);
    set_text(c->strong_text, buf);
    snprintf(buf, sizeof buf, "%d%%", m->weak);
    set_text(c->weak_text, buf);
    snprintf(buf, sizeof buf, "%.1f s", m->length_ms / 1000.0);
    set_text(c->length_text, buf);
}

static void on_strength(LaresMeter *meter, double f, gpointer data)
{
    Card *c = data;
    Memory *m = memory_of(c->v, c->path);
    int v = (int)lround(f * 100 / STRENGTH_STEP) * STRENGTH_STEP;
    if (GTK_WIDGET(meter) == c->strong) m->strong = v; else m->weak = v;
    show_vibration(c);
}

static void on_length(LaresMeter *meter, double f, gpointer data)
{
    Card *c = data;
    Memory *m = memory_of(c->v, c->path);
    m->length_ms = CLAMP((int)lround(f * 20) * 100, 100, 2000);
    lares_meter_set(meter, m->length_ms / 2000.0);
    show_vibration(c);
}

static gboolean played(gpointer data)
{
    GtkWidget *b = data;
    ui_button_label(b, "Play");
    gtk_widget_set_sensitive(b, TRUE);
    g_object_unref(b);
    return G_SOURCE_REMOVE;
}

static void on_play(GtkButton *b, Card *c)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    Memory *m = memory_of(c->v, c->path);
    if (!pad || !pad_rumble(pad, m->strong, m->weak, m->length_ms)) return;
    ui_button_label(GTK_WIDGET(b), "Playing…");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    g_timeout_add(m->length_ms, played, g_object_ref(b));
}

/* ---------------------------------------------------------------- actions */

static void on_test(GtkButton *b, Card *c)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    if (pad && pad_rumble(pad, PAD_RUMBLE_STRENGTH, PAD_RUMBLE_STRENGTH, PAD_RUMBLE_MS))
        lares_pad_picture_shake(LARES_PAD_PICTURE(c->picture));
}

static void find_done(gpointer data)
{
    PadView *v = data;
    for (guint i = 0; i < v->card_list->len; i++) {
        Card *c = v->card_list->pdata[i];
        if (c->find) ui_button_label(c->find, pad_finding(c->path) ? "Stop finding" : "Find controller");
    }
}

/* Ten short, strong buzzes, to find it under a cushion. It carries on with
 * the window closed, and stops if the controller disconnects. */
static void on_find(GtkButton *b, Card *c)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    if (!pad) return;
    if (pad_finding(c->path)) pad_find_stop(c->path);
    else pad_find_start(pad, find_done, c->v);
    find_done(c->v);
}

static void on_disconnect(GtkButton *b, Card *c)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    if (pad) pad_disconnect(pad);
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
}

/* -------------------------------------------------------------- nicknames */

static void edit_done(Card *c, gboolean keep)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    if (keep && pad) pads_set_nickname(c->v->app->pads, pad, gtk_editable_get_text(GTK_EDITABLE(c->entry)));
    gtk_widget_set_visible(c->entry, FALSE);
    gtk_widget_set_visible(c->name, TRUE);
}

static void on_entry_activate(GtkEntry *e, Card *c) { edit_done(c, TRUE); }

static gboolean on_entry_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType m, Card *c)
{
    if (keyval != GDK_KEY_Escape) return FALSE;
    edit_done(c, FALSE);
    return TRUE;
}

static void on_pencil(GtkButton *b, Card *c)
{
    const Pad *pad = pads_find(c->v->app->pads, c->path);
    gtk_editable_set_text(GTK_EDITABLE(c->entry), pad && pad->nickname ? pad->nickname : "");
    gtk_widget_set_visible(c->name, FALSE);
    gtk_widget_set_visible(c->entry, TRUE);
    gtk_widget_grab_focus(c->entry);
}

/* ------------------------------------------------------------------ card */

static GtkWidget *fact(const char *heading, GtkWidget *value)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_box_append(GTK_BOX(box), ui_label(heading, "muted"));
    gtk_box_append(GTK_BOX(box), value);
    return box;
}

static GtkWidget *meter_row(GtkWidget **meter, GtkWidget **text, int blocks, const char *heading)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *h = ui_label(heading, NULL);
    gtk_widget_set_hexpand(h, TRUE);
    gtk_box_append(GTK_BOX(top), h);
    *text = ui_label("", "value");
    gtk_box_append(GTK_BOX(top), *text);
    gtk_box_append(GTK_BOX(box), top);
    *meter = lares_meter_new(blocks, 2);
    gtk_box_append(GTK_BOX(box), *meter);
    return box;
}

static void card_free(gpointer data)
{
    Card *c = data;
    if (c->rest_tick) g_source_remove(c->rest_tick);
    g_clear_pointer(&c->reader, pad_reader_free);
    g_free(c->path);
    g_free(c);
}

static GtkWidget *sticks_page(Card *c)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    const char *names[2] = { "Left stick", "Right stick" }, *trig[2] = { "Left trigger", "Right trigger" };
    for (int i = 0; i < 2; i++) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
        gtk_box_append(GTK_BOX(col), ui_label(names[i], NULL));
        c->gauges[i] = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(c->gauges[i]), GAUGE_SIZE);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(c->gauges[i]), GAUGE_SIZE);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(c->gauges[i]), draw_gauge, &c->sticks[i], NULL);
        gtk_widget_set_halign(c->gauges[i], GTK_ALIGN_START);
        gtk_widget_add_css_class(c->gauges[i], "gauge");
        gtk_box_append(GTK_BOX(col), c->gauges[i]);
        c->stick_text[i] = ui_paragraph("", "muted");
        gtk_box_append(GTK_BOX(col), c->stick_text[i]);
        gtk_box_append(GTK_BOX(col), meter_row(&c->trig_meter[i], &c->trig_text[i], 15, trig[i]));
        gtk_box_append(GTK_BOX(row), col);
    }
    gtk_box_append(GTK_BOX(page), row);
    return page;
}

static GtkWidget *vibration_page(Card *c)
{
    Memory *m = memory_of(c->v, c->path);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(page), meter_row(&c->strong, &c->strong_text, 20, "Strong motor, on the left"));
    gtk_box_append(GTK_BOX(page), meter_row(&c->weak, &c->weak_text, 20, "Weak motor, on the right"));
    gtk_box_append(GTK_BOX(page), meter_row(&c->length, &c->length_text, 20, "Length"));
    lares_meter_set(LARES_METER(c->strong), m->strong / 100.0);
    lares_meter_set(LARES_METER(c->weak), m->weak / 100.0);
    lares_meter_set(LARES_METER(c->length), m->length_ms / 2000.0);
    lares_meter_set_interactive(LARES_METER(c->strong), on_strength, c);
    lares_meter_set_interactive(LARES_METER(c->weak), on_strength, c);
    lares_meter_set_interactive(LARES_METER(c->length), on_length, c);
    show_vibration(c);
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *note = ui_paragraph("Linux drives these two motors; the ones in the triggers aren't part of it.", "muted");
    gtk_widget_set_hexpand(note, TRUE);
    gtk_box_append(GTK_BOX(foot), note);
    c->play = button("Play", "primary", G_CALLBACK(on_play), c);
    ui_button_labels(c->play, PLAY_LABELS);
    gtk_widget_set_valign(c->play, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(foot), c->play);
    gtk_box_append(GTK_BOX(page), foot);
    return page;
}

static char *since_text(gint64 since)
{
    if (!since) return NULL;
    GDateTime *t = g_date_time_new_from_unix_local(since);
    char *s = g_date_time_format(t, "%H:%M");
    g_date_time_unref(t);
    return s;
}

/* The nickname, or the model's name, and how it's connected. */
static void show_name(Card *c, const Pad *pad)
{
    char *since = since_text(pad->since);
    char *sub = pad->nickname ? g_strdup_printf("%s · %s%s%s", pad->name, pad_connection_name(pad->connection),
                                                since ? " since " : "", since ? since : "")
                              : g_strdup_printf("%s%s%s", pad_connection_name(pad->connection),
                                                since ? " since " : "", since ? since : "");
    set_text(c->name, pad_name(pad));
    set_text(c->sub, sub);
    g_free(sub);
    g_free(since);
}

static void show_low(Card *c, const Pad *pad)
{
    if (!c->battery) return;
    gboolean low = pad_battery_low_stage(&pad->battery, prefs(c->v)->low_level) > 0
                && !pad_battery_charging(&pad->battery);
    ui_class(c->battery, "low", low);
    ui_class(c->battery_text, "low", low);
}

static GtkWidget *pad_card(PadView *v, const Pad *pad)
{
    Card *c = g_new0(Card, 1);
    c->v = v;
    c->path = g_strdup(pad->path);
    g_ptr_array_add(v->card_list, c);
    Memory *m = memory_of(v, pad->path);
    if (m->tab == TAB_VIBRATION && !pad->rumble) m->tab = TAB_BUTTONS;

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "padded");

    /* Who it is: the nickname, or the model's name, and how it's connected. */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *art = ui_icon(ART_GAMEPAD, 4, "device-art");
    gtk_widget_add_css_class(art, "on");
    gtk_box_append(GTK_BOX(head), art);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    c->name = ui_label("", "card-title");
    gtk_label_set_ellipsize(GTK_LABEL(c->name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(texts), c->name);
    c->entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(c->entry), PAD_NICKNAME_LENGTH);
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->entry), "A nickname, such as Blue pad");
    gtk_widget_set_visible(c->entry, FALSE);
    g_signal_connect(c->entry, "activate", G_CALLBACK(on_entry_activate), c);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), c);
    gtk_widget_add_controller(c->entry, keys);
    gtk_box_append(GTK_BOX(texts), c->entry);
    c->sub = ui_label("", "muted");
    gtk_label_set_ellipsize(GTK_LABEL(c->sub), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(texts), c->sub);
    show_name(c, pad);
    gtk_box_append(GTK_BOX(head), texts);
    if (pad_can_nickname(pad)) {
        GtkWidget *pencil = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(pencil), ui_icon(ART_PENCIL, 2, NULL));
        gtk_widget_add_css_class(pencil, "quiet");
        gtk_widget_set_valign(pencil, GTK_ALIGN_CENTER);
        gtk_widget_set_cursor_from_name(pencil, "pointer");
        gtk_widget_set_tooltip_text(pencil, NULL);
        gtk_accessible_update_property(GTK_ACCESSIBLE(pencil), GTK_ACCESSIBLE_PROPERTY_LABEL, "Nickname", -1);
        g_signal_connect(pencil, "clicked", G_CALLBACK(on_pencil), c);
        gtk_box_append(GTK_BOX(head), pencil);
    }
    gtk_box_append(GTK_BOX(card), head);

    /* The tabs, and what they show. */
    GtkWidget *tabs = lares_wrap_new(9);
    gtk_widget_add_css_class(tabs, "tabs");
    c->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(c->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(c->stack), 160);
    gtk_stack_set_interpolate_size(GTK_STACK(c->stack), TRUE);
    gtk_stack_set_vhomogeneous(GTK_STACK(c->stack), FALSE);
    v->syncing = TRUE;
    for (int t = 0; t < N_TABS; t++) {
        if (t == TAB_VIBRATION && !pad->rumble) continue;
        GtkWidget *tb = gtk_toggle_button_new_with_label(TAB_NAMES[t]);
        gtk_widget_add_css_class(tb, "chip");
        gtk_widget_set_cursor_from_name(tb, "pointer");
        g_object_set_data(G_OBJECT(tb), "tab", GINT_TO_POINTER(t));
        if (c->tabs[0] && t) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tb), GTK_TOGGLE_BUTTON(c->tabs[0]));
        g_signal_connect(tb, "toggled", G_CALLBACK(on_tab), c);
        c->tabs[t] = tb;
        lares_wrap_append(LARES_WRAP(tabs), tb);
    }
    GtkWidget *test = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    c->picture = lares_pad_picture_new(FALSE);
    lares_pad_picture_set_seen(LARES_PAD_PICTURE(c->picture), m->seen);
    gtk_box_append(GTK_BOX(test), c->picture);
    GtkWidget *count_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    c->count = ui_label("", "muted");
    gtk_widget_set_hexpand(c->count, TRUE);
    gtk_box_append(GTK_BOX(count_row), c->count);
    gtk_box_append(GTK_BOX(count_row), button("Reset", "quiet", G_CALLBACK(on_reset), c));
    gtk_box_append(GTK_BOX(test), count_row);
    show_count(c, pad, m);
    gtk_stack_add_named(GTK_STACK(c->stack), test, TAB_NAMES[TAB_BUTTONS]);
    gtk_stack_add_named(GTK_STACK(c->stack), sticks_page(c), TAB_NAMES[TAB_STICKS]);
    if (pad->rumble) gtk_stack_add_named(GTK_STACK(c->stack), vibration_page(c), TAB_NAMES[TAB_VIBRATION]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->tabs[m->tab]), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(c->stack), TAB_NAMES[m->tab]);
    v->syncing = FALSE;
    gtk_box_append(GTK_BOX(card), tabs);
    gtk_box_append(GTK_BOX(card), c->stack);

    /* Battery, model and firmware; the connection is in the line under the name. */
    GtkWidget *facts = lares_wrap_new(27);
    GtkWidget *bat = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    if (pad->battery.known) {
        c->battery = lares_meter_new(10, 2);
        lares_meter_set(LARES_METER(c->battery), pad_battery_fraction(&pad->battery));
        gtk_box_append(GTK_BOX(bat), c->battery);
        char *text = pad_battery_text(&pad->battery);
        c->battery_text = ui_label(text, NULL);
        gtk_box_append(GTK_BOX(bat), c->battery_text);
        g_free(text);
        show_low(c, pad);
    } else {
        gtk_box_append(GTK_BOX(bat), ui_label(pad->connection == PAD_USB ? "On USB power" : "No reading yet", NULL));
    }
    lares_wrap_append(LARES_WRAP(facts), fact("Battery", bat));
    if (pad->model) lares_wrap_append(LARES_WRAP(facts), fact("Model", ui_label(pad->model, NULL)));
    if (pad->firmware)
        lares_wrap_append(LARES_WRAP(facts), fact("Firmware", ui_label(pad->firmware, pad->firmware_update ? "warn" : NULL)));
    gtk_box_append(GTK_BOX(card), facts);
    if (pad->firmware_update) {
        char *hint = g_strdup_printf("Firmware %s is out. Linux has no updater for it: use the Xbox Accessories "
                                     "app on Windows, or an Xbox console.", pad->firmware_update);
        GtkWidget *l = ui_paragraph(hint, "warn");
        gtk_box_append(GTK_BOX(card), l);
        g_free(hint);
    }
    char *details = g_strdup_printf("%s · %s", pad->driver ? pad->driver : "Unknown driver", pad->event_node);
    gtk_box_append(GTK_BOX(card), ui_label(details, "muted"));
    g_free(details);
    c->users = ui_paragraph(g_hash_table_lookup(v->users, pad->path), "muted");
    gtk_widget_set_visible(c->users, g_hash_table_contains(v->users, pad->path));
    gtk_box_append(GTK_BOX(card), c->users);

    /* What it can do. */
    GtkWidget *actions = lares_wrap_new(9);
    if (pad->rumble) {
        c->test = button("Test vibration", "quiet", G_CALLBACK(on_test), c);
        gtk_widget_set_visible(c->test, m->tab != TAB_VIBRATION);
        lares_wrap_append(LARES_WRAP(actions), c->test);
        if (pad->connection != PAD_USB) {
            c->find = button("Find controller", "quiet", G_CALLBACK(on_find), c);
            ui_button_labels(c->find, FIND_LABELS);
            if (pad_finding(pad->path)) ui_button_label(c->find, "Stop finding");
            lares_wrap_append(LARES_WRAP(actions), c->find);
        }
    }
    if (pad->connection == PAD_BLUETOOTH && pad->address)
        lares_wrap_append(LARES_WRAP(actions), button("Disconnect", "quiet", G_CALLBACK(on_disconnect), c));
    if (gtk_widget_get_first_child(actions)) gtk_box_append(GTK_BOX(card), actions);
    return card;
}

/* ------------------------------------------------------------------ away */

static void build_away(PadView *v);

/* Once connected, the device watcher takes the card away; if it failed,
 * the card gets its button back and says why. */
static void away_done(gboolean ok, const char *message, gpointer user)
{
    PadView *v = user;
    g_free(v->away_error_id);
    g_free(v->away_error);
    v->away_error_id = ok ? NULL : g_strdup(v->away_busy);
    v->away_error = ok ? NULL : g_strdup(message);
    g_clear_pointer(&v->away_busy, g_free);
    build_away(v);
}

static void on_connect_away(GtkButton *b, PadView *v)
{
    if (v->away_busy) return;
    v->away_busy = g_strdup(g_object_get_data(G_OBJECT(b), "device"));
    g_clear_pointer(&v->away_error_id, g_free);
    g_clear_pointer(&v->away_error, g_free);
    build_away(v);
    devices_set_connected(v->app->devices, v->away_busy, TRUE, away_done, v);
}

/* Paired Bluetooth controllers that aren't here right now. */
static void build_away(PadView *v)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(v->away))) gtk_box_remove(GTK_BOX(v->away), c);
    GPtrArray *list = devices_list(v->app->devices);
    gboolean any = FALSE;
    for (guint i = 0; i < list->len; i++) {
        Device *d = list->pdata[i];
        if (d->kind != DEV_CONTROLLER || d->connected) continue;
        any = TRUE;
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        gtk_widget_add_css_class(card, "card");
        gtk_widget_add_css_class(card, "padded");
        gtk_box_append(GTK_BOX(card), ui_icon(ART_GAMEPAD, 4, "device-art"));
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
        gtk_widget_set_hexpand(texts, TRUE);
        gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
        /* The name gives way, not the button, so the card keeps its padding. */
        GtkWidget *name = ui_label(d->name, NULL);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(texts), name);
        gtk_box_append(GTK_BOX(texts), ui_label("Not connected", "muted"));
        if (v->away_error_id && g_str_equal(v->away_error_id, d->id))
            gtk_box_append(GTK_BOX(texts), ui_paragraph(v->away_error, "bad"));
        gtk_box_append(GTK_BOX(card), texts);
        gboolean busy = v->away_busy && g_str_equal(v->away_busy, d->id);
        GtkWidget *b = button("Connect", "primary", G_CALLBACK(on_connect_away), v);
        ui_button_labels(b, CONNECT_LABELS);
        if (busy) ui_button_label(b, "Connecting…");
        gtk_widget_set_sensitive(b, !busy);
        g_object_set_data_full(G_OBJECT(b), "device", g_strdup(d->id), g_free);
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(card), b);
        gtk_box_append(GTK_BOX(v->away), card);
    }
    /* An away card already says no controller is connected. */
    if (v->empty) gtk_widget_set_visible(v->empty, !any);
    /* A box with nothing to show would still take the page's gap. */
    gtk_widget_set_visible(v->away, any);
    gtk_widget_set_visible(v->cards, pads_list(v->app->pads)->len > 0 || !any);
}

/* --------------------------------------------------------------- pairing */

static void on_pair_found(GtkButton *b, PadView *v)
{
    padpair_pair(v->pair, g_object_get_data(G_OBJECT(b), "path"));
}

static void show_pairing(gpointer data)
{
    PadView *v = data;
    const char *action = padpair_action(v->pair);
    ui_button_label(v->pair_action, action ? action : "Pairing…");
    gtk_widget_set_sensitive(v->pair_action, action != NULL);
    char *status = padpair_status(v->pair);
    gtk_label_set_text(GTK_LABEL(v->pair_status), status ? status : "Hold the pair button on top of the controller "
                                                                    "until the Xbox button flashes quickly.");
    g_free(status);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(v->pair_found))) gtk_box_remove(GTK_BOX(v->pair_found), c);
    GPtrArray *found = padpair_state(v->pair) == PAIR_SEARCHING ? padpair_found(v->pair) : NULL;
    gtk_widget_set_visible(v->pair_found, found && found->len > 0);
    for (guint i = 0; found && i < found->len; i++) {
        PairFound *f = found->pdata[i];
        GtkWidget *b = button(f->name, "chip", G_CALLBACK(on_pair_found), v);
        g_object_set_data_full(G_OBJECT(b), "path", g_strdup(f->path), g_free);
        lares_wrap_append(LARES_WRAP(v->pair_found), b);
    }
}

static void on_pair_action(GtkButton *b, PadView *v)
{
    PairState s = padpair_state(v->pair);
    if (s == PAIR_STARTING || s == PAIR_SEARCHING) padpair_cancel(v->pair);
    else padpair_search(v->pair);
}

/* -------------------------------------------------------------- settings */

static void save(PadView *v)
{
    padconf_save(v->app->padconf);
    pad_service_sync(prefs(v));
}

static void show_settings(PadView *v);

static void on_switch(GObject *sw, GParamSpec *ps, PadView *v)
{
    if (v->syncing) return;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    gboolean *field = g_object_get_data(sw, "field");
    *field = on;
    save(v);
    show_settings(v);
}

/* The cards change colour in place: making them again would reset them. */
static void on_level(GtkButton *b, PadView *v)
{
    int step = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "step"));
    prefs(v)->low_level = CLAMP(prefs(v)->low_level + step, 5, 50);
    save(v);
    show_settings(v);
    for (guint i = 0; i < v->card_list->len; i++) {
        Card *c = v->card_list->pdata[i];
        const Pad *pad = pads_find(v->app->pads, c->path);
        if (pad) show_low(c, pad);
    }
}

static void on_minutes(GtkToggleButton *t, PadView *v)
{
    if (v->syncing || !gtk_toggle_button_get_active(t)) return;
    prefs(v)->idle_minutes = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "minutes"));
    save(v);
}

static void on_forget(GtkButton *b, PadView *v)
{
    g_hash_table_remove(prefs(v)->nicknames, g_object_get_data(G_OBJECT(b), "key"));
    padconf_save(v->app->padconf);
}

static GtkWidget *setting(PadView *v, GtkWidget *box, const char *title, const char *sub, gboolean *field,
                          GtkWidget **sw)
{
    GtkWidget *row = ui_setting(box, title, sub);
    if (field) *sw = ui_setting_switch(row, title, field, G_CALLBACK(on_switch), v);
    return row;
}

static void on_open_settings(GtkButton *b, PadView *v)
{
    lares_show(v->app, "settings");
}

static void show_settings(PadView *v)
{
    PadPrefs *p = prefs(v);
    v->syncing = TRUE;
    gtk_switch_set_active(GTK_SWITCH(v->sw_alerts), p->alerts);
    gtk_switch_set_active(GTK_SWITCH(v->sw_notices), p->notices);
    gtk_switch_set_active(GTK_SWITCH(v->sw_awake), p->keep_awake);
    gtk_switch_set_active(GTK_SWITCH(v->sw_idle), p->idle_disconnect);
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", p->low_level);
    gtk_label_set_text(GTK_LABEL(v->level_text), buf);
    int nearest = 0;
    for (int i = 0; i < PAD_IDLE_CHOICES; i++)
        if (abs(PAD_IDLE_MINUTES[i] - p->idle_minutes) < abs(PAD_IDLE_MINUTES[nearest] - p->idle_minutes)) nearest = i;
    int i = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(v->idle_chips); c; c = gtk_widget_get_next_sibling(c), i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), i == nearest);
    gtk_widget_set_visible(v->idle_chips, p->idle_disconnect);
    /* Without the helper, these can't work; they wait until it's back. */
    gtk_widget_set_sensitive(v->depends, p->background);
    gtk_widget_set_visible(v->helper_off, !p->background);
    v->syncing = FALSE;

    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(v->nicknames))) gtk_box_remove(GTK_BOX(v->nicknames), c);
    GHashTableIter it;
    gpointer k, val;
    g_hash_table_iter_init(&it, p->nicknames);
    while (g_hash_table_iter_next(&it, &k, &val)) {
        char **parts = g_strsplit(val, "\t", 2);
        char *sub = parts[1] ? g_strdup(parts[1]) : NULL;
        GtkWidget *row = setting(v, v->nicknames, parts[0], sub, NULL, NULL);
        GtkWidget *b = button("Forget", "quiet", G_CALLBACK(on_forget), v);
        g_object_set_data_full(G_OBJECT(b), "key", g_strdup(k), g_free);
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), b);
        g_free(sub);
        g_strfreev(parts);
    }
    GtkWidget *names_group = gtk_widget_get_parent(gtk_widget_get_parent(v->nicknames));
    gtk_widget_set_visible(names_group, g_hash_table_size(p->nicknames) > 0);
}

static void build_settings(PadView *v, GtkWidget *page)
{
    PadPrefs *p = prefs(v);
    /* Says why the settings below are greyed out, and where to change it. */
    v->helper_off = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(v->helper_off, "card");
    gtk_widget_add_css_class(v->helper_off, "padded");
    GtkWidget *off = setting(v, v->helper_off, "Background is off", "Alerts and play features need it.", NULL, NULL);
    GtkWidget *open = button("Open Settings", "quiet", G_CALLBACK(on_open_settings), v);
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(off), open);
    gtk_box_append(GTK_BOX(page), v->helper_off);

    v->depends = gtk_box_new(GTK_ORIENTATION_VERTICAL, 36);
    gtk_box_append(GTK_BOX(page), v->depends);

    GtkWidget *alerts = ui_group(v->depends, "Alerts");
    setting(v, alerts, "Low battery alerts", "Notifies when low, and again at 10%.", &p->alerts, &v->sw_alerts);
    GtkWidget *level = setting(v, alerts, "Low battery level", "The battery turns orange at this level.", NULL, NULL);
    GtkWidget *minus = button("−", "quiet", G_CALLBACK(on_level), v), *plus = button("+", "quiet", G_CALLBACK(on_level), v);
    g_object_set_data(G_OBJECT(minus), "step", GINT_TO_POINTER(-5));
    g_object_set_data(G_OBJECT(plus), "step", GINT_TO_POINTER(5));
    gtk_accessible_update_property(GTK_ACCESSIBLE(minus), GTK_ACCESSIBLE_PROPERTY_LABEL, "Lower", -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(plus), GTK_ACCESSIBLE_PROPERTY_LABEL, "Higher", -1);
    v->level_text = ui_label("", "value");
    gtk_widget_set_size_request(v->level_text, 54, -1);
    gtk_label_set_xalign(GTK_LABEL(v->level_text), 0.5);
    for (GtkWidget *w = minus; w; w = w == minus ? v->level_text : w == v->level_text ? plus : NULL) {
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(level), w);
    }
    setting(v, alerts, "Connection notices", "Notifies when a controller connects or leaves.", &p->notices,
            &v->sw_notices);

    GtkWidget *play = ui_group(v->depends, "While you play");
    setting(v, play, "Keep the screen awake", "No dimming or locking while you play.", &p->keep_awake, &v->sw_awake);
    setting(v, play, "Disconnect idle controllers", "Saves controller battery. They stay paired.",
            &p->idle_disconnect, &v->sw_idle);
    v->idle_chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *first = NULL;
    for (int i = 0; i < PAD_IDLE_CHOICES; i++) {
        char text[16];
        snprintf(text, sizeof text, "%d min", PAD_IDLE_MINUTES[i]);
        GtkWidget *t = gtk_toggle_button_new_with_label(text);
        gtk_widget_add_css_class(t, "chip");
        gtk_widget_set_cursor_from_name(t, "pointer");
        g_object_set_data(G_OBJECT(t), "minutes", GINT_TO_POINTER(PAD_IDLE_MINUTES[i]));
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(t), GTK_TOGGLE_BUTTON(first));
        else first = t;
        g_signal_connect(t, "toggled", G_CALLBACK(on_minutes), v);
        gtk_box_append(GTK_BOX(v->idle_chips), t);
    }
    gtk_box_append(GTK_BOX(play), v->idle_chips);

    GtkWidget *names = ui_group(page, "Nicknames");
    v->nicknames = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(names), v->nicknames);
}

/* ---------------------------------------------------------------- build */

static void users_found(const char *path, GPtrArray *names, gpointer data)
{
    PadView *v = data;
    if (names->len) {
        GString *s = g_string_new("In use by ");
        for (guint i = 0; i < names->len; i++)
            g_string_append_printf(s, "%s%s", i ? (i + 1 == names->len ? " and " : ", ") : "", (char *)names->pdata[i]);
        g_hash_table_insert(v->users, g_strdup(path), g_string_free(s, FALSE));
    } else {
        g_hash_table_remove(v->users, path);
    }
    for (guint i = 0; i < v->card_list->len; i++) {
        Card *c = v->card_list->pdata[i];
        if (!g_str_equal(c->path, path)) continue;
        const char *text = g_hash_table_lookup(v->users, path);
        gtk_label_set_text(GTK_LABEL(c->users), text ? text : "");
        gtk_widget_set_visible(c->users, text != NULL);
    }
}

/* Which programs have each controller open, worked out when the section
 * opens or the controllers change. */
static void find_users(PadView *v)
{
    GPtrArray *list = pads_list(v->app->pads);
    for (guint i = 0; i < list->len; i++) pad_find_users(list->pdata[i], users_found, v);
}

/* What the cards show; when it's the same, nothing is made again. A
 * nickname and the low battery level change in place, as the cards' own
 * controls change them, and making the cards again would reset those. */
static char *shape_of(PadView *v)
{
    GString *s = g_string_new(NULL);
    GPtrArray *list = pads_list(v->app->pads);
    for (guint i = 0; i < list->len; i++) {
        Pad *p = list->pdata[i];
        g_string_append_printf(s, "%s|%d|%.0f|%u|%u;", p->path, p->battery.known, p->battery.percentage,
                               p->battery.state, p->battery.level);
    }
    return g_string_free(s, FALSE);
}

static void rebuild(gpointer data)
{
    PadView *v = data;
    char *shape = shape_of(v);
    if (v->shape && g_str_equal(shape, v->shape)) {
        g_free(shape);
        for (guint i = 0; i < v->card_list->len; i++) {
            Card *c = v->card_list->pdata[i];
            const Pad *pad = pads_find(v->app->pads, c->path);
            if (pad) show_name(c, pad);
        }
        return;
    }
    g_free(v->shape);
    v->shape = shape;

    GPtrArray *list = pads_list(v->app->pads);
    /* Forget what was kept for controllers that left. */
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, v->memory);
    while (g_hash_table_iter_next(&it, &k, NULL)) if (!pads_find(v->app->pads, k)) g_hash_table_iter_remove(&it);
    g_ptr_array_set_size(v->card_list, 0);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(v->cards))) gtk_box_remove(GTK_BOX(v->cards), c);
    v->empty = NULL;
    for (guint i = 0; i < list->len; i++) gtk_box_append(GTK_BOX(v->cards), pad_card(v, list->pdata[i]));
    if (!list->len) {
        v->empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        gtk_widget_add_css_class(v->empty, "card");
        gtk_widget_add_css_class(v->empty, "padded");
        gtk_box_append(GTK_BOX(v->empty), lares_pad_picture_new(TRUE));
        GtkWidget *hint = ui_paragraph("No controller connected. Plug in a USB cable, or connect one over Bluetooth.", "muted");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.5);
        gtk_box_append(GTK_BOX(v->empty), hint);
        gtk_box_append(GTK_BOX(v->cards), v->empty);
    }
    build_away(v);
    update_readers(v);
    if (v->on_screen) find_users(v);
}

static void on_devices(gpointer data)
{
    build_away(data);
}

static void on_conf(gpointer data)
{
    PadView *v = data;
    show_settings(v);
}

GtkWidget *pad_view_build(Lares *app, Section *self)
{
    PadView *v = g_new0(PadView, 1);
    v->app = app;
    self->view = v;
    v->card_list = g_ptr_array_new_with_free_func(card_free);
    v->memory = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    v->users = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    v->page = ui_page();
    v->cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(v->page), v->cards);
    v->away = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(v->page), v->away);

    /* Pair a controller */
    v->pair = padpair_new(show_pairing, v);
    v->pair_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(v->pair_card, "card");
    gtk_widget_add_css_class(v->pair_card, "padded");
    /* The hint and the button say what this card is for: the hint across
     * the card, so it doesn't wrap into a narrow column beside the button.
     * The hint keeps the height of its longest, and the controllers found
     * come under the button, so the button stays put while pairing goes on. */
    v->pair_status = ui_paragraph("", "muted");
    gtk_widget_add_css_class(v->pair_status, "pair-status");
    gtk_box_append(GTK_BOX(v->pair_card), v->pair_status);
    v->pair_action = button("Pair a controller", "primary", G_CALLBACK(on_pair_action), v);
    ui_button_labels(v->pair_action, PAIR_LABELS);
    gtk_widget_set_halign(v->pair_action, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(v->pair_card), v->pair_action);
    v->pair_found = lares_wrap_new(9);
    gtk_box_append(GTK_BOX(v->pair_card), v->pair_found);
    gtk_box_append(GTK_BOX(v->page), v->pair_card);
    show_pairing(v);

    build_settings(v, v->page);
    show_settings(v);
    pads_watch(app->pads, rebuild, v);
    devices_watch(app->devices, on_devices, v);
    padconf_watch(app->padconf, on_conf, v);
    rebuild(v);
    return v->page;
}

void pad_view_shown(Lares *app, Section *self, gboolean on)
{
    PadView *v = self->view;
    v->on_screen = on;
    update_readers(v);
    if (on) find_users(v);
    else padpair_hidden(v->pair);
}
