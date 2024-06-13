// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Video Capture Pipeline Processing (vcpp) Driver.
 *
 * Copyright (C) 2022-2023 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kmod.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "microchip-common.h"

#define MCHP_VCPP_DRV_NAME			"mchp-vcpp"

#define MCHP_VCPP_DEF_WIDTH			1920
#define MCHP_VCPP_DEF_HEIGHT			1080

/* Minimum and maximum widths are expressed in bytes */
#define MCHP_VCPP_MIN_WIDTH			256U
#define MCHP_VCPP_MAX_WIDTH			2048U
#define MCHP_VCPP_MIN_HEIGHT			256U
#define MCHP_VCPP_MAX_HEIGHT			1080U
#define MCHP_VCPP_DEF_FORMAT			V4L2_PIX_FMT_YUYV

#define MCHP_VCPP_IP_VER			0x00
#define MCHP_VCPP_CTRL_REG			0x04
#define MCHP_VCPP_CORE_ENABLE			BIT(0)
#define MCHP_VCPP_CORE_RESET			BIT(1)
#define MCHP_VCPP_FLASH_FIFO			BIT(2)

#define MCHP_VCPP_GLBL_INT_EN			0x08
#define MCHP_VCPP_GLBL_INT_EN_BIT		BIT(0)

#define MCHP_VCPP_INT_STATUS			0x0C
#define MCHP_VCPP_INT_STATUS_EOF		BIT(0)

#define MCHP_VCPP_INT_EN			0x10
#define MCHP_VCPP_INT_EN_EOF			BIT(0)
#define MCHP_VCPP_INT_EN_FIFO_MT		(2 << 0)
#define MCHP_VCPP_INT_EN_FIFO_FULL		(3 << 0)

#define MCHP_VCPP_FRAME_RESOLUTION		0x14
#define MCHP_VCPP_LINE_GAP			0x18
#define MCHP_VCPP_BUFF_ADDR_FIFO_DATA		0x1C
#define MCHP_VCPP_BUFF_ADDR_FIFO_RDATA_COUNT	0x20
#define MCHP_VCPP_FRAME_SIZE_FIFO_DATA_RD	0x24
#define MCHP_VCPP_FRAME_SIZE_FIFO_WDATA_COUNT	0x28

#define MCHP_VCPP_FRAME_START			0x1
#define MCHP_VCPP_FRAME_STOP			0x0

#define MCHP_VCPP_MAX_FRAMES			8

/* The compression ratio is calculated for every 60 frames */
#define MCHP_VCPP_CR_MAX_FRAMES_RESET_COUNT	60
#define MCHP_VCPP_CR_MAX_FRAMES_INIT		50
#define MCHP_VCPP_CR_MAX_ARRAY			6

#define MCHP_VCPP_NUM_CTRLS			1

/* Minimum wait time for camera to stabilize */
#define MCHP_VCPP_DELAYED_CAM_M_SEC		100

enum mchp_vcpp_state {
	VCPP_STOPPED = 0,
	VCPP_WAIT_FOR_BUFFER,
	VCPP_RUNNING,
};

/**
 * struct mchp_vcpp_buffer - buffer for one video frame
 * @vb:		video buffer information struct vb2_v4l2_buffer
 * @list:	list of all requested buffers from user space
 * @paddr:	physical address of buffer
 * @size:	size of buffer
 * @prepared:	status of buffer
 */
struct mchp_vcpp_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	dma_addr_t paddr;
	size_t size;
	bool prepared;
};

/**
 * struct mchp_vcpp_compression_ratio - for compression calculation
 * @frame_size:		accumulated frames size
 * @frame_size_index:	accumulated present frames size index
 * @frame_count:	present frame count
 */
struct mchp_vcpp_compression_ratio {
	u32 frame_size[MCHP_VCPP_CR_MAX_ARRAY];
	u32 frame_size_index;
	u32 frame_count;
};

/**
 * struct mchp_vcpp_fpga - V4L2 device context
 * @base:		pointer to control register
 * @pdev:		platform device
 * @dev:		device
 * @active:		current buffer in queue
 * @reset_gpio:		sensor fabric rest
 * @v4l2_dev:		top-level v4l2 device struct
 * @vdev:		video node structure
 * @format:		active V4L2 pixel format
 * @fmtinfo:		format information corresponding to the active @format
 * @queue:		vb2 video capture queue
 * @lock:		mutex lock for vb2 buffs
 * @qlock:		spinlock controlling access to buf_list and sequence
 * @buf_list:		list of buffers queued for DMA
 * @subdev_entities:	subdevice list
 * @cambuf:		struct cam_buffer
 * @notifier:		V4L2 asynchronous subdevs notifier
 * @mdev:		media device
 * @vid_cap_pad:	media pad for the video device entity
 * @h264_ratio		struct mchp_vcpp_compression
 * @ctrl_handler:	control handler structure
 * @state:		state of buffers
 * @irq:		external IRQ for new frame
 * @sequence:		frame sequence counter
 */
struct mchp_vcpp_fpga {
	void __iomem *base;
	struct platform_device *pdev;
	struct device *dev;
	struct mchp_vcpp_buffer *active;
	struct gpio_desc *reset_gpio;
	struct clk_bulk_data *clks;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_format format;
	const struct mvideo_format *fmtinfo;
	struct vb2_queue queue;
	struct mutex lock; /* vb2 buffer lock */
	spinlock_t qlock;
	struct list_head buf_list;
	struct list_head subdev_entities;
	struct v4l2_async_notifier notifier;
	struct media_device mdev;
	struct media_pad vid_cap_pad;
	struct mchp_vcpp_compression_ratio h264_ratio;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *compression_ratio_ctrl;
	enum mchp_vcpp_state state;
	int irq;
	int sequence;
};

struct mchp_vcpp_graph_entity {
	struct v4l2_async_connection asc;
	struct media_entity *entity;
	struct v4l2_subdev *subdev;
};

