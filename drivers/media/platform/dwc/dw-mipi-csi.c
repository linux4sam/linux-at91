// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI CSI-2 Host controller driver
 * Core MIPI CSI-2 functions
 *
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 */

#include "dw-mipi-csi.h"

static struct R_CSI2 reg = {
	.VERSION = 0x00,
	.N_LANES = 0x04,
	.CTRL_RESETN = 0x08,
	.INTERRUPT = 0x0C,
	.DATA_IDS_1 = 0x10,
	.DATA_IDS_2 = 0x14,
	.IPI_MODE = 0x80,
	.IPI_VCID = 0x84,
	.IPI_DATA_TYPE = 0x88,
	.IPI_MEM_FLUSH = 0x8C,
	.IPI_HSA_TIME = 0x90,
	.IPI_HBP_TIME = 0x94,
	.IPI_HSD_TIME = 0x98,
	.IPI_HLINE_TIME = 0x9C,
	.IPI_SOFTRSTN = 0xA0,
	.IPI_ADV_FEATURES = 0xAC,
	.IPI_VSA_LINES = 0xB0,
	.IPI_VBP_LINES = 0xB4,
	.IPI_VFP_LINES = 0xB8,
	.IPI_VACTIVE_LINES = 0xBC,
	.INT_PHY_FATAL = 0xe0,
	.MASK_INT_PHY_FATAL = 0xe4,
	.FORCE_INT_PHY_FATAL = 0xe8,
	.INT_PKT_FATAL = 0xf0,
	.MASK_INT_PKT_FATAL = 0xf4,
	.FORCE_INT_PKT_FATAL = 0xf8,
	.INT_PHY = 0x110,
	.MASK_INT_PHY = 0x114,
	.FORCE_INT_PHY = 0x118,
	.INT_LINE = 0x130,
	.MASK_INT_LINE = 0x134,
	.FORCE_INT_LINE = 0x138,
	.INT_IPI = 0x140,
	.MASK_INT_IPI = 0x144,
	.FORCE_INT_IPI = 0x148,
};

struct interrupt_type csi_int = {
	.PHY_FATAL = BIT(0),
	.PKT_FATAL = BIT(1),
	.PHY = BIT(16),
};

#define dw_print(VAR) \
	dev_info(csi_dev->dev, "%s: 0x%x: %X\n", "#VAR#",\
	VAR, dw_mipi_csi_read(csi_dev, VAR))

void dw_mipi_csi_write_part(struct dw_csi *dev, u32 address, u32 data,
			    u8 shift, u8 width)
{
	u32 mask = (1 << width) - 1;
	u32 temp = dw_mipi_csi_read(dev, address);

	temp &= ~(mask << shift);
	temp |= (data & mask) << shift;
	dw_mipi_csi_write(dev, address, temp);
}

void dw_mipi_csi_reset(struct dw_csi *csi_dev)
{
	dw_mipi_csi_write(csi_dev, reg.CTRL_RESETN, 0);
	usleep_range(100, 200);
	dw_mipi_csi_write(csi_dev, reg.CTRL_RESETN, 1);
}

int dw_mipi_csi_mask_irq_power_off(struct dw_csi *csi_dev)
{
	if (csi_dev->hw_version_major == 1) {
		/* set only one lane (lane 0) as active (ON) */
		dw_mipi_csi_write(csi_dev, reg.N_LANES, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY_FATAL, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PKT_FATAL, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_LINE, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_IPI, 0);

		/* only for version 1.30 */
		if (csi_dev->hw_version_minor == 30)
			dw_mipi_csi_write(csi_dev,
					  reg.MASK_INT_FRAME_FATAL, 0);

		dw_mipi_csi_write(csi_dev, reg.CTRL_RESETN, 0);

		/* only for version 1.40 */
		if (csi_dev->hw_version_minor == 40) {
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_BNDRY_FRAME_FATAL, 0);
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_SEQ_FRAME_FATAL, 0);
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_CRC_FRAME_FATAL, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_PLD_CRC_FATAL, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_DATA_ID, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_ECC_CORRECT, 0);
		}
	}

	return 0;
}

