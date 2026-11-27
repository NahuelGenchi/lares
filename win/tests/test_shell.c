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
 * `--no-ready` never says ready; `--crash-after MS` crashes by itself.
 */
#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#include "guard_client.h"

#define SHELL_WINDOW_CLASS L"LaresTestShell"
#define SHELL_QUIT (WM_APP + 1)
#define SHELL_CRASH (WM_APP + 2)
#define SHELL_HANG (WM_APP + 3)
#define SHELL_LEAVE (WM_APP + 4)

/* What a real Lares program does on a crash: end at once, so that whoever
 * waits on the process is not kept waiting by Windows' own crash handling. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *info)
{
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

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
        crash_now();
        return 0;
    default:
        return DefWindowProcW(window, message, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    (void)previous, (void)show;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(on_crash);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = shell_window;
    wc.hInstance = instance;
    wc.lpszClassName = SHELL_WINDOW_CLASS;
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, SHELL_WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance,
                                  NULL);
    if (!window) return 2;

    const wchar_t *after = wcsstr(command, L"--crash-after");
    if (after) SetTimer(window, 1, (UINT)wcstoul(after + 13, NULL, 10), NULL);
    if (!wcsstr(command, L"--no-ready")) lares_guard_client_ready(window);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
