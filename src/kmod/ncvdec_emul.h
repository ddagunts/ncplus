/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Decode and emulate the few ARM instructions ncvdec patches. No kernel dependencies, so
 * tests/ncvdec_emul_test.c runs the same code against the real instructions under qemu-arm.
 * Handled: LDR/STR, 32-bit, pre-indexed without writeback, [Rn, #+/-imm12] or [Rn, +/-Rm]
 * (no shift), any condition. Rn/Rm/Rt = sp or pc are refused.
 */
#ifndef NCVDEC_EMUL_H
#define NCVDEC_EMUL_H

#define NCV_LOAD(i)	((i) >> 20 & 1)

static inline int ncv_ok(unsigned int i)
{
	unsigned int rn = i >> 16 & 15, rt = i >> 12 & 15, rm = i & 15;

	if (i >> 28 == 15 || rn == 13 || rn == 15 || rt == 13 || rt == 15)
		return 0;
	if ((i & 0x0f600000) == 0x05000000)		/* [Rn, #imm]: P=1 B=0 W=0 */
		return 1;
	if ((i & 0x0f600ff0) == 0x07000000)		/* [Rn, Rm]: same, LSL #0 */
		return rm != 13 && rm != 15;
	return 0;
}

static inline int ncv_cond(unsigned int i, unsigned int cpsr)
{
	unsigned int n = cpsr >> 31, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
	unsigned int cond = i >> 28, r;

	switch (cond >> 1) {
	case 0: r = z; break;
	case 1: r = c; break;
	case 2: r = n; break;
	case 3: r = v; break;
	case 4: r = c && !z; break;
	case 5: r = n == v; break;
	case 6: r = n == v && !z; break;
	default: return 1;				/* AL */
	}
	return r ^ (cond & 1);
}

static inline unsigned int ncv_addr(unsigned int i, const unsigned long *r)
{
	unsigned int off = i & 0x02000000 ? (unsigned int)r[i & 15] : i & 0xfff;
	unsigned int base = (unsigned int)r[i >> 16 & 15];

	return i & 0x00800000 ? base + off : base - off;
}

/* Returns the value moved. r = r0..r15 as saved at the trap. */
static inline unsigned int ncv_exec(unsigned int i, unsigned long *r, unsigned int addr)
{
	unsigned int rt = i >> 12 & 15;

	if (NCV_LOAD(i))
		r[rt] = *(volatile unsigned int *)(unsigned long)addr;
	else
		*(volatile unsigned int *)(unsigned long)addr = (unsigned int)r[rt];
	return (unsigned int)r[rt];
}

#endif