int dw_mipi_csi_hw_stdby(struct dw_csi *csi_dev)
{
	if (csi_dev->hw_version_major == 1) {
		/* set only one lane (lane 0) as active (ON) */
		dw_mipi_csi_reset(csi_dev);
		dw_mipi_csi_write(csi_dev, reg.N_LANES, 0);
		phy_init(csi_dev->phy);

		/* only for version 1.30 */
		if (csi_dev->hw_version_minor == 30)
			dw_mipi_csi_write(csi_dev,
					  reg.MASK_INT_FRAME_FATAL,
					  GENMASK(31, 0));

		/* common */
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY_FATAL,
				  GENMASK(8, 0));
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PKT_FATAL,
				  GENMASK(1, 0));
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY, GENMASK(23, 0));
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_LINE, GENMASK(23, 0));
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_IPI, GENMASK(5, 0));

		/* only for version 1.40 */
		if (csi_dev->hw_version_minor == 40) {
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_BNDRY_FRAME_FATAL,
					  GENMASK(31, 0));
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_SEQ_FRAME_FATAL,
					  GENMASK(31, 0));
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_CRC_FRAME_FATAL,
					  GENMASK(31, 0));
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_PLD_CRC_FATAL,
					  GENMASK(31, 0));
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_DATA_ID, GENMASK(31, 0));
			dw_mipi_csi_write(csi_dev,
					  reg.MSK_ECC_CORRECT, GENMASK(31, 0));
		}
	}
	return 0;
}

void dw_mipi_csi_set_ipi_fmt(struct dw_csi *csi_dev)
{
	struct device *dev = csi_dev->dev;

	if (csi_dev->ipi_dt) {
		dw_mipi_csi_write(csi_dev, reg.IPI_DATA_TYPE, csi_dev->ipi_dt);
		switch (csi_dev->ipi_dt) {
		case CSI_2_YUV420_8:
		case CSI_2_YUV420_8_LEG:
		case CSI_2_YUV420_8_SHIFT:
		break;
		case CSI_2_YUV420_10:
		case CSI_2_YUV420_10_SHIFT:
		break;
		}
	} else {
		switch (csi_dev->fmt->mbus_code) {
		/* RGB 666 */
		case MEDIA_BUS_FMT_RGB666_1X18:
		csi_dev->ipi_dt =  CSI_2_RGB666;
		break;
		/* RGB 565 */
		case MEDIA_BUS_FMT_RGB565_2X8_BE:
		case MEDIA_BUS_FMT_RGB565_2X8_LE:
		csi_dev->ipi_dt = CSI_2_RGB565;
		break;
		/* RGB 555 */
		case MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE:
		case MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE:
		csi_dev->ipi_dt = CSI_2_RGB555;
		break;
		/* RGB 444 */
		case MEDIA_BUS_FMT_RGB444_2X8_PADHI_BE:
		case MEDIA_BUS_FMT_RGB444_2X8_PADHI_LE:
		csi_dev->ipi_dt = CSI_2_RGB444;
		break;
		/* RGB 888 */
		break;
		case MEDIA_BUS_FMT_RGB888_2X12_LE:
		case MEDIA_BUS_FMT_RGB888_2X12_BE:
		csi_dev->ipi_dt = CSI_2_RGB888;
		break;
		/* RAW 10 */
		case MEDIA_BUS_FMT_SBGGR10_1X10:
		case MEDIA_BUS_FMT_SRGGB10_1X10:
		case MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE:
		csi_dev->ipi_dt = CSI_2_RAW10;
		break;
		/* RAW 12 */
		case MEDIA_BUS_FMT_SBGGR12_1X12:
		csi_dev->ipi_dt = CSI_2_RAW12;
		break;
		/* RAW 14 */
		case MEDIA_BUS_FMT_SBGGR14_1X14:
		csi_dev->ipi_dt = CSI_2_RAW14;
		break;
		/* RAW 16 */
		case MEDIA_BUS_FMT_SBGGR16_1X16:
		csi_dev->ipi_dt = CSI_2_RAW16;
		break;
		/* RAW 8 */
		case MEDIA_BUS_FMT_SBGGR8_1X8:
		csi_dev->ipi_dt = CSI_2_RAW8;
		break;
		/* YUV 422 8-bit */
		case MEDIA_BUS_FMT_YVYU8_2X8:
		csi_dev->ipi_dt = CSI_2_RAW8;
		break;
		/* YUV 422 10-bit */
		case MEDIA_BUS_FMT_VYUY8_1X16:
		csi_dev->ipi_dt = CSI_2_YUV422_8;
		break;
		/* YUV 420 8-bit LEGACY */
		case MEDIA_BUS_FMT_Y8_1X8:
		csi_dev->ipi_dt = CSI_2_RAW8;
		break;
		/* YUV 420 10-bit */
		case MEDIA_BUS_FMT_Y10_1X10:
		csi_dev->ipi_dt = CSI_2_RAW8;
		break;
		default:
		break;
		}
		dw_mipi_csi_write(csi_dev, reg.DATA_IDS_1, csi_dev->ipi_dt);
	}
	dev_dbg(dev, "Selected IPI Data Type 0x%X\n", csi_dev->ipi_dt);
}

