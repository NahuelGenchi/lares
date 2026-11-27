// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-shell.exe (issue #145): the top bar and the dock.
 *
 * Started by lares-guard.exe (win/guard/), which owns hiding the Windows
 * taskbar: the shell says "ready" only while the dock is on, because the
 * dock is what takes the taskbar's place; the top bar alone sits above it.
 * Lares' Settings turn the parts on and off (win/src/shell_service.h).
 *
 *   lares-shell                     the parts that are on in shell.ini
 *   lares-shell --isolated          a test copy: hides nothing, reserves no
 *                                   space, its windows sit off the screen,
 *                                   no shortcuts, opens nothing
 *   lares-shell --snapshot DIR      the same, with bar and dock on: draws
 *                                   them once into DIR as PNG files and ends
 *   lares-shell --quit              asks a running shell to end cleanly
 *   lares-shell --ts-service start|stop [--ts-startup manual|auto]
 *                                   the short-lived elevated copy that starts
 *                                   or stops Tailscale's service
 *
 * LARES_SHELL_ISOLATED=1 has the same effect as --isolated.
 */
#include "shell.h"

#include "crash.h"
#include "guard_client.h"
#include "lares_version.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winsvc.h>

#define SINGLE_INSTANCE  L"Local\\LaresShell"
#define SINGLE_ISOLATED  L"Local\\LaresShellIsolated"
#define TIMER_TELEMETRY 201
#define TIMER_CLOCK     202
#define TIMER_RAM_TRIM  203

HINSTANCE       g_hInst = NULL;
HWND            g_hHostWnd = NULL;
HWND            g_hTopBarWnd = NULL;
HWND            g_hDockWnd = NULL;
AppConfig       g_cfg;
DockItemConfig  g_dock_cfg[LARES_MAX_DOCK_ITEMS];
int             g_dock_cfg_count = 0;
SystemTelemetry g_telemetry;

static UINT g_wm_taskbar_created = 0;
static UINT g_wm_reload = 0;
static WORD g_last_minute = 99;
static int  g_tel_tick = 0;
static bool g_hiding = false;   /* told the guard "ready": the Windows taskbar is hidden */
static bool g_media_on = false;

void shell_screen_rect(RECT *out) {
    HMONITOR mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) SetRect(&mi.rcMonitor, 0, 0, 1024, 768);
    *out = mi.rcMonitor;
    /* A test copy keeps the primary screen's size but lives far off it. */
    if (g_cfg.isolated) OffsetRect(out, -30000 - out->left, 0);
}

static void trim_working_set(void) {
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
}

static void set_timers(void) {
    KillTimer(g_hHostWnd, TIMER_TELEMETRY);
    KillTimer(g_hHostWnd, TIMER_CLOCK);
    if (g_cfg.topbar_enabled) {
        SetTimer(g_hHostWnd, TIMER_TELEMETRY, (UINT)g_cfg.polling_interval_ms, NULL);
        SetTimer(g_hHostWnd, TIMER_CLOCK, 1000, NULL);
    }
    SetTimer(g_hHostWnd, TIMER_RAM_TRIM, 30000, NULL);
}

/* The dock takes the Windows taskbar's place: only then is it hidden. A
 * test copy hides nothing, except a stand-in taskbar a test points the
 * guard at (LARES_GUARD_TASKBAR_CLASS). */
static void say_ready_if_docked(void) {
    wchar_t stand_in[8];
    bool may_hide = !g_cfg.isolated || GetEnvironmentVariableW(L"LARES_GUARD_TASKBAR_CLASS", stand_in, 8) > 0;
    if (g_cfg.dock_enabled && may_hide && !g_hiding) {
        lares_guard_client_ready(g_hHostWnd);
        g_hiding = true;
        SetPropW(g_hHostWnd, L"LaresShellHides", (HANDLE)1);
    }
}

static void quit_clean(void) {
    if (g_hHostWnd) DestroyWindow(g_hHostWnd);
}

static bool any_part_on(void) {
    return g_cfg.topbar_enabled || g_cfg.dock_enabled || g_cfg.launcher_enabled || g_cfg.clipboard_enabled;
}

/* Lares' Settings changed something. Parts that come on are made, parts
 * that go off are taken away; with the dock gone the taskbar has to come
 * back, which only a clean end does: Lares starts a new shell for the bar. */
