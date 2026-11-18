/* Hardware: the processor, graphics, memory and drive, live. */
#include <math.h>
#include "lares.h"

typedef struct {
    GtkWidget *value, *side, *graph, *meter;
    GtkWidget *facts[3];   /* small facts under the graph, each kept whole when they wrap */
} Card;

typedef struct {
    Lares *app;
    Card cpu, gpu, mem, disk;
} HardwareView;

static void set_text(GtkWidget *label, const char *text)
{
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0)
        gtk_label_set_text(GTK_LABEL(label), text);
}

/* A card: its name and a detail on top, a big number with a second one
 * beside it, then a graph or a meter and a line of small facts. */
static GtkWidget *make_card(Card *c, const char *const *art, const char *name, const char *detail,
                            gboolean graph)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "padded");

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_box_append(GTK_BOX(head), ui_icon(art, 2, "row-icon"));
    GtkWidget *title = ui_label(name, NULL);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(head), title);
    if (detail) gtk_box_append(GTK_BOX(head), ui_label(detail, "muted"));
    gtk_box_append(GTK_BOX(card), head);

    GtkWidget *nums = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    c->value = ui_label("…", "big");
    gtk_widget_set_hexpand(c->value, TRUE);
    gtk_box_append(GTK_BOX(nums), c->value);
    c->side = ui_label("", "big-muted");
    gtk_box_append(GTK_BOX(nums), c->side);
    gtk_box_append(GTK_BOX(card), nums);

    if (graph) {
        c->graph = lares_graph_new(3, 54);
        gtk_box_append(GTK_BOX(card), c->graph);
    } else {
        c->meter = lares_meter_new(34, 2);
        gtk_box_append(GTK_BOX(card), c->meter);
    }
    GtkWidget *facts = lares_wrap_new(27);
    for (int i = 0; i < 3; i++) {
        c->facts[i] = ui_label("", "muted");
        lares_wrap_append(LARES_WRAP(facts), c->facts[i]);
    }
    gtk_box_append(GTK_BOX(card), facts);
    return card;
}

static void set_load(Card *c, double fraction, double celsius)
{
    char buf[32];
    if (fraction < 0) set_text(c->value, "…");
    else { snprintf(buf, sizeof buf, "%d%%", (int)lround(fraction * 100)); set_text(c->value, buf); }
    if (isnan(celsius)) set_text(c->side, "");
    else { snprintf(buf, sizeof buf, "%d °C", (int)lround(celsius)); set_text(c->side, buf); }
    if (fraction >= 0) lares_graph_push(LARES_GRAPH(c->graph), fraction);
}

/* Shows an amount, "<what> of <total>" under it, and a meter filling with
 * what is used. */
static void set_space(Card *c, guint64 shown, guint64 used, guint64 total, const char *what)
{
    char *u = format_bytes(shown), *t = format_bytes(total);
    char *foot = g_strdup_printf("%s of %s", what, t);
    set_text(c->value, u);
    set_text(c->facts[0], foot);
    lares_meter_set(LARES_METER(c->meter), total ? (double)used / total : -1);
    ui_class(c->meter, "hot", total && (double)used / total > 0.9);
    g_free(u); g_free(t); g_free(foot);
}

static void on_sample(gpointer user)
{
    HardwareView *v = user;
    const HwSample *s = hardware_sample(v->app->hw);
    set_load(&v->cpu, s->cpu, s->cpu_temp);

    if (v->gpu.value) {
        set_load(&v->gpu, s->gpu, s->gpu_temp);
        char buf[64] = "";
        if (s->vram_total) {
            char *u = format_bytes(s->vram_used), *t = format_bytes(s->vram_total);
            snprintf(buf, sizeof buf, "Video memory %s of %s", u, t);
            g_free(u); g_free(t);
        }
        set_text(v->gpu.facts[0], buf);
        buf[0] = 0;
        if (!isnan(s->gpu_fan_rpm)) snprintf(buf, sizeof buf, "Fan %d rpm", (int)s->gpu_fan_rpm);
        set_text(v->gpu.facts[1], buf);
        buf[0] = 0;
        if (!isnan(s->gpu_watts)) snprintf(buf, sizeof buf, "%d W", (int)lround(s->gpu_watts));
        set_text(v->gpu.facts[2], buf);
    }

    set_space(&v->mem, s->mem_used, s->mem_used, s->mem_total, "Used");
    /* The drive shows the room left. */
    set_space(&v->disk, s->disk_total - s->disk_used, s->disk_used, s->disk_total, "Free");
    char buf[32] = "";
    if (!isnan(s->disk_temp)) snprintf(buf, sizeof buf, "%d °C", (int)lround(s->disk_temp));
    set_text(v->disk.side, buf);
}

GtkWidget *hardware_build(Lares *app, Section *self)
{
    HardwareView *v = g_new0(HardwareView, 1);
    v->app = app;
    self->view = v;
    const HwInfo *info = hardware_info(app->hw);
    GtkWidget *page = ui_page();

    char *threads = g_strdup_printf("%d threads", info->threads);
    gtk_box_append(GTK_BOX(page), make_card(&v->cpu, ART_CHIP, info->cpu_name, threads, TRUE));
    g_free(threads);
    set_text(v->cpu.facts[0], "Load over the last minutes");
    if (info->gpu_name)
        gtk_box_append(GTK_BOX(page), make_card(&v->gpu, ART_SCREEN, info->gpu_name, NULL, TRUE));
    gtk_box_append(GTK_BOX(page), make_card(&v->mem, ART_MEMORY, "Memory", NULL, FALSE));
    gtk_box_append(GTK_BOX(page), make_card(&v->disk, ART_DISK, "System drive", info->disk_kind, FALSE));

    hardware_watch(app->hw, on_sample, v);
    return page;
}

void hardware_shown(Lares *app, Section *self, gboolean on)
{
    hardware_want(app->hw, on);
    hardware_want_details(app->hw, on);
}
