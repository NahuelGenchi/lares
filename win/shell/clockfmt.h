// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar clock's text from a picture string (issue #165), with the
 * letters Windows uses for dates and times, so a picture reads the same as
 * in Windows' own settings:
 *
 *   d dd ddd dddd     day: 7, 07, Fri, Friday
 *   M MM MMM MMMM     month: 3, 03, Mar, March
 *   yy yyyy           year: 26, 2026
 *   H HH  h hh        hour, 24-hour and 12-hour: 9, 09
 *   m mm  s ss        minutes and seconds
 *   t tt              A or P, AM or PM
 *   'text'            written as is; '' is one quote
 *
 * Anything else is copied. Free of the shell, so tests link it alone. */
#ifndef LARES_CLOCKFMT_H
#define LARES_CLOCKFMT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes `pic` for `st` into `out`, cut to `len` with its terminator. */
void clock_format_picture(const wchar_t *pic, const SYSTEMTIME *st, wchar_t *out, size_t len);

/* Whether `pic` shows seconds: an 's' outside quoted text. */
int clock_picture_has_seconds(const wchar_t *pic);

#ifdef __cplusplus
}
#endif

#endif
