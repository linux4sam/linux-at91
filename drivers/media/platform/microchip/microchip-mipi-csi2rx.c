// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Microchip MIPI CSI-2 Rx
 *
 * Copyright (C) 2023 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/v4l2-subdev.h>
#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/mipi-csi2.h>
#include "microchip-common.h"

#define MIPI_CSI_MEDIA_PADS		2
#define MIPI_CSI_DEFAULT_WIDTH		1920
#define MIPI_CSI_DEFAULT_HEIGHT		1080

#define MIPI_CSI_MEDIA_PADS		2

#define MIPI_CSI_IP_VER			0x00

#define MIPI_CSI_CTRL			0x04
#define MIPI_CSI_CTRL_START		BIT(0)
#define MIPI_CSI_CTRL_RESET		BIT(1)

#define MIPI_CSI_LANE_CONFIG		0x08
#define MIPI_CSI_LANE_CONFIG_VAL(x)	((x) >> 4)

#define MIPI_CSI_DATA_WIDTH		0x0C
#define MIPI_CSI_NO_OF_PIXELS		0x10
#define MIPI_CSI_NO_OF_VC		0x14
#define MIPI_CSI_INPUT_DATA_INVERT	0x18
#define MIPI_CSI_FIFO_SIZE		0x1C
#define MIPI_CSI_FRAME_RESOLUTION	0x20
#define MIPI_CSI_FRAME_WIDTH_MASK	GENMASK(31, 16)
#define MIPI_CSI_FRAME_HIGHT_MASK	GENMASK(15, 0)

#define MIPI_CSI_GLOBAL_INTERRUPT	0x24
#define MIPI_CSI_GLOBAL_IRQ_EN		BIT(0)

#define MIPI_CSI_INTERRUPT_STATUS	0x28
#define MIPI_CSI_ISR_FR			BIT(0)
#define MIPI_CSI_ISR_ACTIVE_LANE_ERR	BIT(1)
#define MIPI_CSI_ISR_SOT_ERR		BIT(2)
#define MIPI_CSI_ISR_SKEW_ERR		BIT(3)
#define MIPI_CSI_ISR_ECC_ERR		BIT(4)
#define MIPI_CSI_ISR_CRC_ERR		BIT(5)
#define MIPI_CSI_ISR_DATAID_ERR		BIT(6)

#define MIPI_CSI_INTERRUPT_EN		0x2C
#define MIPI_CSI_INTERRUPT_EN_MASK	0x7F

#define MIPI_CSI_CLK_STATUS		0x30
#define MIPI_CSI_CLK_STATUS_STOPED	0x00
#define MIPI_CSI_CLK_STATUS_RUNNING	0x01

#define MIPI_CSI_DATA_LANE_STATUS	0x34
#define MIPI_CSI_DATA_LANE_STATUS_CLR	0x54
#define MIPI_CSI_WORD_COUNT		0x58
#define MIPI_CSI_CAM_DATA_TYPE		0x5C
#define MIPI_CSI_CAM_LANES_CONFIG	0x60

/**
 * struct mipi_csi2rx_event - Event log structure
 * @mask: Event mask
 * @name: Name of the event
 */
struct mipi_csi2rx_event {
	u32 mask;
	const char *name;
};

static const struct mipi_csi2rx_event mipi_csi2rx_events[] = {
	{ MIPI_CSI_ISR_FR, "Frame Received" },
	{ MIPI_CSI_ISR_ACTIVE_LANE_ERR, "Active lane mismatch Error" },
	{ MIPI_CSI_ISR_SOT_ERR, "Start-of-Transmission Error" },
	{ MIPI_CSI_ISR_SKEW_ERR, "IOD training failure error" },
	{ MIPI_CSI_ISR_ECC_ERR, "ECC Error" },
	{ MIPI_CSI_ISR_CRC_ERR, "CRC Error" },
	{ MIPI_CSI_ISR_DATAID_ERR, "Data Id Error" },
};

