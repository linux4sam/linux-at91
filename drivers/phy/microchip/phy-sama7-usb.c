// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for the Microchip SAMA7 USB 2.0 PHY
 *
 * Copyright (C) 2022 Microchip Technology, Inc. and its subsidiaries
 *
 * Author: Cristian Birsan <cristian.birsan@microchip.com>
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <soc/at91/sama7-sfr.h>

struct sama7_usb_phy {
	struct phy *phy;
	struct clk *clk;
	struct regmap *sfr;
	enum phy_mode mode;
	int port;
};

int sama7_usb_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode);
int sama7_usb_phy_init(struct phy *phy);
int sama7_phy_power_on(struct phy *phy);
int sama7_phy_power_off(struct phy *phy);
int sama7_usb_phy_probe(struct platform_device *pdev);

int sama7_usb_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct sama7_usb_phy *sama7_phy = phy_get_drvdata(phy);
	int port = sama7_phy->port;

	sama7_phy->mode = PHY_MODE_INVALID;

	if (mode > 0)
		sama7_phy->mode = mode;

	/* Notify the controller when VBUS is present for device mode */
	if (mode == PHY_MODE_USB_DEVICE) {
		regmap_update_bits(sama7_phy->sfr, SAMA7_SFR_UTMI0R(port),
				   SAMA7_SFR_UTMI_RX_VBUS,
				   (submode ? SAMA7_SFR_UTMI_RX_VBUS : 0));
	}

	dev_dbg(&sama7_phy->phy->dev, "USB PHY Set Mode port=%d, mode=%d\n",
		sama7_phy->port, sama7_phy->mode);

	return 0;
}

int sama7_usb_phy_init(struct phy *phy)
{
	struct sama7_usb_phy *sama7_phy = phy_get_drvdata(phy);
	int port = sama7_phy->port;

	/* Set Transmitter Pre-Emphasis AMP Tune to 1X */
	regmap_update_bits(sama7_phy->sfr, SAMA7_SFR_UTMI0R(port),
			   SAMA7_SFR_UTMI_RX_TX_PREEM_AMP_TUNE_1X,
			   SAMA7_SFR_UTMI_RX_TX_PREEM_AMP_TUNE_1X);

	dev_dbg(&sama7_phy->phy->dev, "USB PHY Init , port=%d\n",
		sama7_phy->port);

	return 0;
}

int sama7_phy_power_on(struct phy *phy)
{
	struct sama7_usb_phy *sama7_phy = phy_get_drvdata(phy);

	clk_prepare_enable(sama7_phy->clk);

	dev_dbg(&sama7_phy->phy->dev, "USB PHY Power On port=%d\n",
		sama7_phy->port);

	return 0;
}

int sama7_phy_power_off(struct phy *phy)
{
	struct sama7_usb_phy *sama7_phy = phy_get_drvdata(phy);

	clk_disable_unprepare(sama7_phy->clk);

	dev_dbg(&sama7_phy->phy->dev, "USB PHY Power Off port=%d\n",
		sama7_phy->port);

	return 0;
}

static const struct phy_ops sama7_usb_phy_ops = {
	.init = sama7_usb_phy_init,
	.power_on = sama7_phy_power_on,
	.power_off = sama7_phy_power_off,
	.set_mode = sama7_usb_phy_set_mode,
	.owner = THIS_MODULE,
};

int sama7_usb_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct sama7_usb_phy *sama7_phy;

	sama7_phy = devm_kzalloc(dev, sizeof(*sama7_phy), GFP_KERNEL);
	if (!sama7_phy)
		return -ENOMEM;

	sama7_phy->clk = devm_clk_get(dev, "utmi_clk");
	if (IS_ERR(sama7_phy->clk)) {
		dev_err(dev, "failed to get sama7 usb utmi phy clock\n");
		return PTR_ERR(sama7_phy->clk);
	}

	sama7_phy->sfr =  syscon_regmap_lookup_by_phandle(np, "sfr-phandle");
	if (IS_ERR(sama7_phy->sfr)) {
		sama7_phy->sfr = NULL;
		dev_err(dev, "failed to get sfr\n");
		return -ENODEV;
	}

	if (of_property_read_u32(np, "reg", &sama7_phy->port)) {
		dev_err(dev, "failed to get reg\n");
		return -ENODEV;
	}

	sama7_phy->phy = devm_phy_create(dev, NULL, &sama7_usb_phy_ops);
	if (IS_ERR(sama7_phy->phy))
		return PTR_ERR(sama7_phy->phy);

	phy_set_drvdata(sama7_phy->phy, sama7_phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	dev_info(dev, "probed\n");

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id sama7_usb_phy_of_match[] = {
	{ .compatible = "microchip,sama7g5-usb-phy", },
	{ },
};
MODULE_DEVICE_TABLE(of, sama7_usb_phy_of_match);

static struct platform_driver sama7_usb_phy_driver = {
	.probe = sama7_usb_phy_probe,
	.driver = {
		.name = "sama7-usb-phy",
		.of_match_table =
			sama7_usb_phy_of_match,
	}
};
module_platform_driver(sama7_usb_phy_driver);

MODULE_AUTHOR("Cristian Birsan <cristian.birsan@microchip.com>");
MODULE_DESCRIPTION("Microchip SAMA7X USB PHY driver");
MODULE_LICENSE("GPL");
