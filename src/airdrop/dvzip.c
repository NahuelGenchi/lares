// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* The two compressions of an /Upload body (UP-1): gzip (application/x-cpio),
 * one or more members back to back, and DVZip (application/x-dvzip,
 * §6.6). DVZip is a run of chunks, each a 4-byte big-endian length and
 * that many bytes: one complete zlib stream, or, with the length's top bit
 * set, the archive's bytes as they are (DVZ-1). A sender cuts the archive
 * into 64 KiB pieces and compresses each on its own, storing a piece zlib
 * doesn't shrink (DVZ-2).
 *
 * Both directions run through GIO's zlib converters, a bounded buffer at a
 * time, so a small body that inflates to gigabytes fails at `max_output`
 * without ever being held (SEC-3).
 *
 * Ported from owly's compression (MIT). */
#include "formats.h"
#include <string.h>

#define PIECE     65536               /* DVZ-2 */
#define STORED    0x80000000u         /* DVZ-1: a length's top bit */
#define MAX_CHUNK (4u * 1024 * 1024)  /* DVZ-3: longest chunk taken, as owly's receiver */
#define OUT_SIZE  65536               /* output per converter call */
/* zlib's fastest level, as owly's sender uses: photos and videos, most of
 * what AirDrop carries, don't shrink at any level. */
#define LEVEL     1

static gboolean call_sink(AirdropSink sink, gpointer user, const guint8 *buf, gsize len, GError **error)
{
    GError *local = NULL;
    if (sink(buf, len, user, &local)) return TRUE;
    if (!local) local = g_error_new_literal(AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, "Archive data refused");
    g_propagate_error(error, local);
    return FALSE;
}