static void reload(void) {
    bool had_bar = g_cfg.topbar_enabled, had_dock = g_cfg.dock_enabled;
    bool had_launcher = g_cfg.launcher_enabled, had_clipboard = g_cfg.clipboard_enabled;
    bool isolated = g_cfg.isolated;
    config_load_all();
    g_cfg.isolated = isolated;
    if (!any_part_on() || (g_hiding && !g_cfg.dock_enabled)) {
        quit_clean();
        return;
    }
    if (g_cfg.launcher_enabled && !had_launcher) launcher_init(g_hInst);
    else if (!g_cfg.launcher_enabled && had_launcher) launcher_shutdown();
    if (g_cfg.clipboard_enabled && !had_clipboard) clipboard_init(g_hInst);
    else if (!g_cfg.clipboard_enabled && had_clipboard) clipboard_shutdown();
    if (g_cfg.topbar_enabled && !had_bar) topbar_create(g_hInst);
    else if (!g_cfg.topbar_enabled && had_bar) topbar_destroy();
    else topbar_apply_settings();

    if (g_cfg.dock_enabled && !had_dock) dock_create(g_hInst);
    else dock_apply_settings();

    if (g_cfg.dock_enabled && g_cfg.dock_show_media_widget && !g_media_on) {
        media_init(g_hHostWnd);
        g_media_on = true;
    }
    if (!g_cfg.isolated) {
        hotkeys_shutdown();
        hotkeys_init(g_hHostWnd);
    }
    say_ready_if_docked();
    set_timers();
}

