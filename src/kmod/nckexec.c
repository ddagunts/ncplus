// SPDX-License-Identifier: GPL-2.0
/*
 * nckexec: a small "kexec" for the LG NetCast 3.0.13 ARM kernel.
 *
 * The stock kernel is built with CONFIG_KEXEC *off*, so the kexec_load()
 * syscall and kernel_kexec() do not exist. This module reimplements the ARM
 * warm-boot path to hand control to another kernel image at runtime, without
 * touching flash.
 *
 * Why this matters on NetCast: U-Boot RSA-verifies the kernel on every boot
 * and there is no way to persist a modified kernel (see FINDINGS.md / the
 * secure-boot notes). But the *running*, already-verified kernel can launch
 * another kernel in RAM -- the verification only gates boot from flash.
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
 *                                 stops USB/Ethernet DMA into RAM we hand over
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
 * CPU1 must be offline first: echo 0 > /sys/devices/system/cpu/cpu1/online.
 * This kernel's platform_cpu_die() flushes its L1, leaves SMP coherency and
 * waits in WFI for pen_release -- a clean park. The next kernel must then be
 * booted with maxcpus=1, because waking CPU1 out of that loop would return it
 * into the previous kernel's hotplug path.
 *
 * Usage:
 *   echo 0 > /sys/devices/system/cpu/cpu1/online
 *   insmod nckexec.ko image=/path/Image append='maxcpus=1;panic=5'
 *   (append= takes ';' between arguments: busybox insmod drops quotes)
 *   echo 1 > /sys/module/nckexec/parameters/go      # point of no return
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

extern unsigned int __machine_arch_type;

/* Unexported helpers, resolved via kallsyms at load time. */
static void (*p_setup_mm_for_reboot)(char mode);
static void (*p_machine_shutdown)(void);
static void (*p_kernel_restart_prepare)(char *cmd);

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
	alen = build_atags((void *)(stub_page + ATAGS_OFF), PAGE_SIZE - ATAGS_OFF, cl);
	if (alen < 0) {
		ret = alen;
		goto out;
	}

	/* segment list: ATAGs, then each image page, then the terminator */
	list_order = get_order((nimg + 2) * sizeof(struct seg));
	ret = -ENOMEM;
	list_va = alloc_high(list_order);
	if (!list_va)
		goto out;
	sg = (struct seg *)list_va;
	sg->dest = atags_at;
	sg->src = virt_to_phys((void *)(stub_page + ATAGS_OFF));
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
	params[P_MACH / 4]  = mach ? mach : __machine_arch_type;
	params[P_ATAGS / 4] = atags_at;
	params[P_L2VA / 4]  = (u32)l2_base;
	params[P_L2WAY / 4] = l2_ways;

	pr_info("nckexec: cmdline: %s\n", cl);
	pr_info("nckexec: %u image pages (%lu bytes) -> phys 0x%08lx, atags %d bytes -> 0x%08lx, "
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

	if (!stub_page)
		return -ENODEV;
	if (num_online_cpus() != 1 || raw_smp_processor_id() != 0) {
		pr_err("nckexec: offline CPU1 first (echo 0 > /sys/devices/system/cpu/cpu1/online)\n");
		return -EBUSY;
	}

	pr_emerg("nckexec: booting staged kernel @0x%08lx\n", load);

	if (shutdown)
		p_kernel_restart_prepare(NULL);
	p_machine_shutdown();

	local_irq_disable();
	local_fiq_disable();

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
	int ret = strtobool(val, &fire);

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
	pr_info("nckexec: ready; offline CPU1, then echo 1 > /sys/module/nckexec/parameters/go\n");
	return 0;
}

static void __exit nckexec_exit(void)
{
	free_staging();
	pr_info("nckexec: unloaded (staging freed)\n");
}

module_init(nckexec_init);
module_exit(nckexec_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NetCast+");
MODULE_DESCRIPTION("Minimal runtime kexec (warm raw-Image boot) for CONFIG_KEXEC-less NetCast kernels");
