// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>
#include "padconf.h"

/* Xbox controllers the kernel has right now, read from sysfs, with their
 * batteries from UPower and their nicknames. Ported from Padlight's
 * controllers.js, battery.js, firmware.js and nicknames.js. */

typedef enum { PAD_USB, PAD_BLUETOOTH, PAD_ADAPTER } PadConnection;

typedef enum {
    PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_VIEW, PB_MENU, PB_XBOX,
    PB_SHARE, PB_LS, PB_RS, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_COUNT
} PadButton;

#define PB_BIT(b) (1u << (b))

typedef struct {
    gboolean known;
    double percentage;
    guint32 state;     /* UPower: 1 charging, 4 fully charged */
    guint32 level;     /* UPower battery level, for pads that report words */
} PadBattery;

typedef struct {
    char *path;            /* sysfs input device: new on every connection */
    char *name;            /* "Xbox Wireless Controller" */
    char *nickname;        /* NULL without one */
    PadConnection connection;
    char *model, *firmware, *firmware_update, *driver;
    char *address, *serial;
    char *event_node, *js_node;
    GPtrArray *hidraw_nodes, *power_supplies;
    gboolean rumble;
    guint32 checklist;     /* PB_BIT of each button the button test asks for */
    gint64 since;          /* when it connected, seconds since the epoch */
    PadBattery battery;
} Pad;

typedef struct Pads Pads;
typedef void (*PadsNotify)(gpointer user);

Pads *pads_new(PadConf *conf);
GPtrArray *pads_list(Pads *p);           /* Pad*, in joystick order */
const Pad *pads_find(Pads *p, const char *path);
void pads_watch(Pads *p, PadsNotify fn, gpointer user);
void pads_set_nickname(Pads *p, const Pad *pad, const char *nickname);

const char *pad_name(const Pad *pad);    /* the nickname, or the model's name */
const char *pad_connection_name(PadConnection c);
gboolean pad_can_nickname(const Pad *pad);
char *pad_clean_nickname(const char *text);
char *pad_nickname_key(const Pad *pad, const char *salt);

gboolean pad_battery_charging(const PadBattery *b);
int pad_battery_low_stage(const PadBattery *b, int low_level);   /* 0, 1 low, 2 very low */
char *pad_battery_charge(const PadBattery *b);                   /* "85%" or "Low" */
const char *pad_battery_level_name(const PadBattery *b);         /* "Low", or NULL for a percent */
char *pad_battery_text(const PadBattery *b);                     /* with " · Charging" */
double pad_battery_fraction(const PadBattery *b);

#define PAD_VERY_LOW 10
#define PAD_NICKNAME_LENGTH 32
