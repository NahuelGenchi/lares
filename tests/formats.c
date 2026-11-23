// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for AirDrop's data formats (src/airdrop/formats.h): safe names,
 * binary property lists, CPIO archives, gzip and DVZip, and HTTP framing.
 * Run from the repository's root.
 *
 * The vectors named owly's come from owly's conformance set
 * (tests/vectors/airdrop-vectors.json, MIT), which rebuilds AirDrop's
 * messages from their documented shapes. The PY_ ones were written by
 * Python's plistlib, which lays a plist out as Core Foundation does,
 * sharing repeated values. Every other input is built here by hand. */
#include <gio/gio.h>
#include <string.h>
#include "../src/airdrop/formats.h"

#define CAFE_JPG "Caf\xc3\xa9 \xe2\x98\x95.jpg"   /* "Café ☕.jpg" */
#define SMILE    "\xf0\x9f\x98\x80"               /* U+1F600, a surrogate pair in UTF-16 */
#define MTIME    1700000000

#define assert_failed(err, code)                         \
    do {                                                 \
        g_assert_error(err, AIRDROP_ERROR, (int)(code)); \
        g_clear_error(&(err));                           \
    } while (0)

/* ------------------------------------------------------------ vectors */

/* owly's "ios18-hello" /Ask body. */
static const guint8 ASK_HELLO[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd9, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1f, 0x58, 0x42, 0x75, 0x6e, 0x64,
    0x6c, 0x65, 0x49, 0x44, 0x5f, 0x10, 0x13, 0x43, 0x6f, 0x6e, 0x76, 0x65, 0x72, 0x74, 0x4d, 0x65,
    0x64, 0x69, 0x61, 0x46, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x73, 0x55, 0x46, 0x69, 0x6c, 0x65, 0x73,
    0x55, 0x49, 0x74, 0x65, 0x6d, 0x73, 0x5f, 0x10, 0x12, 0x53, 0x65, 0x6e, 0x64, 0x65, 0x72, 0x43,
    0x6f, 0x6d, 0x70, 0x75, 0x74, 0x65, 0x72, 0x4e, 0x61, 0x6d, 0x65, 0x58, 0x53, 0x65, 0x6e, 0x64,
    0x65, 0x72, 0x49, 0x44, 0x5f, 0x10, 0x0f, 0x53, 0x65, 0x6e, 0x64, 0x65, 0x72, 0x4d, 0x6f, 0x64,
    0x65, 0x6c, 0x4e, 0x61, 0x6d, 0x65, 0x5a, 0x54, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x65, 0x72, 0x49,
    0x44, 0x5c, 0x54, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x65, 0x72, 0x54, 0x79, 0x70, 0x65, 0x5f, 0x10,
    0x10, 0x63, 0x6f, 0x6d, 0x2e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x2e, 0x66, 0x69, 0x6e, 0x64, 0x65,
    0x72, 0x08, 0xa1, 0x0d, 0xd6, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x0b, 0x15, 0x16, 0x17,
    0x0b, 0x5b, 0x46, 0x69, 0x6c, 0x65, 0x42, 0x6f, 0x6d, 0x50, 0x61, 0x74, 0x68, 0x5f, 0x10, 0x0f,
    0x46, 0x69, 0x6c, 0x65, 0x49, 0x73, 0x44, 0x69, 0x72, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x79, 0x58,
    0x46, 0x69, 0x6c, 0x65, 0x4e, 0x61, 0x6d, 0x65, 0x58, 0x46, 0x69, 0x6c, 0x65, 0x53, 0x69, 0x7a,
    0x65, 0x58, 0x46, 0x69, 0x6c, 0x65, 0x54, 0x79, 0x70, 0x65, 0x5f, 0x10, 0x19, 0x53, 0x68, 0x6f,
    0x75, 0x6c, 0x64, 0x43, 0x6f, 0x6e, 0x76, 0x65, 0x72, 0x74, 0x4d, 0x65, 0x64, 0x69, 0x61, 0x46,
    0x6f, 0x72, 0x6d, 0x61, 0x74, 0x73, 0x5b, 0x2e, 0x2f, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e, 0x74,
    0x78, 0x74, 0x59, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e, 0x74, 0x78, 0x74, 0x10, 0x0c, 0x5f, 0x10,
    0x11, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63, 0x2e, 0x70, 0x6c, 0x61, 0x69, 0x6e, 0x2d, 0x74, 0x65,
    0x78, 0x74, 0xa0, 0x58, 0x54, 0x65, 0x73, 0x74, 0x20, 0x4d, 0x61, 0x63, 0x5c, 0x34, 0x38, 0x36,
    0x61, 0x63, 0x32, 0x61, 0x39, 0x34, 0x61, 0x63, 0x36, 0x5b, 0x4d, 0x61, 0x63, 0x42, 0x6f, 0x6f,
    0x6b, 0x20, 0x50, 0x72, 0x6f, 0xd1, 0x1d, 0x1e, 0x52, 0x69, 0x64, 0x5f, 0x10, 0x24, 0x41, 0x33,
    0x46, 0x33, 0x34, 0x45, 0x30, 0x31, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x2d, 0x34, 0x30, 0x30, 0x30,
    0x2d, 0x38, 0x30, 0x30, 0x30, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x31, 0xd1, 0x20, 0x21, 0x55, 0x66, 0x69, 0x6c, 0x65, 0x73, 0xd0, 0x00, 0x08, 0x00, 0x1b,
    0x00, 0x24, 0x00, 0x3a, 0x00, 0x40, 0x00, 0x46, 0x00, 0x5b, 0x00, 0x64, 0x00, 0x76, 0x00, 0x81,
    0x00, 0x8e, 0x00, 0xa1, 0x00, 0xa2, 0x00, 0xa4, 0x00, 0xb1, 0x00, 0xbd, 0x00, 0xcf, 0x00, 0xd8,
    0x00, 0xe1, 0x00, 0xea, 0x01, 0x06, 0x01, 0x12, 0x01, 0x1c, 0x01, 0x1e, 0x01, 0x32, 0x01, 0x33,
    0x01, 0x3c, 0x01, 0x49, 0x01, 0x55, 0x01, 0x58, 0x01, 0x5b, 0x01, 0x82, 0x01, 0x85, 0x01, 0x8b,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x8c,
};

/* owly's "odc, Finder layout" archive: "." and ./hello.txt. */
static const guint8 CPIO_ODC[] = {
    0x30, 0x37, 0x30, 0x37, 0x30, 0x37, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x31, 0x30, 0x34, 0x30, 0x37, 0x35, 0x35, 0x30, 0x30, 0x30, 0x37, 0x36, 0x35, 0x30, 0x30,
    0x30, 0x30, 0x32, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x33, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x31, 0x34, 0x35, 0x32, 0x34, 0x37, 0x37, 0x30, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x32, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2e, 0x00, 0x30, 0x37,
    0x30, 0x37, 0x30, 0x37, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x32,
    0x31, 0x30, 0x30, 0x36, 0x34, 0x34, 0x30, 0x30, 0x30, 0x37, 0x36, 0x35, 0x30, 0x30, 0x30, 0x30,
    0x32, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x34,
    0x35, 0x32, 0x34, 0x37, 0x37, 0x30, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x34, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x34, 0x2e, 0x2f, 0x68, 0x65, 0x6c, 0x6c,
    0x6f, 0x2e, 0x74, 0x78, 0x74, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x77, 0x6f, 0x72, 0x6c,
    0x64, 0x0a, 0x30, 0x37, 0x30, 0x37, 0x30, 0x37, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x31, 0x33, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x54, 0x52,
    0x41, 0x49, 0x4c, 0x45, 0x52, 0x21, 0x21, 0x21, 0x00,
};

/* owly's "newc, Finder layout" archive, the same in newc. */
static const guint8 CPIO_NEWC[] = {
    0x30, 0x37, 0x30, 0x37, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x30, 0x30,
    0x30, 0x30, 0x34, 0x31, 0x65, 0x64, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x66, 0x35, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x31, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35,
    0x35, 0x33, 0x66, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x32, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2e, 0x00,
    0x30, 0x37, 0x30, 0x37, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x32, 0x30, 0x30,
    0x30, 0x30, 0x38, 0x31, 0x61, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x66, 0x35, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x31, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x36, 0x35,
    0x35, 0x33, 0x66, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x63, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x63, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2e, 0x2f,
    0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e, 0x74, 0x78, 0x74, 0x00, 0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c,
    0x6f, 0x20, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x0a, 0x30, 0x37, 0x30, 0x37, 0x30, 0x31, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x62, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x54, 0x52, 0x41, 0x49, 0x4c, 0x45, 0x52, 0x21, 0x21, 0x21,
    0x00, 0x00, 0x00, 0x00,
};

/* owly's "Finder: DVZip, Content-Length, TransferID header" /Upload. */
static const guint8 UPLOAD_FINDER[] = {
    0x50, 0x4f, 0x53, 0x54, 0x20, 0x2f, 0x55, 0x70, 0x6c, 0x6f, 0x61, 0x64, 0x20, 0x48, 0x54, 0x54,
    0x50, 0x2f, 0x31, 0x2e, 0x31, 0x0d, 0x0a, 0x55, 0x73, 0x65, 0x72, 0x2d, 0x41, 0x67, 0x65, 0x6e,
    0x74, 0x3a, 0x20, 0x41, 0x69, 0x72, 0x44, 0x72, 0x6f, 0x70, 0x2f, 0x31, 0x2e, 0x30, 0x0d, 0x0a,
    0x54, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x65, 0x72, 0x49, 0x44, 0x3a, 0x20, 0x41, 0x33, 0x46, 0x33,
    0x34, 0x45, 0x30, 0x31, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x2d, 0x34, 0x30, 0x30, 0x30, 0x2d, 0x38,
    0x30, 0x30, 0x30, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31,
    0x0d, 0x0a, 0x54, 0x6f, 0x74, 0x61, 0x6c, 0x42, 0x79, 0x74, 0x65, 0x73, 0x3a, 0x20, 0x30, 0x0d,
    0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54, 0x79, 0x70, 0x65, 0x3a, 0x20, 0x61,
    0x70, 0x70, 0x6c, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x2f, 0x78, 0x2d, 0x64, 0x76, 0x7a,
    0x69, 0x70, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20,
    0x6b, 0x65, 0x65, 0x70, 0x2d, 0x61, 0x6c, 0x69, 0x76, 0x65, 0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x74,
    0x65, 0x6e, 0x74, 0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a, 0x20, 0x31, 0x31, 0x35, 0x0d,
    0x0a, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x6f, 0x78, 0x9c, 0x33, 0x30, 0x37, 0x00, 0x41, 0x04, 0x30,
    0x34, 0x30, 0x31, 0x30, 0x37, 0x35, 0x05, 0xb2, 0xcc, 0xcd, 0x40, 0xa4, 0x81, 0x91, 0x09, 0x58,
    0xdc, 0x18, 0x2a, 0x6d, 0x62, 0x6a, 0x64, 0x62, 0x6e, 0x6e, 0x60, 0x02, 0x55, 0x6e, 0x84, 0xa4,
    0x55, 0x8f, 0xc1, 0x00, 0xdd, 0x34, 0x23, 0x43, 0x03, 0x03, 0x33, 0x13, 0x13, 0x0c, 0xd3, 0x0c,
    0xb1, 0x9a, 0x66, 0x68, 0x02, 0xd7, 0x68, 0x68, 0xa2, 0xa7, 0x9f, 0x91, 0x9a, 0x93, 0x93, 0xaf,
    0x57, 0x52, 0x51, 0xc2, 0x00, 0x66, 0x29, 0x94, 0xe7, 0x17, 0xe5, 0xa4, 0x70, 0x61, 0xd8, 0x41,
    0x1a, 0x30, 0x34, 0x46, 0xe2, 0x84, 0x04, 0x39, 0x7a, 0xfa, 0xb8, 0x06, 0x29, 0x2a, 0x2a, 0x32,
    0x00, 0x00, 0xde, 0x9d, 0x36, 0xb8,
};

