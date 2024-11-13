// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Manikandan Muralidharan <manikandan.m@microchip.com>
 * Author: Dharma Balasubiramani <dharma.b@microchip.com>
 *
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/media-bus-format.h>
#include <linux/mfd/syscon.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define LVDS_POLL_TIMEOUT_MS 1000

/* LVDSC register offsets */
#define LVDSC_CR	0x00
#define LVDSC_CFGR	0x04
#define LVDSC_SR	0x0C
#define LVDSC_WPMR	0xE4

/* Bitfields in LVDSC_CR (Control Register) */
#define LVDSC_CR_SER_EN	BIT(0)

/* Bitfields in LVDSC_CFGR (Configuration Register) */
#define LVDSC_CFGR_PIXSIZE_24BITS	0
#define LVDSC_CFGR_PIXSIZE_18BITS	1
#define LVDSC_CFGR_DEN_POL_HIGH		0
#define LVDSC_CFGR_DC_UNBALANCED	0
#define LVDSC_CFGR_MAPPING_JEIDA	BIT(6)
#define LVDSC_CFGR_MAPPING_VESA		0

/*Bitfields in LVDSC_SR */
#define LVDSC_SR_CS	BIT(0)

/* Bitfields in LVDSC_WPMR (Write Protection Mode Register) */
#define LVDSC_WPMR_WPKEY_MASK	GENMASK(31, 8)
#define LVDSC_WPMR_WPKEY_PSSWD	0x4C5644

struct mchp_lvds {
	struct device *dev;
	void __iomem *regs;
	struct clk *pclk;
	struct drm_panel *panel;
	struct drm_bridge bridge;
	struct drm_connector connector;
	struct drm_bridge *panel_bridge;
};

static inline struct mchp_lvds *bridge_to_lvds(struct drm_bridge *bridge)
{
	return container_of(bridge, struct mchp_lvds, bridge);
}

static inline struct mchp_lvds *
drm_connector_to_mchp_lvds(struct drm_connector *connector)
{
	return container_of(connector, struct mchp_lvds, connector);
}

static inline u32 lvds_readl(struct mchp_lvds *lvds, u32 offset)
{
	return readl_relaxed(lvds->regs + offset);
}

static inline void lvds_writel(struct mchp_lvds *lvds, u32 offset, u32 val)
{
	writel_relaxed(val, lvds->regs + offset);
}

static void lvds_serialiser_on(struct mchp_lvds *lvds)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(LVDS_POLL_TIMEOUT_MS);
	struct drm_connector *connector = &lvds->connector;

	/* default to jeida-24 */
	u32 bus_formats = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA;
	u8 map, pix_size;

	/* The LVDSC registers can only be written if WPEN is cleared */
	lvds_writel(lvds, LVDSC_WPMR, (LVDSC_WPMR_WPKEY_PSSWD &
				LVDSC_WPMR_WPKEY_MASK));

	/* Wait for the status of configuration registers to be changed */
	while (lvds_readl(lvds, LVDSC_SR) & LVDSC_SR_CS) {
		if (time_after(jiffies, timeout)) {
			dev_err(lvds->dev, "%s: timeout error\n", __func__);
			return;
		}
		usleep_range(1000, 2000);
	}

	if (connector && connector->display_info.num_bus_formats)
		bus_formats = connector->display_info.bus_formats[0];

	/* Configure the LVDSC */
	switch (bus_formats) {
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		map = LVDSC_CFGR_MAPPING_JEIDA;
		pix_size = LVDSC_CFGR_PIXSIZE_18BITS;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		map = LVDSC_CFGR_MAPPING_VESA;
		pix_size = LVDSC_CFGR_PIXSIZE_24BITS;
		break;
	default:
		map = LVDSC_CFGR_MAPPING_JEIDA;
		pix_size = LVDSC_CFGR_PIXSIZE_24BITS;
		break;
	}

	lvds_writel(lvds, LVDSC_CFGR, (map | LVDSC_CFGR_DC_UNBALANCED |
		    LVDSC_CFGR_DEN_POL_HIGH | pix_size));

	/* Enable the LVDS serializer */
	lvds_writel(lvds, LVDSC_CR, LVDSC_CR_SER_EN);
}

static int mchp_lvds_connector_get_modes(struct drm_connector *connector)
{
	struct mchp_lvds *lvds = drm_connector_to_mchp_lvds(connector);

	return drm_panel_get_modes(lvds->panel, connector);
}

static const struct drm_connector_helper_funcs
mchp_lvds_connector_helper_funcs = {
	.get_modes = mchp_lvds_connector_get_modes,
};

static const struct drm_connector_funcs panel_bridge_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int mchp_lvds_attach(struct drm_bridge *bridge,
			    struct drm_encoder *encoder,
			    enum drm_bridge_attach_flags flags)
{
	struct mchp_lvds *lvds = bridge_to_lvds(bridge);
	struct drm_connector *connector = &lvds->connector;
	int ret;

