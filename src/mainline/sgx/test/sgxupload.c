// SPDX-License-Identifier: MIT
/*
 * sgxupload: how fast do CPU-drawn pixels reach the SGX544? glTexImage2D/glTexSubImage2D in several
 * formats and sizes, each frame a different colour, the texture sampled into a small pbuffer and
 * read back (so the upload is complete and correct), and the CPU side of the zero-copy alternative:
 * writing straight into a KMS dumb buffer (write-combined memory). No display is touched.
 */
#define _FILE_OFFSET_BITS 64
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef float GLfloat;
typedef unsigned int GLenum, GLuint;

static struct {
	EGLDisplay (*GetDisplay)(void *);
	unsigned (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	unsigned (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	unsigned (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLint (*GetError)(void);
} egl;
static struct {
	const unsigned char *(*GetString)(GLenum);
	void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (*Clear)(unsigned);
	void (*Viewport)(int, int, int, int);
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, int, const char *const *, const int *);
	void (*CompileShader)(GLuint);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*BindAttribLocation)(GLuint, GLuint, const char *);
	void (*LinkProgram)(GLuint);
	void (*UseProgram)(GLuint);
	int (*GetUniformLocation)(GLuint, const char *);
	void (*Uniform1i)(int, int);
	void (*VertexAttribPointer)(GLuint, int, GLenum, unsigned char, int, const void *);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DrawArrays)(GLenum, int, int);
	void (*ReadPixels)(int, int, int, int, GLenum, GLenum, void *);
	void (*GenTextures)(int, GLuint *);
	void (*DeleteTextures)(int, const GLuint *);
	void (*BindTexture)(GLenum, GLuint);
	void (*TexParameteri)(GLenum, GLenum, int);
	void (*TexImage2D)(GLenum, int, int, int, int, int, GLenum, GLenum, const void *);
	void (*TexSubImage2D)(GLenum, int, int, int, int, int, GLenum, GLenum, const void *);
	GLenum (*GetError)(void);
} gl;

#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGB 0x1907
#define GL_UB 0x1401
#define GL_565 0x8363

static void *le, *lg;
static int bad;

static void *sym(void *l, const char *n)
{
	void *p = dlsym(l, n);
	if (!p) { fprintf(stderr, "sgxupload: no %s\n", n); exit(2); }
	return p;
}
#define E(f) (*(void **)&egl.f = sym(le, "egl" #f))
#define G(f) (*(void **)&gl.f = sym(lg, "gl" #f))

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint mk(GLenum type, const char *src)
{
	GLuint s = gl.CreateShader(type);
	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	return s;
}

static void draw(void)
{
	static const GLfloat q[] = {
		-1, -1, 0, 0,  1, -1, 1, 0,  -1, 1, 0, 1,
		-1, 1, 0, 1,   1, -1, 1, 0,   1, 1, 1, 1,
	};
	gl.VertexAttribPointer(0, 2, 0x1406, 0, 16, q);
	gl.VertexAttribPointer(1, 2, 0x1406, 0, 16, q + 2);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.DrawArrays(4, 0, 6);
}

/* Fill w x h with one colour in the given format; returns bytes */
static size_t fill(void *buf, int w, int h, GLenum fmt, GLenum type, unsigned r, unsigned g, unsigned b)
{
	if (type == GL_565) {
		uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), *p = buf;
		for (int i = 0; i < w * h; i++) p[i] = v;
		return (size_t)w * h * 2;
	}
	if (fmt == GL_RGB) {
		unsigned char *p = buf;
		for (int i = 0; i < w * h; i++, p += 3) { p[0] = r; p[1] = g; p[2] = b; }
		return (size_t)w * h * 3;
	}
	{
		uint32_t v = fmt == GL_BGRA ? (0xffu << 24 | r << 16 | g << 8 | b)
					    : (0xffu << 24 | b << 16 | g << 8 | r), *p = buf;
		for (int i = 0; i < w * h; i++) p[i] = v;
		return (size_t)w * h * 4;
	}
}

static int sample(unsigned r, unsigned g, unsigned b, int tol)
{
	unsigned char c[4];
	gl.Clear(0x4000);
	draw();
	gl.ReadPixels(32, 32, 1, 1, GL_RGBA, GL_UB, c);
	return abs(c[0] - (int)r) <= tol && abs(c[1] - (int)g) <= tol && abs(c[2] - (int)b) <= tol;
}

