// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Digital Serial Camera Memory Interface Driver.
 *
 * Copyright (C) 2021-2022 Microchip Technology Inc. and its subsidiaries
 * Author: Shravan Chippa <shavan.chippa@microchip.com>
 *
 * Driver based on stm32-dcmi.c
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kmod.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define MCHP_DSCMI_DRV_NAME			"mchp-dscmi"
#define MCHP_DSCMI_DRV_VERSION			"0.1"

/* Offset address to control Red, Green, Blue gain etc */
#define MCHP_DSCMI_R_CONSTRAINT			0x1004
#define MCHP_DSCMI_G_CONSTRAINT			0x1008
#define MCHP_DSCMI_B_CONSTRAINT			0x100C
#define MCHP_DSCMI_SECOND_CONSTRAINT		0x1010
#define MCHP_DSCMI_RGB_SUM			0x1038

/* Offset address to control space */
#define MCHP_DSCMI_FRAME_Q_FACTOR		0x1074
#define MCHP_DSCMI_FRAME_WIDTH			0x1078
#define MCHP_DSCMI_FRAME_HIGHT			0x107C
#define MCHP_DSCMI_FRAME_SIZE_REG		0x1080
#define MCHP_DSCMI_FRAME_START_REG		0x1084
#define MCHP_DSCMI_STREAM_ADDR_LOW		0x1088
#define MCHP_DSCMI_STREAM_ADDR_HIGH		0x108C

/* Offset address to get video capability */
#define MCHP_DSCMI_CAPABILITIES_V4L2		0x1500

#define MCHP_DSCMI_FRAME_START			0x1
#define MCHP_DSCMI_FRAME_STOP			0x0

#define MCHP_DSCMI_H264_NUM_CTRLS		8
#define MCHP_DSCMI_NUM_CTRLS			7

#define MCHP_DSCMI_CAM_POWER_ON			1
#define MCHP_DSCMI_CAM_POWER_OFF		0

#define MCHP_DSCMI_CAM_START			1
#define MCHP_DSCMI_CAM_STOP			0

#define MCHP_DSCMI_FRAME_SIZE_MASK		GENMASK(27, 0)
#define MCHP_DSCMI_FRAME_INDEX_MASK		GENMASK(29, 28)

#define MCHP_DSCMI_MAX_FRAMES			4
#define MCHP_DSCMI_FRAME_S_NEXT			(1024 * 1024)
#define MCHP_DSCMI_FRAME_MAX_SIZE		(1024 * 1024)

/* Video capabilities */
#define MCHP_DSCMI_CAPABILITIES_H264		0x48323634
#define MCHP_DSCMI_CAPABILITIES_MJPEG		0x4D4A5047
#define MCHP_DSCMI_CAPABILITIES_YUV		0x59555956

/* User defined v4l2 control */
#define MCHP_DSCMI_CID_RED_GAIN			(V4L2_CID_USER_BASE | 0x1001)
#define MCHP_DSCMI_CID_GREEN_GAIN		(V4L2_CID_USER_BASE | 0x1002)
#define MCHP_DSCMI_CID_BLUE_GAIN		(V4L2_CID_USER_BASE | 0x1003)
#define MCHP_DSCMI_CID_Q_FACTOR			(V4L2_CID_USER_BASE | 0x1004)

/* Default resolution */
#define MCHP_DSCMI_FIXED_WIDTH			1280
#define MCHP_DSCMI_FIXED_HEIGHT			720

#define MCHP_DSCMI_MAX_WIDTH			1920
#define MCHP_DSCMI_MAX_HEIGHT			1080

#define MCHP_DSCMI_GAIN_AVERAGE			125
#define MCHP_DSCMI_GAIN_MIN			5
#define MCHP_DSCMI_GAIN_INIT			80
#define MCHP_DSCMI_HYSTERESIS_GAIN		4
#define MCHP_DSCMI_GAIN_CTL_DEFAULT		112
#define MCHP_DSCMI_GAIN_CTL_MAX			255
#define MCHP_DSCMI_CTL_MAX			255
#define MCHP_DSCMI_CTL_MIN			0
#define MCHP_DSCMI_CTL_STEP			1
#define MCHP_DSCMI_Q_FACTOR_CTL_MAX		52
#define MCHP_DSCMI_Q_FACTOR_CTL_MIN		25
#define MCHP_DSCMI_Q_FACTOR_CTL_DEFAULT		30

/* Auto gain check delay in msecs*/
#define MCHP_DSCMI_DELAYED_WORK_TIME_M_SEC	150

/* Minimum wait time for camera to stabilize */
#define MCHP_DSCMI_DELAYED_CAM_M_SEC		100

enum mchp_dscmi_state {
	STOPPED = 0,
	WAIT_FOR_BUFFER,
	RUNNING,
};

enum mchp_dscmi_capabilities {
	H264 = 0,
	MJPEG,
	YUV_RAW,
};

/**
 * struct mchp_dscmi_framesize - size of the frame
 * @width:	frame width
 * @height:	frame height
 */
struct mchp_dscmi_framesize {
	u32 width;
	u32 height;
};

/**
 * struct mchp_dscmi_buffer - buffer for one video frame
 * @vb:		video buffer information struct vb2_v4l2_buffer
 * @prepared:	status of buffer
 * @paddr:	physical address of buffer
 * @size:	size of buffer
 * @list:	list of all requested buffers from user space
 */
struct mchp_dscmi_buffer {
	struct vb2_v4l2_buffer vb;
	bool prepared;
	dma_addr_t paddr;
	size_t size;
	struct list_head list;
};

/**
 * struct mchp_dscmi_cam_buffer - camera dma buffers and size
 * @paddr:	camera stream base address
 * @size:	size of buffer
 */
struct mchp_dscmi_cam_buffer {
	dma_addr_t paddr;
	size_t size;
};

/**
 * struct mchp_dscmi_format - media bus format information
 * @fourcc:	Fourcc code for this format
 * @mbus_code:	V4L2 media bus format code
 * @bpp:	Bytes per pixel (when stored in memory)
 */
struct mchp_dscmi_format {
	u32 fourcc;
	u32 mbus_code;
	u8 bpp;
};

