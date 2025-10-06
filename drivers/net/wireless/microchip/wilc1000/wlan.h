/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2012 - 2018 Microchip Technology Inc., and its subsidiaries.
 * All rights reserved.
 */

#ifndef WILC_WLAN_H
#define WILC_WLAN_H

#include <linux/types.h>
#include <linux/bitfield.h>

/********************************************
 *
 *      Mac eth header length
 *
 ********************************************/
#define MAX_MAC_HDR_LEN			26 /* QOS_MAC_HDR_LEN */
#define SUB_MSDU_HEADER_LENGTH		14
#define SNAP_HDR_LEN			8
#define ETHERNET_HDR_LEN		14
#define WORD_ALIGNMENT_PAD		0

#define ETH_ETHERNET_HDR_OFFSET		(MAX_MAC_HDR_LEN + \
					 SUB_MSDU_HEADER_LENGTH + \
					 SNAP_HDR_LEN - \
					 ETHERNET_HDR_LEN + \
					 WORD_ALIGNMENT_PAD)

#define HOST_HDR_OFFSET			4
#define ETHERNET_HDR_LEN		14
#define IP_HDR_LEN			20
#define IP_HDR_OFFSET			ETHERNET_HDR_LEN
#define UDP_HDR_OFFSET			(IP_HDR_LEN + IP_HDR_OFFSET)
#define UDP_HDR_LEN			8
#define UDP_DATA_OFFSET			(UDP_HDR_OFFSET + UDP_HDR_LEN)
#define ETH_CONFIG_PKT_HDR_LEN		UDP_DATA_OFFSET

#define ETH_CONFIG_PKT_HDR_OFFSET	(ETH_ETHERNET_HDR_OFFSET + \
					 ETH_CONFIG_PKT_HDR_LEN)
#define PKT_STATUS_NEW          0
#define PKT_STATUS_BUFFERED     1

/********************************************
 *
 *      Register Defines
 *
 ********************************************/
#define WILC_PERIPH_REG_BASE		0x1000
#define WILC_CHIPID			WILC_PERIPH_REG_BASE
#define WILC_GLB_RESET_0		(WILC_PERIPH_REG_BASE + 0x400)
#define WILC_PIN_MUX_0			(WILC_PERIPH_REG_BASE + 0x408)
#define WILC_HOST_TX_CTRL		(WILC_PERIPH_REG_BASE + 0x6c)
#define WILC_HOST_RX_CTRL_0		(WILC_PERIPH_REG_BASE + 0x70)
#define WILC_HOST_RX_CTRL_1		(WILC_PERIPH_REG_BASE + 0x74)
#define WILC_HOST_VMM_CTL		(WILC_PERIPH_REG_BASE + 0x78)
#define WILC_HOST_GP_REG		0x24
#define WILC_ARM_GP_REG			0x28
#define WILC_MSG_MAC_OPEN		(0x1 | (0x1 << 8))
#define WILC_MSG_MAC_CLOSE		(0x1 | (0x2 << 8))
#define WILC_MSG_MAC_RESET		(0x1 | (0x3 << 8))

#define WILC_HOST_RX_CTRL		(WILC_PERIPH_REG_BASE + 0x80)
#define WILC_HOST_RX_EXTRA_SIZE		(WILC_PERIPH_REG_BASE + 0x84)
#define WILC_HOST_TX_CTRL_1		(WILC_PERIPH_REG_BASE + 0x88)
#define WILC_MISC			(WILC_PERIPH_REG_BASE + 0x428)
#define WILC_INTR_REG_BASE		(WILC_PERIPH_REG_BASE + 0xa00)

#define WILC_INTERRUPT_CORTUS_0		(WILC_PERIPH_REG_BASE + 0xa8)
#define WILC1000_CORTUS_INTERRUPT_1	(WILC_INTERRUPT_CORTUS_0 + 0x4)
#define WILC3000_CORTUS_INTERRUPT_1	(WILC_INTERRUPT_CORTUS_0 + 0x14)

#define WILC1000_CORTUS_INTERRUPT_2	(WILC_INTERRUPT_CORTUS_0 + 0x8)
#define WILC3000_CORTUS_INTERRUPT_2	(WILC_INTERRUPT_CORTUS_0 + 0x18)

#define WILC_INTR_ENABLE		WILC_INTR_REG_BASE
#define WILC_INTR2_ENABLE		(WILC_INTR_REG_BASE + 4)

