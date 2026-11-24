// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-only
/* The RTL8188EU driver's pure parts, RX parsing and TX descriptors, and its
 * bring-up against a fake register file (issue #109). */
#include "share_check.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "rtl8188eu.h"
}

namespace {

uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

/* ------------------------------------------------------------ TX */

/* rtl8xxxu.h's struct rtl8xxxu_txdesc32 bits, written out here so the
 * checks don't lean on the driver's own names. */
constexpr uint8_t OWN = 0x80, FIRST_SEG = 0x08, LAST_SEG = 0x04, BMC = 0x01;
constexpr uint32_t QUEUE_MGNT = 0x12u << 8, QUEUE_BE = 0x00u << 8;
constexpr uint32_t AGG_BREAK = 1u << 16, ANT_A = 1u << 24, ANT_B = 1u << 25;
constexpr uint32_t USE_DRIVER_RATE = 1u << 8, DISABLE_DATA_FB = 1u << 10, QOS = 1u << 6;
constexpr uint32_t SHORT_GI = 1u << 6, RETRY_ENABLE = 1u << 17, DATA_FB_LIMIT = 0x0001ff00u;
constexpr uint16_t ANT_C = 0x2000;      /* TXDESC_ANTENNA_SELECT_C >> 16, in txdw7 */

struct want_desc {
    uint16_t pkt_size;
    uint8_t txdw0;
    uint32_t txdw1, txdw2, txdw3, txdw4, txdw5, txdw6;
    uint16_t txdw7;
};

void check_desc(const uint8_t *d, const want_desc &w)
{
    CHECK(le16(d) == w.pkt_size);
    CHECK(d[2] == RTL_TXDESC_LEN);
    CHECK(d[3] == w.txdw0);
    CHECK(le32(d + 4) == w.txdw1);
    CHECK(le32(d + 8) == w.txdw2);
    CHECK(le32(d + 12) == w.txdw3);
    CHECK(le32(d + 16) == w.txdw4);
    CHECK(le32(d + 20) == w.txdw5);
    CHECK(le32(d + 24) == w.txdw6);
    CHECK(le16(d + 30) == w.txdw7);
    /* rtl8xxxu_calc_tx_desc_csum: the XOR of the 16 words, its own at 0 */
    uint16_t x = 0;
    for (int i = 0; i < RTL_TXDESC_LEN; i += 2)
        if (i != 28) x = static_cast<uint16_t>(x ^ le16(d + i));
    CHECK(le16(d + 28) == x);
    uint16_t all = 0;
    for (int i = 0; i < RTL_TXDESC_LEN; i += 2) all = static_cast<uint16_t>(all ^ le16(d + i));
    CHECK(all == 0);
}

/* An 802.11 header: frame control, duration, three addresses, sequence. */
std::vector<uint8_t> frame(uint8_t fc0, uint8_t fc1, const uint8_t a1[6], const uint8_t a3[6], uint16_t seq_ctrl,
                           size_t body)
{
    static const uint8_t a2[6] = { 0x02, 0x10, 0x20, 0x30, 0x40, 0x50 };
    std::vector<uint8_t> f(24 + body);
    f[0] = fc0;
    f[1] = fc1;
    std::memcpy(&f[4], a1, 6);
    std::memcpy(&f[10], a2, 6);
    std::memcpy(&f[16], a3, 6);
    put16(&f[22], seq_ctrl);
    for (size_t i = 0; i < body; i++) f[24 + i] = static_cast<uint8_t>(0xa0 + i);
    return f;
}

rtl_chip chip_with_eps(unsigned n)
{
    rtl_chip c;
    std::memset(&c, 0, sizeof c);
    c.out_eps = n;
    for (unsigned i = 0; i < n; i++) c.out_ep[i] = static_cast<uint8_t>(0x02 + i);
    return c;
}

void tx_tests()
{
    static const uint8_t peer[6] = { 0x02, 0xaa, 0x00, 0x11, 0x22, 0x33 };
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t out[RTL_TX_MAX];
    uint8_t ep = 0;
    rtl_chip c2 = chip_with_eps(2);

    /* An action frame (management, subtype 13) to one peer, sent once with
     * no ACK at 6 Mbit/s on the management queue. rtl8xxxu's v3 would
     * write the same but: retries 6 and no BMC for a unicast DA, and no
     * DISABLE_DATA_FB. */
    {
        std::vector<uint8_t> f = frame(0xd0, 0x00, peer, peer, 0x1230, 16);
        rtl_tx_opts o = {};
        o.rate = 12;
        o.no_ack = true;
        o.retries = 7;          /* ignored: no ACK, no retries */
        o.mgmt = true;
        size_t n = rtl_tx_build(&c2, out, sizeof out, f.data(), f.size(), &o, &ep);
        CHECK(n == RTL_TXDESC_LEN + f.size());
        want_desc w = {};
        w.pkt_size = static_cast<uint16_t>(f.size());
        w.txdw0 = OWN | FIRST_SEG | LAST_SEG | BMC;
        w.txdw1 = QUEUE_MGNT;
        w.txdw2 = AGG_BREAK | ANT_A | ANT_B;
        w.txdw3 = 0x123u << 16;
        w.txdw4 = USE_DRIVER_RATE | DISABLE_DATA_FB;
        w.txdw5 = 0x04 /* DESC_RATE_6M */ | RETRY_ENABLE | 0u << 18;
        w.txdw7 = ANT_C;
        check_desc(out, w);
        CHECK(std::memcmp(out + RTL_TXDESC_LEN, f.data(), f.size()) == 0);
        CHECK(ep == 0x02);      /* two endpoints: management on the first */
    }

    /* A unicast QoS data frame at 24 Mbit/s with 5 retries, best effort. */
    std::vector<uint8_t> qos = frame(0x88, 0x00, peer, peer, 0x0450, 26);
    {
        rtl_tx_opts o = {};
        o.rate = 48;
        o.retries = 5;
        size_t n = rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &o, &ep);
        CHECK(n == RTL_TXDESC_LEN + qos.size());
        want_desc w = {};
        w.pkt_size = static_cast<uint16_t>(qos.size());
        w.txdw0 = OWN | FIRST_SEG | LAST_SEG;
        w.txdw1 = QUEUE_BE;
        w.txdw2 = AGG_BREAK | ANT_A | ANT_B;
        w.txdw3 = 0x45u << 16;
        w.txdw4 = USE_DRIVER_RATE | DISABLE_DATA_FB | QOS;
        w.txdw5 = 0x08 /* DESC_RATE_24M */ | DATA_FB_LIMIT | RETRY_ENABLE | 5u << 18;
        w.txdw7 = ANT_C;
        check_desc(out, w);
        CHECK(ep == 0x03);      /* best effort on the second */

        o.retries = 200;        /* the field holds 6 bits */
        rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &o, &ep);
        CHECK((le32(out + 20) >> 18 & 0x3f) == 63);
    }

    /* The endpoint rtl8xxxu_init_queue_priority gives each queue. */
    {
        rtl_tx_opts be = {}, mg = {};
        be.rate = 2;
        mg.rate = 2;
        mg.mgmt = true;
        rtl_chip c1 = chip_with_eps(1), c3 = chip_with_eps(3), c0 = chip_with_eps(0);
        CHECK(rtl_tx_build(&c1, out, sizeof out, qos.data(), qos.size(), &be, &ep) && ep == 0x02);
        CHECK(rtl_tx_build(&c1, out, sizeof out, qos.data(), qos.size(), &mg, &ep) && ep == 0x02);
        CHECK(rtl_tx_build(&c3, out, sizeof out, qos.data(), qos.size(), &be, &ep) && ep == 0x04);
        CHECK(rtl_tx_build(&c3, out, sizeof out, qos.data(), qos.size(), &mg, &ep) && ep == 0x02);
        CHECK(rtl_tx_build(&c0, out, sizeof out, qos.data(), qos.size(), &be, &ep) == 0);
    }

    /* HT MCS 7 with a short guard interval, plain data from a station
     * (ToDS) to a group address: BMC as rtl8xxxu sets it for one, even with
     * an ACK asked for. */
    {
        std::vector<uint8_t> f = frame(0x08, 0x01, peer, bcast, 0x0010, 40);
        rtl_tx_opts o = {};
        o.ht = true;
        o.sgi = true;
        o.rate = 7;
        o.retries = 2;
        size_t n = rtl_tx_build(&c2, out, sizeof out, f.data(), f.size(), &o, &ep);
        CHECK(n == RTL_TXDESC_LEN + f.size());
        want_desc w = {};
        w.pkt_size = static_cast<uint16_t>(f.size());
        w.txdw0 = OWN | FIRST_SEG | LAST_SEG | BMC;
        w.txdw1 = QUEUE_BE;
        w.txdw2 = AGG_BREAK | ANT_A | ANT_B;
        w.txdw3 = 0x001u << 16;
        w.txdw4 = USE_DRIVER_RATE | DISABLE_DATA_FB;
        w.txdw5 = 0x13 /* DESC_RATE_MCS7 */ | SHORT_GI | DATA_FB_LIMIT | RETRY_ENABLE | 2u << 18;
        w.txdw7 = ANT_C;
        check_desc(out, w);
    }

    /* MCS 0; a short guard interval asked for a legacy rate is dropped. */
    {
        rtl_tx_opts o = {};
        o.ht = true;
        o.rate = 0;
        CHECK(rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &o, &ep));
        CHECK((le32(out + 20) & 0x7f) == 0x0c);
        o.ht = false;
        o.sgi = true;
        o.rate = 108;
        CHECK(rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &o, &ep));
        CHECK((le32(out + 20) & 0x7f) == 0x0b);
    }

    /* Every legacy rate, radiotap's 500 kbit/s units to DESC_RATE_*. */
    {
        static const uint8_t units[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
        for (uint8_t i = 0; i < 12; i++) {
            rtl_tx_opts o = {};
            o.rate = units[i];
            CHECK(rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &o, &ep) == RTL_TXDESC_LEN + qos.size());
            CHECK((le32(out + 20) & 0x3f) == static_cast<uint32_t>(i));
        }
        rtl_tx_opts bad = {};
        bad.rate = 3;
        CHECK(rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &bad, &ep) == 0);
        bad.ht = true;
        bad.rate = 8;           /* the 8188e has one stream */
        CHECK(rtl_tx_build(&c2, out, sizeof out, qos.data(), qos.size(), &bad, &ep) == 0);
    }

    /* What doesn't fit. */
    {
        rtl_tx_opts o = {};
        o.rate = 2;
        CHECK(rtl_tx_build(&c2, out, RTL_TXDESC_LEN + qos.size() - 1, qos.data(), qos.size(), &o, &ep) == 0);
        CHECK(rtl_tx_build(&c2, out, sizeof out, qos.data(), 9, &o, &ep) == 0);
    }
}

