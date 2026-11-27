/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hiding the Windows taskbar and giving it back.
 *
 * Written only from Microsoft's public documentation:
 *
 *   SHAppBarMessage, ABM_GETSTATE and ABM_SETSTATE (the taskbar's auto-hide
 *   state, and that it is the taskbar's own setting, kept by Windows):
 *     https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-shappbarmessage
 *     https://learn.microsoft.com/windows/win32/shell/abm-getstate
 *     https://learn.microsoft.com/windows/win32/shell/abm-setstate
 *     https://learn.microsoft.com/windows/win32/api/shellapi/ns-shellapi-appbardata
 *   FindWindowW and FindWindowExW (finding a top-level window by its class):
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-findwindoww
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-findwindowexw
 *   ShowWindowAsync and IsWindowVisible (showing another program's window
 *   without waiting for it):
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-showwindowasync
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-iswindowvisible
 *   GetPropW and SetPropW (the stand-in taskbar the tests use):
 *     https://learn.microsoft.com/windows/win32/winmsg/about-window-properties
 *
 * Microsoft has no reference page for the taskbar's window classes. The two
 * names used here, "Shell_TrayWnd" for the main taskbar and
 * "Shell_SecondaryTrayWnd" for the one on each further screen, are the ones
 * Microsoft's own articles and answers use:
 *     https://learn.microsoft.com/archive/msdn-magazine/2002/september/c-q-a-typename-disabling-keys-in-windows-xp-with-trapkeys
 *     https://learn.microsoft.com/answers/questions/1669229/how-to-programmatically-display-the-windows-taskba
 * If a later Windows renames them, nothing is found, nothing is hidden and
 * the guard has nothing to give back.
 */
#include "taskbar.h"

#include <shellapi.h>

#define MAIN_CLASS L"Shell_TrayWnd"
#define SECONDARY_CLASS L"Shell_SecondaryTrayWnd"
#define TEST_AUTOHIDE_PROP L"LaresTestAutoHide"

static wchar_t test_class[64];

typedef void (*each_fn)(HWND window, void *ctx);

void lares_taskbar_use_test_class(const wchar_t *window_class)
{
    test_class[0] = 0;
    if (window_class) lstrcpynW(test_class, window_class, ARRAYSIZE(test_class));
}

/* Every taskbar is found first and acted on after, so that showing or hiding
 * one cannot make the search skip the next. */
#define MAX_TASKBARS 16

static void each_taskbar(each_fn fn, void *ctx)
{
    HWND found[MAX_TASKBARS], w = NULL;
    int n = 0;
    if (test_class[0]) {
        while (n < MAX_TASKBARS && (w = FindWindowExW(NULL, w, test_class, NULL)) != NULL) found[n++] = w;
    } else {
        w = FindWindowW(MAIN_CLASS, NULL);
        if (w) found[n++] = w;
        w = NULL;
        while (n < MAX_TASKBARS && (w = FindWindowExW(NULL, w, SECONDARY_CLASS, NULL)) != NULL) found[n++] = w;
    }
    for (int i = 0; i < n; ++i) fn(found[i], ctx);
}

static HWND main_taskbar(void)
{
    if (test_class[0]) return FindWindowExW(NULL, NULL, test_class, NULL);
    return FindWindowW(MAIN_CLASS, NULL);
}

/* The stand-in's auto-hide is one setting for all its taskbars, as Windows'
 * is: a property of one more window, never shown, of the class named after
 * the taskbars' with "AutoHide" added. */
static HWND test_setting_window(void)
{
    wchar_t name[80];
    lstrcpyW(name, test_class);
    lstrcatW(name, L"AutoHide");
    return FindWindowW(name, NULL);
}

static void count_one(HWND window, void *ctx)
{
    (void)window;
    ++*(int *)ctx;
}

static void count_visible(HWND window, void *ctx)
{
    if (IsWindowVisible(window)) ++*(int *)ctx;
}

int lares_taskbar_count(void)
{
    int n = 0;
    each_taskbar(count_one, &n);
    return n;
}

int lares_taskbar_visible_count(void)
{
    int n = 0;
    each_taskbar(count_visible, &n);
    return n;
}

static void show_one(HWND window, void *ctx)
{
    int show = *(int *)ctx;
    if ((IsWindowVisible(window) != 0) != (show != 0))
        ShowWindowAsync(window, show ? SW_SHOWNA : SW_HIDE);
}

void lares_taskbar_show(int show)
{
    each_taskbar(show_one, &show);
}

/* shell32 is loaded only when the real taskbar is asked, so the tests' guard
 * and a guard with nothing to hide never pay for it. */
typedef UINT_PTR (WINAPI *appbar_fn)(DWORD message, PAPPBARDATA data);

static appbar_fn appbar(void)
{
    static appbar_fn fn;
    if (!fn) {
        HMODULE shell32 = LoadLibraryExW(L"shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shell32) fn = (appbar_fn)GetProcAddress(shell32, "SHAppBarMessage");
    }
    return fn;
}

int lares_taskbar_get_autohide(void)
{
    HWND w = main_taskbar();
    if (!w) return -1;
    if (test_class[0]) {
        HWND setting = test_setting_window();
        return setting ? ((INT_PTR)GetPropW(setting, TEST_AUTOHIDE_PROP) == 2 ? 1 : 0) : -1;
    }

    appbar_fn fn = appbar();
    if (!fn) return -1;
    APPBARDATA d;
    ZeroMemory(&d, sizeof d);
    d.cbSize = sizeof d;
    d.hWnd = w;
    return (fn(ABM_GETSTATE, &d) & ABS_AUTOHIDE) ? 1 : 0;
}

void lares_taskbar_set_autohide(int on)
{
    HWND w = main_taskbar();
    if (!w) return;
    if (test_class[0]) {
        HWND setting = test_setting_window();
        if (setting) SetPropW(setting, TEST_AUTOHIDE_PROP, (HANDLE)(INT_PTR)(on ? 2 : 1));
        return;
    }
    appbar_fn fn = appbar();
    if (!fn) return;
    APPBARDATA d;
    ZeroMemory(&d, sizeof d);
    d.cbSize = sizeof d;
    d.hWnd = w;
    /* Only the auto-hide bit changes; whatever else the state holds stays. */
    UINT_PTR state = fn(ABM_GETSTATE, &d);
    d.lParam = (LPARAM)((state & ~(UINT_PTR)ABS_AUTOHIDE) | (on ? ABS_AUTOHIDE : 0));
    fn(ABM_SETSTATE, &d);
}

DWORD lares_taskbar_process(void)
{
    DWORD pid = 0;
    HWND w = main_taskbar();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid;
}

int lares_taskbar_is(HWND window)
{
    wchar_t name[64];
    if (!GetClassNameW(window, name, ARRAYSIZE(name))) return 0;
    if (test_class[0]) return lstrcmpiW(name, test_class) == 0;
    return lstrcmpiW(name, MAIN_CLASS) == 0 || lstrcmpiW(name, SECONDARY_CLASS) == 0;
}
