/* Overview: everything at a glance, each part opening its own section. */
#include <math.h>
#include "lares.h"

typedef struct {
    GtkWidget *button, *value, *sub, *meter;
} Tile;

typedef struct {
    Lares *app;
    Tile cpu, gpu, mem, disk, remote;
    GtkWidget *devices;      /* the card holding one row per device */
} Overview;

static void open_section(GtkButton *b, Lares *app)
{
    lares_show(app, g_object_get_data(G_OBJECT(b), "section"));
}

static GtkButton *clickable(Lares *app, const char *section, const char *css)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, css);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_object_set_data(G_OBJECT(b), "section", (gpointer)section);
    g_signal_connect(b, "clicked", G_CALLBACK(open_section), app);
    return GTK_BUTTON(b);
}

/* A tile: a small heading with its icon, one big number, a line under it
 * and, for loads and space, a meter of that many blocks. */
static void make_tile(Overview *o, Tile *t, const char *section, const char *const *art,
                      const char *heading, int meter_blocks)
{
    t->button = GTK_WIDGET(clickable(o->app, section, "tile"));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_box_append(GTK_BOX(head), ui_icon(art, 2, "tile-icon"));
    gtk_box_append(GTK_BOX(head), ui_label(heading, "muted"));
    gtk_box_append(GTK_BOX(box), head);
    t->value = ui_label("…", "big");
    gtk_box_append(GTK_BOX(box), t->value);
    t->sub = ui_label("", "muted");
    gtk_label_set_ellipsize(GTK_LABEL(t->sub), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), t->sub);
    if (meter_blocks) {
        t->meter = lares_meter_new(meter_blocks, 2);
        gtk_box_append(GTK_BOX(box), t->meter);
    }
    gtk_button_set_child(GTK_BUTTON(t->button), box);
}

static void set_text(GtkWidget *label, const char *text)
{
    /* Setting the same text would still relayout; skip it. */
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0)
        gtk_label_set_text(GTK_LABEL(label), text);
}

static void set_percent(Tile *t, double fraction)
{
    char buf[16];
    if (fraction < 0) set_text(t->value, "…");
    else { snprintf(buf, sizeof buf, "%d%%", (int)lround(fraction * 100)); set_text(t->value, buf); }
    lares_meter_set(LARES_METER(t->meter), fraction);
    ui_class(t->meter, "hot", fraction > 0.9);
}

static void set_temperature(GtkWidget *label, double celsius, const char *otherwise)
{
    char buf[32];
    if (isnan(celsius)) set_text(label, otherwise);
    else { snprintf(buf, sizeof buf, "%d °C", (int)lround(celsius)); set_text(label, buf); }
}

static void on_hardware(gpointer user)
{
    Overview *o = user;
    const HwSample *s = hardware_sample(o->app->hw);
    set_percent(&o->cpu, s->cpu);
    set_temperature(o->cpu.sub, s->cpu_temp, "Load");
    if (o->gpu.button) {
        set_percent(&o->gpu, s->gpu);
        set_temperature(o->gpu.sub, s->gpu_temp, "Load");
    }
    char *used = format_bytes(s->mem_used), *total = format_bytes(s->mem_total);
    char *of = g_strdup_printf("of %s", total);
    set_text(o->mem.value, used);
    set_text(o->mem.sub, of);
    lares_meter_set(LARES_METER(o->mem.meter), s->mem_total ? (double)s->mem_used / s->mem_total : -1);
    g_free(used); g_free(total); g_free(of);

    /* The room left, with the meter still filling as the drive does. Space
     * only: the drive's temperature costs a command to the drive. */
    double full = s->disk_total ? (double)s->disk_used / s->disk_total : -1;
    char *left = format_bytes(s->disk_total - s->disk_used);
    char *free_text = g_strdup_printf("%s free", left);
    total = format_bytes(s->disk_total);
    of = g_strdup_printf("of %s", total);
    set_text(o->disk.value, s->disk_total ? free_text : "…");
    set_text(o->disk.sub, s->disk_total ? of : "");
    lares_meter_set(LARES_METER(o->disk.meter), full);
    ui_class(o->disk.meter, "hot", full > 0.9);
    g_free(left); g_free(free_text); g_free(total); g_free(of);
}

/* Every kind of device has its own section. */
static const char *section_for(DevKind kind)
{
    switch (kind) {
    case DEV_CONTROLLER: return "controller";
    case DEV_AUDIO:      return "headphones";
    case DEV_KEYBOARD:   return "keyboard";
    default:             return "mouse";
    }
}

/* One row per device: what it is, whether it's there, and its battery.
 * Clicking it opens the device's section. */
