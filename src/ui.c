/* Small helpers shared by the sections. */
#include "lares.h"

GtkWidget *ui_label(const char *text, const char *css_class)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css_class) gtk_widget_add_css_class(l, css_class);
    return l;
}

/* A label that wraps: a paragraph, with room between its lines. */
GtkWidget *ui_paragraph(const char *text, const char *css_class)
{
    GtkWidget *l = ui_label(text, css_class);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "para");
    return l;
}

/* A button's text, or while it works a spinner over the same text made
 * invisible, so the button keeps its size and what's beside it stays put. */
GtkWidget *ui_busy_child(const char *text, gboolean busy)
{
    GtkWidget *label = gtk_label_new(text);
    if (!busy) return label;
    gtk_widget_set_opacity(label, 0);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), label);
    GtkWidget *spin = lares_pixel_spinner_new(2);
    gtk_widget_set_halign(spin, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), spin);
    return overlay;
}

/* A button whose text changes keeps the width of its widest text, so
 * neither it nor what's beside it moves when it says something else.
 * `labels` ends with NULL, and the first shows; ui_button_label shows
 * another of them. */
void ui_button_labels(GtkWidget *button, const char *const *labels)
{
    GtkWidget *stack = gtk_stack_new();
    for (int i = 0; labels[i]; i++) gtk_stack_add_named(GTK_STACK(stack), gtk_label_new(labels[i]), labels[i]);
    gtk_button_set_child(GTK_BUTTON(button), stack);
    /* A button made with a label stays "labelled by" it once it's gone,
     * which leaves it nameless: it's named by ui_button_label instead. */
    gtk_accessible_reset_relation(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_RELATION_LABELLED_BY);
    ui_button_label(button, labels[0]);
}

void ui_button_label(GtkWidget *button, const char *label)
{
    gtk_stack_set_visible_child_name(GTK_STACK(gtk_button_get_child(GTK_BUTTON(button))), label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
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
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, 36);
}

/* A heading over a card of settings; hiding the card's parent hides both. */
GtkWidget *ui_group(GtkWidget *page, const char *heading)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_box_append(GTK_BOX(section), ui_label(heading, "group-heading"));
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, "padded");
    gtk_box_append(GTK_BOX(section), card);
    gtk_box_append(GTK_BOX(page), section);
    return card;
}

/* A sub-section inside a group's card, for one device: its pixel icon and
 * name over the settings that follow. */
void ui_subheading(GtkWidget *group, const char *const *art, const char *title)
{
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_box_append(GTK_BOX(head), ui_icon(art, 2, "tile-icon"));
    GtkWidget *l = ui_label(title, "card-title");
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(head), l);
    gtk_box_append(GTK_BOX(group), head);
}

/* A setting's row in a group: its title, an optional line under it, and
 * room on the right for its control. The row's first child is the column of
 * texts, where a note about the setting belongs. */
GtkWidget *ui_setting(GtkWidget *group, const char *title, const char *sub)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(row, "setting");
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_hexpand(texts, TRUE);
    /* A title alone sits level with its control, not at the row's top. */
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(texts), ui_label(title, NULL));
    if (sub) {
        GtkWidget *l = ui_paragraph(sub, "muted");
        gtk_box_append(GTK_BOX(texts), l);
    }
    gtk_box_append(GTK_BOX(row), texts);
    gtk_box_append(GTK_BOX(group), row);
    return row;
}

/* What turning a setting off saves, as a small tag under its description:
 * "Off saves 1 MB RAM". Only for a saving a measurement backs. */
void ui_setting_saving(GtkWidget *row, const char *text)
{
    GtkWidget *tag = ui_label(text, "saving");
    gtk_widget_set_halign(tag, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(gtk_widget_get_first_child(row)), tag);
}

/* A switch at the end of a setting's row, for the boolean `field`. `cb` runs
 * on notify::active and finds the field in the switch's "field" data. */
GtkWidget *ui_setting_switch(GtkWidget *row, const char *title, gboolean *field, GCallback cb, gpointer data)
{
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(sw), "field", field);
    g_signal_connect(sw, "notify::active", cb, data);
    gtk_accessible_update_property(GTK_ACCESSIBLE(sw), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    gtk_box_append(GTK_BOX(row), sw);
    return sw;
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
