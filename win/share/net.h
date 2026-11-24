// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* IPv6 for AirDrop on Windows (issue #109): lwIP, inside lares-share.exe, on
 * an interface of its own that the AWDL link carries. Nothing of it is
 * Windows' network: no adapter, no firewall rule, no Windows mDNS. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "awdl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct net_iface;

/* Starts lwIP's thread, once per process. */
void net_init(void);

/* Where an interface's packets go: the AWDL link (link_send), or another
 * interface in the tests. Called on lwIP's thread. */
typedef void (*net_output)(void *ctx, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len);

/* An interface on `mac`, its EUI-64 link-local address (NET-1), MTU 1450,
 * up. NULL if lwIP has no room. */
struct net_iface *net_iface_add(const awdl_mac *mac, net_output out, void *ctx);
void net_iface_remove(struct net_iface *i);
/* A packet the link heard for this interface; any thread. */
void net_iface_input(struct net_iface *i, const uint8_t *ip, size_t len);
/* The interface's index, the zone of its link-local addresses. */
unsigned net_iface_index(const struct net_iface *i);
/* Its link-local address, as text ("fe80::…") without a zone. */
void net_iface_address(const struct net_iface *i, char out[64]);

#ifdef __cplusplus
}
#endif
