// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* macOS Finder-inspired AirDrop Share section for GTK 4: concentric radar
 * rings, official vector AirDrop emblem, inline "Allow me to be discovered by:"
 * dropdown (Receiving Off, Contacts Only, Everyone for 10 Minutes), live
 * nearby device circles with profile icons and circular progress arcs,
 * floating transfer popovers with preview thumbnails and ETA, drag-and-drop
 * directly onto device circles, and local Contacts & Avatars management. */
#include "lares.h"
#include "share.h"
#include <math.h>
#include <string.h>

typedef struct {
    char *key;
    char *name;
    char *subtitle;
    ShareDeviceKind kind;
    cairo_surface_t *photo;
    double progress;
    gboolean active_progress;
    gboolean waiting;
    gboolean done;
    gboolean drag_over;
} AvatarData;

typedef struct {
    Lares *app;
    Share *share;

    GtkWidget *stage;
    GtkWidget *radar;

    GtkWidget *setup_box;
    GtkWidget *allow;
    GtkWidget *allow_error;

    GtkWidget *ready_box;
    GtkWidget *staged_pill;
    GtkWidget *staged_label;
    GtkWidget *scan_btn;
    GtkWidget *contacts_btn;
    GtkWidget *adapter_btn;
    GtkWidget *card_value;
    GtkWidget *card_link;
    GtkWidget *card_note;

    GtkWidget *popover_slot;
    GtkWidget *popover_bar;
    GtkWidget *popover_detail;
    GtkWidget *devices_row;

    GtkWidget *emblem;
    GtkWidget *discover_btn;
    GtkWidget *discover_label;
    GtkWidget *discover_popover;
    GtkWidget *mode_checks[3];
    GtkWidget *status_label;

    GtkWidget *contacts_win;
    GtkWidget *contacts_list;
    GtkWidget *entry_name;
    GtkWidget *entry_device;
    GtkWidget *entry_email;
    GtkWidget *entry_phone;
    GtkWidget *draft_photo_btn;
    char *draft_photo;

    char *devices_shape;
    char *popover_shape;
    GHashTable *avatars; /* device key -> GtkWidget* (GtkDrawingArea) */
    guint tick;
    gboolean on_screen;
    gboolean browsing_auto;
} ShareView;

static void update(ShareView *v);
static void refresh_contacts_dialog(ShareView *v);

static void destroy_surface(gpointer p)
{
    if (p) cairo_surface_destroy(p);
}

static void destroy_contact(gpointer p)
{
    share_contact_free(p);
}

static void avatar_data_free(gpointer p)
{
    AvatarData *d = p;
    if (!d) return;
    g_free(d->key);
    g_free(d->name);
    g_free(d->subtitle);
    g_clear_pointer(&d->photo, cairo_surface_destroy);
    g_free(d);
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(box))) gtk_box_remove(GTK_BOX(box), c);
}

static char *size_text(guint64 bytes)
{
    return g_format_size_full(bytes, G_FORMAT_SIZE_DEFAULT);
}

static char *eta_text(guint64 done, guint64 total, gint64 elapsed_us)
{
    if (!total || done == 0 || elapsed_us < 500000) return NULL;
    if (done >= total) return g_strdup("Less than 5 seconds");
    double rate = (double)done / ((double)elapsed_us / 1000000.0);
    if (rate < 1.0) return NULL;
    double rem = (double)(total - done) / rate;
    if (rem < 5.0) return g_strdup("About 5 seconds");
    if (rem < 55.0) {
        int s = (int)llround(rem / 5.0) * 5;
        return g_strdup_printf("About %d seconds", s);
    }
    int mins = (int)llround(rem / 60.0);
    if (mins <= 1) return g_strdup("About a minute");
    return g_strdup_printf("About %d minutes", mins);
}

static const char *discover_mode_label(ShareDiscoverMode m)
{
    switch (m) {
    case SHARE_DISCOVER_OFF: return "Receiving Off";
    case SHARE_DISCOVER_CONTACTS: return "Contacts Only";
    case SHARE_DISCOVER_EVERYONE: return "Everyone for 10 Minutes";
    }
    return "Receiving Off";
}

static char *offer_text(const ShareIncoming *in)
{
    guint n = in->names->len;
    if (in->links) return g_strdup(n == 1 ? "a link" : "links");
    const char *first = n ? in->names->pdata[0] : "files";
    return n <= 1 ? g_strdup(first) : g_strdup_printf("%s and %u more", first, n - 1);
}

static gboolean looks_like_image(const char *name)
{
    if (!name) return FALSE;
    const char *dot = strrchr(name, '.');
    if (!dot) return FALSE;
    return g_ascii_strcasecmp(dot, ".png") == 0 || g_ascii_strcasecmp(dot, ".jpg") == 0 ||
           g_ascii_strcasecmp(dot, ".jpeg") == 0 || g_ascii_strcasecmp(dot, ".heic") == 0 ||
           g_ascii_strcasecmp(dot, ".webp") == 0 || g_ascii_strcasecmp(dot, ".gif") == 0;
}

/* Downloads a GdkTexture into a self-owned square center-cropped Cairo surface. */
static cairo_surface_t *texture_to_square_surface(GdkTexture *tex, int size)
{
    if (!tex || size <= 0) return NULL;
    int tw = gdk_texture_get_width(tex);
    int th = gdk_texture_get_height(tex);
    if (tw <= 0 || th <= 0) return NULL;

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, tw);
    guchar *buf = g_malloc((gsize)stride * (gsize)th);
    gdk_texture_download(tex, buf, (gsize)stride);
    cairo_surface_t *src = cairo_image_surface_create_for_data(buf, CAIRO_FORMAT_ARGB32, tw, th, stride);

    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(dst);
    double side = (double)MIN(tw, th);
    double sc = (double)size / side;
    cairo_translate(cr, ((double)size - (double)tw * sc) * 0.5, ((double)size - (double)th * sc) * 0.5);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    cairo_surface_destroy(src);
    g_free(buf);
    return dst;
}

static cairo_surface_t *load_photo_surface(const char *path, int size)
{
    if (!path || !*path) return NULL;
    GdkTexture *tex = gdk_texture_new_from_filename(path, NULL);
    if (!tex) return NULL;
    cairo_surface_t *surf = texture_to_square_surface(tex, size);
    g_object_unref(tex);
    return surf;
}

static cairo_surface_t *load_bytes_surface(GBytes *bytes, int size)
{
    if (!bytes || !g_bytes_get_size(bytes)) return NULL;
    GdkTexture *tex = gdk_texture_new_from_bytes(bytes, NULL);
    if (!tex) return NULL;
    cairo_surface_t *surf = texture_to_square_surface(tex, size);
    g_object_unref(tex);
    return surf;
}

/* ------------------------------------------------------------- Cairo drawing */

static void draw_radar(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user)
{
    GdkRGBA c;
    gtk_widget_get_color(GTK_WIDGET(area), &c);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
    cairo_set_line_width(cr, 1.0);

    double cx = width * 0.5;
    double cy = CLAMP(height * 0.72, 240.0, MAX(240.0, height - 118.0));
    double step = CLAMP(MIN(width * 0.112, cy * 0.21), 54.0, 74.0);
    double max_r = hypot(MAX(cx, width - cx), MAX(cy, height - cy)) + step;

    for (double r = step; r <= max_r; r += step) {
        cairo_arc(cr, cx, cy, r, 0, 2.0 * G_PI);
        cairo_stroke(cr);
    }
}

static void draw_emblem(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user)
{
    GdkRGBA c;
    gtk_widget_get_color(GTK_WIDGET(area), &c);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);

    double cx = width * 0.5;
    double cy = height * 0.5;
    double scale = MIN(width, height) / 68.0;

    cairo_arc(cr, cx, cy, 4.6 * scale, 0, 2.0 * G_PI);
    cairo_fill(cr);

    static const double radii[3] = { 12.6, 20.6, 28.6 };
    double gap_half = 34.0 * (G_PI / 180.0);
    double a_start = G_PI * 0.5 + gap_half;
    double a_end = G_PI * 0.5 - gap_half + 2.0 * G_PI;

    cairo_set_line_width(cr, 3.5 * scale);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 3; i++) {
        cairo_arc(cr, cx, cy, radii[i] * scale, a_start, a_end);
        cairo_stroke(cr);
    }
}

