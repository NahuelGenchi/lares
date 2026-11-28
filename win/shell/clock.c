// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar clock (issue #165): how the date and time read, and the
 * right-click menu that changes it.
 *
 * The text comes from a picture string (clockfmt.h), "ddd dd MMM, HH:mm" by
 * default, built from the menu's choices, or from one the user typed in
 * "Custom format...". The choices are kept in shell.ini's [clock] group. */
#include "shell.h"
#include <shellapi.h>

/* The picture the menu's choices describe, used when no custom one is set. */
static void build_picture(wchar_t *pic, size_t len) {
    static const wchar_t *dates[5] = { L"dd MMM", L"MMM d", L"dd/MM/yyyy", L"MM/dd/yyyy", L"yyyy-MM-dd" };
    int style = (g_cfg.clock_date_style >= 0 && g_cfg.clock_date_style <= 4) ? g_cfg.clock_date_style : 0;
    pic[0] = 0;
    if (g_cfg.clock_weekday) wcscat_s(pic, len, L"ddd");
    if (g_cfg.clock_date) {
        if (pic[0]) wcscat_s(pic, len, L" ");
        wcscat_s(pic, len, dates[style]);
    }
    if (pic[0]) wcscat_s(pic, len, L", ");
    wcscat_s(pic, len, g_cfg.clock_24h ? L"HH:mm" : L"h:mm");
    if (g_cfg.clock_seconds) wcscat_s(pic, len, L":ss");
    if (!g_cfg.clock_24h) wcscat_s(pic, len, L" tt");
}

static const wchar_t *active_picture(wchar_t *scratch, size_t len) {
    if (g_cfg.clock_format[0]) return g_cfg.clock_format;
    build_picture(scratch, len);
    return scratch;
}

void clock_format_now(wchar_t *out, size_t len) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t pic[64];
    clock_format_picture(active_picture(pic, 64), &st, out, len);
}

/* True when the text changes every second, so the bar repaints that often. */
bool clock_shows_seconds(void) {
    wchar_t pic[64];
    return clock_picture_has_seconds(active_picture(pic, 64)) != 0;
}

/* ---- "Custom format...": a small box built in memory, no resource file ---- */

#define IDC_CLK_EDIT    100
#define IDC_CLK_PREVIEW 101

typedef struct {
    WORD *p;
    DWORD buf[640];
} DlgTpl;

static void tpl_word(DlgTpl *t, WORD w) { *t->p++ = w; }
static void tpl_dword(DlgTpl *t, DWORD d) { tpl_word(t, LOWORD(d)); tpl_word(t, HIWORD(d)); }
static void tpl_str(DlgTpl *t, const wchar_t *s) { do { *t->p++ = (WORD)*s; } while (*s++); }
static void tpl_align(DlgTpl *t) { t->p = (WORD *)(((ULONG_PTR)t->p + 3) & ~(ULONG_PTR)3); }

static void tpl_item(DlgTpl *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls,
                     const wchar_t *text) {
    tpl_align(t);
    tpl_dword(t, style | WS_CHILD | WS_VISIBLE);
    tpl_dword(t, 0);
    tpl_word(t, (WORD)x);
    tpl_word(t, (WORD)y);
    tpl_word(t, (WORD)cx);
    tpl_word(t, (WORD)cy);
    tpl_word(t, id);
    tpl_word(t, 0xFFFF);
    tpl_word(t, cls);
    tpl_str(t, text);
    tpl_word(t, 0);
}

static void update_preview(HWND dlg) {
    wchar_t pic[64], text[96];
    GetDlgItemTextW(dlg, IDC_CLK_EDIT, pic, 64);
    if (!pic[0]) build_picture(pic, 64);
    SYSTEMTIME st;
    GetLocalTime(&st);
    clock_format_picture(pic, &st, text, 96);
    SetDlgItemTextW(dlg, IDC_CLK_PREVIEW, text);
}

static INT_PTR CALLBACK custom_dlg_proc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t pic[64];
        SendDlgItemMessageW(dlg, IDC_CLK_EDIT, EM_LIMITTEXT, 63, 0);
        SetDlgItemTextW(dlg, IDC_CLK_EDIT, active_picture(pic, 64));
        update_preview(dlg);
        SetForegroundWindow(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CLK_EDIT && HIWORD(wParam) == EN_CHANGE) {
            update_preview(dlg);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            wchar_t pic[64], built[64];
            GetDlgItemTextW(dlg, IDC_CLK_EDIT, pic, 64);
            build_picture(built, 64);
            /* Typing back what the menu already makes is no custom format. */
            if (wcscmp(pic, built) == 0) pic[0] = 0;
            wcscpy_s(g_cfg.clock_format, 64, pic);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static bool show_custom_dialog(HWND owner) {
    static DlgTpl t;
    ZeroMemory(&t, sizeof(t));
    t.p = (WORD *)t.buf;
    tpl_dword(&t, DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    tpl_dword(&t, WS_EX_TOPMOST);
    tpl_word(&t, 7); /* controls below */
    tpl_word(&t, 0);
    tpl_word(&t, 0);
    tpl_word(&t, 268);
    tpl_word(&t, 150);
    tpl_word(&t, 0); /* no menu */
    tpl_word(&t, 0); /* the standard dialog class */
    tpl_str(&t, L"Clock format");
    tpl_word(&t, 9);
    tpl_str(&t, L"Segoe UI");

    tpl_item(&t, SS_LEFT, 10, 8, 248, 10, (WORD)-1, 0x0082,
             L"Write how the clock should read. These letters stand for the date and time:");
    tpl_item(&t, SS_LEFT, 10, 22, 248, 52, (WORD)-1, 0x0082,
             L"d  day (7)     dd  day (07)     ddd  Fri     dddd  Friday\n"
             L"M  month (3)     MM  month (03)     MMM  Mar     MMMM  March\n"
             L"yy  year (26)     yyyy  year (2026)\n"
             L"HH  24-hour     hh  12-hour     tt  AM or PM\n"
             L"mm  minutes     ss  seconds     'text'  written as is");
    tpl_item(&t, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 10, 80, 248, 14, IDC_CLK_EDIT, 0x0081, L"");
    tpl_item(&t, SS_LEFT, 10, 100, 36, 10, (WORD)-1, 0x0082, L"Preview:");
    tpl_item(&t, SS_LEFT | SS_NOPREFIX, 46, 100, 212, 10, IDC_CLK_PREVIEW, 0x0082, L"");
    tpl_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 126, 52, 15, IDOK, 0x0080, L"OK");
    tpl_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 206, 126, 52, 15, IDCANCEL, 0x0080, L"Cancel");

    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t.buf, owner, custom_dlg_proc, 0);
    return r == IDOK;
}

