// SPDX-License-Identifier: (GPL-2.0)
/**
 * Microchip coreTSE(Triple SPEED Ethernet) MAC driver
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Praveen Kumar Vattipalli <praveen.kumar@microchip.com>
 *
 */

#include <linux/clk-provider.h>
#include <linux/crc32.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/circ_buf.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/iopoll.h>
#include <linux/reset.h>
#include "mchp_coretse.h"

/* max number of receive buffers */
#define PCDMA_MAX_RX_DESCR	1
#define PCDMA_MAX_TX_DESCR	1
#define MAX_TX_LENGTH		2048

#define MCHP_FRAME_FILTER_CTRL		0x3F
#define MCHP_MM2S_START			0x1
#define MCHP_MM2S_BURST_TYPE		0x2
#define MCHP_MM2S_START_ADDR		BIT(16)
#define MCHP_S2MM_CMD_ID		0x3FF
#define MCHP_S2MM_CMD_ID_OFFSET		16

#define CORETSE_MII_IND_READ(tse)	(readl_relaxed(tse->regs + CORETSE_MII_IND))

#define CORETSE_MDIO_TIMEOUT	    1000000 /* in usecs */
#define CORETSE_MDIO_READ_SLEEP     40000 /* in usecs */
/* 1518 rounded up */
#define CORETSE_MAX_RBUFF_SZ	0x600

#define POLL_TIMEOUT_U_SEC		1000
#define POLL_SLEEP_U_SEC		10

static struct coretse *pcs_to_coretse(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct coretse, pcs);
}

static int coretse_mdio_wait_for_idle(struct coretse *tse, u32 flags)
{
	u32 val;
	u32 ret;

	ret = readx_poll_timeout_coretse(CORETSE_MII_IND_READ, tse, val,
					 (!(val & flags)), POLL_SLEEP_U_SEC,
					 POLL_TIMEOUT_U_SEC);
	return ret;
}

/* CoreTSE driver supports a fixed configuration for Time-Sensitive Networking
 *  (TSN) solutions. Specifically, it operates at a speed of 1000 Mbps (1 Gbps)
 *  and in full duplex mode. This configuration is essential for ensuring the
 *  high performance and reliability required for TSN applications
 */
static void coretse_pcs_get_state(struct phylink_pcs *pcs,
				  struct phylink_link_state *state)
{
	state->speed = SPEED_1000;
	state->duplex = 1;
	state->an_complete = 1;
}

static void coretse_pcs_an_restart(struct phylink_pcs *pcs)
{
	struct mdio_device *pcs_phy = pcs_to_coretse(pcs)->pcs_phy;

	phylink_mii_c22_pcs_an_restart(pcs_phy);
}

static int coretse_pcs_config(struct phylink_pcs *pcs, unsigned int mode,
			      phy_interface_t interface,
			      const unsigned long *advertising,
			      bool permit_pause_to_mac)
{
	struct mdio_device *pcs_phy = pcs_to_coretse(pcs)->pcs_phy;
	struct net_device *ndev = pcs_to_coretse(pcs)->dev;
	int ret;

	ret = phylink_mii_c22_pcs_config(pcs_phy, interface, advertising,
					 mode);
	if (ret < 0)
		netdev_warn(ndev, "Failed to configure PCS: %d\n", ret);
	return ret;
}

static const struct phylink_pcs_ops coretse_pcs_ops = {
	.pcs_get_state = coretse_pcs_get_state,
	.pcs_config = coretse_pcs_config,
	.pcs_an_restart = coretse_pcs_an_restart,
};

static struct phylink_pcs *coretse_mac_select_pcs(struct phylink_config *config,
						  phy_interface_t interface)
{
	struct net_device *ndev = to_net_dev(config->dev);
	struct coretse *bp = netdev_priv(ndev);

	if (interface == PHY_INTERFACE_MODE_1000BASEX ||
	    interface ==  PHY_INTERFACE_MODE_SGMII)
		return &bp->pcs;

	return NULL;
}

static void coretse_mac_config(struct phylink_config *config,
			       unsigned int mode,
			       const struct phylink_link_state *state)
{
	/* nothing meaningful to do */
}

static void coretse_mac_link_down(struct phylink_config *config,
				  unsigned int mode,
				  phy_interface_t interface)
{
/* nothing meaningful to do */
}

static void coretse_mac_link_up(struct phylink_config *config,
				struct phy_device *phy,
				unsigned int mode, phy_interface_t interface,
				int speed, int duplex,
				bool tx_pause, bool rx_pause)
{
	/* nothing meaningful to do */
}

