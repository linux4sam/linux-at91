// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Microchip FLEXCOM USART LIN controller
 *
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Balakrishnan Sambath <balakrishnan.s@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/spinlock.h>
#include <net/lin.h>

/*
 * USART register offsets, relative to the USART sub-block. On FLEXCOM parts
 * the USART sits at offset 0x200, so US_LINMR (0x54) maps to FLEX_US_LINMR at
 * 0x254 - the same convention as atmel_serial.
 */
#define US_CR			0x00	/* Control Register (write-only) */
#define   US_CR_RSTRX		BIT(2)
#define   US_CR_RSTTX		BIT(3)
#define   US_CR_RXEN		BIT(4)
#define   US_CR_RXDIS		BIT(5)
#define   US_CR_TXEN		BIT(6)
#define   US_CR_TXDIS		BIT(7)
#define   US_CR_RSTSTA		BIT(8)
#define   US_CR_LINABT		BIT(20)	/* Abort LIN Transmission */

#define US_MR			0x04	/* Mode Register */
#define   US_MR_USMODE		GENMASK(3, 0)
#define     US_MR_USMODE_LIN_MASTER	0xa
#define     US_MR_USMODE_LIN_SLAVE	0xb
#define   US_MR_CHRL		GENMASK(7, 6)
#define     US_MR_CHRL_8	0x3
#define   US_MR_PAR		GENMASK(11, 9)
#define     US_MR_PAR_NONE	0x4
#define   US_MR_NBSTOP		GENMASK(13, 12)
#define     US_MR_NBSTOP_1	0x0

#define US_IER			0x08	/* Interrupt Enable Register */
#define US_IDR			0x0c	/* Interrupt Disable Register */
#define US_IMR			0x10	/* Interrupt Mask Register */
#define US_CSR			0x14	/* Channel Status Register */
#define US_RHR			0x18	/* Receiver Holding Register */
#define US_THR			0x1c	/* Transmitter Holding Register */
#define US_BRGR			0x20	/* Baud Rate Generator Register */
#define   US_BRGR_CD		GENMASK(15, 0)

/* CSR / IER common bits */
#define US_INT_RXRDY		BIT(0)
#define US_INT_TXRDY		BIT(1)

/* LIN-specific status/interrupt bits (CSR, IER/IDR/IMR) */
#define US_LIN_LINBK		BIT(13)	/* LIN Break Sent/Received */
#define US_LIN_LINID		BIT(14)	/* LIN Identifier Sent/Received */
#define US_LIN_LINTC		BIT(15)	/* LIN Transfer Completed */
#define US_LIN_LINBLS		BIT(23)	/* LIN Bus Line Status (CSR only) */
#define US_LIN_LINBE		BIT(25)	/* LIN Bit Error */
#define US_LIN_LINISFE		BIT(26)	/* LIN Inconsistent Synch Field Error */
#define US_LIN_LINIPE		BIT(27)	/* LIN Identifier Parity Error */
#define US_LIN_LINCE		BIT(28)	/* LIN Checksum Error */
#define US_LIN_LINSNRE		BIT(29)	/* LIN Slave Not Responding Error */
#define US_LIN_LINSTE		BIT(30)	/* LIN Synch Tolerance Error */
#define US_LIN_LINHTE		BIT(31)	/* LIN Header Timeout Error */

#define US_LIN_ERRORS	(US_LIN_LINBE | US_LIN_LINISFE | US_LIN_LINIPE | \
			 US_LIN_LINCE | US_LIN_LINSNRE | US_LIN_LINSTE | \
			 US_LIN_LINHTE)