static void put_be32(guint8 *p, guint32 v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* ------------------------------------------------------------ inflating */

typedef enum { LENGTH, STORED_DATA, CHUNK } Part;

struct AirdropInflater {
    gboolean dvzip;
    guint64 max_output;
    guint64 total;          /* bytes out so far */
    AirdropSink sink;
    gpointer user;
    GConverter *z;          /* gzip, or zlib for DVZip's chunks */
    guint8 *out;
    Part part;              /* DVZip: where in a chunk */
    guint8 length[4];
    guint have;             /* bytes of `length` in */
    guint64 left;           /* bytes of the chunk still to come */
    gboolean ended;         /* gzip: a member is complete, and no other has begun */
    gboolean failed;
};

AirdropInflater *airdrop_inflater_new(gboolean dvzip, guint64 max_output, AirdropSink sink, gpointer user)
{
    AirdropInflater *z = g_new0(AirdropInflater, 1);
    z->dvzip = dvzip;
    z->max_output = max_output;
    z->sink = sink;
    z->user = user;
    z->z = G_CONVERTER(g_zlib_decompressor_new(dvzip ? G_ZLIB_COMPRESSOR_FORMAT_ZLIB : G_ZLIB_COMPRESSOR_FORMAT_GZIP));
    z->out = g_malloc(OUT_SIZE);
    z->part = LENGTH;
    return z;
}

void airdrop_inflater_free(AirdropInflater *z)
{
    if (!z) return;
    g_object_unref(z->z);
    g_free(z->out);
    g_free(z);
}

static gboolean bad(AirdropInflater *z, GError **error, AirdropError code, const char *what)
{
    g_set_error(error, AIRDROP_ERROR, code, "Bad %s body: %s", z->dvzip ? "DVZip" : "gzip", what);
    z->failed = TRUE;
    return FALSE;
}

/* Hands archive bytes on, within max_output (SEC-3). */
static gboolean emit(AirdropInflater *z, const guint8 *buf, gsize len, GError **error)
{
    if (!len) return TRUE;
    if (len > z->max_output - z->total) return bad(z, error, AIRDROP_ERROR_LIMIT, "it decompresses to too much");
    z->total += len;
    if (call_sink(z->sink, z->user, buf, len, error)) return TRUE;
    z->failed = TRUE;
    return FALSE;
}

/* Runs the decompressor over `in` until it has taken all of it and has no
 * output left, or its stream has ended. */
static gboolean run(AirdropInflater *z, const guint8 *in, gsize len, gsize *used, gboolean *ended, GError **error)
{
    gsize pos = 0;
    gboolean full = FALSE;
    *ended = FALSE;
    while (pos < len || full) {
        gsize read = 0, written = 0;
        GError *local = NULL;
        GConverterResult res = g_converter_convert(z->z, pos < len ? in + pos : NULL, len - pos, z->out, OUT_SIZE,
                                                   G_CONVERTER_NO_FLAGS, &read, &written, &local);
        if (res == G_CONVERTER_ERROR) {
            /* No progress without more input: what's here is all taken. */
            gboolean wait = pos == len && g_error_matches(local, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
            if (!wait) bad(z, error, AIRDROP_ERROR_INVALID, local->message);
            g_error_free(local);
            if (!wait) return FALSE;
            break;
        }
        pos += read;
        if (!emit(z, z->out, written, error)) return FALSE;
        full = written == OUT_SIZE;
        if (res == G_CONVERTER_FINISHED) {
            *ended = TRUE;
            break;
        }
        if (!read && !written) break;
    }
    *used = pos;
    return TRUE;
}

/* gzip members one after another decode as one stream (RFC 1952, §2.2), as
 * owly takes them from iPhones. Whatever follows a member must start
 * another: anything else fails at its header, or at the end if it's cut
 * short. max_output counts across all of them. */
static gboolean feed_gzip(AirdropInflater *z, const guint8 *buf, gsize len, GError **error)
{
    gsize pos = 0;
    while (pos < len) {
        if (z->ended) {
            g_converter_reset(z->z);
            z->ended = FALSE;
        }
        gsize used;
        if (!run(z, buf + pos, len - pos, &used, &z->ended, error)) return FALSE;
        pos += used;
        if (!z->ended && pos < len) return bad(z, error, AIRDROP_ERROR_INVALID, "a member that makes no progress");
    }
    return TRUE;
}

static gboolean feed_dvzip(AirdropInflater *z, const guint8 *buf, gsize len, GError **error)
{
    gsize pos = 0;
    while (pos < len) {
        if (z->part == LENGTH) {
            gsize n = MIN(len - pos, (gsize)(4 - z->have));
            memcpy(z->length + z->have, buf + pos, n);
            z->have += n;
            pos += n;
            if (z->have < 4) break;
            guint32 raw = (guint32)z->length[0] << 24 | z->length[1] << 16 | z->length[2] << 8 | z->length[3];
            z->have = 0;
            z->left = raw & ~STORED;
            /* DVZ-3: no empty chunk, and none past the limit. */
            if (!z->left) return bad(z, error, AIRDROP_ERROR_INVALID, "an empty chunk");
            if (z->left > MAX_CHUNK) return bad(z, error, AIRDROP_ERROR_LIMIT, "a chunk too long");
            z->part = raw & STORED ? STORED_DATA : CHUNK;
            if (z->part == CHUNK) g_converter_reset(z->z);
        } else if (z->part == STORED_DATA) {
            gsize n = (gsize)MIN((guint64)(len - pos), z->left);
            if (!emit(z, buf + pos, n, error)) return FALSE;
            pos += n;
            z->left -= n;
            if (!z->left) z->part = LENGTH;
        } else {
            gsize n = (gsize)MIN((guint64)(len - pos), z->left), used;
            gboolean ended;
            if (!run(z, buf + pos, n, &used, &ended, error)) return FALSE;
            pos += used;
            z->left -= used;
            /* DVZ-3: each compressed chunk is exactly one complete zlib
             * stream, however much it inflates to. */
            if (ended && z->left) return bad(z, error, AIRDROP_ERROR_INVALID, "data after a chunk's zlib stream");
            if (!ended && (!z->left || used < n))
                return bad(z, error, AIRDROP_ERROR_INVALID, "a chunk that isn't one complete zlib stream");
            if (ended) z->part = LENGTH;
        }
    }
    return TRUE;
}

gboolean airdrop_inflater_feed(AirdropInflater *z, const guint8 *buf, gsize len, GError **error)
{
    if (z->failed) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The body failed earlier");
        return FALSE;
    }
    if (!len) return TRUE;
    return z->dvzip ? feed_dvzip(z, buf, len, error) : feed_gzip(z, buf, len, error);
}

/* The body has ended: it must end where a gzip member or a DVZip chunk
 * does. */
gboolean airdrop_inflater_finish(AirdropInflater *z, GError **error)
{
    if (z->failed) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The body failed earlier");
        return FALSE;
    }
    if (z->dvzip ? z->part == LENGTH && !z->have : z->ended) return TRUE;
    return bad(z, error, AIRDROP_ERROR_INVALID, "it is cut short");
}

/* ------------------------------------------------------------ deflating */

struct AirdropDeflater {
    gboolean dvzip;
    AirdropSink sink;
    gpointer user;
    GConverter *z;
    guint8 *piece;     /* DVZip: the piece being filled */
    gsize have;
    guint8 *out;       /* a chunk's length and bytes, or gzip's output */
    gboolean finished;
    gboolean failed;
};

