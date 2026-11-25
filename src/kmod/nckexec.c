// SPDX-License-Identifier: GPL-2.0
/*
 * nckexec: a small "kexec" for the LG NetCast 3.0.13 ARM kernel.
 *
 * The stock kernel is built with CONFIG_KEXEC *off*, so the kexec_load()
 * syscall and kernel_kexec() do not exist. This module reimplements the ARM
 * warm-boot path to hand control to another kernel image at runtime, without
 * touching flash.
 *
 * Image format: a raw ARM Linux "Image" (what LG ships: p21 is an MTK LZ4P
 * container around a uImage around a raw Image; tools/unlz4p.py unpacks it).
 * A leading 64-byte uImage header is skipped automatically. A raw Image is not
 * position independent, so it is copied to its link address (load=, default
 * phys 0x8000 = PHYS_OFFSET 0 + TEXT_OFFSET) -- i.e. on top of the running
 * kernel. That copy therefore runs from a stub with the MMU and caches off,
 * like a real kexec's relocate_kernel.
 *
 * Staging (at insmod): every image page, the segment list and the stub page
 * are ordinary GFP_KERNEL pages, each forced above lowmark= (16 MiB) so none
 * of them can sit inside the destination. No large contiguous allocation.
 * Fresh ATAGs are built: CORE, one MEM per "System RAM" bank in /proc/iomem,
 * and CMDLINE (this boot's command line, or cmdline=, plus append=).
 *
 * Handoff (echo 1 > .../parameters/go), on the only online CPU:
 *   kernel_restart_prepare(NULL)  reboot notifiers + device_shutdown(), the
 *                                 same as a normal reboot/mainline kexec; this
 *                                 stops USB/Ethernet DMA into RAM we hand over; the MUSB
 *                                 DMA and the GPU are stopped right before the jump
 *   machine_shutdown()            SMP stop (no-op with CPU1 offline)
 *   setup_mm_for_reboot(0)        1:1 section map so a branch to a physical
 *                                 address resolves with the MMU on
 *   flush L1 + PL310              everything staged is in DRAM
 *   stub (at its physical addr):  D-cache off; L1 clean+inv by set/way; PL310
 *                                 clean+inv by way; MMU/I-cache/BP off; copy
 *                                 segments; enter kernel r0=0 r1=mach r2=atags
 * The stub uses no stack and no kernel code, so nothing it relies on can be
 * overwritten by the copy.
 *
 * CPU1, without a hold request: it must be offline first (echo 0 >
 * /sys/devices/system/cpu/cpu1/online). This kernel's platform_cpu_die() flushes
 * its L1 and waits in WFI for pen_release, but that loop is stock kernel text
 * which the copy overwrites: the next kernel must never wake CPU1 (maxcpus=1).
 *
 * CPU1 with a hold request (DTB "nckexec,cpu1-hold = <CPU1_HOLD_MARK page>", a
 * page in the RAM hole): CPU1 stays online. After kernel_restart_prepare() a
 * kthread bound to CPU1 takes it out of this kernel (IRQs off, offline in the
 * mask as ipi_cpu_stop does, 1:1 map, L1 flushed) into a stub copied to that
 * page, which turns the MMU and caches off, stops CPU1's private timer and GIC
 * CPU interface and waits in WFE for an entry address -- the mainline kernel's
 * "netcast,mt5398-smp" enable-method writes it and sends SEV.
 *
 * Usage:
 *   echo 0 > /sys/devices/system/cpu/cpu1/online
 *   insmod nckexec.ko image=/path/Image append='maxcpus=1;panic=5'
 *   (append= takes ';' between arguments: busybox insmod drops quotes)
 *   echo 1 > /sys/module/nckexec/parameters/go      # point of no return
 *   (go also takes "probe": dry run of the hand-off checks, "dump": log the
 *   OSD planes (reads only), and "layer": fails
 *   unless OSD plane 3 is LG's 1280x720 layer, i.e. an app window is up)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/kallsyms.h>
#include <linux/ioport.h>
#include <linux/cpumask.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <asm/cacheflush.h>
#include <asm/outercache.h>
#include <asm/setup.h>
#include <asm/io.h>

static char *image;
module_param(image, charp, 0444);
MODULE_PARM_DESC(image, "path to the next kernel: raw ARM Image (uImage header skipped)");

static ulong load = 0x8000;
module_param(load, ulong, 0444);
MODULE_PARM_DESC(load, "physical load/entry address of the Image (default 0x8000)");

static uint mach;
module_param(mach, uint, 0644);
MODULE_PARM_DESC(mach, "ARM machine number (0 = reuse current __machine_arch_type)");

static char *cmdline;
module_param(cmdline, charp, 0444);
MODULE_PARM_DESC(cmdline, "kernel command line (default: this boot's)");

static char *append;
module_param(append, charp, 0444);
MODULE_PARM_DESC(append, "extra arguments appended to the command line, ';'-separated");

static ulong atags_at = 0x100;
module_param(atags_at, ulong, 0444);
MODULE_PARM_DESC(atags_at, "physical address the ATAGs are copied to (default 0x100)");

static ulong lowmark = 0x01000000;
module_param(lowmark, ulong, 0444);
MODULE_PARM_DESC(lowmark, "staging pages are kept above this physical address");

static bool shutdown = true;
module_param(shutdown, bool, 0644);
MODULE_PARM_DESC(shutdown, "run kernel_restart_prepare() (notifiers + device_shutdown) first");

static char *dtb;
module_param(dtb, charp, 0444);
MODULE_PARM_DESC(dtb, "flattened device tree for the next kernel: replaces the ATAGs, copied to atags_at (must end below 0x4000)");

/*
 * OSD hand-off for a DT kernel. The panel shows MediaTek OSD plane 3, whose region header
 * (in System RAM) points at an ARGB8888 bitmap in the reserved RAM hole. Both addresses
 * change at run time, so the DTB carries cookies that nckexec_fire() replaces with the
 * live values: OSD_FB_COOKIE -> bitmap base (simple-framebuffer reg), OSD_HDR_COOKIE ->
 * the header's page (reserved-memory, so the new kernel never reuses it while the OSD
 * keeps fetching it). The OSD block is reached through the stock static map (VA == PA).
 */
#define OSD_FB_COOKIE	0x0fb0c0de
#define OSD_HDR_COOKIE	0x0ead0c0d
#define OSD_PLA3	((void __iomem *)0xf0021300)
#define OSD_PLA(n)	((void __iomem *)(0xf0021000 + (n) * 0x100))
#define OSD_W		1280
#define OSD_H		720
#define RAM_HOLE_LO	0x26000000UL
#define RAM_HOLE_HI	0x40000000UL

/*
 * Optional 1080p hand-off. The panel is 1920x1080 and stock upscales plane 3 1.5x in its
 * scaler SC3 (OSD base + 0x600; MT5398 maps SC1/SC2/SC3 at +0x400/+0x500/+0x600 to planes
 * 1/2/3, per LG's GPL U-Boot osd_scaler_hw.c). A DTB that carries the property
 * "nckexec,osd-1080 = <OSD_1080_MARK addr>" asks for a native 1920x1080 plane instead:
 * after shutdown nckexec fills the buffer at addr (System RAM the DT reserves no-map, top
 * of bank 0 = stock highmem, so no staging page can be there) with opaque black, points
 * plane 3's region header at it with the 1080p geometry and puts SC3 into 1:1 bypass, the
 * values LG's OSD_SC_Scale() programs for src == dst (SC1/SC2 sit like that already).
 */
