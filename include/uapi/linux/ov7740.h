/*
 * ov7740.h
 *
 * OmniVision ov7740 Image Sensor - User-space API
 *
 * Copyright (C) 2015 iRobot
 *
 * Contact: Patrick Doyle <pdoyle@irobot.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#ifndef _UAPI_OV7740_H
#define _UAPI_OV7740_H

#include <linux/types.h>
#include <linux/videodev2.h>

#define VIDIOC_OV7740_MAX_REGS 256 // Nobody should ever need more than 640Kbytes of RAM
#define VIDIOC_OV7740_AUTOSIZE_FLAG -1
#define VIDIOC_OV7740_ENDMARKER { 0xff, 0xff }

struct v4l2_ov7740_register_group {
        __s32 numregs;
        // Note: Set the ignore_errors flag to ignore all errors when writing registers.
        // It seems there are some registers that will give a NAK error when written to.
        __u32 ignore_errors;
        struct {
                __u8 reg;
                __u8 val;
        } regs[VIDIOC_OV7740_MAX_REGS + 1];
} __attribute__ ((packed));

/*
 * Private IOCTLs
 *
 * VIDIOC_OV7740_S_VGA_REGS:       Configure register settings for VGA mode
 * VIDIOC_OV7740_G_VGA_REGS:       Retrieve register settings for VGA mode
 * VIDIOC_OV7740_S_QVGA_REGS:      Configure register settings for QVGA mode
 * VIDIOC_OV7740_G_QVGA_REGS:      Retrieve register settings for QVGA mode
 * VIDIOC_OV7740_S_REGISTER_GROUP: Write a group of registers on the device
 * VIDIOC_OV7740_G_REGISTER_GROUP: Read a group of registers on the device
 */

#define VIDIOC_OV7740_S_VGA_REGS \
        _IOW ('V', BASE_VIDIOC_PRIVATE + 0, struct v4l2_ov7740_register_group)
#define VIDIOC_OV7740_G_VGA_REGS \
        _IOWR('V', BASE_VIDIOC_PRIVATE + 1, struct v4l2_ov7740_register_group)
#define VIDIOC_OV7740_S_QVGA_REGS \
        _IOW ('V', BASE_VIDIOC_PRIVATE + 2, struct v4l2_ov7740_register_group)
#define VIDIOC_OV7740_G_QVGA_REGS \
        _IOWR('V', BASE_VIDIOC_PRIVATE + 3, struct v4l2_ov7740_register_group)
#define VIDIOC_OV7740_S_REGISTER_GROUP \
        _IOW ('V', BASE_VIDIOC_PRIVATE + 4, struct v4l2_ov7740_register_group)
#define VIDIOC_OV7740_G_REGISTER_GROUP \
        _IOWR('V', BASE_VIDIOC_PRIVATE + 5, struct v4l2_ov7740_register_group)

#endif /* _UAPI_OV7740_H */
