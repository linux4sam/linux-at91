// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Manikandan Muralidharan <manikandan.m@microchip.com>
 *
 */

#include <drm/bridge/dw_mipi_dsi.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DSI_PLL_REF_CLK			24000000

#define DSI_PWR_UP			0x04
#define HOST_RESET			BIT(0)
#define HOST_PWRUP			0

#define DSI_PHY_RSTZ			0xa0
#define PHY_SHUTDOWNZ			0

#define DSI_PHY_TST_CTRL0		0xb4
#define PHY_TESTCLK			BIT(1)
#define PHY_UNTESTCLK			0
#define PHY_TESTCLR			BIT(0)
#define PHY_UNTESTCLR			0

#define DSI_PHY_TST_CTRL1		0xb8
#define PHY_TESTEN			BIT(16)
#define PHY_UNTESTEN			0
#define PHY_TESTDOUT(n)			(((n) & 0xff) << 8)
#define PHY_TESTDIN(n)			(((n) & 0xff) << 0)

#define BYPASS_VCO_RANGE		BIT(7)
#define VCO_RANGE_CON_SEL(val)		(((val) & 0x7) << 3)
#define VCO_IN_CAP_CON_LOW		BIT(1)

#define CP_CURRENT_0			0x2
#define CP_CURRENT_1			0x4
#define CP_CURRENT_2			0x5
#define CP_CURRENT_3			0x6
#define CP_CURRENT_4			0x7
#define CP_CURRENT_5			0x8
#define CP_CURRENT_6			0xc
#define CP_CURRENT_SEL(val)		((val) & 0xf)
#define CP_PROGRAM_EN			BIT(7)

#define LPF_RESISTORS_18KOHM		0x0
#define LPF_RESISTORS_15_6KOHM		0x1
#define LPF_RESISTORS_15KOHM		0x2
#define LPF_RESISTORS_14_4KOHM		0x4
#define LPF_RESISTORS_12_8KOHM		0x8
#define LPF_RESISTORS_11_4KOHM		0x10
#define LPF_RESISTORS_10_5KOHM		0x20
#define LPF_PROGRAM_EN			BIT(6)
#define LPF_RESISTORS_SEL(val)		((val) & 0x3f)

#define HSFREQRANGE_SEL(val)		(((val) & 0x3f) << 1)

#define INPUT_DIVIDER(val)		(((val) - 1) & 0x7f)
#define LOW_PROGRAM_EN			0
#define HIGH_PROGRAM_EN			BIT(7)
#define LOOP_DIV_LOW_SEL(val)		(((val) - 1) & 0x1f)
#define LOOP_DIV_HIGH_SEL(val)		((((val) - 1) >> 5) & 0xf)
#define PLL_LOOP_DIV_EN			BIT(5)
#define PLL_INPUT_DIV_EN		BIT(4)

#define PLL_BIAS_CUR_SEL_CAP_VCO_CONTROL		0x10
#define PLL_CP_CONTROL_PLL_LOCK_BYPASS			0x11
#define PLL_LPF_AND_CP_CONTROL				0x12
#define PLL_INPUT_DIVIDER_RATIO				0x17
#define PLL_LOOP_DIVIDER_RATIO				0x18
#define PLL_INPUT_AND_LOOP_DIVIDER_RATIOS_CONTROL	0x19

#define SFR_ISS_CFG			0x240
#define ISS_CFG_DSI_MODE		1

struct dw_mipi_dsi_mchp_chip_data {
	unsigned int max_data_lanes;
	struct dw_mipi_dsi_phy_ops *phy_ops;
};

struct dw_mipi_dsi_mchp {
	struct device *dev;
	void __iomem *base;
	struct dw_mipi_dsi_plat_data pdata;
	struct dw_mipi_dsi *dsi;

	/* needed for PLL config */
	unsigned int lane_mbps;
	u16 input_div;
	u16 feedback_div;
	u32 format;

