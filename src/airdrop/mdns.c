// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* mDNS and DNS-SD for `_airdrop._tcp` on one interface (SPEC §5), ported
 * from owly's proto/mdns.rs and driver/mdns.rs (MIT): a strict DNS message
 * codec, the four records a receiver publishes (MDNS-1), a responder that
 * repeats them in bursts while published (MDNS-7) and answers queries, and
 * a browser whose cache reports a service once its SRV and AAAA resolve
 * (MDNS-6) and forgets it when it's withdrawn or stops being refreshed
 * (SEC-11). Everything a peer sends is hostile until checked. */
#define _GNU_SOURCE
#include "discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#define MDNS_PORT     5353
#define MAX_MESSAGE   9000            /* RFC 6762 §17: the largest message */
#define MAX_QUERY     1400            /* a query of ours, so it needs no fragments */

#define TYPE_PTR      12
#define TYPE_TXT      16
#define TYPE_AAAA     28
#define TYPE_SRV      33
#define TYPE_NSEC     47
#define TYPE_ANY      255
#define CLASS_IN      1
#define CLASS_ANY     255
/* The class's top bit: cache-flush on a record, unicast-response (QU) on a
 * question. */
#define CLASS_TOP     0x8000

/* RFC 6762 §10: host records live 120 s, others 75 minutes; a legacy
 * querier's answers 10 s at most (§6.7). */
#define HOST_TTL      120
#define OTHER_TTL     4500
#define LEGACY_TTL    10

/* Most instances and hosts the browser tracks, so a flood of announcements
 * can't grow the cache without bound, and the longest a record is kept,
 * whatever TTL it claims: RFC 6762's TTL for host records. The browser
 * queries at least every 10 s, so a peer that's still there refreshes its
 * records long before (SEC-11). A goodbye means gone in a second (§10.1). */
#define MAX_CACHE     256
#define MAX_TTL_US    (120 * G_USEC_PER_SEC)
#define GOODBYE_US    G_USEC_PER_SEC

/* The browser's first query 20 ms after it starts, then 1 s later, the
 * wait doubling up to 10 s. */
#define FIRST_QUERY_US        (20 * 1000)
#define QUERY_INTERVAL_US     G_USEC_PER_SEC
#define MAX_QUERY_INTERVAL_US (10 * G_USEC_PER_SEC)

/* The burst owly-radio asks for when the radio is fixed to one channel,
 * as lares-awdl's is (AWDL-12): the phone visits it for a slot or two in
 * sixteen, so a copy goes every 16 TU, one availability window, for a
 * whole 16-slot period, then 3 s of quiet (OWLY-1). The goodbye goes in one
 * such burst too. A radio sharing an access point's channel would want 3
 * copies 50 ms apart instead. */
#define BURST_COUNT       64
#define BURST_INTERVAL_US 16400
#define BURST_PAUSE_US    (3 * G_USEC_PER_SEC)

/* Replies to queries, at most this many a second: a peer flooding queries
 * doesn't make the responder flood the link. */
#define MAX_REPLIES   20

/* ---- Names ---- */

/* A domain name in wire form without its final zero: length-prefixed
 * labels, 255 bytes at most with that zero (RFC 1035 §3.1). Labels never
 * hold a zero byte, so the bytes double as a C string, and comparing them
 * without regard to ASCII case compares the names (RFC 6762 §16). */
typedef struct {
    guint8 len;
    char b[256];
} Name;

static const Name SERVICE = { 20, "\x08_airdrop\x04_tcp\x05local" };
static const Name LOCAL = { 6, "\x05local" };

/* Adds a label of valid UTF-8 with no zero byte, if the name has room. */
static gboolean name_add(Name *n, const char *label, gsize len)
{
    if (len == 0 || len > 63 || n->len + 1 + len + 1 > 255 || !g_utf8_validate(label, len, NULL)) return FALSE;
    n->b[n->len] = (char)len;
    memcpy(n->b + n->len + 1, label, len);
    n->len += 1 + len;
    n->b[n->len] = 0;
    return TRUE;
}

static gboolean name_append(Name *n, const Name *suffix)
{
    if (n->len + suffix->len + 1 > 255) return FALSE;
    memcpy(n->b + n->len, suffix->b, suffix->len + 1);
    n->len += suffix->len;
    return TRUE;
}

static gboolean name_equal(const Name *a, const Name *b)
{
    return a->len == b->len && g_ascii_strncasecmp(a->b, b->b, a->len) == 0;
}

/* The name in wire form with its final zero, uncompressed. */
static gsize name_wire(const Name *n, guint8 *out)
{
    memcpy(out, n->b, n->len);
    out[n->len] = 0;
    return n->len + 1u;
}

static char *name_dotted(const Name *n)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < n->len; i += 1u + (guint8)n->b[i]) {
        g_string_append_len(s, n->b + i + 1, (guint8)n->b[i]);
        g_string_append_c(s, '.');
    }
    if (s->len == 0) g_string_append_c(s, '.');
    return g_string_free(s, FALSE);
}

/* The instance label, if the name is `<instance>._airdrop._tcp.local`. */
static gboolean instance_label(const Name *n, char out[64])
{
    if (n->len == 0) return FALSE;
    guint l = (guint8)n->b[0];
    if (1u + l + SERVICE.len != n->len || g_ascii_strncasecmp(n->b + 1 + l, SERVICE.b, SERVICE.len) != 0) return FALSE;
    memcpy(out, n->b + 1, l);
    out[l] = 0;
    return TRUE;
}

static gboolean link_local(const guint8 a[16])
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

/* ---- Parsing ---- */

static guint16 get16(const guint8 *p)
{
    return (guint16)(p[0] << 8 | p[1]);
}

static guint32 get32(const guint8 *p)
{
    return (guint32)p[0] << 24 | (guint32)p[1] << 16 | (guint32)p[2] << 8 | p[3];
}

/* A name at `pos`, following compression pointers, within the first `len`
 * bytes. A pointer must point strictly before the run of labels it ends,
 * so pointers can't loop, and never into the header; the name is capped at
 * 255 bytes. `end` is where the name ends in place. */
