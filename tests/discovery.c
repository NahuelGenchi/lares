// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for AirDrop discovery on plain data: the Bluetooth LE payload,
 * and the mDNS codec and browser cache, with messages built here by hand
 * byte by byte. Some cases follow owly's own tests (MIT). Run from the
 * repository's root. */
#include <glib.h>
#include <string.h>
#include "../src/airdrop/discovery.h"

#define SEC G_USEC_PER_SEC

/* ---- Recording callbacks ---- */

typedef struct {
    GPtrArray *found;   /* AirdropService* copies */
    GPtrArray *lost;    /* instance names */
} Seen;

static void service_free(gpointer p)
{
    AirdropService *s = p;
    g_free(s->instance);
    g_free(s->host);
    g_free(s->address);
    g_free(s);
}

static void on_found(const AirdropService *s, gpointer user)
{
    Seen *seen = user;
    AirdropService *c = g_new0(AirdropService, 1);
    *c = *s;
    c->instance = g_strdup(s->instance);
    c->host = g_strdup(s->host);
    c->address = g_strdup(s->address);
    g_ptr_array_add(seen->found, c);
}

static void on_lost(const char *instance, gpointer user)
{
    g_ptr_array_add(((Seen *)user)->lost, g_strdup(instance));
}

static const MdnsCallbacks CB = { on_found, on_lost };

static void seen_init(Seen *s)
{
    s->found = g_ptr_array_new_with_free_func(service_free);
    s->lost = g_ptr_array_new_with_free_func(g_free);
}

static void seen_clear(Seen *s)
{
    g_ptr_array_set_size(s->found, 0);
    g_ptr_array_set_size(s->lost, 0);
}

static void seen_free(Seen *s)
{
    g_ptr_array_unref(s->found);
    g_ptr_array_unref(s->lost);
}

static gboolean feed(MdnsCache *c, GBytes *msg, gint64 now, Seen *seen)
{
    gsize n;
    const guint8 *d = g_bytes_get_data(msg, &n);
    return mdns_cache_feed(c, d, n, "awdl0", now, &CB, seen);
}

static gboolean feed_array(MdnsCache *c, GByteArray *msg, gint64 now, Seen *seen)
{
    return mdns_cache_feed(c, msg->data, msg->len, "awdl0", now, &CB, seen);
}

/* ---- Building messages by hand ---- */

static void b16(GByteArray *o, guint v)
{
    guint8 b[2] = { (guint8)(v >> 8), (guint8)v };
    g_byte_array_append(o, b, 2);
}

static void b32(GByteArray *o, guint32 v)
{
    b16(o, v >> 16);
    b16(o, v & 0xffff);
}

static void bbytes(GByteArray *o, const char *s, gsize n)
{
    g_byte_array_append(o, (const guint8 *)s, (guint)n);
}

/* A dotted name, uncompressed, with its final zero. */
static void bname(GByteArray *o, const char *dotted)
{
    char **labels = g_strsplit(dotted, ".", -1);
    for (char **l = labels; *l; l++) {
        if (!**l) continue;
        guint8 n = (guint8)strlen(*l);
        g_byte_array_append(o, &n, 1);
        bbytes(o, *l, n);
    }
    g_strfreev(labels);
    guint8 zero = 0;
    g_byte_array_append(o, &zero, 1);
}

