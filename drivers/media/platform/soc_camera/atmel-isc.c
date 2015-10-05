/*
 * Copyright (c) 2015 Atmel Corporation
 * Josh Wu, <josh.wu@atmel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/atmel-isc.h>
#include <media/soc_camera.h>
#include <media/soc_mediabus.h>
#include <media/v4l2-of.h>
#include <media/videobuf2-dma-contig.h>

#define MAX_BUFFER_NUM			32
#define MAX_SUPPORT_WIDTH		2592
#define MAX_SUPPORT_HEIGHT		2592
#define VID_LIMIT_BYTES			(16 * 1024 * 1024)
#define MIN_FRAME_RATE			15
#define FRAME_INTERVAL_MILLI_SEC	(1000 / MIN_FRAME_RATE)
#define ISC_DEFAULT_MCLK_FREQ		25000000

/* Frame buffer DMA descriptor view0 */
struct fbd_v0 {
	/* DMA Control Register */
	u32 dma_ctrl;
	/* Physical address of the next fbd */
	u32 next_fbd_address;
	/* Physical address of the frame buffer 0 */
	u32 fb_address;
	/* stride 0 */
	u32 fb_stride;
};

static void set_dma_ctrl(struct fbd_v0 *fb_desc, u32 ctrl)
{
	fb_desc->dma_ctrl = ctrl;
}

struct isc_dma_desc {
	struct list_head list;
	struct fbd_v0 *p_fbd;
	dma_addr_t fbd_phys;
};

/* Frame buffer data */
struct frame_buffer {
	struct vb2_buffer vb;
	struct isc_dma_desc *p_dma_desc;
	struct list_head list;
};

struct atmel_isc {
	/* Protects the access of variables shared with the ISR */
	spinlock_t			lock;
	void __iomem			*regs;

	int				sequence;

	struct vb2_alloc_ctx		*alloc_ctx;

	/* Allocate descriptors for dma buffer use */
	struct fbd_v0			*p_fb_descriptors;
	dma_addr_t			fb_descriptors_phys;
	struct list_head		dma_desc_head;
	struct isc_dma_desc		dma_desc[MAX_BUFFER_NUM];

	struct completion		complete;
	/* peripherial clock */
	struct clk			*pclk;
	struct clk			*iscck;
	unsigned int			irq;
	struct isc_platform_data	pdata;
	u16				width_flags;	/* max 12 bits */

	u32				bus_param;	/* Polarity */

	/* frame buffer list, items should be handled when dma transfer is done */
	struct list_head		frame_buffer_list;
	/* the frame buffer which is transfering data by dma */
	struct frame_buffer		*active; /* only used by interrupt handler */

	struct soc_camera_host		soc_host;
	
	// AP: Internal variable to tell if streaming is on or off. access only under lock for isr synch.
	int is_streaming_on;
};

static inline struct atmel_isc *to_isc(const struct soc_camera_host *soc_host)
{
	return container_of(soc_host, struct atmel_isc, soc_host);
}

static void isc_writel(struct atmel_isc *isc, u32 reg, u32 val)
{
	writel(val, isc->regs + reg);
}
static u32 isc_readl(struct atmel_isc *isc, u32 reg)
{
	return readl(isc->regs + reg);
}

static const u32 gammar_table[64] = {
	0x00000E6,0x0E80040,0x129002D,0x1570025,0x17C001F,0x19C001B,0x1B70019,0x1D00016,
	0x1E70014,0x1FC0013,0x20F0012,0x2210011,0x2330010,0x243000F,0x253000E,0x261000E,
	0x270000D,0x27D000D,0x28A000D,0x297000C,0x2A3000C,0x2AF000C,0x2BB000B,0x2C6000B,
	0x2D1000A,0x2DB000A,0x2E6000A,0x2F00009,0x2FA0009,0x3030009,0x30D0009,0x3160009,
	0x31F0008,0x3280008,0x3300009,0x3390008,0x3410008,0x3490008,0x3510008,0x3590008,
	0x3610008,0x3690007,0x3700008,0x3780007,0x37F0007,0x3860007,0x38D0007,0x3940007,
	0x39B0007,0x3A20007,0x3A90006,0x3AF0007,0x3B60006,0x3BC0007,0x3C30006,0x3C90006,
	0x3CF0007,0x3D60006,0x3DC0006,0x3E20006,0x3E80006,0x3EE0005,0x3F40005,0x3F90006,
};

static int setup_gammar_table(struct atmel_isc *isc, u32 isc_entry_base,
			      const u32 *table, int table_len)
{
	u32 *p, i;
	if (table_len > 64)
		return -EINVAL;

	for (i = 0, p = (u32*)table; i < table_len; i++, p++)
		isc_writel(isc, isc_entry_base + i * sizeof(u32), *p);

	return 0;
}

