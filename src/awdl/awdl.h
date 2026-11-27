// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* lares-awdl: Apple Wireless Direct Link (AWDL) on an ordinary Linux Wi-Fi
 * card, for Lares' AirDrop. It follows owly (MIT, Copyright (c) 2026 T4T5):
 * owly-awdl's sans-IO node and owly-radio's injection backend, ported to C
 * with libc only. Rule IDs (AWDL-12, NET-2, SEC-11, OWLY-2) are those of
 * owly's docs/SPEC.md and docs/implementation.md.
 *
 * frame.c holds what goes over the air (radiotap, 802.11, AWDL action
 * frames and TLVs, data frames, the IPv6 bits the data path looks at),
 * node.c the AWDL node as a state machine driven by explicit time, radio.c
 * the card (netlink, interfaces, the TAP, the crash journal) and main.c the
 * command line and the event loop. frame.c and node.c do no I/O and read no
 * clock, so the unit tests run them on plain data. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ timing
 * Every interval lares-awdl keeps, in one place. Times are microseconds
 * on CLOCK_MONOTONIC; 0 is never a time. */
typedef uint64_t awdl_time;

#define AWDL_TU_US              1024u   /* one time unit (§0.2) */
#define AWDL_AW_TU              16u     /* availability window, AWDL-7, §3.2 AW period */
#define AWDL_PRESENCE_MODE      4u      /* AWs per slot, §3.2 presence mode */
#define AWDL_SLOT_TU            (AWDL_AW_TU * AWDL_PRESENCE_MODE)       /* 64 TU */
#define AWDL_SLOT_US            ((uint64_t)AWDL_SLOT_TU * AWDL_TU_US)   /* 65.5 ms */
#define AWDL_SLOTS              16u     /* slots in a channel sequence, AWDL-10 */
#define AWDL_PERIOD_US          (AWDL_SLOT_US * AWDL_SLOTS)             /* 1.05 s */
#define AWDL_EAW_WRAP           (1 << 14) /* slot counts wrap with the u16 AW count */
#define AWDL_UNICAST_GUARD_US   (3u * AWDL_TU_US)   /* AWDL-16 */
#define AWDL_MULTICAST_GUARD_US (16u * AWDL_TU_US)  /* AWDL-16 */
#define AWDL_MULTICAST_SLOT_A   0u      /* AWDL-16: where Apple devices send multicast */
#define AWDL_MULTICAST_SLOT_B   10u
#define AWDL_PSF_INTERVAL_TU    110u    /* §3.2 action frame period */
#define AWDL_PSF_INTERVAL_US    ((uint64_t)AWDL_PSF_INTERVAL_TU * AWDL_TU_US)
#define AWDL_PEER_TIMEOUT_US    5000000u /* AWDL-17: a silent valid peer is dropped */
#define AWDL_TICK_US            1000000u /* peer sweep and election, as owly runs them */
#define AWDL_ECHO_US            3000000u /* OWLY-2: an ICMPv6 echo to each valid peer */
#define AWDL_PACKET_EXPIRY_US   2000000u /* packets waiting longer for their slot are dropped */
#define AWDL_SYN_HOLD_US        2000000u /* AWDL-18 */
#define AWDL_TX_LATE_MAX_US     AWDL_SLOT_US /* a frame's PHY TX time past its target, AWDL-3 */
#define AWDL_BURST_PAUSE_US     500u    /* after AWDL_BURST frames for one input */
#define AWDL_RETRY_PAUSE_US     500u    /* after the radio's queue was full */
#define AWDL_MULTICAST_RETRY_US 100000u /* multicast the radio refused is retried this long */
#define AWDL_STATS_US           10000000u /* the STATS line on stderr */

/* Setup, in radio.c: blocking steps before the loop starts. */
#define AWDL_NETLINK_TIMEOUT_S  5       /* a kernel that never answers */
#define AWDL_IBSS_START_MS      10000u  /* mac80211 scans once before the cell starts */
#define AWDL_REGDOM_APPLY_MS    5000u
#define AWDL_SETUP_POLL_MS      50u
#define AWDL_REGDOM_POLL_MS     100u
#define AWDL_NMCLI_TIMEOUT_MS   15000u
#define AWDL_AUTOCONNECT_MS     10000u  /* NetworkManager's own reconnect, before ours */
#define AWDL_IBSS_BEACON_TU     10000u  /* the cell beacons as rarely as cfg80211 allows */

