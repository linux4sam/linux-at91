// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Raspberry Pi Sense HAT 8x8 LED matrix display driver
 * http://raspberrypi.org
 *
 * Copyright (C) 2015 Raspberry Pi
 * Copyright (C) 2021 Charles Mirabile, Joel Savitz
 *
 * Original Author: Serge Schneider
 * Revised for upstream Linux by: Charles Mirabile, Joel Savitz
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/miscdevice.h>
#include <linux/regmap.h>
#include <linux/property.h>

#define DISPLAY_SMB_REG 0x00
#define RGB_555_MASK 0x1f
#define NUM_LEDS 8
#define NUM_CHANNELS 3

struct sensehat_display {
	struct platform_device *pdev;
	struct miscdevice mdev;
	struct mutex rw_mtx;
	u8 vmem[NUM_LEDS][NUM_LEDS][NUM_CHANNELS];
	struct regmap *regmap;
};

#define VMEM_SIZE sizeof_field(struct sensehat_display, vmem)


static int sensehat_update_display(struct sensehat_display *display)
{
	int i, j, k, ret;
	u8 buff[NUM_LEDS][NUM_CHANNELS][NUM_LEDS];

	for (i = 0; i < NUM_LEDS; ++i)
		for (j = 0; j < NUM_LEDS; ++j)
			for (k = 0; k < NUM_CHANNELS; ++k)
				buff[i][k][j] =
					display->vmem[i][j][k] & RGB_555_MASK;

	ret = regmap_bulk_write(display->regmap, DISPLAY_SMB_REG, buff,
				VMEM_SIZE);
	if (ret < 0)
		dev_err(&display->pdev->dev,
			"Update to 8x8 LED matrix display failed");
	return ret;
}

static loff_t sensehat_display_llseek(struct file *filp, loff_t offset,
				      int whence)
{
	return fixed_size_llseek(filp, offset, whence, VMEM_SIZE);
}

static ssize_t sensehat_display_read(struct file *filp, char __user *buf,
				     size_t count, loff_t *f_pos)
{
	struct sensehat_display *sensehat_display =
		container_of(filp->private_data, struct sensehat_display, mdev);
	ssize_t ret = -EFAULT;

	if (*f_pos < 0 || *f_pos >= VMEM_SIZE)
		return 0;
	count = min_t(size_t, count, VMEM_SIZE - *f_pos);

	if (mutex_lock_interruptible(&sensehat_display->rw_mtx))
		return -ERESTARTSYS;
	if (copy_to_user(buf, *f_pos + (u8 *)sensehat_display->vmem, count))
		goto out;
	*f_pos += count;
	ret = count;
out:
	mutex_unlock(&sensehat_display->rw_mtx);
	return ret;
}

static ssize_t sensehat_display_write(struct file *filp, const char __user *buf,
				      size_t count, loff_t *f_pos)
{
	struct sensehat_display *sensehat_display =
		container_of(filp->private_data, struct sensehat_display, mdev);
	int ret = -EFAULT;

	if (*f_pos < 0 || *f_pos >= VMEM_SIZE)
		return -EFBIG;
	count = min_t(size_t, count, VMEM_SIZE - *f_pos);

	if (mutex_lock_interruptible(&sensehat_display->rw_mtx))
		return -ERESTARTSYS;
	if (copy_from_user(*f_pos + (u8 *)sensehat_display->vmem, buf, count))
		goto out;
	ret = sensehat_update_display(sensehat_display);
	if (ret < 0) {
		ret = -EIO;
		goto out;
	}
	*f_pos += count;
	ret = count;
out:
	mutex_unlock(&sensehat_display->rw_mtx);
	return ret;
}

static const struct file_operations sensehat_display_fops = {
	.owner = THIS_MODULE,
	.llseek = sensehat_display_llseek,
	.read = sensehat_display_read,
	.write = sensehat_display_write,
};

static int sensehat_display_probe(struct platform_device *pdev)
{
	int ret;

	struct sensehat_display *sensehat_display =
		devm_kmalloc(&pdev->dev, sizeof(*sensehat_display), GFP_KERNEL);
	if (!sensehat_display)
		return -ENOMEM;

	sensehat_display->pdev = pdev;

	dev_set_drvdata(&pdev->dev, sensehat_display);

	sensehat_display->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!sensehat_display->regmap) {
		dev_err(&pdev->dev,
			"unable to get sensehat regmap");
		return -ENODEV;
	}

	memset(sensehat_display->vmem, 0, VMEM_SIZE);

	mutex_init(&sensehat_display->rw_mtx);

	ret = sensehat_update_display(sensehat_display);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Could not communicate with sensehat");
		return ret;
	}

	sensehat_display->mdev = (struct miscdevice){
		.minor = MISC_DYNAMIC_MINOR,
		.name = "sense-hat",
		.mode = 0666,
		.fops = &sensehat_display_fops,
	};

	ret = misc_register(&sensehat_display->mdev);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Could not register 8x8 LED matrix display.");
		return ret;
	}

	dev_info(&pdev->dev,
		 "8x8 LED matrix display registered with minor number %i",
		 sensehat_display->mdev.minor);

	return 0;
}

static int sensehat_display_remove(struct platform_device *pdev)
{
	struct sensehat_display *sensehat_display = dev_get_drvdata(&pdev->dev);

	misc_deregister(&sensehat_display->mdev);
	return 0;
}

static const struct of_device_id sensehat_display_device_id[] = {
	{ .compatible = "raspberrypi,sensehat-display" },
	{},
};
MODULE_DEVICE_TABLE(of, sensehat_display_device_id);

static struct platform_driver sensehat_display_driver = {
	.probe = sensehat_display_probe,
	.remove = sensehat_display_remove,
	.driver = {
		.name = "sensehat-display",
		.of_match_table = sensehat_display_device_id,
	},
};

module_platform_driver(sensehat_display_driver);

MODULE_DESCRIPTION("Raspberry Pi Sense HAT 8x8 LED matrix display driver");
MODULE_AUTHOR("Charles Mirabile <cmirabil@redhat.com>");
MODULE_AUTHOR("Serge Schneider <serge@raspberrypi.org>");
MODULE_LICENSE("GPL");
