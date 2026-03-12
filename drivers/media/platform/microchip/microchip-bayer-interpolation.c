// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Video Bayer Interpolation Driver
 *
 * Copyright (C) 2023-2024 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/v4l2-subdev.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>
#include "microchip-common.h"

#define MCHP_BAYER_INTERPOLATION_IP_VER		(0x00)
#define MCHP_BAYER_INTERPOLATION_CTRL_REG	(0x04)
#define MCHP_BAYER_INTERPOLATION_BAYER		(0x08)

#define MCHP_BAYER_INTERPOLATION_START		BIT(0)
#define MCHP_BAYER_INTERPOLATION_RESET		BIT(1)

#define MCHP_BAYER_INTERPOLATION_DEF_WIDTH	1920
#define MCHP_BAYER_INTERPOLATION_DEF_HEIGHT	1080

enum mchp_bayer_interpolation_format {
	MCHP_BAYER_INTERPOLATION_RGGB = 0,
	MCHP_BAYER_INTERPOLATION_GRBG,
	MCHP_BAYER_INTERPOLATION_GBRG,
	MCHP_BAYER_INTERPOLATION_BGGR,
};

/**
 * struct mchp_bayer_interpolation_device - Microchip bayer interpolation device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: active V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @bayer_fmt: bayer format
 */
struct mchp_bayer_interpolation_dev {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];

	enum mchp_bayer_interpolation_format bayer_fmt;
};

static inline u32
mchp_bayer_interpolation_reg_read(struct mchp_bayer_interpolation_dev *mchp_bayer, u32 addr)
{
	return ioread32(mchp_bayer->iomem + addr);
}

static inline void
mchp_bayer_interpolation_reg_write(struct mchp_bayer_interpolation_dev *mchp_bayer,
				   u32 addr, u32 value)
{
	iowrite32(value, mchp_bayer->iomem + addr);
}

static inline struct mchp_bayer_interpolation_dev *to_mchp_bayer(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_bayer_interpolation_dev, subdev);
}

static struct v4l2_mbus_framefmt
*__mchp_bayer_interpolation_get_pad_format(struct mchp_bayer_interpolation_dev *mchp_bayer,
					   struct v4l2_subdev_state *sd_state,
					   unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_state_get_format(sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &mchp_bayer->formats[pad];
	default:
		return NULL;
	}
}

static int mchp_bayer_interpolation_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct mchp_bayer_interpolation_dev *mchp_bayer = to_mchp_bayer(subdev);

	if (!enable) {
		mchp_bayer_interpolation_reg_write(mchp_bayer, MCHP_BAYER_INTERPOLATION_CTRL_REG,
						   MCHP_BAYER_INTERPOLATION_RESET);
		return 0;
	}

	mchp_bayer_interpolation_reg_write(mchp_bayer, MCHP_BAYER_INTERPOLATION_BAYER,
					   mchp_bayer->bayer_fmt);
	mchp_bayer_interpolation_reg_write(mchp_bayer, MCHP_BAYER_INTERPOLATION_CTRL_REG,
					   MCHP_BAYER_INTERPOLATION_START);

	return 0;
}

static const struct v4l2_subdev_video_ops mchp_bayer_interpolation_video_ops = {
	.s_stream = mchp_bayer_interpolation_s_stream,
};

static int mchp_bayer_interpolation_get_format(struct v4l2_subdev *subdev,
					       struct v4l2_subdev_state *sd_state,
					       struct v4l2_subdev_format *fmt)
{
	struct mchp_bayer_interpolation_dev *mchp_bayer = to_mchp_bayer(subdev);
	struct v4l2_mbus_framefmt *get_fmt;

	get_fmt = __mchp_bayer_interpolation_get_pad_format(mchp_bayer, sd_state,
							    fmt->pad, fmt->which);
	if (!get_fmt)
		return -EINVAL;

	fmt->format = *get_fmt;

	return 0;
}

static bool
mchp_bayer_interpolation_is_format_bayer(struct mchp_bayer_interpolation_dev *mchp_bayer, u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
	case MEDIA_BUS_FMT_SRGGB16_1X16:
		mchp_bayer->bayer_fmt = MCHP_BAYER_INTERPOLATION_RGGB;
		break;
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SGRBG16_1X16:
		mchp_bayer->bayer_fmt = MCHP_BAYER_INTERPOLATION_GRBG;
		break;
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGBRG16_1X16:
		mchp_bayer->bayer_fmt = MCHP_BAYER_INTERPOLATION_GBRG;
		break;
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SBGGR16_1X16:
		mchp_bayer->bayer_fmt = MCHP_BAYER_INTERPOLATION_BGGR;
		break;
	default:
		dev_dbg(mchp_bayer->dev, "Unsupported format for Sink Pad");
		return false;
	}
	return true;
}

static int mchp_bayer_interpolation_set_format(struct v4l2_subdev *subdev,
					       struct v4l2_subdev_state *sd_state,
					       struct v4l2_subdev_format *fmt)
{
	struct mchp_bayer_interpolation_dev *mchp_bayer = to_mchp_bayer(subdev);
	struct v4l2_mbus_framefmt *__format;

	__format = __mchp_bayer_interpolation_get_pad_format(mchp_bayer, sd_state,
							     fmt->pad, fmt->which);
	if (!__format)
		return -EINVAL;

	*__format = fmt->format;

