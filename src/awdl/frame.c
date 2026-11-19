// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* What lares-awdl sends and hears: radiotap, the 802.11 MAC header, AWDL
 * action frames and their TLVs, data frames and A-MSDUs, Block Ack
 * answers, channel sequences, and the few IPv6 fields the data path looks
 * at. It follows owly: owly-awdl's frame.rs, tlv.rs, channel.rs and ip.rs,
 * and owly-radio's radiotap.rs. Every read is bounds-checked; nothing here
 * allocates, logs or keeps state. */
#include "awdl.h"
#include <stdio.h>

const awdl_mac AWDL_BSSID = { { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 } };
const awdl_mac AWDL_BROADCAST = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };

/* Apple's OUI, in action frames and in data frames' SNAP header. */
static const uint8_t OUI[3] = { 0x00, 0x17, 0xf2 };
/* AWDL-13: the SNAP header AWDL data frames carry. */
static const uint8_t LLC_SNAP[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00 };

#define CATEGORY_VENDOR     127     /* AWDL-3 */
#define CATEGORY_BLOCK_ACK  3       /* AWDL-15 */
#define ADDBA_REQUEST       0
#define ADDBA_RESPONSE      1
#define AWDL_TYPE           8       /* AWDL within Apple's OUI */
#define AWDL_VERSION_COMPAT 0x10    /* AWDL 1.0, major in the high nibble, in the action header */
#define DATA_HEAD           0x0403  /* AWDL-13 */
#define SEQ_STEP_COUNT      3       /* §3.2: AWs per slot less one */
#define SEQ_FILL            0xffff
#define ARPA_FLAGS          3       /* the one flag value IO80211Family accepts */
#define ARPA_LOCAL_POINTER  0xc00c  /* a DNS pointer to `.local` */
#define DATA_PATH_FLAGS     0x8f24  /* owly's (legacy's) Data Path State flags */
#define SYNC_FLAGS          0x1800  /* §3.2, reserved */

#define FC_TYPE             0x000c
#define FC_TYPE_MGMT        0x0000
#define FC_TYPE_DATA        0x0008
#define FC_TYPE_SUBTYPE     0x00fc
#define FC_TO_DS            0x0100
#define FC_FROM_DS          0x0200
#define FC_MORE_FRAGMENTS   0x0400
#define FC_PROTECTED        0x4000
#define QOS_AMSDU           0x0080  /* AWDL-14 */

/* ---------------------------------------------------------- reading */

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Whether `len` bytes at `at` lie within `n`. */
static bool fits(size_t n, size_t at, size_t len)
{
    return at <= n && len <= n - at;
}

static void rd_mac(const uint8_t *p, awdl_mac *m)
{
    memcpy(m->a, p, 6);
}

/* ---------------------------------------------------------- addresses */

void awdl_mac_format(const awdl_mac *m, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m->a[0], m->a[1], m->a[2], m->a[3], m->a[4],
             m->a[5]);
}

void awdl_link_local(const awdl_mac *m, uint8_t out[16])
{
    memset(out, 0, 16);
    out[0] = 0xfe;
    out[1] = 0x80;
    out[8] = m->a[0] ^ 0x02;
    out[9] = m->a[1];
    out[10] = m->a[2];
    out[11] = 0xff;
    out[12] = 0xfe;
    out[13] = m->a[3];
    out[14] = m->a[4];
    out[15] = m->a[5];
}

bool awdl_mac_from_link_local(const uint8_t a[16], awdl_mac *out)
{
    static const uint8_t prefix[8] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0 };
    if (memcmp(a, prefix, 8) != 0 || a[11] != 0xff || a[12] != 0xfe)
        return false;
    uint8_t m[6] = { (uint8_t)(a[8] ^ 0x02), a[9], a[10], a[13], a[14], a[15] };
    memcpy(out->a, m, 6);
    return true;
}

/* ---------------------------------------------------------- writing */

void awdl_w_init(struct awdl_wbuf *w, uint8_t *p, size_t cap)
{
    w->p = p;
    w->len = 0;
    w->cap = cap;
    w->overflow = false;
}

void awdl_w_bytes(struct awdl_wbuf *w, const void *b, size_t n)
{
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->len, b, n);
    w->len += n;
}

void awdl_w_zeros(struct awdl_wbuf *w, size_t n)
{
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memset(w->p + w->len, 0, n);
    w->len += n;
}

void awdl_w_u8(struct awdl_wbuf *w, uint8_t v)
{
    awdl_w_bytes(w, &v, 1);
}

void awdl_w_le16(struct awdl_wbuf *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    awdl_w_bytes(w, b, 2);
}

void awdl_w_be16(struct awdl_wbuf *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    awdl_w_bytes(w, b, 2);
}

void awdl_w_le32(struct awdl_wbuf *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    awdl_w_bytes(w, b, 4);
}

void awdl_w_mac(struct awdl_wbuf *w, const awdl_mac *m)
{
    awdl_w_bytes(w, m->a, 6);
}

/* A TLV's header, its length filled in by tlv_end. */
static size_t tlv_begin(struct awdl_wbuf *w, uint8_t type)
{
    awdl_w_u8(w, type);
    size_t at = w->len;
    awdl_w_le16(w, 0);
    return at;
}

