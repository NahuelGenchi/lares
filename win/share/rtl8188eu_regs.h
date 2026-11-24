// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2014 - 2017 Jes Sorensen
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
/* The RTL8188EU's registers and descriptor bits that rtl8188eu.c uses, as
 * Linux rtl8xxxu's regs.h and rtl8xxxu.h name them (issue #109), and the
 * init tables of rtl8188eu_tables.c. Private to the driver. */
#include <stdint.h>

#define RTL_BIT(n) (1u << (n))

/* 0x0000 - 0x00ff: system configuration */
#define REG_SYS_ISO_CTRL            0x0000
#define  SYS_ISO_PWC_EV12V          RTL_BIT(15)
#define REG_SYS_FUNC                0x0002
#define  SYS_FUNC_BBRSTB            RTL_BIT(0)
#define  SYS_FUNC_BB_GLB_RSTN       RTL_BIT(1)
#define  SYS_FUNC_USBA              RTL_BIT(2)
#define  SYS_FUNC_USBD              RTL_BIT(4)
#define  SYS_FUNC_CPU_ENABLE        RTL_BIT(10)
#define  SYS_FUNC_ELDR              RTL_BIT(12)
#define  SYS_FUNC_DIO_RF            RTL_BIT(13)
#define REG_APS_FSMCO               0x0004
#define  APS_FSMCO_MAC_ENABLE       RTL_BIT(8)
#define  APS_FSMCO_MAC_OFF          RTL_BIT(9)  /* Realtek's APFM_OFF */
#define  APS_FSMCO_HW_SUSPEND       RTL_BIT(11)
#define  APS_FSMCO_PCIE             RTL_BIT(12)
#define  APS_FSMCO_HW_POWERDOWN     RTL_BIT(15)
#define REG_SYS_CLKR                0x0008
#define  SYS_CLK_ANA8M              RTL_BIT(1)
#define  SYS_CLK_LOADER_ENABLE      RTL_BIT(5)
#define  SYS_CLK_MAC_CLK_ENABLE     RTL_BIT(11)
#define REG_RSV_CTRL                0x001c
#define  RSV_CTRL_MCU_IO_WRAPPER    RTL_BIT(3)  /* of REG_RSV_CTRL + 1 (#107) */
#define REG_RF_CTRL                 0x001f
#define  RF_ENABLE                  RTL_BIT(0)
#define  RF_RSTB                    RTL_BIT(1)
#define  RF_SDMRSTB                 RTL_BIT(2)
#define REG_LPLDO_CTRL              0x0023
#define REG_AFE_XTAL_CTRL           0x0024
#define  AFE_XTAL1_MASK             0x007e0000u /* GENMASK(22, 17), 8188f.c's XTAL1 */
#define  AFE_XTAL0_MASK             0x0001f800u /* GENMASK(16, 11), 8188f.c's XTAL0 */
#define REG_EFUSE_CTRL              0x0030
#define  EFUSE_ACCESS_ENABLE        0x69
#define  EFUSE_ACCESS_DISABLE       0x00
#define REG_GPIO_MUXCFG             0x0040
#define  GPIO_MUXCFG_IO_SEL_ENBT    RTL_BIT(5)
#define REG_GPIO_IO_SEL             0x0042
#define REG_GPIO_PIN_CTRL           0x0044
#define REG_LEDCFG2                 0x004e
#define  LEDCFG2_DPDT_SELECT        RTL_BIT(7)
#define REG_PAD_CTRL1               0x0064
#define REG_MCU_FW_DL               0x0080
#define  MCU_FW_DL_ENABLE           RTL_BIT(0)
#define  MCU_FW_DL_READY            RTL_BIT(1)
#define  MCU_FW_DL_CSUM_REPORT      RTL_BIT(2)
#define  MCU_WINT_INIT_READY        RTL_BIT(6)
#define  MCU_FW_RAM_SEL             RTL_BIT(7)
#define REG_HISR0                   0x00b4
#define REG_HIMR0                   0x00b0
#define  IMR0_PSTIMEOUT             RTL_BIT(29)
#define  IMR0_TBDER                 RTL_BIT(26)
#define  IMR0_CPWM2                 RTL_BIT(9)
#define  IMR0_CPWM                  RTL_BIT(8)
#define REG_HIMR1                   0x00b8
#define  IMR1_TXERR                 RTL_BIT(11)
#define  IMR1_RXERR                 RTL_BIT(10)
#define  IMR1_TXFOVW                RTL_BIT(9)
#define  IMR1_RXFOVW                RTL_BIT(8)
#define REG_EFUSE_ACCESS            0x00cf
#define REG_SYS_CFG                 0x00f0
#define  SYS_CFG_VENDOR_ID          RTL_BIT(19)
#define  SYS_CFG_TRP_VAUX_EN        RTL_BIT(23)
#define  SYS_CFG_CHIP_VERSION_MASK  0xf000u

