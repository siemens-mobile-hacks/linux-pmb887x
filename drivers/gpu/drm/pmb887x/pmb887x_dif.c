// SPDX-License-Identifier: GPL-2.0
/*
 * Transport layer for the Infineon PMB8876 (S-Gold2) DIFv2 display
 * interface in MCU-parallel mode.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>

#include "pmb887x_dif.h"
#include "pmb887x_dif_regs.h"

/*
 * Bus timings that the bare-metal pmb887x-emu driver uses for writes on both
 * a Siemens EL71 and the KE970 R61505: LCDTIM1 ACCESSCYCLE = 4, LCDTIM2
 * CSDEACT = 4 and WRRDACT = 2. Reads need a much longer /RD cycle, but this
 * driver never reads.
 */
#define PMB887X_DIF_LCDTIM1_WRITE	0x00000400
#define PMB887X_DIF_LCDTIM2_WRITE	0x02000400

#define PMB887X_DIF_BUSY_TIMEOUT_US	100000
#define PMB887X_DIF_DMA_TIMEOUT_MS	1000

/*
 * Burst sizes copied from the stock firmware, which programs CONTROL with
 * SBSIZE = 16 and DBSIZE = 8. The destination burst has to match the DIF's own
 * TXBS threshold: the peripheral holds its request asserted until the
 * controller has moved a full burst, so a controller that moves fewer items
 * leaves the request latched and the transfer stalls.
 */
#define PMB887X_DIF_DMA_SRC_BURST	16
#define PMB887X_DIF_DMA_DST_BURST	8

/* Below this a DMA round trip costs more than just filling the FIFO. */
#define PMB887X_DIF_DMA_MIN_WORDS	64

/* The only sources the hardware ever raises in parallel mode, all fatal. */
#define PMB887X_DIF_ERR_FATAL \
	(DIF_ERRIRQ_RXFUFL | DIF_ERRIRQ_RXFOFL | DIF_ERRIRQ_TXFOFL)

#define PMB887X_DIF_ERR_ALL \
	(PMB887X_DIF_ERR_FATAL | DIF_ERRIRQ_PHASE | DIF_ERRIRQ_CMD | \
	 DIF_ERRIRQ_MASTER | DIF_ERRIRQ_TXUFL | DIF_ERRIRQ_MASTER2 | \
	 DIF_ERRIRQ_IDLE)

/* Six bit-multiplexer selectors per register, with a gap at bit 15. */
static const u8 pmb887x_dif_bm_shift[DIF_BMREG_MUX_PER_REG] = {
	0, 5, 10, 16, 21, 26,
};

/*
 * RGB565 is the native format. One 32-bit framebuffer word holds two
 * little-endian pixels, and the panel wants each pixel most significant byte
 * first, so the two bytes of each 16-bit half are swapped. With TXFA_4 and
 * BSCONF_4x8BIT the four output bytes leave the chip in bit order 0..7,
 * 8..15, 16..23, 24..31, i.e. two pixels per stage.
 */
static const struct pmb887x_dif_format pmb887x_dif_format_rgb565 = {
	.fourcc = DRM_FORMAT_RGB565,
	.bsconf = DIF_CSREG_BSCONF_4X8BIT,
	.pixels_per_word = 2,
	.bitmux = {
		 8,  9, 10, 11, 12, 13, 14, 15,
		 0,  1,  2,  3,  4,  5,  6,  7,
		24, 25, 26, 27, 28, 29, 30, 31,
		16, 17, 18, 19, 20, 21, 22, 23,
	},
};

/*
 * XRGB8888 holds one pixel per 32-bit word as B[7:0] G[15:8] R[23:16], and
 * BSCONF_2x8BIT emits two bus bytes from output bits 0..7 and 8..15. The
 * RGB565 word the panel expects is
 *
 *	R7..R3 G7..G2 B7..B3   (bit 15 down to bit 0)
 *
 * so its high byte - the first byte on the wire, hence output bits 0..7 -
 * is G[7:5] then R[7:3], and its low byte is B[7:3] then G[4:2]. Output bits
 * 16..31 are never emitted and are left mapped to themselves.
 */