AirdropDeflater *airdrop_deflater_new(gboolean dvzip, AirdropSink sink, gpointer user)
{
    AirdropDeflater *d = g_new0(AirdropDeflater, 1);
    d->dvzip = dvzip;
    d->sink = sink;
    d->user = user;
    d->z = G_CONVERTER(g_zlib_compressor_new(dvzip ? G_ZLIB_COMPRESSOR_FORMAT_ZLIB : G_ZLIB_COMPRESSOR_FORMAT_GZIP, LEVEL));
    d->piece = dvzip ? g_malloc(PIECE) : NULL;
    d->out = g_malloc(4 + MAX(PIECE, OUT_SIZE));
    return d;
}

void airdrop_deflater_free(AirdropDeflater *d)
{
    if (!d) return;
    g_object_unref(d->z);
    g_free(d->piece);
    g_free(d->out);
    g_free(d);
}

static gboolean failed(AirdropDeflater *d, GError **error, GError *local)
{
    if (local) {
        g_set_error(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "Can't compress: %s", local->message);
        g_error_free(local);
    }
    d->failed = TRUE;
    return FALSE;
}

/* One DVZip chunk: `piece` as its own zlib stream, or stored when that
 * isn't shorter (DVZ-1, DVZ-2). */
static gboolean put_chunk(AirdropDeflater *d, const guint8 *piece, gsize len, GError **error)
{
    g_converter_reset(d->z);
    gsize in = 0, out = 0;
    gboolean shorter = FALSE;
    /* Room for one byte less than the piece: a stream that fills it doesn't
     * shrink the piece. */
    while (out < len) {
        gsize read = 0, written = 0;
        GError *local = NULL;
        GConverterResult res = g_converter_convert(d->z, in < len ? piece + in : NULL, len - in, d->out + 4 + out,
                                                   len - out, G_CONVERTER_INPUT_AT_END, &read, &written, &local);
        if (res == G_CONVERTER_ERROR) return failed(d, error, local);
        in += read;
        out += written;
        if (res == G_CONVERTER_FINISHED) {
            shorter = out < len;
            break;
        }
    }
    if (shorter) {
        put_be32(d->out, (guint32)out);
    } else {
        put_be32(d->out, (guint32)len | STORED);
        memcpy(d->out + 4, piece, len);
        out = len;
    }
    return call_sink(d->sink, d->user, d->out, 4 + out, error) || failed(d, error, NULL);
}

/* Runs the gzip compressor over `buf`, or to its end with `last`. */
static gboolean run_gzip(AirdropDeflater *d, const guint8 *buf, gsize len, gboolean last, GError **error)
{
    gsize pos = 0;
    for (;;) {
        gsize read = 0, written = 0;
        GError *local = NULL;
        GConverterResult res = g_converter_convert(d->z, pos < len ? buf + pos : NULL, len - pos, d->out, OUT_SIZE,
                                                   last ? G_CONVERTER_INPUT_AT_END : G_CONVERTER_NO_FLAGS,
                                                   &read, &written, &local);
        if (res == G_CONVERTER_ERROR) return failed(d, error, local);
        pos += read;
        if (written && !call_sink(d->sink, d->user, d->out, written, error)) return failed(d, error, NULL);
        if (res == G_CONVERTER_FINISHED || (!last && pos == len)) return TRUE;
        if (!read && !written) return failed(d, error, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "no progress"));
    }
}

gboolean airdrop_deflater_feed(AirdropDeflater *d, const guint8 *buf, gsize len, GError **error)
{
    if (d->failed || d->finished) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID,
                            d->failed ? "Compression failed earlier" : "Compression is finished");
        return FALSE;
    }
    if (!len) return TRUE;
    if (!d->dvzip) return run_gzip(d, buf, len, FALSE, error);
    while (len) {
        gsize n = MIN(len, PIECE - d->have);
        memcpy(d->piece + d->have, buf, n);
        d->have += n;
        buf += n;
        len -= n;
        if (d->have == PIECE) {
            d->have = 0;
            if (!put_chunk(d, d->piece, PIECE, error)) return FALSE;
        }
    }
    return TRUE;
}

gboolean airdrop_deflater_finish(AirdropDeflater *d, GError **error)
{
    if (d->failed || d->finished) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID,
                            d->failed ? "Compression failed earlier" : "Compression is finished");
        return FALSE;
    }
    gboolean ok = d->dvzip ? !d->have || put_chunk(d, d->piece, d->have, error) : run_gzip(d, NULL, 0, TRUE, error);
    d->have = 0;
    d->finished = ok;
    return ok;
}
