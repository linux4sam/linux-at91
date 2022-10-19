// SPDX-License-Identifier: GPL-2.0
// ASoC driver for Microchip ASRC
//
// Copyright (C) 2018-2022 Microchip Technology Inc. and its subsidiaries
//
// Author: Codrin Ciubotariu <codrin.ciubotariu@microchip.com>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

#include "mchp-asrc.h"

/*
 * ---- ASRC Controller Register map ----
 */
#define MCHP_ASRC_CR			0x0000	/* Control Register */
#define MCHP_ASRC_MR			0x0004	/* Mode Register */
#define MCHP_ASRC_RATIO(block)		(0x0008 + (block) * 4)	/* Ratio Register x */
#define MCHP_ASRC_VBPS_IN		0x0018	/* Valid Bit Per Sample In Register */
#define MCHP_ASRC_VBPS_OUT		0x001C	/* Valid Bit Per Sample Out Register */
#define MCHP_ASRC_CH_CONF		0x0020	/* Channel Configuration Register */
#define MCHP_ASRC_TRIG			0x0024	/* Trigger Selection Register */

#define MCHP_ASRC_RHR(block)		(0x0028 + 4 * (block))	/* Receive Holding Registers */
#define MCHP_ASRC_THR(block)		(0x0048 + 4 * (block))	/* Transmit Holding Registers */

#define MCHP_ASRC_IER(block)		(0x0068 + 4 * (block))
#define MCHP_ASRC_IDR(block)		(0x0078 + 4 * (block))
#define MCHP_ASRC_IMR(block)		(0x0088 + 4 * (block))
#define MCHP_ASRC_ISR(block)		(0x0098 + 4 * (block))

#define MCHP_ASRC_ESR			0x00A8	/* Error Status Register */

#define MCHP_ASRC_DAPSELR		0x00B4	/* Direct Access Peripheral Select Register */
#define MCHP_ASRC_WPMR			0x00E4	/* Write Protection Mode Register */
#define MCHP_ASRC_WPSR			0x00E8	/* Write Protection Status Register */

#define MCHP_ASRC_VERSION		0x00FC	/* Version Register */

/*
 * ---- Control Register (Write-only) ----
 */
#define MCHP_ASRC_CR_SWRST		BIT(0)

/*
 * ---- Mode Register (Read/Write) ----
 */
/* ASRC Stereo Channel x Enable */
#define MCHP_ASRC_MR_ASRCEN_MASK	GENMASK(3, 0)
#define MCHP_ASRC_MR_ASRCEN(block)	BIT(block)

/* Sampling frequency greater than 96 KHz  */
#define MCHP_ASRC_MR_GT96K		BIT(12)

/* Direct Access Transmit Holding Register Source */
#define MCHP_ASRC_MR_DATHRX(thr)	BIT(16 + (thr))
#define MCHP_ASRC_MR_DATHRX_MASK	GENMASK(19, 16)

/* Direct Access Receive Holding Register Source */
#define MCHP_ASRC_MR_DARHRX(rhr)	BIT(24 + (rhr))
#define MCHP_ASRC_MR_DARHRX_MASK	GENMASK(27, 24)

/*
 * ---- Ratio Register of Stereo Channel x (Read/Write) ----
 */
/* Input Internal Sampling Rate Ratio */
#define MCHP_ASRC_RATIO_INRATIO_MASK	GENMASK(15, 0)
#define MCHP_ASRC_RATIO_INRATIO(ratio)	((ratio) & MCHP_ASRC_RATIO_INRATIO_MASK)

/* Output Internal Sampling Rate Ratio */
#define MCHP_ASRC_RATIO_OUTRATIO_MASK	GENMASK(31, 16)
#define MCHP_ASRC_RATIO_OUTRATIO(ratio) \
	(((ratio) << 16) & MCHP_ASRC_RATIO_OUTRATIO_MASK)

/*
 * ---- Valid bit Per Sample In/Out (Read/Write) ----
 */
#define MCHP_ASRC_VBPS_MASK(block) \
	(GENMASK(3, 0) << ((block) * 8))
#define MCHP_ASRC_VBPS(block, val) \
	(((val) & GENMASK(3, 0)) << ((block) * 8))

#define MCHP_ASRC_VBPS_8_BIT(block)	MCHP_ASRC_VBPS(block, 0)
#define MCHP_ASRC_VBPS_16_BIT(block)	MCHP_ASRC_VBPS(block, 1)
#define MCHP_ASRC_VBPS_20_BIT(block)	MCHP_ASRC_VBPS(block, 2)
#define MCHP_ASRC_VBPS_24_BIT(block)	MCHP_ASRC_VBPS(block, 3)
#define MCHP_ASRC_VBPS_32_BIT(block)	MCHP_ASRC_VBPS(block, 4)
#define MCHP_ASRC_VBPS_10_BIT(block)	MCHP_ASRC_VBPS(block, 5)
#define MCHP_ASRC_VBPS_12_BIT(block)	MCHP_ASRC_VBPS(block, 6)
#define MCHP_ASRC_VBPS_14_BIT(block)	MCHP_ASRC_VBPS(block, 7)
#define MCHP_ASRC_VBPS_18_BIT(block)	MCHP_ASRC_VBPS(block, 8)

/*
 * ---- Channel configuration Registers (Read/Write) ----
 */
/* Operating Mode for Transmit Holding Registers */
#define MCHP_ASRC_CH_CONF_THROPMODE_MASK	GENMASK(2, 0)
#define MCHP_ASRC_CH_CONF_THROPMODE(val) \
	((val) & MCHP_ASRC_CH_CONF_THROPMODE_MASK)

/* Operating Mode for Receive Holding Registers */
#define MCHP_ASRC_CH_CONF_RHROPMODE_MASK	GENMASK(6, 4)
#define MCHP_ASRC_CH_CONF_RHROPMODE(val) \
	(((val) << 4) & MCHP_ASRC_CH_CONF_RHROPMODE_MASK)

/* Audio Block Operating Mode; Mono or Stereo */
#define MCHP_ASRC_CH_CONF_MONO_MASK		GENMASK(11, 8)
#define MCHP_ASRC_CH_CONF_MONO(ch)		BIT((ch) + 8)

/* DMA Audio Block x CHUNK Size */
#define MCHP_ASRC_CH_CONF_CHUNK_MASK(block) \
	(GENMASK(2, 0) << ((block) * 4 + 16))
#define MCHP_ASRC_CH_CONF_CHUNK(block, val) \
	(((val) & GENMASK(2, 0)) << ((block) * 4 + 16))
#define MCHP_ASRC_CH_CONF_CHUNK_1_DATA(block)	MCHP_ASRC_CH_CONF_CHUNK(block, 0)
#define MCHP_ASRC_CH_CONF_CHUNK_2_DATA(block)	MCHP_ASRC_CH_CONF_CHUNK(block, 1)
#define MCHP_ASRC_CH_CONF_CHUNK_4_DATA(block)	MCHP_ASRC_CH_CONF_CHUNK(block, 2)
#define MCHP_ASRC_CH_CONF_CHUNK_8_DATA(block)	MCHP_ASRC_CH_CONF_CHUNK(block, 3)
#define MCHP_ASRC_CH_CONF_CHUNK_16_DATA(block)	MCHP_ASRC_CH_CONF_CHUNK(block, 4)

/*
 * ---- Trigger Selection Registers (Read/Write) ----
 */
#define MCHP_ASRC_TRIG_TRIGSELIN_MASK(ch) \
	(GENMASK(3, 0) << ((ch) * 4))
#define MCHP_ASRC_TRIG_TRIGSELIN(src, ch) \
	(((src) & GENMASK(3, 0)) << ((ch) * 4))

#define MCHP_ASRC_TRIG_TRIGSELOUT_MASK(ch) \
	(GENMASK(3, 0) << ((ch) * 4 + 16))
#define MCHP_ASRC_TRIG_TRIGSELOUT(src, ch) \
	(((src) & GENMASK(3, 0)) << ((ch) * 4 + 16))

/*
 * ---- Interrupt Enable/Disable/Mask/Status Registers ----
 */
#define MCHP_ASRC_IR_RXRDY		BIT(0)
#define MCHP_ASRC_IR_RXEMPTY		BIT(1)
#define MCHP_ASRC_IR_RXFULL		BIT(2)
#define MCHP_ASRC_IR_RXCHUNK		BIT(3)
#define MCHP_ASRC_IR_RXUDR		BIT(4)
#define MCHP_ASRC_IR_RXOVR		BIT(5)

#define MCHP_ASRC_IR_TXRDY		BIT(8)
#define MCHP_ASRC_IR_TXEMPTY		BIT(9)
#define MCHP_ASRC_IR_TXFULL		BIT(10)
#define MCHP_ASRC_IR_TXCHUNK		BIT(11)
#define MCHP_ASRC_IR_TXUDR		BIT(12)
#define MCHP_ASRC_IR_TXOVR		BIT(13)

#define MCHP_ASRC_IR_SECE		BIT(16)

#define MCHP_ASRC_IR_LOCK		BIT(30)

/*
 * ---- Error Status Register (Read-only) ----
 */
#define MCHP_ASRC_ESR_INCFGERR_MASK	GENMASK(4, 0)
#define MCHP_ASRC_ESR_OUTCFGERR_MASK	GENMASK(12, 8)

/* Direct Access Channel x Input Audio Peripheral Source */
#define MCHP_ASRC_DAPSELR_IN_CH_MASK(ch)	GENMASK((2 + (ch) * 4), ((ch) * 4))
#define MCHP_ASRC_DAPSELR_IN_CH(ch, idx)	((idx) << (4 * (ch)))

/* Direct Access Channel x Output Audio Peripheral Destination */
#define MCHP_ASRC_DAPSELR_OUT_CH_MASK(ch)	GENMASK((18 + (ch) * 4), (16 + (ch) * 4))
#define MCHP_ASRC_DAPSELR_OUT_CH(ch, idx)	((idx) << (16 + 4 * (ch)))

/*
 * ---- Version Register (Read-only) ----
 */
#define MCHP_ASRC_VERSION_MASK		GENMASK(11, 0)

#define MCHP_ASRC_LT96K_RATIO_MIN		2048
#define MCHP_ASRC_GT96K_RATIO_MIN		1024
#define MCHP_ASRC_TRIG_IDX_MAX_VAL		16
#define MCHP_ASRC_DP_IDX_MAX			10
#define MCHP_ASRC_RATIO_MAX			MCHP_ASRC_RATIO_INRATIO_MASK
#define MCHP_ASRC_OPMODE_MAX			5

static const struct regmap_config mchp_asrc_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = MCHP_ASRC_VERSION,
};

static const struct snd_soc_dapm_route mchp_asrc_route[] = {
	{"ASRC-Playback 1", NULL, "ASRC-Capture 1"},
	{"ASRC-Playback 2", NULL, "ASRC-Capture 2"},
	{"ASRC-Playback 3", NULL, "ASRC-Capture 3"},
	{"ASRC-Playback 4", NULL, "ASRC-Capture 4"},
};