/**
 * struct mchp_dscmi_fpga - V4L2 device context
 * @base:		pointer to control register
 * @pdev:		platform device
 * @dev:		device
 * @active:		current buffer in queue
 * @current_subdev:	current subdevice: the camera sensor
 * @reset_gpio:		sensor fabric rest
 * @dma_chan:		DMA engine channel
 * @auto_gain_wq:	auto gain control work queue struct
 * @auto_gain_dw:	auto gain delayed work struct
 * @v4l2_dev:		top-level v4l2 device struct
 * @vdev:		video node structure
 * @fmt:		current v4l2 format
 * @queue:		vb2 video capture queue
 * @lock:		mutex lock for vb2 buffs
 * @qlock:		spinlock controlling access to buf_list and sequence
 * @buf_list:		list of buffers queued for DMA
 * @subdev_entities:	subdevice list
 * @ctrl_handler:	control handler structure
 * @dma_cookie:		DMA engine cookie
 * @dma_lock:		DMA mutex lock for serializing dma use
 * @cambuf:		struct cam_buffer
 * @state:		state of buffers
 * @capabilities:	capabilities to identify the fabric v4l2 support
 * @s_buff_index:	current streaming buf index
 * @s_buff_size:	current streaming buf size
 * @irq:		external IRQ for new frame
 * @contrast:		contrast value
 * @brightness:		brightness value
 * @sequence:		frame sequence counter
 * @drop_count:		total frame drop counter
 */
struct mchp_dscmi_fpga {
	void __iomem *base;
	struct platform_device *pdev;
	struct device *dev;
	struct mchp_dscmi_buffer *active;
	struct mchp_dscmi_subdev_entity *current_subdev;
	struct gpio_desc *reset_gpio;
	struct dma_chan	 *dma_chan;
	struct workqueue_struct	*auto_gain_wq;
	struct delayed_work auto_gain_dw;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_format fmt;
	struct vb2_queue queue;
	struct mutex lock; /* vb2 buffer lock */
	spinlock_t qlock;
	struct list_head buf_list;
	struct list_head subdev_entities;
	struct v4l2_ctrl_handler ctrl_handler;
	struct mutex dma_lock; /* mutex for dma operations */
	struct mchp_dscmi_cam_buffer cambuf;
	enum mchp_dscmi_state state;
	enum mchp_dscmi_capabilities capabilities;
	dma_cookie_t dma_cookie;
	int s_buff_index;
	int s_buff_size;
	int irq;
	int contrast;
	int brightness;
	int sequence;
	int drop_count;
};

struct mchp_dscmi_subdev_entity {
	struct v4l2_subdev *subdev;
	struct v4l2_async_subdev *async_subdev;
	struct device_node *epn;
	struct v4l2_async_notifier notifier;
	struct list_head list;
};

static struct mchp_dscmi_framesize framesize_list[] = {
	{
		.width = 1920,
		.height = 1072,
	},
	{
		.width = 1280,
		.height = 720,
	},
	{
		.width = 960,
		.height = 544,
	},
	{
		.width = 640,
		.height = 480,
	},
	{
		.width = 432,
		.height = 240,
	},
	{},
};

static struct mchp_dscmi_format mchp_dscmi_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_H264,
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
	}, {
		.fourcc	= V4L2_PIX_FMT_MJPEG,
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
	}, {
		.fourcc = V4L2_PIX_FMT_YUYV,
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.bpp = 2,
	},
};

static inline void mchp_dscmi_reg_write(struct mchp_dscmi_fpga *mchp_dscmi,
					u32 reg, u32 val)
{
	writel_relaxed(val, mchp_dscmi->base + reg);
}

static inline u32 mchp_dscmi_reg_read(struct mchp_dscmi_fpga *mchp_dscmi,
				      u32 reg)
{
	return readl_relaxed(mchp_dscmi->base + reg);
}

static void mchp_dscmi_buffer_done(struct mchp_dscmi_fpga *mchp_dscmi,
				   struct mchp_dscmi_buffer *buf,
				   size_t bytesused,
				   int err)
{
	struct vb2_v4l2_buffer *vbuf;

	if (!buf)
		return;

	list_del_init(&buf->list);
	vbuf = &buf->vb;
	vbuf->sequence = mchp_dscmi->sequence++;
	vbuf->field = V4L2_FIELD_NONE;
	vbuf->vb2_buf.timestamp = ktime_get_ns();

	vb2_set_plane_payload(&vbuf->vb2_buf, 0, bytesused);

	vb2_buffer_done(&vbuf->vb2_buf,
			err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);

	dev_dbg(mchp_dscmi->dev, "Buffer[%d] done seq=%d, bytesused=%zu\n",
		vbuf->vb2_buf.index, vbuf->sequence, bytesused);

	mchp_dscmi->active = NULL;
}

static void mchp_dscmi_dma_callback(void *param)
{
	struct mchp_dscmi_fpga *mchp_dscmi = param;
	struct mchp_dscmi_buffer *buf = mchp_dscmi->active;
	struct dma_tx_state state;
	enum dma_status status;

	spin_lock_irq(&mchp_dscmi->qlock);

	status = dmaengine_tx_status(mchp_dscmi->dma_chan,
				     mchp_dscmi->dma_cookie, &state);

	switch (status) {
	case DMA_ERROR:
		dev_err(mchp_dscmi->dev, "Received DMA_ERROR\n");
		mchp_dscmi_buffer_done(mchp_dscmi, buf, 0, -EIO);
		break;
	case DMA_COMPLETE:
		dev_dbg(mchp_dscmi->dev, "Received DMA_COMPLETE\n");
		mchp_dscmi_buffer_done(mchp_dscmi, buf,
				       mchp_dscmi->s_buff_size, 0);
		break;

	default:
		dev_err(mchp_dscmi->dev, "Received unknown status\n");
		break;
	}

	spin_unlock_irq(&mchp_dscmi->qlock);
}

static int mchp_dscmi_start_dma(struct mchp_dscmi_fpga *mchp_dscmi,
				struct mchp_dscmi_buffer *buf)
{
	struct dma_async_tx_descriptor *desc = NULL;
	struct dma_slave_config config;
	int ret, buf_size;

	memset(&config, 0, sizeof(config));

	spin_lock_irq(&mchp_dscmi->qlock);
	config.src_addr = mchp_dscmi->cambuf.paddr +
		(MCHP_DSCMI_FRAME_S_NEXT * mchp_dscmi->s_buff_index);

	buf_size = mchp_dscmi->s_buff_size;
	spin_unlock_irq(&mchp_dscmi->qlock);

	config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.dst_maxburst = 4;

	ret = dmaengine_slave_config(mchp_dscmi->dma_chan, &config);
	if (ret) {
		dev_err(mchp_dscmi->dev,
			"DMA channel config failed (%d)\n", ret);
		return ret;
	}

	mutex_lock(&mchp_dscmi->dma_lock);

	desc = dmaengine_prep_dma_memcpy(mchp_dscmi->dma_chan, buf->paddr,
					 config.src_addr, buf_size,
					 DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err(mchp_dscmi->dev,
			"DMA failed for buffer src=%pad dst=%pad size=%d\n",
			 &config.src_addr, &buf->paddr, buf_size);
		mutex_unlock(&mchp_dscmi->dma_lock);
		return -EINVAL;
	}

