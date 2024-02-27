// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI CSI-2 Host controller driver.
 * Platform driver
 *
 * Author: Luis Oliveira <luis.oliveira@synopsys.com>
 */

#include <media/dwc/dw-csi-data.h>
#include <media/dwc/dw-dphy-data.h>

#include "dw-csi-plat.h"
#include <linux/clk.h>

const struct mipi_dt csi_dt[] = {
	{
		.hex = CSI_2_YUV420_8,
		.name = "YUV420_8bits",
	}, {
		.hex = CSI_2_YUV420_10,
		.name = "YUV420_10bits",
	}, {
		.hex = CSI_2_YUV420_8_LEG,
		.name = "YUV420_8bits_LEGACY",
	}, {
		.hex = CSI_2_YUV420_8_SHIFT,
		.name = "YUV420_8bits_SHIFT",
	}, {
		.hex = CSI_2_YUV420_10_SHIFT,
		.name = "YUV420_10bits_SHIFT",
	}, {
		.hex = CSI_2_YUV422_8,
		.name = "YUV442_8bits",
	}, {
		.hex = CSI_2_YUV422_10,
		.name = "YUV442_10bits",
	}, {
		.hex = CSI_2_RGB444,
		.name = "RGB444",
	}, {
		.hex = CSI_2_RGB555,
		.name = "RGB555",
	}, {
		.hex = CSI_2_RGB565,
		.name = "RGB565",
	}, {
		.hex = CSI_2_RGB666,
		.name = "RGB666",
	}, {
		.hex = CSI_2_RGB888,
		.name = "RGB888",
	}, {
		.hex = CSI_2_RAW6,
		.name = "RAW6",
	}, {
		.hex = CSI_2_RAW7,
		.name = "RAW7",
	}, {
		.hex = CSI_2_RAW8,
		.name = "RAW8",
	}, {
		.hex = CSI_2_RAW10,
		.name = "RAW10",
	}, {
		.hex = CSI_2_RAW12,
		.name = "RAW12",
	}, {
		.hex = CSI_2_RAW14,
		.name = "RAW14",
	}, {
		.hex = CSI_2_RAW16,
		.name = "RAW16",
	},
};

static struct mipi_fmt *
find_dw_mipi_csi_format(struct v4l2_mbus_framefmt *mf)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dw_mipi_csi_formats); i++)
		if (mf->code == dw_mipi_csi_formats[i].mbus_code)
			return &dw_mipi_csi_formats[i];

	return NULL;
}

static int dw_mipi_csi_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(dw_mipi_csi_formats))
		return -EINVAL;

	code->code = dw_mipi_csi_formats[code->index].mbus_code;
	return 0;
}

static struct mipi_fmt *
dw_mipi_csi_try_format(struct v4l2_mbus_framefmt *mf)
{
	struct mipi_fmt *fmt;

	fmt = find_dw_mipi_csi_format(mf);
	if (!fmt)
		fmt = &dw_mipi_csi_formats[0];

	mf->code = fmt->mbus_code;

	return fmt;
}

static struct v4l2_mbus_framefmt *
dw_mipi_csi_get_format(struct dw_csi *dev, struct v4l2_subdev_state *sd_state,
		       enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return sd_state->pads ? v4l2_subdev_get_try_format(&dev->sd,
							sd_state,
							0) : NULL;
	dev_dbg(dev->dev,
		"%s got v4l2_mbus_pixelcode. 0x%x\n", __func__,
		dev->format.code);
	dev_dbg(dev->dev,
		"%s got width. 0x%x\n", __func__,
		dev->format.width);
	dev_dbg(dev->dev,
		"%s got height. 0x%x\n", __func__,
		dev->format.height);
	return &dev->format;
}

static int
dw_mipi_csi_set_fmt(struct v4l2_subdev *sd,
		    struct v4l2_subdev_state *sd_state,
		    struct v4l2_subdev_format *fmt)
{
	struct dw_csi *dev = sd_to_mipi_csi_dev(sd);
	struct mipi_fmt *dev_fmt;
	struct v4l2_mbus_framefmt *mf = dw_mipi_csi_get_format(dev, sd_state,
							       fmt->which);
	int i;