#define OSD_1080_MARK	0x0fb1080f
#define CPU1_HOLD_MARK	0x0cc1b007
#define OSD_RB_COOKIE(n) (0x05d0c0a0 + (n))
#define OSD_BASE	((void __iomem *)0xf0021000)
#define OSD_SC3		((void __iomem *)0xf0021600)
#define FB_W		1920
#define FB_H		1080
#define FB_SIZE		(FB_W * FB_H * 4)
#define RAM_BANK0_HI	0x26000000UL

/*
 * Plane 3 off (LG shows no UI, e.g. after a watchdog return) or not stock's layer: there is
 * no header of LG's to edit, so nckexec writes its own in the page right after the 1080p
 * bitmap (the DTB's header cookie then reserves it), points the plane at it and enables the
 * plane. Template = the header the 1080p hand-off left on the TV (boot 11): ARGB8888, single
 * region, mix weight ff, AlphaSel 3, pixel mix; the address and geometry fields are filled
 * in by osd1080_go(). Plane +8 gets stock's visible value (HFilter on, blend/fade ff).
 */
#define OSD_OWN_HDR(fb)	((fb) + FB_SIZE)
#define OSD_BLEND_ON	0x0001ffff
static const u32 osd_hdr_1080[8] = {
	0xe0000000, 0xff580000, 0xe6000000, 0x00000000,
	0xf0438780, 0x10001000, 0x00000438, 0x40000780,
};

/*
 * MUSB hand-off. The stock MtkUsbHcd driver has no shutdown hook, so its four cores keep
 * running through the jump and an armed HSDMA channel (the Wi-Fi dongle's bulk-in, seen
 * live on port 3) would still write into a stock buffer the next kernel may own by then.
 * With IRQs off nothing can re-arm them: clear the four channel controls of every core.
 * Same static map as the OSD; MT53xx USB registers take 32-bit writes only.
 */
/* Unexported helpers, resolved via kallsyms at load time. */
static void (*p_setup_mm_for_reboot)(char mode);
static void (*p_machine_shutdown)(void);
static void (*p_kernel_restart_prepare)(char *cmd);

#define MUSB_CORE(n)		((void __iomem *)(0xf0050000 + 0x1000 * (n)))
#define MUSB_HSDMA_CTRL(ch)	(0x204 + 0x10 * (ch))

static bool usbstop = true;
module_param(usbstop, bool, 0644);
MODULE_PARM_DESC(usbstop, "stop the MT5398 USB DMA channels right before the jump");

static void musb_dma_stop(void)
{
	int n, ch;

	for (n = 0; n < 4; n++)
		for (ch = 0; ch < 4; ch++)
			writel(0, MUSB_CORE(n) + MUSB_HSDMA_CTRL(ch));
}

/*
 * SGX544 hand-off. LG's pvrsrvkm has no shutdown hook that stops the microkernel: its 1 kHz
 * timer task keeps incrementing SGXMKIF_HOST_CTL.ui32TimeWraps, a word in ordinary highmem
 * that the next kernel owns by then (seen corrupting its RAM, FINDINGS.md "RAM corruption").
 * Hold the whole GPU in reset, the first step of LG's SGXReset(); a later GPU driver does
 * that itself. First wait (bounded) for the core to go idle, so no burst is cut short.
 * Master bank registers stay readable while the cores are in reset (SGXResetSleep() reads
 * this very one). Same static map as the OSD.
 */
#define SGX_CORE0_CLKGATESTATUS	((void __iomem *)0xf0048008)	/* 0 = every module idle */
#define SGX_MASTER_SOFT_RESET	((void __iomem *)0xf0044080)
#define SGX_SOFT_RESET_ALL	0x1ff	/* cores 0-3, IPF, DPM, VDM, SLC, BIF */

static bool gpustop = true;
module_param(gpustop, bool, 0644);
MODULE_PARM_DESC(gpustop, "hold the SGX544 in reset right before the jump");

static void sgx_stop(void)
{
	int n;

	for (n = 0; n < 5000 && readl(SGX_CORE0_CLKGATESTATUS); n++)
		udelay(10);
	writel(SGX_SOFT_RESET_ALL, SGX_MASTER_SOFT_RESET);
	readl(SGX_MASTER_SOFT_RESET);	/* push the write out */
	udelay(5);			/* 100 core clocks at 324 MHz is 0.3 us */
}

/*
 * CPU1 hold page (see the header comment). Layout shared with the next kernel's
 * arch/arm/mach-mt5398/platsmp.c: stub code from 0, words at the end.
 */
#define HOLD_STATE	0xff0	/* 0, then HOLD_WAITING, then HOLD_RELEASED */
#define HOLD_ENTRY	0xff4	/* physical entry; 0 = keep waiting */
#define HOLD_ACTLR	0xff8	/* CPU1's ACTLR / CPSR as the stub saw them */
#define HOLD_CPSR	0xffc
#define HOLD_WAITING	0x5398c1a0
#define HOLD_RELEASED	0x5398c1e0
#define TRUSTZONE_LO	0x3f000000UL	/* secure world: the last 16 MB of the hole */

