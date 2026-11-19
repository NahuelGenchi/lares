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

typedef enum { PICK_STAGE, PICK_RATE, PICK_LIFT, PICK_LIGHT, PICK_COLOUR } Pick;

/* A card's widgets, kept so news from the device updates them in place:
 * making the card again would lose the keyboard focus and the scroll
 * position. It's made again only when its layout changes (its `shape`). */
typedef struct {
    char *id, *shape;
    GtkWidget *root, *status, *battery, *battery_text, *connection, *error;
    GtkWidget *waiting;            /* until the mouse's settings are known */
    GtkWidget *dpi, *minus, *plus, *light_slot, *light_note, *colours, *bright, *bright_text;
    GPtrArray *rows;               /* the settings, greyed out while the mouse sleeps */
    GPtrArray *chips;              /* every choice, with its Pick and value */
} Card;

typedef struct {
    Lares *app;
    DevKind kind;
    GtkWidget *page;
    GPtrArray *cards;
    char *busy_id;       /* the device being connected, disconnected or allowed */
    char *error_id, *error;
    guint update_idle;
    gboolean syncing;    /* the view sets its own controls; their handlers stay out */
    /* A mouse's settings as the person sets them: shown at once, and
     * written one change at a time, the latest last. Only what differs
     * from `base`, what the mouse held when they began, is written. */
    char *want_id;
    MouseSettings base, want;
    gboolean pending;    /* `want` waits to be written */
    gboolean writing;
    guint settle;
} DeviceView;

static void update(gpointer user);
static void commit(DeviceView *v);

static void card_free(gpointer p)
{
    Card *c = p;
    g_free(c->id);
    g_free(c->shape);
    g_ptr_array_unref(c->rows);
    g_ptr_array_unref(c->chips);
    g_free(c);
}

static gboolean update_now(gpointer user)
{
    DeviceView *v = user;
    v->update_idle = 0;
    update(v);
    return G_SOURCE_REMOVE;
}

/* From a control's own handler, the cards follow once it returns. */
static void update_soon(DeviceView *v)
{
    if (!v->update_idle) v->update_idle = g_idle_add(update_now, v);
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
    update(v);
}

static void on_button(GtkButton *b, DeviceView *v)
{
    if (v->busy_id) return;
    const char *id = g_object_get_data(G_OBJECT(b), "device");
    gboolean allow = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "allow"));
    gboolean on = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "connect"));
    v->busy_id = g_strdup(id);
    set_error(v, NULL, NULL);
    update_soon(v);
    if (allow) devices_allow_batteries(v->app->devices, action_done, v);
    else devices_set_connected(v->app->devices, id, on, action_done, v);
}

/* Connect or Disconnect for a Bluetooth device, Allow for a battery that
 * needs permission; a spinner while it works, in the same space. */
static GtkWidget *action_button(DeviceView *v, Device *d, gboolean busy)
{
    gboolean allow = !d->bluetooth;
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, allow || !d->connected ? "primary" : "quiet");
    gtk_widget_add_css_class(b, "action"); /* one width, with a label or the spinner */
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    gtk_button_set_child(GTK_BUTTON(b), ui_busy_child(allow ? "Allow" : d->connected ? "Disconnect" : "Connect", busy));
    gtk_widget_set_sensitive(b, !busy);
    g_object_set_data_full(G_OBJECT(b), "device", g_strdup(d->id), g_free);
    g_object_set_data(G_OBJECT(b), "allow", GINT_TO_POINTER(allow));
    g_object_set_data(G_OBJECT(b), "connect", GINT_TO_POINTER(!d->connected));
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

/* A row of the card: a muted title, and its value on the right, which it
 * returns; NULL without a value. */
static GtkWidget *detail(GtkWidget *card, const char *title, const char *value)
{
    if (!value) return NULL;
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
    return l;
}

/* ---------------------------------------------------------- settings */

static gboolean has_colour(LightEffect light)
{
    return light == LIGHT_SOLID || light == LIGHT_BREATHING;
}

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
    if (!ok) set_error(v, v->want_id, message);
    if (v->pending) commit(v);
    /* What the mouse holds now comes with Devices' next update. */
    update_soon(v);
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
    devices_set_mouse(v->app->devices, v->want_id, &v->base, &v->want, written, v);
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
    if (v->syncing || !d || !shown(v, d)) return NULL;
    if (!v->want_id || !g_str_equal(v->want_id, d->id) || !(v->pending || v->writing)) {
        if (v->want_id && !g_str_equal(v->want_id, d->id)) commit(v);  /* another mouse's change goes first */
        g_free(v->want_id);
        v->want_id = g_strdup(d->id);
        v->base = v->want = *d->mouse;
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
    update_soon(v);
}