	struct clk *pclk;
	struct clk *pllref_clk;
};

struct dphy_pll_parameter_map {
	unsigned int max_mbps;
	u8 hsfreqrange;
	u8 icpctrl;
	u8 lpfctrl;
};

static const struct dphy_pll_parameter_map dppa_map[] = {
	{  89, 0x00, CP_CURRENT_1, LPF_RESISTORS_11_4KOHM },
	{  99, 0x20, CP_CURRENT_1, LPF_RESISTORS_11_4KOHM },
	{ 109, 0x40, CP_CURRENT_1, LPF_RESISTORS_11_4KOHM },
	{ 129, 0x02, CP_CURRENT_5, LPF_RESISTORS_12_8KOHM },
	{ 139, 0x22, CP_CURRENT_5, LPF_RESISTORS_12_8KOHM },
	{ 149, 0x42, CP_CURRENT_5, LPF_RESISTORS_12_8KOHM },
	{ 169, 0x04, CP_CURRENT_6, LPF_RESISTORS_12_8KOHM },
	{ 179, 0x24, CP_CURRENT_6, LPF_RESISTORS_12_8KOHM },
	{ 199, 0x44, CP_CURRENT_6, LPF_RESISTORS_12_8KOHM },
	{ 219, 0x06, CP_CURRENT_6, LPF_RESISTORS_12_8KOHM },
	{ 239, 0x26, CP_CURRENT_6, LPF_RESISTORS_12_8KOHM },
	{ 249, 0x46, CP_CURRENT_6, LPF_RESISTORS_12_8KOHM },
	{ 269, 0x08, CP_CURRENT_0, LPF_RESISTORS_12_8KOHM },
	{ 299, 0x28, CP_CURRENT_0, LPF_RESISTORS_12_8KOHM },
	{ 329, 0x0a, CP_CURRENT_2, LPF_RESISTORS_12_8KOHM },
	{ 359, 0x2a, CP_CURRENT_2, LPF_RESISTORS_12_8KOHM },
	{ 399, 0x4a, CP_CURRENT_2, LPF_RESISTORS_12_8KOHM },
	{ 449, 0x0C, CP_CURRENT_2, LPF_RESISTORS_15_6KOHM },
	{ 499, 0x2c, CP_CURRENT_2, LPF_RESISTORS_15_6KOHM },
	{ 549, 0x0e, CP_CURRENT_3, LPF_RESISTORS_11_4KOHM },
	{ 599, 0x2e, CP_CURRENT_3, LPF_RESISTORS_11_4KOHM },
	{ 649, 0x10, CP_CURRENT_3, LPF_RESISTORS_14_4KOHM },
	{ 699, 0x30, CP_CURRENT_3, LPF_RESISTORS_14_4KOHM },
	{ 749, 0x12, CP_CURRENT_3, LPF_RESISTORS_14_4KOHM },
	{ 799, 0x32, CP_CURRENT_3, LPF_RESISTORS_14_4KOHM },
	{ 849, 0x52, CP_CURRENT_3, LPF_RESISTORS_14_4KOHM },
	{ 899, 0x72, CP_CURRENT_3, LPF_RESISTORS_14_4KOHM },
	{ 949, 0x14, CP_CURRENT_4, LPF_RESISTORS_11_4KOHM },
	{1000, 0x34, CP_CURRENT_4, LPF_RESISTORS_11_4KOHM }
};

struct hstt {
	unsigned int maxfreq;
	struct dw_mipi_dsi_dphy_timing timing;
};

#define HSTT(_maxfreq, _c_lp2hs, _c_hs2lp, _d_lp2hs, _d_hs2lp)	\
{					\
	.maxfreq = _maxfreq,		\
	.timing = {			\
		.clk_lp2hs = _c_lp2hs,	\
		.clk_hs2lp = _c_hs2lp,	\
		.data_lp2hs = _d_lp2hs,	\
		.data_hs2lp = _d_hs2lp,	\
	}				\
}