/* ------------------------------------------------------------ sizes */
#define AWDL_MAX_PEERS          512u    /* SEC-11, owly's MAX_PEERS */
#define AWDL_UNICAST_QUEUE      128u
#define AWDL_MULTICAST_QUEUE    16u
#define AWDL_HELD_SYNS          128u    /* AWDL-18 */
#define AWDL_SYN_MAX            128u    /* a bare SYN: 40 bytes of IPv6, at most 60 of TCP */
#define AWDL_BURST              64u     /* frames sent for one input at most */
#define AWDL_AMSDU_MAX          16u     /* AWDL-14: subframes unpacked from one frame */
#define AWDL_NAME_MAX           64u     /* host names kept from or put in an ARPA TLV */
#define AWDL_TAP_MTU            1450u   /* awdl0, as owly sets it */
#define AWDL_MONITOR_MTU        8192u   /* asked of the monitor, so A-MSDUs fit */
#define AWDL_PACKET_MAX         AWDL_TAP_MTU
#define AWDL_RADIOTAP_MAX       16u     /* the most a radiotap header of ours takes */
#define AWDL_FRAME_MAX          2304u   /* frames we build, MAC header on */
#define AWDL_RX_BUF             16384u  /* one captured frame or TAP frame */

/* ------------------------------------------------------------ protocol values */
#define AWDL_METRIC_INIT        60u     /* AWDL-8 */
#define AWDL_COUNTER_INIT       0u      /* AWDL-8 */
#define AWDL_MAX_HEIGHT         10u     /* longest path to a master that is followed */
#define AWDL_RSSI_THRESHOLD     (-65)   /* owly: weaker new peers aren't taken on... */
#define AWDL_RSSI_HYSTERESIS    5       /* ...and known ones are kept down to 5 dB below */
#define AWDL_VERSION            0x34    /* the AWDL version announced, 3.4 as owly */
#define AWDL_DEVCLASS_MACOS     1u      /* Version TLV device classes, AWDL-6 */
#define AWDL_DEVCLASS_IOS       2u
#define AWDL_DEVCLASS_TVOS      8u
#define AWDL_HT_CAPABILITIES    0x11ce  /* owly's HT values: one stream, MCS 0-7 */
#define AWDL_HT_AMPDU_PARAMS    0x1b
#define AWDL_HT_RX_MCS          0xffu
#define AWDL_RATE_BASIC         24u     /* 12 Mbit/s in radiotap's 500 kbit/s units */
#define AWDL_ECHO_ID            0x4f77  /* OWLY-2's identifier and payload */
#define AWDL_ECHO_PAYLOAD       "owly"

/* ------------------------------------------------------------ addresses */
typedef struct {
    uint8_t a[6];
} awdl_mac;

extern const awdl_mac AWDL_BSSID;       /* AWDL-1: 00:25:00:ff:94:73 */
extern const awdl_mac AWDL_BROADCAST;

static inline bool awdl_mac_eq(const awdl_mac *x, const awdl_mac *y)
{
    return memcmp(x->a, y->a, 6) == 0;
}

static inline int awdl_mac_cmp(const awdl_mac *x, const awdl_mac *y)
{
    return memcmp(x->a, y->a, 6);
}

static inline bool awdl_mac_multicast(const awdl_mac *m)
{
    return m->a[0] & 1;
}

/* "aa:bb:cc:dd:ee:ff" into out[18]. */
void awdl_mac_format(const awdl_mac *m, char out[18]);
/* NET-1: the modified EUI-64 link-local address of a MAC. */
void awdl_link_local(const awdl_mac *m, uint8_t out[16]);
/* The MAC a modified EUI-64 link-local address was made from, if it was. */
bool awdl_mac_from_link_local(const uint8_t addr[16], awdl_mac *out);

