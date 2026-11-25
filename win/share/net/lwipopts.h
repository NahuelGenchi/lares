// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* lwIP for AirDrop's link (issue #109): IPv6 link-local only, on one
 * interface that AWDL carries. UDP for mDNS, TCP for the transfers, ICMPv6
 * for neighbour discovery and echoes. Blocking sockets in worker threads, as
 * src/airdrop/session.c does with GIO. */

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_TCPIP_CORE_LOCKING_INPUT   0

#define LWIP_IPV4                       0
#define LWIP_IPV6                       1
#define LWIP_ARP                        0
#define LWIP_ETHERNET                   0
#define LWIP_ICMP6                      1
#define LWIP_IPV6_MLD                   1   /* to take mDNS's group, ff02::fb */
#define LWIP_IPV6_AUTOCONFIG            0
#define LWIP_IPV6_DHCP6                 0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT   0
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS   0   /* AWDL's addresses come from unique MACs (NET-1) */
#define LWIP_IPV6_FORWARD               0
#define LWIP_IPV6_FRAG                  0
#define LWIP_IPV6_REASS                 0
#define LWIP_IPV6_NUM_ADDRESSES         1
#define LWIP_ND6_ALLOW_RA_UPDATES       0
#define LWIP_ND6_QUEUEING               0
#define LWIP_IPV6_SCOPES                1
#define MEMP_NUM_MLD6_GROUP             4
#define LWIP_MULTICAST_TX_OPTIONS       1

#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        0
#define LWIP_DHCP                       0
#define LWIP_IGMP                       0
#define LWIP_AUTOIP                     0

#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             0   /* lwip_ names only: Winsock's stay Winsock's */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 1   /* timeouts as an int of milliseconds */
#define LWIP_SO_LINGER                  1
#define SO_REUSE                        1   /* mDNS's port, for a publisher and a browser */
#define LWIP_SOCKET_SELECT              1
#define LWIP_SOCKET_POLL                1
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_NETIF_API                  0
#define LWIP_NETIF_LOOPBACK             0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_ERRNO_STDINCLUDE           1

/* AWDL's MTU is 1450 (awdl0's on Linux): 1450 - 40 - 20, less TCP options. */
#define TCP_MSS                         1380
#define TCP_WND                         (44 * TCP_MSS)
#define TCP_SND_BUF                     (44 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_QUEUE_OOSEQ                 1
#define LWIP_TCP_SACK_OUT               1   /* the link loses frames; tell the sender which came */
#define LWIP_TCP_MAX_SACK_NUM           4
#define LWIP_TCP_TIMESTAMPS             0
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      16

#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (768 * 1024)
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_TCP_PCB                48
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                512
#define MEMP_NUM_NETCONN                64
#define MEMP_NUM_NETBUF                 32
#define MEMP_NUM_TCPIP_MSG_INPKT        256
#define MEMP_NUM_TCPIP_MSG_API          64
#define PBUF_POOL_SIZE                  128
#define PBUF_POOL_BUFSIZE               1536
#define LWIP_CHECKSUM_ON_COPY           1

#define TCPIP_THREAD_NAME               "lwip"
#define TCPIP_THREAD_STACKSIZE          0
#define TCPIP_THREAD_PRIO               1
#define TCPIP_MBOX_SIZE                 512
#define DEFAULT_THREAD_STACKSIZE        0
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       32
#define DEFAULT_TCP_RECVMBOX_SIZE       64
#define DEFAULT_ACCEPTMBOX_SIZE         16

#define LWIP_STATS                      0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0

/* What lwIP asks of the system, in sys_arch.c. */
unsigned int lares_lwip_rand(void);
#define LWIP_RAND() lares_lwip_rand()
