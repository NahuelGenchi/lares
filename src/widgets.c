#include "widgets.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ Spring */

gboolean lares_animations_enabled(GtkWidget *w)
{
    gboolean on = TRUE;
    g_object_get(gtk_widget_get_settings(w), "gtk-enable-animations", &on, NULL);
    return on;
}

/* Integrates in 2 ms steps so the motion is the same at any frame rate.
 * Returns FALSE once the spring has settled on its target. */
gboolean lares_spring_step(LaresSpring *s, gint64 now, double k, double zeta, double rest)
{
    double dt = s->last ? (now - s->last) / 1e6 : 1.0 / 60.0;
    s->last = now;
    if (dt > 0.05) dt = 0.05;
    double c = 2.0 * sqrt(k) * zeta;
    int steps = (int)ceil(dt / 0.002);
    double h = dt / steps;
    for (int i = 0; i < steps; i++) {
        double a = -k * (s->pos - s->target) - c * s->vel;
        s->vel += a * h;
        s->pos += s->vel * h;
    }
    if (fabs(s->pos - s->target) < rest && fabs(s->vel) < rest * 4) {
        lares_spring_jump(s, s->target);
        return FALSE;
    }
    return TRUE;
}

void lares_spring_jump(LaresSpring *s, double to)
{
    s->pos = s->target = to;
    s->vel = 0;
    s->last = 0;
}

/* ---------------------------------------------------------------- PixelIcon */

struct _LaresPixelIcon {
    GtkWidget parent;
    char **rows;
    int pixel, cols, lines;
};

G_DEFINE_FINAL_TYPE(LaresPixelIcon, lares_pixel_icon, GTK_TYPE_WIDGET)

static void pixel_icon_measure(GtkWidget *w, GtkOrientation o, int for_size,
                               int *min, int *nat, int *min_base, int *nat_base)
{
    LaresPixelIcon *s = LARES_PIXEL_ICON(w);
    *min = *nat = (o == GTK_ORIENTATION_HORIZONTAL ? s->cols : s->lines) * s->pixel;
}

static void pixel_icon_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LaresPixelIcon *s = LARES_PIXEL_ICON(w);
    GdkRGBA full, half;
    gtk_widget_get_color(w, &full);
    half = full;
    half.alpha *= 0.45f;
    int p = s->pixel;
    int ox = (gtk_widget_get_width(w) - s->cols * p) / 2;
    int oy = (gtk_widget_get_height(w) - s->lines * p) / 2;

    for (int y = 0; y < s->lines; y++) {
        const char *r = s->rows[y];
        int x = 0;
        while (r[x]) {
            char c = r[x];
            if (c != '#' && c != '+') { x++; continue; }
            int start = x;
            while (r[x] == c) x++; /* a run is one rectangle: fewer nodes, no seams */
            gtk_snapshot_append_color(snap, c == '#' ? &full : &half,
                &GRAPHENE_RECT_INIT(ox + start * p, oy + y * p, (x - start) * p, p));
        }
    }
}

/* Widgets that paint with their CSS color repaint when it changes, so color
 * transitions play on them too. */
static void redraw_on_css_change(GtkWidget *w, GtkCssStyleChange *change)
{
    GTK_WIDGET_CLASS(g_type_class_peek(GTK_TYPE_WIDGET))->css_changed(w, change);
    gtk_widget_queue_draw(w);
}

static void pixel_icon_finalize(GObject *o)
{
    g_strfreev(LARES_PIXEL_ICON(o)->rows);
    G_OBJECT_CLASS(lares_pixel_icon_parent_class)->finalize(o);
}

static void lares_pixel_icon_class_init(LaresPixelIconClass *k)
{
    G_OBJECT_CLASS(k)->finalize = pixel_icon_finalize;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = pixel_icon_measure;
    wc->snapshot = pixel_icon_snapshot;
    wc->css_changed = redraw_on_css_change;
    gtk_widget_class_set_css_name(wc, "pixelicon");
}

static void lares_pixel_icon_init(LaresPixelIcon *s)
{
    s->pixel = 2;
    gtk_widget_set_can_target(GTK_WIDGET(s), FALSE);
}

