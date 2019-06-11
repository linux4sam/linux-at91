// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI D-PHY controller driver.
 * SysFS components for the platform driver
 *
 * Author: Luis Oliveira <luis.oliveira@synopsys.com>
 */

#include "dw-dphy-rx.h"

static ssize_t dphy_reset_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	char buffer[15];

	dw_dphy_write(dphy, R_CSI2_DPHY_RSTZ, 0);
	usleep_range(100, 200);
	dw_dphy_write(dphy, R_CSI2_DPHY_RSTZ, 1);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t dphy_freq_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	int ret;
	unsigned long freq;

	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);

	ret = kstrtoul(buf, 10, &freq);
	if (ret < 0)
		return ret;

	if (freq > 2500) {
		dev_info(dev, "Freq must be under 2500 Mhz\n");
		return count;
	}
	if (freq < 80) {
		dev_info(dev, "Freq must be over 80 Mhz\n");
		return count;
	}

	dev_vdbg(dev, "Data Rate %lu Mbps\n", freq);
	dphy->dphy_freq = freq;

	return count;
}

static ssize_t dphy_freq_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	char buffer[15];

	snprintf(buffer,
		 sizeof(buffer),
		 "Freq %d\n", dphy->dphy_freq);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t dphy_addr_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	unsigned long val;
	u8 addr, payload;
	int ret;

	ret = kstrtoul(buf, 32, &val);
	if (ret < 0)
		return ret;

	payload = (u16)val;
	addr = (u16)(val >> 16);

	dev_vdbg(dev, "addr 0x%lX\n", val);
	dev_vdbg(dev, "payload: 0x%X\n", addr);
	dev_vdbg(dev, "Addr [0x%x] -> 0x%x\n", (unsigned int)addr,
		 dw_dphy_te_read(dphy, addr));

	return count;
}

#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
static ssize_t idelay_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	char buffer[15];

	snprintf(buffer,
		 sizeof(buffer), "idelay %d\n", dw_dphy_if_get_idelay(dphy));

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t idelay_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	unsigned long val;
	u8 lane, delay;
	int ret;

	ret = kstrtoul(buf, 16, &val);
	if (ret < 0)
		return ret;

	lane = (u8)val;
	delay = (u8)(val >> 8);

	dev_vdbg(dev, "Lanes %u\n", lane);
	dev_vdbg(dev, "Delay %u\n", delay);

	dw_dphy_if_set_idelay_lane(dphy, delay, lane);

	return count;
}
#endif

static ssize_t len_config_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf,
				size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	unsigned long length;
	int ret;

	ret = kstrtoul(buf, 10, &length);
	if (ret < 0)
		return ret;

	if (length == BIT8)
		dev_vdbg(dev, "Configured for 8-bit interface\n");
	else if (length == BIT12)
		dev_vdbg(dev, "Configured for 12-bit interface\n");
	else
		return count;

	dphy->dphy_te_len = length;

	return count;
}

static ssize_t len_config_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	char buffer[20];

	snprintf(buffer, sizeof(buffer), "Length %d\n", dphy->dphy_te_len);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t dw_dphy_g118_settle_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	unsigned long lp_time;
	int ret;

	ret = kstrtoul(buf, 10, &lp_time);
	if (ret < 0)
		return ret;

	if (lp_time > 1 && lp_time < 10000) {
		dphy->lp_time = lp_time;
	} else {
		dev_vdbg(dev, "Invalid Value configuring for 1000 ns\n");
		dphy->lp_time = 1000;
	}

	dphy->lp_time = lp_time;

	return count;
}

static ssize_t dw_dphy_g118_settle_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_dphy_rx *dphy = platform_get_drvdata(pdev);
	char buffer[10];

	snprintf(buffer, sizeof(buffer), "Settle %d ns\n", dphy->lp_time);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static DEVICE_ATTR_RO(dphy_reset);
static DEVICE_ATTR_RW(dphy_freq);
static DEVICE_ATTR_WO(dphy_addr);
#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
static DEVICE_ATTR_RW(idelay);
#endif
static DEVICE_ATTR_RW(len_config);
static DEVICE_ATTR_RW(dw_dphy_g118_settle);

int dw_dphy_create_capabilities_sysfs(struct platform_device *pdev)
{
	device_create_file(&pdev->dev, &dev_attr_dphy_reset);
	device_create_file(&pdev->dev, &dev_attr_dphy_freq);
	device_create_file(&pdev->dev, &dev_attr_dphy_addr);
#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
	device_create_file(&pdev->dev, &dev_attr_idelay);
#endif
	device_create_file(&pdev->dev, &dev_attr_len_config);
	device_create_file(&pdev->dev, &dev_attr_dw_dphy_g118_settle);
	return 0;
}
