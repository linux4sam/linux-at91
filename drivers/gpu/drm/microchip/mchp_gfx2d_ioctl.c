// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */


#include <linux/cacheflush.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>

#include <drm/drm_prime.h>

#include <uapi/drm/microchip_drm.h>

#include "mchp_gfx2d_drv.h"
#include "mchp_gfx2d_cmd.h"
#include "mchp_gfx2d_gem.h"
#include "mchp_gfx2d_ioctl.h"

/*
 * GFX2D timeouts are specified wrt CLOCK_MONOTONIC, not jiffies.
 * We need to calculate the timeout in terms of number of jiffies
 * between the specified timeout and the current CLOCK_MONOTONIC time.
 */
unsigned long mchp_timeout_to_jiffies(const struct drm_mchp_timespec *timeout)
{
	struct timespec64 ts, to = {
		.tv_sec = timeout->tv_sec,
		.tv_nsec = timeout->tv_nsec,
	};

	ktime_get_ts64(&ts);

	/* timeots before "now" have already expired */
	if (timespec64_compare(&to, &ts) <= 0)
		return 0;

	ts = timespec64_sub(to, ts);

	return timespec64_to_jiffies(&ts);
}

static int mchp_gfx2d_ioctl_submit(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct mchp_gfx2d_device *priv = drm_to_dev(dev);
	struct drm_mchp_gfx2d_submit *args = data;

	return mchp_gfx2d_submit(priv, file_priv, args);
}

static int mchp_gfx2d_ioctl_wait(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct mchp_gfx2d_device *priv = drm_to_dev(dev);
	struct drm_mchp_gfx2d_wait *args = data;
	const struct drm_mchp_timespec *timeout = &args->timeout;
	struct drm_gem_object *obj;
	int ret;

	if (args->flags & ~DRM_MCHP_GFX2D_WAIT_NONBLOCK)
		return -EINVAL;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	if (args->flags & DRM_MCHP_GFX2D_WAIT_NONBLOCK)
		timeout = NULL;

	ret = mchp_gfx2d_wait(priv, to_mchp_gfx2d_bo(obj), timeout);

	drm_gem_object_put(obj);

	return ret;
}

static int to_dma_data_direction(enum drm_mchp_gfx2d_direction gfx2d_dir,
				 enum dma_data_direction *dir)
{
	switch (gfx2d_dir) {
	case DRM_MCHP_GFX2D_DIR_BIDIRECTIONAL:
		*dir = DMA_BIDIRECTIONAL;
		break;

	case DRM_MCHP_GFX2D_DIR_TO_DEVICE:
		*dir = DMA_TO_DEVICE;
		break;

	case DRM_MCHP_GFX2D_DIR_FROM_DEVICE:
		*dir = DMA_FROM_DEVICE;
		break;

	case DRM_MCHP_GFX2D_DIR_NONE:
		*dir = DMA_NONE;
		break;

	default:
		return -EINVAL;
		//break;
	}

	return 0;
}

static bool mchp_gfx2d_valid_buffer_params(size_t width, size_t stride,
					   enum drm_mchp_gfx2d_pixel_format format)
{
	size_t bytes_per_pixel;

	switch (format) {
	case DRM_MCHP_GFX2D_PF_A4IDX4:
	case DRM_MCHP_GFX2D_PF_A8:
	case DRM_MCHP_GFX2D_PF_IDX8:
		bytes_per_pixel = 1;
		break;

	case DRM_MCHP_GFX2D_PF_A8IDX8:
	case DRM_MCHP_GFX2D_PF_RGB12:
	case DRM_MCHP_GFX2D_PF_ARGB16:
	case DRM_MCHP_GFX2D_PF_RGB15:
	case DRM_MCHP_GFX2D_PF_TRGB16:
	case DRM_MCHP_GFX2D_PF_RGBT16:
	case DRM_MCHP_GFX2D_PF_RGB16:
		bytes_per_pixel = 2;
		break;

	case DRM_MCHP_GFX2D_PF_RGB24:
		bytes_per_pixel = 3;
		break;

	case DRM_MCHP_GFX2D_PF_ARGB32:
	case DRM_MCHP_GFX2D_PF_RGBA32:
		bytes_per_pixel = 4;
		break;

	default:
		return false;
		//break;
	}

	return (width * bytes_per_pixel) <= stride;
}

static bool mchp_gfx2d_valid_size(size_t stride, size_t height, size_t size)
{
	return (stride * height) <= size;
}

static int mchp_gfx2d_ioctl_alloc_buffer(struct drm_device *dev, void *data,
					 struct drm_file *file_priv)
{
	struct drm_mchp_gfx2d_alloc_buffer *args = data;
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	enum dma_data_direction dir;
	struct drm_gem_object *obj;
	int ret;

	ret = to_dma_data_direction(args->direction, &dir);
	if (ret)
		return ret;