#define MIPI_CSI_NUM_EVENTS		ARRAY_SIZE(mipi_csi2rx_events)
#define MIPI_CSI2_MAX_RAW_FORMATS	4

/*
 * This table provides a mapping between CSI-2 Data type
 * and media bus formats
 */
static const u32 mipi_csi2dt_mbus_lut[][2] = {
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SRGGB8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SBGGR8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SGBRG8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SGRBG8_1X8 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SRGGB10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SBGGR10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SGBRG10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SGRBG10_1X10 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SRGGB12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SBGGR12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SGBRG12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SGRBG12_1X12 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SRGGB16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SBGGR16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SGBRG16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SGRBG16_1X16 },
	{ MIPI_CSI2_DT_RGB888, MEDIA_BUS_FMT_RGB888_1X24 },
	{ MIPI_CSI2_DT_RAW20, 0 },
};

/**
 * struct mipi_csi2rx_state - CSI-2 Rx Subsystem device structure
 * @subdev: The v4l2 subdev structure
 * @format: Active V4L2 formats on each pad
 * @default_format: Default V4L2 format
 * @events: counter for events
 * @dev: Platform structure
 * @rsubdev: Remote subdev connected to sink pad
 * @clks: array of clocks
 * @iomem: Base address of subsystem
 * @max_num_lanes: Maximum number of lanes present
 * @datatype: Data type filter
 * @lock: mutex for accessing this structure
 * @pads: media pads
 * @streaming: Flag for storing streaming state
 * @csi_fixed_out_raw8: If out put format is fixed to raw8
 *
 * This structure contains the device driver related parameters
 */
struct mipi_csi2rx_state {
	struct v4l2_subdev subdev;
	struct v4l2_mbus_framefmt format[2];
	struct v4l2_mbus_framefmt default_format[2];
	struct device *dev;
	struct v4l2_subdev *rsubdev;
	struct clk_bulk_data *clks;
	void __iomem *iomem;
	u32 max_num_lanes;
	u32 datatype;
	u32 events[MIPI_CSI_NUM_EVENTS];
	/* used to protect access to this struct */
	struct mutex lock;
	struct media_pad pads[MIPI_CSI_MEDIA_PADS];
	bool streaming;
	bool csi_fixed_out_raw8;
};

static const struct clk_bulk_data mipi_csi2rx_clks[] = {
	{ .id = "axi" },
	{ .id = "video" },
};

static inline struct mipi_csi2rx_state *
to_csi2rxstate(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mipi_csi2rx_state, subdev);
}

/*
 * Register related operations
 */
static inline u32 mipi_csi2rx_read(struct mipi_csi2rx_state *csi2rx, u32 addr)
{
	return ioread32(csi2rx->iomem + addr);
}

static inline void mipi_csi2rx_write(struct mipi_csi2rx_state *csi2rx, u32 addr,
				     u32 value)
{
	iowrite32(value, csi2rx->iomem + addr);
}

static inline void mipi_csi2rx_clr(struct mipi_csi2rx_state *csi2rx, u32 addr,
				   u32 clr)
{
	mipi_csi2rx_write(csi2rx, addr,
			  mipi_csi2rx_read(csi2rx, addr) & ~clr);
}

static inline void mipi_csi2rx_set(struct mipi_csi2rx_state *csi2rx, u32 addr,
				   u32 set)
{
	mipi_csi2rx_write(csi2rx, addr, mipi_csi2rx_read(csi2rx, addr) | set);
}

static u32 mipi_csi2rx_get_nth_mbus(u32 dt, u32 n)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mipi_csi2dt_mbus_lut); i++) {
		if (mipi_csi2dt_mbus_lut[i][0] == dt) {
			if (n-- == 0)
				return mipi_csi2dt_mbus_lut[i][1];
		}
	}

	return 0;
}

