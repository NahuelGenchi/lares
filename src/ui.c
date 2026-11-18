/* Small helpers shared by the sections. */
#include "lares.h"

GtkWidget *ui_label(const char *text, const char *css_class)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css_class) gtk_widget_add_css_class(l, css_class);
    return l;
}

GtkWidget *ui_icon(const char *const *art, int pixel, const char *css_class)
{
    GtkWidget *w = lares_pixel_icon_new(art, pixel);
    if (css_class) gtk_widget_add_css_class(w, css_class);
    return w;
}

void ui_class(GtkWidget *w, const char *css_class, gboolean on)
{
    if (on) gtk_widget_add_css_class(w, css_class);
    else gtk_widget_remove_css_class(w, css_class);
}

/* Every section's page is a column with the same spacing. */
GtkWidget *ui_page(void)
{
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, 27);
}

const char *const *device_art(DevKind kind)
{
    switch (kind) {
    case DEV_CONTROLLER: return ART_GAMEPAD;
    case DEV_AUDIO:      return ART_HEADPHONES;
    case DEV_KEYBOARD:   return ART_KEYBOARD;
    default:             return ART_MOUSE;
    }
}

char *battery_text(int battery)
{
    return battery < 0 ? g_strdup("No battery reading") : g_strdup_printf("%d%%", battery);
}
