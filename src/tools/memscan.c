/* memscan: read-only survey of a physical memory range through /dev/mem (mmap, so it also
 * works above the kernel's lowmem where read() on /dev/mem refuses).
 *   memscan <phys> <len> [chunk]           one line per chunk: address, zero/ones/mixed, hash
 *   memscan -d <phys> [len]                hex+ASCII dump (default 256 bytes)
 *   memscan -s <phys> <len> <string>       print the offsets where <string> occurs
 *   memscan -r <phys> <len>                raw bytes to stdout (e.g. | base64)
 * Numbers are hex (0x optional) or decimal with a k/M suffix. Never writes. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long num(const char *s)
{
	char *e;
	unsigned long v = strtoul(s, &e, 0);
	if (*e == 'k' || *e == 'K') v <<= 10;
	if (*e == 'M') v <<= 20;
	return v;
}

static const uint8_t *map(int fd, unsigned long phys, unsigned long len, void **base, size_t *mlen)
{
	unsigned long pg = phys & ~0xfffUL;
	*mlen = (phys - pg) + len;
	*base = mmap(0, *mlen, PROT_READ, MAP_SHARED, fd, pg);
	if (*base == MAP_FAILED) { perror("mmap"); exit(1); }
	return (const uint8_t *)*base + (phys - pg);
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	void *base;
	size_t mlen;
	if (fd < 0) { perror("/dev/mem"); return 1; }
	setvbuf(stdout, 0, _IOLBF, 0);

	if (argc >= 3 && !strcmp(argv[1], "-d")) {
		unsigned long phys = num(argv[2]), len = argc > 3 ? num(argv[3]) : 256;
		const uint8_t *p = map(fd, phys, len, &base, &mlen);
		for (unsigned long i = 0; i < len; i += 16) {
			printf("%08lx:", phys + i);
			for (int j = 0; j < 16 && i + j < len; j++) printf(" %02x", p[i + j]);
			printf("  ");
			for (int j = 0; j < 16 && i + j < len; j++) putchar(p[i + j] >= 32 && p[i + j] < 127 ? p[i + j] : '.');
			putchar('\n');
		}
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "-r")) {
		unsigned long phys = num(argv[2]), len = num(argv[3]);
		const uint8_t *p = map(fd, phys, len, &base, &mlen);
		/* copy word-wise through a buffer: device-memory mappings dislike memcpy's tricks */
		static uint32_t buf[4096];
		for (unsigned long off = 0; off < len; off += sizeof(buf)) {
			unsigned long n = len - off < sizeof(buf) ? len - off : sizeof(buf);
			for (unsigned long i = 0; i < (n + 3) / 4; i++)
				buf[i] = ((const volatile uint32_t *)(p + off))[i];
			if (fwrite(buf, 1, n, stdout) != n) return 1;
		}
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "-s")) {
		unsigned long phys = num(argv[2]), len = num(argv[3]);
		size_t n = strlen(argv[4]);
		const uint8_t *p = map(fd, phys, len, &base, &mlen);
		for (unsigned long i = 0; i + n <= len; i++)
			if (p[i] == (uint8_t)argv[4][0] && !memcmp(p + i, argv[4], n))
				printf("%08lx\n", phys + i);
		return 0;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: memscan <phys> <len> [chunk] | -d <phys> [len] | -s <phys> <len> <str> | -r <phys> <len>\n");
		return 2;
	}
	unsigned long phys = num(argv[1]), len = num(argv[2]), chunk = argc > 3 ? num(argv[3]) : 1 << 20;
	for (unsigned long off = 0; off < len; off += chunk) {
		unsigned long n = len - off < chunk ? len - off : chunk;
		const uint32_t *w = (const uint32_t *)map(fd, phys + off, n, &base, &mlen);
		uint32_t h = 2166136261u, zero = 1, ones = 1;
		for (unsigned long i = 0; i < n / 4; i++) {
			uint32_t v = w[i];
			zero &= v == 0;
			ones &= v == 0xffffffff;
			h = (h ^ v) * 16777619u;
		}
		printf("%08lx %s %08x\n", phys + off, zero ? "zero " : ones ? "ones " : "mixed", h);
		munmap(base, mlen);
	}
	return 0;
}
