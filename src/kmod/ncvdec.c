// SPDX-License-Identifier: GPL-2.0
/*
 * ncvdec: log the H.264 decoder register traffic of LG's MediaTek driver (stock 3.0.13 kernel).
 *
 * The decoder has no firmware: the driver (mtk_mod) programs it with inline loads and stores,
 * so hooking functions would miss most of it, and the stock kernel has no kprobes. ncvdec
 * replaces the load/store instructions listed in ncvdec_sites.h with a permanently undefined
 * instruction; an undef hook then logs each access, performs it and steps over it. The header
 * parser's bitstream reads are included: they consume bits, so they show where slice data starts.
 *
 * Nothing is patched at insmod. Patching and unpatching run under stop_machine(), and every
 * site is checked first (symbol found, in one module, instruction of the expected kind). When
 * the buffer is full, recording stops and the driver is unpatched by itself.
 *
 * Bring-up stages (each one returns by itself; run them in order):
 *   echo N > /sys/module/ncvdec/parameters/stage
 *     1 trap and emulate in this module's own code       2 stop_machine() that does nothing
 *     3 rewrite one driver instruction with itself       4 patch _VDEC_EnableCRC for 5 s
 *     5 patch every site for 5 s
 * While something is patched, or a stage runs, the SoC watchdog (PDWNC) is armed: a hang resets
 * the TV after wdt= seconds. Progress markers go to a page of the RAM hole that survives that
 * reset (not a power cycle); the next insmod prints them.
 *
 *   insmod ncvdec.ko [max=131072] [reads=1] [wdt=20]
 *   echo 1 > /sys/module/ncvdec/parameters/on     # clear the log, patch
 *   ... play a clip ...
 *   echo 0 > /sys/module/ncvdec/parameters/on     # unpatch
 *   cat /proc/ncvdec > trace.txt                  # then tools/vdectrace.py on the PC
 *   rmmod ncvdec
 *
 * It only touches RAM: a reboot or power cycle removes every trace of it.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/stop_machine.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/hrtimer.h>	/* ktime_get() */
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/err.h>
#include <linux/atomic.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <asm/cacheflush.h>
#include <asm/ptrace.h>
#include <asm/traps.h>

#include "ncvdec_emul.h"

#define NCV_UNDEF	0xe7f5dcf8	/* UDF: bit 27 clear, so call_fpe passes it on */

struct ncv_site {
	const char *fn;
	u16 off;
	u8 load;
	u32 *pc;
	u32 insn;
	bool patched;
};

#define NSELF	3
static struct ncv_site sites[] = {
	{ NULL }, { NULL }, { NULL },	/* stage 1: ncv_self_insns */
#include "ncvdec_sites.h"
};

struct ncv_ent {
	u32 us;
	u32 addr;
	u32 val;
	u16 site;
	u8 cpu;
	u8 done;	/* written last */
};

static uint max = 131072;
module_param(max, uint, 0444);
MODULE_PARM_DESC(max, "log entries (16 bytes each, 1024..1048576)");

static bool reads = 1;
module_param(reads, bool, 0444);
MODULE_PARM_DESC(reads, "also log register and bitstream reads");

static uint wdt = 20;
module_param(wdt, uint, 0444);
MODULE_PARM_DESC(wdt, "watchdog seconds while patched (5..120)");

/* PDWNC watchdog in the stock static map: +0 bit 0 enable, +4 counts up at 27 MHz, overflow resets. */
#define WDT_CTRL	((void __iomem *)0xf0028100)
#define WDT_COUNT	((void __iomem *)0xf0028104)
static bool wdt_on;

/* Markers: RAM hole page next to the ramoops area, unused by stock and U-Boot. */
#define MARK_PHYS	0x3e200000
#define MARK_MAGIC	0x4e435644	/* "NCVD" */
#define MARK_MAX	1000
static u32 __iomem *marks;	/* [0] magic, [1] count, [2...] codes */

static void mark(u32 code)
{
	u32 n = readl(&marks[1]);

	if (n >= MARK_MAX)
		return;
	writel(code, &marks[2 + n]);
	writel(n + 1, &marks[1]);
	dsb();
}

static void mark_reset(void)
{
	writel(0, &marks[1]);
	writel(MARK_MAGIC, &marks[0]);
	dsb();
}

static void wdt_feed(void)
{
	writel(0u - wdt * 27000000u, WDT_COUNT);
	dsb();
}

