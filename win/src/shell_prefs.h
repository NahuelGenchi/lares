/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The shell's settings for its C code (issue #141). They live in Lares' own
 * configuration: config.cpp reads and writes %APPDATA%\lares\shell.ini, or
 * shell-dry.ini when LARES_DRY_RUN is set. Every part is off until it is
 * turned on in Settings. Nothing is read from the program's own folder.
 */
#ifndef LARES_SHELL_PREFS_H
#define LARES_SHELL_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LARES_DOCK_ALWAYS_SHOWN = 0, /* reserves its strip of the screen */
    LARES_DOCK_AUTO_HIDE = 1,
    LARES_DOCK_WHEN_COVERED = 2  /* hides when a window covers it */
} LaresDockHide;

typedef struct {
    /* The parts, each off by default. */
    bool bar;
    bool dock;
    bool launcher;
    bool launcher_windows_key;
    bool clipboard;
    bool screenshot;
    bool files;
    bool viewers;
    /* How they behave. */
    bool bar_auto_hide;
    bool week_starts_monday;
    bool bar_airdrop;
    bool bar_indicators;
    int dock_hide; /* LaresDockHide */
    bool dock_window_counts;
    bool dock_middle_click_new;
    bool dock_media;
    bool animations;
    /* The top bar's clock (issue #165). */
    bool clock_24h;
    bool clock_seconds;
    bool clock_weekday;
    bool clock_date;
    int clock_date_style;  /* 0 dd MMM, 1 MMM d, 2 dd/MM/yyyy, 3 MM/dd/yyyy, 4 yyyy-MM-dd */
    wchar_t clock_format[64]; /* a picture of the user's own; empty: the choices above */
} LaresShellPrefs;

#define LARES_PIN_NAME 128
#define LARES_PIN_APP_ID 256
#define LARES_PIN_PATH 260
#define LARES_PIN_ARGS 512

typedef struct {
    wchar_t name[LARES_PIN_NAME];
    wchar_t app_id[LARES_PIN_APP_ID]; /* a Store app's AppUserModelID, or empty */
    wchar_t path[LARES_PIN_PATH];     /* a program file, or empty */
    wchar_t args[LARES_PIN_ARGS];
    wchar_t icon[LARES_PIN_PATH];     /* a picture, absolute or in the settings folder */
} LaresDockPin;

/* True when LARES_DRY_RUN is set: settings go to their own file. */
bool lares_shell_dry_run(void);

/* Reads the settings; a missing file gives the defaults (everything off). */
void lares_shell_prefs_load(LaresShellPrefs *out);
/* Writes the switches and behaviour, reading the file again first so the
 * pins, and anything another Lares program wrote, stay as they are. */
bool lares_shell_prefs_save(const LaresShellPrefs *prefs);

/* The pins saved in the file, up to max; returns how many, or -1 when none
 * were ever saved (the shell then picks its first set itself). */
int lares_shell_pins_load(LaresDockPin *out, int max);
/* Saves the pins, keeping every other setting as it is in the file. */
bool lares_shell_pins_save(const LaresDockPin *pins, int count);

/* Files' bookmarked folders (issue #153). */
#define LARES_BOOKMARK_NAME 64

typedef struct {
    wchar_t name[LARES_BOOKMARK_NAME];
    wchar_t path[LARES_PIN_PATH];
} LaresBookmark;

/* The saved bookmarks, up to max; returns how many. */
int lares_shell_bookmarks_load(LaresBookmark *out, int max);
/* Saves the bookmarks, keeping every other setting as it is in the file. */
bool lares_shell_bookmarks_save(const LaresBookmark *marks, int count);

/* The folder of Lares' settings, %APPDATA%\lares. */
bool lares_shell_settings_dir(wchar_t *out, size_t cch);
/* The full path of the settings file in use. */
bool lares_shell_settings_file(wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif

#endif
