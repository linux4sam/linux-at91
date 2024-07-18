// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#include <linux/circ_buf.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/pm_runtime.h>

#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include "mchp_gfx2d_cmd.h"
#include "mchp_gfx2d_drv.h"
#include "mchp_gfx2d_gem.h"

#define MCHP_GFX2D_TIMEOUT                  1000 /* ms */
#define MCHP_GFX2D_NB_WORDS_LDR             2
#define MCHP_GFX2D_NB_WORDS_STR             1

/*
 * 3 LDR commands, 2 words each, to set GFX2D_PAx, GFX2D_PITCHx and
 * GFX2D_CFGx registers, when changing a surface.
 */
#define MCHP_GFX2D_NB_WORDS_SET_SURFACE     (3 * MCHP_GFX2D_NB_WORDS_LDR)

#define MCHP_GFX2D_NB_WORDS_EPILOGUE        (MCHP_GFX2D_NB_WORDS_LDR + MCHP_GFX2D_NB_WORDS_STR)

#define MCHP_GFX2D_NB_WORDS_FILL            4
#define MCHP_GFX2D_NB_WORDS_COPY            4
#define MCHP_GFX2D_NB_WORDS_BLEND           6
#define MCHP_GFX2D_NB_WORDS_ROP             7

#define BLEND_SREG                          REG_GPREG0
#define BLEND_DREG                          REG_GPREG1
#define CMD_END_REG                         REG_GPREG4
#define MBX_ADDR_REG                        REG_GPREG5

static void mchp_gfx2d_enable_exend(struct mchp_gfx2d_device *priv)
{
	drm_dbg(&priv->drm, "enable EXEND interrupt\n");
	writel(GFX2D_IRQ_EXEND, priv->regs + GFX2D_IE);
}

static void mchp_gfx2d_push(struct mchp_gfx2d_device *priv, uint32_t value)
{
	priv->rb.data[priv->rb.head] = value;
	priv->rb.head = (priv->rb.head + 1) & (priv->rb.size - 1);
}

static inline void mchp_gfx2d_trigger(struct mchp_gfx2d_device *priv)
{
	writel(priv->rb.head, priv->regs + GFX2D_HEAD);
}

static void mchp_gfx2d_ldr(struct mchp_gfx2d_device *priv,
			   enum mchp_gfx2d_reg_id reg, uint32_t value)
{
	uint32_t word0 = GFX2D_WD0_OPCODE(GFX2D_OPCODE_LDR) | GFX2D_WD0_REG(reg);

	mchp_gfx2d_push(priv, word0);
	mchp_gfx2d_push(priv, value);
}

static void mchp_gfx2d_epilogue(struct mchp_gfx2d_device *priv,
				struct mchp_gfx2d_command *cmd)
{
	mchp_gfx2d_ldr(priv, CMD_END_REG, cmd->id);
	mchp_gfx2d_push(priv,
			GFX2D_WD0_OPCODE(GFX2D_OPCODE_STR) |
			GFX2D_WD0_IE |
			GFX2D_WD0_REG(CMD_END_REG) |
			GFX2D_WD0_REGAD(MBX_ADDR_REG));
	mchp_gfx2d_trigger(priv);
}

/**
 * mchp_gfx2d_alloc_rectangles - reserve rectangles for a command
 * @cmd: the command which the rectangles are reserved for
 * @num_rectangles: the number of rectangles to reserve
 *
 * If requested rectangles fit into the 'cmd->ctx->rectangles' array, this
 * array is returned (as a temporary storage). Otherwise, memory is allocated
 * with kvmalloc_array(), hence should be freed later.
 *
 * In any case, mchp_gfx2d_free_rectangles() MUST be called on @cmd from
 * mchp_gfx2d_release_command().
 *
 * RETURNS:
 * 0 if the reservation was successful, -ENOMEM otherwise.
 */
static int mchp_gfx2d_alloc_rectangles(struct mchp_gfx2d_command *cmd,
				       size_t num_rectangles)
{
	cmd->num_rects = num_rectangles;

	if (num_rectangles <= ARRAY_SIZE(cmd->ctx->rectangles)) {
		cmd->can_free_rects = false;
		cmd->rects = cmd->ctx->rectangles;
		return 0;
	}

	cmd->rects = kvmalloc_array(num_rectangles,
				    sizeof(struct drm_mchp_gfx2d_rectangle),
				    GFP_KERNEL);
	cmd->can_free_rects = (cmd->rects != NULL);
	return cmd->can_free_rects ? 0 : -ENOMEM;
}

/**
 * mchp_gfx2d_dup_rectangles - duplicate rectangles for a command, if needed
 * @cmd: the command which the rectangles are duplicated for
 *
 * If 'cmd->rects' points to 'cmd->ctx->rectangles' ('cmd->can_free_rect'
 * is false) this functions allocates memory then copy the command rectangles
 * into the new array.
 *
 * If 'cmd->rects' has already been allocated with kvmalloc_array() from
 * mchp_gfx2d_alloc_rectangles() then this function does nothing.
 *
 * RETURNS:
 * 0 on success, -ENOMEM otherwise.
 */
static int mchp_gfx2d_dup_rectangles(struct mchp_gfx2d_command *cmd)
{
	struct drm_mchp_gfx2d_rectangle *dup;

	if (cmd->can_free_rects)
		return 0;

	dup = kvmalloc_array(cmd->num_rects, sizeof(*cmd->rects), GFP_KERNEL);
	if (!dup)
		return -ENOMEM;

	memcpy(dup, cmd->rects, cmd->num_rects * sizeof(*cmd->rects));
	cmd->rects = dup;
	cmd->can_free_rects = true;
	return 0;
}

/**
 * mchp_gfx2d_free_rectangles - free the rectangles for a command, if needed
 * @cmd: the command which the rectangles are freed for
 *
 * If 'cmd->can_free_rects' is true, call kvfree() on 'cmd->rects'.
 * Otherwise, either 'cmd->rects' is NULL or points to 'cmd->ctx->rectangles',
 * so nothing has to be done.
 */
static void mchp_gfx2d_free_rectangles(struct mchp_gfx2d_command *cmd)
{
	if (cmd->can_free_rects)
		kvfree(cmd->rects);
}

/**
 * mchp_gfx2d_release_command - release a command
 * @priv: the GFX2D device
 * @cmd: the command to release
 * @recycle: if true, insert @cmd into 'priv->free_cmdlist' for reuse
 *
 * This function releases a command and its resources.
 *
 * It MAY be called with the 'priv->cmdlist_mutex' held.
 */
static void mchp_gfx2d_release_command(struct mchp_gfx2d_device *priv,
				       struct mchp_gfx2d_command *cmd,
				       bool recycle)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	int i;

	drm_dbg(&priv->drm, "cmd %u: release command\n", cmd->id);

	gfx2d_obj = cmd->target;
	if (gfx2d_obj) {
		mchp_gfx2d_gem_unref(gfx2d_obj);
		drm_dbg(&priv->drm,
			"cmd %u: unref object %u (%d) as target\n",
			cmd->id,
			gfx2d_obj->id, atomic_read(&gfx2d_obj->gpu_active));
	}

	for (i = 0; i < ARRAY_SIZE(cmd->sources); i++) {
		struct mchp_gfx2d_source *src = &cmd->sources[i];

		gfx2d_obj = src->gfx2d_obj;

		if (!gfx2d_obj)
			continue;

		mchp_gfx2d_gem_unref(gfx2d_obj);
		drm_dbg(&priv->drm,
			"cmd %u: unref object %u (%d) as source %d at (%d,%d)\n",
			cmd->id,
			gfx2d_obj->id, atomic_read(&gfx2d_obj->gpu_active),
			i, src->x, src->y);
	}

	gfx2d_obj = cmd->mask;
	if (gfx2d_obj) {
		mchp_gfx2d_gem_unref(gfx2d_obj);
		drm_dbg(&priv->drm,
			"cmd %u: unref object %u (%d) as mask\n",
			cmd->id,
			gfx2d_obj->id, atomic_read(&gfx2d_obj->gpu_active));
	}

	mchp_gfx2d_free_rectangles(cmd);

	if (recycle) {
		mutex_lock(&priv->free_cmdlist_mutex);
		list_add_tail(&cmd->cmd_node, &priv->free_cmdlist);
		mutex_unlock(&priv->free_cmdlist_mutex);
	}
}

