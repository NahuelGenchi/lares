/* Headphones, Keyboard and Mouse: one card per paired device, with its
 * battery, who made it and how it's connected, and, for Bluetooth ones, a
 * button to connect or disconnect it. AirPods show up in Headphones. A
 * keyboard or mouse plugged in by USB, wired or on its own receiver, has no
 * button, unless its battery needs the person's permission once: then the
 * button allows it. A mouse whose own protocol reports its settings shows
 * them under its details, and changes them. */
#include "lares.h"

#define DPI_STEP  100   /* each − or + */
#define SETTLE_MS 700   /* a stepped DPI or a dragged brightness is written once it stops */

typedef struct {
    Lares *app;
    DevKind kind;
    GtkWidget *page;
    char *busy_id;       /* the device being connected, disconnected or allowed */
    char *error_id, *error;
    guint rebuild_idle;
    /* A mouse's settings as the person sets them: shown at once, and
     * written one change at a time, the latest last. */
    char *want_id;
    MouseSettings want;
    gboolean pending;    /* `want` waits to be written */
    gboolean writing;
    guint settle;
} DeviceView;

static void rebuild(gpointer user);
static void commit(DeviceView *v);

static gboolean rebuild_now(gpointer user)
{
    DeviceView *v = user;
    v->rebuild_idle = 0;
    rebuild(v);
    return G_SOURCE_REMOVE;
}

/* From a control's own handler, the cards are made again once it returns. */
static void rebuild_soon(DeviceView *v)
{
    if (!v->rebuild_idle) v->rebuild_idle = g_idle_add(rebuild_now, v);
}

static Device *find(DeviceView *v, const char *id)
{
    GPtrArray *list = devices_list(v->app->devices);
    for (guint i = 0; id && i < list->len; i++)
        if (g_str_equal(((Device *)list->pdata[i])->id, id)) return list->pdata[i];
    return NULL;
}

static void set_error(DeviceView *v, const char *id, const char *message)
{
    g_free(v->error_id);
    g_free(v->error);
    v->error_id = id ? g_strdup(id) : NULL;
    v->error = id ? g_strdup(message) : NULL;
}

static void action_done(gboolean ok, const char *message, gpointer user)
{
    DeviceView *v = user;
    set_error(v, ok ? NULL : v->busy_id, message);
    g_clear_pointer(&v->busy_id, g_free);
    rebuild(v);
}

static void on_button(GtkButton *b, DeviceView *v)
{
    if (v->busy_id) return;
    const char *id = g_object_get_data(G_OBJECT(b), "device");
    gboolean allow = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "allow"));
    gboolean on = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "connect"));
    v->busy_id = g_strdup(id);
    set_error(v, NULL, NULL);
    rebuild_soon(v);
    if (allow) devices_allow_batteries(v->app->devices, action_done, v);
    else devices_set_connected(v->app->devices, id, on, action_done, v);
}

/* A control keeps its keyboard focus when the cards are made again: it
 * carries a key, and the new one with the same key takes the focus. */
static void set_key(GtkWidget *w, Device *d, const char *what, int value)
{
    g_object_set_data_full(G_OBJECT(w), "focus-key", g_strdup_printf("%s %s %d", d->id, what, value), g_free);
}

static char *focused_key(DeviceView *v)
{
    GtkRoot *root = gtk_widget_get_root(v->page);
    for (GtkWidget *f = root ? gtk_root_get_focus(root) : NULL; f && f != v->page; f = gtk_widget_get_parent(f)) {
        const char *key = g_object_get_data(G_OBJECT(f), "focus-key");
        if (key) return gtk_widget_is_ancestor(f, v->page) ? g_strdup(key) : NULL;
    }
    return NULL;
}

static GtkWidget *find_key(GtkWidget *w, const char *key)
{
    const char *k = g_object_get_data(G_OBJECT(w), "focus-key");
    if (k && g_str_equal(k, key)) return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *found = find_key(c, key);
        if (found) return found;
    }
    return NULL;
}

/* Connect or Disconnect for a Bluetooth device, Allow for a battery that
 * needs permission; a spinner while it works. */
