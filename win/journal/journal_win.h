/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The journal's backend for registry values (journal.h).
 */
#ifndef LARES_JOURNAL_WIN_H
#define LARES_JOURNAL_WIN_H

#include "journal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kind "registry". A target is the key, a '|' and the value's name:
 *   HKCU\Software\Microsoft\Windows\CurrentVersion\Run|LaresPad
 * HKCU and HKLM are known. A value that is not there reads as absent, and
 * writing an absent value deletes it; keys are made when needed and never
 * removed. A value larger than LARES_JOURNAL_VALUE_MAX cannot be read, so it
 * cannot be changed through the journal. */
extern const lares_journal_backend lares_journal_registry;

/* Kind "registry-key". A target is a whole key, with no '|':
 *   HKCU\Software\Classes\Lares.Markdown
 * Present while the key is there. Making it present creates it; making it
 * absent deletes it only while it holds no value and no key, so undo takes
 * away a key Lares made and leaves one someone else has put things in. */
extern const lares_journal_backend lares_journal_registry_key;

/* A string value (REG_SZ) from text. Returns 0 when it does not fit. */
int lares_journal_value_string(lares_journal_value *value, const wchar_t *text);

#ifdef __cplusplus
}
#endif

#endif