#define US_LINMR		0x54	/* LIN Mode Register */
#define   US_LINMR_NACT		GENMASK(1, 0)
#define     US_LINMR_NACT_PUBLISH	0x0	/* USART transmits the response */
#define     US_LINMR_NACT_SUBSCRIBE	0x1	/* USART receives the response */
#define     US_LINMR_NACT_IGNORE	0x2
#define   US_LINMR_PARDIS	BIT(2)	/* Parity Disable */
#define   US_LINMR_CHKDIS	BIT(3)	/* Checksum Disable */
#define   US_LINMR_CHKTYP	BIT(4)	/* 0: LIN2.0 enhanced, 1: LIN1.3 classic */
#define   US_LINMR_DLM		BIT(5)	/* Data Length Mode (1: len from ID) */
#define   US_LINMR_FSDIS	BIT(6)	/* Frame Slot Mode Disable */
#define   US_LINMR_WKUPTYP	BIT(7)
#define   US_LINMR_DLC		GENMASK(15, 8)	/* response len = DLC + 1 */
#define   US_LINMR_PDCM		BIT(16)
#define   US_LINMR_SYNCDIS	BIT(17)

#define US_LINIR		0x58	/* LIN Identifier Register */
#define   US_LINIR_IDCHR	GENMASK(7, 0)

#define US_LINBRR		0x5c	/* LIN Baud Rate Register (read-only) */
#define   US_LINBRR_LINCD	GENMASK(15, 0)
#define   US_LINBRR_LINFP	GENMASK(18, 16)

/* diagnostic ids use the classic checksum, all others enhanced */
#define LIN_DIAG_MASTER_REQ	0x3c
#define LIN_DIAG_SLAVE_RESP	0x3d

#define MCHP_LIN_OVERSAMPLING	16

/* one frame slot at the slowest rate (1200 baud) is ~160 ms */
#define MCHP_LIN_TX_TIMEOUT_MS	250

/* abort LINTC latch bound, one byte slot at 1200 baud is ~8 ms */
#define MCHP_LIN_ABORT_SETTLE_US	10000

enum mchp_lin_state {
	MCHP_LIN_IDLE = 0,
	MCHP_LIN_RX,		/* subscribe: collecting rx bytes */
	MCHP_LIN_TX,		/* publish: feeding tx */
};

/**
 * struct mchp_lin - per-controller private data
 * @ldev:	the registered LIN device (recovered via dev_get_drvdata)
 * @base:	USART register block (FLEXCOM USART sub-block)
 * @clk:	USART peripheral clock (source for the LIN baud generator)
 * @dev:	owning device
 * @irq:	USART interrupt, freed before the netdev on remove
 * @lock:	guards @answers
 * @answers:	per-identifier responder answers (software responder table;
 *		the hardware is a single-frame engine, so scheduling is in sw)
 * @is_master:	true for LIN commander (host), false for responder (client)
 * @tx_done:	signals the commander that its published frame has completed
 * @tx_err:	set from the IRQ if that frame aborted on a bus error
 * @state:	in-frame state machine, set by ldo_tx and the irq handler
 * @buf:	in-frame TX/RX byte buffer
 * @buf_pos:	bytes transferred so far in the current frame
 * @buf_len:	expected byte count for the current TX frame
 * @cur_id:	identifier of the frame currently being serviced
 * @cur_cksum:	checksum mode of the frame currently being serviced
 */
struct mchp_lin {
	struct lin_device *ldev;
	void __iomem *base;
	struct clk *clk;
	struct device *dev;
	int irq;
	raw_spinlock_t lock;	/* protects @answers; taken in hard irq */
	struct lin_responder_answer answers[LIN_NUM_IDS];
	bool is_master;

	struct completion tx_done;
	bool tx_err;
	enum mchp_lin_state state;
	u8 buf[LIN_MAX_DLEN];
	u8 buf_pos;
	u8 buf_len;
	u8 cur_id;
	enum lin_checksum_mode cur_cksum;
};

static inline u32 mchp_lin_readl(struct mchp_lin *ml, u32 reg)
{
	return readl(ml->base + reg);
}

static inline void mchp_lin_writel(struct mchp_lin *ml, u32 reg, u32 val)
{
	writel(val, ml->base + reg);
}

static struct mchp_lin *ldev_to_mchp(struct lin_device *ldev)
{
	return dev_get_drvdata(ldev->dev);
}

