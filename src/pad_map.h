// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <glib.h>
#include "pad/pads.h"

/* The controller picture's pixel map, data/pad/controller.txt: one character
 * per pixel, saying what that pixel is. tools/pad-map.py draws it; the file's
 * own header has the legend. GLib only, so the unit tests can read it. */

typedef enum {
    PAD_CELL_EMPTY,
    PAD_CELL_LINE,
    PAD_CELL_FAINT,     /* a line drawn faint: the D-pad's middle */
    PAD_CELL_GAUGE,     /* a trigger gauge's outline */
    PAD_CELL_AREA,      /* lights when its button is pressed */
    PAD_CELL_GLYPH,     /* a button's letter, icon or logo */
    PAD_CELL_CAP_LINE,  /* a stick's cap, which moves with the stick */
    PAD_CELL_CAP_AREA,  /* the inside of a cap, lit when the stick is clicked */
} PadCellKind;

/* How far a stick's cap travels at full tilt, in pixels. The map leaves this
 * much room between each cap and its rim. */
#define PAD_MAP_STICK_TRAVEL 3

/* A cell packs its kind with the button it belongs to. */
#define PAD_CELL(kind, button) ((guint8)((kind) << 5 | (button)))
static inline PadCellKind pad_cell_kind(guint8 cell) { return cell >> 5; }
static inline PadButton pad_cell_button(guint8 cell) { return cell & 31; }

typedef struct {
    int width, height;
    guint8 *cells;      /* width * height, row by row */
} PadMap;

/* Lines starting with ';' are comments. Every other line is a row of the
 * picture; all rows have the same length. NULL and an error otherwise. */
PadMap *pad_map_parse(const char *text, gsize length, GError **error);
void pad_map_free(PadMap *map);

static inline guint8 pad_map_at(const PadMap *map, int x, int y)
{
    if (x < 0 || y < 0 || x >= map->width || y >= map->height) return PAD_CELL(PAD_CELL_EMPTY, 0);
    return map->cells[y * map->width + x];
}

typedef enum {
    PAD_PAINT_NONE,     /* the background shows */
    PAD_PAINT_LINE,     /* the text color */
    PAD_PAINT_FAINT,    /* the text color, faint */
    PAD_PAINT_LIT,      /* the accent color: held now */
    PAD_PAINT_SEEN,     /* the accent color, soft: the button test has seen it */
} PadPaint;

/* How a pixel shows, given the buttons held (PB_BIT) and the ones the button
 * test has seen. A trigger gauge counts as held as far as it is pulled. */
PadPaint pad_cell_paint(guint8 cell, guint32 held, guint32 seen);
