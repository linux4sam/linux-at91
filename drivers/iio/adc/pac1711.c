// SPDX-License-Identifier: GPL-2.0+
/*
 * IIO driver for PAC1711 Single-Channel DC Power/Energy Monitor
 *
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Ariana Lazar <ariana.lazar@microchip.com>
 *
 * Datasheet links:
 * [PAC1711]: https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/PAC1711-Data-Sheet-DS20007058.pdf
 * [PAC1721]: https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/PAC1721-Single-Channel-Power-Monitor-with-Accumulator-DS20007088.pdf
 * [PAC1811]: https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/PAC1811-Data-Sheet-DS20007066.pdf
 * [PAC1821]: https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/PAC1821-Data-Sheet-DS20007097.pdf
 */
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

/*
 * Maximum accumulation time should be 1,165 hours at 1,024 sps
 * till PAC1711 accumulation registers starts to saturate
 */
#define PAC1711_MAX_RFSH_LIMIT_MS		60000
/* 50msec is the timeout for validity of the cached registers */
#define PAC1711_MIN_POLLING_TIME_MS		50
/*
 * 1000usec is the minimum wait time for normal conversions when sample
 * rate doesn't change
 */
#define PAC1711_MIN_UPDATE_WAIT_TIME_US		1000

/* 42000mV */
#define PAC1711_VOLTAGE_MILLIVOLTS_MAX	42000
#define PAC1721_VOLTAGE_MILLIVOLTS_MAX	9000

/* Maximum power-product value - 42 V * 0.1 V */
#define PAC1711_PRODUCT_VOLTAGE_PV_FSR		4200000000000UL
#define PAC1721_PRODUCT_VOLTAGE_PV_FSR		900000000000UL

/* I2C address map */
#define PAC1711_REFRESH_REG_ADDR		0x00
#define PAC1711_ACC_COUNT_REG_ADDR		0x02
#define PAC1711_VACC_REG_ADDR			0x03
#define PAC1711_VBUS_REG_ADDR			0x04
#define PAC1711_VSENSE_REG_ADDR			0x05
#define PAC1711_VPOWER_REG_ADDR			0x08
#define PAC1711_CTRL_LAT_REG_ADDR		0x0F
#define PAC1711_SLOW_REG_ADDR			0x16
#define PAC1711_CTRL_ACT_REG_ADDR		0x17

#define PAC1711_NEG_PWR_FSR_REG_ADDR		0x13
#define PAC1711_NEG_PWR_FSR_VS_MASK		GENMASK(3, 2)
#define PAC1711_NEG_PWR_FSR_VB_MASK		GENMASK(1, 0)

#define PAC1711_CTRL_REG_ADDR			0x01
#define PAC1711_CTRL_SAMPLE_MODE_MASK		GENMASK(15, 12)
#define PAC1711_CTRL_AVERAGE_MASK		GENMASK(7, 5)

#define PAC1711_PID_REG_ADDR			0xFD
#define PAC1711_REVISION_ID_REG_ADDR		0xFF

#define PAC1711_COMMON_DEVATTR			1

/* Dimension of each register in bytes */
#define PAC1711_ACC_REG_LEN			4
#define PAC1711_VACC_REG_LEN			7
#define PAC1711_VBUS_SENSE_REG_LEN		2

/*
 * The sum of the measurement registers' dimensions in bytes - from acc_count to
 * vpower in datasheet register description.
 */
#define PAC1711_MEAS_REG_SNAPSHOT_LEN		23

#define PAC1711_ACC_VPOWER			"vpower"
#define PAC1711_ACC_VSENSE			"vsense"
#define PAC1711_VBUS_PROP			"microchip,vbus-input-range-microvolt"
#define PAC1711_VSENSE_PROP			"microchip,vsense-input-range-microvolt"

#define PAC_PRODUCT_ID_1711			0x80
#define PAC_PRODUCT_ID_1721			0x81
#define PAC_PRODUCT_ID_1811			0x84
#define PAC_PRODUCT_ID_1821			0x85

#define PAC1711_DEV_ATTR(name)			(&iio_dev_attr_##name.dev_attr.attr)

enum pac1711_ch_idx {
	PAC1711_CH_POWER,
	PAC1711_CH_VOLTAGE,
	PAC1711_CH_CURRENT,
};

enum pac1711_acc_mode {
	PAC1711_ACCMODE_VPOWER = 0,
	PAC1711_ACCMODE_VSENSE = 1,
};

enum pac1711_fsr {
	PAC1711_FULL_RANGE_UNIPOLAR = 0,
	PAC1711_FULL_RANGE_BIPOLAR = 1,
	PAC1711_HALF_RANGE_BIPOLAR = 2,
};

static const int pac1711_vbus_range_tbl[3][2] = {
	[PAC1711_FULL_RANGE_UNIPOLAR] = { 0, 42000000 },
	[PAC1711_FULL_RANGE_BIPOLAR] = { -42000000, 42000000 },
	[PAC1711_HALF_RANGE_BIPOLAR] = { -21000000, 21000000 },
};

static const int pac1711_vsense_range_tbl[3][2] = {
	[PAC1711_FULL_RANGE_UNIPOLAR] = { 0, 100000 },
	[PAC1711_FULL_RANGE_BIPOLAR] = { -100000, 100000 },
	[PAC1711_HALF_RANGE_BIPOLAR] = { -50000, 50000 },
};

