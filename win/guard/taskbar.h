/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Windows taskbar, as much of it as the guard needs: is it there, is it
 * shown, is auto-hide on, and changing the last two. See taskbar.c for the
 * Microsoft pages this is written from.
 */
#ifndef LARES_GUARD_TASKBAR_H
#define LARES_GUARD_TASKBAR_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tests only: work on windows of this class instead of the Windows taskbar,
 * and keep "auto-hide" as a property of those windows. An empty or NULL name
 * means the real taskbar. */
void lares_taskbar_use_test_class(const wchar_t *window_class);

/* How many taskbars there are (one per screen that shows one), and how many
 * of them are visible. */
int lares_taskbar_count(void);
int lares_taskbar_visible_count(void);

/* Shows or hides every taskbar. Never waits for Explorer. */
void lares_taskbar_show(int show);

/* Auto-hide: 1 on, 0 off, -1 when there is no taskbar to ask. */
int lares_taskbar_get_autohide(void);
void lares_taskbar_set_autohide(int on);

/* The process that owns the taskbar (0 when there is none), and whether a
 * window is a taskbar. */
DWORD lares_taskbar_process(void);
int lares_taskbar_is(HWND window);

#ifdef __cplusplus
}
#endif

#endif
