// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The newest local crash report (win/crash/crash.h), as Settings shows it:
 * which program, when, and copying its text to the clipboard. Nothing here
 * sends anything anywhere. */
#pragma once
#include <string>
#include <windows.h>

namespace lares {

struct CrashReportInfo {
    std::wstring path;     // empty when there is no report
    std::wstring program;  // "lares", "lares-pad", "lares-guard", ...
    std::wstring when;     // in this PC's own time, e.g. "27 Nov 2026, 15:30"
};

CrashReportInfo crash_latest();

// Puts the report's text on the clipboard. False when it could not.
bool crash_copy(HWND owner, const std::wstring& path);

// The program's name from a report's file name, "<program>-YYYYMMDD-HHMMSS.txt".
std::wstring crash_program_from_name(const std::wstring& file_name);

} // namespace lares