enum pac1711_samps {
	PAC1711_SAMP_8192SPS = 0,
	PAC1711_SAMP_4096SPS = 1,
	PAC1711_SAMP_1024SPS = 2,
	PAC1711_SAMP_256SPS = 3,
	PAC1711_SAMP_64SPS = 4,
	PAC1711_SAMP_8SPS = 5,
};

static const unsigned int pac1711_samp_rate_map_tbl[] = {
	[PAC1711_SAMP_8192SPS] = 8192,
	[PAC1711_SAMP_4096SPS] = 4096,
	[PAC1711_SAMP_1024SPS] = 1024, // default
	[PAC1711_SAMP_256SPS] = 256,
	[PAC1711_SAMP_64SPS] = 64,
	[PAC1711_SAMP_8SPS] = 8,
};

/**
 * struct pac1711_features - features of a pac1711 instance
 * @name: chip's name
 * @prod_id: hardware ID
 */
struct pac1711_features {
	const char *name;
	u8 prod_id;
};

static const struct pac1711_features pac1711_chip_features = {
	.name = "pac1711",
	.prod_id = PAC_PRODUCT_ID_1711,
};

static const struct pac1711_features pac1721_chip_features = {
	.name = "pac1721",
	.prod_id = PAC_PRODUCT_ID_1721,
};

static const struct pac1711_features pac1811_chip_features = {
	.name = "pac1811",
	.prod_id = PAC_PRODUCT_ID_1811,
};

static const struct pac1711_features pac1821_chip_features = {
	.name = "pac1821",
	.prod_id = PAC_PRODUCT_ID_1821,
};

/**
 * struct reg_data - data from the registers
 * @vacc:		accumulated vpower value
 * @acc_val:		accumulated values per second
 * @vpower:		vpower registers
 * @vsense:		vsense registers
 * @vbus:		vbus registers
 * @acc_count:		the acc_count register
 * @total_samples_nr:	total number of samples
 * @jiffies_tstamp:	timestamp
 * @ctrl_act_reg:	the ctrl_act register
 * @ctrl_lat_reg:	the ctrl_lat register
 * @meas_regs:		snapshot of raw measurements registers
 */
struct reg_data {
	s64	vacc;
	s64	acc_val;
	s64	vpower;
	s32	vsense;
	s32	vbus;
	u32	acc_count;
	u32	total_samples_nr;
	unsigned long jiffies_tstamp;
	u16	ctrl_act_reg;
	u16	ctrl_lat_reg;
	u8	meas_regs[PAC1711_MEAS_REG_SNAPSHOT_LEN];
};

/**
 * struct pac1711_chip_info - information about the chip
 * @chip_reg_data:	measurement/control/accumulator output device registers
 * @iio_info:		device information
 * @client:		the i2c-client attached to the device
 * @work_chip_rfsh:	work queue used for refresh commands
 * @lock:		synchronize access to driver's state members
 * @shunt:		shunt resistor value
 * @vbus_mode:		Full Scale Range (FSR) mode for VBus
 * @vsense_mode:	Full Scale Range (FSR) mode for VSense
 * @accumulation_mode:	accumulation mode for hardware accumulator
 * @sample_rate_idx:	sampling frequency index
 * @chip_variant:	chip variant
 * @enable_acc:		true means that accumulation channel is measured
 * @is_pac18x1_family:	true if device is part of the PAC18x1 family
 */
struct pac1711_chip_info {
	struct reg_data		chip_reg_data;
	struct iio_info		iio_info;
	struct i2c_client	*client;
	struct delayed_work	work_chip_rfsh;
	/* Prevents concurrent writes into control, voltage measurement or accumulator registers. */
	struct mutex		lock;
	u32			shunt;
	u8			vbus_mode;
	u8			vsense_mode;
	u8			accumulation_mode;
	u8			sample_rate_idx;
	u8			chip_variant;
	bool			enable_acc;
	bool			is_pac18x1_family;
};

static inline u64 pac1711_get_unaligned_be56(u8 *p)
{
	return (u64)p[0] << 48 | (u64)p[1] << 40 | (u64)p[2] << 32 |
		(u64)p[3] << 24 | p[4] << 16 | p[5] << 8 | p[6];
}

static int pac1711_send_refresh(struct pac1711_chip_info *info, u8 refresh_cmd,
				u32 wait_time)
{
	struct i2c_client *client = info->client;
	int ret;

	/* Writing a REFRESH or a REFRESH_V command */
	ret = i2c_smbus_write_byte(client, refresh_cmd);
	if (ret) {
		dev_err(&client->dev, "%s - cannot send Refresh cmd (0x%02X)\n",
			__func__, refresh_cmd);
		return ret;
	}

	/* Register data retrieval timestamp */
	info->chip_reg_data.jiffies_tstamp = jiffies;

	/* Wait till the data is available */
	fsleep(wait_time);

	return 0;
}