static const struct phylink_mac_ops coretse_phylink_ops = {
	.validate = phylink_generic_validate,
	.mac_select_pcs = coretse_mac_select_pcs,
	.mac_config = coretse_mac_config,
	.mac_link_down = coretse_mac_link_down,
	.mac_link_up = coretse_mac_link_up,
};

static int mchp_coretse_rx_poll(struct napi_struct *napi, int quota)
{
	struct net_device *dev = napi->dev;
	struct coretse *lp = netdev_priv(dev);
	struct coretse_queue *q = &lp->queues[0];
	struct pcdma_desc *desc;
	unsigned char *p_recv;
	struct sk_buff *skb;
	u32 len;

	len = readl_relaxed(lp->s2mm_regs + S2MM_PCDMA_LENGTH);
	desc = &q->rx_ring[lp->cmd_id];
	p_recv = q->rx_buffers + (lp->cmd_id * CORETSE_MAX_RBUFF_SZ);

	skb = netdev_alloc_skb_ip_align(dev, len + NET_IP_ALIGN);
	if (skb) {
		skb_copy_to_linear_data(skb, p_recv, len);
		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, dev);
		skb_checksum_none_assert(skb);
		napi_gro_receive(napi, skb);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += len;
		skb->ip_summed = CHECKSUM_NONE;
	} else {
		dev->stats.rx_dropped++;
	}
	writel_relaxed(desc->addr0, lp->s2mm_regs + S2MM_PCDMA_ADDR0);
	writel_relaxed(desc->addr1, lp->s2mm_regs + S2MM_PCDMA_ADDR1);
	writel_relaxed(desc->ctrl, lp->s2mm_regs + S2MM_PCDMA_CTRL);

	napi_complete(&lp->queues[0].napi_rx);

	return 0;
}

/* S2MM PCDMA Rx interrupt handler */
static irqreturn_t mchp_pcdma_rx_irq(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device *)dev_id;
	struct coretse *lp = netdev_priv(dev);
	u32 status;
	unsigned long flags;

	status = readl_relaxed(lp->s2mm_regs + S2MM_PCDMA_STATUS);
	if (unlikely(!status))
		return IRQ_NONE;

	spin_lock_irqsave(&lp->rx_lock, flags);

	if (status & PCDMA_STATUS_ERR) {
		netdev_err(dev, "Rx error 0x%x\n", status);
		writel_relaxed(PCDMA_INT_SRC_ERR_CLEAR, lp->s2mm_regs +
			       S2MM_PCDMA_INT_SRC);
	}

	/* Receive complete */
	if (status & PCDMA_STATUS_DONE) {
		lp->cmd_id = (status >> MCHP_S2MM_CMD_ID_OFFSET) &
			      MCHP_S2MM_CMD_ID;
		writel_relaxed(PCDMA_INT_SRC_DONE_CLEAR, lp->s2mm_regs +
			       S2MM_PCDMA_INT_SRC);
		napi_schedule(&lp->queues[0].napi_rx);
	}

	spin_unlock_irqrestore(&lp->rx_lock, flags);
	return IRQ_HANDLED;
}

/* MM2S PCDMA Tx interrupt handler */
static irqreturn_t mchp_pcdma_tx_irq(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct coretse *lp = netdev_priv(dev);
	u32 status;
	unsigned int desc;
	unsigned long flags;

	status = readl_relaxed(lp->s2mm_regs + MM2S_PCDMA_STATUS);

	if (unlikely(!status))
		return IRQ_NONE;

	spin_lock_irqsave(&lp->tx_lock, flags);

	if (status & PCDMA_STATUS_ERR) {
		dev->stats.tx_errors++;
		writel_relaxed(PCDMA_INT_SRC_ERR_CLEAR, lp->s2mm_regs +
			       MM2S_PCDMA_INT_SRC);
		netdev_err(dev, "Tx PCDMA_STATUS_ERR\n");
	}
	/* Transmit complete */
	if (status & PCDMA_STATUS_DONE) {
		writel_relaxed(PCDMA_INT_SRC_DONE_CLEAR, lp->s2mm_regs +
			       MM2S_PCDMA_INT_SRC);
		desc = 0;

		if (lp->rm9200_txq[desc].skb) {
			dev_consume_skb_irq(lp->rm9200_txq[desc].skb);
			lp->rm9200_txq[desc].skb = NULL;
			dma_unmap_single(&lp->pdev->dev,
					 lp->rm9200_txq[desc].mapping,
					 lp->rm9200_txq[desc].size,
					 DMA_TO_DEVICE);
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += lp->rm9200_txq[desc].size;
			lp->coretse_tx_in_progress = 0;
		}
		netif_wake_queue(dev);
	}

	spin_unlock_irqrestore(&lp->tx_lock, flags);
	return IRQ_HANDLED;
}

