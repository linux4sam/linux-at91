// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI CSI-2 Host controller driver.
 * SysFS components for the platform driver
 *
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 */

#include "dw-mipi-csi.h"

static ssize_t core_version_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "v.%d.%d*\n", csi_dev->hw_version_major,
		 csi_dev->hw_version_minor);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t n_lanes_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int ret;
	unsigned long lanes;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &lanes);
	if (ret < 0)
		return ret;

	if (lanes > 8) {
		dev_err(dev, "Invalid number of lanes %lu\n", lanes);
		return count;
	}

	dev_info(dev, "Lanes %lu\n", lanes);
	csi_dev->hw.num_lanes = lanes;

	return count;
}

static ssize_t n_lanes_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%d\n", csi_dev->hw.num_lanes);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t core_reset_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	/* Reset Controller and DPHY */
	phy_reset(csi_dev->phy);
	dw_mipi_csi_reset(csi_dev);

	snprintf(buffer, 10, "Reset\n");

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t data_type_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int ret;
	unsigned long dt;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &dt);
	if (ret < 0)
		return ret;

	if (dt < 0x18 || dt > 0x2F) {
		dev_err(dev, "Invalid data type %lx\n", dt);
		return count;
	}

	dev_info(dev, "Data type 0x%lx\n", dt);
	csi_dev->ipi_dt = dt;

	return count;
}

static ssize_t data_type_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->ipi_dt);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t hsa_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret;
	unsigned long hsa;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &hsa);
	if (ret < 0)
		return ret;

	if (hsa > 0xFFF) {
		dev_err(dev, "Invalid HSA time %lx\n", hsa);
		return count;
	}

	dev_info(dev, "HSA time 0x%lx\n", hsa);
	csi_dev->hw.hsa = hsa;

	return count;
}

static ssize_t hsa_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->hw.hsa);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t hbp_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret;
	unsigned long hbp;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &hbp);
	if (ret < 0)
		return ret;

	if (hbp > 0xFFF) {
		dev_err(dev, "Invalid HBP time %lx\n", hbp);
		return count;
	}

	dev_info(dev, "HBP time 0x%lx\n", hbp);
	csi_dev->hw.hbp = hbp;

	return count;
}

static ssize_t hbp_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->hw.hbp);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t hsd_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret;
	unsigned long hsd;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &hsd);
	if (ret < 0)
		return ret;

	if (hsd > 0xFF) {
		dev_err(dev, "Invalid HSD time %lx\n", hsd);
		return count;
	}

	dev_info(dev, "HSD time 0x%lx\n", hsd);
	csi_dev->hw.hsd = hsd;

	return count;
}

static ssize_t hsd_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->hw.hsd);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t vsa_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret;
	unsigned long vsa;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &vsa);
	if (ret < 0)
		return ret;

	if (vsa > 0x3FF) {
		dev_err(dev, "Invalid VSA period %lx\n", vsa);
		return count;
	}

	dev_info(dev, "VSA period 0x%lx\n", vsa);
	csi_dev->hw.vsa = vsa;

	return count;
}

static ssize_t vsa_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->hw.vsa);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t vbp_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret;
	unsigned long vbp;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &vbp);
	if (ret < 0)
		return ret;

	if (vbp > 0x2FF) {
		dev_err(dev, "Invalid VBP period %lx\n", vbp);
		return count;
	}

	dev_info(dev, "VBP period 0x%lx\n", vbp);
	csi_dev->hw.vbp = vbp;

	return count;
}

static ssize_t vbp_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->hw.vbp);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t vfp_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret;
	unsigned long vfp;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &vfp);
	if (ret < 0)
		return ret;

	if (vfp > 0x3ff) {
		dev_err(dev, "Invalid VFP period %lx\n", vfp);
		return count;
	}

	dev_info(dev, "VFP period 0x%lx\n", vfp);
	csi_dev->hw.vfp = vfp;

	return count;
}

static ssize_t vfp_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%x\n", csi_dev->hw.vfp);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t virtual_channel_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned long virtual_ch;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &virtual_ch);
	if (ret < 0)
		return ret;

	if ((signed int)virtual_ch < 0 || (signed int)virtual_ch > 8) {
		dev_err(dev, "Invalid Virtual Channel %lu\n", virtual_ch);
		return count;
	}

	dev_info(dev, "Virtual Channel %lu\n", virtual_ch);
	csi_dev->hw.virtual_ch = virtual_ch;

	return count;
}