static const struct pmb887x_dif_format pmb887x_dif_format_xrgb8888 = {
	.fourcc = DRM_FORMAT_XRGB8888,
	.bsconf = DIF_CSREG_BSCONF_2X8BIT,
	.pixels_per_word = 1,
	.bitmux = {
		13, 14, 15, 19, 20, 21, 22, 23,
		 3,  4,  5,  6,  7, 10, 11, 12,
		16, 17, 18, 19, 20, 21, 22, 23,
		24, 25, 26, 27, 28, 29, 30, 31,
	},
};

static const struct pmb887x_dif_format * const pmb887x_dif_formats[] = {
	&pmb887x_dif_format_rgb565,
	&pmb887x_dif_format_xrgb8888,
};

const struct pmb887x_dif_format *pmb887x_dif_find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pmb887x_dif_formats); i++)
		if (pmb887x_dif_formats[i]->fourcc == fourcc)
			return pmb887x_dif_formats[i];

	return NULL;
}

static void pmb887x_dif_identity_map(u8 *map)
{
	unsigned int i;

	for (i = 0; i < PMB887X_DIF_BITMUX_BITS; i++)
		map[i] = i;
}

static void pmb887x_dif_pack_bitmux(const u8 *map, u32 *regs)
{
	unsigned int i;

	memset(regs, 0, DIF_BMREG_COUNT * sizeof(*regs));

	for (i = 0; i < PMB887X_DIF_BITMUX_BITS; i++)
		regs[i / DIF_BMREG_MUX_PER_REG] |=
			((u32)map[i] & DIF_BMREG_MUX_MASK)
			<< pmb887x_dif_bm_shift[i % DIF_BMREG_MUX_PER_REG];
}

/*
 * The identity mapping has a documented, hardware-verified register encoding.
 * Reproducing it from the packing helper proves the selector layout - and in
 * particular the gap at bit 15 - is right before any pixel is sent.
 */
int pmb887x_dif_bitmux_selftest(struct device *dev)
{
	static const u32 expected[DIF_BMREG_COUNT] = {
		DIF_BMREG0_IDENTITY, DIF_BMREG1_IDENTITY, DIF_BMREG2_IDENTITY,
		DIF_BMREG3_IDENTITY, DIF_BMREG4_IDENTITY, DIF_BMREG5_IDENTITY,
	};
	u8 identity[PMB887X_DIF_BITMUX_BITS];
	u32 regs[DIF_BMREG_COUNT];
	unsigned int i;

	pmb887x_dif_identity_map(identity);
	pmb887x_dif_pack_bitmux(identity, regs);

	for (i = 0; i < DIF_BMREG_COUNT; i++) {
		if (regs[i] != expected[i])
			return dev_err_probe(dev, -EINVAL,
					     "bit multiplexer packing is wrong: BMREG%u %#010x, expected %#010x\n",
					     i, regs[i], expected[i]);
	}

	return 0;
}

static void pmb887x_dif_program_bitmux(struct pmb887x_dif *dif, const u8 *map)
{
	u32 regs[DIF_BMREG_COUNT];
	unsigned int i;

	pmb887x_dif_pack_bitmux(map, regs);

	for (i = 0; i < DIF_BMREG_COUNT; i++)
		writel(regs[i], dif->base + DIF_BMREG(i));
}

int pmb887x_dif_sync(struct pmb887x_dif *dif)
{
	u32 stat;
	int ret;

	/*
	 * STAT.BSY is the only reliable completion indicator: TXFFS_STAT can
	 * read zero while the last word is still going out on the bus.
	 */
	ret = readl_poll_timeout(dif->base + DIF_STAT, stat,
				 !(stat & DIF_STAT_BSY), 10,
				 PMB887X_DIF_BUSY_TIMEOUT_US);
	if (ret)
		drm_err_ratelimited(&dif->drm, "timed out waiting for the bus to drain\n");

	return ret;
}

static int pmb887x_dif_fifo_space(struct pmb887x_dif *dif, unsigned int *space)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);
	u32 filled;

	for (;;) {
		filled = readl_relaxed(dif->base + DIF_TXFFS_STAT);
		if (filled < DIF_FIFO_DEPTH) {
			*space = DIF_FIFO_DEPTH - filled;
			return 0;
		}

		if (time_after(jiffies, timeout)) {
			drm_err_ratelimited(&dif->drm, "TX FIFO never drained\n");
			return -ETIMEDOUT;
		}

		cpu_relax();
	}
}

