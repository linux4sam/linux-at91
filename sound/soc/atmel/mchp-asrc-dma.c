// SPDX-License-Identifier: GPL-2.0
// ASoC Platform (DMA) driver for Microchip ASRC
//
// Copyright (C) 2018-2022 Microchip Technology Inc. and its subsidiaries
//
// Author: Codrin Ciubotariu <codrin.ciubotariu@microchip.com>

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>

#include "mchp-asrc.h"

#define MCHP_ASRC_DMAENGINE_PCM_DRV_NAME  "mchp_snd_asrc_dmaengine_pcm"
#define MCHP_ASRC_DMABUF_SIZE	(512 * 1024)

struct mchp_asrc_dma_be_priv {
	struct device				*dev;
	struct of_phandle_args			*phandle;
	struct snd_soc_dpcm			*dpcm;
	struct dma_chan				*chan;
	struct dma_async_tx_descriptor		*desc;
	struct list_head			list;
};

struct mchp_asrc_dma_priv {
	struct device				*dev;
	struct list_head			dma_in_head;
	struct list_head			dma_out_head;
	unsigned int				refcount;
	/* used for FE */
	dma_cookie_t				cookie;
	unsigned int				pos;
	unsigned int				no_residue: 1;
};

static struct snd_pcm_hardware mchp_asrc_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID,
	.buffer_bytes_max = MCHP_ASRC_DMABUF_SIZE,
	.period_bytes_min = 256,
	.periods_min = 2,
	.periods_max = UINT_MAX,
	.fifo_size = 0,
};

static inline struct mchp_asrc_dma_priv *
mchp_asrc_substream_to_prtd(const struct snd_pcm_substream *substream)
{
	return substream->runtime->private_data;
}

static void
snd_asrc_pcm_refine_runtime_hwparams(struct snd_pcm_substream *substream,
				     struct snd_pcm_hardware *hw, struct dma_slave_caps *dma_caps)
{
	u32 addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
			  BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
			  BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	snd_pcm_format_t i;

	if (dma_caps) {
		if (dma_caps->cmd_pause && dma_caps->cmd_resume)
			hw->info |= SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME;
		if (dma_caps->residue_granularity <= DMA_RESIDUE_GRANULARITY_SEGMENT)
			hw->info |= SNDRV_PCM_INFO_BATCH;

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			addr_widths = dma_caps->dst_addr_widths;
		else
			addr_widths = dma_caps->src_addr_widths;
	}

	pcm_for_each_format(i) {
		int bits = snd_pcm_format_physical_width(i);

		/*
		 * Enable only samples with DMA supported physical
		 * widths
		 */
		switch (bits) {
		case 8:
		case 16:
		case 24:
		case 32:
		case 64:
			if (addr_widths & (1 << (bits / 8)))
				hw->formats |= pcm_format_to_bits(i);
			break;
		default:
			/* Unsupported types */
			break;
		}
	}
}

static int mchp_asrc_dma_open(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct dma_slave_caps dma_caps;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct device *dev = component->dev;
	struct dma_chan *tmp_chan;
	struct mchp_asrc_dma_priv *dma_priv;
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	char tmp_str[4];
	int ret;
	int i;

	if (runtime->private_data) {
		dma_priv = runtime->private_data;
		dma_priv->refcount++;
		dev_dbg(dev, "%s(): DMA private data already allocated\n", __func__);
		return 0;
	}

	dma_priv = kzalloc(sizeof(*dma_priv), GFP_KERNEL);
	if (!dma_priv)
		return -ENOMEM;
	dma_priv->dev = dev;
	INIT_LIST_HEAD(&dma_priv->dma_in_head);
	INIT_LIST_HEAD(&dma_priv->dma_out_head);
	dma_priv->refcount++;
	runtime->private_data = dma_priv;

	ret = snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0) {
		dev_err(dev, "snd_pcm_hw_constraint_integer() failed for HW_PARAM_PERIODS: %d\n",
			ret);
		goto __out_data_free;
	}

	/* since we are not preallocating the DMA channels, we use a
	 * dummy DMA channel, just to get the capabilities
	 */
	for (i = 0; i < MCHP_ASRC_NB_STEREO_CH; i++) {
		sprintf(tmp_str, "%cx%d",  is_playback ? 't' : 'r', i);

		tmp_chan = dma_request_chan(dev, tmp_str);
		if (!IS_ERR(tmp_chan))
			break;
	}
	if (i == MCHP_ASRC_NB_STEREO_CH) {
		dev_err(dev, "no free DMA %s channels\n", is_playback ? "tx" : "rx");
		ret = -EINVAL;
		goto __out_data_free;
	}

	mchp_asrc_hw.period_bytes_max = dma_get_max_seg_size(tmp_chan->device->dev);
	ret = dma_get_slave_caps(tmp_chan, &dma_caps);
	if (ret == 0) {
		dma_priv->no_residue = dma_caps.residue_granularity ==
			DMA_RESIDUE_GRANULARITY_DESCRIPTOR;

		/* Refine mchp_asrc_hw according to caps of DMA */
		snd_asrc_pcm_refine_runtime_hwparams(substream, &mchp_asrc_hw, &dma_caps);
	} else {
		snd_asrc_pcm_refine_runtime_hwparams(substream, &mchp_asrc_hw, NULL);
	}

	ret = snd_soc_set_runtime_hwparams(substream, &mchp_asrc_hw);
	if (ret < 0) {
		dev_err(dev, "runtime_hwparams() failed: %d\n", ret);
		goto __out_release_chan;
	}

	dma_release_channel(tmp_chan);

	return 0;

