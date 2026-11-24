// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* mDNS and DNS-SD for `_airdrop._tcp` on one link (SPEC §5), as
 * src/airdrop/mdns.c, ported from owly's proto/mdns.rs and driver/mdns.rs
 * (MIT): a strict DNS message codec, the four records a receiver publishes
 * (MDNS-1), a responder that repeats them in bursts while published
 * (MDNS-7) and answers queries, and a browser whose cache reports a service
 * once its SRV and AAAA resolve (MDNS-6) and forgets it when it's withdrawn
 * or stops being refreshed (SEC-11). Everything a peer sends is hostile
 * until checked.
 *
 * The Linux file owns its socket and timer; here the caller does, and
 * hands in each datagram and the time, so the same decisions come out of
 * the same inputs. */
#include "airdrop_text.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lares::airdrop {

namespace {

constexpr size_t MAX_QUERY = 1400;   /* a query of ours, so it needs no fragments */

constexpr uint16_t TYPE_PTR = 12;
constexpr uint16_t TYPE_TXT = 16;
constexpr uint16_t TYPE_AAAA = 28;
constexpr uint16_t TYPE_SRV = 33;
constexpr uint16_t TYPE_NSEC = 47;
constexpr uint16_t TYPE_ANY = 255;
constexpr uint16_t CLASS_IN = 1;
constexpr uint16_t CLASS_ANY = 255;
/* The class's top bit: cache-flush on a record, unicast-response (QU) on a
 * question. */
constexpr uint16_t CLASS_TOP = 0x8000;

/* RFC 6762 §10: host records live 120 s, others 75 minutes; a legacy
 * querier's answers 10 s at most (§6.7). */
constexpr uint32_t HOST_TTL = 120;
constexpr uint32_t OTHER_TTL = 4500;
constexpr uint32_t LEGACY_TTL = 10;

constexpr int64_t SECOND_US = 1000000;

/* Most instances and hosts the browser tracks, so a flood of announcements
 * can't grow the cache without bound, and the longest a record is kept,
 * whatever TTL it claims: RFC 6762's TTL for host records. The browser
 * queries at least every 10 s, so a peer that's still there refreshes its
 * records long before (SEC-11). A goodbye means gone in a second (§10.1). */
constexpr size_t MAX_CACHE = 256;
constexpr int64_t MAX_TTL_US = 120 * SECOND_US;
constexpr int64_t GOODBYE_US = SECOND_US;

/* The browser's first query 20 ms after it starts, then one a second for
 * as long as it browses, without the usual doubling: Lares browses only
 * while a send waits, and over a fixed radio a phone hears a query only in
 * the slot it spends on our channel, about once a second, which the AWDL
 * link holds each one for. A query that backed off to 10 s would let a
 * phone that missed the first ones wait that long to be found. */
constexpr int64_t FIRST_QUERY_US = 20 * 1000;
constexpr int64_t QUERY_INTERVAL_US = SECOND_US;
constexpr int64_t MAX_QUERY_INTERVAL_US = SECOND_US;

/* The burst owly-radio asks for when the radio is fixed to one channel,
 * as Lares' AWDL link is (AWDL-12): the phone visits it for a slot or two
 * in sixteen, so a copy goes every 16 TU, one availability window, for a
 * whole 16-slot period, then 3 s of quiet (OWLY-1). The goodbye goes in one
 * such burst too. A radio sharing an access point's channel would want 3
 * copies 50 ms apart instead. */
constexpr unsigned BURST_COUNT = 64;
constexpr int64_t BURST_INTERVAL_US = 16400;
constexpr int64_t BURST_PAUSE_US = 3 * SECOND_US;

/* Replies to queries, at most this many a second: a peer flooding queries
 * doesn't make the responder flood the link. */
constexpr unsigned MAX_REPLIES = 20;

/* ---- Addresses, as glibc's inet_pton() and inet_ntop() write them ---- */

bool pton4(const char *src, const char *end, uint8_t *dst)
{
    uint8_t tmp[4] = {};
    size_t at = 0;
    unsigned octets = 0;
    bool saw_digit = false;
    while (src < end) {
        char ch = *src++;
        if (ch >= '0' && ch <= '9') {
            unsigned v = tmp[at] * 10u + static_cast<unsigned>(ch - '0');
            if (saw_digit && tmp[at] == 0) return false;
            if (v > 255) return false;
            tmp[at] = static_cast<uint8_t>(v);
            if (!saw_digit) {
                if (++octets > 4) return false;
                saw_digit = true;
            }
        } else if (ch == '.' && saw_digit) {
            if (octets == 4) return false;
            tmp[++at] = 0;
            saw_digit = false;
        } else {
            return false;
        }
    }
    if (octets < 4) return false;
    std::memcpy(dst, tmp, 4);
    return true;
}

bool pton6(const char *src, const char *end, uint8_t dst[16])
{
    uint8_t tmp[16] = {};
    size_t tp = 0, colonp = SIZE_MAX;
    if (src == end) return false;
    /* A leading "::" needs its own check. */
    if (*src == ':') {
        ++src;
        if (src == end || *src != ':') return false;
    }
    const char *curtok = src;
    size_t xdigits_seen = 0;
    unsigned val = 0;
    while (src < end) {
        char ch = *src++;
        int digit = xdigit_value(static_cast<unsigned char>(ch));
        if (digit >= 0) {
            if (xdigits_seen == 4) return false;
            val = val << 4 | static_cast<unsigned>(digit);
            if (val > 0xffff) return false;
            ++xdigits_seen;
            continue;
        }
        if (ch == ':') {
            curtok = src;
            if (xdigits_seen == 0) {
                if (colonp != SIZE_MAX) return false;
                colonp = tp;
                continue;
            }
            if (src == end) return false;
            if (tp + 2 > 16) return false;
            tmp[tp++] = static_cast<uint8_t>(val >> 8);
            tmp[tp++] = static_cast<uint8_t>(val);
            xdigits_seen = 0;
            val = 0;
            continue;
        }
        if (ch == '.' && tp + 4 <= 16 && pton4(curtok, end, tmp + tp)) {
            tp += 4;
            xdigits_seen = 0;
            break;
        }
        return false;
    }
    if (xdigits_seen > 0) {
        if (tp + 2 > 16) return false;
        tmp[tp++] = static_cast<uint8_t>(val >> 8);
        tmp[tp++] = static_cast<uint8_t>(val);
    }
    if (colonp != SIZE_MAX) {
        /* "::" can't stand for no group at all. */
        if (tp == 16) return false;
        size_t n = tp - colonp;
        std::memmove(tmp + 16 - n, tmp + colonp, n);
        std::memset(tmp + colonp, 0, 16 - n - colonp);
        tp = 16;
    }
    if (tp != 16) return false;
    std::memcpy(dst, tmp, 16);
    return true;
}

std::string ntop6(const uint8_t a[16])
{
    unsigned words[8];
    for (int i = 0; i < 8; i++) words[i] = static_cast<unsigned>(a[2 * i] << 8 | a[2 * i + 1]);
    /* The longest run of zero groups, the first of equals, if two or more. */
    int best_base = -1, best_len = 0, cur_base = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (words[i] == 0) {
            if (cur_base == -1) {
                cur_base = i;
                cur_len = 1;
            } else {
                cur_len++;
            }
        } else if (cur_base != -1) {
            if (best_base == -1 || cur_len > best_len) {
                best_base = cur_base;
                best_len = cur_len;
            }
            cur_base = -1;
        }
    }
    if (cur_base != -1 && (best_base == -1 || cur_len > best_len)) {
        best_base = cur_base;
        best_len = cur_len;
    }
    if (best_base != -1 && best_len < 2) best_base = -1;
    std::string out;
    char b[24];
    for (int i = 0; i < 8; i++) {
        if (best_base != -1 && i >= best_base && i < best_base + best_len) {
            if (i == best_base) out += ':';
            continue;
        }
        if (i != 0) out += ':';
        /* An IPv4 address inside, compatible or mapped. */
        if (i == 6 && best_base == 0 && (best_len == 6 || (best_len == 5 && words[5] == 0xffff))) {
            std::snprintf(b, sizeof b, "%u.%u.%u.%u", unsigned{ a[12] }, unsigned{ a[13] }, unsigned{ a[14] },
                          unsigned{ a[15] });
            out += b;
            break;
        }
        std::snprintf(b, sizeof b, "%x", words[i]);
        out += b;
    }
    if (best_base != -1 && best_base + best_len == 8) out += ':';
    return out;
}

