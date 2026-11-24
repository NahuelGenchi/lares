// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2014 - 2017 Jes Sorensen
// SPDX-FileCopyrightText: 2007 - 2011 Realtek Corporation
// SPDX-FileCopyrightText: 2022 Bitterblue Smith
// SPDX-License-Identifier: GPL-2.0-only
/* The RTL8188EU from userspace (issue #109): Linux rtl8xxxu's 8188e path,
 * at commit 2f77c66694108785a1f6ae6c5de06453e7f7a91f, with
 * rtl8xxxu_fileops resolved to rtl8188eu_fops: 8188e.c, the parts of core.c
 * it calls, and 8188f.c's TX power and crystal cap, which the 8188e uses.
 * Each function says which rtl8xxxu function it ports.
 *
 * Left out, since a monitor that injects at a chosen rate doesn't need
 * them: mac80211, rate adaptation (the 8188e's is in the driver, not the
 * firmware), H2C commands, power save, beacons, Bluetooth coexistence, the
 * LED, CFO tracking, stations, keys and aggregation sessions. */
#include "rtl8188eu.h"
#include "rtl8188eu_regs.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

/* rtl8xxxu.h and rtl8188eu_fops */
#define MAX_REG_POLL            500
#define FIRMWARE_POLL_MAX       1000
#define FW_PAGE_SIZE            4096u
#define FW_HEADER_LEN           32u     /* struct rtl8xxxu_firmware_header */
#define WRITEN_BLOCK_SIZE       196u    /* .writeN_block_size */
#define TRXFF_BOUNDARY          0x25ff  /* .trxff_boundary: 9 KiB of RX buffer */
#define TOTAL_PAGE_NUM          0xa9    /* TX_TOTAL_PAGE_NUM_8188E */
#define PAGE_NUM_HI             0x29
#define PAGE_NUM_LO             0x1c
#define PAGE_NUM_NORM           0x1c
#define LAST_LLT_ENTRY          175
#define ADDA_1T_INIT            0x0b1b25a0u
#define ADDA_1T_PATH_ON         0x0bdb25a0u
#define EFUSE_MAP_LEN           512u
#define EFUSE_REAL_CONTENT_LEN  512u    /* EFUSE_REAL_CONTENT_LEN_8723A, which rtl8xxxu reads on every chip */
#define EFUSE_MAX_WORD_UNIT     4
#define ADDA_REGS               16
#define MAC_REGS                4
#define BB_REGS                 9
#define MAX_TOLERANCE           5

/* struct rtl8188eu_efuse */
#define EFUSE_RTL_ID            0x00
#define EFUSE_CCK_BASE          0x10
#define EFUSE_HT40_BASE         0x16
#define EFUSE_XTAL_K            0xb9
#define EFUSE_MAC_ADDR          0xd7

#define RX_PKT_ALIGN            128u    /* rtl8xxxu_parse_rxdesc16 rounds each packet up to it */
#define FCS_LEN                 4u

static const char IO_FAILED[] = "The Wi-Fi adapter stopped answering its register reads and writes.";

/* ------------------------------------------------------------ helpers */

static uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* u32p_replace_bits */
static uint32_t replace_bits(uint32_t v, uint32_t field, uint32_t mask)
{
    unsigned shift = 0;
    while (shift < 31 && !(mask & (1u << shift))) shift++;
    return (v & ~mask) | ((field << shift) & mask);
}

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

/* udelay: a busy wait, since Sleep's resolution is a millisecond at best. */
static void rtl_udelay(unsigned us)
{
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    LONGLONG want = (LONGLONG)us * f.QuadPart / 1000000;
    do {
        QueryPerformanceCounter(&t);
    } while (t.QuadPart - t0.QuadPart < want);
}

/* mdelay and msleep: at least that long. */
static void rtl_mdelay(unsigned ms) { Sleep(ms); }

/* ------------------------------------------------------------ registers */

/* rtl8xxxu_read8/16/32 and write8/16/32: vendor request 0x05 underneath,
 * little-endian. A failed transfer marks the chip failed, and the steps of
 * rtl_chip_up check that. */
static uint8_t r8(struct rtl_chip *c, uint16_t a)
{
    uint8_t b = 0;
    if (c->io.read(c->io.ctx, a, &b, 1) < 0) c->failed = true;
    return b;
}

static uint16_t r16(struct rtl_chip *c, uint16_t a)
{
    uint8_t b[2] = { 0, 0 };
    if (c->io.read(c->io.ctx, a, b, 2) < 0) c->failed = true;
    return get_le16(b);
}

static uint32_t r32(struct rtl_chip *c, uint16_t a)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (c->io.read(c->io.ctx, a, b, 4) < 0) c->failed = true;
    return get_le32(b);
}

static void w8(struct rtl_chip *c, uint16_t a, uint8_t v)
{
    if (c->io.write(c->io.ctx, a, &v, 1) < 0) c->failed = true;
}

static void w16(struct rtl_chip *c, uint16_t a, uint16_t v)
{
    uint8_t b[2];
    put_le16(b, v);
    if (c->io.write(c->io.ctx, a, b, 2) < 0) c->failed = true;
}

static void w32(struct rtl_chip *c, uint16_t a, uint32_t v)
{
    uint8_t b[4];
    put_le32(b, v);
    if (c->io.write(c->io.ctx, a, b, 4) < 0) c->failed = true;
}

/* rtl8xxxu_read_rfreg, for path A, the 8188e's only one. */
static uint32_t rf_read(struct rtl_chip *c, uint8_t reg)
{
    uint32_t hssia = r32(c, REG_FPGA0_XA_HSSI_PARM2);
    uint32_t v = hssia;
    v &= ~FPGA0_HSSI_PARM2_ADDR_MASK;
    v |= (uint32_t)reg << FPGA0_HSSI_PARM2_ADDR_SHIFT;
    v |= FPGA0_HSSI_PARM2_EDGE_READ;
    hssia &= ~FPGA0_HSSI_PARM2_EDGE_READ;
    w32(c, REG_FPGA0_XA_HSSI_PARM2, hssia);
    rtl_udelay(10);
    w32(c, REG_FPGA0_XA_HSSI_PARM2, v);
    rtl_udelay(100);
    hssia |= FPGA0_HSSI_PARM2_EDGE_READ;
    w32(c, REG_FPGA0_XA_HSSI_PARM2, hssia);
    rtl_udelay(10);
    if (r32(c, REG_FPGA0_XA_HSSI_PARM1) & FPGA0_HSSI_PARM1_PI)
        v = r32(c, REG_HSPI_XA_READBACK);
    else
        v = r32(c, REG_FPGA0_XA_LSSI_READBACK);
    return v & 0xfffff;
}

/* rtl8xxxu_write_rfreg */
static void rf_write(struct rtl_chip *c, uint8_t reg, uint32_t data)
{
    data &= FPGA0_LSSI_PARM_DATA_MASK;
    w32(c, REG_FPGA0_XA_LSSI_PARM, (uint32_t)reg << FPGA0_LSSI_PARM_ADDR_SHIFT | data);
    rtl_udelay(1);
}

/* ------------------------------------------------------------ identity, efuse, firmware */

/* rtl8188eu_identify_chip */
static int identify_chip(struct rtl_chip *c, char *err, size_t errlen)
{
    uint32_t sys_cfg = r32(c, REG_SYS_CFG);
    c->chip_cut = (uint8_t)((sys_cfg & SYS_CFG_CHIP_VERSION_MASK) >> 12);
    if (sys_cfg & SYS_CFG_TRP_VAUX_EN) {
        set_err(err, errlen, "The Wi-Fi adapter is an RTL8188EU test chip, which isn't supported.");
        return -1;
    }
    /* rtl8xxxu refuses cut I: it wants other firmware and tables. */
    if (c->chip_cut == 8) {
        set_err(err, errlen, "The Wi-Fi adapter is an RTL8188EU cut I, which isn't supported.");
        return -1;
    }
    /* SYS_CFG_VENDOR_ID (UMC or TSMC) only names the chip in rtl8xxxu's log. */
    return 0;
}

/* rtl8xxxu_read_efuse8 */
static int efuse_read8(struct rtl_chip *c, uint16_t offset, uint8_t *data)
{
    w8(c, REG_EFUSE_CTRL + 1, (uint8_t)(offset & 0xff));
    uint8_t v8 = r8(c, REG_EFUSE_CTRL + 2);
    v8 = (uint8_t)((v8 & 0xfc) | ((offset >> 8) & 0x03));
    w8(c, REG_EFUSE_CTRL + 2, v8);
    v8 = r8(c, REG_EFUSE_CTRL + 3);
    w8(c, REG_EFUSE_CTRL + 3, (uint8_t)(v8 & 0x7f));

    uint32_t v32 = r32(c, REG_EFUSE_CTRL);
    int i;
    for (i = 0; i < MAX_REG_POLL && !c->failed; i++) {
        v32 = r32(c, REG_EFUSE_CTRL);
        if (v32 & RTL_BIT(31)) break;
    }
    if (i == MAX_REG_POLL || c->failed) return -1;
    rtl_udelay(50);
    v32 = r32(c, REG_EFUSE_CTRL);
    *data = (uint8_t)(v32 & 0xff);
    return 0;
}

/* The loop of rtl8xxxu_read_efuse: sections of up to four words, each with
 * a header (or an extended header) naming its offset and its absent words. */
static int efuse_parse_sections(struct rtl_chip *c)
{
    uint16_t efuse_addr = 0;
    while (efuse_addr < EFUSE_REAL_CONTENT_LEN) {
        uint8_t header, extheader, word_mask, val8;
        unsigned offset;
        if (efuse_read8(c, efuse_addr++, &header) < 0) return -1;
        if (header == 0xff) return 0;
        if ((header & 0x1f) == 0x0f) {      /* extended header */
            offset = (header & 0xe0u) >> 5;
            if (efuse_read8(c, efuse_addr++, &extheader) < 0) return -1;
            if ((extheader & 0x0f) == 0x0f) continue;  /* all words disabled */
            offset |= (extheader & 0xf0u) >> 1;
            word_mask = (uint8_t)(extheader & 0x0f);
        } else {
            offset = (header >> 4) & 0x0fu;
            word_mask = (uint8_t)(header & 0x0f);
        }
        unsigned map_addr = offset * 8;
        for (unsigned i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
            if (word_mask & RTL_BIT(i)) {
                map_addr += 2;
                continue;
            }
            if (efuse_read8(c, efuse_addr++, &val8) < 0) return -1;
            if (map_addr >= EFUSE_MAP_LEN - 1) return -2;  /* efuse corrupt */
            c->efuse[map_addr++] = val8;
            if (efuse_read8(c, efuse_addr++, &val8) < 0) return -1;
            c->efuse[map_addr++] = val8;
        }
    }
    return 0;
}

/* rtl8xxxu_read_efuse. REG_9346CR (EEPROM or efuse) only feeds its log, and
 * the 8188e isn't a multi-function chip, so REG_EFUSE_TEST stays as is. */