static int pmb887x_dif_push(struct pmb887x_dif *dif, u32 csreg, const u8 *buf,
			    size_t len)
{
	unsigned int space = 0;
	size_t i;
	int ret;

	writel(csreg, dif->base + DIF_CSREG);

	for (i = 0; i < len; i++) {
		if (!space) {
			ret = pmb887x_dif_fifo_space(dif, &space);
			if (ret)
				return ret;
		}

		writel_relaxed(buf[i], dif->base + DIF_TXD);
		space--;
	}

	return 0;
}

/**
 * pmb887x_dif_write - send a command and its parameters to the panel
 * @dif: display interface
 * @cmd: command bytes, sent with CD asserted
 * @ncmd: number of command bytes
 * @par: parameter bytes, sent with CD deasserted, may be NULL
 * @npar: number of parameter bytes
 *
 * Both phases are queued back to back: CSREG is latched into a side FIFO on
 * every TXD write, so the parameter bytes carry their own CD without the bus
 * having to drain in between.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_write(struct pmb887x_dif *dif, const u8 *cmd, size_t ncmd,
		      const u8 *par, size_t npar)
{
	int ret;

	lockdep_assert_held(&dif->lock);

	/*
	 * Only the command bytes bypass the crossbar. The parameters are data
	 * words, so a pixel format left programmed by the preceding blit would
	 * swizzle every one of them - and since a parameter byte occupies the
	 * low stage alone, under either pixel map it would leave the chip as
	 * zero. Idling the crossbar here rather than at the call sites is what
	 * keeps that from depending on which path last touched the bus.
	 */
	ret = pmb887x_dif_set_format(dif, NULL);
	if (ret)
		return ret;

	ret = pmb887x_dif_push(dif, dif->cs | DIF_CSREG_CD |
			       DIF_CSREG_BSCONF_1X8BIT, cmd, ncmd);
	if (ret || !npar)
		return ret;

	return pmb887x_dif_push(dif, dif->cs | DIF_CSREG_BSCONF_1X8BIT,
				par, npar);
}

