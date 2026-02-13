// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC DMA proxy driver
 *
 * Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Shravan Chippa <shravan.chippa@microchip.com>
 *
 * Driver based on Xilinx DMA proxy driver
 */

#include <linux/cdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <uapi/misc/mpfs-dma-proxy.h>

#define MPFS_DMA_PROXY_DRIVER_NAME		"mpfs_dma_proxy"
#define MPFS_DMA_PROXY_XFER_TIMEOUT_M_SEC	3000
#define MPFS_DMA_PROXY_MAXBURST_SIZE		8

/**
 * struct mpfs_dma_proxy_channel -  one channel resource
 * @channel_config:	dma config data pointer
 * @proxy_device:	device struct proxy dma
 * @dma_device:		device struct dma
 * @class:		struct class proxy dma
 * @channel:		dma chan pointer
 * @cdev:		struct cdev for char device
 * @proxy_status:	status of dma xfer
 * @complete:		struct completion
 * @cookie:		transaction cookie
 * @dev:		device node
 */
struct mpfs_dma_proxy_channel {
	struct mpfs_dma_proxy_channel_config *channel_config;
	struct device *proxy_device;
	struct device *dma_device;
	struct class *class;
	struct dma_chan *channel;
	struct cdev cdev;
	enum mpfs_dma_proxy_status proxy_status;
	struct completion complete;
	dma_cookie_t cookie;
	dev_t dev;
};

/**
 * struct mpfs_dma_proxy - dma proxy platform data for all channels
 * @channels:		pointer for all channels
 * @names:		array of channel names
 * @channel_count:	channel count
 * @active_channels:	active channel counter for cleanup class
 */
struct mpfs_dma_proxy {
	struct mpfs_dma_proxy_channel *channels;
	char **names;
	int channel_count;
	int active_channels;
};

static void mpfs_dma_proxy_sync_callback(void *done)
{
	complete(done);
}

static int mpfs_dma_proxy_start_transfer(struct mpfs_dma_proxy_channel *channel)
{
	struct mpfs_dma_proxy_channel_config *channel_config = channel->channel_config;
	struct dma_async_tx_descriptor *desc;
	struct dma_slave_config config;
	int ret;

	memset(&config, 0, sizeof(config));

	config.src_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	config.dst_maxburst = MPFS_DMA_PROXY_MAXBURST_SIZE;

	ret = dmaengine_slave_config(channel->channel, &config);
	if (ret < 0) {
		dev_err(channel->dma_device, "DMA channel config failed (%d)\n", ret);
		return ret;
	}

	desc = dmaengine_prep_dma_memcpy(channel->channel, channel_config->dst,
					 channel_config->src, channel_config->length,
					 DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err(channel->dma_device, "DMA dmaengine_prep_slave_single failed\n");
		return -EINVAL;
	}

	desc->callback = mpfs_dma_proxy_sync_callback;
	desc->callback_param = &channel->complete;
	init_completion(&channel->complete);

	channel->cookie = dmaengine_submit(desc);
	if (dma_submit_error(channel->cookie)) {
		dev_err(channel->dma_device, "DMA submission failed\n");
		return -ENXIO;
	}

	dma_async_issue_pending(channel->channel);

	return 0;
}

static void mpfs_dma_proxy_wait_for_xfer(struct mpfs_dma_proxy_channel *channel)
{
	unsigned long timeout = msecs_to_jiffies(MPFS_DMA_PROXY_XFER_TIMEOUT_M_SEC);
	enum dma_status status;

	timeout = wait_for_completion_timeout(&channel->complete, timeout);
	status = dma_async_is_tx_complete(channel->channel, channel->cookie, NULL, NULL);

	if (timeout == 0)  {
		dev_err(channel->dma_device, "DMA timed out\n");
		channel->proxy_status = PROXY_TIMEOUT;
	} else if (status != DMA_COMPLETE) {
		channel->proxy_status = PROXY_ERROR;
		dev_err(channel->dma_device,
			"DMA returned completion callback status of: %s\n",
			status == DMA_ERROR ? "error" : "in progress");
	} else {
		channel->proxy_status = PROXY_SUCCESS;
	}
}

static int mpfs_dma_proxy_local_open(struct inode *ino, struct file *file)
{
	file->private_data = container_of(ino->i_cdev,
					  struct mpfs_dma_proxy_channel, cdev);

	return 0;
}