static void tlv_end(struct awdl_wbuf *w, size_t at)
{
    if (w->overflow)
        return;
    size_t len = w->len - at - 2;
    w->p[at] = (uint8_t)len;
    w->p[at + 1] = (uint8_t)(len >> 8);
}

/* ---------------------------------------------------------- channels, §3.4 */

uint32_t awdl_channel_freq(uint8_t ch)
{
    if (ch >= 1 && ch <= 13)
        return 2407u + 5u * ch;
    if (ch == 14)
        return 2484;
    if (ch >= 32 && ch <= 196)
        return 5000u + 5u * ch;
    return 0;
}

uint8_t awdl_channel_from_freq(uint32_t mhz)
{
    if (mhz == 2484)
        return 14;
    if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0)
        return (uint8_t)((mhz - 2407) / 5);
    if (mhz >= 5160 && mhz <= 5980 && mhz % 5 == 0)
        return (uint8_t)((mhz - 5000) / 5);
    return 0;
}

/* AWDL-10: 81 on 2.4 GHz, 128 on 5 GHz. */
uint8_t awdl_channel_opclass(uint8_t ch)
{
    return ch <= 14 ? 0x51 : 0x80;
}

bool awdl_channel_social(uint8_t ch)
{
    return ch == 6 || ch == 44 || ch == 149;
}

static size_t enc_size(uint8_t encoding)
{
    return encoding == AWDL_ENC_SIMPLE ? 1 : 2;
}

void awdl_seq_empty(struct awdl_sequence *s, uint8_t encoding)
{
    memset(s, 0, sizeof *s);
    s->encoding = encoding;
}

void awdl_seq_fixed(struct awdl_sequence *s, uint8_t channel)
{
    s->encoding = AWDL_ENC_OPCLASS;
    for (unsigned i = 0; i < AWDL_SLOTS; i++) {
        s->slots[i][0] = channel;
        s->slots[i][1] = awdl_channel_opclass(channel);
    }
}

uint8_t awdl_seq_channel(const struct awdl_sequence *s, unsigned slot)
{
    const uint8_t *b = s->slots[slot % AWDL_SLOTS];
    return s->encoding == AWDL_ENC_LEGACY ? b[1] : b[0];
}

size_t awdl_seq_wire_len(const struct awdl_sequence *s)
{
    return 6 + AWDL_SLOTS * enc_size(s->encoding);
}

/* The sequence structure the Channel Sequence TLV carries: only the shape
 * owly accepts, 16 slots, no duplicates, four AWs a slot, fill 0xffff. */
int awdl_seq_parse(const uint8_t *v, size_t len, struct awdl_sequence *s)
{
    if (len < 6)
        return -1;
    if (v[0] + 1u != AWDL_SLOTS)
        return -1;
    if (v[1] != AWDL_ENC_SIMPLE && v[1] != AWDL_ENC_LEGACY && v[1] != AWDL_ENC_OPCLASS)
        return -1;
    if (v[2] != 0 || v[3] != SEQ_STEP_COUNT || rd_le16(v + 4) != SEQ_FILL)
        return -1;
    awdl_seq_empty(s, v[1]);
    size_t size = enc_size(s->encoding);
    if (!fits(len, 6, AWDL_SLOTS * size))
        return -1;
    for (unsigned i = 0; i < AWDL_SLOTS; i++)
        memcpy(s->slots[i], v + 6 + i * size, size);
    return 0;
}

void awdl_seq_put(struct awdl_wbuf *w, const struct awdl_sequence *s)
{
    awdl_w_u8(w, AWDL_SLOTS - 1);
    awdl_w_u8(w, s->encoding);
    awdl_w_u8(w, 0);
    awdl_w_u8(w, SEQ_STEP_COUNT);
    awdl_w_le16(w, SEQ_FILL);
    size_t size = enc_size(s->encoding);
    for (unsigned i = 0; i < AWDL_SLOTS; i++)
        awdl_w_bytes(w, s->slots[i], size);
}

/* ---------------------------------------------------------- radiotap */

#define RT_FLAGS        1
#define RT_RATE         2
#define RT_CHANNEL      3
#define RT_DBM_SIGNAL   5
#define RT_TX_FLAGS     15
#define RT_MCS          19
#define RT_EXT          31
#define RT_F_FCS        0x10
#define RT_F_BADFCS     0x40
#define RT_TX_NOACK     0x0008

uint32_t awdl_crc32(const uint8_t *p, size_t n)
{
    static const uint32_t nibble[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ nibble[crc & 0xf];
        crc = (crc >> 4) ^ nibble[crc & 0xf];
    }
    return ~crc;
}

/* Some drivers (ath12k) append the FCS without setting radiotap's flag. */
static bool ends_in_fcs(const uint8_t *f, size_t n)
{
    if (n < 14)
        return false;
    return awdl_crc32(f, n - 4) == rd_le32(f + n - 4);
}

