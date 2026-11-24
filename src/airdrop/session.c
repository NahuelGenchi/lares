// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* AirDrop's exchange over TLS, following owly's sender and receiver
 * (crates/owly-airdrop, MIT): what each request carries, the receiver's
 * per-connection slots (HTTP-7), how an upload is checked against its /Ask
 * and unpacked into a staging folder, and the timeouts on both sides. Rule
 * IDs are those of owly's AirDrop specification. */
#define _GNU_SOURCE
#include "session.h"
#include "formats.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MODEL "Linux"                        /* Receiver/SenderModelName, as owly sends */
#define IO_TIMEOUT 30                        /* s: a peer that stops reading or writing (SEC-10) */
#define CONNECT_TIMEOUT 10                   /* s */
/* A /Discover that isn't answered soon is tried again on a new connection
 * rather than waited for: over a fixed radio a phone is there about once a
 * second, and a fresh connection's first SYN goes at once, where a lost one
 * is resent only after the kernel's 1 s, then 3 s, backoff. */
#define DISCOVER_CONNECT_TIMEOUT 3            /* s */
#define DISCOVER_TIMEOUT 6                    /* s */
#define PROMPT_TIMEOUT (40 * G_USEC_PER_SEC) /* under the ~45 s an Apple sender waits (ASK-2, ASK-2a) */
#define PROMPT_SLICE (500 * G_TIME_SPAN_MILLISECOND)
#define SENDER_ASK_TIMEOUT 45                /* s, longer than a receiver's 15 s (ASK-2a) */
#define UPLOAD_RESPONSE_TIMEOUT 60           /* s */
#define DRAIN_TIMEOUT 2                      /* s, reading what follows a complete archive (UP-11) */
#define MAX_MESSAGE (1024 * 1024)            /* a /Discover or /Ask body, and a response */
#define MAX_PENDING (1024 * 1024)            /* bytes buffered while a prompt is up */
#define MAX_UPLOAD (16ULL << 30)             /* an upload, unpacked (SEC-3) */
#define PROGRESS_EVERY (250 * G_TIME_SPAN_MILLISECOND)
#define READ_CHUNK (64 * 1024)

/* ------------------------------------------------------------ identity */

GTlsCertificate *airdrop_identity(GError **error)
{
    char *dir = g_build_filename(g_get_user_data_dir(), "lares", "airdrop", NULL);
    char *cert = g_build_filename(dir, "cert.pem", NULL), *key = g_build_filename(dir, "key.pem", NULL);
    GTlsCertificate *c = NULL;
    if (!g_file_test(cert, G_FILE_TEST_EXISTS) || !g_file_test(key, G_FILE_TEST_EXISTS)) {
        g_mkdir_with_parents(dir, 0700);
        /* A P-256 key, as owly's; the subject says nothing about the user. */
        const char *argv[] = { "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                               "ec_paramgen_curve:prime256v1", "-nodes", "-days", "3650", "-subj", "/CN=Lares",
                               "-keyout", key, "-out", cert, NULL };
        int status = 0;
        gboolean ran = g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                                    | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, error);
        /* The folder is ours alone already; the key is too. */
        g_chmod(key, 0600);
        if (!ran || !g_spawn_check_wait_status(status, NULL)) {
            if (ran) g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't make a certificate with openssl.");
            g_unlink(key);
            g_unlink(cert);
            goto out;
        }
    }
    c = g_tls_certificate_new_from_files(cert, key, error);
out:
    g_free(dir);
    g_free(cert);
    g_free(key);
    return c;
}

/* owly's table (proto/sender.rs), for FileType. */
const char *airdrop_uti(const char *name, gboolean is_directory)
{
    static const struct { const char *ext, *uti; } TABLE[] = {
        { "jpg", "public.jpeg" }, { "jpeg", "public.jpeg" }, { "png", "public.png" },
        { "gif", "com.compuserve.gif" }, { "heic", "public.heic" }, { "heif", "public.heif" },
        { "tif", "public.tiff" }, { "tiff", "public.tiff" }, { "webp", "org.webmproject.webp" },
        { "mov", "com.apple.quicktime-movie" }, { "mp4", "public.mpeg-4" }, { "m4a", "com.apple.m4a-audio" },
        { "mp3", "public.mp3" }, { "wav", "com.microsoft.waveform-audio" }, { "pdf", "com.adobe.pdf" },
        { "txt", "public.plain-text" }, { "text", "public.plain-text" }, { "md", "net.daringfireball.markdown" },
        { "html", "public.html" }, { "htm", "public.html" }, { "rtf", "public.rtf" }, { "vcf", "public.vcard" },
        { "ics", "com.apple.ical.ics" }, { "zip", "public.zip-archive" }, { "json", "public.json" },
        { "csv", "public.comma-separated-values-text" }, { "doc", "com.microsoft.word.doc" },
        { "docx", "org.openxmlformats.wordprocessingml.document" }, { "xls", "com.microsoft.excel.xls" },
        { "xlsx", "org.openxmlformats.spreadsheetml.sheet" }, { "ppt", "com.microsoft.powerpoint.ppt" },
        { "pptx", "org.openxmlformats.presentationml.presentation" }, { "key", "com.apple.keynote.key" },
        { "pages", "com.apple.iwork.pages.sffpages" }, { "numbers", "com.apple.iwork.numbers.sffnumbers" },
    };
    if (is_directory) return "public.folder";
    const char *dot = strrchr(name, '.');
    if (!dot) return "public.data";
    for (gsize i = 0; i < G_N_ELEMENTS(TABLE); i++)
        if (!g_ascii_strcasecmp(dot + 1, TABLE[i].ext)) return TABLE[i].uti;
    return "public.data";
}

/* ------------------------------------------------------------- helpers */

static GVariant *v_str(const char *s) { return g_variant_new_string(s ? s : ""); }

static GVariant *v_data(const void *buf, gsize len)
{
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, buf, len, 1);
}

static GVariant *v_empty_dict(void) { return g_variant_new("a{sv}", NULL); }

/* A dict value, NULL when it's missing or of another type. */
static GVariant *lookup(GVariant *dict, const char *key, const GVariantType *type)
{
    if (!dict || !g_variant_is_of_type(dict, G_VARIANT_TYPE("a{sv}"))) return NULL;
    return g_variant_lookup_value(dict, key, type);
}

static char *lookup_str(GVariant *dict, const char *key)
{
    GVariant *v = lookup(dict, key, G_VARIANT_TYPE_STRING);
    char *s = v ? g_variant_dup_string(v, NULL) : NULL;
    if (v) g_variant_unref(v);
    return s;
}

static GBytes *response_bytes(guint status, GBytes *body, gboolean close)
{
    const char *reason = status == 100 ? "Continue" : status == 200 ? "OK" : status == 400 ? "Bad Request"
                       : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 406 ? "Not Acceptable"
                       : status == 413 ? "Content Too Large" : "Internal Server Error";
    gsize len = body ? g_bytes_get_size(body) : 0;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "HTTP/1.1 %u %s\r\n", status, reason);
    if (body) g_string_append(s, "Content-Type: application/octet-stream\r\n");
    if (close) g_string_append(s, "Connection: close\r\n");
    g_string_append_printf(s, "Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n", len);
    if (body) g_string_append_len(s, g_bytes_get_data(body, NULL), len);
    return g_string_free_to_bytes(s);
}

static gboolean write_bytes(GOutputStream *out, GBytes *b, GCancellable *cancel, GError **error)
{
    gsize len;
    const void *data = g_bytes_get_data(b, &len);
    return g_output_stream_write_all(out, data, len, NULL, cancel, error) && g_output_stream_flush(out, cancel, error);
}

static void set_timeout(GSocketConnection *conn, guint seconds)
{
    g_socket_set_timeout(g_socket_connection_get_socket(conn), seconds);
}

static gboolean accept_any(GTlsConnection *tls, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer data)
{
    return TRUE; /* TLS-2: encryption only, never peer authentication */
}

/* TLS connections made with no certificate database from the start: no
 * certificate is ever checked against one (TLS-2), and a connection given
 * none only after it's made has already loaded the system's, about 6.5 MB
 * that stay for good. Made this way, a connection costs about 0.3 MB. */
static GIOStream *tls_server(GIOStream *base, GTlsCertificate *cert, GError **error)
{
    GType type = g_tls_backend_get_server_connection_type(g_tls_backend_get_default());
    /* Asked for, Apple senders present a certificate; nothing depends on
     * it (TLS-3), and any is accepted. */
    GIOStream *tls = g_initable_new(type, NULL, error, "base-io-stream", base, "certificate", cert, "database", NULL,
                                    "authentication-mode", G_TLS_AUTHENTICATION_REQUESTED, NULL);
    if (tls) g_signal_connect(tls, "accept-certificate", G_CALLBACK(accept_any), NULL);
    return tls;
}