static gboolean read_name(const guint8 *p, gsize len, gsize pos, Name *out, gsize *end)
{
    gsize floor = pos, after = 0;
    out->len = 0;
    out->b[0] = 0;
    for (;;) {
        if (pos >= len) return FALSE;
        guint l = p[pos];
        if (l == 0) {
            *end = after ? after : pos + 1;
            return TRUE;
        } else if (l <= 63) {
            if (pos + 1 + l > len || !name_add(out, (const char *)p + pos + 1, l)) return FALSE;
            pos += 1 + l;
        } else if ((l & 0xc0) == 0xc0) {
            if (pos + 1 >= len) return FALSE;
            gsize target = (gsize)(l & 0x3f) << 8 | p[pos + 1];
            if (target >= floor || target < 12) return FALSE;
            if (!after) after = pos + 2;
            pos = floor = target;
        } else {
            return FALSE;   /* the extended label types, 0x40 and 0x80 */
        }
    }
}

typedef enum { QUESTION, ANSWER, AUTHORITY, ADDITIONAL } Section;

typedef struct {
    Section section;
    Name name;
    guint16 type, cls;   /* the class without its top bit */
    gboolean top;        /* cache-flush on a record, QU on a question */
    guint32 ttl;
    Name target;         /* PTR, SRV */
    guint16 port;        /* SRV */
    guint flags;         /* TXT: its `flags` key (MDNS-4), or 0 */
    guint8 addr[16];     /* AAAA */
} Record;

typedef void (*Visit)(const Record *r, gpointer ctx);

/* The value of a TXT string `flags=<decimal>`; keys ignore case (RFC 6763
 * §6.4). */
static gboolean txt_flags(const guint8 *s, gsize len, guint *flags)
{
    if (len < 7 || len > 16 || g_ascii_strncasecmp((const char *)s, "flags=", 6) != 0) return FALSE;
    guint64 v = 0;
    for (gsize i = 6; i < len; i++) {
        if (!g_ascii_isdigit(s[i])) return FALSE;
        v = v * 10 + (guint)(s[i] - '0');
    }
    if (v > G_MAXUINT32) return FALSE;
    *flags = (guint)v;
    return TRUE;
}

static gboolean read_record(const guint8 *p, gsize len, gsize *at, Record *r)
{
    gsize pos, stop;
    if (!read_name(p, len, *at, &r->name, &pos) || pos + 10 > len) return FALSE;
    r->type = get16(p + pos);
    guint16 cls = get16(p + pos + 2);
    r->cls = cls & ~CLASS_TOP;
    r->top = (cls & CLASS_TOP) != 0;
    r->ttl = get32(p + pos + 4);
    gsize start = pos + 10, end = start + get16(p + pos + 8);
    if (end > len) return FALSE;
    switch (r->type) {
    case TYPE_PTR:
        if (!read_name(p, end, start, &r->target, &stop) || stop != end) return FALSE;
        break;
    case TYPE_SRV:
        if (end - start < 7) return FALSE;
        r->port = get16(p + start + 4);
        if (!read_name(p, end, start + 6, &r->target, &stop) || stop != end) return FALSE;
        break;
    case TYPE_TXT: {
        gboolean found = FALSE;
        r->flags = 0;
        for (gsize i = start; i < end; i += 1u + p[i]) {
            if (i + 1 + p[i] > end) return FALSE;
            if (!found) found = txt_flags(p + i + 1, p[i], &r->flags);
        }
        break;
    }
    case TYPE_AAAA:
        if (end - start != 16) return FALSE;
        memcpy(r->addr, p + start, 16);
        break;
    default:
        break;
    }
    *at = end;
    return TRUE;
}

/* Reads a whole message, handing each question and record to `visit`, if
 * any. FALSE for anything malformed: a visitor may have seen part of it. */
static gboolean walk(const guint8 *p, gsize len, guint16 *id, Visit visit, gpointer ctx)
{
    if (len < 12 || len > MAX_MESSAGE) return FALSE;
    /* Standard queries and responses only, with no error (RFC 6762 §18.3,
     * §18.11). */
    if (get16(p + 2) & 0x780f) return FALSE;
    guint counts[4] = { get16(p + 4), get16(p + 6), get16(p + 8), get16(p + 10) };
    /* Every question takes at least 5 bytes, every record 11. */
    if (counts[0] * 5 + (counts[1] + counts[2] + counts[3]) * 11 > len) return FALSE;
    if (id) *id = get16(p);
    gsize pos = 12;
    Record r;
    for (guint i = 0; i < counts[0]; i++) {
        memset(&r, 0, sizeof r);
        r.section = QUESTION;
        if (!read_name(p, len, pos, &r.name, &pos) || pos + 4 > len) return FALSE;
        r.type = get16(p + pos);
        guint16 cls = get16(p + pos + 2);
        r.cls = cls & ~CLASS_TOP;
        r.top = (cls & CLASS_TOP) != 0;
        pos += 4;
        if (visit) visit(&r, ctx);
    }
    for (guint s = 1; s < 4; s++)
        for (guint i = 0; i < counts[s]; i++) {
            memset(&r, 0, sizeof r);
            r.section = (Section)s;
            if (!read_record(p, len, &pos, &r)) return FALSE;
            if (visit) visit(&r, ctx);
        }
    return TRUE;
}

/* ---- Building ---- */

static void put16(GByteArray *o, guint v)
{
    guint8 b[2] = { (guint8)(v >> 8), (guint8)v };
    g_byte_array_append(o, b, 2);
}

static void put32(GByteArray *o, guint32 v)
{
    guint8 b[4] = { (guint8)(v >> 24), (guint8)(v >> 16), (guint8)(v >> 8), (guint8)v };
    g_byte_array_append(o, b, 4);
}

static void put_name(GByteArray *o, const Name *n)
{
    g_byte_array_append(o, (const guint8 *)n->b, n->len + 1u);
}

static void put_header(GByteArray *o, guint16 id, gboolean response, guint qd, guint an, guint ar)
{
    put16(o, id);
    put16(o, response ? 0x8400 : 0);   /* a response is authoritative (§18.4) */
    put16(o, qd);
    put16(o, an);
    put16(o, 0);
    put16(o, ar);
}