void dw_mipi_csi_fill_timings(struct dw_csi *dev,
			      struct v4l2_subdev_format *fmt)
{
	/* expected values */
	dev->hw.virtual_ch = 0;
	dev->hw.ipi_color_mode = COLOR48;
	dev->hw.ipi_auto_flush = 1;
	dev->hw.ipi_mode = CAMERA_TIMING;
	dev->hw.ipi_cut_through = CTINACTIVE;
	dev->hw.ipi_adv_features = LINE_EVENT_SELECTION(EVSELAUTO);
	dev->hw.htotal = fmt->format.width + dev->hw.hsa +
			 dev->hw.hbp + dev->hw.hsd;
	dev->hw.vactive = fmt->format.height;
	dev->hw.output = 2;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		dev_dbg(dev->dev, "*********** timings *********\n");
		dev_dbg(dev->dev, "Horizontal Sync Active: %d\n", dev->hw.hsa);
		dev_dbg(dev->dev, "Horizontal Back Porch: %d\n", dev->hw.hbp);
		dev_dbg(dev->dev, "Horizontal Width: %d\n", fmt->format.width);
		dev_dbg(dev->dev, "Horizontal Total: %d\n", dev->hw.htotal);
		dev_dbg(dev->dev, "Vertical Sync Active: %d\n", dev->hw.vsa);
		dev_dbg(dev->dev, "Vertical Back Porch: %d\n", dev->hw.vbp);
		dev_dbg(dev->dev, "Vertical Front Porch: %d\n", dev->hw.vfp);
		dev_dbg(dev->dev, "Vertical Active: %d\n", dev->hw.vactive);
	}
}

