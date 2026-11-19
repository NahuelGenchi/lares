// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* CPIO archives, the payload of /Upload (UP-2, UP-2a). An entry is a header,
 * its name with a NUL, then its data; the entry named TRAILER!!! ends the
 * archive. odc (magic 070707, POSIX's portable format) has a 76-byte header
 * of octal fields and pads nothing. newc (070701) has a 110-byte header of
 * 8-digit hex fields and pads the name and the data to 4 bytes.
 *
 * The writer emits odc as Finder lays it out: owner 501:20, one link for a
 * file and two for a directory, inode numbers counting up from 1. The
 * reader takes odc and newc in pieces of any size, holds no more than one
 * header and name, and hands file data straight through. It refuses
 * absolute paths, ".." and anything but directories and regular files:
 * links, hard-linked files, devices, FIFOs and sockets (SEC-1).
 *
 * Ported from owly's CPIO reader and writer (MIT). */
#include "formats.h"
#include <string.h>

#define ODC_HEADER  76
#define NEWC_HEADER 110
#define TRAILER     "TRAILER!!!"

#define MODE_TYPE 0170000
#define MODE_DIR  0040000
#define MODE_FILE 0100000

#define ODC_MAX_SIZE G_GUINT64_CONSTANT(077777777777)   /* 11 octal digits */
#define ODC_MAX_SIX  0777777                            /* 6 octal digits */

/* The owner a Mac's archives carry; receivers ignore it. */
#define UID 501
#define GID 20

static gboolean call_sink(AirdropSink sink, gpointer user, const guint8 *buf, gsize len, GError **error)
{
    GError *local = NULL;
    if (sink(buf, len, user, &local)) return TRUE;
    if (!local) local = g_error_new_literal(AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, "Archive data refused");
    g_propagate_error(error, local);
    return FALSE;
}

/* Whether a path names something outside where the archive is unpacked: an
 * absolute path, or one with a ".." component (SEC-1). */
static gboolean escapes(const char *path)
{
    if (path[0] == '/') return TRUE;
    for (const char *p = path; *p;) {
        const char *slash = strchr(p, '/');
        gsize n = slash ? (gsize)(slash - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return TRUE;
        p += n;
        if (*p == '/') p++;
    }
    return FALSE;
}

/* ------------------------------------------------------------ writing */

struct CpioWriter {
    AirdropSink sink;
    gpointer user;
    guint32 ino;       /* the next entry's */
    guint64 left;      /* bytes the current file still needs */
    gboolean finished;
    gboolean failed;
};

CpioWriter *cpio_writer_new(AirdropSink sink, gpointer user)
{
    CpioWriter *w = g_new0(CpioWriter, 1);
    w->sink = sink;
    w->user = user;
    w->ino = 1;
    return w;
}

void cpio_writer_free(CpioWriter *w)
{
    g_free(w);
}

/* Whether a new entry or the trailer may come now. */
static gboolean ready(CpioWriter *w, GError **error)
{
    if (w->failed || w->finished)
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID,
                            w->failed ? "The archive failed earlier" : "The archive is finished");
    else if (w->left)
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "A file's data is missing");
    else
        return TRUE;
    return FALSE;
}

/* An odc header and the name after it, in one piece. */
static gboolean put_header(CpioWriter *w, const char *name, guint mode, guint uid, guint gid, guint nlink,
                           guint ino, guint64 mtime, guint64 size, GError **error)
{
    gsize namesize = strlen(name) + 1;
    guint8 buf[ODC_HEADER + CPIO_MAX_NAME + 1];
    g_snprintf((char *)buf, sizeof buf,
               "070707%06o%06o%06o%06o%06o%06o%06o%011" G_GINT64_MODIFIER "o%06o%011" G_GINT64_MODIFIER "o",
               0u, ino & ODC_MAX_SIX, mode, uid, gid, nlink, 0u, mtime, (guint)namesize, size);
    memcpy(buf + ODC_HEADER, name, namesize);
    if (call_sink(w->sink, w->user, buf, ODC_HEADER + namesize, error)) return TRUE;
    w->failed = TRUE;
    return FALSE;
}

