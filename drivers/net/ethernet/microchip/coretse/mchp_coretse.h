/* SPDX-License-Identifier: (GPL-2.0) */
/**
 * Microchip coreTSE(Triple Speed Ethernet) MAC driver
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Praveen Kumar Vattipalli <praveen.kumar@microchip.com>
 *
 */

#ifndef _CORETSE_H
#define _CORETSE_H

#include <linux/clk.h>
#include <linux/phylink.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/net_tstamp.h>
#include <linux/interrupt.h>
#include <linux/phy/phy.h>

#define CORETSE_CONFIG1			0x00 /* MAC Configuration register 1 */
#define CORETSE_CONFIG2			0x04 /* MAC Configuration register 2 */
#define CORETSE_IFG			0x08 /* Inter Packet/Frame gaps	     */
#define CORETSE_HALF_DUPLEX		0x0C /* Definition of half duplex */
#define CORETSE_MAX_FRAME_LEN		0x10 /* Maximum frame size */
#define CORETSE_CTRL_FRAME_EXT		0x14
#define CORETSE_CTRL_FRAME		0x18
#define CORETSE_TEST			0x1C /* For testing purposes */
#define CORETSE_MII_CONFIG		0x20 /* MII configuration */
#define CORETSE_MII_COMMAND		0x24 /* MII command */
#define CORETSE_MII_ADDRESS		0x28 /* 5-bit PHY addr */
#define CORETSE_MII_CTRL		0x2C /* MII Mgmt write cycle control */
#define CORETSE_MII_STATUS		0x30 /* MII Mgmt read cycle status */
#define CORETSE_MII_IND			0x34 /* MII Mgmt indication */
#define CORETSE_IF_CTRL			0x38 /* Interface controls */
#define CORETSE_IF_STATUS		0x3C /* Interface status */
#define CORETSE_STATION_ADDR0		0x40 /* Station MAC address */
#define CORETSE_STATION_ADDR1		0x44
#define CORETSE_FIFO_CONFIG0		0x48 /* A-MCXFIFO config registers */
#define CORETSE_FIFO_CONFIG1		0x4C
#define CORETSE_FIFO_CONFIG2		0x50
#define CORETSE_FIFO_CONFIG3		0x54
#define CORETSE_FIFO_CONFIG4		0x58
#define CORETSE_FIFO_CONFIG5		0x5C
#define CORETSE_FIFO_RAM_ACCESS0	0x60 /* FIFO RAM access registers */
#define CORETSE_FIFO_RAM_ACCESS1	0x64
#define CORETSE_FIFO_RAM_ACCESS2	0x68
#define CORETSE_FIFO_RAM_ACCESS3	0x6C
#define CORETSE_FIFO_RAM_ACCESS4	0x70
#define CORETSE_FIFO_RAM_ACCESS5	0x74
#define CORETSE_FIFO_RAM_ACCESS6	0x78
#define CORETSE_FIFO_RAM_ACCESS7	0x7C

#define CORETSE_FPC			0x1C0u
#define CORETSE_MISCC			0x1D4u

/**
 * CFG1 register fields
 */
#define CORETSE_CFG1_RST	BIT(31)	/* PE-MCXMAC full reset	      */
#define CORETSE_CFG1_RX_ENA	BIT(2)	/* MAC receive enable	      */
#define CORETSE_CFG1_TX_ENA	BIT(0)	/* MAC transmit enable	      */

/**
 * CFG2 register fields
 */
#define CORETSE_CFG2_PREAM_LEN_BIT	12
#define CORETSE_CFG2_PREAM_LEN_MSK	0xf
#define CORETSE_CFG2_PREAM_LEN_DEFAULT	0x7
#define CORETSE_CFG2_MODE_BIT		8      /* MAC interface mode */
#define CORETSE_CFG2_MODE_MSK		0x3
#define CORETSE_CFG2_MODE_BYTE		0x2	/* Byte mode */
#define CORETSE_CFG2_MODE_MII		0x1	/* Nibble mode */
#define CORETSE_CFG2_HUGE_FRAME_EN	BIT(5)
#define CORETSE_CFG2_LEN_CHECK		BIT(4)
#define CORETSE_CFG2_PAD_CRC		BIT(2) /* PAD&CRC appending enable   */
#define CORETSE_CFG2_CRC_EN		BIT(1)
#define CORETSE_CFG2_FULL_DUP		BIT(0) /* PE-MCXMAC Full duplex      */

/**
 * MII_COMMAND register fields
 */
#define CORETSE_MII_CMD_READ	0x1 /* BIT(0)	Do single Read cycle */

/**
 * MII_ADDRESS register fields
 */
