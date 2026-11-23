// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* Binary property lists, bplist00, the bodies of /Discover and /Ask
 * (HTTP-3, §6.2). A plist is the magic, its objects one after another, a
 * table of each object's offset, and a 32-byte trailer: 5 unused bytes, the
 * sort version, the width of an offset and of an object reference, then
 * the number of objects, the top object's index and the table's offset, as
 * 64-bit big-endian numbers. Containers hold their children as references,
 * indexes into the offset table, so one object can be referenced many
 * times, and a hostile plist can reference its own ancestors.
 *
 * The reader accepts only bplist00, bounds-checks every offset, reference
 * and length, refuses reference cycles, caps nesting at 64 (SEC-6), and
 * counts every object and every byte of string and data it produces, a
 * shared object each time it is used, so a small plist can't expand into a
 * huge tree (SEC-3). The writer keeps a dict's order, shares nothing, and
 * lays the trailer out as Core Foundation's CFBinaryPList reads it.
 *
 * Ported from owly's plist reader and writer (MIT). */
#include "formats.h"
#include <string.h>

#define MAGIC       "bplist00"
#define MAGIC_LEN   8
#define TRAILER_LEN 32
#define MAX_DEPTH   64                      /* SEC-6; AirDrop's messages nest 4 deep */
#define MAX_DECODED (8 * BPLIST_MAX_BYTES)  /* bytes of strings and data, SEC-3 */

static void set_error(GError **error, AirdropError code, const char *what)
{
    g_set_error(error, AIRDROP_ERROR, code, "Bad property list: %s", what);
}