int awdl_radiotap_parse(const uint8_t *buf, size_t n, struct awdl_rx *rx)
{
    /* Alignment and size of the fields before the antenna signal, which
     * are all that's read: TSFT, flags, rate, channel, FHSS. */
    static const uint8_t field[5][2] = { { 8, 8 }, { 1, 1 }, { 1, 1 }, { 2, 4 }, { 1, 2 } };
    memset(rx, 0, sizeof *rx);
    if (n < 8 || buf[0] != 0)
        return -1;
    size_t len = rd_le16(buf + 2);
    if (len < 8 || len > n)
        return -1;
    uint32_t present = rd_le32(buf + 4);
    size_t at = 4;
    /* The data follows every present word; extensions only add fields
     * past those read here. */
    while (rd_le32(buf + at) & (1u << RT_EXT)) {
        at += 4;
        if (!fits(len, at, 4))
            return -1;
    }
    at += 4;
    uint8_t flags = 0;
    for (unsigned bit = 0; bit < 5; bit++) {
        if (!(present & (1u << bit)))
            continue;
        size_t align = field[bit][0], size = field[bit][1];
        at = (at + align - 1) / align * align;
        if (!fits(len, at, size))
            return -1;
        if (bit == RT_FLAGS)
            flags = buf[at];
        if (bit == RT_CHANNEL) {
            rx->has_freq = true;
            rx->freq = rd_le16(buf + at);
        }
        at += size;
    }
    if (present & (1u << RT_DBM_SIGNAL)) {
        if (!fits(len, at, 1))
            return -1;
        rx->has_rssi = true;
        rx->rssi = (int8_t)buf[at];
    }
    if (flags & RT_F_BADFCS)
        return -1;
    rx->frame = buf + len;
    rx->len = n - len;
    if ((flags & RT_F_FCS) || ends_in_fcs(rx->frame, rx->len)) {
        if (rx->len < 4)
            return -1;
        rx->len -= 4;
    }
    return 0;
}

size_t awdl_radiotap_tx(uint8_t *out, struct awdl_rate rate, bool no_ack)
{
    uint32_t present = 0;
    uint8_t fields[8];
    size_t n = 0;
    if (!rate.ht) {
        present |= 1u << RT_RATE;
        fields[n++] = rate.value;
        if (no_ack) {
            /* TX flags are a u16 at an even offset: the header is 8. */
            fields[n++] = 0;
            present |= 1u << RT_TX_FLAGS;
            fields[n++] = RT_TX_NOACK & 0xff;
            fields[n++] = RT_TX_NOACK >> 8;
        }
    } else {
        if (no_ack) {
            present |= 1u << RT_TX_FLAGS;
            fields[n++] = RT_TX_NOACK & 0xff;
            fields[n++] = RT_TX_NOACK >> 8;
        }
        present |= 1u << RT_MCS;
        /* Known: bandwidth, MCS, guard interval; 20 MHz. */
        fields[n++] = 0x07;
        fields[n++] = rate.sgi ? 0x04 : 0;
        fields[n++] = rate.value;
    }
    size_t len = 8 + n;
    out[0] = 0;
    out[1] = 0;
    out[2] = (uint8_t)len;
    out[3] = 0;
    out[4] = (uint8_t)present;
    out[5] = (uint8_t)(present >> 8);
    out[6] = (uint8_t)(present >> 16);
    out[7] = (uint8_t)(present >> 24);
    memcpy(out + 8, fields, n);
    return len;
}

/* ---------------------------------------------------------- 802.11 frames */

static int parse_action(const uint8_t *body, size_t n, struct awdl_rx_frame *f)
{
    if (n == 0)
        return 0;
    if (body[0] == CATEGORY_BLOCK_ACK) {
        if (n < 2)
            return -1;
        if (body[1] == ADDBA_REQUEST) {
            if (n < 7)
                return -1;
            f->kind = AWDL_RX_ADDBA;
            f->token = body[2];
            f->params = rd_le16(body + 3);
            f->timeout = rd_le16(body + 5);
            return 0;
        }
    }
    if (n < AWDL_ACTION_HEADER_LEN || body[0] != CATEGORY_VENDOR || memcmp(body + 1, OUI, 3) != 0
        || body[4] != AWDL_TYPE || body[5] != AWDL_VERSION_COMPAT)
        return 0;
    if (body[6] != AWDL_SUBTYPE_PSF && body[6] != AWDL_SUBTYPE_MIF)
        return 0;
    f->kind = AWDL_RX_ACTION;
    f->subtype = body[6];
    f->phy_tx = rd_le32(body + 8);
    f->target_tx = rd_le32(body + 12);
    f->tlvs = body + AWDL_ACTION_HEADER_LEN;
    f->tlvs_len = n - AWDL_ACTION_HEADER_LEN;
    return 0;
}

