// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT53xx BIM interrupt gate (MT5398).
 *
 * On the MT53xx TV SoCs every peripheral interrupt passes through the BIM before it reaches
 * the Cortex-A9 GIC. The BIM has three banks of 32 lines, each with a status, an enable and
 * a write-1-to-clear register; bank n line b is GIC SPI 32*n + b:
 *
 *   bank 0 "IRQ"   SPI  0..31   status 0x30, enable 0x34, clear 0x38
 *   bank 1 "MISC"  SPI 32..63   status 0x48, enable 0x4c, clear 0x5c
 *   bank 2 "MISC2" SPI 64..95   status 0x80, enable 0x84, clear 0x88
 *
 * A line only reaches the GIC while its BIM enable bit is set, and the BIM latch has to be
 * cleared for every interrupt. LG's / MediaTek's 3.0 kernel (arch/arm/mach-mt53xx/core.c
 * in the GPL sources) clears it before running the handler; this driver does the same from
 * ->irq_ack under handle_fasteoi_ack_irq, and leaves everything else to the GIC.
 *
 * Bank 0 bits 30/31 are the MISC/MISC2 summaries; the stock kernel keeps them enabled.
 *
 * DT: interrupt-controller with #interrupt-cells = <3>, GIC binding (GIC_SPI only), and
 * interrupt-parent = the GIC.
 */
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define BIM_NR_IRQS	96
#define BIM_SUMMARY	(BIT(30) | BIT(31))

struct bim_bank {
	u16 st, en, clr;
};

static const struct bim_bank bim_banks[3] = {
	{ 0x30, 0x34, 0x38 },
	{ 0x48, 0x4c, 0x5c },
	{ 0x80, 0x84, 0x88 },
};

struct bim {
	void __iomem *base;
	raw_spinlock_t lock;
};

static void bim_update_en(struct irq_data *d, bool on)
{
	struct bim *bim = irq_data_get_irq_chip_data(d);
	const struct bim_bank *b = &bim_banks[d->hwirq / 32];
	u32 bit = BIT(d->hwirq % 32), v;
	unsigned long flags;

	raw_spin_lock_irqsave(&bim->lock, flags);
	v = readl_relaxed(bim->base + b->en);
	v = on ? v | bit : v & ~bit;
	writel_relaxed(v, bim->base + b->en);
	raw_spin_unlock_irqrestore(&bim->lock, flags);
}

static void bim_mask(struct irq_data *d)
{
	irq_chip_mask_parent(d);
	bim_update_en(d, false);
}

static void bim_unmask(struct irq_data *d)
{
	bim_update_en(d, true);
	irq_chip_unmask_parent(d);
}

static void bim_ack(struct irq_data *d)
{
	struct bim *bim = irq_data_get_irq_chip_data(d);

	writel_relaxed(BIT(d->hwirq % 32), bim->base + bim_banks[d->hwirq / 32].clr);
}

static struct irq_chip bim_chip = {
	.name			= "BIM",
	.irq_mask		= bim_mask,
	.irq_unmask		= bim_unmask,
	.irq_ack		= bim_ack,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static int bim_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
			 unsigned long *hwirq, unsigned int *type)
{
	if (!is_of_node(fwspec->fwnode) || fwspec->param_count != 3)
		return -EINVAL;
	if (fwspec->param[0] != 0 || fwspec->param[1] >= BIM_NR_IRQS)
		return -EINVAL;	/* SPIs only: PPIs do not pass the BIM */
	*hwirq = fwspec->param[1];
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static int bim_alloc(struct irq_domain *domain, unsigned int virq,
		     unsigned int nr_irqs, void *arg)
{
	struct irq_fwspec *fwspec = arg, gic_fwspec = *fwspec;
	unsigned long hwirq;
	unsigned int type, i;
	int ret;

	ret = bim_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &bim_chip, domain->host_data);

	gic_fwspec.fwnode = domain->parent->fwnode;
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &gic_fwspec);
	if (ret)
		return ret;

	/* the GIC installed handle_fasteoi_irq; the BIM latch also needs ->irq_ack */
	for (i = 0; i < nr_irqs; i++)
		irq_set_handler(virq + i, handle_fasteoi_ack_irq);
	return 0;
}

static const struct irq_domain_ops bim_domain_ops = {
	.translate	= bim_translate,
	.alloc		= bim_alloc,
	.free		= irq_domain_free_irqs_common,
};

static int __init bim_of_init(struct device_node *node, struct device_node *parent)
{
	struct irq_domain *domain, *domain_parent;
	struct bim *bim;
	int i;

	domain_parent = irq_find_host(parent);
	if (!domain_parent) {
		pr_err("%pOF: interrupt-parent not found\n", node);
		return -EINVAL;
	}

	bim = kzalloc(sizeof(*bim), GFP_KERNEL);
	if (!bim)
		return -ENOMEM;
	raw_spin_lock_init(&bim->lock);

	bim->base = of_iomap(node, 0);
	if (!bim->base) {
		pr_err("%pOF: cannot map registers\n", node);
		kfree(bim);
		return -ENOMEM;
	}

	/*
	 * The stock kernel hands over with its own lines still enabled. Start from what its
	 * init does: everything off and cleared, only the MISC/MISC2 summaries on.
	 */
	for (i = 0; i < ARRAY_SIZE(bim_banks); i++) {
		writel_relaxed(i ? 0 : BIM_SUMMARY, bim->base + bim_banks[i].en);
		writel_relaxed(~0U, bim->base + bim_banks[i].clr);
	}

	domain = irq_domain_add_hierarchy(domain_parent, 0, BIM_NR_IRQS, node,
					  &bim_domain_ops, bim);
	if (!domain) {
		iounmap(bim->base);
		kfree(bim);
		return -ENOMEM;
	}
	pr_info("%pOF: MT53xx BIM, %d lines\n", node, BIM_NR_IRQS);
	return 0;
}
IRQCHIP_DECLARE(mt5398_bim, "mediatek,mt5398-bim", bim_of_init);