static guint64 read_be(const guint8 *p, guint n)
{
    guint64 v = 0;
    for (guint i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

/* ------------------------------------------------------------ reading */

typedef struct {
    const guint8 *data;
    gsize len;
    guint offset_size;
    guint ref_size;
    guint64 count;            /* objects in the offset table */
    gsize table;              /* the table's offset: objects end there */
    guint objects;            /* objects decoded, a shared one each time */
    gsize produced;           /* bytes of strings and data decoded */
    guint64 path[MAX_DEPTH];  /* the containers being decoded, outermost first */
    guint depth;
} Reader;

/* Whether `len` bytes from `pos` lie inside the objects. */
static gboolean span(Reader *r, gsize pos, guint64 len, GError **error)
{
    if (pos <= r->table && len <= r->table - pos) return TRUE;
    set_error(error, AIRDROP_ERROR_INVALID, "an object runs past the end");
    return FALSE;
}

/* Counts `n` more bytes of output against MAX_DECODED. */
static gboolean produce(Reader *r, guint64 n, GError **error)
{
    if (n <= MAX_DECODED - r->produced) {
        r->produced += n;
        return TRUE;
    }
    set_error(error, AIRDROP_ERROR_LIMIT, "it decodes to too many bytes");
    return FALSE;
}

/* An object's count, in the marker's low nibble, or with 0xF there in an
 * integer object right after the marker. */
static gboolean read_count(Reader *r, guint8 info, gsize *pos, guint64 *out, GError **error)
{
    if (info != 0x0f) {
        *out = info;
        return TRUE;
    }
    if (!span(r, *pos, 1, error)) return FALSE;
    guint8 marker = r->data[*pos];
    if (marker >> 4 != 0x1 || (marker & 0x0f) > 3) {
        set_error(error, AIRDROP_ERROR_INVALID, "a bad count");
        return FALSE;
    }
    guint n = 1u << (marker & 0x0f);
    if (!span(r, *pos + 1, n, error)) return FALSE;
    guint64 v = read_be(r->data + *pos + 1, n);
    /* No count can be larger than the input it describes. */
    if (v > r->len) {
        set_error(error, AIRDROP_ERROR_INVALID, "a count runs past the end");
        return FALSE;
    }
    *pos += 1 + n;
    *out = v;
    return TRUE;
}

/* The `i`th of the references at `pos`, which span() has checked. */
static gboolean read_ref(Reader *r, gsize pos, guint64 i, guint64 *out, GError **error)
{
    guint64 ref = read_be(r->data + pos + i * r->ref_size, r->ref_size);
    if (ref >= r->count) {
        set_error(error, AIRDROP_ERROR_INVALID, "a reference out of range");
        return FALSE;
    }
    *out = ref;
    return TRUE;
}

/* Starts decoding container `index`, refusing a cycle and nesting past 64. */
static gboolean enter(Reader *r, guint64 index, GError **error)
{
    for (guint i = 0; i < r->depth; i++)
        if (r->path[i] == index) {
            set_error(error, AIRDROP_ERROR_INVALID, "a reference cycle");
            return FALSE;
        }
    if (r->depth >= MAX_DEPTH) {
        set_error(error, AIRDROP_ERROR_LIMIT, "it nests too deeply");
        return FALSE;
    }
    r->path[r->depth++] = index;
    return TRUE;
}

static GVariant *read_object(Reader *r, guint64 index, gboolean *dropped, GError **error);

static GVariant *read_integer(Reader *r, guint8 info, gsize pos, GError **error)
{
    if (info > 4) {
        set_error(error, AIRDROP_ERROR_INVALID, "a bad integer size");
        return NULL;
    }
    guint n = 1u << info;
    if (!span(r, pos, n, error)) return NULL;
    const guint8 *p = r->data + pos;
    /* Integers of 1, 2 and 4 bytes are unsigned, of 8 signed. Core
     * Foundation writes 16 bytes for an unsigned value past G_MAXINT64;
     * as in owly, only one that still fits a signed 64-bit integer is
     * taken. */
    if (n <= 4) return g_variant_new_int64((gint64)read_be(p, n));
    if (n == 8) return g_variant_new_int64((gint64)read_be(p, 8));
    guint64 high = read_be(p, 8), low = read_be(p + 8, 8);
    if (high != 0 || low > G_MAXINT64) {
        set_error(error, AIRDROP_ERROR_INVALID, "an integer out of range");
        return NULL;
    }
    return g_variant_new_int64((gint64)low);
}

static GVariant *read_real(Reader *r, guint8 info, gsize pos, GError **error)
{
    if (info == 2 && span(r, pos, 4, error)) {
        guint32 bits = (guint32)read_be(r->data + pos, 4);
        float f;
        memcpy(&f, &bits, sizeof f);
        return g_variant_new_double(f);
    }
    if (info == 3 && span(r, pos, 8, error)) {
        guint64 bits = read_be(r->data + pos, 8);
        double d;
        memcpy(&d, &bits, sizeof d);
        return g_variant_new_double(d);
    }
    if (info != 2 && info != 3) set_error(error, AIRDROP_ERROR_INVALID, "a bad real size");
    return NULL;
}

static GVariant *read_ascii(Reader *r, guint8 info, gsize pos, GError **error)
{
    guint64 len;
    if (!read_count(r, info, &pos, &len, error) || !span(r, pos, len, error)
        || !produce(r, len, error))
        return NULL;
    const guint8 *p = r->data + pos;
    for (guint64 i = 0; i < len; i++)
        /* A NUL would cut the string short wherever it's used. */
        if (p[i] == 0 || p[i] > 0x7f) {
            set_error(error, AIRDROP_ERROR_INVALID, "a bad byte in an ASCII string");
            return NULL;
        }
    return g_variant_new_take_string(g_strndup((const char *)p, len));
}

static GVariant *read_utf16(Reader *r, guint8 info, gsize pos, GError **error)
{
    guint64 units;
    if (!read_count(r, info, &pos, &units, error) || !span(r, pos, units * 2, error)
        || !produce(r, units * 3, error))   /* a unit becomes at most 3 bytes of UTF-8 */
        return NULL;
    const guint8 *p = r->data + pos;
    GString *s = g_string_sized_new(units);
    for (guint64 i = 0; i < units; i++) {
        gunichar c = p[2 * i] << 8 | p[2 * i + 1];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < units) {
            gunichar low = p[2 * i + 2] << 8 | p[2 * i + 3];
            if (low >= 0xdc00 && low <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                i++;
            }
        }
        if (c == 0 || (c >= 0xd800 && c <= 0xdfff)) {
            g_string_free(s, TRUE);
            set_error(error, AIRDROP_ERROR_INVALID, "bad UTF-16");
            return NULL;
        }
        g_string_append_unichar(s, c);
    }
    return g_variant_new_take_string(g_string_free(s, FALSE));
}

static GVariant *read_array(Reader *r, guint64 index, guint8 info, gsize pos, GError **error)
{
    guint64 n;
    if (!read_count(r, info, &pos, &n, error) || !span(r, pos, n * r->ref_size, error)
        || !enter(r, index, error))
        return NULL;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("av"));
    for (guint64 i = 0; i < n; i++) {
        guint64 ref;
        gboolean dropped;
        GVariant *child = NULL;
        if (!read_ref(r, pos, i, &ref, error)
            || (!(child = read_object(r, ref, &dropped, error)) && !dropped)) {
            g_variant_builder_clear(&b);
            return NULL;
        }
        if (child) g_variant_builder_add(&b, "v", child);
    }
    r->depth--;
    return g_variant_builder_end(&b);
}

