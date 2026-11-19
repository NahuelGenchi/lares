// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Share section: AirDrop with iPhones, iPads and Macs. Receive for ten
 * minutes, as an iPhone does, or pick files and a nearby device to send
 * them to. The Wi-Fi card is lent only while one of those is on. The
 * structure is rebuilt when what's shown changes kind; progress and the
 * countdown update in place, so nothing moves under the pointer. */
#include "lares.h"
#include "share.h"
#include <string.h>

typedef struct {
    Lares *app;
    Share *share;
    GtkWidget *setup_box, *allow, *allow_error;
    GtkWidget *ready_box, *sw_receive, *receive_status, *send_button, *link_status;
    GtkWidget *incoming_group, *incoming_box;
    GtkWidget *nearby_group, *nearby_box;
    GtkWidget *card_value, *card_note;
    char *incoming_shape, *nearby_shape;
    /* in place */
    GtkWidget *in_meter, *in_text;
    GHashTable *rows;      /* device key -> its status label; "<key>#meter" -> its meter */
    guint tick;
    gboolean syncing, on_screen;
} ShareView;

static void update(ShareView *v);

static char *size_text(guint64 bytes)
{
    return g_format_size_full(bytes, G_FORMAT_SIZE_DEFAULT);
}

static GtkWidget *button(const char *text, const char *css, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(b, css);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static void clear(GtkWidget *box)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(box))) gtk_box_remove(GTK_BOX(box), c);
}

/* ------------------------------------------------------------- actions */

static void on_allow(GtkButton *b, ShareView *v)
{
    share_allow(v->share);
}

static void on_receive(GObject *sw, GParamSpec *ps, ShareView *v)
{
    if (v->syncing) return;
    share_set_receiving(v->share, gtk_switch_get_active(GTK_SWITCH(sw)));
}

static void files_picked(GObject *src, GAsyncResult *res, gpointer data)
{
    ShareView *v = data;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!files) return;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        GFile *f = g_list_model_get_item(files, i);
        char *p = g_file_get_path(f);
        if (p) g_ptr_array_add(paths, p);
        g_object_unref(f);
    }
    g_ptr_array_add(paths, NULL);
    share_set_sending(v->share, (const char *const *)paths->pdata);
    g_ptr_array_unref(paths);
    g_object_unref(files);
}

static void on_send(GtkButton *b, ShareView *v)
{
    if (share_state(v->share)->sending) {
        share_set_sending(v->share, NULL);
        return;
    }
    /* A dry run can pick files without the dialog, for testing. */
    const char *dry = g_getenv("LARES_DRY_RUN") ? g_getenv("LARES_DRY_FILES") : NULL;
    if (dry) {
        char **paths = g_strsplit(dry, ":", -1);
        share_set_sending(v->share, (const char *const *)paths);
        g_strfreev(paths);
        return;
    }
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose files to send");
    gtk_file_dialog_set_accept_label(d, "Choose");
    gtk_file_dialog_open_multiple(d, GTK_WINDOW(v->app->win), NULL, files_picked, v);
    g_object_unref(d);
}

static void on_send_to(GtkButton *b, ShareView *v)
{
    const char *key = g_object_get_data(G_OBJECT(b), "key");
    const ShareState *st = share_state(v->share);
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        if (!g_str_equal(d->key, key)) continue;
        if (d->state == DEVICE_ASKING || d->state == DEVICE_SENDING) share_cancel_send(v->share);
        else share_send_to(v->share, key);
        return;
    }
}

static void on_accept(GtkButton *b, ShareView *v) { share_answer(v->share, TRUE); }
static void on_decline(GtkButton *b, ShareView *v) { share_answer(v->share, FALSE); }
static void on_dismiss(GtkButton *b, ShareView *v) { share_dismiss_incoming(v->share); }

static void on_show_folder(GtkButton *b, ShareView *v)
{
    const ShareIncoming *in = share_state(v->share)->incoming;
    if (!in || !in->saved || !in->saved->len) return;
    GFile *f = g_file_new_for_path(in->saved->pdata[0]);
    GtkFileLauncher *l = gtk_file_launcher_new(f);
    gtk_file_launcher_open_containing_folder(l, GTK_WINDOW(v->app->win), NULL, NULL, NULL);
    g_object_unref(l);
    g_object_unref(f);
}