/* owly's "Legacy form: gzip cpio, chunked, no TransferID" /Upload. */
static const guint8 UPLOAD_LEGACY[] = {
    0x50, 0x4f, 0x53, 0x54, 0x20, 0x2f, 0x55, 0x70, 0x6c, 0x6f, 0x61, 0x64, 0x20, 0x48, 0x54, 0x54,
    0x50, 0x2f, 0x31, 0x2e, 0x31, 0x0d, 0x0a, 0x55, 0x73, 0x65, 0x72, 0x2d, 0x41, 0x67, 0x65, 0x6e,
    0x74, 0x3a, 0x20, 0x41, 0x69, 0x72, 0x44, 0x72, 0x6f, 0x70, 0x2f, 0x31, 0x2e, 0x30, 0x0d, 0x0a,
    0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54, 0x79, 0x70, 0x65, 0x3a, 0x20, 0x61, 0x70,
    0x70, 0x6c, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x2f, 0x78, 0x2d, 0x63, 0x70, 0x69, 0x6f,
    0x0d, 0x0a, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x6b, 0x65,
    0x65, 0x70, 0x2d, 0x61, 0x6c, 0x69, 0x76, 0x65, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65, 0x70, 0x74,
    0x3a, 0x20, 0x2a, 0x2f, 0x2a, 0x0d, 0x0a, 0x41, 0x63, 0x63, 0x65, 0x70, 0x74, 0x2d, 0x4c, 0x61,
    0x6e, 0x67, 0x75, 0x61, 0x67, 0x65, 0x3a, 0x20, 0x65, 0x6e, 0x2d, 0x75, 0x73, 0x0d, 0x0a, 0x41,
    0x63, 0x63, 0x65, 0x70, 0x74, 0x2d, 0x45, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20,
    0x62, 0x72, 0x2c, 0x20, 0x67, 0x7a, 0x69, 0x70, 0x2c, 0x20, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74,
    0x65, 0x0d, 0x0a, 0x54, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x65, 0x72, 0x2d, 0x45, 0x6e, 0x63, 0x6f,
    0x64, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x63, 0x68, 0x75, 0x6e, 0x6b, 0x65, 0x64, 0x0d, 0x0a, 0x0d,
    0x0a, 0x36, 0x33, 0x0d, 0x0a, 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x33,
    0x30, 0x37, 0x00, 0x41, 0x04, 0x30, 0x34, 0x34, 0x30, 0x30, 0x33, 0x31, 0x01, 0xb2, 0xcc, 0xcd,
    0x4c, 0x41, 0x02, 0x46, 0x26, 0x10, 0x71, 0xa8, 0xb4, 0x89, 0xa9, 0x91, 0x89, 0xb9, 0xb9, 0x81,
    0x09, 0x8c, 0x8b, 0xd0, 0x68, 0xa2, 0xa7, 0x9f, 0x91, 0x9a, 0x93, 0x93, 0xaf, 0x57, 0x52, 0x51,
    0xc2, 0x00, 0x66, 0x29, 0x94, 0xe7, 0x17, 0xe5, 0xa4, 0x70, 0x19, 0xa0, 0xdb, 0x41, 0x1a, 0x30,
    0x34, 0x46, 0xe2, 0x84, 0x04, 0x39, 0x7a, 0xfa, 0xb8, 0x06, 0x29, 0x2a, 0x2a, 0x32, 0x00, 0x00,
    0xf5, 0xea, 0xaf, 0xaa, 0xbb, 0x00, 0x00, 0x00, 0x0d, 0x0a, 0x30, 0x0d, 0x0a, 0x0d, 0x0a,
};

/* plistlib: {Name: "Caf\u00e9 \u2615.jpg", Smile: U+1F600, When: a date, Who: UID 7, Negative: -5,
 * Real: 1.5, List: [1, a date, "x", UID 1], Same: "x"}, in that order, "x" shared. */
static const guint8 PY_MIXED[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x11, 0x54, 0x4e, 0x61, 0x6d, 0x65, 0x55, 0x53,
    0x6d, 0x69, 0x6c, 0x65, 0x54, 0x57, 0x68, 0x65, 0x6e, 0x53, 0x57, 0x68, 0x6f, 0x58, 0x4e, 0x65,
    0x67, 0x61, 0x74, 0x69, 0x76, 0x65, 0x54, 0x52, 0x65, 0x61, 0x6c, 0x54, 0x4c, 0x69, 0x73, 0x74,
    0x54, 0x53, 0x61, 0x6d, 0x65, 0x6a, 0x00, 0x43, 0x00, 0x61, 0x00, 0x66, 0x00, 0xe9, 0x00, 0x20,
    0x26, 0x15, 0x00, 0x2e, 0x00, 0x6a, 0x00, 0x70, 0x00, 0x67, 0x62, 0xd8, 0x3d, 0xde, 0x00, 0x33,
    0x41, 0xc5, 0xa1, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x80, 0x07, 0x13, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xfb, 0x23, 0x3f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x10, 0x0b, 0x11,
    0x12, 0x10, 0x01, 0x51, 0x78, 0x80, 0x01, 0x08, 0x19, 0x1e, 0x24, 0x29, 0x2d, 0x36, 0x3b, 0x40,
    0x45, 0x5a, 0x5f, 0x68, 0x6a, 0x73, 0x7c, 0x81, 0x83, 0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87,
};

/* plistlib: {Big: 2^63}, in 16 bytes. */
static const guint8 PY_BIG[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02, 0x53, 0x42, 0x69, 0x67, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x0b, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x20,
};

/* plistlib: [2^63 - 1, -2^63, 0xff, 0x100, 0xffff, 0x10000, 0xffffffff, 0x100000000]. */
static const guint8 PY_MAXU[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xa8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x13, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x13, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0xff, 0x11, 0x01, 0x00, 0x11, 0xff, 0xff, 0x12, 0x00, 0x01, 0x00, 0x00,
    0x12, 0xff, 0xff, 0xff, 0xff, 0x13, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x11,
    0x1a, 0x23, 0x25, 0x28, 0x2b, 0x30, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e,
};

/* ------------------------------------------------------------ helpers */

static gboolean collect(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    g_byte_array_append(user, buf, len);
    return TRUE;
}

static gboolean disk_full(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "No space left");
    return FALSE;
}

/* Bytes zlib can't shrink, as a photo's are. */
static guint8 *noise(gsize n)
{
    GRand *rand = g_rand_new_with_seed(64);
    guint8 *out = g_malloc(n);
    for (gsize i = 0; i < n; i++) out[i] = g_rand_int(rand) >> 24;
    g_rand_free(rand);
    return out;
}

/* Lines of text, which shrink well. */
static guint8 *text(gsize n)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; s->len < n; i++) g_string_append_printf(s, "AirDrop vector line %06u\n", i);
    g_string_truncate(s, n);
    return (guint8 *)g_string_free(s, FALSE);
}

/* ------------------------------------------------------------ names */

static void test_safe_name(void)
{
    static const char *const good[] = {
        "photo.jpg", CAFE_JPG, ".hidden", "a b", "...", "a..b", "IMG_0049.HEIC", SMILE ".txt",
        "\xe2\x80\x8f\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.jpg",   /* a right-to-left mark, then Hebrew */
    };
    static const char *const bad[] = {
        "", ".", "..", "a/b", "/", "./a", "a/", "a\\b", "a\nb", "a\tb", "a\rb", "\x7f", "a\x01",
        "a\xc2\x85",                       /* U+0085, a C1 control */
        "photo\xe2\x80\xaegpj.exe",        /* U+202E, right-to-left override */
        "\xe2\x80\xaa" "a",                /* U+202A, left-to-right embedding */
        "a\xe2\x80\xac" "b",               /* U+202C, pop directional formatting */
        "a\xe2\x81\xa6" "b",               /* U+2066, left-to-right isolate */
        "a\xe2\x81\xa9",                   /* U+2069, pop directional isolate */
        "\xff", "a\xc3", "\xc0\xae",       /* not UTF-8 */
    };
    for (guint i = 0; i < G_N_ELEMENTS(good); i++) g_assert_true(airdrop_safe_name(good[i]));
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) g_assert_false(airdrop_safe_name(bad[i]));
    g_assert_false(airdrop_safe_name(NULL));
    char *name = g_strnfill(255, 'a');
    g_assert_true(airdrop_safe_name(name));
    g_free(name);
    name = g_strnfill(256, 'a');
    g_assert_false(airdrop_safe_name(name));
    g_free(name);
    g_assert_cmpstr(g_quark_to_string(AIRDROP_ERROR), ==, "airdrop-error-quark");
}

/* ------------------------------------------------------------ plists */

/* A plist built object by object, for inputs the writer would never make. */
typedef struct {
    GByteArray *out;
    GArray *offsets;
} Plist;

static Plist *plist_new(void)
{
    Plist *p = g_new0(Plist, 1);
    p->out = g_byte_array_new();
    p->offsets = g_array_new(FALSE, FALSE, sizeof(guint64));
    g_byte_array_append(p->out, (const guint8 *)"bplist00", 8);
    return p;
}

static void plist_obj(Plist *p, const void *bytes, gsize len)
{
    guint64 at = p->out->len;
    g_array_append_val(p->offsets, at);
    g_byte_array_append(p->out, bytes, len);
}

static void put_be(GByteArray *out, guint64 v, guint n)
{
    for (guint i = 0; i < n; i++) {
        guint8 b = v >> (8 * (n - 1 - i));
        g_byte_array_append(out, &b, 1);
    }
}

static GBytes *plist_end(Plist *p, guint offset_size, guint ref_size, guint64 top)
{
    guint64 table = p->out->len;
    for (guint i = 0; i < p->offsets->len; i++) put_be(p->out, g_array_index(p->offsets, guint64, i), offset_size);
    static const guint8 zero[6] = { 0 };
    g_byte_array_append(p->out, zero, 6);
    guint8 sizes[2] = { offset_size, ref_size };
    g_byte_array_append(p->out, sizes, 2);
    put_be(p->out, p->offsets->len, 8);
    put_be(p->out, top, 8);
    put_be(p->out, table, 8);
    g_array_unref(p->offsets);
    GBytes *b = g_byte_array_free_to_bytes(p->out);
    g_free(p);
    return b;
}

static GVariant *decode(GBytes *b, GError **error)
{
    gsize len;
    const guint8 *data = g_bytes_get_data(b, &len);
    return bplist_decode(data, len, error);
}

/* Fails to decode with `code`, and frees the plist. */
static void assert_refused(GBytes *b, AirdropError code)
{
    GError *error = NULL;
    GVariant *v = decode(b, &error);
    g_assert_null(v);
    assert_failed(error, code);
    g_bytes_unref(b);
}

/* Arrays nested `depth` deep, each holding the next, built by hand. */
static GBytes *nested(guint depth)
{
    Plist *p = plist_new();
    for (guint i = 0; i < depth; i++) {
        guint8 obj[3] = { 0xa1, (i + 1) >> 8, i + 1 };
        plist_obj(p, i + 1 < depth ? obj : (const guint8 *)"\xa0", i + 1 < depth ? 3 : 1);
    }
    return plist_end(p, 2, 2, 0);
}

