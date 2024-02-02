// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip RGB Scaler Driver.
 *
 * Copyright (C) 2022-2023 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/device.h>
#include <linux/fixp-arith.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#include <dt-bindings/media/microchip-common.h>

#include "microchip-common.h"

#define MCHP_RGB_SCALER_MIN_WIDTH		32
#define MCHP_RGB_SCALER_MAX_WIDTH		4096
#define MCHP_RGB_SCALER_MIN_HEIGHT		32
#define MCHP_RGB_SCALER_MAX_HEIGHT		4096

#define MCHP_RGB_SCALER_DEFAULT_WIDTH		1920
#define MCHP_RGB_SCALER_DEFAULT_HEIGHT		1080

#define MCHP_RGB_SCALER_IP_VER			0x00
#define MCHP_RGB_SCALER_CTRL			0x04
#define MCHP_RGB_SCALER_CTRL_START		BIT(0)
#define MCHP_RGB_SCALER_CTRL_RESET		BIT(1)

#define MCHP_RGB_SCALER_INPUT_HRES		0x08
#define MCHP_RGB_SCALER_INPUT_VRES		0x0C
#define MCHP_RGB_SCALER_OUTPUT_HRES		0x10
#define MCHP_RGB_SCALER_OUTPUT_VRES		0x14
#define MCHP_RGB_SCALER_FACTOR_H		0x18
#define MCHP_RGB_SCALER_FACTOR_V		0x1C

#define MCHP_RGB_SCALER_DEF_FORMAT		MVCF_RGB

/**
 * struct mchp_rgb_scaler_device - Microchip RGB Scaler device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @vip_formats: Microchi Video IP format
 * @crop: Active crop rectangle for the sink pad
 */
struct mchp_rgb_scaler_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
	const struct mvideo_format *vip_formats[2];

	struct v4l2_rect crop;
};

static inline struct mchp_rgb_scaler_device *to_scaler(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_rgb_scaler_device, subdev);
}

static inline u32 mchp_rgb_scaler_read(struct mchp_rgb_scaler_device *rgb_scaler, u32 addr)
{
	return readl(rgb_scaler->iomem + addr);
}

static inline void mchp_rgb_scaler_write(struct mchp_rgb_scaler_device *rgb_scaler,
					 u32 addr, u32 value)
{
	writel(value, rgb_scaler->iomem + addr);
}

static inline void mchp_rgb_scaler_clr(struct mchp_rgb_scaler_device *rgb_scaler,
				       u32 addr, u32 clr)
{
	mchp_rgb_scaler_write(rgb_scaler, addr,
			      mchp_rgb_scaler_read(rgb_scaler, addr) & ~clr);
}

static inline void mchp_rgb_scaler_set(struct mchp_rgb_scaler_device *rgb_scaler,
				       u32 addr, u32 set)
{
	mchp_rgb_scaler_write(rgb_scaler, addr, mchp_rgb_scaler_read(rgb_scaler, addr) | set);
}

static void mchp_set_scale_factor(struct mchp_rgb_scaler_device *rgb_scaler)
{
	u32 in_width, in_height, out_width, out_height;
	u32 scale_factor_v, scale_factor_h;

	in_width = rgb_scaler->formats[MVC_PAD_SINK].width;
	in_height = rgb_scaler->formats[MVC_PAD_SINK].height;
	out_width = rgb_scaler->formats[MVC_PAD_SOURCE].width;
	out_height = rgb_scaler->formats[MVC_PAD_SOURCE].height;

	scale_factor_h = (((in_width - 1) * 1024) / out_width);
	scale_factor_v = (((in_height - 1) * 1024) / out_height);

	mchp_rgb_scaler_write(rgb_scaler, MCHP_RGB_SCALER_INPUT_HRES, in_width);
	mchp_rgb_scaler_write(rgb_scaler, MCHP_RGB_SCALER_INPUT_VRES, in_height);
	mchp_rgb_scaler_write(rgb_scaler, MCHP_RGB_SCALER_OUTPUT_HRES, out_width);
	mchp_rgb_scaler_write(rgb_scaler, MCHP_RGB_SCALER_OUTPUT_VRES, out_height);
	mchp_rgb_scaler_write(rgb_scaler, MCHP_RGB_SCALER_FACTOR_H, scale_factor_h);
	mchp_rgb_scaler_write(rgb_scaler, MCHP_RGB_SCALER_FACTOR_V, scale_factor_v);
}

