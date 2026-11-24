// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* CPIO archives, the payload of /Upload (UP-2, UP-2a), as src/airdrop/
 * cpio.c. An entry is a header, its name with a NUL, then its data; the
 * entry named TRAILER!!! ends the archive. odc (magic 070707, POSIX's
 * portable format) has a 76-byte header of octal fields and pads nothing.
 * newc (070701) has a 110-byte header of 8-digit hex fields and pads the
 * name and the data to 4 bytes.
 *
 * The writer emits odc as Finder lays it out: owner 501:20, one link for a
 * file and two for a directory, inode numbers counting up from 1. The
 * reader takes odc and newc in pieces of any size, holds no more than one
 * header and name, and hands file data straight through. It refuses
 * absolute paths, ".." and anything but directories and regular files:
 * links, hard-linked files, devices, FIFOs and sockets (SEC-1).
 *
 * Ported from owly's CPIO reader and writer (MIT). */
#include "airdrop_text.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace lares::airdrop {

namespace {

constexpr size_t ODC_HEADER = 76;
constexpr size_t NEWC_HEADER = 110;
constexpr char TRAILER[] = "TRAILER!!!";

constexpr unsigned MODE_TYPE = 0170000;
constexpr unsigned MODE_DIR = 0040000;
constexpr unsigned MODE_FILE = 0100000;

constexpr uint64_t ODC_MAX_SIZE = 077777777777ull;   /* 11 octal digits */
constexpr unsigned ODC_MAX_SIX = 0777777;             /* 6 octal digits */

/* The owner a Mac's archives carry; receivers ignore it. */
constexpr unsigned UID = 501;
constexpr unsigned GID = 20;

constexpr int ODC = 1, NEWC = 2;

bool call_sink(const Sink &sink, const uint8_t *buf, size_t len, Error *error)
{
    Error local;
    if (!sink || sink(buf, len, local)) return true;
    if (!local.set) local.assign(ErrorCode::Refused, "Archive data refused");
    propagate(error, local);
    return false;
}

/* Whether a path names something outside where the archive is unpacked: an
 * absolute path, or one with a ".." component (SEC-1). */
bool escapes(const std::string &path)
{
    if (!path.empty() && path[0] == '/') return true;
    for (size_t p = 0; p < path.size();) {
        size_t slash = path.find('/', p);
        size_t n = (slash == std::string::npos ? path.size() : slash) - p;
        if (n == 2 && path[p] == '.' && path[p + 1] == '.') return true;
        p += n;
        if (p < path.size() && path[p] == '/') p++;
    }
    return false;
}

/* A path the reader on the other side will take. A NUL inside would cut it
 * short on Linux; here it's refused. */
bool check_path(const std::string &path, Error *error)
{
    if (path.empty() || path == TRAILER || escapes(path) || path.find('\0') != std::string::npos) {
        set_error(error, ErrorCode::Refused, "A path that can't go in an archive");
        return false;
    }
    if (path.size() + 1 > CPIO_MAX_NAME) {
        set_error(error, ErrorCode::Limit, "A path too long to archive");
        return false;
    }
    return true;
}

} // namespace

/* ------------------------------------------------------------ writing */

CpioWriter::CpioWriter(Sink sink) : sink_(std::move(sink)) {}

/* Whether a new entry or the trailer may come now. */
bool CpioWriter::ready(Error *error)
{
    if (failed_ || finished_)
        set_error(error, ErrorCode::Invalid, failed_ ? "The archive failed earlier" : "The archive is finished");
    else if (left_)
        set_error(error, ErrorCode::Invalid, "A file's data is missing");
    else
        return true;
    return false;
}

/* An odc header and the name after it, in one piece. */
bool CpioWriter::put_header(const char *name, size_t namelen, unsigned mode, unsigned uid, unsigned gid,
                            unsigned nlink, unsigned ino, uint64_t mtime, uint64_t size, Error *error)
{
    size_t namesize = namelen + 1;
    uint8_t buf[ODC_HEADER + CPIO_MAX_NAME + 1];
    std::snprintf(reinterpret_cast<char *>(buf), sizeof buf, "070707%06o%06o%06o%06o%06o%06o%06o%011llo%06o%011llo",
                  0u, ino & ODC_MAX_SIX, mode, uid, gid, nlink, 0u, static_cast<unsigned long long>(mtime),
                  static_cast<unsigned>(namesize), static_cast<unsigned long long>(size));
    std::memcpy(buf + ODC_HEADER, name, namelen);
    buf[ODC_HEADER + namelen] = 0;
    if (call_sink(sink_, buf, ODC_HEADER + namesize, error)) return true;
    failed_ = true;
    return false;
}

