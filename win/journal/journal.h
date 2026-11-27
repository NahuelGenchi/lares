/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The change journal: one small file per user that records every Windows
 * setting Lares changes, so that every one of them can be put back.
 *
 * Each entry says what was changed, the value before, the value after, when,
 * and which feature asked. It is written and flushed to disk BEFORE the
 * setting is touched, and marked "done" after. A crash or a power cut at any
 * point leaves an entry that undo can still act on:
 *
 *   entry torn while being written   the setting was never touched; the torn
 *                                    line is ignored
 *   entry written, change not made   undo finds the value still as before
 *   change made, no "done" mark      undo finds the new value and puts the
 *                                    old one back
 *
 * Settings, the uninstaller and the guard all undo with the one routine
 * below, lares_journal_undo.
 *
 * A change is made through a backend: a pair of functions that read and
 * write one kind of setting (a registry value, the taskbar's auto-hide).
 * journal_win.h has the ones for Windows; the tests bring their own.
 *
 * Plain Win32 and no C runtime, so the guard can link it.
 */
#ifndef LARES_JOURNAL_H
#define LARES_JOURNAL_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARES_JOURNAL_VALUE_MAX 1024
#define LARES_JOURNAL_TARGET_MAX 260
#define LARES_JOURNAL_BACKENDS 4

/* A setting's value: absent, or a type and its bytes. The types are the
 * registry's (REG_DWORD, REG_SZ, ...); other backends use REG_DWORD. */
typedef struct lares_journal_value {
    int present;
    DWORD type;
    DWORD size;
    BYTE data[LARES_JOURNAL_VALUE_MAX];
} lares_journal_value;

enum {
    LARES_JOURNAL_PENDING = 0, /* written, no "done" mark: the change may or may not have been made */
    LARES_JOURNAL_DONE = 1,
    LARES_JOURNAL_UNDONE = 2,
};

typedef struct lares_journal_entry {
    ULONGLONG id;      /* rising, never reused within one file */
    ULONGLONG time;    /* when, as a FILETIME (UTC) */
    char feature[32];  /* which feature asked: "shell", "remote", ... */
    char kind[16];     /* which backend: "registry", "taskbar", ... */
    wchar_t target[LARES_JOURNAL_TARGET_MAX]; /* what, in the backend's own words */
    lares_journal_value before;
    lares_journal_value after;
    int state;
} lares_journal_entry;

/* Reads and writes one kind of setting. Both return nonzero on success;
 * reading something that is not there succeeds with `present` 0, and
 * writing a value with `present` 0 removes the setting. */
typedef struct lares_journal_backend {
    const char *kind;
    int (*read)(const wchar_t *target, lares_journal_value *value, void *ctx);
    int (*write)(const wchar_t *target, const lares_journal_value *value, void *ctx);
    void *ctx;
} lares_journal_backend;

typedef struct lares_journal {
    wchar_t path[MAX_PATH];
    lares_journal_backend backends[LARES_JOURNAL_BACKENDS];
    int backend_count;
} lares_journal;

/* `path` NULL means the user's own journal: the file LARES_JOURNAL names
 * when set (tests), otherwise journal.log in Lares' folder under the local
 * application data. Nothing is created until the first change. */
int lares_journal_open(lares_journal *journal, const wchar_t *path);
int lares_journal_add_backend(lares_journal *journal, const lares_journal_backend *backend);

/* Changes a setting, the only way Lares may: reads the value before, writes
 * and flushes the entry, makes the change, marks it done. Returns 0 and
 * changes nothing when the entry could not be written. A setting that
 * already has the wanted value gets no entry. `id` may be NULL. */
int lares_journal_change(lares_journal *journal, const char *feature, const char *kind, const wchar_t *target,
                         const lares_journal_value *after, ULONGLONG *id);

/* The two halves of lares_journal_change, for callers that must make the
 * change themselves between them. */
int lares_journal_begin(lares_journal *journal, const char *feature, const char *kind, const wchar_t *target,
                        const lares_journal_value *before, const lares_journal_value *after, ULONGLONG *id);
int lares_journal_complete(lares_journal *journal, ULONGLONG id);

typedef struct lares_journal_undo_result {
    int restored; /* put back to the value before */
    int already;  /* found already at the value before (the change was never made, or was undone outside) */
    int changed;  /* left alone: the setting no longer has the value Lares gave it, so someone else changed it since */
    int failed;   /* no backend for it, or the setting could not be read or written */
} lares_journal_undo_result;

/* Undo, newest entry first. `feature` NULL undoes every feature's entries;
 * `id` 0 undoes all of them, otherwise only that one. An entry whose setting
 * was changed by someone else since is left alone and stays in the journal,
 * unless `force` is set. Returns nonzero when nothing failed. `result` may
 * be NULL. When no entry is left to undo, the file is removed. */
int lares_journal_undo(lares_journal *journal, const char *feature, ULONGLONG id, int force,
                       lares_journal_undo_result *result);

/* Makes an owed change hold again: when the newest entry for this target
 * is not undone and the setting no longer has the value it set (Windows put
 * it back by itself, say), writes that value again. No new entry. Done
 * under the journal's lock, so it can never land after an undo of the same
 * entry. Returns nonzero when the change is owed and holds now. */
int lares_journal_reapply(lares_journal *journal, const char *feature, const char *kind, const wchar_t *target);

/* Gives up on entries without touching their settings: they are marked
 * undone as they are. For a setting someone else has changed since, when
 * their value is to stay for good. Same choice of entries as undo. */
int lares_journal_forget(lares_journal *journal, const char *feature, ULONGLONG id);

/* Calls `each` for every entry not yet undone, oldest first, until it
 * returns 0. Returns how many there are. `each` may be NULL. */
int lares_journal_list(lares_journal *journal, int (*each)(const lares_journal_entry *entry, void *ctx), void *ctx);

/* Small helpers for values. */
void lares_journal_value_dword(lares_journal_value *value, DWORD number);
void lares_journal_value_absent(lares_journal_value *value);
int lares_journal_value_equal(const lares_journal_value *a, const lares_journal_value *b);

#ifdef __cplusplus
}
#endif

#endif