#define CORETSE_MII_ADR_PHY_BIT	8		/* 5-bit PHY address  */
#define CORETSE_MII_ADR_REG_BIT	0		/* 5-bit register address */

/**
 * MII_INDICATORS register fields
 */
#define CORETSE_MII_IND_NVAL  0x4 /* BIT(2)	Read Data not yet validated */
#define CORETSE_MII_IND_BUSY  0x1 /* BIT(0)	MII is performing cycle    */

#define CORETSE_MGMT_CLOCK_SEL 0x7 /* 3’b111: pclk(clock) divided by 28 */
/**
 * Interface Control register fields
 */
#define CORETSE_INTF_RESET		BIT(31) /* Reset interface module */
#define CORETSE_INTF_SPEED_100	BIT(16)  /* MII PHY speed 100Mbit */

/**
 * FIFO_CFG0 register fields
 */
#define CORETSE_FIFO_CFG0_FTFENRPLY	BIT(20) /* Fabric tx iface ena ack  */
#define CORETSE_FIFO_CFG0_STFENRPLY	BIT(19) /* PE-MCXMAC tx iface */
#define CORETSE_FIFO_CFG0_FRFENRPLY	BIT(18) /* Fabric rx iface */
#define CORETSE_FIFO_CFG0_SRFENRPLY	BIT(17) /* PE-MCXMAC rx iface */
#define CORETSE_FIFO_CFG0_WTMENRPLY	BIT(16) /* PE-MCXMAC watermark modu */

/**
 * Note, PE-MCXMAC rx becomes 'enabled' only after smth received from line,
 * so don't care about this at initialization: miss CORETSE_FIFO_CFG0_SRFENRPLY
 * in the following mask
 */
#define CORETSE_FIFO_CFG0_ALL_RPLY	(CORETSE_FIFO_CFG0_FTFENRPLY      | \
		CORETSE_FIFO_CFG0_STFENRPLY | CORETSE_FIFO_CFG0_FRFENRPLY | \
		CORETSE_FIFO_CFG0_WTMENRPLY)

#define CORETSE_FIFO_CFG0_FTFENREQ	BIT(12) /* Fabric tx iface ena req  */
#define CORETSE_FIFO_CFG0_STFENREQ	BIT(11) /* PE-MCXMAC tx iface */
#define CORETSE_FIFO_CFG0_FRFENREQ	BIT(10) /* Fabric rx iface */
#define CORETSE_FIFO_CFG0_SRFENREQ	BIT(9)  /* PE-MCXMAC rx iface */
#define CORETSE_FIFO_CFG0_WTMENREQ	BIT(8)  /* PE-MCXMAC watermark modu */
#define CORETSE_FIFO_CFG0_ALL_REQ	(CORETSE_FIFO_CFG0_FTFENREQ     | \
		CORETSE_FIFO_CFG0_STFENREQ | CORETSE_FIFO_CFG0_FRFENREQ | \
		CORETSE_FIFO_CFG0_SRFENREQ | CORETSE_FIFO_CFG0_WTMENREQ)

#define CORETSE_FIFO_CFG0_HSTRSTFT	BIT(4)  /* Fabric tx iface reset    */
#define CORETSE_FIFO_CFG0_HSTRSTST	BIT(3)  /* PE-MCXMAC tx iface */
#define CORETSE_FIFO_CFG0_HSTRSTFR	BIT(2)  /* Fabric rx iface */
#define CORETSE_FIFO_CFG0_HSTRSTSR	BIT(1)  /* PE-MCXMAC rx iface */
#define CORETSE_FIFO_CFG0_HSTRSTWT	BIT(0)  /* PE-MCXMAC watermark modu */
#define CORETSE_FIFO_CFG0_ALL_RST	(CORETSE_FIFO_CFG0_HSTRSTFT     | \
		CORETSE_FIFO_CFG0_HSTRSTST | CORETSE_FIFO_CFG0_HSTRSTFR | \
		CORETSE_FIFO_CFG0_HSTRSTSR | CORETSE_FIFO_CFG0_HSTRSTWT)

/**
 * FIFO_CFG5 register fields
 */
#define CORETSE_FIFO_CFG5_CFGHDPLX	BIT(22) /* Half-duplex flow ctrl */

#define IFG_VALUE		0x40605060
#define HALF_DUPLEX_VALUE	0x00a0f037
#define FIFO_CONFIG1_VALUE	0x0fff0000
#define FIFO_CONFIG2_VALUE	0x04000180
#define FIFO_CONFIG3_VALUE	0x0680FFFF
#define FIFO_CONFIG4_VALUE	0x00002018
#define FIFO_CONFIG5_MASK	0x000FFFFF

/**
 * Register base offsets
 */
