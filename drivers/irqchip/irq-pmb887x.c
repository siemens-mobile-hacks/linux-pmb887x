// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <asm/exception.h>

#define INTC_FIQ_STAT 0x08
#define INTC_FIQ_STAT_NUM 0xFF
#define INTC_FIQ_STAT_NUM_SHIFT 0
#define INTC_FIQ_STAT_UNREAD BIT(16)
#define INTC_FIQ_STAT_NOT_ACK BIT(24)

#define INTC_IRQ_STAT 0x0C
#define INTC_IRQ_STAT_NUM 0xFF
#define INTC_IRQ_STAT_NUM_SHIFT 0
#define INTC_IRQ_STAT_UNREAD BIT(16)
#define INTC_IRQ_STAT_NOT_ACK BIT(24)

#define INTC_FIQ_ACK 0x10
#define INTC_IRQ_ACK 0x14

#define INTC_CURRENT_FIQ 0x18
#define INTC_CURRENT_IRQ 0x1C

#define INTC_CON(n) (0x30 + ((n) * 0x4))
#define INTC_CON_PRIORITY 0xFF
#define INTC_CON_PRIORITY_SHIFT 0
#define INTC_CON_FIQ BIT(8)

#define INTC_NR_IRQ 170

struct pmb887x_irqc_priv {
	void __iomem *irq_base;
	struct irq_domain *irq_domain;
};

static struct pmb887x_irqc_priv *current_intc;

static void __irq_entry pmb887x_intc_handler_irq(struct pt_regs *regs)
{
	irq_hw_number_t hwirq =
		readl_relaxed(current_intc->irq_base + INTC_CURRENT_IRQ);
	generic_handle_domain_irq(current_intc->irq_domain, hwirq);
}

static void pmb887x_irqc_ack(struct irq_data *d)
{
	struct pmb887x_irqc_priv *priv = irq_data_get_irq_chip_data(d);
	writel(1, priv->irq_base + INTC_IRQ_ACK);
}

static void pmb887x_irqc_mask(struct irq_data *d)
{
	struct pmb887x_irqc_priv *priv = irq_data_get_irq_chip_data(d);
	writel(0, priv->irq_base + INTC_CON(d->hwirq));
}

static void pmb887x_irqc_unmask(struct irq_data *d)
{
	struct pmb887x_irqc_priv *priv = irq_data_get_irq_chip_data(d);
	writel(1, priv->irq_base + INTC_CON(d->hwirq));
}

static struct irq_chip pmb887x_irqc_chip = {
	.name = "pmb887x_irqc",
	.irq_ack = pmb887x_irqc_ack,
	.irq_mask = pmb887x_irqc_mask,
	.irq_unmask = pmb887x_irqc_unmask,
};

static int pmb887x_irqc_map(struct irq_domain *d, unsigned int virq,
			    irq_hw_number_t hwirq)
{
	struct pmb887x_irqc_priv *priv = d->host_data;
	irq_domain_set_info(d, virq, hwirq, &pmb887x_irqc_chip, priv,
			    handle_level_irq, NULL, NULL);
	irq_set_probe(virq);
	return 0;
}

static const struct irq_domain_ops pmb887x_irqc_ops = {
	.map = pmb887x_irqc_map,
	.xlate = irq_domain_xlate_onecell,
};

static int __init pmb887x_intc_init(struct device_node *node,
				    struct device_node *parent)
{
	struct pmb887x_irqc_priv *priv =
		kzalloc(sizeof(struct pmb887x_irqc_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->irq_base = of_iomap(node, 0);
	if (!priv->irq_base)
		panic("%pOF: iomap not set!\n", node);

	priv->irq_domain = irq_domain_add_linear(node, INTC_NR_IRQ,
						 &pmb887x_irqc_ops, priv);
	if (!priv->irq_domain)
		panic("%pOF: unable to create IRQ domain!\n", node);

	current_intc = priv;
	set_handle_irq(pmb887x_intc_handler_irq);

	return 0;
}

IRQCHIP_DECLARE(pmb887x_intc, "pmb887x,intc", pmb887x_intc_init);
