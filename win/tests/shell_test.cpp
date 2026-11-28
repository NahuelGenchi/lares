// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// lares-shell-test (issue #145): the shell's switches, from the outside,
// through the same code Lares' Settings use (win/src/shell_service.h).
//
//   lares-shell-test                    a fresh account changes nothing; then
//                                       the top bar and the dock on and off,
//                                       once, and together
//   lares-shell-test --cycles N         every on and off N times in a row
//   lares-shell-test --measure SECONDS  also the shell's own memory and its
//                                       processor time over SECONDS at rest,
//                                       and what it has loaded
//   lares-shell-test --real             Windows' own taskbar and a shell that
//                                       reserves its strips (only with
//                                       LARES_TEST_REAL_TASKBAR=1)
//
// By default nothing of the PC's own desktop is touched: the guard is
// pointed at a stand-in taskbar (windows of this program, off screen), the
// shell runs as a copy that hides nothing and reserves no space, with its
// windows off screen, and settings, journal, guard state, crash reports and
// the start-up entry all live in a temporary folder and a test key.

#include "config.h"
#include "shell_service.h"

extern "C" {
#include "journal.h"
#include "journal_win.h"
}

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

using namespace lares;

static int g_failures, g_checks;
static bool g_real;
static wchar_t g_dir[MAX_PATH], g_class[64], g_setting_class[80];
static std::wstring g_run_target;
static HANDLE g_bar_done;
static DWORD g_bar_thread;
static HWND g_setting;
#define BARS_RESET (WM_APP + 1)

static void check(bool ok, const char* what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAILED: %s\n", what);
}

// ---- the taskbar, stand-in or real ----------------------------------------

static DWORD WINAPI bar_thread(void*)
{
    HWND bar = nullptr;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = g_class;
    RegisterClassW(&wc);
    wc.lpszClassName = g_setting_class;
    RegisterClassW(&wc);
    g_setting = CreateWindowExW(WS_EX_TOOLWINDOW, g_setting_class, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                wc.hInstance, nullptr);
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
    bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, g_class, L"", WS_POPUP | WS_VISIBLE, -32000, -32000, 8, 8,
                          nullptr, nullptr, wc.hInstance, nullptr);
    SetPropW(g_setting, L"LaresTestAutoHide", (HANDLE)(INT_PTR)1);
    SetEvent(g_bar_done);
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    if (bar) DestroyWindow(bar);
    return 0;
}

static int taskbar_visible()
{
    if (g_real) {
        HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
        return t && IsWindowVisible(t);
    }
    HWND t = FindWindowW(g_class, nullptr);
    return t && IsWindowVisible(t);
}

// ---- the start-up entry -----------------------------------------------------

static std::wstring run_value()
{
    lares_journal_value v;
    if (!lares_journal_registry.read(g_run_target.c_str(), &v, nullptr) || !v.present || v.type != REG_SZ) return {};
    std::wstring text(reinterpret_cast<const wchar_t*>(v.data), v.size / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}

static std::wstring sibling(const wchar_t* file)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    return dir.substr(0, dir.find_last_of(L'\\') + 1) + file;
}

// ---- waiting -----------------------------------------------------------------

template <typename F> static bool wait_for(F done, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        if (done()) return true;
        Sleep(50);
    }
    return done();
}

static HWND shell_window()
{
    return FindWindowW(g_real ? L"LaresShellHost" : L"LaresShellHostIsolated", nullptr);
}

static DWORD shell_pid()
{
    HWND w = shell_window();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid;
}

static int count_processes(const wchar_t* name)
{
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) == 0) ++n;
    }
    CloseHandle(snap);
    return n;
}

static ShellPrefs g_prefs;