void dw_mipi_csi_start(struct dw_csi *csi_dev)
{
	struct device *dev = csi_dev->dev;

	dw_mipi_csi_write(csi_dev, reg.N_LANES, (csi_dev->hw.num_lanes - 1));
	dev_dbg(dev, "number of lanes: %d\n", csi_dev->hw.num_lanes);

	/* IPI Related Configuration */
	if (csi_dev->hw.output == IPI_OUT || csi_dev->hw.output == BOTH_OUT) {
		if (csi_dev->hw_version_major >= 1) {
			if (csi_dev->hw_version_minor >= 20)
				dw_mipi_csi_write(csi_dev,
						  reg.IPI_ADV_FEATURES,
						  csi_dev->hw.ipi_adv_features);
			if (csi_dev->hw_version_minor >= 30)
				dw_mipi_csi_write(csi_dev,
						  reg.IPI_SOFTRSTN, 0x1);
		}
		/*  address | data, | shift | width */
		dw_mipi_csi_write_part(csi_dev, reg.IPI_MODE, 1, 24, 1);
		dw_mipi_csi_write_part(csi_dev,
				       reg.IPI_MODE,
				       csi_dev->hw.ipi_mode,
				       0, 1);
		if (csi_dev->hw.ipi_mode == CAMERA_TIMING) {
			dw_mipi_csi_write(csi_dev,
					  reg.IPI_ADV_FEATURES,
					  LINE_EVENT_SELECTION(EVSELPROG) |
					  EN_VIDEO |
					  EN_LINE_START |
					  EN_NULL |
					  EN_BLANKING |
					  EN_EMBEDDED);
		}
		dw_mipi_csi_write_part(csi_dev,
				       reg.IPI_MODE,
				       csi_dev->hw.ipi_color_mode,
				       8, 1);
		dw_mipi_csi_write_part(csi_dev,
				       reg.IPI_MODE,
				       csi_dev->hw.ipi_cut_through,
				       16, 1);
		dw_mipi_csi_write_part(csi_dev,
				       reg.IPI_VCID,
				       csi_dev->hw.virtual_ch,
				       0, 2);
		dw_mipi_csi_write_part(csi_dev,
				       reg.IPI_MEM_FLUSH,
				       csi_dev->hw.ipi_auto_flush,
				       8, 1);

		dev_vdbg(dev, "*********** config *********\n");
		dev_vdbg(dev, "IPI enable: %s\n",
			 csi_dev->hw.output ? "YES" : "NO");
		dev_vdbg(dev, "video mode transmission type: %s timming\n",
			 csi_dev->hw.ipi_mode ? "controller" : "camera");
		dev_vdbg(dev, "Color Mode: %s\n",
			 csi_dev->hw.ipi_color_mode ? "16 bits" : "48 bits");
		dev_vdbg(dev, "Cut Through Mode: %s\n",
			 csi_dev->hw.ipi_cut_through ? "enable" : "disable");
		dev_vdbg(dev, "Virtual Channel: %d\n",
			 csi_dev->hw.virtual_ch);
		dev_vdbg(dev, "Auto-flush: %d\n",
			 csi_dev->hw.ipi_auto_flush);
		dw_mipi_csi_write(csi_dev, reg.IPI_SOFTRSTN, 1);

		if (csi_dev->hw.ipi_mode == AUTO_TIMING)
			phy_power_on(csi_dev->phy);

		dw_mipi_csi_write(csi_dev,
				  reg.IPI_HSA_TIME, csi_dev->hw.hsa);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_HBP_TIME, csi_dev->hw.hbp);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_HSD_TIME, csi_dev->hw.hsd);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_HLINE_TIME, csi_dev->hw.htotal);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_VSA_LINES, csi_dev->hw.vsa);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_VBP_LINES, csi_dev->hw.vbp);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_VFP_LINES, csi_dev->hw.vfp);
		dw_mipi_csi_write(csi_dev,
				  reg.IPI_VACTIVE_LINES, csi_dev->hw.vactive);
	}
	phy_power_on(csi_dev->phy);
}

