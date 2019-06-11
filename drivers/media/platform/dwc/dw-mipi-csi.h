/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI CSI-2 Host controller driver
 *
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 */

#ifndef _DW_MIPI_CSI_H__
#define _DW_MIPI_CSI_H__

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/phy/phy.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/reset.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/dwc/dw-mipi-csi-pltfrm.h>

/* Advanced features */
#define IPI_DT_OVERWRITE BIT(0)
#define DATA_TYPE_OVERWRITE(dt) (((dt) & GENMASK(5, 0)) << 8)
#define LINE_EVENT_SELECTION(n) ((n) << 16)

enum line_event {
	EVSELAUTO = 0,
	EVSELPROG = 1,
};

#define EN_VIDEO BIT(17)
#define EN_LINE_START BIT(18)
#define EN_NULL BIT(19)
#define EN_BLANKING BIT(20)
#define EN_EMBEDDED BIT(21)
#define IPI_SYNC_EVENT_MODE(n) ((n) << 24)

enum sync_event {
	SYNCEVFSN = 0,
	SYNCEVFS = 1,
};

/* DW MIPI CSI-2 register addresses*/

struct R_CSI2 {
	u16 VERSION;
	u16 N_LANES;
	u16 CTRL_RESETN;
	u16 INTERRUPT;
	u16 DATA_IDS_1;
	u16 DATA_IDS_2;
	u16 DATA_IDS_VC_1;
	u16 DATA_IDS_VC_2;
	u16 IPI_MODE;
	u16 IPI_VCID;
	u16 IPI_DATA_TYPE;
	u16 IPI_MEM_FLUSH;
	u16 IPI_HSA_TIME;
	u16 IPI_HBP_TIME;
	u16 IPI_HSD_TIME;
	u16 IPI_HLINE_TIME;
	u16 IPI_SOFTRSTN;
	u16 IPI_ADV_FEATURES;
	u16 IPI_VSA_LINES;
	u16 IPI_VBP_LINES;
	u16 IPI_VFP_LINES;
	u16 IPI_VACTIVE_LINES;
	u16 VC_EXTENSION;
	u16 INT_PHY_FATAL;
	u16 MASK_INT_PHY_FATAL;
	u16 FORCE_INT_PHY_FATAL;
	u16 INT_PKT_FATAL;
	u16 MASK_INT_PKT_FATAL;
	u16 FORCE_INT_PKT_FATAL;
	u16 INT_FRAME_FATAL;
	u16 MASK_INT_FRAME_FATAL;
	u16 FORCE_INT_FRAME_FATAL;
	u16 INT_PHY;
	u16 MASK_INT_PHY;
	u16 FORCE_INT_PHY;
	u16 INT_PKT;
	u16 MASK_INT_PKT;
	u16 FORCE_INT_PKT;
	u16 INT_LINE;
	u16 MASK_INT_LINE;
	u16 FORCE_INT_LINE;
	u16 INT_IPI;
	u16 MASK_INT_IPI;
	u16 FORCE_INT_IPI;
	u16 ST_BNDRY_FRAME_FATAL;
	u16 MSK_BNDRY_FRAME_FATAL;
	u16 FORCE_BNDRY_FRAME_FATAL;
	u16 ST_SEQ_FRAME_FATAL;
	u16 MSK_SEQ_FRAME_FATAL;
	u16 FORCE_SEQ_FRAME_FATAL;
	u16 ST_CRC_FRAME_FATAL;
	u16 MSK_CRC_FRAME_FATAL;
	u16 FORCE_CRC_FRAME_FATAL;
	u16 ST_PLD_CRC_FATAL;
	u16 MSK_PLD_CRC_FATAL;
	u16 FORCE_PLD_CRC_FATAL;
	u16 ST_DATA_ID;
	u16 MSK_DATA_ID;
	u16 FORCE_DATA_ID;
	u16 ST_ECC_CORRECT;
	u16 MSK_ECC_CORRECT;
	u16 FORCE_ECC_CORRECT;
};

/* Interrupt Masks */
struct interrupt_type {
	u32 PHY_FATAL;
	u32 PKT_FATAL;
	u32 FRAME_FATAL;
	u32 PHY;
	u32 PKT;
	u32 LINE;
	u32 IPI;
	u32 BNDRY_FRAME_FATAL;
	u32 SEQ_FRAME_FATAL;
	u32 CRC_FRAME_FATAL;
	u32 PLD_CRC_FATAL;
	u32 DATA_ID;
	u32 ECC_CORRECTED;
};

