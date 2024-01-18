// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Video Common Driver.
 *
 * Copyright (C) 2023 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/clk.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "microchip-common.h"

#include <dt-bindings/media/microchip-common.h>

static const struct mvideo_format mvideo_formats[] = {
	{ MVCF_YUV_420, 8, NULL, MEDIA_BUS_FMT_UYVY8_2X8, 1,
		V4L2_PIX_FMT_NV12, 2, 1, 1, 2 },
	{ MVCF_H264, 8, NULL, MEDIA_BUS_FMT_H264_1X8, 1,
		V4L2_PIX_FMT_H264, 1, 1, 1, 1 },
	{ MVCF_YUV_422, 8, NULL, MEDIA_BUS_FMT_UYVY8_1X16, 2,
		V4L2_PIX_FMT_YUYV, 1, 1, 1, 1 },
	{ MVCF_YUV_444, 8, NULL, MEDIA_BUS_FMT_VUY8_1X24, 3,
		V4L2_PIX_FMT_YUV444, 1, 1, 1, 1 },
	{ MVCF_RBG, 8, NULL, MEDIA_BUS_FMT_RBG888_1X24, 3,
		V4L2_PIX_FMT_RGB24, 1, 1, 1, 1 },
	{ MVCF_RGB, 8, NULL, MEDIA_BUS_FMT_RGB888_1X24, 3,
		V4L2_PIX_FMT_RGB24, 1, 1, 1, 1 },
	{ MVCF_MJPEG, 8, NULL, MEDIA_BUS_FMT_JPEG_1X8, 3,
		V4L2_PIX_FMT_MJPEG, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 8, "mono", MEDIA_BUS_FMT_Y8_1X8, 1,
		V4L2_PIX_FMT_GREY, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 8, "rggb", MEDIA_BUS_FMT_SRGGB8_1X8, 1,
		V4L2_PIX_FMT_SRGGB8, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 8, "grbg", MEDIA_BUS_FMT_SGRBG8_1X8, 1,
		V4L2_PIX_FMT_SGRBG8, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 8, "gbrg", MEDIA_BUS_FMT_SGBRG8_1X8, 1,
		V4L2_PIX_FMT_SGBRG8, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 8, "bggr", MEDIA_BUS_FMT_SBGGR8_1X8, 1,
		V4L2_PIX_FMT_SBGGR8, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 10, "rggb", MEDIA_BUS_FMT_SRGGB10_1X10, 2,
		V4L2_PIX_FMT_SRGGB10, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 10, "grbg", MEDIA_BUS_FMT_SGRBG10_1X10, 2,
		V4L2_PIX_FMT_SGRBG10, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 10, "gbrg", MEDIA_BUS_FMT_SGBRG10_1X10, 2,
		V4L2_PIX_FMT_SGBRG10, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 10, "bggr", MEDIA_BUS_FMT_SBGGR10_1X10, 2,
		V4L2_PIX_FMT_SBGGR10, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 12, "rggb", MEDIA_BUS_FMT_SRGGB12_1X12, 2,
		V4L2_PIX_FMT_SRGGB12, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 12, "grbg", MEDIA_BUS_FMT_SGRBG12_1X12, 2,
		V4L2_PIX_FMT_SGRBG12, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 12, "gbrg", MEDIA_BUS_FMT_SGBRG12_1X12, 2,
		V4L2_PIX_FMT_SGBRG12, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 12, "bggr", MEDIA_BUS_FMT_SBGGR12_1X12, 2,
		V4L2_PIX_FMT_SBGGR12, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 16, "rggb", MEDIA_BUS_FMT_SRGGB16_1X16, 2,
		V4L2_PIX_FMT_SRGGB16, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 16, "grbg", MEDIA_BUS_FMT_SGRBG16_1X16, 2,
		V4L2_PIX_FMT_SGRBG16, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 16, "gbrg", MEDIA_BUS_FMT_SGBRG16_1X16, 2,
		V4L2_PIX_FMT_SGBRG16, 1, 1, 1, 1 },
	{ MVCF_MONO_SENSOR, 16, "bggr", MEDIA_BUS_FMT_SBGGR16_1X16, 2,
		V4L2_PIX_FMT_SBGGR16, 1, 1, 1, 1 },
};