int dw_mipi_csi_irq_handler(struct dw_csi *csi_dev)
{
	struct device *dev = csi_dev->dev;
	u32 global_int_status, i_sts;
	unsigned long flags;

	spin_lock_irqsave(&csi_dev->slock, flags);
	global_int_status = dw_mipi_csi_read(csi_dev, reg.INTERRUPT);

	if (global_int_status & csi_int.PHY_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PHY_FATAL);
		dev_err_ratelimited(dev, "int %08X: PHY FATAL: %08X\n",
				    reg.INT_PHY_FATAL, i_sts);
	}

	if (global_int_status & csi_int.PKT_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PKT_FATAL);
		dev_err_ratelimited(dev, "int %08X: PKT FATAL: %08X\n",
				    reg.INT_PKT_FATAL, i_sts);
	}

	if (global_int_status & csi_int.FRAME_FATAL &&
	    csi_dev->hw_version_major == 1 &&
	    csi_dev->hw_version_minor == 30) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_FRAME_FATAL);
		dev_err_ratelimited(dev, "int %08X: FRAME FATAL: %08X\n",
				    reg.INT_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.PHY) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PHY);
		dev_err_ratelimited(dev, "int %08X: PHY: %08X\n",
				    reg.INT_PHY, i_sts);
	}

	if (global_int_status & csi_int.PKT &&
	    csi_dev->hw_version_major == 1 &&
	    csi_dev->hw_version_minor <= 30) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PKT);
		dev_err_ratelimited(dev, "int %08X: PKT: %08X\n",
				    reg.INT_PKT, i_sts);
	}

	if (global_int_status & csi_int.LINE) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_LINE);
		dev_err_ratelimited(dev, "int %08X: LINE: %08X\n",
				    reg.INT_LINE, i_sts);
	}

	if (global_int_status & csi_int.IPI) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_IPI);
		dev_err_ratelimited(dev, "int %08X: IPI: %08X\n",
				    reg.INT_IPI, i_sts);
	}

	if (global_int_status & csi_int.BNDRY_FRAME_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_BNDRY_FRAME_FATAL);
		dev_err_ratelimited(dev,
				    "int %08X: ST_BNDRY_FRAME_FATAL: %08X\n",
				    reg.ST_BNDRY_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.SEQ_FRAME_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_SEQ_FRAME_FATAL);
		dev_err_ratelimited(dev,
				    "int %08X: ST_SEQ_FRAME_FATAL: %08X\n",
				    reg.ST_SEQ_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.CRC_FRAME_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_CRC_FRAME_FATAL);
		dev_err_ratelimited(dev,
				    "int %08X: ST_CRC_FRAME_FATAL: %08X\n",
				    reg.ST_CRC_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.PLD_CRC_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_PLD_CRC_FATAL);
		dev_err_ratelimited(dev,
				    "int %08X: ST_PLD_CRC_FATAL: %08X\n",
				    reg.ST_PLD_CRC_FATAL, i_sts);
	}

	if (global_int_status & csi_int.DATA_ID) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_DATA_ID);
		dev_err_ratelimited(dev, "int %08X: ST_DATA_ID: %08X\n",
				    reg.ST_DATA_ID, i_sts);
	}

	if (global_int_status & csi_int.ECC_CORRECTED) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_ECC_CORRECT);
		dev_err_ratelimited(dev, "int %08X: ST_ECC_CORRECT: %08X\n",
				    reg.ST_ECC_CORRECT, i_sts);
	}

	spin_unlock_irqrestore(&csi_dev->slock, flags);

	return 1;
}

void dw_mipi_csi_get_version(struct dw_csi *csi_dev)
{
	u32 hw_version;

	hw_version = dw_mipi_csi_read(csi_dev, reg.VERSION);
	csi_dev->hw_version_major = (u8)((hw_version >> 24) - '0');
	csi_dev->hw_version_minor = (u8)((hw_version >> 16) - '0');
	csi_dev->hw_version_minor = csi_dev->hw_version_minor * 10;
	csi_dev->hw_version_minor += (u8)((hw_version >> 8) - '0');
}