static void draw_device_glyph(cairo_t *cr, double cx, double cy, double s, ShareDeviceKind kind)
{
    double lw = MAX(1.2, s * 0.10);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (kind) {
    case DEVICE_KIND_PHONE: {
        double w = s * 0.54, h = s * 0.88;
        cairo_rectangle(cr, cx - w * 0.5, cy - h * 0.5, w, h);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - w * 0.18, cy - h * 0.34);
        cairo_line_to(cr, cx + w * 0.18, cy - h * 0.34);
        cairo_stroke(cr);
        break;
    }
    case DEVICE_KIND_TABLET: {
        double w = s * 0.68, h = s * 0.86;
        cairo_rectangle(cr, cx - w * 0.5, cy - h * 0.5, w, h);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy + h * 0.36, lw * 0.6, 0, 2.0 * G_PI);
        cairo_fill(cr);
        break;
    }
    case DEVICE_KIND_LAPTOP: {
        double w = s * 0.78, h = s * 0.50;
        cairo_rectangle(cr, cx - w * 0.5, cy - h * 0.58, w, h);
        cairo_stroke(cr);
        double by = cy + h * 0.42 + lw * 1.2;
        cairo_move_to(cr, cx - s * 0.50, by);
        cairo_line_to(cr, cx + s * 0.50, by);
        cairo_stroke(cr);
        break;
    }
    case DEVICE_KIND_DESKTOP: {
        double w = s * 0.82, h = s * 0.56;
        cairo_rectangle(cr, cx - w * 0.5, cy - h * 0.64, w, h);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy + h * 0.36);
        cairo_line_to(cr, cx, cy + h * 0.36 + s * 0.18);
        cairo_move_to(cr, cx - s * 0.22, cy + h * 0.36 + s * 0.18);
        cairo_line_to(cr, cx + s * 0.22, cy + h * 0.36 + s * 0.18);
        cairo_stroke(cr);
        break;
    }
    case DEVICE_KIND_WATCH: {
        double w = s * 0.46, h = s * 0.56;
        cairo_rectangle(cr, cx - w * 0.5, cy - h * 0.5, w, h);
        cairo_stroke(cr);
        break;
    }
    case DEVICE_KIND_OTHER:
        cairo_arc(cr, cx, cy, s * 0.36, 0, 2.0 * G_PI);
        cairo_stroke(cr);
        break;
    }
}

static void avatar_gradient_colors(const char *seed, double top[3], double bot[3])
{
    static const double pairs[][6] = {
        { 0.620, 0.651, 0.706, 0.486, 0.518, 0.576 },
        { 0.353, 0.686, 0.980, 0.180, 0.506, 0.902 },
        { 0.541, 0.588, 0.957, 0.369, 0.412, 0.824 },
        { 0.957, 0.533, 0.490, 0.855, 0.376, 0.341 },
        { 0.451, 0.784, 0.545, 0.282, 0.635, 0.392 },
        { 0.961, 0.690, 0.341, 0.851, 0.537, 0.176 },
        { 0.784, 0.537, 0.898, 0.608, 0.369, 0.753 },
    };
    guint32 h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)(seed ? seed : ""); *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    const double *c = pairs[h % G_N_ELEMENTS(pairs)];
    memcpy(top, c, 3 * sizeof(double));
    memcpy(bot, c + 3, 3 * sizeof(double));
}

static void draw_avatar(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user)
{
    AvatarData *d = user;
    double cx = width * 0.5;
    double cy = height * 0.5;
    double r = MIN(width, height) * 0.5 - 6.0;

    if (d->drag_over) {
        cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 0.18);
        cairo_arc(cr, cx, cy, r + 5.5, 0, 2.0 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 0.95);
        cairo_set_line_width(cr, 2.6);
        cairo_arc(cr, cx, cy, r + 4.0, 0, 2.0 * G_PI);
        cairo_stroke(cr);
    }

    /* Subtle drop shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_arc(cr, cx, cy + 1.5, r, 0, 2.0 * G_PI);
    cairo_fill(cr);

    /* Clipped circular interior */
    gboolean drew_device_silhouette = FALSE;
    cairo_save(cr);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * G_PI);
    cairo_clip(cr);

    if (d->photo) {
        int sw = cairo_image_surface_get_width(d->photo);
        double sc = (r * 2.0) / MAX(1, sw);
        cairo_translate(cr, cx - r, cy - r);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, d->photo, 0, 0);
        cairo_paint(cr);
    } else {
        double top[3], bot[3];
        avatar_gradient_colors(d->name, top, bot);
        cairo_pattern_t *pat = cairo_pattern_create_linear(cx, cy - r, cx, cy + r);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, top[0], top[1], top[2], 1.0);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, bot[0], bot[1], bot[2], 1.0);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);

        char *init = share_initials(d->name);
        gboolean generic = d->name && (g_ascii_strcasecmp(d->name, "iPhone") == 0 ||
                                       g_ascii_strcasecmp(d->name, "iPad") == 0 ||
                                       g_ascii_strcasecmp(d->name, "Mac") == 0 ||
                                       g_ascii_strcasecmp(d->name, "MacBook") == 0 ||
                                       g_ascii_strcasecmp(d->name, "Apple Device") == 0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
        if (generic || !init || !*init) {
            drew_device_silhouette = TRUE;
            draw_device_glyph(cr, cx, cy, 28.0, d->kind);
        } else {
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 23.0);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, init, &ext);
            cairo_move_to(cr, cx - ext.width * 0.5 - ext.x_bearing, cy - ext.height * 0.5 - ext.y_bearing);
            cairo_show_text(cr, init);
        }
        g_free(init);
    }
    cairo_restore(cr);

    /* Subtle rim */
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.55, 0.25);
    cairo_set_line_width(cr, 1.2);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * G_PI);
    cairo_stroke(cr);

    /* Active circular progress arc (matches macOS blue arc around avatar) */
    if (d->active_progress) {
        double pr = r + 3.2;
        cairo_set_line_width(cr, 2.8);
        cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 0.18);
        cairo_arc(cr, cx, cy, pr, 0, 2.0 * G_PI);
        cairo_stroke(cr);

        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 1.0);
        double frac = CLAMP(d->progress, 0.06, 1.0);
        cairo_arc(cr, cx, cy, pr, -G_PI * 0.5, -G_PI * 0.5 + frac * 2.0 * G_PI);
        cairo_stroke(cr);
    } else if (d->waiting) {
        cairo_set_line_width(cr, 2.2);
        cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 0.65);
        cairo_arc(cr, cx, cy, r + 3.2, 0, 2.0 * G_PI);
        cairo_stroke(cr);
    } else if (d->done) {
        cairo_set_line_width(cr, 2.4);
        cairo_set_source_rgba(cr, 0.204, 0.780, 0.349, 1.0);
        cairo_arc(cr, cx, cy, r + 3.2, 0, 2.0 * G_PI);
        cairo_stroke(cr);
    } else if (!drew_device_silhouette) {
        /* Bottom-right device type badge */
        double bx = cx + r * 0.68;
        double by = cy + r * 0.68;
        cairo_set_source_rgba(cr, 0.94, 0.94, 0.96, 0.96);
        cairo_arc(cr, bx, by, 10.5, 0, 2.0 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 0.90);
        draw_device_glyph(cr, bx, by, 12.0, d->kind);
    }
}

static void draw_thumb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user)
{
    cairo_surface_t *surf = user;
    if (surf) {
        int sw = cairo_image_surface_get_width(surf);
        double sc = (double)MIN(width, height) / MAX(1, sw);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        return;
    }
    cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 0.12);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.0, 0.478, 1.0, 0.90);
    cairo_set_line_width(cr, 1.8);
    cairo_rectangle(cr, width * 0.26, height * 0.20, width * 0.48, height * 0.60);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------- actions */

static void on_allow(GtkButton *b, ShareView *v) { share_allow(v->share); }

static void on_pick_mode(GtkButton *b, ShareView *v)
{
    int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
    gtk_popover_popdown(GTK_POPOVER(v->discover_popover));
    share_set_discover_mode(v->share, (ShareDiscoverMode)mode);
}

static void on_toggle_scan(GtkButton *b, ShareView *v)
{
    const ShareState *st = share_state(v->share);
    share_set_browsing(v->share, !st->browsing);
}

static void on_clear_staged(GtkButton *b, ShareView *v)
{
    share_set_sending(v->share, NULL);
}

static void on_accept(GtkButton *b, ShareView *v) { share_answer(v->share, TRUE); }
static void on_decline(GtkButton *b, ShareView *v) { share_answer(v->share, FALSE); }
static void on_dismiss_in(GtkButton *b, ShareView *v) { share_dismiss_incoming(v->share); }
static void on_cancel_send(GtkButton *b, ShareView *v) { share_cancel_send(v->share); }

