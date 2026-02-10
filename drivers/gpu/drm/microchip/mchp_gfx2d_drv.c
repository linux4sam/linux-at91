// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_module.h>
#include <drm/drm_print.h>

#include "mchp_gfx2d_cmd.h"
#include "mchp_gfx2d_drv.h"
#include "mchp_gfx2d_gem.h"
#include "mchp_gfx2d_ioctl.h"

#define GFX2D_PM_RUNTIME_DELAY          5000
#define GFX2D_PM_RUNTIME_SUSPEND_DELAY  500

static int mchp_gfx2d_open(struct drm_device *dev, struct drm_file *file_priv)
{
	struct mchp_gfx2d_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	file_priv->driver_priv = ctx;

	return 0;
}

static void mchp_gfx2d_postclose(struct drm_device *dev, struct drm_file *file_priv)
{
	struct mchp_gfx2d_file *ctx = file_priv->driver_priv;
	struct mchp_gfx2d_device *priv = drm_to_dev(dev);

	mchp_gfx2d_cancel_commands(priv, ctx);

	kfree(ctx);
}

DEFINE_DRM_GEM_FOPS(mchp_gfx2d_drm_fops);

static const struct drm_driver mchp_gfx2d_drm_driver = {
	.major = 1,
	.minor = 0,
	.name = "microchip-gfx2d",
	.desc = "Microchip GFX2D DRM",
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.open = mchp_gfx2d_open,
	.postclose = mchp_gfx2d_postclose,
	.gem_prime_import_sg_table = mchp_gfx2d_gem_prime_import_sg_table,
	.ioctls = mchp_gfx2d_ioctls,
	.num_ioctls = ARRAY_SIZE(mchp_gfx2d_ioctls),
	.fops = &mchp_gfx2d_drm_fops,
};

static irqreturn_t mchp_gfx2d_interrupt(int irq, void *dev_id)
{
	struct mchp_gfx2d_device *priv = dev_id;
	u32 status, mask, pending;

	status = readl(priv->regs + GFX2D_IS);
	mask = readl(priv->regs + GFX2D_IM);
	pending = status & mask;

	if (!pending)
		return IRQ_NONE;

	if (pending & GFX2D_IRQ_EXEND)
		return IRQ_WAKE_THREAD;

	return IRQ_HANDLED;
}

static irqreturn_t mchp_gfx2d_thread(int irq, void *dev_id)
{
	struct mchp_gfx2d_device *priv = dev_id;
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	struct device *dev = priv->drm.dev;
	bool is_suspended = false;
	bool enable_exend = false;
	int ret;

	drm_dbg(&priv->drm, "enter IRQ thread\n");

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto exit;

	writel(GFX2D_IRQ_EXEND, priv->regs + GFX2D_ID);

	do {} while (mchp_gfx2d_progress(priv));

	mutex_lock(&priv->cmdlist_mutex);
	is_suspended = priv->is_suspended;
	mutex_unlock(&priv->cmdlist_mutex);

	/*
	 * When suspended, the EXEND interrupt is already enabled from
	 * mchp_gfx2d_process_completed_commands() if needed.
	 */
	if (is_suspended)
		goto put_autosuspend;

	mutex_lock(&priv->wlist_mutex);
	list_for_each_entry(gfx2d_obj, &priv->wlist, w_node) {
		if (is_active(gfx2d_obj)) {
			enable_exend = true;
			break;
		}
	}
	mutex_unlock(&priv->wlist_mutex);

	if (enable_exend) {
		drm_dbg(&priv->drm,
			"enable EXEND interrupt: still waiting for object %u (%d)\n",
			gfx2d_obj->id, atomic_read(&gfx2d_obj->gpu_active));
	} else {
		enable_exend = mchp_gfx2d_has_pending_commands(priv);
		if (enable_exend)
			drm_dbg(&priv->drm,
				"enable EXEND interrupt: some commands are pending\n");
	}

	if (enable_exend)
		writel(GFX2D_IRQ_EXEND, priv->regs + GFX2D_IE);

put_autosuspend:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

exit:
	drm_dbg(&priv->drm, "leave IRQ thread\n");

	return IRQ_HANDLED;
}

