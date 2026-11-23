// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* AirDrop's error domain, and the check every name from a peer passes
 * before Lares shows it or creates a file by it. Ported from owly's
 * extraction rules (MIT). */
#include "formats.h"
#include <string.h>

G_DEFINE_QUARK(airdrop-error-quark, airdrop_error)

/* Longest component in bytes: what ext4, btrfs and most other Linux file
 * systems take as one name. */
#define MAX_COMPONENT 255

/* Bidi embeddings and overrides (U+202A to U+202E) and isolates (U+2066 to
 * U+2069), which can make a name display as another: "photo" U+202E "gpj.exe"
 * shows as "photoexe.jpg". The plain direction marks (U+200E, U+200F) stay
 * allowed, as real names carry them (SEC-1). */
static gboolean is_bidi_control(gunichar c)
{
    return (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}

gboolean airdrop_safe_name(const char *name)
{
    if (!name || !*name) return FALSE;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return FALSE;
    if (strlen(name) > MAX_COMPONENT) return FALSE;
    if (!g_utf8_validate(name, -1, NULL)) return FALSE;
    for (const char *p = name; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        /* A backslash separates paths on Windows, so owly refuses it and so
         * does Lares. Control characters are Unicode's Cc: C0, DEL and C1. */
        if (c == '/' || c == '\\' || g_unichar_iscntrl(c) || is_bidi_control(c)) return FALSE;
    }
    return TRUE;
}
