/*
 * ov7740 Camera Driver
 *
 * Copyright (C) 2013 Josh Wu <josh.wu@atmel.com>
 *
 * Based on ov2640, ov772x, ov9640 drivers and previous non merged implementations.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/v4l2-mediabus.h>
#include <linux/videodev2.h>

#include <media/soc_camera.h>
#include <media/v4l2-clk.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include <linux/ov7740.h> /* OV7740 specific IOCTL's */

#define VAL_SET(x, mask, rshift, lshift)  \
		((((x) >> rshift) & mask) << lshift)

#define REG0C       0x0C /* Register 0C */
#define   REG0C_HFLIP_IMG       0x40 /* Horizontal mirror image ON/OFF */
#define   REG0C_VFLIP_IMG       0x80 /* Vertical flip image ON/OFF */

/*
 * ID
 */
#define PID         0x0A /* Product ID Number MSB */
#define VER         0x0B /* Product ID Number LSB */
#define MIDH        0x1C /* Manufacturer ID byte - high */
#define MIDL        0x1D /* Manufacturer ID byte - low  */

#define PID_OV7740	0x7742
#define VERSION(pid, ver) ((pid << 8) | (ver & 0xFF))

/*
 * Struct
 */
struct regval_list {
	u8 reg_num;
	u8 value;
};

/* Supported resolutions */
enum ov7740_width {
	W_VGA	= 640,
        W_QVGA  = 320,
};

enum ov7740_height {
	H_VGA	= 480,
	H_QVGA	= 240,
};

struct ov7740_win_size {
	char				*name;
	enum ov7740_width		width;
	enum ov7740_height		height;
	const struct regval_list	*regs;
        // It seems there are some registers that will give a NAK error when written to.
        // The ignore_errors flags tells the driver to ignore this issue.
        int                             ignore_errors;
};


struct ov7740_priv {
	struct v4l2_subdev		subdev;
	struct v4l2_ctrl_handler	hdl;
	u32				cfmt_code;
	struct v4l2_clk			*clk;
	const struct ov7740_win_size	*win;

	struct soc_camera_subdev_desc	ssdd_dt;
	struct gpio_desc *resetb_gpio;
	struct gpio_desc *pwdn_gpio;
};

/*
 * Registers settings
 */

#define ENDMARKER { 0xff, 0xff }

static struct regval_list ov7740_vga_regs[VIDIOC_OV7740_MAX_REGS + 1] = {
	{0x55 ,0x40},
	{0x11 ,0x02},

	{0x12 ,0x00},
	{0xd5 ,0x10},
	{0x0c ,0x12},
	{0x0d ,0x34},
	{0x17 ,0x25},
	{0x18 ,0xa0},
	{0x19 ,0x03},
	{0x1a ,0xf0},
	{0x1b ,0x89},
	{0x22 ,0x03},
	{0x29 ,0x18},
	{0x2b ,0xf8},
	{0x2c ,0x01},
	{0x31 ,0xa0},
	{0x32 ,0xf0},
	{0x33 ,0xc4},
	{0x35 ,0x05},
	{0x36 ,0x3f},
	{0x04 ,0x60},
	{0x27 ,0x80},
	{0x3d ,0x0f},
	{0x3e ,0x80},
	{0x3f ,0x40},
	{0x40 ,0x7f},
	{0x41 ,0x6a},
	{0x42 ,0x29},
	{0x44 ,0x22},
	{0x45 ,0x41},
	{0x47 ,0x02},
	{0x49 ,0x64},
	{0x4a ,0xa1},
	{0x4b ,0x40},
	{0x4c ,0x1a},
	{0x4d ,0x50},
	{0x4e ,0x13},
	{0x64 ,0x00},
	{0x67 ,0x88},
	{0x68 ,0x1a},