/* A directory. The call gives no time, so it's dated now. */
bool CpioWriter::dir(const std::string &path, Error *error)
{
    if (!ready(error) || !check_path(path, error)) return false;
    uint64_t now = std::min(static_cast<uint64_t>(std::time(nullptr)), ODC_MAX_SIZE);
    return put_header(path.c_str(), path.size(), MODE_DIR | 0755, UID, GID, 2, ino_++, now, 0, error);
}

bool CpioWriter::file(const std::string &path, uint64_t size, int64_t mtime, Error *error)
{
    if (!ready(error) || !check_path(path, error)) return false;
    if (size > ODC_MAX_SIZE) {
        set_error(error, ErrorCode::Limit, "A file too large to archive");
        return false;
    }
    uint64_t when = mtime < 0 ? 0 : std::min(static_cast<uint64_t>(mtime), ODC_MAX_SIZE);
    if (!put_header(path.c_str(), path.size(), MODE_FILE | 0644, UID, GID, 1, ino_++, when, size, error))
        return false;
    left_ = size;
    return true;
}

bool CpioWriter::file_data(const uint8_t *buf, size_t len, Error *error)
{
    if (failed_ || finished_ || len > left_) {
        set_error(error, ErrorCode::Invalid,
                  len > left_ ? "More data than the file's size" : "The archive can't take data now");
        return false;
    }
    if (len == 0) return true;
    if (!call_sink(sink_, buf, len, error)) {
        failed_ = true;
        return false;
    }
    left_ -= len;
    return true;
}

/* The trailer: all zero but its name, as Finder writes it (UP-2a). */
bool CpioWriter::finish(Error *error)
{
    if (!ready(error) || !put_header(TRAILER, sizeof TRAILER - 1, 0, 0, 0, 0, 0, 0, 0, error)) return false;
    finished_ = true;
    return true;
}

/* ------------------------------------------------------------ reading */

namespace {

enum { F_INO, F_MODE, F_UID, F_GID, F_NLINK, F_MTIME, F_NAMESIZE, F_FILESIZE, F_COUNT };

/* Where each field is: odc's are octal, of 6 digits but mtime's and the
 * size's 11; newc's are 8 hex digits in a row after the magic. */
const struct {
    uint8_t at, len;
} ODC_FIELDS[F_COUNT] = {
    { 12, 6 }, { 18, 6 }, { 24, 6 }, { 30, 6 }, { 36, 6 }, { 48, 11 }, { 59, 6 }, { 65, 11 },
};
const uint8_t NEWC_FIELDS[F_COUNT] = { 0, 1, 2, 3, 4, 5, 11, 6 };

/* Padding after `n` bytes: newc's to 4, odc's none. */
unsigned pad_to_4(int format, uint64_t n)
{
    return format == NEWC ? static_cast<unsigned>((4 - n % 4) % 4) : 0;
}

} // namespace

CpioReader::CpioReader(CpioCallbacks cb) : cb_(std::move(cb)) {}

size_t CpioReader::header_len() const
{
    return format_ == ODC ? ODC_HEADER : NEWC_HEADER;
}

uint64_t CpioReader::field(int which) const
{
    size_t at = format_ == ODC ? ODC_FIELDS[which].at : 6u + 8u * NEWC_FIELDS[which];
    size_t len = format_ == ODC ? ODC_FIELDS[which].len : 8;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++)
        v = v * (format_ == ODC ? 8 : 16) + static_cast<uint64_t>(xdigit_value(buf_[at + i]));
    return v;
}

/* Every digit of the header after the magic is one of its base's. */
bool CpioReader::digits_ok() const
{
    for (size_t i = 6; i < header_len(); i++) {
        uint8_t c = buf_[i];
        if (format_ == ODC ? (c < '0' || c > '7') : xdigit_value(c) < 0) return false;
    }
    return true;
}

bool CpioReader::fail(Error *error, ErrorCode code, const char *what)
{
    set_error(error, code, std::string("Bad archive: ") + what);
    state_ = State::Failed;
    return false;
}

/* A callback's answer; one that refuses without saying why gets a reason. */
bool CpioReader::answer(bool ok, Error &local, Error *error)
{
    if (ok) return true;
    if (!local.set) local.assign(ErrorCode::Refused, "Archive entry refused");
    propagate(error, local);
    state_ = State::Failed;
    return false;
}

bool CpioReader::end_entry(Error *error)
{
    Error local;
    if (cb_.end && !answer(cb_.end(local), local, error)) return false;
    state_ = pad_ ? State::Pad : State::Header;
    have_ = 0;
    return true;
}