#define WILC_INTR_POLARITY		(WILC_INTR_REG_BASE + 0x10)
#define WILC_INTR_TYPE			(WILC_INTR_REG_BASE + 0x20)
#define WILC_INTR_CLEAR			(WILC_INTR_REG_BASE + 0x30)
#define WILC_INTR_STATUS		(WILC_INTR_REG_BASE + 0x40)

#define WILC_RF_REVISION_ID		0x13f4

#define WILC_VMM_TBL_SIZE		64
#define WILC_VMM_TX_TBL_BASE		0x150400
#define WILC_VMM_RX_TBL_BASE		0x150500

#define WILC_VMM_BASE			0x150000
#define WILC_VMM_CORE_CTL		WILC_VMM_BASE
#define WILC_VMM_TBL_CTL		(WILC_VMM_BASE + 0x4)
#define WILC_VMM_TBL_ENTRY		(WILC_VMM_BASE + 0x8)
#define WILC_VMM_TBL0_SIZE		(WILC_VMM_BASE + 0xc)
#define WILC_VMM_TO_HOST_SIZE		(WILC_VMM_BASE + 0x10)
#define WILC_VMM_CORE_CFG		(WILC_VMM_BASE + 0x14)
#define WILC_VMM_TBL_ACTIVE		(WILC_VMM_BASE + 040)
#define WILC_VMM_TBL_STATUS		(WILC_VMM_BASE + 0x44)

#define WILC_SPI_REG_BASE		0xe800
#define WILC_SPI_CTL			WILC_SPI_REG_BASE
#define WILC_SPI_MASTER_DMA_ADDR	(WILC_SPI_REG_BASE + 0x4)
#define WILC_SPI_MASTER_DMA_COUNT	(WILC_SPI_REG_BASE + 0x8)
#define WILC_SPI_SLAVE_DMA_ADDR		(WILC_SPI_REG_BASE + 0xc)
#define WILC_SPI_SLAVE_DMA_COUNT	(WILC_SPI_REG_BASE + 0x10)
#define WILC_SPI_TX_MODE		(WILC_SPI_REG_BASE + 0x20)
#define WILC_SPI_PROTOCOL_CONFIG	(WILC_SPI_REG_BASE + 0x24)
#define WILC_SPI_INTR_CTL		(WILC_SPI_REG_BASE + 0x2c)
#define WILC_SPI_INT_STATUS		(WILC_SPI_REG_BASE + 0x40)
#define WILC_SPI_INT_CLEAR		(WILC_SPI_REG_BASE + 0x44)

/* WILC1000 specific */
#define WILC1000_SPI_WAKEUP_REG	0x1
#define WILC1000_SPI_WAKEUP_BIT	BIT(1)

#define WILC1000_SPI_CLK_STATUS_REG	0x0f
#define WILC1000_SPI_CLK_STATUS_BIT	BIT(2)

/* WILC3000 specific */
#define WILC3000_SPI_WAKEUP_REG	0x1
#define WILC3000_SPI_WAKEUP_BIT	BIT(1)

#define WILC3000_SPI_CLK_STATUS_REG	0x13
#define WILC3000_SPI_CLK_STATUS_BIT	BIT(2)

#define WILC3000_BOOT_REGISTER			0x4F0000
#define WILC3000_BT_CPU_REGISTER		0x3B0090
#define WILC3000_PERIPHERIAL_GLOBAL_REGISTER	0x3B0400

#define WILC_SPI_HOST_TO_FW_REG		0x0b
#define WILC_SPI_HOST_TO_FW_BIT		BIT(0)

#define WILC_SPI_FW_TO_HOST_REG		0x10
#define WILC_SPI_FW_TO_HOST_BIT		BIT(0)

#define WILC_SPI_PROTOCOL_OFFSET	(WILC_SPI_PROTOCOL_CONFIG - \
					 WILC_SPI_REG_BASE)

#define WILC_SPI_CLOCKLESS_ADDR_LIMIT	0x30

/* Functions IO enables bits */
#define WILC_SDIO_CCCR_IO_EN_FUNC1	BIT(1)

/* Function/Interrupt enables bits */
#define WILC_SDIO_CCCR_IEN_MASTER	BIT(0)
#define WILC_SDIO_CCCR_IEN_FUNC1	BIT(1)

/* Abort CCCR register bits */
#define WILC_SDIO_CCCR_ABORT_RESET	BIT(3)

/* Vendor specific CCCR registers */
/* WILC1000 */
#define WILC1000_SDIO_WAKEUP_REG	0xf0
#define WILC1000_SDIO_WAKEUP_BIT	BIT(0)