__out_release_chan:
	dma_release_channel(tmp_chan);
__out_data_free:
	dma_priv->refcount--;
	if (!dma_priv->refcount) {
		kfree(dma_priv);
		runtime->private_data = NULL;
	}

	return ret;
}

static int mchp_asrc_dma_close(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct mchp_asrc_dma_priv *dma_priv = runtime->private_data;

	dma_priv->refcount--;
	if (!dma_priv->refcount) {
		kfree(dma_priv);
		runtime->private_data = NULL;
	}

	return 0;
}

static void mchp_asrc_start_handle(unsigned long data)
{
	struct mchp_asrc_dma_priv *dma_priv = (struct mchp_asrc_dma_priv *)data;
	struct mchp_asrc_dma_be_priv *dma_be_priv;

	list_for_each_entry(dma_be_priv, &dma_priv->dma_in_head, list) {
		if (unlikely(!dma_be_priv->desc)) {
			dev_err(dma_priv->dev, "in BE %s DMA descriptor not initialized\n",
				dma_be_priv->phandle ? dma_be_priv->phandle->np->name :
						       "front-end");
			return;
		}
		dev_dbg(dma_priv->dev, "%s() DMA: in BE channel %s for %s\n", __func__,
			dma_be_priv->desc->chan->name,
			dma_be_priv->phandle ? dma_be_priv->phandle->np->name : "front-end");
	}
	list_for_each_entry(dma_be_priv, &dma_priv->dma_out_head, list) {
		if (unlikely(!dma_be_priv->desc)) {
			dev_err(dma_priv->dev, "out BE %s DMA descriptor not initialized\n",
				dma_be_priv->phandle ? dma_be_priv->phandle->np->name :
						       "front-end");
			return;
		}
		dev_dbg(dma_priv->dev, "%s() DMA: out BE channel %s for %s\n", __func__,
			dma_be_priv->desc->chan->name,
			dma_be_priv->phandle ? dma_be_priv->phandle->np->name : "front-end");
	}

	/* start transmit channel DMAs */
	dev_info(dma_priv->dev, "ASRC IN AIF(s):\n");
	list_for_each_entry(dma_be_priv, &dma_priv->dma_in_head, list) {
		if (!dma_be_priv->phandle) {
			dma_priv->cookie = dmaengine_submit(dma_be_priv->desc);
			dev_info(dma_priv->dev, "\tfront-end\n");
		} else {
			dmaengine_submit(dma_be_priv->desc);
			dev_info(dma_priv->dev, "\t%s, %d channels\n",
				 dma_be_priv->phandle->np->full_name,
				 params_channels(&dma_be_priv->dpcm->be->dpcm[SNDRV_PCM_STREAM_CAPTURE].hw_params));
		}
	}
	list_for_each_entry(dma_be_priv, &dma_priv->dma_in_head, list)
		dma_async_issue_pending(dma_be_priv->desc->chan);

	/* start receive channel DMAs */
	dev_info(dma_priv->dev, "ASRC OUT AIF(s):\n");
	list_for_each_entry(dma_be_priv, &dma_priv->dma_out_head, list) {
		if (!dma_be_priv->phandle) {
			dma_priv->cookie = dmaengine_submit(dma_be_priv->desc);
			dev_info(dma_priv->dev, "\tfront-end\n");
		} else {
			dmaengine_submit(dma_be_priv->desc);
			dev_info(dma_priv->dev, "\t%s, %d channels\n",
				 dma_be_priv->phandle->np->full_name,
				 params_channels(&dma_be_priv->dpcm->be->dpcm[SNDRV_PCM_STREAM_PLAYBACK].hw_params));
		}
	}
	list_for_each_entry(dma_be_priv, &dma_priv->dma_out_head, list)
		dma_async_issue_pending(dma_be_priv->desc->chan);
}