static void put_question(GByteArray *o, const Name *n, guint16 type, guint16 cls)
{
    put_name(o, n);
    put16(o, type);
    put16(o, cls);
}

/* What this receiver publishes (MDNS-1). */
typedef struct {
    char label[64];   /* the instance, the service ID */
    Name instance, host;
    guint8 addr[16];
    guint16 port;
    guint flags;
} Published;

/* The records, as bits; NSEC answers a type the name doesn't have (RFC 6762
 * §6.1) and stands for A beside an AAAA (§6.2). */
enum { R_PTR = 1, R_SRV = 2, R_TXT = 4, R_AAAA = 8, R_NSEC_HOST = 16, R_NSEC_INSTANCE = 32 };

static guint count_bits(guint v)
{
    guint n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

static void put_rr(GByteArray *o, const Name *name, guint16 type, gboolean flush, guint32 ttl,
                   const guint8 *rd, gsize n)
{
    put_name(o, name);
    put16(o, type);
    put16(o, CLASS_IN | (flush ? CLASS_TOP : 0));
    put32(o, ttl);
    put16(o, (guint)n);
    g_byte_array_append(o, rd, (guint)n);
}

/* An NSEC record's data in mDNS's restricted form: the owner as the next
 * name, and window 0 of the type bitmap (RFC 6762 §6.1). */
static gsize nsec_data(guint8 *rd, const Name *owner, const guint16 *types, guint n)
{
    gsize at = name_wire(owner, rd);
    guint8 map[32] = { 0 };
    guint len = 0;
    for (guint i = 0; i < n; i++) {
        map[types[i] / 8] |= (guint8)(0x80 >> (types[i] % 8));
        len = MAX(len, types[i] / 8 + 1u);
    }
    rd[at++] = 0;
    rd[at++] = (guint8)len;
    memcpy(rd + at, map, len);
    return at + len;
}

/* The chosen records, in a fixed order, with owly's TTLs capped at `cap`:
 * the PTR is shared, the others are ours alone and flush caches. */
static void put_records(GByteArray *o, const Published *p, guint which, guint32 cap, gboolean flush)
{
    guint8 rd[300];
    gsize n;
    if (which & R_PTR) {
        n = name_wire(&p->instance, rd);
        put_rr(o, &SERVICE, TYPE_PTR, FALSE, MIN(OTHER_TTL, cap), rd, n);
    }
    if (which & R_SRV) {
        memset(rd, 0, 4);   /* priority and weight */
        rd[4] = (guint8)(p->port >> 8);
        rd[5] = (guint8)p->port;
        n = 6 + name_wire(&p->host, rd + 6);
        put_rr(o, &p->instance, TYPE_SRV, flush, MIN(HOST_TTL, cap), rd, n);
    }
    if (which & R_TXT) {
        int l = g_snprintf((char *)rd + 1, sizeof rd - 1, "flags=%u", p->flags);
        rd[0] = (guint8)l;
        put_rr(o, &p->instance, TYPE_TXT, flush, MIN(OTHER_TTL, cap), rd, 1u + l);
    }
    if (which & R_AAAA)
        put_rr(o, &p->host, TYPE_AAAA, flush, MIN(HOST_TTL, cap), p->addr, 16);
    if (which & R_NSEC_HOST) {
        static const guint16 types[] = { TYPE_AAAA, TYPE_NSEC };
        n = nsec_data(rd, &p->host, types, G_N_ELEMENTS(types));
        put_rr(o, &p->host, TYPE_NSEC, flush, MIN(HOST_TTL, cap), rd, n);
    }
    if (which & R_NSEC_INSTANCE) {
        static const guint16 types[] = { TYPE_TXT, TYPE_SRV, TYPE_NSEC };
        n = nsec_data(rd, &p->instance, types, G_N_ELEMENTS(types));
        put_rr(o, &p->instance, TYPE_NSEC, flush, MIN(HOST_TTL, cap), rd, n);
    }
}

/* A response with the chosen answers and additional records, and for a
 * legacy querier its ID and questions (RFC 6762 §6.7). */
static GBytes *build_response(const Published *p, guint16 id, const GByteArray *questions, guint qd,
                              guint answers, guint additional, guint32 cap, gboolean flush)
{
    GByteArray *o = g_byte_array_sized_new(512);
    put_header(o, id, TRUE, qd, count_bits(answers), count_bits(additional));
    if (questions) g_byte_array_append(o, questions->data, questions->len);
    put_records(o, p, answers, cap, flush);
    put_records(o, p, additional, cap, flush);
    return g_byte_array_free_to_bytes(o);
}

/* Checks and keeps what a receiver publishes. The host may be given as
 * "name", "name.local" or "name.local.", the address with or without its
 * "%scope". */
static gboolean published_init(Published *p, const char *instance, const char *hostname, const char *address,
                               guint16 port, guint flags)
{
    memset(p, 0, sizeof *p);
    if (!instance || !hostname || !address) return FALSE;
    gsize il = strlen(instance);
    if (!name_add(&p->instance, instance, il) || !name_append(&p->instance, &SERVICE)) return FALSE;
    memcpy(p->label, instance, il + 1);

    char *h = g_strdup(hostname);
    gsize hl = strlen(h);
    if (hl && h[hl - 1] == '.') h[--hl] = 0;
    if (hl > 6 && g_ascii_strcasecmp(h + hl - 6, ".local") == 0) h[hl -= 6] = 0;
    gboolean ok = hl > 0;
    for (char *s = h, *dot; ok && s; s = dot ? dot + 1 : NULL) {
        dot = strchr(s, '.');
        ok = name_add(&p->host, s, dot ? (gsize)(dot - s) : strlen(s));
    }
    g_free(h);
    if (!ok || !name_append(&p->host, &LOCAL)) return FALSE;

    char a[INET6_ADDRSTRLEN];
    gsize al = strcspn(address, "%");
    if (al >= sizeof a) return FALSE;
    memcpy(a, address, al);
    a[al] = 0;
    if (inet_pton(AF_INET6, a, p->addr) != 1) return FALSE;
    p->port = port;
    p->flags = flags;
    return TRUE;
}

/* The unsolicited announcement (RFC 6762 §8.3): PTR, SRV, TXT and AAAA,
 * with owly's TTLs (PTR and TXT 4500 s, SRV and AAAA 120 s) each capped at
 * `ttl`, so 0 makes the goodbye (§10.1). NULL if an argument is malformed:
 * an instance or host label over 63 bytes, or an address that isn't IPv6. */
GBytes *mdns_build_announcement(const char *instance, const char *hostname, const char *address,
                                guint16 port, guint flags, guint32 ttl)
{
    Published p;
    if (!published_init(&p, instance, hostname, address, port, flags)) return NULL;
    return build_response(&p, 0, NULL, 0, R_PTR | R_SRV | R_TXT | R_AAAA, 0, ttl, TRUE);
}

/* The question every browse query starts with: `_airdrop._tcp.local` PTR,
 * asking for a multicast answer. */
GBytes *mdns_build_query(void)
{
    GByteArray *o = g_byte_array_sized_new(64);
    put_header(o, 0, FALSE, 1, 0, 0);
    put_question(o, &SERVICE, TYPE_PTR, CLASS_IN);
    return g_byte_array_free_to_bytes(o);
}

/* ---- The browser's cache ---- */

typedef struct {
    char *label;             /* as first heard */
    gboolean has_ptr, has_srv, has_txt;
    gint64 ptr, srv, txt;    /* when each expires */
    Name target;             /* SRV */
    guint16 port;
    guint flags;             /* TXT */
    /* What `found` last reported. */
    gboolean reported;
    Name r_target;
    guint16 r_port;
    guint8 r_addr[16];
    guint r_flags;
} Instance;

typedef struct {
    guint8 addr[16];
    gint64 expires;
} Host;

struct MdnsCache {
    GHashTable *instances;   /* lowercase label -> Instance */
    GHashTable *hosts;       /* lowercase name in wire form -> Host */
    char *ifname;
};

typedef struct {
    gboolean found;
    AirdropService s;
} Event;

static void instance_free(gpointer p)
{
    Instance *i = p;
    g_free(i->label);
    g_free(i);
}

MdnsCache *mdns_cache_new(void)
{
    MdnsCache *c = g_new0(MdnsCache, 1);
    c->instances = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, instance_free);
    c->hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return c;
}

