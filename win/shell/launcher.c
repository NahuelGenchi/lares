// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wctype.h>

/*
 * The launcher (issue #148): Alt+Space, and a lone tap of the Windows key
 * when its own setting is on.
 *
 * - With nothing typed: the dock's first nine pinned apps, as a 3x3 grid
 * - Typing searches Start menu shortcuts, the installed Store apps and a few
 *   Windows Settings pages and tools, and works out simple sums
 * - Never reads the user's own folders: only the two Start menu folders and
 *   Windows' list of installed apps
 * - Gives its memory back when it hides
 */

#define LAUNCHER_CLASS  L"LaresLauncherWnd"
#define ID_LAUNCH_EDIT  3001
#define MAX_LAUNCH_APPS 640
#define L_WND_W         580
#define L_WND_H         476

typedef struct {
    wchar_t name[140];
    wchar_t name_lower[140];
    wchar_t target[MAX_PATH];
    wchar_t umid[256];
    wchar_t icon_rel[MAX_PATH];
    wchar_t category[24];
} LauncherEntry;

static HWND     g_hLaunchWnd = NULL;
static HWND     g_hEdit = NULL;
static WNDPROC  g_orig_edit_proc = NULL;
static HFONT    g_hEditFont = NULL;
static HBRUSH   g_hEditBrush = NULL;

static LauncherEntry g_apps[MAX_LAUNCH_APPS];
static int           g_app_count = 0;
/* The grid: the dock's pinned apps, taken when the launcher opens. */
typedef struct {
    wchar_t name[128];
    wchar_t target[MAX_PATH];
    wchar_t umid[256];
    wchar_t icon_rel[MAX_PATH];
} FavoriteItem;
static FavoriteItem  g_favorites[9];
static int           g_favorite_count = 0;

static int           g_filtered[64];
static int           g_filtered_count = 0;
static int           g_sel_idx = 0;
static int           g_hover_idx = -1;
static bool          g_show_all_mode = false;
static bool          g_indexed = false;
static wchar_t       g_query[128] = {0};
static HWND          g_prev_fg_wnd = NULL;

/* Zero-overhead inline calculator state (no heap allocations) */
static bool          g_calc_valid = false;
static wchar_t       g_calc_expr[128] = {0};
static wchar_t       g_calc_result[64] = {0};

static RECT g_item_rects[64];
static int  g_item_rect_count = 0;
static RECT g_toggle_all_rc = {0};

/* =========================================================================
 * Inline Recursive-Descent Math Evaluator
 * Supports: +, -, *, /, %, ^, parentheses, decimals, unary +/-,
 * postfix %, implicit multiplication, constants (pi, e), and functions
 * (sqrt, abs, sin, cos, tan, log, ln, exp, floor, ceil, round, cbrt,
 *  pow, min, max). Stack-only with zero heap allocation.
 * ========================================================================= */
typedef struct {
    const wchar_t *p;
    bool           err;
    bool           has_op_or_func;
    int            depth;
} CalcParser;

static void calc_skip_ws(CalcParser *cp) {
    while (*cp->p == L' ' || *cp->p == L'\t') cp->p++;
}