/* A link opens only when its button is pressed (SEC-4). */
static void on_open_link(GtkButton *b, ShareView *v)
{
    GtkUriLauncher *l = gtk_uri_launcher_new(g_object_get_data(G_OBJECT(b), "uri"));
    gtk_uri_launcher_launch(l, GTK_WINDOW(v->app->win), NULL, NULL, NULL);
    g_object_unref(l);
}

/* ------------------------------------------------------------ incoming */

/* "IMG_0412.HEIC and 1 more": what the sender offers, briefly. */
static char *offer_text(const ShareIncoming *in)
{
    guint n = in->names->len;
    if (in->links) return g_strdup(n == 1 ? "a link" : "links");
    const char *first = n ? in->names->pdata[0] : "files";
    return n <= 1 ? g_strdup(first) : g_strdup_printf("%s and %u more", first, n - 1);
}

static void build_incoming(ShareView *v, const ShareIncoming *in)
{
    GtkWidget *box = v->incoming_box;
    clear(box);
    v->in_meter = v->in_text = NULL;
    char *what = offer_text(in);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(head, "setting");
    GtkWidget *art = ui_icon(ART_PHONE, 4, "device-art");
    ui_class(art, "on", in->state != INCOMING_FAILED);
    gtk_box_append(GTK_BOX(head), art);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    /* The sender's own words: plain text in a label, never markup (SEC-4). */
    GtkWidget *who = ui_label(in->sender, "card-title");
    gtk_label_set_ellipsize(GTK_LABEL(who), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(texts), who);
    char *line = NULL;
    switch (in->state) {
    case INCOMING_ASKING: line = g_strdup_printf("Wants to send you %s.", what); break;
    case INCOMING_RECEIVING: line = g_strdup_printf("Sending you %s.", what); break;
    case INCOMING_DONE: line = in->links ? g_strdup("Sent you a link.") : g_strdup_printf("Sent you %s. Saved in Downloads.", what); break;
    case INCOMING_FAILED: line = g_strdup_printf("Couldn't receive %s: %s", what, in->message ? in->message : "it stopped."); break;
    }
    GtkWidget *l = ui_paragraph(line, in->state == INCOMING_FAILED ? "warn" : "muted");
    gtk_box_append(GTK_BOX(texts), l);
    g_free(line);
    if (in->state == INCOMING_RECEIVING) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        v->in_meter = lares_meter_new(20, 2);
        gtk_box_append(GTK_BOX(row), v->in_meter);
        v->in_text = ui_label("", "muted");
        gtk_box_append(GTK_BOX(row), v->in_text);
        gtk_box_append(GTK_BOX(texts), row);
    }
    gtk_box_append(GTK_BOX(head), texts);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    if (in->state == INCOMING_ASKING) {
        gtk_box_append(GTK_BOX(actions), button("Accept", "primary", G_CALLBACK(on_accept), v));
        gtk_box_append(GTK_BOX(actions), button("Decline", "quiet", G_CALLBACK(on_decline), v));
    } else if (in->state == INCOMING_DONE && in->links) {
        for (guint i = 0; i < in->saved->len && i < 4; i++) {
            GtkWidget *b = button("Open link", "primary", G_CALLBACK(on_open_link), v);
            g_object_set_data_full(G_OBJECT(b), "uri", g_strdup(in->saved->pdata[i]), g_free);
            gtk_widget_set_tooltip_text(b, in->saved->pdata[i]);
            gtk_box_append(GTK_BOX(actions), b);
        }
        gtk_box_append(GTK_BOX(actions), button("Done", "quiet", G_CALLBACK(on_dismiss), v));
    } else if (in->state == INCOMING_DONE) {
        gtk_box_append(GTK_BOX(actions), button("Show in folder", "primary", G_CALLBACK(on_show_folder), v));
        gtk_box_append(GTK_BOX(actions), button("Done", "quiet", G_CALLBACK(on_dismiss), v));
    } else if (in->state == INCOMING_FAILED) {
        gtk_box_append(GTK_BOX(actions), button("Done", "quiet", G_CALLBACK(on_dismiss), v));
    }
    if (gtk_widget_get_first_child(actions)) gtk_box_append(GTK_BOX(box), actions);
    else g_object_ref_sink(actions), g_object_unref(actions);
    g_free(what);
}

static void update_incoming(ShareView *v, const ShareIncoming *in)
{
    if (!v->in_meter || !in->total) {
        if (v->in_text) {
            char *done = size_text(in->done);
            gtk_label_set_text(GTK_LABEL(v->in_text), done);
            g_free(done);
        }
        return;
    }
    lares_meter_set(LARES_METER(v->in_meter), MIN(1.0, (double)in->done / in->total));
    char *done = size_text(in->done), *total = size_text(in->total);
    char *t = g_strdup_printf("%s of %s", done, total);
    gtk_label_set_text(GTK_LABEL(v->in_text), t);
    g_free(t);
    g_free(done);
    g_free(total);
}

