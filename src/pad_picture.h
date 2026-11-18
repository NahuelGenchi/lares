// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gtk/gtk.h>
#include "pad/padinput.h"

/* The controller picture: pixel line art of an Xbox controller, drawn from
 * data/pad/controller.txt in the text color, with every pressed button lit
 * in the accent color, the sticks' caps moving and the trigger gauges
 * filling. Buttons the button test has seen stay softly lit. Without a
 * controller it draws a dimmed outline. Traced from Padlight's picture. */

#define LARES_TYPE_PAD_PICTURE (lares_pad_picture_get_type())
G_DECLARE_FINAL_TYPE(LaresPadPicture, lares_pad_picture, LARES, PAD_PICTURE, GtkWidget)

GtkWidget *lares_pad_picture_new(gboolean placeholder);
void lares_pad_picture_set_state(LaresPadPicture *self, const PadState *state);
void lares_pad_picture_set_seen(LaresPadPicture *self, guint32 seen);
/* Rattles the picture for about as long as a vibration test lasts. */
void lares_pad_picture_shake(LaresPadPicture *self);