static bool coretse_phy_handle_exists(struct device_node *dn)
{
	dn = of_parse_phandle(dn, "phy-handle", 0);
	of_node_put(dn);
	return dn;
}

static int coretse_phylink_connect(struct coretse *bp)
{
	struct device_node *dn = bp->pdev->dev.of_node;
	struct net_device *dev = bp->dev;
	struct phy_device *phydev;
	int ret;

	if (dn)
		ret = phylink_of_phy_connect(bp->phylink, dn, 0);

	if (!dn || (ret && !coretse_phy_handle_exists(dn))) {
		phydev = phy_find_first(bp->mii_bus);
		if (!phydev) {
			netdev_err(dev, "no PHY found\n");
			return -ENXIO;
		}

		/* attach the mac to the phy */
		ret = phylink_connect_phy(bp->phylink, phydev);
	}

	if (ret) {
		netdev_err(dev, "Could not attach PHY (%d)\n", ret);
		return ret;
	}

	phylink_start(bp->phylink);

	return 0;
}

/**
 * Read register value via MII
 */
static int mchp_coretse_mdio_read(struct mii_bus *bus, int phy_id, int reg)
{
	struct coretse *tse = bus->priv;
	u32 value;
	int ret = 0;

	/**
	 * Set PHY & REG addresses, and issue read command.
	 * Note, we MUST reset 'READ' bit in cmd, otherwise, on each
	 * next read - the same data will be returned in mii_status
	 */

	value =  (phy_id << CORETSE_MII_ADR_PHY_BIT) |
		 (reg << CORETSE_MII_ADR_REG_BIT);

	ret = coretse_mdio_wait_for_idle(tse, CORETSE_MII_IND_BUSY);
	if (ret < 0)
		return ret;

	writel_relaxed(value, tse->regs + CORETSE_MII_ADDRESS);
	writel_relaxed(CORETSE_MII_CMD_READ, tse->regs + CORETSE_MII_COMMAND);
	writel_relaxed(0, tse->regs + CORETSE_MII_COMMAND);

	ret = coretse_mdio_wait_for_idle(tse, (CORETSE_MII_IND_NVAL | CORETSE_MII_IND_BUSY));
	if (ret < 0)
		return ret;

	ret = readl_relaxed(tse->regs + CORETSE_MII_STATUS);

	return ret;
}

/**
 * Write register value to MII
 */
static int mchp_coretse_mdio_write(struct mii_bus *bus, int phy_id, int reg,
				   u16 data)
{
	struct coretse *tse = bus->priv;
	u32 value;
	int ret = 0;

	/**
	 * Set PHY & REG addresses, then write data
	 */

	value = (phy_id << CORETSE_MII_ADR_PHY_BIT) |
		 (reg << CORETSE_MII_ADR_REG_BIT);

	ret = coretse_mdio_wait_for_idle(tse, CORETSE_MII_IND_BUSY);
	if (ret < 0)
		return ret;

	writel_relaxed(value, tse->regs + CORETSE_MII_ADDRESS);
	writel_relaxed(data, tse->regs + CORETSE_MII_CTRL);

	ret = coretse_mdio_wait_for_idle(tse, CORETSE_MII_IND_BUSY);

	return ret;
}

