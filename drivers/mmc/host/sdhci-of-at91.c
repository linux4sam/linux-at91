// SPDX-License-Identifier: GPL-2.0-only
/*
 * Atmel SDMMC controller driver.
 *
 * Copyright (C) 2015 Atmel,
 *		 2015 Ludovic Desroches <ludovic.desroches@atmel.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mmc/host.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>

#include "sdhci-pltfm.h"

#define SDMMC_MC1R	0x204
#define		SDMMC_MC1R_DDR		BIT(3)
#define		SDMMC_MC1R_RSTN		BIT(6)
#define		SDMMC_MC1R_FCD		BIT(7)
#define SDMMC_MC3R	0x206
#define		SDMMC_MC3R_HS400EN	BIT(0)
#define		SDMMC_MC3R_ESMEN	BIT(1)
#define SDMMC_CACR	0x230
#define		SDMMC_CACR_CAPWREN	BIT(0)
#define		SDMMC_CACR_KEY		(0x46 << 8)
#define SDMMC_CALCR	0x240
#define		SDMMC_CALCR_EN		BIT(0)
#define		SDMMC_CALCR_ALWYSON	BIT(4)

#define SDHCI_AT91_PRESET_COMMON_CONF	0x400 /* drv type B, programmable clock mode */

/* drv type A, programmable clock mode */
#define SDHCI_AT91_PRESET_DRVA_CONF	(SDHCI_AT91_PRESET_COMMON_CONF \
					 | 0x4000)

#define SDHCI_AT91_RETRY_TUN_MAX	10

struct sdhci_at91_soc_data {
	const struct sdhci_pltfm_data *pdata;
	bool baseclk_is_generated_internally;
	unsigned int divider_for_baseclk;
	unsigned int max_sdr104_clk;
	bool pm_runtime_disable_clks;
	bool final_tun_brdrdy_masked;
	u32 quirks2;
};

struct sdhci_at91_priv {
	const struct sdhci_at91_soc_data *soc_data;
	struct clk *hclock;
	struct clk *gck;
	struct clk *mainck;
	unsigned long hclock_ns;
	bool restore_needed;
	bool cal_always_on;
};

static void sdhci_at91_set_force_card_detect(struct sdhci_host *host)
{
	u8 mc1r;

	mc1r = readb(host->ioaddr + SDMMC_MC1R);
	mc1r |= SDMMC_MC1R_FCD;
	writeb(mc1r, host->ioaddr + SDMMC_MC1R);
}