#define WILC1000_SDIO_CLK_STATUS_REG	0xf1
#define WILC1000_SDIO_CLK_STATUS_BIT	BIT(0)

#define WILC1000_SDIO_IRQ_FLAG_REG	0xf7
#define WILC1000_SDIO_IRQ_CLEAR_FLAG_REG	0xf8

/* WILC3000 specific */
#define WILC3000_SDIO_WAKEUP_REG	0xf0
#define WILC3000_SDIO_WAKEUP_BIT	BIT(0)

#define WILC3000_SDIO_CLK_STATUS_REG	0xf0 /* clk & wakeup are on same reg*/
#define WILC3000_SDIO_CLK_STATUS_BIT	BIT(4)

#define WILC3000_SDIO_IRQ_FLAG_REG	0xfe
#define WILC3000_SDIO_IRQ_CLEAR_FLAG_REG	0xfe
#define WILC3000_SDIO_VMM_TBL_CTRL_REG	0xf1

/* Common vendor specific CCCR register */
#define WILC_SDIO_INTERRUPT_DATA_SZ_REG	0xf2 /* Read size (2 bytes) */

#define WILC_SDIO_VMM_TBL_CTRL_REG	0xf6

#define WILC_SDIO_HOST_TO_FW_REG	0xfa
#define WILC_SDIO_HOST_TO_FW_BIT	BIT(0)

#define WILC_SDIO_FW_TO_HOST_REG	0xfc
#define WILC_SDIO_FW_TO_HOST_BIT	BIT(0)

/* Function 1 specific FBR register */
#define WILC_SDIO_FBR_CSA_REG		0x10C /* CSA pointer (3 bytes) */
#define WILC_SDIO_FBR_DATA_REG		0x10F

#define WILC_SDIO_F1_DATA_REG		0x0
#define WILC_SDIO_EXT_IRQ_FLAG_REG	0x4

#define WILC_AHB_DATA_MEM_BASE		0x30000
#define WILC_AHB_SHARE_MEM_BASE		0xd0000

#define WILC_VMM_TBL_RX_SHADOW_BASE	WILC_AHB_SHARE_MEM_BASE
#define WILC_VMM_TBL_RX_SHADOW_SIZE	256
#define VMM_TBL_RX_SHADOW_BASE		0xA0420
#define VMM_TBL_RX_SHADOW_SIZE		256

#define WILC_GP_REG_0			0x149c
#define WILC_GP_REG_1			0x14a0

#define GLOBAL_MODE_CONTROL		0x1614
#define PWR_SEQ_MISC_CTRL		0x3008

#define WILC_GLOBAL_MODE_ENABLE_WIFI	BIT(0)
#define WILC_PWR_SEQ_ENABLE_WIFI_SLEEP	BIT(28)

#define COE_AUTO_PS_ON_NULL_PKT		0x160468
#define COE_AUTO_PS_OFF_NULL_PKT	0x16046C
#define CCA_CTL_2 (0x160EF4)
#define CCA_CTL_7 (0x160F08)

#define WILC_HAVE_SDIO_IRQ_GPIO		BIT(0)
#define WILC_HAVE_SLEEP_CLK_SRC_RTC	BIT(2)
#define WILC_HAVE_SLEEP_CLK_SRC_XO	BIT(3)

/* tx control register 1 to 4 for RX */
#define WILC_REG_4_TO_1_RX		0x1e1c

/* tx control register 1 to 4 for TX Bank_0 */
#define WILC_REG_4_TO_1_TX_BANK0	0x1e9c

#define WILC_CORTUS_RESET_MUX_SEL	0x1118
#define WILC_CORTUS_BOOT_REGISTER	0xc0000
#define WILC3000_CHIP_ID		0x3b0000
#define WILC_S02_CHIP_ID		0x1
#define WILC_S02_FW_VERSION		0x2

/* WILC_S02- MSG Handler */
#define WILC_S02_CANCEL_RX		0x0
#define WILC_S02_START_WLAN		0x1
#define WILC_S02_HOST_MALLOC		0x2
#define WILC_S02_FW_UPGRADE		0x3
#define WILC_S02_SOFT_RESET		0x4
#define WILC_S02_SDIO_TEST		0x6
#define WILC_S02_SDIO_VMM_CLEAR		0x7
#define WILC_S02_REG_ADDR_WMM_STAT	0x9

/* type of soft resets */
#define WILC_S02_RAM_RESET		BIT(0)
#define WILC_S02_FLASH_RESET		BIT(1)
#define WILC_S02_WLAN_RESET		BIT(2)