static bool set_parts(bool bar, bool dock, bool launcher = false, bool clipboard = false, bool screenshot = false)
{
    g_prefs.bar = bar;
    g_prefs.dock = dock;
    g_prefs.launcher = launcher;
    g_prefs.clipboard = clipboard;
    g_prefs.screenshot = screenshot;
    if (!save_shell_prefs(shell_ini_path(false), g_prefs)) {
        std::printf("  the settings file could not be saved (%lu)\n", GetLastError());
        return false;
    }
    std::string said;
    bool ok = shell_service_sync(g_prefs, false, &said);
    if (!ok) std::printf("  sync said: %s\n", said.c_str());
    return ok;
}

// A shortcut someone else holds cannot be registered again.
static bool shortcut_taken(UINT mods, UINT vk)
{
    if (RegisterHotKey(nullptr, 0x7a00, mods | MOD_NOREPEAT, vk)) {
        UnregisterHotKey(nullptr, 0x7a00);
        return false;
    }
    return true;
}

// The launcher and clipboard history: the shell runs for them alone, the
// taskbar stays, and their shortcuts exist only while they are on. A test
// copy registers no shortcut, so those are only looked at with --real.
static void cycle_tools(int i)
{
    std::printf("cycle %d: launcher, clipboard history and screenshot tool\n", i);
    check(set_parts(false, false, true, false), "launcher on");
    check(wait_for([] { return shell_window() != nullptr; }, 10000), "launcher on: the shell runs");
    Sleep(1000);
    check(taskbar_visible() != 0, "launcher on: the taskbar stays");
    if (g_real) check(shortcut_taken(MOD_ALT, VK_SPACE), "launcher on: Alt+Space is the launcher's");
    if (g_real) check(!shortcut_taken(MOD_ALT, 'V'), "launcher on: Alt+V stays free");

    DWORD pid = shell_pid();
    check(set_parts(false, false, true, true), "clipboard history on beside the launcher");
    if (g_real) check(wait_for([] { return shortcut_taken(MOD_ALT, 'V'); }, 5000), "clipboard history on: Alt+V is taken");
    check(shell_pid() == pid, "clipboard history on: the same shell");

    check(set_parts(false, false, false, true), "launcher off");
    if (g_real) check(wait_for([] { return !shortcut_taken(MOD_ALT, VK_SPACE); }, 5000), "launcher off: Alt+Space is free");
    check(shell_window() != nullptr, "launcher off: the shell stays for clipboard history");

    check(set_parts(false, false, false, true, true), "screenshot tool on beside clipboard history");
    if (g_real) check(wait_for([] { return shortcut_taken(MOD_ALT | MOD_SHIFT, 'S'); }, 5000), "screenshot tool on: Alt+Shift+S is taken");

    check(set_parts(false, false, false, false, true), "clipboard history off");
    if (g_real) check(wait_for([] { return !shortcut_taken(MOD_ALT, 'V'); }, 5000), "clipboard history off: Alt+V is free");
    check(shell_window() != nullptr, "clipboard history off: the shell stays for the screenshot tool");

    check(set_parts(false, false, false, false, false), "screenshot tool off");
    check(wait_for([] { return shell_window() == nullptr; }, 10000), "screenshot tool off: the shell is gone");
    if (g_real) check(!shortcut_taken(MOD_ALT | MOD_SHIFT, 'S'), "screenshot tool off: Alt+Shift+S is free");
    check(run_value().empty(), "screenshot tool off: the start-up entry is gone");
    check(wait_for([] { return count_processes(L"lares-guard.exe") == 0; }, 10000), "screenshot tool off: no guard is left");
}

// ---- the cases ---------------------------------------------------------------

static void fresh_account()
{
    std::printf("a fresh account\n");
    ShellPrefs def;
    check(load_shell_prefs(shell_ini_path(false), def) == false, "no settings file at first");
    check(!shell_needs_process(def), "nothing on by default");
    check(shell_service_sync(def, false, nullptr), "putting nothing in line works");
    Sleep(500);
    check(!shell_window(), "no shell runs");
    check(count_processes(L"lares-guard.exe") == 0, "no guard runs");
    check(run_value().empty(), "no start-up entry");
    check(taskbar_visible(), "the taskbar is as it was");
    wchar_t journal[MAX_PATH];
    GetEnvironmentVariableW(L"LARES_JOURNAL", journal, MAX_PATH);
    check(GetFileAttributesW(journal) == INVALID_FILE_ATTRIBUTES, "the change journal holds nothing");
    std::wstring expected = shell_autostart_command(sibling(L"lares-guard.exe"), sibling(L"lares-shell.exe"));
    check(expected.find(L" run --sign-in ") != std::wstring::npos, "the start-up entry starts the guard at sign-in");
}