/* ------------------------------------------------------------ channels, AWDL-10 to AWDL-12 */
enum { AWDL_ENC_SIMPLE = 0, AWDL_ENC_LEGACY = 1, AWDL_ENC_OPCLASS = 3 };

/* A channel sequence. Each slot keeps the bytes it came in, so a sequence
 * copied from a peer goes out byte for byte. */
struct awdl_sequence {
    uint8_t encoding;
    uint8_t slots[AWDL_SLOTS][2];
};

uint32_t awdl_channel_freq(uint8_t channel);            /* MHz, 0 for none */
uint8_t awdl_channel_from_freq(uint32_t mhz);           /* 0 for none */
uint8_t awdl_channel_opclass(uint8_t channel);
bool awdl_channel_social(uint8_t channel);              /* AWDL-11: 6, 44, 149 */
void awdl_seq_empty(struct awdl_sequence *s, uint8_t encoding);
void awdl_seq_fixed(struct awdl_sequence *s, uint8_t channel);
uint8_t awdl_seq_channel(const struct awdl_sequence *s, unsigned slot);
size_t awdl_seq_wire_len(const struct awdl_sequence *s);
int awdl_seq_parse(const uint8_t *v, size_t len, struct awdl_sequence *s);
/* AWDL-12: `in` with every slot blanked that doesn't name `keep`; returns
 * how many were kept. */
unsigned awdl_seq_intersect(const struct awdl_sequence *in, uint8_t keep, struct awdl_sequence *out);
/* The channel most slots name, `prefer` winning exact ties, 0 for none. */
uint8_t awdl_seq_dominant(const struct awdl_sequence *s, uint8_t prefer);

/* ------------------------------------------------------------ writing bytes */
struct awdl_wbuf {
    uint8_t *p;
    size_t len, cap;
    bool overflow;      /* something didn't fit; the bytes are unusable */
};

void awdl_w_init(struct awdl_wbuf *w, uint8_t *p, size_t cap);
void awdl_w_u8(struct awdl_wbuf *w, uint8_t v);
void awdl_w_le16(struct awdl_wbuf *w, uint16_t v);
void awdl_w_be16(struct awdl_wbuf *w, uint16_t v);
void awdl_w_le32(struct awdl_wbuf *w, uint32_t v);
void awdl_w_mac(struct awdl_wbuf *w, const awdl_mac *m);
void awdl_w_bytes(struct awdl_wbuf *w, const void *b, size_t n);
void awdl_w_zeros(struct awdl_wbuf *w, size_t n);
void awdl_seq_put(struct awdl_wbuf *w, const struct awdl_sequence *s);

/* ------------------------------------------------------------ radiotap */
struct awdl_rate {
    bool ht;            /* HT MCS on 20 MHz, else a legacy rate */
    bool sgi;
    uint8_t value;      /* legacy: 500 kbit/s units; HT: the MCS */
};

struct awdl_rx {
    const uint8_t *frame;       /* the 802.11 frame, FCS stripped */
    size_t len;
    bool has_rssi, has_freq;
    int8_t rssi;                /* dBm */
    uint16_t freq;              /* MHz */
};

/* 0, or -1 for a malformed header or a frame that failed its FCS. */
int awdl_radiotap_parse(const uint8_t *buf, size_t len, struct awdl_rx *rx);
/* The header for injecting at `rate`, without ACK and retries when
 * `no_ack`, into out[AWDL_RADIOTAP_MAX]; returns its length. */
size_t awdl_radiotap_tx(uint8_t *out, struct awdl_rate rate, bool no_ack);
uint32_t awdl_crc32(const uint8_t *p, size_t n);

/* ------------------------------------------------------------ 802.11 and AWDL frames */
#define AWDL_FC_ACTION          0x00d0
#define AWDL_FC_DATA            0x0008
#define AWDL_FC_QOS_DATA        0x0088
#define AWDL_HEADER_LEN         24u     /* 802.11 MAC header without QoS */
#define AWDL_ACTION_HEADER_LEN  16u     /* AWDL-3 */
#define AWDL_DATA_HEADER_LEN    16u     /* LLC/SNAP and the AWDL data header, AWDL-13 */
#define AWDL_SUBTYPE_PSF        0u
#define AWDL_SUBTYPE_MIF        3u
#define AWDL_ETHERTYPE_IPV6     0x86dd