	dev_fmt = dw_mipi_csi_try_format(&fmt->format);

	if (dev_fmt) {
		*mf = fmt->format;
		if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
			dev->fmt = dev_fmt;
		dw_mipi_csi_set_ipi_fmt(dev);
	}

	if (fmt->format.width > 0 && fmt->format.height > 0) {
		dw_mipi_csi_fill_timings(dev, fmt);
	} else {
		dev_vdbg(dev->dev, "%s unacceptable values 0x%x.\n",
			 __func__, fmt->format.width);
		dev_vdbg(dev->dev, "%s unacceptable values 0x%x.\n",
			 __func__, fmt->format.height);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(csi_dt); i++)
		if (csi_dt[i].hex == dev->ipi_dt) {
			dev_vdbg(dev->dev, "Using data type %s\n",
				 csi_dt[i].name);
		}
	return 0;
}

static int
dw_mipi_csi_get_fmt(struct v4l2_subdev *sd,
		    struct v4l2_subdev_state *sd_state,
		    struct v4l2_subdev_format *fmt)
{
	struct dw_csi *dev = sd_to_mipi_csi_dev(sd);
	struct v4l2_mbus_framefmt *mf;

	mf = dw_mipi_csi_get_format(dev, sd_state, fmt->which);
	if (!mf)
		return -EINVAL;

	mutex_lock(&dev->lock);
	fmt->format = *mf;
	mutex_unlock(&dev->lock);

	return 0;
}

static int
dw_mipi_csi_log_status(struct v4l2_subdev *sd)
{
	struct dw_csi *dev = sd_to_mipi_csi_dev(sd);

	dw_mipi_csi_dump(dev);

	return 0;
}

#if IS_ENABLED(CONFIG_VIDEO_ADV_DEBUG)
static int
dw_mipi_csi_g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
	struct dw_csi *dev = sd_to_mipi_csi_dev(sd);

	dev_vdbg(dev->dev, "%s: reg=%llu\n", __func__, reg->reg);
	reg->val = dw_mipi_csi_read(dev, reg->reg);

	return 0;
}
#endif

static int dw_mipi_csi_init_cfg(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *format =
	    v4l2_subdev_get_try_format(sd, sd_state, 0);

	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->code = MEDIA_BUS_FMT_RGB888_1X24;
	format->field = V4L2_FIELD_NONE;

	return 0;
}

static int dw_mipi_csi_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct dw_csi *dev = sd_to_mipi_csi_dev(sd);
	int ret = v4l2_subdev_call(dev->input_sd, video, s_stream, enable);

	if (enable) {
		dw_mipi_csi_hw_stdby(dev);
		dw_mipi_csi_start(dev);
	}  else {
		phy_power_off(dev->phy);
		dw_mipi_csi_mask_irq_power_off(dev);
		/* reset data type */
		dev->ipi_dt = 0x0;
	}
	return ret;
}

static const struct v4l2_subdev_core_ops dw_mipi_csi_core_ops = {
	.log_status = dw_mipi_csi_log_status,
#if IS_ENABLED(CONFIG_VIDEO_ADV_DEBUG)
	.g_register = dw_mipi_csi_g_register,
#endif
};

static int dw_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
			      struct v4l2_mbus_config *cfg)
{
	cfg->bus.mipi_csi2.num_data_lanes = 2;
	cfg->type = V4L2_MBUS_CSI2_DPHY;

	return 0;
}

static int dw_enum_frame_size(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;

	fse->min_width = 16;
	fse->max_width = 4000;
	fse->min_height = 16;
	fse->max_height = 3000;

	return 0;
}

static struct v4l2_subdev_pad_ops dw_mipi_csi_pad_ops = {
	.init_cfg = dw_mipi_csi_init_cfg,
	.enum_mbus_code = dw_mipi_csi_enum_mbus_code,
	.enum_frame_size = dw_enum_frame_size,
	.get_fmt = dw_mipi_csi_get_fmt,
	.set_fmt = dw_mipi_csi_set_fmt,
	.get_mbus_config = dw_get_mbus_config,
};

