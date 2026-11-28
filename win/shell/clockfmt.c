// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar clock's text from a picture string (issue #165): clockfmt.h. */
#include "clockfmt.h"
#include <stdio.h>

static const wchar_t *k_day_short[7]    = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
static const wchar_t *k_day_long[7]     = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
                                            L"Thursday", L"Friday", L"Saturday" };
static const wchar_t *k_month_short[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                            L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
static const wchar_t *k_month_long[12]  = { L"January", L"February", L"March", L"April", L"May", L"June",
                                            L"July", L"August", L"September", L"October", L"November",
                                            L"December" };

typedef struct {
    wchar_t *p;
    size_t left;
} Out;

static void out_str(Out *o, const wchar_t *s) {
    while (*s && o->left > 1) {
        *o->p++ = *s++;
        o->left--;
    }
    *o->p = 0;
}

static void out_num(Out *o, unsigned v, int width) {
    wchar_t tmp[16];
    _snwprintf_s(tmp, 16, _TRUNCATE, width == 2 ? L"%02u" : (width == 4 ? L"%04u" : L"%u"), v);
    out_str(o, tmp);
}

void clock_format_picture(const wchar_t *pic, const SYSTEMTIME *st, wchar_t *out, size_t len) {
    if (!out || len == 0) return;
    Out o = { out, len };
    out[0] = 0;
    if (!pic || !st) return;
    unsigned mon = (st->wMonth >= 1 && st->wMonth <= 12) ? st->wMonth : 1;
    unsigned hr12 = st->wHour % 12;
    if (hr12 == 0) hr12 = 12;

    for (const wchar_t *c = pic; *c;) {
        wchar_t ch = *c;
        if (ch == L'\'') { /* 'text' is copied as written, '' is one quote */
            c++;
            if (*c == L'\'') {
                out_str(&o, L"'");
                c++;
                continue;
            }
            while (*c && *c != L'\'') {
                wchar_t one[2] = { *c, 0 };
                out_str(&o, one);
                c++;
            }
            if (*c == L'\'') c++;
            continue;
        }
        int run = 0;
        while (c[run] == ch) run++;
        switch (ch) {
        case L'd':
            if (run >= 4) out_str(&o, k_day_long[st->wDayOfWeek % 7]);
            else if (run == 3) out_str(&o, k_day_short[st->wDayOfWeek % 7]);
            else out_num(&o, st->wDay, run);
            break;
        case L'M':
            if (run >= 4) out_str(&o, k_month_long[mon - 1]);
            else if (run == 3) out_str(&o, k_month_short[mon - 1]);
            else out_num(&o, mon, run);
            break;
        case L'y':
            if (run <= 2) out_num(&o, st->wYear % 100, run == 1 ? 1 : 2);
            else out_num(&o, st->wYear, 4);
            break;
        case L'H': out_num(&o, st->wHour, run >= 2 ? 2 : 1); break;
        case L'h': out_num(&o, hr12, run >= 2 ? 2 : 1); break;
        case L'm': out_num(&o, st->wMinute, run >= 2 ? 2 : 1); break;
        case L's': out_num(&o, st->wSecond, run >= 2 ? 2 : 1); break;
        case L't':
            if (run >= 2) out_str(&o, st->wHour >= 12 ? L"PM" : L"AM");
            else out_str(&o, st->wHour >= 12 ? L"P" : L"A");
            break;
        default: {
            wchar_t one[2] = { ch, 0 };
            for (int i = 0; i < run; i++) out_str(&o, one);
        } break;
        }
        c += run;
    }
}

int clock_picture_has_seconds(const wchar_t *pic) {
    int quoted = 0;
    for (const wchar_t *p = pic; p && *p; p++) {
        if (*p == L'\'') quoted = !quoted;
        else if (!quoted && *p == L's') return 1;
    }
    return 0;
}