static void sdhci_at91_set_clock(struct sdhci_host *host, unsigned int clock)
{
	u16 clk;

	host->mmc->actual_clock = 0;

	/*
	 * There is no requirement to disable the internal clock before
	 * changing the SD clock configuration. Moreover, disabling the
	 * internal clock, changing the configuration and re-enabling the
	 * internal clock causes some bugs. It can prevent to get the internal
	 * clock stable flag ready and an unexpected switch to the base clock
	 * when using presets.
	 */
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk &= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		return;

	clk = sdhci_calc_clk(host, clock, &host->mmc->actual_clock);

	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Wait max 20 ms */
	if (read_poll_timeout(sdhci_readw, clk, (clk & SDHCI_CLOCK_INT_STABLE),
			      1000, 20000, false, host, SDHCI_CLOCK_CONTROL)) {
		pr_err("%s: Internal clock never stabilised.\n",
		       mmc_hostname(host->mmc));
		return;
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
}

static int sdhci_at91_platform_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv *priv = sdhci_pltfm_priv(pltfm_host);
	unsigned int retries;
	int i, err = -EAGAIN;
	u16 ctrl;

	/* DDR50 is tuning-free and hardwired to the fixed sampling clock */
	if (host->timing == MMC_TIMING_UHS_DDR50)
		return 0;

	/* SDMMC only has Re-Tuning Mode 1 (Timer); use retune_timer instead */
	host->mmc->retune_period = host->tuning_count;

	for (retries = 0; retries < SDHCI_AT91_RETRY_TUN_MAX; retries++) {
		/* We may start tuning multiple times, end once */
		sdhci_start_tuning(host);

		/*
		 * The tuning HW iterates over an immutable number of phases.
		 * Issue opcode repeatedly till all phases have been tested,
		 * regardless of the Execute Tuning bit.
		 */
		for (i = 0; i < host->tuning_loop_count; i++) {
			sdhci_send_tuning(host, opcode);
			if (host->tuning_done)
				continue;
			if (priv->soc_data->final_tun_brdrdy_masked &&
			    i == host->tuning_loop_count - 1 &&
			    !(sdhci_readw(host, SDHCI_HOST_CONTROL2) &
			      SDHCI_CTRL_EXEC_TUNING))
				/*
				 * SAMA5D2 silicon erratum just caused
				 * sdhci_send_tuning to time out waiting for
				 * Buffer Read Ready. Proceed.
				 */
				break;

			dev_warn(mmc_dev(host->mmc),
				 "%s: tuning cmd error, falling back to fixed sampling clock\n",
				 mmc_hostname(host->mmc));
			err = -ETIMEDOUT;
			host->tuning_err = err;
			sdhci_abort_tuning(host, opcode);
			return err;
		}

		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if ((ctrl & (SDHCI_CTRL_TUNED_CLK | SDHCI_CTRL_EXEC_TUNING)) ==
		    SDHCI_CTRL_TUNED_CLK) {
			err = 0;
			break;
		}

		/*
		 * Due to a silicon erratum, SAMA7D65 occasionally fails to
		 * autoclear the Execute Tuning bit. Recovering requires
		 * clearing the bit manually.
		 * Also, SAMA5D2 MPUs on rare occasions fail to delimit the
		 * sampling window, and then will not set the Sampling Clock
		 * Select bit.
		 * In both cases reset the tuning circuit, and retry the tuning
		 * sequence.
		 */
		sdhci_reset_tuning(host);
		/*
		 * After clearing the Execute Tuning bit, allow one peripheral
		 * clock cycle for the clear event to act before possibly
		 * setting the bit again.
		 */
		ndelay(priv->hclock_ns);
	}

	if (err)
		dev_warn(mmc_dev(host->mmc),
			 "%s: tuning failed, fell back to fixed sampling clock\n",
			 mmc_hostname(host->mmc));
#ifdef CONFIG_MMC_DEBUG
	if (!err && retries >= 2)
		dev_dbg(mmc_dev(host->mmc), "%s: had to tune %u times\n",
			mmc_hostname(host->mmc), 1u + retries);
#endif
	host->tuning_err = err;
	sdhci_end_tuning(host);
	return err;
}

static void sdhci_at91_set_uhs_signaling(struct sdhci_host *host,
					 unsigned int timing)
{
	u16 clk;
	u8 mc3r, mc1r;

	mc1r = readb(host->ioaddr + SDMMC_MC1R);
	mc3r = readb(host->ioaddr + SDMMC_MC3R);
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);

	/* SDCLK must be disabled while changing the mode */
	if (clk & SDHCI_CLOCK_CARD_EN)
		sdhci_writew(host, clk & ~SDHCI_CLOCK_CARD_EN,
			     SDHCI_CLOCK_CONTROL);

	if (timing == MMC_TIMING_MMC_DDR52 || timing == MMC_TIMING_MMC_HS400)
		mc1r |= SDMMC_MC1R_DDR;
	else
		mc1r &= ~SDMMC_MC1R_DDR;

	sdhci_writeb(host, mc1r, SDMMC_MC1R);

	if (timing == MMC_TIMING_MMC_HS400)
		mc3r |= SDMMC_MC3R_HS400EN;
	else
		mc3r &= ~SDMMC_MC3R_HS400EN;

	writeb(mc3r, host->ioaddr + SDMMC_MC3R);

	sdhci_set_uhs_signaling(host, timing);

	/* reenable SDCLK */
	if (clk & SDHCI_CLOCK_CARD_EN) {
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		sdhci_writew(host, clk | SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	}
}

