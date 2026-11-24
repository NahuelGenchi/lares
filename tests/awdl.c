// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for lares-awdl's frames and node (src/awdl/frame.c and
 * node.c), on plain data: an iPhone 17's PSF, MIF and data frame as heard
 * on channel 6, our own frames built and read back, and the node driven
 * with explicit time, after owly's tests of the same rules. Run from the
 * repository's root.
 *
 * The iPhone's frames are anonymised: every MAC address is a documentation
 * address (RFC 7042), its host name is "iPhone", its service records and
 * mDNS payload are zeroed and its country is "XXX", with lengths and FCS
 * fixed to match. */
#include <glib.h>
#include <string.h>
#include "../src/awdl/awdl.h"

/* ---------------------------------------------------------- data */

static const char PSF_HEX[] =
    "00001a002f480000002c0e000000000010188509c000e9000000d0000000ffffffffffff02005e005301002500ff9473"
    "30da7f0017f208100000fcafb5c641abb5c6044900061500060010006e0000101000100005000303030302005e005301"
    "0400ea1400000f010003ffff1d971d971d9700000000000000001d972b061d971d971d970000000000001d9720e41229"
    "000f030003ffff9580958095800000000000000000958006519580958095800000000000009580000000051500000000"
    "000002005e00530104020000040200000000060d000000000700301000200120020118280002005e00530102005e0053"
    "01720000000000000004020000040200000000000000000000720000000c2f00239f5858580700000000000000950002"
    "005e0053010400000000007d19000072000000274c010040140000b901000007140000006f881bffff00000000000000"
    "009600010000110e00bf0c76f0b113faff0000faff0020150200a202210e000100000095800100000000000000da222b"
    "13";

static const char MIF_HEX[] =
    "00001a002f4800009c141c000000000010188509c000e8000000d0000000ffffffffffff02005e005301002500ff9473"
    "d0ee7f0017f2081003009c98c3c6a890c3c6044900061c00060010006e000010100010000c000303030302005e005301"
    "0400221500000f010003ffff1d971d971d9700000000000000001d972b061d971d971d970000000000001d9700241229"
    "000f030003ffff9580958095800000000000000000958006519580958095800000000000009580000000051500000000"
    "000002005e0053010402000004020000000018280002005e00530102005e005301730000000000000004020000040200"
    "000000000000000000730000000c2f00239f5858580700000000000000950002005e0053010400000000007d19000073"
    "0000007158010000150000ba01000007140000006f881bffff00000000000000009600010000110e00bf0c76f0b113fa"
    "ff0000faff0020210e000100000095800100000000000000100a0003066950686f6e65c00c150200a202060d00000000"
    "070030100020012002010238000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000022c00000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000002630000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000002570000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000028d0000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000002970000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000002b78f836";

static const char DATA_HEX[] =
    "00001a002f4800005f81670100000000100c8509c000bb000000080000003333000000fb02005e005302002500ff9473"
    "00c1aaaa030017f208000304790e000086dd600c0e0001a511fffe8000000000000000005efffe005302ff0200000000"
    "000000000000000000fb14e914e901a530df000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000f7"
    "efb5d5";

/* Documentation addresses (RFC 7042): the iPhone, a second node, and us. */
static const awdl_mac PHONE = { { 0x02, 0x00, 0x5e, 0x00, 0x53, 0x01 } };
static const awdl_mac OTHER = { { 0x02, 0x00, 0x5e, 0x00, 0x53, 0x02 } };
static const awdl_mac US = { { 0x02, 0x00, 0x5e, 0x00, 0x53, 0x10 } };
static const awdl_mac MDNS_GROUP = { { 0x33, 0x33, 0x00, 0x00, 0x00, 0xfb } };
#define START 10000000ULL
#define TU AWDL_TU_US

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2;
    g_assert_cmpuint(n, <=, cap);
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(g_ascii_xdigit_value(hex[2 * i]) << 4 | g_ascii_xdigit_value(hex[2 * i + 1]));
    return n;
}

/* A captured frame, radiotap and FCS stripped. */
static struct awdl_rx captured(const char *hex, uint8_t *buf, size_t cap)
{
    size_t n = unhex(hex, buf, cap);
    struct awdl_rx rx;
    g_assert_cmpint(awdl_radiotap_parse(buf, n, &rx), ==, 0);
    return rx;
}

static void seq_from(struct awdl_sequence *s, const uint8_t channels[AWDL_SLOTS])
{
    awdl_seq_empty(s, AWDL_ENC_OPCLASS);
    for (unsigned i = 0; i < AWDL_SLOTS; i++)
        if (channels[i]) {
            s->slots[i][0] = channels[i];
            s->slots[i][1] = awdl_channel_opclass(channels[i]);
        }
}

static void assert_channels(const struct awdl_sequence *s, const uint8_t want[AWDL_SLOTS])
{
    for (unsigned i = 0; i < AWDL_SLOTS; i++)
        g_assert_cmpuint(awdl_seq_channel(s, i), ==, want[i]);
}

/* The iPhone's sequence on the day: 149, off, channel 6 in slot 8. */
static const uint8_t IPHONE_SEQ[AWDL_SLOTS] = { 149, 149, 149, 0, 0, 0, 0, 149, 6, 149, 149, 149, 0, 0, 0, 149 };
/* owly's test sequence. */
static const uint8_t PHONE_SEQ[AWDL_SLOTS] = { 149, 149, 149, 0, 0, 0, 0, 0, 6, 149, 149, 0, 0, 0, 0, 0 };

/* ---------------------------------------------------------- the iPhone's frames */

/* spec: AWDL-3, AWDL-4, AWDL-6, T-AWDL-1 */
static void test_iphone_psf(void)
{
    uint8_t buf[512];
    struct awdl_rx rx = captured(PSF_HEX, buf, sizeof buf);
    g_assert_true(rx.has_rssi);
    g_assert_cmpint(rx.rssi, ==, -23);
    g_assert_cmpuint(rx.freq, ==, 2437);
    g_assert_cmpuint(rx.len, ==, strlen(PSF_HEX) / 2 - 26 - 4);

    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(rx.frame, rx.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_ACTION);
    g_assert_cmpuint(f.subtype, ==, AWDL_SUBTYPE_PSF);
    g_assert_true(awdl_mac_eq(&f.h.src, &PHONE));
    g_assert_true(awdl_mac_eq(&f.h.dst, &AWDL_BROADCAST));
    g_assert_true(awdl_mac_eq(&f.h.bssid, &AWDL_BSSID));

    struct awdl_advert a;
    g_assert_cmpint(awdl_advert_parse(f.tlvs, f.tlvs_len, &a), ==, 0);
    g_assert_true(a.has_sync);
    g_assert_cmpuint(a.sync.next_aw_channel, ==, 6);
    g_assert_cmpuint(a.sync.next_tu, ==, 21);
    g_assert_cmpuint(a.sync.master_channel, ==, 6);
    g_assert_cmpuint(a.sync.aw_period, ==, 16);
    g_assert_cmpuint(a.sync.af_period, ==, 110);
    g_assert_cmpuint(a.sync.presence_mode, ==, 4);
    g_assert_cmpuint(a.sync.aw_counter, ==, 0x14ea);
    g_assert_true(awdl_mac_eq(&a.sync.master, &PHONE));

    /* Version 2 wins over version 1, which came first. */
    g_assert_true(a.has_election && a.election_v2);
    g_assert_true(awdl_mac_eq(&a.election.master, &PHONE));
    g_assert_true(a.election.has_sync && awdl_mac_eq(&a.election.sync, &PHONE));
    g_assert_cmpuint(a.election.master_metric, ==, 516);
    g_assert_cmpuint(a.election.self_metric, ==, 516);
    g_assert_cmpuint(a.election.master_counter, ==, 114);
    g_assert_cmpuint(a.election.self_counter, ==, 114);
    g_assert_cmpuint(a.election.height, ==, 0);

    /* AWDL-10: opclass, channel 6 in slot 8 only, 149 elsewhere. */
    g_assert_true(a.has_sequence);
    g_assert_cmpuint(a.sequence.encoding, ==, AWDL_ENC_OPCLASS);
    assert_channels(&a.sequence, IPHONE_SEQ);
    g_assert_cmpuint(a.sequence.slots[8][1], ==, 0x51);
    g_assert_cmpuint(a.sequence.slots[0][1], ==, 0x80);

    g_assert_true(a.has_version);
    g_assert_cmpuint(a.devclass, ==, AWDL_DEVCLASS_IOS);
    g_assert_cmpuint(a.version, ==, 0xa2);
    g_assert_true(a.has_ht);
    g_assert_cmpuint(a.ht.capabilities, ==, 0x886f);
    g_assert_cmpuint(a.ht.ampdu_params, ==, 0x1b);
    g_assert_cmpuint(a.ht.rx_mcs, ==, 0xffff);
    g_assert_true(a.has_data_path && a.data_path.has_country);
    g_assert_cmpmem(a.data_path.country, 3, "XXX", 3);
    g_assert_false(a.has_name); /* PSFs carry no name */

    /* Their order: 4, 18, 5, 6, 24, 12, 7, then 17, 21 and 33 unknown to us. */
    static const uint8_t order[] = { 4, 18, 5, 6, 24, 12, 7, 17, 21, 33 };
    struct awdl_tlv_iter it = { f.tlvs, f.tlvs_len };
    struct awdl_tlv t;
    size_t i = 0;
    while (awdl_tlv_next(&it, &t) > 0) {
        g_assert_cmpuint(i, <, sizeof order);
        g_assert_cmpuint(t.type, ==, order[i++]);
    }
    g_assert_cmpuint(i, ==, sizeof order);
}

/* spec: AWDL-6, AWDL-17 */
static void test_iphone_mif(void)
{
    uint8_t buf[1024];
    struct awdl_rx rx = captured(MIF_HEX, buf, sizeof buf);
    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(rx.frame, rx.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_ACTION);
    g_assert_cmpuint(f.subtype, ==, AWDL_SUBTYPE_MIF);
    struct awdl_advert a;
    g_assert_cmpint(awdl_advert_parse(f.tlvs, f.tlvs_len, &a), ==, 0);
    g_assert_true(a.has_name);
    g_assert_cmpmem(a.name, a.name_len, "iPhone", 6);
    g_assert_cmpuint(a.devclass, ==, AWDL_DEVCLASS_IOS);
    g_assert_cmpuint(a.election.master_counter, ==, 115);
    assert_channels(&a.sequence, IPHONE_SEQ);
    /* The six service responses (type 2) are skipped by their length. */
    struct awdl_tlv_iter it = { f.tlvs, f.tlvs_len };
    struct awdl_tlv t;
    int responses = 0;
    while (awdl_tlv_next(&it, &t) > 0)
        responses += t.type == AWDL_TLV_SERVICE_RESPONSE;
    g_assert_cmpint(responses, ==, 6);
}