static int mchp_coretse_alloc_coherent(struct coretse *lp)
{
	struct coretse_queue *q = &lp->queues[0];

	q->rx_ring = dma_alloc_coherent(&lp->pdev->dev,
					PCDMA_MAX_RX_DESCR * 16,
					&q->rx_ring_dma, GFP_KERNEL);
	if (!q->rx_ring)
		return -ENOMEM;

	q->rx_buffers = dma_alloc_coherent(&lp->pdev->dev,
					   PCDMA_MAX_RX_DESCR *
					   CORETSE_MAX_RBUFF_SZ,
					   &q->rx_buffers_dma, GFP_KERNEL);
	if (!q->rx_buffers) {
		dma_free_coherent(&lp->pdev->dev,
				  PCDMA_MAX_RX_DESCR * 16,
				  q->rx_ring, q->rx_ring_dma);
		q->rx_ring = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void mchp_coretse_free_coherent(struct coretse *lp)
{
	struct coretse_queue *q = &lp->queues[0];

	if (q->rx_ring) {
		dma_free_coherent(&lp->pdev->dev,
				  PCDMA_MAX_RX_DESCR * 16,
				  q->rx_ring, q->rx_ring_dma);
		q->rx_ring = NULL;
	}

	if (q->rx_buffers) {
		dma_free_coherent(&lp->pdev->dev,
				  PCDMA_MAX_RX_DESCR *
				  CORETSE_MAX_RBUFF_SZ,
				  q->rx_buffers, q->rx_buffers_dma);
		q->rx_buffers = NULL;
	}
}

/* Initialize and start the Receiver and Transmit subsystems */
static int mchp_coretse_start(struct coretse *lp)
{
	struct coretse_queue *q = &lp->queues[0];
	dma_addr_t addr;
	int i, ret;

	ret = mchp_coretse_alloc_coherent(lp);
	if (ret)
		return ret;

	writel_relaxed(PCDMA_INT_SRC_CLEAR, lp->s2mm_regs + MM2S_PCDMA_INT_SRC);
	writel_relaxed(PCDMA_INT_SRC_CLEAR, lp->s2mm_regs + S2MM_PCDMA_INT_SRC);

	/* Enable PCMDA interrupts */
	writel_relaxed(PCDMA_INT_MASK, lp->s2mm_regs + MM2S_PCDMA_INT_EN);
	writel_relaxed(PCDMA_INT_MASK, lp->s2mm_regs + S2MM_PCDMA_INT_EN);

	addr = q->rx_buffers_dma;
	for (i = 0; i < PCDMA_MAX_RX_DESCR; i++) {
		q->rx_ring[i].addr0 = (u32)addr;
		q->rx_ring[i].addr1 = (u32)(addr >> 32);
		q->rx_ring[i].length = 0;
		q->rx_ring[i].ctrl = ((i << PCDMA_CMD_ID_OFFSET) |
				      (PCDMA_BURST_TYPE_INC
				       << PCDMA_BURST_TYPE_OFFSET)
				      | PCDMA_START);

		writel_relaxed(q->rx_ring[i].addr0, lp->s2mm_regs +
			       S2MM_PCDMA_ADDR0);
		writel_relaxed(q->rx_ring[i].addr1, lp->s2mm_regs +
			       S2MM_PCDMA_ADDR1);
		writel_relaxed(q->rx_ring[i].ctrl, lp->s2mm_regs + S2MM_PCDMA_CTRL);

		addr += CORETSE_MAX_RBUFF_SZ;
	}

	/* Enable Receive and Transmit */
	 writel_relaxed(CORETSE_CFG1_RX_ENA | CORETSE_CFG1_TX_ENA, lp->regs +
			 CORETSE_CONFIG1);

	return 0;
}

static void mchp_coretse_stop(struct coretse *lp)
{
	/* Disable MAC interrupts */
	writel_relaxed(0, lp->s2mm_regs + MM2S_PCDMA_INT_EN);
	writel_relaxed(0, lp->s2mm_regs + S2MM_PCDMA_INT_EN);

	/* Disable Receiver and Transmitter */
	writel_relaxed(0, lp->regs + CORETSE_CONFIG1);

	/* Free resources. */
	mchp_coretse_free_coherent(lp);
}

static int mchp_coretse_open(struct net_device *dev)
{
	struct coretse *lp = netdev_priv(dev);
	int ret;

	napi_enable(&lp->queues[0].napi_rx);

	ret = coretse_phylink_connect(lp);
	if (ret)
		goto stop;

	ret = mchp_coretse_start(lp);
	if (ret)
		goto open_exit;

	netif_start_queue(dev);

	return 0;

stop:
	mchp_coretse_stop(lp);
open_exit:
	return ret;
}

/* Close the interface */
static int mchp_coretse_close(struct net_device *dev)
{
	struct coretse *lp = netdev_priv(dev);

	netif_stop_queue(dev);
	netif_carrier_off(dev);
	phylink_stop(lp->phylink);
	phylink_disconnect_phy(lp->phylink);

	mchp_coretse_stop(lp);
	napi_disable(&lp->queues[0].napi_rx);

	return 0;
}

/* Transmit packet */
static netdev_tx_t mchp_coretse_start_xmit(struct sk_buff *skb,
					   struct net_device *dev)
{
	struct coretse *lp = netdev_priv(dev);
	u32 addr0;
	u32 addr1;
	int desc = 0;
	int length = 0;

	if (lp->coretse_tx_in_progress) {
		netdev_err(dev, "%s called, but device is busy!\n", __func__);
		return NETDEV_TX_BUSY;
	}

	netif_stop_queue(dev);

	/* Store packet information (to free when Tx completed) */
	lp->rm9200_txq[desc].skb = skb;
	lp->rm9200_txq[desc].size = skb->len;
	lp->rm9200_txq[desc].mapping = dma_map_single(&lp->pdev->dev, skb->data,
						      skb->len, DMA_TO_DEVICE);

	if (dma_mapping_error(&lp->pdev->dev, lp->rm9200_txq[desc].mapping)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		netdev_err(dev, "%s: DMA mapping error\n", __func__);
		return NETDEV_TX_OK;
	}

	lp->coretse_tx_in_progress = 1;

	addr0 = (u32)(lp->rm9200_txq[desc].mapping & MAPPING_MASK);
	addr1 = (u32)((lp->rm9200_txq[desc].mapping >> 32) & MAPPING_MASK);

	/* Set address of the data in the Transmit Address register */
	writel_relaxed(addr0, lp->s2mm_regs + MM2S_PCDMA_ADDR0);
	writel_relaxed(addr1, lp->s2mm_regs + MM2S_PCDMA_ADDR1);
	/* Set length of the packet in the Transmit Control register */
	writel_relaxed(skb->len, lp->s2mm_regs + MM2S_PCDMA_LENGTH);
	length = readl_relaxed(lp->s2mm_regs + MM2S_PCDMA_LENGTH);

	/* start transmission */
	writel_relaxed((MCHP_MM2S_START_ADDR | MCHP_MM2S_BURST_TYPE |
			MCHP_MM2S_START), lp->s2mm_regs + MM2S_PCDMA_CTRL);

	return NETDEV_TX_OK;
}

static int mchp_coretse_change_mtu(struct net_device *dev, int new_mtu)
{
	if (netif_running(dev))
		return -EBUSY;

	dev->mtu = new_mtu;

	return 0;
}

static int coretse_set_mac_address(struct net_device *ndev, void *addr)
{
	struct coretse *bp = netdev_priv(ndev);

	if (addr)
		eth_hw_addr_set(ndev, addr);
	if (!is_valid_ether_addr(ndev->dev_addr))
		eth_hw_addr_random(ndev);

/* TODO: check any other driver is using macros for this */
	writel_relaxed((ndev->dev_addr[0]) |
		       (ndev->dev_addr[1] << 8) |
		       (ndev->dev_addr[2] << 16) |
		       (ndev->dev_addr[3] << 24), bp->regs +
						  CORETSE_STATION_ADDR0);
	writel_relaxed((ndev->dev_addr[4]) |
		       (ndev->dev_addr[5] << 8), bp->regs +
						 CORETSE_STATION_ADDR0);
	return 0;
}

static int mchp_coretse_set_mac_address(struct net_device *ndev, void *p)
{
	struct sockaddr *addr = p;

	coretse_set_mac_address(ndev, addr->sa_data);

	return 0;
}

/* Ioctl MII Interface */
static int mchp_coretse_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct coretse *bp = netdev_priv(dev);

	if (!netif_running(dev))
		return -EINVAL;

	return phylink_mii_ioctl(bp->phylink, rq, cmd);
}

/**
 * Net device descriptor
 */
static const struct net_device_ops mchp_coretse_netdev_ops = {
	.ndo_open		= mchp_coretse_open,
	.ndo_stop		= mchp_coretse_close,
	.ndo_start_xmit		= mchp_coretse_start_xmit,
	.ndo_change_mtu		= mchp_coretse_change_mtu,
	.ndo_set_mac_address	= mchp_coretse_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl = mchp_coretse_ioctl,
	.ndo_do_ioctl = mchp_coretse_ioctl,
};

static int mchp_coretse_mii_probe(struct net_device *dev)
{
	struct coretse *bp = netdev_priv(dev);

	bp->phylink_config.dev = &dev->dev;
	bp->phylink_config.type = PHYLINK_NETDEV;
	bp->phylink_config.mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
		MAC_10FD | MAC_100FD | MAC_1000FD;
	bp->phylink_config.poll_fixed_state = true;
	__set_bit(bp->phy_interface,
		  bp->phylink_config.supported_interfaces);

	bp->phylink = phylink_create(&bp->phylink_config, bp->pdev->dev.fwnode,
				     bp->phy_interface, &coretse_phylink_ops);
	if (IS_ERR(bp->phylink)) {
		netdev_err(dev, "Could not create a phylink instance (%ld)\n",
			   PTR_ERR(bp->phylink));
		return PTR_ERR(bp->phylink);
	}

	return 0;
}

static int mchp_coretse_mdiobus_register(struct coretse *bp)
{
	struct device_node *child, *np = bp->pdev->dev.of_node;
	int ret;

	/* If we have a child named mdio, probe it instead of looking for PHYs
	 * directly under the MAC node
	 */
	child = of_get_child_by_name(np, "mdio");
	if (child) {
		ret = of_mdiobus_register(bp->mii_bus, child);

		of_node_put(child);
		return ret;
	}

	if (of_phy_is_fixed_link(np))
		return mdiobus_register(bp->mii_bus);

	/* Only create the PHY from the device tree if at least one PHY is
	 * described. Otherwise scan the entire MDIO bus. We do this to support
	 * old device tree that did not follow the best practices and did not
	 * describe their network PHYs.
	 */
	for_each_available_child_of_node(np, child)
		if (of_mdiobus_child_is_phy(child)) {
			/* The loop increments the child refcount,
			 * decrement it before returning.
			 */
			of_node_put(child);

			return of_mdiobus_register(bp->mii_bus, np);
		}

	return mdiobus_register(bp->mii_bus);
}

static int mchp_coretse_mii_init(struct coretse *bp)
{
	struct device_node *np;
	int err = -ENXIO;

	bp->mii_bus = mdiobus_alloc();
	if (!bp->mii_bus) {
		err = -ENOMEM;
		goto err_out;
	}

	bp->mii_bus->name = "Microchip CoreTSE MDIO";
	bp->mii_bus->read = &mchp_coretse_mdio_read;
	bp->mii_bus->write = &mchp_coretse_mdio_write;

	snprintf(bp->mii_bus->id, MII_BUS_ID_SIZE, "%s-%x",
		 bp->pdev->name, bp->pdev->id);

	bp->mii_bus->priv = bp;
	bp->mii_bus->parent = &bp->pdev->dev;

	dev_set_drvdata(&bp->dev->dev, bp->mii_bus);

	np = of_parse_phandle(bp->pdev->dev.of_node, "pcs-handle", 0);
	if (!np)
		np = of_parse_phandle(bp->pdev->dev.of_node, "phy-handle", 0);

	if (!np) {
		err = -EINVAL;
		goto err_out_free_mdiobus;
	}

	if (np) {
		err = mchp_coretse_mdiobus_register(bp);
		if (err)
			goto err_out_free_mdiobus;
	}

	bp->pcs_phy = of_mdio_find_device(np);
	if (!bp->pcs_phy) {
		err = -EPROBE_DEFER;
		of_node_put(np);
		goto err_out_unregister_bus;
	}

	of_node_put(np);
	bp->pcs.ops = &coretse_pcs_ops;
	bp->pcs.poll = true;

	err = mchp_coretse_mii_probe(bp->dev);
	if (err)
		goto err_out_unregister_bus;

	return 0;

err_out_unregister_bus:
	mdiobus_unregister(bp->mii_bus);
err_out_free_mdiobus:
	mdiobus_free(bp->mii_bus);
err_out:
	return err;
}

static int mchp_pcdma_probe(struct platform_device *pdev,
			    struct coretse *bp)
{
	struct device_node *np;
	struct resource s2mres;
	char intr_name[24];
	int ret;

	np = of_parse_phandle(pdev->dev.of_node, "pcdma-connected", 0);
	if (IS_ERR(np)) {
		dev_err(&pdev->dev, "could not find pcdma node\n");
		return PTR_ERR(np);
	}

	ret = of_address_to_resource(np, 0, &s2mres);
	if (ret) {
		dev_err(&pdev->dev, "unable to get pcdma resource\n");
		return ret;
	}

	bp->s2mm_regs = devm_ioremap_resource(&pdev->dev, &s2mres);
	if (IS_ERR(bp->s2mm_regs)) {
		dev_err(&pdev->dev, "iormeap failed for the pcdma\n");
		ret = PTR_ERR(bp->s2mm_regs);
		return ret;
	}

	snprintf(intr_name, sizeof(intr_name), "s2mm_pcdma");

	bp->rx_irq = platform_get_irq_byname(pdev, intr_name);
	if (bp->rx_irq < 0)
		return bp->rx_irq;

	snprintf(intr_name, sizeof(intr_name), "mm2s_pcdma");

	bp->tx_irq = platform_get_irq_byname(pdev, intr_name);
	if (bp->tx_irq < 0)
		return bp->tx_irq;

	return 0;
}

static int mchp_coretse_hw_init(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct coretse *bp = netdev_priv(dev);
	int ret;
	u32 reg;

	bp->queues[0].bp = bp;

	dev->netdev_ops = &mchp_coretse_netdev_ops;

	ret = request_irq(bp->tx_irq, mchp_pcdma_tx_irq,
			  IRQF_SHARED, dev->name, dev);
	if (ret)
		return ret;

	ret = request_irq(bp->rx_irq, mchp_pcdma_rx_irq,
			  IRQF_SHARED, dev->name, dev);
	if (ret)
		return ret;
	/**
	 * Reset all PE-MCXMAC modules, and configure
	 */
	reg = readl_relaxed(bp->regs + CORETSE_CONFIG2);
	reg &= ~(CORETSE_CFG2_MODE_MSK << CORETSE_CFG2_MODE_BIT);
	writel_relaxed(reg, bp->regs + CORETSE_CONFIG2);

	reg = readl_relaxed(bp->regs + CORETSE_CONFIG1);
	reg |= CORETSE_CFG1_RST;
	writel_relaxed(reg, bp->regs + CORETSE_CONFIG1);

	writel_relaxed(CORETSE_MGMT_CLOCK_SEL, bp->regs + CORETSE_MII_CONFIG);

	writel_relaxed(reg, bp->regs + CORETSE_FIFO_CONFIG0);
	reg &= ~CORETSE_FIFO_CFG0_ALL_RST;
	writel_relaxed(reg, bp->regs + CORETSE_FIFO_CONFIG0);

	writel_relaxed(0, bp->regs + CORETSE_CONFIG1);

	reg = readl_relaxed(bp->regs + CORETSE_CONFIG2);
	reg &= ~(CORETSE_CFG2_MODE_MSK << CORETSE_CFG2_MODE_BIT);
	writel_relaxed(reg, bp->regs + CORETSE_CONFIG2);

	/* TBI or GMII*/
	reg = CORETSE_CFG2_FULL_DUP | CORETSE_CFG2_CRC_EN
		 | CORETSE_CFG2_PAD_CRC | CORETSE_CFG2_LEN_CHECK
		 | (CORETSE_CFG2_MODE_BYTE << CORETSE_CFG2_MODE_BIT)
		 | (CORETSE_CFG2_PREAM_LEN_DEFAULT <<
		    CORETSE_CFG2_PREAM_LEN_BIT);

	writel_relaxed(reg, bp->regs + CORETSE_CONFIG2);

	writel_relaxed(IFG_VALUE, bp->regs + CORETSE_IFG);

	writel_relaxed(HALF_DUPLEX_VALUE, bp->regs + CORETSE_HALF_DUPLEX);

	writel_relaxed(CORETSE_MAX_RBUFF_SZ, bp->regs + CORETSE_MAX_FRAME_LEN);

	writel_relaxed(CORETSE_FIFO_CFG0_ALL_REQ, bp->regs +
		       CORETSE_FIFO_CONFIG0);

	writel_relaxed(FIFO_CONFIG1_VALUE, bp->regs + CORETSE_FIFO_CONFIG1);
	writel_relaxed(FIFO_CONFIG2_VALUE, bp->regs + CORETSE_FIFO_CONFIG2);
	writel_relaxed(FIFO_CONFIG3_VALUE, bp->regs + CORETSE_FIFO_CONFIG3);

	/* Filter out bad packets */
	reg = readl_relaxed(bp->regs + CORETSE_FIFO_CONFIG5);
	reg |= FIFO_CONFIG5_MASK;
	reg &= ~FIFO_CONFIG4_VALUE;
	writel_relaxed(reg, bp->regs + CORETSE_FIFO_CONFIG5);

	writel_relaxed(FIFO_CONFIG4_VALUE, bp->regs + CORETSE_FIFO_CONFIG4);

	writel_relaxed(0x0, bp->regs + CORETSE_MISCC);
	writel_relaxed(MCHP_FRAME_FILTER_CTRL, bp->regs + CORETSE_FPC);

	return 0;
}

static int mchp_coretse_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	phy_interface_t interface;
	struct net_device *dev;
	u8 mac_addr[ETH_ALEN];
	struct coretse *bp;
	struct clk *pclk, *pcdma_clk;
	void __iomem *mem;
	int ret;

	mem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(pclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pclk),
				     "could not get clock\n");

	pcdma_clk = devm_clk_get(&pdev->dev, "pcdma");
	if (IS_ERR(pcdma_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pcdma_clk),
				     "could not get clock\n");

	ret = clk_prepare_enable(pclk);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable clock\n");

	ret = clk_prepare_enable(pcdma_clk);
	if (ret) {
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable clock\n");
		goto err_disable_pclk;
	}

	dev = alloc_etherdev_mq(sizeof(*bp), 1);
	if (!dev) {
		ret = -ENOMEM;
		goto err_disable_clocks;
	}

	SET_NETDEV_DEV(dev, &pdev->dev);

	bp = netdev_priv(dev);
	bp->pdev = pdev;
	bp->dev = dev;
	bp->regs = mem;
	bp->clk = pclk;
	bp->dmaclk = pcdma_clk;
	bp->max_tx_length = MAX_TX_LENGTH;

	spin_lock_init(&bp->tx_lock);
	spin_lock_init(&bp->rx_lock);

	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));

	platform_set_drvdata(pdev, dev);

	/* MTU range: 68 - 1500 */
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;

	ret = mchp_pcdma_probe(pdev, bp);
	if (ret)
		goto err_out_free_netdev;

	/* Retrieve the MAC address */
	ret = of_get_mac_address(np, mac_addr);
	if (!ret) {
		coretse_set_mac_address(dev, mac_addr);
	} else {
		dev_warn(&pdev->dev,
			 "could not find MAC address property: %d\n", ret);
		coretse_set_mac_address(dev, NULL);
	}

	ret = of_get_phy_mode(np, &interface);
	if (ret)
		/* not found in DT, SGMII by default */
		bp->phy_interface = PHY_INTERFACE_MODE_SGMII;
	else
		bp->phy_interface = interface;

	/* IP specific init */
	ret = mchp_coretse_hw_init(pdev);
	if (ret)
		goto err_out_free_netdev;

	ret = mchp_coretse_mii_init(bp);
	if (ret)
		goto err_out_phy_exit;

	netif_carrier_off(dev);

	netif_napi_add(dev, &bp->queues[0].napi_rx, mchp_coretse_rx_poll);

	ret = register_netdev(dev);
	if (ret) {
		dev_err(&pdev->dev, "Cannot register net device, aborting.\n");
		goto err_out_cleanup_phylink;
	}

	pr_info("CoreTSE Probe Done");

	return 0;

