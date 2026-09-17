/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for the Infineon PMB8876 (S-Gold2) DIFv2 display
 * interface.
 *
 * Offsets are relative to the module base (0xF7100000 on PMB8876).
 */
#ifndef _PMB887X_DIF_REGS_H
#define _PMB887X_DIF_REGS_H

#include <linux/bits.h>

/* Clock Control Register */
#define DIF_CLC				0x00
#define DIF_CLC_DISR			BIT(0)
#define DIF_CLC_DISS			BIT(1)
#define DIF_CLC_RMC			GENMASK(15, 8)

/* Identification Register */
#define DIF_ID				0x08
#define DIF_ID_MODULE			GENMASK(31, 8)
#define DIF_ID_MODULE_DIFV2		0xf043c0

/* Run Control Register */
#define DIF_RUNCTRL			0x10
#define DIF_RUNCTRL_RUN			BIT(0)

/* Control Register (serial mode) */
#define DIF_CON				0x20
#define DIF_CON_TRI			GENMASK(1, 0)
#define DIF_CON_HB			BIT(4)	/* heading bit: 0 = LSB, 1 = MSB */
#define DIF_CON_PH			BIT(5)	/* CPHA */
#define DIF_CON_PO			BIT(6)	/* CPOL */
#define DIF_CON_LB			BIT(7)	/* loop-back, serial only */
#define DIF_CON_BM			GENMASK(20, 16)	/* data width - 1 */

/* Peripheral Function Register */
#define DIF_PERREG			0x24
#define DIF_PERREG_DIFPERMODE		BIT(0)	/* 0 = serial, 1 = parallel */
#define DIF_PERREG_INBAND		BIT(1)
#define DIF_PERREG_CS1POL		BIT(2)
#define DIF_PERREG_CS2POL		BIT(3)
#define DIF_PERREG_RDPOL		BIT(4)
#define DIF_PERREG_WRPOL		BIT(5)
#define DIF_PERREG_CDPOL		BIT(6)
#define DIF_PERREG_CS3POL		BIT(7)

/*
 * Chip Select and Data Configuration Register.
 *
 * This register is latched into a side FIFO on every DIF_TXD write, so the
 * CD/CS/BSCONF of a queued word is the value that was live when the word was
 * pushed. That makes it safe to write while RUNCTRL.RUN is set, and lets a
 * command be pipelined directly into the pixel data that follows it.
 */
#define DIF_CSREG			0x28
#define DIF_CSREG_CD			BIT(0)	/* 1 = command, 0 = data */
#define DIF_CSREG_CS1			BIT(1)
#define DIF_CSREG_CS2			BIT(2)
#define DIF_CSREG_CS3			BIT(3)	/* no pin on PMB8876 */
#define DIF_CSREG_BSCONF		GENMASK(6, 4)
#define DIF_CSREG_BSCONF_OFF		0x0
#define DIF_CSREG_BSCONF_1X8BIT		0x10
#define DIF_CSREG_BSCONF_1X9BIT		0x20
#define DIF_CSREG_BSCONF_2X8BIT		0x30
#define DIF_CSREG_BSCONF_2X9BIT		0x40
#define DIF_CSREG_BSCONF_3X8BIT		0x50
#define DIF_CSREG_BSCONF_3X9BIT		0x60
#define DIF_CSREG_BSCONF_4X8BIT		0x70
#define DIF_CSREG_GRACMD		BIT(7)	/* enable colour matrix */

/* LCD Timing Register 1 */
#define DIF_LCDTIM1			0x2c
#define DIF_LCDTIM1_ADDRDELAY		GENMASK(6, 0)
#define DIF_LCDTIM1_ACCESSCYCLE		GENMASK(14, 8)
#define DIF_LCDTIM1_DATADELAY		GENMASK(22, 16)

/* LCD Timing Register 2 */
#define DIF_LCDTIM2			0x30
#define DIF_LCDTIM2_CSACT		GENMASK(6, 0)
#define DIF_LCDTIM2_CSDEACT		GENMASK(14, 8)
#define DIF_LCDTIM2_WRRDACT		GENMASK(22, 16)
#define DIF_LCDTIM2_WRRDDEACT		GENMASK(30, 24)

/* Start LCD Read Register */
#define DIF_STARTLCDRD			0x34
#define DIF_STARTLCDRD_STARTREAD	BIT(0)
#define DIF_STARTLCDRD_READBYTES	GENMASK(15, 1)	/* byte count - 1 */

/* DIF Status Register */
#define DIF_STAT			0x38
#define DIF_STAT_BSY			BIT(0)
#define DIF_STAT_GRABSY			BIT(1)
#define DIF_STAT_DSIFULL		BIT(2)
#define DIF_STAT_DSIDIR			BIT(3)
#define DIF_STAT_DSILOCK		BIT(4)

