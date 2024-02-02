// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Video Image Enhancement Driver.
 *
 * Copyright (C) 2021-2022 Microchip Technology Inc. and its subsidiaries
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

#include <dt-bindings/media/microchip-common.h>

#include "microchip-common.h"

#define MCHP_IMAGE_ENHANCEMENT_IP_VER			0x00
#define MCHP_IMAGE_ENHANCEMENT_CTRL			0x04
#define MCHP_IMAGE_ENHANCEMENT_CTRL_START		BIT(0)
#define MCHP_IMAGE_ENHANCEMENT_CTRL_RESET		BIT(1)

#define MCHP_IMAGE_ENHANCEMENT_R_CONSTRAINT		0x08
#define MCHP_IMAGE_ENHANCEMENT_G_CONSTRAINT		0x0C
#define MCHP_IMAGE_ENHANCEMENT_B_CONSTRAINT		0x10
#define MCHP_IMAGE_ENHANCEMENT_SECOND_CONSTRAINT		0x14
#define MCHP_IMAGE_ENHANCEMENT_RGB_SUM			0x18

#define MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_DEFAULT		112
#define MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_MAX		255
#define MCHP_IMAGE_ENHANCEMENT_CTL_MIN			0
#define MCHP_IMAGE_ENHANCEMENT_CTL_MAX			255
#define MCHP_IMAGE_ENHANCEMENT_CTL_STEP			1
#define MCHP_IMAGE_ENHANCEMENT_Q_FACTOR_CTL_MAX		52

#define MCHP_IMAGE_ENHANCEMENT_NUM_CTRLS		7

#define MCHP_IMAGE_ENHANCEMENT_DEF_FORMAT		MVCF_RGB

#define MCHP_IMAGE_ENHANCEMENT_RGB_SUM_MAX		0xF27CB22F
#define MCHP_IMAGE_ENHANCEMENT_RGB_SUM_MIN		0x100000
#define MCHP_IMAGE_ENHANCEMENT_RGB_SUM_DEF		0x127CB22F

/**
 * struct mchp_image_enhancement - Microchip image enhancement device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: active V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @vip_formats: format information corresponding to the pads active formats
 * @ctrl_handler: control handler
 */
struct mchp_image_enhancement {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
	const struct mvideo_format *vip_formats[2];

	struct v4l2_ctrl_handler ctrl_handler;

	int contrast;
	int brightness;
};

static inline struct mchp_image_enhancement *
to_image_enhancement(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_image_enhancement, subdev);
}

static inline u32
mchp_image_enhancement_reg_read(struct mchp_image_enhancement *image_enhancement,
				u32 addr)
{
	return ioread32(image_enhancement->iomem + addr);
}

static inline void
mchp_image_enhancement_reg_write(struct mchp_image_enhancement *image_enhancement,
				 u32 addr, u32 value)
{
	iowrite32(value, image_enhancement->iomem + addr);
}

static inline void
mchp_image_enhancement_clr(struct mchp_image_enhancement *image_enhancement,
			   u32 addr, u32 clr)
{
	u32 val;

	val = mchp_image_enhancement_reg_read(image_enhancement, addr) & ~clr;
	mchp_image_enhancement_reg_write(image_enhancement, addr, val);
}

static inline void
mchp_image_enhancement_set(struct mchp_image_enhancement *image_enhancement,
			   u32 addr, u32 set)
{
	u32 val;

	val = mchp_image_enhancement_reg_read(image_enhancement, addr) | set;
	mchp_image_enhancement_reg_write(image_enhancement, addr, val);
}

static int mchp_image_enhancement_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct mchp_image_enhancement *image_enhancement = to_image_enhancement(subdev);

	mchp_image_enhancement_set(image_enhancement, MCHP_IMAGE_ENHANCEMENT_CTRL,
				   MCHP_IMAGE_ENHANCEMENT_CTRL_RESET);

	if (!enable) {
		mchp_image_enhancement_clr(image_enhancement, MCHP_IMAGE_ENHANCEMENT_CTRL,
					   MCHP_IMAGE_ENHANCEMENT_CTRL_START);
		return 0;
	}

	mchp_image_enhancement_set(image_enhancement, MCHP_IMAGE_ENHANCEMENT_CTRL,
				   MCHP_IMAGE_ENHANCEMENT_CTRL_START);

	return 0;
}

