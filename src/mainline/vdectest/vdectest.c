/*
 * vdectest: MT5398 H.264 decoder bring-up under mainline. Replays one picture's register sequence
 * (gen-replay.py) with the buffers of vdecbuf.ko, and dumps the result.
 *
 *   vdectest probe                              register reads only
 *   vdectest reset                              the decoder's hardware reset writes
 *   vdectest peek addr words                    register reads
 *   vdectest poke addr value                    one register write, read back
 *   vdectest stat                               what the last decode left in the buffer
 *   vdectest decode replay.txt es.h264 es_bytes out.bin  decode the pictures in replay.txt
 *   (out.bin: every frame buffer, 1280x736 tiled, one after the other)
 *   (stops at the first bitstream read that differs from stock)
 *
 * Needs root, vdecbuf.ko, and a watchdog (a register access to an unclocked block hangs the CPU).
 * Every step is printed before it happens.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define VDECBUF_PHYS _IOWR('v', 2, uint32_t)
#define NBUF	4		/* vdecbuf count=4: buffer 0 FIFO/work/MV, 1..3 two frames each */
#define BUFSZ	(4u << 20)
/* buffer layout: everything between the areas is a guard */
#define O_FIFO	0x010000
#define S_FIFO	0x040000
#define O_WORK	0x060000
#define S_WORK	0x008000
#define O_MV	0x080000	/* NMV buffers, 0x40000 apart, in buffer 0 */
#define S_MV	0x039800
#define NMV	8
#define NFB	6		/* frame k: buffer 1 + k/2, offset 0x40000 or 0x1c0000 */
#define FB_OFF(k) (BUFSZ * (1 + (k) / 2) + 0x40000 + ((k) % 2) * 0x180000)
#define S_FRAME	0x159000	/* 1280 x 736 x 3/2: luma in 64x32 tiles, then CbCr in 64x16 tiles */
#define CHROMA_OFF 0xe6000
#define ES_PAD	4		/* stock's data starts 4 bytes into the FIFO */

static int memfd;
static uint8_t *buf;		/* NBUF buffers mapped back to back */
static uint32_t bphys[NBUF];
#define phys bphys[0]
/* physical address of an offset into the mapping */
static uint32_t pa_of(uint32_t off) { return bphys[off / BUFSZ] + off % BUFSZ; }
static volatile uint32_t *pg[0x40];	/* by (addr >> 12) & 0x3f */

static volatile uint32_t *reg(uint32_t a)
{
	uint32_t i = (a >> 12) & 0x3f;

	if ((a >> 18) != (0xf0000000u >> 18)) { fprintf(stderr, "bad register %08x\n", a); exit(2); }
	if (!pg[i]) {
		void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, a & ~0xfffu);
		if (p == MAP_FAILED) { perror("mmap /dev/mem"); exit(2); }
		pg[i] = p;
	}
	return pg[i] + (a & 0xfff) / 4;
}
static uint32_t rd(uint32_t a) { return *reg(a); }
static void wr(uint32_t a, uint32_t v) { *reg(a) = v; __sync_synchronize(); }

static void step(const char *s) { printf("%s\n", s); fflush(stdout); fsync(1); }
static void show(const char *n, uint32_t a) { printf("  %-10s %08x = ", n, a); fflush(stdout); printf("%08x\n", rd(a)); fflush(stdout); }

static void probe(void)
{
	step("read CKGEN (always clocked)");
	show("CKGEN+3f4", 0xf000d3f4);
	step("read VSYS");
	show("VSYS+084", 0xf0003084);
	step("read VLD");
	show("VLD+0fc", 0xf00020fc);
	show("VLD+0f8", 0xf00020f8);
	show("VLD+0f4", 0xf00020f4);
	step("read MC");
	show("MC+208", 0xf000a208);
	step("read AVLD");
	show("AVLD+270", 0xf0030270);
	show("AVLD+274", 0xf0030274);
	step("read AMV");
	show("AMV+200", 0xf0031200);
	step("read PP");
	show("PP+00c", 0xf002f00c);
}

