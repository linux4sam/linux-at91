/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2024 Microchip
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@microchip.com>
 */

#include <drm/drm_ioctl.h>

#ifndef MCHP_GFX2D_IOCTL_H
#define MCHP_GFX2D_IOCTL_H

extern const struct drm_ioctl_desc mchp_gfx2d_ioctls[7];

unsigned long mchp_timeout_to_jiffies(const struct drm_mchp_timespec *timeout);

#endif /* MCHP_GFX2D_IOCTL_H */
