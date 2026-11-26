// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* AirDrop's formats on Windows (win/share/airdrop/formats.h), with every
 * test of tests/formats.c and the mDNS tests of tests/discovery.c, on the
 * same vectors, then the mDNS engine's timing, which the Linux code runs on
 * its own socket and timer and these drive by hand.
 *
 * The vectors named owly's come from owly's conformance set
 * (tests/vectors/airdrop-vectors.json, MIT), which rebuilds AirDrop's
 * messages from their documented shapes. The PY_ ones were written by
 * Python's plistlib, which lays a plist out as Core Foundation does,
 * sharing repeated values. Every other input is built here by hand. */
#include "share_check.h"
#include "share_cms_test.h"

#include "apple_ca.h"
#include "formats.h"
#include "zlib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace lares::airdrop;

#define CAFE_JPG "Caf\xc3\xa9 \xe2\x98\x95.jpg"   /* "Café ☕.jpg" */
#define SMILE    "\xf0\x9f\x98\x80"               /* U+1F600, a surrogate pair in UTF-16 */

/* A failure with `code`, cleared for the next check. */
#define CHECK_FAILED(err, c)                            \
    do {                                                \
        CHECK((err).set && (err).code == (c));          \
        (err).clear();                                  \
    } while (0)

/* No error; one that is there is printed. */
#define CHECK_NO_ERROR(err)                                                              \
    do {                                                                                 \
        CHECK(!(err).set);                                                               \
        if ((err).set) std::fprintf(stderr, "  error: %s\n", (err).message.c_str());   \
        (err).clear();                                                                   \
    } while (0)