static GtkWidget *action_button(DeviceView *v, Device *d, gboolean busy)
{
    gboolean allow = !d->bluetooth;
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, allow || !d->connected ? "primary" : "quiet");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    if (busy) {
        gtk_button_set_child(GTK_BUTTON(b), lares_pixel_spinner_new(2));
        gtk_widget_set_sensitive(b, FALSE);
    } else {
        gtk_button_set_label(GTK_BUTTON(b), allow ? "Allow" : d->connected ? "Disconnect" : "Connect");
    }
    g_object_set_data_full(G_OBJECT(b), "device", g_strdup(d->id), g_free);
    g_object_set_data(G_OBJECT(b), "allow", GINT_TO_POINTER(allow));
    g_object_set_data(G_OBJECT(b), "connect", GINT_TO_POINTER(!d->connected));
    set_key(b, d, "action", 0);
    g_signal_connect(b, "clicked", G_CALLBACK(on_button), v);
    return b;
}

/* ----------------------------------------------------------- details */

static const char *link_text(DevLink link)
{
    switch (link) {
    case LINK_USB:       return "USB";
    case LINK_RECEIVER:  return "USB receiver";
    case LINK_BLUETOOTH: return "Bluetooth";
    default:             return NULL;
    }
}

/* A row of the card: a muted title, and its value on the right. */
static void detail(GtkWidget *card, const char *title, const char *value)
{
    if (!value) return;
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(row, "setting");
    GtkWidget *t = ui_label(title, "muted");
    gtk_widget_set_hexpand(t, TRUE);
    gtk_widget_set_valign(t, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), t);
    GtkWidget *l = ui_label(value, NULL);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(card), row);
}

/* ---------------------------------------------------------- settings */

/* The settings to show: the person's while they're on their way to the
 * mouse, else what the mouse last said. */
static const MouseSettings *shown(DeviceView *v, Device *d)
{
    gboolean mine = v->want_id && g_str_equal(v->want_id, d->id) && (v->pending || v->writing);
    return mine ? &v->want : d->mouse;
}

static void written(gboolean ok, const char *message, gpointer user)
{
    DeviceView *v = user;
    v->writing = FALSE;
    if (!ok) {
        set_error(v, v->want_id, message);
        rebuild_soon(v);
    }
    if (v->pending) commit(v);
    /* Devices tells the view when the mouse's answer is in its list. */
}

static void commit(DeviceView *v)
{
    if (v->settle) {
        g_source_remove(v->settle);
        v->settle = 0;
    }
    if (v->writing || !v->pending) return;
    v->pending = FALSE;
    v->writing = TRUE;
    devices_set_mouse(v->app->devices, v->want_id, &v->want, written, v);
}

static gboolean settled(gpointer user)
{
    DeviceView *v = user;
    v->settle = 0;
    commit(v);
    return G_SOURCE_REMOVE;
}

/* The settings a change starts from, for the device a control belongs to. */
static MouseSettings *wanting(DeviceView *v, GtkWidget *control)
{
    Device *d = find(v, g_object_get_data(G_OBJECT(control), "device"));
    if (!d || !shown(v, d)) return NULL;
    if (!v->want_id || !g_str_equal(v->want_id, d->id) || !(v->pending || v->writing)) {
        if (v->want_id && !g_str_equal(v->want_id, d->id)) commit(v);  /* another mouse's change goes first */
        g_free(v->want_id);
        v->want_id = g_strdup(d->id);
        v->want = *d->mouse;
    }
    set_error(v, NULL, NULL);
    v->pending = TRUE;
    return &v->want;
}

/* A choice is written at once; steps and drags once they stop. */
static void changed(DeviceView *v, gboolean at_once)
{
    if (at_once) {
        commit(v);
    } else {
        if (v->settle) g_source_remove(v->settle);
        v->settle = g_timeout_add(SETTLE_MS, settled, v);
    }
}

static int chip_value(GtkToggleButton *t)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "value"));
}