static void hw_reset(void)
{
	int i;

	step("reset: VLD+108 = 101, clock selects");
	wr(0xf0002108, 0x101);
	wr(0xf000d3f4, 4);
	wr(0xf00030c8, 0xf7);
	wr(0xf00030cc, 0x13a20100);
	wr(0xf0003084, 2);
	wr(0xf0002108, 0);
	step("reset: MC registers, TOP");
	for (i = 0; i < 96; i++) wr(0xf000a3dc + 4 * i, 0);
	wr(0xf0002850, 0x01148000);
	wr(0xf0002858, 0xffffffff);
	wr(0xf000a638, 0);
}

static uint32_t tagval(const char *t, uint32_t au_end)
{
	int k;

	if (!strcmp(t, "FIFO")) return phys + O_FIFO;
	if (!strcmp(t, "FIFOEND")) return phys + O_FIFO + S_FIFO;
	if (!strcmp(t, "WORK")) return phys + O_WORK;
	if (!strcmp(t, "WPTR")) return ((phys + O_FIFO + ES_PAD + au_end + 16) & ~15u) + 0x200;
	if (!strncmp(t, "ES:", 3)) return phys + O_FIFO + ES_PAD + strtoul(t + 3, 0, 16);
	if (sscanf(t, "FBC%d", &k) == 1 && k >= 0 && k < NFB) return pa_of(FB_OFF(k) + CHROMA_OFF);
	if (sscanf(t, "FB%d", &k) == 1 && k >= 0 && k < NFB) return pa_of(FB_OFF(k));
	if (sscanf(t, "MV%d", &k) == 1 && k >= 0 && k < NMV) return phys + O_MV + k * 0x40000;
	fprintf(stderr, "unknown or out-of-range tag %s\n", t); exit(2);
}

/* the bitstream reader waits until the VLD has prefetched more than 8 bytes */
static int fetched(void)
{
	uint32_t f = rd(0xf00020fc), w = rd(0xf00020f8);

	if (w < f) w += S_FIFO;
	return w - f > 8;
}
static int wait_fetched(void)
{
	int i;

	for (i = 0; i < 100000; i++) if (fetched()) return 0;
	return -1;
}

/* the areas the decoder may write; everything else in the buffer is a guard */
static int in_area(uint32_t i)
{
	int k;

	if ((i >= O_FIFO && i < O_FIFO + S_FIFO) || (i >= O_WORK && i < O_WORK + S_WORK)) return 1;
	for (k = 0; k < NMV; k++) if (i >= O_MV + k * 0x40000u && i < O_MV + k * 0x40000u + S_MV) return 1;
	for (k = 0; k < NFB; k++) if (i >= FB_OFF(k) && i < FB_OFF(k) + S_FRAME) return 1;
	return 0;
}

/* after a decode start: LG's _VDEC_WaitDecDone polls AVLD+278 bit 31; then clear as its ISR */
static int finish(int pic)
{
	struct timespec t0, t;
	double ms;
	uint32_t v;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
		v = rd(0xf0030278) & 0x80000000u;
		clock_gettime(CLOCK_MONOTONIC, &t);
		ms = (t.tv_sec - t0.tv_sec) * 1e3 + (t.tv_nsec - t0.tv_nsec) / 1e6;
	} while (!v && ms < 1000);
	printf("picture %d: done bit %s after %.2f ms; AVLD+270 %08x +278 %08x +284 %08x\n", pic, v ? "set" : "NOT set",
	       ms, rd(0xf0030270), rd(0xf0030278), rd(0xf0030284));
	wr(0xf0002854, rd(0xf0002854) & ~1u);
	wr(0xf00302c4, 1);
	wr(0xf00302c4, 0);
	fflush(stdout);
	return v ? 0 : 1;
}