	desc->callback = mchp_dscmi_dma_callback;
	desc->callback_param = mchp_dscmi;

	mchp_dscmi->dma_cookie = dmaengine_submit(desc);

	ret = dma_submit_error(mchp_dscmi->dma_cookie);
	mutex_unlock(&mchp_dscmi->dma_lock);
	if (ret) {
		dev_err(mchp_dscmi->dev, "DMA submission failed %d\n", ret);
		return -ENXIO;
	}

	dma_async_issue_pending(mchp_dscmi->dma_chan);

	return 0;
}

static int mchp_dscmi_restart_capture(struct mchp_dscmi_fpga *mchp_dscmi)
{
	struct mchp_dscmi_buffer *buf;

	spin_lock_irq(&mchp_dscmi->qlock);

	if (list_empty(&mchp_dscmi->buf_list)) {
		dev_dbg(mchp_dscmi->dev,
			"Capture restart is deferred to next buffer queue\n");
		mchp_dscmi->state = WAIT_FOR_BUFFER;
		mchp_dscmi->drop_count++;
		spin_unlock_irq(&mchp_dscmi->qlock);
		return 0;
	}

	buf = list_entry(mchp_dscmi->buf_list.next,
			 struct mchp_dscmi_buffer, list);

	mchp_dscmi->active = buf;

	spin_unlock_irq(&mchp_dscmi->qlock);

	return mchp_dscmi_start_dma(mchp_dscmi, buf);
}

static irqreturn_t mchp_dscmi_irq_ext(int irq, void *dev_id)
{
	struct mchp_dscmi_fpga *mchp_dscmi = dev_id;
	int frame_info;
	unsigned long flags;

	frame_info = mchp_dscmi_reg_read(mchp_dscmi, MCHP_DSCMI_FRAME_SIZE_REG);

	spin_lock_irqsave(&mchp_dscmi->qlock, flags);

	if (mchp_dscmi->state != RUNNING) {
		spin_unlock_irqrestore(&mchp_dscmi->qlock, flags);
		mchp_dscmi->drop_count++;
		return IRQ_HANDLED;
	}

	mchp_dscmi->s_buff_index = FIELD_GET(MCHP_DSCMI_FRAME_INDEX_MASK, frame_info);

	mchp_dscmi->s_buff_size = frame_info & MCHP_DSCMI_FRAME_SIZE_MASK;

	if (mchp_dscmi->s_buff_size >= MCHP_DSCMI_FRAME_S_NEXT ||
	    mchp_dscmi->s_buff_size == 0) {
		spin_unlock_irqrestore(&mchp_dscmi->qlock, flags);
		mchp_dscmi->drop_count++;
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&mchp_dscmi->qlock, flags);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mchp_dscmi_irq_thread_fn(int irq, void *dev_id)
{
	struct mchp_dscmi_fpga *mchp_dscmi = dev_id;
	int ret;

	ret = mchp_dscmi_restart_capture(mchp_dscmi);
	if (ret)
		dev_warn_once(mchp_dscmi->dev,
			      "failed to mchp restart capture %d\n", ret);

	return IRQ_HANDLED;
}

static int mchp_dscmi_queue_setup(struct vb2_queue *vq,
				  unsigned int *nbuffers, unsigned int *nplanes,
				  unsigned int sizes[], struct device *alloc_devs[])
{
	struct mchp_dscmi_fpga *mchp_dscmi = vb2_get_drv_priv(vq);
	unsigned int size;

	if (*nbuffers > MCHP_DSCMI_MAX_FRAMES) {
		dev_dbg(mchp_dscmi->dev,
			"output frame count too high (%d), cut to %d\n",
				 *nbuffers, MCHP_DSCMI_MAX_FRAMES);
		*nbuffers = MCHP_DSCMI_MAX_FRAMES;
	}

	size = mchp_dscmi->fmt.fmt.pix.sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;

	dev_dbg(mchp_dscmi->dev, "Setup queue, count=%d, size=%d\n",
		*nbuffers, size);

	return 0;
}

static int mchp_dscmi_buffer_prepare(struct vb2_buffer *vb)
{
	struct mchp_dscmi_fpga *mchp_dscmi = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mchp_dscmi_buffer *buf =
			container_of(vbuf, struct mchp_dscmi_buffer, vb);
	unsigned long size;

	size = mchp_dscmi->fmt.fmt.pix.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(mchp_dscmi->dev,
			"data will not fit into plane (%lu < %lu)\n",
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

static void mchp_dscmi_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mchp_dscmi_fpga *mchp_dscmi = vb2_get_drv_priv(vb->vb2_queue);
	struct mchp_dscmi_buffer *buf =
				container_of(vbuf, struct mchp_dscmi_buffer, vb);

	spin_lock_irq(&mchp_dscmi->qlock);
	list_add_tail(&buf->list, &mchp_dscmi->buf_list);

	if (mchp_dscmi->state == WAIT_FOR_BUFFER) {
		mchp_dscmi->state = RUNNING;
		mchp_dscmi->active = buf;
	}

	spin_unlock_irq(&mchp_dscmi->qlock);
}

static void return_all_buffers(struct mchp_dscmi_fpga *mchp_dscmi,
			       enum vb2_buffer_state state)
{
	struct mchp_dscmi_buffer *buf, *node;

	list_for_each_entry_safe(buf, node, &mchp_dscmi->buf_list, list) {
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		list_del(&buf->list);
	}
	mchp_dscmi->active = NULL;
}

static int mchp_dscmi_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mchp_dscmi_fpga *mchp_dscmi = vb2_get_drv_priv(vq);
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;

	int ret;

	v4l2_subdev_call(subdev, core, s_power, MCHP_DSCMI_CAM_POWER_ON);

	ret = v4l2_subdev_call(subdev, video, s_stream, MCHP_DSCMI_CAM_START);
	if (ret && ret != -ENOIOCTLCMD) {
		dev_err(mchp_dscmi->dev,
			"stream enable failed in subdev %d\n", ret);
		goto err_free_buffers;
	}

	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_FRAME_START_REG, MCHP_DSCMI_FRAME_STOP);
	mdelay(MCHP_DSCMI_DELAYED_CAM_M_SEC);
	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_FRAME_START_REG,
			     MCHP_DSCMI_FRAME_START);

	mchp_dscmi->sequence = 0;
	mchp_dscmi->drop_count = 0;
	mchp_dscmi->s_buff_index = 0;
	mchp_dscmi->s_buff_size = 0;

	ret = request_threaded_irq(mchp_dscmi->irq, mchp_dscmi_irq_ext,
				   mchp_dscmi_irq_thread_fn, IRQF_NO_SUSPEND,
				   KBUILD_MODNAME, mchp_dscmi);

	if (ret) {
		dev_err(mchp_dscmi->dev, "request threaded irq failed %d\n",
			ret);
		goto err_free_buffers;
	}

	spin_lock_irq(&mchp_dscmi->qlock);

	if (list_empty(&mchp_dscmi->buf_list)) {
		dev_dbg(mchp_dscmi->dev,
			"Start streaming is deferred to next buffer queue\n");
		mchp_dscmi->state = WAIT_FOR_BUFFER;
		spin_unlock_irq(&mchp_dscmi->qlock);
		return 0;
	}

	mchp_dscmi->state = RUNNING;

	spin_unlock_irq(&mchp_dscmi->qlock);

	return 0;

