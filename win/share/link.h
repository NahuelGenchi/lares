// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* The AWDL link on Windows (issue #109): what lares-awdl's main.c and
 * radio.c do on Linux, on the RTL8188EU over WinUSB. node.c and frame.c run
 * unchanged on a thread of their own, woken by the adapter's transfers, a
 * high-resolution timer and packets from the IP stack. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "awdl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct link_callbacks {
    void *ctx;
    /* The link is up on `channel` with `addr`, or failed to come up. */
    void (*ready)(void *ctx, uint8_t channel, const awdl_mac *addr);
    void (*failed)(void *ctx, const char *message);
    /* A peer turned valid (`up`) or went away. */
    void (*peer)(void *ctx, const awdl_mac *mac, const uint8_t *name, size_t len, bool up);
    /* An IPv6 packet for the IP stack, on the link's thread. */
    void (*deliver)(void *ctx, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len);
};

struct link_config {
    uint8_t channel;            /* 6: the adapter is 2.4 GHz only */
    awdl_mac addr;              /* AWDL-2: random, for the session */
    char name[AWDL_NAME_MAX + 1];
    const uint8_t *firmware;
    size_t firmware_len;
    bool listen_only;           /* hear and count, send nothing (--listen) */
};

struct link;

/* Starts the link's thread; `ready` or `failed` follows. NULL if the thread
 * couldn't start. */
struct link *link_start(const struct link_config *cfg, const struct link_callbacks *cb);
/* An IPv6 packet from the IP stack, with its Ethernet addresses; queued for
 * the link's thread, dropped if the queue is full. Any thread. */
void link_send(struct link *l, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len);
/* Updates the AWDL node's ARPA TLV hostname from any thread. */
void link_set_name(struct link *l, const char *name);
/* Stops the thread, powers the chip down and closes the adapter. */
void link_stop(struct link *l);

/* What the link has seen, for --listen and the log. */
struct link_counts {
    uint64_t frames, ofdm, awdl_action, awdl_data, bad, rx_xfers, tx_sent, tx_failed;
    uint64_t raw_sent, probe_resp, probe_resp_retry;   /* --probe-test */
    int8_t last_rssi;
    bool tsf_running;
};
void link_counts(struct link *l, struct link_counts *out);
/* The chip's own receive counters and a few registers, for checks. */
void link_dump(struct link *l, FILE *out);
/* A management frame (no FCS) sent once at 6 Mbit/s from the link's thread,
 * for checks such as --probe-test. Any thread. */
void link_raw(struct link *l, const uint8_t *frame, size_t len);

/* A random locally administered unicast address (AWDL-2). */
void link_session_address(awdl_mac *m);

#ifdef __cplusplus
}
#endif