static const struct clk_bulk_data mchp_vcpp_clks[] = {
	{ .id = "axi" },
	{ .id = "video" },
};

static inline struct mchp_vcpp_graph_entity *
to_mchp_vcpp_entity(struct v4l2_async_connection *asc)
{
	return container_of(asc, struct mchp_vcpp_graph_entity, asc);
}

static struct mchp_vcpp_graph_entity *
mchp_vcpp_graph_find_entity(struct mchp_vcpp_fpga *mchp_vcpp,
			    const struct fwnode_handle *fwnode)
{
	struct mchp_vcpp_graph_entity *entity;
	struct v4l2_async_connection *asc;
	struct list_head *lists[] = {
		&mchp_vcpp->notifier.done_list,
		&mchp_vcpp->notifier.waiting_list
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lists); i++) {
		list_for_each_entry(asc, lists[i], asc_entry) {
			entity = to_mchp_vcpp_entity(asc);
			if (entity->asc.match.fwnode == fwnode)
				return entity;
		}
	}

	return NULL;
}

static struct v4l2_subdev *
mchp_vcpp_remote_subdev(struct media_pad *local, u32 *pad)
{
	struct media_pad *remote;

	remote = media_pad_remote_pad_first(local);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return NULL;

	if (pad)
		*pad = remote->index;

	return media_entity_to_v4l2_subdev(remote->entity);
}

static int mchp_vcpp_verify_format(struct mchp_vcpp_fpga *mchp_vcpp)
{
	struct v4l2_subdev_format fmt= {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_subdev *subdev;
	int ret;

	subdev = mchp_vcpp_remote_subdev(&mchp_vcpp->vid_cap_pad, &fmt.pad);
	if (!subdev)
		return -EPIPE;

	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &fmt);
	if (ret < 0)
		return ret == -ENOIOCTLCMD ? -EINVAL : ret;

	if (mchp_vcpp->fmtinfo->code != fmt.format.code ||
	    mchp_vcpp->format.fmt.pix.height != fmt.format.height ||
	    mchp_vcpp->format.fmt.pix.width != fmt.format.width)
		return -EINVAL;

	return 0;
}

static void mchp_vcpp_buffer_done(struct mchp_vcpp_fpga *mchp_vcpp,
				  struct mchp_vcpp_buffer *buf,
				  size_t bytesused,
				  int err)
{
	struct vb2_v4l2_buffer *vbuf;

	if (!buf)
		return;

	list_del_init(&buf->list);
	vbuf = &buf->vb;
	vbuf->sequence = mchp_vcpp->sequence++;
	vbuf->field = V4L2_FIELD_NONE;
	vbuf->vb2_buf.timestamp = ktime_get_ns();

	vb2_set_plane_payload(&vbuf->vb2_buf, 0, bytesused);

	vb2_buffer_done(&vbuf->vb2_buf,
			err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);

	dev_dbg(mchp_vcpp->dev, "Buffer[%d] done seq=%d, bytesused=%zu\n",
		vbuf->vb2_buf.index, vbuf->sequence, bytesused);

	mchp_vcpp->active = NULL;
}

/*
 * Compression ratio is the percentage of compressed image over the actual image.
 * The YUV420 requires 3 bytes per 2 pixels, or 1.5 bytes per pixel, because
 * groups of pixels share a single color value.
 * The compression ratio can be calculated as:
 * (width * height * accumulated-frame-count * bytes-per-pixel) / accumulated-frame-size
 */
static u32 compression_ratio_calc(u32 hres, u32 vres, u32 accumulated_frame_size)
{
	return ((hres * vres * MCHP_VCPP_CR_MAX_FRAMES_RESET_COUNT * 3) /
		(accumulated_frame_size)) / 2;
}