static void on_stage(GtkToggleButton *t, DeviceView *v)
{
    MouseSettings *m = gtk_toggle_button_get_active(t) ? wanting(v, GTK_WIDGET(t)) : NULL;
    if (!m) return;
    m->active = chip_value(t);
    changed(v, TRUE);
    rebuild_soon(v);
}

static void on_rate(GtkToggleButton *t, DeviceView *v)
{
    MouseSettings *m = gtk_toggle_button_get_active(t) ? wanting(v, GTK_WIDGET(t)) : NULL;
    if (!m) return;
    m->rate = chip_value(t);
    changed(v, TRUE);
}

static void on_lift(GtkToggleButton *t, DeviceView *v)
{
    MouseSettings *m = gtk_toggle_button_get_active(t) ? wanting(v, GTK_WIDGET(t)) : NULL;
    if (!m) return;
    m->lift_mm = chip_value(t);
    changed(v, TRUE);
}

static void on_light(GtkToggleButton *t, DeviceView *v)
{
    MouseSettings *m = gtk_toggle_button_get_active(t) ? wanting(v, GTK_WIDGET(t)) : NULL;
    if (!m) return;
    m->light = chip_value(t);
    /* A colour that's off, or the previous effect's unknown one, would
     * show nothing: start from red. */
    if ((m->light == LIGHT_SOLID || m->light == LIGHT_BREATHING) && !(m->rgb[0] | m->rgb[1] | m->rgb[2])) {
        m->rgb[0] = 0xff;
        m->rgb[1] = m->rgb[2] = 0;
    }
    changed(v, TRUE);
    rebuild_soon(v);
}

static void on_colour(GtkToggleButton *t, DeviceView *v)
{
    MouseSettings *m = gtk_toggle_button_get_active(t) ? wanting(v, GTK_WIDGET(t)) : NULL;
    if (!m) return;
    guint32 rgb = (guint32)chip_value(t);
    m->rgb[0] = rgb >> 16 & 0xff;
    m->rgb[1] = rgb >> 8 & 0xff;
    m->rgb[2] = rgb & 0xff;
    changed(v, TRUE);
}

static void on_dpi_step(GtkButton *b, DeviceView *v)
{
    MouseSettings *m = wanting(v, GTK_WIDGET(b));
    if (!m) return;
    int step = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "step"));
    int dpi = (m->dpi[m->active] + step) / 50 * 50;
    m->dpi[m->active] = CLAMP(dpi, m->dpi_min, m->dpi_max);
    changed(v, FALSE);
    rebuild_soon(v);
}

/* Dragging rebuilds nothing, so the drag goes on: only its number moves. */
static void on_brightness(LaresMeter *meter, double fraction, gpointer user)
{
    DeviceView *v = user;
    MouseSettings *m = wanting(v, GTK_WIDGET(meter));
    if (!m) return;
    m->brightness = (int)lround(fraction * 100);
    char *text = g_strdup_printf("%d%%", m->brightness);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(meter), "label")), text);
    g_free(text);
    changed(v, FALSE);
}

/* A setting of the mouse: its title and line, a small control on their
 * right when it has one (`head`), and its choices under them, across the
 * card. */
static GtkWidget *setting_row(GtkWidget *card, const char *title, const char *sub, gboolean active, GtkWidget **head)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(row, "setting");
    gtk_widget_set_sensitive(row, active);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(texts), ui_label(title, NULL));
    if (sub) gtk_box_append(GTK_BOX(texts), ui_paragraph(sub, "muted"));
    gtk_box_append(GTK_BOX(top), texts);
    gtk_box_append(GTK_BOX(row), top);
    gtk_box_append(GTK_BOX(card), row);
    if (head) *head = top;
    return row;
}

/* A row of chips under a setting, for one choice among them. */
static GtkWidget *chips(GtkWidget *row)
{
    GtkWidget *wrap = lares_wrap_new(9);
    gtk_box_append(GTK_BOX(row), wrap);
    return wrap;
}