static void sdhci_at91_reset(struct sdhci_host *host, u8 mask)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv *priv = sdhci_pltfm_priv(pltfm_host);
	unsigned int tmp;
	u16 clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);

	/* SDCLK must be disabled while resetting the HW block */
	if (clk & SDHCI_CLOCK_CARD_EN)
		sdhci_writew(host, clk & ~SDHCI_CLOCK_CARD_EN,
			     SDHCI_CLOCK_CONTROL);

	sdhci_reset(host, mask);

	/* reenable SDCLK */
	if (clk & SDHCI_CLOCK_CARD_EN) {
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		sdhci_writew(host, clk | SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	}

	if ((host->mmc->caps & MMC_CAP_NONREMOVABLE)
	    || mmc_gpio_get_cd(host->mmc) >= 0)
		sdhci_at91_set_force_card_detect(host);

	if (priv->cal_always_on && (mask & SDHCI_RESET_ALL)) {
		u32 calcr = sdhci_readl(host, SDMMC_CALCR);

		sdhci_writel(host, calcr | SDMMC_CALCR_ALWYSON | SDMMC_CALCR_EN,
			     SDMMC_CALCR);

		if (read_poll_timeout(sdhci_readl, tmp, !(tmp & SDMMC_CALCR_EN),
				      10, 20000, false, host, SDMMC_CALCR))
			dev_err(mmc_dev(host->mmc), "Failed to calibrate\n");
	}
}

static void sdhci_at91_hw_reset(struct sdhci_host *host)
{
	u8 mc1r;

	mc1r = readb(host->ioaddr + SDMMC_MC1R);

	mc1r |= SDMMC_MC1R_RSTN;
	writeb(mc1r, host->ioaddr + SDMMC_MC1R);

	fsleep(10);

	mc1r &= ~SDMMC_MC1R_RSTN;
	writeb(mc1r, host->ioaddr + SDMMC_MC1R);

	/* JEDEC specifies a minimum of 200us for tRSCA (reset to command) */
	usleep_range(200, 500);
}

static const struct sdhci_ops sdhci_at91_sama5d2_ops = {
	.set_clock		= sdhci_at91_set_clock,
	.set_bus_width		= sdhci_set_bus_width,
	.reset			= sdhci_at91_reset,
	.platform_execute_tuning = sdhci_at91_platform_execute_tuning,
	.set_uhs_signaling	= sdhci_at91_set_uhs_signaling,
	.set_power		= sdhci_set_power_and_bus_voltage,
	.hw_reset		= sdhci_at91_hw_reset,
};

static const struct sdhci_pltfm_data sdhci_sama5d2_pdata = {
	.ops = &sdhci_at91_sama5d2_ops,
};

static const struct sdhci_at91_soc_data soc_data_sama5d2 = {
	.pdata = &sdhci_sama5d2_pdata,
	.baseclk_is_generated_internally = false,
	.max_sdr104_clk = 120000000,
	.pm_runtime_disable_clks = true,
	.final_tun_brdrdy_masked = true,
	.quirks2 = SDHCI_QUIRK2_BROKEN_HS200,
};

static const struct sdhci_at91_soc_data soc_data_sam9x60 = {
	.pdata = &sdhci_sama5d2_pdata,
	.baseclk_is_generated_internally = true,
	.divider_for_baseclk = 2,
	.pm_runtime_disable_clks = true,
};

static const struct sdhci_at91_soc_data soc_data_sama7g5 = {
	.pdata = &sdhci_sama5d2_pdata,
	.baseclk_is_generated_internally = true,
	.divider_for_baseclk = 2,
	.max_sdr104_clk = 200000000,
	.quirks2 = SDHCI_QUIRK2_AT91_HS400_PRESET,
};

static const struct of_device_id sdhci_at91_dt_match[] = {
	{ .compatible = "atmel,sama5d2-sdhci", .data = &soc_data_sama5d2 },
	{ .compatible = "microchip,sam9x60-sdhci", .data = &soc_data_sam9x60 },
	{ .compatible = "microchip,sama7g5-sdhci", .data = &soc_data_sama7g5 },
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_at91_dt_match);