static GVariant *ask_like(void)
{
    guint8 icon[300];
    for (guint i = 0; i < sizeof icon; i++) icon[i] = i * 7;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    /* Keys out of order on purpose: the writer keeps them as given. */
    g_variant_builder_add(&b, "{sv}", "TransferID", g_variant_new_parsed("{'id': <'A3F34E01-1234-5678-ABCD-EF0123456789'>}"));
    g_variant_builder_add(&b, "{sv}", "TransferType", g_variant_new_parsed("{'files': <@a{sv} {}>}"));
    g_variant_builder_add(&b, "{sv}", "SenderComputerName", g_variant_new_string("Caf\xc3\xa9 \xe2\x98\x95 Mac"));
    g_variant_builder_add(&b, "{sv}", "SenderModelName", g_variant_new_string("MacBook Pro"));
    g_variant_builder_add(&b, "{sv}", "SenderID", g_variant_new_string("486ac2a94ac6"));
    g_variant_builder_add(&b, "{sv}", "BundleID", g_variant_new_string("com.apple.finder"));
    g_variant_builder_add(&b, "{sv}", "ConvertMediaFormats", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&b, "{sv}", "SenderIdentityAuthTag", g_variant_new_parsed("[byte 0x0c, 0x50, 0x4b]"));
    g_variant_builder_add(&b, "{sv}", "FileIcon", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, icon, sizeof icon, 1));
    g_variant_builder_add(&b, "{sv}", "Files", g_variant_new_parsed(
        "[<{'FileName': <%s>, 'FileType': <'public.jpeg'>, 'FileSize': <%x>, 'FileBomPath': <%s>,"
        "   'FileIsDirectory': <false>, 'ShouldConvertMediaFormats': <false>}>,"
        " <{'FileName': <%s>, 'FileType': <'public.plain-text'>, 'FileSize': <int64 0>, 'FileBomPath': <%s>,"
        "   'FileIsDirectory': <false>, 'ShouldConvertMediaFormats': <false>}>]",
        CAFE_JPG, (gint64)141853, "./" CAFE_JPG, SMILE ".txt", "./" SMILE ".txt"));
    g_variant_builder_add(&b, "{sv}", "Items", g_variant_new_parsed("@av []"));
    g_variant_builder_add(&b, "{sv}", "Numbers", g_variant_new_parsed(
        "[<int64 0>, <int64 255>, <int64 256>, <int64 65535>, <int64 65536>, <int64 4294967295>,"
        " <int64 4294967296>, <int64 -1>, <int64 -5>, <%x>, <%x>]", G_MAXINT64, G_MININT64));
    g_variant_builder_add(&b, "{sv}", "Real", g_variant_new_double(1.5));
    g_variant_builder_add(&b, "{sv}", "Yes", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&b, "{sv}", "A string past fifteen characters", g_variant_new_string("so its count follows its marker"));
    return g_variant_ref_sink(g_variant_builder_end(&b));
}

static void test_plist_round_trip(void)
{
    GVariant *ask = ask_like();
    GError *error = NULL;
    GBytes *bytes = bplist_encode(ask, &error);
    g_assert_no_error(error);
    GVariant *back = decode(bytes, &error);
    g_assert_no_error(error);
    g_assert_false(g_variant_is_floating(back));
    g_assert_true(g_variant_equal(ask, back));

    /* The keys come back in the order they went in. */
    GVariantIter a, b;
    const char *ka, *kb;
    g_variant_iter_init(&a, ask);
    g_variant_iter_init(&b, back);
    while (g_variant_iter_next(&a, "{&s*}", &ka, NULL)) {
        g_assert_true(g_variant_iter_next(&b, "{&s*}", &kb, NULL));
        g_assert_cmpstr(ka, ==, kb);
    }

    /* Every shorter piece of it is refused. */
    gsize len;
    const guint8 *data = g_bytes_get_data(bytes, &len);
    for (gsize n = 0; n < len; n++) {
        GVariant *v = bplist_decode(data, n, &error);
        g_assert_null(v);
        g_assert_nonnull(error);
        g_clear_error(&error);
    }
    g_variant_unref(back);
    g_bytes_unref(bytes);
    g_variant_unref(ask);
}

/* The writer's bytes, exactly as Core Foundation and plistlib lay them out. */
static void test_plist_exact(void)
{
    static const guint8 dict[] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xd1, 0x01, 0x02, 0x51, 0x61, 0x09,
        0x08, 0x0b, 0x0d,                                       /* the offsets */
        0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0e,
    };
    static const guint8 utf16[] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0', 0x6a, 0x00, 0x43, 0x00, 0x61, 0x00, 0x66, 0x00, 0xe9,
        0x00, 0x20, 0x26, 0x15, 0x00, 0x2e, 0x00, 0x6a, 0x00, 0x70, 0x00, 0x67,
        0x08,
        0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1d,
    };
    GError *error = NULL;
    /* A floating value is taken over, so this leaks nothing. */
    GBytes *b = bplist_encode(g_variant_new_parsed("{'a': <true>}"), &error);
    g_assert_no_error(error);
    g_assert_cmpmem(g_bytes_get_data(b, NULL), g_bytes_get_size(b), dict, sizeof dict);
    g_bytes_unref(b);
    b = bplist_encode(g_variant_new_string(CAFE_JPG), &error);
    g_assert_no_error(error);
    g_assert_cmpmem(g_bytes_get_data(b, NULL), g_bytes_get_size(b), utf16, sizeof utf16);
    g_bytes_unref(b);

    /* References wide enough for the object count itself: 256 objects take
     * 2 bytes each, as Core Foundation requires. */
    for (guint n = 254; n <= 256; n++) {
        GVariantBuilder ab;
        g_variant_builder_init(&ab, G_VARIANT_TYPE("av"));
        for (guint i = 0; i < n; i++) g_variant_builder_add(&ab, "v", g_variant_new_int64(i));
        GVariant *array = g_variant_ref_sink(g_variant_builder_end(&ab));
        b = bplist_encode(array, &error);
        g_assert_no_error(error);
        gsize len;
        const guint8 *data = g_bytes_get_data(b, &len);
        g_assert_cmpuint(data[len - 32 + 7], ==, n + 1 < 256 ? 1 : 2);
        GVariant *back = decode(b, &error);
        g_assert_no_error(error);
        g_assert_true(g_variant_equal(array, back));
        g_variant_unref(back);
        g_variant_unref(array);
        g_bytes_unref(b);
    }
}

