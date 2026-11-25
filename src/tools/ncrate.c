/*
 * ncrate: show or choose the panel's refresh rate on the stock system.
 *
 *   ncrate [status]      what the panel runs at, measured on the output line counter (read-only)
 *   ncrate 60 | 50       set that rate and wait until the panel really runs at it (within 1%)
 *   ncrate release RATE  undo: release the force and put RATE (what "status" said before) back
 *
 * LG's software keeps a 50 Hz set at 50 Hz on HOME and puts it at 60 for its media player. The
 * mainline kernel keeps whatever rate it was started at (docs/sgx544.md), so fire.sh sets it here.
 *
 * This is the call LG's own media player makes (UI_EMF_Play in RELEASE): API_VIDEO_SetFrameRate(1, 60)
 * -> VIDEO_SOC_SetFrameRate -> MTPMX_PANEL_ForceFrameRate -> MTAL ioctl 0x40040d29 (rate by value,
 * 0 = release) -> vDrvForceFrameRate(). The driver remembers the force (_bForceFrameRate), and every
 * place that works out a rate from the video signal honours it. The new totals are written from the
 * vsync interrupt a few frames later. During the change the driver fades all OSD planes out for 10
 * frames (OSD_PLA_Mute); nckexec must not hand over then, as it keeps LG's fade value, so this waits
 * for the fade to be back. RAM only: nothing is saved, a power cycle undoes it.
 *
 * Seen on the TV (2026-11-24): the force also starts LG's "good dclk" loop, which follows the
 * video input: 59.9..60.1 Hz at 60, but 51.43 at 50 with no signal, too far off for the mainline
 * driver (2%). And a bare release (rate 0) leaves 48 Hz (2200 x 1406). The plain call,
 * MTPMX_PANEL_SetFrameRate (0x40040d06), is meant to have neither loop nor memory: so release =
 * force 0 + plain call, and setting falls back to that when the forced rate isn't accurate.
 *
 * Exit code: 0 done, 1 the rate was not reached, 2 usage or not a TV this was written for,
 * 3 an OSD plane stayed faded out (don't boot another kernel now: its screen would stay black).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MTPMX_SET_FRAME_RATE	0x40040d06	/* arg: the rate itself */
#define MTPMX_GET_FRAME_RATE	0x40040d07	/* arg: uint32_t * */
#define MTPMX_FORCE_FRAME_RATE	0x40040d29	/* arg: the rate itself */

#define ROUTE_PAGE	0xf0027000UL
#define ROUTE		0x804		/* bit 0: the post-scaler (PSC) times the output */
#define PSC_PAGE	0xf005e000UL
#define PSC_TOTAL	0x30c		/* htotal << 16 | vtotal */
#define PSC_COUNT	0x380		/* live: low half = line */
#define OSD_PAGE	0xf0021000UL
#define OSD_PLANE(n)	((n) * 0x100)	/* +0 bit 0 on; +8 bits 15:8 fade, 0 = invisible */

static volatile uint32_t *psc, *osd, *route;
static int mtal;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static volatile uint32_t *map(int fd, unsigned long phys)
{
	void *p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, phys);

	if (p == MAP_FAILED) {
		perror("ncrate: mmap /dev/mem");
		return NULL;
	}
	return p;
}

/* Refresh rate from the wraps of the live line counter; 0 = no clean measurement. */
static double measure(void)
{
	int try;

	for (try = 0; try < 5; try++) {
		uint32_t vtotal = psc[PSC_TOTAL / 4] & 0xffff;
		double t0 = now(), prev = t0, first = 0, last = 0, t;
		uint32_t l, pl = psc[PSC_COUNT / 4] & 0xffff;
		int wraps = 0, clean = 1;

		while ((t = now()) < t0 + 0.4) {
			l = psc[PSC_COUNT / 4] & 0xffff;
			if (t - prev > 0.005)	/* scheduled out: a wrap may be missing */
				clean = 0;
			if (l + vtotal / 2 < pl) {
				if (!wraps++)
					first = t;
				last = t;
			}
			pl = l;
			prev = t;
		}
		if (clean && wraps > 2 && psc[PSC_TOTAL / 4] % 0x10000 == vtotal)
			return (wraps - 1) / (last - first);
	}
	return 0;
}

/* Within 1%; LG runs 100/120 Hz panels at twice the video rate. */
static int is_rate(double hz, int want)
{
	return (hz > want * 0.99 && hz < want * 1.01) || (hz > want * 1.98 && hz < want * 2.02);
}

/* Highest plane that is on but faded out, 0 = none. */
static int faded(void)
{
	int n;

	for (n = 3; n >= 1; n--)
		if ((osd[OSD_PLANE(n) / 4] & 1) && !(osd[(OSD_PLANE(n) + 8) / 4] >> 8 & 0xff))
			return n;
	return 0;
}