static void on_devices(gpointer user)
{
    Overview *o = user;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(o->devices)))
        gtk_box_remove(GTK_BOX(o->devices), c);

    GPtrArray *list = devices_list(o->app->devices);
    for (guint i = 0; i < list->len; i++) {
        Device *d = list->pdata[i];
        GtkButton *row = clickable(o->app, section_for(d->kind), "row-head");
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        gtk_box_append(GTK_BOX(line), ui_icon(device_art(d->kind), 2, "row-icon"));
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(texts, TRUE);
        GtkWidget *name = ui_label(d->name, NULL);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(texts), name);
        if (!d->connected) gtk_box_append(GTK_BOX(texts), ui_label("Not connected", "muted"));
        gtk_box_append(GTK_BOX(line), texts);
        if (d->connected && d->battery >= 0) {
            char *pct = battery_text(d->battery);
            gtk_box_append(GTK_BOX(line), ui_label(pct, d->battery <= 20 ? "low" : "value"));
            g_free(pct);
            GtkWidget *meter = lares_meter_new(10, 2);
            lares_meter_set(LARES_METER(meter), d->battery / 100.0);
            ui_class(meter, "low", d->battery <= 20);
            gtk_box_append(GTK_BOX(line), meter);
        }
        gtk_button_set_child(row, line);
        if (i > 0) {
            GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(sep, "line");
            gtk_box_append(GTK_BOX(o->devices), sep);
        }
        gtk_box_append(GTK_BOX(o->devices), GTK_WIDGET(row));
    }
    gtk_widget_set_visible(o->devices, list->len > 0);
}

/* "Sunshine is off" when a service isn't as the mode sets it, or NULL. */
static char *differs(Remote *r, const Config *c, Item i, int now)
{
    const char *want = c->values[c->mode][i];
    if (now < 0 || !want || g_str_equal(want, "leave") || remote_matches(r, i, want)) return NULL;
    return g_strdup_printf("%s is %s", item_name(i), now ? "on" : "off");
}

/* The mode, and under it only news: the stream, or a service that isn't as
 * the mode sets it. What the mode sets goes without saying. */
static void on_remote(gpointer user)
{
    Overview *o = user;
    Remote *r = o->app->remote;
    Config *c = remote_config(r);
    Live *l = remote_live(r);
    gtk_widget_set_visible(o->remote.button, c->ready);
    set_text(o->remote.value, c->mode_known ? mode_name(c->mode) : "…");
    char *sub = NULL;
    if (l->streaming) {
        sub = g_strdup("Streaming to Moonlight now");
    } else if (c->mode_known) {
        char *sun = differs(r, c, ITEM_SUNSHINE, l->sunshine), *ts = differs(r, c, ITEM_TAILSCALE, l->tailscale);
        if (sun && ts) sub = g_strdup_printf("%s · %s", sun, ts);
        else if (sun || ts) sub = g_strdup(sun ? sun : ts);
        else if (c->mode == MODE_REMOTE && l->sunshine == 1) sub = g_strdup("Waiting for Moonlight");
        g_free(sun);
        g_free(ts);
    }
    set_text(o->remote.sub, sub ? sub : "");
    g_free(sub);
}

GtkWidget *overview_build(Lares *app, Section *self)
{
    Overview *o = g_new0(Overview, 1);
    o->app = app;
    self->view = o;
    GtkWidget *page = ui_page();

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    make_tile(o, &o->cpu, "hardware", ART_CHIP, "CPU", 15);
    gtk_box_append(GTK_BOX(row), o->cpu.button);
    if (hardware_info(app->hw)->gpu_name) {
        make_tile(o, &o->gpu, "hardware", ART_SCREEN, "GPU", 15);
        gtk_box_append(GTK_BOX(row), o->gpu.button);
    }
    make_tile(o, &o->mem, "hardware", ART_MEMORY, "Memory", 15);
    gtk_box_append(GTK_BOX(row), o->mem.button);
    gtk_box_append(GTK_BOX(page), row);

    /* The system drive spans the width: a fourth tile in the row would
     * leave too little room for its number. */
    make_tile(o, &o->disk, "hardware", ART_DISK, "Storage", 34);
    gtk_box_append(GTK_BOX(page), o->disk.button);

    o->devices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(o->devices, "card");
    gtk_box_append(GTK_BOX(page), o->devices);

    /* The Remote tile shows once remote play is set up. */
    make_tile(o, &o->remote, "remote", ART_LAPTOP, "Mode", 0);
    gtk_box_append(GTK_BOX(page), o->remote.button);
    remote_watch(app->remote, on_remote, o);
    on_remote(o);

    hardware_watch(app->hw, on_hardware, o);
    devices_watch(app->devices, on_devices, o);
    on_hardware(o);
    on_devices(o);
    return page;
}

void overview_shown(Lares *app, Section *self, gboolean on)
{
    hardware_want(app->hw, on);
    devices_want_batteries(app->devices, on);
    if (remote_config(app->remote)->ready) remote_want(app->remote, on);
}
