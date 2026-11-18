// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_picture.h"
#include "pad_map.h"
#include <math.h>
#include <string.h>

#define PIXEL             2       /* screen pixels per map pixel, as Monocraft's at 18 px */
#define FAINT_ALPHA       0.4f    /* the D-pad's middle lines */
#define SEEN_ALPHA        0.36f   /* buttons the test has seen, while they're up again */
#define PLACEHOLDER_ALPHA 0.3f
#define SHAKE_STEP_MS     45.0
#define SHAKE_STEPS       8

/* The map is read the first time a picture is made, and only then: about
 * 35 KB that the rest of Lares never pays for. */
static PadMap *map;
/* Each trigger gauge's inside, in map pixels; LT's fills from the left, RT's
 * from the right, so the two mirror each other. */
static struct { int x0, x1; } gauges[2];

/* What a frame shows, worked out from the controller's state: a new state
 * that looks the same doesn't paint the picture again. */
typedef struct {
    guint32 held, seen;
    int caps[2][2];     /* each stick cap's offset, in map pixels */
    int pulled[2];      /* how many columns of each trigger gauge are lit */
} Look;

struct _LaresPadPicture {
    GtkWidget parent;
    GtkWidget *lit;     /* carries the accent color; see PadLit */
    gboolean placeholder;
    PadState state;
    Look look;
    guint32 seen;
    gint64 shake_start;
    int shake;          /* -1, 0 or 1 map pixel */
    guint tick;
    /* The picture at one screen pixel per map pixel, made again only when
     * its look or colors change; the snapshot scales it up. */
    GdkTexture *texture;
};

G_DEFINE_FINAL_TYPE(LaresPadPicture, lares_pad_picture, GTK_TYPE_WIDGET)

static void invalidate(LaresPadPicture *s)
{
    g_clear_object(&s->texture);
    gtk_widget_queue_draw(GTK_WIDGET(s));
}

/* ---------------------------------------------------------- accent color */

/* GTK hands a widget only its own `color`, so the accent comes from a tiny
 * child styled `padpicture > lit { color: var(--accent); }`. It is never
 * drawn and takes no room. */
#define PAD_TYPE_LIT (pad_lit_get_type())
G_DECLARE_FINAL_TYPE(PadLit, pad_lit, PAD, LIT, GtkWidget)
struct _PadLit { GtkWidget parent; };
G_DEFINE_FINAL_TYPE(PadLit, pad_lit, GTK_TYPE_WIDGET)

static void pad_lit_css_changed(GtkWidget *w, GtkCssStyleChange *change)
{
    GTK_WIDGET_CLASS(pad_lit_parent_class)->css_changed(w, change);
    GtkWidget *picture = gtk_widget_get_parent(w);
    if (picture) invalidate(LARES_PAD_PICTURE(picture));
}

static void pad_lit_class_init(PadLitClass *k)
{
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->css_changed = pad_lit_css_changed;
    gtk_widget_class_set_css_name(wc, "lit");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_PRESENTATION);
}

static void pad_lit_init(PadLit *s) {}

/* ---------------------------------------------------------------- loading */

static void load_map(void)
{
    if (map) return;
    GBytes *bytes = g_resources_lookup_data("/com/nahuelgenchi/Lares/pad/controller.txt", 0, NULL);
    if (!bytes) return;
    gsize length;
    const char *text = g_bytes_get_data(bytes, &length);
    GError *error = NULL;
    map = pad_map_parse(text, length, &error);
    g_bytes_unref(bytes);
    if (!map) {
        g_warning("The controller picture's map: %s", error->message);
        g_error_free(error);
        return;
    }
    for (int g = 0; g < 2; g++) {
        gauges[g].x0 = map->width;
        gauges[g].x1 = -1;
    }
    for (int y = 0; y < map->height; y++)
        for (int x = 0; x < map->width; x++) {
            guint8 cell = pad_map_at(map, x, y);
            PadButton b = pad_cell_button(cell);
            if (pad_cell_kind(cell) != PAD_CELL_AREA || (b != PB_LT && b != PB_RT)) continue;
            int g = b == PB_RT;
            gauges[g].x0 = MIN(gauges[g].x0, x);
            gauges[g].x1 = MAX(gauges[g].x1, x);
        }
}

/* ---------------------------------------------------------------- drawing */

