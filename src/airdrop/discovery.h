// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>

/* How AirDrop devices find each other: the Bluetooth LE advertisement that
 * wakes receivers (SPEC §2), and mDNS / DNS-SD for `_airdrop._tcp` on the
 * AWDL interface (§5). Both run in GLib's main loop and touch nothing until
 * asked to. Rule IDs are those of owly's AirDrop specification. */

/* ---- Bluetooth LE wake-up, BLE-1 to BLE-5 ---- */

/* The 18-byte AirDrop value (type 0x05) of version 3 with the given tag
 * and no contact hashes (BLE-3): what an Everyone-mode receiver wakes for. */
void ble_airdrop_payload(const guint8 tag[3], guint8 out[18]);

typedef struct AirdropBle AirdropBle;
/* Through BlueZ on the system bus (LEAdvertisingManager1): a connectable
 * advertisement at 100 ms with Apple's company ID 0x004C, carrying the
 * payload above under a fresh random tag each time it starts. */
AirdropBle *airdrop_ble_new(void);
void airdrop_ble_set_active(AirdropBle *b, gboolean active);
gboolean airdrop_ble_is_active(AirdropBle *b);
void airdrop_ble_free(AirdropBle *b);

/* ---- mDNS / DNS-SD on the AWDL interface ---- */

typedef struct {
    char *instance;     /* the service ID, 12 hex digits */
    char *host;         /* SRV target, "name.local." */
    char *address;      /* the AAAA address with its scope, "fe80::…%awdl0" */
    guint16 port;
    guint flags;        /* TXT flags (MDNS-4), 0 when there's no TXT yet */
} AirdropService;

typedef struct {
    /* A service resolved (SRV and AAAA, MDNS-6), or changed. */
    void (*found)(const AirdropService *service, gpointer user);
    /* Withdrawn, or not refreshed in time (SEC-11). */
    void (*lost)(const char *instance, gpointer user);
} MdnsCallbacks;

typedef struct AirdropMdns AirdropMdns;
/* Listens on `ifname` only, IPv6 only, on UDP 5353 shared with any other
 * responder on the machine. */
AirdropMdns *airdrop_mdns_new(const char *ifname, const MdnsCallbacks *cb, gpointer user, GError **error);
/* Announce this receiver (MDNS-1): repeated in bursts while published
 * (MDNS-7), and withdrawn with a goodbye when `instance` is NULL. */
void airdrop_mdns_publish(AirdropMdns *m, const char *instance, const char *hostname,
                          const char *address, guint16 port, guint flags);
/* Ask for `_airdrop._tcp` services, repeatedly while on. */
void airdrop_mdns_browse(AirdropMdns *m, gboolean on);
void airdrop_mdns_free(AirdropMdns *m);

/* The DNS message codec under it, on plain bytes, for the tests. */
GBytes *mdns_build_announcement(const char *instance, const char *hostname, const char *address,
                                guint16 port, guint flags, guint32 ttl);
GBytes *mdns_build_query(void);
/* Calls `found`/`lost` for each `_airdrop._tcp` service a message
 * completes; a cache across messages lives in AirdropMdns. Returns FALSE
 * for a malformed message. `ifname` scopes the addresses it reports. */
typedef struct MdnsCache MdnsCache;
MdnsCache *mdns_cache_new(void);
gboolean mdns_cache_feed(MdnsCache *c, const guint8 *msg, gsize len, const char *ifname, gint64 now_us,
                         const MdnsCallbacks *cb, gpointer user);
void mdns_cache_expire(MdnsCache *c, gint64 now_us, const MdnsCallbacks *cb, gpointer user);
void mdns_cache_free(MdnsCache *c);