static int read_efuse(struct rtl_chip *c)
{
    w8(c, REG_EFUSE_ACCESS, EFUSE_ACCESS_ENABLE);

    uint16_t v16 = r16(c, REG_SYS_ISO_CTRL);
    if (!(v16 & SYS_ISO_PWC_EV12V)) w16(c, REG_SYS_ISO_CTRL, (uint16_t)(v16 | SYS_ISO_PWC_EV12V));
    v16 = r16(c, REG_SYS_FUNC);
    if (!(v16 & SYS_FUNC_ELDR)) w16(c, REG_SYS_FUNC, (uint16_t)(v16 | SYS_FUNC_ELDR));
    v16 = r16(c, REG_SYS_CLKR);
    if (!(v16 & SYS_CLK_LOADER_ENABLE) || !(v16 & SYS_CLK_ANA8M))
        w16(c, REG_SYS_CLKR, (uint16_t)(v16 | SYS_CLK_LOADER_ENABLE | SYS_CLK_ANA8M));

    memset(c->efuse, 0xff, sizeof c->efuse);
    int ret = efuse_parse_sections(c);

    w8(c, REG_EFUSE_ACCESS, EFUSE_ACCESS_DISABLE);
    return ret;
}

/* rtl8188eu_parse_efuse. rtl8xxxu reads no OFDM or HT20 power difference
 * for the 8188e, so set_tx_power adds none. */
static int parse_efuse(struct rtl_chip *c)
{
    if (get_le16(c->efuse + EFUSE_RTL_ID) != 0x8129) return -1;
    memcpy(c->mac, c->efuse + EFUSE_MAC_ADDR, 6);
    memcpy(c->cck_power, c->efuse + EFUSE_CCK_BASE, sizeof c->cck_power);
    memcpy(c->ht40_1s_power, c->efuse + EFUSE_HT40_BASE, sizeof c->ht40_1s_power);
    c->crystal_cap = (uint8_t)(c->efuse[EFUSE_XTAL_K] & 0x3f);
    return 0;
}

/* The checks of rtl8xxxu_load_firmware: a 32-byte header whose signature
 * names a chip rtl8xxxu knows; what follows the header is downloaded. */
static int check_firmware(const uint8_t *fw, size_t len)
{
    if (!fw || len <= FW_HEADER_LEN) return -1;
    switch (get_le16(fw) & 0xfff0) {
    case 0x92e0:
    case 0x92c0:
    case 0x88e0:
    case 0x88c0:
    case 0x5300:
    case 0x2300:
    case 0x88f0:
    case 0x10b0:
    case 0x92f0:
        return 0;
    default:
        return -1;
    }
}

/* rtl8188eu_reset_8051 */
static void reset_8051(struct rtl_chip *c)
{
    uint16_t f = r16(c, REG_SYS_FUNC);
    f = (uint16_t)(f & ~SYS_FUNC_CPU_ENABLE);
    w16(c, REG_SYS_FUNC, f);
    f = (uint16_t)(f | SYS_FUNC_CPU_ENABLE);
    w16(c, REG_SYS_FUNC, f);
}

/* rtl8xxxu_writeN: blocks of 196 bytes. -1 if one wasn't taken. */
static int write_n(struct rtl_chip *c, uint16_t addr, const uint8_t *buf, size_t len)
{
    size_t count = len / WRITEN_BLOCK_SIZE, remainder = len % WRITEN_BLOCK_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (c->io.write(c->io.ctx, addr, buf, (uint16_t)WRITEN_BLOCK_SIZE) < 0) return -1;
        addr = (uint16_t)(addr + WRITEN_BLOCK_SIZE);
        buf += WRITEN_BLOCK_SIZE;
    }
    if (remainder && c->io.write(c->io.ctx, addr, buf, (uint16_t)remainder) < 0) return -1;
    return 0;
}

/* rtl8xxxu_download_firmware: 4 KiB pages through the window at 0x1000,
 * the page picked in the low 3 bits of REG_MCU_FW_DL + 2. -1 when a block
 * wasn't taken (rtl8xxxu's -EAGAIN, which it retries). */
static int download_firmware(struct rtl_chip *c, const uint8_t *fw, size_t size)
{
    uint8_t v8 = r8(c, REG_SYS_FUNC + 1);
    w8(c, REG_SYS_FUNC + 1, (uint8_t)(v8 | 4));
    uint16_t v16 = r16(c, REG_SYS_FUNC);
    w16(c, REG_SYS_FUNC, (uint16_t)(v16 | SYS_FUNC_CPU_ENABLE));    /* 8051 enable */

    v8 = r8(c, REG_MCU_FW_DL);
    if (v8 & MCU_FW_RAM_SEL) {      /* firmware already running: reset the MCU */
        w8(c, REG_MCU_FW_DL, 0x00);
        reset_8051(c);
    }

    v8 = r8(c, REG_MCU_FW_DL);
    w8(c, REG_MCU_FW_DL, (uint8_t)(v8 | MCU_FW_DL_ENABLE));
    uint32_t v32 = r32(c, REG_MCU_FW_DL);
    w32(c, REG_MCU_FW_DL, v32 & ~RTL_BIT(19));                      /* 8051 reset */
    v8 = r8(c, REG_MCU_FW_DL);
    w8(c, REG_MCU_FW_DL, (uint8_t)(v8 | MCU_FW_DL_CSUM_REPORT));    /* reset the checksum */

    size_t pages = size / FW_PAGE_SIZE, remainder = size % FW_PAGE_SIZE, i;
    int ret = 0;
    for (i = 0; i < pages && !ret; i++) {
        v8 = (uint8_t)((r8(c, REG_MCU_FW_DL + 2) & 0xf8) | i);
        w8(c, REG_MCU_FW_DL + 2, v8);
        ret = write_n(c, REG_FW_START_ADDRESS, fw, FW_PAGE_SIZE);
        fw += FW_PAGE_SIZE;
    }
    if (!ret && remainder) {
        v8 = (uint8_t)((r8(c, REG_MCU_FW_DL + 2) & 0xf8) | i);
        w8(c, REG_MCU_FW_DL + 2, v8);
        ret = write_n(c, REG_FW_START_ADDRESS, fw, remainder);
    }

    v16 = r16(c, REG_MCU_FW_DL);
    w16(c, REG_MCU_FW_DL, (uint16_t)(v16 & ~MCU_FW_DL_ENABLE));    /* download disable */
    return ret;
}

/* rtl8xxxu_start_firmware. 0, -1 when the checksum never came, -2 when
 * the firmware never said it was ready (what #107 saw with the MCU IO
 * wrapper held). */
static int start_firmware(struct rtl_chip *c)
{
    int i;
    uint32_t v32;
    for (i = 0; i < FIRMWARE_POLL_MAX && !c->failed; i++)
        if (r32(c, REG_MCU_FW_DL) & MCU_FW_DL_CSUM_REPORT) break;
    if (i == FIRMWARE_POLL_MAX || c->failed) return -1;

    v32 = r32(c, REG_MCU_FW_DL);
    v32 |= MCU_FW_DL_READY;
    v32 &= ~MCU_WINT_INIT_READY;
    w32(c, REG_MCU_FW_DL, v32);

    reset_8051(c);

    for (i = 0; i < FIRMWARE_POLL_MAX && !c->failed; i++) {
        if (r32(c, REG_MCU_FW_DL) & MCU_WINT_INIT_READY) break;
        rtl_udelay(100);
    }
    if (i == FIRMWARE_POLL_MAX || c->failed) return -2;
    /* .init_reg_hmtfr isn't set for the 8188e. */
    return 0;
}

/* ------------------------------------------------------------ power */

/* rtl8188e_disabled_to_emu */
static void disabled_to_emu(struct rtl_chip *c)
{
    uint16_t v16 = r16(c, REG_APS_FSMCO);
    w16(c, REG_APS_FSMCO, (uint16_t)(v16 & ~(APS_FSMCO_HW_SUSPEND | APS_FSMCO_PCIE)));
}

/* rtl8188e_emu_to_active */
static int emu_to_active(struct rtl_chip *c)
{
    int count;
    for (count = MAX_REG_POLL; count && !c->failed; count--) {     /* 0x04[17]: power ready */
        if (r32(c, REG_APS_FSMCO) & RTL_BIT(17)) break;
        rtl_udelay(10);
    }
    if (!count || c->failed) return -1;

    uint8_t v8 = r8(c, REG_SYS_FUNC);                               /* reset baseband */
    w8(c, REG_SYS_FUNC, (uint8_t)(v8 & ~(SYS_FUNC_BBRSTB | SYS_FUNC_BB_GLB_RSTN)));
    w32(c, REG_AFE_XTAL_CTRL, r32(c, REG_AFE_XTAL_CTRL) | RTL_BIT(23)); /* schmitt trigger */
    uint16_t v16 = r16(c, REG_APS_FSMCO);                           /* HWPDN off */
    w16(c, REG_APS_FSMCO, (uint16_t)(v16 & ~APS_FSMCO_HW_POWERDOWN));
    v16 = r16(c, REG_APS_FSMCO);                                    /* WL suspend off */
    w16(c, REG_APS_FSMCO, (uint16_t)(v16 & ~(APS_FSMCO_HW_SUSPEND | APS_FSMCO_PCIE)));

    w32(c, REG_APS_FSMCO, r32(c, REG_APS_FSMCO) | APS_FSMCO_MAC_ENABLE);  /* set, then poll until 0 */
    for (count = MAX_REG_POLL; count && !c->failed; count--) {
        if (!(r32(c, REG_APS_FSMCO) & APS_FSMCO_MAC_ENABLE)) break;
        rtl_udelay(10);
    }
    if (!count || c->failed) return -1;

    v8 = r8(c, REG_LPLDO_CTRL);                                     /* LDO normal mode */
    w8(c, REG_LPLDO_CTRL, (uint8_t)(v8 & ~RTL_BIT(4)));
    return 0;
}

/* rtl8188eu_power_on. MAC TX and RX wait for usb_quirks: on the 88E they
 * have to follow REG_TRXFF_BNDY. */
static void active_to_emu(struct rtl_chip *c);

static int power_on(struct rtl_chip *c)
{
    /* Whatever state the chip was left in, by Windows' driver, by a session
     * of ours that ended, or by Linux's power-off: the MAC off first, so
     * the power-on below runs as on a cold chip (issue #109). */
    active_to_emu(c);
    disabled_to_emu(c);
    if (emu_to_active(c) < 0) return -1;
    w16(c, REG_CR, CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE | CR_TXDMA_ENABLE | CR_RXDMA_ENABLE |
                       CR_PROTOCOL_ENABLE | CR_SCHEDULE_ENABLE | CR_SECURITY_ENABLE | CR_CALTIMER_ENABLE);
    return 0;
}

/* rtl8188eu_active_to_emu, with the step Realtek's own sequence
 * (RTL8188E_TRANS_ACT_TO_CARDEMU) has and rtl8xxxu leaves out: the MAC
 * turned off by the hardware state machine (0x04[9]), polled until done.
 * Without it the next power-on finds the MAC still on, its power-on steps
 * do nothing, and the radio hears energy but decodes almost nothing
 * (measured, issue #109): what Linux's driver met too, hearing only on its
 * first start after Windows' driver had the chip (#107). */
static void active_to_emu(struct rtl_chip *c)
{
    uint8_t v8 = r8(c, REG_RF_CTRL);
    w8(c, REG_RF_CTRL, (uint8_t)(v8 & ~RF_ENABLE));                 /* RF off */
    v8 = r8(c, REG_LPLDO_CTRL);
    w8(c, REG_LPLDO_CTRL, (uint8_t)(v8 | RTL_BIT(4)));              /* LDO sleep mode */
    w16(c, REG_APS_FSMCO, (uint16_t)(r16(c, REG_APS_FSMCO) | APS_FSMCO_MAC_OFF));
    for (int count = MAX_REG_POLL; count && !c->failed; count--) {
        if (!(r16(c, REG_APS_FSMCO) & APS_FSMCO_MAC_OFF)) break;
        rtl_udelay(10);
    }
}