/* -------------------------------------------------------------- nearby */

static const char *device_line(const ShareDevice *d)
{
    switch (d->state) {
    case DEVICE_FOUND: return "Ready";
    case DEVICE_ASKING: return "Waiting for them to accept…";
    case DEVICE_SENDING: return "Sending…";
    case DEVICE_SENT: return "Sent";
    case DEVICE_DECLINED: return "Declined";
    case DEVICE_FAILED: return d->message ? d->message : "Couldn't send.";
    }
    return "";
}

static void build_nearby(ShareView *v, const ShareState *st)
{
    GtkWidget *box = v->nearby_box;
    clear(box);
    g_hash_table_remove_all(v->rows);
    guint n = g_strv_length(st->paths);
    char *first = g_path_get_basename(st->paths[0]);
    char *what = n == 1 ? g_strdup_printf("Sending %s.", first)
                        : g_strdup_printf("Sending %s and %u more.", first, n - 1);
    gtk_box_append(GTK_BOX(box), ui_paragraph(what, "muted"));
    g_free(what);
    g_free(first);
    if (!st->devices->len) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        gtk_widget_add_css_class(row, "setting");
        GtkWidget *spin = lares_pixel_spinner_new(2);
        gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), spin);
        const char *text = st->link == LINK_UP ? "Looking for nearby devices. The other device needs AirDrop set to Everyone for 10 Minutes."
                                               : "Starting AirDrop…";
        GtkWidget *l = ui_paragraph(text, "muted");
        gtk_widget_set_hexpand(l, TRUE);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(box), row);
        return;
    }
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        gtk_widget_add_css_class(row, "setting");
        GtkWidget *art = ui_icon(ART_PHONE, 3, "device-art");
        ui_class(art, "on", d->state != DEVICE_FAILED);
        gtk_box_append(GTK_BOX(row), art);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
        gtk_widget_set_hexpand(texts, TRUE);
        gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
        GtkWidget *name = ui_label(d->name, NULL);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(texts), name);
        GtkWidget *status = ui_label(device_line(d), d->state == DEVICE_FAILED || d->state == DEVICE_DECLINED ? "warn" : "muted");
        gtk_label_set_wrap(GTK_LABEL(status), TRUE);
        gtk_box_append(GTK_BOX(texts), status);
        g_hash_table_insert(v->rows, g_strdup(d->key), status);
        if (d->state == DEVICE_SENDING) {
            GtkWidget *m = lares_meter_new(20, 2);
            gtk_box_append(GTK_BOX(texts), m);
            g_hash_table_insert(v->rows, g_strconcat(d->key, "#meter", NULL), m);
        }
        gtk_box_append(GTK_BOX(row), texts);
        gboolean busy = d->state == DEVICE_ASKING || d->state == DEVICE_SENDING;
        const char *label = busy ? "Cancel" : d->state == DEVICE_FOUND ? "Send" : "Send again";
        GtkWidget *b = button(label, busy ? "quiet" : "primary", G_CALLBACK(on_send_to), v);
        gtk_widget_add_css_class(b, "action");
        g_object_set_data_full(G_OBJECT(b), "key", g_strdup(d->key), g_free);
        /* Named for its device: a button made with a label is otherwise
         * "labelled by" it, which would win over this. */
        char *name_label = g_strdup_printf("%s to %s", label, d->name);
        gtk_accessible_reset_relation(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_RELATION_LABELLED_BY);
        gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL, name_label, -1);
        g_free(name_label);
        gtk_box_append(GTK_BOX(row), b);
        gtk_box_append(GTK_BOX(box), row);
    }
}

static void update_nearby(ShareView *v, const ShareState *st)
{
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        char *mk = g_strconcat(d->key, "#meter", NULL);
        GtkWidget *m = g_hash_table_lookup(v->rows, mk), *status = g_hash_table_lookup(v->rows, d->key);
        g_free(mk);
        if (m && d->total) lares_meter_set(LARES_METER(m), MIN(1.0, (double)d->sent / d->total));
        if (status && d->state == DEVICE_SENDING && d->total) {
            char *t = g_strdup_printf("Sending… %d%%", (int)(100.0 * d->sent / d->total));
            gtk_label_set_text(GTK_LABEL(status), t);
            g_free(t);
        }
    }
}