static int pac1711_reg_snapshot(struct pac1711_chip_info *info,
				bool do_refresh, u8 refresh_cmd, u32 wait_time)
{
	struct i2c_client *client = info->client;
	struct device *dev = &client->dev;
	s64 stored_value, tmp_s64;
	u8 *offset_reg_data_p;
	bool is_bipolar;
	__be16 tmp_u16;
	s64 inc = 0;
	u8 shift;
	int ret;

	guard(mutex)(&info->lock);

	if (do_refresh) {
		ret = pac1711_send_refresh(info, refresh_cmd, wait_time);
		if (ret < 0) {
			dev_err(dev, "cannot send refresh\n");
			return ret;
		}
	}

	/* Read the ctrl/status registers for this snapshot */
	ret = i2c_smbus_read_i2c_block_data(client, PAC1711_CTRL_ACT_REG_ADDR,
					    sizeof(tmp_u16), (u8 *)&tmp_u16);
	if (ret < 0) {
		dev_err(dev, "%s - cannot read regs from 0x%02X\n",
			__func__, PAC1711_CTRL_ACT_REG_ADDR);
		return ret;
	}

	be16_to_cpus(&tmp_u16);
	info->chip_reg_data.ctrl_act_reg = tmp_u16;

	ret = i2c_smbus_read_i2c_block_data(client, PAC1711_CTRL_LAT_REG_ADDR,
					    sizeof(tmp_u16), (u8 *)&tmp_u16);
	if (ret < 0) {
		dev_err(dev, "%s - cannot read regs from 0x%02X\n",
			__func__, PAC1711_CTRL_LAT_REG_ADDR);
		return ret;
	}

	be16_to_cpus(&tmp_u16);
	info->chip_reg_data.ctrl_lat_reg = tmp_u16;

	/* Read the data registers */
	ret = i2c_smbus_read_i2c_block_data(client, PAC1711_ACC_COUNT_REG_ADDR,
					    PAC1711_MEAS_REG_SNAPSHOT_LEN,
					    (u8 *)info->chip_reg_data.meas_regs);
	if (ret < 0) {
		dev_err(dev, "%s - cannot read regs from 0x%02X\n",
			__func__, PAC1711_ACC_COUNT_REG_ADDR);
		return ret;
	}

	offset_reg_data_p = &info->chip_reg_data.meas_regs[0];
	info->chip_reg_data.acc_count = get_unaligned_be32(offset_reg_data_p);
	offset_reg_data_p += PAC1711_ACC_REG_LEN;

	/* skip if the energy accumulation is disabled */
	if (info->enable_acc) {
		stored_value = info->chip_reg_data.acc_val;

		info->chip_reg_data.vacc = pac1711_get_unaligned_be56(offset_reg_data_p);
		is_bipolar = false;

		switch (info->accumulation_mode) {
		case PAC1711_ACCMODE_VPOWER:
			if (info->vbus_mode != PAC1711_FULL_RANGE_UNIPOLAR ||
			    info->vsense_mode != PAC1711_FULL_RANGE_UNIPOLAR)
				is_bipolar = true;
			break;
		case PAC1711_ACCMODE_VSENSE:
			if (info->vsense_mode != PAC1711_FULL_RANGE_UNIPOLAR)
				is_bipolar = true;
			break;
		}

		if (is_bipolar)
			info->chip_reg_data.vacc = sign_extend64(info->chip_reg_data.vacc, 55);

		/*
		 * Integrate the accumulated power or current over
		 * the elapsed interval.
		 */
		tmp_u16 = FIELD_GET(PAC1711_CTRL_SAMPLE_MODE_MASK,
				    info->chip_reg_data.ctrl_lat_reg);
		tmp_s64 = info->chip_reg_data.vacc;

		if (tmp_u16 <= PAC1711_SAMP_8SPS) {
			shift = ffs(pac1711_samp_rate_map_tbl[tmp_u16]) - 1;
			inc = tmp_s64 >> shift;
		} else {
			dev_err(dev, "Invalid sample rate index: %d!\n", tmp_u16);
			return -EINVAL;
		}

		if (check_add_overflow(stored_value, inc, &stored_value)) {
			if (is_negative(stored_value))
				info->chip_reg_data.acc_val = S64_MIN;
			else
				info->chip_reg_data.acc_val = S64_MAX;

			dev_err(dev, "Accumulator Overflow detected!\n");

			return -EINVAL;
		}

		info->chip_reg_data.acc_val += inc;
	}

	offset_reg_data_p += PAC1711_VACC_REG_LEN;

	/* VBUS */
	info->chip_reg_data.vbus = get_unaligned_be16(offset_reg_data_p);

	if (info->vbus_mode != PAC1711_FULL_RANGE_UNIPOLAR)
		info->chip_reg_data.vbus = sign_extend32(info->chip_reg_data.vbus, 15);

	offset_reg_data_p += PAC1711_VBUS_SENSE_REG_LEN;

	/* VSENSE */
	info->chip_reg_data.vsense = get_unaligned_be16(offset_reg_data_p);

	if (info->vsense_mode != PAC1711_FULL_RANGE_UNIPOLAR)
		info->chip_reg_data.vsense = sign_extend32(info->chip_reg_data.vsense, 15);

	/* Skip VBUS_AVG and VSENSE_AVG registers */
	offset_reg_data_p += PAC1711_VBUS_SENSE_REG_LEN * 3;

	/* VPOWER */
	info->chip_reg_data.vpower = get_unaligned_be32(offset_reg_data_p);

	if (info->vbus_mode != PAC1711_FULL_RANGE_UNIPOLAR ||
	    info->vsense_mode != PAC1711_FULL_RANGE_UNIPOLAR)
		info->chip_reg_data.vpower = sign_extend64(info->chip_reg_data.vpower, 31);

	return 0;
}