/* Colour matrix coefficients, signed 10-bit Q7 */
#define DIF_COEFF_REG1			0x3c
#define DIF_COEFF_REG2			0x40
#define DIF_COEFF_REG3			0x44
#define DIF_COEFF_COEFF0		GENMASK(9, 0)
#define DIF_COEFF_COEFF1		GENMASK(19, 10)
#define DIF_COEFF_COEFF2		GENMASK(29, 20)

/* Colour matrix input offsets, subtracted before multiplication */
#define DIF_OFFSET			0x48
#define DIF_OFFSET_OFF0			GENMASK(9, 0)
#define DIF_OFFSET_OFF1			GENMASK(19, 10)
#define DIF_OFFSET_OFF2			GENMASK(29, 20)

/* Pixel-Bit Conversion Register */
#define DIF_PBCCON			0x4c
/* Pair consecutive 16-bit word slots into one 32-bit bit-mux input. */
#define DIF_PBCCON_PBBCONV_MODE		BIT(0)

/*
 * Bit Multiplex Configuration Registers: a full 32x32 crossbar. Each output
 * bit has a 5-bit selector naming the input bit that feeds it, six selectors
 * per register at shifts 0, 5, 10, 16, 21 and 26 (note the gap at bit 15).
 *
 * The crossbar is bypassed to identity for command words (DIF_CSREG_CD set),
 * which is what lets a pixel-format swizzle stay programmed permanently.
 */
#define DIF_BMREG(n)			(0x50 + (n) * 4)
#define DIF_BMREG_COUNT			6
#define DIF_BMREG_MUX_PER_REG		6
#define DIF_BMREG_MUX_MASK		GENMASK(4, 0)

/* Identity (reset) configuration of the bit multiplexer. */
#define DIF_BMREG0_IDENTITY		0x14830820
#define DIF_BMREG1_IDENTITY		0x2d4920e6
#define DIF_BMREG2_IDENTITY		0x460f39ac
#define DIF_BMREG3_IDENTITY		0x5ed55272
#define DIF_BMREG4_IDENTITY		0x779b6b38
#define DIF_BMREG5_IDENTITY		0x000003fe

/*
 * Per-output-bit source select, 2 bits each: 0 = bit multiplexer, 1 = the
 * matching DIF_BCREG constant, 2 and 3 = constant zero.
 *
 * Unlike the bit multiplexer these are NOT bypassed for command words, so
 * they must be left at zero when a pixel format swizzle is in use.
 */
#define DIF_BCSEL(n)			(0x68 + (n) * 4)
#define DIF_BCSEL_COUNT			2
#define DIF_BCREG			0x70
#define DIF_INVERT_BIT			0x74

/*
 * Transfer synchronisation. Unvalidated on this SoC: DIF_HD is pinmuxed to
 * SCU EXTI1 rather than into the DIF, so tearing effect is handled as a
 * plain GPIO interrupt instead.
 */
#define DIF_SYNC_CONFIG			0x78
#define DIF_SYNC_CONFIG_SYNCEN		BIT(0)
#define DIF_SYNC_CONFIG_HDPOL		BIT(1)
#define DIF_SYNC_CONFIG_VDPOL		BIT(2)
#define DIF_SYNC_CONFIG_SYNCCD		BIT(3)
#define DIF_SYNC_CONFIG_SYNCCS1		BIT(4)
#define DIF_SYNC_CONFIG_SYNCCS2		BIT(5)
#define DIF_SYNC_CONFIG_SYNCCS3		BIT(6)
#define DIF_SYNC_CONFIG_EXTSTART	GENMASK(9, 8)
#define DIF_SYNC_CONFIG_EXTBYTES	GENMASK(11, 10)
#define DIF_SYNC_CONFIG_EXTROWS		GENMASK(13, 12)
#define DIF_SYNC_CONFIG_COMP		GENMASK(23, 16)

#define DIF_SYNC_COUNT			0x7c
#define DIF_SYNC_COUNT_HDSTART		GENMASK(9, 0)
#define DIF_SYNC_COUNT_NUMBYTES		GENMASK(21, 10)
#define DIF_SYNC_COUNT_NUMROWS		GENMASK(31, 22)

/* Baud rate timer reload and fractional divider (serial mode) */
#define DIF_BR				0x80
#define DIF_BR_VALUE			GENMASK(15, 0)
#define DIF_FDIV			0x84
#define DIF_FDIV_VALUE			GENMASK(8, 0)

#define DIF_DEBUG			0x8c