/* 0x0100 - 0x01ff: MAC top */
#define REG_CR                      0x0100
#define  CR_HCI_TXDMA_ENABLE        RTL_BIT(0)
#define  CR_HCI_RXDMA_ENABLE        RTL_BIT(1)
#define  CR_TXDMA_ENABLE            RTL_BIT(2)
#define  CR_RXDMA_ENABLE            RTL_BIT(3)
#define  CR_PROTOCOL_ENABLE         RTL_BIT(4)
#define  CR_SCHEDULE_ENABLE         RTL_BIT(5)
#define  CR_MAC_TX_ENABLE           RTL_BIT(6)
#define  CR_MAC_RX_ENABLE           RTL_BIT(7)
#define  CR_SECURITY_ENABLE         RTL_BIT(9)
#define  CR_CALTIMER_ENABLE         RTL_BIT(10)
#define REG_MSR                     0x0102
#define  MSR_LINKTYPE_NONE          0x0
#define REG_PBP                     0x0104
#define  PBP_PAGE_SIZE_RX_SHIFT     0
#define  PBP_PAGE_SIZE_TX_SHIFT     4
#define  PBP_PAGE_SIZE_128          0x1
#define REG_TRXDMA_CTRL             0x010c
#define  TRXDMA_CTRL_RXDMA_AGG_EN   RTL_BIT(2)
#define  TRXDMA_CTRL_VOQ_SHIFT      4
#define  TRXDMA_CTRL_VIQ_SHIFT      6
#define  TRXDMA_CTRL_BEQ_SHIFT      8
#define  TRXDMA_CTRL_BKQ_SHIFT      10
#define  TRXDMA_CTRL_MGQ_SHIFT      12
#define  TRXDMA_CTRL_HIQ_SHIFT      14
#define  TRXDMA_QUEUE_LOW           1
#define  TRXDMA_QUEUE_NORMAL        2
#define  TRXDMA_QUEUE_HIGH          3
#define REG_TRXFF_BNDY              0x0114
#define REG_32K_CTRL                0x0194
#define REG_HMTFR                   0x01cc
#define REG_LLT_INIT                0x01e0
#define  LLT_OP_INACTIVE            0x0u
#define  LLT_OP_WRITE               (0x1u << 30)
#define  LLT_OP_MASK                (0x3u << 30)

/* 0x0200 - 0x02ff: TX and RX DMA */
#define REG_RQPN                    0x0200
#define  RQPN_HI_PQ_SHIFT           0
#define  RQPN_LO_PQ_SHIFT           8
#define  RQPN_PUB_PQ_SHIFT          16
#define  RQPN_LOAD                  RTL_BIT(31)
#define REG_TDECTRL                 0x0208
#define REG_TXDMA_OFFSET_CHK        0x020c
#define  TXDMA_OFFSET_DROP_DATA_EN  RTL_BIT(9)
#define REG_RQPN_NPQ                0x0214
#define  RQPN_NPQ_SHIFT             0
#define  RQPN_EPQ_SHIFT             16
#define REG_RXPKT_NUM               0x0284
#define  RXPKT_NUM_RXDMA_IDLE       RTL_BIT(17)
#define  RXPKT_NUM_RW_RELEASE_EN    RTL_BIT(18)