err_free_buffers:
	spin_lock_irq(&mchp_dscmi->qlock);
	return_all_buffers(mchp_dscmi, VB2_BUF_STATE_QUEUED);
	spin_unlock_irq(&mchp_dscmi->qlock);
	return ret;
}

static void mchp_dscmi_stop_streaming(struct vb2_queue *vq)
{
	struct mchp_dscmi_fpga *mchp_dscmi = vb2_get_drv_priv(vq);
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;

	free_irq(mchp_dscmi->irq, mchp_dscmi);

	spin_lock_irq(&mchp_dscmi->qlock);

	return_all_buffers(mchp_dscmi, VB2_BUF_STATE_QUEUED);

	mchp_dscmi->active = NULL;
	mchp_dscmi->state = STOPPED;

	spin_unlock_irq(&mchp_dscmi->qlock);

	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_FRAME_START_REG, MCHP_DSCMI_FRAME_STOP);

	v4l2_subdev_call(subdev, video, s_stream, MCHP_DSCMI_CAM_STOP);

	v4l2_subdev_call(subdev, core, s_power, MCHP_DSCMI_CAM_POWER_OFF);

	dev_dbg(mchp_dscmi->dev, "Capture frame count %d & drop count %d\n",
		mchp_dscmi->sequence, mchp_dscmi->drop_count);
}

static const struct vb2_ops mchp_dscmi_qops = {
	.queue_setup		= mchp_dscmi_queue_setup,
	.buf_prepare		= mchp_dscmi_buffer_prepare,
	.buf_queue		= mchp_dscmi_buffer_queue,
	.start_streaming	= mchp_dscmi_start_streaming,
	.stop_streaming		= mchp_dscmi_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int mchp_dscmi_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	strscpy(cap->driver, MCHP_DSCMI_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MCHP Camera Memory Interface", sizeof(cap->card));
	strscpy(cap->bus_info, "platform: mchp-dscmi", sizeof(cap->bus_info));

	return 0;
}

static int mchp_dscmi_try_fmt(struct mchp_dscmi_fpga *mchp_dscmi,
			      struct v4l2_format *fmt)
{
	struct v4l2_pix_format *pix = &fmt->fmt.pix;
	struct v4l2_pix_format *pix_present = &mchp_dscmi->fmt.fmt.pix;
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;
	struct mchp_dscmi_framesize *framessize;
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	v4l2_fill_mbus_format(&format.format, pix,
			      mchp_dscmi_formats[mchp_dscmi->capabilities].mbus_code);

	ret = v4l2_subdev_call(subdev, pad, set_fmt, NULL, &format);
	if (ret < 0)
		return ret;

	framessize = v4l2_find_nearest_size(framesize_list,
					    ARRAY_SIZE(framesize_list),
					    width, height,
					    pix->width, pix->height);

	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_FRAME_WIDTH, framessize->width);
	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_FRAME_HIGHT, framessize->height);

	/* Driver supports only fixed format */
	pix->pixelformat = mchp_dscmi_formats[mchp_dscmi->capabilities].fourcc;
	pix->width = framessize->width;
	pix->height = framessize->height;
	pix->field = pix_present->field;
	pix->colorspace = pix_present->colorspace;
	pix->ycbcr_enc = pix_present->ycbcr_enc;
	pix->quantization = pix_present->quantization;
	pix->xfer_func = pix_present->xfer_func;
	pix->sizeimage = pix_present->sizeimage;

	return 0;
}

static int mchp_dscmi_try_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_format *fmt)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);

	mchp_dscmi_try_fmt(mchp_dscmi, fmt);

	return 0;
}

static int mchp_dscmi_s_fmt_vid_cap(struct file *file, void *priv,
				    struct v4l2_format *fmt)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);

	if (vb2_is_streaming(&mchp_dscmi->queue))
		return -EBUSY;

	return mchp_dscmi_try_fmt_vid_cap(file, priv, fmt);
}

static int mchp_dscmi_g_fmt_vid_cap(struct file *file, void *priv,
				    struct v4l2_format *fmt)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);

	*fmt = mchp_dscmi->fmt;

	return 0;
}

static int mchp_dscmi_enum_fmt_vid_cap(struct file *file, void *priv,
				       struct v4l2_fmtdesc *fmt)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);

	if (fmt->index > 0)
		return -EINVAL;

	fmt->pixelformat = mchp_dscmi_formats[mchp_dscmi->capabilities].fourcc;
	return 0;
}

static int mchp_dscmi_enum_input(struct file *file, void *priv,
				 struct v4l2_input *input)
{
	if (input->index != 0)
		return -EINVAL;

	input->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(input->name, "Camera", sizeof(input->name));
	return 0;
}

static int mchp_dscmi_s_input(struct file *file, void *priv, unsigned int index)
{
	if (index > 0)
		return -EINVAL;

	return 0;
}

static int mchp_dscmi_g_input(struct file *file, void *priv, unsigned int *index)
{
	*index = 0;
	return 0;
}

static int mchp_dscmi_enum_framesizes(struct file *file, void *fh,
				      struct v4l2_frmsizeenum *fsize)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);

	if (fsize->index > 0)
		return -EINVAL;

	if (fsize->pixel_format !=
			mchp_dscmi_formats[mchp_dscmi->capabilities].fourcc)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = MCHP_DSCMI_FIXED_WIDTH;
	fsize->discrete.height = MCHP_DSCMI_FIXED_HEIGHT;

	return 0;
}

static int mchp_dscmi_g_parm(struct file *file, void *priv,
			     struct v4l2_streamparm *parm)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;

	return v4l2_g_parm_cap(video_devdata(file), subdev, parm);
}

static int mchp_dscmi_s_parm(struct file *file, void *priv,
			     struct v4l2_streamparm *p)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;

	return v4l2_s_parm_cap(video_devdata(file), subdev, p);
}

