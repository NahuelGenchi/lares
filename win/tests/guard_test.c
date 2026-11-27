/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * lares-guard-test: proves lares-guard from the outside, with the stand-in
 * shell (test_shell.c).
 *
 *   lares-guard-test              every case, against a stand-in taskbar
 *   lares-guard-test --real       a few cases against the real Windows
 *                                 taskbar; it hides and shows it, so it does
 *                                 nothing unless LARES_TEST_REAL_TASKBAR=1,
 *                                 which only the Checks workflow sets
 *   lares-guard-test --measure N  the guard's memory and processor time
 *                                 over N idle seconds
 *   lares-guard-test --only NAME  one group: display, sign-out, stale, safe
 *
 * The stand-in taskbar is one or two windows of this program, placed off
 * screen, and its auto-hide is a property of one more window. The guard is
 * pointed at them with LARES_GUARD_TASKBAR_CLASS and keeps its state in a
 * temporary folder (LARES_GUARD_STATE_DIR), so nothing of the PC's own
 * desktop is touched. With a stand-in the guard shows nothing either: its
 * question after two crashes goes through a file in that folder.
 */
#include <windows.h>

#include <psapi.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define SHELL_WINDOW_CLASS L"LaresTestShell"
#define SHELL_QUIT (WM_APP + 1)
#define SHELL_CRASH (WM_APP + 2)
#define SHELL_HANG (WM_APP + 3)
#define SHELL_LEAVE (WM_APP + 4)
#define BARS_RESET (WM_APP + 10)
#define AUTOHIDE_PROP L"LaresTestAutoHide"
#define CLICK_BUTTON (WM_USER + 102) /* TDM_CLICK_BUTTON */
#define LIMIT_MS 2000

static wchar_t g_dir[MAX_PATH], g_state_file[MAX_PATH], g_journal_file[MAX_PATH], g_class[64];
static wchar_t g_setting_class[80];
static HWND g_setting;
static wchar_t g_guard[MAX_PATH], g_shell[MAX_PATH];
static int g_real, g_failures, g_checks;
static DWORD g_bar_thread;
static HANDLE g_bar_done;
static const char *g_case = "";

#include "journal.h"
#include "safe.h"
#include "state.h"

int guard_state_tests(void); /* guard_state_test.c */

static void note(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("  ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static void check(int ok, const char *what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    printf("  FAILED [%s]: %s\n", g_case, what);
    fflush(stdout);
}

/* ---- the taskbar, stand-in or real ---------------------------------- */

static DWORD WINAPI bar_thread(void *unused)
{
    HWND bars[4];
    int count = 0;
    MSG m;
    WNDCLASSW wc = { 0 };
    (void)unused;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_class;
    RegisterClassW(&wc);
    /* The stand-in's auto-hide: one setting for all its taskbars, as Windows'
     * is, kept on a window that is never shown (win/guard/taskbar.c). */
    wc.lpszClassName = g_setting_class;
    RegisterClassW(&wc);
    g_setting = CreateWindowExW(WS_EX_TOOLWINDOW, g_setting_class, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance,
                                NULL);
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);
    SetEvent(g_bar_done);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.hwnd || m.message != BARS_RESET) {
            DispatchMessageW(&m);
            continue;
        }
        while (count) DestroyWindow(bars[--count]);
        for (; count < (int)m.wParam && count < 4; ++count)
            bars[count] = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, g_class, L"", WS_POPUP | WS_VISIBLE,
                                          -32000, -32000, 8, 8, NULL, NULL, wc.hInstance, NULL);
        SetEvent(g_bar_done);
    }
    return 0;
}

static void set_bars_autohide(int on)
{
    SetPropW(g_setting, AUTOHIDE_PROP, (HANDLE)(INT_PTR)(on ? 2 : 1));
}

/* A fresh stand-in taskbar: `count` visible windows, auto-hide as given. */
static void bars_reset(int count, int autohide)
{
    if (g_real) return;
    ResetEvent(g_bar_done);
    PostThreadMessageW(g_bar_thread, BARS_RESET, (WPARAM)count, 0);
    WaitForSingleObject(g_bar_done, 5000);
    set_bars_autohide(autohide);
}

static int bars_total(void)
{
    int n = 0;
    HWND w = NULL;
    if (g_real) {
        n = FindWindowW(L"Shell_TrayWnd", NULL) != NULL;
        while ((w = FindWindowExW(NULL, w, L"Shell_SecondaryTrayWnd", NULL)) != NULL) ++n;
        return n;
    }
    while ((w = FindWindowExW(NULL, w, g_class, NULL)) != NULL) ++n;
    return n;
}

static int bars_visible(void)
{
    int n = 0;
    HWND w = NULL;
    if (g_real) {
        w = FindWindowW(L"Shell_TrayWnd", NULL);
        n = w && IsWindowVisible(w);
        w = NULL;
        while ((w = FindWindowExW(NULL, w, L"Shell_SecondaryTrayWnd", NULL)) != NULL) n += IsWindowVisible(w) != 0;
        return n;
    }
    while ((w = FindWindowExW(NULL, w, g_class, NULL)) != NULL) n += IsWindowVisible(w) != 0;
    return n;
}

