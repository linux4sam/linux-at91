// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Microchip Polarfire SoC MSS CAN controller driver
 *
 * Copyright (C) 2023 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Naga Sureshkumar Relli <nagasuresh.relli@microchip.com>
 *
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/can/dev.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define DRIVER_NAME	"mpfs_can"

/* CAN registers set */
enum mpfs_can_reg {
	MPFS_CAN_ISR_OFFSET		= 0x00,
	MPFS_CAN_IER_OFFSET		= 0x04,
	MPFS_CAN_RXBUF_STAT_OFFSET	= 0x08,
	MPFS_CAN_TXBUF_STAT_OFFSET	= 0x0C,
	MPFS_CAN_ESR_OFFSET		= 0x10,
	MPFS_CAN_COMMAND_OFFSET		= 0x14,
	MPFS_CAN_CONFIG_OFFSET		= 0x18,
	MPFS_CAN_TX_MSG_CTRL_CMD_BASE	= 0x20,
	MPFS_CAN_TX_MSG_ID_BASE		= 0x24,
	MPFS_CAN_TX_MSG_DATAL_BASE	= 0x28,
	MPFS_CAN_TX_MSG_DATAH_BASE	= 0x2C,
	MPFS_CAN_RX_MSG_CTRL_CMD_BASE	= 0x220,
	MPFS_CAN_RX_MSG_ID_BASE		= 0x224,
	MPFS_CAN_RX_MSG_DATAL_BASE	= 0x228,
	MPFS_CAN_RX_MSG_DATAH_BASE	= 0x22C,
	MPFS_CAN_RX_MSG_AMR_BASE	= 0x230,
	MPFS_CAN_RX_MSG_ACR_BASE	= 0x234,
	MPFS_CAN_RX_MSG_AMR_DATA_BASE	= 0x238,
	MPFS_CAN_RX_MSG_ACR_DATA_BASE	= 0x23C,
};

/* CAN ISR Register masks */
#define MPFS_CAN_ISR_ARB_LOSS_MASK		BIT(2)
#define MPFS_CAN_ISR_OVR_LOAD_MASK		BIT(3)
#define MPFS_CAN_ISR_BIT_ERR_MASK		BIT(4)
#define MPFS_CAN_ISR_STUFF_ERR_MASK		BIT(5)
#define MPFS_CAN_ISR_ACK_ERR_MASK		BIT(6)
#define MPFS_CAN_ISR_FORM_ERR_MASK		BIT(7)
#define MPFS_CAN_ISR_CRC_ERR_MASK		BIT(8)
#define MPFS_CAN_ISR_BUS_OFF_MASK		BIT(9)
#define MPFS_CAN_ISR_RXMSG_LOS_MASK		BIT(10)
#define MPFS_CAN_ISR_TXMSG_SNT_MASK		BIT(11)
#define MPFS_CAN_ISR_RXMSG_SNT_MASK		BIT(12)
#define MPFS_CAN_ISR_RTR_SNT_MASK		BIT(13)
#define MPFS_CAN_ISR_STUKAT_0_MASK		BIT(14)
#define MPFS_CAN_ISR_SST_FAIL_MASK		BIT(15)

/* CAN IER Register masks */
#define MPFS_CAN_IER_GLOBAL_MASK		BIT(0)
#define MPFS_CAN_IER_ARB_LOSS_MASK		BIT(2)
#define MPFS_CAN_IER_OVR_LOAD_MASK		BIT(3)
#define MPFS_CAN_IER_BIT_ERR_MASK		BIT(4)
#define MPFS_CAN_IER_STUFF_ERR_MASK		BIT(5)
#define MPFS_CAN_IER_ACK_ERR_MASK		BIT(6)
#define MPFS_CAN_IER_FORM_ERR_MASK		BIT(7)
#define MPFS_CAN_IER_CRC_ERR_MASK		BIT(8)
#define MPFS_CAN_IER_BUS_OFF_MASK		BIT(9)
#define MPFS_CAN_IER_RXMSG_LOS_MASK		BIT(10)
#define MPFS_CAN_IER_TXMSG_SNT_MASK		BIT(11)
#define MPFS_CAN_IER_RXMSG_SNT_MASK		BIT(12)
#define MPFS_CAN_IER_RTR_SNT_MASK		BIT(13)
#define MPFS_CAN_IER_STUKAT_0_MASK		BIT(14)
#define MPFS_CAN_IER_SST_FAIL_MASK		BIT(15)

/* CAN ERR Register masks */
#define MPFS_CAN_ERR_TXCNT_MASK			GENMASK(7, 0)
#define MPFS_CAN_ERR_RXCNT_MASK			GENMASK(15, 8)
#define MPFS_CAN_ERR_STATE_MASK			GENMASK(17, 16)
#define MPFS_CAN_ERR_TX96_MASK			BIT(18)
#define MPFS_CAN_ERR_RX96_MASK			BIT(19)
#define MPFS_CAN_ERR_RXCNT_SHIFT		8