/* A header and name are in: check the entry and start it. */
bool CpioReader::begin_entry(Error *error)
{
    size_t hl = header_len();
    const char *name = reinterpret_cast<const char *>(buf_) + hl;
    if (name[namesize_ - 1] != '\0' || std::memchr(name, '\0', namesize_ - 1))
        return fail(error, ErrorCode::Invalid, "a name isn't NUL-terminated");
    std::string path(name, namesize_ - 1);
    if (path == TRAILER) {
        state_ = State::Done;
        return true;
    }
    if (path.empty()) return fail(error, ErrorCode::Invalid, "an empty name");
    if (!utf8_valid(path.data(), path.size())) return fail(error, ErrorCode::Invalid, "a name isn't UTF-8");
    uint64_t mode = field(F_MODE), size = field(F_FILESIZE);
    CpioType type;
    if ((mode & MODE_TYPE) == MODE_DIR) {
        if (size) return fail(error, ErrorCode::Invalid, "a directory with data");
        type = CpioType::Dir;
    } else if ((mode & MODE_TYPE) == MODE_FILE) {
        /* A file with other links is a hard link, whose data may be in
         * another entry. */
        if (field(F_NLINK) > 1) return fail(error, ErrorCode::Refused, "a hard-linked file");
        type = CpioType::File;
    } else {
        return fail(error, ErrorCode::Refused, "a link, device or other special file");
    }
    if (escapes(path)) return fail(error, ErrorCode::Refused, "a path outside the archive");
    if (++entries_ > CPIO_MAX_ENTRIES) return fail(error, ErrorCode::Limit, "too many entries");

    Error local;
    if (cb_.begin && !answer(cb_.begin(path, type, size, local), local, error)) return false;
    left_ = size;
    pad_ = pad_to_4(format_, size);
    state_ = State::Data;
    return size ? true : end_entry(error);
}

/* Moves up to `want` bytes into the header buffer; true once it holds them. */
bool CpioReader::collect(const uint8_t *buf, size_t len, size_t &pos, size_t want)
{
    size_t n = std::min(len - pos, want - have_);
    std::memcpy(buf_ + have_, buf + pos, n);
    have_ += n;
    pos += n;
    return have_ == want;
}

bool CpioReader::feed(const uint8_t *buf, size_t len, bool *done, Error *error)
{
    size_t pos = 0;
    bool ok = true;
    if (state_ == State::Failed) {
        set_error(error, ErrorCode::Invalid, "The archive failed earlier");
        ok = false;
    }
    while (ok && pos < len && state_ != State::Done) {
        switch (state_) {
        case State::Header: {
            if (have_ < 6) {
                if (!collect(buf, len, pos, 6)) break;
                int f = std::memcmp(buf_, "070707", 6) == 0 ? ODC : std::memcmp(buf_, "070701", 6) == 0 ? NEWC : 0;
                if (!f) {
                    ok = fail(error, ErrorCode::Invalid, "not an odc or newc archive");
                    break;
                }
                if (format_ && f != format_) {
                    ok = fail(error, ErrorCode::Invalid, "its format changes midway");
                    break;
                }
                format_ = f;
            }
            if (!collect(buf, len, pos, header_len())) break;
            if (!digits_ok()) {
                ok = fail(error, ErrorCode::Invalid, "a header field isn't a number");
                break;
            }
            uint64_t namesize = field(F_NAMESIZE);
            if (namesize == 0) {
                ok = fail(error, ErrorCode::Invalid, "an entry without a name");
                break;
            }
            if (namesize > CPIO_MAX_NAME) {
                ok = fail(error, ErrorCode::Limit, "a name too long");
                break;
            }
            namesize_ = static_cast<size_t>(namesize);
            want_ = header_len() + namesize_ + pad_to_4(format_, header_len() + namesize_);
            state_ = State::Name;
            break;
        }
        case State::Name:
            if (collect(buf, len, pos, want_)) ok = begin_entry(error);
            break;
        case State::Data: {
            auto n = static_cast<size_t>(std::min(static_cast<uint64_t>(len - pos), left_));
            if (cb_.data) {
                Error local;
                ok = answer(cb_.data(buf + pos, n, local), local, error);
                if (!ok) break;
            }
            pos += n;
            left_ -= n;
            if (!left_) ok = end_entry(error);
            break;
        }
        case State::Pad: {
            size_t n = std::min(len - pos, static_cast<size_t>(pad_));
            pos += n;
            pad_ -= static_cast<unsigned>(n);
            if (!pad_) state_ = State::Header;
            break;
        }
        case State::Done:
        case State::Failed:
            break;
        }
    }
    if (done) *done = state_ == State::Done;
    return ok;
}

bool CpioReader::finish(Error *error)
{
    if (state_ == State::Done) return true;
    if (state_ == State::Failed)
        set_error(error, ErrorCode::Invalid, "The archive failed earlier");
    else
        fail(error, ErrorCode::Invalid, "it ends before its trailer");
    return false;
}

} // namespace lares::airdrop
