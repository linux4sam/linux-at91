/*
 * arch/arm/mach-at91/pm_slow_clock.S
 *
 *  Copyright (C) 2006 Savin Zlobec
 *
 * AT91SAM9 support:
 *  Copyright (C) 2007 Anti Sullin <anti.sullin@artecdesign.ee
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/linkage.h>
#include <mach/hardware.h>
#include <mach/at91_pmc.h>
#include <mach/at91_ramc.h>

#ifdef CONFIG_SOC_AT91SAM9263
/*
 * FIXME either or both the SDRAM controllers (EB0, EB1) might be in use;
 * handle those cases both here and in the Suspend-To-RAM support.
 */
#warning Assuming EB1 SDRAM controller is *NOT* used
#endif

/*
 * When SLOWDOWN_MASTER_CLOCK is defined we will also slow down the Master
 * clock during suspend by adjusting its prescalar and divisor.
 * NOTE: This hasn't been shown to be stable on SAM9s; and on the RM9200 there
 *       are errata regarding adjusting the prescalar and divisor.
 */
#undef SLOWDOWN_MASTER_CLOCK

#define MCKRDY_TIMEOUT		1000
#define MOSCRDY_TIMEOUT 	1000
#define PLLALOCK_TIMEOUT	1000
#define PLLBLOCK_TIMEOUT	1000
#define UPLLLOCK_TIMEOUT	1000

pmc	.req	r0
sdramc	.req	r1
ramc1	.req	r2
memctrl	.req	r3
tmp1	.req	r4
tmp2	.req	r5
ddrcid	.req	r6
flag	.req	r7

/*
 * Wait until master clock is ready (after switching master clock source)
 */
	.macro wait_mckrdy
	mov	tmp2, #MCKRDY_TIMEOUT
