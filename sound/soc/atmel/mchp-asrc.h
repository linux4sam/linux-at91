/* SPDX-License-Identifier: GPL-2.0 */
// ASoC Platform header file for Microchip ASRC
//
// Copyright (C) 2018 Microchip Technology Inc. and its subsidiaries
//
// Author: Codrin Ciubotariu <codrin.ciubotariu@microchip.com>

#ifndef __MCHP_ASRC_H
#define __MCHP_ASRC_H

#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

#define MCHP_ASRC_NB_CHANNELS		8
#define MCHP_ASRC_NB_STEREO_CH		((MCHP_ASRC_NB_CHANNELS) / 2)

struct mchp_asrc_dmaengine_be_dma {
	struct of_phandle_args *phandle;
	struct snd_dmaengine_dai_dma_data dma_data;
	struct list_head list;
};

struct mchp_asrc_dmaengine_dai_dma {
	struct list_head dma_in_list;
	struct list_head dma_out_list;
	struct tasklet_struct start_handle;
	int unlocked_dsps;
};

int mchp_asrc_pcm_register(struct device *dev);

#endif	/* __MCHP_ASRC_H */