static int mchp_asrc_dmaengine_pcm_prepare_slave_config(struct device *dev,
							struct snd_pcm_substream *substream,
							struct snd_pcm_hw_params *params,
							struct snd_dmaengine_dai_dma_data *dma_data,
							struct dma_slave_config *slave_config)
{
	int ret;

	ret = snd_hwparams_to_dma_slave_config(substream, params, slave_config);
	if (ret)
		return ret;

	snd_dmaengine_pcm_set_config_from_dai_data(substream, dma_data, slave_config);

	dev_dbg(dev,
		"%s() FE AIF dir %s, src addr %pap, dst addr: %pap, src_addr_width %d, dst_addr_width %d, src maxburst %u, dst maxburst %u\n",
		__func__,
		slave_config->direction == DMA_MEM_TO_DEV ? "DMA_MEM_TO_DEV" :
							"DMA_DEV_TO_MEM",
		&slave_config->src_addr, &slave_config->dst_addr,
		slave_config->src_addr_width, slave_config->dst_addr_width,
		slave_config->src_maxburst, slave_config->dst_maxburst);

	return 0;
}

static int
mchp_asrc_be_dmaengine_slave_config(struct mchp_asrc_dmaengine_dai_dma *dma,
				    struct mchp_asrc_dma_be_priv *dma_be_priv,
				    int is_playback)
{
	struct dma_slave_config config_be = {0};
	struct snd_soc_dpcm *dpcm = dma_be_priv->dpcm;
	struct snd_soc_pcm_runtime *be = dpcm->be;
	struct snd_soc_dai *dai = snd_soc_rtd_to_cpu(be, 0);
	struct device *dev = dma_be_priv->dev;
	struct mchp_asrc_dmaengine_be_dma *dma_be;
	struct snd_dmaengine_dai_dma_data *dma_data_be;
	struct list_head *dmaengine_list;
	enum dma_slave_buswidth buswidth;
	int ret;
	int bits_be;

	dmaengine_list = is_playback ? &dma->dma_out_list : &dma->dma_in_list;
	list_for_each_entry(dma_be, dmaengine_list, list) {
		if (dma_be->phandle && dma_be->phandle->np == dai->dev->of_node) {
			dev_dbg(dev, "found DMA data %s\n", dma_be->phandle->np->name);
			break;
		}
	}
	if (list_entry_is_head(dma_be, dmaengine_list, list)) {
		dev_err(dev, "DMA data not found for BE %s\n", dai->name);
		return -EINVAL;
	}

	dma_be_priv->phandle = dma_be->phandle;

	dma_data_be = &dma_be->dma_data;
	dma_be_priv->chan = dma_request_slave_channel(dev, dma_data_be->chan_name);
	if (!dma_be_priv->chan) {
		dev_err(dai->dev, "failed to request DMA channel %s for BE\n",
			dma_data_be->chan_name);
		ret = -EBUSY;
		goto __cleanup_phandle;
	}

	bits_be = params_physical_width(&be->dpcm[!is_playback].hw_params);

	if (bits_be < 8 || bits_be > 64) {
		dev_err(dai->dev, "invalid physical width %d for BE %s\n", bits_be, dai->name);
		ret = -EINVAL;
		goto __cleanup_chan;
	}
	if (bits_be == 8)
		buswidth = DMA_SLAVE_BUSWIDTH_1_BYTE;
	else if (bits_be == 16)
		buswidth = DMA_SLAVE_BUSWIDTH_2_BYTES;
	else if (bits_be == 24)
		buswidth = DMA_SLAVE_BUSWIDTH_3_BYTES;
	else if (bits_be <= 32)
		buswidth = DMA_SLAVE_BUSWIDTH_4_BYTES;
	else
		buswidth = DMA_SLAVE_BUSWIDTH_8_BYTES;

	if (is_playback) {
		config_be.direction = DMA_DEV_TO_MEM;
		config_be.src_addr_width = buswidth;
		config_be.src_maxburst = dma_data_be->maxburst;
		config_be.src_addr = dma_data_be->addr;
	} else {
		config_be.direction = DMA_MEM_TO_DEV;
		config_be.dst_addr_width = buswidth;
		config_be.dst_maxburst = dma_data_be->maxburst;
		config_be.dst_addr = dma_data_be->addr;
	}
	config_be.device_fc = false;

	dev_dbg(dev,
		"%s() AIF %s dir %s, src addr %pap, dst addr: %pap, src_addr_width %d, dst_addr_width %d, src maxburst %u, dst maxburst %u\n",
		__func__, dai->name,
		config_be.direction == DMA_MEM_TO_DEV ? "DMA_MEM_TO_DEV" :
							"DMA_DEV_TO_MEM",
		&config_be.src_addr, &config_be.dst_addr,
		config_be.src_addr_width, config_be.dst_addr_width,
		config_be.src_maxburst, config_be.dst_maxburst);

	ret = dmaengine_slave_config(dma_be_priv->chan, &config_be);
	if (ret) {
		dev_err(dev, "failed to config DMA channel for BE %s: %d\n", dai->name, ret);
		goto __cleanup_chan;
	}

	return 0;

__cleanup_chan:
	dma_release_channel(dma_be_priv->chan);
__cleanup_phandle:
	dma_be_priv->phandle = NULL;
	return ret;
}

