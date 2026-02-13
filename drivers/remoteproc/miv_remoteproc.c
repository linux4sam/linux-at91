// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microchip Mi-V Remote Proc driver
 *
 * Copyright (c) 2021 - 2022 Microchip Technology Inc. All rights reserved.
 *
 * Author: Valentina Fernandez <valentina.fernandezalanis@microchip.com>
 *
 * Derived from the imx_rproc implementation:
 * Copyright (c) 2017 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/workqueue.h>

#include <asm/sbi.h>
#include <linux/mailbox/miv_ihc.h>
#include <soc/microchip/mpfs.h>

#include "remoteproc_internal.h"

#define SBI_EXT_VENDOR_START			0x09000000
#define MICROCHIP_TECHNOLOGY_MVENDOR_ID		0x029

#define SBI_EXT_MICROCHIP_TECHNOLOGY		(SBI_EXT_VENDOR_START | \
						MICROCHIP_TECHNOLOGY_MVENDOR_ID)

#define MIV_RPROC_MEM_MAX			2

/**
 * enum miv_remote_state - remote context state
 *
 * @CONTEXT_OFFLINE: The remote context is offline, so loading of firmware
 * and booting should be done by remoteproc.
 *
 * @CONTEXT_RUNNNING: The remote context has been booted by the bootloader
 */
enum miv_remote_state {
	CONTEXT_OFFLINE,
	CONTEXT_RUNNNING,
};

/**
 * enum miv_rp_mbox_messages - predefined mailbox messages
 *
 * @MIV_RP_MBOX_READY: a ready message response from a remote context indicating
 * that the remote context is up and running.
 *
 * @MIV_RP_MBOX_PENDING_MSG: Not currently in use, but reserved for future use
 * to inform the receiver that there is a message awaiting in its receive-side
 * vring. At the moment, one can explicitly send the index of the triggered
 * virtqueue as a payload.
 *
 * @MIV_RP_MBOX_STOP: a stop request for the remote context
 *
 * @MIV_RP_MBOX_END_MSG: Indicates end of known/defined messages.
 * This should be the last definition.
 *
 */
enum miv_rp_mbox_messages {
	MIV_RP_MBOX_READY = 0xFFFFFF00,
	MIV_RP_MBOX_PENDING_MSG = 0xFFFFFF01,
	MIV_RP_MBOX_STOP = 0xFFFFFF02,
	MIV_RP_MBOX_END_MSG = 0xFFFFFF03,
};

struct miv_rproc_mem {
	void __iomem *cpu_addr;
	phys_addr_t sys_addr;
	size_t size;
};

struct miv_rproc {
	struct device *dev;
	struct rproc *rproc;
	struct mbox_chan *mbox_channel;
	struct workqueue_struct *workqueue;
	struct miv_rproc_mem mem[MIV_RPROC_MEM_MAX];
	struct mbox_client mbox_client;
	struct completion start_done;
	struct work_struct rproc_work;
	struct miv_ihc_msg miv_ihc_message;
	void __iomem *rsc_table;
	bool initialized;
	u32 remote_context_id;
};

static int miv_rproc_get_status(u32 remote_context_id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY, SBI_EXT_RPROC_STATE,
			remote_context_id, 0, 0, 0, 0, 0);

	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return ret.value;
}

static int miv_rproc_start(struct rproc *rproc)
{
	struct miv_rproc *priv = rproc->priv;
	struct sbiret ret;
	int result;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY, SBI_EXT_RPROC_START,
			priv->remote_context_id, rproc->bootaddr, 0, 0, 0, 0);

	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	if (rproc->nb_vdev > 0) {
		result = wait_for_completion_timeout(&priv->start_done,
						     msecs_to_jiffies(5000));
		if (!result)
			dev_err(priv->dev, "timeout waiting for ready notification\n");
	}

	return ret.value;
}

static int miv_rproc_stop(struct rproc *rproc)
{
	struct miv_rproc *priv = rproc->priv;
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_MICROCHIP_TECHNOLOGY, SBI_EXT_RPROC_STOP,
			priv->remote_context_id, MIV_RP_MBOX_STOP, 0, 0, 0, 0);

	priv->initialized = false;

	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return ret.value;
}