/* spec: AWDL-13, NET-3 */
static void test_iphone_data(void)
{
    uint8_t buf[1024];
    struct awdl_rx rx = captured(DATA_HEX, buf, sizeof buf);
    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(rx.frame, rx.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_DATA);
    g_assert_false(f.amsdu);
    struct awdl_msdu m[AWDL_AMSDU_MAX];
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, 1);
    g_assert_true(awdl_mac_eq(&m[0].dst, &MDNS_GROUP));
    g_assert_true(awdl_mac_eq(&m[0].src, &OTHER));
    g_assert_cmpuint(m[0].ethertype, ==, AWDL_ETHERTYPE_IPV6);
    g_assert_cmpuint(m[0].payload[0] >> 4, ==, 6);
    g_assert_cmpuint(m[0].payload[6], ==, 17); /* UDP, mDNS */
    g_assert_cmpuint(m[0].len, ==, 40u + (m[0].payload[4] << 8 | m[0].payload[5]));
    /* Its source address is the modified EUI-64 of its MAC (NET-1). */
    awdl_mac from;
    g_assert_true(awdl_mac_from_link_local(m[0].payload + 8, &from));
    g_assert_true(awdl_mac_eq(&from, &OTHER));
}

/* ---------------------------------------------------------- our frames */

static void own_tlvs(struct awdl_wbuf *w, uint8_t *buf, size_t cap, bool mif, struct awdl_sequence *seq,
                     struct awdl_election *e)
{
    awdl_mac us = { { 2, 0, 0, 0, 0, 1 } }, master = { { 2, 0, 0, 0, 0, 9 } };
    awdl_seq_fixed(seq, 44);
    awdl_election_init(e, &us);
    e->master = master;
    e->sync = master;
    e->height = 1;
    e->master_metric = 522;
    e->master_counter = 393;
    struct awdl_own own = {
        .addr = us, .name = "owly", .name_len = 4, .devclass = AWDL_DEVCLASS_MACOS, .channel = 44,
        .master_channel = 44, .next_tu = 20, .aw_counter = 401, .psf_interval_tu = 110, .sequence = seq,
        .election = e, .has_ht = true, .ht = { AWDL_HT_CAPABILITIES, AWDL_HT_AMPDU_PARAMS, AWDL_HT_RX_MCS },
    };
    awdl_w_init(w, buf, cap);
    awdl_put_tlvs(w, &own, mif);
    g_assert_false(w->overflow);
}

/* spec: AWDL-4, AWDL-6 */
static void test_own_tlvs_round_trip(void)
{
    uint8_t buf[1024];
    struct awdl_wbuf w;
    struct awdl_sequence seq;
    struct awdl_election e;
    own_tlvs(&w, buf, sizeof buf, true, &seq, &e);
    /* owly's order and lengths: sync 33 + sequence 38 + 2, election 21,
     * sequence 38 + 3, election v2 40, service 9, HT 8, ARPA 4 + name,
     * data path 15, version 2. */
    static const uint8_t types[] = { 4, 5, 18, 24, 6, 7, 16, 12, 21 };
    static const size_t lens[] = { 73, 21, 41, 40, 9, 8, 8, 15, 2 };
    struct awdl_tlv_iter it = { buf, w.len };
    struct awdl_tlv t;
    size_t i = 0;
    while (awdl_tlv_next(&it, &t) > 0) {
        g_assert_cmpuint(t.type, ==, types[i]);
        g_assert_cmpuint(t.len, ==, lens[i]);
        i++;
    }
    g_assert_cmpuint(i, ==, 9);

    struct awdl_advert a;
    g_assert_cmpint(awdl_advert_parse(buf, w.len, &a), ==, 0);
    g_assert_cmpuint(a.sync.next_tu, ==, 20);
    g_assert_cmpuint(a.sync.aw_counter, ==, 401);
    g_assert_cmpuint(a.sync.master_channel, ==, 44);
    g_assert_true(awdl_mac_eq(&a.sync.master, &e.master));
    g_assert_true(a.election_v2);
    g_assert_true(awdl_mac_eq(&a.election.sync, &e.master));
    g_assert_cmpuint(a.election.height, ==, 1);
    g_assert_cmpuint(a.election.master_metric, ==, 522);
    g_assert_cmpuint(a.election.master_counter, ==, 393);
    g_assert_cmpmem(a.sequence.slots, sizeof a.sequence.slots, seq.slots, sizeof seq.slots);
    g_assert_cmpmem(a.name, a.name_len, "owly", 4);
    g_assert_cmpuint(a.version, ==, AWDL_VERSION);
    g_assert_cmpuint(a.devclass, ==, AWDL_DEVCLASS_MACOS);
    g_assert_cmpuint(a.ht.capabilities, ==, AWDL_HT_CAPABILITIES);
    g_assert_cmpuint(a.ht.rx_mcs, ==, 0xff);
    g_assert_cmpmem(a.data_path.country, 3, "X0\0", 3);

    /* A PSF leaves out only the name. */
    own_tlvs(&w, buf, sizeof buf, false, &seq, &e);
    g_assert_cmpint(awdl_advert_parse(buf, w.len, &a), ==, 0);
    g_assert_false(a.has_name);
    g_assert_true(a.has_ht);
}

/* spec: AWDL-3 */
static void test_action_frame_round_trip(void)
{
    struct awdl_sequence seq;
    struct awdl_election e;
    awdl_seq_fixed(&seq, 6);
    awdl_election_init(&e, &US);
    struct awdl_own own = {
        .addr = US, .name = "lares", .name_len = 5, .devclass = AWDL_DEVCLASS_MACOS, .channel = 6,
        .master_channel = 6, .next_tu = 64, .psf_interval_tu = 110, .sequence = &seq, .election = &e,
    };
    uint8_t buf[1024];
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, sizeof buf);
    awdl_action_frame(&w, &US, 5, AWDL_SUBTYPE_MIF, 0xdeadbeef, &own);
    static const uint8_t header[] = { 127, 0x00, 0x17, 0xf2, 8, 0x10, 3, 0 };
    g_assert_cmpmem(buf + AWDL_HEADER_LEN, 8, header, 8);
    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_ACTION);
    g_assert_cmpuint(f.subtype, ==, AWDL_SUBTYPE_MIF);
    g_assert_cmpuint(f.phy_tx, ==, 0xdeadbeef);
    g_assert_cmpuint(f.target_tx, ==, 0xdeadbeef);
    g_assert_cmpuint(f.h.seq, ==, 5);
    g_assert_true(awdl_mac_eq(&f.h.src, &US));
    struct awdl_advert a;
    g_assert_cmpint(awdl_advert_parse(f.tlvs, f.tlvs_len, &a), ==, 0);
    g_assert_cmpmem(a.name, a.name_len, "lares", 5);
    /* AWDL-12: a fixed radio's channel in every slot. */
    static const uint8_t six[AWDL_SLOTS] = { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 };
    assert_channels(&a.sequence, six);

    /* Another vendor's action frame, or another AWDL subtype, isn't ours. */
    buf[AWDL_HEADER_LEN + 3] = 0xf3;
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_OTHER);
    buf[AWDL_HEADER_LEN + 3] = 0xf2;
    buf[AWDL_HEADER_LEN + 6] = 1;
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_OTHER);
}

/* spec: AWDL-5 */
static void test_padding(void)
{
    static const uint8_t version[] = { AWDL_TLV_VERSION, 2, 0, AWDL_VERSION, AWDL_DEVCLASS_IOS };
    uint8_t body[32];
    struct awdl_advert a;
    for (size_t pad = 0; pad <= 8; pad++) {
        if (pad > 3 && pad != 8)
            continue;
        memcpy(body, version, sizeof version);
        memset(body + sizeof version, 0, pad);
        g_assert_cmpint(awdl_advert_parse(body, sizeof version + pad, &a), ==, 0);
        g_assert_cmpuint(a.devclass, ==, AWDL_DEVCLASS_IOS);
    }
    /* A nonzero tail, half a header, or a length past the end. */
    memcpy(body, version, sizeof version);
    body[5] = 0;
    body[6] = 1;
    g_assert_cmpint(awdl_advert_parse(body, 7, &a), ==, -1);
    body[5] = AWDL_TLV_VERSION;
    body[6] = 2;
    g_assert_cmpint(awdl_advert_parse(body, 7, &a), ==, -1);
    body[5] = AWDL_TLV_VERSION;
    body[6] = 9;
    body[7] = 0;
    body[8] = 1;
    g_assert_cmpint(awdl_advert_parse(body, 9, &a), ==, -1);
}

/* spec: AWDL-6 */
static void test_unknown_tlvs(void)
{
    static const uint8_t body[] = { 99, 4, 0, 1, 2, 3, 4, AWDL_TLV_BLOOM_FILTER, 1, 0, 7,
                                    AWDL_TLV_VERSION, 2, 0, AWDL_VERSION, AWDL_DEVCLASS_IOS };
    struct awdl_advert a;
    g_assert_cmpint(awdl_advert_parse(body, sizeof body, &a), ==, 0);
    g_assert_true(a.has_version);
    g_assert_cmpuint(a.devclass, ==, AWDL_DEVCLASS_IOS);
    /* A known TLV too short for its layout is refused. */
    static const uint8_t short_sync[] = { AWDL_TLV_SYNC_PARAMS, 3, 0, 6, 1, 0 };
    g_assert_cmpint(awdl_advert_parse(short_sync, sizeof short_sync, &a), ==, -1);
}

/* Names are bounded, and cut on a character boundary. */
static void test_names(void)
{
    char long_name[101];
    memset(long_name, 'x', 100);
    long_name[100] = 0;
    uint8_t buf[256], name[AWDL_NAME_MAX], len;
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, sizeof buf);
    awdl_put_arpa(&w, long_name, 100);
    g_assert_cmpint(awdl_arpa_parse(buf + 3, w.len - 3, name, &len), ==, 0);
    g_assert_cmpuint(len, ==, AWDL_NAME_MAX);
    /* 63 bytes of ASCII, then a 2-byte character across the limit. */
    char utf8[70];
    memset(utf8, 'a', 63);
    memcpy(utf8 + 63, "\xc3\xa9z", 4);
    awdl_w_init(&w, buf, sizeof buf);
    awdl_put_arpa(&w, utf8, strlen(utf8));
    g_assert_cmpint(awdl_arpa_parse(buf + 3, w.len - 3, name, &len), ==, 0);
    g_assert_cmpuint(len, ==, 63);
    static const uint8_t truncated[] = { 3, 10, 'a' };
    g_assert_cmpint(awdl_arpa_parse(truncated, sizeof truncated, name, &len), ==, -1);
}