static GVariant *read_dict(Reader *r, guint64 index, guint8 info, gsize pos, GError **error)
{
    guint64 n;
    if (!read_count(r, info, &pos, &n, error) || !span(r, pos, 2 * n * r->ref_size, error)
        || !enter(r, index, error))
        return NULL;
    gsize values = pos + n * r->ref_size;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    for (guint64 i = 0; i < n; i++) {
        guint64 ref;
        gboolean dropped;
        if (!read_ref(r, pos, i, &ref, error)) goto fail;
        GVariant *key = read_object(r, ref, &dropped, error);
        if (!key && !dropped) goto fail;
        if (!key || !g_variant_is_of_type(key, G_VARIANT_TYPE_STRING)) {
            if (key) g_variant_unref(g_variant_ref_sink(key));
            set_error(error, AIRDROP_ERROR_INVALID, "a dictionary key that isn't a string");
            goto fail;
        }
        g_variant_ref_sink(key);
        const char *name = g_variant_get_string(key, NULL);
        if (!g_hash_table_add(seen, g_strdup(name))) {
            g_variant_unref(key);
            set_error(error, AIRDROP_ERROR_INVALID, "a dictionary key given twice");
            goto fail;
        }
        GVariant *value = NULL;
        if (!read_ref(r, values, i, &ref, error)
            || (!(value = read_object(r, ref, &dropped, error)) && !dropped)) {
            g_variant_unref(key);
            goto fail;
        }
        /* Dates and UIDs have no use in AirDrop's messages: dropped. */
        if (value) g_variant_builder_add(&b, "{sv}", name, value);
        g_variant_unref(key);
    }
    g_hash_table_unref(seen);
    r->depth--;
    return g_variant_builder_end(&b);

fail:
    g_hash_table_unref(seen);
    g_variant_builder_clear(&b);
    return NULL;
}

/* Object `index`, a floating GVariant. A date or UID gives NULL with
 * *dropped set and no error. */