static int wdt_arm(void)
{
	if (wdt_on)
		return 0;
	if (readl(WDT_CTRL) & 1) {
		printk(KERN_ERR "ncvdec: the watchdog is already in use\n");
		return -EBUSY;
	}
	wdt_feed();
	writel(1, WDT_CTRL);	/* as in the mainline mt5398_wdt driver */
	dsb();
	wdt_on = true;
	return 0;
}

static void wdt_disarm(void)
{
	if (!wdt_on)
		return;
	writel(0, WDT_CTRL);
	dsb();
	wdt_on = false;
}

static struct ncv_ent *ents;
static atomic_t head = ATOMIC_INIT(0);
static atomic_t other = ATOMIC_INIT(0);	/* emulated accesses outside the decoder blocks */
static ktime_t t0;
static bool patched;
static struct module *target;
static DEFINE_MUTEX(lock);

static void (*p_register_undef_hook)(struct undef_hook *);
static void (*p_unregister_undef_hook)(struct undef_hook *);

static bool vdec_addr(u32 a)
{
	switch (a >> 12) {
	case 0xf0002: case 0xf0003:	/* VLD, VLD_TOP, clock select */
	case 0xf000a:			/* MC */
	case 0xf000d:			/* CKGEN */
	case 0xf002f:			/* PP */
	case 0xf0030: case 0xf0031:	/* AVC VLD, AVC MV */
		return true;
	}
	return false;
}

/*
 * Runs from the undef exception with IRQs off on this CPU: between the trap and the return
 * stop_machine() can't run, so a trapped site is always still in the table.
 */
static int ncv_undef(struct pt_regs *regs, unsigned int instr)
{
	u32 *pc = (u32 *)instruction_pointer(regs);
	struct ncv_ent *e;
	static bool first_seen;
	u32 addr, val;
	int i, n;

	for (i = 0; i < ARRAY_SIZE(sites); i++)
		if (sites[i].pc == pc && sites[i].patched)
			break;
	if (i == ARRAY_SIZE(sites))
		return 1;	/* not ours */
	if (i >= NSELF && !first_seen) {
		first_seen = true;
		mark(0x54000000 | i);	/* first trap in the driver */
	}

	if (ncv_cond(sites[i].insn, regs->ARM_cpsr)) {
		addr = ncv_addr(sites[i].insn, regs->uregs);
		if (!vdec_addr(addr)) {
			ncv_exec(sites[i].insn, regs->uregs, addr);
			atomic_inc(&other);
		} else {
			/* the slot is taken first, so one CPU's entries are in access order */
			n = atomic_read(&head) < max ? atomic_inc_return(&head) - 1 : max;
			val = ncv_exec(sites[i].insn, regs->uregs, addr);
			if (n < max) {
				e = &ents[n];
				e->us = (u32)ktime_to_us(ktime_sub(ktime_get(), t0));
				e->addr = addr;
				e->val = val;
				e->site = i;
				e->cpu = smp_processor_id();
				smp_wmb();
				e->done = 1;
			}
		}
	}
	regs->ARM_pc += 4;
	return 0;
}

static struct undef_hook ncv_hook = {
	.instr_mask	= 0xffffffff,
	.instr_val	= NCV_UNDEF,
	.cpsr_mask	= MODE_MASK | PSR_T_BIT,
	.cpsr_val	= SVC_MODE,
	.fn		= ncv_undef,
};

static void put_insn(u32 *pc, u32 insn)
{
	*(volatile u32 *)pc = insn;
	flush_icache_range((unsigned long)pc, (unsigned long)(pc + 1));
}

struct ncv_job {
	int lo, hi;
	int op;		/* 0 restore, 1 patch, 2 rewrite the same word */
};

/* stop_machine(): every other CPU spins with IRQs off. */
static int ncv_patch(void *arg)
{
	struct ncv_job *j = arg;
	int i;

	mark(0x50000000 | j->op << 16 | j->lo);
	for (i = j->lo; i < j->hi; i++) {
		struct ncv_site *s = &sites[i];

		if (j->op == 2) {
			put_insn(s->pc, s->insn);
		} else if (j->op == 1 && !s->patched && (reads || !s->load)) {
			s->patched = true;
			put_insn(s->pc, NCV_UNDEF);
		} else if (j->op == 0 && s->patched) {
			put_insn(s->pc, s->insn);
			s->patched = false;
		}
	}
	mark(0x51000000 | j->op << 16 | j->hi);
	return 0;
}

static int ncv_nop(void *arg)
{
	mark(0x52000000);
	return 0;
}

static struct ncv_job all_on = { NSELF, ARRAY_SIZE(sites), 1 };
static struct ncv_job all_off = { NSELF, ARRAY_SIZE(sites), 0 };

