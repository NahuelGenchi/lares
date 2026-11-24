// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* Binary property lists, bplist00, the bodies of /Discover and /Ask
 * (HTTP-3, §6.2), as src/airdrop/bplist.c. A plist is the magic, its
 * objects one after another, a table of each object's offset, and a 32-byte
 * trailer: 5 unused bytes, the sort version, the width of an offset and of
 * an object reference, then the number of objects, the top object's index
 * and the table's offset, as 64-bit big-endian numbers. Containers hold
 * their children as references, indexes into the offset table, so one
 * object can be referenced many times, and a hostile plist can reference
 * its own ancestors.
 *
 * The reader accepts only bplist00, bounds-checks every offset, reference
 * and length, refuses reference cycles, caps nesting at 64 (SEC-6), and
 * counts every object and every byte of string and data it produces, a
 * shared object each time it is used, so a small plist can't expand into a
 * huge tree (SEC-3). The writer keeps a dict's order, shares nothing, and
 * lays the trailer out as Core Foundation's CFBinaryPList reads it.
 *
 * Ported from owly's plist reader and writer (MIT). */
#include "airdrop_text.h"
#include <cstring>
#include <set>

namespace lares::airdrop {

/* ------------------------------------------------------------ values */

Value Value::string(std::string s)
{
    Value v(Type::String);
    v.text_ = std::move(s);
    return v;
}

Value Value::integer(int64_t n)
{
    Value v(Type::Integer);
    v.number_ = n;
    return v;
}

Value Value::boolean(bool b)
{
    Value v(Type::Boolean);
    v.number_ = b ? 1 : 0;
    return v;
}

Value Value::data(Bytes d)
{
    Value v(Type::Data);
    v.bytes_ = std::move(d);
    return v;
}

Value Value::data(const void *p, size_t n)
{
    const auto *b = static_cast<const uint8_t *>(p);
    return data(n ? Bytes(b, b + n) : Bytes());
}

Value Value::real(double d)
{
    Value v(Type::Real);
    v.real_ = d;
    return v;
}

const Value *Value::find(const std::string &key) const
{
    if (type_ != Type::Dict) return nullptr;
    for (size_t i = 0; i < keys_.size(); i++)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

const Value *Value::find(const std::string &key, Type t) const
{
    const Value *v = find(key);
    return v && v->type_ == t ? v : nullptr;
}

Value &Value::add(std::string key, Value v)
{
    keys_.push_back(std::move(key));
    items_.push_back(std::move(v));
    return *this;
}

Value &Value::push(Value v)
{
    keys_.emplace_back();
    items_.push_back(std::move(v));
    return *this;
}

bool Value::operator==(const Value &o) const
{
    if (type_ != o.type_) return false;
    switch (type_) {
    case Type::Dict:
        return keys_ == o.keys_ && items_ == o.items_;
    case Type::Array:
        return items_ == o.items_;
    case Type::String:
        return text_ == o.text_;
    case Type::Integer:
    case Type::Boolean:
        return number_ == o.number_;
    case Type::Data:
        return bytes_ == o.bytes_;
    case Type::Real:
        return std::memcmp(&real_, &o.real_, sizeof real_) == 0;
    }
    return false;
}

/* ------------------------------------------------------------ common */

namespace {

constexpr char MAGIC[] = "bplist00";
constexpr size_t MAGIC_LEN = 8;
constexpr size_t TRAILER_LEN = 32;
constexpr unsigned MAX_DEPTH = 64;                       /* SEC-6; AirDrop's messages nest 4 deep */
constexpr size_t MAX_DECODED = 8 * BPLIST_MAX_BYTES;     /* bytes of strings and data, SEC-3 */

void bad(Error *error, ErrorCode code, const char *what)
{
    set_error(error, code, std::string("Bad property list: ") + what);
}

uint64_t read_be(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

/* ------------------------------------------------------------ reading */

struct Reader {
    const uint8_t *data = nullptr;
    size_t len = 0;
    unsigned offset_size = 0;
    unsigned ref_size = 0;
    uint64_t count = 0;           /* objects in the offset table */
    size_t table = 0;             /* the table's offset: objects end there */
    unsigned objects = 0;         /* objects decoded, a shared one each time */
    size_t produced = 0;          /* bytes of strings and data decoded */
    uint64_t path[MAX_DEPTH] = {};   /* the containers being decoded, outermost first */
    unsigned depth = 0;
};

/* Whether `len` bytes from `pos` lie inside the objects. */
bool span(const Reader &r, size_t pos, uint64_t len, Error *error)
{
    if (pos <= r.table && len <= r.table - pos) return true;
    bad(error, ErrorCode::Invalid, "an object runs past the end");
    return false;
}

/* Counts `n` more bytes of output against MAX_DECODED. */
bool produce(Reader &r, uint64_t n, Error *error)
{
    if (n <= MAX_DECODED - r.produced) {
        r.produced += static_cast<size_t>(n);
        return true;
    }
    bad(error, ErrorCode::Limit, "it decodes to too many bytes");
    return false;
}

/* An object's count, in the marker's low nibble, or with 0xF there in an
 * integer object right after the marker. */
bool read_count(const Reader &r, uint8_t info, size_t &pos, uint64_t &out, Error *error)
{
    if (info != 0x0f) {
        out = info;
        return true;
    }
    if (!span(r, pos, 1, error)) return false;
    uint8_t marker = r.data[pos];
    if (marker >> 4 != 0x1 || (marker & 0x0f) > 3) {
        bad(error, ErrorCode::Invalid, "a bad count");
        return false;
    }
    unsigned n = 1u << (marker & 0x0f);
    if (!span(r, pos + 1, n, error)) return false;
    uint64_t v = read_be(r.data + pos + 1, n);
    /* No count can be larger than the input it describes. */
    if (v > r.len) {
        bad(error, ErrorCode::Invalid, "a count runs past the end");
        return false;
    }
    pos += 1 + n;
    out = v;
    return true;
}

/* The `i`th of the references at `pos`, which span() has checked. */
bool read_ref(const Reader &r, size_t pos, uint64_t i, uint64_t &out, Error *error)
{
    uint64_t ref = read_be(r.data + pos + i * r.ref_size, r.ref_size);
    if (ref >= r.count) {
        bad(error, ErrorCode::Invalid, "a reference out of range");
        return false;
    }
    out = ref;
    return true;
}

/* Starts decoding container `index`, refusing a cycle and nesting past 64. */
bool enter(Reader &r, uint64_t index, Error *error)
{
    for (unsigned i = 0; i < r.depth; i++)
        if (r.path[i] == index) {
            bad(error, ErrorCode::Invalid, "a reference cycle");
            return false;
        }
    if (r.depth >= MAX_DEPTH) {
        bad(error, ErrorCode::Limit, "it nests too deeply");
        return false;
    }
    r.path[r.depth++] = index;
    return true;
}

bool read_object(Reader &r, uint64_t index, Value &out, bool &dropped, Error *error);

bool read_integer(const Reader &r, uint8_t info, size_t pos, Value &out, Error *error)
{
    if (info > 4) {
        bad(error, ErrorCode::Invalid, "a bad integer size");
        return false;
    }
    unsigned n = 1u << info;
    if (!span(r, pos, n, error)) return false;
    const uint8_t *p = r.data + pos;
    /* Integers of 1, 2 and 4 bytes are unsigned, of 8 signed. Core
     * Foundation writes 16 bytes for an unsigned value past INT64_MAX; as
     * in owly, only one that still fits a signed 64-bit integer is taken. */
    if (n <= 8) {
        out = Value::integer(static_cast<int64_t>(read_be(p, n)));
        return true;
    }
    uint64_t high = read_be(p, 8), low = read_be(p + 8, 8);
    if (high != 0 || low > static_cast<uint64_t>(INT64_MAX)) {
        bad(error, ErrorCode::Invalid, "an integer out of range");
        return false;
    }
    out = Value::integer(static_cast<int64_t>(low));
    return true;
}

bool read_real(const Reader &r, uint8_t info, size_t pos, Value &out, Error *error)
{
    if (info == 2) {
        if (!span(r, pos, 4, error)) return false;
        auto bits = static_cast<uint32_t>(read_be(r.data + pos, 4));
        float f;
        std::memcpy(&f, &bits, sizeof f);
        out = Value::real(f);
        return true;
    }
    if (info == 3) {
        if (!span(r, pos, 8, error)) return false;
        uint64_t bits = read_be(r.data + pos, 8);
        double d;
        std::memcpy(&d, &bits, sizeof d);
        out = Value::real(d);
        return true;
    }
    bad(error, ErrorCode::Invalid, "a bad real size");
    return false;
}

bool read_ascii(Reader &r, uint8_t info, size_t pos, Value &out, Error *error)
{
    uint64_t len;
    if (!read_count(r, info, pos, len, error) || !span(r, pos, len, error) || !produce(r, len, error)) return false;
    const uint8_t *p = r.data + pos;
    for (uint64_t i = 0; i < len; i++)
        /* A NUL would cut the string short wherever it's used. */
        if (p[i] == 0 || p[i] > 0x7f) {
            bad(error, ErrorCode::Invalid, "a bad byte in an ASCII string");
            return false;
        }
    out = Value::string(std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(len)));
    return true;
}

bool read_utf16(Reader &r, uint8_t info, size_t pos, Value &out, Error *error)
{
    uint64_t units;
    if (!read_count(r, info, pos, units, error) || !span(r, pos, units * 2, error)
        || !produce(r, units * 3, error))   /* a unit becomes at most 3 bytes of UTF-8 */
        return false;
    const uint8_t *p = r.data + pos;
    std::string s;
    s.reserve(static_cast<size_t>(units));
    for (uint64_t i = 0; i < units; i++) {
        uint32_t c = static_cast<uint32_t>(p[2 * i] << 8 | p[2 * i + 1]);
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < units) {
            uint32_t low = static_cast<uint32_t>(p[2 * i + 2] << 8 | p[2 * i + 3]);
            if (low >= 0xdc00 && low <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                i++;
            }
        }
        if (c == 0 || (c >= 0xd800 && c <= 0xdfff)) {
            bad(error, ErrorCode::Invalid, "bad UTF-16");
            return false;
        }
        utf8_append(s, c);
    }
    out = Value::string(std::move(s));
    return true;
}

bool read_array(Reader &r, uint64_t index, uint8_t info, size_t pos, Value &out, Error *error)
{
    uint64_t n;
    if (!read_count(r, info, pos, n, error) || !span(r, pos, n * r.ref_size, error) || !enter(r, index, error))
        return false;
    out = Value::array();
    for (uint64_t i = 0; i < n; i++) {
        uint64_t ref;
        bool dropped;
        Value child;
        if (!read_ref(r, pos, i, ref, error) || !read_object(r, ref, child, dropped, error)) return false;
        if (!dropped) out.push(std::move(child));
    }
    r.depth--;
    return true;
}

bool read_dict(Reader &r, uint64_t index, uint8_t info, size_t pos, Value &out, Error *error)
{
    uint64_t n;
    if (!read_count(r, info, pos, n, error) || !span(r, pos, 2 * n * r.ref_size, error) || !enter(r, index, error))
        return false;
    size_t values = pos + static_cast<size_t>(n * r.ref_size);
    std::set<std::string> seen;
    out = Value::dict();
    for (uint64_t i = 0; i < n; i++) {
        uint64_t ref;
        bool dropped;
        Value key;
        if (!read_ref(r, pos, i, ref, error) || !read_object(r, ref, key, dropped, error)) return false;
        if (dropped || !key.is(Value::Type::String)) {
            bad(error, ErrorCode::Invalid, "a dictionary key that isn't a string");
            return false;
        }
        if (!seen.insert(key.get_string()).second) {
            bad(error, ErrorCode::Invalid, "a dictionary key given twice");
            return false;
        }
        Value value;
        if (!read_ref(r, values, i, ref, error) || !read_object(r, ref, value, dropped, error)) return false;
        /* Dates and UIDs have no use in AirDrop's messages: dropped. */
        if (!dropped) out.add(key.get_string(), std::move(value));
    }
    r.depth--;
    return true;
}

/* Object `index`. A date or UID gives true with `dropped` set. */
bool read_object(Reader &r, uint64_t index, Value &out, bool &dropped, Error *error)
{
    dropped = false;
    if (++r.objects > BPLIST_MAX_OBJECTS) {
        bad(error, ErrorCode::Limit, "it holds too many objects");
        return false;
    }
    uint64_t offset = read_be(r.data + r.table + index * r.offset_size, r.offset_size);
    if (offset < MAGIC_LEN || offset >= r.table) {
        bad(error, ErrorCode::Invalid, "an object offset out of range");
        return false;
    }
    auto pos = static_cast<size_t>(offset);
    uint8_t marker = r.data[pos++];
    auto info = static_cast<uint8_t>(marker & 0x0f);
    switch (marker >> 4) {
    case 0x0:
        if (info == 0x8 || info == 0x9) {
            out = Value::boolean(info == 0x9);
            return true;
        }
        break;
    case 0x1:
        return read_integer(r, info, pos, out, error);
    case 0x2:
        return read_real(r, info, pos, out, error);
    case 0x3:
        if (info != 0x3) break;
        if (!span(r, pos, 8, error)) return false;
        dropped = true;
        return true;
    case 0x4: {
        uint64_t len;
        if (!read_count(r, info, pos, len, error) || !span(r, pos, len, error) || !produce(r, len, error))
            return false;
        out = Value::data(r.data + pos, static_cast<size_t>(len));
        return true;
    }
    case 0x5:
        return read_ascii(r, info, pos, out, error);
    case 0x6:
        return read_utf16(r, info, pos, out, error);
    case 0x8:
        if (info > 7) break;
        if (!span(r, pos, info + 1u, error)) return false;
        dropped = true;
        return true;
    case 0xa:
        return read_array(r, index, info, pos, out, error);
    case 0xd:
        return read_dict(r, index, info, pos, out, error);
    default:
        break;
    }
    bad(error, ErrorCode::Invalid, "an unsupported object");
    return false;
}

} // namespace

/* A plist's value (SEC-3, SEC-6). */
bool bplist_decode(const uint8_t *data, size_t len, Value &out, Error *error)
{
    if (len > BPLIST_MAX_BYTES) {
        bad(error, ErrorCode::Limit, "it is too large");
        return false;
    }
    if (len < MAGIC_LEN || std::memcmp(data, MAGIC, MAGIC_LEN) != 0) {
        bad(error, ErrorCode::Invalid, "not bplist00");
        return false;
    }
    if (len < MAGIC_LEN + 1 + TRAILER_LEN) {
        bad(error, ErrorCode::Invalid, "too short");
        return false;
    }
    const uint8_t *t = data + len - TRAILER_LEN;
    Reader r;
    r.data = data;
    r.len = len;
    r.offset_size = t[6];
    r.ref_size = t[7];
    r.count = read_be(t + 8, 8);
    uint64_t top = read_be(t + 16, 8);
    uint64_t table = read_be(t + 24, 8);
    size_t end = len - TRAILER_LEN;
    if (r.offset_size < 1 || r.offset_size > 8 || r.ref_size < 1 || r.ref_size > 8) {
        bad(error, ErrorCode::Invalid, "bad sizes in the trailer");
        return false;
    }
    if (r.count == 0 || top >= r.count || r.count > end) {
        bad(error, ErrorCode::Invalid, "a bad object count");
        return false;
    }
    /* The table sits between the objects and the trailer. */
    if (table <= MAGIC_LEN || table > end || r.count * r.offset_size > end - table) {
        bad(error, ErrorCode::Invalid, "the offset table out of range");
        return false;
    }
    r.table = static_cast<size_t>(table);
    Value value;
    bool dropped = false;
    if (!read_object(r, top, value, dropped, error)) return false;
    if (dropped) {
        bad(error, ErrorCode::Invalid, "the top object is a date or UID");
        return false;
    }
    out = std::move(value);
    return true;
}

/* ------------------------------------------------------------ writing */

namespace {

/* An object to write: a leaf or a container, or a dict's key. */
struct Flat {
    const Value *value;
    const std::string *key;
    size_t first;   /* a container's first reference in Writer::refs */
    size_t n;       /* and its references: n for an array, 2n for a dict */
};

struct Writer {
    std::vector<Flat> objects;   /* in output order */
    std::vector<uint64_t> refs;  /* object indexes */
    size_t least = 0;            /* the fewest bytes the objects can take */
};

/* Bytes an unsigned value needs: 1, 2, 4 or 8. */
unsigned int_size(uint64_t v)
{
    return v <= 0xff ? 1 : v <= 0xffff ? 2 : v <= 0xffffffffu ? 4 : 8;
}

void put_be(Bytes &out, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) out.push_back(static_cast<uint8_t>(v >> (8 * (n - 1 - i))));
}

/* Non-negative integers take the fewest of 1, 2, 4 or 8 bytes, which are
 * read as unsigned; negative ones always take 8. */
void put_int(Bytes &out, int64_t v)
{
    unsigned n = v < 0 ? 8 : int_size(static_cast<uint64_t>(v));
    out.push_back(static_cast<uint8_t>(0x10 | (n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : 3)));
    put_be(out, static_cast<uint64_t>(v), n);
}

/* A marker with its count, inline below 15. */
void put_head(Bytes &out, uint8_t kind, uint64_t n)
{
    out.push_back(static_cast<uint8_t>(kind << 4 | (n < 0x0f ? n : 0x0f)));
    if (n >= 0x0f) put_int(out, static_cast<int64_t>(n));
}

void put_string(Bytes &out, const std::string &s)
{
    bool ascii = true;
    for (size_t i = 0; i < s.size() && ascii; i++) ascii = static_cast<unsigned char>(s[i]) < 0x80;
    if (ascii) {
        put_head(out, 0x5, s.size());
        out.insert(out.end(), s.begin(), s.end());
        return;
    }
    /* UTF-16, with surrogate pairs past U+FFFF; flatten() checked the UTF-8. */
    std::vector<uint16_t> units;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = utf8_next(s.data(), i);
        if (c < 0x10000) {
            units.push_back(static_cast<uint16_t>(c));
        } else {
            c -= 0x10000;
            units.push_back(static_cast<uint16_t>(0xd800 + (c >> 10)));
            units.push_back(static_cast<uint16_t>(0xdc00 + (c & 0x3ff)));
        }
    }
    put_head(out, 0x6, units.size());
    for (uint16_t u : units) put_be(out, u, 2);
}