static void cycle(int i)
{
    std::wstring expected = shell_autostart_command(sibling(L"lares-guard.exe"), sibling(L"lares-shell.exe"));
    std::printf("cycle %d\n", i);

    // The dock alone: it takes the taskbar's place.
    check(set_parts(false, true), "dock on");
    check(wait_for([] { return shell_window() != nullptr; }, 10000), "dock on: the shell runs");
    check(wait_for([] { return !taskbar_visible(); }, 10000), "dock on: the taskbar is hidden");
    check(run_value() == expected, "dock on: the start-up entry is there");

    check(set_parts(false, false), "dock off");
    check(wait_for([] { return shell_window() == nullptr; }, 10000), "dock off: the shell is gone");
    check(wait_for([] { return taskbar_visible() != 0; }, 10000), "dock off: the taskbar is back");
    check(run_value().empty(), "dock off: the start-up entry is gone");
    check(wait_for([] { return count_processes(L"lares-guard.exe") == 0; }, 10000), "dock off: no guard is left");

    // The top bar alone: the taskbar stays.
    check(set_parts(true, false), "bar on");
    check(wait_for([] { return shell_window() != nullptr; }, 10000), "bar on: the shell runs");
    Sleep(1500);
    check(taskbar_visible() != 0, "bar on: the taskbar stays");
    check(run_value() == expected, "bar on: the start-up entry is there");

    // AirDrop in the bar (issue #155): a button only; nothing of sharing
    // runs until its panel is opened, and the same shell carries on.
    DWORD bar_pid = shell_pid();
    g_prefs.bar_airdrop = true;
    check(set_parts(true, false), "AirDrop on beside the bar");
    Sleep(1000);
    check(shell_pid() == bar_pid, "AirDrop on: the same shell");
    check(count_processes(L"lares-share.exe") == 0, "AirDrop on: nothing of sharing runs");
    g_prefs.bar_airdrop = false;
    check(set_parts(true, false), "AirDrop off");
    Sleep(500);
    check(shell_pid() == bar_pid, "AirDrop off: the bar stays");
    check(count_processes(L"lares-share.exe") == 0, "AirDrop off: nothing of sharing runs");

    // The device indicators (issue #157): they only read, and the same
    // shell carries on.
    g_prefs.bar_indicators = true;
    check(set_parts(true, false), "device indicators on beside the bar");
    Sleep(1500);
    check(shell_pid() == bar_pid, "device indicators on: the same shell");
    g_prefs.bar_indicators = false;
    check(set_parts(true, false), "device indicators off");
    Sleep(500);
    check(shell_pid() == bar_pid, "device indicators off: the bar stays");

    // The dock joins the running bar: no new shell.
    DWORD pid = shell_pid();
    check(set_parts(true, true), "dock on beside the bar");
    check(wait_for([] { return !taskbar_visible(); }, 10000), "dock on beside the bar: the taskbar is hidden");
    check(shell_pid() == pid, "dock on beside the bar: the same shell");

    // The dock leaves the bar: a new shell for the bar, the taskbar back.
    check(set_parts(true, false), "dock off, bar on");
    check(wait_for([] { return taskbar_visible() != 0; }, 10000), "dock off, bar on: the taskbar is back");
    check(wait_for([pid] { return shell_window() != nullptr && shell_pid() != pid; }, 10000),
          "dock off, bar on: a new shell for the bar");
    check(run_value() == expected, "dock off, bar on: the start-up entry stays");

    check(set_parts(false, false), "bar off");
    check(wait_for([] { return shell_window() == nullptr; }, 10000), "bar off: the shell is gone");
    check(wait_for([] { return count_processes(L"lares-shell.exe") == 0; }, 10000), "bar off: no shell process");
    check(wait_for([] { return count_processes(L"lares-guard.exe") == 0; }, 10000), "bar off: no guard is left");
    check(taskbar_visible() != 0, "bar off: the taskbar is as it was");
    check(run_value().empty(), "bar off: the start-up entry is gone");
}