int awdl_frame_parse(const uint8_t *b, size_t n, struct awdl_rx_frame *f)
{
    memset(f, 0, sizeof *f);
    f->kind = AWDL_RX_OTHER;
    if (n < 2)
        return -1;
    uint16_t fc = rd_le16(b);
    /* Control and extension frames have no three-address header, and a
     * monitor hears plenty: not AWDL, not malformed. */
    if ((fc & FC_TYPE) != FC_TYPE_MGMT && (fc & FC_TYPE) != FC_TYPE_DATA)
        return 0;
    if (n < AWDL_HEADER_LEN)
        return -1;
    f->h.fc = fc;
    rd_mac(b + 4, &f->h.dst);
    rd_mac(b + 10, &f->h.src);
    rd_mac(b + 16, &f->h.bssid);
    f->h.seq = rd_le16(b + 22) >> 4;
    if ((fc & (FC_PROTECTED | FC_MORE_FRAGMENTS)) || (rd_le16(b + 22) & 0xf))
        return 0;
    const uint8_t *body = b + AWDL_HEADER_LEN;
    size_t blen = n - AWDL_HEADER_LEN;
    switch (fc & FC_TYPE_SUBTYPE) {
    case AWDL_FC_ACTION:
        return parse_action(body, blen, f);
    case AWDL_FC_DATA:
    case AWDL_FC_QOS_DATA:
        /* AWDL-1: data between peers on AWDL's BSSID, to or from no
         * distribution system. */
        if ((fc & (FC_TO_DS | FC_FROM_DS)) || !awdl_mac_eq(&f->h.bssid, &AWDL_BSSID))
            return 0;
        if ((fc & FC_TYPE_SUBTYPE) == AWDL_FC_QOS_DATA) {
            if (blen < 2)
                return -1;
            f->amsdu = rd_le16(body) & QOS_AMSDU;
            body += 2;
            blen -= 2;
        }
        f->kind = AWDL_RX_DATA;
        f->body = body;
        f->body_len = blen;
        return 0;
    default:
        return 0;
    }
}

static int msdu(const awdl_mac *dst, const awdl_mac *src, const uint8_t *b, size_t n,
                struct awdl_msdu *m)
{
    if (n < AWDL_DATA_HEADER_LEN)
        return -1;
    /* As owly checks it: SNAP and AWDL's protocol ID; the OUI isn't. */
    if (memcmp(b, LLC_SNAP, 3) != 0 || memcmp(b + 6, LLC_SNAP + 6, 2) != 0)
        return -1;
    m->dst = *dst;
    m->src = *src;
    m->ethertype = rd_be16(b + 14);
    m->payload = b + AWDL_DATA_HEADER_LEN;
    m->len = n - AWDL_DATA_HEADER_LEN;
    return 0;
}

int awdl_msdus(const struct awdl_hdr *h, bool amsdu, const uint8_t *body, size_t len,
               struct awdl_msdu out[AWDL_AMSDU_MAX])
{
    if (!amsdu)
        return msdu(&h->dst, &h->src, body, len, &out[0]) == 0 ? 1 : -1;
    int count = 0;
    while (len > 0) {
        if (count == (int)AWDL_AMSDU_MAX)
            return -1;
        if (len < 14)
            return -1;
        awdl_mac dst, src;
        rd_mac(body, &dst);
        rd_mac(body + 6, &src);
        size_t sub = rd_be16(body + 12);
        if (!fits(len, 14, sub))
            return -1;
        if (msdu(&dst, &src, body + 14, sub, &out[count]) != 0)
            return -1;
        count++;
        body += 14 + sub;
        len -= 14 + sub;
        if (len > 0) {
            /* Each subframe but the last is padded to 4 bytes. */
            size_t pad = (4 - (14 + sub) % 4) % 4;
            if (pad > len)
                return -1;
            body += pad;
            len -= pad;
        }
    }
    return count;
}

void awdl_put_header(struct awdl_wbuf *w, uint16_t fc, const awdl_mac *dst, const awdl_mac *src,
                     uint16_t seq)
{
    awdl_w_le16(w, fc);
    awdl_w_le16(w, 0); /* duration */
    awdl_w_mac(w, dst);
    awdl_w_mac(w, src);
    awdl_w_mac(w, &AWDL_BSSID);
    awdl_w_le16(w, (uint16_t)((seq & 0x0fff) << 4));
}

static void put_msdu(struct awdl_wbuf *w, uint16_t awdl_seq, const uint8_t *p, size_t len)
{
    awdl_w_bytes(w, LLC_SNAP, sizeof LLC_SNAP);
    awdl_w_le16(w, DATA_HEAD);
    awdl_w_le16(w, awdl_seq);
    awdl_w_le16(w, 0);
    awdl_w_be16(w, AWDL_ETHERTYPE_IPV6);
    awdl_w_bytes(w, p, len);
}

void awdl_data_frame(struct awdl_wbuf *w, const awdl_mac *src, const awdl_mac *dst, uint16_t seq,
                     uint16_t awdl_seq, const uint8_t *packet, size_t len)
{
    awdl_put_header(w, AWDL_FC_DATA, dst, src, seq);
    put_msdu(w, awdl_seq, packet, len);
}

void awdl_amsdu_frame(struct awdl_wbuf *w, const awdl_mac *src, const awdl_mac *dst, uint16_t seq,
                      uint16_t awdl_seq, const uint8_t *const *packets, const size_t *lens, size_t n)
{
    awdl_put_header(w, AWDL_FC_QOS_DATA, dst, src, seq);
    awdl_w_le16(w, QOS_AMSDU); /* QoS control, TID 0 */
    size_t start = w->len;
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            awdl_w_zeros(w, (4 - (w->len - start) % 4) % 4);
        awdl_w_mac(w, dst);
        awdl_w_mac(w, src);
        awdl_w_be16(w, (uint16_t)(AWDL_DATA_HEADER_LEN + lens[i]));
        put_msdu(w, (uint16_t)(awdl_seq + i), packets[i], lens[i]);
    }
}

