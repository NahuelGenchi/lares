// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "padinput.h"
#include "pads.h"
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FRAME_US        (G_USEC_PER_SEC / 60)
#define ACTIVITY_EVERY  5      /* seconds between "in use" reports */
#define MOVED           0.25   /* how far a stick or trigger must move to count */

#define BITS_LONGS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define HAS_BIT(a, n) (((a)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long)))) & 1)

static const struct { int code; PadButton button; } KEYS[] = {
    { BTN_SOUTH, PB_A }, { BTN_EAST, PB_B }, { BTN_NORTH, PB_X }, { BTN_WEST, PB_Y },
    { BTN_TL, PB_LB }, { BTN_TR, PB_RB }, { BTN_SELECT, PB_VIEW }, { BTN_START, PB_MENU },
    { BTN_MODE, PB_XBOX }, { KEY_RECORD, PB_SHARE }, { BTN_THUMBL, PB_LS }, { BTN_THUMBR, PB_RS },
    { BTN_DPAD_UP, PB_UP }, { BTN_DPAD_DOWN, PB_DOWN }, { BTN_DPAD_LEFT, PB_LEFT },
    { BTN_DPAD_RIGHT, PB_RIGHT },
};

typedef struct { gboolean present; int value, low, high, flat; } Axis;

struct PadReader {
    int fd;
    guint watch, timer;
    gboolean raw, activity;
    int stick[4], trigger[2];      /* axis codes: lx ly rx ry, lt rt */
    Axis axes[ABS_CNT];
    guint32 keys;
    PadState shown;
    gint64 shown_at, reported_at;
    gboolean pending;              /* used since the last report, inside its window */
    PadStateFn on_state;
    PadUsedFn on_used;
    PadGoneFn on_gone;
    gpointer user, gone_user;
};

/* ----------------------------------------------------------------- values */

static float stick(PadReader *r, int code, gboolean raw)
{
    Axis *a = &r->axes[code];
    if (!a->present || a->high == a->low) return 0;
    double middle = (a->low + a->high) / 2.0, offset = a->value - middle;
    if (fabs(offset) <= a->flat && !raw) return 0;
    double v = CLAMP(offset / ((a->high - a->low) / 2.0), -1, 1);
    double step = raw ? 1000 : 100;
    return (float)(round(v * step) / step);
}

static float trigger(PadReader *r, int code, gboolean raw)
{
    Axis *a = &r->axes[code];
    if (!a->present || a->high == a->low) return 0;
    double v = CLAMP((double)(a->value - a->low) / (a->high - a->low), 0, 1);
    double step = raw ? 1000 : 100;
    return (float)(round(v * step) / step);
}

static void make_state(PadReader *r, PadState *s)
{
    memset(s, 0, sizeof *s);
    s->buttons = r->keys;
    int hx = r->axes[ABS_HAT0X].value, hy = r->axes[ABS_HAT0Y].value;
    if (hx) s->buttons |= PB_BIT(hx < 0 ? PB_LEFT : PB_RIGHT);
    if (hy) s->buttons |= PB_BIT(hy < 0 ? PB_UP : PB_DOWN);
    s->lx = stick(r, r->stick[0], r->raw);
    s->ly = stick(r, r->stick[1], r->raw);
    s->rx = stick(r, r->stick[2], r->raw);
    s->ry = stick(r, r->stick[3], r->raw);
    s->lt = trigger(r, r->trigger[0], r->raw);
    s->rt = trigger(r, r->trigger[1], r->raw);
}

/* An axis away from rest: the d-pad pressed, or a stick or trigger moved at
 * least MOVED of its way. */
static gboolean moved(PadReader *r, int code)
{
    if (code == ABS_HAT0X || code == ABS_HAT0Y) return r->axes[code].value != 0;
    if (code == r->trigger[0] || code == r->trigger[1]) return trigger(r, code, TRUE) >= MOVED;
    return fabsf(stick(r, code, TRUE)) >= MOVED;
}

static gboolean held(PadReader *r)
{
    if (r->keys) return TRUE;
    for (int c = 0; c < ABS_CNT; c++)
        if (r->axes[c].present && moved(r, c)) return TRUE;
    return FALSE;
}

/* Reads again what's held right now, which also covers presses and releases
 * that happened while not reading. */
static void refresh(PadReader *r)
{
    for (int c = 0; c < ABS_CNT; c++) {
        if (!r->axes[c].present) continue;
        struct input_absinfo info;
        if (ioctl(r->fd, EVIOCGABS(c), &info) == 0) r->axes[c].value = info.value;
    }
    unsigned long held_keys[BITS_LONGS(KEY_CNT)] = { 0 };
    r->keys = 0;
    if (ioctl(r->fd, EVIOCGKEY(sizeof held_keys), held_keys) >= 0)
        for (guint i = 0; i < G_N_ELEMENTS(KEYS); i++)
            if (HAS_BIT(held_keys, KEYS[i].code)) r->keys |= PB_BIT(KEYS[i].button);
}

/* -------------------------------------------------------------- reading */

static void gone(PadReader *r)
{
    if (r->watch) { g_source_remove(r->watch); r->watch = 0; }
    if (r->timer) { g_source_remove(r->timer); r->timer = 0; }
    if (r->on_gone) r->on_gone(r->gone_user);
}

