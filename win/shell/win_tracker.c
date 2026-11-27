// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The windows the bar and dock show, their icons, and starting apps (issue
 * #145). Window events are merged into one refresh. */
#include "shell.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
#include <appmodel.h>
#include <psapi.h>
#include <stdio.h>

static CRITICAL_SECTION g_win_cs;
static TrackedWindow g_windows[LARES_MAX_WINDOWS];
static int g_window_count = 0;
static HWND g_notify_hwnd = NULL;
static HWINEVENTHOOK g_hook_fg = NULL;
static HWINEVENTHOOK g_hook_obj = NULL;
static HWINEVENTHOOK g_hook_min = NULL;
static DWORD g_own_pid = 0;

/* Bounded LRU icon cache (<= LARES_MAX_ICON_CACHE entries, tiny RAM footprint) */
typedef struct {
    wchar_t   key[300];
    GpBitmap *bmp;
    bool      is_square;
    uint32_t  last_used;
} IconCacheEntry;

static IconCacheEntry g_icon_cache[LARES_MAX_ICON_CACHE];
static int g_icon_cache_count = 0;
static uint32_t g_icon_tick = 1;

static bool is_window_cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

static bool is_alt_tab_window(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (is_window_cloaked(hwnd)) return false;

    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if (exStyle & WS_EX_NOACTIVATE) return false;
    if ((style & WS_CHILD) != 0) return false;

    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner && !(exStyle & WS_EX_APPWINDOW)) return false;

    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);

    /* The shell's own windows are not apps. Files and the viewers run in
     * programs of their own and are listed like any other app. */
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_own_pid) return false;

    if (wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    /* Require non-empty title or ApplicationFrameWindow */
    wchar_t title[64] = {0};
    if (GetWindowTextW(hwnd, title, 64) == 0) return false;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return false;
    if (!IsIconic(hwnd) && (rc.right - rc.left < 16 || rc.bottom - rc.top < 16)) return false;

    return true;
}

static void get_window_umid(HWND hwnd, DWORD pid, wchar_t *out, size_t out_len) {
    out[0] = L'\0';

    /* 1. Try IPropertyStore PKEY_AppUserModel_ID on the window */
    IPropertyStore *ps = NULL;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, &IID_IPropertyStore, (void **)&ps)) && ps) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_AppUserModel_ID, &pv))) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal && pv.pwszVal[0]) {
                wcsncpy_s(out, out_len, pv.pwszVal, _TRUNCATE);
            }
            PropVariantClear(&pv);
        }
        IPropertyStore_Release(ps);
    }
    if (out[0]) return;

    /* 2. Try packaged process AUMID */
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        UINT32 len = (UINT32)out_len;
        if (GetApplicationUserModelId(hProc, &len, out) != ERROR_SUCCESS) {
            out[0] = L'\0';
        }
        CloseHandle(hProc);
    }
}

static void get_process_exe(DWORD pid, wchar_t *out, DWORD out_len) {
    out[0] = L'\0';
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;
    DWORD sz = out_len;
    if (!QueryFullProcessImageNameW(hProc, 0, out, &sz)) {
        out[0] = L'\0';
    }
    CloseHandle(hProc);
}

static void friendly_name_from_exe(const wchar_t *exe, const wchar_t *fallback_title, wchar_t *out, size_t out_len) {
    if (exe && exe[0]) {
        /* The program's file name without its extension, capitalised */
        const wchar_t *fname = PathFindFileNameW(exe);
        wcsncpy_s(out, out_len, fname, _TRUNCATE);
        wchar_t *dot = wcsrchr(out, L'.');
        if (dot) *dot = L'\0';
        if (out[0] >= L'a' && out[0] <= L'z') out[0] = (wchar_t)(out[0] - L'a' + L'A');
        return;
    }
    wcsncpy_s(out, out_len, fallback_title ? fallback_title : L"", _TRUNCATE);
}

/* For UWP ApplicationFrameHost windows, find hosted child process PID */
typedef struct {
    DWORD host_pid;
    DWORD child_pid;
} UwpFindCtx;