static const struct snd_soc_component_driver mchp_asrc_component = {
	.name			= "mchp-asrc-dai",
	.dapm_routes		= mchp_asrc_route,
	.num_dapm_routes	= ARRAY_SIZE(mchp_asrc_route),
};

/* one slot in the ASRC, representing one or more DSPs reserverd for PCM */
struct mchp_asrc_slot {
	int first_dsp;
	int dsp_count;
	int slot_id;
};

struct opmode_to_slot {
	struct mchp_asrc_slot slot[MCHP_ASRC_NB_STEREO_CH];
};

/* represents the slots with the used DSPs for each opmode */
static const struct opmode_to_slot asrc_map[] = {
	{{{0, 1, 0},		{1, 1, 1},	{2, 1, 2},	{3, 1, 3}}},
	{{{0, 2, 0},		{2, 1, 1},	{3, 1, 2}}},
	{{{0, 2, 0},		{2, 2, 1}}},
	{{{0, 3, 0},		{3, 1, 1}}},
	{{{0, 4, 0}}},
};

struct xmit_ch {
	int thr_ch[MCHP_ASRC_NB_STEREO_CH];
	int rhr_ch[MCHP_ASRC_NB_STEREO_CH];
	int thr_ch_count;
	int rhr_ch_count;
	int dsps_used;
	bool in_use;
};

struct opmode {
	struct xmit_ch tc[MCHP_ASRC_NB_STEREO_CH];
	int tc_count;
};

/* represents the transmit and receive channels for all opmode combinations */
static struct opmode ch_conf[MCHP_ASRC_OPMODE_MAX][MCHP_ASRC_OPMODE_MAX] = {
	[0][0] = {{
		{{0}, {0}, 1, 1, 1},
		{{1}, {1}, 1, 1, 1},
		{{2}, {2}, 1, 1, 1},
		{{3}, {3}, 1, 1, 1},
	}, 4,					/* THOPMODE 0, RHOPMODE 0 */
	},
	[0][1] = {{
		{{0, 1}, {0}, 2, 1, 2},
		{{2}, {1}, 1, 1, 1},
		{{3}, {2}, 1, 1, 1},
	}, 3,					/* THOPMODE 0, RHOPMODE 1 */
	},
	[0][2] = {{
		{{0, 1}, {0}, 2, 1, 2},
		{{2, 3}, {1}, 2, 1, 2},
	}, 2,					/* THOPMODE 0, RHOPMODE 2 */
	},
	[0][3] = {{
		{{0, 1, 2}, {0}, 3, 1, 3},
		{{3}, {1}, 1, 1, 1},
	}, 2,					/* THOPMODE 0, RHOPMODE 3 */
	},
	[0][4] = {{
		{{0, 1, 2, 3}, {0}, 4, 1, 4},
	}, 1,					/* THOPMODE 0, RHOPMODE 4 */
	},
	[1][0] = {{
		{{0}, {0, 1}, 1, 2, 2},
		{{1}, {2}, 1, 1, 1},
		{{2}, {3}, 1, 1, 1},
	}, 3,					/* THOPMODE 1, RHOPMODE 0 */
	},
	[1][1] = {{
		{{0}, {0}, 1, 1, 2},
		{{1}, {1}, 1, 1, 1},
		{{2}, {2}, 1, 1, 1},
	}, 3,					/* THOPMODE 1, RHOPMODE 1 */
	},
	[1][2] = {{
		{{0}, {0}, 1, 1, 2},
		{{1, 2}, {1}, 2, 1, 2},
	}, 2,					/* THOPMODE 1, RHOPMODE 2 */
	},
	[1][3] = {{
		{{0, 1}, {0}, 2, 1, 3},
		{{2}, {1}, 1, 1, 1},
	}, 2,					/* THOPMODE 1, RHOPMODE 3 */
	},
	[1][4] = {{
		{{0, 1, 2}, {0}, 3, 1, 4},
	}, 1,					/* THOPMODE 1, RHOPMODE 4 */
	},
	[2][0] = {{
		{{0}, {0, 1}, 1, 2, 2},
		{{1}, {2, 3}, 1, 2, 2},
	}, 2,					/* THOPMODE 2, RHOPMODE 0 */
	},
	[2][1] = {{
		{{0}, {0}, 1, 1, 2},
		{{1}, {1, 2}, 1, 2, 2},
	}, 2,					/* THOPMODE 2, RHOPMODE 1 */
	},
	[2][2] = {{
		{{0}, {0}, 1, 1, 2},
		{{1}, {1}, 1, 1, 2},
	}, 2,					/* THOPMODE 2, RHOPMODE 2 */
	},
	[2][3] = {{
		/* not implemented for now */
		{{0}, {0}, 1, 1, 2},
		{{1}, {0}, 1, 1, 1},
		{{1}, {1}, 1, 1, 1},
	}, 3,					/* THOPMODE 2, RHOPMODE 3 */
	},
	[2][4] = {{
		{{0, 1}, {0}, 2, 1, 4},
	}, 1,					/* THOPMODE 2, RHOPMODE 4 */
	},
	[3][0] = {{
		{{0}, {0, 1, 2}, 1, 3, 3},
		{{1}, {3}, 1, 1, 1},
	}, 2,					/* THOPMODE 3, RHOPMODE 0 */
	},
	[3][1] = {{
		{{0}, {0, 1}, 1, 2, 3},
		{{1}, {2}, 1, 1, 1},
	}, 2,					/* THOPMODE 3, RHOPMODE 1 */
	},
	[3][2] = {{
		/* not implemented for now */
		{{0}, {0}, 1, 1, 2},
		{{0}, {1}, 1, 1, 1},
		{{1}, {1}, 1, 1, 1},
	}, 3,					/* THOPMODE 3, RHOPMODE 2 */
	},
	[3][3] = {{
		{{0}, {0}, 1, 1, 3},
		{{1}, {1}, 1, 1, 1},
	}, 2,					/* THOPMODE 3, RHOPMODE 3 */
	},
	[3][4] = {{
		{{0, 1}, {0}, 2, 1, 4},
	}, 1,					/* THOPMODE 3, RHOPMODE 4 */
	},
	[4][0] = {{
		{{0}, {0, 1, 2, 3}, 1, 4, 4},
	}, 1,					/* THOPMODE 4, RHOPMODE 0 */
	},
	[4][1] = {{
		{{0}, {0, 1, 2}, 1, 3, 4},
	}, 1,					/* THOPMODE 4, RHOPMODE 1 */
	},
	[4][2] = {{
		{{0}, {0, 1}, 1, 2, 4},
	}, 1,					/* THOPMODE 4, RHOPMODE 2 */
	},
	[4][3] = {{
		{{0}, {0, 1}, 1, 2, 4},
	}, 1,					/* THOPMODE 4, RHOPMODE 3 */
	},
	[4][4] = {{
		{{0}, {0}, 1, 1, 4},
	}, 1,					/* THOPMODE 4, RHOPMODE 4 */
	},
};

struct mchp_asrc_be_trigger;

/* runtime structure to dynamically associate a trigger wth a BE DAI */
struct mchp_asrc_be_rtm {
	struct mchp_asrc_be_trigger	*trig;	/* backpointer */
	struct snd_pcm_hw_params	*hw_params;	// used by the links that are not BE
	struct mchp_asrc_slot const	*slot;
	struct snd_soc_pcm_runtime	*dpcm_be;
	struct list_head		list;
};

/* associated with one HW BE audio IP */
struct mchp_asrc_be_trigger {
	struct mchp_asrc_be_rtm		*rtm[SNDRV_PCM_STREAM_LAST + 1];
	struct mchp_asrc_dev		*priv;				/* backpointer */
	struct of_phandle_args		*phandle;
	u32				idx;
	u32				dp_tx_idx;
	u32				dp_rx_idx;
};

struct mchp_asrc_dsp_priv {
	struct mchp_asrc_dmaengine_dai_dma	*dma;
};

struct mchp_asrc_pcm_priv {
	struct mchp_asrc_dmaengine_dai_dma	dma;
	struct list_head			list_head_in;
	struct list_head			list_head_out;
	struct xmit_ch				*tc;
	struct mchp_asrc_be_rtm			rtm_fe;		/* used for FE --- BEs links */
	int					refcount;
	int					maxburst;
	bool					is_hostless;
};

struct mchp_asrc_dev {
	struct mchp_asrc_dsp_priv		dsp[MCHP_ASRC_NB_STEREO_CH];
	struct mchp_asrc_pcm_priv		*pcm[MCHP_ASRC_NB_STEREO_CH];
	struct device				*dev;
	struct regmap				*regmap;
	struct clk				*pclk;
	struct clk				*gclk;
	struct resource				*mem;
	struct mchp_asrc_be_trigger		*trig;
	int					trig_count;
	int					thr_opmode;
	int					rhr_opmode;
	int					direct_path;
};

static inline int mchp_asrc_period_to_burst(int period_size, int sample_size)
{
	if (!(period_size % (sample_size * 16)))
		return 16;

	if (!(period_size % (sample_size * 8)))
		return 8;

	if (!(period_size % (sample_size * 4)))
		return 4;

	if (!(period_size % (sample_size * 2)))
		return 2;

	return 1;
}

static inline int mchp_asrc_burst_to_chunk(int dsp, int burst)
{
	switch (burst) {
	case 16:
		return MCHP_ASRC_CH_CONF_CHUNK_16_DATA(dsp);
	case 8:
		return MCHP_ASRC_CH_CONF_CHUNK_8_DATA(dsp);
	case 4:
		return MCHP_ASRC_CH_CONF_CHUNK_4_DATA(dsp);
	case 2:
		return MCHP_ASRC_CH_CONF_CHUNK_2_DATA(dsp);
	default:
		return MCHP_ASRC_CH_CONF_CHUNK_1_DATA(dsp);
	}
}

/* rule to limit the number of channels based on available DSPs and available BEs */
static int mchp_asrc_ch_rule(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct mchp_asrc_dev *priv = rule->private;
	struct snd_interval t;
	int dsps_running;
	int max_dsps_avail;
	u32 mr;

	regmap_read(priv->regmap, MCHP_ASRC_MR, &mr);
	dsps_running = hweight32(mr & MCHP_ASRC_MR_ASRCEN_MASK);

	/* there are no limitatins if there are no running DSPs */
	if (!dsps_running)
		return 0;

	/* with sr over 96000, only 2 DSPs can be used */
	if (mr & MCHP_ASRC_MR_GT96K)
		max_dsps_avail = 2;
	else
		max_dsps_avail = MCHP_ASRC_NB_STEREO_CH;

	if (max_dsps_avail <= dsps_running)
		return -EBUSY;

	snd_interval_any(&t);
	t.max = (max_dsps_avail - dsps_running) * 2;

	return snd_interval_refine(hw_param_interval(params, rule->var), &t);
}