	{0x14 ,0x28},
	{0x24 ,0x3c},
	{0x25 ,0x30},
	{0x26 ,0x72},
	{0x50 ,0x97},
	{0x51 ,0x1f},
	{0x52 ,0x00},
	{0x53 ,0x00},
	{0x20 ,0x00},
	{0x21 ,0xcf},
	{0x50, 0x4b},
	{0x38 ,0x14},
	{0xe9 ,0x00},
	{0x56 ,0x55},
	{0x57 ,0xff},
	{0x58 ,0xff},
	{0x59 ,0xff},
	{0x5f ,0x04},
	{0xec ,0x00},
	{0x13 ,0xff},

	{0x80 ,0x7f},
	{0x81 ,0x3f},
	{0x82 ,0x32},
	{0x83 ,0x01},
	{0x38 ,0x11},
	{0x84 ,0x70},
	{0x85 ,0x00},
	{0x86 ,0x03},
	{0x87 ,0x01},
	{0x88 ,0x05},
	{0x89 ,0x30},
	{0x8d ,0x30},
	{0x8f ,0x85},
	{0x93 ,0x30},
	{0x95 ,0x85},
	{0x99 ,0x30},
	{0x9b ,0x85},

	{0x9c ,0x08},
	{0x9d ,0x12},
	{0x9e ,0x23},
	{0x9f ,0x45},
	{0xa0 ,0x55},
	{0xa1 ,0x64},
	{0xa2 ,0x72},
	{0xa3 ,0x7f},
	{0xa4 ,0x8b},
	{0xa5 ,0x95},
	{0xa6 ,0xa7},
	{0xa7 ,0xb5},
	{0xa8 ,0xcb},
	{0xa9 ,0xdd},
	{0xaa ,0xec},
	{0xab ,0x1a},

	{0xce ,0x78},
	{0xcf ,0x6e},
	{0xd0 ,0x0a},
	{0xd1 ,0x0c},
	{0xd2 ,0x84},
	{0xd3 ,0x90},
	{0xd4 ,0x1e},

	{0x5a ,0x24},
	{0x5b ,0x1f},
	{0x5c ,0x88},
	{0x5d ,0x60},

	{0xac ,0x6e},
	{0xbe ,0xff},
	{0xbf ,0x00},

	{0x0f ,0x1d},
	{0x0f ,0x1f},
	ENDMARKER,
};

static struct regval_list ov7740_qvga_regs[VIDIOC_OV7740_MAX_REGS + 1] = {
	/* These are only used if a user program supplies QVGA register
         * settings for the OV7740.
         */
	ENDMARKER,
};

#define OV7740_SIZE(n, w, h, r) \
	{.name = n, .width = w , .height = h, .regs = r, .ignore_errors = 0 }

#define QVGA_INDEX 0 // Index 0 can be replaced by user supplied QVGA register values
static struct ov7740_win_size ov7740_supported_win_sizes[] = {
	OV7740_SIZE("VGA", W_VGA, H_VGA, ov7740_vga_regs),
	OV7740_SIZE("VGA", W_VGA, H_VGA, ov7740_vga_regs),
};

static u32 ov7740_codes[] = {
	MEDIA_BUS_FMT_YUYV8_2X8,
};

/*
 * General functions
 */
static struct ov7740_priv *to_ov7740(const struct i2c_client *client)
{
	return container_of(i2c_get_clientdata(client), struct ov7740_priv,
			    subdev);
}

static int ov7740_write_array(struct i2c_client *client,
			      const struct regval_list *vals,
                              int ignore_errors)
{
	int ret;

	while ((vals->reg_num != 0xff) || (vals->value != 0xff)) {
		ret = i2c_smbus_write_byte_data(client,
						vals->reg_num, vals->value);
		dev_vdbg(&client->dev, "array: 0x%02x, 0x%02x",
			 vals->reg_num, vals->value);

		if (!ignore_errors && ret < 0) {
			dev_err(&client->dev, "array: 0x%02x, 0x%02x write failed",
				vals->reg_num, vals->value);
			return ret;
		}
		vals++;
	}
	return 0;
}