/* rtl8188eu_emu_to_disabled */
static void emu_to_disabled(struct rtl_chip *c)
{
    w32(c, REG_AFE_XTAL_CTRL, r32(c, REG_AFE_XTAL_CTRL) | RTL_BIT(23));
    uint16_t v16 = r16(c, REG_APS_FSMCO);
    v16 = (uint16_t)((v16 & ~APS_FSMCO_PCIE) | APS_FSMCO_HW_SUSPEND);
    w16(c, REG_APS_FSMCO, v16);
    w8(c, REG_APS_FSMCO + 3, 0x00);
    uint8_t v8 = r8(c, REG_GPIO_MUXCFG + 1);
    w8(c, REG_GPIO_MUXCFG + 1, (uint8_t)(v8 & ~RTL_BIT(4)));
    v8 = r8(c, 0xfe10);                                             /* USB suspend enable */
    w8(c, 0xfe10, (uint8_t)(v8 | RTL_BIT(4)));
}

/* rtl8188eu_active_to_lps. rtl8xxxu's TX flush poll ends without failing
 * in practice (its retry count runs past zero), and so does this one. */
static void active_to_lps(struct rtl_chip *c)
{
    w8(c, REG_TXPAUSE, 0x7f);
    for (int retry = 0; retry <= 100 && !c->failed; retry++)
        if (!r32(c, REG_SCH_TX_CMD)) break;
    uint8_t v8 = r8(c, REG_SYS_FUNC);                               /* CCK and OFDM off, clock gated */
    w8(c, REG_SYS_FUNC, (uint8_t)(v8 & ~SYS_FUNC_BBRSTB));
    rtl_udelay(2);
    uint16_t v16 = r16(c, REG_CR);                                  /* reset MAC TRX */
    v16 = (uint16_t)((v16 | 0xff) & ~(CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE | CR_SECURITY_ENABLE));
    w16(c, REG_CR, v16);
    v8 = r8(c, REG_DUAL_TSF_RST);
    w8(c, REG_DUAL_TSF_RST, (uint8_t)(v8 | DUAL_TSF_TX_OK));
}

/* rtl8xxxu_flush_fifo */
static void flush_fifo(struct rtl_chip *c)
{
    w8(c, REG_TXPAUSE, 0xff);
    w32(c, REG_RXPKT_NUM, r32(c, REG_RXPKT_NUM) | RXPKT_NUM_RW_RELEASE_EN);
    for (int retry = 0; retry <= 100 && !c->failed; retry++)
        if (r32(c, REG_RXPKT_NUM) & RXPKT_NUM_RXDMA_IDLE) break;
    w16(c, REG_RQPN_NPQ, 0);
    w32(c, REG_RQPN, 0x80000000u);
    rtl_mdelay(2);
}

/* rtl8xxxu_firmware_self_reset */
static void firmware_self_reset(struct rtl_chip *c)
{
    int i;
    w8(c, REG_HMTFR + 3, 0x20);                                     /* ask the 8051 to reset */
    for (i = 100; i > 0 && !c->failed; i--) {
        if (!(r16(c, REG_SYS_FUNC) & SYS_FUNC_CPU_ENABLE)) break;
        rtl_udelay(50);
    }
    if (!i) {                                                       /* force it */
        uint16_t v16 = r16(c, REG_SYS_FUNC);
        w16(c, REG_SYS_FUNC, (uint16_t)(v16 & ~SYS_FUNC_CPU_ENABLE));
    }
}

/* rtl8188e_enable_rf */
static void enable_rf(struct rtl_chip *c)
{
    w8(c, REG_RF_CTRL, RF_ENABLE | RF_RSTB | RF_SDMRSTB);
    uint32_t v = r32(c, REG_OFDM0_TRX_PATH_ENABLE);
    v &= ~(OFDM_RF_PATH_RX_MASK | OFDM_RF_PATH_TX_MASK);
    v |= OFDM_RF_PATH_RX_A | OFDM_RF_PATH_TX_A;
    w32(c, REG_OFDM0_TRX_PATH_ENABLE, v);
    w8(c, REG_TXPAUSE, 0x00);
}

/* rtl8188e_disable_rf */
static void disable_rf(struct rtl_chip *c)
{
    w32(c, REG_OFDM0_TRX_PATH_ENABLE, r32(c, REG_OFDM0_TRX_PATH_ENABLE) & ~OFDM_RF_PATH_TX_MASK);
    rf_write(c, RF6052_REG_AC, 0);                                  /* RF module down */
    active_to_emu(c);
}

/* rtl8188eu_power_off */
static void power_off(struct rtl_chip *c)
{
    flush_fifo(c);
    uint8_t v8 = r8(c, REG_TX_REPORT_CTRL);
    w8(c, REG_TX_REPORT_CTRL, (uint8_t)(v8 & ~TX_REPORT_CTRL_TIMER_ENABLE));
    w8(c, REG_RF_CTRL, 0x00);                                       /* RF off */
    active_to_lps(c);
    if (r8(c, REG_MCU_FW_DL) & MCU_FW_RAM_SEL)                      /* firmware running from RAM */
        firmware_self_reset(c);
    uint16_t v16 = r16(c, REG_SYS_FUNC);                            /* reset the MCU */
    w16(c, REG_SYS_FUNC, (uint16_t)(v16 & ~SYS_FUNC_CPU_ENABLE));
    w8(c, REG_MCU_FW_DL, 0x00);                                     /* and its ready status */
    v8 = r8(c, REG_32K_CTRL);
    w8(c, REG_32K_CTRL, (uint8_t)(v8 & ~RTL_BIT(0)));
    active_to_emu(c);
    emu_to_disabled(c);

    v8 = r8(c, REG_RSV_CTRL + 1);                                   /* reset the MCU IO wrapper */
    w8(c, REG_RSV_CTRL + 1, (uint8_t)(v8 & ~RSV_CTRL_MCU_IO_WRAPPER));
    v8 = r8(c, REG_RSV_CTRL + 1);
    w8(c, REG_RSV_CTRL + 1, (uint8_t)(v8 | RSV_CTRL_MCU_IO_WRAPPER));

    v8 = r8(c, REG_GPIO_PIN_CTRL);                                  /* GPIO_IN to GPIO_OUT */
    w8(c, REG_GPIO_PIN_CTRL + 1, v8);
    w8(c, REG_GPIO_PIN_CTRL + 2, 0xff);
    v8 = r8(c, REG_GPIO_IO_SEL);
    w8(c, REG_GPIO_IO_SEL, (uint8_t)(v8 << 4));
    v8 = r8(c, REG_GPIO_IO_SEL + 1);
    w8(c, REG_GPIO_IO_SEL + 1, (uint8_t)(v8 | 0x0f));
    w32(c, REG_PAD_CTRL1, 0x00080808);                              /* LNA, TRSW, EX_PA pins to output */
    w8(c, REG_RSV_CTRL, 0x00);
    w32(c, REG_GPIO_MUXCFG, 0x00000000);
}

/* ------------------------------------------------------------ queues and pages */

/* rtl8xxxu_config_endpoints_no_sie and the mapping of
 * rtl8xxxu_init_queue_priority: which DMA queue each TX queue uses, and
 * which bulk OUT endpoint (by index) management and best effort frames go
 * out on. */
struct qmap {
    bool hi, normal, low;       /* the endpoints' queues */
    uint16_t trxdma;            /* REG_TRXDMA_CTRL's queue bits */
    unsigned mgp, bep;
};

static int queue_map(unsigned out_eps, struct qmap *m)
{
    unsigned hiq, mgq, bkq, beq, viq, voq;
    memset(m, 0, sizeof *m);
    if (out_eps < 1 || out_eps > 6) return -1;      /* "Unsupported USB TX end-points" */
    m->hi = true;
    m->normal = out_eps >= 2;
    m->low = out_eps >= 3;
    if (out_eps == 1) {
        hiq = mgq = bkq = beq = viq = voq = TRXDMA_QUEUE_HIGH;
        m->mgp = m->bep = 0;
    } else if (out_eps == 2) {                      /* high and normal */
        hiq = mgq = viq = voq = TRXDMA_QUEUE_HIGH;
        bkq = beq = TRXDMA_QUEUE_NORMAL;
        m->mgp = 0;
        m->bep = 1;
    } else {                                        /* high, normal and low */
        beq = bkq = TRXDMA_QUEUE_LOW;
        viq = TRXDMA_QUEUE_NORMAL;
        voq = mgq = hiq = TRXDMA_QUEUE_HIGH;
        m->mgp = mgq ^ 3;
        m->bep = beq ^ 3;
    }
    m->trxdma = (uint16_t)(voq << TRXDMA_CTRL_VOQ_SHIFT | viq << TRXDMA_CTRL_VIQ_SHIFT |
                           beq << TRXDMA_CTRL_BEQ_SHIFT | bkq << TRXDMA_CTRL_BKQ_SHIFT |
                           mgq << TRXDMA_CTRL_MGQ_SHIFT | hiq << TRXDMA_CTRL_HIQ_SHIFT);
    return 0;
}

/* rtl8xxxu_init_queue_reserved_page */
static void init_queue_reserved_page(struct rtl_chip *c, const struct qmap *m)
{
    uint32_t hq = m->hi ? PAGE_NUM_HI : 0, lq = m->low ? PAGE_NUM_LO : 0, nq = m->normal ? PAGE_NUM_NORM : 0;
    uint32_t eq = 0;
    w32(c, REG_RQPN_NPQ, nq << RQPN_NPQ_SHIFT | eq << RQPN_EPQ_SHIFT);
    uint32_t pubq = TOTAL_PAGE_NUM - hq - lq - nq - 1;
    w32(c, REG_RQPN, RQPN_LOAD | hq << RQPN_HI_PQ_SHIFT | lq << RQPN_LO_PQ_SHIFT | pubq << RQPN_PUB_PQ_SHIFT);
}

/* rtl8xxxu_init_queue_priority (the endpoints are rtl_tx_build's) */
static void init_queue_priority(struct rtl_chip *c, const struct qmap *m)
{
    uint16_t v16 = r16(c, REG_TRXDMA_CTRL);
    w16(c, REG_TRXDMA_CTRL, (uint16_t)((v16 & 0x7) | m->trxdma));
}

/* rtl8xxxu_llt_write */
static int llt_write(struct rtl_chip *c, unsigned address, unsigned data)
{
    w32(c, REG_LLT_INIT, LLT_OP_WRITE | (address & 0xff) << 8 | (data & 0xff));
    int count = 0;
    do {
        if ((r32(c, REG_LLT_INIT) & LLT_OP_MASK) == LLT_OP_INACTIVE) return 0;
    } while (count++ < 20 && !c->failed);
    return -1;
}

/* rtl8xxxu_init_llt_table: the TX pages in a chain, the rest a ring. */
static int init_llt_table(struct rtl_chip *c)
{
    unsigned i;
    for (i = 0; i < TOTAL_PAGE_NUM; i++)
        if (llt_write(c, i, i + 1) < 0) return -1;
    if (llt_write(c, TOTAL_PAGE_NUM, 0xff) < 0) return -1;
    for (i = TOTAL_PAGE_NUM + 1; i < LAST_LLT_ENTRY; i++)
        if (llt_write(c, i, i + 1) < 0) return -1;
    return llt_write(c, LAST_LLT_ENTRY, TOTAL_PAGE_NUM + 1);
}