GtkWidget *lares_pixel_icon_new(const char *const *rows, int pixel)
{
    LaresPixelIcon *s = g_object_new(LARES_TYPE_PIXEL_ICON, NULL);
    s->rows = g_strdupv((char **)rows);
    s->pixel = pixel;
    s->lines = (int)g_strv_length(s->rows);
    for (int i = 0; i < s->lines; i++)
        s->cols = MAX(s->cols, (int)strlen(s->rows[i]));
    gtk_widget_set_halign(GTK_WIDGET(s), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(s), GTK_ALIGN_CENTER);
    return GTK_WIDGET(s);
}

/* ------------------------------------------------------------- PixelSpinner */

struct _LaresPixelSpinner {
    GtkWidget parent;
    int pixel, step;
    guint tick;
    gint64 start;
};

G_DEFINE_FINAL_TYPE(LaresPixelSpinner, lares_pixel_spinner, GTK_TYPE_WIDGET)

/* Slots of the ring on a 3 by 3 grid, clockwise from the top left. */
static const int RING[8][2] = {{0,0},{1,0},{2,0},{2,1},{2,2},{1,2},{0,2},{0,1}};

static void spinner_measure(GtkWidget *w, GtkOrientation o, int for_size,
                            int *min, int *nat, int *min_base, int *nat_base)
{
    *min = *nat = 9 * LARES_PIXEL_SPINNER(w)->pixel;
}

static void spinner_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LaresPixelSpinner *s = LARES_PIXEL_SPINNER(w);
    GdkRGBA c;
    gtk_widget_get_color(w, &c);
    int p = s->pixel;
    int ox = (gtk_widget_get_width(w) - 9 * p) / 2 + p / 2;
    int oy = (gtk_widget_get_height(w) - 9 * p) / 2 + p / 2;
    static const float TRAIL[8] = {1.0f, 0.62f, 0.38f, 0.22f, 0.14f, 0.14f, 0.14f, 0.14f};
    for (int k = 0; k < 8; k++) {
        GdkRGBA d = c;
        d.alpha *= TRAIL[(s->step - k + 8) % 8];
        gtk_snapshot_append_color(snap, &d,
            &GRAPHENE_RECT_INIT(ox + RING[k][0] * 3 * p, oy + RING[k][1] * 3 * p, 2 * p, 2 * p));
    }
}

/* Repaints only when the light moves, about twelve times a second. */
static gboolean spinner_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    LaresPixelSpinner *s = LARES_PIXEL_SPINNER(w);
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (!s->start) s->start = now;
    int step = (int)(((now - s->start) / 80000) % 8);
    if (step != s->step) {
        s->step = step;
        gtk_widget_queue_draw(w);
    }
    return G_SOURCE_CONTINUE;
}

static void spinner_map(GtkWidget *w)
{
    GTK_WIDGET_CLASS(lares_pixel_spinner_parent_class)->map(w);
    LaresPixelSpinner *s = LARES_PIXEL_SPINNER(w);
    s->start = 0;
    if (!s->tick) s->tick = gtk_widget_add_tick_callback(w, spinner_tick, NULL, NULL);
}

static void spinner_unmap(GtkWidget *w)
{
    LaresPixelSpinner *s = LARES_PIXEL_SPINNER(w);
    if (s->tick) { gtk_widget_remove_tick_callback(w, s->tick); s->tick = 0; }
    GTK_WIDGET_CLASS(lares_pixel_spinner_parent_class)->unmap(w);
}

static void lares_pixel_spinner_class_init(LaresPixelSpinnerClass *k)
{
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = spinner_measure;
    wc->snapshot = spinner_snapshot;
    wc->css_changed = redraw_on_css_change;
    wc->map = spinner_map;
    wc->unmap = spinner_unmap;
    gtk_widget_class_set_css_name(wc, "pixelspinner");
}

static void lares_pixel_spinner_init(LaresPixelSpinner *s)
{
    s->pixel = 2;
    gtk_widget_set_can_target(GTK_WIDGET(s), FALSE);
}

GtkWidget *lares_pixel_spinner_new(int pixel)
{
    LaresPixelSpinner *s = g_object_new(LARES_TYPE_PIXEL_SPINNER, NULL);
    s->pixel = pixel;
    gtk_widget_set_halign(GTK_WIDGET(s), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(s), GTK_ALIGN_CENTER);
    return GTK_WIDGET(s);
}

/* --------------------------------------------------------------- ModeSwitch */

/* A slightly underdamped spring: quick, settling two percent past the
 * target, which reads as weight rather than wobble. */
#define SWITCH_STIFFNESS 360.0
#define SWITCH_DAMPING   0.8

