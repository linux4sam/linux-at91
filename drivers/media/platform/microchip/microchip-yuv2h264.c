// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip H264 Encoder Driver.
 *
 * Copyright (C) 2022-2023 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "microchip-common.h"

#include <dt-bindings/media/microchip-common.h>

#define MCHP_YUV2H264_MIN_WIDTH		32
#define MCHP_YUV2H264_MAX_WIDTH		4096
#define MCHP_YUV2H264_MIN_HEIGHT	32
#define MCHP_YUV2H264_MAX_HEIGHT	4096

#define MCHP_YUV2H264_IP_VER		0x00
#define MCHP_YUV2H264_CTRL		0x04
#define MCHP_YUV2H264_CTRL_START	BIT(0)
#define MCHP_YUV2H264_CTRL_RESET	BIT(1)

#define MCHP_YUV2H264_IP_TYPE		0x08
#define MCHP_YUV2H264_Q_FACTOR		0x0C
#define MCHP_YUV2H264_I_P_FRAME_GAP	0x14
#define MCHP_YUV2H264_H_RES		0x18
#define MCHP_YUV2H264_V_RES		0x1C

#define MCHP_CTL_STEP			1
#define MCHP_Q_FACTOR_CTL_MAX		52
#define MCHP_Q_FACTOR_CTL_MIN		25
#define MCHP_Q_FACTOR_CTL_DEFAULT	30

#define MCHP_P_COUNT_CTL_MAX		255
#define MCHP_P_COUNT_CTL_MIN		0
#define MCHP_P_COUNT_CTL_DEFAULT	0

#define MCHP_YUV2H264_DEFAULT_WIDTH	1920
#define MCHP_YUV2H264_DEFAULT_HEIGHT	1080

#define MCHP_YUV2H264_DEF_IN_FORMAT	MVCF_YUV_422
#define MCHP_YUV2H264_DEF_OUT_FORMAT	MVCF_H264

/**
 * struct mchp_yuv2h264_device - Microchip YUV2H264 device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @vip_formats: Microchip Video IP formats
 * @q_factor: Quality factor
 * @p_count: P-frame count
 * @ctrl_handler: control handler
 */
struct mchp_yuv2h264_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
	const struct mvideo_format *vip_formats[2];
	struct v4l2_ctrl *q_factor;
	struct v4l2_ctrl *p_count;

	struct v4l2_ctrl_handler ctrl_handler;
};

static inline struct mchp_yuv2h264_device *to_mchp_yuv2h264(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_yuv2h264_device, subdev);
}

static inline u32 mchp_yuv2h264_read(struct mchp_yuv2h264_device *yuv2h264,
				     u32 addr)
{
	return readl(yuv2h264->iomem + addr);
}

static inline void mchp_yuv2h264_write(struct mchp_yuv2h264_device *yuv2h264,
				       u32 addr, u32 value)
{
	writel(value, yuv2h264->iomem + addr);
}

static inline void mchp_yuv2h264_clr(struct mchp_yuv2h264_device *yuv2h264,
				     u32 addr, u32 clr)
{
	mchp_yuv2h264_write(yuv2h264, addr,
			    mchp_yuv2h264_read(yuv2h264, addr) & ~clr);
}

static inline void mchp_yuv2h264_set(struct mchp_yuv2h264_device *yuv2h264,
				     u32 addr, u32 set)
{
	mchp_yuv2h264_write(yuv2h264, addr, mchp_yuv2h264_read(yuv2h264, addr) | set);
}

static int mchp_yuv2h264_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct mchp_yuv2h264_device *yuv2h264 = to_mchp_yuv2h264(subdev);
	struct v4l2_mbus_framefmt *format;

	mchp_yuv2h264_set(yuv2h264, MCHP_YUV2H264_CTRL, MCHP_YUV2H264_CTRL_RESET);

	if (!enable) {
		mchp_yuv2h264_clr(yuv2h264, MCHP_YUV2H264_CTRL, MCHP_YUV2H264_CTRL_START);
		return 0;
	}

	format = &yuv2h264->formats[MVC_PAD_SINK];

	mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_H_RES, format->width);
	mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_V_RES, format->height);
	mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_Q_FACTOR, yuv2h264->q_factor->val);
	mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_I_P_FRAME_GAP, yuv2h264->p_count->val);

	mchp_yuv2h264_set(yuv2h264, MCHP_YUV2H264_CTRL, MCHP_YUV2H264_CTRL_START);

	return 0;
}