static uint32_t lg_rate(void)
{
	uint32_t r = 0;

	return ioctl(mtal, MTPMX_GET_FRAME_RATE, &r) == 0 ? r : 0;
}

static void report(const char *what, double hz)
{
	uint32_t total = psc[PSC_TOTAL / 4];

	printf("ncrate: %s: panel %u x %u at %.2f Hz, LG's driver says %u Hz\n", what,
	       total >> 16, total & 0xffff, hz, lg_rate());
}

/* The plain rate, without a force. */
static int set_plain(int want)
{
	return ioctl(mtal, MTPMX_FORCE_FRAME_RATE, 0UL) ||
	       ioctl(mtal, MTPMX_SET_FRAME_RATE, (unsigned long)want);
}

/* The force first, if allowed: LG's software recomputing the rate keeps it. Then the plain rate. */
static int set_rate(int want, int force)
{
	int before = faded(), try, f;
	double hz = 0, end;

	for (try = force ? 0 : 1; try < 4; try++) {
		if (try & 1 ? set_plain(want) : ioctl(mtal, MTPMX_FORCE_FRAME_RATE, (unsigned long)want)) {
			fprintf(stderr, "ncrate: the driver refused\n");
			return 1;
		}
		/* steady: the right rate in two measurements running (0.8 s, longer than the OSD fade) */
		end = now() + 3;
		do {
			usleep(100000);
			hz = measure();
			if (is_rate(hz, want)) {
				hz = measure();
				if (is_rate(hz, want))
					goto there;
			}
		} while (now() < end);
		fprintf(stderr, "ncrate: %s %d: the panel runs at %.2f Hz\n", try & 1 ? "plain" : "forced", want, hz);
	}
	report("NOT changed", hz);
	return 1;
there:
	for (end = now() + 3; (f = faded()) && f != before && now() < end; )
		usleep(50000);
	report(!force ? "released" : try & 1 ? "set (plain)" : "set (forced)", hz);
	if (f && f != before) {
		fprintf(stderr, "ncrate: OSD plane %d is still faded out (blend %08x)\n", f,
			(unsigned)osd[(OSD_PLANE(f) + 8) / 4]);
		return 3;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";
	int mem, want = !strcmp(cmd, "60") ? 60 : !strcmp(cmd, "50") ? 50 : 0, back = 0;
	uint32_t total, r;
	double hz;

	if (argc == 3 && !strcmp(cmd, "release")) {
		back = atoi(argv[2]);
		if (back != 48 && back != 50 && back != 60 && back != 100 && back != 120)
			back = 0;
	}
	if (!back && (argc > 2 || (!want && strcmp(cmd, "status")))) {
		fprintf(stderr, "usage: ncrate [status | 60 | 50 | release 48|50|60|100|120]\n");
		return 2;
	}
	mtal = open("/dev/mtal", O_RDWR);
	if (mtal < 0) {
		perror("ncrate: /dev/mtal (not a MediaTek NetCast TV on LG's system?)");
		return 2;
	}
	mem = open("/dev/mem", O_RDONLY | O_SYNC);
	if (mem < 0) {
		perror("ncrate: /dev/mem");
		return 2;
	}
	if (!(route = map(mem, ROUTE_PAGE)) || !(psc = map(mem, PSC_PAGE)) || !(osd = map(mem, OSD_PAGE)))
		return 2;

	/*
	 * Know this firmware before asking it for anything: the output must be where this expects
	 * it, its line counter must run, and the driver's own idea of the rate, from the
	 * neighbouring read-only ioctl, must be the measured one. (While a force is being
	 * released or a source locks the two can differ for a moment.)
	 */
	total = psc[PSC_TOTAL / 4];
	if (!(route[ROUTE / 4] & 1) || (total >> 16) < 1920 || (total >> 16) > 4400 ||
	    (total & 0xffff) < 1081 || (total & 0xffff) > 2200) {
		fprintf(stderr, "ncrate: unknown panel timing (route %08x total %08x): not touching it\n",
			(unsigned)route[ROUTE / 4], (unsigned)total);
		return 2;
	}
	hz = measure();
	r = lg_rate();
	if (!strcmp(cmd, "status")) {
		report("now", hz);
		return 0;
	}
	if (!hz || !r || r > 125 || hz < r - 2.0 || hz > r + 2.0) {
		usleep(500000);
		hz = measure();
		r = lg_rate();
	}
	if (!hz || !r || r > 125 || hz < r - 2.0 || hz > r + 2.0) {
		fprintf(stderr, "ncrate: measured %.2f Hz but LG's driver says %u: not touching it\n", hz, r);
		return 2;
	}

	return back ? set_rate(back, 0) : set_rate(want, 1);
}
