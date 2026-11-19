// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <glib.h>

/* A keyboard's lighting effects. KEY_LIGHT_OWN is the effect it came with,
 * such as the G715's factory one; KEY_LIGHT_OTHER one Lares doesn't know,
 * such as an effect another app saved. Shared by the window and the helper,
 * which plays some of them live (src/hidpp.c). */
typedef enum {
    KEY_LIGHT_OFF, KEY_LIGHT_SOLID, KEY_LIGHT_BREATHING, KEY_LIGHT_CYCLE, KEY_LIGHT_WAVE, KEY_LIGHT_RIPPLE,
    KEY_LIGHT_OWN, KEY_LIGHT_OTHER,
} KeyLight;

/* Whether a keyboard's light has a colour of its own to pick. */
static inline gboolean key_light_coloured(KeyLight light)
{
    return light == KEY_LIGHT_SOLID || light == KEY_LIGHT_BREATHING || light == KEY_LIGHT_RIPPLE;
}

/* Whether the helper plays it live, as the keyboard can't keep it. */
static inline gboolean key_light_live(KeyLight light)
{
    return light == KEY_LIGHT_OFF || light == KEY_LIGHT_SOLID || light == KEY_LIGHT_BREATHING
        || light == KEY_LIGHT_CYCLE;
}