/* A path the reader on the other side will take. */
static gboolean check_path(const char *path, GError **error)
{
    if (!*path || strcmp(path, TRAILER) == 0 || escapes(path)) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, "A path that can't go in an archive");
        return FALSE;
    }
    if (strlen(path) + 1 > CPIO_MAX_NAME) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_LIMIT, "A path too long to archive");
        return FALSE;
    }
    return TRUE;
}

/* A directory. The call gives no time, so it's dated now. */
gboolean cpio_writer_dir(CpioWriter *w, const char *path, GError **error)
{
    if (!ready(w, error) || !check_path(path, error)) return FALSE;
    guint64 now = MIN((guint64)(g_get_real_time() / G_USEC_PER_SEC), ODC_MAX_SIZE);
    return put_header(w, path, MODE_DIR | 0755, UID, GID, 2, w->ino++, now, 0, error);
}

gboolean cpio_writer_file(CpioWriter *w, const char *path, guint64 size, gint64 mtime, GError **error)
{
    if (!ready(w, error) || !check_path(path, error)) return FALSE;
    if (size > ODC_MAX_SIZE) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_LIMIT, "A file too large to archive");
        return FALSE;
    }
    guint64 when = mtime < 0 ? 0 : MIN((guint64)mtime, ODC_MAX_SIZE);
    if (!put_header(w, path, MODE_FILE | 0644, UID, GID, 1, w->ino++, when, size, error)) return FALSE;
    w->left = size;
    return TRUE;
}

gboolean cpio_writer_file_data(CpioWriter *w, const guint8 *buf, gsize len, GError **error)
{
    if (w->failed || w->finished || len > w->left) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID,
                            len > w->left ? "More data than the file's size" : "The archive can't take data now");
        return FALSE;
    }
    if (len == 0) return TRUE;
    if (!call_sink(w->sink, w->user, buf, len, error)) {
        w->failed = TRUE;
        return FALSE;
    }
    w->left -= len;
    return TRUE;
}

