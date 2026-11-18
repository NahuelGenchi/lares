/* The window: a sidebar of sections and the section on screen. */
#include "lares.h"
#include "pad_service.h"

#define CSS_RESOURCE "/com/nahuelgenchi/Lares/style.css"
#define CONTENT_WIDTH 620

static Section SECTIONS[] = {
    { "overview",   "Overview",   ART_OVERVIEW,   0,              overview_build, overview_shown, NULL, NULL, FALSE },
    { "hardware",   "Hardware",   ART_CHIP,       0,              hardware_build, hardware_shown, NULL, NULL, FALSE },
    { "controller", "Controller", ART_GAMEPAD,    DEV_CONTROLLER, pad_view_build, pad_view_shown, NULL, NULL, FALSE },
    { "headphones", "Headphones", ART_HEADPHONES, DEV_AUDIO,      device_build,   device_shown,   NULL, NULL, FALSE },
    { "remote",     "Remote",     ART_LAPTOP,     0,              remote_build,   remote_shown,   NULL, NULL, FALSE },
};

static Section *find(Lares *app, const char *id)
{
    for (int i = 0; i < app->n_sections; i++)
        if (g_str_equal(app->sections[i].id, id)) return &app->sections[i];
    return NULL;
}

static gboolean available(Lares *app, Section *s)
{
    if (s->build == pad_view_build) return devices_any(app->devices, DEV_CONTROLLER) || pads_list(app->pads)->len;
    if (s->build == device_build) return devices_any(app->devices, s->kind);
    return TRUE;
}

/* A section reads only while it is the one shown and the window isn't
 * minimized or hidden. */
static void update_on_screen(Lares *app)
{
    gboolean visible = gtk_widget_get_mapped(app->win) && !gtk_window_is_suspended(GTK_WINDOW(app->win));
    for (int i = 0; i < app->n_sections; i++) {
        Section *s = &app->sections[i];
        gboolean on = visible && s == app->current;
        if (s->on_screen == on) continue;
        s->on_screen = on;
        if (s->shown) s->shown(app, s, on);
    }
}

/* Pages scroll when the window is short, with no scroll bar, and keep a
 * readable width when it's wide. */
static GtkWidget *wrap_page(GtkWidget *page)
{
    gtk_widget_add_css_class(page, "page");
    gtk_widget_set_valign(page, GTK_ALIGN_START);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), lares_clamp_new(page, CONTENT_WIDTH));
    return scroll;
}

void lares_show(Lares *app, const char *id)
{
    Section *s = find(app, id);
    if (!s || s == app->current) return;
    if (!s->page) {
        s->page = wrap_page(s->build(app, s));
        gtk_stack_add_named(GTK_STACK(app->stack), s->page, s->id);
    }
    gboolean first = app->current == NULL;
    app->current = s;
    lares_nav_select(LARES_NAV(app->nav), id, !first);
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), id);
    update_on_screen(app);
}

void lares_set_mode_colors(Lares *app)
{
    Config *c = remote_config(app->remote);
    gboolean remote = c->ready && c->mode_known && c->mode == MODE_REMOTE;
    ui_class(app->win, "remote", remote);
    ui_class(app->win, "home", !remote);
}

static void update_sections(gpointer user)
{
    Lares *app = user;
    /* Once a controller is around, the helper starts or stops to match the
     * background features set for it. Without one, nothing runs. */
    static gboolean synced;
    if (!synced && available(app, find(app, "controller"))) {
        synced = TRUE;
        pad_service_sync(padconf_prefs(app->padconf));
    }
    for (int i = 0; i < app->n_sections; i++) {
        Section *s = &app->sections[i];
        gboolean on = available(app, s);
        lares_nav_set_item_visible(LARES_NAV(app->nav), s->id, on);
        if (!on && s == app->current) lares_show(app, "overview");
    }
}