static int mchp_rgb_scaler_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct mchp_rgb_scaler_device *rgb_scaler = to_scaler(subdev);

	if (!enable) {
		mchp_rgb_scaler_clr(rgb_scaler, MCHP_RGB_SCALER_CTRL, MCHP_RGB_SCALER_CTRL_START);
		return 0;
	}

	mchp_set_scale_factor(rgb_scaler);

	mchp_rgb_scaler_set(rgb_scaler, MCHP_RGB_SCALER_CTRL, MCHP_RGB_SCALER_CTRL_START);

	return 0;
}

static int mchp_rgb_scaler_enum_frame_size(struct v4l2_subdev *subdev,
					   struct v4l2_subdev_state *sd_state,
					   struct v4l2_subdev_frame_size_enum *fse)
{
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_get_try_format(subdev, sd_state, fse->pad);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	fse->min_width = MCHP_RGB_SCALER_MIN_WIDTH;
	fse->max_width = MCHP_RGB_SCALER_MAX_WIDTH;
	fse->min_height = MCHP_RGB_SCALER_MIN_HEIGHT;
	fse->max_height = MCHP_RGB_SCALER_MAX_HEIGHT;

	return 0;
}

static struct v4l2_mbus_framefmt *
__mchp_rgb_scaler_get_pad_format(struct mchp_rgb_scaler_device *rgb_scaler,
				 struct v4l2_subdev_state *sd_state,
				 unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&rgb_scaler->subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &rgb_scaler->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static struct v4l2_rect *
__mchp_rgb_scaler_get_crop(struct mchp_rgb_scaler_device *rgb_scaler,
			   struct v4l2_subdev_state *sd_state, u32 which)
{
	struct v4l2_rect *crop;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		crop = v4l2_subdev_get_try_crop(&rgb_scaler->subdev,
						sd_state,
						MVC_PAD_SINK);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		crop = &rgb_scaler->crop;
		break;
	default:
		crop = NULL;
		break;
	}

	return crop;
}

