// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>
#include "../keylight.h"

/* The settings the background helper follows, in ~/.config/lares/pad.ini:
 * the controllers', and the keyboard lighting it plays. Lares and the
 * helper both read it, and each notices when the other writes. */

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
    /* The lighting the helper plays on a Logitech keyboard Lares knows, for
     * an effect the keyboard can't keep (key_light_live); KEY_LIGHT_OWN
     * leaves the keyboard to play what its profile keeps. */
    KeyLight key_light;
    guint8 key_rgb[3];
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
 * works while Lares is closed, such as a keyboard's live lighting. */
gboolean padconf_needs_helper(const PadPrefs *p);