/**
 * mchp_gfx2d_cmd_remaining_rects - get the number of remaining rects to process
 * @cmd: the command gathering graphics instructions with the same op code
 *
 * RETURNS:
 * number of rectangles that have not been appended yet into the ring buffer.
 */
static inline size_t
mchp_gfx2d_cmd_remaining_rects(const struct mchp_gfx2d_command *cmd)
{
	return cmd->num_rects - cmd->next_rect;
}

/**
 * mchp_gfx2d_cmd_is_fully_scheduled - tell whether there are remaining graphics
 * instructions for this command that have not been appended into the ring
 * buffer yet
 * @cmd: the command gathering graphics instructions with the same op code
 *
 * RETURNS:
 * true if all graphics instructions (or rectangles) have already been appended
 * into the ring buffer, false otherwise.
 */
static inline bool
mchp_gfx2d_cmd_is_fully_scheduled(const struct mchp_gfx2d_command *cmd)
{
	return mchp_gfx2d_cmd_remaining_rects(cmd) == 0;
}

/**
 * mchp_gfx2d_set_surface - change a surface in the GFX2D hardware, if needed
 * @priv: the GFX2D device
 * @index: the surface index to be changed (MUST be in [0..2])
 * @gfx2d_obj: the GFX2D object to be set as a surface
 *
 * If the current surface @index is already fetched from the @gfx2d_obj object,
 * then this function does nothing. Otherwise, it appends LDR instructions into
 * the ring buffer to load the GFX2D_PAx, GFX2D_PITCHx and GFX2D_CFGx registers
 * (x being the @index value) with the proper values before executing further
 * graphics instructions.
 *
 * Called from mchp_gfx2d_run_command().
 */
static void mchp_gfx2d_set_surface(struct mchp_gfx2d_device *priv,
				   uint32_t index,
				   struct mchp_gfx2d_gem_object *gfx2d_obj)
{
	enum drm_mchp_gfx2d_pixel_format format = gfx2d_obj->format;
	u16 stride = gfx2d_obj->stride;

	if (priv->surfaces[index] == gfx2d_obj)
		return;

	drm_gem_object_get(&gfx2d_obj->base);

	drm_dbg(&priv->drm, "load object %u into surface %u\n",
		gfx2d_obj->id, index);

	mchp_gfx2d_ldr(priv, REG_PA(index), gfx2d_obj->dma_addr);
	mchp_gfx2d_ldr(priv, REG_PITCH(index), GFX2D_PITCH_DAT(stride));
	mchp_gfx2d_ldr(priv, REG_CFG(index), GFX2D_CFG_PF(format));
	mchp_gfx2d_trigger(priv);

	/*
	 * If the previous gfx2d_obj pointed by priv->surfaces[index] is
	 * still accessed by the GPU, it means at least some command still
	 * has a reference to it. Hence, we can safely release another
	 * reference to it here.
	 */
	mchp_gfx2d_gem_put(priv->surfaces[index]);
	priv->surfaces[index] = gfx2d_obj;
}

/**
 * mchp_gfx2d_intersection - compute the intersection of two rectangles
 * @a: the first rectangle
 * @b: the second rectangle
 * @result: the intersection rectangle, if not empty
 *
 * Called from mchp_gfx2d_clip_rect().
 *
 * RETURNS:
 * true if the resulting rectangle is not empty, false otherwise.
 */
static bool mchp_gfx2d_intersection(const struct drm_mchp_gfx2d_rectangle *a,
				    const struct drm_mchp_gfx2d_rectangle *b,
				    struct drm_mchp_gfx2d_rectangle *result)
{
	int min_x = max(a->x, b->x);
	int max_x = min(a->x + a->w, b->x + b->w);
	int min_y = max(a->y, b->y);
	int max_y = min(a->y + a->h, b->y + b->h);

	if (min_x >= max_x)
		return false;

	if (min_y >= max_y)
		return false;

	result->x = min_x;
	result->y = min_y;
	result->w = max_x - min_x;
	result->h = max_y - min_y;
	return true;
}

/**
 * mchp_gfx2d_clip_rect - clip a rectangle based on a command target and sources
 * @cmd: the command being prepared for execution
 * @r: the coordinates to be written into the WDx words of a graphics instruction
 * @rect: the rectangle coming from the submit ioctl
 * @num_sources: the number of sources to consider for this @cmd command
 *
 * This function clips the @rect rectangle computing its intersection with
 * the @cmd command target surface and all its source surfaces (the actual
 * sources taken into account depend on the @cmd command operation, hence on
 * @num_sources).
 *
 * If the intersection is not empty, then it fills the @r output parameter with
 * the proper coordinates to be written in the WDx words of the FILL, COPY,
 * BLEND or ROP instruction to be appened into the GFX2D ring buffer.
 *
 * This function is called from mchp_gfx2d_run_instructions().
 *
 * RESULTS:
 * true if the clipped rectangle is not empty and should be sent to the GFX2D
 * through its rinb buffer, false otherwise (meaning the rectangle will be
 * discarded).
 */
static bool mchp_gfx2d_clip_rect(const struct mchp_gfx2d_command *cmd,
				 struct mchp_gfx2d_rectangle *r,
				 const struct drm_mchp_gfx2d_rectangle *rect,
				 uint32_t num_sources)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj = cmd->target;
	struct drm_mchp_gfx2d_rectangle target, clip;
	uint32_t i;

	target.x = 0;
	target.y = 0;
	target.w = gfx2d_obj->width;
	target.h = gfx2d_obj->height;
	if (!mchp_gfx2d_intersection(rect, &target, &clip))
		return false;

	for (i = 0; i < num_sources; i++) {
		const struct mchp_gfx2d_source *src = &cmd->sources[i];
		struct drm_mchp_gfx2d_rectangle source;

		gfx2d_obj = src->gfx2d_obj;

		source.x = src->x;
		source.y = src->y;
		source.w = gfx2d_obj->width;
		source.h = gfx2d_obj->height;
		if (!mchp_gfx2d_intersection(&clip, &source, &clip))
			return false;
	}

	r->dst_x = clip.x;
	r->dst_y = clip.y;
	r->width = clip.w;
	r->height = clip.h;
	for (i = 0; i < num_sources; i++) {
		const struct mchp_gfx2d_source *src = &cmd->sources[i];

		r->src_x[i] = clip.x - src->x;
		r->src_y[i] = clip.y - src->y;
	}

	return true;
}

/**
 * mchp_gfx2d_fill - append a FILL instruction into the ring buffer
 * @priv: the GFX2D device
 * @cmd: the FILL command to be appended
 * @r: the coordinates to set into the FILL WD1 and WD2 words
 *
 * This function is called from mchp_gfx2d_run_instructions().
 */
static void mchp_gfx2d_fill(struct mchp_gfx2d_device *priv,
			    const struct mchp_gfx2d_command *cmd,
			    const struct mchp_gfx2d_rectangle *r)
{
	mchp_gfx2d_push(priv, GFX2D_WD0_OPCODE(GFX2D_OPCODE_FILL) |
			GFX2D_WD0_ARGS(2));
	mchp_gfx2d_push(priv, GFX2D_WD1_SIZE(r));
	mchp_gfx2d_push(priv, GFX2D_WD2_DPOS(r));
	mchp_gfx2d_push(priv, cmd->fill.color);
}

/**
 * mchp_gfx2d_copy - append a COPY instruction into the ring buffer
 * @priv: the GFX2D device
 * @cmd: the COPY command to be appended
 * @r: the coordinates to set into the COPY WD1, WD2, and WD3 words
 *
 * This function is called from mchp_gfx2d_run_instructions().
 */