static int ov7740_mask_set(struct i2c_client *client,
			   u8  reg, u8  mask, u8  set)
{
	s32 val = i2c_smbus_read_byte_data(client, reg);
	if (val < 0)
		return val;

	val &= ~mask;
	val |= set & mask;

	dev_vdbg(&client->dev, "masks: 0x%02x, 0x%02x", reg, val);

	return i2c_smbus_write_byte_data(client, reg, val);
}

static int ov7740_reset(struct i2c_client *client)
{
	int ret;
	const struct regval_list reset_seq[] = {
		{0x12 ,0x80},
		ENDMARKER,
	};

	ret = ov7740_write_array(client, reset_seq, 0);
	if (ret)
		goto err;

	msleep(5);
err:
	dev_dbg(&client->dev, "%s: (ret %d)", __func__, ret);
	return ret;
}

/*
 * soc_camera_ops functions
 */
static int ov7740_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static int ov7740_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd =
		&container_of(ctrl->handler, struct ov7740_priv, hdl)->subdev;
	struct i2c_client  *client = v4l2_get_subdevdata(sd);
	u8 val;

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		val = ctrl->val ? REG0C_VFLIP_IMG : 0x00;
		return ov7740_mask_set(client, REG0C, REG0C_VFLIP_IMG, val);
	case V4L2_CID_HFLIP:
		val = ctrl->val ? REG0C_HFLIP_IMG : 0x00;
		return ov7740_mask_set(client, REG0C, REG0C_HFLIP_IMG, val);
	}

	return -EINVAL;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int ov7740_g_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	reg->size = 1;
	if (reg->reg > 0xff)
		return -EINVAL;

	ret = i2c_smbus_read_byte_data(client, reg->reg);
	if (ret < 0)
		return ret;

	reg->val = ret;

	return 0;
}

static int ov7740_s_register(struct v4l2_subdev *sd,
			     const struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (reg->reg > 0xff ||
	    reg->val > 0xff)
		return -EINVAL;

	return i2c_smbus_write_byte_data(client, reg->reg, reg->val);
}
#endif

static int ov7740_s_power(struct v4l2_subdev *sd, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);
	struct ov7740_priv *priv = to_ov7740(client);

	return soc_camera_set_power(&client->dev, ssdd, priv->clk, on);
}

static long ov7740_set_regs(struct i2c_client *c,
                            struct v4l2_ov7740_register_group *g,
                            struct regval_list *r)
{
        int i;
        long ret = 0;
        if (g->numregs == VIDIOC_OV7740_AUTOSIZE_FLAG) {
                for (i = 0;
                     ((i < VIDIOC_OV7740_MAX_REGS) && 
                      !(g->regs[i].reg == 0xFF && g->regs[i].val == 0xFF));
                     i++) {
                        r[i].reg_num = g->regs[i].reg;
                        r[i].value = g->regs[i].val;
                }
        } else if (g->numregs >= 0 && g->numregs < VIDIOC_OV7740_MAX_REGS) {
                for (i = 0; i < g->numregs; i++) {
                        r[i].reg_num = g->regs[i].reg;
                        r[i].value = g->regs[i].val;
                }
        } else {
                ret = -EINVAL;
                goto done;
        }
        // ENDMARKER
        r[i].reg_num = 0xFF;
        r[i].value = 0xFF;
 done:
        return 0;
}

static long ov7740_get_regs(struct i2c_client *c,
                            struct v4l2_ov7740_register_group *g,
                            const struct regval_list *r)
{
        int count = 0;
        while (count < VIDIOC_OV7740_MAX_REGS &&
               !(r[count].reg_num == 0xFF && r[count].value == 0xFF)) {
                g->regs[count].reg = r[count].reg_num;
                g->regs[count].val = r[count].value;
                count++;
        }
        g->numregs = count;
        return 0;
}

static inline long ov7740_set_vga_regs(struct i2c_client *c,
                                       struct v4l2_ov7740_register_group *g)
{
        return ov7740_set_regs(c, g, ov7740_vga_regs);
}