static void on_dismiss_dev(GtkButton *b, ShareView *v)
{
    const char *key = g_object_get_data(G_OBJECT(b), "key");
    if (key) share_dismiss_device(v->share, key);
}

static void on_send_again(GtkButton *b, ShareView *v)
{
    const char *key = g_object_get_data(G_OBJECT(b), "key");
    if (key) share_send_to(v->share, key);
}

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

static void on_open_link(GtkButton *b, ShareView *v)
{
    const char *uri = g_object_get_data(G_OBJECT(b), "uri");
    if (!uri) return;
    GtkUriLauncher *l = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(l, GTK_WINDOW(v->app->win), NULL, NULL, NULL);
    g_object_unref(l);
}

typedef struct {
    ShareView *v;
    char *target_key;
} FilePickCtx;

static void file_pick_done(GObject *src, GAsyncResult *res, gpointer data)
{
    FilePickCtx *ctx = data;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (files) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
            GFile *f = g_list_model_get_item(files, i);
            char *p = g_file_get_path(f);
            if (p) g_ptr_array_add(paths, p);
            g_object_unref(f);
        }
        g_ptr_array_add(paths, NULL);
        if (paths->len > 1) {
            if (ctx->target_key) share_send_files_to(ctx->v->share, ctx->target_key, (const char *const *)paths->pdata);
            else share_set_sending(ctx->v->share, (const char *const *)paths->pdata);
        }
        g_ptr_array_unref(paths);
        g_object_unref(files);
    }
    g_free(ctx->target_key);
    g_free(ctx);
}

typedef struct {
    ShareView *v;
    char *device_name;
    int contact_index; /* -1 for device photo, -2 for draft photo */
} PhotoPickCtx;

static void photo_pick_done(GObject *src, GAsyncResult *res, gpointer data)
{
    PhotoPickCtx *ctx = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        char *path = g_file_get_path(f);
        if (path) {
            if (ctx->contact_index == -2) {
                g_free(ctx->v->draft_photo);
                ctx->v->draft_photo = g_strdup(path);
                if (ctx->v->draft_photo_btn) gtk_button_set_label(GTK_BUTTON(ctx->v->draft_photo_btn), "Photo Selected");
            } else if (ctx->contact_index == -1 && ctx->device_name) {
                share_set_device_photo(ctx->v->share, ctx->device_name, path);
            } else if (ctx->contact_index >= 0) {
                const ShareState *st = share_state(ctx->v->share);
                GPtrArray *copy = g_ptr_array_new_with_free_func(destroy_contact);
                for (guint i = 0; st->contacts && i < st->contacts->len; i++) {
                    ShareContact *src_c = st->contacts->pdata[i];
                    ShareContact *c = g_new0(ShareContact, 1);
                    c->name = g_strdup(src_c->name);
                    c->device = g_strdup(src_c->device);
                    c->email = g_strdup(src_c->email);
                    c->phone = g_strdup(src_c->phone);
                    c->photo = g_strdup((int)i == ctx->contact_index ? path : src_c->photo);
                    g_ptr_array_add(copy, c);
                }
                share_save_contacts(ctx->v->share, copy);
                g_ptr_array_unref(copy);
                refresh_contacts_dialog(ctx->v);
            }
            g_free(path);
        }
        g_object_unref(f);
    }
    g_free(ctx->device_name);
    g_free(ctx);
}

static void launch_photo_picker(ShareView *v, GtkWindow *parent, const char *device_name, int contact_index)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose profile photo");
    gtk_file_dialog_set_accept_label(d, "Use Photo");
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "Images");
    gtk_file_filter_add_mime_type(flt, "image/png");
    gtk_file_filter_add_mime_type(flt, "image/jpeg");
    gtk_file_filter_add_mime_type(flt, "image/webp");
    gtk_file_dialog_set_default_filter(d, flt);
    g_object_unref(flt);

    PhotoPickCtx *ctx = g_new0(PhotoPickCtx, 1);
    ctx->v = v;
    ctx->device_name = g_strdup(device_name);
    ctx->contact_index = contact_index;
    gtk_file_dialog_open(d, parent, NULL, photo_pick_done, ctx);
    g_object_unref(d);
}

static void on_device_left_click(GtkGestureClick *g, int n_press, double x, double y, gpointer user)
{
    ShareView *v = user;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    const char *key = g_object_get_data(G_OBJECT(w), "dev-key");
    const char *name = g_object_get_data(G_OBJECT(w), "dev-name");
    if (!key) return;
    const ShareState *st = share_state(v->share);
    if (st->sending && st->paths && st->paths[0]) {
        share_send_to(v->share, key);
        return;
    }
    const char *dry = g_getenv("LARES_DRY_RUN") ? g_getenv("LARES_DRY_FILES") : NULL;
    if (dry) {
        char **paths = g_strsplit(dry, ":", -1);
        share_send_files_to(v->share, key, (const char *const *)paths);
        g_strfreev(paths);
        return;
    }
    GtkFileDialog *d = gtk_file_dialog_new();
    char *title = g_strdup_printf("Send files to %s", name ? name : "device");
    gtk_file_dialog_set_title(d, title);
    gtk_file_dialog_set_accept_label(d, "Send");
    g_free(title);
    FilePickCtx *ctx = g_new0(FilePickCtx, 1);
    ctx->v = v;
    ctx->target_key = g_strdup(key);
    gtk_file_dialog_open_multiple(d, GTK_WINDOW(v->app->win), NULL, file_pick_done, ctx);
    g_object_unref(d);
}

static void on_device_right_click(GtkGestureClick *g, int n_press, double x, double y, gpointer user)
{
    ShareView *v = user;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    const char *name = g_object_get_data(G_OBJECT(w), "dev-name");
    if (name) launch_photo_picker(v, GTK_WINDOW(v->app->win), name, -1);
}

static GdkDragAction on_device_drag_enter(GtkDropTarget *t, double x, double y, gpointer user)
{
    GtkWidget *da = user;
    AvatarData *ad = g_object_get_data(G_OBJECT(da), "avatar-data");
    if (ad) {
        ad->drag_over = TRUE;
        gtk_widget_queue_draw(da);
    }
    return GDK_ACTION_COPY;
}

static void on_device_drag_leave(GtkDropTarget *t, gpointer user)
{
    GtkWidget *da = user;
    AvatarData *ad = g_object_get_data(G_OBJECT(da), "avatar-data");
    if (ad) {
        ad->drag_over = FALSE;
        gtk_widget_queue_draw(da);
    }
}

static gboolean on_drop_files(GtkDropTarget *t, const GValue *val, double x, double y, gpointer user)
{
    ShareView *v = user;
    if (!G_VALUE_HOLDS(val, GDK_TYPE_FILE_LIST)) return FALSE;
    GdkFileList *fl = g_value_get_boxed(val);
    GSList *files = gdk_file_list_get_files(fl);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (GSList *l = files; l; l = l->next) {
        char *p = g_file_get_path(G_FILE(l->data));
        if (p) g_ptr_array_add(paths, p);
    }
    g_slist_free(files);
    g_ptr_array_add(paths, NULL);

    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
    const char *key = g_object_get_data(G_OBJECT(w), "dev-key");
    if (paths->len > 1) {
        if (key) share_send_files_to(v->share, key, (const char *const *)paths->pdata);
        else share_set_sending(v->share, (const char *const *)paths->pdata);
    }
    g_ptr_array_unref(paths);
    return TRUE;
}

/* ------------------------------------------------------------- Contacts dialog */

static void on_contact_remove(GtkButton *b, ShareView *v)
{
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
    const ShareState *st = share_state(v->share);
    GPtrArray *copy = g_ptr_array_new_with_free_func(destroy_contact);
    for (guint i = 0; st->contacts && i < st->contacts->len; i++) {
        if ((int)i == idx) continue;
        ShareContact *src_c = st->contacts->pdata[i];
        ShareContact *c = g_new0(ShareContact, 1);
        c->name = g_strdup(src_c->name);
        c->device = g_strdup(src_c->device);
        c->email = g_strdup(src_c->email);
        c->phone = g_strdup(src_c->phone);
        c->photo = g_strdup(src_c->photo);
        g_ptr_array_add(copy, c);
    }
    share_save_contacts(v->share, copy);
    g_ptr_array_unref(copy);
    refresh_contacts_dialog(v);
}

static void on_contact_photo(GtkButton *b, ShareView *v)
{
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
    launch_photo_picker(v, GTK_WINDOW(v->contacts_win), NULL, idx);
}