static void mchp_gfx2d_copy(struct mchp_gfx2d_device *priv,
			    const struct mchp_gfx2d_command *cmd,
			    const struct mchp_gfx2d_rectangle *r)
{
	(void)cmd;

	mchp_gfx2d_push(priv, GFX2D_WD0_OPCODE(GFX2D_OPCODE_COPY) |
			GFX2D_WD0_ARGS(2));
	mchp_gfx2d_push(priv, GFX2D_WD1_SIZE(r));
	mchp_gfx2d_push(priv, GFX2D_WD2_DPOS(r));
	mchp_gfx2d_push(priv, GFX2D_WD3_SPOS(r));
}

/**
 * mchp_gfx2d_blend - append a BLEND instruction into the ring buffer
 * @priv: the GFX2D device
 * @cmd: the BLEND command to be appended
 * @r: the coordinates to set into the BLEND WD1, WD2, WD3 and WD4 words
 *
 * This function is called from mchp_gfx2d_run_instructions().
 */
static void mchp_gfx2d_blend(struct mchp_gfx2d_device *priv,
			     const struct mchp_gfx2d_command *cmd,
			     const struct mchp_gfx2d_rectangle *r)
{
	uint32_t word0, word5;

	word0 = GFX2D_WD0_OPCODE(GFX2D_OPCODE_BLEND) |
		GFX2D_WD0_REG(BLEND_SREG) |
		GFX2D_WD0_ARGS(4);
	if (priv->caps->has_dreg)
		word0 |= GFX2D_WD0_DREG(BLEND_DREG);

	word5 = GFX2D_WD5_DPRE(FIELD_GET(GFX2D_BLEND_DPRE, cmd->blend.flags)) |
		GFX2D_WD5_DAFACT(cmd->blend.dafact) |
		GFX2D_WD5_SPRE(FIELD_GET(GFX2D_BLEND_SPRE, cmd->blend.flags)) |
		GFX2D_WD5_SAFACT(cmd->blend.safact) |
		GFX2D_WD5_DCFACT(cmd->blend.dcfact) |
		GFX2D_WD5_SCFACT(cmd->blend.scfact) |
		GFX2D_WD5_FUNC(cmd->blend.func);

	mchp_gfx2d_push(priv, word0);
	mchp_gfx2d_push(priv, GFX2D_WD1_SIZE(r));
	mchp_gfx2d_push(priv, GFX2D_WD2_DPOS(r));
	mchp_gfx2d_push(priv, GFX2D_WD3_S0POS(r));
	mchp_gfx2d_push(priv, GFX2D_WD4_S1POS(r));
	mchp_gfx2d_push(priv, word5);
}

/**
 * mchp_gfx2d_rop - append a ROP instruction into the ring buffer
 * @priv: the GFX2D device
 * @cmd: the ROP command to be appended
 * @r: the coordinates to set into the ROP WD1, WD2, WD3 and WD4 words
 *
 * This function is called from mchp_gfx2d_run_instructions().
 */
static void mchp_gfx2d_rop(struct mchp_gfx2d_device *priv,
			   const struct mchp_gfx2d_command *cmd,
			   const struct mchp_gfx2d_rectangle *r)
{
	const struct mchp_gfx2d_gem_object *gfx2d_obj = cmd->mask;
	uint32_t word6;

	word6 = GFX2D_WD6_ROPM(cmd->rop.mode) |
		GFX2D_WD6_ROPH(cmd->rop.high) |
		GFX2D_WD6_ROPL(cmd->rop.low);

	mchp_gfx2d_push(priv, GFX2D_WD0_OPCODE(GFX2D_OPCODE_ROP) |
			GFX2D_WD0_ARGS(5));
	mchp_gfx2d_push(priv, GFX2D_WD1_SIZE(r));
	mchp_gfx2d_push(priv, GFX2D_WD2_DPOS(r));
	mchp_gfx2d_push(priv, GFX2D_WD3_S0POS(r));
	mchp_gfx2d_push(priv, GFX2D_WD4_S1POS(r));
	mchp_gfx2d_push(priv, gfx2d_obj ? gfx2d_obj->dma_addr : 0);
	mchp_gfx2d_push(priv, word6);
}

/**
 * mchp_gfx2d_run_instructions - append graphics instructions
 * @priv: the GFX2D device
 * @cmd: the command gathering all the rectangles to process
 * @num_rects: the exact number of rectangles to process
 *
 * This function appends up to @num_rects graphics instructions into the ring
 * buffer (one graphics instruction per rectangle). If @cmd has no more
 * rectangle to append then this function also appends the command epilogue.
 *
 * It triggers the GFX2D after each graphics instruction by updating its
 * GFX2D_HEAD register but sets the IE bit of the WD0 word only for the last
 * graphics instruction, hence limiting the number of interrupts when they are
 * enabled.
 *
 * It is called from mchp_gfx2d_run_command().
 */
static void mchp_gfx2d_run_instructions(struct mchp_gfx2d_device *priv,
					struct mchp_gfx2d_command *cmd,
					size_t num_rects)
{
	u32 num_sources;
	const struct drm_mchp_gfx2d_rectangle *rect;
	uint32_t *latest_command = NULL;
	void (*op_func)(struct mchp_gfx2d_device *priv,
			const struct mchp_gfx2d_command *cmd,
			const struct mchp_gfx2d_rectangle *r);
	size_t cnt = 0;

	switch (cmd->operation) {
	case DRM_MCHP_GFX2D_OP_FILL:
		num_sources = 0;
		op_func = mchp_gfx2d_fill;
		break;

	case DRM_MCHP_GFX2D_OP_COPY:
		num_sources = 1;
		op_func = mchp_gfx2d_copy;
		break;

	case DRM_MCHP_GFX2D_OP_BLEND:
		num_sources = 2;
		op_func = mchp_gfx2d_blend;
		break;

	case DRM_MCHP_GFX2D_OP_ROP:
		num_sources = 2;
		op_func = mchp_gfx2d_rop;
		break;

	default:
		/* Should not happen... */
		cmd->next_rect += num_rects;
		goto exit;
	}

	while (num_rects--) {
		struct mchp_gfx2d_rectangle r;

		rect = &cmd->rects[cmd->next_rect++];

		if (!mchp_gfx2d_clip_rect(cmd, &r, rect, num_sources))
			continue;

		if (latest_command)
			mchp_gfx2d_trigger(priv);

		latest_command = &priv->rb.data[priv->rb.head];
		op_func(priv, cmd, &r);

		cnt++;
	}

	if (latest_command) {
		*latest_command |= GFX2D_WD0_IE;
		mchp_gfx2d_trigger(priv);
	}

exit:
	drm_dbg(&priv->drm, "cmd %u: append %zu rectangle(s)\n", cmd->id, cnt);

	if (mchp_gfx2d_cmd_is_fully_scheduled(cmd))
		mchp_gfx2d_epilogue(priv, cmd);
}

/**
 * mchp_gfx2d_run_command - append GFX2D instructions into its ring buffer
 * @priv: the GFX2D device
 * @cmd: the command gathering all the rectangles to process
 * @num_rects: the exact number of rectangles to process
 *
 * This function appends LDRS instructions into the ring buffer to set the surfaces
 * as needed in the GFX2D hardware, before appending the graphics instructions to
 * follow by calling mchp_gfx2d_run_instructions().
 *
 * It is called from either mchp_gfx2d_run_pending_commands() or
 * mchp_gfx2d_schedule(), with the 'priv->cmdlist_mutex' held in both cases.
 */