static int pac1711_retrieve_data(struct pac1711_chip_info *info, u32 wait_time)
{
	int ret = 0;

	/*
	 * Check if the minimal elapsed time has passed and if so,
	 * read again the chip, otherwise use the cached info.
	 */
	if (time_after(jiffies, info->chip_reg_data.jiffies_tstamp +
			msecs_to_jiffies(PAC1711_MIN_POLLING_TIME_MS))) {
		ret = pac1711_reg_snapshot(info, true, PAC1711_REFRESH_REG_ADDR,
					   wait_time);

		/*
		 * Re-schedule the work for the read registers timeout
		 * (to prevent chip regs saturation)
		 */
		cancel_delayed_work_sync(&info->work_chip_rfsh);
		schedule_delayed_work(&info->work_chip_rfsh,
				      msecs_to_jiffies(PAC1711_MAX_RFSH_LIMIT_MS));
	}

	return ret;
}

static int pac1711_get_samp_rate_idx(struct pac1711_chip_info *info, u32 new_samp_rate)
{
	int cnt;

	for (cnt = 0; cnt < ARRAY_SIZE(pac1711_samp_rate_map_tbl); cnt++)
		if (new_samp_rate == pac1711_samp_rate_map_tbl[cnt])
			return cnt;

	return -EINVAL;
}

static ssize_t pac1711_read_shunt_resistor(struct iio_dev *indio_dev, uintptr_t private,
					   const struct iio_chan_spec *ch, char *buf)
{
	struct pac1711_chip_info *info = iio_priv(indio_dev);

	return sysfs_emit(buf, "%u\n", info->shunt);
}

static ssize_t pac1711_in_power_acc_raw_show(struct device *dev, struct device_attribute *attr,
					     char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	s64 curr_energy, int_part;
	int ret, rem;

	ret = pac1711_retrieve_data(info, PAC1711_MIN_UPDATE_WAIT_TIME_US);
	if (ret)
		return 0;

	/* Expresses the 64 bit energy value as a 64 bit integer and a 32 bit nano value */
	curr_energy = info->chip_reg_data.acc_val;
	int_part = div_s64_rem(curr_energy, 1000000000, &rem);

	if (rem < 0)
		return sysfs_emit(buf, "-%lld.%09u\n", abs(int_part), -rem);
	else
		return sysfs_emit(buf, "%lld.%09u\n", int_part, abs(rem));
}

static ssize_t pac1711_in_power_acc_scale_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	unsigned int rem;
	u64 tmp, ref;

	/* Scale constant = (10^3 * 4.2 * 10^9 / 2^(31 or 23)) for mili Watt-second */
	ref = info->is_pac18x1_family ? (u64)1958 : (u64)500680;

	if ((info->vsense_mode == PAC1711_FULL_RANGE_UNIPOLAR &&
	     info->vbus_mode == PAC1711_FULL_RANGE_UNIPOLAR)  ||
	    info->vsense_mode == PAC1711_HALF_RANGE_BIPOLAR ||
	    info->vbus_mode == PAC1711_HALF_RANGE_BIPOLAR)
		ref = ref >> 1;

	tmp = div_u64(ref * 1000000000UL, info->shunt);
	rem = do_div(tmp, 1000000000UL);

	return sysfs_emit(buf, "%lld.%09u\n", tmp, rem);
}

static ssize_t pac1711_in_enable_acc_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct pac1711_chip_info *info = iio_priv(indio_dev);

	return sysfs_emit(buf, "%d\n", info->enable_acc);
}

static ssize_t pac1711_in_enable_acc_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	int val;

	if (kstrtouint(buf, 10, &val)) {
		dev_err(dev, "Value is not valid\n");
		return -EINVAL;
	}

	scoped_guard(mutex, &info->lock) {
		info->enable_acc = val ? true : false;
		if (!val) {
			info->chip_reg_data.acc_val = 0;
			info->chip_reg_data.total_samples_nr = 0;
		}
	}

	return count;
}

static ssize_t pac1711_in_coulomb_counter_raw_show(struct device *dev,
						   struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	int ret;

	ret = pac1711_retrieve_data(info, PAC1711_MIN_UPDATE_WAIT_TIME_US);
	if (ret)
		return 0;

	return sysfs_emit(buf, "%lld\n", info->chip_reg_data.acc_val);
}

static ssize_t pac1711_in_coulomb_counter_scale_show(struct device *dev,
						     struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	u64 tmp_u64, ref;
	unsigned int rem;

	if (info->is_pac18x1_family)
		/*
		 * Calculate the scale for accumulated current/Coulomb counter
		 * (100mV * 1000000) / (2^16 * shunt(uOhm)) - depends on the channel's shunt value
		 */
		ref = (u64)1525878906250ULL;
	else
		/* (100mV * 1000000) / (2^12 * shunt(uOhm)) */
		ref = (u64)24414062500000ULL;

	if (info->vsense_mode == PAC1711_FULL_RANGE_BIPOLAR)
		ref = ref << 1;

	/*
	 * Increasing precision
	 * (100mV * 1000000 * 1000000000) / 2^(12 or 16))
	 */
	tmp_u64 = div_u64(ref, info->shunt);
	rem = do_div(tmp_u64, 1000000000UL);

	return sysfs_emit(buf, "%lld.%09u\n", tmp_u64, rem);
}

static IIO_DEVICE_ATTR(in_energy_raw, 0444,
		       pac1711_in_power_acc_raw_show, NULL, 0);

static IIO_DEVICE_ATTR(in_energy_scale, 0444,
		       pac1711_in_power_acc_scale_show, NULL, 0);

