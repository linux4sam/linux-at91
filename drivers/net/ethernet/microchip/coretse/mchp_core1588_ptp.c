// SPDX-License-Identifier: GPL-2.0
/*
 * 1588 PTP support for Microchip Core1588 IP.
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Praveen Kumar Vattipalli <praveen.kumar@microchip.com>
 *
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/time64.h>
#include <linux/ptp_classify.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/net_tstamp.h>
#include <linux/circ_buf.h>
#include <linux/spinlock.h>
#include <linux/of_address.h>

#include "mchp_core1588_ptp.h"

#define  MCHP_CORE1588_TIMER_NAME "mchp-core1588-timer"

static inline void mchp_core1588_timer_reg_write(struct mchp_core1588_timer *timer,
						 u32 reg, u32 val)
{
	writel(val, timer->base + reg);
}

static inline u32 mchp_core1588_timer_reg_read(struct mchp_core1588_timer *timer,
					       u32 reg)
{
	return readl(timer->base + reg);
}

static void mchp_core1588_clear_timer(struct mchp_core1588_timer *timer)
{
	timer->tsu_incr.sub_ns = 0;
	timer->tsu_incr.ns = 0;

	mchp_core1588_timer_reg_write(timer, MCHP_CORE1588_RTCCTRL, 0);
	mchp_core1588_timer_reg_write(timer, MCHP_CORE1588_RTCICR, 0);
}

/**
 * mchp_core1588_ptp_gettime - Get the current time on the hardware clock
 * @ptp: ptp clock structure
 * @ts: timespec64 containing the current TX port timer time.
 * @sts: structure to hold the system time before and after reading the PHC
 * Return: 0 on success
 *
 * read the timecounter and return the correct value on ns,
 * after converting it into a struct timespec.
 */
static int mchp_core1588_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts,
				     struct ptp_system_timestamp *sts)
{
	struct mchp_core1588_timer *timer = container_of(ptp, struct mchp_core1588_timer,
					ptp_clock_info);
	long first, second;
	unsigned long flags;
	u32 secl, sech;

	spin_lock_irqsave(&timer->reg_lock, flags);
	ptp_read_system_prets(sts);

	first = mchp_core1588_timer_reg_read(timer, RTC_NSEC_29BITS);

	ptp_read_system_postts(sts);

	secl =  mchp_core1588_timer_reg_read(timer, RTC_SEC_LSB_32BITS);
	sech =  mchp_core1588_timer_reg_read(timer, RTC_SEC_MSB_16BITS);
	second = mchp_core1588_timer_reg_read(timer, RTC_NSEC_29BITS);

	/* test for nsec rollover */
	if (first > second) {
		/* if so, use later read & re-read seconds
		 * (assume all done within 1s)
		 */
		ptp_read_system_prets(sts);
		ts->tv_nsec = mchp_core1588_timer_reg_read(timer, RTC_NSEC_29BITS);
		ptp_read_system_postts(sts);
		secl =  mchp_core1588_timer_reg_read(timer, RTC_SEC_LSB_32BITS);
		sech =  mchp_core1588_timer_reg_read(timer, RTC_SEC_MSB_16BITS);
	} else {
		ts->tv_nsec = first;
	}

	spin_unlock_irqrestore(&timer->reg_lock, flags);

	ts->tv_sec = ((((u64)sech << CORE1588_TSL_SIZE) | secl)
			& CORE1588_SEC_MAX_VAL);

	return 0;
}

/**
 * mchp_core1588_ptp_settime - Set the current time on the hardware clock
 * @ptp: ptp clock structure
 * @ts: timespec64 containing the new time
 * Return: 0 on success
 *
 * reset the timecounter to use a new base value instead of the kernel
 * wall timer value.
 */