/* CAN CMD Register masks */
#define MPFS_CAN_CMD_NORMAL_MASK		BIT(0)
#define MPFS_CAN_CMD_LISTEN_MASK		GENMASK(1, 0)
#define MPFS_CAN_CMD_LOOP_MASK			GENMASK(2, 0)
#define MPFS_CAN_CMD_SRAM_TEST_MASK		BIT(3)
#define MPFS_CAN_CMD_SW_REST_MASK		BIT(4)
#define MPFS_CAN_CMD_REV_CTRL_MASK		GENMASK(31, 16)

/* CAN CONFIG Register masks */
#define MPFS_CAN_CONFIG_EDGE_MODE_MASK		BIT(0)
#define MPFS_CAN_CONFIG_SAMPLE_MODE_MASK	BIT(1)
#define MPFS_CAN_CONFIG_CFG_SJW_MASK		GENMASK(3, 2)
#define MPFS_CAN_CONFIG_ATO_RSRT_MASK		BIT(4)
#define MPFS_CAN_CONFIG_TSEG2_MASK		GENMASK(7, 5)
#define MPFS_CAN_CONFIG_TSEG1_MASK		GENMASK(11, 8)
#define MPFS_CAN_CONFIG_ARBITR_MASK		BIT(12)
#define MPFS_CAN_CONFIG_SWP_ENDIN_MASK		BIT(13)
#define MPFS_CAN_CONFIG_ECR_MASK		BIT(14)
#define MPFS_CAN_CONFIG_BITR_MASK		GENMASK(30, 16)

/* CAN ECR Register masks */
#define MPFS_CAN_ECR_STAT_MASK			BIT(0)
#define MPFS_CAN_ECR_ERR_TYPE_MASK		GENMASK(3, 1)
#define MPFS_CAN_ECR_TXMODE_MASK		BIT(4)
#define MPFS_CAN_ECR_RXMODE_MASK		BIT(5)
#define MPFS_CAN_ECR_BITNR_MASK			GENMASK(11, 6)
#define MPFS_CAN_ECR_FILED_MASK			GENMASK(16, 12)

/* CAN TXMSG CTRL Register masks */
#define MPFS_CAN_TXMSG_CTRL_CMD_TXREQ_MASK	BIT(0)
#define MPFS_CAN_TXMSG_CTRL_CMD_TXABORT_MASK	BIT(1)
#define MPFS_CAN_TXMSG_CTRL_CMD_TXINTEN_MASK	BIT(2)
#define MPFS_CAN_TXMSG_CTRL_CMD_WPNA_MASK	BIT(3)
#define MPFS_CAN_TXMSG_CTRL_CMD_DLC_MASK	GENMASK(19, 16)
#define MPFS_CAN_TXMSG_CTRL_CMD_IDE_MASK	BIT(20)
#define MPFS_CAN_TXMSG_CTRL_CMD_RTR_MASK	BIT(21)
#define MPFS_CAN_TXMSG_CTRL_CMD_WPNB_MASK	BIT(23)

/* CAN TXMSG Id/Data Register masks */
#define MPFS_CAN_TXMSG_ID_MASK			GENMASK(31, 3)
#define MPFS_CAN_TXMSG_DATAH_MASK		GENMASK(31, 0)
#define MPFS_CAN_TXMSG_DATAL_MASK		GENMASK(31, 0)

/* CAN RXMSG CTRL Register masks */
#define MPFS_CAN_RXMSG_CTRL_CMD_AVAIL_MASK	BIT(0)
#define MPFS_CAN_RXMSG_CTRL_CMD_RTRP_MASK	BIT(1)
#define MPFS_CAN_RXMSG_CTRL_CMD_RTRABRT_MASK	BIT(2)
#define MPFS_CAN_RXMSG_CTRL_CMD_BUFEN_MASK	BIT(3)
#define MPFS_CAN_RXMSG_CTRL_CMD_RTRRPLY_MASK	BIT(4)
#define MPFS_CAN_RXMSG_CTRL_CMD_RXINTEN_MASK	BIT(5)
#define MPFS_CAN_RXMSG_CTRL_CMD_LF_MASK		BIT(6)
#define MPFS_CAN_RXMSG_CTRL_CMD_WPNL_MASK	BIT(7)
#define MPFS_CAN_RXMSG_CTRL_CMD_DLC_MASK	GENMASK(19, 16)
#define MPFS_CAN_RXMSG_CTRL_CMD_IDE_MASK	BIT(20)
#define MPFS_CAN_RXMSG_CTRL_CMD_RTR_MASK	BIT(21)
#define MPFS_CAN_RXMSG_CTRL_CMD_WPNH_MASK	BIT(23)

/* CAN RXMSG ID/Data Register masks */
#define MPFS_CAN_RXMSG_ID_MASK			GENMASK(31, 3)
#define MPFS_CAN_RXMSG_DATAH_MASK		GENMASK(31, 0)
#define MPFS_CAN_RXMSG_DATAL_MASK		GENMASK(31, 0)
#define MPFS_CAN_RXMSG_AMR_MASK			GENMASK(31, 0)
#define MPFS_CAN_RXMSG_ACR_MASK			GENMASK(31, 0)
#define MPFS_CAN_RXMSG_AMR_DATA			GENMASK(31, 0)
#define MPFS_CAN_RXMSG_ACR_DATA			GENMASK(31, 0)

