/* vplane: register and memory poke tool for the MT5398 video-plane experiments (Debian side).
 *   vplane w <list>            apply "addr value [mask]" lines (hex, '#' comments, 32-bit writes;
 *                              with a mask only those bits change). Prints "addr old" undo lines,
 *                              so `vplane w list > undo; vplane w undo` restores the old values.
 *   vplane r <addr> [n]        read n words
 *   vplane load <phys> <file>  copy a file into physical memory (mmap, so the RAM hole works)
 *   vplane wait <addr> <mask> <ms>  poll until (word & mask) != 0, print the time it took
 * Every access goes through /dev/mem mappings of whole pages. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fd;

static volatile uint32_t *reg(unsigned long a)
{
	static unsigned long pg[32];
	static volatile uint8_t *map[32];
	static int n;
	unsigned long p = a & ~0xfffUL;
	for (int i = 0; i < n; i++)
		if (pg[i] == p) return (volatile uint32_t *)(map[i] + (a & 0xfff));
	if (n == 32) { fprintf(stderr, "too many pages\n"); exit(1); }
	void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p);
	if (m == MAP_FAILED) { perror("mmap"); exit(1); }
	pg[n] = p; map[n] = m;
	return (volatile uint32_t *)((volatile uint8_t *)map[n++] + (a & 0xfff));
}

static long now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	setvbuf(stdout, 0, _IOLBF, 0);

	if (argc == 3 && !strcmp(argv[1], "w")) {
		FILE *f = strcmp(argv[2], "-") ? fopen(argv[2], "r") : stdin;
		char line[256];
		if (!f) { perror(argv[2]); return 1; }
		while (fgets(line, sizeof(line), f)) {
			char *h = strchr(line, '#');
			unsigned long a, v, m = 0xffffffff;
			if (h) *h = 0;
			int k = sscanf(line, "%lx %lx %lx", &a, &v, &m);
			if (k < 2) continue;
			if (a & 3) { fprintf(stderr, "unaligned %08lx\n", a); return 1; }
			volatile uint32_t *r = reg(a);
			uint32_t old = *r;
			*r = (old & ~m) | (v & m);
			printf("%08lx %08x\n", a, old);
		}
		return 0;
	}
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "r")) {
		unsigned long a = strtoul(argv[2], 0, 16), n = argc == 4 ? strtoul(argv[3], 0, 0) : 1;
		for (unsigned long i = 0; i < n; i++) {
			if (i % 4 == 0) printf("%s%08lx:", i ? "\n" : "", a + 4 * i);
			printf(" %08x", *reg(a + 4 * i));
		}
		printf("\n");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "load")) {
		unsigned long p = strtoul(argv[2], 0, 16);
		int in = open(argv[3], O_RDONLY);
		struct stat st;
		if (in < 0 || fstat(in, &st)) { perror(argv[3]); return 1; }
		if (p & 0xfff) { fprintf(stderr, "phys must be page aligned\n"); return 1; }
		size_t len = (st.st_size + 0xfff) & ~0xfffUL;
		uint8_t *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p);
		if (m == MAP_FAILED) { perror("mmap"); return 1; }
		for (size_t o = 0; o < (size_t)st.st_size;) {
			ssize_t k = read(in, m + o, st.st_size - o);
			if (k <= 0) { perror("read"); return 1; }
			o += k;
		}
		msync(m, len, MS_SYNC);
		printf("loaded %ld bytes at %08lx\n", (long)st.st_size, p);
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "wait")) {
		unsigned long a = strtoul(argv[2], 0, 16), m = strtoul(argv[3], 0, 16);
		long ms = strtol(argv[4], 0, 0), t0 = now_us();
		while (!(*reg(a) & m))
			if (now_us() - t0 > ms * 1000) { printf("timeout\n"); return 1; }
		printf("%ld us\n", now_us() - t0);
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "play")) {
		/* play <base> <frame bytes> <frames> <loops>: point B2R HD at frame i (luma at
		 * base + i * size, chroma 1280x736 after it) on each B2R vsync (+0x30c bit 4, W1C) */
		unsigned long base = strtoul(argv[2], 0, 16), size = strtoul(argv[3], 0, 16);
		int n = atoi(argv[4]), loops = atoi(argv[5]), late = 0;
		volatile uint32_t *y = reg(0xf0026000), *c = reg(0xf0026004), *st = reg(0xf002630c);
		long t0 = now_us();
		for (int k = 0; k < n * loops; k++) {
			unsigned long a = base + (k % n) * size;
			long tw = now_us();
			*st = 0x10;
			while (!(*st & 0x10))
				if (now_us() - tw > 100000) { printf("no vsync\n"); return 1; }
			if (now_us() - tw < 1000) late++;
			*y = a;
			*c = a + 1280 * 736;
		}
		long us = now_us() - t0;
		printf("%d flips in %ld ms (%.1f fps), %d late\n", n * loops, us / 1000, n * loops * 1e6 / us, late);
		return 0;
	}
	fprintf(stderr, "usage: vplane w <list> | r <addr> [n] | load <phys> <file> | wait <addr> <mask> <ms>\n"
		"       play <base> <frame bytes> <frames> <loops>\n");
	return 2;
}
