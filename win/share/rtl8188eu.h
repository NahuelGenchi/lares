// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2014 - 2017 Jes Sorensen
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
/* The Realtek RTL8188EU, driven from userspace for AWDL (issue #109): a
 * port of Linux's rtl8xxxu (drivers/net/wireless/realtek/rtl8xxxu, 8188e.c
 * and the parts of core.c it uses), GPL-2.0-only like its source. It does
 * what lares-awdl needs of a card and nothing else: a monitor that hears
 * every frame on one channel with the chip's TSF, raw frames out at a rate
 * and retry count chosen per frame, and hardware ACKs for one address.
 *
 * The chip is reached through register reads and writes (usb.h); bulk
 * transfers are the caller's, with rtl_rx_parse and rtl_tx_build turning
 * their buffers into frames and back. rtl_rx_parse and rtl_tx_build are
 * pure, so the unit tests run them on plain data. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtl_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_TXDESC_LEN 32       /* struct rtl8xxxu_txdesc32 */
#define RTL_RXDESC_LEN 24       /* struct rtl8xxxu_rxdesc16 */
#define RTL_RX_XFER    16384u   /* a bulk IN read's buffer */
#define RTL_TX_MAX     2400u    /* descriptor and the longest frame sent */

/* Register access, so the chip code runs on a fake register file in tests:
 * usb.c's rtl_usb_read and rtl_usb_write in Lares. */
struct rtl_io {
    void *ctx;
    int (*read)(void *ctx, uint16_t addr, void *buf, uint16_t len);
    int (*write)(void *ctx, uint16_t addr, const void *buf, uint16_t len);
};

struct rtl_chip {
    struct rtl_io io;
    unsigned out_eps;               /* bulk OUT endpoints the device has */
    uint8_t out_ep[RTL_USB_MAX_OUT];
    uint8_t mac[6];                 /* the chip's own, from efuse */
    uint8_t channel;
    bool up;
    /* What efuse says and calibration found; private to rtl8188eu.c. */
    uint8_t efuse[512];             /* the logical map */
    uint8_t cck_power[6], ht40_1s_power[5];
    uint8_t crystal_cap;            /* efuse's */
    uint8_t crystal_cap_set;        /* what REG_AFE_XTAL_CTRL was given (0 at first) */
    uint8_t chip_cut;
    bool pi_enabled;                /* the LSSI in PI mode, as IQ calibration found it */
    bool failed;                    /* a register access failed */
    uint32_t adda_backup[16], mac_backup[4], bb_backup[9], bb_recovery_backup[9];
    bool iqk_done;
};

/* Brings the chip up from whatever state Windows' driver left it in, as
 * rtl8xxxu_init_device does for this chip:
 *
 *  - releases the MCU IO wrapper (register 0x1d, bit 3), which Windows'
 *    driver leaves held and without which the firmware never starts (#107);
 *  - powers on, loads `fw` (rtl8188eufw.bin, as linux-firmware ships it),
 *    the MAC, BB, AGC and RF tables, reads efuse, sets TX power, runs IQ
 *    and LC calibration, and tunes to `channel` at 20 MHz;
 *  - receives as a monitor: every data, management and control frame
 *    whatever its addresses and BSSID, with its PHY status (for the RSSI),
 *    no RX aggregation, and frames that failed their FCS dropped;
 *  - ACKs only frames addressed to `ack_addr` (REG_MACID, 0x0610: the chip
 *    ACKs that address and no other, #107);
 *  - leaves retries to each frame's descriptor (REG_RETRY_LIMIT, 0x042a, at
 *    0: rtl8xxxu's 48 delayed everything queued behind one unicast, #107).
 *
 * 0, or -1 with `err` in plain words. */
int rtl_chip_up(struct rtl_chip *c, const struct rtl_io *io, const uint8_t *out_ep, unsigned out_eps,
                const uint8_t *fw, size_t fwlen, uint8_t channel, const uint8_t ack_addr[6], char *err,
                size_t errlen);
/* Retunes (20 MHz). 0 or -1. */
int rtl_chip_set_channel(struct rtl_chip *c, uint8_t channel);
/* An RF register of path A, for checks. */
uint32_t rtl_chip_rf_read(struct rtl_chip *c, uint8_t reg);
/* The chip's 64-bit TSF (REG_TSFTR, µs). 0 or -1. */
int rtl_chip_tsf(struct rtl_chip *c, uint64_t *tsf);
/* RX off and the radio powered down, as rtl8xxxu's power off does, so
 * Windows' own driver finds the chip as it expects. */
void rtl_chip_down(struct rtl_chip *c);

/* One frame from a bulk IN transfer. */
struct rtl_rx_frame {
    const uint8_t *frame;   /* the 802.11 frame from its MAC header on, FCS stripped */
    size_t len;
    bool has_rssi;
    int8_t rssi;            /* dBm, from the PHY status */
    uint32_t tsfl;          /* the TSF's low 32 bits when it was received */
    uint8_t rate;           /* the descriptor's rate index (DESC_RATE_*) */
};

/* Walks one bulk IN transfer, which holds one or more descriptors with
 * their frames, and calls `fn` for each 802.11 frame; chip reports (C2H,
 * TX reports) and frames with CRC or ICV errors are skipped. Returns the
 * frames passed on, or -1 if the buffer is malformed (what came before is
 * still passed on). */
int rtl_rx_parse(const uint8_t *buf, size_t len,
                 void (*fn)(void *ctx, const struct rtl_rx_frame *f), void *ctx);

struct rtl_tx_opts {
    bool ht;                /* an HT MCS at 20 MHz, else a legacy rate */
    bool sgi;
    uint8_t rate;           /* legacy: radiotap's 500 kbit/s units (2, 4, 11, 12, 18, 22, 24, 36, 48, 72, 96, 108); HT: the MCS */
    bool no_ack;            /* sent once, and nobody is asked to ACK it */
    uint8_t retries;        /* when no_ack is false: retransmissions on a missing ACK */
    bool mgmt;              /* the management queue (action frames), else best effort */
};

/* Writes the TX descriptor and `frame` (from its MAC header on, no FCS)
 * into `out` (cap at least RTL_TXDESC_LEN + len), at the rate and retries
 * the options say (USE_DRIVER_RATE: the chip's own rate choice never
 * applies), with the checksum the chip wants. Returns the length to send,
 * 0 if it doesn't fit or the rate is one the chip doesn't have (HT past
 * MCS 7, a legacy value not in the list above), and the bulk OUT endpoint
 * for it in *ep. */
size_t rtl_tx_build(const struct rtl_chip *c, uint8_t *out, size_t cap, const uint8_t *frame, size_t len,
                    const struct rtl_tx_opts *o, uint8_t *ep);

#ifdef __cplusplus
}
#endif