static int sdhci_at91_set_clks_presets(struct device *dev, bool start_clks)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv *priv = sdhci_pltfm_priv(pltfm_host);
	unsigned int			caps0, caps1;
	unsigned int			clk_base, clk_mul;
	unsigned int			gck_rate, clk_base_rate;
	unsigned int			preset_div;

	/*
	 * Manage clock prepare/enable procedure depending on if the board
	 * support runtime clock disabling and if we come from the probing
	 * or restoring procedure (for backup+self refresh).
	 */
	if (start_clks)
		clk_prepare_enable(priv->hclock);

	caps0 = readl(host->ioaddr + SDHCI_CAPABILITIES);
	caps1 = readl(host->ioaddr + SDHCI_CAPABILITIES_1);

	/*
	 * We experience some issues with SDR104. If the SD clock is higher
	 * than 100 MHz, we can get data corruption. With a 100 MHz clock,
	 * the tuning procedure may fail. For those reasons, it is useless to
	 * advertise that we can use SDR104 mode, so remove it from
	 * the capabilities.
	 */
	writel(SDMMC_CACR_KEY | SDMMC_CACR_CAPWREN, host->ioaddr + SDMMC_CACR);
	caps1 &= (~SDHCI_SUPPORT_SDR104);
	/*
	 * Capabilities in silicon typically avoid specifying the re-tuning
	 * period. Instead, they code the 'Get information from other source'
	 * case. In that case, default to 32 sec.
	 */
	if ((caps1 & SDHCI_RETUNING_TIMER_COUNT_MASK) ==
		SDHCI_RETUNING_TIMER_COUNT_MASK) {
		caps1 &= ~SDHCI_RETUNING_TIMER_COUNT_MASK;
		caps1 |= FIELD_PREP(SDHCI_RETUNING_TIMER_COUNT_MASK, 0x6);
	}
	writel(caps1, host->ioaddr + SDHCI_CAPABILITIES_1);
	writel(0, host->ioaddr + SDMMC_CACR);

	gck_rate = clk_get_rate(priv->gck);
	if (priv->soc_data->baseclk_is_generated_internally)
		clk_base_rate = gck_rate / priv->soc_data->divider_for_baseclk;
	else
		clk_base_rate = clk_get_rate(priv->mainck);

	clk_base = clk_base_rate / 1000000;
	clk_mul = gck_rate / clk_base_rate - 1;

	caps0 &= ~SDHCI_CLOCK_V3_BASE_MASK;
	caps0 |= FIELD_PREP(SDHCI_CLOCK_V3_BASE_MASK, clk_base);
	caps1 &= ~SDHCI_CLOCK_MUL_MASK;
	caps1 |= FIELD_PREP(SDHCI_CLOCK_MUL_MASK, clk_mul);
	/* Set capabilities in r/w mode. */
	writel(SDMMC_CACR_KEY | SDMMC_CACR_CAPWREN, host->ioaddr + SDMMC_CACR);
	writel(caps0, host->ioaddr + SDHCI_CAPABILITIES);
	writel(caps1, host->ioaddr + SDHCI_CAPABILITIES_1);
	/* Set capabilities in ro mode. */
	writel(0, host->ioaddr + SDMMC_CACR);

	dev_dbg(dev, "update clk mul to %u as gck rate is %u Hz and clk base is %u Hz\n",
		clk_mul, gck_rate, clk_base_rate);

	/*
	 * We have to set preset values because it depends on the clk_mul
	 * value. Moreover, SDR104 is supported in a degraded mode since the
	 * maximum sd clock value is 120 MHz instead of 208 MHz. For that
	 * reason, we need to use presets to support SDR104.
	 */
	preset_div = DIV_ROUND_UP(gck_rate, 24000000) - 1;
	writew(SDHCI_AT91_PRESET_COMMON_CONF | preset_div,
	       host->ioaddr + SDHCI_PRESET_FOR_SDR12);
	preset_div = DIV_ROUND_UP(gck_rate, 50000000) - 1;
	writew(SDHCI_AT91_PRESET_COMMON_CONF | preset_div,
	       host->ioaddr + SDHCI_PRESET_FOR_SDR25);
	preset_div = DIV_ROUND_UP(gck_rate, 100000000) - 1;
	writew(SDHCI_AT91_PRESET_COMMON_CONF | preset_div,
	       host->ioaddr + SDHCI_PRESET_FOR_SDR50);
	if (priv->soc_data->max_sdr104_clk) {
		preset_div = DIV_ROUND_UP(gck_rate,
					  priv->soc_data->max_sdr104_clk) - 1;
		writew(SDHCI_AT91_PRESET_COMMON_CONF | preset_div,
		       host->ioaddr + SDHCI_PRESET_FOR_SDR104);
	}
	preset_div = DIV_ROUND_UP(gck_rate, 50000000) - 1;
	writew(SDHCI_AT91_PRESET_COMMON_CONF | preset_div,
	       host->ioaddr + SDHCI_PRESET_FOR_DDR50);
	if (priv->soc_data->max_sdr104_clk) {
		preset_div = DIV_ROUND_UP(gck_rate,
					  priv->soc_data->max_sdr104_clk) - 1;
		writew(SDHCI_AT91_PRESET_DRVA_CONF | preset_div,
		       host->ioaddr + SDHCI_PRESET_FOR_HS400);
	}

	/* Same clock management as for hclock */
	if (start_clks) {
		clk_prepare_enable(priv->mainck);
		clk_prepare_enable(priv->gck);
	}

	return 0;
}

