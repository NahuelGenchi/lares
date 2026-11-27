// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "changes.h"

#include <windows.h>

extern "C" {
#include "journal.h"
#include "journal_win.h"
#include "taskbar_journal.h"
}

namespace lares {

static bool open_journal(bool dry_run, lares_journal& journal)
{
    if (dry_run) {
        wchar_t path[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LARES_JOURNAL", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
            if (n == 0 || n + 28 >= MAX_PATH) return false;
            lstrcatW(path, L"\\Lares\\journal-dry.log");
        }
        if (!lares_journal_open(&journal, path)) return false;
    } else if (!lares_journal_open(&journal, nullptr)) {
        return false;
    }
    lares_journal_add_backend(&journal, &lares_journal_registry);
    lares_journal_add_backend(&journal, &lares_journal_taskbar);
    return true;
}

int changes_open(bool dry_run)
{
    lares_journal journal;
    if (!open_journal(dry_run, journal)) return 0;
    return lares_journal_list(&journal, nullptr, nullptr);
}

static std::string counted(int n, const char* one, const char* many)
{
    return n == 1 ? std::string(one) : std::to_string(n) + many;
}

bool changes_undo(bool dry_run, std::string& said)
{
    lares_journal journal;
    lares_journal_undo_result r = {};
    if (!open_journal(dry_run, journal)) {
        said = "The list of changes could not be opened.";
        return false;
    }
    if (!dry_run) guard_command(L"stop");
    bool ok = lares_journal_undo(&journal, nullptr, 0, 0, &r) != 0;

    int back = r.restored + r.already;
    if (back + r.changed + r.failed == 0) {
        said = "Nothing to undo.";
        return ok;
    }
    said.clear();
    if (back) said += counted(back, "1 setting is", " settings are") + " back as before.";
    if (r.changed) {
        if (!said.empty()) said += " ";
        said += counted(r.changed, "1 was", " were") + " changed by you or another program since, and left alone.";
    }
    if (r.failed) {
        if (!said.empty()) said += " ";
        said += counted(r.failed, "1 could", " could") + " not be put back.";
    }
    return ok;
}

void guard_command(const wchar_t* verb)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring guard(path);
    size_t slash = guard.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    guard.resize(slash + 1);
    guard += L"lares-guard.exe";
    if (GetFileAttributesW(guard.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    std::wstring line = L"\"" + guard + L"\" " + verb;
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

} // namespace lares
