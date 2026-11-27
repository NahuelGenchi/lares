// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Turning the shell's parts on and off (issue #145). lares-shell.exe runs
// under lares-guard.exe, which owns hiding the Windows taskbar
// (docs/SAFETY.md); this file starts and ends that pair and keeps its
// start-up entry, through the change journal, so that Undo all, `lares undo`
// and an uninstaller can take it away again.

#include "config.h"

#include <string>

namespace lares {

inline constexpr char kShellFeature[] = "shell-startup";
inline constexpr wchar_t kShellAutostartTarget[] = L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run|LaresShell";

// The parts that run in lares-shell.exe (bar, dock, launcher, clipboard
// history); with none of them on, nothing runs.
bool shell_needs_process(const ShellPrefs& prefs);

// The start-up entry's command: the guard, told this is the start at sign-in
// (where Shift held means safe mode), running the shell.
std::wstring shell_autostart_command(const std::wstring& guard_exe, const std::wstring& shell_exe);

// Puts what runs in line with `prefs`: the start-up entry (through the
// journal), and a running shell started, told to read its settings again,
// or ended cleanly (the guard then gives the taskbar back). A shell that
// hides the taskbar and whose dock went off is ended and started again for
// the bar alone. Does nothing in a dry run. Returns false when something
// could not be done; `said` then has a plain sentence for Settings.
bool shell_service_sync(const ShellPrefs& prefs, bool dry_run, std::string* said = nullptr);

// Ends a running shell cleanly and waits up to `wait_ms` for it to go.
bool shell_service_stop(unsigned wait_ms);

// True while a lares-shell.exe of this user is running.
bool shell_service_running();

} // namespace lares
