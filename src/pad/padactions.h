// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>
#include "pads.h"

/* What Lares can do to a controller. Ported from Padlight's rumble.py,
 * inuse.py and its bluetoothctl calls, all done in-process now. */

#define PAD_RUMBLE_STRENGTH 75
#define PAD_RUMBLE_MS       400

/* A rumble: the strong motor is the left one, the weak one the right. */
gboolean pad_rumble(const Pad *pad, int strong, int weak, int length_ms);

/* Find Controller: ten short, strong buzzes over six seconds. `done` runs
 * when they end or are stopped. */
typedef void (*PadFindDone)(gpointer user);
gboolean pad_find_start(const Pad *pad, PadFindDone done, gpointer user);
void pad_find_stop(const char *pad_path);
gboolean pad_finding(const char *pad_path);

/* Disconnects a Bluetooth controller through BlueZ. It stays paired. */
void pad_disconnect(const Pad *pad);

/* Which programs have the controller's device files open: names to show,
 * worked out in a thread. `skip_pids` leaves out Lares and its helper. */
typedef void (*PadUsersFn)(const char *pad_path, GPtrArray *names, gpointer user);
void pad_find_users(const Pad *pad, PadUsersFn fn, gpointer user);