#define CORETSE_OFS		0x000 /* MAC register block base */

/* S2MM PCMDA */
#define S2MM_PCDMA_VERSION	0x00
#define S2MM_PCDMA_CTRL		0x10
#define S2MM_PCDMA_STATUS	0x14
  #define PCDMA_STATUS_DONE	0x01
  #define PCDMA_STATUS_ERR	0xFFE
  #define PCDMA_STATUS_AXI_ERR_MASK	0xC
#define S2MM_PCDMA_LENGTH	0x18
#define S2MM_PCDMA_ADDR0	0x1C
#define S2MM_PCDMA_ADDR1	0x20
#define S2MM_PCDMA_INT_EN	0x24 /* Interrupt Enable */
  #define PCDMA_INT_DONE_EN	0x01
  #define PCDMA_INT_DONE_DIS	0xFFE
  #define PCDMA_INT_DIS		0x0
  #define PCDMA_INT_MASK	0xFFF

#define S2MM_PCDMA_INT_SRC		0x28 /* Interrupt Source */
  #define PCDMA_INT_SRC_CLEAR		0xFFF
  #define PCDMA_INT_SRC_DONE_CLEAR	0x01
  #define PCDMA_INT_SRC_ERR_CLEAR	0xFFE

/* MM2S PCMDA */
#define MM2S_PCDMA_VERSION	0x0400
#define MM2S_PCDMA_CTRL		0x0410
#define MM2S_PCDMA_STATUS	0x0414
#define MM2S_PCDMA_LENGTH	0x0418
#define MM2S_PCDMA_ADDR0	0x041C
#define MM2S_PCDMA_ADDR1	0x0420
#define MM2S_PCDMA_INT_EN	0x0424 /* Interrupt Enable */

#define MM2S_PCDMA_INT_SRC	0x0428 /* Interrupt Source */

#define PCDMA_INT_THR_CNT	0x2C /* Interrupt threshold Count */
#define PCDMA_PKT_TRNS_CNT	0x30 /* Packet Transfer Count */

#define PCDMA_AXI4_BUS_ERR_CNT	0x100 /* AXI4 bus error counter */
#define PCDMA_PKT_DROP_ERR_CNT	0x104 /* Packet drop error counter */
#define PCDMA_PKT_DROP_OVF_CNT	0x108 /* Packet drop overflow counter */
#define PCDMA_CMD_FIFO_ERR_CNT	0x10C /* Command FIFO Single bit error count */
#define PCDMA_CMD_FIFO_DOUBLE_ERR_CNT 0x110 /* Command FIFO double bit error count */
#define PCDMA_STATUS_FIFO_ERR_CNT  0x114 /* Status FIFO Single bit error correct count */
#define PCDMA_STATUS_FIFO_DOUBLE_ERR_CNT 0x118 /* Status FIFO Double bit error correct count */

/* DMA descriptor bitfields */
#define PCDMA_START			1
#define PCDMA_BURST_TYPE_OFFSET		1
#define PCDMA_BURST_TYPE_SIZE		2
#define PCDMA_BURST_TYPE_FIXED		0
#define PCDMA_BURST_TYPE_INC		1
#define PCDMA_CMD_ID_OFFSET		16
#define PCDMA_CMD_ID_SIZE		10

#define DELAY_OF_ONE_MILLISEC		10000

#define readx_poll_timeout_coretse(op, addr, val, cond, sleep_us, timeout_us)	\
	read_poll_timeout(op, val, cond, sleep_us, timeout_us, true, addr)

/**
 * struct pcdma_desc - Protocol Converter DMA descriptor
 * @addr: DMA address of data buffer
 * @length: length of data buffer
 * @ctrl: Control bits
 */
struct pcdma_desc {
	u32	addr0;
	u32	addr1;
	u32	length;
	u32	ctrl;
};

#define MAPPING_MASK		0xFFFFFFFF

/**
 * struct coretse_tx_skb - data about an skb which is being transmitted
 * @skb: skb currently being transmitted, only set for the last buffer
 *       of the frame
 * @mapping: DMA address of the skb's fragment buffer
 * @size: size of the DMA mapped buffer
 * @mapped_as_page: true when buffer was mapped with skb_frag_dma_map(),
 *                  false when buffer was mapped with dma_map_single()
 */
struct coretse_tx_skb {
	struct sk_buff		*skb;
	dma_addr_t		mapping;
	size_t			size;
	bool			mapped_as_page;
};

/**
 * Hardware-collected statistics. Used when updating the network
 * device stats by a periodic timer.
 */