static inline long ov7740_get_vga_regs(struct i2c_client *c,
                                       struct v4l2_ov7740_register_group *g)
{
        return ov7740_get_regs(c, g, ov7740_vga_regs);
}

static inline long ov7740_set_qvga_regs(struct i2c_client *c,
                                        struct v4l2_ov7740_register_group *g)
{
        long ret = ov7740_set_regs(c, g, ov7740_qvga_regs);
        if (ret < 0) 
                return ret;
        // We assume (and require) that the user supplied register values do, in fact,
        // configure the device for QVGA mode.
        ov7740_supported_win_sizes[QVGA_INDEX].name          = "QVGA";
        ov7740_supported_win_sizes[QVGA_INDEX].width         = W_QVGA;
        ov7740_supported_win_sizes[QVGA_INDEX].height        = H_QVGA;
        ov7740_supported_win_sizes[QVGA_INDEX].regs          = ov7740_qvga_regs;
        ov7740_supported_win_sizes[QVGA_INDEX].ignore_errors = g->ignore_errors;
        return 0;
}

static inline long ov7740_get_qvga_regs(struct i2c_client *c,
                                       struct v4l2_ov7740_register_group *g)
{
        return ov7740_get_regs(c, g, ov7740_qvga_regs);
}

static long ov7740_get_register_group(struct i2c_client *c,
                                      struct v4l2_ov7740_register_group *g)
{
	long ret;
        int i;

        for (i = 0; i < g->numregs; i++) {
                if (g->regs[i].reg > 0xff)
                        return -EINVAL;
                ret = i2c_smbus_read_byte_data(c, g->regs[i].reg);
                if (ret < 0)
                        return ret;
                g->regs[i].val = ret;
        }
        return 0;
}

static long ov7740_set_register_group(struct i2c_client *c,
                                      struct v4l2_ov7740_register_group *g)
{
	long ret;
        int i;

        for (i = 0; i < g->numregs; i++) {
                if (g->regs[i].reg > 0xff ||
                    g->regs[i].val > 0xff)
                        return -EINVAL;
                ret = i2c_smbus_write_byte_data(c, g->regs[i].reg, g->regs[i].val);
                if (ret < 0)
                        return ret;
                g->regs[i].val = ret;
        }
        return 0;
}

static long ov7740_ioctl(struct v4l2_subdev *sd,
                         unsigned int cmd,
                         void *arg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct v4l2_ov7740_register_group *g = arg;
	long ret;

        switch (cmd) {
        case VIDIOC_OV7740_S_VGA_REGS:
                ret = ov7740_set_vga_regs(client, g);
                break;

        case VIDIOC_OV7740_G_VGA_REGS:
                ret = ov7740_get_vga_regs(client, g);
                break;

        case VIDIOC_OV7740_S_QVGA_REGS:
                ret = ov7740_set_qvga_regs(client, g);
                break;

        case VIDIOC_OV7740_G_QVGA_REGS:
                ret = ov7740_get_qvga_regs(client, g);
                break;

        case VIDIOC_OV7740_G_REGISTER_GROUP:
                ret = ov7740_get_register_group(client, g);
                break;

        case VIDIOC_OV7740_S_REGISTER_GROUP:
                ret = ov7740_set_register_group(client, g);
                break;

        default:
                ret = -ENOTTY;
        }

        return ret;
}

/* Select the nearest higher resolution for capture */
static const struct ov7740_win_size *ov7740_select_win(u32 *width, u32 *height)
{
	int i, default_size = ARRAY_SIZE(ov7740_supported_win_sizes) - 1;

	for (i = 0; i < ARRAY_SIZE(ov7740_supported_win_sizes); i++) {
		if (ov7740_supported_win_sizes[i].width  >= *width &&
		    ov7740_supported_win_sizes[i].height >= *height) {
			*width = ov7740_supported_win_sizes[i].width;
			*height = ov7740_supported_win_sizes[i].height;
			return &ov7740_supported_win_sizes[i];
		}
	}

	*width = ov7740_supported_win_sizes[default_size].width;
	*height = ov7740_supported_win_sizes[default_size].height;
	return &ov7740_supported_win_sizes[default_size];
}

