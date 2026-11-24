// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* The AWDL node: a state machine driven by explicit time. It follows
 * owly-awdl's node.rs, election.rs, sync.rs, channel.rs and peer.rs, and
 * the radio-full handling of its driver.rs: a PSF every 110 TU and a MIF in
 * the middle of every slot, the peer table swept and the election run
 * every second, the channel sequence derived from the sync master's, data
 * sent only in the slots a peer shares with us and clear of their edges,
 * a peer's early TCP SYN held until it is valid, and an ICMPv6 echo to
 * each valid peer every few seconds (OWLY-2). It does no I/O and reads no
 * clock: the host feeds it frames, packets and the time, and it calls the
 * host back. */
#include "awdl.h"

/* ---------------------------------------------------------- channels, AWDL-12 */

unsigned awdl_seq_intersect(const struct awdl_sequence *in, uint8_t keep, struct awdl_sequence *out)
{
    awdl_seq_empty(out, in->encoding);
    unsigned kept = 0;
    for (unsigned i = 0; i < AWDL_SLOTS; i++)
        if (keep != 0 && awdl_seq_channel(in, i) == keep) {
            memcpy(out->slots[i], in->slots[i], 2);
            kept++;
        }
    return kept;
}

uint8_t awdl_seq_dominant(const struct awdl_sequence *s, uint8_t prefer)
{
    uint8_t counts[256] = { 0 };
    for (unsigned i = 0; i < AWDL_SLOTS; i++) {
        uint8_t c = awdl_seq_channel(s, i);
        if (c != 0)
            counts[c]++;
    }
    unsigned best = 0;
    for (unsigned c = 1; c < 256; c++)
        if (counts[c] > counts[best])
            best = c;
    if (counts[best] == 0)
        return 0;
    if (prefer != 0 && counts[prefer] == counts[best])
        return prefer;
    return (uint8_t)best;
}

/* ---------------------------------------------------------- the election, AWDL-7 to AWDL-9 */

void awdl_election_init(struct awdl_election *e, const awdl_mac *own)
{
    e->own = *own;
    e->master = *own;
    e->sync = *own;
    e->height = 0;
    e->master_metric = AWDL_METRIC_INIT;
    e->master_counter = AWDL_COUNTER_INIT;
    e->self_metric = AWDL_METRIC_INIT;
    e->self_counter = AWDL_COUNTER_INIT;
}

bool awdl_election_is_master(const struct awdl_election *e)
{
    return awdl_mac_eq(&e->sync, &e->own);
}

/* Compares two masters as `ranking` says: by counter then metric, as owly
 * ranks in shared mode, or by metric then counter, as Apple devices do. */
static int rank_cmp(const struct awdl_election *a, const struct awdl_election *b, enum awdl_ranking r)
{
    uint32_t a1 = r == AWDL_RANK_COUNTER_FIRST ? a->master_counter : a->master_metric;
    uint32_t a2 = r == AWDL_RANK_COUNTER_FIRST ? a->master_metric : a->master_counter;
    uint32_t b1 = r == AWDL_RANK_COUNTER_FIRST ? b->master_counter : b->master_metric;
    uint32_t b2 = r == AWDL_RANK_COUNTER_FIRST ? b->master_metric : b->master_counter;
    if (a1 != b1)
        return a1 < b1 ? -1 : 1;
    if (a2 != b2)
        return a2 < b2 ? -1 : 1;
    return 0;
}

/* A node follows whichever valid peer offers the best master, then the
 * shorter path, then the higher address, and never a peer that follows it. */
bool awdl_election_run(struct awdl_election *e, const struct awdl_election *const *peers, size_t n,
                       enum awdl_ranking ranking)
{
    awdl_mac old_master = e->master, old_sync = e->sync;
    e->height = 0;
    e->master = e->own;
    e->sync = e->own;
    e->master_metric = e->self_metric;
    e->master_counter = e->self_counter;
    struct awdl_election best = *e;
    for (size_t i = 0; i < n; i++) {
        const struct awdl_election *p = peers[i];
        /* Its height is whatever it sent: no arithmetic on it. */
        if (p->height >= AWDL_MAX_HEIGHT)
            continue;
        if (awdl_mac_eq(&p->sync, &e->own))
            continue;
        int c = rank_cmp(p, &best, ranking);
        if (c < 0)
            continue;
        if (c == 0
            && (p->height > best.height
                || (p->height == best.height && awdl_mac_cmp(&p->own, &best.own) <= 0)))
            continue;
        best = *p;
    }
    if (!awdl_mac_eq(&best.own, &e->own)) {
        e->master = best.master;
        e->sync = best.own;
        e->master_metric = best.master_metric;
        e->master_counter = best.master_counter;
        e->height = best.height + 1;
    }
    return !awdl_mac_eq(&e->master, &old_master) || !awdl_mac_eq(&e->sync, &old_sync);
}

/* ---------------------------------------------------------- the AW schedule, AWDL-7 */

void awdl_clock_init(struct awdl_clock *c, awdl_time now)
{
    c->last = now;
    c->aw_counter = 0;
}

static uint64_t since(const struct awdl_clock *c, awdl_time now)
{
    return now > c->last ? now - c->last : 0;
}

uint64_t awdl_clock_next_slot_us(const struct awdl_clock *c, awdl_time now)
{
    return AWDL_SLOT_US - since(c, now) % AWDL_SLOT_US;
}

