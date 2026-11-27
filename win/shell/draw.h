// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef LARES_SHELL_DRAW_H
#define LARES_SHELL_DRAW_H

/* The shell's drawing layer (issue #141), on the engine chosen in #140
 * (docs/SHELL.md): GDI+ draws shapes and pictures into a 32-bit bitmap in
 * memory, DirectWrite lays out text and turns it into ClearType coverage,
 * and draw.c blends that coverage into the bitmap itself, also where the
 * bitmap is see-through. No Direct2D, no Direct3D. */

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include "gdiplus_c.h"

/* The shell's colours, from Windows' light or dark mode and accent colour. */
typedef struct {
    bool is_dark;
    ARGB accent;
    ARGB accent_hover;
    ARGB accent_overlay_hover;
    ARGB accent_overlay_pressed;
    ARGB accent_overlay_selected;

    ARGB topbar_bg;     /* see-through */
    ARGB dock_bg;       /* see-through */
    ARGB tile_bg;
    ARGB tile_bg_hover;
    ARGB popup_bg;      /* nearly solid surface of a popup */
    ARGB popup_surface; /* a card inside a popup */
    ARGB popup_border;  /* 1 px border */
    ARGB fg_primary;
    ARGB fg_secondary;
    ARGB fg_muted;
    ARGB fg_on_accent;  /* text on the accent colour */
    ARGB badge_bg;      /* the count of a dock app's windows */
    ARGB badge_fg;
    ARGB dot_open;      /* an app that runs but is not in front */
    ARGB separator;
    ARGB danger;
    ARGB success;

    /* Files and the viewers: opaque windows. */
    COLORREF files_sidebar;
    COLORREF files_toolbar;
    COLORREF files_addrbar;
    COLORREF files_bg;
    COLORREF files_alt;
    COLORREF files_info_bg;
    COLORREF files_border;
    COLORREF files_text;
    COLORREF files_text_muted;
    COLORREF files_sel_bg;
    COLORREF files_hover_bg;
} ThemeColors;

/* A bitmap in memory with GDI+ on it. See-through ones hold premultiplied
 * colour and go on screen with UpdateLayeredWindow; opaque ones are copied
 * into a window's paint. */
typedef struct {
    HDC hdc;
    HBITMAP hbmp;
    HBITMAP old_bmp;
    void *bits;
    int width;
    int height;
    GpGraphics *g;
    bool is_opaque;
} LayerSurface;

extern ThemeColors g_theme;

bool render_init(void);
void render_shutdown(void);

/* Reads light or dark mode and the accent colour from Windows. */
void theme_reload(void);
/* Sets the palette from a given mode and accent colour (also for tests). */
void theme_apply(bool dark, COLORREF accent);

bool surface_ensure(LayerSurface *s, int w, int h);
void surface_destroy(LayerSurface *s);
void surface_present(HWND hwnd, LayerSurface *s, int x, int y, BYTE alpha);
bool opaque_frame_begin(LayerSurface *s, int w, int h);
void opaque_frame_end(HDC target_hdc, LayerSurface *s);
/* Writes the surface as a PNG file (for checks); nonzero when written. */
int surface_save_png(LayerSurface *s, const wchar_t *path);

void draw_round_rect_fill(GpGraphics *g, float x, float y, float w, float h, float r, ARGB color);
void draw_round_rect_stroke(GpGraphics *g, float x, float y, float w, float h, float r, ARGB color, float width);
void draw_circle_fill(GpGraphics *g, float cx, float cy, float r, ARGB color);
void draw_line(GpGraphics *g, float x1, float y1, float x2, float y2, ARGB color, float width);

/* One line of text in a box: halign and valign are GP_ALIGN_NEAR, _CENTER
 * or _FAR; style is GP_FONT_REGULAR, _BOLD (semibold) or _ITALIC; font_name
 * NULL is the shell's own face. */
void draw_text(GpGraphics *g, const wchar_t *text, const wchar_t *font_name, float size, int style,
               ARGB color, float x, float y, float w, float h, int halign, int valign, bool ellipsis);
float measure_text_width(GpGraphics *g, const wchar_t *text, const wchar_t *font_name, float size, int style);
/* A word-wrapped paragraph; returns its height, and draws only when draw is true. */
float draw_text_wrapped(GpGraphics *g, const wchar_t *text, int len, const wchar_t *font_name, float size,
                        int style, ARGB color, float x, float y, float w, float h, bool draw);
/* One symbol from Windows' own icon font (Segoe Fluent Icons, else Segoe MDL2 Assets). */
void draw_icon_glyph(GpGraphics *g, wchar_t glyph, float size, ARGB color, float x, float y, float w, float h);
/* A GDI font for Windows' own edit controls, in the shell's face. */
HFONT render_create_ui_font(int px_height, bool semibold);
/* The face text is drawn in when font_name is NULL. */
const wchar_t *render_ui_face(void);

/* Ease-out cubic: fast start, soft landing; t in 0..1. */
float anim_ease_out(float t);

/* The sharing symbol: three rings open at the bottom around a small core; r is the outer radius. */
void draw_share_icon(GpGraphics *g, float cx, float cy, float r, ARGB color, float stroke);

#endif /* LARES_SHELL_DRAW_H */