static int initialize_isc(struct atmel_isc *isc)
{
	u32 pfe_cfg0 = 0;

	if (isc->bus_param & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		pfe_cfg0 |= ISC_PFE_HSYNC_ACTIVE_LOW;
	if (isc->bus_param & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		pfe_cfg0 |= ISC_PFE_VSYNC_ACTIVE_LOW;
	if (isc->bus_param & V4L2_MBUS_PCLK_SAMPLE_FALLING)
		pfe_cfg0 |= ISC_PFE_PIX_CLK_FALLING_EDGE;

	pfe_cfg0 |= ISC_PFE_MODE_PROGRESSIVE | ISC_PFE_CONT_VIDEO;

	/* TODO: need to revisit. */
	pfe_cfg0 |= ISC_PFE_BPS_8_BIT;

	isc_writel(isc, ISC_PFE_CFG0, pfe_cfg0);

	/* setup gammar table  */
	setup_gammar_table(isc, ISC_GAM_BENTRY0, gammar_table, ARRAY_SIZE(gammar_table));
	setup_gammar_table(isc, ISC_GAM_GENTRY0, gammar_table, ARRAY_SIZE(gammar_table));
	setup_gammar_table(isc, ISC_GAM_RENTRY0, gammar_table, ARRAY_SIZE(gammar_table));

	return 0;
}

static void configure_geometry(struct atmel_isc *isc,
				const struct soc_camera_format_xlate *xlate)
{
	/* According to sensor's output format to set cfg2 */
	switch (xlate->code) {
	/* YUV, including grey */
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	default:
		isc_writel(isc, ISC_CFA_CTRL, 0);
		isc_writel(isc, ISC_GAM_CTRL, 0);
		isc_writel(isc, ISC_RLP_CFG, ISC_RLP_CFG_MODE_DAT8);
		isc_writel(isc, ISC_DCFG, ISC_DCFG_IMODE_PACKED8);
		break;
	/* Bayer RGB */
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		if (xlate->host_fmt->fourcc == V4L2_PIX_FMT_RGB565) {
			isc_writel(isc, ISC_CFA_CTRL, 1);
			isc_writel(isc, ISC_CFA_CFG, 3 | 1 << 4);
			isc_writel(isc, ISC_GAM_CTRL, ISC_GAM_CTRL_ENABLE | ISC_GAM_CTRL_ENABLE_ALL_CHAN);
			isc_writel(isc, ISC_RLP_CFG, ISC_RLP_CFG_MODE_RGB565);
			isc_writel(isc, ISC_DCFG, ISC_DCFG_IMODE_PACKED16);
		} else {
			/* output to Bayer RGB */
			isc_writel(isc, ISC_CFA_CTRL, 0);
			isc_writel(isc, ISC_GAM_CTRL, 0);
			isc_writel(isc, ISC_RLP_CFG, ISC_RLP_CFG_MODE_DAT8);
			isc_writel(isc, ISC_DCFG, ISC_DCFG_IMODE_PACKED8);
		}
		break;
	}
}

static void start_dma(struct atmel_isc *isc, struct frame_buffer *buffer);
static irqreturn_t atmel_isc_handle_streaming(struct atmel_isc *isc)
{
	if (isc->active) {
		struct vb2_buffer *vb = &isc->active->vb;
		struct frame_buffer *buf = isc->active;

		list_del_init(&buf->list);
		v4l2_get_timestamp(&vb->v4l2_buf.timestamp);
		vb->v4l2_buf.sequence = isc->sequence++;
		vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
	}

	if (list_empty(&isc->frame_buffer_list)) {
		isc->active = NULL;
	} else {
		/* start next dma frame. */
		isc->active = list_entry(isc->frame_buffer_list.next,
					struct frame_buffer, list);
		start_dma(isc, isc->active);
	}
	return IRQ_HANDLED;
}

/* interrupt service routine */
static irqreturn_t isc_interrupt(int irq, void *dev_id)
{
	struct atmel_isc *isc = dev_id;
	u32 status, mask, pending;
	irqreturn_t ret = IRQ_NONE;

	spin_lock(&isc->lock);

	status = isc_readl(isc, ISC_INTSR);
	mask = isc_readl(isc, ISC_INTMASK);
	pending = status & mask;

	if (pending & ISC_INT_SWRST_COMPLETE) {
		complete(&isc->complete);
		isc_writel(isc, ISC_INTEN, ISC_INT_SWRST_COMPLETE);
		ret = IRQ_HANDLED;
	} else if (pending & ISC_INT_DISABLE_COMPLETE) {
		complete(&isc->complete);
		isc_writel(isc, ISC_INTEN, ISC_INT_DISABLE_COMPLETE);
		ret = IRQ_HANDLED;
	} else if (likely(pending & ISC_INT_DMA_DONE)) {
		ret = atmel_isc_handle_streaming(isc);
	}

	spin_unlock(&isc->lock);
	return ret;
}

#define	WAIT_ISC_RESET		1
#define	WAIT_ISC_DISABLE	0
static int atmel_isc_wait_status(struct atmel_isc *isc, int wait_reset)
{
	unsigned long timeout;
	/*
	 * The reset or disable will only succeed if we have a
	 * pixel clock from the camera.
	 */
	init_completion(&isc->complete);

	if (wait_reset) {
		isc_writel(isc, ISC_INTEN, ISC_INT_SWRST_COMPLETE);
		isc_writel(isc, ISC_CTRLDIS, ISC_CTRLDIS_SWRST);
	} else {
		isc_writel(isc, ISC_INTEN, ISC_INT_DISABLE_COMPLETE);
		isc_writel(isc, ISC_CTRLDIS, ISC_CTRLDIS_CAPTURE);
	}

	timeout = wait_for_completion_timeout(&isc->complete,
			msecs_to_jiffies(500));
	if (timeout == 0)
		return -ETIMEDOUT;

	return 0;
}

/* ------------------------------------------------------------------
	Videobuf operations
   ------------------------------------------------------------------*/
static int queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], void *alloc_ctxs[])
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vq);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	unsigned long size;

	size = icd->sizeimage;

	if (!*nbuffers || *nbuffers > MAX_BUFFER_NUM)
		*nbuffers = MAX_BUFFER_NUM;

	if (size * *nbuffers > VID_LIMIT_BYTES)
		*nbuffers = VID_LIMIT_BYTES / size;

	*nplanes = 1;
	sizes[0] = size;
	alloc_ctxs[0] = isc->alloc_ctx;

	isc->sequence = 0;
	isc->active = NULL;

	dev_dbg(icd->parent, "%s, count=%d, size=%ld\n", __func__,
		*nbuffers, size);

	return 0;
}