/* What GVariant guarantees of a string on Linux. */
bool string_ok(const std::string &s, Error *error)
{
    if (utf8_valid(s.data(), s.size())) return true;
    bad(error, ErrorCode::Invalid, "a string that isn't UTF-8");
    return false;
}

bool push(Writer &w, const Value *value, const std::string *key, size_t least, size_t &index, Error *error)
{
    w.least += least;
    if (w.least > BPLIST_MAX_BYTES) {
        bad(error, ErrorCode::Limit, "it would be too large");
        return false;
    }
    index = w.objects.size();
    w.objects.push_back(Flat{ value, key, 0, 0 });
    return true;
}

/* Lists `value` and everything under it as objects, a container before its
 * children and a dict's keys before its values. A string counts as GVariant
 * sizes it, with its NUL, so the limits fall where they do on Linux. */
bool flatten(Writer &w, const Value &value, unsigned depth, size_t &index, Error *error)
{
    switch (value.type()) {
    case Value::Type::Boolean:
    case Value::Type::Integer:
    case Value::Type::Real:
        return push(w, &value, nullptr, 1, index, error);
    case Value::Type::String:
        return string_ok(value.get_string(), error)
               && push(w, &value, nullptr, 1 + value.get_string().size() + 1, index, error);
    case Value::Type::Data:
        return push(w, &value, nullptr, 1 + value.get_data().size(), index, error);
    case Value::Type::Array:
    case Value::Type::Dict:
        break;
    }
    bool dict = value.is(Value::Type::Dict);
    /* As deep as the reader takes: 64 containers, the top one first. */
    if (depth > MAX_DEPTH) {
        bad(error, ErrorCode::Limit, "it nests too deeply");
        return false;
    }
    size_t n = value.size();
    if (!push(w, &value, nullptr, 1 + (dict ? 2 * n : n), index, error)) return false;
    size_t me = index;
    size_t first = w.refs.size();
    w.refs.resize(first + (dict ? 2 * n : n));
    w.objects[me].first = first;
    w.objects[me].n = dict ? 2 * n : n;
    for (size_t i = 0; dict && i < n; i++) {
        const std::string &key = value.key(i);
        size_t k;
        if (!string_ok(key, error) || !push(w, nullptr, &key, 1 + key.size() + 1, k, error)) return false;
        w.refs[first + i] = k;
    }
    for (size_t i = 0; i < n; i++) {
        size_t c;
        if (!flatten(w, value.at(i), depth + 1, c, error)) return false;
        w.refs[first + (dict ? n : 0) + i] = c;
    }
    return true;
}