static int mchp_core1588_ptp_settime(struct ptp_clock_info *ptp,
				     const struct timespec64 *ts)
{
	struct mchp_core1588_timer *timer = container_of(ptp, struct mchp_core1588_timer,
					ptp_clock_info);
	u32 ns, sech, secl;
	unsigned long flags;

	secl = (u32)ts->tv_sec;
	sech = (ts->tv_sec >> CORE1588_TSL_SIZE) & ((1 << CORE1588_TSH_SIZE) - 1);
	ns = ts->tv_nsec;

	spin_lock_irqsave(&timer->reg_lock, flags);

	mchp_core1588_timer_reg_write(timer, RTC_NSEC_29BITS, ns);
	mchp_core1588_timer_reg_write(timer, RTC_SEC_LSB_32BITS, secl);
	mchp_core1588_timer_reg_write(timer, RTC_SEC_MSB_16BITS, sech);

	spin_unlock_irqrestore(&timer->reg_lock, flags);

	return 0;
}

/**
 * mchp_core1588_timer_incr_set - utility function to adjust nanoseconds and subns
 * @timer: the private struct
 * @incr_spec: structure holds increment nanoseconds and sub-nanoseconds
 */
static int mchp_core1588_timer_incr_set(struct mchp_core1588_timer *timer,
					struct core1588_tsu_incr *incr_spec)
{
	u32 rtcincr;
	unsigned long flags;

	/**
	 * RTCINCR is used to increment nanoseconds and sub-nanoseconds
	 * in every clock cycle. RTCCTRL is used to add/subtract integral values
	 * of nanoseconds in one-off operation.
	 */

	spin_lock_irqsave(&timer->reg_lock, flags);
	/**
	 * RegBit[31:24] = NS_INCR ; RegBit[23:0] = SUB_NS_INCR
	 */

	rtcincr = (incr_spec->sub_ns | (incr_spec->ns << CORE1588_SUBNSINCR_SIZE));

	mchp_core1588_timer_reg_write(timer, MCHP_CORE1588_RTCICR, rtcincr);

	spin_unlock_irqrestore(&timer->reg_lock, flags);

	return 0;
}

/**
 * mchp_core1588_ptp_adjfine - Fine adjustment of the frequency on the hardware clock
 * @ptp: ptp clock structure
 * @scaled_ppm: signed scaled parts per million for frequency adjustment.
 * Return: 0 on success
 *
 * Adjust the frequency of the SYSTIME registers by the indicated scaled_ppm
 * from base frequency.
 *
 * Scaled parts per million is ppm with a 16-bit binary fractional field.
 */
static int mchp_core1588_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct mchp_core1588_timer *timer = container_of(ptp, struct mchp_core1588_timer,
					ptp_clock_info);
	struct core1588_tsu_incr incr_spec;
	bool neg_adj = false;
	u32 word;
	u64 adj;

	if (scaled_ppm < 0) {
		neg_adj = true;
		scaled_ppm = -scaled_ppm;
	}

	/* Adjustment is relative to base frequency */
	incr_spec.sub_ns = timer->tsu_incr.sub_ns;
	incr_spec.ns = timer->tsu_incr.ns;

	/* scaling: unused(8bit) | ns(8bit) | fractions(16bit) */
	word = ((u64)incr_spec.ns << CORE1588_SUBNSINCR_SIZE) + incr_spec.sub_ns;
	adj = (u64)scaled_ppm * word;
	/* Divide with rounding, equivalent to floating dividing:
	 * (temp / USEC_PER_SEC) + 0.5
	 */
	adj += (USEC_PER_SEC >> 1);
	adj >>= PPM_FRACTION; /* remove fractions */
	adj = div_u64(adj, USEC_PER_SEC);
	adj = neg_adj ? (word - adj) : (word + adj);

	incr_spec.ns = (adj >> CORE1588_SUBNSINCR_SIZE)
			& ((1 << CORE1588_NSINCR_SIZE) - 1);
	incr_spec.sub_ns = adj & ((1 << CORE1588_SUBNSINCR_SIZE) - 1);

	mchp_core1588_timer_incr_set(timer, &incr_spec);

	return 0;
}

/**
 * mchp_core1588_ptp_adjtime - Adjust the current time on the hardware clock
 * @ptp: ptp clock structure
 * @delta: signed time in ns to be adjusted.
 * Return: 0 on success
 *
 * adjust the timer by resetting the timecounter structure.
 *
 */