namespace {

constexpr unsigned MTIME = 1700000000;
constexpr int64_t SEC = 1000000;

const ErrorCode INVALID = ErrorCode::Invalid;
const ErrorCode LIMIT = ErrorCode::Limit;
const ErrorCode REFUSED = ErrorCode::Refused;

/* ------------------------------------------------------------ vectors */

/* owly's "ios18-hello" /Ask body. */
const uint8_t ASK_HELLO[] = {
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
const uint8_t CPIO_ODC[] = {
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
const uint8_t CPIO_NEWC[] = {
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
const uint8_t UPLOAD_FINDER[] = {
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
const uint8_t UPLOAD_LEGACY[] = {
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
const uint8_t PY_MIXED[] = {
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
const uint8_t PY_BIG[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02, 0x53, 0x42, 0x69, 0x67, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x0b, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x20,
};

/* plistlib: [2^63 - 1, -2^63, 0xff, 0x100, 0xffff, 0x10000, 0xffffffff, 0x100000000]. */
const uint8_t PY_MAXU[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xa8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x13, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x13, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0xff, 0x11, 0x01, 0x00, 0x11, 0xff, 0xff, 0x12, 0x00, 0x01, 0x00, 0x00,
    0x12, 0xff, 0xff, 0xff, 0xff, 0x13, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x11,
    0x1a, 0x23, 0x25, 0x28, 0x2b, 0x30, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e,
};

/* ------------------------------------------------------------ helpers */

Sink collect(Bytes &out)
{
    return [&out](const uint8_t *buf, size_t len, Error &) {
        out.insert(out.end(), buf, buf + len);
        return true;
    };
}

bool disk_full(const uint8_t *, size_t, Error &error)
{
    error.assign(ErrorCode::Failed, "No space left");
    return false;
}

bool same(const Bytes &a, const void *b, size_t n)
{
    return a.size() == n && (n == 0 || std::memcmp(a.data(), b, n) == 0);
}

Bytes bytes_of(const char *s, size_t n)
{
    return Bytes(s, s + n);
}

/* Bytes zlib can't shrink, as a photo's are: GRand's Mersenne Twister,
 * seeded as the Linux test seeds it. */
Bytes noise(size_t n)
{
    std::mt19937 rand(64);
    Bytes out(n);
    for (size_t i = 0; i < n; i++) out[i] = static_cast<uint8_t>(rand() >> 24);
    return out;
}

/* Lines of text, which shrink well. */
Bytes text(size_t n)
{
    std::string s;
    char line[64];
    for (unsigned i = 0; s.size() < n; i++) {
        std::snprintf(line, sizeof line, "AirDrop vector line %06u\n", i);
        s += line;
    }
    s.resize(n);
    return Bytes(s.begin(), s.end());
}

void put_be(Bytes &out, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) out.push_back(static_cast<uint8_t>(v >> (8 * (n - 1 - i))));
}

/* ------------------------------------------------------------ names */

void test_safe_name()
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
    for (const char *g : good) CHECK(airdrop_safe_name(g));
    for (const char *b : bad) CHECK(!airdrop_safe_name(b));
    CHECK(!airdrop_safe_name(static_cast<const char *>(nullptr)));
    CHECK(airdrop_safe_name(std::string(255, 'a')));
    CHECK(!airdrop_safe_name(std::string(256, 'a')));
    /* Only a std::string holds a NUL inside: refused. */
    CHECK(!airdrop_safe_name(std::string("a\0b", 3)));
}

void test_uti()
{
    CHECK(std::strcmp(airdrop_uti("IMG_0049.JPG", false), "public.jpeg") == 0);
    CHECK(std::strcmp(airdrop_uti("notes.txt", false), "public.plain-text") == 0);
    CHECK(std::strcmp(airdrop_uti("a.tar.Zip", false), "public.zip-archive") == 0);
    CHECK(std::strcmp(airdrop_uti("README", false), "public.data") == 0);
    CHECK(std::strcmp(airdrop_uti("odd.", false), "public.data") == 0);
    CHECK(std::strcmp(airdrop_uti("photos.jpg", true), "public.folder") == 0);
}

/* ------------------------------------------------------------ plists */

/* A plist built object by object, for inputs the writer would never make. */
struct Plist {
    Bytes out = Bytes{ 'b', 'p', 'l', 'i', 's', 't', '0', '0' };
    std::vector<uint64_t> offsets;

    void obj(const void *bytes, size_t len)
    {
        offsets.push_back(out.size());
        const auto *b = static_cast<const uint8_t *>(bytes);
        out.insert(out.end(), b, b + len);
    }

    Bytes end(unsigned offset_size, unsigned ref_size, uint64_t top)
    {
        uint64_t table = out.size();
        for (uint64_t o : offsets) put_be(out, o, offset_size);
        out.insert(out.end(), size_t{ 6 }, uint8_t{ 0 });
        out.push_back(static_cast<uint8_t>(offset_size));
        out.push_back(static_cast<uint8_t>(ref_size));
        put_be(out, offsets.size(), 8);
        put_be(out, top, 8);
        put_be(out, table, 8);
        return out;
    }
};

/* Fails to decode with `code`. */
#define CHECK_REFUSED(bytes, c)                                    \
    do {                                                           \
        const Bytes b_ = (bytes);                                  \
        Value v_;                                                  \
        Error e_;                                                  \
        CHECK(!bplist_decode(b_.data(), b_.size(), v_, &e_));      \
        CHECK_FAILED(e_, c);                                       \
    } while (0)

bool decode(const Bytes &b, Value &v, Error &e)
{
    return bplist_decode(b.data(), b.size(), v, &e);
}

/* Arrays nested `depth` deep, each holding the next, built by hand. */
Bytes nested(unsigned depth)
{
    Plist p;
    for (unsigned i = 0; i < depth; i++) {
        uint8_t obj[3] = { 0xa1, static_cast<uint8_t>((i + 1) >> 8), static_cast<uint8_t>(i + 1) };
        if (i + 1 < depth) p.obj(obj, 3);
        else p.obj("\xa0", 1);
    }
    return p.end(2, 2, 0);
}

Value file_entry(const char *name, const char *type, int64_t size, const char *bom)
{
    Value f = Value::dict();
    f.add("FileName", Value::string(name));
    f.add("FileType", Value::string(type));
    f.add("FileSize", Value::integer(size));
    f.add("FileBomPath", Value::string(bom));
    f.add("FileIsDirectory", Value::boolean(false));
    f.add("ShouldConvertMediaFormats", Value::boolean(false));
    return f;
}

Value ask_like()
{
    uint8_t icon[300];
    for (unsigned i = 0; i < sizeof icon; i++) icon[i] = static_cast<uint8_t>(i * 7);
    Value b = Value::dict();
    /* Keys out of order on purpose: the writer keeps them as given. */
    b.add("TransferID", Value::dict().add("id", Value::string("A3F34E01-1234-5678-ABCD-EF0123456789")));
    b.add("TransferType", Value::dict().add("files", Value::dict()));
    b.add("SenderComputerName", Value::string("Caf\xc3\xa9 \xe2\x98\x95 Mac"));
    b.add("SenderModelName", Value::string("MacBook Pro"));
    b.add("SenderID", Value::string("486ac2a94ac6"));
    b.add("BundleID", Value::string("com.apple.finder"));
    b.add("ConvertMediaFormats", Value::boolean(false));
    b.add("SenderIdentityAuthTag", Value::data("\x0c\x50\x4b", 3));
    b.add("FileIcon", Value::data(icon, sizeof icon));
    Value files = Value::array();
    files.push(file_entry(CAFE_JPG, "public.jpeg", 141853, "./" CAFE_JPG));
    files.push(file_entry(SMILE ".txt", "public.plain-text", 0, "./" SMILE ".txt"));
    b.add("Files", files);
    b.add("Items", Value::array());
    Value numbers = Value::array();
    for (int64_t n : { int64_t{ 0 }, int64_t{ 255 }, int64_t{ 256 }, int64_t{ 65535 }, int64_t{ 65536 },
                       int64_t{ 4294967295 }, int64_t{ 4294967296 }, int64_t{ -1 }, int64_t{ -5 }, INT64_MAX,
                       INT64_MIN })
        numbers.push(Value::integer(n));
    b.add("Numbers", numbers);
    b.add("Real", Value::real(1.5));
    b.add("Yes", Value::boolean(true));
    b.add("A string past fifteen characters", Value::string("so its count follows its marker"));
    return b;
}

void test_plist_round_trip()
{
    Value ask = ask_like();
    Error error;
    Bytes bytes;
    CHECK(bplist_encode(ask, bytes, &error));
    CHECK_NO_ERROR(error);
    Value back;
    CHECK(decode(bytes, back, error));
    CHECK_NO_ERROR(error);
    CHECK(back == ask);

    /* The keys come back in the order they went in. */
    CHECK(back.size() == ask.size());
    for (size_t i = 0; i < ask.size() && i < back.size(); i++) CHECK(ask.key(i) == back.key(i));

    /* Every shorter piece of it is refused. */
    for (size_t n = 0; n < bytes.size(); n++) {
        Value v;
        CHECK(!bplist_decode(bytes.data(), n, v, &error));
        CHECK(error.set);
        error.clear();
    }
}

/* The writer's bytes, exactly as Core Foundation and plistlib lay them out. */
void test_plist_exact()
{
    static const uint8_t dict[] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xd1, 0x01, 0x02, 0x51, 0x61, 0x09,
        0x08, 0x0b, 0x0d,                                       /* the offsets */
        0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0e,
    };
    static const uint8_t utf16[] = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0', 0x6a, 0x00, 0x43, 0x00, 0x61, 0x00, 0x66, 0x00, 0xe9,
        0x00, 0x20, 0x26, 0x15, 0x00, 0x2e, 0x00, 0x6a, 0x00, 0x70, 0x00, 0x67,
        0x08,
        0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1d,
    };
    Error error;
    Bytes b;
    CHECK(bplist_encode(Value::dict().add("a", Value::boolean(true)), b, &error));
    CHECK_NO_ERROR(error);
    CHECK(same(b, dict, sizeof dict));
    CHECK(bplist_encode(Value::string(CAFE_JPG), b, &error));
    CHECK_NO_ERROR(error);
    CHECK(same(b, utf16, sizeof utf16));

    /* References wide enough for the object count itself: 256 objects take
     * 2 bytes each, as Core Foundation requires. */
    for (unsigned n = 254; n <= 256; n++) {
        Value array = Value::array();
        for (unsigned i = 0; i < n; i++) array.push(Value::integer(i));
        CHECK(bplist_encode(array, b, &error));
        CHECK_NO_ERROR(error);
        CHECK(b.size() > 32 && b[b.size() - 32 + 7] == (n + 1 < 256 ? 1 : 2));
        Value back;
        CHECK(decode(b, back, error));
        CHECK_NO_ERROR(error);
        CHECK(back == array);
    }
}

void test_plist_vectors()
{
    Error error;
    Value ask;
    CHECK(bplist_decode(ASK_HELLO, sizeof ASK_HELLO, ask, &error));
    CHECK_NO_ERROR(error);
    const Value *transfer = ask.find("TransferID", Value::Type::Dict);
    const Value *id = transfer ? transfer->find("id", Value::Type::String) : nullptr;
    CHECK(id && id->get_string() == "A3F34E01-0000-4000-8000-000000000001");
    const Value *convert = ask.find("ConvertMediaFormats", Value::Type::Boolean);
    CHECK(convert && !convert->get_boolean());
    const Value *items = ask.find("Items", Value::Type::Array);
    CHECK(items && items->size() == 0);
    const Value *files = ask.find("Files", Value::Type::Array);
    CHECK(files && files->size() == 1);
    if (files && files->size() == 1) {
        const Value &file = files->at(0);
        const Value *name = file.find("FileName", Value::Type::String);
        const Value *bom = file.find("FileBomPath", Value::Type::String);
        const Value *size = file.find("FileSize", Value::Type::Integer);
        /* FileIsDirectory shares one false object with two other keys. */
        const Value *dir = file.find("FileIsDirectory", Value::Type::Boolean);
        CHECK(name && name->get_string() == "hello.txt");
        CHECK(bom && bom->get_string() == "./hello.txt");
        CHECK(size && size->get_integer() == 12);
        CHECK(dir && !dir->get_boolean());
    }

    /* Dates and UIDs drop out of dicts and arrays; a string shared by two
     * values stays the same string. */
    Value mixed;
    CHECK(bplist_decode(PY_MIXED, sizeof PY_MIXED, mixed, &error));
    CHECK_NO_ERROR(error);
    Value expect = Value::dict();
    expect.add("Name", Value::string(CAFE_JPG));
    expect.add("Smile", Value::string(SMILE));
    expect.add("Negative", Value::integer(-5));
    expect.add("Real", Value::real(1.5));
    expect.add("List", Value::array().push(Value::integer(1)).push(Value::string("x")));
    expect.add("Same", Value::string("x"));
    CHECK(mixed == expect);

    Value ints;
    CHECK(bplist_decode(PY_MAXU, sizeof PY_MAXU, ints, &error));
    CHECK_NO_ERROR(error);
    expect = Value::array();
    for (int64_t n : { INT64_MAX, INT64_MIN, int64_t{ 255 }, int64_t{ 256 }, int64_t{ 65535 }, int64_t{ 65536 },
                       int64_t{ 4294967295 }, int64_t{ 4294967296 } })
        expect.push(Value::integer(n));
    CHECK(ints == expect);

    /* 2^63, which Core Foundation and plistlib write in 16 bytes, doesn't fit. */
    Value big;
    CHECK(!bplist_decode(PY_BIG, sizeof PY_BIG, big, &error));
    CHECK_FAILED(error, INVALID);

    /* A small value in 16 bytes does, and so does a 4-byte real. */
    Plist p;
    p.obj("\xa2\x01\x02", 3);
    p.obj("\x14\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x2a", 17);
    p.obj("\x22\x3f\xc0\x00\x00", 5);
    Value v;
    CHECK(decode(p.end(1, 1, 0), v, error));
    CHECK_NO_ERROR(error);
    CHECK(v == Value::array().push(Value::integer(42)).push(Value::real(1.5)));
}

void test_plist_malformed()
{
    Error error;
    Value v;
    /* SEC-6: bplist00 only. */
    static const char xml[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>";
    CHECK(!bplist_decode(reinterpret_cast<const uint8_t *>(xml), std::strlen(xml), v, &error));
    CHECK_FAILED(error, INVALID);
    Bytes good;
    CHECK(bplist_encode(Value::dict().add("a", Value::boolean(true)), good));
    Bytes bad = good;
    bad[7] = '1';
    CHECK_REFUSED(bad, INVALID);

    /* Each trailer field out of range. */
    static const struct {
        unsigned at;
        uint8_t value;
    } trailer[] = {
        { 6, 0 }, { 6, 9 }, { 7, 0 }, { 7, 9 },       /* offset and reference sizes */
        { 15, 0 }, { 15, 0xff }, { 8, 0x80 },          /* object count: none, more than fit, 2^63 */
        { 23, 3 },                                     /* the top object past the count */
        { 31, 8 }, { 31, 7 }, { 31, 0x20 }, { 24, 1 }, /* the table's offset */
    };
    for (const auto &t : trailer) {
        bad = good;
        bad[bad.size() - 32 + t.at] = t.value;
        CHECK_REFUSED(bad, INVALID);
    }
    /* An offset into the table, or before the objects. */
    for (int o = 0; o < 2; o++) {
        bad = good;
        bad[14] = o ? 0x0e : 0x07;
        CHECK_REFUSED(bad, INVALID);
    }
    /* A reference past the object count. */
    bad = good;
    bad[10] = 3;
    CHECK_REFUSED(bad, INVALID);

    /* Too short, and too large (SEC-3). */
    CHECK_REFUSED(bytes_of("bplist00\x08", 9), INVALID);
    Bytes huge(BPLIST_MAX_BYTES + 1, 0);
    std::memcpy(huge.data(), "bplist00", 8);
    CHECK_REFUSED(huge, LIMIT);

    /* Objects the reader doesn't take, or that are broken. */
    static const struct {
        const char *bytes;
        size_t len;
    } objects[] = {
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
    for (const auto &o : objects) {
        Plist p;
        p.obj(o.bytes, o.len);
        CHECK_REFUSED(p.end(1, 1, 0), INVALID);
    }

    /* A key that isn't a string, and a key given twice. */
    {
        Plist p;
        p.obj("\xd1\x01\x02", 3);
        p.obj("\x10\x05", 2);
        p.obj("\x09", 1);
        CHECK_REFUSED(p.end(1, 1, 0), INVALID);
    }
    {
        Plist p;
        p.obj("\xd2\x01\x01\x02\x03", 5);
        p.obj("\x51\x61", 2);
        p.obj("\x09", 1);
        p.obj("\x33\0\0\0\0\0\0\0\0", 9);   /* even when one value is a date */
        CHECK_REFUSED(p.end(1, 1, 0), INVALID);
    }

    /* Reference cycles: an array holding itself, and two holding each other. */
    {
        Plist p;
        p.obj("\xa1\x00", 2);
        CHECK_REFUSED(p.end(1, 1, 0), INVALID);
    }
    {
        Plist p;
        p.obj("\xa1\x01", 2);
        p.obj("\xd1\x02\x00", 3);
        p.obj("\x51\x6b", 2);
        CHECK_REFUSED(p.end(1, 1, 0), INVALID);
    }
}

void test_plist_limits()
{
    Error error;
    Value v;
    /* SEC-6: 64 levels of nesting, no more. */
    CHECK(decode(nested(64), v, error));
    CHECK_NO_ERROR(error);
    CHECK_REFUSED(nested(65), LIMIT);
    CHECK_REFUSED(nested(200), LIMIT);

    /* SEC-3: each array holds the next twice, 2^40 values from 41 objects. */
    {
        Plist p;
        for (unsigned i = 0; i < 41; i++) {
            uint8_t obj[3] = { 0xa2, static_cast<uint8_t>(i + 1), static_cast<uint8_t>(i + 1) };
            if (i < 40) p.obj(obj, 3);
            else p.obj("\x08", 1);
        }
        CHECK_REFUSED(p.end(1, 1, 0), LIMIT);
    }

    /* SEC-3: one 64 KiB data object referenced 1000 times, 64 MB from 66 KB. */
    {
        Plist p;
        Bytes array = { 0xaf, 0x11, 0x03, 0xe8 };
        for (unsigned i = 0; i < 1000; i++) {
            array.push_back(0x00);
            array.push_back(0x01);
        }
        p.obj(array.data(), array.size());
        Bytes data(6 + 65536, 0);
        std::memcpy(data.data(), "\x4f\x12\x00\x01\x00\x00", 6);
        p.obj(data.data(), data.size());
        CHECK_REFUSED(p.end(4, 2, 0), LIMIT);
    }

    /* The object budget counts every object: 65536 decode, 65537 don't. */
    for (unsigned n = 65535; n <= 65536; n++) {
        Value array = Value::array();
        for (unsigned i = 0; i < n; i++) array.push(Value::boolean(true));
        Bytes b;
        CHECK(bplist_encode(array, b, &error));
        CHECK_NO_ERROR(error);
        if (n + 1 <= BPLIST_MAX_OBJECTS) {
            CHECK(decode(b, v, error));
            CHECK_NO_ERROR(error);
            CHECK(v.size() == n);
        } else {
            CHECK_REFUSED(b, LIMIT);
        }
    }

    /* The writer: as deep and as large as the reader takes, and no more. */
    for (unsigned depth = 64; depth <= 65; depth++) {
        Value deep = Value::array();
        for (unsigned i = 1; i < depth; i++) {
            Value outer = Value::array();
            outer.push(std::move(deep));
            deep = std::move(outer);
        }
        Bytes b;
        if (depth == 64) {
            CHECK(bplist_encode(deep, b, &error));
            CHECK_NO_ERROR(error);
            CHECK(decode(b, v, error));
            CHECK_NO_ERROR(error);
        } else {
            CHECK(!bplist_encode(deep, b, &error));
            CHECK(b.empty());
            CHECK_FAILED(error, LIMIT);
        }
    }
    {
        Bytes data(BPLIST_MAX_BYTES, 0);
        Bytes b;
        CHECK(bplist_encode(Value::data(data.data(), BPLIST_MAX_BYTES - 64), b, &error));
        CHECK_NO_ERROR(error);
        CHECK(b.size() <= BPLIST_MAX_BYTES);
        CHECK(decode(b, v, error));
        CHECK_NO_ERROR(error);
        Bytes none;
        CHECK(!bplist_encode(Value::data(data.data(), BPLIST_MAX_BYTES), none, &error));
        CHECK(none.empty());
        CHECK_FAILED(error, LIMIT);
    }

    /* What GVariant can't hold, and so neither can a plist here: a string
     * or key that isn't UTF-8, or holds a NUL. (The Linux test's tuples,
     * maybes and object paths have no Value to be.) */
    Bytes b;
    CHECK(!bplist_encode(Value::string("\xff"), b, &error));
    CHECK_FAILED(error, INVALID);
    CHECK(!bplist_encode(Value::string(std::string("a\0b", 3)), b, &error));
    CHECK_FAILED(error, INVALID);
    CHECK(!bplist_encode(Value::dict().add("\xc0\xae", Value::boolean(true)), b, &error));
    CHECK_FAILED(error, INVALID);
}

/* ------------------------------------------------------------ cpio */

void odc_entry(Bytes &out, const std::string &name, unsigned mode, unsigned nlink, const char *data, size_t size)
{
    char head[128];
    std::snprintf(head, sizeof head, "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o", 0u, 1u, mode, 501u, 20u,
                  nlink, 0u, MTIME, static_cast<unsigned>(name.size() + 1), static_cast<unsigned>(size));
    out.insert(out.end(), head, head + std::strlen(head));
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    if (size) out.insert(out.end(), data, data + size);
}

void newc_entry(Bytes &out, const std::string &name, unsigned mode, unsigned nlink, const char *data, size_t size)
{
    size_t namesize = name.size() + 1;
    char head[128];
    std::snprintf(head, sizeof head, "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x", 1u, mode, 501u, 20u,
                  nlink, MTIME, static_cast<unsigned>(size), 0u, 0u, 0u, 0u, static_cast<unsigned>(namesize), 0u);
    out.insert(out.end(), head, head + 110);
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    out.insert(out.end(), (4 - (110 + namesize) % 4) % 4, uint8_t{ 0 });
    if (size) out.insert(out.end(), data, data + size);
    out.insert(out.end(), (4 - size % 4) % 4, uint8_t{ 0 });
}

using EntryFn = void (*)(Bytes &, const std::string &, unsigned, unsigned, const char *, size_t);

/* What the reader saw: a line per entry, its data, and "|end" after it. */
struct CpioLog {
    std::string log;
    const char *refuse = nullptr;   /* a path to refuse at its header */
    uint64_t left = 0;              /* data the current entry still has */
};

CpioCallbacks cpio_log(CpioLog &c)
{
    CpioCallbacks cb;
    cb.begin = [&c](const std::string &path, CpioType type, uint64_t size, Error &error) {
        CHECK(c.left == 0);
        c.log += (type == CpioType::Dir ? "dir " : "file ") + path + " " + std::to_string(size) + "\n";
        c.left = size;
        if (c.refuse && path == c.refuse) {
            error.assign(REFUSED, "Not asked for");
            return false;
        }
        return true;
    };
    cb.data = [&c](const uint8_t *buf, size_t len, Error &) {
        /* Nothing past an entry's size reaches it. */
        CHECK(len > 0);
        CHECK(len <= c.left);
        c.left -= std::min<uint64_t>(len, c.left);
        c.log.append(reinterpret_cast<const char *>(buf), len);
        return true;
    };
    cb.end = [&c](Error &) {
        CHECK(c.left == 0);
        c.log += "|end\n";
        return true;
    };
    return cb;
}

/* Reads an archive in pieces of `step` bytes (0: at once) and finishes it:
 * true with the log, or false with the error. */
bool read_cpio(const uint8_t *data, size_t len, size_t step, const char *refuse, Error &error,
               std::string *log = nullptr)
{
    CpioLog c;
    c.refuse = refuse;
    CpioReader r(cpio_log(c));
    bool ok = true, done = false;
    for (size_t pos = 0; ok && pos < len; pos += step ? step : len)
        ok = r.feed(data + pos, std::min(step ? step : len, len - pos), &done, &error);
    ok = ok && r.finish(&error);
    CHECK(!ok || done);
    if (ok && log) *log = c.log;
    return ok;
}

void test_cpio_vectors()
{
    const std::string want = "dir . 0\n|end\nfile ./hello.txt 12\nhello world\n|end\n";
    for (size_t step : { size_t{ 0 }, size_t{ 1 }, size_t{ 7 }, size_t{ 76 }, size_t{ 110 } }) {
        Error error;
        std::string log;
        CHECK(read_cpio(CPIO_ODC, sizeof CPIO_ODC, step, nullptr, error, &log));
        CHECK_NO_ERROR(error);
        CHECK(log == want);
        CHECK(read_cpio(CPIO_NEWC, sizeof CPIO_NEWC, step, nullptr, error, &log));
        CHECK_NO_ERROR(error);
        CHECK(log == want);
    }

    /* Two files back to back, an empty one between: each gets its own
     * bytes, a byte at a time or all at once. */
    for (int newc = 0; newc < 2; newc++) {
        Bytes a;
        EntryFn entry = newc ? newc_entry : odc_entry;
        entry(a, "./a", 0100644, 1, "hello", 5);
        entry(a, "./empty", 0100644, 1, nullptr, 0);
        entry(a, "./b", 0100644, 1, "xyz", 3);
        entry(a, "TRAILER!!!", 0, 0, nullptr, 0);
        for (size_t step = 0; step <= 1; step++) {
            Error error;
            std::string log;
            CHECK(read_cpio(a.data(), a.size(), step, nullptr, error, &log));
            CHECK_NO_ERROR(error);
            CHECK(log == "file ./a 5\nhello|end\nfile ./empty 0\n|end\nfile ./b 3\nxyz|end\n");
        }
    }
}

void test_cpio_after_trailer()
{
    /* UP-11: done at TRAILER!!!; whatever follows is ignored. */
    Bytes a(CPIO_ODC, CPIO_ODC + sizeof CPIO_ODC);
    const char *junk = "not an entry at all";
    a.insert(a.end(), junk, junk + 19);
    CpioLog c;
    CpioReader r(cpio_log(c));
    Error error;
    bool done = true;
    CHECK(r.feed(a.data(), sizeof CPIO_ODC - 1, &done, &error));
    CHECK(!done);
    CHECK(r.feed(a.data() + sizeof CPIO_ODC - 1, a.size() - sizeof CPIO_ODC + 1, &done, &error));
    CHECK_NO_ERROR(error);
    CHECK(done);
    CHECK(r.feed(reinterpret_cast<const uint8_t *>("more"), 4, &done, &error));
    CHECK(done);
    CHECK(r.finish(&error));
    CHECK_NO_ERROR(error);
}

void test_cpio_refused()
{
    static const struct {
        const char *name;
        unsigned mode, nlink;
        size_t size;
        ErrorCode code;
    } cases[] = {
        { "/etc/passwd", 0100644, 1, 1, REFUSED },
        { "../x", 0100644, 1, 1, REFUSED },
        { "./../x", 0100644, 1, 1, REFUSED },
        { "./a/../../x", 0100644, 1, 1, REFUSED },
        { "./a/..", 040755, 2, 0, REFUSED },
        { "..", 040755, 2, 0, REFUSED },
        { "./link", 0120777, 1, 1, REFUSED },     /* a symlink */
        { "./hard", 0100644, 2, 1, REFUSED },     /* a hard-linked file */
        { "./tty", 020644, 1, 0, REFUSED },       /* a character device */
        { "./disk", 060644, 1, 0, REFUSED },      /* a block device */
        { "./fifo", 010644, 1, 0, REFUSED },
        { "./socket", 0140755, 1, 0, REFUSED },
        { "./dir", 040755, 2, 1, INVALID },       /* a directory with data */
        { "", 0100644, 1, 1, INVALID },           /* no name */
        { "./\xff", 0100644, 1, 1, INVALID },     /* not UTF-8 */
    };
    for (const auto &k : cases)
        for (int newc = 0; newc < 2; newc++) {
            Bytes a;
            EntryFn entry = newc ? newc_entry : odc_entry;
            entry(a, ".", 040755, 2, nullptr, 0);
            entry(a, k.name, k.mode, k.nlink, "x", k.size);
            entry(a, "TRAILER!!!", 0, 0, nullptr, 0);
            Error error;
            CHECK(!read_cpio(a.data(), a.size(), 0, nullptr, error));
            CHECK_FAILED(error, k.code);
        }

    /* The caller refuses an entry at its header. */
    Error error;
    CHECK(!read_cpio(CPIO_ODC, sizeof CPIO_ODC, 1, "./hello.txt", error));
    CHECK(error.set && error.code == REFUSED && error.message == "Not asked for");
}

void test_cpio_malformed()
{
    Error error;
    static const struct {
        unsigned at;
        uint8_t c;
    } edits[] = {
        { 0, '1' },          /* not a magic */
        { 5, '2' },          /* 070702, newc with checksums */
        { 20, '9' },         /* not an octal digit */
        { 59 + 5, '0' },     /* namesize 0 */
        { 76 + 1, 'x' },     /* "." without its NUL */
    };
    for (const auto &e : edits) {
        Bytes a(CPIO_ODC, CPIO_ODC + sizeof CPIO_ODC);
        a[e.at] = e.c;
        CHECK(!read_cpio(a.data(), a.size(), 0, nullptr, error));
        CHECK_FAILED(error, INVALID);
    }

    /* A name with a NUL inside. */
    Bytes b;
    odc_entry(b, "./ab", 0100644, 1, "x", 1);
    b[76 + 2] = 0;
    odc_entry(b, "TRAILER!!!", 0, 0, nullptr, 0);
    CHECK(!read_cpio(b.data(), b.size(), 0, nullptr, error));
    CHECK_FAILED(error, INVALID);
    b.clear();

    /* The format changing midway. */
    odc_entry(b, "./a", 0100644, 1, "x", 1);
    newc_entry(b, "./b", 0100644, 1, "y", 1);
    odc_entry(b, "TRAILER!!!", 0, 0, nullptr, 0);
    CHECK(!read_cpio(b.data(), b.size(), 0, nullptr, error));
    CHECK_FAILED(error, INVALID);

    /* Cut short anywhere: the reader waits, then finishing fails. */
    for (size_t n = 0; n < sizeof CPIO_ODC; n++) {
        CHECK(!read_cpio(CPIO_ODC, n, 1, nullptr, error));
        CHECK_FAILED(error, INVALID);
    }
    for (size_t n = 0; n < sizeof CPIO_NEWC; n += 3) {
        CHECK(!read_cpio(CPIO_NEWC, n, 0, nullptr, error));
        CHECK_FAILED(error, INVALID);
    }

    /* A reader that failed stays failed. */
    CpioReader r;
    bool done = false;
    CHECK(!r.feed(reinterpret_cast<const uint8_t *>("123456"), 6, &done, &error));
    CHECK_FAILED(error, INVALID);
    CHECK(!r.feed(CPIO_ODC, sizeof CPIO_ODC, &done, &error));
    CHECK_FAILED(error, INVALID);
    CHECK(!done);
    CHECK(!r.finish(&error));
    CHECK_FAILED(error, INVALID);
}

void test_cpio_limits()
{
    Error error;
    /* Names up to CPIO_MAX_NAME bytes with their NUL, no longer. */
    for (size_t n = CPIO_MAX_NAME - 1; n <= CPIO_MAX_NAME; n++) {
        Bytes a;
        std::string name(n, 'a');
        name[0] = '.';
        name[1] = '/';
        odc_entry(a, name, 0100644, 1, nullptr, 0);
        odc_entry(a, "TRAILER!!!", 0, 0, nullptr, 0);
        bool ok = read_cpio(a.data(), a.size(), 0, nullptr, error);
        if (n + 1 <= CPIO_MAX_NAME) {
            CHECK(ok);
            CHECK_NO_ERROR(error);
        } else {
            CHECK(!ok);
            CHECK_FAILED(error, LIMIT);
        }
    }
    /* A name of 0777777 bytes is refused at its header, before it arrives. */
    {
        Bytes a;
        odc_entry(a, "./a", 0100644, 1, nullptr, 0);
        std::memcpy(a.data() + 59, "777777", 6);
        CpioReader r;
        bool done = false;
        CHECK(!r.feed(a.data(), 76, &done, &error));
        CHECK_FAILED(error, LIMIT);
    }

    /* CPIO_MAX_ENTRIES entries, the root among them, and no more. */
    for (unsigned n = CPIO_MAX_ENTRIES; n <= CPIO_MAX_ENTRIES + 1; n++) {
        Bytes a;
        odc_entry(a, ".", 040755, 2, nullptr, 0);
        for (unsigned i = 1; i < n; i++) odc_entry(a, "./" + std::to_string(i), 0100644, 1, nullptr, 0);
        odc_entry(a, "TRAILER!!!", 0, 0, nullptr, 0);
        bool ok = read_cpio(a.data(), a.size(), 0, nullptr, error);
        if (n <= CPIO_MAX_ENTRIES) {
            CHECK(ok);
            CHECK_NO_ERROR(error);
        } else {
            CHECK(!ok);
            CHECK_FAILED(error, LIMIT);
        }
    }
}

void test_cpio_writer()
{
    Bytes out;
    CpioWriter w(collect(out));
    Error error;
    CHECK(w.dir(".", &error));
    CHECK(w.file("./hello.txt", 12, MTIME, &error));
    CHECK(w.file_data(reinterpret_cast<const uint8_t *>("hello "), 6, &error));
    CHECK(w.file_data(reinterpret_cast<const uint8_t *>("world\n"), 6, &error));
    CHECK(w.file("./" CAFE_JPG, 0, -5, &error));
    CHECK(w.finish(&error));
    CHECK_NO_ERROR(error);
    if (out.size() < 78) {
        CHECK(out.size() >= 78);
        return;
    }

    /* The directory: dated now, so its mtime is checked apart. */
    static const char dir[] = "070707" "000000" "000001" "040755" "000765" "000024" "000002" "000000";
    CHECK(std::memcmp(out.data(), dir, 48) == 0);
    std::string digits(reinterpret_cast<const char *>(out.data()) + 48, 11);
    long long mtime = std::strtoll(digits.c_str(), nullptr, 8);
    CHECK(std::llabs(mtime - static_cast<long long>(std::time(nullptr))) < 60);
    static const char dir_tail[] = "000002" "00000000000" ".";
    CHECK(std::memcmp(out.data() + 59, dir_tail, 19) == 0);

    /* Then the rest, byte for byte: POSIX odc as Finder writes it. */
    static const char rest[] =
        "070707" "000000" "000002" "100644" "000765" "000024" "000001" "000000" "14524770400" "000014" "00000000014"
        "./hello.txt\0" "hello world\n"
        "070707" "000000" "000003" "100644" "000765" "000024" "000001" "000000" "00000000000" "000020" "00000000000"
        "./" CAFE_JPG "\0"
        "070707" "000000" "000000" "000000" "000000" "000000" "000000" "000000" "00000000000" "000013" "00000000000"
        "TRAILER!!!";   /* with the literal's own NUL */
    CHECK(out.size() - 78 == sizeof rest && std::memcmp(out.data() + 78, rest, sizeof rest) == 0);

    /* And the reader takes it back. */
    std::string log;
    CHECK(read_cpio(out.data(), out.size(), 1, nullptr, error, &log));
    CHECK_NO_ERROR(error);
    CHECK(log == "dir . 0\n|end\nfile ./hello.txt 12\nhello world\n|end\nfile ./" CAFE_JPG " 0\n|end\n");
}

void test_cpio_writer_errors()
{
    Bytes out;
    CpioWriter w(collect(out));
    Error error;
    static const char *const refused[] = { "", "/etc/passwd", "../x", "./a/../../x", "..", "TRAILER!!!" };
    for (const char *path : refused) {
        CHECK(!w.file(path, 1, 0, &error));
        CHECK_FAILED(error, REFUSED);
        CHECK(!w.dir(path, &error));
        CHECK_FAILED(error, REFUSED);
    }
    /* A NUL inside, which only a std::string holds. */
    CHECK(!w.file(std::string("./a\0b", 5), 1, 0, &error));
    CHECK_FAILED(error, REFUSED);
    CHECK(!w.dir(std::string(CPIO_MAX_NAME, 'a'), &error));
    CHECK_FAILED(error, LIMIT);
    CHECK(!w.file("./big", 077777777777ull + 1, 0, &error));
    CHECK_FAILED(error, LIMIT);
    CHECK(out.empty());

    /* A file's data must be exactly its size. */
    CHECK(w.file("./a", 3, 0, &error));
    CHECK(!w.file_data(reinterpret_cast<const uint8_t *>("abcd"), 4, &error));
    CHECK_FAILED(error, INVALID);
    CHECK(w.file_data(reinterpret_cast<const uint8_t *>("ab"), 2, &error));
    CHECK(!w.dir("./d", &error));
    CHECK_FAILED(error, INVALID);
    CHECK(!w.finish(&error));
    CHECK_FAILED(error, INVALID);
    CHECK(w.file_data(reinterpret_cast<const uint8_t *>("c"), 1, &error));
    CHECK(w.finish(&error));
    CHECK(!w.dir("./d", &error));
    CHECK_FAILED(error, INVALID);

    /* The sink's error comes back as it is. */
    CpioWriter full(disk_full);
    CHECK(!full.dir(".", &error));
    CHECK(error.set && error.code == ErrorCode::Failed && error.message == "No space left");
    error.clear();
    CHECK(!full.finish(&error));
    CHECK_FAILED(error, INVALID);
}

/* ------------------------------------------------------------ gzip and DVZip */

/* Compresses `len` bytes fed in pieces of `step`. */
Bytes deflate_all(bool dvzip, const uint8_t *data, size_t len, size_t step)
{
    Bytes out;
    Deflater d(dvzip, collect(out));
    Error error;
    for (size_t pos = 0; pos < len; pos += step) CHECK(d.feed(data + pos, std::min(step, len - pos), &error));
    CHECK(d.finish(&error));
    CHECK_NO_ERROR(error);
    return out;
}

/* Decompresses a body fed in pieces of `step` (0: at once): true with the
 * output, or false with the error. */
bool inflate_all(bool dvzip, uint64_t max, const uint8_t *body, size_t len, size_t step, Error &error,
                 Bytes *out = nullptr)
{
    Bytes got;
    Inflater z(dvzip, max, collect(got));
    bool ok = true;
    for (size_t pos = 0; ok && pos < len; pos += step ? step : len)
        ok = z.feed(body + pos, std::min(step ? step : len, len - pos), &error);
    ok = ok && z.finish(&error);
    if (ok && out) *out = std::move(got);
    return ok;
}

/* A DVZip body's chunks, each checked to be one 64 KiB piece at most. */
unsigned dvzip_chunks(const Bytes &body, unsigned &stored)
{
    unsigned n = 0;
    stored = 0;
    for (size_t pos = 0; pos < body.size(); n++) {
        if (body.size() - pos < 4) {
            CHECK(body.size() - pos >= 4);
            break;
        }
        uint32_t raw = static_cast<uint32_t>(body[pos]) << 24 | static_cast<uint32_t>(body[pos + 1]) << 16
                       | static_cast<uint32_t>(body[pos + 2]) << 8 | body[pos + 3];
        uint32_t len = raw & 0x7fffffff;
        CHECK(len > 0);
        CHECK(len <= 65536);
        stored += raw >> 31;
        pos += 4 + len;
        CHECK(pos <= body.size());
    }
    return n;
}

void test_compress_round_trip()
{
    Bytes t = text(200000), z = noise(150000);
    Bytes mixed(t.begin(), t.begin() + 65536);
    mixed.insert(mixed.end(), z.begin(), z.begin() + 131072);
    mixed.insert(mixed.end(), t.begin(), t.begin() + 2600);
    static const struct {
        const char *name;
        unsigned chunks, stored;
    } want[] = {
        { "text", 4, 0 }, { "noise", 3, 3 }, { "mixed", 4, 2 }, { "hello", 1, 1 }, { "empty", 0, 0 },
    };
    const uint8_t *inputs[] = { t.data(), z.data(), mixed.data(), reinterpret_cast<const uint8_t *>("hello"),
                                reinterpret_cast<const uint8_t *>("") };
    const size_t lens[] = { 200000, 150000, mixed.size(), 5, 0 };
    for (int dvzip = 0; dvzip < 2; dvzip++)
        for (size_t i = 0; i < 5; i++) {
            Bytes body = deflate_all(dvzip != 0, inputs[i], lens[i], 7777);
            if (dvzip) {
                unsigned stored = 0, chunks = dvzip_chunks(body, stored);
                CHECK(chunks == want[i].chunks);
                CHECK(stored == want[i].stored);
            } else {
                CHECK(body.size() >= 18);
                CHECK(body.size() >= 3 && std::memcmp(body.data(), "\x1f\x8b\x08", 3) == 0);
                /* The OS byte Linux's zlib writes, Unix. */
                CHECK(body.size() >= 10 && body[9] == 3);
            }
            if (i == 0) CHECK(body.size() < 200000 / 4);
            for (size_t step = 0; step <= (lens[i] <= 150000 ? 1u : 0u); step++) {
                Error error;
                Bytes back;
                CHECK(inflate_all(dvzip != 0, lens[i], body.data(), body.size(), step, error, &back));
                CHECK_NO_ERROR(error);
                CHECK(same(back, inputs[i], lens[i]));
            }
        }

    /* A stored chunk by hand, as an iPhone sends a photo's bytes. */
    Error error;
    Bytes back;
    CHECK(inflate_all(true, 100, reinterpret_cast<const uint8_t *>("\x80\x00\x00\x05hello\x80\x00\x00\x01!"), 14, 1,
                      error, &back));
    CHECK_NO_ERROR(error);
    CHECK(same(back, "hello!", 6));
}

/* gzip members back to back decode as one stream, within one output limit. */
void test_gzip_members()
{
    Bytes t = text(100000);
    Bytes body;
    const size_t cuts[] = { 0, 6, 6, 70000, 100000 };   /* an empty member among them */
    for (size_t i = 0; i + 1 < 5; i++) {
        Bytes member = deflate_all(false, t.data() + cuts[i], cuts[i + 1] - cuts[i], 4096);
        body.insert(body.end(), member.begin(), member.end());
    }
    for (size_t step = 0; step <= 1; step++) {
        Error error;
        Bytes back;
        CHECK(inflate_all(false, 100000, body.data(), body.size(), step, error, &back));
        CHECK_NO_ERROR(error);
        CHECK(back == t);
        CHECK(!inflate_all(false, 99999, body.data(), body.size(), step, error));
        CHECK_FAILED(error, LIMIT);
        /* A second member cut short. */
        CHECK(!inflate_all(false, 100000, body.data(), body.size() - 1, step, error));
        CHECK_FAILED(error, INVALID);
    }
}

/* One zlib stream of `len` zero bytes, at zlib's best. */
Bytes zlib_zeros(size_t len)
{
    Bytes zeros(len, 0), out;
    z_stream s{};
    CHECK(deflateInit(&s, 9) == Z_OK);
    s.next_in = zeros.data();
    s.avail_in = static_cast<uInt>(len);
    uint8_t buf[65536];
    int res;
    do {
        s.next_out = buf;
        s.avail_out = sizeof buf;
        res = deflate(&s, Z_FINISH);
        out.insert(out.end(), buf, buf + (sizeof buf - s.avail_out));
    } while (res == Z_OK);
    CHECK(res == Z_STREAM_END);
    deflateEnd(&s);
    return out;
}

void test_compress_bombs()
{
    /* SEC-3: 8 MiB of zeros from a few kilobytes fails cleanly past the
     * limit, and passes at it. */
    Bytes zeros(8 << 20, 0);
    for (int dvzip = 0; dvzip < 2; dvzip++) {
        Bytes body = deflate_all(dvzip != 0, zeros.data(), zeros.size(), 1 << 20);
        CHECK(body.size() < (64 << 10));
        Error error;
        CHECK(!inflate_all(dvzip != 0, 1 << 20, body.data(), body.size(), 0, error));
        CHECK_FAILED(error, LIMIT);
        Bytes back;
        CHECK(inflate_all(dvzip != 0, 8 << 20, body.data(), body.size(), 0, error, &back));
        CHECK_NO_ERROR(error);
        CHECK(back.size() == (8 << 20));
    }

    /* DVZ-3: one chunk may inflate to far more than 64 KiB. */
    Bytes stream = zlib_zeros(4 << 20);
    Bytes body;
    put_be(body, stream.size(), 4);
    body.insert(body.end(), stream.begin(), stream.end());
    Error error;
    Bytes back;
    CHECK(inflate_all(true, 4 << 20, body.data(), body.size(), 0, error, &back));
    CHECK_NO_ERROR(error);
    CHECK(back.size() == (4 << 20));
    CHECK(!inflate_all(true, 1 << 20, body.data(), body.size(), 0, error));
    CHECK_FAILED(error, LIMIT);
}

void test_compress_malformed()
{
    Error error;
    Bytes stream = zlib_zeros(5);
    /* DVZ-3: no empty chunk, stored or not; none past the limit. */
    static const struct {
        const char *bytes;
        size_t len;
        ErrorCode code;
    } chunks[] = {
        { "\x00\x00\x00\x00", 4, INVALID },
        { "\x80\x00\x00\x00", 4, INVALID },
        { "\x00\x40\x00\x01", 4, LIMIT },
        { "\x80\x40\x00\x01", 4, LIMIT },
        { "\x00\x00\x00\x03" "abc", 7, INVALID },                          /* not zlib */
        { "\x00\x00\x00\x07\xcb\x48\xcd\xc9\xc9\x07\x00", 11, INVALID },   /* raw deflate */
    };
    for (const auto &c : chunks) {
        CHECK(!inflate_all(true, 1 << 20, reinterpret_cast<const uint8_t *>(c.bytes), c.len, 0, error));
        CHECK_FAILED(error, c.code);
    }
    /* A chunk with a byte after its zlib stream, and one too short for it. */
    Bytes b;
    put_be(b, stream.size() + 1, 4);
    b.insert(b.end(), stream.begin(), stream.end());
    b.push_back(0);
    for (size_t step = 0; step <= 1; step++) {
        CHECK(!inflate_all(true, 1 << 20, b.data(), b.size(), step, error));
        CHECK_FAILED(error, INVALID);
    }
    b.clear();
    put_be(b, stream.size() - 1, 4);
    b.insert(b.end(), stream.begin(), stream.end() - 1);
    const char *stored = "\x80\x00\x00\x01!";
    b.insert(b.end(), stored, stored + 5);
    CHECK(!inflate_all(true, 1 << 20, b.data(), b.size(), 0, error));
    CHECK_FAILED(error, INVALID);

    /* Truncated anywhere: in a length, a stored chunk or a zlib one. */
    Bytes t = text(70000);
    Bytes body = deflate_all(true, t.data(), 70000, 70000);
    for (size_t n = 1; n < body.size(); n += (n < 8 || body.size() - n < 8) ? 1 : 97) {
        CHECK(!inflate_all(true, 1 << 20, body.data(), n, 0, error));
        CHECK_FAILED(error, INVALID);
    }
    body = deflate_all(false, t.data(), 70000, 70000);
    for (size_t n = 0; n < body.size(); n += (n < 20 || body.size() - n < 20) ? 1 : 97) {
        CHECK(!inflate_all(false, 1 << 20, body.data(), n, 0, error));
        CHECK_FAILED(error, INVALID);
    }

    /* gzip: after a member, only another member; its checksum right. */
    static const char *const garbage[] = { "x", "not a gzip member", "\x1f\x8b\x08" };
    for (const char *g : garbage) {
        size_t whole = body.size();
        body.insert(body.end(), g, g + std::strlen(g));
        for (size_t step = 0; step <= 1; step++) {
            CHECK(!inflate_all(false, 1 << 20, body.data(), body.size(), step, error));
            CHECK_FAILED(error, INVALID);
        }
        body.resize(whole);
    }
    body[body.size() - 8] ^= 1;
    CHECK(!inflate_all(false, 1 << 20, body.data(), body.size(), 0, error));
    CHECK_FAILED(error, INVALID);
    CHECK(!inflate_all(false, 1 << 20, reinterpret_cast<const uint8_t *>("PK\x03\x04"), 4, 0, error));
    CHECK_FAILED(error, INVALID);

    /* A failed inflater stays failed; a sink's error comes back as it is. */
    {
        Bytes out;
        Inflater z(true, 100, collect(out));
        CHECK(!z.feed(reinterpret_cast<const uint8_t *>("\0\0\0\0"), 4, &error));
        CHECK_FAILED(error, INVALID);
        CHECK(!z.feed(reinterpret_cast<const uint8_t *>("\x80\0\0\x01x"), 5, &error));
        CHECK_FAILED(error, INVALID);
        CHECK(!z.finish(&error));
        CHECK_FAILED(error, INVALID);
    }
    {
        Inflater z(true, 100, disk_full);
        CHECK(!z.feed(reinterpret_cast<const uint8_t *>("\x80\0\0\x01x"), 5, &error));
        CHECK(error.set && error.code == ErrorCode::Failed && error.message == "No space left");
        error.clear();
    }
    for (int dvzip = 0; dvzip < 2; dvzip++) {
        Deflater d(dvzip != 0, disk_full);
        bool ok = d.feed(reinterpret_cast<const uint8_t *>("hello"), 5, &error) && d.finish(&error);
        CHECK(!ok);
        CHECK(error.set && error.code == ErrorCode::Failed && error.message == "No space left");
        error.clear();
        CHECK(!d.feed(reinterpret_cast<const uint8_t *>("x"), 1, &error));
        CHECK_FAILED(error, INVALID);
    }
}

/* ------------------------------------------------------------ HTTP */

struct HttpLog {
    std::string log;       /* a head's line, its body and "|done" */
    HttpHeaders headers;   /* the last head's, copied */
    bool refuse = false;   /* refuse heads without saying why */
};

HttpCallbacks http_log(HttpLog &h)
{
    HttpCallbacks cb;
    cb.head = [&h](const std::string &method, const std::string &path, unsigned status, const HttpHeaders &headers,
                   Error &) {
        if (!method.empty()) h.log += method + " " + path + "\n";
        else h.log += std::to_string(status) + "\n";
        h.headers = headers;
        return !h.refuse;
    };
    cb.body = [&h](const uint8_t *buf, size_t len, Error &) {
        CHECK(len > 0);
        h.log.append(reinterpret_cast<const char *>(buf), len);
        return true;
    };
    cb.done = [&h](Error &) {
        h.log += "|done\n";
        return true;
    };
    return cb;
}

/* Feeds `data` in pieces of `step` (0: at once), resetting after each
 * message as a connection does, then closes the connection. */
bool http_run(HttpParser &p, const std::string &data, size_t step, bool eof, Error &error)
{
    for (size_t pos = 0; pos < data.size();) {
        size_t n = step ? std::min(step, data.size() - pos) : data.size() - pos, used = 0;
        if (!p.feed(reinterpret_cast<const uint8_t *>(data.data()) + pos, n, &used, &error)) return false;
        pos += used;
        /* A feed stops short only at a message's end, or takes nothing
         * once one has ended: either way, the next message starts here. */
        if (used < n) p.reset();
    }
    return !eof || p.eof(&error);
}

/* Parses `data` as one kind, at once and a byte at a time: true with the
 * log of both, which must agree, or false with the first error. */
bool http_parse(HttpKind kind, const std::string &data, bool eof, Error &error, std::string *log = nullptr)
{
    std::string logs[2];
    bool oks[2];
    Error errors[2];
    for (int i = 0; i < 2; i++) {
        HttpLog h;
        HttpParser p(kind, http_log(h));
        oks[i] = http_run(p, data, static_cast<size_t>(i), eof, errors[i]);
        if (oks[i]) logs[i] = h.log;
    }
    CHECK(oks[0] == oks[1]);
    CHECK(logs[0] == logs[1]);
    if (!oks[0] && !oks[1]) CHECK(errors[0].code == errors[1].code);
    if (!oks[0]) error = errors[0];
    if (log) *log = logs[0];
    return oks[0];
}

void test_http_requests()
{
    Error error;
    std::string log;
    /* HTTP-2: Finder's /Ask head, a chunked body with an extension and a
     * trailer, both dropped. */
    const std::string ask =
        "POST /Ask HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nContent-Type: application/octet-stream\r\n"
        "Connection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n";
    CHECK(http_parse(HttpKind::Request, ask, false, error, &log));
    CHECK_NO_ERROR(error);
    CHECK(log == "POST /Ask\nhello world|done\n");

    /* Names come lowercase, values trimmed; a repeated field is joined. */
    {
        HttpLog h;
        HttpParser p(HttpKind::Request, http_log(h));
        const std::string discover =
            "POST /Discover HTTP/1.1\r\nContent-Length: 5\r\nExpect:  100-continue \r\nAccept: a\r\nACCEPT: b\r\n"
            "X-Empty:\r\n\r\nhello";
        CHECK(http_run(p, discover, 0, false, error));
        CHECK(h.log == "POST /Discover\nhello|done\n");
        CHECK(h.headers["content-length"] == "5");
        CHECK(h.headers["expect"] == "100-continue");
        CHECK(h.headers["accept"] == "a, b");
        CHECK(h.headers.count("x-empty") == 1 && h.headers["x-empty"].empty());
        CHECK(h.headers.count("Content-Length") == 0);
    }

    /* Keep-alive: two requests in one buffer. The first feed stops at the
     * first one's end; after a reset, the rest is the second. */
    const std::string two =
        "POST /Discover HTTP/1.1\r\nContent-Length: 2\r\n\r\nab"
        "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nxyz\r\n0\r\n\r\n"
        "GET / HTTP/1.1\r\n\r\n";
    {
        HttpLog h;
        HttpParser p(HttpKind::Request, http_log(h));
        size_t used = 0;
        const auto *d = reinterpret_cast<const uint8_t *>(two.data());
        CHECK(p.feed(d, two.size(), &used, &error));
        CHECK(used == std::strlen("POST /Discover HTTP/1.1\r\nContent-Length: 2\r\n\r\nab"));
        CHECK(h.log == "POST /Discover\nab|done\n");
        /* Done until reset. */
        size_t again = 1;
        CHECK(p.feed(d + used, two.size() - used, &again, &error));
        CHECK(again == 0);
    }
    CHECK(http_parse(HttpKind::Request, two, true, error, &log));
    CHECK_NO_ERROR(error);
    CHECK(log == "POST /Discover\nab|done\nPOST /Ask\nxyz|done\nGET /\n|done\n");

    /* A head of exactly HTTP_MAX_HEAD bytes is taken. */
    std::string s = "POST / HTTP/1.1\r\nX: ";
    while (s.size() < HTTP_MAX_HEAD - 4) s += 'a';
    s += "\r\n\r\n";
    CHECK(s.size() == HTTP_MAX_HEAD);
    CHECK(http_parse(HttpKind::Request, s, false, error, &log));
    CHECK_NO_ERROR(error);
    CHECK(log == "POST /\n|done\n");
    s.insert(20, 1, 'a');
    CHECK(!http_parse(HttpKind::Request, s, false, error));
    CHECK_FAILED(error, LIMIT);
}

void test_http_responses()
{
    Error error;
    static const struct {
        const char *in, *log;
        bool eof;
    } cases[] = {
        { "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "200\nabc|done\n", false },
        /* Neither length nor chunked: the body runs to the connection's end. */
        { "HTTP/1.1 403\r\n\r\nno, thanks", "403\nno, thanks|done\n", true },
        { "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", "200\n|done\n", true },
        /* UP-3: an interim 100 Continue has no body, even without a
         * length, and the final response follows it. */
        { "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", "100\n|done\n200\nok|done\n",
          false },
        { "HTTP/1.1 100 Continue\r\nContent-Length: 9\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          "2\r\nok\r\n0\r\n\r\n",
          "100\n|done\n200\nok|done\n", false },
        { "HTTP/1.1 204 No Content\r\n\r\nHTTP/1.1 304 Not Modified\r\n\r\nHTTP/1.1 200 OK\r\n\r\nend",
          "204\n|done\n304\n|done\n200\nend|done\n", true },
    };
    for (const auto &c : cases) {
        std::string log;
        CHECK(http_parse(HttpKind::Response, c.in, c.eof, error, &log));
        CHECK_NO_ERROR(error);
        CHECK(log == c.log);
    }

    /* The connection closing: fine between messages and after one that
     * runs to the end, not inside one. */
    static const char *const cut[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", "HTTP/1.1 200 OK\r\nContent-Le",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n",
    };
    for (const char *c : cut) {
        CHECK(!http_parse(HttpKind::Response, c, true, error));
        CHECK_FAILED(error, INVALID);
    }
    HttpParser p(HttpKind::Response);
    CHECK(p.eof(&error));
    CHECK_NO_ERROR(error);
}

void test_http_rejected()
{
    /* SEC-7, after owly's cases. */
    static const char *const requests[] = {
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 1, 1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: +1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length : 1\r\n\r\n",
        "POST / HTTP/1.1\r\nX: a\r\n b\r\n\r\n",          /* obs-fold */
        "POST / HTTP/1.1\r\n\tX: a\r\n\r\n",
        "POST / HTTP/1.1\nContent-Length: 1\r\n\r\n",    /* a bare LF */
        "POST / HTTP/1.1\r\nX: a\n\r\n",
        "POST / HTTP/1.1\n\n",
        "POST / HTTP/1.1\r\nX: a\rb\r\n\r\n",           /* a bare CR */
        "POST / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n",
        "POST / HTTP/1.1\r\nX: \xff\r\n\r\n",           /* not UTF-8 */
        "POST / HTTP/1.1\r\nNo colon\r\n\r\n",
        "POST / HTTP/1.1\r\n: empty name\r\n\r\n",
        "POST  / HTTP/1.1\r\n\r\n",
        "POST / HTTP/1.1 \r\n\r\n",
        "POST /\x7f HTTP/1.1\r\n\r\n",
        "POST / HTTP/1.0\r\n\r\n",
        "POST http://x/ HTTP/1.1\r\n\r\n",
        "PO(ST / HTTP/1.1\r\n\r\n",
        "\r\nPOST / HTTP/1.1\r\n\r\n",
        /* Chunks: hex sizes only, 16 digits at most, CRLF after the data. */
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nx\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n 5\r\nhello\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5 x\r\nhello\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0x5\r\nhello\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5;a\x01\r\nhello\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n11111111111111111\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nbad trailer\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n folded: x\r\n\r\n",
    };
    for (const char *r : requests) {
        Error error;
        CHECK(!http_parse(HttpKind::Request, r, false, error));
        CHECK_FAILED(error, INVALID);
    }
    static const char *const responses[] = {
        "HTTP/1.0 200 OK\r\n\r\n", "HTTP/1.1 600 Odd\r\n\r\n", "HTTP/1.1 099 Odd\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 200OK\r\n\r\n", "HTTP/1.1  200 OK\r\n\r\n", "HTTP/1.1 2x0 OK\r\n\r\n", "http/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
    };
    for (const char *r : responses) {
        Error error;
        CHECK(!http_parse(HttpKind::Response, r, false, error));
        CHECK_FAILED(error, INVALID);
    }

    /* Limits: the head, the number of fields, a chunk line, a trailer. */
    Error error;
    std::string s = "POST / HTTP/1.1\r\nX: " + std::string(20000, 'a');
    CHECK(!http_parse(HttpKind::Request, s, false, error));   /* no end in sight */
    CHECK_FAILED(error, LIMIT);
    s += "\r\n\r\n";
    CHECK(!http_parse(HttpKind::Request, s, false, error));
    CHECK_FAILED(error, LIMIT);
    s = "POST / HTTP/1.1\r\n";
    for (unsigned i = 0; i < 65; i++) s += "X" + std::to_string(i) + ": a\r\n";
    s += "\r\n";
    CHECK(!http_parse(HttpKind::Request, s, false, error));
    CHECK_FAILED(error, LIMIT);
    s = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5;" + std::string(2000, 'e') + "\r\nhello\r\n0\r\n\r\n";
    CHECK(!http_parse(HttpKind::Request, s, false, error));
    CHECK_FAILED(error, LIMIT);
    s = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
    for (unsigned i = 0; i < 20; i++) {
        std::string digits = std::to_string(i);
        s += "X" + digits + ": " + std::string(1000 - digits.size(), '0') + digits + "\r\n";
    }
    s += "\r\n";
    CHECK(!http_parse(HttpKind::Request, s, false, error));
    CHECK_FAILED(error, LIMIT);
    /* Extensions within the line's limit are fine. */
    s = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0000000000000005 ;a=b;c=\"d\"\r\nhello\r\n0\r\n\r\n";
    std::string log;
    CHECK(http_parse(HttpKind::Request, s, false, error, &log));
    CHECK_NO_ERROR(error);
    CHECK(log == "POST /\nhello|done\n");

    /* A head refused by the caller, with no reason given; a failed parser
     * stays failed. */
    HttpLog h;
    h.refuse = true;
    HttpParser p(HttpKind::Request, http_log(h));
    size_t used = 0;
    CHECK(!p.feed(reinterpret_cast<const uint8_t *>("GET / HTTP/1.1\r\n\r\n"), 18, &used, &error));
    CHECK(error.set && error.code == REFUSED && error.message == "HTTP message refused");
    error.clear();
    CHECK(!p.feed(reinterpret_cast<const uint8_t *>("GET"), 3, &used, &error));
    CHECK_FAILED(error, INVALID);
    CHECK(!p.eof(&error));
    CHECK_FAILED(error, INVALID);
    /* A reset starts over. */
    h.refuse = false;
    p.reset();
    CHECK(p.feed(reinterpret_cast<const uint8_t *>("GET / HTTP/1.1\r\n\r\n"), 18, &used, &error));
    CHECK_NO_ERROR(error);
}

void test_http_chunk()
{
    Bytes b = http_chunk(reinterpret_cast<const uint8_t *>("hello"), 5);
    CHECK(same(b, "5\r\nhello\r\n", 10));
    b = http_chunk(nullptr, 0);
    CHECK(same(b, "0\r\n\r\n", 5));

    /* Chunks made here, read back by the parser. */
    Bytes t = text(70000);
    const std::string head = "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string s = head;
    for (const Bytes &part : { http_chunk(t.data(), 69999), http_chunk(t.data() + 69999, 1), http_chunk(nullptr, 0) })
        s.append(part.begin(), part.end());
    CHECK(s.compare(head.size(), 7, "1116f\r\n") == 0);
    Error error;
    std::string log;
    CHECK(http_parse(HttpKind::Request, s, false, error, &log));
    CHECK_NO_ERROR(error);
    CHECK(log.rfind("POST /Ask\nAirDrop vector line 000000\n", 0) == 0);
    CHECK(log.size() == std::strlen("POST /Ask\n") + 70000 + std::strlen("|done\n"));
    CHECK(log.size() >= 10 + 70000 && std::memcmp(log.data() + 10, t.data(), 70000) == 0);
}

/* ------------------------------------------------------------ an upload */

/* An /Upload as a receiver takes it: HTTP framing, then gzip or DVZip,
 * then the archive, each streaming into the next. */
struct Upload {
    std::unique_ptr<Inflater> z;
    std::unique_ptr<CpioReader> cpio;
    CpioLog log;
    std::string transfer_id;
    bool has_id = false;
    bool done = false;   /* the archive's trailer was read (UP-11) */
};

void test_upload_vectors()
{
    static const struct {
        const uint8_t *data;
        size_t len;
        const char *log, *id;
    } uploads[] = {
        { UPLOAD_FINDER, sizeof UPLOAD_FINDER, "dir . 0\n|end\nfile ./hello.txt 12\nhello world\n|end\n",
          "A3F34E01-0000-4000-8000-000000000001" },
        /* The legacy form: gzip, chunked, no TransferID and no root entry. */
        { UPLOAD_LEGACY, sizeof UPLOAD_LEGACY, "file ./hello.txt 12\nhello world\n|end\n", nullptr },
    };
    for (const auto &up : uploads)
        for (size_t s : { size_t{ 0 }, size_t{ 1 }, size_t{ 7 }, size_t{ 100 } }) {
            Upload u;
            u.cpio = std::make_unique<CpioReader>(cpio_log(u.log));
            HttpCallbacks cb;
            cb.head = [&u](const std::string &method, const std::string &path, unsigned, const HttpHeaders &headers,
                           Error &) {
                CHECK(method == "POST");
                CHECK(path == "/Upload");
                auto type = headers.find("content-type");
                bool dvzip = type != headers.end() && type->second == "application/x-dvzip";
                CHECK(dvzip || (type != headers.end() && type->second == "application/x-cpio"));
                auto id = headers.find("transferid");
                u.has_id = id != headers.end();
                if (u.has_id) u.transfer_id = id->second;
                u.z = std::make_unique<Inflater>(dvzip, 1 << 20, [&u](const uint8_t *buf, size_t len, Error &error) {
                    return u.cpio->feed(buf, len, &u.done, &error);
                });
                return true;
            };
            cb.body = [&u](const uint8_t *buf, size_t len, Error &error) { return u.z->feed(buf, len, &error); };
            cb.done = [&u](Error &error) { return u.z->finish(&error) && u.cpio->finish(&error); };
            HttpParser http(HttpKind::Request, cb);
            Error error;
            size_t step = s ? s : up.len;
            for (size_t pos = 0; pos < up.len; pos += step) {
                size_t used = 0;
                CHECK(http.feed(up.data + pos, std::min(step, up.len - pos), &used, &error));
                CHECK_NO_ERROR(error);
            }
            CHECK(u.done);
            CHECK(u.log.log == up.log);
            CHECK(up.id ? u.has_id && u.transfer_id == up.id : !u.has_id);
        }
}

/* ------------------------------------------------------------ mDNS: by hand */

struct Seen {
    std::vector<MdnsService> found;
    std::vector<std::string> lost;
    void clear()
    {
        found.clear();
        lost.clear();
    }
};

MdnsCallbacks callbacks(Seen &seen)
{
    MdnsCallbacks cb;
    cb.found = [&seen](const MdnsService &service) { seen.found.push_back(service); };
    cb.lost = [&seen](const std::string &instance) { seen.lost.push_back(instance); };
    return cb;
}

bool feed(MdnsCache &c, const Bytes &msg, int64_t now, Seen &seen)
{
    return c.feed(msg.data(), msg.size(), "awdl0", now, callbacks(seen));
}

void b16(Bytes &o, unsigned v)
{
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void b32(Bytes &o, uint32_t v)
{
    b16(o, v >> 16);
    b16(o, v & 0xffff);
}

void bbytes(Bytes &o, const char *s, size_t n)
{
    o.insert(o.end(), s, s + n);
}

/* A dotted name, uncompressed, with its final zero. */
void bname(Bytes &o, const std::string &dotted)
{
    for (size_t s = 0; s <= dotted.size();) {
        size_t dot = std::min(dotted.find('.', s), dotted.size());
        if (dot > s) {
            o.push_back(static_cast<uint8_t>(dot - s));
            o.insert(o.end(), dotted.begin() + static_cast<std::ptrdiff_t>(s),
                     dotted.begin() + static_cast<std::ptrdiff_t>(dot));
        }
        s = dot + 1;
    }
    o.push_back(0);
}

Bytes bheader(unsigned flags, unsigned qd, unsigned an, unsigned ns, unsigned ar)
{
    Bytes o;
    b16(o, 0);
    b16(o, flags);
    b16(o, qd);
    b16(o, an);
    b16(o, ns);
    b16(o, ar);
    return o;
}

/* A record's type, class and TTL after its name; returns where its data
 * length goes, for bend(). */
size_t bstart(Bytes &o, unsigned type, unsigned cls, uint32_t ttl)
{
    b16(o, type);
    b16(o, cls);
    b32(o, ttl);
    b16(o, 0);
    return o.size() - 2;
}

void bend(Bytes &o, size_t at)
{
    size_t n = o.size() - at - 2;
    o[at] = static_cast<uint8_t>(n >> 8);
    o[at + 1] = static_cast<uint8_t>(n);
}

const uint8_t FE80_1[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

const char *const INST = "3fa9c01b22de._airdrop._tcp.local";
const char *const HOST = "lares-3fa9c01b22de.local";

/* owly's test service's records, one at a time. */
void rec_ptr(Bytes &w, uint32_t ttl, unsigned cls)
{
    bname(w, "_airdrop._tcp.local");
    size_t at = bstart(w, 12, cls, ttl);
    bname(w, INST);
    bend(w, at);
}

void rec_srv(Bytes &w, uint32_t ttl, unsigned cls)
{
    bname(w, INST);
    size_t at = bstart(w, 33, cls, ttl);
    b16(w, 0);
    b16(w, 0);
    b16(w, 8770);
    bname(w, HOST);
    bend(w, at);
}

void rec_txt(Bytes &w, uint32_t ttl, unsigned cls)
{
    bname(w, INST);
    size_t at = bstart(w, 16, cls, ttl);
    bbytes(w, "\x09" "flags=137", 10);
    bend(w, at);
}

void rec_aaaa(Bytes &w, uint32_t ttl, unsigned cls)
{
    bname(w, HOST);
    size_t at = bstart(w, 28, cls, ttl);
    w.insert(w.end(), FE80_1, FE80_1 + 16);
    bend(w, at);
}

/* NSEC for the host: AAAA and NSEC, standing for no A (RFC 6762 §6.2). */
void rec_nsec_host(Bytes &w, uint32_t ttl, unsigned cls)
{
    bname(w, HOST);
    size_t at = bstart(w, 47, cls, ttl);
    bname(w, HOST);
    bbytes(w, "\x00\x06\x00\x00\x00\x08\x00\x01", 8);
    bend(w, at);
}

/* NSEC for the instance: TXT, SRV and NSEC. */
void rec_nsec_instance(Bytes &w, uint32_t ttl, unsigned cls)
{
    bname(w, INST);
    size_t at = bstart(w, 47, cls, ttl);
    bname(w, INST);
    bbytes(w, "\x00\x06\x00\x00\x80\x00\x40\x01", 8);
    bend(w, at);
}

/* The announcement of owly's test service, with each record's TTL. */
Bytes expected_announcement(uint32_t ptr_ttl, uint32_t srv_ttl, uint32_t txt_ttl, uint32_t aaaa_ttl)
{
    Bytes w = bheader(0x8400, 0, 4, 0, 0);
    rec_ptr(w, ptr_ttl, 0x0001);     /* PTR, shared */
    rec_srv(w, srv_ttl, 0x8001);     /* SRV, cache-flush */
    rec_txt(w, txt_ttl, 0x8001);     /* TXT */
    rec_aaaa(w, aaaa_ttl, 0x8001);   /* AAAA */
    return w;
}

/* ---- mDNS: building ---- */

void test_announcement_bytes()
{
    /* RFC 6762 §10: host records 120 s, others 75 minutes. */
    Bytes w = expected_announcement(4500, 120, 4500, 120);

    /* The host's three spellings, and an address with its zone, give the
     * same message. */
    const char *hosts[] = { "lares-3fa9c01b22de", "lares-3fa9c01b22de.local", "lares-3fa9c01b22de.local." };
    for (int i = 0; i < 3; i++) {
        Bytes a = mdns_build_announcement("3fa9c01b22de", hosts[i], i ? "fe80::1%awdl0" : "fe80::1", 8770, 0x089,
                                          UINT32_MAX);
        CHECK(!a.empty());
        CHECK(a == w);
    }

    /* The TTL argument caps each record's: 0 makes the goodbye. */
    CHECK(mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, 0)
          == expected_announcement(0, 0, 0, 0));
    CHECK(mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, 600)
          == expected_announcement(600, 120, 600, 120));
}

void test_announcement_round_trip()
{
    MdnsCache c;
    Seen seen;
    Bytes a = mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, UINT32_MAX);
    CHECK(feed(c, a, 0, seen));
    CHECK(seen.found.size() == 1);
    if (seen.found.size() == 1) {
        const MdnsService &s = seen.found[0];
        CHECK(s.instance == "3fa9c01b22de");
        CHECK(s.host == "lares-3fa9c01b22de.local.");
        CHECK(s.address == "fe80::1%awdl0");
        CHECK(s.port == 8770);
        CHECK(s.flags == 0x089);
    }

    /* The same again changes nothing; new flags are reported. */
    seen.clear();
    CHECK(feed(c, a, SEC, seen));
    CHECK(seen.found.empty());
    Bytes b = mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x08b, UINT32_MAX);
    CHECK(feed(c, b, 2 * SEC, seen));
    CHECK(seen.found.size() == 1 && seen.found[0].flags == 0x08b);
    CHECK(seen.lost.empty());
}

void test_build_rejects()
{
    const std::string long_label(64, 'a');
    CHECK(mdns_build_announcement(long_label, "host", "fe80::1", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("3fa9c01b22de", long_label, "fe80::1", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("3fa9c01b22de", "host", "192.0.2.1", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("3fa9c01b22de", "host", "not an address", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("", "host", "fe80::1", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("3fa9c01b22de", "", "fe80::1", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("3fa9c01b22de", "a..b", "fe80::1", 1, 0, 120).empty());
    const std::string max(63, 'b');
    CHECK(!mdns_build_announcement(max, max, "fe80::1", 1, 0, 120).empty());
    /* inet_pton()'s IPv6 forms, and a few it refuses. */
    CHECK(!mdns_build_announcement("a", "h", "::ffff:192.0.2.1", 1, 0, 120).empty());
    CHECK(!mdns_build_announcement("a", "h", "FE80:0:0:0:0:0:0:1%awdl0", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("a", "h", "fe80:::1", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("a", "h", "fe80::1::2", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("a", "h", "1:2:3:4:5:6:7:8:9", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("a", "h", "fe80::12345", 1, 0, 120).empty());
    CHECK(mdns_build_announcement("a", "h", "1:2:3:4:5:6:7::8", 1, 0, 120).empty());
}

void test_query()
{
    Bytes w = bheader(0, 1, 0, 0, 0);
    bname(w, "_airdrop._tcp.local");
    b16(w, 12);       /* PTR */
    b16(w, 0x0001);   /* IN, asking for a multicast answer */
    Bytes q = mdns_build_query();
    CHECK(q == w);

    /* A query parses, and teaches a cache nothing. */
    MdnsCache c;
    Seen seen;
    CHECK(feed(c, q, 0, seen));
    c.expire(1000 * SEC, callbacks(seen));
    CHECK(seen.found.empty() && seen.lost.empty());
}

/* ---- mDNS: parsing ---- */

/* Every name after the first points back into earlier ones, as Apple's
 * responders write them. */
void test_compression()
{
    Bytes m = bheader(0x8400, 1, 3, 0, 1);
    size_t service = m.size();   /* 12 */
    bname(m, "_airdrop._tcp.local");
    size_t local = service + 1 + 8 + 1 + 4;   /* the "local" label */
    b16(m, 12);
    b16(m, 1);

    /* PTR: the name points at the question's, the target is "inst" and a
     * pointer to it too. */
    b16(m, 0xc000 | static_cast<unsigned>(service));
    size_t at = bstart(m, 12, 1, 4500);
    size_t instance = m.size();
    bbytes(m, "\x04" "inst", 5);
    b16(m, 0xc000 | static_cast<unsigned>(service));
    bend(m, at);
    /* SRV, named by a pointer to the instance, with "box" and a pointer to
     * "local" as its target. */
    b16(m, 0xc000 | static_cast<unsigned>(instance));
    at = bstart(m, 33, 0x8001, 120);
    b16(m, 0);
    b16(m, 0);
    b16(m, 8770);
    size_t host = m.size();
    bbytes(m, "\x03" "box", 4);
    b16(m, 0xc000 | static_cast<unsigned>(local));
    bend(m, at);
    /* AAAA, named by a pointer to the SRV's target. */
    b16(m, 0xc000 | static_cast<unsigned>(host));
    at = bstart(m, 28, 0x8001, 120);
    m.insert(m.end(), FE80_1, FE80_1 + 16);
    bend(m, at);
    /* An NSEC in the additional section, which the browser passes over. */
    b16(m, 0xc000 | static_cast<unsigned>(host));
    at = bstart(m, 47, 0x8001, 120);
    b16(m, 0xc000 | static_cast<unsigned>(host));
    bbytes(m, "\x00\x06\x00\x00\x00\x08\x00\x01", 8);
    bend(m, at);

    MdnsCache c;
    Seen seen;
    CHECK(feed(c, m, 0, seen));
    CHECK(seen.found.size() == 1);
    if (seen.found.size() == 1) {
        const MdnsService &s = seen.found[0];
        CHECK(s.instance == "inst");
        CHECK(s.host == "box.local.");
        CHECK(s.address == "fe80::1%awdl0");
        CHECK(s.port == 8770);
        CHECK(s.flags == 0);   /* no TXT yet */
    }
}

#define CHECK_REJECTED(msg)                                               \
    do {                                                                  \
        const Bytes m_ = (msg);                                           \
        MdnsCache c_;                                                     \
        Seen s_;                                                          \
        CHECK(!feed(c_, m_, 0, s_));                                      \
        CHECK(s_.found.empty() && s_.lost.empty());                       \
    } while (0)

/* A query whose one name is the given bytes. */
Bytes question(const char *name, size_t n)
{
    Bytes m = bheader(0, 1, 0, 0, 0);
    bbytes(m, name, n);
    b16(m, 12);
    b16(m, 1);
    return m;
}

void test_malformed()
{
    /* Pointer loops: to itself, and two pointing at each other. */
    CHECK_REJECTED(question("\xc0\x0c", 2));
    Bytes m = bheader(0, 1, 0, 0, 0);
    bbytes(m, "\xc0\x0e\xc0\x0c", 4);
    b16(m, 12);
    b16(m, 1);
    CHECK_REJECTED(m);
    /* A pointer forward, past the end, into the header, or cut short. */
    CHECK_REJECTED(question("\x01" "a" "\xc0\x20", 4));
    CHECK_REJECTED(question("\xff\xff", 2));
    CHECK_REJECTED(question("\xc0\x02", 2));
    m = bheader(0, 1, 0, 0, 0);
    bbytes(m, "\x01" "a" "\xc0", 3);
    CHECK_REJECTED(m);
    /* A loop through a name that runs on into its own pointer. */
    m = bheader(0, 2, 0, 0, 0);
    bname(m, "a");
    b16(m, 12);
    b16(m, 1);
    bbytes(m, "\x01" "b" "\xc0\x13", 4);   /* points at itself, at 0x13 */
    b16(m, 12);
    b16(m, 1);
    CHECK_REJECTED(m);

    /* Labels: 64 bytes is an extended label type, and so are 0x40 and 0x80;
     * a name over 255 bytes; a label that isn't UTF-8; one cut short. */
    char label[65];
    label[0] = 64;
    std::memset(label + 1, 'x', 64);
    CHECK_REJECTED(question(label, 65));
    CHECK_REJECTED(question("\x80" "a", 2));
    std::string big;
    for (int i = 0; i < 4; i++) {
        big += static_cast<char>(63);
        big += std::string(63, 'y');
    }
    big += '\0';
    CHECK_REJECTED(question(big.data(), big.size()));
    /* The same length reached through a pointer: three labels of 63, then
     * a name of one more label pointing at them. */
    m = bheader(0, 2, 0, 0, 0);
    bbytes(m, big.data() + 64, 3 * 64 + 1);
    b16(m, 12);
    b16(m, 1);
    bbytes(m, big.data(), 64);
    b16(m, 0xc00c);
    b16(m, 12);
    b16(m, 1);
    CHECK_REJECTED(m);
    /* 255 bytes with the final zero fit: three labels of 63 and one of
     * 61. One of 62 doesn't. */
    big[3 * 64] = 62;
    big.erase(3 * 64 + 1, 1);
    CHECK_REJECTED(question(big.data(), big.size()));
    big[3 * 64] = 61;
    big.erase(3 * 64 + 1, 1);
    CHECK(big.size() == 255);
    {
        Bytes q = question(big.data(), big.size());
        MdnsCache c;
        CHECK(c.feed(q.data(), q.size(), "awdl0", 0, MdnsCallbacks{}));
    }
    CHECK_REJECTED(question("\x02\xff\xfe\x00", 4));
    m = bheader(0, 1, 0, 0, 0);
    bbytes(m, "\x05" "ab", 3);
    CHECK_REJECTED(m);

    /* Record data: an AAAA of 15 bytes, a TXT string running past its
     * record, an SRV too short, a PTR whose name runs past its data, data
     * longer than the message. */
    size_t at;
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "h.local");
    at = bstart(m, 28, 1, 120);
    m.insert(m.end(), FE80_1, FE80_1 + 15);
    bend(m, at);
    CHECK_REJECTED(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "i._airdrop._tcp.local");
    at = bstart(m, 16, 1, 120);
    bbytes(m, "\x09" "flags", 6);
    bend(m, at);
    CHECK_REJECTED(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "i._airdrop._tcp.local");
    at = bstart(m, 33, 1, 120);
    bbytes(m, "\x00\x00\x00\x00\x22", 5);
    bend(m, at);
    CHECK_REJECTED(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "_airdrop._tcp.local");
    at = bstart(m, 12, 1, 120);
    bname(m, "i._airdrop._tcp.local");
    bend(m, at);
    m[at + 1] = static_cast<uint8_t>(m[at + 1] - 2);   /* the data ends inside the name */
    CHECK_REJECTED(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "h.local");
    bstart(m, 28, 1, 120);
    m[m.size() - 1] = 16;   /* no data follows */
    CHECK_REJECTED(m);

    /* The header: counts past the message, an opcode, an error, short. */
    CHECK_REJECTED(bheader(0, 0xffff, 0, 0, 0));
    CHECK_REJECTED(bheader(0x8400, 0, 0, 0, 1));
    m = question("\x00", 1);
    m[2] = 0x08;   /* opcode 1 */
    CHECK_REJECTED(m);
    m = question("\x00", 1);
    m[3] = 0x03;   /* NXDOMAIN */
    CHECK_REJECTED(m);
    CHECK_REJECTED(bytes_of("\0\0\x84\0\0\0\0", 7));
    /* Longer than any mDNS message (RFC 6762 §17). */
    m = bheader(0x8400, 0, 0, 0, 0);
    m.resize(9001);
    CHECK_REJECTED(m);
    m = bheader(0x8400, 0, 0, 0, 0);
    m.resize(9000);
    {
        MdnsCache c;
        CHECK(c.feed(m.data(), m.size(), "awdl0", 0, MdnsCallbacks{}));
    }

    /* Every prefix of a good announcement fails, without a found. */
    Bytes a = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, 120);
    for (size_t len = 0; len < a.size(); len++) CHECK_REJECTED(Bytes(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(len)));

    /* A malformed message changes nothing, even what it read before the
     * fault: here a goodbye, then a broken record. */
    MdnsCache c;
    Seen seen;
    CHECK(feed(c, a, 0, seen));
    CHECK(seen.found.size() == 1);
    m = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, 0);
    m[7] = 5;   /* one record more than there is */
    bname(m, "x.local");
    bstart(m, 28, 1, 120);
    CHECK(!feed(c, m, SEC, seen));
    c.expire(3 * SEC, callbacks(seen));
    CHECK(seen.lost.empty());
}

/* ---- mDNS: the browser's cache ---- */

/* MDNS-6: nothing is found before the SRV target's address is known, and
 * only a link-local address counts (NET-2). */
void test_resolve_needs_aaaa()
{
    MdnsCache c;
    Seen seen;
    Bytes m = bheader(0x8400, 0, 3, 0, 0);
    bname(m, "_airdrop._tcp.local");
    size_t at = bstart(m, 12, 1, 4500);
    bname(m, "abc._airdrop._tcp.local");
    bend(m, at);
    bname(m, "abc._airdrop._tcp.local");
    at = bstart(m, 33, 0x8001, 120);
    bbytes(m, "\0\0\0\0\x22\x42", 6);
    bname(m, "mac.local");
    bend(m, at);
    bname(m, "abc._airdrop._tcp.local");
    at = bstart(m, 16, 0x8001, 4500);
    bbytes(m, "\x05" "other" "\x08" "flags=59", 15);
    bend(m, at);
    CHECK(feed(c, m, 0, seen));
    CHECK(seen.found.empty());

    const uint8_t routable[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "MAC.local");   /* names ignore case */
    at = bstart(m, 28, 0x8001, 120);
    m.insert(m.end(), routable, routable + 16);
    bend(m, at);
    CHECK(feed(c, m, 0, seen));
    CHECK(seen.found.empty());
    std::memcpy(m.data() + m.size() - 16, FE80_1, 16);
    CHECK(feed(c, m, SEC, seen));
    CHECK(seen.found.size() == 1);
    if (seen.found.size() == 1) {
        CHECK(seen.found[0].instance == "abc");
        CHECK(seen.found[0].host == "mac.local.");
        CHECK(seen.found[0].flags == 59);
    }
}

/* A goodbye (TTL 0) means gone in one second (RFC 6762 §10.1). */
void test_goodbye()
{
    MdnsCache c;
    Seen seen;
    Bytes a = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, UINT32_MAX);
    Bytes bye = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, 0);
    CHECK(feed(c, a, 0, seen));
    CHECK(seen.found.size() == 1);
    CHECK(feed(c, bye, 10 * SEC, seen));
    CHECK(seen.lost.empty());
    c.expire(10 * SEC + SEC / 2, callbacks(seen));
    CHECK(seen.lost.empty());
    /* Later copies of the goodbye, as in a burst, don't put it off. */
    CHECK(feed(c, bye, 10 * SEC + SEC * 9 / 10, seen));
    CHECK(seen.lost.empty());
    c.expire(11 * SEC, callbacks(seen));
    CHECK(seen.lost.size() == 1 && seen.lost[0] == "3fa9c01b22de");
    c.expire(20 * SEC, callbacks(seen));
    CHECK(seen.lost.size() == 1);
    CHECK(seen.found.size() == 1);

    /* A late copy of the goodbye doesn't bring it back, and a goodbye for
     * something never cached teaches nothing. */
    CHECK(feed(c, bye, 20 * SEC, seen));
    Bytes other = mdns_build_announcement("0a0b0c0d0e0f", "other", "fe80::2", 8770, 0x089, 0);
    CHECK(feed(c, other, 20 * SEC, seen));
    c.expire(20 * SEC + SEC / 2, callbacks(seen));
    CHECK(seen.found.size() == 1);
    CHECK(seen.lost.size() == 1);

    /* Announced again, it's found again. */
    CHECK(feed(c, a, 21 * SEC, seen));
    CHECK(seen.found.size() == 2);

    /* A goodbye for another address of the host leaves the one cached
     * alone. One for that address unresolves the service, but it's only
     * lost once its PTR goes, as in owly: a new address may follow. */
    Bytes m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "lares.local");
    size_t at = bstart(m, 28, 0x8001, 0);
    const uint8_t fe80_9[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9 };
    m.insert(m.end(), fe80_9, fe80_9 + 16);
    bend(m, at);
    CHECK(feed(c, m, 22 * SEC, seen));
    c.expire(30 * SEC, callbacks(seen));
    CHECK(seen.lost.size() == 1);
    std::memcpy(m.data() + m.size() - 16, FE80_1, 16);
    CHECK(feed(c, m, 31 * SEC, seen));
    CHECK(seen.lost.size() == 1);
    c.expire(32 * SEC, callbacks(seen));
    CHECK(seen.lost.size() == 1);   /* the PTR still stands */
}

/* SEC-11: whatever lifetime a record claims, it's kept 120 s at most
 * unless refreshed. */
void test_expiry()
{
    MdnsCache c;
    Seen seen;
    Bytes m = bheader(0x8400, 0, 4, 0, 0);
    size_t at;
    bname(m, "_airdrop._tcp.local");
    at = bstart(m, 12, 1, UINT32_MAX);
    bname(m, "abc._airdrop._tcp.local");
    bend(m, at);
    bname(m, "abc._airdrop._tcp.local");
    at = bstart(m, 33, 0x8001, UINT32_MAX);
    bbytes(m, "\0\0\0\0\x22\x42", 6);
    bname(m, "mac.local");
    bend(m, at);
    bname(m, "abc._airdrop._tcp.local");
    at = bstart(m, 16, 0x8001, UINT32_MAX);
    bbytes(m, "\x09" "flags=137", 10);
    bend(m, at);
    bname(m, "mac.local");
    at = bstart(m, 28, 0x8001, UINT32_MAX);
    m.insert(m.end(), FE80_1, FE80_1 + 16);
    bend(m, at);
    CHECK(feed(c, m, 0, seen));
    CHECK(seen.found.size() == 1);
    c.expire(119 * SEC, callbacks(seen));
    CHECK(seen.lost.empty());
    /* Refreshed at 100 s, it lasts to 220 s. */
    CHECK(feed(c, m, 100 * SEC, seen));
    CHECK(c.deadline() == 220 * SEC);
    c.expire(219 * SEC, callbacks(seen));
    CHECK(seen.lost.empty());
    c.expire(221 * SEC, callbacks(seen));
    CHECK(seen.lost.size() == 1);
    CHECK(seen.found.size() == 1);
    CHECK(c.deadline() == MDNS_NEVER);
}

/* SEC-11: the cache holds 256 instances and 256 hosts, and makes room as
 * they age out. */
void test_cache_bounds()
{
    MdnsCache c;
    Seen seen;
    char inst[32], host[32], addr[32];
    for (unsigned i = 0; i < 400; i++) {
        std::snprintf(inst, sizeof inst, "%012x", i);
        std::snprintf(host, sizeof host, "h%u", i);
        std::snprintf(addr, sizeof addr, "fe80::%x", i + 1);
        CHECK(feed(c, mdns_build_announcement(inst, host, addr, 8770, 0x089, UINT32_MAX), 0, seen));
    }
    CHECK(seen.found.size() == 256);
    c.expire(121 * SEC, callbacks(seen));
    CHECK(seen.lost.size() == 256);
    CHECK(feed(c, mdns_build_announcement("3fa9c01b22de", "late", "fe80::abc", 8770, 0x089, UINT32_MAX), 122 * SEC,
               seen));
    CHECK(seen.found.size() == 257);
    CHECK(seen.found.size() == 257 && seen.found[256].instance == "3fa9c01b22de");
    CHECK(seen.found.size() == 257 && seen.found[256].address == "fe80::abc%awdl0");
}

/* ------------------------------------------------------------ mDNS: the engine */

/* What the engine sent: to the group, or to an address and port. */
struct Sent {
    Bytes msg;
    bool group;
    Bytes to;
    uint16_t port;
};

Mdns::Send sender(std::vector<Sent> &out)
{
    return [&out](const uint8_t *msg, size_t len, const uint8_t *to, uint16_t port) {
        out.push_back(Sent{ Bytes(msg, msg + len), to == nullptr, to ? Bytes(to, to + 16) : Bytes(), port });
    };
}

const uint8_t PEER[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

bool publish_test_service(Mdns &m, unsigned flags, int64_t now)
{
    return m.publish("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1%awdl0", 8770, flags, now);
}

/* MDNS-7: copies every 16.4 ms for 64 copies, then 3 s of quiet, keeping
 * the spacing when the timer runs a little late. */
void test_engine_announce()
{
    Seen seen;
    std::vector<Sent> out;
    Mdns m("awdl0", callbacks(seen), sender(out));
    CHECK(m.deadline() == MDNS_NEVER);
    const int64_t t0 = 50 * SEC;
    CHECK(publish_test_service(m, 0x089, t0));
    CHECK(m.deadline() == t0);
    const Bytes ann = expected_announcement(4500, 120, 4500, 120);
    for (int64_t k = 0; k < 64; k++) {
        CHECK(m.deadline() == t0 + k * 16400);
        m.on_timer(m.deadline());
    }
    CHECK(out.size() == 64);
    for (const Sent &s : out) CHECK(s.group && s.port == MDNS_PORT && s.msg == ann);
    const int64_t last = t0 + 63 * 16400;
    CHECK(m.deadline() == last + 3 * SEC);
    m.on_timer(last + 2 * SEC);   /* early: nothing goes */
    CHECK(out.size() == 64);
    m.on_timer(last + 3 * SEC);
    CHECK(out.size() == 65);
    CHECK(m.deadline() == last + 3 * SEC + 16400);
    /* A little late, the next copy keeps to the plan; later than a copy's
     * spacing, it counts from now. */
    int64_t due = m.deadline();
    m.on_timer(due + 5000);
    CHECK(m.deadline() == due + 16400);
    due = m.deadline();
    m.on_timer(due + 40000);
    CHECK(m.deadline() == due + 40000 + 16400);
    CHECK(out.size() == 67);

    /* The same service with new flags is announced again at once, with no
     * goodbye: its records flush caches. */
    out.clear();
    const int64_t t1 = 60 * SEC;
    CHECK(publish_test_service(m, 0x08b, t1));
    CHECK(m.deadline() == t1);
    m.on_timer(t1);
    CHECK(out.size() == 1);
    CHECK(out.size() == 1
          && out[0].msg == mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x08b,
                                                   UINT32_MAX));
    CHECK(seen.found.empty() && seen.lost.empty());
}

/* The goodbye: a burst of 64 copies with TTL 0, from the moment it's
 * withdrawn, and nothing after. */
void test_engine_goodbye()
{
    Seen seen;
    std::vector<Sent> out;
    Mdns m("awdl0", callbacks(seen), sender(out));
    const int64_t t0 = 5 * SEC;
    CHECK(publish_test_service(m, 0x089, t0));
    m.on_timer(t0);
    CHECK(out.size() == 1);
    out.clear();
    const int64_t tw = t0 + 100000;   /* mid-burst */
    m.withdraw(tw);
    CHECK(m.deadline() == tw);
    const Bytes bye = expected_announcement(0, 0, 0, 0);
    for (int64_t k = 0; k < 64; k++) {
        CHECK(m.deadline() == tw + k * 16400);
        m.on_timer(m.deadline());
    }
    CHECK(out.size() == 64);
    for (const Sent &s : out) CHECK(s.group && s.port == MDNS_PORT && s.msg == bye);
    CHECK(m.deadline() == MDNS_NEVER);
    m.on_timer(tw + 10 * SEC);
    CHECK(out.size() == 64);

    /* Published again while its goodbye goes on, the goodbye stops. */
    out.clear();
    const int64_t t1 = 20 * SEC;
    CHECK(publish_test_service(m, 0x089, t1));
    m.on_timer(t1);
    m.withdraw(t1 + 1000);
    m.on_timer(t1 + 1000);
    CHECK(out.size() == 2 && out[1].msg == bye);
    CHECK(publish_test_service(m, 0x089, t1 + 2000));
    CHECK(m.deadline() == t1 + 2000);
    m.on_timer(t1 + 2000);
    CHECK(out.size() == 3 && out[2].msg == expected_announcement(4500, 120, 4500, 120));
    CHECK(m.deadline() == t1 + 2000 + 16400);

    /* Another instance replaces it: a goodbye burst for the old one beside
     * the new one's announcements. */
    out.clear();
    const int64_t t2 = 30 * SEC;
    CHECK(m.publish("0a0b0c0d0e0f", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, t2));
    CHECK(m.deadline() == t2);
    m.on_timer(t2);
    const Bytes other = mdns_build_announcement("0a0b0c0d0e0f", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089,
                                                UINT32_MAX);
    CHECK(out.size() == 2 && out[0].msg == other && out[1].msg == bye);
    size_t goodbyes = 0;
    while (m.deadline() < t2 + 2 * SEC) m.on_timer(m.deadline());
    for (const Sent &s : out) goodbyes += s.msg == bye;
    CHECK(goodbyes == 64);
    /* The same instance on another host: the goodbye withdraw() starts for
     * it is dropped at once, as the instance is published again, so only
     * the new records go, and their cache-flush bits replace the old ones
     * (as airdrop_mdns_publish() does on Linux). */
    out.clear();
    CHECK(m.publish("0a0b0c0d0e0f", "elsewhere", "fe80::1", 8770, 0x089, t2 + 5 * SEC));
    m.on_timer(t2 + 5 * SEC);
    CHECK(out.size() == 1
          && out[0].msg == mdns_build_announcement("0a0b0c0d0e0f", "elsewhere", "fe80::1", 8770, 0x089, UINT32_MAX));

    /* A malformed service is withdrawn, as Linux takes it for none. */
    CHECK(!m.publish("0a0b0c0d0e0f", "elsewhere", "192.0.2.1", 8770, 0x089, t2 + 6 * SEC));
    out.clear();
    m.on_timer(t2 + 6 * SEC);
    CHECK(out.size() == 1
          && out[0].msg == mdns_build_announcement("0a0b0c0d0e0f", "elsewhere", "fe80::1", 8770, 0x089, 0));
}

/* RFC 6762 §6: what a query asks of the published service is answered at
 * once, to the group, or to the asker for QU and legacy questions. */
void test_engine_answers()
{
    Seen seen;
    std::vector<Sent> out;
    Mdns m("awdl0", callbacks(seen), sender(out));
    int64_t t = 10 * SEC;
    CHECK(publish_test_service(m, 0x089, t));

    /* The service's PTR (QM): to the group, with SRV, TXT, AAAA and the
     * host's NSEC as additional records. */
    Bytes ptr_reply = bheader(0x8400, 0, 1, 0, 4);
    rec_ptr(ptr_reply, 4500, 0x0001);
    rec_srv(ptr_reply, 120, 0x8001);
    rec_txt(ptr_reply, 4500, 0x8001);
    rec_aaaa(ptr_reply, 120, 0x8001);
    rec_nsec_host(ptr_reply, 120, 0x8001);
    Bytes q = mdns_build_query();
    m.on_message(q.data(), q.size(), PEER, MDNS_PORT, t);
    CHECK(out.size() == 1 && out[0].group && out[0].port == MDNS_PORT && out[0].msg == ptr_reply);

    /* The same asked for a unicast answer (QU): to the asker. */
    out.clear();
    q[q.size() - 2] = 0x80;
    m.on_message(q.data(), q.size(), PEER, MDNS_PORT, t += SEC);
    CHECK(out.size() == 1 && !out[0].group && out[0].port == MDNS_PORT && same(out[0].to, PEER, 16)
          && out[0].msg == ptr_reply);

    /* A legacy querier, on another port: its ID and question back, no
     * cache-flush bits, TTLs of 10 s, to it alone (§6.7). */
    out.clear();
    q = mdns_build_query();
    q[0] = 0x12;
    q[1] = 0x34;
    m.on_message(q.data(), q.size(), PEER, 40000, t += SEC);
    Bytes legacy = bheader(0x8400, 1, 1, 0, 4);
    legacy[0] = 0x12;
    legacy[1] = 0x34;
    bname(legacy, "_airdrop._tcp.local");
    b16(legacy, 12);
    b16(legacy, 1);
    rec_ptr(legacy, 10, 1);
    rec_srv(legacy, 10, 1);
    rec_txt(legacy, 10, 1);
    rec_aaaa(legacy, 10, 1);
    rec_nsec_host(legacy, 10, 1);
    CHECK(out.size() == 1 && !out[0].group && out[0].port == 40000 && same(out[0].to, PEER, 16)
          && out[0].msg == legacy);

    /* Known-answer suppression (§7.1): a PTR the asker holds with half its
     * TTL left isn't sent again; one about to expire is. */
    out.clear();
    Bytes known = bheader(0, 1, 1, 0, 0);
    bname(known, "_airdrop._tcp.local");
    b16(known, 12);
    b16(known, 1);
    rec_ptr(known, 2250, 1);
    m.on_message(known.data(), known.size(), PEER, MDNS_PORT, t += SEC);
    CHECK(out.empty());
    known = bheader(0, 1, 1, 0, 0);
    bname(known, "_airdrop._tcp.local");
    b16(known, 12);
    b16(known, 1);
    rec_ptr(known, 2249, 1);
    m.on_message(known.data(), known.size(), PEER, MDNS_PORT, t += SEC);
    CHECK(out.size() == 1 && out[0].msg == ptr_reply);

    /* The instance's SRV: AAAA and the host's NSEC beside it. */
    out.clear();
    Bytes srv_q = bheader(0, 1, 0, 0, 0);
    bname(srv_q, INST);
    b16(srv_q, 33);
    b16(srv_q, 1);
    m.on_message(srv_q.data(), srv_q.size(), PEER, MDNS_PORT, t += SEC);
    Bytes srv_reply = bheader(0x8400, 0, 1, 0, 2);
    rec_srv(srv_reply, 120, 0x8001);
    rec_aaaa(srv_reply, 120, 0x8001);
    rec_nsec_host(srv_reply, 120, 0x8001);
    CHECK(out.size() == 1 && out[0].group && out[0].msg == srv_reply);

    /* ANY for the instance: SRV and TXT, then AAAA and NSEC. */
    out.clear();
    srv_q[srv_q.size() - 3] = 255;
    m.on_message(srv_q.data(), srv_q.size(), PEER, MDNS_PORT, t += SEC);
    Bytes any_reply = bheader(0x8400, 0, 2, 0, 2);
    rec_srv(any_reply, 120, 0x8001);
    rec_txt(any_reply, 4500, 0x8001);
    rec_aaaa(any_reply, 120, 0x8001);
    rec_nsec_host(any_reply, 120, 0x8001);
    CHECK(out.size() == 1 && out[0].msg == any_reply);

    /* A type the names don't have: the host's A, the instance's A, answered
     * with NSEC (§6.1). */
    out.clear();
    Bytes a_q = bheader(0, 1, 0, 0, 0);
    bname(a_q, HOST);
    b16(a_q, 1);
    b16(a_q, 1);
    m.on_message(a_q.data(), a_q.size(), PEER, MDNS_PORT, t += SEC);
    Bytes nsec_reply = bheader(0x8400, 0, 1, 0, 0);
    rec_nsec_host(nsec_reply, 120, 0x8001);
    CHECK(out.size() == 1 && out[0].msg == nsec_reply);
    out.clear();
    a_q = bheader(0, 1, 0, 0, 0);
    bname(a_q, INST);
    b16(a_q, 1);
    b16(a_q, 1);
    m.on_message(a_q.data(), a_q.size(), PEER, MDNS_PORT, t += SEC);
    nsec_reply = bheader(0x8400, 0, 1, 0, 0);
    rec_nsec_instance(nsec_reply, 120, 0x8001);
    CHECK(out.size() == 1 && out[0].msg == nsec_reply);

    /* Nothing for other names, for our own address, for an address off
     * the link, or for a response while not browsing. */
    out.clear();
    Bytes other_q = bheader(0, 1, 0, 0, 0);
    bname(other_q, "_companion-link._tcp.local");
    b16(other_q, 12);
    b16(other_q, 1);
    m.on_message(other_q.data(), other_q.size(), PEER, MDNS_PORT, t += SEC);
    q = mdns_build_query();
    m.on_message(q.data(), q.size(), FE80_1, MDNS_PORT, t += SEC);
    const uint8_t routable[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
    m.on_message(q.data(), q.size(), routable, MDNS_PORT, t += SEC);
    Bytes a = mdns_build_announcement("0a0b0c0d0e0f", "phone", "fe80::2", 8770, 0x089, UINT32_MAX);
    m.on_message(a.data(), a.size(), PEER, MDNS_PORT, t += SEC);
    CHECK(out.empty());
    CHECK(seen.found.empty());

    /* At most 20 replies a second, however many queries come. */
    t += SEC;
    for (int i = 0; i < 25; i++) m.on_message(q.data(), q.size(), PEER, MDNS_PORT, t + i * 1000);
    CHECK(out.size() == 20);
    m.on_message(q.data(), q.size(), PEER, MDNS_PORT, t + SEC);
    CHECK(out.size() == 21);
}

/* The browser: the first query 20 ms in, then one a second; what it finds
 * reported, ours apart, and what it lacks asked for. */
void test_engine_browse()
{
    Seen seen;
    std::vector<Sent> out;
    Mdns m("awdl0", callbacks(seen), sender(out));
    const int64_t t = 7 * SEC;
    m.browse(true, t);
    CHECK(m.deadline() == t + 20000);
    m.on_timer(t + 19999);
    CHECK(out.empty());
    m.on_timer(t + 20000);
    CHECK(out.size() == 1 && out[0].group && out[0].port == MDNS_PORT && out[0].msg == mdns_build_query());
    CHECK(m.deadline() == t + 20000 + SEC);
    m.on_timer(t + 20000 + SEC);
    CHECK(out.size() == 2);
    CHECK(m.deadline() == t + 20000 + 2 * SEC);   /* without doubling */

    /* A peer's announcement resolves its service, its address with the
     * zone after it. */
    Bytes a = mdns_build_announcement("0a0b0c0d0e0f", "phone", "fe80::2", 8771, 0x3fb, UINT32_MAX);
    m.on_message(a.data(), a.size(), PEER, MDNS_PORT, t + SEC);
    CHECK(seen.found.size() == 1);
    if (seen.found.size() == 1) {
        CHECK(seen.found[0].instance == "0a0b0c0d0e0f");
        CHECK(seen.found[0].host == "phone.local.");
        CHECK(seen.found[0].address == "fe80::2%awdl0");
        CHECK(seen.found[0].port == 8771);
        CHECK(seen.found[0].flags == 0x3fb);
    }
    /* A response from another port than 5353 is ignored. */
    Bytes b = mdns_build_announcement("111111111111", "other", "fe80::3", 8770, 0x089, UINT32_MAX);
    m.on_message(b.data(), b.size(), PEER, 40000, t + SEC);
    CHECK(seen.found.size() == 1);
    /* The query due before any record expires stays next. */
    CHECK(m.deadline() == t + 20000 + 2 * SEC);

    /* Half a service: the next query asks for what's missing, TXT and SRV
     * for the instance (ANY) and the address of its SRV target. */
    Bytes half = bheader(0x8400, 0, 2, 0, 0);
    bname(half, "_airdrop._tcp.local");
    size_t at = bstart(half, 12, 1, 4500);
    bname(half, "abc._airdrop._tcp.local");
    bend(half, at);
    bname(half, "abc._airdrop._tcp.local");
    at = bstart(half, 33, 0x8001, 120);
    bbytes(half, "\0\0\0\0\x22\x42", 6);
    bname(half, "mac.local");
    bend(half, at);
    m.on_message(half.data(), half.size(), PEER, MDNS_PORT, t + SEC + 500000);
    CHECK(seen.found.size() == 1);
    out.clear();
    m.on_timer(m.deadline());
    Bytes want = bheader(0, 3, 0, 0, 0);
    bname(want, "_airdrop._tcp.local");
    b16(want, 12);
    b16(want, 1);
    bname(want, "abc._airdrop._tcp.local");
    b16(want, 255);
    b16(want, 1);
    bname(want, "mac.local");
    b16(want, 28);
    b16(want, 1);
    CHECK(out.size() == 1 && out[0].msg == want);

    /* This receiver's own service, heard back from another address, isn't
     * reported, found or lost. */
    CHECK(m.publish("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, t + 3 * SEC));
    Bytes own = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::5", 8770, 0x089, UINT32_MAX);
    const uint8_t fe80_5[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5 };
    m.on_message(own.data(), own.size(), fe80_5, MDNS_PORT, t + 3 * SEC);
    CHECK(seen.found.size() == 1);

    /* Browsing off: what was found is lost, and responses no longer count. */
    m.browse(false, t + 4 * SEC);
    CHECK(seen.lost.size() == 1 && seen.lost[0] == "0a0b0c0d0e0f");
    m.on_message(a.data(), a.size(), PEER, MDNS_PORT, t + 4 * SEC);
    CHECK(seen.found.size() == 1);
    CHECK(m.deadline() == t + 3 * SEC);   /* only the announcements are left */
}

/* SEC-11 through the engine: a service not refreshed is lost when its
 * records expire, which deadline() wakes the caller for. */
void test_engine_expiry()
{
    Seen seen;
    std::vector<Sent> out;
    Mdns m("awdl0", callbacks(seen), sender(out));
    const int64_t t = 3 * SEC;
    m.browse(true, t);
    Bytes a = mdns_build_announcement("0a0b0c0d0e0f", "phone", "fe80::2", 8771, 0x089, UINT32_MAX);
    m.on_message(a.data(), a.size(), PEER, MDNS_PORT, t);
    CHECK(seen.found.size() == 1);
    while (m.deadline() < t + 120 * SEC) m.on_timer(m.deadline());
    CHECK(seen.lost.empty());
    CHECK(m.deadline() == t + 120 * SEC);
    m.on_timer(m.deadline());
    CHECK(seen.lost.size() == 1 && seen.lost[0] == "0a0b0c0d0e0f");
    /* About one query a second went out meanwhile. */
    CHECK(out.size() >= 119 && out.size() <= 121);
}

/* Closing: one goodbye for what's published, the last copy of a goodbye
 * under way, and from then on nothing at all. */
void test_engine_close()
{
    Seen seen;
    std::vector<Sent> out;
    Mdns m("awdl0", callbacks(seen), sender(out));
    const int64_t t = 9 * SEC;
    CHECK(publish_test_service(m, 0x089, t));
    m.on_timer(t);
    m.withdraw(t + 1000);
    CHECK(m.publish("0a0b0c0d0e0f", "lares", "fe80::1", 8770, 0x089, t + 2000));
    m.browse(true, t + 2000);
    Bytes a = mdns_build_announcement("111111111111", "phone", "fe80::2", 8771, 0x089, UINT32_MAX);
    m.on_message(a.data(), a.size(), PEER, MDNS_PORT, t + 3000);
    CHECK(seen.found.size() == 1);
    out.clear();
    m.close();
    CHECK(out.size() == 2);
    CHECK(out.size() == 2
          && out[0].msg == mdns_build_announcement("0a0b0c0d0e0f", "lares", "fe80::1", 8770, 0x089, 0)
          && out[1].msg == expected_announcement(0, 0, 0, 0));
    CHECK(m.deadline() == MDNS_NEVER);
    out.clear();
    m.on_timer(t + 10 * SEC);
    Bytes q = mdns_build_query();
    m.on_message(q.data(), q.size(), PEER, MDNS_PORT, t + 10 * SEC);
    m.browse(false, t + 10 * SEC);
    CHECK(!publish_test_service(m, 0x089, t + 10 * SEC));
    m.close();
    CHECK(out.empty());
    CHECK(seen.lost.empty());
}

Bytes wrap_der(uint8_t tag, const Bytes& content)
{
    Bytes out;
    out.push_back(tag);
    if (content.size() < 128) {
        out.push_back(static_cast<uint8_t>(content.size()));
    } else if (content.size() < 256) {
        out.push_back(0x81);
        out.push_back(static_cast<uint8_t>(content.size()));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<uint8_t>((content.size() >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(content.size() & 0xff));
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

void test_record_hashes()
{
    const std::string h1 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::string h2 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const std::string h1_upper = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";

    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>"
        "<key>ValidatedEmailHashes</key><array><string>" + h1_upper + "</string><string>not-a-hash</string></array>"
        "<key>ValidatedPhoneHashes</key><array><string>" + h2 + "</string></array>"
        "</dict></plist>";
    Bytes xml_bytes(xml.begin(), xml.end());

    Value root = Value::dict();
    Value emails = Value::array();
    emails.push(Value::string(h1));
    root.add("ValidatedEmailHashes", std::move(emails));
    Bytes h2_raw(32);
    for (size_t i = 0; i < 32; ++i) {
        auto hexval = [](char c) -> uint8_t {
            return static_cast<uint8_t>(c >= 'a' ? c - 'a' + 10 : c - '0');
        };
        h2_raw[i] = static_cast<uint8_t>((hexval(h2[i * 2]) << 4) | hexval(h2[i * 2 + 1]));
    }
    Value phones = Value::array();
    phones.push(Value::data(h2_raw));
    root.add("ValidatedPhoneHashes", std::move(phones));
    Error err;
    Bytes bp;
    CHECK(bplist_encode(root, bp, &err));

    std::vector<std::string> got;

    /* 1. Unsigned raw XML plist and unsigned raw binary plist MUST be rejected. */
    CHECK(!airdrop_extract_record_hashes(xml_bytes.data(), xml_bytes.size(), got));
    CHECK(got.empty());
    CHECK(!airdrop_extract_record_hashes(bp.data(), bp.size(), got));
    CHECK(got.empty());

    /* 2. Unsigned CMS SignedData (signerInfos = {}) MUST be rejected. */
    Bytes oct = wrap_der(0x04, xml_bytes);
    Bytes ctx0 = wrap_der(0xA0, oct);
    Bytes oid_data = { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01 };
    Bytes encap = oid_data;
    encap.insert(encap.end(), ctx0.begin(), ctx0.end());
    encap = wrap_der(0x30, encap);
    Bytes signed_body = { 0x02, 0x01, 0x01, 0x31, 0x00 }; /* version=1, digestAlgs={} */
    signed_body.insert(signed_body.end(), encap.begin(), encap.end());
    signed_body.push_back(0x31);
    signed_body.push_back(0x00); /* signerInfos={} */
    Bytes signed_seq = wrap_der(0x30, signed_body);
    Bytes oid_sd = { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
    Bytes ci_body = oid_sd;
    Bytes sd_ctx = wrap_der(0xA0, signed_seq);
    ci_body.insert(ci_body.end(), sd_ctx.begin(), sd_ctx.end());
    Bytes unsigned_cms = wrap_der(0x30, ci_body);
    CHECK(!airdrop_extract_record_hashes(unsigned_cms.data(), unsigned_cms.size(), got));
    CHECK(got.empty());

    /* 3. Self-signed CMS SignedData MUST be rejected by both Apple-pinned and test-root paths. */
    test_cms::EphemeralCert self_cert;
    CHECK(test_cms::make_cert({ L"CN=Forged Self-Signed", false, false, -1, 7, nullptr }, nullptr, self_cert));
    Bytes self_cms;
    CHECK(test_cms::sign_cms(self_cert, { &self_cert }, xml_bytes.data(), xml_bytes.size(), self_cms));
    CHECK(!airdrop_extract_record_hashes(self_cms.data(), self_cms.size(), got));
    CHECK(got.empty());
    CHECK(!airdrop_extract_record_hashes_with_roots(self_cms.data(), self_cms.size(), { self_cert.der }, got));
    CHECK(got.empty());

    /* 4. Valid ECDSA Root -> Intermediate -> Leaf chain: rejected against Apple CA, accepted against its own Root. */
    test_cms::EphemeralCert ec_root, ec_inter, ec_leaf;
    CHECK(test_cms::make_cert({ L"CN=Test Root EC", true, false, -1, 30, nullptr }, nullptr, ec_root));
    CHECK(test_cms::make_cert({ L"CN=Test Inter EC", true, false, -1, 15, nullptr }, &ec_root, ec_inter));
    CHECK(test_cms::make_cert({ L"CN=Test Leaf EC", false, false, -1, 7, nullptr }, &ec_inter, ec_leaf));
    Bytes ec_cms_xml, ec_cms_bp;
    CHECK(test_cms::sign_cms(ec_leaf, { &ec_leaf, &ec_inter }, xml_bytes.data(), xml_bytes.size(), ec_cms_xml));
    CHECK(test_cms::sign_cms(ec_leaf, { &ec_leaf, &ec_inter }, bp.data(), bp.size(), ec_cms_bp));

    /* Untrusted CA chain MUST be rejected by production Apple CA verifier. */
    CHECK(!airdrop_extract_record_hashes(ec_cms_xml.data(), ec_cms_xml.size(), got));
    CHECK(got.empty());
    CHECK(!airdrop_extract_record_hashes(ec_cms_bp.data(), ec_cms_bp.size(), got));
    CHECK(got.empty());
    /* Unsigned plist still rejected even when a test root is supplied. */
    CHECK(!airdrop_extract_record_hashes_with_roots(xml_bytes.data(), xml_bytes.size(), { ec_root.der }, got));
    CHECK(!airdrop_extract_record_hashes_with_roots(unsigned_cms.data(), unsigned_cms.size(), { ec_root.der }, got));

    /* Verified against its own test root: both XML and binary plist payloads succeed. */
    CHECK(airdrop_extract_record_hashes_with_roots(ec_cms_xml.data(), ec_cms_xml.size(), { ec_root.der }, got));
    CHECK(got.size() == 2);
    CHECK(std::find(got.begin(), got.end(), h1) != got.end());
    CHECK(std::find(got.begin(), got.end(), h2) != got.end());

    CHECK(airdrop_extract_record_hashes_with_roots(ec_cms_bp.data(), ec_cms_bp.size(), { ec_root.der }, got));
    CHECK(got.size() == 2);
    CHECK(std::find(got.begin(), got.end(), h1) != got.end());
    CHECK(std::find(got.begin(), got.end(), h2) != got.end());

    /* 5. RSA-2048 CA -> Leaf chain also verifies cleanly. */
    test_cms::EphemeralCert rsa_root, rsa_leaf;
    CHECK(test_cms::make_cert({ L"CN=Test Root RSA", true, true, -1, 30, nullptr }, nullptr, rsa_root));
    CHECK(test_cms::make_cert({ L"CN=Test Leaf RSA", false, true, -1, 7, nullptr }, &rsa_root, rsa_leaf));
    Bytes rsa_cms;
    CHECK(test_cms::sign_cms(rsa_leaf, { &rsa_leaf }, xml_bytes.data(), xml_bytes.size(), rsa_cms));
    CHECK(!airdrop_extract_record_hashes(rsa_cms.data(), rsa_cms.size(), got));
    CHECK(!airdrop_extract_record_hashes_with_roots(rsa_cms.data(), rsa_cms.size(), { ec_root.der }, got));
    CHECK(airdrop_extract_record_hashes_with_roots(rsa_cms.data(), rsa_cms.size(), { rsa_root.der }, got));
    CHECK(got.size() == 2);

    /* 6. Tampered CMS payload or signature MUST be rejected. */
    Bytes tampered = ec_cms_xml;
    size_t pos = std::string(reinterpret_cast<const char*>(tampered.data()), tampered.size()).find(h1_upper);
    CHECK(pos != std::string::npos);
    if (pos != std::string::npos) {
        tampered[pos] = (tampered[pos] == 'B') ? 'A' : 'B';
        CHECK(!airdrop_extract_record_hashes_with_roots(tampered.data(), tampered.size(), { ec_root.der }, got));
        CHECK(got.empty());
    }

    /* 7. Expired leaf certificate MUST be rejected. */
    test_cms::EphemeralCert exp_leaf;
    CHECK(test_cms::make_cert({ L"CN=Expired Leaf", false, false, -10, -1, nullptr }, &ec_inter, exp_leaf));
    Bytes exp_cms;
    CHECK(test_cms::sign_cms(exp_leaf, { &exp_leaf, &ec_inter }, xml_bytes.data(), xml_bytes.size(), exp_cms));
    CHECK(!airdrop_extract_record_hashes_with_roots(exp_cms.data(), exp_cms.size(), { ec_root.der }, got));
    CHECK(got.empty());

    /* 8. Leaf signed by a non-CA leaf MUST be rejected (BasicConstraints CA:FALSE). */
    test_cms::EphemeralCert fake_sub;
    CHECK(test_cms::make_cert({ L"CN=Fake Sub", false, false, -1, 7, nullptr }, &ec_leaf, fake_sub));
    Bytes fake_sub_cms;
    CHECK(test_cms::sign_cms(fake_sub, { &fake_sub, &ec_leaf, &ec_inter }, xml_bytes.data(), xml_bytes.size(),
                             fake_sub_cms));
    CHECK(!airdrop_extract_record_hashes_with_roots(fake_sub_cms.data(), fake_sub_cms.size(), { ec_root.der }, got));
    CHECK(got.empty());

    /* 9. Critical extension handling: Apple marker OID (1.2.840.113635.*) allowed,
     * arbitrary unknown critical OID rejected. */
    test_cms::EphemeralCert apple_ext_leaf, foreign_ext_leaf;
    CHECK(test_cms::make_cert({ L"CN=Apple Ext Leaf", false, false, -1, 7, "1.2.840.113635.100.6.25" },
                              &ec_inter, apple_ext_leaf));
    CHECK(test_cms::make_cert({ L"CN=Foreign Ext Leaf", false, false, -1, 7, "1.3.6.1.4.1.99999.1" },
                              &ec_inter, foreign_ext_leaf));
    Bytes apple_ext_cms, foreign_ext_cms;
    CHECK(test_cms::sign_cms(apple_ext_leaf, { &apple_ext_leaf, &ec_inter }, xml_bytes.data(), xml_bytes.size(),
                             apple_ext_cms));
    CHECK(test_cms::sign_cms(foreign_ext_leaf, { &foreign_ext_leaf, &ec_inter }, xml_bytes.data(), xml_bytes.size(),
                             foreign_ext_cms));
    CHECK(airdrop_extract_record_hashes_with_roots(apple_ext_cms.data(), apple_ext_cms.size(), { ec_root.der }, got));
    CHECK(got.size() == 2);
    CHECK(!airdrop_extract_record_hashes_with_roots(foreign_ext_cms.data(), foreign_ext_cms.size(), { ec_root.der },
                                                    got));
    CHECK(got.empty());

    /* 10. Official Apple AAI intermediates verify cleanly against Apple's embedded Root CAs
     * under Windows CryptoAPI, while untrusted/self-signed certs do not. */
    CHECK(airdrop_verify_apple_cert(kAppleAai2Ca, sizeof(kAppleAai2Ca)));
    CHECK(airdrop_verify_apple_cert(kAppleAaiCaG3, sizeof(kAppleAaiCaG3)));
    CHECK(airdrop_verify_apple_cert(kAppleAaiCa5G1, sizeof(kAppleAaiCa5G1)));
    CHECK(airdrop_verify_apple_cert(kAppleAaiCa7G1, sizeof(kAppleAaiCa7G1)));
    CHECK(!airdrop_verify_apple_cert(self_cert.der.data(), self_cert.der.size()));
    CHECK(!airdrop_verify_apple_cert(ec_leaf.der.data(), ec_leaf.der.size()));

    /* 11. Empty and garbage inputs rejected cleanly. */
    got.clear();
    CHECK(!airdrop_extract_record_hashes(nullptr, 0, got));
    const uint8_t junk[] = { 0x01, 0x02, 0x03, 0x04 };
    CHECK(!airdrop_extract_record_hashes(junk, sizeof(junk), got));
}

} // namespace

void run_format_tests()
{
    test_safe_name();
    test_uti();
    test_plist_round_trip();
    test_plist_exact();
    test_plist_vectors();
    test_plist_malformed();
    test_plist_limits();
    test_record_hashes();
    test_cpio_vectors();
    test_cpio_after_trailer();
    test_cpio_refused();
    test_cpio_malformed();
    test_cpio_limits();
    test_cpio_writer();
    test_cpio_writer_errors();
    test_compress_round_trip();
    test_gzip_members();
    test_compress_bombs();
    test_compress_malformed();
    test_http_requests();
    test_http_responses();
    test_http_rejected();
    test_http_chunk();
    test_upload_vectors();
    test_announcement_bytes();
    test_announcement_round_trip();
    test_build_rejects();
    test_query();
    test_compression();
    test_malformed();
    test_resolve_needs_aaaa();
    test_goodbye();
    test_expiry();
    test_cache_bounds();
    test_engine_announce();
    test_engine_goodbye();
    test_engine_answers();
    test_engine_browse();
    test_engine_expiry();
    test_engine_close();
}