static int ov7740_set_params(struct i2c_client *client, u32 *width, u32 *height,
			     u32 code)
{
	struct ov7740_priv       *priv = to_ov7740(client);
	int ret;

	/* select win */
	priv->win = ov7740_select_win(width, height);

	/* select format */
	priv->cfmt_code = 0;
	switch (code) {
	default:
	case MEDIA_BUS_FMT_YUYV8_2X8:
		dev_dbg(&client->dev, "%s: Selected cfmt YUYV (YUV422)", __func__);
	}

	/* reset hardware */
	ov7740_reset(client);

	/* initialize the sensor with default data */
	dev_dbg(&client->dev, "%s: Init default", __func__);
	ret = ov7740_write_array(client, priv->win->regs, priv->win->ignore_errors);
	if (ret < 0)
		goto err;

	priv->cfmt_code = code;
	*width = priv->win->width;
	*height = priv->win->height;

	return 0;

err:
	dev_err(&client->dev, "%s: Error %d", __func__, ret);
	ov7740_reset(client);
	priv->win = NULL;

	return ret;
}

static int ov7740_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct i2c_client  *client = v4l2_get_subdevdata(sd);
	struct ov7740_priv *priv = to_ov7740(client);

	if (format->pad)
		return -EINVAL;

	if (!priv->win) {
		u32 width = W_VGA, height = H_VGA;
		priv->win = ov7740_select_win(&width, &height);
		priv->cfmt_code = MEDIA_BUS_FMT_YUYV8_2X8;
	}

	mf->width	= priv->win->width;
	mf->height	= priv->win->height;
	mf->code	= priv->cfmt_code;

	switch (mf->code) {
	case MEDIA_BUS_FMT_RGB565_2X8_BE:
	case MEDIA_BUS_FMT_RGB565_2X8_LE:
		mf->colorspace = V4L2_COLORSPACE_SRGB;
		break;
	default:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
		mf->colorspace = V4L2_COLORSPACE_JPEG;
	}
	mf->field	= V4L2_FIELD_NONE;

	return 0;
}

static int ov7740_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (format->pad)
		return -EINVAL;

	/*
	 * select suitable win, but don't store it
	 */
	ov7740_select_win(&mf->width, &mf->height);

	mf->field	= V4L2_FIELD_NONE;

	switch (mf->code) {
	case MEDIA_BUS_FMT_RGB565_2X8_BE:
	case MEDIA_BUS_FMT_RGB565_2X8_LE:
		mf->colorspace = V4L2_COLORSPACE_SRGB;
		break;
	default:
		mf->code = MEDIA_BUS_FMT_UYVY8_2X8;
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
		mf->colorspace = V4L2_COLORSPACE_JPEG;
	}

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return ov7740_set_params(client, &mf->width, &mf->height,
				mf->code);

	cfg->try_fmt = *mf;
	return 0;
}

static int ov7740_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index >= ARRAY_SIZE(ov7740_codes))
		return -EINVAL;

	code->code = ov7740_codes[code->index];
	return 0;
}

static int ov7740_video_probe(struct i2c_client *client)
{
	struct ov7740_priv *priv = to_ov7740(client);
	u8 pid, ver, midh, midl;
	const char *devname;
	int ret;

	ret = ov7740_s_power(&priv->subdev, 1);
	if (ret < 0)
		return ret;

	/*
	 * check and show product ID and manufacturer ID
	 */
	pid  = i2c_smbus_read_byte_data(client, PID);
	ver  = i2c_smbus_read_byte_data(client, VER);
	midh = i2c_smbus_read_byte_data(client, MIDH);
	midl = i2c_smbus_read_byte_data(client, MIDL);

	switch (VERSION(pid, ver)) {
	case PID_OV7740:
		devname     = "ov7740";
		break;
	default:
		dev_err(&client->dev,
			"Product ID error %x:%x\n", pid, ver);
		ret = -ENODEV;
		goto done;
	}

	dev_info(&client->dev,
		 "%s Product ID %0x:%0x Manufacturer ID %x:%x\n",
		 devname, pid, ver, midh, midl);

	ret = v4l2_ctrl_handler_setup(&priv->hdl);

done:
	ov7740_s_power(&priv->subdev, 0);
	return ret;
}

