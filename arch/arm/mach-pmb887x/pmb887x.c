// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/of.h>
#include <linux/of_platform.h>

#include <linux/amba/pl08x.h>
#include <linux/amba/pl080.h>
#include <linux/amba/mmci.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_misc.h>

#define PMB887X_IO_BASE		0xF0000000
#define PMB887X_IO_SIZE		0x0E000000

#define PMB887X_SCU_DMARS	0xF4400084
#define PMB887X_DMAC_BASE	0xF3000000
#define PMB887X_MMCI_BASE	0xF7301000

static spinlock_t pmb887x_dma_lock = __SPIN_LOCK_UNLOCKED(x);

/* MMCI */
static struct mmci_platform_data pmb887x_pl180_plat_data = {
	.ocr_mask	= MMC_VDD_29_30,
};

/* DMA */
static int pl08x_get_xfer_signal(const struct pl08x_channel_data *cd) {
	unsigned int signal = cd->min_signal, val;
	unsigned long flags;

	spin_lock_irqsave(&pmb887x_dma_lock, flags);
	val = readl((void *) PMB887X_SCU_DMARS);
	if (cd->muxval)
		val |= 1 << signal;
	else
		val &= ~(1 << signal);
	writel(val, (void *) PMB887X_SCU_DMARS);
	spin_unlock_irqrestore(&pmb887x_dma_lock, flags);

	return signal;
}

static void pl08x_put_xfer_signal(const struct pl08x_channel_data *cd, int signal) {
	/*
	 * Nothing to undo: the selection only matters while a transfer is set
	 * up, and every request programs it before use.
	 */
}

static struct pl08x_channel_data pmb887x_dma_info[] = {
	{
		.bus_id = "mmci0_tx",
		.min_signal = 13,
		.max_signal = 13,
		.muxval = 1,
		.periph_buses = PL08X_AHB2,
	}, 
	{
		.bus_id = "mmci0_rx",
		.min_signal = 6,
		.max_signal = 6,
		.muxval = 1,
		.periph_buses = PL08X_AHB2
	}
};

static const struct dma_slave_map pmb887x_dma_slave_map[] = {
	{ "pmb887x-mmc.0", "tx", &pmb887x_dma_info[0] },
	{ "pmb887x-mmc.0", "rx", &pmb887x_dma_info[1] },
};

struct pl08x_platform_data pmb887x_pl080_plat_data = {
	.lli_buses			= PL08X_AHB2,
	.mem_buses			= PL08X_AHB2,
	.slave_channels		= pmb887x_dma_info,
	.num_slave_channels	= ARRAY_SIZE(pmb887x_dma_info),
	.slave_map			= pmb887x_dma_slave_map,
	.slave_map_len		= ARRAY_SIZE(pmb887x_dma_slave_map),
	.get_xfer_signal	= pl08x_get_xfer_signal,
	.put_xfer_signal	= pl08x_put_xfer_signal,
};

/* Add auxdata to pass platform data */
static struct of_dev_auxdata pmb887x_auxdata[] __initdata = {
	OF_DEV_AUXDATA("arm,pl080", PMB887X_DMAC_BASE, "pmb887x-dma.0", &pmb887x_pl080_plat_data),
	OF_DEV_AUXDATA("arm,primecell", PMB887X_MMCI_BASE, "pmb887x-mmc.0", &pmb887x_pl180_plat_data),
	{}
};

static void __init pmb887x_init(void) {
	of_platform_default_populate(NULL, pmb887x_auxdata, NULL);
}

/* This is needed for LL-debug/earlyprintk/debug-macro.S */
static struct map_desc pmb887x_io_desc[] __initdata = {
	{
		.virtual	= PMB887X_IO_BASE,
		.pfn		= __phys_to_pfn(PMB887X_IO_BASE),
		.length		= PMB887X_IO_SIZE,
		.type		= MT_DEVICE,
	}
};

static void __init pmb887x_map_io(void) {
	iotable_init(pmb887x_io_desc, ARRAY_SIZE(pmb887x_io_desc));
}

static const char *pmb8876_board_compat[] = {
	"infineon,pmb8876",
	NULL,
};

DT_MACHINE_START(pmb8876_dt, "Infineon PMB8876")
	.init_machine	= pmb887x_init,
	.map_io		= pmb887x_map_io,
	.dt_compat	= pmb8876_board_compat,
MACHINE_END