static int mchp_asrc_dma_hw_params(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct dma_slave_config config_fe = {0};
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct mchp_asrc_dma_priv *priv = mchp_asrc_substream_to_prtd(substream);
	struct snd_soc_dpcm *dpcm;
	struct device *dev = component->dev;
	struct mchp_asrc_dmaengine_dai_dma *dma;
	struct snd_dmaengine_dai_dma_data *dma_data_fe = NULL;
	struct snd_pcm_substream *substream_be;
	struct mchp_asrc_dma_be_priv *dma_be_priv, *tmp, *dma_fe_priv;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct mchp_asrc_dmaengine_be_dma *dma_be;
	struct list_head *dma_be_list;
	struct list_head *dma_be_head;
	int ret;
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;

	/* save BEs for this substream */
	for_each_dpcm_be(rtd, substream->stream, dpcm) {
		struct snd_soc_pcm_runtime *be = dpcm->be;

		if (dpcm->fe != rtd)
			continue;

		substream_be = snd_soc_dpcm_get_substream(be, substream->stream);
		if (!substream_be)
			continue;

		dma_be_priv = kzalloc(sizeof(*dma_be_priv), GFP_KERNEL);
		if (!dma_be_priv) {
			ret = -ENOMEM;
			goto __cleanup_dma_free;
		}
		dma_be_priv->dev = component->dev;
		dma_be_priv->dpcm = dpcm;

		if (is_playback)
			list_add_tail(&dma_be_priv->list, &priv->dma_out_head);
		else
			list_add_tail(&dma_be_priv->list, &priv->dma_in_head);
		dev_dbg(dev, "%s: add %s %s\n", __func__, is_playback ? "OUT" : "IN",
			snd_soc_rtd_to_cpu(be, 0)->name);
	}

	dma = snd_soc_dai_get_dma_data(cpu_dai, substream);
	if (!dma) {
		if (params) {
			dev_err(rtd->dev, "platform data not found for FE %s\n", cpu_dai->name);
			ret = -EINVAL;
			goto __cleanup_dma_free;
		}
		dev_dbg(rtd->dev, "platform data not set; probably half stream called: stream %s\n",
			is_playback ? "playback" : "capture");
		return 0;
	}

	/* start DMA config; start with current stream DMAs */
	dma_be_head = is_playback ? &priv->dma_out_head : &priv->dma_in_head;
	list_for_each_entry(dma_be_priv, dma_be_head, list) {
		ret = mchp_asrc_be_dmaengine_slave_config(dma, dma_be_priv, is_playback);
		if (ret < 0)
			goto __cleanup_dma_be;
	}

	/* configure DMAs for the 'other' substream */
	if (!params) {
		dma_be_head = is_playback ? &priv->dma_in_head : &priv->dma_out_head;
		list_for_each_entry(dma_be_priv, dma_be_head, list) {
			ret = mchp_asrc_be_dmaengine_slave_config(dma, dma_be_priv, !is_playback);
			if (ret < 0)
				goto __cleanup_dma_rev;
		}

		return 0;
	}

	/* set FE */
	dma_be_list = is_playback ? &dma->dma_in_list : &dma->dma_out_list;

	list_for_each_entry(dma_be, dma_be_list, list) {
		if (!dma_be->phandle) {
			dma_data_fe = &dma_be->dma_data;
			break;
		}
	}
	if (!dma_data_fe) {
		dev_err(dev, "FE DMA data not found\n");
		ret = -EINVAL;
		goto __cleanup_dma_rev;
	}

	dma_fe_priv = kzalloc(sizeof(*dma_fe_priv), GFP_KERNEL);
	if (!dma_fe_priv) {
		ret = -ENOMEM;
		goto __cleanup_dma_rev;
	}

	dma_fe_priv->chan = dma_request_slave_channel(dev, dma_data_fe->chan_name);
	if (!dma_fe_priv->chan) {
		dev_err(dev, "failed to request DMA channel %s for FE %s\n",
			dma_data_fe->chan_name, cpu_dai->name);
		ret = -EINVAL;
		goto __cleanup_free_fe;
	}

	ret = mchp_asrc_dmaengine_pcm_prepare_slave_config(dev, substream, params,
							   dma_data_fe, &config_fe);
	if (ret) {
		dev_err(dev, "failed to prepare DMA config for FE %s\n", cpu_dai->name);
		goto __cleanup_dma_fe;
	}

	dev_dbg(dev,
		"%s() config %s: dir %s, src addr %pap, dst addr %pap, src_addr_width %d, dst_addr_width %d, src maxburst %u, dst maxburst %u\n",
		__func__, cpu_dai->name,
		config_fe.direction == DMA_MEM_TO_DEV ? "MEM_TO_DEV" :
							"DEV_TO_MEM",
		&config_fe.src_addr, &config_fe.dst_addr,
		config_fe.src_addr_width, config_fe.dst_addr_width,
		config_fe.src_maxburst, config_fe.dst_maxburst);

	ret = dmaengine_slave_config(dma_fe_priv->chan, &config_fe);
	if (ret) {
		dev_err(dev, "failed to config DMA channel for FE %s\n", cpu_dai->name);
		goto __cleanup_dma_fe;
	}

	/* on playback, FE is in AIF and on capture FE is out AIF */
	if (is_playback)
		list_add_tail(&dma_fe_priv->list, &priv->dma_in_head);
	else
		list_add_tail(&dma_fe_priv->list, &priv->dma_out_head);

	dev_dbg(component->dev, "%s: added %s %s\n", __func__, is_playback ? "IN" : "OUT",
		cpu_dai->name);
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV_IRAM, dev, params_buffer_bytes(params),
				  &substream->dma_buffer);
	if (ret < 0)
		goto __cleanup_dma_fe_list;

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	return 0;