static ssize_t virtual_channel_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%d\n", csi_dev->hw.virtual_ch);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t ipi_color_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int ret;
	unsigned long ipi_color_mode;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &ipi_color_mode);
	if (ret < 0)
		return ret;

	if ((signed int)ipi_color_mode < 0 || (signed int)ipi_color_mode > 1) {
		dev_err(dev,
			"Wrong Color Mode %lu, (48 bits -> 0 or 16 bits -> 1\n",
			ipi_color_mode);
		return count;
	}

	dev_info(dev, "IPI Color mode %lu\n", ipi_color_mode);
	csi_dev->hw.ipi_color_mode = ipi_color_mode;

	return count;
}

static ssize_t ipi_color_mode_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%d\n", csi_dev->hw.ipi_color_mode);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t ipi_auto_flush_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int ret;
	unsigned long ipi_auto_flush;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &ipi_auto_flush);
	if (ret < 0)
		return ret;

	if ((signed int)ipi_auto_flush < 0 || (signed int)ipi_auto_flush > 1) {
		dev_err(dev,
			"Invalid Auto Flush Mode %lu, (No -> 0 or Yes -> 1\n",
			ipi_auto_flush);
		return count;
	}

	dev_info(dev, "IPI Auto Flush %lu\n", ipi_auto_flush);
	csi_dev->hw.ipi_auto_flush = ipi_auto_flush;

	return count;
}

static ssize_t ipi_auto_flush_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%d\n", csi_dev->hw.ipi_auto_flush);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t ipi_timings_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int ret;
	unsigned long ipi_mode;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &ipi_mode);
	if (ret < 0)
		return ret;

	if ((signed int)ipi_mode < 0 || (signed int)ipi_mode > 1) {
		dev_err(dev,
			"Invalid Timing Source %lu (Camera:0|Controller:1)\n",
			ipi_mode);
		return count;
	}

	dev_info(dev, "IPI Color mode %lu\n", ipi_mode);
	csi_dev->hw.ipi_mode = ipi_mode;

	return count;
}

static ssize_t ipi_timings_mode_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%d\n", csi_dev->hw.ipi_mode);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t output_type_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long output;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &output);
	if (ret < 0)
		return ret;

	if ((signed int)output < 0 || (signed int)output > 1) {
		dev_err(dev,
			"Invalid Core output %lu to be used (IPI-> 0 or IDI->1 or BOTH- 2\n",
			output);
		return count;
	}

	dev_info(dev, "IPI Color mode %lu\n", output);
	csi_dev->hw.output = output;

	return count;
}

static ssize_t output_type_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct dw_csi *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "%d\n", csi_dev->hw.output);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static DEVICE_ATTR_RO(core_version);
static DEVICE_ATTR_RO(core_reset);
static DEVICE_ATTR_RW(n_lanes);
static DEVICE_ATTR_RW(data_type);
static DEVICE_ATTR_RW(hsa);
static DEVICE_ATTR_RW(hbp);
static DEVICE_ATTR_RW(hsd);
static DEVICE_ATTR_RW(vsa);
static DEVICE_ATTR_RW(vbp);
static DEVICE_ATTR_RW(vfp);
static DEVICE_ATTR_RW(virtual_channel);
static DEVICE_ATTR_RW(ipi_color_mode);
static DEVICE_ATTR_RW(ipi_auto_flush);
static DEVICE_ATTR_RW(ipi_timings_mode);
static DEVICE_ATTR_RW(output_type);

int dw_csi_create_capabilities_sysfs(struct platform_device *pdev)
{
	device_create_file(&pdev->dev, &dev_attr_core_version);
	device_create_file(&pdev->dev, &dev_attr_core_reset);
	device_create_file(&pdev->dev, &dev_attr_n_lanes);
	device_create_file(&pdev->dev, &dev_attr_data_type);
	device_create_file(&pdev->dev, &dev_attr_hsa);
	device_create_file(&pdev->dev, &dev_attr_hbp);
	device_create_file(&pdev->dev, &dev_attr_hsd);
	device_create_file(&pdev->dev, &dev_attr_vsa);
	device_create_file(&pdev->dev, &dev_attr_vbp);
	device_create_file(&pdev->dev, &dev_attr_vfp);
	device_create_file(&pdev->dev, &dev_attr_virtual_channel);
	device_create_file(&pdev->dev, &dev_attr_ipi_color_mode);
	device_create_file(&pdev->dev, &dev_attr_ipi_auto_flush);
	device_create_file(&pdev->dev, &dev_attr_ipi_timings_mode);
	device_create_file(&pdev->dev, &dev_attr_output_type);

	return 0;
}