/* CAN Bit Timing masks */
#define MPFS_CAN_BTR_SJW_MASK		GENMASK(3, 2)
#define MPFS_CAN_BTR_MASK		GENMASK(30, 16)
#define MPFS_CAN_DLC_MASK		GENMASK(19, 16)
#define MPFS_CAN_TS1_MASK		GENMASK(11, 8)
#define MPFS_CAN_TS2_MASK		GENMASK(7, 5)
#define MPFS_CAN_STDID_SHIFT		21
#define MPFS_CAN_EXTID_SHIFT		3
#define MPFS_CAN_ESR_REC_SHIFT		8

/* CAN TX/RX register fifo offsets */
#define TX_REG_LEN			16
#define RX_REG_LEN			32
#define TX_MSG_BASE(base, fifono)	((base) + ((fifono) * TX_REG_LEN))
#define RX_MSG_BASE(base, fifono)	((base) + ((fifono) * RX_REG_LEN))

#define MPFS_CAN_TX_MSG_CTR_OFFSET(fifono)	TX_MSG_BASE(MPFS_CAN_TX_MSG_CTRL_CMD_BASE, fifono)
#define MPFS_CAN_TX_MSG_ID_OFFSET(fifono)	TX_MSG_BASE(MPFS_CAN_TX_MSG_ID_BASE, fifono)
#define MPFS_CAN_TX_DATAL_OFFSET(fifono)	TX_MSG_BASE(MPFS_CAN_TX_MSG_DATAL_BASE, fifono)
#define MPFS_CAN_TX_DATAH_OFFSET(fifono)	TX_MSG_BASE(MPFS_CAN_TX_MSG_DATAH_BASE, fifono)

#define MPFS_CAN_RX_MSG_CTR_OFFSET(fifono)	RX_MSG_BASE(MPFS_CAN_RX_MSG_CTRL_CMD_BASE, fifono)
#define MPFS_CAN_RX_MSG_ID_OFFSET(fifono)	RX_MSG_BASE(MPFS_CAN_RX_MSG_ID_BASE, fifono)
#define MPFS_CAN_RX_DATAL_OFFSET(fifono)	RX_MSG_BASE(MPFS_CAN_RX_MSG_DATAL_BASE, fifono)
#define MPFS_CAN_RX_DATAH_OFFSET(fifono)	RX_MSG_BASE(MPFS_CAN_RX_MSG_DATAH_BASE, fifono)
#define MPFS_CAN_AMR_OFFSET(fifono)		RX_MSG_BASE(MPFS_CAN_RX_MSG_AMR_BASE, fifono)
#define MPFS_CAN_ACR_OFFSET(fifono)		RX_MSG_BASE(MPFS_CAN_RX_MSG_ACR_BASE, fifono)
#define MPFS_CAN_AMR_DATA_OFFSET(fifono)	RX_MSG_BASE(MPFS_CAN_RX_MSG_AMR_DATA_BASE, fifono)
#define MPFS_CAN_ACR_DATA_OFFSET(fifono)	RX_MSG_BASE(MPFS_CAN_RX_MSG_ACR_DATA_BASE, fifono)

#define MPFS_CAN_TX_BUFFERS		32
#define MPFS_CAN_RX_BUFFERS		32
#define MPFS_CAN_AMR_MASK		0xFFFFFFFF
#define MPFS_CAN_AMR_DATA_MASK		0xFFFFFFFF

/* CAN Error and Interrupt masks */
#define MPFS_CAN_ERR_MASK			(MPFS_CAN_ISR_ARB_LOSS_MASK | \
						 MPFS_CAN_ISR_BIT_ERR_MASK | \
						 MPFS_CAN_ISR_STUFF_ERR_MASK | \
						 MPFS_CAN_ISR_ACK_ERR_MASK | \
						 MPFS_CAN_ISR_FORM_ERR_MASK | \
						 MPFS_CAN_ISR_CRC_ERR_MASK | \
						 MPFS_CAN_ISR_BUS_OFF_MASK)

#define MPFS_CAN_IER_MASK			(MPFS_CAN_IER_ARB_LOSS_MASK | \
						 MPFS_CAN_IER_BIT_ERR_MASK | \
						 MPFS_CAN_IER_STUFF_ERR_MASK | \
						 MPFS_CAN_IER_ACK_ERR_MASK | \
						 MPFS_CAN_IER_FORM_ERR_MASK | \
						 MPFS_CAN_IER_CRC_ERR_MASK | \
						 MPFS_CAN_IER_BUS_OFF_MASK | \
						 MPFS_CAN_IER_RXMSG_LOS_MASK | \
						 MPFS_CAN_IER_TXMSG_SNT_MASK | \
						 MPFS_CAN_IER_RXMSG_SNT_MASK | \
						 MPFS_CAN_IER_RTR_SNT_MASK | \
						 MPFS_CAN_IER_GLOBAL_MASK)