static void on_draft_photo(GtkButton *b, ShareView *v)
{
    launch_photo_picker(v, GTK_WINDOW(v->contacts_win), NULL, -2);
}

static void on_contact_add(GtkButton *b, ShareView *v)
{
    const char *name = gtk_editable_get_text(GTK_EDITABLE(v->entry_name));
    const char *dev = gtk_editable_get_text(GTK_EDITABLE(v->entry_device));
    const char *email = gtk_editable_get_text(GTK_EDITABLE(v->entry_email));
    const char *phone = gtk_editable_get_text(GTK_EDITABLE(v->entry_phone));
    if (!*name && !*dev && !*email && !*phone) return;

    const ShareState *st = share_state(v->share);
    GPtrArray *copy = g_ptr_array_new_with_free_func(destroy_contact);
    for (guint i = 0; st->contacts && i < st->contacts->len; i++) {
        ShareContact *src_c = st->contacts->pdata[i];
        ShareContact *c = g_new0(ShareContact, 1);
        c->name = g_strdup(src_c->name);
        c->device = g_strdup(src_c->device);
        c->email = g_strdup(src_c->email);
        c->phone = g_strdup(src_c->phone);
        c->photo = g_strdup(src_c->photo);
        g_ptr_array_add(copy, c);
    }
    ShareContact *nc = g_new0(ShareContact, 1);
    nc->name = g_strdup(*name ? name : (*dev ? dev : (*email ? email : phone)));
    nc->device = *dev ? g_strdup(dev) : NULL;
    nc->email = *email ? g_strdup(email) : NULL;
    nc->phone = *phone ? g_strdup(phone) : NULL;
    nc->photo = v->draft_photo ? g_strdup(v->draft_photo) : NULL;
    g_ptr_array_add(copy, nc);

    share_save_contacts(v->share, copy);
    g_ptr_array_unref(copy);

    gtk_editable_set_text(GTK_EDITABLE(v->entry_name), "");
    gtk_editable_set_text(GTK_EDITABLE(v->entry_device), "");
    gtk_editable_set_text(GTK_EDITABLE(v->entry_email), "");
    gtk_editable_set_text(GTK_EDITABLE(v->entry_phone), "");
    g_clear_pointer(&v->draft_photo, g_free);
    gtk_button_set_label(GTK_BUTTON(v->draft_photo_btn), "Choose Photo…");
    refresh_contacts_dialog(v);
}

static void refresh_contacts_dialog(ShareView *v)
{
    if (!v->contacts_list) return;
    clear_box(v->contacts_list);
    const ShareState *st = share_state(v->share);
    if (!st->contacts || !st->contacts->len) {
        GtkWidget *empty = gtk_label_new("No contacts saved yet. Add one below or right-click a nearby device.");
        gtk_widget_add_css_class(empty, "airdrop-status");
        gtk_label_set_xalign(GTK_LABEL(empty), 0);
        gtk_box_append(GTK_BOX(v->contacts_list), empty);
        return;
    }
    for (guint i = 0; i < st->contacts->len; i++) {
        ShareContact *c = st->contacts->pdata[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(texts, TRUE);
        GtkWidget *nl = gtk_label_new(c->name ? c->name : (c->device ? c->device : "Contact"));
        gtk_widget_add_css_class(nl, "airdrop-popover-title");
        gtk_label_set_xalign(GTK_LABEL(nl), 0);
        gtk_box_append(GTK_BOX(texts), nl);

        GString *meta = g_string_new(NULL);
        if (c->email && *c->email) g_string_append(meta, c->email);
        if (c->phone && *c->phone) { if (meta->len) g_string_append(meta, " · "); g_string_append(meta, c->phone); }
        if (c->device && *c->device) { if (meta->len) g_string_append(meta, " · "); g_string_append(meta, c->device); }
        GtkWidget *ml = gtk_label_new(meta->len ? meta->str : "Avatar photo");
        gtk_widget_add_css_class(ml, "airdrop-status");
        gtk_label_set_xalign(GTK_LABEL(ml), 0);
        gtk_label_set_ellipsize(GTK_LABEL(ml), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(texts), ml);
        g_string_free(meta, TRUE);
        gtk_box_append(GTK_BOX(row), texts);

        GtkWidget *pb = gtk_button_new_with_label("Photo");
        gtk_widget_add_css_class(pb, "airdrop-btn");
        g_object_set_data(G_OBJECT(pb), "idx", GINT_TO_POINTER((int)i));
        g_signal_connect(pb, "clicked", G_CALLBACK(on_contact_photo), v);
        gtk_box_append(GTK_BOX(row), pb);

        GtkWidget *rb = gtk_button_new_with_label("Remove");
        gtk_widget_add_css_class(rb, "airdrop-btn");
        g_object_set_data(G_OBJECT(rb), "idx", GINT_TO_POINTER((int)i));
        g_signal_connect(rb, "clicked", G_CALLBACK(on_contact_remove), v);
        gtk_box_append(GTK_BOX(row), rb);
        gtk_box_append(GTK_BOX(v->contacts_list), row);
    }
}

static gboolean on_contacts_close(GtkWindow *w, ShareView *v)
{
    v->contacts_win = NULL;
    v->contacts_list = NULL;
    v->entry_name = NULL;
    v->entry_device = NULL;
    v->entry_email = NULL;
    v->entry_phone = NULL;
    v->draft_photo_btn = NULL;
    return FALSE;
}

static void on_open_contacts(GtkButton *b, ShareView *v)
{
    if (v->contacts_win) {
        gtk_window_present(GTK_WINDOW(v->contacts_win));
        return;
    }
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "AirDrop Contacts & Avatars");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(v->app->win));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 440, -1);
    gtk_widget_add_css_class(win, "lares");
    g_signal_connect(win, "close-request", G_CALLBACK(on_contacts_close), v);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(box, "airdrop-stage");
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *sub = gtk_label_new("Used locally to verify Apple ID hashes in Contacts Only mode and display profile photos.");
    gtk_widget_add_css_class(sub, "airdrop-status");
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_append(GTK_BOX(box), sub);

    v->contacts_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(box), v->contacts_list);

    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    v->entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->entry_name), "Name (e.g. Susuin)");
    gtk_widget_set_hexpand(v->entry_name, TRUE);
    v->entry_device = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->entry_device), "Device name (optional)");
    gtk_widget_set_hexpand(v->entry_device, TRUE);
    gtk_box_append(GTK_BOX(row1), v->entry_name);
    gtk_box_append(GTK_BOX(row1), v->entry_device);
    gtk_box_append(GTK_BOX(box), row1);

    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    v->entry_email = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->entry_email), "Apple ID email");
    gtk_widget_set_hexpand(v->entry_email, TRUE);
    v->entry_phone = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->entry_phone), "Phone (+1…)");
    gtk_widget_set_hexpand(v->entry_phone, TRUE);
    gtk_box_append(GTK_BOX(row2), v->entry_email);
    gtk_box_append(GTK_BOX(row2), v->entry_phone);
    gtk_box_append(GTK_BOX(box), row2);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    v->draft_photo_btn = gtk_button_new_with_label("Choose Photo…");
    gtk_widget_add_css_class(v->draft_photo_btn, "airdrop-btn");
    g_signal_connect(v->draft_photo_btn, "clicked", G_CALLBACK(on_draft_photo), v);
    gtk_box_append(GTK_BOX(actions), v->draft_photo_btn);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(actions), spacer);

    GtkWidget *add = gtk_button_new_with_label("Add Contact");
    gtk_widget_add_css_class(add, "airdrop-btn-primary");
    g_signal_connect(add, "clicked", G_CALLBACK(on_contact_add), v);
    gtk_box_append(GTK_BOX(actions), add);
    gtk_box_append(GTK_BOX(box), actions);

    gtk_window_set_child(GTK_WINDOW(win), box);
    v->contacts_win = win;
    refresh_contacts_dialog(v);
    gtk_window_present(GTK_WINDOW(win));
}

/* ------------------------------------------------------------- Popover & Devices */

static ShareDevice *active_outgoing_device(const ShareState *st)
{
    ShareDevice *fallback = NULL;
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        if (d->state == DEVICE_SENDING || d->state == DEVICE_ASKING) return d;
        if (d->state != DEVICE_FOUND && !fallback) fallback = d;
    }
    return fallback;
}

static char *compute_popover_shape(const ShareState *st)
{
    if (st->incoming) {
        return g_strdup_printf("in:%u:%d:%s", st->incoming->id, st->incoming->state,
                               st->incoming->message ? st->incoming->message : "");
    }
    ShareDevice *d = active_outgoing_device(st);
    if (d) {
        return g_strdup_printf("out:%s:%d:%s", d->key, d->state, d->message ? d->message : "");
    }
    return NULL;
}