static int mchp_core1588_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct mchp_core1588_timer *timer = container_of(ptp, struct mchp_core1588_timer,
					ptp_clock_info);
	struct timespec64 now, then = ns_to_timespec64(delta);
	u32 adj, sign = 0;

	if (delta < 0) {
		sign = 1;
		delta = -delta;
	}

	if (delta > CORE1588_NSEC_MAX_VAL) {
		mchp_core1588_ptp_gettime(&timer->ptp_clock_info, &now, NULL);
		now = timespec64_add(now, then);

		mchp_core1588_ptp_settime(&timer->ptp_clock_info,
					  (const struct timespec64 *)&now);
	} else {
		adj = (sign << CORE1588_ADDSUB_OFFSET) | delta;

		mchp_core1588_timer_reg_write(timer, MCHP_CORE1588_RTCCTRL, adj);
	}

	return 0;
}

/**
 * mchp_core1588_ptp_enable
 * @ptp: the ptp clock structure
 * @rq: the requested feature to change
 * @on: whether to enable or disable the feature
 *
 * enable (or disable) ancillary features of the phc subsystem.
 * our driver do not support this right now
 *
 * TODO:Next enhance this driver to use PTP_CLK_REQ_EXTTS and ISR
 */
static int mchp_core1588_ptp_enable(struct ptp_clock_info *ptp,
				    struct ptp_clock_request *rq, int on)
{
	int ret;
	struct mchp_core1588_timer *timer = container_of(ptp, struct mchp_core1588_timer,
					ptp_clock_info);

	ret = mchp_core1588_timer_reg_read(timer, CORE1588_GCFG);
	ret |= CORE1588_GCFG_EN;

	mchp_core1588_timer_reg_write(timer, CORE1588_GCFG, ret);
	ret = mchp_core1588_timer_reg_read(timer, CORE1588_GCFG);

	return 0;
}

/* structure describing a PTP hardware clock */
static struct ptp_clock_info mchp_core1588_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "Microchip Core1588 PTP Timer",
	.max_adj	= mchp_core1588_MAX_ADJ,	/* Safe max adjutment for clock rate */
	.n_ext_ts	= N_EXT_TS,
	.pps		= N_EXT_PPS,
	.adjfine	= mchp_core1588_ptp_adjfine,
	.adjtime	= mchp_core1588_ptp_adjtime,
	.gettimex64	= mchp_core1588_ptp_gettime,
	.settime64	= mchp_core1588_ptp_settime,
	.enable		= mchp_core1588_ptp_enable,
};

/**
 * mchp_core1588_get_ptp_peer
 * @skb: the packet
 * @ptp_class: PTP event messages
 *
 * While core1588 can timestamp PTP packets.
 * UDP packets must be parsed to identify PTP packets.
 *
 * Note: Inspired from drivers/net/ethernet/ti/cpts.c
 */
static int mchp_core1588_get_ptp_peer(struct sk_buff *skb, int ptp_class)
{
	struct ptp_header *ptp_header;
	u8 msg_type;

	ptp_header = ptp_parse_header(skb, ptp_class);

	if (!ptp_header)
		return -1;

	msg_type = ptp_get_msgtype(ptp_header, ptp_class);

	return ((msg_type) & 0x2);
}

/**
 * mchp_core1588_rx_hwtstamp - utility function which checks for RX time stamp
 * @timer: the private struct
 * @skb: particular skb to send timestamp with
 *
 * Check for Peer/Event RX timestamp and convert it into the timecounter ns
 * value, then store that result into the shhwtstamps structure which
 * is passed up the network stack
 */
