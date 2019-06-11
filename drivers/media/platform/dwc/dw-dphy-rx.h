/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI D-PHY controller driver
 *
 * Author: Luis Oliveira <luis.oliveira@synopsys.com>
 */

#ifndef __PHY_SNPS_DPHY_RX_H__
#define __PHY_SNPS_DPHY_RX_H__

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

/* DPHY interface register bank*/

#define R_CSI2_DPHY_SHUTDOWNZ 0x0
#define R_CSI2_DPHY_RSTZ 0x4
#define R_CSI2_DPHY_RX 0x8
#define	R_CSI2_DPHY_STOPSTATE 0xC
#define R_CSI2_DPHY_TST_CTRL0 0x10
#define R_CSI2_DPHY_TST_CTRL1 0x14
#define R_CSI2_DPHY2_TST_CTRL0 0x18
#define R_CSI2_DPHY2_TST_CTRL1 0x1C

enum dphy_id_mask {
	DPHY_ID_LANE_SUPPORT = 0,
	DPHY_ID_IF = 4,
	DPHY_ID_GEN = 8,
};

enum dphy_gen_values {
	GEN1,
	GEN2,
	GEN3,
};

enum dphy_interface_length {
	BIT8 = 8,
	BIT12 = 12,
};

enum tst_ctrl0 {
	PHY_TESTCLR,
	PHY_TESTCLK,
};

enum tst_ctrl1 {
	PHY_TESTDIN = 0,
	PHY_TESTDOUT = 8,
	PHY_TESTEN = 16,
};

enum lanes_config_values {
	CTRL_4_LANES,
	CTRL_8_LANES,
};

enum dphy_tc {
	CFGCLKFREQRANGE_TX = 0x02,
	CFGCLKFREQRANGE_RX = 0x05,
	BYPASS = 0x20,
	IO_DS = 0x30,
};

enum dphy_8bit_interface_addr {
	BANDGAP_CTRL = 0x24,
	HS_RX_CTRL_LANE0 = 0x42,
	HSFREQRANGE_8BIT = 0x44,
	OSC_FREQ_TARGET_RX0_LSB	= 0x4e,
	OSC_FREQ_TARGET_RX0_MSB	= 0x4f,
	HS_RX_CTRL_LANE1 = 0x52,
	OSC_FREQ_TARGET_RX1_LSB	= 0x5e,
	OSC_FREQ_TARGET_RX1_MSB	= 0x5f,
	RX_SKEW_CAL = 0x7e,
	HS_RX_CTRL_LANE2 = 0x82,
	OSC_FREQ_TARGET_RX2_LSB	= 0x8e,
	OSC_FREQ_TARGET_RX2_MSB	= 0x8f,
	HS_RX_CTRL_LANE3 = 0x92,
	OSC_FREQ_TARGET_RX3_LSB	= 0x9e,
	OSC_FREQ_TARGET_RX3_MSB	= 0x9f,
};

enum dphy_12bit_interface_addr {
	RX_SYS_0 = 0x01,
	RX_SYS_1 = 0x02,
	RX_SYS_7 = 0x08,
	RX_RX_STARTUP_OVR_0 = 0xe0,
	RX_RX_STARTUP_OVR_1 = 0xe1,
	RX_RX_STARTUP_OVR_2 = 0xe2,
	RX_RX_STARTUP_OVR_3 = 0xe3,
	RX_RX_STARTUP_OVR_4 = 0xe4,
};

#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
/* Testchip interface register bank */
#define IDLYCFG	0x00
#define IDLYSEL	0x04
#define IDLYCNTINVAL 0x08
#define IDLYCNTOUTVAL 0x0c
#define DPHY1REGRSTN 0x10
#define DPHYZCALSTAT 0x14
#define DPHYZCALCTRL 0x18
#define DPHYLANE0STAT 0x1c
#define DPHYLANE1STAT 0x20
#define DPHYLANE2STAT 0x24
#define DPHYLANE3STAT 0x28
#define DPHYCLKSTAT 0x2c
#define DPHYZCLKCTRL 0x30
#define TCGENPURPOSOUT 0x34
#define TCGENPURPOSIN 0x38
#define DPHYGENERICOUT 0x3c
#define DPHYGENERICIN 0x40
#define DPHYGLUEIFTESTER 0x44
#define DPHYID 0x100