static int ncv_set(bool on)
{
	int i, ret = 0;

	mutex_lock(&lock);
	if (on == patched)
		goto out;
	if (on) {
		for (i = NSELF; i < ARRAY_SIZE(sites); i++)
			if (*sites[i].pc != sites[i].insn) {
				printk(KERN_ERR "ncvdec: %s+0x%x changed, not patching\n",
				       sites[i].fn, sites[i].off);
				ret = -EBUSY;
				goto out;
			}
		memset(ents, 0, max * sizeof(*ents));
		atomic_set(&head, 0);
		atomic_set(&other, 0);
		t0 = ktime_get();
		ret = wdt_arm();
		if (ret)
			goto out;
		mark(0x10);
		p_register_undef_hook(&ncv_hook);
		ret = stop_machine(ncv_patch, &all_on, NULL);
		if (ret) {	/* did not run: nothing is patched */
			p_unregister_undef_hook(&ncv_hook);
			wdt_disarm();
			goto out;
		}
		mark(0x11);
	} else {
		mark(0x12);
		ret = stop_machine(ncv_patch, &all_off, NULL);
		if (ret)
			goto out;
		p_unregister_undef_hook(&ncv_hook);
		mark(0x13);
		wdt_disarm();
	}
	patched = on;
	printk(KERN_INFO "ncvdec: %s, %d entries\n", on ? "recording" : "stopped",
	       min_t(int, atomic_read(&head), max));
out:
	mutex_unlock(&lock);
	return ret;
}

/*
 * Feeds the watchdog while patched, and unpatches when the buffer is full (the trap handler
 * can't). A thread, not a work item: WORK_STRUCT_NO_CPU is built from CONFIG_NR_CPUS, which is 4
 * on the TV and 2 in our build config, and the kernel BUG()s on the resulting CPU number.
 */
static struct task_struct *watcher;

static int ncv_watch(void *arg)
{
	while (!kthread_should_stop()) {
		if (patched) {
			wdt_feed();
			if (atomic_read(&head) >= max)
				ncv_set(false);
		}
		msleep(100);
	}
	return 0;
}

static int on_set(const char *val, const struct kernel_param *kp)
{
	bool on;

	if (val && *val == '1')
		on = true;
	else if (val && *val == '0')
		on = false;
	else
		return -EINVAL;
	if (!ents)	/* on= on the insmod line */
		return -EINVAL;
	if (on && !patched)
		mark_reset();
	return ncv_set(on);
}

static int on_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%d", patched);
}

static struct kernel_param_ops on_ops = { .set = on_set, .get = on_get };
module_param_cb(on, &on_ops, NULL, 0644);
MODULE_PARM_DESC(on, "1: clear the log and patch, 0: unpatch");

/* Stage 1 target: a store, a register-offset load and a store that must not happen. */
extern u32 ncv_self_insns[];
u32 ncv_self_run(u32 *p, u32 off, u32 val);
asm(	".text\n\t.align 2\n"
"	.type ncv_self_run, %function\n"
"ncv_self_run:\n"
"	mov	r3, r0\n"
"	cmp	r0, r0\n"		/* Z set */
"	.globl	ncv_self_insns\n"
"ncv_self_insns:\n"
"	str	r2, [r3, #4]\n"
"	ldr	r0, [r3, r1]\n"
"	strne	r2, [r3, #8]\n"
"	bx	lr\n"
"	.size ncv_self_run, . - ncv_self_run\n");

static int self_check(bool irqs_off)
{
	static u32 buf[4];
	unsigned long flags = 0;
	u32 r;

	buf[0] = 0x11111111; buf[1] = 0; buf[2] = 0x33333333; buf[3] = 0x44444444;
	if (irqs_off)
		local_irq_save(flags);
	r = ncv_self_run(buf, 12, 0xcafe0001);
	if (irqs_off)
		local_irq_restore(flags);
	return r == 0x44444444 && buf[0] == 0x11111111 && buf[1] == 0xcafe0001 &&
	       buf[2] == 0x33333333 ? 0 : -EIO;
}

