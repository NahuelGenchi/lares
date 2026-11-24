// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-share: src/share.c without a window, for the Windows port, which
 * runs it inside WSL where the Wi-Fi adapter can do AWDL (issue #98). It
 * takes one command a line on stdin and writes the whole state, a block of
 * lines ending in "end", whenever anything changes:
 *
 *   state setup=ready link=up receiving=1 left=540 sending=0 wake=0 quiet=0
 *   name <how others see this PC>
 *   card <what the card is>
 *   error <why the link failed>
 *   incoming id=3 state=asking done=0 total=0 links=0
 *   incoming-sender <name>
 *   incoming-name <a file's name>            (one a file)
 *   incoming-saved <path>                    (one a file, once done)
 *   incoming-message <why it failed>
 *   device state=found sent=0 total=0 <TAB> key <TAB> name <TAB> model <TAB> message
 *   end
 *
 * Commands: receive on|off, accept, decline, dismiss, look on|off,
 * send <TAB> path <TAB> path..., send-stop, to <TAB> key, cancel, refresh,
 * quit. It ends when stdin does, which stops the link and gives the card
 * back. */
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "../share.h"

static Share *share;
static GMainLoop *loop;
static gboolean looking;
static char *last;       /* the block written last, so the same one isn't written twice */
static guint tick;

/* One line: no tabs or newlines from a name someone else chose. */
static char *one_line(const char *text)
{
    char *t = g_strdup(text ? text : "");
    for (char *p = t; *p; p++)
        if ((guchar)*p < 0x20 || *p == 0x7f) *p = ' ';
    return t;
}

static void put(GString *out, const char *key, const char *text)
{
    if (!text || !*text) return;
    char *t = one_line(text);
    g_string_append_printf(out, "%s %s\n", key, t);
    g_free(t);
}

static const char *setup_name(ShareSetup s)
{
    return s == SHARE_READY ? "ready" : s == SHARE_NEEDS_ALLOW ? "needs-allow" : "no-card";
}

static const char *link_name(ShareLink l)
{
    switch (l) {
    case LINK_STARTING: return "starting";
    case LINK_UP:       return "up";
    case LINK_STOPPING: return "stopping";
    case LINK_FAILED:   return "failed";
    default:            return "off";
    }
}

static const char *device_name(DeviceState d)
{
    switch (d) {
    case DEVICE_ASKING:   return "asking";
    case DEVICE_SENDING:  return "sending";
    case DEVICE_SENT:     return "sent";
    case DEVICE_DECLINED: return "declined";
    case DEVICE_FAILED:   return "failed";
    default:              return "found";
    }
}

static const char *incoming_name(IncomingState i)
{
    switch (i) {
    case INCOMING_RECEIVING: return "receiving";
    case INCOMING_DONE:      return "done";
    case INCOMING_FAILED:    return "failed";
    default:                 return "asking";
    }
}

static void write_state(gpointer user)
{
    const ShareState *st = share_state(share);
    GString *out = g_string_new(NULL);
    gint64 left = st->receiving ? (st->receive_until - g_get_monotonic_time()) / G_USEC_PER_SEC : 0;
    g_string_append_printf(out, "state setup=%s link=%s receiving=%d left=%" G_GINT64_FORMAT " sending=%d wake=%d quiet=%d\n",
                           setup_name(st->setup), link_name(st->link), st->receiving, MAX(left, 0),
                           st->sending, st->sending || looking, st->quiet);
    put(out, "name", st->name);
    put(out, "card", st->card_text);
    put(out, "error", st->link_error);
    if (st->incoming) {
        const ShareIncoming *in = st->incoming;
        g_string_append_printf(out, "incoming id=%u state=%s done=%" G_GUINT64_FORMAT " total=%" G_GUINT64_FORMAT " links=%d\n",
                               in->id, incoming_name(in->state), in->done, in->total, in->links);
        put(out, "incoming-sender", in->sender);
        for (guint i = 0; in->names && i < in->names->len; i++) put(out, "incoming-name", in->names->pdata[i]);
        for (guint i = 0; in->saved && i < in->saved->len; i++) put(out, "incoming-saved", in->saved->pdata[i]);
        put(out, "incoming-message", in->message);
    }
    for (guint i = 0; st->devices && i < st->devices->len; i++) {
        const ShareDevice *d = st->devices->pdata[i];
        char *key = one_line(d->key), *name = one_line(d->name), *model = one_line(d->model), *msg = one_line(d->message);
        g_string_append_printf(out, "device state=%s sent=%" G_GUINT64_FORMAT " total=%" G_GUINT64_FORMAT "\t%s\t%s\t%s\t%s\n",
                               device_name(d->state), d->sent, d->total, key, name, model, msg);
        g_free(key); g_free(name); g_free(model); g_free(msg);
    }
    g_string_append(out, "end\n");
    if (!last || !g_str_equal(last, out->str)) {
        fputs(out->str, stdout);
        fflush(stdout);
        g_free(last);
        last = g_string_free(out, FALSE);
    } else {
        g_string_free(out, TRUE);
    }
}

/* While receiving, the seconds left count down. */
static gboolean on_tick(gpointer user)
{
    write_state(NULL);
    return G_SOURCE_CONTINUE;
}

static void command(const char *line)
{
    char **a = g_strsplit(line, "\t", -1);
    const char *c = a[0] ? a[0] : "";
    if (g_str_equal(c, "receive on")) share_set_receiving(share, TRUE);
    else if (g_str_equal(c, "receive off")) share_set_receiving(share, FALSE);
    else if (g_str_equal(c, "accept")) share_answer(share, TRUE);
    else if (g_str_equal(c, "decline")) share_answer(share, FALSE);
    else if (g_str_equal(c, "dismiss")) share_dismiss_incoming(share);
    else if (g_str_equal(c, "look on") || g_str_equal(c, "look off")) {
        looking = g_str_equal(c, "look on");
        share_set_looking(share, looking);
    } else if (g_str_equal(c, "send") && a[1]) {
        looking = FALSE;
        share_set_sending(share, (const char *const *)(a + 1));
    } else if (g_str_equal(c, "send-stop")) {
        looking = FALSE;
        share_set_sending(share, NULL);
    } else if (g_str_equal(c, "to") && a[1]) share_send_to(share, a[1]);
    else if (g_str_equal(c, "cancel")) share_cancel_send(share);
    else if (g_str_equal(c, "refresh")) share_refresh(share);
    else if (g_str_equal(c, "quit")) g_main_loop_quit(loop);
    g_strfreev(a);
    write_state(NULL);
}

static void on_line(GObject *src, GAsyncResult *res, gpointer user)
{
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
    if (!line) {   /* stdin ended: whoever ran this is gone */
        g_main_loop_quit(loop);
        return;
    }
    g_strchomp(line);
    command(line);
    g_free(line);
    g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(src), G_PRIORITY_DEFAULT, NULL, on_line, NULL);
}