#define WILC_S02_REG_ADDR_FW_UPDATE	0x5
#define WILC_S02_REG_ADDR_SHADOW_RX_TBL	0x6

#define WILC_S02_REG_ADDR_CFG		0x8

#define WILC_CORTUS_BOOT_FROM_IRAM	0x71

#define WILC_1000_BASE_ID		0x100000

#define WILC_1000_BASE_ID_2A		0x1002A0
#define WILC_1000_BASE_ID_2A_REV1	(WILC_1000_BASE_ID_2A + 1)

#define WILC_1000_BASE_ID_2B		0x1002B0
#define WILC_1000_BASE_ID_2B_REV1	(WILC_1000_BASE_ID_2B + 1)
#define WILC_1000_BASE_ID_2B_REV2	(WILC_1000_BASE_ID_2B + 2)

#define WILC_3000_BASE_ID		0x300000
#define WILC_S02_BASE_ID		0x000053

#define WILC_CHIP_REV_FIELD		GENMASK(11, 0)

/********************************************
 *
 *      Wlan Defines
 *
 ********************************************/
#define WILC_CFG_PKT		1
#define WILC_NET_PKT		0
#define WILC_MGMT_PKT		2

#define WILC_CFG_SET		1
#define WILC_CFG_QUERY		0

#define WILC_CFG_RSP		1
#define WILC_CFG_RSP_STATUS	2
#define WILC_CFG_RSP_SCAN	3

#define WILC_ABORT_REQ_BIT		BIT(31)

#define WILC_RX_BUFF_SIZE	(96 * 1024)
#define WILC_TX_BUFF_SIZE	(64 * 1024)

#define GPIO_NUM_CHIP_EN	94
#define GPIO_NUM_RESET		60

#define NQUEUES			4
#define ECDA_MAX_LIMIT		32
#define AC_BUFFER_SIZE		1000

#define VO_AC_COUNT_FIELD		GENMASK(31, 25)
#define VO_AC_ACM_STAT_FIELD		BIT(24)
#define VI_AC_COUNT_FIELD		GENMASK(23, 17)
#define VI_AC_ACM_STAT_FIELD		BIT(16)
#define BE_AC_COUNT_FIELD		GENMASK(15, 9)
#define BE_AC_ACM_STAT_FIELD		BIT(8)
#define BK_AC_COUNT_FIELD		GENMASK(7, 3)
#define BK_AC_ACM_STAT_FIELD		BIT(1)

#define WILC_PKT_HDR_CONFIG_FIELD	BIT(31)
#define WILC_PKT_HDR_OFFSET_FIELD	GENMASK(30, 22)
#define WILC_PKT_HDR_TOTAL_LEN_FIELD	GENMASK(21, 11)
#define WILC_PKT_HDR_LEN_FIELD		GENMASK(10, 0)

#define WILC_INTERRUPT_DATA_SIZE	GENMASK(14, 0)

#define WILC_VMM_BUFFER_SIZE		GENMASK(9, 0)

#define WILC_VMM_HDR_TYPE		BIT(31)
#define WILC_VMM_HDR_MGMT_FIELD		BIT(30)
#define WILC_VMM_HDR_PKT_SIZE		GENMASK(29, 15)
#define WILC_VMM_HDR_BUFF_SIZE		GENMASK(14, 0)

#define WILC_VMM_ENTRY_COUNT		GENMASK(8, 3)
#define WILC_VMM_ENTRY_AVAILABLE	BIT(2)
/*******************************************/
/*        E0 and later Interrupt flags.    */
/*******************************************/
/*******************************************/
/*        E0 and later Interrupt flags.    */
/*           IRQ Status word               */
/* 15:0 = DMA count in words.              */
/* 16: INT0 flag                           */
/* 17: INT1 flag                           */
/* 18: INT2 flag                           */
/* 19: INT3 flag                           */
/* 20: INT4 flag                           */
/* 21: INT5 flag                           */
/*******************************************/
#define IRG_FLAGS_OFFSET	16
#define IRQ_DMA_WD_CNT_MASK	GENMASK(IRG_FLAGS_OFFSET - 1, 0)
#define INT_0			BIT(IRG_FLAGS_OFFSET)
#define INT_1			BIT(IRG_FLAGS_OFFSET + 1)
#define INT_2			BIT(IRG_FLAGS_OFFSET + 2)
#define INT_3			BIT(IRG_FLAGS_OFFSET + 3)
#define INT_4			BIT(IRG_FLAGS_OFFSET + 4)
#define MAX_NUM_INT		5
#define IRG_FLAGS_MASK		GENMASK(IRG_FLAGS_OFFSET + MAX_NUM_INT, \
					IRG_FLAGS_OFFSET)