	if (fmt->pad == MVC_PAD_SOURCE) {
		if (__format->code != MEDIA_BUS_FMT_RGB888_1X24) {
			dev_dbg(mchp_bayer->dev,
				"%s : Unsupported source media bus code format",
				__func__);
			__format->code = MEDIA_BUS_FMT_RGB888_1X24;
		}
	}

	if (fmt->pad == MVC_PAD_SINK) {
		if (!mchp_bayer_interpolation_is_format_bayer(mchp_bayer, __format->code)) {
			dev_dbg(mchp_bayer->dev,
				"Unsupported Sink Pad Media format, defaulting to RGGB");
			__format->code = MEDIA_BUS_FMT_SRGGB8_1X8;
		}
	}

	fmt->format = *__format;

	return 0;
}

static int mchp_bayer_interpolation_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct mchp_bayer_interpolation_dev *mchp_bayer = to_mchp_bayer(subdev);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(fh->state, MVC_PAD_SINK);
	*format = mchp_bayer->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_state_get_format(fh->state, MVC_PAD_SOURCE);
	*format = mchp_bayer->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_bayer_interpolation_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static const struct v4l2_subdev_internal_ops mchp_bayer_interpolation_internal_ops = {
	.open = mchp_bayer_interpolation_open,
	.close = mchp_bayer_interpolation_close,
};

static const struct v4l2_subdev_pad_ops mchp_bayer_interpolation_pad_ops = {
	.enum_mbus_code = mvc_enum_mbus_code,
	.get_fmt = mchp_bayer_interpolation_get_format,
	.set_fmt = mchp_bayer_interpolation_set_format,
};

static const struct v4l2_subdev_ops mchp_bayer_interpolation_ops = {
	.video = &mchp_bayer_interpolation_video_ops,
	.pad = &mchp_bayer_interpolation_pad_ops,
};

static int mchp_bayer_interpolation_parse_of(struct mchp_bayer_interpolation_dev *mchp_bayer)
{
	struct device *dev = mchp_bayer->dev;
	struct device_node *node = dev->of_node;
	struct device_node *ports;
	struct device_node *port;
	u32 port_id = 0;
	int ret;

	ports = of_get_child_by_name(node, "ports");
	if (!ports)
		ports = node;

	for_each_child_of_node(ports, port) {
		if (port->name && (of_node_cmp(port->name, "port") == 0)) {
			ret = of_property_read_u32(port, "reg", &port_id);
			if (ret < 0) {
				dev_err(dev, "No reg in DT");
				return ret;
			}

			if (port_id != 0 && port_id != 1) {
				dev_err(dev, "Invalid reg in DT");
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int mchp_bayer_interpolation_probe(struct platform_device *pdev)
{
	struct mchp_bayer_interpolation_dev *mchp_bayer;
	struct v4l2_subdev *subdev;
	struct v4l2_mbus_framefmt *def_fmt;
	int ret;

	mchp_bayer = devm_kzalloc(&pdev->dev, sizeof(*mchp_bayer), GFP_KERNEL);
	if (!mchp_bayer)
		return -ENOMEM;
	mchp_bayer->dev = &pdev->dev;

	mchp_bayer->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mchp_bayer->iomem))
		return PTR_ERR(mchp_bayer->iomem);

	ret = mchp_bayer_interpolation_parse_of(mchp_bayer);
	if (ret < 0)
		return ret;

	subdev = &mchp_bayer->subdev;
	v4l2_subdev_init(subdev, &mchp_bayer_interpolation_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_bayer_interpolation_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	def_fmt = &mchp_bayer->default_formats[MVC_PAD_SINK];
	def_fmt->field = V4L2_FIELD_NONE;
	def_fmt->colorspace = V4L2_COLORSPACE_SRGB;
	def_fmt->width = MCHP_BAYER_INTERPOLATION_DEF_WIDTH;
	def_fmt->height = MCHP_BAYER_INTERPOLATION_DEF_HEIGHT;

	def_fmt->code = MEDIA_BUS_FMT_SRGGB8_1X8;
	mchp_bayer->formats[MVC_PAD_SINK] = *def_fmt;

	def_fmt = &mchp_bayer->default_formats[MVC_PAD_SOURCE];
	*def_fmt = mchp_bayer->default_formats[MVC_PAD_SINK];

	def_fmt->code = MEDIA_BUS_FMT_RGB888_1X24;
	mchp_bayer->formats[MVC_PAD_SOURCE] = *def_fmt;

	mchp_bayer->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	mchp_bayer->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&subdev->entity, 2, mchp_bayer->pads);
	if (ret < 0)
		goto media_error;

	platform_set_drvdata(pdev, mchp_bayer);
	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev");
		goto v4l2_subdev_error;
	}

	return 0;

v4l2_subdev_error:
	media_entity_cleanup(&subdev->entity);
media_error:
	return ret;
}

static void mchp_bayer_interpolation_remove(struct platform_device *pdev)
{
	struct mchp_bayer_interpolation_dev *mchp_bayer = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &mchp_bayer->subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id mchp_bayer_interpolation_of_id_table[] = {
	{.compatible = "microchip,bayer-interpolation-rtl-v4.8"},
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_bayer_interpolation_of_id_table);

static struct platform_driver mchp_bayer_interpolation_driver = {
	.driver = {
		.name = "microchip-bayer-interpolation",
		.of_match_table = mchp_bayer_interpolation_of_id_table,
	},
	.probe = mchp_bayer_interpolation_probe,
	.remove = mchp_bayer_interpolation_remove,

};

module_platform_driver(mchp_bayer_interpolation_driver);
MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip Bayer Interpolation IP Driver");
MODULE_LICENSE("GPL");