static GIOStream *tls_client(GIOStream *base, GError **error)
{
    GType type = g_tls_backend_get_client_connection_type(g_tls_backend_get_default());
    GIOStream *tls = g_initable_new(type, NULL, error, "base-io-stream", base, "database", NULL, NULL);
    if (tls) g_signal_connect(tls, "accept-certificate", G_CALLBACK(accept_any), NULL);
    return tls;
}

/* --------------------------------------------------------------- receiver */

struct AirdropReceiver {
    gint refs;
    GSocketService *service;
    GTlsCertificate *cert;
    char *name, *folder;
    guint16 port;
    AirdropReceiverCallbacks cb;
    gpointer user;
    GMainContext *ctx;
    GCancellable *cancel;
    GMutex lock;
    GCond cond;
    GHashTable *answers;   /* id -> GINT: 0 waiting, 1 accept, 2 decline */
    gboolean contacts_only;
    GHashTable *contact_hashes; /* lowercase 64-char SHA-256 hex -> self */
    guint next_id;
    gboolean closed;       /* main thread only */
};

static AirdropReceiver *receiver_ref(AirdropReceiver *r)
{
    g_atomic_int_inc(&r->refs);
    return r;
}

static void receiver_unref(AirdropReceiver *r)
{
    if (!g_atomic_int_dec_and_test(&r->refs)) return;
    g_clear_object(&r->service);
    g_clear_object(&r->cert);
    g_clear_object(&r->cancel);
    g_hash_table_unref(r->answers);
    if (r->contact_hashes) g_hash_table_unref(r->contact_hashes);
    g_mutex_clear(&r->lock);
    g_cond_clear(&r->cond);
    g_main_context_unref(r->ctx);
    g_free(r->name);
    g_free(r->folder);
    g_free(r);
}

/* What a connection's thread tells the main context. */
typedef enum { NOTE_OFFER, NOTE_WITHDRAWN, NOTE_PROGRESS, NOTE_RECEIVED, NOTE_FAILED, NOTE_TRACE } NoteKind;

typedef struct {
    AirdropReceiver *r;
    NoteKind kind;
    AirdropOffer offer;    /* NOTE_OFFER, and NOTE_RECEIVED's items in offer.names */
    guint64 done, total;
    char *message;
} Note;

static void note_free(gpointer data)
{
    Note *n = data;
    g_free(n->offer.sender);
    g_free(n->offer.sender_model);
    g_free(n->offer.matched_hash);
    if (n->offer.names) g_ptr_array_unref(n->offer.names);
    if (n->offer.file_icon) g_bytes_unref(n->offer.file_icon);
    g_free(n->message);
    receiver_unref(n->r);
    g_free(n);
}

static gboolean note_deliver(gpointer data)
{
    Note *n = data;
    AirdropReceiver *r = n->r;
    if (r->closed) return G_SOURCE_REMOVE;
    guint id = n->offer.id;
    switch (n->kind) {
    case NOTE_OFFER: if (r->cb.offer) r->cb.offer(&n->offer, r->user); break;
    case NOTE_WITHDRAWN: if (r->cb.withdrawn) r->cb.withdrawn(id, r->user); break;
    case NOTE_PROGRESS: if (r->cb.progress) r->cb.progress(id, n->done, n->total, r->user); break;
    case NOTE_RECEIVED: if (r->cb.received) r->cb.received(id, n->offer.links, n->offer.names, r->user); break;
    case NOTE_FAILED: if (r->cb.failed) r->cb.failed(id, n->message, r->user); break;
    case NOTE_TRACE: if (r->cb.trace) r->cb.trace(id, n->message, r->user); break;
    }
    return G_SOURCE_REMOVE;
}

static void note(AirdropReceiver *r, Note *n)
{
    n->r = receiver_ref(r);
    g_main_context_invoke_full(r->ctx, G_PRIORITY_DEFAULT, note_deliver, n, note_free);
}

/* An item the accepted /Ask lists, as UP-7 checks archive entries against. */
typedef struct {
    char *name;            /* FileName: what's saved */
    char *path;            /* FileBomPath without "./" */
    char *wrapper;         /* a regular file's one-folder wrapper, or NULL (ASK-5) */
    gboolean dir;
} Listed;

typedef enum { ROUTE_NONE, ROUTE_DISCOVER, ROUTE_ASK, ROUTE_UPLOAD, ROUTE_OTHER, ROUTE_GET } Route;

typedef struct {
    AirdropReceiver *r;
    GSocketConnection *conn;
    GIOStream *tls;
    GInputStream *in;
    GOutputStream *out;
    HttpParser *http;
    GByteArray *input;     /* read but not yet parsed */
    gboolean eof, closing;

    /* the message being parsed */
    Route route;
    gboolean msg_done, too_big;
    guint fail;            /* a status to answer at the message's end */
    GByteArray *body;

    /* HTTP-7's slots */
    gboolean slot_discover, slot_upload;
    gboolean accepted;
    guint id;
    char *transfer_id;
    GPtrArray *listed;     /* Listed* */
    gboolean links;
    guint64 total;

    /* the upload */
    AirdropInflater *inflate;
    CpioReader *cpio;
    char *staging;
    int fd;                /* the file being written, or -1 */
    gboolean skipping;     /* an AppleDouble member, discarded (UP-7a) */
    gboolean archive_done;
    guint64 written;
    gint64 last_progress;
    GError *upload_error;

    /* what the upload looked like, for the trace: never a name */
    guint64 raw_in, archive_in;
    guint8 raw_head[4], archive_head[6];
    guint entries;
} Conn;

static void G_GNUC_PRINTF(2, 3) trace(Conn *c, const char *fmt, ...)
{
    if (!c->r->cb.trace) return;
    Note *n = g_new0(Note, 1);
    n->kind = NOTE_TRACE;
    n->offer.id = c->id;
    va_list ap;
    va_start(ap, fmt);
    n->message = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    note(c->r, n);
}

/* A sender's header value as protocol text only, for the trace. */
static char *plain(const char *value)
{
    GString *s = g_string_new(NULL);
    for (const char *p = value ? value : ""; *p && s->len < 40; p++)
        g_string_append_c(s, g_ascii_isalnum(*p) || strchr("/.-+;=", *p) ? *p : '?');
    return g_string_free(s, FALSE);
}

static char *hex(const guint8 *b, gsize n)
{
    GString *s = g_string_new(NULL);
    for (gsize i = 0; i < n; i++) g_string_append_printf(s, "%02x", b[i]);
    return g_string_free(s, FALSE);
}

/* The extension of the last component, lowercased, or "" : what an entry and
 * a listed name are compared by in the trace. */
static char *ext_of(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && dot != name && strlen(dot) <= 8 ? g_ascii_strdown(dot, -1) : g_strdup("");
}

static void listed_free(gpointer data)
{
    Listed *l = data;
    g_free(l->name);
    g_free(l->path);
    g_free(l->wrapper);
    g_free(l);
}

static gboolean send_response(Conn *c, guint status, GBytes *body, gboolean close)
{
    GBytes *b = response_bytes(status, body, close);
    gboolean ok = write_bytes(c->out, b, c->r->cancel, NULL);
    g_bytes_unref(b);
    if (close || !ok) c->closing = TRUE;
    return ok;
}

static void fail(Conn *c, guint status)
{
    send_response(c, status, NULL, TRUE);
}

/* ---- upload: archive entries into the staging folder ---- */

static gboolean refuse(GError **error, const char *why)
{
    g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, why);
    return FALSE;
}

/* The archive path's components, without "./" and empty ones; NULL if any
 * component is unsafe. "." alone gives an empty vector. */
static char **components(const char *path)
{
    char **parts = g_strsplit(path, "/", -1);
    GPtrArray *out = g_ptr_array_new();
    for (char **p = parts; *p; p++) {
        if (!**p || g_str_equal(*p, ".")) continue;
        if (!airdrop_safe_name(*p)) {
            g_ptr_array_unref(out);
            g_strfreev(parts);
            return NULL;
        }
        g_ptr_array_add(out, g_strdup(*p));
    }
    g_strfreev(parts);
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

static gboolean make_dirs(Conn *c, char **parts, guint n, GError **error)
{
    char *dir = g_strdup(c->staging);
    for (guint i = 0; i < n; i++) {
        char *next = g_build_filename(dir, parts[i], NULL);
        g_free(dir);
        dir = next;
        if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
            g_free(dir);
            return FALSE;
        }
    }
    g_free(dir);
    return TRUE;
}