static int buffer_init(struct vb2_buffer *vb)
{
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);

	buf->p_dma_desc = NULL;
	INIT_LIST_HEAD(&buf->list);

	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vb->vb2_queue);
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	unsigned long size;
	struct isc_dma_desc *desc;

	size = icd->sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(icd->parent, "%s data will not fit into plane (%lu < %lu)\n",
				__func__, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb, 0, size);

	if (!buf->p_dma_desc) {
		if (list_empty(&isc->dma_desc_head)) {
			dev_err(icd->parent, "Not enough dma descriptors.\n");
			return -EINVAL;
		} else {
			/* Get an available descriptor */
			desc = list_entry(isc->dma_desc_head.next,
						struct isc_dma_desc, list);
			/* Delete the descriptor since now it is used */
			list_del_init(&desc->list);

			/* Initialize the dma descriptor */
			desc->p_fbd->fb_address =
					vb2_dma_contig_plane_dma_addr(vb, 0);
			desc->p_fbd->next_fbd_address = 0;
			desc->p_fbd->fb_stride = 0;
			set_dma_ctrl(desc->p_fbd, ISC_DCTRL_DESC_ENABLE | ISC_DCTRL_DVIEW_PACKED);
			buf->p_dma_desc = desc;
		}
	}
	return 0;
}

static void buffer_cleanup(struct vb2_buffer *vb)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vb->vb2_queue);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);

	/* This descriptor is available now and we add to head list */
	if (buf->p_dma_desc)
		list_add(&buf->p_dma_desc->list, &isc->dma_desc_head);
}

static void start_dma(struct atmel_isc *isc, struct frame_buffer *buffer)
{
	isc_writel(isc, ISC_DNDA,
		(u32)buffer->p_dma_desc->fbd_phys);
	isc_writel(isc, ISC_DCTRL,
			ISC_DCTRL_DESC_ENABLE | ISC_DCTRL_DVIEW_PACKED |
			ISC_DCTRL_DMA_DONE_INT_ENABLE | ISC_DCTRL_WRITE_BACK_ENABLE);
	isc_writel(isc, ISC_DAD0, buffer->p_dma_desc->p_fbd->fb_address);

	isc_writel(isc, ISC_CTRLEN, ISC_CTRLEN_CAPTURE);
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vb->vb2_queue);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);
	unsigned long flags = 0;

	spin_lock_irqsave(&isc->lock, flags);
	list_add(&buf->list, &isc->frame_buffer_list);

	// AP: in case is not active but streaming is on, trigger a new sma transfer
	if (isc->active == NULL)
	{
		isc->active = buf;
		if (isc->is_streaming_on)
		{
			start_dma(isc, isc->active);
		}
	}
	spin_unlock_irqrestore(&isc->lock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vq);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	int ret;

	// AP: Why there should be buffers already enqueued? Try to relax this limitation
	/*if (!isc->active){
		dev_info(icd->parent, "%s (ret %d)\n", __func__, -EINVAL);
		return -EINVAL;
	}*/

	pm_runtime_get_sync(ici->v4l2_dev.dev);

	ret = atmel_isc_wait_status(isc, WAIT_ISC_RESET);
	if (ret < 0) {
		dev_err(icd->parent, "Reset timed out\n");
		pm_runtime_put(ici->v4l2_dev.dev);
		return ret;
	}

	/* Disable all interrupts */
	isc_writel(isc, ISC_INTDIS, (u32)~0UL);

	initialize_isc(isc);

	configure_geometry(isc, icd->current_fmt);

	/* update profile */
	isc_writel(isc, ISC_CTRLEN, ISC_CTRLEN_UPPRO);

	/* AP: this calls requires a video stream already running while for v4l2 it is not yet started!
	while((isc_readl(isc, ISC_CTRLSR) & ISC_CTRLSR_UPPRO) == ISC_CTRLSR_UPPRO)
		cpu_relax();
	*/

	spin_lock_irq(&isc->lock);

	/* Enable dma irq */
	// AP: Why there should be buffers already enqueued? Try to relax this limitation
	//if (count) {
				
		// AP: set straming variable
		isc->is_streaming_on = 1;
		
		/* Enable dma irq */
		isc_writel(isc, ISC_INTEN, ISC_INT_DMA_DONE);

		// AP: if a buffer is ready, fire DMA
		if (isc->active)
			start_dma(isc, isc->active);
	//}

	spin_unlock_irq(&isc->lock);

	return 0;
}