/* Build the LINMR action word. HW CHKTYP is inverted vs the framework enum. */
static u32 mchp_lin_linmr(u32 nact, u8 len, enum lin_checksum_mode cm)
{
	u32 linmr = FIELD_PREP(US_LINMR_NACT, nact);

	linmr |= FIELD_PREP(US_LINMR_DLC, len ? len - 1 : 0);
	if (cm == LINBUS_CLASSIC)
		linmr |= US_LINMR_CHKTYP;

	return linmr;
}

/* Put the USART into LIN mode with 8-bit, no-parity, 1-stop framing. */
static int mchp_lin_ldo_open(struct lin_device *ldev)
{
	struct mchp_lin *ml = ldev_to_mchp(ldev);
	u32 mode, ints;

	/* Reset and disable RX/TX while reconfiguring */
	mchp_lin_writel(ml, US_CR, US_CR_RSTRX | US_CR_RSTTX |
				   US_CR_RXDIS | US_CR_TXDIS | US_CR_RSTSTA);

	mode = FIELD_PREP(US_MR_USMODE, ml->is_master ?
			  US_MR_USMODE_LIN_MASTER : US_MR_USMODE_LIN_SLAVE);
	mode |= FIELD_PREP(US_MR_CHRL, US_MR_CHRL_8);
	mode |= FIELD_PREP(US_MR_PAR, US_MR_PAR_NONE);
	mode |= FIELD_PREP(US_MR_NBSTOP, US_MR_NBSTOP_1);
	mchp_lin_writel(ml, US_MR, mode);

	ml->state = MCHP_LIN_IDLE;
	mchp_lin_writel(ml, US_CR, US_CR_RXEN | US_CR_TXEN);

	/* clear a stale action so a reopen ignores the first header */
	mchp_lin_writel(ml, US_LINMR,
			FIELD_PREP(US_LINMR_NACT, US_LINMR_NACT_IGNORE));

	/* both roles want errors + LINTC; only a responder enables LINID */
	ints = US_LIN_ERRORS | US_LIN_LINTC;
	if (!ml->is_master)
		ints |= US_LIN_LINID;
	mchp_lin_writel(ml, US_IER, ints);

	return 0;
}

static int mchp_lin_ldo_stop(struct lin_device *ldev)
{
	struct mchp_lin *ml = ldev_to_mchp(ldev);

	mchp_lin_writel(ml, US_IDR, ~0u);
	/* wait out any in-flight handler before the netdev is freed */
	synchronize_irq(ml->irq);
	mchp_lin_writel(ml, US_CR, US_CR_RXDIS | US_CR_TXDIS);
	ml->state = MCHP_LIN_IDLE;

	return 0;
}

/*
 * Commander publish: write the identifier to emit break + sync + PID, feed
 * the data bytes to THR from the tx interrupt and block on @tx_done until
 * LINTC while the hardware appends parity and checksum.
 */
