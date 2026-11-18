// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_picture.h"
#include "pad/pads.h"
#include <math.h>
#include <string.h>

/* Every position is in pixels of the full-size artwork (1444 by 1007). */
#define ART_W          1444.0
#define ART_H          1007.0
#define MARGIN_SIDE    130.0     /* room for the glow */
#define MARGIN_TOP     110.0     /* and above it, for the trigger gauges */
#define MARGIN_BOTTOM  100.0
#define GLOW_MARGIN    130.0     /* the glow picture is the outline, blurred, this much around */
#define CENTER_X       721.25
#define PRESSED_ALPHA  0.82
#define SEEN_ALPHA     0.36      /* buttons the test has seen, while they're up again */
#define STICK_ALPHA    0.6
#define CAP_RADIUS     80.0
#define WELL_RADIUS    92.5
#define STICK_TRAVEL   30.0
#define PLACEHOLDER_ALPHA 0.16
#define SHAKE_PX       3.0
#define SHAKE_STEP_MS  45.0
#define SHAKE_STEPS    8

static const double GREEN[3] = { 0.2, 0.86, 0.2 };
static const double GLOW_DARK[4] = { 0.2, 0.86, 0.2, 0.55 };
static const double GLOW_LIGHT[4] = { 0.06, 0.49, 0.06, 0.5 };

static const struct { PadButton b; double x, y, r; } ROUND[] = {
    { PB_A, 1086.5, 370, 45.5 }, { PB_B, 1182.5, 277.5, 45.5 }, { PB_X, 988.5, 275.5, 45.5 },
    { PB_Y, 1082.5, 183, 44.5 }, { PB_VIEW, 618, 277.5, 33.5 }, { PB_MENU, 824.5, 277.5, 33.5 },
    { PB_XBOX, 721.5, 134, 53 },
};
static const struct { PadButton b; double x, y, w, h, r; } BOXES[] = {
    { PB_SHARE, 680, 328, 80, 50, 20 }, { PB_UP, 499, 407, 71, 69, 12 }, { PB_DOWN, 499, 546, 71, 63, 12 },
    { PB_LEFT, 433, 476, 66, 70, 12 }, { PB_RIGHT, 570, 476, 70, 70, 12 },
};
/* The left bumper's lower edge follows the seam with the faceplate; the rest
 * overshoots the outline and is trimmed by the artwork's own transparency. */
static const double LEFT_BUMPER[][2] = {
    { 200, 150 }, { 212, 140 }, { 228, 121 }, { 260, 98 }, { 300, 78 }, { 340, 63 }, { 380, 52 },
    { 420, 44 }, { 460, 39 }, { 500, 37 }, { 506, 30 }, { 506, -10 }, { 200, -10 },
};
static const double LEFT_TRIGGER[5] = { 300, -56, 170, 44, 14 };

/* The artwork is decoded once, the first time a picture is shown, and only
 * then: about 1.5 MB that the rest of Lares never pays for. */
static cairo_surface_t *art, *glow;

struct _LaresPadPicture {
    GtkWidget parent;
    int height;
    gboolean placeholder;
    PadState state;
    guint32 seen;
    gint64 shake_start;
    double shake;
    guint tick;
    /* The artwork and glow scaled to this picture's size, made once per size:
     * a frame then only copies them and paints what's lit. */
    cairo_surface_t *art_cache, *glow_cache;
    double cache_fit;
};

G_DEFINE_FINAL_TYPE(LaresPadPicture, lares_pad_picture, GTK_TYPE_WIDGET)

/* ---------------------------------------------------------------- loading */

typedef struct { const guchar *data; gsize left; } PngSource;