static irqreturn_t mchp_vcpp_irq_ext(int irq, void *dev_id)
{
	struct mchp_vcpp_fpga *mchp_vcpp = dev_id;
	u32 irq_status;
	unsigned long flags;

	irq_status = readl_relaxed(mchp_vcpp->base + MCHP_VCPP_INT_STATUS);

	spin_lock_irqsave(&mchp_vcpp->qlock, flags);

	if (!(irq_status & MCHP_VCPP_INT_STATUS_EOF)) {
		writel_relaxed(0xFF, mchp_vcpp->base + MCHP_VCPP_INT_STATUS);
		spin_unlock_irqrestore(&mchp_vcpp->qlock, flags);
		return IRQ_NONE;
	} else {
		writel_relaxed(MCHP_VCPP_INT_EN_EOF, mchp_vcpp->base + MCHP_VCPP_INT_STATUS);
	}

	if (mchp_vcpp->state != VCPP_RUNNING) {
		spin_unlock_irqrestore(&mchp_vcpp->qlock, flags);
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&mchp_vcpp->qlock, flags);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mchp_vcpp_irq_thread_fn(int irq, void *dev_id)
{
	struct mchp_vcpp_fpga *mchp_vcpp = dev_id;
	struct mchp_vcpp_buffer *buf;
	struct v4l2_pix_format *pix = &mchp_vcpp->format.fmt.pix;
	struct mchp_vcpp_compression_ratio *h264_ratio = &mchp_vcpp->h264_ratio;
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_subdev *subdev;
	int ret, buf_size, i;
	int *frame_size_index = &h264_ratio->frame_size_index;

	spin_lock_irq(&mchp_vcpp->qlock);

	if (list_empty(&mchp_vcpp->buf_list)) {
		dev_dbg(mchp_vcpp->dev,
			"Capture restart is deferred to next buffer queue\n");
		mchp_vcpp->state = VCPP_WAIT_FOR_BUFFER;
		spin_unlock_irq(&mchp_vcpp->qlock);
		return IRQ_HANDLED;
	}

	buf = list_entry(mchp_vcpp->buf_list.next,
			 struct mchp_vcpp_buffer, list);

	mchp_vcpp->active = buf;

	spin_unlock_irq(&mchp_vcpp->qlock);

	if (mchp_vcpp->fmtinfo->fourcc == V4L2_PIX_FMT_H264) {
		buf_size = readl_relaxed(mchp_vcpp->base + MCHP_VCPP_FRAME_SIZE_FIFO_DATA_RD);
		mchp_vcpp_buffer_done(mchp_vcpp, buf, buf_size, 0);
		h264_ratio->frame_count++;
		h264_ratio->frame_size[*frame_size_index] += buf_size;

		if (h264_ratio->frame_count % 10 == 0)
			(*frame_size_index)++;

		if (*frame_size_index == MCHP_VCPP_CR_MAX_ARRAY)
			*frame_size_index = 0;

		if (h264_ratio->frame_count == MCHP_VCPP_CR_MAX_FRAMES_RESET_COUNT) {
			u32 accumulated_frame_size = 0, compression_ratio, hres, vres;

			for (i = 0; i < MCHP_VCPP_CR_MAX_ARRAY; i++)
				accumulated_frame_size += h264_ratio->frame_size[i];

			subdev = mchp_vcpp_remote_subdev(&mchp_vcpp->vid_cap_pad, &fmt.pad);
			if (!subdev)
				return -EPIPE;

			ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &fmt);
			if (ret < 0)
				return ret == -ENOIOCTLCMD ? -EINVAL : ret;

			hres = fmt.format.width;
			vres = fmt.format.height;

			compression_ratio = compression_ratio_calc(hres, vres,
								   accumulated_frame_size);
			__v4l2_ctrl_s_ctrl(mchp_vcpp->compression_ratio_ctrl, compression_ratio);

			h264_ratio->frame_count = MCHP_VCPP_CR_MAX_FRAMES_INIT;
			h264_ratio->frame_size[*frame_size_index] = 0;
		}
	} else {
		mchp_vcpp_buffer_done(mchp_vcpp, buf,
				      pix->width * pix->height * mchp_vcpp->fmtinfo->bpp, 0);
	}

	spin_lock_irq(&mchp_vcpp->qlock);

	if (list_empty(&mchp_vcpp->buf_list)) {
		dev_dbg(mchp_vcpp->dev,
			"Capture restart is deferred to next buffer queue\n");
		mchp_vcpp->state = VCPP_WAIT_FOR_BUFFER;
		spin_unlock_irq(&mchp_vcpp->qlock);
		return IRQ_HANDLED;
	}

	spin_unlock_irq(&mchp_vcpp->qlock);

	return IRQ_HANDLED;
}

static int mchp_vcpp_pipeline_set_stream(struct mchp_vcpp_fpga *mchp_vcpp, bool state)
{
	struct media_entity *entity = &mchp_vcpp->vdev.entity;
	struct v4l2_subdev *subdev;
	struct media_pad *pad;
	int ret;

	/* Start/stop all entities within pipeline */
	while (1) {
		pad = &entity->pads[0];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		pad = media_pad_remote_pad_first(pad);
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			break;

		entity = pad->entity;
		subdev = media_entity_to_v4l2_subdev(entity);

		ret = v4l2_subdev_call(subdev, video, s_stream, state);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(mchp_vcpp->dev, "%s: \"%s\" failed to %s streaming (%d)\n",
				__func__, subdev->name,
				state ? "start" : "stop", ret);
			return ret;
		}

		dev_dbg(mchp_vcpp->dev, "\"%s\" is %s\n",
			subdev->name, state ? "started" : "stopped");
	}

	return 0;
}

static int mchp_vcpp_queue_setup(struct vb2_queue *vq,
				 unsigned int *nbuffers, unsigned int *nplanes,
				 unsigned int sizes[], struct device *alloc_devs[])
{
	struct mchp_vcpp_fpga *mchp_vcpp = vb2_get_drv_priv(vq);
	unsigned int size;

	if (*nbuffers > MCHP_VCPP_MAX_FRAMES) {
		dev_dbg(mchp_vcpp->dev,
			"output frame count too high (%d), cut to %d\n",
				 *nbuffers, MCHP_VCPP_MAX_FRAMES);
		*nbuffers = MCHP_VCPP_MAX_FRAMES;
	}

	size = mchp_vcpp->format.fmt.pix.sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;

	dev_dbg(mchp_vcpp->dev, "Setup queue, count=%d, size=%d\n",
		*nbuffers, size);

	return 0;
}

static int mchp_vcpp_buffer_prepare(struct vb2_buffer *vb)
{
	struct mchp_vcpp_fpga *mchp_vcpp = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mchp_vcpp_buffer *buf =
			container_of(vbuf, struct mchp_vcpp_buffer, vb);
	unsigned int size;

	size = mchp_vcpp->format.fmt.pix.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(mchp_vcpp->dev,
			"data will not fit into plane (%lu < %u)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	if (!buf->prepared) {
		buf->paddr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		buf->size = vb2_plane_size(&buf->vb.vb2_buf, 0);
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, buf->size);
		buf->prepared = true;
	}

	return 0;
}

static void mchp_vcpp_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mchp_vcpp_fpga *mchp_vcpp = vb2_get_drv_priv(vb->vb2_queue);
	struct mchp_vcpp_buffer *buf =
				container_of(vbuf, struct mchp_vcpp_buffer, vb);

	spin_lock_irq(&mchp_vcpp->qlock);
	list_add_tail(&buf->list, &mchp_vcpp->buf_list);
	if (mchp_vcpp->state == VCPP_WAIT_FOR_BUFFER) {
		mchp_vcpp->state = VCPP_RUNNING;
		mchp_vcpp->active = buf;
	}

	writel_relaxed(buf->paddr >> 6, mchp_vcpp->base + MCHP_VCPP_BUFF_ADDR_FIFO_DATA);

	spin_unlock_irq(&mchp_vcpp->qlock);
}