/* ------------------------------------------------------------ RX */

struct rx_seen {
    std::vector<uint8_t> bytes;
    bool has_rssi;
    int rssi;
    uint32_t tsfl;
    uint8_t rate;
};

void collect(void *ctx, const rtl_rx_frame *f)
{
    auto *v = static_cast<std::vector<rx_seen> *>(ctx);
    rx_seen s;
    s.bytes.assign(f->frame, f->frame + f->len);
    s.has_rssi = f->has_rssi;
    s.rssi = f->rssi;
    s.tsfl = f->tsfl;
    s.rate = f->rate;
    v->push_back(s);
}

struct rx_pkt {
    unsigned drvinfo_sz = 4;    /* the PHY status, 8-byte units */
    unsigned shift = 0;
    bool phy = true, crc = false, icv = false;
    unsigned pkt_cnt = 1, rxmcs = 4, rpt_sel = 0;
    uint32_t tsfl = 0;
    uint8_t pwdb = 0, cck_agc = 0;
    std::vector<uint8_t> body;  /* the 802.11 frame, without FCS */
};

/* Appends one struct rtl8xxxu_rxdesc16, its PHY status, shift and frame
 * with an FCS at `at`, which is 128-aligned as the chip aggregates. */
size_t put_rx(std::vector<uint8_t> &buf, const rx_pkt &p)
{
    size_t at = (buf.size() + 127) / 128 * 128;
    unsigned pkt_len = static_cast<unsigned>(p.body.size()) + (p.rpt_sel ? 0u : 4u);
    size_t drvinfo = p.drvinfo_sz * 8u;
    buf.resize(at + RTL_RXDESC_LEN + drvinfo + p.shift + pkt_len, 0);
    uint8_t *d = &buf[at];
    uint32_t w0 = (pkt_len & 0x3fff) | (p.crc ? 1u << 14 : 0u) | (p.icv ? 1u << 15 : 0u) | (p.drvinfo_sz & 0xf) << 16 |
                  (p.shift & 3) << 24 | (p.phy ? 1u << 26 : 0u) | 1u << 28 | 1u << 29;
    put32(d, w0);
    put32(d + 8, (p.pkt_cnt & 0xff) << 16);
    put32(d + 12, (p.rxmcs & 0x3f) | (p.rpt_sel & 3) << 14);
    put32(d + 20, p.tsfl);
    uint8_t *phy = d + RTL_RXDESC_LEN;
    if (drvinfo >= 6) {
        phy[4] = p.pwdb;        /* cck_sig_qual_ofdm_pwdb_all */
        phy[5] = p.cck_agc;     /* cck_agc_rpt_ofdm_cfosho_a */
    }
    uint8_t *f = phy + drvinfo + p.shift;
    if (!p.body.empty()) std::memcpy(f, p.body.data(), p.body.size());
    if (!p.rpt_sel) put32(f + p.body.size(), 0xdeadbeef);  /* the FCS, whatever it holds */
    return at;
}