/* abort streaming and wait for last buffer */
static void stop_streaming(struct vb2_queue *vq)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vq);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	struct frame_buffer *buf, *node;
	int ret = 0;
	unsigned long timeout;

	spin_lock_irq(&isc->lock);
	/* Release all active buffers */
	list_for_each_entry_safe(buf, node, &isc->frame_buffer_list, list) {
		list_del_init(&buf->list);
		if (buf != isc->active)
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irq(&isc->lock);

	// AP: no need to wait in single frames
	/* Wait until the end of the current frame. as dma will terminated it capture */
	timeout = jiffies + FRAME_INTERVAL_MILLI_SEC * HZ;
	while ((isc_readl(isc, ISC_CTRLSR) & ISC_CTRLSR_CAPTURE) &&
			time_before(jiffies, timeout))
		msleep(1);

	if (time_after(jiffies, timeout))
		dev_err(icd->parent,
			"Timeout waiting for finishing codec request\n");

	/* Disable interrupts */
	isc_writel(isc, ISC_INTDIS,
			ISC_INT_DMA_DONE);

	spin_lock_irq(&isc->lock);
	isc->active = NULL;
	isc->is_streaming_on = 0;
	spin_unlock_irq(&isc->lock);

	// AP: requires a frame to complete, otherwise timeout!
	// AP: anyway no need to wait
	/* Disable ISC and wait for it is done */ 
	ret = atmel_isc_wait_status(isc, WAIT_ISC_DISABLE);
	if (ret < 0)
		dev_err(icd->parent, "Disable timed out\n");

	pm_runtime_put(ici->v4l2_dev.dev);
}

static int clock_start(struct soc_camera_host *ici)
{
	/* as the clock (ISC_MCK) is provided by peripheral clock, so just resume pm */
	pm_runtime_get_sync(ici->v4l2_dev.dev);

	return 0;
}

static void clock_stop(struct soc_camera_host *ici)
{
	/* as the clock (ISC_MCK) is provided by peripheral clock, so just suspend pm */
	pm_runtime_put(ici->v4l2_dev.dev);
}

static struct vb2_ops isc_video_qops = {
	.queue_setup		= queue_setup,
	.buf_init		= buffer_init,
	.buf_prepare		= buffer_prepare,
	.buf_cleanup		= buffer_cleanup,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming		= stop_streaming,
	.wait_prepare		= soc_camera_unlock,
	.wait_finish		= soc_camera_lock,
};

/* ------------------------------------------------------------------
	SOC camera operations for the device
   ------------------------------------------------------------------*/
static int isc_camera_init_videobuf(struct vb2_queue *q,
				     struct soc_camera_device *icd)
{
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP;
	q->drv_priv = icd;
	q->buf_struct_size = sizeof(struct frame_buffer);
	q->ops = &isc_video_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	return vb2_queue_init(q);
}

static bool is_supported(struct soc_camera_device *icd,
		const struct soc_camera_format_xlate *xlate)
{
	bool ret = true;

	switch (xlate->code) {
	/* YUV, including grey */
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	/* Bayer RGB */
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		break;
	/* RGB, TODO */
	default:
		dev_err(icd->parent, "not supported format: %d\n",
					xlate->code);
		ret = false;
	}

	return ret;
}

static int isc_camera_set_fmt(struct soc_camera_device *icd,
			      struct v4l2_format *f)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	const struct soc_camera_format_xlate *xlate;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct v4l2_mbus_framefmt mf;
	int ret;

	xlate = soc_camera_xlate_by_fourcc(icd, pix->pixelformat);
	if (!xlate) {
		dev_warn(icd->parent, "Format %x not found\n",
			 pix->pixelformat);
		return -EINVAL;
	}

	dev_dbg(icd->parent, "Plan to set format %dx%d\n",
			pix->width, pix->height);

	mf.width	= pix->width;
	mf.height	= pix->height;
	mf.field	= pix->field;
	mf.colorspace	= pix->colorspace;
	mf.code		= xlate->code;

	ret = v4l2_subdev_call(sd, video, s_mbus_fmt, &mf);
	if (ret < 0)
		return ret;

	if (mf.code != xlate->code)
		return -EINVAL;

	/* check with atmel-isi support format */
	if (!is_supported(icd, xlate))
		return -EINVAL;

	pix->width		= mf.width;
	pix->height		= mf.height;
	pix->field		= mf.field;
	pix->colorspace		= mf.colorspace;
	icd->current_fmt	= xlate;

	dev_dbg(icd->parent, "Finally set format %dx%d\n",
		pix->width, pix->height);

	return ret;
}