/* A stick moves in a circle; the raw corners reach past it. */
static void cap_offset(float dx, float dy, int out[2])
{
    double reach = hypot(dx, dy);
    if (reach > 1) { dx /= reach; dy /= reach; }
    out[0] = (int)lround(dx * PAD_MAP_STICK_TRAVEL);
    out[1] = (int)lround(dy * PAD_MAP_STICK_TRAVEL);
}

static Look look_of(LaresPadPicture *s)
{
    Look l = { 0 };
    if (s->placeholder) return l;
    /* The gauges stand for the triggers, lit as far as they are pulled. */
    l.held = s->state.buttons & ~(PB_BIT(PB_LT) | PB_BIT(PB_RT));
    l.seen = s->seen;
    cap_offset(s->state.lx, s->state.ly, l.caps[0]);
    cap_offset(s->state.rx, s->state.ry, l.caps[1]);
    float pull[2] = { s->state.lt, s->state.rt };
    for (int g = 0; g < 2; g++)
        l.pulled[g] = (int)lround(CLAMP(pull[g], 0, 1) * (gauges[g].x1 - gauges[g].x0 + 1));
    return l;
}

static gboolean gauge_lit(const Look *l, PadButton b, int x)
{
    int g = b == PB_RT;
    if (g) return x > gauges[g].x1 - l->pulled[g];
    return x < gauges[g].x0 + l->pulled[g];
}

static void put(guchar *px, int x, int y, PadPaint paint, const GdkRGBA *fg, const GdkRGBA *accent,
                float strength)
{
    const GdkRGBA *c = fg;
    float alpha = 1;
    switch (paint) {
    case PAD_PAINT_NONE: return;
    case PAD_PAINT_LINE: break;
    case PAD_PAINT_FAINT: alpha = FAINT_ALPHA; break;
    case PAD_PAINT_LIT: c = accent; break;
    case PAD_PAINT_SEEN: c = accent; alpha = SEEN_ALPHA; break;
    }
    if (x < 0 || y < 0 || x >= map->width || y >= map->height) return;
    guchar *p = px + ((gsize)y * map->width + x) * 4;
    p[0] = (guchar)lroundf(c->red * 255);
    p[1] = (guchar)lroundf(c->green * 255);
    p[2] = (guchar)lroundf(c->blue * 255);
    p[3] = (guchar)lroundf(c->alpha * alpha * strength * 255);
}

static GdkTexture *paint(LaresPadPicture *s)
{
    GdkRGBA fg, accent;
    gtk_widget_get_color(GTK_WIDGET(s), &fg);
    gtk_widget_get_color(s->lit, &accent);
    const Look *l = &s->look;
    float strength = s->placeholder ? PLACEHOLDER_ALPHA : 1;
    gsize size = (gsize)map->width * map->height * 4;
    guchar *px = g_malloc0(size);

    /* Everything that stays put, then the caps where the sticks have them. */
    for (int pass = 0; pass < 2; pass++)
        for (int y = 0; y < map->height; y++)
            for (int x = 0; x < map->width; x++) {
                guint8 cell = pad_map_at(map, x, y);
                PadCellKind kind = pad_cell_kind(cell);
                PadButton b = pad_cell_button(cell);
                gboolean cap = kind == PAD_CELL_CAP_LINE || kind == PAD_CELL_CAP_AREA;
                if (cap != (pass == 1)) continue;
                if (s->placeholder && kind == PAD_CELL_GAUGE) continue;
                guint32 held = l->held;
                if (kind == PAD_CELL_AREA && (b == PB_LT || b == PB_RT) && gauge_lit(l, b, x))
                    held |= PB_BIT(b);
                int dx = 0, dy = 0;
                if (cap) {
                    dx = l->caps[b == PB_RS][0];
                    dy = l->caps[b == PB_RS][1];
                }
                put(px, x + dx, y + dy, pad_cell_paint(cell, held, l->seen), &fg, &accent, strength);
            }

    GBytes *bytes = g_bytes_new_take(px, size);
    GdkTexture *texture = gdk_memory_texture_new(map->width, map->height, GDK_MEMORY_R8G8B8A8,
                                                 bytes, (gsize)map->width * 4);
    g_bytes_unref(bytes);
    return texture;
}

static int pixel_for(LaresPadPicture *s, int width)
{
    if (s->placeholder || width < map->width * PIXEL) return 1;
    return PIXEL;
}