static void mchp_core1588_rx_hwtstamp(struct mchp_core1588_timer *timer, struct sk_buff *skb,
				      int peer_ev)
{
	struct skb_shared_hwtstamps *shhwtstamps = skb_hwtstamps(skb);
	struct timespec64 ts;

	if (peer_ev) {
		/* PTP Peer Event Frame packets */
		ts.tv_sec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_PEERRTSM);
		ts.tv_nsec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_PEERRTSL);
	} else {
		/* PTP Event Frame packets */
		ts.tv_sec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_RTSM);
		ts.tv_nsec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_RTSL);
	}

	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = ktime_set(ts.tv_sec, ts.tv_nsec);  //github dma
}

/**
 * mchp_core1588_ptp_rxstamp - utility function which reads RX time stamp
 * @timer: the private struct
 * @skb: the packet
 *
 * Identify the PTP packets from parsed UDP packets using mchp_core1588_get_ptp_peer
 * function and timestamp the packet and convert it into the timecounter ns
 * value, then store that result into the shhwtstamps structure which
 * is passed up the network stack using mchp_core1588_rx_hwtstamp function.
 */
static int mchp_core1588_ptp_rxstamp(struct mchp_core1588_timer *timer, struct sk_buff *skb)
{
	int class, peer;

	if (!timer)
		return 0;

	__skb_push(skb, ETH_HLEN);
	class = ptp_classify_raw(skb);
	__skb_pull(skb, ETH_HLEN);

	peer = mchp_core1588_get_ptp_peer(skb, class);
	if (peer < 0)
		return peer;

	mchp_core1588_rx_hwtstamp(timer, skb, peer);

	return 0;
}

/**
 * mchp_core1588_ptp_tx_hwtstamp - utility function which checks for TX time stamp
 * @timer: the private struct
 * @skb: the packet
 *
 * reads the TX timestamp and convert it into the timecounter ns
 * value, then store that result into the shhwtstamps structure which
 * is passed up the network stack.
 */
static void mchp_core1588_tx_hwtstamp(struct mchp_core1588_timer *timer, struct sk_buff *skb,
				      int peer_ev)
{
	struct skb_shared_hwtstamps *shhwtstamps = skb_hwtstamps(skb);
	struct timespec64 ts;

	/* PTP Peer Event Frame packets */
	if (peer_ev) {
		ts.tv_sec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_PEERTTSM);
		ts.tv_nsec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_PEERTTSL);

	/* PTP Event Frame packets */
	} else {
		ts.tv_sec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_TTSM);
		ts.tv_nsec = mchp_core1588_timer_reg_read(timer, MCHP_CORE1588_TTSL);
	}

	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = ktime_set(ts.tv_sec, ts.tv_nsec);
	skb_tstamp_tx(skb, skb_hwtstamps(skb));
}

/**
 * mchp_core1588_ptp_txstamp - utility function which checks for TX time stamp
 * @timer: the private struct
 * @skb: the packet
 *
 * Identify the PTP packets from parsed UDP packets using mchp_core1588_get_ptp_peer
 * function and timestamp the packet and convert it into the timecounter ns
 * value, then store that result into the shhwtstamps structure which
 * is passed up the network stack using mchp_core1588_tx_hwtstamp function.
 */
static int mchp_core1588_ptp_txstamp(struct mchp_core1588_timer *timer, struct sk_buff *skb)
{
	int class = ptp_classify_raw(skb);
	int peer;

	if (!timer)
		return 0;

	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
		peer = mchp_core1588_get_ptp_peer(skb, class);
		if (peer < 0)
			return peer;

		/* Timestamp this packet */
		mchp_core1588_tx_hwtstamp(timer, skb, peer);
	}

	return 0;
}

/**
 * mchp_core1588_ptp_init_timer - Calculate the clock period
 * @timer: the private struct
 * @tsu_rate:clock period rate
 *
 * Calculates the clock period and stores in tsu_incr structure
 *
 */
static void mchp_core1588_ptp_init_timer(struct mchp_core1588_timer *timer, unsigned int tsu_rate)
{
	u32 rem = 0;
	u64 adj;

	timer->tsu_incr.ns = div_u64_rem(NSEC_PER_SEC, tsu_rate, &rem);
	if (rem) {
		adj = rem;
		adj <<= CORE1588_SUBNSINCR_SIZE;
		timer->tsu_incr.sub_ns = div_u64(adj, tsu_rate);
	} else {
		timer->tsu_incr.sub_ns = 0;
	}
}