static int isc_camera_try_fmt(struct soc_camera_device *icd,
			      struct v4l2_format *f)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	const struct soc_camera_format_xlate *xlate;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct v4l2_mbus_framefmt mf;
	u32 pixfmt = pix->pixelformat;
	int ret;

	xlate = soc_camera_xlate_by_fourcc(icd, pixfmt);
	if (pixfmt && !xlate) {
		dev_warn(icd->parent, "Format %x not found\n", pixfmt);
		return -EINVAL;
	}

	/* limit to Atmel ISC hardware capabilities */
	if (pix->height > MAX_SUPPORT_HEIGHT)
		pix->height = MAX_SUPPORT_HEIGHT;
	if (pix->width > MAX_SUPPORT_WIDTH)
		pix->width = MAX_SUPPORT_WIDTH;

	/* limit to sensor capabilities */
	mf.width	= pix->width;
	mf.height	= pix->height;
	mf.field	= pix->field;
	mf.colorspace	= pix->colorspace;
	mf.code		= xlate->code;

	ret = v4l2_subdev_call(sd, video, try_mbus_fmt, &mf);
	if (ret < 0)
		return ret;

	pix->width	= mf.width;
	pix->height	= mf.height;
	pix->colorspace	= mf.colorspace;

	switch (mf.field) {
	case V4L2_FIELD_ANY:
		pix->field = V4L2_FIELD_NONE;
		break;
	case V4L2_FIELD_NONE:
		break;
	default:
		dev_err(icd->parent, "Field type %d unsupported.\n",
			mf.field);
		ret = -EINVAL;
	}

	return ret;
}

/* This will be corrected as we get more formats */
static bool isc_camera_packing_supported(const struct soc_mbus_pixelfmt *fmt)
{
	return	fmt->packing == SOC_MBUS_PACKING_NONE ||
		(fmt->bits_per_sample == 8 &&
		 fmt->packing == SOC_MBUS_PACKING_2X8_PADHI) ||
		(fmt->bits_per_sample > 8 &&
		 fmt->packing == SOC_MBUS_PACKING_EXTEND16);
}

#define ISC_BUS_PARAM (V4L2_MBUS_MASTER |	\
		V4L2_MBUS_HSYNC_ACTIVE_HIGH |	\
		V4L2_MBUS_HSYNC_ACTIVE_LOW |	\
		V4L2_MBUS_VSYNC_ACTIVE_HIGH |	\
		V4L2_MBUS_VSYNC_ACTIVE_LOW |	\
		V4L2_MBUS_PCLK_SAMPLE_RISING |	\
		V4L2_MBUS_PCLK_SAMPLE_FALLING |	\
		V4L2_MBUS_DATA_ACTIVE_HIGH)

static int isc_camera_try_bus_param(struct soc_camera_device *icd,
				    unsigned char buswidth)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	struct v4l2_mbus_config cfg = {.type = V4L2_MBUS_PARALLEL,};
	unsigned long common_flags;
	int ret;

	ret = v4l2_subdev_call(sd, video, g_mbus_config, &cfg);
	if (!ret) {
		common_flags = soc_mbus_config_compatible(&cfg,
							  ISC_BUS_PARAM);
		if (!common_flags) {
			dev_warn(icd->parent,
				 "Flags incompatible: camera 0x%x, host 0x%x\n",
				 cfg.flags, ISC_BUS_PARAM);
			return -EINVAL;
		}
	} else if (ret != -ENOIOCTLCMD) {
		return ret;
	}

	if ((1 << (buswidth - 1)) & isc->width_flags)
		return 0;
	return -EINVAL;
}

static const struct soc_mbus_pixelfmt isc_rgb565_formats[] = {
	{
		.fourcc			= V4L2_PIX_FMT_RGB565,
		.name			= "RGB565",
		.bits_per_sample	= 8,
		.packing		= SOC_MBUS_PACKING_2X8_PADHI,
		.order			= SOC_MBUS_ORDER_LE,
		.layout			= SOC_MBUS_LAYOUT_PACKED,
	},
};

