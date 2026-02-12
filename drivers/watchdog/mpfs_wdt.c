// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watchdog driver for Microchip PolarFire SoC watchdog.
 *
 * Copyright (C) 2025 Microchip Corporation
 */
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>

#define MPFS_WDT_REFRESH	0x0
#define  MPFS_WDT_MAGIC_WORD	0xdeadc0de
#define MPFS_WDT_CONTROL	0x4
#define MPFS_WDT_STATUS		0x8
#define  MPFS_MVRP_TRIPPED	0x1
#define  MPFS_WDOG_TRIPPED	0x2
#define MPFS_WDT_TIME		0xc
#define MPFS_WDT_MSVP		0x10

/* 24-bit counter */
#define MPFS_WDT_LOAD_MAX	0xfffff0
#define MPFS_WDT_LOAD_MIN	1

#define MPFS_WDT_CLK_PRESCALER	256

struct mpfs_wdt {
	struct watchdog_device wdd;
	struct clk *clk;
	unsigned int rate_hz;
	int mvrp_irq;
	int trig_irq;
	void __iomem *base;
};

static void mpfs_wdt_set_timeout_reg(struct watchdog_device *wdog)
{
	struct mpfs_wdt *mpfs_wdt = watchdog_get_drvdata(wdog);
	u32 trig_counter = wdog->timeout * mpfs_wdt->rate_hz;

	writel(trig_counter, mpfs_wdt->base + MPFS_WDT_TIME);
}

static void mpfs_wdt_set_pretimeout_reg(struct watchdog_device *wdog)
{
	struct mpfs_wdt *mpfs_wdt = watchdog_get_drvdata(wdog);
	u32 mvrp_counter = 0;
	u32 mvrp_secs;

	if (wdog->pretimeout) {
		mvrp_secs = wdog->timeout - wdog->pretimeout;
		mvrp_counter = mvrp_secs * mpfs_wdt->rate_hz;
 	}

	writel(mvrp_counter, mpfs_wdt->base + MPFS_WDT_MSVP);
}

static int mpfs_wdt_ping(struct watchdog_device *wdog)
{
	struct mpfs_wdt *mpfs_wdt = watchdog_get_drvdata(wdog);

	writel(MPFS_WDT_MAGIC_WORD, mpfs_wdt->base + MPFS_WDT_REFRESH);

	return 0;
}

static int mpfs_wdt_start(struct watchdog_device *wdog)
{
	return mpfs_wdt_ping(wdog);
}

static int mpfs_wdt_stop(struct watchdog_device *wdog)
{
	/* can't stop the dog, just return */
	return 0;
}

static int mpfs_wdt_set_timeout(struct watchdog_device *wdog, u32 timeout_secs)
{
	wdog->timeout = timeout_secs;
	mpfs_wdt_set_pretimeout_reg(wdog);
	mpfs_wdt_set_timeout_reg(wdog);

	return 0;
}

static int mpfs_wdt_set_pretimeout(struct watchdog_device *wdog, u32 timeout_secs)
{
	if (timeout_secs > wdog->timeout)
		return -EINVAL;

	wdog->pretimeout = timeout_secs;
	mpfs_wdt_set_pretimeout_reg(wdog);

	return 0;
}

static unsigned int mpfs_wdt_get_timeleft(struct watchdog_device *wdog)
{
	struct mpfs_wdt *mpfs_wdt = watchdog_get_drvdata(wdog);
	u32 time_secs;

	time_secs = readl(mpfs_wdt->base + MPFS_WDT_REFRESH) / mpfs_wdt->rate_hz;

	return time_secs;
}

static irqreturn_t mpfs_wdt_trig_isr(int irq, void *dev_id)
{
	struct mpfs_wdt *mpfs_wdt = dev_id;

	writel(MPFS_WDOG_TRIPPED, mpfs_wdt->base + MPFS_WDT_STATUS);

	dev_crit(mpfs_wdt->wdd.parent, "Microchip PolarFire SoC wdt tripped\n");
	emergency_restart();

	return IRQ_HANDLED;
}

static irqreturn_t mpfs_wdt_mvrp_isr(int irq, void *dev_id)
{
	struct mpfs_wdt *mpfs_wdt = dev_id;

	writel(MPFS_MVRP_TRIPPED, mpfs_wdt->base + MPFS_WDT_STATUS);
	watchdog_notify_pretimeout(&mpfs_wdt->wdd);

	return IRQ_HANDLED;
}

static const struct watchdog_info mpfs_wdt_info = {
	.identity = "Microchip PolarFire SoC Watchdog",
	.options = WDIOF_SETTIMEOUT |
		   WDIOF_PRETIMEOUT |
		   WDIOF_MAGICCLOSE |
		   WDIOF_KEEPALIVEPING,
};