struct awdl_hdr {
    uint16_t fc;
    awdl_mac dst, src, bssid;   /* addresses 1, 2 and 3 */
    uint16_t seq;               /* 12-bit sequence number */
};

enum awdl_rx_kind { AWDL_RX_OTHER, AWDL_RX_ACTION, AWDL_RX_ADDBA, AWDL_RX_DATA };

struct awdl_rx_frame {
    enum awdl_rx_kind kind;
    struct awdl_hdr h;
    /* AWDL_RX_ACTION: a PSF or MIF */
    uint8_t subtype;
    uint32_t phy_tx, target_tx;
    const uint8_t *tlvs;
    size_t tlvs_len;
    /* AWDL_RX_ADDBA: a Block Ack request, AWDL-15 */
    uint8_t token;
    uint16_t params, timeout;
    /* AWDL_RX_DATA: one MSDU or an A-MSDU */
    bool amsdu;
    const uint8_t *body;
    size_t body_len;
};

/* 0 with the frame classified (OTHER for anything not AWDL's), -1 for a
 * frame shorter than its headers say. */
int awdl_frame_parse(const uint8_t *b, size_t len, struct awdl_rx_frame *f);

struct awdl_msdu {
    awdl_mac dst, src;
    uint16_t ethertype;
    const uint8_t *payload;
    size_t len;
};

/* AWDL-13, AWDL-14: the packets in a data frame's body, at most
 * AWDL_AMSDU_MAX; returns how many, or -1 for a malformed or oversized one. */
int awdl_msdus(const struct awdl_hdr *h, bool amsdu, const uint8_t *body, size_t len,
               struct awdl_msdu out[AWDL_AMSDU_MAX]);

void awdl_put_header(struct awdl_wbuf *w, uint16_t fc, const awdl_mac *dst, const awdl_mac *src,
                     uint16_t seq);
void awdl_data_frame(struct awdl_wbuf *w, const awdl_mac *src, const awdl_mac *dst, uint16_t seq,
                     uint16_t awdl_seq, const uint8_t *packet, size_t len);
void awdl_amsdu_frame(struct awdl_wbuf *w, const awdl_mac *src, const awdl_mac *dst, uint16_t seq,
                      uint16_t awdl_seq, const uint8_t *const *packets, const size_t *lens, size_t n);
void awdl_addba_response(struct awdl_wbuf *w, const awdl_mac *src, const awdl_mac *dst,
                         uint16_t seq, uint8_t token, uint16_t requested, uint16_t timeout);

/* ------------------------------------------------------------ TLVs, AWDL-4 to AWDL-6 */
enum {
    AWDL_TLV_SERVICE_RESPONSE = 2,
    AWDL_TLV_SYNC_PARAMS = 4,
    AWDL_TLV_ELECTION_PARAMS = 5,
    AWDL_TLV_SERVICE_PARAMS = 6,
    AWDL_TLV_HT_CAPABILITIES = 7,
    AWDL_TLV_DATA_PATH_STATE = 12,
    AWDL_TLV_ARPA = 16,
    AWDL_TLV_IEEE80211_CONTAINER = 17,
    AWDL_TLV_CHANNEL_SEQUENCE = 18,
    AWDL_TLV_SYNC_TREE = 20,
    AWDL_TLV_VERSION = 21,
    AWDL_TLV_BLOOM_FILTER = 22,
    AWDL_TLV_ELECTION_PARAMS_V2 = 24,
};

struct awdl_tlv {
    uint8_t type;
    const uint8_t *value;
    size_t len;
};

struct awdl_tlv_iter {
    const uint8_t *p;
    size_t left;
};

/* AWDL-4, AWDL-5: 1 with the next TLV, 0 at the end (fewer than 3 bytes
 * left, all zero), -1 for a truncated TLV or a nonzero tail. */
