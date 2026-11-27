/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Local crash reports. When a Lares program crashes it writes one small
 * text file on this PC and ends at once. Nothing is ever sent anywhere:
 * this code opens no network connection and starts no program.
 *
 * A report holds the program and its version, the Windows version, what
 * went wrong, the faulting module and the call stack as module names and
 * offsets. It holds no file name other than those modules' own, no window
 * title, nothing from the clipboard, and neither the account's nor the
 * computer's name; a user folder path is replaced with a placeholder before
 * the file is written (lares_crash_scrub), whatever part of the report it
 * came from.
 *
 * Plain Win32 and no C runtime, so every Lares program can link it and the
 * handler does not lean on a runtime that may be the thing that broke.
 */
#ifndef LARES_CRASH_H
#define LARES_CRASH_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARES_CRASH_REPORT_MAX 16384
#define LARES_CRASH_KEEP 10
#define LARES_CRASH_FRAMES 48

/* Call once at start. `program` is a short name such as "lares" or
 * "lares-pad"; `version` is the build's version text. From then on an
 * unhandled crash writes a report and ends the process with the crash's own
 * code, without waiting on Windows' crash handling. Also removes all but
 * the newest LARES_CRASH_KEEP reports. */
void lares_crash_install(const char *program, const char *version);

/* The folder reports are written to: LARES_CRASH_DIR when set (tests),
 * otherwise crash\ in Lares' folder under the local application data. */
int lares_crash_dir(wchar_t *dir, int capacity);

/* The newest report's path; 0 when there is none. */
int lares_crash_latest(wchar_t *path, int capacity);

/* A report's text, UTF-8, at most LARES_CRASH_REPORT_MAX bytes with its
 * ending zero. Returns its length, 0 when it cannot be read. */
int lares_crash_read(const wchar_t *path, char *text, int capacity);

/* What must never reach a report. */
typedef struct lares_crash_private {
    const char *user_folder; /* the user's own folder, e.g. from USERPROFILE; may be NULL */
    const char *account;     /* the account's name; may be NULL */
    const char *computer;    /* the computer's name; may be NULL */
} lares_crash_private;

/* Removes private details from `text` in place and returns its new length:
 *   the user's own folder                      becomes  %USERPROFILE%
 *   X:\Users\<anyone>, /home/<anyone>, ...     becomes  X:\Users\<name>
 *   the account's name as a word               becomes  <name>
 *   the computer's name as a word              becomes  <pc>
 * Letter case does not matter and either slash is recognised. */
int lares_crash_scrub(char *text, int length, int capacity, const lares_crash_private *priv);

/* One frame of a call stack, or the faulting place. */
typedef struct lares_crash_frame {
    char module[64];  /* the module's file name without its folder; "" when unknown */
    ULONGLONG offset; /* from the module's start, or the bare address when unknown */
} lares_crash_frame;

typedef struct lares_crash_info {
    const char *program;
    const char *version;
    DWORD windows_major, windows_minor, windows_build;
    const char *machine; /* "x64", "arm64", "x86" */
    SYSTEMTIME when;     /* UTC */
    DWORD code;          /* the exception's code */
    int access;          /* for an access violation: 0 reading, 1 writing, 8 running; -1 otherwise */
    ULONGLONG address;   /* for an access violation: the address touched */
    int frame_count;
    lares_crash_frame frames[LARES_CRASH_FRAMES]; /* frames[0] is the faulting place */
} lares_crash_info;

/* The report's text for `info`, already scrubbed with `priv`. Returns its
 * length. This is all a report ever holds. */
int lares_crash_format(const lares_crash_info *info, const lares_crash_private *priv, char *text, int capacity);

#ifdef __cplusplus
}
#endif

#endif