/* Entered on CPU1 at its PA through the 1:1 map, r0 = the page's PA. No stack. */
asm(
"	.pushsection .text\n"
"	.arm\n"
"	.align	5\n"
"	.global	nckexec_hold\n"
"nckexec_hold:\n"
"	mov	r12, r0\n"
	/* D-cache off, then clean+invalidate L1 D by set/way (as the main stub) */
"	mrc	p15, 0, r0, c1, c0, 0\n"
"	bic	r0, r0, #(1 << 2)\n"
"	mcr	p15, 0, r0, c1, c0, 0\n"
"	isb\n"
"	dmb\n"
"	mrc	p15, 1, r0, c0, c0, 1\n"
"	ands	r3, r0, #0x7000000\n"
"	mov	r3, r3, lsr #23\n"
"	beq	5f\n"
"	mov	r10, #0\n"
"1:	add	r2, r10, r10, lsr #1\n"
"	mov	r1, r0, lsr r2\n"
"	and	r1, r1, #7\n"
"	cmp	r1, #2\n"
"	blt	4f\n"
"	mcr	p15, 2, r10, c0, c0, 0\n"
"	isb\n"
"	mrc	p15, 1, r1, c0, c0, 0\n"
"	and	r2, r1, #7\n"
"	add	r2, r2, #4\n"
"	movw	r4, #0x3ff\n"
"	ands	r4, r4, r1, lsr #3\n"
"	clz	r5, r4\n"
"	movw	r7, #0x7fff\n"
"	ands	r7, r7, r1, lsr #13\n"
"2:	mov	r9, r4\n"
"3:	orr	r11, r10, r9, lsl r5\n"
"	orr	r11, r11, r7, lsl r2\n"
"	mcr	p15, 0, r11, c7, c14, 2\n"
"	subs	r9, r9, #1\n"
"	bge	3b\n"
"	subs	r7, r7, #1\n"
"	bge	2b\n"
"4:	add	r10, r10, #2\n"
"	cmp	r3, r10\n"
"	bgt	1b\n"
"5:	mov	r10, #0\n"
"	mcr	p15, 2, r10, c0, c0, 0\n"
"	dsb\n"
"	isb\n"
	/* MMU, branch predictor and I-cache off; we run at VA == PA */
"	mrc	p15, 0, r0, c1, c0, 0\n"
"	bic	r0, r0, #(1 << 0)\n"
"	bic	r0, r0, #(1 << 11)\n"
"	bic	r0, r0, #(1 << 12)\n"
"	mcr	p15, 0, r0, c1, c0, 0\n"
"	isb\n"
"	mov	r0, #0\n"
"	mcr	p15, 0, r0, c7, c5, 0\n"	/* ICIALLU */
"	mcr	p15, 0, r0, c7, c5, 6\n"	/* BPIALL  */
"	mcr	p15, 0, r0, c8, c7, 0\n"	/* TLBIALL */
"	dsb\n"
"	isb\n"
	/* this CPU's A9 private timer off (stock's local tick), its event flag cleared */
"	movw	r1, #0x2600\n"
"	movt	r1, #0xf200\n"
"	mov	r2, #0\n"
"	str	r2, [r1, #0x8]\n"
"	mov	r2, #1\n"
"	str	r2, [r1, #0xc]\n"
	/* this CPU's GIC CPU interface off; the next kernel's gic_cpu_init() turns it on */
"	movw	r1, #0x2100\n"
"	movt	r1, #0xf200\n"
"	mov	r2, #0\n"
"	str	r2, [r1]\n"
"	mrc	p15, 0, r1, c1, c0, 1\n"
"	str	r1, [r12, #0xff8]\n"
"	mrs	r1, cpsr\n"
"	str	r1, [r12, #0xffc]\n"
"	movw	r1, #0xc1a0\n"
"	movt	r1, #0x5398\n"
"	str	r1, [r12, #0xff0]\n"	/* HOLD_WAITING */
"	dsb\n"
"6:	wfe\n"
"	ldr	r1, [r12, #0xff4]\n"
"	cmp	r1, #0\n"
"	beq	6b\n"
"	movw	r2, #0xc1e0\n"
"	movt	r2, #0x5398\n"
"	str	r2, [r12, #0xff0]\n"	/* HOLD_RELEASED */
"	dsb\n"
"	bx	r1\n"
"	.global	nckexec_hold_end\n"
"nckexec_hold_end:\n"
"	.popsection\n"
);

extern const char nckexec_hold[], nckexec_hold_end[];

static u32 hold_pa;			/* from the DTB; 0 = no CPU1 hand-off */
static void __iomem *hold_va;

static struct task_struct *hold_task;

static int hold_thread(void *unused)
{
	void (*fn)(u32) = (void (*)(u32))hold_pa;

	local_irq_disable();
	local_fiq_disable();
	/* as ipi_cpu_stop(): nobody may wait for this CPU from now on */
	cpumask_clear_cpu(1, (struct cpumask *)cpu_online_mask);
	p_setup_mm_for_reboot(0);	/* 1:1 map in the mm this kthread borrowed */
	flush_cache_all();
	fn(hold_pa);
	return 0;			/* not reached */
}

/*
 * CPU0, after the shutdown: copy the stub, let the kthread take CPU1 there and wait for
 * HOLD_WAITING. CPU1 is idle by now, so this takes microseconds.
 */
static void hold_cpu1(void)
{
	unsigned int ms;
	u32 state = 0;

	memcpy_toio(hold_va, nckexec_hold, nckexec_hold_end - nckexec_hold);
	writel(0, hold_va + HOLD_ENTRY);
	writel(0, hold_va + HOLD_STATE);
	wmb();
	wake_up_process(hold_task);
	for (ms = 0; ms < 1000; ms++) {
		state = readl(hold_va + HOLD_STATE);
		if (state == HOLD_WAITING)
			break;
		mdelay(1);
	}
	if (state == HOLD_WAITING)
		pr_emerg("nckexec: CPU1 waits in the hold page 0x%08x (%u ms, ACTLR 0x%08x CPSR 0x%08x)\n",
			 hold_pa, ms, readl(hold_va + HOLD_ACTLR), readl(hold_va + HOLD_CPSR));
	else
		pr_emerg("nckexec: CPU1 did not reach the hold page (state 0x%08x): the next kernel "
			 "will leave it alone\n", state);
}

extern unsigned int __machine_arch_type;


/* ---- the relocation stub (copied into a staging page, run at its PA) ---- */

#define P_LIST	0	/* phys of the {dest, src, len} list, len 0 ends it */
#define P_ENTRY	4
#define P_MACH	8
#define P_ATAGS	12
#define P_L2VA	16	/* PL310 virtual base, 0 = no outer cache */
#define P_L2WAY	20	/* PL310 way mask */
#define P_SIZE	24