static int mchp_lin_ldo_tx(struct lin_device *ldev, const struct lin_frame *lf)
{
	struct mchp_lin *ml = ldev_to_mchp(ldev);
	enum lin_checksum_mode cksum;
	unsigned long tmo;
	bool classic;
	u32 csr;
	int err;

	/* Only a commander originates a header; a responder answers via HW. */
	if (!ml->is_master)
		return -EOPNOTSUPP;

	if (!lf->len || lf->len > LIN_MAX_DLEN)
		return -EINVAL;

	/* diagnostic ids always use the classic checksum, all others honour
	 * the caller's request (LIN_ENHANCED_CKSUM_FLAG in can_id)
	 */
	classic = lf->lin_id == LIN_DIAG_MASTER_REQ ||
		  lf->lin_id == LIN_DIAG_SLAVE_RESP;
	cksum = classic ? LINBUS_CLASSIC : lf->checksum_mode;

	ml->buf_len = lf->len;
	ml->buf_pos = 0;
	memcpy(ml->buf, lf->data, lf->len);
	ml->tx_err = false;
	ml->state = MCHP_LIN_TX;
	reinit_completion(&ml->tx_done);

	mchp_lin_writel(ml, US_LINMR,
			mchp_lin_linmr(US_LINMR_NACT_PUBLISH, lf->len,
				       cksum));
	/* the id write emits break + sync + PID; arm the feed only after */
	mchp_lin_writel(ml, US_LINIR,
			FIELD_PREP(US_LINIR_IDCHR, lf->lin_id & LIN_ID_MASK));
	mchp_lin_writel(ml, US_IER, US_INT_TXRDY);

	tmo = wait_for_completion_timeout(&ml->tx_done,
					  msecs_to_jiffies(MCHP_LIN_TX_TIMEOUT_MS));
	if (!tmo) {
		/* mask, wait for the handler to finish, then re-check: a frame
		 * completing at the timeout boundary may still have signalled
		 * tx_done
		 */
		mchp_lin_writel(ml, US_IDR,
				US_INT_TXRDY | US_LIN_LINTC | US_LIN_ERRORS);
		synchronize_irq(ml->irq);
		if (try_wait_for_completion(&ml->tx_done)) {
			mchp_lin_writel(ml, US_IER,
					US_LIN_LINTC | US_LIN_ERRORS);
			return ml->tx_err ? -EIO : 0;
		}
		mchp_lin_writel(ml, US_CR, US_CR_LINABT | US_CR_RSTSTA);
		/* abort settles on bus time, synchronize_irq cannot see it */
		err = readl_poll_timeout_atomic(ml->base + US_CSR, csr,
						csr & US_LIN_LINTC, 5,
						MCHP_LIN_ABORT_SETTLE_US);
		if (err) {
			/* reset tx and rx to drop the stuck transfer */
			netdev_warn_once(ldev->ndev, "abort did not settle\n");
			mchp_lin_writel(ml, US_CR,
					US_CR_RSTTX | US_CR_RSTRX | US_CR_RSTSTA);
		} else {
			mchp_lin_writel(ml, US_CR, US_CR_RSTSTA);
		}
		reinit_completion(&ml->tx_done);
		/* reset state before re-arming so no stale TX completes */
		ml->state = MCHP_LIN_IDLE;
		mchp_lin_writel(ml, US_IER, US_LIN_LINTC | US_LIN_ERRORS);
		return -ETIMEDOUT;
	}

	return ml->tx_err ? -EIO : 0;
}

static int mchp_lin_update_bitrate(struct lin_device *ldev, u16 bitrate)
{
	struct mchp_lin *ml = ldev_to_mchp(ldev);
	unsigned long clk_rate = clk_get_rate(ml->clk);
	u32 cd;

	if (!bitrate)
		return -EINVAL;

	/* responder re-times in HW from the sync field, so this is only a seed */
	cd = DIV_ROUND_CLOSEST(clk_rate, MCHP_LIN_OVERSAMPLING * bitrate);
	if (cd < 1 || cd > FIELD_MAX(US_BRGR_CD))
		return -EINVAL;

	mchp_lin_writel(ml, US_BRGR, FIELD_PREP(US_BRGR_CD, cd));

	return 0;
}

static int mchp_lin_update_responder_answer(struct lin_device *ldev,
					    const struct lin_responder_answer *answ)
{
	struct mchp_lin *ml = ldev_to_mchp(ldev);
	u8 id = answ->lf.lin_id & LIN_ID_MASK;
	unsigned long flags;

	/* Only unconditional frames are supported, not event-triggered ones. */
	if (answ->is_event_frame)
		return -EOPNOTSUPP;

	/* a published answer must carry 1..LIN_MAX_DLEN bytes */
	if (answ->is_active && (!answ->lf.len || answ->lf.len > LIN_MAX_DLEN))
		return -EINVAL;

	raw_spin_lock_irqsave(&ml->lock, flags);
	ml->answers[id] = *answ;
	raw_spin_unlock_irqrestore(&ml->lock, flags);

	return 0;
}

static int mchp_lin_get_responder_answer(struct lin_device *ldev, u8 id,
					 struct lin_responder_answer *answ)
{
	struct mchp_lin *ml = ldev_to_mchp(ldev);
	unsigned long flags;