struct _LaresModeSwitch {
    GtkWidget parent;
    GtkWidget *pill;
    GtkWidget *sides[2];
    int active;
    LaresSpring spring; /* 0 is the first side, 1 the second */
    guint tick;
};

G_DEFINE_FINAL_TYPE(LaresModeSwitch, lares_mode_switch, GTK_TYPE_WIDGET)

enum { SWITCH_PICKED, N_SWITCH_SIGNALS };
static guint switch_signals[N_SWITCH_SIGNALS];

static void switch_measure(GtkWidget *w, GtkOrientation o, int for_size,
                           int *min, int *nat, int *min_base, int *nat_base)
{
    LaresModeSwitch *s = LARES_MODE_SWITCH(w);
    int m = 0, n = 0;
    for (int i = 0; i < 2; i++) {
        int cm, cn;
        gtk_widget_measure(s->sides[i], o, -1, &cm, &cn, NULL, NULL);
        m = MAX(m, cm);
        n = MAX(n, cn);
    }
    int both = o == GTK_ORIENTATION_HORIZONTAL ? 2 : 1;
    *min = m * both;
    *nat = n * both;
}

static void switch_allocate(GtkWidget *w, int width, int height, int baseline)
{
    LaresModeSwitch *s = LARES_MODE_SWITCH(w);
    int half = width / 2;
    for (int i = 0; i < 2; i++)
        gtk_widget_size_allocate(s->sides[i],
            &(GtkAllocation){ i * (width - half), 0, half, height }, -1);

    int m, n;
    gtk_widget_measure(s->pill, GTK_ORIENTATION_HORIZONTAL, -1, &m, &n, NULL, NULL);
    gtk_widget_measure(s->pill, GTK_ORIENTATION_VERTICAL, -1, &m, &n, NULL, NULL);
    float x = (float)round(s->spring.pos * (width - half));
    gtk_widget_allocate(s->pill, half, height, -1,
                        gsk_transform_translate(NULL, &GRAPHENE_POINT_INIT(x, 0)));
}

static gboolean switch_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    LaresModeSwitch *s = LARES_MODE_SWITCH(w);
    gboolean moving = lares_spring_step(&s->spring, gdk_frame_clock_get_frame_time(clock),
                                        SWITCH_STIFFNESS, SWITCH_DAMPING, 0.0005);
    gtk_widget_queue_allocate(w);
    if (!moving) s->tick = 0;
    return moving ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void lares_mode_switch_set_active(LaresModeSwitch *s, int side, gboolean animate)
{
    s->active = side ? 1 : 0;
    for (int i = 0; i < 2; i++) {
        if (i == s->active) gtk_widget_add_css_class(s->sides[i], "selected");
        else gtk_widget_remove_css_class(s->sides[i], "selected");
        gtk_accessible_update_state(GTK_ACCESSIBLE(s->sides[i]),
            GTK_ACCESSIBLE_STATE_PRESSED, i == s->active ? GTK_ACCESSIBLE_TRISTATE_TRUE
                                                         : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
    }
    s->spring.target = s->active;
    if (animate && lares_animations_enabled(GTK_WIDGET(s)) && gtk_widget_get_mapped(GTK_WIDGET(s))) {
        if (!s->tick) s->tick = gtk_widget_add_tick_callback(GTK_WIDGET(s), switch_tick, NULL, NULL);
    } else {
        if (s->tick) { gtk_widget_remove_tick_callback(GTK_WIDGET(s), s->tick); s->tick = 0; }
        lares_spring_jump(&s->spring, s->active);
        gtk_widget_queue_allocate(GTK_WIDGET(s));
    }
}

static void side_clicked(GtkButton *b, LaresModeSwitch *s)
{
    g_signal_emit(s, switch_signals[SWITCH_PICKED], 0, b == GTK_BUTTON(s->sides[1]));
}

static gboolean switch_key(GtkEventControllerKey *k, guint keyval, guint code,
                           GdkModifierType mods, LaresModeSwitch *s)
{
    int side = keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left ? 0
             : keyval == GDK_KEY_Right || keyval == GDK_KEY_KP_Right ? 1 : -1;
    if (side < 0) return FALSE;
    g_signal_emit(s, switch_signals[SWITCH_PICKED], 0, side);
    gtk_widget_grab_focus(s->sides[side]);
    return TRUE;
}

static void unparent_children(GtkWidget *w)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(w)))
        gtk_widget_unparent(c);
}