static bool calc_starts_primary(const CalcParser *cp) {
    wchar_t ch = *cp->p;
    return (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'(' || iswalpha(ch);
}

static double calc_parse_expr(CalcParser *cp);
static double calc_parse_unary(CalcParser *cp);

static double calc_parse_primary(CalcParser *cp) {
    calc_skip_ws(cp);
    if (cp->err) return 0.0;
    if (++cp->depth > 64) { cp->err = true; return 0.0; }

    double val = 0.0;
    wchar_t ch = *cp->p;

    if (ch == L'(') {
        cp->p++;
        cp->has_op_or_func = true;
        val = calc_parse_expr(cp);
        calc_skip_ws(cp);
        if (*cp->p != L')') {
            cp->err = true;
            cp->depth--;
            return 0.0;
        }
        cp->p++;
    } else if ((ch >= L'0' && ch <= L'9') || ch == L'.') {
        const wchar_t *start = cp->p;
        wchar_t *endp = NULL;
        val = wcstod(start, &endp);
        if (!endp || endp == start) {
            cp->err = true;
            cp->depth--;
            return 0.0;
        }
        cp->p = endp;
    } else if (iswalpha(ch)) {
        wchar_t ident[24];
        size_t n = 0;
        while (iswalpha(*cp->p) && n + 1 < sizeof(ident) / sizeof(ident[0])) {
            ident[n++] = (wchar_t)towlower(*cp->p++);
        }
        ident[n] = L'\0';
        calc_skip_ws(cp);

        if (*cp->p == L'(') {
            cp->p++;
            cp->has_op_or_func = true;
            double a = calc_parse_expr(cp);
            double b = 0.0;
            bool has_b = false;
            calc_skip_ws(cp);
            if (*cp->p == L',') {
                cp->p++;
                b = calc_parse_expr(cp);
                has_b = true;
                calc_skip_ws(cp);
            }
            if (*cp->p != L')' || cp->err) {
                cp->err = true;
                cp->depth--;
                return 0.0;
            }
            cp->p++;

            if (!has_b) {
                if (wcscmp(ident, L"sqrt") == 0) {
                    if (a < 0.0) cp->err = true;
                    else val = sqrt(a);
                } else if (wcscmp(ident, L"cbrt") == 0) {
                    val = cbrt(a);
                } else if (wcscmp(ident, L"abs") == 0) {
                    val = fabs(a);
                } else if (wcscmp(ident, L"sin") == 0) {
                    val = sin(a);
                } else if (wcscmp(ident, L"cos") == 0) {
                    val = cos(a);
                } else if (wcscmp(ident, L"tan") == 0) {
                    val = tan(a);
                } else if (wcscmp(ident, L"log") == 0 || wcscmp(ident, L"log10") == 0) {
                    if (a <= 0.0) cp->err = true;
                    else val = log10(a);
                } else if (wcscmp(ident, L"ln") == 0) {
                    if (a <= 0.0) cp->err = true;
                    else val = log(a);
                } else if (wcscmp(ident, L"exp") == 0) {
                    val = exp(a);
                } else if (wcscmp(ident, L"floor") == 0) {
                    val = floor(a);
                } else if (wcscmp(ident, L"ceil") == 0) {
                    val = ceil(a);
                } else if (wcscmp(ident, L"round") == 0) {
                    val = round(a);
                } else {
                    cp->err = true;
                }
            } else {
                if (wcscmp(ident, L"pow") == 0) {
                    val = pow(a, b);
                } else if (wcscmp(ident, L"min") == 0) {
                    val = (a < b) ? a : b;
                } else if (wcscmp(ident, L"max") == 0) {
                    val = (a > b) ? a : b;
                } else {
                    cp->err = true;
                }
            }
        } else {
            if (wcscmp(ident, L"pi") == 0) {
                val = 3.14159265358979323846;
                cp->has_op_or_func = true;
            } else if (wcscmp(ident, L"e") == 0) {
                /* Do not set has_op_or_func for bare 'e' so searching 'e' still finds Edge/Explorer */
                val = 2.71828182845904523536;
            } else {
                cp->err = true;
            }
        }
    } else {
        cp->err = true;
    }

    /* Postfix percent when '%' is not followed by a primary operand (e.g. 200 * 15%) */
    while (!cp->err) {
        calc_skip_ws(cp);
        if (*cp->p == L'%') {
            CalcParser peek = *cp;
            peek.p++;
            calc_skip_ws(&peek);
            if (!calc_starts_primary(&peek)) {
                cp->p++;
                cp->has_op_or_func = true;
                val *= 0.01;
                continue;
            }
        }
        break;
    }

    cp->depth--;
    return val;
}

static double calc_parse_power(CalcParser *cp) {
    double base = calc_parse_primary(cp);
    if (cp->err) return 0.0;
    calc_skip_ws(cp);
    if (*cp->p == L'^') {
        cp->p++;
        cp->has_op_or_func = true;
        double expo = calc_parse_unary(cp);
        if (cp->err) return 0.0;
        base = pow(base, expo);
    }
    return base;
}

static double calc_parse_unary(CalcParser *cp) {
    calc_skip_ws(cp);
    if (cp->err) return 0.0;
    if (*cp->p == L'+') {
        cp->p++;
        return calc_parse_unary(cp);
    }
    if (*cp->p == L'-' || *cp->p == 0x2212) {
        cp->p++;
        return -calc_parse_unary(cp);
    }
    return calc_parse_power(cp);
}

static double calc_parse_term(CalcParser *cp) {
    double val = calc_parse_unary(cp);
    while (!cp->err) {
        calc_skip_ws(cp);
        wchar_t op = *cp->p;
        if (op == L'*' || op == 0x00D7) {
            cp->p++;
            cp->has_op_or_func = true;
            val *= calc_parse_unary(cp);
        } else if (op == L'/' || op == 0x00F7) {
            cp->p++;
            cp->has_op_or_func = true;
            double rhs = calc_parse_unary(cp);
            if (rhs == 0.0) { cp->err = true; return 0.0; }
            val /= rhs;
        } else if (op == L'%') {
            cp->p++;
            cp->has_op_or_func = true;
            double rhs = calc_parse_unary(cp);
            if (rhs == 0.0) { cp->err = true; return 0.0; }
            val = fmod(val, rhs);
        } else if (op == L'(' || iswalpha(op)) {
            /* Implicit multiplication: e.g. 2(3+4), 2pi, 2sqrt(9) */
            cp->has_op_or_func = true;
            val *= calc_parse_power(cp);
        } else {
            break;
        }
    }
    return val;
}

static double calc_parse_expr(CalcParser *cp) {
    double val = calc_parse_term(cp);
    while (!cp->err) {
        calc_skip_ws(cp);
        wchar_t op = *cp->p;
        if (op == L'+') {
            cp->p++;
            cp->has_op_or_func = true;
            val += calc_parse_term(cp);
        } else if (op == L'-' || op == 0x2212) {
            cp->p++;
            cp->has_op_or_func = true;
            val -= calc_parse_term(cp);
        } else {
            break;
        }
    }
    return val;
}

static bool calc_format_number(double val, wchar_t *out, size_t len) {
    if (!isfinite(val)) return false;
    if (fabs(val) < 1e-14) val = 0.0;

    /* Round to 12 significant digits to eliminate IEEE-754 dust like 0.30000000000000004 */
    wchar_t gbuf[64];
    _snwprintf_s(gbuf, 64, _TRUNCATE, L"%.12g", val);
    double clean = wcstod(gbuf, NULL);
    if (fabs(clean) < 1e-14) clean = 0.0;

    if (wcschr(gbuf, L'e') == NULL && wcschr(gbuf, L'E') == NULL) {
        if (wcscmp(gbuf, L"-0") == 0) wcsncpy_s(out, len, L"0", _TRUNCATE);
        else wcsncpy_s(out, len, gbuf, _TRUNCATE);
        return true;
    }

    /* Prefer clean non-scientific formatting for everyday magnitudes */
    double av = fabs(clean);
    if (av >= 1e-6 && av < 1e15) {
        if (floor(clean) == clean) {
            _snwprintf_s(out, len, _TRUNCATE, L"%.0f", clean);
            return true;
        }
        wchar_t fbuf[64];
        _snwprintf_s(fbuf, 64, _TRUNCATE, L"%.12f", clean);
        wchar_t *dot = wcschr(fbuf, L'.');
        if (dot) {
            size_t n = wcslen(fbuf);
            while (n > 0 && fbuf[n - 1] == L'0') fbuf[--n] = L'\0';
            if (n > 0 && fbuf[n - 1] == L'.') fbuf[--n] = L'\0';
        }
        if (wcscmp(fbuf, L"-0") == 0) wcsncpy_s(out, len, L"0", _TRUNCATE);
        else wcsncpy_s(out, len, fbuf, _TRUNCATE);
        return true;
    }

    wcsncpy_s(out, len, gbuf, _TRUNCATE);
    return true;
}

static bool calc_evaluate_query(const wchar_t *raw, wchar_t *out_expr, size_t expr_len,
                                wchar_t *out_res, size_t res_len) {
    if (!raw || !raw[0]) return false;
    const wchar_t *p = raw;
    while (*p == L' ' || *p == L'\t') p++;
    bool force_eval = false;
    if (*p == L'=') {
        force_eval = true;
        p++;
        while (*p == L' ' || *p == L'\t') p++;
    }
    if (!*p) return false;

    CalcParser cp;
    cp.p = p;
    cp.err = false;
    cp.has_op_or_func = force_eval;
    cp.depth = 0;

    double val = calc_parse_expr(&cp);
    calc_skip_ws(&cp);
    if (cp.err || *cp.p != L'\0' || !cp.has_op_or_func) return false;
    if (!calc_format_number(val, out_res, res_len)) return false;

    wcsncpy_s(out_expr, expr_len, p, _TRUNCATE);
    size_t n = wcslen(out_expr);
    while (n > 0 && (out_expr[n - 1] == L' ' || out_expr[n - 1] == L'\t')) out_expr[--n] = L'\0';
    return true;
}

static void launcher_copy_text_clipboard(const wchar_t *text) {
    if (!text || !text[0]) return;
    if (!OpenClipboard(g_hLaunchWnd)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hm) {
        void *dst = GlobalLock(hm);
        if (dst) {
            memcpy(dst, text, bytes);
            GlobalUnlock(hm);
            if (!SetClipboardData(CF_UNICODETEXT, hm)) {
                GlobalFree(hm);
            }
        } else {
            GlobalFree(hm);
        }
    }
    CloseClipboard();
}

static void str_to_lower(const wchar_t *src, wchar_t *dst, size_t max_len) {
    size_t i = 0;
    for (; src[i] && i + 1 < max_len; i++) dst[i] = (wchar_t)towlower(src[i]);
    dst[i] = L'\0';
}

static bool app_exists_lower(const wchar_t *lower) {
    for (int i = 0; i < g_app_count; i++) {
        if (wcscmp(g_apps[i].name_lower, lower) == 0) return true;
    }
    return false;
}

static void add_app(const wchar_t *name, const wchar_t *target, const wchar_t *umid,
                    const wchar_t *icon_rel, const wchar_t *cat) {
    if (g_app_count >= MAX_LAUNCH_APPS || !name || !name[0]) return;
    wchar_t low[140];
    str_to_lower(name, low, 140);
    if (app_exists_lower(low)) return;

    LauncherEntry *e = &g_apps[g_app_count++];
    ZeroMemory(e, sizeof(*e));
    wcsncpy_s(e->name, 140, name, _TRUNCATE);
    wcsncpy_s(e->name_lower, 140, low, _TRUNCATE);
    if (target)   wcsncpy_s(e->target, MAX_PATH, target, _TRUNCATE);
    if (umid)     wcsncpy_s(e->umid, 256, umid, _TRUNCATE);
    if (icon_rel) wcsncpy_s(e->icon_rel, MAX_PATH, icon_rel, _TRUNCATE);
    if (cat)      wcsncpy_s(e->category, 24, cat, _TRUNCATE);
}

static void load_favorites(void) {
    g_favorite_count = 0;
    for (int i = 0; i < g_dock_cfg_count && g_favorite_count < 9; i++) {
        const DockItemConfig *d = &g_dock_cfg[i];
        if (d->type != DOCK_ITEM_APP || !d->pinned || !d->name[0]) continue;
        FavoriteItem *f = &g_favorites[g_favorite_count++];
        wcsncpy_s(f->name, 128, d->name, _TRUNCATE);
        wcsncpy_s(f->target, MAX_PATH, d->command[0] ? d->command : d->path, _TRUNCATE);
        wcsncpy_s(f->umid, 256, d->umid, _TRUNCATE);
        wcsncpy_s(f->icon_rel, MAX_PATH, d->icon_rel, _TRUNCATE);
    }
}

static void scan_start_menu_dir(const wchar_t *dir, int depth) {
    if (depth > 3 || g_app_count >= MAX_LAUNCH_APPS) return;
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (hf == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH];
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_start_menu_dir(full, depth + 1);
        } else {
            const wchar_t *ext = PathFindExtensionW(fd.cFileName);
            if (ext && (_wcsicmp(ext, L".lnk") == 0 || _wcsicmp(ext, L".url") == 0)) {
                wchar_t base[140];
                size_t n = (size_t)(ext - fd.cFileName);
                if (n >= 140) n = 139;
                wcsncpy_s(base, 140, fd.cFileName, n);
                base[n] = L'\0';
                if (!wcsstr(base, L"Uninstall") && !wcsstr(base, L"Documentation") && !wcsstr(base, L"Help")) {
                    add_app(base, full, NULL, NULL, L"App");
                }
            }
        }
    } while (FindNextFileW(hf, &fd) && g_app_count < MAX_LAUNCH_APPS);
    FindClose(hf);
}

