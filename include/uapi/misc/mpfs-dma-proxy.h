/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Microchip PolarFire SoC DMA proxy userspace header
 *
 * Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Shravan Chippa <shravan.chippa@microchip.com>
 */

#ifndef _MPFS_DMA_PROXY_H
#define _MPFS_DMA_PROXY_H

#include <linux/types.h>

#define MPFS_DMA_PROXY_IOC_MAGIC	'u'

#define MPFS_DMA_PROXY_FINISH_XFER	_IOW(MPFS_DMA_PROXY_IOC_MAGIC, \
					     '1', enum mpfs_dma_proxy_status*)
#define MPFS_DMA_PROXY_START_XFER	_IOW(MPFS_DMA_PROXY_IOC_MAGIC, \
					     '2', struct mpfs_dma_proxy_channel_config*)
#define MPFS_DMA_PROXY_IOC_MAXNR	2

/**
 * struct mpfs_dma_proxy_channel_config  - dma config info
 * @src:	source dma phy address
 * @dst:	destination dma phy address
 * @length:	size of xfer
 */
struct mpfs_dma_proxy_channel_config {
	__u64 src;
	__u64 dst;
	__kernel_size_t length;
};

enum mpfs_dma_proxy_status {
	PROXY_SUCCESS = 0,
	PROXY_BUSY,
	PROXY_TIMEOUT,
	PROXY_ERROR,
};

#endif	/* _MPFS_DMA_PROXY_H */
