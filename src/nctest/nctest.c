/*
 * nctest: first native GUI probe for LG NetCast 4.x (MT5398 / PowerVR SGX).
 *
 * Flow: HOA_PROC_RegisterService -> HOA_GX_GetWindowID -> make window visible
 *       -> EGL window surface on that window ID -> GLES2 animation -> exit.
 *
 * All LG libraries are dlopen'ed at runtime, so no LG headers/libs are needed
 * at build time. Signatures were reverse-engineered from libafopenapi.so,
 * gxtest and libpvrPVR2D_CobraNetCast40WSEGL.so; treat them as best guesses.
 *
 * Usage: nctest [--service NAME] [--wid N] [--noreg] [--secs S]
 */
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>

/* LG's libdbgfrwk hijacks stdout/stderr when loaded, so log through a private dup. */
static int g_log = 1;
static void logf_(const char *fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    if (n > 0) write(g_log, b, n);
}
#define printf logf_

/* ---- minimal EGL / GLES2 declarations ---- */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
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

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

#define FN(ret, name, args) static ret(*p_##name) args
FN(EGLDisplay, eglGetDisplay, (void *));
FN(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *));
FN(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *));
FN(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, void *, const EGLint *));
FN(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
FN(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
FN(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface));
FN(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *));
FN(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface));
FN(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext));
FN(EGLBoolean, eglTerminate, (EGLDisplay));
FN(EGLint, eglGetError, (void));
FN(const char *, eglQueryString, (EGLDisplay, EGLint));

FN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
FN(void, glClear, (GLbitfield));
FN(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
FN(GLuint, glCreateShader, (GLenum));
FN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
FN(void, glCompileShader, (GLuint));
FN(void, glGetShaderiv, (GLuint, GLenum, GLint *));
FN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
FN(GLuint, glCreateProgram, (void));
FN(void, glAttachShader, (GLuint, GLuint));
FN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *));
FN(void, glLinkProgram, (GLuint));
FN(void, glGetProgramiv, (GLuint, GLenum, GLint *));
FN(void, glUseProgram, (GLuint));
FN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
FN(GLint, glGetUniformLocation, (GLuint, const GLchar *));
FN(void, glUniform1f, (GLint, GLfloat));
FN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, unsigned char, GLsizei, const void *));
FN(void, glEnableVertexAttribArray, (GLuint));
FN(void, glDrawArrays, (GLenum, GLint, GLsizei));
FN(const GLubyte *, glGetString, (GLenum));

/* ---- HOA (LG app framework) ---- */
typedef struct {
    int (*msg)(void *);          /* MsgHandlerCallback   (gxtest returns 0) */
    int (*key)(int, int, int, int);   /* KeyEventCallback   (returns 1)  */
    int (*mouse)(int, int, int, int); /* MouseEventCallback (returns 1)  */
} hoa_cb_t;
FN(int, HOA_PROC_RegisterService, (int, char **, hoa_cb_t *));
FN(int, HOA_GX_GetWindowID, (uint32_t *));
FN(int, HOA_GX_GetWindowProperty, (uint32_t, void *));
FN(int, HOA_GX_SetWindowProperty, (uint32_t, void *));
FN(int, HOA_GX_GetVisibleWindowList, (uint32_t **, uint32_t *));
FN(int, HOA_GX_UpdateWindow, (uint32_t, void *));
FN(int, HOA_GX_RaiseWindow, (uint32_t));

static void die_sig(int s) {
    char b[64];
    int n = snprintf(b, sizeof b, "nctest: caught signal %d, exiting\n", s);
    write(g_log, b, n);
    _exit(100 + s);
}
static void guard_signals(void) {
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGALRM};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) signal(sigs[i], die_sig);
}

static void *must_open(const char *n, int flags) {
    void *h = dlopen(n, flags);
    printf("  dlopen %-22s %s\n", n, h ? "ok" : dlerror());
    return h;
}
#define SYM(h, name) (p_##name = (__typeof__(p_##name))dlsym(h, #name))

static int cb_msg(void *m) { (void)m; printf("  [cb] msg\n"); return 0; }
static int cb_key(int a, int b, int c, int d) { printf("  [cb] key %d %d %d %d\n", a, b, c, d); return 1; }
static int cb_mouse(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return 1; }

static void dump_prop(uint32_t wid) {
    uint32_t prop[16];
    memset(prop, 0, sizeof prop);
    if (!p_HOA_GX_GetWindowProperty) return;
    int r = p_HOA_GX_GetWindowProperty(wid, prop);
    printf("  window %u property rc=%d:", wid, r);
    for (int i = 0; i < 12; i++) printf(" %d", (int)prop[i]);
    printf("\n");
}

static GLuint shader(GLenum t, const char *src) {
    GLuint s = p_glCreateShader(t);
    p_glShaderSource(s, 1, &src, 0);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; p_glGetShaderInfoLog(s, sizeof log, 0, log); printf("  shader error: %s\n", log); }
    return s;
}

