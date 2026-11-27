// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The shell's light and dark palettes, picked from Windows' own mode and
 * accent colour (issue #141). */
#include "draw.h"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

ThemeColors g_theme;

static ARGB rgb_with_alpha(BYTE a, COLORREF c) {
    return MAKE_ARGB(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

/* Dark unless Windows says its own surfaces are light. */
static bool read_system_is_dark(void) {
    HKEY hKey = NULL;
    DWORD val = 0;
    DWORD cb = sizeof(val);
    bool is_dark = true;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, NULL, (LPBYTE)&val, &cb) == ERROR_SUCCESS) {
            is_dark = (val == 0);
        } else {
            cb = sizeof(val);
            if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &cb) == ERROR_SUCCESS) {
                is_dark = (val == 0);
            }
        }
        RegCloseKey(hKey);
    }
    return is_dark;
}

/* Windows' accent colour, as RGB. */
static bool read_windows_accent_rgb(COLORREF *out) {
    HKEY hKey = NULL;
    DWORD abgr = 0;
    DWORD cb = sizeof(abgr);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st = RegQueryValueExW(hKey, L"AccentColor", NULL, NULL, (LPBYTE)&abgr, &cb);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return false;
    *out = RGB((BYTE)(abgr & 0xFF), (BYTE)((abgr >> 8) & 0xFF), (BYTE)((abgr >> 16) & 0xFF));
    return true;
}

