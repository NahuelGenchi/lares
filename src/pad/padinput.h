// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <glib.h>

/* Reads a controller's input straight from its evdev node, without taking it
 * from games: every reader gets its own copy of the events. Ported from
 * Padlight's monitor.py, which ran as a Python process per reader. */

typedef struct {
    guint32 buttons;           /* PB_BIT of each button held now */
    float lx, ly, rx, ry;      /* sticks, -1 to 1 */
    float lt, rt;              /* triggers, 0 to 1 */
} PadState;

typedef struct PadReader PadReader;
typedef void (*PadStateFn)(const PadState *state, gpointer user);
typedef void (*PadUsedFn)(gpointer user);
typedef void (*PadGoneFn)(gpointer user);

/* Live state, at most 60 times a second and only when it changes. `raw`
 * keeps the driver's dead zone in and gives finer steps, for the stick
 * check. */
PadReader *pad_reader_live(const char *node, gboolean raw, PadStateFn fn, gpointer user);

/* Only that the controller is in use, at most every five seconds: a button,
 * or a stick or trigger moved at least a quarter of its way. A stick that
 * drifts less doesn't count. */
PadReader *pad_reader_activity(const char *node, PadUsedFn fn, gpointer user);

void pad_reader_on_gone(PadReader *r, PadGoneFn fn, gpointer user);
void pad_reader_free(PadReader *r);
