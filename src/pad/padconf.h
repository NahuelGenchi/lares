// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>

/* The controller settings, in ~/.config/lares/pad.ini. Lares and its
 * background helper both read it, and each notices when the other writes. */

#define PAD_IDLE_CHOICES 4
extern const int PAD_IDLE_MINUTES[PAD_IDLE_CHOICES];   /* 5, 10, 15, 30 */

typedef struct {
    int low_level;              /* battery percent that counts as low, 5 to 50 */
    gboolean alerts;            /* low battery notifications */
    gboolean notices;           /* connect and disconnect notifications */
    gboolean keep_awake;        /* controller input keeps the screen on */
    gboolean idle_disconnect;   /* Bluetooth controllers left idle disconnect */
    int idle_minutes;
    gboolean topbar;            /* the top-bar addon, off until allowed */
    gboolean topbar_level;      /* battery level beside the top-bar icon */
    gboolean topbar_hide;       /* no top-bar icon without a controller */
    gboolean background;        /* the helper may run; off stops all of the above */
    char *salt;                 /* for nickname keys, made when first needed */
    GHashTable *nicknames;      /* key -> "nickname\tmodel name" */
} PadPrefs;

typedef struct PadConf PadConf;
typedef void (*PadConfNotify)(gpointer user);

PadConf *padconf_new(void);
PadPrefs *padconf_prefs(PadConf *c);
void padconf_save(PadConf *c);
/* Runs `fn` after the file changes on disk, whoever changed it. */
void padconf_watch(PadConf *c, PadConfNotify fn, gpointer user);

/* Whether anything needs the helper running: the addon, or a feature that
 * works while Lares is closed. */
gboolean padconf_needs_helper(const PadPrefs *p);