struct hstt hstt_table[] = {
	HSTT(90,  32, 20,  26, 13),
	HSTT(100,  35, 23,  28, 14),
	HSTT(110,  32, 22,  26, 13),
	HSTT(130,  31, 20,  27, 13),
	HSTT(140,  33, 22,  26, 14),
	HSTT(150,  33, 21,  26, 14),
	HSTT(170,  32, 20,  27, 13),
	HSTT(180,  36, 23,  30, 15),
	HSTT(200,  40, 22,  33, 15),
	HSTT(220,  40, 22,  33, 15),
	HSTT(240,  44, 24,  36, 16),
	HSTT(250,  48, 24,  38, 17),
	HSTT(270,  48, 24,  38, 17),
	HSTT(300,  50, 27,  41, 18),
	HSTT(330,  56, 28,  45, 18),
	HSTT(360,  59, 28,  48, 19),
	HSTT(400,  61, 30,  50, 20),
	HSTT(450,  67, 31,  55, 21),
	HSTT(500,  73, 31,  59, 22),
	HSTT(550,  79, 36,  63, 24),
	HSTT(600,  83, 37,  68, 25),
	HSTT(650,  90, 38,  73, 27),
	HSTT(700,  95, 40,  77, 28),
	HSTT(750, 102, 40,  84, 28),
	HSTT(800, 106, 42,  87, 30),
	HSTT(850, 113, 44,  93, 31),
	HSTT(900, 118, 47,  98, 32),
	HSTT(950, 124, 47, 102, 34),
	HSTT(1000, 130, 49, 107, 35),
};

static int max_mbps_to_parameter(unsigned int max_mbps)
{
	int index;

	for (index = 0; index < ARRAY_SIZE(dppa_map); index++)
		if (dppa_map[index].max_mbps >= max_mbps)
			return index;

	return -EINVAL;
}

static inline void dsi_write(struct dw_mipi_dsi_mchp *dsi, u32 reg, u32 val)
{
	writel(val, dsi->base + reg);
}

static inline u32 dsi_read(struct dw_mipi_dsi_mchp *dsi, u32 reg)
{
	return readl(dsi->base + reg);
}

static void dw_mipi_dsi_phy_write(struct dw_mipi_dsi_mchp *dsi,
				  u8 test_code,
				  u8 test_data)
{
	/*
	 * With the falling edge on TESTCLK, the TESTDIN[7:0] signal content
	 * is latched internally as the current test code. Test data is
	 * programmed internally by rising edge on TESTCLK.
	 */

	/* General DPHY control operation */

	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_TESTCLK | PHY_UNTESTCLR);
	dsi_write(dsi, DSI_PHY_TST_CTRL1, PHY_TESTEN | PHY_TESTDOUT(1) |
						PHY_TESTDIN(test_code));
	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_UNTESTCLK | PHY_UNTESTCLR);
	dsi_write(dsi, DSI_PHY_TST_CTRL1, PHY_UNTESTEN | PHY_TESTDOUT(0) |
						PHY_TESTDIN(test_data));
	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_TESTCLK | PHY_UNTESTCLR);
	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_UNTESTCLK | PHY_UNTESTCLR);
}