static cairo_status_t read_png(void *closure, unsigned char *out, unsigned int length)
{
    PngSource *s = closure;
    if (length > s->left) return CAIRO_STATUS_READ_ERROR;
    memcpy(out, s->data, length);
    s->data += length;
    s->left -= length;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *load_png(const char *resource)
{
    GBytes *bytes = g_resources_lookup_data(resource, 0, NULL);
    if (!bytes) return NULL;
    PngSource src;
    src.data = g_bytes_get_data(bytes, &src.left);
    cairo_surface_t *s = cairo_image_surface_create_from_png_stream(read_png, &src);
    g_bytes_unref(bytes);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
    return s;
}

/* ---------------------------------------------------------------- drawing */

static void rounded_box(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static double mirror(double x) { return 2 * CENTER_X - x; }

static cairo_surface_t *scaled(cairo_surface_t *src, double out_w, double fit)
{
    double k = out_w * fit / cairo_image_surface_get_width(src);
    int w = (int)ceil(cairo_image_surface_get_width(src) * k);
    int h = (int)ceil(cairo_image_surface_get_height(src) * k);
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, MAX(1, w), MAX(1, h));
    cairo_t *cr = cairo_create(dst);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    return dst;
}

static void ensure_cache(LaresPadPicture *s, double fit)
{
    if (s->art_cache && fabs(s->cache_fit - fit) < 1e-6) return;
    g_clear_pointer(&s->art_cache, cairo_surface_destroy);
    g_clear_pointer(&s->glow_cache, cairo_surface_destroy);
    s->art_cache = scaled(art, ART_W, fit);
    s->glow_cache = scaled(glow, ART_W + 2 * GLOW_MARGIN, fit);
    s->cache_fit = fit;
}

/* Called in artwork units; the cache is in pixels, so it's drawn 1:1. */
static void paint_art(LaresPadPicture *s, cairo_t *cr)
{
    cairo_save(cr);
    cairo_scale(cr, 1 / s->cache_fit, 1 / s->cache_fit);
    cairo_set_source_surface(cr, s->art_cache, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

/* The controller's flat shape in one color. */
static void paint_silhouette(LaresPadPicture *s, cairo_t *cr, const double rgb[3], double alpha)
{
    cairo_save(cr);
    cairo_scale(cr, 1 / s->cache_fit, 1 / s->cache_fit);
    cairo_set_source_rgba(cr, rgb[0], rgb[1], rgb[2], alpha);
    cairo_mask_surface(cr, s->art_cache, 0, 0);
    cairo_restore(cr);
}

static void draw_glow(LaresPadPicture *s, cairo_t *cr, const double c[4])
{
    cairo_save(cr);
    cairo_translate(cr, -GLOW_MARGIN, -GLOW_MARGIN);
    cairo_scale(cr, 1 / s->cache_fit, 1 / s->cache_fit);
    cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
    cairo_mask_surface(cr, s->glow_cache, 0, 0);
    cairo_restore(cr);
}

static void draw_trigger(cairo_t *cr, double value, gboolean right, const double fg[3], gboolean seen)
{
    double x = LEFT_TRIGGER[0], y = LEFT_TRIGGER[1], w = LEFT_TRIGGER[2], h = LEFT_TRIGGER[3], r = LEFT_TRIGGER[4];
    double left = right ? mirror(x + w) : x;
    rounded_box(cr, left, y, w, h, r);
    if (seen) cairo_set_source_rgba(cr, GREEN[0], GREEN[1], GREEN[2], SEEN_ALPHA);
    else cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], 0.18);
    cairo_fill(cr);
    double pulled = CLAMP(value, 0, 1);
    if (pulled <= 0) return;
    /* Fills from the outside in, so the two gauges mirror each other. */
    cairo_save(cr);
    cairo_rectangle(cr, right ? left + w * (1 - pulled) : left, y, w * pulled, h);
    cairo_clip(cr);
    rounded_box(cr, left, y, w, h, r);
    cairo_set_source_rgb(cr, GREEN[0], GREEN[1], GREEN[2]);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_bumper(LaresPadPicture *s, cairo_t *cr, gboolean right, double alpha)
{
    cairo_save(cr);
    for (guint i = 0; i < G_N_ELEMENTS(LEFT_BUMPER); i++) {
        double x = right ? mirror(LEFT_BUMPER[i][0]) : LEFT_BUMPER[i][0];
        if (i == 0) cairo_move_to(cr, x, LEFT_BUMPER[i][1]);
        else cairo_line_to(cr, x, LEFT_BUMPER[i][1]);
    }
    cairo_close_path(cr);
    cairo_clip(cr);
    paint_silhouette(s, cr, GREEN, alpha);
    cairo_restore(cr);
}

static void draw_stick(LaresPadPicture *s, cairo_t *cr, double cx, double cy, double dx, double dy, double alpha)
{
    /* A real stick moves in a circle; the raw corners reach past it. */
    double reach = hypot(dx, dy);
    if (reach > 1) { dx /= reach; dy /= reach; }
    /* The empty well where the cap sits in the artwork */
    cairo_arc(cr, cx, cy, WELL_RADIUS, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0.03, 0.03, 0.03);
    cairo_fill(cr);
    /* The cap itself, cut from the artwork and slid to where the stick is */
    cairo_save(cr);
    cairo_translate(cr, dx * STICK_TRAVEL, dy * STICK_TRAVEL);
    cairo_arc(cr, cx, cy, CAP_RADIUS, 0, 2 * G_PI);
    cairo_clip(cr);
    paint_art(s, cr);
    if (alpha > 0) {
        cairo_set_source_rgba(cr, GREEN[0], GREEN[1], GREEN[2], alpha);
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

static double lit(LaresPadPicture *s, PadButton b, double pressed_alpha)
{
    if (s->state.buttons & PB_BIT(b)) return pressed_alpha;
    return s->seen & PB_BIT(b) ? SEEN_ALPHA : 0;
}

static void draw_controller(LaresPadPicture *s, cairo_t *cr, double width, double height, const double fg[3])
{
    double full_w = ART_W + 2 * MARGIN_SIDE, full_h = MARGIN_TOP + ART_H + MARGIN_BOTTOM;
    double fit = MIN(width / full_w, height / full_h);
    ensure_cache(s, fit);
    cairo_save(cr);
    /* Whole pixels, so the cached artwork is copied without resampling. */
    cairo_translate(cr, round((width - full_w * fit) / 2 + s->shake), round((height - full_h * fit) / 2));
    cairo_scale(cr, fit, fit);
    cairo_translate(cr, MARGIN_SIDE, MARGIN_TOP);

    /* Light text means a dark background. */
    gboolean on_dark = 0.299 * fg[0] + 0.587 * fg[1] + 0.114 * fg[2] > 0.5;
    draw_glow(s, cr, on_dark ? GLOW_DARK : GLOW_LIGHT);
    draw_trigger(cr, s->state.lt, FALSE, fg, s->seen & PB_BIT(PB_LT));
    draw_trigger(cr, s->state.rt, TRUE, fg, s->seen & PB_BIT(PB_RT));
    paint_art(s, cr);
    if (lit(s, PB_LB, PRESSED_ALPHA) > 0) draw_bumper(s, cr, FALSE, lit(s, PB_LB, PRESSED_ALPHA));
    if (lit(s, PB_RB, PRESSED_ALPHA) > 0) draw_bumper(s, cr, TRUE, lit(s, PB_RB, PRESSED_ALPHA));
    for (guint i = 0; i < G_N_ELEMENTS(ROUND); i++) {
        double a = lit(s, ROUND[i].b, PRESSED_ALPHA);
        if (a <= 0) continue;
        cairo_set_source_rgba(cr, GREEN[0], GREEN[1], GREEN[2], a);
        cairo_arc(cr, ROUND[i].x, ROUND[i].y, ROUND[i].r, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    for (guint i = 0; i < G_N_ELEMENTS(BOXES); i++) {
        double a = lit(s, BOXES[i].b, PRESSED_ALPHA);
        if (a <= 0) continue;
        cairo_set_source_rgba(cr, GREEN[0], GREEN[1], GREEN[2], a);
        rounded_box(cr, BOXES[i].x, BOXES[i].y, BOXES[i].w, BOXES[i].h, BOXES[i].r);
        cairo_fill(cr);
    }
    draw_stick(s, cr, 358, 277.5, s->state.lx, s->state.ly, lit(s, PB_LS, STICK_ALPHA));
    draw_stick(s, cr, 906.5, 490.5, s->state.rx, s->state.ry, lit(s, PB_RS, STICK_ALPHA));
    cairo_restore(cr);
}

static void draw_placeholder(LaresPadPicture *s, cairo_t *cr, double width, double height, const double fg[3])
{
    double fit = MIN(width / ART_W, height / ART_H);
    ensure_cache(s, fit);
    cairo_save(cr);
    cairo_translate(cr, round((width - ART_W * fit) / 2), round((height - ART_H * fit) / 2));
    cairo_scale(cr, fit, fit);
    paint_silhouette(s, cr, fg, PLACEHOLDER_ALPHA);
    cairo_restore(cr);
}

static void picture_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(w);
    if (!art || !glow) return;
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    GdkRGBA c;
    gtk_widget_get_color(w, &c);
    double fg[3] = { c.red, c.green, c.blue };
    cairo_t *cr = gtk_snapshot_append_cairo(snap, &GRAPHENE_RECT_INIT(-8, 0, width + 16, height));
    if (s->placeholder) draw_placeholder(s, cr, width, height, fg);
    else draw_controller(s, cr, width, height, fg);
    cairo_destroy(cr);
}

/* ----------------------------------------------------------------- widget */

static void picture_measure(GtkWidget *w, GtkOrientation o, int for_size,
                            int *min, int *nat, int *min_base, int *nat_base)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(w);
    if (o == GTK_ORIENTATION_VERTICAL) *min = *nat = s->height;
    else { *min = 120; *nat = (int)(s->height * (ART_W + 2 * MARGIN_SIDE) / (MARGIN_TOP + ART_H + MARGIN_BOTTOM)); }
}

static gboolean shake_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(w);
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (!s->shake_start) s->shake_start = now;
    double t = (now - s->shake_start) / 1000.0;
    if (t >= SHAKE_STEP_MS * SHAKE_STEPS) {
        s->shake = 0;
        s->shake_start = 0;
        s->tick = 0;
        gtk_widget_queue_draw(w);
        return G_SOURCE_REMOVE;
    }
    /* Side to side, easing in and out of each turn. */
    double phase = fmod(t / SHAKE_STEP_MS, 2.0);
    double eased = 0.5 - 0.5 * cos(G_PI * (phase < 1 ? phase : 2 - phase));
    s->shake = -SHAKE_PX + 2 * SHAKE_PX * eased;
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

void lares_pad_picture_shake(LaresPadPicture *s)
{
    s->shake_start = 0;
    if (!s->tick) s->tick = gtk_widget_add_tick_callback(GTK_WIDGET(s), shake_tick, NULL, NULL);
}

void lares_pad_picture_set_state(LaresPadPicture *s, const PadState *state)
{
    PadState idle = { 0 };
    if (!state) state = &idle;
    if (memcmp(state, &s->state, sizeof *state) == 0) return;
    s->state = *state;
    gtk_widget_queue_draw(GTK_WIDGET(s));
}

void lares_pad_picture_set_seen(LaresPadPicture *s, guint32 seen)
{
    if (seen == s->seen) return;
    s->seen = seen;
    gtk_widget_queue_draw(GTK_WIDGET(s));
}

static void picture_css_changed(GtkWidget *w, GtkCssStyleChange *change)
{
    GTK_WIDGET_CLASS(lares_pad_picture_parent_class)->css_changed(w, change);
    gtk_widget_queue_draw(w);
}

static void picture_finalize(GObject *o)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(o);
    g_clear_pointer(&s->art_cache, cairo_surface_destroy);
    g_clear_pointer(&s->glow_cache, cairo_surface_destroy);
    G_OBJECT_CLASS(lares_pad_picture_parent_class)->finalize(o);
}

static void lares_pad_picture_class_init(LaresPadPictureClass *k)
{
    G_OBJECT_CLASS(k)->finalize = picture_finalize;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = picture_measure;
    wc->snapshot = picture_snapshot;
    wc->css_changed = picture_css_changed;
    gtk_widget_class_set_css_name(wc, "padpicture");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_IMG);
}

static void lares_pad_picture_init(LaresPadPicture *s)
{
    if (!art) art = load_png("/com/nahuelgenchi/Lares/pad/controller.png");
    if (!glow) glow = load_png("/com/nahuelgenchi/Lares/pad/glow.png");
}

GtkWidget *lares_pad_picture_new(int height, gboolean placeholder)
{
    LaresPadPicture *s = g_object_new(LARES_TYPE_PAD_PICTURE, NULL);
    s->height = height;
    s->placeholder = placeholder;
    gtk_widget_set_hexpand(GTK_WIDGET(s), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(s), GTK_ACCESSIBLE_PROPERTY_LABEL,
        placeholder ? "No controller connected" : "The controller, live", -1);
    return GTK_WIDGET(s);
}