/* The TX down-counter the Synchronization Parameters TLV announces. */
uint16_t awdl_clock_next_slot_tu(const struct awdl_clock *c, awdl_time now)
{
    return (uint16_t)(AWDL_SLOT_TU - (since(c, now) / AWDL_TU_US) % AWDL_SLOT_TU);
}

/* The AW count now, as announced in the TLV's next AW sequence number. */
uint16_t awdl_clock_current_aw(const struct awdl_clock *c, awdl_time now)
{
    uint64_t tu = since(c, now) / AWDL_TU_US;
    uint64_t aw = c->aw_counter + (tu % AWDL_SLOT_TU) / AWDL_AW_TU + AWDL_PRESENCE_MODE * (tu / AWDL_SLOT_TU);
    return (uint16_t)aw;
}

uint16_t awdl_clock_current_eaw(const struct awdl_clock *c, awdl_time now)
{
    return awdl_clock_current_aw(c, now) / AWDL_PRESENCE_MODE;
}

unsigned awdl_clock_slot(const struct awdl_clock *c, awdl_time now)
{
    return awdl_clock_current_eaw(c, now) % AWDL_SLOTS;
}

/* How far, in TU, a schedule announced at `now` is ahead of ours (negative:
 * behind). Slot counts compare modulo their wrap. */
int64_t awdl_clock_error_tu(const struct awdl_clock *c, awdl_time now, uint16_t next_tu,
                            uint16_t aw_counter)
{
    int32_t theirs = aw_counter / AWDL_PRESENCE_MODE;
    int32_t ours = awdl_clock_current_eaw(c, now);
    int32_t d = theirs - ours + AWDL_EAW_WRAP / 2;
    int32_t slots = ((d % AWDL_EAW_WRAP) + AWDL_EAW_WRAP) % AWDL_EAW_WRAP - AWDL_EAW_WRAP / 2;
    return (int64_t)slots * AWDL_SLOT_TU - ((int64_t)next_tu - awdl_clock_next_slot_tu(c, now));
}

/* AWDL-7: adopts a schedule announced at `now`: the next boundary is
 * `next_tu` away, and the slot holds AW `aw_counter`. */
void awdl_clock_update(struct awdl_clock *c, awdl_time now, uint16_t next_tu, uint16_t aw_counter)
{
    uint64_t next = next_tu < AWDL_SLOT_TU ? next_tu : AWDL_SLOT_TU;
    uint64_t back = (AWDL_SLOT_TU - next) * AWDL_TU_US;
    c->last = now > back ? now - back : 0;
    c->aw_counter = aw_counter & (uint16_t)~(AWDL_PRESENCE_MODE - 1);
}

/* How long until `now` is clear of `guard` at both edges of its slot. */
uint64_t awdl_clock_edge_wait(const struct awdl_clock *c, awdl_time now, uint64_t guard)
{
    uint64_t next = awdl_clock_next_slot_us(c, now);
    if (next < guard)
        return next + guard;
    uint64_t into = AWDL_SLOT_US - next;
    return guard > into ? guard - into : 0;
}

/* ---------------------------------------------------------- packet queues */

static void queue_init(struct awdl_queue *q, struct awdl_packet *slot, uint16_t *ring, uint16_t *spare,
                       uint8_t *data, uint16_t cap, size_t data_max)
{
    q->slot = slot;
    q->ring = ring;
    q->spare = spare;
    q->cap = cap;
    q->head = 0;
    q->n = 0;
    q->nspare = cap;
    q->data_max = data_max;
    for (uint16_t i = 0; i < cap; i++) {
        slot[i].data = data + (size_t)i * data_max;
        spare[i] = (uint16_t)(cap - 1 - i);
    }
}

static struct awdl_packet *queue_at(const struct awdl_queue *q, uint16_t i)
{
    return &q->slot[q->ring[(q->head + i) % q->cap]];
}

static void queue_pop_front(struct awdl_queue *q)
{
    q->spare[q->nspare++] = q->ring[q->head];
    q->head = (uint16_t)((q->head + 1) % q->cap);
    q->n--;
}

/* Removes the i-th packet, keeping the others' order. */
static void queue_remove(struct awdl_queue *q, uint16_t i)
{
    if (i == 0) {
        queue_pop_front(q);
        return;
    }
    uint16_t s = q->ring[(q->head + i) % q->cap];
    for (uint16_t k = i; k + 1 < q->n; k++)
        q->ring[(q->head + k) % q->cap] = q->ring[(q->head + k + 1) % q->cap];
    q->n--;
    q->spare[q->nspare++] = s;
}

/* A slot at the back for a packet of `len` bytes; the oldest makes room
 * when full (`dropped` counts it). NULL if the packet can never fit. */
static struct awdl_packet *queue_push(struct awdl_queue *q, size_t len, uint64_t *dropped)
{
    if (len > q->data_max)
        return NULL;
    if (q->n == q->cap) {
        queue_pop_front(q);
        if (dropped)
            (*dropped)++;
    }
    uint16_t s = q->spare[--q->nspare];
    q->ring[(q->head + q->n) % q->cap] = s;
    q->n++;
    return &q->slot[s];
}

