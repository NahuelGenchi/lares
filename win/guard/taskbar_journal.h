/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The taskbar's auto-hide as a setting the change journal can change and
 * put back (win/journal/journal.h). Auto-hide is Windows' own setting and
 * outlives Lares, the session and a restart, which is why it is journalled;
 * a hidden taskbar window is not, Windows shows it again by itself.
 */
#ifndef LARES_GUARD_TASKBAR_JOURNAL_H
#define LARES_GUARD_TASKBAR_JOURNAL_H

#include "journal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LARES_SHELL_FEATURE "shell"
#define LARES_TASKBAR_KIND "taskbar"
#define LARES_TASKBAR_AUTOHIDE L"auto-hide"

/* Kind "taskbar", target "auto-hide", a number: 1 on, 0 off. */
extern const lares_journal_backend lares_journal_taskbar;

/* The user's journal with this backend added. 0 when it cannot be opened. */
int lares_shell_journal(lares_journal *journal);

/* Switches auto-hide on through the journal, for the shell. Nonzero when it
 * is on afterwards. */
int lares_shell_autohide_on(lares_journal *journal);

/* Puts auto-hide back as the journal says it was. If someone else changed
 * it since, their value stays and Lares forgets its own. */
void lares_shell_autohide_back(lares_journal *journal);

/* Whether the journal still holds Lares' switching on of auto-hide. */
int lares_shell_autohide_owed(lares_journal *journal);

#ifdef __cplusplus
}
#endif

#endif
