// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Video White Balance Driver.
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

#define MCHP_WB_IP_VER			0x00
#define MCHP_WB_CTRL			0x04
#define MCHP_WB_CTRL_START		BIT(0)
#define MCHP_WB_CTRL_RESET		BIT(1)

/*
 * Input luminance index varies between 0 to 255. A value of 0
 * corresponds to temperature of 1000K and each increment
 * corresponds to a temperature rise of 100 Kelvin.
 */
#define MCHP_WB_LUMINANCE		0x08

#define MCHP_WB_LUMINANCE_MAX		256
#define MCHP_WB_LUMINANCE_MIN		0
#define MCHP_WB_LUMINANCE_DEFAULT	30
#define MCHP_WB_LUMINANCE_STEP		1

#define MCHP_WB_NUM_CTRLS		1

#define MCHP_WB_DEF_FORMAT		MVCF_RGB

#define MCHP_CID_WB_LUMINANCE		(V4L2_CID_USER_BASE | 0x100E)

/**
 * struct mchp_white_balance_device - Microchip white balance device structure
 * @dev: device
 * @subdev: The v4l2 subdev structure
 * @iomem: Base address of subsystem
 * @pads: media pads
 * @formats: active V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @vip_formats: format information corresponding to the pads active formats
 * @ctrl_handler: control handler
 */
struct mchp_white_balance_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	void __iomem *iomem;

	struct media_pad pads[2];

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
	const struct mvideo_format *vip_formats[2];

	struct v4l2_ctrl_handler ctrl_handler;
};

static inline struct mchp_white_balance_device *to_wb(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct mchp_white_balance_device, subdev);
}

static inline u32 mchp_white_balance_reg_read(struct mchp_white_balance_device *wb,
					      u32 addr)
{
	return readl(wb->iomem + addr);
}

static inline void mchp_white_balance_reg_write(struct mchp_white_balance_device *wb,
						u32 addr, u32 value)
{
	writel(value, wb->iomem + addr);
}

static inline void mchp_white_balance_clr(struct mchp_white_balance_device *wb,
					  u32 addr, u32 clr)
{
	mchp_white_balance_reg_write(wb, addr, mchp_white_balance_reg_read(wb, addr) & ~clr);
}

static inline void mchp_white_balance_set(struct mchp_white_balance_device *wb,
					  u32 addr, u32 set)
{
	mchp_white_balance_reg_write(wb, addr, mchp_white_balance_reg_read(wb, addr) | set);
}

static int mchp_white_balance_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct mchp_white_balance_device *wb = to_wb(subdev);
	int ret;

	if (!enable) {
		mchp_white_balance_clr(wb, MCHP_WB_CTRL, MCHP_WB_CTRL_START);
		return 0;
	}

	mchp_white_balance_set(wb, MCHP_WB_CTRL, MCHP_WB_CTRL_RESET);

	ret = __v4l2_ctrl_handler_setup(&wb->ctrl_handler);
	if (ret)
		return ret;

	mchp_white_balance_set(wb, MCHP_WB_CTRL, MCHP_WB_CTRL_START);

	return 0;
}

static struct v4l2_mbus_framefmt *
__mchp_white_balance_get_pad_format(struct mchp_white_balance_device *wb,
				    struct v4l2_subdev_state *sd_state,
				    unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_state_get_format(sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &wb->formats[pad];
	default:
		return NULL;
	}
}