/**
 * struct mpfs_can_priv - CAN controller private data
 * @can:			CAN private data structure.
 * @tx_lock:			Lock for synchronizing TX interrupt handling
 * @tx_head:			Tx CAN packets ready to send on the queue
 * @tx_tail:			Tx CAN packets successfully sended on the queue
 * @tx_max:			Maximum number packets the driver can send
 * @napi:			NAPI structure
 * @dev:			Network device data structure
 * @reg:			Ioremapped address to registers
 * @irq_flags:			For request_irq()
 * @pclk:			Pointer to struct clk
 * @can_clk:			Pointer to struct clk
 */
struct mpfs_can_priv {
	struct can_priv can;
	spinlock_t tx_lock;
	unsigned int tx_head;
	unsigned int tx_tail;
	unsigned int tx_max;
	struct napi_struct napi;
	struct device *dev;
	void __iomem *reg;
	unsigned long irq_flags;
	struct clk_bulk_data    *clks;
	int     num_clocks;
	const struct can_bittiming_const *bittiming_const;
};

static const struct can_bittiming_const mpfs_can_bittiming_const = {
	.name = DRIVER_NAME,
	.tseg1_min = 2,
	.tseg1_max = 16,
	.tseg2_min = 1,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 0,
	.brp_max = 32767,
	.brp_inc = 1,
};

static void mpfs_can_write(const struct mpfs_can_priv *priv, enum mpfs_can_reg reg,
			   u32 val)
{
	writel(val, priv->reg + reg);
}

static u32 mpfs_can_read(const struct mpfs_can_priv *priv, enum mpfs_can_reg reg)
{
	return readl(priv->reg + reg);
}

static int mpfs_can_reset(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	int ret;

	/* There is no separate register in the MSS CAN to do the soft reset
	 * hence do the full device reset and this is needed to bring back the
	 * bus to normal state in error cases.
	 */
	ret = device_reset(priv->dev);
	if (ret)
		return ret;

	priv->tx_head = 0;
	priv->tx_tail = 0;

	return ret;
}

static int mpfs_can_set_bittiming(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	struct can_bittiming *bt = &priv->can.bittiming;
	u32 config, can_started;

	can_started = mpfs_can_read(priv, MPFS_CAN_COMMAND_OFFSET) & MPFS_CAN_CMD_NORMAL_MASK;
	if (can_started) {
		netdev_alert(ndev,
			     "BUG! Cannot set bittiming - CAN is already running\n");
		return -EPERM;
	}

	/* The TS1, TS2 and buad rate values written in the Config register are
	 * added to 1 to compute the prescalar.
	 * TS1 configuration for the mpfs can includes both propagation seg and phase seg1
	 */
	config = FIELD_PREP(MPFS_CAN_BTR_MASK, bt->brp - 1) |
		 FIELD_PREP(MPFS_CAN_TS1_MASK, bt->prop_seg + bt->phase_seg1 - 1) |
		 FIELD_PREP(MPFS_CAN_TS2_MASK, bt->phase_seg2 - 1) |
		 FIELD_PREP(MPFS_CAN_BTR_SJW_MASK, bt->sjw - 1);

	mpfs_can_write(priv, MPFS_CAN_CONFIG_OFFSET, config);

	return 0;
}

static int mpfs_can_start(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	u32 reg_msr;
	int ret, buf;

	ret = mpfs_can_reset(ndev);
	if (ret)
		return ret;

	ret = mpfs_can_set_bittiming(ndev);
	if (ret < 0)
		return ret;

	/* Clear pending interrupts */
	mpfs_can_write(priv, MPFS_CAN_ISR_OFFSET, 0x0);

	/* Enable interrupts */
	mpfs_can_write(priv, MPFS_CAN_IER_OFFSET, MPFS_CAN_IER_MASK);

	/* Check whether it is loopback mode or normal mode  */
	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		reg_msr = MPFS_CAN_CMD_LOOP_MASK;
	else
		reg_msr = MPFS_CAN_CMD_NORMAL_MASK;

	mpfs_can_write(priv, MPFS_CAN_COMMAND_OFFSET, reg_msr);

	for (buf = 0; buf < MPFS_CAN_RX_BUFFERS; buf++)	{
		mpfs_can_write(priv, MPFS_CAN_AMR_OFFSET(buf), MPFS_CAN_AMR_MASK);
		mpfs_can_write(priv, MPFS_CAN_ACR_OFFSET(buf), 0x0);
		mpfs_can_write(priv, MPFS_CAN_AMR_DATA_OFFSET(buf), MPFS_CAN_AMR_DATA_MASK);
		mpfs_can_write(priv, MPFS_CAN_ACR_DATA_OFFSET(buf), 0x0);

		/* Enable the link flag for the buffers, so that the receive data will store
		 * in sequential order.
		 */
		mpfs_can_write(priv, MPFS_CAN_RX_MSG_CTR_OFFSET(buf),
			       (MPFS_CAN_RXMSG_CTRL_CMD_LF_MASK |
				MPFS_CAN_RXMSG_CTRL_CMD_WPNL_MASK |
				MPFS_CAN_RXMSG_CTRL_CMD_WPNH_MASK |
				MPFS_CAN_RXMSG_CTRL_CMD_BUFEN_MASK |
				MPFS_CAN_RXMSG_CTRL_CMD_RXINTEN_MASK));

		/* Link flag is not needed for the last buffer */
		if (buf == (MPFS_CAN_RX_BUFFERS - 1))
			mpfs_can_write(priv, MPFS_CAN_RX_MSG_CTR_OFFSET(buf),
				       (MPFS_CAN_RXMSG_CTRL_CMD_WPNL_MASK |
					MPFS_CAN_RXMSG_CTRL_CMD_WPNH_MASK |
					MPFS_CAN_RXMSG_CTRL_CMD_BUFEN_MASK |
					MPFS_CAN_RXMSG_CTRL_CMD_RXINTEN_MASK));
	}

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	return 0;
}