static void switch_dispose(GObject *o)
{
    unparent_children(GTK_WIDGET(o));
    G_OBJECT_CLASS(lares_mode_switch_parent_class)->dispose(o);
}

static void lares_mode_switch_class_init(LaresModeSwitchClass *k)
{
    G_OBJECT_CLASS(k)->dispose = switch_dispose;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = switch_measure;
    wc->size_allocate = switch_allocate;
    gtk_widget_class_set_css_name(wc, "modeswitch");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_GROUP);
    switch_signals[SWITCH_PICKED] = g_signal_new("picked", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST,
                                                 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void lares_mode_switch_init(LaresModeSwitch *s)
{
    s->pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(s->pill, "pill");
    gtk_widget_set_can_target(s->pill, FALSE);
    gtk_widget_set_parent(s->pill, GTK_WIDGET(s)); /* first child: drawn under the sides */

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(switch_key), s);
    gtk_widget_add_controller(GTK_WIDGET(s), keys);
}

static GtkWidget *make_side(LaresModeSwitch *s, const char *const *icon, const char *text)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), lares_pixel_icon_new(icon, 3));
    gtk_box_append(GTK_BOX(box), gtk_label_new(text));
    gtk_button_set_child(GTK_BUTTON(b), box);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_signal_connect(b, "clicked", G_CALLBACK(side_clicked), s);
    gtk_widget_set_parent(b, GTK_WIDGET(s));
    return b;
}

GtkWidget *lares_mode_switch_new(const char *const *icon_a, const char *label_a,
                                 const char *const *icon_b, const char *label_b)
{
    LaresModeSwitch *s = g_object_new(LARES_TYPE_MODE_SWITCH, NULL);
    s->sides[0] = make_side(s, icon_a, label_a);
    s->sides[1] = make_side(s, icon_b, label_b);
    lares_mode_switch_set_active(s, 0, FALSE);
    return GTK_WIDGET(s);
}

/* --------------------------------------------------------------------- Wrap */

struct _LaresWrap {
    GtkWidget parent;
    int spacing, row_spacing;
};

G_DEFINE_FINAL_TYPE(LaresWrap, lares_wrap, GTK_TYPE_WIDGET)

/* Walks the children as they would sit in `width`, placing them when asked,
 * and returns the height they need. */
static int wrap_layout(LaresWrap *s, int width, gboolean place)
{
    int x = 0, y = 0, line = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(s)); c; c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int cw, ch;
        gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &cw, NULL, NULL);
        cw = MIN(cw, width);
        gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, cw, NULL, &ch, NULL, NULL);
        if (x > 0 && x + cw > width) {
            x = 0;
            y += line + s->row_spacing;
            line = 0;
        }
        if (place) gtk_widget_size_allocate(c, &(GtkAllocation){ x, y, cw, ch }, -1);
        x += cw + s->spacing;
        line = MAX(line, ch);
    }
    return y + line;
}

static GtkSizeRequestMode height_for_width(GtkWidget *w)
{
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void wrap_measure(GtkWidget *w, GtkOrientation o, int for_size,
                         int *min, int *nat, int *min_base, int *nat_base)
{
    LaresWrap *s = LARES_WRAP(w);
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        int m = 0, n = 0, count = 0;
        for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c)) continue;
            int cm, cn;
            gtk_widget_measure(c, o, -1, &cm, &cn, NULL, NULL);
            m = MAX(m, cm);
            n += cn;
            count++;
        }
        *min = m;
        *nat = n + MAX(0, count - 1) * s->spacing;
    } else {
        if (for_size < 0) {
            int ignored;
            wrap_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &ignored, &for_size, NULL, NULL);
        }
        *min = *nat = wrap_layout(s, for_size, FALSE);
    }
}

static void wrap_allocate(GtkWidget *w, int width, int height, int baseline)
{
    wrap_layout(LARES_WRAP(w), width, TRUE);
}

static void wrap_dispose(GObject *o)
{
    unparent_children(GTK_WIDGET(o));
    G_OBJECT_CLASS(lares_wrap_parent_class)->dispose(o);
}

static void lares_wrap_class_init(LaresWrapClass *k)
{
    G_OBJECT_CLASS(k)->dispose = wrap_dispose;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->get_request_mode = height_for_width;
    wc->measure = wrap_measure;
    wc->size_allocate = wrap_allocate;
    gtk_widget_class_set_css_name(wc, "wrap");
}