static GtkWidget *chip(DeviceView *v, Device *d, GtkWidget *wrap, const char *label, const char *what, int value,
                       gboolean on, GCallback cb)
{
    GtkWidget *first = g_object_get_data(G_OBJECT(wrap), "first");
    GtkWidget *t = gtk_toggle_button_new();
    if (label) gtk_button_set_label(GTK_BUTTON(t), label);
    gtk_widget_add_css_class(t, "chip");
    gtk_widget_set_cursor_from_name(t, "pointer");
    if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(t), GTK_TOGGLE_BUTTON(first));
    else g_object_set_data(G_OBJECT(wrap), "first", t);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), on);
    g_object_set_data_full(G_OBJECT(t), "device", g_strdup(d->id), g_free);
    g_object_set_data(G_OBJECT(t), "value", GINT_TO_POINTER(value));
    set_key(t, d, what, value);
    g_signal_connect(t, "toggled", cb, v);
    lares_wrap_append(LARES_WRAP(wrap), t);
    return t;
}

static const struct { const char *name; guint32 rgb; } COLOURS[] = {
    { "Red", 0xff0000 }, { "Orange", 0xff5000 }, { "Yellow", 0xffc800 }, { "Green", 0x00ff00 },
    { "Cyan", 0x00ffff }, { "Blue", 0x0000ff }, { "Purple", 0x8000ff }, { "Pink", 0xff00a0 },
    { "White", 0xffffff },
};

/* A square of the colour, edged in the text's colour so white shows on a
 * light card. */
static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    guint32 rgb = GPOINTER_TO_UINT(data);
    GdkRGBA edge;
    gtk_widget_get_color(GTK_WIDGET(area), &edge);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source_rgba(cr, edge.red, edge.green, edge.blue, 0.3);
    cairo_fill(cr);
    cairo_rectangle(cr, 2, 2, w - 4, h - 4);
    cairo_set_source_rgb(cr, (rgb >> 16 & 0xff) / 255.0, (rgb >> 8 & 0xff) / 255.0, (rgb & 0xff) / 255.0);
    cairo_fill(cr);
}

static void colours(DeviceView *v, Device *d, GtkWidget *row, const MouseSettings *m)
{
    GtkWidget *wrap = chips(row);
    guint32 now = (guint32)m->rgb[0] << 16 | m->rgb[1] << 8 | m->rgb[2];
    for (guint i = 0; i < G_N_ELEMENTS(COLOURS); i++) {
        GtkWidget *t = chip(v, d, wrap, NULL, "colour", (int)COLOURS[i].rgb, COLOURS[i].rgb == now, G_CALLBACK(on_colour));
        GtkWidget *square = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(square), 18);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(square), 18);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(square), draw_swatch, GUINT_TO_POINTER(COLOURS[i].rgb), NULL);
        gtk_button_set_child(GTK_BUTTON(t), square);
        gtk_widget_add_css_class(t, "swatch");
        gtk_widget_set_tooltip_text(t, COLOURS[i].name);
        gtk_accessible_update_property(GTK_ACCESSIBLE(t), GTK_ACCESSIBLE_PROPERTY_LABEL, COLOURS[i].name, -1);
    }
}

static GtkWidget *step_button(DeviceView *v, Device *d, const char *text, int step, const char *name)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(b, "quiet");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_object_set_data_full(G_OBJECT(b), "device", g_strdup(d->id), g_free);
    g_object_set_data(G_OBJECT(b), "step", GINT_TO_POINTER(step));
    gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    set_key(b, d, "dpi-step", step);
    g_signal_connect(b, "clicked", G_CALLBACK(on_dpi_step), v);
    return b;
}