/* ---- the right-click menu ---- */

enum {
    CLK_24H = 1, CLK_12H, CLK_SECONDS, CLK_WEEKDAY, CLK_DATE,
    CLK_STYLE0 = 10, CLK_STYLE1, CLK_STYLE2, CLK_STYLE3, CLK_STYLE4,
    CLK_CUSTOM = 20, CLK_RESET, CLK_WINDOWS
};

/* Returns true when the clock reads differently now. */
bool clock_menu(HWND owner, int sx, int sy) {
    HMENU m = CreatePopupMenu();
    HMENU order = CreatePopupMenu();
    if (!m || !order) {
        if (m) DestroyMenu(m);
        if (order) DestroyMenu(order);
        return false;
    }
    /* Picking any choice below drops a custom format, so the menu never looks stuck. */
    bool custom = g_cfg.clock_format[0] != 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t *styles[5] = { L"dd MMM", L"MMM d", L"dd/MM/yyyy", L"MM/dd/yyyy", L"yyyy-MM-dd" };
    for (int i = 0; i < 5; i++) {
        wchar_t label[32];
        clock_format_picture(styles[i], &st, label, 32);
        AppendMenuW(order, MF_STRING, (UINT_PTR)(CLK_STYLE0 + i), label);
    }
    int style = (g_cfg.clock_date_style >= 0 && g_cfg.clock_date_style <= 4) ? g_cfg.clock_date_style : 0;
    CheckMenuRadioItem(order, CLK_STYLE0, CLK_STYLE4, (UINT)(CLK_STYLE0 + style), MF_BYCOMMAND);

    AppendMenuW(m, MF_STRING, CLK_24H, L"24-hour time");
    AppendMenuW(m, MF_STRING, CLK_12H, L"12-hour time (AM and PM)");
    if (!custom) CheckMenuRadioItem(m, CLK_24H, CLK_12H, g_cfg.clock_24h ? CLK_24H : CLK_12H, MF_BYCOMMAND);
    AppendMenuW(m, MF_STRING | (g_cfg.clock_seconds ? MF_CHECKED : 0), CLK_SECONDS, L"Show seconds");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_cfg.clock_weekday ? MF_CHECKED : 0), CLK_WEEKDAY, L"Show the day of the week");
    AppendMenuW(m, MF_STRING | (g_cfg.clock_date ? MF_CHECKED : 0), CLK_DATE, L"Show the date");
    AppendMenuW(m, MF_POPUP | (g_cfg.clock_date ? 0 : MF_GRAYED), (UINT_PTR)order, L"Date style");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (custom ? MF_CHECKED : 0), CLK_CUSTOM, L"Custom format...");
    AppendMenuW(m, MF_STRING, CLK_RESET, L"Use the default format");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CLK_WINDOWS, L"Windows date and time settings");

    SetForegroundWindow(owner);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, owner, NULL);
    DestroyMenu(m); /* also frees the submenu */

    bool changed = true;
    switch (cmd) {
    case CLK_24H: g_cfg.clock_24h = true; break;
    case CLK_12H: g_cfg.clock_24h = false; break;
    case CLK_SECONDS: g_cfg.clock_seconds = !g_cfg.clock_seconds; break;
    case CLK_WEEKDAY: g_cfg.clock_weekday = !g_cfg.clock_weekday; break;
    case CLK_DATE: g_cfg.clock_date = !g_cfg.clock_date; break;
    case CLK_STYLE0: case CLK_STYLE1: case CLK_STYLE2: case CLK_STYLE3: case CLK_STYLE4:
        g_cfg.clock_date_style = (int)(cmd - CLK_STYLE0);
        break;
    case CLK_CUSTOM: changed = show_custom_dialog(owner); break;
    case CLK_RESET:
        g_cfg.clock_24h = true;
        g_cfg.clock_seconds = false;
        g_cfg.clock_weekday = true;
        g_cfg.clock_date = true;
        g_cfg.clock_date_style = 0;
        g_cfg.clock_format[0] = 0;
        break;
    case CLK_WINDOWS:
        ShellExecuteW(NULL, L"open", L"ms-settings:dateandtime", NULL, NULL, SW_SHOWNORMAL);
        changed = false;
        break;
    default: changed = false; break;
    }
    if (changed && cmd != CLK_CUSTOM) g_cfg.clock_format[0] = 0;
    if (changed) config_save_clock();
    return changed;
}
