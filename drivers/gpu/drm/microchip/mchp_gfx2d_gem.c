// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>

#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>

#include "mchp_gfx2d_gem.h"
#include "mchp_gfx2d_ioctl.h"

static void mchp_gfx2d_gem_object_free(struct drm_gem_object *obj)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj = to_mchp_gfx2d_bo(obj);

	mchp_gfx2d_gem_free(gfx2d_obj);
}

static int mchp_gfx2d_gem_object_mmap(struct drm_gem_object *obj,
				      struct vm_area_struct *vma)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj = to_mchp_gfx2d_bo(obj);
	int ret;

	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	vm_flags_mod(vma, VM_DONTEXPAND, VM_PFNMAP);

	if (gfx2d_obj->direction == DMA_NONE) {
		ret = dma_mmap_wc(obj->dev->dev, vma, gfx2d_obj->vaddr,
				  gfx2d_obj->dma_addr,
				  vma->vm_end - vma->vm_start);
	} else {
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

		ret = dma_mmap_pages(obj->dev->dev,
				     vma, vma->vm_end - vma->vm_start,
				     virt_to_page(gfx2d_obj->vaddr));
	}
	if (ret)
		drm_gem_vm_close(vma);
	else
		memcpy(&gfx2d_obj->vma, vma, sizeof(gfx2d_obj->vma));

	return ret;
}

static const struct vm_operations_struct mchp_gfx2d_gem_object_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs mchp_gfx2d_gem_object_funcs = {
	.free = mchp_gfx2d_gem_object_free,
	.mmap = mchp_gfx2d_gem_object_mmap,
	.vm_ops = &mchp_gfx2d_gem_object_vm_ops,
};

static struct mchp_gfx2d_gem_object *
__mchp_gfx2d_gem_create(struct drm_device *dev, size_t size)
{
	static u32 next_id;
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	struct drm_gem_object *obj;
	int ret = 0;

	gfx2d_obj = kzalloc(sizeof(*gfx2d_obj), GFP_KERNEL);
	if (!gfx2d_obj)
		return ERR_PTR(-ENOMEM);

	obj = &gfx2d_obj->base;
	obj->funcs = &mchp_gfx2d_gem_object_funcs;

	drm_gem_private_object_init(dev, obj, size);

	ret = drm_gem_create_mmap_offset(obj);
	if (ret) {
		drm_gem_object_release(obj);
		goto error;
	}

	init_waitqueue_head(&gfx2d_obj->event);

	gfx2d_obj->id = next_id++;

	return gfx2d_obj;

error:
	kfree(gfx2d_obj);
	return ERR_PTR(ret);
}

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_create(struct drm_device *dev, size_t size,
		      enum dma_data_direction dir)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj;

	size = round_up(size, PAGE_SIZE);

	gfx2d_obj = __mchp_gfx2d_gem_create(dev, size);
	if (IS_ERR(gfx2d_obj))
		return gfx2d_obj;

	gfx2d_obj->direction = dir;
	if (dir == DMA_NONE) {
		gfx2d_obj->vaddr = dma_alloc_wc(dev->dev, size,
						&gfx2d_obj->dma_addr,
						GFP_KERNEL | __GFP_NOWARN);
	} else {
		gfx2d_obj->vaddr = dma_alloc_noncoherent(dev->dev, size,
							 &gfx2d_obj->dma_addr,
							 dir,
							 GFP_KERNEL | __GFP_NOWARN);
	}
	if (!gfx2d_obj->vaddr) {
		drm_dbg(dev, "failed to allocate buffer with size %zu\n", size);
		drm_gem_object_put(&gfx2d_obj->base);
		return ERR_PTR(-ENOMEM);
	}

	return gfx2d_obj;
}

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_create_with_handle(struct drm_file *file_priv,
				  struct drm_device *dev, size_t size,
				  enum dma_data_direction dir,
				  uint32_t *handle)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	struct drm_gem_object *obj;
	int ret;

	gfx2d_obj = mchp_gfx2d_gem_create(dev, size, dir);
	if (IS_ERR(gfx2d_obj))
		return gfx2d_obj;

	obj = &gfx2d_obj->base;

	ret = drm_gem_handle_create(file_priv, obj, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(obj);
	if (ret)
		return ERR_PTR(ret);

	return gfx2d_obj;
}