static int mchp_dscmi_enum_frameintervals(struct file *file, void *fh,
					  struct v4l2_frmivalenum *fival)
{
	struct mchp_dscmi_fpga *mchp_dscmi = video_drvdata(file);
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;
	struct v4l2_subdev_frame_interval_enum fie = {
		.index = fival->index,
		.width = fival->width,
		.height = fival->height,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	if (fival->pixel_format !=
			mchp_dscmi_formats[mchp_dscmi->capabilities].fourcc)
		return -EINVAL;

	fie.code = mchp_dscmi_formats[mchp_dscmi->capabilities].mbus_code;

	ret = v4l2_subdev_call(subdev, pad, enum_frame_interval, NULL, &fie);
	if (ret)
		return ret;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = fie.interval;

	return 0;
}

/*
 * The calculated parameters are used by Image Enhancement IP UG0646 running
 * in the FPGA logic. The equations used for calculation are explained in
 * UG0646 page number 2 in the below link
 *
 * (https://ww1.microchip.com/downloads/aemDocuments/documents/FPGA/ProductDocuments/UserGuides/microsemi_image_enhancement_ip_user_guide_ug0646_v3.pdf)
 *
 * V4l2 controls like brightness, contrast, r_gain, g_gain and b_gain are
 * calulated based on two functions second_constraint_cal() and
 * contrast_scale_cal()
 */

static inline int second_constraint_cal(int brightness, int contrast_scale)
{
	return (128 * ((brightness) - ((128 * (contrast_scale)) / 10)));
}

static inline int contrast_scale_cal(int contrast)
{
	return ((325 * (contrast + 128) / (387 - contrast)) >> 5u);
}

static int mchp_dscmi_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mchp_dscmi_fpga *mchp_dscmi;
	u32 contrast_scale, second_constraint, r_gain, g_gain, b_gain;

	mchp_dscmi = container_of(ctrl->handler,
				  struct mchp_dscmi_fpga, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		mchp_dscmi->brightness = ctrl->val;
		contrast_scale = contrast_scale_cal(mchp_dscmi->contrast);
		second_constraint = second_constraint_cal(ctrl->val,
							  contrast_scale);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_SECOND_CONSTRAINT,
				     second_constraint);
		break;
	case V4L2_CID_CONTRAST:
		mchp_dscmi->contrast = ctrl->val;
		contrast_scale = contrast_scale_cal(ctrl->val);
		second_constraint = second_constraint_cal(mchp_dscmi->brightness,
							  contrast_scale);

		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_SECOND_CONSTRAINT,
				     second_constraint);

		break;
	case MCHP_DSCMI_CID_RED_GAIN:
		contrast_scale = contrast_scale_cal(mchp_dscmi->contrast);
		r_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_R_CONSTRAINT, r_gain);
		break;
	case MCHP_DSCMI_CID_GREEN_GAIN:
		contrast_scale = contrast_scale_cal(mchp_dscmi->contrast);
		g_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_G_CONSTRAINT, g_gain);
		break;
	case MCHP_DSCMI_CID_BLUE_GAIN:
		contrast_scale = contrast_scale_cal(mchp_dscmi->contrast);
		b_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_B_CONSTRAINT, b_gain);
		break;
	case V4L2_CID_GAIN:
		contrast_scale = contrast_scale_cal(mchp_dscmi->contrast);
		r_gain = ((ctrl->val * contrast_scale) / 10);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_R_CONSTRAINT, r_gain);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_G_CONSTRAINT, r_gain);
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_B_CONSTRAINT, r_gain);
		break;
	case MCHP_DSCMI_CID_Q_FACTOR:
		mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_FRAME_Q_FACTOR,
				     ctrl->val);
		break;
	case V4L2_CID_AUTOGAIN:
		if (ctrl->val)
			queue_delayed_work(mchp_dscmi->auto_gain_wq,
					   &mchp_dscmi->auto_gain_dw,
					   msecs_to_jiffies(MCHP_DSCMI_DELAYED_WORK_TIME_M_SEC));
		else
			cancel_delayed_work(&mchp_dscmi->auto_gain_dw);

		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mchp_dscmi_ctrl_ops = {
	.s_ctrl = mchp_dscmi_s_ctrl,
};

static const struct v4l2_ioctl_ops mchp_dscmi_ioctl_ops = {
	.vidioc_querycap = mchp_dscmi_querycap,
	.vidioc_try_fmt_vid_cap = mchp_dscmi_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = mchp_dscmi_s_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = mchp_dscmi_g_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap = mchp_dscmi_enum_fmt_vid_cap,

	.vidioc_enum_input = mchp_dscmi_enum_input,
	.vidioc_g_input = mchp_dscmi_g_input,
	.vidioc_s_input = mchp_dscmi_s_input,

	.vidioc_g_parm = mchp_dscmi_g_parm,
	.vidioc_s_parm = mchp_dscmi_s_parm,
	.vidioc_enum_framesizes = mchp_dscmi_enum_framesizes,
	.vidioc_enum_frameintervals = mchp_dscmi_enum_frameintervals,

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

static const struct v4l2_file_operations mchp_dscmi_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.mmap		= vb2_fop_mmap,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static struct v4l2_format default_fmt[] = {
	{
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = {
			.width		= MCHP_DSCMI_FIXED_WIDTH,
			.height		= MCHP_DSCMI_FIXED_HEIGHT,
			.pixelformat	= V4L2_PIX_FMT_H264,
			.bytesperline	= 0,
			.sizeimage	= MCHP_DSCMI_FRAME_MAX_SIZE,
			.field		= V4L2_FIELD_NONE,
			.ycbcr_enc	= V4L2_YCBCR_ENC_DEFAULT,
			.colorspace	= V4L2_COLORSPACE_RAW,
			.xfer_func	= V4L2_XFER_FUNC_NONE,
		},
	}, {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = {
			.width		= MCHP_DSCMI_FIXED_WIDTH,
			.height		= MCHP_DSCMI_FIXED_HEIGHT,
			.pixelformat	= V4L2_PIX_FMT_MJPEG,
			.bytesperline	= 0,
			.sizeimage	= MCHP_DSCMI_FRAME_MAX_SIZE,
			.field		= V4L2_FIELD_NONE,
			.ycbcr_enc	= V4L2_YCBCR_ENC_DEFAULT,
			.colorspace	= V4L2_COLORSPACE_RAW,
			.xfer_func	= V4L2_XFER_FUNC_NONE,
		},
	},
};

static const struct v4l2_ctrl_config mchp_dscmi_gain_ctrls[] = {
	{
		.ops	= &mchp_dscmi_ctrl_ops,
		.id	= MCHP_DSCMI_CID_RED_GAIN,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Gain, Red",
		.min	= MCHP_DSCMI_CTL_MIN,
		.max	= MCHP_DSCMI_GAIN_CTL_MAX,
		.def	= MCHP_DSCMI_GAIN_CTL_DEFAULT,
		.step	= MCHP_DSCMI_CTL_STEP,
	}, {
		.ops	= &mchp_dscmi_ctrl_ops,
		.id	= MCHP_DSCMI_CID_GREEN_GAIN,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Gain, Green",
		.min	= MCHP_DSCMI_CTL_MIN,
		.max	= MCHP_DSCMI_GAIN_CTL_MAX,
		.def	= MCHP_DSCMI_GAIN_CTL_DEFAULT,
		.step	= MCHP_DSCMI_CTL_STEP,
	}, {
		.ops	= &mchp_dscmi_ctrl_ops,
		.id	= MCHP_DSCMI_CID_BLUE_GAIN,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Gain, Blue",
		.min	= MCHP_DSCMI_CTL_MIN,
		.max	= MCHP_DSCMI_GAIN_CTL_MAX,
		.def	= MCHP_DSCMI_GAIN_CTL_DEFAULT,
		.step	= MCHP_DSCMI_CTL_STEP,
	}, {
		.ops	= &mchp_dscmi_ctrl_ops,
		.id	= MCHP_DSCMI_CID_Q_FACTOR,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Quality Factor",
		.min	= MCHP_DSCMI_Q_FACTOR_CTL_MIN,
		.max	= MCHP_DSCMI_Q_FACTOR_CTL_MAX,
		.def	= MCHP_DSCMI_Q_FACTOR_CTL_DEFAULT,
		.step	= MCHP_DSCMI_CTL_STEP,
	},
};

static int mchp_dscmi_formats_init(struct mchp_dscmi_fpga *mchp_dscmi)
{
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;
	struct v4l2_subdev_mbus_code_enum mbus_code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.index = 0,
	};

	/*
	 * Read all mbus codes supported by the camera sensor and return error
	 * if it is not matching with mbus codes supported by the driver
	 */

	while (!v4l2_subdev_call(subdev, pad, enum_mbus_code, NULL, &mbus_code)) {
		dev_dbg(mchp_dscmi->dev, "mbus_code.index %d\n", mbus_code.index);

		if (mchp_dscmi_formats[0].mbus_code == mbus_code.code)
			return 0;
		mbus_code.index++;
	}

	return -ENXIO;
}