/* rule to never have a sampling rate greater than 96kHz if more than 2 DSPs are used */
static int mchp_asrc_ch_rate_rule(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_interval t;
	struct mchp_asrc_dev *priv = rule->private;
	unsigned int gclk_max_rate;
	unsigned int fs = params_rate(params);
	int dsps_running;
	int dsps_req = (params_channels(params) + 1) / 2;
	int gt96k;
	u32 mr;

	snd_interval_any(&t);

	/* GCLK can't be 4 times higher than PCLK */
	gclk_max_rate = clk_get_rate(priv->pclk) * 4 - 1;

	regmap_read(priv->regmap, MCHP_ASRC_MR, &mr);
	dsps_running = hweight32(mr & MCHP_ASRC_MR_ASRCEN_MASK);

	if (!dsps_running) {
		if (fs > 96000)
			t.max = gclk_max_rate / MCHP_ASRC_GT96K_RATIO_MIN;
		else
			t.max = min(gclk_max_rate / MCHP_ASRC_LT96K_RATIO_MIN, 96000U);
		return snd_interval_refine(hw_param_interval(params, rule->var), &t);
	}

	/* some DSPs are running so GT96K can't be changed */
	gt96k = !!(mr & MCHP_ASRC_MR_GT96K);
	if (dsps_running + dsps_req <= 2) {
		if (gt96k)
			t.max = gclk_max_rate / MCHP_ASRC_GT96K_RATIO_MIN;
		else
			t.max = min(gclk_max_rate / MCHP_ASRC_LT96K_RATIO_MIN, 96000U);
		return snd_interval_refine(hw_param_interval(params, rule->var), &t);
	}

	/* if GT96K bit is set, only 2 DSPs can run */
	if (dsps_running + dsps_req > 2 && gt96k)
		return -EBUSY;

	t.max = min(gclk_max_rate / MCHP_ASRC_LT96K_RATIO_MIN, 96000U);
	return snd_interval_refine(hw_param_interval(params, rule->var), &t);
}

static int mchp_asrc_startup(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mchp_asrc_dev *priv = snd_soc_dai_get_drvdata(dai);
	struct snd_soc_dpcm *dpcm;
	int dsps_running;
	int ret;
	u32 mr;
	int i;

	regmap_read(priv->regmap, MCHP_ASRC_MR, &mr);
	dsps_running = hweight32(mr & MCHP_ASRC_MR_ASRCEN_MASK);

	/*
	 * if there are already 2 DSPs running at a sampling rate higher than
	 * 96kHz, we can not use the rest of the DSPs
	 */
	if (dsps_running >= 2 && (mr & MCHP_ASRC_MR_GT96K))
		return -EBUSY;

	ret = snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
				  mchp_asrc_ch_rule, priv, SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	if (ret < 0) {
		dev_err(rtd->dev, "failed to add rule for no of channels: %d", ret);
		return ret;
	}

	ret = snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
				  mchp_asrc_ch_rate_rule, priv, SNDRV_PCM_HW_PARAM_CHANNELS,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (ret < 0) {
		dev_err(rtd->dev, "failed to add rule for sampling rate: %d", ret);
		return ret;
	}

	for_each_dpcm_be(rtd, substream->stream, dpcm) {
		struct snd_soc_pcm_runtime *be = dpcm->be;
		struct snd_soc_dai *dai_be = snd_soc_rtd_to_cpu(be, 0);
		struct snd_pcm_substream *substream_be;

		if (dpcm->fe != rtd)
			continue;

		substream_be = snd_soc_dpcm_get_substream(be, substream->stream);
		if (!substream_be)
			continue;

		dev_dbg(rtd->dev, "%s() Found BE DAI %s\n", __func__, dai_be->name);

		for (i = 0; i < priv->trig_count; i++) {
			struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];
			struct mchp_asrc_be_rtm *rtm_be;

			if (!trig_be->phandle || trig_be->phandle->np != dai_be->dev->of_node)
				continue;

			dev_dbg(priv->dev, "BE %s found in triger list with id %d\n",
				dai_be->name, trig_be->idx);

			if (trig_be->rtm[substream->stream]) {
				dev_err(priv->dev, "interface %s already running\n",
					trig_be->phandle->np->full_name);
				ret = -EBUSY;
				goto __cleanup_rtm;
			}

			rtm_be = kzalloc(sizeof(*rtm_be), GFP_KERNEL);
			if (!rtm_be) {
				ret = -ENOMEM;
				goto __cleanup_rtm;
			}

			rtm_be->trig = trig_be;
			trig_be->rtm[substream->stream] = rtm_be;
			break;
		}
		if (i == priv->trig_count) {
			dev_warn(rtd->dev, "Trigger not found for BE DAI %s\n", dai_be->name);
			continue;
		}
	}

	return 0;

__cleanup_rtm:
	for_each_dpcm_be_rollback(rtd, substream->stream, dpcm) {
		struct snd_soc_pcm_runtime *be = dpcm->be;
		struct snd_soc_dai *dai_be = snd_soc_rtd_to_cpu(be, 0);
		struct snd_pcm_substream *substream_be;
		int i;

		if (dpcm->fe != rtd)
			continue;

		substream_be = snd_soc_dpcm_get_substream(be, substream->stream);
		if (!substream_be)
			continue;

		for (i = 0; i < priv->trig_count; i++) {
			struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];

			if (!trig_be->phandle || trig_be->phandle->np != dai_be->dev->of_node)
				continue;

			if (!trig_be->rtm[substream->stream])
				break;

			kfree(trig_be->rtm[substream->stream]);
			trig_be->rtm[substream->stream] = NULL;
		}
	}

	return ret;
}

static void mchp_asrc_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mchp_asrc_dev *priv = snd_soc_dai_get_drvdata(dai);
	struct snd_soc_dpcm *dpcm;
	int i;

	for_each_dpcm_be(rtd, substream->stream, dpcm) {
		struct snd_soc_pcm_runtime *be = dpcm->be;
		struct snd_soc_dai *dai_be = snd_soc_rtd_to_cpu(be, 0);
		struct snd_pcm_substream *substream_be;

		if (dpcm->fe != rtd)
			continue;

		substream_be = snd_soc_dpcm_get_substream(be, substream->stream);
		if (!substream_be)
			continue;

		for (i = 0; i < priv->trig_count; i++) {
			struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];

			if (!trig_be->phandle || trig_be->phandle->np != dai_be->dev->of_node)
				continue;

			kfree(trig_be->rtm[substream->stream]);
			trig_be->rtm[substream->stream] = NULL;
			break;
		}
	}
}

static u32 mchp_asrc_vbps_to_reg(int vbps, int block)
{
	switch (vbps) {
	case 8:
		return MCHP_ASRC_VBPS_8_BIT(block);
	case 10:
		return MCHP_ASRC_VBPS_10_BIT(block);
	case 12:
		return MCHP_ASRC_VBPS_12_BIT(block);
	case 14:
		return MCHP_ASRC_VBPS_14_BIT(block);
	case 16:
		return MCHP_ASRC_VBPS_16_BIT(block);
	case 18:
		return MCHP_ASRC_VBPS_18_BIT(block);
	case 20:
		return MCHP_ASRC_VBPS_20_BIT(block);
	case 24:
		return MCHP_ASRC_VBPS_24_BIT(block);
	case 32:
		return MCHP_ASRC_VBPS_32_BIT(block);
	default:
		return 0;
	}
}

static int mchp_asrc_bes_get(struct mchp_asrc_dev *priv, struct snd_pcm_substream *substream,
			     struct list_head *head, bool set_trig)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dpcm *dpcm;
	int i;

	for_each_dpcm_be(rtd, substream->stream, dpcm) {
		struct snd_soc_pcm_runtime *be = dpcm->be;
		struct snd_soc_dai *dai = snd_soc_rtd_to_cpu(be, 0);
		struct snd_pcm_substream *substream_be;

		if (dpcm->fe != rtd)
			continue;

		substream_be = snd_soc_dpcm_get_substream(be, substream->stream);
		if (!substream_be)
			continue;

		dev_dbg(rtd->dev,
			"%s() BE DAI %s: (dpcm_params) rate=%u format=%#x width=%u channels=%u\n",
			__func__, dai->name,
			params_rate(&be->dpcm[substream->stream].hw_params),
			params_format(&be->dpcm[substream->stream].hw_params),
			params_width(&be->dpcm[substream->stream].hw_params),
			params_channels(&be->dpcm[substream->stream].hw_params));

		for (i = 0; i < priv->trig_count; i++) {
			struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];
			struct mchp_asrc_be_rtm *rtm_be;

			if (!trig_be->phandle || trig_be->phandle->np != dai->dev->of_node)
				continue;

			dev_dbg(priv->dev, "BE found in triger list with id %d\n",
				trig_be->idx);

			rtm_be = trig_be->rtm[substream->stream];
			if (set_trig)
				rtm_be->trig = trig_be;

			rtm_be->hw_params = &be->dpcm[substream->stream].hw_params;
			rtm_be->dpcm_be = be;

			/* keep the order */
			list_add_tail(&rtm_be->list, head);
			break;
		}

		if (i == priv->trig_count) {
			dev_err(rtd->dev, "no BE trigger found for DAI %s\n", dai->name);
			return -EINVAL;
		}
	}

	return 0;
}

static int mchp_asrc_validate_bes(struct mchp_asrc_dev *priv, struct list_head *head, int channels)
{
	struct mchp_asrc_be_rtm *rtm_be;
	int dsps_req = (channels + 1) / 2;
	int ch_be_sum = 0;
	int dsps_avail;
	int dsps_prev_be = MCHP_ASRC_NB_STEREO_CH;
	u32 reg;

	/* get the number of free DSPs */
	regmap_read(priv->regmap, MCHP_ASRC_MR, &reg);
	dsps_avail = MCHP_ASRC_NB_STEREO_CH - hweight32(reg & MCHP_ASRC_MR_ASRCEN_MASK);

	if (dsps_req && dsps_avail < dsps_req) {
		dev_err(priv->dev, "Not enough DSPs available (%d) to play %d audio channel(s)\n",
			dsps_avail, channels);
		return -EBUSY;
	}

	/* the number of DSPs needed for each BE needs to be in a descending order */
	list_for_each_entry(rtm_be, head, list) {
		int ch_be = params_channels(rtm_be->hw_params);

		if ((ch_be + 1) / 2 > dsps_prev_be) {
			dev_err(priv->dev,
				"the nr of DSPs needed for each BE is not in a descending order\n");
			return -EINVAL;
		}
		ch_be_sum += ch_be;
		dsps_prev_be = (ch_be + 1) / 2;
	}

	/*
	 * the number of channels on FE needs to be the same with
	 * the one used on all BEs
	 */
	if (channels && channels != ch_be_sum) {
		dev_err(priv->dev, "FE uses %d channels, BEs uses %d channels\n",
			channels, ch_be_sum);
		return -EINVAL;
	}

	return ch_be_sum;
}