static u32 mipi_csi2rx_get_dt(u32 mbus)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mipi_csi2dt_mbus_lut); i++) {
		if (mipi_csi2dt_mbus_lut[i][1] == mbus)
			return mipi_csi2dt_mbus_lut[i][0];
	}

	return 0;
}

static int mipi_csi2rx_soft_reset(struct mipi_csi2rx_state *state)
{
	mipi_csi2rx_set(state, MIPI_CSI_CTRL, MIPI_CSI_CTRL_RESET);

	return 0;
}

static void mipi_csi2rx_reset_event_counters(struct mipi_csi2rx_state *state)
{
	unsigned int i;

	for (i = 0; i < MIPI_CSI_NUM_EVENTS; i++)
		state->events[i] = 0;
}

static void mipi_csi2rx_log_counters(struct mipi_csi2rx_state *state)
{
	struct device *dev = state->dev;
	unsigned int i;

	for (i = 0; i < MIPI_CSI_NUM_EVENTS; i++) {
		if (state->events[i] > 0) {
			dev_info(dev, "%s events: %d\n",
				 mipi_csi2rx_events[i].name,
				 state->events[i]);
		}
	}
}

static int mipi_csi2rx_log_status(struct v4l2_subdev *sd)
{
	struct mipi_csi2rx_state *csi2rx = to_csi2rxstate(sd);
	struct device *dev = csi2rx->dev;
	u32 data;

	mutex_lock(&csi2rx->lock);

	mipi_csi2rx_log_counters(csi2rx);

	dev_info(dev, "***** Core Status *****\n");
	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_CTRL);
	dev_info(dev, "Core Status is %s\n",
		 data & MIPI_CSI_CTRL_START ? "enable" : "disable");
	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_GLOBAL_INTERRUPT);
	dev_info(dev, "MIPI_CSI_GLOBAL_IRQ_EN is %s\n",
		 data & MIPI_CSI_GLOBAL_IRQ_EN ? "enable" : "disable");

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_INTERRUPT_EN);
	dev_info(dev, "MIPI_CSI_INTERRUPT_EN value = %x\n", data);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_LANE_CONFIG);
	dev_info(dev, "No of Lanes configured = %x\n", (data & 0xF));
	dev_info(dev, "Max Lanes supported = %x\n", (data >> 4));

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_DATA_WIDTH);
	dev_info(dev, "DATA_WIDTH configured = %x\n", data);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_NO_OF_PIXELS);
	dev_info(dev, "No of output pixels per pixel clock = %x\n", data);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_NO_OF_VC);
	dev_info(dev, "No of Virtual channels supported = %x\n", data & 0xF);
	dev_info(dev, "Max Virtual channels supported = %x\n", data >> 4);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_INPUT_DATA_INVERT);
	dev_info(dev, "Input data is inverted = %s\n", data ? "true" : "false");

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_FIFO_SIZE);
	dev_info(dev, "FIFO size = %x\n", data);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_CLK_STATUS);
	dev_info(dev, "MIPI clock state = %s\n", data ? "Running" : "Stopped");

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_WORD_COUNT);
	dev_info(dev, "total packet recvied = %x\n", data);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_CAM_DATA_TYPE);
	dev_info(dev, "MIPI_CAM_DATA_TYPE = %x\n", data);

	data = mipi_csi2rx_read(csi2rx, MIPI_CSI_CAM_LANES_CONFIG);
	dev_info(dev, "MIPI_CAM_LANES_CONFIG = %x\n", data);

	mutex_unlock(&csi2rx->lock);

	return 0;
}

static struct v4l2_subdev *mipi_csi2rx_get_remote_subdev(struct media_pad *local)
{
	struct media_pad *remote;

	remote = media_pad_remote_pad_first(local);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return NULL;

	return media_entity_to_v4l2_subdev(remote->entity);
}

