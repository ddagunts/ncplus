// SPDX-License-Identifier: MIT
/*
 * sgxflip: frames drawn by the SGX544 on the panel. Takes DRM master (stop the desktop first),
 * sets the panel to three GPU-rendered 1920x1080 buffers in turn and page-flips between them,
 * each flip waited for. Scenes, 10 s each (-t seconds): colour chart, moving bar (tearing),
 * spinning triangle. Prints the flip intervals. Restores the console by closing the device.
 * Same set-up as sgxscan.c (which renders without showing).
 */
#define _FILE_OFFSET_BITS 64
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../wsegl/netcast_window.h"

#define W 1920
#define H 1080
#define NB 3

/* KMS uapi structs (drm_mode.h), written out: the toolchain's headers predate them */
struct modeinfo {
	uint32_t clock;
	uint16_t h[5], v[5];
	uint32_t vrefresh, flags, type;
	char name[32];
};
struct card_res {
	uint64_t fb, crtc, conn, enc;
	uint32_t n_fb, n_crtc, n_conn, n_enc, minw, maxw, minh, maxh;
};
struct get_conn {
	uint64_t enc, modes, props, propvals;
	uint32_t n_modes, n_props, n_enc, encoder_id, connector_id, type, type_id, connection,
		mmw, mmh, subpixel, pad;
};
struct set_crtc {
	uint64_t conns;
	uint32_t n_conns, crtc_id, fb_id, x, y, gamma, mode_valid;
	struct modeinfo mode;
};
struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct addfb { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };

#define IOWR(nr, size) (0xc0000000u | ((unsigned)(size) << 16) | 0x6400u | (nr))
#define DRM_SET_MASTER 0x641eu

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef float GLfloat;
typedef unsigned int GLenum, GLuint;

static struct {
	EGLDisplay (*GetDisplay)(void *);
	unsigned (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	unsigned (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	unsigned (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	unsigned (*SwapBuffers)(EGLDisplay, EGLSurface);
	unsigned (*DestroySurface)(EGLDisplay, EGLSurface);
	unsigned (*DestroyContext)(EGLDisplay, EGLContext);
	unsigned (*Terminate)(EGLDisplay);
	EGLint (*GetError)(void);
} egl;
static struct {
	void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (*Clear)(unsigned);
	void (*Viewport)(int, int, int, int);
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, int, const char *const *, const int *);
	void (*CompileShader)(GLuint);
	void (*GetShaderiv)(GLuint, GLenum, int *);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*BindAttribLocation)(GLuint, GLuint, const char *);
	void (*LinkProgram)(GLuint);
	void (*GetProgramiv)(GLuint, GLenum, int *);
	void (*UseProgram)(GLuint);
	void (*VertexAttribPointer)(GLuint, int, GLenum, unsigned char, int, const void *);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DrawArrays)(GLenum, int, int);
	GLenum (*GetError)(void);
} gl;
static void *lib_egl, *lib_gl;

static void *sym(void *l, const char *n)
{
	void *p = dlsym(l, n);
	if (!p) { fprintf(stderr, "sgxflip: no %s\n", n); exit(2); }
	return p;
}
#define E(f) (*(void **)&egl.f = sym(lib_egl, "egl" #f))
#define G(f) (*(void **)&gl.f = sym(lib_gl, "gl" #f))

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = gl.CreateShader(type);
	int ok = 0;
	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	gl.GetShaderiv(s, 0x8B81, &ok);
	if (!ok) { fprintf(stderr, "sgxflip: shader failed\n"); exit(3); }
	return s;
}

/*
 * -m: time the OSD's own latch. Plane 3's "fetched header word 1" (0xf0021324) is the bitmap
 * address the OSD fetched last, so the moments it changes are the display's frame starts. Only
 * reads (the driver reads the same register), through /dev/mem.
 */
static volatile int measuring;
static double latch_t[4096];
static int latch_n;