static void scan_apps_folder(void) {
    PIDLIST_ABSOLUTE pidl_apps = NULL;
    if (FAILED(SHGetKnownFolderIDList(&FOLDERID_AppsFolder, 0, NULL, &pidl_apps)) || !pidl_apps) return;

    IShellFolder *desktop = NULL;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
        IShellFolder *apps_dir = NULL;
        if (SUCCEEDED(IShellFolder_BindToObject(desktop, pidl_apps, NULL, &IID_IShellFolder, (void **)&apps_dir)) && apps_dir) {
            IEnumIDList *en = NULL;
            if (SUCCEEDED(IShellFolder_EnumObjects(apps_dir, NULL, SHCONTF_NONFOLDERS, &en)) && en) {
                PITEMID_CHILD child = NULL;
                ULONG fetched = 0;
                while (g_app_count < MAX_LAUNCH_APPS &&
                       IEnumIDList_Next(en, 1, &child, &fetched) == S_OK && fetched == 1) {
                    STRRET sr_name, sr_parse;
                    wchar_t disp[140] = {0}, parse[256] = {0};
                    if (SUCCEEDED(IShellFolder_GetDisplayNameOf(apps_dir, child, SHGDN_NORMAL, &sr_name))) {
                        StrRetToBufW(&sr_name, child, disp, 140);
                    }
                    if (SUCCEEDED(IShellFolder_GetDisplayNameOf(apps_dir, child, SHGDN_FORPARSING, &sr_parse))) {
                        StrRetToBufW(&sr_parse, child, parse, 256);
                    }
                    if (disp[0] && parse[0] && wcschr(parse, L'!') != NULL) {
                        wchar_t tgt[320];
                        _snwprintf_s(tgt, 320, _TRUNCATE, L"shell:AppsFolder\\%ls", parse);
                        add_app(disp, tgt, parse, NULL, L"App");
                    }
                    CoTaskMemFree(child);
                }
                IEnumIDList_Release(en);
            }
            IShellFolder_Release(apps_dir);
        }
        IShellFolder_Release(desktop);
    }
    CoTaskMemFree(pidl_apps);
}