/* n frames; sub = TexSubImage2D of the whole texture, else TexImage2D each time. Times the GL calls
 * and the calls + draw + one-pixel readback. */
static void run(const char *name, int w, int h, GLenum fmt, GLenum type, int bpp, int sub, int n)
{
	size_t max = (size_t)w * h * 4;
	void *buf = malloc(max);
	GLuint t;
	double tc = 0, ta, tot = 0;
	int i, wrong = 0, tol = type == GL_565 ? 9 : 2;

	gl.GenTextures(1, &t);
	gl.BindTexture(0x0DE1, t);
	gl.TexParameteri(0x0DE1, 0x2801, 0x2600);
	gl.TexParameteri(0x0DE1, 0x2800, 0x2600);
	gl.TexParameteri(0x0DE1, 0x2802, 0x812F);
	gl.TexParameteri(0x0DE1, 0x2803, 0x812F);
	memset(buf, 0, max);
	gl.TexImage2D(0x0DE1, 0, fmt, w, h, 0, fmt, type, sub ? NULL : buf);
	if (gl.GetError()) { printf("%-46s not supported\n", name); free(buf); gl.DeleteTextures(1, &t); return; }
	sample(0, 0, 0, 255);
	for (i = 0; i < n; i++) {
		unsigned r = 30 + 20 * (i % 10), g = 220 - 15 * (i % 11), b = 17 * (i % 13);
		size_t bytes = fill(buf, w, h, fmt, type, r, g, b);
		(void)bytes;
		ta = now();
		if (sub)
			gl.TexSubImage2D(0x0DE1, 0, 0, 0, w, h, fmt, type, buf);
		else
			gl.TexImage2D(0x0DE1, 0, fmt, w, h, 0, fmt, type, buf);
		tc += now() - ta;
		if (!sample(r, g, b, tol))
			wrong++;
		tot += now() - ta;
	}
	printf("%-46s %7.1f ms call, %7.1f ms with draw+read  = %6.0f MB/s  %s\n", name, tc / n * 1e3, tot / n * 1e3,
	       (double)w * h * bpp * n / tot / 1e6, wrong ? "WRONG PIXELS" : "");
	if (wrong)
		bad = 1;
	if (gl.GetError())
		printf("   GL error\n"), bad = 1;
	gl.DeleteTextures(1, &t);
	free(buf);
}

struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };

/* CPU memory bandwidth: cached to cached, cached to write-combined dumb buffer (and 32-bit fills). */
static void cpu_tests(void)
{
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	struct create_dumb c = { .height = 1080, .width = 1920, .bpp = 32 };
	struct map_dumb m = { 0 };
	size_t n = 1920 * 1080 * 4;
	uint8_t *a = malloc(n), *b = malloc(n);
	uint32_t *wc;
	double t0, t;
	int i;

	memset(a, 1, n);
	memset(b, 2, n);
	t0 = now();
	for (i = 0; i < 5; i++) memcpy(b, a, n);
	t = now() - t0;
	printf("%-46s %7.1f ms per 1080p frame = %6.0f MB/s\n", "CPU memcpy cached -> cached", t / 5 * 1e3, n * 5 / t / 1e6);
	if (fd < 0 || ioctl(fd, 0xc02064b2u, &c) || (m.handle = c.handle, ioctl(fd, 0xc01064b3u, &m))) {
		printf("no dumb buffer\n");
		return;
	}
	wc = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (wc == MAP_FAILED) { printf("no mmap\n"); return; }
	t0 = now();
	for (i = 0; i < 5; i++) memcpy(wc, a, n);
	t = now() - t0;
	printf("%-46s %7.1f ms per 1080p frame = %6.0f MB/s\n", "CPU memcpy cached -> screen buffer (WC)", t / 5 * 1e3, n * 5 / t / 1e6);
	t0 = now();
	for (i = 0; i < 5; i++) memset(wc, i, n);
	t = now() - t0;
	printf("%-46s %7.1f ms per 1080p frame = %6.0f MB/s\n", "CPU memset -> screen buffer (WC)", t / 5 * 1e3, n * 5 / t / 1e6);
	t0 = now();
	memcpy(a, wc, n);
	t = now() - t0;
	printf("%-46s %7.1f ms per 1080p frame = %6.0f MB/s\n", "CPU memcpy screen buffer (WC) -> cached", t * 1e3, n / t / 1e6);
	munmap(wc, c.size);
	ioctl(fd, 0xc00464b4u, &c.handle);
	close(fd);
	free(a);
	free(b);
}