static void emit_state(PadReader *r)
{
    PadState s;
    make_state(r, &s);
    if (memcmp(&s, &r->shown, sizeof s) == 0) return;
    r->shown = s;
    r->shown_at = g_get_monotonic_time();
    r->on_state(&s, r->user);
}

static gboolean frame_due(gpointer data)
{
    PadReader *r = data;
    r->timer = 0;
    emit_state(r);
    return G_SOURCE_REMOVE;
}

static gboolean recheck_held(gpointer data);

static void report_use(PadReader *r)
{
    r->reported_at = g_get_monotonic_time();
    r->on_used(r->user);
    /* A button or stick held still sends no events; look again later. */
    if (!r->timer) r->timer = g_timeout_add_seconds(ACTIVITY_EVERY, recheck_held, r);
}

static gboolean recheck_held(gpointer data)
{
    PadReader *r = data;
    r->timer = 0;
    refresh(r);
    if (r->pending || held(r)) {
        r->pending = FALSE;
        report_use(r);
    }
    return G_SOURCE_REMOVE;
}

static gboolean readable(int fd, GIOCondition cond, gpointer data)
{
    PadReader *r = data;
    if (cond & (G_IO_HUP | G_IO_ERR)) { r->watch = 0; gone(r); return G_SOURCE_REMOVE; }

    struct input_event ev[64];
    gboolean used = FALSE;
    for (;;) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n < 0 && errno == ENODEV) { r->watch = 0; gone(r); return G_SOURCE_REMOVE; }
            break;
        }
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_KEY) {
                for (guint k = 0; k < G_N_ELEMENTS(KEYS); k++) {
                    if (KEYS[k].code != e->code) continue;
                    if (e->value) r->keys |= PB_BIT(KEYS[k].button);
                    else r->keys &= ~PB_BIT(KEYS[k].button);
                }
                if (e->value == 1) used = TRUE;
            } else if (e->type == EV_ABS && e->code < ABS_CNT && r->axes[e->code].present) {
                r->axes[e->code].value = e->value;
                if (r->activity && moved(r, e->code)) used = TRUE;
            } else if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                refresh(r);
            }
        }
    }

    if (r->activity) {
        /* Use inside the five seconds after a report is told when they end. */
        if (used && g_get_monotonic_time() - r->reported_at >= ACTIVITY_EVERY * G_USEC_PER_SEC)
            report_use(r);
        else if (used)
            r->pending = TRUE;
        return G_SOURCE_CONTINUE;
    }
    /* At most one frame per 1/60 s; a change inside a frame waits for it. */
    gint64 wait = r->shown_at + FRAME_US - g_get_monotonic_time();
    if (wait <= 0) emit_state(r);
    else if (!r->timer) r->timer = g_timeout_add(MAX(1, (guint)(wait / 1000)), frame_due, r);
    return G_SOURCE_CONTINUE;
}

static PadReader *open_reader(const char *node)
{
    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    PadReader *r = g_new0(PadReader, 1);
    r->fd = fd;

    unsigned long abs_bits[BITS_LONGS(ABS_CNT)] = { 0 };
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits);
    /* hid-microsoft over Bluetooth puts the triggers on GAS and BRAKE and the
     * right stick on Z and RZ; xpad on a cable uses Z and RZ for triggers. */
    if (HAS_BIT(abs_bits, ABS_GAS) && HAS_BIT(abs_bits, ABS_BRAKE)) {
        int s[4] = { ABS_X, ABS_Y, ABS_Z, ABS_RZ }, t[2] = { ABS_BRAKE, ABS_GAS };
        memcpy(r->stick, s, sizeof s);
        memcpy(r->trigger, t, sizeof t);
    } else {
        int s[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY }, t[2] = { ABS_Z, ABS_RZ };
        memcpy(r->stick, s, sizeof s);
        memcpy(r->trigger, t, sizeof t);
    }
    int used[8] = { r->stick[0], r->stick[1], r->stick[2], r->stick[3], r->trigger[0], r->trigger[1],
                    ABS_HAT0X, ABS_HAT0Y };
    for (int i = 0; i < 8; i++) {
        int c = used[i];
        struct input_absinfo info;
        if (!HAS_BIT(abs_bits, c) || ioctl(fd, EVIOCGABS(c), &info) != 0) continue;
        r->axes[c] = (Axis){ TRUE, info.value, info.minimum, info.maximum, info.flat };
    }
    refresh(r);
    r->watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, readable, r);
    return r;
}

PadReader *pad_reader_live(const char *node, gboolean raw, PadStateFn fn, gpointer user)
{
    PadReader *r = open_reader(node);
    if (!r) return NULL;
    r->raw = raw;
    r->on_state = fn;
    r->user = user;
    make_state(r, &r->shown);
    fn(&r->shown, user);    /* what's held right now, before any event */
    return r;
}

PadReader *pad_reader_activity(const char *node, PadUsedFn fn, gpointer user)
{
    PadReader *r = open_reader(node);
    if (!r) return NULL;
    r->activity = TRUE;
    r->on_used = fn;
    r->user = user;
    return r;
}

void pad_reader_on_gone(PadReader *r, PadGoneFn fn, gpointer user)
{
    r->on_gone = fn;
    r->gone_user = user;
}

void pad_reader_free(PadReader *r)
{
    if (!r) return;
    if (r->watch) g_source_remove(r->watch);
    if (r->timer) g_source_remove(r->timer);
    close(r->fd);
    g_free(r);
}
