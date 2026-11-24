// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* The two compressions of an /Upload body (UP-1), as src/airdrop/dvzip.c:
 * gzip (application/x-cpio), one or more members back to back, and DVZip
 * (application/x-dvzip, §6.6). DVZip is a run of chunks, each a 4-byte
 * big-endian length and that many bytes: one complete zlib stream, or, with
 * the length's top bit set, the archive's bytes as they are (DVZ-1). A
 * sender cuts the archive into 64 KiB pieces and compresses each on its
 * own, storing a piece zlib doesn't shrink (DVZ-2).
 *
 * Both directions run through zlib a bounded buffer at a time, so a small
 * body that inflates to gigabytes fails at `max_output` without ever being
 * held (SEC-3). zlib is called as GIO's zlib converters call it on Linux,
 * with the same buffers, flushes and errors, so the bytes and the decisions
 * are the same.
 *
 * Ported from owly's compression (MIT). */
#include "airdrop_text.h"
#include "zlib.h"
#include <algorithm>
#include <climits>
#include <cstring>

namespace lares::airdrop {

namespace {

constexpr size_t PIECE = 65536;                /* DVZ-2 */
constexpr uint32_t STORED = 0x80000000u;       /* DVZ-1: a length's top bit */
constexpr uint64_t MAX_CHUNK = 4u * 1024 * 1024;   /* DVZ-3: longest chunk taken, as owly's receiver */
constexpr size_t OUT_SIZE = 65536;             /* output per zlib call */
/* zlib's fastest level, as owly's sender uses: photos and videos, most of
 * what AirDrop carries, don't shrink at any level. */
constexpr int LEVEL = 1;
/* The gzip header's OS: Unix, what zlib writes on Linux. zlib on Windows
 * would write NTFS (10), and the bodies would differ by that byte. */
constexpr int GZIP_OS_UNIX = 3;

bool call_sink(const Sink &sink, const uint8_t *buf, size_t len, Error *error)
{
    Error local;
    if (!sink || sink(buf, len, local)) return true;
    if (!local.set) local.assign(ErrorCode::Refused, "Archive data refused");
    propagate(error, local);
    return false;
}

void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

/* One g_converter_convert() call on a GZlibDecompressor or GZlibCompressor:
 * what it returned, how much it read and wrote, and its error. */
enum class Conv { Converted, Finished, Error };

struct Step {
    Conv res = Conv::Error;
    size_t read = 0;
    size_t written = 0;
    bool partial = false;   /* G_IO_ERROR_PARTIAL_INPUT: no progress without more input */
    std::string message;
};

/* zlib takes 32-bit lengths; the loops around a step take the rest next. */
uInt clamp_len(size_t n)
{
    return n > UINT_MAX ? UINT_MAX : static_cast<uInt>(n);
}

/* After zlib's answer, as GIO reads it. */
Step finish_step(z_stream &s, int res, uInt in_len, size_t out_size)
{
    Step st;
    if (res == Z_DATA_ERROR || res == Z_NEED_DICT) {
        st.message = "Invalid compressed data";
    } else if (res == Z_MEM_ERROR) {
        st.message = "Not enough memory";
    } else if (res == Z_STREAM_ERROR) {
        st.message = std::string("Internal error: ") + (s.msg ? s.msg : "(null)");
    } else if (res == Z_BUF_ERROR) {
        st.partial = true;
        st.message = "Need more input";
    } else {
        st.read = in_len - s.avail_in;
        st.written = out_size - s.avail_out;
        st.res = res == Z_STREAM_END ? Conv::Finished : Conv::Converted;
    }
    return st;
}

Step inflate_step(z_stream &s, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size)
{
    uInt n = clamp_len(in_len);
    s.next_in = const_cast<Bytef *>(in);
    s.avail_in = n;
    s.next_out = out;
    s.avail_out = static_cast<uInt>(out_size);
    return finish_step(s, inflate(&s, Z_NO_FLUSH), n, out_size);
}

Step deflate_step(z_stream &s, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, bool at_end)
{
    uInt n = clamp_len(in_len);
    s.next_in = const_cast<Bytef *>(in);
    s.avail_in = n;
    s.next_out = out;
    s.avail_out = static_cast<uInt>(out_size);
    return finish_step(s, deflate(&s, at_end ? Z_FINISH : Z_NO_FLUSH), n, out_size);
}

} // namespace

/* ------------------------------------------------------------ inflating */

struct Inflater::Impl {
    enum class Part { Length, StoredData, Chunk };

