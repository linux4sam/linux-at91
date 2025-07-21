// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) User Crypto driver
 *
 * Copyright (c) 2023 Microchip Corporation. All rights reserved.
 *
 * Author: Padmarao Begari <padmarao.begari@microchip.com>
 *
 */

#include <crypto/engine.h>
#include <linux/crypto.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <asm/sbi.h>
#include "mchp-crypto-sbi.h"

#define MPFS_CRYPTO_DMA_BIT_MASK		32

static DEFINE_SPINLOCK(cryto_service_lock);

static dma_addr_t dma_addr_crypto;
static struct mchp_crypto_info *crypto_info;

struct mchp_dev_list {
	struct list_head	dev_list;
	spinlock_t		lock; /* protect dev_list */
};

static struct mchp_dev_list dev_list = {
	.dev_list = LIST_HEAD_INIT(dev_list.dev_list),
	.lock     = __SPIN_LOCK_UNLOCKED(dev_list.lock),
};

struct mchp_crypto_dev *mchp_crypto_find_dev(struct mchp_crypto_ctx *ctx)
{
	struct mchp_crypto_dev *cryp = NULL, *tmp;

	spin_lock(&dev_list.lock);
	if (!ctx->cryp) {
		list_for_each_entry(tmp, &dev_list.dev_list, list) {
			cryp = tmp;
			break;
		}
		ctx->cryp = cryp;
	} else {
		cryp = ctx->cryp;
	}

	spin_unlock(&dev_list.lock);

	return cryp;
}

int mchp_crypto_sbi_services(u32 service, u64 crypto_addr, u32 flags)
{
	struct sbiret ret;

	spin_lock(&cryto_service_lock);
	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY,
			MICROCHIP_SBI_EXT_CRYPTO_SERVICES,
			service, crypto_addr, flags, 0, 0, 0);
	spin_unlock(&cryto_service_lock);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return ret.value;
}

static int mchp_crypto_sbi_sevices_probe(u64 crypto_addr)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY,
			MICROCHIP_SBI_EXT_CRYPTO_SERVICES_PROBE,
			crypto_addr, 0, 0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return ret.value;
}

static int mchp_crypto_sbi_init(bool enable)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY,
			MICROCHIP_SBI_EXT_CRYPTO_INIT,
			enable, 0, 0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return ret.value;
}

static inline int mchp_crypto_sbi_startup(void)
{
	return mchp_crypto_sbi_init(true);
}

static inline int mchp_crypto_sbi_shutdown(void)
{
	return mchp_crypto_sbi_init(false);
}

static int mchp_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mchp_crypto_dev *cryp;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(MPFS_CRYPTO_DMA_BIT_MASK));
	if (ret < 0)
		return dev_err_probe(dev, ret, "No usable DMA configuration\n");

	cryp = devm_kzalloc(dev, sizeof(*cryp), GFP_KERNEL);
	if (!cryp)
		return -ENOMEM;

	platform_set_drvdata(pdev, cryp);

	cryp->dev = dev;

	ret = sbi_probe_extension(SBI_EXT_MICROCHIP_TECHNOLOGY);
	if (!ret)
		return dev_err_probe(dev, ret, "SBI extension not detected\n");

	crypto_info = dma_alloc_coherent(dev, sizeof(struct mchp_crypto_info),
					 &dma_addr_crypto, GFP_KERNEL);
	if (!crypto_info)
		return -ENOMEM;

	/* Initializes the crypto processor and enable clock. */
	ret = mchp_crypto_sbi_startup();
	if (ret < 0)
		goto err_free_info;

	ret = mchp_crypto_sbi_sevices_probe(dma_addr_crypto);
	if (ret < 0)
		goto err_crypto_shutdown;

	cryp->crypto = crypto_info;

	spin_lock(&dev_list.lock);
	list_add(&cryp->list, &dev_list.dev_list);
	spin_unlock(&dev_list.lock);

	cryp->engine = crypto_engine_alloc_init(dev, 1);
	if (!cryp->engine) {
		ret = -ENOMEM;
		goto err_list_del;
	}

	ret = crypto_engine_start(cryp->engine);
	if (ret)
		goto err_engine_exit;

	if (cryp->crypto->services & CRYPTO_SERVICE_AES) {
		ret = mchp_aes_register_algs(cryp);
		if (ret)
			goto err_engine_stop;
	}

	return 0;

err_engine_stop:
	crypto_engine_stop(cryp->engine);
err_engine_exit:
	crypto_engine_exit(cryp->engine);
err_list_del:
	spin_lock(&dev_list.lock);
	list_del(&cryp->list);
	spin_unlock(&dev_list.lock);
err_crypto_shutdown:
	mchp_crypto_sbi_shutdown();
err_free_info:
	dma_free_coherent(cryp->dev, sizeof(struct mchp_crypto_info),
			  crypto_info, dma_addr_crypto);
	return ret;
}

static void mchp_crypto_remove(struct platform_device *pdev)
{
	struct mchp_crypto_dev *cryp = platform_get_drvdata(pdev);
	/* Disable crypto clock */
	mchp_crypto_sbi_shutdown();

	if (cryp->crypto->services & CRYPTO_SERVICE_AES)
		mchp_aes_unregister_algs(cryp);

	crypto_engine_stop(cryp->engine);
	crypto_engine_exit(cryp->engine);

	spin_lock(&dev_list.lock);
	list_del(&cryp->list);
	spin_unlock(&dev_list.lock);

	dma_free_coherent(cryp->dev, sizeof(struct mchp_crypto_info),
			  crypto_info, dma_addr_crypto);
}

static const struct of_device_id mchp_crypto_dt_ids[] = {
	{ .compatible = "microchip,mpfs-crypto", },
	{}
};
MODULE_DEVICE_TABLE(of, mchp_crypto_dt_ids);

static struct platform_driver mchp_crypto_driver = {
	.driver = {
		.name = "microchip-crypto",
		.of_match_table = mchp_crypto_dt_ids,
	},
	.probe = mchp_crypto_probe,
	.remove = mchp_crypto_remove,
};
module_platform_driver(mchp_crypto_driver);

MODULE_AUTHOR("Padmarao Begari <padmarao.begari@microchip.com>");
MODULE_DESCRIPTION("Microchip PolarFire SoC User Crypto driver");
MODULE_LICENSE("GPL");
