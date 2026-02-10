/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/types.h>

struct drm_file;

struct drm_mchp_timespec;
struct drm_mchp_gfx2d_rectangle;
struct drm_mchp_gfx2d_submit;

struct mchp_gfx2d_device;
struct mchp_gfx2d_file;
struct mchp_gfx2d_gem_object;

#define GFX2D_WD0_OPCODE(op)    FIELD_PREP(GENMASK(31, 28), (op))
#define GFX2D_WD0_IE            BIT(24)
#define GFX2D_WD0_DREG(reg)     FIELD_PREP(GENMASK(23, 20), (reg))
#define GFX2D_WD0_REG(reg)      FIELD_PREP(GENMASK(19, 16), (reg))
#define GFX2D_WD0_REGAD(reg)    FIELD_PREP(GENMASK(15, 12), (reg))
#define GFX2D_WD0_ARGS(nargs)   FIELD_PREP(GENMASK(2, 0), (nargs))

#define GFX2D_WD1_HEIGHT(h)     FIELD_PREP(GENMASK(28, 16), (h) - 1)
#define GFX2D_WD1_WIDTH(w)      FIELD_PREP(GENMASK(12, 0), (w) - 1)
#define GFX2D_WD1_SIZE(rect)    (GFX2D_WD1_HEIGHT((rect)->height) | \
				 GFX2D_WD1_WIDTH((rect)->width))

#define GFX2D_Y(y)              FIELD_PREP(GENMASK(28, 16), (y))
#define GFX2D_X(x)              FIELD_PREP(GENMASK(12, 0), (x))
#define GFX2D_DPOS(rect)        (GFX2D_Y((rect)->dst_y) | GFX2D_X((rect)->dst_x))
#define GFX2D_SPOS(rect, index) (GFX2D_Y((rect)->src_y[(index)]) | \
				 GFX2D_X((rect)->src_x[(index)]))

#define GFX2D_WD2_DPOS(rect)    GFX2D_DPOS(rect)
#define GFX2D_WD3_SPOS(rect)    GFX2D_SPOS(rect, 0)
#define GFX2D_WD3_S0POS(rect)   GFX2D_SPOS(rect, 0)
#define GFX2D_WD4_S1POS(rect)   GFX2D_SPOS(rect, 1)

#define GFX2D_WD5_DPRE(b)       FIELD_PREP(BIT(23), (b))
#define GFX2D_WD5_DAFACT(f)     FIELD_PREP(GENMASK(22, 20), (f))
#define GFX2D_WD5_SPRE(b)       FIELD_PREP(BIT(19), (b))
#define GFX2D_WD5_SAFACT(f)     FIELD_PREP(GENMASK(18, 16), (f))
#define GFX2D_WD5_FUNC(func)    FIELD_PREP(GENMASK(15, 8), (func))
#define GFX2D_WD5_DCFACT(f)     FIELD_PREP(GENMASK(7, 4), (f))
#define GFX2D_WD5_SCFACT(f)     FIELD_PREP(GENMASK(3, 0), (f))

#define GFX2D_WD6_ROPM(mode)    FIELD_PREP(GENMASK(17, 16), (mode))
#define GFX2D_WD6_ROPH(h)       FIELD_PREP(GENMASK(15, 8), (h))
#define GFX2D_WD6_ROPL(l)       FIELD_PREP(GENMASK(7, 0), (l))

enum mchp_gfx2d_opcode {
	GFX2D_OPCODE_LDR = 8,
	GFX2D_OPCODE_STR = 9,
	GFX2D_OPCODE_FILL = 11,
	GFX2D_OPCODE_COPY = 12,
	GFX2D_OPCODE_BLEND = 13,
	GFX2D_OPCODE_ROP = 14,
};

enum mchp_gfx2d_reg_id {
	REG_PA0,
	REG_PITCH0,
	REG_CFG0,
	REG_PA1,
	REG_PITCH1,
	REG_CFG1,
	REG_PA2,
	REG_PITCH2,
	REG_CFG2,
	REG_PA3,
	REG_GPREG0,
	REG_GPREG1,
	REG_GPREG2,
	REG_GPREG3,
	REG_GPREG4,
	REG_GPREG5,
};

#define REG_PA(i)       ((enum mchp_gfx2d_reg_id)(REG_PA0 + 3 * (i)))
#define REG_PITCH(i)    ((enum mchp_gfx2d_reg_id)(REG_PITCH0 + 3 * (i)))
#define REG_CFG(i)      ((enum mchp_gfx2d_reg_id)(REG_CFG0 + 3 * (i)))

struct mchp_gfx2d_rectangle {
	u16 dst_x;
	u16 dst_y;
	u16 src_x[2];
	u16 src_y[2];
	u16 width;
	u16 height;
};

struct mchp_gfx2d_source {
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	int x;
	int y;
};

struct mchp_gfx2d_fill {
	u32 color;
};

#define GFX2D_BLEND_DPRE                BIT(0)
#define GFX2D_BLEND_SPRE                BIT(1)
#define GFX2D_BLEND_SET_DST_COLOR       BIT(2)
#define GFX2D_BLEND_SET_SRC_COLOR       BIT(3)

struct mchp_gfx2d_blend {
	u32 flags;
	u32 dst_color;
	u32 src_color;
	u8 func;
	u8 safact;
	u8 dafact;
	u8 scfact;
	u8 dcfact;
};

struct mchp_gfx2d_rop {
	u8 mode;
	u8 high;
	u8 low;
};

struct mchp_gfx2d_command {
	struct list_head cmd_node;

	struct mchp_gfx2d_file *ctx;

	struct mchp_gfx2d_gem_object *target;
	struct mchp_gfx2d_source sources[2];
	struct mchp_gfx2d_gem_object *mask;

	struct drm_mchp_gfx2d_rectangle *rects;
	size_t num_rects;
	size_t next_rect;

	uint32_t id;

	bool can_free_rects;
	u8 operation;
	union {
		struct mchp_gfx2d_fill fill;
		struct mchp_gfx2d_blend blend;
		struct mchp_gfx2d_rop rop;
	};
};

int mchp_gfx2d_init_command_queue(struct mchp_gfx2d_device *priv);
void mchp_gfx2d_cleanup_command_queue(struct mchp_gfx2d_device *priv);

int mchp_gfx2d_submit(struct mchp_gfx2d_device *priv,
		      struct drm_file *file_priv,
		      const struct drm_mchp_gfx2d_submit *args);

int mchp_gfx2d_wait(struct mchp_gfx2d_device *priv,
		    struct mchp_gfx2d_gem_object *gfx2d_obj,
		    const struct drm_mchp_timespec *timeout);

void mchp_gfx2d_cancel_commands(struct mchp_gfx2d_device *priv,
				struct mchp_gfx2d_file *ctx);

bool mchp_gfx2d_has_pending_commands(struct mchp_gfx2d_device *priv);

bool mchp_gfx2d_progress(struct mchp_gfx2d_device *priv);