/*******************************************/
/*        E0 and later Interrupt flags.    */
/*           IRQ Clear word                */
/* 0: Clear INT0                           */
/* 1: Clear INT1                           */
/* 2: Clear INT2                           */
/* 3: Clear INT3                           */
/* 4: Clear INT4                           */
/* 5: Clear INT5                           */
/* 6: Select VMM table 1                   */
/* 7: Select VMM table 2                   */
/* 8: Enable VMM                           */
/*******************************************/
#define CLR_INT0		BIT(0)
#define CLR_INT1		BIT(1)
#define CLR_INT2		BIT(2)
#define CLR_INT3		BIT(3)
#define CLR_INT4		BIT(4)
#define CLR_INT5		BIT(5)
#define SEL_VMM_TBL0		BIT(6)
#define SEL_VMM_TBL1		BIT(7)
#define EN_VMM			BIT(8)

#define DATA_INT_EXT		INT_0
#define ALL_INT_EXT		DATA_INT_EXT
#define NUM_INT_EXT		1
#define UNHANDLED_IRQ_MASK	GENMASK(MAX_NUM_INT - 1, NUM_INT_EXT)

#define DATA_INT_CLR		CLR_INT0

#define SDIO_MSG_VMM_INT_CLR	0x7

#define ENABLE_RX_VMM		(SEL_VMM_TBL1 | EN_VMM)
#define DISBLE_RX_VMM		(SDIO_MSG_VMM_INT_CLR | 1 << 8)
#define DISBLE_TX_VMM		(SDIO_MSG_VMM_INT_CLR | 2 << 8)
#define ENABLE_TX_VMM		(SEL_VMM_TBL0 | EN_VMM)
/* time for expiring the completion of cfg packets */
#define WILC_CFG_PKTS_TIMEOUT	msecs_to_jiffies(3000)

#define IS_MANAGMEMENT		0x100
#define IS_MANAGMEMENT_CALLBACK	0x080
#define IS_MGMT_STATUS_SUCCES	0x040
#define IS_MGMT_AUTH_PKT       0x010
#define IS_MON_PKT		0x020

#define WILC_WID_TYPE		GENMASK(15, 12)
#define WILC_VMM_ENTRY_FULL_RETRY	1
/********************************************
 *
 *      Tx/Rx Queue Structure
 *
 ********************************************/
enum ip_pkt_priority {
	AC_VO_Q = 0,
	AC_VI_Q = 1,
	AC_BE_Q = 2,
	AC_BK_Q = 3
};

struct txq_entry_t {
	struct list_head list;
	int type;
	u8 q_num;
	int ack_idx;
	u8 *buffer;
	int buffer_size;
	void *priv;
	int status;
	struct wilc_vif *vif;
	void (*tx_complete_func)(void *priv, int status);
};

struct txq_fw_recv_queue_stat {
	u8 acm;
	u8 count;
};

struct txq_handle {
	struct txq_entry_t txq_head;
	u16 count;
	struct txq_fw_recv_queue_stat fw;
};

struct rxq_entry_t {
	struct list_head list;
	u8 *buffer;
	int buffer_size;
};

struct wilc_vmm_ctl {
	u32 vmm_tbl_rx_shadow_base;
	u32 vmm_tbl_fw_update_base;
	u32 host_vmm_rx_ctl;
	u32 host_vmm_tx_ctl;
	u32 host_tx_ctl;
	u32 host_rx_ctl;
	u32 host_wmm_stat;
};

/********************************************
 *
 *      Host IF Structure
 *
 ********************************************/
