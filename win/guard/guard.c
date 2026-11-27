/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * lares-guard: the watchdog that gives the Windows taskbar back.
 *
 *   lares-guard run <shell program and its arguments>
 *   lares-guard restore
 *
 * `run` starts the shell and owns the fact "the Windows taskbar is hidden".
 * The shell says "I am ready" and the guard hides the taskbar; it says "I am
 * quitting" and the guard gives it back and leaves. If the shell ends in any
 * other way (a crash, a kill, a hang the guard has to end), the guard gives
 * the taskbar back at once and starts the shell again. Two such endings
 * inside five minutes: it stops, leaves the Windows taskbar on screen and
 * asks once.
 *
 * The guard can be killed too, so three things stand behind it:
 *
 *   1. A second process, the twin (`lares-guard twin`), waits on the guard.
 *      If the guard ends without having said "stopped" in the state file, the
 *      twin gives the taskbar back and starts a new guard. The guard waits on
 *      the twin and starts a new one if it ends.
 *   2. The shell runs in a job object that ends it when the guard's handle
 *      closes, so a shell never runs on with nobody watching it.
 *   3. The state file (state.h) is written and flushed before the taskbar is
 *      touched. Whatever starts next (a guard, or `lares-guard restore`)
 *      gives the taskbar back from it.
 *
 * Small on purpose. The guard and the twin wait in one call on process
 * handles and use kernel32 alone: no window, no polling. Everything that
 * needs Windows' window library (hiding, giving back, the message) runs in a
 * short-lived copy of this program (`lares-guard do-hide`, `do-restore`,
 * `do-ask`), so the two that stay never load it; user32 and shell32 are
 * delay-loaded for that. If such a copy cannot be started, the guard does
 * the work itself.
 *
 * What the shell sees (guard_client.h does all of it): three environment
 * variables that name three events. LARES_GUARD_READY, set: "ready, hide the
 * taskbar". LARES_GUARD_QUIT, set just before leaving: "this is a clean
 * quit". LARES_GUARD_ALIVE, set every two seconds from the thread that runs
 * the bar: "still answering". While the taskbar is hidden the guard's wait
 * ends every four seconds to look at that last one; a shell that began
 * saying it and then misses two looks is ended, and counts as a crash.
 *
 * Written only from Microsoft's public documentation. The taskbar's pages are
 * listed in taskbar.c, the shell's side in guard_client.c; the rest:
 *
 *   Job objects, and ending a job's processes when its last handle closes:
 *     https://learn.microsoft.com/windows/win32/procthread/job-objects
 *     https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information
 *   WaitForMultipleObjects on process and event handles:
 *     https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects
 *   SM_SHUTTINGDOWN (the session is ending):
 *     https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getsystemmetrics
 *   TaskDialogIndirect (the one message):
 *     https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect
 *   Delay-loaded DLLs:
 *     https://learn.microsoft.com/cpp/build/reference/linker-support-for-delay-loaded-dlls
 */
#include <windows.h>

#include <commctrl.h>

#include "state.h"
#include "taskbar.h"

#define LOOK_MS 4000
#define MISSED_LOOKS 2
#define CHILD_WAIT_MS 15000
#define EXIT_KEPT_WINDOWS_TASKBAR 3
#define EXIT_HUNG_SHELL 0xE0000001u
/* What the short-lived copies answer with. */
#define DONE 10
#define DONE_SESSION_ENDING 11
#define ANSWER_KEEP 20
#define ANSWER_TRY_AGAIN 21

static struct {
    wchar_t dir[MAX_PATH];
    wchar_t self[MAX_PATH];
    wchar_t command[2048];  /* what follows "run": the shell and its arguments */
    wchar_t scratch[2600];  /* a command line being put together */
    lares_guard_state st;   /* running, the three processes and the crashes are the guard's to write */
    HANDLE job, shell, twin, ready, quit, alive;
    int shell_ready, heard_alive, missed;
} g;

/* ---- small things the C runtime would have given -------------------- */

#ifdef LARES_NO_CRT
#include <intrin.h>
#pragma function(memset, memcpy)
void *memset(void *to, int value, size_t n)
{
#if defined(_M_X64) || defined(_M_IX86)
    __stosb((unsigned char *)to, (unsigned char)value, n);
#else
    volatile unsigned char *p = (volatile unsigned char *)to;
    while (n--) *p++ = (unsigned char)value;
#endif
    return to;
}
void *memcpy(void *to, const void *from, size_t n)
{
#if defined(_M_X64) || defined(_M_IX86)
    __movsb((unsigned char *)to, (const unsigned char *)from, n);
#else
    volatile unsigned char *p = (volatile unsigned char *)to;
    const unsigned char *q = (const unsigned char *)from;
    while (n--) *p++ = *q++;
#endif
    return to;
}