void mdns_cache_free(MdnsCache *c)
{
    if (!c) return;
    g_hash_table_destroy(c->instances);
    g_hash_table_destroy(c->hosts);
    g_free(c->ifname);
    g_free(c);
}

/* When a record expires. A goodbye never lengthens what's there, so the
 * copies of a goodbye burst don't keep pushing its second back. */
static gint64 expiry(guint32 ttl, gint64 now, gint64 current)
{
    if (ttl) return now + MIN((gint64)ttl * G_USEC_PER_SEC, MAX_TTL_US);
    return MIN(current, now + GOODBYE_US);
}

/* The cache entry for an instance, if there's room for it; a goodbye
 * (`create` FALSE) only finds one, as there's nothing to withdraw from a
 * service not cached, and a late copy mustn't bring one back. */
static Instance *instance_for(MdnsCache *c, const char *label, gboolean create)
{
    char *key = g_ascii_strdown(label, -1);
    Instance *i = g_hash_table_lookup(c->instances, key);
    if (i || !create || g_hash_table_size(c->instances) >= MAX_CACHE) {
        g_free(key);
        return i;
    }
    i = g_new0(Instance, 1);
    i->label = g_strdup(label);
    g_hash_table_insert(c->instances, key, i);
    return i;
}

static Host *host_for(MdnsCache *c, const Name *n)
{
    char *key = g_ascii_strdown(n->b, n->len);
    Host *h = g_hash_table_lookup(c->hosts, key);
    g_free(key);
    return h;
}

typedef struct {
    MdnsCache *c;
    gint64 now;
} Feed;

/* Takes in one record of a response: answers and additional records count,
 * the authority section is for probing. */
static void cache_visit(const Record *r, gpointer data)
{
    Feed *f = data;
    char label[64];
    Instance *i;
    gboolean create = r->ttl != 0;
    if ((r->section != ANSWER && r->section != ADDITIONAL) || r->cls != CLASS_IN) return;
    switch (r->type) {
    case TYPE_PTR:
        if (!name_equal(&r->name, &SERVICE) || !instance_label(&r->target, label)) return;
        if (!(i = instance_for(f->c, label, create)) || (!create && !i->has_ptr)) return;
        i->ptr = expiry(r->ttl, f->now, i->has_ptr ? i->ptr : G_MAXINT64);
        i->has_ptr = TRUE;
        break;
    case TYPE_SRV:
        if (!instance_label(&r->name, label) || !(i = instance_for(f->c, label, create))) return;
        /* A goodbye withdraws the record it names, data and all. */
        if (!create && (!i->has_srv || i->port != r->port || !name_equal(&i->target, &r->target))) return;
        i->srv = expiry(r->ttl, f->now, i->has_srv ? i->srv : G_MAXINT64);
        i->has_srv = TRUE;
        i->target = r->target;
        i->port = r->port;
        break;
    case TYPE_TXT:
        if (!instance_label(&r->name, label) || !(i = instance_for(f->c, label, create))) return;
        if (!create && !i->has_txt) return;
        i->txt = expiry(r->ttl, f->now, i->has_txt ? i->txt : G_MAXINT64);
        i->has_txt = TRUE;
        i->flags = r->flags;
        break;
    case TYPE_AAAA: {
        /* AirDrop peers are reached on a link-local address (NET-2), so no
         * other address is ever kept. */
        if (!link_local(r->addr)) return;
        Host *h = host_for(f->c, &r->name);
        if (h && !create && memcmp(h->addr, r->addr, 16) != 0) return;
        if (!h) {
            if (!create || g_hash_table_size(f->c->hosts) >= MAX_CACHE) return;
            h = g_new0(Host, 1);
            h->expires = G_MAXINT64;
            g_hash_table_insert(f->c->hosts, g_ascii_strdown(r->name.b, r->name.len), h);
        }
        memcpy(h->addr, r->addr, 16);
        h->expires = expiry(r->ttl, f->now, h->expires);
        break;
    }
    default:
        break;
    }
}