static int mchp_dscmi_graph_notify_bound(struct v4l2_async_notifier *notifier,
					 struct v4l2_subdev *subdev,
					 struct v4l2_async_subdev *async_subdev)
{
	struct mchp_dscmi_subdev_entity *subdev_entity =
		container_of(notifier, struct mchp_dscmi_subdev_entity, notifier);

	subdev_entity->subdev = subdev;

	return 0;
}

static void mchp_dscmi_graph_notify_unbind(struct v4l2_async_notifier *notifier,
					   struct v4l2_subdev *subdev,
					   struct v4l2_async_subdev *async_subdev)
{
	struct mchp_dscmi_fpga *mchp_dscmi = container_of(notifier->v4l2_dev,
					     struct mchp_dscmi_fpga, v4l2_dev);

	video_unregister_device(&mchp_dscmi->vdev);
	v4l2_ctrl_handler_free(&mchp_dscmi->ctrl_handler);
}

static int mchp_dscmi_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mchp_dscmi_fpga	*mchp_dscmi = container_of(notifier->v4l2_dev,
							   struct mchp_dscmi_fpga,
							   v4l2_dev);
	struct vb2_queue *vb2_q;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct video_device *vdev;
	int ret;

	ret = v4l2_device_register_subdev_nodes(&mchp_dscmi->v4l2_dev);
	if (ret < 0) {
		dev_err(mchp_dscmi->dev, "Failed to register subdev nodes\n");
		return ret;
	}

	mchp_dscmi->current_subdev = container_of(notifier,
						  struct mchp_dscmi_subdev_entity,
						  notifier);

	ctrl_hdlr = &mchp_dscmi->ctrl_handler;

	if (mchp_dscmi->capabilities == H264)
		v4l2_ctrl_handler_init(ctrl_hdlr, MCHP_DSCMI_H264_NUM_CTRLS);
	else
		v4l2_ctrl_handler_init(ctrl_hdlr, MCHP_DSCMI_NUM_CTRLS);

	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_dscmi_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, MCHP_DSCMI_CTL_MIN, MCHP_DSCMI_CTL_MAX,
			  MCHP_DSCMI_CTL_STEP, MCHP_DSCMI_CTL_MAX / 2);
	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_dscmi_ctrl_ops,
			  V4L2_CID_CONTRAST, MCHP_DSCMI_CTL_MIN, MCHP_DSCMI_CTL_MAX,
			  MCHP_DSCMI_CTL_STEP, MCHP_DSCMI_CTL_MAX / 2);
	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_dscmi_ctrl_ops,
			  V4L2_CID_GAIN, MCHP_DSCMI_CTL_MIN, MCHP_DSCMI_CTL_MAX,
			  MCHP_DSCMI_CTL_STEP, MCHP_DSCMI_GAIN_CTL_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &mchp_dscmi_ctrl_ops,
			  V4L2_CID_AUTOGAIN, MCHP_DSCMI_CTL_MIN, 1, MCHP_DSCMI_CTL_STEP, 0);

	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_dscmi_gain_ctrls[0], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_dscmi_gain_ctrls[1], NULL);
	v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_dscmi_gain_ctrls[2], NULL);

	if (mchp_dscmi->capabilities == H264)
		v4l2_ctrl_new_custom(ctrl_hdlr, &mchp_dscmi_gain_ctrls[3], NULL);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		return ret;
	}
	mchp_dscmi->v4l2_dev.ctrl_handler = ctrl_hdlr;

	ret = mchp_dscmi_formats_init(mchp_dscmi);
	if (ret) {
		dev_err(mchp_dscmi->dev, "No supported mediabus format found\n");
		goto free_ctrl_hdlr;
	}

	mchp_dscmi->fmt = default_fmt[mchp_dscmi->capabilities];

	vb2_q = &mchp_dscmi->queue;
	vb2_q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vb2_q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	vb2_q->dev = mchp_dscmi->dev;
	vb2_q->drv_priv = mchp_dscmi;
	vb2_q->buf_struct_size = sizeof(struct mchp_dscmi_buffer);
	vb2_q->ops = &mchp_dscmi_qops;
	vb2_q->mem_ops = &vb2_dma_contig_memops;
	vb2_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb2_q->min_buffers_needed = 2;
	vb2_q->lock = &mchp_dscmi->lock;

	ret = vb2_queue_init(vb2_q);
	if (ret) {
		dev_err(mchp_dscmi->dev, "vb2 queue init failed %d\n", ret);
		goto free_ctrl_hdlr;
	}

	INIT_LIST_HEAD(&mchp_dscmi->buf_list);
	spin_lock_init(&mchp_dscmi->qlock);

	vdev = &mchp_dscmi->vdev;
	strscpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->fops = &mchp_dscmi_fops,
	vdev->ioctl_ops = &mchp_dscmi_ioctl_ops,
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
			    V4L2_CAP_STREAMING;
	vdev->lock = &mchp_dscmi->lock;
	vdev->queue = vb2_q;
	vdev->v4l2_dev = &mchp_dscmi->v4l2_dev;
	video_set_drvdata(vdev, mchp_dscmi);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(mchp_dscmi->dev, "video register device failed %d\n",
			ret);
		goto vb2_queue_free;
	}

	dev_dbg(mchp_dscmi->dev, "Async_complete\n");

	return 0;

