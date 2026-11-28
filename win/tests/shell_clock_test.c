// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar clock's formatter (issue #165, win/shell/clockfmt.c): every
 * letter run, quoted text, 12 and 24 hours at midnight and noon, and what a
 * short buffer keeps. */
#include "clockfmt.h"

#include <stdio.h>
#include <wchar.h>

static int checks, failures;

static SYSTEMTIME at(int year, int month, int day, int weekday, int hour, int minute, int second)
{
    SYSTEMTIME st;
    ZeroMemory(&st, sizeof(st));
    st.wYear = (WORD)year;
    st.wMonth = (WORD)month;
    st.wDay = (WORD)day;
    st.wDayOfWeek = (WORD)weekday;
    st.wHour = (WORD)hour;
    st.wMinute = (WORD)minute;
    st.wSecond = (WORD)second;
    return st;
}

static void expect_len(const wchar_t *pic, const SYSTEMTIME *st, size_t len, const wchar_t *want)
{
    wchar_t out[128];
    wmemset(out, L'#', 128);
    clock_format_picture(pic, st, out, len);
    ++checks;
    if (wcscmp(out, want) != 0) {
        ++failures;
        wprintf(L"  FAILED: \"%ls\" in %u gave \"%ls\", not \"%ls\"\n", pic ? pic : L"(none)", (unsigned)len, out, want);
    }
}

static void expect(const wchar_t *pic, const SYSTEMTIME *st, const wchar_t *want)
{
    expect_len(pic, st, 128, want);
}

static void expect_true(int cond, const char *what)
{
    ++checks;
    if (!cond) {
        ++failures;
        printf("  FAILED: %s\n", what);
    }
}

int main(void)
{
    /* Friday 6 March 2026 */
    SYSTEMTIME midnight = at(2026, 3, 6, 5, 0, 5, 9);
    SYSTEMTIME noon = at(2026, 3, 6, 5, 12, 30, 0);
    SYSTEMTIME morning = at(2005, 11, 9, 3, 9, 7, 3); /* Wednesday 9 November 2005, 09:07:03 */
    SYSTEMTIME evening = at(2026, 12, 31, 4, 23, 59, 59);

    /* 12 and 24 hours at midnight and noon */
    expect(L"HH:mm", &midnight, L"00:05");
    expect(L"H:mm", &midnight, L"0:05");
    expect(L"h:mm tt", &midnight, L"12:05 AM");
    expect(L"hh:mm t", &midnight, L"12:05 A");
    expect(L"HH:mm", &noon, L"12:30");
    expect(L"h:mm tt", &noon, L"12:30 PM");
    expect(L"hh:mm t", &noon, L"12:30 P");
    expect(L"h:mm tt", &evening, L"11:59 PM");
    expect(L"HH:mm:ss", &evening, L"23:59:59");

    /* every letter run */
    expect(L"d", &morning, L"9");
    expect(L"dd", &morning, L"09");
    expect(L"ddd", &morning, L"Wed");
    expect(L"dddd", &morning, L"Wednesday");
    expect(L"ddddd", &morning, L"Wednesday");
    expect(L"M", &morning, L"11");
    expect(L"M", &midnight, L"3");
    expect(L"MM", &midnight, L"03");
    expect(L"MMM", &midnight, L"Mar");
    expect(L"MMMM", &midnight, L"March");
    expect(L"y", &morning, L"5");
    expect(L"yy", &morning, L"05");
    expect(L"yyy", &morning, L"2005");
    expect(L"yyyy", &midnight, L"2026");
    expect(L"H", &morning, L"9");
    expect(L"HH", &morning, L"09");
    expect(L"h", &morning, L"9");
    expect(L"hh", &morning, L"09");
    expect(L"m", &morning, L"7");
    expect(L"mm", &morning, L"07");
    expect(L"s", &morning, L"3");
    expect(L"ss", &morning, L"03");
    expect(L"t", &morning, L"A");
    expect(L"tt", &morning, L"AM");

    /* the default picture and the date styles */
    expect(L"ddd dd MMM, HH:mm", &midnight, L"Fri 06 Mar, 00:05");
    expect(L"MMM d", &midnight, L"Mar 6");
    expect(L"dd/MM/yyyy", &midnight, L"06/03/2026");
    expect(L"MM/dd/yyyy", &midnight, L"03/06/2026");
    expect(L"yyyy-MM-dd", &midnight, L"2026-03-06");

    /* quoted text, and other characters copied */
    expect(L"'at' HH:mm", &noon, L"at 12:30");
    expect(L"HH'h'mm", &noon, L"12h30");
    expect(L"'dd MMM yyyy'", &noon, L"dd MMM yyyy");
    expect(L"''HH''", &noon, L"'12'");
    expect(L"'open", &noon, L"open");
    expect(L"HH.mm !", &noon, L"12.30 !");
    expect(L"", &noon, L"");
    expect(NULL, &noon, L"");

    /* a month out of range reads as January rather than past the table */
    SYSTEMTIME odd = at(2026, 0, 1, 4, 8, 0, 0);
    expect(L"MMM", &odd, L"Jan");

    /* a short buffer keeps what fits, always ended */
    expect_len(L"dddd", &midnight, 6, L"Frida");
    expect_len(L"HH:mm", &noon, 3, L"12");
    expect_len(L"'abc'", &noon, 2, L"a");
    expect_len(L"HH:mm", &noon, 1, L"");
    {
        wchar_t untouched[2] = { L'#', 0 };
        clock_format_picture(L"HH", &noon, untouched, 0);
        expect_true(untouched[0] == L'#', "a buffer of no length is left alone");
    }

    /* seconds, so the bar knows to repaint every second */
    expect_true(clock_picture_has_seconds(L"HH:mm:ss"), "HH:mm:ss shows seconds");
    expect_true(!clock_picture_has_seconds(L"ddd dd MMM, HH:mm"), "the default shows no seconds");
    expect_true(!clock_picture_has_seconds(L"HH:mm 'secs'"), "an s in quotes is no seconds");
    expect_true(clock_picture_has_seconds(L"'at' s"), "an s after quotes is seconds");

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
