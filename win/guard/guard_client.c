/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The shell's side of lares-guard (guard_client.h).
 *
 * Written only from Microsoft's public documentation:
 *
 *   The "TaskbarCreated" message Explorer sends to every top-level window
 *   when it makes its taskbar again ("Taskbar Creation Notification"):
 *     https://learn.microsoft.com/windows/win32/shell/taskbar
 *   WM_DISPLAYCHANGE and WM_ENDSESSION:
 *     https://learn.microsoft.com/windows/win32/gdi/wm-displaychange
 *     https://learn.microsoft.com/windows/win32/shutdown/wm-endsession
 *   SetWinEventHook and EVENT_OBJECT_SHOW (a taskbar shown again by Explorer):
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwineventhook
 *     https://learn.microsoft.com/windows/win32/winauto/event-constants
 *   SetTimer and event objects:
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-settimer
 *     https://learn.microsoft.com/windows/win32/sync/event-objects
 */
#include "guard_client.h"

#include "state.h"
#include "taskbar.h"
#include "taskbar_journal.h"

#define ALIVE_TIMER 0x4C47
#define ALIVE_EVERY_MS 2000

static struct {
    HANDLE alive;
    HWINEVENTHOOK hook;
    UINT taskbar_created;
    int hiding; /* this shell asked for the taskbar to be hidden */
    wchar_t dir[MAX_PATH];
} c;

static HANDLE guard_event(const wchar_t *variable)
{
    wchar_t name[128];
    DWORD n = GetEnvironmentVariableW(variable, name, ARRAYSIZE(name));
    if (n == 0 || n >= ARRAYSIZE(name)) return NULL;
    return OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
}

static void signal_guard(const wchar_t *variable)
{
    HANDLE event = guard_event(variable);
    if (!event) return;
    SetEvent(event);
    CloseHandle(event);
}

/* Hides a taskbar that came back, but only while the guard's file says
 * "hidden": the guard decides, this only repeats it. */
static void hide_again(void)
{
    lares_guard_state s;
    lares_journal journal;
    lares_guard_state_load(c.dir, &s);
    if (s.hidden != LARES_TASKBAR_HIDDEN) return;
    lares_taskbar_show(0);
    /* Auto-hide that the journal says Lares switched on, and that a new
     * Explorer came up without: on again, with no new entry. */
    if (lares_taskbar_get_autohide() == 0 && lares_shell_journal(&journal) && lares_shell_autohide_owed(&journal))
        lares_taskbar_set_autohide(1);
}

static void CALLBACK on_shown(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread,
                              DWORD time)
{
    (void)hook, (void)event, (void)child, (void)thread, (void)time;
    if (object != OBJID_WINDOW || !window || !c.hiding) return;
    if (lares_taskbar_is(window)) hide_again();
}

static void unwatch(void)
{
    if (c.hook) UnhookWinEvent(c.hook);
    c.hook = NULL;
}

/* Only the taskbar's own process is listened to, and only for windows being
 * shown. */
static void watch(void)
{
    DWORD pid = lares_taskbar_process();
    unwatch();
    if (pid) c.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, on_shown, pid, 0, WINEVENT_OUTOFCONTEXT);
}

void lares_guard_client_ready(HWND window)
{
    wchar_t test_class[64];
    if (c.hiding || !lares_guard_state_dir(c.dir, ARRAYSIZE(c.dir))) return;
    c.alive = guard_event(L"LARES_GUARD_ALIVE");
    if (!c.alive) return; /* no guard: leave the Windows taskbar alone */
    if (GetEnvironmentVariableW(L"LARES_GUARD_TASKBAR_CLASS", test_class, ARRAYSIZE(test_class)) > 0)
        lares_taskbar_use_test_class(test_class);

    c.hiding = 1;
    c.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    SetEvent(c.alive);
    SetTimer(window, ALIVE_TIMER, ALIVE_EVERY_MS, NULL);
    watch();
    signal_guard(L"LARES_GUARD_READY");
}

int lares_guard_client_message(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (!c.hiding) return 0;
    if (message == WM_TIMER && wp == ALIVE_TIMER) {
        SetEvent(c.alive);
        return 1;
    }
    if (message == WM_DISPLAYCHANGE || message == c.taskbar_created) {
        hide_again();
        watch(); /* a new Explorer is a new process */
        return 0; /* the shell has its own use for these */
    }
    if (message == WM_ENDSESSION && wp) {
        /* Signing out. The taskbar's auto-hide is Windows' own setting and
         * outlives the session, so it goes back now, from this process:
         * nothing new can be started this late. */
        lares_guard_state s;
        lares_journal journal;
        KillTimer(window, ALIVE_TIMER);
        unwatch();
        c.hiding = 0;
        lares_guard_state_load(c.dir, &s);
        /* Auto-hide first: it is the part that would outlive the session. */
        if (lares_shell_journal(&journal)) lares_shell_autohide_back(&journal);
        if (s.hidden) {
            s.hidden = LARES_TASKBAR_GIVING_BACK;
            lares_guard_state_save(c.dir, &s);
            lares_taskbar_show(1);
            s.hidden = 0;
            lares_guard_state_save(c.dir, &s);
        }
        signal_guard(L"LARES_GUARD_QUIT");
        return 0;
    }
    return 0;
}

void lares_guard_client_quit(void)
{
    unwatch();
    c.hiding = 0;
    signal_guard(L"LARES_GUARD_QUIT");
}
