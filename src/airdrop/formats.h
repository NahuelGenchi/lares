// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>

/* AirDrop's data formats, on plain bytes and with no I/O of their own:
 * binary property lists, CPIO archives, gzip and DVZip compression, and
 * HTTP/1.1 framing. Rule IDs (UP-2, SEC-7) are those of owly's AirDrop
 * specification, which docs/AIRDROP.md follows. Every input from a peer is
 * hostile until checked, so each decoder has limits and fails cleanly. */

#define AIRDROP_ERROR (airdrop_error_quark())
GQuark airdrop_error_quark(void);

typedef enum {
    AIRDROP_ERROR_INVALID,   /* malformed input */
    AIRDROP_ERROR_LIMIT,     /* input over a limit */
    AIRDROP_ERROR_REFUSED,   /* well formed, but not allowed (a path, an entry type) */
} AirdropError;

/* One path component that is safe to show and to create in a folder: not
 * empty, not "." or "..", no "/", no control characters, and no bidi
 * embeddings, overrides or isolates (SEC-1). */
gboolean airdrop_safe_name(const char *name);

/* ---- Binary property lists (bplist00), SPEC §6.2, SEC-3, SEC-6 ----
 *
 * Values are GVariants: a dict is a{sv}, an array av, a string s, an
 * integer x, a boolean b, data ay and a real d. Decoding accepts only
 * bplist00, caps nesting at 64 and the decoded size (shared references
 * count each time they're used), and drops dates and UIDs from dicts. */

#define BPLIST_MAX_BYTES   (1024 * 1024)   /* encoded size */
#define BPLIST_MAX_OBJECTS 65536           /* decoded objects, references counted */

GVariant *bplist_decode(const guint8 *data, gsize len, GError **error);
GBytes *bplist_encode(GVariant *value, GError **error);

/* Extracts lowercase 64-char SHA-256 hex hashes from ValidatedEmailHashes and
 * ValidatedPhoneHashes inside a SenderRecordData CMS SignedData envelope
 * (or raw XML/binary validation plist). Caller unrefs the returned array. */
GPtrArray *airdrop_extract_record_hashes(const guint8 *data, gsize len);

/* ---- CPIO archives, UP-2 ----
 *
 * The writer emits odc (magic 070707); the reader accepts odc and newc
 * (070701). Both stream: nothing holds a whole file. */

typedef gboolean (*AirdropSink)(const guint8 *buf, gsize len, gpointer user, GError **error);

typedef struct CpioWriter CpioWriter;
CpioWriter *cpio_writer_new(AirdropSink sink, gpointer user);
gboolean cpio_writer_dir(CpioWriter *w, const char *path, GError **error);
/* A regular file of exactly `size` bytes, given next through file_data. */
gboolean cpio_writer_file(CpioWriter *w, const char *path, guint64 size, gint64 mtime, GError **error);
gboolean cpio_writer_file_data(CpioWriter *w, const guint8 *buf, gsize len, GError **error);
gboolean cpio_writer_finish(CpioWriter *w, GError **error);   /* TRAILER!!! */
void cpio_writer_free(CpioWriter *w);

typedef enum { CPIO_DIR, CPIO_FILE } CpioType;

typedef struct {
    /* An entry's header; return FALSE with an error to refuse it. The
     * path is the archive's, such as "./photo.jpg", and the reader has
     * already refused absolute paths, ".." and any type but these two. */
    gboolean (*begin)(const char *path, CpioType type, guint64 size, gpointer user, GError **error);
    gboolean (*data)(const guint8 *buf, gsize len, gpointer user, GError **error);
    gboolean (*end)(gpointer user, GError **error);
} CpioCallbacks;

#define CPIO_MAX_ENTRIES 4096
#define CPIO_MAX_NAME    1024

typedef struct CpioReader CpioReader;
CpioReader *cpio_reader_new(const CpioCallbacks *cb, gpointer user);
/* Feed archive bytes in any pieces. *done turns TRUE once TRAILER!!! has
 * been read (UP-11); bytes after it are ignored. */
gboolean cpio_reader_feed(CpioReader *r, const guint8 *buf, gsize len, gboolean *done, GError **error);
/* The archive's bytes have ended: fails unless TRAILER!!! was read. */
gboolean cpio_reader_finish(CpioReader *r, GError **error);
void cpio_reader_free(CpioReader *r);

/* ---- Upload compression: gzip (application/x-cpio) or DVZip
 * (application/x-dvzip), UP-1, §6.6. Built on GIO's zlib converters. ---- */

typedef struct AirdropInflater AirdropInflater;
/* Decompresses an /Upload body and hands the archive bytes to `sink`,
 * failing once more than `max_output` bytes come out (SEC-3). */
AirdropInflater *airdrop_inflater_new(gboolean dvzip, guint64 max_output, AirdropSink sink, gpointer user);
gboolean airdrop_inflater_feed(AirdropInflater *z, const guint8 *buf, gsize len, GError **error);
gboolean airdrop_inflater_finish(AirdropInflater *z, GError **error);
void airdrop_inflater_free(AirdropInflater *z);

typedef struct AirdropDeflater AirdropDeflater;
/* Compresses archive bytes into an /Upload body: DVZip in 64 KiB pieces,
 * each stored when zlib doesn't shrink it (DVZ-1, DVZ-2), or gzip. */
AirdropDeflater *airdrop_deflater_new(gboolean dvzip, AirdropSink sink, gpointer user);
gboolean airdrop_deflater_feed(AirdropDeflater *z, const guint8 *buf, gsize len, GError **error);
gboolean airdrop_deflater_finish(AirdropDeflater *z, GError **error);
void airdrop_deflater_free(AirdropDeflater *z);

/* ---- HTTP/1.1 framing, §6.1, SEC-7 ----
 *
 * A push parser for one message at a time, requests on the receiver's side
 * and responses on the sender's. Strict per RFC 9112: no conflicting or
 * duplicate Content-Length, not both it and chunked, hex chunk sizes only,
 * and a cap on header bytes. A message with neither is empty, except a
 * response, which then runs to the connection's end. */

typedef enum { HTTP_REQUEST, HTTP_RESPONSE } HttpKind;

typedef struct {
    /* Headers are complete: names are lowercase. `method` and `path` are
     * set for a request, `status` for a response. */
    gboolean (*head)(const char *method, const char *path, guint status, GHashTable *headers,
                     gpointer user, GError **error);
    gboolean (*body)(const guint8 *buf, gsize len, gpointer user, GError **error);
    gboolean (*done)(gpointer user, GError **error);
} HttpCallbacks;

#define HTTP_MAX_HEAD (16 * 1024)

typedef struct HttpParser HttpParser;
HttpParser *http_parser_new(HttpKind kind, const HttpCallbacks *cb, gpointer user);
/* Consumes bytes up to the end of the current message and sets *used;
 * after `done`, call http_parser_reset() for the next message on the
 * connection, and feed it the rest. */
gboolean http_parser_feed(HttpParser *p, const guint8 *buf, gsize len, gsize *used, GError **error);
/* The connection closed: completes a response that runs to the end. */
gboolean http_parser_eof(HttpParser *p, GError **error);
void http_parser_reset(HttpParser *p);
void http_parser_free(HttpParser *p);

/* Chunked transfer coding: one chunk, or the last ("0\r\n\r\n") for len 0. */
GBytes *http_chunk(const guint8 *buf, gsize len);