static char *format_address(const guint8 addr[16], const char *ifname)
{
    char a[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, addr, a, sizeof a);
    return ifname && *ifname ? g_strdup_printf("%s%%%s", a, ifname) : g_strdup(a);
}

static void push_lost(GArray *events, const char *label)
{
    Event e = { FALSE, { .instance = g_strdup(label) } };
    g_array_append_val(events, e);
}

/* Drops what has expired and works out what changed: a service resolves
 * when its PTR is alive and its SRV target has an address (MDNS-6), and is
 * lost once its PTR is gone. */
static void cache_sweep(MdnsCache *c, gint64 now, GArray *events)
{
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, c->hosts);
    while (g_hash_table_iter_next(&it, &key, &value))
        if (((Host *)value)->expires <= now) g_hash_table_iter_remove(&it);

    g_hash_table_iter_init(&it, c->instances);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        Instance *i = value;
        if (i->has_ptr && i->ptr <= now) i->has_ptr = FALSE;
        if (i->has_srv && i->srv <= now) i->has_srv = FALSE;
        if (i->has_txt && i->txt <= now) i->has_txt = FALSE;
        Host *h = i->has_ptr && i->has_srv ? host_for(c, &i->target) : NULL;
        if (h) {
            guint flags = i->has_txt ? i->flags : 0;
            if (!i->reported || i->r_port != i->port || i->r_flags != flags
                || i->r_target.len != i->target.len || memcmp(i->r_target.b, i->target.b, i->target.len) != 0
                || memcmp(i->r_addr, h->addr, 16) != 0) {
                i->reported = TRUE;
                i->r_target = i->target;
                i->r_port = i->port;
                i->r_flags = flags;
                memcpy(i->r_addr, h->addr, 16);
                Event e = { TRUE, { g_strdup(i->label), name_dotted(&i->target), format_address(h->addr, c->ifname),
                                    i->port, flags } };
                g_array_append_val(events, e);
            }
        } else if (i->reported && !i->has_ptr) {
            i->reported = FALSE;
            push_lost(events, i->label);
        }
        if (!i->has_ptr && !i->has_srv && !i->has_txt) g_hash_table_iter_remove(&it);
    }
}

/* Hands the events over once the cache is settled, so a callback may do
 * anything with it. */
static void deliver(GArray *events, const MdnsCallbacks *cb, gpointer user)
{
    for (guint n = 0; n < events->len; n++) {
        Event *e = &g_array_index(events, Event, n);
        if (e->found && cb && cb->found) cb->found(&e->s, user);
        else if (!e->found && cb && cb->lost) cb->lost(e->s.instance, user);
        g_free(e->s.instance);
        g_free(e->s.host);
        g_free(e->s.address);
    }
    g_array_free(events, TRUE);
}

gboolean mdns_cache_feed(MdnsCache *c, const guint8 *msg, gsize len, const char *ifname, gint64 now_us,
                         const MdnsCallbacks *cb, gpointer user)
{
    g_return_val_if_fail(c != NULL, FALSE);
    if (!msg || !walk(msg, len, NULL, NULL, NULL)) return FALSE;
    if (!(msg[2] & 0x80)) return TRUE;   /* a query: nothing to learn */
    if (g_strcmp0(c->ifname, ifname) != 0) {
        g_free(c->ifname);
        c->ifname = g_strdup(ifname);
    }
    Feed f = { c, now_us };
    walk(msg, len, NULL, cache_visit, &f);
    GArray *events = g_array_new(FALSE, FALSE, sizeof(Event));
    cache_sweep(c, now_us, events);
    deliver(events, cb, user);
    return TRUE;
}

void mdns_cache_expire(MdnsCache *c, gint64 now_us, const MdnsCallbacks *cb, gpointer user)
{
    g_return_if_fail(c != NULL);
    GArray *events = g_array_new(FALSE, FALSE, sizeof(Event));
    cache_sweep(c, now_us, events);
    deliver(events, cb, user);
}

/* Forgets everything, reporting each service found as lost. */
static void cache_clear(MdnsCache *c, const MdnsCallbacks *cb, gpointer user)
{
    GArray *events = g_array_new(FALSE, FALSE, sizeof(Event));
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, c->instances);
    while (g_hash_table_iter_next(&it, NULL, &value))
        if (((Instance *)value)->reported) push_lost(events, ((Instance *)value)->label);
    g_hash_table_remove_all(c->instances);
    g_hash_table_remove_all(c->hosts);
    deliver(events, cb, user);
}

/* When the cache next needs sweeping, G_MAXINT64 for never. */
static gint64 cache_deadline(MdnsCache *c)
{
    gint64 next = G_MAXINT64;
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, c->instances);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        Instance *i = value;
        if (i->has_ptr) next = MIN(next, i->ptr);
        if (i->has_srv) next = MIN(next, i->srv);
        if (i->has_txt) next = MIN(next, i->txt);
    }
    g_hash_table_iter_init(&it, c->hosts);
    while (g_hash_table_iter_next(&it, NULL, &value)) next = MIN(next, ((Host *)value)->expires);
    return next;
}

/* A browse query: the service's PTR, then what the cache still lacks, as
 * far as a query of MAX_QUERY bytes goes: SRV and TXT for an instance (ANY),
 * and the address of an SRV target. */