static guint32 rgb_of(const MouseSettings *m)
{
    return (guint32)m->rgb[0] << 16 | m->rgb[1] << 8 | m->rgb[2];
}

static void on_chip(GtkToggleButton *t, DeviceView *v)
{
    MouseSettings *m = gtk_toggle_button_get_active(t) ? wanting(v, GTK_WIDGET(t)) : NULL;
    if (!m) return;
    int value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "value"));
    switch ((Pick)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "pick"))) {
    case PICK_STAGE: m->active = value; break;
    case PICK_RATE:  m->rate = value; break;
    case PICK_LIFT:  m->lift_mm = value; break;
    case PICK_LIGHT:
        m->light = value;
        /* A colour that's off would show nothing: start from red. */
        if (has_colour(m->light) && !rgb_of(m)) m->rgb[0] = 0xff;
        break;
    case PICK_COLOUR:
        if (!has_colour(m->light)) m->light = LIGHT_SOLID;
        m->rgb[0] = (guint32)value >> 16 & 0xff;
        m->rgb[1] = (guint32)value >> 8 & 0xff;
        m->rgb[2] = (guint32)value & 0xff;
        break;
    }
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
}

static void on_brightness(LaresMeter *meter, double fraction, gpointer user)
{
    DeviceView *v = user;
    MouseSettings *m = wanting(v, GTK_WIDGET(meter));
    if (!m) return;
    m->brightness = (int)lround(fraction * 100);
    changed(v, FALSE);
}

/* A setting of the mouse: its title and line, a small control on their
 * right when it has one (`head`), and its choices under them, across the
 * card. */
static GtkWidget *setting_row(Card *c, GtkWidget *card, const char *title, const char *sub, GtkWidget **head)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(row, "setting");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(texts), ui_label(title, NULL));
    if (sub) gtk_box_append(GTK_BOX(texts), ui_paragraph(sub, "muted"));
    gtk_box_append(GTK_BOX(top), texts);
    gtk_box_append(GTK_BOX(row), top);
    gtk_box_append(GTK_BOX(card), row);
    g_ptr_array_add(c->rows, row);
    if (head) *head = top;
    return row;
}

static GtkWidget *chips(GtkWidget *row)
{
    GtkWidget *wrap = lares_wrap_new(9);
    gtk_box_append(GTK_BOX(row), wrap);
    return wrap;
}

static GtkWidget *chip(DeviceView *v, Card *c, GtkWidget *wrap, const char *label, Pick pick, int value)
{
    GtkWidget *first = g_object_get_data(G_OBJECT(wrap), "first");
    GtkWidget *t = gtk_toggle_button_new();
    if (label) gtk_button_set_label(GTK_BUTTON(t), label);
    gtk_widget_add_css_class(t, "chip");
    gtk_widget_set_cursor_from_name(t, "pointer");
    if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(t), GTK_TOGGLE_BUTTON(first));
    else g_object_set_data(G_OBJECT(wrap), "first", t);
    g_object_set_data_full(G_OBJECT(t), "device", g_strdup(c->id), g_free);
    g_object_set_data(G_OBJECT(t), "pick", GINT_TO_POINTER(pick));
    g_object_set_data(G_OBJECT(t), "value", GINT_TO_POINTER(value));
    g_signal_connect(t, "toggled", G_CALLBACK(on_chip), v);
    lares_wrap_append(LARES_WRAP(wrap), t);
    g_ptr_array_add(c->chips, t);
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

static GtkWidget *step_button(DeviceView *v, Card *c, const char *text, int step, const char *name)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(b, "quiet");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_object_set_data_full(G_OBJECT(b), "device", g_strdup(c->id), g_free);
    g_object_set_data(G_OBJECT(b), "step", GINT_TO_POINTER(step));
    gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect(b, "clicked", G_CALLBACK(on_dpi_step), v);
    return b;
}

