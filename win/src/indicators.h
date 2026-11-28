// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's device indicators (issue #157): what Lares' own pages would
 * show, read the way they read it, so the bar can point at them. Reading
 * only; nothing here changes a device. */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LARES_PAD_BATTERY_UNKNOWN = -1, LARES_PAD_BATTERY_EMPTY = 0, LARES_PAD_BATTERY_LOW,
       LARES_PAD_BATTERY_MEDIUM, LARES_PAD_BATTERY_FULL };

typedef struct {
    bool controller;         /* an Xbox controller, through XInput */
    int  controller_battery; /* the lowest of them; unknown when wired */
    bool headphones;         /* Bluetooth headphones or speakers, connected */
    bool keyboard;           /* a keyboard Lares can light */
    bool mouse;              /* a mouse Lares can set up */
    bool remote;             /* Remote mode, as Lares saved it */
} LaresIndicators;

/* Looks at the devices and Lares' saved mode; blocking, so the shell calls
 * it on a worker thread. A dry run pretends every device is there. */
void lares_indicators_scan(LaresIndicators *out);

#ifdef __cplusplus
}
#endif
