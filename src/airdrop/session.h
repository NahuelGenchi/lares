// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>

/* AirDrop's exchange over TLS (SPEC §6): the receiver's server, and the
 * sender's /Discover, /Ask and /Upload. Each connection runs in a worker
 * thread with blocking I/O and timeouts, so a slow or hostile peer never
 * holds the window; what the window needs to know comes back on the main
 * context the object was made on. */

#define AIRDROP_PORT 8770           /* MDNS-1's conventional port, and DISC-5's */
#define AIRDROP_PORTS 10            /* 8770 to 8779: what Share's firewall rule opens */
#define AIRDROP_RECEIVER_FLAGS 0x089 /* URL sharing, mixed types, Discover (MDNS-5) */

/* Our self-signed certificate (TLS-2), made once with the openssl command
 * and kept, its key readable by us only, in ~/.local/share/lares/airdrop.
 * Blocks while it's made: call it from a worker thread. */
GTlsCertificate *airdrop_identity(GError **error);

/* A Uniform Type Identifier for a file name, `public.data` when unknown. */
const char *airdrop_uti(const char *name, gboolean is_directory);

/* ---- Receiver ---- */

typedef struct {
    guint id;                /* for airdrop_receiver_answer */
    char *sender;            /* the sender's name: untrusted, escape it for display (SEC-4) */
    char *sender_model;      /* SenderModelName, e.g. "iPhone17,1" */
    char *matched_hash;      /* matched contact SHA-256 hash when SenderRecordData matched */
    gboolean links;          /* a link share: `names` holds URLs, never opened unasked */
    GPtrArray *names;        /* the file names, or the URLs */
    GBytes *file_icon;       /* FileIcon preview bytes from /Ask, bounded */
    guint64 total;           /* the files' sizes as the sender gives them, 0 when unknown */
} AirdropOffer;

typedef struct {
    void (*offer)(const AirdropOffer *offer, gpointer user);
    /* The sender gave up, or the prompt timed out: take the prompt down. */
    void (*withdrawn)(guint id, gpointer user);
    void (*progress)(guint id, guint64 done, guint64 total, gpointer user);
    /* Saved files' paths, or an accepted link share's URLs. */
    void (*received)(guint id, gboolean links, GPtrArray *items, gpointer user);
    void (*failed)(guint id, const char *message, gpointer user);
    /* How an upload went, for a log: formats, sizes and counts, never a
     * name. Optional. */
    void (*trace)(guint id, const char *line, gpointer user);
} AirdropReceiverCallbacks;

typedef struct AirdropReceiver AirdropReceiver;
/* Listens on `address` ("fe80::…%awdl0") only, on AIRDROP_PORT or the next
 * free one of AIRDROP_PORTS, else any: airdrop_receiver_port says which. Files go to `folder`,
 * through a staging folder inside it, never over an existing file. */
AirdropReceiver *airdrop_receiver_new(const char *address, const char *name, const char *folder,
                                      GTlsCertificate *cert, const AirdropReceiverCallbacks *cb,
                                      gpointer user, GError **error);
guint16 airdrop_receiver_port(AirdropReceiver *r);
void airdrop_receiver_answer(AirdropReceiver *r, guint id, gboolean accept);
/* Updates Contacts Only filtering and allowed contact SHA-256 hex hashes (ID-3). */
void airdrop_receiver_set_policy(AirdropReceiver *r, gboolean contacts_only, const char *const *contact_hashes);
/* Stops listening and cuts open connections; no callback runs after it. */
void airdrop_receiver_free(AirdropReceiver *r);

/* ---- Sender ---- */

typedef struct {
    char *name, *model;
} AirdropPeer;

void airdrop_peer_free(AirdropPeer *p);

/* POST /Discover on a connection of its own (§6.3). Finishes with the
 * receiver's name and model when it lists us (DISC-1), and NULL with no
 * error when it doesn't. */
void airdrop_discover_async(const char *address, guint16 port, GCancellable *cancel,
                            GAsyncReadyCallback done, gpointer user);
AirdropPeer *airdrop_discover_finish(GAsyncResult *res, GError **error);

/* Upload progress on the main context: `accepted` once the receiver's user
 * said yes, then the compressed bytes sent of `total`. */
typedef void (*AirdropSendProgress)(gboolean accepted, guint64 sent, guint64 total, gpointer user);

typedef struct {
    char *address;           /* "fe80::…%awdl0" */
    guint16 port;
    guint flags;             /* the receiver's TXT flags, 0 when unknown (UP-1b) */
    char *name;              /* our SenderComputerName */
    char *id;                /* our service ID, as SenderID (MDNS-3) */
    GStrv paths;             /* files and folders to send */
} AirdropSendRequest;

/* /Ask and, once accepted, /Upload on one fresh connection (HTTP-1). The
 * archive is compressed into a temporary file first, so the upload carries
 * a Content-Length. Fails with G_IO_ERROR_PERMISSION_DENIED when the
 * receiver declines or doesn't answer in time. */
void airdrop_send_async(const AirdropSendRequest *request, GCancellable *cancel, AirdropSendProgress progress,
                        gpointer progress_user, GAsyncReadyCallback done, gpointer user);
gboolean airdrop_send_finish(GAsyncResult *res, GError **error);