int awdl_tlv_next(struct awdl_tlv_iter *it, struct awdl_tlv *t);

struct awdl_sync_params {
    uint8_t next_aw_channel;
    uint16_t next_tu;           /* TX down-counter: TU to the next slot boundary */
    uint8_t master_channel;
    uint16_t aw_period, af_period;
    awdl_mac master;
    uint8_t presence_mode;
    uint16_t aw_counter;        /* next AW sequence number */
};

struct awdl_election_params {
    awdl_mac master;
    bool has_sync;              /* only version 2 names the node synced to */
    awdl_mac sync;
    uint32_t height, master_metric, self_metric, master_counter, self_counter;
};

struct awdl_ht {
    uint16_t capabilities;
    uint8_t ampdu_params;
    uint32_t rx_mcs;            /* MCS 0-31, a bit each */
};

struct awdl_data_path {
    bool has_country, has_infra;
    uint8_t country[3];
    awdl_mac infra;
};

/* What one action frame's TLVs say, decoded whole before anything acts on
 * them, so a malformed TLV late in a frame can't half-update a peer. */
struct awdl_advert {
    bool has_sync, has_election, election_v2, has_sequence, has_name, has_version, has_ht,
        has_data_path;
    struct awdl_sync_params sync;
    struct awdl_election_params election;
    struct awdl_sequence sequence;
    uint8_t name_len;
    uint8_t name[AWDL_NAME_MAX];        /* raw bytes, not terminated */
    uint8_t version, devclass;
    struct awdl_ht ht;
    struct awdl_data_path data_path;
};

int awdl_sync_params_parse(const uint8_t *v, size_t len, struct awdl_sync_params *s);
int awdl_election_v1_parse(const uint8_t *v, size_t len, struct awdl_election_params *e);
int awdl_election_v2_parse(const uint8_t *v, size_t len, struct awdl_election_params *e);
int awdl_ht_parse(const uint8_t *v, size_t len, struct awdl_ht *ht);
int awdl_data_path_parse(const uint8_t *v, size_t len, struct awdl_data_path *d);
int awdl_arpa_parse(const uint8_t *v, size_t len, uint8_t name[AWDL_NAME_MAX], uint8_t *name_len);
/* 0, or -1 for a malformed TLV; unknown types are skipped by length. */
int awdl_advert_parse(const uint8_t *body, size_t len, struct awdl_advert *a);

struct awdl_election {
    awdl_mac own;               /* the node itself */
    awdl_mac master;            /* the top master it follows */
    awdl_mac sync;              /* the node it syncs to, one hop towards the master */
    uint32_t height;            /* hops to the master */
    uint32_t master_metric, master_counter, self_metric, self_counter;
};

/* What a node announces about itself, for writing its TLVs. */
struct awdl_own {
    awdl_mac addr;
    const char *name;
    size_t name_len;
    uint8_t devclass;
    uint8_t channel;            /* the channel the radio is on now */
    uint8_t master_channel;     /* the channel it serves in every slot it can */
    uint16_t next_tu, aw_counter, psf_interval_tu;
    const struct awdl_sequence *sequence;
    const struct awdl_election *election;
    bool has_ht;
    struct awdl_ht ht;
};

/* A PSF's or MIF's TLVs, in the order of §3.2: 4, 5, 18, 24, 6, 7, then
 * 16 on a MIF, then 12 and 21. */
void awdl_put_tlvs(struct awdl_wbuf *w, const struct awdl_own *own, bool mif);
void awdl_put_arpa(struct awdl_wbuf *w, const char *name, size_t len);
/* AWDL-3: the action frame, stamped with `time` (the sender's µs), TLVs on. */
void awdl_action_frame(struct awdl_wbuf *w, const awdl_mac *src, uint16_t seq, uint8_t subtype,
                       uint32_t time, const struct awdl_own *own);

/* ------------------------------------------------------------ IPv6 on the data path, §3.5, §4 */
#define AWDL_IPV6_HEADER_LEN    40u