/* spec: AWDL-10 */
static void test_sequence_encodings(void)
{
    struct awdl_sequence s, back;
    seq_from(&s, PHONE_SEQ);
    uint8_t buf[64];
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, sizeof buf);
    awdl_seq_put(&w, &s);
    g_assert_cmpuint(w.len, ==, awdl_seq_wire_len(&s));
    g_assert_cmpint(awdl_seq_parse(buf, w.len, &back), ==, 0);
    g_assert_cmpmem(&back, sizeof back, &s, sizeof s);

    /* Legacy: flags, then the channel. */
    uint8_t legacy[6 + 32] = { 15, 1, 0, 3, 0xff, 0xff };
    for (int i = 0; i < 16; i++) {
        legacy[6 + 2 * i] = 0x01;
        legacy[7 + 2 * i] = i < 8 ? 44 : 6;
    }
    g_assert_cmpint(awdl_seq_parse(legacy, sizeof legacy, &back), ==, 0);
    g_assert_cmpuint(awdl_seq_channel(&back, 0), ==, 44);
    g_assert_cmpuint(awdl_seq_channel(&back, 15), ==, 6);

    /* Simple: one byte a slot, written back as it came. */
    uint8_t simple[6 + 16] = { 15, 0, 0, 3, 0xff, 0xff };
    memset(simple + 6, 6, 16);
    g_assert_cmpint(awdl_seq_parse(simple, sizeof simple, &back), ==, 0);
    awdl_w_init(&w, buf, sizeof buf);
    awdl_seq_put(&w, &back);
    g_assert_cmpmem(buf, w.len, simple, sizeof simple);

    /* Only owly's shape parses. */
    uint8_t good[64];
    awdl_seq_fixed(&s, 6);
    awdl_w_init(&w, good, sizeof good);
    awdl_seq_put(&w, &s);
    static const uint8_t at[] = { 0, 1, 2, 3, 4 }, bad[] = { 7, 2, 1, 0, 0 };
    for (size_t i = 0; i < sizeof at; i++) {
        memcpy(buf, good, w.len);
        buf[at[i]] = bad[i];
        g_assert_cmpint(awdl_seq_parse(buf, w.len, &back), ==, -1);
    }
    g_assert_cmpint(awdl_seq_parse(good, w.len - 1, &back), ==, -1);
}

/* spec: AWDL-12 */
static void test_sequence_intersect(void)
{
    static const uint8_t phone[AWDL_SLOTS] = { 36, 36, 149, 0, 0, 0, 0, 36, 6, 36, 149, 36, 0, 0, 0, 36 };
    static const uint8_t want[AWDL_SLOTS] = { 0, 0, 149, 0, 0, 0, 0, 0, 0, 0, 149, 0, 0, 0, 0, 0 };
    struct awdl_sequence s, ours;
    seq_from(&s, phone);
    g_assert_cmpuint(awdl_seq_intersect(&s, 149, &ours), ==, 2);
    assert_channels(&ours, want);
    g_assert_cmpuint(awdl_seq_intersect(&s, 44, &ours), ==, 0);
    /* The dominant channel; the preferred one breaks exact ties only. */
    g_assert_cmpuint(awdl_seq_dominant(&s, 6), ==, 36);
    static const uint8_t tie[AWDL_SLOTS] = { 6, 149 };
    seq_from(&s, tie);
    g_assert_cmpuint(awdl_seq_dominant(&s, 149), ==, 149);
    awdl_seq_empty(&s, AWDL_ENC_OPCLASS);
    g_assert_cmpuint(awdl_seq_dominant(&s, 6), ==, 0);
}

static void test_channels(void)
{
    g_assert_cmpuint(awdl_channel_freq(6), ==, 2437);
    g_assert_cmpuint(awdl_channel_freq(44), ==, 5220);
    g_assert_cmpuint(awdl_channel_freq(149), ==, 5745);
    g_assert_cmpuint(awdl_channel_freq(0), ==, 0);
    g_assert_cmpuint(awdl_channel_from_freq(5745), ==, 149);
    g_assert_cmpuint(awdl_channel_from_freq(2437), ==, 6);
    g_assert_cmpuint(awdl_channel_from_freq(2436), ==, 0);
    g_assert_true(awdl_channel_social(44));
    g_assert_false(awdl_channel_social(1));
}

/* ---------------------------------------------------------- election and schedule */

static awdl_mac mac_n(uint8_t n)
{
    awdl_mac m = { { 2, 0, 0, 0, 0, n } };
    return m;
}

static struct awdl_election peer_el(uint8_t n, uint32_t metric, uint32_t counter)
{
    awdl_mac m = mac_n(n);
    struct awdl_election e;
    awdl_election_init(&e, &m);
    e.self_metric = e.master_metric = metric;
    e.self_counter = e.master_counter = counter;
    return e;
}

/* spec: AWDL-7, AWDL-8, AWDL-9 */
static void test_election(void)
{
    awdl_mac one = mac_n(1), two = mac_n(2);
    struct awdl_election low, high;
    awdl_election_init(&low, &one);
    g_assert_true(awdl_election_is_master(&low));
    g_assert_cmpuint(low.self_metric, ==, 60);
    g_assert_cmpuint(low.self_counter, ==, 0);

    /* The higher address wins a tie. */
    awdl_election_init(&high, &two);
    const struct awdl_election *p[2] = { &high };
    g_assert_true(awdl_election_run(&low, p, 1, AWDL_RANK_COUNTER_FIRST));
    g_assert_true(awdl_mac_eq(&low.master, &two) && awdl_mac_eq(&low.sync, &two));
    g_assert_cmpuint(low.height, ==, 1);
    p[0] = &low;
    g_assert_false(awdl_election_run(&high, p, 1, AWDL_RANK_COUNTER_FIRST));
    g_assert_true(awdl_election_is_master(&high));

    /* Apple ranks by metric first: 522/393 beats 510/3214. */
    struct awdl_election old = peer_el(3, 510, 3214), new = peer_el(4, 522, 393), us;
    awdl_election_init(&us, &one);
    p[0] = &old;
    p[1] = &new;
    awdl_election_run(&us, p, 2, AWDL_RANK_METRIC_FIRST);
    g_assert_cmpuint(us.master.a[5], ==, 4);
    awdl_election_run(&us, p, 2, AWDL_RANK_COUNTER_FIRST);
    g_assert_cmpuint(us.master.a[5], ==, 3);

    /* Shorter paths, and no cycles. */
    struct awdl_election master = peer_el(9, 600, 10), relay;
    awdl_mac five = mac_n(5);
    awdl_election_init(&relay, &five);
    p[0] = &master;
    awdl_election_run(&relay, p, 1, AWDL_RANK_METRIC_FIRST);
    p[0] = &relay;
    p[1] = &master;
    awdl_election_run(&us, p, 2, AWDL_RANK_METRIC_FIRST);
    g_assert_cmpuint(us.sync.a[5], ==, 9);
    g_assert_cmpuint(us.height, ==, 1);
    awdl_election_run(&us, p, 1, AWDL_RANK_METRIC_FIRST);
    g_assert_cmpuint(us.master.a[5], ==, 9);
    g_assert_cmpuint(us.sync.a[5], ==, 5);
    g_assert_cmpuint(us.height, ==, 2);
    struct awdl_election follower = peer_el(7, 900, 0);
    follower.sync = one;
    p[0] = &follower;
    awdl_election_run(&us, p, 1, AWDL_RANK_METRIC_FIRST);
    g_assert_true(awdl_election_is_master(&us));
    struct awdl_election far = peer_el(8, 900, 0);
    far.height = AWDL_MAX_HEIGHT;
    p[0] = &far;
    awdl_election_run(&us, p, 1, AWDL_RANK_METRIC_FIRST);
    g_assert_true(awdl_election_is_master(&us));
    far.height = UINT32_MAX;
    awdl_election_run(&us, p, 1, AWDL_RANK_METRIC_FIRST);
    g_assert_true(awdl_election_is_master(&us));
}

/* spec: AWDL-7 */
static void test_clock(void)
{
    struct awdl_clock c;
    awdl_clock_init(&c, 1000000);
    awdl_time t = 1000000;
    g_assert_cmpuint(awdl_clock_next_slot_us(&c, t), ==, AWDL_SLOT_US);
    g_assert_cmpuint(awdl_clock_next_slot_tu(&c, t), ==, 64);
    g_assert_cmpuint(awdl_clock_slot(&c, t), ==, 0);
    g_assert_cmpuint(awdl_clock_current_aw(&c, t + 16 * TU), ==, 1);
    g_assert_cmpuint(awdl_clock_slot(&c, t + AWDL_SLOT_US), ==, 1);
    g_assert_cmpuint(awdl_clock_slot(&c, t + 17 * AWDL_SLOT_US), ==, 1);
    g_assert_cmpuint(awdl_clock_next_slot_us(&c, t + AWDL_SLOT_US - 1), ==, 1);

    /* Following a master: its phase and count. */
    awdl_clock_init(&c, 1);
    awdl_time now = 10000000;
    g_assert_cmpint(awdl_clock_error_tu(&c, now, 20, 401), !=, 0);
    awdl_clock_update(&c, now, 20, 401);
    g_assert_cmpint(awdl_clock_error_tu(&c, now, 20, 401), ==, 0);
    g_assert_cmpuint(awdl_clock_next_slot_tu(&c, now), ==, 20);
    g_assert_cmpuint(awdl_clock_current_eaw(&c, now), ==, 100);
    g_assert_cmpuint(awdl_clock_slot(&c, now), ==, 100 % 16);
    g_assert_cmpuint(awdl_clock_slot(&c, now + 20 * TU), ==, 101 % 16);
    g_assert_cmpint(awdl_clock_error_tu(&c, now, 17, 401), ==, 3);
    g_assert_cmpint(awdl_clock_error_tu(&c, now, 20, 397), ==, -64);

    /* Slot counts compare across their wrap. */
    awdl_clock_init(&c, AWDL_SLOT_US);
    awdl_clock_update(&c, AWDL_SLOT_US, 64, 65532);
    g_assert_cmpuint(awdl_clock_current_eaw(&c, AWDL_SLOT_US), ==, 16383);
    g_assert_cmpint(awdl_clock_error_tu(&c, AWDL_SLOT_US, 64, 0), ==, 64);
    g_assert_cmpuint(awdl_clock_current_eaw(&c, 2 * AWDL_SLOT_US), ==, 0);
    g_assert_cmpuint(awdl_clock_slot(&c, 2 * AWDL_SLOT_US), ==, 0);
}