static void packet_set(struct awdl_packet *p, awdl_time at, const awdl_mac *dst, const awdl_mac *src,
                       const uint8_t *ip, size_t len)
{
    p->at = at;
    p->tried = 0;
    p->dst = *dst;
    p->src = *src;
    p->len = (uint16_t)len;
    memcpy(p->data, ip, len);
}

/* ---------------------------------------------------------- peers */

const struct awdl_peer *awdl_node_peer(const struct awdl_node *n, const awdl_mac *mac)
{
    for (size_t i = 0; i < AWDL_MAX_PEERS; i++)
        if (n->peers[i].used && awdl_mac_eq(&n->peers[i].mac, mac))
            return &n->peers[i];
    return NULL;
}

static struct awdl_peer *peer_find(struct awdl_node *n, const awdl_mac *mac)
{
    return (struct awdl_peer *)awdl_node_peer(n, mac);
}

static struct awdl_peer *peer_add(struct awdl_node *n, const awdl_mac *mac, awdl_time now)
{
    for (size_t i = 0; i < AWDL_MAX_PEERS; i++) {
        struct awdl_peer *p = &n->peers[i];
        if (p->used)
            continue;
        memset(p, 0, sizeof *p);
        p->used = true;
        p->mac = *mac;
        p->last_seen = now;
        awdl_election_init(&p->election, mac);
        n->npeers++;
        return p;
    }
    return NULL;
}

static void peer_remove(struct awdl_node *n, struct awdl_peer *p)
{
    if (p->valid)
        n->host.peer_down(n->host.ctx, p);
    p->used = false;
    n->npeers--;
}

/* Takes in what one of its action frames said, except its sync parameters,
 * which the node reads against its own clock. Returns whether it has just
 * turned valid (AWDL-17). */
static bool peer_absorb(struct awdl_peer *p, const struct awdl_advert *a, bool mif)
{
    if (a->has_name) {
        memcpy(p->name, a->name, a->name_len);
        p->name_len = a->name_len;
    }
    if (a->has_version) {
        p->version = a->version;
        p->devclass = a->devclass;
    }
    if (a->has_sequence) {
        p->sequence = a->sequence;
        p->has_sequence = true;
    }
    if (a->has_ht) {
        p->ht = a->ht;
        p->has_ht = true;
    }
    if (a->has_election && (a->election_v2 || !p->v2)) {
        struct awdl_election *el = &p->election;
        el->master = a->election.master;
        el->height = a->election.height;
        el->master_metric = a->election.master_metric;
        el->self_metric = a->election.self_metric;
        if (a->election_v2) {
            el->sync = a->election.has_sync ? a->election.sync : p->mac;
            el->master_counter = a->election.master_counter;
            el->self_counter = a->election.self_counter;
            p->v2 = true;
        }
    }
    p->mif |= mif;
    bool was = p->valid;
    p->valid = p->mif && p->version != 0 && p->devclass != 0;
    return p->valid && !was;
}

/* ---------------------------------------------------------- set-up */

void awdl_config_init(struct awdl_config *c, const awdl_mac *addr, const char *name, uint8_t channel,
                      enum awdl_presence presence)
{
    memset(c, 0, sizeof *c);
    c->addr = *addr;
    size_t len = strlen(name);
    if (len > AWDL_NAME_MAX)
        len = AWDL_NAME_MAX;
    memcpy(c->name, name, len);
    c->name_len = len;
    c->channel = channel;
    c->presence = presence;
    c->rate = (struct awdl_rate){ .ht = false, .value = AWDL_RATE_BASIC };
    /* owly: on a fixed radio unicast goes once, unACKed (legacy patch
     * owl-05): a peer visits our channel for a slot or two, and a frame
     * retried past the visit holds up every frame behind it. */
    c->no_ack = presence == AWDL_PRESENCE_FIXED;
    c->accept_addba = false;
    c->neighbour_echo = true;
    c->has_rssi_threshold = true;
    c->rssi_threshold = AWDL_RSSI_THRESHOLD;
    c->max_peers = AWDL_MAX_PEERS;
}

void awdl_node_init(struct awdl_node *n, const struct awdl_config *cfg, const struct awdl_host *host,
                    awdl_time now)
{
    /* Everything but the queues' packet storage, which stays untouched
     * (and out of the resident set) until packets need it. */
    memset(n, 0, offsetof(struct awdl_node, udata));
    n->cfg = *cfg;
    if (n->cfg.max_peers == 0 || n->cfg.max_peers > AWDL_MAX_PEERS)
        n->cfg.max_peers = AWDL_MAX_PEERS;
    n->host = *host;
    awdl_clock_init(&n->clock, now);
    awdl_election_init(&n->election, &cfg->addr);
    awdl_seq_fixed(&n->sequence, cfg->channel);
    n->next_psf = now + AWDL_PSF_INTERVAL_US;
    n->next_mif = now;
    n->next_tick = now + AWDL_TICK_US;
    n->next_echo = now + AWDL_ECHO_US;
    queue_init(&n->unicast, n->uslot, n->uring, n->uspare, &n->udata[0][0], AWDL_UNICAST_QUEUE,
               AWDL_PACKET_MAX);
    queue_init(&n->multicast, n->mslot, n->mring, n->mspare, &n->mdata[0][0], AWDL_MULTICAST_QUEUE,
               AWDL_PACKET_MAX);
    queue_init(&n->held, n->hslot, n->hring, n->hspare, &n->hdata[0][0], AWDL_HELD_SYNS, AWDL_SYN_MAX);
}

