// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "pmb887x-tpu: " fmt

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
#include <linux/gcd.h>

#include "timer-of.h"

#define TPU_CLC (0x00)

#define TPU_OVERFLOW (0x20)
#define TPU_OVERFLOW_VALUE GENMASK(15, 0)
#define TPU_OVERFLOW_VALUE_SHIFT 0

#define TPU_OVERFLOW_MIN 1
#define TPU_OVERFLOW_MAX 32767

#define TPU_INT(n) (0x24 + ((n) * 0x4))
#define TPU_INT_VALUE GENMASK(15, 0)
#define TPU_INT_VALUE_SHIFT 0

#define TPU_COUNTER (0x34)
#define TPU_COUNTER_VALUE GENMASK(15, 0)
#define TPU_COUNTER_VALUE_SHIFT 0

#define TPU_PARAM (0x5C)
#define TPU_PARAM_TINI BIT(0)
#define TPU_PARAM_FDIS BIT(1)

#define TPU_PLLCON0 (0x68)
#define TPU_PLLCON0_K_DIV GENMASK(30, 0)
#define TPU_PLLCON0_K_DIV_SHIFT 0

#define TPU_PLLCON1 (0x6C)
#define TPU_PLLCON1_L_DIV GENMASK(30, 0)
#define TPU_PLLCON1_L_DIV_SHIFT 0

#define TPU_PLLCON2 (0x70)
#define TPU_PLLCON2_LOAD BIT(0)
#define TPU_PLLCON2_INIT BIT(1)

#define TPU_SRC(n) (0xF8 + ((n) * 0x4))

#define TPU_SRC_SRPN GENMASK(8, 0)
#define TPU_SRC_SRPN_SHIFT 0
#define TPU_SRC_TOS GENMASK(2, 10)
#define TPU_SRC_TOS_SHIFT 10
#define TPU_SRC_SRE BIT(12)
#define TPU_SRC_SRR BIT(13)
#define TPU_SRC_CLRR BIT(14)
#define TPU_SRC_SETR BIT(15)

struct pmb887x_tpu_priv {
	u32 freq;
};

static int pmb887x_tpu_set_next_event(long unsigned int delta,
				      struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	pr_debug("set_next_event = %ld\n", delta);

	writel(0, timer_of_base(to) + TPU_PARAM);
	writel(delta, timer_of_base(to) + TPU_OVERFLOW);
	writel(TPU_PARAM_FDIS | TPU_PARAM_TINI, timer_of_base(to) + TPU_PARAM);

	return 0;
}

static int pmb887x_tpu_shutdown(struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	pr_debug("shutdown");
	writel(0, timer_of_base(to) + TPU_PARAM);

	return 0;
}

static int pmb887x_tpu_set_periodic(struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);
	struct pmb887x_tpu_priv *priv = to->private_data;
	u32 ticks_per_jiffy = DIV_ROUND_CLOSEST(priv->freq, HZ);

	pr_debug("set periodic mode\n");

	if (ticks_per_jiffy < 1 || ticks_per_jiffy > TPU_OVERFLOW_MAX) {
		pr_err("Bad tpu freq (%d Hz) for %d Hz ticks.\n", priv->freq,
		       HZ);
		return -ETIME;
	}

	pmb887x_tpu_set_next_event(ticks_per_jiffy - 1, clkevt);

	return 0;
}

static int pmb887x_tpu_set_oneshot(struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);
	pr_debug("set oneshot mode\n");
	writel(0, timer_of_base(to) + TPU_PARAM);
	return 0;
}

static void pmb887x_tpu_init_hw(struct timer_of *to)
{
	struct pmb887x_tpu_priv *priv = to->private_data;
	unsigned long ftpu_rate = clk_get_rate(to->of_clk.clk);
	unsigned long target_rate = min(ftpu_rate, (unsigned long)priv->freq);
	u64 tmp, lcm_value;
	u32 L, K;

	pr_info("setup TPU timer at %d Hz\n", priv->freq);

	// Calculare dividers
	lcm_value =
		(u64)((target_rate * 6) / gcd((target_rate * 6), ftpu_rate)) *
		(u64)ftpu_rate;

	tmp = lcm_value;
	do_div(tmp, (target_rate * 6));
	L = tmp;

	tmp = lcm_value;
	do_div(tmp, ftpu_rate);
	K = tmp;

	// Setup timer freq
	writel(K << TPU_PLLCON0_K_DIV_SHIFT, timer_of_base(to) + TPU_PLLCON0);
	writel(L << TPU_PLLCON1_L_DIV_SHIFT, timer_of_base(to) + TPU_PLLCON1);
	writel(TPU_PLLCON2_INIT, timer_of_base(to) + TPU_PLLCON2);

	// Overflow
	writel(0xFFFF, timer_of_base(to) + TPU_OVERFLOW);
	writel(0, timer_of_base(to) + TPU_PARAM);

	// Enable IRQ
	writel(0, timer_of_base(to) + TPU_INT(0));
	writel(TPU_SRC_CLRR | TPU_SRC_SRE, timer_of_base(to) + TPU_SRC(0));
}

static irqreturn_t pmb887x_tpu_handler(int irq, void *dev_id)
{
	struct clock_event_device *clkevt = (struct clock_event_device *)dev_id;
	struct timer_of *to = to_timer_of(clkevt);
	u32 tmp;

	if (!clockevent_state_periodic(clkevt))
		pmb887x_tpu_shutdown(clkevt);

	writel(TPU_SRC_CLRR | TPU_SRC_SRE, timer_of_base(to) + TPU_SRC(0));

	clkevt->event_handler(clkevt);

	return IRQ_HANDLED;
}

static int __init pmb887x_tpu_init(struct device_node *node)
{
	struct timer_of *to;
	struct pmb887x_tpu_priv *priv;
	int ret;

	to = kzalloc(sizeof(*to), GFP_KERNEL);
	if (!to)
		return -ENOMEM;

	priv = kzalloc(sizeof(struct pmb887x_tpu_priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err;
	}

	to->flags = TIMER_OF_IRQ | TIMER_OF_CLOCK | TIMER_OF_BASE;
	to->of_irq.handler = pmb887x_tpu_handler;
	to->private_data = priv;

	ret = timer_of_init(node, to);
	if (ret)
		goto err;

	ret = of_property_read_u32(node, "clock-frequency", &priv->freq);
	if (ret) {
		pr_err("Can't read clock-frequency\n");
		ret = -EINVAL;
		goto deinit;
	}

	to->clkevt.name = to->np->full_name;
	to->clkevt.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	to->clkevt.set_state_shutdown = pmb887x_tpu_shutdown;
	to->clkevt.set_state_periodic = pmb887x_tpu_set_periodic;
	to->clkevt.set_state_oneshot = pmb887x_tpu_set_oneshot;
	to->clkevt.tick_resume = pmb887x_tpu_shutdown;
	to->clkevt.set_next_event = pmb887x_tpu_set_next_event;
	to->clkevt.rating = 250;

	pmb887x_tpu_init_hw(to);

	clockevents_config_and_register(&to->clkevt, priv->freq,
					TPU_OVERFLOW_MIN, TPU_OVERFLOW_MAX);

	return 0;

deinit:
	timer_of_cleanup(to);
err:
	kfree(to);
	pr_err("failed to register tpu clockevent: %d\n", ret);
	return ret;
}

TIMER_OF_DECLARE(pmb887x_tpu, "pmb887x,tpu", pmb887x_tpu_init);