/* A TCP SYN to `local`, with nothing else: how a peer opens a connection. */
bool awdl_ip_bare_syn_to(const awdl_mac *dst, const awdl_mac *local, const uint8_t *ip, size_t len);
/* OWLY-2: an ICMPv6 echo request, link-local to link-local. */
size_t awdl_ip_echo_request(uint8_t *out, size_t cap, const awdl_mac *src, const awdl_mac *dst,
                            uint16_t id, uint16_t seq, const uint8_t *payload, size_t plen);
bool awdl_ip_is_echo_reply(const uint8_t *ip, size_t len, uint16_t id);

/* A JSON string, quotes included, for bytes off the air: control
 * characters escaped, invalid UTF-8 replaced by U+FFFD. Returns its length,
 * or 0 when it doesn't fit in `cap` (a NUL is written after it). */
size_t awdl_json_string(char *out, size_t cap, const uint8_t *s, size_t len);

/* ------------------------------------------------------------ the node (node.c) */
enum awdl_ranking { AWDL_RANK_COUNTER_FIRST, AWDL_RANK_METRIC_FIRST };

void awdl_election_init(struct awdl_election *e, const awdl_mac *own);
bool awdl_election_is_master(const struct awdl_election *e);
/* AWDL-7 to AWDL-9: runs the election over the valid peers' states.
 * Returns whether the master or the node synced to changed. */
bool awdl_election_run(struct awdl_election *e, const struct awdl_election *const *peers, size_t n,
                       enum awdl_ranking ranking);

/* Where the node is in the AW schedule: the AW count at a slot boundary,
 * and when that boundary was (AWDL-7). */
struct awdl_clock {
    awdl_time last;
    uint16_t aw_counter;
};

void awdl_clock_init(struct awdl_clock *c, awdl_time now);
uint64_t awdl_clock_next_slot_us(const struct awdl_clock *c, awdl_time now);   /* 1..=SLOT_US */
uint16_t awdl_clock_next_slot_tu(const struct awdl_clock *c, awdl_time now);   /* 1..=64 */
uint16_t awdl_clock_current_aw(const struct awdl_clock *c, awdl_time now);
uint16_t awdl_clock_current_eaw(const struct awdl_clock *c, awdl_time now);
unsigned awdl_clock_slot(const struct awdl_clock *c, awdl_time now);
int64_t awdl_clock_error_tu(const struct awdl_clock *c, awdl_time now, uint16_t next_tu,
                            uint16_t aw_counter);
void awdl_clock_update(struct awdl_clock *c, awdl_time now, uint16_t next_tu, uint16_t aw_counter);
uint64_t awdl_clock_edge_wait(const struct awdl_clock *c, awdl_time now, uint64_t guard);

/* An AWDL peer, as heard. */
struct awdl_peer {
    bool used;
    bool valid;         /* sent a MIF and its version and device class, AWDL-17 */
    bool mif;
    bool v2;            /* sends Election Parameters v2, which then wins over v1 */
    bool has_rssi, has_sequence, has_ht, has_sync_offset;
    awdl_mac mac;
    uint8_t name_len;
    uint8_t name[AWDL_NAME_MAX];
    uint8_t version, devclass;
    int8_t rssi;
    awdl_time last_seen;        /* by action or data frame */
    struct awdl_election election;
    struct awdl_sequence sequence;
    int64_t sync_offset_us;     /* how far its schedule is ahead of ours */
    struct awdl_ht ht;
};

/* How the radio is present on AWDL: always on one channel. */
enum awdl_presence {
    /* Shares its channel with an access point: advertises the master's
     * slots on it and no others (AWDL-12), ranks masters by counter first. */
    AWDL_PRESENCE_SHARE,
    /* Has the channel to itself: advertises it in every slot (AWDL-12),
     * sends multicast in any slot, ranks masters by metric first (AWDL-9). */
    AWDL_PRESENCE_FIXED,
};