static gboolean entry_begin(const char *path, CpioType type, guint64 size, gpointer user, GError **error)
{
    Conn *c = user;
    c->skipping = FALSE;
    char **parts = components(path);
    if (!parts) {
        trace(c, "entry %u: unsafe name, refused", ++c->entries);
        return refuse(error, "An archive entry has an unsafe name.");
    }
    guint n = g_strv_length(parts);
    gboolean ok = FALSE;
    if (n == 0) { /* the root entry (UP-2a) */
        ok = type == CPIO_DIR || refuse(error, "The archive's root isn't a folder.");
        goto out;
    }
    const char *last = parts[n - 1];
    /* Which listed item the entry belongs to, and where it goes. */
    Listed *owner = NULL;
    char **target = NULL;   /* components under staging */
    guint tn = 0;
    for (guint i = 0; i < c->listed->len && !owner; i++) {
        Listed *l = c->listed->pdata[i];
        if (l->wrapper) {
            if (n == 1 && g_str_equal(parts[0], l->wrapper) && type == CPIO_DIR) {
                ok = TRUE; /* the wrapper itself: nothing to create (UP-2a) */
                goto out;
            }
            if (n == 2 && g_str_equal(parts[0], l->wrapper)) {
                if (g_str_equal(last, l->name)) owner = l, target = &l->name, tn = 1;
                else if (g_str_has_prefix(last, "._")) owner = l;
            }
        } else if (l->dir) {
            if (g_str_equal(parts[0], l->name)) owner = l, target = parts, tn = n;
            else if (n == 1 && g_str_has_prefix(last, "._") && g_str_equal(last + 2, l->name)) owner = l;
        } else if (n == 1) {
            if (g_str_equal(last, l->name)) owner = l, target = parts, tn = 1;
            else if (g_str_has_prefix(last, "._") && g_str_equal(last + 2, l->name)) owner = l;
        }
    }
    if (!owner) {
        refuse(error, "The archive holds something the transfer didn't list."); /* UP-7 */
        goto out;
    }
    /* AppleDouble metadata beside a listed item, or inside a listed folder,
     * is accepted and dropped (UP-7a). */
    if (g_str_has_prefix(last, "._")) {
        c->skipping = TRUE;
        ok = type == CPIO_FILE || refuse(error, "Bad metadata entry.");
        goto out;
    }
    if (!target) {
        refuse(error, "The archive holds something the transfer didn't list.");
        goto out;
    }
    if (type == CPIO_DIR) {
        ok = owner->dir && make_dirs(c, target, tn, error);
        if (!owner->dir) refuse(error, "A listed file arrived as a folder.");
        goto out;
    }
    if (owner->dir && tn == 1) {
        refuse(error, "A listed folder arrived as a file.");
        goto out;
    }
    if (owner->dir == FALSE && tn != 1) {
        refuse(error, "Bad file entry.");
        goto out;
    }
    if (c->written + size > MAX_UPLOAD) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_LIMIT, "The transfer is too large.");
        goto out;
    }
    if (tn > 1 && !make_dirs(c, target, tn - 1, error)) goto out;
    char *rel = g_strjoinv("/", target);
    char *file = g_build_filename(c->staging, rel, NULL);
    g_free(rel);
    /* Never over anything, never through a link (SEC-1, SEC-2). */
    c->fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (c->fd < 0) g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
    g_free(file);
    ok = c->fd >= 0;
out:
    /* The first entries and any refused one, compared with the list by
     * extension and by whether the name is the same: no names. */
    if (++c->entries <= 8 || !ok) {
        char *e = ext_of(n ? parts[n - 1] : NULL);
        GString *l = g_string_new(NULL);
        for (guint i = 0; c->listed && i < c->listed->len && i < 4; i++) {
            Listed *x = c->listed->pdata[i];
            char *le = ext_of(x->name);
            g_string_append_printf(l, "%s%s%s%s", i ? ", " : "", x->wrapper ? "wrapped " : x->dir ? "folder " : "",
                                   le[0] ? le : "no extension", n && g_str_equal(parts[n - 1], x->name) ? " (same name)" : "");
            g_free(le);
        }
        trace(c, "entry %u: %u parts, %s, %" G_GUINT64_FORMAT " bytes, %s; listed: %s; %s", c->entries, n,
              type == CPIO_DIR ? "folder" : type == CPIO_FILE ? "file" : "other", size, e[0] ? e : "no extension",
              l->len ? l->str : "nothing", ok ? "taken" : "refused");
        g_string_free(l, TRUE);
        g_free(e);
    }
    g_strfreev(parts);
    return ok;
}

static void upload_progress(Conn *c, gboolean force)
{
    gint64 now = g_get_monotonic_time();
    if (!force && now - c->last_progress < PROGRESS_EVERY) return;
    c->last_progress = now;
    Note *n = g_new0(Note, 1);
    n->kind = NOTE_PROGRESS;
    n->offer.id = c->id;
    n->done = c->written;
    n->total = c->total;
    note(c->r, n);
}

static gboolean entry_data(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Conn *c = user;
    if (c->skipping) return TRUE;
    if (c->fd < 0) return refuse(error, "Data outside a file.");
    while (len) {
        ssize_t n = write(c->fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
            return FALSE;
        }
        buf += n;
        len -= n;
        c->written += n;
    }
    upload_progress(c, FALSE);
    return TRUE;
}

static gboolean entry_end(gpointer user, GError **error)
{
    Conn *c = user;
    c->skipping = FALSE;
    if (c->fd >= 0 && close(c->fd) < 0) {
        c->fd = -1;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
        return FALSE;
    }
    c->fd = -1;
    return TRUE;
}

static const CpioCallbacks CPIO = { entry_begin, entry_data, entry_end };

static gboolean to_cpio(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Conn *c = user;
    for (gsize i = 0; i < len && c->archive_in + i < sizeof c->archive_head; i++)
        c->archive_head[c->archive_in + i] = buf[i];
    c->archive_in += len;
    if (c->archive_done) return TRUE;
    return cpio_reader_feed(c->cpio, buf, len, &c->archive_done, error);
}

/* Removes what the staging folder holds. Everything in it is ours: made by
 * this connection, with no links followed. */
static void remove_tree(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            char *child = g_build_filename(path, name, NULL);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else g_unlink(child);
            g_free(child);
        }
        g_dir_close(d);
    }
    g_rmdir(path);
}

/* Moves `from` to `to` unless something is there already: 0, or an errno.
 * Where the file system has no RENAME_NOREPLACE, as WSL's mount of the
 * Windows drives (issue #107), a file goes by a hard link and an unlink, or
 * by a rename over a name taken with O_EXCL, and a folder by a rename over
 * an empty folder mkdir made: each refuses a name that's taken. */
static int move_noreplace(const char *from, const char *to)
{
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0) return 0;
    int err = errno;
    if (err != EINVAL && err != ENOSYS && err != EOPNOTSUPP) return err;
    struct stat st;
    if (lstat(from, &st) < 0) return errno;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, 0700) < 0) return errno;
    } else if (link(from, to) == 0) {
        unlink(from);
        return 0;
    } else if (errno == EPERM || errno == EOPNOTSUPP || errno == ENOSYS) { /* no hard links */
        int fd = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return errno;
        close(fd);
    } else {
        return errno;
    }
    if (rename(from, to) == 0) return 0;
    err = errno;
    if (S_ISDIR(st.st_mode)) rmdir(to);
    else unlink(to);
    return err;
}

/* `name` in `folder`, or "name (2).ext" and so on, moved without ever
 * replacing anything (SEC-2). Returns the path it got, or NULL. */
static char *move_unique(const char *from, const char *folder, const char *name)
{
    const char *dot = strrchr(name, '.');
    gboolean ext = dot && dot != name;
    char *stem = ext ? g_strndup(name, dot - name) : g_strdup(name);
    for (int i = 1; i < 1000; i++) {
        char *candidate = i == 1 ? g_strdup(name) : g_strdup_printf("%s (%d)%s", stem, i, ext ? dot : "");
        char *to = g_build_filename(folder, candidate, NULL);
        g_free(candidate);
        int err = move_noreplace(from, to);
        if (err == 0) {
            g_free(stem);
            return to;
        }
        g_free(to);
        if (err != EEXIST) break;
    }
    g_free(stem);
    return NULL;
}

static void upload_reset(Conn *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    g_clear_pointer(&c->inflate, airdrop_inflater_free);
    g_clear_pointer(&c->cpio, cpio_reader_free);
    if (c->staging) remove_tree(c->staging);
    g_clear_pointer(&c->staging, g_free);
}

static void upload_failed(Conn *c, const char *message)
{
    char *raw = hex(c->raw_head, MIN(c->raw_in, sizeof c->raw_head));
    char *arc = hex(c->archive_head, MIN(c->archive_in, sizeof c->archive_head));
    trace(c, "upload failed: %" G_GUINT64_FORMAT " bytes in (starting %s), %" G_GUINT64_FORMAT
          " unpacked (starting %s), %u entries, %" G_GUINT64_FORMAT " written, archive %s",
          c->raw_in, raw[0] ? raw : "-", c->archive_in, arc[0] ? arc : "-", c->entries, c->written,
          c->archive_done ? "complete" : "not complete");
    g_free(raw);
    g_free(arc);
    upload_reset(c);
    Note *n = g_new0(Note, 1);
    n->kind = NOTE_FAILED;
    n->offer.id = c->id;
    n->message = g_strdup(message);
    note(c->r, n);
}