static IIO_DEVICE_ATTR(in_energy_en, 0644,
		       pac1711_in_enable_acc_show, pac1711_in_enable_acc_store, 0);

static IIO_DEVICE_ATTR(in_coulomb_counter_raw, 0444,
		       pac1711_in_coulomb_counter_raw_show, NULL, 0);

static IIO_DEVICE_ATTR(in_coulomb_counter_scale, 0444,
		       pac1711_in_coulomb_counter_scale_show, NULL, 0);

static IIO_DEVICE_ATTR(in_coulomb_counter_en, 0644,
		       pac1711_in_enable_acc_show, pac1711_in_enable_acc_store, 0);

static struct attribute *pac1711_power_acc_attr[] = {
	PAC1711_DEV_ATTR(in_energy_raw),
	PAC1711_DEV_ATTR(in_energy_scale),
	PAC1711_DEV_ATTR(in_energy_en),
	NULL
};

static struct attribute *pac1711_coulomb_counter_attr[] = {
	PAC1711_DEV_ATTR(in_coulomb_counter_raw),
	PAC1711_DEV_ATTR(in_coulomb_counter_scale),
	PAC1711_DEV_ATTR(in_coulomb_counter_en),
	NULL
};

static ssize_t pac1711_write_shunt_resistor(struct iio_dev *indio_dev, uintptr_t private,
					    const struct iio_chan_spec *ch, const char *buf,
					    size_t len)
{
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	struct device *dev = &info->client->dev;
	int sh_val;

	if (kstrtouint(buf, 10, &sh_val)) {
		dev_err(dev, "Shunt value is not valid\n");
		return -EINVAL;
	}

	scoped_guard(mutex, &info->lock)
		info->shunt = sh_val;

	return len;
}

static const struct iio_chan_spec_ext_info pac1711_ext_info[] = {
	{
		.name = "in_shunt_resistor",
		.read = pac1711_read_shunt_resistor,
		.write = pac1711_write_shunt_resistor,
		.shared = IIO_SHARED_BY_ALL,
	},
	{ }
};

#define TO_PAC1711_CHIP_INFO(d) container_of(d, struct pac1711_chip_info, work_chip_rfsh)

#define PAC1711_VBUS_CHANNEL(_index, _address) {				\
	.type = IIO_VOLTAGE,							\
	.address = (_address),							\
	.indexed = 1,								\
	.channel = (_index),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |				\
			      BIT(IIO_CHAN_INFO_SCALE),				\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
}

#define PAC1711_VSENSE_CHANNEL(_index, _address) {				\
	.type = IIO_CURRENT,							\
	.address = (_address),							\
	.indexed = 1,								\
	.channel = (_index),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |				\
			      BIT(IIO_CHAN_INFO_SCALE),				\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.ext_info = pac1711_ext_info,						\
}

#define PAC1711_VPOWER_CHANNEL(_index, _address) {				\
	.type = IIO_POWER,							\
	.address = (_address),							\
	.indexed = 1,								\
	.channel = (_index),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |				\
			      BIT(IIO_CHAN_INFO_SCALE),				\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
}