__cleanup_dma_fe_list:
	list_del(&dma_fe_priv->list);
__cleanup_dma_fe:
	dma_release_channel(dma_fe_priv->chan);
	dma_fe_priv->chan = NULL;
__cleanup_free_fe:
	kfree(dma_fe_priv);
__cleanup_dma_rev:
	if (!params) {
		dma_be_head = is_playback ? &priv->dma_in_head : &priv->dma_out_head;
		list_for_each_entry(dma_be_priv, dma_be_head, list) {
			if (dma_be_priv->chan) {
				dma_release_channel(dma_be_priv->chan);
				dma_be_priv->chan = NULL;
			}
		}
	}
__cleanup_dma_be:
	dma_be_head = is_playback ? &priv->dma_out_head : &priv->dma_in_head;
	list_for_each_entry(dma_be_priv, dma_be_head, list) {
		if (dma_be_priv->chan) {
			dma_release_channel(dma_be_priv->chan);
			dma_be_priv->chan = NULL;
		}
	}
__cleanup_dma_free:
	dma_be_head = is_playback ? &priv->dma_out_head : &priv->dma_in_head;
	list_for_each_entry_safe(dma_be_priv, tmp, dma_be_head, list) {
		list_del(&dma_be_priv->list);
		kfree(dma_be_priv);
	}

	return ret;
}

static int mchp_asrc_dma_hw_free(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream)
{
	struct mchp_asrc_dma_priv *priv = mchp_asrc_substream_to_prtd(substream);
	struct mchp_asrc_dma_be_priv *dma_be_priv, *tmp;

	if (substream->dma_buffer.area) {
		snd_dma_free_pages(&substream->dma_buffer);
		substream->dma_buffer.area = NULL;
		snd_pcm_set_runtime_buffer(substream, NULL);
	}

	list_for_each_entry(dma_be_priv, &priv->dma_in_head, list) {
		if (dma_be_priv->chan)
			dmaengine_synchronize(dma_be_priv->chan);
	}

	list_for_each_entry(dma_be_priv, &priv->dma_out_head, list) {
		if (dma_be_priv->chan)
			dmaengine_synchronize(dma_be_priv->chan);
	}

	list_for_each_entry_safe(dma_be_priv, tmp, &priv->dma_in_head, list) {
		if (dma_be_priv->chan)
			dma_release_channel(dma_be_priv->chan);
		dma_be_priv->chan = NULL;
		list_del(&dma_be_priv->list);
		kfree(dma_be_priv);
	}

	list_for_each_entry_safe(dma_be_priv, tmp, &priv->dma_out_head, list) {
		if (dma_be_priv->chan)
			dma_release_channel(dma_be_priv->chan);
		dma_be_priv->chan = NULL;
		list_del(&dma_be_priv->list);
		kfree(dma_be_priv);
	}

	return 0;
}