static void lares_wrap_init(LaresWrap *s) { s->spacing = s->row_spacing = 9; }

GtkWidget *lares_wrap_new(int spacing)
{
    LaresWrap *s = g_object_new(LARES_TYPE_WRAP, NULL);
    s->spacing = s->row_spacing = spacing;
    return GTK_WIDGET(s);
}

void lares_wrap_set_row_spacing(LaresWrap *s, int spacing)
{
    s->row_spacing = spacing;
    gtk_widget_queue_resize(GTK_WIDGET(s));
}

void lares_wrap_append(LaresWrap *s, GtkWidget *child)
{
    gtk_widget_set_parent(child, GTK_WIDGET(s));
    gtk_widget_queue_resize(GTK_WIDGET(s));
}

/* ---------------------------------------------------------------------- Nav */

#define NAV_STIFFNESS 420.0
#define NAV_DAMPING   0.86

typedef struct {
    char *id;
    GtkWidget *button;
} NavItem;

struct _LaresNav {
    GtkWidget parent;
    GtkWidget *pill, *box;
    GPtrArray *items;
    char *selected;
    LaresSpring y, h;   /* the highlight's top and height, in pixels */
    gboolean placed, animate;
    guint tick;
};

G_DEFINE_FINAL_TYPE(LaresNav, lares_nav, GTK_TYPE_WIDGET)

enum { NAV_SELECTED, N_NAV_SIGNALS };
static guint nav_signals[N_NAV_SIGNALS];

static NavItem *nav_find(LaresNav *s, const char *id)
{
    for (guint i = 0; i < s->items->len; i++) {
        NavItem *it = s->items->pdata[i];
        if (g_str_equal(it->id, id)) return it;
    }
    return NULL;
}

static void nav_item_free(gpointer p)
{
    NavItem *it = p;
    g_free(it->id);
    g_free(it);
}

static void nav_measure(GtkWidget *w, GtkOrientation o, int for_size,
                        int *min, int *nat, int *min_base, int *nat_base)
{
    gtk_widget_measure(LARES_NAV(w)->box, o, for_size, min, nat, NULL, NULL);
}

static gboolean nav_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    LaresNav *s = LARES_NAV(w);
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    gboolean a = lares_spring_step(&s->y, now, NAV_STIFFNESS, NAV_DAMPING, 0.25);
    gboolean b = lares_spring_step(&s->h, now, NAV_STIFFNESS, NAV_DAMPING, 0.25);
    gtk_widget_queue_allocate(w);
    if (!a && !b) s->tick = 0;
    return a || b ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void nav_allocate(GtkWidget *w, int width, int height, int baseline)
{
    LaresNav *s = LARES_NAV(w);
    gtk_widget_size_allocate(s->box, &(GtkAllocation){ 0, 0, width, height }, -1);

    NavItem *it = s->selected ? nav_find(s, s->selected) : NULL;
    graphene_rect_t r;
    gboolean shown = it && gtk_widget_get_visible(it->button)
                  && gtk_widget_compute_bounds(it->button, s->box, &r);
    gtk_widget_set_child_visible(s->pill, shown);
    if (!shown) return;

    if (!s->placed || !s->animate) {
        lares_spring_jump(&s->y, r.origin.y);
        lares_spring_jump(&s->h, r.size.height);
        s->placed = TRUE;
    } else if (s->y.target != r.origin.y || s->h.target != r.size.height) {
        s->y.target = r.origin.y;
        s->h.target = r.size.height;
        if (!s->tick) s->tick = gtk_widget_add_tick_callback(w, nav_tick, NULL, NULL);
    }
    int m, n;
    gtk_widget_measure(s->pill, GTK_ORIENTATION_HORIZONTAL, -1, &m, &n, NULL, NULL);
    gtk_widget_measure(s->pill, GTK_ORIENTATION_VERTICAL, -1, &m, &n, NULL, NULL);
    gtk_widget_allocate(s->pill, (int)r.size.width, MAX(1, (int)round(s->h.pos)), -1,
        gsk_transform_translate(NULL, &GRAPHENE_POINT_INIT(r.origin.x, (float)round(s->y.pos))));
}

