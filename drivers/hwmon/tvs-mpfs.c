// SPDX-License-Identifier: GPL-2.0+
/*
 *
 * Author: Lars Randers <lranders@mail.dk>
 *
 */

#include <linux/err.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define PFSOC_CONTROL_SCB_TVS_CONTROL 0x08
#define PFSOC_CONTROL_SCB_TVS_OUTPUT0 0x24
#define PFSOC_CONTROL_SCB_TVS_OUTPUT1 0x28

#define CTRL_POWEROFF BIT(5)
#define CTRL_ABORT    BIT(4)
#define CTRL_TEMP     BIT(3)
#define CTRL_2P5      BIT(2)
#define CTRL_1P8      BIT(1)
#define CTRL_1P05     BIT(0)

#define OUTPUT0_U1P8_MASK GENMASK(30, 16)
#define OUTPUT0_U1P8_OFF  16
#define OUTPUT0_U1P0_MASK GENMASK(14, 0)
#define OUTPUT0_U1P0_OFF  0
#define OUTPUT1_TEMP_MASK GENMASK(31, 16)
#define OUTPUT1_TEMP_OFF  16
#define OUTPUT1_U2P5_MASK GENMASK(14, 0)
#define OUTPUT1_U2P5_OFF  0

#define MPFS_TVS_MIN_POLL_INTERVAL_IN_MILLIS 2000

/* The following constant is 273.5 in (16.4) fixedpoint notation */
#define MPFS_TVS_MIN_TEMP_IN_K 0x1112

typedef struct {
	long min;
	long actual;
	long max;
} mpfs_tvs_sensor_t;

typedef enum {
	SN_V1P05 = 0,
	SN_V1P8,
	SN_V2P5,
	SN_TEMP,

	SN_MAX
} mpfs_tvs_sn_t;

static const char *mpfs_tvs_voltage_labels[] = { "U1P05", "U1P8", "U2P5" };

struct mpfs_tvs {
	struct device *dev;
	struct device *hwmon_dev;
	struct task_struct *poll_task;
	struct regmap *regmap;
	bool kthread_running;
	long update_interval;	/* in milli-seconds */
	mpfs_tvs_sensor_t sensors[SN_MAX];
};

static int mpfs_tvs_update_sensors(struct mpfs_tvs *data) {
	u32 temp;
	u32 work;
	int ret;

	ret = regmap_read(data->regmap, PFSOC_CONTROL_SCB_TVS_OUTPUT1, &temp);
	if (ret)
		return ret;

	work = temp;

	temp = (temp & OUTPUT1_TEMP_MASK) >> OUTPUT1_TEMP_OFF;
	temp = clamp_val(temp, MPFS_TVS_MIN_TEMP_IN_K, INT_MAX);
	temp = temp - MPFS_TVS_MIN_TEMP_IN_K; /* Kelvin to Celsius */
	temp = (1000 * temp) >> 4; /* fixed point (10.4) to millicentigrade */
	data->sensors[SN_TEMP].actual = temp;
	data->sensors[SN_TEMP].max =
		max(data->sensors[SN_TEMP].actual, data->sensors[SN_TEMP].max);
	data->sensors[SN_TEMP].min =
		min(data->sensors[SN_TEMP].min, data->sensors[SN_TEMP].actual);

	work &= OUTPUT1_U2P5_MASK;
	/* fixed point (11.3) adjust; value is already millivolts */
	work  = (1 * work) >> 3;
	data->sensors[SN_V2P5].actual = work;
	data->sensors[SN_V2P5].max =
		max(data->sensors[SN_V2P5].actual, data->sensors[SN_V2P5].max);
	data->sensors[SN_V2P5].min =
		min(data->sensors[SN_V2P5].min, data->sensors[SN_V2P5].actual);

	ret = regmap_read(data->regmap, PFSOC_CONTROL_SCB_TVS_OUTPUT0, &temp);
	if (ret)
		return ret;

	work = temp;
	temp = (OUTPUT0_U1P8_MASK & temp) >> OUTPUT0_U1P8_OFF;
	/* fixed point (11.3) adjust; value is already millivolts */
	temp = (1 * temp) >> 3;
	data->sensors[SN_V1P8].actual = temp;
	data->sensors[SN_V1P8].max =
		max(data->sensors[SN_V1P8].actual, data->sensors[SN_V1P8].max);
	data->sensors[SN_V2P5].min =
		min(data->sensors[SN_V1P8].min, data->sensors[SN_V1P8].actual);

	work &= OUTPUT0_U1P0_MASK;
	 /* fixed point (11.3) adjust; value is already millivolts */
	work  = (1 * work) >> 3;
	data->sensors[SN_V1P05].actual = work;
	data->sensors[SN_V1P05].max =
		max(data->sensors[SN_V1P05].actual, data->sensors[SN_V1P05].max);
	data->sensors[SN_V1P05].min =
		min(data->sensors[SN_V1P05].min, data->sensors[SN_V1P05].actual);

	return 0;
}


static int mpfs_tvs_chip_read(struct mpfs_tvs *data, long *val)
{
	*val = data->update_interval;
	return 0;
}

