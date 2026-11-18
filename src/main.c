// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Lares: one calm place to check and manage this PC and its devices. */
#include "lares.h"
#include "pad_service.h"

/* --------------------------------------------------------- command line */

typedef struct {
    Remote *be;
    GMainLoop *loop;
    const Item *order;
    int n, step, failed;
} Cli;

static void cli_next(Cli *c);

static void cli_done(gboolean ok, gboolean perm, const char *msg, gpointer user)
{
    Cli *c = user;
    Config *cf = remote_config(c->be);
    Item i = c->order[c->step];
    char *d = remote_describe(i, cf->values[cf->mode][i]);
    if (ok) {
        g_print("  %-10s %s\n", item_name(i), d);
    } else {
        g_print("  %-10s %s didn't happen: %s\n", item_name(i), d, msg);
        if (perm)
            g_print("  %-10s Allow it once with: pkexec tailscale set --operator=%s\n", "", g_get_user_name());
        c->failed++;
    }
    g_free(d);
    c->step++;
    cli_next(c);
}

static void cli_next(Cli *c)
{
    if (c->step >= c->n) {
        g_main_loop_quit(c->loop);
        return;
    }
    Config *cf = remote_config(c->be);
    Item i = c->order[c->step];
    remote_apply(c->be, i, cf->values[cf->mode][i], cli_done, c);
}

static void cli_status(gpointer user)
{
    Cli *c = user;
    Config *cf = remote_config(c->be);
    g_print("%s\n", cf->mode_known ? mode_name(cf->mode) : "No mode picked yet");
    for (int i = 0; i < N_ITEMS; i++) {
        char *now = remote_describe_live(c->be, i);
        g_print("  %-10s %s%s\n", item_name(i), now,
                cf->mode_known && !remote_matches(c->be, i, cf->values[cf->mode][i]) ? "  (differs)" : "");
        g_free(now);
    }
    if (remote_live(c->be)->streaming) g_print("  Moonlight is streaming\n");
    g_main_loop_quit(c->loop);
}

static int run_cli(const char *cmd)
{
    Cli c = { 0 };
    c.be = remote_new();
    c.loop = g_main_loop_new(NULL, FALSE);
    Config *cf = remote_config(c.be);
    if (g_str_equal(cmd, "status")) {
        remote_read_live(c.be, cli_status, &c);
    } else {
        if (g_str_equal(cmd, "home")) cf->mode = MODE_HOME;
        else if (g_str_equal(cmd, "remote")) cf->mode = MODE_REMOTE;
        else if (!cf->mode_known) { g_print("No mode picked yet, nothing to restore.\n"); return 0; }
        cf->mode_known = TRUE;
        remote_save(c.be);
        g_print("%s\n", mode_name(cf->mode));
        c.order = apply_order(cf->mode, &c.n);
        cli_next(&c);
    }
    if (c.step < c.n || g_str_equal(cmd, "status")) g_main_loop_run(c.loop);
    return c.failed ? 1 : 0;
}

/* ------------------------------------------------------------------ window */

static Lares *the_app;

static void activate(GtkApplication *gapp, gpointer data)
{
    GList *wins = gtk_application_get_windows(gapp);
    if (wins) {
        gtk_window_present(wins->data);
        return;
    }
    g_object_set(gtk_settings_get_default(), "gtk-hint-font-metrics", TRUE, NULL);
    const char *scheme = g_getenv("LARES_COLOR_SCHEME"); /* "dark" or "light", for testing */
    if (scheme)
        g_object_set(gtk_settings_get_default(), "gtk-interface-color-scheme",
                     g_str_equal(scheme, "dark") ? GTK_INTERFACE_COLOR_SCHEME_DARK
                                                 : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
    Lares *app = g_new0(Lares, 1);
    app->gapp = gapp;
    app->remote = remote_new();
    app->hw = hardware_new();
    app->devices = devices_new();
    app->padconf = padconf_new();
    app->pads = pads_new(app->padconf);
    the_app = app;
    lares_open_window(app);
}

/* `lares --section controller` opens on a section, also when Lares is
 * already open: the running copy gets the words and shows it. */
static int command_line(GApplication *gapp, GApplicationCommandLine *cmd, gpointer data)
{
    int argc;
    char **argv = g_application_command_line_get_arguments(cmd, &argc);
    activate(GTK_APPLICATION(gapp), NULL);
    for (int i = 1; i + 1 < argc; i++)
        if (g_str_equal(argv[i], "--section") && the_app) lares_show(the_app, argv[i + 1]);
    g_strfreev(argv);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !g_str_equal(argv[1], "--section")) {
        const char *cmd = argv[1];
        if (g_str_equal(cmd, "home") || g_str_equal(cmd, "remote")
            || g_str_equal(cmd, "restore") || g_str_equal(cmd, "status"))
            return run_cli(cmd);
        g_print("Usage: lares [home | remote | restore | status | --section NAME]\n"
                "With no word, Lares opens its window.\n");
        return g_str_equal(cmd, "--help") || g_str_equal(cmd, "-h") ? 0 : 2;
    }
    /* GTK's software renderer: this window is flat colors and pixel text, so
     * drawing it on the CPU costs almost nothing, while the GPU renderers
     * load the whole graphics driver, about 80 MB more memory. */
    g_setenv("GSK_RENDERER", "cairo", FALSE);
    GtkApplication *gapp = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(gapp, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(gapp, "command-line", G_CALLBACK(command_line), NULL);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    return status;
}