struct awdl_config {
    awdl_mac addr;              /* the AWDL address, AWDL-2 */
    char name[AWDL_NAME_MAX + 1];
    size_t name_len;
    uint8_t channel;
    enum awdl_presence presence;
    struct awdl_rate rate;
    bool no_ack;                /* unicast once, without waiting for an ACK */
    bool accept_addba;          /* AWDL-15 */
    bool neighbour_echo;        /* OWLY-2 */
    bool has_rssi_threshold;
    int8_t rssi_threshold;
    size_t max_peers;
};

/* owly's defaults for a radio: no_ack on a fixed one, a -65 dBm threshold,
 * neighbour echoes on, no Block Ack. */
void awdl_config_init(struct awdl_config *c, const awdl_mac *addr, const char *name,
                      uint8_t channel, enum awdl_presence presence);

struct awdl_tx {
    const uint8_t *bytes;       /* from the MAC header on, no FCS */
    size_t len;
    awdl_mac to;
    struct awdl_rate rate;
    bool no_ack;
    bool action;                /* a PSF, MIF or Block Ack answer */
};

enum awdl_tx_result { AWDL_TX_SENT, AWDL_TX_FULL, AWDL_TX_DROPPED };

/* What the node asks of its host, synchronously and in order. */
struct awdl_host {
    void *ctx;
    enum awdl_tx_result (*transmit)(void *ctx, const struct awdl_tx *tx);
    /* A peer turned valid: give it a neighbour entry (NET-2) before the
     * packets that follow. */
    void (*peer_up)(void *ctx, const struct awdl_peer *p);
    /* A valid peer went quiet or made room for another: called just
     * before it is forgotten. */
    void (*peer_down)(void *ctx, const struct awdl_peer *p);
    /* A packet for the IP stack. */
    void (*deliver)(void *ctx, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len);
};

struct awdl_stats {
    uint64_t rx_action, rx_data, rx_malformed, tx_action, tx_unicast, tx_multicast, delivered,
        dropped_no_peer, dropped_queue_full, dropped_expired, dropped_too_big, syn_held,
        syn_released, no_overlap, peers_evicted, radio_full, radio_gave_up;
};

struct awdl_packet {
    awdl_time at;               /* when it was queued */
    awdl_time tried;            /* when the radio first refused it, 0 if never */
    awdl_mac dst, src;
    uint16_t len;
    uint8_t *data;
};

/* A FIFO of packets in fixed slots; the order is kept as slot numbers, so
 * packets never move. */
struct awdl_queue {
    struct awdl_packet *slot;
    uint16_t *ring, *spare;
    uint16_t cap, head, n, nspare;
    size_t data_max;
};

struct awdl_node {
    struct awdl_config cfg;
    struct awdl_host host;
    struct awdl_clock clock;
    struct awdl_election election;
    struct awdl_sequence sequence;      /* the one it advertises */
    struct awdl_peer peers[AWDL_MAX_PEERS];
    size_t npeers;
    struct awdl_queue unicast, multicast, held;
    uint16_t seq, awdl_seq, echo_seq;
    awdl_time next_psf, next_mif, next_tick, next_echo;
    awdl_time paused_until;             /* after a burst or a full radio, 0 if not */
    bool has_no_overlap;                /* a shared radio's master has no slot on its channel */
    awdl_mac no_overlap_master;
    uint8_t no_overlap_channel;
    struct awdl_stats stats;
    uint8_t tx[AWDL_FRAME_MAX];
    /* the queues' storage */
    struct awdl_packet uslot[AWDL_UNICAST_QUEUE], mslot[AWDL_MULTICAST_QUEUE], hslot[AWDL_HELD_SYNS];
    uint16_t uring[AWDL_UNICAST_QUEUE], uspare[AWDL_UNICAST_QUEUE];
    uint16_t mring[AWDL_MULTICAST_QUEUE], mspare[AWDL_MULTICAST_QUEUE];
    uint16_t hring[AWDL_HELD_SYNS], hspare[AWDL_HELD_SYNS];
    uint8_t udata[AWDL_UNICAST_QUEUE][AWDL_PACKET_MAX];
    uint8_t mdata[AWDL_MULTICAST_QUEUE][AWDL_PACKET_MAX];
    uint8_t hdata[AWDL_HELD_SYNS][AWDL_SYN_MAX];
};

