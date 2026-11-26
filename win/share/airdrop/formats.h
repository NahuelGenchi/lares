// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* AirDrop's formats on Windows (issue #109), ported from src/airdrop/
 * formats.h and the mDNS half of discovery.h: binary property lists, CPIO
 * archives, gzip and DVZip compression, HTTP/1.1 framing, safe names, and
 * mDNS / DNS-SD, all on plain bytes with no socket and no clock of their own
 * (the CPIO writer's directory date aside). Rule IDs (UP-2, SEC-7, MDNS-6)
 * are those of owly's AirDrop specification, which docs/AIRDROP.md follows.
 * Every input from a peer is hostile until checked, so each decoder has
 * limits and fails cleanly.
 *
 * Nothing here throws: a call that fails returns false and fills an Error,
 * as the Linux code fills a GError, with the same category and message.
 * Nothing is thread-safe; each object belongs to one thread. */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lares::airdrop {

using Bytes = std::vector<uint8_t>;

/* ---- Errors ---- */

enum class ErrorCode {
    Invalid,   /* malformed input */
    Limit,     /* input over a limit */
    Refused,   /* well formed, but not allowed (a path, an entry type) */
    Failed,    /* not the input's fault: a sink's or callback's own (a full disk) */
};

/* GError's place. Every call that takes an `Error *` accepts nullptr. */
struct Error {
    bool set = false;
    ErrorCode code = ErrorCode::Invalid;
    std::string message;

    void assign(ErrorCode c, std::string m)
    {
        set = true;
        code = c;
        message = std::move(m);
    }
    void clear()
    {
        set = false;
        code = ErrorCode::Invalid;
        message.clear();
    }
};

/* Where bytes go: false, with `error` set or not, refuses them. One that
 * refuses without a reason gets one ("Archive data refused"). */
using Sink = std::function<bool(const uint8_t *buf, size_t len, Error &error)>;

/* One path component that is safe to show and to create in a folder: not
 * empty, not "." or "..", 255 bytes at most, UTF-8, no "/" or "\", no
 * control characters, and no bidi embeddings, overrides or isolates
 * (SEC-1). nullptr is not. */
bool airdrop_safe_name(const char *name);
bool airdrop_safe_name(const std::string &name);

/* The UTI a sent file is described with (FileType), by its extension,
 * after owly's table: "public.folder", "public.jpeg", ... or "public.data". */
const char *airdrop_uti(const std::string &name, bool is_directory);

/* ---- Binary property lists (bplist00), SPEC §6.2, SEC-3, SEC-6 ----
 *
 * Values are a dict (string keys, kept in order), an array, a string
 * (UTF-8), a 64-bit integer, a boolean, data and a real, as GVariant's
 * a{sv}, av, s, x, b, ay and d on Linux. Decoding accepts only bplist00,
 * caps nesting at 64 and the decoded size (shared references count each
 * time they're used), and drops dates and UIDs from dicts and arrays. */

constexpr size_t BPLIST_MAX_BYTES = 1024 * 1024;   /* encoded size */
constexpr unsigned BPLIST_MAX_OBJECTS = 65536;     /* decoded objects, references counted */

class Value {
public:
    enum class Type { Dict, Array, String, Integer, Boolean, Data, Real };

    Value() = default;   /* an empty dict */
    static Value dict() { return Value(Type::Dict); }
    static Value array() { return Value(Type::Array); }
    static Value string(std::string s);
    static Value integer(int64_t v);
    static Value boolean(bool v);
    static Value data(Bytes d);
    static Value data(const void *p, size_t n);
    static Value real(double v);

    Type type() const { return type_; }
    bool is(Type t) const { return type_ == t; }

    /* A leaf's value; another type's gives its zero. */
    const std::string &get_string() const { return text_; }
    int64_t get_integer() const { return type_ == Type::Integer ? number_ : 0; }
    bool get_boolean() const { return type_ == Type::Boolean && number_ != 0; }
    const Bytes &get_data() const { return bytes_; }
    double get_real() const { return type_ == Type::Real ? real_ : 0.0; }

    /* A dict's or an array's entries, in order; an array's keys are empty. */
    size_t size() const { return items_.size(); }
    const std::string &key(size_t i) const { return keys_[i]; }
    const Value &at(size_t i) const { return items_[i]; }
    Value &at(size_t i) { return items_[i]; }

    /* A dict's value for `key` (the first, were it given twice), or null,
     * as g_variant_lookup_value(): with a type, also null for another. */
    const Value *find(const std::string &key) const;
    const Value *find(const std::string &key, Type t) const;

    /* Appends to a dict, keeping the order given, or to an array. */
    Value &add(std::string key, Value v);
    Value &push(Value v);

    /* As g_variant_equal(): types, order and reals' bits all count. */
    bool operator==(const Value &o) const;
    bool operator!=(const Value &o) const { return !(*this == o); }

private:
    explicit Value(Type t) : type_(t) {}
    Type type_ = Type::Dict;
    int64_t number_ = 0;   /* an integer, or a boolean's 0 or 1 */
    double real_ = 0.0;
    std::string text_;
    Bytes bytes_;
    std::vector<std::string> keys_;
    std::vector<Value> items_;
};

bool bplist_decode(const uint8_t *data, size_t len, Value &out, Error *error = nullptr);
/* A string or key that isn't UTF-8, or holds a NUL, is refused (Invalid):
 * GVariant can't hold one on Linux. */
bool bplist_encode(const Value &value, Bytes &out, Error *error = nullptr);

/* Extracts lowercase SHA-256 hex hashes from ValidatedEmailHashes and
 * ValidatedPhoneHashes inside an Apple-signed SenderRecordData /
 * ReceiverRecordData CMS SignedData envelope (SPEC §7, ID-1 to ID-3).
 * Requires a valid CMS signature whose signer certificate chains exclusively
 * to Apple's embedded Root CAs; unsigned plists and untrusted or self-signed
 * CMS blobs are rejected. Never logs hashes (SEC-5). */
bool airdrop_extract_record_hashes(const uint8_t *data, size_t len, std::vector<std::string> &out_hashes);

/* Same CMS SignedData signature and exclusive CA chain verification against
 * explicit DER-encoded root CA certificates (for unit and loopback tests). */
bool airdrop_extract_record_hashes_with_roots(const uint8_t *data, size_t len,
                                              const std::vector<Bytes> &trusted_roots_der,
                                              std::vector<std::string> &out_hashes);

/* Verifies that a DER-encoded X.509 certificate chains cleanly to Apple's
 * embedded Root CAs using the same exclusive chain engine. */
bool airdrop_verify_apple_cert(const uint8_t *cert_der, size_t len);

/* ---- CPIO archives, UP-2 ----
 *
 * The writer emits odc (magic 070707); the reader accepts odc and newc
 * (070701). Both stream: nothing holds a whole file. */

class CpioWriter {
public:
    explicit CpioWriter(Sink sink);
    /* A directory, dated now. */
    bool dir(const std::string &path, Error *error = nullptr);
    /* A regular file of exactly `size` bytes, given next through file_data. */
    bool file(const std::string &path, uint64_t size, int64_t mtime, Error *error = nullptr);
    bool file_data(const uint8_t *buf, size_t len, Error *error = nullptr);
    bool finish(Error *error = nullptr);   /* TRAILER!!! */

private:
    bool ready(Error *error);
    bool put_header(const char *name, size_t namelen, unsigned mode, unsigned uid, unsigned gid, unsigned nlink,
                    unsigned ino, uint64_t mtime, uint64_t size, Error *error);
    Sink sink_;
    uint32_t ino_ = 1;     /* the next entry's */
    uint64_t left_ = 0;    /* bytes the current file still needs */
    bool finished_ = false;
    bool failed_ = false;
};

enum class CpioType { Dir, File };

struct CpioCallbacks {
    /* An entry's header; return false with an error to refuse it. The path
     * is the archive's, such as "./photo.jpg", and the reader has already
     * refused absolute paths, ".." and any type but these two. */
    std::function<bool(const std::string &path, CpioType type, uint64_t size, Error &error)> begin;
    std::function<bool(const uint8_t *buf, size_t len, Error &error)> data;
    std::function<bool(Error &error)> end;
};

constexpr unsigned CPIO_MAX_ENTRIES = 4096;
constexpr size_t CPIO_MAX_NAME = 1024;

class CpioReader {
public:
    explicit CpioReader(CpioCallbacks cb = {});
    /* Feed archive bytes in any pieces. *done turns true once TRAILER!!!
     * has been read (UP-11); bytes after it are ignored. */
    bool feed(const uint8_t *buf, size_t len, bool *done, Error *error = nullptr);
    /* The archive's bytes have ended: fails unless TRAILER!!! was read. */
    bool finish(Error *error = nullptr);

private:
    enum class State { Header, Name, Data, Pad, Done, Failed };
    bool fail(Error *error, ErrorCode code, const char *what);
    bool answer(bool ok, Error &local, Error *error);
    bool end_entry(Error *error);
    bool begin_entry(Error *error);
    bool collect(const uint8_t *buf, size_t len, size_t &pos, size_t want);
    uint64_t field(int which) const;
    bool digits_ok() const;
    size_t header_len() const;
    CpioCallbacks cb_;
    State state_ = State::Header;
    int format_ = 0;   /* the first entry's, which every entry must share */
    uint8_t buf_[110 + CPIO_MAX_NAME + 3] = {};   /* a header, its name and padding */
    size_t have_ = 0;
    size_t want_ = 0;   /* the header, name and padding together */
    size_t namesize_ = 0;
    uint64_t left_ = 0;   /* the entry's data still to come */
    unsigned pad_ = 0;    /* newc's padding after it */
    unsigned entries_ = 0;
};

/* ---- Upload compression: gzip (application/x-cpio) or DVZip
 * (application/x-dvzip), UP-1, §6.6, on zlib. ---- */

class Inflater {
public:
    /* Decompresses an /Upload body and hands the archive bytes to `sink`,
     * failing once more than `max_output` bytes come out (SEC-3). */
    Inflater(bool dvzip, uint64_t max_output, Sink sink);
    ~Inflater();
    Inflater(const Inflater &) = delete;
    Inflater &operator=(const Inflater &) = delete;
    bool feed(const uint8_t *buf, size_t len, Error *error = nullptr);
    /* The body has ended: it must end where a gzip member or a DVZip chunk
     * does. */
    bool finish(Error *error = nullptr);

    struct Impl;

private:
    std::unique_ptr<Impl> z_;
};

class Deflater {
public:
    /* Compresses archive bytes into an /Upload body: DVZip in 64 KiB
     * pieces, each stored when zlib doesn't shrink it (DVZ-1, DVZ-2), or
     * gzip. */
    Deflater(bool dvzip, Sink sink);
    ~Deflater();
    Deflater(const Deflater &) = delete;
    Deflater &operator=(const Deflater &) = delete;
    bool feed(const uint8_t *buf, size_t len, Error *error = nullptr);
    bool finish(Error *error = nullptr);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

/* ---- HTTP/1.1 framing, §6.1, SEC-7 ----
 *
 * A push parser for one message at a time, requests on the receiver's side
 * and responses on the sender's. Strict per RFC 9112: no conflicting or
 * duplicate Content-Length, not both it and chunked, hex chunk sizes only,
 * and a cap on header bytes. A message with neither is empty, except a
 * response, which then runs to the connection's end. */

enum class HttpKind { Request, Response };

/* Lowercase name to value; a repeated field's values are joined with ", ". */
using HttpHeaders = std::map<std::string, std::string>;

struct HttpCallbacks {
    /* Headers are complete. `method` and `path` are set for a request
     * (empty for a response), `status` for a response (0 for a request). */
    std::function<bool(const std::string &method, const std::string &path, unsigned status,
                       const HttpHeaders &headers, Error &error)> head;
    std::function<bool(const uint8_t *buf, size_t len, Error &error)> body;
    std::function<bool(Error &error)> done;
};

constexpr size_t HTTP_MAX_HEAD = 16 * 1024;

class HttpParser {
public:
    explicit HttpParser(HttpKind kind, HttpCallbacks cb = {});
    /* Consumes bytes up to the end of the current message and sets *used;
     * after `done`, call reset() for the next message on the connection,
     * and feed it the rest. */
    bool feed(const uint8_t *buf, size_t len, size_t *used, Error *error = nullptr);
    /* The connection closed: completes a response that runs to the end. */
    bool eof(Error *error = nullptr);
    void reset();

private:
    enum class State { Head, Length, ChunkSize, ChunkData, ChunkCr, ChunkLf, Trailer, UntilEof, Done, Failed };
    bool fail(Error *error, ErrorCode code, const char *what);
    bool answer(bool ok, Error &local, Error *error);
    bool finish_message(Error *error);
    bool parse_field(const char *s, size_t len, std::string *name, std::string *value, Error *error);
    bool parse_head(Error *error);
    bool line_byte(uint8_t c, size_t max, bool &eol, Error *error);
    bool feed_head(const uint8_t *buf, size_t len, size_t &pos, Error *error);
    bool feed_chunk_size(const uint8_t *buf, size_t len, size_t &pos, Error *error);
    bool feed_trailer(const uint8_t *buf, size_t len, size_t &pos, Error *error);
    bool give_body(const uint8_t *buf, size_t len, Error *error);
    HttpKind kind_;
    HttpCallbacks cb_;
    State state_ = State::Head;
    std::string line_;      /* the head, a chunk-size line or a trailer line so far */
    HttpHeaders headers_;
    uint64_t left_ = 0;     /* bytes of the body or chunk still to come */
    size_t trailer_ = 0;    /* trailer bytes so far */
};

/* Chunked transfer coding: one chunk, or the last ("0\r\n\r\n") for len 0. */
Bytes http_chunk(const uint8_t *buf, size_t len);

/* ---- mDNS / DNS-SD for `_airdrop._tcp` on the AWDL link, §5 ---- */

constexpr uint16_t MDNS_PORT = 5353;
constexpr size_t MDNS_MAX_MESSAGE = 9000;   /* RFC 6762 §17: anything longer is refused */
constexpr int64_t MDNS_NEVER = INT64_MAX;

struct MdnsService {
    std::string instance;   /* the service ID, 12 hex digits */
    std::string host;       /* SRV target, "name.local." */
    std::string address;    /* the AAAA address with its zone, "fe80::…%<zone>" */
    uint16_t port = 0;
    unsigned flags = 0;     /* TXT flags (MDNS-4), 0 when there's no TXT yet */
};

struct MdnsCallbacks {
    /* A service resolved (SRV and AAAA, MDNS-6), or changed. */
    std::function<void(const MdnsService &service)> found;
    /* Withdrawn, or not refreshed in time (SEC-11). */
    std::function<void(const std::string &instance)> lost;
};

/* The unsolicited announcement (RFC 6762 §8.3): PTR, SRV, TXT and AAAA with
 * owly's TTLs (PTR and TXT 4500 s, SRV and AAAA 120 s), each capped at
 * `ttl`, so 0 makes the goodbye (§10.1). The host may be "name",
 * "name.local" or "name.local.", the address may carry "%zone". Empty if an
 * argument is malformed: an instance or host label over 63 bytes, or an
 * address that isn't IPv6. */
Bytes mdns_build_announcement(const std::string &instance, const std::string &hostname, const std::string &address,
                              uint16_t port, unsigned flags, uint32_t ttl);
/* `_airdrop._tcp.local` PTR, asking for a multicast answer. */
Bytes mdns_build_query();

/* The browser's cache: calls `found`/`lost` for each `_airdrop._tcp`
 * service the messages fed to it complete. Times are microseconds on any
 * monotonic clock. */
class MdnsCache {
public:
    MdnsCache();
    ~MdnsCache();
    MdnsCache(const MdnsCache &) = delete;
    MdnsCache &operator=(const MdnsCache &) = delete;
    /* False for a malformed message, which changes nothing. `zone` is put
     * after the addresses it reports ("awdl0" on Linux). */
    bool feed(const uint8_t *msg, size_t len, const std::string &zone, int64_t now_us, const MdnsCallbacks &cb);
    void expire(int64_t now_us, const MdnsCallbacks &cb);
    /* Forgets everything, reporting each service found as lost. */
    void clear(const MdnsCallbacks &cb);
    /* When expire() next has something to do, MDNS_NEVER for never. */
    int64_t deadline() const;
    /* A browse query: the service's PTR, then what the cache still lacks. */
    Bytes browse_query() const;

    struct State;

private:
    std::unique_ptr<State> s_;
};

/* What airdrop_mdns_new/publish/browse and their timers do on Linux, without
 * the socket and the clock: the responder that repeats the published
 * records in bursts (MDNS-7), says goodbye and answers queries, and the
 * browser that queries and keeps the cache.
 *
 * The caller owns a UDP socket on [::]:5353 in ff02::fb on the AWDL link
 * alone, and a monotonic clock. It hands every datagram that arrived on
 * that link to on_message(), whole (a 9001-byte buffer is enough: anything
 * longer is refused), and calls on_timer() whenever the clock reaches
 * deadline(), which it reads again after every call into the engine. The
 * engine sends through `send`, which must not call back into it; `found`
 * and `lost` may call publish(), withdraw(), browse() or close(), but must
 * not destroy the engine. */
class Mdns {
public:
    /* One message: to ff02::fb port 5353 when `to` is null, else to the
     * 16-byte address `to` (on the same link) at `port`. */
    using Send = std::function<void(const uint8_t *msg, size_t len, const uint8_t *to, uint16_t port)>;

    /* `zone` is put after found addresses, as "fe80::…%<zone>". */
    Mdns(std::string zone, MdnsCallbacks cb, Send send);
    ~Mdns();
    Mdns(const Mdns &) = delete;
    Mdns &operator=(const Mdns &) = delete;

    /* Announce this receiver (MDNS-1), in bursts while published (MDNS-7).
     * Another instance or host replaces the one published, with a goodbye
     * for it. False, and withdrawn, for a malformed service. */
    bool publish(const std::string &instance, const std::string &hostname, const std::string &address,
                 uint16_t port, unsigned flags, int64_t now_us);
    /* Withdraws what's published, with a burst of goodbyes. */
    void withdraw(int64_t now_us);
    /* Ask for `_airdrop._tcp` services, repeatedly while on; off reports
     * every service found as lost. */
    void browse(bool on, int64_t now_us);
    /* A datagram from `from` (16 bytes) port `from_port`, as it arrived. */
    void on_message(const uint8_t *msg, size_t len, const uint8_t *from, uint16_t from_port, int64_t now_us);
    void on_timer(int64_t now_us);
    /* When on_timer() is next due, MDNS_NEVER for never. */
    int64_t deadline() const;
    /* Stops at once: what's published gets one goodbye, as nothing waits
     * for a burst, and no callback runs from here on. */
    void close();

    struct State;

private:
    std::unique_ptr<State> s_;
};

} // namespace lares::airdrop