/* What the structure depends on; progress isn't part of it. */
static char *nearby_shape(const ShareState *st)
{
    if (!st->sending) return NULL;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "%d|%u|", st->link == LINK_UP, g_strv_length(st->paths));
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        g_string_append_printf(s, "%s:%s:%d:%s;", d->key, d->name, d->state, d->message ? d->message : "");
    }
    return g_string_free(s, FALSE);
}

static char *incoming_shape(const ShareState *st)
{
    const ShareIncoming *in = st->incoming;
    return in ? g_strdup_printf("%u:%d", in->id, in->state) : NULL;
}

/* ------------------------------------------------------------- update */

static void update_receive_status(ShareView *v)
{
    const ShareState *st = share_state(v->share);
    char *text = NULL;
    if (st->receiving) {
        gint64 left = (st->receive_until - g_get_monotonic_time()) / G_USEC_PER_SEC;
        if (left < 0) left = 0;
        const char *link = st->link == LINK_UP ? "Visible as" : "Starting AirDrop for";
        text = g_strdup_printf("%s %s · %d:%02d left", link, st->name, (int)(left / 60), (int)(left % 60));
    }
    gtk_label_set_text(GTK_LABEL(v->receive_status), text ? text : "");
    gtk_widget_set_visible(v->receive_status, text != NULL);
    g_free(text);
}

static gboolean on_tick(gpointer data)
{
    update_receive_status(data);
    return G_SOURCE_CONTINUE;
}

/* The countdown ticks only while it's on screen. */
static void sync_tick(ShareView *v)
{
    gboolean want = v->on_screen && share_state(v->share)->receiving;
    if (want && !v->tick) v->tick = g_timeout_add_seconds(1, on_tick, v);
    else if (!want) g_clear_handle_id(&v->tick, g_source_remove);
}

static void update(ShareView *v)
{
    const ShareState *st = share_state(v->share);
    gboolean ready = st->setup == SHARE_READY;
    gtk_widget_set_visible(v->setup_box, !ready);
    gtk_widget_set_visible(v->ready_box, ready);

    gtk_widget_set_sensitive(v->allow, !st->allow_busy);
    gtk_button_set_child(GTK_BUTTON(v->allow), ui_busy_child("Allow", st->allow_busy));
    gtk_label_set_text(GTK_LABEL(v->allow_error), st->allow_error ? st->allow_error : "");
    gtk_widget_set_visible(v->allow_error, st->allow_error != NULL);

    v->syncing = TRUE;
    gtk_switch_set_active(GTK_SWITCH(v->sw_receive), st->receiving);
    v->syncing = FALSE;
    update_receive_status(v);
    ui_button_label(v->send_button, st->sending ? "Stop" : "Choose files…");
    ui_class(v->send_button, "primary", !st->sending);
    ui_class(v->send_button, "quiet", st->sending);

    const char *link = st->link == LINK_FAILED ? (st->link_error ? st->link_error : "AirDrop stopped.")
                     : st->link == LINK_UP && st->link_error ? st->link_error : NULL;
    gtk_label_set_text(GTK_LABEL(v->link_status), link ? link : "");
    gtk_widget_set_visible(v->link_status, link != NULL);

    char *shape = incoming_shape(st);
    if (g_strcmp0(shape, v->incoming_shape)) {
        g_free(v->incoming_shape);
        v->incoming_shape = shape;
        if (st->incoming) build_incoming(v, st->incoming);
    } else {
        g_free(shape);
    }
    gtk_widget_set_visible(gtk_widget_get_parent(v->incoming_group), st->incoming != NULL);
    if (st->incoming) update_incoming(v, st->incoming);

    shape = nearby_shape(st);
    if (g_strcmp0(shape, v->nearby_shape)) {
        g_free(v->nearby_shape);
        v->nearby_shape = shape;
        if (st->sending) build_nearby(v, st);
    } else {
        g_free(shape);
    }
    gtk_widget_set_visible(gtk_widget_get_parent(v->nearby_group), st->sending);
    if (st->sending) update_nearby(v, st);

    gtk_label_set_text(GTK_LABEL(v->card_value), st->card_text ? st->card_text : "None");
    GString *note = g_string_new(NULL);
    if (st->card_in_use) g_string_append(note, "This card carries your Wi-Fi: it disconnects while AirDrop is on. ");
    if (st->card_text && !st->card_5ghz && strstr(st->card_text, "2.4 GHz only"))
        g_string_append(note, "On 2.4 GHz alone, an iPhone gives AirDrop little time: photos take about a minute each. "
                              "A card with 5 GHz is several times faster.");
    gtk_label_set_text(GTK_LABEL(v->card_note), note->str);
    gtk_widget_set_visible(v->card_note, note->len > 0);
    g_string_free(note, TRUE);
    sync_tick(v);
}