static BOOL CALLBACK find_uwp_child_cb(HWND child, LPARAM lp) {
    UwpFindCtx *ctx = (UwpFindCtx *)lp;
    DWORD cpid = 0;
    GetWindowThreadProcessId(child, &cpid);
    if (cpid && cpid != ctx->host_pid) {
        ctx->child_pid = cpid;
        return FALSE;
    }
    return TRUE;
}

typedef struct {
    TrackedWindow *arr;
    int count;
    int max_count;
    HWND fg;
} EnumWinCtx;

static BOOL CALLBACK enum_interactable_cb(HWND hwnd, LPARAM lp) {
    EnumWinCtx *ctx = (EnumWinCtx *)lp;
    if (ctx->count >= ctx->max_count) return FALSE;
    if (!is_alt_tab_window(hwnd)) return TRUE;

    TrackedWindow *w = &ctx->arr[ctx->count];
    ZeroMemory(w, sizeof(*w));
    w->hwnd = hwnd;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w->pid = pid;

    GetWindowTextW(hwnd, w->title, 256);
    w->is_minimized = IsIconic(hwnd) != 0;
    w->is_maximized = IsZoomed(hwnd) != 0;
    w->is_focused   = (hwnd == ctx->fg);

    RECT ext;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))) {
        w->rect = ext;
    } else {
        GetWindowRect(hwnd, &w->rect);
    }

    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        UwpFindCtx uctx = { pid, 0 };
        EnumChildWindows(hwnd, find_uwp_child_cb, (LPARAM)&uctx);
        if (uctx.child_pid) {
            pid = uctx.child_pid;
            w->pid = pid;
        }
    }

    get_process_exe(pid, w->exe_path, MAX_PATH);
    get_window_umid(hwnd, pid, w->umid, 256);

    /* A pinned app keeps the name it was pinned with. */
    bool named = false;
    for (int i = 0; i < g_dock_cfg_count; i++) {
        const DockItemConfig *dc = &g_dock_cfg[i];
        if (dc->type != DOCK_ITEM_APP) continue;
        if (w->umid[0] && dc->umid[0] && _wcsicmp(w->umid, dc->umid) == 0) {
            wcsncpy_s(w->app_name, 128, dc->name, _TRUNCATE);
            named = true;
            break;
        }
        if (w->exe_path[0] && dc->command[0] && _wcsicmp(w->exe_path, dc->command) == 0) {
            wcsncpy_s(w->app_name, 128, dc->name, _TRUNCATE);
            named = true;
            break;
        }
        if (w->exe_path[0] && dc->path[0] && _wcsicmp(w->exe_path, dc->path) == 0) {
            wcsncpy_s(w->app_name, 128, dc->name, _TRUNCATE);
            named = true;
            break;
        }
    }
    if (!named) {
        friendly_name_from_exe(w->exe_path, w->title, w->app_name, 128);
    }

    ctx->count++;
    return TRUE;
}

void win_tracker_refresh(void) {
    TrackedWindow tmp[LARES_MAX_WINDOWS];
    EnumWinCtx ctx;
    ctx.arr = tmp;
    ctx.count = 0;
    ctx.max_count = LARES_MAX_WINDOWS;
    ctx.fg = GetForegroundWindow();

    EnumWindows(enum_interactable_cb, (LPARAM)&ctx);

    EnterCriticalSection(&g_win_cs);
    g_window_count = ctx.count;
    if (ctx.count > 0) {
        memcpy(g_windows, tmp, (size_t)ctx.count * sizeof(TrackedWindow));
    }
    LeaveCriticalSection(&g_win_cs);
}

#define TIMER_WIN_CHANGE 0x7A01
static bool g_change_pending = false;

static void CALLBACK change_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)msg; (void)tick;
    KillTimer(hwnd, id);
    g_change_pending = false;
    PostMessageW(hwnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
}