static int mpfs_tvs_temp_read(struct mpfs_tvs *data, u32 attr,
			      int channel, long *val)
{
	switch(attr) {
	case hwmon_temp_input:
		*val = data->sensors[SN_TEMP].actual;
		break;

	case hwmon_temp_max:
		*val = data->sensors[SN_TEMP].max;
		break;

	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int mpfs_tvs_voltage_read(struct mpfs_tvs *data, u32 attr,
				 int channel, long *val)
{
	dev_dbg(data->dev, "read voltage chan %d\n", channel);
	switch(attr) {
	case hwmon_in_input:
		*val = data->sensors[channel].actual;
		break;

	case hwmon_in_max:
		*val = data->sensors[channel].max;
		break;

	default:
		return -EOPNOTSUPP;
	}
	return 0;
}


static ssize_t mpfs_tvs_interval_write(struct mpfs_tvs *data, long val)
{
	data->update_interval =
		clamp_val(val, MPFS_TVS_MIN_POLL_INTERVAL_IN_MILLIS, INT_MAX);
	return 0;
}


static umode_t mpfs_tvs_is_visible(const void *data,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	if(type == hwmon_chip && attr == hwmon_chip_update_interval)
		return 0644;

	if(type == hwmon_temp) {
		switch(attr) {
		case hwmon_temp_input:
		case hwmon_temp_max:
		case hwmon_temp_label:
			return 0444;

		default:
			return 0;
		}
	} else if(type == hwmon_in) {
		switch(attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			return 0444;

		default:
			return 0;
		}
	}
	return 0;
}

static int mpfs_tvs_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct mpfs_tvs *data = dev_get_drvdata(dev);

	switch(type) {
	case hwmon_temp:
		return mpfs_tvs_temp_read(data, attr, channel, val);
	case hwmon_in:
		return mpfs_tvs_voltage_read(data, attr, channel, val);
	case hwmon_chip:
		return mpfs_tvs_chip_read(data, val);

	default:
		return -EOPNOTSUPP;
	}
}

static int mpfs_tvs_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	struct mpfs_tvs *data = dev_get_drvdata(dev);

	switch(type) {
	case hwmon_chip:
		return mpfs_tvs_interval_write(data, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int mpfs_tvs_read_labels(struct device *dev,
				enum hwmon_sensor_types type,
				u32 attr, int channel,
				const char **str)
{
	switch(type) {
	case hwmon_temp:
		*str = "CPU Temp";
		return 0;
	case hwmon_in:
		*str = mpfs_tvs_voltage_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}


static const struct hwmon_ops mpfs_tvs_ops = {
	.is_visible = mpfs_tvs_is_visible,
	.read_string = mpfs_tvs_read_labels,
	.read = mpfs_tvs_read,
	.write = mpfs_tvs_write,
};

static const struct hwmon_channel_info *mpfs_tvs_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ | HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MIN |
			   HWMON_T_MAX | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	NULL
};

static const struct hwmon_chip_info mpfs_tvs_chip_info = {
	.ops = &mpfs_tvs_ops,
	.info = mpfs_tvs_info,
};


static int mpfs_tvs_poll_task(void *ptr)
{
	struct mpfs_tvs *data = ptr;
	int ret = 0;

	data->kthread_running = true;

	set_freezable();

	while(!kthread_should_stop()) {
		schedule_timeout_interruptible(data->update_interval);
		try_to_freeze();
		ret = mpfs_tvs_update_sensors(data);
		if(ret)
			break;
	}

	data->kthread_running = false;
	return ret;
}

static int mpfs_tvs_probe(struct platform_device *pdev)
{
	struct device *hwmon_dev;
	struct mpfs_tvs *data;
	struct task_struct *task;
	int err;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if(!data)
		return -ENOMEM;

	data->dev = &pdev->dev;

	data->regmap = device_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(data->regmap))
		dev_err_probe(data->dev, PTR_ERR(data->regmap), "Failed to find syscon regmap\n");

	data->kthread_running = false;

	hwmon_dev = devm_hwmon_device_register_with_info(data->dev, "mpfs_tvs",
							 data,
							 &mpfs_tvs_chip_info,
							 NULL);

	if(IS_ERR(hwmon_dev)) {
		err = PTR_ERR(hwmon_dev);
		dev_err(data->dev, "Class registration failed (%d)\n", err);
		return err;
	}

	/* enable HW sensor */
	err = regmap_write(data->regmap, PFSOC_CONTROL_SCB_TVS_CONTROL,
			   CTRL_1P05 | CTRL_1P8 | CTRL_2P5 | CTRL_TEMP);

	data->hwmon_dev = hwmon_dev;
	data->sensors[SN_TEMP].max = 0;
	data->sensors[SN_V1P05].min =
		data->sensors[SN_V1P8].min =
		data->sensors[SN_V2P5].min = 20000;
	data->sensors[SN_V1P05].max =
		data->sensors[SN_V1P8].max =
		data->sensors[SN_V2P5].max = 0;
	data->update_interval = MPFS_TVS_MIN_POLL_INTERVAL_IN_MILLIS;
	mpfs_tvs_update_sensors(data);

	task = kthread_run(mpfs_tvs_poll_task, data, "tvs-mpfs-kthread");
	if (IS_ERR(task)) {
		err = PTR_ERR(task);
		dev_err(data->dev, "Unable to run kthread err %d\n", err);
		return err;
	}

	data->poll_task = task;

	return 0;
}

static struct platform_driver mpfs_tvs_driver = {
	.probe		= mpfs_tvs_probe,
	.driver = {
		.name = "mpfs-tvs",
	},
};
module_platform_driver(mpfs_tvs_driver);

MODULE_AUTHOR("Lars Randers <lranders@mail.dk>");
MODULE_DESCRIPTION("PolarFire SoC temperature & voltage sensor driver");
MODULE_LICENSE("GPL");