static int miv_rproc_mem_alloc(struct rproc *rproc,
			       struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	dev_dbg(dev, "map memory: %p+%zx\n", &mem->dma, mem->len);
	va = ioremap_wc(mem->dma, mem->len);
	if (IS_ERR_OR_NULL(va)) {
		dev_err(dev, "Unable to map memory region: %p+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	mem->va = va;

	return 0;
}

static int miv_rproc_mem_release(struct rproc *rproc,
				 struct rproc_mem_entry *mem)
{
	dev_dbg(rproc->dev.parent, "unmap memory: %pa\n", &mem->dma);
	iounmap(mem->va);

	return 0;
}

static int miv_rproc_prepare(struct rproc *rproc)
{
	struct miv_rproc *priv = rproc->priv;
	struct device_node *np = priv->dev->of_node;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	struct of_phandle_iterator it;
	u64 da;

	reinit_completion(&priv->start_done);

	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		/*
		 * Ignore the first memory region which will be used vdev
		 * buffer. No need to do extra handlings, rproc_add_virtio_dev
		 * will handle it.
		 */
		if (!strcmp(it.node->name, "vdev0buffer"))
			continue;

		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(priv->dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		da = rmem->base;

		mem = rproc_mem_entry_init(priv->dev, NULL, (dma_addr_t)rmem->base,
					   rmem->size, da, miv_rproc_mem_alloc,
					   miv_rproc_mem_release, it.node->name);

		if (!mem)
			return -ENOMEM;

		rproc_coredump_add_segment(rproc, da, rmem->size);
		rproc_add_carveout(rproc, mem);
	}

	return  0;
}

static int miv_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret)
		dev_info(&rproc->dev, "No resource table in elf\n");

	return 0;
}

static void miv_rproc_kick(struct rproc *rproc, int vqid)
{
	struct miv_rproc *priv = (struct miv_rproc *)rproc->priv;
	int ret;

	/*
	 * If already initialized, do not notify each time rx buffers
	 * are consumed, since we are already sending an ACK using the
	 * Inter-Hart Communication (IHC) driver. The RPMsg-lite on the
	 * remote side has the RL_ALLOW_CONSUMED_BUFFERS_NOTIFICATION
	 * macro set to 0.
	 */
	if (vqid == 0) {
		if (!priv->initialized)
			priv->initialized = true;
		else
			return;
	}

	ret = mbox_send_message(priv->mbox_channel, (void *)&vqid);
	if (ret < 0)
		dev_err(priv->dev,
			"failed to send mbox message, status = %d\n", ret);
}

static int miv_rproc_attach(struct rproc *rproc)
{
	return 0;
}

static struct resource_table
*miv_rproc_get_loaded_rsc_table(struct rproc *rproc, size_t *table_sz)
{
	struct miv_rproc *priv = rproc->priv;

	if (!priv->rsc_table)
		return NULL;

	*table_sz = SZ_1K;
	return (struct resource_table *)priv->rsc_table;
}

