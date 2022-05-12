// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) generic service driver
 *
 * Copyright (c) 2020-2022 Microchip Corporation. All rights reserved.
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 *
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/of_platform.h>
#include <soc/microchip/mpfs.h>

static DEFINE_MUTEX(mpfs_generic_service_mutex);

#define MPFS_MAX_PAYLOAD	4096U
#define MPFS_MIN_PAYLOAD	9u

struct mpfs_generic_service_priv {
	struct mpfs_sys_controller *sys_controller;
	struct mpfs_mss_response *response;
	struct device *dev;
	u8 *response_msg;
};

struct mpfs_generic_service_priv *generic_service_priv;

static inline void mpfs_generic_service_free_message_structs(void)
{
	devm_kfree(generic_service_priv->dev, generic_service_priv->response->resp_msg);
	devm_kfree(generic_service_priv->dev, generic_service_priv->response);
	generic_service_priv->response = NULL;
}

static ssize_t mpfs_generic_service_write(struct file *filep, const char __user *userbuf,
					  size_t len, loff_t *offset)
{
	struct mpfs_mss_response *response;
	u8 *cmd_data;
	struct mpfs_mss_msg msg;
	int ret;

	if (len < MPFS_MIN_PAYLOAD || len > MPFS_MAX_PAYLOAD)
		return -EINVAL;

	cmd_data = devm_kmalloc(generic_service_priv->dev, len, GFP_KERNEL);
	if (!cmd_data)
		return -ENOMEM;

	ret = copy_from_user(cmd_data, userbuf, len);
	if (ret) {
		devm_kfree(generic_service_priv->dev, cmd_data);
		return -EFAULT;
	}

	if (generic_service_priv->response)
		mpfs_generic_service_free_message_structs();

	response = devm_kmalloc(generic_service_priv->dev, sizeof(*response), GFP_KERNEL);
	if (!response) {
		devm_kfree(generic_service_priv->dev, cmd_data);
		return -ENOMEM;
	}

	response->resp_status = 0U;
	response->resp_size = ALIGN(*(u16 *)(cmd_data + 3), (4U));
	response->resp_msg = devm_kzalloc(generic_service_priv->dev,
					  response->resp_size * sizeof(u8), GFP_KERNEL);
	if (!response->resp_msg) {
		devm_kfree(generic_service_priv->dev, cmd_data);
		devm_kfree(generic_service_priv->dev, response);
		return -ENOMEM;
	}

	generic_service_priv->response = response;

	msg = (struct mpfs_mss_msg) {
		.cmd_opcode = *(cmd_data),
		.cmd_data_size = *(u16 *)(cmd_data + 1),
		.response = generic_service_priv->response,
		.cmd_data = cmd_data + MPFS_MIN_PAYLOAD,
		.mbox_offset = *(u16 *)(cmd_data + 5),
		.resp_offset = *(u16 *)(cmd_data + 7)
	};

	if (msg.cmd_data_size != len - MPFS_MIN_PAYLOAD) {
		devm_kfree(generic_service_priv->dev, cmd_data);
		mpfs_generic_service_free_message_structs();
		return -EINVAL;
	}

	ret = mpfs_blocking_transaction(generic_service_priv->sys_controller, &msg);
	devm_kfree(generic_service_priv->dev, cmd_data);
	if (ret) {
		mpfs_generic_service_free_message_structs();
		return -EIO;
	}

	return len;
}

static ssize_t mpfs_generic_service_read(struct file *filp, char __user *userbuf,
					 size_t len, loff_t *f_pos)
{
	u8 *buffer, *response_msg;
	int ret;

	if (!generic_service_priv->response)
		return -ENOMSG;

	response_msg = (u8 *)generic_service_priv->response->resp_msg;
	buffer = devm_kmalloc(generic_service_priv->dev,
			      generic_service_priv->response->resp_size + 1, GFP_KERNEL);

	if (!buffer) {
		mpfs_generic_service_free_message_structs();
		return -ENOMEM;
	}

	*buffer = generic_service_priv->response->resp_status;

	memcpy(buffer + 1, response_msg, generic_service_priv->response->resp_size);

	ret = simple_read_from_buffer(userbuf, len, f_pos, buffer,
				      generic_service_priv->response->resp_size + 1);

	mpfs_generic_service_free_message_structs();
	devm_kfree(generic_service_priv->dev, buffer);

	return ret;
}

static int mpfs_generic_service_open(struct inode *inode, struct file *filp)
{
	if (!mutex_trylock(&mpfs_generic_service_mutex)) {
		pr_debug("Device Busy\n");
		return -EBUSY;
	}
	return 0;
}

static int mpfs_generic_service_release(struct inode *inode, struct file *filp)
{
	mutex_unlock(&mpfs_generic_service_mutex);
	return 0;
}

static const struct file_operations mpfs_generic_service_fops = {
	.owner = THIS_MODULE,
	.read = mpfs_generic_service_read,
	.open = mpfs_generic_service_open,
	.write = mpfs_generic_service_write,
	.release = mpfs_generic_service_release
};

static struct miscdevice mpfs_generic_service_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mpfs_generic_service",
	.fops = &mpfs_generic_service_fops
};

static int mpfs_generic_service_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	generic_service_priv =
		devm_kzalloc(dev, sizeof(*generic_service_priv), GFP_KERNEL);
	if (!generic_service_priv)
		return -ENOMEM;

	generic_service_priv->sys_controller =  mpfs_sys_controller_get(&pdev->dev);
	if (IS_ERR(generic_service_priv->sys_controller))
		return dev_err_probe(dev, PTR_ERR(generic_service_priv->sys_controller),
				     "Could not register as a sub device of the system controller\n");

	generic_service_priv->dev = dev;

	platform_set_drvdata(pdev, generic_service_priv);
	misc_register(&mpfs_generic_service_dev);
	/*
	 * This driver exposes all the system controller functions to userspace
	 * without performing any validation. Additionally, while access to the
	 * system controller itself is serialised, services requested using this
	 * driver from userspace may interfere with kernel's own use of the
	 * same services.
	 */
	dev_warn(&pdev->dev,
		 "Registered MPFS generic service - FOR DEVELOPMENT ONLY, DO NOT USE IN PRODUCTION\n");

	return 0;
}

static const struct of_device_id mpfs_generic_service_of_match[] = {
	{
		.compatible = "microchip,mpfs-generic-service",
	},
	{}
};
MODULE_DEVICE_TABLE(of, mpfs_generic_service_of_match);

static struct platform_driver mpfs_generic_service_driver = {
	.driver = {
	.name = "mpfs-generic-service",
	.of_match_table = mpfs_generic_service_of_match,
	},
	.probe = mpfs_generic_service_probe,
};
module_platform_driver(mpfs_generic_service_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Conor Dooley <conor.dooley@microchip.com>");
MODULE_DESCRIPTION("PolarFire SoC (MPFS) generic service driver");