static void *latch_thread(void *arg)
{
	int dm = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile uint32_t *r = NULL;
	uint32_t last = 0;

	(void)arg;
	if (dm >= 0)
		r = mmap(NULL, 4096, PROT_READ, MAP_SHARED, dm, 0xf0021000);
	if (!r || r == MAP_FAILED) { perror("sgxflip: /dev/mem"); return NULL; }
	last = r[0x324 / 4];
	while (measuring) {
		uint32_t v = r[0x324 / 4];

		if (v != last) {
			last = v;
			if (latch_n < 4096)
				latch_t[latch_n++] = now();
		}
	}
	return NULL;
}

static void latch_report(void)
{
	double sum = 0, mn = 9, mx = 0;
	int i, n10 = 0, n20 = 0;

	for (i = 1; i < latch_n; i++) {
		double d = latch_t[i] - latch_t[i - 1];

		sum += d;
		if (d < mn) mn = d;
		if (d > mx) mx = d;
		if (d < 0.025) n10++;
		else n20++;
	}
	printf("latch: %d changes in %.2f s: avg %.4f ms (%.3f per s), min %.2f, max %.2f; "
	       "%d under 25 ms, %d longer\n", latch_n, latch_n > 1 ? latch_t[latch_n - 1] - latch_t[0] : 0,
	       latch_n > 1 ? sum / (latch_n - 1) * 1e3 : 0, latch_n > 1 ? (latch_n - 1) / sum : 0,
	       mn * 1e3, mx * 1e3, n10, n20);
	printf("first latch intervals (ms):");
	for (i = 1; i < latch_n && i < 14; i++)
		printf(" %.2f", (latch_t[i] - latch_t[i - 1]) * 1e3);
	printf("\n");
}

/* --- KMS ------------------------------------------------------------------------------- */

static int fd = -1;
static uint32_t crtc, conn;
static struct modeinfo mode;
static struct { uint32_t handle, fb; uint64_t size; void *map; } buf[NB];
static struct netcast_window win;
static int pending, started, failed;
static volatile int quit;
static double last_flip, first_flip, sum, worst, best = 1;
static long flips, late, busy;

static void reset_stats(void)
{
	last_flip = 0;
	sum = worst = 0;
	best = 1;
	flips = late = 0;
}

static int xioctl(unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg)) {
		perror(what);
		return 0;
	}
	return 1;
}

static void read_flip(void)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	char b[256];
	ssize_t n;
	double t;

	while (pending) {
		if (poll(&p, 1, 1000) <= 0) {
			fprintf(stderr, "sgxflip: no flip event within 1 s\n");
			failed = 1;
			pending = 0;
			return;
		}
		n = read(fd, b, sizeof(b));
		for (ssize_t i = 0; i + 8 <= n; ) {
			uint32_t type, len;

			memcpy(&type, b + i, 4);
			memcpy(&len, b + i + 4, 4);
			if (len < 8)
				return;
			if (type == 2) {
				pending = 0;
				t = now();
				if (last_flip) {
					double d = t - last_flip;

					sum += d;
					flips++;
					if (d > worst) worst = d;
					if (d < best) best = d;
					if (d > 0.020) late++;
				} else {
					first_flip = t;
				}
				last_flip = t;
			}
			i += len;
		}
	}
}

static void on_swap(void *priv, unsigned int n)
{
	struct page_flip f;

	(void)priv;
	if (!started) {
		struct set_crtc s;
		uint32_t ids[1] = { conn };

		memset(&s, 0, sizeof(s));
		s.conns = (uintptr_t)ids;
		s.n_conns = 1;
		s.crtc_id = crtc;
		s.fb_id = buf[n].fb;
		s.mode_valid = 1;
		s.mode = mode;
		if (!xioctl(IOWR(0xA2, sizeof(s)), &s, "sgxflip: SETCRTC"))
			failed = 1;
		started = 1;
		return;
	}
	read_flip();		/* the last flip must be on screen before the next is queued */
	memset(&f, 0, sizeof(f));
	f.crtc_id = crtc;
	f.fb_id = buf[n].fb;
	f.flags = 1;		/* DRM_MODE_PAGE_FLIP_EVENT */
	/* EBUSY: the commit before (the mode set) isn't done yet; its event comes a few vblanks later */
	for (int tries = 0; ; tries++) {
		if (!ioctl(fd, IOWR(0xB0, sizeof(f)), &f))
			break;
		if (errno != EBUSY || tries > 250) {
			perror("sgxflip: PAGE_FLIP");
			failed = 1;
			return;
		}
		busy++;
		usleep(1000);
	}
	pending = 1;
}