static void on_nav(LaresNav *nav, const char *id, Lares *app) { lares_show(app, id); }
static void on_window_state(GObject *o, GParamSpec *p, Lares *app) { update_on_screen(app); }
static void on_map_change(GtkWidget *w, Lares *app) { update_on_screen(app); }
/* Remote is a guide until it's set up, and says so in the sidebar. */
static void on_remote_read(gpointer user)
{
    Lares *app = user;
    lares_set_mode_colors(app);
    lares_nav_set_item_label(LARES_NAV(app->nav), "remote",
                             remote_config(app->remote)->ready ? "Remote" : "Set up remote");
}

static void css_error(GtkCssProvider *p, GtkCssSection *section, GError *e, gpointer data)
{
    char *where = gtk_css_section_to_string(section);
    g_printerr("style.css %s: %s\n", where, e->message);
    g_free(where);
}

static void load_css(void)
{
    GtkCssProvider *p = gtk_css_provider_new();
    g_signal_connect(p, "parsing-error", G_CALLBACK(css_error), NULL);
    const char *dev = g_getenv("LARES_CSS"); /* edit the look without rebuilding */
    if (dev) gtk_css_provider_load_from_path(p, dev);
    else gtk_css_provider_load_from_resource(p, CSS_RESOURCE);
    /* An app's own provider has to be told the system's light or dark choice
     * and its reduced-motion choice; only GTK's theme follows them by itself. */
    GtkSettings *settings = gtk_settings_get_default();
    g_object_bind_property(settings, "gtk-interface-color-scheme", p, "prefers-color-scheme",
                           G_BINDING_SYNC_CREATE);
    g_object_bind_property(settings, "gtk-interface-reduced-motion", p, "prefers-reduced-motion",
                           G_BINDING_SYNC_CREATE);
    /* Above the user's ~/.config/gtk-4.0/gtk.css (a desktop theme such as
     * WhiteSur), which otherwise repaints the app's own surfaces. */
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
}

void lares_open_window(Lares *app)
{
    load_css();
    app->sections = SECTIONS;
    app->n_sections = G_N_ELEMENTS(SECTIONS);

    app->win = gtk_application_window_new(app->gapp);
    gtk_window_set_title(GTK_WINDOW(app->win), "Lares");
    gtk_window_set_default_size(GTK_WINDOW(app->win), 900, 720);
    gtk_window_set_icon_name(GTK_WINDOW(app->win), APP_ID);
    gtk_widget_add_css_class(app->win, "lares");

    GtkWidget *bar = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(bar), ui_label("Lares", "app-name"));
    gtk_window_set_titlebar(GTK_WINDOW(app->win), bar);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->nav = lares_nav_new();
    gtk_widget_set_valign(app->nav, GTK_ALIGN_START);
    for (int i = 0; i < app->n_sections; i++)
        lares_nav_add(LARES_NAV(app->nav), SECTIONS[i].id, SECTIONS[i].icon, SECTIONS[i].label);
    g_signal_connect(app->nav, "selected", G_CALLBACK(on_nav), app);
    gtk_box_append(GTK_BOX(body), app->nav);

    app->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(app->stack), 180);
    gtk_widget_set_hexpand(app->stack, TRUE);
    gtk_box_append(GTK_BOX(body), app->stack);
    gtk_window_set_child(GTK_WINDOW(app->win), body);

    g_signal_connect(app->win, "notify::suspended", G_CALLBACK(on_window_state), app);
    g_signal_connect(app->win, "map", G_CALLBACK(on_map_change), app);
    g_signal_connect(app->win, "unmap", G_CALLBACK(on_map_change), app);
    devices_watch(app->devices, update_sections, app);
    pads_watch(app->pads, update_sections, app);
    remote_watch(app->remote, on_remote_read, app);

    lares_set_mode_colors(app);
    update_sections(app);
    on_remote_read(app);
    remote_refresh(app->remote);
    lares_show(app, "overview");
    const char *start = g_getenv("LARES_SECTION"); /* open on another section, for testing */
    if (start) lares_show(app, start);
    gtk_window_present(GTK_WINDOW(app->win));
}
