/* SPDX-License-Identifier: GPL-2.0-only */
/*
 */

#ifndef __DT_BINDINGS_MICROCHIP_COMMON_H__
#define __DT_BINDINGS_MICROCHIP_COMMON_H__

/*
 * Video format codes as defined in "AXI4-Stream Video IP and System Design
 * Guide".
 */
#define MVCF_MONO_SENSOR		0
#define MVCF_YUV_444			1
#define MVCF_YUV_422			2
#define MVCF_YUV_420			3
#define MVCF_RBG			4
#define MVCF_H264			5
#define MVCF_MJPEG			6
#define MVCF_RGB			7

#define MVC_MIN_WIDTH                  32
#define MVC_MAX_WIDTH                  7680
#define MVC_MIN_HEIGHT                 32
#define MVC_MAX_HEIGHT                 7680

#endif /* __DT_BINDINGS_MICROCHIP_COMMON_H__ */
