// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gtk/gtk.h>
#include "pad/padinput.h"

/* Padlight's controller picture: its artwork and glow, with every pressed
 * button lit green, the sticks moving and the triggers filling. Buttons the
 * button test has seen stay softly lit. Without a controller it draws a
 * dimmed outline. Ported from Padlight's controllerArt.js and
 * controllerView.js. */

#define LARES_TYPE_PAD_PICTURE (lares_pad_picture_get_type())
G_DECLARE_FINAL_TYPE(LaresPadPicture, lares_pad_picture, LARES, PAD_PICTURE, GtkWidget)

GtkWidget *lares_pad_picture_new(int height, gboolean placeholder);
void lares_pad_picture_set_state(LaresPadPicture *self, const PadState *state);
void lares_pad_picture_set_seen(LaresPadPicture *self, guint32 seen);
/* Rattles the picture for about as long as a vibration test lasts. */
void lares_pad_picture_shake(LaresPadPicture *self);