static int mpfs_dma_proxy_release(struct inode *ino, struct file *file)
{
	return 0;
}

static long mpfs_dma_proxy_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct mpfs_dma_proxy_channel *channel =
			(struct mpfs_dma_proxy_channel *)file->private_data;

	void __user *uarg = (void __user *)arg;
	int ret;

	switch (cmd) {
	case MPFS_DMA_PROXY_START_XFER:
		if (copy_from_user(channel->channel_config, uarg,
				   sizeof(struct mpfs_dma_proxy_channel_config)))
			return -EFAULT;

		ret = mpfs_dma_proxy_start_transfer(channel);
		if (ret)
			return ret;

		break;

	case MPFS_DMA_PROXY_FINISH_XFER:
		mpfs_dma_proxy_wait_for_xfer(channel);
		if (copy_to_user(uarg, &channel->proxy_status,
				 sizeof(enum mpfs_dma_proxy_status)))
			return -EFAULT;

		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct file_operations dm_fops = {
	.open		= mpfs_dma_proxy_local_open,
	.release	= mpfs_dma_proxy_release,
	.unlocked_ioctl = mpfs_dma_proxy_ioctl,
};

static int mpfs_dma_proxy_cdevice_init(struct mpfs_dma_proxy_channel *channel,
				       char *name)
{
	static struct class *local_class;
	int ret;

	ret = alloc_chrdev_region(&channel->dev, 0, 1, MPFS_DMA_PROXY_DRIVER_NAME);

	if (ret)
		return dev_err_probe(channel->dma_device, ret,
				     "unable to get a char device number\n");

	cdev_init(&channel->cdev, &dm_fops);
	channel->cdev.owner = THIS_MODULE;
	ret = cdev_add(&channel->cdev, channel->dev, 1);

	if (ret) {
		dev_err_probe(channel->dma_device, ret, "unable to add char device\n");
		goto error_cdev_add;
	}

	if (!local_class) {
		local_class = class_create(MPFS_DMA_PROXY_DRIVER_NAME);

		if (IS_ERR(channel->dma_device->class)) {
			ret = dev_err_probe(channel->dma_device,
					    PTR_ERR(channel->dma_device->class),
					    "unable to create class\n");
			goto error_class_create;
		}
	}

	channel->class = local_class;

	channel->proxy_device = device_create(channel->class, NULL,
					      channel->dev, NULL, name);

	if (IS_ERR(channel->proxy_device)) {
		ret = dev_err_probe(channel->dma_device, PTR_ERR(channel->proxy_device),
				    "unable to create the device\n");
		goto error_device_create;
	}

	return 0;

error_device_create:
error_class_create:
	cdev_del(&channel->cdev);

error_cdev_add:
	unregister_chrdev_region(channel->dev, 1);

	return ret;
}

static void mpfs_dma_proxy_cdevice_exit(struct mpfs_dma_proxy *dma_proxy, int channel_no)
{
	struct mpfs_dma_proxy_channel *channel = &dma_proxy->channels[channel_no];

	if (channel->proxy_device) {
		device_destroy(channel->class, channel->dev);

		/* If the active channels count is one we can destroy the class */
		if (dma_proxy->active_channels == 1)
			class_destroy(channel->class);

		cdev_del(&channel->cdev);
		unregister_chrdev_region(channel->dev, 1);
	}
}

static int mpfs_dma_proxy_create_channel(struct platform_device *pdev,
					 struct mpfs_dma_proxy_channel *channel,
					 char *name, u32 direction)
{
	int ret;

	channel->dma_device = &pdev->dev;
	channel->channel = dma_request_chan(&pdev->dev, name);
	if (IS_ERR(channel->channel)) {
		return dev_err_probe(channel->dma_device, PTR_ERR(channel->channel),
				     "Failed to request DMA channel\n");
	}

	channel->channel_config =
		devm_kmalloc(&pdev->dev, sizeof(struct mpfs_dma_proxy_channel_config), GFP_KERNEL);
	if (IS_ERR(channel->channel_config)) {
		ret = dev_err_probe(channel->dma_device, PTR_ERR(channel->channel_config),
				    "Could not allocate channel config\n");
		goto free_channel;
	}

	ret = mpfs_dma_proxy_cdevice_init(channel, name);
	if (ret)
		goto free_channel;

	return 0;

free_channel:
	dma_release_channel(channel->channel);

	return ret;
}

static void mpfs_dma_proxy_cleanup_channels(struct mpfs_dma_proxy *dma_proxy)
{
	struct mpfs_dma_proxy_channel *channels = dma_proxy->channels;
	int i;

	for (i = 0; i < dma_proxy->channel_count; i++) {
		if (channels[i].proxy_device)
			mpfs_dma_proxy_cdevice_exit(dma_proxy, i);

		if (channels[i].channel) {
			channels[i].channel->device->device_terminate_all(channels[i].channel);
			dma_release_channel(channels[i].channel);
		}

		dma_proxy->active_channels--;
	}
}

static int mpfs_dma_proxy_probe(struct platform_device *pdev)
{
	struct mpfs_dma_proxy *dma_proxy;
	struct device *dev = &pdev->dev;
	int ret, i;

	dma_proxy = devm_kzalloc(dev, sizeof(struct mpfs_dma_proxy), GFP_KERNEL);
	if (IS_ERR(dma_proxy))
		return dev_err_probe(dev, PTR_ERR(dma_proxy), "Could not allocate proxy device\n");

	dev_set_drvdata(dev, dma_proxy);

	dma_proxy->channel_count =
		device_property_read_string_array(dev, "dma-names", NULL, 0);

	if (dma_proxy->channel_count <= 0)
		return dev_err_probe(dev, dma_proxy->channel_count,
				     "Could not get DMA channel count\n");

	dma_proxy->names = devm_kmalloc_array(&pdev->dev, dma_proxy->channel_count,
					      sizeof(char *), GFP_KERNEL);
	if (IS_ERR(dma_proxy->names))
		return dev_err_probe(dev, PTR_ERR(dma_proxy->names),
				     "Could not allocate name array\n");

	ret = device_property_read_string_array(&pdev->dev, "dma-names",
						(const char **)dma_proxy->names,
						dma_proxy->channel_count);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Could not get the dma-names");

	dma_proxy->channels =
		devm_kmalloc(dev,
			     sizeof(struct mpfs_dma_proxy_channel) * dma_proxy->channel_count,
			     GFP_KERNEL);
	if (IS_ERR(dma_proxy->channels))
		return dev_err_probe(dev, PTR_ERR(dma_proxy->channels),
				     "Could not allocate channels\n");

	for (i = 0; i < dma_proxy->channel_count; i++) {
		ret = mpfs_dma_proxy_create_channel(pdev, &dma_proxy->channels[i],
						    dma_proxy->names[i], DMA_MEM_TO_MEM);

		if (ret) {
			dev_err_probe(dev, ret, "Fail to create channel\n");
			dma_proxy->channel_count = dma_proxy->active_channels;
			goto cleanup_channels;
		}

		dma_proxy->active_channels++;
	}

	dev_info(dev, "proxy dma %d channels initialized\n", dma_proxy->channel_count);

	return 0;

cleanup_channels:
	mpfs_dma_proxy_cleanup_channels(dma_proxy);

	return ret;
}

static void mpfs_dma_proxy_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpfs_dma_proxy *dma_proxy = dev_get_drvdata(dev);

	mpfs_dma_proxy_cleanup_channels(dma_proxy);

}

static const struct of_device_id mpfs_dma_proxy_of_ids[] = {
	{ .compatible = "microchip,mpfs-dma-proxy",},
	{}
};

static struct platform_driver mpfs_dma_proxy_driver = {
	.driver = {
		.name = MPFS_DMA_PROXY_DRIVER_NAME,
		.of_match_table = mpfs_dma_proxy_of_ids,
	},
	.probe = mpfs_dma_proxy_probe,
	.remove = mpfs_dma_proxy_remove,
};

static int __init mpfs_dma_proxy_init(void)
{
	return platform_driver_register(&mpfs_dma_proxy_driver);
}

static void __exit mpfs_dma_proxy_exit(void)
{
	platform_driver_unregister(&mpfs_dma_proxy_driver);
}

module_init(mpfs_dma_proxy_init);
module_exit(mpfs_dma_proxy_exit);

MODULE_AUTHOR("Shravan.chippa@microchip.com");
MODULE_DESCRIPTION("Microchip PolarFire SoC proxy DMA driver");
MODULE_LICENSE("GPL");