static char *compute_devices_shape(const ShareState *st)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "c:%u|", st->contacts ? st->contacts->len : 0);
    if (st->incoming) g_string_append_printf(s, "in:%s:%d|", st->incoming->sender ? st->incoming->sender : "", st->incoming->state);
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        g_string_append_printf(s, "%s:%s:%s:%d;", d->key, d->name ? d->name : "", d->model ? d->model : "", d->state);
    }
    return g_string_free(s, FALSE);
}

static GtkWidget *popover_button(const char *label, gboolean primary, GCallback cb, gpointer user)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, primary ? "airdrop-btn-primary" : "airdrop-btn");
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_signal_connect(b, "clicked", cb, user);
    return b;
}

static void rebuild_popover(ShareView *v, const ShareState *st)
{
    clear_box(v->popover_slot);
    v->popover_bar = NULL;
    v->popover_detail = NULL;

    const ShareIncoming *in = st->incoming;
    ShareDevice *out = in ? NULL : active_outgoing_device(st);
    if (!in && !out) {
        gtk_widget_set_visible(v->popover_slot, FALSE);
        return;
    }

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "airdrop-popover");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);

    /* Left 46x46 preview thumbnail */
    cairo_surface_t *thumb = NULL;
    if (in) {
        if (in->file_icon) thumb = load_bytes_surface(in->file_icon, 92);
        else if (in->state == INCOMING_DONE && in->saved && in->saved->len &&
                 in->names && in->names->len && looks_like_image(in->names->pdata[0])) {
            thumb = load_photo_surface(in->saved->pdata[0], 92);
        }
    } else if (st->paths && st->paths[0] && looks_like_image(st->paths[0])) {
        thumb = load_photo_surface(st->paths[0], 92);
    }
    GtkWidget *tda = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(tda), 46);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(tda), 46);
    gtk_widget_set_valign(tda, GTK_ALIGN_START);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(tda), draw_thumb, thumb,
                                   thumb ? destroy_surface : NULL);
    gtk_box_append(GTK_BOX(card), tda);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(card), col);

    char *title = NULL;
    char *detail = NULL;
    gboolean show_bar = FALSE;
    gboolean show_cancel = FALSE;

    if (in) {
        const ShareContact *c = share_find_contact(st, in->sender, in->matched_hash);
        const char *who = (c && c->name && *c->name) ? c->name : (in->sender ? in->sender : "Someone nearby");
        char *what = offer_text(in);
        switch (in->state) {
        case INCOMING_ASKING:
            title = in->links ? g_strdup_printf("%s would like to share a link.", who)
                              : g_strdup_printf("%s would like to share “%s”.", who, what);
            if (in->total) {
                char *sz = size_text(in->total);
                detail = g_strdup_printf("%s · Saved to Downloads", sz);
                g_free(sz);
            } else {
                detail = g_strdup("Saved to Downloads when accepted");
            }
            break;
        case INCOMING_RECEIVING:
            title = g_strdup_printf("Receiving “%s”.", what);
            show_bar = TRUE;
            detail = g_strdup("");
            break;
        case INCOMING_DONE:
            title = in->links ? g_strdup("Received a link.") : g_strdup_printf("Received “%s”.", what);
            detail = g_strdup(in->links && in->saved && in->saved->len ? (const char *)in->saved->pdata[0]
                                                                       : "Saved in Downloads");
            break;
        case INCOMING_FAILED:
            if (in->message && (strstr(in->message, "cancel") || strstr(in->message, "Cancel"))) {
                title = g_strdup_printf("%s canceled the copy.", who);
                detail = g_strdup_printf("“%s”", what);
            } else {
                title = g_strdup_printf("Couldn't receive “%s”.", what);
                detail = g_strdup(in->message ? in->message : "The transfer stopped.");
            }
            break;
        }
        g_free(what);
    } else {
        const ShareContact *c = share_find_contact(st, out->name, NULL);
        const char *who = (c && c->name && *c->name) ? c->name : (out->name ? out->name : "device");
        char *first = st->paths && st->paths[0] ? g_path_get_basename(st->paths[0]) : g_strdup("files");
        guint n = st->paths ? g_strv_length(st->paths) : 0;
        char *what = n > 1 ? g_strdup_printf("%s and %u more", first, n - 1) : g_strdup(first);
        g_free(first);
        switch (out->state) {
        case DEVICE_ASKING:
            title = g_strdup_printf("Waiting for %s…", who);
            detail = g_strdup_printf("Sending “%s”", what);
            show_bar = TRUE;
            show_cancel = TRUE;
            break;
        case DEVICE_SENDING:
            title = g_strdup_printf("Sending “%s”.", what);
            detail = g_strdup("");
            show_bar = TRUE;
            show_cancel = TRUE;
            break;
        case DEVICE_SENT:
            title = g_strdup_printf("Sent “%s”.", what);
            detail = g_strdup_printf("Delivered to %s", who);
            break;
        case DEVICE_DECLINED:
            title = g_strdup_printf("%s declined.", who);
            detail = g_strdup("The transfer was declined on the other device.");
            break;
        case DEVICE_FAILED:
            title = g_strdup_printf("Couldn't send to %s.", who);
            detail = g_strdup(out->message ? out->message : "The transfer stopped.");
            break;
        case DEVICE_FOUND:
            break;
        }
        g_free(what);
    }

    GtkWidget *tl = gtk_label_new(title ? title : "");
    gtk_widget_add_css_class(tl, "airdrop-popover-title");
    gtk_label_set_xalign(GTK_LABEL(tl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(tl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(col), tl);
    g_free(title);

    if (show_bar) {
        GtkWidget *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        v->popover_bar = gtk_progress_bar_new();
        gtk_widget_add_css_class(v->popover_bar, "airdrop-progress");
        gtk_widget_set_hexpand(v->popover_bar, TRUE);
        gtk_widget_set_valign(v->popover_bar, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(brow), v->popover_bar);
        if (show_cancel) {
            GtkWidget *xb = gtk_button_new_with_label("×");
            gtk_widget_add_css_class(xb, "airdrop-cancel-x");
            gtk_widget_set_cursor_from_name(xb, "pointer");
            g_signal_connect(xb, "clicked", G_CALLBACK(on_cancel_send), v);
            gtk_box_append(GTK_BOX(brow), xb);
        }
        gtk_box_append(GTK_BOX(col), brow);
    }

    v->popover_detail = gtk_label_new(detail ? detail : "");
    gtk_widget_add_css_class(v->popover_detail, "airdrop-popover-sub");
    gtk_label_set_xalign(GTK_LABEL(v->popover_detail), 0);
    gtk_label_set_ellipsize(GTK_LABEL(v->popover_detail), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(col), v->popover_detail);
    g_free(detail);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    if (in) {
        if (in->state == INCOMING_ASKING) {
            gtk_box_append(GTK_BOX(btns), popover_button("Decline", FALSE, G_CALLBACK(on_decline), v));
            gtk_box_append(GTK_BOX(btns), popover_button("Accept", TRUE, G_CALLBACK(on_accept), v));
        } else if (in->state == INCOMING_DONE && in->links) {
            gtk_box_append(GTK_BOX(btns), popover_button("OK", FALSE, G_CALLBACK(on_dismiss_in), v));
            if (in->saved && in->saved->len) {
                GtkWidget *ob = popover_button("Open Link", TRUE, G_CALLBACK(on_open_link), v);
                g_object_set_data_full(G_OBJECT(ob), "uri", g_strdup(in->saved->pdata[0]), g_free);
                gtk_box_append(GTK_BOX(btns), ob);
            }
        } else if (in->state == INCOMING_DONE) {
            gtk_box_append(GTK_BOX(btns), popover_button("OK", FALSE, G_CALLBACK(on_dismiss_in), v));
            gtk_box_append(GTK_BOX(btns), popover_button("Show in Folder", TRUE, G_CALLBACK(on_show_folder), v));
        } else if (in->state == INCOMING_FAILED) {
            gtk_box_append(GTK_BOX(btns), popover_button("OK", FALSE, G_CALLBACK(on_dismiss_in), v));
        }
    } else if (out->state == DEVICE_SENT || out->state == DEVICE_DECLINED || out->state == DEVICE_FAILED) {
        GtkWidget *ok = popover_button("OK", FALSE, G_CALLBACK(on_dismiss_dev), v);
        g_object_set_data_full(G_OBJECT(ok), "key", g_strdup(out->key), g_free);
        gtk_box_append(GTK_BOX(btns), ok);
        if (out->state == DEVICE_DECLINED || out->state == DEVICE_FAILED) {
            GtkWidget *sa = popover_button("Send Again", TRUE, G_CALLBACK(on_send_again), v);
            g_object_set_data_full(G_OBJECT(sa), "key", g_strdup(out->key), g_free);
            gtk_box_append(GTK_BOX(btns), sa);
        }
    }
    if (gtk_widget_get_first_child(btns)) gtk_box_append(GTK_BOX(col), btns);
    else { g_object_ref_sink(btns); g_object_unref(btns); }

    gtk_box_append(GTK_BOX(v->popover_slot), card);
    gtk_widget_set_visible(v->popover_slot, TRUE);
}

static void add_device_node(ShareView *v, const char *key, const char *raw_name, const char *model,
                            const char *matched_hash, gboolean clickable,
                            gboolean active_prog, double frac, gboolean waiting, gboolean done)
{
    const ShareState *st = share_state(v->share);
    const ShareContact *c = share_find_contact(st, raw_name, matched_hash);
    const char *disp = (c && c->name && *c->name) ? c->name : (raw_name && *raw_name ? raw_name : "Apple Device");
    const char *sub = share_model_label(model, raw_name);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box, "airdrop-device");
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    if (clickable) {
        gtk_widget_set_cursor_from_name(box, "pointer");
        g_object_set_data_full(G_OBJECT(box), "dev-key", g_strdup(key), g_free);
        g_object_set_data_full(G_OBJECT(box), "dev-name", g_strdup(disp), g_free);
    }

    AvatarData *ad = g_new0(AvatarData, 1);
    ad->key = g_strdup(key);
    ad->name = g_strdup(disp);
    ad->subtitle = g_strdup(sub);
    ad->kind = share_device_kind(model, raw_name);
    if (c && c->photo && *c->photo) ad->photo = load_photo_surface(c->photo, 136);
    ad->active_progress = active_prog;
    ad->progress = frac;
    ad->waiting = waiting;
    ad->done = done;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), 78);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 78);
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(da), "avatar-data", ad);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_avatar, ad, avatar_data_free);
    gtk_box_append(GTK_BOX(box), da);
    g_hash_table_insert(v->avatars, g_strdup(key), da);

    GtkWidget *nl = gtk_label_new(disp);
    gtk_widget_add_css_class(nl, "airdrop-device-name");
    gtk_label_set_max_width_chars(GTK_LABEL(nl), 16);
    gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), nl);

    GtkWidget *sl = gtk_label_new(sub);
    gtk_widget_add_css_class(sl, "airdrop-device-sub");
    gtk_label_set_max_width_chars(GTK_LABEL(sl), 16);
    gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), sl);

    if (clickable) {
        GtkGesture *left = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left), 1);
        g_signal_connect(left, "released", G_CALLBACK(on_device_left_click), v);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(left));

        GtkGesture *right = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), 3);
        g_signal_connect(right, "released", G_CALLBACK(on_device_right_click), v);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(right));

        GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(drop, "enter", G_CALLBACK(on_device_drag_enter), da);
        g_signal_connect(drop, "leave", G_CALLBACK(on_device_drag_leave), da);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop_files), v);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));
    }

    gtk_box_append(GTK_BOX(v->devices_row), box);
}