static void build_settings(DeviceView *v, Card *c, GtkWidget *card, const MouseSettings *m)
{
    GtkWidget *head;
    GtkWidget *row = setting_row(c, card, "DPI", "Its DPI button steps through these.", &head);
    c->minus = step_button(v, c, "−", -DPI_STEP, "Lower DPI");
    c->plus = step_button(v, c, "+", DPI_STEP, "Higher DPI");
    c->dpi = ui_label("", "value");
    gtk_widget_set_size_request(c->dpi, 72, -1);
    gtk_label_set_xalign(GTK_LABEL(c->dpi), 0.5);
    gtk_widget_set_valign(c->dpi, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(head), c->minus);
    gtk_box_append(GTK_BOX(head), c->dpi);
    gtk_box_append(GTK_BOX(head), c->plus);
    GtkWidget *wrap = chips(row);
    for (int i = 0; i < m->stages; i++) chip(v, c, wrap, "", PICK_STAGE, i);

    row = setting_row(c, card, "Polling rate", "How often it reports a move. Higher is smoother, and uses more battery.",
                      NULL);
    wrap = chips(row);
    for (int hz = 125; hz <= m->rate_max; hz *= 2) {
        char label[16];
        snprintf(label, sizeof label, "%d Hz", hz);
        chip(v, c, wrap, label, PICK_RATE, hz);
    }

    row = setting_row(c, card, "Lift-off distance", "How high it can be lifted before it stops following.", NULL);
    wrap = chips(row);
    chip(v, c, wrap, "1 mm", PICK_LIFT, 1);
    chip(v, c, wrap, "2 mm", PICK_LIFT, 2);

    /* No choice here changes the page's length, which would move it when
     * it's scrolled to the end: the colours always show, and picking one
     * picks Solid if the effect has none; brightness dims when the light is
     * off; and the note for an effect another app saved takes the colours'
     * place, in a stack as tall as they are. */
    row = setting_row(c, card, "Lighting", NULL, NULL);
    wrap = chips(row);
    static const char *const EFFECTS[] = { "Off", "Solid", "Breathing", "Colour cycle" };
    for (int e = LIGHT_OFF; e < LIGHT_OTHER; e++) chip(v, c, wrap, EFFECTS[e], PICK_LIGHT, e);
    c->light_slot = gtk_stack_new();
    gtk_box_append(GTK_BOX(row), c->light_slot);
    c->colours = lares_wrap_new(9);
    gtk_stack_add_child(GTK_STACK(c->light_slot), c->colours);
    c->light_note = ui_paragraph("Now an effect another app saved to it.", "muted");
    gtk_widget_set_valign(c->light_note, GTK_ALIGN_START);
    gtk_stack_add_child(GTK_STACK(c->light_slot), c->light_note);
    for (guint i = 0; i < G_N_ELEMENTS(COLOURS); i++) {
        GtkWidget *t = chip(v, c, c->colours, NULL, PICK_COLOUR, (int)COLOURS[i].rgb);
        GtkWidget *square = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(square), 18);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(square), 18);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(square), draw_swatch, GUINT_TO_POINTER(COLOURS[i].rgb), NULL);
        gtk_button_set_child(GTK_BUTTON(t), square);
        gtk_widget_add_css_class(t, "swatch");
        gtk_widget_set_tooltip_text(t, COLOURS[i].name);
        gtk_accessible_update_property(GTK_ACCESSIBLE(t), GTK_ACCESSIBLE_PROPERTY_LABEL, COLOURS[i].name, -1);
    }

    setting_row(c, card, "Brightness", NULL, &head);
    c->bright = lares_meter_new(10, 3);
    lares_meter_set_interactive(LARES_METER(c->bright), on_brightness, v);
    gtk_accessible_update_property(GTK_ACCESSIBLE(c->bright), GTK_ACCESSIBLE_PROPERTY_LABEL, "Brightness", -1);
    g_object_set_data_full(G_OBJECT(c->bright), "device", g_strdup(c->id), g_free);
    c->bright_text = ui_label("", "value");
    gtk_widget_set_size_request(c->bright_text, 54, -1);
    gtk_label_set_xalign(GTK_LABEL(c->bright_text), 1);
    gtk_widget_set_valign(c->bright_text, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(head), c->bright);
    gtk_box_append(GTK_BOX(head), c->bright_text);
}

/* -------------------------------------------------------------- cards */

static gboolean settings_shown(Device *d)
{
    return d->connected && d->has_settings && !d->battery_locked;
}

