// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* HTTP/1.1 framing for AirDrop's requests and responses (§6.1), as
 * src/airdrop/http.c, strict enough to face anyone in radio range (SEC-7,
 * RFC 9112). Every line of a head and of chunked framing ends in CRLF, and
 * a bare CR or LF is refused; header names are tokens and obsolete line
 * folding is refused. A message carries Content-Length or
 * Transfer-Encoding: chunked, never both and neither twice; Content-Length
 * is plain digits and a chunk size plain hex. Chunk extensions are skipped
 * within a line's limit, and trailers are checked and dropped. Bodies go
 * straight from the input to the caller, so nothing holds one whole.
 *
 * Ported from owly's HTTP framing (MIT). */
#include "airdrop_text.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lares::airdrop {

namespace {

constexpr unsigned MAX_HEADERS = 64;      /* fields in a head */
constexpr size_t MAX_CHUNK_LINE = 1024;   /* a chunk-size line with its extensions, without CRLF */

/* RFC 9110's tchar: what a method or a header name is made of. */
bool is_tchar(uint8_t c)
{
    return ascii_alnum(c) || (c && std::strchr("!#$%&'*+-.^_`|~", c));
}

bool is_space(uint8_t c)
{
    return c == ' ' || c == '\t';
}

/* No control character but a tab. */
bool is_text(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        auto c = static_cast<uint8_t>(s[i]);
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

bool parse_request_line(const char *s, size_t len, std::string &method, std::string &path, const char *&what)
{
    const char *end = s + len;
    const char *sp1 = static_cast<const char *>(std::memchr(s, ' ', len));
    const char *sp2 = sp1 ? static_cast<const char *>(std::memchr(sp1 + 1, ' ', static_cast<size_t>(end - sp1 - 1)))
                          : nullptr;
    if (!sp2 || std::memchr(sp2 + 1, ' ', static_cast<size_t>(end - sp2 - 1))) {
        what = "a bad request line";
        return false;
    }
    auto mlen = static_cast<size_t>(sp1 - s), tlen = static_cast<size_t>(sp2 - sp1 - 1),
         vlen = static_cast<size_t>(end - sp2 - 1);
    what = "a bad method";
    if (!mlen) return false;
    for (size_t i = 0; i < mlen; i++)
        if (!is_tchar(static_cast<uint8_t>(s[i]))) return false;
    /* Only the origin form: a path from the root. */
    what = "a bad request target";
    if (!tlen || sp1[1] != '/') return false;
    for (size_t i = 0; i < tlen; i++) {
        auto c = static_cast<uint8_t>(sp1[1 + i]);
        if (c < 0x21 || c > 0x7e) return false;
    }
    what = "not HTTP/1.1";
    if (vlen != 8 || std::memcmp(sp2 + 1, "HTTP/1.1", 8) != 0) return false;
    method.assign(s, mlen);
    path.assign(sp1 + 1, tlen);
    return true;
}

bool parse_status_line(const char *s, size_t len, unsigned &status, const char *&what)
{
    what = "not HTTP/1.1";
    if (len < 12 || std::memcmp(s, "HTTP/1.1 ", 9) != 0) return false;
    const char *code = s + 9;
    what = "a bad status line";
    if (code[0] < '1' || code[0] > '5' || !ascii_digit(code[1]) || !ascii_digit(code[2])
        || (len > 12 && code[3] != ' ') || !is_text(s + 12, len - 12))
        return false;
    status = static_cast<unsigned>((code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0'));
    return true;
}

enum class Body { None, Length, Chunked, ToEof };

} // namespace

HttpParser::HttpParser(HttpKind kind, HttpCallbacks cb) : kind_(kind), cb_(std::move(cb)) {}

void HttpParser::reset()
{
    state_ = State::Head;
    line_.clear();
    headers_.clear();
    left_ = 0;
    trailer_ = 0;
}

bool HttpParser::fail(Error *error, ErrorCode code, const char *what)
{
    set_error(error, code, std::string("Bad HTTP message: ") + what);
    state_ = State::Failed;
    return false;
}

/* A callback's answer; one that refuses without saying why gets a reason. */
bool HttpParser::answer(bool ok, Error &local, Error *error)
{
    if (ok) return true;
    if (!local.set) local.assign(ErrorCode::Refused, "HTTP message refused");
    propagate(error, local);
    state_ = State::Failed;
    return false;
}

bool HttpParser::finish_message(Error *error)
{
    state_ = State::Done;
    if (!cb_.done) return true;
    Error local;
    return answer(cb_.done(local), local, error);
}

/* A field line, "name: value", into a lowercase name and a trimmed value. */
bool HttpParser::parse_field(const char *s, size_t len, std::string *name, std::string *value, Error *error)
{
    if (len && is_space(static_cast<uint8_t>(s[0]))) return fail(error, ErrorCode::Invalid, "obsolete line folding");
    const char *colon = static_cast<const char *>(std::memchr(s, ':', len));
    if (!colon) return fail(error, ErrorCode::Invalid, "a header without a colon");
    auto n = static_cast<size_t>(colon - s);
    if (!n) return fail(error, ErrorCode::Invalid, "a bad header name");
    for (size_t i = 0; i < n; i++)
        if (!is_tchar(static_cast<uint8_t>(s[i]))) return fail(error, ErrorCode::Invalid, "a bad header name");
    const char *v = colon + 1, *end = s + len;
    while (v < end && is_space(static_cast<uint8_t>(*v))) v++;
    while (end > v && is_space(static_cast<uint8_t>(end[-1]))) end--;
    auto vlen = static_cast<size_t>(end - v);
    if (!is_text(v, vlen) || !utf8_valid(v, vlen)) return fail(error, ErrorCode::Invalid, "a bad header value");
    if (name) *name = ascii_down(s, n);
    if (value) value->assign(v, vlen);
    return true;
}

/* How the body is framed, refusing whatever two readers could take two
 * ways (SEC-7). A response that says "go on" (1xx), or has nothing to send
 * (204, 304), has no body whatever its headers say (RFC 9112, §6.3); any
 * other response with neither framing runs to the connection's end. The
 * head is in, ending in an empty line: parse it, hand it on and set up the
 * body. */
bool HttpParser::parse_head(Error *error)
{
    const char *s = line_.data(), *end = s + line_.size() - 2;   /* up to the empty line's CRLF */
    const char *eol = static_cast<const char *>(std::memchr(s, '\r', static_cast<size_t>(end - s)));
    std::string method, path;
    unsigned status = 0, lengths = 0, encodings = 0, fields = 0;
    const char *what = nullptr;
    bool ok = kind_ == HttpKind::Request
                  ? parse_request_line(s, static_cast<size_t>(eol - s), method, path, what)
                  : parse_status_line(s, static_cast<size_t>(eol - s), status, what);
    if (!ok) fail(error, ErrorCode::Invalid, what);
    headers_.clear();
    for (s = eol + 2; ok && s < end; s = eol + 2) {
        eol = static_cast<const char *>(std::memchr(s, '\r', static_cast<size_t>(end - s)));
        if (++fields > MAX_HEADERS) {
            ok = fail(error, ErrorCode::Limit, "too many headers");
            break;
        }
        std::string name, value;
        ok = parse_field(s, static_cast<size_t>(eol - s), &name, &value, error);
        if (!ok) break;
        if (name == "content-length") lengths++;
        if (name == "transfer-encoding") encodings++;
        /* A repeated field reads as one, its values joined (RFC 9110). */
        auto before = headers_.find(name);
        if (before != headers_.end()) before->second += ", " + value;
        else headers_.emplace(std::move(name), std::move(value));
    }
    line_.clear();

    Body body = Body::None;
    if (ok && !(kind_ == HttpKind::Response && (status < 200 || status == 204 || status == 304))) {
        if (lengths && encodings) {
            ok = fail(error, ErrorCode::Invalid, "both Content-Length and Transfer-Encoding");
        } else if (lengths > 1) {
            ok = fail(error, ErrorCode::Invalid, "a repeated Content-Length");
        } else if (encodings > 1) {
            ok = fail(error, ErrorCode::Invalid, "a repeated Transfer-Encoding");
        } else if (lengths) {
            const std::string &v = headers_["content-length"];
            bool digits = v.size() >= 1 && v.size() <= 19;   /* 19 digits always fit 64 bits */
            for (size_t i = 0; digits && i < v.size(); i++) digits = ascii_digit(v[i]);
            if (!digits) {
                ok = fail(error, ErrorCode::Invalid, "a bad Content-Length");
            } else {
                left_ = 0;
                for (char c : v) left_ = left_ * 10 + static_cast<uint64_t>(c - '0');
                body = left_ ? Body::Length : Body::None;
            }
        } else if (encodings) {
            if (!ascii_case_equal(headers_["transfer-encoding"], std::string("chunked")))
                ok = fail(error, ErrorCode::Invalid, "a Transfer-Encoding other than chunked");
            else
                body = Body::Chunked;
        } else if (kind_ == HttpKind::Response) {
            body = Body::ToEof;
        }
    }

    if (ok && cb_.head) {
        Error local;
        ok = answer(cb_.head(method, path, status, headers_, local), local, error);
    }
    if (!ok) return false;
    switch (body) {
    case Body::None: return finish_message(error);
    case Body::Length: state_ = State::Length; break;
    case Body::Chunked: state_ = State::ChunkSize; break;
    case Body::ToEof: state_ = State::UntilEof; break;
    }
    return true;
}

/* Adds one byte to the line being collected, refusing a bare CR or LF.
 * `eol` turns true at a line's CRLF. */
bool HttpParser::line_byte(uint8_t c, size_t max, bool &eol, Error *error)
{
    size_t n = line_.size();
    uint8_t prev = n ? static_cast<uint8_t>(line_[n - 1]) : 0;
    if (c == '\n' && prev != '\r') return fail(error, ErrorCode::Invalid, "a bare LF");
    if (prev == '\r' && c != '\n') return fail(error, ErrorCode::Invalid, "a bare CR");
    if (n >= max)
        return fail(error, ErrorCode::Limit,
                    state_ == State::Head      ? "a head too large"
                    : state_ == State::Trailer ? "a trailer too large"
                                               : "a chunk line too long");
    line_ += static_cast<char>(c);
    eol = c == '\n';
    return true;
}

bool HttpParser::feed_head(const uint8_t *buf, size_t len, size_t &pos, Error *error)
{
    while (pos < len) {
        bool eol = false;
        if (!line_byte(buf[pos++], HTTP_MAX_HEAD, eol, error)) return false;
        if (!eol) continue;
        const char *d = line_.data();
        size_t n = line_.size();
        if (n == 2) return fail(error, ErrorCode::Invalid, "an empty start line");
        if (n >= 4 && d[n - 4] == '\r' && d[n - 3] == '\n') return parse_head(error);
    }
    return true;
}

/* `chunk-size [ BWS ";" chunk-ext ]`: 1 to 16 hex digits, then nothing or
 * extensions, which are skipped. */
bool HttpParser::feed_chunk_size(const uint8_t *buf, size_t len, size_t &pos, Error *error)
{
    while (pos < len) {
        bool eol = false;
        if (!line_byte(buf[pos++], MAX_CHUNK_LINE + 2, eol, error)) return false;
        if (!eol) continue;
        const char *s = line_.data();
        size_t n = line_.size() - 2, digits = 0;
        uint64_t size = 0;
        while (digits < n && xdigit_value(static_cast<uint8_t>(s[digits])) >= 0)
            size = size << 4 | static_cast<uint64_t>(xdigit_value(static_cast<uint8_t>(s[digits++])));
        size_t ext = digits;
        while (ext < n && is_space(static_cast<uint8_t>(s[ext]))) ext++;
        if (!digits || digits > 16 || (ext < n && s[ext] != ';') || !is_text(s, n))
            return fail(error, ErrorCode::Invalid, "a bad chunk size");
        line_.clear();
        left_ = size;
        state_ = size ? State::ChunkData : State::Trailer;
        return true;
    }
    return true;
}

/* Trailer fields are checked like headers, then dropped; together they
 * count against HTTP_MAX_HEAD. */
bool HttpParser::feed_trailer(const uint8_t *buf, size_t len, size_t &pos, Error *error)
{
    while (pos < len) {
        bool eol = false;
        if (trailer_ >= HTTP_MAX_HEAD) return fail(error, ErrorCode::Limit, "a trailer too large");
        if (!line_byte(buf[pos++], HTTP_MAX_HEAD, eol, error)) return false;
        trailer_++;
        if (!eol) continue;
        size_t n = line_.size() - 2;
        if (n && !parse_field(line_.data(), n, nullptr, nullptr, error)) return false;
        line_.clear();
        if (!n) return finish_message(error);
    }
    return true;
}

bool HttpParser::give_body(const uint8_t *buf, size_t len, Error *error)
{
    if (!cb_.body) return true;
    Error local;
    return answer(cb_.body(buf, len, local), local, error);
}

bool HttpParser::feed(const uint8_t *buf, size_t len, size_t *used, Error *error)
{
    size_t pos = 0;
    bool ok = true;
    if (state_ == State::Failed) {
        set_error(error, ErrorCode::Invalid, "The HTTP message failed earlier");
        ok = false;
    }
    while (ok && pos < len && state_ != State::Done) {
        switch (state_) {
        case State::Head:
            ok = feed_head(buf, len, pos, error);
            break;
        case State::Length:
        case State::ChunkData: {
            auto n = static_cast<size_t>(std::min(static_cast<uint64_t>(len - pos), left_));
            ok = give_body(buf + pos, n, error);
            if (!ok) break;
            pos += n;
            left_ -= n;
            if (left_) break;
            if (state_ == State::Length) ok = finish_message(error);
            else state_ = State::ChunkCr;
            break;
        }
        case State::ChunkCr:
        case State::ChunkLf:
            if (buf[pos++] != (state_ == State::ChunkCr ? '\r' : '\n'))
                ok = fail(error, ErrorCode::Invalid, "chunk data not followed by CRLF");
            else
                state_ = state_ == State::ChunkCr ? State::ChunkLf : State::ChunkSize;
            break;
        case State::ChunkSize:
            ok = feed_chunk_size(buf, len, pos, error);
            break;
        case State::Trailer:
            ok = feed_trailer(buf, len, pos, error);
            break;
        case State::UntilEof:
            ok = give_body(buf + pos, len - pos, error);
            pos = len;
            break;
        case State::Done:
        case State::Failed:
            break;
        }
    }
    if (used) *used = pos;
    return ok;
}

/* The connection closed. A response that runs to the end is complete; a
 * parser between messages has nothing to finish; anything else was cut
 * short. */
bool HttpParser::eof(Error *error)
{
    switch (state_) {
    case State::Done:
        return true;
    case State::Head:
        if (line_.empty()) return true;
        break;
    case State::UntilEof:
        return finish_message(error);
    case State::Failed:
        set_error(error, ErrorCode::Invalid, "The HTTP message failed earlier");
        return false;
    default:
        break;
    }
    return fail(error, ErrorCode::Invalid, "the connection closed midway");
}

Bytes http_chunk(const uint8_t *buf, size_t len)
{
    static const char LAST[] = "0\r\n\r\n";
    if (!len) return Bytes(LAST, LAST + 5);
    char head[24];
    int n = std::snprintf(head, sizeof head, "%zx\r\n", len);
    Bytes out;
    out.reserve(static_cast<size_t>(n) + len + 2);
    out.insert(out.end(), head, head + n);
    out.insert(out.end(), buf, buf + len);
    out.push_back('\r');
    out.push_back('\n');
    return out;
}

} // namespace lares::airdrop
