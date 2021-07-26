// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2012, Bootlin
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/parser.h>
#include <linux/string.h>
#include "generic.h"
#include "sam_secure.h"
#include "pm.h"

static int suspend_mode = AT91_PM_ULP0;

static const match_table_t pm_modes __initconst = {
	{ AT91_PM_STANDBY,	"standby" },
	{ AT91_PM_ULP0,		"ulp0" },
	{ AT91_PM_ULP0_FAST,    "ulp0-fast" },
	{ AT91_PM_ULP1,		"ulp1" },
	{ AT91_PM_BACKUP,	"backup" },
	{ -1, NULL },
};

static void at91_pm_secure_init(void)
{
	struct arm_smccc_res res;

	res = sam_smccc_call(SAMA5_SMC_SIP_SET_SUSPEND_MODE, suspend_mode, 0);
	if (res.a0 == 0) {
		pr_info("AT91: Secure PM: suspend mode set to %s\n",
			pm_modes[suspend_mode].pattern);
		return;
	}

	pr_warn("AT91: Secure PM: %s mode not supported !\n",
		pm_modes[suspend_mode].pattern);

	res = sam_smccc_call(SAMA5_SMC_SIP_GET_SUSPEND_MODE, 0, 0);
	if (res.a0 == 0) {
		pr_warn("AT91: Secure PM: failed to get default mode\n");
		return;
	}
	suspend_mode = res.a1;

	pr_info("AT91: Secure PM: using default suspend mode %s\n",
		pm_modes[suspend_mode].pattern);
}

void __init at91rm9200_pm_init(void)
{
}

void __init at91sam9_pm_init(void)
{
}

void __init sam9x60_pm_init(void)
{
}

void __init sama5_pm_init(void)
{
}

void __init sama5d2_pm_init(void)
{
	at91_pm_secure_init();
}

int at91_suspend_entering_slow_clock(void)
{
	return (suspend_mode >= AT91_PM_ULP0);
}
EXPORT_SYMBOL(at91_suspend_entering_slow_clock);

static int __init at91_pm_modes_select(char *str)
{
	substring_t args[MAX_OPT_ARGS];
	int suspend;

	if (!str)
		return 0;

	pr_warn("AT91: Secure PM: ignoring standby mode\n");
	/*
	 * Keep the same format than for standard suspend but only consider
	 * second part of the parameter
	 */
	strsep(&str, ",");
	suspend = match_token(str, pm_modes, args);
	if (suspend < 0)
		return 0;

	suspend_mode = suspend;
	return 0;
}
early_param("atmel.pm_modes", at91_pm_modes_select);