vb2_queue_free:
	vb2_queue_release(vb2_q);
free_ctrl_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static const struct v4l2_async_notifier_operations mchp_dscmi_v4l2_async_ops = {
	.bound = mchp_dscmi_graph_notify_bound,
	.unbind = mchp_dscmi_graph_notify_unbind,
	.complete = mchp_dscmi_graph_notify_complete,
};

static void mchp_dscmi_subdev_cleanup(struct mchp_dscmi_fpga *mchp_dscmi)
{
	struct mchp_dscmi_subdev_entity *subdev_entity;

	list_for_each_entry(subdev_entity, &mchp_dscmi->subdev_entities, list) {
		v4l2_async_notifier_unregister(&subdev_entity->notifier);
		v4l2_async_notifier_cleanup(&subdev_entity->notifier);
	}

	INIT_LIST_HEAD(&mchp_dscmi->subdev_entities);
}

static void mchp_dscmi_gain_cal(struct mchp_dscmi_fpga *mchp_dscmi,
				uint32_t total_average)
{
	struct v4l2_subdev *subdev = mchp_dscmi->current_subdev->subdev;
	struct v4l2_control ctrl;
	const u16 hs_threshold_high = (MCHP_DSCMI_GAIN_AVERAGE + MCHP_DSCMI_HYSTERESIS_GAIN);
	const u16 hs_threshold_low = (MCHP_DSCMI_GAIN_AVERAGE - MCHP_DSCMI_HYSTERESIS_GAIN);
	static u16 in_gain = MCHP_DSCMI_GAIN_INIT;
	static u16 last_step;
	u16 step;

	/*
	 * The total_average feed back value from fabric is less than threshold
	 * value then the gain will be step up by one, if the value is more than
	 * the threshold value then the gain will be step down by one.
	 */

	if (total_average < hs_threshold_low)
		step = 1;
	else
		if (total_average > hs_threshold_high)
			step = -1;
		else
			step = 0;

	in_gain = in_gain + step;

	if (in_gain < MCHP_DSCMI_GAIN_MIN)
		in_gain = MCHP_DSCMI_GAIN_MIN;

	if (in_gain >= MCHP_DSCMI_GAIN_AVERAGE)
		in_gain = MCHP_DSCMI_GAIN_AVERAGE;

	if (last_step != step && step != 0) {
		dev_dbg(mchp_dscmi->dev, "average=%d in_gain=%d step=%d\n",
			total_average, in_gain, step);
	}

	last_step = step;
	if (step != 0) {
		memset(&ctrl, 0, sizeof(ctrl));
		ctrl.id = V4L2_CID_ANALOGUE_GAIN;
		ctrl.value = in_gain;
		v4l2_s_ctrl(NULL, subdev->ctrl_handler, &ctrl);
	}
}

static void mchp_dscmi_work_auto_analog_gain(struct work_struct *work)
{
	struct mchp_dscmi_fpga *mchp_dscmi = container_of(work,
							struct mchp_dscmi_fpga,
							auto_gain_dw.work);
	int div = MCHP_DSCMI_MAX_WIDTH * MCHP_DSCMI_MAX_HEIGHT * 2;
	u32 total_sum, total_average;

	total_sum = mchp_dscmi_reg_read(mchp_dscmi, MCHP_DSCMI_RGB_SUM);
	total_average = total_sum / div;
	mchp_dscmi_gain_cal(mchp_dscmi, total_average);
	queue_delayed_work(mchp_dscmi->auto_gain_wq,
			   &mchp_dscmi->auto_gain_dw,
			   msecs_to_jiffies(MCHP_DSCMI_DELAYED_WORK_TIME_M_SEC));
}

static int mchp_dscmi_graph_parse_dt(struct device *dev,
				     struct mchp_dscmi_fpga *mchp_dscmi)
{
	struct device_node *np = dev->of_node;
	struct device_node *epn;
	struct mchp_dscmi_subdev_entity *subdev_entity;
	struct v4l2_fwnode_endpoint v4l2_epn = { .bus_type = 0 };
	int ret;

	INIT_LIST_HEAD(&mchp_dscmi->subdev_entities);

	epn = of_graph_get_next_endpoint(np, NULL);
	if (!epn)
		return 0;

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(epn),
					 &v4l2_epn);
	if (ret) {
		dev_err(dev, "Could not parse the endpoint\n");
		of_node_put(epn);
		return -EINVAL;
	}

	subdev_entity = devm_kzalloc(dev, sizeof(*subdev_entity),
				     GFP_KERNEL);
	if (!subdev_entity) {
		of_node_put(epn);
		return -ENOMEM;
	}

	subdev_entity->epn = epn;

	list_add_tail(&subdev_entity->list,
		      &mchp_dscmi->subdev_entities);

	of_node_put(epn);

	return 0;
}

static int mchp_dscmi_read_capabilities(struct platform_device *pdev,
					struct mchp_dscmi_fpga *mchp_dscmi)
{
	u32 capabilities;
	int ret = 0;

	capabilities = mchp_dscmi_reg_read(mchp_dscmi, MCHP_DSCMI_CAPABILITIES_V4L2);

	switch (capabilities) {
	case MCHP_DSCMI_CAPABILITIES_H264:
		dev_info(&pdev->dev, "Found H.264 video capabilities\n");
		mchp_dscmi->capabilities = H264;
		break;
	case MCHP_DSCMI_CAPABILITIES_MJPEG:
		dev_info(&pdev->dev, "Found mJPEG video capabilities\n");
		mchp_dscmi->capabilities = MJPEG;
		break;
	default:
		dev_err(&pdev->dev, "capabilities not available 0x%x 0x%x\n",
			capabilities,
			mchp_dscmi_reg_read(mchp_dscmi, MCHP_DSCMI_CAPABILITIES_V4L2 + 4));
		ret = -ENODEV;
		break;
	}