/**
 * pmb887x_dif_set_format - point the bit multiplexer at a framebuffer format
 * @dif: display interface
 * @fmt: format description, from pmb887x_dif_find_format()
 *
 * The crossbar is a configuration register, so programming it needs a RUNCTRL
 * cycle, which also empties the FIFOs - nothing may be queued across the call.
 *
 * Only command words bypass the crossbar; the parameters that follow them are
 * data words and would be swizzled like pixels. A pixel format therefore
 * cannot stay programmed across register writes, so @fmt is NULL for control
 * traffic and set only for the pixel stream itself.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_set_format(struct pmb887x_dif *dif,
			   const struct pmb887x_dif_format *fmt)
{
	u8 identity[PMB887X_DIF_BITMUX_BITS];
	const u8 *map;
	int ret;

	lockdep_assert_held(&dif->lock);

	if (dif->format == fmt)
		return 0;

	ret = pmb887x_dif_sync(dif);
	if (ret)
		return ret;

	if (fmt) {
		map = fmt->bitmux;
	} else {
		pmb887x_dif_identity_map(identity);
		map = identity;
	}

	writel(0, dif->base + DIF_RUNCTRL);
	pmb887x_dif_program_bitmux(dif, map);
	writel(DIF_RUNCTRL_RUN, dif->base + DIF_RUNCTRL);

	dif->format = fmt;

	return 0;
}

static int pmb887x_dif_blit_pio(struct pmb887x_dif *dif, const void *vaddr,
				const struct pmb887x_dif_span *spans,
				unsigned int nspans)
{
	unsigned int space = 0;
	unsigned int i, j;
	int ret;

	for (i = 0; i < nspans; i++) {
		const u32 *src = vaddr + spans[i].offset;
		unsigned int words = spans[i].len / sizeof(u32);

		for (j = 0; j < words; j++) {
			if (!space) {
				ret = pmb887x_dif_fifo_space(dif, &space);
				if (ret)
					return ret;
			}

			writel_relaxed(src[j], dif->base + DIF_TXD);
			space--;
		}
	}

	return pmb887x_dif_sync(dif);
}

static void pmb887x_dif_dma_done(void *data)
{
	struct pmb887x_dif *dif = data;

	complete(&dif->tx_done);
}

static int pmb887x_dif_blit_dma(struct pmb887x_dif *dif, dma_addr_t dma,
				const struct pmb887x_dif_span *spans,
				unsigned int nspans)
{
	struct dma_async_tx_descriptor *desc;
	struct scatterlist *sg;
	dma_cookie_t cookie;
	unsigned int i;
	int ret;

	sg = dif->sgt.sgl;
	for (i = 0; i < nspans; i++) {
		sg_dma_address(sg) = dma + spans[i].offset;
		sg_dma_len(sg) = spans[i].len;
		sg = sg_next(sg);
	}

	desc = dmaengine_prep_slave_sg(dif->tx_chan, dif->sgt.sgl, nspans,
				       DMA_MEM_TO_DEV,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;

	desc->callback = pmb887x_dif_dma_done;
	desc->callback_param = dif;

	reinit_completion(&dif->tx_done);

	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret)
		return ret;

	dma_async_issue_pending(dif->tx_chan);

	/*
	 * Enabling DMA requests is what actually starts the transfer: the DIF
	 * latched TXBREQ when RUNCTRL went high on an empty FIFO, and the
	 * request line is gated by DMAE alone. The pending request must not be
	 * cleared here - the stock firmware never writes ICR - and the software
	 * seed that follows is only insurance for the case where it was not
	 * already raised.
	 */
	/*
	 * The request has to be unmasked in IMSC as well as enabled in DMAE.
	 * The emulator gates the request on DMAE alone, but on silicon a
	 * request masked off in IMSC never reaches the controller: every DMA
	 * path that works on hardware - the stock firmware and each passing
	 * bare-metal test - programs both, and the one that programs only DMAE
	 * never reaches terminal count. The line stays masked at the interrupt
	 * controller, so unmasking it here costs nothing.
	 */
	writel(DIF_IRQ_ERR | DIF_IRQ_TXBREQ, dif->base + DIF_IMSC);
	writel(DIF_IRQ_TXBREQ, dif->base + DIF_DMAE);
	writel(DIF_IRQ_TXBREQ, dif->base + DIF_ISR);

	if (!wait_for_completion_timeout(&dif->tx_done,
					 msecs_to_jiffies(PMB887X_DIF_DMA_TIMEOUT_MS))) {
		writel(0, dif->base + DIF_DMAE);
		writel(DIF_IRQ_ERR, dif->base + DIF_IMSC);
		dmaengine_terminate_sync(dif->tx_chan);
		drm_err_ratelimited(&dif->drm, "pixel DMA timed out\n");
		return -ETIMEDOUT;
	}

	writel(0, dif->base + DIF_DMAE);
	writel(DIF_IRQ_ERR, dif->base + DIF_IMSC);

	return pmb887x_dif_sync(dif);
}