static void on_changed(gpointer data)
{
    update(data);
}

/* -------------------------------------------------------------- build */

GtkWidget *share_build(Lares *app, Section *self)
{
    ShareView *v = g_new0(ShareView, 1);
    v->app = app;
    v->share = share_new();
    v->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->view = v;
    GtkWidget *page = ui_page();

    GtkWidget *group = ui_group(page, "AirDrop");
    v->setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(v->setup_box),
                   ui_paragraph("Send and receive files with iPhones, iPads and Macs nearby. Lares lends AirDrop "
                                "the Wi-Fi card while it's on, which needs your password, one time.", NULL));
    v->allow = gtk_button_new();
    gtk_widget_add_css_class(v->allow, "primary");
    gtk_widget_add_css_class(v->allow, "action");
    gtk_widget_set_halign(v->allow, GTK_ALIGN_START);
    gtk_widget_set_cursor_from_name(v->allow, "pointer");
    gtk_accessible_update_property(GTK_ACCESSIBLE(v->allow), GTK_ACCESSIBLE_PROPERTY_LABEL, "Allow AirDrop", -1);
    g_signal_connect(v->allow, "clicked", G_CALLBACK(on_allow), v);
    gtk_box_append(GTK_BOX(v->setup_box), v->allow);
    v->allow_error = ui_paragraph("", "warn");
    gtk_box_append(GTK_BOX(v->setup_box), v->allow_error);
    gtk_box_append(GTK_BOX(group), v->setup_box);

    v->ready_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *receive = ui_setting(v->ready_box, "Receive", "Nearby iPhones, iPads and Macs can send you files for 10 minutes.");
    gboolean dummy = FALSE;
    v->sw_receive = ui_setting_switch(receive, "Receive with AirDrop", &dummy, G_CALLBACK(on_receive), v);
    g_object_set_data(G_OBJECT(v->sw_receive), "field", NULL);
    /* Under the row, so the switch doesn't move when the line comes. */
    v->receive_status = ui_label("", "muted");
    gtk_box_insert_child_after(GTK_BOX(v->ready_box), v->receive_status, receive);
    GtkWidget *send = ui_setting(v->ready_box, "Send", "To an iPhone, iPad or Mac set to Everyone for 10 Minutes.");
    v->send_button = gtk_button_new();
    ui_button_labels(v->send_button, (const char *const[]){ "Choose files…", "Stop", NULL });
    gtk_widget_add_css_class(v->send_button, "primary");
    gtk_widget_set_valign(v->send_button, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(v->send_button, "pointer");
    g_signal_connect(v->send_button, "clicked", G_CALLBACK(on_send), v);
    gtk_box_append(GTK_BOX(send), v->send_button);
    v->link_status = ui_paragraph("", "warn");
    gtk_box_append(GTK_BOX(v->ready_box), v->link_status);
    gtk_box_append(GTK_BOX(group), v->ready_box);

    v->incoming_group = ui_group(page, "Incoming");
    v->incoming_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(v->incoming_group), v->incoming_box);

    v->nearby_group = ui_group(page, "Nearby");
    v->nearby_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(v->nearby_group), v->nearby_box);

    GtkWidget *card = ui_group(page, "Wi-Fi card");
    GtkWidget *row = ui_setting(card, "Card", NULL);
    v->card_value = ui_label("", "muted");
    gtk_box_append(GTK_BOX(row), v->card_value);
    v->card_note = ui_paragraph("", "muted");
    gtk_box_append(GTK_BOX(card), v->card_note);

    gtk_box_append(GTK_BOX(page),
                   ui_paragraph("On an iPhone: open Control Center, touch and hold the network controls, tap AirDrop, "
                                "then choose Everyone for 10 Minutes.", "muted"));

    share_watch(v->share, on_changed, v);
    update(v);
    return page;
}

void share_shown(Lares *app, Section *self, gboolean on)
{
    ShareView *v = self->view;
    v->on_screen = on;
    if (on) share_refresh(v->share);
    sync_tick(v);
}