static const struct watchdog_ops mpfs_wdt_ops = {
	.owner = THIS_MODULE,
	.start = mpfs_wdt_start,
	.stop = mpfs_wdt_stop,
	.ping = mpfs_wdt_ping,
	.set_timeout = mpfs_wdt_set_timeout,
	.set_pretimeout = mpfs_wdt_set_pretimeout,
	.get_timeleft = mpfs_wdt_get_timeleft,
};

static int mpfs_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpfs_wdt *mpfs_wdt;
	int ret;

	mpfs_wdt = devm_kzalloc(dev, sizeof(*mpfs_wdt), GFP_KERNEL);
	if (!mpfs_wdt)
		return -ENOMEM;

	mpfs_wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mpfs_wdt->base))
		return PTR_ERR(mpfs_wdt->base);

	mpfs_wdt->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(mpfs_wdt->clk))
		return dev_err_probe(dev, PTR_ERR(mpfs_wdt->clk), "Failed to get clock\n");

	mpfs_wdt->rate_hz = clk_get_rate(mpfs_wdt->clk) / MPFS_WDT_CLK_PRESCALER;
	if (!mpfs_wdt->rate_hz)
		return dev_err_probe(dev, -EINVAL, "Failed to get clock rate\n");

	mpfs_wdt->mvrp_irq = platform_get_irq_byname(pdev, "mvrp");
	if (mpfs_wdt->mvrp_irq < 0)
		return dev_err_probe(dev, mpfs_wdt->mvrp_irq, "Failed to get IRQ for mvrp\n");

	ret = devm_request_irq(dev, mpfs_wdt->mvrp_irq, mpfs_wdt_mvrp_isr, 0,
			       "mpfs-wdt", mpfs_wdt);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ for trig\n");

	mpfs_wdt->trig_irq = platform_get_irq_byname(pdev, "trig");
	if (mpfs_wdt->trig_irq < 0)
		return dev_err_probe(dev, mpfs_wdt->trig_irq, "Failed to get IRQ for trig\n");

	ret = devm_request_irq(dev, mpfs_wdt->trig_irq, mpfs_wdt_trig_isr, 0,
			       "mpfs-wdt", mpfs_wdt);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ for timeout\n");

	mpfs_wdt->wdd.parent = dev;
	mpfs_wdt->wdd.info = &mpfs_wdt_info;
	mpfs_wdt->wdd.ops = &mpfs_wdt_ops;
	mpfs_wdt->wdd.min_timeout = MPFS_WDT_LOAD_MIN / mpfs_wdt->rate_hz;
	mpfs_wdt->wdd.max_timeout = MPFS_WDT_LOAD_MAX / mpfs_wdt->rate_hz;
	mpfs_wdt->wdd.timeout = MPFS_WDT_LOAD_MAX / mpfs_wdt->rate_hz;
	mpfs_wdt->wdd.pretimeout = MPFS_WDT_LOAD_MAX / (mpfs_wdt->rate_hz * 2);

	watchdog_set_drvdata(&mpfs_wdt->wdd, mpfs_wdt);
	watchdog_set_nowayout(&mpfs_wdt->wdd, true);
	watchdog_init_timeout(&mpfs_wdt->wdd, mpfs_wdt->wdd.timeout, dev);
	mpfs_wdt_set_pretimeout(&mpfs_wdt->wdd, mpfs_wdt->wdd.pretimeout);
	mpfs_wdt_set_timeout(&mpfs_wdt->wdd, mpfs_wdt->wdd.timeout);

	ret = devm_watchdog_register_device(dev, &mpfs_wdt->wdd);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register watchdog device.\n");

	platform_set_drvdata(pdev, mpfs_wdt);
	dev_info(dev, "timeout %d sec (nowayout=true)\n",
		mpfs_wdt->wdd.timeout);

	return 0;
}

static const struct of_device_id mpfs_wdt_match[] = {
	{ .compatible = "microchip,mpfs-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, mpfs_wdt_match);

static struct platform_driver mpfs_wdt_driver = {
	.probe	= mpfs_wdt_probe,
	.driver	= {
		.name = "mpfs_wdt",
		.of_match_table	= mpfs_wdt_match,
	},
};

module_platform_driver(mpfs_wdt_driver);

MODULE_DESCRIPTION("Microchip PolarFire SoC watchdog driver");
MODULE_AUTHOR("Daire McNamara <daire.mcnamara@microchip.com");
MODULE_LICENSE("GPL v2");