static int mchp_asrc_is_tc_valid(struct mchp_asrc_pcm_priv *pcm, struct xmit_ch *tc,
				 int single_aif_is_in, int thr_opmode, int rhr_opmode,
				 int *aif, int aif_nr)
{
	struct opmode_to_slot const *be_opm;
	int *be_chan;
	int fe_chan_count, be_chan_count;
	int i;

	/* for now we support only one FE */
	/* one-to-many scenario */
	if (single_aif_is_in) {
		be_chan = tc->rhr_ch;
		fe_chan_count = tc->thr_ch_count;
		be_chan_count = tc->rhr_ch_count;
		be_opm = &asrc_map[rhr_opmode];
	} else {
	/* many-to-one scenario */
		be_chan = tc->thr_ch;
		fe_chan_count = tc->rhr_ch_count;
		be_chan_count = tc->thr_ch_count;
		be_opm = &asrc_map[thr_opmode];
	}

	if (fe_chan_count != 1 || aif_nr != be_chan_count)
		return -EINVAL;

	for (i = 0; i < aif_nr; i++) {
		int dsps_avail = be_opm->slot[be_chan[i]].dsp_count;

		if (dsps_avail != aif[i])
			return -EINVAL;
	}

	tc->in_use = true;

	return 0;
}

static struct xmit_ch *
mchp_asrc_find_tc(struct mchp_asrc_pcm_priv *pcm, int dsps_req, int single_aif_is_in,
		  int thr_opmode, int rhr_opmode, int *aif, int aif_nr)
{
	struct opmode *current_conf = &ch_conf[thr_opmode][rhr_opmode];
	int i;
	int ret;

	for (i = 0; i < current_conf->tc_count; i++) {
		struct xmit_ch *tc = &current_conf->tc[i];

		if (tc->in_use || dsps_req != tc->dsps_used)
			continue;

		/* the first slot is used both as FE and BE */
		ret = mchp_asrc_is_tc_valid(pcm, tc, single_aif_is_in, thr_opmode, rhr_opmode,
					    aif, aif_nr);
		if (!ret)
			return tc;
	}

	return NULL;
}

static int mchp_asrc_tc_get(struct mchp_asrc_dev *priv, struct snd_soc_dai *dai)
{
	int aif_in[MCHP_ASRC_NB_STEREO_CH] = {0};
	int aif_out[MCHP_ASRC_NB_STEREO_CH] = {0};
	int aif_in_nr = 0;
	int aif_out_nr = 0;
	struct mchp_asrc_be_rtm *rtm_be;
	int dsps_in = 0, dsps_out = 0;
	struct xmit_ch *tc;
	int thr, rhr;
	int *aif, aif_nr;
	bool single_aif_is_in;
	struct mchp_asrc_pcm_priv *pcm = priv->pcm[dai->id];
	int i;

	/* both OPMODES must be set, or none */
	if (priv->thr_opmode * priv->rhr_opmode < 0)
		return -EINVAL;

	/* get no of DSPs needed for each IN and OUT AIFs */
	list_for_each_entry(rtm_be, &pcm->list_head_in, list) {
		int dsps = (params_channels(rtm_be->hw_params) + 1) / 2;

		aif_in[aif_in_nr++] = dsps;

		dev_dbg(priv->dev, "AIF %s needs %d DSPs, index %d\n",
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : "front-end",
			dsps, aif_in_nr - 1);
	}
	list_for_each_entry(rtm_be, &pcm->list_head_out, list) {
		int dsps = (params_channels(rtm_be->hw_params) + 1) / 2;

		aif_out[aif_out_nr++] = dsps;

		dev_dbg(priv->dev, "AIF %s needs %d DSPs, index %d\n",
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : "front-end",
			dsps, aif_out_nr - 1);
	}

	if (aif_in_nr == 1) {
		single_aif_is_in = true;
		aif = aif_out;
		aif_nr = aif_out_nr;
	} else if (aif_out_nr == 1) {
		single_aif_is_in = false;
		aif = aif_in;
		aif_nr = aif_in_nr;
	} else {
		dev_err(priv->dev,
			"PCM %s (de)mux %d -> %d AIFs; do not use many-to-many\n",
			dai->name, aif_in_nr, aif_out_nr);
		return -EINVAL;
	}

	for (i = 0; i < aif_in_nr; i++)
		dsps_in += aif_in[i];
	for (i = 0; i < aif_out_nr; i++)
		dsps_out += aif_out[i];
	if (dsps_in != dsps_out) {
		dev_err(priv->dev, "%d -> %d DSPs; number needs to match\n",
			dsps_in, dsps_out);
		return -EINVAL;
	}

	/* the OPMODEs are already set, check if there are free transmit channels */
	if (priv->thr_opmode != -1) {
		thr = priv->thr_opmode;
		rhr = priv->rhr_opmode;
		tc = mchp_asrc_find_tc(pcm, dsps_in, single_aif_is_in, thr, rhr,
				       aif, aif_nr);
		if (tc)
			goto __found_tc;

		/* already selected opmode is not ok for FE */
		dev_err(priv->dev, "no compatible transmit channels left in THR/RHR opmode %d/%d\n",
			priv->thr_opmode, priv->rhr_opmode);
		return -EBUSY;
	}
	for (thr = 0; thr < MCHP_ASRC_OPMODE_MAX; thr++) {
		for (rhr = 0; rhr < MCHP_ASRC_OPMODE_MAX; rhr++) {
			tc = mchp_asrc_find_tc(pcm, dsps_in, single_aif_is_in, thr,
					       rhr, aif, aif_nr);
			if (tc)
				goto __found_tc_init;
		}
	}

	return -EINVAL;

__found_tc_init:
	dev_dbg(priv->dev, "%s() opmodes THR %d RHR %d\n", __func__,
		thr, rhr);
	priv->thr_opmode = thr;
	priv->rhr_opmode = rhr;
	regmap_update_bits(priv->regmap, MCHP_ASRC_CH_CONF,
			   MCHP_ASRC_CH_CONF_THROPMODE_MASK |
			   MCHP_ASRC_CH_CONF_RHROPMODE_MASK,
			   MCHP_ASRC_CH_CONF_THROPMODE(thr) |
			   MCHP_ASRC_CH_CONF_RHROPMODE(rhr));
__found_tc:
#ifdef DEBUG
	dev_dbg(priv->dev, "%s() transmit channels:\n", __func__);
	for (i = 0; i < tc->thr_ch_count; i++)
		dev_dbg(priv->dev, "%s() %d, ", __func__, tc->thr_ch[i]);
	dev_dbg(priv->dev, "%s() receive channels:\n", __func__);
	for (i = 0; i < tc->rhr_ch_count; i++)
		dev_dbg(priv->dev, "%s() %d, ", __func__, tc->rhr_ch[i]);
#endif
	/* select one FE slot, the first one */
	pcm->tc = tc;
	return 0;
}

static void mchp_asrc_tc_free(struct mchp_asrc_dev *priv, struct mchp_asrc_pcm_priv *pcm)
{
	struct opmode *op;
	int i;

	pcm->tc->in_use = false;

	op = &ch_conf[priv->thr_opmode][priv->rhr_opmode];
	for (i = 0; i < op->tc_count; i++) {
		/* do not clear OPMODES if there are running PCMs */
		if (op->tc[i].in_use)
			return;
	}

	priv->thr_opmode = -1;
	priv->rhr_opmode = -1;
	regmap_update_bits(priv->regmap, MCHP_ASRC_CH_CONF,
			   MCHP_ASRC_CH_CONF_THROPMODE_MASK | MCHP_ASRC_CH_CONF_RHROPMODE_MASK, 0);
}

/* apply HW DMA Chunk Size limitations */
static void mchp_asrc_maxburst_limit(int thr_opmode, int rhr_opmode, bool is_mono,
				     int *maxburst)
{
	if ((thr_opmode == 0 || rhr_opmode == 0) &&
	    *maxburst > 4) {
		if (is_mono)
			*maxburst = 4;
		else
			*maxburst = 8;
		return;
	}
	if ((thr_opmode != 4 || rhr_opmode != 4) && is_mono && *maxburst > 8)
		*maxburst = 8;
}

static int mchp_asrc_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct mchp_asrc_dev *priv = snd_soc_dai_get_drvdata(dai);
	struct mchp_asrc_dmaengine_dai_dma *dma;
	struct mchp_asrc_dmaengine_be_dma *dma_be, *tmp;
	struct mchp_asrc_pcm_priv *pcm;
	int width = params ? params_width(params) : 0;
	int src_width, dst_width;
	unsigned long rate;
	unsigned int fs = params ? params_rate(params) : 0;
	unsigned int max_sr = fs;
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u32 ratio;
	unsigned int channels = params ? params_channels(params) : 0;
	unsigned int dsps_req = (channels + 1) / 2;
	u32 ch_conf = 0;
	u32 ch_conf_mask = 0;
	u32 dapselr_mask = 0;
	u32 dapselr = 0;
	u32 mr_mask = MCHP_ASRC_MR_DATHRX_MASK | MCHP_ASRC_MR_DARHRX_MASK;
	u32 mr_val = 0;
	int ret;
	u32 mr;
	u32 trig = 0, trig_mask = 0;
	u32 vbps_in = 0, vbps_out = 0, vbps_mask = 0;
	struct mchp_asrc_be_rtm *rtm_be;
	int dsp;
	int dsps_running;
	int ratio_min;
	struct list_head *list_head_bes;
	int i;
	struct opmode_to_slot const *be_opm;
	struct xmit_ch *tc;
	bool is_mono = false;

	dev_dbg(priv->dev,"%s() DAI %s id %d rate=%u format=%#x width=%u channels=%u\n",
		        __func__,  dai->name, dai->id, fs,
			params ? params_format(params) : 0, width, channels);

	/* on a hostless link, pcm might be allocated previously */
	if (priv->pcm[dai->id]) {
		if (params) {
			dev_err(priv->dev, "PCM running on id %d\n", dai->id);
			return -EBUSY;
		}
		pcm = priv->pcm[dai->id];
		pcm->refcount++;
	} else {
		pcm = kzalloc(sizeof(*pcm), GFP_KERNEL);
		if (!pcm)
			return -ENOMEM;
		if (!params)
			pcm->is_hostless = true;

		INIT_LIST_HEAD(&pcm->list_head_in);
		INIT_LIST_HEAD(&pcm->list_head_out);
		INIT_LIST_HEAD(&pcm->dma.dma_in_list);
		INIT_LIST_HEAD(&pcm->dma.dma_out_list);
		pcm->refcount++;
		priv->pcm[dai->id] = pcm;
	}
	dma = &pcm->dma;

	if (is_playback) {
		/* for playback, this callback is for the OUT BEs */
		list_head_bes = &pcm->list_head_out;
	} else {
		/* for capture, this callback is for the IN BEs */
		list_head_bes = &pcm->list_head_in;
	}

	INIT_LIST_HEAD(list_head_bes);

	ret = mchp_asrc_bes_get(priv, substream, list_head_bes, true);
	if (ret < 0)
		goto __cleanup_free_pcm;

	dev_dbg(priv->dev, "%s() BE triggers:\n", __func__);
#ifdef DEBUG
	list_for_each_entry(rtm_be, list_head_bes, list) {
		struct mchp_asrc_be_trigger *trig_be = rtm_be->trig;

		dev_dbg(priv->dev, "%s, index %d\n",
				trig_be->phandle->np->full_name, trig_be->idx);
	}