/* ------------------------------------------------------------ MAC, BB, RF */

/* rtl8xxxu_init_mac */
static void init_mac(struct rtl_chip *c)
{
    for (const struct rtl_reg8val *t = rtl8188e_mac_init_table; !(t->reg == 0xffff && t->val == 0xff) && !c->failed; t++)
        w8(c, t->reg, t->val);
    w16(c, REG_MAX_AGGR_NUM, 0x0707);
}

/* rtl8xxxu_init_phy_regs */
static void init_phy_regs(struct rtl_chip *c, const struct rtl_reg32val *t)
{
    for (; !(t->reg == 0xffff && t->val == 0xffffffffu) && !c->failed; t++) {
        w32(c, t->reg, t->val);
        rtl_udelay(1);
    }
}

/* rtl8188f_set_crystal_cap: crystal_cap_set stands for rtl8xxxu's
 * cfo_tracking.crystal_cap, 0 until the first call. */
static void set_crystal_cap(struct rtl_chip *c, uint8_t cap)
{
    if (cap == c->crystal_cap_set) return;
    uint32_t v = r32(c, REG_AFE_XTAL_CTRL);
    v &= ~(AFE_XTAL1_MASK | AFE_XTAL0_MASK);
    v |= ((uint32_t)cap << 17 & AFE_XTAL1_MASK) | ((uint32_t)cap << 11 & AFE_XTAL0_MASK);
    w32(c, REG_AFE_XTAL_CTRL, v);
    c->crystal_cap_set = cap;
}

/* rtl8188eu_init_phy_bb, then what rtl8xxxu_init_phy_bb adds for a 1T1R
 * chip: the crystal cap. */
static void init_phy_bb(struct rtl_chip *c)
{
    uint16_t v16 = r16(c, REG_SYS_FUNC);
    w16(c, REG_SYS_FUNC, (uint16_t)(v16 | SYS_FUNC_BB_GLB_RSTN | SYS_FUNC_BBRSTB | SYS_FUNC_DIO_RF));
    w8(c, REG_RF_CTRL, RF_ENABLE | RF_RSTB | RF_SDMRSTB);          /* power sequence before the RF */
    w8(c, REG_SYS_FUNC, SYS_FUNC_USBA | SYS_FUNC_USBD | SYS_FUNC_BB_GLB_RSTN | SYS_FUNC_BBRSTB);
    init_phy_regs(c, rtl8188eu_phy_init_table);
    init_phy_regs(c, rtl8188e_agc_table);
    set_crystal_cap(c, c->crystal_cap);
}

/* rtl8xxxu_init_rf_regs: 0xf9 - 0xfe are pauses. */
static void init_rf_regs(struct rtl_chip *c, const struct rtl_rfregval *t)
{
    for (; !(t->reg == 0xff && t->val == 0xffffffffu) && !c->failed; t++) {
        switch (t->reg) {
        case 0xfe: rtl_mdelay(50); continue;
        case 0xfd: rtl_mdelay(5); continue;
        case 0xfc: rtl_mdelay(1); continue;
        case 0xfb: rtl_udelay(50); continue;
        case 0xfa: rtl_udelay(5); continue;
        case 0xf9: rtl_udelay(1); continue;
        default: break;
        }
        rf_write(c, t->reg, t->val);
        rtl_udelay(1);
    }
}

/* rtl8xxxu_init_phy_rf, path A */
static void init_phy_rf(struct rtl_chip *c)
{
    uint16_t rfenv = (uint16_t)(r16(c, REG_FPGA0_XA_RF_SW_CTRL) & FPGA0_RF_RFENV);
    w32(c, REG_FPGA0_XA_RF_INT_OE, r32(c, REG_FPGA0_XA_RF_INT_OE) | RTL_BIT(20));
    rtl_udelay(1);
    w32(c, REG_FPGA0_XA_RF_INT_OE, r32(c, REG_FPGA0_XA_RF_INT_OE) | RTL_BIT(4));
    rtl_udelay(1);
    w32(c, REG_FPGA0_XA_HSSI_PARM2, r32(c, REG_FPGA0_XA_HSSI_PARM2) & ~FPGA0_HSSI_3WIRE_ADDR_LEN);
    rtl_udelay(1);
    w32(c, REG_FPGA0_XA_HSSI_PARM2, r32(c, REG_FPGA0_XA_HSSI_PARM2) & ~FPGA0_HSSI_3WIRE_DATA_LEN);
    rtl_udelay(1);
    init_rf_regs(c, rtl8188eu_radioa_init_table);
    uint16_t v16 = r16(c, REG_FPGA0_XA_RF_SW_CTRL);
    w16(c, REG_FPGA0_XA_RF_SW_CTRL, (uint16_t)((v16 & ~FPGA0_RF_RFENV) | rfenv));
}

/* rtl8188f_set_tx_power, at 20 MHz: per channel group, the efuse's CCK and
 * HT40 1S bases. */
static void set_tx_power(struct rtl_chip *c, int channel)
{
    int group = channel < 3 ? 0 : channel < 6 ? 1 : channel < 9 ? 2 : channel < 12 ? 3 : 4;
    int cck_group = channel == 14 ? 5 : group;
    uint32_t cck = c->cck_power[cck_group];
    w32(c, REG_TX_AGC_A_CCK1_MCS32, (r32(c, REG_TX_AGC_A_CCK1_MCS32) & 0xffff00ffu) | cck << 8);
    w32(c, REG_TX_AGC_B_CCK11_A_CCK2_11,
        (r32(c, REG_TX_AGC_B_CCK11_A_CCK2_11) & 0xffu) | cck << 8 | cck << 16 | cck << 24);
    uint32_t ofdm = c->ht40_1s_power[group] * 0x01010101u;     /* + ofdm_tx_power_diff, 0 here */
    w32(c, REG_TX_AGC_A_RATE18_06, ofdm);
    w32(c, REG_TX_AGC_A_RATE54_24, ofdm);
    uint32_t mcs = c->ht40_1s_power[group] * 0x01010101u;      /* + ht20_tx_power_diff, 0 here */
    w32(c, REG_TX_AGC_A_MCS03_MCS00, mcs);
    w32(c, REG_TX_AGC_A_MCS07_MCS04, mcs);
    w32(c, REG_TX_AGC_A_MCS11_MCS08, mcs);
    w32(c, REG_TX_AGC_A_MCS15_MCS12, mcs);
}

/* rtl8188eu_config_channel, at 20 MHz */
static void config_channel(struct rtl_chip *c, int channel)
{
    uint8_t opmode = r8(c, REG_BW_OPMODE);
    w8(c, REG_BW_OPMODE, (uint8_t)(opmode | BW_OPMODE_20MHZ));
    w32(c, REG_FPGA0_RF_MODE, r32(c, REG_FPGA0_RF_MODE) & ~FPGA_RF_MODE);
    w32(c, REG_FPGA1_RF_MODE, r32(c, REG_FPGA1_RF_MODE) & ~FPGA_RF_MODE);
    uint32_t v = rf_read(c, RF6052_REG_MODE_AG);
    v = replace_bits(v, (uint32_t)channel, MODE_AG_CHANNEL_MASK);
    rf_write(c, RF6052_REG_MODE_AG, v);
    v = rf_read(c, RF6052_REG_MODE_AG);
    v = (v & ~MODE_AG_BW_MASK) | MODE_AG_BW_20MHZ_8723B;
    rf_write(c, RF6052_REG_MODE_AG, v);
}

/* rtl8723a_phy_lc_calibrate, for one path and no S0S1 */
static void lc_calibrate(struct rtl_chip *c)
{
    uint32_t rf_amode = 0;
    uint32_t lstf = r32(c, REG_OFDM1_LSTF);
    if (lstf & OFDM_LSTF_MASK) {                    /* continuous TX: stop it */
        w32(c, REG_OFDM1_LSTF, lstf & ~OFDM_LSTF_MASK);
        rf_amode = rf_read(c, RF6052_REG_AC);
        rf_write(c, RF6052_REG_AC, (rf_amode & 0x8ffff) | 0x10000);
    } else {                                        /* packet TX: block every queue */
        w8(c, REG_TXPAUSE, 0xff);
    }
    rf_write(c, RF6052_REG_MODE_AG, rf_read(c, RF6052_REG_MODE_AG) | 0x08000);
    rtl_mdelay(100);
    if (lstf & OFDM_LSTF_MASK) {
        w32(c, REG_OFDM1_LSTF, lstf);
        rf_write(c, RF6052_REG_AC, rf_amode);
    } else {
        w8(c, REG_TXPAUSE, 0x00);
    }
}

/* ------------------------------------------------------------ IQ calibration */

static const uint16_t adda_regs[ADDA_REGS] = {
    REG_FPGA0_XCD_SWITCH_CTRL, REG_BLUETOOTH, REG_RX_WAIT_CCA, REG_TX_CCK_RFON,
    REG_TX_CCK_BBON, REG_TX_OFDM_RFON, REG_TX_OFDM_BBON, REG_TX_TO_RX,
    REG_TX_TO_TX, REG_RX_CCK, REG_RX_OFDM, REG_RX_WAIT_RIFS,
    REG_RX_TO_RX, REG_STANDBY, REG_SLEEP, REG_PMPD_ANAEN,
};
static const uint16_t iqk_mac_regs[MAC_REGS] = {
    REG_TXPAUSE, REG_BEACON_CTRL, REG_BEACON_CTRL_1, REG_GPIO_MUXCFG,
};
static const uint16_t iqk_bb_regs[BB_REGS] = {
    REG_OFDM0_TRX_PATH_ENABLE, REG_OFDM0_TR_MUX_PAR, REG_FPGA0_XCD_RF_SW_CTRL,
    REG_CONFIG_ANT_A, REG_CONFIG_ANT_B, REG_FPGA0_XAB_RF_SW_CTRL,
    REG_FPGA0_XA_RF_INT_OE, REG_FPGA0_XB_RF_INT_OE, REG_CCK0_AFE_SETTING,
};
/* rtl8xxxu_iqk_phy_iq_bb_reg */
static const uint16_t iqk_phy_iq_bb_regs[BB_REGS] = {
    REG_OFDM0_XA_RX_IQ_IMBALANCE, REG_OFDM0_XB_RX_IQ_IMBALANCE, REG_OFDM0_ENERGY_CCA_THRES,
    REG_OFDM0_AGC_RSSI_TABLE, REG_OFDM0_XA_TX_IQ_IMBALANCE, REG_OFDM0_XB_TX_IQ_IMBALANCE,
    REG_OFDM0_XC_TX_AFE, REG_OFDM0_XD_TX_AFE, REG_OFDM0_RX_IQ_EXT_ANTA,
};

/* rtl8xxxu_save_regs and rtl8xxxu_restore_regs */
static void save_regs(struct rtl_chip *c, const uint16_t *regs, uint32_t *backup, int n)
{
    for (int i = 0; i < n; i++) backup[i] = r32(c, regs[i]);
}

static void restore_regs(struct rtl_chip *c, const uint16_t *regs, const uint32_t *backup, int n)
{
    for (int i = 0; i < n; i++) w32(c, regs[i], backup[i]);
}

/* rtl8xxxu_save_mac_regs and rtl8xxxu_restore_mac_regs: three bytes and a
 * word. */