/* spec: AWDL-16 */
static void test_guards(void)
{
    struct awdl_clock c;
    awdl_time o = AWDL_SLOT_US * 10; /* a slot boundary */
    awdl_clock_init(&c, o);
    uint64_t g = AWDL_UNICAST_GUARD_US, m = AWDL_MULTICAST_GUARD_US;
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o, g), ==, g);
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o + g, g), ==, 0);
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o + AWDL_SLOT_US / 2, g), ==, 0);
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o + AWDL_SLOT_US - g, g), ==, 0);
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o + AWDL_SLOT_US - 1000, g), ==, 1000 + g);
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o + AWDL_SLOT_US / 2, m), ==, 0);
    g_assert_cmpuint(awdl_clock_edge_wait(&c, o + 1000, m), ==, m - 1000);
}

/* ---------------------------------------------------------- data frames */

/* spec: AWDL-13, T-AWDL-2 */
static void test_data_frame_golden(void)
{
    static const uint8_t packet[] = { 0x60, 0, 0, 0, 0, 0, 0x3b, 0x40 };
    uint8_t buf[128];
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, sizeof buf);
    awdl_data_frame(&w, &US, &PHONE, 0x123, 7, packet, sizeof packet);
    static const uint8_t golden[] = {
        0x08, 0x00, 0x00, 0x00,                             /* data, duration 0 */
        0x02, 0x00, 0x5e, 0x00, 0x53, 0x01,                 /* receiver */
        0x02, 0x00, 0x5e, 0x00, 0x53, 0x10,                 /* transmitter */
        0x00, 0x25, 0x00, 0xff, 0x94, 0x73,                 /* AWDL's BSSID */
        0x30, 0x12,                                         /* sequence 0x123 */
        0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00,     /* LLC/SNAP, AWDL */
        0x03, 0x04, 0x07, 0x00, 0x00, 0x00, 0x86, 0xdd,     /* head, seq, pad, IPv6 */
        0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3b, 0x40,     /* the packet */
    };
    g_assert_cmpmem(buf, w.len, golden, sizeof golden);
    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_DATA);
    g_assert_cmpuint(f.h.seq, ==, 0x123);
    struct awdl_msdu m[AWDL_AMSDU_MAX];
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, 1);
    g_assert_true(awdl_mac_eq(&m[0].src, &US) && awdl_mac_eq(&m[0].dst, &PHONE));
    g_assert_cmpmem(m[0].payload, m[0].len, packet, sizeof packet);
}

/* spec: AWDL-14 */
static void test_amsdu_bounds(void)
{
    static const uint8_t p1[] = { 0x60, 1, 2 }, p2[] = { 0x60, 3, 4, 5, 6 };
    const uint8_t *packets[AWDL_AMSDU_MAX + 1];
    size_t lens[AWDL_AMSDU_MAX + 1];
    uint8_t buf[2048];
    struct awdl_wbuf w;
    struct awdl_rx_frame f;
    struct awdl_msdu m[AWDL_AMSDU_MAX];

    packets[0] = p1;
    lens[0] = sizeof p1;
    packets[1] = p2;
    lens[1] = sizeof p2;
    awdl_w_init(&w, buf, sizeof buf);
    awdl_amsdu_frame(&w, &PHONE, &US, 1, 10, packets, lens, 2);
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_true(f.amsdu);
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, 2);
    g_assert_true(awdl_mac_eq(&m[0].src, &PHONE) && awdl_mac_eq(&m[0].dst, &US));
    g_assert_cmpmem(m[0].payload, m[0].len, p1, sizeof p1);
    g_assert_cmpmem(m[1].payload, m[1].len, p2, sizeof p2);

    /* One subframe too many is refused whole; the bound itself passes. */
    for (size_t i = 0; i <= AWDL_AMSDU_MAX; i++) {
        packets[i] = p1;
        lens[i] = sizeof p1;
    }
    awdl_w_init(&w, buf, sizeof buf);
    awdl_amsdu_frame(&w, &PHONE, &US, 1, 10, packets, lens, AWDL_AMSDU_MAX + 1);
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, -1);
    awdl_w_init(&w, buf, sizeof buf);
    awdl_amsdu_frame(&w, &PHONE, &US, 1, 10, packets, lens, AWDL_AMSDU_MAX);
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, (int)AWDL_AMSDU_MAX);
    /* A subframe longer than what's left. */
    g_assert_cmpint(awdl_frame_parse(buf, w.len - 1, &f), ==, 0);
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, -1);
    /* Not AWDL's LLC/SNAP. */
    awdl_w_init(&w, buf, sizeof buf);
    awdl_data_frame(&w, &PHONE, &US, 1, 1, p1, sizeof p1);
    buf[AWDL_HEADER_LEN + 6] = 0x86;
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(awdl_msdus(&f.h, f.amsdu, f.body, f.body_len, m), ==, -1);
}

static void test_malformed_frames(void)
{
    struct awdl_rx_frame f;
    /* An ACK (10 bytes) and an RTS (16) aren't AWDL, nor malformed. */
    static const uint8_t ack[] = { 0xd4, 0, 0, 0, 2, 0, 0x5e, 0, 0x53, 1 };
    g_assert_cmpint(awdl_frame_parse(ack, sizeof ack, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_OTHER);
    uint8_t rts[16] = { 0xb4 };
    g_assert_cmpint(awdl_frame_parse(rts, sizeof rts, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_OTHER);
    /* A management frame that short is. */
    static const uint8_t short_mgmt[] = { 0xd0, 0, 0, 0, 2, 0, 0x5e, 0, 0x53, 1 };
    g_assert_cmpint(awdl_frame_parse(short_mgmt, sizeof short_mgmt, &f), ==, -1);
    g_assert_cmpint(awdl_frame_parse(short_mgmt, 1, &f), ==, -1);
    /* An action frame with no category. */
    uint8_t buf[128];
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, sizeof buf);
    awdl_put_header(&w, AWDL_FC_ACTION, &PHONE, &US, 0);
    g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_OTHER);
    /* Data through a distribution system, off AWDL's BSSID, or protected. */
    static const uint8_t ip[] = { 0x60 };
    for (int k = 0; k < 3; k++) {
        awdl_w_init(&w, buf, sizeof buf);
        awdl_data_frame(&w, &US, &PHONE, 0, 0, ip, 1);
        if (k == 0)
            buf[1] = 0x01;
        else if (k == 1)
            buf[16] = 0x02;
        else
            buf[1] = 0x40;
        g_assert_cmpint(awdl_frame_parse(buf, w.len, &f), ==, 0);
        g_assert_cmpint(f.kind, ==, AWDL_RX_OTHER);
    }
    /* Radiotap: too short, a bad length, a failed FCS. */
    struct awdl_rx rx;
    uint8_t frame[512];
    size_t n = unhex(PSF_HEX, frame, sizeof frame);
    g_assert_cmpint(awdl_radiotap_parse(frame, 6, &rx), ==, -1);
    uint8_t copy[512];
    memcpy(copy, frame, n);
    copy[2] = 200;
    copy[3] = 3;
    g_assert_cmpint(awdl_radiotap_parse(copy, n, &rx), ==, -1);
    memcpy(copy, frame, n);
    copy[16] |= 0x40; /* bad FCS */
    g_assert_cmpint(awdl_radiotap_parse(copy, n, &rx), ==, -1);
}

/* spec: AWDL-15 */
static void test_addba_response(void)
{
    uint8_t req[64];
    struct awdl_wbuf w;
    awdl_w_init(&w, req, sizeof req);
    awdl_put_header(&w, AWDL_FC_ACTION, &US, &PHONE, 3);
    /* Dialog 9, TID 5, window 256 (capped to 64), timeout 0. */
    uint16_t params = (uint16_t)(256 << 6 | 5 << 2 | 0x0003);
    uint8_t body[] = { 3, 0, 9, (uint8_t)params, (uint8_t)(params >> 8), 0, 0, 0, 0 };
    awdl_w_bytes(&w, body, sizeof body);
    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(req, w.len, &f), ==, 0);
    g_assert_cmpint(f.kind, ==, AWDL_RX_ADDBA);
    g_assert_cmpuint(f.token, ==, 9);
    g_assert_cmpuint(f.timeout, ==, 0);
    uint8_t resp[64];
    awdl_w_init(&w, resp, sizeof resp);
    awdl_addba_response(&w, &US, &PHONE, 4, f.token, f.params, f.timeout);
    static const uint8_t head[] = { 3, 1, 9, 0, 0 };
    g_assert_cmpmem(resp + AWDL_HEADER_LEN, 5, head, 5);
    uint16_t p = (uint16_t)(resp[AWDL_HEADER_LEN + 5] | resp[AWDL_HEADER_LEN + 6] << 8);
    g_assert_cmpuint((p >> 2) & 0xf, ==, 5);
    g_assert_cmpuint(p & 3, ==, 2);
    g_assert_cmpuint(p >> 6, ==, 64);
    g_assert_cmpmem(resp + 4, 6, PHONE.a, 6);
}

static void test_radiotap_tx(void)
{
    uint8_t out[AWDL_RADIOTAP_MAX];
    struct awdl_rate basic = { .value = AWDL_RATE_BASIC };
    static const uint8_t plain[] = { 0, 0, 9, 0, 0x04, 0, 0, 0, 24 };
    static const uint8_t noack[] = { 0, 0, 12, 0, 0x04, 0x80, 0, 0, 24, 0, 0x08, 0 };
    static const uint8_t ht[] = { 0, 0, 13, 0, 0, 0x80, 0x08, 0, 0x08, 0, 0x07, 0x04, 7 };
    size_t n = awdl_radiotap_tx(out, basic, false);
    g_assert_cmpmem(out, n, plain, sizeof plain);
    n = awdl_radiotap_tx(out, basic, true);
    g_assert_cmpmem(out, n, noack, sizeof noack);
    n = awdl_radiotap_tx(out, (struct awdl_rate){ .ht = true, .sgi = true, .value = 7 }, true);
    g_assert_cmpmem(out, n, ht, sizeof ht);
    /* What it writes, it reads. */
    uint8_t f[32];
    n = awdl_radiotap_tx(f, (struct awdl_rate){ .ht = true, .value = 3 }, false);
    memcpy(f + n, "x", 1);
    struct awdl_rx rx;
    g_assert_cmpint(awdl_radiotap_parse(f, n + 1, &rx), ==, 0);
    g_assert_cmpmem(rx.frame, rx.len, "x", 1);
    g_assert_cmpuint(awdl_crc32((const uint8_t *)"123456789", 9), ==, 0xcbf43926);
}

