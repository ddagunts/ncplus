// SPDX-License-Identifier: MIT
/*
 * sgxscan: can the SGX544 render straight into KMS buffers? Creates two or three 1920x1080 ARGB8888 dumb
 * buffers on /dev/dri/card0 (no DRM master needed: nothing is shown and the desktop is not
 * touched), makes them an EGL window through our WSEGL (netcast_window.h), renders with LG's
 * GLES2, and checks the result through the CPU mapping of the very buffers the OSD would scan out.
 *
 *   LD_LIBRARY_PATH=<LG's libs, with our WSEGL as libpvrEWS_NullWSEGL.so> sgxscan [-o frame.ppm] [-d]
 * Exit status 0 = every check passed. pvrsrvkm is opened before card0 so that it is closed first.
 */
#define _FILE_OFFSET_BITS 64
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
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

struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
#define DRM_CREATE_DUMB 0xc02064b2u
#define DRM_MAP_DUMB 0xc01064b3u
#define DRM_DESTROY_DUMB 0xc00464b4u

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
	const unsigned char *(*GetString)(GLenum);
	void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (*Clear)(unsigned);
	void (*Viewport)(int, int, int, int);
	void (*Enable)(GLenum);
	void (*Disable)(GLenum);
	void (*BlendFunc)(GLenum, GLenum);
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
static int bad;

static void *sym(void *l, const char *n)
{
	void *p = dlsym(l, n);
	if (!p) { fprintf(stderr, "sgxscan: no %s\n", n); exit(2); }
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

static void check(int ok, const char *what)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		bad = 1;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = gl.CreateShader(type);
	int ok = 0;
	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	gl.GetShaderiv(s, 0x8B81, &ok);
	if (!ok) { fprintf(stderr, "sgxscan: shader failed\n"); exit(3); }
	return s;
}

static GLuint program(const char *v, const char *f)
{
	GLuint p = gl.CreateProgram();
	int ok = 0;
	gl.AttachShader(p, shader(0x8B31, v));
	gl.AttachShader(p, shader(0x8B30, f));
	gl.BindAttribLocation(p, 0, "pos");
	gl.BindAttribLocation(p, 1, "col");
	gl.LinkProgram(p);
	gl.GetProgramiv(p, 0x8B82, &ok);
	if (!ok) { fprintf(stderr, "sgxscan: link failed\n"); exit(3); }
	return p;
}

/* --- KMS buffers --------------------------------------------------------------------- */

static int drm_fd = -1;
static struct { uint32_t handle, pitch; uint64_t size; uint32_t *map; } buf[3];
static struct netcast_window win;
static unsigned int shown = ~0u, nswaps;

static void on_swap(void *priv, unsigned int n)
{
	(void)priv;
	shown = n;
	nswaps++;
}

static int kms_buffers(int n)
{
	for (int i = 0; i < n; i++) {
		struct create_dumb c = { .height = H, .width = W, .bpp = 32 };
		struct map_dumb m = { 0 };

		if (ioctl(drm_fd, DRM_CREATE_DUMB, &c)) {
			perror("sgxscan: CREATE_DUMB");
			return 0;
		}
		buf[i].handle = c.handle;
		buf[i].pitch = c.pitch;
		buf[i].size = c.size;
		m.handle = c.handle;
		if (ioctl(drm_fd, DRM_MAP_DUMB, &m)) {
			perror("sgxscan: MAP_DUMB");
			return 0;
		}
		buf[i].map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, m.offset);
		if (buf[i].map == MAP_FAILED) {
			perror("sgxscan: mmap");
			return 0;
		}
		printf("buffer %d: handle %u pitch %u size %llu mapped %p\n", i, c.handle, c.pitch,
		       (unsigned long long)c.size, (void *)buf[i].map);
	}
	return 1;
}

static void kms_free(int n)
{
	for (int i = 0; i < n; i++) {
		if (buf[i].map && buf[i].map != MAP_FAILED)
			munmap(buf[i].map, buf[i].size);
		if (buf[i].handle)
			ioctl(drm_fd, DRM_DESTROY_DUMB, &buf[i].handle);
	}
}

/* a pixel of the buffer the swap callback got last, as 0xAARRGGBB */
static uint32_t px(int x, int y)
{
	return buf[shown].map[(size_t)y * (buf[shown].pitch / 4) + x];
}

static int near_px(uint32_t got, int r, int g, int b, int tol)
{
	return abs((int)((got >> 16) & 255) - r) <= tol && abs((int)((got >> 8) & 255) - g) <= tol &&
	       abs((int)(got & 255) - b) <= tol;
}