static int dw_mipi_dsi_mchp_init(void *priv_data)
{
	struct dw_mipi_dsi_mchp *dsi = priv_data;
	int ret, index, vco;

	/*
	 * Get vco from frequency(lane_mbps)
	 * vco	frequency table
	 * 000 - between   80 and  200 MHz
	 * 001 - between  200 and  300 MHz
	 * 010 - between  300 and  500 MHz
	 * 011 - between  500 and  700 MHz
	 * 100 - between  700 and  900 MHz
	 * 101 - between  900 and 1000 MHz
	 */
	vco = (dsi->lane_mbps < 200) ? 0 : (dsi->lane_mbps + 100) / 200;

	index = max_mbps_to_parameter(dsi->lane_mbps);
	if (index < 0) {
		dev_err(dsi->dev,
			"failed to get parameter for %dmbps clock\n",
			dsi->lane_mbps);
		return index;
	}

	/* D-PHY in Shutdown mode */
	dsi_write(dsi, DSI_PHY_RSTZ, PHY_SHUTDOWNZ);

	dw_mipi_dsi_phy_write(dsi, PLL_BIAS_CUR_SEL_CAP_VCO_CONTROL,
			      BYPASS_VCO_RANGE |
			      VCO_RANGE_CON_SEL(vco) |
			      VCO_IN_CAP_CON_LOW);

	dw_mipi_dsi_phy_write(dsi, PLL_CP_CONTROL_PLL_LOCK_BYPASS,
			      CP_CURRENT_SEL(dppa_map[index].icpctrl));

	dw_mipi_dsi_phy_write(dsi, PLL_LPF_AND_CP_CONTROL,
			      CP_PROGRAM_EN | LPF_PROGRAM_EN |
			      LPF_RESISTORS_SEL(dppa_map[index].lpfctrl));

	dw_mipi_dsi_phy_write(dsi, PLL_INPUT_AND_LOOP_DIVIDER_RATIOS_CONTROL,
			      PLL_LOOP_DIV_EN | PLL_INPUT_DIV_EN);

	dw_mipi_dsi_phy_write(dsi, PLL_INPUT_DIVIDER_RATIO,
			      INPUT_DIVIDER(dsi->input_div));

	dw_mipi_dsi_phy_write(dsi, PLL_LOOP_DIVIDER_RATIO,
			      LOOP_DIV_LOW_SEL(dsi->feedback_div) |
			      LOW_PROGRAM_EN);

	dw_mipi_dsi_phy_write(dsi, PLL_LOOP_DIVIDER_RATIO,
			      LOOP_DIV_HIGH_SEL(dsi->feedback_div) |
			      HIGH_PROGRAM_EN);

	return ret;
}

static int dw_mipi_dsi_mchp_get_lane_mbps(void *priv_data,
					  const struct drm_display_mode *mode,
					  unsigned long mode_flags, u32 lanes,
					  u32 format, unsigned int *lane_mbps)
{
	struct dw_mipi_dsi_mchp *dsi = priv_data;
	unsigned int mpclk, target_mbps, desired_mbps;
	unsigned int max_mbps = dppa_map[ARRAY_SIZE(dppa_map) - 1].max_mbps;
	unsigned long best_freq, fvco_min, fvco_max, fin, fout;
	unsigned long min_delta = ULONG_MAX, delta;
	int bpp, min_prediv, max_prediv, _fbdiv, best_fbdiv, _prediv, best_prediv;
	u64 freq_factor;

	dsi->format = format;
	bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);
	if (bpp < 0) {
		dev_err(dsi->dev,
			"failed to get bpp for pixel format %d\n",
			dsi->format);
		return bpp;
	}

	mpclk = DIV_ROUND_UP(mode->clock, MSEC_PER_SEC);
	if (mpclk) {
		/* take 1/0.8, since mbps must be bigger than bandwidth of RGB */
		desired_mbps = mpclk * (bpp / lanes) * 10 / 8;
		if (desired_mbps < max_mbps) {
			target_mbps = desired_mbps;
		} else {
			dev_err(dsi->dev,
				"DPHY clock frequency is out of range\n");
			return -ERANGE;
		}
	}

	fin = clk_get_rate(dsi->pllref_clk);
	fout = target_mbps * USEC_PER_SEC;

	/* constraint: 5Mhz <= Fref / N <= 40MHz */
	min_prediv = DIV_ROUND_UP(fin, 40 * USEC_PER_SEC);
	max_prediv = fin / (5 * USEC_PER_SEC);

	/* constraint: 80MHz <= Fvco <= 1000Mhz */
	fvco_min = 80 * USEC_PER_SEC;
	fvco_max = 1000 * USEC_PER_SEC;

	best_freq = 0;
	for (_prediv = min_prediv; _prediv <= max_prediv; _prediv++) {
		/* Fvco = Fref * M / N */
		freq_factor = (uint64_t)fout * _prediv;
		do_div(freq_factor, fin);
		_fbdiv = freq_factor;
		/*
		 * Due to the use of a "by 2 pre-scaler," the range of the
		 * feedback multiplication value M is limited to even division
		 * numbers, and m must be greater than 6, not bigger than 512.
		 */
		if (_fbdiv < 6 || _fbdiv > 512)
			continue;

		_fbdiv += _fbdiv % 2;

		freq_factor = (uint64_t)_fbdiv * fin;
		do_div(freq_factor, _prediv);
		if (freq_factor < fvco_min || freq_factor > fvco_max)
			continue;

		delta = abs(fout - freq_factor);
		if (delta < min_delta) {
			best_prediv = _prediv;
			best_fbdiv = _fbdiv;
			min_delta = delta;
			best_freq = freq_factor;
		}
	}

	if (best_freq) {
		dsi->lane_mbps = DIV_ROUND_UP(best_freq, USEC_PER_SEC);
		*lane_mbps = dsi->lane_mbps;
		dsi->input_div = best_prediv;
		dsi->feedback_div = best_fbdiv;
	} else {
		dev_err(dsi->dev, "Can not find best_freq for DPHY\n");
		return -EINVAL;
	}

	return 0;
}

