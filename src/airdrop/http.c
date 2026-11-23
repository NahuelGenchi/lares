// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* HTTP/1.1 framing for AirDrop's requests and responses (§6.1), strict
 * enough to face anyone in radio range (SEC-7, RFC 9112). Every line of a
 * head and of chunked framing ends in CRLF, and a bare CR or LF is refused;
 * header names are tokens and obsolete line folding is refused. A message
 * carries Content-Length or Transfer-Encoding: chunked, never both and
 * neither twice; Content-Length is plain digits and a chunk size plain hex.
 * Chunk extensions are skipped within a line's limit, and trailers are
 * checked and dropped. Bodies go straight from the input to the caller, so
 * nothing holds one whole.
 *
 * Ported from owly's HTTP framing (MIT). */
#include "formats.h"
#include <string.h>

#define MAX_HEADERS    64     /* fields in a head */
#define MAX_CHUNK_LINE 1024   /* a chunk-size line with its extensions, without CRLF */

typedef enum {
    HEAD,          /* collecting the head */
    LENGTH,        /* a Content-Length body */
    CHUNK_SIZE,    /* collecting a chunk-size line */
    CHUNK_DATA,
    CHUNK_CR,      /* the CRLF after a chunk's data */
    CHUNK_LF,
    TRAILER,       /* collecting trailer lines, up to the empty one */
    UNTIL_EOF,     /* a response body that runs to the connection's end */
    DONE,
    FAILED,
} State;

struct HttpParser {
    HttpKind kind;
    HttpCallbacks cb;
    gpointer user;
    State state;
    GByteArray *line;       /* the head, a chunk-size line or a trailer line so far */
    GHashTable *headers;    /* lowercase name to value */
    guint64 left;           /* bytes of the body or chunk still to come */
    gsize trailer;          /* trailer bytes so far */
};

HttpParser *http_parser_new(HttpKind kind, const HttpCallbacks *cb, gpointer user)
{
    HttpParser *p = g_new0(HttpParser, 1);
    p->kind = kind;
    if (cb) p->cb = *cb;
    p->user = user;
    p->state = HEAD;
    p->line = g_byte_array_new();
    p->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return p;
}

void http_parser_reset(HttpParser *p)
{
    p->state = HEAD;
    g_byte_array_set_size(p->line, 0);
    g_hash_table_remove_all(p->headers);
    p->left = 0;
    p->trailer = 0;
}

void http_parser_free(HttpParser *p)
{
    if (!p) return;
    g_byte_array_unref(p->line);
    g_hash_table_unref(p->headers);
    g_free(p);
}

static gboolean fail(HttpParser *p, GError **error, AirdropError code, const char *what)
{
    g_set_error(error, AIRDROP_ERROR, code, "Bad HTTP message: %s", what);
    p->state = FAILED;
    return FALSE;
}

/* A callback's answer; one that refuses without saying why gets a reason. */
static gboolean answer(HttpParser *p, gboolean ok, GError **local, GError **error)
{
    /* `local` is passed by address: the callback runs, as an argument,
     * before this reads what it set. */
    if (ok) {
        g_clear_error(local);
        return TRUE;
    }
    if (!*local) *local = g_error_new_literal(AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, "HTTP message refused");
    g_propagate_error(error, *local);
    p->state = FAILED;
    return FALSE;
}

static gboolean finish_message(HttpParser *p, GError **error)
{
    GError *local = NULL;
    p->state = DONE;
    return !p->cb.done || answer(p, p->cb.done(p->user, &local), &local, error);
}