/* ---------------------------------------------------------- IPv6 and JSON */

/* spec: NET-1 */
static void test_link_local(void)
{
    uint8_t ll[16];
    static const uint8_t want[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x5e, 0xff, 0xfe, 0x00, 0x53, 0x01 };
    awdl_link_local(&PHONE, ll);
    g_assert_cmpmem(ll, 16, want, 16);
    awdl_mac back;
    g_assert_true(awdl_mac_from_link_local(ll, &back));
    g_assert_true(awdl_mac_eq(&back, &PHONE));
    static const uint8_t one[16] = { 0xfe, 0x80, [15] = 1 };
    g_assert_false(awdl_mac_from_link_local(one, &back));
    char text[18];
    awdl_mac_format(&PHONE, text);
    g_assert_cmpstr(text, ==, "02:00:5e:00:53:01");
}

static size_t tcp_packet(uint8_t *out, const awdl_mac *src, const awdl_mac *dst, uint8_t flags,
                         const char *payload)
{
    size_t plen = strlen(payload), seg = 20 + plen;
    memset(out, 0, 40 + seg);
    out[0] = 0x60;
    out[4] = (uint8_t)(seg >> 8);
    out[5] = (uint8_t)seg;
    out[6] = 6;
    out[7] = 64;
    awdl_link_local(src, out + 8);
    awdl_link_local(dst, out + 24);
    out[40 + 12] = 5 << 4;
    out[40 + 13] = flags;
    memcpy(out + 60, payload, plen);
    return 40 + seg;
}

static void test_ip(void)
{
    uint8_t p[128];
    size_t n = tcp_packet(p, &PHONE, &US, 0x02, "");
    g_assert_true(awdl_ip_bare_syn_to(&US, &US, p, n));
    g_assert_false(awdl_ip_bare_syn_to(&PHONE, &US, p, n)); /* not to us */
    n = tcp_packet(p, &PHONE, &US, 0x12, "");
    g_assert_false(awdl_ip_bare_syn_to(&US, &US, p, n)); /* SYN-ACK */
    n = tcp_packet(p, &PHONE, &US, 0x02, "x");
    g_assert_false(awdl_ip_bare_syn_to(&US, &US, p, n)); /* with data */
    n = tcp_packet(p, &PHONE, &US, 0x10, "");
    g_assert_false(awdl_ip_bare_syn_to(&US, &US, p, n)); /* ACK */

    /* OWLY-2: a valid echo request; its checksum sums to zero with itself. */
    uint8_t e[128];
    n = awdl_ip_echo_request(e, sizeof e, &US, &PHONE, AWDL_ECHO_ID, 1, (const uint8_t *)"owly", 4);
    g_assert_cmpuint(n, ==, 40 + 8 + 4);
    g_assert_cmpuint(e[7], ==, 255);
    uint32_t sum = 0;
    for (int i = 8; i < 40; i += 2)
        sum += (uint32_t)(e[i] << 8 | e[i + 1]);
    sum += (uint32_t)(n - 40) + 58;
    for (size_t i = 40; i < n; i += 2)
        sum += (uint32_t)(e[i] << 8 | (i + 1 < n ? e[i + 1] : 0));
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    g_assert_cmpuint(sum, ==, 0xffff);
    /* The reply: type 129, the same identifier. */
    g_assert_false(awdl_ip_is_echo_reply(e, n, AWDL_ECHO_ID));
    e[40] = 129;
    g_assert_true(awdl_ip_is_echo_reply(e, n, AWDL_ECHO_ID));
    g_assert_false(awdl_ip_is_echo_reply(e, n, 1));
    g_assert_cmpuint(awdl_ip_echo_request(e, 40, &US, &PHONE, 1, 1, (const uint8_t *)"x", 1), ==, 0);
}

static void test_json(void)
{
    char out[64];
    g_assert_cmpuint(awdl_json_string(out, sizeof out, (const uint8_t *)"iPhone", 6), ==, 8);
    g_assert_cmpstr(out, ==, "\"iPhone\"");
    static const uint8_t nasty[] = { '"', '\\', '\n', 0x01, 0x7f, 0xff, 0xc3, 0xa9, 0xe2, 0x80 };
    g_assert_cmpuint(awdl_json_string(out, sizeof out, nasty, sizeof nasty), >, 0);
    g_assert_cmpstr(out, ==, "\"\\\"\\\\\\u000a\\u0001\\u007f\xef\xbf\xbd\xc3\xa9\xef\xbf\xbd\xef\xbf\xbd\"");
    /* Overlong forms and surrogates are replaced too. */
    static const uint8_t overlong[] = { 0xc0, 0xaf, 0xed, 0xa0, 0x80 };
    g_assert_cmpuint(awdl_json_string(out, sizeof out, overlong, sizeof overlong), >, 0);
    g_assert_null(strchr(out, '\xc0'));
    g_assert_null(strstr(out, "\xed\xa0"));
    /* What doesn't fit isn't written half. */
    g_assert_cmpuint(awdl_json_string(out, 6, (const uint8_t *)"iPhone", 6), ==, 0);
    g_assert_cmpuint(awdl_json_string(out, sizeof out, NULL, 0), ==, 2);
    g_assert_cmpstr(out, ==, "\"\"");
}

/* ---------------------------------------------------------- the node */

#define MAX_TX 512

/* What the node asked of its host. */
static struct {
    struct {
        uint8_t bytes[AWDL_FRAME_MAX];
        size_t len;
        awdl_mac to;
        bool no_ack, action;
        unsigned slot;
    } tx[MAX_TX];
    int ntx;
    int up, down, delivered;
    char log[2048];
    awdl_mac up_mac;
    uint8_t up_name[AWDL_NAME_MAX];
    size_t up_name_len;
    int8_t up_rssi;
    uint8_t devclass;
    uint8_t ip[2048];
    size_t ip_len;
    enum awdl_tx_result result;
    struct awdl_node *node;
    awdl_time now;
} R;

static void rec_log(const char *what)
{
    g_strlcat(R.log, what, sizeof R.log);
    g_strlcat(R.log, ";", sizeof R.log);
}

static enum awdl_tx_result rec_transmit(void *ctx, const struct awdl_tx *tx)
{
    if (R.result != AWDL_TX_SENT)
        return R.result;
    if (R.ntx < MAX_TX) {
        memcpy(R.tx[R.ntx].bytes, tx->bytes, tx->len);
        R.tx[R.ntx].len = tx->len;
        R.tx[R.ntx].to = tx->to;
        R.tx[R.ntx].no_ack = tx->no_ack;
        R.tx[R.ntx].action = tx->action;
        R.tx[R.ntx].slot = awdl_clock_slot(&R.node->clock, R.now);
        R.ntx++;
    }
    return AWDL_TX_SENT;
}

static void rec_peer_up(void *ctx, const struct awdl_peer *p)
{
    R.up++;
    R.up_mac = p->mac;
    memcpy(R.up_name, p->name, p->name_len);
    R.up_name_len = p->name_len;
    R.up_rssi = p->rssi;
    R.devclass = p->devclass;
    rec_log("up");
}

static void rec_peer_down(void *ctx, const struct awdl_peer *p)
{
    R.down++;
    rec_log("down");
}

static void rec_deliver(void *ctx, const awdl_mac *dst, const awdl_mac *src, const uint8_t *ip, size_t len)
{
    R.delivered++;
    memcpy(R.ip, ip, len);
    R.ip_len = len;
    rec_log("deliver");
}

static struct awdl_node *node_new(uint8_t channel, enum awdl_presence presence, bool accept_addba)
{
    memset(&R, 0, sizeof R);
    struct awdl_config cfg;
    awdl_config_init(&cfg, &US, "lares", channel, presence);
    cfg.accept_addba = accept_addba;
    struct awdl_host host = { NULL, rec_transmit, rec_peer_up, rec_peer_down, rec_deliver };
    struct awdl_node *n = g_malloc0(sizeof *n);
    R.node = n;
    R.now = START;
    awdl_node_init(n, &cfg, &host, START);
    return n;
}

/* An iPhone's action frame, after owly's tests: master of its own tree
 * with metric 600, at the start of its AW `aw_counter`'s slot. */
static size_t frame_from(uint8_t *buf, size_t cap, const awdl_mac *mac, uint8_t subtype,
                         const struct awdl_sequence *seq, uint16_t aw_counter, uint16_t next_tu)
{
    struct awdl_election e;
    awdl_election_init(&e, mac);
    e.self_metric = e.master_metric = 600;
    struct awdl_own own = {
        .addr = *mac, .name = "iPhone", .name_len = 6, .devclass = AWDL_DEVCLASS_IOS,
        .channel = awdl_seq_channel(seq, 0), .master_channel = 149, .next_tu = next_tu,
        .aw_counter = aw_counter, .psf_interval_tu = 110, .sequence = seq, .election = &e, .has_ht = true,
        .ht = { AWDL_HT_CAPABILITIES, AWDL_HT_AMPDU_PARAMS, AWDL_HT_RX_MCS },
    };
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, cap);
    awdl_action_frame(&w, mac, 1, subtype, 0, &own);
    g_assert_false(w.overflow);
    return w.len;
}

static void feed(struct awdl_node *n, awdl_time t, const uint8_t *b, size_t len)
{
    R.now = t;
    awdl_node_frame(n, t, true, -40, b, len);
}

static void feed_phone(struct awdl_node *n, awdl_time t, uint8_t subtype, const struct awdl_sequence *s,
                       uint16_t aw, uint16_t next_tu)
{
    uint8_t buf[1024];
    size_t len = frame_from(buf, sizeof buf, &PHONE, subtype, s, aw, next_tu);
    feed(n, t, buf, len);
}

static void tick(struct awdl_node *n, awdl_time t)
{
    R.now = t;
    awdl_node_timer(n, t);
}

/* A tick with only its own transmissions in R.tx. */
static void tick_fresh(struct awdl_node *n, awdl_time t)
{
    R.ntx = 0;
    tick(n, t);
}