static void mchp_return_all_buffers(struct mchp_vcpp_fpga *mchp_vcpp,
				    enum vb2_buffer_state state)
{
	struct mchp_vcpp_buffer *buf, *node;

	list_for_each_entry_safe(buf, node, &mchp_vcpp->buf_list, list) {
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		list_del(&buf->list);
	}

	mchp_vcpp->active = NULL;
}

static int mchp_vcpp_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mchp_vcpp_fpga *mchp_vcpp = vb2_get_drv_priv(vq);
	int ret;

	ret = mchp_vcpp_verify_format(mchp_vcpp);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "%s: Failed to mchp_vcpp_verify_format (%d)\n",
			__func__, ret);
		goto err_free_buffers;
	}

	ret = v4l2_pipeline_pm_get(&mchp_vcpp->vdev.entity);
	if (ret) {
		dev_err(mchp_vcpp->dev, "open cif pipeline failed %d\n", ret);
		return ret;
	}

	ret = mchp_vcpp_pipeline_set_stream(mchp_vcpp, true);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "%s: Failed to mchp_vcpp_pipeline_start (%d)\n",
			__func__, ret);
		return ret;
	}

	writel_relaxed(MCHP_VCPP_GLBL_INT_EN_BIT, mchp_vcpp->base + MCHP_VCPP_GLBL_INT_EN);
	writel_relaxed(MCHP_VCPP_INT_EN_EOF, mchp_vcpp->base + MCHP_VCPP_INT_EN);

	writel_relaxed(mchp_vcpp->format.fmt.pix.width,	mchp_vcpp->base + MCHP_VCPP_LINE_GAP);

	writel_relaxed(MCHP_VCPP_FRAME_STOP, mchp_vcpp->base + MCHP_VCPP_CTRL_REG);
	mdelay(MCHP_VCPP_DELAYED_CAM_M_SEC);
	writel_relaxed(MCHP_VCPP_FRAME_START, mchp_vcpp->base + MCHP_VCPP_CTRL_REG);

	mchp_vcpp->sequence = 0;

	ret = request_threaded_irq(mchp_vcpp->irq, mchp_vcpp_irq_ext,
				   mchp_vcpp_irq_thread_fn, IRQF_NO_SUSPEND,
				   KBUILD_MODNAME, mchp_vcpp);

	if (ret) {
		dev_err(mchp_vcpp->dev, "request threaded irq failed %d\n",
			ret);
		goto err_free_buffers;
	}

	spin_lock_irq(&mchp_vcpp->qlock);

	if (list_empty(&mchp_vcpp->buf_list)) {
		dev_dbg(mchp_vcpp->dev,
			"Start streaming is deferred to next buffer queue\n");
		mchp_vcpp->state = VCPP_WAIT_FOR_BUFFER;
		spin_unlock_irq(&mchp_vcpp->qlock);
		return 0;
	}

	mchp_vcpp->state = VCPP_RUNNING;

	spin_unlock_irq(&mchp_vcpp->qlock);

	return 0;

err_free_buffers:
	spin_lock_irq(&mchp_vcpp->qlock);
	mchp_return_all_buffers(mchp_vcpp, VB2_BUF_STATE_QUEUED);
	spin_unlock_irq(&mchp_vcpp->qlock);

	return ret;
}

static void mchp_vcpp_stop_streaming(struct vb2_queue *vq)
{
	struct mchp_vcpp_fpga *mchp_vcpp = vb2_get_drv_priv(vq);

	writel_relaxed(MCHP_VCPP_FRAME_STOP, mchp_vcpp->base + MCHP_VCPP_CTRL_REG);

	free_irq(mchp_vcpp->irq, mchp_vcpp);

	spin_lock_irq(&mchp_vcpp->qlock);

	mchp_return_all_buffers(mchp_vcpp, VB2_BUF_STATE_QUEUED);

	mchp_vcpp->active = NULL;
	mchp_vcpp->state = VCPP_STOPPED;

	spin_unlock_irq(&mchp_vcpp->qlock);

	writel_relaxed(MCHP_VCPP_CORE_RESET, mchp_vcpp->base + MCHP_VCPP_CTRL_REG);

	mchp_vcpp_pipeline_set_stream(mchp_vcpp, false);
}

static const struct vb2_ops mchp_vcpp_qops = {
	.queue_setup		= mchp_vcpp_queue_setup,
	.buf_prepare		= mchp_vcpp_buffer_prepare,
	.buf_queue		= mchp_vcpp_buffer_queue,
	.start_streaming	= mchp_vcpp_start_streaming,
	.stop_streaming		= mchp_vcpp_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int mchp_vcpp_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	strscpy(cap->driver, MCHP_VCPP_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MCHP Camera Media Pipeline", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:mchp-vcpp", sizeof(cap->bus_info));

	return 0;
}

static void __mchp_vcpp_try_format(struct mchp_vcpp_fpga *mchp_vcpp,
				   struct v4l2_pix_format *pix,
				   const struct mvideo_format **fmtinfo)
{
	const struct mvideo_format *info;
	struct v4l2_subdev_format v4l_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_subdev *subdev;
	unsigned int ret;

	subdev = mchp_vcpp_remote_subdev(&mchp_vcpp->vid_cap_pad, &v4l_fmt.pad);
	if (!subdev)
		return;

	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &v4l_fmt);
	if (ret < 0)
		return;

	info = mvc_get_format_by_code(v4l_fmt.format.code);
	if (IS_ERR(info))
		return;

	pix->pixelformat = info->fourcc;
	pix->field = V4L2_FIELD_NONE;

	pix->width = clamp(pix->width, MCHP_VCPP_MIN_WIDTH, MCHP_VCPP_MAX_WIDTH);
	pix->height = clamp(pix->height, MCHP_VCPP_MIN_HEIGHT, MCHP_VCPP_MAX_HEIGHT);

	pix->bytesperline = info->bpp * pix->width;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = mchp_vcpp->format.fmt.pix.colorspace;
	pix->ycbcr_enc = mchp_vcpp->format.fmt.pix.ycbcr_enc;
	pix->quantization = mchp_vcpp->format.fmt.pix.quantization;

	if (fmtinfo)
		*fmtinfo = info;
}