asm(
"	.pushsection .text\n"
"	.arm\n"
"	.align	5\n"
"	.global	nckexec_stub\n"
"nckexec_stub:\n"
"	adr	r12, nckexec_stub_params\n"
	/* 1. data cache off: from here no new lines are allocated */
"	mrc	p15, 0, r0, c1, c0, 0\n"
"	bic	r0, r0, #(1 << 2)\n"
"	mcr	p15, 0, r0, c1, c0, 0\n"
"	isb\n"
	/* 2. clean+invalidate L1 D by set/way (v7_flush_dcache_all, no stack) */
"	dmb\n"
"	mrc	p15, 1, r0, c0, c0, 1\n"
"	ands	r3, r0, #0x7000000\n"
"	mov	r3, r3, lsr #23\n"
"	beq	5f\n"
"	mov	r10, #0\n"
"1:	add	r2, r10, r10, lsr #1\n"
"	mov	r1, r0, lsr r2\n"
"	and	r1, r1, #7\n"
"	cmp	r1, #2\n"
"	blt	4f\n"
"	mcr	p15, 2, r10, c0, c0, 0\n"
"	isb\n"
"	mrc	p15, 1, r1, c0, c0, 0\n"
"	and	r2, r1, #7\n"
"	add	r2, r2, #4\n"
"	movw	r4, #0x3ff\n"
"	ands	r4, r4, r1, lsr #3\n"
"	clz	r5, r4\n"
"	movw	r7, #0x7fff\n"
"	ands	r7, r7, r1, lsr #13\n"
"2:	mov	r9, r4\n"
"3:	orr	r11, r10, r9, lsl r5\n"
"	orr	r11, r11, r7, lsl r2\n"
"	mcr	p15, 0, r11, c7, c14, 2\n"
"	subs	r9, r9, #1\n"
"	bge	3b\n"
"	subs	r7, r7, #1\n"
"	bge	2b\n"
"4:	add	r10, r10, #2\n"
"	cmp	r3, r10\n"
"	bgt	1b\n"
"5:	mov	r10, #0\n"
"	mcr	p15, 2, r10, c0, c0, 0\n"
"	dsb\n"
"	isb\n"
	/* 3. PL310 clean+invalidate all ways, then cache sync (as __l2x0_flush_all) */
"	ldr	r0, [r12, #16]\n"
"	ldr	r1, [r12, #20]\n"
"	cmp	r0, #0\n"
"	beq	7f\n"
"	str	r1, [r0, #0x7fc]\n"
"6:	ldr	r2, [r0, #0x7fc]\n"
"	tst	r2, r1\n"
"	bne	6b\n"
"	mov	r2, #0\n"
"	str	r2, [r0, #0x730]\n"
"6:	ldr	r2, [r0, #0x730]\n"
"	tst	r2, #1\n"
"	bne	6b\n"
"7:	dsb\n"
	/* 4. MMU, branch predictor and I-cache off; we run at VA == PA */
"	mrc	p15, 0, r0, c1, c0, 0\n"
"	bic	r0, r0, #(1 << 0)\n"
"	bic	r0, r0, #(1 << 11)\n"
"	bic	r0, r0, #(1 << 12)\n"
"	mcr	p15, 0, r0, c1, c0, 0\n"
"	isb\n"
"	mov	r0, #0\n"
"	mcr	p15, 0, r0, c7, c5, 0\n"	/* ICIALLU */
"	mcr	p15, 0, r0, c7, c5, 6\n"	/* BPIALL  */
"	mcr	p15, 0, r0, c8, c7, 0\n"	/* TLBIALL */
"	dsb\n"
"	isb\n"
	/* 5. copy {dest, src, len} segments; len is a multiple of 32 */
"	ldr	r8, [r12, #0]\n"
"8:	ldmia	r8!, {r9, r10, r11}\n"
"	cmp	r11, #0\n"
"	beq	10f\n"
"9:	ldmia	r10!, {r0-r7}\n"
"	stmia	r9!, {r0-r7}\n"
"	subs	r11, r11, #32\n"
"	bgt	9b\n"
"	b	8b\n"
"10:	dsb\n"
"	mov	r0, #0\n"
"	mcr	p15, 0, r0, c7, c5, 0\n"
"	mcr	p15, 0, r0, c7, c5, 6\n"
"	dsb\n"
"	isb\n"
	/* 6. enter the kernel: r0 = 0, r1 = machine, r2 = atags */
"	ldr	r3, [r12, #4]\n"
"	ldr	r1, [r12, #8]\n"
"	ldr	r2, [r12, #12]\n"
"	mov	r0, #0\n"
"	bx	r3\n"
"	.align	2\n"
"	.global	nckexec_stub_params\n"
"nckexec_stub_params:\n"
"	.space	24\n"
"	.global	nckexec_stub_end\n"
"nckexec_stub_end:\n"
"	.popsection\n"
);

extern const char nckexec_stub[], nckexec_stub_params[], nckexec_stub_end[];

/* ---- staging state ---- */

struct seg {
	u32 dest, src, len;
};

#define ATAGS_OFF	2048		/* ATAGs live in the stub page from here */

static unsigned long *img_pages;	/* kernel VAs of the image pages        */
static unsigned int nimg;
static unsigned long stub_page;		/* VA of the stub + params + ATAGs page */
static unsigned long list_va;		/* VA of the segment list               */
static unsigned int list_order;
static unsigned long img_size;
static void __iomem *l2_base;
static u32 l2_ways;
static unsigned long dtb_va;		/* VA of the staged DTB (physically contiguous) */
static unsigned int dtb_order;
static size_t dtb_len;

/* Pages below lowmark go here until staging is done, so we don't get them again. */
struct reject {
	struct reject *next;
	unsigned int order;
};
static struct reject *rejects;

static unsigned long alloc_high(unsigned int order)
{
	int tries;

	for (tries = 0; tries < 100000; tries++) {
		unsigned long va = __get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
		struct reject *r;

		if (!va)
			return 0;
		if (virt_to_phys((void *)va) >= lowmark)
			return va;
		r = (struct reject *)va;
		r->order = order;
		r->next = rejects;
		rejects = r;
	}
	return 0;
}

static unsigned int free_rejects(void)
{
	unsigned int n = 0;

	while (rejects) {
		struct reject *r = rejects;

		rejects = r->next;
		free_pages((unsigned long)r, r->order);
		n++;
	}
	return n;
}

static void free_staging(void)
{
	unsigned int i;

	if (img_pages) {
		for (i = 0; i < nimg; i++)
			if (img_pages[i])
				free_page(img_pages[i]);
		vfree(img_pages);
		img_pages = NULL;
	}
	nimg = 0;
	if (list_va)
		free_pages(list_va, list_order);
	list_va = 0;
	if (stub_page)
		free_page(stub_page);
	stub_page = 0;
	if (dtb_va)
		free_pages(dtb_va, dtb_order);
	dtb_va = 0;
	dtb_len = 0;
}

/* A region header we can read through the linear map: below the hole and not highmem. */
static bool osd_hdr_readable(u32 h)
{
	return h && !(h & 0xf) && h + 32 <= RAM_HOLE_LO && pfn_valid(h >> PAGE_SHIFT) &&
	       !PageHighMem(pfn_to_page(h >> PAGE_SHIFT));
}