static int kms_setup(void)
{
	struct card_res res;
	struct get_conn gc;
	uint32_t crtcs[8], conns[8];
	struct modeinfo modes[32];
	unsigned i, k;

	if (ioctl(fd, DRM_SET_MASTER, 0)) {
		perror("sgxflip: DRM master (is the desktop still running?)");
		return 0;
	}
	memset(&res, 0, sizeof(res));
	res.crtc = (uintptr_t)crtcs;
	res.conn = (uintptr_t)conns;
	if (!xioctl(IOWR(0xA0, sizeof(res)), &res, "sgxflip: GETRESOURCES"))
		return 0;
	/* first call fills the counts; nothing points at arrays too small for one CRTC/connector */
	if (res.n_crtc < 1 || res.n_crtc > 8 || res.n_conn < 1 || res.n_conn > 8)
		return 0;
	memset(&res, 0, sizeof(res));
	res.crtc = (uintptr_t)crtcs; res.n_crtc = 8;
	res.conn = (uintptr_t)conns; res.n_conn = 8;
	if (!xioctl(IOWR(0xA0, sizeof(res)), &res, "sgxflip: GETRESOURCES"))
		return 0;
	crtc = crtcs[0];
	for (i = 0; i < res.n_conn && i < 8; i++) {
		memset(&gc, 0, sizeof(gc));
		gc.connector_id = conns[i];
		gc.modes = (uintptr_t)modes;
		gc.n_modes = 32;
		if (!xioctl(IOWR(0xA7, sizeof(gc)), &gc, "sgxflip: GETCONNECTOR"))
			return 0;
		if (gc.connection == 1 && gc.n_modes) {
			conn = conns[i];
			mode = modes[0];
			break;
		}
	}
	if (!conn || mode.h[0] != W || mode.v[0] != H) {
		fprintf(stderr, "sgxflip: no 1920x1080 connector\n");
		return 0;
	}
	printf("crtc %u connector %u mode %s %ux%u@%u\n", crtc, conn, mode.name, mode.h[0], mode.v[0], mode.vrefresh);
	for (k = 0; k < NB; k++) {
		struct create_dumb c = { .height = H, .width = W, .bpp = 32 };
		struct map_dumb m = { 0 };
		struct addfb a = { 0 };

		if (!xioctl(0xc02064b2u, &c, "sgxflip: CREATE_DUMB"))
			return 0;
		if (c.pitch != W * 4) { fprintf(stderr, "sgxflip: pitch %u\n", c.pitch); return 0; }
		m.handle = c.handle;
		if (!xioctl(0xc01064b3u, &m, "sgxflip: MAP_DUMB"))
			return 0;
		buf[k].handle = c.handle;
		buf[k].size = c.size;
		buf[k].map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
		if (buf[k].map == MAP_FAILED) { perror("mmap"); return 0; }
		memset(buf[k].map, 0, c.size);
		a.width = W; a.height = H; a.pitch = c.pitch; a.bpp = 32; a.depth = 24; a.handle = c.handle;
		if (!xioctl(0xc01c64aeu, &a, "sgxflip: ADDFB"))
			return 0;
		buf[k].fb = a.fb_id;
	}
	return 1;
}

static void on_signal(int s)
{
	(void)s;
	quit = 1;
}

/* --- scenes ------------------------------------------------------------------------------ */

static GLuint prog;
static float verts[512 * 5];
static int nv;

static void v2(float x, float y, float r, float g, float b)
{
	float *p = verts + nv++ * 5;

	p[0] = x; p[1] = y; p[2] = r; p[3] = g; p[4] = b;
}