static struct v4l2_mbus_framefmt *
__mchp_yuv2h264_get_pad_format(struct mchp_yuv2h264_device *yuv2h264,
			       struct v4l2_subdev_state *sd_state,
			       unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&yuv2h264->subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &yuv2h264->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int mchp_yuv2h264_get_format(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_format *fmt)
{
	struct mchp_yuv2h264_device *yuv2h264 = to_mchp_yuv2h264(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_yuv2h264_get_pad_format(yuv2h264, sd_state, fmt->pad,
						fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int mchp_yuv2h264_set_format(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_format *req_fmt)
{
	struct mchp_yuv2h264_device *yuv2h264 = to_mchp_yuv2h264(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_yuv2h264_get_pad_format(yuv2h264, sd_state, req_fmt->pad,
						req_fmt->which);
	if (!format)
		return -EINVAL;

	if (req_fmt->pad == MVC_PAD_SOURCE) {
		req_fmt->format = *format;
		return 0;
	}

	/* Propagate the format to the source pad. */
	format = __mchp_yuv2h264_get_pad_format(yuv2h264, sd_state, MVC_PAD_SOURCE,
						req_fmt->which);

	format->width = req_fmt->format.width;
	format->height = req_fmt->format.height;

	v4l_bound_align_image(&format->width,
			      16, MCHP_YUV2H264_MAX_WIDTH, 0,
			      &format->height,
			      16, MCHP_YUV2H264_MAX_HEIGHT, 0, 0);

	if (req_fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	yuv2h264->formats[MVC_PAD_SINK].width = format->width;
	yuv2h264->formats[MVC_PAD_SINK].height = format->height;
	yuv2h264->formats[MVC_PAD_SOURCE].width = format->width;
	yuv2h264->formats[MVC_PAD_SOURCE].height = format->height;

	mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_H_RES, format->width);
	mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_V_RES, format->height);

	return 0;
}

static int mchp_yuv2h264_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct mchp_yuv2h264_device *yuv2h264 = to_mchp_yuv2h264(subdev);
	struct v4l2_mbus_framefmt *format;

	/* Initialize with default formats */
	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SINK);
	*format = yuv2h264->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SOURCE);
	*format = yuv2h264->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_yuv2h264_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static int mchp_yuv2h264_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mchp_yuv2h264_device *yuv2h264 =
		container_of(ctrl->handler, struct mchp_yuv2h264_device,
			     ctrl_handler);

	switch (ctrl->id) {
	case MCHP_CID_Q_FACTOR:
		mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_Q_FACTOR, ctrl->val);
		return 0;
	case MCHP_CID_P_COUNT:
		mchp_yuv2h264_write(yuv2h264, MCHP_YUV2H264_I_P_FRAME_GAP, ctrl->val);
		return 0;
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops mchp_yuv2h264_ctrl_ops = {
	.s_ctrl	= mchp_yuv2h264_s_ctrl,
};

static const struct v4l2_subdev_video_ops mchp_yuv2h264_video_ops = {
	.s_stream = mchp_yuv2h264_s_stream,
};

static const struct v4l2_subdev_pad_ops mchp_yuv2h264_pad_ops = {
	.enum_mbus_code		= mvc_enum_mbus_code,
	.enum_frame_size	= mvc_enum_frame_size,
	.get_fmt		= mchp_yuv2h264_get_format,
	.set_fmt		= mchp_yuv2h264_set_format,
};

static const struct v4l2_subdev_ops mchp_yuv2h264_ops = {
	.video  = &mchp_yuv2h264_video_ops,
	.pad    = &mchp_yuv2h264_pad_ops,
};

static const struct v4l2_subdev_internal_ops mchp_yuv2h264_internal_ops = {
	.open	= mchp_yuv2h264_open,
	.close	= mchp_yuv2h264_close,
};

/*
 * Control Configs
 */
static struct v4l2_ctrl_config mchp_yuv2h264_ctrls[] = {
	{
		.ops	= &mchp_yuv2h264_ctrl_ops,
		.id	= MCHP_CID_Q_FACTOR,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Quality Factor",
		.min	= MCHP_Q_FACTOR_CTL_MIN,
		.max	= MCHP_Q_FACTOR_CTL_MAX,
		.def	= MCHP_Q_FACTOR_CTL_DEFAULT,
		.step	= MCHP_CTL_STEP,
	}, {
		.ops	= &mchp_yuv2h264_ctrl_ops,
		.id	= MCHP_CID_P_COUNT,
		.name	= "P frame count",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= MCHP_P_COUNT_CTL_MIN,
		.max	= MCHP_P_COUNT_CTL_MAX,
		.def	= MCHP_P_COUNT_CTL_DEFAULT,
		.step	= MCHP_CTL_STEP,
	},
};

static const struct media_entity_operations mchp_yuv2h264_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int mchp_yuv2h264_init_formats(struct mchp_yuv2h264_device *yuv2h264)
{
	struct device *dev = yuv2h264->dev;
	const struct mvideo_format *vip_format;

	vip_format = mvc_get_format_by_vf_code(MCHP_YUV2H264_DEF_IN_FORMAT);
	if (IS_ERR(vip_format))
		return dev_err_probe(dev, PTR_ERR(vip_format), "invalid format");

	yuv2h264->vip_formats[MVC_PAD_SINK] = vip_format;

	vip_format = mvc_get_format_by_vf_code(MCHP_YUV2H264_DEF_OUT_FORMAT);
	if (IS_ERR(vip_format))
		return dev_err_probe(dev, PTR_ERR(vip_format), "invalid format");

	yuv2h264->vip_formats[MVC_PAD_SOURCE] = vip_format;

	return 0;
}

static int mchp_yuv2h264_probe(struct platform_device *pdev)
{
	struct mchp_yuv2h264_device *yuv2h264;
	struct v4l2_subdev *subdev;
	struct v4l2_mbus_framefmt *default_format;
	int ret;

	yuv2h264 = devm_kzalloc(&pdev->dev, sizeof(*yuv2h264), GFP_KERNEL);
	if (!yuv2h264)
		return -ENOMEM;

	yuv2h264->dev = &pdev->dev;

	ret = mchp_yuv2h264_init_formats(yuv2h264);
	if (ret < 0)
		return ret;

	yuv2h264->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(yuv2h264->iomem))
		return dev_err_probe(&pdev->dev, PTR_ERR(yuv2h264->iomem),
				     "could not get mem resource\n");

	/* Initialize V4L2 subdevice and media entity */
	subdev = &yuv2h264->subdev;
	v4l2_subdev_init(subdev, &mchp_yuv2h264_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_yuv2h264_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, yuv2h264);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* Initialize default and active formats */
	default_format = &yuv2h264->default_formats[MVC_PAD_SINK];
	default_format->code = yuv2h264->vip_formats[MVC_PAD_SINK]->code;
	default_format->field = V4L2_FIELD_NONE;
	default_format->colorspace = V4L2_COLORSPACE_SRGB;
	default_format->width = MCHP_YUV2H264_DEFAULT_WIDTH;
	default_format->height = MCHP_YUV2H264_DEFAULT_HEIGHT;

	yuv2h264->formats[MVC_PAD_SINK] = *default_format;

	default_format = &yuv2h264->default_formats[MVC_PAD_SOURCE];
	*default_format = yuv2h264->default_formats[MVC_PAD_SINK];
	default_format->code = yuv2h264->vip_formats[MVC_PAD_SOURCE]->code;

	yuv2h264->formats[MVC_PAD_SOURCE] = *default_format;

	yuv2h264->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	yuv2h264->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &mchp_yuv2h264_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, yuv2h264->pads);
	if (ret < 0)
		return ret;

	v4l2_ctrl_handler_init(&yuv2h264->ctrl_handler, 2);

	yuv2h264->q_factor = v4l2_ctrl_new_custom(&yuv2h264->ctrl_handler,
						  &mchp_yuv2h264_ctrls[0], NULL);

	yuv2h264->p_count = v4l2_ctrl_new_custom(&yuv2h264->ctrl_handler,
						 &mchp_yuv2h264_ctrls[1], NULL);

	if (yuv2h264->ctrl_handler.error) {
		dev_err(&pdev->dev, "failed to add controls\n");
		ret = yuv2h264->ctrl_handler.error;
		goto media_error;
	}
	subdev->ctrl_handler = &yuv2h264->ctrl_handler;

	platform_set_drvdata(pdev, yuv2h264);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	v4l2_ctrl_handler_free(&yuv2h264->ctrl_handler);
media_error:
	media_entity_cleanup(&subdev->entity);
	return ret;
}

static int mchp_yuv2h264_remove(struct platform_device *pdev)
{
	struct mchp_yuv2h264_device *yuv2h264 = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &yuv2h264->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&yuv2h264->ctrl_handler);
	media_entity_cleanup(&subdev->entity);

	return 0;
}

static const struct of_device_id mchp_yuv2h264_of_id_table[] = {
	{ .compatible = "microchip,yuv2h264-rtl-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_yuv2h264_of_id_table);

static struct platform_driver mchp_yuv2h264_driver = {
	.driver			= {
		.name		= "microchip-yuv2h264",
		.of_match_table	= mchp_yuv2h264_of_id_table,
	},
	.probe			= mchp_yuv2h264_probe,
	.remove			= mchp_yuv2h264_remove,
};

module_platform_driver(mchp_yuv2h264_driver);

MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip H264 Encoder Driver");
MODULE_LICENSE("GPL");