static int mchp_gfx2d_probe(struct platform_device *pdev)
{
	struct mchp_gfx2d_device *priv;
	struct resource *res;
	int irq;
	int ret;

	priv = devm_drm_dev_alloc(&pdev->dev, &mchp_gfx2d_drm_driver,
				  struct mchp_gfx2d_device, drm);
	if (IS_ERR(priv)) {
		dev_err(&pdev->dev, "failed to allocate the DRM device\n");
		ret = PTR_ERR(priv);
		goto err_exit;
	}
	platform_set_drvdata(pdev, priv);

	priv->caps = of_device_get_match_data(&pdev->dev);
	if (!priv->caps) {
		dev_err(&pdev->dev, "could not retrieve GFX2D caps\n");
		ret = -EINVAL;
		goto err_exit;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->regs)) {
		dev_err(&pdev->dev, "missing registers\n");
		ret = PTR_ERR(priv->regs);
		goto err_exit;
	}

	/* Request the IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get the IRQ\n");
		ret = irq;
		goto err_exit;
	}

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					mchp_gfx2d_interrupt,
					mchp_gfx2d_thread,
					0, dev_name(&pdev->dev), priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to request the IRQ\n");
		goto err_exit;
	}

	priv->pclk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->pclk)) {
		dev_err(&pdev->dev, "missing peripheral clock\n");
		ret = PTR_ERR(priv->pclk);
		goto err_exit;
	}

	/* Enable the peripheral clock */
	ret = clk_prepare_enable(priv->pclk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable the peripheral clock\n");
		goto err_exit;
	}

	ret = mchp_gfx2d_init_command_queue(priv);
	if (ret)
		goto err_disable_unprepare_clock;

	pm_runtime_set_autosuspend_delay(&pdev->dev, GFX2D_PM_RUNTIME_DELAY);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	ret = drm_dev_register(&priv->drm, 0);
	if (ret) {
		pm_runtime_put_noidle(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
		pm_runtime_set_suspended(&pdev->dev);
		pm_runtime_dont_use_autosuspend(&pdev->dev);
		dev_err(&pdev->dev, "failed to register the DRM device\n");
		goto err_cleanup_cmd_queue;
	}
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);

	return 0;

err_cleanup_cmd_queue:
	mchp_gfx2d_cleanup_command_queue(priv);
err_disable_unprepare_clock:
	clk_disable_unprepare(priv->pclk);
err_exit:
	return ret;
}

static void mchp_gfx2d_remove(struct platform_device *pdev)
{
	struct mchp_gfx2d_device *priv = platform_get_drvdata(pdev);
	int ret;

	drm_dev_unregister(&priv->drm);

	ret = pm_runtime_resume_and_get(&pdev->dev);
	mchp_gfx2d_cleanup_command_queue(priv);
	if (ret) {
		dev_warn(&pdev->dev, "Failed to resume device on remove\n");
	} else {
		pm_runtime_put_noidle(&pdev->dev);
		clk_disable(priv->pclk);
	}

	pm_runtime_disable(&pdev->dev);

	clk_unprepare(priv->pclk);
}

static int __maybe_unused mchp_gfx2d_runtime_suspend(struct device *dev)
{
	struct mchp_gfx2d_device *priv = dev_get_drvdata(dev);
	unsigned long to;

	mutex_lock(&priv->cmdlist_mutex);
	priv->is_suspended = true;
	reinit_completion(&priv->running_cmdlist_empty);
	mutex_unlock(&priv->cmdlist_mutex);

	(void)mchp_gfx2d_progress(priv);
	to = msecs_to_jiffies(GFX2D_PM_RUNTIME_SUSPEND_DELAY);
	if (!wait_for_completion_timeout(&priv->running_cmdlist_empty, to)) {
		mutex_lock(&priv->cmdlist_mutex);
		priv->is_suspended = false;
		pm_runtime_mark_last_busy(dev);
		mutex_unlock(&priv->cmdlist_mutex);
		writel(GFX2D_IRQ_EXEND, priv->regs + GFX2D_IE);
		return -EBUSY;
	}

	clk_disable(priv->pclk);

	return 0;
}

static int __maybe_unused mchp_gfx2d_runtime_resume(struct device *dev)
{
	struct mchp_gfx2d_device *priv = dev_get_drvdata(dev);
	int ret;

	ret = clk_enable(priv->pclk);
	if (ret)
		return ret;

	mutex_lock(&priv->cmdlist_mutex);
	priv->is_suspended = false;
	pm_runtime_mark_last_busy(dev);
	mutex_unlock(&priv->cmdlist_mutex);

	(void)mchp_gfx2d_progress(priv);

	return 0;
}

static const struct dev_pm_ops __maybe_unused mchp_gfx2d_pm_ops = {
	SET_RUNTIME_PM_OPS(mchp_gfx2d_runtime_suspend, mchp_gfx2d_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static const struct mchp_gfx2d_caps mchp_sam9x60_gfx2d_caps = {
	.has_dreg = false,
};

static const struct mchp_gfx2d_caps mchp_sam9x75_gfx2d_caps = {
	.has_dreg = true,
};

static const struct of_device_id mchp_gfx2d_of_match[] = {
	{
		.compatible = "microchip,sam9x60-gfx2d",
		.data = &mchp_sam9x60_gfx2d_caps,
	},
	{
		.compatible = "microchip,sam9x7-gfx2d",
		.data = &mchp_sam9x75_gfx2d_caps,
	},
	{ /* sentinel */ },
};

static struct platform_driver mchp_gfx2d_platform_driver = {
	.driver = {
		.name = "mchp-gfx2d",
		.of_match_table = mchp_gfx2d_of_match,
		.pm = pm_ptr(&mchp_gfx2d_pm_ops),
	},
	.probe = mchp_gfx2d_probe,
	.remove = mchp_gfx2d_remove,
};
drm_module_platform_driver(mchp_gfx2d_platform_driver);

MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@microchip.com>");
MODULE_DESCRIPTION("Microchip GFX2D Driver");
MODULE_LICENSE("GPL");