static int mchp_vcpp_try_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *format)
{
	struct mchp_vcpp_fpga *mchp_vcpp = video_drvdata(file);

	__mchp_vcpp_try_format(mchp_vcpp, &format->fmt.pix, NULL);

	return 0;
}

static int mchp_vcpp_s_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *format)
{
	struct mchp_vcpp_fpga *mchp_vcpp = video_drvdata(file);
	const struct mvideo_format *info;

	if (vb2_is_streaming(&mchp_vcpp->queue))
		return -EBUSY;

	__mchp_vcpp_try_format(mchp_vcpp, &format->fmt.pix, &info);

	mchp_vcpp->format.fmt.pix.height = format->fmt.pix.height;
	mchp_vcpp->format.fmt.pix.width = format->fmt.pix.width;
	mchp_vcpp->format.fmt.pix.sizeimage = format->fmt.pix.sizeimage;
	mchp_vcpp->format.fmt.pix.bytesperline = format->fmt.pix.bytesperline;
	mchp_vcpp->format.fmt.pix.pixelformat = format->fmt.pix.pixelformat;
	mchp_vcpp->format.fmt.pix.quantization = 0;
	mchp_vcpp->fmtinfo = info;

	return 0;
}

static int mchp_vcpp_g_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *format)
{
	struct mchp_vcpp_fpga *mchp_vcpp = video_drvdata(file);

	*format = mchp_vcpp->format;

	return 0;
}

static int mchp_vcpp_enum_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_fmtdesc *fmt)
{
	struct mchp_vcpp_fpga *mchp_vcpp = video_drvdata(file);
	struct v4l2_subdev *subdev;
	struct v4l2_subdev_format v4l_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	const struct mvideo_format *mvideo_fmt;
	int ret;

	/* Establish media pad format */
	subdev = mchp_vcpp_remote_subdev(&mchp_vcpp->vid_cap_pad, &v4l_fmt.pad);
	if (!subdev)
		return -EPIPE;

	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &v4l_fmt);
	if (ret < 0)
		return ret == -ENOIOCTLCMD ? -EINVAL : ret;

	if (fmt->index > 0)
		return -EINVAL;

	mvideo_fmt = mvc_get_format_by_code(v4l_fmt.format.code);
	if (IS_ERR(mvideo_fmt))
		return PTR_ERR(mvideo_fmt);

	fmt->pixelformat = mvideo_fmt->fourcc;

	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return 0;
}

static int mchp_vcpp_enum_input(struct file *file, void *priv,
				struct v4l2_input *input)
{
	if (input->index != 0)
		return -EINVAL;

	input->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(input->name, "Camera", sizeof(input->name));

	return 0;
}

static int mchp_vcpp_s_input(struct file *file, void *priv, unsigned int index)
{
	if (index > 0)
		return -EINVAL;

	return 0;
}

static int mchp_vcpp_g_input(struct file *file, void *priv, unsigned int *index)
{
	*index = 0;

	return 0;
}

static int mchp_vcpp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	if (ctrl->id != MCHP_CID_COMPRESSION_RATIO)
		return -EINVAL;

	return 0;
}

static const struct v4l2_ctrl_ops mchp_vcpp_ctrl_ops = {
	.s_ctrl = mchp_vcpp_s_ctrl,
};

static const struct v4l2_ctrl_config mchp_vcpp_ctrls[] = {
	{
		.ops	= &mchp_vcpp_ctrl_ops,
		.id	= MCHP_CID_COMPRESSION_RATIO,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "compression ratio",
		.min	= 0,
		.max	= 200,
		.def	= 30,
		.step	= 1,
	},
};

