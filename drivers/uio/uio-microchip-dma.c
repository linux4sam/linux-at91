// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip userspace FPGA fabric DMA driver
 *
 * Copyright (C) 2021 Microchip Incorporated - http://www.microchip.com/
 *
 * Author: Vattipalli Praveen <praveen.kumar@microchip.com>
 *
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>

#define DRV_NAME	"mss-dma-uio"
#define DRV_VERSION	"0.1"

#define MAX_MSS_DMA_EVT (0x01)
#define DMA_INT_OCCURED	(0x01)
#define DMA_INT_STA	(0x10)
#define DMA_INT_CLR	(0x18)

struct uio_mss_dma_dev {
	struct uio_info *info;
	struct clk *mss_dma_clk;
	void __iomem *base;
	int irq;
};

static irqreturn_t mss_dma_handler(int irq, struct uio_info *info)
{
	struct uio_mss_dma_dev *dev_info = info->priv;
	void __iomem *base = dev_info->base;

	if ((ioread32(base + DMA_INT_STA) & DMA_INT_OCCURED)) {
		iowrite32(DMA_INT_OCCURED, base + DMA_INT_CLR);

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void mss_dma_cleanup(struct device *dev, struct uio_mss_dma_dev *uio_dma)
{
	struct uio_info *uio_dma_info = uio_dma->info;
	int cnt;

	for (cnt = 0; cnt < MAX_MSS_DMA_EVT; cnt++, uio_dma_info++) {
		uio_unregister_device(uio_dma_info);
		kfree(uio_dma_info->name);
	}

	iounmap(uio_dma->base);
	kfree(uio_dma->info);
	kfree(uio_dma);
}

static int mss_dma_probe(struct platform_device *pdev)
{
	struct uio_info *uio_dma_info;
	struct uio_mss_dma_dev *uio_dma;
	struct resource *regs;
	struct device *dev = &pdev->dev;
	int ret = -ENODEV, len;

	uio_dma = kzalloc(sizeof(*uio_dma), GFP_KERNEL);
	if (!uio_dma)
		return -ENOMEM;

	uio_dma->info = kzalloc(sizeof(*uio_dma_info) * MAX_MSS_DMA_EVT, GFP_KERNEL);
	if (!uio_dma->info) {
		kfree(uio_dma);
		return -ENOMEM;
	}

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs) {
		dev_err(dev, "No MSS DMA I/O resource specified\n");
		goto out_free;
	}

	len = resource_size(regs);
	uio_dma->base = ioremap(regs->start, len);
	if (!uio_dma->base) {
		dev_err(dev, "Can't remap MSS DMA I/O address range\n");
		goto out_free;
	}

	uio_dma->irq = platform_get_irq(pdev, 0);

	uio_dma_info = uio_dma->info;

	uio_dma_info->mem[0].addr = regs->start;
	uio_dma_info->mem[0].size = resource_size(regs);
	uio_dma_info->mem[0].memtype = UIO_MEM_PHYS;

	uio_dma_info->mem[1].size = 0;

	uio_dma_info->name = dev->of_node->full_name;
	uio_dma_info->version = DRV_VERSION;

	uio_dma_info->irq = uio_dma->irq;
	uio_dma_info->irq_flags = IRQF_SHARED;
	uio_dma_info->handler = mss_dma_handler;
	uio_dma_info->priv = uio_dma;

	ret = uio_register_device(dev, uio_dma_info);
	if (ret < 0) {
		dev_err(dev, "failed to register device\n");
		goto out_free;
	}

	platform_set_drvdata(pdev, uio_dma);

	dev_info(dev, "registered device as %s\n", uio_dma_info->name);

	return 0;

out_free:
	mss_dma_cleanup(dev, uio_dma);

	return ret;
}

static void mss_dma_remove(struct platform_device *dev)
{
	struct uio_mss_dma_dev *dev_info = platform_get_drvdata(dev);

	mss_dma_cleanup(&dev->dev, dev_info);
}

#define MICROCHIP_DMA_PM_OPS (NULL)

#if defined(CONFIG_OF)
static const struct of_device_id mss_dma_dt_ids[] = {
	{ .compatible = "microchip,mpfs-fpga-dma" },
	{ /*sentinel */ }
};
#endif

static struct platform_driver mss_dma_driver = {
	.probe = mss_dma_probe,
	.remove = mss_dma_remove,
	.driver = {
		.name = DRV_NAME,
		.pm = MICROCHIP_DMA_PM_OPS,
		.of_match_table = of_match_ptr(mss_dma_dt_ids),
		.owner = THIS_MODULE,
		   },
};

module_platform_driver(mss_dma_driver);

MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_AUTHOR("Vattipalli Praveen <praveen.kumar@microchip.com>");