int main(int argc, char **argv) {
    const char *service = "lg.service.normal.nctest";
    long force_wid = -1;
    int noreg = 0, secs = 12;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--service") && i + 1 < argc) service = argv[++i];
        else if (!strcmp(argv[i], "--wid") && i + 1 < argc) force_wid = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--noreg")) noreg = 1;
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    g_log = dup(1);
    if (g_log < 0) g_log = 1;
    guard_signals();
    alarm(secs + 28); /* hard stop no matter what */

    printf("[1] loading LG libraries\n");
    int G = RTLD_LAZY | RTLD_GLOBAL;
    /* Order matters: dbgfrwk must be initialised before anything else touches it,
     * and libafcommon defines 'gendprog', which libscf/libafopenapi need. */
    const char *pre[] = {"libdbus-1.so.3", "libsqlite3.so.0", "libexpat.so.1", "libstdc++.so.6"};
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) must_open(pre[i], G);
    void *dbg = must_open("libdbgfrwk.so", G);
    if (dbg) {
        int (*dbgfrwk_init)(int, int) = (int (*)(int, int))dlsym(dbg, "dbgfrwk_init");
        if (dbgfrwk_init) printf("  dbgfrwk_init rc=%d\n", dbgfrwk_init(1, 0));
        guard_signals();
    }
    const char *post[] = {"libafcommon.so", "libscf.so"};
    for (unsigned i = 0; i < sizeof post / sizeof *post; i++) { must_open(post[i], G); guard_signals(); }
    void *af = must_open("libafopenapi.so", G); guard_signals();
    void *egl = must_open("libEGL.so", G); guard_signals();
    void *gl = must_open("libGLESv2.so", G); guard_signals();
    if (!af || !egl || !gl) { printf("  required library missing, abort\n"); _exit(2); }
    SYM(af, HOA_PROC_RegisterService); SYM(af, HOA_GX_GetWindowID); SYM(af, HOA_GX_GetWindowProperty);
    SYM(af, HOA_GX_SetWindowProperty); SYM(af, HOA_GX_GetVisibleWindowList); SYM(af, HOA_GX_UpdateWindow);
    SYM(af, HOA_GX_RaiseWindow);
    SYM(egl, eglGetDisplay); SYM(egl, eglInitialize); SYM(egl, eglChooseConfig); SYM(egl, eglCreateWindowSurface);
    SYM(egl, eglCreateContext); SYM(egl, eglMakeCurrent); SYM(egl, eglSwapBuffers); SYM(egl, eglQuerySurface);
    SYM(egl, eglDestroySurface); SYM(egl, eglDestroyContext); SYM(egl, eglTerminate); SYM(egl, eglGetError);
    SYM(egl, eglQueryString);
    SYM(gl, glClearColor); SYM(gl, glClear); SYM(gl, glViewport); SYM(gl, glCreateShader); SYM(gl, glShaderSource);
    SYM(gl, glCompileShader); SYM(gl, glGetShaderiv); SYM(gl, glGetShaderInfoLog); SYM(gl, glCreateProgram);
    SYM(gl, glAttachShader); SYM(gl, glBindAttribLocation); SYM(gl, glLinkProgram); SYM(gl, glGetProgramiv);
    SYM(gl, glUseProgram); SYM(gl, glGetProgramInfoLog); SYM(gl, glGetUniformLocation); SYM(gl, glUniform1f); SYM(gl, glVertexAttribPointer);
    SYM(gl, glEnableVertexAttribArray); SYM(gl, glDrawArrays); SYM(gl, glGetString);
    guard_signals(); /* library constructors may have installed LG crash hooks: override them */

    uint32_t wid = 0;
    if (!noreg) {
        printf("[2] HOA_PROC_RegisterService(--service %s)\n", service);
        char *rargv[] = {argv[0], "--service", (char *)service, 0};
        hoa_cb_t cb = {cb_msg, cb_key, cb_mouse};
        int r = p_HOA_PROC_RegisterService(3, rargv, &cb);
        guard_signals();
        printf("  rc=%d\n", r);
        if (r != 0) _exit(3);
    }

    if (p_HOA_GX_GetVisibleWindowList) {
        uint32_t *list = 0, n = 0;
        int r = p_HOA_GX_GetVisibleWindowList(&list, &n);
        printf("[3] visible windows rc=%d n=%u:", r, n);
        for (uint32_t i = 0; r == 0 && list && i < n; i++) printf(" %u", list[i]);
        printf("\n");
        for (uint32_t i = 0; r == 0 && list && i < n; i++) dump_prop(list[i]);
    }

    if (force_wid >= 0) {
        wid = (uint32_t)force_wid;
        printf("[4] using forced window id %u\n", wid);
    } else {
        int r = p_HOA_GX_GetWindowID(&wid);
        printf("[4] HOA_GX_GetWindowID rc=%d wid=%u\n", r, wid);
        if (r != 0) _exit(4);
    }
    dump_prop(wid);

    printf("[5] EGL init\n");
    EGLDisplay dpy = p_eglGetDisplay(0);
    EGLint maj = 0, min = 0;
    if (!p_eglInitialize(dpy, &maj, &min)) { printf("  eglInitialize failed 0x%x\n", p_eglGetError()); return 5; }
    printf("  EGL %d.%d vendor=%s version=%s\n", maj, min, p_eglQueryString(dpy, EGL_VENDOR), p_eglQueryString(dpy, EGL_VERSION));
    EGLint cattr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint ncfg = 0;
    if (!p_eglChooseConfig(dpy, cattr, &cfg, 1, &ncfg) || ncfg < 1) { printf("  no config 0x%x\n", p_eglGetError()); return 6; }
    EGLSurface surf = p_eglCreateWindowSurface(dpy, cfg, (void *)(uintptr_t)wid, 0);
    if (!surf) { printf("  eglCreateWindowSurface(wid=%u) failed 0x%x\n", wid, p_eglGetError()); p_eglTerminate(dpy); return 7; }
    EGLint ctxattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = p_eglCreateContext(dpy, cfg, 0, ctxattr);
    if (!ctx || !p_eglMakeCurrent(dpy, surf, surf, ctx)) { printf("  context failed 0x%x\n", p_eglGetError()); return 8; }
    EGLint w = 0, h = 0;
    p_eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    p_eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    printf("  surface %dx%d  GL_RENDERER=%s  GL_VERSION=%s\n", w, h, p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));

    /* make our window visible (gxtest "window on": property word 1 = bVisible) */
    if (p_HOA_GX_GetWindowProperty && p_HOA_GX_SetWindowProperty) {
        uint32_t prop[16] = {0};
        if (p_HOA_GX_GetWindowProperty(wid, prop) == 0) {
            prop[1] = 1;
            printf("[6] SetWindowProperty(visible) rc=%d\n", p_HOA_GX_SetWindowProperty(wid, prop));
        }
    }
    if (p_HOA_GX_RaiseWindow) printf("  RaiseWindow rc=%d\n", p_HOA_GX_RaiseWindow(wid));

    const char *vs = "attribute vec2 pos; uniform float t;\n"
                     "void main(){ float c=cos(t), s=sin(t);\n"
                     "  gl_Position = vec4(c*pos.x - s*pos.y, s*pos.x + c*pos.y, 0.0, 1.0); }\n";
    /* separate uniform: GLSL ES forbids one uniform with different precisions across stages */
    const char *fs = "precision mediump float; uniform float tf;\n"
                     "void main(){ gl_FragColor = vec4(0.5+0.5*sin(tf*3.0), 0.8, 0.5+0.5*cos(tf*2.0), 1.0); }\n";
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
    p_glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
    p_glBindAttribLocation(prog, 0, "pos");
    p_glLinkProgram(prog);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    printf("[7] shader program linked=%d, rendering %d s\n", linked, secs);
    if (!linked) { char log[512] = ""; p_glGetProgramInfoLog(prog, sizeof log, 0, log); printf("  link log: %s\n", log); }
    p_glUseProgram(prog);
    GLint ut = p_glGetUniformLocation(prog, "t");
    GLint utf = p_glGetUniformLocation(prog, "tf");
    static const GLfloat tri[] = {0.0f, 0.6f, -0.52f, -0.3f, 0.52f, -0.3f};
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, tri);
    p_glEnableVertexAttribArray(0);
    p_glViewport(0, 0, w, h);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int frames = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        float t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9f;
        if (t > secs) break;
        p_glClearColor(0.1f + 0.1f * sinf(t), 0.1f, 0.3f + 0.2f * cosf(t * 0.7f), 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glUniform1f(ut, t);
        p_glUniform1f(utf, t);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!p_eglSwapBuffers(dpy, surf)) { printf("  swap failed 0x%x\n", p_eglGetError()); break; }
        if (frames == 0 && p_HOA_GX_UpdateWindow) printf("  first frame; UpdateWindow rc=%d\n", p_HOA_GX_UpdateWindow(wid, 0));
        frames++;
    }
    printf("[8] %d frames in %d s (%.1f fps)\n", frames, secs, frames / (float)secs);

    /* hide window again */
    if (p_HOA_GX_GetWindowProperty && p_HOA_GX_SetWindowProperty) {
        uint32_t prop[16] = {0};
        if (p_HOA_GX_GetWindowProperty(wid, prop) == 0) { prop[1] = 0; p_HOA_GX_SetWindowProperty(wid, prop); }
    }
    p_eglMakeCurrent(dpy, 0, 0, 0);
    p_eglDestroyContext(dpy, ctx);
    p_eglDestroySurface(dpy, surf);
    p_eglTerminate(dpy);
    printf("[9] done\n");
    _exit(0); /* skip LG library destructors */
}
