/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Other programs Lares works with, found the way Windows finds them and
 * never at a fixed place (issue #141). Each returns false when the program
 * is not installed, and what needs it hides.
 */
#ifndef LARES_PROGRAMS_H
#define LARES_PROGRAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tailscale.exe, Tailscale's command line: next to the program its Windows
 * service runs, else on the PATH. */
bool lares_find_tailscale(wchar_t *out, size_t cch);

/* A terminal: Windows Terminal (its app alias, wt.exe), else PowerShell 7,
 * else Windows PowerShell, else the command prompt Windows names in ComSpec.
 * name gets a name to show; either buffer may be NULL. */
bool lares_find_terminal(wchar_t *exe, size_t cch, wchar_t *name, size_t name_cch);

/* The browser Windows opens web links with, and its name. */
bool lares_find_browser(wchar_t *exe, size_t cch, wchar_t *name, size_t name_cch);

/* A program of Windows' own, in the Windows folder (explorer.exe) or in its
 * System32 folder (anything else), wherever Windows is installed. */
bool lares_windows_program(const wchar_t *file, wchar_t *out, size_t cch);

/* The folder of a service's program, from its command line as the service
 * configuration holds it: quoted or not, with arguments or without.
 * Exposed for tests. */
bool lares_service_program_folder(const wchar_t *image_path, wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif

#endif