/**
 * pmb887x_dif_blit - queue a GRAM write command and stream pixels after it
 * @dif: display interface
 * @cmd: GRAM write command bytes, sent with CD asserted
 * @ncmd: number of command bytes
 * @fmt: framebuffer format the crossbar is programmed for
 * @dma: DMA address of the framebuffer
 * @vaddr: CPU mapping of the framebuffer, may be NULL if DMA is available
 * @spans: framebuffer byte runs to send, in wire order
 * @nspans: number of spans
 *
 * The command and the pixel data are queued back to back without draining the
 * bus in between; only the CSREG that each word was pushed with distinguishes
 * them.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_blit(struct pmb887x_dif *dif, const u8 *cmd, size_t ncmd,
		     const struct pmb887x_dif_format *fmt, dma_addr_t dma,
		     const void *vaddr, const struct pmb887x_dif_span *spans,
		     unsigned int nspans)
{
	unsigned int words = 0;
	unsigned int i;
	int ret;

	lockdep_assert_held(&dif->lock);

	if (WARN_ON(!fmt))
		return -EINVAL;

	for (i = 0; i < nspans; i++)
		words += spans[i].len / sizeof(u32);
	if (!words)
		return 0;

	/*
	 * The swizzle has to be in place before anything is queued: programming
	 * it cycles RUNCTRL, which would discard the GRAM write command. The
	 * command itself is unaffected by it, being a command word.
	 */
	ret = pmb887x_dif_set_format(dif, fmt);
	if (ret)
		return ret;

	ret = pmb887x_dif_push(dif, dif->cs | DIF_CSREG_CD |
			       DIF_CSREG_BSCONF_1X8BIT, cmd, ncmd);
	if (ret)
		return ret;

	/* Switch to pixel data; already queued words keep their latched CSREG. */
	writel(dif->cs | fmt->bsconf, dif->base + DIF_CSREG);

	if (dif->tx_chan && words >= PMB887X_DIF_DMA_MIN_WORDS) {
		/*
		 * Let the command drain, then take RUNCTRL down and back up
		 * before arming the controller. The rising edge resets the TX
		 * request state machine, so a request left latched by the
		 * previous transfer cannot suppress this one - the stock
		 * firmware cycles it around every single DMA transfer.
		 */
		ret = pmb887x_dif_sync(dif);
		if (ret)
			return ret;

		writel(0, dif->base + DIF_RUNCTRL);
		writel(FIELD_PREP(DIF_TXFIFO_CFG_TXBS, DIF_FIFO_BS_8_WORD) |
		       FIELD_PREP(DIF_TXFIFO_CFG_TXFA, DIF_FIFO_FA_4),
		       dif->base + DIF_TXFIFO_CFG);
		writel(DIF_RUNCTRL_RUN, dif->base + DIF_RUNCTRL);

		ret = pmb887x_dif_blit_dma(dif, dma, spans, nspans);
	} else if (vaddr) {
		ret = pmb887x_dif_blit_pio(dif, vaddr, spans, nspans);
	} else {
		ret = -ENOMEM;
	}

	if (ret) {
		/*
		 * A half-sent GRAM write would desynchronise the panel's
		 * command state machine. Clearing RUN aborts the transfer and
		 * flushes both FIFOs; the configuration registers, including
		 * the bit multiplexer, survive it.
		 */
		writel(0, dif->base + DIF_RUNCTRL);
		writel(DIF_RUNCTRL_RUN, dif->base + DIF_RUNCTRL);
	}

	return ret;
}

