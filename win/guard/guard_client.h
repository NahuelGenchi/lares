/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The shell's side of lares-guard. A program that draws Lares' bar builds
 * guard_client.c, state.c and taskbar.c in and makes three calls:
 *
 *   lares_guard_client_ready(window)     once the bar is on screen;
 *   lares_guard_client_message(...)      first thing in that window's
 *                                        procedure, for every message;
 *   lares_guard_client_quit()            just before leaving on purpose.
 *
 * Started without a guard, all three do nothing and the Windows taskbar is
 * left alone.
 */
#ifndef LARES_GUARD_CLIENT_H
#define LARES_GUARD_CLIENT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tells the guard to hide the Windows taskbar, and from then on: says "still
 * answering" every two seconds from this window's thread, and hides the
 * taskbar again when Explorer makes it anew, shows it again, or the screens
 * change. */
void lares_guard_client_ready(HWND window);

/* Nonzero when the message was the client's own and needs nothing more.
 * At the end of the session it gives the taskbar back itself, because the
 * guard has no window to hear about it. */
int lares_guard_client_message(HWND window, UINT message, WPARAM wp, LPARAM lp);

/* A clean quit: the guard gives the taskbar back and leaves too. */
void lares_guard_client_quit(void);

#ifdef __cplusplus
}
#endif

#endif