static void save_mac_regs(struct rtl_chip *c, const uint16_t *regs, uint32_t *backup)
{
    int i;
    for (i = 0; i < MAC_REGS - 1; i++) backup[i] = r8(c, regs[i]);
    backup[i] = r32(c, regs[i]);
}

static void restore_mac_regs(struct rtl_chip *c, const uint16_t *regs, const uint32_t *backup)
{
    int i;
    for (i = 0; i < MAC_REGS - 1; i++) w8(c, regs[i], (uint8_t)backup[i]);
    w32(c, regs[i], backup[i]);
}

/* rtl8xxxu_path_adda_on, one TX path */
static void path_adda_on(struct rtl_chip *c, const uint16_t *regs)
{
    w32(c, regs[0], ADDA_1T_INIT);
    for (int i = 1; i < ADDA_REGS; i++) w32(c, regs[i], ADDA_1T_PATH_ON);
}

/* rtl8xxxu_mac_calibration */
static void mac_calibration(struct rtl_chip *c, const uint16_t *regs, const uint32_t *backup)
{
    int i = 0;
    w8(c, regs[i], 0x3f);
    for (i = 1; i < MAC_REGS - 1; i++) w8(c, regs[i], (uint8_t)(backup[i] & ~RTL_BIT(3)));
    w8(c, regs[i], (uint8_t)(backup[i] & ~RTL_BIT(5)));
}

/* rtl8188eu_iqk_path_a */
static int iqk_path_a(struct rtl_chip *c)
{
    w32(c, REG_TX_IQK_TONE_A, 0x10008c1c);          /* path A IQK setting */
    w32(c, REG_RX_IQK_TONE_A, 0x30008c1c);
    w32(c, REG_TX_IQK_PI_A, 0x8214032a);
    w32(c, REG_RX_IQK_PI_A, 0x28160000);
    w32(c, REG_IQK_AGC_RSP, 0x00462911);            /* LO calibration setting */
    w32(c, REG_IQK_AGC_PTS, 0xf9000000);            /* one shot, path A LOK and IQK */
    w32(c, REG_IQK_AGC_PTS, 0xf8000000);
    rtl_mdelay(10);
    uint32_t eac = r32(c, REG_RX_POWER_AFTER_IQK_A_2);
    uint32_t e94 = r32(c, REG_TX_POWER_BEFORE_IQK_A);
    uint32_t e9c = r32(c, REG_TX_POWER_AFTER_IQK_A);
    int result = 0;
    if (!(eac & RTL_BIT(28)) && (e94 & 0x03ff0000) != 0x01420000 && (e9c & 0x03ff0000) != 0x00420000)
        result |= 0x01;
    return result;
}

/* rtl8188eu_rx_iqk_path_a */
static int rx_iqk_path_a(struct rtl_chip *c)
{
    int result = 0;
    w32(c, REG_FPGA0_IQK, replace_bits(r32(c, REG_FPGA0_IQK), 0, 0xffffff00));        /* leave IQK mode */
    rf_write(c, RF6052_REG_WE_LUT, 0x800a0);         /* path A PA on in TX IQK mode */
    rf_write(c, RF6052_REG_RCK_OS, 0x30000);
    rf_write(c, RF6052_REG_TXPA_G1, 0x0000f);
    rf_write(c, RF6052_REG_TXPA_G2, 0xf117b);
    w32(c, REG_FPGA0_IQK, replace_bits(r32(c, REG_FPGA0_IQK), 0x808000, 0xffffff00)); /* enter IQK mode */
    w32(c, REG_TX_IQK, 0x01007c00);                 /* TX IQK setting */
    w32(c, REG_RX_IQK, 0x81004800);
    w32(c, REG_TX_IQK_TONE_A, 0x10008c1c);          /* path A IQK setting */
    w32(c, REG_RX_IQK_TONE_A, 0x30008c1c);
    w32(c, REG_TX_IQK_PI_A, 0x82160804);
    w32(c, REG_RX_IQK_PI_A, 0x28160000);
    w32(c, REG_IQK_AGC_RSP, 0x0046a911);            /* LO calibration setting */
    w32(c, REG_IQK_AGC_PTS, 0xf9000000);            /* one shot, path A LOK and IQK */
    w32(c, REG_IQK_AGC_PTS, 0xf8000000);
    rtl_mdelay(10);

    uint32_t eac = r32(c, REG_RX_POWER_AFTER_IQK_A_2);
    uint32_t e94 = r32(c, REG_TX_POWER_BEFORE_IQK_A);
    uint32_t e9c = r32(c, REG_TX_POWER_AFTER_IQK_A);
    if (!(eac & RTL_BIT(28)) && (e94 & 0x03ff0000) != 0x01420000 && (e9c & 0x03ff0000) != 0x00420000)
        result |= 0x01;
    else
        return result;

    w32(c, REG_TX_IQK, 0x80007c00 | (e94 & 0x03ff0000) | ((e9c >> 16) & 0x03ff));
    w32(c, REG_FPGA0_IQK, replace_bits(r32(c, REG_FPGA0_IQK), 0, 0xffffff00));        /* RX IQK mode table */
    rf_write(c, RF6052_REG_WE_LUT, 0x800a0);
    rf_write(c, RF6052_REG_RCK_OS, 0x30000);
    rf_write(c, RF6052_REG_TXPA_G1, 0x0000f);
    rf_write(c, RF6052_REG_TXPA_G2, 0xf7ffa);
    w32(c, REG_FPGA0_IQK, replace_bits(r32(c, REG_FPGA0_IQK), 0x808000, 0xffffff00)); /* enter IQK mode */
    w32(c, REG_RX_IQK, 0x01004800);
    w32(c, REG_TX_IQK_TONE_A, 0x30008c1c);
    w32(c, REG_RX_IQK_TONE_A, 0x10008c1c);
    w32(c, REG_TX_IQK_PI_A, 0x82160c05);
    w32(c, REG_RX_IQK_PI_A, 0x28160c05);
    w32(c, REG_IQK_AGC_RSP, 0x0046a911);
    w32(c, REG_IQK_AGC_PTS, 0xf9000000);
    w32(c, REG_IQK_AGC_PTS, 0xf8000000);
    rtl_mdelay(10);

    eac = r32(c, REG_RX_POWER_AFTER_IQK_A_2);
    uint32_t ea4 = r32(c, REG_RX_POWER_BEFORE_IQK_A_2);
    if (!(eac & RTL_BIT(27)) && (ea4 & 0x03ff0000) != 0x01320000 && (eac & 0x03ff0000) != 0x00360000)
        result |= 0x02;
    return result;
}

/* rtl8188eu_phy_iqcalibrate: run `t` of up to three. */
static void phy_iqcalibrate(struct rtl_chip *c, int result[][8], int t)
{
    const int retry = 2;
    int i, path_a_ok;

    if (t == 0) {               /* save ADDA, MAC and BB, turn path A's ADDA on */
        save_regs(c, adda_regs, c->adda_backup, ADDA_REGS);
        save_mac_regs(c, iqk_mac_regs, c->mac_backup);
        save_regs(c, iqk_bb_regs, c->bb_backup, BB_REGS);
    }
    path_adda_on(c, adda_regs);
    if (t == 0) c->pi_enabled = (r32(c, REG_FPGA0_XA_HSSI_PARM1) & FPGA0_HSSI_PARM1_PI) != 0;
    if (!c->pi_enabled) {       /* BB to PI mode for the calibration */
        w32(c, REG_FPGA0_XA_HSSI_PARM1, 0x01000100);
        w32(c, REG_FPGA0_XB_HSSI_PARM1, 0x01000100);
    }

    mac_calibration(c, iqk_mac_regs, c->mac_backup);
    w32(c, REG_CCK0_AFE_SETTING, replace_bits(r32(c, REG_CCK0_AFE_SETTING), 0xf, 0x0f000000));
    w32(c, REG_OFDM0_TRX_PATH_ENABLE, 0x03a05600);
    w32(c, REG_OFDM0_TR_MUX_PAR, 0x000800e4);
    w32(c, REG_FPGA0_XCD_RF_SW_CTRL, 0x22204000);
    /* no_pape is false on the 8188e */
    w32(c, REG_FPGA0_XAB_RF_SW_CTRL,
        r32(c, REG_FPGA0_XAB_RF_SW_CTRL) | FPGA0_RF_PAPE | FPGA0_RF_PAPE << FPGA0_RF_BD_CTRL_SHIFT);
    w32(c, REG_FPGA0_XA_RF_INT_OE, r32(c, REG_FPGA0_XA_RF_INT_OE) & ~RTL_BIT(10));
    w32(c, REG_FPGA0_XB_RF_INT_OE, r32(c, REG_FPGA0_XB_RF_INT_OE) & ~RTL_BIT(10));
    w32(c, REG_CONFIG_ANT_A, 0x0f600000);           /* page B init */
    w32(c, REG_FPGA0_IQK, replace_bits(r32(c, REG_FPGA0_IQK), 0x808000, 0xffffff00));
    w32(c, REG_TX_IQK, 0x01007c00);
    w32(c, REG_RX_IQK, 0x81004800);

    for (i = 0; i < retry; i++) {
        path_a_ok = iqk_path_a(c);
        if (path_a_ok == 0x01) {
            result[t][0] = (int)((r32(c, REG_TX_POWER_BEFORE_IQK_A) >> 16) & 0x3ff);
            result[t][1] = (int)((r32(c, REG_TX_POWER_AFTER_IQK_A) >> 16) & 0x3ff);
            break;
        }
    }
    for (i = 0; i < retry; i++) {
        path_a_ok = rx_iqk_path_a(c);
        if (path_a_ok == 0x03) {
            result[t][2] = (int)((r32(c, REG_RX_POWER_BEFORE_IQK_A_2) >> 16) & 0x3ff);
            result[t][3] = (int)((r32(c, REG_RX_POWER_AFTER_IQK_A_2) >> 16) & 0x3ff);
            break;
        }
    }

    w32(c, REG_FPGA0_IQK, replace_bits(r32(c, REG_FPGA0_IQK), 0, 0xffffff00));        /* back to BB mode */
    if (t == 0) return;

    if (!c->pi_enabled) {       /* BB back to SI mode */
        w32(c, REG_FPGA0_XA_HSSI_PARM1, 0x01000000);
        w32(c, REG_FPGA0_XB_HSSI_PARM1, 0x01000000);
    }
    restore_regs(c, adda_regs, c->adda_backup, ADDA_REGS);
    restore_mac_regs(c, iqk_mac_regs, c->mac_backup);
    restore_regs(c, iqk_bb_regs, c->bb_backup, BB_REGS);
    w32(c, REG_FPGA0_XA_LSSI_PARM, 0x00032ed3);    /* RX initial gain */
    w32(c, REG_TX_IQK_TONE_A, 0x01008c00);          /* 0xe30 IQC default */
    w32(c, REG_RX_IQK_TONE_A, 0x01008c00);
}