static void nav_clicked(GtkButton *b, LaresNav *s)
{
    for (guint i = 0; i < s->items->len; i++) {
        NavItem *it = s->items->pdata[i];
        if (it->button == GTK_WIDGET(b)) {
            g_signal_emit(s, nav_signals[NAV_SELECTED], 0, it->id);
            return;
        }
    }
}

void lares_nav_add(LaresNav *s, const char *id, const char *const *icon, const char *label)
{
    NavItem *it = g_new0(NavItem, 1);
    it->id = g_strdup(id);
    it->button = gtk_button_new();
    gtk_widget_add_css_class(it->button, "nav-item");
    gtk_widget_set_cursor_from_name(it->button, "pointer");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *art = lares_pixel_icon_new(icon, 2);
    gtk_widget_set_size_request(art, 22, -1); /* icons of different widths line up their labels */
    gtk_box_append(GTK_BOX(row), art);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(row), l);
    gtk_button_set_child(GTK_BUTTON(it->button), row);
    g_signal_connect(it->button, "clicked", G_CALLBACK(nav_clicked), s);
    gtk_box_append(GTK_BOX(s->box), it->button);
    g_ptr_array_add(s->items, it);
}

void lares_nav_set_item_visible(LaresNav *s, const char *id, gboolean visible)
{
    NavItem *it = nav_find(s, id);
    if (it) gtk_widget_set_visible(it->button, visible);
}

void lares_nav_set_item_label(LaresNav *s, const char *id, const char *label)
{
    NavItem *it = nav_find(s, id);
    if (!it) return;
    GtkWidget *row = gtk_button_get_child(GTK_BUTTON(it->button));
    GtkWidget *l = gtk_widget_get_last_child(row);
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(l)), label) != 0) gtk_label_set_text(GTK_LABEL(l), label);
}

void lares_nav_select(LaresNav *s, const char *id, gboolean animate)
{
    g_free(s->selected);
    s->selected = g_strdup(id);
    s->animate = animate && lares_animations_enabled(GTK_WIDGET(s));
    for (guint i = 0; i < s->items->len; i++) {
        NavItem *it = s->items->pdata[i];
        gboolean on = g_str_equal(it->id, id);
        if (on) gtk_widget_add_css_class(it->button, "selected");
        else gtk_widget_remove_css_class(it->button, "selected");
        gtk_accessible_update_state(GTK_ACCESSIBLE(it->button), GTK_ACCESSIBLE_STATE_SELECTED, on, -1);
    }
    gtk_widget_queue_allocate(GTK_WIDGET(s));
}

static void nav_dispose(GObject *o)
{
    LaresNav *s = LARES_NAV(o);
    unparent_children(GTK_WIDGET(o));
    g_clear_pointer(&s->items, g_ptr_array_unref);
    g_clear_pointer(&s->selected, g_free);
    G_OBJECT_CLASS(lares_nav_parent_class)->dispose(o);
}

static void lares_nav_class_init(LaresNavClass *k)
{
    G_OBJECT_CLASS(k)->dispose = nav_dispose;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = nav_measure;
    wc->size_allocate = nav_allocate;
    gtk_widget_class_set_css_name(wc, "nav");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_NAVIGATION);
    nav_signals[NAV_SELECTED] = g_signal_new("selected", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST,
                                             0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void lares_nav_init(LaresNav *s)
{
    s->items = g_ptr_array_new_with_free_func(nav_item_free);
    s->pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(s->pill, "pill");
    gtk_widget_set_can_target(s->pill, FALSE);
    gtk_widget_set_parent(s->pill, GTK_WIDGET(s));
    s->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_parent(s->box, GTK_WIDGET(s));
}

GtkWidget *lares_nav_new(void)
{
    return g_object_new(LARES_TYPE_NAV, NULL);
}

/* -------------------------------------------------------------------- Clamp */

struct _LaresClamp {
    GtkWidget parent;
    GtkWidget *child;
    int max, lead;
};

G_DEFINE_FINAL_TYPE(LaresClamp, lares_clamp, GTK_TYPE_WIDGET)

static void clamp_measure(GtkWidget *w, GtkOrientation o, int for_size,
                          int *min, int *nat, int *min_base, int *nat_base)
{
    LaresClamp *s = LARES_CLAMP(w);
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        gtk_widget_measure(s->child, o, -1, min, nat, NULL, NULL);
        *nat = MAX(*min, MIN(*nat, s->max));
    } else {
        int width = for_size < 0 ? -1 : MIN(for_size, s->max);
        gtk_widget_measure(s->child, o, width, min, nat, NULL, NULL);
    }
}