/* Asking the real taskbar waits on Explorer, so it is done on a thread of
 * its own and given three seconds: an Explorer that does not answer fails
 * the check instead of hanging the test. */
static DWORD WINAPI ask_real_autohide(void *answer)
{
    APPBARDATA d = { 0 };
    d.cbSize = sizeof d;
    d.hWnd = FindWindowW(L"Shell_TrayWnd", NULL);
    *(volatile LONG *)answer = (SHAppBarMessage(ABM_GETSTATE, &d) & ABS_AUTOHIDE) != 0;
    return 0;
}

static int bars_autohide(void)
{
    if (g_real) {
        static int told;
        LONG *answer = (LONG *)calloc(1, sizeof *answer); /* left to a thread that never answers */
        HANDLE thread = answer ? CreateThread(NULL, 0, ask_real_autohide, answer, 0, NULL) : NULL;
        int result = -1;
        if (thread && WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0) {
            result = (int)*answer;
            free(answer);
        } else if (!told++) {
            note("Explorer did not say whether auto-hide is on within three seconds");
        }
        if (thread) CloseHandle(thread);
        return result;
    }
    return (INT_PTR)GetPropW(g_setting, AUTOHIDE_PROP) == 2;
}

/* ---- the guard's state file and processes --------------------------- */

typedef struct {
    int running, hidden, crashes;
    DWORD guard, twin, shell;
} snapshot;

static snapshot read_state(void)
{
    snapshot s = { 0 };
    char text[1024], *line, *next = NULL;
    DWORD got = 0;
    /* Opened so that the guard can replace the file meanwhile, and tried
     * again while another program (a scanner) holds it for a moment, as the
     * guard itself does: a read that fails here would aim a kill at nobody. */
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100; ++i) {
        f = CreateFileW(g_state_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE || GetLastError() == ERROR_FILE_NOT_FOUND) break;
        Sleep(5);
    }
    if (f == INVALID_HANDLE_VALUE) return s;
    if (!ReadFile(f, text, sizeof text - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    text[got] = 0;
    for (line = strtok_s(text, "\n", &next); line; line = strtok_s(NULL, "\n", &next)) {
        unsigned long long v = 0;
        char key[32] = "";
        if (sscanf(line, "%31s %llu", key, &v) < 1) continue;
        if (!strcmp(key, "running")) s.running = (int)v;
        else if (!strcmp(key, "hidden")) s.hidden = (int)v;
        else if (!strcmp(key, "guard")) s.guard = (DWORD)v;
        else if (!strcmp(key, "twin")) s.twin = (DWORD)v;
        else if (!strcmp(key, "shell")) s.shell = (DWORD)v;
        else if (!strcmp(key, "crash")) ++s.crashes;
    }
    return s;
}

static int is_ours(DWORD pid, const wchar_t *image)
{
    wchar_t path[MAX_PATH];
    DWORD n = ARRAYSIZE(path);
    int ours = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!p) return 0;
    if (WaitForSingleObject(p, 0) == WAIT_TIMEOUT && QueryFullProcessImageNameW(p, 0, path, &n))
        ours = _wcsicmp(path, image) == 0;
    CloseHandle(p);
    return ours;
}

static int guard_alive(DWORD pid) { return pid && is_ours(pid, g_guard); }
static int shell_alive(DWORD pid) { return pid && is_ours(pid, g_shell); }

/* Times are counted from the last thing done to a process: mark() is called
 * just before each kill and each message to the shell. */
static LARGE_INTEGER g_mark;

static void mark(void) { QueryPerformanceCounter(&g_mark); }

static int since_mark(void)
{
    LARGE_INTEGER now, per_second;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&per_second);
    return (int)((now.QuadPart - g_mark.QuadPart) * 1000 / per_second.QuadPart);
}

/* Ends a process and waits until it is really gone, so that nothing that
 * follows mistakes it for a living one. */
static void kill_pid(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!p) return;
    mark();
    TerminateProcess(p, 99);
    WaitForSingleObject(p, 2000);
    CloseHandle(p);
}

/* How many of this test's guards and shells are running; ends them on request. */
static int our_processes(int end_them)
{
    int n = 0;
    PROCESSENTRY32W e = { 0 };
    HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (all == INVALID_HANDLE_VALUE) return 0;
    e.dwSize = sizeof e;
    if (Process32FirstW(all, &e)) do {
        if (_wcsicmp(e.szExeFile, L"lares-guard.exe") && _wcsicmp(e.szExeFile, L"lares-test-shell.exe")) continue;
        if (!is_ours(e.th32ProcessID, g_guard) && !is_ours(e.th32ProcessID, g_shell)) continue;
        ++n;
        if (end_them) kill_pid(e.th32ProcessID);
    } while (Process32NextW(all, &e));
    CloseHandle(all);
    return n;
}