/* AWDL-15: accepting a Block Ack session as owly answers it: immediate
 * Block Ack, the requested TID, no A-MSDU inside A-MPDUs, a window of at
 * most 64, the requested timeout. */
void awdl_addba_response(struct awdl_wbuf *w, const awdl_mac *src, const awdl_mac *dst,
                         uint16_t seq, uint8_t token, uint16_t requested, uint16_t timeout)
{
    uint16_t window = (requested >> 6) & 0x3ff;
    if (window == 0 || window > 64)
        window = 64;
    uint16_t params = (uint16_t)((requested & 0x003c) | 0x0002 | (window << 6));
    awdl_put_header(w, AWDL_FC_ACTION, dst, src, seq);
    awdl_w_u8(w, CATEGORY_BLOCK_ACK);
    awdl_w_u8(w, ADDBA_RESPONSE);
    awdl_w_u8(w, token);
    awdl_w_le16(w, 0); /* success */
    awdl_w_le16(w, params);
    awdl_w_le16(w, timeout);
}

/* ---------------------------------------------------------- TLVs, §3.2 */

int awdl_tlv_next(struct awdl_tlv_iter *it, struct awdl_tlv *t)
{
    if (it->left < 3) {
        /* AWDL-5: fewer than 3 bytes of zeros after the last TLV are
         * padding; anything else is a truncated TLV. */
        for (size_t i = 0; i < it->left; i++)
            if (it->p[i] != 0) {
                it->left = 0;
                return -1;
            }
        it->left = 0;
        return 0;
    }
    size_t len = rd_le16(it->p + 1);
    if (!fits(it->left, 3, len)) {
        it->left = 0;
        return -1;
    }
    t->type = it->p[0];
    t->value = it->p + 3;
    t->len = len;
    it->p += 3 + len;
    it->left -= 3 + len;
    return 1;
}

int awdl_sync_params_parse(const uint8_t *v, size_t len, struct awdl_sync_params *s)
{
    if (len < 31) /* offsets 0-30, all a reader needs */
        return -1;
    s->next_aw_channel = v[0];
    s->next_tu = rd_le16(v + 1);
    s->master_channel = v[3];
    s->aw_period = rd_le16(v + 5);
    s->af_period = rd_le16(v + 7);
    rd_mac(v + 21, &s->master);
    s->presence_mode = v[27];
    s->aw_counter = rd_le16(v + 29);
    return 0;
}

int awdl_election_v1_parse(const uint8_t *v, size_t len, struct awdl_election_params *e)
{
    if (len < 19)
        return -1;
    memset(e, 0, sizeof *e);
    e->height = v[3];
    rd_mac(v + 5, &e->master);
    e->master_metric = rd_le32(v + 11);
    e->self_metric = rd_le32(v + 15);
    return 0;
}

int awdl_election_v2_parse(const uint8_t *v, size_t len, struct awdl_election_params *e)
{
    if (len < 40)
        return -1;
    memset(e, 0, sizeof *e);
    rd_mac(v, &e->master);
    e->has_sync = true;
    rd_mac(v + 6, &e->sync);
    e->master_counter = rd_le32(v + 12);
    e->height = rd_le32(v + 16);
    e->master_metric = rd_le32(v + 20);
    e->self_metric = rd_le32(v + 24);
    e->self_counter = rd_le32(v + 36);
    return 0;
}

/* The RX MCS mask may be longer than one byte; bits past MCS 31 are
 * dropped. */
int awdl_ht_parse(const uint8_t *v, size_t len, struct awdl_ht *ht)
{
    size_t end = len >= 8 ? len - 2 : len;
    size_t mask_len = end > 5 ? end - 5 : 0;
    if (mask_len == 0)
        mask_len = 1;
    if (!fits(len, 5, mask_len))
        return -1;
    uint8_t mcs[4] = { 0 };
    memcpy(mcs, v + 5, mask_len < 4 ? mask_len : 4);
    ht->capabilities = rd_le16(v + 2);
    ht->ampdu_params = v[4];
    ht->rx_mcs = rd_le32(mcs);
    return 0;
}

#define DP_INFRA_INFO       0x0001
#define DP_INFRA_ADDRESS    0x0002
#define DP_AWDL_ADDRESS     0x0004
#define DP_COUNTRY_CODE     0x0100
#define DP_SOCIAL_CHANNELS  0x0200

/* Flags, then each optional field the flags announce, in §3.2's order. */
int awdl_data_path_parse(const uint8_t *v, size_t len, struct awdl_data_path *d)
{
    memset(d, 0, sizeof *d);
    if (len < 2)
        return -1;
    uint16_t flags = rd_le16(v);
    size_t at = 2;
    if (flags & DP_COUNTRY_CODE) {
        if (!fits(len, at, 3))
            return -1;
        d->has_country = true;
        memcpy(d->country, v + at, 3);
        at += 3;
    }
    if (flags & DP_SOCIAL_CHANNELS) {
        if (!fits(len, at, 2))
            return -1;
        at += 2;
    }
    if (flags & DP_INFRA_INFO) {
        if (!fits(len, at, 8))
            return -1;
        at += 8;
    }
    if (flags & DP_INFRA_ADDRESS) {
        if (!fits(len, at, 6))
            return -1;
        d->has_infra = true;
        rd_mac(v + at, &d->infra);
        at += 6;
    }
    if ((flags & DP_AWDL_ADDRESS) && !fits(len, at, 6))
        return -1;
    return 0;
}