static int sdhci_at91_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv *priv = sdhci_pltfm_priv(pltfm_host);
	int ret;

	ret = pm_runtime_force_suspend(dev);

	priv->restore_needed = true;

	return ret;
}

static int sdhci_at91_runtime_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv *priv = sdhci_pltfm_priv(pltfm_host);

	sdhci_runtime_suspend_host(host);

	if (host->tuning_mode != SDHCI_TUNING_MODE_3)
		mmc_retune_needed(host->mmc);

	if (priv->soc_data->pm_runtime_disable_clks) {
		clk_disable_unprepare(priv->gck);
		clk_disable_unprepare(priv->hclock);
		clk_disable_unprepare(priv->mainck);
	}

	return 0;
}

static int sdhci_at91_runtime_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv *priv = sdhci_pltfm_priv(pltfm_host);
	int ret;

	if (priv->restore_needed) {
		ret = sdhci_at91_set_clks_presets(dev, priv->soc_data->pm_runtime_disable_clks);
		if (ret)
			return ret;

		priv->restore_needed = false;
		goto out;
	}

	if (!priv->soc_data->pm_runtime_disable_clks)
		goto out;

	ret = clk_prepare_enable(priv->mainck);
	if (ret) {
		dev_err(dev, "can't enable mainck\n");
		return ret;
	}

	ret = clk_prepare_enable(priv->hclock);
	if (ret) {
		dev_err(dev, "can't enable hclock\n");
		return ret;
	}

	ret = clk_prepare_enable(priv->gck);
	if (ret) {
		dev_err(dev, "can't enable gck\n");
		return ret;
	}

out:
	sdhci_runtime_resume_host(host, 0);
	return 0;
}

static const struct dev_pm_ops sdhci_at91_dev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(sdhci_at91_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(sdhci_at91_runtime_suspend, sdhci_at91_runtime_resume, NULL)
};

static void at91_sdhci_hs400_enhanced_strobe(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u8 mc3r;

	mc3r = readb(host->ioaddr + SDMMC_MC3R);
	if (ios->enhanced_strobe)
		mc3r |= SDMMC_MC3R_ESMEN;
	else
		mc3r &= ~SDMMC_MC3R_ESMEN;

	writeb(mc3r, host->ioaddr + SDMMC_MC3R);
}