/* The archive is complete: everything moves from staging into place. */
static void upload_finish(Conn *c)
{
    GPtrArray *saved = g_ptr_array_new_with_free_func(g_free);
    gboolean ok = TRUE;
    for (guint i = 0; i < c->listed->len && ok; i++) {
        Listed *l = c->listed->pdata[i];
        char *from = g_build_filename(c->staging, l->name, NULL);
        if (g_file_test(from, G_FILE_TEST_EXISTS)) {
            char *to = move_unique(from, c->r->folder, l->name);
            if (to) g_ptr_array_add(saved, to);
            else ok = FALSE;
        }
        g_free(from);
    }
    upload_progress(c, TRUE);
    if (!ok) {
        upload_failed(c, "Couldn't save the files.");
        g_ptr_array_unref(saved);
        fail(c, 500);
        return;
    }
    upload_reset(c);
    send_response(c, 200, NULL, TRUE); /* UP-5 */
    Note *n = g_new0(Note, 1);
    n->kind = NOTE_RECEIVED;
    n->offer.id = c->id;
    n->offer.names = saved;
    note(c->r, n);
}

/* ---- the three requests ---- */

static GBytes *encode(GVariant *v)
{
    GVariant *owned = g_variant_ref_sink(v);
    GBytes *b = bplist_encode(owned, NULL);
    g_variant_unref(owned);
    return b;
}

static GVariant *decode_body(Conn *c)
{
    if (!c->body || c->body->len == 0) return g_variant_ref_sink(v_empty_dict());
    GVariant *v = bplist_decode(c->body->data, c->body->len, NULL);
    if (v && !g_variant_is_of_type(v, G_VARIANT_TYPE("a{sv}"))) g_clear_pointer(&v, g_variant_unref);
    return v;
}

/* Matches SenderRecordData against saved contact hashes (ID-3). Never logs hashes (SEC-5). */
static char *match_sender_record(Conn *c, GVariant *req, gboolean *contacts_only_out)
{
    AirdropReceiver *r = c->r;
    char *matched = NULL;
    g_mutex_lock(&r->lock);
    *contacts_only_out = r->contacts_only;
    gboolean have = r->contact_hashes && g_hash_table_size(r->contact_hashes) > 0;
    g_mutex_unlock(&r->lock);
    if (!have) return NULL;
    GVariant *rec = lookup(req, "SenderRecordData", G_VARIANT_TYPE_BYTESTRING);
    if (!rec) return NULL;
    gsize len = 0;
    const guint8 *data = g_variant_get_fixed_array(rec, &len, 1);
    GPtrArray *hashes = airdrop_extract_record_hashes(data, len);
    g_variant_unref(rec);
    g_mutex_lock(&r->lock);
    for (guint i = 0; i < hashes->len && !matched; i++) {
        const char *h = hashes->pdata[i];
        if (r->contact_hashes && g_hash_table_contains(r->contact_hashes, h)) matched = g_strdup(h);
    }
    g_mutex_unlock(&r->lock);
    g_ptr_array_unref(hashes);
    return matched;
}

static void on_discover(Conn *c)
{
    /* HTTP-7: once per connection, and only first. */
    if (c->slot_discover || c->accepted || c->slot_upload) return fail(c, 400);
    GVariant *request = decode_body(c);
    if (!request) return fail(c, 400);
    c->slot_discover = TRUE;
    gboolean contacts_only = FALSE;
    char *matched = match_sender_record(c, request, &contacts_only);
    g_variant_unref(request);
    if (contacts_only && !matched) {
        /* DISC-2 / ID-3: Contacts Only with no match answers 401 and no name. */
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "StatusCode", g_variant_new_int64(401));
        GBytes *body = encode(g_variant_builder_end(&b));
        if (body) send_response(c, 200, body, FALSE);
        else fail(c, 500);
        if (body) g_bytes_unref(body);
        return;
    }
    /* Everyone (100) or matched Contact (200) (ID-3, ID-4). */
    static const char CAPS[] = "{\"Version\":1}"; /* DISC-3: JPEG rather than HEIC */
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "StatusCode", g_variant_new_int64(matched ? 200 : 100));
    g_variant_builder_add(&b, "{sv}", "ReceiverComputerName", v_str(c->r->name));
    g_variant_builder_add(&b, "{sv}", "ReceiverModelName", v_str(MODEL));
    g_variant_builder_add(&b, "{sv}", "ReceiverMediaCapabilities", v_data(CAPS, sizeof CAPS - 1));
    g_free(matched);
    GBytes *body = encode(g_variant_builder_end(&b));
    if (body) send_response(c, 200, body, FALSE);
    else fail(c, 500);
    if (body) g_bytes_unref(body);
}

/* ASK-5: a file's FileBomPath is "./<FileName>", or "./<folder>/<FileName>"
 * for a regular file; a folder's is "./<FileName>". */
static Listed *listed_from(GVariant *file)
{
    char *name = lookup_str(file, "FileName"), *bom = lookup_str(file, "FileBomPath");
    GVariant *isdir = lookup(file, "FileIsDirectory", G_VARIANT_TYPE_BOOLEAN);
    gboolean dir = isdir && g_variant_get_boolean(isdir);
    if (isdir) g_variant_unref(isdir);
    Listed *l = NULL;
    if (!name || !airdrop_safe_name(name)) goto out;
    if (!bom) bom = g_strconcat("./", name, NULL);
    char **parts = g_str_has_prefix(bom, "./") ? components(bom) : NULL;
    guint n = parts ? g_strv_length(parts) : 0;
    if (n == 1 && g_str_equal(parts[0], name)) {
        l = g_new0(Listed, 1);
        l->path = g_strdup(parts[0]);
    } else if (n == 2 && !dir && g_str_equal(parts[1], name)) {
        l = g_new0(Listed, 1);
        l->path = g_strjoinv("/", parts);
        l->wrapper = g_strdup(parts[0]);
    }
    g_strfreev(parts);
    if (l) {
        l->name = g_strdup(name);
        l->dir = dir;
    }
out:
    g_free(name);
    g_free(bom);
    return l;
}

/* Waits for the user's answer, the prompt's timeout, or the sender giving
 * up, whichever comes first. Bytes that arrive meanwhile are kept for the
 * parser, up to a bound. Returns 1 accept, 2 decline, 0 gone. */
static int wait_answer(Conn *c)
{
    AirdropReceiver *r = c->r;
    gint64 end = g_get_monotonic_time() + PROMPT_TIMEOUT;
    GSocket *sock = g_socket_connection_get_socket(c->conn);
    int answer = 0;
    g_mutex_lock(&r->lock);
    for (;;) {
        answer = GPOINTER_TO_INT(g_hash_table_lookup(r->answers, GUINT_TO_POINTER(c->id)));
        if (answer || g_cancellable_is_cancelled(r->cancel)) break;
        gint64 now = g_get_monotonic_time();
        if (now >= end) break;
        g_cond_wait_until(&r->cond, &r->lock, MIN(end, now + PROMPT_SLICE));
        answer = GPOINTER_TO_INT(g_hash_table_lookup(r->answers, GUINT_TO_POINTER(c->id)));
        if (answer) break;
        g_mutex_unlock(&r->lock);
        GIOCondition cond = g_socket_condition_check(sock, G_IO_IN | G_IO_HUP | G_IO_ERR);
        gboolean gone = (cond & (G_IO_HUP | G_IO_ERR)) != 0;
        if (!gone && (cond & G_IO_IN)) {
            guint8 buf[4096];
            gssize n = g_input_stream_read(c->in, buf, sizeof buf, r->cancel, NULL);
            if (n <= 0 || c->input->len + n > MAX_PENDING) gone = TRUE;
            else g_byte_array_append(c->input, buf, n);
        }
        g_mutex_lock(&r->lock);
        if (gone) {
            answer = -1;
            break;
        }
    }
    g_hash_table_remove(r->answers, GUINT_TO_POINTER(c->id));
    g_mutex_unlock(&r->lock);
    return answer;
}

