/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What the guard knows, kept in one small file so that it outlives the guard:
 * whether a guard is meant to be running, whether the Windows taskbar's
 * windows are hidden, and when the shell last crashed. The file is written
 * and flushed before the taskbar is touched. The taskbar's auto-hide, a
 * setting of Windows' own, is in the change journal instead
 * (taskbar_journal.h).
 */
#ifndef LARES_GUARD_STATE_H
#define LARES_GUARD_STATE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARES_GUARD_MAX_CRASHES 4
/* Two crashes inside this long stop the restarts: five minutes, in the
 * 100-nanosecond units of FILETIME. */
#define LARES_GUARD_CRASH_WINDOW (5ull * 60ull * 10000000ull)
#define LARES_GUARD_CRASH_LIMIT 2

/* While the taskbar is being given back the file says so, and the shell's
 * side stops hiding a taskbar it sees shown. Either value means Lares still
 * owes Windows its taskbar. */
#define LARES_TASKBAR_HIDDEN 1
#define LARES_TASKBAR_GIVING_BACK 2

typedef struct lares_guard_state {
    int running;         /* a guard is meant to be running */
    int hidden;          /* the Windows taskbar's windows: 0, LARES_TASKBAR_HIDDEN or LARES_TASKBAR_GIVING_BACK */
    DWORD guard_pid;
    DWORD twin_pid;
    DWORD shell_pid;
    int crash_count;
    ULONGLONG crashes[LARES_GUARD_MAX_CRASHES];
} lares_guard_state;

/* The folder the state lives in: LARES_GUARD_STATE_DIR when set (tests),
 * otherwise Lares' folder under the user's local application data. */
int lares_guard_state_dir(wchar_t *dir, int capacity);

/* A missing or unreadable file loads as "nothing running, nothing hidden". */
void lares_guard_state_load(const wchar_t *dir, lares_guard_state *state);

/* Writes a new file, flushes it and puts it in place of the old one, so a
 * power cut leaves either the old state or the new one. 0 on failure. */
int lares_guard_state_save(const wchar_t *dir, const lares_guard_state *state);

/* Removes the half-written files that processes killed in the middle of a
 * save left behind. */
void lares_guard_state_tidy(const wchar_t *dir);

/* Text form, for the file and for tests. Returns the length written. */
int lares_guard_state_format(const lares_guard_state *state, char *text, int capacity);
void lares_guard_state_parse(const char *text, int length, lares_guard_state *state);

/* Records a crash at `now`, forgets the ones older than the window, and
 * returns how many are left inside it. */
int lares_guard_state_add_crash(lares_guard_state *state, ULONGLONG now);

ULONGLONG lares_guard_now(void);

#ifdef __cplusplus
}
#endif

#endif
