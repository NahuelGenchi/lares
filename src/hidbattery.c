// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* HyperX's wireless mice answer battery questions on their receiver's vendor
 * interface, in 64-byte reports whose first byte is the report ID, as the
 * ngenuity-linux-cli project worked out from NGENUITY and tested on the
 * Pulsefire Haste 2 Wireless. Output report 50 02 asks; the mouse answers
 * with input report 51 02: byte 2 the level, byte 3 the charging state
 * (0 not charging, 1 charging, 2 full). The receiver acknowledges every
 * report first with report ff, and while the mouse sleeps that's all that
 * comes back. Nothing here changes a setting on the mouse. */
#include "hidbattery.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#define REPORT_SIZE 64

static const HidBatteryModel MODELS[] = {
    { 0x03f0, 0x0f98, 2 },   /* HyperX Pulsefire Haste 2 Wireless, its 2.4 GHz receiver */
};

const HidBatteryModel *hidbattery_model(guint16 vendor, guint16 product)
{
    for (guint i = 0; i < G_N_ELEMENTS(MODELS); i++)
        if (MODELS[i].vendor == vendor && MODELS[i].product == product) return &MODELS[i];
    return NULL;
}

gboolean hidbattery_parse(const guint8 *report, gsize len, int *level, gboolean *charging)
{
    if (len < 4 || report[0] != 0x51 || report[1] != 0x02 || report[2] > 100) return FALSE;
    *level = report[2];
    *charging = report[3] == 1;
    return TRUE;
}

/* Reads reports until the battery answer or `ms` pass, skipping the
 * receiver's acknowledgements and anything else the mouse sends. */
static gboolean wait_answer(int fd, int ms, int *level, gboolean *charging)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    for (;;) {
        int left = (int)((end - g_get_monotonic_time()) / 1000);
        if (left <= 0) return FALSE;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int ready = poll(&p, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return FALSE;
        guint8 report[REPORT_SIZE];
        ssize_t got = read(fd, report, sizeof report);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (got <= 0) return FALSE;
        if (hidbattery_parse(report, (gsize)got, level, charging)) return TRUE;
    }
}

gboolean hidbattery_query(const char *node, int *level, gboolean *charging, GError **error)
{
    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s", g_strerror(e));
        return FALSE;
    }
    const guint8 ask[REPORT_SIZE] = { 0x50, 0x02 };
    gboolean ok = FALSE, sent = TRUE;
    /* The receiver may only wake the mouse on the first ask. */
    for (int round = 0; round < 2 && !ok && sent; round++) {
        sent = write(fd, ask, sizeof ask) == (ssize_t)sizeof ask;
        ok = sent && wait_answer(fd, round == 0 ? 400 : 800, level, charging);
    }
    close(fd);
    if (!ok)
        g_set_error_literal(error, G_IO_ERROR, sent ? G_IO_ERROR_TIMED_OUT : G_IO_ERROR_FAILED,
                            sent ? "It didn't answer; it may be asleep." : "It couldn't be asked.");
    return ok;
}

char *hidbattery_udev_rules(void)
{
    GString *s = g_string_new("# Installed by Lares: lets the person at the screen read these devices'\n"
                              "# batteries, through their battery interface only.\n");
    for (guint i = 0; i < G_N_ELEMENTS(MODELS); i++)
        g_string_append_printf(s, "SUBSYSTEM==\"hidraw\", ENV{ID_USB_VENDOR_ID}==\"%04x\", "
                               "ENV{ID_USB_MODEL_ID}==\"%04x\", ENV{ID_USB_INTERFACE_NUM}==\"%02d\", "
                               "TAG+=\"uaccess\"\n", MODELS[i].vendor, MODELS[i].product, MODELS[i].interface);
    return g_string_free(s, FALSE);
}