    bool dvzip = false;
    uint64_t max_output = 0;
    uint64_t total = 0;       /* bytes out so far */
    Sink sink;
    z_stream z{};             /* gzip, or zlib for DVZip's chunks */
    bool zlib_ok = false;
    std::unique_ptr<uint8_t[]> out;
    Part part = Part::Length; /* DVZip: where in a chunk */
    uint8_t length[4] = {};
    unsigned have = 0;        /* bytes of `length` in */
    uint64_t left = 0;        /* bytes of the chunk still to come */
    bool ended = false;       /* gzip: a member is complete, and no other has begun */
    bool failed = false;

    bool bad(Error *error, ErrorCode code, const std::string &what)
    {
        set_error(error, code, std::string("Bad ") + (dvzip ? "DVZip" : "gzip") + " body: " + what);
        failed = true;
        return false;
    }

    /* Hands archive bytes on, within max_output (SEC-3). */
    bool emit(const uint8_t *buf, size_t len, Error *error)
    {
        if (!len) return true;
        if (len > max_output - total) return bad(error, ErrorCode::Limit, "it decompresses to too much");
        total += len;
        if (call_sink(sink, buf, len, error)) return true;
        failed = true;
        return false;
    }

    /* Runs the decompressor over `in` until it has taken all of it and has
     * no output left, or its stream has ended. */
    bool run(const uint8_t *in, size_t len, size_t &used, bool &stream_end, Error *error)
    {
        size_t pos = 0;
        bool full = false;
        stream_end = false;
        while (pos < len || full) {
            Step st = inflate_step(z, pos < len ? in + pos : nullptr, len - pos, out.get(), OUT_SIZE);
            if (st.res == Conv::Error) {
                /* No progress without more input: what's here is all taken. */
                bool wait = pos == len && st.partial;
                if (!wait) return bad(error, ErrorCode::Invalid, st.message);
                break;
            }
            pos += st.read;
            if (!emit(out.get(), st.written, error)) return false;
            full = st.written == OUT_SIZE;
            if (st.res == Conv::Finished) {
                stream_end = true;
                break;
            }
            if (!st.read && !st.written) break;
        }
        used = pos;
        return true;
    }

    /* gzip members one after another decode as one stream (RFC 1952,
     * §2.2), as owly takes them from iPhones. Whatever follows a member
     * must start another: anything else fails at its header, or at the end
     * if it's cut short. max_output counts across all of them. */
    bool feed_gzip(const uint8_t *buf, size_t len, Error *error)
    {
        size_t pos = 0;
        while (pos < len) {
            if (ended) {
                inflateReset(&z);
                ended = false;
            }
            size_t used = 0;
            if (!run(buf + pos, len - pos, used, ended, error)) return false;
            pos += used;
            if (!ended && pos < len) return bad(error, ErrorCode::Invalid, "a member that makes no progress");
        }
        return true;
    }

