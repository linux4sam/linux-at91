/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#ifndef MCHP_GFX2D_H
#define MCHP_GFX2D_H

#include <drm/drm_device.h>

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include <uapi/drm/microchip_drm.h>

struct mchp_gfx2d_gem_object;

#define MCHP_GFX2D_MAX_RECTANGLES       32
#define MCHP_GFX2D_MAX_SURFACES         3

struct mchp_gfx2d_caps {
	bool has_dreg;
};

struct mchp_gfx2d_ring_buffer {
	uint32_t *data;
	u32 head;
	size_t size;
	size_t size_in_bytes;
};

struct mchp_gfx2d_device {
	struct drm_device drm;
	void __iomem *regs;
	struct clk *pclk;

	/* Waiters management. */
	struct mutex wlist_mutex;
	struct list_head wlist;

	/* Command ring buffer management. */
	dma_addr_t rb_dma_addr;
	struct mchp_gfx2d_ring_buffer rb;
	dma_addr_t mbx_dma_addr;
	const uint32_t *mbx;
	uint32_t cpu_latest_completed_cmd;

	/* Command queue management. */
	struct mutex free_cmdlist_mutex;
	struct list_head free_cmdlist;
	struct mutex cmdlist_mutex;
	struct list_head pending_cmdlist;
	struct list_head running_cmdlist;
	struct kmem_cache *cmd_cache;
	struct completion running_cmdlist_empty;
	bool is_suspended;

	/*
	 * Latest surfaces queued to be loaded into the GFX2D registers.
	 * LDR commands may still be queued, hence not executed yet,
	 * so the GFX2D_PAx, GFX2D_PITCHx and GFX2D_GFGx registers may
	 * still have different values.
	 * This is used to know whether some LDR commands should be queued
	 * to change a surface inside the GFX2D before executed other
	 * operations.
	 */
	struct mchp_gfx2d_gem_object *surfaces[MCHP_GFX2D_MAX_SURFACES];

	const struct mchp_gfx2d_caps *caps;
};

#define drm_to_dev(dev) container_of(dev, struct mchp_gfx2d_device, drm)

struct mchp_gfx2d_file {
	struct drm_mchp_gfx2d_rectangle rectangles[MCHP_GFX2D_MAX_RECTANGLES];
};

/* GFX2D register offsets */
#define GFX2D_GC                0x00    /* Global Configuration Register */
#define GFX2D_GE                0x04    /* Global Enable Register */
#define GFX2D_GD                0x08    /* Global Disable Register */
#define GFX2D_GS                0x0C    /* Global Status Register */
#define GFX2D_IE                0x10    /* Interrupt Enable Register */
#define GFX2D_ID                0x14    /* Interrupt Disable Register */
#define GFX2D_IM                0x18    /* Interrupt Mask Register */
#define GFX2D_IS                0x1c    /* Interrupt Status Register */
#define GFX2D_PC0               0x20    /* Performance Configuration 0 Register */
#define GFX2D_MC0               0x24    /* Metrics Counter 0 Register */
#define GFX2D_PC1               0x28    /* Performance Configuration 1 Register */
#define GFX2D_MC1               0x2c    /* Metrics Counter 1 Register */
#define GFX2D_BASE              0x30    /* Ring Buffer Base Physical Address Register */
#define GFX2D_LEN               0x34    /* Ring Buffer Length Register */
#define GFX2D_HEAD              0x38    /* Ring Buffer Head Register */
#define GFX2D_TAIL              0x3c    /* Ring Buffer Tail Register */

/* Surface x Physical Address Register [x=0..3] */
#define GFX2D_PA(x)             (0x40 + 0x10 * (x))

/* Surface x Pitch Register [x=0..3] */
#define GFX2D_PITCH(x)          (0x44 + 0x10 * (x))
#define GFX2D_PITCH_DAT(stride) FIELD_PREP(GENMASK(15, 0), (stride))

/* Surface x Configuration Register [x=0..3] */
#define GFX2D_CFG(x)            (0x48 + 0x10 * (x))
#define GFX2D_CFG_IDXCX         BIT(4)
#define GFX2D_CFG_PF(format)    FIELD_PREP(GENMASK(3, 0), (format))

/* Bitfields in GFX2D_GE (Global Enable Register) */
#define GFX2D_GE_ENABLE         BIT(0)  /* GFX2D Enable */

/* Bitfields in GFX2D_GD (Global Disable Register) */
#define GFX2D_GD_DISABLE        BIT(0)  /* GFX2D Disable Bit */
#define GFX2D_GD_WFERES         BIT(8)  /* WFE Software Resume Bit */

/* Bitfields in GFX2D_GS (Global Status Register) */
#define GFX2D_GS_STATUS         BIT(0)  /* GFX2D Status Bit */
#define GFX2D_GS_BUSY           BIT(4)  /* GFX2D Busy Bit */
#define GFX2D_GS_WFEIP          BIT(8)  /* Wait For Event Status bit */

/* Bitfields in GFX2D_I{S,E,D,M} (Interrupt Status/Enable/Disable/Mask Register */
#define GFX2D_IRQ_RBEMPTY       BIT(0)  /* Ring Buffer Empty */
#define GFX2D_IRQ_EXEND         BIT(1)  /* End of Execution */
#define GFX2D_IRQ_RERR          BIT(2)  /* Read Error */
#define GFX2D_IRQ_BERR          BIT(3)  /* Write Error */
#define GFX2D_IRQ_IERR          BIT(4)  /* Illegal Instruction Error */

#endif /* MCHP_GFX2D_H */