/* "dump": log what the OSD shows right now (reads only), for a plane 3 that isn't LG's layer. */
static void osd_dump(void)
{
	unsigned int i;
	u32 h;

	pr_info("nckexec: dump: OSD base %08x, SC3 %08x src %08x dst %08x\n",
		readl(OSD_BASE), readl(OSD_SC3), readl(OSD_SC3 + 4), readl(OSD_SC3 + 8));
	for (i = 1; i <= 3; i++) {
		void __iomem *p = OSD_BASE + i * 0x100;

		pr_info("nckexec: dump: plane %u ctrl %08x hdr %08x blend %08x fetched %08x %08x %08x\n",
			i, readl(p), readl(p + 4), readl(p + 8),
			readl(p + 0x20), readl(p + 0x24), readl(p + 0x30));
	}
	h = (readl(OSD_PLA3 + 4) & 0x0fffffff) << 4;
	if (osd_hdr_readable(h)) {
		const u32 *w = phys_to_virt(h);

		pr_info("nckexec: dump: header 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			h, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	} else {
		pr_info("nckexec: dump: header 0x%08x is not in readable lowmem\n", h);
	}
}

/* Read a live OSD plane's state; returns 0 (fb, hdr filled) only if it is LG's 1280x720 ARGB layer. */
static int osd_probe_plane(unsigned int n, u32 *fb, u32 *hdr)
{
	u32 pla0 = readl(OSD_PLA(n)), pla1 = readl(OSD_PLA(n) + 4);
	u32 h = (pla1 & 0x0fffffff) << 4;
	const u32 *w;
	u32 base;

	if (!(pla0 & 1)) {
		pr_err("nckexec: OSD plane %u is off (0x%08x)\n", n, pla0);
		return -ENODEV;
	}
	if (!osd_hdr_readable(h)) {
		pr_err("nckexec: OSD plane %u header 0x%08x not in low System RAM\n", n, h);
		return -EINVAL;
	}
	w = phys_to_virt(h);
	if ((w[0] >> 28) != 14 || (w[0] & (1 << 27)) ||		/* ARGB8888, no chained region */
	    (w[4] & 0xfff) != OSD_W || ((w[4] >> 12) & 0x7ff) != OSD_H ||
	    (w[4] >> 23) != OSD_W * 4 / 16) {
		pr_err("nckexec: OSD plane %u region @0x%08x unexpected: %08x %08x %08x %08x %08x\n",
		       n, h, w[0], w[1], w[2], w[4], w[7]);
		return -EINVAL;
	}
	base = ((w[1] & 0xffffff) << 4) | (((w[2] >> 24) & 3) << 28) | (((w[7] >> 27) & 3) << 30);
	if (base < RAM_HOLE_LO || base + OSD_W * OSD_H * 4 > RAM_HOLE_HI) {
		pr_err("nckexec: OSD bitmap 0x%08x outside the RAM hole\n", base);
		return -EINVAL;
	}
	*fb = base;
	*hdr = h & PAGE_MASK;
	return 0;
}

/* The plane that gets handed over is always 3. */
static int osd_probe(u32 *fb, u32 *hdr)
{
	return osd_probe_plane(3, fb, hdr);
}

/* Replace every big-endian cookie cell in the staged DTB; returns the count. */
static unsigned int dtb_patch(u32 cookie, u32 val)
{
	__be32 *p = (__be32 *)dtb_va;
	unsigned int i, n = 0;

	for (i = 0; i < dtb_len / 4; i++)
		if (be32_to_cpu(p[i]) == cookie) {
			p[i] = cpu_to_be32(val);
			n++;
		}
	return n;
}

static unsigned int dtb_count(u32 cookie)
{
	__be32 *p = (__be32 *)dtb_va;
	unsigned int i, n = 0;

	for (i = 0; i < dtb_len / 4; i++)
		n += be32_to_cpu(p[i]) == cookie;
	return n;
}

/* Value of the cell that follows mark in the DTB: 0 = no such request, ~0 = malformed. */
static u32 dtb_mark(u32 mark)
{
	__be32 *p = (__be32 *)dtb_va;
	unsigned int i;
	u32 val = 0;

	if (!dtb_va)
		return 0;
	for (i = 0; i + 1 < dtb_len / 4; i++)
		if (be32_to_cpu(p[i]) == mark) {
			if (val)
				return ~0u;	/* more than one request */
			val = be32_to_cpu(p[i + 1]);
			if (!val)
				return ~0u;
		}
	return val;
}

static u32 dtb_osd1080(void)
{
	return dtb_mark(OSD_1080_MARK);
}

/* Is the DTB's CPU1 hold page usable? Maps it (hold_va) on success. */
static int hold_check(u32 pa)
{
	size_t len = nckexec_hold_end - nckexec_hold;

	if (pa == ~0u || (pa & ~PAGE_MASK) || pa < RAM_HOLE_LO || pa + PAGE_SIZE > TRUSTZONE_LO ||
	    pfn_valid(pa >> PAGE_SHIFT) || len > HOLD_STATE) {
		pr_err("nckexec: CPU1 hold page 0x%08x unusable (page in the RAM hole below 0x%08lx, "
		       "stub %u bytes)\n", pa, TRUSTZONE_LO, (unsigned)len);
		return -EINVAL;
	}
	/* The mark's own cell + the reserved-memory reg the next kernel finds it by. */
	if (dtb_count(pa) < 2) {
		pr_err("nckexec: DTB asks to hold CPU1 at 0x%08x but has no node for it\n", pa);
		return -EINVAL;
	}
	if (!hold_va)
		hold_va = ioremap(pa, PAGE_SIZE);
	return hold_va ? 0 : -ENOMEM;
}

/* In the 1080p bitmap or the own-header page after it? */
static bool in_fb(unsigned long va, u32 fb)
{
	unsigned long pa = virt_to_phys((void *)va);

	return pa + PAGE_SIZE > fb && pa < OSD_OWN_HDR(fb) + PAGE_SIZE;
}

/* Is the DTB's 1080p buffer usable? Checked while backing out is still possible. */
static int osd1080_check(u32 fb)
{
	unsigned int i;

	if (fb == ~0u || (fb & ~PAGE_MASK) || fb < lowmark || OSD_OWN_HDR(fb) + PAGE_SIZE > RAM_BANK0_HI ||
	    !pfn_valid(fb >> PAGE_SHIFT) || !pfn_valid(OSD_OWN_HDR(fb) >> PAGE_SHIFT)) {
		pr_err("nckexec: 1080p buffer 0x%08x unusable (page aligned, + header page in bank 0 below 0x%08lx)\n",
		       fb, RAM_BANK0_HI);
		return -EINVAL;
	}
	/* The mark's own cell + the simple-framebuffer reg + the reserved-memory reg. */
	if (dtb_count(fb) < 3 || dtb_count(OSD_FB_COOKIE)) {
		pr_err("nckexec: DTB asks for 1080p at 0x%08x but does not reserve it (%u cells) or "
		       "still has the 720p framebuffer cookie (%u)\n", fb, dtb_count(fb),
		       dtb_count(OSD_FB_COOKIE));
		return -EINVAL;
	}
	/* Staging is GFP_KERNEL = stock lowmem, far below bank 0's top; make sure anyway. */
	if (in_fb(stub_page, fb) || in_fb(list_va, fb) || in_fb(dtb_va, fb))
		return -EBUSY;
	for (i = 0; i < nimg; i++)
		if (in_fb(img_pages[i], fb))
			return -EBUSY;
	return 0;
}

/*
 * Retarget OSD plane 3 to the 1920x1080 buffer at fb (IRQs off, stock shut down). With
 * own == false the region header is LG's, validated by osd_probe() just before: the OSD
 * fetches it from DRAM every frame (plane +0x20 mirrors what it fetched), so it is edited
 * in place, as LG's compositor does for flips. With own == true nckexec writes its own
 * header (OSD_OWN_HDR) from the template and points plane 3 at it. SC3 and the update
 * strobe are LG's OSD_SC_Scale() 1:1 branch and _OSD_UpdateReg().
 */
static void osd1080_go(u32 fb, bool own)
{
	u32 h = own ? OSD_OWN_HDR(fb) : (readl(OSD_PLA3 + 4) & 0x0fffffff) << 4;
	u32 *w, *map = NULL;
	unsigned long pfn;
	unsigned int i;

	for (pfn = fb >> PAGE_SHIFT; pfn < (fb + FB_SIZE + PAGE_SIZE - 1) >> PAGE_SHIFT; pfn++) {
		u32 *p = kmap_atomic(pfn_to_page(pfn));

		for (i = 0; i < PAGE_SIZE / 4; i++)
			p[i] = 0xff000000;	/* opaque black */
		kunmap_atomic(p);
	}

	if (own) {	/* stock highmem: no linear mapping */
		map = kmap_atomic(pfn_to_page(h >> PAGE_SHIFT));
		w = map;
		memset(w, 0, PAGE_SIZE);
		memcpy(w, osd_hdr_1080, sizeof(osd_hdr_1080));
	} else {
		w = phys_to_virt(h);
	}
	w[1] = (w[1] & 0xff000000) | ((fb >> 4) & 0xffffff);		/* data addr [27:4] */
	w[2] = (w[2] & ~(3u << 24)) | (((fb >> 28) & 3) << 24);		/* data addr [29:28] */
	w[4] = FB_W | (FB_H << 12) | ((FB_W * 4 / 16) << 23);		/* in w/h, line size */
	w[6] = (w[6] & ~0x7ffu) | FB_H;					/* out height */
	w[7] = (w[7] & ~(0x7ffu | (3u << 27))) | FB_W | (((fb >> 30) & 3) << 27);
	flush_cache_all();
	if (map)
		kunmap_atomic(map);
	if (outer_cache.flush_all)
		outer_cache.flush_all();

	if (own) {
		/*
		 * LG's UI may be on plane 2 (seen 2026-11-24, plane 3 then holds a 64x64 pointer or
		 * is off): with LG's compositor gone its last picture would stay up next to ours.
		 */
		writel(readl(OSD_PLA(1)) & ~1u, OSD_PLA(1));
		writel(readl(OSD_PLA(2)) & ~1u, OSD_PLA(2));
		writel((readl(OSD_PLA3 + 4) & 0xf0000000) | (h >> 4), OSD_PLA3 + 4);
		writel(OSD_BLEND_ON, OSD_PLA3 + 8);
		writel(readl(OSD_PLA3) | 1, OSD_PLA3);	/* plane on */
	}

	writel(readl(OSD_SC3) & ~0x3cu, OSD_SC3);	/* V/H up/down scalers off, ScEn kept */
	writel((FB_W << 16) | FB_H, OSD_SC3 + 0x04);	/* src */
	writel((FB_W << 16) | FB_H, OSD_SC3 + 0x08);	/* dst */
	writel(FB_W, OSD_SC3 + 0x0c);			/* vsc h size */
	for (i = 0x10; i <= 0x1c; i += 4)
		writel(0, OSD_SC3 + i);			/* steps and offsets */
	writel(readl(OSD_BASE) | 1, OSD_BASE);		/* update */
	writel(readl(OSD_BASE) | 8, OSD_BASE);		/* update OSD1 settings */
}

/* Stage the DTB at atags_at instead of ATAGs. */
static int stage_dtb(void)
{
	struct file *f;
	loff_t fsz;
	__be32 *hd;
	int ret;

	f = filp_open(dtb, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	fsz = i_size_read(f->f_path.dentry->d_inode);
	ret = -EFBIG;
	if (fsz < 40 || atags_at + ALIGN(fsz, 32) > 0x4000)	/* 0x4000: the next kernel's page tables */
		goto out;
	dtb_order = get_order(ALIGN(fsz, 32));
	ret = -ENOMEM;
	dtb_va = alloc_high(dtb_order);
	if (!dtb_va)
		goto out;
	ret = kernel_read(f, 0, (char *)dtb_va, fsz);
	if (ret != fsz) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	hd = (__be32 *)dtb_va;
	ret = -EINVAL;
	if (be32_to_cpu(hd[0]) != 0xd00dfeed || be32_to_cpu(hd[1]) != fsz) {
		pr_err("nckexec: %s is not a DTB (or totalsize != file size)\n", dtb);
		goto out;
	}
	dtb_len = fsz;
	pr_info("nckexec: DTB %u bytes, %u framebuffer and %u header cookies, 1080p request 0x%08x, "
		"CPU1 hold 0x%08x\n", (unsigned)fsz, dtb_count(OSD_FB_COOKIE), dtb_count(OSD_HDR_COOKIE),
		dtb_osd1080(), dtb_mark(CPU1_HOLD_MARK));
	ret = 0;
out:
	filp_close(f, NULL);
	return ret;
}

/*
 * Find the PL310 the stock kernel uses. l2x0_base is static and not in
 * kallsyms, but l2x0_disable() starts "push; ldr r5, =&l2x0_base", and
 * l2x0_way_mask follows l2x0_base (both confirmed in the stock Image).
 */
static int find_l2(void)
{
	unsigned long fn = kallsyms_lookup_name("l2x0_disable");
	u32 insn, id, *var;

	if (!outer_cache.flush_all) {
		l2_base = NULL;		/* no outer cache to maintain */
		return 0;
	}
	if (!fn)
		return -ENOENT;
	insn = *(u32 *)(fn + 4);
	if ((insn & 0xfffff000) != 0xe59f5000)
		return -ENOEXEC;
	var = (u32 *)*(u32 *)(fn + 4 + 8 + (insn & 0xfff));
	l2_base = (void __iomem *)var[0];
	l2_ways = var[1];
	if (!l2_base)
		return -ENODEV;
	id = readl(l2_base);
	if ((id >> 24) != 0x41 || ((id >> 6) & 0xf) != 3 ||
	    (l2_ways != 0xff && l2_ways != 0xffff))
		return -ENODEV;
	pr_info("nckexec: PL310 id 0x%08x at %p, ways 0x%x, %s\n", id, l2_base,
		l2_ways, (readl(l2_base + 0x100) & 1) ? "enabled" : "disabled");
	return 0;
}

static int build_atags(void *buf, size_t max, const char *cl)
{
	struct tag *t = buf;
	struct resource *r;
	size_t len = strlen(cl) + 1;
	int nmem = 0;

#define ROOM(sz) do { if ((char *)t + (sz) + 8 > (char *)buf + max) return -E2BIG; } while (0)
	ROOM(sizeof(struct tag_header) + sizeof(struct tag_core));
	t->hdr.tag = ATAG_CORE;
	t->hdr.size = tag_size(tag_core);
	t->u.core.flags = 0;
	t->u.core.pagesize = PAGE_SIZE;
	t->u.core.rootdev = 0;
	t = tag_next(t);

	for (r = iomem_resource.child; r; r = r->sibling) {
		if (!r->name || strcmp(r->name, "System RAM"))
			continue;
		ROOM(sizeof(struct tag_header) + sizeof(struct tag_mem32));
		t->hdr.tag = ATAG_MEM;
		t->hdr.size = tag_size(tag_mem32);
		t->u.mem.start = r->start;
		t->u.mem.size = r->end - r->start + 1;
		pr_info("nckexec: ATAG_MEM 0x%08x @ 0x%08x\n",
			t->u.mem.size, t->u.mem.start);
		t = tag_next(t);
		nmem++;
	}
	if (!nmem)
		return -ENODEV;

	ROOM(sizeof(struct tag_header) + len + 3);
	t->hdr.tag = ATAG_CMDLINE;
	t->hdr.size = (sizeof(struct tag_header) + len + 3) >> 2;
	memcpy(t->u.cmdline.cmdline, cl, len);
	t = tag_next(t);

	t->hdr.tag = ATAG_NONE;
	t->hdr.size = 0;
	return (char *)t + sizeof(struct tag_header) - (char *)buf;
#undef ROOM
}

static int nckexec_stage(void)
{
	struct file *f;
	loff_t pos = 0, fsz;
	mm_segment_t fs;
	char *cl = NULL, **saved;
	const char *base;
	size_t stub_len = nckexec_stub_end - nckexec_stub;
	struct seg *sg;
	u32 *params;
	unsigned int i, nrej;
	int alen, ret;
	__be32 magic = 0;

	f = filp_open(image, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	fsz = i_size_read(f->f_path.dentry->d_inode);

	fs = get_fs();
	set_fs(KERNEL_DS);

	/* uImage? skip its 64-byte header */
	if (vfs_read(f, (char __user __force *)&magic, 4, &pos) == 4 &&
	    be32_to_cpu(magic) == 0x27051956)
		pos = 64;
	else
		pos = 0;
	img_size = fsz - pos;
	if (fsz <= pos || img_size > 32 << 20) {
		ret = -EFBIG;
		goto out;
	}

	nimg = DIV_ROUND_UP(img_size, PAGE_SIZE);
	img_pages = vzalloc(nimg * sizeof(*img_pages));
	ret = -ENOMEM;
	if (!img_pages)
		goto out;
	for (i = 0; i < nimg; i++) {
		size_t want = min_t(size_t, PAGE_SIZE, img_size - (size_t)i * PAGE_SIZE);
		ssize_t rd;

		img_pages[i] = alloc_high(0);
		if (!img_pages[i])
			goto out;
		rd = vfs_read(f, (char __user __force *)img_pages[i], want, &pos);
		if (rd != (ssize_t)want) {
			ret = rd < 0 ? (int)rd : -EIO;
			goto out;
		}
	}

	/* stub + params + ATAGs */
	ret = -ENOMEM;
	stub_page = alloc_high(0);
	if (!stub_page)
		goto out;
	if (stub_len + P_SIZE > ATAGS_OFF) {
		ret = -E2BIG;
		goto out;
	}
	memcpy((void *)stub_page, nckexec_stub, stub_len);

	/* command line */
	cl = kmalloc(COMMAND_LINE_SIZE, GFP_KERNEL);
	if (!cl)
		goto out;
	base = cmdline;
	if (!base) {
		saved = (char **)kallsyms_lookup_name("saved_command_line");
		base = (saved && *saved) ? *saved : "";
	}
	ret = -E2BIG;
	if (snprintf(cl, COMMAND_LINE_SIZE, "%s%s%s", base,
		     append ? " " : "", append ? append : "") >= COMMAND_LINE_SIZE)
		goto out;
	/* busybox insmod drops quotes, so append= separates its arguments with ';' */
	if (append) {
		char *p;

		for (p = cl + strlen(base); *p; p++)
			if (*p == ';')
				*p = ' ';
	}
	if (dtb) {
		ret = stage_dtb();
		if (ret)
			goto out;
		alen = dtb_len;
	} else {
		alen = build_atags((void *)(stub_page + ATAGS_OFF), PAGE_SIZE - ATAGS_OFF, cl);
		if (alen < 0) {
			ret = alen;
			goto out;
		}
	}

	/* segment list: ATAGs, then each image page, then the terminator */
	list_order = get_order((nimg + 2) * sizeof(struct seg));
	ret = -ENOMEM;
	list_va = alloc_high(list_order);
	if (!list_va)
		goto out;
	sg = (struct seg *)list_va;
	sg->dest = atags_at;
	sg->src = virt_to_phys((void *)(dtb ? dtb_va : stub_page + ATAGS_OFF));
	sg->len = ALIGN(alen, 32);
	sg++;
	for (i = 0; i < nimg; i++, sg++) {
		sg->dest = load + i * PAGE_SIZE;
		sg->src = virt_to_phys((void *)img_pages[i]);
		sg->len = PAGE_SIZE;
	}
	sg->dest = sg->src = sg->len = 0;

	/* The destination must not overlap anything the stub reads. */
	ret = -EINVAL;
	if (load + nimg * PAGE_SIZE > lowmark || atags_at + ALIGN(alen, 32) > load ||
	    (load & (PAGE_SIZE - 1)) || (atags_at & 3))
		goto out;

	params = (u32 *)(stub_page + (nckexec_stub_params - nckexec_stub));
	params[P_LIST / 4]  = virt_to_phys((void *)list_va);
	params[P_ENTRY / 4] = load;
	params[P_MACH / 4]  = dtb ? ~0u : (mach ? mach : __machine_arch_type);
	params[P_ATAGS / 4] = atags_at;
	params[P_L2VA / 4]  = (u32)l2_base;
	params[P_L2WAY / 4] = l2_ways;

	if (dtb)
		pr_info("nckexec: DT boot: cmdline comes from the DTB, cmdline=/append= ignored\n");
	else
		pr_info("nckexec: cmdline: %s\n", cl);
	pr_info("nckexec: %u image pages (%lu bytes) -> phys 0x%08lx, atags/dtb %d bytes -> 0x%08lx, "
		"mach %u, stub 0x%08lx, list 0x%08lx\n",
		nimg, img_size, load, alen, atags_at, params[P_MACH / 4],
		(unsigned long)virt_to_phys((void *)stub_page), (unsigned long)params[P_LIST / 4]);
	ret = 0;
out:
	set_fs(fs);
	filp_close(f, NULL);
	kfree(cl);
	nrej = free_rejects();
	if (nrej)
		pr_info("nckexec: skipped %u allocations below lowmark\n", nrej);
	if (ret)
		free_staging();
	return ret;
}

static int nckexec_fire(void)
{
	void (*stub)(void);
	u32 fb1080 = dtb_osd1080();
	bool osd = dtb_va && (dtb_count(OSD_FB_COOKIE) || dtb_count(OSD_HDR_COOKIE) || fb1080);
	u32 fb = 0, hdr = 0, fb2, hdr2;
	bool own = false;
	int ret;

	if (!stub_page)
		return -ENODEV;
	hold_pa = dtb_mark(CPU1_HOLD_MARK);
	if (hold_pa) {
		ret = hold_check(hold_pa);
		if (ret)
			return ret;
		if (num_online_cpus() != 2 || !cpu_online(1)) {
			pr_err("nckexec: the DTB holds CPU1 for the next kernel: bring it online "
			       "(echo 1 > /sys/devices/system/cpu/cpu1/online)\n");
			return -EBUSY;
		}
		set_cpus_allowed_ptr(current, cpumask_of(0));	/* fire from CPU0 */
		if (raw_smp_processor_id() != 0)
			return -EBUSY;
	} else if (num_online_cpus() != 1 || raw_smp_processor_id() != 0) {
		pr_err("nckexec: offline CPU1 first (echo 0 > /sys/devices/system/cpu/cpu1/online)\n");
		return -EBUSY;
	}
	/* Validate the OSD hand-off while backing out is still clean. */
	if (osd) {
		ret = osd_probe(&fb, &hdr);
		if (ret && !fb1080)
			return ret;
		if (ret)	/* the 1080p hand-off doesn't need LG's layer */
			pr_info("nckexec: OSD plane 3 not LG's layer: own header at 0x%08x\n",
				OSD_OWN_HDR(fb1080));
		else
			pr_info("nckexec: OSD plane 3: bitmap 0x%08x, header page 0x%08x\n", fb, hdr);
	}
	if (fb1080) {
		ret = osd1080_check(fb1080);
		if (ret)
			return ret;
		pr_info("nckexec: OSD plane 3 -> 1920x1080 at 0x%08x after shutdown\n", fb1080);
	}

	if (hold_pa) {
		hold_task = kthread_create(hold_thread, NULL, "nckexec-cpu1");
		if (IS_ERR(hold_task))
			return PTR_ERR(hold_task);
		kthread_bind(hold_task, 1);	/* runs once woken, after the shutdown */
	}

	pr_emerg("nckexec: booting staged kernel @0x%08lx\n", load);

	if (shutdown)
		p_kernel_restart_prepare(NULL);
	if (hold_pa)
		hold_cpu1();
	p_machine_shutdown();		/* SMP stop: nothing left to stop */

	/*
	 * The compositor may have flipped (or switched plane 3 off) during shutdown: take the
	 * final state if still valid; for 1080p, write an own header if it isn't.
	 */
	if (osd) {
		if (!osd_probe(&fb2, &hdr2)) {
			fb = fb2;
			hdr = hdr2;
		} else if (fb1080) {
			own = true;
			hdr = OSD_OWN_HDR(fb1080);
		}
		dtb_patch(OSD_FB_COOKIE, fb);
		dtb_patch(OSD_HDR_COOKIE, hdr);
	}

	local_irq_disable();
	local_fiq_disable();
	if (usbstop)
		musb_dma_stop();
	if (gpustop)
		sgx_stop();

	/*
	 * With IRQs off LG's OSD vsync handler can no longer apply a pending flip over ours.
	 * Three frames later, report what the OSD latched through the DTB's readback cookies
	 * (/proc/device-tree/chosen/framebuffer@.../nckexec,osd-readback in the new kernel).
	 */
	if (fb1080) {
		osd1080_go(fb1080, own);
		mdelay(50);
		dtb_patch(OSD_RB_COOKIE(0), readl(OSD_PLA3 + 0x30));	/* fetched region w4 */
		dtb_patch(OSD_RB_COOKIE(1), readl(OSD_PLA3 + 0x24));	/* fetched region w1 */
		dtb_patch(OSD_RB_COOKIE(2), readl(OSD_SC3 + 0x04));	/* SC3 src size */
		dtb_patch(OSD_RB_COOKIE(3), readl(OSD_SC3));		/* SC3 control */
	}

	p_setup_mm_for_reboot(0);
	flush_cache_all();
	if (outer_cache.flush_all)
		outer_cache.flush_all();

	stub = (void (*)(void))virt_to_phys((void *)stub_page);
	stub();
	/* not reached */
	return 0;
}

static int go_set(const char *val, const struct kernel_param *kp)
{
	bool fire;
	u32 fb, hdr;
	int ret;

	/*
	 * "layer": is LG's 1280x720 ARGB layer (an app window) up right now? On plane 3, or, for a
	 * 1080p hand-off (which then brings its own header for plane 3), on plane 2. The write
	 * fails otherwise, so fire.sh can put a splash app up first.
	 */
	if (sysfs_streq(val, "layer")) {
		ret = osd_probe(&fb, &hdr);
		if (ret && dtb_osd1080() && !osd_probe_plane(2, &fb, &hdr)) {
			pr_info("nckexec: LG's layer is on plane 2: plane 3 gets an own header\n");
			ret = 0;
		}
		return ret;
	}
	if (sysfs_streq(val, "dump")) {
		osd_dump();
		return 0;
	}
	/* "probe": dry run of the OSD hand-off check, logs what fire would patch in. */
	if (sysfs_streq(val, "probe")) {
		u32 fb1080 = dtb_osd1080();
		u32 hold = dtb_mark(CPU1_HOLD_MARK);

		if (hold) {
			ret = hold_check(hold);
			if (ret)
				return ret;
			pr_info("nckexec: probe: would park CPU1 (%s) in the hold page 0x%08x, stub %u bytes "
				"(page now: state %08x entry %08x)\n", cpu_online(1) ? "online" : "OFFLINE: refused",
				hold, (unsigned)(nckexec_hold_end - nckexec_hold),
				readl(hold_va + HOLD_STATE), readl(hold_va + HOLD_ENTRY));
		}

		ret = osd_probe(&fb, &hdr);
		if (!ret)
			pr_info("nckexec: probe: OSD plane 3 bitmap 0x%08x, header page 0x%08x\n", fb, hdr);
		if (fb1080) {
			if (ret)
				pr_info("nckexec: probe: plane 3 (ctrl %08x hdr %08x) not LG's layer: "
					"would write an own header at 0x%08x and enable it\n",
					readl(OSD_PLA3), readl(OSD_PLA3 + 4), OSD_OWN_HDR(fb1080));
			ret = osd1080_check(fb1080);
			if (!ret)
				pr_info("nckexec: probe: would switch plane 3 to 1920x1080 at 0x%08x "
					"(SC3 now %08x src %08x)\n", fb1080, readl(OSD_SC3),
					readl(OSD_SC3 + 4));
		}
		return ret;
	}
	ret = strtobool(val, &fire);
	if (ret)
		return ret;
	if (!fire)
		return 0;
	return nckexec_fire();		/* returns only on failure */
}

static const struct kernel_param_ops go_ops = {
	.set = go_set,
};
module_param_cb(go, &go_ops, NULL, 0200);
MODULE_PARM_DESC(go, "write 1 to boot the staged kernel (irreversible)");

static int __init nckexec_init(void)
{
	int ret;

	if (!image) {
		pr_err("nckexec: pass image=/path/to/Image\n");
		return -EINVAL;
	}

	p_setup_mm_for_reboot = (void *)kallsyms_lookup_name("setup_mm_for_reboot");
	p_machine_shutdown = (void *)kallsyms_lookup_name("machine_shutdown");
	p_kernel_restart_prepare = (void *)kallsyms_lookup_name("kernel_restart_prepare");
	if (!p_setup_mm_for_reboot || !p_machine_shutdown || !p_kernel_restart_prepare) {
		pr_err("nckexec: could not resolve reboot helpers\n");
		return -ENOENT;
	}

	ret = find_l2();
	if (ret) {
		pr_err("nckexec: could not locate the PL310: %d\n", ret);
		return ret;
	}

	ret = nckexec_stage();
	if (ret) {
		pr_err("nckexec: staging '%s' failed: %d\n", image, ret);
		return ret;
	}
	pr_info("nckexec: ready; %s, then echo 1 > /sys/module/nckexec/parameters/go\n",
		dtb_mark(CPU1_HOLD_MARK) ? "CPU1 online (it is parked for the next kernel)" : "offline CPU1");
	return 0;
}

static void __exit nckexec_exit(void)
{
	free_staging();
	if (hold_va)
		iounmap(hold_va);
	pr_info("nckexec: unloaded (staging freed)\n");
}

module_init(nckexec_init);
module_exit(nckexec_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ncplus");
MODULE_DESCRIPTION("Minimal runtime kexec (warm raw-Image boot) for CONFIG_KEXEC-less NetCast kernels");