	if (!mchp_gfx2d_valid_buffer_params(args->width, args->stride, args->format))
		return -EINVAL;

	if (!mchp_gfx2d_valid_size(args->stride, args->height, args->size))
		return -EINVAL;

	gfx2d_obj = mchp_gfx2d_gem_create_with_handle(file_priv, dev,
						      args->size, dir,
						      &args->handle);
	if (IS_ERR(gfx2d_obj))
		return PTR_ERR(gfx2d_obj);

	gfx2d_obj->width = args->width;
	gfx2d_obj->height = args->height;
	gfx2d_obj->stride = args->stride;
	gfx2d_obj->format = args->format;

	obj = &gfx2d_obj->base;

	args->offset = drm_vma_node_offset_addr(&obj->vma_node);

	if (valid_dma_direction(dir))
		dma_sync_single_for_device(dev->dev, gfx2d_obj->dma_addr,
					   obj->size, dir);

	return 0;
}

static int mchp_gfx2d_ioctl_import_buffer(struct drm_device *dev, void *data,
					  struct drm_file *file_priv)
{
	struct drm_mchp_gfx2d_import_buffer *args = data;
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	struct drm_gem_object *obj;
	int ret;

	if (!mchp_gfx2d_valid_buffer_params(args->width, args->stride, args->format))
		return -EINVAL;

	ret = drm_gem_prime_fd_to_handle(dev, file_priv, args->fd, &args->handle);
	if (ret)
		return ret;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	if (!mchp_gfx2d_valid_size(args->stride, args->height, obj->size)) {
		drm_gem_object_put(obj);
		drm_gem_handle_delete(file_priv, args->handle);
		return -EINVAL;
	}

	gfx2d_obj = to_mchp_gfx2d_bo(obj);
	gfx2d_obj->width = args->width;
	gfx2d_obj->height = args->height;
	gfx2d_obj->stride = args->stride;
	gfx2d_obj->format = args->format;

	drm_gem_object_put(obj);

	return 0;
}

static int mchp_gfx2d_ioctl_free_buffer(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	struct drm_mchp_gfx2d_free_buffer *args = data;

	return drm_gem_handle_delete(file_priv, args->handle);
}

static int mchp_gfx2d_ioctl_sync_for_cpu(struct drm_device *dev, void *data,
					 struct drm_file *file_priv)
{
	struct mchp_gfx2d_device *priv = drm_to_dev(dev);
	struct drm_mchp_gfx2d_sync_for_cpu *args = data;
	const struct drm_mchp_timespec *timeout = &args->timeout;
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	struct drm_gem_object *obj;
	int ret;

	if (args->flags & ~DRM_MCHP_GFX2D_WAIT_NONBLOCK)
		return -EINVAL;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	gfx2d_obj = to_mchp_gfx2d_bo(obj);

	if (args->flags & DRM_MCHP_GFX2D_WAIT_NONBLOCK)
		timeout = NULL;

	ret = mchp_gfx2d_wait(priv, gfx2d_obj, timeout);

	drm_gem_object_put(obj);

	return ret;
}

static int mchp_gfx2d_ioctl_sync_for_gpu(struct drm_device *dev, void *data,
					 struct drm_file *file_priv)
{
	struct drm_mchp_gfx2d_sync_for_gpu *args = data;
	struct mchp_gfx2d_gem_object *gfx2d_obj;
	enum dma_data_direction dir;
	struct drm_gem_object *obj;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	gfx2d_obj = to_mchp_gfx2d_bo(obj);

	dir = gfx2d_obj->direction;
	if (valid_dma_direction(dir)) {
		struct vm_area_struct *vma = &gfx2d_obj->vma;

		flush_cache_range(vma, vma->vm_start, vma->vm_end);
	}

	drm_gem_object_put(obj);

	return 0;
}

const struct drm_ioctl_desc mchp_gfx2d_ioctls[] = {
#define GFX2D_IOCTL(n, func, flags) \
	DRM_IOCTL_DEF_DRV(MCHP_GFX2D_##n, mchp_gfx2d_ioctl_##func, flags)
	GFX2D_IOCTL(SUBMIT,             submit,               DRM_RENDER_ALLOW),
	GFX2D_IOCTL(WAIT,               wait,                 DRM_RENDER_ALLOW),
	GFX2D_IOCTL(ALLOC_BUFFER,       alloc_buffer,         DRM_RENDER_ALLOW),
	GFX2D_IOCTL(IMPORT_BUFFER,      import_buffer,        DRM_RENDER_ALLOW),
	GFX2D_IOCTL(FREE_BUFFER,        free_buffer,          DRM_RENDER_ALLOW),
	GFX2D_IOCTL(SYNC_FOR_CPU,       sync_for_cpu,         DRM_RENDER_ALLOW),
	GFX2D_IOCTL(SYNC_FOR_GPU,       sync_for_gpu,         DRM_RENDER_ALLOW),
};
