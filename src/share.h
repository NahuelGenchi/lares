// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>

/* Sharing with iPhones, iPads and Macs over AirDrop (docs/AIRDROP.md). The
 * Wi-Fi card is lent to `lares-awdl` only while receiving is on or a send
 * is waiting, and given back the moment neither is. Everything above the
 * link (Bluetooth LE, mDNS, TLS and the files) runs here, in src/airdrop/. */

#define SHARE_AWDL_PATH "/usr/local/libexec/lares-awdl"
#define SHARE_NM_CONF   "/etc/NetworkManager/conf.d/90-lares-airdrop.conf"
#define SHARE_LINK_FILE "/etc/systemd/network/10-lares-awdl.link"
#define SHARE_RECEIVE_SECONDS 600   /* as an iPhone's Everyone for 10 minutes */

typedef enum {
    SHARE_NO_CARD,        /* no Wi-Fi card */
    SHARE_NEEDS_ALLOW,    /* lares-awdl isn't installed with its permission, or is older */
    SHARE_READY,
} ShareSetup;

typedef enum {
    LINK_OFF,
    LINK_STARTING,
    LINK_UP,
    LINK_STOPPING,
    LINK_FAILED,
} ShareLink;

typedef enum {
    SHARE_DISCOVER_OFF,
    SHARE_DISCOVER_CONTACTS,
    SHARE_DISCOVER_EVERYONE,
} ShareDiscoverMode;

typedef enum {
    DEVICE_KIND_PHONE,
    DEVICE_KIND_TABLET,
    DEVICE_KIND_LAPTOP,
    DEVICE_KIND_DESKTOP,
    DEVICE_KIND_WATCH,
    DEVICE_KIND_OTHER,
} ShareDeviceKind;

typedef struct {
    char *name;
    char *device;
    char *email;
    char *phone;
    char *photo;
} ShareContact;

void share_contact_free(ShareContact *c);

typedef enum {
    DEVICE_FOUND,         /* answered /Discover: can be sent to */
    DEVICE_ASKING,        /* waiting for its user to accept */
    DEVICE_SENDING,
    DEVICE_SENT,
    DEVICE_DECLINED,
    DEVICE_FAILED,
} DeviceState;

typedef struct {
    char *key;            /* the mDNS instance, or the AWDL address for DISC-5 */
    char *name, *model;
    char *address;        /* "fe80::…%awdl0" */
    guint16 port;
    guint flags;
    DeviceState state;
    guint64 sent, total;
    char *message;        /* why a send failed */
} ShareDevice;

typedef enum {
    INCOMING_ASKING,
    INCOMING_RECEIVING,
    INCOMING_DONE,
    INCOMING_FAILED,
} IncomingState;

typedef struct {
    guint id;
    IncomingState state;
    char *sender;
    char *sender_model;
    char *matched_hash;
    gboolean links;
    GPtrArray *names;     /* file names, or URLs */
    GPtrArray *saved;     /* saved paths, once done */
    GBytes *file_icon;    /* FileIcon preview bytes from /Ask */
    guint64 done, total;
    char *message;
} ShareIncoming;

typedef struct {
    ShareSetup setup;
    char *card;           /* the Wi-Fi interface it would use */
    char *card_text;      /* "Realtek USB adapter, 2.4 GHz" */
    gboolean card_5ghz;
    gboolean card_in_use; /* it carries this PC's own Wi-Fi connection */
    gboolean allow_busy;
    char *allow_error;

    ShareLink link;
    char *link_error;
    gboolean quiet;       /* the link has been up a while and has heard no Apple device */
    char *name;           /* how others see this PC */

    ShareDiscoverMode mode;
    gboolean receiving;   /* mode != SHARE_DISCOVER_OFF */
    gint64 receive_until; /* monotonic µs */
    gint64 xfer_start_us; /* monotonic µs when current transfer started */
    ShareIncoming *incoming;

    gboolean browsing;    /* continuously finding nearby devices while Share is visible */
    gboolean sending;     /* looking for devices to send `paths` to */
    GStrv paths;
    GPtrArray *devices;   /* ShareDevice*, found while browsing or sending */
    GPtrArray *contacts;  /* ShareContact*, saved locally for Contacts Only & photos */
} ShareState;

typedef struct Share Share;
typedef void (*ShareFn)(gpointer user);

Share *share_new(void);
const ShareState *share_state(Share *s);
/* `fn` runs whenever anything in the state changed. */
void share_watch(Share *s, ShareFn fn, gpointer user);
/* Looks again for the card and the installed lares-awdl. */
void share_refresh(Share *s);

/* Installs lares-awdl with its two capabilities; asks for the password. */
void share_allow(Share *s);

void share_set_discover_mode(Share *s, ShareDiscoverMode mode);
void share_set_receiving(Share *s, gboolean on);
void share_set_browsing(Share *s, gboolean on);
void share_answer(Share *s, gboolean accept);
void share_dismiss_incoming(Share *s);

/* Starts looking for devices while files are being picked, so the link
 * and the phones' wake-up don't wait for the choice; FALSE when the pick is
 * cancelled. */
void share_set_looking(Share *s, gboolean on);
/* Starts looking for devices to send `paths` to, keeping any found while
 * they were picked; NULL stops. */
void share_set_sending(Share *s, const char *const *paths);
void share_send_to(Share *s, const char *key);
void share_send_files_to(Share *s, const char *key, const char *const *paths);
void share_dismiss_device(Share *s, const char *key);
void share_cancel_send(Share *s);
/* Saves contacts & profile photos and refreshes Contacts Only hashes live. */
void share_save_contacts(Share *s, GPtrArray *contacts);
void share_set_device_photo(Share *s, const char *device_name, const char *photo_path);
/* Ends the link at once rather than after its minute of waiting for a next use. */
void share_stop_link(Share *s);

/* Friendly device subtitle, icon kind, monogram initials, and contact matching. */
ShareDeviceKind share_device_kind(const char *model, const char *name);
const char *share_model_label(const char *model, const char *name);
char *share_initials(const char *name);
char *share_hash_email(const char *email);
GPtrArray *share_hash_phone_variants(const char *phone);
const ShareContact *share_find_contact(const ShareState *st, const char *name, const char *matched_hash);

/* Whether this PC has a Wi-Fi card at all, for the sidebar: a cheap check. */
gboolean share_has_card(void);

/* Exposed for the tests: a flat JSON object's string or number values, as
 * lares-awdl writes its events, NULL if it isn't one. */
GHashTable *share_parse_event(const char *line);
/* A peer's IPv6 link-local address from its MAC (modified EUI-64, NET-1),
 * scoped to `ifname`. */
char *share_link_local(const char *mac, const char *ifname);
