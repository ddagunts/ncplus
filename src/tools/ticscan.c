/* ticscan: read-only search for 32-bit words in physical RAM that count up at a given rate.
 *   ticscan <phys> <len> [rate_hz=1000] [gap_ms=100]
 * Per 1 MB chunk: three snapshots gap_ms apart (uncached /dev/mem mapping, so it sees what is
 * really in DRAM); a hit is a word whose two deltas are both within 30% of rate * dt.
 * Prints: phys, the three values, measured rate.  "~" rows: same small positive delta pattern
 * at any other rate from 50/s to 20000/s in both gaps (within 10% of each other), off by default
 * (argv[5] = any). Never writes. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHUNK (1 << 20)
#define NW (CHUNK / 4)
static uint32_t a[NW], b[NW], c[NW];

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static double snap(uint32_t *dst, const volatile uint32_t *src, unsigned long n)
{
	double t0 = now();
	for (unsigned long i = 0; i < n; i++) dst[i] = src[i];
	return t0;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: ticscan <phys> <len> [hz] [gap_ms] [any]\n"); return 2; }
	unsigned long phys = strtoul(argv[1], 0, 0), len = strtoul(argv[2], 0, 0);
	double hz = argc > 3 ? atof(argv[3]) : 1000;
	int gap = argc > 4 ? atoi(argv[4]) : 100, any = argc > 5;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	setvbuf(stdout, 0, _IOLBF, 0);
	nice(10);
	unsigned long hits = 0;
	for (unsigned long off = 0; off < len; off += CHUNK) {
		unsigned long n = len - off < CHUNK ? len - off : CHUNK;
		void *m = mmap(0, n, PROT_READ, MAP_SHARED, fd, phys + off);
		if (m == MAP_FAILED) { perror("mmap"); return 1; }
		double t0 = snap(a, m, n / 4);
		usleep(gap * 1000);
		double t1 = snap(b, m, n / 4);
		usleep(gap * 1000);
		double t2 = snap(c, m, n / 4);
		munmap(m, n);
		for (unsigned long i = 0; i < n / 4; i++) {
			if (a[i] == b[i]) continue;
			uint32_t d1 = b[i] - a[i], d2 = c[i] - b[i];
			double r1 = d1 / (t1 - t0), r2 = d2 / (t2 - t1);
			int hit = r1 > hz * .7 && r1 < hz * 1.3 && r2 > hz * .7 && r2 < hz * 1.3;
			int other = any && r1 > 50 && r1 < 20000 && r2 > r1 * .9 && r2 < r1 * 1.1;
			if (!hit && !other) continue;
			hits++;
			if (hits < 500)
				printf("%s%08lx %08x %08x %08x  %.0f/s %.0f/s\n", hit ? "" : "~", phys + off + i * 4, a[i], b[i], c[i], r1, r2);
		}
	}
	printf("%08lx+%lx: %lu hits\n", phys, len, hits);
	return 0;
}
