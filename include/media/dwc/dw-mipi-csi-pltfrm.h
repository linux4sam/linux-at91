/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI CSI-2 Host media entities
 *
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 */

#ifndef __DW_MIPI_CSI_PLTFRM_INCLUDES_H_
#define __DW_MIPI_CSI_PLTFRM_INCLUDES_H_

#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define MAX_WIDTH	3280
#define MAX_HEIGHT	1852

/* The subdevices' group IDs. */
#define GRP_ID_SENSOR		(10)
#define GRP_ID_CSI		(20)
#define GRP_ID_VIF		(30)
#define GRP_ID_VIDEODEV		(40)

#define CSI_MAX_ENTITIES	(2)
#define VIF_MAX_ENTITIES	(2)
#define PLAT_MAX_SENSORS	(2)

struct pdata_names {
	char *name;
};

enum video_dev_pads {
	VIDEO_DEV_SD_PAD_SINK_VIF1,
	VIDEO_DEV_SD_PAD_SINK_VIF2,
	VIDEO_DEV_SD_PAD_SOURCE_DMA,
	VIDEO_DEV_SD_PADS_NUM,
};

enum vif_pads {
	VIF_PAD_SINK_CSI,
	VIF_PAD_SOURCE_DMA,
	VIF_PADS_NUM,
};

enum mipi_csi_pads {
	CSI_PAD_SINK,
	CSI_PAD_SOURCE,
	CSI_PADS_NUM,
};

struct plat_csi_source_info {
	u16 flags;
	u16 mux_id;
};

struct plat_csi_fmt {
	char *name;
	u32 mbus_code;
	u32 fourcc;
	u8 depth;
};

struct plat_csi_media_pipeline;

/*
 * Media pipeline operations to be called from within a video node,  i.e. the
 * last entity within the pipeline. Implemented by related media device driver.
 */
struct plat_csi_media_pipeline_ops {
	int (*prepare)(struct plat_csi_media_pipeline *p,
		       struct media_entity *me);
	int (*unprepare)(struct plat_csi_media_pipeline *p);
	int (*open)(struct plat_csi_media_pipeline *p, struct media_entity *me,
		    bool resume);
	int (*close)(struct plat_csi_media_pipeline *p);
	int (*set_stream)(struct plat_csi_media_pipeline *p, bool state);
	int (*set_format)(struct plat_csi_media_pipeline *p,
			  struct v4l2_subdev_format *fmt);
};

struct plat_csi_video_entity {
	struct video_device vdev;
	struct plat_csi_media_pipeline *pipe;
};

struct plat_csi_media_pipeline {
	struct media_pipeline mp;
	const struct plat_csi_media_pipeline_ops *ops;
};

static inline struct plat_csi_video_entity
*vdev_to_plat_csi_video_entity(struct video_device *vdev)
{
	return container_of(vdev, struct plat_csi_video_entity, vdev);
}

#define plat_csi_pipeline_call(ent, op, args...)			  \
	(!(ent) ? -ENOENT : (((ent)->pipe->ops && (ent)->pipe->ops->op) ? \
	(ent)->pipe->ops->op(((ent)->pipe), ##args) : -ENOIOCTLCMD))	  \

#endif /* __DW_MIPI_CSI_PLTFRM_INCLUDES_H_ */
