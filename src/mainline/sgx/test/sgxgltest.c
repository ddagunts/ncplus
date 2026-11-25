// SPDX-License-Identifier: MIT
/*
 * sgxgltest: first-frame test of the SGX544 under the mainline kernel.
 *
 * Runs LG's own OpenGL ES 2 userspace (DDK 1.9, armel, from the TV) against our pvrsrvkm.ko,
 * with no window system: EGL pbuffer, clear, one shaded triangle, glReadPixels, then a
 * benchmark (-b). Writes the frame as a PPM. The libraries are dlopen()ed, so it builds without them:
 *
 *   LD_LIBRARY_PATH=<dir with LG's libs> sgxgltest [-o frame.ppm] [-s size] [-b]
 *
 * The directory needs libEGL.so, libGLESv2.so (IMGegl dlopens that name), libIMGegl.so,
 * libsrv_um.so, libusc.so, libglslcompiler.so, libpvrPVR2D_NullWSEGL.so and a powervr.ini
 * (WindowSystem=libpvrPVR2D_NullWSEGL.so) in /etc. pvrsrvinit must have run once.
 * The benchmark only counts work whose result it reads back and checks: a tile-based GPU drops
 * frames that nobody reads and, with opaque overdraw, hidden layers. So every frame ends in a
 * 1-pixel glReadPixels, and the fill tests blend additively (value = number of layers).
 * Exit status 0 = pixels as expected.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CLIENT_APIS 0x308D

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte, GLboolean;
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_BLEND 0x0BE2
#define GL_ONE 1
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

static struct {
	EGLDisplay (*GetDisplay)(void *);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	const char *(*QueryString)(EGLDisplay, EGLint);
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLint (*GetError)(void);
	EGLBoolean (*Terminate)(EGLDisplay);
} egl;

static struct {
	const GLubyte *(*GetString)(GLenum);
	void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (*Clear)(GLbitfield);
	void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
	void (*CompileShader)(GLuint);
	void (*GetShaderiv)(GLuint, GLenum, GLint *);
	void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*BindAttribLocation)(GLuint, GLuint, const GLchar *);
	void (*LinkProgram)(GLuint);
	void (*GetProgramiv)(GLuint, GLenum, GLint *);
	void (*UseProgram)(GLuint);
	GLint (*GetUniformLocation)(GLuint, const GLchar *);
	void (*Uniform1f)(GLint, GLfloat);
	void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DrawArrays)(GLenum, GLint, GLsizei);
	void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
	void (*Finish)(void);
	void (*Enable)(GLenum);
	void (*Disable)(GLenum);
	void (*BlendFunc)(GLenum, GLenum);
	void (*GenTextures)(GLsizei, GLuint *);
	void (*BindTexture)(GLenum, GLuint);
	void (*TexParameteri)(GLenum, GLenum, GLint);
	void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
	void (*Uniform1i)(GLint, GLint);
	void (*GenBuffers)(GLsizei, GLuint *);
	void (*BindBuffer)(GLenum, GLuint);
	void (*BufferData)(GLenum, long, const void *, GLenum);
	GLenum (*GetError)(void);
} gl;

static void *sym(void *lib, const char *name)
{
	void *p = dlsym(lib, name);

	if (!p) {
		fprintf(stderr, "sgxgltest: no %s\n", name);
		exit(2);
	}
	return p;
}

#define LOAD(tab, lib, pfx, f) (*(void **)&tab.f = sym(lib, pfx #f))

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = gl.CreateShader(type);
	GLint ok = 0;
	char log[512] = "";

	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		gl.GetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "sgxgltest: shader compile failed: %s\n", log);
		exit(3);
	}
	return s;
}

static const char *vs =
	"attribute vec2 pos;\n"
	"attribute vec3 col;\n"
	"varying vec3 v;\n"
	"uniform float t;\n"
	"void main() {\n"
	"  float c = cos(t), s = sin(t);\n"
	"  gl_Position = vec4(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0);\n"
	"  v = col;\n"
	"}\n";
static const char *fs =
	"precision mediump float;\n"
	"varying vec3 v;\n"
	"void main() { gl_FragColor = vec4(v, 1.0); }\n";

/* --- benchmark ----------------------------------------------------------------------- */
#define BW 1280
#define BH 720

/* one fullscreen quad as two triangles; "flat" adds 1/255 per layer, "tex" adds a texel */
static const char *vs_quad =
	"attribute vec2 pos;\n"
	"attribute vec2 uv;\n"
	"varying vec2 tc;\n"
	"void main() { gl_Position = vec4(pos, 0.0, 1.0); tc = uv; }\n";
