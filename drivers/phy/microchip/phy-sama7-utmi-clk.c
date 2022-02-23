// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for the Microchip SAMA7 USB 2.0 PHY Clock
 *
 * Copyright (C) 2023 Microchip Technology, Inc. and its subsidiaries
 *
 * Author: Cristian Birsan <cristian.birsan@microchip.com>
 *
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/at91_pmc.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <soc/at91/sama7-sfr.h>

struct sama7_utmi_clk {
	struct clk_hw		hw;
	struct regmap		*regmap_sfr;
	struct reset_control	*reset;
	u8 id;
};
#define to_sama7_utmi_clk(hw) container_of(hw, struct sama7_utmi_clk, hw)

/*
 * UTMI clock description
 * @n:	clock name
 * @p:	clock parent name
 * @id: clock id
 */
static struct {
	const char *n;
	const char *p;
	u8 id;
} sama7_utmick[] = {
	{ .n = "utmi1",		.p = "utmick",		.id = 0, },
	{ .n = "utmi2",		.p = "utmi1",		.id = 1, },
	{ .n = "utmi3",		.p = "utmi1",		.id = 2, },
};

int sama7_utmi_clk_probe(struct platform_device *pdev);

static int sama7_utmi_clk_enable(struct clk_hw *hw)
{
	int ret;

	struct sama7_utmi_clk *utmi = to_sama7_utmi_clk(hw);
	u8 id = utmi->id;

	ret = reset_control_assert(utmi->reset);
	if (ret)
		return ret;

	ret = regmap_update_bits(utmi->regmap_sfr, SAMA7_SFR_UTMI0R(id),
				SAMA7_SFR_UTMI_COMMONON, 0);
	if (ret < 0)
		return ret;

	ret = reset_control_deassert(utmi->reset);
	if (ret)
		return ret;

	/* Datasheet states a minimum of 45 us before any USB operation */
	udelay(50);

	return 0;
}

static void sama7_utmi_clk_disable(struct clk_hw *hw)
{
	int ret;
	struct sama7_utmi_clk *utmi = to_sama7_utmi_clk(hw);
	u8 id = utmi->id;

	ret = reset_control_assert(utmi->reset);
	if (ret)
		return;

	regmap_update_bits(utmi->regmap_sfr, SAMA7_SFR_UTMI0R(id),
			   SAMA7_SFR_UTMI_COMMONON, SAMA7_SFR_UTMI_COMMONON);
}

static int sama7_utmi_clk_is_enabled(struct clk_hw *hw)
{
	int ret;
	struct sama7_utmi_clk *utmi = to_sama7_utmi_clk(hw);

	ret = reset_control_status(utmi->reset);
	if (ret)
		return false;
	else
		return true;
}

static const struct clk_ops sama7_utmi_ops = {
	.enable = sama7_utmi_clk_enable,
	.disable = sama7_utmi_clk_disable,
	.is_enabled = sama7_utmi_clk_is_enabled,
};

static struct clk_hw *
sama7_utmi_clk_register(struct device *dev,
			 struct regmap *regmap_sfr, struct reset_control *reset,
			 const char *name, const char *parent_name,
			 const struct clk_ops *ops, u8 id)
{
	struct clk_init_data init = {};
	struct clk_hw *hw;
	struct sama7_utmi_clk *utmi_clk;
	int ret;

	utmi_clk = devm_kzalloc(dev, sizeof(*utmi_clk), GFP_KERNEL);
	if (!utmi_clk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = ops;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	utmi_clk->hw.init = &init;
	utmi_clk->reset = reset;
	utmi_clk->regmap_sfr = regmap_sfr;
	utmi_clk->id = id;

	hw = &utmi_clk->hw;
	ret = devm_clk_hw_register(dev, &utmi_clk->hw);
	if (ret) {
		devm_kfree(dev, utmi_clk);
		hw = ERR_PTR(ret);
	}

	return hw;
}

int sama7_utmi_clk_probe(struct platform_device *pdev)
{
	struct clk *utmi_parent_clk;
	struct clk_hw *hw;
	struct clk_hw_onecell_data *hw_data;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regmap *regmap_sfr;
	struct reset_control	*phy_reset;
	int i;

	char name[16];

	hw_data = devm_kzalloc(dev, struct_size(hw_data, hws,
			       ARRAY_SIZE(sama7_utmick)), GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	utmi_parent_clk = devm_clk_get(dev, "utmi_clk");
	if (IS_ERR(utmi_parent_clk))
		return PTR_ERR(utmi_parent_clk);

	sama7_utmick[0].p = __clk_get_name(utmi_parent_clk);

	regmap_sfr = syscon_regmap_lookup_by_phandle(np, "sfr-phandle");
	if (IS_ERR(regmap_sfr))
		return PTR_ERR(regmap_sfr);

	for (i = 0; i < ARRAY_SIZE(sama7_utmick); i++) {

		snprintf(name, sizeof(name), "usb%d_reset", i);
		phy_reset = devm_reset_control_get(dev, name);
		if (IS_ERR(phy_reset)) {
			dev_err(dev, "failed to get reset %s\n", name);
			return PTR_ERR(phy_reset);
		}

		hw = sama7_utmi_clk_register(dev, regmap_sfr, phy_reset,
					      sama7_utmick[i].n,
					      sama7_utmick[i].p,
					      &sama7_utmi_ops,
					      sama7_utmick[i].id);
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		hw_data->hws[i] = hw;
	}
	hw_data->num = ARRAY_SIZE(sama7_utmick);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, hw_data);
};

static const struct of_device_id sama7_utmi_clk_dt_ids[] = {
	{ .compatible = "microchip,sama7g5-utmi-clk", },
	{ /* santinel */},
};
MODULE_DEVICE_TABLE(of, sama7_utmi_clk_dt_ids);

static struct platform_driver sama7_utmi_clk_driver = {
	.probe  = sama7_utmi_clk_probe,
	.driver = {
		.name = "sama7-utmi-clk",
		.of_match_table = sama7_utmi_clk_dt_ids,
	},
};
builtin_platform_driver(sama7_utmi_clk_driver);

MODULE_AUTHOR("Cristian Birsan <cristian.birsan@microchip.com>");
MODULE_DESCRIPTION("Microchip SAMA7X UTMI clock driver");
MODULE_LICENSE("GPL v2");