#endif

	ret = mchp_asrc_validate_bes(priv, list_head_bes, channels);
	if (ret < 0)
		goto __cleanup_bes;

	if (!pcm->is_hostless) {
		pcm->rtm_fe.hw_params = params;
		pcm->rtm_fe.trig = NULL;

		/* keep the order */
		if (is_playback)
			list_add_tail(&pcm->rtm_fe.list, &pcm->list_head_in);
		else
			list_add_tail(&pcm->rtm_fe.list, &pcm->list_head_out);
	}

	if (list_empty(&pcm->list_head_in) || list_empty(&pcm->list_head_out)) {
		/* incomplete BE-to-BE link; return and wait for the BEs on the other stream */
		dev_dbg(priv->dev, "%s() first half for %s finished\n", __func__,
			is_playback ? "playback" : "capture");
		goto __skip_conf;
	}

	ret = mchp_asrc_tc_get(priv, dai);
	if (ret < 0)
		goto __cleanup_bes;

	tc = pcm->tc;

	/* check if DSPs needed by this TC are free */
	for (i = 0; i < tc->thr_ch_count; i++) {
		struct mchp_asrc_slot const *slot = &asrc_map[priv->thr_opmode].slot[tc->thr_ch[i]];
		int j;

		for (j = slot->first_dsp; j < slot->first_dsp + slot->dsp_count; j++) {
			if (priv->dsp[j].dma) {
				dev_err(priv->dev, "DSP %d already in use\n", j);
				ret = -EBUSY;
				goto __cleanup_free_tc;
			}
		}
	}

	/* reserve DSPs */
	for (i = 0; i < tc->thr_ch_count; i++) {
		struct mchp_asrc_slot const *slot = &asrc_map[priv->thr_opmode].slot[tc->thr_ch[i]];
		int j;

		for (j = slot->first_dsp; j < slot->first_dsp + slot->dsp_count; j++)
			priv->dsp[j].dma = dma;
	}

	/* assign in and out slots for AIFs */
	be_opm = &asrc_map[priv->rhr_opmode];
	i = 0;
	list_for_each_entry(rtm_be, &pcm->list_head_out, list) {
		dev_dbg(priv->dev, "%s(): AIF %s slot id %d\n", __func__,
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : dai->name,
		tc->rhr_ch[i]);
		rtm_be->slot = &be_opm->slot[tc->rhr_ch[i++]];
	}

	be_opm = &asrc_map[priv->thr_opmode];
	i = 0;
	list_for_each_entry(rtm_be, &pcm->list_head_in, list) {
		dev_dbg(priv->dev, "%s(): AIF %s slot id %d\n", __func__,
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : dai->name,
			tc->thr_ch[i]);
		rtm_be->slot = &be_opm->slot[tc->thr_ch[i++]];
	}

	/* set mono DSPs, trigger index and valid bits */
	list_for_each_entry(rtm_be, &pcm->list_head_in, list) {
		struct mchp_asrc_be_trigger *trig_be = rtm_be->trig;
		struct mchp_asrc_slot const *slot_be = rtm_be->slot;
		int chan_be = params_channels(rtm_be->hw_params);
		u32 idx = trig_be ? trig_be->idx : 0;
		int physical_width = params_physical_width(rtm_be->hw_params) / 8;
		int period_bytes = params_period_size(rtm_be->hw_params) *
			params_channels(rtm_be->hw_params) * physical_width;
		int maxburst = mchp_asrc_period_to_burst(period_bytes, physical_width);

		/* treat odd streams, set only last DSP as mono */
		if (chan_be % 2) {
			dsp = slot_be->first_dsp + slot_be->dsp_count - 1;
			ch_conf |= MCHP_ASRC_CH_CONF_MONO(dsp);
			ch_conf_mask |= MCHP_ASRC_CH_CONF_MONO(dsp);
			is_mono = true;
		}

		for (dsp = slot_be->first_dsp;
		     dsp < slot_be->first_dsp + slot_be->dsp_count; dsp++) {
			/* Configure input direct path. */
			if (priv->direct_path && rtm_be && rtm_be->dpcm_be &&
				rtm_be->dpcm_be->dai_link->no_pcm) {
					dapselr_mask |= MCHP_ASRC_DAPSELR_IN_CH_MASK(dsp);
					dapselr |= MCHP_ASRC_DAPSELR_IN_CH(dsp, trig_be->dp_rx_idx);
					mr_val |= MCHP_ASRC_MR_DATHRX(dsp);
			}

			trig_mask |= MCHP_ASRC_TRIG_TRIGSELIN_MASK(dsp);
			trig |= MCHP_ASRC_TRIG_TRIGSELIN(idx, dsp);
		}

		src_width = params_width(rtm_be->hw_params);
		for (dsp = slot_be->first_dsp; dsp < slot_be->first_dsp + slot_be->dsp_count;
		     dsp++) {
			vbps_mask |= MCHP_ASRC_VBPS_MASK(dsp);
			vbps_in |= mchp_asrc_vbps_to_reg(src_width, dsp);
		}

		if (max_sr < params_rate(rtm_be->hw_params))
			max_sr = params_rate(rtm_be->hw_params);

		if (pcm->maxburst)
			pcm->maxburst = min(pcm->maxburst, maxburst);
		else
			pcm->maxburst = maxburst;

		dev_dbg(priv->dev, "%s: period bytes %d, width %d -> maxburst %d\n",
			trig_be ? trig_be->phandle->np->full_name : dai->name,
			period_bytes, physical_width, maxburst);
	}
	regmap_update_bits(priv->regmap, MCHP_ASRC_DAPSELR, dapselr_mask, dapselr);

	i = 0;
	list_for_each_entry(rtm_be, &pcm->list_head_out, list) {
		struct mchp_asrc_be_trigger *trig_be = rtm_be->trig;
		struct mchp_asrc_slot const *slot_be = rtm_be->slot;
		int chan_be = params_channels(rtm_be->hw_params);
		int physical_width = params_physical_width(rtm_be->hw_params) / 8;
		int period_bytes = params_period_size(rtm_be->hw_params) *
			params_channels(rtm_be->hw_params) * physical_width;
		int maxburst = mchp_asrc_period_to_burst(period_bytes, physical_width);
		u32 idx = trig_be ? trig_be->idx : 0;

		/* Configure output direct path. */
		if (priv->direct_path && rtm_be && rtm_be->dpcm_be &&
		    rtm_be->dpcm_be->dai_link->no_pcm) {

			dapselr_mask |= MCHP_ASRC_DAPSELR_OUT_CH_MASK(i);
			dapselr |= MCHP_ASRC_DAPSELR_OUT_CH(i, trig_be->dp_tx_idx);
			mr_val |= MCHP_ASRC_MR_DARHRX(i);

		}

		/* treat odd streams, set only last DSP as mono */
		if (chan_be % 2) {
			dsp = slot_be->first_dsp + slot_be->dsp_count - 1;
			ch_conf |= MCHP_ASRC_CH_CONF_MONO(dsp);
			ch_conf_mask |= MCHP_ASRC_CH_CONF_MONO(dsp);
			is_mono = true;
		}

		for (dsp = slot_be->first_dsp;
		     dsp < slot_be->first_dsp + slot_be->dsp_count; dsp++) {
			trig_mask |= MCHP_ASRC_TRIG_TRIGSELOUT_MASK(dsp);
			trig |= MCHP_ASRC_TRIG_TRIGSELOUT(idx, dsp);
		}

		dst_width = params_width(rtm_be->hw_params);
		for (dsp = slot_be->first_dsp; dsp < slot_be->first_dsp + slot_be->dsp_count;
		     dsp++) {
			vbps_mask |= MCHP_ASRC_VBPS_MASK(dsp);
			vbps_out |= mchp_asrc_vbps_to_reg(dst_width, dsp);
		}

		if (max_sr < params_rate(rtm_be->hw_params))
			max_sr = params_rate(rtm_be->hw_params);

		if (pcm->maxburst)
			pcm->maxburst = min(pcm->maxburst, maxburst);
		else
			pcm->maxburst = maxburst;

		dev_dbg(priv->dev, "%s: period bytes %d, width %d -> maxburst %d\n",
			trig_be ? trig_be->phandle->np->full_name : dai->name,
			period_bytes, physical_width, maxburst);
		i++;
	}

	mchp_asrc_maxburst_limit(priv->thr_opmode, priv->rhr_opmode, is_mono, &pcm->maxburst);
	dev_dbg(priv->dev, "maxburst value to be used by all DSPs: %d\n", pcm->maxburst);

	/* set DMAengine data for each IN/OUT AIF */
	INIT_LIST_HEAD(&dma->dma_in_list);
	list_for_each_entry(rtm_be, &pcm->list_head_in, list) {
		struct mchp_asrc_be_trigger *trig_be = rtm_be->trig;
		struct mchp_asrc_slot const *slot_be = rtm_be->slot;
		struct snd_dmaengine_dai_dma_data *dma_data_be;

		dma_be = kzalloc(sizeof(*dma_be), GFP_KERNEL);
		if (!dma_be) {
			ret = -ENOMEM;
			goto __cleanup_dma_bes;
		}

		dma_be->phandle = trig_be ? trig_be->phandle : NULL;
		dma_data_be = &dma_be->dma_data;
		dma_data_be->addr = (dma_addr_t)priv->mem->start + MCHP_ASRC_THR(slot_be->slot_id);
		dma_data_be->chan_name = kasprintf(GFP_KERNEL, "tx%d", slot_be->slot_id);
		if (!dma_data_be->chan_name) {
			ret = -ENOMEM;
			goto __cleanup_dma_bes;
		}
		dma_data_be->maxburst = pcm->maxburst;

		/* keep the order of the BEs */
		list_add_tail(&dma_be->list, &dma->dma_in_list);

		dev_dbg(priv->dev,
			"%s() IN AIF %s: DMA addr %pad DMA chan %s DMA maxburst %d\n",
			__func__, trig_be ? trig_be->phandle->np->full_name : dai->name,
			&dma_data_be->addr, dma_data_be->chan_name, dma_data_be->maxburst);
	}

	INIT_LIST_HEAD(&dma->dma_out_list);
	list_for_each_entry(rtm_be, &pcm->list_head_out, list) {
		struct mchp_asrc_be_trigger *trig_be = rtm_be->trig;
		struct mchp_asrc_slot const *slot_be = rtm_be->slot;
		struct snd_dmaengine_dai_dma_data *dma_data_be;
		int dsp;

		dma_be = kzalloc(sizeof(*dma_be), GFP_KERNEL);
		if (!dma_be) {
			ret = -ENOMEM;
			goto __cleanup_dma_bes;
		}

		dma_be->phandle = trig_be ? trig_be->phandle : NULL;
		dma_data_be = &dma_be->dma_data;
		dma_data_be->addr = (dma_addr_t)priv->mem->start + MCHP_ASRC_RHR(slot_be->slot_id);
		dma_data_be->chan_name = kasprintf(GFP_KERNEL, "rx%d", slot_be->slot_id);
		if (!dma_data_be->chan_name) {
			ret = -ENOMEM;
			goto __cleanup_dma_bes;
		}
		dma_data_be->maxburst = pcm->maxburst;

		 /* needed only once, since the same DPSs are used for IN and OUT AIFs */
		for (dsp = slot_be->first_dsp; dsp < slot_be->first_dsp + slot_be->dsp_count;
		     dsp++) {
			ch_conf |= mchp_asrc_burst_to_chunk(dsp,
							    pcm->maxburst);
			ch_conf_mask |= MCHP_ASRC_CH_CONF_CHUNK_MASK(dsp);
		}

		/* keep the order of the BEs */
		list_add_tail(&dma_be->list, &dma->dma_out_list);

		dev_dbg(priv->dev,
			"%s() OUT AIF %s: DMA addr %pad DMA chan %s DMA maxburst %d\n",
			__func__, trig_be ? trig_be->phandle->np->full_name : dai->name,
			&dma_data_be->addr, dma_data_be->chan_name, dma_data_be->maxburst);
	}

	/* get the number of free DSPs */
	regmap_read(priv->regmap, MCHP_ASRC_MR, &mr);
	dsps_running = hweight32(mr & MCHP_ASRC_MR_ASRCEN_MASK);

	/* GCLK could be set to a good value using assigned clocks or by a previous DPCM */
	rate = clk_get_rate(priv->gclk);
	if (!rate) {
		dev_err(priv->dev, "Unable to get rate for gclk\n");
		ret = -ENODEV;
		goto __cleanup_dma_bes;
	}
	dev_dbg(priv->dev, "Current gclk rate: %lu\n", rate);

	/* we assume GCLK is enabled if there are running DSPs */
	if (dsps_running) {
		if (mr & MCHP_ASRC_MR_GT96K)
			ratio_min = MCHP_ASRC_GT96K_RATIO_MIN;
		else
			ratio_min = MCHP_ASRC_LT96K_RATIO_MIN;
	} else {
		if (max_sr > 96000) {
			ratio_min = MCHP_ASRC_GT96K_RATIO_MIN;
			regmap_update_bits(priv->regmap, MCHP_ASRC_MR, MCHP_ASRC_MR_GT96K,
					   MCHP_ASRC_MR_GT96K);
		} else {
			ratio_min = MCHP_ASRC_LT96K_RATIO_MIN;
		}
	}
	if (!dsps_running && ((rate % max_sr) || ratio_min > (rate / max_sr))) {
		/* try to change GCLK if current value is not ok */
		unsigned long round_rate = (unsigned long)ratio_min * max_sr;
		unsigned long best_rate;
		unsigned long diff;

		for (rate = round_rate;
		     rate <= (unsigned long)ratio_min * MCHP_ASRC_RATIO_MAX;
		     rate += max_sr) {
			round_rate = clk_round_rate(priv->gclk, rate);
			if (diff > abs(round_rate - rate)) {
				diff = abs(round_rate - rate);
				best_rate = rate;
			}
			if (!diff)
				break;
		}

		dev_dbg(priv->dev, "%s() trying to set GCLK to %lu\n", __func__, best_rate);
		ret = clk_set_rate(priv->gclk, best_rate);
		if (ret) {
			dev_err(priv->dev, "unable to set GCLK rate: SR %u * ratio %lu\n",
				max_sr, (best_rate / max_sr));
			goto __cleanup_mr;
		}
	}

	/* get real GCLK rate, to set the correct ratio */
	rate = clk_get_rate(priv->gclk);
	if (!rate) {
		dev_err(priv->dev, "Unable to get rate for GCLK\n");
		ret = -ENODEV;
		goto __cleanup_mr;
	}
	dev_dbg(priv->dev, "Real GCLK rate: %lu\n", rate);

	ret = clk_prepare_enable(priv->gclk);
	if (ret < 0) {
		dev_err(priv->dev, "unable to enable GCLK: %d\n", ret);
		goto __cleanup_mr;
	}

	/* IN/OUT RATIO are not needed on a hostless PCM */
	if (pcm->is_hostless)
		goto __skip_inout;

	/* set IN/OUT RATIO for all DSPs in the FE slot */
	if (is_playback)
		list_head_bes = &pcm->list_head_in;
	else
		list_head_bes = &pcm->list_head_out;

	list_for_each_entry(rtm_be, list_head_bes, list) {
		struct mchp_asrc_slot const *slot_fe = rtm_be->slot;

		ratio = rate / params_rate(rtm_be->hw_params);
		for (dsp = slot_fe->first_dsp; dsp < slot_fe->first_dsp + slot_fe->dsp_count;
		     dsp++) {
			if (is_playback) {
				regmap_update_bits(priv->regmap, MCHP_ASRC_RATIO(dsp),
						   MCHP_ASRC_RATIO_INRATIO_MASK,
						   MCHP_ASRC_RATIO_INRATIO(ratio));
			} else {
				regmap_update_bits(priv->regmap, MCHP_ASRC_RATIO(dsp),
						   MCHP_ASRC_RATIO_OUTRATIO_MASK,
						   MCHP_ASRC_RATIO_OUTRATIO(ratio));
			}
		}
	}