/* user32 is delay-loaded, and the linker's own helper for that needs the C
 * runtime. This is the same helper, written as Microsoft describes:
 *   https://learn.microsoft.com/cpp/build/reference/developing-your-own-helper-function
 * The first call to a user32 function lands here; it loads the library,
 * looks the function up and stores it, so later calls go straight there. */
#include <delayimp.h>
extern IMAGE_DOS_HEADER __ImageBase;

FARPROC WINAPI __delayLoadHelper2(PCImgDelayDescr library, FARPROC *slot)
{
    BYTE *base = (BYTE *)&__ImageBase;
    HMODULE *loaded = (HMODULE *)(base + library->rvaHmod);
    FARPROC *slots = (FARPROC *)(base + library->rvaIAT);
    const IMAGE_THUNK_DATA *names = (const IMAGE_THUNK_DATA *)(base + library->rvaINT);
    const IMAGE_THUNK_DATA *name = names + (slot - slots);
    FARPROC found;
    DWORD old;

    if (!*loaded) *loaded = LoadLibraryExA((const char *)(base + library->rvaDLLName), NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!*loaded) ExitProcess(2);
    if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal))
        found = GetProcAddress(*loaded, (const char *)IMAGE_ORDINAL(name->u1.Ordinal));
    else
        found = GetProcAddress(*loaded, ((const IMAGE_IMPORT_BY_NAME *)(base + name->u1.AddressOfData))->Name);
    if (!found) ExitProcess(2);
    if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
        *slot = found;
        VirtualProtect(slot, sizeof *slot, old, &old);
    }
    return found;
}
#endif