static void outbound(struct awdl_node *n, awdl_time t, const awdl_mac *dst, const uint8_t *ip, size_t len)
{
    R.now = t;
    awdl_node_outbound(n, t, dst, &US, ip, len);
}

/* The phone made our master: a MIF, then the election's tick. */
static awdl_time meet(struct awdl_node *n, const struct awdl_sequence *s)
{
    feed_phone(n, START + 100000, AWDL_SUBTYPE_MIF, s, 400, 64);
    g_assert_cmpint(R.up, ==, 1);
    awdl_time t = START + 1000000;
    /* Heard again just before the tick, so its phase is current. */
    feed_phone(n, t, AWDL_SUBTYPE_PSF, s, 400 + 4 * 14, 64);
    tick(n, t);
    g_assert_true(awdl_mac_eq(&n->election.sync, &PHONE));
    return t;
}

static size_t data_from(uint8_t *buf, size_t cap, const awdl_mac *src, const awdl_mac *dst, const uint8_t *ip,
                        size_t len)
{
    struct awdl_wbuf w;
    awdl_w_init(&w, buf, cap);
    awdl_data_frame(&w, src, dst, 9, 9, ip, len);
    return w.len;
}

static bool ends_with(const uint8_t *b, size_t n, const char *tail)
{
    size_t t = strlen(tail);
    return n >= t && memcmp(b + n - t, tail, t) == 0;
}

/* spec: AWDL-17, T-AWDL-1: the iPhone's own frames make it a valid peer. */
static void test_node_iphone_frames(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    uint8_t buf[1024];
    struct awdl_rx rx = captured(PSF_HEX, buf, sizeof buf);
    R.now = START;
    awdl_node_frame(n, START, rx.has_rssi, rx.rssi, rx.frame, rx.len);
    g_assert_nonnull(awdl_node_peer(n, &PHONE));
    g_assert_false(awdl_node_peer(n, &PHONE)->valid);
    g_assert_cmpint(R.up, ==, 0);
    rx = captured(MIF_HEX, buf, sizeof buf);
    R.now = START + 50000;
    awdl_node_frame(n, R.now, rx.has_rssi, rx.rssi, rx.frame, rx.len);
    g_assert_cmpint(R.up, ==, 1);
    g_assert_true(awdl_mac_eq(&R.up_mac, &PHONE));
    g_assert_cmpmem(R.up_name, R.up_name_len, "iPhone", 6);
    g_assert_cmpint(R.up_rssi, ==, -24);
    g_assert_cmpuint(R.devclass, ==, AWDL_DEVCLASS_IOS);
    /* The tick: metric 516 beats our 60, so we follow it (AWDL-9). */
    tick(n, START + AWDL_TICK_US);
    g_assert_true(awdl_mac_eq(&n->election.sync, &PHONE));
    g_assert_cmpuint(n->election.height, ==, 1);
    g_assert_cmpuint(n->election.master_metric, ==, 516);
    /* Its mDNS from another node, which isn't a peer: not delivered. */
    rx = captured(DATA_HEX, buf, sizeof buf);
    awdl_node_frame(n, START + AWDL_TICK_US, rx.has_rssi, rx.rssi, rx.frame, rx.len);
    g_assert_cmpint(R.delivered, ==, 0);
    g_assert_cmpuint(n->stats.rx_data, ==, 1);
    /* Our clock follows its schedule (AWDL-7), as of the frame's target TX
     * time: this one went out a little after it. */
    rx = captured(PSF_HEX, buf, sizeof buf);
    struct awdl_rx_frame f;
    g_assert_cmpint(awdl_frame_parse(rx.frame, rx.len, &f), ==, 0);
    uint32_t late = f.phy_tx - f.target_tx;
    g_assert_cmpuint(late, >, 0);
    g_assert_cmpuint(late, <=, AWDL_TX_LATE_MAX_US);
    awdl_time t = START + AWDL_TICK_US + 1000;
    awdl_node_frame(n, t, rx.has_rssi, rx.rssi, rx.frame, rx.len);
    g_assert_cmpint(awdl_clock_error_tu(&n->clock, t - late, 21, 0x14ea), ==, 0);
    g_assert_cmpuint(awdl_clock_slot(&n->clock, t - late), ==, (0x14ea / 4) % 16);
    g_free(n);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* spec: AWDL-3, AWDL-7: a frame that waited for the air carries the
 * schedule as of its target TX time (issue #107). */
static void test_node_follows_target_time(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    awdl_time t = meet(n, &s) + 200000;
    uint8_t buf[1024];
    /* PHY and target TX times sit 8 and 12 bytes into the action body. */
    size_t len = frame_from(buf, sizeof buf, &PHONE, AWDL_SUBTYPE_PSF, &s, 480, 40);
    put_le32(buf + AWDL_HEADER_LEN + 8, 1010000);
    put_le32(buf + AWDL_HEADER_LEN + 12, 1000000);
    feed(n, t, buf, len);
    /* 40 TU to the boundary was true 10 ms before it arrived. */
    g_assert_cmpint(awdl_clock_error_tu(&n->clock, t - 10000, 40, 480), ==, 0);
    /* Out later than a slot past its target: not followed. */
    len = frame_from(buf, sizeof buf, &PHONE, AWDL_SUBTYPE_PSF, &s, 484, 10);
    put_le32(buf + AWDL_HEADER_LEN + 8, 1000000 + (uint32_t)AWDL_SLOT_US + 1);
    put_le32(buf + AWDL_HEADER_LEN + 12, 1000000);
    feed(n, t + 1000, buf, len);
    g_assert_cmpint(awdl_clock_error_tu(&n->clock, t - 10000, 40, 480), ==, 0);
    g_free(n);
}

/* spec: AWDL-12 */
static void test_node_fixed_sequence(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    meet(n, &s);
    static const uint8_t six[AWDL_SLOTS] = { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 };
    assert_channels(&n->sequence, six);
    g_free(n);

    /* A shared radio advertises only the master's slots on its channel; on
     * a channel the master never visits it keeps what it had. */
    n = node_new(6, AWDL_PRESENCE_SHARE, false);
    meet(n, &s);
    uint8_t want[AWDL_SLOTS] = { 0 };
    want[8] = 6;
    assert_channels(&n->sequence, want);
    g_assert_false(n->has_no_overlap);
    g_free(n);
    n = node_new(44, AWDL_PRESENCE_SHARE, false);
    meet(n, &s);
    static const uint8_t forty_four[AWDL_SLOTS] = { 44, 44, 44, 44, 44, 44, 44, 44,
                                                    44, 44, 44, 44, 44, 44, 44, 44 };
    assert_channels(&n->sequence, forty_four);
    g_assert_true(n->has_no_overlap);
    g_assert_cmpuint(n->no_overlap_channel, ==, 149);
    g_free(n);
}

/* spec: AWDL-17 */
static void test_node_valid_once(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    feed_phone(n, START, AWDL_SUBTYPE_PSF, &s, 400, 64);
    g_assert_cmpint(R.up, ==, 0);
    feed_phone(n, START + 1000, AWDL_SUBTYPE_MIF, &s, 400, 63);
    g_assert_cmpint(R.up, ==, 1);
    g_assert_cmpmem(R.up_name, R.up_name_len, "iPhone", 6);
    feed_phone(n, START + 2000, AWDL_SUBTYPE_MIF, &s, 400, 62);
    g_assert_cmpint(R.up, ==, 1);
    g_free(n);
}

/* spec: SEC-11 */
static void test_node_spoofed_peers(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    uint8_t buf[1024];
    for (unsigned i = 0; i < AWDL_MAX_PEERS; i++) {
        awdl_mac fake = { { 0x02, 0xff, 0, 0, (uint8_t)(i >> 8), (uint8_t)i } };
        size_t len = frame_from(buf, sizeof buf, &fake, AWDL_SUBTYPE_MIF, &s, 400, 64);
        feed(n, START + i * 1000ULL, buf, len);
    }
    g_assert_cmpuint(n->npeers, ==, AWDL_MAX_PEERS);
    g_assert_cmpint(R.up, ==, (int)AWDL_MAX_PEERS);
    /* The phone takes the place of the fake heard from longest ago. */
    R.log[0] = 0;
    feed_phone(n, START + AWDL_MAX_PEERS * 1000ULL, AWDL_SUBTYPE_MIF, &s, 400, 64);
    g_assert_cmpstr(R.log, ==, "down;up;");
    awdl_mac first = { { 0x02, 0xff, 0, 0, 0, 0 } }, second = { { 0x02, 0xff, 0, 0, 0, 1 } };
    g_assert_null(awdl_node_peer(n, &first));
    g_assert_nonnull(awdl_node_peer(n, &second));
    g_assert_nonnull(awdl_node_peer(n, &PHONE));
    g_assert_cmpuint(n->stats.peers_evicted, ==, 1);
    g_assert_cmpuint(n->npeers, ==, AWDL_MAX_PEERS);
    g_free(n);
}

/* spec: AWDL-18 */
static void test_node_early_syn(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    feed_phone(n, START, AWDL_SUBTYPE_PSF, &s, 400, 64);
    uint8_t syn[128], frame[256];
    size_t sl = tcp_packet(syn, &PHONE, &US, 0x02, "");
    size_t fl = data_from(frame, sizeof frame, &PHONE, &US, syn, sl);
    feed(n, START + 10, frame, fl);
    feed(n, START + 20, frame, fl); /* the duplicate is held once */
    g_assert_cmpint(R.delivered, ==, 0);
    g_assert_cmpuint(n->stats.syn_held, ==, 1);
    feed_phone(n, START + 30, AWDL_SUBTYPE_MIF, &s, 400, 63);
    /* The neighbour entry comes first. */
    g_assert_cmpstr(R.log, ==, "up;deliver;");
    g_assert_cmpmem(R.ip, R.ip_len, syn, sl);
    g_free(n);

    /* Held SYNs expire. */
    n = node_new(6, AWDL_PRESENCE_FIXED, false);
    feed(n, START, frame, fl);
    tick(n, START + 3000000);
    feed_phone(n, START + 3100000, AWDL_SUBTYPE_MIF, &s, 400, 63);
    g_assert_cmpint(R.delivered, ==, 0);
    g_free(n);
}

/* spec: AWDL-17, NET-3 */
static void test_node_data_to_stack(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    uint8_t ip[128], frame[256];
    size_t il = awdl_ip_echo_request(ip, sizeof ip, &PHONE, &US, 1, 1, (const uint8_t *)"hi", 2);
    size_t fl = data_from(frame, sizeof frame, &PHONE, &US, ip, il);
    feed(n, START, frame, fl); /* from a stranger: dropped */
    g_assert_cmpint(R.delivered, ==, 0);
    meet(n, &s);
    awdl_time t = START + 1100000;
    feed(n, t, frame, fl);
    g_assert_cmpint(R.delivered, ==, 1);
    g_assert_cmpmem(R.ip, R.ip_len, ip, il);
    /* To someone else, which a monitor overhears. */
    awdl_mac else_ = { { 0x02, 9, 9, 9, 9, 9 } };
    fl = data_from(frame, sizeof frame, &PHONE, &else_, ip, il);
    feed(n, t, frame, fl);
    g_assert_cmpint(R.delivered, ==, 1);
    /* To a multicast group. */
    fl = data_from(frame, sizeof frame, &PHONE, &MDNS_GROUP, ip, il);
    feed(n, t, frame, fl);
    g_assert_cmpint(R.delivered, ==, 2);
    /* Data keeps the peer alive without action frames (owly's patch owl-07). */
    awdl_time late = t + 4500000;
    feed(n, late, frame, fl);
    tick(n, late + 1000000);
    g_assert_cmpint(R.down, ==, 0);
    tick(n, late + 5100000);
    g_assert_cmpint(R.down, ==, 1);
    g_assert_null(awdl_node_peer(n, &PHONE));
    g_free(n);
}

static void test_node_outbound_to_nobody(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    uint8_t ip[128];
    size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, 1, (const uint8_t *)"x", 1);
    outbound(n, START, &PHONE, ip, il);
    g_assert_cmpuint(n->stats.dropped_no_peer, ==, 1);
    il = awdl_ip_echo_request(ip, sizeof ip, &US, &US, 1, 1, (const uint8_t *)"x", 1);
    outbound(n, START, &US, ip, il);
    g_assert_cmpint(R.delivered, ==, 1);
    g_free(n);
}

/* Steps the node every 256 µs until a frame to `to` ending in `tail` goes;
 * its index in R.tx, or -1. */
static int run_until_sent(struct awdl_node *n, awdl_time *t, const awdl_mac *to, const char *tail)
{
    for (uint64_t i = 0; i < 32 * AWDL_SLOT_US / 256; i++) {
        *t += 256;
        tick_fresh(n, *t);
        for (int k = 0; k < R.ntx; k++)
            if (awdl_mac_eq(&R.tx[k].to, to) && ends_with(R.tx[k].bytes, R.tx[k].len, tail))
                return k;
    }
    return -1;
}

/* spec: AWDL-16 */
static void test_node_unicast_slots(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_SHARE, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    awdl_time t = meet(n, &s);
    uint8_t ip[128];
    size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, 1, (const uint8_t *)"payload", 7);
    outbound(n, t, &PHONE, ip, il);
    int k = run_until_sent(n, &t, &PHONE, "payload");
    g_assert_cmpint(k, >=, 0);
    g_assert_cmpuint(awdl_clock_slot(&n->clock, t), ==, 8); /* the slot both are on 6 */
    uint64_t into = AWDL_SLOT_US - awdl_clock_next_slot_us(&n->clock, t);
    g_assert_cmpuint(into, >=, AWDL_UNICAST_GUARD_US);
    g_assert_false(R.tx[k].no_ack); /* a shared radio waits for ACKs */
    g_free(n);
}