static void mchp_gfx2d_run_command(struct mchp_gfx2d_device *priv,
				   struct mchp_gfx2d_command *cmd,
				   size_t num_rects)
{
	/* Append all instructions for the command prologue, if needed. */
	mchp_gfx2d_set_surface(priv, 0, cmd->target);

	switch (cmd->operation) {
	case DRM_MCHP_GFX2D_OP_FILL:
		break;

	case DRM_MCHP_GFX2D_OP_COPY:
		mchp_gfx2d_set_surface(priv, 1, cmd->sources[0].gfx2d_obj);
		break;

	case DRM_MCHP_GFX2D_OP_BLEND:
		mchp_gfx2d_set_surface(priv, 1, cmd->sources[0].gfx2d_obj);
		mchp_gfx2d_set_surface(priv, 2, cmd->sources[1].gfx2d_obj);
		if (cmd->blend.flags & GFX2D_BLEND_SET_DST_COLOR) {
			enum mchp_gfx2d_reg_id reg = priv->caps->has_dreg ?
				BLEND_DREG :
				BLEND_SREG;
			mchp_gfx2d_ldr(priv, reg, cmd->blend.dst_color);
			mchp_gfx2d_trigger(priv);
			cmd->blend.flags &= ~GFX2D_BLEND_SET_DST_COLOR;
		}
		if (cmd->blend.flags & GFX2D_BLEND_SET_SRC_COLOR) {
			mchp_gfx2d_ldr(priv, BLEND_SREG, cmd->blend.src_color);
			mchp_gfx2d_trigger(priv);
			cmd->blend.flags &= ~GFX2D_BLEND_SET_SRC_COLOR;
		}
		break;

	case DRM_MCHP_GFX2D_OP_ROP:
		mchp_gfx2d_set_surface(priv, 1, cmd->sources[0].gfx2d_obj);
		mchp_gfx2d_set_surface(priv, 2, cmd->sources[1].gfx2d_obj);
		break;

	default:
		/* Should not happen... */
		cmd->next_rect += num_rects;
		return;
	}

	/* Then append graphics instructions and eventually the epilogue. */
	mchp_gfx2d_run_instructions(priv, cmd, num_rects);
}

/**
 * mchp_gfx2d_cmd_instructions_have_completed - tell whether all instructions
 *     already appended into the ring buffer for this command have completed
 * @priv: the GFX2D device
 * @cmd: the command gathering graphics instructions with the same op code.
 *
 * RETURNS:
 * true if all instructions already appended into the ring buffer for this
 * command have completed, false otherwise.
 */
static bool
mchp_gfx2d_cmd_instructions_have_completed(const struct mchp_gfx2d_device *priv,
					   const struct mchp_gfx2d_command *cmd)
{
	uint32_t origin = priv->cpu_latest_completed_cmd;
	uint32_t gpu_id = *priv->mbx; /* GPU latest completed cmd. */

	drm_dbg(&priv->drm, "cmd %u: gpu_id = %u, origin = %u\n",
		cmd->id, gpu_id, origin);

	return (cmd->id - origin) <= (gpu_id - origin);
}

/**
 * mchp_gfx2d_cmd_has_completed - tell whether all graphics instructions have completed
 * @priv: the GFX2D device
 * @cmd: the command gathering graphics instructions with the same op code.
 *
 * This function is called from mchp_gfx2d_process_completed_commands(), with
 * the 'priv->cmdlist_mutex' held.
 *
 * RETURNS:
 * true if all graphics instructions (or rectangles) have completed, false otherwise.
 */
static bool mchp_gfx2d_cmd_has_completed(const struct mchp_gfx2d_device *priv,
					 const struct mchp_gfx2d_command *cmd)
{
	return mchp_gfx2d_cmd_is_fully_scheduled(cmd) &&
		mchp_gfx2d_cmd_instructions_have_completed(priv, cmd);
}

/**
 * mchp_gfx2d_process_completed_commands - release all completed commands
 * @priv: the GFX2D device
 *
 * This function iterates on the running list, releasing completed commands and
 * stops on the very first command that has not completed yet.
 *
 * It is called from mchp_gfx2d_progress(), with the 'priv->cmdlist_mutex'
 * unlocked as it locks itself this mutex.
 *
 * RETURNS:
 * true if at least one command has completed then has been released.
 */
static bool mchp_gfx2d_process_completed_commands(struct mchp_gfx2d_device *priv)
{
	struct mchp_gfx2d_command *cmd, *tmp;
	struct device *dev = priv->drm.dev;
	bool did_something = false;

	mutex_lock(&priv->cmdlist_mutex);

	list_for_each_entry_safe(cmd, tmp, &priv->running_cmdlist, cmd_node) {
		if (!mchp_gfx2d_cmd_has_completed(priv, cmd))
			break;

		drm_dbg(&priv->drm, "cmd %u: command has completed\n", cmd->id);
		priv->cpu_latest_completed_cmd = cmd->id;

		list_del(&cmd->cmd_node);
		pm_runtime_put_noidle(dev);

		mchp_gfx2d_release_command(priv, cmd, true);
		did_something = true;
	}

	if (priv->is_suspended) {
		if (list_empty(&priv->running_cmdlist)) {
			complete(&priv->running_cmdlist_empty);
			/*
			 * Prevent mchp_gfx2d_thread() from calling
			 * mchp_gfx2d_progress() again, unless
			 * mchp_gfx2d_run_pending_commands() returns true just
			 * after.
			 */
			did_something = false;
		} else {
			mchp_gfx2d_enable_exend(priv);
		}
	}

	mutex_unlock(&priv->cmdlist_mutex);

	return did_something;
}

/**
 * mchp_gfx2d_rooms_for_rects - tell how many rectangles can be appended into
 * the ring buffer
 * @priv: the GFX2D device
 * @cmd: the command gathering graphics instructions with the same op code.
 *
 * This function MUST be called before calling mchp_gfx2d_run_command() the verify
 * there is enough space in the ring buffer before appending more instructions.
 *
 * It is called from either mchp_gfx2d_run_pending_command() or
 * mchp_gfx2d_schedule(), with the 'priv->cmdlist_mutex' held in both cases.
 *
 * RETURNS:
 * the number of rectangles that can be appended by mchp_gfx2d_run_command().
 */
static size_t mchp_gfx2d_rooms_for_rects(const struct mchp_gfx2d_device *priv,
					 const struct mchp_gfx2d_command *cmd)
{
	size_t remaining_rects = mchp_gfx2d_cmd_remaining_rects(cmd);
	size_t prologue_words = 0;
	size_t nb_words_per_rect;
	size_t available;
	u32 tail;

	if (!remaining_rects)
		return 0;

	if (priv->surfaces[0] != cmd->target)
		prologue_words += MCHP_GFX2D_NB_WORDS_SET_SURFACE;

	switch (cmd->operation) {
	case DRM_MCHP_GFX2D_OP_FILL:
		nb_words_per_rect = MCHP_GFX2D_NB_WORDS_FILL;
		break;

	case DRM_MCHP_GFX2D_OP_COPY:
		nb_words_per_rect = MCHP_GFX2D_NB_WORDS_COPY;
		if (priv->surfaces[1] != cmd->sources[0].gfx2d_obj)
			prologue_words += MCHP_GFX2D_NB_WORDS_SET_SURFACE;
		break;

	case DRM_MCHP_GFX2D_OP_BLEND:
		nb_words_per_rect = MCHP_GFX2D_NB_WORDS_BLEND;
		if (cmd->blend.flags & GFX2D_BLEND_SET_DST_COLOR)
			prologue_words += MCHP_GFX2D_NB_WORDS_LDR;
		if (cmd->blend.flags & GFX2D_BLEND_SET_SRC_COLOR)
			prologue_words += MCHP_GFX2D_NB_WORDS_LDR;
		if (priv->surfaces[1] != cmd->sources[0].gfx2d_obj)
			prologue_words += MCHP_GFX2D_NB_WORDS_SET_SURFACE;
		if (priv->surfaces[2] != cmd->sources[1].gfx2d_obj)
			prologue_words += MCHP_GFX2D_NB_WORDS_SET_SURFACE;
		break;

	case DRM_MCHP_GFX2D_OP_ROP:
		nb_words_per_rect = MCHP_GFX2D_NB_WORDS_ROP;
		if (priv->surfaces[1] != cmd->sources[0].gfx2d_obj)
			prologue_words += MCHP_GFX2D_NB_WORDS_SET_SURFACE;
		if (priv->surfaces[2] != cmd->sources[1].gfx2d_obj)
			prologue_words += MCHP_GFX2D_NB_WORDS_SET_SURFACE;
		break;

	default:
		/* Should not happen... */
		return 0;
	}

	tail = readl(priv->regs + GFX2D_TAIL);
	available = CIRC_SPACE(priv->rb.head, tail, priv->rb.size);

	/* The prologue should not be appended alone, without any rect. */
	if (prologue_words + nb_words_per_rect > available)
		return 0;

	if ((prologue_words +
	     remaining_rects * nb_words_per_rect +
	     MCHP_GFX2D_NB_WORDS_EPILOGUE) <= available)
		return remaining_rects;

	/* The epilogue should not be appended alone, without any rect. */
	return min(remaining_rects - 1,
		   (available - prologue_words) / nb_words_per_rect);
}