static const struct v4l2_ioctl_ops mchp_vcpp_ioctl_ops = {
	.vidioc_querycap = mchp_vcpp_querycap,
	.vidioc_try_fmt_vid_cap = mchp_vcpp_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = mchp_vcpp_s_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = mchp_vcpp_g_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap = mchp_vcpp_enum_fmt_vid_cap,

	.vidioc_enum_input = mchp_vcpp_enum_input,
	.vidioc_g_input = mchp_vcpp_g_input,
	.vidioc_s_input = mchp_vcpp_s_input,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations mchp_vcpp_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.mmap		= vb2_fop_mmap,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static int mchp_vcpp_graph_notify_bound(struct v4l2_async_notifier *notifier,
					struct v4l2_subdev *subdev,
					struct v4l2_async_connection *asc)
{
	struct mchp_vcpp_graph_entity *entity = to_mchp_vcpp_entity(asc);

	entity->entity = &subdev->entity;
	entity->subdev = subdev;

	return 0;
}

static int mchp_vcpp_graph_build_one(struct mchp_vcpp_fpga *mchp_vcpp,
				     struct mchp_vcpp_graph_entity *entity)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *local = entity->entity;
	struct media_entity *remote;
	struct media_pad *local_pad;
	struct media_pad *remote_pad;
	struct mchp_vcpp_graph_entity *graph_entity;
	struct v4l2_fwnode_link link;
	struct fwnode_handle *ep_fw_handle = NULL;
	int ret = 0;

	dev_dbg(mchp_vcpp->dev, "creating links for entity %s\n", local->name);

	while (1) {
		ep_fw_handle = fwnode_graph_get_next_endpoint(entity->asc.match.fwnode,
							      ep_fw_handle);
		if (!ep_fw_handle)
			break;

		ret = v4l2_fwnode_parse_link(ep_fw_handle, &link);
		if (ret < 0) {
			dev_err(mchp_vcpp->dev,
				"failed to parse link for %p\n", ep_fw_handle);
			continue;
		}

		if (link.local_port >= local->num_pads) {
			dev_err(mchp_vcpp->dev,
				"invalid port number %u for %p\n",
				link.local_port, link.local_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		local_pad = &local->pads[link.local_port];

		if (local_pad->flags & MEDIA_PAD_FL_SINK) {
			dev_dbg(mchp_vcpp->dev, "skipping sink port %p:%u\n",
				link.local_node, link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		if (link.remote_node == of_fwnode_handle(mchp_vcpp->dev->of_node)) {
			dev_dbg(mchp_vcpp->dev, "skipping DMA %p:%u\n",
				link.local_node, link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		graph_entity = mchp_vcpp_graph_find_entity(mchp_vcpp, link.remote_node);
		if (!graph_entity) {
			dev_dbg(mchp_vcpp->dev, "no entity found for %p\n",
				link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		remote = graph_entity->entity;

		if (link.remote_port >= remote->num_pads) {
			dev_err(mchp_vcpp->dev, "invalid port number %u on %p\n",
				link.remote_port, link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		remote_pad = &remote->pads[link.remote_port];

		v4l2_fwnode_put_link(&link);

		dev_info(mchp_vcpp->dev, "creating %s:%u -> %s:%u link\n",
			 local->name, local_pad->index,
			 remote->name, remote_pad->index);

		ret = media_create_pad_link(local, local_pad->index,
					    remote, remote_pad->index,
					    link_flags);
		if (ret < 0) {
			dev_err(mchp_vcpp->dev,
				"failed to create %s:%u -> %s:%u link\n",
				local->name, local_pad->index,
				remote->name, remote_pad->index);
			break;
		}
	}

	fwnode_handle_put(ep_fw_handle);

	return ret;
}

static int mchp_vcpp_graph_build_dma(struct mchp_vcpp_fpga *mchp_vcpp)
{
	struct device_node *node = mchp_vcpp->dev->of_node;
	struct media_entity *source;
	struct media_entity *sink;
	struct media_pad *source_pad;
	struct media_pad *sink_pad;
	struct mchp_vcpp_graph_entity *g_entity;
	struct v4l2_fwnode_link link;
	struct device_node *ep_node;
	int ret = 0;
	u32 link_flags = MEDIA_LNK_FL_ENABLED;

	ep_node = of_graph_get_next_endpoint(node, NULL);
	if (!ep_node)
		goto done;

	ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep_node), &link);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "failed to parse link for %pOF\n",
			ep_node);
		goto done;
	}

	dev_dbg(mchp_vcpp->dev, "creating link for DMA engine %s\n",
		mchp_vcpp->vdev.name);

	g_entity = mchp_vcpp_graph_find_entity(mchp_vcpp, link.remote_node);
	if (!g_entity) {
		dev_err(mchp_vcpp->dev, "no entity found for %pOF\n",
			to_of_node(link.remote_node));
		v4l2_fwnode_put_link(&link);
		ret = -ENODEV;
		goto done;
	}

	if (link.remote_port >= g_entity->entity->num_pads) {
		dev_err(mchp_vcpp->dev, "invalid port number %u on %pOF\n",
			link.remote_port, to_of_node(link.remote_node));
		v4l2_fwnode_put_link(&link);
		ret = -EINVAL;
		goto done;
	}

	source = g_entity->entity;
	source_pad = &source->pads[link.remote_port];
	sink = &mchp_vcpp->vdev.entity;
	sink_pad = &mchp_vcpp->vid_cap_pad;

	v4l2_fwnode_put_link(&link);

	dev_dbg(mchp_vcpp->dev, "creating %s:%u -> %s:%u link\n",
		source->name, source_pad->index, sink->name, sink_pad->index);

	ret = media_create_pad_link(source, source_pad->index, sink,
				    sink_pad->index, link_flags);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev,
			"failed to create %s:%u -> %s:%u link\n", source->name,
			source_pad->index, sink->name, sink_pad->index);
	}

done:
	return ret;
}

static int mchp_vcpp_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mchp_vcpp_fpga	*mchp_vcpp = container_of(notifier->v4l2_dev,
							  struct mchp_vcpp_fpga,
							  v4l2_dev);
	struct mchp_vcpp_graph_entity *entity;
	struct v4l2_async_connection *asc;
	int ret;

	dev_info(mchp_vcpp->dev, "notify complete, all subdevices registered\n");

	list_for_each_entry(asc, &mchp_vcpp->notifier.done_list, asc_entry) {
		entity = to_mchp_vcpp_entity(asc);
		ret = mchp_vcpp_graph_build_one(mchp_vcpp, entity);
		if (ret < 0)
			return ret;
	}

	ret = mchp_vcpp_graph_build_dma(mchp_vcpp);
	if (ret < 0)
		return ret;

	ret = v4l2_device_register_subdev_nodes(&mchp_vcpp->v4l2_dev);
	if (ret < 0)
		dev_err(mchp_vcpp->dev, "failed to register subdev nodes\n");

	return media_device_register(&mchp_vcpp->mdev);
}

static const struct v4l2_async_notifier_operations mchp_vcpp_v4l2_async_ops = {
	.bound = mchp_vcpp_graph_notify_bound,
	.complete = mchp_vcpp_graph_notify_complete,
};

static void mchp_vcpp_set_default_format(struct mchp_vcpp_fpga *mchp_vcpp)
{
	struct v4l2_pix_format *pix;

	mchp_vcpp->fmtinfo = mvc_get_format_by_fourcc(MCHP_VCPP_DEF_FORMAT);
	mchp_vcpp->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	pix = &mchp_vcpp->format.fmt.pix;
	pix->pixelformat = mchp_vcpp->fmtinfo->fourcc;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->field = V4L2_FIELD_NONE;
	pix->width = MCHP_VCPP_DEF_WIDTH;
	pix->height = MCHP_VCPP_DEF_HEIGHT;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
	pix->ycbcr_enc	= V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = 0;
	pix->bytesperline = pix->width * mchp_vcpp->fmtinfo->bpp;
	pix->sizeimage =
		pix->width * pix->height * mchp_vcpp->fmtinfo->bpp;
}

static void mchp_vcpp_graph_cleanup(struct mchp_vcpp_fpga *mchp_vcpp)
{
	v4l2_async_nf_unregister(&mchp_vcpp->notifier);
	v4l2_async_nf_cleanup(&mchp_vcpp->notifier);
}

static int mchp_vcpp_graph_parse_one(struct mchp_vcpp_fpga *mchp_vcpp,
				     struct fwnode_handle *fwnode)
{
	struct fwnode_handle *remote;
	struct fwnode_handle *ep_fw_handle = NULL;
	int ret = 0;

	dev_dbg(mchp_vcpp->dev, "parsing node %p\n", fwnode);

	while (1) {
		struct mchp_vcpp_graph_entity *graph_entity;

		ep_fw_handle = fwnode_graph_get_next_endpoint(fwnode, ep_fw_handle);
		if (!ep_fw_handle)
			break;

		remote = fwnode_graph_get_remote_port_parent(ep_fw_handle);
		if (!remote) {
			ret = -EINVAL;
			goto err_notifier_cleanup;
		}

		fwnode_handle_put(ep_fw_handle);

		if (remote == of_fwnode_handle(mchp_vcpp->dev->of_node) ||
		    mchp_vcpp_graph_find_entity(mchp_vcpp, remote)) {
			fwnode_handle_put(remote);
			continue;
		}

		graph_entity = v4l2_async_nf_add_fwnode(&mchp_vcpp->notifier,
							remote,
							struct mchp_vcpp_graph_entity);
		fwnode_handle_put(remote);
		if (IS_ERR(graph_entity)) {
			ret = PTR_ERR(graph_entity);
			goto err_notifier_cleanup;
		}
	}

	return 0;

err_notifier_cleanup:
	v4l2_async_nf_cleanup(&mchp_vcpp->notifier);
	fwnode_handle_put(ep_fw_handle);
	return ret;
}

static int mchp_vcpp_graph_parse(struct mchp_vcpp_fpga *mchp_vcpp)
{
	struct mchp_vcpp_graph_entity *entity;
	struct v4l2_async_connection *asc;
	int ret;

	ret = mchp_vcpp_graph_parse_one(mchp_vcpp, of_fwnode_handle(mchp_vcpp->dev->of_node));
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "mchp_vcpp_graph_parse_one error %d\n", ret);
		return 0;
	}

	list_for_each_entry(asc, &mchp_vcpp->notifier.waiting_list, asc_entry) {
		entity = to_mchp_vcpp_entity(asc);
		ret = mchp_vcpp_graph_parse_one(mchp_vcpp, entity->asc.match.fwnode);
		if (ret < 0) {
			v4l2_async_nf_cleanup(&mchp_vcpp->notifier);
			break;
		}
	}

	return ret;
}

static int mchp_vcpp_graph_init(struct mchp_vcpp_fpga *mchp_vcpp)
{
	int ret;

	/* Parse the graph to extract a list of subdevice DT nodes. */
	ret = mchp_vcpp_graph_parse(mchp_vcpp);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "graph parsing failed\n");
		goto err_graph_cleanup;
	}

	if (list_empty(&mchp_vcpp->notifier.waiting_list)) {
		dev_err(mchp_vcpp->dev, "no subdev found in graph\n");
		goto err_graph_cleanup;
	}

	mchp_vcpp->notifier.ops = &mchp_vcpp_v4l2_async_ops;

	ret = v4l2_async_nf_register(&mchp_vcpp->notifier);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "Failed to register notifier\n");
		goto err_graph_cleanup;
	}

	ret = 0;

