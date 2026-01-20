// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Authors: Durai Manickam KR <durai.manickamkr@microchip.com>
 *
 * Microchip I3C HCI Quirks
 */

#include <linux/i3c/master.h>
#include "hci.h"

/* Timing registers */
#define MCHP_HCI_SCL_I3C_OD_TIMING          0x214
#define MCHP_HCI_SCL_I3C_PP_TIMING          0x218
#define MCHP_HCI_SDA_HOLD_SWITCH_DLY_TIMING 0x230

/* Timing values to configure 9MHz frequency */
#define MCHP_SCL_I3C_OD_TIMING          0x00cf00cf
#define MCHP_SCL_I3C_PP_TIMING          0x00160016

#define MCHP_QUEUE_THLD_CTRL                0xD0

void mchp_set_od_pp_timing(struct i3c_hci *hci)
{
	u32 data;

	reg_write(MCHP_HCI_SCL_I3C_OD_TIMING, MCHP_SCL_I3C_OD_TIMING);
	reg_write(MCHP_HCI_SCL_I3C_PP_TIMING, MCHP_SCL_I3C_PP_TIMING);
	data = reg_read(MCHP_HCI_SDA_HOLD_SWITCH_DLY_TIMING);
	/* Configure maximum TX hold time */
	data |= W0_MASK(18, 16);
	reg_write(MCHP_HCI_SDA_HOLD_SWITCH_DLY_TIMING, data);
}

void mchp_set_resp_buf_thld(struct i3c_hci *hci)
{
	u32 data;

	data = reg_read(MCHP_QUEUE_THLD_CTRL);
	data = data & ~W0_MASK(15, 8);
	reg_write(MCHP_QUEUE_THLD_CTRL, data);
}