static int mipi_csi2rx_start_stream(struct mipi_csi2rx_state *state)
{
	int ret = 0;

	ret = mipi_csi2rx_soft_reset(state);
	if (ret) {
		state->streaming = false;
		return ret;
	}

	mipi_csi2rx_set(state, MIPI_CSI_GLOBAL_INTERRUPT,
			MIPI_CSI_GLOBAL_IRQ_EN);
	mipi_csi2rx_set(state, MIPI_CSI_INTERRUPT_EN,
			MIPI_CSI_INTERRUPT_EN_MASK);
	mipi_csi2rx_set(state, MIPI_CSI_CTRL, MIPI_CSI_CTRL_START);

	state->streaming = true;

	state->rsubdev =
		mipi_csi2rx_get_remote_subdev(&state->pads[MVC_PAD_SINK]);

	ret = v4l2_subdev_call(state->rsubdev, video, s_stream, 1);
	if (ret) {
		mipi_csi2rx_clr(state, MIPI_CSI_GLOBAL_INTERRUPT,
				MIPI_CSI_GLOBAL_IRQ_EN);
		mipi_csi2rx_clr(state, MIPI_CSI_INTERRUPT_EN,
				MIPI_CSI_INTERRUPT_EN_MASK);
		mipi_csi2rx_clr(state, MIPI_CSI_CTRL, MIPI_CSI_CTRL_START);
		state->streaming = false;
	}

	return ret;
}

static void mipi_csi2rx_stop_stream(struct mipi_csi2rx_state *state)
{
	v4l2_subdev_call(state->rsubdev, video, s_stream, 0);

	mipi_csi2rx_clr(state, MIPI_CSI_GLOBAL_INTERRUPT, MIPI_CSI_GLOBAL_IRQ_EN);
	mipi_csi2rx_clr(state, MIPI_CSI_INTERRUPT_EN, MIPI_CSI_INTERRUPT_EN_MASK);
	mipi_csi2rx_clr(state, MIPI_CSI_CTRL, MIPI_CSI_CTRL_START);

	state->streaming = false;
}

static irqreturn_t mipi_csi2rx_irq_handler(int irq, void *data)
{
	struct mipi_csi2rx_state *state = (struct mipi_csi2rx_state *)data;
	struct device *dev = state->dev;
	u32 status;

	status = mipi_csi2rx_read(state, MIPI_CSI_INTERRUPT_STATUS);
	mipi_csi2rx_set(state, MIPI_CSI_INTERRUPT_STATUS, status);

	if (status) {
		unsigned int i;

		for (i = 0; i < MIPI_CSI_NUM_EVENTS; i++) {
			if (!(status & mipi_csi2rx_events[i].mask))
				continue;
			state->events[i]++;
			dev_dbg_ratelimited(dev, "%s: %u\n",
					    mipi_csi2rx_events[i].name,
					    state->events[i]);
		}
	}

	return IRQ_HANDLED;
}

static int mipi_csi2rx_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mipi_csi2rx_state *csi2rx = to_csi2rxstate(sd);
	int ret = 0;

	mutex_lock(&csi2rx->lock);

	if (enable == csi2rx->streaming)
		goto stream_done;

	if (enable) {
		mipi_csi2rx_reset_event_counters(csi2rx);
		ret = mipi_csi2rx_start_stream(csi2rx);
	} else {
		mipi_csi2rx_stop_stream(csi2rx);
	}

stream_done:
	mutex_unlock(&csi2rx->lock);

	return ret;
}

static struct v4l2_mbus_framefmt *
__mipi_csi2rx_get_pad_format(struct mipi_csi2rx_state *csi2rx,
			     struct v4l2_subdev_state *sd_state,
			     unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&csi2rx->subdev,
						  sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
			return &csi2rx->format[pad];
	default:
		return NULL;
	}
}