std::vector<uint8_t> body(size_t n, uint8_t seed)
{
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>(seed + i);
    b[0] = 0xd0;                /* an action frame */
    return b;
}

void rx_tests()
{
    /* One OFDM frame, PHY status appended. */
    {
        std::vector<uint8_t> buf;
        rx_pkt p;
        p.rxmcs = 0x04;         /* 6M */
        p.pwdb = 0x50;          /* (0x50 >> 1) - 110 = -70 dBm */
        p.tsfl = 0x12345678;
        p.body = body(30, 1);
        put_rx(buf, p);
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(buf.data(), buf.size(), collect, &seen) == 1);
        CHECK(seen.size() == 1);
        if (seen.size() == 1) {
            CHECK(seen[0].bytes == p.body);     /* FCS stripped */
            CHECK(seen[0].has_rssi && seen[0].rssi == -70);
            CHECK(seen[0].tsfl == 0x12345678);
            CHECK(seen[0].rate == 0x04);
        }
    }

    /* A CCK frame after two shift bytes: RSSI from LNA 5 (-15) and VGA 20. */
    {
        std::vector<uint8_t> buf;
        rx_pkt p;
        p.rxmcs = 0x00;         /* 1M */
        p.shift = 2;
        p.cck_agc = 5 << 5 | 20;
        p.body = body(24, 7);
        put_rx(buf, p);
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(buf.data(), buf.size(), collect, &seen) == 1);
        CHECK(seen.size() == 1 && seen[0].bytes == p.body && seen[0].rssi == -15 - 2 * 20);
    }

    /* No PHY status: no RSSI. */
    {
        std::vector<uint8_t> buf;
        rx_pkt p;
        p.drvinfo_sz = 0;
        p.phy = false;
        p.body = body(24, 3);
        put_rx(buf, p);
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(buf.data(), buf.size(), collect, &seen) == 1);
        CHECK(seen.size() == 1 && !seen[0].has_rssi && seen[0].bytes == p.body);
    }

    /* Two aggregated frames, the second at the next 128-byte boundary. */
    rx_pkt a, b;
    a.pkt_cnt = 2;
    a.tsfl = 1000;
    a.body = body(60, 0x10);
    b.pkt_cnt = 0;              /* only the first descriptor's count is read */
    b.tsfl = 2000;
    b.rxmcs = 0x13;
    b.pwdb = 0x60;
    b.body = body(100, 0x40);
    {
        std::vector<uint8_t> buf;
        put_rx(buf, a);
        size_t second = put_rx(buf, b);
        CHECK(second == 128);   /* 24 + 32 + 64 = 120, rounded up */
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(buf.data(), buf.size(), collect, &seen) == 2);
        CHECK(seen.size() == 2);
        if (seen.size() == 2) {
            CHECK(seen[0].bytes == a.body && seen[0].tsfl == 1000);
            CHECK(seen[1].bytes == b.body && seen[1].tsfl == 2000 && seen[1].rate == 0x13);
            CHECK(seen[1].rssi == (0x60 >> 1) - 110);
        }

        /* Cut inside the second frame: the first still comes out, then -1. */
        std::vector<rx_seen> cut;
        CHECK(rtl_rx_parse(buf.data(), second + RTL_RXDESC_LEN + 32 + 50, collect, &cut) == -1);
        CHECK(cut.size() == 1 && cut[0].bytes == a.body);
        /* Cut inside the second descriptor. */
        cut.clear();
        CHECK(rtl_rx_parse(buf.data(), second + 10, collect, &cut) == -1);
        CHECK(cut.size() == 1);
        /* The count says two, but the transfer ends with the first. */
        cut.clear();
        CHECK(rtl_rx_parse(buf.data(), second, collect, &cut) == 1);
    }

    /* A C2H report (rpt_sel) before a frame: skipped. */
    {
        std::vector<uint8_t> buf;
        rx_pkt r;
        r.pkt_cnt = 2;
        r.rpt_sel = 2;          /* TX report type 2 */
        r.drvinfo_sz = 0;
        r.phy = false;
        r.body = std::vector<uint8_t>(16, 0x77);
        put_rx(buf, r);
        put_rx(buf, b);
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(buf.data(), buf.size(), collect, &seen) == 1);
        CHECK(seen.size() == 1 && seen[0].bytes == b.body);
    }

    /* Frames with a CRC or ICV error: skipped, and the walk goes on. */
    {
        std::vector<uint8_t> buf;
        rx_pkt bad = a;
        bad.pkt_cnt = 3;
        bad.crc = true;
        rx_pkt icv = a;
        icv.icv = true;
        put_rx(buf, bad);
        put_rx(buf, icv);
        put_rx(buf, b);
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(buf.data(), buf.size(), collect, &seen) == 1);
        CHECK(seen.size() == 1 && seen[0].bytes == b.body);
    }

    /* Shorter than a descriptor. */
    {
        uint8_t tiny[10] = {};
        std::vector<rx_seen> seen;
        CHECK(rtl_rx_parse(tiny, sizeof tiny, collect, &seen) == -1);
        CHECK(seen.empty());
    }
}