// The shell's own memory and processor time at rest, and what it loaded.
static void measure(int seconds)
{
    std::printf("measuring the shell at rest for %d seconds, with the AirDrop button and device indicators\n", seconds);
    g_prefs.bar_airdrop = true;
    g_prefs.bar_indicators = true;
    check(set_parts(true, true), "bar and dock on");
    check(wait_for([] { return shell_window() != nullptr; }, 10000), "the shell runs");
    Sleep(5000); // its first readings and icons
    DWORD pid = shell_pid();
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    check(p != nullptr, "the shell can be measured");
    if (p) {
        FILETIME c, x, k0, u0, k1, u1;
        GetProcessTimes(p, &c, &x, &k0, &u0);
        Sleep(static_cast<DWORD>(seconds) * 1000);
        GetProcessTimes(p, &c, &x, &k1, &u1);
        auto v = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        double cpu_ms = ((v(k1) - v(k0)) + (v(u1) - v(u0))) / 10000.0;
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(p, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        std::printf("  own memory: %.1f MB, working set %.1f MB\n", pmc.PrivateUsage / 1048576.0,
                    pmc.WorkingSetSize / 1048576.0);
        std::printf("  processor time in %d seconds at rest: %.1f ms (%.3f%% of one core)\n", seconds, cpu_ms,
                    cpu_ms / (seconds * 10.0));
        HMODULE mods[1024];
        DWORD need = 0;
        int heavy = 0;
        if (EnumProcessModules(p, mods, sizeof(mods), &need)) {
            for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
                wchar_t name[MAX_PATH];
                if (!GetModuleBaseNameW(p, mods[i], name, MAX_PATH)) continue;
                _wcslwr_s(name);
                if (wcsstr(name, L"d3d") || wcsstr(name, L"dxgi") || wcsstr(name, L"d2d1") ||
                    wcsstr(name, L"webview") || wcsstr(name, L"coreclr") || wcsstr(name, L"clrjit") ||
                    wcsstr(name, L"mscoree") || wcsstr(name, L"chrome") || wcsstr(name, L"libcef")) {
                    // A library this test has loaded too, which draws nothing,
                    // is loaded into every process here by another program.
                    if (GetModuleHandleW(name)) {
                        std::printf("  loaded, as in every process on this PC: %ls\n", name);
                        continue;
                    }
                    std::printf("  loaded: %ls\n", name);
                    ++heavy;
                }
            }
        }
        check(heavy == 0, "no Direct3D, Direct2D, WebView2, Chromium or .NET loaded");
        CloseHandle(p);
    }
    check(set_parts(false, false), "bar and dock off");
    check(wait_for([] { return shell_window() == nullptr; }, 10000), "the shell is gone");
    check(wait_for([] { return taskbar_visible() != 0; }, 10000), "the taskbar is back");
}