static int mpfs_can_do_set_mode(struct net_device *ndev, enum can_mode mode)
{
	int ret = 0;

	switch (mode) {
	case CAN_MODE_START:
		ret = mpfs_can_start(ndev);
		if (ret < 0) {
			netdev_err(ndev, "mpfs_can_start failed!\n");
			return ret;
		}
		netif_wake_queue(ndev);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static void mpfs_can_tx(struct net_device *ndev, struct sk_buff *skb,
			int buf_off)
{
	struct can_frame *frame = (struct can_frame *)skb->data;
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	u32 id = 0, data[2] = {0, 0}, ctlr = 0;

	if (frame->can_id & CAN_EFF_FLAG) {
		ctlr = MPFS_CAN_TXMSG_CTRL_CMD_IDE_MASK;
		id = (frame->can_id << MPFS_CAN_EXTID_SHIFT);
	} else {
		id = (frame->can_id << MPFS_CAN_STDID_SHIFT);
	}

	if (frame->can_id & CAN_RTR_FLAG)
		ctlr |= MPFS_CAN_TXMSG_CTRL_CMD_RTR_MASK;

	ctlr |= FIELD_PREP(MPFS_CAN_DLC_MASK, frame->len) |
		MPFS_CAN_TXMSG_CTRL_CMD_WPNA_MASK | MPFS_CAN_TXMSG_CTRL_CMD_WPNB_MASK |
		 MPFS_CAN_TXMSG_CTRL_CMD_TXREQ_MASK | MPFS_CAN_TXMSG_CTRL_CMD_TXINTEN_MASK;
	can_put_echo_skb(skb, ndev, priv->tx_head % priv->tx_max, 0);

	priv->tx_head++;
	mpfs_can_write(priv, MPFS_CAN_TX_MSG_ID_OFFSET(buf_off), id);

	if (frame->len > 0)
		data[0] = be32_to_cpup((__be32 *)(frame->data + 0));
	if (frame->len > 4)
		data[1] = be32_to_cpup((__be32 *)(frame->data + 4));

	if (!(frame->can_id & CAN_RTR_FLAG)) {
		mpfs_can_write(priv, MPFS_CAN_TX_DATAL_OFFSET(buf_off), data[0]);
		mpfs_can_write(priv, MPFS_CAN_TX_DATAH_OFFSET(buf_off), data[1]);
	}

	mpfs_can_write(priv, MPFS_CAN_TX_MSG_CTR_OFFSET(buf_off), ctlr);
}

static int mpfs_can_get_txmsg_index(struct mpfs_can_priv *priv)
{
	int available_txreq;

	for (int buf_index = 0; buf_index < MPFS_CAN_TX_BUFFERS; buf_index++) {
		available_txreq = mpfs_can_read(priv, MPFS_CAN_TX_MSG_CTR_OFFSET(buf_index));

		if (available_txreq & MPFS_CAN_TXMSG_CTRL_CMD_TXREQ_MASK)
			continue;

		return buf_index;
	}

	return -ENOBUFS;
}

static int mpfs_can_get_rxmsg_index(struct mpfs_can_priv *priv)
{
	int available_rxreq;

	for (int buf_index = 0; buf_index < MPFS_CAN_RX_BUFFERS; buf_index++) {
		available_rxreq = mpfs_can_read(priv, MPFS_CAN_RX_MSG_CTR_OFFSET(buf_index));

		if (!(available_rxreq & MPFS_CAN_RXMSG_CTRL_CMD_AVAIL_MASK))
			continue;

		return buf_index;
	}

	return -ENOBUFS;
}

static netdev_tx_t mpfs_can_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	int buf_off;
	unsigned long flags;

	if (can_dropped_invalid_skb(ndev, skb))
		return NETDEV_TX_OK;

	buf_off = mpfs_can_get_txmsg_index(priv);
	if (buf_off < 0) {
		netdev_err(ndev, "BUG!, TX full when queue awake!\n");
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);
	mpfs_can_tx(ndev, skb, buf_off);

	if ((priv->tx_head - priv->tx_tail) == priv->tx_max)
		netif_stop_queue(ndev);

	netif_stop_queue(ndev);

	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return NETDEV_TX_OK;
}

static int mpfs_can_rx(struct net_device *ndev, int buf_off)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct can_frame *frame;
	struct sk_buff *skb;
	u32 id, data[2] = {0, 0}, ctrl;

	skb = alloc_can_skb(ndev, &frame);
	if (unlikely(!skb)) {
		stats->rx_dropped++;
		return 0;
	}

	id = mpfs_can_read(priv, MPFS_CAN_RX_MSG_ID_OFFSET(buf_off));
	ctrl = mpfs_can_read(priv, MPFS_CAN_RX_MSG_CTR_OFFSET(buf_off));

	if (ctrl & MPFS_CAN_RXMSG_CTRL_CMD_IDE_MASK) {
		frame->can_id = CAN_EFF_FLAG;
		frame->can_id |= (id >> MPFS_CAN_EXTID_SHIFT);
	} else {
		frame->can_id = (id >> MPFS_CAN_STDID_SHIFT);
	}

	if (ctrl & MPFS_CAN_RXMSG_CTRL_CMD_RTR_MASK)
		frame->can_id |= CAN_RTR_FLAG;

	frame->len = (ctrl & MPFS_CAN_DLC_MASK) >> 16;

	data[0] = mpfs_can_read(priv, MPFS_CAN_RX_DATAL_OFFSET(buf_off));
	data[1] = mpfs_can_read(priv, MPFS_CAN_RX_DATAH_OFFSET(buf_off));

	if (!(frame->can_id & CAN_RTR_FLAG)) {
		if (frame->len > 0)
			*(__be32 *)(frame->data) = cpu_to_be32(data[0]);
		if (frame->len > 4)
			*(__be32 *)(frame->data + 4) = cpu_to_be32(data[1]);
	}

	/* Notify the controller that the rx message is read by setting message
	 * available bit.
	 */
	ctrl = mpfs_can_read(priv, MPFS_CAN_RX_MSG_CTR_OFFSET(buf_off));
	ctrl |= MPFS_CAN_RXMSG_CTRL_CMD_AVAIL_MASK;
	mpfs_can_write(priv, MPFS_CAN_RX_MSG_CTR_OFFSET(buf_off), ctrl);

	stats->rx_bytes += frame->len;
	stats->rx_packets++;
	netif_receive_skb(skb);

	return 1;
}