static GBytes *build_browse_query(MdnsCache *c)
{
    GByteArray *o = g_byte_array_sized_new(256);
    put_header(o, 0, FALSE, 0, 0, 0);
    put_question(o, &SERVICE, TYPE_PTR, CLASS_IN);
    guint qd = 1;
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, c->instances);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        Instance *i = value;
        Name n = { 0 };
        if ((!i->has_srv || !i->has_txt) && name_add(&n, i->label, strlen(i->label)) && name_append(&n, &SERVICE)
            && o->len + n.len + 5u <= MAX_QUERY) {
            put_question(o, &n, TYPE_ANY, CLASS_IN);
            qd++;
        }
        if (i->has_srv && !host_for(c, &i->target) && o->len + i->target.len + 5u <= MAX_QUERY) {
            put_question(o, &i->target, TYPE_AAAA, CLASS_IN);
            qd++;
        }
    }
    o->data[4] = (guint8)(qd >> 8);
    o->data[5] = (guint8)qd;
    return g_byte_array_free_to_bytes(o);
}

/* ---- The socket ---- */

struct AirdropMdns {
    int refs;
    gboolean closed;
    char *ifname;
    guint index;
    GSocket *socket;
    int fd;
    GMainContext *context;
    GSource *io, *timer;
    struct sockaddr_in6 group;
    MdnsCallbacks cb, relay;
    gpointer user;
    guint8 *buf;
    gboolean send_logged;

    /* The responder: the records and when the next copy goes. */
    gboolean published;
    Published pub;
    GBytes *announcement;
    gint64 announce_at;
    guint sent;
    /* The goodbye of records just withdrawn, while its burst lasts. */
    GBytes *farewell;
    Name farewell_instance;
    gint64 farewell_at;
    guint farewell_left;
    /* Replies sent in the current second. */
    gint64 window;
    guint replies;

    /* The browser. */
    gboolean browsing;
    MdnsCache *cache;
    gint64 query_at, query_interval;
};

static void mdns_unref(AirdropMdns *m)
{
    if (--m->refs > 0) return;
    g_clear_pointer(&m->announcement, g_bytes_unref);
    g_clear_pointer(&m->farewell, g_bytes_unref);
    mdns_cache_free(m->cache);
    if (m->context) g_main_context_unref(m->context);
    g_free(m->buf);
    g_free(m->ifname);
    g_free(m);
}

static void send_to(AirdropMdns *m, GBytes *msg, const struct sockaddr_in6 *to)
{
    gsize n;
    const guint8 *d = g_bytes_get_data(msg, &n);
    if (sendto(m->fd, d, n, MSG_DONTWAIT, (const struct sockaddr *)to, sizeof *to) < 0 && !m->send_logged) {
        g_debug("AirDrop mDNS: can't send on %s: %s", m->ifname, g_strerror(errno));
        m->send_logged = TRUE;
    }
}

static void reschedule(AirdropMdns *m)
{
    if (m->closed) return;
    gint64 next = G_MAXINT64;
    if (m->published) next = MIN(next, m->announce_at);
    if (m->farewell) next = MIN(next, m->farewell_at);
    if (m->browsing) next = MIN(next, MIN(m->query_at, cache_deadline(m->cache)));
    g_source_set_ready_time(m->timer, next == G_MAXINT64 ? -1 : next);
}

/* The next copy of a burst: from when this one was due, so the spacing
 * holds on average whatever the main loop's latency, unless it's late by
 * more than a copy's spacing. */
static gint64 next_copy(gint64 due, gint64 now, gint64 wait)
{
    return (now - due < BURST_INTERVAL_US ? due : now) + wait;
}

static gboolean on_timer(gpointer data)
{
    AirdropMdns *m = data;
    gint64 now = g_get_monotonic_time();
    m->refs++;
    if (m->published && now >= m->announce_at) {
        send_to(m, m->announcement, &m->group);
        m->sent++;
        m->announce_at = next_copy(m->announce_at, now, m->sent % BURST_COUNT ? BURST_INTERVAL_US : BURST_PAUSE_US);
    }
    if (m->farewell && now >= m->farewell_at) {
        send_to(m, m->farewell, &m->group);
        if (--m->farewell_left == 0) g_clear_pointer(&m->farewell, g_bytes_unref);
        else m->farewell_at = next_copy(m->farewell_at, now, BURST_INTERVAL_US);
    }
    if (m->browsing) mdns_cache_expire(m->cache, now, &m->relay, m);
    if (m->browsing && !m->closed && now >= m->query_at) {
        GBytes *q = build_browse_query(m->cache);
        send_to(m, q, &m->group);
        g_bytes_unref(q);
        m->query_at = now + m->query_interval;
        m->query_interval = MIN(m->query_interval * 2, MAX_QUERY_INTERVAL_US);
    }
    reschedule(m);
    mdns_unref(m);
    return G_SOURCE_CONTINUE;
}

/* What a query asks of the published records. */
typedef struct {
    const Published *pub;
    guint questions, answers, additional;
    gboolean want_ptr, known_ptr, all_qu;
    GByteArray *echo;   /* the questions again, for a legacy querier */
    guint echoed;
} Ask;

static void ask_visit(const Record *r, gpointer data)
{
    Ask *a = data;
    const Published *p = a->pub;
    if (r->section == ANSWER) {
        /* RFC 6762 §7.1: skip what the asker already knows, with at least
         * half its TTL left. */
        if (r->type == TYPE_PTR && name_equal(&r->name, &SERVICE) && name_equal(&r->target, &p->instance)
            && r->ttl >= OTHER_TTL / 2)
            a->known_ptr = TRUE;
        return;
    }
    if (r->section != QUESTION) return;
    a->questions++;
    if (!r->top) a->all_qu = FALSE;
    if (a->echo && a->echoed < 16) {
        put_question(a->echo, &r->name, r->type, r->cls | (r->top ? CLASS_TOP : 0));
        a->echoed++;
    }
    if (r->cls != CLASS_IN && r->cls != CLASS_ANY) return;
    gboolean any = r->type == TYPE_ANY;
    if (name_equal(&r->name, &SERVICE) && (any || r->type == TYPE_PTR)) a->want_ptr = TRUE;
    if (name_equal(&r->name, &p->instance)) {
        if (any || r->type == TYPE_SRV) {
            a->answers |= R_SRV;
            a->additional |= R_AAAA;
        }
        if (any || r->type == TYPE_TXT) a->answers |= R_TXT;
        if (!any && r->type != TYPE_SRV && r->type != TYPE_TXT) a->answers |= R_NSEC_INSTANCE;
    }
    if (name_equal(&r->name, &p->host)) a->answers |= any || r->type == TYPE_AAAA ? R_AAAA : R_NSEC_HOST;
}

