/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * lares-test-shell: a stand-in for Lares' bar, for tests only. It is never
 * installed. It has one window that is never shown, talks to the guard the
 * way the real bar will (guard_client.h), and does what it is told with a
 * message:
 *
 *   WM_APP + 1   quit cleanly (says so to the guard first)
 *   WM_APP + 2   crash (writes through a null pointer)
 *   WM_APP + 3   hang (stops answering messages)
 *   WM_APP + 4   leave with no word to the guard
 *
 * `--no-ready` never says ready; `--ready-after MS` says it late, as a bar
 * that takes a moment to draw would; `--crash-after MS` crashes by itself.
 */
#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#include "crash.h"
#include "guard_client.h"
#include "lares_version.h"

#define SHELL_WINDOW_CLASS L"LaresTestShell"
#define SHELL_QUIT (WM_APP + 1)
#define SHELL_CRASH (WM_APP + 2)
#define SHELL_HANG (WM_APP + 3)
#define SHELL_LEAVE (WM_APP + 4)
#define CRASH_TIMER 1
#define READY_TIMER 2

static void crash_now(void)
{
    volatile int *nowhere = NULL;
    *nowhere = 1;
}

static LRESULT CALLBACK shell_window(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (lares_guard_client_message(window, message, wp, lp)) return 0;
    switch (message) {
    case SHELL_QUIT:
        lares_guard_client_quit();
        PostQuitMessage(0);
        return 0;
    case SHELL_CRASH:
        crash_now();
        return 0;
    case SHELL_HANG:
        Sleep(INFINITE);
        return 0;
    case SHELL_LEAVE:
        ExitProcess(1);
    case WM_TIMER:
        if (wp == READY_TIMER) {
            KillTimer(window, READY_TIMER);
            lares_guard_client_ready(window);
        } else {
            crash_now();
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    (void)previous, (void)show;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    /* What every Lares program does: a crash writes a local report and
     * ends the process at once (win/crash/crash.h). */
    lares_crash_install("lares-test-shell", LARES_VERSION);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = shell_window;
    wc.hInstance = instance;
    wc.lpszClassName = SHELL_WINDOW_CLASS;
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, SHELL_WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance,
                                  NULL);
    if (!window) return 2;

    const wchar_t *after = wcsstr(command, L"--crash-after");
    if (after) SetTimer(window, CRASH_TIMER, (UINT)wcstoul(after + 13, NULL, 10), NULL);
    after = wcsstr(command, L"--ready-after");
    if (after) SetTimer(window, READY_TIMER, (UINT)wcstoul(after + 13, NULL, 10), NULL);
    else if (!wcsstr(command, L"--no-ready")) lares_guard_client_ready(window);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
