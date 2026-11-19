// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../pad/padconf.h"

/* The keyboard lighting the helper plays live: an effect a Logitech
 * keyboard can't keep in its onboard profile, as the settings ask. It starts
 * at once, and gives the lights back to the keyboard when freed. */
typedef struct LiveLight LiveLight;

LiveLight *livelight_new(PadConf *conf);
void livelight_free(LiveLight *l);