static int isc_camera_get_formats(struct soc_camera_device *icd,
				  unsigned int idx,
				  struct soc_camera_format_xlate *xlate)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	int formats = 0, ret;
	int i, n;
	/* sensor format */
	u32 code;
	/* soc camera host format */
	const struct soc_mbus_pixelfmt *fmt;

	ret = v4l2_subdev_call(sd, video, enum_mbus_fmt, idx, &code);
	if (ret < 0)
	/* No more formats */
	{
		dev_err(icd->parent,
			"No more formats #%u: %d\n", idx, code);
		return 0;
	}

	fmt = soc_mbus_get_fmtdesc(code);
	if (!fmt) {
		dev_err(icd->parent,
			"Invalid format code #%u: %d\n", idx, code);
		return 0;
	}

	/* This also checks support for the requested bits-per-sample */
	ret = isc_camera_try_bus_param(icd, fmt->bits_per_sample);
	if (ret < 0) {
		dev_err(icd->parent,
			"Fail to try the bus parameters.\n");
		return 0;
	}

	switch (code) {
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
		if (!isc_camera_packing_supported(fmt)){
			dev_err(icd->parent,
				"Format not supported #%u: 0x%x\n", idx, code);
			return 0;
		}
		
		if (xlate)
			dev_dbg(icd->parent,
				"Providing format %s in pass-through mode\n",
				fmt->name);
		/* just pass-through */
		break;

	case MEDIA_BUS_FMT_SBGGR8_1X8:
		n = ARRAY_SIZE(isc_rgb565_formats);
		formats += n;
		for (i = 0; i < n; i++) {
			if (xlate) {
				xlate->host_fmt	= &isc_rgb565_formats[i];
				xlate->code	= code;
				//dev_dbg(icd->parent, "Providing format %s (%s) when sensor output RGB565\n",
				//	xlate->host_fmt->name, mbus_fmt_string(xlate->code));
				xlate++;
			}
		}
		/* pass-through */
		break;

	default:
		// AP of not directly supported maybe can be packed in another similar format, Code taken from atmel-isi.
                if (!isc_camera_packing_supported(fmt)){
                        dev_err(icd->parent,
                                "Format not supported #%u: 0x%x\n", idx, code);
                        return 0;
                }
	}

	/* Generic pass-through */
	formats++;
	if (xlate) {
		xlate->host_fmt	= fmt;
		xlate->code	= code;
		xlate++;
	}

	return formats;
}

static int isc_camera_add_device(struct soc_camera_device *icd)
{
	dev_dbg(icd->parent, "Atmel ISC Camera driver attached to camera %d\n",
		 icd->devnum);

	return 0;
}

static void isc_camera_remove_device(struct soc_camera_device *icd)
{
	dev_dbg(icd->parent, "Atmel ISC Camera driver detached from camera %d\n",
		 icd->devnum);
}

static unsigned int isc_camera_poll(struct file *file, poll_table *pt)
{
	struct soc_camera_device *icd = file->private_data;

	return vb2_poll(&icd->vb2_vidq, file, pt);
}

static int isc_camera_querycap(struct soc_camera_host *ici,
			       struct v4l2_capability *cap)
{
	strcpy(cap->driver, "atmel-isc");
	strcpy(cap->card, "Atmel Image Sensor Interface");
	cap->capabilities = (V4L2_CAP_VIDEO_CAPTURE |
				V4L2_CAP_STREAMING);
	return 0;
}

static int isc_camera_set_bus_param(struct soc_camera_device *icd)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isc *isc = ici->priv;
	struct v4l2_mbus_config cfg = {.type = V4L2_MBUS_PARALLEL,};
	unsigned long common_flags;
	int ret;

	ret = v4l2_subdev_call(sd, video, g_mbus_config, &cfg);
	if (!ret) {
		common_flags = soc_mbus_config_compatible(&cfg,
							  ISC_BUS_PARAM);
		if (!common_flags) {
			dev_warn(icd->parent,
				 "Flags incompatible: camera 0x%x, host 0x%x\n",
				 cfg.flags, ISC_BUS_PARAM);
			return -EINVAL;
		}
	} else if (ret != -ENOIOCTLCMD) {
		return ret;
	} else {
		common_flags = ISC_BUS_PARAM;
	}
	dev_dbg(icd->parent, "Flags cam: 0x%x host: 0x%x common: 0x%lx\n",
		cfg.flags, ISC_BUS_PARAM, common_flags);

	/* Make choises, based on platform preferences */
	if ((common_flags & V4L2_MBUS_HSYNC_ACTIVE_HIGH) &&
	    (common_flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)) {
		if (isc->pdata.hsync_act_low)
			common_flags &= ~V4L2_MBUS_HSYNC_ACTIVE_HIGH;
		else
			common_flags &= ~V4L2_MBUS_HSYNC_ACTIVE_LOW;
	}

	if ((common_flags & V4L2_MBUS_VSYNC_ACTIVE_HIGH) &&
	    (common_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)) {
		if (isc->pdata.vsync_act_low)
			common_flags &= ~V4L2_MBUS_VSYNC_ACTIVE_HIGH;
		else
			common_flags &= ~V4L2_MBUS_VSYNC_ACTIVE_LOW;
	}

	if ((common_flags & V4L2_MBUS_PCLK_SAMPLE_RISING) &&
	    (common_flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)) {
		if (isc->pdata.pclk_act_falling)
			common_flags &= ~V4L2_MBUS_PCLK_SAMPLE_RISING;
		else
			common_flags &= ~V4L2_MBUS_PCLK_SAMPLE_FALLING;
	}

	cfg.flags = common_flags;
	ret = v4l2_subdev_call(sd, video, s_mbus_config, &cfg);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		dev_dbg(icd->parent, "camera s_mbus_config(0x%lx) returned %d\n",
			common_flags, ret);
		return ret;
	}

	/* set bus param */
	isc->bus_param = common_flags;

	return 0;
}