/* Flags, then a length-prefixed label; longer names are cut at
 * AWDL_NAME_MAX bytes. */
int awdl_arpa_parse(const uint8_t *v, size_t len, uint8_t name[AWDL_NAME_MAX], uint8_t *name_len)
{
    if (len < 2 || !fits(len, 2, v[1]))
        return -1;
    size_t n = v[1] < AWDL_NAME_MAX ? v[1] : AWDL_NAME_MAX;
    memcpy(name, v + 2, n);
    *name_len = (uint8_t)n;
    return 0;
}

int awdl_advert_parse(const uint8_t *body, size_t len, struct awdl_advert *a)
{
    memset(a, 0, sizeof *a);
    struct awdl_tlv_iter it = { body, len };
    struct awdl_tlv t;
    int r;
    while ((r = awdl_tlv_next(&it, &t)) > 0) {
        switch (t.type) {
        case AWDL_TLV_SYNC_PARAMS:
            if (awdl_sync_params_parse(t.value, t.len, &a->sync))
                return -1;
            a->has_sync = true;
            break;
        case AWDL_TLV_ELECTION_PARAMS: {
            struct awdl_election_params e;
            if (awdl_election_v1_parse(t.value, t.len, &e))
                return -1;
            /* A node ignores type 5 from a peer that also sends 24. */
            if (!a->election_v2) {
                a->election = e;
                a->has_election = true;
            }
            break;
        }
        case AWDL_TLV_ELECTION_PARAMS_V2:
            if (awdl_election_v2_parse(t.value, t.len, &a->election))
                return -1;
            a->has_election = true;
            a->election_v2 = true;
            break;
        case AWDL_TLV_CHANNEL_SEQUENCE:
            if (awdl_seq_parse(t.value, t.len, &a->sequence))
                return -1;
            a->has_sequence = true;
            break;
        case AWDL_TLV_ARPA:
            if (awdl_arpa_parse(t.value, t.len, a->name, &a->name_len))
                return -1;
            a->has_name = true;
            break;
        case AWDL_TLV_VERSION:
            if (t.len < 2)
                return -1;
            a->version = t.value[0];
            a->devclass = t.value[1];
            a->has_version = true;
            break;
        case AWDL_TLV_HT_CAPABILITIES:
            if (awdl_ht_parse(t.value, t.len, &a->ht))
                return -1;
            a->has_ht = true;
            break;
        case AWDL_TLV_DATA_PATH_STATE:
            if (awdl_data_path_parse(t.value, t.len, &a->data_path))
                return -1;
            a->has_data_path = true;
            break;
        default:
            /* AWDL-6: unknown TLVs are skipped by length, and so are those
             * a node has no use for (the sync tree's layout isn't known). */
            break;
        }
    }
    return r < 0 ? -1 : 0;
}

/* §3.2: the fixed fields, the AW count and phase, the channel sequence and
 * 2 bytes of padding. */
static void put_sync_params(struct awdl_wbuf *w, const struct awdl_own *own)
{
    size_t at = tlv_begin(w, AWDL_TLV_SYNC_PARAMS);
    uint8_t ext = AWDL_PRESENCE_MODE - 1;
    awdl_w_u8(w, own->channel);
    awdl_w_le16(w, own->next_tu);
    awdl_w_u8(w, own->master_channel);
    awdl_w_u8(w, 0); /* guard time */
    awdl_w_le16(w, AWDL_AW_TU);
    awdl_w_le16(w, own->psf_interval_tu);
    awdl_w_le16(w, SYNC_FLAGS);
    awdl_w_le16(w, AWDL_AW_TU); /* extended AW length */
    awdl_w_le16(w, AWDL_AW_TU); /* common AW length */
    uint16_t next = own->next_tu < AWDL_SLOT_TU ? own->next_tu : AWDL_SLOT_TU;
    uint16_t elapsed = (uint16_t)(AWDL_SLOT_TU - next);
    awdl_w_le16(w, elapsed < AWDL_AW_TU ? (uint16_t)(AWDL_AW_TU - elapsed) : 0); /* remaining AW */
    uint8_t exts[4] = { ext, ext, ext, ext }; /* min ext, max ext multicast, unicast, AF */
    awdl_w_bytes(w, exts, 4);
    awdl_w_mac(w, &own->election->master);
    awdl_w_u8(w, AWDL_PRESENCE_MODE);
    awdl_w_u8(w, 0);
    awdl_w_le16(w, own->aw_counter); /* next AW sequence number */
    awdl_w_le16(w, own->aw_counter); /* AP alignment */
    awdl_seq_put(w, own->sequence);
    awdl_w_zeros(w, 2);
    tlv_end(w, at);
}

static void put_election_v1(struct awdl_wbuf *w, const struct awdl_election *e)
{
    size_t at = tlv_begin(w, AWDL_TLV_ELECTION_PARAMS);
    awdl_w_u8(w, 0);    /* flags */
    awdl_w_le16(w, 0);  /* ID */
    awdl_w_u8(w, e->height > 255 ? 255 : (uint8_t)e->height);
    awdl_w_u8(w, 0);
    awdl_w_mac(w, &e->master);
    awdl_w_le32(w, e->master_metric);
    awdl_w_le32(w, e->self_metric);
    awdl_w_zeros(w, 2);
    tlv_end(w, at);
}