static void CALLBACK win_event_cb(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD dwThread, DWORD dwTime) {
    (void)hHook; (void)dwThread; (void)dwTime;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;

    /* A WinUI 3 window can push topmost windows down when it is made
     * (microsoft-ui-xaml issue 9990): put the bar and dock back on top. */
    if (event == EVENT_OBJECT_CREATE) {
        wchar_t cls[64] = {0};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0) {
            if (g_notify_hwnd) PostMessageW(g_notify_hwnd, WM_LARES_RESTORE_TOPMOST, 0, 0);
        }
    }

    if (!g_notify_hwnd) return;

    /* Busy apps raise thousands of these a second (focus, selection and state changes, child
     * windows, moves of hidden windows). Only the few that can change what the bar and the
     * dock show get through, and they are merged: however many arrive, one refresh follows. */
    UINT delay = 30;
    if (event >= EVENT_OBJECT_CREATE && event <= EVENT_OBJECT_NAMECHANGE) {
        if (event == EVENT_OBJECT_CREATE) return;                 /* nothing to show until it is shown */
        if (event >= EVENT_OBJECT_FOCUS && event <= EVENT_OBJECT_STATECHANGE) return;
        if (event != EVENT_OBJECT_DESTROY && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
        if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_OBJECT_NAMECHANGE ||
            event == EVENT_OBJECT_REORDER) {
            if (!IsWindowVisible(hwnd)) return;
            delay = 120;
        }
    }
    if (g_change_pending) return;
    g_change_pending = true;
    SetTimer(g_notify_hwnd, TIMER_WIN_CHANGE, delay, change_timer_proc);
}

void win_tracker_init(HWND notify_hwnd) {
    InitializeCriticalSection(&g_win_cs);
    g_notify_hwnd = notify_hwnd;
    g_own_pid = GetCurrentProcessId();
    win_tracker_refresh();

    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    g_hook_fg  = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, win_event_cb, 0, 0, flags);
    g_hook_min = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL, win_event_cb, 0, 0, flags);
    g_hook_obj = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE, NULL, win_event_cb, 0, 0, flags);
}

void win_tracker_shutdown(void) {
    if (g_hook_fg)  { UnhookWinEvent(g_hook_fg);  g_hook_fg = NULL; }
    if (g_hook_min) { UnhookWinEvent(g_hook_min); g_hook_min = NULL; }
    if (g_hook_obj) { UnhookWinEvent(g_hook_obj); g_hook_obj = NULL; }
    icon_cache_clear();
    DeleteCriticalSection(&g_win_cs);
}

int win_tracker_get_windows(TrackedWindow *out, int max_count) {
    EnterCriticalSection(&g_win_cs);
    int n = g_window_count;
    if (n > max_count) n = max_count;
    if (n > 0) memcpy(out, g_windows, (size_t)n * sizeof(TrackedWindow));
    LeaveCriticalSection(&g_win_cs);
    return n;
}

bool win_tracker_get_focused(TrackedWindow *out) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    EnterCriticalSection(&g_win_cs);
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i].hwnd == fg) {
            *out = g_windows[i];
            LeaveCriticalSection(&g_win_cs);
            return true;
        }
    }
    LeaveCriticalSection(&g_win_cs);

    /* Fallback if focused window is a dialog or transient child */
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == g_own_pid) return false;

    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
        ZeroMemory(out, sizeof(*out));
        out->hwnd = fg;
        wcsncpy_s(out->app_name, 128, L"Desktop", _TRUNCATE);
        return true;
    }

    ZeroMemory(out, sizeof(*out));
    out->hwnd = fg;
    out->pid = pid;
    GetWindowTextW(fg, out->title, 256);
    get_process_exe(pid, out->exe_path, MAX_PATH);
    get_window_umid(fg, pid, out->umid, 256);
    friendly_name_from_exe(out->exe_path, out->title, out->app_name, 128);
    return out->app_name[0] != L'\0';
}

bool win_tracker_is_rect_overlapped(const RECT *rc, HWND ignore1, HWND ignore2) {
    EnterCriticalSection(&g_win_cs);
    for (int i = 0; i < g_window_count; i++) {
        const TrackedWindow *w = &g_windows[i];
        if (w->is_minimized) continue;
        if (w->hwnd == ignore1 || w->hwnd == ignore2) continue;
        RECT inter;
        if (IntersectRect(&inter, rc, &w->rect)) {
            LeaveCriticalSection(&g_win_cs);
            return true;
        }
    }
    LeaveCriticalSection(&g_win_cs);
    return false;
}

bool win_tracker_is_fullscreen_active(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == g_own_pid) return false;

    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    return (wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
            wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom);
}

/* A 32-bit HBITMAP (from the shell's image factory or an icon) as a GDI+
 * bitmap of its own, with clean alpha. */