static int pac1711_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	u64 tmp = 0;
	int ret;

	ret = pac1711_retrieve_data(info, PAC1711_MIN_UPDATE_WAIT_TIME_US);
	if (ret)
		return ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_VOLTAGE:
			*val = info->chip_reg_data.vbus;
			return IIO_VAL_INT;
		case IIO_CURRENT:
			*val = info->chip_reg_data.vsense;
			return IIO_VAL_INT;
		case IIO_POWER:
			*val = (u32)info->chip_reg_data.vpower;
			*val2 = (u32)(info->chip_reg_data.vpower >> 32);
			return IIO_VAL_INT_64;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->address) {
		case PAC1711_VBUS_REG_ADDR:
			/* Voltages - scale for millivolts */
			switch (info->chip_variant) {
			case PAC_PRODUCT_ID_1711:
			case PAC_PRODUCT_ID_1811:
				*val = PAC1711_VOLTAGE_MILLIVOLTS_MAX;
				break;
			case PAC_PRODUCT_ID_1721:
			case PAC_PRODUCT_ID_1821:
				*val = PAC1721_VOLTAGE_MILLIVOLTS_MAX;
				break;
			default:
				return -EINVAL;
			}

			*val2 = (info->vbus_mode == PAC1711_FULL_RANGE_BIPOLAR) ? 15 : 16;

			return IIO_VAL_FRACTIONAL_LOG2;
		case PAC1711_VSENSE_REG_ADDR:
			/*
			 * Currents - scale for mA - depends on the channel's shunt value
			 * (100mV * 1000000) / (2^16 * shunt(uohm))
			 */
			*val = 1526;
			*val2 = info->shunt;

			if (info->vsense_mode == PAC1711_FULL_RANGE_BIPOLAR)
				*val = *val << 1;

			return IIO_VAL_FRACTIONAL;
		case PAC1711_VPOWER_REG_ADDR:
			/*
			 * Power - uW - it will use the combined scale
			 * for current and voltage
			 * current(mA) * voltage(mV) = power (uW)
			 */
			switch (info->chip_variant) {
			case PAC_PRODUCT_ID_1711:
			case PAC_PRODUCT_ID_1811:
				tmp = PAC1711_PRODUCT_VOLTAGE_PV_FSR;
				break;
			case PAC_PRODUCT_ID_1721:
			case PAC_PRODUCT_ID_1821:
				tmp = PAC1721_PRODUCT_VOLTAGE_PV_FSR;
				break;
			default:
				return -EINVAL;
			}

			do_div(tmp, info->shunt);
			*val = (int)tmp;

			if ((info->vsense_mode == PAC1711_FULL_RANGE_UNIPOLAR &&
			     info->vbus_mode == PAC1711_FULL_RANGE_UNIPOLAR)  ||
			    info->vsense_mode == PAC1711_HALF_RANGE_BIPOLAR ||
			    info->vbus_mode == PAC1711_HALF_RANGE_BIPOLAR)
				*val2 = 32;
			else
				*val2 = 31;

			return IIO_VAL_FRACTIONAL_LOG2;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = pac1711_samp_rate_map_tbl[info->sample_rate_idx];
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int pac1711_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct pac1711_chip_info *info = iio_priv(indio_dev);
	struct i2c_client *client = info->client;
	struct device *dev = &info->client->dev;
	s32 old_samp_rate;
	int new_idx, ret;
	__be16 tmp_be16;
	u16 tmp_u16;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		old_samp_rate = pac1711_samp_rate_map_tbl[info->sample_rate_idx];
		new_idx = pac1711_get_samp_rate_idx(info, val);
		if (new_idx < 0)
			return new_idx;

		ret = i2c_smbus_read_i2c_block_data(client, PAC1711_CTRL_ACT_REG_ADDR,
						    sizeof(tmp_u16), (u8 *)&tmp_be16);
		if (ret < 0) {
			dev_err(&client->dev, "cannot read regs from 0x%02X\n",
				PAC1711_CTRL_ACT_REG_ADDR);
			return ret;
		}

		tmp_u16 = be16_to_cpu(tmp_be16);
		tmp_u16 &= ~PAC1711_CTRL_SAMPLE_MODE_MASK;
		tmp_u16 |= FIELD_PREP(PAC1711_CTRL_SAMPLE_MODE_MASK, new_idx);
		tmp_be16 = cpu_to_be16(tmp_u16);

		scoped_guard(mutex, &info->lock) {
			ret = i2c_smbus_write_word_data(client, PAC1711_CTRL_REG_ADDR, tmp_be16);
			if (ret < 0) {
				dev_err(&client->dev, "Failed to configure sampling mode\n");
				return ret;
			}

			info->sample_rate_idx = new_idx;
			info->chip_reg_data.ctrl_act_reg = tmp_u16;
		}

		/* Force register snapshot and timestamp update with a refresh. */
		info->chip_reg_data.jiffies_tstamp -= msecs_to_jiffies(PAC1711_MIN_POLLING_TIME_MS);
		ret = pac1711_retrieve_data(info, (1024 / old_samp_rate) * 1000);
		if (ret) {
			dev_err(dev, "%s - cannot snapshot ctrl and measurement regs\n", __func__);
			return ret;
		}

		return 0;
	default:
		return -EINVAL;
	}
}

static void pac1711_work_periodic_rfsh(struct work_struct *work)
{
	struct pac1711_chip_info *info = TO_PAC1711_CHIP_INFO((struct delayed_work *)work);
	struct device *dev = &info->client->dev;

	dev_dbg(dev, "%s - Periodic refresh\n", __func__);

	/* Do a REFRESH, then read */
	pac1711_reg_snapshot(info, true, PAC1711_REFRESH_REG_ADDR,
			     PAC1711_MIN_UPDATE_WAIT_TIME_US);

	schedule_delayed_work(&info->work_chip_rfsh,
			      msecs_to_jiffies(PAC1711_MAX_RFSH_LIMIT_MS));
}

static int pac1711_chip_identify(struct iio_dev *indio_dev, struct pac1711_chip_info *info)
{
	struct i2c_client *client = info->client;
	struct device *dev = &client->dev;
	u8 chip_rev_info[3];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(client, PAC1711_PID_REG_ADDR,
					    sizeof(chip_rev_info), chip_rev_info);
	if (ret < 0)
		dev_info(&client->dev, "cannot read product ID reg\n");

	info->chip_variant = chip_rev_info[0];
	indio_dev->name = pac1711_chip_features.name;
	switch (info->chip_variant) {
	case PAC_PRODUCT_ID_1711:
	case PAC_PRODUCT_ID_1721:
		info->is_pac18x1_family = false;
		return 0;
	case PAC_PRODUCT_ID_1811:
	case PAC_PRODUCT_ID_1821:
		info->is_pac18x1_family = true;
		return 0;
	default:
		dev_info(dev, "product ID (0x%02X, 0x%02X, 0x%02X) not recognized\n",
			 chip_rev_info[0], chip_rev_info[1], chip_rev_info[2]);
		return 0;
	}
}

static int pac1711_check_range(s32 *vals, bool is_vbus)
{
	int num_ranges = ARRAY_SIZE(pac1711_vbus_range_tbl);
	const int (*ranges)[3][2];
	int i;

	ranges = is_vbus ? &pac1711_vbus_range_tbl : &pac1711_vsense_range_tbl;

	for (i = 0; i < num_ranges; i++) {
		if (vals[0] == (*ranges)[i][0] && vals[1] == (*ranges)[i][1])
			return i;
	}

	return -EINVAL;
}

