// SPDX-License-Identifier: GPL-2.0
/*
 * Mi-V Inter-hart communication (IHC) driver
 *
 * Copyright (c) 2021 - 2022 Microchip Technology Inc. All rights reserved.
 *
 * Author: Valentina Fernandez <valentina.fernandezalanis@microchip.com>
 *
 */

#include <linux/io.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/mailbox/miv_ihc.h>
#include <asm/sbi.h>

#define SBI_EXT_VENDOR_START			0x09000000
#define MICROCHIP_TECHNOLOGY_MVENDOR_ID		0x029

#define SBI_EXT_MICROCHIP_TECHNOLOGY	(SBI_EXT_VENDOR_START | \
					MICROCHIP_TECHNOLOGY_MVENDOR_ID)

enum {
	IHC_MP_IRQ,
	IHC_ACK_IRQ,
};

struct ihc_sbi_msg {
	struct miv_ihc_msg ihc_msg;
	u8 irq_type;
};

static int ihc_sbi_send(u32 command, u32 remote_context_id, dma_addr_t address)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY, command, remote_context_id,
			address, 0, 0, 0, 0);

	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return ret.value;
}

static int ihc_sbi_init(u32 remote_context_id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY, SBI_EXT_IHC_INIT, remote_context_id,
			0, 0, 0, 0, 0);

	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return ret.value;
}

static struct miv_ihc *mbox_chan_to_ihc(struct mbox_chan *chan)
{
	if (!chan || !chan->con_priv)
		return NULL;

	return (struct miv_ihc *)chan->con_priv;
}

static irqreturn_t ihc_isr(int irq, void *data)
{
	struct ihc_sbi_msg sbi_rx_msg;
	struct mbox_chan *chan = (struct mbox_chan *)data;
	struct miv_ihc *ihc = mbox_chan_to_ihc(chan);
	int ret;

	ret = ihc_sbi_send(SBI_EXT_IHC_RX, ihc->remote_context_id, ihc->dma_addr);

	if (unlikely(ret < 0)) {
		dev_warn_ratelimited(ihc->dev, "incorrect remote context ID\n");
		return IRQ_NONE;
	}

	memcpy(&sbi_rx_msg, ihc->buf_base, sizeof(sbi_rx_msg));

	if (sbi_rx_msg.irq_type == IHC_MP_IRQ)
		mbox_chan_received_data(&ihc->channel, &sbi_rx_msg.ihc_msg);
	else
		mbox_chan_txdone(&ihc->channel, 0);

	return IRQ_HANDLED;
}

static int ihc_send_data(struct mbox_chan *chan, void *data)
{
	struct miv_ihc *ihc = mbox_chan_to_ihc(chan);
	struct miv_ihc_msg *message = data;
	int ret;

	memcpy(ihc->buf_base, message, sizeof(*message));

	ret = ihc_sbi_send(SBI_EXT_IHC_TX, ihc->remote_context_id, ihc->dma_addr);

	return ret;
}

static int ihc_startup(struct mbox_chan *chan)
{
	struct miv_ihc *ihc = mbox_chan_to_ihc(chan);
	int ret;

	ret = devm_request_irq(ihc->dev, ihc->irq, ihc_isr, IRQF_SHARED,
			       "miv-ihc-irq", &ihc->channel);

	if (ret)
		dev_err(ihc->dev, "failed to register interrupt:%d\n", ret);

	return ret;
}

static void ihc_shutdown(struct mbox_chan *chan)
{
	struct miv_ihc *ihc = mbox_chan_to_ihc(chan);

	devm_free_irq(ihc->dev, ihc->irq, chan);
}

static const struct mbox_chan_ops miv_ihc_ops = {
	.startup = ihc_startup,
	.send_data = ihc_send_data,
	.shutdown = ihc_shutdown,
};

static int ihc_probe(struct platform_device *pdev)
{
	struct miv_ihc *ihc;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	ret = sbi_probe_extension(SBI_EXT_MICROCHIP_TECHNOLOGY);
	if (ret <= 0)
		return dev_err_probe(dev, ret, "SBI IHC extension not detected\n");

	ihc = devm_kzalloc(dev, sizeof(*ihc), GFP_KERNEL);
	if (!ihc)
		return -ENOMEM;
	platform_set_drvdata(pdev, ihc);

	ret = of_property_read_u32(np, "microchip,miv-ihc-remote-context-id",
				   &ihc->remote_context_id);
	if (ret) {
		return dev_err_probe(dev, ret,
				     "missing microchip,miv-ihc-remote-context-id property\n");
	}

	ret = ihc_sbi_init(ihc->remote_context_id);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Context init failed\n");

	ihc->channel.con_priv = ihc;
	ihc->dev = dev;

	ihc->controller.txdone_irq = true;
	ihc->controller.dev = ihc->dev;
	ihc->controller.ops = &miv_ihc_ops;
	ihc->controller.chans = &ihc->channel;
	ihc->controller.num_chans = 1;

	ihc->irq = platform_get_irq(pdev, 0);
	if (ihc->irq < 0)
		return dev_err_probe(dev, ihc->irq, "unable to get IRQ\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(39));
	if (ret)
		return dev_err_probe(dev, ret, "dma_set_mask_and_coherent failed\n");

	ihc->pool = dma_pool_create("ihc", dev, IHC_MAX_MESSAGE_SIZE, 0, 0);
	if (!ihc->pool)
		return dev_err_probe(dev, PTR_ERR(ihc->pool), "failed to create dma pool\n");

	ihc->buf_base = dma_pool_alloc(ihc->pool, GFP_KERNEL, &ihc->dma_addr);
	if (!ihc->buf_base) {
		dma_pool_destroy(ihc->pool);
		return dev_err_probe(dev, PTR_ERR(ihc->buf_base),
				     "failed to allocate memory for dma pool\n");
	}

	ret = devm_mbox_controller_register(dev, &ihc->controller);
	if (ret) {
		dma_pool_free(ihc->pool, ihc->buf_base, ihc->dma_addr);
		dma_pool_destroy(ihc->pool);
		return dev_err_probe(dev, ret,
				     "Mi-V inter-hart communication (IHC) registration failed\n");
	}

	dev_info(&pdev->dev, "Mi-V inter-hart communication (IHC) registered\n");
	return 0;
}

static void ihc_remove(struct platform_device *pdev)
{
	struct miv_ihc *ihc = platform_get_drvdata(pdev);

	dma_pool_free(ihc->pool, ihc->buf_base, ihc->dma_addr);
	dma_pool_destroy(ihc->pool);
}

static const struct of_device_id ihc_of_match[] = {
	{ .compatible = "microchip,miv-ihc", },
	{}
};
MODULE_DEVICE_TABLE(of, ihc_of_match);

static struct platform_driver miv_ihc_driver = {
	.driver = {
		.name = "miv_ihc",
		.of_match_table = ihc_of_match
	},
	.probe = ihc_probe,
	.remove = ihc_remove,
};

module_platform_driver(miv_ihc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Valentina Fernandez <valentina.fernandezalanis@microchip.com>");
MODULE_DESCRIPTION("Mi-V Inter-Hart Communication (IHC) driver");
