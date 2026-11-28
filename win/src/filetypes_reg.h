// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Lares offered for Markdown and pictures (issue #159): per-user ProgIDs
 * that open lares-view.exe, Lares in "Open with" for their extensions, and
 * Lares registered under its own name, so Windows' Default apps page can
 * list it. Every key and value goes through the change journal (feature
 * "file-types"), so undo takes away exactly what Lares added. Lares never
 * writes a UserChoice key or hash and never makes itself a default: the
 * user chooses on Windows' own page. */
#pragma once
#include <string>
#include <vector>

namespace lares {

inline constexpr char kFileTypesFeature[] = "file-types";

/* One key to make, or one string value to set, in the order they are made. */
struct FileTypeChange {
    bool key = false;
    std::wstring target;   // "HKCU\...\Key" for a key, "HKCU\...\Key|Name" for a value
    std::wstring text;     // the value's text
};

/* What offering Lares adds, under `root` ("HKCU", or a test key), for the
 * lares-view.exe at `view_exe`. */
std::vector<FileTypeChange> filetypes_changes(const std::wstring& root, const std::wstring& view_exe);

/* Whether Lares is offered now, under `root`. */
bool filetypes_offered(const std::wstring& root);

/* Makes the changes through the journal at `journal_path` (nullptr: Lares'
 * own); on a failure, what was made is taken back. */
bool filetypes_offer(const wchar_t* journal_path, const std::wstring& root, const std::wstring& view_exe,
                     std::string& said);
/* Takes back every change the journal holds for the feature. */
bool filetypes_withdraw(const wchar_t* journal_path, std::string& said);

/* For Settings: HKCU (LARES_FILETYPES_ROOT in tests), lares-view.exe beside
 * this program, Lares' journal; a dry run only says what it would do. */
bool filetypes_offered_now();
bool filetypes_set(bool on, bool dry_run, std::string& said);
/* Windows' Default apps page, on Lares, for the user to choose. */
void filetypes_open_default_apps();

} // namespace lares
