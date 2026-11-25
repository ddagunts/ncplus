/* Host test of src/kmod/ncvdec_emul.h: run real ARM instructions, then the same ones through a
 * SIGILL trap + emulation as ncvdec does, and compare. Build and run:
 *   arm-linux-gcc -marm -O1 -static -o /tmp/t tests/ncvdec_emul_test.c && qemu-arm /tmp/t
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "../src/kmod/ncvdec_emul.h"

#define UNDEF 0xe7f5dcf8u
static unsigned int cur, *site, traps;

static void ill(int s, siginfo_t *si, void *u)
{
	mcontext_t *m = &((ucontext_t *)u)->uc_mcontext;
	unsigned long *r = &m->arm_r0;

	traps++;
	if ((unsigned int *)m->arm_pc != site) _exit(9);
	if (ncv_cond(cur, m->arm_cpsr)) ncv_exec(cur, r, ncv_addr(cur, r));
	m->arm_pc += 4;
}

int main(void)
{
	/* code: msr flags,r3; ldr r2,=val loaded by the caller; <insn>; mov r0,r2; bx lr
	 * called as f(base, off, rtval, cpsr_flags) */
	unsigned int *c = mmap(0, 4096, 7, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	static unsigned int mem[64], a[64], b[64];
	struct sigaction sa = { .sa_sigaction = ill, .sa_flags = SA_SIGINFO };
	unsigned int cond, kind, load, up, fl, ra, rb, n = 0, i, bad = 0;

	sigaction(SIGILL, &sa, 0);
	c[0] = 0xe128f003;	/* msr cpsr_f, r3 */
	c[2] = 0xe1a00002;	/* mov r0, r2 */
	c[3] = 0xe12fff1e;	/* bx lr */
	site = &c[1];
	for (cond = 0; cond < 15; cond++) for (kind = 0; kind < 2; kind++)
	for (load = 0; load < 2; load++) for (up = 0; up < 2; up++) for (fl = 0; fl < 16; fl++) {
		unsigned int insn = cond << 28 | (kind ? 0x07002001 : 0x05002014) | load << 20 | up << 23;
		unsigned int (*f)(unsigned int, unsigned int, unsigned int, unsigned int) = (void *)c;

		if (!ncv_ok(insn) || NCV_LOAD(insn) != load) { printf("refused %08x\n", insn); return 1; }
		for (i = 0; i < 64; i++) mem[i] = 0x1000 + i;
		c[1] = insn; __builtin___clear_cache((char *)c, (char *)(c + 4));
		ra = f((unsigned int)&mem[32], 0x18, 0xabcd0000, fl << 28); memcpy(a, mem, sizeof a);
		for (i = 0; i < 64; i++) mem[i] = 0x1000 + i;
		cur = insn; c[1] = UNDEF; __builtin___clear_cache((char *)c, (char *)(c + 4));
		rb = f((unsigned int)&mem[32], 0x18, 0xabcd0000, fl << 28); memcpy(b, mem, sizeof b);
		n++;
		if (ra != rb || memcmp(a, b, sizeof a)) { bad++; printf("MISMATCH %08x flags %x: %08x %08x\n", insn, fl, ra, rb); }
	}
	/* must be refused: byte, writeback, post-index, shifted Rm, sp/pc operands, NV space, data processing, ldm */
	static const unsigned int no[] = { 0xe5c32004, 0xe5a32004, 0xe4832004, 0xe7832101, 0xe58d2004, 0xe59f2004,
		0xe583f004, 0xe593f004, 0xe583d004, 0xe783200d, 0xf5932004, 0xe0832001, 0xe8930006, 0xe7f5dcf8, 0xe7132011 };
	for (i = 0; i < sizeof no / 4; i++) if (ncv_ok(no[i])) { bad++; printf("accepted %08x\n", no[i]); }
	printf("%u cases, %u traps, %u bad\n", n, traps, bad);
	return bad || traps != n;
}