int main(void)
{
	EGLint cfgattr[] = { 0x3033, 1, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 4, 0x3038 };
	EGLint ctxattr[] = { 0x3098, 2, 0x3038 }, sattr[] = { 0x3057, 64, 0x3056, 64, 0x3038 };
	EGLint maj, min, n;
	EGLDisplay d;
	EGLConfig cfg;
	EGLSurface s;
	EGLContext ctx;
	GLuint p;
	const char *ext;

	le = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	lg = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	if (!le || !lg) { fprintf(stderr, "sgxupload: dlopen: %s\n", dlerror()); return 2; }
	E(GetDisplay); E(Initialize); E(ChooseConfig); E(CreatePbufferSurface); E(CreateContext);
	E(MakeCurrent); E(GetError);
	G(GetString); G(ClearColor); G(Clear); G(Viewport); G(CreateShader); G(ShaderSource); G(CompileShader);
	G(CreateProgram); G(AttachShader); G(BindAttribLocation); G(LinkProgram); G(UseProgram);
	G(GetUniformLocation); G(Uniform1i); G(VertexAttribPointer); G(EnableVertexAttribArray); G(DrawArrays);
	G(ReadPixels); G(GenTextures); G(DeleteTextures); G(BindTexture); G(TexParameteri); G(TexImage2D);
	G(TexSubImage2D); G(GetError);
	d = egl.GetDisplay(NULL);
	if (!d || !egl.Initialize(d, &maj, &min) || !egl.ChooseConfig(d, cfgattr, &cfg, 1, &n) || n < 1 ||
	    !(s = egl.CreatePbufferSurface(d, cfg, sattr)) || !(ctx = egl.CreateContext(d, cfg, NULL, ctxattr)) ||
	    !egl.MakeCurrent(d, s, s, ctx)) {
		fprintf(stderr, "sgxupload: EGL failed 0x%x\n", egl.GetError());
		return 4;
	}
	ext = (const char *)gl.GetString(0x1F03);
	printf("GL extensions: %s\n\n", ext ? ext : "?");
	p = gl.CreateProgram();
	gl.AttachShader(p, mk(0x8B31, "attribute vec2 pos; attribute vec2 uv; varying vec2 t;"
				      "void main() { gl_Position = vec4(pos, 0.0, 1.0); t = uv; }"));
	gl.AttachShader(p, mk(0x8B30, "precision mediump float; varying vec2 t; uniform sampler2D s;"
				      "void main() { gl_FragColor = texture2D(s, t); }"));
	gl.BindAttribLocation(p, 0, "pos");
	gl.BindAttribLocation(p, 1, "uv");
	gl.LinkProgram(p);
	gl.UseProgram(p);
	gl.Uniform1i(gl.GetUniformLocation(p, "s"), 0);
	gl.Viewport(0, 0, 64, 64);
	gl.ClearColor(0, 0, 0, 0);

	cpu_tests();
	printf("\n");
	run("1080p RGBA8888 TexSubImage2D", 1920, 1080, GL_RGBA, GL_UB, 4, 1, 8);
	run("1080p BGRA8888 TexSubImage2D", 1920, 1080, GL_BGRA, GL_UB, 4, 1, 8);
	run("1080p RGBA8888 TexImage2D", 1920, 1080, GL_RGBA, GL_UB, 4, 0, 8);
	run("1080p RGB565   TexSubImage2D", 1920, 1080, GL_RGB, GL_565, 2, 1, 8);
	run("720p  RGBA8888 TexSubImage2D", 1280, 720, GL_RGBA, GL_UB, 4, 1, 12);
	run("720p  BGRA8888 TexSubImage2D", 1280, 720, GL_BGRA, GL_UB, 4, 1, 12);
	run("512x512 RGBA8888 TexSubImage2D", 512, 512, GL_RGBA, GL_UB, 4, 1, 20);
	run("256x256 RGBA8888 TexSubImage2D", 256, 256, GL_RGBA, GL_UB, 4, 1, 40);
	run("64x64   RGBA8888 TexSubImage2D", 64, 64, GL_RGBA, GL_UB, 4, 1, 100);
	printf("\n%s\n", bad ? "RESULTS WRONG" : "every upload checked: correct");
	return bad;
}