bool link_local(const uint8_t a[16])
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

/* ---- Names ---- */

/* A domain name in wire form without its final zero: length-prefixed
 * labels, 255 bytes at most with that zero (RFC 1035 §3.1). Labels never
 * hold a zero byte, so the bytes double as a C string, and comparing them
 * without regard to ASCII case compares the names (RFC 6762 §16). */
struct Name {
    uint8_t len;
    char b[256];
};

const Name SERVICE = { 20, "\x08_airdrop\x04_tcp\x05local" };
const Name LOCAL = { 6, "\x05local" };

/* Adds a label of valid UTF-8 with no zero byte, if the name has room. */
bool name_add(Name &n, const char *label, size_t len)
{
    if (len == 0 || len > 63 || n.len + 1 + len + 1 > 255 || !utf8_valid(label, len)) return false;
    n.b[n.len] = static_cast<char>(len);
    std::memcpy(n.b + n.len + 1, label, len);
    n.len = static_cast<uint8_t>(n.len + 1 + len);
    n.b[n.len] = 0;
    return true;
}

bool name_append(Name &n, const Name &suffix)
{
    if (n.len + suffix.len + 1 > 255) return false;
    std::memcpy(n.b + n.len, suffix.b, suffix.len + 1u);
    n.len = static_cast<uint8_t>(n.len + suffix.len);
    return true;
}

bool name_equal(const Name &a, const Name &b)
{
    return a.len == b.len && ascii_case_equal(a.b, b.b, a.len);
}

/* The name in wire form with its final zero, uncompressed. */
size_t name_wire(const Name &n, uint8_t *out)
{
    std::memcpy(out, n.b, n.len);
    out[n.len] = 0;
    return n.len + 1u;
}

std::string name_dotted(const Name &n)
{
    std::string s;
    for (unsigned i = 0; i < n.len; i += 1u + static_cast<uint8_t>(n.b[i])) {
        s.append(n.b + i + 1, static_cast<uint8_t>(n.b[i]));
        s += '.';
    }
    if (s.empty()) s = ".";
    return s;
}

/* The instance label, if the name is `<instance>._airdrop._tcp.local`. */
bool instance_label(const Name &n, std::string &out)
{
    if (n.len == 0) return false;
    unsigned l = static_cast<uint8_t>(n.b[0]);
    if (1u + l + SERVICE.len != n.len || !ascii_case_equal(n.b + 1 + l, SERVICE.b, SERVICE.len)) return false;
    out.assign(n.b + 1, l);
    return true;
}