/* 0x0400 - 0x04ff: protocol */
#define REG_FWHW_TXQ_CTRL           0x0420
#define  FWHW_TXQ_CTRL_AMPDU_RETRY  RTL_BIT(7)
#define  FWHW_TXQ_CTRL_XMIT_MGMT_ACK RTL_BIT(12)
#define REG_HWSEQ_CTRL              0x0423
#define REG_TXPKTBUF_BCNQ_BDNY      0x0424
#define REG_TXPKTBUF_MGQ_BDNY       0x0425
#define REG_SPEC_SIFS               0x0428
#define  SPEC_SIFS_CCK_MASK         0x00ffu
#define  SPEC_SIFS_CCK_SHIFT        0
#define  SPEC_SIFS_OFDM_MASK        0xff00u
#define  SPEC_SIFS_OFDM_SHIFT       8
#define REG_RETRY_LIMIT             0x042a
#define  RETRY_LIMIT_LONG_SHIFT     0
#define  RETRY_LIMIT_LONG_MASK      0x003fu
#define  RETRY_LIMIT_SHORT_SHIFT    8
#define  RETRY_LIMIT_SHORT_MASK     0x3f00u
#define REG_DARFRC                  0x0430
#define REG_RARFRC                  0x0438
#define REG_RESPONSE_RATE_SET       0x0440
#define  RESPONSE_RATE_BITMAP_ALL   0xfffffu
#define  RESPONSE_RATE_RRSR_CCK_ONLY_1M 0xffff1u
#define REG_TXPKTBUF_WMAC_LBK_BF_HD 0x045d
#define REG_PKT_VO_VI_LIFE_TIME     0x04c0
#define REG_PKT_BE_BK_LIFE_TIME     0x04c2
#define REG_MAX_AGGR_NUM            0x04ca
#define REG_BAR_MODE_CTRL           0x04cc
#define REG_EARLY_MODE_CONTROL_8188E 0x04d0
#define REG_TX_REPORT_CTRL          0x04ec
#define  TX_REPORT_CTRL_TIMER_ENABLE RTL_BIT(1)
#define REG_TX_REPORT_TIME          0x04f0

/* 0x0500 - 0x05ff: EDCA */
#define REG_EDCA_VO_PARAM           0x0500
#define REG_EDCA_VI_PARAM           0x0504
#define REG_EDCA_BE_PARAM           0x0508
#define REG_EDCA_BK_PARAM           0x050c
#define REG_BEACON_TCFG             0x0510
#define REG_SIFS_CCK                0x0514
#define REG_SIFS_OFDM               0x0516
#define REG_TXPAUSE                 0x0522
#define REG_TBTT_PROHIBIT           0x0540
#define REG_BEACON_CTRL             0x0550
#define REG_BEACON_CTRL_1           0x0551
#define  BEACON_ATIM                RTL_BIT(0)
#define  BEACON_FUNCTION_ENABLE     RTL_BIT(3)
#define  BEACON_DISABLE_TSF_UPDATE  RTL_BIT(4)
#define REG_DUAL_TSF_RST            0x0553
#define  DUAL_TSF_TX_OK             RTL_BIT(5)
#define REG_DRIVER_EARLY_INT        0x0558
#define  DRIVER_EARLY_INT_TIME      5
#define REG_BEACON_DMA_TIME         0x0559
#define  BEACON_DMA_ATIME_INT_TIME  2
#define REG_TSFTR                   0x0560
#define REG_SCH_TX_CMD              0x05f8