	return ret;
}

static int mchp_dscmi_probe(struct platform_device *pdev)
{
	struct mchp_dscmi_fpga *mchp_dscmi;
	struct mchp_dscmi_subdev_entity *subdev_entity;
	struct device_node *np;
	struct dma_chan *chan;
	struct resource *res, r;
	int ret;

	mchp_dscmi = devm_kzalloc(&pdev->dev,
				  sizeof(struct mchp_dscmi_fpga), GFP_KERNEL);
	if (!mchp_dscmi)
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_dscmi),
				     "kzalloc failed\n");

	mchp_dscmi->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(mchp_dscmi->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_dscmi->base),
				     "could not get mem resource\n");

	ret = mchp_dscmi_read_capabilities(pdev, mchp_dscmi);
	if (ret < 0)
		return ret;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np)
		return dev_err_probe(&pdev->dev, PTR_ERR(np),
				     "No memory-region specified\n");

	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				"No memory address assigned to the region\n");

	mchp_dscmi->cambuf.paddr = r.start;
	mchp_dscmi->cambuf.size = r.end - r.start;

	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_STREAM_ADDR_LOW,
			     lower_32_bits(r.start));

	mchp_dscmi_reg_write(mchp_dscmi, MCHP_DSCMI_STREAM_ADDR_HIGH,
			     upper_32_bits(r.start));

	mchp_dscmi->irq = platform_get_irq(pdev, 0);
	if (mchp_dscmi->irq <= 0)
		return dev_err_probe(&pdev->dev, mchp_dscmi->irq,
				     "could not get irq\n");

	mchp_dscmi->reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset",
							 GPIOD_OUT_LOW);
	if (IS_ERR(mchp_dscmi->reset_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(mchp_dscmi->reset_gpio),
				     "Failed to get reset gpio");

	if (mchp_dscmi->reset_gpio)
		gpiod_set_value_cansleep(mchp_dscmi->reset_gpio, 1);

	mchp_dscmi->s_buff_index = 0;
	mchp_dscmi->s_buff_size = 0;

	chan = dma_request_chan(&pdev->dev, "rx");
	if (IS_ERR(chan))
		return dev_err_probe(&pdev->dev, PTR_ERR(chan),
				     "Failed to request DMA channel\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "DMA set mask failed");
		goto dma_free;
	}

	mutex_init(&mchp_dscmi->dma_lock);
	mutex_init(&mchp_dscmi->lock);
	mchp_dscmi->dma_chan = chan;

	mchp_dscmi->pdev = pdev;
	mchp_dscmi->dev = &pdev->dev;

	ret = v4l2_device_register(&pdev->dev, &mchp_dscmi->v4l2_dev);
	if (ret)
		goto dma_free;

	platform_set_drvdata(pdev, mchp_dscmi);

	mchp_dscmi->auto_gain_wq = create_workqueue("auto gain");
	INIT_DELAYED_WORK(&mchp_dscmi->auto_gain_dw,
			  mchp_dscmi_work_auto_analog_gain);

	ret = mchp_dscmi_graph_parse_dt(&pdev->dev, mchp_dscmi);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "Fail to parse device tree\n");
		goto v4l2_unregister;
	}

	if (list_empty(&mchp_dscmi->subdev_entities)) {
		ret = -ENODEV;
		dev_err_probe(&pdev->dev, ret, "No subdev found\n");
		goto v4l2_unregister;
	}

	list_for_each_entry(subdev_entity, &mchp_dscmi->subdev_entities, list) {
		struct v4l2_async_subdev *async_subdev;

		v4l2_async_notifier_init(&subdev_entity->notifier);

		async_subdev = v4l2_async_notifier_add_fwnode_remote_subdev(&subdev_entity
									    ->notifier,
							of_fwnode_handle(subdev_entity->epn),
							struct v4l2_async_subdev);
		of_node_put(subdev_entity->epn);
		subdev_entity->epn = NULL;

		if (IS_ERR(async_subdev)) {
			ret = PTR_ERR(async_subdev);
			goto cleanup_subdev;
		}

		subdev_entity->notifier.ops = &mchp_dscmi_v4l2_async_ops;

		ret = v4l2_async_notifier_register(&mchp_dscmi->v4l2_dev,
						   &subdev_entity->notifier);
		if (ret) {
			dev_err_probe(&pdev->dev, ret,
				      "Fail to register async notifier\n");
			goto cleanup_subdev;
		}
	}

	dev_info(&pdev->dev, "Version %s loaded\n", MCHP_DSCMI_DRV_VERSION);
	return 0;

cleanup_subdev:
	mchp_dscmi_subdev_cleanup(mchp_dscmi);
v4l2_unregister:
	v4l2_device_unregister(&mchp_dscmi->v4l2_dev);
dma_free:
	dma_release_channel(mchp_dscmi->dma_chan);
	return ret;
}

static int mchp_dscmi_remove(struct platform_device *pdev)
{
	struct v4l2_device *v4l2_dev = platform_get_drvdata(pdev);
	struct mchp_dscmi_fpga *mchp_dscmi = container_of(v4l2_dev,
							struct mchp_dscmi_fpga,
							v4l2_dev);

	mutex_destroy(&mchp_dscmi->dma_lock);
	mutex_destroy(&mchp_dscmi->lock);
	cancel_delayed_work(&mchp_dscmi->auto_gain_dw);
	flush_workqueue(mchp_dscmi->auto_gain_wq);
	destroy_workqueue(mchp_dscmi->auto_gain_wq);
	v4l2_async_notifier_unregister(&mchp_dscmi->current_subdev->notifier);
	v4l2_async_notifier_cleanup(&mchp_dscmi->current_subdev->notifier);
	v4l2_device_unregister(&mchp_dscmi->v4l2_dev);
	dma_release_channel(mchp_dscmi->dma_chan);

	return 0;
}

static const struct of_device_id mchp_dscmi_of_match[] = {
	{ .compatible = "microchip,fpga-dscmi" },
	{}
};

MODULE_DEVICE_TABLE(of, mchp_dscmi_of_match);

static struct platform_driver mchp_dscmi_driver = {
	.probe = mchp_dscmi_probe,
	.remove = mchp_dscmi_remove,
	.driver = {
		.name = MCHP_DSCMI_DRV_NAME,
		.of_match_table = mchp_dscmi_of_match,
	},
};

module_platform_driver(mchp_dscmi_driver);

MODULE_DESCRIPTION("Microchip Digital Serial Camera Memory Interface Driver");
MODULE_AUTHOR("Shravan Chippa <shravan.chippa@microchip.com>");
MODULE_LICENSE("GPL");