/* Answers a query that asks about the published service, to the group, or
 * to the asker when every question asks so (QU) or it's a legacy querier,
 * one not on port 5353 (RFC 6762 §5.4, §6.7). There's one responder per
 * link, so the reply goes at once (§6). */
static void answer(AirdropMdns *m, const guint8 *msg, gsize n, const struct sockaddr_in6 *from, gboolean legacy)
{
    Ask a = { .pub = &m->pub, .all_qu = TRUE, .echo = legacy ? g_byte_array_new() : NULL };
    guint16 id = 0;
    if (!walk(msg, n, &id, ask_visit, &a) || !a.questions) goto out;
    if (a.want_ptr && !a.known_ptr) {
        a.answers |= R_PTR;
        a.additional |= R_SRV | R_TXT | R_AAAA;
    }
    if (!a.answers) goto out;
    a.additional &= ~a.answers;
    if (((a.answers | a.additional) & R_AAAA) && !(a.answers & R_NSEC_HOST)) a.additional |= R_NSEC_HOST;

    gint64 now = g_get_monotonic_time();
    if (now - m->window >= G_USEC_PER_SEC) {
        m->window = now;
        m->replies = 0;
    }
    if (m->replies++ >= MAX_REPLIES) goto out;
    /* RFC 6762 §6.7: a legacy answer has no cache-flush bits and TTLs of
     * at most 10 s. */
    GBytes *reply = build_response(&m->pub, legacy ? id : 0, a.echo, a.echoed, a.answers, a.additional,
                                   legacy ? LEGACY_TTL : G_MAXUINT32, !legacy);
    send_to(m, reply, legacy || a.all_qu ? from : &m->group);
    g_bytes_unref(reply);
out:
    if (a.echo) g_byte_array_free(a.echo, TRUE);
}

static void handle(AirdropMdns *m, const guint8 *msg, gsize n, const struct sockaddr_in6 *from)
{
    /* Nothing of ours counts, however it came back. */
    if (m->published && memcmp(&from->sin6_addr, m->pub.addr, 16) == 0) return;
    if (n < 12) return;
    guint16 port = ntohs(from->sin6_port);
    if (msg[2] & 0x80) {
        /* RFC 6762 §6: a response from any port but 5353 is ignored. */
        if (port == MDNS_PORT && m->browsing)
            mdns_cache_feed(m->cache, msg, n, m->ifname, g_get_monotonic_time(), &m->relay, m);
    } else if (m->published) {
        answer(m, msg, n, from, port != MDNS_PORT);
    }
}

static gboolean on_readable(GSocket *socket, GIOCondition condition, gpointer data)
{
    AirdropMdns *m = data;
    m->refs++;
    /* A few at a time, so a flood can't hold the main loop. */
    for (int k = 0; k < 32 && !m->closed; k++) {
        struct sockaddr_in6 from;
        union {
            char buf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
            struct cmsghdr align;
        } control;
        struct iovec iov = { m->buf, MAX_MESSAGE + 1 };
        struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = control.buf, .msg_controllen = sizeof control.buf };
        ssize_t n = recvmsg(m->fd, &mh, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || mh.msg_namelen < sizeof from || from.sin6_family != AF_INET6)
            continue;
        guint arrived = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
                struct in6_pktinfo info;
                memcpy(&info, CMSG_DATA(c), sizeof info);
                arrived = info.ipi6_ifindex;
            }
        /* Only this interface's link: the group is joined there alone, but
         * Linux hands a socket the group's packets from every interface. */
        if (arrived != m->index || !link_local(from.sin6_addr.s6_addr)) continue;
        handle(m, m->buf, (gsize)n, &from);
    }
    reschedule(m);
    mdns_unref(m);
    return G_SOURCE_CONTINUE;
}

static void relay_found(const AirdropService *s, gpointer user)
{
    AirdropMdns *m = user;
    if (m->closed || !m->browsing) return;
    if (m->published && g_ascii_strcasecmp(s->instance, m->pub.label) == 0) return;
    if (m->cb.found) m->cb.found(s, m->user);
}

static void relay_lost(const char *instance, gpointer user)
{
    AirdropMdns *m = user;
    if (m->closed) return;
    if (m->published && g_ascii_strcasecmp(instance, m->pub.label) == 0) return;
    if (m->cb.lost) m->cb.lost(instance, m->user);
}

static gboolean timer_dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
    return callback(data);
}

static GSourceFuncs timer_funcs = { .dispatch = timer_dispatch };

static gboolean socket_option(GSocket *s, int level, int name, int value, const char *what, GError **error)
{
    GError *e = NULL;
    if (g_socket_set_option(s, level, name, value, &e)) return TRUE;
    g_propagate_prefixed_error(error, e, "%s: ", what);
    return FALSE;
}

/* A socket on UDP [::]:5353, shared with avahi-daemon or any other
 * responder through SO_REUSEADDR (SO_REUSEPORT too, for one of the same
 * user), and in ff02::fb on `ifname` alone. Sends go out there, with a hop
 * limit of 255 (RFC 6762 §11), and don't loop back. */
