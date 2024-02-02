// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Video On Screen Display Driver.
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

#define MCHP_OSD_IP_VER			0x00
#define MCHP_OSD_CTRL			0x04
#define MCHP_OSD_CTRL_START		BIT(0)
#define MCHP_OSD_CTRL_RESET		BIT(1)

#define MCHP_OSD_X_Y_POS		0x08
#define MCHP_OSD_COLOR			0x0C
#define MCHP_OSD_NUM			0x10
#define MCHP_OSD_H_RES			0x14
#define MCHP_OSD_V_RES			0x18

/* Text overlay (On Screen Display) */
#define MCHP_OSD_X_Y_POS_MAX		4096
#define MCHP_OSD_X_Y_POS_MIN		4
#define MCHP_OSD_X_Y_POS_DEFAULT	4
#define MCHP_OSD_X_Y_POS_STEP		2
#define MCHP_OSD_ENABLE_MAX		1
#define MCHP_OSD_ENABLE_MIN		0
#define MCHP_OSD_ENABLE_DEFAULT		1
#define MCHP_OSD_ENABLE_STEP		1
#define MCHP_OSD_DISABLE_MAX_PIX	0x2000
#define MCHP_OSD_X_POS_SHIFT		16
#define MCHP_OSD_COLOR_MAX		0xFFFFFF
#define MCHP_OSD_COLOR_MIN		0x000000
#define MCHP_OSD_COLOR_DEFAULT		0xFFFFFF
#define MCHP_OSD_COLOR_STEP		1
#define MCHP_OSD_NUM_MAX		200
#define MCHP_OSD_NUM_MIN		1
#define MCHP_OSD_NUM_DEFAULT		30
#define MCHP_OSD_NUM_STEP		1

#define MCHP_OSD_NUM_CTRLS		5

#define MCHP_OSD_DEF_FORMAT		MVCF_RGB

/**
 * struct mchp_osd_device - Microchip On Screen Display device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: active V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @vip_formats: format information corresponding to the pads active formats
 * @ctrl_handler: control handler
 * @horizontal_pos:	overlay horizontal position
 * @vertical_pos:	overlay vertical position
 */
struct mchp_osd_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
	const struct mvideo_format *vip_formats[2];

	struct v4l2_ctrl_handler ctrl_handler;

	int horizontal_pos;
	int vertical_pos;
};

static inline struct mchp_osd_device *to_osd(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_osd_device, subdev);
}

static inline u32 mchp_osd_reg_read(struct mchp_osd_device *osd,
				    u32 addr)
{
	return readl(osd->iomem + addr);
}

static inline void mchp_osd_reg_write(struct mchp_osd_device *osd,
				      u32 addr, u32 value)
{
	writel(value, osd->iomem + addr);
}

static inline void mchp_osd_clr(struct mchp_osd_device *osd,
				u32 addr, u32 clr)
{
	mchp_osd_reg_write(osd, addr, mchp_osd_reg_read(osd, addr) & ~clr);
}

static inline void mchp_osd_set(struct mchp_osd_device *osd,
				u32 addr, u32 set)
{
	mchp_osd_reg_write(osd, addr, mchp_osd_reg_read(osd, addr) | set);
}

static int mchp_osd_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct mchp_osd_device *osd = to_osd(subdev);
	struct v4l2_mbus_framefmt *format = &osd->formats[MVC_PAD_SINK];

	if (!enable) {
		mchp_osd_clr(osd, MCHP_OSD_CTRL, MCHP_OSD_CTRL_START);
		return 0;
	}

	mchp_osd_reg_write(osd, MCHP_OSD_V_RES, format->height);
	mchp_osd_reg_write(osd, MCHP_OSD_H_RES, format->width);

	mchp_osd_set(osd, MCHP_OSD_CTRL, MCHP_OSD_CTRL_START);

	return 0;
}