static const struct rproc_ops miv_rproc_ops = {
	.prepare = miv_rproc_prepare,
	.start = miv_rproc_start,
	.get_loaded_rsc_table = miv_rproc_get_loaded_rsc_table,
	.attach = miv_rproc_attach,
	.stop = miv_rproc_stop,
	.kick = miv_rproc_kick,
	.load = rproc_elf_load_segments,
	.parse_fw = miv_rproc_parse_fw,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check = rproc_elf_sanity_check,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

static int miv_rproc_addr_init(struct miv_rproc *priv,
			       struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int a, b = 0, err, rmem_np;

	/* memory-region is an optional property */
	rmem_np = of_count_phandle_with_args(np, "memory-region", NULL);
	if (rmem_np <= 0)
		return 0;

	/* remap optional addresses */
	for (a = 0; a < rmem_np; a++) {
		struct device_node *node;
		struct resource res;

		node = of_parse_phandle(np, "memory-region", a);

		if (!strncmp(node->name, "vdev", strlen("vdev"))) {
			of_node_put(node);
			continue;
		}

		err = of_address_to_resource(node, 0, &res);
		of_node_put(node);
		if (err)
			return dev_err_probe(dev, err,
					     "unable to resolve memory region\n");

		if (b >= MIV_RPROC_MEM_MAX)
			break;

		priv->mem[b].cpu_addr = devm_ioremap(&pdev->dev,
						     res.start, resource_size(&res));

		if (!priv->mem[b].cpu_addr)
			return dev_err_probe(dev, -ENOMEM,
					     "failed to remap %pr\n", &res);

		priv->mem[b].sys_addr = res.start;
		priv->mem[b].size = resource_size(&res);
		if (!strcmp(node->name, "rsc-table"))
			priv->rsc_table = priv->mem[b].cpu_addr;
		b++;
	}

	return 0;
}

static void miv_rproc_vq_work(struct work_struct *work)
{
	struct miv_rproc *priv = container_of(work, struct miv_rproc, rproc_work);
	struct device *dev = priv->rproc->dev.parent;
	u32 msg = priv->miv_ihc_message.msg[0];

	/*
	 * Currently, we are expected to receive the following messages
	 * from the remote context; a ready message or simply receive the
	 * index of the triggered virtqueue as a payload.
	 * We can silently ignore any other type of mailbox messages since
	 * they are not meant for us and are meant to be received by the
	 * remote context only.
	 */
	switch (msg) {
	case MIV_RP_MBOX_READY:
		complete(&priv->start_done);
		break;
	default:
		if (msg >= MIV_RP_MBOX_READY && msg < MIV_RP_MBOX_END_MSG)
			return;
		if (msg > priv->rproc->max_notifyid) {
			dev_info(dev, "dropping unknown message 0x%x", msg);
			return;
		}
		/* msg contains the index of the triggered vring */
		if (rproc_vq_interrupt(priv->rproc, msg) == IRQ_NONE)
			dev_dbg(dev, "no message was found in vqid %d\n", msg);
	}
}

static void miv_rproc_rx_callback(struct mbox_client *mbox_client, void *msg)
{
	struct rproc *rproc = dev_get_drvdata(mbox_client->dev);
	struct miv_rproc *priv = rproc->priv;

	priv->miv_ihc_message = *(struct miv_ihc_msg *)msg;
	queue_work(priv->workqueue, &priv->rproc_work);
}

static int miv_rproc_mbox_init(struct rproc *rproc)
{
	struct miv_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	struct mbox_client *mbox_client;
	struct miv_ihc *ihc;

	mbox_client = &priv->mbox_client;
	mbox_client->dev = dev;
	mbox_client->tx_block = true;
	mbox_client->tx_tout = 100;
	mbox_client->knows_txdone = false;
	mbox_client->rx_callback = miv_rproc_rx_callback;

	priv->mbox_channel = mbox_request_channel(mbox_client, 0);
	if (IS_ERR(priv->mbox_channel))
		return dev_err_probe(mbox_client->dev,
				     PTR_ERR(priv->mbox_channel),
				     "failed to request mailbox channel\n");

	ihc = (struct miv_ihc *)priv->mbox_channel->con_priv;
	priv->remote_context_id = ihc->remote_context_id;

	return 0;
}

static void miv_rproc_free_mbox(struct rproc *rproc)
{
	struct miv_rproc *priv = rproc->priv;

	mbox_free_channel(priv->mbox_channel);
}

static int miv_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct miv_rproc *priv;
	struct device_node *np = dev->of_node;
	struct rproc *rproc;
	int ret;

	dev_info(dev, "Started Mi-V Remote Processor driver\n");

	rproc = rproc_alloc(dev, "miv-rproc", &miv_rproc_ops,
			    NULL, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	priv = rproc->priv;
	priv->rproc = rproc;
	priv->dev = dev;

	dev_set_drvdata(dev, rproc);

	priv->workqueue = create_workqueue(dev_name(dev));
	if (!priv->workqueue) {
		ret = -ENOMEM;
		dev_err_probe(dev, ret, "cannot create workqueue\n");
		goto err_put_rproc;
	}

	INIT_WORK(&priv->rproc_work, miv_rproc_vq_work);
	init_completion(&priv->start_done);

	ret = miv_rproc_mbox_init(rproc);
	if (ret)
		goto err_put_wkq;

	ret = miv_rproc_addr_init(priv, pdev);
	if (ret)
		goto err_put_mbox;

	ret = miv_rproc_get_status(priv->remote_context_id);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Couldn't get remote context status\n");
		goto err_put_mbox;
	}

	if (ret == CONTEXT_RUNNNING)
		rproc->state = RPROC_DETACHED;

	if (rproc->state != RPROC_DETACHED)
		rproc->auto_boot = of_property_read_bool(np, "microchip,auto-boot");

	ret = rproc_add(rproc);
	if (ret) {
		return dev_err_probe(dev, ret, "rproc_add failed\n");
		goto err_put_mbox;
	}

	return 0;

err_put_mbox:
	miv_rproc_free_mbox(rproc);
err_put_wkq:
	destroy_workqueue(priv->workqueue);
err_put_rproc:
	rproc_free(rproc);

	return ret;
}

static void miv_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct miv_rproc *priv = rproc->priv;

	rproc_del(rproc);
	miv_rproc_free_mbox(rproc);
	destroy_workqueue(priv->workqueue);
	rproc_free(rproc);
}

static const struct of_device_id miv_rproc_of_match[] __maybe_unused = {
	{ .compatible = "microchip,miv-remoteproc", },
	{}
};
MODULE_DEVICE_TABLE(of, miv_rproc_of_match);

static struct platform_driver miv_rproc_driver = {
	.probe = miv_rproc_probe,
	.remove = miv_rproc_remove,
	.driver = {
		.name = "miv-rproc",
		.of_match_table = of_match_ptr(miv_rproc_of_match),
	},
};

module_platform_driver(miv_rproc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Valentina Fernandez <valentina.fernandezalanis@microchip.com>");
MODULE_DESCRIPTION("Microchip Mi-V Remote Processor control driver");