static int mipi_csi2rx_init_cfg(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state)
{
	struct mipi_csi2rx_state *csi2rx = to_csi2rxstate(sd);
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	mutex_lock(&csi2rx->lock);
	for (i = 0; i < MIPI_CSI_MEDIA_PADS; i++) {
		format = v4l2_subdev_get_try_format(sd, sd_state, i);
		*format = csi2rx->default_format[i];
	}
	mutex_unlock(&csi2rx->lock);

	return 0;
}

static int mipi_csi2rx_get_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	struct mipi_csi2rx_state *csi2rx = to_csi2rxstate(sd);

	mutex_lock(&csi2rx->lock);
	fmt->format = *__mipi_csi2rx_get_pad_format(csi2rx, sd_state,
						  fmt->pad,
						  fmt->which);
	mutex_unlock(&csi2rx->lock);

	return 0;
}

static int mipi_csi2rx_set_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	struct mipi_csi2rx_state *csi2rx = to_csi2rxstate(sd);
	struct v4l2_mbus_framefmt *__format, *__format_sync;
	u32 dt;

	mutex_lock(&csi2rx->lock);

	__format = __mipi_csi2rx_get_pad_format(csi2rx, sd_state,
						fmt->pad, fmt->which);

	if (fmt->pad == MVC_PAD_SOURCE) {
		if (csi2rx->csi_fixed_out_raw8) {
			dev_info(csi2rx->dev, "csi_fixed_out_raw8 is enabled");
			switch (__format->code) {
			case MEDIA_BUS_FMT_SBGGR10_1X10:
			case MEDIA_BUS_FMT_SBGGR12_1X12:
			case MEDIA_BUS_FMT_SBGGR14_1X14:
			case MEDIA_BUS_FMT_SBGGR16_1X16:
				__format->code = MEDIA_BUS_FMT_SBGGR8_1X8;
				break;
			case MEDIA_BUS_FMT_SGBRG10_1X10:
			case MEDIA_BUS_FMT_SGBRG12_1X12:
			case MEDIA_BUS_FMT_SGBRG14_1X14:
			case MEDIA_BUS_FMT_SGBRG16_1X16:
				__format->code = MEDIA_BUS_FMT_SGBRG8_1X8;
				break;
			case MEDIA_BUS_FMT_SGRBG10_1X10:
			case MEDIA_BUS_FMT_SGRBG12_1X12:
			case MEDIA_BUS_FMT_SGRBG14_1X14:
			case MEDIA_BUS_FMT_SGRBG16_1X16:
				__format->code = MEDIA_BUS_FMT_SGRBG8_1X8;
				break;
			case MEDIA_BUS_FMT_SRGGB10_1X10:
			case MEDIA_BUS_FMT_SRGGB12_1X12:
			case MEDIA_BUS_FMT_SRGGB14_1X14:
			case MEDIA_BUS_FMT_SRGGB16_1X16:
				__format->code = MEDIA_BUS_FMT_SRGGB8_1X8;
				break;
			}
		}

		__format_sync = __mipi_csi2rx_get_pad_format(csi2rx, sd_state, 0, fmt->which);

		__format->width = __format_sync->width;
		__format->height = __format_sync->height;
		fmt->format = *__format;

		mutex_unlock(&csi2rx->lock);

		return 0;
	}

	dt = mipi_csi2rx_get_dt(fmt->format.code);
	if (dt != csi2rx->datatype && dt != MIPI_CSI2_DT_RAW8) {
		dev_info(csi2rx->dev, "Unsupported media bus format");
		/* set the default format for the data type */
		fmt->format.code = mipi_csi2rx_get_nth_mbus(csi2rx->datatype,
							    0);
	}

	*__format = fmt->format;
	mutex_unlock(&csi2rx->lock);

	return 0;
}