static struct v4l2_mbus_framefmt *
__mchp_osd_get_pad_format(struct mchp_osd_device *osd,
			  struct v4l2_subdev_state *sd_state,
			  unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&osd->subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &osd->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int mchp_osd_get_format(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_format *fmt)
{
	struct mchp_osd_device *osd = to_osd(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_osd_get_pad_format(osd, sd_state,
					   fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int mchp_osd_set_format(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_format *fmt)
{
	struct mchp_osd_device *osd = to_osd(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_osd_get_pad_format(osd, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == MVC_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	mvc_set_format_size(format, fmt);

	fmt->format = *format;

	format = __mchp_osd_get_pad_format(osd, sd_state,
					   MVC_PAD_SOURCE, fmt->which);
	if (!format)
		return -EINVAL;

	mvc_set_format_size(format, fmt);

	mchp_osd_reg_write(osd, MCHP_OSD_V_RES, format->height);
	mchp_osd_reg_write(osd, MCHP_OSD_H_RES, format->width);

	return 0;
}

static int mchp_osd_open(struct v4l2_subdev *subdev,
			 struct v4l2_subdev_fh *fh)
{
	struct mchp_osd_device *osd = to_osd(subdev);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SINK);
	*format = osd->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, MVC_PAD_SOURCE);
	*format = osd->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_osd_close(struct v4l2_subdev *subdev,
			  struct v4l2_subdev_fh *fh)
{
	return 0;
}

static int mchp_osd_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mchp_osd_device *osd =
		container_of(ctrl->handler, struct mchp_osd_device,
			     ctrl_handler);
	u32 val;

	switch (ctrl->id) {
	case MCHP_CID_OSD_X_POS:
		osd->horizontal_pos = ctrl->val;
		val = osd->vertical_pos | (osd->horizontal_pos << MCHP_OSD_X_POS_SHIFT);
		mchp_osd_reg_write(osd, MCHP_OSD_X_Y_POS, val);
		break;
	case MCHP_CID_OSD_Y_POS:
		osd->vertical_pos = ctrl->val;
		val = osd->vertical_pos | (osd->horizontal_pos << MCHP_OSD_X_POS_SHIFT);
		mchp_osd_reg_write(osd, MCHP_OSD_X_Y_POS, val);
		break;
	case MCHP_CID_OSD_ENABLE:
		if (ctrl->val)
			mchp_osd_set(osd, MCHP_OSD_CTRL, MCHP_OSD_CTRL_START);
		else
			mchp_osd_clr(osd, MCHP_OSD_CTRL, MCHP_OSD_CTRL_START);
		break;
	case MCHP_CID_OSD_COLOR:
		mchp_osd_reg_write(osd, MCHP_OSD_COLOR, ctrl->val);
		break;
	case MCHP_CID_OSD_NUM:
		mchp_osd_reg_write(osd, MCHP_OSD_NUM, ctrl->val);
		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mchp_osd_ctrl_ops = {
	.s_ctrl	= mchp_osd_s_ctrl,
};

static const struct v4l2_subdev_video_ops mchp_osd_video_ops = {
	.s_stream = mchp_osd_s_stream,
};

static const struct v4l2_subdev_pad_ops mchp_osd_pad_ops = {
	.enum_mbus_code = mvc_enum_mbus_code,
	.get_fmt = mchp_osd_get_format,
	.set_fmt = mchp_osd_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mchp_osd_ops = {
	.video  = &mchp_osd_video_ops,
	.pad    = &mchp_osd_pad_ops,
};

static const struct v4l2_subdev_internal_ops mchp_osd_internal_ops = {
	.open = mchp_osd_open,
	.close = mchp_osd_close,
};

static const struct media_entity_operations mchp_osd_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static struct v4l2_ctrl_config mchp_osd_ctrls[] = {
	{
		.ops	= &mchp_osd_ctrl_ops,
		.id	= MCHP_CID_OSD_X_POS,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "OSDx Position",
		.min	= MCHP_OSD_X_Y_POS_MIN,
		.max	= MCHP_OSD_X_Y_POS_MAX,
		.def	= MCHP_OSD_X_Y_POS_DEFAULT,
		.step	= MCHP_OSD_X_Y_POS_STEP,
	}, {
		.ops	= &mchp_osd_ctrl_ops,
		.id	= MCHP_CID_OSD_Y_POS,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "OSDy Position",
		.min	= MCHP_OSD_X_Y_POS_MIN,
		.max	= MCHP_OSD_X_Y_POS_MAX,
		.def	= MCHP_OSD_X_Y_POS_DEFAULT,
		.step	= MCHP_OSD_X_Y_POS_STEP,
	}, {
		.ops	= &mchp_osd_ctrl_ops,
		.id	= MCHP_CID_OSD_ENABLE,
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.name	= "OSD enable",
		.min	= MCHP_OSD_ENABLE_MIN,
		.max	= MCHP_OSD_ENABLE_MAX,
		.def	= MCHP_OSD_ENABLE_DEFAULT,
		.step	= MCHP_OSD_ENABLE_STEP,
	}, {
		.ops	= &mchp_osd_ctrl_ops,
		.id	= MCHP_CID_OSD_COLOR,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "OSD color",
		.min	= MCHP_OSD_COLOR_MIN,
		.max	= MCHP_OSD_COLOR_MAX,
		.def	= MCHP_OSD_COLOR_DEFAULT,
		.step	= MCHP_OSD_COLOR_STEP,
	}, {
		.ops	= &mchp_osd_ctrl_ops,
		.id	= MCHP_CID_OSD_NUM,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "OSD num",
		.min	= MCHP_OSD_NUM_MIN,
		.max	= MCHP_OSD_NUM_MAX,
		.def	= MCHP_OSD_NUM_DEFAULT,
		.step	= MCHP_OSD_NUM_STEP,
	},
};

static int mchp_osd_init_controls(struct mchp_osd_device *osd)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &osd->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, MCHP_OSD_NUM_CTRLS);
	if (ret)
		return ret;

	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_osd_ctrls[0], NULL);
	osd->horizontal_pos =  MCHP_OSD_X_Y_POS_MIN;

	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_osd_ctrls[1], NULL);
	osd->vertical_pos =  MCHP_OSD_X_Y_POS_MIN;

	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_osd_ctrls[2], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_osd_ctrls[3], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_osd_ctrls[4], NULL);

	if (ctrl_hdlr->error) {
		dev_err(osd->dev, "control init failed: %d",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	osd->subdev.ctrl_handler = ctrl_hdlr;

	return 0;
}

static int mchp_osd_init_formats(struct mchp_osd_device *osd)
{
	struct device *dev = osd->dev;
	struct v4l2_mbus_framefmt *format;
	const struct mvideo_format *vip_format;

	vip_format = mvc_get_format_by_vf_code(MCHP_OSD_DEF_FORMAT);
	if (IS_ERR(vip_format))
		return dev_err_probe(dev, PTR_ERR(vip_format), "invalid format");

	osd->vip_formats[MVC_PAD_SINK] = vip_format;
	osd->vip_formats[MVC_PAD_SOURCE] = vip_format;

	/* Initialize default and active formats */
	format = &osd->default_formats[MVC_PAD_SINK];
	format->code = osd->vip_formats[MVC_PAD_SINK]->code;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	format->width = MVC_MAX_WIDTH;
	format->height = MVC_MAX_HEIGHT;

	osd->formats[MVC_PAD_SINK] = *format;

	format = &osd->default_formats[MVC_PAD_SOURCE];
	*format = osd->default_formats[MVC_PAD_SINK];
	format->code = osd->vip_formats[MVC_PAD_SOURCE]->code;

	osd->formats[MVC_PAD_SOURCE] = *format;

	return 0;
}

static int mchp_osd_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct mchp_osd_device *osd;
	int ret;

	osd = devm_kzalloc(&pdev->dev, sizeof(*osd), GFP_KERNEL);
	if (!osd)
		return -ENOMEM;

	osd->dev = &pdev->dev;

	osd->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(osd->iomem))
		return PTR_ERR(osd->iomem);

	mchp_osd_set(osd, MCHP_OSD_CTRL, MCHP_OSD_CTRL_RESET);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &osd->subdev;
	v4l2_subdev_init(subdev, &mchp_osd_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_osd_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, osd);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	ret = mchp_osd_init_formats(osd);
	if (ret < 0)
		return ret;

	mchp_osd_init_controls(osd);

	osd->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	osd->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &mchp_osd_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, osd->pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, osd);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error_media;
	}

	return 0;

error_media:
	media_entity_cleanup(&subdev->entity);
error:
	v4l2_ctrl_handler_free(&osd->ctrl_handler);

	return ret;
}

static int mchp_osd_remove(struct platform_device *pdev)
{
	struct mchp_osd_device *osd = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &osd->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&osd->ctrl_handler);
	media_entity_cleanup(&subdev->entity);

	return 0;
}

static const struct of_device_id mchp_osd_of_id_table[] = {
	{ .compatible = "microchip,osd-rtl-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_osd_of_id_table);

static struct platform_driver mchp_osd_driver = {
	.driver = {
		.name = "microchip-osd",
		.of_match_table = mchp_osd_of_id_table,
	},
	.probe = mchp_osd_probe,
	.remove = mchp_osd_remove,
};

module_platform_driver(mchp_osd_driver);

MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip Video On Screen Display Driver");
MODULE_LICENSE("GPL");