/* RFC 9110's tchar: what a method or a header name is made of. */
static gboolean is_tchar(guint8 c)
{
    return g_ascii_isalnum(c) || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static gboolean is_space(guint8 c)
{
    return c == ' ' || c == '\t';
}

/* No control character but a tab. */
static gboolean is_text(const guint8 *s, gsize len)
{
    for (gsize i = 0; i < len; i++)
        if ((s[i] < 0x20 && s[i] != '\t') || s[i] == 0x7f) return FALSE;
    return TRUE;
}

/* A field line, "name: value", into a new lowercase name and a trimmed
 * value. */
static gboolean parse_field(HttpParser *p, const guint8 *s, gsize len, char **name, char **value, GError **error)
{
    if (len && is_space(s[0])) return fail(p, error, AIRDROP_ERROR_INVALID, "obsolete line folding");
    const guint8 *colon = memchr(s, ':', len);
    if (!colon) return fail(p, error, AIRDROP_ERROR_INVALID, "a header without a colon");
    gsize n = colon - s;
    if (!n) return fail(p, error, AIRDROP_ERROR_INVALID, "a bad header name");
    for (gsize i = 0; i < n; i++)
        if (!is_tchar(s[i])) return fail(p, error, AIRDROP_ERROR_INVALID, "a bad header name");
    const guint8 *v = colon + 1, *end = s + len;
    while (v < end && is_space(*v)) v++;
    while (end > v && is_space(end[-1])) end--;
    if (!is_text(v, end - v) || !g_utf8_validate((const char *)v, end - v, NULL))
        return fail(p, error, AIRDROP_ERROR_INVALID, "a bad header value");
    if (name) *name = g_ascii_strdown((const char *)s, n);
    if (value) *value = g_strndup((const char *)v, end - v);
    return TRUE;
}

static gboolean parse_request_line(HttpParser *p, const guint8 *s, gsize len, char **method, char **path,
                                   GError **error)
{
    const guint8 *sp1 = memchr(s, ' ', len);
    const guint8 *sp2 = sp1 ? memchr(sp1 + 1, ' ', s + len - sp1 - 1) : NULL;
    if (!sp2 || memchr(sp2 + 1, ' ', s + len - sp2 - 1))
        return fail(p, error, AIRDROP_ERROR_INVALID, "a bad request line");
    gsize mlen = sp1 - s, tlen = sp2 - sp1 - 1, vlen = s + len - sp2 - 1;
    if (!mlen) return fail(p, error, AIRDROP_ERROR_INVALID, "a bad method");
    for (gsize i = 0; i < mlen; i++)
        if (!is_tchar(s[i])) return fail(p, error, AIRDROP_ERROR_INVALID, "a bad method");
    /* Only the origin form: a path from the root. */
    if (!tlen || sp1[1] != '/') return fail(p, error, AIRDROP_ERROR_INVALID, "a bad request target");
    for (gsize i = 0; i < tlen; i++)
        if (sp1[1 + i] < 0x21 || sp1[1 + i] > 0x7e) return fail(p, error, AIRDROP_ERROR_INVALID, "a bad request target");
    if (vlen != 8 || memcmp(sp2 + 1, "HTTP/1.1", 8) != 0) return fail(p, error, AIRDROP_ERROR_INVALID, "not HTTP/1.1");
    *method = g_strndup((const char *)s, mlen);
    *path = g_strndup((const char *)sp1 + 1, tlen);
    return TRUE;
}

static gboolean parse_status_line(HttpParser *p, const guint8 *s, gsize len, guint *status, GError **error)
{
    if (len < 12 || memcmp(s, "HTTP/1.1 ", 9) != 0) return fail(p, error, AIRDROP_ERROR_INVALID, "not HTTP/1.1");
    const guint8 *code = s + 9;
    if (code[0] < '1' || code[0] > '5' || !g_ascii_isdigit(code[1]) || !g_ascii_isdigit(code[2])
        || (len > 12 && code[3] != ' ') || !is_text(s + 12, len - 12))
        return fail(p, error, AIRDROP_ERROR_INVALID, "a bad status line");
    *status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    return TRUE;
}

typedef enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_TO_EOF } Body;

/* How the body is framed, refusing whatever two readers could take two
 * ways (SEC-7). A response that says "go on" (1xx), or has nothing to send
 * (204, 304), has no body whatever its headers say (RFC 9112, §6.3); any
 * other response with neither framing runs to the connection's end. */
static gboolean framing(HttpParser *p, guint status, guint lengths, guint encodings, Body *body, GError **error)
{
    *body = BODY_NONE;
    if (p->kind == HTTP_RESPONSE && (status < 200 || status == 204 || status == 304)) return TRUE;
    if (lengths && encodings) return fail(p, error, AIRDROP_ERROR_INVALID, "both Content-Length and Transfer-Encoding");
    if (lengths > 1) return fail(p, error, AIRDROP_ERROR_INVALID, "a repeated Content-Length");
    if (encodings > 1) return fail(p, error, AIRDROP_ERROR_INVALID, "a repeated Transfer-Encoding");
    if (lengths) {
        const char *v = g_hash_table_lookup(p->headers, "content-length");
        gsize n = strlen(v);
        gboolean digits = n >= 1 && n <= 19;   /* 19 digits always fit 64 bits */
        for (gsize i = 0; digits && i < n; i++) digits = g_ascii_isdigit(v[i]);
        if (!digits) return fail(p, error, AIRDROP_ERROR_INVALID, "a bad Content-Length");
        p->left = g_ascii_strtoull(v, NULL, 10);
        *body = p->left ? BODY_LENGTH : BODY_NONE;
    } else if (encodings) {
        if (g_ascii_strcasecmp(g_hash_table_lookup(p->headers, "transfer-encoding"), "chunked") != 0)
            return fail(p, error, AIRDROP_ERROR_INVALID, "a Transfer-Encoding other than chunked");
        *body = BODY_CHUNKED;
    } else if (p->kind == HTTP_RESPONSE) {
        *body = BODY_TO_EOF;
    }
    return TRUE;
}