/* ------------------------------------------------------------ bring-up */

/* A 64 KiB register file that answers what rtl_chip_up polls for: power
 * ready, the MAC enable bit clearing itself, efuse reads done, LLT writes
 * done, the firmware's checksum, and the firmware ready, but only once the
 * MCU IO wrapper (0x1d, bit 3) is released, as #107 found. RF registers
 * are kept behind the LSSI write (0x840) and the HSSI read edge (0x824). */
struct fake_chip {
    std::vector<uint8_t> mem = std::vector<uint8_t>(65536, 0);
    uint8_t efuse[512];         /* physical */
    uint32_t rf[256] = {};
    uint8_t rf_addr = 0;
    struct reg_write { uint16_t addr, len; uint32_t val; };
    std::vector<reg_write> log;
    long wrapper_released_at = -1, first_fw_write = -1;
    bool fw_written_while_held = false;
    size_t fw_bytes = 0;
    unsigned fw_block_max = 0;
    bool ignore_wrapper = false, fail_reads = false;
};

uint8_t peek(const fake_chip &f, uint16_t a)
{
    uint8_t v = f.mem[a];
    switch (a) {
    case 0x05: v = static_cast<uint8_t>(v & ~0x01); break;     /* APS_FSMCO bit 8, MAC enable, clears itself */
    case 0x06: v = static_cast<uint8_t>(v | 0x02); break;      /* bit 17: power ready */
    case 0x30: v = f.efuse[(f.mem[0x32] & 3) << 8 | f.mem[0x31]]; break;
    case 0x33: v = static_cast<uint8_t>(v | 0x80); break;      /* efuse read done */
    case 0x80:
        v = static_cast<uint8_t>(v | 0x04);                    /* checksum reported */
        if (f.mem[0x1d] & 0x08) v = static_cast<uint8_t>(v | 0x40);   /* firmware ready */
        break;
    case 0x1e3: v = static_cast<uint8_t>(v & 0x3f); break;     /* LLT op inactive */
    case 0x286: v = static_cast<uint8_t>(v | 0x02); break;     /* RXDMA idle */
    default: break;
    }
    if ((a >= 0x8a0 && a < 0x8a4) || (a >= 0x8b8 && a < 0x8bc))
        v = static_cast<uint8_t>(f.rf[f.rf_addr] >> (8 * (a & 3)));
    return v;
}