static int stage1(void)
{
	int i, ret, n0 = atomic_read(&other);

	ret = self_check(false);	/* the plain code first */
	if (ret)
		return ret;
	for (i = 0; i < NSELF; i++) {
		sites[i].pc = &ncv_self_insns[i];
		sites[i].insn = ncv_self_insns[i];
		sites[i].load = NCV_LOAD(sites[i].insn);
		if (!ncv_ok(sites[i].insn))
			return -EINVAL;
	}
	p_register_undef_hook(&ncv_hook);
	for (i = 0; i < NSELF; i++) {
		sites[i].patched = true;
		put_insn(sites[i].pc, NCV_UNDEF);
	}
	mark(0x20);
	ret = self_check(false);
	mark(0x21);
	if (!ret)
		ret = self_check(true);
	mark(0x22);
	for (i = 0; i < NSELF; i++) {
		put_insn(sites[i].pc, sites[i].insn);
		sites[i].patched = false;
	}
	p_unregister_undef_hook(&ncv_hook);
	/* 2 emulated accesses per run; the strne only steps */
	if (!ret && atomic_read(&other) - n0 != 4)
		ret = -EIO;
	return ret;
}

/* Patch sites [lo, hi) for 5 s. */
static int stage_patch(int lo, int hi)
{
	struct ncv_job j = { lo, hi, 1 };
	int i, ret;

	p_register_undef_hook(&ncv_hook);
	ret = stop_machine(ncv_patch, &j, NULL);
	mark(0x30);
	if (!ret) {
		for (i = 0; i < 50; i++) {	/* a hang here resets after wdt= s */
			msleep(100);
			wdt_feed();
		}
		mark(0x31);
		j.op = 0;
		ret = stop_machine(ncv_patch, &j, NULL);
		mark(0x32);
		if (ret) {	/* still patched: keep the hook; on=0 or rmmod restores */
			patched = true;
			return ret;
		}
	}
	p_unregister_undef_hook(&ncv_hook);
	return ret;
}

static int stage_set(const char *val, const struct kernel_param *kp)
{
	struct ncv_job same;
	int n = val ? *val - '0' : 0, crc, ret;

	if (!ents || n < 1 || n > 5)
		return -EINVAL;
	mutex_lock(&lock);
	if (patched) {
		mutex_unlock(&lock);
		return -EBUSY;
	}
	for (crc = NSELF; crc < ARRAY_SIZE(sites); crc++)
		if (!strcmp(sites[crc].fn, "_VDEC_EnableCRC"))
			break;
	if (crc + 2 > ARRAY_SIZE(sites)) {
		mutex_unlock(&lock);
		return -ENOENT;
	}
	mark_reset();
	mark(n);
	ret = wdt_arm();
	if (ret) {
		mutex_unlock(&lock);
		return ret;
	}
	memset(ents, 0, max * sizeof(*ents));
	atomic_set(&head, 0);
	atomic_set(&other, 0);
	t0 = ktime_get();
	printk(KERN_INFO "ncvdec: stage %d\n", n);
	switch (n) {
	case 1:
		ret = stage1();
		break;
	case 2:
		ret = stop_machine(ncv_nop, NULL, NULL);
		break;
	case 3:
		same.lo = crc;
		same.hi = crc + 1;
		same.op = 2;
		ret = stop_machine(ncv_patch, &same, NULL);
		break;
	case 4:
		ret = stage_patch(crc, crc + 2);
		break;
	case 5:
		ret = stage_patch(NSELF, ARRAY_SIZE(sites));
		break;
	}
	mark(0xd0000000 | n);
	wdt_disarm();
	printk(KERN_INFO "ncvdec: stage %d: %d, %d logged, %d other\n", n, ret,
	       min_t(int, atomic_read(&head), max), atomic_read(&other));
	mutex_unlock(&lock);
	return ret;
}

static struct kernel_param_ops stage_ops = { .set = stage_set };
module_param_cb(stage, &stage_ops, NULL, 0200);
MODULE_PARM_DESC(stage, "run bring-up stage 1..5");

static int ncv_count(void)
{
	return min_t(int, atomic_read(&head), max);
}

static void *seq_at(loff_t pos)
{
	return pos == 0 ? SEQ_START_TOKEN : pos <= ncv_count() ? &ents[pos - 1] : NULL;
}

static void *ncv_start(struct seq_file *m, loff_t *pos)
{
	return seq_at(*pos);
}

static void *ncv_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_at(++*pos);
}

static void ncv_stop(struct seq_file *m, void *v)
{
}