static void rebuild_devices(ShareView *v, const ShareState *st)
{
    clear_box(v->devices_row);
    g_hash_table_remove_all(v->avatars);

    gboolean incoming_attached = FALSE;
    for (guint i = 0; i < st->devices->len; i++) {
        ShareDevice *d = st->devices->pdata[i];
        gboolean match_in = st->incoming && !incoming_attached && st->incoming->sender && d->name &&
                            g_ascii_strcasecmp(st->incoming->sender, d->name) == 0;
        gboolean active = FALSE, waiting = FALSE, done = FALSE;
        double frac = 0.0;
        const char *model = d->model;
        const char *mhash = NULL;
        if (match_in) {
            incoming_attached = TRUE;
            if (!model && st->incoming->sender_model) model = st->incoming->sender_model;
            mhash = st->incoming->matched_hash;
            active = st->incoming->state == INCOMING_RECEIVING;
            frac = st->incoming->total ? MIN(1.0, (double)st->incoming->done / st->incoming->total) : 0.35;
            waiting = st->incoming->state == INCOMING_ASKING;
            done = st->incoming->state == INCOMING_DONE;
        } else {
            active = d->state == DEVICE_SENDING;
            frac = d->total ? MIN(1.0, (double)d->sent / d->total) : 0.35;
            waiting = d->state == DEVICE_ASKING;
            done = d->state == DEVICE_SENT;
        }
        add_device_node(v, d->key, d->name, model, mhash, TRUE, active, frac, waiting, done);
    }

    if (st->incoming && !incoming_attached) {
        const ShareIncoming *in = st->incoming;
        double frac = in->total ? MIN(1.0, (double)in->done / in->total) : 0.35;
        add_device_node(v, "__incoming__", in->sender, in->sender_model, in->matched_hash, FALSE,
                        in->state == INCOMING_RECEIVING, frac,
                        in->state == INCOMING_ASKING, in->state == INCOMING_DONE);
    }
}

static void update_progress_in_place(ShareView *v, const ShareState *st)
{
    if (st->incoming && st->incoming->state == INCOMING_RECEIVING) {
        const ShareIncoming *in = st->incoming;
        double frac = in->total ? MIN(1.0, (double)in->done / in->total) : 0.35;
        if (v->popover_bar) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->popover_bar), frac);
        if (v->popover_detail) {
            char *done_s = size_text(in->done);
            char *tot_s = in->total ? size_text(in->total) : NULL;
            gint64 elapsed = st->xfer_start_us ? (g_get_monotonic_time() - st->xfer_start_us) : 0;
            char *eta = eta_text(in->done, in->total, elapsed);
            char *line = tot_s ? (eta ? g_strdup_printf("%s of %s - %s", done_s, tot_s, eta)
                                      : g_strdup_printf("%s of %s", done_s, tot_s))
                               : g_strdup(done_s);
            gtk_label_set_text(GTK_LABEL(v->popover_detail), line);
            g_free(line);
            g_free(eta);
            g_free(tot_s);
            g_free(done_s);
        }
        GtkWidget *da = g_hash_table_lookup(v->avatars, "__incoming__");
        if (!da && in->sender) {
            for (guint i = 0; i < st->devices->len; i++) {
                ShareDevice *d = st->devices->pdata[i];
                if (d->name && g_ascii_strcasecmp(d->name, in->sender) == 0) {
                    da = g_hash_table_lookup(v->avatars, d->key);
                    break;
                }
            }
        }
        if (da) {
            AvatarData *ad = g_object_get_data(G_OBJECT(da), "avatar-data");
            if (ad) { ad->progress = frac; gtk_widget_queue_draw(da); }
        }
        return;
    }

    ShareDevice *out = active_outgoing_device(st);
    if (out && out->state == DEVICE_SENDING) {
        double frac = out->total ? MIN(1.0, (double)out->sent / out->total) : 0.35;
        if (v->popover_bar) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->popover_bar), frac);
        if (v->popover_detail) {
            char *done_s = size_text(out->sent);
            char *tot_s = out->total ? size_text(out->total) : NULL;
            gint64 elapsed = st->xfer_start_us ? (g_get_monotonic_time() - st->xfer_start_us) : 0;
            char *eta = eta_text(out->sent, out->total, elapsed);
            char *line = tot_s ? (eta ? g_strdup_printf("%s of %s - %s", done_s, tot_s, eta)
                                      : g_strdup_printf("%s of %s", done_s, tot_s))
                               : g_strdup(done_s);
            gtk_label_set_text(GTK_LABEL(v->popover_detail), line);
            g_free(line);
            g_free(eta);
            g_free(tot_s);
            g_free(done_s);
        }
        GtkWidget *da = g_hash_table_lookup(v->avatars, out->key);
        if (da) {
            AvatarData *ad = g_object_get_data(G_OBJECT(da), "avatar-data");
            if (ad) { ad->progress = frac; gtk_widget_queue_draw(da); }
        }
    } else if (out && out->state == DEVICE_ASKING && v->popover_bar) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->popover_bar), 0.14);
    }
}

/* ------------------------------------------------------------- status & tick */