/* Microseconds from `then` to `now`, 0 if `then` is later. */
static uint64_t elapsed(awdl_time now, awdl_time then)
{
    return now > then ? now - then : 0;
}

static bool fixed(const struct awdl_node *n)
{
    return n->cfg.presence == AWDL_PRESENCE_FIXED;
}

static uint16_t next_seq(struct awdl_node *n)
{
    uint16_t s = n->seq;
    n->seq = (uint16_t)((s + 1) & 0x0fff);
    return s;
}

static bool paused(const struct awdl_node *n, awdl_time now)
{
    return n->paused_until != 0 && now < n->paused_until;
}

/* Hands a built frame to the host. A full radio pauses sending for a
 * moment; an action frame it refuses is dropped, since by the next try its
 * sync TLVs and timestamp would be stale (owly's driver does the same). */
static enum awdl_tx_result transmit(struct awdl_node *n, awdl_time now, size_t len, const awdl_mac *to,
                                    struct awdl_rate rate, bool no_ack, bool action)
{
    struct awdl_tx tx = {
        .bytes = n->tx, .len = len, .to = *to, .rate = rate, .no_ack = no_ack, .action = action,
    };
    enum awdl_tx_result r = n->host.transmit(n->host.ctx, &tx);
    if (r == AWDL_TX_FULL) {
        n->stats.radio_full++;
        n->paused_until = now + AWDL_RETRY_PAUSE_US;
        if (action)
            n->stats.radio_gave_up++;
    }
    return r;
}

/* ---------------------------------------------------------- receiving */

static void release_syns(struct awdl_node *n, const awdl_mac *src)
{
    for (uint16_t i = 0; i < n->held.n;) {
        struct awdl_packet *q = queue_at(&n->held, i);
        if (!awdl_mac_eq(&q->src, src)) {
            i++;
            continue;
        }
        n->stats.syn_released++;
        n->stats.delivered++;
        n->host.deliver(n->host.ctx, &q->dst, &q->src, q->data, q->len);
        queue_remove(&n->held, i);
    }
}

static void evict_stalest(struct awdl_node *n)
{
    struct awdl_peer *stalest = NULL;
    for (size_t i = 0; i < AWDL_MAX_PEERS; i++) {
        struct awdl_peer *p = &n->peers[i];
        if (p->used && (!stalest || p->last_seen < stalest->last_seen))
            stalest = p;
    }
    if (!stalest)
        return;
    n->stats.peers_evicted++;
    peer_remove(n, stalest);
}

/* `late`: how long after its target TX time the frame went out (its PHY TX
 * time less its target TX time, AWDL-3), which is when its sync parameters
 * were true. */
static void rx_action(struct awdl_node *n, awdl_time now, uint32_t late, bool has_rssi, int8_t rssi,
                      const awdl_mac *src, uint8_t subtype, const uint8_t *tlvs, size_t len)
{
    struct awdl_peer *peer = peer_find(n, src);
    if (n->cfg.has_rssi_threshold && has_rssi) {
        int floor = peer ? n->cfg.rssi_threshold - AWDL_RSSI_HYSTERESIS : n->cfg.rssi_threshold;
        if (rssi < floor)
            return;
    }
    struct awdl_advert advert; /* parsed whole before anything changes */
    if (awdl_advert_parse(tlvs, len, &advert)) {
        n->stats.rx_malformed++;
        return;
    }
    /* SEC-11: a full table makes room by dropping the peer heard from
     * longest ago, so spoofed peers can't keep a real one out. */
    if (!peer && n->npeers >= n->cfg.max_peers)
        evict_stalest(n);
    n->stats.rx_action++;
    bool ours = awdl_mac_eq(&n->election.sync, src);
    if (!peer)
        peer = peer_add(n, src, now);
    if (!peer)
        return;
    peer->last_seen = now;
    if (has_rssi) {
        peer->rssi = rssi;
        peer->has_rssi = true;
    }
    bool up = peer_absorb(peer, &advert, subtype == AWDL_SUBTYPE_MIF);
    /* AWDL-7: the schedule as of the target TX time. An iPhone's frames wait
     * a millisecond or so for the air, once 28 ms: read as of arrival, one
     * moved our clock by as much (issue #107). Longer than a slot is no
     * schedule to follow. */
    if (advert.has_sync && late <= AWDL_TX_LATE_MAX_US) {
        awdl_time at = now > late ? now - late : now;
        peer->sync_offset_us = awdl_clock_error_tu(&n->clock, at, advert.sync.next_tu,
                                                   advert.sync.aw_counter)
                               * (int64_t)AWDL_TU_US;
        peer->has_sync_offset = true;
        /* Follow the master's schedule. */
        if (ours)
            awdl_clock_update(&n->clock, at, advert.sync.next_tu, advert.sync.aw_counter);
    }
    if (up) {
        n->host.peer_up(n->host.ctx, peer);
        release_syns(n, src);
    }
}

static bool held_already(const struct awdl_node *n, const struct awdl_msdu *m)
{
    for (uint16_t i = 0; i < n->held.n; i++) {
        const struct awdl_packet *q = queue_at(&n->held, i);
        if (q->len == m->len && awdl_mac_eq(&q->dst, &m->dst) && awdl_mac_eq(&q->src, &m->src)
            && memcmp(q->data, m->payload, m->len) == 0)
            return true;
    }
    return false;
}