struct wilc;
struct wilc_hif_func {
	int (*hif_init)(struct wilc *wilc, bool resume);
	int (*hif_deinit)(struct wilc *wilc);
	int (*hif_read_reg)(struct wilc *wilc, u32 addr, u32 *data);
	int (*hif_write_reg)(struct wilc *wilc, u32 addr, u32 data);
	int (*hif_block_rx)(struct wilc *wilc, u32 addr, u8 *buf, u32 size);
	int (*hif_block_tx)(struct wilc *wilc, u32 addr, u8 *buf, u32 size);
	int (*hif_read_int)(struct wilc *wilc, u32 *int_status);
	int (*hif_clear_int_ext)(struct wilc *wilc, u32 val);
	int (*hif_read_size)(struct wilc *wilc, u32 *size);
	int (*hif_block_tx_ext)(struct wilc *wilc, u32 addr, u8 *buf, u32 size);
	int (*hif_block_rx_ext)(struct wilc *wilc, u32 addr, u8 *buf, u32 size);
	int (*hif_sync_ext)(struct wilc *wilc, int nint);
	int (*enable_interrupt)(struct wilc *nic);
	void (*disable_interrupt)(struct wilc *nic);
	int (*hif_reset)(struct wilc *wilc);
	bool (*hif_is_init)(struct wilc *wilc);
	int (*hif_clear_init)(struct wilc *wilc);
};

#define WILC_MAX_CFG_FRAME_SIZE		1468

struct tx_complete_data {
	int size;
	void *buff;
	struct sk_buff *skb;
};

struct wilc_cfg_cmd_hdr {
	u8 cmd_type;
	u8 seq_no;
	__le16 total_len;
	__le32 driver_handler;
};

struct wilc_cfg_frame {
	struct wilc_cfg_cmd_hdr hdr;
	u8 frame[WILC_MAX_CFG_FRAME_SIZE];
};

struct wilc_cfg_rsp {
	u8 type;
	u8 seq_no;
};

struct wilc_vif;

static inline bool is_wilc1000(u32 id)
{
	return (id & (~WILC_CHIP_REV_FIELD)) == WILC_1000_BASE_ID;
}

static inline bool is_wilc3000(u32 id)
{
	return (id & (~WILC_CHIP_REV_FIELD)) == WILC_3000_BASE_ID;
}

static inline bool is_wilcs02(u32 id)
{
	return (id & (WILC_CHIP_REV_FIELD)) == WILC_S02_BASE_ID;
}

int wilc_wlan_firmware_download(struct wilc *wilc, const u8 *buffer,
				u32 buffer_size);
int wilc_wlan_start(struct wilc *wilc);
int wilc_wlan_start_wilcs02_fw(struct wilc *wilc);
int wilc_wlan_stop(struct wilc *wilc, struct wilc_vif *vif);
int wilc_wlan_txq_add_net_pkt(struct net_device *dev,
			      struct tx_complete_data *tx_data, u8 *buffer,
			      u32 buffer_size,
			      void (*tx_complete_fn)(void *, int));
int wilc_wlan_handle_txq(struct wilc *wl, u32 *txq_count);
void wilc_wfi_handle_monitor_rx(struct wilc *wilc, u8 *buff, u32 size);
void wilc_handle_isr(struct wilc *wilc);
void wilc_wlan_cleanup(struct net_device *dev);
int wilc_wlan_cfg_set(struct wilc_vif *vif, int start, u16 wid, u8 *buffer,
		      u32 buffer_size, int commit, u32 drv_handler);
int wilc_wlan_cfg_get(struct wilc_vif *vif, int start, u16 wid, int commit,
		      u32 drv_handler);
int wilc_wlan_txq_add_mgmt_pkt(struct net_device *dev, void *priv, u8 *buffer,
			       u32 buffer_size, void (*func)(void *, int));
void wilc_enable_tcp_ack_filter(struct wilc_vif *vif, bool value);
netdev_tx_t wilc_mac_xmit(struct sk_buff *skb, struct net_device *dev);

bool wilc_wfi_p2p_rx(struct wilc_vif *vif, u8 *buff, u32 size);
bool wilc_wfi_mgmt_frame_rx(struct wilc_vif *vif, u8 *buff, u32 size);
void host_wakeup_notify(struct wilc *wilc, int source);
void host_sleep_notify(struct wilc *wilc, int source);
void chip_allow_sleep(struct wilc *wilc, int source);
void chip_wakeup(struct wilc *wilc, int source);
int wilc_send_config_pkt(struct wilc_vif *vif, u8 mode, struct wid *wids,
			 u32 count);
void wilc_bt_init(struct wilc *wilc);
void wilc_bt_deinit(void);
void eap_buff_timeout(struct timer_list *t);
void acquire_bus(struct wilc *wilc, enum bus_acquire acquire, int source);
void release_bus(struct wilc *wilc, enum bus_release release, int source);
int wilc_wlan_init(struct net_device *dev);
u32 wilc_get_chipid(struct wilc *wilc, bool update);
int wilcs02_init_vmm_registers(struct wilc *wilc);
int wilc3000_prepare_ble_ram(struct wilc *wilc);
#endif