__skip_inout:
	dev_dbg(priv->dev, "writing TRIG %#x, VBPS_IN %#x, VBPS_OUT %#x, CH_CONF %#x\n"
		"DAPSELR_MASK=%#x, DAPSELR=%#x\n",
		trig & trig_mask, vbps_in & vbps_mask, vbps_out & vbps_mask,
		ch_conf & ch_conf_mask,
		dapselr_mask, dapselr);

	regmap_update_bits(priv->regmap, MCHP_ASRC_TRIG, trig_mask, trig);
	regmap_update_bits(priv->regmap, MCHP_ASRC_VBPS_IN, vbps_mask, vbps_in);
	regmap_update_bits(priv->regmap, MCHP_ASRC_VBPS_OUT, vbps_mask, vbps_out);
	regmap_update_bits(priv->regmap, MCHP_ASRC_CH_CONF, ch_conf_mask, ch_conf);
	regmap_update_bits(priv->regmap, MCHP_ASRC_DAPSELR, dapselr_mask, dapselr);
	regmap_update_bits(priv->regmap, MCHP_ASRC_MR, mr_mask, mr_val);

	snd_soc_dai_init_dma_data(dai, dma, dma);
__skip_conf:

	list_for_each_entry(rtm_be, &pcm->list_head_in, list) {
		dev_dbg(priv->dev, "%s(): looking at IN %s\n", __func__,
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : "front-end");
	}
	list_for_each_entry(rtm_be, &pcm->list_head_out, list) {
		dev_dbg(priv->dev, "%s(): looking at OUT %s\n", __func__,
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : "front-end");
	}
	return 0;

__cleanup_mr:
	if (!dsps_running)
		regmap_update_bits(priv->regmap, MCHP_ASRC_MR, MCHP_ASRC_MR_GT96K, 0);
__cleanup_dma_bes:
	list_for_each_entry_safe(dma_be, tmp, &dma->dma_in_list, list) {
		list_del(&dma_be->list);
		kfree(dma_be->dma_data.chan_name);
		dma_be->dma_data.chan_name = NULL;
		kfree(dma_be);
	}
	list_for_each_entry_safe(dma_be, tmp, &dma->dma_out_list, list) {
		list_del(&dma_be->list);
		kfree(dma_be->dma_data.chan_name);
		dma_be->dma_data.chan_name = NULL;
		kfree(dma_be);
	}

	list_for_each_entry(rtm_be, &pcm->list_head_in, list)
		rtm_be->slot = NULL;
	list_for_each_entry(rtm_be, &pcm->list_head_out, list)
		rtm_be->slot = NULL;
	for (i = 0; i < pcm->tc->thr_ch_count; i++) {
		struct mchp_asrc_slot const *slot = &asrc_map[priv->thr_opmode].slot[tc->thr_ch[i]];
		int j;

		for (j = slot->first_dsp; j < slot->first_dsp + slot->dsp_count; j++)
			priv->dsp[j].dma = NULL;
	}
__cleanup_free_tc:
	mchp_asrc_tc_free(priv, pcm);
__cleanup_bes:
	if (is_playback)
		INIT_LIST_HEAD(&pcm->list_head_out);
	else
		INIT_LIST_HEAD(&pcm->list_head_in);
__cleanup_free_pcm:
	pcm->refcount--;
	if (!pcm->refcount) {
		kfree(pcm);
		priv->pcm[dai->id] = NULL;
	}

	return ret;
}