static void cleanup()
{
    // Anything of the test's still running, then the test's own files and key.
    shell_service_stop(5000);
    wait_for([] { return count_processes(L"lares-guard.exe") == 0; }, 5000);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Lares\\ShellTest");
    HKEY k;
    DWORD subkeys = 1, values = 1;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Lares", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr,
                         nullptr);
        RegCloseKey(k);
        if (subkeys == 0 && values == 0) RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Lares");
    }
    std::wstring cmd = L"cmd /c rd /s /q \"" + std::wstring(g_dir) + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring buf = cmd;
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int wmain(int argc, wchar_t** argv)
{
    int cycles = 1, seconds = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--cycles") && i + 1 < argc) cycles = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--measure") && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--real")) g_real = true;
    }
    if (g_real) {
        wchar_t ok[8];
        if (GetEnvironmentVariableW(L"LARES_TEST_REAL_TASKBAR", ok, 8) == 0 || wcscmp(ok, L"1") != 0) {
            std::printf("--real needs LARES_TEST_REAL_TASKBAR=1: it hides this PC's own taskbar. Nothing done.\n");
            return 0;
        }
    }
    // A guard or shell left by a test that ran just before ends within a
    // few seconds; one that stays belongs to an installed Lares, which a
    // test would end. In the Checks workflow nothing else may run: fail.
    wait_for([] { return count_processes(L"lares-shell.exe") == 0 && count_processes(L"lares-guard.exe") == 0; },
             15000);
    if (count_processes(L"lares-shell.exe") || count_processes(L"lares-guard.exe")) {
        wchar_t ci[8];
        bool in_ci = GetEnvironmentVariableW(L"CI", ci, 8) > 0;
        std::printf("a Lares shell or guard already runs here: nothing done\n");
        return in_ci ? 1 : 0;
    }

    // Everything in a folder of the test's own.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(g_dir, MAX_PATH, L"%lslares-shell-test-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_dir, nullptr);
    std::wstring dir(g_dir);
    SetEnvironmentVariableW(L"LARES_CONFIG_DIR", dir.c_str());
    SetEnvironmentVariableW(L"LARES_GUARD_STATE_DIR", dir.c_str());
    SetEnvironmentVariableW(L"LARES_JOURNAL", (dir + L"\\journal.log").c_str());
    SetEnvironmentVariableW(L"LARES_CRASH_DIR", (dir + L"\\crash").c_str());
    g_run_target = L"HKCU\\Software\\Lares\\ShellTest\\Run|LaresShell";
    SetEnvironmentVariableW(L"LARES_SHELL_RUN_TARGET", g_run_target.c_str());
    SetEnvironmentVariableW(L"LARES_DRY_RUN", nullptr);
    if (!g_real) {
        SetEnvironmentVariableW(L"LARES_SHELL_ISOLATED", L"1");
        swprintf(g_class, 64, L"LaresShellTestTaskbar%lu", GetCurrentProcessId());
        swprintf(g_setting_class, 80, L"%lsAutoHide", g_class);
        SetEnvironmentVariableW(L"LARES_GUARD_TASKBAR_CLASS", g_class);
        g_bar_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CreateThread(nullptr, 0, bar_thread, nullptr, 0, &g_bar_thread);
        WaitForSingleObject(g_bar_done, 5000);
    }

    fresh_account();
    for (int i = 1; i <= cycles; ++i) {
        cycle(i);
        cycle_tools(i);
    }
    if (seconds > 0) measure(seconds);

    // What the journal still holds after every switch went off: nothing owed.
    lares_journal journal;
    lares_journal_undo_result result = {};
    if (lares_journal_open(&journal, nullptr) && lares_journal_add_backend(&journal, &lares_journal_registry)) {
        lares_journal_undo(&journal, kShellFeature, 0, 0, &result);
        check(run_value().empty(), "after undo: no start-up entry");
    }
    // No shell or guard crashed along the way.
    {
        WIN32_FIND_DATAW fd;
        std::wstring pattern = std::wstring(g_dir) + L"\\crash\\*.txt";
        HANDLE f = FindFirstFileW(pattern.c_str(), &fd);
        int reports = 0;
        if (f != INVALID_HANDLE_VALUE) {
            do {
                ++reports;
                std::printf("  crash report: %ls\n", fd.cFileName);
                std::wstring path = std::wstring(g_dir) + L"\\crash\\" + fd.cFileName;
                FILE* r = _wfopen(path.c_str(), L"rb");
                if (r) {
                    char line[256];
                    while (fgets(line, sizeof(line), r)) std::printf("    %s", line);
                    fclose(r);
                }
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        check(reports == 0, "nothing crashed");
    }
    cleanup();
    if (!g_real) PostThreadMessageW(g_bar_thread, WM_QUIT, 0, 0);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