int dw_mipi_csi_specific_mappings(struct dw_csi *csi_dev)
{
	struct device *dev = csi_dev->dev;

	if (csi_dev->hw_version_major == 1) {
		if (csi_dev->hw_version_minor == 30) {
			/*
			 * Hardware registers that were
			 * exclusive to version < 1.40
			 */
			reg.INT_FRAME_FATAL = 0x100;
			reg.MASK_INT_FRAME_FATAL = 0x104;
			reg.FORCE_INT_FRAME_FATAL = 0x108;
			reg.INT_PKT = 0x120;
			reg.MASK_INT_PKT = 0x124;
			reg.FORCE_INT_PKT = 0x128;

			/* interrupt source present until this release */
			csi_int.PKT = BIT(17);
			csi_int.LINE = BIT(18);
			csi_int.IPI = BIT(19);
			csi_int.FRAME_FATAL = BIT(2);

		} else if (csi_dev->hw_version_minor == 40) {
			/*
			 * HW registers that were added
			 * to version 1.40
			 */
			reg.ST_BNDRY_FRAME_FATAL = 0x280;
			reg.MSK_BNDRY_FRAME_FATAL = 0x284;
			reg.FORCE_BNDRY_FRAME_FATAL = 0x288;
			reg.ST_SEQ_FRAME_FATAL = 0x290;
			reg.MSK_SEQ_FRAME_FATAL	= 0x294;
			reg.FORCE_SEQ_FRAME_FATAL = 0x298;
			reg.ST_CRC_FRAME_FATAL = 0x2a0;
			reg.MSK_CRC_FRAME_FATAL	= 0x2a4;
			reg.FORCE_CRC_FRAME_FATAL = 0x2a8;
			reg.ST_PLD_CRC_FATAL = 0x2b0;
			reg.MSK_PLD_CRC_FATAL = 0x2b4;
			reg.FORCE_PLD_CRC_FATAL = 0x2b8;
			reg.ST_DATA_ID = 0x2c0;
			reg.MSK_DATA_ID = 0x2c4;
			reg.FORCE_DATA_ID = 0x2c8;
			reg.ST_ECC_CORRECT = 0x2d0;
			reg.MSK_ECC_CORRECT = 0x2d4;
			reg.FORCE_ECC_CORRECT = 0x2d8;
			reg.DATA_IDS_VC_1 = 0x0;
			reg.DATA_IDS_VC_2 = 0x0;
			reg.VC_EXTENSION = 0x0;

			/* interrupts map were changed */
			csi_int.LINE = BIT(17);
			csi_int.IPI = BIT(18);
			csi_int.BNDRY_FRAME_FATAL = BIT(2);
			csi_int.SEQ_FRAME_FATAL	= BIT(3);
			csi_int.CRC_FRAME_FATAL = BIT(4);
			csi_int.PLD_CRC_FATAL = BIT(5);
			csi_int.DATA_ID = BIT(6);
			csi_int.ECC_CORRECTED = BIT(7);

		} else {
			dev_info(dev, "Version minor not supported.");
		}
	} else {
		dev_info(dev, "Version major not supported.");
	}
	return 0;
}

void dw_mipi_csi_dump(struct dw_csi *csi_dev)
{
	dw_print(reg.VERSION);
	dw_print(reg.N_LANES);
	dw_print(reg.CTRL_RESETN);
	dw_print(reg.INTERRUPT);
	dw_print(reg.DATA_IDS_1);
	dw_print(reg.DATA_IDS_2);
	dw_print(reg.IPI_MODE);
	dw_print(reg.IPI_VCID);
	dw_print(reg.IPI_DATA_TYPE);
	dw_print(reg.IPI_MEM_FLUSH);
	dw_print(reg.IPI_HSA_TIME);
	dw_print(reg.IPI_HBP_TIME);
	dw_print(reg.IPI_HSD_TIME);
	dw_print(reg.IPI_HLINE_TIME);
	dw_print(reg.IPI_SOFTRSTN);
	dw_print(reg.IPI_ADV_FEATURES);
	dw_print(reg.IPI_VSA_LINES);
	dw_print(reg.IPI_VBP_LINES);
	dw_print(reg.IPI_VFP_LINES);
	dw_print(reg.IPI_VACTIVE_LINES);
	dw_print(reg.IPI_DATA_TYPE);
	dw_print(reg.VERSION);
	dw_print(reg.IPI_ADV_FEATURES);
}
