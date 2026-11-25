// SPDX-License-Identifier: GPL-2.0
/*
 * MT5398 (LG NetCast) secondary CPU bring-up for a kernel warm-booted by nckexec.
 *
 * On a cold boot MediaTek's boot ROM holds CPU1 until BIM TIMER1_LLMT reads 0xffffb007 and
 * then jumps to the physical address in TIMER1_HLMT (LG's platsmp.c). Under the stock
 * kernel CPU1 has long left that loop, so nckexec moves it instead: just before the jump
 * it runs a kthread on CPU1 that turns the MMU and caches off and enters a small stub in a
 * page of the RAM hole (DT: compatible "netcast,mt5398-cpu-hold", also nckexec's
 * "nckexec,cpu1-hold" request). The stub stops the A9 private timer, disables CPU1's GIC
 * CPU interface, writes HOLD_WAITING and waits in WFE until the entry word is non-zero.
 *
 * Here: if the page says HOLD_WAITING, CPU1 is released by writing secondary_startup's
 * physical address into it and SEV. Anything else (an older nckexec, CPU1 left in the
 * stock kernel's hotplug WFI pen, whose code this kernel has overwritten) means CPU1 must
 * not be woken: it is dropped from the present mask and the kernel runs on CPU0 only.
 * Nothing here sends CPU1 an interrupt, since only SEV can wake the stub's WFE.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <asm/barrier.h>
#include <asm/smp.h>
#include <asm/smp_scu.h>

/* Hold page layout; must match src/kmod/nckexec.c. */
#define HOLD_STATE	0xff0
#define HOLD_ENTRY	0xff4
#define HOLD_ACTLR	0xff8	/* CPU1's ACTLR and CPSR as the stub saw them */
#define HOLD_CPSR	0xffc
#define HOLD_WAITING	0x5398c1a0
#define HOLD_RELEASED	0x5398c1e0

static void __iomem *hold;

static void __init mt5398_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	void __iomem *scu;
	unsigned int cpu;
	u32 state = 0;

	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	scu = of_iomap(np, 0);
	of_node_put(np);
	if (scu) {
		scu_enable(scu);	/* stock left it on: a no-op then */
		iounmap(scu);
	}

	np = of_find_compatible_node(NULL, NULL, "netcast,mt5398-cpu-hold");
	hold = of_iomap(np, 0);
	of_node_put(np);
	if (hold)
		state = readl(hold + HOLD_STATE);
	if (state != HOLD_WAITING) {
		pr_warn("mt5398-smp: CPU1 is not in the hold page (%s, state 0x%08x): CPU0 only\n",
			hold ? "mapped" : "no hold node", state);
		if (hold)
			iounmap(hold);
		hold = NULL;
		for_each_present_cpu(cpu)
			if (cpu)
				set_cpu_present(cpu, false);
		return;
	}
	pr_info("mt5398-smp: CPU1 waits in the hold page (ACTLR 0x%08x, CPSR 0x%08x)\n",
		readl(hold + HOLD_ACTLR), readl(hold + HOLD_CPSR));
}

static int mt5398_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	if (cpu != 1 || !hold)
		return -ENODEV;
	writel(__pa_symbol(secondary_startup_arm), hold + HOLD_ENTRY);
	dsb_sev();
	return 0;
}

static void mt5398_smp_secondary_init(unsigned int cpu)
{
	if (hold && readl(hold + HOLD_STATE) != HOLD_RELEASED)
		pr_warn("mt5398-smp: hold page state 0x%08x after release\n",
			readl(hold + HOLD_STATE));
}

static const struct smp_operations mt5398_smp_ops __initconst = {
	.smp_prepare_cpus	= mt5398_smp_prepare_cpus,
	.smp_secondary_init	= mt5398_smp_secondary_init,
	.smp_boot_secondary	= mt5398_smp_boot_secondary,
};
CPU_METHOD_OF_DECLARE(mt5398_smp, "netcast,mt5398-smp", &mt5398_smp_ops);