/* The head is in, ending in an empty line: parse it, hand it on and set up
 * the body. */
static gboolean parse_head(HttpParser *p, GError **error)
{
    const guint8 *s = p->line->data, *end = s + p->line->len - 2;   /* up to the empty line's CRLF */
    const guint8 *eol = memchr(s, '\r', end - s);
    char *method = NULL, *path = NULL;
    guint status = 0, lengths = 0, encodings = 0, fields = 0;
    gboolean ok = p->kind == HTTP_REQUEST ? parse_request_line(p, s, eol - s, &method, &path, error)
                                          : parse_status_line(p, s, eol - s, &status, error);
    g_hash_table_remove_all(p->headers);
    for (s = eol + 2; ok && s < end; s = eol + 2) {
        eol = memchr(s, '\r', end - s);
        char *name = NULL, *value = NULL;
        if (++fields > MAX_HEADERS) {
            ok = fail(p, error, AIRDROP_ERROR_LIMIT, "too many headers");
            break;
        }
        if (!(ok = parse_field(p, s, eol - s, &name, &value, error))) break;
        lengths += strcmp(name, "content-length") == 0;
        encodings += strcmp(name, "transfer-encoding") == 0;
        /* A repeated field reads as one, its values joined (RFC 9110). */
        const char *before = g_hash_table_lookup(p->headers, name);
        if (before) {
            char *joined = g_strconcat(before, ", ", value, NULL);
            g_free(value);
            value = joined;
        }
        g_hash_table_replace(p->headers, name, value);
    }
    g_byte_array_set_size(p->line, 0);

    Body body = BODY_NONE;
    ok = ok && framing(p, status, lengths, encodings, &body, error);

    if (ok && p->cb.head) {
        GError *local = NULL;
        ok = answer(p, p->cb.head(method, path, status, p->headers, p->user, &local), &local, error);
    }
    g_free(method);
    g_free(path);
    if (!ok) return FALSE;
    switch (body) {
    case BODY_NONE: return finish_message(p, error);
    case BODY_LENGTH: p->state = LENGTH; break;
    case BODY_CHUNKED: p->state = CHUNK_SIZE; break;
    case BODY_TO_EOF: p->state = UNTIL_EOF; break;
    }
    return TRUE;
}

/* Adds one byte to the line being collected, refusing a bare CR or LF.
 * *eol turns TRUE at a line's CRLF. */
static gboolean line_byte(HttpParser *p, guint8 c, gsize max, gboolean *eol, GError **error)
{
    gsize n = p->line->len;
    guint8 prev = n ? p->line->data[n - 1] : 0;
    if (c == '\n' && prev != '\r') return fail(p, error, AIRDROP_ERROR_INVALID, "a bare LF");
    if (prev == '\r' && c != '\n') return fail(p, error, AIRDROP_ERROR_INVALID, "a bare CR");
    if (n >= max)
        return fail(p, error, AIRDROP_ERROR_LIMIT,
                    p->state == HEAD ? "a head too large" : p->state == TRAILER ? "a trailer too large" : "a chunk line too long");
    g_byte_array_append(p->line, &c, 1);
    *eol = c == '\n';
    return TRUE;
}

static gboolean feed_head(HttpParser *p, const guint8 *buf, gsize len, gsize *pos, GError **error)
{
    while (*pos < len) {
        gboolean eol;
        if (!line_byte(p, buf[(*pos)++], HTTP_MAX_HEAD, &eol, error)) return FALSE;
        if (!eol) continue;
        const guint8 *d = p->line->data;
        gsize n = p->line->len;
        if (n == 2) return fail(p, error, AIRDROP_ERROR_INVALID, "an empty start line");
        if (n >= 4 && d[n - 4] == '\r' && d[n - 3] == '\n') return parse_head(p, error);
    }
    return TRUE;
}

/* `chunk-size [ BWS ";" chunk-ext ]`: 1 to 16 hex digits, then nothing or
 * extensions, which are skipped. */