/* spec: AWDL-12, AWDL-16 */
static void test_node_fixed_unicast(void)
{
    uint8_t phone[AWDL_SLOTS];
    memcpy(phone, PHONE_SEQ, sizeof phone);
    phone[3] = 6;
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, phone);
    awdl_time t = meet(n, &s);
    bool seen[AWDL_SLOTS] = { false };
    for (uint16_t i = 0; i < 6; i++) {
        uint8_t ip[128];
        size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, i, (const uint8_t *)"payload", 7);
        outbound(n, t, &PHONE, ip, il);
        int k = run_until_sent(n, &t, &PHONE, "payload");
        g_assert_cmpint(k, >=, 0);
        g_assert_true(R.tx[k].no_ack); /* sent once, without an ACK (owl-05) */
        seen[awdl_clock_slot(&n->clock, t)] = true;
        t += AWDL_SLOT_US;
    }
    for (unsigned i = 0; i < AWDL_SLOTS; i++)
        g_assert_cmpint(seen[i], ==, i == 3 || i == 8);
    g_free(n);
}

/* A multicast IPv6 packet with `tail` at its end, to ff02::fb. */
static size_t mdns_packet(uint8_t *ip, size_t cap, const char *tail)
{
    size_t n = strlen(tail);
    g_assert_cmpuint(40 + n, <=, cap);
    memset(ip, 0, 40);
    ip[0] = 0x60;
    ip[4] = (uint8_t)(n >> 8);
    ip[5] = (uint8_t)n;
    ip[6] = 17;
    ip[7] = 255;
    ip[24] = 0xff;
    ip[25] = 0x02;
    ip[39] = 0xfb;
    memcpy(ip + 40, tail, n);
    return 40 + n;
}

/* A fixed radio holds multicast for the slot the phone spends on its
 * channel, early in it, so mDNS is heard on the phone's next visit. */
static void test_node_fixed_multicast_aims(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, IPHONE_SEQ);
    awdl_time t = meet(n, &s);
    for (int round = 0; round < 3; round++) {
        /* Start somewhere else in the sequence each time. */
        t += (uint64_t)(3 + 5 * round) * AWDL_SLOT_US;
        uint8_t ip[128];
        char tail[16];
        g_snprintf(tail, sizeof tail, "mdns-%d", round);
        size_t il = mdns_packet(ip, sizeof ip, tail);
        outbound(n, t, &MDNS_GROUP, ip, il);
        int k = run_until_sent(n, &t, &MDNS_GROUP, tail);
        g_assert_cmpint(k, >=, 0);
        g_assert_cmpuint(awdl_clock_slot(&n->clock, t), ==, 8);
        uint64_t into = AWDL_SLOT_US - awdl_clock_next_slot_us(&n->clock, t);
        g_assert_cmpuint(into, >=, AWDL_UNICAST_GUARD_US);
        g_assert_cmpuint(into, <, AWDL_UNICAST_GUARD_US + 1024);
    }
    g_free(n);
}

/* With no peer known yet, a fixed radio's multicast goes at once. */
static void test_node_fixed_multicast_alone(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    awdl_time t = START + 3 * AWDL_SLOT_US + AWDL_SLOT_US / 2;
    uint8_t ip[128];
    size_t il = mdns_packet(ip, sizeof ip, "early");
    R.ntx = 0;
    outbound(n, t, &MDNS_GROUP, ip, il);
    bool sent = false;
    for (int k = 0; k < R.ntx; k++)
        sent |= awdl_mac_eq(&R.tx[k].to, &MDNS_GROUP) && ends_with(R.tx[k].bytes, R.tx[k].len, "early");
    g_assert_true(sent);
    g_free(n);
}

/* Copies of one multicast that wait for the same slot go once; a different
 * packet queued between them still goes. */
static void test_node_multicast_copies_merge(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, IPHONE_SEQ);
    awdl_time t = meet(n, &s);
    /* Into a slot the phone spends elsewhere, clear of its edges. */
    while (awdl_clock_slot(&n->clock, t) != 3)
        t += AWDL_SLOT_US / 4;
    t += AWDL_SLOT_US / 2 - (AWDL_SLOT_US - awdl_clock_next_slot_us(&n->clock, t));
    uint8_t ip[128], q[128];
    size_t il = mdns_packet(ip, sizeof ip, "announce"), ql = mdns_packet(q, sizeof q, "query");
    R.ntx = 0;
    for (int i = 0; i < 40; i++) {
        outbound(n, t, &MDNS_GROUP, ip, il);
        if (i == 20)
            outbound(n, t, &MDNS_GROUP, q, ql);
    }
    g_assert_cmpuint(n->multicast.n, ==, 2);
    unsigned announce = 0, query = 0;
    for (uint64_t i = 0; i < 8 * AWDL_SLOT_US / 256 && awdl_clock_slot(&n->clock, t) != 9; i++) {
        t += 256;
        tick_fresh(n, t);
        for (int k = 0; k < R.ntx; k++) {
            if (!awdl_mac_eq(&R.tx[k].to, &MDNS_GROUP))
                continue;
            announce += ends_with(R.tx[k].bytes, R.tx[k].len, "announce");
            query += ends_with(R.tx[k].bytes, R.tx[k].len, "query");
        }
    }
    g_assert_cmpuint(announce, ==, 1);
    g_assert_cmpuint(query, ==, 1);
    g_free(n);
}

static void test_node_packets_go_with_their_peer(void)
{
    /* On 44, where the phone never is, so nothing goes. */
    struct awdl_node *n = node_new(44, AWDL_PRESENCE_SHARE, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    awdl_time t = meet(n, &s);
    awdl_time queued = t + 4500000;
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t ip[128];
        size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, i, (const uint8_t *)"payload", 7);
        outbound(n, queued, &PHONE, ip, il);
    }
    uint16_t backlog = n->unicast.n; /* ours, and the node's own echoes */
    g_assert_cmpuint(backlog, >=, 3);
    tick(n, t + 5100000);
    g_assert_cmpint(R.down, ==, 1);
    g_assert_cmpuint(n->unicast.n, ==, 0);
    g_assert_cmpuint(n->stats.dropped_no_peer, ==, backlog);
    g_assert_cmpuint(n->stats.dropped_expired, ==, 0);
    g_assert_false(n->has_no_overlap); /* no master, no overlap to miss */
    g_free(n);
}

/* spec: AWDL-16 */
static void test_node_multicast_slots(void)
{
    for (int fixed = 0; fixed < 2; fixed++) {
        struct awdl_node *n = node_new(6, fixed ? AWDL_PRESENCE_FIXED : AWDL_PRESENCE_SHARE, false);
        awdl_time t = START;
        bool slots[AWDL_SLOTS] = { false };
        int sent = 0;
        for (uint16_t i = 0; i < 40; i++) {
            uint8_t ip[128];
            size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, i, (const uint8_t *)"m", 1);
            outbound(n, t, &MDNS_GROUP, ip, il);
            awdl_time end = t + AWDL_SLOT_US;
            while (t < end) {
                tick_fresh(n, t);
                for (int k = 0; k < R.ntx; k++)
                    if (awdl_mac_eq(&R.tx[k].to, &MDNS_GROUP)) {
                        slots[R.tx[k].slot] = true;
                        sent++;
                    }
                t += 512;
            }
        }
        g_assert_cmpint(sent, >, 0);
        int distinct = 0;
        for (unsigned i = 0; i < AWDL_SLOTS; i++) {
            distinct += slots[i];
            if (!fixed)
                g_assert_true(!slots[i] || i == 0 || i == 10);
        }
        if (fixed)
            g_assert_cmpint(distinct, >, 2);
        g_free(n);
    }
}