static void mouse_settings(DeviceView *v, Device *d, GtkWidget *card)
{
    const MouseSettings *m = shown(v, d);
    if (!m) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(row, "setting");
        gtk_box_append(GTK_BOX(row), ui_paragraph(d->asleep ? "Move the mouse to wake it, and its settings show here."
                                                            : "Reading its settings…", "muted"));
        gtk_box_append(GTK_BOX(card), row);
        return;
    }
    gboolean active = !d->asleep;
    if (d->asleep) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(row, "setting");
        gtk_box_append(GTK_BOX(row), ui_paragraph("It's asleep. Move it to wake it, then change these.", "muted"));
        gtk_box_append(GTK_BOX(card), row);
    }

    GtkWidget *head;
    GtkWidget *row = setting_row(card, "DPI", "Its DPI button steps through these.", active, &head);
    GtkWidget *minus = step_button(v, d, "−", -DPI_STEP, "Lower DPI");
    GtkWidget *plus = step_button(v, d, "+", DPI_STEP, "Higher DPI");
    char *text = g_strdup_printf("%d", m->dpi[m->active]);
    GtkWidget *value = ui_label(text, "value");
    g_free(text);
    gtk_widget_set_size_request(value, 72, -1);
    gtk_label_set_xalign(GTK_LABEL(value), 0.5);
    gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(minus, m->dpi[m->active] > m->dpi_min);
    gtk_widget_set_sensitive(plus, m->dpi[m->active] < m->dpi_max);
    gtk_box_append(GTK_BOX(head), minus);
    gtk_box_append(GTK_BOX(head), value);
    gtk_box_append(GTK_BOX(head), plus);
    GtkWidget *wrap = chips(row);
    for (int i = 0; i < m->stages; i++) {
        char label[16];
        snprintf(label, sizeof label, "%d", m->dpi[i]);
        chip(v, d, wrap, label, "stage", i, i == m->active, G_CALLBACK(on_stage));
    }

    row = setting_row(card, "Polling rate", "How often it reports a move. Higher is smoother, and uses more battery.",
                      active, NULL);
    wrap = chips(row);
    for (int hz = 125; hz <= m->rate_max; hz *= 2) {
        char label[16];
        snprintf(label, sizeof label, "%d Hz", hz);
        chip(v, d, wrap, label, "rate", hz, hz == m->rate, G_CALLBACK(on_rate));
    }

    row = setting_row(card, "Lift-off distance", "How high it can be lifted before it stops following.", active, NULL);
    wrap = chips(row);
    chip(v, d, wrap, "1 mm", "lift", 1, m->lift_mm == 1, G_CALLBACK(on_lift));
    chip(v, d, wrap, "2 mm", "lift", 2, m->lift_mm == 2, G_CALLBACK(on_lift));

    row = setting_row(card, "Lighting", m->light == LIGHT_OTHER ? "Now an effect another app saved to it." : NULL,
                      active, NULL);
    wrap = chips(row);
    static const char *const EFFECTS[] = { "Off", "Solid", "Breathing", "Colour cycle" };
    for (int e = LIGHT_OFF; e < LIGHT_OTHER; e++)
        chip(v, d, wrap, EFFECTS[e], "light", e, (int)m->light == e, G_CALLBACK(on_light));
    if (m->light == LIGHT_SOLID || m->light == LIGHT_BREATHING) colours(v, d, row, m);

    if (m->light != LIGHT_OFF) {
        row = setting_row(card, "Brightness", NULL, active, &head);
        GtkWidget *meter = lares_meter_new(10, 3);
        lares_meter_set(LARES_METER(meter), m->brightness / 100.0);
        lares_meter_set_interactive(LARES_METER(meter), on_brightness, v);
        gtk_accessible_update_property(GTK_ACCESSIBLE(meter), GTK_ACCESSIBLE_PROPERTY_LABEL, "Brightness", -1);
        g_object_set_data_full(G_OBJECT(meter), "device", g_strdup(d->id), g_free);
        set_key(meter, d, "brightness", 0);
        char *pct = g_strdup_printf("%d%%", m->brightness);
        GtkWidget *label = ui_label(pct, "value");
        g_free(pct);
        gtk_widget_set_size_request(label, 54, -1);
        gtk_label_set_xalign(GTK_LABEL(label), 1);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(meter), "label", label);
        gtk_box_append(GTK_BOX(head), meter);
        gtk_box_append(GTK_BOX(head), label);
    }
}

/* -------------------------------------------------------------- cards */