	ret = drm_bridge_attach(encoder, lvds->panel_bridge,
				bridge, flags);

	if (ret < 0)
		return ret;

	if (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)
		return 0;

	if (!bridge->encoder) {
		dev_err(lvds->dev, "Missing encoder\n");
		return -ENODEV;
	}

	drm_connector_helper_add(connector,
				 &mchp_lvds_connector_helper_funcs);

	ret = drm_connector_init(bridge->dev, connector,
				 &panel_bridge_connector_funcs,
				 DRM_MODE_CONNECTOR_LVDS);
	if (ret) {
		dev_err(lvds->dev, "Failed to initialize connector %d\n", ret);
		return ret;
	}

	drm_panel_bridge_set_orientation(connector, bridge);

	ret = drm_connector_attach_encoder(&lvds->connector, bridge->encoder);
	if (ret) {
		dev_err(lvds->dev, "Failed to attach connector to encoder %d\n", ret);
		drm_connector_cleanup(connector);
		return ret;
	}

	if (bridge->dev->registered) {
		if (connector->funcs->reset)
			connector->funcs->reset(connector);

		ret = drm_connector_register(connector);
		if (ret) {
			dev_err(lvds->dev, "Failed to attach connector to register %d\n", ret);
			drm_connector_cleanup(connector);
			return ret;
		}
	}

	return 0;
}

static void mchp_lvds_enable(struct drm_bridge *bridge)
{
	struct mchp_lvds *lvds = bridge_to_lvds(bridge);
	int ret;

	ret = clk_prepare_enable(lvds->pclk);
	if (ret < 0) {
		dev_err(lvds->dev, "failed to enable lvds pclk %d\n", ret);
		return;
	}

	ret = pm_runtime_get_sync(lvds->dev);
	if (ret < 0) {
		dev_err(lvds->dev, "failed to get pm runtime: %d\n", ret);
		return;
	}

	lvds_serialiser_on(lvds);
}

static void mchp_lvds_disable(struct drm_bridge *bridge)
{
	struct mchp_lvds *lvds = bridge_to_lvds(bridge);

	pm_runtime_put(lvds->dev);
	clk_disable_unprepare(lvds->pclk);
}

static const struct drm_bridge_funcs mchp_lvds_bridge_funcs = {
	.attach = mchp_lvds_attach,
	.enable = mchp_lvds_enable,
	.disable = mchp_lvds_disable,
};

static int mchp_lvds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mchp_lvds *lvds;
	struct device_node *port;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	lvds = devm_drm_bridge_alloc(&pdev->dev, struct mchp_lvds, bridge,
				     &mchp_lvds_bridge_funcs);
	if (IS_ERR(lvds))
		return PTR_ERR(lvds);

	lvds->dev = dev;

	lvds->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lvds->regs))
		return PTR_ERR(lvds->regs);

	lvds->pclk = devm_clk_get(lvds->dev, "pclk");
	if (IS_ERR(lvds->pclk))
		return dev_err_probe(lvds->dev, PTR_ERR(lvds->pclk),
				"could not get pclk_lvds\n");

	port = of_graph_get_remote_node(dev->of_node, 1, 0);
	if (!port) {
		dev_err(dev,
			"can't find port point, please init lvds panel port!\n");
		return -ENODEV;
	}

	lvds->panel = of_drm_find_panel(port);
	of_node_put(port);

	if (IS_ERR(lvds->panel))
		return -EPROBE_DEFER;

	lvds->panel_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 0);

	if (IS_ERR(lvds->panel_bridge))
		return PTR_ERR(lvds->panel_bridge);

	lvds->bridge.of_node = dev->of_node;
	lvds->bridge.type = DRM_MODE_CONNECTOR_LVDS;

	dev_set_drvdata(dev, lvds);
	ret = devm_pm_runtime_enable(dev);
	if (ret < 0) {
		dev_err(lvds->dev, "failed to enable pm runtime: %d\n", ret);
		return ret;
	}

	drm_bridge_add(&lvds->bridge);

	return 0;
}

static const struct of_device_id mchp_lvds_dt_ids[] = {
	{ .compatible = "microchip,sam9x75-lvds" },
	{ .compatible = "microchip,sama7d65-lvds" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mchp_lvds_dt_ids);

static struct platform_driver mchp_lvds_driver = {
	.probe = mchp_lvds_probe,
	.driver = {
		   .name = "microchip-lvds",
		   .of_match_table = mchp_lvds_dt_ids,
	},
};
module_platform_driver(mchp_lvds_driver);

MODULE_AUTHOR("Manikandan Muralidharan <manikandan.m@microchip.com>");
MODULE_AUTHOR("Dharma Balasubiramani <dharma.b@microchip.com>");
MODULE_DESCRIPTION("Low Voltage Differential Signaling Controller Driver");
MODULE_LICENSE("GPL");
