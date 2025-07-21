/* SPDX-License-Identifier: GPL-2.0 */
/*
 *Copyright (c) 2021 Microchip Technology Inc. All rights reserved.
 */

#ifndef _LINUX_MIV_IHC_H_
#define _LINUX_MIV_IHC_H_

#include <linux/mailbox_controller.h>
#include <linux/types.h>

/* Data structure for data-transfer protocol */
#define IHC_MAX_MESSAGE_SIZE 4U

enum {
	SBI_EXT_IHC_INIT,
	SBI_EXT_IHC_TX,
	SBI_EXT_IHC_RX,
	SBI_EXT_RPROC_STATE,
	SBI_EXT_RPROC_START,
	SBI_EXT_RPROC_STOP,
};

struct miv_ihc_msg {
	u32 msg[IHC_MAX_MESSAGE_SIZE];
};

struct miv_ihc {
	struct device *dev;
	void *buf_base;
	struct dma_pool *pool;
	struct mbox_controller controller;
	struct mbox_chan channel;
	int irq;
	dma_addr_t dma_addr;
	u32 remote_context_id;
};

#endif /* _LINUX_MIV_IHC_H_ */