static void on_ask(Conn *c)
{
    c->slot_discover = FALSE; /* HTTP-7 */
    if (c->accepted || c->slot_upload) return fail(c, 400);
    GVariant *req = decode_body(c);
    if (!req) return fail(c, 400);
    GVariant *tid = lookup(req, "TransferID", NULL);
    if (tid && g_variant_is_of_type(tid, G_VARIANT_TYPE("a{sv}"))) c->transfer_id = lookup_str(tid, "id");
    else if (tid && g_variant_is_of_type(tid, G_VARIANT_TYPE_STRING)) c->transfer_id = g_variant_dup_string(tid, NULL);
    if (tid) g_variant_unref(tid);

    GVariant *files = lookup(req, "Files", G_VARIANT_TYPE("av"));
    GVariant *items = lookup(req, "Items", G_VARIANT_TYPE("av"));
    gsize nfiles = files ? g_variant_n_children(files) : 0, nitems = items ? g_variant_n_children(items) : 0;
    Note *n = g_new0(Note, 1);
    n->kind = NOTE_OFFER;
    n->offer.names = g_ptr_array_new_with_free_func(g_free);
    gboolean bad = (nfiles == 0) == (nitems == 0) || nfiles > CPIO_MAX_ENTRIES || nitems > 64;
    c->listed = g_ptr_array_new_with_free_func(listed_free);
    for (gsize i = 0; i < nfiles && !bad; i++) {
        GVariant *f = g_variant_get_child_value(files, i), *fd = g_variant_get_variant(f);
        Listed *l = listed_from(fd);
        GVariant *size = lookup(fd, "FileSize", G_VARIANT_TYPE_INT64);
        if (size && g_variant_get_int64(size) > 0) c->total += g_variant_get_int64(size);
        if (size) g_variant_unref(size);
        g_variant_unref(fd);
        g_variant_unref(f);
        /* Two items saved under one name would collide in staging. */
        for (guint j = 0; l && j < c->listed->len; j++)
            if (g_str_equal(((Listed *)c->listed->pdata[j])->name, l->name)) bad = TRUE;
        if (!l || bad) {
            if (l) listed_free(l);
            bad = TRUE;
            break;
        }
        g_ptr_array_add(c->listed, l);
        g_ptr_array_add(n->offer.names, g_strdup(l->name));
    }
    for (gsize i = 0; i < nitems && !bad; i++) {
        GVariant *it = g_variant_get_child_value(items, i), *s = g_variant_get_variant(it);
        if (g_variant_is_of_type(s, G_VARIANT_TYPE_STRING)) g_ptr_array_add(n->offer.names, g_variant_dup_string(s, NULL));
        else bad = TRUE;
        g_variant_unref(s);
        g_variant_unref(it);
    }
    if (files) g_variant_unref(files);
    if (items) g_variant_unref(items);
    char *sender = lookup_str(req, "SenderComputerName");
    char *sender_model = lookup_str(req, "SenderModelName");
    gboolean contacts_only = FALSE;
    char *matched_hash = match_sender_record(c, req, &contacts_only);
    GBytes *file_icon = NULL;
    GVariant *icon = lookup(req, "FileIcon", G_VARIANT_TYPE_BYTESTRING);
    if (icon) {
        gsize ilen = 0;
        const guint8 *idata = g_variant_get_fixed_array(icon, &ilen, 1);
        if (ilen > 0 && ilen <= 512 * 1024) file_icon = g_bytes_new(idata, ilen);
        g_variant_unref(icon);
    }
    g_variant_unref(req);
    if (contacts_only && !matched_hash) {
        g_free(sender);
        g_free(sender_model);
        if (file_icon) g_bytes_unref(file_icon);
        n->r = receiver_ref(c->r);
        note_free(n);
        return fail(c, 401);
    }
    if (bad) {
        g_free(sender);
        g_free(sender_model);
        g_free(matched_hash);
        if (file_icon) g_bytes_unref(file_icon);
        n->r = receiver_ref(c->r);
        note_free(n);
        return fail(c, 400);
    }
    c->links = nitems > 0;
    AirdropReceiver *r = c->r;
    g_mutex_lock(&r->lock);
    c->id = ++r->next_id;
    g_hash_table_insert(r->answers, GUINT_TO_POINTER(c->id), GINT_TO_POINTER(0));
    g_mutex_unlock(&r->lock);
    n->offer.id = c->id;
    n->offer.sender = sender ? sender : g_strdup("Someone");
    n->offer.sender_model = sender_model;
    n->offer.matched_hash = matched_hash;
    n->offer.file_icon = file_icon;
    n->offer.links = c->links;
    n->offer.total = c->total;
    /* The note is the main thread's once sent: keep what's needed after. */
    GPtrArray *offered = g_ptr_array_ref(n->offer.names);
    note(r, n);

    int answer = wait_answer(c);
    if (answer != 1) {
        Note *w = g_new0(Note, 1);
        w->kind = NOTE_WITHDRAWN;
        w->offer.id = c->id;
        note(r, w);
        if (answer == -1) c->closing = TRUE;
        else fail(c, 403); /* ASK-2: answer a decline rather than leave the sender waiting */
        g_ptr_array_unref(offered);
        return;
    }
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "ReceiverComputerName", v_str(r->name));
    g_variant_builder_add(&b, "{sv}", "ReceiverModelName", v_str(MODEL));
    GBytes *body = encode(g_variant_builder_end(&b));
    send_response(c, 200, body, c->links);
    g_bytes_unref(body);
    c->accepted = TRUE;
    if (c->links) { /* ASK-4: a link share is complete here */
        Note *d = g_new0(Note, 1);
        d->kind = NOTE_RECEIVED;
        d->offer.id = c->id;
        d->offer.links = TRUE;
        d->offer.names = g_ptr_array_ref(offered);
        note(r, d);
    }
    g_ptr_array_unref(offered);
}

static gboolean upload_start(Conn *c, GHashTable *headers)
{
    /* UP-6: only after an accepted /Ask on this connection, once. */
    if (!c->accepted || c->links || c->slot_upload) {
        c->fail = 400;
        return TRUE;
    }
    const char *type = g_hash_table_lookup(headers, "content-type");
    gboolean dvzip = type && !g_ascii_strcasecmp(type, "application/x-dvzip");
    char *t = plain(type), *te = plain(g_hash_table_lookup(headers, "transfer-encoding")),
         *cl = plain(g_hash_table_lookup(headers, "content-length")),
         *ce = plain(g_hash_table_lookup(headers, "content-encoding"));
    trace(c, "upload: type %s, transfer-encoding %s, length %s, content-encoding %s, %u listed",
          t, te[0] ? te : "-", cl[0] ? cl : "-", ce[0] ? ce : "-", c->listed ? c->listed->len : 0);
    g_free(t);
    g_free(te);
    g_free(cl);
    g_free(ce);
    if (!dvzip && !(type && !g_ascii_strcasecmp(type, "application/x-cpio"))) {
        c->fail = 406; /* UP-4 */
        return TRUE;
    }
    const char *tid = g_hash_table_lookup(headers, "transferid");
    if (c->transfer_id && !(tid && !g_ascii_strcasecmp(tid, c->transfer_id))) { /* UP-9 */
        c->fail = 400;
        return TRUE;
    }
    char *tmpl = g_build_filename(c->r->folder, ".lares-airdrop-XXXXXX", NULL);
    c->staging = g_mkdtemp_full(tmpl, 0700);
    if (!c->staging) {
        g_free(tmpl);
        c->fail = 500;
        return TRUE;
    }
    c->slot_upload = TRUE;
    c->cpio = cpio_reader_new(&CPIO, c);
    c->inflate = airdrop_inflater_new(dvzip, MAX_UPLOAD, to_cpio, c);
    const char *expect = g_hash_table_lookup(headers, "expect");
    if (expect && !g_ascii_strcasecmp(expect, "100-continue")) { /* UP-3 */
        GBytes *b = g_bytes_new_static("HTTP/1.1 100 Continue\r\n\r\n", 25);
        write_bytes(c->out, b, c->r->cancel, NULL);
        g_bytes_unref(b);
    }
    return TRUE;
}

static gboolean http_head(const char *method, const char *path, guint status, GHashTable *headers,
                          gpointer user, GError **error)
{
    Conn *c = user;
    c->fail = 0;
    c->too_big = FALSE;
    if (c->body) g_byte_array_set_size(c->body, 0);
    if (g_str_equal(method, "GET") || g_str_equal(method, "HEAD")) c->route = ROUTE_GET; /* HTTP-5 */
    else if (!g_str_equal(method, "POST")) c->route = ROUTE_OTHER;
    else if (g_str_equal(path, "/Discover")) c->route = ROUTE_DISCOVER;
    else if (g_str_equal(path, "/Ask")) c->route = ROUTE_ASK;
    else if (g_str_equal(path, "/Upload")) c->route = ROUTE_UPLOAD;
    else c->route = ROUTE_OTHER;
    if (c->route == ROUTE_UPLOAD) return upload_start(c, headers);
    return TRUE;
}

static gboolean http_body(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Conn *c = user;
    if (c->route == ROUTE_UPLOAD) {
        for (gsize i = 0; i < len && c->raw_in + i < sizeof c->raw_head; i++) c->raw_head[c->raw_in + i] = buf[i];
        c->raw_in += len;
        if (c->fail || !c->inflate || c->upload_error) return TRUE; /* discarded; answered at its end */
        if (!airdrop_inflater_feed(c->inflate, buf, len, &c->upload_error)) return TRUE;
        return TRUE;
    }
    if (c->route == ROUTE_DISCOVER || c->route == ROUTE_ASK) {
        if (c->body->len + len > MAX_MESSAGE) c->too_big = TRUE;
        else g_byte_array_append(c->body, buf, len);
    }
    return TRUE; /* any other body is read and dropped */
}

static gboolean http_done(gpointer user, GError **error)
{
    ((Conn *)user)->msg_done = TRUE;
    return TRUE;
}