static enum can_state mpfs_can_current_error_state(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	u32 status = mpfs_can_read(priv, MPFS_CAN_ESR_OFFSET);

	if (status & MPFS_CAN_ERR_STATE_MASK)
		return CAN_STATE_ERROR_PASSIVE;
	else if (status & (MPFS_CAN_ERR_TX96_MASK | MPFS_CAN_ERR_RX96_MASK))
		return CAN_STATE_ERROR_WARNING;
	else
		return CAN_STATE_ERROR_ACTIVE;
}

static void mpfs_can_set_error_state(struct net_device *ndev, enum can_state new_state,
				     struct can_frame *frame)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	u32 ecr = mpfs_can_read(priv, MPFS_CAN_ESR_OFFSET);
	u32 txerr = ecr & MPFS_CAN_ERR_TXCNT_MASK;
	u32 rxerr = (ecr & MPFS_CAN_ERR_RXCNT_MASK) >> MPFS_CAN_ERR_RXCNT_SHIFT;
	enum can_state tx_state = txerr >= rxerr ? new_state : 0;
	enum can_state rx_state = txerr <= rxerr ? new_state : 0;

	if (new_state > CAN_STATE_ERROR_PASSIVE) {
		frame->can_id |= CAN_ERR_CRTL;
		frame->data[1] = (txerr > rxerr) ?
				  CAN_ERR_CRTL_TX_PASSIVE : CAN_ERR_CRTL_RX_PASSIVE;
	} else {
		can_change_state(ndev, frame, tx_state, rx_state);

		if (frame) {
			frame->data[6] = txerr;
			frame->data[7] = rxerr;
		}
	}
}

static void mpfs_can_update_error_state(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	enum can_state old_state = priv->can.state;
	enum can_state new_state;

	if (old_state != CAN_STATE_ERROR_WARNING &&
	    old_state != CAN_STATE_ERROR_PASSIVE)
		return;

	new_state = mpfs_can_current_error_state(ndev);

	if (new_state != old_state) {
		struct sk_buff *skb;
		struct can_frame *cf;

		skb = alloc_can_err_skb(ndev, &cf);

		mpfs_can_set_error_state(ndev, new_state, skb ? cf : NULL);

		if (skb) {
			struct net_device_stats *stats = &ndev->stats;

			stats->rx_packets++;
			stats->rx_bytes += cf->len;
			netif_rx(skb);
		}
	}
}