static GByteArray *bheader(guint flags, guint qd, guint an, guint ns, guint ar)
{
    GByteArray *o = g_byte_array_new();
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
static guint bstart(GByteArray *o, guint type, guint cls, guint32 ttl)
{
    b16(o, type);
    b16(o, cls);
    b32(o, ttl);
    b16(o, 0);
    return o->len - 2;
}

static void bend(GByteArray *o, guint at)
{
    guint n = o->len - at - 2;
    o->data[at] = (guint8)(n >> 8);
    o->data[at + 1] = (guint8)n;
}

static const guint8 FE80_1[16] = { 0xfe, 0x80, [15] = 1 };

/* ---- Bluetooth LE ---- */

static void test_ble_payload(void)
{
    /* BLE-2 version 3 with zero hashes (BLE-3). */
    const guint8 tag[3] = { 0xa1, 0xb2, 0xc3 };
    const guint8 want[18] = { 0x40, 0xa1, 0xb2, 0xc3, 0, 0, 0, 0, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0x00 };
    guint8 out[18];
    memset(out, 0xee, sizeof out);
    ble_airdrop_payload(tag, out);
    g_assert_cmpmem(out, sizeof out, want, sizeof want);

    const guint8 other[3] = { 0x12, 0x34, 0x56 };
    ble_airdrop_payload(other, out);
    g_assert_cmpmem(out + 1, 3, other, 3);
    g_assert_cmpint(out[0], ==, 0x40);
    g_assert_cmpint(out[8], ==, 0x03);
}

/* ---- mDNS: building ---- */

/* The announcement of owly's test service, with each record's TTL. */
static GByteArray *expected_announcement(guint32 ptr_ttl, guint32 srv_ttl, guint32 txt_ttl, guint32 aaaa_ttl)
{
    const char *inst = "3fa9c01b22de._airdrop._tcp.local";
    GByteArray *w = bheader(0x8400, 0, 4, 0, 0);
    guint at;
    bname(w, "_airdrop._tcp.local");
    at = bstart(w, 12, 0x0001, ptr_ttl);   /* PTR, shared */
    bname(w, inst);
    bend(w, at);
    bname(w, inst);
    at = bstart(w, 33, 0x8001, srv_ttl);   /* SRV, cache-flush */
    b16(w, 0);
    b16(w, 0);
    b16(w, 8770);
    bname(w, "lares-3fa9c01b22de.local");
    bend(w, at);
    bname(w, inst);
    at = bstart(w, 16, 0x8001, txt_ttl);   /* TXT */
    bbytes(w, "\x09" "flags=137", 10);
    bend(w, at);
    bname(w, "lares-3fa9c01b22de.local");
    at = bstart(w, 28, 0x8001, aaaa_ttl);  /* AAAA */
    g_byte_array_append(w, FE80_1, 16);
    bend(w, at);
    return w;
}

static void assert_bytes(GBytes *got, GByteArray *want)
{
    gsize n;
    const guint8 *d = g_bytes_get_data(got, &n);
    g_assert_cmpmem(d, n, want->data, want->len);
}

static void test_announcement_bytes(void)
{
    /* RFC 6762 §10: host records 120 s, others 75 minutes. */
    GByteArray *w = expected_announcement(4500, 120, 4500, 120);

    /* The host's three spellings, and an address with its scope, give the
     * same message. */
    const char *hosts[] = { "lares-3fa9c01b22de", "lares-3fa9c01b22de.local", "lares-3fa9c01b22de.local." };
    for (guint i = 0; i < G_N_ELEMENTS(hosts); i++) {
        GBytes *a = mdns_build_announcement("3fa9c01b22de", hosts[i], i ? "fe80::1%awdl0" : "fe80::1", 8770,
                                            0x089, G_MAXUINT32);
        g_assert_nonnull(a);
        assert_bytes(a, w);
        g_bytes_unref(a);
    }
    g_byte_array_unref(w);

    /* The TTL argument caps each record's: 0 makes the goodbye. */
    GBytes *a = mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, 0);
    w = expected_announcement(0, 0, 0, 0);
    assert_bytes(a, w);
    g_bytes_unref(a);
    g_byte_array_unref(w);
    a = mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, 600);
    w = expected_announcement(600, 120, 600, 120);
    assert_bytes(a, w);
    g_bytes_unref(a);
    g_byte_array_unref(w);
}