#define DPHY_DEFAULT_FREQ 300000

enum glueiftester {
	RESET = 0x0,
	TX_PHY = 0x1,
	RX_PHY = 0x2,
	GLUELOGIC = 0x4,
};
#endif

/**
 * struct phy		specifies associated phy component
 * struct cfg		to pass mipi dphy specific configurations
 * @lanes_config	lanes configuration
 * @dphy_freq		operating frequency of the d-phy (mbps)
 * @phy_type		dphy can be of two types, passed here
 * @dphy_gen		dphy can be of three generations, passed here
 * @dphy_te_len		bus width
 * @max_lanes		maximum number of lanes
 * @lp_time		time in low-power
 * @base_address	memmory address of dphy test interface
 * @dphy1_if_addr	gluelogic dphy 1 memmory address of interface
 * @dphy2_if_addr	gluelogic dphy 2 memmory address of interface
 * @config_8l		eight lanes configuration
 */

struct dw_dphy_rx {
	struct phy *phy;
	struct phy_configure_opts_mipi_dphy *cfg;
	u32 lanes_config;
	u32 dphy_freq;
	u32 phy_type;
	u32 dphy_gen;
	u32 dphy_te_len;
	u32 max_lanes;
	u32 lp_time;
	void __iomem *base_address;
#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
	void __iomem *dphy1_if_addr;
	void __iomem *dphy2_if_addr;
	u8 config_8l;
	u8 (*get_config_8l)(struct device *dev, struct dw_dphy_rx *dphy);
#endif
	u8 (*phy_register)(struct device *dev);
	void (*phy_unregister)(struct device *dev);
};

int dw_dphy_init(struct phy *phy);
int dw_dphy_reset(struct phy *phy);
int dw_dphy_power_off(struct phy *phy);
int dw_dphy_power_on(struct phy *phy);
u8 dw_dphy_setup_config(struct dw_dphy_rx *dphy);
void dw_dphy_write(struct dw_dphy_rx *dphy, u32 address, u32 data);
u32 dw_dphy_read(struct dw_dphy_rx *dphy, u32 address);
int dw_dphy_te_read(struct dw_dphy_rx *dphy, u32 addr);

#if IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)
u32 dw_dphy_if_read(struct dw_dphy_rx *dphy, u32 address);
int dw_dphy_if_get_idelay(struct dw_dphy_rx *dphy);
int dw_dphy_if_set_idelay_lane(struct dw_dphy_rx *dphy, u8 dly, u8 lane);
int dw_dphy_create_capabilities_sysfs(struct platform_device *pdev);

static inline
u32 dw_dphy_if_read_msk(struct dw_dphy_rx *dphy,
			u32 address, u8 shift, u8 width)
{
	return (dw_dphy_if_read(dphy, address) >> shift) & ((1 << width) - 1);
}
#endif /*IS_ENABLED(CONFIG_DWC_MIPI_TC_DPHY_GEN3)*/

static inline struct phy *dw_dphy_xlate(struct device *dev,
					struct of_phandle_args *args)
{
	struct dw_dphy_rx *dphy = dev_get_drvdata(dev);

	return dphy->phy;
}

static inline
u32 dw_dphy_read_msk(struct dw_dphy_rx *dev, u32 address, u8 shift,  u8 width)
{
	return (dw_dphy_read(dev, address) >> shift) & ((1 << width) - 1);
}
#endif /*__PHY_SNPS_DPHY_RX_H__*/
