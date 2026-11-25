// SPDX-License-Identifier: GPL-2.0-only
/*
 * PowerVR SGX544MP1 in the MediaTek MT5398 (LG NetCast): identify and reset only.
 *
 * Stage 2-4 of bringing a core up (powervr_sgx docs/porting): find the registers, read the
 * core ID and revision, and run the reset sequence, before any driver relies on them.
 * Nothing renders: the open powervr_sgx stack has no USSE2 (SGX543/544/554) microkernel
 * or compiler yet.
 *
 * Facts (docs/sgx544.md has the sources):
 *  - Registers at 0xf0040000, 0xc000 long; IRQ GIC 85 = BIM MISC line 21 (LG pvrsrvkm
 *    SysLocateDevices). MP layout, 16 KB banks (Imagination's sgx_mkif_km.h): bank 0
 *    broadcasts to the cores, bank 1 (0x4000) is the master, core n is bank n + 2, so the
 *    single core is at 0x8000.
 *  - Reset = LG pvrsrvkm SGXReset(): MASTER_SOFT_RESET 0x1ff (cores 0-3, IPF, DPM, VDM,
 *    SLC, BIF), MASTER_BIF_CTRL 0, MASTER_SLC_CTRL 0x4c000, MASTER_SLC_CTRL_BYPASS
 *    0x04000000, then MASTER_SOFT_RESET 0, with ~100 core clocks between steps.
 *  - LG's SysInitialise() writes CKGEN 0xf000d3d4 = 9 and 0xf000d3d8 = 1 after the core
 *    is already up; read and logged here, never written.
 *
 * Probing touches no SGX register. A read of a gated SGX can hang the bus, and at
 * device-initcall time nothing would get the TV back. Register access only happens on a
 * write to the "action" sysfs file, after the initramfs has armed the PDWNC watchdog:
 *   echo id    > /sys/bus/platform/devices/f0040000.gpu/action   # read-only identify
 *   echo reset > .../action                                       # LG's reset, then id
 *   echo park  > .../action                                       # hold the GPU in reset
 *   cat .../report
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#define BANK_MASTER		0x4000
#define BANK_CORE0		0x8000

/* Master bank (offsets include the bank, as in sgxmpdefs.h). */
#define MASTER_CORE_ID		0x4010
#define MASTER_CORE_REVISION	0x4014
#define MASTER_SOFT_RESET	0x4080
#define MASTER_SOFT_RESET_ALL	0x1ff
#define MASTER_BIF_CTRL		0x4c00
#define MASTER_SLC_CTRL		0x4d00
#define MASTER_SLC_CTRL_BYPASS	0x4d04

/* Core bank (sgx544defs.h), the registers safe to read before a reset. */
#define CR_CLKGATECTL		0x0000
#define CR_CLKGATESTATUS	0x0008
#define CR_CORE_ID		0x0020
#define CR_CORE_REVISION	0x0024
#define CR_SOFT_RESET		0x0080
#define CR_EVENT_STATUS		0x012c
#define CR_EVENT_HOST_ENABLE	0x0130
#define CR_BIF_CTRL		0x0c00

/* SGX_CORE_ID_544 in the ID field; the revision phase 0 read under stock (1.1.5). */
#define SGX544_ID		0x0119
#define SGX544_REV_SEEN		0x00010105

/* CKGEN words LG's SysInitialise() writes (offsets in reg "ckgen"). */
#define CKGEN_A			0x0
#define CKGEN_B			0x4

struct mt5398_sgx {
	struct device *dev;
	void __iomem *regs;
	void __iomem *ckgen;
	struct mutex lock;
	char report[768];
};

static u32 rd(struct mt5398_sgx *s, u32 off)
{
	return readl(s->regs + off);
}

static void wr(struct mt5398_sgx *s, u32 off, u32 v)
{
	writel(v, s->regs + off);
	readl(s->regs + MASTER_SOFT_RESET);	/* push the write out before the wait */
}

/* SGXWaitClocks(100): 100 core clocks at 324 MHz is ~0.3 us. */
static void clocks_wait(void)
{
	udelay(5);
}