/**
 * mchp_gfx2d_run_pending_commands - execute as much commands as possible
 * @priv: the GFX2D device
 *
 * This function resumes the partially running command, if any, then
 * move commands from the pending into the running list.
 *
 * It MUST be called with the 'priv->cmdlist_mutex' unlocked as it locks itself
 * this mutex.
 *
 * RETURNS:
 * true if at least one command has progressed, meaning some graphics instructions
 * have been appended into the GFX2D ring buffer.
 */
static bool mchp_gfx2d_run_pending_commands(struct mchp_gfx2d_device *priv)
{
	struct mchp_gfx2d_command *cmd, *tmp;
	struct device *dev = priv->drm.dev;
	bool did_something = false;
	size_t num_rects;

	mutex_lock(&priv->cmdlist_mutex);

	/* Only the latest running command may be partial. */
	if (!list_empty(&priv->running_cmdlist)) {
		cmd = list_last_entry(&priv->running_cmdlist,
				      typeof(*cmd), cmd_node);

		num_rects = mchp_gfx2d_rooms_for_rects(priv, cmd);
		if (num_rects) {
			did_something = true;
			mchp_gfx2d_run_command(priv, cmd, num_rects);
			drm_dbg(&priv->drm, "cmd %u: make command progress\n",
				cmd->id);
		}

		if (!mchp_gfx2d_cmd_is_fully_scheduled(cmd)) {
			/*
			 * If the latest running command is not yet
			 * fully scheduled, then we can't move any other
			 * command from the pending into the running
			 * list.
			 */
			goto unlock;
		}
	}

	/*
	 * If being suspended, don't move any more command from the pending list
	 * into the running list.
	 */
	if (priv->is_suspended)
		goto unlock;

	list_for_each_entry_safe(cmd, tmp, &priv->pending_cmdlist, cmd_node) {
		struct list_head *node = &cmd->cmd_node;

		num_rects = mchp_gfx2d_rooms_for_rects(priv, cmd);
		if (!num_rects)
			break;

		drm_dbg(&priv->drm,
			"cmd %u: move command from pending to running list\n",
			cmd->id);

		list_del(node);
		list_add_tail(node, &priv->running_cmdlist);
		pm_runtime_get_noresume(dev);

		did_something = true;
		mchp_gfx2d_run_command(priv, cmd, num_rects);

		if (!mchp_gfx2d_cmd_is_fully_scheduled(cmd))
			break;
	}

unlock:
	mutex_unlock(&priv->cmdlist_mutex);

	return did_something;
}

/**
 * mchp_gfx2d_blend_cfact - set some Color Factor of a command
 * @priv: the GFX2D device
 * @factor: the blend factor coming from the submit ioctl arguments
 * @fact: the SCFACT or DCFACT bitfield value to write into the WD5 word of the
 *        BLEND command.
 * @is_source: if true, @fact will be written into SCFACT, otherwise into DCFACT
 *
 * RETURNS:
 * 0 if the color factor is valid, -EINVAL otherwise.
 */
