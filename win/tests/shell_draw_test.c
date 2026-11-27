/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * lares-shell-draw-test: the shell's drawing layer (win/shell/draw.c) draws
 * into bitmaps in memory, and this reads back what it drew (issue #141).
 * Nothing is shown on screen. Every DirectWrite call draw.c makes goes
 * through the C declarations in dwrite_c.h on a real factory, so a slot out
 * of place shows up here as text that is not drawn or measured.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"

static int g_failures, g_checks;

static void check(int ok, const char *what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    printf("  FAILED: %s\n", what);
}

static const BYTE *px(const LayerSurface *s, int x, int y)
{
    return (const BYTE *)s->bits + ((size_t)y * (size_t)s->width + (size_t)x) * 4;
}

/* Pixels in a box whose colour differs between channels by more than d:
 * ClearType's coloured edges. */
static int fringed(const LayerSurface *s, int x0, int y0, int x1, int y1, int d)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const BYTE *p = px(s, x, y);
            if (abs((int)p[0] - (int)p[2]) > d) ++n;
        }
    return n;
}

static int brighter_than(const LayerSurface *s, int x0, int y0, int x1, int y1, int level)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (px(s, x, y)[1] > level) ++n;
    return n;
}

static int darker_than(const LayerSurface *s, int x0, int y0, int x1, int y1, int level)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (px(s, x, y)[1] < level) ++n;
    return n;
}

static void test_palette(void)
{
    theme_apply(true, RGB(0, 120, 212));
    check(g_theme.is_dark, "dark palette says dark");
    check(((g_theme.topbar_bg >> 24) & 0xff) == 232, "the dark bar is see-through");
    /* The accent is lifted a quarter of the way to white on dark. */
    check(((g_theme.accent >> 16) & 0xff) == (0 * 3 + 255) / 4 && ((g_theme.accent >> 8) & 0xff) == (120 * 3 + 255) / 4,
          "dark accent lifted");
    check(g_theme.files_bg == RGB(0x1E, 0x1B, 0x24), "dark Files background");

    theme_apply(false, RGB(0, 120, 212));
    check(!g_theme.is_dark, "light palette says light");
    check(g_theme.accent == MAKE_ARGB(255, 0, 120, 212), "light accent as Windows gives it");
    check(g_theme.files_bg == RGB(0xFF, 0xFF, 0xFF), "light Files background");

    theme_reload();
    check(((g_theme.accent >> 24) & 0xff) == 255, "palette read from Windows");
}

static void test_see_through(void)
{
    LayerSurface s;
    ZeroMemory(&s, sizeof(s));
    check(surface_ensure(&s, 240, 64), "see-through surface made");
    if (!s.bits) return;
    memset(s.bits, 0, (size_t)s.width * s.height * 4);

    draw_round_rect_fill(s.g, 10.0f, 8.0f, 220.0f, 48.0f, 12.0f, MAKE_ARGB(255, 30, 30, 34));
    GdipFlush(s.g, GP_FLUSH_INTENTION_SYNC);
    const BYTE *mid = px(&s, 120, 12);
    check(mid[3] == 255 && mid[2] == 30 && mid[1] == 30 && mid[0] == 34, "filled inside, as asked");
    check(px(&s, 3, 3)[3] == 0, "nothing outside the shape");
    check(px(&s, 10, 8)[3] < 128, "the corner is rounded");

    draw_text(s.g, L"Lares 27 Nov", NULL, 18.0f, GP_FONT_BOLD, MAKE_ARGB(255, 240, 240, 240),
              20.0f, 8.0f, 200.0f, 48.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
    check(brighter_than(&s, 20, 8, 220, 56, 160) > 40, "light text on the dark shape");
    check(fringed(&s, 20, 8, 220, 56, 10) > 10, "ClearType edges on a see-through surface");
    check(px(&s, 3, 3)[3] == 0, "text leaves the outside alone");

    /* Text on the empty, fully see-through part takes coverage as alpha. */
    memset(s.bits, 0, (size_t)s.width * s.height * 4);
    draw_text(s.g, L"Lares", NULL, 18.0f, GP_FONT_REGULAR, MAKE_ARGB(255, 255, 255, 255),
              0.0f, 0.0f, 240.0f, 64.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    int covered = 0, premultiplied = 1;
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x) {
            const BYTE *p = px(&s, x, y);
            if (p[3]) ++covered;
            if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) premultiplied = 0;
        }
    check(covered > 30, "text on nothing gains alpha");
    check(premultiplied, "colour stays premultiplied");

    surface_destroy(&s);
    check(s.bits == NULL && s.hdc == NULL, "surface released");
}