static int pac1711_init_vbus_vsense_ranges(struct pac1711_chip_info *info, bool is_vbus)
{
	struct i2c_client *client = info->client;
	struct device *dev = &client->dev;
	const char *prop_name;
	s32 vals[2];
	int ret;

	prop_name = is_vbus ? PAC1711_VBUS_PROP : PAC1711_VSENSE_PROP;
	ret = device_property_read_u32_array(dev, prop_name, vals, 2);
	if (ret) {
		dev_dbg(dev, "%s property error %X\n", prop_name, ret);
		/* Set default range to PAC1711_FULL_RANGE_UNIPOLAR */
		ret = PAC1711_FULL_RANGE_UNIPOLAR;
	} else {
		ret = pac1711_check_range(vals, is_vbus);
		if (ret < 0)
			return dev_err_probe(dev, -EINVAL, "Invalid value %d, %d for prop %s\n",
					     vals[0], vals[1], prop_name);
	}

	if (is_vbus)
		info->vbus_mode = ret;
	else
		info->vsense_mode = ret;

	return 0;
}

static bool pac1711_parse_fw(struct i2c_client *client, struct pac1711_chip_info *info)
{
	struct device *dev = &client->dev;
	const char *temp;
	int ret = 0;
	int tmp;

	ret = device_property_read_u32(dev, "shunt-resistor-micro-ohms", &info->shunt);
	if (ret)
		return dev_err_probe(dev, ret, "Shunt-resistor property error\n");

	ret = pac1711_init_vbus_vsense_ranges(info, true);
	if (ret)
		return ret;

	ret = pac1711_init_vbus_vsense_ranges(info, false);
	if (ret)
		return ret;

	ret = device_property_read_string(dev, "microchip,accumulation-mode", &temp);
	if (ret)
		return dev_err_probe(dev, ret, "microchip,accumulation-mode property error\n");

	if (!strcmp(temp, PAC1711_ACC_VPOWER))
		tmp = PAC1711_ACCMODE_VPOWER;
	else if (!strcmp(temp, PAC1711_ACC_VSENSE))
		tmp = PAC1711_ACCMODE_VSENSE;
	else
		return dev_err_probe(dev, -EINVAL, "invalid accumulation-mode value %s\n", temp);

	dev_dbg(dev, "Accumulation mode set to: %s\n", temp);
	info->accumulation_mode = tmp;

	return 0;
}

static void pac1711_cancel_delayed_work(void *dwork)
{
	cancel_delayed_work_sync(dwork);
}

static int pac1711_chip_configure(struct pac1711_chip_info *info)
{
	struct i2c_client *client = info->client;
	struct device *dev = &client->dev;
	__be16 tmp_be16;
	u32 wait_time;
	u16 tmp_u16;
	int ret = 0;
	u8 tmp_u8;

	/* Get sampling rate from PAC */
	ret = i2c_smbus_read_i2c_block_data(client, PAC1711_CTRL_REG_ADDR,
					    sizeof(tmp_u16), (u8 *)&tmp_be16);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot read 0x%02X reg\n", PAC1711_CTRL_REG_ADDR);

	be16_to_cpus(&tmp_be16);
	info->sample_rate_idx = FIELD_GET(PAC1711_CTRL_SAMPLE_MODE_MASK, tmp_be16);

	/*
	 * The current/voltage can be measured unidirectional, bidirectional or half FSR
	 * no SLOW triggered REFRESH, clear POR
	 */
	tmp_u8 = FIELD_PREP(PAC1711_NEG_PWR_FSR_VS_MASK, info->vsense_mode) |
		FIELD_PREP(PAC1711_NEG_PWR_FSR_VB_MASK, info->vbus_mode);

	ret = i2c_smbus_write_byte_data(client, PAC1711_NEG_PWR_FSR_REG_ADDR, tmp_u8);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot write 0x%02X reg\n",
				     PAC1711_NEG_PWR_FSR_REG_ADDR);

	ret = i2c_smbus_write_word_data(client, PAC1711_SLOW_REG_ADDR, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot write 0x%02X reg\n", PAC1711_SLOW_REG_ADDR);

	ret = i2c_smbus_write_word_data(client, PAC1711_CTRL_REG_ADDR, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot write 0x%02X reg\n", PAC1711_CTRL_REG_ADDR);

	/*
	 * Sending a REFRESH to the chip, so the new settings take place
	 * as well as resetting the accumulators
	 */
	ret = i2c_smbus_write_byte(client, PAC1711_REFRESH_REG_ADDR);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot write 0x%02X reg\n",
				     PAC1711_REFRESH_REG_ADDR);

	fsleep(125);

	/*
	 * Get the current (in the chip) sampling speed and compute the
	 * required timeout based on its value the timeout is 1/sampling_speed
	 * wait the maximum amount of time to be on the safe side - the
	 * maximum wait time is for 8sps
	 */
	wait_time = (1024 / pac1711_samp_rate_map_tbl[info->sample_rate_idx]) * 1000;
	fsleep(wait_time);

	INIT_DELAYED_WORK(&info->work_chip_rfsh, pac1711_work_periodic_rfsh);
	/* Setup the latest moment for reading the regs before saturation */
	schedule_delayed_work(&info->work_chip_rfsh,
			      msecs_to_jiffies(PAC1711_MAX_RFSH_LIMIT_MS));

	return devm_add_action_or_reset(&client->dev, pac1711_cancel_delayed_work,
					&info->work_chip_rfsh);
}

static struct iio_chan_spec pac1711_chan_spec[] = {
	PAC1711_VPOWER_CHANNEL(0, PAC1711_VPOWER_REG_ADDR),
	PAC1711_VBUS_CHANNEL(0, PAC1711_VBUS_REG_ADDR),
	PAC1711_VSENSE_CHANNEL(0, PAC1711_VSENSE_REG_ADDR),
};