/* 0x0600 - 0x07ff: WMAC */
#define REG_BW_OPMODE               0x0603
#define  BW_OPMODE_20MHZ            RTL_BIT(2)
#define REG_RCR                     0x0608
#define  RCR_ACCEPT_AP              RTL_BIT(0)
#define  RCR_ACCEPT_PHYS_MATCH      RTL_BIT(1)
#define  RCR_ACCEPT_MCAST           RTL_BIT(2)
#define  RCR_ACCEPT_BCAST           RTL_BIT(3)
#define  RCR_ACCEPT_PM              RTL_BIT(5)
#define  RCR_CHECK_BSSID_MATCH      RTL_BIT(6)
#define  RCR_CHECK_BSSID_BEACON     RTL_BIT(7)
#define  RCR_ACCEPT_CRC32           RTL_BIT(8)
#define  RCR_ACCEPT_CTRL_FRAME      RTL_BIT(12)
#define  RCR_ACCEPT_MGMT_FRAME      RTL_BIT(13)
#define  RCR_HTC_LOC_CTRL           RTL_BIT(14)
#define  RCR_APPEND_PHYSTAT         RTL_BIT(28)
#define  RCR_APPEND_ICV             RTL_BIT(29)
#define  RCR_APPEND_MIC             RTL_BIT(30)
#define  RCR_APPEND_FCS             RTL_BIT(31)
#define REG_RX_DRVINFO_SZ           0x060f
#define REG_MACID                   0x0610
#define REG_MAR                     0x0620
#define REG_MAC_SPEC_SIFS           0x063a
#define REG_ACKTO                   0x0640
#define REG_NAV_UPPER               0x0652
#define  NAV_UPPER_UNIT             128
#define REG_CAM_CMD                 0x0670
#define  CAM_CMD_POLLING            RTL_BIT(31)
#define REG_RXFLTMAP0               0x06a0  /* management frames */
#define REG_RXFLTMAP1               0x06a2  /* control frames */
#define REG_RXFLTMAP2               0x06a4  /* data frames */

