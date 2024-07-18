/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Copyright (c) 2024 Microchip
 * All right reserved.
 */

#ifndef __MICROCHIP_DRM_H__
#define __MICROCHIP_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* timeouts are specified in clock-monotonic absolute times (to simplify
 * restarting interrupted ioctls).  The following struct is logically the
 * same as 'struct timespec' but 32/64b ABI safe.
 */
struct drm_mchp_timespec {
	__s64 tv_sec;          /* seconds */
	__s64 tv_nsec;         /* nanoseconds */
};

struct drm_mchp_gfx2d_source {
	__u32 handle;
	__s32 x;
	__s32 y;
};

struct drm_mchp_gfx2d_rectangle {
	__s32 x;
	__s32 y;
	__s32 w;
	__s32 h;
};

/**
 * Command op codes.
 */
enum drm_mchp_gfx2d_operation {
	DRM_MCHP_GFX2D_OP_FILL,
	DRM_MCHP_GFX2D_OP_COPY,
	DRM_MCHP_GFX2D_OP_BLEND,
	DRM_MCHP_GFX2D_OP_ROP,
};

/**
 * Parameters for GFX2D FILL graphics instruction.
 */
struct drm_mchp_gfx2d_fill {
	__u32 color;
};

/**
 * Blend functions.
 */
enum drm_mchp_gfx2d_blend_function {
	/** @f[ C_f = S*C_s + D*C_d @f] */
	DRM_MCHP_GFX2D_BFUNC_ADD,
	/** @f[ C_f = S*C_s - D*C_d @f] */
	DRM_MCHP_GFX2D_BFUNC_SUBTRACT,
	/** @f[ C_f = D*C_d - S*C_s @f] */
	DRM_MCHP_GFX2D_BFUNC_REVERSE,
	/** @f[ C_f = min(C_s,C_d) @f] */
	DRM_MCHP_GFX2D_BFUNC_MIN,
	/** @f[ C_f = max(C_s,C_d) @f] */
	DRM_MCHP_GFX2D_BFUNC_MAX,
	/** Special Blending Functions */
	DRM_MCHP_GFX2D_BFUNC_SPE,