static const char *fs_flat =
	"precision mediump float;\n"
	"varying vec2 tc;\n"
	"void main() { gl_FragColor = vec4(1.0 / 255.0); }\n";
static const char *fs_tex =
	"precision mediump float;\n"
	"varying vec2 tc;\n"
	"uniform sampler2D s;\n"
	"void main() { gl_FragColor = texture2D(s, tc); }\n";

static GLuint program(const char *v, const char *f)
{
	GLuint p = gl.CreateProgram();
	GLint ok = 0;

	gl.AttachShader(p, shader(GL_VERTEX_SHADER, v));
	gl.AttachShader(p, shader(GL_FRAGMENT_SHADER, f));
	gl.BindAttribLocation(p, 0, "pos");
	gl.BindAttribLocation(p, 1, "uv");
	gl.LinkProgram(p);
	gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "sgxgltest: link failed\n");
		exit(3);
	}
	return p;
}

/* the pixel at (x, y); completes everything drawn before it */
static void pixel(int x, int y, unsigned char *c)
{
	gl.ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
}

static int near_(int a, int b, int tol)
{
	return a >= b - tol && a <= b + tol;
}

/*
 * 'layers' additive fullscreen quads per frame, 'frames' frames, each frame cleared and checked.
 * Returns the pixel rate in Mpixel/s or <0 if a frame's result was wrong.
 */
static double fill(int layers, int frames, int expect_per_layer, double *frame_ms, int *worst)
{
	unsigned char c[4];
	double t0, t1;
	int i, l, err = 0;

	*worst = 0;
	gl.ClearColor(0, 0, 0, 0);
	gl.Clear(GL_COLOR_BUFFER_BIT);	/* a warm-up frame: not timed */
	gl.DrawArrays(GL_TRIANGLES, 0, 6);
	pixel(5, 5, c);
	t0 = now();
	for (i = 0; i < frames; i++) {
		gl.Clear(GL_COLOR_BUFFER_BIT);
		for (l = 0; l < layers; l++)
			gl.DrawArrays(GL_TRIANGLES, 0, 6);
		pixel((37 * i) % BW, (59 * i) % BH, c);
		if (abs(c[0] - layers * expect_per_layer) > *worst)
			*worst = abs(c[0] - layers * expect_per_layer);
		if (!near_(c[0], layers * expect_per_layer, 2 + layers / 10)) {
			err++;
			printf("  frame %d: got %d, want %d\n", i, c[0], layers * expect_per_layer);
		}
	}
	t1 = now();
	*frame_ms = (t1 - t0) * 1e3 / frames;
	return err ? -1 : (double)BW * BH * layers * frames / (t1 - t0) / 1e6;
}

