/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Microchip ISC statistics userspace API
 *
 * Copyright (C) 2025 Microchip Technology, Inc.
 */

#ifndef _UAPI_LINUX_MEDIA_MICROCHIP_ISC_CONFIG_H
#define _UAPI_LINUX_MEDIA_MICROCHIP_ISC_CONFIG_H

#include <linux/types.h>

/* Histogram has 512 bins per Bayer channel: GR, R, GB, B. */
#define MCHP_ISC_HIST_BINS		512
#define MCHP_ISC_HIST_BAYER_CHAN	4

/* bayer_pattern values for struct mchp_isc_stat_buffer */
#define MCHP_ISC_BAYCFG_GRGR		0
#define MCHP_ISC_BAYCFG_RGRG		1
#define MCHP_ISC_BAYCFG_GBGB		2
#define MCHP_ISC_BAYCFG_BGBG		3

/**
 * enum mchp_isc_stat_meas_type - Statistics payload type flags
 * @MCHP_ISC_STAT_MEAS_HIST: Histogram fields are valid
 */
enum mchp_isc_stat_meas_type {
	MCHP_ISC_STAT_MEAS_HIST = 1U << 0,
};

/**
 * struct mchp_isc_hist_channel - Per-channel histogram statistics
 * @hist_bins: 512-bin histogram data
 * @hist_min: Effective minimum histogram bin
 * @hist_max: Effective maximum histogram bin
 * @total_pixels: Number of pixels contributing to histogram
 * @capture_frame: Frame sequence number when this channel's histogram
 *                 was measured (snapshotted at HISDONE IRQ time)
 * @capture_ts: CLOCK_MONOTONIC timestamp (ktime_get_ns()) at HISDONE IRQ
 *              for this channel; correlates directly with V4L2 capture buffer
 *              timestamps which also use CLOCK_MONOTONIC
 */
struct mchp_isc_hist_channel {
	__u32 hist_bins[MCHP_ISC_HIST_BINS];
	__u32 hist_min;
	__u32 hist_max;
	__u32 total_pixels;
	__u32 capture_frame;
	__u64 capture_ts;
};

/**
 * struct mchp_isc_stat_buffer - V4L2_META_FMT_ISC_STAT_3A payload
 * @timestamp_ns: Frame timestamp in nanoseconds
 * @frame_number: Sequential frame counter
 * @meas_type: Bitmask of &enum mchp_isc_stat_meas_type values
 * @hist: Histogram data for GR, R, GB and B channels, in this order
 * @valid_channels: Bitmask of valid channels in @hist
 * @bayer_pattern: Active CFA pattern (MCHP_ISC_BAYCFG_*)
 * @reserved: Reserved for future extensions, must be zero
 */
struct mchp_isc_stat_buffer {
	__u64 timestamp_ns;
	__u32 frame_number;
	__u32 meas_type;
	struct mchp_isc_hist_channel hist[MCHP_ISC_HIST_BAYER_CHAN];
	__u8 valid_channels;
	__u8 bayer_pattern;
	__u16 reserved[3];
};

#endif /* _UAPI_LINUX_MEDIA_MICROCHIP_ISC_CONFIG_H */