static int mchp_asrc_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct mchp_asrc_dev *priv = snd_soc_dai_get_drvdata(dai);
	struct mchp_asrc_dmaengine_be_dma *dma_be, *tmp;
	struct mchp_asrc_dmaengine_dai_dma *dma;
	struct mchp_asrc_be_rtm *rtm_be;
	struct xmit_ch *tc;
	struct list_head *list_head_fes;
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u32 val = 0;
	u32 mask = 0;
	u32 ch_conf_mask = 0;
	int dsp;
	struct mchp_asrc_pcm_priv *pcm = priv->pcm[dai->id];
	bool are_dsps_running;
	int i;

	/* nothing to do if no pcm is available */
	if (!pcm)
		return 0;

	regmap_read(priv->regmap, MCHP_ASRC_MR, &val);
	are_dsps_running = !!(val & MCHP_ASRC_MR_ASRCEN_MASK);

	if (!pcm->is_hostless) {
		if (is_playback)
			list_head_fes = &pcm->list_head_in;
		else
			list_head_fes = &pcm->list_head_out;

		list_for_each_entry(rtm_be, list_head_fes, list) {
			struct mchp_asrc_slot const *slot_fe = rtm_be->slot;

			for (dsp = slot_fe->first_dsp;
			     dsp < slot_fe->first_dsp + slot_fe->dsp_count; dsp++) {
				if (is_playback) {
					regmap_update_bits(priv->regmap, MCHP_ASRC_RATIO(dsp),
							   MCHP_ASRC_RATIO_INRATIO_MASK, 0);
				} else {
					regmap_update_bits(priv->regmap, MCHP_ASRC_RATIO(dsp),
							   MCHP_ASRC_RATIO_OUTRATIO_MASK, 0);
				}
			}
		}
	}

	if (pcm->refcount == 2 || !pcm->is_hostless) {
		/* unset DMAengine data */
		snd_soc_dai_init_dma_data(dai, NULL, NULL);

		clk_disable_unprepare(priv->gclk);

		/* cleanup BE channel names */
		dma = &pcm->dma;
		list_for_each_entry_safe(dma_be, tmp, &dma->dma_in_list, list) {
			struct snd_dmaengine_dai_dma_data *dma_data_be;

			list_del(&dma_be->list);
			dma_data_be = &dma_be->dma_data;
			kfree(dma_data_be->chan_name);
			dma_data_be->chan_name = NULL;
			kfree(dma_be);
		}

		/* free DSPs */
		tc = pcm->tc;
		for (i = 0; i < tc->thr_ch_count; i++) {
			struct mchp_asrc_slot const *slot;
			int j;

			slot = &asrc_map[priv->thr_opmode].slot[tc->thr_ch[i]];
			for (j = slot->first_dsp; j < slot->first_dsp + slot->dsp_count; j++)
				priv->dsp[j].dma = NULL;
		}
		list_for_each_entry_safe(dma_be, tmp, &dma->dma_out_list, list) {
			struct snd_dmaengine_dai_dma_data *dma_data_be;

			list_del(&dma_be->list);
			dma_data_be = &dma_be->dma_data;
			kfree(dma_data_be->chan_name);
			dma_data_be->chan_name = NULL;
			kfree(dma_be);
		}

		/* free mono DSPs, trigger index, and chunk size */
		list_for_each_entry(rtm_be, &pcm->list_head_in, list) {
			struct mchp_asrc_slot const *slot_be = rtm_be->slot;

			for (dsp = slot_be->first_dsp;
			     dsp < slot_be->first_dsp + slot_be->dsp_count; dsp++) {
				ch_conf_mask |= MCHP_ASRC_CH_CONF_MONO(dsp);
				mask |= MCHP_ASRC_TRIG_TRIGSELIN_MASK(dsp);
			}
			ch_conf_mask |= MCHP_ASRC_CH_CONF_CHUNK_MASK(slot_be->slot_id);
			rtm_be->slot = NULL;
		}
		list_for_each_entry(rtm_be, &pcm->list_head_out, list) {
			struct mchp_asrc_slot const *slot_be = rtm_be->slot;

			for (dsp = slot_be->first_dsp;
			     dsp < slot_be->first_dsp + slot_be->dsp_count; dsp++) {
				ch_conf_mask |= MCHP_ASRC_CH_CONF_MONO(dsp);
				mask |= MCHP_ASRC_TRIG_TRIGSELOUT_MASK(dsp);
			}
			ch_conf_mask |= MCHP_ASRC_CH_CONF_CHUNK_MASK(slot_be->slot_id);
			rtm_be->slot = NULL;
		}

		mchp_asrc_tc_free(priv, pcm);
	}

	pcm->refcount--;
	if (!pcm->refcount) {
		kfree(pcm);
		priv->pcm[dai->id] = NULL;
	}

	if (!are_dsps_running)
		regmap_update_bits(priv->regmap, MCHP_ASRC_MR, MCHP_ASRC_MR_GT96K, 0);

	/* cleanup the triggers for the used DSPs */
	regmap_update_bits(priv->regmap, MCHP_ASRC_TRIG, mask, 0);

	/* cleanup CHUNK and MONO DSPs */
	regmap_update_bits(priv->regmap, MCHP_ASRC_CH_CONF, ch_conf_mask, 0);

	return 0;
}

#ifdef DEBUG
static void mchp_asrc_reg_dump(struct mchp_asrc_dev *priv)
{
	int i;
	u32 val;

	regmap_read(priv->regmap, MCHP_ASRC_MR, &val);
	dev_dbg(priv->dev, "%s() Mode register %#x\n", __func__, val);
	for (i = 0; i < MCHP_ASRC_NB_STEREO_CH; i++) {
		regmap_read(priv->regmap, MCHP_ASRC_RATIO(i), &val);
		dev_dbg(priv->dev, "%s() Ratio%d register %#x\n", __func__, i, val);
	}
	regmap_read(priv->regmap, MCHP_ASRC_VBPS_IN, &val);
	dev_dbg(priv->dev, "%s() VBPS in register %#x\n", __func__, val);
	regmap_read(priv->regmap, MCHP_ASRC_VBPS_OUT, &val);
	dev_dbg(priv->dev, "%s() VBPS out register %#x\n", __func__, val);
	regmap_read(priv->regmap, MCHP_ASRC_CH_CONF, &val);
	dev_dbg(priv->dev, "%s() Channel conf register %#x\n", __func__, val);
	regmap_read(priv->regmap, MCHP_ASRC_TRIG, &val);
	dev_dbg(priv->dev, "%s() Trigger sel register %#x\n", __func__, val);
	for (i = 0; i < MCHP_ASRC_NB_STEREO_CH; i++) {
		regmap_read(priv->regmap, MCHP_ASRC_IMR(i), &val);
		dev_dbg(priv->dev, "%s() Interrupt mask register%d %#x\n", __func__, i, val);
	}
}
#else
static void mchp_asrc_reg_dump(struct mchp_asrc_dev *priv) {}
#endif

static void mchp_asrc_dsps_trigger(struct mchp_asrc_dev *priv, struct mchp_asrc_be_rtm *rtm_be,
				   bool enable, u32 *mr)
{
	struct mchp_asrc_slot const *slot_be = rtm_be->slot;
	u32 ir_reg = 0;
	u32 ir = 0;
	int dsp;

	if (!slot_be) {
		dev_dbg(priv->dev, "%s has no slot\n",
			rtm_be->trig ? rtm_be->trig->phandle->np->full_name : "front-end");
		return;
	}
#ifdef DEBUG
	ir |= MCHP_ASRC_IR_RXUDR | MCHP_ASRC_IR_RXOVR | MCHP_ASRC_IR_TXUDR | MCHP_ASRC_IR_TXOVR;
#endif

	if (enable)
		ir |= MCHP_ASRC_IR_LOCK;

	dev_dbg(priv->dev, "%s() DSPs %s for BE %s:\n", __func__, enable ? "enabled" : "disabled",
		rtm_be->trig ? rtm_be->trig->phandle->np->full_name : "front-end");

	for (dsp = slot_be->first_dsp; dsp < slot_be->first_dsp + slot_be->dsp_count; dsp++) {
		/* Enable interrupt and wait for DPLL LOCK */
		if (enable && !priv->direct_path)
			ir_reg = MCHP_ASRC_IER(dsp);
		else
			ir_reg = MCHP_ASRC_IDR(dsp);
		regmap_update_bits(priv->regmap, ir_reg, ir, ir);
		*mr |= MCHP_ASRC_MR_ASRCEN(dsp);
		if (enable)
			priv->dsp[dsp].dma->unlocked_dsps++;
		dev_dbg(priv->dev, "%s() %d\n", __func__, dsp);
	}
}

static int mchp_asrc_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	struct mchp_asrc_dev *priv = snd_soc_dai_get_drvdata(dai);
	struct mchp_asrc_pcm_priv *pcm = priv->pcm[dai->id];
	struct mchp_asrc_dmaengine_dai_dma *dma;
	struct mchp_asrc_be_rtm *rtm_be;
	u32 mr = 0;

	if (!pcm) {
		dev_err(priv->dev, "PCM not allocated for id %d\n", dai->id);
		return -EINVAL;
	}

	dma = &pcm->dma;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		list_for_each_entry(rtm_be, &pcm->list_head_in, list)
			mchp_asrc_dsps_trigger(priv, rtm_be, true, &mr);
		list_for_each_entry(rtm_be, &pcm->list_head_out, list)
			mchp_asrc_dsps_trigger(priv, rtm_be, true, &mr);
		dma->unlocked_dsps /= 2;
		regmap_update_bits(priv->regmap, MCHP_ASRC_MR, mr, mr);
		mchp_asrc_reg_dump(priv);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		list_for_each_entry(rtm_be, &pcm->list_head_out, list)
			mchp_asrc_dsps_trigger(priv, rtm_be, false, &mr);
		list_for_each_entry(rtm_be, &pcm->list_head_in, list)
			mchp_asrc_dsps_trigger(priv, rtm_be, false, &mr);

		regmap_update_bits(priv->regmap, MCHP_ASRC_MR, mr, 0);
		mchp_asrc_reg_dump(priv);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mchp_asrc_dai_ops = {
	.startup	= mchp_asrc_startup,
	.trigger	= mchp_asrc_trigger,
	.hw_params	= mchp_asrc_hw_params,
	.hw_free	= mchp_asrc_hw_free,
	.shutdown	= mchp_asrc_shutdown,
};