int main(int argc, char **argv)
{
	EGLint cfgattr[] = { 0x3033, 4 /* WINDOW */, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,
			     0x3040, 4, 0x3038 };
	EGLint ctxattr[] = { 0x3098, 2, 0x3038 };
	const char *out = NULL;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	EGLint maj, min, n;
	GLuint prog, flat;
	int i, k;
	double t0, t1;
	static const GLfloat tri[] = {
		-0.8f, -0.7f, 1, 0, 0,   0.8f, -0.7f, 0, 1, 0,   0.0f, 0.8f, 0, 0, 1,
	};
	static const GLfloat quad[] = {
		-1, -1, 0, 0, 0,  1, -1, 0, 0, 0,  -1, 1, 0, 0, 0,
		-1, 1, 0, 0, 0,   1, -1, 0, 0, 0,   1, 1, 0, 0, 0,
	};

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-d"))
			setenv("NETCAST_WSEGL_DEBUG", "1", 1);

	lib_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	lib_gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	if (!lib_egl || !lib_gl) { fprintf(stderr, "sgxscan: dlopen: %s\n", dlerror()); return 2; }
	E(GetDisplay); E(Initialize); E(ChooseConfig); E(CreateWindowSurface); E(CreateContext);
	E(MakeCurrent); E(SwapBuffers); E(DestroySurface); E(DestroyContext); E(Terminate); E(GetError);
	G(GetString); G(ClearColor); G(Clear); G(Viewport); G(Enable); G(Disable); G(BlendFunc);
	G(CreateShader); G(ShaderSource); G(CompileShader); G(GetShaderiv); G(CreateProgram);
	G(AttachShader); G(BindAttribLocation); G(LinkProgram); G(GetProgramiv); G(UseProgram);
	G(VertexAttribPointer); G(EnableVertexAttribArray); G(DrawArrays); G(GetError);

	dpy = egl.GetDisplay(NULL);		/* opens pvrsrvkm first */
	if (!dpy || !egl.Initialize(dpy, &maj, &min)) {
		fprintf(stderr, "sgxscan: eglInitialize failed (0x%x)\n", egl.GetError());
		return 4;
	}
	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) { perror("sgxscan: /dev/dri/card0"); return 4; }
	if (!kms_buffers(2)) return 4;
	if (buf[0].pitch != W * 4 || buf[0].size < (uint64_t)W * H * 4) {
		fprintf(stderr, "sgxscan: unexpected pitch/size\n");
		kms_free(2);
		return 4;
	}
	/* a pattern the GPU must overwrite completely */
	for (k = 0; k < 2; k++)
		for (i = 0; i < W * H; i++)
			buf[k].map[i] = 0xa5a5a5a5u;

	win.magic = NETCAST_WINDOW_MAGIC;
	win.width = W; win.height = H; win.stride = W; win.nbuffers = 2; win.size = W * H * 4;
	win.map[0] = buf[0].map; win.map[1] = buf[1].map;
	win.swap = on_swap;

	if (!egl.ChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "sgxscan: no window config (0x%x)\n", egl.GetError());
		return 4;
	}
	surf = egl.CreateWindowSurface(dpy, cfg, &win, NULL);
	if (!surf) {
		fprintf(stderr, "sgxscan: eglCreateWindowSurface failed (0x%x): the wrap or the WSEGL window was refused\n", egl.GetError());
		kms_free(2);
		return 5;
	}
	ctx = egl.CreateContext(dpy, cfg, NULL, ctxattr);
	if (!ctx || !egl.MakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "sgxscan: context/MakeCurrent failed (0x%x)\n", egl.GetError());
		return 5;
	}
	printf("window surface on KMS buffers: %s\n", gl.GetString(0x1F01));
	gl.Viewport(0, 0, W, H);

	/* 1: solid frames, six of them, alternating buffers, each a different colour; corners + centre */
	printf("solid frames\n");
	for (i = 0; i < 6; i++) {
		int r = 40 + 30 * i, g = 200 - 25 * i, b = 10 + 12 * i;
		char what[64];

		gl.ClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1);
		gl.Clear(0x4000);
		if (!egl.SwapBuffers(dpy, surf)) {
			fprintf(stderr, "sgxscan: eglSwapBuffers failed (0x%x)\n", egl.GetError());
			bad = 1;
			break;
		}
		snprintf(what, sizeof(what), "frame %d (buffer %u): corners, centre, edges", i, shown);
		check(nswaps == (unsigned)i + 1 && near_px(px(0, 0), r, g, b, 1) &&
		      near_px(px(W - 1, 0), r, g, b, 1) && near_px(px(0, H - 1), r, g, b, 1) &&
		      near_px(px(W - 1, H - 1), r, g, b, 1) && near_px(px(W / 2, H / 2), r, g, b, 1) &&
		      near_px(px(959, 1), r, g, b, 1), what);
		if (i == 0)
			printf("    first pixel 0x%08x (AARRGGBB; want ~ff%02x%02x%02x)\n", px(0, 0), r, g, b);
	}
	{
		/* every pixel of the last frame: the whole buffer was written, with no stale pattern */
		uint32_t ref = px(0, 0);
		size_t diff = 0;

		for (i = 0; i < W * H; i++)
			diff += buf[shown].map[i] != ref;
		check(diff == 0, "every pixel of the last frame is the clear colour");
	}

	/* 2: a triangle, orientation and shading */
	printf("triangle\n");
	prog = program("attribute vec2 pos; attribute vec3 col; varying vec3 v;"
		       "void main() { gl_Position = vec4(pos, 0.0, 1.0); v = col; }",
		       "precision mediump float; varying vec3 v; void main() { gl_FragColor = vec4(v, 1.0); }");
	gl.UseProgram(prog);
	gl.VertexAttribPointer(0, 2, 0x1406, 0, 20, tri);
	gl.VertexAttribPointer(1, 3, 0x1406, 0, 20, tri + 2);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.ClearColor(0.25f, 0.5f, 0.75f, 1);
	gl.Clear(0x4000);
	gl.DrawArrays(4, 0, 3);
	egl.SwapBuffers(dpy, surf);
	check(near_px(px(0, 0), 0x40, 0x80, 0xbf, 2), "corner = clear colour");
	{
		uint32_t top = px(W / 2, 110), bl = px(W / 10 + 12, H * 85 / 100 - 8);

		/* GL's y goes up: the blue apex must be at the top of the buffer (row 0 = top of the panel) */
		check((top & 255) > 200 && ((top >> 16) & 255) < 60, "apex (blue) near the top row: image is the right way up");
		check(((bl >> 16) & 255) > 180 && (bl & 255) < 60, "bottom left is red");
	}
	if (out) {
		FILE *f = fopen(out, "wb");
		int x, y;

		if (f) {
			fprintf(f, "P6\n480 270\n255\n");
			for (y = 0; y < H; y += 4)
				for (x = 0; x < W; x += 4) {
					uint32_t p = px(x, y);
					unsigned char c[3] = { p >> 16, p >> 8, p };
					fwrite(c, 1, 3, f);
				}
			fclose(f);
		}
	}

	/* 3: is the wait real? 20 additive blended layers (slow), read right after the swap */
	printf("GPU wait\n");
	flat = program("attribute vec2 pos; attribute vec3 col; varying vec3 v;"
		       "void main() { gl_Position = vec4(pos, 0.0, 1.0); v = col; }",
		       "precision mediump float; varying vec3 v;"
		       "void main() { gl_FragColor = vec4(1.0 / 255.0) + 0.0001 * vec4(v, 0.0); }");
	gl.UseProgram(flat);
	gl.VertexAttribPointer(0, 2, 0x1406, 0, 20, quad);
	gl.VertexAttribPointer(1, 3, 0x1406, 0, 20, quad + 2);
	gl.Enable(0x0BE2);
	gl.BlendFunc(1, 1);
	for (k = 0; k < 2; k++) {
		char what[80];
		uint32_t got[2];

		/* an already correct frame first: black */
		win.no_wait = 0;
		gl.ClearColor(0, 0, 0, 0);
		gl.Clear(0x4000);
		egl.SwapBuffers(dpy, surf);
		egl.SwapBuffers(dpy, surf);
		win.no_wait = k;		/* k = 1: no wait, to see what that looks like */
		t0 = now();
		gl.Clear(0x4000);
		for (i = 0; i < 20; i++)
			gl.DrawArrays(4, 0, 6);
		egl.SwapBuffers(dpy, surf);
		got[0] = px(960, 540);
		t1 = now();
		usleep(200000);
		got[1] = px(960, 540);
		snprintf(what, sizeof(what), "%s: at once %3d, 200 ms later %3d (want 20), swap took %.1f ms",
			 k ? "no wait" : "wait", (int)(got[0] & 255), (int)(got[1] & 255), (t1 - t0) * 1e3);
		if (k == 0)
			check(abs((int)(got[0] & 255) - 20) <= 2, what);
		else
			printf("  %-58s (for comparison)\n", what);
	}
	win.no_wait = 0;
	gl.Disable(0x0BE2);

	/* 4: frame rate with a small scene, waiting for each frame */
	gl.UseProgram(prog);
	gl.VertexAttribPointer(0, 2, 0x1406, 0, 20, tri);
	gl.VertexAttribPointer(1, 3, 0x1406, 0, 20, tri + 2);
	t0 = now();
	for (i = 0; i < 100; i++) {
		gl.ClearColor(0.1f, (i & 15) / 32.0f, 0.2f, 1);
		gl.Clear(0x4000);
		gl.DrawArrays(4, 0, 3);
		egl.SwapBuffers(dpy, surf);
	}
	t1 = now();
	printf("100 simple 1080p frames, waited: %.1f fps (%.2f ms per frame)\n", 100 / (t1 - t0),
	       (t1 - t0) * 10);

	printf("glGetError 0x%x\n", gl.GetError());
	egl.MakeCurrent(dpy, NULL, NULL, NULL);
	egl.DestroySurface(dpy, surf);		/* unwraps the buffers: before they are freed */
	egl.DestroyContext(dpy, ctx);
	egl.Terminate(dpy);
	kms_free(2);
	close(drm_fd);
	printf("%s\n", bad ? "FAIL" : "PASS");
	return bad;
}