static void mchp_asrc_pcm_dma_complete(void *arg)
{
	struct snd_pcm_substream *substream = arg;
	struct mchp_asrc_dma_priv *priv = mchp_asrc_substream_to_prtd(substream);

	priv->pos += snd_pcm_lib_period_bytes(substream);
	if (priv->pos >= snd_pcm_lib_buffer_bytes(substream))
		priv->pos = 0;
	snd_pcm_period_elapsed(substream);
}

static int mchp_asrc_dma_prepare(struct snd_soc_component *component,
				 struct snd_pcm_substream *subs, struct mchp_asrc_dma_priv *priv)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(subs);
	struct device *dev = component->dev;
	unsigned long flags = DMA_CTRL_ACK;
	enum dma_transfer_direction dir;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct mchp_asrc_dmaengine_dai_dma *dma;
	struct mchp_asrc_dma_be_priv *dma_fe_priv = NULL, *dma_be_priv;
	struct dma_async_tx_descriptor *desc;
	bool is_playback = subs->stream == SNDRV_PCM_STREAM_PLAYBACK;
	struct list_head *dma_be_head;
	int stream;

	dma = snd_soc_dai_get_dma_data(cpu_dai, subs);
	if (!dma) {
		dev_dbg(rtd->dev, "platform data not found for FE %s\n", cpu_dai->name);
		return 0;
	}

	if (is_playback) {
		dma_be_head = &priv->dma_out_head;
		stream = SNDRV_PCM_STREAM_PLAYBACK;
	} else {
		dma_be_head = &priv->dma_in_head;
		stream = SNDRV_PCM_STREAM_CAPTURE;
	}
	list_for_each_entry(dma_be_priv, dma_be_head, list) {
		struct snd_soc_dpcm *dpcm;
		struct snd_soc_pcm_runtime *be;
		struct snd_soc_dai *dai;
		struct snd_pcm_substream *subs_be;
		struct snd_pcm_runtime *rtm_be;

		if (!dma_be_priv->phandle) {
			if (dma_fe_priv) {
				dev_err(dev, "found multiple AIFs without DMA data\n");
				return -EINVAL;
			}
			/* private DMA data for FE */
			dma_fe_priv = dma_be_priv;
			continue;
		}

		dpcm = dma_be_priv->dpcm;
		be = dpcm->be;
		dai = snd_soc_rtd_to_cpu(be, 0);

		dev_dbg(dev, "%s() OUT BE DAI %s: rate=%u format=%#x width=%u channels=%u\n",
			__func__, dai->name, params_rate(&be->dpcm[stream].hw_params),
			params_format(&be->dpcm[stream].hw_params),
			params_width(&be->dpcm[stream].hw_params),
			params_channels(&be->dpcm[stream].hw_params));

		if (!dma_be_priv->chan) {
			dev_err(dev, "DMA channel not found for BE %s\n", dai->name);
			return -EINVAL;
		}

		subs_be = snd_soc_dpcm_get_substream(be, stream);
		rtm_be = subs_be->runtime;

		dev_dbg(dev,
			"%s() BE buffer_size(periods) %ld, buffer_size(bytes) %zu period_size(frames) %ld period_size(bytes) %zu\n",
			__func__,
			rtm_be->buffer_size, snd_pcm_lib_buffer_bytes(subs_be),
			rtm_be->period_size, snd_pcm_lib_period_bytes(subs_be));

		desc = dmaengine_prep_dma_cyclic(dma_be_priv->chan, rtm_be->dma_addr,
						 snd_pcm_lib_buffer_bytes(subs_be),
						 snd_pcm_lib_period_bytes(subs_be),
						 is_playback ? DMA_DEV_TO_MEM :
							       DMA_MEM_TO_DEV, DMA_CTRL_ACK);
		if (!desc) {
			dev_err(dev, "failed to prepare client DMA for BE %s\n", dai->name);
			return -ENOMEM;
		}
		dma_be_priv->desc = desc;
	}

	if (is_playback) {
		dma_be_head = &priv->dma_in_head;
		stream = SNDRV_PCM_STREAM_CAPTURE;
	} else {
		dma_be_head = &priv->dma_out_head;
		stream = SNDRV_PCM_STREAM_PLAYBACK;
	}

	/* get FE DMA data, if available */
	list_for_each_entry(dma_be_priv, dma_be_head, list) {
		struct snd_soc_dpcm *dpcm;
		struct snd_soc_pcm_runtime *be;
		struct snd_soc_dai *dai;
		struct snd_pcm_substream *subs_be;
		struct snd_pcm_runtime *rtm_be;

		if (!dma_be_priv->phandle) {
			if (dma_fe_priv) {
				dev_err(dev, "found multiple AIFs without DMA data\n");
				return -EINVAL;
			}
			/* private DMA data for FE */
			dma_fe_priv = dma_be_priv;
			continue;
		}

		dpcm = dma_be_priv->dpcm;
		be = dpcm->be;
		dai = snd_soc_rtd_to_cpu(be, 0);

		dev_dbg(dev, "%s() IN BE DAI %s: rate=%u format=%#x width=%u channels=%u\n",
			__func__, dai->name,
			params_rate(&be->dpcm[stream].hw_params),
			params_format(&be->dpcm[stream].hw_params),
			params_width(&be->dpcm[stream].hw_params),
			params_channels(&be->dpcm[stream].hw_params));

		if (!dma_be_priv->chan) {
			dev_err(dev, "DMA channel not found for BE %s\n", dai->name);
			return -EINVAL;
		}

		subs_be = snd_soc_dpcm_get_substream(be, stream);
		rtm_be = subs_be->runtime;

		dev_dbg(dev,
			"%s() BE buffer_size(periods) %ld, buffer_size(bytes) %zu period_size(frames) %ld period_size(bytes) %zu\n",
			__func__,
			rtm_be->buffer_size, snd_pcm_lib_buffer_bytes(subs_be),
			rtm_be->period_size, snd_pcm_lib_period_bytes(subs_be));

		desc = dmaengine_prep_dma_cyclic(dma_be_priv->chan, rtm_be->dma_addr,
						 snd_pcm_lib_buffer_bytes(subs_be),
						 snd_pcm_lib_period_bytes(subs_be),
						 !is_playback ? DMA_DEV_TO_MEM :
							       DMA_MEM_TO_DEV, DMA_CTRL_ACK);
		if (!desc) {
			dev_err(dev, "failed to prepare client DMA for BE %s\n", dai->name);
			return -ENOMEM;
		}
		dma_be_priv->desc = desc;
	}

	/* FE present, initialize the DMA transfer to user space */
	if (dma_fe_priv) {
		if (!subs->runtime->no_period_wakeup)
			flags |= DMA_PREP_INTERRUPT;

		priv->pos = 0;
		dir = snd_pcm_substream_to_dma_direction(subs);
		desc = dmaengine_prep_dma_cyclic(dma_fe_priv->chan, subs->runtime->dma_addr,
						 snd_pcm_lib_buffer_bytes(subs),
						 snd_pcm_lib_period_bytes(subs), dir, flags);
		if (!desc) {
			dev_err(dev, "failed to prepare client DMA for FE %s\n", cpu_dai->name);
			return -ENOMEM;
		}

		dma_fe_priv->desc = desc;
		dma_fe_priv->desc->callback = mchp_asrc_pcm_dma_complete;
		dma_fe_priv->desc->callback_param = subs;

		dev_dbg(dev,
			"%s() FE buffer_size(periods) %ld, buffer_size(bytes) %zu period_size(frames) %ld period_size(bytes) %zu\n",
			__func__,
			subs->runtime->buffer_size, snd_pcm_lib_buffer_bytes(subs),
			subs->runtime->period_size, snd_pcm_lib_period_bytes(subs));
	} else {
		dev_dbg(dev, "%s() FE DMA not found\n", __func__);
	}

	dev_dbg(dev, "%s: initializing start_handle tasklet\n", __func__);
	tasklet_init(&dma->start_handle, &mchp_asrc_start_handle, (unsigned long)priv);

	return 0;
}