static GpBitmap *bitmap_clone_from_argb_hbitmap(HBITMAP hbm) {
    if (!hbm) return NULL;
    BITMAP bm;
    if (GetObjectW(hbm, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return NULL;

    int w = bm.bmWidth;
    int h = bm.bmHeight;
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    size_t bytes = (size_t)w * (size_t)h * 4;
    BYTE *pixels = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!pixels) return NULL;

    HDC hdc = GetDC(NULL);
    int got = GetDIBits(hdc, hbm, 0, (UINT)h, pixels, &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    if (!got) {
        HeapFree(GetProcessHeap(), 0, pixels);
        return NULL;
    }

    bool has_alpha = false;
    for (size_t i = 3; i < bytes; i += 4) {
        if (pixels[i] != 0) { has_alpha = true; break; }
    }
    if (!has_alpha) {
        for (size_t i = 3; i < bytes; i += 4) pixels[i] = 255;
    }

    GpBitmap *src = NULL;
    GpBitmap *dst = NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, GP_PIXELFORMAT_32BPP_ARGB, pixels, &src) == GP_OK && src) {
        if (GdipCreateBitmapFromScan0(w, h, 0, GP_PIXELFORMAT_32BPP_ARGB, NULL, &dst) == GP_OK && dst) {
            GpGraphics *g = NULL;
            if (GdipGetImageGraphicsContext((GpImage *)dst, &g) == GP_OK && g) {
                GdipSetCompositingMode(g, GP_COMP_SOURCE_COPY);
                GdipDrawImageRectI(g, (GpImage *)src, 0, 0, w, h);
                GdipDeleteGraphics(g);
            }
        }
        GdipDisposeImage((GpImage *)src);
    }
    HeapFree(GetProcessHeap(), 0, pixels);
    return dst;
}

static GpBitmap *load_icon_via_shell_item(const wchar_t *parse_name, int size) {
    if (!parse_name || !parse_name[0]) return NULL;
    IShellItemImageFactory *factory = NULL;
    HRESULT hr = SHCreateItemFromParsingName(parse_name, NULL, &IID_IShellItemImageFactory, (void **)&factory);
    if (FAILED(hr) || !factory) return NULL;

    SIZE sz = { size, size };
    HBITMAP hbm = NULL;
    hr = IShellItemImageFactory_GetImage(factory, sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm);
    if (FAILED(hr) || !hbm) {
        hr = IShellItemImageFactory_GetImage(factory, sz, SIIGBF_BIGGERSIZEOK, &hbm);
    }
    IShellItemImageFactory_Release(factory);
    if (FAILED(hr) || !hbm) return NULL;

    GpBitmap *bmp = bitmap_clone_from_argb_hbitmap(hbm);
    DeleteObject(hbm);
    return bmp;
}

static GpBitmap *load_icon_from_hicon(HICON hicon) {
    if (!hicon) return NULL;
    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    if (GetIconInfo(hicon, &ii)) {
        GpBitmap *bmp = NULL;
        if (ii.hbmColor) {
            bmp = bitmap_clone_from_argb_hbitmap(ii.hbmColor);
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (bmp) return bmp;
    }
    GpBitmap *fallback = NULL;
    GdipCreateBitmapFromHICON(hicon, &fallback);
    return fallback;
}

static GpBitmap *load_file_icon_bitmap(const wchar_t *path) {
    if (!path || !path[0]) return NULL;
    /* Try PNG/ICO file directly if it's an image */
    const wchar_t *ext = PathFindExtensionW(path);
    if (ext && (_wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".bmp") == 0)) {
        GpBitmap *bmp = NULL;
        if (GdipCreateBitmapFromFile(path, &bmp) == GP_OK && bmp) return bmp;
    }
    if (ext && _wcsicmp(ext, L".ico") == 0) {
        HICON hi = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 48, 48, LR_LOADFROMFILE);
        if (hi) {
            GpBitmap *bmp = load_icon_from_hicon(hi);
            DestroyIcon(hi);
            if (bmp) return bmp;
        }
    }

    /* Try IShellItemImageFactory (crisp 48px PNG-quality icons for exe, lnk, UWP) */
    GpBitmap *bmp = load_icon_via_shell_item(path, 48);
    if (bmp) return bmp;

    /* Fallback SHGetFileInfoW */
    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
        bmp = load_icon_from_hicon(sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        return bmp;
    }
    return NULL;
}