static void apply_colors(bool dark, COLORREF acc) {
    g_theme.is_dark = dark;
    g_theme.accent = rgb_with_alpha(255, acc);
    g_theme.accent_hover = MAKE_ARGB(255,
        (BYTE)(dark ? min(255, GetRValue(acc) + 18) : max(0, (int)GetRValue(acc) - 18)),
        (BYTE)(dark ? min(255, GetGValue(acc) + 18) : max(0, (int)GetGValue(acc) - 18)),
        (BYTE)(dark ? min(255, GetBValue(acc) + 18) : max(0, (int)GetBValue(acc) - 18)));
    g_theme.accent_overlay_hover    = rgb_with_alpha(52, acc);
    g_theme.accent_overlay_pressed  = rgb_with_alpha(78, acc);
    g_theme.accent_overlay_selected = rgb_with_alpha(30, acc);

    if (dark) {
        g_theme.topbar_bg     = MAKE_ARGB(232, 26, 26, 28);
        g_theme.dock_bg       = MAKE_ARGB(208, 26, 26, 28);
        g_theme.tile_bg       = MAKE_ARGB(235, 38, 38, 42);
        g_theme.tile_bg_hover = MAKE_ARGB(245, 52, 52, 58);
        g_theme.popup_bg      = MAKE_ARGB(244, 28, 28, 32);
        g_theme.popup_surface = MAKE_ARGB(255, 40, 40, 46);
        g_theme.popup_border  = MAKE_ARGB(95, 255, 255, 255);
        g_theme.fg_primary    = MAKE_ARGB(255, 232, 232, 236);
        g_theme.fg_secondary  = MAKE_ARGB(255, 190, 190, 198);
        g_theme.fg_muted      = MAKE_ARGB(255, 140, 140, 150);
        g_theme.fg_on_accent  = MAKE_ARGB(255, 18, 18, 20);
        g_theme.badge_bg      = MAKE_ARGB(255, 118, 118, 126);
        g_theme.badge_fg      = MAKE_ARGB(255, 24, 24, 28);
        g_theme.dot_open      = MAKE_ARGB(255, 168, 168, 176);
        g_theme.separator     = MAKE_ARGB(110, 160, 160, 170);
        g_theme.danger        = MAKE_ARGB(255, 238, 92, 92);
        g_theme.success       = MAKE_ARGB(255, 86, 204, 124);

        g_theme.files_sidebar    = RGB(0x22, 0x1D, 0x28);
        g_theme.files_toolbar    = RGB(0x2B, 0x27, 0x31);
        g_theme.files_addrbar    = RGB(0x38, 0x34, 0x3C);
        g_theme.files_bg         = RGB(0x1E, 0x1B, 0x24);
        g_theme.files_alt        = RGB(0x26, 0x22, 0x2D);
        g_theme.files_info_bg    = RGB(0x26, 0x22, 0x2D);
        g_theme.files_border     = RGB(0x3E, 0x38, 0x48);
        g_theme.files_text       = RGB(0xEC, 0xEA, 0xF0);
        g_theme.files_text_muted = RGB(0xA0, 0x9A, 0xAE);
        g_theme.files_sel_bg     = RGB(0x3B, 0x57, 0x86);
        g_theme.files_hover_bg   = RGB(0x32, 0x2D, 0x3A);
    } else {
        g_theme.topbar_bg     = MAKE_ARGB(232, 243, 243, 246);
        g_theme.dock_bg       = MAKE_ARGB(208, 243, 243, 246);
        g_theme.tile_bg       = MAKE_ARGB(242, 252, 252, 254);
        g_theme.tile_bg_hover = MAKE_ARGB(250, 234, 234, 240);
        g_theme.popup_bg      = MAKE_ARGB(246, 246, 246, 249);
        g_theme.popup_surface = MAKE_ARGB(255, 255, 255, 255);
        g_theme.popup_border  = MAKE_ARGB(60, 0, 0, 0);
        g_theme.fg_primary    = MAKE_ARGB(255, 26, 26, 30);
        g_theme.fg_secondary  = MAKE_ARGB(255, 76, 76, 84);
        g_theme.fg_muted      = MAKE_ARGB(255, 120, 120, 130);
        g_theme.fg_on_accent  = MAKE_ARGB(255, 255, 255, 255);
        g_theme.badge_bg      = MAKE_ARGB(255, 138, 138, 148);
        g_theme.badge_fg      = MAKE_ARGB(255, 255, 255, 255);
        g_theme.dot_open      = MAKE_ARGB(255, 108, 108, 118);
        g_theme.separator     = MAKE_ARGB(95, 100, 100, 110);
        g_theme.danger        = MAKE_ARGB(255, 210, 48, 48);
        g_theme.success       = MAKE_ARGB(255, 28, 148, 72);

        g_theme.files_sidebar    = RGB(0xEC, 0xEC, 0xEC);
        g_theme.files_toolbar    = RGB(0xF5, 0xF5, 0xF7);
        g_theme.files_addrbar    = RGB(0xE8, 0xE8, 0xED);
        g_theme.files_bg         = RGB(0xFF, 0xFF, 0xFF);
        g_theme.files_alt        = RGB(0xF5, 0xF5, 0xF7);
        g_theme.files_info_bg    = RGB(0xF8, 0xF8, 0xFA);
        g_theme.files_border     = RGB(0xD8, 0xD8, 0xDF);
        g_theme.files_text       = RGB(0x1C, 0x1C, 0x20);
        g_theme.files_text_muted = RGB(0x6E, 0x6E, 0x78);
        g_theme.files_sel_bg     = RGB(0xCC, 0xE2, 0xFF);
        g_theme.files_hover_bg   = RGB(0xEB, 0xEB, 0xF0);
    }
}

void theme_apply(bool dark, COLORREF acc) {
    /* On dark surfaces the accent is lifted a quarter of the way to white. */
    if (dark) {
        acc = RGB((GetRValue(acc) * 3 + 255) / 4, (GetGValue(acc) * 3 + 255) / 4, (GetBValue(acc) * 3 + 255) / 4);
    }
    apply_colors(dark, acc);
}

void theme_reload(void) {
    bool dark = read_system_is_dark();
    COLORREF acc = 0;
    if (read_windows_accent_rgb(&acc)) {
        theme_apply(dark, acc);
    } else {
        /* No accent colour set: Windows' own blue, lighter on dark. */
        apply_colors(dark, dark ? RGB(96, 180, 255) : RGB(0, 103, 192));
    }
}