static int mchp_rgb_scaler_get_format(struct v4l2_subdev *subdev,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_format *fmt)
{
	struct mchp_rgb_scaler_device *rgb_scaler = to_scaler(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_rgb_scaler_get_pad_format(rgb_scaler, sd_state, fmt->pad,
						  fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static void mchp_rgb_scaler_try_crop(const struct v4l2_mbus_framefmt *sink,
				     struct v4l2_rect *crop)
{
	crop->left = min_t(u32, crop->left, sink->width - MCHP_RGB_SCALER_MIN_WIDTH);
	crop->top = min_t(u32, crop->top, sink->height - MCHP_RGB_SCALER_MIN_HEIGHT);
	crop->width = clamp_t(u32, crop->width, MCHP_RGB_SCALER_MIN_WIDTH,
			      sink->width - crop->left);
	crop->height = clamp_t(u32, crop->height, MCHP_RGB_SCALER_MIN_HEIGHT,
			       sink->height - crop->top);
}

static int mchp_rgb_scaler_set_format(struct v4l2_subdev *subdev,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_format *fmt)
{
	struct mchp_rgb_scaler_device *rgb_scaler = to_scaler(subdev);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	format = __mchp_rgb_scaler_get_pad_format(rgb_scaler, sd_state, fmt->pad,
						  fmt->which);
	if (!format)
		return -EINVAL;

	format->width = clamp_t(unsigned int, fmt->format.width,
				MCHP_RGB_SCALER_MIN_WIDTH, MCHP_RGB_SCALER_MAX_WIDTH);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 MCHP_RGB_SCALER_MIN_HEIGHT, MCHP_RGB_SCALER_MAX_HEIGHT);

	fmt->format = *format;

	if (fmt->pad == MVC_PAD_SINK) {
		/* Set the crop rectangle to the full frame */
		crop = __mchp_rgb_scaler_get_crop(rgb_scaler, sd_state, fmt->which);
		if (!crop)
			return -EINVAL;
		crop->left = 0;
		crop->top = 0;
		crop->width = fmt->format.width;
		crop->height = fmt->format.height;
		rgb_scaler->formats[MVC_PAD_SOURCE].width = crop->width;
		rgb_scaler->formats[MVC_PAD_SOURCE].height = crop->height;

		mchp_set_scale_factor(rgb_scaler);
	}

	return 0;
}

static int mchp_rgb_scaler_get_selection(struct v4l2_subdev *subdev,
					 struct v4l2_subdev_state *sd_state,
					 struct v4l2_subdev_selection *sel)
{
	struct mchp_rgb_scaler_device *rgb_scaler = to_scaler(subdev);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	if (sel->pad != MVC_PAD_SINK)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
		format = __mchp_rgb_scaler_get_pad_format(rgb_scaler, sd_state,
							  MVC_PAD_SINK,
							  sel->which);
		if (!format)
			return -EINVAL;

		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = format->width;
		sel->r.height = format->height;
		return 0;
	case V4L2_SEL_TGT_CROP:
		crop = __mchp_rgb_scaler_get_crop(rgb_scaler, sd_state, sel->which);
		if (!crop)
			return -EINVAL;
		sel->r = *crop;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mchp_rgb_scaler_set_selection(struct v4l2_subdev *subdev,
					 struct v4l2_subdev_state *sd_state,
					 struct v4l2_subdev_selection *sel)
{
	struct mchp_rgb_scaler_device *rgb_scaler = to_scaler(subdev);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	if (sel->target != V4L2_SEL_TGT_CROP || sel->pad != MVC_PAD_SINK)
		return -EINVAL;

	format = __mchp_rgb_scaler_get_pad_format(rgb_scaler, sd_state, MVC_PAD_SINK,
						  sel->which);
	if (!format)
		return -EINVAL;

	mchp_rgb_scaler_try_crop(format, &sel->r);
	crop = __mchp_rgb_scaler_get_crop(rgb_scaler, sd_state, sel->which);
	if (!crop)
		return -EINVAL;

	*crop = sel->r;

	rgb_scaler->formats[MVC_PAD_SOURCE].width = crop->width;
	rgb_scaler->formats[MVC_PAD_SOURCE].height = crop->height;

	mchp_set_scale_factor(rgb_scaler);

	return 0;
}

static int mchp_rgb_scaler_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct mchp_rgb_scaler_device *rgb_scaler = to_scaler(subdev);
	struct v4l2_mbus_framefmt *format;

	/* Initialize with default formats */
	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SINK);
	*format = rgb_scaler->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SOURCE);
	*format = rgb_scaler->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_rgb_scaler_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static const struct v4l2_subdev_video_ops mchp_rgb_scaler_video_ops = {
	.s_stream = mchp_rgb_scaler_s_stream,
};

static const struct v4l2_subdev_pad_ops mchp_rgb_scaler_pad_ops = {
	.enum_mbus_code		= mvc_enum_mbus_code,
	.enum_frame_size	= mchp_rgb_scaler_enum_frame_size,
	.get_fmt		= mchp_rgb_scaler_get_format,
	.set_fmt		= mchp_rgb_scaler_set_format,
	.get_selection		= mchp_rgb_scaler_get_selection,
	.set_selection		= mchp_rgb_scaler_set_selection,
};

static const struct v4l2_subdev_ops mchp_rgb_scaler_ops = {
	.video  = &mchp_rgb_scaler_video_ops,
	.pad    = &mchp_rgb_scaler_pad_ops,
};

static const struct v4l2_subdev_internal_ops mchp_rgb_scaler_internal_ops = {
	.open	= mchp_rgb_scaler_open,
	.close	= mchp_rgb_scaler_close,
};

static const struct media_entity_operations mchp_rgb_scaler_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int mchp_rgb_scaler_init_formats(struct mchp_rgb_scaler_device *rgb_scaler)
{
	struct device *dev = rgb_scaler->dev;
	const struct mvideo_format *vip_format;

	vip_format = mvc_get_format_by_vf_code(MCHP_RGB_SCALER_DEF_FORMAT);
	if (IS_ERR(vip_format))
		return dev_err_probe(dev, PTR_ERR(vip_format), "invalid format");

	rgb_scaler->vip_formats[MVC_PAD_SINK] = vip_format;
	rgb_scaler->vip_formats[MVC_PAD_SOURCE] = vip_format;

	return 0;
}

static int mchp_rgb_scaler_probe(struct platform_device *pdev)
{
	struct mchp_rgb_scaler_device *rgb_scaler;
	struct v4l2_subdev *subdev;
	struct v4l2_mbus_framefmt *default_format;
	int ret;

	rgb_scaler = devm_kzalloc(&pdev->dev, sizeof(*rgb_scaler), GFP_KERNEL);
	if (!rgb_scaler)
		return -ENOMEM;

	rgb_scaler->dev = &pdev->dev;

	ret = mchp_rgb_scaler_init_formats(rgb_scaler);
	if (ret < 0)
		return ret;

	rgb_scaler->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rgb_scaler->iomem))
		return PTR_ERR(rgb_scaler->iomem);

	mchp_rgb_scaler_set(rgb_scaler, MCHP_RGB_SCALER_CTRL, MCHP_RGB_SCALER_CTRL_RESET);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &rgb_scaler->subdev;
	v4l2_subdev_init(subdev, &mchp_rgb_scaler_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_rgb_scaler_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, rgb_scaler);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* Initialize default and active formats */
	default_format = &rgb_scaler->default_formats[MVC_PAD_SINK];
	default_format->code = rgb_scaler->vip_formats[MVC_PAD_SINK]->code;
	default_format->field = V4L2_FIELD_NONE;
	default_format->colorspace = V4L2_COLORSPACE_SRGB;
	default_format->width = MCHP_RGB_SCALER_DEFAULT_WIDTH;
	default_format->height = MCHP_RGB_SCALER_DEFAULT_HEIGHT;

	rgb_scaler->formats[MVC_PAD_SINK] = *default_format;

	default_format = &rgb_scaler->default_formats[MVC_PAD_SOURCE];
	*default_format = rgb_scaler->default_formats[MVC_PAD_SINK];
	default_format->width = MCHP_RGB_SCALER_DEFAULT_WIDTH;
	default_format->height = MCHP_RGB_SCALER_DEFAULT_HEIGHT;

	rgb_scaler->formats[MVC_PAD_SOURCE] = *default_format;

	rgb_scaler->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	rgb_scaler->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &mchp_rgb_scaler_media_ops;

	ret = media_entity_pads_init(&subdev->entity, 2, rgb_scaler->pads);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, rgb_scaler);

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

static int mchp_rgb_scaler_remove(struct platform_device *pdev)
{
	struct mchp_rgb_scaler_device *rgb_scaler = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &rgb_scaler->subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);

	return 0;
}

static const struct of_device_id mchp_rgb_scaler_of_id_table[] = {
	{ .compatible = "microchip,rgb-scaler-rtl-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_rgb_scaler_of_id_table);

static struct platform_driver mchp_rgb_scaler_driver = {
	.driver			= {
		.name		= "microchip-rgb-scaler",
		.of_match_table	= mchp_rgb_scaler_of_id_table,
	},
	.probe			= mchp_rgb_scaler_probe,
	.remove			= mchp_rgb_scaler_remove,
};

module_platform_driver(mchp_rgb_scaler_driver);

MODULE_DESCRIPTION("Microchip RGB Scaler Driver");
MODULE_LICENSE("GPL");
