/* Overview: everything at a glance, each part opening its own section. */
#include <math.h>
#include "lares.h"

typedef struct {
    GtkWidget *button, *value, *sub, *meter;
} Tile;

typedef struct {
    Lares *app;
    Tile cpu, gpu, mem, remote;
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
 * and, for loads, a meter. */
static void make_tile(Overview *o, Tile *t, const char *section, const char *const *art,
                      const char *heading, gboolean with_meter)
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
    if (with_meter) {
        t->meter = lares_meter_new(15, 2);
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
}

static const char *section_for(DevKind kind)
{
    return kind == DEV_CONTROLLER ? "controller" : kind == DEV_AUDIO ? "headphones" : NULL;
}

/* One row per device: what it is, whether it's there, and its battery. */
static void on_devices(gpointer user)
{
    Overview *o = user;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(o->devices)))
        gtk_box_remove(GTK_BOX(o->devices), c);

    GPtrArray *list = devices_list(o->app->devices);
    for (guint i = 0; i < list->len; i++) {
        Device *d = list->pdata[i];
        const char *section = section_for(d->kind);
        GtkWidget *row = section ? GTK_WIDGET(clickable(o->app, section, "row-head"))
                                 : gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        if (!section) gtk_widget_add_css_class(row, "row-static");
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
        if (section) gtk_button_set_child(GTK_BUTTON(row), line);
        else gtk_box_append(GTK_BOX(row), line);
        if (i > 0) {
            GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(sep, "line");
            gtk_box_append(GTK_BOX(o->devices), sep);
        }
        gtk_box_append(GTK_BOX(o->devices), row);
    }
    gtk_widget_set_visible(o->devices, list->len > 0);
}

static void on_remote(gpointer user)
{
    Overview *o = user;
    Config *c = remote_config(o->app->remote);
    Live *l = remote_live(o->app->remote);
    gtk_widget_set_visible(o->remote.button, c->ready);
    set_text(o->remote.value, c->mode_known ? mode_name(c->mode) : "…");
    char *sub;
    if (l->streaming) sub = g_strdup("Streaming to Moonlight now");
    else if (l->sunshine < 0) sub = g_strdup("");
    else sub = g_strdup_printf("Sunshine %s · Tailscale %s", l->sunshine ? "on" : "off",
                               l->tailscale ? "on" : "off");
    set_text(o->remote.sub, sub);
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
    make_tile(o, &o->cpu, "hardware", ART_CHIP, "CPU", TRUE);
    gtk_box_append(GTK_BOX(row), o->cpu.button);
    if (hardware_info(app->hw)->gpu_name) {
        make_tile(o, &o->gpu, "hardware", ART_SCREEN, "GPU", TRUE);
        gtk_box_append(GTK_BOX(row), o->gpu.button);
    }
    make_tile(o, &o->mem, "hardware", ART_MEMORY, "Memory", TRUE);
    gtk_box_append(GTK_BOX(row), o->mem.button);
    gtk_box_append(GTK_BOX(page), row);

    o->devices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(o->devices, "card");
    gtk_box_append(GTK_BOX(page), o->devices);

    /* The Remote tile shows once remote play is set up. */
    make_tile(o, &o->remote, "remote", ART_LAPTOP, "Remote", FALSE);
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
    if (remote_config(app->remote)->ready) remote_want(app->remote, on);
}