static GVariant *read_object(Reader *r, guint64 index, gboolean *dropped, GError **error)
{
    *dropped = FALSE;
    if (++r->objects > BPLIST_MAX_OBJECTS) {
        set_error(error, AIRDROP_ERROR_LIMIT, "it holds too many objects");
        return NULL;
    }
    guint64 offset = read_be(r->data + r->table + index * r->offset_size, r->offset_size);
    if (offset < MAGIC_LEN || offset >= r->table) {
        set_error(error, AIRDROP_ERROR_INVALID, "an object offset out of range");
        return NULL;
    }
    gsize pos = offset;
    guint8 marker = r->data[pos++];
    guint8 info = marker & 0x0f;
    switch (marker >> 4) {
    case 0x0:
        if (info == 0x8) return g_variant_new_boolean(FALSE);
        if (info == 0x9) return g_variant_new_boolean(TRUE);
        break;
    case 0x1:
        return read_integer(r, info, pos, error);
    case 0x2:
        return read_real(r, info, pos, error);
    case 0x3:
        if (info != 0x3) break;
        if (!span(r, pos, 8, error)) return NULL;
        *dropped = TRUE;
        return NULL;
    case 0x4: {
        guint64 len;
        if (!read_count(r, info, &pos, &len, error) || !span(r, pos, len, error)
            || !produce(r, len, error))
            return NULL;
        return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, r->data + pos, len, 1);
    }
    case 0x5:
        return read_ascii(r, info, pos, error);
    case 0x6:
        return read_utf16(r, info, pos, error);
    case 0x8:
        if (info > 7 || !span(r, pos, info + 1, error)) break;
        *dropped = TRUE;
        return NULL;
    case 0xa:
        return read_array(r, index, info, pos, error);
    case 0xd:
        return read_dict(r, index, info, pos, error);
    }
    if (!error || !*error) set_error(error, AIRDROP_ERROR_INVALID, "an unsupported object");
    return NULL;
}

/* A plist's value, as a new, non-floating reference (SEC-3, SEC-6). */
GVariant *bplist_decode(const guint8 *data, gsize len, GError **error)
{
    if (len > BPLIST_MAX_BYTES) {
        set_error(error, AIRDROP_ERROR_LIMIT, "it is too large");
        return NULL;
    }
    if (len < MAGIC_LEN || memcmp(data, MAGIC, MAGIC_LEN) != 0) {
        set_error(error, AIRDROP_ERROR_INVALID, "not bplist00");
        return NULL;
    }
    if (len < MAGIC_LEN + 1 + TRAILER_LEN) {
        set_error(error, AIRDROP_ERROR_INVALID, "too short");
        return NULL;
    }
    const guint8 *t = data + len - TRAILER_LEN;
    Reader *r = g_new0(Reader, 1);
    r->data = data;
    r->len = len;
    r->offset_size = t[6];
    r->ref_size = t[7];
    r->count = read_be(t + 8, 8);
    guint64 top = read_be(t + 16, 8);
    guint64 table = read_be(t + 24, 8);
    gsize end = len - TRAILER_LEN;
    GVariant *value = NULL;
    gboolean dropped = FALSE;
    if (r->offset_size < 1 || r->offset_size > 8 || r->ref_size < 1 || r->ref_size > 8)
        set_error(error, AIRDROP_ERROR_INVALID, "bad sizes in the trailer");
    else if (r->count == 0 || top >= r->count || r->count > end)
        set_error(error, AIRDROP_ERROR_INVALID, "a bad object count");
    /* The table sits between the objects and the trailer. */
    else if (table <= MAGIC_LEN || table > end || r->count * r->offset_size > end - table)
        set_error(error, AIRDROP_ERROR_INVALID, "the offset table out of range");
    else {
        r->table = table;
        value = read_object(r, top, &dropped, error);
        if (dropped) set_error(error, AIRDROP_ERROR_INVALID, "the top object is a date or UID");
    }
    g_free(r);
    return value ? g_variant_ref_sink(value) : NULL;
}

/* ------------------------------------------------------------ writing */

typedef struct {
    GVariant *value;   /* a leaf, a container, or a dict's key */
    guint first;       /* a container's first reference in Writer.refs */
    guint n;           /* and its references: n for an array, 2n for a dict */
} Flat;