/* 0x0800 - 0x0fff: baseband */
#define REG_FPGA0_RF_MODE           0x0800
#define  FPGA_RF_MODE               RTL_BIT(0)
#define  FPGA_RF_MODE_CCK           RTL_BIT(24)
#define  FPGA_RF_MODE_OFDM          RTL_BIT(25)
#define REG_FPGA0_XA_HSSI_PARM1     0x0820
#define  FPGA0_HSSI_PARM1_PI        RTL_BIT(8)
#define REG_FPGA0_XA_HSSI_PARM2     0x0824
#define REG_FPGA0_XB_HSSI_PARM1     0x0828
#define  FPGA0_HSSI_3WIRE_DATA_LEN  0x800u
#define  FPGA0_HSSI_3WIRE_ADDR_LEN  0x400u
#define  FPGA0_HSSI_PARM2_ADDR_SHIFT 23
#define  FPGA0_HSSI_PARM2_ADDR_MASK 0x7f800000u
#define  FPGA0_HSSI_PARM2_EDGE_READ RTL_BIT(31)
#define REG_FPGA0_XA_LSSI_PARM      0x0840
#define  FPGA0_LSSI_PARM_ADDR_SHIFT 20
#define  FPGA0_LSSI_PARM_DATA_MASK  0x000fffffu
#define REG_FPGA0_XCD_SWITCH_CTRL   0x085c
#define REG_FPGA0_XA_RF_INT_OE      0x0860
#define REG_FPGA0_XB_RF_INT_OE      0x0864
#define REG_TX_AGC_B_CCK11_A_CCK2_11 0x086c
#define REG_FPGA0_XAB_RF_SW_CTRL    0x0870
#define REG_FPGA0_XA_RF_SW_CTRL     0x0870  /* 16 bit */
#define REG_FPGA0_XCD_RF_SW_CTRL    0x0874
#define  FPGA0_RF_RFENV             RTL_BIT(4)
#define  FPGA0_RF_TRSW              RTL_BIT(5)
#define  FPGA0_RF_TRSWB             RTL_BIT(6)
#define  FPGA0_RF_ANTSW             RTL_BIT(8)
#define  FPGA0_RF_ANTSWB            RTL_BIT(9)
#define  FPGA0_RF_PAPE              RTL_BIT(10)
#define  FPGA0_RF_BD_CTRL_SHIFT     16
#define REG_FPGA0_XA_LSSI_READBACK  0x08a0
#define REG_HSPI_XA_READBACK        0x08b8
#define REG_FPGA1_RF_MODE           0x0900
#define REG_CCK0_AFE_SETTING        0x0a04
#define REG_CONFIG_ANT_A            0x0b68
#define REG_CONFIG_ANT_B            0x0b6c
#define REG_OFDM0_TRX_PATH_ENABLE   0x0c04
#define  OFDM_RF_PATH_RX_MASK       0x0fu
#define  OFDM_RF_PATH_RX_A          RTL_BIT(0)
#define  OFDM_RF_PATH_TX_MASK       0xf0u
#define  OFDM_RF_PATH_TX_A          RTL_BIT(4)
#define REG_OFDM0_TR_MUX_PAR        0x0c08
#define REG_OFDM0_XA_RX_IQ_IMBALANCE 0x0c14
#define REG_OFDM0_XB_RX_IQ_IMBALANCE 0x0c1c
#define REG_OFDM0_ENERGY_CCA_THRES  0x0c4c
#define REG_OFDM0_XA_AGC_CORE1      0x0c50
#define  OFDM0_X_AGC_CORE1_IGI_MASK 0x0000007fu
#define REG_OFDM0_AGC_RSSI_TABLE    0x0c78
#define REG_OFDM0_XA_TX_IQ_IMBALANCE 0x0c80
#define REG_OFDM0_XB_TX_IQ_IMBALANCE 0x0c88
#define REG_OFDM0_XC_TX_AFE         0x0c94
#define REG_OFDM0_XD_TX_AFE         0x0c9c
#define REG_OFDM0_RX_IQ_EXT_ANTA    0x0ca0
#define REG_OFDM1_LSTF              0x0d00
#define  OFDM_LSTF_MASK             0x70000000u
#define REG_TX_AGC_A_RATE18_06      0x0e00
#define REG_TX_AGC_A_RATE54_24      0x0e04
#define REG_TX_AGC_A_CCK1_MCS32     0x0e08
#define REG_TX_AGC_A_MCS03_MCS00    0x0e10
#define REG_TX_AGC_A_MCS07_MCS04    0x0e14
#define REG_TX_AGC_A_MCS11_MCS08    0x0e18
#define REG_TX_AGC_A_MCS15_MCS12    0x0e1c
#define REG_FPGA0_IQK               0x0e28
#define REG_TX_IQK_TONE_A           0x0e30
#define REG_RX_IQK_TONE_A           0x0e34
#define REG_TX_IQK_PI_A             0x0e38
#define REG_RX_IQK_PI_A             0x0e3c
#define REG_TX_IQK                  0x0e40
#define REG_RX_IQK                  0x0e44
#define REG_IQK_AGC_PTS             0x0e48
#define REG_IQK_AGC_RSP             0x0e4c
#define REG_BLUETOOTH               0x0e6c
#define REG_RX_WAIT_CCA             0x0e70
#define REG_TX_CCK_RFON             0x0e74
#define REG_TX_CCK_BBON             0x0e78
#define REG_TX_OFDM_RFON            0x0e7c
#define REG_TX_OFDM_BBON            0x0e80
#define REG_TX_TO_RX                0x0e84
#define REG_TX_TO_TX                0x0e88
#define REG_RX_CCK                  0x0e8c
#define REG_TX_POWER_BEFORE_IQK_A   0x0e94
#define REG_TX_POWER_AFTER_IQK_A    0x0e9c
#define REG_RX_POWER_BEFORE_IQK_A_2 0x0ea4
#define REG_RX_POWER_AFTER_IQK_A_2  0x0eac
#define REG_RX_OFDM                 0x0ed0
#define REG_RX_WAIT_RIFS            0x0ed4
#define REG_RX_TO_RX                0x0ed8
#define REG_STANDBY                 0x0edc
#define REG_SLEEP                   0x0ee0
#define REG_PMPD_ANAEN              0x0eec