    bool feed_dvzip(const uint8_t *buf, size_t len, Error *error)
    {
        size_t pos = 0;
        while (pos < len) {
            if (part == Part::Length) {
                size_t n = std::min(len - pos, static_cast<size_t>(4 - have));
                std::memcpy(length + have, buf + pos, n);
                have += static_cast<unsigned>(n);
                pos += n;
                if (have < 4) break;
                uint32_t raw = static_cast<uint32_t>(length[0]) << 24 | static_cast<uint32_t>(length[1]) << 16
                               | static_cast<uint32_t>(length[2]) << 8 | length[3];
                have = 0;
                left = raw & ~STORED;
                /* DVZ-3: no empty chunk, and none past the limit. */
                if (!left) return bad(error, ErrorCode::Invalid, "an empty chunk");
                if (left > MAX_CHUNK) return bad(error, ErrorCode::Limit, "a chunk too long");
                part = raw & STORED ? Part::StoredData : Part::Chunk;
                if (part == Part::Chunk) inflateReset(&z);
            } else if (part == Part::StoredData) {
                auto n = static_cast<size_t>(std::min(static_cast<uint64_t>(len - pos), left));
                if (!emit(buf + pos, n, error)) return false;
                pos += n;
                left -= n;
                if (!left) part = Part::Length;
            } else {
                auto n = static_cast<size_t>(std::min(static_cast<uint64_t>(len - pos), left));
                size_t used = 0;
                bool stream_end = false;
                if (!run(buf + pos, n, used, stream_end, error)) return false;
                pos += used;
                left -= used;
                /* DVZ-3: each compressed chunk is exactly one complete zlib
                 * stream, however much it inflates to. */
                if (stream_end && left) return bad(error, ErrorCode::Invalid, "data after a chunk's zlib stream");
                if (!stream_end && (!left || used < n))
                    return bad(error, ErrorCode::Invalid, "a chunk that isn't one complete zlib stream");
                if (stream_end) part = Part::Length;
            }
        }
        return true;
    }
};

Inflater::Inflater(bool dvzip, uint64_t max_output, Sink sink) : z_(std::make_unique<Impl>())
{
    Impl &z = *z_;
    z.dvzip = dvzip;
    z.max_output = max_output;
    z.sink = std::move(sink);
    /* As GIO's decompressor: zlib's own window, + 16 for gzip. */
    z.zlib_ok = (dvzip ? inflateInit(&z.z) : inflateInit2(&z.z, MAX_WBITS + 16)) == Z_OK;
    z.out = std::make_unique<uint8_t[]>(OUT_SIZE);
}

Inflater::~Inflater()
{
    if (z_->zlib_ok) inflateEnd(&z_->z);
}

bool Inflater::feed(const uint8_t *buf, size_t len, Error *error)
{
    Impl &z = *z_;
    if (z.failed) {
        set_error(error, ErrorCode::Invalid, "The body failed earlier");
        return false;
    }
    if (!len) return true;
    if (!z.zlib_ok) return z.bad(error, ErrorCode::Invalid, "Not enough memory");
    return z.dvzip ? z.feed_dvzip(buf, len, error) : z.feed_gzip(buf, len, error);
}

/* The body has ended: it must end where a gzip member or a DVZip chunk
 * does. */
bool Inflater::finish(Error *error)
{
    Impl &z = *z_;
    if (z.failed) {
        set_error(error, ErrorCode::Invalid, "The body failed earlier");
        return false;
    }
    if (z.dvzip ? z.part == Impl::Part::Length && !z.have : z.ended) return true;
    return z.bad(error, ErrorCode::Invalid, "it is cut short");
}

/* ------------------------------------------------------------ deflating */

struct Deflater::Impl {
    bool dvzip = false;
    Sink sink;
    z_stream z{};
    gz_header head{};
    bool zlib_ok = false;
    std::unique_ptr<uint8_t[]> piece;   /* DVZip: the piece being filled */
    size_t have = 0;
    std::unique_ptr<uint8_t[]> out;     /* a chunk's length and bytes, or gzip's output */
    bool finished = false;
    bool failed = false;

    bool fail(Error *error, const std::string *message)
    {
        if (message) set_error(error, ErrorCode::Invalid, "Can't compress: " + *message);
        failed = true;
        return false;
    }