	DRM_MCHP_GFX2D_BFUNC_SPE_LIGHTEN =    DRM_MCHP_GFX2D_BFUNC_SPE | (0<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_DARKEN =     DRM_MCHP_GFX2D_BFUNC_SPE | (1<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_MULTIPLY =   DRM_MCHP_GFX2D_BFUNC_SPE | (2<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_AVERAGE =    DRM_MCHP_GFX2D_BFUNC_SPE | (3<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_ADD =        DRM_MCHP_GFX2D_BFUNC_SPE | (4<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_SUBTRACT =   DRM_MCHP_GFX2D_BFUNC_SPE | (5<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_DIFFERENCE = DRM_MCHP_GFX2D_BFUNC_SPE | (6<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_NEGATION =   DRM_MCHP_GFX2D_BFUNC_SPE | (7<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_SCREEN =     DRM_MCHP_GFX2D_BFUNC_SPE | (8<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_OVERLAY =    DRM_MCHP_GFX2D_BFUNC_SPE | (9<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_DODGE =      DRM_MCHP_GFX2D_BFUNC_SPE | (10<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_BURN =       DRM_MCHP_GFX2D_BFUNC_SPE | (11<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_REFLECT =    DRM_MCHP_GFX2D_BFUNC_SPE | (12<<4),
	DRM_MCHP_GFX2D_BFUNC_SPE_GLOW =       DRM_MCHP_GFX2D_BFUNC_SPE | (13<<4),
};

/**
 * Source and destination blend factors.
 */
enum drm_mchp_gfx2d_blend_factor {
	/** @f[ (0,0,0,0) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ZERO,
	/** @f[ (1,1,1,1) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE,
	/** @f[ (A_s,R_s,G_s,B_s) @f] */
	DRM_MCHP_GFX2D_BFACTOR_SRC_COLOR,
	/** @f[ (1,1,1,1) - (A_s,R_s,G_s,B_s) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_SRC_COLOR,
	/** @f[ (A_d,R_d,G_d,B_d) @f] */
	DRM_MCHP_GFX2D_BFACTOR_DST_COLOR,
	/** @f[ (1,1,1,1)-(A_d,R_d,G_d,B_d) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_DST_COLOR,
	/** @f[ (A_s,A_s,A_s,A_s) @f] */
	DRM_MCHP_GFX2D_BFACTOR_SRC_ALPHA,
	/** @f[ (1,1,1,1)-(A_s,A_s,A_s,A_s) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_SRC_ALPHA,
	/** @f[ (A_d,A_d,A_d,A_d) @f] */
	DRM_MCHP_GFX2D_BFACTOR_DST_ALPHA,
	/** @f[ (1,1,1,1)-(A_d,A_d,A_d,A_d) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_DST_ALPHA,
	/** @f[ (A_c,R_c,G_c,B_c) @f] */
	DRM_MCHP_GFX2D_BFACTOR_CONSTANT_COLOR,
	/** @f[ (1,1,1,1) - (A_c,R_c,G_c,B_c) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_COLOR,
	/** @f[ (A_c,A_c,A_c,A_c) @f] */
	DRM_MCHP_GFX2D_BFACTOR_CONSTANT_ALPHA,
	/** @f[ (1,1,1,1)-(A_c,A_c,A_c,A_c) @f] */
	DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_ALPHA,
	/** @f[ (1,i,i,i) @f] where i is equal to the minimum between
	 *  @f[ A_s @f] and @f[ 1-A_d @f]
	 */
	DRM_MCHP_GFX2D_BFACTOR_SRC_ALPHA_SATURATE,
};

#define DRM_MCHP_GFX2D_BLEND_DPRE       0x00000001
#define DRM_MCHP_GFX2D_BLEND_SPRE       0x00000002

/**
 * Parameters for the GFX2D BLEND graphics instruction.
 */
struct drm_mchp_gfx2d_blend {
	__u32 src_color;
	__u32 dst_color;
	__u32 flags;
	enum drm_mchp_gfx2d_blend_function function;
	enum drm_mchp_gfx2d_blend_factor safactor;
	enum drm_mchp_gfx2d_blend_factor dafactor;
	enum drm_mchp_gfx2d_blend_factor scfactor;
	enum drm_mchp_gfx2d_blend_factor dcfactor;
};

/**
 * ROP mode.
 */
enum drm_mchp_gfx2d_rop_mode {
	DRM_MCHP_GFX2D_ROP2,
	DRM_MCHP_GFX2D_ROP3,
	DRM_MCHP_GFX2D_ROP4,
};

/**
 * Parameters for the GFX2D ROP graphics instruction.
 */
struct drm_mchp_gfx2d_rop {
	__u32 mask_handle;
	enum drm_mchp_gfx2d_rop_mode mode;
	__u8 high;
	__u8 low;
};

struct drm_mchp_gfx2d_submit {
	__u64 rectangles;
	__kernel_size_t num_rectangles;
	__u32 target_handle;
	struct drm_mchp_gfx2d_source sources[2];
	enum drm_mchp_gfx2d_operation operation;
	union {
		struct drm_mchp_gfx2d_fill fill;
		struct drm_mchp_gfx2d_blend blend;
		struct drm_mchp_gfx2d_rop rop;
	};
};

#define DRM_MCHP_GFX2D_WAIT_NONBLOCK    0x00000001

struct drm_mchp_gfx2d_wait {
	struct drm_mchp_timespec timeout;
	__u32 handle;
	__u32 flags;
};

/**
 * Supported surface formats.
 */
enum drm_mchp_gfx2d_pixel_format {
	/** 4-bit indexed color, with 4-bit alpha value */
	DRM_MCHP_GFX2D_PF_A4IDX4,
	/** 8 bits per pixel alpha, with user-defined constant color */
	DRM_MCHP_GFX2D_PF_A8,
	/** 8 bits indexed color, uses the Color Look-Up Table to expand to true color */
	DRM_MCHP_GFX2D_PF_IDX8,
	/** 8-bit indexed color, with 8-bit alpha value */
	DRM_MCHP_GFX2D_PF_A8IDX8,
	/** 12 bits per pixel, 4 bits per color channel */
	DRM_MCHP_GFX2D_PF_RGB12,
	/** 16 bits per pixel with 4-bit width alpha value, and 4 bits per color channel */
	DRM_MCHP_GFX2D_PF_ARGB16,
	/** 15 bits per pixel, 5 bits per color channel */
	DRM_MCHP_GFX2D_PF_RGB15,
	/** 16 bits per pixel, with 1 bit for transparency and 5 bits for color channels */
	DRM_MCHP_GFX2D_PF_TRGB16,
	/** 16 bits per pixel, with 5 bits for color channels and 1 bit for transparency */
	DRM_MCHP_GFX2D_PF_RGBT16,
	/** 16 bits per pixel, 5 bits for the red and blue channels and 6 bits for the green one */
	DRM_MCHP_GFX2D_PF_RGB16,
	/** 24 bits per pixel, 8 bits for alpha and color channels */
	DRM_MCHP_GFX2D_PF_RGB24,
	/** 32 bits per pixel, 8 bits for alpha and color channels */
	DRM_MCHP_GFX2D_PF_ARGB32,
	/** 32 bits per pixel, 8 bits for alpha and color channels */
	DRM_MCHP_GFX2D_PF_RGBA32,
};

enum drm_mchp_gfx2d_direction {
	DRM_MCHP_GFX2D_DIR_BIDIRECTIONAL,
	DRM_MCHP_GFX2D_DIR_TO_DEVICE,
	DRM_MCHP_GFX2D_DIR_FROM_DEVICE,
	DRM_MCHP_GFX2D_DIR_NONE,
};

struct drm_mchp_gfx2d_alloc_buffer {
	__u32 size;     /* in bytes for mmap() */
	__u16 width;
	__u16 height;
	__u16 stride;
	enum drm_mchp_gfx2d_pixel_format format;
	enum drm_mchp_gfx2d_direction direction;

	__u32 handle;
	__u64 offset;
};

struct drm_mchp_gfx2d_import_buffer {
	__s32 fd;
	__u16 width;
	__u16 height;
	__u16 stride;
	enum drm_mchp_gfx2d_pixel_format format;

	__u32 handle;
};

struct drm_mchp_gfx2d_free_buffer {
	__u32 handle;
};

struct drm_mchp_gfx2d_sync_for_cpu {
	struct drm_mchp_timespec timeout;
	__u32 handle;
	__u32 flags;
};

struct drm_mchp_gfx2d_sync_for_gpu {
	__u32 handle;
};

#define DRM_MCHP_GFX2D_SUBMIT                   0x00
#define DRM_MCHP_GFX2D_WAIT                     0x01
#define DRM_MCHP_GFX2D_ALLOC_BUFFER             0x02
#define DRM_MCHP_GFX2D_IMPORT_BUFFER            0x03
#define DRM_MCHP_GFX2D_FREE_BUFFER              0x04
#define DRM_MCHP_GFX2D_SYNC_FOR_CPU             0x05
#define DRM_MCHP_GFX2D_SYNC_FOR_GPU             0x06

#define DRM_IOCTL_MCHP_GFX2D_SUBMIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_SUBMIT, struct drm_mchp_gfx2d_submit)
#define DRM_IOCTL_MCHP_GFX2D_WAIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_WAIT, struct drm_mchp_gfx2d_wait)
#define DRM_IOCTL_MCHP_GFX2D_ALLOC_BUFFER \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_ALLOC_BUFFER, struct drm_mchp_gfx2d_alloc_buffer)
#define DRM_IOCTL_MCHP_GFX2D_IMPORT_BUFFER \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_IMPORT_BUFFER, \
		 struct drm_mchp_gfx2d_import_buffer)
#define DRM_IOCTL_MCHP_GFX2D_FREE_BUFFER \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_FREE_BUFFER, struct drm_mchp_gfx2d_free_buffer)
#define DRM_IOCTL_MCHP_GFX2D_SYNC_FOR_CPU \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_SYNC_FOR_CPU, struct drm_mchp_gfx2d_sync_for_cpu)
#define DRM_IOCTL_MCHP_GFX2D_SYNC_FOR_GPU \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MCHP_GFX2D_SYNC_FOR_GPU, struct drm_mchp_gfx2d_sync_for_gpu)

#if defined(__cplusplus)
}
#endif

#endif /* __MICROCHIP_DRM_H__ */
