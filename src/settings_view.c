// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Settings section: what applies to Lares as a whole rather than to one
 * controller, so it's there with or without one. The background helper, and
 * the top-bar icons with a sub-section for each device. They live in
 * ~/.config/lares/pad.ini with the controller settings, which the helper also
 * reads. Nothing here reads any hardware. */
#include "lares.h"
#include "pad_service.h"

typedef struct {
    Lares *app;
    GtkWidget *top, *topbar_note;
    GtkWidget *sw_background, *sw_topbar, *sw_level, *sw_hide;
    gboolean syncing;
} SettingsView;

static PadPrefs *prefs(SettingsView *v) { return padconf_prefs(v->app->padconf); }

static void on_addon_state(PadAddonState state, gpointer data)
{
    SettingsView *v = data;
    gtk_widget_set_visible(v->topbar_note, prefs(v)->topbar && state == PAD_ADDON_NEEDS_LOGIN);
}

static void show_settings(SettingsView *v)
{
    PadPrefs *p = prefs(v);
    v->syncing = TRUE;
    gtk_switch_set_active(GTK_SWITCH(v->sw_background), p->background);
    gtk_switch_set_active(GTK_SWITCH(v->sw_topbar), p->topbar);
    gtk_switch_set_active(GTK_SWITCH(v->sw_level), p->topbar_level);
    gtk_switch_set_active(GTK_SWITCH(v->sw_hide), p->topbar_hide);
    /* The top-bar icon gets its readings from the helper. */
    gtk_widget_set_sensitive(v->top, p->background);
    v->syncing = FALSE;
    pad_service_addon_state(on_addon_state, v);
}

static void on_switch(GObject *sw, GParamSpec *ps, SettingsView *v)
{
    if (v->syncing) return;
    gboolean *field = g_object_get_data(sw, "field");
    *field = gtk_switch_get_active(GTK_SWITCH(sw));
    padconf_save(v->app->padconf);
    pad_service_sync(prefs(v));
    show_settings(v);
}

static GtkWidget *toggle(SettingsView *v, GtkWidget *group, const char *title, const char *sub, gboolean *field)
{
    return ui_setting_switch(ui_setting(group, title, sub), title, field, G_CALLBACK(on_switch), v);
}

static void on_conf(gpointer data)
{
    show_settings(data);
}

GtkWidget *settings_build(Lares *app, Section *self)
{
    SettingsView *v = g_new0(SettingsView, 1);
    v->app = app;
    self->view = v;
    PadPrefs *p = prefs(v);
    GtkWidget *page = ui_page();

    GtkWidget *bg = ui_group(page, "Background");
    v->sw_background = toggle(v, bg, "Run in the background",
        "Lets controller alerts, notices, keeping the screen awake, idle disconnect and the top-bar icon work "
        "while Lares is closed, through a small helper that needs under 8 MB. Off, all of those stop, and Lares "
        "shows your controllers only while its window is open.", &p->background);

    /* One sub-section per device that has a top-bar icon. */
    v->top = ui_group(page, "Top bar");
    ui_subheading(v->top, ART_GAMEPAD, "Xbox controller");
    v->sw_topbar = toggle(v, v->top, "Show in the top bar", "Lit while a controller is connected. Its menu has the "
                          "live picture, the battery and quick actions.", &p->topbar);
    v->topbar_note = ui_label("It appears after you log out and back in, as GNOME loads new top-bar items only then.",
                              "warn");
    gtk_label_set_wrap(GTK_LABEL(v->topbar_note), TRUE);
    gtk_widget_set_visible(v->topbar_note, FALSE);
    gtk_box_append(GTK_BOX(v->top), v->topbar_note);
    v->sw_level = toggle(v, v->top, "Battery level beside the icon", "With several controllers, the lowest charge.",
                         &p->topbar_level);
    v->sw_hide = toggle(v, v->top, "Hide the icon without a controller", NULL, &p->topbar_hide);

    show_settings(v);
    padconf_watch(app->padconf, on_conf, v);
    return page;
}