static void picture_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(w);
    if (!map) return;
    int width = gtk_widget_get_width(w);
    int p = pixel_for(s, width);
    if (!s->texture) s->texture = paint(s);
    /* Whole pixels, so every map pixel lands on the screen's grid. */
    float x = (float)((width - map->width * p) / 2 + s->shake * p);
    gtk_snapshot_append_scaled_texture(snap, s->texture, GSK_SCALING_FILTER_NEAREST,
                                       &GRAPHENE_RECT_INIT(x, 0, map->width * p, map->height * p));
}

/* ----------------------------------------------------------------- widget */

static GtkSizeRequestMode picture_request_mode(GtkWidget *w)
{
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

/* Two screen pixels per map pixel when there's room, one when there isn't. */
static void picture_measure(GtkWidget *w, GtkOrientation o, int for_size,
                            int *min, int *nat, int *min_base, int *nat_base)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(w);
    if (!map) { *min = *nat = 0; return; }
    int most = s->placeholder ? 1 : PIXEL;
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        *min = map->width;
        *nat = map->width * most;
    } else if (for_size < 0) {
        *min = map->height;
        *nat = map->height * most;
    } else {
        *min = *nat = map->height * pixel_for(s, for_size);
    }
}

static void picture_size_allocate(GtkWidget *w, int width, int height, int baseline)
{
    gtk_widget_allocate(LARES_PAD_PICTURE(w)->lit, 0, 0, -1, NULL);
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
    /* Side to side, a whole pixel at a time. */
    double phase = fmod(t / SHAKE_STEP_MS, 2.0);
    double eased = 0.5 - 0.5 * cos(G_PI * (phase < 1 ? phase : 2 - phase));
    int shake = (int)lround(2 * eased - 1);
    if (shake != s->shake) {
        s->shake = shake;
        gtk_widget_queue_draw(w);
    }
    return G_SOURCE_CONTINUE;
}

void lares_pad_picture_shake(LaresPadPicture *s)
{
    s->shake_start = 0;
    if (!s->tick) s->tick = gtk_widget_add_tick_callback(GTK_WIDGET(s), shake_tick, NULL, NULL);
}

static void update_look(LaresPadPicture *s)
{
    if (!map) return;
    Look l = look_of(s);
    if (memcmp(&l, &s->look, sizeof l) == 0) return;
    s->look = l;
    invalidate(s);
}

void lares_pad_picture_set_state(LaresPadPicture *s, const PadState *state)
{
    PadState idle = { 0 };
    if (!state) state = &idle;
    s->state = *state;
    update_look(s);
}

void lares_pad_picture_set_seen(LaresPadPicture *s, guint32 seen)
{
    s->seen = seen;
    update_look(s);
}

static void picture_css_changed(GtkWidget *w, GtkCssStyleChange *change)
{
    GTK_WIDGET_CLASS(lares_pad_picture_parent_class)->css_changed(w, change);
    invalidate(LARES_PAD_PICTURE(w));
}

static void picture_dispose(GObject *o)
{
    LaresPadPicture *s = LARES_PAD_PICTURE(o);
    g_clear_pointer(&s->lit, gtk_widget_unparent);
    g_clear_object(&s->texture);
    G_OBJECT_CLASS(lares_pad_picture_parent_class)->dispose(o);
}

static void lares_pad_picture_class_init(LaresPadPictureClass *k)
{
    G_OBJECT_CLASS(k)->dispose = picture_dispose;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->get_request_mode = picture_request_mode;
    wc->measure = picture_measure;
    wc->size_allocate = picture_size_allocate;
    wc->snapshot = picture_snapshot;
    wc->css_changed = picture_css_changed;
    gtk_widget_class_set_css_name(wc, "padpicture");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_IMG);
}

static void lares_pad_picture_init(LaresPadPicture *s)
{
    load_map();
    s->lit = g_object_new(PAD_TYPE_LIT, NULL);
    gtk_widget_set_parent(s->lit, GTK_WIDGET(s));
}

GtkWidget *lares_pad_picture_new(gboolean placeholder)
{
    LaresPadPicture *s = g_object_new(LARES_TYPE_PAD_PICTURE, NULL);
    s->placeholder = placeholder;
    s->look = look_of(s);
    gtk_widget_set_hexpand(GTK_WIDGET(s), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(s), GTK_ACCESSIBLE_PROPERTY_LABEL,
        placeholder ? "No controller connected" : "The controller, live", -1);
    return GTK_WIDGET(s);
}