static int benchmark(EGLDisplay dpy, EGLConfig cfg, EGLContext ctx, EGLSurface old)
{
	EGLint sattr[] = { EGL_WIDTH, BW, EGL_HEIGHT, BH, EGL_NONE };
	EGLSurface surf = egl.CreatePbufferSurface(dpy, cfg, sattr);
	static const GLfloat quad[] = {
		-1, -1, 0, 0,   1, -1, 1, 0,   -1, 1, 0, 1,
		-1,  1, 0, 1,   1, -1, 1, 0,    1, 1, 1, 1,
	};
	static unsigned char tex[256 * 256 * 4];
	unsigned char c[4], *px;
	GLuint pf, pt, vb, vb2, tx;
	int i, j, x, y, bad = 0, worst;
	double t0, t1, ms, r;

	if (!surf || !egl.MakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "sgxgltest: benchmark pbuffer failed (0x%x)\n", egl.GetError());
		return 1;
	}
	gl.Viewport(0, 0, BW, BH);
	printf("benchmark at %dx%d\n", BW, BH);

	/* frame rate: clear to a different colour each frame, read it back */
	t0 = now();
	for (i = 0; i < 120; i++) {
		gl.ClearColor(((i * 7) & 255) / 255.0f, 0, 0, 1);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		pixel(i, i, c);
		if (!near_(c[0], (i * 7) & 255, 1))
			bad |= 1, printf("  clear frame %d: got %d, want %d\n", i, c[0], (i * 7) & 255);
	}
	t1 = now();
	printf("clear + 1 pixel readback:     %7.1f fps (%.2f ms per frame)\n",
	       120 / (t1 - t0), (t1 - t0) * 1e3 / 120);

	/* what glFinish alone costs, and whether it really waits: compare with the readback */
	gl.GenBuffers(1, &vb);
	gl.BindBuffer(GL_ARRAY_BUFFER, vb);
	gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), (void *)0);
	gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	pf = program(vs_quad, fs_flat);
	pt = program(vs_quad, fs_tex);
	gl.Enable(GL_BLEND);
	gl.BlendFunc(GL_ONE, GL_ONE);
	gl.UseProgram(pf);
	for (j = 0; j < 2; j++) {
		gl.ClearColor(0, 0, 0, 0);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		t0 = now();
		for (i = 0; i < 20; i++)
			gl.DrawArrays(GL_TRIANGLES, 0, 6);
		if (j == 0)
			gl.Finish();
		else
			pixel(3, 3, c);
		t1 = now();
		printf("20 blended layers, %-12s %7.1f ms\n", j ? "1-px readback:" : "glFinish:",
		       (t1 - t0) * 1e3);
	}

	/* fill rate */
	r = fill(10, 10, 1, &ms, &worst);
	printf("flat blended fill, 10 layers:  %7.0f Mpixel/s (%.1f ms per frame)%s\n", r, ms,
	       r < 0 ? "  WRONG" : "");
	bad |= r < 0;
	r = fill(30, 10, 1, &ms, &worst);
	printf("flat blended fill, 30 layers:  %7.0f Mpixel/s (%.1f ms per frame)%s\n", r, ms,
	       r < 0 ? "  WRONG" : "");
	bad |= r < 0;

	for (i = 0; i < 256 * 256 * 4; i++)
		tex[i] = 1;	/* every texel adds 1/255 */
	gl.GenTextures(1, &tx);
	gl.BindTexture(GL_TEXTURE_2D, tx);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
	gl.UseProgram(pt);
	gl.Uniform1i(gl.GetUniformLocation(pt, "s"), 0);
	r = fill(10, 10, 1, &ms, &worst);
	printf("textured blended fill, 10 l.:  %7.0f Mpixel/s (%.1f ms per frame)%s\n", r, ms,
	       r < 0 ? "  WRONG" : "");
	bad |= r < 0;
	r = fill(30, 10, 1, &ms, &worst);
	printf("textured blended fill, 30 l.:  %7.0f Mpixel/s (%.1f ms per frame)%s\n", r, ms,
	       r < 0 ? "  WRONG" : "");
	bad |= r < 0;

	/* triangles: a grid that tiles the screen exactly, 200 x 100 quads = 40000 triangles,
	 * additive, so every pixel must read exactly the number of passes */
	{
		enum { GX = 200, GY = 100, PASSES = 6, FRAMES = 10 };
		GLfloat *v = malloc(GX * GY * 6 * 4 * sizeof(GLfloat)), *p = v;
		int f, k, wrong = 0;

		for (y = 0; y < GY; y++)
			for (x = 0; x < GX; x++) {
				GLfloat x0 = -1 + 2.0f * x / GX, x1 = -1 + 2.0f * (x + 1) / GX;
				GLfloat y0 = -1 + 2.0f * y / GY, y1 = -1 + 2.0f * (y + 1) / GY;
				GLfloat q[6][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 },
						    { x0, y1 }, { x1, y0 }, { x1, y1 } };

				for (k = 0; k < 6; k++) {
					*p++ = q[k][0];
					*p++ = q[k][1];
					*p++ = 0;
					*p++ = 0;
				}
			}
		gl.GenBuffers(1, &vb2);
		gl.BindBuffer(GL_ARRAY_BUFFER, vb2);
		gl.BufferData(GL_ARRAY_BUFFER, GX * GY * 6 * 4 * sizeof(GLfloat), v, GL_STATIC_DRAW);
		gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), (void *)0);
		gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat),
				       (void *)(2 * sizeof(GLfloat)));
		gl.UseProgram(pf);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		gl.DrawArrays(GL_TRIANGLES, 0, GX * GY * 6);
		pixel(5, 5, c);
		t0 = now();
		for (f = 0; f < FRAMES; f++) {
			gl.Clear(GL_COLOR_BUFFER_BIT);
			for (k = 0; k < PASSES; k++)
				gl.DrawArrays(GL_TRIANGLES, 0, GX * GY * 6);
			pixel((101 * f) % BW, (67 * f) % BH, c);
			if (!near_(c[0], PASSES, 1))
				wrong++, printf("  frame %d: got %d, want %d\n", f, c[0], PASSES);
		}
		t1 = now();
		bad |= wrong != 0;
		printf("%d triangles per frame:       %7.2f Mtriangles/s (%.1f ms per frame)%s\n",
		       GX * GY * 2 * PASSES, GX * GY * 2.0 * PASSES * FRAMES / (t1 - t0) / 1e6,
		       (t1 - t0) * 1e3 / FRAMES, wrong ? "  WRONG" : "");
		free(v);
	}

	/* the readback path itself: the whole surface */
	px = malloc(BW * BH * 4);
	t0 = now();
	for (i = 0; i < 5; i++) {
		gl.ClearColor(0, i / 255.0f, 0, 1);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		gl.ReadPixels(0, 0, BW, BH, GL_RGBA, GL_UNSIGNED_BYTE, px);
		if (!near_(px[1], i, 1) || !near_(px[(BW * BH - 1) * 4 + 1], i, 1))
			bad |= 1, printf("  readback %d: got %d\n", i, px[1]);
	}
	t1 = now();
	printf("full frame glReadPixels:      %7.1f ms per frame (%.0f MB/s)\n",
	       (t1 - t0) * 1e3 / 5, BW * BH * 4.0 * 5 / (t1 - t0) / 1e6);
	free(px);

	gl.Disable(GL_BLEND);
	printf("benchmark glGetError 0x%x, %s\n", gl.GetError(), bad ? "RESULTS WRONG" : "every checked frame correct");
	egl.MakeCurrent(dpy, old, old, ctx);
	return bad;
}