/* AWDL-18: a phone can open its connection before the MIF that makes it
 * valid; delivered now, the kernel's answer would wait on a neighbour
 * entry that doesn't exist yet. */
static void hold_syn(struct awdl_node *n, awdl_time now, const struct awdl_msdu *m)
{
    if (m->len > AWDL_SYN_MAX || held_already(n, m))
        return;
    struct awdl_packet *q = queue_push(&n->held, m->len, NULL);
    if (!q)
        return;
    n->stats.syn_held++;
    packet_set(q, now, &m->dst, &m->src, m->payload, m->len);
}

static void rx_data(struct awdl_node *n, awdl_time now, const struct awdl_rx_frame *f)
{
    struct awdl_msdu msdus[AWDL_AMSDU_MAX];
    int count = awdl_msdus(&f->h, f->amsdu, f->body, f->body_len, msdus);
    if (count < 0) {
        n->stats.rx_malformed++;
        return;
    }
    n->stats.rx_data++;
    struct awdl_peer *peer = peer_find(n, &f->h.src);
    bool known = peer != NULL, valid = peer && peer->valid;
    /* AWDL-17: data proves a peer is still there; one deep in a transfer
     * can go seconds between the action frames we hear. */
    if (peer)
        peer->last_seen = now;
    for (int i = 0; i < count; i++) {
        const struct awdl_msdu *m = &msdus[i];
        /* NET-3: IPv6 only. And never another node's unicast, which a
         * monitor hears too. */
        if (m->ethertype != AWDL_ETHERTYPE_IPV6
            || !(awdl_mac_eq(&m->dst, &n->cfg.addr) || awdl_mac_multicast(&m->dst)))
            continue;
        if (valid) {
            if (awdl_mac_eq(&m->dst, &n->cfg.addr)
                && awdl_ip_is_echo_reply(m->payload, m->len, AWDL_ECHO_ID))
                continue;
            n->stats.delivered++;
            n->host.deliver(n->host.ctx, &m->dst, &m->src, m->payload, m->len);
        } else if (awdl_ip_bare_syn_to(&m->dst, &n->cfg.addr, m->payload, m->len)) {
            hold_syn(n, now, m);
        } else if (known) {
            /* owly, as legacy did, hands on data from any peer in its table. */
            n->stats.delivered++;
            n->host.deliver(n->host.ctx, &m->dst, &m->src, m->payload, m->len);
        }
    }
}

