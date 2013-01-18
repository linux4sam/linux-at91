/*
 * sama5d3ek_wm8904 - SoC audio for SAMA5D3EK based boards
 *                    which use WM8904 as codec.
 *
 * Copyright (C) 2012 Atmel
 *
 * Author: Bo Shen <voice.shen@atmel.com>
 * Based on sam9g20_wm8731.c by Sedji Gaouaou
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include <asm/mach-types.h>
#include <mach/hardware.h>
#include <mach/gpio.h>

#include "../codecs/wm8904.h"
#include "atmel_ssc_dai.h"

#define MCLK_RATE 32768

static struct clk *mclk;

static const struct snd_soc_dapm_route intercon[] = {
	{ "MICBIAS", NULL, "IN1L" },
	{ "Left Capture Mux", NULL, "MICBIAS" },
};

static int sama5d3_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	ret = snd_soc_dai_set_pll(codec_dai, WM8904_FLL_MCLK, WM8904_FLL_MCLK,
		32768, params_rate(params) * 256);
	if (ret < 0) {
		pr_err("%s - Failed to set CODEC PLL.", __func__);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8904_CLK_FLL,
		12000000, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		pr_err("%s - Failed to set WM8904 SYSCLK\n", __func__);
		return ret;
	}

	return 0;
}

static struct snd_soc_ops sama5d3_soc_ops = {
	.hw_params = sama5d3_hw_params,
};

static int sama5d3_wm8904_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	pr_debug("ASoC: sama5d3_wm8904_init() called\n");

	snd_soc_dapm_nc_pin(dapm, "IN1R");
	snd_soc_dapm_nc_pin(dapm, "IN3L");
	snd_soc_dapm_nc_pin(dapm, "IN3R");
	
	snd_soc_dapm_nc_pin(dapm, "LINEOUTL");
	snd_soc_dapm_nc_pin(dapm, "LINEOUTR");

	snd_soc_dapm_enable_pin(dapm, "HPOUTL");
	snd_soc_dapm_enable_pin(dapm, "HPOUTR");
	snd_soc_dapm_enable_pin(dapm, "IN1L");
	snd_soc_dapm_enable_pin(dapm, "IN2L");
	snd_soc_dapm_enable_pin(dapm, "IN2R");
	snd_soc_dapm_enable_pin(dapm, "MICBIAS");

	snd_soc_dapm_add_routes(dapm, intercon, ARRAY_SIZE(intercon));

	snd_soc_dapm_sync(dapm);

	return 0;
}

int sama5d3ek_snd_suspend_pre(struct snd_soc_card *card)
{
	clk_disable(mclk);
	return 0;
}

int sama5d3ek_snd_resume_pre(struct snd_soc_card *card)
{
	clk_enable(mclk);
	return 0;
}

static struct snd_soc_dai_link sama5d3ek_dai = {
	.name = "WM8904",
	.stream_name = "WM8904 PCM",
	.codec_dai_name = "wm8904-hifi",
	.init = sama5d3_wm8904_init,
	.dai_fmt = SND_SOC_DAIFMT_I2S
		| SND_SOC_DAIFMT_NB_NF
		| SND_SOC_DAIFMT_CBM_CFM,
	.ops = &sama5d3_soc_ops,
};

static struct snd_soc_card snd_soc_sama5d3ek = {
	.name = "WM8904 @ SAMA5D3",
	.dai_link = &sama5d3ek_dai,
	.num_links = 1,
	.suspend_pre = sama5d3ek_snd_suspend_pre,
	.resume_pre = sama5d3ek_snd_resume_pre,
};

static int sama5d3ek_wm8904_dt_init(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *codec_np, *cpu_np;
	struct snd_soc_card *card = &snd_soc_sama5d3ek;
	int ret;

	if (!np)
		return -1;

	ret = snd_soc_of_parse_card_name(card, "atmel,model");
	if (ret) {
		dev_err(&pdev->dev, "failed to parse card name\n");
		return ret;
	}

	ret = snd_soc_of_parse_audio_routing(card, "atmel,audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "failed to parse audio routing\n");
		return ret;
	}

	cpu_np = of_parse_phandle(np, "atmel,ssc-controller", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "failed to get dai and pcm info\n");
		ret = -EINVAL;
		return ret;
	}
	sama5d3ek_dai.cpu_of_node = cpu_np;
	sama5d3ek_dai.platform_of_node = cpu_np;
	of_node_put(cpu_np);

	codec_np = of_parse_phandle(np, "atmel,audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "failed to get codec info\n");
		ret = -EINVAL;
		return ret;
	}
	sama5d3ek_dai.codec_of_node = codec_np;
	of_node_put(codec_np);

	return 0;
}

static int sama5d3ek_wm8904_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_sama5d3ek;
	struct clk *clk_src;
	struct pinctrl *pinctrl;
	int ret;

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		dev_err(&pdev->dev, "failed to request pinctrl\n");
		return PTR_ERR(pinctrl);
	}

	ret = atmel_ssc_set_audio(0);
	if (ret != 0) {
		dev_err(&pdev->dev, "failed to set SSC 0 for audio\n");
		return ret;
	}

	card->dev = &pdev->dev;
	ret = sama5d3ek_wm8904_dt_init(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to init dt info\n");
		goto err_set_audio;
	}

	mclk = clk_get(NULL, "pck0");
	if (IS_ERR(mclk)) {
		dev_err(&pdev->dev, "failed to get pck0\n");
		ret = PTR_ERR(mclk);
		goto err_set_audio;
	}

	clk_src = clk_get(NULL, "clk32k");
	if (IS_ERR(clk_src)) {
		dev_err(&pdev->dev, "failed to get clk32k\n");
		ret = PTR_ERR(clk_src);
		goto err_set_audio;
	}

	ret = clk_set_parent(mclk, clk_src);
	clk_put(clk_src);
	if (ret != 0) {
		dev_err(&pdev->dev, "failed to set MCLK parent\n");
		goto err_set_audio;
	}
	
	dev_info(&pdev->dev, "setting pck0 to %dHz\n", MCLK_RATE);

	clk_set_rate(mclk, MCLK_RATE);
	clk_enable(mclk);

	snd_soc_register_card(card);

	return 0;

err_set_audio:
	atmel_ssc_put_audio(0);
	return ret;
}

static int sama5d3ek_wm8904_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	atmel_ssc_put_audio(0);
	snd_soc_unregister_card(card);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id sama5d3ek_wm8904_dt_ids[] = {
	{ .compatible = "atmel,sama5d3ek-wm8904", },
	{ }
};
#endif

static struct platform_driver sama5d3ek_wm8904_driver = {
	.driver = {
		.name = "sama5d3ek-audio",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(sama5d3ek_wm8904_dt_ids),
	},
	.probe = sama5d3ek_wm8904_probe,
	.remove = sama5d3ek_wm8904_remove,
};

module_platform_driver(sama5d3ek_wm8904_driver);

/* Module information */
MODULE_AUTHOR("Bo Shen <voice.shen@atmel.com>");
MODULE_DESCRIPTION("ALSA SoC machine driver for AT91SAMA5D3 - WM8904");
MODULE_LICENSE("GPL");