/* The firmware's window: one 4 KiB page at a time, picked in
 * REG_MCU_FW_DL + 2. */
#define REG_FW_START_ADDRESS        0x1000

/* USB */
#define REG_USB_SPECIAL_OPTION      0xfe55
#define  USB_SPEC_USB_AGG_ENABLE    RTL_BIT(3)
#define  USB_SPEC_INT_BULK_SELECT   RTL_BIT(4)
#define REG_USB_HRPWM               0xfe58

/* RF6052 registers (path A, through the LSSI) */
#define RF6052_REG_AC               0x00
#define RF6052_REG_MODE_AG          0x18
#define  MODE_AG_CHANNEL_MASK       0x3ffu
#define  MODE_AG_BW_MASK            (RTL_BIT(10) | RTL_BIT(11))
#define  MODE_AG_BW_20MHZ_8723B     (RTL_BIT(10) | RTL_BIT(11))
#define RF6052_REG_RCK_OS           0x30
#define RF6052_REG_TXPA_G1          0x31
#define RF6052_REG_TXPA_G2          0x32
#define RF6052_REG_T_METER_8723B    0x42
#define RF6052_REG_WE_LUT           0xef

/* TX descriptor (struct rtl8xxxu_txdesc32): pkt_size (le16), pkt_offset,
 * txdw0 (one byte), then txdw1 - txdw6 (le32), csum (le16), txdw7 (le16). */
#define TXDESC_BROADMULTICAST       RTL_BIT(0)  /* txdw0; TXDESC_BMC, bit 24 of the first word */
#define TXDESC_LAST_SEGMENT         RTL_BIT(2)
#define TXDESC_FIRST_SEGMENT        RTL_BIT(3)
#define TXDESC_OWN                  RTL_BIT(7)
#define TXDESC_QUEUE_SHIFT          8           /* txdw1 */
#define TXDESC_QUEUE_BE             0x0
#define TXDESC_QUEUE_MGNT           0x12
#define TXDESC40_AGG_BREAK          RTL_BIT(16) /* txdw2, as fill_txdesc_v3 uses it */
#define TXDESC_ANTENNA_SELECT_A     RTL_BIT(24)
#define TXDESC_ANTENNA_SELECT_B     RTL_BIT(25)
#define TXDESC32_SEQ_SHIFT          16          /* txdw3 */
#define TXDESC32_QOS                RTL_BIT(6)  /* txdw4 */
#define TXDESC32_USE_DRIVER_RATE    RTL_BIT(8)
#define TXDESC_DISABLE_DATA_FB      RTL_BIT(10)
#define TXDESC32_SHORT_GI           RTL_BIT(6)  /* txdw5 */
#define TXDESC32_RETRY_LIMIT_ENABLE RTL_BIT(17)
#define TXDESC32_RETRY_LIMIT_SHIFT  18
#define TXDESC32_RETRY_LIMIT_MASK   0x00fc0000u
#define TXDESC_ANTENNA_SELECT_C     RTL_BIT(29) /* txdw7, shifted down 16 */

/* Rates, as descriptors give them */
#define DESC_RATE_1M                0x00
#define DESC_RATE_6M                0x04
#define DESC_RATE_MCS0              0x0c

/* Init tables: rtl8xxxu's, ending in their sentinels. */
struct rtl_reg8val { uint16_t reg; uint8_t val; };
struct rtl_reg32val { uint16_t reg; uint32_t val; };
struct rtl_rfregval { uint8_t reg; uint32_t val; };

extern const struct rtl_reg8val rtl8188e_mac_init_table[];
extern const struct rtl_reg32val rtl8188eu_phy_init_table[];
extern const struct rtl_reg32val rtl8188e_agc_table[];
extern const struct rtl_rfregval rtl8188eu_radioa_init_table[];
