/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Safe mode (safe.h).
 */
#include "safe.h"

#define MARK_NAME L"com.nahuelgenchi.Lares.SafeMode"

int lares_safe_mode_wanted(int safe_switch, int at_sign_in, int shift_held, int session_marked)
{
    return safe_switch || session_marked || (at_sign_in && shift_held);
}

static void mark_name(const wchar_t *scope, wchar_t *name, int capacity)
{
    lstrcpyW(name, MARK_NAME);
    if (scope && *scope && lstrlenW(scope) + lstrlenW(name) + 2 < capacity) {
        lstrcatW(name, L".");
        lstrcatW(name, scope);
    }
}

int lares_safe_mode_marked(const wchar_t *scope)
{
    wchar_t name[160];
    mark_name(scope, name, ARRAYSIZE(name));
    return GlobalFindAtomW(name) != 0;
}

void lares_safe_mode_mark(const wchar_t *scope, int on)
{
    wchar_t name[160];
    mark_name(scope, name, ARRAYSIZE(name));
    ATOM atom = GlobalFindAtomW(name);
    if (on) {
        if (!atom) GlobalAddAtomW(name);
        return;
    }
    /* An atom is counted: take it away as often as it was added. */
    for (int i = 0; atom && i < 64; ++i) {
        GlobalDeleteAtom(atom);
        atom = GlobalFindAtomW(name);
    }
}
