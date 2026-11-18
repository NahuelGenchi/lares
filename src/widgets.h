#pragma once
#include <gtk/gtk.h>

/* Spring motion, shared by everything that slides. Positions are in any
 * unit; `rest` is how close counts as arrived in that unit. */
typedef struct {
    double pos, vel, target;
    gint64 last;
} LaresSpring;

gboolean lares_spring_step(LaresSpring *s, gint64 frame_time, double stiffness,
                           double damping_ratio, double rest);
void lares_spring_jump(LaresSpring *s, double to);
gboolean lares_animations_enabled(GtkWidget *w);

/* A pixel-art icon drawn in the widget's CSS color. Rows use '#' for a full
 * pixel, '+' for a half-tone pixel and '.' for empty. */
#define LARES_TYPE_PIXEL_ICON (lares_pixel_icon_get_type())
G_DECLARE_FINAL_TYPE(LaresPixelIcon, lares_pixel_icon, LARES, PIXEL_ICON, GtkWidget)
GtkWidget *lares_pixel_icon_new(const char *const *rows, int pixel);

/* A pixel loader: a light runs around a 3 by 3 ring of blocks. */
#define LARES_TYPE_PIXEL_SPINNER (lares_pixel_spinner_get_type())
G_DECLARE_FINAL_TYPE(LaresPixelSpinner, lares_pixel_spinner, LARES, PIXEL_SPINNER, GtkWidget)
GtkWidget *lares_pixel_spinner_new(int pixel);

/* Two-sided switch with a pill that springs between the sides.
 * "picked" (int side) fires when the person asks for a side; the code using
 * it decides and calls lares_mode_switch_set_active(). */
#define LARES_TYPE_MODE_SWITCH (lares_mode_switch_get_type())
G_DECLARE_FINAL_TYPE(LaresModeSwitch, lares_mode_switch, LARES, MODE_SWITCH, GtkWidget)
GtkWidget *lares_mode_switch_new(const char *const *icon_a, const char *label_a,
                                 const char *const *icon_b, const char *label_b);
void lares_mode_switch_set_active(LaresModeSwitch *self, int side, gboolean animate);

/* Lays children out left to right at their natural width and wraps them,
 * without stretching them to fill a line the way GtkFlowBox does. */
#define LARES_TYPE_WRAP (lares_wrap_get_type())
G_DECLARE_FINAL_TYPE(LaresWrap, lares_wrap, LARES, WRAP, GtkWidget)
GtkWidget *lares_wrap_new(int spacing);
/* The gap between lines when they wrap; the spacing until it's set. */
void lares_wrap_set_row_spacing(LaresWrap *self, int spacing);
void lares_wrap_append(LaresWrap *self, GtkWidget *child);

/* The sidebar: one item per section, with a highlight that springs to the
 * picked one. "selected" (const char *id) fires when the person picks. */
#define LARES_TYPE_NAV (lares_nav_get_type())
G_DECLARE_FINAL_TYPE(LaresNav, lares_nav, LARES, NAV, GtkWidget)
GtkWidget *lares_nav_new(void);
void lares_nav_add(LaresNav *self, const char *id, const char *const *icon, const char *label);
void lares_nav_set_item_visible(LaresNav *self, const char *id, gboolean visible);
void lares_nav_set_item_label(LaresNav *self, const char *id, const char *label);
void lares_nav_select(LaresNav *self, const char *id, gboolean animate);

/* Gives its child at most `max_width`, centered, but at most `max_lead`
 * from the start: content stays a readable column however wide the window
 * gets, and stays near what comes before it. */
#define LARES_TYPE_CLAMP (lares_clamp_get_type())
G_DECLARE_FINAL_TYPE(LaresClamp, lares_clamp, LARES, CLAMP, GtkWidget)
GtkWidget *lares_clamp_new(GtkWidget *child, int max_width, int max_lead);

/* A row of pixel blocks filled to a fraction, for batteries and load. */
#define LARES_TYPE_METER (lares_meter_get_type())
G_DECLARE_FINAL_TYPE(LaresMeter, lares_meter, LARES, METER, GtkWidget)
GtkWidget *lares_meter_new(int blocks, int pixel);
void lares_meter_set(LaresMeter *self, double fraction);
/* Lets the person set it by clicking, dragging or with the arrow keys, one
 * block at a time. */
typedef void (*LaresMeterChanged)(LaresMeter *self, double fraction, gpointer user);
void lares_meter_set_interactive(LaresMeter *self, LaresMeterChanged fn, gpointer user);

/* Recent samples as pixel columns, newest on the right. */
#define LARES_TYPE_GRAPH (lares_graph_get_type())
G_DECLARE_FINAL_TYPE(LaresGraph, lares_graph, LARES, GRAPH, GtkWidget)
GtkWidget *lares_graph_new(int pixel, int height);
void lares_graph_push(LaresGraph *self, double fraction);
