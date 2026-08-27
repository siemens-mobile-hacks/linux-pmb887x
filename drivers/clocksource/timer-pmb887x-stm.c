// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "pmb887x-stm: " fmt

#include <linux/kernel.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>

#include "timer-of.h"

#define STM_CLC 0x00
#define STM_ID 0x08
#define STM_TIM0 0x10
#define STM_TIM1 0x14
#define STM_TIM2 0x18
#define STM_TIM3 0x1C
#define STM_TIM4 0x20
#define STM_TIM5 0x24
#define STM_TIM6 0x28
#define STM_CAP 0x2C

#define STM_CLC_DISR BIT(0)
#define STM_CLC_DISS BIT(1)
#define STM_CLC_SPEN BIT(2)
#define STM_CLC_EDIS BIT(3)
#define STM_CLC_SBWE BIT(4)
#define STM_CLC_FSOE BIT(5)
#define STM_CLC_RMC GENMASK(8, 8)
#define STM_CLC_RMC_SHIFT 8

static void __iomem *stm_base;

static u64 pmb887x_stm_read(void)
{
	return ((u64)readl(stm_base + STM_TIM6) << 32) |
	       (u64)readl(stm_base + STM_TIM0);
}

static u64 pmb887x_stm_clocksource_read(struct clocksource *cs)
{
	return pmb887x_stm_read();
}

static struct clocksource pmb887x_stm = {
	.name = "pmb887x-stm",
	.rating = 300,
	.read = pmb887x_stm_clocksource_read,
	.mask = CLOCKSOURCE_MASK(56),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static int __init pmb887x_stm_init(struct device_node *node)
{
	struct timer_of *to;
	int ret;

	to = kzalloc(sizeof(*to), GFP_KERNEL);
	if (!to)
		return -ENOMEM;

	to->flags = TIMER_OF_CLOCK | TIMER_OF_BASE;

	ret = timer_of_init(node, to);
	if (ret)
		goto err;

	stm_base = timer_of_base(to);

	ret = clocksource_register_hz(&pmb887x_stm, timer_of_rate(to));
	if (ret)
		goto deinit;
	sched_clock_register(pmb887x_stm_read, 56, timer_of_rate(to));

	pr_info("stm configured at %p, freq=%lu\n", stm_base,
		timer_of_rate(to));

	return 0;

deinit:
	timer_of_cleanup(to);
err:
	kfree(to);
	pr_err("failed to register stm clocksource: %d\n", ret);
	return ret;
}

TIMER_OF_DECLARE(pmb887x_stm, "pmb887x,stm", pmb887x_stm_init);