static int sdhci_at91_probe(struct platform_device *pdev)
{
	const struct sdhci_at91_soc_data	*soc_data;
	struct sdhci_host		*host;
	struct sdhci_pltfm_host		*pltfm_host;
	struct sdhci_at91_priv		*priv;
	unsigned long			rate;
	int				ret;

	soc_data = of_device_get_match_data(&pdev->dev);
	if (!soc_data)
		return -EINVAL;

	host = sdhci_pltfm_init(pdev, soc_data->pdata, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);
	priv->soc_data = soc_data;

	if (of_device_is_compatible(pdev->dev.of_node, "atmel,sama5d2-sdhci"))
		host->tuning_loop_count = 34;
	else
		host->tuning_loop_count = 17;

	/* Perform a software reset before using the IP */
	sdhci_at91_reset(host, SDHCI_RESET_ALL);
	/* Perform a hardware reset before using the IP */
	sdhci_at91_hw_reset(host);

	priv->mainck = devm_clk_get(&pdev->dev, "baseclk");
	if (IS_ERR(priv->mainck)) {
		if (soc_data->baseclk_is_generated_internally)
			priv->mainck = NULL;
		else
			return dev_err_probe(&pdev->dev, PTR_ERR(priv->mainck),
					     "failed to get baseclk\n");
	}

	priv->hclock = devm_clk_get(&pdev->dev, "hclock");
	if (IS_ERR(priv->hclock))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->hclock),
				     "failed to get hclock\n");

	priv->gck = devm_clk_get(&pdev->dev, "multclk");
	if (IS_ERR(priv->gck))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->gck),
				     "failed to get multclk\n");

	ret = sdhci_at91_set_clks_presets(&pdev->dev, true);
	if (ret)
		return ret;
	rate = clk_get_rate(priv->hclock);
	priv->hclock_ns = DIV_ROUND_UP(NSEC_PER_SEC, rate);

	priv->restore_needed = false;

	/*
	 * if SDCAL pin is wrongly connected, we must enable
	 * the analog calibration cell permanently.
	 */
	priv->cal_always_on =
		device_property_read_bool(&pdev->dev,
					  "microchip,sdcal-inverted");

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto clocks_disable_unprepare;

	sdhci_get_of_property(pdev);

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, 50);
	pm_runtime_use_autosuspend(&pdev->dev);

	host->quirks2 |= priv->soc_data->quirks2;

	ret = sdhci_add_host(host);
	if (ret)
		goto pm_runtime_disable;

	host->mmc_host_ops.hs400_enhanced_strobe = at91_sdhci_hs400_enhanced_strobe;

	/*
	 * When calling sdhci_runtime_suspend_host(), the sdhci layer makes
	 * the assumption that all the clocks of the controller are disabled.
	 * It means we can't get irq from it when it is runtime suspended.
	 * For that reason, it is not planned to wake-up on a card detect irq
	 * from the controller.
	 * If we want to use runtime PM and to be able to wake-up on card
	 * insertion, we have to use a GPIO for the card detection or we can
	 * use polling. Be aware that using polling will resume/suspend the
	 * controller between each attempt.
	 * Disable SDHCI_QUIRK_BROKEN_CARD_DETECTION to be sure nobody tries
	 * to enable polling via device tree with broken-cd property.
	 */
	if (mmc_card_is_removable(host->mmc) &&
	    mmc_gpio_get_cd(host->mmc) < 0) {
		host->mmc->caps |= MMC_CAP_NEEDS_POLL;
		host->quirks &= ~SDHCI_QUIRK_BROKEN_CARD_DETECTION;
	}

	/*
	 * If the device attached to the MMC bus is not removable, it is safer
	 * to set the Force Card Detect bit. People often don't connect the
	 * card detect signal and use this pin for another purpose. If the card
	 * detect pin is not muxed to SDHCI controller, a default value is
	 * used. This value can be different from a SoC revision to another
	 * one. Problems come when this default value is not card present. To
	 * avoid this case, if the device is non removable then the card
	 * detection procedure using the SDMCC_CD signal is bypassed.
	 * This bit is reset when a software reset for all command is performed
	 * so we need to implement our own reset function to set back this bit.
	 *
	 * WA: SAMA5D2 doesn't drive CMD if using CD GPIO line.
	 */
	if ((host->mmc->caps & MMC_CAP_NONREMOVABLE)
	    || mmc_gpio_get_cd(host->mmc) >= 0)
		sdhci_at91_set_force_card_detect(host);

	pm_runtime_put_autosuspend(&pdev->dev);

	return 0;

pm_runtime_disable:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
clocks_disable_unprepare:
	clk_disable_unprepare(priv->gck);
	clk_disable_unprepare(priv->mainck);
	clk_disable_unprepare(priv->hclock);
	return ret;
}

static void sdhci_at91_remove(struct platform_device *pdev)
{
	struct sdhci_host	*host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host	*pltfm_host = sdhci_priv(host);
	struct sdhci_at91_priv	*priv = sdhci_pltfm_priv(pltfm_host);
	struct clk *gck = priv->gck;
	struct clk *hclock = priv->hclock;
	struct clk *mainck = priv->mainck;

	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	sdhci_pltfm_remove(pdev);

	clk_disable_unprepare(gck);
	clk_disable_unprepare(hclock);
	clk_disable_unprepare(mainck);
}

static struct platform_driver sdhci_at91_driver = {
	.driver		= {
		.name	= "sdhci-at91",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = sdhci_at91_dt_match,
		.pm	= pm_ptr(&sdhci_at91_dev_pm_ops),
	},
	.probe		= sdhci_at91_probe,
	.remove		= sdhci_at91_remove,
};

module_platform_driver(sdhci_at91_driver);

MODULE_DESCRIPTION("SDHCI driver for at91");
MODULE_AUTHOR("Ludovic Desroches <ludovic.desroches@atmel.com>");
MODULE_LICENSE("GPL v2");