static const struct v4l2_ctrl_ops ov7740_ctrl_ops = {
	.s_ctrl = ov7740_s_ctrl,
};

static struct v4l2_subdev_core_ops ov7740_subdev_core_ops = {
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register	= ov7740_g_register,
	.s_register	= ov7740_s_register,
#endif
	.s_power	= ov7740_s_power,
        .ioctl          = ov7740_ioctl,
};

static int ov7740_g_mbus_config(struct v4l2_subdev *sd,
				struct v4l2_mbus_config *cfg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);

	cfg->flags = V4L2_MBUS_PCLK_SAMPLE_RISING | V4L2_MBUS_MASTER |
		V4L2_MBUS_VSYNC_ACTIVE_HIGH | V4L2_MBUS_HSYNC_ACTIVE_HIGH |
		V4L2_MBUS_VSYNC_ACTIVE_LOW |
		V4L2_MBUS_DATA_ACTIVE_HIGH;
	cfg->type = V4L2_MBUS_PARALLEL;
	cfg->flags = soc_camera_apply_board_flags(ssdd, cfg);

	return 0;
}

static struct v4l2_subdev_video_ops ov7740_subdev_video_ops = {
	.s_stream	= ov7740_s_stream,
	.g_mbus_config	= ov7740_g_mbus_config,
};

static const struct v4l2_subdev_pad_ops ov7740_subdev_pad_ops = {
	.enum_mbus_code = ov7740_enum_mbus_code,
	.get_fmt	= ov7740_get_fmt,
	.set_fmt	= ov7740_set_fmt,
};

static struct v4l2_subdev_ops ov7740_subdev_ops = {
	.core	= &ov7740_subdev_core_ops,
	.video	= &ov7740_subdev_video_ops,
	.pad	= &ov7740_subdev_pad_ops,
};

/* OF probe functions */
static int ov7740_hw_power(struct device *dev, int on)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ov7740_priv *priv = to_ov7740(client);

	dev_dbg(&client->dev, "%s: %s the camera\n",
			__func__, on ? "ENABLE" : "DISABLE");

	if (priv->pwdn_gpio)
		gpiod_direction_output(priv->pwdn_gpio, !on);

	/* We need wait for ~1ms, then access the SCCB */
	if (on)
		usleep_range(1000, 2000);

	return 0;
}

static int ov7740_hw_reset(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ov7740_priv *priv = to_ov7740(client);

	if (priv->resetb_gpio) {
		/* Active the resetb pin to perform a reset pulse */
		gpiod_direction_output(priv->resetb_gpio, 1);
		usleep_range(1000, 3000);
		gpiod_direction_output(priv->resetb_gpio, 0);
	}

	/* We need wait for ~20ms, then access the SCCB */
	usleep_range(20000, 21000);

	return 0;
}

static int ov7740_probe_dt(struct i2c_client *client,
		struct ov7740_priv *priv)
{
	/* Request the reset GPIO deasserted */
	priv->resetb_gpio = devm_gpiod_get_optional(&client->dev, "resetb",
			GPIOD_OUT_LOW);
	if (!priv->resetb_gpio)
		dev_dbg(&client->dev, "resetb gpio is not assigned!\n");
	else if (IS_ERR(priv->resetb_gpio))
		return PTR_ERR(priv->resetb_gpio);