/**
 * mchp_core1588_ptp_init_timer - Initialize the free running timer clock period
 * @timer: the private struct
 *
 * Initialize the free running timer by reading current system time and set the
 * time with current time and set the calculated clock period.
 *
 */
static void mchp_core1588_init_tsu(struct mchp_core1588_timer *timer)
{
	struct timespec64 ts;

	/* 1. get current system time */
	ts = ns_to_timespec64(ktime_to_ns(ktime_get_real()));

	/* 2. set ptp timer */
	mchp_core1588_ptp_settime(&timer->ptp_clock_info, &ts);

	/* 3. set PTP timer increment value to BASE_INCREMENT */
	mchp_core1588_timer_incr_set(timer, &timer->tsu_incr);
}

/**
 * mchp_core1588_get_tsu_rate
 * @pdev: platform device structure
 *
 * Read the tsu_clk from device tree which is used to
 * calculate the clock period.
 *
 */
static unsigned int mchp_core1588_get_tsu_rate(struct platform_device *pdev)
{
	struct clk *tsu_clk;
	unsigned int tsu_rate;

	tsu_clk = devm_clk_get(&pdev->dev, "tsu");
	if (!IS_ERR(tsu_clk))
		tsu_rate = clk_get_rate(tsu_clk);
	else
		return -EOPNOTSUPP;

	return tsu_rate;
}

/**
 * mchp_core1588_init
 * @pdev: platform device structure
 * @timer: the private struct
 *
 * This function performs the required steps for enabling PTP
 * support and configures the Core1588 IP and intializes the
 * free running clock.
 *
 * This function is getting called from the Ethernet driver
 * for now the GEM driver probe function
 */
static void mchp_core1588_init(struct platform_device *pdev, struct mchp_core1588_timer *timer)
{
	unsigned int tsu_rate;
	int ret;

	ret = mchp_core1588_timer_reg_read(timer, CORE1588_GCFG);
	ret |= CORE1588_GCFG_EN;
	mchp_core1588_timer_reg_write(timer, CORE1588_GCFG, ret);
	ret = mchp_core1588_timer_reg_read(timer, CORE1588_GCFG);

	tsu_rate = mchp_core1588_get_tsu_rate(pdev);
	mchp_core1588_ptp_init_timer(timer, tsu_rate);

	mchp_core1588_init_tsu(timer);
}

/**
 * mchp_core1588_get_hwtst - get current hardware timestamping configuration
 * @timer: the private struct
 * @rq: ioctl data
 *
 * This function returns the current timestamping settings. Rather than
 * attempt to deconstruct registers to fill in the values, simply keep a copy
 * of the old settings around.
 */
static int mchp_core1588_get_hwtst(struct mchp_core1588_timer *timer, struct ifreq *rq)
{
	struct hwtstamp_config *tstamp_config;

	tstamp_config = &timer->tstamp_config;

	if (copy_to_user(rq->ifr_data, tstamp_config, sizeof(*tstamp_config)))
		return -EFAULT;
	else
		return 0;
}

/**
 * mchp_core1588_set_one_step_sync - set one/two step sync messages mode
 * @timer: the private struct
 * @enable: enable/disable
 *
 * This function is used to set one/two step sync messages mode.
 */
static void mchp_core1588_set_one_step_sync(struct mchp_core1588_timer *timer, bool enable)
{
	u32 reg;

	reg = mchp_core1588_timer_reg_read(timer, CORE1588_GCFG);

	if (enable)
		mchp_core1588_timer_reg_write(timer, CORE1588_GCFG, reg | CORE1588_GCFG_ONE_STEP);
	else
		mchp_core1588_timer_reg_write(timer, CORE1588_GCFG, reg &
					      ~(CORE1588_GCFG_ONE_STEP));
}