static int mipi_csi2rx_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	struct mipi_csi2rx_state *state = to_csi2rxstate(sd);
	u32 dt, n;
	int ret = 0;

	if (code->index < MIPI_CSI2_MAX_RAW_FORMATS) {
		n = code->index;
		dt = MIPI_CSI2_DT_RAW8;
	} else if (state->datatype != MIPI_CSI2_DT_RAW8) {
		n = code->index - MIPI_CSI2_MAX_RAW_FORMATS;
		dt = state->datatype;
	} else {
		return -EINVAL;
	}

	code->code = mipi_csi2rx_get_nth_mbus(dt, n);
	if (!code->code)
		ret = -EINVAL;

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations mipi_csi2rx_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static const struct v4l2_subdev_core_ops mipi_csi2rx_core_ops = {
	.log_status = mipi_csi2rx_log_status,
};

static const struct v4l2_subdev_video_ops mipi_csi2rx_video_ops = {
	.s_stream = mipi_csi2rx_s_stream
};

static const struct v4l2_subdev_pad_ops mipi_csi2rx_pad_ops = {
	.init_cfg = mipi_csi2rx_init_cfg,
	.get_fmt = mipi_csi2rx_get_format,
	.set_fmt = mipi_csi2rx_set_format,
	.enum_mbus_code = mipi_csi2rx_enum_mbus_code,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mipi_csi2rx_ops = {
	.core = &mipi_csi2rx_core_ops,
	.video = &mipi_csi2rx_video_ops,
	.pad = &mipi_csi2rx_pad_ops
};

static int mipi_csi2rx_parse_of(struct mipi_csi2rx_state *csi2rx)
{
	struct device *dev = csi2rx->dev;
	struct device_node *node = dev->of_node;

	struct fwnode_handle *ep;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;

	ret = of_property_read_u32(node, "microchip,csi-pxl-format",
				   &csi2rx->datatype);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing microchip,csi-pxl-format property\n");

	switch (csi2rx->datatype) {
	case MIPI_CSI2_DT_RAW8:
	case MIPI_CSI2_DT_RAW10:
	case MIPI_CSI2_DT_RAW12:
	case MIPI_CSI2_DT_RAW14:
	case MIPI_CSI2_DT_RAW16:
		csi2rx->csi_fixed_out_raw8 =
			of_property_read_bool(node, "microchip,csi-fixed-out-raw8");
		break;
	case MIPI_CSI2_DT_RGB888:
		break;
	default:
		return dev_err_probe(dev, -EINVAL, "invalid csi-pxl-format property!\n");
	}

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     MVC_PAD_SINK, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return dev_err_probe(dev, -EINVAL, "no sink port found");

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "error parsing sink port");

	dev_dbg(dev, "mipi number lanes = %d\n",
		vep.bus.mipi_csi2.num_data_lanes);

	csi2rx->max_num_lanes = vep.bus.mipi_csi2.num_data_lanes;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     MVC_PAD_SOURCE, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return dev_err_probe(dev, -EINVAL, "no source port found");

	fwnode_handle_put(ep);

	dev_dbg(dev, "%u data lanes, data type 0x%02x\n",
		csi2rx->max_num_lanes,
		csi2rx->datatype);

	return 0;
}