typedef struct {
    GArray *objects;   /* Flat, in output order */
    GArray *refs;      /* guint64 object indexes */
    gsize least;       /* the fewest bytes the objects can take */
} Writer;

/* Bytes an unsigned value needs: 1, 2, 4 or 8. */
static guint int_size(guint64 v)
{
    return v <= 0xff ? 1 : v <= 0xffff ? 2 : v <= 0xffffffffu ? 4 : 8;
}

static void put_be(GByteArray *out, guint64 v, guint n)
{
    guint8 buf[8];
    for (guint i = 0; i < n; i++) buf[i] = (guint8)(v >> (8 * (n - 1 - i)));
    g_byte_array_append(out, buf, n);
}

/* Non-negative integers take the fewest of 1, 2, 4 or 8 bytes, which are
 * read as unsigned; negative ones always take 8. */
static void put_int(GByteArray *out, gint64 v)
{
    guint n = v < 0 ? 8 : int_size((guint64)v);
    guint8 marker = 0x10 | (n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : 3);
    g_byte_array_append(out, &marker, 1);
    put_be(out, (guint64)v, n);
}

/* A marker with its count, inline below 15. */
static void put_head(GByteArray *out, guint8 kind, guint64 n)
{
    guint8 marker = kind << 4 | (n < 0x0f ? (guint8)n : 0x0f);
    g_byte_array_append(out, &marker, 1);
    if (n >= 0x0f) put_int(out, (gint64)n);
}

static void put_string(GByteArray *out, const char *s)
{
    gsize len = strlen(s);
    gboolean ascii = TRUE;
    for (gsize i = 0; i < len && ascii; i++) ascii = (guchar)s[i] < 0x80;
    if (ascii) {
        put_head(out, 0x5, len);
        g_byte_array_append(out, (const guint8 *)s, len);
        return;
    }
    glong units = 0;
    gunichar2 *u = g_utf8_to_utf16(s, -1, NULL, &units, NULL);   /* GVariant strings are valid UTF-8 */
    put_head(out, 0x6, units);
    for (glong i = 0; i < units; i++) put_be(out, u[i], 2);
    g_free(u);
}

static gboolean is_integer(const GVariantType *t)
{
    return g_variant_type_equal(t, G_VARIANT_TYPE_INT64) || g_variant_type_equal(t, G_VARIANT_TYPE_UINT64)
           || g_variant_type_equal(t, G_VARIANT_TYPE_INT32) || g_variant_type_equal(t, G_VARIANT_TYPE_UINT32)
           || g_variant_type_equal(t, G_VARIANT_TYPE_INT16) || g_variant_type_equal(t, G_VARIANT_TYPE_UINT16)
           || g_variant_type_equal(t, G_VARIANT_TYPE_BYTE);
}

static gboolean push(Writer *w, GVariant *value, gsize least, guint *index, GError **error)
{
    w->least += least;
    if (w->least > BPLIST_MAX_BYTES) {
        set_error(error, AIRDROP_ERROR_LIMIT, "it would be too large");
        return FALSE;
    }
    Flat f = { g_variant_ref(value), 0, 0 };
    *index = w->objects->len;
    g_array_append_val(w->objects, f);
    return TRUE;
}

/* Lists `value` and everything under it as objects, a container before its
 * children and a dict's keys before its values. */