/* rtl8xxxu_simularity_compare, one TX path (path A's four results) */
static bool simularity_compare(int result[][8], int c1, int c2)
{
    const uint32_t bound = 4;
    uint32_t i, j, diff, simubitmap = 0;
    int candidate[2] = { -1, -1 };
    bool retval = true;

    for (i = 0; i < bound; i++) {
        diff = result[c1][i] > result[c2][i] ? (uint32_t)(result[c1][i] - result[c2][i])
                                             : (uint32_t)(result[c2][i] - result[c1][i]);
        if (diff > MAX_TOLERANCE) {
            if ((i == 2 || i == 6) && !simubitmap) {
                if (result[c1][i] + result[c1][i + 1] == 0)
                    candidate[i / 4] = c2;
                else if (result[c2][i] + result[c2][i + 1] == 0)
                    candidate[i / 4] = c1;
                else
                    simubitmap |= 1u << i;
            } else {
                simubitmap |= 1u << i;
            }
        }
    }

    if (simubitmap == 0) {
        for (i = 0; i < bound / 4; i++) {
            if (candidate[i] >= 0) {
                for (j = i * 4; j < (i + 1) * 4 - 2; j++) result[3][j] = result[candidate[i]][j];
                retval = false;
            }
        }
        return retval;
    }
    if (!(simubitmap & 0x0f)) {     /* path A OK */
        for (i = 0; i < 4; i++) result[3][i] = result[c1][i];
    }
    return false;
}

/* rtl8xxxu_fill_iqk_matrix_a. As in rtl8xxxu, tx0_a goes into the register
 * without a mask. */
static void fill_iqk_matrix_a(struct rtl_chip *c, bool iqk_ok, int result[][8], int candidate, bool tx_only)
{
    if (!iqk_ok) return;

    uint32_t oldval = r32(c, REG_OFDM0_XA_TX_IQ_IMBALANCE) >> 22;
    uint32_t x = (uint32_t)result[candidate][0];
    if (x & 0x00000200) x |= 0xfffffc00u;
    uint32_t tx0_a = (x * oldval) >> 8;
    uint32_t v = r32(c, REG_OFDM0_XA_TX_IQ_IMBALANCE);
    v = (v & ~0x3ffu) | tx0_a;
    w32(c, REG_OFDM0_XA_TX_IQ_IMBALANCE, v);
    v = r32(c, REG_OFDM0_ENERGY_CCA_THRES) & ~RTL_BIT(31);
    if ((x * oldval >> 7) & 0x1) v |= RTL_BIT(31);
    w32(c, REG_OFDM0_ENERGY_CCA_THRES, v);

    uint32_t y = (uint32_t)result[candidate][1];
    if (y & 0x00000200) y |= 0xfffffc00u;
    uint32_t tx0_c = (y * oldval) >> 8;
    v = r32(c, REG_OFDM0_XC_TX_AFE) & ~0xf0000000u;
    v |= ((tx0_c & 0x3c0) >> 6) << 28;
    w32(c, REG_OFDM0_XC_TX_AFE, v);
    v = r32(c, REG_OFDM0_XA_TX_IQ_IMBALANCE) & ~0x003f0000u;
    v |= (tx0_c & 0x3f) << 16;
    w32(c, REG_OFDM0_XA_TX_IQ_IMBALANCE, v);
    v = r32(c, REG_OFDM0_ENERGY_CCA_THRES) & ~RTL_BIT(29);
    if ((y * oldval >> 7) & 0x1) v |= RTL_BIT(29);
    w32(c, REG_OFDM0_ENERGY_CCA_THRES, v);

    if (tx_only) return;

    uint32_t reg = (uint32_t)result[candidate][2];
    v = r32(c, REG_OFDM0_XA_RX_IQ_IMBALANCE);
    w32(c, REG_OFDM0_XA_RX_IQ_IMBALANCE, (v & ~0x3ffu) | (reg & 0x3ff));
    reg = (uint32_t)result[candidate][3] & 0x3f;
    v = r32(c, REG_OFDM0_XA_RX_IQ_IMBALANCE);
    w32(c, REG_OFDM0_XA_RX_IQ_IMBALANCE, (v & ~0xfc00u) | ((reg << 10) & 0xfc00));
    reg = ((uint32_t)result[candidate][3] >> 6) & 0xf;
    v = r32(c, REG_OFDM0_RX_IQ_EXT_ANTA);
    w32(c, REG_OFDM0_RX_IQ_EXT_ANTA, (v & ~0xf0000000u) | reg << 28);
}

/* rtl8188eu_phy_iq_calibrate: up to three runs, kept once two agree. */
static void phy_iq_calibrate(struct rtl_chip *c)
{
    int result[4][8];
    int i, candidate = -1;

    memset(result, 0, sizeof result);
    result[3][0] = 0x100;
    result[3][2] = 0x100;
    result[3][4] = 0x100;
    result[3][6] = 0x100;

    for (i = 0; i < 3; i++) {
        phy_iqcalibrate(c, result, i);
        if (i == 1 && simularity_compare(result, 0, 1)) {
            candidate = 0;
            break;
        }
        if (i == 2) {
            if (simularity_compare(result, 0, 2)) {
                candidate = 0;
                break;
            }
            candidate = simularity_compare(result, 1, 2) ? 1 : 3;
        }
    }

    if (candidate >= 0) {
        uint32_t e94 = (uint32_t)result[candidate][0];
        uint32_t ea4 = (uint32_t)result[candidate][2];
        if (e94) fill_iqk_matrix_a(c, true, result, candidate, ea4 == 0);
    }
    save_regs(c, iqk_phy_iq_bb_regs, c->bb_recovery_backup, BB_REGS);
    c->iqk_done = true;
}

/* ------------------------------------------------------------ bring-up */

/* rtl8188e_usb_quirks, with rtl8xxxu_gen2_usb_quirks */
static void usb_quirks(struct rtl_chip *c)
{
    uint16_t v16 = r16(c, REG_CR);
    w16(c, REG_CR, (uint16_t)(v16 | CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE));
    w32(c, REG_TXDMA_OFFSET_CHK, r32(c, REG_TXDMA_OFFSET_CHK) | TXDMA_OFFSET_DROP_DATA_EN);
    w8(c, REG_EARLY_MODE_CONTROL_8188E + 3, 0x01);  /* pre-TX WEP/TKIP */
}

/* rtl8188eu_init_aggregation: one frame per bulk IN transfer. */
static void init_aggregation(struct rtl_chip *c)
{
    uint8_t usb_spec = r8(c, REG_USB_SPECIAL_OPTION);
    w8(c, REG_USB_SPECIAL_OPTION, (uint8_t)(usb_spec & ~USB_SPEC_USB_AGG_ENABLE));
    uint8_t agg_ctrl = r8(c, REG_TRXDMA_CTRL);
    w8(c, REG_TRXDMA_CTRL, (uint8_t)(agg_ctrl & ~TRXDMA_CTRL_RXDMA_AGG_EN));
}

/* rtl8xxxu_set_retry */
static void set_retry(struct rtl_chip *c, unsigned short_retry, unsigned long_retry)
{
    w16(c, REG_RETRY_LIMIT, (uint16_t)((short_retry << RETRY_LIMIT_SHORT_SHIFT & RETRY_LIMIT_SHORT_MASK) |
                                       (long_retry << RETRY_LIMIT_LONG_SHIFT & RETRY_LIMIT_LONG_MASK)));
}

/* rtl8xxxu_set_spec_sifs */
static void set_spec_sifs(struct rtl_chip *c, unsigned cck, unsigned ofdm)
{
    w16(c, REG_SPEC_SIFS, (uint16_t)((cck << SPEC_SIFS_CCK_SHIFT & SPEC_SIFS_CCK_MASK) |
                                     (ofdm << SPEC_SIFS_OFDM_SHIFT & SPEC_SIFS_OFDM_MASK)));
}

/* What rtl8xxxu_configure_filter makes of REG_RCR for mac80211's monitor
 * flags (FIF_BCN_PRBRESP_PROMISC, FIF_CONTROL, FIF_OTHER_BSS, FIF_PSPOLL,
 * no FIF_FCSFAIL), from rtl8xxxu_init_device's value: every unicast
 * (ACCEPT_AP), no BSSID checks, control frames, the PHY status appended.
 * APPEND_FCS is added, as Realtek's own 8188eu driver sets it: rtl_rx_parse
 * strips the 4 bytes, and with the bit set they're there for certain. */
#define MONITOR_RCR (RCR_ACCEPT_AP | RCR_ACCEPT_PHYS_MATCH | RCR_ACCEPT_MCAST | RCR_ACCEPT_BCAST | \
                     RCR_ACCEPT_PM | RCR_ACCEPT_CTRL_FRAME | RCR_ACCEPT_MGMT_FRAME | RCR_HTC_LOC_CTRL | \
                     RCR_APPEND_PHYSTAT | RCR_APPEND_ICV | RCR_APPEND_MIC | RCR_APPEND_FCS)

static bool step_failed(struct rtl_chip *c, bool bad, const char *msg, char *err, size_t errlen)
{
    if (!bad && !c->failed) return false;
    set_err(err, errlen, c->failed ? IO_FAILED : msg);
    return true;
}

/* rtl8xxxu_init_device with rtl8188eu_fops, then what rtl8xxxu_start,
 * add_interface and config do for a monitor on `channel`. */