static int cmp_entries(const void *a, const void *b) {
    const LauncherEntry *ea = (const LauncherEntry *)a;
    const LauncherEntry *eb = (const LauncherEntry *)b;
    return wcscmp(ea->name_lower, eb->name_lower);
}

static void build_launcher_index(void) {
    g_app_count = 0;

    /* Lares' own parts that are on, then Windows' Settings pages and tools,
     * which Windows itself finds by name. Installed apps come from the scans. */
    if (g_cfg.clipboard_enabled)
        add_app(L"Clipboard History (Alt+V)", L"@builtin/clipboard", L"@builtin/clipboard", NULL, L"Lares");
    add_app(L"Processes", L"@builtin/processes", L"@builtin/processes", NULL, L"Lares");
    if (g_cfg.files_enabled)
        add_app(L"Files", L"@builtin/files", L"@builtin/files", NULL, L"Lares");
    if (g_cfg.viewers_enabled)
        add_app(L"Text Editor", L"@builtin/editor", L"@builtin/editor", NULL, L"Lares");
    add_app(L"Screenshot (Region)", L"@builtin/screenshot", L"@builtin/screenshot", NULL, L"Lares");
    add_app(L"Settings (Windows)",          L"ms-settings:",              NULL, NULL, L"Setting");
    add_app(L"Settings: Display",           L"ms-settings:display",       NULL, NULL, L"Setting");
    add_app(L"Settings: Sound",             L"ms-settings:sound",         NULL, NULL, L"Setting");
    add_app(L"Settings: Bluetooth",         L"ms-settings:bluetooth",     NULL, NULL, L"Setting");
    add_app(L"Settings: Network",           L"ms-settings:network",       NULL, NULL, L"Setting");
    add_app(L"Settings: Installed Apps",    L"ms-settings:appsfeatures",  NULL, NULL, L"Setting");
    add_app(L"Settings: Windows Update",    L"ms-settings:windowsupdate", NULL, NULL, L"Setting");
    add_app(L"Settings: Startup Apps",      L"ms-settings:startupapps",   NULL, NULL, L"Setting");
    add_app(L"Task Manager",                L"taskmgr.exe",               NULL, NULL, L"Tool");
    add_app(L"Control Panel",               L"control.exe",               NULL, NULL, L"Tool");
    add_app(L"Device Manager",              L"devmgmt.msc",               NULL, NULL, L"Tool");
    add_app(L"Services",                    L"services.msc",              NULL, NULL, L"Tool");
    add_app(L"Disk Management",             L"diskmgmt.msc",              NULL, NULL, L"Tool");
    add_app(L"Resource Monitor",            L"resmon.exe",                NULL, NULL, L"Tool");

    /* The dock's pins first, so their names and pictures win */
    for (int i = 0; i < g_dock_cfg_count; i++) {
        const DockItemConfig *d = &g_dock_cfg[i];
        if (d->type == DOCK_ITEM_APP && d->name[0]) {
            add_app(d->name, d->command[0] ? d->command : d->path, d->umid, d->icon_rel, L"App");
        }
    }

    wchar_t sm_path[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, sm_path) == S_OK) {
        scan_start_menu_dir(sm_path, 0);
    }
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, sm_path) == S_OK) {
        scan_start_menu_dir(sm_path, 0);
    }
    scan_apps_folder();

    qsort(g_apps, (size_t)g_app_count, sizeof(LauncherEntry), cmp_entries);
    g_indexed = true;
}

