// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
// Turning the shell's parts on and off (issue #145). See shell_service.h.
#include "shell_service.h"

extern "C" {
#include "journal.h"
#include "journal_win.h"
}

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>
#include <vector>

namespace lares {

namespace {

constexpr UINT kQuitClean = WM_APP + 20; // win/shell/shell.h: WM_LARES_QUIT_CLEAN

std::wstring env(const wchar_t* name)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

// Tests run the shell as a copy that hides nothing (LARES_SHELL_ISOLATED),
// which has a window class of its own.
const wchar_t* host_class()
{
    std::wstring iso = env(L"LARES_SHELL_ISOLATED");
    return (!iso.empty() && iso != L"0") ? L"LaresShellHostIsolated" : L"LaresShellHost";
}

// Tests point the start-up entry at a key of their own.
std::wstring autostart_target()
{
    std::wstring t = env(L"LARES_SHELL_RUN_TARGET");
    return t.empty() ? std::wstring(kShellAutostartTarget) : t;
}

// The guard and the shell sit next to the program that asks.
std::wstring sibling(const wchar_t* file)
{
    wchar_t self[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(self);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return dir.substr(0, slash + 1) + file;
}

bool exists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return !path.empty() && a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

struct Owed {
    const wchar_t* target;
    bool owed;
};

int find_owed(const lares_journal_entry* e, void* ctx)
{
    auto* o = static_cast<Owed*>(ctx);
    if (std::strcmp(e->feature, kShellFeature) == 0 && std::wcscmp(e->target, o->target) == 0) o->owed = true;
    return 1;
}

// The start-up entry, through the journal. Nothing is written when the
// entry is already as wanted, so a fresh account's journal stays empty.
bool autostart_apply(bool wanted, const std::wstring& command)
{
    std::wstring target = autostart_target();
    lares_journal journal;
    if (!lares_journal_open(&journal, nullptr) || !lares_journal_add_backend(&journal, &lares_journal_registry))
        return false;
    lares_journal_value current, absent, value;
    lares_journal_value_absent(&absent);
    if (!lares_journal_registry.read(target.c_str(), &current, nullptr)) return false;
    if (wanted) {
        if (!lares_journal_value_string(&value, command.c_str())) return false;
    } else {
        value = absent;
    }
    if (lares_journal_value_equal(&current, &value)) return true;

    // An entry of that name that the journal does not know is someone
    // else's: written down first, so an undo puts it back.
    Owed owed = { target.c_str(), false };
    lares_journal_list(&journal, find_owed, &owed);
    if (current.present && !owed.owed) {
        ULONGLONG id = 0;
        if (!lares_journal_begin(&journal, kShellFeature, "registry", target.c_str(), &absent, &current, &id))
            return false;
        lares_journal_complete(&journal, id);
    }
    return lares_journal_change(&journal, kShellFeature, "registry", target.c_str(), &value, nullptr) != 0;
}

bool start_shell(const std::wstring& guard_exe, const std::wstring& shell_exe)
{
    std::wstring cmd = L"\"" + guard_exe + L"\" run \"" + shell_exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(guard_exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool shell_needs_process(const ShellPrefs& prefs)
{
    return prefs.bar || prefs.dock || prefs.launcher || prefs.clipboard;
}

std::wstring shell_autostart_command(const std::wstring& guard_exe, const std::wstring& shell_exe)
{
    return L"\"" + guard_exe + L"\" run --sign-in \"" + shell_exe + L"\"";
}

bool shell_service_running()
{
    return FindWindowW(host_class(), nullptr) != nullptr;
}

// The process that started `pid`: the shell's guard.
static DWORD parent_of(DWORD pid)
{
    DWORD parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == pid) {
            if (_wcsicmp(pe.szExeFile, L"lares-shell.exe") == 0) parent = pe.th32ParentProcessID;
            break;
        }
    }
    if (parent) {
        // Only a guard counts; a shell started by hand has none.
        bool guard = false;
        PROCESSENTRY32W pg = {};
        pg.dwSize = sizeof(pg);
        for (BOOL ok = Process32FirstW(snap, &pg); ok; ok = Process32NextW(snap, &pg)) {
            if (pg.th32ProcessID == parent) {
                guard = _wcsicmp(pg.szExeFile, L"lares-guard.exe") == 0;
                break;
            }
        }
        if (!guard) parent = 0;
    }
    CloseHandle(snap);
    return parent;
}

bool shell_service_stop(unsigned wait_ms)
{
    HWND host = FindWindowW(host_class(), nullptr);
    if (!host) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(host, &pid);
    HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    // The guard gives the taskbar back and leaves after a clean end; a new
    // guard can only start once it has, so wait for it too.
    DWORD guard_pid = pid ? parent_of(pid) : 0;
    HANDLE guard = guard_pid ? OpenProcess(SYNCHRONIZE, FALSE, guard_pid) : nullptr;
    PostMessageW(host, kQuitClean, 0, 0);
    ULONGLONG until = GetTickCount64() + wait_ms;
    bool gone = true;
    if (process) {
        gone = WaitForSingleObject(process, wait_ms) == WAIT_OBJECT_0;
        CloseHandle(process);
    }
    if (guard) {
        ULONGLONG now = GetTickCount64();
        DWORD left = now < until ? static_cast<DWORD>(until - now) : 0;
        gone = WaitForSingleObject(guard, left) == WAIT_OBJECT_0 && gone;
        CloseHandle(guard);
    }
    return gone;
}

bool shell_service_sync(const ShellPrefs& prefs, bool dry_run, std::string* said)
{
    if (dry_run) return true;
    bool wanted = shell_needs_process(prefs);
    std::wstring guard_exe = sibling(L"lares-guard.exe");
    std::wstring shell_exe = sibling(L"lares-shell.exe");
    bool installed = exists(guard_exe) && exists(shell_exe);
    bool ok = true;

    if (!autostart_apply(wanted && installed, shell_autostart_command(guard_exe, shell_exe))) {
        ok = false;
        if (said) *said = "The start-up entry could not be changed.";
    }

    HWND host = FindWindowW(host_class(), nullptr);
    if (!wanted) {
        if (host && !shell_service_stop(5000)) {
            ok = false;
            if (said) *said = "The bar and dock did not end.";
        }
        return ok;
    }
    if (!installed) {
        if (said) *said = "The bar and dock are not installed next to Lares.";
        return false;
    }
    if (host) {
        // Showing the dock is what hides the Windows taskbar; without it the
        // taskbar comes back only with a clean end, so start a new shell.
        bool hides = GetPropW(host, L"LaresShellHides") != nullptr;
        if (hides && !prefs.dock) {
            if (!shell_service_stop(5000)) {
                if (said) *said = "The bar and dock did not end.";
                return false;
            }
            host = nullptr;
        } else {
            PostMessageW(host, RegisterWindowMessageW(L"LaresShellReload"), 0, 0);
            return ok;
        }
    }
    if (!start_shell(guard_exe, shell_exe)) {
        if (said) *said = "The bar and dock could not be started.";
        return false;
    }
    return ok;
}

} // namespace lares