static void mpfs_can_err_interrupt(struct net_device *ndev, u32 int_status)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct can_frame cf = { };
	u32 err_status;

	err_status = mpfs_can_read(priv, MPFS_CAN_ESR_OFFSET);
	mpfs_can_write(priv, MPFS_CAN_ESR_OFFSET, err_status);

	if (int_status & MPFS_CAN_ISR_BUS_OFF_MASK) {
		priv->can.state = CAN_STATE_BUS_OFF;
		priv->can.can_stats.bus_off++;
		can_bus_off(ndev);
		cf.can_id |= CAN_ERR_BUSOFF;
	} else {
		enum can_state new_state = mpfs_can_current_error_state(ndev);

		if (new_state != priv->can.state)
			mpfs_can_set_error_state(ndev, new_state, &cf);
	}

	if (int_status & MPFS_CAN_ISR_ARB_LOSS_MASK) {
		priv->can.can_stats.arbitration_lost++;
		cf.can_id |= CAN_ERR_LOSTARB;
		cf.data[0] = CAN_ERR_LOSTARB_UNSPEC;
	}

	if (int_status & MPFS_CAN_ERR_MASK) {
		priv->can.can_stats.bus_error++;
		cf.can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

		if (int_status & MPFS_CAN_ISR_ACK_ERR_MASK) {
			stats->tx_errors++;
			cf.can_id |= CAN_ERR_ACK;
			cf.data[3] = CAN_ERR_PROT_LOC_ACK;
		}

		if (int_status & MPFS_CAN_ISR_BIT_ERR_MASK) {
			stats->tx_errors++;
			cf.can_id |= CAN_ERR_PROT;
			cf.data[2] |= CAN_ERR_PROT_BIT;
		}

		if (int_status & MPFS_CAN_ISR_STUFF_ERR_MASK) {
			stats->rx_errors++;
			cf.can_id |= CAN_ERR_PROT;
			cf.data[2] |= CAN_ERR_PROT_STUFF;
		}

		if (err_status & MPFS_CAN_ISR_FORM_ERR_MASK) {
			stats->rx_errors++;
			cf.can_id |= CAN_ERR_PROT;
			cf.data[2] |= CAN_ERR_PROT_FORM;
		}

		if (err_status & MPFS_CAN_ISR_CRC_ERR_MASK) {
			stats->rx_errors++;
			cf.can_id |= CAN_ERR_PROT;
			cf.data[3] |= CAN_ERR_PROT_LOC_CRC_SEQ;
		}

		if (err_status & MPFS_CAN_ISR_OVR_LOAD_MASK) {
			stats->rx_over_errors++;
			stats->rx_errors++;
			cf.can_id |= CAN_ERR_PROT;
			cf.data[3] |= CAN_ERR_PROT_OVERLOAD;
		}
	}

	if (cf.can_id) {
		struct can_frame *frame;
		struct sk_buff *skb = alloc_can_err_skb(ndev, &frame);

		if (skb) {
			frame->can_id |= cf.can_id;
			memcpy(frame->data, cf.data, CAN_ERR_DLC);
			stats->rx_packets++;
			stats->rx_bytes += CAN_ERR_DLC;
			netif_rx(skb);
		}
	}

	netdev_dbg(ndev, "%s: error status register:0x%x\n",
		   __func__, mpfs_can_read(priv, MPFS_CAN_ESR_OFFSET));
}

static int mpfs_can_rx_poll(struct napi_struct *napi, int quota)
{
	struct net_device *ndev = napi->dev;
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	u32 int_enable;
	int processed = 0, buf_off;

	while ((buf_off = mpfs_can_get_rxmsg_index(priv)) >= 0 && (processed < quota))
		processed += mpfs_can_rx(ndev, buf_off);

	if (processed)
		mpfs_can_update_error_state(ndev);

	if (processed < quota) {
		if (napi_complete_done(napi, processed)) {
			int_enable = mpfs_can_read(priv, MPFS_CAN_IER_OFFSET);
			int_enable |= (MPFS_CAN_IER_RXMSG_SNT_MASK);
			mpfs_can_write(priv, MPFS_CAN_IER_OFFSET, int_enable);
		}
	}

	return processed;
}

static void mpfs_can_tx_interrupt(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	unsigned int frames_in_fifo;
	unsigned long flags;
	int bytes = 0;

	spin_lock_irqsave(&priv->tx_lock, flags);
	frames_in_fifo = priv->tx_head - priv->tx_tail;

	while (frames_in_fifo--) {
		bytes = can_get_echo_skb(ndev, priv->tx_tail % priv->tx_max, NULL);
		stats->tx_bytes += bytes;
		priv->tx_tail++;
		stats->tx_packets++;
	}

	netif_wake_queue(ndev);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	mpfs_can_update_error_state(ndev);
}

static irqreturn_t mpfs_can_interrupt(int irq, void *dev_id)
{
	struct net_device *ndev = (struct net_device *)dev_id;
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	u32 isr, ier;

	isr = mpfs_can_read(priv, MPFS_CAN_ISR_OFFSET);
	if (!isr)
		return IRQ_NONE;

	mpfs_can_write(priv, MPFS_CAN_ISR_OFFSET, isr);

	if (isr & MPFS_CAN_ISR_TXMSG_SNT_MASK)
		mpfs_can_tx_interrupt(ndev);

	if (isr & MPFS_CAN_ERR_MASK)
		mpfs_can_err_interrupt(ndev, isr);

	if (isr & MPFS_CAN_ISR_RXMSG_SNT_MASK) {
		ier = mpfs_can_read(priv, MPFS_CAN_IER_OFFSET);
		ier &= ~(MPFS_CAN_IER_RXMSG_SNT_MASK | MPFS_CAN_IER_RTR_SNT_MASK);
		mpfs_can_write(priv, MPFS_CAN_IER_OFFSET, ier);
		napi_schedule(&priv->napi);
	}

	return IRQ_HANDLED;
}

