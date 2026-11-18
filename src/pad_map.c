// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_map.h"
#include <gio/gio.h>
#include <string.h>

/* The legend, as in the map's header. */
static gboolean decode(char c, guint8 *cell)
{
    static const struct { char c; PadCellKind kind; PadButton button; } LEGEND[] = {
        { '.', PAD_CELL_EMPTY, 0 }, { '#', PAD_CELL_LINE, 0 }, { '+', PAD_CELL_FAINT, 0 },
        { '=', PAD_CELL_GAUGE, 0 },
        { 'a', PAD_CELL_AREA, PB_A }, { 'b', PAD_CELL_AREA, PB_B }, { 'x', PAD_CELL_AREA, PB_X },
        { 'y', PAD_CELL_AREA, PB_Y }, { 'v', PAD_CELL_AREA, PB_VIEW }, { 'm', PAD_CELL_AREA, PB_MENU },
        { 's', PAD_CELL_AREA, PB_SHARE },
        { 'A', PAD_CELL_GLYPH, PB_A }, { 'B', PAD_CELL_GLYPH, PB_B }, { 'X', PAD_CELL_GLYPH, PB_X },
        { 'Y', PAD_CELL_GLYPH, PB_Y }, { 'V', PAD_CELL_GLYPH, PB_VIEW }, { 'M', PAD_CELL_GLYPH, PB_MENU },
        { 'S', PAD_CELL_GLYPH, PB_SHARE }, { 'H', PAD_CELL_GLYPH, PB_XBOX },
        { '[', PAD_CELL_AREA, PB_LB }, { ']', PAD_CELL_AREA, PB_RB },
        { '(', PAD_CELL_AREA, PB_LT }, { ')', PAD_CELL_AREA, PB_RT },
        { 'u', PAD_CELL_AREA, PB_UP }, { 'd', PAD_CELL_AREA, PB_DOWN },
        { 'l', PAD_CELL_AREA, PB_LEFT }, { 'r', PAD_CELL_AREA, PB_RIGHT },
        { '<', PAD_CELL_CAP_LINE, PB_LS }, { '>', PAD_CELL_CAP_LINE, PB_RS },
        { '{', PAD_CELL_CAP_AREA, PB_LS }, { '}', PAD_CELL_CAP_AREA, PB_RS },
    };
    for (guint i = 0; i < G_N_ELEMENTS(LEGEND); i++) {
        if (LEGEND[i].c != c) continue;
        *cell = PAD_CELL(LEGEND[i].kind, LEGEND[i].button);
        return TRUE;
    }
    return FALSE;
}

PadMap *pad_map_parse(const char *text, gsize length, GError **error)
{
    GByteArray *cells = g_byte_array_new();
    int width = 0, height = 0;
    const char *p = text, *end = text + length;
    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);
        if (!eol) eol = end;
        int len = (int)(eol - p);
        if (len && p[len - 1] == '\r') len--;
        if (len && p[0] != ';') {
            if (width && len != width) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "row %d is %d pixels long, not %d", height + 1, len, width);
                g_byte_array_free(cells, TRUE);
                return NULL;
            }
            width = len;
            for (int i = 0; i < len; i++) {
                guint8 cell;
                if (!decode(p[i], &cell)) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "row %d has '%c', which the legend doesn't know", height + 1, p[i]);
                    g_byte_array_free(cells, TRUE);
                    return NULL;
                }
                g_byte_array_append(cells, &cell, 1);
            }
            height++;
        }
        p = eol + 1;
    }
    if (!height) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the map has no rows");
        g_byte_array_free(cells, TRUE);
        return NULL;
    }
    PadMap *map = g_new(PadMap, 1);
    map->width = width;
    map->height = height;
    map->cells = g_byte_array_free(cells, FALSE);
    return map;
}

void pad_map_free(PadMap *map)
{
    if (!map) return;
    g_free(map->cells);
    g_free(map);
}

PadPaint pad_cell_paint(guint8 cell, guint32 held, guint32 seen)
{
    guint32 bit = PB_BIT(pad_cell_button(cell));
    switch (pad_cell_kind(cell)) {
    case PAD_CELL_LINE:
    case PAD_CELL_GAUGE:
    case PAD_CELL_CAP_LINE:
        return PAD_PAINT_LINE;
    case PAD_CELL_FAINT:
        return PAD_PAINT_FAINT;
    case PAD_CELL_AREA:
    case PAD_CELL_CAP_AREA:
        if (held & bit) return PAD_PAINT_LIT;
        return seen & bit ? PAD_PAINT_SEEN : PAD_PAINT_NONE;
    case PAD_CELL_GLYPH:
        /* The Xbox logo is its own button and lights itself; a letter or icon
         * is cut out of its lit button instead. */
        if (pad_cell_button(cell) == PB_XBOX) {
            if (held & bit) return PAD_PAINT_LIT;
            return seen & bit ? PAD_PAINT_SEEN : PAD_PAINT_LINE;
        }
        return held & bit ? PAD_PAINT_NONE : PAD_PAINT_LINE;
    default:
        return PAD_PAINT_NONE;
    }
}