static GtkWidget *device_card(DeviceView *v, Device *d)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "padded");
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(head, "setting");
    gtk_box_append(GTK_BOX(card), head);
    GtkWidget *art = ui_icon(device_art(d->kind), 4, "device-art");
    ui_class(art, "on", d->connected);
    gtk_box_append(GTK_BOX(head), art);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    GtkWidget *name = ui_label(d->name, NULL);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(texts), name);

    gboolean busy = v->busy_id && g_str_equal(v->busy_id, d->id);
    gboolean saving = d->settings_busy || (v->want_id && g_str_equal(v->want_id, d->id) && v->pending);
    const char *status = busy && d->battery_locked ? "Waiting for your password…"
                       : busy ? (d->connected ? "Disconnecting…" : "Connecting…")
                       : !d->connected ? "Not connected"
                       : saving ? "Saving to the mouse…"
                       : d->charging ? "Charging" : "Connected";
    gtk_box_append(GTK_BOX(texts), ui_label(status, "muted"));

    if (d->connected && d->battery >= 0) {
        GtkWidget *bat = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        GtkWidget *meter = lares_meter_new(10, 2);
        lares_meter_set(LARES_METER(meter), d->battery / 100.0);
        ui_class(meter, "low", d->battery <= 20);
        gtk_box_append(GTK_BOX(bat), meter);
        char *pct = battery_text(d->battery);
        gtk_box_append(GTK_BOX(bat), ui_label(pct, d->battery <= 20 ? "low" : "value"));
        g_free(pct);
        gtk_box_append(GTK_BOX(texts), bat);
    }
    /* Allow sits under what it allows, which leaves the name its room. */
    if (d->connected && d->battery_locked) {
        GtkWidget *hint = ui_label(d->has_settings ? "Lares can show its battery and settings once you allow it. "
                                                     "That asks for your password, one time."
                                                   : "Lares can show its battery once you allow it. That asks for "
                                                     "your password, one time.", "muted");
        gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
        gtk_box_append(GTK_BOX(texts), hint);
        GtkWidget *allow = action_button(v, d, busy);
        gtk_widget_set_halign(allow, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(texts), allow);
    }
    gtk_box_append(GTK_BOX(head), texts);
    if (d->bluetooth) gtk_box_append(GTK_BOX(head), action_button(v, d, busy));

    detail(card, "Maker", d->maker);
    detail(card, "Model", d->model);
    detail(card, "Connection", link_text(d->link));
    if (d->connected && d->has_settings && !d->battery_locked) mouse_settings(v, d, card);

    if (v->error_id && g_str_equal(v->error_id, d->id))
        gtk_box_append(GTK_BOX(card), ui_paragraph(v->error, "bad"));
    return card;
}

/* Devices change rarely, so the cards are simply made again. */
static void rebuild(gpointer user)
{
    DeviceView *v = user;
    char *focus = focused_key(v);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(v->page)))
        gtk_box_remove(GTK_BOX(v->page), c);
    GPtrArray *list = devices_list(v->app->devices);
    for (guint i = 0; i < list->len; i++) {
        Device *d = list->pdata[i];
        if (d->kind == v->kind) gtk_box_append(GTK_BOX(v->page), device_card(v, d));
    }
    GtkWidget *again = focus ? find_key(v->page, focus) : NULL;
    if (again) gtk_widget_grab_focus(again);
    g_free(focus);
}

GtkWidget *device_build(Lares *app, Section *self)
{
    DeviceView *v = g_new0(DeviceView, 1);
    v->app = app;
    v->kind = self->kind;
    v->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    self->view = v;
    devices_watch(app->devices, rebuild, v);
    rebuild(v);
    return v->page;
}

/* Devices report changes by themselves, except what only their own
 * protocol reports: batteries are asked for while the section shows, and a
 * mouse's settings while Mouse shows. A change still settling is written
 * when the section goes. */
void device_shown(Lares *app, Section *self, gboolean on)
{
    DeviceView *v = self->view;
    devices_want_batteries(app->devices, on);
    if (self->kind != DEV_MOUSE) return;
    if (!on && v) commit(v);
    devices_want_settings(app->devices, on);
}