err_out_cleanup_phylink:
	phylink_destroy(bp->phylink);

	mdiobus_unregister(bp->mii_bus);
	mdiobus_free(bp->mii_bus);

err_out_phy_exit:
	if (bp->pcs_phy)
		put_device(&bp->pcs_phy->dev);

err_out_free_netdev:
	free_netdev(dev);

err_disable_clocks:
	clk_disable_unprepare(pclk);
	clk_disable_unprepare(pcdma_clk);

err_disable_pclk:
	clk_disable_unprepare(pclk);

	return ret;
}

static int mchp_coretse_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct coretse *bp = netdev_priv(ndev);

	unregister_netdev(ndev);
	netif_napi_del(&bp->queues[0].napi_rx);
	if (bp->phylink)
		phylink_destroy(bp->phylink);

	if (bp->pcs_phy)
		put_device(&bp->pcs_phy->dev);

	if (bp->mii_bus) {
		mdiobus_unregister(bp->mii_bus);
		mdiobus_free(bp->mii_bus);
	}

	clk_disable_unprepare(bp->clk);
	clk_disable_unprepare(bp->dmaclk);

	free_netdev(ndev);

	return 0;
}

static const struct of_device_id mchp_coretse_of_match[] = {
	{ .compatible = "microchip,coretse-rtl-v3", },
	{}
};
MODULE_DEVICE_TABLE(of, mchp_coretse_of_match);

static struct platform_driver mchp_coretse_driver = {
	.probe = mchp_coretse_probe,
	.remove = mchp_coretse_remove,
	.driver = {
		 .name = "microchip_coretse",
		 .of_match_table = mchp_coretse_of_match,
	},
};
module_platform_driver(mchp_coretse_driver);

MODULE_AUTHOR("Praveen Kumar Vattipalli <praveen.kumar@microchip.com>");
MODULE_DESCRIPTION("Microchip CoreTSE(Ethernet) driver");
MODULE_LICENSE("GPL");
