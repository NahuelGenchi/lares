// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pad/padconf.h"

#define PAD_ADDON_UUID "lares@nahuelgenchi.github.io"

typedef enum {
    PAD_ADDON_NO_SHELL,     /* not on GNOME Shell, or a dry run */
    PAD_ADDON_NEEDS_LOGIN,  /* installed since log-in: the shell will load it at the next one */
    PAD_ADDON_OFF,
    PAD_ADDON_ON,
} PadAddonState;

typedef void (*PadAddonFn)(PadAddonState state, gpointer user);

/* Starts or stops the helper, and switches the addon, as the settings say. */
void pad_service_sync(const PadPrefs *prefs);
void pad_service_addon_state(PadAddonFn fn, gpointer user);