int main(void)
{
    /* Whoever read the state may be gone before the card is given back. */
    signal(SIGPIPE, SIG_IGN);
    loop = g_main_loop_new(NULL, FALSE);
    share = share_new();
    share_watch(share, write_state, NULL);
    share_refresh(share);
    write_state(NULL);
    tick = g_timeout_add_seconds(5, on_tick, NULL);

    GInputStream *in = g_unix_input_stream_new(0, FALSE);
    GDataInputStream *lines = g_data_input_stream_new(in);
    g_data_input_stream_read_line_async(lines, G_PRIORITY_DEFAULT, NULL, on_line, NULL);
    g_main_loop_run(loop);

    /* Stop what uses the link and wait for it to give the card back. */
    share_set_sending(share, NULL);
    share_set_looking(share, FALSE);
    share_set_receiving(share, FALSE);
    share_stop_link(share);
    for (int i = 0; i < 250 && ((share_state(share)->link != LINK_OFF && share_state(share)->link != LINK_FAILED)
                                || share_bluetooth_lent(share)); i++) {
        gint64 until = g_get_monotonic_time() + 100 * 1000;
        while (g_get_monotonic_time() < until) g_main_context_iteration(NULL, FALSE);
        g_usleep(20 * 1000);
    }
    return 0;
}