static const wchar_t *skip_spaces(const wchar_t *p)
{
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

/* Steps over one argument of a command line, quoted or not. */
static const wchar_t *skip_argument(const wchar_t *p)
{
    int quoted = 0;
    for (; *p; ++p) {
        if (*p == L'"') quoted = !quoted;
        else if (!quoted && (*p == L' ' || *p == L'\t')) break;
    }
    return skip_spaces(p);
}

/* Is the argument at `p` this word? Then `rest` is what follows it. */
static int take_word(const wchar_t *p, const wchar_t *word, const wchar_t **rest)
{
    int i = 0;
    while (word[i] && p[i] == word[i]) ++i;
    if (word[i] || (p[i] && p[i] != L' ' && p[i] != L'\t')) return 0;
    *rest = skip_spaces(p + i);
    return 1;
}

static DWORD take_number(const wchar_t *p, const wchar_t **rest)
{
    DWORD v = 0;
    while (*p >= L'0' && *p <= L'9') v = v * 10 + (DWORD)(*p++ - L'0');
    *rest = skip_spaces(p);
    return v;
}

static void append_number(wchar_t *text, DWORD value)
{
    wchar_t digits[12];
    int n = 0, at = lstrlenW(text);
    do {
        digits[n++] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    } while (value);
    while (n) text[at++] = digits[--n];
    text[at] = 0;
}

/* The taskbar's two lines in the file belong to whoever hides and gives
 * back; the guard keeps them as they are and writes its own around them. */
static void save_state(void)
{
    lares_guard_state on_disk;
    lares_guard_state_load(g.dir, &on_disk);
    g.st.hidden = on_disk.hidden;
    g.st.autohide_before = on_disk.autohide_before;
    lares_guard_state_save(g.dir, &g.st);
}

/* ---- the work that needs user32: done here by the short-lived copies,
 *      by the twin once the guard is gone, and by the guard itself only
 *      when a copy cannot be started -------------------------------------- */

static void do_hide(void)
{
    lares_guard_state s;
    lares_guard_state_load(g.dir, &s);
    if (!s.hidden) {
        /* The file says "hidden" before anything is hidden. */
        s.autohide_before = lares_taskbar_get_autohide();
        s.hidden = 1;
        lares_guard_state_save(g.dir, &s);
    }
    lares_taskbar_show(0);
    if (s.autohide_before == 0 && lares_taskbar_get_autohide() == 0) lares_taskbar_set_autohide(1);
    lares_taskbar_show(0);
}

static int do_restore(void)
{
    lares_guard_state s;
    lares_guard_state_load(g.dir, &s);
    if (s.hidden) {
        lares_taskbar_show(1);
        if (s.autohide_before == 0) lares_taskbar_set_autohide(0);
        s.hidden = 0;
        lares_guard_state_save(g.dir, &s);
    }
    return GetSystemMetrics(SM_SHUTTINGDOWN) ? DONE_SESSION_ENDING : DONE;
}

static const wchar_t kTitle[] = L"Lares";
static const wchar_t kHeading[] = L"Lares' bar stopped twice in a row";
static const wchar_t kBody[] =
    L"Windows' own taskbar is back, so nothing is lost. "
    L"You can try Lares' bar again, or keep Windows' taskbar for now.";
static const wchar_t kBodyPlain[] =
    L"Lares' bar stopped twice in a row. Windows' own taskbar is back, so nothing is lost.\n\n"
    L"Retry tries Lares' bar again. Cancel keeps Windows' own taskbar.";

static HRESULT CALLBACK on_message_shown(HWND window, UINT message, WPARAM wp, LPARAM lp, LONG_PTR data)
{
    (void)wp, (void)lp, (void)data;
    if (message == TDN_CREATED) {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(window);
    }
    return S_OK;
}

typedef HRESULT (WINAPI *task_dialog_fn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);

/* The one message. Closing it means "Keep Windows' own taskbar". */
static int do_ask(void)
{
    static const TASKDIALOG_BUTTON buttons[] = {
        { IDRETRY, L"Try again" },
        { IDCANCEL, L"Keep Windows' own taskbar" },
    };
    int pressed = 0;
    HMODULE comctl = LoadLibraryExW(L"comctl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    task_dialog_fn show = comctl ? (task_dialog_fn)GetProcAddress(comctl, "TaskDialogIndirect") : NULL;
    if (show) {
        TASKDIALOGCONFIG c;
        ZeroMemory(&c, sizeof c);
        c.cbSize = sizeof c;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        c.pszWindowTitle = kTitle;
        c.pszMainInstruction = kHeading;
        c.pszContent = kBody;
        c.cButtons = ARRAYSIZE(buttons);
        c.pButtons = buttons;
        c.nDefaultButton = IDCANCEL;
        c.pfCallback = on_message_shown;
        if (SUCCEEDED(show(&c, &pressed, NULL, NULL)) && pressed)
            return pressed == IDRETRY ? ANSWER_TRY_AGAIN : ANSWER_KEEP;
    }
    pressed = MessageBoxW(NULL, kBodyPlain, kTitle,
                          MB_RETRYCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
    return pressed == IDRETRY ? ANSWER_TRY_AGAIN : ANSWER_KEEP;
}

/* ---- processes ------------------------------------------------------ */

static HANDLE start_process(wchar_t *command_line, DWORD flags, DWORD *pid, HANDLE *thread)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, command_line, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) return NULL;
    if (pid) *pid = pi.dwProcessId;
    if (thread) *thread = pi.hThread;
    else CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* "<this program>" verb [number run] [the shell's command] */
static wchar_t *command_line(const wchar_t *verb, DWORD number, int with_shell)
{
    lstrcpyW(g.scratch, L"\"");
    lstrcatW(g.scratch, g.self);
    lstrcatW(g.scratch, L"\" ");
    lstrcatW(g.scratch, verb);
    if (number) {
        lstrcatW(g.scratch, L" ");
        append_number(g.scratch, number);
        lstrcatW(g.scratch, L" run");
    }
    if (with_shell) {
        lstrcatW(g.scratch, L" ");
        lstrcatW(g.scratch, g.command);
    }
    return g.scratch;
}

/* Runs one of the short-lived copies and returns its answer, or 0 when it
 * could not be started or gave none. */
static DWORD run_copy(const wchar_t *verb, DWORD wait_ms)
{
    DWORD answer = 0;
    HANDLE copy = start_process(command_line(verb, 0, 0), 0, NULL, NULL);
    if (!copy) return 0;
    if (WaitForSingleObject(copy, wait_ms) == WAIT_OBJECT_0) GetExitCodeProcess(copy, &answer);
    CloseHandle(copy);
    return answer;
}

static void hide_taskbar(void)
{
    if (run_copy(L"do-hide", CHILD_WAIT_MS) != DONE) do_hide();
}

/* Gives the taskbar back; nonzero when the session is ending. */
static int restore_taskbar(void)
{
    DWORD answer = run_copy(L"do-restore", CHILD_WAIT_MS);
    if (answer != DONE && answer != DONE_SESSION_ENDING) answer = (DWORD)do_restore();
    return answer == DONE_SESSION_ENDING;
}

static int ask_try_again(void)
{
    DWORD answer = run_copy(L"do-ask", INFINITE);
    if (answer != ANSWER_KEEP && answer != ANSWER_TRY_AGAIN) answer = (DWORD)do_ask();
    return answer == ANSWER_TRY_AGAIN;
}

static void start_twin(void)
{
    if (g.twin) CloseHandle(g.twin);
    g.st.twin_pid = 0;
    g.twin = start_process(command_line(L"twin", GetCurrentProcessId(), 1), 0, &g.st.twin_pid, NULL);
    save_state();
}

static int start_shell(void)
{
    HANDLE thread = NULL;
    ResetEvent(g.ready);
    ResetEvent(g.quit);
    ResetEvent(g.alive);
    g.shell_ready = g.heard_alive = g.missed = 0;
    g.st.shell_pid = 0;

    lstrcpyW(g.scratch, g.command);
    g.shell = start_process(g.scratch, CREATE_SUSPENDED, &g.st.shell_pid, &thread);
    if (g.shell) {
        AssignProcessToJobObject(g.job, g.shell);
        ResumeThread(thread);
        CloseHandle(thread);
    }
    save_state();
    return g.shell != NULL;
}

static void leave(int code)
{
    g.st.running = 0;
    g.st.guard_pid = g.st.twin_pid = g.st.shell_pid = 0;
    save_state();
    ExitProcess((UINT)code);
}

/* The shell ended and it was no clean quit. Starts it again, or asks. */
static void shell_crashed(void)
{
    for (;;) {
        int recent = lares_guard_state_add_crash(&g.st, lares_guard_now());
        save_state();
        if (recent >= LARES_GUARD_CRASH_LIMIT) {
            if (!ask_try_again()) leave(EXIT_KEPT_WINDOWS_TASKBAR);
            g.st.crash_count = 0;
            save_state();
        }
        if (start_shell()) return;
    }
}

static void shell_ended(void)
{
    int clean = WaitForSingleObject(g.quit, 0) == WAIT_OBJECT_0;
    CloseHandle(g.shell);
    g.shell = NULL;
    g.shell_ready = 0;
    g.st.shell_pid = 0;

    int session_ending = restore_taskbar(); /* first, whatever comes next */
    if (clean || session_ending) leave(0);
    shell_crashed();
}

/* A shell that began saying "still answering" and then misses two looks is
 * ended, which the wait then sees as a crash. */
static void look_at_shell(void)
{
    if (WaitForSingleObject(g.alive, 0) == WAIT_OBJECT_0) {
        ResetEvent(g.alive);
        g.heard_alive = 1;
        g.missed = 0;
    } else if (g.heard_alive && ++g.missed >= MISSED_LOOKS) {
        TerminateProcess(g.shell, EXIT_HUNG_SHELL);
    }
}

static HANDLE named_event(const wchar_t *kind, const wchar_t *variable)
{
    wchar_t name[96];
    lstrcpyW(name, L"Local\\LaresGuard.");
    append_number(name, GetCurrentProcessId());
    lstrcatW(name, kind);
    SetEnvironmentVariableW(variable, name);
    return CreateEventW(NULL, TRUE, FALSE, name);
}

/* One guard for each taskbar it could hide: a running guard holds this. */
static void guard_mutex_name(wchar_t *name)
{
    wchar_t test_class[64];
    lstrcpyW(name, L"Local\\com.nahuelgenchi.Lares.guard");
    if (GetEnvironmentVariableW(L"LARES_GUARD_TASKBAR_CLASS", test_class, ARRAYSIZE(test_class)) > 0)
        lstrcatW(name, test_class);
}

static int run_guard(void)
{
    wchar_t name[128];
    guard_mutex_name(name);
    HANDLE only = CreateMutexW(NULL, FALSE, name);
    if (!only || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    /* What an earlier guard left hidden, if anything, comes back first. */
    lares_guard_state_load(g.dir, &g.st);
    if (g.st.hidden) restore_taskbar();
    g.st.running = 1;
    g.st.guard_pid = GetCurrentProcessId();
    g.st.twin_pid = g.st.shell_pid = 0;
    save_state();

    g.ready = named_event(L".ready", L"LARES_GUARD_READY");
    g.quit = named_event(L".quit", L"LARES_GUARD_QUIT");
    g.alive = named_event(L".alive", L"LARES_GUARD_ALIVE");

    /* The shell ends with the guard; what the shell starts does not. */
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    g.job = CreateJobObjectW(NULL, NULL);
    SetInformationJobObject(g.job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    start_twin();
    if (!start_shell()) shell_crashed();

    for (;;) {
        HANDLE handles[3];
        DWORD n = 0, shell_at = MAXDWORD, twin_at = MAXDWORD, ready_at = MAXDWORD;
        if (g.shell) handles[shell_at = n++] = g.shell;
        if (g.twin) handles[twin_at = n++] = g.twin;
        if (g.shell && !g.shell_ready) handles[ready_at = n++] = g.ready;
        DWORD timeout = (g.shell && g.shell_ready) || !g.twin ? LOOK_MS : INFINITE;

        DWORD r = n ? WaitForMultipleObjects(n, handles, FALSE, timeout) : (Sleep(LOOK_MS), WAIT_TIMEOUT);
        DWORD which = r - WAIT_OBJECT_0;
        if (r == WAIT_TIMEOUT) {
            if (!g.twin) start_twin();
            if (g.shell && g.shell_ready) look_at_shell();
        } else if (which >= n) {
            return 1; /* the wait itself failed: leave rather than spin */
        } else if (which == shell_at) {
            shell_ended();
        } else if (which == twin_at) {
            CloseHandle(g.twin);
            g.twin = NULL;
            start_twin();
        } else if (which == ready_at) {
            g.shell_ready = 1;
            hide_taskbar();
        }
    }
}

/* The twin: waits for the guard to end, and if it did not say "stopped",
 * gives the taskbar back and starts a new guard. */
static int run_twin(DWORD guard_pid)
{
    HANDLE guard = OpenProcess(SYNCHRONIZE, FALSE, guard_pid);
    if (guard) {
        WaitForSingleObject(guard, INFINITE);
        CloseHandle(guard);
    }

    lares_guard_state_load(g.dir, &g.st);
    if (!g.st.running || g.st.guard_pid != guard_pid) return 0;

    int session_ending = do_restore() == DONE_SESSION_ENDING;
    g.st.running = 0;
    g.st.guard_pid = g.st.twin_pid = g.st.shell_pid = 0;
    if (session_ending) {
        save_state();
        return 0;
    }
    int recent = lares_guard_state_add_crash(&g.st, lares_guard_now());
    save_state();
    if (recent >= LARES_GUARD_CRASH_LIMIT) {
        if (do_ask() != ANSWER_TRY_AGAIN) return EXIT_KEPT_WINDOWS_TASKBAR;
        g.st.crash_count = 0;
        save_state();
    }
    HANDLE next = start_process(command_line(L"run", 0, 1), 0, NULL, NULL);
    if (next) CloseHandle(next);
    return 0;
}

/* Gives the taskbar back from the state file when no guard is running. */
static int run_restore(void)
{
    wchar_t name[128];
    guard_mutex_name(name);
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (running) {
        CloseHandle(running);
        return 1; /* a guard is running, and the taskbar is its to give back */
    }
    lares_guard_state_load(g.dir, &g.st);
    do_restore();
    g.st.running = 0;
    save_state();
    return 0;
}

static int guard_main(void)
{
    wchar_t test_class[64];
    const wchar_t *rest = skip_argument(GetCommandLineW());
    const wchar_t *tail;

    if (!lares_guard_state_dir(g.dir, ARRAYSIZE(g.dir))) return 2;
    if (!GetModuleFileNameW(NULL, g.self, ARRAYSIZE(g.self))) return 2;
    if (GetEnvironmentVariableW(L"LARES_GUARD_TASKBAR_CLASS", test_class, ARRAYSIZE(test_class)) > 0)
        lares_taskbar_use_test_class(test_class);

    if (take_word(rest, L"do-hide", &tail)) return do_hide(), DONE;
    if (take_word(rest, L"do-restore", &tail)) return do_restore();
    if (take_word(rest, L"do-ask", &tail)) return do_ask();
    if (take_word(rest, L"restore", &tail)) return run_restore();

    if (take_word(rest, L"twin", &tail)) {
        DWORD guard_pid = take_number(tail, &tail);
        if (!guard_pid || !take_word(tail, L"run", &tail) || !*tail) return 2;
        if (lstrlenW(tail) >= ARRAYSIZE(g.command)) return 2;
        lstrcpyW(g.command, tail);
        return run_twin(guard_pid);
    }
    if (take_word(rest, L"run", &tail) && *tail) {
        if (lstrlenW(tail) >= ARRAYSIZE(g.command)) return 2;
        lstrcpyW(g.command, tail);
        return run_guard();
    }
    return 2;
}

#ifdef LARES_NO_CRT
void lares_guard_entry(void)
{
    ExitProcess((UINT)guard_main());
}
#else
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    (void)instance, (void)previous, (void)command, (void)show;
    return guard_main();
}
#endif