	/* Request the power down GPIO asserted */
	priv->pwdn_gpio = devm_gpiod_get_optional(&client->dev, "pwdn",
			GPIOD_OUT_HIGH);
	if (!priv->pwdn_gpio)
		dev_dbg(&client->dev, "pwdn gpio is not assigned!\n");
	else if (IS_ERR(priv->pwdn_gpio))
		return PTR_ERR(priv->pwdn_gpio);

	/* Initialize the soc_camera_subdev_desc */
	priv->ssdd_dt.power = ov7740_hw_power;
	priv->ssdd_dt.reset = ov7740_hw_reset;
	client->dev.platform_data = &priv->ssdd_dt;

	return 0;
}

/*
 * i2c_driver functions
 */
static int ov7740_probe(struct i2c_client *client,
			const struct i2c_device_id *did)
{
	struct ov7740_priv	*priv;
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);
	struct i2c_adapter	*adapter = to_i2c_adapter(client->dev.parent);
	int			ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&adapter->dev,
			"OV7740: I2C-Adapter doesn't support SMBUS\n");
		return -EIO;
	}

	priv = devm_kzalloc(&client->dev, sizeof(struct ov7740_priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&adapter->dev,
			"Failed to allocate memory for private data!\n");
		return -ENOMEM;
	}

	priv->clk = v4l2_clk_get(&client->dev, "xvclk");
	if (IS_ERR(priv->clk))
		return -EPROBE_DEFER;

	if (!ssdd && !client->dev.of_node) {
		dev_err(&adapter->dev,
			"OV7740: Missing platform_data for driver\n");
		ret = -EINVAL;
		goto err_clk;
	}

	if (!ssdd) {
		ret = ov7740_probe_dt(client, priv);
		if (ret)
			goto err_clk;
	}

	v4l2_i2c_subdev_init(&priv->subdev, client, &ov7740_subdev_ops);
        priv->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	v4l2_ctrl_handler_init(&priv->hdl, 2);
	v4l2_ctrl_new_std(&priv->hdl, &ov7740_ctrl_ops,
			V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&priv->hdl, &ov7740_ctrl_ops,
			V4L2_CID_HFLIP, 0, 1, 1, 0);
	priv->subdev.ctrl_handler = &priv->hdl;
	if (priv->hdl.error) {
		ret = priv->hdl.error;
		goto err_clk;
	}

	ret = ov7740_video_probe(client);
	if (ret)
		goto err_handler;

	ret = v4l2_async_register_subdev(&priv->subdev);
	if (ret < 0)
		goto err_handler;

        ret = v4l2_device_register_subdev_nodes(priv->subdev.v4l2_dev);
        if (ret < 0)
                goto err_handler;

	dev_info(&adapter->dev, "OV7740 Probed\n");
	return 0;

err_handler:
	v4l2_ctrl_handler_free(&priv->hdl);
err_clk:
	v4l2_clk_put(priv->clk);
	return ret;
}

static int ov7740_remove(struct i2c_client *client)
{
	struct ov7740_priv       *priv = to_ov7740(client);

	v4l2_async_unregister_subdev(&priv->subdev);
	v4l2_clk_put(priv->clk);
	v4l2_device_unregister_subdev(&priv->subdev);
	v4l2_ctrl_handler_free(&priv->hdl);
	return 0;
}

static const struct i2c_device_id ov7740_id[] = {
	{ "ov7740", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov7740_id);

static const struct of_device_id ov7740_of_match[] = {
	{.compatible = "ovti,ov7740", },
	{},
};
MODULE_DEVICE_TABLE(of, ov7740_of_match);

static struct i2c_driver ov7740_i2c_driver = {
	.driver = {
		.name = "ov7740",
		.of_match_table = of_match_ptr(ov7740_of_match),
	},
	.probe    = ov7740_probe,
	.remove   = ov7740_remove,
	.id_table = ov7740_id,
};

module_i2c_driver(ov7740_i2c_driver);

MODULE_DESCRIPTION("SoC Camera driver for Omni Vision 7740 sensor");
MODULE_AUTHOR("Josh Wu");
MODULE_LICENSE("GPL v2");