static void update_filtered_list(void) {
    GetWindowTextW(g_hEdit, g_query, 128);
    wchar_t qlow[128];
    str_to_lower(g_query, qlow, 128);

    g_calc_valid = calc_evaluate_query(g_query, g_calc_expr, 128, g_calc_result, 64);
    int max_rows = g_calc_valid ? 7 : 9;

    g_filtered_count = 0;
    if (qlow[0] || g_show_all_mode) {
        /* Prefix matches first, then substring matches */
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < g_app_count && g_filtered_count < max_rows; i++) {
                const wchar_t *pos = qlow[0] ? wcsstr(g_apps[i].name_lower, qlow) : g_apps[i].name_lower;
                if (!pos) continue;
                bool is_prefix = (pos == g_apps[i].name_lower);
                if ((pass == 0 && is_prefix) || (pass == 1 && !is_prefix)) {
                    g_filtered[g_filtered_count++] = i;
                }
            }
        }
    }
    g_sel_idx = 0;
    g_hover_idx = -1;
    if (g_hLaunchWnd) InvalidateRect(g_hLaunchWnd, NULL, FALSE);
}

static void launch_target_item(const wchar_t *target, const wchar_t *umid) {
    launcher_hide();
    if (!target || !target[0]) return;
    if (wcscmp(target, L"@builtin/processes") == 0) {
        action_open_processes();
        return;
    }
    if (wcscmp(target, L"@builtin/files") == 0) {
        action_open_files(NULL);
        return;
    }
    if (wcscmp(target, L"@builtin/editor") == 0) {
        action_open_viewer(1, NULL);
        return;
    }
    if (wcscmp(target, L"@builtin/screenshot") == 0) {
        action_screenshot(0);
        return;
    }
    if (wcscmp(target, L"@builtin/clipboard") == 0) {
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_TOGGLE_CLIPBOARD, 0, 0);
        return;
    }
    if (wcsncmp(target, L"shell:AppsFolder\\", 17) == 0) {
        ShellExecuteW(NULL, L"open", L"explorer.exe", target, NULL, SW_SHOWNORMAL);
        return;
    }
    shell_launch_app(target, NULL, NULL, umid);
}

static void activate_current_selection(void) {
    bool is_grid = (!g_query[0] && !g_show_all_mode);
    if (is_grid) {
        if (g_sel_idx >= 0 && g_sel_idx < g_favorite_count) {
            launch_target_item(g_favorites[g_sel_idx].target, g_favorites[g_sel_idx].umid);
        }
    } else {
        if (g_calc_valid) {
            if (g_sel_idx == 0) {
                launcher_copy_text_clipboard(g_calc_result);
                launcher_hide();
                return;
            }
            int app_pos = g_sel_idx - 1;
            if (app_pos >= 0 && app_pos < g_filtered_count) {
                const LauncherEntry *e = &g_apps[g_filtered[app_pos]];
                launch_target_item(e->target, e->umid);
            }
            return;
        }
        if (g_sel_idx >= 0 && g_sel_idx < g_filtered_count) {
            const LauncherEntry *e = &g_apps[g_filtered[g_sel_idx]];
            launch_target_item(e->target, e->umid);
        } else if (g_query[0]) {
            launch_target_item(g_query, NULL);
        }
    }
}