static const wchar_t kPara[] = L"Tailscale keeps this PC reachable from your own devices.";

static void test_opaque(void)
{
    LayerSurface s;
    ZeroMemory(&s, sizeof(s));
    check(opaque_frame_begin(&s, 300, 80), "opaque surface made");
    if (!s.bits) return;
    GdipGraphicsClear(s.g, MAKE_ARGB(255, 255, 255, 255));
    GdipFlush(s.g, GP_FLUSH_INTENTION_SYNC);
    check(darker_than(&s, 0, 0, 300, 80, 250) == 0, "white all over");

    /* Clipped: text in a 60 px box leaves the rest white. */
    draw_text(s.g, L"Meeting notes, project plan and budget.md", NULL, 14.0f, GP_FONT_REGULAR,
              MAKE_ARGB(255, 20, 20, 24), 10.0f, 0.0f, 60.0f, 40.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
    check(darker_than(&s, 10, 0, 70, 40, 128) > 20, "dark text on white");
    check(darker_than(&s, 72, 0, 300, 40, 250) == 0, "nothing past the box");
    check(fringed(&s, 10, 0, 70, 40, 10) > 10, "ClearType edges on an opaque surface");
    int opaque = 1;
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x)
            if (px(&s, x, y)[3] != 255) opaque = 0;
    check(opaque, "opaque stays opaque");

    float h_wide = draw_text_wrapped(s.g, kPara, (int)wcslen(kPara),
                                     NULL, 12.0f, GP_FONT_REGULAR, MAKE_ARGB(255, 0, 0, 0), 0.0f, 44.0f, 290.0f,
                                     0.0f, true);
    float h_narrow = draw_text_wrapped(s.g, kPara, (int)wcslen(kPara),
                                       NULL, 12.0f, GP_FONT_REGULAR, MAKE_ARGB(255, 0, 0, 0), 0.0f, 0.0f, 70.0f,
                                       0.0f, false);
    check(h_wide > 8.0f, "a wrapped paragraph has a height");
    check(h_narrow > h_wide * 2.0f, "narrower wraps into more lines");
    draw_icon_glyph(s.g, 0xE713, 16.0f, MAKE_ARGB(255, 0, 0, 0), 270.0f, 50.0f, 24.0f, 24.0f);

    /* The frame is copied out and released. */
    HDC screen = GetDC(NULL);
    HDC target = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 300, 80);
    HGDIOBJ old = SelectObject(target, bmp);
    opaque_frame_end(target, &s);
    check(s.bits == NULL, "opaque frame released after the copy");
    COLORREF c = GetPixel(target, 290, 5);
    check(GetRValue(c) > 240 && GetGValue(c) > 240, "the copy holds the frame");
    SelectObject(target, old);
    DeleteObject(bmp);
    DeleteDC(target);
    ReleaseDC(NULL, screen);
}

static void test_measure(void)
{
    float one = measure_text_width(NULL, L"Lares", NULL, 14.0f, GP_FONT_REGULAR);
    float two = measure_text_width(NULL, L"Lares Lares", NULL, 14.0f, GP_FONT_REGULAR);
    float bold = measure_text_width(NULL, L"Lares", NULL, 14.0f, GP_FONT_BOLD);
    float big = measure_text_width(NULL, L"Lares", NULL, 28.0f, GP_FONT_REGULAR);
    check(one > 20.0f && one < 80.0f, "a word has a sensible width");
    check(two > one * 1.8f, "two words are about twice as wide");
    check(bold >= one, "semibold is no narrower");
    check(big > one * 1.7f, "twice the size is about twice as wide");
    check(measure_text_width(NULL, L"", NULL, 14.0f, GP_FONT_REGULAR) == 0.0f, "nothing is zero wide");
    check(render_ui_face()[0] != 0, "a face was picked");

    /* Many sizes and styles: the cache of text formats fills and recycles. */
    float last = 0.0f;
    for (int i = 0; i < 100; ++i) last = measure_text_width(NULL, L"x", NULL, 8.0f + (float)i * 0.5f, i & 1);
    check(last > 0.0f, "format cache recycles");

    HFONT f = render_create_ui_font(14, true);
    check(f != NULL, "a font for edit controls");
    if (f) DeleteObject(f);
    check(anim_ease_out(0.0f) == 0.0f && anim_ease_out(1.0f) == 1.0f && anim_ease_out(0.5f) > 0.5f, "easing");
}

int wmain(void)
{
    check(render_init(), "drawing layer starts");
    test_palette();
    test_see_through();
    test_opaque();
    test_measure();
    render_shutdown();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