static int init_device(struct rtl_chip *c, const uint8_t *fw, size_t fwlen, char *err, size_t errlen)
{
    struct qmap m;
    uint8_t v8;
    uint32_t v32;
    int ret = -1;

    /* rtl8xxxu skips the reserved pages, the TX boundary, the LLT table,
     * usb_quirks and the TX reports when it finds the MAC powered (REG_CR
     * not 0xea, the MAC clock on). For the 8188e that would leave MAC TX
     * and RX off, since power_on rewrites REG_CR without them and only
     * usb_quirks turns them on; and Windows' driver may leave the MAC
     * powered. So the 8188e always gets the full init here (macpower
     * false), as rtl8xxxu gives the chips with .needs_full_init. */
    queue_map(c->out_eps, &m);
    if (step_failed(c, power_on(c) < 0, "The Wi-Fi adapter didn't power on.", err, errlen)) return -1;
    init_queue_reserved_page(c, &m);
    init_queue_priority(c, &m);
    w16(c, REG_TRXFF_BNDY + 2, TRXFF_BOUNDARY);    /* RX page boundary */
    /* .hw_feature_report is 0 for the 8188e: no HW feature request. */

    for (int retry = 5; retry >= 0; retry--) {
        ret = download_firmware(c, fw + FW_HEADER_LEN, fwlen - FW_HEADER_LEN);
        if (ret == 0 || c->failed) break;
    }
    if (step_failed(c, ret < 0, "The firmware couldn't be written to the Wi-Fi adapter.", err, errlen)) return -1;
    ret = start_firmware(c);
    if (step_failed(c, ret == -1, "The Wi-Fi adapter didn't confirm the firmware's checksum.", err, errlen)) return -1;
    if (step_failed(c, ret == -2, "The Wi-Fi adapter's firmware didn't start.", err, errlen)) return -1;

    init_mac(c);
    init_phy_bb(c);
    init_phy_rf(c);
    if (step_failed(c, false, NULL, err, errlen)) return -1;

    /* RF switch control; FPGA0_TX_INFO and 0x860 aren't for the 8188e. */
    v32 = FPGA0_RF_TRSW | FPGA0_RF_TRSWB | FPGA0_RF_ANTSW | FPGA0_RF_ANTSWB |
          (FPGA0_RF_ANTSW | FPGA0_RF_ANTSWB) << FPGA0_RF_BD_CTRL_SHIFT;
    v32 |= FPGA0_RF_PAPE | FPGA0_RF_PAPE << FPGA0_RF_BD_CTRL_SHIFT;     /* no_pape is false */
    w32(c, REG_FPGA0_XAB_RF_SW_CTRL, v32);

    v8 = TOTAL_PAGE_NUM + 1;    /* TX buffer boundary */
    w8(c, REG_TXPKTBUF_BCNQ_BDNY, v8);
    w8(c, REG_TXPKTBUF_MGQ_BDNY, v8);
    w8(c, REG_TXPKTBUF_WMAC_LBK_BF_HD, v8);
    w8(c, REG_TRXFF_BNDY, v8);
    w8(c, REG_TDECTRL + 1, v8);
    w8(c, REG_PBP, PBP_PAGE_SIZE_128 << PBP_PAGE_SIZE_RX_SHIFT | PBP_PAGE_SIZE_128 << PBP_PAGE_SIZE_TX_SHIFT);

    if (step_failed(c, init_llt_table(c) < 0, "The Wi-Fi adapter's LLT table couldn't be set up.", err, errlen))
        return -1;
    usb_quirks(c);
    /* TX reports (.has_tx_report): bit 0 for both kinds, the timer for the
     * periodic one rtl8xxxu's rate control reads. rtl_rx_parse skips them. */
    v8 = r8(c, REG_TX_REPORT_CTRL);
    w8(c, REG_TX_REPORT_CTRL, (uint8_t)(v8 | RTL_BIT(0) | TX_REPORT_CTRL_TIMER_ENABLE));
    w8(c, REG_TX_REPORT_CTRL + 1, 0x02);           /* max report MACID */
    w16(c, REG_TX_REPORT_TIME, 0xcdf0);            /* units of 32 us */
    v8 = r8(c, 0xa3);
    w8(c, 0xa3, (uint8_t)(v8 & 0xf8));

    w8(c, REG_RX_DRVINFO_SZ, 4);                   /* PHY status, 4 x 8 bytes, for the RSSI */

    w32(c, REG_HISR0, 0xffffffffu);                 /* 8188e interrupts, over the bulk IN pipe */
    w32(c, REG_HIMR0, IMR0_PSTIMEOUT | IMR0_TBDER | IMR0_CPWM | IMR0_CPWM2);
    w32(c, REG_HIMR1, IMR1_TXERR | IMR1_RXERR | IMR1_TXFOVW | IMR1_RXFOVW);
    v8 = r8(c, REG_USB_SPECIAL_OPTION);
    w8(c, REG_USB_SPECIAL_OPTION, (uint8_t)(v8 | USB_SPEC_INT_BULK_SELECT));

    w32(c, REG_RCR, MONITOR_RCR);
    w32(c, REG_MAR, 0xffffffffu);                   /* every multicast (no .init_reg_rxfltmap) */
    w32(c, REG_MAR + 4, 0xffffffffu);

    v32 = r32(c, REG_RESPONSE_RATE_SET);            /* adaptive controls */
    w32(c, REG_RESPONSE_RATE_SET, (v32 & ~RESPONSE_RATE_BITMAP_ALL) | RESPONSE_RATE_RRSR_CCK_ONLY_1M);

    set_spec_sifs(c, 0x10, 0x10);
    /* rtl8xxxu sets 0x30 (48) retries here; each frame's descriptor decides
     * instead, since 48 held everything queued behind one unicast (#107). */
    set_retry(c, 0, 0);
    set_spec_sifs(c, 0x0a, 0x10);

    w16(c, REG_MAC_SPEC_SIFS, 0x100a);              /* EDCA */
    w16(c, REG_SIFS_CCK, 0x100a);
    w16(c, REG_SIFS_OFDM, 0x100a);
    w32(c, REG_EDCA_BE_PARAM, 0x005ea42b);
    w32(c, REG_EDCA_BK_PARAM, 0x0000a44f);
    w32(c, REG_EDCA_VI_PARAM, 0x005ea324);
    w32(c, REG_EDCA_VO_PARAM, 0x002fa226);
    w32(c, REG_DARFRC, 0x00000000);                 /* data auto rate fallback retries */
    w32(c, REG_DARFRC + 4, 0x10080404);
    w32(c, REG_RARFRC, 0x04030201);
    w32(c, REG_RARFRC + 4, 0x08070605);
    v8 = r8(c, REG_FWHW_TXQ_CTRL);
    w8(c, REG_FWHW_TXQ_CTRL, (uint8_t)(v8 | FWHW_TXQ_CTRL_AMPDU_RETRY));
    w8(c, REG_ACKTO, 0x40);

    w16(c, REG_BEACON_CTRL, BEACON_DISABLE_TSF_UPDATE | BEACON_DISABLE_TSF_UPDATE << 8);
    w16(c, REG_TBTT_PROHIBIT, 0x6404);
    w8(c, REG_DRIVER_EARLY_INT, DRIVER_EARLY_INT_TIME);
    w8(c, REG_BEACON_DMA_TIME, BEACON_DMA_ATIME_INT_TIME);
    w16(c, REG_BEACON_TCFG, 0x660f);

    /* No .init_burst for the 8188e. */
    init_aggregation(c);
    w16(c, REG_PKT_VO_VI_LIFE_TIME, 0x0400);        /* .init_reg_pkt_life_time: 256 ms */
    w16(c, REG_PKT_BE_BK_LIFE_TIME, 0x0400);

    w32(c, REG_FPGA0_RF_MODE, r32(c, REG_FPGA0_RF_MODE) | FPGA_RF_MODE_CCK | FPGA_RF_MODE_OFDM);
    w32(c, REG_CAM_CMD, CAM_CMD_POLLING | RTL_BIT(30));    /* invalidate every CAM entry */
    set_tx_power(c, 1);                             /* rtl8xxxu's default, until config_channel */

    v8 = r8(c, REG_LEDCFG2);                        /* the 8051 controls the antenna */
    w8(c, REG_LEDCFG2, (uint8_t)(v8 | LEDCFG2_DPDT_SELECT));
    w8(c, REG_HWSEQ_CTRL, 0xff);
    w32(c, REG_BAR_MODE_CTRL, 0x0201ffff);
    v8 = r8(c, REG_GPIO_MUXCFG);
    w8(c, REG_GPIO_MUXCFG, (uint8_t)(v8 & ~GPIO_MUXCFG_IO_SEL_ENBT));
    if (step_failed(c, false, NULL, err, errlen)) return -1;

    lc_calibrate(c);
    phy_iq_calibrate(c);
    rf_write(c, RF6052_REG_T_METER_8723B, 0x37cf8); /* .gen2_thermal_meter */
    w8(c, REG_NAV_UPPER, (30000 + NAV_UPPER_UNIT - 1) / NAV_UPPER_UNIT);   /* 30 ms */
    w8(c, REG_USB_HRPWM, 0x00);
    w32(c, REG_FWHW_TXQ_CTRL, r32(c, REG_FWHW_TXQ_CTRL) | FWHW_TXQ_CTRL_XMIT_MGMT_ACK);
    /* Left out: the 0xa9c CCK AGC read, CFO tracking's start and
     * rtl8188e_ra_info_init_all, which only feed the watchdog and rate
     * control. */
    return step_failed(c, false, NULL, err, errlen) ? -1 : 0;
}

/* rtl8xxxu_set_mac, port 0: the address the chip ACKs. */
static void set_mac(struct rtl_chip *c, const uint8_t addr[6])
{
    for (int i = 0; i < 6; i++) w8(c, (uint16_t)(REG_MACID + i), addr[i]);
}

/* rtl8xxxu_stop_tx_beacon */
static void stop_tx_beacon(struct rtl_chip *c)
{
    uint8_t v8 = r8(c, REG_FWHW_TXQ_CTRL + 2);
    w8(c, REG_FWHW_TXQ_CTRL + 2, (uint8_t)(v8 & ~RTL_BIT(6)));
    w8(c, REG_TBTT_PROHIBIT + 1, 0x64);
    v8 = r8(c, REG_TBTT_PROHIBIT + 2);
    w8(c, REG_TBTT_PROHIBIT + 2, (uint8_t)(v8 & ~RTL_BIT(0)));
}

/* rtl8xxxu_start for a monitor, and the parts of add_interface that hold
 * without a station: link type none, the beacon function on (the TSF
 * counts only with it) but never updated from beacons, and the ACK
 * address in REG_MACID. */
static void start_monitor(struct rtl_chip *c, const uint8_t ack_addr[6])
{
    enable_rf(c);
    /* Every data, management and control subtype. */
    w16(c, REG_RXFLTMAP2, 0xffff);
    w16(c, REG_RXFLTMAP1, 0xffff);
    w16(c, REG_RXFLTMAP0, 0xffff);
    w32(c, REG_OFDM0_XA_AGC_CORE1, replace_bits(r32(c, REG_OFDM0_XA_AGC_CORE1), 0x1e, OFDM0_X_AGC_CORE1_IGI_MASK));

    uint8_t v8 = r8(c, REG_MSR);                    /* rtl8xxxu_set_linktype(UNSPECIFIED, 0) */
    w8(c, REG_MSR, (uint8_t)((v8 & 0x0c) | MSR_LINKTYPE_NONE));
    stop_tx_beacon(c);
    v8 = r8(c, REG_BEACON_CTRL);
    w8(c, REG_BEACON_CTRL, (uint8_t)(v8 | BEACON_ATIM | BEACON_FUNCTION_ENABLE | BEACON_DISABLE_TSF_UPDATE));
    set_mac(c, ack_addr);
}

/* ------------------------------------------------------------ API */

int rtl_chip_up(struct rtl_chip *c, const struct rtl_io *io, const uint8_t *out_ep, unsigned out_eps,
                const uint8_t *fw, size_t fwlen, uint8_t channel, const uint8_t ack_addr[6], char *err,
                size_t errlen)
{
    struct qmap m;
    if (err && errlen) err[0] = 0;
    if (!c || !io || !io->read || !io->write || !out_ep || !ack_addr) {
        set_err(err, errlen, "The Wi-Fi adapter wasn't given to the driver.");
        return -1;
    }
    memset(c, 0, sizeof *c);
    c->io = *io;
    if (out_eps > RTL_USB_MAX_OUT || queue_map(out_eps, &m) < 0) {
        set_err(err, errlen, "The Wi-Fi adapter doesn't have the bulk OUT endpoints of an RTL8188EU.");
        return -1;
    }
    c->out_eps = out_eps;
    memcpy(c->out_ep, out_ep, out_eps);
    if (channel < 1 || channel > 14) {
        set_err(err, errlen, "AWDL's channel isn't a 2.4 GHz one.");
        return -1;
    }
    if (check_firmware(fw, fwlen) < 0) {
        set_err(err, errlen, "The firmware isn't firmware rtl8xxxu knows (rtl8188eufw.bin).");
        return -1;
    }

    /* Windows' driver leaves the MCU IO wrapper held in reset, and the 8051
     * then never reports its firmware started (#107): release it first. */
    uint8_t v8 = r8(c, REG_RSV_CTRL + 1);
    w8(c, REG_RSV_CTRL + 1, (uint8_t)(v8 | RSV_CTRL_MCU_IO_WRAPPER));
    if (step_failed(c, false, NULL, err, errlen)) return -1;

    /* rtl8xxxu_probe: identify, efuse, firmware; then init_device. */
    if (identify_chip(c, err, errlen) < 0) return -1;
    int ret = read_efuse(c);
    if (step_failed(c, ret == -1, "The Wi-Fi adapter's efuse couldn't be read.", err, errlen)) return -1;
    if (step_failed(c, ret == -2, "The Wi-Fi adapter's efuse is corrupt.", err, errlen)) return -1;
    if (parse_efuse(c) < 0) {
        set_err(err, errlen, "The Wi-Fi adapter's efuse doesn't hold an RTL8188EU's settings.");
        return -1;
    }
    if (init_device(c, fw, fwlen, err, errlen) < 0) return -1;

    start_monitor(c, ack_addr);
    set_tx_power(c, channel);                       /* rtl8xxxu_config */
    config_channel(c, channel);
    if (step_failed(c, false, NULL, err, errlen)) return -1;
    c->channel = channel;
    c->up = true;
    return 0;
}