err_graph_cleanup:
	if (ret < 0)
		mchp_vcpp_graph_cleanup(mchp_vcpp);

	return ret;
}

static int mchp_vcpp_probe(struct platform_device *pdev)
{
	struct mchp_vcpp_fpga *mchp_vcpp;
	struct vb2_queue *vb2_q;
	struct resource *res;
	struct video_device *vdev;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	int num_clks = ARRAY_SIZE(mchp_vcpp_clks);
	int ret;

	mchp_vcpp = devm_kzalloc(&pdev->dev,
				 sizeof(struct mchp_vcpp_fpga), GFP_KERNEL);
	if (!mchp_vcpp)
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_vcpp),
				     "kzalloc failed\n");

	mchp_vcpp->clks = devm_kmemdup(&pdev->dev, mchp_vcpp_clks,
				       sizeof(mchp_vcpp_clks), GFP_KERNEL);
	if (!mchp_vcpp->clks)
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_vcpp->clks),
				     "Failed to get clocks\n");

	mchp_vcpp->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(mchp_vcpp->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_vcpp->base),
				     "could not get mem resource\n");

	mchp_vcpp->irq = platform_get_irq(pdev, 0);
	if (mchp_vcpp->irq <= 0)
		return dev_err_probe(&pdev->dev, mchp_vcpp->irq,
				     "could not get irq\n");

	ret = clk_bulk_get(&pdev->dev, num_clks, mchp_vcpp->clks);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(num_clks, mchp_vcpp->clks);
	if (ret)
		goto err_clk_put;

	mchp_vcpp->reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset",
							GPIOD_OUT_LOW);
	if (IS_ERR(mchp_vcpp->reset_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_vcpp->reset_gpio),
				     "Failed to get reset gpio");

	if (mchp_vcpp->reset_gpio)
		gpiod_set_value_cansleep(mchp_vcpp->reset_gpio, 1);

	writel_relaxed(MCHP_VCPP_CORE_RESET, mchp_vcpp->base + MCHP_VCPP_CTRL_REG);

	mutex_init(&mchp_vcpp->lock);

	mchp_vcpp->pdev = pdev;
	mchp_vcpp->dev = &pdev->dev;

	mchp_vcpp_set_default_format(mchp_vcpp);

	ret = v4l2_device_register(&pdev->dev, &mchp_vcpp->v4l2_dev);
	if (ret)
		return ret;

	ctrl_hdlr = &mchp_vcpp->ctrl_handler;

	v4l2_ctrl_handler_init(ctrl_hdlr, MCHP_VCPP_NUM_CTRLS);

	mchp_vcpp->compression_ratio_ctrl = v4l2_ctrl_new_custom(ctrl_hdlr,
								 &mchp_vcpp_ctrls[0],
								 NULL);
	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto v4l2_unregister;
	}

	mchp_vcpp->v4l2_dev.ctrl_handler = ctrl_hdlr;

	INIT_LIST_HEAD(&mchp_vcpp->buf_list);
	spin_lock_init(&mchp_vcpp->qlock);

	vdev = &mchp_vcpp->vdev;
	strscpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->fops = &mchp_vcpp_fops,
	vdev->ioctl_ops = &mchp_vcpp_ioctl_ops,
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
			    V4L2_CAP_STREAMING;
	vdev->lock = &mchp_vcpp->lock;
	vdev->queue = &mchp_vcpp->queue;
	vdev->v4l2_dev = &mchp_vcpp->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->vfl_type = VFL_TYPE_VIDEO;

	/* Initialize media device */
	strscpy(mchp_vcpp->mdev.model, MCHP_VCPP_DRV_NAME, sizeof(mchp_vcpp->mdev.model));
	snprintf(mchp_vcpp->mdev.bus_info, sizeof(mchp_vcpp->mdev.bus_info),
		 "platform:%s", MCHP_VCPP_DRV_NAME);

	mchp_vcpp->mdev.dev = &mchp_vcpp->pdev->dev;
	media_device_init(&mchp_vcpp->mdev);
	mchp_vcpp->v4l2_dev.mdev = &mchp_vcpp->mdev;

	/* Media entity pads */
	mchp_vcpp->vid_cap_pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&mchp_vcpp->vdev.entity,
				     1, &mchp_vcpp->vid_cap_pad);
	if (ret) {
		dev_err(mchp_vcpp->dev, "Failed to init media entity pad\n");
		goto v4l2_unregister;
	}

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(mchp_vcpp->dev, "video register device failed %d\n",
			ret);
		goto v4l2_unregister;
	}

	vb2_q = &mchp_vcpp->queue;
	vb2_q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vb2_q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ | VB2_USERPTR;
	vb2_q->dev = mchp_vcpp->dev;
	vb2_q->drv_priv = mchp_vcpp;
	vb2_q->buf_struct_size = sizeof(struct mchp_vcpp_buffer);
	vb2_q->ops = &mchp_vcpp_qops;
	vb2_q->mem_ops = &vb2_dma_contig_memops;
	vb2_q->gfp_flags = GFP_DMA32;
	vb2_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb2_q->min_buffers_needed = 2;
	vb2_q->lock = &mchp_vcpp->lock;

	ret = vb2_queue_init(vb2_q);
	if (ret) {
		dev_err(mchp_vcpp->dev, "vb2 queue init failed %d\n", ret);
		goto v4l2_unregister;
	}

	v4l2_async_nf_init(&mchp_vcpp->notifier, &mchp_vcpp->v4l2_dev);

	ret = mchp_vcpp_graph_init(mchp_vcpp);
	if (ret < 0) {
		dev_err(mchp_vcpp->dev, "mchp dscmi graph init failed %d\n", ret);
		goto v4l2_unregister;
	}

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret)
		dev_dbg(&pdev->dev, "of_reserved_mem_device_init: %d\n", ret);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent: %d\n", ret);
		goto v4l2_unregister;
	}

	platform_set_drvdata(pdev, mchp_vcpp);

	video_set_drvdata(vdev, mchp_vcpp);

	return 0;