/* What a card's widgets are made for. Everything else is updated in place. */
static char *shape_of(DeviceView *v, Device *d)
{
    gboolean busy = v->busy_id && g_str_equal(v->busy_id, d->id);
    const MouseSettings *m = settings_shown(d) ? shown(v, d) : NULL;
    return g_strdup_printf("%s\n%s\n%s\n%d %d %d %d %d %d %d %d %d", d->id, d->name, d->model ? d->model : "",
                           d->link, d->bluetooth, d->connected, d->connected && d->battery >= 0, d->battery_locked,
                           busy, d->has_settings, m ? m->stages : -1, m ? m->rate_max : 0);
}

static Card *build_card(DeviceView *v, Device *d)
{
    Card *c = g_new0(Card, 1);
    c->id = g_strdup(d->id);
    c->shape = shape_of(v, d);
    c->rows = g_ptr_array_new();
    c->chips = g_ptr_array_new();
    GtkWidget *card = c->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
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
    /* Connected goes without saying: the icon lights up, and a Bluetooth one
     * offers Disconnect. The line shows only when there's more to say, and
     * never comes or goes with a click: a button that works shows a spinner
     * instead, and Connecting… takes the place of Not connected. */
    c->status = ui_label("", "muted");
    gtk_box_append(GTK_BOX(texts), c->status);

    if (d->connected && d->battery >= 0) {
        GtkWidget *bat = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        c->battery = lares_meter_new(10, 2);
        gtk_box_append(GTK_BOX(bat), c->battery);
        c->battery_text = ui_label("", NULL);
        gtk_box_append(GTK_BOX(bat), c->battery_text);
        gtk_box_append(GTK_BOX(texts), bat);
    }
    gboolean busy = v->busy_id && g_str_equal(v->busy_id, d->id);
    /* Allow sits under what it allows, which leaves the name its room. The
     * hint stays while it waits, so Allow doesn't move. */
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

    detail(card, "Model", d->model);
    c->connection = detail(card, "Connection", link_text(d->link));
    if (settings_shown(d)) {
        const MouseSettings *m = shown(v, d);
        if (m) {
            build_settings(v, c, card, m);
        } else {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class(row, "setting");
            c->waiting = ui_paragraph("", "muted");
            gtk_box_append(GTK_BOX(row), c->waiting);
            gtk_box_append(GTK_BOX(card), row);
        }
    }
    c->error = ui_paragraph("", "bad");
    gtk_box_append(GTK_BOX(card), c->error);
    return c;
}

static void set_text(GtkWidget *label, const char *text)
{
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0) gtk_label_set_text(GTK_LABEL(label), text);
}

static void set_number(GtkWidget *label, const char *format, int n)
{
    char text[32];
    snprintf(text, sizeof text, format, n);
    set_text(label, text);
}

static gboolean chip_on(GtkWidget *t, const MouseSettings *m)
{
    int value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "value"));
    switch ((Pick)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "pick"))) {
    case PICK_STAGE:  return value == m->active;
    case PICK_RATE:   return value == m->rate;
    case PICK_LIFT:   return value == m->lift_mm;
    case PICK_LIGHT:  return value == (int)m->light;
    case PICK_COLOUR: return has_colour(m->light) && (guint32)value == rgb_of(m);
    }
    return FALSE;
}

static void update_settings(Card *c, Device *d, const MouseSettings *m)
{
    for (guint i = 0; i < c->rows->len; i++) gtk_widget_set_sensitive(c->rows->pdata[i], !d->asleep);
    set_number(c->dpi, "%d", m->dpi[m->active]);
    gtk_widget_set_sensitive(c->minus, m->dpi[m->active] > m->dpi_min);
    gtk_widget_set_sensitive(c->plus, m->dpi[m->active] < m->dpi_max);
    gtk_stack_set_visible_child(GTK_STACK(c->light_slot), m->light == LIGHT_OTHER ? c->light_note : c->colours);
    gtk_widget_set_sensitive(c->bright, m->light != LIGHT_OFF);
    gtk_widget_set_sensitive(c->bright_text, m->light != LIGHT_OFF);
    lares_meter_set(LARES_METER(c->bright), m->brightness / 100.0);
    set_number(c->bright_text, "%d%%", m->brightness);
    /* The chips that are on first, which turns the others in their groups
     * off; then any still on, for a group with none on. */
    for (int pass = 0; pass < 2; pass++) {
        for (guint i = 0; i < c->chips->len; i++) {
            GtkWidget *t = c->chips->pdata[i];
            gboolean on = chip_on(t, m);
            if (pass == 0 && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "pick")) == PICK_STAGE) {
                char label[16];
                snprintf(label, sizeof label, "%d", m->dpi[GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "value"))]);
                if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(t)), label) != 0) gtk_button_set_label(GTK_BUTTON(t), label);
            }
            if (on == (pass == 0) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(t)) != on)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), on);
        }
    }
}