static HWND window_of(const wchar_t *window_class, DWORD pid)
{
    HWND w = NULL;
    while ((w = FindWindowExW(NULL, w, window_class, NULL)) != NULL) {
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (owner == pid) return w;
    }
    return NULL;
}

static BOOL CALLBACK find_message_window(HWND w, LPARAM out)
{
    wchar_t name[32];
    DWORD pid = 0;
    if (!GetClassNameW(w, name, ARRAYSIZE(name)) || wcscmp(name, L"#32770")) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (!is_ours(pid, g_guard)) return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

/* A window of the guard's on the desktop: with a stand-in taskbar there must
 * never be one. */
static HWND message_window(void)
{
    HWND w = NULL;
    EnumWindows(find_message_window, (LPARAM)&w);
    return w;
}

/* With a stand-in taskbar the guard asks its question through a file in the
 * state folder and shows nothing (guard.c, test_ask). */
static void ask_file(wchar_t *path, const wchar_t *name)
{
    swprintf(path, MAX_PATH + 16, L"%ls\\%ls", g_dir, name);
}

static int asked(void)
{
    wchar_t pending[MAX_PATH + 16];
    ask_file(pending, L"ask.pending");
    return GetFileAttributesW(pending) != INVALID_FILE_ATTRIBUTES;
}

static void answer_message(int button)
{
    wchar_t answer[MAX_PATH + 16];
    ask_file(answer, L"ask.answer");
    FILE *f = _wfopen(answer, L"wb");
    if (!f) return;
    fputs(button == IDRETRY ? "retry" : "keep", f);
    fclose(f);
}

/* ---- conditions and waiting ----------------------------------------- */

static int g_before; /* auto-hide before the guard touched it */

static int taskbar_back(void) { return bars_total() > 0 && bars_visible() == bars_total() && bars_autohide() == g_before; }
static int taskbar_hidden(void) { return bars_visible() == 0 && bars_autohide() == 1; }
static int message_up(void) { return asked(); }
static int nothing_runs(void) { return our_processes(0) == 0; }

/* A guard, its twin and a ready shell, with the taskbar hidden. */
static int healthy(void)
{
    snapshot s = read_state();
    return s.running && s.hidden == 1 && guard_alive(s.guard) && guard_alive(s.twin) && shell_alive(s.shell) &&
           taskbar_hidden();
}

static int safe(void) { return taskbar_back() || healthy(); }

/* Milliseconds from the last mark() until `condition` held, or -1 when it
 * did not within the limit (counted from this call). */
/* What the taskbar did during the last wait, printed when a check fails:
 * each change of (visible taskbars, auto-hide) and when it came. */
static struct {
    int count;
    int ms[16], visible[16], autohide[16], journal[16];
} g_seen;

static int journal_exists(void);

static void see(int ms)
{
    int v = bars_visible(), a = bars_autohide(), j = journal_exists();
    int last = g_seen.count - 1;
    if (g_seen.count && g_seen.visible[last] == v && g_seen.autohide[last] == a && g_seen.journal[last] == j) return;
    if (g_seen.count == 16) return;
    g_seen.ms[g_seen.count] = ms;
    g_seen.visible[g_seen.count] = v;
    g_seen.autohide[g_seen.count] = a;
    g_seen.journal[g_seen.count++] = j;
}

static int wait_for(int (*condition)(void), int limit_ms)
{
    int start = since_mark();
    g_seen.count = 0;
    for (;;) {
        int elapsed = since_mark();
        see(elapsed);
        if (condition()) return elapsed;
        if (elapsed - start > limit_ms) return -1;
        Sleep(1);
    }
}


/* After a failed check: the change journal's raw lines. */
static void journal_dump(void)
{
    char text[2048];
    DWORD got = 0;
    HANDLE f = CreateFileW(g_journal_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        note("    no change journal (error %lu)", GetLastError());
        return;
    }
    if (!ReadFile(f, text, sizeof text - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    text[got] = 0;
    printf("    change journal:\n%s", text);
}

static void check_within(int (*condition)(void), int limit_ms, const char *what)
{
    int ms = wait_for(condition, limit_ms);
    char text[160];
    snprintf(text, sizeof text, "%s (limit %d ms)", what, limit_ms);
    check(ms >= 0, text);
    if (ms >= 0) note("%s: %d ms", what, ms);
    else {
        snapshot s = read_state();
        note("  taskbars: %d of %d visible, auto-hide %d (was %d before); state hidden %d, shell %s", bars_visible(),
             bars_total(), bars_autohide(), g_before, s.hidden, shell_alive(s.shell) ? "alive" : "gone");
        for (int i = 0; i < g_seen.count; ++i)
            note("    at %d ms: %d visible, auto-hide %d, journal %d", g_seen.ms[i], g_seen.visible[i], g_seen.autohide[i],
                 g_seen.journal[i]);
        journal_dump();
    }
}

static void start_guard(const wchar_t *shell_command)
{
    wchar_t line[1024];
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi;
    si.cb = sizeof si;
    if (shell_command) swprintf(line, ARRAYSIZE(line), L"\"%ls\" run %ls", g_guard, shell_command);
    /* The stand-in says "ready" a little late, as a real bar would: a shell
     * started again right after a crash would otherwise hide the taskbar
     * within a few milliseconds of it coming back, too briefly to be seen
     * by a test on a busy machine. */
    else swprintf(line, ARRAYSIZE(line), L"\"%ls\" run \"%ls\" --ready-after 300", g_guard, g_shell);
    mark();
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        check(0, "the guard starts");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static int run_guard_command(const wchar_t *verb)
{
    wchar_t line[1024];
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi;
    DWORD code = 99;
    si.cb = sizeof si;
    swprintf(line, ARRAYSIZE(line), L"\"%ls\" %ls", g_guard, verb);
    mark();
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 99;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void tell_shell(UINT message)
{
    HWND w = window_of(SHELL_WINDOW_CLASS, read_state().shell);
    check(w != NULL, "the stand-in shell has its window");
    mark();
    if (w) PostMessageW(w, message, 0, 0);
}

/* Ends whatever a case left behind and forgets the state. */
static void clean_up(void)
{
    int quiet = 0;
    wchar_t ask[MAX_PATH + 16];
    for (int i = 0; i < 200 && quiet < 3; ++i) {
        quiet = our_processes(1) ? 0 : quiet + 1;
        Sleep(20);
    }
    ask_file(ask, L"ask.pending");
    DeleteFileW(ask);
    ask_file(ask, L"ask.answer");
    DeleteFileW(ask);
    /* The real taskbar's auto-hide is put back by the guard itself, from
     * the journal; the stand-in's journal is just thrown away. */
    if (g_real) run_guard_command(L"restore");
    else DeleteFileW(g_journal_file);
    DeleteFileW(g_state_file);
}

static int journal_exists(void)
{
    return GetFileAttributesW(g_journal_file) != INVALID_FILE_ATTRIBUTES;
}

static void begin(const char *name, int bars, int autohide)
{
    g_case = name;
    printf("%s\n", name);
    fflush(stdout);
    clean_up();
    bars_reset(bars, autohide);
    g_before = g_real ? bars_autohide() : autohide;
}

static void begin_running(const char *name, int bars, int autohide)
{
    begin(name, bars, autohide);
    start_guard(NULL);
    check_within(healthy, 5000, "shell ready and taskbar hidden");
    if (!healthy()) {
        snapshot s = read_state();
        note("state: running %d hidden %d crashes %d; guard %s, twin %s, shell %s; taskbars %d of %d visible, "
             "auto-hide %d; %d of this test's processes; message %s",
             s.running, s.hidden, s.crashes, guard_alive(s.guard) ? "alive" : "gone",
             guard_alive(s.twin) ? "alive" : "gone", shell_alive(s.shell) ? "alive" : "gone", bars_visible(),
             bars_total(), bars_autohide(), our_processes(0), message_up() ? "shown" : "not shown");
    }
}

/* ---- the cases ------------------------------------------------------ */

static void case_clean_quit(int autohide)
{
    begin_running(autohide ? "clean quit, auto-hide was on" : "clean quit, auto-hide was off", 1, autohide);
    check(journal_exists() == !autohide, autohide ? "auto-hide was on already: nothing for the change journal"
                                                 : "the change journal holds the switching on of auto-hide");
    tell_shell(SHELL_QUIT);
    check_within(taskbar_back, LIMIT_MS, "taskbar back after a clean quit");
    check_within(nothing_runs, 3000, "guard and twin left");
    check(!journal_exists(), "nothing left in the change journal");
    check(read_state().running == 0 && read_state().crashes == 0, "state says stopped, no crash counted");
}

static void case_shell_ends(const char *name, UINT message)
{
    begin_running(name, 1, 0);
    DWORD old_shell = read_state().shell;
    if (message) tell_shell(message);
    else kill_pid(old_shell);
    check_within(taskbar_back, LIMIT_MS, "taskbar back");
    check_within(healthy, 5000, "shell started again and taskbar hidden again");
    check(read_state().shell != old_shell, "it is a new shell");
    check(read_state().crashes == 1, "one crash counted");
}

static void case_hang(void)
{
    begin_running("shell hangs", 1, 0);
    tell_shell(SHELL_HANG);
    check_within(taskbar_back, 20000, "taskbar back after the shell stopped answering");
    check_within(healthy, 5000, "shell started again");
}

static void case_guard_killed(void)
{
    begin_running("guard killed", 1, 0);
    snapshot s = read_state();
    kill_pid(s.guard);
    check_within(taskbar_back, LIMIT_MS, "taskbar back");
    Sleep(200);
    check(!shell_alive(s.shell), "the old shell ended with its guard");
    check_within(healthy, 5000, "a new guard, twin and shell");
    check(read_state().guard != s.guard, "it is a new guard");
}

static void case_twin_killed(void)
{
    begin_running("twin killed, then guard", 1, 0);
    snapshot s = read_state();
    kill_pid(s.twin);
    Sleep(50);
    check_within(healthy, 3000, "a new twin, taskbar still hidden");
    check(read_state().twin != s.twin, "it is a new twin");
    check(read_state().shell == s.shell, "the shell was left alone");
    kill_pid(read_state().guard);
    check_within(taskbar_back, LIMIT_MS, "taskbar back when the guard is killed next");
}

/* Kills in the given order ('S' shell, 'G' guard, 'T' twin), each aimed at
 * whoever holds that part at that moment. */
static int kill_in_order(const char *order, int gap_ms)
{
    for (const char *p = order; *p; ++p) {
        snapshot s = read_state();
        DWORD pid = *p == 'S' ? s.shell : (*p == 'G' ? s.guard : s.twin);
        if (pid && (is_ours(pid, g_guard) || is_ours(pid, g_shell))) kill_pid(pid);
        if (p[1] && gap_ms) Sleep((DWORD)gap_ms);
    }
    return wait_for(safe, LIMIT_MS);
}

static void case_orders(void)
{
    static const char *orders[] = { "SG", "ST", "GS", "GT", "TG", "TS", "SGT", "STG", "GST", "GTS", "TSG", "TGS" };
    char name[64];
    int quick_ok = 0;

    for (int i = 0; i < (int)ARRAYSIZE(orders); ++i) {
        snprintf(name, sizeof name, "kill order %s, 250 ms apart", orders[i]);
        begin_running(name, 1, 0);
        int ms = kill_in_order(orders[i], 250);
        check(ms >= 0, "usable desktop within two seconds of the last kill");
        if (ms >= 0) note("usable after %d ms", ms);
        /* What follows is either a healthy set again or the message. */
        check(wait_for(healthy, 6000) >= 0 || message_up() || nothing_runs(), "it settles");
    }

    /* With no time between the kills, guard and twin can both be gone before
     * either acts. Not required to pass; what is required is that the state
     * file then gives the taskbar back on the next start. */
    for (int i = 0; i < (int)ARRAYSIZE(orders); ++i) {
        snprintf(name, sizeof name, "kill order %s, no gap", orders[i]);
        begin_running(name, 1, 0);
        int ms = kill_in_order(orders[i], 0);
        if (ms >= 0) ++quick_ok;
        note(ms >= 0 ? "usable after %d ms" : "not back by itself (%d)", ms);
        if (ms < 0) {
            for (int k = 0; k < 50 && our_processes(1); ++k) Sleep(20);
            mark();
            check(run_guard_command(L"restore") == 0, "lares-guard restore runs");
            check_within(taskbar_back, LIMIT_MS, "taskbar back from the state file");
        }
    }
    note("no gap: %d of %d orders came back by themselves", quick_ok, (int)ARRAYSIZE(orders));

    /* How far apart twin and guard have to be killed for the guard to have
     * its new twin in place. Told, not required. */
    static const int gaps[] = { 1, 2, 5, 10, 20, 50 };
    for (int i = 0; i < (int)ARRAYSIZE(gaps); ++i) {
        snprintf(name, sizeof name, "kill order TG, %d ms apart", gaps[i]);
        begin_running(name, 1, 0);
        int ms = kill_in_order("TG", gaps[i]);
        note(ms >= 0 ? "usable after %d ms" : "not back by itself (%d)", ms);
    }
}

/* Healthy again, with a shell that is not the one that just crashed: right
 * after a crash the old one can still look alive for a moment. */
static DWORD g_crashed_shell;
static int healthy_new(void) { return healthy() && read_state().shell != g_crashed_shell; }

static void crash_shell(void)
{
    g_crashed_shell = read_state().shell;
    tell_shell(SHELL_CRASH);
}

static void case_crash_loop(void)
{
    begin_running("crash loop", 1, 0);
    crash_shell();
    check_within(taskbar_back, LIMIT_MS, "taskbar back after the first crash");
    check_within(healthy_new, 5000, "shell started again");
    crash_shell();
    check_within(taskbar_back, LIMIT_MS, "taskbar back after the second crash");
    check_within(message_up, 5000, "the message is asked");
    check(message_window() == NULL, "nothing is shown on the desktop");
    Sleep(300);
    check(!shell_alive(read_state().shell), "no shell is started while the message waits");
    check(taskbar_back(), "taskbar stays visible while the message waits");

    answer_message(IDRETRY);
    g_crashed_shell = 0;
    check_within(healthy_new, 5000, "Try again starts the shell");
    crash_shell();
    check_within(healthy_new, 5000, "one crash after Try again just restarts");
    crash_shell();
    check_within(message_up, 5000, "the message is asked again");
    answer_message(IDCANCEL);
    check_within(nothing_runs, 3000, "Keep Windows' own taskbar ends guard and twin");
    check(taskbar_back(), "taskbar visible, auto-hide as before");
    check(read_state().running == 0 && read_state().hidden == 0, "state says stopped, nothing hidden");
}

static void case_missing_shell(void)
{
    begin("shell cannot be started", 1, 0);
    start_guard(L"\"no-such-program.exe\"");
    check_within(message_up, 5000, "the message is asked");
    check(message_window() == NULL, "nothing is shown on the desktop");
    check(taskbar_back(), "taskbar never hidden");
    answer_message(IDCANCEL);
    check_within(nothing_runs, 3000, "guard and twin left");
}

static void case_explorer_restart(void)
{
    begin_running("Explorer restart (stand-in: new taskbar window, TaskbarCreated)", 1, 0);
    HWND shell = window_of(SHELL_WINDOW_CLASS, read_state().shell);
    bars_reset(1, 0);
    mark();
    PostMessageW(shell, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    check_within(taskbar_hidden, LIMIT_MS, "new taskbar hidden, auto-hide on again");

    mark();
    ShowWindow(FindWindowExW(NULL, NULL, g_class, NULL), SW_SHOWNA);
    check_within(taskbar_hidden, LIMIT_MS, "a taskbar shown again by its owner is hidden again");

    kill_pid(read_state().shell);
    check_within(taskbar_back, LIMIT_MS, "taskbar back after a crash");
}

static void case_display_change(void)
{
    begin_running("display change (stand-in: a second taskbar, WM_DISPLAYCHANGE)", 1, 0);
    DWORD shell_pid = read_state().shell;
    HWND shell = window_of(SHELL_WINDOW_CLASS, shell_pid);
    bars_reset(2, 1); /* Explorer keeps auto-hide across a display change */
    mark();
    PostMessageW(shell, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    check_within(taskbar_hidden, LIMIT_MS, "both taskbars hidden");
    kill_pid(shell_pid);
    check_within(taskbar_back, LIMIT_MS, "both taskbars back after a crash");
}

static void case_sign_out(void)
{
    DWORD_PTR r = 0;
    begin_running("sign-out (stand-in: the two end-of-session messages, sent to the shell)", 1, 0);
    snapshot s = read_state();
    HWND shell = window_of(SHELL_WINDOW_CLASS, s.shell);

    SendMessageTimeoutW(shell, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 2000, &r);
    SendMessageTimeoutW(shell, WM_ENDSESSION, FALSE, 0, SMTO_ABORTIFHUNG, 2000, &r);
    check(healthy(), "a sign-out that is called off changes nothing");

    mark();
    SendMessageTimeoutW(shell, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 2000, &r);
    SendMessageTimeoutW(shell, WM_ENDSESSION, TRUE, 0, SMTO_ABORTIFHUNG, 2000, &r);
    check_within(taskbar_back, LIMIT_MS, "taskbar and its auto-hide back at sign-out");
    check(read_state().hidden == 0, "state says nothing hidden");

    /* Windows then ends the programs. The shell goes first here; the other
     * order needs a real sign-out, where the twin sees that the session is
     * ending. */
    kill_pid(s.shell);
    check_within(nothing_runs, 3000, "guard and twin leave, no new shell");
    check(read_state().running == 0 && read_state().crashes == 0, "state says stopped, no crash counted");
    check(taskbar_back(), "taskbar still visible");
}

/* What a guard that died in the middle leaves behind: a state file that
 * says "hidden", and in the change journal the switching on of auto-hide,
 * with or without the change itself having been made. */
static int stand_in_read(const wchar_t *target, lares_journal_value *value, void *ctx)
{
    (void)target, (void)ctx;
    lares_journal_value_dword(value, (DWORD)bars_autohide());
    return 1;
}

static int stand_in_write(const wchar_t *target, const lares_journal_value *value, void *ctx)
{
    (void)target, (void)ctx;
    set_bars_autohide(value->data[0] != 0);
    return 1;
}

static void leave_behind(int hidden, int autohide_switched)
{
    static const lares_journal_backend stand_in = { "taskbar", stand_in_read, stand_in_write, NULL };
    lares_journal journal;
    lares_journal_value off, on;
    HWND bar = FindWindowExW(NULL, NULL, g_class, NULL);
    if (hidden) {
        FILE *f = _wfopen(g_state_file, L"wb");
        if (f) {
            fputs("lares-guard-state 1\nrunning 1\nhidden 1\nguard 4\ntwin 0\nshell 0\nend\n", f);
            fclose(f);
        }
        ShowWindow(bar, SW_HIDE);
    }
    lares_journal_open(&journal, g_journal_file);
    lares_journal_add_backend(&journal, &stand_in);
    lares_journal_value_dword(&off, 0);
    lares_journal_value_dword(&on, 1);
    if (autohide_switched) lares_journal_change(&journal, "shell", "taskbar", L"auto-hide", &on, NULL);
    else lares_journal_begin(&journal, "shell", "taskbar", L"auto-hide", &off, &on, NULL);
    g_before = 0;
    mark();
}

static void case_stale_state(void)
{
    begin("left by a guard that is gone: hidden, auto-hide switched on", 1, 0);
    leave_behind(1, 1);
    check(bars_visible() == 0 && bars_autohide() == 1 && journal_exists(), "the taskbar is hidden and the journal says why");
    check(run_guard_command(L"restore") == 0, "lares-guard restore runs");
    check_within(taskbar_back, LIMIT_MS, "taskbar and auto-hide back");
    check(!journal_exists(), "nothing left in the journal");

    begin("left by a guard that is gone: the journal written, auto-hide not yet switched", 1, 0);
    leave_behind(1, 0);
    check(run_guard_command(L"restore") == 0, "lares-guard restore runs");
    check_within(taskbar_back, LIMIT_MS, "taskbar back, auto-hide as it was");
    check(!journal_exists(), "nothing left in the journal");

    begin("left by a guard that is gone: only the auto-hide entry", 1, 0);
    leave_behind(0, 1);
    check(run_guard_command(L"restore") == 0, "lares-guard restore runs");
    check_within(taskbar_back, LIMIT_MS, "auto-hide back");
    check(!journal_exists(), "nothing left in the journal");

    begin("left by a guard that is gone: a guard that starts", 1, 0);
    leave_behind(1, 1);
    start_guard(L"\"no-such-program.exe\"");
    check_within(taskbar_back, LIMIT_MS, "it gives the taskbar back first");
}

/* Safe mode: nothing of the shell starts and the taskbar is never touched. */
static void case_safe_mode(void)
{
    wchar_t command[MAX_PATH + 32], verb[MAX_PATH + 48];
    swprintf(command, ARRAYSIZE(command), L"--safe \"%ls\"", g_shell);
    swprintf(verb, ARRAYSIZE(verb), L"run %ls", command);

    begin("safe mode: the --safe switch", 1, 0);
    lares_safe_mode_mark(g_class, 0);
    start_guard(command);
    Sleep(500);
    check(nothing_runs(), "no guard, twin or shell stays");
    check(taskbar_back() && read_state().hidden == 0, "the taskbar was left alone");
    check(lares_safe_mode_marked(g_class), "the session is marked");

    start_guard(NULL);
    Sleep(500);
    check(nothing_runs() && taskbar_back(), "in a marked session a plain start does nothing either");

    lares_safe_mode_mark(g_class, 0);
    check(!lares_safe_mode_marked(g_class), "the mark can be taken away");
    begin_running("safe mode: --safe while the bar is running", 1, 0);
    check(run_guard_command(verb) == 0, "lares-guard run --safe returns");
    check_within(taskbar_back, LIMIT_MS, "taskbar back");
    check_within(nothing_runs, 3000, "guard, twin and shell are gone");
    check(read_state().running == 0 && read_state().crashes == 0, "state says stopped, no crash counted");
    lares_safe_mode_mark(g_class, 0);

    begin_running("lares-guard stop", 1, 0);
    check(run_guard_command(L"stop") == 0, "it returns once the guard has left");
    check(nothing_runs() && taskbar_back(), "guard, twin and shell are gone and the taskbar is back");
    check(read_state().running == 0 && read_state().crashes == 0, "state says stopped, no crash counted");
    check(run_guard_command(L"stop") == 0, "with no guard running it does nothing");

    /* The copy that decides: 30 is a normal start, 31 safe mode. Shift is
     * not looked at here, since no start in a test is "at sign-in". */
    check(run_guard_command(L"do-safe") == 30, "nothing asked and no mark: a normal start");
    check(run_guard_command(L"do-safe --safe") == 31 && lares_safe_mode_marked(g_class), "--safe: safe mode, and the mark");
    check(run_guard_command(L"do-safe") == 31, "the mark alone: safe mode");
    lares_safe_mode_mark(g_class, 0);
}

static void case_real(void)
{
    begin_running("real taskbar: crash, guard killed, clean quit", 1, 0);
    kill_pid(read_state().shell);
    check_within(taskbar_back, LIMIT_MS, "taskbar back after the shell is killed");
    check_within(healthy, 8000, "shell started again and taskbar hidden again");
    clean_up();

    begin_running("real taskbar: guard killed", 1, 0);
    kill_pid(read_state().guard);
    check_within(taskbar_back, LIMIT_MS, "taskbar back after the guard is killed");
    check_within(healthy, 8000, "a new guard, twin and shell");
    tell_shell(SHELL_QUIT);
    check_within(taskbar_back, LIMIT_MS, "taskbar back after a clean quit");
    check_within(nothing_runs, 3000, "guard and twin left");
}

/* ---- measuring ------------------------------------------------------ */

static void measure_one(const char *name, DWORD pid, int seconds, int wait)
{
    PROCESS_MEMORY_COUNTERS_EX m = { 0 };
    FILETIME c, e, k0, u0, k1, u1;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    GetProcessTimes(p, &c, &e, &k0, &u0);
    if (wait) Sleep((DWORD)seconds * 1000);
    GetProcessTimes(p, &c, &e, &k1, &u1);
    GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    ULONGLONG total = (((ULONGLONG)k1.dwHighDateTime << 32) | k1.dwLowDateTime) +
                      (((ULONGLONG)u1.dwHighDateTime << 32) | u1.dwLowDateTime);
    ULONGLONG before = (((ULONGLONG)k0.dwHighDateTime << 32) | k0.dwLowDateTime) +
                       (((ULONGLONG)u0.dwHighDateTime << 32) | u0.dwLowDateTime);
    printf("%s: own memory %llu KB, working set %llu KB, processor time since start %.1f ms", name,
           (unsigned long long)m.PrivateUsage / 1024, (unsigned long long)m.WorkingSetSize / 1024, total / 10000.0);
    if (wait) printf(", in the %d idle seconds %.1f ms", seconds, (total - before) / 10000.0);
    printf("\n");
    CloseHandle(p);
}

static void measure(int seconds)
{
    begin_running("measure", 1, 0);
    snapshot s = read_state();
    Sleep(2000);
    measure_one("guard", s.guard, seconds, 1);
    measure_one("twin", s.twin, seconds, 0);
    tell_shell(SHELL_QUIT);
    wait_for(nothing_runs, 3000);
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t temp[MAX_PATH], *slash;
    int seconds = 0;
    const wchar_t *only = NULL; /* --only sign-out | stale | safe: one group of cases, while working on it */
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--real")) g_real = 1;
        else if (!wcscmp(argv[i], L"--only") && i + 1 < argc) only = argv[++i];
        else if (!wcscmp(argv[i], L"--measure") && i + 1 < argc) seconds = _wtoi(argv[++i]);
    }

    GetModuleFileNameW(NULL, g_guard, ARRAYSIZE(g_guard));
    slash = wcsrchr(g_guard, L'\\');
    if (!slash) return 2;
    wcscpy(slash + 1, L"lares-test-shell.exe");
    wcscpy(g_shell, g_guard);
    wcscpy(slash + 1, L"lares-guard.exe");

    GetTempPathW(ARRAYSIZE(temp), temp);
    swprintf(g_dir, ARRAYSIZE(g_dir), L"%lslares-guard-test-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_dir, NULL);
    swprintf(g_state_file, ARRAYSIZE(g_state_file), L"%ls\\guard.state", g_dir);
    SetEnvironmentVariableW(L"LARES_GUARD_STATE_DIR", g_dir);
    swprintf(g_journal_file, ARRAYSIZE(g_journal_file), L"%ls\\journal.log", g_dir);
    SetEnvironmentVariableW(L"LARES_JOURNAL", g_journal_file);

    if (g_real) {
        HWND bar = FindWindowW(L"Shell_TrayWnd", NULL);
        wchar_t opt_in[8] = L"";
        GetEnvironmentVariableW(L"LARES_TEST_REAL_TASKBAR", opt_in, ARRAYSIZE(opt_in));
        if (wcscmp(opt_in, L"1") != 0) {
            printf("SKIPPED: the real taskbar is touched only with LARES_TEST_REAL_TASKBAR=1, which the Checks\n"
                   "workflow sets on its throwaway machine. Never set it on a PC someone works on.\n");
            RemoveDirectoryW(g_dir);
            return 0;
        }
        if (!bar || !IsWindowVisible(bar)) {
            printf("SKIPPED: this machine shows no Windows taskbar, so the real one was not checked.\n");
            RemoveDirectoryW(g_dir);
            return 0;
        }
        printf("Real Windows taskbar: %d found, auto-hide %s.\n", bars_total(), bars_autohide() ? "on" : "off");
        case_real();
    } else {
        swprintf(g_class, ARRAYSIZE(g_class), L"LaresTestTaskbar%lu", GetCurrentProcessId());
        swprintf(g_setting_class, ARRAYSIZE(g_setting_class), L"%lsAutoHide", g_class);
        SetEnvironmentVariableW(L"LARES_GUARD_TASKBAR_CLASS", g_class);
        g_bar_done = CreateEventW(NULL, TRUE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, bar_thread, NULL, 0, &g_bar_thread));
        WaitForSingleObject(g_bar_done, 5000);

        if (seconds) {
            measure(seconds);
        } else if (only && !wcscmp(only, L"sign-out")) {
            case_sign_out();
        } else if (only && !wcscmp(only, L"display")) {
            case_explorer_restart();
            case_display_change();
        } else if (only && !wcscmp(only, L"stale")) {
            case_stale_state();
        } else if (only && !wcscmp(only, L"safe")) {
            case_safe_mode();
        } else {
            g_failures += guard_state_tests();
            case_clean_quit(0);
            case_clean_quit(1);
            case_shell_ends("shell crashes", SHELL_CRASH);
            case_shell_ends("shell killed", 0);
            case_shell_ends("shell leaves without a word", SHELL_LEAVE);
            case_hang();
            case_guard_killed();
            case_twin_killed();
            case_orders();
            case_crash_loop();
            case_missing_shell();
            case_explorer_restart();
            case_display_change();
            case_sign_out();
            case_stale_state();
            case_safe_mode();
        }
    }

    g_case = "clean up";
    clean_up();
    lares_guard_state_tidy(g_dir);
    DeleteFileW(g_state_file);
    RemoveDirectoryW(g_dir);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
