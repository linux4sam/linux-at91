/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 *
 * Synopsys DesignWare MIPI CSI-2 platform data
 *
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 */

#include <linux/kernel.h>
#include <media/dwc/dw-mipi-csi-pltfrm.h>

struct dw_csih_pdata {
	u8 eotp_enabled;
	u32 hs_freq;
	u32 lanes;
	u32 pclk;
	u32 fps;
	u32 bpp;
	u8 id;
};

static const struct pdata_names csis[] = {
	{ .name = "dw-csi.0", },
	{ .name = "dw-csi.1", },
};