static const HttpCallbacks SERVER_HTTP = { http_head, http_body, http_done };

/* A whole message has been read: answer it. */
static void dispatch(Conn *c)
{
    if (c->too_big) return fail(c, 413);
    switch (c->route) {
    case ROUTE_DISCOVER: return on_discover(c);
    case ROUTE_ASK: return on_ask(c);
    case ROUTE_GET: send_response(c, 200, NULL, FALSE); return;
    case ROUTE_OTHER: return fail(c, 400); /* HTTP-5 */
    case ROUTE_UPLOAD:
        if (c->fail) return fail(c, c->fail);
        if (!c->upload_error && c->inflate) airdrop_inflater_finish(c->inflate, &c->upload_error);
        if (!c->upload_error && !c->archive_done)
            g_set_error_literal(&c->upload_error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The archive ended early.");
        if (c->upload_error) {
            upload_failed(c, c->upload_error->message);
            g_clear_error(&c->upload_error);
            return fail(c, 400);
        }
        return; /* finished as soon as the archive completed */
    default: return;
    }
}

static void serve(Conn *c)
{
    AirdropReceiver *r = c->r;
    set_timeout(c->conn, IO_TIMEOUT);
    GIOStream *tls = tls_server(G_IO_STREAM(c->conn), r->cert, NULL);
    if (!tls) return;
    if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), r->cancel, NULL)) {
        g_object_unref(tls);
        return;
    }
    c->tls = tls;
    c->in = g_io_stream_get_input_stream(tls);
    c->out = g_io_stream_get_output_stream(tls);
    c->http = http_parser_new(HTTP_REQUEST, &SERVER_HTTP, c);
    c->input = g_byte_array_new();
    c->body = g_byte_array_new();
    c->fd = -1;
    guint8 *buf = g_malloc(READ_CHUNK);
    while (!c->closing && !g_cancellable_is_cancelled(r->cancel)) {
        if (c->input->len == 0) {
            gssize n = g_input_stream_read(c->in, buf, READ_CHUNK, r->cancel, NULL);
            if (n <= 0) break;
            g_byte_array_append(c->input, buf, n);
        }
        gsize used = 0;
        GError *e = NULL;
        gboolean ok = http_parser_feed(c->http, c->input->data, c->input->len, &used, &e);
        g_byte_array_remove_range(c->input, 0, used);
        if (!ok) {
            g_clear_error(&e);
            if (c->slot_upload && c->staging) upload_failed(c, "The transfer was malformed.");
            fail(c, 400);
            break;
        }
        /* A refused upload is answered at once, not after its whole body. */
        if (c->route == ROUTE_UPLOAD && c->fail) {
            fail(c, c->fail);
            break;
        }
        /* UP-11: complete once TRAILER!!! is read, whatever follows. What
         * does follow is read for a moment, so closing doesn't reset the
         * connection under the answer. */
        if (c->route == ROUTE_UPLOAD && c->archive_done && c->staging && !c->upload_error) {
            upload_finish(c);
            set_timeout(c->conn, DRAIN_TIMEOUT);
            gsize drained = 0;
            gssize n;
            while (drained < MAX_PENDING && (n = g_input_stream_read(c->in, buf, READ_CHUNK, r->cancel, NULL)) > 0)
                drained += n;
            break;
        }
        if (c->route == ROUTE_UPLOAD && c->upload_error && c->staging) {
            upload_failed(c, c->upload_error->message);
            g_clear_error(&c->upload_error);
            fail(c, 400);
            break;
        }
        if (c->msg_done) {
            c->msg_done = FALSE;
            dispatch(c);
            http_parser_reset(c->http);
            c->route = ROUTE_NONE;
        }
    }
    if (c->slot_upload && c->staging) upload_failed(c, "The sender stopped.");
    upload_reset(c);
    g_free(buf);
    g_io_stream_close(tls, NULL, NULL);
}

static void closure_unref(gpointer data, GClosure *closure)
{
    receiver_unref(data);
}

static gboolean on_run(GThreadedSocketService *s, GSocketConnection *conn, GObject *src, gpointer data)
{
    AirdropReceiver *r = receiver_ref(data);
    Conn c = { .r = r, .conn = conn, .fd = -1 };
    serve(&c);
    if (c.http) http_parser_free(c.http);
    if (c.input) g_byte_array_unref(c.input);
    if (c.body) g_byte_array_unref(c.body);
    if (c.listed) g_ptr_array_unref(c.listed);
    g_clear_error(&c.upload_error);
    g_free(c.transfer_id);
    g_clear_object(&c.tls);
    receiver_unref(r);
    return TRUE;
}

AirdropReceiver *airdrop_receiver_new(const char *address, const char *name, const char *folder,
                                      GTlsCertificate *cert, const AirdropReceiverCallbacks *cb,
                                      gpointer user, GError **error)
{
    AirdropReceiver *r = g_new0(AirdropReceiver, 1);
    r->refs = 1;
    r->cert = g_object_ref(cert);
    r->name = g_strdup(name);
    r->folder = g_strdup(folder);
    r->cb = *cb;
    r->user = user;
    r->ctx = g_main_context_ref_thread_default();
    r->cancel = g_cancellable_new();
    r->answers = g_hash_table_new(NULL, NULL);
    r->contact_hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_mutex_init(&r->lock);
    g_cond_init(&r->cond);
    /* A few connections at once: a sender's /Discover connection can stay
     * open beside its /Ask (HTTP-1). */
    r->service = g_threaded_socket_service_new(6);
    gboolean ok = FALSE;
    for (int i = 0; i <= AIRDROP_PORTS && !ok; i++) {
        GSocketAddress *a = g_inet_socket_address_new_from_string(address, i < AIRDROP_PORTS ? AIRDROP_PORT + i : 0);
        if (!a) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Bad address %s", address);
            break;
        }
        GSocketAddress *bound = NULL;
        g_clear_error(error);
        ok = g_socket_listener_add_address(G_SOCKET_LISTENER(r->service), a, G_SOCKET_TYPE_STREAM,
                                           G_SOCKET_PROTOCOL_TCP, NULL, &bound, error);
        if (ok) r->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
        g_clear_object(&bound);
        g_object_unref(a);
    }
    if (!ok) {
        receiver_unref(r);
        return NULL;
    }
    /* The handler holds the receiver until the service is gone, which is
     * only once its last connection's thread has finished. */
    g_signal_connect_data(r->service, "run", G_CALLBACK(on_run), receiver_ref(r), closure_unref, 0);
    g_socket_service_start(r->service);
    return r;
}

guint16 airdrop_receiver_port(AirdropReceiver *r) { return r->port; }

void airdrop_receiver_answer(AirdropReceiver *r, guint id, gboolean accept)
{
    g_mutex_lock(&r->lock);
    if (g_hash_table_contains(r->answers, GUINT_TO_POINTER(id)))
        g_hash_table_insert(r->answers, GUINT_TO_POINTER(id), GINT_TO_POINTER(accept ? 1 : 2));
    g_cond_broadcast(&r->cond);
    g_mutex_unlock(&r->lock);
}

void airdrop_receiver_set_policy(AirdropReceiver *r, gboolean contacts_only, const char *const *contact_hashes)
{
    g_mutex_lock(&r->lock);
    r->contacts_only = !!contacts_only;
    g_hash_table_remove_all(r->contact_hashes);
    if (contact_hashes) {
        for (const char *const *h = contact_hashes; *h; h++) {
            if (strlen(*h) == 64) g_hash_table_add(r->contact_hashes, g_ascii_strdown(*h, 64));
        }
    }
    g_mutex_unlock(&r->lock);
}

void airdrop_receiver_free(AirdropReceiver *r)
{
    if (!r) return;
    r->closed = TRUE;
    g_cancellable_cancel(r->cancel);
    g_socket_service_stop(r->service);
    g_socket_listener_close(G_SOCKET_LISTENER(r->service));
    g_mutex_lock(&r->lock);
    g_cond_broadcast(&r->cond);
    g_mutex_unlock(&r->lock);
    /* Dropping the service here breaks the cycle through its handler. */
    GSocketService *service = r->service;
    r->service = NULL;
    g_object_unref(service);
    receiver_unref(r);
}

/* ---------------------------------------------------------------- sender */

void airdrop_peer_free(AirdropPeer *p)
{
    if (!p) return;
    g_free(p->name);
    g_free(p->model);
    g_free(p);
}

typedef struct {
    GSocketConnection *conn;
    GIOStream *tls;
    GInputStream *in;
    GOutputStream *out;
} Link;

static void link_close(Link *l)
{
    if (l->tls) g_io_stream_close(l->tls, NULL, NULL);
    g_clear_object(&l->tls);
    g_clear_object(&l->conn);
}

