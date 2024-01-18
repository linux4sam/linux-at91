// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip GVPC (Generic Video Pipeline Connector) Driver.
 *
 * Copyright (C) 2022-2023 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "microchip-common.h"

#define MVC_DEF_HEIGHT	1080
#define MVC_DEF_WIDTH	1920

/**
 * struct mchp_gvpc_device - Microchip GVPC Core device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: active V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @vip_formats: format information corresponding to the pads active formats
 */
struct mchp_gvpc_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
	const struct mvideo_format *vip_formats[2];
};

static inline struct mchp_gvpc_device *to_gvpc(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_gvpc_device, subdev);
}

static int mchp_gvpc_s_stream(struct v4l2_subdev *subdev, int enable)
{
	/*
	 * struct mchp_gvpc_device *mchp_gvpc = to_gvpc(subdev);
	 * struct v4l2_mbus_framefmt *format = &mchp_gvpc->formats[MVC_PAD_SINK];
	 */

	return 0;
}

static struct v4l2_mbus_framefmt *
__mchp_gvpc_get_pad_format(struct mchp_gvpc_device *mchp_gvpc,
			   struct v4l2_subdev_state *sd_state,
			   unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&mchp_gvpc->subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &mchp_gvpc->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int mchp_gvpc_get_format(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct mchp_gvpc_device *mchp_gvpc = to_gvpc(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_gvpc_get_pad_format(mchp_gvpc, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int mchp_gvpc_set_format(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct mchp_gvpc_device *mchp_gvpc = to_gvpc(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_gvpc_get_pad_format(mchp_gvpc, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == MVC_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	mvc_set_format_size(format, fmt);

	fmt->format = *format;

	format = __mchp_gvpc_get_pad_format(mchp_gvpc, sd_state, MVC_PAD_SOURCE,
					    fmt->which);
	if (!format)
		return -EINVAL;

	mvc_set_format_size(format, fmt);

	return 0;
}

static int mchp_gvpc_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct mchp_gvpc_device *mchp_gvpc = to_gvpc(subdev);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SINK);
	*format = mchp_gvpc->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SOURCE);
	*format = mchp_gvpc->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_gvpc_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static const struct v4l2_subdev_video_ops mchp_gvpc_video_ops = {
	.s_stream = mchp_gvpc_s_stream,
};

static const struct v4l2_subdev_pad_ops mchp_gvpc_pad_ops = {
	.enum_mbus_code = mvc_enum_mbus_code,
	.get_fmt = mchp_gvpc_get_format,
	.set_fmt = mchp_gvpc_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mchp_gvpc_ops = {
	.video  = &mchp_gvpc_video_ops,
	.pad    = &mchp_gvpc_pad_ops,
};

static const struct v4l2_subdev_internal_ops mchp_gvpc_internal_ops = {
	.open = mchp_gvpc_open,
	.close = mchp_gvpc_close,
};

static const struct media_entity_operations mchp_gvpc_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static void mchp_gvpc_init_formats(struct mchp_gvpc_device *mchp_gvpc)
{
	struct v4l2_mbus_framefmt *format;

	/* Initialize default and active formats */
	format = &mchp_gvpc->default_formats[MVC_PAD_SINK];
	format->code = mchp_gvpc->vip_formats[MVC_PAD_SINK]->code;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	format->width = MVC_DEF_WIDTH;
	format->height = MVC_DEF_HEIGHT;

	mchp_gvpc->formats[MVC_PAD_SINK] = *format;

	format = &mchp_gvpc->default_formats[MVC_PAD_SOURCE];
	*format = mchp_gvpc->default_formats[MVC_PAD_SINK];
	format->code = mchp_gvpc->vip_formats[MVC_PAD_SOURCE]->code;

	mchp_gvpc->formats[MVC_PAD_SOURCE] = *format;
}

static int mchp_gvpc_parse_of(struct mchp_gvpc_device *mchp_gvpc)
{
	struct device *dev = mchp_gvpc->dev;
	struct device_node *node = mchp_gvpc->dev->of_node;
	struct device_node *ports;
	struct device_node *port;
	u32 port_id;
	int ret;

	ports = of_get_child_by_name(node, "ports");
	if (!ports)
		ports = node;

	for_each_child_of_node(ports, port) {
		if (port->name && (of_node_cmp(port->name, "port") == 0)) {
			const struct mvideo_format *vip_format;

			vip_format = mvc_of_get_format(port);
			if (IS_ERR(vip_format))
				return dev_err_probe(dev, IS_ERR(vip_format),
						     "invalid format in DT");

			ret = of_property_read_u32(port, "reg", &port_id);
			if (ret < 0) {
				dev_err(dev, "no reg in DT");
				return ret;
			}

			if (port_id != 0 && port_id != 1) {
				dev_err(dev, "invalid reg in DT");
				return -EINVAL;
			}

			mchp_gvpc->vip_formats[port_id] = vip_format;
		}
	}

	return 0;
}

static int mchp_gvpc_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct mchp_gvpc_device *mchp_gvpc;
	struct resource *res;
	int ret;

	mchp_gvpc = devm_kzalloc(&pdev->dev, sizeof(*mchp_gvpc), GFP_KERNEL);
	if (!mchp_gvpc)
		return -ENOMEM;

	mchp_gvpc->dev = &pdev->dev;

	ret = mchp_gvpc_parse_of(mchp_gvpc);
	if (ret < 0)
		return ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mchp_gvpc->iomem = devm_ioremap_resource(mchp_gvpc->dev, res);
	if (IS_ERR(mchp_gvpc->iomem))
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_gvpc->iomem),
				     "could not get mem resource\n");

	/* Initialize V4L2 subdevice and media entity */
	subdev = &mchp_gvpc->subdev;
	v4l2_subdev_init(subdev, &mchp_gvpc_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_gvpc_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, mchp_gvpc);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	mchp_gvpc_init_formats(mchp_gvpc);

	mchp_gvpc->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	mchp_gvpc->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &mchp_gvpc_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, mchp_gvpc->pads);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, mchp_gvpc);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&subdev->entity);
	return ret;
}

static int mchp_gvpc_remove(struct platform_device *pdev)
{
	struct mchp_gvpc_device *mchp_gvpc = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &mchp_gvpc->subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);

	return 0;
}

static const struct of_device_id mchp_gvpc_of_id_table[] = {
	{ .compatible = "microchip,generic-video-pipeline-connector" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_gvpc_of_id_table);

static struct platform_driver mchp_gvpc_driver = {
	.driver = {
		.name = "microchip-gvpc",
		.of_match_table = mchp_gvpc_of_id_table,
	},
	.probe = mchp_gvpc_probe,
	.remove = mchp_gvpc_remove,
};

module_platform_driver(mchp_gvpc_driver);

MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip Generic Video Pipeline Connector Driver");
MODULE_LICENSE("GPL");
