// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lwIP's interface on the AWDL link (issue #109). Peers' link-local
 * addresses are made from their MACs (NET-1), so a packet's link address
 * comes from its IPv6 one and neighbour discovery never has to cross the
 * link first, as the permanent neighbour entries do on Linux (NET-2). */
#include "net.h"

#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"

struct net_iface {
    struct netif netif;
    awdl_mac mac;
    net_output out;
    void *ctx;
};

static sys_sem_t started;

static void init_done(void *arg)
{
    (void)arg;
    sys_sem_signal(&started);
}

void net_init(void)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    sys_sem_new(&started, 0);
    tcpip_init(init_done, NULL);
    sys_arch_sem_wait(&started, 0);
    sys_sem_free(&started);
}

/* What goes out: IPv6 with link addresses, and none of lwIP's own MLD
 * reports, which nobody on AWDL needs (they carry a hop-by-hop header, the
 * only packets of ours that do). */
static err_t output6(struct netif *netif, struct pbuf *p, const ip6_addr_t *dst)
{
    struct net_iface *i = (struct net_iface *)netif->state;
    uint8_t buf[AWDL_PACKET_MAX];
    if (p->tot_len > sizeof buf || p->tot_len < AWDL_IPV6_HEADER_LEN) return ERR_BUF;
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    if (buf[6] == 0) return ERR_OK;
    uint8_t d[16];
    memcpy(d, dst->addr, 16);
    awdl_mac to;
    if (d[0] == 0xff) {
        /* RFC 2464: 33:33 and the group's last four bytes. */
        to.a[0] = 0x33;
        to.a[1] = 0x33;
        memcpy(to.a + 2, d + 12, 4);
    } else if (!awdl_mac_from_link_local(d, &to)) {
        return ERR_RTE;
    }
    i->out(i->ctx, &to, &i->mac, buf, p->tot_len);
    return ERR_OK;
}

static err_t iface_init(struct netif *netif)
{
    struct net_iface *i = (struct net_iface *)netif->state;
    netif->name[0] = 'a';
    netif->name[1] = 'w';
    netif->output_ip6 = output6;
    netif->mtu = AWDL_TAP_MTU;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, i->mac.a, 6);
    netif->flags = NETIF_FLAG_MLD6;
    return ERR_OK;
}

struct net_iface *net_iface_add(const awdl_mac *mac, net_output out, void *ctx)
{
    net_init();
    struct net_iface *i = (struct net_iface *)calloc(1, sizeof *i);
    if (!i) return NULL;
    i->mac = *mac;
    i->out = out;
    i->ctx = ctx;
    LOCK_TCPIP_CORE();
    if (!netif_add_noaddr(&i->netif, i, iface_init, tcpip_input)) {
        UNLOCK_TCPIP_CORE();
        free(i);
        return NULL;
    }
    netif_create_ip6_linklocal_address(&i->netif, 1);
    netif_ip6_addr_set_state(&i->netif, 0, IP6_ADDR_PREFERRED);
    netif_set_up(&i->netif);
    netif_set_link_up(&i->netif);
    UNLOCK_TCPIP_CORE();
    return i;
}

void net_iface_remove(struct net_iface *i)
{
    if (!i) return;
    LOCK_TCPIP_CORE();
    netif_remove(&i->netif);
    UNLOCK_TCPIP_CORE();
    free(i);
}

void net_iface_input(struct net_iface *i, const uint8_t *ip, size_t len)
{
    if (len < AWDL_IPV6_HEADER_LEN || len > 0xffff) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) return;
    pbuf_take(p, ip, (u16_t)len);
    if (i->netif.input(p, &i->netif) != ERR_OK) pbuf_free(p);
}

unsigned net_iface_index(const struct net_iface *i)
{
    return netif_get_index(&i->netif);
}

void net_iface_address(const struct net_iface *i, char out[64])
{
    ip6addr_ntoa_r(netif_ip6_addr(&i->netif, 0), out, 64);
}