static gboolean link_open(Link *l, const char *address, guint16 port, guint connect_s, guint io_s,
                          GCancellable *cancel, GError **error)
{
    GSocketAddress *a = g_inet_socket_address_new_from_string(address, port);
    if (!a) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Bad address %s", address);
        return FALSE;
    }
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, connect_s);
    /* A link-local peer is never behind a proxy, and looking one up loads
     * the desktop's proxy libraries, several megabytes. */
    g_socket_client_set_enable_proxy(client, FALSE);
    l->conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(a), cancel, error);
    g_object_unref(client);
    g_object_unref(a);
    if (!l->conn) return FALSE;
    set_timeout(l->conn, connect_s);
    l->tls = tls_client(G_IO_STREAM(l->conn), error);
    if (!l->tls) return FALSE;
    if (!g_tls_connection_handshake(G_TLS_CONNECTION(l->tls), cancel, error)) return FALSE;
    set_timeout(l->conn, io_s);
    l->in = g_io_stream_get_input_stream(l->tls);
    l->out = g_io_stream_get_output_stream(l->tls);
    return TRUE;
}

/* A plist request with a Content-Length body (HTTP-2, HTTP-2a). */
static GBytes *plist_request(const char *path, GVariant *value)
{
    GBytes *body = encode(value);
    if (!body) return NULL;
    gsize len;
    const void *data = g_bytes_get_data(body, &len);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "POST %s HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nContent-Type: application/octet-stream\r\n"
                              "Connection: keep-alive\r\nContent-Length: %" G_GSIZE_FORMAT "\r\n\r\n", path, len);
    g_string_append_len(s, data, len);
    g_bytes_unref(body);
    return g_string_free_to_bytes(s);
}

typedef struct {
    guint status;
    GByteArray *body;
    gboolean done, too_big;
    gboolean interim, interim_ended; /* a 1xx answer: the real one follows */
} Response;

static gboolean resp_head(const char *method, const char *path, guint status, GHashTable *headers,
                          gpointer user, GError **error)
{
    Response *r = user;
    r->interim = status < 200;
    if (!r->interim) r->status = status;
    return TRUE;
}

static gboolean resp_body(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Response *r = user;
    if (r->body->len + len > MAX_MESSAGE) r->too_big = TRUE;
    else g_byte_array_append(r->body, buf, len);
    return TRUE;
}

static gboolean resp_done(gpointer user, GError **error)
{
    Response *r = user;
    if (r->interim) r->interim_ended = TRUE;
    else r->done = TRUE;
    r->interim = FALSE;
    return TRUE;
}

static const HttpCallbacks CLIENT_HTTP = { resp_head, resp_body, resp_done };

/* Reads one response, skipping any 100 Continue before it. */
static gboolean read_response(Link *l, Response *resp, GCancellable *cancel, GError **error)
{
    resp->body = g_byte_array_new();
    HttpParser *p = http_parser_new(HTTP_RESPONSE, &CLIENT_HTTP, resp);
    guint8 *buf = g_malloc(READ_CHUNK);
    gboolean ok = FALSE;
    while (!resp->done) {
        gssize n = g_input_stream_read(l->in, buf, READ_CHUNK, cancel, error);
        if (n < 0) break;
        if (n == 0) {
            if (!http_parser_eof(p, error)) break;
            if (!resp->done) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "The receiver closed the connection.");
                break;
            }
            continue;
        }
        gsize off = 0;
        while (off < (gsize)n && !resp->done) {
            gsize used = 0;
            if (!http_parser_feed(p, buf + off, n - off, &used, error)) goto out;
            off += used;
            if (resp->interim_ended) { /* a 100 Continue ended: the next message follows */
                resp->interim_ended = FALSE;
                http_parser_reset(p);
            } else if (!resp->done) {
                break; /* the message goes on in the next read */
            }
        }
    }
    ok = resp->done && !resp->too_big;
    if (resp->done && resp->too_big)
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_LIMIT, "The receiver's answer is too long.");
out:
    g_free(buf);
    http_parser_free(p);
    return ok;
}

typedef struct {
    char *address;
    guint16 port;
} DiscoverJob;

static void discover_job_free(gpointer data)
{
    DiscoverJob *j = data;
    g_free(j->address);
    g_free(j);
}

static void discover_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    DiscoverJob *j = data;
    Link l = { 0 };
    GError *e = NULL;
    Response resp = { 0 };
    AirdropPeer *peer = NULL;
    /* An empty dict, as every sender known to work with iOS 18 sends (§6.3). */
    GBytes *req = plist_request("/Discover", v_empty_dict());
    if (!link_open(&l, j->address, j->port, DISCOVER_CONNECT_TIMEOUT, DISCOVER_TIMEOUT, cancel, &e)
        || !write_bytes(l.out, req, cancel, &e)
        || !read_response(&l, &resp, cancel, &e))
        goto out;
    if (resp.status == 200) {
        GVariant *v = bplist_decode(resp.body->data, resp.body->len, NULL);
        char *name = v ? lookup_str(v, "ReceiverComputerName") : NULL;
        if (name) { /* DISC-1 */
            peer = g_new0(AirdropPeer, 1);
            peer->name = name;
            peer->model = lookup_str(v, "ReceiverModelName");
        }
        if (v) g_variant_unref(v);
    }
out:
    g_bytes_unref(req);
    if (resp.body) g_byte_array_unref(resp.body);
    link_close(&l);
    if (e) g_task_return_error(task, e);
    else g_task_return_pointer(task, peer, (GDestroyNotify)airdrop_peer_free);
}

void airdrop_discover_async(const char *address, guint16 port, GCancellable *cancel,
                            GAsyncReadyCallback done, gpointer user)
{
    DiscoverJob *j = g_new0(DiscoverJob, 1);
    j->address = g_strdup(address);
    j->port = port;
    GTask *task = g_task_new(NULL, cancel, done, user);
    g_task_set_task_data(task, j, discover_job_free);
    g_task_run_in_thread(task, discover_thread);
    g_object_unref(task);
}

AirdropPeer *airdrop_discover_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* ---- send ---- */

typedef struct {
    AirdropSendRequest req;
    AirdropSendProgress progress;
    gpointer progress_user;
    GMainContext *ctx;
} SendJob;

static void send_job_free(gpointer data)
{
    SendJob *j = data;
    g_free(j->req.address);
    g_free(j->req.name);
    g_free(j->req.id);
    g_strfreev(j->req.paths);
    g_main_context_unref(j->ctx);
    g_free(j);
}

typedef struct {
    GTask *task;            /* holds the job until the note has run */
    gboolean accepted;
    guint64 sent, total;
} SendNote;

static gboolean send_note_deliver(gpointer data)
{
    SendNote *n = data;
    SendJob *j = g_task_get_task_data(n->task);
    if (j->progress && !g_task_get_completed(n->task))
        j->progress(n->accepted, n->sent, n->total, j->progress_user);
    return G_SOURCE_REMOVE;
}

static void send_note_free(gpointer data)
{
    SendNote *n = data;
    g_object_unref(n->task);
    g_free(n);
}

static void send_note(GTask *task, SendJob *j, gboolean accepted, guint64 sent, guint64 total)
{
    SendNote *n = g_new0(SendNote, 1);
    n->task = g_object_ref(task);
    n->accepted = accepted;
    n->sent = sent;
    n->total = total;
    g_main_context_invoke_full(j->ctx, G_PRIORITY_DEFAULT, send_note_deliver, n, send_note_free);
}

/* The archive, written into a compressor that writes into the spool file. */
typedef struct {
    int fd;
    CpioWriter *cpio;
    AirdropDeflater *z;
    guint32 ino;
} Spool;

static gboolean to_file(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    Spool *s = user;
    while (len) {
        ssize_t n = write(s->fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
            return FALSE;
        }
        buf += n;
        len -= n;
    }
    return TRUE;
}

static gboolean to_deflater(const guint8 *buf, gsize len, gpointer user, GError **error)
{
    return airdrop_deflater_feed(((Spool *)user)->z, buf, len, error);
}

static gboolean spool_file(Spool *s, const char *path, const char *entry, guint64 *size, GCancellable *cancel,
                           GError **error)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s: %s", path, g_strerror(errno));
        if (fd >= 0) close(fd);
        return FALSE;
    }
    gboolean ok = cpio_writer_file(s->cpio, entry, st.st_size, st.st_mtime, error);
    guint8 *buf = g_malloc(READ_CHUNK);
    guint64 left = st.st_size;
    while (ok && left) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) { ok = FALSE; break; }
        ssize_t n = read(fd, buf, MIN(left, READ_CHUNK));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s changed while it was read.", path);
            ok = FALSE;
            break;
        }
        ok = cpio_writer_file_data(s->cpio, buf, n, error);
        left -= n;
    }
    g_free(buf);
    close(fd);
    *size += st.st_size;
    return ok;
}