static gboolean feed_chunk_size(HttpParser *p, const guint8 *buf, gsize len, gsize *pos, GError **error)
{
    while (*pos < len) {
        gboolean eol;
        if (!line_byte(p, buf[(*pos)++], MAX_CHUNK_LINE + 2, &eol, error)) return FALSE;
        if (!eol) continue;
        const guint8 *s = p->line->data;
        gsize n = p->line->len - 2, digits = 0;
        guint64 size = 0;
        while (digits < n && g_ascii_isxdigit(s[digits])) size = size << 4 | g_ascii_xdigit_value(s[digits++]);
        gsize ext = digits;
        while (ext < n && is_space(s[ext])) ext++;
        if (!digits || digits > 16 || (ext < n && s[ext] != ';') || !is_text(s, n))
            return fail(p, error, AIRDROP_ERROR_INVALID, "a bad chunk size");
        g_byte_array_set_size(p->line, 0);
        p->left = size;
        p->state = size ? CHUNK_DATA : TRAILER;
        return TRUE;
    }
    return TRUE;
}

/* Trailer fields are checked like headers, then dropped; together they
 * count against HTTP_MAX_HEAD. */
static gboolean feed_trailer(HttpParser *p, const guint8 *buf, gsize len, gsize *pos, GError **error)
{
    while (*pos < len) {
        gboolean eol;
        if (p->trailer >= HTTP_MAX_HEAD) return fail(p, error, AIRDROP_ERROR_LIMIT, "a trailer too large");
        if (!line_byte(p, buf[(*pos)++], HTTP_MAX_HEAD, &eol, error)) return FALSE;
        p->trailer++;
        if (!eol) continue;
        gsize n = p->line->len - 2;
        if (n && !parse_field(p, p->line->data, n, NULL, NULL, error)) return FALSE;
        g_byte_array_set_size(p->line, 0);
        if (!n) return finish_message(p, error);
    }
    return TRUE;
}

static gboolean give_body(HttpParser *p, const guint8 *buf, gsize len, GError **error)
{
    GError *local = NULL;
    return !p->cb.body || answer(p, p->cb.body(buf, len, p->user, &local), &local, error);
}

gboolean http_parser_feed(HttpParser *p, const guint8 *buf, gsize len, gsize *used, GError **error)
{
    gsize pos = 0;
    gboolean ok = TRUE;
    if (p->state == FAILED) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The HTTP message failed earlier");
        ok = FALSE;
    }
    while (ok && pos < len && p->state != DONE) {
        switch (p->state) {
        case HEAD:
            ok = feed_head(p, buf, len, &pos, error);
            break;
        case LENGTH:
        case CHUNK_DATA: {
            gsize n = (gsize)MIN((guint64)(len - pos), p->left);
            if (!(ok = give_body(p, buf + pos, n, error))) break;
            pos += n;
            p->left -= n;
            if (p->left) break;
            if (p->state == LENGTH) ok = finish_message(p, error);
            else p->state = CHUNK_CR;
            break;
        }
        case CHUNK_CR:
        case CHUNK_LF:
            if (buf[pos++] != (p->state == CHUNK_CR ? '\r' : '\n'))
                ok = fail(p, error, AIRDROP_ERROR_INVALID, "chunk data not followed by CRLF");
            else p->state = p->state == CHUNK_CR ? CHUNK_LF : CHUNK_SIZE;
            break;
        case CHUNK_SIZE:
            ok = feed_chunk_size(p, buf, len, &pos, error);
            break;
        case TRAILER:
            ok = feed_trailer(p, buf, len, &pos, error);
            break;
        case UNTIL_EOF:
            ok = give_body(p, buf + pos, len - pos, error);
            pos = len;
            break;
        case DONE:
        case FAILED:
            break;
        }
    }
    if (used) *used = pos;
    return ok;
}

/* The connection closed. A response that runs to the end is complete; a
 * parser between messages has nothing to finish; anything else was cut
 * short. */
gboolean http_parser_eof(HttpParser *p, GError **error)
{
    switch (p->state) {
    case DONE:
        return TRUE;
    case HEAD:
        if (!p->line->len) return TRUE;
        break;
    case UNTIL_EOF:
        return finish_message(p, error);
    case FAILED:
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The HTTP message failed earlier");
        return FALSE;
    default:
        break;
    }
    return fail(p, error, AIRDROP_ERROR_INVALID, "the connection closed midway");
}

GBytes *http_chunk(const guint8 *buf, gsize len)
{
    if (!len) return g_bytes_new_static("0\r\n\r\n", 5);
    char head[24];
    gsize n = g_snprintf(head, sizeof head, "%" G_GSIZE_MODIFIER "x\r\n", len);
    guint8 *out = g_malloc(n + len + 2);
    memcpy(out, head, n);
    memcpy(out + n, buf, len);
    memcpy(out + n + len, "\r\n", 2);
    return g_bytes_new_take(out, n + len + 2);
}