	if (id >= LIN_NUM_IDS)
		return -EINVAL;

	raw_spin_lock_irqsave(&ml->lock, flags);
	*answ = ml->answers[id];
	raw_spin_unlock_irqrestore(&ml->lock, flags);

	return 0;
}

/* a commander never answers headers, so it omits the responder callbacks
 * and the core then hides the per-id answer attrs
 */
static const struct lin_device_ops mchp_lin_commander_ops = {
	.ldo_open			= mchp_lin_ldo_open,
	.ldo_stop			= mchp_lin_ldo_stop,
	.ldo_tx				= mchp_lin_ldo_tx,
	.update_bitrate			= mchp_lin_update_bitrate,
};

static const struct lin_device_ops mchp_lin_responder_ops = {
	.ldo_open			= mchp_lin_ldo_open,
	.ldo_stop			= mchp_lin_ldo_stop,
	.ldo_tx				= mchp_lin_ldo_tx,
	.update_bitrate			= mchp_lin_update_bitrate,
	.update_responder_answer	= mchp_lin_update_responder_answer,
	.get_responder_answer		= mchp_lin_get_responder_answer,
};

/* Report a hardware-detected LIN error as a CAN error frame. */
static void mchp_lin_report_error(struct mchp_lin *ml, u32 csr, bool tx)
{
	struct net_device *ndev = ml->ldev->ndev;
	struct can_frame *cf;
	struct sk_buff *skb;

	if (tx)
		DEV_STATS_INC(ndev, tx_errors);
	else
		DEV_STATS_INC(ndev, rx_errors);

	skb = alloc_can_err_skb(ndev, &cf);
	if (!skb)
		return;

	/* map LIN error bits onto CAN error classes, best effort */
	if (csr & (US_LIN_LINIPE | US_LIN_LINCE | US_LIN_LINISFE |
		   US_LIN_LINSTE | US_LIN_LINHTE | US_LIN_LINBE)) {
		cf->can_id |= CAN_ERR_PROT;
		cf->data[2] |= CAN_ERR_PROT_FORM;
	}
	if (csr & US_LIN_LINCE)
		cf->data[3] |= CAN_ERR_PROT_LOC_CRC_SEQ;
	if (csr & US_LIN_LINSNRE)
		cf->can_id |= CAN_ERR_ACK;

	netif_rx(skb);
}

/* Responder received a header: publish a stored answer or subscribe. */
static void mchp_lin_handle_header(struct mchp_lin *ml)
{
	struct lin_responder_answer answ;
	u8 id = FIELD_GET(US_LINIR_IDCHR, mchp_lin_readl(ml, US_LINIR));
	unsigned long flags;

	id &= LIN_ID_MASK;
	ml->cur_id = id;

	raw_spin_lock_irqsave(&ml->lock, flags);
	answ = ml->answers[id];
	raw_spin_unlock_irqrestore(&ml->lock, flags);

	if (answ.is_active) {
		/* publish the stored answer; diagnostic ids force classic */
		bool classic = id == LIN_DIAG_MASTER_REQ ||
			       id == LIN_DIAG_SLAVE_RESP;

		ml->cur_cksum = classic ? LINBUS_CLASSIC : answ.lf.checksum_mode;
		ml->buf_len = min_t(u8, answ.lf.len, LIN_MAX_DLEN);
		ml->buf_pos = 0;
		memcpy(ml->buf, answ.lf.data, ml->buf_len);

		mchp_lin_writel(ml, US_LINMR,
				mchp_lin_linmr(US_LINMR_NACT_PUBLISH,
					       ml->buf_len, ml->cur_cksum));
		ml->state = MCHP_LIN_TX;
		/* Feed bytes as the holding register drains. */
		mchp_lin_writel(ml, US_IER, US_INT_TXRDY);
	} else {
		/* subscribe: use DLC for a configured len; with none, fall back
		 * to DLM (HW derives len from the id, LIN 1.3 style). cksum
		 * follows the id; diag ids are classic.
		 */
		bool classic = id == LIN_DIAG_MASTER_REQ ||
			       id == LIN_DIAG_SLAVE_RESP ||
			       answ.lf.checksum_mode == LINBUS_CLASSIC;
		u8 len = min_t(u8, answ.lf.len, LIN_MAX_DLEN);
		u32 linmr = FIELD_PREP(US_LINMR_NACT, US_LINMR_NACT_SUBSCRIBE);

		if (len)
			linmr |= FIELD_PREP(US_LINMR_DLC, len - 1);
		else
			linmr |= US_LINMR_DLM;
		if (classic)
			linmr |= US_LINMR_CHKTYP;
		mchp_lin_writel(ml, US_LINMR, linmr);
		ml->buf_pos = 0;
		ml->cur_cksum = classic ? LINBUS_CLASSIC : LINBUS_ENHANCED;
		ml->state = MCHP_LIN_RX;
		/* drop the header PID left in RHR so it is not read as data[0] */
		if (mchp_lin_readl(ml, US_CSR) & US_INT_RXRDY)
			mchp_lin_readl(ml, US_RHR);
		mchp_lin_writel(ml, US_IER, US_INT_RXRDY);
	}
}