static gboolean flatten(Writer *w, GVariant *value, guint depth, guint *index, GError **error)
{
    g_variant_ref(value);
    while (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
    }
    const GVariantType *t = g_variant_get_type(value);
    gboolean ok = FALSE;
    if (g_variant_type_equal(t, G_VARIANT_TYPE_BOOLEAN) || is_integer(t)
        || g_variant_type_equal(t, G_VARIANT_TYPE_DOUBLE)) {
        ok = push(w, value, 1, index, error);
    } else if (g_variant_type_equal(t, G_VARIANT_TYPE_STRING) || g_variant_type_equal(t, G_VARIANT_TYPE_BYTESTRING)) {
        ok = push(w, value, 1 + g_variant_get_size(value), index, error);
    } else if (g_variant_type_is_array(t)) {
        const GVariantType *element = g_variant_type_element(t);
        gboolean dict = g_variant_type_is_dict_entry(element)
                        && g_variant_type_equal(g_variant_type_key(element), G_VARIANT_TYPE_STRING);
        if (g_variant_type_is_dict_entry(element) && !dict) {
            set_error(error, AIRDROP_ERROR_INVALID, "a dictionary key that isn't a string");
            goto out;
        }
        /* As deep as the reader takes: 64 containers, the top one first. */
        if (depth > MAX_DEPTH) {
            set_error(error, AIRDROP_ERROR_LIMIT, "it nests too deeply");
            goto out;
        }
        guint n = g_variant_n_children(value);
        if (!push(w, value, 1 + (dict ? 2 * (gsize)n : n), index, error)) goto out;
        guint me = *index;
        guint first = w->refs->len;
        g_array_set_size(w->refs, first + (dict ? 2 * n : n));
        g_array_index(w->objects, Flat, me).first = first;
        g_array_index(w->objects, Flat, me).n = dict ? 2 * n : n;
        for (guint i = 0; dict && i < n; i++) {
            GVariant *entry = g_variant_get_child_value(value, i);
            GVariant *key = g_variant_get_child_value(entry, 0);
            guint k;
            gboolean pushed = push(w, key, 1 + g_variant_get_size(key), &k, error);
            g_variant_unref(key);
            g_variant_unref(entry);
            if (!pushed) goto out;
            g_array_index(w->refs, guint64, first + i) = k;
        }
        for (guint i = 0; i < n; i++) {
            GVariant *child = g_variant_get_child_value(value, i);
            if (dict) {
                GVariant *entry = child;
                child = g_variant_get_child_value(entry, 1);
                g_variant_unref(entry);
            }
            guint c;
            gboolean flat = flatten(w, child, depth + 1, &c, error);
            g_variant_unref(child);
            if (!flat) goto out;
            g_array_index(w->refs, guint64, first + (dict ? n : 0) + i) = c;
        }
        ok = TRUE;
    } else {
        set_error(error, AIRDROP_ERROR_INVALID, "a value of a type it can't hold");
    }
out:
    g_variant_unref(value);
    return ok;
}