GpBitmap *icon_cache_get(const wchar_t *umid, const wchar_t *exe_path, const wchar_t *custom_rel, bool *out_square) {

    wchar_t key[300] = {0};
    _snwprintf_s(key, 300, _TRUNCATE, L"%ls|%ls|%ls",
                 custom_rel ? custom_rel : L"",
                 umid ? umid : L"",
                 exe_path ? exe_path : L"");

    for (int i = 0; i < g_icon_cache_count; i++) {
        if (_wcsicmp(g_icon_cache[i].key, key) == 0) {
            g_icon_cache[i].last_used = ++g_icon_tick;
            if (out_square) *out_square = g_icon_cache[i].is_square;
            return g_icon_cache[i].bmp;
        }
    }

    GpBitmap *bmp = NULL;
    bool is_sq = false;

    /* 1. The picture a pin names */
    if (custom_rel && custom_rel[0]) {
        wchar_t full[MAX_PATH];
        config_resolve_path(custom_rel, full, MAX_PATH);
        bmp = load_file_icon_bitmap(full);
    }

    /* 2. A Store app's icon through shell:AppsFolder\<AppUserModelID> */
    if (!bmp && umid && umid[0] && wcschr(umid, L'!') != NULL) {
        wchar_t apps_path[320];
        _snwprintf_s(apps_path, 320, _TRUNCATE, L"shell:AppsFolder\\%ls", umid);
        bmp = load_icon_via_shell_item(apps_path, 48);
    }

    /* 3. A program or shortcut file's icon */
    if (!bmp && exe_path && exe_path[0]) {
        if (_wcsicmp(exe_path, L"wt.exe") == 0) {
            wchar_t apps_path[320] = L"shell:AppsFolder\\Microsoft.WindowsTerminal_8wekyb3d8bbwe!App";
            bmp = load_icon_via_shell_item(apps_path, 48);
        }
        if (!bmp) {
            bmp = load_file_icon_bitmap(exe_path);
        }
    }

    /* Store in bounded LRU cache (evict oldest if full) */
    int slot = g_icon_cache_count;
    if (slot >= LARES_MAX_ICON_CACHE) {
        slot = 0;
        uint32_t oldest = g_icon_cache[0].last_used;
        for (int i = 1; i < LARES_MAX_ICON_CACHE; i++) {
            if (g_icon_cache[i].last_used < oldest) {
                oldest = g_icon_cache[i].last_used;
                slot = i;
            }
        }
        if (g_icon_cache[slot].bmp) {
            GdipDisposeImage((GpImage *)g_icon_cache[slot].bmp);
            g_icon_cache[slot].bmp = NULL;
        }
    } else {
        g_icon_cache_count++;
    }

    wcsncpy_s(g_icon_cache[slot].key, 300, key, _TRUNCATE);
    g_icon_cache[slot].bmp = bmp;
    g_icon_cache[slot].is_square = is_sq;
    g_icon_cache[slot].last_used = ++g_icon_tick;
    if (out_square) *out_square = is_sq;
    return bmp;
}

void icon_cache_clear(void) {
    for (int i = 0; i < g_icon_cache_count; i++) {
        if (g_icon_cache[i].bmp) {
            GdipDisposeImage((GpImage *)g_icon_cache[i].bmp);
            g_icon_cache[i].bmp = NULL;
        }
    }
    g_icon_cache_count = 0;
}

void shell_launch_app(const wchar_t *command, const wchar_t *args, const wchar_t *workdir, const wchar_t *umid) {

    if (command && command[0]) {
        SHELLEXECUTEINFOW sei;
        ZeroMemory(&sei, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open";
        sei.lpFile = command;
        sei.lpParameters = (args && args[0]) ? args : NULL;
        sei.lpDirectory  = (workdir && workdir[0]) ? workdir : NULL;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) return;
    }

    if (umid && umid[0] && wcschr(umid, L'!') != NULL) {
        wchar_t target[320];
        _snwprintf_s(target, 320, _TRUNCATE, L"shell:AppsFolder\\%ls", umid);
        ShellExecuteW(NULL, L"open", L"explorer.exe", target, NULL, SW_SHOWNORMAL);
    }
}