/**
 * pmb887x_dif_hw_init - bring the parallel interface up
 * @dif: display interface
 *
 * Every configuration register has to be written with RUNCTRL.RUN clear, so
 * this is the only place that touches them outside of a format change.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_hw_init(struct pmb887x_dif *dif)
{
	u8 identity[PMB887X_DIF_BITMUX_BITS];
	u32 txfifo, id;

	id = FIELD_GET(DIF_ID_MODULE, readl(dif->base + DIF_ID));
	if (id != DIF_ID_MODULE_DIFV2)
		return dev_err_probe(dif->dev, -ENODEV,
				     "not a DIFv2 module (ID %#x)\n", id);

	txfifo = FIELD_PREP(DIF_TXFIFO_CFG_TXBS, DIF_FIFO_BS_8_WORD) |
		 FIELD_PREP(DIF_TXFIFO_CFG_TXFA, DIF_FIFO_FA_4);
	/*
	 * Continuous DMA needs flow control off: with no packet armed the DIF
	 * only keeps TXBREQ asserted from FIFO occupancy when TXFC is clear.
	 * The polled path ignores requests entirely, so it keeps the flow
	 * control the bare-metal driver uses.
	 */
	if (!dif->tx_chan)
		txfifo |= DIF_TXFIFO_CFG_TXFC;

	writel(0, dif->base + DIF_RUNCTRL);

	writel(0, dif->base + DIF_CON);
	/* Parallel mode, all polarity bits zero, i.e. active-low strobes. */
	writel(DIF_PERREG_DIFPERMODE, dif->base + DIF_PERREG);
	writel(txfifo, dif->base + DIF_TXFIFO_CFG);
	writel(FIELD_PREP(DIF_RXFIFO_CFG_RXBS, DIF_FIFO_BS_4_WORD) |
	       FIELD_PREP(DIF_RXFIFO_CFG_RXFA, DIF_FIFO_FA_4) |
	       DIF_RXFIFO_CFG_RXFC, dif->base + DIF_RXFIFO_CFG);
	writel(PMB887X_DIF_LCDTIM1_WRITE, dif->base + DIF_LCDTIM1);
	writel(PMB887X_DIF_LCDTIM2_WRITE, dif->base + DIF_LCDTIM2);
	writel(dif->cs | DIF_CSREG_BSCONF_1X8BIT, dif->base + DIF_CSREG);

	/*
	 * Only the bit multiplexer may be used for format conversion: it is
	 * the one conversion stage that command words bypass. BCSEL, BCREG
	 * and INVERT_BIT would corrupt commands, and the pixel-bit-conversion
	 * pairing is unnecessary because DMA always delivers whole stages.
	 */
	writel(0, dif->base + DIF_PBCCON);
	writel(0, dif->base + DIF_BCSEL(0));
	writel(0, dif->base + DIF_BCSEL(1));
	writel(0, dif->base + DIF_BCREG);
	writel(0, dif->base + DIF_INVERT_BIT);
	writel(0, dif->base + DIF_COEFF_REG1);
	writel(0, dif->base + DIF_COEFF_REG2);
	writel(0, dif->base + DIF_COEFF_REG3);
	writel(0, dif->base + DIF_OFFSET);

	/*
	 * The crossbar keeps whatever the boot loader left in it, so program
	 * the identity map rather than assuming it: dif->format tracks what is
	 * in these registers and has to start out telling the truth.
	 */
	pmb887x_dif_identity_map(identity);
	pmb887x_dif_program_bitmux(dif, identity);
	dif->format = NULL;

	/* The error mask survives a CLC disable, so program it explicitly. */
	writel(PMB887X_DIF_ERR_FATAL, dif->base + DIF_ERRIRQSM);
	writel(PMB887X_DIF_ERR_ALL, dif->base + DIF_ERRIRQSC);

	/*
	 * DMA requests come from RIS & DMAE and do not need the matching IMSC
	 * bit, so only the error interrupt is routed to the interrupt
	 * controller. Unmasking TXBREQ would keep IRQ 136 asserted for as long
	 * as the FIFO has room, and nothing handles that line.
	 */
	writel(GENMASK(8, 0), dif->base + DIF_ICR);
	writel(DIF_IRQ_ERR, dif->base + DIF_IMSC);
	/*
	 * DMA requests stay disabled until a transfer is actually armed. In
	 * continuous mode the DIF raises TXBREQ as soon as the FIFO has room,
	 * and a request raised with no channel to service it is latched and
	 * never raised again, which would wedge the first blit.
	 */
	writel(0, dif->base + DIF_DMAE);

	writel(DIF_RUNCTRL_RUN, dif->base + DIF_RUNCTRL);

	return 0;
}

void pmb887x_dif_hw_stop(struct pmb887x_dif *dif)
{
	writel(0, dif->base + DIF_DMAE);
	writel(0, dif->base + DIF_IMSC);
	writel(0, dif->base + DIF_RUNCTRL);
}

/*
 * Never expected to run: the line is claimed only so that it stays masked, and
 * the DMA controller is what clears the request that raises it.
 */
static irqreturn_t pmb887x_dif_tx_irq(int irq, void *data)
{
	struct pmb887x_dif *dif = data;

	/* Mask the source rather than the request: the transfer must survive. */
	writel(DIF_IRQ_ERR, dif->base + DIF_IMSC);

	return IRQ_HANDLED;
}

static irqreturn_t pmb887x_dif_err_irq(int irq, void *data)
{
	struct pmb887x_dif *dif = data;
	u32 status;

	status = readl(dif->base + DIF_ERRIRQSS);
	if (!status)
		return IRQ_NONE;

	writel(status, dif->base + DIF_ERRIRQSC);
	writel(DIF_IRQ_ERR, dif->base + DIF_ICR);

	/*
	 * A FIFO underflow or overflow resets the transfer engine, so the
	 * transaction in flight is already lost; its own timeout will report
	 * the failure. Only record why it happened.
	 */
	drm_err_ratelimited(&dif->drm, "transfer aborted, error status %#x\n",
			    status);

	return IRQ_HANDLED;
}