static const struct v4l2_subdev_video_ops dw_mipi_csi_video_ops = {
	.s_stream = dw_mipi_csi_s_stream,
};

static const struct v4l2_subdev_ops dw_mipi_csi_subdev_ops = {
	.core = &dw_mipi_csi_core_ops,
	.pad = &dw_mipi_csi_pad_ops,
	.video = &dw_mipi_csi_video_ops,
};

static irqreturn_t dw_mipi_csi_irq1(int irq, void *dev_id)
{
	struct dw_csi *csi_dev = dev_id;

	dw_mipi_csi_irq_handler(csi_dev);

	return IRQ_HANDLED;
}

static int dw_async_bound(struct v4l2_async_notifier *notifier,
			  struct v4l2_subdev *subdev,
			  struct v4l2_async_connection *asc)
{
	struct dw_csi *dw = container_of(notifier,
					struct dw_csi, notifier);
	int ret;
	int pad;

	dev_dbg(dw->dev, "async bound\n");
	dw->input_sd = subdev;

	pad = media_entity_get_fwnode_pad(&subdev->entity,
					  asc->match.fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0) {
		dev_err(dw->dev, "Failed to find pad for %s\n",
			dw->sd.name);
		return pad;
	}

	dw->remote_pad = pad;

	ret = media_create_pad_link(&dw->input_sd->entity, dw->remote_pad,
				    &dw->sd.entity, 0, MEDIA_LNK_FL_ENABLED);

	if (ret < 0) {
		dev_err(dw->dev,
			"Failed to create pad link: %s to %s\n",
			dw->input_sd->entity.name, dw->sd.entity.name);
		return ret;
	}

	dev_dbg(dw->dev, "link with %s pad: %d\n",
		dw->input_sd->name, dw->remote_pad);

	return ret;
}

static const struct v4l2_async_notifier_operations csi2host_async_ops = {
	.bound = dw_async_bound,
};

static int
dw_mipi_csi_parse_dt(struct platform_device *pdev, struct dw_csi *dev)
{
	struct device_node *of_node = pdev->dev.of_node;
	struct fwnode_handle *input_fwnode, *output_fwnode;
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct v4l2_fwnode_endpoint ep2 = { };
	struct v4l2_async_connection *asc;
	int ret = 0;

	if (of_property_read_u32(of_node, "snps,output-type",
				 &dev->hw.output))
		dev->hw.output = 2;

	input_fwnode = fwnode_graph_get_next_endpoint(of_fwnode_handle(of_node),
						      NULL);
	if (!input_fwnode) {
		dev_err(&pdev->dev,
			"missing port node at %pOF, input node is mandatory.\n",
			of_node);
		return -EINVAL;
	}

	/* Get port node and validate MIPI-CSI channel id. */
	ret = v4l2_fwnode_endpoint_parse(input_fwnode, &ep);
	if (ret)
		goto err;

	dev->index = ep.base.port - 1;
	if (dev->index >= CSI_MAX_ENTITIES) {
		ret = -ENXIO;
		goto err;
	}
	dev->hw.num_lanes = ep.bus.mipi_csi2.num_data_lanes;

	output_fwnode = fwnode_graph_get_next_endpoint
				(of_fwnode_handle(of_node), input_fwnode);

	if (output_fwnode) {
		ret = v4l2_fwnode_endpoint_parse(output_fwnode,
						 &ep2);

		fwnode_handle_put(output_fwnode);
	}

	if (!output_fwnode || ret) {
		dev_info(&pdev->dev,
			 "missing output node at %pOF\n", of_node);
	}

	v4l2_async_subdev_nf_init(&dev->notifier, &dev->sd);
	asc = v4l2_async_nf_add_fwnode_remote(&dev->notifier, input_fwnode,
					      struct v4l2_async_connection);

	if (IS_ERR(asc)) {
		ret = PTR_ERR(asc);
		goto err;
	}

	if (ret) {
		dev_err(&pdev->dev, "failed to add async notifier.\n");
		goto err;
	}

	dev->notifier.ops = &csi2host_async_ops;

	ret = v4l2_async_nf_register(&dev->notifier);

	if (ret) {
		dev_err(&pdev->dev, "fail to register async notifier.\n");
		goto err;
	}

err:
	of_node_put(of_node);
	return ret;
}

