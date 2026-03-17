/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * IIO driver for MCP47FEB02 Multi-Channel DAC with I2C or SPI interface
 *
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Ariana Lazar <ariana.lazar@microchip.com>
 */
#ifndef _MCP47FEB02_H_
#define _MCP47FEB02_H_

#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/types.h>

/* Register addresses must be left shifted with 3 positions in order to append command mask */
#define MCP47FEB02_DAC0_REG_ADDR			0x00
#define MCP47FEB02_VREF_REG_ADDR			0x40
#define MCP47FEB02_POWER_DOWN_REG_ADDR			0x48
#define MCP47FEB02_DAC_CTRL_MASK			GENMASK(1, 0)

#define MCP47FEB02_GAIN_CTRL_STATUS_REG_ADDR		0x50
#define MCP47FEB02_GAIN_BIT_MASK			BIT(0)
#define MCP47FEB02_GAIN_BIT_STATUS_EEWA_MASK		BIT(6)
#define MCP47FEB02_GAIN_BITS_MASK			GENMASK(15, 8)

#define MCP47FEB02_WIPERLOCK_STATUS_REG_ADDR		0x58

#define MCP47FEB02_NV_DAC0_REG_ADDR			0x80
#define MCP47FEB02_NV_VREF_REG_ADDR			0xC0
#define MCP47FEB02_NV_POWER_DOWN_REG_ADDR		0xC8
#define MCP47FEB02_NV_GAIN_CTRL_I2C_SLAVE_REG_ADDR	0xD0
#define MCP47FEB02_NV_I2C_SLAVE_ADDR_MASK		GENMASK(7, 0)

/* Voltage reference, Power-Down control register and DAC Wiperlock status register fields */
#define DAC_CTRL_MASK(ch)				(GENMASK(1, 0) << (2 * (ch)))
#define DAC_CTRL_VAL(ch, val)				((val) << (2 * (ch)))

/* Gain Control and I2C Slave Address Reguster fields */
#define DAC_GAIN_MASK(ch)				BIT(8 + (ch))
#define DAC_GAIN_VAL(ch, val)				((val) * BIT(8 + (ch)))

#define REG_ADDR(reg)					((reg) << 3)
#define NV_REG_ADDR(reg)				((NV_DAC_ADDR_OFFSET + (reg)) << 3)
#define READFLAG_MASK					GENMASK(2, 1)

#define MCP47FEB02_MAX_CH				8
#define MCP47FEB02_MAX_SCALES_CH			3
#define MCP47FEB02_DAC_WIPER_UNLOCKED			0
#define MCP47FEB02_NORMAL_OPERATION			0
#define MCP47FEB02_INTERNAL_BAND_GAP_uV			2440000
#define NV_DAC_ADDR_OFFSET				0x10

/**
 * struct mcp47feb02_features - chip specific data
 * @name: device name
 * @phys_channels: number of hardware channels
 * @resolution: DAC resolution
 * @have_ext_vref1: does the hardware have an the second external voltage reference?
 * @have_eeprom: does the hardware have an internal eeprom?
 */
struct mcp47feb02_features {
	const char *name;
	unsigned int phys_channels;
	unsigned int resolution;
	bool have_ext_vref1;
	bool have_eeprom;
};

/* Macro used for generating chip features structures */
#define MCP47FEB02_CHIP_INFO(_name, _channels, _res, _vref1, _eeprom) \
static const struct mcp47feb02_features _name##_chip_features = { \
	.name = #_name, \
	.phys_channels = _channels, \
	.resolution = _res, \
	.have_ext_vref1 = _vref1, \
	.have_eeprom = _eeprom, \
}

enum mcp47feb02_vref_mode {
	MCP47FEB02_VREF_VDD = 0,
	MCP47FEB02_INTERNAL_BAND_GAP = 1,
	MCP47FEB02_EXTERNAL_VREF_UNBUFFERED = 2,
	MCP47FEB02_EXTERNAL_VREF_BUFFERED = 3,
};

enum mcp47feb02_scale {
	MCP47FEB02_SCALE_VDD = 0,
	MCP47FEB02_SCALE_GAIN_X1 = 1,
	MCP47FEB02_SCALE_GAIN_X2 = 2,
};

enum mcp47feb02_gain_bit_mode {
	MCP47FEB02_GAIN_BIT_X1 = 0,
	MCP47FEB02_GAIN_BIT_X2 = 1,
};

/**
 * struct mcp47feb02_channel_data - channel configuration
 * @scale_avail: scales available for the channel based on current configuration
 * @dac_data: DAC value
 * @ref_mode: chosen voltage for reference
 * @powerdown_mode: selected power-down mode
 * @use_2x_gain: output driver gain control
 * @powerdown: is false if the channel is in normal operation mode
 */
struct mcp47feb02_channel_data {
	int *scale_avail;
	u16 dac_data;
	u8 ref_mode;
	u8 powerdown_mode;
	bool use_2x_gain;
	bool powerdown;
};

/**
 * struct mcp47feb02_data - chip configuration
 * @chdata: options configured for each channel on the device
 * @lock: prevents concurrent reads/writes to driver's state members
 * @chip_features: pointer to features struct
 * @scale_1: scales set on channels that are based on Vref1
 * @scale_0: scales set on channels that are based on Vref/Vref0
 * @active_channels_mask: enabled channels
 * @regmap: regmap for directly accessing device register
 * @labels: table with channels labels
 * @phys_channels: physical channels on the device
 * @vref1_buffered: Vref1 buffer is enabled
 * @vref_buffered: Vref/Vref0 buffer is enabled
 * @use_vref1: vref1-supply is defined
 * @use_vref: vref-supply is defined
 */
struct mcp47feb02_data {
	struct mcp47feb02_channel_data chdata[MCP47FEB02_MAX_CH];
	struct mutex lock; /* prevents concurrent reads/writes to driver's state members */
	const struct mcp47feb02_features *chip_features;
	int scale_1[2 * MCP47FEB02_MAX_SCALES_CH];
	int scale_0[2 * MCP47FEB02_MAX_SCALES_CH];
	unsigned long active_channels_mask;
	struct regmap *regmap;
	const char *labels[MCP47FEB02_MAX_CH];
	u16 phys_channels;
	bool vref1_buffered;
	bool vref_buffered;
	bool use_vref1;
	bool use_vref;
};

extern const struct regmap_config mcp47feb02_regmap_config;
extern const struct regmap_config mcp47fvb02_regmap_config;

/* Properties shared by I2C and SPI families */
int mcp47feb02_common_probe(const struct mcp47feb02_features *chip_features, struct regmap *regmap);

extern const struct dev_pm_ops mcp47feb02_pm_ops;

#endif /* _MCP47FEB02_H_ */