/* pixel rectangle, y down from the top */
static void rect(float x0, float y0, float x1, float y1, float r, float g, float b)
{
	float l = x0 / (W / 2.0f) - 1, rr = x1 / (W / 2.0f) - 1;
	float t = 1 - y0 / (H / 2.0f), bt = 1 - y1 / (H / 2.0f);

	v2(l, t, r, g, b); v2(rr, t, r, g, b); v2(l, bt, r, g, b);
	v2(l, bt, r, g, b); v2(rr, t, r, g, b); v2(rr, bt, r, g, b);
}

static void draw(void)
{
	gl.VertexAttribPointer(0, 2, 0x1406, 0, 20, verts);
	gl.VertexAttribPointer(1, 3, 0x1406, 0, 20, verts + 2);
	gl.DrawArrays(4, 0, nv);
	nv = 0;
}

static void scene(int s, int i)
{
	static const float bars[8][3] = {
		{ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 1 },
		{ 0.5f, 0.5f, 0.5f }, { 1, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 },
	};
	int k;

	nv = 0;
	if (s == 0) {
		/* eight bars, left to right red green blue white grey yellow cyan magenta, corner
		 * squares, and a 1-pixel white frame on the panel's outermost pixels */
		gl.ClearColor(0, 0, 0, 1);
		gl.Clear(0x4000);
		for (k = 0; k < 8; k++)
			rect(W / 8 * k, 120, W / 8 * (k + 1), H - 120, bars[k][0], bars[k][1], bars[k][2]);
		rect(0, 0, W, 2, 1, 1, 1); rect(0, H - 2, W, H, 1, 1, 1);
		rect(0, 0, 2, H, 1, 1, 1); rect(W - 2, 0, W, H, 1, 1, 1);
		rect(0, 0, 60, 60, 1, 0, 0); rect(W - 60, 0, W, 60, 0, 1, 0);
		rect(0, H - 60, 60, H, 0, 0, 1); rect(W - 60, H - 60, W, H, 1, 1, 0);
		draw();
	} else if (s == 1) {
		/* a white bar 8 px per frame: a torn frame shows as a step in it */
		int x = (i * 8) % (W + 200) - 100;

		gl.ClearColor(0.12f, 0.12f, 0.12f, 1);
		gl.Clear(0x4000);
		for (k = 0; k < H; k += 90)
			rect(0, k, W, k + 2, 0.25f, 0.25f, 0.25f);
		rect(x, 0, x + 100, H, 1, 1, 1);
		draw();
	} else {
		float a = i * 0.02f, c = cosf(a), sn = sinf(a);

		gl.ClearColor(0.05f, 0.08f, 0.15f, 1);
		gl.Clear(0x4000);
		for (k = 0; k < 3; k++) {
			float t = a * (k ? 1.0f : 1.0f) + k * 2.0944f;
			float r = 0.8f;

			v2(r * cosf(t) * 0.5625f, r * sinf(t), k == 0, k == 1, k == 2);
		}
		draw();
		(void)c; (void)sn;
	}
}