struct coretse_stats {
	u32	rx_pause_frames;
	u32	tx_ok;
	u32	tx_single_cols;
	u32	tx_multiple_cols;
	u32	rx_ok;
	u32	rx_fcs_errors;
	u32	rx_align_errors;
	u32	tx_deferred;
	u32	tx_late_cols;
	u32	tx_excessive_cols;
	u32	tx_underruns;
	u32	tx_carrier_errors;
	u32	rx_resource_errors;
	u32	rx_overruns;
	u32	rx_symbol_errors;
	u32	rx_oversize_pkts;
	u32	rx_jabbers;
	u32	rx_undersize_pkts;
	u32	sqe_test_errors;
	u32	rx_length_mismatch;
	u32	tx_pause_frames;
};

/**
 * struct queue_stats - Statistics counters collected by the MAC
 * @rx_packets:		    total packets received
 * @rx_bytes:		    total bytes received
 * @rx_dropped:		    no space in Linux buffers
 * @tx_packets:		    total packets transmitted
 * @tx_bytes:		    total bytes transmitted
 * @tx_dropped:		    no space available in Linux
 */
struct queue_stats {
	union {
		unsigned long first;
		unsigned long rx_packets;
	};
	unsigned long rx_bytes;
	unsigned long rx_dropped;
	unsigned long tx_packets;
	unsigned long tx_bytes;
	unsigned long tx_dropped;
};

struct coretse;
struct coretse_queue;

/**
 * struct coretse_queue - Queue to hold dma buffers
 * @bp:		    private per device data
 * @tx_skb:	    data about an skb which is being transmitted
 * @rx_ring_dma:    Physical address(start address) of the RX buffer descr ring
 * @rx_buffers_dma: Physical address(start address) of the RX buffer descr ring
 * @rx_ring:	    Virtual address of the RX buffer descriptor ring
 * @rx_buffers:	    Virtual address of the RX buffer descriptor ring
 * @napi_rx:	    NAPI RX control structure
 * @stats:	    Statistics counters collected by the MAC
 */
struct coretse_queue {
	struct coretse		*bp;
	struct coretse_tx_skb	*tx_skb;

	dma_addr_t		rx_ring_dma;
	dma_addr_t		rx_buffers_dma;
	struct pcdma_desc	*rx_ring;
	void			*rx_buffers;
	struct napi_struct	napi_rx;
	struct queue_stats	stats;
};

/**
 * struct coretse - private per device data
 * @regs:	Base address for the device address space
 * @mm2s_regs:	pointer to mm2s registers base address
 * @s2mm_regs:	pointer to s2mm registers base address
 * @queues:	data structure to hold dma buffers
 * @lock:	Spin lock
 * @tx_lock:	Spin lock for tx path
 * @rx_lock:	Spin lock for tx path
 * @tx_irq:	mm2s TX IRQ number
 * @rx_irq:	s2mm RX IRQ number
 * @cmd_id:	command_id for PCDMA buffer descriptor
 * @coretse_tx_in_progress flag for tx completion
 * @clk:	CoreTSE bus clock
 * @dmaclk:	PCDMA clock
 * @pdev:       platform device structure
 * @dev:	Pointer for net_device to which it will be attached.
 * @coretse:	Hardware-collected statistics for coretse.
 * @mii_bus:	Pointer to MII bus structure
 * @phylink:	Pointer to phylink instance
 * @phylink_config: phylink configuration settings
 * @pcs_phy:	Reference to PCS/PMA PHY if used
 * @pcs:	phylink pcs structure for PCS PHY
 * @:phy_interface	Phy type to identify between GMII/SGMII/1000 Base-X
 * @struct coretse_tx_skb - data about an skb which is being transmitted
 * @:max_tx_length	mtu length
 */
struct coretse {
	void __iomem		*regs;
	void __iomem		*mm2s_regs;
	void __iomem		*s2mm_regs;

	struct coretse_queue	queues[1];

	/* Lock to protect TX and RX */
	spinlock_t		lock;
	spinlock_t		tx_lock;   /* Exclusive access   */
	spinlock_t		rx_lock;   /* Exclusive access   */
	int tx_irq;
	int rx_irq;
	u32 cmd_id;
	u16 coretse_tx_in_progress;

	struct clk		*clk;
	struct clk		*dmaclk;
	struct platform_device	*pdev;
	struct net_device	*dev;
	struct coretse_stats	coretse;

	struct mii_bus		*mii_bus;
	struct phylink		*phylink;
	struct phylink_config	phylink_config;
	struct mdio_device	*pcs_phy;
	struct phylink_pcs	pcs;

	phy_interface_t		phy_interface;

	struct coretse_tx_skb	rm9200_txq[2];
	unsigned int		max_tx_length;
};
#endif /* _CORETSE_H */