static int dw_mipi_dsi_mchp_get_timing(void *priv_data, unsigned int lane_mbps,
				       struct dw_mipi_dsi_dphy_timing *timing)
{
	int index;

	for (index = 0; index < ARRAY_SIZE(hstt_table); index++)
		if (lane_mbps < hstt_table[index].maxfreq)
			break;

	if (index == ARRAY_SIZE(hstt_table))
		index--;

	*timing = hstt_table[index].timing;

	return 0;
}

static void dw_mipi_dsi_mchp_power_on(void *priv_data)
{
	struct dw_mipi_dsi_mchp *dsi = priv_data;

	/* Enable the DSI wrapper */
	dsi_write(dsi, DSI_PWR_UP, HOST_PWRUP);
}

static void dw_mipi_dsi_mchp_power_off(void *priv_data)
{
	struct dw_mipi_dsi_mchp *dsi = priv_data;

	/* Disable the DSI wrapper */
	dsi_write(dsi, DSI_PWR_UP, HOST_RESET);
}

struct dw_mipi_dsi_phy_ops dw_mipi_dsi_mchp_phy_ops = {
	.init = dw_mipi_dsi_mchp_init,
	.power_on = dw_mipi_dsi_mchp_power_on,
	.power_off = dw_mipi_dsi_mchp_power_off,
	.get_lane_mbps = dw_mipi_dsi_mchp_get_lane_mbps,
	.get_timing = dw_mipi_dsi_mchp_get_timing,
};