/* RX FIFO Configuration Register */
#define DIF_RXFIFO_CFG			0x90
#define DIF_RXFIFO_CFG_RXBS		GENMASK(2, 0)
#define DIF_RXFIFO_CFG_RXFA		GENMASK(9, 8)
#define DIF_RXFIFO_CFG_RXFC		BIT(16)

/* Maximum Received Packet Size Control Register. Write-only: reads fault. */
#define DIF_MRPS_CTRL			0x94
#define DIF_MRPS_CTRL_MRPS		GENMASK(13, 0)

/* Received Packet Size Status Register */
#define DIF_RPS_STAT			0x98
#define DIF_RPS_STAT_RPS		GENMASK(13, 0)

/* Filled RX FIFO Stages Status Register */
#define DIF_RXFFS_STAT			0x9c

/* TX FIFO Configuration Register */
#define DIF_TXFIFO_CFG			0xa0
#define DIF_TXFIFO_CFG_TXBS		GENMASK(2, 0)
#define DIF_TXFIFO_CFG_TXFA		GENMASK(9, 8)
#define DIF_TXFIFO_CFG_TXFC		BIT(16)

/* Burst size encoding, shared by DIF_RXFIFO_CFG_RXBS and DIF_TXFIFO_CFG_TXBS */
#define DIF_FIFO_BS_1_WORD		0x0
#define DIF_FIFO_BS_2_WORD		0x1
#define DIF_FIFO_BS_4_WORD		0x2
#define DIF_FIFO_BS_8_WORD		0x3
#define DIF_FIFO_BS_16_WORD		0x4
#define DIF_FIFO_BS_32_WORD		0x5
#define DIF_FIFO_BS_64_WORD		0x6
#define DIF_FIFO_BS_128_WORD		0x7

/* Alignment encoding, in bytes per bus word */
#define DIF_FIFO_FA_1			0x0
#define DIF_FIFO_FA_2			0x1
#define DIF_FIFO_FA_4			0x2

/* Both FIFOs are 16 stages of 32 bits. */
#define DIF_FIFO_DEPTH			16

/* Transmit Packet Size Register. Writing a non-zero value starts a packet. */
#define DIF_TPS_CTRL			0xa4
#define DIF_TPS_CTRL_TPS		GENMASK(13, 0)
#define DIF_TPS_CTRL_MAX		16383

/* Filled TX FIFO Stages Status Register */
#define DIF_TXFFS_STAT			0xa8

/*
 * Error interrupt source mask, status and clear. The status register reads
 * back already masked, and the mask survives a CLC disable, so it has to be
 * programmed explicitly rather than assumed.
 */
#define DIF_ERRIRQSM			0xb0
#define DIF_ERRIRQSS			0xb4
#define DIF_ERRIRQSC			0xb8
#define DIF_ERRIRQ_RXFUFL		BIT(0)	/* fatal: aborts the transfer */
#define DIF_ERRIRQ_RXFOFL		BIT(1)	/* fatal */
#define DIF_ERRIRQ_TXFOFL		BIT(2)	/* fatal */
#define DIF_ERRIRQ_PHASE		BIT(3)
#define DIF_ERRIRQ_CMD			BIT(4)
#define DIF_ERRIRQ_MASTER		BIT(5)
#define DIF_ERRIRQ_TXUFL		BIT(11)
#define DIF_ERRIRQ_MASTER2		BIT(12)
#define DIF_ERRIRQ_IDLE			BIT(13)

/*
 * Interrupt raw status, mask, masked status, clear (W1C) and set (W1S), plus
 * the DMA request enable. All share one bit layout.
 */
#define DIF_RIS				0xc0
#define DIF_IMSC			0xc4
#define DIF_MIS				0xc8
#define DIF_ICR				0xcc
#define DIF_ISR				0xd0
#define DIF_DMAE			0xd4
#define DIF_IRQ_RXLSREQ			BIT(0)
#define DIF_IRQ_RXSREQ			BIT(1)
#define DIF_IRQ_RXLBREQ			BIT(2)
#define DIF_IRQ_RXBREQ			BIT(3)
#define DIF_IRQ_TXLSREQ			BIT(4)
#define DIF_IRQ_TXSREQ			BIT(5)
#define DIF_IRQ_TXLBREQ			BIT(6)
#define DIF_IRQ_TXBREQ			BIT(7)
#define DIF_IRQ_ERR			BIT(8)
#define DIF_IRQ_CMD			BIT(9)	/* never raised */
#define DIF_IRQ_FRAME			BIT(10)	/* never raised */

/*
 * FIFO data ports. Each is decoded over a window, but the DIF expects a
 * fixed address from the DMA controller, so only the base is ever used.
 */
#define DIF_TXD				0x8000
#define DIF_RXD				0xc000

#define DIF_IO_SIZE			0xc004

#endif /* _PMB887X_DIF_REGS_H */