static void mchp_asrc_dmaengine_resume(struct mchp_asrc_dma_priv *priv)
{
	struct mchp_asrc_dma_be_priv *dma_be_priv;

	list_for_each_entry(dma_be_priv, &priv->dma_in_head, list) {
		if (dma_be_priv->chan)
			dmaengine_resume(dma_be_priv->chan);
	}
	list_for_each_entry(dma_be_priv, &priv->dma_out_head, list) {
		if (dma_be_priv->chan)
			dmaengine_resume(dma_be_priv->chan);
	}
}

static void mchp_asrc_dmaengine_pause(struct mchp_asrc_dma_priv *priv)
{
	struct mchp_asrc_dma_be_priv *dma_be_priv;

	list_for_each_entry(dma_be_priv, &priv->dma_in_head, list) {
		if (dma_be_priv->chan)
			dmaengine_pause(dma_be_priv->chan);
	}
	list_for_each_entry(dma_be_priv, &priv->dma_out_head, list) {
		if (dma_be_priv->chan)
			dmaengine_pause(dma_be_priv->chan);
	}
}

static void mchp_asrc_dmaengine_term(struct mchp_asrc_dma_priv *priv)
{
	struct mchp_asrc_dma_be_priv *dma_be_priv;

	list_for_each_entry(dma_be_priv, &priv->dma_in_head, list) {
		if (dma_be_priv->chan)
			dmaengine_terminate_async(dma_be_priv->chan);
		else
			dev_dbg(priv->dev, "no DMA channel in IN queue\n");
	}
	list_for_each_entry(dma_be_priv, &priv->dma_out_head, list) {
		if (dma_be_priv->chan)
			dmaengine_terminate_async(dma_be_priv->chan);
		else
			dev_dbg(priv->dev, "no DMA channel in OUT queue\n");
	}
}

