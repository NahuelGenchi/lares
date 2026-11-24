// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* The check every name from a peer passes before Lares shows it or creates
 * a file by it, the UTI a sent file is described with, and the text helpers
 * the formats share in place of GLib's. Ported from owly's extraction rules
 * and sender (MIT), as src/airdrop/names.c and session.c are. */
#include "airdrop_text.h"
#include <cstring>

namespace lares::airdrop {

/* ------------------------------------------------------------ text */

bool utf8_valid(const char *s, size_t n)
{
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        if (c == 0) return false;
        if (c < 0x80) {
            i++;
            continue;
        }
        /* RFC 3629 §4: the second byte's range rules out overlong forms,
         * surrogates and code points past U+10FFFF. */
        size_t more;
        unsigned lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) {
            more = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            more = 2;
            if (c == 0xe0) lo = 0xa0;
            if (c == 0xed) hi = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            more = 3;
            if (c == 0xf0) lo = 0x90;
            if (c == 0xf4) hi = 0x8f;
        } else {
            return false;
        }
        if (n - i - 1 < more) return false;
        if (p[i + 1] < lo || p[i + 1] > hi) return false;
        for (size_t k = 2; k <= more; k++)
            if ((p[i + k] & 0xc0) != 0x80) return false;
        i += 1 + more;
    }
    return true;
}

uint32_t utf8_next(const char *s, size_t &i)
{
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    uint32_t c = p[i];
    if (c < 0x80) {
        i += 1;
        return c;
    }
    size_t more = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    c &= more == 1 ? 0x1fu : more == 2 ? 0x0fu : 0x07u;
    for (size_t k = 1; k <= more; k++) c = c << 6 | (p[i + k] & 0x3fu);
    i += 1 + more;
    return c;
}

void utf8_append(std::string &out, uint32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

bool ascii_case_equal(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

bool ascii_case_equal(const std::string &a, const std::string &b)
{
    return a.size() == b.size() && ascii_case_equal(a.data(), b.data(), a.size());
}

std::string ascii_down(const char *s, size_t n)
{
    std::string out(s, n);
    for (char &c : out) c = ascii_lower(c);
    return out;
}

/* ------------------------------------------------------------ names */

/* Longest component in bytes: what ext4, btrfs and most other Linux file
 * systems take as one name, and NTFS's 255 UTF-16 units never fall short
 * of it. */
constexpr size_t MAX_COMPONENT = 255;

/* Bidi embeddings and overrides (U+202A to U+202E) and isolates (U+2066 to
 * U+2069), which can make a name display as another: "photo" U+202E "gpj.exe"
 * shows as "photoexe.jpg". The plain direction marks (U+200E, U+200F) stay
 * allowed, as real names carry them (SEC-1). */
static bool is_bidi_control(uint32_t c)
{
    return (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}

/* Unicode's Cc: C0, DEL and C1. */
static bool is_control(uint32_t c)
{
    return c < 0x20 || (c >= 0x7f && c <= 0x9f);
}

bool airdrop_safe_name(const std::string &name)
{
    if (name.empty() || name == "." || name == "..") return false;
    if (name.size() > MAX_COMPONENT) return false;
    /* A NUL would cut the name short on Linux; here it's refused. */
    if (!utf8_valid(name.data(), name.size())) return false;
    for (size_t i = 0; i < name.size();) {
        uint32_t c = utf8_next(name.data(), i);
        /* A backslash separates paths on Windows, so owly refuses it and so
         * does Lares. */
        if (c == '/' || c == '\\' || is_control(c) || is_bidi_control(c)) return false;
    }
    return true;
}

bool airdrop_safe_name(const char *name)
{
    return name && airdrop_safe_name(std::string(name));
}

/* owly's table (proto/sender.rs), for FileType. */
const char *airdrop_uti(const std::string &name, bool is_directory)
{
    static const struct {
        const char *ext, *uti;
    } TABLE[] = {
        { "jpg", "public.jpeg" }, { "jpeg", "public.jpeg" }, { "png", "public.png" },
        { "gif", "com.compuserve.gif" }, { "heic", "public.heic" }, { "heif", "public.heif" },
        { "tif", "public.tiff" }, { "tiff", "public.tiff" }, { "webp", "org.webmproject.webp" },
        { "mov", "com.apple.quicktime-movie" }, { "mp4", "public.mpeg-4" }, { "m4a", "com.apple.m4a-audio" },
        { "mp3", "public.mp3" }, { "wav", "com.microsoft.waveform-audio" }, { "pdf", "com.adobe.pdf" },
        { "txt", "public.plain-text" }, { "text", "public.plain-text" }, { "md", "net.daringfireball.markdown" },
        { "html", "public.html" }, { "htm", "public.html" }, { "rtf", "public.rtf" }, { "vcf", "public.vcard" },
        { "ics", "com.apple.ical.ics" }, { "zip", "public.zip-archive" }, { "json", "public.json" },
        { "csv", "public.comma-separated-values-text" }, { "doc", "com.microsoft.word.doc" },
        { "docx", "org.openxmlformats.wordprocessingml.document" }, { "xls", "com.microsoft.excel.xls" },
        { "xlsx", "org.openxmlformats.spreadsheetml.sheet" }, { "ppt", "com.microsoft.powerpoint.ppt" },
        { "pptx", "org.openxmlformats.presentationml.presentation" }, { "key", "com.apple.keynote.key" },
        { "pages", "com.apple.iwork.pages.sffpages" }, { "numbers", "com.apple.iwork.numbers.sffnumbers" },
    };
    if (is_directory) return "public.folder";
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "public.data";
    std::string ext = name.substr(dot + 1);
    for (const auto &t : TABLE)
        if (ascii_case_equal(ext, std::string(t.ext))) return t.uti;
    return "public.data";
}

} // namespace lares::airdrop