static int mchp_gfx2d_blend_cfact(struct mchp_gfx2d_device *priv,
				  enum drm_mchp_gfx2d_blend_factor factor,
				  u8 *fact,
				  bool is_source)
{
	switch (factor) {
	case DRM_MCHP_GFX2D_BFACTOR_ZERO:
		*fact = 0;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE:
		*fact = 1;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_SRC_COLOR:
		*fact = 2;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_SRC_COLOR:
		*fact = 3;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_DST_COLOR:
		*fact = 4;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_DST_COLOR:
		*fact = 5;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_SRC_ALPHA:
		*fact = 6;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_SRC_ALPHA:
		*fact = 7;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_DST_ALPHA:
		*fact = 8;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_DST_ALPHA:
		*fact = 9;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_CONSTANT_COLOR:
		*fact = 10;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_COLOR:
		*fact = 11;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_CONSTANT_ALPHA:
		*fact = 12;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_ALPHA:
		*fact = 13;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_SRC_ALPHA_SATURATE:
		*fact = 14;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * mchp_gfx2d_blend_scfact - set the Source Color Factor of a command
 * @priv: the GFX2D device
 * @factor: the blend factor coming from the submit ioctl arguments
 * @fact: the SCFACT bitfield value to write into the WD5 word of the BLEND
 *        command.
 *
 * RETURNS:
 * 0 if the source color factor is valid, -EINVAL otherwise.
 */
static inline int
mchp_gfx2d_blend_scfact(struct mchp_gfx2d_device *priv,
			enum drm_mchp_gfx2d_blend_factor factor, u8 *fact)
{
	return mchp_gfx2d_blend_cfact(priv, factor, fact, true);
}

/**
 * mchp_gfx2d_blend_dcfact - set the Destination Color Factor of a command
 * @priv: the GFX2D device
 * @factor: the blend factor coming from the submit ioctl arguments
 * @fact: the DCFACT bitfield value to write into the WD5 word of the BLEND
 *        command.
 *
 * RETURNS:
 * 0 if the destination color factor is valid, -EINVAL otherwise.
 */
static inline int
mchp_gfx2d_blend_dcfact(struct mchp_gfx2d_device *priv,
			enum drm_mchp_gfx2d_blend_factor factor, u8 *fact)
{
	return mchp_gfx2d_blend_cfact(priv, factor, fact, false);
}

/**
 * mchp_gfx2d_blend_afact - set some Alpha Factor of a command
 * @priv: the GFX2D device
 * @factor: the blend factor coming from the submit ioctl arguments
 * @fact: the SAFACT or DAFACT bitfield value to write into the WD5 word of the
 *        BLEND command.
 * @is_source: if true, @fact will be written into SAFACT, otherwise into DAFACT
 *
 * RETURNS:
 * 0 if the alpha factor is valid, -EINVAL otherwise.
 */
static int mchp_gfx2d_blend_afact(struct mchp_gfx2d_device *priv,
				  enum drm_mchp_gfx2d_blend_factor factor,
				  u8 *fact,
				  bool is_source)
{
	switch (factor) {
	case DRM_MCHP_GFX2D_BFACTOR_ZERO:
		*fact = 0;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE:
		*fact = 1;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_SRC_ALPHA:
		*fact = 2;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_SRC_ALPHA:
		*fact = 3;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_DST_ALPHA:
		*fact = 4;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_DST_ALPHA:
		*fact = 5;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_CONSTANT_ALPHA:
		*fact = 6;
		break;

	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_ALPHA:
		*fact = 7;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * mchp_gfx2d_blend_safact - set the Source Alpha Factor of a command
 * @priv: the GFX2D device
 * @factor: the blend factor coming from the submit ioctl arguments
 * @fact: the SAFACT bitfield value to write into the WD5 word of the BLEND
 *        command.
 *
 * RETURNS:
 * 0 if the source alpha factor is valid, -EINVAL otherwise.
 */
static inline int
mchp_gfx2d_blend_safact(struct mchp_gfx2d_device *priv,
			enum drm_mchp_gfx2d_blend_factor factor, u8 *fact)
{
	return mchp_gfx2d_blend_afact(priv, factor, fact, true);
}

/**
 * mchp_gfx2d_blend_dafact - set the Destination Alpha Factor of a command
 * @priv: the GFX2D device
 * @factor: the blend factor coming from the submit ioctl arguments
 * @fact: the DAFACT bitfield value to write into the WD5 word of the BLEND
 *        command.
 *
 * RETURNS:
 * 0 if the destination alpha factor is valid, -EINVAL otherwise.
 */
static inline int
mchp_gfx2d_blend_dafact(struct mchp_gfx2d_device *priv,
			enum drm_mchp_gfx2d_blend_factor factor, u8 *fact)
{
	return mchp_gfx2d_blend_afact(priv, factor, fact, false);
}

/**
 * mchp_gfx2d_set_blend_factors - initialize the blend factors of a command
 * @priv: the GFX2D device
 * @args: the submit ioctl arguments for the new submitted command
 * @cmd: the command to initialize before scheduling
 *
 * This function initializes the blend factors of the @cmd command from the
 * @args submit ioctl parameters.
 *
 * RETURNS:
 * 0 if all blend factors are valid, -EINVAL otherwise.
 */
static int
mchp_gfx2d_set_blend_factors(struct mchp_gfx2d_device *priv,
			     const struct drm_mchp_gfx2d_submit *args,
			     struct mchp_gfx2d_command *cmd)
{
	int ret;

	ret = mchp_gfx2d_blend_scfact(priv, args->blend.scfactor,
				      &cmd->blend.scfact);
	if (ret)
		return ret;

	ret = mchp_gfx2d_blend_dcfact(priv, args->blend.dcfactor,
				      &cmd->blend.dcfact);
	if (ret)
		return ret;

	ret = mchp_gfx2d_blend_safact(priv, args->blend.safactor,
				      &cmd->blend.safact);
	if (ret)
		return ret;

	return mchp_gfx2d_blend_dafact(priv, args->blend.dafactor,
				       &cmd->blend.dafact);
}

/**
 * mchp_gfx2d_blend_factor_uses_constant - tell whether the @factor refers to a
 * constant color.
 * @factor: A blend factor
 *
 * RETURNS:
 * true if the blend factor implies the use of a constant color.
 */
static bool
mchp_gfx2d_blend_factor_uses_constant(enum drm_mchp_gfx2d_blend_factor factor)
{
	switch (factor) {
	case DRM_MCHP_GFX2D_BFACTOR_CONSTANT_COLOR:
	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_COLOR:
	case DRM_MCHP_GFX2D_BFACTOR_CONSTANT_ALPHA:
	case DRM_MCHP_GFX2D_BFACTOR_ONE_MINUS_CONSTANT_ALPHA:
		return true;
	default:
		break;
	}

	return false;
}

/**
 * mchp_gfx2d_set_blend_params - initialize the blend parameters of a command
 * @priv: the GFX2D device
 * @args: the submit ioctl arguments for the new submitted command
 * @cmd: the command to initialize before scheduling
 *
 * This function initializes the blend parameters of the @cmd command from the
 * @args submit ioctl parameters.
 *
 * RETURNS:
 * 0 if all blend parameters are valid, -EINVAL otherwise.
 */
static int
mchp_gfx2d_set_blend_params(struct mchp_gfx2d_device *priv,
			    const struct drm_mchp_gfx2d_submit *args,
			    struct mchp_gfx2d_command *cmd)
{
	bool src_c, dst_c, spre, dpre;
	int ret;

	if (args->blend.flags & ~(DRM_MCHP_GFX2D_BLEND_DPRE |
				  DRM_MCHP_GFX2D_BLEND_SPRE))
		return -EINVAL;

	ret = mchp_gfx2d_set_blend_factors(priv, args, cmd);
	if (ret)
		return ret;

	cmd->blend.func = (u8)args->blend.function;
	if (cmd->blend.func > DRM_MCHP_GFX2D_BFUNC_SPE)
		return -EINVAL;

	spre = FIELD_GET(DRM_MCHP_GFX2D_BLEND_SPRE, args->blend.flags);
	dpre = FIELD_GET(DRM_MCHP_GFX2D_BLEND_DPRE, args->blend.flags);

	cmd->blend.src_color = args->blend.src_color;
	src_c = spre ||
		mchp_gfx2d_blend_factor_uses_constant(args->blend.scfactor) ||
		mchp_gfx2d_blend_factor_uses_constant(args->blend.safactor);

	cmd->blend.dst_color = args->blend.dst_color;
	dst_c = dpre ||
		mchp_gfx2d_blend_factor_uses_constant(args->blend.dcfactor) ||
		mchp_gfx2d_blend_factor_uses_constant(args->blend.dafactor);

	cmd->blend.flags = FIELD_PREP(GFX2D_BLEND_SET_SRC_COLOR, src_c) |
		FIELD_PREP(GFX2D_BLEND_SET_DST_COLOR, dst_c) |
		FIELD_PREP(GFX2D_BLEND_SPRE, spre) |
		FIELD_PREP(GFX2D_BLEND_DPRE, dpre);

	return 0;
}

/**
 * mchp_gfx2d_set_rop_params - initialize the rop parameters of a command
 * @priv: the GFX2D device
 * @file_priv: the DRM file-private structure
 * @args: the submit ioctl arguments for the new submitted command
 * @cmd: the command to initialize before scheduling
 *
 * This function initializes the rop parameters of the @cmd command from the
 * @args submit ioctl parameters.
 *
 * RETURNS:
 * 0 if all rop parameters are valid, an error code otherwise.
 */
static int
mchp_gfx2d_set_rop_params(struct mchp_gfx2d_device *priv,
			  struct drm_file *file_priv,
			  const struct drm_mchp_gfx2d_submit *args,
			  struct mchp_gfx2d_command *cmd)
{
	uint32_t handle = args->rop.mask_handle;

	cmd->rop.mode = (u8)args->rop.mode;
	if (cmd->rop.mode > DRM_MCHP_GFX2D_ROP4)
		return -EINVAL;

	if (args->rop.mode == DRM_MCHP_GFX2D_ROP2 && args->rop.low > 15)
		return -EINVAL;

	if (((args->rop.mode == DRM_MCHP_GFX2D_ROP4) && !handle) ||
	    ((args->rop.mode != DRM_MCHP_GFX2D_ROP4) && handle))
		return -EINVAL;

	if (handle) {
		cmd->mask = mchp_gfx2d_gem_addref(file_priv, handle);
		if (!cmd->mask)
			return -ENOENT;

		drm_dbg(&priv->drm,
			"cmd %u: add ref to object %u (%d) as mask\n",
			cmd->id,
			cmd->mask->id, atomic_read(&cmd->mask->gpu_active));
	}

	cmd->rop.high = args->rop.high;
	cmd->rop.low = args->rop.low;

	return 0;
}

/**
 * mchp_gfx2d_alloc_command - get a free command to be scheduled
 * @priv: the GFX2D device
 *
 * This function first tries to get a free command from the 'priv->free_cmdlist'
 * list, then allocated a new command from 'priv->cmd_cache'.
 *
 * It is called from @mchp_gfx2d_submit() and MAY be called with
 * 'priv->cmdlist_mutex' held.
 *
 * RETURNS:
 * Pointer to newly allocated command, NULL on failure.
 */
static struct mchp_gfx2d_command *
mchp_gfx2d_alloc_command(struct mchp_gfx2d_device *priv)
{
	static uint32_t next_id;
	struct mchp_gfx2d_command *cmd = NULL;

	mutex_lock(&priv->free_cmdlist_mutex);
	if (!list_empty(&priv->free_cmdlist)) {
		cmd = list_first_entry(&priv->free_cmdlist,
				       typeof(*cmd), cmd_node);
		list_del(&cmd->cmd_node);
	}
	mutex_unlock(&priv->free_cmdlist_mutex);

	if (!cmd)
		cmd = kmem_cache_alloc(priv->cmd_cache, GFP_KERNEL);
	if (!cmd)
		return NULL;

	memset(cmd, 0, sizeof(*cmd));
	cmd->id = next_id++;

	return cmd;
}

/**
 * mchp_gfx2d_has_pending_commands_locked - check whether some commands are
 *     waiting for being executed
 * @priv: the GFX2D device
 *
 * This function MUST be called with 'priv->cmdlist_mutex' held, otherwise
 * call mchp_gfx2d_has_pending_commands() instead.
 *
 * RETURNS:
 * true if there is at least one pending command, otherwise false.
 */
static bool
mchp_gfx2d_has_pending_commands_locked(struct mchp_gfx2d_device *priv)
{
	if (!list_empty(&priv->running_cmdlist)) {
		struct mchp_gfx2d_command *cmd;

		cmd = list_last_entry(&priv->running_cmdlist,
				      typeof(*cmd), cmd_node);
		if (!mchp_gfx2d_cmd_is_fully_scheduled(cmd))
			return true;
	}

	return !list_empty(&priv->pending_cmdlist);
}

/**
 * mchp_gfx2d_schedule - insert @cmd into the relevant queue
 * @priv: the GFX2D device
 * @cmd: the new command to schedule
 *
 * This function inserts the @cmd command into the running list if possible,
 * otherwise into the pending list.
 *
 * It is called from mchp_gfx2d_submit(), hence with 'priv->cmdlist_mutex'
 * unlocked so it can lock this mutex itself.
 *
 * RETURNS:
 * 0 if successful, otherwise an error code.
 */
static int mchp_gfx2d_schedule(struct mchp_gfx2d_device *priv,
			       struct mchp_gfx2d_command *cmd)
{
	struct device *dev = priv->drm.dev;
	size_t num_rects;
	bool to_pending;
	int ret = 0;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	(void)mchp_gfx2d_progress(priv);

	mutex_lock(&priv->cmdlist_mutex);

	to_pending = mchp_gfx2d_has_pending_commands_locked(priv);
	if (!to_pending) {
		num_rects = mchp_gfx2d_rooms_for_rects(priv, cmd);
		to_pending = !num_rects;
	}

	if (to_pending) {
		ret = mchp_gfx2d_dup_rectangles(cmd);
		if (!ret) {
			/* Enqueue this command into the pending list. */
			list_add_tail(&cmd->cmd_node, &priv->pending_cmdlist);
			drm_dbg(&priv->drm,
				"cmd %u: enqueue command into pending list\n",
				cmd->id);

			mchp_gfx2d_enable_exend(priv);
		}
		goto unlock;
	}

	/*
	 * If here, there is no previous command in the queue waiting for
	 * being executed and 'cmd' can be executed, at least partially.
	 *
	 * So, if not all instructions can fit into the ring buffer, then
	 * duplicate the array of rectangles if needed; that is to say if the
	 * current rectangle array is 'cmd->ctx->rectangles'.
	 */
	if (num_rects < mchp_gfx2d_cmd_remaining_rects(cmd)) {
		ret = mchp_gfx2d_dup_rectangles(cmd);
		if (ret)
			goto unlock;
	}

	/* Fill the ring buffer with some, if not all, command instructions. */
	mchp_gfx2d_run_command(priv, cmd, num_rects);

	/* All scheduled; cmd->ctx->rectangles' is no longer needed for sure. */
	if (mchp_gfx2d_cmd_is_fully_scheduled(cmd) && !cmd->can_free_rects)
		cmd->rects = NULL;

	/* Enqueue this command into the running list. */
	list_add_tail(&cmd->cmd_node, &priv->running_cmdlist);
	pm_runtime_get_noresume(dev);

	drm_dbg(&priv->drm, "cmd %u: enqueue command into running list\n",
		cmd->id);

unlock:
	mutex_unlock(&priv->cmdlist_mutex);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/**
 * mchp_gfx2d_init_command_queue - init the command queue and the ring buffer
 * @priv: the GFX2D device
 *
 * This function is called from mchp_gfx2d_probe().
 *
 * RETURNS:
 * 0 on success, -ENOMEM otherwise.
 */
int mchp_gfx2d_init_command_queue(struct mchp_gfx2d_device *priv)
{
	struct device *dev = priv->drm.dev;
	int ret;

	init_completion(&priv->running_cmdlist_empty);

	mutex_init(&priv->wlist_mutex);
	INIT_LIST_HEAD(&priv->wlist);

	mutex_init(&priv->free_cmdlist_mutex);
	INIT_LIST_HEAD(&priv->free_cmdlist);

	mutex_init(&priv->cmdlist_mutex);
	INIT_LIST_HEAD(&priv->pending_cmdlist);
	INIT_LIST_HEAD(&priv->running_cmdlist);

	priv->cmd_cache = kmem_cache_create("gfx2d_cmd_cache",
					    sizeof(struct mchp_gfx2d_command),
					    0, 0, NULL);
	if (!priv->cmd_cache) {
		dev_err(dev, "failed to create kmem cache for commands\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	priv->rb.size_in_bytes = PAGE_SIZE;
	priv->rb.size = priv->rb.size_in_bytes / sizeof(uint32_t);
	priv->rb.data = dma_alloc_wc(dev, priv->rb.size_in_bytes,
				     &priv->rb_dma_addr, GFP_KERNEL);
	if (!priv->rb.data) {
		dev_err(dev, "failed to allocate memory for the command ring buffer\n");
		ret = -ENOMEM;
		goto err_destroy_cache;
	}
	priv->rb.head = 0;

	priv->mbx = dma_alloc_coherent(dev, sizeof(uint32_t),
				       &priv->mbx_dma_addr, GFP_KERNEL);
	if (!priv->mbx) {
		ret = -ENOMEM;
		goto err_free_rb;
	}

	writel(GFX2D_GD_DISABLE, priv->regs + GFX2D_GD);
	writel(0, priv->regs + GFX2D_HEAD);
	writel(0, priv->regs + GFX2D_TAIL);
	writel((u32)priv->rb_dma_addr, priv->regs + GFX2D_BASE);
	writel((priv->rb.size_in_bytes >> 8) - 1, priv->regs + GFX2D_LEN);
	writel(GFX2D_GE_ENABLE, priv->regs + GFX2D_GE);

	mchp_gfx2d_ldr(priv, MBX_ADDR_REG, (u32)priv->mbx_dma_addr);
	mchp_gfx2d_trigger(priv);

	priv->cpu_latest_completed_cmd = ~0;
	*((uint32_t *)priv->mbx) = priv->cpu_latest_completed_cmd;

	return 0;

err_free_rb:
	dma_free_wc(dev, priv->rb.size_in_bytes,
		    (void *)priv->rb.data, priv->rb_dma_addr);
err_destroy_cache:
	kmem_cache_destroy(priv->cmd_cache);
err_exit:
	return ret;
}

/**
 * mchp_gfx2d_cleanup_command_queue - stop the GFX2D and deinit the command queue
 * @priv: the GFX2D device
 *
 * This function is called from mchp_gfx2d_remove() and from mchp_gfx2d_probe() if
 * an error occurred.
 */
void mchp_gfx2d_cleanup_command_queue(struct mchp_gfx2d_device *priv)
{
	struct device *dev = priv->drm.dev;
	struct mchp_gfx2d_command *cmd, *tmp;
	uint32_t i;

	writel(GFX2D_IRQ_EXEND, priv->regs + GFX2D_ID);
	writel(GFX2D_GD_DISABLE, priv->regs + GFX2D_GD);

	mutex_lock(&priv->cmdlist_mutex);
	list_for_each_entry_safe(cmd, tmp, &priv->running_cmdlist, cmd_node) {
		list_del(&cmd->cmd_node);
		mchp_gfx2d_release_command(priv, cmd, false);
		kmem_cache_free(priv->cmd_cache, cmd);
	}

	list_for_each_entry_safe(cmd, tmp, &priv->pending_cmdlist, cmd_node) {
		list_del(&cmd->cmd_node);
		mchp_gfx2d_release_command(priv, cmd, false);
		kmem_cache_free(priv->cmd_cache, cmd);
	}
	mutex_unlock(&priv->cmdlist_mutex);

	mutex_lock(&priv->free_cmdlist_mutex);
	list_for_each_entry_safe(cmd, tmp, &priv->free_cmdlist, cmd_node) {
		list_del(&cmd->cmd_node);
		mchp_gfx2d_release_command(priv, cmd, false);
		kmem_cache_free(priv->cmd_cache, cmd);
	}
	mutex_unlock(&priv->free_cmdlist_mutex);

	for (i = 0; i < ARRAY_SIZE(priv->surfaces); i++)
		mchp_gfx2d_gem_put(priv->surfaces[i]);

	dma_free_coherent(dev, sizeof(uint32_t),
			  (void *)priv->mbx, priv->mbx_dma_addr);
	dma_free_wc(dev, priv->rb.size_in_bytes,
		    (void *)priv->rb.data, priv->rb_dma_addr);
	kmem_cache_destroy(priv->cmd_cache);
}

/**
 * mchp_gfx2d_submit - enqueue a new command gathering graphics instructions
 * @priv: the GFX2D device
 * @file_priv: the DRM file-private structure
 * @args: the submit ioctl arguments
 *
 * All graphics instructions of this new command share the same op code (FILL,
 * COPY, BLEND or ROP) and the same surfaces.
 *
 * This function is called from mchp_gfx2d_ioctl_submit().
 *
 * RETURNS:
 * 0 on success, an error code otherwise.
 */
int mchp_gfx2d_submit(struct mchp_gfx2d_device *priv,
		      struct drm_file *file_priv,
		      const struct drm_mchp_gfx2d_submit *args)
{
	struct mchp_gfx2d_file *ctx = file_priv->driver_priv;
	struct mchp_gfx2d_command *cmd;
	size_t i, num_sources;
	int ret;

	if (!args->num_rectangles)
		return -EINVAL;

	cmd = mchp_gfx2d_alloc_command(priv);
	if (!cmd)
		return -ENOMEM;

	cmd->ctx = ctx;
	cmd->operation = (u8)args->operation;
	drm_dbg(&priv->drm, "cmd %u: allocate command (operation = %u)\n",
		cmd->id, cmd->operation);

	ret = mchp_gfx2d_alloc_rectangles(cmd, args->num_rectangles);
	if (ret)
		goto err_release_command;

	ret = copy_from_user(cmd->rects, u64_to_user_ptr(args->rectangles),
			     args->num_rectangles * sizeof(*cmd->rects));
	if (ret) {
		ret = -EFAULT;
		goto err_release_command;
	}
	drm_dbg(&priv->drm, "cmd %u: copy %zu rectangle(s)\n",
		cmd->id, cmd->num_rects);

	cmd->target = mchp_gfx2d_gem_addref(file_priv, args->target_handle);
	if (!cmd->target) {
		ret = -ENOENT;
		goto err_release_command;
	}
	drm_dbg(&priv->drm,
		"cmd %u: add ref to object %u (%d) as target\n",
		cmd->id,
		cmd->target->id, atomic_read(&cmd->target->gpu_active));

	switch (cmd->operation) {
	case DRM_MCHP_GFX2D_OP_FILL:
		num_sources = 0;
		cmd->fill.color = args->fill.color;
		break;

	case DRM_MCHP_GFX2D_OP_COPY:
		num_sources = 1;
		break;

	case DRM_MCHP_GFX2D_OP_BLEND:
		num_sources = 2;
		ret = mchp_gfx2d_set_blend_params(priv, args, cmd);
		if (ret)
			goto err_release_command;
		break;

	case DRM_MCHP_GFX2D_OP_ROP:
		num_sources = 2;
		ret = mchp_gfx2d_set_rop_params(priv, file_priv, args, cmd);
		if (ret)
			goto err_release_command;
		break;

	default:
		ret = -EINVAL;
		goto err_release_command;
	}

	for (i = 0; i < num_sources; i++) {
		const struct drm_mchp_gfx2d_source *from = &args->sources[i];
		struct mchp_gfx2d_source *to = &cmd->sources[i];

		to->gfx2d_obj = mchp_gfx2d_gem_addref(file_priv, from->handle);
		if (!to->gfx2d_obj) {
			ret = -ENOENT;
			goto err_release_command;
		}

		to->x = from->x;
		to->y = from->y;

		drm_dbg(&priv->drm,
			"cmd %u: add ref to object %u (%d) as source %zu at (%d,%d)\n",
			cmd->id,
			to->gfx2d_obj->id, atomic_read(&cmd->target->gpu_active),
			i, to->x, to->y);
	}

	ret = mchp_gfx2d_schedule(priv, cmd);
	if (ret)
		goto err_release_command;

	return 0;

err_release_command:
	mchp_gfx2d_release_command(priv, cmd, true);
	return ret;
}

/**
 * mchp_gfx2d_wait - wait for a GFX2D object to be inactive
 * @priv: the GFX2D device
 * @gfx2d_obj: the GFX2D object to wait for
 * @timeout: the timeout
 *
 * This function waits for a given GFX2D object to be inactive, that is to say
 * this GFX2D object is not involved in any graphics instructions currently
 * running in the ring buffer or queued in the pending list.
 *
 * It is called from:
 * - mchp_gfx2d_ioctl_wait()
 * - mchp_gfx2d_iocl_sync_for_cpu()
 *
 * RETURNS:
 * 0 on success, on error code otherwise.
 */
int mchp_gfx2d_wait(struct mchp_gfx2d_device *priv,
		    struct mchp_gfx2d_gem_object *gfx2d_obj,
		    const struct drm_mchp_timespec *timeout)
{
	struct device *dev = priv->drm.dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	(void)mchp_gfx2d_progress(priv);

	drm_dbg(&priv->drm, "obj %u: poll object\n", gfx2d_obj->id);

	ret = mchp_gfx2d_gem_wait(gfx2d_obj, NULL);
	if (!ret || !timeout)
		goto put_autosuspend;

	drm_dbg(&priv->drm, "obj %u: wait for object\n", gfx2d_obj->id);

	mutex_lock(&priv->wlist_mutex);
	list_add_tail(&gfx2d_obj->w_node, &priv->wlist);
	mutex_unlock(&priv->wlist_mutex);

	mchp_gfx2d_enable_exend(priv);

	ret = mchp_gfx2d_gem_wait(gfx2d_obj, timeout);

	mutex_lock(&priv->wlist_mutex);
	list_del(&gfx2d_obj->w_node);
	mutex_unlock(&priv->wlist_mutex);

	drm_dbg(&priv->drm, "obj %u: %s for object\n",
		gfx2d_obj->id,
		ret ? "failed to wait" : "successfully waited");

put_autosuspend:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/**
 * mchp_gfx2d_cancel_commands - cancel all commands for a give context
 * @priv: the GFX2D device
 * @ctx: the GFX2D context
 *
 * This function is called from mchp_gfx2d_postclose().
 */
void mchp_gfx2d_cancel_commands(struct mchp_gfx2d_device *priv,
				struct mchp_gfx2d_file *ctx)
{
	struct mchp_gfx2d_command *cmd, *tmp;

	mutex_lock(&priv->cmdlist_mutex);

	list_for_each_entry(cmd, &priv->running_cmdlist, cmd_node) {
		if (cmd->ctx == ctx)
			cmd->ctx = NULL;
	}

	list_for_each_entry_safe(cmd, tmp, &priv->pending_cmdlist, cmd_node) {
		if (cmd->ctx == ctx) {
			list_del(&cmd->cmd_node);
			mchp_gfx2d_release_command(priv, cmd, true);
		}
	}

	mutex_unlock(&priv->cmdlist_mutex);
}

/**
 * mchp_gfx2d_has_pending_commands - check whether some commands are waiting for
 *     being executed
 * @priv: the GFX2D device
 *
 * This function MUST be called with the 'priv->cmdlist_mutex' unlocked as it
 * locks this mutex itself. Otherwise, call mchp_gfx2d_has_pending_command_locked()
 * instead.
 *
 * RETURNS:
 * true if there is at least one pending command, otherwise false.
 */
bool mchp_gfx2d_has_pending_commands(struct mchp_gfx2d_device *priv)
{
	bool ret;

	mutex_lock(&priv->cmdlist_mutex);
	ret = mchp_gfx2d_has_pending_commands_locked(priv);
	mutex_unlock(&priv->cmdlist_mutex);

	return ret;
}

/**
 * mchp_gfx2d_progress - make the command queue progress
 * @priv: the GFX2D device
 *
 * This function first processes completed commands then append
 * as much graphics instructions as possible in the ring buffer.
 *
 * It is called from:
 * - mchp_gfx2d_schedule()
 * - mchp_gfx2d_wait()
 * - mchp_gfx2d_thread()
 *
 * RETURNS:
 * true if at least one command has progressed in the queue.
 */
bool mchp_gfx2d_progress(struct mchp_gfx2d_device *priv)
{
	bool did_something;

	/* Clear pending interrupts */
	(void)readl(priv->regs + GFX2D_IS);

	did_something = mchp_gfx2d_process_completed_commands(priv);
	return mchp_gfx2d_run_pending_commands(priv) || did_something;
}