static bool mchp_lin_handle_complete(struct mchp_lin *ml)
{
	struct lin_frame lf = {};
	bool wake = false;

	switch (ml->state) {
	case MCHP_LIN_RX:
		mchp_lin_writel(ml, US_IDR, US_INT_RXRDY);
		lf.lin_id = ml->cur_id;
		lf.len = ml->buf_pos;
		memcpy(lf.data, ml->buf, ml->buf_pos);
		lf.checksum_mode = ml->cur_cksum;
		/* recompute the checksum so the lin_rx() software check agrees */
		lf.checksum = lin_get_checksum(LIN_FORM_PID(lf.lin_id),
					       lf.len, lf.data, lf.checksum_mode);
		lin_rx(ml->ldev, &lf);
		break;
	case MCHP_LIN_TX:
		mchp_lin_writel(ml, US_IDR, US_INT_TXRDY);
		if (ml->is_master) {
			/* ldo_tx is woken later, once the frame is torn down */
			wake = true;
		} else {
			DEV_STATS_INC(ml->ldev->ndev, tx_packets);
			DEV_STATS_ADD(ml->ldev->ndev, tx_bytes, ml->buf_len);
		}
		break;
	default:
		break;
	}
	ml->state = MCHP_LIN_IDLE;

	return wake;
}

static irqreturn_t mchp_lin_irq(int irq, void *dev_id)
{
	struct mchp_lin *ml = dev_id;
	u32 csr = mchp_lin_readl(ml, US_CSR);
	u32 imr = mchp_lin_readl(ml, US_IMR);
	bool wake = false;

	csr &= imr;
	if (!csr)
		return IRQ_NONE;

	/* no ldev yet: mask and clear so the level line cannot re-fire */
	if (unlikely(!ml->ldev)) {
		mchp_lin_writel(ml, US_IDR, ~0u);
		mchp_lin_writel(ml, US_CR, US_CR_RSTSTA);
		return IRQ_HANDLED;
	}

	if (csr & US_LIN_ERRORS) {
		bool tx = ml->state == MCHP_LIN_TX;

		mchp_lin_report_error(ml, csr, tx);
		mchp_lin_writel(ml, US_CR, US_CR_RSTSTA);
		mchp_lin_writel(ml, US_IDR, US_INT_RXRDY | US_INT_TXRDY);
		ml->state = MCHP_LIN_IDLE;
		/* Fail fast instead of waiting out the timeout in ldo_tx. */
		if (ml->is_master && tx) {
			ml->tx_err = true;
			complete(&ml->tx_done);
		}
		/* a header latched with any error carries a bad identifier, so
		 * drop it rather than publish or subscribe on it
		 */
		return IRQ_HANDLED;
	}

	/* Responder data movement within the frame slot. */
	if (ml->state == MCHP_LIN_RX && (csr & US_INT_RXRDY)) {
		if (ml->buf_pos < LIN_MAX_DLEN)
			ml->buf[ml->buf_pos++] = mchp_lin_readl(ml, US_RHR);
		else
			mchp_lin_writel(ml, US_IDR, US_INT_RXRDY);
	}
	if (ml->state == MCHP_LIN_TX && (csr & US_INT_TXRDY)) {
		if (ml->buf_pos < ml->buf_len)
			mchp_lin_writel(ml, US_THR, ml->buf[ml->buf_pos++]);
		else
			mchp_lin_writel(ml, US_IDR, US_INT_TXRDY);
	}

	/* finish the old frame before a new LINID starts the next one */
	if (csr & US_LIN_LINTC)
		wake = mchp_lin_handle_complete(ml);

	/* responder header detect (slave only; on master LINID is an echo) */
	if (!ml->is_master && (csr & US_LIN_LINID))
		mchp_lin_handle_header(ml);

	/* LINID/LINTC latch in US_CSR; clear with RSTSTA or the irq refires */
	if (csr & (US_LIN_LINID | US_LIN_LINTC))
		mchp_lin_writel(ml, US_CR, US_CR_RSTSTA);

	/* the frame is torn down and status cleared, so ldo_tx can wake now */
	if (wake)
		complete(&ml->tx_done);

	return IRQ_HANDLED;
}