static int decode(const char *replay, const char *es, uint32_t au_end, const char *out)
{
	FILE *f = fopen(replay, "r");
	char line[128], tag[24];
	uint32_t a, v, sh, add, n = 0;
	int fd = open(es, O_RDONLY), i, k, pic = -1;
	ssize_t got;

	if (!f || fd < 0) { perror("input"); return 2; }
	memset(buf, 0xa5, (size_t)BUFSZ * NBUF);	/* guards */
	memset(buf + O_FIFO, 0, S_FIFO);
	memset(buf + O_WORK, 0, S_WORK);
	for (k = 0; k < NMV; k++) memset(buf + O_MV + k * 0x40000, 0, S_MV);
	for (k = 0; k < NFB; k++) memset(buf + FB_OFF(k), 0x5a, S_FRAME);
	if (au_end > S_FIFO - ES_PAD - 0x1000) { fprintf(stderr, "au_end too large for the FIFO\n"); return 2; }
	got = read(fd, buf + O_FIFO + ES_PAD, au_end);
	if (got < (ssize_t)au_end) { fprintf(stderr, "es too short\n"); return 2; }
	__sync_synchronize();
	printf("buffers at %08x %08x %08x %08x, fifo %08x, %u bytes of stream\n", bphys[0], bphys[1], bphys[2], bphys[3],
	       phys + O_FIFO, au_end);
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		if (line[0] == 'P') { pic = atoi(line + 2); printf("-- picture %d\n", pic); fflush(stdout); continue; }
		if (line[0] == 'W') {
			add = 0;
			if (sscanf(line, "W %x @%23s %u %u", &a, tag, &sh, &add) >= 3) v = (tagval(tag, au_end) >> sh) + add;
			else if (sscanf(line, "W %x %x", &a, &v) != 2) { fprintf(stderr, "bad line %s", line); return 2; }
			wr(a, v); n++;
			if (a == 0xf00020b0) {
				uint32_t r = rd(0xf00020fc);
				if (r != (v & ~15u)) { printf("VLD+0fc = %08x, expected %08x\n", r, v & ~15u); return 1; }
			}
			if (a == 0xf0030238 && v == 1 && finish(pic)) return 1;
		} else if (line[0] == 'B') {
			for (i = 0; i < 4096 && !(rd(0xf00020e8) & 1); i++) ;
			if (i == 4096) { printf("barrel shifter: 0e8 timeout (%08x)\n", rd(0xf00020e8)); return 1; }
			/* as LG's code: bit 15 = still busy, then wait for bit 0 */
			for (i = 0; i < 4096; i++) { v = rd(0xf00020f4); if (!(v & 0x8000) || (v & 1)) break; }
			if (i == 4096) { printf("barrel shifter: 0f4 timeout (%08x)\n", rd(0xf00020f4)); return 1; }
		} else if (line[0] == 'S') {
			/* sync: read 8 bits at a time until the last reads equal stock's whole sync sequence
			 * (one value alone can also occur earlier, e.g. in the access unit delimiter) */
			uint32_t want[16], got[16];
			int nw = 0, j, ok;
			char nl[128];
			long pos;
			if (sscanf(line, "S %x %x", &a, &want[nw]) != 2) { fprintf(stderr, "bad line %s", line); return 2; }
			nw++;
			for (;;) {
				pos = ftell(f);
				if (!fgets(nl, sizeof nl, f)) break;
				if (nl[0] != 'S' || nw == 16) { fseek(f, pos, SEEK_SET); break; }
				sscanf(nl, "S %x %x", &a, &want[nw++]);
			}
			for (i = 0, ok = 0; i < 64 && !ok; i++) {
				if (wait_fetched()) { printf("VLD not fetched during sync\n"); return 1; }
				for (j = 0; j < nw - 1; j++) got[j] = got[j + 1];
				got[nw - 1] = rd(a);
				ok = i >= nw - 1;
				for (j = 0; ok && j < nw; j++) ok = got[j] == want[j];
			}
			if (!ok) { printf("sync: never saw stock's %d sync values\n", nw); return 1; }
			printf("sync: %d reads\n", i);
		} else if (line[0] == 'R') {
			uint32_t e;
			if (sscanf(line, "R %x %x", &a, &e) != 2) { fprintf(stderr, "bad line %s", line); return 2; }
			if (wait_fetched()) { printf("VLD not fetched: fc %08x f8 %08x\n", rd(0xf00020fc), rd(0xf00020f8)); return 1; }
			v = rd(a);
			if (v != e) { printf("read %08x: got %08x, stock saw %08x: stopping before the decode\n", a, v, e); return 1; }
		}
	}
	printf("replayed %u writes\n", n);
	{
		uint32_t bad = 0, first = 0, untouched;
		for (i = 0; i < (int)BUFSZ * NBUF; i++)
			if (!in_area(i) && buf[i] != 0xa5) { if (!bad) first = i; bad++; }
		printf("guards: %u bytes changed", bad);
		if (bad) printf(", first at +%x", first);
		printf("\n");
		for (k = 0; k < NFB; k++) {
			untouched = 0;
			for (i = 0; i < S_FRAME; i++) untouched += buf[FB_OFF(k) + i] == 0x5a;
			printf("frame %d: %u of %u bytes still 0x5a\n", k, untouched, S_FRAME);
		}
	}
	{
		int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (o < 0) { perror(out); return 2; }
		for (k = 0; k < NFB; k++)
			if (write(o, buf + FB_OFF(k), S_FRAME) != S_FRAME) { perror(out); return 2; }
		close(o);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int b, r = 0;

	if (argc < 2) { fprintf(stderr, "usage: vdectest probe|reset|decode replay es au_end out\n"); return 2; }
	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) { perror("/dev/mem"); return 2; }
	if (!strcmp(argv[1], "probe")) { probe(); return 0; }
	if (!strcmp(argv[1], "poke") && argc == 4) {	/* poke addr value, then read it back */
		uint32_t a = strtoul(argv[2], 0, 0), v = strtoul(argv[3], 0, 0);
		printf("%08x: %08x -> ", a, rd(a)); fflush(stdout);
		wr(a, v);
		printf("%08x\n", rd(a));
		return rd(a) == v ? 0 : 1;
	}
	if (!strcmp(argv[1], "peek") && argc == 4) {	/* peek addr words: reads only */
		uint32_t a = strtoul(argv[2], 0, 0), n = strtoul(argv[3], 0, 0), i;
		for (i = 0; i < n; i++) {
			if (i % 4 == 0) printf("%s%08x:", i ? "\n" : "", a + 4 * i);
			printf(" %08x", rd(a + 4 * i));
			fflush(stdout);
		}
		printf("\n");
		return 0;
	}
	if (!strcmp(argv[1], "reset")) { hw_reset(); probe(); return 0; }
	if (strcmp(argv[1], "stat") && (strcmp(argv[1], "decode") || argc != 6)) return 2;
	b = open("/dev/vdecbuf", O_RDWR);
	if (b < 0) { perror("/dev/vdecbuf"); return 2; }
	/* one address range for all buffers, each mapped at its place */
	buf = mmap(NULL, (size_t)BUFSZ * NBUF, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 2; }
	for (r = 0; r < NBUF; r++) {
		bphys[r] = r;
		if (ioctl(b, VDECBUF_PHYS, &bphys[r])) { fprintf(stderr, "vdecbuf has no buffer %d (load it with count=%d)\n", r, NBUF); return 2; }
		if (bphys[r] & 0x1ff || bphys[r] + BUFSZ > 0x26000000) { fprintf(stderr, "buffer %d at %08x not usable\n", r, bphys[r]); return 2; }
		if (mmap(buf + (size_t)r * BUFSZ, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, b, (off_t)r * BUFSZ) == MAP_FAILED) { perror("mmap vdecbuf"); return 2; }
	}
	r = 0;
	if (!strcmp(argv[1], "stat")) {		/* what the last run left in the buffer, read only */
		static const struct { const char *n; uint32_t o, s; } a[] = {
			{ "guard0", 0, O_FIFO }, { "fifo", O_FIFO, S_FIFO }, { "work", O_WORK, S_WORK },
			{ "mv0", O_MV, S_MV }, { "mv4", O_MV + 4 * 0x40000, S_MV }, { "frame0", FB_OFF(0), S_FRAME },
			{ "frame4", FB_OFF(4), S_FRAME }, { "frame5", FB_OFF(5), S_FRAME } };
		unsigned k, j, z, f5, fa;
		for (k = 0; k < sizeof a / sizeof a[0]; k++) {
			z = f5 = fa = 0;
			for (j = 0; j < a[k].s; j++) { uint8_t c = buf[a[k].o + j]; z += !c; f5 += c == 0x5a; fa += c == 0xa5; }
			printf("%-7s +%06x %7u bytes: %7u zero, %7u 0x5a, %7u 0xa5, %7u other\n", a[k].n, a[k].o, a[k].s, z, f5, fa, a[k].s - z - f5 - fa);
		}
		return 0;
	}
	r = decode(argv[2], argv[3], strtoul(argv[4], 0, 0), argv[5]);
	step("done");
	return r;
}
