/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Safe mode: Lares starts with every shell part off and the Windows desktop
 * exactly as it is. Three things ask for it: the `--safe` switch, Shift held
 * while signing in, and a mark that an earlier `--safe` left on the session.
 *
 * The mark is a global atom: Windows keeps it until the session ends, with
 * no file to clean up and no process to keep alive, and it is gone at the
 * next sign-in.
 *   https://learn.microsoft.com/windows/win32/dataxchg/about-atom-tables
 *
 * A global atom belongs to the window station, which a program joins when it
 * loads user32. The mark must therefore be read and set from a program that
 * has: Lares' window, or one of the guard's short-lived copies. The guard
 * itself, which never loads user32, would see a table of its own.
 *
 * No C runtime: the guard links this.
 */
#ifndef LARES_GUARD_SAFE_H
#define LARES_GUARD_SAFE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The decision, as plain data. Shift counts only for a start at sign-in:
 * otherwise typing a capital letter while Lares starts would be enough. */
int lares_safe_mode_wanted(int safe_switch, int at_sign_in, int shift_held, int session_marked);

/* The session's mark. `scope` is "" for the user's own session; the dry run
 * and the tests pass a name of their own so they never mark it. */
int lares_safe_mode_marked(const wchar_t *scope);
void lares_safe_mode_mark(const wchar_t *scope, int on);

#ifdef __cplusplus
}
#endif

#endif