static int dw_mipi_dsi_mchp_probe(struct platform_device *pdev)
{
	struct dw_mipi_dsi_mchp *dsi;
	struct resource *res;
	struct regmap *sfr;
	const struct dw_mipi_dsi_mchp_chip_data *cdata;
	int ret;

	dsi = devm_kzalloc(&pdev->dev, sizeof(*dsi), GFP_KERNEL);
	if (!dsi)
		return -ENOMEM;

	dsi->dev = &pdev->dev;
	cdata = of_device_get_match_data(dsi->dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dsi->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dsi->base)) {
		ret = PTR_ERR(dsi->base);
		dev_err(dsi->dev, "Unable to get DSI Base address: %d\n", ret);
		return ret;
	}

	dsi->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(dsi->pclk)) {
		ret = PTR_ERR(dsi->pclk);
		dev_err(dsi->dev, "Unable to get pclk: %d\n", ret);
		return ret;
	}

	dsi->pllref_clk = devm_clk_get(&pdev->dev, "refclk");
	if (IS_ERR(dsi->pllref_clk)) {
		ret = PTR_ERR(dsi->pllref_clk);
		dev_err(dsi->dev, "Unable to get DSI PHY PLL reference clock: %d\n",
			ret);
		return ret;
	}

	clk_set_rate(dsi->pllref_clk, DSI_PLL_REF_CLK);
	if (clk_get_rate(dsi->pllref_clk) != DSI_PLL_REF_CLK) {
		dev_err(dsi->dev, "Failed to set DSI PHY PLL reference clock to: %d\n",
			DSI_PLL_REF_CLK);
		return -1;
	}

	ret = clk_prepare_enable(dsi->pllref_clk);
	if (ret) {
		dev_err(dsi->dev, "Failed to enable DSI PHY PLL reference clock: %d\n",
			ret);
		return ret;
	}

	sfr = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "microchip,sfr");
	if (IS_ERR_OR_NULL(sfr)) {
		ret = PTR_ERR(sfr);
		dev_err(dsi->dev, "Failed to get handle on Special Function Register: %d\n",
			ret);
		goto err_dsi_probe;
	}
	/* Select DSI in SFR's ISS Configuration Register */
	ret = regmap_write(sfr, SFR_ISS_CFG, ISS_CFG_DSI_MODE);
	if (ret) {
		dev_err(dsi->dev, "Failed to enable DSI in SFR ISS configuration register: %d\n",
			ret);
		goto err_dsi_probe;
	}

	dsi->pdata.base = dsi->base;
	dsi->pdata.max_data_lanes = cdata->max_data_lanes;
	dsi->pdata.phy_ops = cdata->phy_ops;
	dsi->pdata.priv_data = dsi;
	platform_set_drvdata(pdev, dsi);

	/* call synopsis probe */
	dsi->dsi = dw_mipi_dsi_probe(pdev, &dsi->pdata);
	if (IS_ERR(dsi->dsi)) {
		ret = PTR_ERR(dsi->dsi);
		dev_err(dsi->dev, "Failed to initialize mipi dsi host: %d\n", ret);
		goto err_dsi_probe;
	}

	return 0;

err_dsi_probe:
	clk_disable_unprepare(dsi->pllref_clk);

	return ret;
}

static int dw_mipi_dsi_mchp_remove(struct platform_device *pdev)
{
	struct dw_mipi_dsi_mchp *dsi = platform_get_drvdata(pdev);

	dw_mipi_dsi_remove(dsi->dsi);
	clk_disable_unprepare(dsi->pllref_clk);

	return 0;
}

static const struct dw_mipi_dsi_mchp_chip_data sam9x75_chip_data = {
	.max_data_lanes = 4,
	.phy_ops = &dw_mipi_dsi_mchp_phy_ops,
};

static const struct of_device_id dw_mipi_dsi_mchp_dt_ids[] = {
	{
	 .compatible	= "microchip,sam9x75-mipi-dsi",
	 .data		= &sam9x75_chip_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dw_mipi_dsi_mchp_dt_ids);

struct platform_driver dw_mipi_dsi_mchp_driver = {
	.probe		= dw_mipi_dsi_mchp_probe,
	.remove		= dw_mipi_dsi_mchp_remove,
	.driver		= {
		.of_match_table = dw_mipi_dsi_mchp_dt_ids,
		.name		= "dw-mipi-dsi-mchp",
	},
};
module_platform_driver(dw_mipi_dsi_mchp_driver);

MODULE_AUTHOR("Manikandan Muralidharan <manikandan.m@microchip.com>");
MODULE_DESCRIPTION("Microchip DW MIPI DSI Host Controller Driver");
MODULE_LICENSE("GPL");