static void draw_launcher(LayerSurface *fb, int w, int h) {
    {
        GpGraphics *g = fb->g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);
        GdipSetInterpolationMode(g, GP_INTERP_HIGHQUALITY_BICUBIC);

        /* Background */
        ARGB bg_solid = g_theme.is_dark ? MAKE_ARGB(255, 28, 28, 32) : MAKE_ARGB(255, 245, 245, 248);
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, (float)h, 0.0f, bg_solid);

        /* Search box container */
        draw_round_rect_fill(g, 18.0f, 16.0f, (float)(w - 36), 42.0f, 10.0f, g_theme.popup_surface);
        draw_round_rect_stroke(g, 18.5f, 16.5f, (float)(w - 37), 41.0f, 10.0f, g_theme.popup_border, 1.0f);
        draw_icon_glyph(g, 0xE721, 15.0f, g_theme.fg_muted, 26.0f, 16.0f, 26.0f, 42.0f);

        bool is_grid = (!g_query[0] && !g_show_all_mode);

        /* Section header + All Apps / Favorites toggle button */
        int hy = 68;
        const wchar_t *hdr = is_grid ? L"Pinned in the dock"
                           : (g_calc_valid ? L"Calculator"
                           : (g_query[0] ? L"Search Results" : L"All Apps"));
        draw_text(g, hdr, L"Segoe UI", 12.5f, GP_FONT_BOLD, g_theme.fg_secondary,
                  22.0f, (float)hy, 220.0f, 24.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        g_toggle_all_rc.left = w - 114; g_toggle_all_rc.top = hy;
        g_toggle_all_rc.right = w - 20; g_toggle_all_rc.bottom = hy + 24;
        if (!g_query[0]) {
            draw_round_rect_fill(g, (float)g_toggle_all_rc.left, (float)g_toggle_all_rc.top,
                                 94.0f, 24.0f, 6.0f, g_theme.popup_surface);
            draw_text(g, g_show_all_mode ? L"Favorites" : L"All Apps", L"Segoe UI", 11.5f, GP_FONT_BOLD,
                      g_theme.accent, (float)g_toggle_all_rc.left, (float)g_toggle_all_rc.top,
                      94.0f, 24.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }

        g_item_rect_count = 0;

        if (is_grid) {
            /* 3x3 Favorites Grid */
            int cols = 3;
            int gx = 20, gy = 102;
            int gap = 12;
            int tile_w = (w - 40 - gap * (cols - 1)) / cols;
            int tile_h = 104;
            int show_n = min(g_favorite_count, 9);

            for (int i = 0; i < show_n; i++) {
                int r = i / cols, c = i % cols;
                int x = gx + c * (tile_w + gap);
                int y = gy + r * (tile_h + gap);

                g_item_rects[g_item_rect_count].left = x;
                g_item_rects[g_item_rect_count].top = y;
                g_item_rects[g_item_rect_count].right = x + tile_w;
                g_item_rects[g_item_rect_count].bottom = y + tile_h;
                g_item_rect_count++;

                ARGB fill = (i == g_sel_idx) ? g_theme.accent_overlay_selected
                          : (i == g_hover_idx) ? g_theme.accent_overlay_hover : g_theme.popup_surface;
                draw_round_rect_fill(g, (float)x, (float)y, (float)tile_w, (float)tile_h, 12.0f, fill);
                if (i == g_sel_idx) {
                    draw_round_rect_stroke(g, (float)x + 0.5f, (float)y + 0.5f,
                                           (float)tile_w - 1.0f, (float)tile_h - 1.0f, 12.0f, g_theme.accent, 1.5f);
                }

                GpBitmap *ico = icon_cache_get(g_favorites[i].umid, g_favorites[i].target,
                                               g_favorites[i].icon_rel, NULL);
                if (ico) {
                    GdipDrawImageRectI(g, (GpImage *)ico, x + (tile_w - 42) / 2, y + 16, 42, 42);
                } else {
                    draw_icon_glyph(g, 0xE71D, 24.0f, g_theme.accent,
                                    (float)x, (float)(y + 16), (float)tile_w, 42.0f);
                }
                draw_text(g, g_favorites[i].name, L"Segoe UI", 12.5f, GP_FONT_REGULAR, g_theme.fg_primary,
                          (float)(x + 8), (float)(y + 66), (float)(tile_w - 16), 28.0f,
                          GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
            }
        } else {
            /* Search / Calculator / All Apps List */
            int ly = 100;
            int row_h = 38;
            int idx_offset = 0;

            if (g_calc_valid) {
                int card_h = 64;
                g_item_rects[g_item_rect_count].left = 18;
                g_item_rects[g_item_rect_count].top = ly;
                g_item_rects[g_item_rect_count].right = w - 18;
                g_item_rects[g_item_rect_count].bottom = ly + card_h;
                g_item_rect_count++;
                idx_offset = 1;

                bool calc_sel = (g_sel_idx == 0);
                bool calc_hov = (g_hover_idx == 0);
                draw_round_rect_fill(g, 18.0f, (float)ly, (float)(w - 36), (float)card_h, 11.0f, g_theme.popup_surface);
                if (calc_sel) {
                    draw_round_rect_fill(g, 18.0f, (float)ly, (float)(w - 36), (float)card_h, 11.0f, g_theme.accent_overlay_selected);
                    draw_round_rect_stroke(g, 18.5f, (float)ly + 0.5f, (float)(w - 37), (float)card_h - 1.0f, 11.0f, g_theme.accent, 1.5f);
                } else if (calc_hov) {
                    draw_round_rect_fill(g, 18.0f, (float)ly, (float)(w - 36), (float)card_h, 11.0f, g_theme.accent_overlay_hover);
                    draw_round_rect_stroke(g, 18.5f, (float)ly + 0.5f, (float)(w - 37), (float)card_h - 1.0f, 11.0f, g_theme.popup_border, 1.0f);
                } else {
                    draw_round_rect_stroke(g, 18.5f, (float)ly + 0.5f, (float)(w - 37), (float)card_h - 1.0f, 11.0f, g_theme.popup_border, 1.0f);
                }

                /* Left accent icon badge */
                draw_round_rect_fill(g, 30.0f, (float)(ly + 14), 36.0f, 36.0f, 9.0f, g_theme.accent);
                draw_icon_glyph(g, 0xE8EF, 17.0f, g_theme.fg_on_accent, 30.0f, (float)(ly + 14), 36.0f, 36.0f);

                /* Primary answer + expression subtitle */
                draw_text(g, g_calc_result, L"Segoe UI", 22.0f, GP_FONT_BOLD, g_theme.fg_primary,
                          78.0f, (float)(ly + 7), (float)(w - 208), 30.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                wchar_t expr_sub[144];
                _snwprintf_s(expr_sub, 144, _TRUNCATE, L"= %ls", g_calc_expr);
                draw_text(g, expr_sub, L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.fg_muted,
                          78.0f, (float)(ly + 37), (float)(w - 208), 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

                /* Right-side Copy pill */
                draw_round_rect_fill(g, (float)(w - 122), (float)(ly + 19), 92.0f, 26.0f, 7.0f,
                                     calc_sel ? g_theme.accent : g_theme.tile_bg_hover);
                draw_text(g, L"Copy Result", L"Segoe UI", 11.5f, GP_FONT_BOLD,
                          calc_sel ? g_theme.fg_on_accent : g_theme.fg_primary,
                          (float)(w - 122), (float)(ly + 19), 92.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

                ly += card_h + 10;
            }

            if (g_filtered_count == 0 && !g_calc_valid) {
                draw_text(g, L"No matching apps or settings", L"Segoe UI", 13.0f, GP_FONT_REGULAR, g_theme.fg_muted,
                          20.0f, 160.0f, (float)(w - 40), 32.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
            }
            for (int i = 0; i < g_filtered_count; i++) {
                int item_idx = i + idx_offset;
                const LauncherEntry *e = &g_apps[g_filtered[i]];
                g_item_rects[g_item_rect_count].left = 18;
                g_item_rects[g_item_rect_count].top = ly;
                g_item_rects[g_item_rect_count].right = w - 18;
                g_item_rects[g_item_rect_count].bottom = ly + row_h - 2;
                g_item_rect_count++;

                if (item_idx == g_sel_idx) {
                    draw_round_rect_fill(g, 18.0f, (float)ly, (float)(w - 36), (float)(row_h - 2), 8.0f, g_theme.accent_overlay_selected);
                } else if (item_idx == g_hover_idx) {
                    draw_round_rect_fill(g, 18.0f, (float)ly, (float)(w - 36), (float)(row_h - 2), 8.0f, g_theme.accent_overlay_hover);
                }

                GpBitmap *ico = icon_cache_get(e->umid, e->target, e->icon_rel, NULL);
                if (ico) {
                    GdipDrawImageRectI(g, (GpImage *)ico, 28, ly + 6, 24, 24);
                } else {
                    draw_icon_glyph(g, 0xE71D, 15.0f, g_theme.fg_secondary, 28.0f, (float)ly, 24.0f, (float)(row_h - 2));
                }

                draw_text(g, e->name, L"Segoe UI", 13.0f, (item_idx == g_sel_idx) ? GP_FONT_BOLD : GP_FONT_REGULAR,
                          g_theme.fg_primary, 62.0f, (float)ly, (float)(w - 154), (float)(row_h - 2),
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                draw_text(g, e->category, L"Segoe UI", 11.0f, GP_FONT_REGULAR, g_theme.fg_muted,
                          (float)(w - 92), (float)ly, 64.0f, (float)(row_h - 2), GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
                ly += row_h;
            }
        }

        /* Footer hint */
        wchar_t foot[128];
        if (g_calc_valid && g_sel_idx == 0) {
            wcsncpy_s(foot, 128, L"Enter or click to copy answer to clipboard  |  Esc to close", _TRUNCATE);
        } else {
            _snwprintf_s(foot, 128, _TRUNCATE, L"Search %d apps or type math (e.g. sqrt(144)*2)  |  Enter to run  |  Esc to close", g_app_count);
        }
        draw_text(g, foot, L"Segoe UI", 11.0f, GP_FONT_REGULAR, g_theme.fg_muted,
                  20.0f, (float)(h - 28), (float)(w - 40), 20.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    }
}

static void paint_launcher(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        draw_launcher(&fb, cr.right, cr.bottom);
        opaque_frame_end(hdc, &fb);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            launcher_hide();
            return 0;
        }
        if (wParam == VK_RETURN) {
            activate_current_selection();
            return 0;
        }
        bool is_grid = (!g_query[0] && !g_show_all_mode);
        int max_n = is_grid ? min(g_favorite_count, 9) : (g_filtered_count + (g_calc_valid ? 1 : 0));
        if (max_n > 0) {
            if (wParam == VK_DOWN) {
                g_sel_idx = is_grid ? min(max_n - 1, g_sel_idx + 3) : min(max_n - 1, g_sel_idx + 1);
                InvalidateRect(g_hLaunchWnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_UP) {
                g_sel_idx = is_grid ? max(0, g_sel_idx - 3) : max(0, g_sel_idx - 1);
                InvalidateRect(g_hLaunchWnd, NULL, FALSE);
                return 0;
            }
            if (is_grid && wParam == VK_RIGHT && !g_query[0]) {
                if (g_sel_idx + 1 < max_n) g_sel_idx++;
                InvalidateRect(g_hLaunchWnd, NULL, FALSE);
                return 0;
            }
            if (is_grid && wParam == VK_LEFT && !g_query[0]) {
                if (g_sel_idx > 0) g_sel_idx--;
                InvalidateRect(g_hLaunchWnd, NULL, FALSE);
                return 0;
            }
        }
    }
    return CallWindowProcW(g_orig_edit_proc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK launcher_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(hwnd)) {
            launcher_hide();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_LAUNCH_EDIT && HIWORD(wParam) == EN_CHANGE) {
            update_filtered_list();
        }
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        COLORREF fg = g_theme.is_dark ? RGB(236, 236, 240) : RGB(24, 24, 28);
        COLORREF bg = g_theme.is_dark ? RGB(40, 40, 46) : RGB(255, 255, 255);
        SetTextColor(hdc, fg);
        SetBkColor(hdc, bg);
        return (LRESULT)g_hEditBrush;
    }

    case WM_MOUSEMOVE: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        int nh = -1;
        for (int i = 0; i < g_item_rect_count; i++) {
            if (mx >= g_item_rects[i].left && mx <= g_item_rects[i].right &&
                my >= g_item_rects[i].top && my <= g_item_rects[i].bottom) {
                nh = i;
                break;
            }
        }
        if (nh != g_hover_idx) {
            g_hover_idx = nh;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        if (!g_query[0] && mx >= g_toggle_all_rc.left && mx <= g_toggle_all_rc.right &&
            my >= g_toggle_all_rc.top && my <= g_toggle_all_rc.bottom) {
            g_show_all_mode = !g_show_all_mode;
            update_filtered_list();
            return 0;
        }
        for (int i = 0; i < g_item_rect_count; i++) {
            if (mx >= g_item_rects[i].left && mx <= g_item_rects[i].right &&
                my >= g_item_rects[i].top && my <= g_item_rects[i].bottom) {
                g_sel_idx = i;
                activate_current_selection();
                return 0;
            }
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint_launcher(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void refresh_launcher_theme_objects(void) {
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
    g_hEditBrush = CreateSolidBrush(g_theme.is_dark ? RGB(40, 40, 46) : RGB(255, 255, 255));
    if (g_hLaunchWnd) {
        BOOL dark = g_theme.is_dark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hLaunchWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
}

void launcher_init(HINSTANCE hInst) {
    g_hEditFont = render_create_ui_font(17, false);
    refresh_launcher_theme_objects();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = launcher_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = LAUNCHER_CLASS;
    RegisterClassExW(&wc);

    g_hLaunchWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        LAUNCHER_CLASS, L"Lares launcher",
        WS_POPUP | WS_BORDER,
        100, 100, L_WND_W, L_WND_H,
        NULL, NULL, hInst, NULL);
    if (!g_hLaunchWnd) return;

    DWORD corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(g_hLaunchWnd, 33, &corner, sizeof(corner));
    refresh_launcher_theme_objects();

    g_hEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        54, 26, L_WND_W - 80, 24,
        g_hLaunchWnd, (HMENU)(INT_PTR)ID_LAUNCH_EDIT, hInst, NULL);
    SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
    g_orig_edit_proc = (WNDPROC)SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc);
}

HWND launcher_get_prev_fg(void) {
    return g_prev_fg_wnd;
}

void launcher_toggle(void) {
    if (!g_hLaunchWnd) return;
    if (IsWindowVisible(g_hLaunchWnd)) {
        launcher_hide();
        return;
    }
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hLaunchWnd && fg != g_hDockWnd && fg != g_hTopBarWnd) {
        g_prev_fg_wnd = fg;
    }
    if (!g_indexed) build_launcher_index();
    load_favorites();

    MONITORINFO mi;
    shell_screen_rect(&mi.rcMonitor);

    int x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - L_WND_W) / 2;
    int y = mi.rcMonitor.top + ((mi.rcMonitor.bottom - mi.rcMonitor.top) - L_WND_H) / 3;

    g_show_all_mode = false;
    g_sel_idx = 0;
    g_hover_idx = -1;
    SetWindowTextW(g_hEdit, L"");
    update_filtered_list();

    /* A test copy shows nothing and never takes the focus. */
    if (g_cfg.isolated) return;
    SetWindowPos(g_hLaunchWnd, HWND_TOPMOST, x, y, L_WND_W, L_WND_H, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hLaunchWnd);
    SetFocus(g_hEdit);
    InvalidateRect(g_hLaunchWnd, NULL, FALSE);
}

/* The launcher drawn into a picture, for checks; typed is what the search
 * box holds, NULL for none. */
int launcher_snapshot(const wchar_t *typed, const wchar_t *png_path) {
    if (!g_hLaunchWnd) return 0;
    if (!g_indexed) build_launcher_index();
    load_favorites();
    g_show_all_mode = false;
    SetWindowTextW(g_hEdit, typed ? typed : L"");
    update_filtered_list();
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    int ok = 0;
    if (opaque_frame_begin(&fb, L_WND_W, L_WND_H)) {
        draw_launcher(&fb, L_WND_W, L_WND_H);
        ok = surface_save_png(&fb, png_path);
        surface_destroy(&fb);
    }
    SetWindowTextW(g_hEdit, L"");
    return ok;
}

void launcher_hide(void) {
    if (g_hLaunchWnd && IsWindowVisible(g_hLaunchWnd)) {
        ShowWindow(g_hLaunchWnd, SW_HIDE);
        g_calc_valid = false;
        g_calc_expr[0] = L'\0';
        g_calc_result[0] = L'\0';
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    }
}

/* Settings changed: what the launcher lists is gathered again when it next opens. */
void launcher_on_settings_changed(void) {
    g_indexed = false;
}

void launcher_on_theme_changed(void) {
    refresh_launcher_theme_objects();
    if (g_hLaunchWnd && IsWindowVisible(g_hLaunchWnd)) {
        InvalidateRect(g_hLaunchWnd, NULL, TRUE);
    }
}

void launcher_shutdown(void) {
    if (g_hLaunchWnd) {
        DestroyWindow(g_hLaunchWnd);
        g_hLaunchWnd = NULL;
    }
    if (g_hEditFont)  { DeleteObject(g_hEditFont);  g_hEditFont = NULL; }
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
}