static struct v4l2_mbus_framefmt *
__mchp_image_enhancement_get_pad_format(struct mchp_image_enhancement *image_enhancement,
					struct v4l2_subdev_state *sd_state,
					unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&image_enhancement->subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &image_enhancement->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int mchp_image_enhancement_get_format(struct v4l2_subdev *subdev,
					     struct v4l2_subdev_state *sd_state,
					     struct v4l2_subdev_format *fmt)
{
	struct mchp_image_enhancement *image_enhancement = to_image_enhancement(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_image_enhancement_get_pad_format(image_enhancement, sd_state,
							 fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int mchp_image_enhancement_set_format(struct v4l2_subdev *subdev,
					     struct v4l2_subdev_state *sd_state,
					     struct v4l2_subdev_format *fmt)
{
	struct mchp_image_enhancement *image_enhancement = to_image_enhancement(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_image_enhancement_get_pad_format(image_enhancement, sd_state,
							 fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == MVC_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	mvc_set_format_size(format, fmt);

	fmt->format = *format;

	format = __mchp_image_enhancement_get_pad_format(image_enhancement, sd_state,
							 MVC_PAD_SOURCE,
							 fmt->which);
	if (!format)
		return -EINVAL;

	mvc_set_format_size(format, fmt);

	return 0;
}

static int mchp_image_enhancement_open(struct v4l2_subdev *subdev,
				       struct v4l2_subdev_fh *fh)
{
	struct mchp_image_enhancement *image_enhancement = to_image_enhancement(subdev);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SINK);
	*format = image_enhancement->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SOURCE);
	*format = image_enhancement->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_image_enhancement_close(struct v4l2_subdev *subdev,
					struct v4l2_subdev_fh *fh)
{
	return 0;
}

/*
 * The calculated parameters are used by Image Enhancement IP UG0646 running
 * in the FPGA logic. The equations used for calculation are explained in
 * UG0646 page number 2 in the below link
 *
 * (https://ww1.microchip.com/downloads/aemDocuments/documents/FPGA/ProductDocuments/UserGuides/microsemi_image_enhancement_ip_user_guide_ug0646_v3.pdf)
 *
 * V4l2 controls like brightness, contrast, r_gain, g_gain and b_gain are
 * calulated based on two functions second_constraint_cal() and
 * contrast_scale_cal()
 */

static inline int second_constraint_cal(int brightness, int contrast_scale)
{
	return (128 * ((brightness) - ((128 * (contrast_scale)) / 10)));
}

static inline int contrast_scale_cal(int contrast)
{
	return ((325 * (contrast + 128) / (387 - contrast)) >> 5u);
}

static int mchp_image_enhancement_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mchp_image_enhancement *image_enhancement =
		container_of(ctrl->handler, struct mchp_image_enhancement,
			     ctrl_handler);
	u32 contrast_scale, second_constraint, r_gain, g_gain, b_gain;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		image_enhancement->brightness = ctrl->val;
		contrast_scale = contrast_scale_cal(image_enhancement->contrast);
		second_constraint = second_constraint_cal(ctrl->val,
							  contrast_scale);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_SECOND_CONSTRAINT,
						 second_constraint);
		break;
	case V4L2_CID_CONTRAST:
		image_enhancement->contrast = ctrl->val;
		contrast_scale = contrast_scale_cal(ctrl->val);
		second_constraint = second_constraint_cal(image_enhancement->brightness,
							  contrast_scale);

		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_SECOND_CONSTRAINT,
						 second_constraint);

		break;
	case MCHP_CID_RED_GAIN:
		contrast_scale = contrast_scale_cal(image_enhancement->contrast);
		r_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_R_CONSTRAINT,
						 r_gain);
		break;
	case MCHP_CID_GREEN_GAIN:
		contrast_scale = contrast_scale_cal(image_enhancement->contrast);
		g_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_G_CONSTRAINT,
						 g_gain);
		break;
	case MCHP_CID_BLUE_GAIN:
		contrast_scale = contrast_scale_cal(image_enhancement->contrast);
		b_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_B_CONSTRAINT,
						 b_gain);
		break;
	case V4L2_CID_GAIN:
		contrast_scale = contrast_scale_cal(image_enhancement->contrast);
		r_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_R_CONSTRAINT,
						 r_gain);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_G_CONSTRAINT,
						 r_gain);
		mchp_image_enhancement_reg_write(image_enhancement,
						 MCHP_IMAGE_ENHANCEMENT_B_CONSTRAINT,
						 r_gain);
		break;
	case MCHP_CID_RGB_SUM:
		ctrl->val = mchp_image_enhancement_reg_read(image_enhancement,
							    MCHP_IMAGE_ENHANCEMENT_RGB_SUM);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mchp_image_enhancement_ctrl_ops = {
	.s_ctrl	= mchp_image_enhancement_s_ctrl,
};