static int ncv_show(struct seq_file *m, void *v)
{
	struct ncv_ent *e = v;
	struct ncv_site *s;

	if (v == SEQ_START_TOKEN) {
		seq_printf(m, "# ncvdec 1 entries=%d max=%u reads=%d other=%d recording=%d\n"
			   "# us cpu R|W addr value function+offset\n",
			   ncv_count(), max, reads, atomic_read(&other), patched);
		return 0;
	}
	if (!e->done) {
		seq_printf(m, "# entry not complete\n");
		return 0;
	}
	smp_rmb();
	s = &sites[e->site];
	seq_printf(m, "%u %u %c %08x %08x %s+0x%x\n", e->us, e->cpu, s->load ? 'R' : 'W',
		   e->addr, e->val, s->fn, s->off);
	return 0;
}

static const struct seq_operations ncv_seq = {
	.start = ncv_start, .next = ncv_next, .stop = ncv_stop, .show = ncv_show,
};

static int ncv_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &ncv_seq);
}

static const struct file_operations ncv_fops = {
	.owner = THIS_MODULE, .open = ncv_open, .read = seq_read, .llseek = seq_lseek,
	.release = seq_release,
};

/* Resolve and check every site; nothing is written here. */
static int __init ncv_resolve(void)
{
	struct module *mod;
	unsigned long a;
	int i;

	for (i = NSELF; i < ARRAY_SIZE(sites); i++) {
		struct ncv_site *s = &sites[i];

		a = kallsyms_lookup_name(s->fn);
		if (!a || (a & 3)) {
			printk(KERN_ERR "ncvdec: %s not found\n", s->fn);
			return -ENOENT;
		}
		preempt_disable();
		mod = __module_text_address(a + s->off);
		preempt_enable();
		if (!mod || (target && mod != target)) {
			printk(KERN_ERR "ncvdec: %s is not in the driver module\n", s->fn);
			return -ENOENT;
		}
		target = mod;
		s->pc = (u32 *)(a + s->off);
		s->insn = *s->pc;
		if (!ncv_ok(s->insn) || NCV_LOAD(s->insn) != s->load) {
			printk(KERN_ERR "ncvdec: %s+0x%x is %08x, not the expected %s (other driver version?)\n",
			       s->fn, s->off, s->insn, s->load ? "load" : "store");
			return -EINVAL;
		}
	}
	return 0;
}

static int __init ncvdec_init(void)
{
	int ret, i, n;

	if (wdt < 5 || wdt > 120)
		return -EINVAL;
	if (max < 1024 || max > 1048576)
		return -EINVAL;
	p_register_undef_hook = (void *)kallsyms_lookup_name("register_undef_hook");
	p_unregister_undef_hook = (void *)kallsyms_lookup_name("unregister_undef_hook");
	if (!p_register_undef_hook || !p_unregister_undef_hook)
		return -ENOENT;
	ret = ncv_resolve();
	if (ret)
		return ret;
	if (!try_module_get(target))	/* it must not go away under us */
		return -ENODEV;
	ents = vzalloc(max * sizeof(*ents));
	if (!ents) {
		module_put(target);
		return -ENOMEM;
	}
	marks = ioremap(MARK_PHYS, PAGE_SIZE);
	if (!marks) {
		vfree(ents);
		module_put(target);
		return -ENOMEM;
	}
	printk(KERN_INFO "ncvdec: last reset %s the watchdog\n",
	       readl(WDT_CTRL) & (1 << 24) ? "came from" : "was not from");
	if (readl(&marks[0]) == MARK_MAGIC) {
		n = min_t(u32, readl(&marks[1]), MARK_MAX);
		printk(KERN_INFO "ncvdec: %d markers from an earlier run:\n", n);
		for (i = 0; i < n && i < 64; i++)
			printk(KERN_INFO "ncvdec:   %08x\n", readl(&marks[2 + i]));
	}
	watcher = kthread_run(ncv_watch, NULL, "ncvdec");
	if (IS_ERR(watcher)) {
		iounmap(marks);
		vfree(ents);
		module_put(target);
		return PTR_ERR(watcher);
	}
	if (!proc_create("ncvdec", 0400, NULL, &ncv_fops)) {
		kthread_stop(watcher);
		iounmap(marks);
		vfree(ents);
		module_put(target);
		return -ENOMEM;
	}
	printk(KERN_INFO "ncvdec: %zu sites in %s checked, not patched\n",
	       ARRAY_SIZE(sites) - NSELF, target->name);
	return 0;
}

static void __exit ncvdec_exit(void)
{
	int i;

	for (i = 0; ncv_set(false) && i < 10; i++)
		;
	kthread_stop(watcher);
	remove_proc_entry("ncvdec", NULL);
	iounmap(marks);
	vfree(ents);
	module_put(target);
}

module_init(ncvdec_init);
module_exit(ncvdec_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("H.264 decoder register trace of the MediaTek TV driver");
