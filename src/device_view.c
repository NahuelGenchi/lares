/* Headphones, Keyboard and Mouse: one card per paired device, with its
 * battery and, for Bluetooth ones, a button to connect or disconnect it.
 * AirPods show up in Headphones. A keyboard or mouse plugged in by USB,
 * wired or on its own receiver, has no button, unless its battery needs the
 * person's permission once: then the button allows it. */
#include "lares.h"

typedef struct {
    Lares *app;
    DevKind kind;
    GtkWidget *page;
    char *busy_id;       /* the device being connected, disconnected or allowed */
    char *error_id, *error;
} DeviceView;

static void rebuild(gpointer user);

static void action_done(gboolean ok, const char *message, gpointer user)
{
    DeviceView *v = user;
    g_free(v->error_id);
    g_free(v->error);
    v->error_id = ok ? NULL : g_strdup(v->busy_id);
    v->error = ok ? NULL : g_strdup(message);
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
    g_clear_pointer(&v->error_id, g_free);
    rebuild(v);
    if (allow) devices_allow_batteries(v->app->devices, action_done, v);
    else devices_set_connected(v->app->devices, id, on, action_done, v);
}

/* Connect or Disconnect for a Bluetooth device, Allow for a battery that
 * needs permission; a spinner while it works. */
static GtkWidget *action_button(DeviceView *v, Device *d, gboolean busy)
{
    gboolean allow = !d->bluetooth;
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, allow || !d->connected ? "primary" : "quiet");
    gtk_widget_add_css_class(b, "action"); /* one width, with a label or the spinner */
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
    g_signal_connect(b, "clicked", G_CALLBACK(on_button), v);
    return b;
}

static GtkWidget *device_card(DeviceView *v, Device *d)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "padded");
    GtkWidget *art = ui_icon(device_art(d->kind), 4, "device-art");
    ui_class(art, "on", d->connected);
    gtk_box_append(GTK_BOX(card), art);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    GtkWidget *name = ui_label(d->name, NULL);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(texts), name);

    /* Connected goes without saying: the icon lights up, and a Bluetooth one
     * offers Disconnect. */
    gboolean busy = v->busy_id && g_str_equal(v->busy_id, d->id);
    const char *status = busy && d->battery_locked ? "Waiting for your password…"
                       : busy ? (d->connected ? "Disconnecting…" : "Connecting…")
                       : !d->connected ? "Not connected"
                       : d->charging ? "Charging" : NULL;
    if (status) gtk_box_append(GTK_BOX(texts), ui_label(status, "muted"));

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
        /* While it waits, the line above says so. */
        if (!busy) {
            GtkWidget *hint = ui_label("Lares can show its battery once you allow it. That asks for your password, "
                                       "one time.", "muted");
            gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
            gtk_box_append(GTK_BOX(texts), hint);
        }
        GtkWidget *allow = action_button(v, d, busy);
        gtk_widget_set_halign(allow, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(texts), allow);
    }
    if (v->error_id && g_str_equal(v->error_id, d->id)) {
        GtkWidget *err = ui_paragraph(v->error, "bad");
        gtk_box_append(GTK_BOX(texts), err);
    }
    gtk_box_append(GTK_BOX(card), texts);

    if (d->bluetooth) gtk_box_append(GTK_BOX(card), action_button(v, d, busy));
    return card;
}

/* Devices change rarely, so the cards are simply made again. */
static void rebuild(gpointer user)
{
    DeviceView *v = user;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(v->page)))
        gtk_box_remove(GTK_BOX(v->page), c);
    GPtrArray *list = devices_list(v->app->devices);
    for (guint i = 0; i < list->len; i++) {
        Device *d = list->pdata[i];
        if (d->kind == v->kind) gtk_box_append(GTK_BOX(v->page), device_card(v, d));
    }
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

/* Devices report changes by themselves, except batteries that only their
 * own protocol reports: those are asked for while the section shows. */
void device_shown(Lares *app, Section *self, gboolean on)
{
    devices_want_batteries(app->devices, on);
}
