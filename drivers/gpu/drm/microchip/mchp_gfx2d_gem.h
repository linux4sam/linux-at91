/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#include <linux/dma-direction.h>
#include <linux/mm_types.h>

#include <drm/drm_gem.h>

#include <uapi/drm/microchip_drm.h>

struct dma_buf_attachment;
struct sg_table;

struct drm_mchp_timespec;

#ifndef MCHP_GFX2D_GEM_H
#define MCHP_GFX2D_GEM_H

struct mchp_gfx2d_gem_object {
	struct drm_gem_object base;
	u32 id;

	dma_addr_t dma_addr;
	struct sg_table *sgt;

	void *vaddr;
	enum dma_data_direction direction;

	u16 width;
	u16 height;
	u16 stride;
	enum drm_mchp_gfx2d_pixel_format format;

	struct vm_area_struct vma;

	atomic_t gpu_active;
	wait_queue_head_t event;
	struct list_head w_node;
};

static inline
struct mchp_gfx2d_gem_object *to_mchp_gfx2d_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct mchp_gfx2d_gem_object, base);
}

static inline bool is_active(struct mchp_gfx2d_gem_object *gfx2d_obj)
{
	return atomic_read(&gfx2d_obj->gpu_active) != 0;
}

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_create(struct drm_device *dev, size_t size,
		      enum dma_data_direction dir);

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_create_with_handle(struct drm_file *file_priv,
				  struct drm_device *dev, size_t size,
				  enum dma_data_direction dir,
				  uint32_t *handle);

void mchp_gfx2d_gem_free(struct mchp_gfx2d_gem_object *gfx2d_obj);

struct drm_gem_object *
mchp_gfx2d_gem_prime_import_sg_table(struct drm_device *dev,
				     struct dma_buf_attachment *attach,
				     struct sg_table *sgt);

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_get(struct drm_file *file_priv, uint32_t handle);

static inline void mchp_gfx2d_gem_put(struct mchp_gfx2d_gem_object *gfx2d_obj)
{
	if (gfx2d_obj)
		drm_gem_object_put(&gfx2d_obj->base);
}

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_addref(struct drm_file *file_priv, uint32_t handle);

void mchp_gfx2d_gem_unref(struct mchp_gfx2d_gem_object *gfx2d_obj);

int mchp_gfx2d_gem_wait(struct mchp_gfx2d_gem_object *gfx2d_obj,
			const struct drm_mchp_timespec *timeout);

#endif /* MCHP_GFX2D_GEM_H */
