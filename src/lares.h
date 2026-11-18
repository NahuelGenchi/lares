#pragma once
#include <gtk/gtk.h>
#include "art.h"
#include "devices.h"
#include "hardware.h"
#include "remote_model.h"
#include "widgets.h"
#include "pad/padconf.h"
#include "pad/pads.h"

#define APP_ID "com.nahuelgenchi.Lares"

typedef struct Lares Lares;
typedef struct Section Section;

/* A section is built the first time it's opened, and is told when it's on
 * screen so it only reads anything while someone can see it. */
struct Section {
    const char *id, *label;
    const char *const *icon;
    int kind;                                   /* DevKind, for device sections */
    GtkWidget *(*build)(Lares *app, Section *self);
    void (*shown)(Lares *app, Section *self, gboolean on);
    GtkWidget *page;
    gpointer view;
    gboolean on_screen;
};

struct Lares {
    GtkApplication *gapp;
    GtkWidget *win, *nav, *stack;
    Remote *remote;
    Hardware *hw;
    Devices *devices;
    PadConf *padconf;
    Pads *pads;
    Section *sections;
    int n_sections;
    Section *current;
};

void lares_open_window(Lares *app);
void lares_show(Lares *app, const char *id);
void lares_set_mode_colors(Lares *app);

/* Sections */
GtkWidget *overview_build(Lares *app, Section *self);
void overview_shown(Lares *app, Section *self, gboolean on);
GtkWidget *hardware_build(Lares *app, Section *self);
void hardware_shown(Lares *app, Section *self, gboolean on);
GtkWidget *device_build(Lares *app, Section *self);
void device_shown(Lares *app, Section *self, gboolean on);
GtkWidget *pad_view_build(Lares *app, Section *self);
void pad_view_shown(Lares *app, Section *self, gboolean on);
GtkWidget *remote_build(Lares *app, Section *self);
void remote_shown(Lares *app, Section *self, gboolean on);
GtkWidget *settings_build(Lares *app, Section *self);

/* Small helpers shared by the sections */
GtkWidget *ui_label(const char *text, const char *css_class);
GtkWidget *ui_icon(const char *const *art, int pixel, const char *css_class);
void ui_class(GtkWidget *w, const char *css_class, gboolean on);
GtkWidget *ui_page(void);
GtkWidget *ui_group(GtkWidget *page, const char *heading);
GtkWidget *ui_setting(GtkWidget *group, const char *title, const char *sub);
GtkWidget *ui_setting_switch(GtkWidget *row, const char *title, gboolean *field, GCallback cb, gpointer data);
const char *const *device_art(DevKind kind);
char *battery_text(int battery);