static void update_status_line(ShareView *v)
{
    const ShareState *st = share_state(v->share);
    char *text = NULL;
    gboolean is_err = FALSE;
    const char *pc = st->name ? st->name : "this PC";

    if (st->link == LINK_FAILED) {
        text = g_strdup(st->link_error ? st->link_error : "AirDrop stopped.");
        is_err = TRUE;
    } else if (st->link == LINK_UP && st->link_error) {
        text = g_strdup(st->link_error);
        is_err = TRUE;
    } else if (st->mode == SHARE_DISCOVER_EVERYONE) {
        gint64 left = (st->receive_until - g_get_monotonic_time()) / G_USEC_PER_SEC;
        if (left < 0) left = 0;
        text = g_strdup_printf("%s %s · %d:%02d left",
                               st->link == LINK_UP ? "Visible as" : "Starting AirDrop as",
                               pc, (int)(left / 60), (int)(left % 60));
    } else if (st->mode == SHARE_DISCOVER_CONTACTS) {
        guint n = st->contacts ? st->contacts->len : 0;
        if (!n) text = g_strdup("Contacts Only · Add a contact to verify nearby Apple devices");
        else text = g_strdup_printf("Visible to %u %s as %s", n, n == 1 ? "contact" : "contacts", pc);
    } else if (!st->devices->len && !st->incoming) {
        if (st->quiet && st->link == LINK_UP)
            text = g_strdup("No Apple devices found nearby yet · Open AirDrop on your iPhone, iPad or Mac");
        else if (st->browsing)
            text = g_strdup("Looking for nearby Apple devices…");
        else if (st->card_in_use)
            text = g_strdup("Your Wi-Fi card is in use · Click Scan or pick a discovery mode to start AirDrop");
    } else {
        text = g_strdup("Click a person or drop files onto them to share · Right-click to set a photo");
    }

    gtk_label_set_text(GTK_LABEL(v->status_label), text ? text : "");
    gtk_widget_set_visible(v->status_label, text != NULL);
    ui_class(v->status_label, "err", is_err);
    g_free(text);
}

static gboolean on_tick(gpointer data)
{
    ShareView *v = data;
    const ShareState *st = share_state(v->share);
    update_status_line(v);
    update_progress_in_place(v, st);
    return G_SOURCE_CONTINUE;
}

static void sync_tick(ShareView *v)
{
    const ShareState *st = share_state(v->share);
    gboolean xfer = (st->incoming && st->incoming->state == INCOMING_RECEIVING) ||
                    (active_outgoing_device(st) && active_outgoing_device(st)->state == DEVICE_SENDING);
    gboolean want = v->on_screen && (st->mode == SHARE_DISCOVER_EVERYONE || xfer);
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
    gtk_button_set_child(GTK_BUTTON(v->allow), ui_busy_child("Allow AirDrop", st->allow_busy));
    gtk_label_set_text(GTK_LABEL(v->allow_error), st->allow_error ? st->allow_error : "");
    gtk_widget_set_visible(v->allow_error, st->allow_error != NULL);

    /* Discoverability trigger and menu checkmarks */
    char *dl = g_strdup_printf("Allow me to be discovered by: %s ▾", discover_mode_label(st->mode));
    gtk_label_set_text(GTK_LABEL(v->discover_label), dl);
    g_free(dl);
    for (int i = 0; i < 3; i++) {
        gtk_label_set_text(GTK_LABEL(v->mode_checks[i]), (int)st->mode == i ? "✓" : "");
    }

    /* Contacts button count */
    guint nc = st->contacts ? st->contacts->len : 0;
    char *cl = nc ? g_strdup_printf("Contacts (%u)", nc) : g_strdup("Contacts");
    gtk_button_set_label(GTK_BUTTON(v->contacts_btn), cl);
    g_free(cl);

    /* Manual scan button only when card is shared with active Wi-Fi */
    gtk_widget_set_visible(v->scan_btn, st->card_in_use);
    gtk_button_set_label(GTK_BUTTON(v->scan_btn), st->browsing ? "Stop Scanning" : "Scan Nearby");

    /* Staged files pill */
    if (st->sending && st->paths && st->paths[0]) {
        char *base = g_path_get_basename(st->paths[0]);
        guint n = g_strv_length(st->paths);
        char *pl = n > 1 ? g_strdup_printf("Ready to send: %s +%u", base, n - 1)
                         : g_strdup_printf("Ready to send: %s", base);
        gtk_label_set_text(GTK_LABEL(v->staged_label), pl);
        g_free(pl);
        g_free(base);
        gtk_widget_set_visible(v->staged_pill, TRUE);
    } else {
        gtk_widget_set_visible(v->staged_pill, FALSE);
    }

    /* Adapter popover details */
    gtk_label_set_text(GTK_LABEL(v->card_value), st->card_text ? st->card_text : "None");
    const char *lstr = st->link == LINK_UP       ? "Link: Active (AWDL)"
                     : st->link == LINK_STARTING ? "Link: Starting…"
                     : st->link == LINK_FAILED   ? "Link: Stopped"
                                                 : "Link: Idle";
    gtk_label_set_text(GTK_LABEL(v->card_link), lstr);
    GString *note = g_string_new(NULL);
    if (st->card_in_use) g_string_append(note, "This card carries your Wi-Fi and disconnects while AirDrop is active. ");
    if (st->card_text && !st->card_5ghz && strstr(st->card_text, "2.4 GHz only"))
        g_string_append(note, "On 2.4 GHz alone, photos take about a minute each.");
    gtk_label_set_text(GTK_LABEL(v->card_note), note->str);
    gtk_widget_set_visible(v->card_note, note->len > 0);
    g_string_free(note, TRUE);

    /* Rebuild nearby device circles when structure changes */
    char *dshape = compute_devices_shape(st);
    if (g_strcmp0(dshape, v->devices_shape) != 0) {
        g_free(v->devices_shape);
        v->devices_shape = dshape;
        rebuild_devices(v, st);
    } else {
        g_free(dshape);
    }

    /* Rebuild floating transfer popover when transfer phase changes */
    char *pshape = compute_popover_shape(st);
    if (g_strcmp0(pshape, v->popover_shape) != 0) {
        g_free(v->popover_shape);
        v->popover_shape = pshape;
        rebuild_popover(v, st);
    } else {
        g_free(pshape);
    }

    update_progress_in_place(v, st);
    update_status_line(v);
    sync_tick(v);
}

static void on_changed(gpointer data) { update(data); }

/* -------------------------------------------------------------- build */