#define MCHP_ASRC_RATES	SNDRV_PCM_RATE_8000_192000
#define MCHP_ASRC_FORMATS	(SNDRV_PCM_FMTBIT_S8 | \
				 SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S18_3LE | \
				 SNDRV_PCM_FMTBIT_S20_3LE | \
				 SNDRV_PCM_FMTBIT_S24_3LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mchp_asrc_dai[] = {
	{
		.name = "asrc-pcm-1",
		.playback = {
			.stream_name = "ASRC-Playback 1",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.capture = {
			.stream_name = "ASRC-Capture 1",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.ops = &mchp_asrc_dai_ops,
	},
	{
		.name = "asrc-pcm-2",
		.playback = {
			.stream_name = "ASRC-Playback 2",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.capture = {
			.stream_name = "ASRC-Capture 2",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.ops = &mchp_asrc_dai_ops,
	},
	{
		.name = "asrc-pcm-3",
		.playback = {
			.stream_name = "ASRC-Playback 3",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.capture = {
			.stream_name = "ASRC-Capture 3",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.ops = &mchp_asrc_dai_ops,
	},
	{
		.name = "asrc-pcm-4",
		.playback = {
			.stream_name = "ASRC-Playback 4",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.capture = {
			.stream_name = "ASRC-Capture 4",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MCHP_ASRC_RATES,
			.formats = MCHP_ASRC_FORMATS,
		},
		.ops = &mchp_asrc_dai_ops,
	},
};

static void mchp_asrc_block_pending_intr(struct mchp_asrc_dev *priv,
					 u32 pending, int block)
{
	struct device *dev = priv->dev;

#ifdef DEBUG
	if (pending & MCHP_ASRC_IR_RXRDY)
		dev_info_ratelimited(dev, "block %d: RX Ready detected\n", block);

	if (pending & MCHP_ASRC_IR_RXEMPTY)
		dev_warn_ratelimited(dev, "block %d: RX FIFO Empty detected\n", block);

	if (pending & MCHP_ASRC_IR_RXFULL)
		dev_warn_ratelimited(dev, "block %d: RX FIFO Full detected\n", block);

	if (pending & MCHP_ASRC_IR_RXCHUNK)
		dev_info_ratelimited(dev, "block %d: RX FIFO Chunk detected\n", block);

	if (pending & MCHP_ASRC_IR_TXRDY)
		dev_info_ratelimited(dev, "block %d: TX Ready detected\n", block);

	if (pending & MCHP_ASRC_IR_TXEMPTY)
		dev_warn_ratelimited(dev, "block %d: TX FIFO Empty detected\n", block);

	if (pending & MCHP_ASRC_IR_TXFULL)
		dev_warn_ratelimited(dev, "block %d: TX FIFO Full detected\n", block);

	if (pending & MCHP_ASRC_IR_TXCHUNK)
		dev_info_ratelimited(dev, "block %d: TX FIFO Chunk detected\n", block);

#endif
	if (pending & MCHP_ASRC_IR_RXUDR)
		dev_err_ratelimited(dev, "block %d: RX Underflow detected\n", block);

	if (pending & MCHP_ASRC_IR_RXOVR)
		dev_err_ratelimited(dev, "block %d: RX Overflow detected\n", block);

	if (pending & MCHP_ASRC_IR_TXUDR)
		dev_err_ratelimited(dev, "block %d: TX Underflow detected\n", block);

	if (pending & MCHP_ASRC_IR_TXOVR)
		dev_err_ratelimited(dev, "block %d: TX Overflow detected\n", block);

	if (pending & MCHP_ASRC_IR_LOCK) {
		dev_dbg_ratelimited(dev, "block %d: DPLL locked detected\n", block);
		/* Disable DPLL LOCK interrupt  */
		regmap_update_bits(priv->regmap, MCHP_ASRC_IDR(block),
				   MCHP_ASRC_IR_LOCK, MCHP_ASRC_IR_LOCK);

		if (!priv->dsp[block].dma) {
			dev_err_ratelimited(dev, "block %d: DSP locked but no PCM assigned\n",
					    block);
			return;
		}

		if (!priv->dsp[block].dma->unlocked_dsps)
			return;
		priv->dsp[block].dma->unlocked_dsps--;

		/* DPLLs locked, start DMA transfers */
		if (!priv->dsp[block].dma->unlocked_dsps) {
			dev_dbg_ratelimited(dev, "block %d: DSPs ready, calling DMA handle\n",
					    block);
		} else {
			dev_dbg_ratelimited(dev, "block %d: waiting for %d more DSPs to lock\n",
					    block, priv->dsp[block].dma->unlocked_dsps);
		}
	}
}

static void mchp_asrc_block_interrupts(struct mchp_asrc_dev *priv, int block,
				       irqreturn_t *ret)
{
	u32 isr, imr, pending;

	regmap_read(priv->regmap, MCHP_ASRC_ISR(block), &isr);
	regmap_read(priv->regmap, MCHP_ASRC_IMR(block), &imr);
	dev_dbg_ratelimited(priv->dev, "IRQ handler: IMR %#x ISR %#x\n", imr, isr);

	pending = isr & imr;
	if (!pending)
		return;

	if (pending & MCHP_ASRC_IR_SECE) {
		dev_err_ratelimited(priv->dev, "block %d: Security/Safety Event detected\n", block);
		*ret = IRQ_HANDLED;
	}

	mchp_asrc_block_pending_intr(priv, pending, block);
	*ret = IRQ_HANDLED;
}

static irqreturn_t mchp_asrc_interrupt(int irq, void *dev_id)
{
	struct mchp_asrc_dev *priv = dev_id;
	irqreturn_t ret = IRQ_NONE;
	int block;

	for (block = 0; block < MCHP_ASRC_NB_STEREO_CH; block++)
		mchp_asrc_block_interrupts(priv, block, &ret);

	return ret;
}

static int mchp_asrc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct mchp_asrc_dev *priv;
	struct regmap *regmap;
	void __iomem *base;
	struct property *prop;
	const __be32 *cur;
	const void *data;
	u32 pv;
	u32 version;
	int irq;
	int err;
	int count;
	int i;

	/* Get memory for driver data */
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	data = device_get_match_data(&pdev->dev);
	if (data && !of_property_present(np, "microchip,disable-direct-path"))
		priv->direct_path = *(bool *)data;

	/* Map I/O registers */
	base = devm_platform_get_and_ioremap_resource(pdev, 0, &priv->mem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(&pdev->dev, base, &mchp_asrc_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&pdev->dev, "failed to init regmap: %ld\n", PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}
	priv->dev = &pdev->dev;
	priv->regmap = regmap;

	/* Request IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ no: %d\n", irq);
		return irq;
	}

	err = devm_request_irq(&pdev->dev, irq, mchp_asrc_interrupt, 0, dev_name(&pdev->dev), priv);
	if (err) {
		dev_err(&pdev->dev, "failed to claim IRQ %d: %d\n", irq, err);
		return err;
	}

	priv->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(priv->pclk)) {
		dev_err(&pdev->dev, "failed to get pclk clock: %ld\n", PTR_ERR(priv->pclk));
		return PTR_ERR(priv->pclk);
	}

	priv->gclk = devm_clk_get(&pdev->dev, "gclk");
	if (IS_ERR(priv->gclk)) {
		dev_err(&pdev->dev, "failed to get gclk clock: %ld\n", PTR_ERR(priv->gclk));
		return PTR_ERR(priv->gclk);
	}

	/* get the triggers list */
	priv->trig_count = of_count_phandle_with_args(np, "microchip,triggers", NULL);
	if (priv->trig_count < 0) {
		dev_err(&pdev->dev, "failed to get microchip,triggers: %d\n",
			priv->trig_count);
		return priv->trig_count;
	}
	if (priv->trig_count == 0) {
		dev_err(&pdev->dev, "no triggers found\n");
		return -EINVAL;
	}
	dev_dbg(&pdev->dev, "found %d triggers:\n", priv->trig_count);
	priv->trig = devm_kcalloc(&pdev->dev, priv->trig_count, sizeof(*priv->trig), GFP_KERNEL);
	if (!priv->trig)
		return -ENOMEM;

	for (i = 0; i < priv->trig_count; i++) {
		struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];

		trig_be->phandle = devm_kzalloc(&pdev->dev, sizeof(*trig_be->phandle), GFP_KERNEL);
		if (!trig_be->phandle) {
			err = -ENOMEM;
			goto __cleanup_of_phandle_triggers;
		}
		err = of_parse_phandle_with_fixed_args(np, "microchip,triggers",
						       0, i, trig_be->phandle);
		if (err < 0) {
			dev_err(&pdev->dev, "failed to get phandle for trigger %d: %d\n", i, err);
			devm_kfree(&pdev->dev, trig_be->phandle);
			trig_be->phandle = NULL;
			continue;
		}
	}
	count = 0;
	of_property_for_each_u32(np, "microchip,trigger-indexes", pv) {
		struct mchp_asrc_be_trigger *trig_be = &priv->trig[count];

		if (pv >= MCHP_ASRC_TRIG_IDX_MAX_VAL) {
			dev_err(&pdev->dev,
				"error: microchip,trigger-indexes too big: %d\n",
				pv);
			err = -EINVAL;
			goto __cleanup_of_phandle_triggers;
		}

		/* skip not found phandle */
		if (!trig_be->phandle)
			continue;

		trig_be->idx = pv;
		trig_be->priv = priv;
		count++;
		if (count > priv->trig_count) {
			dev_err(&pdev->dev,
				"Too many microchip,trigger-indexes: %d\n", count);
			err = -EINVAL;
			goto __cleanup_of_phandle_triggers;
		}
	}

#ifdef DEBUG
	dev_dbg(&pdev->dev, "Triggers %d:\n", count);
	for (i = 0; i < count; i++) {
		dev_dbg(&pdev->dev, "%s, index %d\n", priv->trig[i].phandle->np->full_name,
			priv->trig[i].idx);
	}
#endif

	count = 0;
	of_property_for_each_u32(np, "microchip,direct-path-indexes", prop, cur, pv) {
		struct mchp_asrc_be_trigger *trig_be = &priv->trig[count / 2];

		if (pv > MCHP_ASRC_DP_IDX_MAX) {
			dev_err(&pdev->dev,
				"error: microchip,direct-path-indexes too big: %d\n",
				pv);
			err = -EINVAL;
			goto __cleanup_of_phandle_triggers;
		}

		/* skip not found phandle */
		if (!trig_be->phandle)
			continue;

		if (!(count % 2))
			trig_be->dp_tx_idx = pv;
		else
			trig_be->dp_rx_idx = pv;

		count++;

		if (count > priv->trig_count * 2) {
			dev_err(&pdev->dev,
				"Too many microchip,direct-path-indexes: %d\n", count);
			err = -EINVAL;
			goto __cleanup_of_phandle_triggers;
		}
	}

#ifdef DEBUG
	dev_dbg(&pdev->dev, "Direct path IDs %d:\n", count);
	for (i = 0; i < count; i++) {
		dev_dbg(&pdev->dev, "%s, DP: TX=%d, RX=%d\n",
			priv->trig[i / 2].phandle->np->full_name,
			priv->trig[i / 2].dp_tx_idx,
			priv->trig[i / 2].dp_rx_idx);
	}
#endif

	priv->thr_opmode = -1;
	priv->rhr_opmode = -1;

	platform_set_drvdata(pdev, priv);

	err = devm_snd_soc_register_component(&pdev->dev, &mchp_asrc_component, mchp_asrc_dai,
					      ARRAY_SIZE(mchp_asrc_dai));
	if (err) {
		dev_err(&pdev->dev, "failed to register ASoC DAI: %d\n", err);
		clk_disable_unprepare(priv->pclk);
		goto __cleanup_of_phandle_triggers;
	}

	err = mchp_asrc_pcm_register(&pdev->dev, regmap, priv->direct_path);
	if (err) {
		dev_err(&pdev->dev, "failed to register ASoC platform\n");
		goto __cleanup_of_phandle_triggers;
	}

	/* Enable the peripheral clock */
	err = clk_prepare_enable(priv->pclk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable the peripheral clock: %d\n", err);
		goto __cleanup_of_phandle_triggers;
	}

	/* Software reset for all registers */
	regmap_write(priv->regmap, MCHP_ASRC_CR, MCHP_ASRC_CR_SWRST);

	/* Get IP version. */
	regmap_read(priv->regmap, MCHP_ASRC_VERSION, &version);

	dev_info(priv->dev, "hw version: %#lx\n", version & MCHP_ASRC_VERSION_MASK);

	return 0;

__cleanup_of_phandle_triggers:
	for (i = 0; i < priv->trig_count; i++) {
		struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];

		if (trig_be->phandle)
			of_node_put(trig_be->phandle->np);
	}
	return err;
}

static void mchp_asrc_remove(struct platform_device *pdev)
{
	struct mchp_asrc_dev *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->trig_count; i++) {
		struct mchp_asrc_be_trigger *trig_be = &priv->trig[i];

		if (trig_be->phandle)
			of_node_put(trig_be->phandle->np);
	}
	clk_disable_unprepare(priv->pclk);
}

static const int sama7d65_asrc_direct_path = 1;

static const struct of_device_id mchp_asrc_dt_ids[] = {
	{
		.compatible = "microchip,sama7g5-asrc",
	},
	{
		.compatible = "microchip,sama7d65-asrc",
		.data = &sama7d65_asrc_direct_path,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mchp_asrc_dt_ids);

static struct platform_driver mchp_asrc_driver = {
	.driver		= {
		.name	= "mchp_asrc",
		.of_match_table	= of_match_ptr(mchp_asrc_dt_ids),
	},
	.probe		= mchp_asrc_probe,
	.remove		= mchp_asrc_remove,
};
module_platform_driver(mchp_asrc_driver);

MODULE_DESCRIPTION("Microchip Asynchronous Sample Rate Converter (ASRC) driver");
MODULE_AUTHOR("Codrin Ciubotariu <codrin.ciubotariu@microchip.com>");
MODULE_LICENSE("GPL v2");