static void put_channel_sequence(struct awdl_wbuf *w, const struct awdl_sequence *s)
{
    size_t at = tlv_begin(w, AWDL_TLV_CHANNEL_SEQUENCE);
    awdl_seq_put(w, s);
    awdl_w_zeros(w, 3);
    tlv_end(w, at);
}

static void put_election_v2(struct awdl_wbuf *w, const struct awdl_election *e)
{
    size_t at = tlv_begin(w, AWDL_TLV_ELECTION_PARAMS_V2);
    awdl_w_mac(w, &e->master);
    awdl_w_mac(w, &e->sync);
    awdl_w_le32(w, e->master_counter);
    awdl_w_le32(w, e->height);
    awdl_w_le32(w, e->master_metric);
    awdl_w_le32(w, e->self_metric);
    awdl_w_le32(w, 0);
    awdl_w_le32(w, 0);
    awdl_w_le32(w, e->self_counter);
    tlv_end(w, at);
}

/* All zero: a changed SUI makes peers flush their mDNS caches. */
static void put_service_params(struct awdl_wbuf *w)
{
    size_t at = tlv_begin(w, AWDL_TLV_SERVICE_PARAMS);
    awdl_w_zeros(w, 9);
    tlv_end(w, at);
}

static void put_ht(struct awdl_wbuf *w, const struct awdl_ht *ht)
{
    size_t at = tlv_begin(w, AWDL_TLV_HT_CAPABILITIES);
    awdl_w_le16(w, 0);
    awdl_w_le16(w, ht->capabilities);
    awdl_w_u8(w, ht->ampdu_params);
    awdl_w_u8(w, (uint8_t)ht->rx_mcs);
    awdl_w_le16(w, 0);
    tlv_end(w, at);
}

/* owly's flags (0x8f24): country `X0`, the master channel in the social
 * channel map, our AWDL address, 2 bytes of extended flags. */
static void put_data_path(struct awdl_wbuf *w, const struct awdl_own *own)
{
    size_t at = tlv_begin(w, AWDL_TLV_DATA_PATH_STATE);
    awdl_w_le16(w, DATA_PATH_FLAGS);
    awdl_w_bytes(w, "X0", 3);
    awdl_w_le16(w, own->master_channel == 6 ? 0x0001 : own->master_channel == 44 ? 0x0002 : 0x0004);
    awdl_w_mac(w, &own->addr);
    awdl_w_le16(w, 0);
    tlv_end(w, at);
}

/* Cut at AWDL_NAME_MAX bytes, on a UTF-8 character boundary. */
void awdl_put_arpa(struct awdl_wbuf *w, const char *name, size_t len)
{
    size_t end = len < AWDL_NAME_MAX ? len : AWDL_NAME_MAX;
    while (end > 0 && end < len && ((uint8_t)name[end] & 0xc0) == 0x80)
        end--;
    size_t at = tlv_begin(w, AWDL_TLV_ARPA);
    awdl_w_u8(w, ARPA_FLAGS);
    awdl_w_u8(w, (uint8_t)end);
    awdl_w_bytes(w, name, end);
    awdl_w_be16(w, ARPA_LOCAL_POINTER);
    tlv_end(w, at);
}

static void put_version(struct awdl_wbuf *w, uint8_t devclass)
{
    size_t at = tlv_begin(w, AWDL_TLV_VERSION);
    awdl_w_u8(w, AWDL_VERSION);
    awdl_w_u8(w, devclass);
    tlv_end(w, at);
}

void awdl_put_tlvs(struct awdl_wbuf *w, const struct awdl_own *own, bool mif)
{
    put_sync_params(w, own);
    put_election_v1(w, own->election);
    put_channel_sequence(w, own->sequence);
    put_election_v2(w, own->election);
    put_service_params(w);
    if (own->has_ht)
        put_ht(w, &own->ht);
    if (mif)
        awdl_put_arpa(w, own->name, own->name_len);
    put_data_path(w, own);
    put_version(w, own->devclass);
}

void awdl_action_frame(struct awdl_wbuf *w, const awdl_mac *src, uint16_t seq, uint8_t subtype,
                       uint32_t time, const struct awdl_own *own)
{
    awdl_put_header(w, AWDL_FC_ACTION, &AWDL_BROADCAST, src, seq);
    awdl_w_u8(w, CATEGORY_VENDOR);
    awdl_w_bytes(w, OUI, 3);
    awdl_w_u8(w, AWDL_TYPE);
    awdl_w_u8(w, AWDL_VERSION_COMPAT);
    awdl_w_u8(w, subtype);
    awdl_w_u8(w, 0);
    awdl_w_le32(w, time); /* PHY TX time */
    awdl_w_le32(w, time); /* target TX time */
    awdl_put_tlvs(w, own, subtype == AWDL_SUBTYPE_MIF);
}

/* ---------------------------------------------------------- IPv6, §3.5 and §4 */

#define IP_TCP      6
#define IP_ICMPV6   58
#define ICMPV6_ECHO_REQUEST 128
#define ICMPV6_ECHO_REPLY   129
#define HOP_LIMIT_LINK      255

