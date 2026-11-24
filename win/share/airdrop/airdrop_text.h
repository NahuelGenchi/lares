// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* What the formats share in place of GLib's: strict UTF-8, ASCII case, and
 * setting an Error. Private to win/share/airdrop. */
#include "formats.h"

namespace lares::airdrop {

/* Whether `n` bytes are UTF-8 as g_utf8_validate() takes them with a
 * length: RFC 3629's (no overlong forms, no surrogates, nothing past
 * U+10FFFF), and no NUL. */
bool utf8_valid(const char *s, size_t n);

/* The code point at s[i] of a string utf8_valid() took; moves i past it. */
uint32_t utf8_next(const char *s, size_t &i);

/* Appends code point `c` (a valid one) as UTF-8. */
void utf8_append(std::string &out, uint32_t c);

inline char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool ascii_digit(int c)
{
    return c >= '0' && c <= '9';
}

inline bool ascii_alnum(int c)
{
    return ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* A hex digit's value, -1 for anything else. */
inline int xdigit_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `n` bytes equal ignoring ASCII case, as g_ascii_strncasecmp() == 0 for
 * bytes with no NUL. */
bool ascii_case_equal(const char *a, const char *b, size_t n);
/* g_ascii_strcasecmp() == 0. */
bool ascii_case_equal(const std::string &a, const std::string &b);
/* g_ascii_strdown(). */
std::string ascii_down(const char *s, size_t n);

inline void set_error(Error *error, ErrorCode code, std::string message)
{
    if (error) error->assign(code, std::move(message));
}

/* g_propagate_error(): hands a callback's error on as it is. */
inline void propagate(Error *error, const Error &local)
{
    if (error) *error = local;
}

} // namespace lares::airdrop