AirdropMdns *airdrop_mdns_new(const char *ifname, const MdnsCallbacks *cb, gpointer user, GError **error)
{
    g_return_val_if_fail(ifname != NULL && cb != NULL, NULL);
    guint index = if_nametoindex(ifname);
    if (!index) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No network interface named %s", ifname);
        return NULL;
    }
    GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV6, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, error);
    if (!s) return NULL;
    int fd = g_socket_get_fd(s);
    if (!socket_option(s, IPPROTO_IPV6, IPV6_V6ONLY, 1, "IPV6_V6ONLY", error)
        || !socket_option(s, SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR", error)
        || !socket_option(s, SOL_SOCKET, SO_REUSEPORT, 1, "SO_REUSEPORT", error)
        || !socket_option(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1, "IPV6_RECVPKTINFO", error)
        || !socket_option(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, (int)index, "IPV6_MULTICAST_IF", error)
        || !socket_option(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255, "IPV6_MULTICAST_HOPS", error)
        || !socket_option(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, 255, "IPV6_UNICAST_HOPS", error)
        || !socket_option(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, 0, "IPV6_MULTICAST_LOOP", error)) {
        g_object_unref(s);
        return NULL;
    }
    /* Binding to the interface too, where the kernel allows it to a user
     * (Linux 5.7 on), spares waking for other links' mDNS; the packet
     * information above still decides. */
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname)) < 0)
        g_debug("AirDrop mDNS: not bound to %s: %s", ifname, g_strerror(errno));

    GInetAddress *any = g_inet_address_new_any(G_SOCKET_FAMILY_IPV6);
    GSocketAddress *local = g_inet_socket_address_new(any, MDNS_PORT);
    gboolean bound = g_socket_bind(s, local, TRUE, error);
    g_object_unref(local);
    g_object_unref(any);
    struct ipv6_mreq join = { .ipv6mr_interface = index };
    inet_pton(AF_INET6, "ff02::fb", &join.ipv6mr_multiaddr);
    if (!bound) {
        g_object_unref(s);
        return NULL;
    }
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &join, sizeof join) < 0) {
        int err = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Can't join ff02::fb on %s: %s", ifname,
                    g_strerror(err));
        g_object_unref(s);
        return NULL;
    }

    AirdropMdns *m = g_new0(AirdropMdns, 1);
    m->refs = 1;
    m->ifname = g_strdup(ifname);
    m->index = index;
    m->socket = s;
    m->fd = fd;
    m->cb = *cb;
    m->relay = (MdnsCallbacks){ relay_found, relay_lost };
    m->user = user;
    m->buf = g_malloc(MAX_MESSAGE + 1);
    m->cache = mdns_cache_new();
    m->group = (struct sockaddr_in6){ .sin6_family = AF_INET6, .sin6_port = htons(MDNS_PORT), .sin6_scope_id = index };
    m->group.sin6_addr = join.ipv6mr_multiaddr;
    m->context = g_main_context_ref_thread_default();

    m->io = g_socket_create_source(s, G_IO_IN, NULL);
    g_source_set_callback(m->io, G_SOURCE_FUNC(on_readable), m, NULL);
    g_source_attach(m->io, m->context);
    m->timer = g_source_new(&timer_funcs, sizeof(GSource));
    g_source_set_callback(m->timer, on_timer, m, NULL);
    g_source_set_ready_time(m->timer, -1);
    g_source_attach(m->timer, m->context);
    return m;
}

/* Starts a goodbye burst for the published records. */
static void withdraw(AirdropMdns *m, gint64 now)
{
    if (!m->published) return;
    if (m->farewell) send_to(m, m->farewell, &m->group);   /* the last copy of an earlier one */
    g_clear_pointer(&m->farewell, g_bytes_unref);
    m->farewell = build_response(&m->pub, 0, NULL, 0, R_PTR | R_SRV | R_TXT | R_AAAA, 0, 0, TRUE);
    m->farewell_instance = m->pub.instance;
    m->farewell_at = now;
    m->farewell_left = BURST_COUNT;
    m->published = FALSE;
    g_clear_pointer(&m->announcement, g_bytes_unref);
}

void airdrop_mdns_publish(AirdropMdns *m, const char *instance, const char *hostname, const char *address,
                          guint16 port, guint flags)
{
    g_return_if_fail(m != NULL && !m->closed);
    gint64 now = g_get_monotonic_time();
    Published p;
    if (instance && !published_init(&p, instance, hostname, address, port, flags)) {
        g_debug("AirDrop mDNS: not publishing a malformed service");
        instance = NULL;
    }
    if (!instance) {
        withdraw(m, now);
        reschedule(m);
        return;
    }
    /* Another service replaces this one: say goodbye to it first. The same
     * one changing needs no goodbye, as its records flush caches. */
    if (m->published && (!name_equal(&m->pub.instance, &p.instance) || !name_equal(&m->pub.host, &p.host)))
        withdraw(m, now);
    if (m->farewell && name_equal(&m->farewell_instance, &p.instance)) g_clear_pointer(&m->farewell, g_bytes_unref);
    m->pub = p;
    m->published = TRUE;
    g_clear_pointer(&m->announcement, g_bytes_unref);
    m->announcement = build_response(&p, 0, NULL, 0, R_PTR | R_SRV | R_TXT | R_AAAA, 0, G_MAXUINT32, TRUE);
    m->announce_at = now;
    m->sent = 0;
    reschedule(m);
}

void airdrop_mdns_browse(AirdropMdns *m, gboolean on)
{
    g_return_if_fail(m != NULL && !m->closed);
    on = !!on;
    if (on == m->browsing) return;
    if (on) {
        m->browsing = TRUE;
        m->query_at = g_get_monotonic_time() + FIRST_QUERY_US;
        m->query_interval = QUERY_INTERVAL_US;
    } else {
        /* What was found is lost to the caller, who stops listening. */
        m->refs++;
        cache_clear(m->cache, &m->relay, m);
        m->browsing = FALSE;
        mdns_unref(m);
    }
    reschedule(m);
}

/* Stops at once: the published records get one goodbye, as nothing waits
 * for a burst, and no callback runs from here on. */
void airdrop_mdns_free(AirdropMdns *m)
{
    if (!m || m->closed) return;
    if (m->published) {
        GBytes *bye = build_response(&m->pub, 0, NULL, 0, R_PTR | R_SRV | R_TXT | R_AAAA, 0, 0, TRUE);
        send_to(m, bye, &m->group);
        g_bytes_unref(bye);
    }
    if (m->farewell) send_to(m, m->farewell, &m->group);
    m->closed = TRUE;
    m->published = m->browsing = FALSE;
    g_source_destroy(m->io);
    g_source_unref(m->io);
    g_source_destroy(m->timer);
    g_source_unref(m->timer);
    g_socket_close(m->socket, NULL);
    g_object_unref(m->socket);
    m->fd = -1;
    mdns_unref(m);
}