/* The trailer: all zero but its name, as Finder writes it (UP-2a). */
gboolean cpio_writer_finish(CpioWriter *w, GError **error)
{
    if (!ready(w, error) || !put_header(w, TRAILER, 0, 0, 0, 0, 0, 0, 0, error)) return FALSE;
    w->finished = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------ reading */

typedef enum { ODC = 1, NEWC } Format;

typedef enum { HEADER, NAME, DATA, PAD, DONE, FAILED } State;

struct CpioReader {
    CpioCallbacks cb;
    gpointer user;
    State state;
    Format format;     /* the first entry's, which every entry must share */
    guint8 buf[NEWC_HEADER + CPIO_MAX_NAME + 3];   /* a header, its name and padding */
    gsize have;
    gsize want;        /* the header, name and padding together */
    gsize namesize;
    guint64 left;      /* the entry's data still to come */
    guint pad;         /* newc's padding after it */
    guint entries;
};

CpioReader *cpio_reader_new(const CpioCallbacks *cb, gpointer user)
{
    CpioReader *r = g_new0(CpioReader, 1);
    if (cb) r->cb = *cb;
    r->user = user;
    r->state = HEADER;
    return r;
}

void cpio_reader_free(CpioReader *r)
{
    g_free(r);
}

static gsize header_len(Format f)
{
    return f == ODC ? ODC_HEADER : NEWC_HEADER;
}

/* Padding after `n` bytes: newc's to 4, odc's none. */
static guint pad_to_4(Format f, guint64 n)
{
    return f == NEWC ? (guint)(-n & 3) : 0;
}

enum { F_INO, F_MODE, F_UID, F_GID, F_NLINK, F_MTIME, F_NAMESIZE, F_FILESIZE, F_COUNT };

/* Where each field is: odc's are octal, of 6 digits but mtime's and the
 * size's 11; newc's are 8 hex digits in a row after the magic. */
static const struct { guint8 at, len; } ODC_FIELDS[F_COUNT] = {
    [F_INO] = { 12, 6 }, [F_MODE] = { 18, 6 }, [F_UID] = { 24, 6 }, [F_GID] = { 30, 6 },
    [F_NLINK] = { 36, 6 }, [F_MTIME] = { 48, 11 }, [F_NAMESIZE] = { 59, 6 }, [F_FILESIZE] = { 65, 11 },
};
static const guint8 NEWC_FIELDS[F_COUNT] = {
    [F_INO] = 0, [F_MODE] = 1, [F_UID] = 2, [F_GID] = 3,
    [F_NLINK] = 4, [F_MTIME] = 5, [F_NAMESIZE] = 11, [F_FILESIZE] = 6,
};

static guint64 field(const CpioReader *r, int which)
{
    gsize at = r->format == ODC ? ODC_FIELDS[which].at : 6u + 8u * NEWC_FIELDS[which];
    gsize len = r->format == ODC ? ODC_FIELDS[which].len : 8;
    guint64 v = 0;
    for (gsize i = 0; i < len; i++) v = v * (r->format == ODC ? 8 : 16) + g_ascii_xdigit_value(r->buf[at + i]);
    return v;
}

/* Every digit of the header after the magic is one of its base's. */
static gboolean digits_ok(const CpioReader *r)
{
    for (gsize i = 6; i < header_len(r->format); i++) {
        guint8 c = r->buf[i];
        if (r->format == ODC ? (c < '0' || c > '7') : !g_ascii_isxdigit(c)) return FALSE;
    }
    return TRUE;
}

static gboolean fail(CpioReader *r, GError **error, AirdropError code, const char *what)
{
    g_set_error(error, AIRDROP_ERROR, code, "Bad archive: %s", what);
    r->state = FAILED;
    return FALSE;
}

/* A callback's answer; one that refuses without saying why gets a reason. */
static gboolean answer(CpioReader *r, gboolean ok, GError **local, GError **error)
{
    /* `local` is passed by address: the callback runs, as an argument,
     * before this reads what it set. */
    if (ok) {
        g_clear_error(local);
        return TRUE;
    }
    if (!*local) *local = g_error_new_literal(AIRDROP_ERROR, AIRDROP_ERROR_REFUSED, "Archive entry refused");
    g_propagate_error(error, *local);
    r->state = FAILED;
    return FALSE;
}

static gboolean end_entry(CpioReader *r, GError **error)
{
    GError *local = NULL;
    if (r->cb.end && !answer(r, r->cb.end(r->user, &local), &local, error)) return FALSE;
    r->state = r->pad ? PAD : HEADER;
    r->have = 0;
    return TRUE;
}

/* A header and name are in: check the entry and start it. */
static gboolean begin_entry(CpioReader *r, GError **error)
{
    gsize hl = header_len(r->format);
    const char *name = (const char *)r->buf + hl;
    if (name[r->namesize - 1] != '\0' || memchr(name, '\0', r->namesize - 1))
        return fail(r, error, AIRDROP_ERROR_INVALID, "a name isn't NUL-terminated");
    if (strcmp(name, TRAILER) == 0) {
        r->state = DONE;
        return TRUE;
    }
    if (!*name) return fail(r, error, AIRDROP_ERROR_INVALID, "an empty name");
    if (!g_utf8_validate(name, -1, NULL)) return fail(r, error, AIRDROP_ERROR_INVALID, "a name isn't UTF-8");
    guint64 mode = field(r, F_MODE), size = field(r, F_FILESIZE);
    CpioType type;
    if ((mode & MODE_TYPE) == MODE_DIR) {
        if (size) return fail(r, error, AIRDROP_ERROR_INVALID, "a directory with data");
        type = CPIO_DIR;
    } else if ((mode & MODE_TYPE) == MODE_FILE) {
        /* A file with other links is a hard link, whose data may be in
         * another entry. */
        if (field(r, F_NLINK) > 1) return fail(r, error, AIRDROP_ERROR_REFUSED, "a hard-linked file");
        type = CPIO_FILE;
    } else {
        return fail(r, error, AIRDROP_ERROR_REFUSED, "a link, device or other special file");
    }
    if (escapes(name)) return fail(r, error, AIRDROP_ERROR_REFUSED, "a path outside the archive");
    if (++r->entries > CPIO_MAX_ENTRIES) return fail(r, error, AIRDROP_ERROR_LIMIT, "too many entries");

    GError *local = NULL;
    if (r->cb.begin && !answer(r, r->cb.begin(name, type, size, r->user, &local), &local, error)) return FALSE;
    r->left = size;
    r->pad = pad_to_4(r->format, size);
    r->state = DATA;
    return size ? TRUE : end_entry(r, error);
}

/* Moves up to `want` bytes into the header buffer; TRUE once it holds them. */
static gboolean collect(CpioReader *r, const guint8 *buf, gsize len, gsize *pos, gsize want)
{
    gsize n = MIN(len - *pos, want - r->have);
    memcpy(r->buf + r->have, buf + *pos, n);
    r->have += n;
    *pos += n;
    return r->have == want;
}

gboolean cpio_reader_feed(CpioReader *r, const guint8 *buf, gsize len, gboolean *done, GError **error)
{
    gsize pos = 0;
    gboolean ok = TRUE;
    if (r->state == FAILED) {
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The archive failed earlier");
        ok = FALSE;
    }
    while (ok && pos < len && r->state != DONE) {
        switch (r->state) {
        case HEADER: {
            if (r->have < 6) {
                if (!collect(r, buf, len, &pos, 6)) break;
                Format f = memcmp(r->buf, "070707", 6) == 0 ? ODC : memcmp(r->buf, "070701", 6) == 0 ? NEWC : 0;
                if (!f) {
                    ok = fail(r, error, AIRDROP_ERROR_INVALID, "not an odc or newc archive");
                    break;
                }
                if (r->format && f != r->format) {
                    ok = fail(r, error, AIRDROP_ERROR_INVALID, "its format changes midway");
                    break;
                }
                r->format = f;
            }
            if (!collect(r, buf, len, &pos, header_len(r->format))) break;
            if (!digits_ok(r)) {
                ok = fail(r, error, AIRDROP_ERROR_INVALID, "a header field isn't a number");
                break;
            }
            guint64 namesize = field(r, F_NAMESIZE);
            if (namesize == 0) {
                ok = fail(r, error, AIRDROP_ERROR_INVALID, "an entry without a name");
                break;
            }
            if (namesize > CPIO_MAX_NAME) {
                ok = fail(r, error, AIRDROP_ERROR_LIMIT, "a name too long");
                break;
            }
            r->namesize = namesize;
            r->want = header_len(r->format) + namesize + pad_to_4(r->format, header_len(r->format) + namesize);
            r->state = NAME;
            break;
        }
        case NAME:
            if (collect(r, buf, len, &pos, r->want)) ok = begin_entry(r, error);
            break;
        case DATA: {
            gsize n = (gsize)MIN((guint64)(len - pos), r->left);
            GError *local = NULL;
            if (r->cb.data && !(ok = answer(r, r->cb.data(buf + pos, n, r->user, &local), &local, error)))
                break;
            pos += n;
            r->left -= n;
            if (!r->left) ok = end_entry(r, error);
            break;
        }
        case PAD: {
            gsize n = MIN(len - pos, (gsize)r->pad);
            pos += n;
            r->pad -= n;
            if (!r->pad) r->state = HEADER;
            break;
        }
        case DONE:
        case FAILED:
            break;
        }
    }
    if (done) *done = r->state == DONE;
    return ok;
}

gboolean cpio_reader_finish(CpioReader *r, GError **error)
{
    if (r->state == DONE) return TRUE;
    if (r->state == FAILED)
        g_set_error_literal(error, AIRDROP_ERROR, AIRDROP_ERROR_INVALID, "The archive failed earlier");
    else
        fail(r, error, AIRDROP_ERROR_INVALID, "it ends before its trailer");
    return FALSE;
}