static void clamp_allocate(GtkWidget *w, int width, int height, int baseline)
{
    LaresClamp *s = LARES_CLAMP(w);
    int cw = MIN(width, s->max);
    gtk_widget_size_allocate(s->child, &(GtkAllocation){ MIN((width - cw) / 2, s->lead), 0, cw, height }, -1);
}

static void clamp_dispose(GObject *o)
{
    unparent_children(GTK_WIDGET(o));
    G_OBJECT_CLASS(lares_clamp_parent_class)->dispose(o);
}

static void lares_clamp_class_init(LaresClampClass *k)
{
    G_OBJECT_CLASS(k)->dispose = clamp_dispose;
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->get_request_mode = height_for_width;
    wc->measure = clamp_measure;
    wc->size_allocate = clamp_allocate;
    gtk_widget_class_set_css_name(wc, "clamp");
}

static void lares_clamp_init(LaresClamp *s) { s->max = s->lead = 620; }

GtkWidget *lares_clamp_new(GtkWidget *child, int max_width, int max_lead)
{
    LaresClamp *s = g_object_new(LARES_TYPE_CLAMP, NULL);
    s->max = max_width;
    s->lead = max_lead;
    s->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(s));
    return GTK_WIDGET(s);
}

/* -------------------------------------------------------------------- Meter */

struct _LaresMeter {
    GtkWidget parent;
    int blocks, pixel;
    double value;
    LaresMeterChanged changed;
    gpointer changed_user;
};

G_DEFINE_FINAL_TYPE(LaresMeter, lares_meter, GTK_TYPE_WIDGET)

/* Each block is 3 by 3 pixels with one pixel between blocks. */
static void meter_measure(GtkWidget *w, GtkOrientation o, int for_size,
                          int *min, int *nat, int *min_base, int *nat_base)
{
    LaresMeter *s = LARES_METER(w);
    *min = *nat = o == GTK_ORIENTATION_HORIZONTAL ? s->pixel * (4 * s->blocks - 1) : s->pixel * 3;
}

static void meter_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LaresMeter *s = LARES_METER(w);
    GdkRGBA on, off;
    gtk_widget_get_color(w, &on);
    off = on;
    off.alpha *= 0.16f;
    int filled = s->value < 0 ? 0 : (int)round(CLAMP(s->value, 0, 1) * s->blocks);
    if (s->value > 0 && filled == 0) filled = 1;
    int p = s->pixel;
    for (int i = 0; i < s->blocks; i++)
        gtk_snapshot_append_color(snap, i < filled ? &on : &off,
                                  &GRAPHENE_RECT_INIT(i * 4 * p, 0, 3 * p, 3 * p));
}

static void lares_meter_class_init(LaresMeterClass *k)
{
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = meter_measure;
    wc->snapshot = meter_snapshot;
    wc->css_changed = redraw_on_css_change;
    gtk_widget_class_set_css_name(wc, "meter");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_SLIDER);
}

static void lares_meter_init(LaresMeter *s)
{
    s->blocks = 10;
    s->pixel = 2;
    s->value = -1;
}

GtkWidget *lares_meter_new(int blocks, int pixel)
{
    LaresMeter *s = g_object_new(LARES_TYPE_METER, NULL);
    s->blocks = blocks;
    s->pixel = pixel;
    gtk_widget_set_halign(GTK_WIDGET(s), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(s), GTK_ALIGN_CENTER);
    return GTK_WIDGET(s);
}

void lares_meter_set(LaresMeter *s, double fraction)
{
    if (fabs(fraction - s->value) < 0.001) return;
    s->value = fraction;
    gtk_widget_queue_draw(GTK_WIDGET(s));
}

static void meter_pick(LaresMeter *s, double fraction)
{
    fraction = CLAMP(round(fraction * s->blocks) / s->blocks, 0.0, 1.0);
    if (fabs(fraction - s->value) < 0.001) return;
    lares_meter_set(s, fraction);
    s->changed(s, fraction, s->changed_user);
}

/* A block counts once the pointer is past its middle. */
static void meter_at(LaresMeter *s, double x)
{
    meter_pick(s, (x + 2.0 * s->pixel) / (4.0 * s->pixel * s->blocks));
}

static void meter_drag_begin(GtkGestureDrag *g, double x, double y, LaresMeter *s) { meter_at(s, x); }