static int pac1711_prep_custom_attributes(struct pac1711_chip_info *info,
					  struct iio_dev *indio_dev)
{
	struct attribute **pac1711_custom_attrs, **tmp_attr;
	struct device *dev = &info->client->dev;
	struct attribute_group *pac1711_group;
	int custom_attr_cnt, j;

	pac1711_group = devm_kzalloc(dev, sizeof(*pac1711_group), GFP_KERNEL);
	if (!pac1711_group)
		return -ENOMEM;

	custom_attr_cnt = PAC1711_COMMON_DEVATTR + ARRAY_SIZE(pac1711_power_acc_attr);
	pac1711_custom_attrs = devm_kzalloc(dev, custom_attr_cnt * sizeof(*pac1711_group) + 1,
					    GFP_KERNEL);

	if (!pac1711_custom_attrs)
		return -ENOMEM;

	j = 0;
	switch (info->accumulation_mode) {
	case PAC1711_ACCMODE_VPOWER:
		tmp_attr = pac1711_power_acc_attr;
		break;
	case PAC1711_ACCMODE_VSENSE:
		tmp_attr = pac1711_coulomb_counter_attr;
		break;
	default:
		return -EINVAL;
	}

	pac1711_custom_attrs[j++] = tmp_attr[0];
	pac1711_custom_attrs[j++] = tmp_attr[1];
	pac1711_custom_attrs[j++] = tmp_attr[2];

	pac1711_group->attrs = pac1711_custom_attrs;
	info->iio_info.attrs = pac1711_group;

	return 0;
}

static int pac1711_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *channel,
			      const int **vals, int *type, int *length, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT;
		*vals = pac1711_samp_rate_map_tbl;
		*length = ARRAY_SIZE(pac1711_samp_rate_map_tbl);
		return IIO_AVAIL_LIST;
	}

	return -EINVAL;
}

static const struct iio_info pac1711_info = {
	.read_raw = pac1711_read_raw,
	.write_raw = pac1711_write_raw,
	.read_avail = pac1711_read_avail,
};

static int pac1711_probe(struct i2c_client *client)
{
	const struct pac1711_features *chip;
	struct device *dev = &client->dev;
	struct pac1711_chip_info *info;
	struct iio_dev *indio_dev;
	int ret, err;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*info));
	if (!indio_dev)
		return -ENOMEM;

	info = iio_priv(indio_dev);
	info->client = client;

	ret = pac1711_chip_identify(indio_dev, info);
	if (ret) {
		/*
		 * If it fail to identify the hardware based on internal
		 * registers, use compatible from devicetree.
		 */
		chip = i2c_get_match_data(client);
		if (!chip)
			return -EINVAL;

		info->chip_variant = chip->prod_id;
		indio_dev->name = chip->name;
	}

	/* Always start with accumulation channels enabled. */
	info->enable_acc = true;

	err = pac1711_parse_fw(client, info);
	if (err)
		return dev_err_probe(dev, err, "Error parsing devicetree data\n");

	ret = devm_mutex_init(dev, &info->lock);
	if (ret)
		return ret;

	ret = pac1711_chip_configure(info);
	if (ret)
		return ret;

	indio_dev->num_channels = ARRAY_SIZE(pac1711_chan_spec);
	indio_dev->channels = pac1711_chan_spec;
	info->iio_info = pac1711_info;
	indio_dev->info = &info->iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = pac1711_prep_custom_attributes(info, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Can't configure custom attributes\n");

	/* Read what has been accumulated in the chip so far and reset the accumulators. */
	ret = pac1711_reg_snapshot(info, true, PAC1711_REFRESH_REG_ADDR,
				   PAC1711_MIN_UPDATE_WAIT_TIME_US);
	if (ret)
		return ret;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Can't register IIO device\n");

	return 0;
}

static const struct i2c_device_id pac1711_id[] = {
	{ .name = "pac1711", .driver_data = (kernel_ulong_t)&pac1711_chip_features },
	{ .name = "pac1721", .driver_data = (kernel_ulong_t)&pac1721_chip_features },
	{ .name = "pac1811", .driver_data = (kernel_ulong_t)&pac1811_chip_features },
	{ .name = "pac1821", .driver_data = (kernel_ulong_t)&pac1821_chip_features },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pac1711_id);

static const struct of_device_id pac1711_of_match[] = {
	{
		.compatible = "microchip,pac1711",
		.data = &pac1711_chip_features
	},
	{
		.compatible = "microchip,pac1721",
		.data = &pac1721_chip_features
	},
	{
		.compatible = "microchip,pac1811",
		.data = &pac1811_chip_features
	},
	{
		.compatible = "microchip,pac1821",
		.data = &pac1821_chip_features
	},
	{ }
};
MODULE_DEVICE_TABLE(of, pac1711_of_match);

static struct i2c_driver pac1711_driver = {
	.driver	 = {
		.name = "pac1711",
		.of_match_table = pac1711_of_match,
	},
	.probe = pac1711_probe,
	.id_table = pac1711_id,
};

module_i2c_driver(pac1711_driver);

MODULE_AUTHOR("Ariana Lazar <ariana.lazar@microchip.com>");
MODULE_DESCRIPTION("IIO driver for PAC1711 DC Power Monitor with Accumulator");
MODULE_LICENSE("GPL");