const struct mvideo_format *mvc_get_format_by_code(unsigned int code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mvideo_formats); ++i) {
		const struct mvideo_format *format = &mvideo_formats[i];

		if (format->code == code)
			return format;
	}

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(mvc_get_format_by_code);

const struct mvideo_format *mvc_get_format_by_vf_code(unsigned int vf_code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mvideo_formats); ++i) {
		const struct mvideo_format *format = &mvideo_formats[i];

		if (format->vf_code == vf_code)
			return format;
	}

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(mvc_get_format_by_vf_code);

const struct mvideo_format *mvc_get_format_by_fourcc(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mvideo_formats); ++i) {
		const struct mvideo_format *format = &mvideo_formats[i];

		if (format->fourcc == fourcc)
			return format;
	}

	return &mvideo_formats[0];
}
EXPORT_SYMBOL_GPL(mvc_get_format_by_fourcc);

const struct mvideo_format *mvc_of_get_format(struct device_node *node)
{
	const char *pattern = "mono";
	unsigned int vf_code;
	unsigned int i;
	u32 width;
	int ret;

	ret = of_property_read_u32(node, "microchip,video-format", &vf_code);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = of_property_read_u32(node, "microchip,video-width", &width);
	if (ret < 0)
		return ERR_PTR(ret);

	if (vf_code == MVCF_MONO_SENSOR)
		of_property_read_string(node, "microchip,cfa-pattern", &pattern);

	for (i = 0; i < ARRAY_SIZE(mvideo_formats); ++i) {
		const struct mvideo_format *format = &mvideo_formats[i];

		if (format->vf_code != vf_code || format->width != width)
			continue;

		if (vf_code == MVCF_MONO_SENSOR &&
		    strcmp(pattern, format->pattern))
			continue;

		return format;
	}

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(mvc_of_get_format);

void mvc_set_format_size(struct v4l2_mbus_framefmt *format,
			 const struct v4l2_subdev_format *fmt)
{
	format->width = clamp_t(unsigned int, fmt->format.width,
				MVC_MIN_WIDTH, MVC_MAX_WIDTH);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 MVC_MIN_HEIGHT, MVC_MAX_HEIGHT);
}
EXPORT_SYMBOL_GPL(mvc_set_format_size);

int mvc_init_resources(struct mvc_device *mvc)
{
	struct platform_device *pdev = to_platform_device(mvc->dev);
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mvc->iomem = devm_ioremap_resource(mvc->dev, res);
	if (IS_ERR(mvc->iomem))
		return PTR_ERR(mvc->iomem);

	mvc->clk = devm_clk_get(mvc->dev, NULL);
	if (IS_ERR(mvc->clk))
		return PTR_ERR(mvc->clk);

	clk_prepare_enable(mvc->clk);
	return 0;
}
EXPORT_SYMBOL_GPL(mvc_init_resources);

void mvc_cleanup_resources(struct mvc_device *mvc)
{
	clk_disable_unprepare(mvc->clk);
}
EXPORT_SYMBOL_GPL(mvc_cleanup_resources);

int mvc_enum_mbus_code(struct v4l2_subdev *subdev,
		       struct v4l2_subdev_state *sd_state,
		       struct v4l2_subdev_mbus_code_enum *code)
{
	struct v4l2_mbus_framefmt *format;

	if (code->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	if (code->index)
		return -EINVAL;

	format = v4l2_subdev_get_try_format(subdev, sd_state, code->pad);

	code->code = format->code;

	return 0;
}
EXPORT_SYMBOL_GPL(mvc_enum_mbus_code);

int mvc_enum_frame_size(struct v4l2_subdev *subdev,
			struct v4l2_subdev_state *sd_state,
			struct v4l2_subdev_frame_size_enum *fse)
{
	struct v4l2_mbus_framefmt *format;

	if (fse->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	format = v4l2_subdev_get_try_format(subdev, sd_state, fse->pad);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	if (fse->pad == MVC_PAD_SINK) {
		fse->min_width = MVC_MIN_WIDTH;
		fse->max_width = MVC_MAX_WIDTH;
		fse->min_height = MVC_MIN_HEIGHT;
		fse->max_height = MVC_MAX_HEIGHT;
	} else {
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mvc_enum_frame_size);

MODULE_DESCRIPTION("Microchip Video Common Driver");
MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_LICENSE("GPL");