int fake_read(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
    auto *f = static_cast<fake_chip *>(ctx);
    if (f->fail_reads) return -1;
    auto *b = static_cast<uint8_t *>(buf);
    for (uint16_t i = 0; i < len; i++) b[i] = peek(*f, static_cast<uint16_t>(addr + i));
    return 0;
}

int fake_write(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
    auto *f = static_cast<fake_chip *>(ctx);
    auto *b = static_cast<const uint8_t *>(buf);
    uint32_t val = 0;
    for (uint16_t i = 0; i < len && i < 4; i++) val |= static_cast<uint32_t>(b[i]) << (8 * i);
    long at = static_cast<long>(f->log.size());
    f->log.push_back({ addr, len, val });
    if (addr >= 0x1000 && addr < 0x2000) {
        if (f->first_fw_write < 0) f->first_fw_write = at;
        if (!(f->mem[0x1d] & 0x08)) f->fw_written_while_held = true;
        f->fw_bytes += len;
        if (len > f->fw_block_max) f->fw_block_max = len;
    }
    uint32_t hssi_was = le32(&f->mem[0x824]);
    for (uint16_t i = 0; i < len; i++) {
        uint16_t a = static_cast<uint16_t>(addr + i);
        if (a == 0x1d && f->ignore_wrapper) continue;
        f->mem[a] = b[i];
    }
    if (addr == 0x1d && (b[0] & 0x08) && !f->ignore_wrapper && f->wrapper_released_at < 0) f->wrapper_released_at = at;
    if (addr == 0x840 && len == 4) f->rf[(val >> 20) & 0xff] = val & 0xfffff;
    if (addr == 0x824 && len == 4 && (val & 0x80000000u) && !(hssi_was & 0x80000000u))
        f->rf_addr = static_cast<uint8_t>((val >> 23) & 0xff);
    return 0;
}