static int mchp_white_balance_get_format(struct v4l2_subdev *subdev,
					 struct v4l2_subdev_state *sd_state,
					 struct v4l2_subdev_format *fmt)
{
	struct mchp_white_balance_device *wb = to_wb(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_white_balance_get_pad_format(wb, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int mchp_white_balance_set_format(struct v4l2_subdev *subdev,
					 struct v4l2_subdev_state *sd_state,
					 struct v4l2_subdev_format *fmt)
{
	struct mchp_white_balance_device *wb = to_wb(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __mchp_white_balance_get_pad_format(wb, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == MVC_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	mvc_set_format_size(format, fmt);

	fmt->format = *format;

	format = __mchp_white_balance_get_pad_format(wb, sd_state, MVC_PAD_SOURCE, fmt->which);
	if (!format)
		return -EINVAL;

	mvc_set_format_size(format, fmt);

	return 0;
}

static int mchp_white_balance_open(struct v4l2_subdev *subdev,
				   struct v4l2_subdev_fh *fh)
{
	struct mchp_white_balance_device *wb = to_wb(subdev);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(fh->state, MVC_PAD_SINK);
	*format = wb->default_formats[MVC_PAD_SINK];

	format = v4l2_subdev_state_get_format(fh->state, MVC_PAD_SOURCE);
	*format = wb->default_formats[MVC_PAD_SOURCE];

	return 0;
}

static int mchp_white_balance_close(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_fh *fh)
{
	return 0;
}

static int mchp_white_balance_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mchp_white_balance_device *wb =
		container_of(ctrl->handler, struct mchp_white_balance_device,
			     ctrl_handler);
	int ret = 0;

	switch (ctrl->id) {
	case MCHP_CID_WB_LUMINANCE:
		mchp_white_balance_reg_write(wb, MCHP_WB_LUMINANCE, ctrl->val);
		break;
	default:
		dev_err(wb->dev, "Invalid control %d", ctrl->id);
		ret = -EINVAL;
	}

	return ret;
}

static const struct v4l2_ctrl_ops mchp_white_balance_ctrl_ops = {
	.s_ctrl	= mchp_white_balance_s_ctrl,
};

static const struct v4l2_subdev_video_ops mchp_white_balance_video_ops = {
	.s_stream = mchp_white_balance_s_stream,
};

static const struct v4l2_subdev_pad_ops mchp_white_balance_pad_ops = {
	.enum_mbus_code = mvc_enum_mbus_code,
	.get_fmt = mchp_white_balance_get_format,
	.set_fmt = mchp_white_balance_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mchp_white_balance_ops = {
	.video  = &mchp_white_balance_video_ops,
	.pad    = &mchp_white_balance_pad_ops,
};

static const struct v4l2_subdev_internal_ops mchp_white_balance_internal_ops = {
	.open = mchp_white_balance_open,
	.close = mchp_white_balance_close,
};

static struct v4l2_ctrl_config mchp_white_balance_ctrls[] = {
	{
		.ops	= &mchp_white_balance_ctrl_ops,
		.id	= MCHP_CID_WB_LUMINANCE,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "WB Luminance",
		.min	= MCHP_WB_LUMINANCE_MIN,
		.max	= MCHP_WB_LUMINANCE_MAX,
		.def	= MCHP_WB_LUMINANCE_DEFAULT,
		.step	= MCHP_WB_LUMINANCE_STEP,
	},
};

static int mchp_white_balance_init_controls(struct mchp_white_balance_device *wb)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &wb->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, MCHP_WB_NUM_CTRLS);
	if (ret)
		return ret;

	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_white_balance_ctrls[0], NULL);

	if (ctrl_hdlr->error) {
		dev_err(wb->dev, "control init failed: %d",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	wb->subdev.ctrl_handler = ctrl_hdlr;

	return 0;
}

static int mchp_white_balance_init_formats(struct mchp_white_balance_device *wb)
{
	struct device *dev = wb->dev;
	struct v4l2_mbus_framefmt *format;
	const struct mvideo_format *vip_format;

	vip_format = mvc_get_format_by_vf_code(MCHP_WB_DEF_FORMAT);
	if (IS_ERR(vip_format))
		return dev_err_probe(dev, PTR_ERR(vip_format), "invalid format");

	wb->vip_formats[MVC_PAD_SINK] = vip_format;
	wb->vip_formats[MVC_PAD_SOURCE] = vip_format;

	/* Initialize default and active formats */
	format = &wb->default_formats[MVC_PAD_SINK];
	format->code = wb->vip_formats[MVC_PAD_SINK]->code;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	format->width = MVC_MAX_WIDTH;
	format->height = MVC_MAX_HEIGHT;

	wb->formats[MVC_PAD_SINK] = *format;

	format = &wb->default_formats[MVC_PAD_SOURCE];
	*format = wb->default_formats[MVC_PAD_SINK];
	format->code = wb->vip_formats[MVC_PAD_SOURCE]->code;

	wb->formats[MVC_PAD_SOURCE] = *format;

	return 0;
}

static int mchp_white_balance_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct mchp_white_balance_device *wb;
	int ret;

	wb = devm_kzalloc(&pdev->dev, sizeof(*wb), GFP_KERNEL);
	if (!wb)
		return -ENOMEM;

	wb->dev = &pdev->dev;

	wb->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wb->iomem))
		return PTR_ERR(wb->iomem);

	mchp_white_balance_set(wb, MCHP_WB_CTRL, MCHP_WB_CTRL_RESET);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &wb->subdev;
	v4l2_subdev_init(subdev, &mchp_white_balance_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &mchp_white_balance_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, wb);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	ret = mchp_white_balance_init_formats(wb);
	if (ret < 0)
		return ret;

	mchp_white_balance_init_controls(wb);

	wb->pads[MVC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	wb->pads[MVC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&subdev->entity, 2, wb->pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, wb);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error_media;
	}

	return 0;

error_media:
	media_entity_cleanup(&subdev->entity);
error:
	v4l2_ctrl_handler_free(&wb->ctrl_handler);

	return ret;
}

static void mchp_white_balance_remove(struct platform_device *pdev)
{
	struct mchp_white_balance_device *wb = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &wb->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&wb->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id mchp_white_balance_of_id_table[] = {
	{ .compatible = "microchip,white-balance-rtl-v1.1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_white_balance_of_id_table);

static struct platform_driver mchp_white_balance_driver = {
	.driver = {
		.name = "microchip-white-balance",
		.of_match_table = mchp_white_balance_of_id_table,
	},
	.probe = mchp_white_balance_probe,
	.remove = mchp_white_balance_remove,
};

module_platform_driver(mchp_white_balance_driver);

MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_DESCRIPTION("Microchip Video White Balance Driver");
MODULE_LICENSE("GPL");