/* ---- Parsing ---- */

uint16_t get16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

uint32_t get32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8
           | p[3];
}

/* A name at `pos`, following compression pointers, within the first `len`
 * bytes. A pointer must point strictly before the run of labels it ends,
 * so pointers can't loop, and never into the header; the name is capped at
 * 255 bytes. `end` is where the name ends in place. */
bool read_name(const uint8_t *p, size_t len, size_t pos, Name &out, size_t &end)
{
    size_t floor = pos, after = 0;
    out.len = 0;
    out.b[0] = 0;
    for (;;) {
        if (pos >= len) return false;
        unsigned l = p[pos];
        if (l == 0) {
            end = after ? after : pos + 1;
            return true;
        } else if (l <= 63) {
            if (pos + 1 + l > len || !name_add(out, reinterpret_cast<const char *>(p) + pos + 1, l)) return false;
            pos += 1 + l;
        } else if ((l & 0xc0) == 0xc0) {
            if (pos + 1 >= len) return false;
            size_t target = static_cast<size_t>(l & 0x3f) << 8 | p[pos + 1];
            if (target >= floor || target < 12) return false;
            if (!after) after = pos + 2;
            pos = floor = target;
        } else {
            return false;   /* the extended label types, 0x40 and 0x80 */
        }
    }
}

enum Section { QUESTION, ANSWER, AUTHORITY, ADDITIONAL };

struct Record {
    Section section;
    Name name;
    uint16_t type, cls;   /* the class without its top bit */
    bool top;             /* cache-flush on a record, QU on a question */
    uint32_t ttl;
    Name target;          /* PTR, SRV */
    uint16_t port;        /* SRV */
    unsigned flags;       /* TXT: its `flags` key (MDNS-4), or 0 */
    uint8_t addr[16];     /* AAAA */
};

/* The value of a TXT string `flags=<decimal>`; keys ignore case (RFC 6763
 * §6.4). */
bool txt_flags(const uint8_t *s, size_t len, unsigned &flags)
{
    if (len < 7 || len > 16 || !ascii_case_equal(reinterpret_cast<const char *>(s), "flags=", 6)) return false;
    uint64_t v = 0;
    for (size_t i = 6; i < len; i++) {
        if (!ascii_digit(s[i])) return false;
        v = v * 10 + static_cast<unsigned>(s[i] - '0');
    }
    if (v > UINT32_MAX) return false;
    flags = static_cast<unsigned>(v);
    return true;
}

bool read_record(const uint8_t *p, size_t len, size_t &at, Record &r)
{
    size_t pos = 0, stop = 0;
    if (!read_name(p, len, at, r.name, pos) || pos + 10 > len) return false;
    r.type = get16(p + pos);
    uint16_t cls = get16(p + pos + 2);
    r.cls = static_cast<uint16_t>(cls & ~CLASS_TOP);
    r.top = (cls & CLASS_TOP) != 0;
    r.ttl = get32(p + pos + 4);
    size_t start = pos + 10, end = start + get16(p + pos + 8);
    if (end > len) return false;
    switch (r.type) {
    case TYPE_PTR:
        if (!read_name(p, end, start, r.target, stop) || stop != end) return false;
        break;
    case TYPE_SRV:
        if (end - start < 7) return false;
        r.port = get16(p + start + 4);
        if (!read_name(p, end, start + 6, r.target, stop) || stop != end) return false;
        break;
    case TYPE_TXT: {
        bool found = false;
        r.flags = 0;
        for (size_t i = start; i < end; i += 1u + p[i]) {
            if (i + 1 + p[i] > end) return false;
            if (!found) found = txt_flags(p + i + 1, p[i], r.flags);
        }
        break;
    }
    case TYPE_AAAA:
        if (end - start != 16) return false;
        std::memcpy(r.addr, p + start, 16);
        break;
    default:
        break;
    }
    at = end;
    return true;
}

/* Reads a whole message, handing each question and record to `visit`.
 * False for anything malformed: a visitor may have seen part of it. */
template <class Visit>
bool walk(const uint8_t *p, size_t len, uint16_t *id, Visit &&visit)
{
    if (len < 12 || len > MDNS_MAX_MESSAGE) return false;
    /* Standard queries and responses only, with no error (RFC 6762 §18.3,
     * §18.11). */
    if (get16(p + 2) & 0x780f) return false;
    unsigned counts[4] = { get16(p + 4), get16(p + 6), get16(p + 8), get16(p + 10) };
    /* Every question takes at least 5 bytes, every record 11. */
    if (counts[0] * 5 + (counts[1] + counts[2] + counts[3]) * 11 > len) return false;
    if (id) *id = get16(p);
    size_t pos = 12;
    Record r;
    for (unsigned i = 0; i < counts[0]; i++) {
        r = Record{};
        r.section = QUESTION;
        size_t end = 0;
        if (!read_name(p, len, pos, r.name, end) || end + 4 > len) return false;
        pos = end;
        r.type = get16(p + pos);
        uint16_t cls = get16(p + pos + 2);
        r.cls = static_cast<uint16_t>(cls & ~CLASS_TOP);
        r.top = (cls & CLASS_TOP) != 0;
        pos += 4;
        visit(r);
    }
    for (unsigned s = 1; s < 4; s++)
        for (unsigned i = 0; i < counts[s]; i++) {
            r = Record{};
            r.section = static_cast<Section>(s);
            if (!read_record(p, len, pos, r)) return false;
            visit(r);
        }
    return true;
}

void ignore(const Record &) {}

/* ---- Building ---- */