static void test_plist_vectors(void)
{
    GError *error = NULL;
    GVariant *ask = bplist_decode(ASK_HELLO, sizeof ASK_HELLO, &error);
    g_assert_no_error(error);
    char *id = NULL, *name = NULL, *bom = NULL;
    gint64 size = 0;
    gboolean convert = TRUE, dir = TRUE;
    GVariant *files = NULL, *items = NULL, *transfer = NULL;
    g_assert_true(g_variant_lookup(ask, "TransferID", "@a{sv}", &transfer));
    g_assert_true(g_variant_lookup(transfer, "id", "s", &id));
    g_assert_cmpstr(id, ==, "A3F34E01-0000-4000-8000-000000000001");
    g_assert_true(g_variant_lookup(ask, "ConvertMediaFormats", "b", &convert));
    g_assert_false(convert);
    g_assert_true(g_variant_lookup(ask, "Items", "@av", &items));
    g_assert_cmpuint(g_variant_n_children(items), ==, 0);
    g_assert_true(g_variant_lookup(ask, "Files", "@av", &files));
    g_assert_cmpuint(g_variant_n_children(files), ==, 1);
    GVariant *child = g_variant_get_child_value(files, 0);
    GVariant *file = g_variant_get_variant(child);
    g_variant_unref(child);
    g_assert_true(g_variant_lookup(file, "FileName", "s", &name));
    g_assert_true(g_variant_lookup(file, "FileBomPath", "s", &bom));
    g_assert_true(g_variant_lookup(file, "FileSize", "x", &size));
    /* FileIsDirectory shares one false object with two other keys. */
    g_assert_true(g_variant_lookup(file, "FileIsDirectory", "b", &dir));
    g_assert_cmpstr(name, ==, "hello.txt");
    g_assert_cmpstr(bom, ==, "./hello.txt");
    g_assert_cmpint(size, ==, 12);
    g_assert_false(dir);
    g_free(id), g_free(name), g_free(bom);
    g_variant_unref(file), g_variant_unref(files), g_variant_unref(items), g_variant_unref(transfer);
    g_variant_unref(ask);

    /* Dates and UIDs drop out of dicts and arrays; a string shared by two
     * values stays the same string. */
    GVariant *mixed = bplist_decode(PY_MIXED, sizeof PY_MIXED, &error);
    g_assert_no_error(error);
    GVariant *expect = g_variant_ref_sink(g_variant_new_parsed(
        "{'Name': <%s>, 'Smile': <%s>, 'Negative': <int64 -5>, 'Real': <1.5>,"
        " 'List': <[<int64 1>, <'x'>]>, 'Same': <'x'>}", CAFE_JPG, SMILE));
    g_assert_true(g_variant_equal(mixed, expect));
    g_variant_unref(expect);
    g_variant_unref(mixed);

    GVariant *ints = bplist_decode(PY_MAXU, sizeof PY_MAXU, &error);
    g_assert_no_error(error);
    expect = g_variant_ref_sink(g_variant_new_parsed(
        "[<%x>, <%x>, <int64 255>, <int64 256>, <int64 65535>, <int64 65536>, <int64 4294967295>,"
        " <int64 4294967296>]", G_MAXINT64, G_MININT64));
    g_assert_true(g_variant_equal(ints, expect));
    g_variant_unref(expect);
    g_variant_unref(ints);

    /* 2^63, which Core Foundation and plistlib write in 16 bytes, doesn't fit. */
    g_assert_null(bplist_decode(PY_BIG, sizeof PY_BIG, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);

    /* A small value in 16 bytes does, and so does a 4-byte real. */
    Plist *p = plist_new();
    plist_obj(p, "\xa2\x01\x02", 3);
    plist_obj(p, "\x14\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x2a", 17);
    plist_obj(p, "\x22\x3f\xc0\x00\x00", 5);
    GBytes *b = plist_end(p, 1, 1, 0);
    GVariant *v = decode(b, &error);
    g_assert_no_error(error);
    expect = g_variant_ref_sink(g_variant_new_parsed("[<int64 42>, <1.5>]"));
    g_assert_true(g_variant_equal(v, expect));
    g_variant_unref(expect), g_variant_unref(v), g_bytes_unref(b);
}

static void test_plist_malformed(void)
{
    GError *error = NULL;
    /* SEC-6: bplist00 only. */
    static const char xml[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>";
    g_assert_null(bplist_decode((const guint8 *)xml, strlen(xml), &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    GBytes *good = bplist_encode(g_variant_new_parsed("{'a': <true>}"), NULL);
    gsize len;
    const guint8 *g = g_bytes_get_data(good, &len);
    guint8 *bad = g_memdup2(g, len);
    bad[7] = '1';
    g_assert_null(bplist_decode(bad, len, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);

    /* Each trailer field out of range. */
    static const struct { guint at; guint8 value; } trailer[] = {
        { 6, 0 }, { 6, 9 }, { 7, 0 }, { 7, 9 },       /* offset and reference sizes */
        { 15, 0 }, { 15, 0xff }, { 8, 0x80 },          /* object count: none, more than fit, 2^63 */
        { 23, 3 },                                     /* the top object past the count */
        { 31, 8 }, { 31, 7 }, { 31, 0x20 }, { 24, 1 }, /* the table's offset */
    };
    for (guint i = 0; i < G_N_ELEMENTS(trailer); i++) {
        memcpy(bad, g, len);
        bad[len - 32 + trailer[i].at] = trailer[i].value;
        g_assert_null(bplist_decode(bad, len, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    /* An offset into the table, or before the objects. */
    for (guint8 o = 0; o < 2; o++) {
        memcpy(bad, g, len);
        bad[14] = o ? 0x0e : 0x07;
        g_assert_null(bplist_decode(bad, len, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    /* A reference past the object count. */
    memcpy(bad, g, len);
    bad[10] = 3;
    g_assert_null(bplist_decode(bad, len, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_free(bad);
    g_bytes_unref(good);

    /* Too short, and too large (SEC-3). */
    g_assert_null(bplist_decode((const guint8 *)"bplist00\x08", 9, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    guint8 *huge = g_malloc0(BPLIST_MAX_BYTES + 1);
    memcpy(huge, "bplist00", 8);
    g_assert_null(bplist_decode(huge, BPLIST_MAX_BYTES + 1, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_free(huge);

    /* Objects the reader doesn't take, or that are broken. */
    static const struct { const char *bytes; guint len; } objects[] = {
        { "\x00", 1 }, { "\x0f", 1 }, { "\x0c", 1 },     /* null, fill, URL */
        { "\x15", 1 },                                   /* a 32-byte integer */
        { "\x24\0\0\0\0\0\0\0\0", 9 },                   /* a 16-byte real */
        { "\x34\0\0\0\0\0\0\0\0", 9 },                   /* a date of the wrong size */
        { "\x70", 1 }, { "\xc0", 1 }, { "\xb0", 1 },     /* unused, set, ordered set */
        { "\x52\x61\x00", 3 },                           /* a NUL in a string */
        { "\x51\xe9", 2 },                               /* a byte past ASCII in an ASCII string */
        { "\x61\xd8\x3d", 3 },                           /* a lone high surrogate */
        { "\x61\xde\x00", 3 },                           /* a lone low surrogate */
        { "\x62\xd8\x3d\x00\x41", 5 },                   /* a high surrogate before a letter */
        { "\x5f\x50", 2 },                               /* a count that isn't an integer */
        { "\x5f\x14\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x01", 18 }, /* a 16-byte count */
        { "\x4f\x13\x7f\xff\xff\xff\xff\xff\xff\xff", 10 },   /* a count past the input */
        { "\x43\x01\x02", 3 },                           /* data running past the objects */
        { "\x13\x00\x00", 3 },                           /* an integer running past them */
        { "\x33\0\0\0\0\0\0\0\0", 9 },                   /* the top object a date */
        { "\x80\x07", 2 },                               /* or a UID */
    };
    for (guint i = 0; i < G_N_ELEMENTS(objects); i++) {
        Plist *p = plist_new();
        plist_obj(p, objects[i].bytes, objects[i].len);
        assert_refused(plist_end(p, 1, 1, 0), AIRDROP_ERROR_INVALID);
    }

    /* A key that isn't a string, and a key given twice. */
    Plist *p = plist_new();
    plist_obj(p, "\xd1\x01\x02", 3);
    plist_obj(p, "\x10\x05", 2);
    plist_obj(p, "\x09", 1);
    assert_refused(plist_end(p, 1, 1, 0), AIRDROP_ERROR_INVALID);
    p = plist_new();
    plist_obj(p, "\xd2\x01\x01\x02\x03", 5);
    plist_obj(p, "\x51\x61", 2);
    plist_obj(p, "\x09", 1);
    plist_obj(p, "\x33\0\0\0\0\0\0\0\0", 9);   /* even when one value is a date */
    assert_refused(plist_end(p, 1, 1, 0), AIRDROP_ERROR_INVALID);

    /* Reference cycles: an array holding itself, and two holding each other. */
    p = plist_new();
    plist_obj(p, "\xa1\x00", 2);
    assert_refused(plist_end(p, 1, 1, 0), AIRDROP_ERROR_INVALID);
    p = plist_new();
    plist_obj(p, "\xa1\x01", 2);
    plist_obj(p, "\xd1\x02\x00", 3);
    plist_obj(p, "\x51\x6b", 2);
    assert_refused(plist_end(p, 1, 1, 0), AIRDROP_ERROR_INVALID);
}

static void test_plist_limits(void)
{
    GError *error = NULL;
    /* SEC-6: 64 levels of nesting, no more. */
    GBytes *b = nested(64);
    GVariant *v = decode(b, &error);
    g_assert_no_error(error);
    g_variant_unref(v);
    g_bytes_unref(b);
    assert_refused(nested(65), AIRDROP_ERROR_LIMIT);
    assert_refused(nested(200), AIRDROP_ERROR_LIMIT);

    /* SEC-3: each array holds the next twice, 2^40 values from 41 objects. */
    Plist *p = plist_new();
    for (guint i = 0; i < 41; i++) {
        guint8 obj[3] = { 0xa2, i + 1, i + 1 };
        plist_obj(p, i < 40 ? obj : (const guint8 *)"\x08", i < 40 ? 3 : 1);
    }
    assert_refused(plist_end(p, 1, 1, 0), AIRDROP_ERROR_LIMIT);

    /* SEC-3: one 64 KiB data object referenced 1000 times, 64 MB from 66 KB. */
    p = plist_new();
    GByteArray *array = g_byte_array_new();
    g_byte_array_append(array, (const guint8 *)"\xaf\x11\x03\xe8", 4);
    for (guint i = 0; i < 1000; i++) g_byte_array_append(array, (const guint8 *)"\x00\x01", 2);
    plist_obj(p, array->data, array->len);
    g_byte_array_unref(array);
    guint8 *data = g_malloc0(6 + 65536);
    memcpy(data, "\x4f\x12\x00\x01\x00\x00", 6);
    plist_obj(p, data, 6 + 65536);
    g_free(data);
    assert_refused(plist_end(p, 4, 2, 0), AIRDROP_ERROR_LIMIT);

    /* The object budget counts every object: 65536 decode, 65537 don't. */
    for (guint n = 65535; n <= 65536; n++) {
        GVariantBuilder ab;
        g_variant_builder_init(&ab, G_VARIANT_TYPE("av"));
        for (guint i = 0; i < n; i++) g_variant_builder_add(&ab, "v", g_variant_new_boolean(TRUE));
        b = bplist_encode(g_variant_builder_end(&ab), &error);
        g_assert_no_error(error);
        if (n + 1 <= BPLIST_MAX_OBJECTS) {
            v = decode(b, &error);
            g_assert_no_error(error);
            g_assert_cmpuint(g_variant_n_children(v), ==, n);
            g_variant_unref(v);
            g_bytes_unref(b);
        } else {
            assert_refused(b, AIRDROP_ERROR_LIMIT);
        }
    }

    /* The writer: as deep and as large as the reader takes, and no more. */
    for (guint depth = 64; depth <= 65; depth++) {
        GVariant *deep = g_variant_new_parsed("@av []");
        for (guint i = 1; i < depth; i++) {
            GVariant *child = g_variant_new_variant(deep);
            deep = g_variant_new_array(G_VARIANT_TYPE_VARIANT, &child, 1);
        }
        b = bplist_encode(deep, &error);
        if (depth == 64) {
            g_assert_no_error(error);
            v = decode(b, &error);
            g_assert_no_error(error);
            g_variant_unref(v);
            g_bytes_unref(b);
        } else {
            g_assert_null(b);
            assert_failed(error, AIRDROP_ERROR_LIMIT);
        }
    }
    data = g_malloc0(BPLIST_MAX_BYTES);
    b = bplist_encode(g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, BPLIST_MAX_BYTES - 64, 1), &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_bytes_get_size(b), <=, BPLIST_MAX_BYTES);
    v = decode(b, &error);
    g_assert_no_error(error);
    g_variant_unref(v);
    g_bytes_unref(b);
    b = bplist_encode(g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, BPLIST_MAX_BYTES, 1), &error);
    g_assert_null(b);
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_free(data);

    /* Types a plist can't hold. */
    GVariant *odd[] = {
        g_variant_new("(ii)", 1, 2), g_variant_new_parsed("{1: <true>}"),
        g_variant_new_maybe(G_VARIANT_TYPE_STRING, NULL), g_variant_new_object_path("/a"),
    };
    for (guint i = 0; i < G_N_ELEMENTS(odd); i++) {
        g_assert_null(bplist_encode(odd[i], &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
}

/* ------------------------------------------------------------ cpio */

static void odc_entry(GByteArray *out, const char *name, guint mode, guint nlink, const char *data, gsize size)
{
    char *head = g_strdup_printf("070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o", 0u, 1u, mode, 501u, 20u, nlink,
                                 0u, (guint)MTIME, (guint)strlen(name) + 1, (guint)size);
    g_byte_array_append(out, (const guint8 *)head, strlen(head));
    g_byte_array_append(out, (const guint8 *)name, strlen(name) + 1);
    if (size) g_byte_array_append(out, (const guint8 *)data, size);
    g_free(head);
}

static void newc_entry(GByteArray *out, const char *name, guint mode, guint nlink, const char *data, gsize size)
{
    static const guint8 zero[4] = { 0 };
    gsize namesize = strlen(name) + 1;
    char *head = g_strdup_printf("070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x", 1u, mode, 501u, 20u,
                                 nlink, (guint)MTIME, (guint)size, 0u, 0u, 0u, 0u, (guint)namesize, 0u);
    g_byte_array_append(out, (const guint8 *)head, 110);
    g_byte_array_append(out, (const guint8 *)name, namesize);
    g_byte_array_append(out, zero, (4 - (110 + namesize) % 4) % 4);
    if (size) g_byte_array_append(out, (const guint8 *)data, size);
    g_byte_array_append(out, zero, (4 - size % 4) % 4);
    g_free(head);
}

/* What the reader saw: a line per entry, its data, and "|end" after it. */
typedef struct {
    GString *log;
    const char *refuse;   /* a path to refuse at its header */
    guint64 left;         /* data the current entry still has */
} Cpio;

static gboolean on_begin(const char *path, CpioType type, guint64 size, gpointer user, GError **error)
{
    Cpio *c = user;
    g_assert_cmpuint(c->left, ==, 0);
    g_string_append_printf(c->log, "%s %s %" G_GUINT64_FORMAT "\n", type == CPIO_DIR ? "dir" : "file", path, size);
    c->left = size;
    if (c->refuse && strcmp(path, c->refuse) == 0) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, "Not asked for");
        return FALSE;
    }
    return TRUE;
}

static gboolean on_data(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Cpio *c = user;
    /* Nothing past an entry's size reaches it. */
    g_assert_cmpuint(len, >, 0);
    g_assert_cmpuint(len, <=, c->left);
    c->left -= len;
    g_string_append_len(c->log, (const char *)buf, len);
    return TRUE;
}

static gboolean on_end(gpointer user, GError **error)
{
    Cpio *c = user;
    g_assert_cmpuint(c->left, ==, 0);
    g_string_append(c->log, "|end\n");
    return TRUE;
}

static const CpioCallbacks CPIO_LOG = { on_begin, on_data, on_end };

/* Reads an archive in pieces of `step` bytes (0: at once) and finishes it.
 * The log, or NULL with the error. */
static char *read_cpio(const guint8 *data, gsize len, gsize step, const char *refuse, GError **error)
{
    Cpio c = { g_string_new(NULL), refuse, 0 };
    CpioReader *r = cpio_reader_new(&CPIO_LOG, &c);
    gboolean ok = TRUE, done = FALSE;
    for (gsize pos = 0; ok && pos < len; pos += step ? step : len)
        ok = cpio_reader_feed(r, data + pos, MIN(step ? step : len, len - pos), &done, error);
    ok = ok && cpio_reader_finish(r, error);
    g_assert_true(!ok || done);
    cpio_reader_free(r);
    return g_string_free(c.log, !ok);
}

static void test_cpio_vectors(void)
{
    static const char *const want = "dir . 0\n|end\nfile ./hello.txt 12\nhello world\n|end\n";
    static const gsize steps[] = { 0, 1, 7, 76, 110 };
    for (guint i = 0; i < G_N_ELEMENTS(steps); i++) {
        GError *error = NULL;
        char *log = read_cpio(CPIO_ODC, sizeof CPIO_ODC, steps[i], NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpstr(log, ==, want);
        g_free(log);
        log = read_cpio(CPIO_NEWC, sizeof CPIO_NEWC, steps[i], NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpstr(log, ==, want);
        g_free(log);
    }

    /* Two files back to back, an empty one between: each gets its own
     * bytes, a byte at a time or all at once. */
    for (int newc = 0; newc < 2; newc++) {
        GByteArray *a = g_byte_array_new();
        void (*entry)(GByteArray *, const char *, guint, guint, const char *, gsize) = newc ? newc_entry : odc_entry;
        entry(a, "./a", 0100644, 1, "hello", 5);
        entry(a, "./empty", 0100644, 1, NULL, 0);
        entry(a, "./b", 0100644, 1, "xyz", 3);
        entry(a, "TRAILER!!!", 0, 0, NULL, 0);
        for (gsize step = 0; step <= 1; step++) {
            GError *error = NULL;
            char *log = read_cpio(a->data, a->len, step, NULL, &error);
            g_assert_no_error(error);
            g_assert_cmpstr(log, ==, "file ./a 5\nhello|end\nfile ./empty 0\n|end\nfile ./b 3\nxyz|end\n");
            g_free(log);
        }
        g_byte_array_unref(a);
    }
}

static void test_cpio_after_trailer(void)
{
    /* UP-11: done at TRAILER!!!; whatever follows is ignored. */
    GByteArray *a = g_byte_array_new();
    g_byte_array_append(a, CPIO_ODC, sizeof CPIO_ODC);
    g_byte_array_append(a, (const guint8 *)"not an entry at all", 19);
    Cpio c = { g_string_new(NULL), NULL, 0 };
    CpioReader *r = cpio_reader_new(&CPIO_LOG, &c);
    GError *error = NULL;
    gboolean done = TRUE;
    g_assert_true(cpio_reader_feed(r, a->data, sizeof CPIO_ODC - 1, &done, &error));
    g_assert_false(done);
    g_assert_true(cpio_reader_feed(r, a->data + sizeof CPIO_ODC - 1, a->len - sizeof CPIO_ODC + 1, &done, &error));
    g_assert_no_error(error);
    g_assert_true(done);
    g_assert_true(cpio_reader_feed(r, (const guint8 *)"more", 4, &done, &error));
    g_assert_true(done);
    g_assert_true(cpio_reader_finish(r, &error));
    cpio_reader_free(r);
    g_string_free(c.log, TRUE);
    g_byte_array_unref(a);
}

static void test_cpio_refused(void)
{
    static const struct {
        const char *name;
        guint mode, nlink;
        gsize size;
        AirdropError code;
    } cases[] = {
        { "/etc/passwd", 0100644, 1, 1, AIRDROP_ERROR_REFUSED },
        { "../x", 0100644, 1, 1, AIRDROP_ERROR_REFUSED },
        { "./../x", 0100644, 1, 1, AIRDROP_ERROR_REFUSED },
        { "./a/../../x", 0100644, 1, 1, AIRDROP_ERROR_REFUSED },
        { "./a/..", 040755, 2, 0, AIRDROP_ERROR_REFUSED },
        { "..", 040755, 2, 0, AIRDROP_ERROR_REFUSED },
        { "./link", 0120777, 1, 1, AIRDROP_ERROR_REFUSED },     /* a symlink */
        { "./hard", 0100644, 2, 1, AIRDROP_ERROR_REFUSED },     /* a hard-linked file */
        { "./tty", 020644, 1, 0, AIRDROP_ERROR_REFUSED },       /* a character device */
        { "./disk", 060644, 1, 0, AIRDROP_ERROR_REFUSED },      /* a block device */
        { "./fifo", 010644, 1, 0, AIRDROP_ERROR_REFUSED },
        { "./socket", 0140755, 1, 0, AIRDROP_ERROR_REFUSED },
        { "./dir", 040755, 2, 1, AIRDROP_ERROR_INVALID },       /* a directory with data */
        { "", 0100644, 1, 1, AIRDROP_ERROR_INVALID },           /* no name */
        { "./\xff", 0100644, 1, 1, AIRDROP_ERROR_INVALID },     /* not UTF-8 */
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++)
        for (int newc = 0; newc < 2; newc++) {
            GByteArray *a = g_byte_array_new();
            void (*entry)(GByteArray *, const char *, guint, guint, const char *, gsize) = newc ? newc_entry : odc_entry;
            entry(a, ".", 040755, 2, NULL, 0);
            entry(a, cases[i].name, cases[i].mode, cases[i].nlink, "x", cases[i].size);
            entry(a, "TRAILER!!!", 0, 0, NULL, 0);
            GError *error = NULL;
            char *log = read_cpio(a->data, a->len, 0, NULL, &error);
            g_assert_null(log);
            assert_failed(error, cases[i].code);
            g_byte_array_unref(a);
        }

    /* The caller refuses an entry at its header. */
    GError *error = NULL;
    g_assert_null(read_cpio(CPIO_ODC, sizeof CPIO_ODC, 1, "./hello.txt", &error));
    g_assert_error(error, AIRDROP_ERROR, AIRDROP_ERROR_REFUSED);
    g_assert_cmpstr(error->message, ==, "Not asked for");
    g_clear_error(&error);
}

static void test_cpio_malformed(void)
{
    GError *error = NULL;
    guint8 *a = g_memdup2(CPIO_ODC, sizeof CPIO_ODC);
    static const struct { guint at; guint8 c; } edits[] = {
        { 0, '1' },          /* not a magic */
        { 5, '2' },          /* 070702, newc with checksums */
        { 20, '9' },         /* not an octal digit */
        { 59 + 5, '0' },     /* namesize 0 */
        { 76 + 1, 'x' },     /* "." without its NUL */
    };
    for (guint i = 0; i < G_N_ELEMENTS(edits); i++) {
        memcpy(a, CPIO_ODC, sizeof CPIO_ODC);
        a[edits[i].at] = edits[i].c;
        g_assert_null(read_cpio(a, sizeof CPIO_ODC, 0, NULL, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    g_free(a);

    /* A name with a NUL inside. */
    GByteArray *b = g_byte_array_new();
    odc_entry(b, "./ab", 0100644, 1, "x", 1);
    b->data[76 + 2] = 0;
    odc_entry(b, "TRAILER!!!", 0, 0, NULL, 0);
    g_assert_null(read_cpio(b->data, b->len, 0, NULL, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_byte_array_set_size(b, 0);

    /* The format changing midway. */
    odc_entry(b, "./a", 0100644, 1, "x", 1);
    newc_entry(b, "./b", 0100644, 1, "y", 1);
    odc_entry(b, "TRAILER!!!", 0, 0, NULL, 0);
    g_assert_null(read_cpio(b->data, b->len, 0, NULL, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_byte_array_unref(b);

    /* Cut short anywhere: the reader waits, then finishing fails. */
    for (gsize n = 0; n < sizeof CPIO_ODC; n++) {
        g_assert_null(read_cpio(CPIO_ODC, n, 1, NULL, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    for (gsize n = 0; n < sizeof CPIO_NEWC; n += 3) {
        g_assert_null(read_cpio(CPIO_NEWC, n, 0, NULL, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }

    /* A reader that failed stays failed. */
    CpioReader *r = cpio_reader_new(NULL, NULL);
    gboolean done;
    g_assert_false(cpio_reader_feed(r, (const guint8 *)"123456", 6, &done, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_false(cpio_reader_feed(r, CPIO_ODC, sizeof CPIO_ODC, &done, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_false(done);
    g_assert_false(cpio_reader_finish(r, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    cpio_reader_free(r);
}

static void test_cpio_limits(void)
{
    GError *error = NULL;
    /* Names up to CPIO_MAX_NAME bytes with their NUL, no longer. */
    for (gsize n = CPIO_MAX_NAME - 1; n <= CPIO_MAX_NAME; n++) {
        GByteArray *a = g_byte_array_new();
        char *name = g_strnfill(n, 'a');
        name[0] = '.', name[1] = '/';
        odc_entry(a, name, 0100644, 1, NULL, 0);
        odc_entry(a, "TRAILER!!!", 0, 0, NULL, 0);
        char *log = read_cpio(a->data, a->len, 0, NULL, &error);
        if (n + 1 <= CPIO_MAX_NAME) {
            g_assert_no_error(error);
            g_free(log);
        } else {
            g_assert_null(log);
            assert_failed(error, AIRDROP_ERROR_LIMIT);
        }
        g_free(name);
        g_byte_array_unref(a);
    }
    /* A name of 0777777 bytes is refused at its header, before it arrives. */
    GByteArray *a = g_byte_array_new();
    odc_entry(a, "./a", 0100644, 1, NULL, 0);
    memcpy(a->data + 59, "777777", 6);
    CpioReader *r = cpio_reader_new(NULL, NULL);
    gboolean done;
    g_assert_false(cpio_reader_feed(r, a->data, 76, &done, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    cpio_reader_free(r);
    g_byte_array_unref(a);

    /* CPIO_MAX_ENTRIES entries, the root among them, and no more. */
    for (guint n = CPIO_MAX_ENTRIES; n <= CPIO_MAX_ENTRIES + 1; n++) {
        a = g_byte_array_new();
        odc_entry(a, ".", 040755, 2, NULL, 0);
        for (guint i = 1; i < n; i++) {
            char name[16];
            g_snprintf(name, sizeof name, "./%u", i);
            odc_entry(a, name, 0100644, 1, NULL, 0);
        }
        odc_entry(a, "TRAILER!!!", 0, 0, NULL, 0);
        char *log = read_cpio(a->data, a->len, 0, NULL, &error);
        if (n <= CPIO_MAX_ENTRIES) {
            g_assert_no_error(error);
            g_free(log);
        } else {
            g_assert_null(log);
            assert_failed(error, AIRDROP_ERROR_LIMIT);
        }
        g_byte_array_unref(a);
    }
}

static void test_cpio_writer(void)
{
    GByteArray *out = g_byte_array_new();
    CpioWriter *w = cpio_writer_new(collect, out);
    GError *error = NULL;
    g_assert_true(cpio_writer_dir(w, ".", &error));
    g_assert_true(cpio_writer_file(w, "./hello.txt", 12, MTIME, &error));
    g_assert_true(cpio_writer_file_data(w, (const guint8 *)"hello ", 6, &error));
    g_assert_true(cpio_writer_file_data(w, (const guint8 *)"world\n", 6, &error));
    g_assert_true(cpio_writer_file(w, "./" CAFE_JPG, 0, -5, &error));
    g_assert_true(cpio_writer_finish(w, &error));
    g_assert_no_error(error);
    cpio_writer_free(w);

    /* The directory: dated now, so its mtime is checked apart. */
    static const char dir[] = "070707" "000000" "000001" "040755" "000765" "000024" "000002" "000000";
    g_assert_cmpmem(out->data, 48, dir, 48);
    char *digits = g_strndup((const char *)out->data + 48, 11);
    gint64 mtime = g_ascii_strtoll(digits, NULL, 8);
    g_free(digits);
    g_assert_cmpint(ABS(mtime - g_get_real_time() / G_USEC_PER_SEC), <, 60);
    static const char dir_tail[] = "000002" "00000000000" ".";
    g_assert_cmpmem(out->data + 59, 19, dir_tail, 19);

    /* Then the rest, byte for byte: POSIX odc as Finder writes it. */
    static const char rest[] =
        "070707" "000000" "000002" "100644" "000765" "000024" "000001" "000000" "14524770400" "000014" "00000000014"
        "./hello.txt\0" "hello world\n"
        "070707" "000000" "000003" "100644" "000765" "000024" "000001" "000000" "00000000000" "000020" "00000000000"
        "./" CAFE_JPG "\0"
        "070707" "000000" "000000" "000000" "000000" "000000" "000000" "000000" "00000000000" "000013" "00000000000"
        "TRAILER!!!";   /* with the literal's own NUL */
    g_assert_cmpmem(out->data + 78, out->len - 78, rest, sizeof rest);

    /* And the reader takes it back. */
    char *log = read_cpio(out->data, out->len, 1, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(log, ==, "dir . 0\n|end\nfile ./hello.txt 12\nhello world\n|end\nfile ./" CAFE_JPG " 0\n|end\n");
    g_free(log);
    g_byte_array_unref(out);
}

static void test_cpio_writer_errors(void)
{
    GByteArray *out = g_byte_array_new();
    CpioWriter *w = cpio_writer_new(collect, out);
    GError *error = NULL;
    static const char *const refused[] = { "", "/etc/passwd", "../x", "./a/../../x", "..", "TRAILER!!!" };
    for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
        g_assert_false(cpio_writer_file(w, refused[i], 1, 0, &error));
        assert_failed(error, AIRDROP_ERROR_REFUSED);
        g_assert_false(cpio_writer_dir(w, refused[i], &error));
        assert_failed(error, AIRDROP_ERROR_REFUSED);
    }
    char *name = g_strnfill(CPIO_MAX_NAME, 'a');
    g_assert_false(cpio_writer_dir(w, name, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_free(name);
    g_assert_false(cpio_writer_file(w, "./big", G_GUINT64_CONSTANT(077777777777) + 1, 0, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_assert_cmpuint(out->len, ==, 0);

    /* A file's data must be exactly its size. */
    g_assert_true(cpio_writer_file(w, "./a", 3, 0, &error));
    g_assert_false(cpio_writer_file_data(w, (const guint8 *)"abcd", 4, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_true(cpio_writer_file_data(w, (const guint8 *)"ab", 2, &error));
    g_assert_false(cpio_writer_dir(w, "./d", &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_false(cpio_writer_finish(w, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_true(cpio_writer_file_data(w, (const guint8 *)"c", 1, &error));
    g_assert_true(cpio_writer_finish(w, &error));
    g_assert_false(cpio_writer_dir(w, "./d", &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    cpio_writer_free(w);
    g_byte_array_unref(out);

    /* The sink's error comes back as it is. */
    w = cpio_writer_new(disk_full, NULL);
    g_assert_false(cpio_writer_dir(w, ".", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    g_assert_false(cpio_writer_finish(w, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    cpio_writer_free(w);
}

/* ------------------------------------------------------------ gzip and DVZip */

/* Compresses `len` bytes fed in pieces of `step`. */
static GByteArray *deflate_all(gboolean dvzip, const guint8 *data, gsize len, gsize step)
{
    GByteArray *out = g_byte_array_new();
    AirdropDeflater *d = airdrop_deflater_new(dvzip, collect, out);
    GError *error = NULL;
    for (gsize pos = 0; pos < len; pos += step)
        g_assert_true(airdrop_deflater_feed(d, data + pos, MIN(step, len - pos), &error));
    g_assert_true(airdrop_deflater_finish(d, &error));
    g_assert_no_error(error);
    airdrop_deflater_free(d);
    return out;
}

/* Decompresses a body fed in pieces of `step` (0: at once): the output, or
 * NULL with the error. */
static GByteArray *inflate_all(gboolean dvzip, guint64 max, const guint8 *body, gsize len, gsize step, GError **error)
{
    GByteArray *out = g_byte_array_new();
    AirdropInflater *z = airdrop_inflater_new(dvzip, max, collect, out);
    gboolean ok = TRUE;
    for (gsize pos = 0; ok && pos < len; pos += step ? step : len)
        ok = airdrop_inflater_feed(z, body + pos, MIN(step ? step : len, len - pos), error);
    ok = ok && airdrop_inflater_finish(z, error);
    airdrop_inflater_free(z);
    if (ok) return out;
    g_byte_array_unref(out);
    return NULL;
}

/* A DVZip body's chunks, each checked to be one 64 KiB piece at most. */
static guint dvzip_chunks(const GByteArray *body, guint *stored)
{
    guint n = 0;
    *stored = 0;
    for (gsize pos = 0; pos < body->len; n++) {
        g_assert_cmpuint(body->len - pos, >=, 4);
        guint32 raw = (guint32)body->data[pos] << 24 | body->data[pos + 1] << 16 | body->data[pos + 2] << 8
                      | body->data[pos + 3];
        guint32 len = raw & 0x7fffffff;
        g_assert_cmpuint(len, >, 0);
        g_assert_cmpuint(len, <=, 65536);
        *stored += raw >> 31;
        pos += 4 + len;
        g_assert_cmpuint(pos, <=, body->len);
    }
    return n;
}

static void test_compress_round_trip(void)
{
    guint8 *t = text(200000), *z = noise(150000);
    GByteArray *mixed = g_byte_array_new();
    g_byte_array_append(mixed, t, 65536);
    g_byte_array_append(mixed, z, 131072);
    g_byte_array_append(mixed, t, 2600);
    static const struct { const char *name; guint chunks, stored; } want[] = {
        { "text", 4, 0 }, { "noise", 3, 3 }, { "mixed", 4, 2 }, { "hello", 1, 1 }, { "empty", 0, 0 },
    };
    const guint8 *inputs[] = { t, z, mixed->data, (const guint8 *)"hello", (const guint8 *)"" };
    const gsize lens[] = { 200000, 150000, mixed->len, 5, 0 };
    for (int dvzip = 0; dvzip < 2; dvzip++)
        for (guint i = 0; i < G_N_ELEMENTS(inputs); i++) {
            GByteArray *body = deflate_all(dvzip, inputs[i], lens[i], 7777);
            if (dvzip) {
                guint stored, chunks = dvzip_chunks(body, &stored);
                g_assert_cmpuint(chunks, ==, want[i].chunks);
                g_assert_cmpuint(stored, ==, want[i].stored);
            } else {
                g_assert_cmpuint(body->len, >=, 18);
                g_assert_cmpmem(body->data, 3, "\x1f\x8b\x08", 3);
            }
            if (i == 0) g_assert_cmpuint(body->len, <, 200000 / 4);
            for (gsize step = 0; step <= (lens[i] <= 150000 ? 1 : 0); step++) {
                GError *error = NULL;
                GByteArray *back = inflate_all(dvzip, lens[i], body->data, body->len, step, &error);
                g_assert_no_error(error);
                g_assert_cmpmem(back->data, back->len, inputs[i], lens[i]);
                g_byte_array_unref(back);
            }
            g_byte_array_unref(body);
        }
    g_byte_array_unref(mixed);
    g_free(t);
    g_free(z);

    /* A stored chunk by hand, as an iPhone sends a photo's bytes. */
    GError *error = NULL;
    GByteArray *back = inflate_all(TRUE, 100, (const guint8 *)"\x80\x00\x00\x05hello\x80\x00\x00\x01!", 14, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpmem(back->data, back->len, "hello!", 6);
    g_byte_array_unref(back);
}

/* gzip members back to back decode as one stream, within one output limit. */
static void test_gzip_members(void)
{
    guint8 *t = text(100000);
    GByteArray *body = g_byte_array_new();
    const gsize cuts[] = { 0, 6, 6, 70000, 100000 };   /* an empty member among them */
    for (guint i = 0; i + 1 < G_N_ELEMENTS(cuts); i++) {
        GByteArray *member = deflate_all(FALSE, t + cuts[i], cuts[i + 1] - cuts[i], 4096);
        g_byte_array_append(body, member->data, member->len);
        g_byte_array_unref(member);
    }
    for (gsize step = 0; step <= 1; step++) {
        GError *error = NULL;
        GByteArray *back = inflate_all(FALSE, 100000, body->data, body->len, step, &error);
        g_assert_no_error(error);
        g_assert_cmpmem(back->data, back->len, t, 100000);
        g_byte_array_unref(back);
        g_assert_null(inflate_all(FALSE, 99999, body->data, body->len, step, &error));
        assert_failed(error, AIRDROP_ERROR_LIMIT);
        /* A second member cut short. */
        g_assert_null(inflate_all(FALSE, 100000, body->data, body->len - 1, step, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    g_byte_array_unref(body);
    g_free(t);
}

/* One zlib stream of `len` zero bytes. */
static GByteArray *zlib_zeros(gsize len)
{
    GConverter *c = G_CONVERTER(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 9));
    guint8 *zeros = g_malloc0(len);
    GByteArray *out = g_byte_array_new();
    guint8 buf[65536];
    gsize pos = 0, read, written;
    GConverterResult res;
    do {
        res = g_converter_convert(c, zeros + pos, len - pos, buf, sizeof buf, G_CONVERTER_INPUT_AT_END, &read, &written, NULL);
        g_assert_cmpint(res, !=, G_CONVERTER_ERROR);
        pos += read;
        g_byte_array_append(out, buf, written);
    } while (res != G_CONVERTER_FINISHED);
    g_free(zeros);
    g_object_unref(c);
    return out;
}

static void test_compress_bombs(void)
{
    /* SEC-3: 8 MiB of zeros from a few kilobytes fails cleanly past the
     * limit, and passes at it. */
    guint8 *zeros = g_malloc0(8 << 20);
    for (int dvzip = 0; dvzip < 2; dvzip++) {
        GByteArray *body = deflate_all(dvzip, zeros, 8 << 20, 1 << 20);
        g_assert_cmpuint(body->len, <, 64 << 10);
        GError *error = NULL;
        g_assert_null(inflate_all(dvzip, 1 << 20, body->data, body->len, 0, &error));
        assert_failed(error, AIRDROP_ERROR_LIMIT);
        GByteArray *back = inflate_all(dvzip, 8 << 20, body->data, body->len, 0, &error);
        g_assert_no_error(error);
        g_assert_cmpuint(back->len, ==, 8 << 20);
        g_byte_array_unref(back);
        g_byte_array_unref(body);
    }
    g_free(zeros);

    /* DVZ-3: one chunk may inflate to far more than 64 KiB. */
    GByteArray *stream = zlib_zeros(4 << 20);
    GByteArray *body = g_byte_array_new();
    put_be(body, stream->len, 4);
    g_byte_array_append(body, stream->data, stream->len);
    GError *error = NULL;
    GByteArray *back = inflate_all(TRUE, 4 << 20, body->data, body->len, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(back->len, ==, 4 << 20);
    g_byte_array_unref(back);
    g_assert_null(inflate_all(TRUE, 1 << 20, body->data, body->len, 0, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_byte_array_unref(body);
    g_byte_array_unref(stream);
}

static void test_compress_malformed(void)
{
    GError *error = NULL;
    GByteArray *stream = zlib_zeros(5);
    /* DVZ-3: no empty chunk, stored or not; none past the limit. */
    static const struct { const char *bytes; gsize len; AirdropError code; } chunks[] = {
        { "\x00\x00\x00\x00", 4, AIRDROP_ERROR_INVALID },
        { "\x80\x00\x00\x00", 4, AIRDROP_ERROR_INVALID },
        { "\x00\x40\x00\x01", 4, AIRDROP_ERROR_LIMIT },
        { "\x80\x40\x00\x01", 4, AIRDROP_ERROR_LIMIT },
        { "\x00\x00\x00\x03" "abc", 7, AIRDROP_ERROR_INVALID },                          /* not zlib */
        { "\x00\x00\x00\x07\xcb\x48\xcd\xc9\xc9\x07\x00", 11, AIRDROP_ERROR_INVALID },   /* raw deflate */
    };
    for (guint i = 0; i < G_N_ELEMENTS(chunks); i++) {
        g_assert_null(inflate_all(TRUE, 1 << 20, (const guint8 *)chunks[i].bytes, chunks[i].len, 0, &error));
        assert_failed(error, chunks[i].code);
    }
    /* A chunk with a byte after its zlib stream, and one too short for it. */
    GByteArray *b = g_byte_array_new();
    put_be(b, stream->len + 1, 4);
    g_byte_array_append(b, stream->data, stream->len);
    g_byte_array_append(b, (const guint8 *)"", 1);
    for (gsize step = 0; step <= 1; step++) {
        g_assert_null(inflate_all(TRUE, 1 << 20, b->data, b->len, step, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    g_byte_array_set_size(b, 0);
    put_be(b, stream->len - 1, 4);
    g_byte_array_append(b, stream->data, stream->len - 1);
    g_byte_array_append(b, (const guint8 *)"\x80\x00\x00\x01!", 5);
    g_assert_null(inflate_all(TRUE, 1 << 20, b->data, b->len, 0, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_byte_array_unref(b);
    g_byte_array_unref(stream);

    /* Truncated anywhere: in a length, a stored chunk or a zlib one. */
    guint8 *t = text(70000);
    GByteArray *body = deflate_all(TRUE, t, 70000, 70000);
    for (gsize n = 1; n < body->len; n += (n < 8 || body->len - n < 8) ? 1 : 97) {
        g_assert_null(inflate_all(TRUE, 1 << 20, body->data, n, 0, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    g_byte_array_unref(body);
    body = deflate_all(FALSE, t, 70000, 70000);
    for (gsize n = 0; n < body->len; n += (n < 20 || body->len - n < 20) ? 1 : 97) {
        g_assert_null(inflate_all(FALSE, 1 << 20, body->data, n, 0, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }

    /* gzip: after a member, only another member; its checksum right. */
    static const char *const garbage[] = { "x", "not a gzip member", "\x1f\x8b\x08" };
    for (guint i = 0; i < G_N_ELEMENTS(garbage); i++) {
        gsize whole = body->len;
        g_byte_array_append(body, (const guint8 *)garbage[i], strlen(garbage[i]));
        for (gsize step = 0; step <= 1; step++) {
            g_assert_null(inflate_all(FALSE, 1 << 20, body->data, body->len, step, &error));
            assert_failed(error, AIRDROP_ERROR_INVALID);
        }
        g_byte_array_set_size(body, whole);
    }
    body->data[body->len - 8] ^= 1;
    g_assert_null(inflate_all(FALSE, 1 << 20, body->data, body->len, 0, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_byte_array_unref(body);
    g_free(t);
    g_assert_null(inflate_all(FALSE, 1 << 20, (const guint8 *)"PK\x03\x04", 4, 0, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);

    /* A failed inflater stays failed; a sink's error comes back as it is. */
    GByteArray *out = g_byte_array_new();
    AirdropInflater *z = airdrop_inflater_new(TRUE, 100, collect, out);
    g_assert_false(airdrop_inflater_feed(z, (const guint8 *)"\0\0\0\0", 4, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_false(airdrop_inflater_feed(z, (const guint8 *)"\x80\0\0\x01x", 5, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_false(airdrop_inflater_finish(z, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    airdrop_inflater_free(z);
    g_byte_array_unref(out);
    z = airdrop_inflater_new(TRUE, 100, disk_full, NULL);
    g_assert_false(airdrop_inflater_feed(z, (const guint8 *)"\x80\0\0\x01x", 5, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    airdrop_inflater_free(z);
    for (int dvzip = 0; dvzip < 2; dvzip++) {
        AirdropDeflater *d = airdrop_deflater_new(dvzip, disk_full, NULL);
        gboolean ok = airdrop_deflater_feed(d, (const guint8 *)"hello", 5, &error)
                      && airdrop_deflater_finish(d, &error);
        g_assert_false(ok);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
        g_clear_error(&error);
        g_assert_false(airdrop_deflater_feed(d, (const guint8 *)"x", 1, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
        airdrop_deflater_free(d);
    }
}

/* ------------------------------------------------------------ HTTP */

typedef struct {
    GString *log;            /* a head's line, its body and "|done" */
    GHashTable *headers;     /* the last head's, copied */
    gboolean refuse;         /* refuse heads without saying why */
} Http;

static gboolean on_head(const char *method, const char *path, guint status, GHashTable *headers, gpointer user,
                        GError **error)
{
    Http *h = user;
    if (method) g_string_append_printf(h->log, "%s %s\n", method, path);
    else g_string_append_printf(h->log, "%u\n", status);
    g_hash_table_remove_all(h->headers);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, headers);
    while (g_hash_table_iter_next(&it, &k, &v)) g_hash_table_insert(h->headers, g_strdup(k), g_strdup(v));
    return !h->refuse;
}

static gboolean on_body(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Http *h = user;
    g_assert_cmpuint(len, >, 0);
    g_string_append_len(h->log, (const char *)buf, len);
    return TRUE;
}

static gboolean on_done(gpointer user, GError **error)
{
    g_string_append(((Http *)user)->log, "|done\n");
    return TRUE;
}

static const HttpCallbacks HTTP_LOG = { on_head, on_body, on_done };

static Http *http_new(void)
{
    Http *h = g_new0(Http, 1);
    h->log = g_string_new(NULL);
    h->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return h;
}

static void http_free(Http *h)
{
    g_string_free(h->log, TRUE);
    g_hash_table_unref(h->headers);
    g_free(h);
}

/* Feeds `len` bytes in pieces of `step` (0: at once), resetting after each
 * message as a connection does, then closes the connection. */
static gboolean http_run(HttpParser *p, const char *data, gsize len, gsize step, gboolean eof, GError **error)
{
    for (gsize pos = 0; pos < len;) {
        gsize n = step ? MIN(step, len - pos) : len - pos, used;
        if (!http_parser_feed(p, (const guint8 *)data + pos, n, &used, error)) return FALSE;
        pos += used;
        /* A feed stops short only at a message's end, or takes nothing
         * once one has ended: either way, the next message starts here. */
        if (used < n) http_parser_reset(p);
    }
    return !eof || http_parser_eof(p, error);
}

/* Parses `data` as one kind, at once and a byte at a time: the log of both,
 * which must agree, or NULL with the first error. */
static char *http_parse(HttpKind kind, const char *data, gboolean eof, GError **error)
{
    char *logs[2] = { NULL, NULL };
    GError *errors[2] = { NULL, NULL };
    for (int i = 0; i < 2; i++) {
        Http *h = http_new();
        HttpParser *p = http_parser_new(kind, &HTTP_LOG, h);
        if (http_run(p, data, strlen(data), i, eof, &errors[i])) logs[i] = g_strdup(h->log->str);
        http_parser_free(p);
        http_free(h);
    }
    g_assert_cmpstr(logs[0], ==, logs[1]);
    g_assert_true((errors[0] == NULL) == (errors[1] == NULL));
    if (errors[0]) g_assert_cmpint(errors[0]->code, ==, errors[1]->code);
    g_clear_error(&errors[1]);
    g_free(logs[1]);
    if (errors[0]) g_propagate_error(error, errors[0]);
    return logs[0];
}

static void test_http_requests(void)
{
    GError *error = NULL;
    /* HTTP-2: Finder's /Ask head, a chunked body with an extension and a
     * trailer, both dropped. */
    static const char ask[] =
        "POST /Ask HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nContent-Type: application/octet-stream\r\n"
        "Connection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n";
    char *log = http_parse(HTTP_REQUEST, ask, FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(log, ==, "POST /Ask\nhello world|done\n");
    g_free(log);

    /* Names come lowercase, values trimmed; a repeated field is joined. */
    Http *h = http_new();
    HttpParser *p = http_parser_new(HTTP_REQUEST, &HTTP_LOG, h);
    static const char discover[] =
        "POST /Discover HTTP/1.1\r\nContent-Length: 5\r\nExpect:  100-continue \r\nAccept: a\r\nACCEPT: b\r\n"
        "X-Empty:\r\n\r\nhello";
    g_assert_true(http_run(p, discover, strlen(discover), 0, FALSE, &error));
    g_assert_cmpstr(h->log->str, ==, "POST /Discover\nhello|done\n");
    g_assert_cmpstr(g_hash_table_lookup(h->headers, "content-length"), ==, "5");
    g_assert_cmpstr(g_hash_table_lookup(h->headers, "expect"), ==, "100-continue");
    g_assert_cmpstr(g_hash_table_lookup(h->headers, "accept"), ==, "a, b");
    g_assert_cmpstr(g_hash_table_lookup(h->headers, "x-empty"), ==, "");
    g_assert_null(g_hash_table_lookup(h->headers, "Content-Length"));
    http_parser_free(p);
    http_free(h);

    /* Keep-alive: two requests in one buffer. The first feed stops at the
     * first one's end; after a reset, the rest is the second. */
    static const char two[] =
        "POST /Discover HTTP/1.1\r\nContent-Length: 2\r\n\r\nab"
        "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nxyz\r\n0\r\n\r\n"
        "GET / HTTP/1.1\r\n\r\n";
    h = http_new();
    p = http_parser_new(HTTP_REQUEST, &HTTP_LOG, h);
    gsize used;
    g_assert_true(http_parser_feed(p, (const guint8 *)two, strlen(two), &used, &error));
    g_assert_cmpuint(used, ==, strlen("POST /Discover HTTP/1.1\r\nContent-Length: 2\r\n\r\nab"));
    g_assert_cmpstr(h->log->str, ==, "POST /Discover\nab|done\n");
    /* Done until reset. */
    gsize again;
    g_assert_true(http_parser_feed(p, (const guint8 *)two + used, strlen(two) - used, &again, &error));
    g_assert_cmpuint(again, ==, 0);
    http_parser_free(p);
    http_free(h);
    log = http_parse(HTTP_REQUEST, two, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(log, ==, "POST /Discover\nab|done\nPOST /Ask\nxyz|done\nGET /\n|done\n");
    g_free(log);

    /* A head of exactly HTTP_MAX_HEAD bytes is taken. */
    GString *s = g_string_new("POST / HTTP/1.1\r\nX: ");
    while (s->len < HTTP_MAX_HEAD - 4) g_string_append_c(s, 'a');
    g_string_append(s, "\r\n\r\n");
    g_assert_cmpuint(s->len, ==, HTTP_MAX_HEAD);
    log = http_parse(HTTP_REQUEST, s->str, FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(log, ==, "POST /\n|done\n");
    g_free(log);
    g_string_insert_c(s, 20, 'a');
    g_assert_null(http_parse(HTTP_REQUEST, s->str, FALSE, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_string_free(s, TRUE);
}

static void test_http_responses(void)
{
    GError *error = NULL;
    static const struct { const char *in, *log; gboolean eof; } cases[] = {
        { "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "200\nabc|done\n", FALSE },
        /* Neither length nor chunked: the body runs to the connection's end. */
        { "HTTP/1.1 403\r\n\r\nno, thanks", "403\nno, thanks|done\n", TRUE },
        { "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", "200\n|done\n", TRUE },
        /* UP-3: an interim 100 Continue has no body, even without a
         * length, and the final response follows it. */
        { "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", "100\n|done\n200\nok|done\n", FALSE },
        { "HTTP/1.1 100 Continue\r\nContent-Length: 9\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          "2\r\nok\r\n0\r\n\r\n", "100\n|done\n200\nok|done\n", FALSE },
        { "HTTP/1.1 204 No Content\r\n\r\nHTTP/1.1 304 Not Modified\r\n\r\nHTTP/1.1 200 OK\r\n\r\nend",
          "204\n|done\n304\n|done\n200\nend|done\n", TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        char *log = http_parse(HTTP_RESPONSE, cases[i].in, cases[i].eof, &error);
        g_assert_no_error(error);
        g_assert_cmpstr(log, ==, cases[i].log);
        g_free(log);
    }

    /* The connection closing: fine between messages and after one that
     * runs to the end, not inside one. */
    static const char *const cut[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", "HTTP/1.1 200 OK\r\nContent-Le",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n",
    };
    for (guint i = 0; i < G_N_ELEMENTS(cut); i++) {
        g_assert_null(http_parse(HTTP_RESPONSE, cut[i], TRUE, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }
    HttpParser *p = http_parser_new(HTTP_RESPONSE, NULL, NULL);
    g_assert_true(http_parser_eof(p, &error));
    http_parser_free(p);
}

static void test_http_rejected(void)
{
    /* SEC-7, after owly's cases. */
    static const struct { const char *in; AirdropError code; } requests[] = {
        { "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: 1, 1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: +1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nContent-Length : 1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nX: a\r\n b\r\n\r\n", AIRDROP_ERROR_INVALID },          /* obs-fold */
        { "POST / HTTP/1.1\r\n\tX: a\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\nContent-Length: 1\r\n\r\n", AIRDROP_ERROR_INVALID },    /* a bare LF */
        { "POST / HTTP/1.1\r\nX: a\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\n\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nX: a\rb\r\n\r\n", AIRDROP_ERROR_INVALID },           /* a bare CR */
        { "POST / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nX: \xff\r\n\r\n", AIRDROP_ERROR_INVALID },           /* not UTF-8 */
        { "POST / HTTP/1.1\r\nNo colon\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\n: empty name\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST  / HTTP/1.1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1 \r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST /\x7f HTTP/1.1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.0\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST http://x/ HTTP/1.1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "PO(ST / HTTP/1.1\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "\r\nPOST / HTTP/1.1\r\n\r\n", AIRDROP_ERROR_INVALID },
        /* Chunks: hex sizes only, 16 digits at most, CRLF after the data. */
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nx\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n 5\r\nhello\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5 x\r\nhello\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0x5\r\nhello\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5;a\x01\r\nhello\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n11111111111111111\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nbad trailer\r\n\r\n", AIRDROP_ERROR_INVALID },
        { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n folded: x\r\n\r\n", AIRDROP_ERROR_INVALID },
    };
    for (guint i = 0; i < G_N_ELEMENTS(requests); i++) {
        GError *error = NULL;
        g_assert_null(http_parse(HTTP_REQUEST, requests[i].in, FALSE, &error));
        assert_failed(error, requests[i].code);
    }
    static const char *const responses[] = {
        "HTTP/1.0 200 OK\r\n\r\n", "HTTP/1.1 600 Odd\r\n\r\n", "HTTP/1.1 099 Odd\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 200OK\r\n\r\n", "HTTP/1.1  200 OK\r\n\r\n", "HTTP/1.1 2x0 OK\r\n\r\n", "http/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
    };
    for (guint i = 0; i < G_N_ELEMENTS(responses); i++) {
        GError *error = NULL;
        g_assert_null(http_parse(HTTP_RESPONSE, responses[i], FALSE, &error));
        assert_failed(error, AIRDROP_ERROR_INVALID);
    }

    /* Limits: the head, the number of fields, a chunk line, a trailer. */
    GError *error = NULL;
    GString *s = g_string_new("POST / HTTP/1.1\r\nX: ");
    for (guint i = 0; i < 20000; i++) g_string_append_c(s, 'a');
    g_assert_null(http_parse(HTTP_REQUEST, s->str, FALSE, &error));   /* no end in sight */
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_string_append(s, "\r\n\r\n");
    g_assert_null(http_parse(HTTP_REQUEST, s->str, FALSE, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_string_assign(s, "POST / HTTP/1.1\r\n");
    for (guint i = 0; i < 65; i++) g_string_append_printf(s, "X%u: a\r\n", i);
    g_string_append(s, "\r\n");
    g_assert_null(http_parse(HTTP_REQUEST, s->str, FALSE, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_string_assign(s, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5;");
    for (guint i = 0; i < 2000; i++) g_string_append_c(s, 'e');
    g_string_append(s, "\r\nhello\r\n0\r\n\r\n");
    g_assert_null(http_parse(HTTP_REQUEST, s->str, FALSE, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    g_string_assign(s, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n");
    for (guint i = 0; i < 20; i++) g_string_append_printf(s, "X%u: %01000u\r\n", i, i);
    g_string_append(s, "\r\n");
    g_assert_null(http_parse(HTTP_REQUEST, s->str, FALSE, &error));
    assert_failed(error, AIRDROP_ERROR_LIMIT);
    /* Extensions within the line's limit are fine. */
    g_string_assign(s, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0000000000000005 ;a=b;c=\"d\"\r\nhello\r\n0\r\n\r\n");
    char *log = http_parse(HTTP_REQUEST, s->str, FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(log, ==, "POST /\nhello|done\n");
    g_free(log);
    g_string_free(s, TRUE);

    /* A head refused by the caller, with no reason given; a failed parser
     * stays failed. */
    Http *h = http_new();
    h->refuse = TRUE;
    HttpParser *p = http_parser_new(HTTP_REQUEST, &HTTP_LOG, h);
    gsize used;
    g_assert_false(http_parser_feed(p, (const guint8 *)"GET / HTTP/1.1\r\n\r\n", 18, &used, &error));
    assert_failed(error, AIRDROP_ERROR_REFUSED);
    g_assert_false(http_parser_feed(p, (const guint8 *)"GET", 3, &used, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    g_assert_false(http_parser_eof(p, &error));
    assert_failed(error, AIRDROP_ERROR_INVALID);
    /* A reset starts over. */
    h->refuse = FALSE;
    http_parser_reset(p);
    g_assert_true(http_parser_feed(p, (const guint8 *)"GET / HTTP/1.1\r\n\r\n", 18, &used, &error));
    g_assert_no_error(error);
    http_parser_free(p);
    http_free(h);
}

static void test_http_chunk(void)
{
    GBytes *b = http_chunk((const guint8 *)"hello", 5);
    g_assert_cmpmem(g_bytes_get_data(b, NULL), g_bytes_get_size(b), "5\r\nhello\r\n", 10);
    g_bytes_unref(b);
    b = http_chunk(NULL, 0);
    g_assert_cmpmem(g_bytes_get_data(b, NULL), g_bytes_get_size(b), "0\r\n\r\n", 5);
    g_bytes_unref(b);

    /* Chunks made here, read back by the parser. */
    guint8 *t = text(70000);
    GString *s = g_string_new("POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    GBytes *parts[] = { http_chunk(t, 69999), http_chunk(t + 69999, 1), http_chunk(NULL, 0) };
    for (guint i = 0; i < G_N_ELEMENTS(parts); i++) {
        g_string_append_len(s, g_bytes_get_data(parts[i], NULL), g_bytes_get_size(parts[i]));
        g_bytes_unref(parts[i]);
    }
    g_assert_true(g_str_has_prefix(s->str + strlen("POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"), "1116f\r\n"));
    GError *error = NULL;
    char *log = http_parse(HTTP_REQUEST, s->str, FALSE, &error);
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(log, "POST /Ask\nAirDrop vector line 000000\n"));
    g_assert_cmpuint(strlen(log), ==, strlen("POST /Ask\n") + 70000 + strlen("|done\n"));
    g_assert_cmpmem(log + strlen("POST /Ask\n"), 70000, t, 70000);
    g_free(log);
    g_string_free(s, TRUE);
    g_free(t);
}

/* ------------------------------------------------------------ an upload */

/* An /Upload as a receiver takes it: HTTP framing, then gzip or DVZip,
 * then the archive, each streaming into the next. */
typedef struct {
    HttpParser *http;
    AirdropInflater *z;
    CpioReader *cpio;
    Cpio log;
    char *transfer_id;
    gboolean done;   /* the archive's trailer was read (UP-11) */
} Upload;

static gboolean to_cpio(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Upload *u = user;
    return cpio_reader_feed(u->cpio, buf, len, &u->done, error);
}

static gboolean upload_head(const char *method, const char *path, guint status, GHashTable *headers, gpointer user,
                            GError **error)
{
    Upload *u = user;
    g_assert_cmpstr(method, ==, "POST");
    g_assert_cmpstr(path, ==, "/Upload");
    const char *type = g_hash_table_lookup(headers, "content-type");
    gboolean dvzip = g_strcmp0(type, "application/x-dvzip") == 0;
    g_assert_true(dvzip || g_strcmp0(type, "application/x-cpio") == 0);
    u->transfer_id = g_strdup(g_hash_table_lookup(headers, "transferid"));
    u->z = airdrop_inflater_new(dvzip, 1 << 20, to_cpio, u);
    return TRUE;
}

static gboolean upload_body(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    return airdrop_inflater_feed(((Upload *)user)->z, buf, len, error);
}

static gboolean upload_done(gpointer user, GError **error)
{
    Upload *u = user;
    return airdrop_inflater_finish(u->z, error) && cpio_reader_finish(u->cpio, error);
}

static void test_upload_vectors(void)
{
    static const HttpCallbacks cb = { upload_head, upload_body, upload_done };
    static const struct { const guint8 *data; gsize len; const char *log, *id; } uploads[] = {
        { UPLOAD_FINDER, sizeof UPLOAD_FINDER, "dir . 0\n|end\nfile ./hello.txt 12\nhello world\n|end\n",
          "A3F34E01-0000-4000-8000-000000000001" },
        /* The legacy form: gzip, chunked, no TransferID and no root entry. */
        { UPLOAD_LEGACY, sizeof UPLOAD_LEGACY, "file ./hello.txt 12\nhello world\n|end\n", NULL },
    };
    static const gsize steps[] = { 0, 1, 7, 100 };
    for (guint i = 0; i < G_N_ELEMENTS(uploads); i++)
        for (guint s = 0; s < G_N_ELEMENTS(steps); s++) {
            Upload u = { 0 };
            u.log.log = g_string_new(NULL);
            u.cpio = cpio_reader_new(&CPIO_LOG, &u.log);
            u.http = http_parser_new(HTTP_REQUEST, &cb, &u);
            GError *error = NULL;
            gsize len = uploads[i].len, step = steps[s] ? steps[s] : len;
            for (gsize pos = 0; pos < len; pos += step) {
                gsize used;
                g_assert_true(http_parser_feed(u.http, uploads[i].data + pos, MIN(step, len - pos), &used, &error));
                g_assert_no_error(error);
            }
            g_assert_true(u.done);
            g_assert_cmpstr(u.log.log->str, ==, uploads[i].log);
            g_assert_cmpstr(u.transfer_id, ==, uploads[i].id);
            http_parser_free(u.http);
            airdrop_inflater_free(u.z);
            cpio_reader_free(u.cpio);
            g_string_free(u.log.log, TRUE);
            g_free(u.transfer_id);
        }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/airdrop/names/safe", test_safe_name);
    g_test_add_func("/airdrop/plist/round-trip", test_plist_round_trip);
    g_test_add_func("/airdrop/plist/exact", test_plist_exact);
    g_test_add_func("/airdrop/plist/vectors", test_plist_vectors);
    g_test_add_func("/airdrop/plist/malformed", test_plist_malformed);
    g_test_add_func("/airdrop/plist/limits", test_plist_limits);
    g_test_add_func("/airdrop/cpio/vectors", test_cpio_vectors);
    g_test_add_func("/airdrop/cpio/after-trailer", test_cpio_after_trailer);
    g_test_add_func("/airdrop/cpio/refused", test_cpio_refused);
    g_test_add_func("/airdrop/cpio/malformed", test_cpio_malformed);
    g_test_add_func("/airdrop/cpio/limits", test_cpio_limits);
    g_test_add_func("/airdrop/cpio/writer", test_cpio_writer);
    g_test_add_func("/airdrop/cpio/writer-errors", test_cpio_writer_errors);
    g_test_add_func("/airdrop/compress/round-trip", test_compress_round_trip);
    g_test_add_func("/airdrop/compress/gzip-members", test_gzip_members);
    g_test_add_func("/airdrop/compress/bombs", test_compress_bombs);
    g_test_add_func("/airdrop/compress/malformed", test_compress_malformed);
    g_test_add_func("/airdrop/http/requests", test_http_requests);
    g_test_add_func("/airdrop/http/responses", test_http_responses);
    g_test_add_func("/airdrop/http/rejected", test_http_rejected);
    g_test_add_func("/airdrop/http/chunk", test_http_chunk);
    g_test_add_func("/airdrop/upload/vectors", test_upload_vectors);
    return g_test_run();
}