1:	sub	tmp2, tmp2, #1
	cmp	tmp2, #0
	beq	2f
	ldr	tmp1, [pmc, #AT91_PMC_SR]
	tst	tmp1, #AT91_PMC_MCKRDY
	beq	1b
2:
	.endm

/*
 * Wait until master oscillator has stabilized.
 */
	.macro wait_moscrdy
	mov	tmp2, #MOSCRDY_TIMEOUT
1:	sub	tmp2, tmp2, #1
	cmp	tmp2, #0
	beq	2f
	ldr	tmp1, [pmc, #AT91_PMC_SR]
	tst	tmp1, #AT91_PMC_MOSCS
	beq	1b
2:
	.endm

/*
 * Wait until PLLA has locked.
 */
	.macro wait_pllalock
	mov	tmp2, #PLLALOCK_TIMEOUT
1:	sub	tmp2, tmp2, #1
	cmp	tmp2, #0
	beq	2f
	ldr	tmp1, [pmc, #AT91_PMC_SR]
	tst	tmp1, #AT91_PMC_LOCKA
	beq	1b
2:
	.endm

/*
 * Wait until PLLB has locked.
 */
	.macro wait_pllblock
	mov	tmp2, #PLLBLOCK_TIMEOUT
1:	sub	tmp2, tmp2, #1
	cmp	tmp2, #0
	beq	2f
	ldr	tmp1, [pmc, #AT91_PMC_SR]
	tst	tmp1, #AT91_PMC_LOCKB
	beq	1b
2:
	.endm

/*
 * Wait until UTMI PLL has locked.
 */
	.macro wait_uplllock
	mov	tmp2, #UPLLLOCK_TIMEOUT
1:	sub	tmp2, tmp2, #1
	cmp	tmp2, #0
	beq	2f
	ldr	tmp1, [pmc, #AT91_PMC_SR]
	tst	tmp1, #AT91_PMC_LOCKU
	beq	1b
2:
	.endm

/*
 * Put the processor to enter into Standby mode, wait for interrupt to wakeup
 */
	.macro _do_wfi

#if defined(CONFIG_CPU_V7)
	dsb

	mov	tmp1, #AT91_PMC_PCK
	str	tmp1, [pmc, #AT91_PMC_SCDR]

	wfi		@ Wait For Interrupt
#else
	mcr	p15, 0, tmp1, c7, c0, 4
#endif

	.endm

	.text

/* void at91_slow_clock(void __iomem *pmc, void __iomem *sdramc,
 *			void __iomem *ramc1, int memctrl)
 */
ENTRY(at91_slow_clock)
	/* Save registers on stack */
	stmfd	sp!, {r4 - r12, lr}

	/*
	 * Register usage:
	 *  R0 = Base address of AT91_PMC
	 *  R1 = Base address of RAM Controller (SDRAM, DDRSDR, or AT91_SYS)
	 *  R2 = Base address of second RAM Controller or 0 if not present
	 *  R3 = Memory controller related info
	 *  R4 = temporary register
	 *  R5 = temporary register
	 *  R6 = Memory controller's peripherial ID
	 *  R7 = temporary indicator
	 */

	/* Drain write buffer */
	mov	tmp1, #0
	mcr	p15, 0, tmp1, c7, c10, 4

	and	ddrcid, memctrl, #AT91_MEMCTRL_ID_MASK
	lsr	ddrcid, #AT91_MEMCTRL_ID_SHIFT
	and	memctrl, memctrl, #AT91_MEMCTRL_MASK

	cmp	memctrl, #AT91_MEMCTRL_MC
	bne	ddr_sr_enable

	/*
	 * at91rm9200 Memory controller
	 */
	/* Put SDRAM in self-refresh mode */
	mov	tmp1, #1
	str	tmp1, [sdramc, #AT91RM9200_SDRAMC_SRR]
	b	sdr_sr_done

	/*
	 * DDRSDR Memory controller
	 */
ddr_sr_enable:
	cmp	memctrl, #AT91_MEMCTRL_DDRSDR
	bne	sdr_sr_enable

	/* LPDDR1 --> force DDR2 mode during self-refresh */
	ldr	tmp1, [sdramc, #AT91_DDRSDRC_MDR]
	str	tmp1, .saved_sam9_mdr
	bic	tmp1, tmp1, #~AT91_DDRSDRC_MD
	cmp	tmp1, #AT91_DDRSDRC_MD_LOW_POWER_DDR
	ldreq	tmp1, [sdramc, #AT91_DDRSDRC_MDR]
	biceq	tmp1, tmp1, #AT91_DDRSDRC_MD
	orreq	tmp1, tmp1, #AT91_DDRSDRC_MD_DDR2
	streq	tmp1, [sdramc, #AT91_DDRSDRC_MDR]

	/* prepare for DDRAM self-refresh mode */
	ldr	tmp1, [sdramc, #AT91_DDRSDRC_LPR]
	str	tmp1, .saved_sam9_lpr
	bic	tmp1, #AT91_DDRSDRC_LPCB
	orr	tmp1, #AT91_DDRSDRC_LPCB_SELF_REFRESH

	/* figure out if we use the second ram controller */
	cmp	ramc1, #0
	beq	ddr_no_2nd_ctrl

	ldr	tmp2, [ramc1, #AT91_DDRSDRC_MDR]
	str	tmp2, .saved_sam9_mdr1
	bic	tmp2, tmp2, #~AT91_DDRSDRC_MD
	cmp	tmp2, #AT91_DDRSDRC_MD_LOW_POWER_DDR
	ldreq	tmp2, [ramc1, #AT91_DDRSDRC_MDR]
	biceq	tmp2, tmp2, #AT91_DDRSDRC_MD
	orreq	tmp2, tmp2, #AT91_DDRSDRC_MD_DDR2
	streq	tmp2, [ramc1, #AT91_DDRSDRC_MDR]

	ldr	tmp2, [ramc1, #AT91_DDRSDRC_LPR]
	str	tmp2, .saved_sam9_lpr1
	bic	tmp2, #AT91_DDRSDRC_LPCB
	orr	tmp2, #AT91_DDRSDRC_LPCB_SELF_REFRESH

	/* Enable DDRAM self-refresh mode */
	str	tmp2, [ramc1, #AT91_DDRSDRC_LPR]
ddr_no_2nd_ctrl:
	str	tmp1, [sdramc, #AT91_DDRSDRC_LPR]

	b	sdr_sr_done

	/*
	 * SDRAMC Memory controller
	 */
sdr_sr_enable:
	/* Enable SDRAM self-refresh mode */
	ldr	tmp1, [sdramc, #AT91_SDRAMC_LPR]
	str	tmp1, .saved_sam9_lpr

	bic	tmp1, #AT91_SDRAMC_LPCB
	orr	tmp1, #AT91_SDRAMC_LPCB_SELF_REFRESH
	str	tmp1, [sdramc, #AT91_SDRAMC_LPR]

sdr_sr_done:
	/* Disable MPDDRC Clock*/
	cmp	ddrcid, #0
	beq	2f
	bic	tmp2, ddrcid, #0xe0 /* fetch lowest 5 bits */
	mov	tmp1, #0x01
	mov	tmp1, tmp1, lsl tmp2

	tst	ddrcid, #0x20	/* > 32 ? */
	beq	1f
	str	tmp1, [pmc, #AT91_PMC_PCDR1]
	b	2f
1:
	str	tmp1, [pmc, #AT91_PMC_PCDR]
2:

	/* Disable DDR Clock */
	mov 	tmp1, #AT91_PMC_SYS_DDR
	str	tmp1, [pmc, #AT91_PMC_SCDR]

	/* Save PLLB setting and disable it */
	ldr	tmp1, [pmc, #AT91_CKGR_PLLBR]
	str	tmp1, .saved_pllbr

	mov	tmp1, #AT91_PMC_PLLCOUNT
	str	tmp1, [pmc, #AT91_CKGR_PLLBR]

	/* Disable UTMI PLL */
	ldr	tmp1, [pmc, #AT91_CKGR_UCKR]
	tst	tmp1, #AT91_PMC_UPLLEN
	beq	1f
	mov	flag, #1
	bic	tmp1, tmp1, #AT91_PMC_UPLLEN
	str	tmp1, [pmc, #AT91_CKGR_UCKR]
	b	2f
1:
	mov	flag, #0
2:

	/* Save Master clock setting */
	ldr	tmp1, [pmc, #AT91_PMC_MCKR]
	str	tmp1, .saved_mckr

	/*
	 * Set the Master clock source to slow clock
	 */
	bic	tmp1, tmp1, #AT91_PMC_CSS
	str	tmp1, [pmc, #AT91_PMC_MCKR]

	wait_mckrdy

#ifdef SLOWDOWN_MASTER_CLOCK
	/*
	 * Set the Master Clock PRES and MDIV fields.
	 *
	 * See AT91RM9200 errata #27 and #28 for details.
	 */
	mov	tmp1, #0
	str	tmp1, [pmc, #AT91_PMC_MCKR]

	wait_mckrdy
#endif

	/* Save PLLA setting and disable it */
	ldr	tmp1, [pmc, #AT91_CKGR_PLLAR]
	str	tmp1, .saved_pllar

	mov	tmp1, #AT91_PMC_PLLCOUNT
	orr	tmp1, tmp1, #(1 << 29)		/* bit 29 always set */
	str	tmp1, [pmc, #AT91_CKGR_PLLAR]

	/* Turn off the main oscillator */
	ldr	tmp1, [pmc, #AT91_CKGR_MOR]
	bic	tmp1, tmp1, #AT91_PMC_MOSCEN
	orr	tmp1, tmp1, #AT91_PMC_KEY
	str	tmp1, [pmc, #AT91_CKGR_MOR]

	/* Wait for interrupt */
	_do_wfi

	/* Turn on the main oscillator */
	ldr	tmp1, [pmc, #AT91_CKGR_MOR]
	orr	tmp1, tmp1, #AT91_PMC_MOSCEN
	orr	tmp1, tmp1, #AT91_PMC_KEY
	str	tmp1, [pmc, #AT91_CKGR_MOR]

	wait_moscrdy

	/* Restore PLLA setting */
	ldr	tmp1, .saved_pllar
	str	tmp1, [pmc, #AT91_CKGR_PLLAR]

	tst	tmp1, #(AT91_PMC_MUL &  0xff0000)
	bne	3f
	tst	tmp1, #(AT91_PMC_MUL & ~0xff0000)
	beq	4f
3:
	wait_pllalock
4:

#ifdef SLOWDOWN_MASTER_CLOCK
	/*
	 * First set PRES if it was not 0,
	 * than set CSS and MDIV fields.
	 *
	 * See AT91RM9200 errata #27 and #28 for details.
	 */
	ldr	tmp1, .saved_mckr
	tst	tmp1, #AT91_PMC_PRES
	beq	2f
	and	tmp1, tmp1, #AT91_PMC_PRES
	str	tmp1, [pmc, #AT91_PMC_MCKR]

	wait_mckrdy
#endif

	/*
	 * Restore master clock setting
	 */
2:	ldr	tmp1, .saved_mckr
	str	tmp1, [pmc, #AT91_PMC_MCKR]

	wait_mckrdy

	/* Turn on UTMI PLL */
	cmp	flag, #1
	bne	1f
	ldr	tmp1, [pmc, #AT91_CKGR_UCKR]
	orr	tmp1, tmp1, #AT91_PMC_UPLLEN
	str	tmp1, [pmc, #AT91_CKGR_UCKR]

	wait_uplllock
1:

	/* Restore PLLB setting */
	ldr	tmp1, .saved_pllbr
	str	tmp1, [pmc, #AT91_CKGR_PLLBR]

	tst	tmp1, #(AT91_PMC_MUL &  0xff0000)
	bne	1f
	tst	tmp1, #(AT91_PMC_MUL & ~0xff0000)
	beq	2f
1:
	wait_pllblock
2:

	/* Enable MPDDRC Clock*/
	cmp	ddrcid, #0
	beq	4f
	bic	tmp2, ddrcid, #0xe0 /* fetch lowest 5 bits */
	mov	tmp1, #0x01
	mov	tmp1, tmp1, lsl tmp2

	tst	ddrcid, #0x20	/* > 32 ? */
	beq	3f
	str	tmp1, [pmc, #AT91_PMC_PCER1]
	b	4f
3:
	str	tmp1, [pmc, #AT91_PMC_PCER]
4:

	/* Enable DDR clock */
	mov 	tmp1, #AT91_PMC_SYS_DDR
	str	tmp1, [pmc, #AT91_PMC_SCER]

	/*
	 * at91rm9200 Memory controller
	 * Do nothing - self-refresh is automatically disabled.
	 */
	cmp	memctrl, #AT91_MEMCTRL_MC
	beq	ram_restored

	/*
	 * DDRSDR Memory controller
	 */
	cmp	memctrl, #AT91_MEMCTRL_DDRSDR
	bne	sdr_en_restore
	/* Restore MDR in case of LPDDR1 */
	ldr	tmp1, .saved_sam9_mdr
	str	tmp1, [sdramc, #AT91_DDRSDRC_MDR]
	/* Restore LPR on AT91 with DDRAM */
	ldr	tmp1, .saved_sam9_lpr
	str	tmp1, [sdramc, #AT91_DDRSDRC_LPR]

	/* if we use the second ram controller */
	cmp	ramc1, #0
	ldrne	tmp2, .saved_sam9_mdr1
	strne	tmp2, [ramc1, #AT91_DDRSDRC_MDR]
	ldrne	tmp2, .saved_sam9_lpr1
	strne	tmp2, [ramc1, #AT91_DDRSDRC_LPR]

	b	ram_restored

	/*
	 * SDRAMC Memory controller
	 */
sdr_en_restore:
	/* Restore LPR on AT91 with SDRAM */
	ldr	tmp1, .saved_sam9_lpr
	str	tmp1, [sdramc, #AT91_SDRAMC_LPR]

ram_restored:
	/* Restore registers, and return */
	ldmfd	sp!, {r4 - r12, pc}


.saved_mckr:
	.word 0

.saved_pllar:
	.word 0

.saved_pllbr:
	.word 0

.saved_sam9_lpr:
	.word 0

.saved_sam9_lpr1:
	.word 0

.saved_sam9_mdr:
	.word 0

.saved_sam9_mdr1:
	.word 0

ENTRY(at91_slow_clock_sz)
	.word .-at91_slow_clock