static LRESULT CALLBACK host_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (lares_guard_client_message(hwnd, msg, wParam, lParam)) return 0;

    if (g_wm_taskbar_created && msg == g_wm_taskbar_created) {
        topbar_on_explorer_restart();
        dock_on_explorer_restart();
        return 0;
    }
    if (g_wm_reload && msg == g_wm_reload) {
        reload();
        return 0;
    }

    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_TELEMETRY) {
            g_tel_tick++;
            telemetry_update(true);
            if ((g_tel_tick % 15) == 0) telemetry_refresh_tailscale_async();
            topbar_repaint();
            popups_refresh_if_open();
        } else if (wParam == TIMER_CLOCK) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wMinute != g_last_minute) {
                g_last_minute = st.wMinute;
                topbar_repaint();
                popups_refresh_if_open();
            } else if (popups_is_open()) {
                popups_refresh_if_open();
            }
        } else if (wParam == TIMER_RAM_TRIM) {
            if (!popups_is_open()) trim_working_set();
        }
        return 0;

    case WM_LARES_WINDOWS_CHANGED:
        win_tracker_refresh();
        topbar_on_windows_changed();
        topbar_repaint();
        dock_on_windows_changed();
        return 0;

    case WM_LARES_TELEMETRY_UPDATED:
        topbar_repaint();
        popups_refresh_if_open();
        return 0;

    case WM_LARES_MEDIA_UPDATED:
        dock_repaint();
        popups_refresh_if_open();
        return 0;

    case WM_LARES_TAILSCALE_UPDATED:
        topbar_repaint();
        popups_on_tailscale_updated();
        return 0;

    case WM_LARES_BT_SCAN_DONE:
        popups_on_bt_scan_done();
        return 0;

    case WM_LARES_RESTORE_TOPMOST:
        topbar_reassert_topmost();
        dock_reassert_topmost();
        return 0;

    case WM_LARES_DOCK_SHORTCUT:
        dock_activate_index((int)wParam);
        return 0;

    case WM_LARES_TOGGLE_LAUNCHER:
        if (!g_cfg.launcher_enabled) return 0;
        popups_close();
        if (g_cfg.clipboard_enabled) clipboard_hide();
        launcher_toggle();
        return 0;

    case WM_LARES_TOGGLE_CLIPBOARD:
        if (!g_cfg.clipboard_enabled) return 0;
        popups_close();
        if (g_cfg.launcher_enabled) launcher_hide();
        clipboard_toggle();
        return 0;

    case WM_HOTKEY:
        if (wParam == LARES_HOTKEY_LAUNCHER) PostMessageW(hwnd, WM_LARES_TOGGLE_LAUNCHER, 0, 0);
        else if (wParam == LARES_HOTKEY_CLIPBOARD) PostMessageW(hwnd, WM_LARES_TOGGLE_CLIPBOARD, 0, 0);
        return 0;

    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
        theme_reload();
        topbar_repaint();
        dock_repaint();
        popups_refresh_if_open();
        if (g_cfg.launcher_enabled) launcher_on_theme_changed();
        if (g_cfg.clipboard_enabled) clipboard_on_theme_changed();
        return 0;

    case WM_DISPLAYCHANGE:
        topbar_on_display_change();
        dock_on_display_change();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_LARES_QUIT_CLEAN:
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TELEMETRY);
        KillTimer(hwnd, TIMER_CLOCK);
        KillTimer(hwnd, TIMER_RAM_TRIM);
        RemovePropW(hwnd, L"LaresShellHides");
        /* A clean end: the guard gives the taskbar back and leaves too. */
        lares_guard_client_quit();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool env_on(const wchar_t *name) {
    wchar_t v[8];
    DWORD n = GetEnvironmentVariableW(name, v, 8);
    return n > 0 && n < 8 && v[0] && wcscmp(v, L"0") != 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)nShow; (void)lpCmd;
    g_hInst = hInst;
    lares_crash_install("lares-shell", LARES_VERSION);

    bool quit_now = false;
    bool isolated = env_on(L"LARES_SHELL_ISOLATED");
    wchar_t snapshot_dir[MAX_PATH] = {0};

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && _wcsicmp(argv[1], L"--ts-service") == 0) {
        /* Touches the "Tailscale" service only, and nothing else. */
        bool start = (_wcsicmp(argv[2], L"start") == 0);
        bool stop  = (_wcsicmp(argv[2], L"stop") == 0);
        int startup = 0;
        if (argc >= 5 && _wcsicmp(argv[3], L"--ts-startup") == 0) {
            if (_wcsicmp(argv[4], L"manual") == 0) startup = SERVICE_DEMAND_START;
            else if (_wcsicmp(argv[4], L"auto") == 0) startup = SERVICE_AUTO_START;
        }
        LocalFree(argv);
        if (!start && !stop) return ERROR_INVALID_PARAMETER;
        return telemetry_ts_service_helper(start, startup);
    }
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--quit") == 0) quit_now = true;
        else if (_wcsicmp(argv[i], L"--isolated") == 0) isolated = true;
        else if (_wcsicmp(argv[i], L"--snapshot") == 0 && i + 1 < argc) {
            wcsncpy_s(snapshot_dir, MAX_PATH, argv[++i], _TRUNCATE);
            isolated = true;
        }
    }
    if (argv) LocalFree(argv);

    const wchar_t *host_class = isolated ? LARES_SHELL_HOST_CLASS_ISOLATED : LARES_SHELL_HOST_CLASS;
    if (quit_now) {
        HWND existing = FindWindowW(host_class, NULL);
        if (existing) PostMessageW(existing, WM_LARES_QUIT_CLEAN, 0, 0);
        return 0;
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, isolated ? SINGLE_ISOLATED : SINGLE_INSTANCE);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        /* Another shell runs: this one is not needed, and is no crash. */
        lares_guard_client_quit();
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    ZeroMemory(&g_cfg, sizeof(g_cfg));
    config_load_all();
    g_cfg.isolated = isolated;
    if (snapshot_dir[0]) {
        g_cfg.topbar_enabled = true;
        g_cfg.dock_enabled = true;
        g_cfg.launcher_enabled = true;
        g_cfg.clipboard_enabled = true;
        g_cfg.animations = false;
        g_cfg.topbar_autohide = false;
        g_cfg.dock_hide_mode = HIDE_NEVER;
    }
    if (!any_part_on()) {
        /* Nothing is on: a clean end, so the guard does not start it again. */
        lares_guard_client_quit();
        CoUninitialize();
        CloseHandle(mutex);
        return 0;
    }

    if (!render_init()) {
        lares_guard_client_quit();
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }
    {
        /* LARES_COLOR_SCHEME=light or dark picks a palette, as for Lares' window. */
        wchar_t scheme[8];
        DWORD n = GetEnvironmentVariableW(L"LARES_COLOR_SCHEME", scheme, 8);
        if (n > 0 && n < 8) {
            if (_wcsicmp(scheme, L"dark") == 0) theme_apply(true, RGB(0, 103, 192));
            else if (_wcsicmp(scheme, L"light") == 0) theme_apply(false, RGB(0, 103, 192));
        }
    }

    g_wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_wm_reload = RegisterWindowMessageW(LARES_SHELL_RELOAD_MESSAGE);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = host_wnd_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = host_class;
    RegisterClassExW(&wc);

    g_hHostWnd = CreateWindowExW(0, host_class, L"Lares shell", 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hHostWnd) {
        render_shutdown();
        lares_guard_client_quit();
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }
    /* Lares' window and Settings post the reload message here, from a
     * process of the same user: let it through. */
    ChangeWindowMessageFilterEx(g_hHostWnd, g_wm_reload, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_hHostWnd, WM_LARES_QUIT_CLEAN, MSGFLT_ALLOW, NULL);

    win_tracker_init(g_hHostWnd);
    telemetry_init(g_hHostWnd);
    if (g_cfg.dock_enabled && g_cfg.dock_show_media_widget && !snapshot_dir[0]) {
        media_init(g_hHostWnd);
        g_media_on = true;
    }
    popups_init(hInst);
    if (g_cfg.launcher_enabled) launcher_init(hInst);
    if (g_cfg.clipboard_enabled) clipboard_init(hInst);
    if (g_cfg.topbar_enabled) topbar_create(hInst);
    if (g_cfg.dock_enabled) dock_create(hInst);
    if (!g_cfg.isolated) hotkeys_init(g_hHostWnd);

    if (snapshot_dir[0]) {
        /* Drawn once with readings taken now, saved, and gone. */
        telemetry_update(true);
        topbar_repaint();
        dock_repaint();
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\top-bar.png", snapshot_dir);
        int ok = topbar_snapshot(path);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\dock.png", snapshot_dir);
        ok &= dock_snapshot(path);
        static const struct { PopupKind kind; const wchar_t *name; } pops[] = {
            { POPUP_CALENDAR, L"calendar" }, { POPUP_KEYBOARD, L"keyboard" },
            { POPUP_BLUETOOTH, L"bluetooth" }, { POPUP_NETWORK, L"network" },
            { POPUP_MEDIA, L"media" }, { POPUP_NOTIFICATIONS, L"notifications" },
            { POPUP_POWER, L"power" }, { POPUP_SCREENSHOT, L"screenshot" },
        };
        for (int i = 0; i < (int)(sizeof(pops) / sizeof(pops[0])); ++i) {
            popups_toggle(pops[i].kind, 400, 40);
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\popup-%ls.png", snapshot_dir, pops[i].name);
            ok &= popups_snapshot(path);
            popups_close();
        }
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\launcher.png", snapshot_dir);
        ok &= launcher_snapshot(NULL, path);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\launcher-search.png", snapshot_dir);
        ok &= launcher_snapshot(L"set", path);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\launcher-sum.png", snapshot_dir);
        ok &= launcher_snapshot(L"sqrt(144)*2", path);
        clipboard_add_sample(L"Meeting at ten, room 4", false);
        clipboard_add_sample(L"https://example.com/notes", false);
        clipboard_add_sample(L"A pinned note\nwith a second line", true);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\clipboard.png", snapshot_dir);
        ok &= clipboard_snapshot(path);
        DestroyWindow(g_hHostWnd);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        clipboard_shutdown();
        launcher_shutdown();
        popups_shutdown();
        dock_destroy();
        topbar_destroy();
        if (g_media_on) media_shutdown();
        telemetry_shutdown();
        win_tracker_shutdown();
        render_shutdown();
        CoUninitialize();
        CloseHandle(mutex);
        return ok ? 0 : 1;
    }

    say_ready_if_docked();
    set_timers();
    trim_working_set();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    hotkeys_shutdown();
    if (g_cfg.clipboard_enabled) clipboard_shutdown();
    if (g_cfg.launcher_enabled) launcher_shutdown();
    popups_shutdown();
    dock_destroy();
    topbar_destroy();
    if (g_media_on) media_shutdown();
    telemetry_shutdown();
    win_tracker_shutdown();
    render_shutdown();
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
