// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Setup code for SAMA7
 *
 * Copyright (C) 2021 Microchip Technology, Inc. and its subsidiaries
 *
 */

#include <linux/of.h>
#include <linux/of_platform.h>

#include <asm/mach/arch.h>
#include <asm/system_misc.h>

#include "generic.h"
#include "sam_secure.h"

static void __init sama7_secure_cache_init(void)
{
	sam_secure_init();
}

static void __init sama7_dt_device_init(void)
{
	of_platform_default_populate(NULL, NULL, NULL);
	sama7_pm_init();
}

static const char *const sama7_dt_board_compat[] __initconst = {
	"microchip,sama7",
	NULL
};

DT_MACHINE_START(sama7_dt, "Microchip SAMA7")
	/* Maintainer: Microchip */
	.init_machine	= sama7_dt_device_init,
	.init_early	= sama7_secure_cache_init,
	.dt_compat	= sama7_dt_board_compat,
MACHINE_END