void awdl_node_init(struct awdl_node *n, const struct awdl_config *cfg, const struct awdl_host *host,
                    awdl_time now);
/* Updates the ARPA TLV hostname advertised in subsequent MIF frames. */
void awdl_node_set_name(struct awdl_node *n, const char *name);
/* A frame the radio heard at `now`, from its MAC header on, without FCS. */
void awdl_node_frame(struct awdl_node *n, awdl_time now, bool has_rssi, int8_t rssi,
                     const uint8_t *bytes, size_t len);
/* An IPv6 packet the IP stack wants sent, with its Ethernet addresses. */
void awdl_node_outbound(struct awdl_node *n, awdl_time now, const awdl_mac *dst, const awdl_mac *src,
                        const uint8_t *ip, size_t len);
/* The time came, or just the time now. */
void awdl_node_timer(struct awdl_node *n, awdl_time now);
/* When to be called back. */
awdl_time awdl_node_deadline(const struct awdl_node *n, awdl_time now);
/* Whether to read the host's next packet: not while as many as the node
 * queues already wait, so the host's own queue holds them meanwhile. */
bool awdl_node_takes_outbound(const struct awdl_node *n);
const struct awdl_peer *awdl_node_peer(const struct awdl_node *n, const awdl_mac *mac);
/* AWDL-16: how long until unicast to `peer`, or multicast, may go. */
uint64_t awdl_node_unicast_wait(const struct awdl_node *n, const struct awdl_peer *peer, awdl_time now);
uint64_t awdl_node_multicast_wait(const struct awdl_node *n, awdl_time now);

/* ------------------------------------------------------------ the card (radio.c) */
enum awdl_mode { AWDL_MODE_IBSS, AWDL_MODE_ACTIVE_MONITOR, AWDL_MODE_MONITOR };

/* What a session set up. */
struct awdl_radio {
    enum awdl_mode mode;
    uint8_t channel;
    awdl_mac addr;
    int monitor_ifindex;
    bool accept_addba;          /* owly's rx_ampdu for this driver */
};

/* Takes the journal's lock for the life of the process, then replays and
 * removes any journal a previous run left. 0, or -1 with `err`. */
int awdl_radio_recover(char *err, size_t errlen);
/* Sets the card up for AWDL on `channel` (0 picks owly's order), with
 * `addr` as the AWDL address, recording every change in the journal
 * first. On failure it undoes what it did. 0, or -1 with `err`. */
int awdl_radio_up(const char *ifname, uint8_t channel, const awdl_mac *addr, struct awdl_radio *r,
                  char *err, size_t errlen);
/* Puts the card back as the journal says, and removes the journal. */
int awdl_radio_down(char *err, size_t errlen);
/* The TAP `name` on `addr`, MTU AWDL_TAP_MTU, its EUI-64 link-local
 * address without DAD, no other address, up. Returns its descriptor, or
 * -1 with `err`; it goes away with the descriptor. */
int awdl_tap_open(const char *name, const awdl_mac *addr, int *ifindex, char *err, size_t errlen);
/* After setup: NetworkManager told to leave the TAP alone, and its MAC
 * given back the AWDL address if something rewrote it (AWDL-2). */
void awdl_radio_settle_tap(const char *name, int ifindex, const awdl_mac *addr);
/* A raw packet socket on the monitor, filtered to AWDL's BSSID. */
int awdl_monitor_open(int ifindex, char *err, size_t errlen);
/* NET-2: a permanent neighbour entry for a peer on the TAP, or none. */
int awdl_neigh_add(int ifindex, const awdl_mac *mac);
int awdl_neigh_del(int ifindex, const awdl_mac *mac);
/* The host name, for the ARPA TLV, into out[AWDL_NAME_MAX + 1]. */
void awdl_host_name(char *out);

/* Defined by main.c: sleeps up to `ms`, false when asked to stop. */
bool awdl_wait_ms(unsigned ms);
/* Human diagnostics on stderr. */
#if defined(__GNUC__)
void awdl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void awdl_log(const char *fmt, ...);   /* MSVC, for the Windows port (issue #109) */
#endif