static void sgx_identify(struct mt5398_sgx *s, const char *when)
{
	u32 mid = rd(s, MASTER_CORE_ID), mrev = rd(s, MASTER_CORE_REVISION);
	u32 id = rd(s, BANK_CORE0 + CR_CORE_ID), rev = rd(s, BANK_CORE0 + CR_CORE_REVISION);
	bool known = (id >> 16) == SGX544_ID;

	scnprintf(s->report, sizeof(s->report),
		  "%s\n"
		  "ckgen d3d4 %08x d3d8 %08x\n"
		  "master core_id %08x rev %08x soft_reset %08x\n"
		  "core0 core_id %08x (id %04x config %04x: multi %u cores-field %u slc %u) rev %08x (%u.%u.%u)\n"
		  "core0 clkgatectl %08x clkgatestatus %08x soft_reset %08x\n"
		  "core0 event_status %08x event_host_enable %08x bif_ctrl %08x\n"
		  "verdict: %s\n",
		  when, readl(s->ckgen + CKGEN_A), readl(s->ckgen + CKGEN_B),
		  mid, mrev, rd(s, MASTER_SOFT_RESET),
		  id, id >> 16, id & 0xffff, id & 1, (id >> 8) & 0xf, (id >> 12) & 0xf,
		  rev, (rev >> 16) & 0xff, (rev >> 8) & 0xff, rev & 0xff,
		  rd(s, BANK_CORE0 + CR_CLKGATECTL), rd(s, BANK_CORE0 + CR_CLKGATESTATUS),
		  rd(s, BANK_CORE0 + CR_SOFT_RESET),
		  rd(s, BANK_CORE0 + CR_EVENT_STATUS), rd(s, BANK_CORE0 + CR_EVENT_HOST_ENABLE),
		  rd(s, BANK_CORE0 + CR_BIF_CTRL),
		  !known ? "NOT an SGX544 ID: report these values" :
		  rev == SGX544_REV_SEEN ? "SGX544 1.1.5, as read under stock" :
		  "SGX544, revision differs from the 1.1.5 read under stock");
	dev_info(s->dev, "%s", s->report);
}

/* LG pvrsrvkm SGXReset(), normal (not recovery) path, up to the clock gating setup. */
static void sgx_reset(struct mt5398_sgx *s)
{
	wr(s, MASTER_SOFT_RESET, MASTER_SOFT_RESET_ALL);
	clocks_wait();
	wr(s, MASTER_BIF_CTRL, 0);
	wr(s, MASTER_SLC_CTRL, 0x0004c000);
	wr(s, MASTER_SLC_CTRL_BYPASS, 0x04000000);
	clocks_wait();
	wr(s, MASTER_SOFT_RESET, 0);
	clocks_wait();
}

static ssize_t action_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct mt5398_sgx *s = dev_get_drvdata(dev);

	mutex_lock(&s->lock);
	if (sysfs_streq(buf, "id")) {
		sgx_identify(s, "identify (no writes)");
	} else if (sysfs_streq(buf, "reset")) {
		sgx_reset(s);
		sgx_identify(s, "after LG's reset sequence");
	} else if (sysfs_streq(buf, "park")) {
		wr(s, MASTER_SOFT_RESET, MASTER_SOFT_RESET_ALL);
		clocks_wait();
		sgx_identify(s, "parked in reset");
	} else {
		len = -EINVAL;
	}
	mutex_unlock(&s->lock);
	return len;
}
static DEVICE_ATTR_WO(action);

static ssize_t report_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt5398_sgx *s = dev_get_drvdata(dev);
	ssize_t n;

	mutex_lock(&s->lock);
	n = sysfs_emit(buf, "%s", s->report[0] ? s->report : "nothing read yet\n");
	mutex_unlock(&s->lock);
	return n;
}
static DEVICE_ATTR_RO(report);

static struct attribute *sgx_attrs[] = {
	&dev_attr_action.attr,
	&dev_attr_report.attr,
	NULL
};
ATTRIBUTE_GROUPS(sgx);

static int sgx_probe(struct platform_device *pdev)
{
	struct mt5398_sgx *s;

	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = &pdev->dev;
	mutex_init(&s->lock);

	s->regs = devm_platform_ioremap_resource_byname(pdev, "sgx");
	if (IS_ERR(s->regs))
		return PTR_ERR(s->regs);
	s->ckgen = devm_platform_ioremap_resource_byname(pdev, "ckgen");
	if (IS_ERR(s->ckgen))
		return PTR_ERR(s->ckgen);

	platform_set_drvdata(pdev, s);
	dev_info(&pdev->dev, "SGX544MP1 mapped; untouched until 'echo id > %s/action'\n",
		 dev_name(&pdev->dev));
	return 0;
}

static const struct of_device_id sgx_of_match[] = {
	{ .compatible = "mediatek,mt5398-sgx" },
	{ }
};
MODULE_DEVICE_TABLE(of, sgx_of_match);

static struct platform_driver sgx_driver = {
	.driver = {
		.name = "mt5398-sgx",
		.of_match_table = sgx_of_match,
		.dev_groups = sgx_groups,
	},
	.probe = sgx_probe,
};
module_platform_driver(sgx_driver);

MODULE_DESCRIPTION("MT5398 PowerVR SGX544 identify and reset");
MODULE_LICENSE("GPL");