static const struct of_device_id dw_mipi_csi_of_match[];

static int dw_csi_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id = NULL;
	struct dw_csih_pdata *pdata = NULL;
	struct device *dev = &pdev->dev;
	struct resource *res = NULL;
	struct dw_csi *csi;
	struct v4l2_subdev *sd;
	int ret;

	if (!IS_ENABLED(CONFIG_OF))
		pdata = pdev->dev.platform_data;

	dev_dbg(dev, "Probing started\n");

	/* Resource allocation */
	csi = devm_kzalloc(dev, sizeof(*csi), GFP_KERNEL);
	if (!csi)
		return -ENOMEM;

	mutex_init(&csi->lock);
	spin_lock_init(&csi->slock);
	csi->dev = dev;

	csi->perclk = devm_clk_get(dev, "perclk");
	if (IS_ERR(csi->perclk)) {
		ret = PTR_ERR(csi->perclk);
		dev_err(dev, "failed to get perclk: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(csi->perclk);
	if (ret) {
		dev_err(dev, "failed to enable perclk: %d\n", ret);
		return ret;
	}

	csi->phyclk = devm_clk_get(dev, "phyclk");
	if (IS_ERR(csi->perclk)) {
		ret = PTR_ERR(csi->phyclk);
		dev_err(dev, "failed to get phyclk: %d\n", ret);
		goto csi2host_phyclk_err;
	}

	ret = clk_prepare_enable(csi->phyclk);
	if (ret) {
		dev_err(dev, "failed to enable phyclk: %d\n", ret);
		goto csi2host_phyclk_err;
	}

	if (dev->of_node) {
		of_id = of_match_node(dw_mipi_csi_of_match, dev->of_node);
		if (!of_id) {
			ret = -EINVAL;
			goto csi2host_reg_err;
		}

		ret = dw_mipi_csi_parse_dt(pdev, csi);
		if (ret < 0)
			goto csi2host_reg_err;

		csi->phy = devm_of_phy_get(dev, dev->of_node, NULL);
		if (IS_ERR(csi->phy)) {
			dev_dbg(dev, "No DPHY available\n");
			ret = -EPROBE_DEFER; /* attempt to defer */
			goto csi2host_defer_err;
		}
	} else {
		if (!pdata)
			goto csi2host_reg_err;

		csi->phy = devm_phy_get(dev, phys[pdata->id].name);
		if (IS_ERR(csi->phy)) {
			dev_err(dev, "No '%s' DPHY available\n",
				phys[pdata->id].name);
			return PTR_ERR(csi->phy);
		}
		dev_vdbg(dev, "got D-PHY %s with id %d\n", phys[pdata->id].name,
			 csi->phy->id);
	}
	/* Registers mapping */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENXIO;
		goto csi2host_defer_err;
	}

	csi->base_address = devm_ioremap_resource(dev, res);
	if (IS_ERR(csi->base_address)) {
		dev_err(dev, "Base address not set.\n");
		ret = PTR_ERR(csi->base_address);
		goto csi2host_defer_err;
	}

	csi->ctrl_irq_number = platform_get_irq(pdev, 0);
	if (csi->ctrl_irq_number < 0) {
		dev_err(dev, "irq number %d not set.\n", csi->ctrl_irq_number);
		ret = csi->ctrl_irq_number;
		goto end;
	}

	csi->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(csi->rst)) {
		dev_err(dev, "error getting reset control %d\n", ret);
		ret =  PTR_ERR(csi->rst);
		goto end;
	}

	ret = devm_request_irq(dev, csi->ctrl_irq_number,
			       dw_mipi_csi_irq1, IRQF_SHARED,
			       dev_name(dev), csi);
	if (ret) {
		if (dev->of_node)
			dev_err(dev, "irq csi %s failed\n", of_id->name);
		else
			dev_err(dev, "irq csi %d failed\n", pdata->id);

		goto end;
	}

	sd = &csi->sd;
	v4l2_subdev_init(sd, &dw_mipi_csi_subdev_ops);
	csi->sd.owner = THIS_MODULE;
	csi->sd.fwnode = of_fwnode_handle(dev->of_node);

	if (dev->of_node) {
		snprintf(sd->name, sizeof(sd->name), "%s.%d",
			 "dw-csi", csi->index);

		csi->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	} else {
		strlcpy(sd->name, dev_name(dev), sizeof(sd->name));
	}
	csi->fmt = &dw_mipi_csi_formats[0];
	csi->format.code = dw_mipi_csi_formats[0].mbus_code;

	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	if (dev->of_node) {
		csi->pads[CSI_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
		csi->pads[CSI_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

		ret = media_entity_pads_init(&csi->sd.entity,
					     CSI_PADS_NUM, csi->pads);
		if (ret < 0) {
			dev_err(dev, "media entity init failed\n");
			goto end;
		}
	} else {
		csi->hw.num_lanes = pdata->lanes;
		csi->hw.pclk = pdata->pclk;
		csi->hw.fps = pdata->fps;
		csi->hw.dphy_freq = pdata->hs_freq;
	}
	v4l2_set_subdevdata(&csi->sd, pdev);
	platform_set_drvdata(pdev, &csi->sd);
	dev_set_drvdata(dev, sd);

	if (csi->rst)
		reset_control_deassert(csi->rst);

#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
	dw_csi_create_capabilities_sysfs(pdev);
#endif
	dw_mipi_csi_get_version(csi);
	dw_mipi_csi_specific_mappings(csi);
	dw_mipi_csi_mask_irq_power_off(csi);

	dev_info(dev, "DW MIPI CSI-2 Host registered successfully HW v%u.%u\n",
		 csi->hw_version_major, csi->hw_version_minor);

	phy_init(csi->phy);

	ret = v4l2_async_register_subdev(&csi->sd);

	if (ret)
		dev_dbg(csi->dev, "failed to register the subdevice\n");

	return ret;
end:
#if IS_ENABLED(CONFIG_OF)
	media_entity_cleanup(&csi->sd.entity);
#endif
csi2host_defer_err:
	v4l2_async_nf_unregister(&csi->notifier);
	v4l2_async_nf_cleanup(&csi->notifier);

csi2host_reg_err:
	clk_disable_unprepare(csi->phyclk);
csi2host_phyclk_err:
	clk_disable_unprepare(csi->perclk);
	return ret;
}

/**
 * @short Exit routine - Exit point of the driver
 * @param[in] pdev pointer to the platform device structure
 * @return 0 on success
 */
static int dw_csi_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *mipi_csi = sd_to_mipi_csi_dev(sd);

	dev_dbg(&pdev->dev, "Removing DW MIPI CSI-2 Host module\n");

	if (mipi_csi->rst)
		reset_control_assert(mipi_csi->rst);
#if IS_ENABLED(CONFIG_OF)
	media_entity_cleanup(&mipi_csi->sd.entity);
#endif

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id dw_mipi_csi_of_match[] = {
	{ .compatible = "snps,dw-csi" },
	{},
};

MODULE_DEVICE_TABLE(of, dw_mipi_csi_of_match);
#endif

static struct platform_driver dw_mipi_csi_driver = {
	.remove = dw_csi_remove,
	.probe = dw_csi_probe,
	.driver = {
		.name = "dw-csi",
		.owner = THIS_MODULE,
#if IS_ENABLED(CONFIG_OF)
		.of_match_table = of_match_ptr(dw_mipi_csi_of_match),
#endif
	},
};

module_platform_driver(dw_mipi_csi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Luis Oliveira <luis.oliveira@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DesignWare MIPI CSI-2 Host Platform driver");