static int isc_camera_set_parm(struct soc_camera_device *icd, struct v4l2_streamparm *parm)
{
	return 0;
}

static struct soc_camera_host_ops isc_soc_camera_host_ops = {
	.owner		= THIS_MODULE,
	.add		= isc_camera_add_device,
	.remove		= isc_camera_remove_device,
	.set_fmt	= isc_camera_set_fmt,
	.try_fmt	= isc_camera_try_fmt,
	.get_formats	= isc_camera_get_formats,
	.init_videobuf2	= isc_camera_init_videobuf,
	.poll		= isc_camera_poll,
	.querycap	= isc_camera_querycap,
	.clock_start	= clock_start,
	.clock_stop	= clock_stop,
	.set_bus_param	= isc_camera_set_bus_param,
	.set_parm	= isc_camera_set_parm,
	.get_parm	= isc_camera_set_parm,
};

/* -----------------------------------------------------------------------*/
static int atmel_isc_remove(struct platform_device *pdev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(&pdev->dev);
	struct atmel_isc *isc = to_isc(soc_host);

	soc_camera_host_unregister(soc_host);
	vb2_dma_contig_cleanup_ctx(isc->alloc_ctx);
	dma_free_coherent(&pdev->dev,
			sizeof(struct fbd_v0) * MAX_BUFFER_NUM,
			isc->p_fb_descriptors,
			isc->fb_descriptors_phys);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int atmel_isc_probe_dt(struct atmel_isc *isc,
			struct platform_device *pdev)
{
	struct device_node *np= pdev->dev.of_node;
	struct v4l2_of_endpoint ep;
	int err;

	np = of_graph_get_next_endpoint(np, NULL);
	if (!np) {
		dev_err(&pdev->dev, "Could not find the endpoint\n");
		return -EINVAL;
	}

	err = v4l2_of_parse_endpoint(np, &ep);
	if (err) {
		dev_err(&pdev->dev, "Could not parse the endpoint\n");
		goto err_probe_dt;
	}

	switch (ep.bus.parallel.bus_width) {
	case 8:
		isc->pdata.data_width_flags = ISC_DATAWIDTH_8;
		break;
	case 10:
		isc->pdata.data_width_flags =
				ISC_DATAWIDTH_8 | ISC_DATAWIDTH_10;
		break;
	default:
		dev_err(&pdev->dev, "Unsupported bus width: %d\n",
				ep.bus.parallel.bus_width);
		err = -EINVAL;
		goto err_probe_dt;
	}

	if (ep.bus.parallel.flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		isc->pdata.hsync_act_low = true;
	if (ep.bus.parallel.flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		isc->pdata.vsync_act_low = true;

err_probe_dt:
	of_node_put(np);

	return err;
}

static int atmel_isc_probe(struct platform_device *pdev)
{
	unsigned int irq;
	struct atmel_isc *isc;
	struct resource *regs;
	int ret, i;
	struct device *dev = &pdev->dev;
	struct soc_camera_host *soc_host;
	struct isc_platform_data *pdata;
	u32 cfg;

	pdata = dev->platform_data;
	if ((!pdata || !pdata->data_width_flags) && !pdev->dev.of_node) {
		dev_err(&pdev->dev,
			"No config available for Atmel ISC\n");
		return -EINVAL;
	}

	isc = devm_kzalloc(&pdev->dev, sizeof(struct atmel_isc), GFP_KERNEL);
	if (!isc) {
		dev_err(&pdev->dev, "Can't allocate interface!\n");
		return -ENOMEM;
	}

	isc->pclk = devm_clk_get(&pdev->dev, "isc_clk");
	if (IS_ERR(isc->pclk))
		return PTR_ERR(isc->pclk);

	isc->iscck = devm_clk_get(&pdev->dev, "iscck");
	if (IS_ERR(isc->iscck))
		isc->iscck = NULL;

	if (pdata) {
		memcpy(&isc->pdata, pdata, sizeof(isc->pdata));
	} else {
		ret = atmel_isc_probe_dt(isc, pdev);
		if (ret)
			return ret;
	}

	isc->active = NULL;
	spin_lock_init(&isc->lock);
	INIT_LIST_HEAD(&isc->frame_buffer_list);
	INIT_LIST_HEAD(&isc->dma_desc_head);

	isc->p_fb_descriptors = dma_alloc_coherent(&pdev->dev,
				sizeof(struct fbd_v0) * MAX_BUFFER_NUM,
				&isc->fb_descriptors_phys,
				GFP_KERNEL);
	if (!isc->p_fb_descriptors) {
		dev_err(&pdev->dev, "Can't allocate descriptors!\n");
		return -ENOMEM;
	}

	for (i = 0; i < MAX_BUFFER_NUM; i++) {
		/* mapping the allocated memorys */
		isc->dma_desc[i].p_fbd = isc->p_fb_descriptors + i;
		isc->dma_desc[i].fbd_phys = isc->fb_descriptors_phys +
					i * sizeof(struct fbd_v0);
		/* add to list */
		list_add(&isc->dma_desc[i].list, &isc->dma_desc_head);
	}

	isc->alloc_ctx = vb2_dma_contig_init_ctx(&pdev->dev);
	if (IS_ERR(isc->alloc_ctx)) {
		ret = PTR_ERR(isc->alloc_ctx);
		goto err_alloc_ctx;
	}

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	isc->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(isc->regs)) {
		ret = PTR_ERR(isc->regs);
		goto err_ioremap;
	}

	if (isc->pdata.data_width_flags & ISC_DATAWIDTH_8)
		isc->width_flags = 1 << 7;
	if (isc->pdata.data_width_flags & ISC_DATAWIDTH_10)
		isc->width_flags |= 1 << 9;

	irq = platform_get_irq(pdev, 0);
	if (IS_ERR_VALUE(irq)) {
		ret = irq;
		goto err_req_irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, isc_interrupt, 0, "isc", isc);
	if (ret) {
		dev_err(&pdev->dev, "Unable to request irq %d\n", irq);
		goto err_req_irq;
	}
	isc->irq = irq;

	soc_host		= &isc->soc_host;
	soc_host->drv_name	= "isc-camera";
	soc_host->ops		= &isc_soc_camera_host_ops;
	soc_host->priv		= isc;
	soc_host->v4l2_dev.dev	= &pdev->dev;
	soc_host->nr		= pdev->id;

	pm_suspend_ignore_children(&pdev->dev, true);
	pm_runtime_enable(&pdev->dev);

	if (isc->pdata.asd_sizes) {
		soc_host->asd = isc->pdata.asd;
		soc_host->asd_sizes = isc->pdata.asd_sizes;
	}

	ret = soc_camera_host_register(soc_host);
	if (ret) {
		dev_err(&pdev->dev, "Unable to register soc camera host\n");
		goto err_register_soc_camera_host;
	}

	pm_runtime_get_sync(soc_host->v4l2_dev.dev);

	/*Config the MCK div and select it to isc_clk(hclock) */
	cfg = ISC_CLKCFG_MCDIV(6) & ISC_CLKCFG_MCDIV_MASK;
	cfg |= ISC_CLKCFG_MASTER_SEL_HCLOCK;

	isc_writel(isc, ISC_CLKCFG, cfg);
	while ((isc_readl(isc, ISC_CLKSR) & ISC_CLK_SIP) == ISC_CLK_SIP);
	isc_writel(isc, ISC_CLKEN, ISC_CLK_MASTER);

	/* keep original clock config */
	cfg |= ISC_CLKCFG_ICDIV(5) & ISC_CLKCFG_ICDIV_MASK;
	cfg |= ISC_CLKCFG_ISP_SEL_HCLOCK;

	isc_writel(isc, ISC_CLKCFG, cfg);
	while ((isc_readl(isc, ISC_CLKSR) & ISC_CLK_SIP) == ISC_CLK_SIP);
	/* Enable isp clock */
	isc_writel(isc, ISC_CLKEN, ISC_CLK_ISP);

	pm_runtime_put(soc_host->v4l2_dev.dev);
	return 0;

err_register_soc_camera_host:
	pm_runtime_disable(&pdev->dev);
err_req_irq:
err_ioremap:
	vb2_dma_contig_cleanup_ctx(isc->alloc_ctx);
err_alloc_ctx:
	dma_free_coherent(&pdev->dev,
			sizeof(struct fbd_v0) * MAX_BUFFER_NUM,
			isc->p_fb_descriptors,
			isc->fb_descriptors_phys);

	return ret;
}

static int atmel_isc_runtime_suspend(struct device *dev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(dev);
	struct atmel_isc *isc = to_isc(soc_host);
	if (isc->iscck)
		clk_disable_unprepare(isc->iscck);
	clk_disable_unprepare(isc->pclk);

	return 0;
}
static int atmel_isc_runtime_resume(struct device *dev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(dev);
	struct atmel_isc *isc = to_isc(soc_host);
	if (isc->iscck)
		clk_prepare_enable(isc->iscck);
	return clk_prepare_enable(isc->pclk);
}

static const struct dev_pm_ops atmel_isc_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(atmel_isc_runtime_suspend,
				atmel_isc_runtime_resume, NULL)
};

static const struct of_device_id atmel_isc_of_match[] = {
	{ .compatible = "atmel,sama5d2-isc" },
	{ }
};
MODULE_DEVICE_TABLE(of, atmel_isc_of_match);

static struct platform_driver atmel_isc_driver = {
	.remove		= atmel_isc_remove,
	.driver		= {
		.name = "atmel_isc",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(atmel_isc_of_match),
		.pm	= &atmel_isc_dev_pm_ops,
	},
};

module_platform_driver_probe(atmel_isc_driver, atmel_isc_probe);

MODULE_AUTHOR("Josh Wu <josh.wu@atmel.com>");
MODULE_DESCRIPTION("The V4L2 driver for Atmel-ISC");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("video");