int main(int argc, char **argv)
{
	EGLint cfgattr[] = { 0x3033, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 4, 0x3038 };
	EGLint ctxattr[] = { 0x3098, 2, 0x3038 };
	static const char *names[] = { "colour chart", "moving bar", "spinning triangle" };
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	EGLint maj, min, n;
	GLuint p;
	double secs = 10, t0;
	int s, i, ok = 0, measure = 0, only = -1;
	pthread_t th;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-t") && i + 1 < argc)
			secs = atof(argv[++i]);
		else if (!strcmp(argv[i], "-m"))
			measure = 1;
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			only = atoi(argv[++i]);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	lib_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	lib_gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	if (!lib_egl || !lib_gl) { fprintf(stderr, "sgxflip: dlopen: %s\n", dlerror()); return 2; }
	E(GetDisplay); E(Initialize); E(ChooseConfig); E(CreateWindowSurface); E(CreateContext);
	E(MakeCurrent); E(SwapBuffers); E(DestroySurface); E(DestroyContext); E(Terminate); E(GetError);
	G(ClearColor); G(Clear); G(Viewport); G(CreateShader); G(ShaderSource); G(CompileShader);
	G(GetShaderiv); G(CreateProgram); G(AttachShader); G(BindAttribLocation); G(LinkProgram);
	G(GetProgramiv); G(UseProgram); G(VertexAttribPointer); G(EnableVertexAttribArray);
	G(DrawArrays); G(GetError);

	dpy = egl.GetDisplay(NULL);		/* opens pvrsrvkm before card0 */
	if (!dpy || !egl.Initialize(dpy, &maj, &min)) {
		fprintf(stderr, "sgxflip: eglInitialize failed (0x%x)\n", egl.GetError());
		return 4;
	}
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("/dev/dri/card0"); return 4; }
	if (!kms_setup()) return 4;

	win.magic = NETCAST_WINDOW_MAGIC;
	win.width = W; win.height = H; win.stride = W; win.nbuffers = NB; win.size = W * H * 4;
	for (i = 0; i < NB; i++)
		win.map[i] = buf[i].map;
	win.swap = on_swap;
	if (!egl.ChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1 ||
	    !(surf = egl.CreateWindowSurface(dpy, cfg, &win, NULL))) {
		fprintf(stderr, "sgxflip: window surface failed (0x%x)\n", egl.GetError());
		return 5;
	}
	ctx = egl.CreateContext(dpy, cfg, NULL, ctxattr);
	if (!ctx || !egl.MakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "sgxflip: context failed (0x%x)\n", egl.GetError());
		return 5;
	}
	gl.Viewport(0, 0, W, H);
	p = gl.CreateProgram();
	gl.AttachShader(p, shader(0x8B31, "attribute vec2 pos; attribute vec3 col; varying vec3 v;"
		"void main() { gl_Position = vec4(pos, 0.0, 1.0); v = col; }"));
	gl.AttachShader(p, shader(0x8B30, "precision mediump float; varying vec3 v;"
		"void main() { gl_FragColor = vec4(v, 1.0); }"));
	gl.BindAttribLocation(p, 0, "pos");
	gl.BindAttribLocation(p, 1, "col");
	gl.LinkProgram(p);
	gl.GetProgramiv(p, 0x8B82, &ok);
	if (!ok) { fprintf(stderr, "sgxflip: link failed\n"); return 3; }
	prog = p;
	gl.UseProgram(prog);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);

	for (s = 0; s < 3 && !quit && !failed; s++) {
		if (only >= 0 && s != only)
			continue;
		reset_stats();
		if (measure && !measuring) {
			measuring = 1;
			pthread_create(&th, NULL, latch_thread, NULL);
		}
		t0 = now();
		i = 0;
		while (!quit && !failed && now() - t0 < secs) {
			scene(s, i++);
			if (!egl.SwapBuffers(dpy, surf)) {
				fprintf(stderr, "sgxflip: eglSwapBuffers failed (0x%x)\n", egl.GetError());
				failed = 1;
			}
		}
		read_flip();
		printf("%s: %d frames in %.1f s; %ld flip intervals: avg %.2f ms, min %.2f, max %.2f, over 20 ms: %ld\n",
		       names[s], i, now() - t0, flips, flips ? sum / flips * 1e3 : 0.0, best * 1e3, worst * 1e3, late);
		fflush(stdout);
	}
	if (measuring) {
		measuring = 0;
		pthread_join(th, NULL);
		latch_report();
	}
	printf("EBUSY retries %ld, glGetError 0x%x, %s\n", busy, gl.GetError(), failed ? "FAIL" : quit ? "stopped" : "done");
	egl.MakeCurrent(dpy, NULL, NULL, NULL);
	egl.DestroySurface(dpy, surf);		/* unwraps the buffers before they are freed */
	egl.DestroyContext(dpy, ctx);
	egl.Terminate(dpy);
	for (i = 0; i < NB; i++) {
		uint32_t h = buf[i].handle, id = buf[i].fb;

		munmap(buf[i].map, buf[i].size);
		ioctl(fd, IOWR(0xAF, 4), &id);
		ioctl(fd, 0xc00464b4u, &h);
	}
	close(fd);				/* console comes back */
	return failed;
}