static void update_card(DeviceView *v, Card *c, Device *d)
{
    v->syncing = TRUE;
    gboolean busy = v->busy_id && g_str_equal(v->busy_id, d->id);
    const char *status = !d->connected ? (busy ? "Connecting…" : "Not connected") : d->charging ? "Charging" : "";
    set_text(c->status, status);
    gtk_widget_set_visible(c->status, *status != '\0');
    if (c->battery) {
        lares_meter_set(LARES_METER(c->battery), d->battery / 100.0);
        ui_class(c->battery, "low", d->battery <= 20);
        char *pct = battery_text(d->battery);
        set_text(c->battery_text, pct);
        g_free(pct);
        ui_class(c->battery_text, "low", d->battery <= 20);
        ui_class(c->battery_text, "value", d->battery > 20);
    }
    /* Asleep shows on the connection's line, which is always there: a note
     * that came and went as the mouse woke would move the page. */
    if (c->connection) {
        char *link = d->has_settings && d->asleep ? g_strdup_printf("%s, asleep", link_text(d->link))
                                                  : g_strdup(link_text(d->link));
        set_text(c->connection, link);
        g_free(link);
    }
    if (c->waiting)
        set_text(c->waiting, d->asleep ? "Move the mouse to wake it, and its settings show here." : "Reading its settings…");
    const MouseSettings *m = settings_shown(d) ? shown(v, d) : NULL;
    if (m && c->dpi) update_settings(c, d, m);
    gboolean failed = v->error_id && g_str_equal(v->error_id, d->id);
    set_text(c->error, failed ? v->error : "");
    gtk_widget_set_visible(c->error, failed);
    v->syncing = FALSE;
}

/* Devices tell the view of every change. Cards whose layout still fits are
 * updated; otherwise they're all made again, at the same scroll position. */
static void update(gpointer user)
{
    DeviceView *v = user;
    GPtrArray *list = devices_list(v->app->devices), *mine = g_ptr_array_new();
    for (guint i = 0; i < list->len; i++)
        if (((Device *)list->pdata[i])->kind == v->kind) g_ptr_array_add(mine, list->pdata[i]);

    gboolean fits = mine->len == v->cards->len;
    for (guint i = 0; fits && i < mine->len; i++) {
        char *shape = shape_of(v, mine->pdata[i]);
        fits = g_str_equal(shape, ((Card *)v->cards->pdata[i])->shape);
        g_free(shape);
    }
    if (!fits) {
        GtkWidget *scroll = gtk_widget_get_ancestor(v->page, GTK_TYPE_SCROLLED_WINDOW);
        GtkAdjustment *adj = scroll ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)) : NULL;
        double y = adj ? gtk_adjustment_get_value(adj) : 0;
        g_ptr_array_set_size(v->cards, 0);
        GtkWidget *c;
        while ((c = gtk_widget_get_first_child(v->page))) gtk_box_remove(GTK_BOX(v->page), c);
        for (guint i = 0; i < mine->len; i++) {
            Card *card = build_card(v, mine->pdata[i]);
            g_ptr_array_add(v->cards, card);
            gtk_box_append(GTK_BOX(v->page), card->root);
        }
        if (adj) gtk_adjustment_set_value(adj, y);
    }
    for (guint i = 0; i < mine->len; i++) update_card(v, v->cards->pdata[i], mine->pdata[i]);
    g_ptr_array_free(mine, TRUE);
}

GtkWidget *device_build(Lares *app, Section *self)
{
    DeviceView *v = g_new0(DeviceView, 1);
    v->app = app;
    v->kind = self->kind;
    v->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    v->cards = g_ptr_array_new_with_free_func(card_free);
    self->view = v;
    devices_watch(app->devices, update, v);
    update(v);
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