/* A folder and what it holds: folders and regular files only. */
static gboolean spool_tree(Spool *s, const char *path, const char *entry, guint64 *size, GCancellable *cancel,
                           GError **error)
{
    struct stat st;
    if (lstat(path, &st) < 0) return TRUE;
    if (!cpio_writer_dir(s->cpio, entry, error)) return FALSE;
    GDir *d = g_dir_open(path, 0, error);
    if (!d) return FALSE;
    gboolean ok = TRUE;
    const char *name;
    while (ok && (name = g_dir_read_name(d))) {
        if (!airdrop_safe_name(name) || g_str_has_prefix(name, "._")) continue;
        char *child = g_build_filename(path, name, NULL), *child_entry = g_strconcat(entry, "/", name, NULL);
        if (lstat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) ok = spool_tree(s, child, child_entry, size, cancel, error);
            else if (S_ISREG(st.st_mode)) ok = spool_file(s, child, child_entry, size, cancel, error);
        }
        g_free(child);
        g_free(child_entry);
    }
    g_dir_close(d);
    return ok;
}

static char *uuid_upper(void)
{
    char *u = g_uuid_string_random();
    char *up = g_ascii_strup(u, -1);
    g_free(u);
    return up;
}

/* DVZip unless the receiver's flags are known and leave it out (UP-1b). */
static gboolean use_dvzip(guint flags)
{
    gboolean known = (flags & (0x004 | 0x008)) != 0;
    return !known || (flags & 0x002);
}

static void send_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    SendJob *j = data;
    GError *e = NULL;
    Link l = { 0 };
    Spool s = { .fd = -1 };
    char *spool_path = NULL, *tid = uuid_upper();
    GBytes *ask = NULL;
    Response resp = { 0 };
    gboolean dvzip = use_dvzip(j->req.flags);

    /* The offer and its archive (UP-2a: the root entry first). */
    s.fd = g_file_open_tmp("lares-airdrop-XXXXXX", &spool_path, &e);
    if (s.fd < 0) goto out;
    g_unlink(spool_path); /* gone when closed */
    s.z = airdrop_deflater_new(dvzip, to_file, &s);
    s.cpio = cpio_writer_new(to_deflater, &s);
    if (!cpio_writer_dir(s.cpio, ".", &e)) goto out;
    GVariantBuilder files;
    g_variant_builder_init(&files, G_VARIANT_TYPE("av"));
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean ok = TRUE;
    for (char **p = j->req.paths; ok && *p; p++) {
        char *name = g_path_get_basename(*p);
        struct stat st;
        if (!g_utf8_validate(name, -1, NULL) || !airdrop_safe_name(name) || g_hash_table_contains(names, name)) {
            g_set_error(&e, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Can't send %s under its name.", name);
            ok = FALSE;
        } else if (stat(*p, &st) < 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
            g_set_error(&e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s isn't a file or folder.", name);
            ok = FALSE;
        } else {
            char *entry = g_strconcat("./", name, NULL);
            guint64 size = 0;
            gboolean dir = S_ISDIR(st.st_mode);
            ok = dir ? spool_tree(&s, *p, entry, &size, cancel, &e) : spool_file(&s, *p, entry, &size, cancel, &e);
            GVariantBuilder f;
            g_variant_builder_init(&f, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&f, "{sv}", "FileBomPath", v_str(entry));
            g_variant_builder_add(&f, "{sv}", "FileIsDirectory", g_variant_new_boolean(dir));
            g_variant_builder_add(&f, "{sv}", "FileName", v_str(name));
            g_variant_builder_add(&f, "{sv}", "FileSize", g_variant_new_int64(size));
            g_variant_builder_add(&f, "{sv}", "FileType", v_str(airdrop_uti(name, dir)));
            g_variant_builder_add(&f, "{sv}", "ShouldConvertMediaFormats", g_variant_new_boolean(FALSE));
            g_variant_builder_add(&files, "v", g_variant_builder_end(&f));
            g_hash_table_add(names, g_strdup(name));
            g_free(entry);
        }
        g_free(name);
    }
    g_hash_table_unref(names);
    if (!ok || !cpio_writer_finish(s.cpio, &e) || !airdrop_deflater_finish(s.z, &e)) {
        g_variant_builder_clear(&files);
        goto out;
    }
    guint64 total = lseek(s.fd, 0, SEEK_CUR);
    lseek(s.fd, 0, SEEK_SET);

    /* ASK-7: the current form, keys in the order Finder's come. */
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "BundleID", v_str("com.apple.finder"));
    g_variant_builder_add(&b, "{sv}", "ConvertMediaFormats", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&b, "{sv}", "Files", g_variant_builder_end(&files));
    g_variant_builder_add(&b, "{sv}", "Items", g_variant_new("av", NULL));
    g_variant_builder_add(&b, "{sv}", "SenderComputerName", v_str(j->req.name));
    g_variant_builder_add(&b, "{sv}", "SenderID", v_str(j->req.id));
    g_variant_builder_add(&b, "{sv}", "SenderModelName", v_str(MODEL));
    GVariantBuilder idb;
    g_variant_builder_init(&idb, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&idb, "{sv}", "id", v_str(tid));
    g_variant_builder_add(&b, "{sv}", "TransferID", g_variant_builder_end(&idb));
    GVariantBuilder tt;
    g_variant_builder_init(&tt, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&tt, "{sv}", "files", v_empty_dict());
    g_variant_builder_add(&b, "{sv}", "TransferType", g_variant_builder_end(&tt));
    ask = plist_request("/Ask", g_variant_builder_end(&b));
    if (!ask) {
        g_set_error_literal(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't describe the files.");
        goto out;
    }

    if (!link_open(&l, j->req.address, j->req.port, CONNECT_TIMEOUT, IO_TIMEOUT, cancel, &e)
        || !write_bytes(l.out, ask, cancel, &e))
        goto out;
    set_timeout(l.conn, SENDER_ASK_TIMEOUT);
    if (!read_response(&l, &resp, cancel, &e) || resp.status != 200) {
        g_clear_error(&e);
        g_set_error_literal(&e, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Declined, or not answered."); /* ASK-1 */
        goto out;
    }
    send_note(task, j, TRUE, 0, total);
    set_timeout(l.conn, IO_TIMEOUT);

    /* UP-1a, UP-9, UP-10. */
    char *head = g_strdup_printf("POST /Upload HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nTransferID: %s\r\nTotalBytes: 0\r\n"
                                 "Content-Type: %s\r\nConnection: keep-alive\r\nContent-Length: %" G_GUINT64_FORMAT "\r\n\r\n",
                                 tid, dvzip ? "application/x-dvzip" : "application/x-cpio", total);
    gboolean wrote = g_output_stream_write_all(l.out, head, strlen(head), NULL, cancel, &e);
    g_free(head);
    guint8 *buf = g_malloc(READ_CHUNK);
    guint64 sent = 0;
    gint64 last = 0;
    while (wrote && sent < total) {
        ssize_t n = read(s.fd, buf, READ_CHUNK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            g_set_error_literal(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "The spool file ended early.");
            wrote = FALSE;
            break;
        }
        wrote = g_output_stream_write_all(l.out, buf, n, NULL, cancel, &e);
        sent += n;
        gint64 now = g_get_monotonic_time();
        if (wrote && (now - last >= PROGRESS_EVERY || sent == total)) {
            last = now;
            send_note(task, j, TRUE, sent, total);
        }
    }
    g_free(buf);
    if (wrote) wrote = g_output_stream_flush(l.out, cancel, &e);
    /* The receiver may have refused and closed; its response, if any, says
     * why. */
    g_clear_error(&e);
    if (resp.body) g_byte_array_unref(resp.body);
    memset(&resp, 0, sizeof resp);
    set_timeout(l.conn, UPLOAD_RESPONSE_TIMEOUT);
    if (!read_response(&l, &resp, cancel, &e)) goto out;
    if (resp.status != 200)
        g_set_error(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "The receiver refused the files (%u).", resp.status);
out:
    if (s.cpio) cpio_writer_free(s.cpio);
    if (s.z) airdrop_deflater_free(s.z);
    if (s.fd >= 0) close(s.fd);
    g_free(spool_path);
    g_free(tid);
    if (ask) g_bytes_unref(ask);
    if (resp.body) g_byte_array_unref(resp.body);
    link_close(&l);
    if (e) g_task_return_error(task, e);
    else g_task_return_boolean(task, TRUE);
}

void airdrop_send_async(const AirdropSendRequest *request, GCancellable *cancel, AirdropSendProgress progress,
                        gpointer progress_user, GAsyncReadyCallback done, gpointer user)
{
    SendJob *j = g_new0(SendJob, 1);
    j->req.address = g_strdup(request->address);
    j->req.port = request->port;
    j->req.flags = request->flags;
    j->req.name = g_strdup(request->name);
    j->req.id = g_strdup(request->id);
    j->req.paths = g_strdupv(request->paths);
    j->progress = progress;
    j->progress_user = progress_user;
    j->ctx = g_main_context_ref_thread_default();
    GTask *task = g_task_new(NULL, cancel, done, user);
    g_task_set_task_data(task, j, send_job_free);
    g_task_run_in_thread(task, send_thread);
    g_object_unref(task);
}

gboolean airdrop_send_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean(G_TASK(res), error);
}