void put16(Bytes &o, unsigned v)
{
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void put32(Bytes &o, uint32_t v)
{
    put16(o, v >> 16);
    put16(o, v & 0xffff);
}

void put_name(Bytes &o, const Name &n)
{
    o.insert(o.end(), n.b, n.b + n.len + 1);
}

void put_header(Bytes &o, uint16_t id, bool response, unsigned qd, unsigned an, unsigned ar)
{
    put16(o, id);
    put16(o, response ? 0x8400 : 0);   /* a response is authoritative (§18.4) */
    put16(o, qd);
    put16(o, an);
    put16(o, 0);
    put16(o, ar);
}

void put_question(Bytes &o, const Name &n, unsigned type, unsigned cls)
{
    put_name(o, n);
    put16(o, type);
    put16(o, cls);
}

/* What this receiver publishes (MDNS-1). */
struct Published {
    std::string label;   /* the instance, the service ID */
    Name instance{}, host{};
    uint8_t addr[16] = {};
    uint16_t port = 0;
    unsigned flags = 0;
};

/* The records, as bits; NSEC answers a type the name doesn't have (RFC 6762
 * §6.1) and stands for A beside an AAAA (§6.2). */
enum : unsigned { R_PTR = 1, R_SRV = 2, R_TXT = 4, R_AAAA = 8, R_NSEC_HOST = 16, R_NSEC_INSTANCE = 32 };
constexpr unsigned R_ALL = R_PTR | R_SRV | R_TXT | R_AAAA;

unsigned count_bits(unsigned v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

void put_rr(Bytes &o, const Name &name, uint16_t type, bool flush, uint32_t ttl, const uint8_t *rd, size_t n)
{
    put_name(o, name);
    put16(o, type);
    put16(o, CLASS_IN | (flush ? CLASS_TOP : 0u));
    put32(o, ttl);
    put16(o, static_cast<unsigned>(n));
    o.insert(o.end(), rd, rd + n);
}

/* An NSEC record's data in mDNS's restricted form: the owner as the next
 * name, and window 0 of the type bitmap (RFC 6762 §6.1). */
size_t nsec_data(uint8_t *rd, const Name &owner, const uint16_t *types, size_t n)
{
    size_t at = name_wire(owner, rd);
    uint8_t map[32] = {};
    unsigned len = 0;
    for (size_t i = 0; i < n; i++) {
        map[types[i] / 8] = static_cast<uint8_t>(map[types[i] / 8] | 0x80 >> (types[i] % 8));
        len = std::max(len, types[i] / 8 + 1u);
    }
    rd[at++] = 0;
    rd[at++] = static_cast<uint8_t>(len);
    std::memcpy(rd + at, map, len);
    return at + len;
}

/* The chosen records, in a fixed order, with owly's TTLs capped at `cap`:
 * the PTR is shared, the others are ours alone and flush caches. */
void put_records(Bytes &o, const Published &p, unsigned which, uint32_t cap, bool flush)
{
    uint8_t rd[300];
    size_t n;
    if (which & R_PTR) {
        n = name_wire(p.instance, rd);
        put_rr(o, SERVICE, TYPE_PTR, false, std::min(OTHER_TTL, cap), rd, n);
    }
    if (which & R_SRV) {
        std::memset(rd, 0, 4);   /* priority and weight */
        rd[4] = static_cast<uint8_t>(p.port >> 8);
        rd[5] = static_cast<uint8_t>(p.port);
        n = 6 + name_wire(p.host, rd + 6);
        put_rr(o, p.instance, TYPE_SRV, flush, std::min(HOST_TTL, cap), rd, n);
    }
    if (which & R_TXT) {
        int l = std::snprintf(reinterpret_cast<char *>(rd) + 1, sizeof rd - 1, "flags=%u", p.flags);
        rd[0] = static_cast<uint8_t>(l);
        put_rr(o, p.instance, TYPE_TXT, flush, std::min(OTHER_TTL, cap), rd, 1u + static_cast<size_t>(l));
    }
    if (which & R_AAAA) put_rr(o, p.host, TYPE_AAAA, flush, std::min(HOST_TTL, cap), p.addr, 16);
    if (which & R_NSEC_HOST) {
        static const uint16_t types[] = { TYPE_AAAA, TYPE_NSEC };
        n = nsec_data(rd, p.host, types, 2);
        put_rr(o, p.host, TYPE_NSEC, flush, std::min(HOST_TTL, cap), rd, n);
    }
    if (which & R_NSEC_INSTANCE) {
        static const uint16_t types[] = { TYPE_TXT, TYPE_SRV, TYPE_NSEC };
        n = nsec_data(rd, p.instance, types, 3);
        put_rr(o, p.instance, TYPE_NSEC, flush, std::min(HOST_TTL, cap), rd, n);
    }
}

/* A response with the chosen answers and additional records, and for a
 * legacy querier its ID and questions (RFC 6762 §6.7). */
Bytes build_response(const Published &p, uint16_t id, const Bytes *questions, unsigned qd, unsigned answers,
                     unsigned additional, uint32_t cap, bool flush)
{
    Bytes o;
    o.reserve(512);
    put_header(o, id, true, qd, count_bits(answers), count_bits(additional));
    if (questions) o.insert(o.end(), questions->begin(), questions->end());
    put_records(o, p, answers, cap, flush);
    put_records(o, p, additional, cap, flush);
    return o;
}

/* Checks and keeps what a receiver publishes. The host may be given as
 * "name", "name.local" or "name.local.", the address with or without its
 * "%zone". A NUL in any of them is refused (it would end them on Linux). */
bool published_init(Published &p, const std::string &instance, const std::string &hostname,
                    const std::string &address, uint16_t port, unsigned flags)
{
    p = Published{};
    if (!name_add(p.instance, instance.data(), instance.size()) || !name_append(p.instance, SERVICE)) return false;
    p.label = instance;

    std::string h = hostname;
    if (!h.empty() && h.back() == '.') h.pop_back();
    if (h.size() > 6 && ascii_case_equal(h.data() + h.size() - 6, ".local", 6)) h.resize(h.size() - 6);
    bool ok = !h.empty();
    for (size_t s = 0; ok;) {
        size_t dot = h.find('.', s);
        ok = name_add(p.host, h.data() + s, (dot == std::string::npos ? h.size() : dot) - s);
        if (dot == std::string::npos) break;
        s = dot + 1;
    }
    if (!ok || !name_append(p.host, LOCAL)) return false;

    size_t al = std::min(address.find('%'), address.size());
    if (al >= 46) return false;   /* INET6_ADDRSTRLEN */
    if (!pton6(address.data(), address.data() + al, p.addr)) return false;
    p.port = port;
    p.flags = flags;
    return true;
}

/* ---- The browser's cache ---- */

struct Instance {
    std::string label;                  /* as first heard */
    bool has_ptr = false, has_srv = false, has_txt = false;
    int64_t ptr = 0, srv = 0, txt = 0;  /* when each expires */
    Name target{};                      /* SRV */
    uint16_t port = 0;
    unsigned flags = 0;                 /* TXT */
    /* What `found` last reported. */
    bool reported = false;
    Name r_target{};
    uint16_t r_port = 0;
    uint8_t r_addr[16] = {};
    unsigned r_flags = 0;
};

struct Host {
    uint8_t addr[16] = {};
    int64_t expires = MDNS_NEVER;
};

struct Event {
    bool found;
    MdnsService s;
};

/* When a record expires. A goodbye never lengthens what's there, so the
 * copies of a goodbye burst don't keep pushing its second back. */
int64_t expiry(uint32_t ttl, int64_t now, int64_t current)
{
    if (ttl) return now + std::min(static_cast<int64_t>(ttl) * SECOND_US, MAX_TTL_US);
    return std::min(current, now + GOODBYE_US);
}

/* Hands the events over once the cache is settled, so a callback may do
 * anything with it. */
void deliver(const std::vector<Event> &events, const MdnsCallbacks &cb)
{
    for (const Event &e : events) {
        if (e.found && cb.found) cb.found(e.s);
        else if (!e.found && cb.lost) cb.lost(e.s.instance);
    }
}

} // namespace

/* Instances and hosts by lowercase key. Linux keeps them in hash tables,
 * whose order is arbitrary; these are sorted, so the order of events in one
 * sweep and of questions in one query is the keys'. */
struct MdnsCache::State {
    std::map<std::string, Instance> instances;   /* lowercase label */
    std::map<std::string, Host> hosts;           /* lowercase name in wire form */
    std::string zone;

    /* The cache entry for an instance, if there's room for it; a goodbye
     * (`create` false) only finds one, as there's nothing to withdraw from
     * a service not cached, and a late copy mustn't bring one back. */
    Instance *instance_for(const std::string &label, bool create)
    {
        std::string key = ascii_down(label.data(), label.size());
        auto it = instances.find(key);
        if (it != instances.end()) return &it->second;
        if (!create || instances.size() >= MAX_CACHE) return nullptr;
        Instance &i = instances[key];
        i.label = label;
        return &i;
    }

    Host *host_for(const Name &n)
    {
        auto it = hosts.find(ascii_down(n.b, n.len));
        return it == hosts.end() ? nullptr : &it->second;
    }

    /* Takes in one record of a response: answers and additional records
     * count, the authority section is for probing. */
    void visit(const Record &r, int64_t now)
    {
        std::string label;
        Instance *i = nullptr;
        bool create = r.ttl != 0;
        if ((r.section != ANSWER && r.section != ADDITIONAL) || r.cls != CLASS_IN) return;
        switch (r.type) {
        case TYPE_PTR:
            if (!name_equal(r.name, SERVICE) || !instance_label(r.target, label)) return;
            i = instance_for(label, create);
            if (!i || (!create && !i->has_ptr)) return;
            i->ptr = expiry(r.ttl, now, i->has_ptr ? i->ptr : MDNS_NEVER);
            i->has_ptr = true;
            break;
        case TYPE_SRV:
            if (!instance_label(r.name, label)) return;
            i = instance_for(label, create);
            if (!i) return;
            /* A goodbye withdraws the record it names, data and all. */
            if (!create && (!i->has_srv || i->port != r.port || !name_equal(i->target, r.target))) return;
            i->srv = expiry(r.ttl, now, i->has_srv ? i->srv : MDNS_NEVER);
            i->has_srv = true;
            i->target = r.target;
            i->port = r.port;
            break;
        case TYPE_TXT:
            if (!instance_label(r.name, label)) return;
            i = instance_for(label, create);
            if (!i || (!create && !i->has_txt)) return;
            i->txt = expiry(r.ttl, now, i->has_txt ? i->txt : MDNS_NEVER);
            i->has_txt = true;
            i->flags = r.flags;
            break;
        case TYPE_AAAA: {
            /* AirDrop peers are reached on a link-local address (NET-2), so
             * no other address is ever kept. */
            if (!link_local(r.addr)) return;
            Host *h = host_for(r.name);
            if (h && !create && std::memcmp(h->addr, r.addr, 16) != 0) return;
            if (!h) {
                if (!create || hosts.size() >= MAX_CACHE) return;
                h = &hosts[ascii_down(r.name.b, r.name.len)];
                h->expires = MDNS_NEVER;
            }
            std::memcpy(h->addr, r.addr, 16);
            h->expires = expiry(r.ttl, now, h->expires);
            break;
        }
        default:
            break;
        }
    }

    /* Drops what has expired and works out what changed: a service
     * resolves when its PTR is alive and its SRV target has an address
     * (MDNS-6), and is lost once its PTR is gone. */
    void sweep(int64_t now, std::vector<Event> &events)
    {
        for (auto it = hosts.begin(); it != hosts.end();) {
            if (it->second.expires <= now) it = hosts.erase(it);
            else ++it;
        }
        for (auto it = instances.begin(); it != instances.end();) {
            Instance &i = it->second;
            if (i.has_ptr && i.ptr <= now) i.has_ptr = false;
            if (i.has_srv && i.srv <= now) i.has_srv = false;
            if (i.has_txt && i.txt <= now) i.has_txt = false;
            Host *h = i.has_ptr && i.has_srv ? host_for(i.target) : nullptr;
            if (h) {
                unsigned flags = i.has_txt ? i.flags : 0;
                if (!i.reported || i.r_port != i.port || i.r_flags != flags || i.r_target.len != i.target.len
                    || std::memcmp(i.r_target.b, i.target.b, i.target.len) != 0
                    || std::memcmp(i.r_addr, h->addr, 16) != 0) {
                    i.reported = true;
                    i.r_target = i.target;
                    i.r_port = i.port;
                    i.r_flags = flags;
                    std::memcpy(i.r_addr, h->addr, 16);
                    Event e{ true, {} };
                    e.s.instance = i.label;
                    e.s.host = name_dotted(i.target);
                    e.s.address = ntop6(h->addr) + (zone.empty() ? std::string() : "%" + zone);
                    e.s.port = i.port;
                    e.s.flags = flags;
                    events.push_back(std::move(e));
                }
            } else if (i.reported && !i.has_ptr) {
                i.reported = false;
                Event e{ false, {} };
                e.s.instance = i.label;
                events.push_back(std::move(e));
            }
            if (!i.has_ptr && !i.has_srv && !i.has_txt) it = instances.erase(it);
            else ++it;
        }
    }
};

MdnsCache::MdnsCache() : s_(std::make_unique<State>()) {}
MdnsCache::~MdnsCache() = default;

bool MdnsCache::feed(const uint8_t *msg, size_t len, const std::string &zone, int64_t now_us, const MdnsCallbacks &cb)
{
    if (!msg || !walk(msg, len, nullptr, ignore)) return false;
    if (!(msg[2] & 0x80)) return true;   /* a query: nothing to learn */
    State &c = *s_;
    if (c.zone != zone) c.zone = zone;
    walk(msg, len, nullptr, [&c, now_us](const Record &r) { c.visit(r, now_us); });
    std::vector<Event> events;
    c.sweep(now_us, events);
    deliver(events, cb);
    return true;
}

void MdnsCache::expire(int64_t now_us, const MdnsCallbacks &cb)
{
    std::vector<Event> events;
    s_->sweep(now_us, events);
    deliver(events, cb);
}

void MdnsCache::clear(const MdnsCallbacks &cb)
{
    std::vector<Event> events;
    for (const auto &entry : s_->instances)
        if (entry.second.reported) {
            Event e{ false, {} };
            e.s.instance = entry.second.label;
            events.push_back(std::move(e));
        }
    s_->instances.clear();
    s_->hosts.clear();
    deliver(events, cb);
}

int64_t MdnsCache::deadline() const
{
    int64_t next = MDNS_NEVER;
    for (const auto &entry : s_->instances) {
        const Instance &i = entry.second;
        if (i.has_ptr) next = std::min(next, i.ptr);
        if (i.has_srv) next = std::min(next, i.srv);
        if (i.has_txt) next = std::min(next, i.txt);
    }
    for (const auto &entry : s_->hosts) next = std::min(next, entry.second.expires);
    return next;
}

/* A browse query: the service's PTR, then what the cache still lacks, as
 * far as a query of MAX_QUERY bytes goes: SRV and TXT for an instance
 * (ANY), and the address of an SRV target. */
Bytes MdnsCache::browse_query() const
{
    Bytes o;
    o.reserve(256);
    put_header(o, 0, false, 0, 0, 0);
    put_question(o, SERVICE, TYPE_PTR, CLASS_IN);
    unsigned qd = 1;
    for (const auto &entry : s_->instances) {
        const Instance &i = entry.second;
        Name n{};
        if ((!i.has_srv || !i.has_txt) && name_add(n, i.label.data(), i.label.size()) && name_append(n, SERVICE)
            && o.size() + n.len + 5u <= MAX_QUERY) {
            put_question(o, n, TYPE_ANY, CLASS_IN);
            qd++;
        }
        if (i.has_srv && !s_->host_for(i.target) && o.size() + i.target.len + 5u <= MAX_QUERY) {
            put_question(o, i.target, TYPE_AAAA, CLASS_IN);
            qd++;
        }
    }
    o[4] = static_cast<uint8_t>(qd >> 8);
    o[5] = static_cast<uint8_t>(qd);
    return o;
}

Bytes mdns_build_announcement(const std::string &instance, const std::string &hostname, const std::string &address,
                              uint16_t port, unsigned flags, uint32_t ttl)
{
    Published p;
    if (!published_init(p, instance, hostname, address, port, flags)) return {};
    return build_response(p, 0, nullptr, 0, R_ALL, 0, ttl, true);
}

Bytes mdns_build_query()
{
    Bytes o;
    put_header(o, 0, false, 1, 0, 0);
    put_question(o, SERVICE, TYPE_PTR, CLASS_IN);
    return o;
}

/* ---- The responder and browser ---- */

struct Mdns::State {
    std::string zone;
    MdnsCallbacks cb, relay;
    Send send;
    bool closed = false;

    /* The responder: the records and when the next copy goes. */
    bool published = false;
    Published pub;
    Bytes announcement;
    int64_t announce_at = 0;
    unsigned sent = 0;
    /* The goodbye of records just withdrawn, while its burst lasts. */
    bool farewell = false;
    Bytes goodbye;
    Name farewell_instance{};
    int64_t farewell_at = 0;
    unsigned farewell_left = 0;
    /* Replies sent in the current second. */
    bool window_open = false;
    int64_t window = 0;
    unsigned replies = 0;

    /* The browser. */
    bool browsing = false;
    MdnsCache cache;
    int64_t query_at = 0, query_interval = 0;
};

namespace {

void send_to(Mdns::State &m, const Bytes &msg, const uint8_t *to, uint16_t port)
{
    if (m.send) m.send(msg.data(), msg.size(), to, port);
}

/* The next copy of a burst: from when this one was due, so the spacing
 * holds on average whatever the caller's latency, unless it's late by more
 * than a copy's spacing. */
int64_t next_copy(int64_t due, int64_t now, int64_t wait)
{
    return (now - due < BURST_INTERVAL_US ? due : now) + wait;
}

/* Starts a goodbye burst for the published records. */
void withdraw_published(Mdns::State &m, int64_t now)
{
    if (!m.published) return;
    if (m.farewell) send_to(m, m.goodbye, nullptr, MDNS_PORT);   /* the last copy of an earlier one */
    m.goodbye = build_response(m.pub, 0, nullptr, 0, R_ALL, 0, 0, true);
    m.farewell = true;
    m.farewell_instance = m.pub.instance;
    m.farewell_at = now;
    m.farewell_left = BURST_COUNT;
    m.published = false;
    m.announcement.clear();
}

/* What a query asks of the published records. */
struct Ask {
    unsigned questions = 0, answers = 0, additional = 0;
    bool want_ptr = false, known_ptr = false, all_qu = true;
    Bytes *echo = nullptr;   /* the questions again, for a legacy querier */
    unsigned echoed = 0;
};

void ask_visit(Ask &a, const Published &p, const Record &r)
{
    if (r.section == ANSWER) {
        /* RFC 6762 §7.1: skip what the asker already knows, with at least
         * half its TTL left. */
        if (r.type == TYPE_PTR && name_equal(r.name, SERVICE) && name_equal(r.target, p.instance)
            && r.ttl >= OTHER_TTL / 2)
            a.known_ptr = true;
        return;
    }
    if (r.section != QUESTION) return;
    a.questions++;
    if (!r.top) a.all_qu = false;
    if (a.echo && a.echoed < 16) {
        put_question(*a.echo, r.name, r.type, r.cls | (r.top ? CLASS_TOP : 0u));
        a.echoed++;
    }
    if (r.cls != CLASS_IN && r.cls != CLASS_ANY) return;
    bool any = r.type == TYPE_ANY;
    if (name_equal(r.name, SERVICE) && (any || r.type == TYPE_PTR)) a.want_ptr = true;
    if (name_equal(r.name, p.instance)) {
        if (any || r.type == TYPE_SRV) {
            a.answers |= R_SRV;
            a.additional |= R_AAAA;
        }
        if (any || r.type == TYPE_TXT) a.answers |= R_TXT;
        if (!any && r.type != TYPE_SRV && r.type != TYPE_TXT) a.answers |= R_NSEC_INSTANCE;
    }
    if (name_equal(r.name, p.host)) a.answers |= any || r.type == TYPE_AAAA ? R_AAAA : R_NSEC_HOST;
}

/* Answers a query that asks about the published service, to the group, or
 * to the asker when every question asks so (QU) or it's a legacy querier,
 * one not on port 5353 (RFC 6762 §5.4, §6.7). There's one responder per
 * link, so the reply goes at once (§6). */
void answer(Mdns::State &m, const uint8_t *msg, size_t n, const uint8_t *from, uint16_t from_port, bool legacy,
            int64_t now)
{
    Bytes echo;
    Ask a;
    a.echo = legacy ? &echo : nullptr;
    uint16_t id = 0;
    const Published &p = m.pub;
    if (!walk(msg, n, &id, [&a, &p](const Record &r) { ask_visit(a, p, r); }) || !a.questions) return;
    if (a.want_ptr && !a.known_ptr) {
        a.answers |= R_PTR;
        a.additional |= R_SRV | R_TXT | R_AAAA;
    }
    if (!a.answers) return;
    a.additional &= ~a.answers;
    if (((a.answers | a.additional) & R_AAAA) && !(a.answers & R_NSEC_HOST)) a.additional |= R_NSEC_HOST;

    if (!m.window_open || now - m.window >= SECOND_US) {
        m.window_open = true;
        m.window = now;
        m.replies = 0;
    }
    if (m.replies++ >= MAX_REPLIES) return;
    /* RFC 6762 §6.7: a legacy answer has no cache-flush bits and TTLs of
     * at most 10 s. */
    Bytes reply = build_response(m.pub, legacy ? id : 0, a.echo, a.echoed, a.answers, a.additional,
                                 legacy ? LEGACY_TTL : UINT32_MAX, !legacy);
    if (legacy || a.all_qu) send_to(m, reply, from, from_port);
    else send_to(m, reply, nullptr, MDNS_PORT);
}

} // namespace

Mdns::Mdns(std::string zone, MdnsCallbacks cb, Send send) : s_(std::make_unique<State>())
{
    State *m = s_.get();
    m->zone = std::move(zone);
    m->cb = std::move(cb);
    m->send = std::move(send);
    /* What the cache finds goes on to the caller, but never this receiver's
     * own service, and nothing once closed. */
    m->relay.found = [m](const MdnsService &service) {
        if (m->closed || !m->browsing) return;
        if (m->published && ascii_case_equal(service.instance, m->pub.label)) return;
        if (m->cb.found) m->cb.found(service);
    };
    m->relay.lost = [m](const std::string &instance) {
        if (m->closed) return;
        if (m->published && ascii_case_equal(instance, m->pub.label)) return;
        if (m->cb.lost) m->cb.lost(instance);
    };
}

Mdns::~Mdns() = default;

bool Mdns::publish(const std::string &instance, const std::string &hostname, const std::string &address,
                   uint16_t port, unsigned flags, int64_t now_us)
{
    State &m = *s_;
    if (m.closed) return false;
    Published p;
    if (!published_init(p, instance, hostname, address, port, flags)) {
        withdraw_published(m, now_us);
        return false;
    }
    /* Another service replaces this one: say goodbye to it first. The same
     * one changing needs no goodbye, as its records flush caches. */
    if (m.published && (!name_equal(m.pub.instance, p.instance) || !name_equal(m.pub.host, p.host)))
        withdraw_published(m, now_us);
    if (m.farewell && name_equal(m.farewell_instance, p.instance)) {
        m.farewell = false;
        m.goodbye.clear();
    }
    m.pub = p;
    m.published = true;
    m.announcement = build_response(p, 0, nullptr, 0, R_ALL, 0, UINT32_MAX, true);
    m.announce_at = now_us;
    m.sent = 0;
    return true;
}

void Mdns::withdraw(int64_t now_us)
{
    if (!s_->closed) withdraw_published(*s_, now_us);
}

void Mdns::browse(bool on, int64_t now_us)
{
    State &m = *s_;
    if (m.closed || on == m.browsing) return;
    if (on) {
        m.browsing = true;
        m.query_at = now_us + FIRST_QUERY_US;
        m.query_interval = QUERY_INTERVAL_US;
    } else {
        /* What was found is lost to the caller, who stops listening. */
        m.cache.clear(m.relay);
        m.browsing = false;
    }
}

void Mdns::on_message(const uint8_t *msg, size_t len, const uint8_t *from, uint16_t from_port, int64_t now_us)
{
    State &m = *s_;
    if (m.closed || !msg || !from) return;
    /* Only the link's own addresses: Linux checks the same as it reads. */
    if (!link_local(from)) return;
    /* Nothing of ours counts, however it came back. */
    if (m.published && std::memcmp(from, m.pub.addr, 16) == 0) return;
    if (len < 12) return;
    if (msg[2] & 0x80) {
        /* RFC 6762 §6: a response from any port but 5353 is ignored. */
        if (from_port == MDNS_PORT && m.browsing) m.cache.feed(msg, len, m.zone, now_us, m.relay);
    } else if (m.published) {
        answer(m, msg, len, from, from_port, from_port != MDNS_PORT, now_us);
    }
}

void Mdns::on_timer(int64_t now_us)
{
    State &m = *s_;
    if (m.closed) return;
    if (m.published && now_us >= m.announce_at) {
        send_to(m, m.announcement, nullptr, MDNS_PORT);
        m.sent++;
        m.announce_at = next_copy(m.announce_at, now_us, m.sent % BURST_COUNT ? BURST_INTERVAL_US : BURST_PAUSE_US);
    }
    if (m.farewell && now_us >= m.farewell_at) {
        send_to(m, m.goodbye, nullptr, MDNS_PORT);
        if (--m.farewell_left == 0) {
            m.farewell = false;
            m.goodbye.clear();
        } else {
            m.farewell_at = next_copy(m.farewell_at, now_us, BURST_INTERVAL_US);
        }
    }
    if (m.browsing) m.cache.expire(now_us, m.relay);
    if (m.browsing && !m.closed && now_us >= m.query_at) {
        send_to(m, m.cache.browse_query(), nullptr, MDNS_PORT);
        m.query_at = now_us + m.query_interval;
        m.query_interval = std::min(m.query_interval * 2, MAX_QUERY_INTERVAL_US);
    }
}

int64_t Mdns::deadline() const
{
    const State &m = *s_;
    if (m.closed) return MDNS_NEVER;
    int64_t next = MDNS_NEVER;
    if (m.published) next = std::min(next, m.announce_at);
    if (m.farewell) next = std::min(next, m.farewell_at);
    if (m.browsing) next = std::min(next, std::min(m.query_at, m.cache.deadline()));
    return next;
}

void Mdns::close()
{
    State &m = *s_;
    if (m.closed) return;
    if (m.published) send_to(m, build_response(m.pub, 0, nullptr, 0, R_ALL, 0, 0, true), nullptr, MDNS_PORT);
    if (m.farewell) send_to(m, m.goodbye, nullptr, MDNS_PORT);
    m.closed = true;
    m.published = m.browsing = false;
}

} // namespace lares::airdrop
