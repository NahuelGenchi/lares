/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The taskbar's auto-hide through the change journal (taskbar_journal.h).
 */
#include "taskbar_journal.h"

#include "taskbar.h"

static int autohide_read(const wchar_t *target, lares_journal_value *value, void *ctx)
{
    (void)ctx;
    if (lstrcmpW(target, LARES_TASKBAR_AUTOHIDE) != 0) return 0;
    int on = lares_taskbar_get_autohide();
    if (on < 0) return 0; /* no taskbar to ask */
    lares_journal_value_dword(value, on ? 1u : 0u);
    return 1;
}

static int autohide_write(const wchar_t *target, const lares_journal_value *value, void *ctx)
{
    (void)ctx;
    if (lstrcmpW(target, LARES_TASKBAR_AUTOHIDE) != 0 || !value->present || value->size < 1) return 0;
    lares_taskbar_set_autohide(value->data[0] != 0);
    /* Counted as made: a change that is in doubt must stay in the journal,
     * where undo finds out by reading the setting. */
    return 1;
}

const lares_journal_backend lares_journal_taskbar = { LARES_TASKBAR_KIND, autohide_read, autohide_write, NULL };

int lares_shell_journal(lares_journal *journal)
{
    return lares_journal_open(journal, NULL) && lares_journal_add_backend(journal, &lares_journal_taskbar);
}

int lares_shell_autohide_on(lares_journal *journal)
{
    lares_journal_value on;
    lares_journal_value_dword(&on, 1);
    return lares_journal_change(journal, LARES_SHELL_FEATURE, LARES_TASKBAR_KIND, LARES_TASKBAR_AUTOHIDE, &on, NULL);
}

void lares_shell_autohide_back(lares_journal *journal)
{
    lares_journal_undo_result result;
    lares_journal_undo(journal, LARES_SHELL_FEATURE, 0, 0, &result);
    if (result.changed) lares_journal_forget(journal, LARES_SHELL_FEATURE, 0);
}

int lares_shell_autohide_reapply(lares_journal *journal)
{
    return lares_journal_reapply(journal, LARES_SHELL_FEATURE, LARES_TASKBAR_KIND, LARES_TASKBAR_AUTOHIDE);
}

static int is_autohide(const lares_journal_entry *entry, void *found)
{
    if (lstrcmpA(entry->feature, LARES_SHELL_FEATURE) == 0 && lstrcmpA(entry->kind, LARES_TASKBAR_KIND) == 0 &&
        lstrcmpW(entry->target, LARES_TASKBAR_AUTOHIDE) == 0)
        *(int *)found = 1;
    return 1;
}

int lares_shell_autohide_owed(lares_journal *journal)
{
    int found = 0;
    lares_journal_list(journal, is_autohide, &found);
    return found;
}