int main(int argc, char **argv)
{
	const char *out = "sgxgltest.ppm";
	int size = 256, i, bad = 0, bench = 0;
	EGLint maj, min, n, cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	void *libegl, *libgl;
	GLuint prog;
	GLint ok = 0, ut;
	unsigned char *px;
	static const GLfloat tri[] = {
		/* x, y, r, g, b: a big triangle around the centre */
		-0.8f, -0.7f, 1, 0, 0,
		 0.8f, -0.7f, 0, 1, 0,
		 0.0f,  0.8f, 0, 0, 1,
	};

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			size = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b"))
			bench = 1;
	}

	libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	libgl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	if (!libegl || !libgl) {
		fprintf(stderr, "sgxgltest: dlopen: %s\n", dlerror());
		return 2;
	}
	LOAD(egl, libegl, "egl", GetDisplay); LOAD(egl, libegl, "egl", Initialize);
	LOAD(egl, libegl, "egl", QueryString); LOAD(egl, libegl, "egl", ChooseConfig);
	LOAD(egl, libegl, "egl", CreatePbufferSurface); LOAD(egl, libegl, "egl", CreateContext);
	LOAD(egl, libegl, "egl", MakeCurrent); LOAD(egl, libegl, "egl", GetError);
	LOAD(egl, libegl, "egl", Terminate);
	LOAD(gl, libgl, "gl", GetString); LOAD(gl, libgl, "gl", ClearColor);
	LOAD(gl, libgl, "gl", Clear); LOAD(gl, libgl, "gl", Viewport);
	LOAD(gl, libgl, "gl", CreateShader); LOAD(gl, libgl, "gl", ShaderSource);
	LOAD(gl, libgl, "gl", CompileShader); LOAD(gl, libgl, "gl", GetShaderiv);
	LOAD(gl, libgl, "gl", GetShaderInfoLog); LOAD(gl, libgl, "gl", CreateProgram);
	LOAD(gl, libgl, "gl", AttachShader); LOAD(gl, libgl, "gl", BindAttribLocation);
	LOAD(gl, libgl, "gl", LinkProgram); LOAD(gl, libgl, "gl", GetProgramiv);
	LOAD(gl, libgl, "gl", UseProgram); LOAD(gl, libgl, "gl", GetUniformLocation);
	LOAD(gl, libgl, "gl", Uniform1f); LOAD(gl, libgl, "gl", VertexAttribPointer);
	LOAD(gl, libgl, "gl", EnableVertexAttribArray); LOAD(gl, libgl, "gl", DrawArrays);
	LOAD(gl, libgl, "gl", ReadPixels); LOAD(gl, libgl, "gl", Finish);
	LOAD(gl, libgl, "gl", GetError); LOAD(gl, libgl, "gl", Enable);
	LOAD(gl, libgl, "gl", Disable); LOAD(gl, libgl, "gl", BlendFunc);
	LOAD(gl, libgl, "gl", GenTextures); LOAD(gl, libgl, "gl", BindTexture);
	LOAD(gl, libgl, "gl", TexParameteri); LOAD(gl, libgl, "gl", TexImage2D);
	LOAD(gl, libgl, "gl", Uniform1i); LOAD(gl, libgl, "gl", GenBuffers);
	LOAD(gl, libgl, "gl", BindBuffer); LOAD(gl, libgl, "gl", BufferData);

	dpy = egl.GetDisplay(NULL);
	if (!dpy || !egl.Initialize(dpy, &maj, &min)) {
		fprintf(stderr, "sgxgltest: eglInitialize failed (0x%x)\n", egl.GetError());
		return 4;
	}
	printf("EGL %d.%d  vendor '%s'  version '%s'  apis '%s'\n", maj, min,
	       egl.QueryString(dpy, EGL_VENDOR), egl.QueryString(dpy, EGL_VERSION),
	       egl.QueryString(dpy, EGL_CLIENT_APIS));
	printf("EGL extensions: %s\n", egl.QueryString(dpy, EGL_EXTENSIONS));
	if (!egl.ChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "sgxgltest: no RGBA8888 ES2 pbuffer config (0x%x)\n", egl.GetError());
		return 4;
	}
	{
		EGLint sattr[] = { EGL_WIDTH, size, EGL_HEIGHT, size, EGL_NONE };

		surf = egl.CreatePbufferSurface(dpy, cfg, sattr);
	}
	ctx = egl.CreateContext(dpy, cfg, NULL, ctxattr);
	if (!surf || !ctx || !egl.MakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "sgxgltest: pbuffer/context failed (0x%x)\n", egl.GetError());
		return 4;
	}
	printf("GL vendor '%s'  renderer '%s'  version '%s'  GLSL '%s'\n",
	       gl.GetString(GL_VENDOR), gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION),
	       gl.GetString(GL_SHADING_LANGUAGE_VERSION));

	px = malloc((size_t)size * size * 4);

	/* 1: a clear the GPU's 3D pass must write */
	gl.Viewport(0, 0, size, size);
	gl.ClearColor(0.25f, 0.5f, 0.75f, 1.0f);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	gl.ReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("clear: pixel(0,0) = %02x %02x %02x %02x (want ~40 80 bf ff)\n",
	       px[0], px[1], px[2], px[3]);
	if (abs(px[0] - 0x40) > 2 || abs(px[1] - 0x80) > 2 || abs(px[2] - 0xbf) > 2)
		bad = 1;

	/* 2: a shaded triangle: vertex + fragment shaders through the USSE */
	prog = gl.CreateProgram();
	gl.AttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	gl.AttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	gl.BindAttribLocation(prog, 0, "pos");
	gl.BindAttribLocation(prog, 1, "col");
	gl.LinkProgram(prog);
	gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "sgxgltest: link failed\n");
		return 3;
	}
	gl.UseProgram(prog);
	ut = gl.GetUniformLocation(prog, "t");
	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(GLfloat), tri);
	gl.VertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(GLfloat), tri + 2);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.Uniform1f(ut, 0.0f);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	gl.ReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, px);
	{
		unsigned char *c = px + ((size / 2) * size + size / 2) * 4;
		unsigned char *k = px + (2 * size + 2) * 4;

		printf("triangle: centre = %02x %02x %02x, corner = %02x %02x %02x (corner = clear)\n",
		       c[0], c[1], c[2], k[0], k[1], k[2]);
		/* the centre mixes all three vertex colours; the corner is outside */
		if (c[0] + c[1] + c[2] < 0x60 || (c[0] == k[0] && c[1] == k[1] && c[2] == k[2]))
			bad = 1;
	}
	{
		FILE *f = fopen(out, "wb");
		int y, x;

		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", size, size);
			for (y = size - 1; y >= 0; y--)
				for (x = 0; x < size; x++)
					fwrite(px + (y * size + x) * 4, 1, 3, f);
			fclose(f);
			printf("wrote %s\n", out);
		}
	}

	/* 3: the benchmark (optional; opt-in because it runs for some seconds) */
	if (bench)
		bad |= benchmark(dpy, cfg, ctx, surf);

	egl.MakeCurrent(dpy, NULL, NULL, NULL);
	egl.Terminate(dpy);
	printf("%s\n", bad ? "FAIL: pixels not as expected" : "PASS");
	return bad;
}
