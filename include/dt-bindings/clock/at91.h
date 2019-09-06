/*
 * This header provides constants for AT91 pmc status.
 *
 * The constants defined in this header are being used in dts.
 *
 * Licensed under GPLv2 or later.
 */

#ifndef _DT_BINDINGS_CLK_AT91_H
#define _DT_BINDINGS_CLK_AT91_H

#define PMC_TYPE_CORE		0
#define PMC_TYPE_SYSTEM		1
#define PMC_TYPE_PERIPHERAL	2
#define PMC_TYPE_GCK		3

#define PMC_SLOW		0
#define PMC_MCK			1
#define PMC_UTMI		2
#define PMC_MAIN		3
#define PMC_MCK2		4
#define PMC_I2S0_MUX		5
#define PMC_I2S1_MUX		6

#ifndef AT91_PMC_MOSCS
#define AT91_PMC_MOSCS		0		/* MOSCS Flag */
#define AT91_PMC_LOCKA		1		/* PLLA Lock */
#define AT91_PMC_LOCKB		2		/* PLLB Lock */
#define AT91_PMC_MCKRDY		3		/* Master Clock */
#define AT91_PMC_LOCKU		6		/* UPLL Lock */
#define AT91_PMC_PCKRDY(id)	(8 + (id))	/* Programmable Clock */
#define AT91_PMC_MOSCSELS	16		/* Main Oscillator Selection */
#define AT91_PMC_MOSCRCS	17		/* Main On-Chip RC */
#define AT91_PMC_CFDEV		18		/* Clock Failure Detector Event */
#define AT91_PMC_GCKRDY		24		/* Generated Clocks */
#endif

/* SAMA5 Peripheral Identifiers - see table 18-9 in the "SAMA5D2 series" datasheet */
#define AT91_PERIPH_ID_ARM_PMU      2
#define AT91_PERIPH_ID_PIT          3
#define AT91_PERIPH_ID_WDT          4
#define AT91_PERIPH_ID_GMAC         5
#define AT91_PERIPH_ID_XDMAC0       6
#define AT91_PERIPH_ID_XDMAC1       7
#define AT91_PERIPH_ID_ICM          8
#define AT91_PERIPH_ID_AES          9
#define AT91_PERIPH_ID_AESB        10
#define AT91_PERIPH_ID_TDES        11
#define AT91_PERIPH_ID_SHA         12
#define AT91_PERIPH_ID_MPDDRC      13
#define AT91_PERIPH_ID_H32MX       14
#define AT91_PERIPH_ID_H64MX       15
#define AT91_PERIPH_ID_SECUMOD     16
#define AT91_PERIPH_ID_HSMC        17
#define AT91_PERIPH_ID_PIOA        18
#define AT91_PERIPH_ID_FLEXCOM0    19
#define AT91_PERIPH_ID_FLEXCOM1    20
#define AT91_PERIPH_ID_FLEXCOM2    21
#define AT91_PERIPH_ID_FLEXCOM3    22
#define AT91_PERIPH_ID_FLEXCOM4    23
#define AT91_PERIPH_ID_UART0       24
#define AT91_PERIPH_ID_UART1       25
#define AT91_PERIPH_ID_UART2       26
#define AT91_PERIPH_ID_UART3       27
#define AT91_PERIPH_ID_UART4       28
#define AT91_PERIPH_ID_TWIHS0      29
#define AT91_PERIPH_ID_TWIHS1      30
#define AT91_PERIPH_ID_SDMMC0      31
#define AT91_PERIPH_ID_SDMMC1      32
#define AT91_PERIPH_ID_SPI0        33
#define AT91_PERIPH_ID_SPI1        34
#define AT91_PERIPH_ID_TC0         35
#define AT91_PERIPH_ID_TC1         36
#define AT91_PERIPH_ID_PWM         38
#define AT91_PERIPH_ID_ADC         40
#define AT91_PERIPH_ID_UHPHS       41
#define AT91_PERIPH_ID_UDPHS       42
#define AT91_PERIPH_ID_SSC0        43
#define AT91_PERIPH_ID_SSC1        44
#define AT91_PERIPH_ID_LCDC        45
#define AT91_PERIPH_ID_ISC         46
#define AT91_PERIPH_ID_TRNG        47
#define AT91_PERIPH_ID_PDMIC       48
#define AT91_PERIPH_ID_AIC_IRQ     49
#define AT91_PERIPH_ID_SFC         50
#define AT91_PERIPH_ID_SECURAM     51
#define AT91_PERIPH_ID_QSPI0       52
#define AT91_PERIPH_ID_QSPI1       53
#define AT91_PERIPH_ID_I2SC0       54
#define AT91_PERIPH_ID_I2SC1       55
#define AT91_PERIPH_ID_MCAN0_INT0  56
#define AT91_PERIPH_ID_MCAN1_INT0  57
#define AT91_PERIPH_ID_PTC         58
#define AT91_PERIPH_ID_CLASSD      59
#define AT91_PERIPH_ID_SFR         60
#define AT91_PERIPH_ID_SAIC        61
#define AT91_PERIPH_ID_AIC         62
#define AT91_PERIPH_ID_L2CC        63
#define AT91_PERIPH_ID_MCAN0_INT1  64
#define AT91_PERIPH_ID_MCAN1_INT1  65
#define AT91_PERIPH_ID_GMAC_Q1     66
#define AT91_PERIPH_ID_GMAC_Q2     67
#define AT91_PERIPH_ID_PIOB        68
#define AT91_PERIPH_ID_PIOC        69
#define AT91_PERIPH_ID_PIOD        70
#define AT91_PERIPH_ID_SDMMC0_TMR  71
#define AT91_PERIPH_ID_SDMMC1_TMR  72
#define AT91_PERIPH_ID_RSTC        73
#define AT91_PERIPH_ID_SYSC_RTC    74
#define AT91_PERIPH_ID_ACC         75
#define AT91_PERIPH_ID_RXLP        76
#define AT91_PERIPH_ID_SFRBU       77
#define AT91_PERIPH_ID_CHIPID      78

#endif