static int mchp_lin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mchp_lin *ml;
	struct lin_device *ldev;
	int irq, ret;

	ml = devm_kzalloc(dev, sizeof(*ml), GFP_KERNEL);
	if (!ml)
		return -ENOMEM;

	ml->dev = dev;
	raw_spin_lock_init(&ml->lock);
	init_completion(&ml->tx_done);
	/* Commander unless the board wires this node as a responder. */
	ml->is_master = !device_property_read_bool(dev, "lin-responder");

	ml->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ml->base))
		return PTR_ERR(ml->base);

	ml->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(ml->clk))
		return dev_err_probe(dev, PTR_ERR(ml->clk), "missing clk\n");

	/* Mask all interrupt sources before requesting the irq. */
	mchp_lin_writel(ml, US_IDR, ~0u);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ml->irq = irq;

	/* ops recover @ml via drvdata, so set it before register_lin() */
	dev_set_drvdata(dev, ml);

	/* hard irq: the rx holding register is single-byte, drain per char */
	ret = request_irq(irq, mchp_lin_irq, 0, dev_name(dev), ml);
	if (ret)
		return ret;

	/* keep the irq masked till ml->ldev is live so a racing open cannot
	 * fire the handler through the no-ldev path
	 */
	disable_irq(irq);
	ldev = register_lin(dev, ml->is_master ? &mchp_lin_commander_ops :
						 &mchp_lin_responder_ops);
	if (IS_ERR(ldev)) {
		enable_irq(irq);
		free_irq(irq, ml);
		return dev_err_probe(dev, PTR_ERR(ldev), "register_lin failed\n");
	}
	ml->ldev = ldev;
	enable_irq(irq);

	return 0;
}

static void mchp_lin_remove(struct platform_device *pdev)
{
	struct mchp_lin *ml = platform_get_drvdata(pdev);

	/* unregister_lin() masks the irq via ldo_stop(); free it afterwards */
	unregister_lin(ml->ldev);
	free_irq(ml->irq, ml);
}

static const struct of_device_id mchp_lin_of_match[] = {
	{ .compatible = "microchip,sama5d2-usart-lin" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mchp_lin_of_match);

static struct platform_driver mchp_lin_driver = {
	.driver = {
		.name		= "microchip_usart_lin",
		.of_match_table	= mchp_lin_of_match,
	},
	.probe	= mchp_lin_probe,
	.remove	= mchp_lin_remove,
};
module_platform_driver(mchp_lin_driver);

MODULE_AUTHOR("Balakrishnan Sambath <balakrishnan.s@microchip.com>");
MODULE_DESCRIPTION("Microchip USART hardware-offload LIN controller driver");
MODULE_LICENSE("GPL");