static void test_node_psf_and_mif_timing(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    awdl_time t = START, psf[16], mif[32];
    int npsf = 0, nmif = 0;
    while (t < START + 1000000) {
        tick_fresh(n, t);
        for (int k = 0; k < R.ntx; k++) {
            struct awdl_rx_frame f;
            if (awdl_frame_parse(R.tx[k].bytes, R.tx[k].len, &f) == 0 && f.kind == AWDL_RX_ACTION) {
                if (f.subtype == AWDL_SUBTYPE_PSF && npsf < 16)
                    psf[npsf++] = t;
                else if (f.subtype == AWDL_SUBTYPE_MIF && nmif < 32)
                    mif[nmif++] = t;
                g_assert_true(R.tx[k].action);
            }
        }
        awdl_time next = awdl_node_deadline(n, t);
        g_assert_cmpuint(next, >, t);
        t = next;
    }
    g_assert_cmpint(npsf, >, 2);
    g_assert_cmpuint(psf[0], ==, START + AWDL_PSF_INTERVAL_US);
    for (int i = 1; i < npsf; i++)
        g_assert_cmpuint(psf[i] - psf[i - 1], ==, AWDL_PSF_INTERVAL_US);
    g_assert_cmpint(nmif, >=, 14);
    g_assert_cmpint(nmif, <=, 16);
    /* The first goes at once, the rest mid-slot. */
    g_assert_cmpuint(mif[0], ==, START);
    for (int i = 2; i < nmif; i++) {
        int64_t d = (int64_t)(mif[i] - mif[i - 1]) - (int64_t)AWDL_SLOT_US;
        g_assert_cmpint(d < 0 ? -d : d, <, 2 * TU);
    }
    g_free(n);
}

static void test_node_weak_frames(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    uint8_t buf[1024];
    size_t len = frame_from(buf, sizeof buf, &PHONE, AWDL_SUBTYPE_MIF, &s, 400, 64);
    awdl_node_frame(n, START, true, -66, buf, len);
    g_assert_null(awdl_node_peer(n, &PHONE));
    awdl_node_frame(n, START, true, -60, buf, len);
    awdl_node_frame(n, START + 1, true, -69, buf, len);
    g_assert_cmpint(awdl_node_peer(n, &PHONE)->rssi, ==, -69);
    awdl_node_frame(n, START + 2, true, -71, buf, len);
    g_assert_cmpint(awdl_node_peer(n, &PHONE)->rssi, ==, -69);
    g_free(n);
}

static void test_node_malformed_changes_nothing(void)
{
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    uint8_t buf[1024];
    size_t len = frame_from(buf, sizeof buf, &PHONE, AWDL_SUBTYPE_MIF, &s, 400, 64);
    buf[len++] = AWDL_TLV_VERSION; /* half a TLV header */
    buf[len++] = 2;
    feed(n, START, buf, len);
    g_assert_null(awdl_node_peer(n, &PHONE));
    g_assert_cmpuint(n->stats.rx_malformed, ==, 1);
    g_free(n);
}

/* spec: AWDL-15 */
static void test_node_addba(void)
{
    uint8_t req[64];
    struct awdl_wbuf w;
    awdl_w_init(&w, req, sizeof req);
    awdl_put_header(&w, AWDL_FC_ACTION, &US, &PHONE, 3);
    static const uint8_t body[] = { 3, 0, 9, 0x02, 0x10, 0, 0, 0, 0 };
    awdl_w_bytes(&w, body, sizeof body);
    for (int accept = 0; accept < 2; accept++) {
        struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, accept);
        struct awdl_sequence s;
        seq_from(&s, PHONE_SEQ);
        meet(n, &s);
        int before = R.ntx;
        feed(n, START + 1200000, req, w.len);
        bool answered = false;
        for (int k = before; k < R.ntx; k++)
            answered |= awdl_mac_eq(&R.tx[k].to, &PHONE) && R.tx[k].bytes[24] == 3 && R.tx[k].bytes[25] == 1;
        g_assert_cmpint(answered, ==, accept);
        g_free(n);
    }
}

/* A full radio holds data for a moment, without spinning, and drops
 * action frames rather than send them stale. */
static void test_node_radio_full(void)
{
    uint8_t phone[AWDL_SLOTS];
    memset(phone, 6, sizeof phone);
    struct awdl_node *n = node_new(6, AWDL_PRESENCE_FIXED, false);
    struct awdl_sequence s;
    seq_from(&s, phone);
    awdl_time t = meet(n, &s);
    t += AWDL_SLOT_US / 2; /* clear of the slot's edges */
    R.result = AWDL_TX_FULL;
    uint8_t ip[128];
    size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, 1, (const uint8_t *)"payload", 7);
    uint16_t before = n->unicast.n;
    outbound(n, t, &PHONE, ip, il);
    g_assert_cmpuint(n->unicast.n, ==, before + 1);
    g_assert_cmpuint(n->stats.radio_full, >=, 1);
    /* It waits out the pause rather than asking to be woken at once. */
    g_assert_cmpuint(n->paused_until, ==, t + AWDL_RETRY_PAUSE_US);
    awdl_time want = n->paused_until;
    awdl_time timers[] = { n->next_tick, n->next_psf, n->next_mif, n->next_echo };
    for (size_t i = 0; i < G_N_ELEMENTS(timers); i++)
        want = MIN(want, timers[i]);
    g_assert_cmpuint(awdl_node_deadline(n, t), ==, MAX(want, t));
    R.result = AWDL_TX_SENT;
    int k = run_until_sent(n, &t, &PHONE, "payload");
    g_assert_cmpint(k, >=, 0);
    g_free(n);
}

static void test_node_takes_outbound(void)
{
    struct awdl_node *n = node_new(44, AWDL_PRESENCE_SHARE, false);
    struct awdl_sequence s;
    seq_from(&s, PHONE_SEQ);
    awdl_time t = meet(n, &s);
    uint8_t ip[128];
    size_t il = awdl_ip_echo_request(ip, sizeof ip, &US, &PHONE, 1, 1, (const uint8_t *)"x", 1);
    for (unsigned i = 0; i < AWDL_UNICAST_QUEUE + 5; i++)
        outbound(n, t, &PHONE, ip, il);
    g_assert_false(awdl_node_takes_outbound(n));
    g_assert_cmpuint(n->unicast.n, ==, AWDL_UNICAST_QUEUE);
    g_assert_cmpuint(n->stats.dropped_queue_full, >=, 5);
    /* Too big for a slot: refused, not truncated. */
    static uint8_t big[AWDL_PACKET_MAX + 1];
    outbound(n, t, &MDNS_GROUP, big, sizeof big);
    g_assert_cmpuint(n->stats.dropped_too_big, ==, 1);
    g_free(n);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/awdl/iphone/psf", test_iphone_psf);
    g_test_add_func("/awdl/iphone/mif", test_iphone_mif);
    g_test_add_func("/awdl/iphone/data", test_iphone_data);
    g_test_add_func("/awdl/tlv/own-round-trip", test_own_tlvs_round_trip);
    g_test_add_func("/awdl/tlv/action-frame", test_action_frame_round_trip);
    g_test_add_func("/awdl/tlv/padding", test_padding);
    g_test_add_func("/awdl/tlv/unknown", test_unknown_tlvs);
    g_test_add_func("/awdl/tlv/names", test_names);
    g_test_add_func("/awdl/channel/encodings", test_sequence_encodings);
    g_test_add_func("/awdl/channel/intersect", test_sequence_intersect);
    g_test_add_func("/awdl/channel/frequencies", test_channels);
    g_test_add_func("/awdl/election", test_election);
    g_test_add_func("/awdl/schedule/clock", test_clock);
    g_test_add_func("/awdl/schedule/guards", test_guards);
    g_test_add_func("/awdl/data/golden", test_data_frame_golden);
    g_test_add_func("/awdl/data/amsdu-bounds", test_amsdu_bounds);
    g_test_add_func("/awdl/frame/malformed", test_malformed_frames);
    g_test_add_func("/awdl/frame/addba", test_addba_response);
    g_test_add_func("/awdl/frame/radiotap-tx", test_radiotap_tx);
    g_test_add_func("/awdl/ip/link-local", test_link_local);
    g_test_add_func("/awdl/ip/packets", test_ip);
    g_test_add_func("/awdl/json", test_json);
    g_test_add_func("/awdl/node/iphone-frames", test_node_iphone_frames);
    g_test_add_func("/awdl/node/target-time", test_node_follows_target_time);
    g_test_add_func("/awdl/node/sequence", test_node_fixed_sequence);
    g_test_add_func("/awdl/node/valid-once", test_node_valid_once);
    g_test_add_func("/awdl/node/spoofed-peers", test_node_spoofed_peers);
    g_test_add_func("/awdl/node/early-syn", test_node_early_syn);
    g_test_add_func("/awdl/node/data-to-stack", test_node_data_to_stack);
    g_test_add_func("/awdl/node/outbound-to-nobody", test_node_outbound_to_nobody);
    g_test_add_func("/awdl/node/unicast-slots", test_node_unicast_slots);
    g_test_add_func("/awdl/node/fixed-unicast", test_node_fixed_unicast);
    g_test_add_func("/awdl/node/fixed-multicast-aims", test_node_fixed_multicast_aims);
    g_test_add_func("/awdl/node/fixed-multicast-alone", test_node_fixed_multicast_alone);
    g_test_add_func("/awdl/node/multicast-copies-merge", test_node_multicast_copies_merge);
    g_test_add_func("/awdl/node/packets-go-with-peer", test_node_packets_go_with_their_peer);
    g_test_add_func("/awdl/node/multicast-slots", test_node_multicast_slots);
    g_test_add_func("/awdl/node/psf-mif-timing", test_node_psf_and_mif_timing);
    g_test_add_func("/awdl/node/weak-frames", test_node_weak_frames);
    g_test_add_func("/awdl/node/malformed", test_node_malformed_changes_nothing);
    g_test_add_func("/awdl/node/addba", test_node_addba);
    g_test_add_func("/awdl/node/radio-full", test_node_radio_full);
    g_test_add_func("/awdl/node/takes-outbound", test_node_takes_outbound);
    return g_test_run();
}