const uint8_t EFUSE_MAC[6] = { 0x02, 0x00, 0x5e, 0x10, 0x20, 0x30 };
const uint8_t ACK_ADDR[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
const uint8_t CCK[6] = { 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f };
const uint8_t HT40[5] = { 0x20, 0x21, 0x22, 0x23, 0x24 };

/* The logical efuse map packed as the chip stores it: per 8-byte section
 * a header (extended past section 15) with a mask of the absent words,
 * then the present words. */
void efuse_pack(const uint8_t map[512], uint8_t phys[512])
{
    std::memset(phys, 0xff, 512);
    size_t at = 0;
    for (unsigned sec = 0; sec < 64; sec++) {
        uint8_t mask = 0;
        for (unsigned w = 0; w < 4; w++)
            if (map[sec * 8 + w * 2] == 0xff && map[sec * 8 + w * 2 + 1] == 0xff) mask = static_cast<uint8_t>(mask | 1u << w);
        if (mask == 0x0f) continue;
        if (sec < 16) {
            phys[at++] = static_cast<uint8_t>(sec << 4 | mask);
        } else {
            phys[at++] = static_cast<uint8_t>((sec & 7) << 5 | 0x0f);
            phys[at++] = static_cast<uint8_t>((sec >> 3) << 4 | mask);
        }
        for (unsigned w = 0; w < 4; w++) {
            if (mask & (1u << w)) continue;
            phys[at++] = map[sec * 8 + w * 2];
            phys[at++] = map[sec * 8 + w * 2 + 1];
        }
    }
}

void fake_init(fake_chip &f, bool good_id = true)
{
    uint8_t map[512];
    std::memset(map, 0xff, sizeof map);
    map[0] = good_id ? 0x29 : 0x00;     /* rtl_id 0x8129 */
    map[1] = 0x81;
    std::memcpy(map + 0x10, CCK, 6);
    std::memcpy(map + 0x16, HT40, 5);
    map[0xb9] = 0x5a;                   /* xtal_k: cap 0x1a */
    std::memcpy(map + 0xd7, EFUSE_MAC, 6);
    efuse_pack(map, f.efuse);

    f.mem[0x100] = 0xea;                /* REG_CR: MAC off */
    f.mem[0x1d] = 0x00;                 /* MCU IO wrapper held, as Windows' driver leaves it */
    f.mem[0x42a] = 0x30;                /* REG_RETRY_LIMIT */
    f.mem[0x42b] = 0x30;
    f.mem[0xfe55] = 0x08;               /* USB aggregation on */
    f.mem[0x10c] = 0x04;                /* RX DMA aggregation on */
    put32(&f.mem[0x608], 0x000000c0);   /* REG_RCR with the BSSID checks */
    std::memcpy(&f.mem[0x610], EFUSE_MAC, 6);
    put32(&f.mem[0xc80], 0x40000100);   /* REG_OFDM0_XA_TX_IQ_IMBALANCE */
}

std::vector<uint8_t> firmware(uint16_t signature)
{
    std::vector<uint8_t> fw(32 + 5000);
    put16(&fw[0], signature);
    for (size_t i = 32; i < fw.size(); i++) fw[i] = static_cast<uint8_t>(i * 7);
    return fw;
}

void chip_tests()
{
    static const uint8_t eps[2] = { 0x02, 0x03 };
    std::vector<uint8_t> fw = firmware(0x88e1);

    {
        fake_chip f;
        fake_init(f);
        rtl_io io = { &f, fake_read, fake_write };
        rtl_chip c;
        char err[160] = "";
        int r = rtl_chip_up(&c, &io, eps, 2, fw.data(), fw.size(), 6, ACK_ADDR, err, sizeof err);
        if (r) std::fprintf(stderr, "rtl_chip_up: %s\n", err);
        CHECK(r == 0);
        CHECK(c.up && c.channel == 6);
        CHECK(std::memcmp(c.mac, EFUSE_MAC, 6) == 0);

        /* The MCU IO wrapper released before anything reached the firmware
         * window, and the firmware there in 196-byte blocks. */
        CHECK(f.wrapper_released_at >= 0);
        CHECK(f.first_fw_write > f.wrapper_released_at);
        CHECK(!f.fw_written_while_held);
        CHECK(f.mem[0x1d] & 0x08);
        CHECK(f.fw_bytes == fw.size() - 32);
        CHECK(f.fw_block_max == 196);
        std::vector<unsigned> pages;
        for (const auto &w : f.log)
            if (w.addr == 0x82 && w.len == 1) pages.push_back(w.val & 7);
        CHECK(pages.size() == 2 && pages[0] == 0 && pages[1] == 1);

        /* Retries per frame, the ACK address, no RX aggregation. */
        bool retry_written = false;
        for (const auto &w : f.log)
            if (w.addr == 0x42a) retry_written = true;
        CHECK(retry_written && le16(&f.mem[0x42a]) == 0);
        CHECK(std::memcmp(&f.mem[0x610], ACK_ADDR, 6) == 0);
        CHECK(!(f.mem[0xfe55] & 0x08));
        CHECK(!(f.mem[0x10c] & 0x04));

        /* The monitor: every unicast, no BSSID checks, control and
         * management frames, PHY status; every subtype; 32 bytes of it. */
        uint32_t rcr = le32(&f.mem[0x608]);
        CHECK(rcr & 0x1);                       /* ACCEPT_AP */
        CHECK(!(rcr & 0xc0));                   /* CHECK_BSSID_MATCH, _BEACON */
        CHECK(!(rcr & 0x100));                  /* no CRC32 errors asked for */
        CHECK((rcr & 0x3000) == 0x3000);        /* control and management */
        CHECK(rcr & (1u << 28));                /* APPEND_PHYSTAT */
        CHECK(le16(&f.mem[0x6a0]) == 0xffff && le16(&f.mem[0x6a2]) == 0xffff && le16(&f.mem[0x6a4]) == 0xffff);
        CHECK(f.mem[0x60f] == 4);
        CHECK((le16(&f.mem[0x100]) & 0xc0) == 0xc0);  /* MAC TX and RX on */
        CHECK((f.mem[0x102] & 0x3) == 0);       /* link type none */
        CHECK((f.mem[0x550] & 0x18) == 0x18);   /* TSF counting, never synced to beacons */

        /* Channel 6 at 20 MHz, and its TX power (group 2). */
        CHECK((f.rf[0x18] & 0x3ff) == 6);
        CHECK((f.rf[0x18] & 0xc00) == 0xc00);
        CHECK(f.mem[0x603] & 0x04);             /* BW_OPMODE_20MHZ */
        CHECK(!(le32(&f.mem[0x800]) & 1) && !(le32(&f.mem[0x900]) & 1));
        CHECK(le32(&f.mem[0xe00]) == 0x22222222u);
        CHECK(le32(&f.mem[0xe10]) == 0x22222222u);
        CHECK((le32(&f.mem[0xe08]) >> 8 & 0xff) == 0x2c);
        CHECK((le32(&f.mem[0x24]) >> 11 & 0x3f) == 0x1a && (le32(&f.mem[0x24]) >> 17 & 0x3f) == 0x1a);

        /* The TSF, both words. */
        put32(&f.mem[0x560], 0xbcdef012);
        put32(&f.mem[0x564], 0x0000000a);
        uint64_t tsf = 0;
        CHECK(rtl_chip_tsf(&c, &tsf) == 0 && tsf == 0x0000000abcdef012ull);

        /* Retuning. */
        CHECK(rtl_chip_set_channel(&c, 11) == 0 && c.channel == 11);
        CHECK((f.rf[0x18] & 0x3ff) == 11 && le32(&f.mem[0xe00]) == 0x23232323u);
        CHECK(rtl_chip_set_channel(&c, 15) == -1 && c.channel == 11);

        /* Down: RX filters closed, RF off. */
        rtl_chip_down(&c);
        CHECK(!c.up);
        CHECK(le16(&f.mem[0x6a0]) == 0 && le16(&f.mem[0x6a4]) == 0);
        CHECK(!(f.mem[0x1f] & 0x01));
    }

    /* The wrapper held (writes to 0x1d lost): the firmware never starts. */
    {
        fake_chip f;
        fake_init(f);
        f.ignore_wrapper = true;
        rtl_io io = { &f, fake_read, fake_write };
        rtl_chip c;
        char err[160] = "";
        CHECK(rtl_chip_up(&c, &io, eps, 2, fw.data(), fw.size(), 6, ACK_ADDR, err, sizeof err) == -1);
        CHECK(!c.up && std::string(err).find("firmware") != std::string::npos);
    }

    /* Firmware that isn't: refused before any register is written. */
    {
        fake_chip f;
        fake_init(f);
        std::vector<uint8_t> bad = firmware(0x1234);
        rtl_io io = { &f, fake_read, fake_write };
        rtl_chip c;
        char err[160] = "";
        CHECK(rtl_chip_up(&c, &io, eps, 2, bad.data(), bad.size(), 6, ACK_ADDR, err, sizeof err) == -1);
        CHECK(err[0] && f.log.empty());
    }

    /* An efuse without the 8188e's ID. */
    {
        fake_chip f;
        fake_init(f, false);
        rtl_io io = { &f, fake_read, fake_write };
        rtl_chip c;
        char err[160] = "";
        CHECK(rtl_chip_up(&c, &io, eps, 2, fw.data(), fw.size(), 6, ACK_ADDR, err, sizeof err) == -1);
        CHECK(std::string(err).find("efuse") != std::string::npos);
    }

    /* Register reads that fail. */
    {
        fake_chip f;
        fake_init(f);
        f.fail_reads = true;
        rtl_io io = { &f, fake_read, fake_write };
        rtl_chip c;
        char err[160] = "";
        CHECK(rtl_chip_up(&c, &io, eps, 2, fw.data(), fw.size(), 6, ACK_ADDR, err, sizeof err) == -1);
        CHECK(err[0] != 0);
    }
}

} // namespace

void run_rtl_tests()
{
    tx_tests();
    rx_tests();
    chip_tests();
}