/* The upper-layer message, if the next header is `protocol` with no
 * extension headers between. */
static const uint8_t *upper(const uint8_t *ip, size_t len, uint8_t protocol, size_t *ulen)
{
    if (len < AWDL_IPV6_HEADER_LEN || ip[0] >> 4 != 6 || ip[6] != protocol)
        return NULL;
    size_t n = rd_be16(ip + 4);
    if (!fits(len, AWDL_IPV6_HEADER_LEN, n))
        return NULL;
    *ulen = n;
    return ip + AWDL_IPV6_HEADER_LEN;
}

bool awdl_ip_bare_syn_to(const awdl_mac *dst, const awdl_mac *local, const uint8_t *ip, size_t len)
{
    if (!awdl_mac_eq(dst, local))
        return false;
    size_t n;
    const uint8_t *tcp = upper(ip, len, IP_TCP, &n);
    if (!tcp || n < 20)
        return false;
    uint8_t flags = tcp[13] & 0x17; /* FIN, SYN, RST, ACK */
    size_t offset = (size_t)(tcp[12] >> 4) * 4;
    return flags == 0x02 && offset >= 20 && n == offset;
}

/* The Internet checksum over the IPv6 pseudo-header and `msg`. */
static uint16_t checksum(const uint8_t src[16], const uint8_t dst[16], uint8_t next,
                         const uint8_t *msg, size_t len)
{
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)(src[i] << 8 | src[i + 1]) + (uint32_t)(dst[i] << 8 | dst[i + 1]);
    sum += (uint32_t)(len >> 16) + (uint32_t)(len & 0xffff);
    sum += next;
    for (size_t i = 0; i < len; i += 2)
        sum += (uint32_t)(msg[i] << 8 | (i + 1 < len ? msg[i + 1] : 0));
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

size_t awdl_ip_echo_request(uint8_t *out, size_t cap, const awdl_mac *src, const awdl_mac *dst,
                            uint16_t id, uint16_t seq, const uint8_t *payload, size_t plen)
{
    size_t icmp_len = 8 + plen;
    size_t total = AWDL_IPV6_HEADER_LEN + icmp_len;
    if (total > cap)
        return 0;
    uint8_t s[16], d[16];
    awdl_link_local(src, s);
    awdl_link_local(dst, d);
    uint8_t *ip = out, *icmp = out + AWDL_IPV6_HEADER_LEN;
    memset(ip, 0, AWDL_IPV6_HEADER_LEN);
    ip[0] = 0x60;
    ip[4] = (uint8_t)(icmp_len >> 8);
    ip[5] = (uint8_t)icmp_len;
    ip[6] = IP_ICMPV6;
    ip[7] = HOP_LIMIT_LINK;
    memcpy(ip + 8, s, 16);
    memcpy(ip + 24, d, 16);
    icmp[0] = ICMPV6_ECHO_REQUEST;
    icmp[1] = 0;
    icmp[2] = icmp[3] = 0;
    icmp[4] = (uint8_t)(id >> 8);
    icmp[5] = (uint8_t)id;
    icmp[6] = (uint8_t)(seq >> 8);
    icmp[7] = (uint8_t)seq;
    memcpy(icmp + 8, payload, plen);
    uint16_t sum = checksum(s, d, IP_ICMPV6, icmp, icmp_len);
    icmp[2] = (uint8_t)(sum >> 8);
    icmp[3] = (uint8_t)sum;
    return total;
}

bool awdl_ip_is_echo_reply(const uint8_t *ip, size_t len, uint16_t id)
{
    size_t n;
    const uint8_t *m = upper(ip, len, IP_ICMPV6, &n);
    return m && n >= 8 && m[0] == ICMPV6_ECHO_REPLY && rd_be16(m + 4) == id;
}

/* ---------------------------------------------------------- JSON */

/* The length of a valid UTF-8 sequence at `s`, or 0. */
static size_t utf8_valid(const uint8_t *s, size_t n)
{
    uint8_t c = s[0];
    size_t len;
    uint32_t cp;
    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf) {
        len = 2;
        cp = c & 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        cp = c & 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (len > n)
        return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return 0;
        cp = cp << 6 | (s[i] & 0x3f);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff))
        || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    return len;
}

size_t awdl_json_string(char *out, size_t cap, const uint8_t *s, size_t n)
{
    size_t o = 0;
#define EMIT(str, k)                                                                              \
    do {                                                                                          \
        if (o + (k) + 2 > cap)                                                                    \
            return 0;                                                                             \
        memcpy(out + o, (str), (k));                                                              \
        o += (k);                                                                                 \
    } while (0)
    EMIT("\"", 1);
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        size_t v = utf8_valid(s + i, n - i);
        if (v == 0) {
            EMIT("\xef\xbf\xbd", 3); /* U+FFFD */
            i++;
        } else if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            EMIT(e, 2);
            i++;
        } else if (c < 0x20 || c == 0x7f) {
            char e[7];
            snprintf(e, sizeof e, "\\u%04x", c);
            EMIT(e, 6);
            i++;
        } else {
            EMIT(s + i, v);
            i += v;
        }
    }
    EMIT("\"", 1);
#undef EMIT
    out[o] = 0;
    return o;
}