static int mchp_asrc_dma_trigger(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct mchp_asrc_dma_priv *priv = mchp_asrc_substream_to_prtd(substream);
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		ret = mchp_asrc_dma_prepare(component, substream, priv);
		if (ret)
			return ret;
		break;
	case SNDRV_PCM_TRIGGER_RESUME:
		fallthrough;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		mchp_asrc_dmaengine_resume(priv);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (runtime->info & SNDRV_PCM_INFO_PAUSE)
			mchp_asrc_dmaengine_pause(priv);
		else
			mchp_asrc_dmaengine_term(priv);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		mchp_asrc_dmaengine_pause(priv);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		mchp_asrc_dmaengine_term(priv);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t
mchp_asrc_dma_pcm_pointer(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct mchp_asrc_dma_priv *priv = mchp_asrc_substream_to_prtd(substream);
	struct mchp_asrc_dma_be_priv *dma_fe_priv;
	struct dma_tx_state state;
	enum dma_status status;
	unsigned int buf_size;
	unsigned int pos = 0;
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	struct list_head *dma_fe_head = is_playback ? &priv->dma_in_head : &priv->dma_out_head;

	priv = mchp_asrc_substream_to_prtd(substream);
	if (priv->no_residue)
		return bytes_to_frames(runtime, priv->pos);

	list_for_each_entry(dma_fe_priv, dma_fe_head, list) {
		if (!dma_fe_priv->phandle)
			break;
	}
	if (list_entry_is_head(dma_fe_priv, dma_fe_head, list)) {
		dev_err(priv->dev, "FE DMA phandle not found\n");
		return 0;
	}
	status = dmaengine_tx_status(dma_fe_priv->chan, priv->cookie, &state);
	if (status == DMA_IN_PROGRESS || status == DMA_PAUSED) {
		buf_size = snd_pcm_lib_buffer_bytes(substream);
		if (state.residue > 0 && state.residue <= buf_size)
			pos = buf_size - state.residue;

		runtime->delay = bytes_to_frames(runtime, state.in_flight_bytes);
	}

	return bytes_to_frames(substream->runtime, pos);
}

static int mchp_asrc_dma_pcm_new(struct snd_soc_component *component,
				 struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm *pcm = rtd->pcm;
	int i;

	for (i = SNDRV_PCM_STREAM_PLAYBACK; i <= SNDRV_PCM_STREAM_LAST; i++) {
		substream = pcm->streams[i].substream;
		if (!substream)
			continue;

		substream->dma_buffer.bytes = 0;
		substream->dma_buffer.area = NULL;
		substream->dma_buffer.addr = 0;
		substream->dma_buffer.private_data = NULL;
	}

	return 0;
}

static void mchp_asrc_dma_pcm_free(struct snd_soc_component *component,
				   struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	int i;

	for (i = SNDRV_PCM_STREAM_PLAYBACK; i <= SNDRV_PCM_STREAM_LAST; i++) {
		substream = pcm->streams[i].substream;
		if (!substream)
			continue;

		substream->dma_buffer.area = NULL;
		substream->dma_buffer.addr = 0;
	}
}

const struct snd_soc_component_driver mchp_asrc_platform = {
	.name		= MCHP_ASRC_DMAENGINE_PCM_DRV_NAME,
	.pcm_construct	= mchp_asrc_dma_pcm_new,
	.pcm_destruct	= mchp_asrc_dma_pcm_free,
	.hw_params	= mchp_asrc_dma_hw_params,
	.hw_free	= mchp_asrc_dma_hw_free,
	.trigger	= mchp_asrc_dma_trigger,
	.open		= mchp_asrc_dma_open,
	.close		= mchp_asrc_dma_close,
	.pointer	= mchp_asrc_dma_pcm_pointer,
};
EXPORT_SYMBOL_GPL(mchp_asrc_platform);

int mchp_asrc_pcm_register(struct device *dev)
{
	int err;

	err = devm_snd_soc_register_component(dev, &mchp_asrc_platform, NULL, 0);
	if (err) {
		dev_err(dev, "failed to register ASoC platform for ASRC\n");
		return err;
	}

	return 0;
}