v4l2_unregister:
	mutex_destroy(&mchp_vcpp->lock);
	v4l2_device_unregister(&mchp_vcpp->v4l2_dev);
err_clk_put:
	clk_bulk_put(num_clks, mchp_vcpp->clks);

	return ret;
}

static int mchp_vcpp_remove(struct platform_device *pdev)
{
	struct v4l2_device *v4l2_dev = platform_get_drvdata(pdev);
	struct mchp_vcpp_fpga *mchp_vcpp = container_of(v4l2_dev,
							struct mchp_vcpp_fpga,
							v4l2_dev);
	int num_clks = ARRAY_SIZE(mchp_vcpp_clks);

	mutex_destroy(&mchp_vcpp->lock);
	v4l2_async_nf_unregister(&mchp_vcpp->notifier);
	v4l2_async_nf_cleanup(&mchp_vcpp->notifier);
	v4l2_device_unregister(&mchp_vcpp->v4l2_dev);
	clk_bulk_disable_unprepare(num_clks, mchp_vcpp->clks);
	clk_bulk_put(num_clks, mchp_vcpp->clks);

	return 0;
}

static const struct of_device_id mchp_vcpp_of_match[] = {
	{ .compatible = "microchip,video-dma-rtl-v0" },
	{}
};

MODULE_DEVICE_TABLE(of, mchp_vcpp_of_match);

static struct platform_driver mchp_vcpp_driver = {
	.probe = mchp_vcpp_probe,
	.remove = mchp_vcpp_remove,
	.driver = {
		.name = MCHP_VCPP_DRV_NAME,
		.of_match_table = mchp_vcpp_of_match,
	},
};

module_platform_driver(mchp_vcpp_driver);

MODULE_DESCRIPTION("Microchip Video Capture Pipeline Processing (Video DMA) Driver");
MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_LICENSE("GPL");