static const struct v4l2_subdev_video_ops mchp_image_enhancement_video_ops = {
	.s_stream = mchp_image_enhancement_s_stream,
};

static const struct v4l2_subdev_pad_ops mchp_image_enhancement_pad_ops = {
	.enum_mbus_code = mvc_enum_mbus_code,
	.get_fmt = mchp_image_enhancement_get_format,
	.set_fmt = mchp_image_enhancement_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mchp_image_enhancement_ops = {
	.video  = &mchp_image_enhancement_video_ops,
	.pad    = &mchp_image_enhancement_pad_ops,
};

static const struct v4l2_subdev_internal_ops mchp_image_enhancement_internal_ops = {
	.open = mchp_image_enhancement_open,
	.close = mchp_image_enhancement_close,
};

static const struct media_entity_operations mchp_image_enhancement_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static struct v4l2_ctrl_config mchp_image_enhancement_ctrls[] = {
	{
		.ops	= &mchp_image_enhancement_ctrl_ops,
		.id	= MCHP_CID_RED_GAIN,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Gain, Red",
		.min	= MCHP_IMAGE_ENHANCEMENT_CTL_MIN,
		.max	= MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_MAX,
		.def	= MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_DEFAULT,
		.step	= MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
	}, {
		.ops	= &mchp_image_enhancement_ctrl_ops,
		.id	= MCHP_CID_GREEN_GAIN,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Gain, Green",
		.min	= MCHP_IMAGE_ENHANCEMENT_CTL_MIN,
		.max	= MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_MAX,
		.def	= MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_DEFAULT,
		.step	= MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
	}, {
		.ops	= &mchp_image_enhancement_ctrl_ops,
		.id	= MCHP_CID_BLUE_GAIN,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Gain, Blue",
		.min	= MCHP_IMAGE_ENHANCEMENT_CTL_MIN,
		.max	= MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_MAX,
		.def	= MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_DEFAULT,
		.step	= MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
	}, {
		.ops	= &mchp_image_enhancement_ctrl_ops,
		.id	= MCHP_CID_RGB_SUM,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Rgb, Sum",
		.min	= MCHP_IMAGE_ENHANCEMENT_RGB_SUM_MIN,
		.max	= MCHP_IMAGE_ENHANCEMENT_RGB_SUM_MAX,
		.def	= MCHP_IMAGE_ENHANCEMENT_RGB_SUM_DEF,
		.step	= MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
	},
};

static int mchp_image_enhancement_init_controls(struct mchp_image_enhancement *image_enhancement)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &image_enhancement->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, MCHP_IMAGE_ENHANCEMENT_NUM_CTRLS);
	if (ret)
		return ret;

	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_image_enhancement_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, MCHP_IMAGE_ENHANCEMENT_CTL_MIN,
			  MCHP_IMAGE_ENHANCEMENT_CTL_MAX, MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
			  MCHP_IMAGE_ENHANCEMENT_CTL_MAX / 2);

	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_image_enhancement_ctrl_ops,
			  V4L2_CID_CONTRAST, MCHP_IMAGE_ENHANCEMENT_CTL_MIN,
			  MCHP_IMAGE_ENHANCEMENT_CTL_MAX, MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
			  MCHP_IMAGE_ENHANCEMENT_CTL_MAX / 2);

	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_image_enhancement_ctrl_ops,
			  V4L2_CID_GAIN, MCHP_IMAGE_ENHANCEMENT_CTL_MIN,
			  MCHP_IMAGE_ENHANCEMENT_CTL_MAX, MCHP_IMAGE_ENHANCEMENT_CTL_STEP,
			  MCHP_IMAGE_ENHANCEMENT_GAIN_CTL_DEFAULT);

	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_image_enhancement_ctrls[0], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_image_enhancement_ctrls[1], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_image_enhancement_ctrls[2], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_image_enhancement_ctrls[3], NULL);

	if (ctrl_hdlr->error) {
		dev_err(image_enhancement->dev, "control init failed: %d",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	image_enhancement->subdev.ctrl_handler = ctrl_hdlr;

	return 0;
}

static int mchp_image_enhancement_init_formats(struct mchp_image_enhancement *image_enhancement)
{
	struct device *dev = image_enhancement->dev;
	struct v4l2_mbus_framefmt *format;
	const struct mvideo_format *vip_format;

	vip_format = mvc_get_format_by_vf_code(MCHP_IMAGE_ENHANCEMENT_DEF_FORMAT);
	if (IS_ERR(vip_format))
		return dev_err_probe(dev, PTR_ERR(vip_format), "invalid format");

	image_enhancement->vip_formats[MVC_PAD_SINK] = vip_format;
	image_enhancement->vip_formats[MVC_PAD_SOURCE] = vip_format;

	/* Initialize default and active formats */
	format = &image_enhancement->default_formats[MVC_PAD_SINK];
	format->code = image_enhancement->vip_formats[MVC_PAD_SINK]->code;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	format->width = MVC_MAX_WIDTH;
	format->height = MVC_MAX_HEIGHT;

	image_enhancement->formats[MVC_PAD_SINK] = *format;

	format = &image_enhancement->default_formats[MVC_PAD_SOURCE];
	*format = image_enhancement->default_formats[MVC_PAD_SINK];
	format->code = image_enhancement->vip_formats[MVC_PAD_SOURCE]->code;

	image_enhancement->formats[MVC_PAD_SOURCE] = *format;

	return 0;
}

static int mchp_image_enhancement_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct mchp_image_enhancement *image_enhancement;
	int ret;

	image_enhancement = devm_kzalloc(&pdev->dev, sizeof(*image_enhancement), GFP_KERNEL);
	if (!image_enhancement)
		return -ENOMEM;

	image_enhancement->dev = &pdev->dev;

	image_enhancement->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(image_enhancement->iomem))
		return PTR_ERR(image_enhancement->iomem);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &image_enhancement->subdev;
	v4l2_subdev_init(subdev, &mchp_image_enhancement_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_image_enhancement_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, image_enhancement);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	ret = mchp_image_enhancement_init_formats(image_enhancement);
	if (ret < 0)
		return ret;

	mchp_image_enhancement_init_controls(image_enhancement);

	mchp_image_enhancement_set(image_enhancement, MCHP_IMAGE_ENHANCEMENT_CTRL,
				   MCHP_IMAGE_ENHANCEMENT_CTRL_RESET);

	image_enhancement->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	image_enhancement->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &mchp_image_enhancement_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, image_enhancement->pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, image_enhancement);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error_media;
	}

	return 0;

error_media:
	media_entity_cleanup(&subdev->entity);
error:
	v4l2_ctrl_handler_free(&image_enhancement->ctrl_handler);

	return ret;
}

static int mchp_image_enhancement_remove(struct platform_device *pdev)
{
	struct mchp_image_enhancement *image_enhancement = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &image_enhancement->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&image_enhancement->ctrl_handler);
	media_entity_cleanup(&subdev->entity);

	return 0;
}

static const struct of_device_id mchp_image_enhancement_of_id_table[] = {
	{ .compatible = "microchip,image-enhancement-rtl-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_image_enhancement_of_id_table);

static struct platform_driver mchp_image_enhancement_driver = {
	.driver = {
		.name = "microchip-image-enhancement",
		.of_match_table = mchp_image_enhancement_of_id_table,
	},
	.probe = mchp_image_enhancement_probe,
	.remove = mchp_image_enhancement_remove,
};

module_platform_driver(mchp_image_enhancement_driver);

MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip Video Image Enhancement Driver");
MODULE_LICENSE("GPL");