/* IPI Data Types */
enum data_type {
	CSI_2_YUV420_8 = 0x18,
	CSI_2_YUV420_10 = 0x19,
	CSI_2_YUV420_8_LEG = 0x1A,
	CSI_2_YUV420_8_SHIFT = 0x1C,
	CSI_2_YUV420_10_SHIFT = 0x1D,
	CSI_2_YUV422_8 = 0x1E,
	CSI_2_YUV422_10 = 0x1F,
	CSI_2_RGB444 = 0x20,
	CSI_2_RGB555 = 0x21,
	CSI_2_RGB565 = 0x22,
	CSI_2_RGB666 = 0x23,
	CSI_2_RGB888 = 0x24,
	CSI_2_RAW6 = 0x28,
	CSI_2_RAW7 = 0x29,
	CSI_2_RAW8 = 0x2A,
	CSI_2_RAW10 = 0x2B,
	CSI_2_RAW12 = 0x2C,
	CSI_2_RAW14 = 0x2D,
	CSI_2_RAW16 = 0x2E,
	CSI_2_RAW20 = 0x2F,
	USER_DEFINED_1 = 0x30,
	USER_DEFINED_2 = 0x31,
	USER_DEFINED_3 = 0x32,
	USER_DEFINED_4 = 0x33,
	USER_DEFINED_5 = 0x34,
	USER_DEFINED_6 = 0x35,
	USER_DEFINED_7 = 0x36,
	USER_DEFINED_8 = 0x37,
};

/* DWC MIPI CSI-2 output types */
enum output {
	IPI_OUT = 0,
	IDI_OUT = 1,
	BOTH_OUT = 2
};

/* IPI color components */
enum color_mode {
	COLOR48 = 0,
	COLOR16 = 1
};

/* IPI cut through */
enum cut_through {
	CTINACTIVE = 0,
	CTACTIVE = 1
};

/* IPI output types */
enum ipi_output {
	CAMERA_TIMING = 0,
	AUTO_TIMING = 1
};

/* Format template */
struct mipi_fmt {
	u32 mbus_code;
	u8 depth;
};

struct mipi_dt {
	u32 hex;
	char *name;
};

/* CSI specific configuration */
struct csi_data {
	u32 num_lanes;
	u32 dphy_freq;
	u32 pclk;
	u32 fps;
	u32 bpp;
	u32 output;
	u32 ipi_mode;
	u32 ipi_adv_features;
	u32 ipi_cut_through;
	u32 ipi_color_mode;
	u32 ipi_auto_flush;
	u32 virtual_ch;
	u32 hsa;
	u32 hbp;
	u32 hsd;
	u32 htotal;
	u32 vsa;
	u32 vbp;
	u32 vfp;
	u32 vactive;
};

/* Structure to embed device driver information */
struct dw_csi {
	struct v4l2_subdev sd;
	struct video_device vdev;
	struct v4l2_device v4l2_dev;
	struct device *dev;
	struct clk *perclk, *phyclk;
	struct media_pad pads[CSI_PADS_NUM];
	struct mipi_fmt *fmt;
	struct v4l2_mbus_framefmt format;
	void __iomem *base_address;
	void __iomem *demo;
	void __iomem *csc;
	int ctrl_irq_number;
	int demosaic_irq;
	struct csi_data hw;
	struct reset_control *rst;
	struct phy *phy;
	struct dw_csih_pdata *config;
	struct mutex lock; /* protect resources sharing */
	spinlock_t slock; /* interrupt handling lock */
	u8 ipi_dt;
	u8 index;
	u8 hw_version_major;
	u16 hw_version_minor;

	struct v4l2_async_notifier notifier;

	u32 remote_pad;

	struct v4l2_subdev *input_sd;
};

static inline struct dw_csi *sd_to_mipi_csi_dev(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct dw_csi, sd);
}

void dw_mipi_csi_reset(struct dw_csi *csi_dev);
int dw_mipi_csi_mask_irq_power_off(struct dw_csi *csi_dev);
int dw_mipi_csi_hw_stdby(struct dw_csi *csi_dev);
void dw_mipi_csi_set_ipi_fmt(struct dw_csi *csi_dev);
void dw_mipi_csi_start(struct dw_csi *csi_dev);
int dw_mipi_csi_irq_handler(struct dw_csi *csi_dev);
void dw_mipi_csi_get_version(struct dw_csi *csi_dev);
int dw_mipi_csi_specific_mappings(struct dw_csi *csi_dev);
void dw_mipi_csi_fill_timings(struct dw_csi *dev,
			      struct v4l2_subdev_format *fmt);
void dw_mipi_csi_dump(struct dw_csi *csi_dev);

#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
int dw_csi_create_capabilities_sysfs(struct platform_device *pdev);
#endif

static inline void dw_mipi_csi_write(struct dw_csi *dev,
				     u32 address, u32 data)
{
	writel(data, dev->base_address + address);
}

static inline u32 dw_mipi_csi_read(struct dw_csi *dev, u32 address)
{
	return readl(dev->base_address + address);
}

#endif /*_DW_MIPI_CSI_H__ */