GtkWidget *share_build(Lares *app, Section *self)
{
    ShareView *v = g_new0(ShareView, 1);
    v->app = app;
    v->share = share_new();
    v->avatars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->view = v;

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(page, TRUE);
    gtk_widget_set_vexpand(page, TRUE);

    v->stage = gtk_overlay_new();
    gtk_widget_add_css_class(v->stage, "airdrop-stage");
    gtk_widget_set_overflow(v->stage, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_hexpand(v->stage, TRUE);
    gtk_widget_set_vexpand(v->stage, TRUE);
    gtk_box_append(GTK_BOX(page), v->stage);

    /* Stage-level drag-and-drop stages dropped files so clicking any device sends them */
    GtkDropTarget *stage_drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(stage_drop, "drop", G_CALLBACK(on_drop_files), v);
    gtk_widget_add_controller(v->stage, GTK_EVENT_CONTROLLER(stage_drop));

    /* Background concentric radar rings */
    v->radar = gtk_drawing_area_new();
    gtk_widget_add_css_class(v->radar, "airdrop-radar");
    gtk_widget_set_can_target(v->radar, FALSE);
    gtk_widget_set_hexpand(v->radar, TRUE);
    gtk_widget_set_vexpand(v->radar, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->radar), draw_radar, v, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(v->stage), v->radar);

    /* Setup overlay (when lares-awdl still needs Allow) */
    v->setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_halign(v->setup_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(v->setup_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(v->setup_box, 36);
    gtk_widget_set_margin_end(v->setup_box, 36);
    GtkWidget *setup_emblem = gtk_drawing_area_new();
    gtk_widget_add_css_class(setup_emblem, "airdrop-emblem");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(setup_emblem), 68);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(setup_emblem), 68);
    gtk_widget_set_halign(setup_emblem, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(setup_emblem), draw_emblem, v, NULL);
    gtk_box_append(GTK_BOX(v->setup_box), setup_emblem);

    GtkWidget *stitle = gtk_label_new("Allow AirDrop on this PC");
    gtk_widget_add_css_class(stitle, "airdrop-popover-title");
    gtk_box_append(GTK_BOX(v->setup_box), stitle);

    GtkWidget *sdesc = gtk_label_new("Lares lends AirDrop your Wi-Fi card while it is active and lets AirDrop alone through the firewall. That needs your password once.");
    gtk_widget_add_css_class(sdesc, "airdrop-status");
    gtk_label_set_wrap(GTK_LABEL(sdesc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(sdesc), 52);
    gtk_label_set_justify(GTK_LABEL(sdesc), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(v->setup_box), sdesc);

    v->allow = gtk_button_new();
    gtk_widget_add_css_class(v->allow, "airdrop-btn-primary");
    gtk_widget_set_halign(v->allow, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(v->allow, "pointer");
    g_signal_connect(v->allow, "clicked", G_CALLBACK(on_allow), v);
    gtk_box_append(GTK_BOX(v->setup_box), v->allow);

    v->allow_error = gtk_label_new("");
    gtk_widget_add_css_class(v->allow_error, "airdrop-status");
    gtk_widget_add_css_class(v->allow_error, "err");
    gtk_box_append(GTK_BOX(v->setup_box), v->allow_error);
    gtk_overlay_add_overlay(GTK_OVERLAY(v->stage), v->setup_box);

    /* Ready macOS AirDrop stage */
    v->ready_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(v->ready_box, TRUE);
    gtk_widget_set_vexpand(v->ready_box, TRUE);

    /* Top toolbar */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(top, 14);
    gtk_widget_set_margin_start(top, 16);
    gtk_widget_set_margin_end(top, 16);

    v->staged_pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(v->staged_pill, "airdrop-staged-pill");
    v->staged_label = gtk_label_new("");
    gtk_label_set_max_width_chars(GTK_LABEL(v->staged_label), 28);
    gtk_label_set_ellipsize(GTK_LABEL(v->staged_label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(v->staged_pill), v->staged_label);
    GtkWidget *clr = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(clr, "airdrop-cancel-x");
    g_signal_connect(clr, "clicked", G_CALLBACK(on_clear_staged), v);
    gtk_box_append(GTK_BOX(v->staged_pill), clr);
    gtk_box_append(GTK_BOX(top), v->staged_pill);

    v->scan_btn = gtk_button_new_with_label("Scan Nearby");
    gtk_widget_add_css_class(v->scan_btn, "airdrop-pill");
    gtk_widget_set_cursor_from_name(v->scan_btn, "pointer");
    g_signal_connect(v->scan_btn, "clicked", G_CALLBACK(on_toggle_scan), v);
    gtk_box_append(GTK_BOX(top), v->scan_btn);

    GtkWidget *top_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(top_spacer, TRUE);
    gtk_box_append(GTK_BOX(top), top_spacer);

    v->contacts_btn = gtk_button_new_with_label("Contacts");
    gtk_widget_add_css_class(v->contacts_btn, "airdrop-pill");
    gtk_widget_set_cursor_from_name(v->contacts_btn, "pointer");
    g_signal_connect(v->contacts_btn, "clicked", G_CALLBACK(on_open_contacts), v);
    gtk_box_append(GTK_BOX(top), v->contacts_btn);

    v->adapter_btn = gtk_menu_button_new();
    gtk_widget_add_css_class(v->adapter_btn, "airdrop-pill");
    gtk_menu_button_set_label(GTK_MENU_BUTTON(v->adapter_btn), "Adapter");
    GtkWidget *apop = gtk_popover_new();
    GtkWidget *abox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(abox, 8);
    gtk_widget_set_margin_bottom(abox, 8);
    gtk_widget_set_margin_start(abox, 10);
    gtk_widget_set_margin_end(abox, 10);
    GtkWidget *ah = gtk_label_new("Wi-Fi Adapter");
    gtk_widget_add_css_class(ah, "airdrop-popover-title");
    gtk_label_set_xalign(GTK_LABEL(ah), 0);
    gtk_box_append(GTK_BOX(abox), ah);
    v->card_value = gtk_label_new("");
    gtk_widget_add_css_class(v->card_value, "airdrop-status");
    gtk_label_set_xalign(GTK_LABEL(v->card_value), 0);
    gtk_box_append(GTK_BOX(abox), v->card_value);
    v->card_link = gtk_label_new("");
    gtk_widget_add_css_class(v->card_link, "airdrop-status");
    gtk_label_set_xalign(GTK_LABEL(v->card_link), 0);
    gtk_box_append(GTK_BOX(abox), v->card_link);
    v->card_note = gtk_label_new("");
    gtk_widget_add_css_class(v->card_note, "airdrop-status");
    gtk_label_set_wrap(GTK_LABEL(v->card_note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v->card_note), 34);
    gtk_label_set_xalign(GTK_LABEL(v->card_note), 0);
    gtk_box_append(GTK_BOX(abox), v->card_note);
    gtk_popover_set_child(GTK_POPOVER(apop), abox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(v->adapter_btn), apop);
    gtk_box_append(GTK_BOX(top), v->adapter_btn);
    gtk_box_append(GTK_BOX(v->ready_box), top);

    /* Center radar device zone (floating transfer popover + circular nearby device avatars) */
    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_vexpand(center, TRUE);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);

    v->popover_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(v->popover_slot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(center), v->popover_slot);

    v->devices_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_halign(v->devices_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(center), v->devices_row);
    gtk_box_append(GTK_BOX(v->ready_box), center);

    /* Bottom macOS identity block: blue AirDrop logo + tagline + inline discoverability dropdown */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign(bottom, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bottom, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(bottom, 22);

    v->emblem = gtk_drawing_area_new();
    gtk_widget_add_css_class(v->emblem, "airdrop-emblem");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->emblem), 68);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->emblem), 68);
    gtk_widget_set_halign(v->emblem, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->emblem), draw_emblem, v, NULL);
    gtk_box_append(GTK_BOX(bottom), v->emblem);

    GtkWidget *tagline = gtk_label_new("AirDrop lets you share instantly with people nearby.");
    gtk_widget_add_css_class(tagline, "airdrop-tagline");
    gtk_box_append(GTK_BOX(bottom), tagline);

    v->discover_btn = gtk_menu_button_new();
    gtk_widget_add_css_class(v->discover_btn, "airdrop-discover-btn");
    gtk_widget_set_halign(v->discover_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(v->discover_btn, "pointer");
    v->discover_label = gtk_label_new("Allow me to be discovered by: Receiving Off ▾");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(v->discover_btn), v->discover_label);

    v->discover_popover = gtk_popover_new();
    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    static const char *const mode_names[3] = { "Receiving Off", "Contacts Only", "Everyone for 10 Minutes" };
    for (int i = 0; i < 3; i++) {
        GtkWidget *mi = gtk_button_new();
        gtk_widget_add_css_class(mi, "airdrop-menu-item");
        GtkWidget *mr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        v->mode_checks[i] = gtk_label_new("");
        gtk_widget_set_size_request(v->mode_checks[i], 14, -1);
        GtkWidget *ml = gtk_label_new(mode_names[i]);
        gtk_label_set_xalign(GTK_LABEL(ml), 0);
        gtk_box_append(GTK_BOX(mr), v->mode_checks[i]);
        gtk_box_append(GTK_BOX(mr), ml);
        gtk_button_set_child(GTK_BUTTON(mi), mr);
        g_object_set_data(G_OBJECT(mi), "mode", GINT_TO_POINTER(i));
        g_signal_connect(mi, "clicked", G_CALLBACK(on_pick_mode), v);
        gtk_box_append(GTK_BOX(mbox), mi);
    }
    gtk_popover_set_child(GTK_POPOVER(v->discover_popover), mbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(v->discover_btn), v->discover_popover);
    gtk_box_append(GTK_BOX(bottom), v->discover_btn);

    v->status_label = gtk_label_new("");
    gtk_widget_add_css_class(v->status_label, "airdrop-status");
    gtk_box_append(GTK_BOX(bottom), v->status_label);

    gtk_box_append(GTK_BOX(v->ready_box), bottom);
    gtk_overlay_add_overlay(GTK_OVERLAY(v->stage), v->ready_box);

    share_watch(v->share, on_changed, v);
    update(v);
    return page;
}

void share_shown(Lares *app, Section *self, gboolean on)
{
    ShareView *v = self->view;
    v->on_screen = on;
    if (on) {
        share_refresh(v->share);
        const ShareState *st = share_state(v->share);
        if (st->setup == SHARE_READY && !st->card_in_use && !st->browsing) {
            v->browsing_auto = TRUE;
            share_set_browsing(v->share, TRUE);
        }
    } else if (v->browsing_auto) {
        v->browsing_auto = FALSE;
        share_set_browsing(v->share, FALSE);
    }
    sync_tick(v);
}