int rtl_chip_set_channel(struct rtl_chip *c, uint8_t channel)
{
    if (!c || !c->up || channel < 1 || channel > 14) return -1;
    c->failed = false;
    set_tx_power(c, channel);
    config_channel(c, channel);
    if (c->failed) return -1;
    c->channel = channel;
    return 0;
}

/* REG_TSFTR's two words, the high one read on both sides of the low one so
 * a carry between them is caught. Touches nothing but the registers. */
int rtl_chip_tsf(struct rtl_chip *c, uint64_t *tsf)
{
    uint8_t b[4];
    uint32_t hi, lo, hi2;
    if (!c || !tsf || !c->io.read) return -1;
    if (c->io.read(c->io.ctx, REG_TSFTR + 4, b, 4) < 0) return -1;
    hi = get_le32(b);
    if (c->io.read(c->io.ctx, REG_TSFTR, b, 4) < 0) return -1;
    lo = get_le32(b);
    if (c->io.read(c->io.ctx, REG_TSFTR + 4, b, 4) < 0) return -1;
    hi2 = get_le32(b);
    if (hi2 != hi) {
        if (c->io.read(c->io.ctx, REG_TSFTR, b, 4) < 0) return -1;
        lo = get_le32(b);
    }
    *tsf = (uint64_t)hi2 << 32 | lo;
    return 0;
}

uint32_t rtl_chip_rf_read(struct rtl_chip *c, uint8_t reg)
{
    return c && c->io.read ? rf_read(c, reg) : 0;
}

/* rtl8xxxu_stop's register writes, then rtl8188eu_power_off, as
 * rtl8xxxu_disconnect runs them. */
void rtl_chip_down(struct rtl_chip *c)
{
    if (!c || !c->io.read || !c->io.write) return;
    c->failed = false;
    w8(c, REG_TXPAUSE, 0xff);
    w16(c, REG_RXFLTMAP0, 0x0000);
    w16(c, REG_RXFLTMAP2, 0x0000);
    w8(c, REG_TXPAUSE, 0xff);
    disable_rf(c);
    power_off(c);
    c->up = false;
}

/* ------------------------------------------------------------ RX */

/* rtl8723au_rx_parse_phystats with rtl8188e_cck_rssi: CCK from the LNA and
 * VGA indexes (the TSMC table: cut I, the SMIC one, is refused at
 * bring-up), OFDM from the PWDB. dBm. */
static int8_t rx_rssi(const uint8_t *phy, uint8_t rxmcs)
{
    if (rxmcs < DESC_RATE_6M) {
        static const int8_t lna_gain[8] = { 29, 20, 12, 3, -6, -15, -24, -33 };
        uint8_t agc = phy[5];                       /* cck_agc_rpt_ofdm_cfosho_a */
        int lna_idx = agc >> 5, vga_idx = agc & 0x1f;
        return (int8_t)(lna_gain[lna_idx] - 2 * vga_idx);
    }
    return (int8_t)((phy[4] >> 1) - 110);           /* cck_sig_qual_ofdm_pwdb_all */
}

/* rtl8xxxu_parse_rxdesc16: struct rtl8xxxu_rxdesc16 is six little-endian
 * words; the packet count comes from the first descriptor, and each packet
 * starts on a 128-byte boundary. */
int rtl_rx_parse(const uint8_t *buf, size_t len, void (*fn)(void *ctx, const struct rtl_rx_frame *f), void *ctx)
{
    if (!buf || len < RTL_RXDESC_LEN) return -1;
    int passed = 0;
    unsigned pkt_cnt = 0;
    size_t off = 0;
    for (bool first = true;; first = false) {
        const uint8_t *d = buf + off;
        size_t left = len - off;
        uint32_t w0 = get_le32(d), w2 = get_le32(d + 8), w3 = get_le32(d + 12), tsfl = get_le32(d + 20);
        size_t pkt_len = w0 & 0x3fff;
        bool crc32 = ((w0 >> 14) & 1) != 0, icverr = ((w0 >> 15) & 1) != 0;
        size_t drvinfo = ((w0 >> 16) & 0xf) * 8;
        size_t shift = (w0 >> 24) & 0x3;
        bool phy_stats = ((w0 >> 26) & 1) != 0;
        uint8_t rxmcs = (uint8_t)(w3 & 0x3f);
        unsigned rpt_sel = (w3 >> 14) & 0x3;       /* 8188e: C2H and TX reports */
        if (first) pkt_cnt = (w2 >> 16) & 0xff;

        size_t need = RTL_RXDESC_LEN + drvinfo + shift + pkt_len;
        if (need > left) return -1;                 /* truncated */
        if (!rpt_sel && !crc32 && !icverr && pkt_len > FCS_LEN) {
            struct rtl_rx_frame f = { 0 };
            f.frame = d + RTL_RXDESC_LEN + drvinfo + shift;
            f.len = pkt_len - FCS_LEN;
            f.tsfl = tsfl;
            f.rate = rxmcs;
            if (phy_stats && drvinfo >= 6) {
                f.has_rssi = true;
                f.rssi = rx_rssi(d + RTL_RXDESC_LEN, rxmcs);
            }
            if (fn) fn(ctx, &f);
            passed++;
        }

        size_t step = (need + RX_PKT_ALIGN - 1) / RX_PKT_ALIGN * RX_PKT_ALIGN;
        if (pkt_cnt <= 1 || left <= step) break;    /* the last one announced, or nothing after it */
        if (left - step < RTL_RXDESC_LEN) return -1;    /* a descriptor cut short */
        pkt_cnt--;
        off += step;
    }
    return passed;
}

/* ------------------------------------------------------------ TX */

/* radiotap's legacy rates (500 kbit/s units) as DESC_RATE_* */
static int legacy_rate(uint8_t rate, uint8_t *desc)
{
    static const uint8_t units[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    for (uint8_t i = 0; i < 12; i++) {
        if (units[i] == rate) {
            *desc = i;          /* DESC_RATE_1M ... DESC_RATE_54M */
            return 0;
        }
    }
    return -1;
}

/* rtl8xxxu_calc_tx_desc_csum: the XOR of the descriptor's 16 words, with
 * the checksum's own word at 0 while it's taken. */
static void tx_desc_csum(uint8_t *d)
{
    uint16_t csum = 0;
    d[28] = d[29] = 0;
    for (int i = 0; i < RTL_TXDESC_LEN; i += 2) csum ^= get_le16(d + i);
    put_le16(d + 28, csum);
}

/* rtl8xxxu_tx with rtl8xxxu_fill_txdesc_v3, for an injected frame:
 *  - the rate always the driver's (USE_DRIVER_RATE), and no fallback to
 *    lower rates on retries (DISABLE_DATA_FB, which rtl8xxxu doesn't set:
 *    its rate control picks the rate per frame anyway);
 *  - the retry count always from the options (RETRY_LIMIT_ENABLE, which
 *    rtl8xxxu sets only on management frames, at 6);
 *  - a no-ack frame marked broadcast/multicast (TXDESC_BMC), as rtl8xxxu
 *    marks every frame whose DA is a group address: the MAC then neither
 *    waits for an ACK nor retries; its retry count is 0 as well;
 *  - no RTS/CTS, no A-MPDU, no security, power training stage 0, the
 *    sequence number copied from the frame (no HW_SEQ, as v3 does).
 * rtl8xxxu doesn't pad a transfer whose length is a multiple of the USB
 * packet size (Realtek's driver adds 8 bytes of offset); the transport
 * ends those with a zero-length packet. */
size_t rtl_tx_build(const struct rtl_chip *c, uint8_t *out, size_t cap, const uint8_t *frame, size_t len,
                    const struct rtl_tx_opts *o, uint8_t *ep)
{
    struct qmap m;
    uint8_t rate;
    if (!c || !out || !frame || !o || !ep) return 0;
    if (len < 10 || len > 0xffff - RTL_TXDESC_LEN || cap < RTL_TXDESC_LEN + len) return 0;
    if (o->ht) {
        if (o->rate > 7) return 0;                  /* one spatial stream */
        rate = (uint8_t)(DESC_RATE_MCS0 + o->rate);
    } else if (legacy_rate(o->rate, &rate) < 0) {
        return 0;
    }
    if (queue_map(c->out_eps, &m) < 0) return 0;

    /* The frame control's type and subtype, and ieee80211_get_DA. */
    bool is_data = (frame[0] & 0x0c) == 0x08;
    bool is_qos = (frame[0] & 0x8c) == 0x88;
    bool to_ds = (frame[1] & 0x01) != 0;
    const uint8_t *da = to_ds ? (len >= 22 ? frame + 16 : NULL) : frame + 4;
    bool bmc = (da && (da[0] & 0x01)) || o->no_ack;
    uint16_t seq = len >= 24 ? (uint16_t)(get_le16(frame + 22) >> 4) : 0;
    unsigned retries = o->no_ack ? 0 : (o->retries > 63 ? 63 : o->retries);
    unsigned queue = o->mgmt ? TXDESC_QUEUE_MGNT : TXDESC_QUEUE_BE;

    uint8_t *d = out;
    memset(d, 0, RTL_TXDESC_LEN);
    put_le16(d + 0, (uint16_t)len);                 /* pkt_size */
    d[2] = RTL_TXDESC_LEN;                          /* pkt_offset */
    d[3] = (uint8_t)(TXDESC_OWN | TXDESC_FIRST_SEGMENT | TXDESC_LAST_SEGMENT | (bmc ? TXDESC_BROADMULTICAST : 0));
    put_le32(d + 4, queue << TXDESC_QUEUE_SHIFT);  /* txdw1; MACID 0 */
    put_le32(d + 8, TXDESC40_AGG_BREAK | TXDESC_ANTENNA_SELECT_A | TXDESC_ANTENNA_SELECT_B);
    put_le32(d + 12, (uint32_t)seq << TXDESC32_SEQ_SHIFT);
    put_le32(d + 16, TXDESC32_USE_DRIVER_RATE | TXDESC_DISABLE_DATA_FB | (is_qos ? TXDESC32_QOS : 0));
    uint32_t txdw5 = rate;
    if (is_data) txdw5 |= 0x0001ff00;               /* data/RTS rate FB limit, as v3 */
    if (o->ht && o->sgi) txdw5 |= TXDESC32_SHORT_GI;
    txdw5 |= TXDESC32_RETRY_LIMIT_ENABLE | (retries << TXDESC32_RETRY_LIMIT_SHIFT & TXDESC32_RETRY_LIMIT_MASK);
    put_le32(d + 20, txdw5);
    put_le16(d + 30, (uint16_t)(TXDESC_ANTENNA_SELECT_C >> 16));   /* txdw7 */
    tx_desc_csum(d);

    memcpy(out + RTL_TXDESC_LEN, frame, len);
    *ep = c->out_ep[o->mgmt ? m.mgp : m.bep];
    return RTL_TXDESC_LEN + len;
}