static int mpfs_can_chip_stop(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	int ret;

	ret = mpfs_can_reset(ndev);
	if (ret)
		return ret;

	priv->can.state = CAN_STATE_STOPPED;

	return 0;
}

static int mpfs_can_open(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	int ret;

	ret = request_irq(ndev->irq, mpfs_can_interrupt, priv->irq_flags,
			  ndev->name, ndev);
	if (ret < 0) {
		netdev_err(ndev, "irq allocation for CAN failed\n");
		return ret;
	}

	ret = mpfs_can_reset(ndev);
	if (ret)
		goto err_irq;

	ret = open_candev(ndev);
	if (ret)
		goto err_irq;

	ret = mpfs_can_start(ndev);
	if (ret < 0) {
		netdev_err(ndev, "mpfs_can_start failed!\n");
		goto err_candev;
	}

	napi_enable(&priv->napi);
	netif_start_queue(ndev);

	return 0;

err_candev:
	close_candev(ndev);
err_irq:
	free_irq(ndev->irq, ndev);

	return ret;
}

static int mpfs_can_close(struct net_device *ndev)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	int ret;

	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	ret = mpfs_can_chip_stop(ndev);
	free_irq(ndev->irq, ndev);
	close_candev(ndev);

	return ret;
}

static int mpfs_can_get_berr_counter(const struct net_device *ndev,
				     struct can_berr_counter *bec)
{
	struct mpfs_can_priv *priv = netdev_priv(ndev);
	int err_cnt;

	err_cnt = mpfs_can_read(priv, MPFS_CAN_ESR_OFFSET);
	bec->txerr = err_cnt & MPFS_CAN_ERR_TXCNT_MASK;
	bec->rxerr = (err_cnt & MPFS_CAN_ERR_RXCNT_MASK) >> MPFS_CAN_ERR_RXCNT_SHIFT;

	return 0;
}

static const struct net_device_ops mpfs_can_netdev_ops = {
	.ndo_open	= mpfs_can_open,
	.ndo_stop	= mpfs_can_close,
	.ndo_start_xmit	= mpfs_can_start_xmit,
	.ndo_change_mtu	= can_change_mtu,
};

static int mpfs_can_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct mpfs_can_priv *priv;
	struct resource *res;
	int ret;

	ndev = alloc_candev(sizeof(struct mpfs_can_priv), MPFS_CAN_TX_BUFFERS);
	if (!ndev)
		return -ENOMEM;

	priv = netdev_priv(ndev);
	priv->dev = &pdev->dev;
	priv->can.bittiming_const = &mpfs_can_bittiming_const;
	priv->can.do_set_mode = mpfs_can_do_set_mode;
	priv->can.do_get_berr_counter = mpfs_can_get_berr_counter;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_BERR_REPORTING;
	priv->tx_max = MPFS_CAN_TX_BUFFERS;

	priv->reg = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(priv->reg)) {
		ret = PTR_ERR(priv->reg);
		goto err;
	}

	spin_lock_init(&priv->tx_lock);

	ndev->irq = platform_get_irq(pdev, 0);
	ndev->flags |= IFF_ECHO;
	ndev->netdev_ops = &mpfs_can_netdev_ops;
	platform_set_drvdata(pdev, ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);

	ret = clk_bulk_get_all(&pdev->dev, &priv->clks);
	if (ret < 2)
		goto err;

	priv->num_clocks = ret;
	ret = clk_bulk_prepare_enable(priv->num_clocks, priv->clks);
	if (ret)
		goto err;

	/* Retrieve the CAN clock rate */
	priv->can.clock.freq = clk_get_rate(priv->clks[1].clk);

	netif_napi_add_weight(ndev, &priv->napi, mpfs_can_rx_poll, MPFS_CAN_RX_BUFFERS);

	ret = register_candev(ndev);
	if (ret) {
		ret = dev_err_probe(&pdev->dev, ret, "fail to register can device\n");
		goto err;
	}

	netdev_dbg(ndev, "reg=0x%p irq=%d clock=%d, max tx buffers %d\n",
		   priv->reg, ndev->irq, priv->can.clock.freq,
		   priv->tx_max);

	return 0;

err:
	free_candev(ndev);
	netif_napi_del(&priv->napi);

	return ret;
}

static void mpfs_can_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct mpfs_can_priv *priv = netdev_priv(ndev);

	unregister_candev(ndev);
	netif_napi_del(&priv->napi);
	free_candev(ndev);
}

static const struct of_device_id mpfs_can_of_match[] = {
	{ .compatible = "microchip,mpfs-can", .data = NULL },
	{ /* end of list */ },
};

MODULE_DEVICE_TABLE(of, mpfs_can_of_match);

static struct platform_driver mpfs_can_driver = {
	.probe = mpfs_can_probe,
	.remove	= mpfs_can_remove,
	.driver	= {
		.name = DRIVER_NAME,
		.of_match_table	= mpfs_can_of_match,
	},
};

module_platform_driver(mpfs_can_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Microchip Polarfire SoC MSS CAN controller driver");
MODULE_AUTHOR("Naga Sureshkumar Relli <nagasuresh.relli@microchip.com");