static int mipi_csi2rx_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct mipi_csi2rx_state *csi2rx;
	int num_clks = ARRAY_SIZE(mipi_csi2rx_clks);
	struct device *dev = &pdev->dev;
	int irq, ret;

	csi2rx = devm_kzalloc(dev, sizeof(*csi2rx), GFP_KERNEL);
	if (!csi2rx)
		return -ENOMEM;

	csi2rx->dev = dev;

	csi2rx->clks = devm_kmemdup(dev, mipi_csi2rx_clks,
				    sizeof(mipi_csi2rx_clks), GFP_KERNEL);
	if (!csi2rx->clks)
		return -ENOMEM;

	ret = mipi_csi2rx_parse_of(csi2rx);
	if (ret < 0)
		return ret;

	csi2rx->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(csi2rx->iomem))
		return PTR_ERR(csi2rx->iomem);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(dev, irq, NULL,
					mipi_csi2rx_irq_handler, IRQF_ONESHOT,
					dev_name(dev), csi2rx);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to allocate irq\n");

	ret = clk_bulk_get(dev, num_clks, csi2rx->clks);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(num_clks, csi2rx->clks);
	if (ret)
		goto err_clk_put;

	mutex_init(&csi2rx->lock);

	mipi_csi2rx_soft_reset(csi2rx);

	csi2rx->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	csi2rx->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize the default format */
	csi2rx->default_format[MVC_PAD_SINK].code =
		mipi_csi2rx_get_nth_mbus(csi2rx->datatype, 0);
	csi2rx->default_format[MVC_PAD_SINK].field = V4L2_FIELD_NONE;
	csi2rx->default_format[MVC_PAD_SINK].colorspace = V4L2_COLORSPACE_SRGB;
	csi2rx->default_format[MVC_PAD_SINK].width = MIPI_CSI_DEFAULT_WIDTH;
	csi2rx->default_format[MVC_PAD_SINK].height = MIPI_CSI_DEFAULT_HEIGHT;
	csi2rx->format[MVC_PAD_SINK] = csi2rx->default_format[MVC_PAD_SINK];

	csi2rx->default_format[MVC_PAD_SOURCE].code =
		mipi_csi2rx_get_nth_mbus(csi2rx->datatype, 0);
	csi2rx->default_format[MVC_PAD_SOURCE].field = V4L2_FIELD_NONE;
	csi2rx->default_format[MVC_PAD_SOURCE].colorspace = V4L2_COLORSPACE_SRGB;
	csi2rx->default_format[MVC_PAD_SOURCE].width = MIPI_CSI_DEFAULT_WIDTH;
	csi2rx->default_format[MVC_PAD_SOURCE].height = MIPI_CSI_DEFAULT_HEIGHT;
	csi2rx->format[MVC_PAD_SOURCE] = csi2rx->default_format[MVC_PAD_SOURCE];

	/* Initialize V4L2 subdevice and media entity */
	subdev = &csi2rx->subdev;
	v4l2_subdev_init(subdev, &mipi_csi2rx_ops);
	subdev->dev = dev;
	strscpy(subdev->name, dev_name(dev), sizeof(subdev->name));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_EVENTS | V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &mipi_csi2rx_media_ops;
	v4l2_set_subdevdata(subdev, csi2rx);

	ret = media_entity_pads_init(&subdev->entity, MIPI_CSI_MEDIA_PADS,
				     csi2rx->pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, csi2rx);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto error;
	}

	return 0;
error:
	media_entity_cleanup(&subdev->entity);
	mutex_destroy(&csi2rx->lock);
	clk_bulk_disable_unprepare(num_clks, csi2rx->clks);
err_clk_put:
	clk_bulk_put(num_clks, csi2rx->clks);
	return ret;
}

static int mipi_csi2rx_remove(struct platform_device *pdev)
{
	struct mipi_csi2rx_state *csi2rx = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &csi2rx->subdev;
	int num_clks = ARRAY_SIZE(mipi_csi2rx_clks);

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);
	mutex_destroy(&csi2rx->lock);
	clk_bulk_disable_unprepare(num_clks, csi2rx->clks);
	clk_bulk_put(num_clks, csi2rx->clks);

	return 0;
}

static const struct of_device_id mipi_csi2rx_of_id_table[] = {
	{ .compatible = "microchip,mipi-csi2-rx-rtl-v0", },
	{ }
};

MODULE_DEVICE_TABLE(of, mipi_csi2rx_of_id_table);

static struct platform_driver mipi_csi2rx_driver = {
	.driver = {
		.name		= "microchip-csi2rxss",
		.of_match_table	= mipi_csi2rx_of_id_table,
	},
	.probe			= mipi_csi2rx_probe,
	.remove			= mipi_csi2rx_remove,
};

module_platform_driver(mipi_csi2rx_driver);

MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip MIPI CSI-2 Rx Subsystem Driver");
MODULE_LICENSE("GPL");