static void test_announcement_round_trip(void)
{
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    GBytes *a = mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x089, G_MAXUINT32);
    g_assert_true(feed(c, a, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    AirdropService *s = seen.found->pdata[0];
    g_assert_cmpstr(s->instance, ==, "3fa9c01b22de");
    g_assert_cmpstr(s->host, ==, "lares-3fa9c01b22de.local.");
    g_assert_cmpstr(s->address, ==, "fe80::1%awdl0");
    g_assert_cmpuint(s->port, ==, 8770);
    g_assert_cmpuint(s->flags, ==, 0x089);

    /* The same again changes nothing; new flags are reported. */
    seen_clear(&seen);
    g_assert_true(feed(c, a, SEC, &seen));
    g_assert_cmpuint(seen.found->len, ==, 0);
    GBytes *b = mdns_build_announcement("3fa9c01b22de", "lares-3fa9c01b22de", "fe80::1", 8770, 0x08b, G_MAXUINT32);
    g_assert_true(feed(c, b, 2 * SEC, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    g_assert_cmpuint(((AirdropService *)seen.found->pdata[0])->flags, ==, 0x08b);
    g_assert_cmpuint(seen.lost->len, ==, 0);
    g_bytes_unref(a);
    g_bytes_unref(b);
    seen_free(&seen);
    mdns_cache_free(c);
}

static void test_build_rejects(void)
{
    char *long_label = g_strnfill(64, 'a');
    g_assert_null(mdns_build_announcement(long_label, "host", "fe80::1", 1, 0, 120));
    g_assert_null(mdns_build_announcement("3fa9c01b22de", long_label, "fe80::1", 1, 0, 120));
    g_assert_null(mdns_build_announcement("3fa9c01b22de", "host", "192.0.2.1", 1, 0, 120));
    g_assert_null(mdns_build_announcement("3fa9c01b22de", "host", "not an address", 1, 0, 120));
    g_assert_null(mdns_build_announcement("", "host", "fe80::1", 1, 0, 120));
    g_assert_null(mdns_build_announcement("3fa9c01b22de", "", "fe80::1", 1, 0, 120));
    g_assert_null(mdns_build_announcement("3fa9c01b22de", "a..b", "fe80::1", 1, 0, 120));
    g_free(long_label);
    char *max = g_strnfill(63, 'b');
    GBytes *ok = mdns_build_announcement(max, max, "fe80::1", 1, 0, 120);
    g_assert_nonnull(ok);
    g_bytes_unref(ok);
    g_free(max);
}

static void test_query(void)
{
    GByteArray *w = bheader(0, 1, 0, 0, 0);
    bname(w, "_airdrop._tcp.local");
    b16(w, 12);       /* PTR */
    b16(w, 0x0001);   /* IN, asking for a multicast answer */
    GBytes *q = mdns_build_query();
    gsize n;
    const guint8 *d = g_bytes_get_data(q, &n);
    g_assert_cmpmem(d, n, w->data, w->len);

    /* A query parses, and teaches a cache nothing. */
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    g_assert_true(feed(c, q, 0, &seen));
    mdns_cache_expire(c, 1000 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.found->len + seen.lost->len, ==, 0);
    seen_free(&seen);
    mdns_cache_free(c);
    g_bytes_unref(q);
    g_byte_array_unref(w);
}

/* ---- mDNS: parsing ---- */

/* Every name after the first points back into earlier ones, as Apple's
 * responders write them. */
static void test_compression(void)
{
    GByteArray *m = bheader(0x8400, 1, 3, 0, 1);
    guint service = m->len;   /* 12 */
    bname(m, "_airdrop._tcp.local");
    guint local = service + 1 + 8 + 1 + 4;   /* the "local" label */
    b16(m, 12);
    b16(m, 1);

    /* PTR: the name points at the question's, the target is "inst" and a
     * pointer to it too. */
    b16(m, 0xc000 | service);
    guint at = bstart(m, 12, 1, 4500);
    guint instance = m->len;
    bbytes(m, "\x04" "inst", 5);
    b16(m, 0xc000 | service);
    bend(m, at);
    /* SRV, named by a pointer to the instance, with "box" and a pointer to
     * "local" as its target. */
    b16(m, 0xc000 | instance);
    at = bstart(m, 33, 0x8001, 120);
    b16(m, 0);
    b16(m, 0);
    b16(m, 8770);
    guint host = m->len;
    bbytes(m, "\x03" "box", 4);
    b16(m, 0xc000 | local);
    bend(m, at);
    /* AAAA, named by a pointer to the SRV's target. */
    b16(m, 0xc000 | host);
    at = bstart(m, 28, 0x8001, 120);
    g_byte_array_append(m, FE80_1, 16);
    bend(m, at);
    /* An NSEC in the additional section, which the browser passes over. */
    b16(m, 0xc000 | host);
    at = bstart(m, 47, 0x8001, 120);
    b16(m, 0xc000 | host);
    bbytes(m, "\x00\x06\x00\x00\x00\x08\x00\x01", 8);
    bend(m, at);

    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    g_assert_true(feed_array(c, m, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    AirdropService *s = seen.found->pdata[0];
    g_assert_cmpstr(s->instance, ==, "inst");
    g_assert_cmpstr(s->host, ==, "box.local.");
    g_assert_cmpstr(s->address, ==, "fe80::1%awdl0");
    g_assert_cmpuint(s->port, ==, 8770);
    g_assert_cmpuint(s->flags, ==, 0);   /* no TXT yet */
    seen_free(&seen);
    mdns_cache_free(c);
    g_byte_array_unref(m);
}

static void assert_rejected(GByteArray *m)
{
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    g_assert_false(feed_array(c, m, 0, &seen));
    g_assert_cmpuint(seen.found->len + seen.lost->len, ==, 0);
    seen_free(&seen);
    mdns_cache_free(c);
    g_byte_array_unref(m);
}

/* A query whose one name is the given bytes. */
static GByteArray *question(const char *name, gsize n)
{
    GByteArray *m = bheader(0, 1, 0, 0, 0);
    bbytes(m, name, n);
    b16(m, 12);
    b16(m, 1);
    return m;
}

static void test_malformed(void)
{
    /* Pointer loops: to itself, and two pointing at each other. */
    assert_rejected(question("\xc0\x0c", 2));
    GByteArray *m = bheader(0, 1, 0, 0, 0);
    bbytes(m, "\xc0\x0e\xc0\x0c", 4);
    b16(m, 12);
    b16(m, 1);
    assert_rejected(m);
    /* A pointer forward, past the end, into the header, or cut short. */
    assert_rejected(question("\x01" "a" "\xc0\x20", 4));
    assert_rejected(question("\xff\xff", 2));
    assert_rejected(question("\xc0\x02", 2));
    m = bheader(0, 1, 0, 0, 0);
    bbytes(m, "\x01" "a" "\xc0", 3);
    assert_rejected(m);
    /* A loop through a name that runs on into its own pointer. */
    m = bheader(0, 2, 0, 0, 0);
    bname(m, "a");
    b16(m, 12);
    b16(m, 1);
    bbytes(m, "\x01" "b" "\xc0\x13", 4);   /* points at itself, at 0x13 */
    b16(m, 12);
    b16(m, 1);
    assert_rejected(m);

    /* Labels: 64 bytes is an extended label type, and so are 0x40 and 0x80;
     * a name over 255 bytes; a label that isn't UTF-8; one cut short. */
    char label[65];
    label[0] = 64;
    memset(label + 1, 'x', 64);
    m = question(label, 65);
    assert_rejected(m);
    assert_rejected(question("\x80" "a", 2));
    GString *big = g_string_new(NULL);
    for (int i = 0; i < 4; i++) {
        g_string_append_c(big, 63);
        for (int j = 0; j < 63; j++) g_string_append_c(big, 'y');
    }
    g_string_append_c(big, 0);
    assert_rejected(question(big->str, big->len));
    /* The same length reached through a pointer: three labels of 63, then
     * a name of one more label pointing at them. */
    m = bheader(0, 2, 0, 0, 0);
    bbytes(m, big->str + 64, 3 * 64 + 1);
    b16(m, 12);
    b16(m, 1);
    bbytes(m, big->str, 64);
    b16(m, 0xc00c);
    b16(m, 12);
    b16(m, 1);
    assert_rejected(m);
    /* 255 bytes with the final zero fit: three labels of 63 and one of
     * 61. One of 62 doesn't. */
    big->str[3 * 64] = 62;
    g_string_erase(big, 3 * 64 + 1, 1);
    assert_rejected(question(big->str, big->len));
    big->str[3 * 64] = 61;
    g_string_erase(big, 3 * 64 + 1, 1);
    g_assert_cmpuint(big->len, ==, 255);
    m = question(big->str, big->len);
    MdnsCache *c = mdns_cache_new();
    g_assert_true(mdns_cache_feed(c, m->data, m->len, "awdl0", 0, &CB, NULL));
    mdns_cache_free(c);
    g_byte_array_unref(m);
    g_string_free(big, TRUE);
    assert_rejected(question("\x02\xff\xfe\x00", 4));
    m = bheader(0, 1, 0, 0, 0);
    bbytes(m, "\x05" "ab", 3);
    assert_rejected(m);

    /* Record data: an AAAA of 15 bytes, a TXT string running past its
     * record, an SRV too short, a PTR whose name runs past its data, data
     * longer than the message. */
    guint at;
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "h.local");
    at = bstart(m, 28, 1, 120);
    g_byte_array_append(m, FE80_1, 15);
    bend(m, at);
    assert_rejected(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "i._airdrop._tcp.local");
    at = bstart(m, 16, 1, 120);
    bbytes(m, "\x09" "flags", 6);
    bend(m, at);
    assert_rejected(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "i._airdrop._tcp.local");
    at = bstart(m, 33, 1, 120);
    bbytes(m, "\x00\x00\x00\x00\x22", 5);
    bend(m, at);
    assert_rejected(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "_airdrop._tcp.local");
    at = bstart(m, 12, 1, 120);
    bname(m, "i._airdrop._tcp.local");
    bend(m, at);
    m->data[at + 1] -= 2;   /* the data ends inside the name */
    assert_rejected(m);
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "h.local");
    bstart(m, 28, 1, 120);
    m->data[m->len - 1] = 16;   /* no data follows */
    assert_rejected(m);

    /* The header: counts past the message, an opcode, an error, short. */
    assert_rejected(bheader(0, 0xffff, 0, 0, 0));
    assert_rejected(bheader(0x8400, 0, 0, 0, 1));
    m = question("\x00", 1);
    m->data[2] = 0x08;   /* opcode 1 */
    assert_rejected(m);
    m = question("\x00", 1);
    m->data[3] = 0x03;   /* NXDOMAIN */
    assert_rejected(m);
    m = g_byte_array_new();
    bbytes(m, "\0\0\x84\0\0\0\0", 7);
    assert_rejected(m);
    /* Longer than any mDNS message (RFC 6762 §17). */
    m = bheader(0x8400, 0, 0, 0, 0);
    g_byte_array_set_size(m, 9001);
    assert_rejected(m);
    m = bheader(0x8400, 0, 0, 0, 0);
    g_byte_array_set_size(m, 9000);
    c = mdns_cache_new();
    g_assert_true(mdns_cache_feed(c, m->data, m->len, "awdl0", 0, &CB, NULL));
    mdns_cache_free(c);
    g_byte_array_unref(m);

    /* Every prefix of a good announcement fails, without a found. */
    GBytes *a = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, 120);
    gsize n;
    const guint8 *d = g_bytes_get_data(a, &n);
    for (gsize len = 0; len < n; len++) {
        m = g_byte_array_new();
        g_byte_array_append(m, d, (guint)len);
        assert_rejected(m);
    }

    /* A malformed message changes nothing, even what it read before the
     * fault: here a goodbye, then a broken record. */
    c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    g_assert_true(feed(c, a, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    GBytes *bye = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, 0);
    m = g_byte_array_new();
    d = g_bytes_get_data(bye, &n);
    g_byte_array_append(m, d, (guint)n);
    m->data[7] = 5;   /* one record more than there is */
    bname(m, "x.local");
    bstart(m, 28, 1, 120);
    g_assert_false(feed_array(c, m, SEC, &seen));
    mdns_cache_expire(c, 3 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 0);
    g_byte_array_unref(m);
    g_bytes_unref(bye);
    seen_free(&seen);
    mdns_cache_free(c);
    g_bytes_unref(a);
}

/* ---- mDNS: the browser's cache ---- */

/* MDNS-6: nothing is found before the SRV target's address is known, and
 * only a link-local address counts (NET-2). */
static void test_resolve_needs_aaaa(void)
{
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    GByteArray *m = bheader(0x8400, 0, 3, 0, 0);
    bname(m, "_airdrop._tcp.local");
    guint at = bstart(m, 12, 1, 4500);
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
    g_assert_true(feed_array(c, m, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 0);
    g_byte_array_unref(m);

    const guint8 routable[16] = { 0x20, 0x01, 0x0d, 0xb8, [15] = 1 };
    m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "MAC.local");   /* names ignore case */
    at = bstart(m, 28, 0x8001, 120);
    g_byte_array_append(m, routable, 16);
    bend(m, at);
    g_assert_true(feed_array(c, m, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 0);
    memcpy(m->data + m->len - 16, FE80_1, 16);
    g_assert_true(feed_array(c, m, SEC, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    AirdropService *s = seen.found->pdata[0];
    g_assert_cmpstr(s->instance, ==, "abc");
    g_assert_cmpstr(s->host, ==, "mac.local.");
    g_assert_cmpuint(s->flags, ==, 59);
    g_byte_array_unref(m);
    seen_free(&seen);
    mdns_cache_free(c);
}

/* A goodbye (TTL 0) means gone in one second (RFC 6762 §10.1). */
static void test_goodbye(void)
{
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    GBytes *a = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, G_MAXUINT32);
    GBytes *bye = mdns_build_announcement("3fa9c01b22de", "lares", "fe80::1", 8770, 0x089, 0);
    g_assert_true(feed(c, a, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    g_assert_true(feed(c, bye, 10 * SEC, &seen));
    g_assert_cmpuint(seen.lost->len, ==, 0);
    mdns_cache_expire(c, 10 * SEC + SEC / 2, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 0);
    /* Later copies of the goodbye, as in a burst, don't put it off. */
    g_assert_true(feed(c, bye, 10 * SEC + SEC * 9 / 10, &seen));
    g_assert_cmpuint(seen.lost->len, ==, 0);
    mdns_cache_expire(c, 11 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 1);
    g_assert_cmpstr(seen.lost->pdata[0], ==, "3fa9c01b22de");
    mdns_cache_expire(c, 20 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 1);
    g_assert_cmpuint(seen.found->len, ==, 1);

    /* A late copy of the goodbye doesn't bring it back, and a goodbye for
     * something never cached teaches nothing. */
    g_assert_true(feed(c, bye, 20 * SEC, &seen));
    GBytes *other = mdns_build_announcement("0a0b0c0d0e0f", "other", "fe80::2", 8770, 0x089, 0);
    g_assert_true(feed(c, other, 20 * SEC, &seen));
    mdns_cache_expire(c, 20 * SEC + SEC / 2, &CB, &seen);
    g_assert_cmpuint(seen.found->len, ==, 1);
    g_assert_cmpuint(seen.lost->len, ==, 1);
    g_bytes_unref(other);

    /* Announced again, it's found again. */
    g_assert_true(feed(c, a, 21 * SEC, &seen));
    g_assert_cmpuint(seen.found->len, ==, 2);

    /* A goodbye for another address of the host leaves the one cached
     * alone. One for that address unresolves the service, but it's only
     * lost once its PTR goes, as in owly: a new address may follow. */
    GByteArray *m = bheader(0x8400, 0, 1, 0, 0);
    bname(m, "lares.local");
    guint at = bstart(m, 28, 0x8001, 0);
    const guint8 fe80_9[16] = { 0xfe, 0x80, [15] = 9 };
    g_byte_array_append(m, fe80_9, 16);
    bend(m, at);
    g_assert_true(feed_array(c, m, 22 * SEC, &seen));
    mdns_cache_expire(c, 30 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 1);
    memcpy(m->data + m->len - 16, FE80_1, 16);
    g_assert_true(feed_array(c, m, 31 * SEC, &seen));
    g_assert_cmpuint(seen.lost->len, ==, 1);
    mdns_cache_expire(c, 32 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 1);   /* the PTR still stands */
    g_byte_array_unref(m);
    g_bytes_unref(a);
    g_bytes_unref(bye);
    seen_free(&seen);
    mdns_cache_free(c);
}

/* SEC-11: whatever lifetime a record claims, it's kept 120 s at most
 * unless refreshed. */
static void test_expiry(void)
{
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    GByteArray *m = bheader(0x8400, 0, 4, 0, 0);
    guint at;
    bname(m, "_airdrop._tcp.local");
    at = bstart(m, 12, 1, G_MAXUINT32);
    bname(m, "abc._airdrop._tcp.local");
    bend(m, at);
    bname(m, "abc._airdrop._tcp.local");
    at = bstart(m, 33, 0x8001, G_MAXUINT32);
    bbytes(m, "\0\0\0\0\x22\x42", 6);
    bname(m, "mac.local");
    bend(m, at);
    bname(m, "abc._airdrop._tcp.local");
    at = bstart(m, 16, 0x8001, G_MAXUINT32);
    bbytes(m, "\x09" "flags=137", 10);
    bend(m, at);
    bname(m, "mac.local");
    at = bstart(m, 28, 0x8001, G_MAXUINT32);
    g_byte_array_append(m, FE80_1, 16);
    bend(m, at);
    g_assert_true(feed_array(c, m, 0, &seen));
    g_assert_cmpuint(seen.found->len, ==, 1);
    mdns_cache_expire(c, 119 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 0);
    /* Refreshed at 100 s, it lasts to 220 s. */
    g_assert_true(feed_array(c, m, 100 * SEC, &seen));
    mdns_cache_expire(c, 219 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 0);
    mdns_cache_expire(c, 221 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 1);
    g_assert_cmpuint(seen.found->len, ==, 1);
    g_byte_array_unref(m);
    seen_free(&seen);
    mdns_cache_free(c);
}

/* SEC-11: the cache holds 256 instances and 256 hosts, and makes room as
 * they age out. */
static void test_cache_bounds(void)
{
    MdnsCache *c = mdns_cache_new();
    Seen seen;
    seen_init(&seen);
    for (guint i = 0; i < 400; i++) {
        char *inst = g_strdup_printf("%012x", i);
        char *host = g_strdup_printf("h%u", i);
        char *addr = g_strdup_printf("fe80::%x", i + 1);
        GBytes *a = mdns_build_announcement(inst, host, addr, 8770, 0x089, G_MAXUINT32);
        g_assert_true(feed(c, a, 0, &seen));
        g_bytes_unref(a);
        g_free(inst);
        g_free(host);
        g_free(addr);
    }
    g_assert_cmpuint(seen.found->len, ==, 256);
    mdns_cache_expire(c, 121 * SEC, &CB, &seen);
    g_assert_cmpuint(seen.lost->len, ==, 256);
    GBytes *a = mdns_build_announcement("3fa9c01b22de", "late", "fe80::abc", 8770, 0x089, G_MAXUINT32);
    g_assert_true(feed(c, a, 122 * SEC, &seen));
    g_assert_cmpuint(seen.found->len, ==, 257);
    g_assert_cmpstr(((AirdropService *)seen.found->pdata[256])->instance, ==, "3fa9c01b22de");
    g_bytes_unref(a);
    seen_free(&seen);
    mdns_cache_free(c);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/airdrop/ble/payload", test_ble_payload);
    g_test_add_func("/airdrop/mdns/announcement-bytes", test_announcement_bytes);
    g_test_add_func("/airdrop/mdns/announcement-round-trip", test_announcement_round_trip);
    g_test_add_func("/airdrop/mdns/build-rejects", test_build_rejects);
    g_test_add_func("/airdrop/mdns/query", test_query);
    g_test_add_func("/airdrop/mdns/compression", test_compression);
    g_test_add_func("/airdrop/mdns/malformed", test_malformed);
    g_test_add_func("/airdrop/mdns/resolve-needs-aaaa", test_resolve_needs_aaaa);
    g_test_add_func("/airdrop/mdns/goodbye", test_goodbye);
    g_test_add_func("/airdrop/mdns/expiry", test_expiry);
    g_test_add_func("/airdrop/mdns/cache-bounds", test_cache_bounds);
    return g_test_run();
}