void put_object(const Writer &w, const Flat &f, unsigned ref_size, Bytes &out)
{
    if (f.key) {
        put_string(out, *f.key);
        return;
    }
    const Value &v = *f.value;
    switch (v.type()) {
    case Value::Type::Array:
    case Value::Type::Dict: {
        bool dict = v.is(Value::Type::Dict);
        put_head(out, dict ? 0xd : 0xa, dict ? f.n / 2 : f.n);
        for (size_t i = 0; i < f.n; i++) put_be(out, w.refs[f.first + i], ref_size);
        break;
    }
    case Value::Type::Boolean:
        out.push_back(static_cast<uint8_t>(v.get_boolean() ? 0x09 : 0x08));
        break;
    case Value::Type::Integer:
        put_int(out, v.get_integer());
        break;
    case Value::Type::Real: {
        double d = v.get_real();
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        out.push_back(0x23);
        put_be(out, bits, 8);
        break;
    }
    case Value::Type::Data:
        put_head(out, 0x4, v.get_data().size());
        out.insert(out.end(), v.get_data().begin(), v.get_data().end());
        break;
    case Value::Type::String:
        put_string(out, v.get_string());
        break;
    }
}

} // namespace

/* `value` as bplist00. */
bool bplist_encode(const Value &value, Bytes &out, Error *error)
{
    Writer w;
    size_t top;
    if (!flatten(w, value, 1, top, error)) return false;

    /* Core Foundation refuses a plist whose references or offsets are too
     * narrow to hold the object count or the table's offset themselves, so
     * both are sized to fit those, not the largest index. */
    size_t count = w.objects.size();
    unsigned ref_size = int_size(count);
    Bytes o;
    o.reserve(MAGIC_LEN + w.least + TRAILER_LEN);
    o.insert(o.end(), MAGIC, MAGIC + MAGIC_LEN);
    std::vector<uint64_t> offsets(count);
    for (size_t i = 0; i < count && o.size() <= BPLIST_MAX_BYTES; i++) {
        offsets[i] = o.size();
        put_object(w, w.objects[i], ref_size, o);
    }
    uint64_t table = o.size();
    unsigned offset_size = int_size(table);
    if (o.size() <= BPLIST_MAX_BYTES)
        for (size_t i = 0; i < count; i++) put_be(o, offsets[i], offset_size);
    o.insert(o.end(), size_t{ 6 }, uint8_t{ 0 });   /* 5 unused bytes and the sort version */
    o.push_back(static_cast<uint8_t>(offset_size));
    o.push_back(static_cast<uint8_t>(ref_size));
    put_be(o, count, 8);
    put_be(o, 0, 8);   /* the top object comes first */
    put_be(o, table, 8);
    if (o.size() > BPLIST_MAX_BYTES) {
        bad(error, ErrorCode::Limit, "it would be too large");
        return false;
    }
    out = std::move(o);
    return true;
}

} // namespace lares::airdrop