    /* One DVZip chunk: `piece` as its own zlib stream, or stored when that
     * isn't shorter (DVZ-1, DVZ-2). */
    bool put_chunk(const uint8_t *data, size_t len, Error *error)
    {
        deflateReset(&z);
        size_t in = 0, done = 0;
        bool shorter = false;
        /* Room for no more than the piece: a stream that fills it doesn't
         * shrink the piece. */
        while (done < len) {
            Step st = deflate_step(z, in < len ? data + in : nullptr, len - in, out.get() + 4 + done, len - done, true);
            if (st.res == Conv::Error) return fail(error, &st.message);
            in += st.read;
            done += st.written;
            if (st.res == Conv::Finished) {
                shorter = done < len;
                break;
            }
        }
        if (shorter) {
            put_be32(out.get(), static_cast<uint32_t>(done));
        } else {
            put_be32(out.get(), static_cast<uint32_t>(len) | STORED);
            std::memcpy(out.get() + 4, data, len);
            done = len;
        }
        return call_sink(sink, out.get(), 4 + done, error) || fail(error, nullptr);
    }

    /* Runs the gzip compressor over `buf`, or to its end with `last`. */
    bool run_gzip(const uint8_t *buf, size_t len, bool last, Error *error)
    {
        size_t pos = 0;
        for (;;) {
            Step st = deflate_step(z, pos < len ? buf + pos : nullptr, len - pos, out.get(), OUT_SIZE, last);
            if (st.res == Conv::Error) return fail(error, &st.message);
            pos += st.read;
            if (st.written && !call_sink(sink, out.get(), st.written, error)) return fail(error, nullptr);
            if (st.res == Conv::Finished || (!last && pos == len)) return true;
            if (!st.read && !st.written) {
                const std::string why = "no progress";
                return fail(error, &why);
            }
        }
    }
};

Deflater::Deflater(bool dvzip, Sink sink) : d_(std::make_unique<Impl>())
{
    Impl &d = *d_;
    d.dvzip = dvzip;
    d.sink = std::move(sink);
    if (dvzip) {
        d.zlib_ok = deflateInit(&d.z, LEVEL) == Z_OK;
        d.piece = std::make_unique<uint8_t[]>(PIECE);
    } else {
        d.zlib_ok = deflateInit2(&d.z, LEVEL, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        d.head.os = GZIP_OS_UNIX;
        if (d.zlib_ok) deflateSetHeader(&d.z, &d.head);
    }
    d.out = std::make_unique<uint8_t[]>(4 + std::max(PIECE, OUT_SIZE));
}

Deflater::~Deflater()
{
    if (d_->zlib_ok) deflateEnd(&d_->z);
}

bool Deflater::feed(const uint8_t *buf, size_t len, Error *error)
{
    Impl &d = *d_;
    if (d.failed || d.finished) {
        set_error(error, ErrorCode::Invalid, d.failed ? "Compression failed earlier" : "Compression is finished");
        return false;
    }
    if (!len) return true;
    if (!d.zlib_ok) {
        const std::string why = "Not enough memory";
        return d.fail(error, &why);
    }
    if (!d.dvzip) return d.run_gzip(buf, len, false, error);
    while (len) {
        size_t n = std::min(len, PIECE - d.have);
        std::memcpy(d.piece.get() + d.have, buf, n);
        d.have += n;
        buf += n;
        len -= n;
        if (d.have == PIECE) {
            d.have = 0;
            if (!d.put_chunk(d.piece.get(), PIECE, error)) return false;
        }
    }
    return true;
}

bool Deflater::finish(Error *error)
{
    Impl &d = *d_;
    if (d.failed || d.finished) {
        set_error(error, ErrorCode::Invalid, d.failed ? "Compression failed earlier" : "Compression is finished");
        return false;
    }
    if (!d.zlib_ok) {
        const std::string why = "Not enough memory";
        return d.fail(error, &why);
    }
    bool ok = d.dvzip ? !d.have || d.put_chunk(d.piece.get(), d.have, error) : d.run_gzip(nullptr, 0, true, error);
    d.have = 0;
    d.finished = ok;
    return ok;
}

} // namespace lares::airdrop