static void meter_drag_update(GtkGestureDrag *g, double dx, double dy, LaresMeter *s)
{
    double x, y;
    gtk_gesture_drag_get_start_point(g, &x, &y);
    meter_at(s, x + dx);
}

static gboolean meter_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType mods, LaresMeter *s)
{
    int step = keyval == GDK_KEY_Right || keyval == GDK_KEY_Up ? 1 : keyval == GDK_KEY_Left || keyval == GDK_KEY_Down ? -1 : 0;
    if (!step) return FALSE;
    meter_pick(s, s->value + (double)step / s->blocks);
    return TRUE;
}

void lares_meter_set_interactive(LaresMeter *s, LaresMeterChanged fn, gpointer user)
{
    s->changed = fn;
    s->changed_user = user;
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(meter_drag_begin), s);
    g_signal_connect(drag, "drag-update", G_CALLBACK(meter_drag_update), s);
    gtk_widget_add_controller(GTK_WIDGET(s), GTK_EVENT_CONTROLLER(drag));
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(meter_key), s);
    gtk_widget_add_controller(GTK_WIDGET(s), keys);
    gtk_widget_set_focusable(GTK_WIDGET(s), TRUE);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(s), "pointer");
    gtk_widget_add_css_class(GTK_WIDGET(s), "interactive");
    gtk_accessible_update_property(GTK_ACCESSIBLE(s), GTK_ACCESSIBLE_PROPERTY_VALUE_MIN, 0.0,
                                   GTK_ACCESSIBLE_PROPERTY_VALUE_MAX, 1.0, -1);
}

/* -------------------------------------------------------------------- Graph */

#define GRAPH_SAMPLES 240

struct _LaresGraph {
    GtkWidget parent;
    float ring[GRAPH_SAMPLES];
    int head, len, pixel, height;
};

G_DEFINE_FINAL_TYPE(LaresGraph, lares_graph, GTK_TYPE_WIDGET)

static void graph_measure(GtkWidget *w, GtkOrientation o, int for_size,
                          int *min, int *nat, int *min_base, int *nat_base)
{
    LaresGraph *s = LARES_GRAPH(w);
    if (o == GTK_ORIENTATION_HORIZONTAL) { *min = 30 * s->pixel; *nat = 120 * s->pixel; }
    else *min = *nat = s->height;
}

/* Heights snap to whole pixels of the grid, so the graph stays pixel art. */
static void graph_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    LaresGraph *s = LARES_GRAPH(w);
    GdkRGBA c, base;
    gtk_widget_get_color(w, &c);
    base = c;
    base.alpha *= 0.14f;
    int p = s->pixel, width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    int steps = height / p;
    gtk_snapshot_append_color(snap, &base, &GRAPHENE_RECT_INIT(0, height - p, width, p));
    int cols = MIN(width / p, s->len);
    for (int i = 0; i < cols; i++) {
        float v = s->ring[(s->head - 1 - i + GRAPH_SAMPLES) % GRAPH_SAMPLES];
        int h = (int)lroundf(CLAMP(v, 0.0f, 1.0f) * steps);
        if (v > 0 && h == 0) h = 1;
        if (h == 0) continue;
        gtk_snapshot_append_color(snap, &c,
            &GRAPHENE_RECT_INIT(width - (i + 1) * p, height - h * p, p, h * p));
    }
}

static void lares_graph_class_init(LaresGraphClass *k)
{
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    wc->measure = graph_measure;
    wc->snapshot = graph_snapshot;
    wc->css_changed = redraw_on_css_change;
    gtk_widget_class_set_css_name(wc, "graph");
}

static void lares_graph_init(LaresGraph *s)
{
    s->pixel = 3;
    s->height = 54;
}

GtkWidget *lares_graph_new(int pixel, int height)
{
    LaresGraph *s = g_object_new(LARES_TYPE_GRAPH, NULL);
    s->pixel = pixel;
    s->height = height;
    gtk_widget_set_hexpand(GTK_WIDGET(s), TRUE);
    return GTK_WIDGET(s);
}

void lares_graph_push(LaresGraph *s, double fraction)
{
    s->ring[s->head] = (float)fraction;
    s->head = (s->head + 1) % GRAPH_SAMPLES;
    if (s->len < GRAPH_SAMPLES) s->len++;
    gtk_widget_queue_draw(GTK_WIDGET(s));
}