static void put_object(Writer *w, const Flat *f, guint ref_size, GByteArray *out)
{
    GVariant *v = f->value;
    const GVariantType *t = g_variant_get_type(v);
    if (g_variant_type_is_array(t) && !g_variant_type_equal(t, G_VARIANT_TYPE_BYTESTRING)) {
        gboolean dict = g_variant_type_is_dict_entry(g_variant_type_element(t));
        put_head(out, dict ? 0xd : 0xa, dict ? f->n / 2 : f->n);
        for (guint i = 0; i < f->n; i++)
            put_be(out, g_array_index(w->refs, guint64, f->first + i), ref_size);
    } else if (g_variant_type_equal(t, G_VARIANT_TYPE_BOOLEAN)) {
        guint8 marker = g_variant_get_boolean(v) ? 0x09 : 0x08;
        g_byte_array_append(out, &marker, 1);
    } else if (g_variant_type_equal(t, G_VARIANT_TYPE_UINT64) && g_variant_get_uint64(v) > G_MAXINT64) {
        /* As Core Foundation writes an unsigned value past G_MAXINT64. */
        guint8 marker = 0x14;
        g_byte_array_append(out, &marker, 1);
        put_be(out, 0, 8);
        put_be(out, g_variant_get_uint64(v), 8);
    } else if (is_integer(t)) {
        gint64 n = g_variant_type_equal(t, G_VARIANT_TYPE_INT64)    ? g_variant_get_int64(v)
                   : g_variant_type_equal(t, G_VARIANT_TYPE_UINT64) ? (gint64)g_variant_get_uint64(v)
                   : g_variant_type_equal(t, G_VARIANT_TYPE_INT32)  ? (gint64)g_variant_get_int32(v)
                   : g_variant_type_equal(t, G_VARIANT_TYPE_UINT32) ? (gint64)g_variant_get_uint32(v)
                   : g_variant_type_equal(t, G_VARIANT_TYPE_INT16)  ? (gint64)g_variant_get_int16(v)
                   : g_variant_type_equal(t, G_VARIANT_TYPE_UINT16) ? (gint64)g_variant_get_uint16(v)
                                                                    : (gint64)g_variant_get_byte(v);
        put_int(out, n);
    } else if (g_variant_type_equal(t, G_VARIANT_TYPE_DOUBLE)) {
        double d = g_variant_get_double(v);
        guint64 bits;
        memcpy(&bits, &d, sizeof bits);
        guint8 marker = 0x23;
        g_byte_array_append(out, &marker, 1);
        put_be(out, bits, 8);
    } else if (g_variant_type_equal(t, G_VARIANT_TYPE_BYTESTRING)) {
        gsize len;
        const guint8 *bytes = g_variant_get_fixed_array(v, &len, 1);
        put_head(out, 0x4, len);
        if (len) g_byte_array_append(out, bytes, len);
    } else {
        put_string(out, g_variant_get_string(v, NULL));
    }
}

static void clear_flat(gpointer p)
{
    g_variant_unref(((Flat *)p)->value);
}

/* `value` as bplist00. A floating reference is consumed. */
GBytes *bplist_encode(GVariant *value, GError **error)
{
    g_variant_ref_sink(value);
    Writer w = { g_array_new(FALSE, FALSE, sizeof(Flat)), g_array_new(FALSE, FALSE, sizeof(guint64)), 0 };
    g_array_set_clear_func(w.objects, clear_flat);
    GByteArray *out = NULL;
    guint top;
    if (!flatten(&w, value, 1, &top, error)) goto done;

    /* Core Foundation refuses a plist whose references or offsets are too
     * narrow to hold the object count or the table's offset themselves, so
     * both are sized to fit those, not the largest index. */
    guint count = w.objects->len;
    guint ref_size = int_size(count);
    out = g_byte_array_sized_new(MAGIC_LEN + w.least + TRAILER_LEN);
    g_byte_array_append(out, (const guint8 *)MAGIC, MAGIC_LEN);
    guint64 *offsets = g_new(guint64, count);
    for (guint i = 0; i < count && out->len <= BPLIST_MAX_BYTES; i++) {
        offsets[i] = out->len;
        put_object(&w, &g_array_index(w.objects, Flat, i), ref_size, out);
    }
    guint64 table = out->len;
    guint offset_size = int_size(table);
    if (out->len <= BPLIST_MAX_BYTES)
        for (guint i = 0; i < count; i++) put_be(out, offsets[i], offset_size);
    g_free(offsets);
    static const guint8 unused[6] = { 0 };   /* 5 unused bytes and the sort version */
    g_byte_array_append(out, unused, sizeof unused);
    guint8 sizes[2] = { (guint8)offset_size, (guint8)ref_size };
    g_byte_array_append(out, sizes, 2);
    put_be(out, count, 8);
    put_be(out, 0, 8);   /* the top object comes first */
    put_be(out, table, 8);
    if (out->len > BPLIST_MAX_BYTES) {
        set_error(error, AIRDROP_ERROR_LIMIT, "it would be too large");
        g_byte_array_unref(out);
        out = NULL;
    }
done:
    g_array_unref(w.objects);
    g_array_unref(w.refs);
    g_variant_unref(value);
    return out ? g_byte_array_free_to_bytes(out) : NULL;
}