/**
 * mchp_core1588_set_hwtst - user entry point for timestamp mode
 * @adapter: pointer to adapter struct
 * @ifr: ioctl data
 * @cmd: command received from user-space
 *
 * Set hardware to requested mode. If unsupported, return an error with no
 * changes. Otherwise, store the mode for future reference.
 *
 */
static int mchp_core1588_set_hwtst(struct mchp_core1588_timer *timer, struct ifreq *ifr, int cmd)
{
	struct hwtstamp_config *tstamp_config;

	tstamp_config = &timer->tstamp_config;

	if (copy_from_user(tstamp_config, ifr->ifr_data,
			   sizeof(*tstamp_config)))
		return -EFAULT;

	switch (tstamp_config->tx_type) {
	case HWTSTAMP_TX_OFF:
		break;
	case HWTSTAMP_TX_ONESTEP_SYNC:
		mchp_core1588_set_one_step_sync(timer, true);
		break;
	case HWTSTAMP_TX_ON:
		mchp_core1588_set_one_step_sync(timer, false);
		break;
	default:
		return -ERANGE;
	}

	switch (tstamp_config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		tstamp_config->rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		tstamp_config->rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_ALL:
		tstamp_config->rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	default:
		tstamp_config->rx_filter = HWTSTAMP_FILTER_NONE;
		return -ERANGE;
	}

	if (copy_to_user(ifr->ifr_data, tstamp_config, sizeof(*tstamp_config)))
		return -EFAULT;
	else
		return 0;

	return 0;
}

static int mchp_core1588_timer_probe(struct platform_device *pdev)
{
	struct mchp_core1588_timer *timer;
	struct resource *res;

	timer = devm_kzalloc(&pdev->dev, sizeof(struct mchp_core1588_timer), GFP_KERNEL);
	if (!timer)
		return dev_err_probe(&pdev->dev, PTR_ERR(timer),
				"kzalloc failed\n");

	timer->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(timer->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(timer->base),
				     "could not get mem resource\n");

	spin_lock_init(&timer->reg_lock);

	timer->ptp_clock_info = mchp_core1588_ptp_clock_info;

	mchp_core1588_init(pdev, timer);

	timer->ptp_clock = ptp_clock_register(&timer->ptp_clock_info,
					      &pdev->dev);
	if (IS_ERR(timer->ptp_clock))
		return dev_err_probe(&pdev->dev, PTR_ERR(timer->ptp_clock),
				     "unable to register ptp clock\n");

	platform_set_drvdata(pdev, timer);

	timer->phc_index = ptp_clock_index(timer->ptp_clock);

	timer->ptp_rxstamp = mchp_core1588_ptp_rxstamp;
	timer->ptp_txstamp = mchp_core1588_ptp_txstamp;
	timer->get_hwtst = mchp_core1588_get_hwtst;
	timer->set_hwtst = mchp_core1588_set_hwtst;

	dev_info(&pdev->dev, "Microchip Core1588timer driver probed\n");

	return 0;
}

static void mchp_core1588_timer_remove(struct platform_device *pdev)
{
	struct mchp_core1588_timer *timer = platform_get_drvdata(pdev);

	ptp_clock_unregister(timer->ptp_clock);

	mchp_core1588_clear_timer(timer);

	dev_info(&pdev->dev, "ptp clock unregistered.\n");
}

static const struct of_device_id mchp_core1588_timer_of_match[] = {
	{ .compatible = "microchip,core1588-rtl-v3" },
	{}
};

MODULE_DEVICE_TABLE(of, mchp_core1588_timer_of_match);

static struct platform_driver mchp_core1588_timer_driver = {
	.probe		= mchp_core1588_timer_probe,
	.remove		= mchp_core1588_timer_remove,
	.driver		= {
		.name = MCHP_CORE1588_TIMER_NAME,
		.of_match_table = mchp_core1588_timer_of_match,
	},
};

module_platform_driver(mchp_core1588_timer_driver);

MODULE_DESCRIPTION("PTP Timer 1588 driver");
MODULE_AUTHOR("Praveen Kumar <praveen.kumar@microchip.com>");
MODULE_LICENSE("GPL");
