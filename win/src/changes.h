// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Windows settings Lares has changed, as its change journal lists them
 * (win/journal/journal.h), and the one undo that Settings and `lares undo`
 * share with the guard and, later, the uninstaller. */
#pragma once
#include <string>

namespace lares {

// How many changes the journal still holds. A dry run reads a journal of
// its own (LARES_JOURNAL when set, otherwise journal-dry.log), never the
// user's.
int changes_open(bool dry_run);

// Undoes every change, newest first, and says what happened in a sentence
// or two. Outside a dry run a running guard is stopped first, so the bar is
// not left without its taskbar setting. Returns false when something could
// not be put back.
bool changes_undo(bool dry_run, std::string& said);

// Runs lares-guard.exe, from beside this program, with one word, and waits
// for it. Does nothing when it is not there.
void guard_command(const wchar_t* verb);

} // namespace lares