static void receive(struct awdl_node *n, awdl_time now, bool has_rssi, int8_t rssi,
                    const uint8_t *bytes, size_t len)
{
    struct awdl_rx_frame f;
    if (awdl_frame_parse(bytes, len, &f)) {
        n->stats.rx_malformed++;
        return;
    }
    switch (f.kind) {
    case AWDL_RX_ACTION:
        if (!awdl_mac_eq(&f.h.src, &n->cfg.addr))
            rx_action(n, now, f.phy_tx - f.target_tx, has_rssi, rssi, &f.h.src, f.subtype, f.tlvs, f.tlvs_len);
        break;
    case AWDL_RX_ADDBA:
        /* AWDL-15 */
        if (n->cfg.accept_addba && awdl_mac_eq(&f.h.dst, &n->cfg.addr) && peer_find(n, &f.h.src)) {
            struct awdl_wbuf w;
            awdl_w_init(&w, n->tx, sizeof n->tx);
            awdl_addba_response(&w, &n->cfg.addr, &f.h.src, next_seq(n), f.token, f.params, f.timeout);
            struct awdl_rate basic = { .ht = false, .value = AWDL_RATE_BASIC };
            transmit(n, now, w.len, &f.h.src, basic, false, true);
        }
        break;
    case AWDL_RX_DATA:
        if (!awdl_mac_eq(&f.h.src, &n->cfg.addr))
            rx_data(n, now, &f);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------- sending */

static void enqueue(struct awdl_node *n, awdl_time now, const awdl_mac *dst, const awdl_mac *src,
                    const uint8_t *ip, size_t len)
{
    if (awdl_mac_eq(dst, &n->cfg.addr)) {
        n->host.deliver(n->host.ctx, dst, src, ip, len);
        return;
    }
    struct awdl_queue *q;
    if (awdl_mac_multicast(dst)) {
        q = &n->multicast;
    } else {
        if (!peer_find(n, dst)) {
            n->stats.dropped_no_peer++;
            return;
        }
        q = &n->unicast;
    }
    /* A multicast already waiting for its slot goes once: the copies of
     * an mDNS burst would otherwise fill the queue and push out other
     * packets, such as a query, while they wait. */
    if (q == &n->multicast)
        for (uint16_t i = 0; i < q->n; i++) {
            const struct awdl_packet *w = queue_at(q, i);
            if (w->len == len && awdl_mac_eq(&w->dst, dst) && memcmp(w->data, ip, len) == 0)
                return;
        }
    struct awdl_packet *p = queue_push(q, len, &n->stats.dropped_queue_full);
    if (!p) {
        n->stats.dropped_too_big++;
        return;
    }
    packet_set(p, now, dst, src, ip, len);
}

/* The channel we're on in `slot`, as far as sending goes. */
static uint8_t own_channel(const struct awdl_node *n, unsigned slot)
{
    return fixed(n) ? n->cfg.channel : awdl_seq_channel(&n->sequence, slot);
}

static bool same_channel(const struct awdl_node *n, const struct awdl_peer *p, awdl_time t)
{
    unsigned slot = awdl_clock_slot(&n->clock, t);
    uint8_t own = own_channel(n, slot);
    uint8_t theirs = p->has_sequence ? awdl_seq_channel(&p->sequence, slot) : 0;
    return own != 0 && own == theirs;
}

/* AWDL-16: in a slot both are on the same channel, clear of the 3 TU guard
 * at its edges unless the neighbouring slot is shared too. */
uint64_t awdl_node_unicast_wait(const struct awdl_node *n, const struct awdl_peer *p, awdl_time now)
{
    uint64_t next = awdl_clock_next_slot_us(&n->clock, now);
    if (!same_channel(n, p, now))
        return next;
    uint64_t g = AWDL_UNICAST_GUARD_US;
    if (next < g)
        return same_channel(n, p, now + AWDL_SLOT_US) ? 0 : next + g;
    if (AWDL_SLOT_US - next < g) {
        awdl_time before = now > AWDL_SLOT_US ? now - AWDL_SLOT_US : 0;
        return same_channel(n, p, before) ? 0 : g - (AWDL_SLOT_US - next);
    }
    return 0;
}

/* The slots in which a valid peer whose schedule we know is on our
 * channel, as a mask of slot numbers. */
static uint32_t listening_slots(const struct awdl_node *n)
{
    uint32_t mask = 0;
    for (size_t i = 0; n->npeers > 0 && i < AWDL_MAX_PEERS; i++) {
        const struct awdl_peer *p = &n->peers[i];
        if (!p->used || !p->valid || !p->has_sequence)
            continue;
        for (unsigned slot = 0; slot < AWDL_SLOTS; slot++) {
            uint8_t own = own_channel(n, slot);
            if (own != 0 && own == awdl_seq_channel(&p->sequence, slot))
                mask |= 1u << slot;
        }
    }
    return mask;
}

/* How long until a valid peer is on our channel, clear of `guard` at the
 * slot's edges, early in the slot as its first AW may be all it spends
 * there; UINT64_MAX if no peer's schedule ever brings it. */
static uint64_t listening_wait(const struct awdl_node *n, awdl_time now, uint64_t guard)
{
    uint32_t mask = listening_slots(n);
    if (mask == 0)
        return UINT64_MAX;
    uint64_t wait = awdl_clock_edge_wait(&n->clock, now, guard);
    if (mask & (1u << awdl_clock_slot(&n->clock, now + wait)))
        return wait;
    uint64_t start = awdl_clock_next_slot_us(&n->clock, now);
    for (unsigned k = 0; k < AWDL_SLOTS; k++) {
        uint64_t at = start + (uint64_t)k * AWDL_SLOT_US + guard;
        if (mask & (1u << awdl_clock_slot(&n->clock, now + at)))
            return at;
    }
    return UINT64_MAX;
}

/* AWDL-16: multicast in slots 0 and 10, where Apple devices send it, clear
 * of a 16 TU guard. A fixed radio sends it with the unicast guard in a slot
 * a valid peer spends on its channel: an iPhone gives channel 6 one slot in
 * sixteen, so multicast sent in any slot, as owly does, reaches it about one
 * time in sixteen, and mDNS with it. Until such a peer is known, any slot. */
uint64_t awdl_node_multicast_wait(const struct awdl_node *n, awdl_time now)
{
    uint64_t guard = fixed(n) ? AWDL_UNICAST_GUARD_US : AWDL_MULTICAST_GUARD_US;
    uint64_t wait = awdl_clock_edge_wait(&n->clock, now, guard);
    if (fixed(n)) {
        uint64_t aimed = listening_wait(n, now, guard);
        return aimed != UINT64_MAX ? aimed : wait;
    }
    unsigned slot = awdl_clock_slot(&n->clock, now);
    if (wait > 0 || slot == AWDL_MULTICAST_SLOT_A || slot == AWDL_MULTICAST_SLOT_B)
        return wait;
    return awdl_clock_next_slot_us(&n->clock, now) + guard;
}

static void send_multicast(struct awdl_node *n, awdl_time now, unsigned *sent)
{
    while (*sent < AWDL_BURST && n->multicast.n > 0 && !paused(n, now)
           && awdl_node_multicast_wait(n, now) == 0) {
        struct awdl_packet *q = queue_at(&n->multicast, 0);
        struct awdl_wbuf w;
        awdl_w_init(&w, n->tx, sizeof n->tx);
        awdl_data_frame(&w, &n->cfg.addr, &q->dst, next_seq(n), n->awdl_seq, q->data, q->len);
        if (w.overflow) {
            queue_pop_front(&n->multicast);
            n->stats.dropped_too_big++;
            continue;
        }
        struct awdl_rate basic = { .ht = false, .value = AWDL_RATE_BASIC };
        if (transmit(n, now, w.len, &q->dst, basic, false, false) == AWDL_TX_FULL) {
            /* Tried again while its slot lasts, for a moment. */
            if (q->tried == 0)
                q->tried = now;
            if (elapsed(now, q->tried) > AWDL_MULTICAST_RETRY_US) {
                queue_pop_front(&n->multicast);
                n->stats.radio_gave_up++;
            }
            return;
        }
        n->awdl_seq++;
        n->stats.tx_multicast++;
        (*sent)++;
        queue_pop_front(&n->multicast);
    }
}

static void send_unicast(struct awdl_node *n, awdl_time now, unsigned *sent)
{
    awdl_mac blocked[AWDL_UNICAST_QUEUE];
    size_t nblocked = 0;
    for (uint16_t i = 0; i < n->unicast.n;) {
        struct awdl_packet *q = queue_at(&n->unicast, i);
        bool is_blocked = false;
        for (size_t k = 0; k < nblocked; k++)
            if (awdl_mac_eq(&blocked[k], &q->dst))
                is_blocked = true;
        const struct awdl_peer *peer = peer_find(n, &q->dst);
        bool open = *sent < AWDL_BURST && !paused(n, now) && !is_blocked && peer
                    && awdl_node_unicast_wait(n, peer, now) == 0;
        if (!open) {
            if (!is_blocked)
                blocked[nblocked++] = q->dst;
            i++;
            continue;
        }
        struct awdl_wbuf w;
        awdl_w_init(&w, n->tx, sizeof n->tx);
        awdl_data_frame(&w, &n->cfg.addr, &q->dst, next_seq(n), n->awdl_seq, q->data, q->len);
        if (w.overflow) {
            queue_remove(&n->unicast, i);
            n->stats.dropped_too_big++;
            continue;
        }
        /* Unicast waits for its peer's next slot when the radio is full,
         * as owly's driver keeps it (legacy patch owl-07). */
        if (transmit(n, now, w.len, &q->dst, n->cfg.rate, n->cfg.no_ack, false) == AWDL_TX_FULL)
            return;
        n->awdl_seq++;
        n->stats.tx_unicast++;
        (*sent)++;
        queue_remove(&n->unicast, i);
    }
}

static void send_action(struct awdl_node *n, awdl_time now, uint8_t subtype)
{
    struct awdl_own own = {
        .addr = n->cfg.addr,
        .name = n->cfg.name,
        .name_len = n->cfg.name_len,
        .devclass = AWDL_DEVCLASS_MACOS, /* as owly announces */
        .channel = n->cfg.channel,
        .master_channel = n->cfg.channel,
        .next_tu = awdl_clock_next_slot_tu(&n->clock, now),
        .aw_counter = awdl_clock_current_aw(&n->clock, now),
        .psf_interval_tu = AWDL_PSF_INTERVAL_TU,
        .sequence = &n->sequence,
        .election = &n->election,
        .has_ht = true,
        .ht = { AWDL_HT_CAPABILITIES, AWDL_HT_AMPDU_PARAMS, AWDL_HT_RX_MCS },
    };
    struct awdl_wbuf w;
    awdl_w_init(&w, n->tx, sizeof n->tx);
    awdl_action_frame(&w, &n->cfg.addr, next_seq(n), subtype, (uint32_t)now, &own);
    if (w.overflow)
        return;
    n->stats.tx_action++;
    struct awdl_rate basic = { .ht = false, .value = AWDL_RATE_BASIC };
    transmit(n, now, w.len, &AWDL_BROADCAST, basic, false, true);
}

/* ---------------------------------------------------------- the tick */

/* The sequence to advertise: derived from the sync master's, as the
 * radio's presence allows (AWDL-12). */
static void adopt_sequence(struct awdl_node *n)
{
    const struct awdl_peer *master = NULL;
    if (!awdl_election_is_master(&n->election)) {
        master = peer_find(n, &n->election.sync);
        if (master && !master->has_sequence)
            master = NULL;
    }
    n->has_no_overlap = false;
    if (fixed(n) || !master) {
        awdl_seq_fixed(&n->sequence, n->cfg.channel);
        return;
    }
    struct awdl_sequence ours;
    if (awdl_seq_intersect(&master->sequence, n->cfg.channel, &ours) == 0) {
        /* Rather than announce that we're never available, which would
         * have the phone stop addressing us, keep what we had. */
        n->stats.no_overlap++;
        n->has_no_overlap = true;
        n->no_overlap_master = n->election.sync;
        n->no_overlap_channel = awdl_seq_dominant(&master->sequence, 0);
        return;
    }
    n->sequence = ours;
}

static void tick(struct awdl_node *n, awdl_time now)
{
    for (size_t i = 0; i < AWDL_MAX_PEERS; i++) {
        struct awdl_peer *p = &n->peers[i];
        if (p->used && elapsed(now, p->last_seen) > AWDL_PEER_TIMEOUT_US)
            peer_remove(n, p);
    }
    /* Packets for a peer that's gone never go, and would hold up the rest
     * until they expired. */
    for (uint16_t i = 0; i < n->unicast.n;) {
        struct awdl_packet *q = queue_at(&n->unicast, i);
        if (!peer_find(n, &q->dst)) {
            queue_remove(&n->unicast, i);
            n->stats.dropped_no_peer++;
        } else if (elapsed(now, q->at) >= AWDL_PACKET_EXPIRY_US) {
            queue_remove(&n->unicast, i);
            n->stats.dropped_expired++;
        } else {
            i++;
        }
    }
    for (uint16_t i = 0; i < n->multicast.n;) {
        if (elapsed(now, queue_at(&n->multicast, i)->at) >= AWDL_PACKET_EXPIRY_US) {
            queue_remove(&n->multicast, i);
            n->stats.dropped_expired++;
        } else {
            i++;
        }
    }
    for (uint16_t i = 0; i < n->held.n;) {
        if (elapsed(now, queue_at(&n->held, i)->at) >= AWDL_SYN_HOLD_US)
            queue_remove(&n->held, i);
        else
            i++;
    }
    const struct awdl_election *valid[AWDL_MAX_PEERS];
    size_t nvalid = 0;
    for (size_t i = 0; i < AWDL_MAX_PEERS; i++)
        if (n->peers[i].used && n->peers[i].valid)
            valid[nvalid++] = &n->peers[i].election;
    awdl_election_run(&n->election, valid, nvalid,
                      fixed(n) ? AWDL_RANK_METRIC_FIRST : AWDL_RANK_COUNTER_FIRST);
    adopt_sequence(n);
}

static void service(struct awdl_node *n, awdl_time now)
{
    if (now >= n->next_tick) {
        tick(n, now);
        while (n->next_tick <= now)
            n->next_tick += AWDL_TICK_US;
    }
    if (now >= n->next_psf) {
        send_action(n, now, AWDL_SUBTYPE_PSF);
        while (n->next_psf <= now)
            n->next_psf += AWDL_PSF_INTERVAL_US;
    }
    if (now >= n->next_mif) {
        /* In the middle of the next slot. */
        send_action(n, now, AWDL_SUBTYPE_MIF);
        n->next_mif = now + awdl_clock_next_slot_us(&n->clock, now) + AWDL_SLOT_US / 2;
    }
    if (n->cfg.neighbour_echo && now >= n->next_echo) {
        /* OWLY-2: makes Apple peers learn our neighbour entry. */
        n->next_echo = now + AWDL_ECHO_US;
        n->echo_seq++;
        uint8_t ip[AWDL_IPV6_HEADER_LEN + 8 + sizeof AWDL_ECHO_PAYLOAD];
        for (size_t i = 0; i < AWDL_MAX_PEERS; i++) {
            const struct awdl_peer *p = &n->peers[i];
            if (!p->used || !p->valid)
                continue;
            size_t len = awdl_ip_echo_request(ip, sizeof ip, &n->cfg.addr, &p->mac, AWDL_ECHO_ID,
                                              n->echo_seq, (const uint8_t *)AWDL_ECHO_PAYLOAD,
                                              sizeof AWDL_ECHO_PAYLOAD - 1);
            enqueue(n, now, &p->mac, &n->cfg.addr, ip, len);
        }
    }
    unsigned sent = 0;
    send_multicast(n, now, &sent);
    send_unicast(n, now, &sent);
    if (sent >= AWDL_BURST && !paused(n, now))
        n->paused_until = now + AWDL_BURST_PAUSE_US;
}

/* ---------------------------------------------------------- inputs */

void awdl_node_frame(struct awdl_node *n, awdl_time now, bool has_rssi, int8_t rssi,
                     const uint8_t *bytes, size_t len)
{
    receive(n, now, has_rssi, rssi, bytes, len);
    service(n, now);
}

void awdl_node_outbound(struct awdl_node *n, awdl_time now, const awdl_mac *dst, const awdl_mac *src,
                        const uint8_t *ip, size_t len)
{
    enqueue(n, now, dst, src, ip, len);
    service(n, now);
}

void awdl_node_timer(struct awdl_node *n, awdl_time now)
{
    service(n, now);
}

bool awdl_node_takes_outbound(const struct awdl_node *n)
{
    return n->unicast.n < AWDL_UNICAST_QUEUE;
}

static awdl_time earliest(awdl_time a, awdl_time b)
{
    return a < b ? a : b;
}

awdl_time awdl_node_deadline(const struct awdl_node *n, awdl_time now)
{
    awdl_time t = earliest(earliest(n->next_tick, n->next_psf), n->next_mif);
    if (n->cfg.neighbour_echo)
        t = earliest(t, n->next_echo);
    if (paused(n, now)) {
        /* Data waits out the pause rather than spinning on it. */
        if (n->multicast.n > 0 || n->unicast.n > 0)
            t = earliest(t, n->paused_until);
        return t > now ? t : now;
    }
    if (n->multicast.n > 0) {
        uint64_t w = awdl_node_multicast_wait(n, now);
        t = earliest(t, now + (w > 0 ? w : 1));
    }
    awdl_mac seen[AWDL_UNICAST_QUEUE];
    size_t nseen = 0;
    for (uint16_t i = 0; i < n->unicast.n; i++) {
        const struct awdl_packet *q = queue_at(&n->unicast, i);
        bool done = false;
        for (size_t k = 0; k < nseen; k++)
            if (awdl_mac_eq(&seen[k], &q->dst))
                done = true;
        if (done)
            continue;
        seen[nseen++] = q->dst;
        const struct awdl_peer *p = awdl_node_peer(n, &q->dst);
        if (p) {
            uint64_t w = awdl_node_unicast_wait(n, p, now);
            t = earliest(t, now + (w > 0 ? w : 1));
        }
    }
    return t > now ? t : now;
}
