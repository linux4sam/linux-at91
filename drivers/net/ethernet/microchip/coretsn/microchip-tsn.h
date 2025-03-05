/* SPDX-License-Identifier: (GPL-2.0) */
/**
 * Microchip CoreTSN driver
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Pallela Venkat Karthik <pallela.karthik@microchip.com>
 *
 */

#ifndef __MICROCHIP_TSN_H__
#define __MICROCHIP_TSN_H__

#include <linux/module.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <asm/barrier.h>

/* CoreTSN device, driver strings */
#define MCHP_TSN_PDEV_DRV_NAME		"microchip-coretsn"
#define MCHP_TSN_CHARDEV_NAME		"mchpcoretsn"
#define MCHP_TSN_CHARDEV_CLASS_NAME	"mchpcoretsn-class"
#define MCHP_TSN_NUM_CHARDEVS		1

/**
 * struct mchp_tsn_dev - private per device data
 * @pdev:		Pointer to platform device
 * @classdev:		Pointer to class device
 * @cdevname:		Name of character device
 * @cdev:		Character device
 * @class:		Pointer to class
 * @dev:		Device
 * @tsn_dev_mutex:		Mutex to lock multiple configurations at a time
 * @tsn_reg_base:	IO Mapped base address
 * @tsn_dev_id:		TSN device id
 * @core_clk_rate:	Operating clock rate of TSN Device
 * @qbv_gcl_mul:	Multiplier for Qbv scheduler time fields
 * @qbv_gcl_div:	Divider for Qbv scheduler time fields
 * @tsn_config_buff:	Buffer which holds command and response
 */
struct mchp_tsn_dev {
	struct platform_device *pdev;
	struct device *classdev;
	char cdevname[32];
	struct cdev cdev;
	struct class *class;
	dev_t dev;
	struct mutex tsn_dev_mutex;
	void __iomem *tsn_reg_base;
	u64 tsn_dev_id;
	u32 core_clk_rate;
	u32 qbv_gcl_mul;
	u32 qbv_gcl_div;
	u8 tsn_config_buff[];
};

/* Get or Set TSN configuration */

#define mchp_tsn_get(tsn_dev, reg, mask) \
	FIELD_GET(mask, readl_relaxed(tsn_dev->tsn_reg_base + reg))

#define mchp_tsn_set(tsn_dev, reg, data, mask) \
	mchp_tsn_write_register(tsn_dev, reg, FIELD_PREP(mask, data), mask)

/* TSN Register size */
#define TSN_REG_SIZE sizeof(u32)

/* QCI REGS and MASKS */
#define TSN_REG_PSC			0x020
#define TSN_REG_DST_MAC_MSB		0x008
#define TSN_REG_DST_MAC_LSB		0x00C
#define TSN_REG_SRC_MAC_MSB		0x010
#define TSN_REG_SRC_MAC_LSB		0x014
#define TSN_MASK_DA_CHECK		0x00000001
#define TSN_MASK_SA_CHECK		0x00000002
#define TSN_MASK_MAC_MSB		0xFFFFFFFF
#define TSN_MASK_MAC_LSB		0x0000FFFF

/* QBU REGS and MASKS */
#define TSN_REG_PREEMPT_CONTROL		0x000
#define TSN_MASK_PREEMPT_EN		0x00000001
#define TSN_MASK_PREEMPT_FRAG_SIZE	0x00000006

/* QBV REGS and MASKS */
#define TSN_REG_PCR			0x024
#define TSN_REG_PQE			0x028
#define TSN_REG_PQ0VR			0x02C
#define TSN_REG_SCTR			0x060
#define TSN_REG_SGSR			0x080
#define TSN_REG_SCLLR			0x084
#define TSN_REG_SGCL0ER			0x08C
#define TSN_REG_BTLR			0x070
#define TSN_REG_BTHR0			0x074
#define TSN_REG_BTHR1			0x078
#define TSN_REG_TIME_ADJUST		0x118
#define TSN_MASK_CONFIG_EN		0x00000004
#define TSN_MASK_INIT_GATE_STATE	0x000000FF
#define TSN_MASK_PRIO_ENABLE		0x00000001
#define TSN_MASK_GATE_ENABLE		0x00000008
#define TSN_MASK_PRIOQ_ENABLE		0x0000007F
#define TSN_MASK_CYCLE_TIME		0xFFFFFFFF
#define TSN_MASK_BASETIME_HIGH		0xFFFFFFFF
#define TSN_MASK_BASETIME_LOW		0xFFFFFFFF
#define TSN_MASK_BASETIME_ADJUST	0x0000003F
#define TSN_MASK_CONTROL_LIST_LENGTH	0x0000003F
#define TSN_MASK_PRIOQ_PRIO		0x00000007
#define TSN_MASK_TIME_INTERVAL		0x00FFFFFF
#define TSN_MASK_GATESTATE		0xFF000000
#define MAX_TSN_GCL_LEN			32

/* Destination RX Port REGS and MASKS */
#define TSN_REG_DRXPOID			0x110
#define TSN_MASK_PORT_ID_RX		0x0000FFFF
#define TSN_MASK_PORT_ID_RX_CHECK	0x00000004

/* PTP Transmit Queue REGS and MASKS */
#define TSN_REG_PTP_TX_PRIOQ		0x10C
#define TSN_MASK_PTP_TX_PRIOQ		0x00000007

/* Packet Length Deduct REGS and MASKS */
#define TSN_REG_LDB			0x114
#define TSN_MASK_LDB			0x000007FF

#define TSN_MASK_DEV_ID			0x0000FFFF

#endif /* __MICROCHIP_TSN_H__ */