static void pmb887x_dif_release_dma(void *data)
{
	struct pmb887x_dif *dif = data;

	dma_release_channel(dif->tx_chan);
	dif->tx_chan = NULL;
}

static int pmb887x_dif_request_dma(struct pmb887x_dif *dif)
{
	struct dma_slave_config cfg = {
		.direction = DMA_MEM_TO_DEV,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.src_maxburst = PMB887X_DIF_DMA_SRC_BURST,
		.dst_addr = dif->txd_phys,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_maxburst = PMB887X_DIF_DMA_DST_BURST,
	};
	struct dma_chan *chan;
	int ret;

	/*
	 * There is no "dmas" phandle: the channel is matched through the
	 * platform dma_slave_map. DMA is an optimisation, so anything other
	 * than a probe deferral just leaves the polled path in charge.
	 */
	chan = dma_request_chan(dif->dev, "tx");
	if (IS_ERR(chan)) {
		ret = PTR_ERR(chan);
		if (ret == -EPROBE_DEFER)
			return ret;

		dev_info(dif->dev, "no TX DMA channel (%d), using polled transfers\n",
			 ret);
		return 0;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dma_release_channel(chan);
		return dev_err_probe(dif->dev, ret, "failed to configure TX DMA\n");
	}

	dif->tx_chan = chan;

	return devm_add_action_or_reset(dif->dev, pmb887x_dif_release_dma, dif);
}

static void pmb887x_dif_free_sgt(void *data)
{
	sg_free_table(data);
}

static void pmb887x_dif_disable_clk(void *data)
{
	clk_disable_unprepare(data);
}

/**
 * pmb887x_dif_acquire - claim the interface's resources
 * @dif: display interface
 * @pdev: platform device backing it
 * @nspans: largest number of framebuffer runs a single blit can produce
 *
 * Nothing here touches the panel or the bus: the interface is left exactly
 * as the boot loader configured it until pmb887x_dif_hw_init() runs.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_acquire(struct pmb887x_dif *dif, struct platform_device *pdev,
			unsigned int nspans)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret, irq;

	dif->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dif->base))
		return PTR_ERR(dif->base);
	dif->txd_phys = res->start + DIF_TXD;

	dif->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dif->clk))
		return dev_err_probe(dev, PTR_ERR(dif->clk), "failed to get the module clock\n");

	ret = clk_prepare_enable(dif->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable the module clock\n");

	ret = devm_add_action_or_reset(dev, pmb887x_dif_disable_clk, dif->clk);
	if (ret)
		return ret;

	irq = platform_get_irq_byname(pdev, "err");
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, pmb887x_dif_err_irq, 0,
			       dev_name(dev), dif);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request the error interrupt\n");

	/*
	 * Arming a transfer unmasks TXBREQ in the DIF, which drives this line.
	 * The controller is serviced by the DMA controller rather than by the
	 * CPU, so claim the line only to hold it masked: nothing clears it at
	 * boot, and a level interrupt left enabled by the boot loader with no
	 * handler would livelock the machine.
	 */
	irq = platform_get_irq_byname(pdev, "tx");
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, pmb887x_dif_tx_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), dif);
	if (ret)
		return dev_err_probe(dev, ret, "failed to claim the transmit interrupt\n");

	ret = pmb887x_dif_request_dma(dif);
	if (ret)
		return ret;

	dif->spans = devm_kcalloc(dev, nspans, sizeof(*dif->spans), GFP_KERNEL);
	if (!dif->spans)
		return -ENOMEM;
	dif->span_max = nspans;

	if (dif->tx_chan) {
		ret = sg_alloc_table(&dif->sgt, nspans, GFP_KERNEL);
		if (ret)
			return ret;

		ret = devm_add_action_or_reset(dev, pmb887x_dif_free_sgt, &dif->sgt);
		if (ret)
			return ret;
	}

	return 0;
}