void mchp_gfx2d_gem_free(struct mchp_gfx2d_gem_object *gfx2d_obj)
{
	struct drm_gem_object *obj = &gfx2d_obj->base;

	if (obj->import_attach) {
		drm_prime_gem_destroy(obj, gfx2d_obj->sgt);
	} else if (gfx2d_obj->vaddr) {
		if (gfx2d_obj->direction == DMA_NONE) {
			dma_free_wc(obj->dev->dev, obj->size,
				    gfx2d_obj->vaddr, gfx2d_obj->dma_addr);
		} else {
			dma_free_noncoherent(obj->dev->dev, obj->size,
					     gfx2d_obj->vaddr,
					     gfx2d_obj->dma_addr,
					     gfx2d_obj->direction);
		}
	}

	drm_gem_object_release(obj);

	kfree(gfx2d_obj);
}

struct drm_gem_object *
mchp_gfx2d_gem_prime_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj;

	/* Check if the entries in the sg_table are contiguous. */
	if (drm_prime_get_contiguous_size(sgt) < attach->dmabuf->size)
		return ERR_PTR(-EINVAL);

	/* Create a GFX2D GEM buffer. */
	gfx2d_obj = __mchp_gfx2d_gem_create(dev, attach->dmabuf->size);
	if (IS_ERR(gfx2d_obj))
		return ERR_CAST(gfx2d_obj);

	gfx2d_obj->dma_addr = sg_dma_address(sgt->sgl);
	gfx2d_obj->sgt = sgt;

	drm_dbg_prime(dev, "dma_addr = %pad, size = %zu\n",
		      &gfx2d_obj->dma_addr,
		      attach->dmabuf->size);

	return &gfx2d_obj->base;
}

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_get(struct drm_file *file_priv, uint32_t handle)
{
	struct drm_gem_object *obj;

	obj = drm_gem_object_lookup(file_priv, handle);
	if (!obj)
		return NULL;
	return to_mchp_gfx2d_bo(obj);
}

struct mchp_gfx2d_gem_object *
mchp_gfx2d_gem_addref(struct drm_file *file_priv, uint32_t handle)
{
	struct mchp_gfx2d_gem_object *gfx2d_obj;

	gfx2d_obj = mchp_gfx2d_gem_get(file_priv, handle);
	if (gfx2d_obj)
		atomic_inc(&gfx2d_obj->gpu_active);

	return gfx2d_obj;
}

void mchp_gfx2d_gem_unref(struct mchp_gfx2d_gem_object *gfx2d_obj)
{
	if (!gfx2d_obj)
		return;

	atomic_dec(&gfx2d_obj->gpu_active);
	if (!is_active(gfx2d_obj))
		wake_up_all(&gfx2d_obj->event);

	mchp_gfx2d_gem_put(gfx2d_obj);
}

int mchp_gfx2d_gem_wait(struct mchp_gfx2d_gem_object *gfx2d_obj,
			const struct drm_mchp_timespec *timeout)
{
	unsigned long remaining;
	long ret;

	if (!timeout)
		return !is_active(gfx2d_obj) ? 0 : -EBUSY;

	remaining = mchp_timeout_to_jiffies(timeout);

	ret = wait_event_interruptible_timeout(gfx2d_obj->event,
					       !is_active(gfx2d_obj),
					       remaining);
	if (ret > 0)
		return 0;
	else if (ret == -ERESTARTSYS)
		return -ERESTARTSYS;
	else
		return -ETIMEDOUT;
}
