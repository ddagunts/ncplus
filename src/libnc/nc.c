/*
 * libnc implementation. See nc.h and FINDINGS.md ("Native GUI recipe").
 *
 * LG's HOA libraries are dlopen'ed (they need libdbgfrwk initialised first and
 * a specific load order); EGL/GLES2 are linked normally against the TV's libs.
 */
#define _GNU_SOURCE
#include "nc.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#include "keynames.h"

/* ------------------------------------------------------------------ log */
static int g_log = 2;

void nc_log(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    if (n < 0) return;
    if (n == 0 || b[n - 1] != '\n') b[n++] = '\n';
    (void)!write(g_log, b, n);
}

const char *nc_key_name(int code) {
    for (unsigned i = 0; i < sizeof nc_keynames / sizeof *nc_keynames; i++)
        if (nc_keynames[i].code == code) return nc_keynames[i].name;
    return "KEY_?";
}

/* --------------------------------------------------------- HOA bindings */
typedef struct {
    int (*msg)(void *);
    int (*key)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    int (*mouse)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
} hoa_cb_t;

static int (*hoa_register)(int, char **, hoa_cb_t *);
static int (*hoa_unregister)(void);
static int (*hoa_set_ready)(void);
static int (*hoa_get_window_id)(uint32_t *);
static int (*hoa_get_prop)(uint32_t, void *);
static int (*hoa_set_prop)(uint32_t, void *);
static int (*hoa_raise)(uint32_t);
static int (*hoa_update)(uint32_t, void *);
static int (*hoa_get_dispatch)(uint32_t, uint32_t *);
static int (*hoa_get_res)(uint32_t, uint32_t *, uint32_t *);
static int (*hoa_set_res)(uint32_t, uint32_t, uint32_t);
static void *g_af;
void *nc_hoa_sym(const char *name) { return g_af ? dlsym(g_af, name) : 0; }

/* ------------------------------------------------------------- state */
#define MAXDEV 32
#define QLEN 256

static struct {
    int inited;
    nc_config cfg;
    uint32_t wid;
    int registered;
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    int w, h;
    int frames;
    int wake[2];                 /* self-pipe: callbacks/signals wake nc_poll */
    volatile sig_atomic_t quit;
    pthread_mutex_t qlock;
    nc_event q[QLEN];
    int qhead, qtail;
    int devfd[MAXDEV];
    char devname[MAXDEV][64];
    time_t last_scan;
} S = {.wake = {-1, -1}, .qlock = PTHREAD_MUTEX_INITIALIZER};

static void q_push(const nc_event *e) {
    pthread_mutex_lock(&S.qlock);
    int next = (S.qtail + 1) % QLEN;
    if (next != S.qhead) { S.q[S.qtail] = *e; S.qtail = next; }
    pthread_mutex_unlock(&S.qlock);
    if (S.wake[1] >= 0) (void)!write(S.wake[1], "x", 1);
}
static int q_pop(nc_event *e) {
    int ok = 0;
    pthread_mutex_lock(&S.qlock);
    if (S.qhead != S.qtail) { *e = S.q[S.qhead]; S.qhead = (S.qhead + 1) % QLEN; ok = 1; }
    pthread_mutex_unlock(&S.qlock);
    return ok;
}

/* -------------------------------------------------------- callbacks */
static int cb_msg(void *m) {
    /* The argument's type is unknown (may not be a pointer): record its value only. */
    nc_event e = {.type = NC_EV_HOA_MSG, .dev = -1, .raw = {(uint32_t)(uintptr_t)m}};
    q_push(&e);
    return 0;
}
static int cb_key(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t y) {
    nc_event e = {.type = NC_EV_HOA_KEY, .dev = -1, .code = (int)a, .value = (int)b, .raw = {a, b, c, d, x, y}};
    q_push(&e);
    return 1;
}
static int cb_mouse(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t y) {
    nc_event e = {.type = NC_EV_HOA_MOUSE, .dev = -1, .code = (int)a, .value = (int)b, .raw = {a, b, c, d, x, y}};
    q_push(&e);
    return 1;
}

/* ------------------------------------------------------------ signals */
static void on_crash(int s, siginfo_t *si, void *ucv) {
    char b[160];
    ucontext_t *uc = ucv;
    int n = snprintf(b, sizeof b, "libnc: fatal signal %d (tid %ld) addr=%p pc=%08lx lr=%08lx, exiting\n", s,
                     (long)syscall(SYS_gettid), si ? si->si_addr : 0,
                     uc ? (unsigned long)uc->uc_mcontext.arm_pc : 0, uc ? (unsigned long)uc->uc_mcontext.arm_lr : 0);
    (void)!write(g_log, b, n);
    /* resolve pc/lr to library+offset (best effort, not async-signal-safe) */
    if (uc) {
        unsigned long addrs[2] = {(unsigned long)uc->uc_mcontext.arm_pc, (unsigned long)uc->uc_mcontext.arm_lr};
        int fd = open("/proc/self/maps", O_RDONLY);
        static char maps[65536];
        int len = fd >= 0 ? (int)read(fd, maps, sizeof maps - 1) : 0;
        if (fd >= 0) close(fd);
        if (len > 0) {
            maps[len] = 0;
            for (int k = 0; k < 2; k++) {
                for (char *line = maps; line && *line; ) {
                    char *nl = strchr(line, '\n');
                    unsigned long lo, hi, off;
                    char path[256] = "";
                    if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %255s", &lo, &hi, &off, path) >= 3 && addrs[k] >= lo && addrs[k] < hi) {
                        n = snprintf(b, sizeof b, "libnc:   %s %08lx = %s +0x%lx\n", k ? "lr" : "pc", addrs[k], path[0] ? path : "?", addrs[k] - lo + off);
                        (void)!write(g_log, b, n);
                        break;
                    }
                    line = nl ? nl + 1 : NULL;
                }
            }
        }
    }
    _exit(100 + s);
}
static void on_term(int s) {
    (void)s;
    S.quit = 1;
    if (S.wake[1] >= 0) (void)!write(S.wake[1], "q", 1);
}
static void on_alarm(int s) {
    (void)s;
    const char m[] = "libnc: timeout reached, exiting\n";
    (void)!write(g_log, m, sizeof m - 1);
    _exit(124);
}
/* LG's libdbgfrwk installs its own crash handlers (which report to the LG crash
 * framework). Re-install ours after every LG library load. */
static void guard_signals(void) {
    int crash[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction sa;
    for (unsigned i = 0; i < sizeof crash / sizeof *crash; i++) {
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_crash;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(crash[i], &sa, 0);
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);
}

static void *lg_open(const char *n) {
    void *h = dlopen(n, RTLD_LAZY | RTLD_GLOBAL);
    if (!h) nc_log("libnc: dlopen %s: %s", n, dlerror());
    guard_signals();
    return h;
}

/* -------------------------------------------------------------- evdev */
static void evdev_scan(void) {
    S.last_scan = time(0);
    for (int i = 0; i < MAXDEV; i++) {
        if (S.devfd[i] >= 0) continue;
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[64] = "?";
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) < 0) {
            /* node exists but no device behind it */
            close(fd);
            continue;
        }
        S.devfd[i] = fd;
        snprintf(S.devname[i], sizeof S.devname[i], "%s", name);
        nc_event e = {.type = NC_EV_DEVICE_ADDED, .dev = i};
        snprintf(e.name, sizeof e.name, "%s", name);
        q_push(&e);
    }
}

/* --------------------------------------------------------------- volume
 * While our app has focus LG doesn't act on Vol+/Vol-/Mute (they reach us on "LGE RCU"), so we call LG's
 * own volume API, the way LG's Magic Remote gesture UI does (UI_GESTURE_CtrlSetVoluem in RELEASE):
 *   HOA_CTRL_SetVolume(0 = relative, 2 = volume type, 1, delta, &new_volume)
 * LG stores the level itself, as it does whenever the volume is changed. NC_NO_VOLUME=1 disables this. */
static int (*hoa_set_volume)(int, int, int, int, uint32_t *);   /* LG writes one byte */
static int (*hoa_get_mute)(uint32_t *);                         /* size unknown: word buffer */
static int (*hoa_set_mute)(int, int);
static int vol_state;   /* 0 = not loaded yet, 1 = ok, -1 = unavailable */

static int volume_api(void) {
    if (vol_state) return vol_state > 0;
    vol_state = -1;
    if (getenv("NC_NO_VOLUME") || !g_af) return 0;
    void *bc = lg_open("libbcopenapi.so");
    if (!bc) return 0;
    hoa_set_volume = (int (*)(int, int, int, int, uint32_t *))dlsym(bc, "HOA_CTRL_SetVolume");
    hoa_get_mute = (int (*)(uint32_t *))dlsym(bc, "HOA_CTRL_GetMute");
    hoa_set_mute = (int (*)(int, int))dlsym(bc, "HOA_CTRL_SetMute");
    if (hoa_set_volume) vol_state = 1;
    return vol_state > 0;
}

static void volume_key(int code) {
    if (!volume_api()) return;
    if (code == KEY_MUTE) {
        uint32_t m = 0;
        int r = -1;
        if (hoa_get_mute && hoa_set_mute && hoa_get_mute(&m) == 0) r = hoa_set_mute((m & 0xff) ? 0 : 1, 1);
        nc_log("libnc: mute %s (rc=%d)", (m & 0xff) ? "off" : "on", r);
        return;
    }
    uint32_t v = 0;
    int r = hoa_set_volume(0, 2, 1, code == KEY_VOLUMEUP ? 1 : -1, &v);
    nc_log("libnc: volume %s -> %u (rc=%d)", code == KEY_VOLUMEUP ? "up" : "down", v & 0xff, r);
}

static void evdev_read(int i) {
    struct input_event iev[32];
    for (;;) {
        ssize_t n = read(S.devfd[i], iev, sizeof iev);
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {   /* ENODEV: unplugged */
            nc_event e = {.type = NC_EV_DEVICE_REMOVED, .dev = i};
            snprintf(e.name, sizeof e.name, "%s", S.devname[i]);
            close(S.devfd[i]);
            S.devfd[i] = -1;
            q_push(&e);
            return;
        }
        for (int k = 0; k < (int)(n / sizeof *iev); k++) {
            nc_event e = {.dev = i, .code = iev[k].code, .value = iev[k].value};
            switch (iev[k].type) {
            case EV_KEY:
                e.type = NC_EV_KEY;
                if (e.code == KEY_PREVIOUS) e.code = KEY_ESC;            /* remote Back (412) = Escape */
                if ((e.code == KEY_VOLUMEUP || e.code == KEY_VOLUMEDOWN) && e.value) volume_key(e.code);
                if (e.code == KEY_MUTE && e.value == 1) volume_key(e.code);
                break;
            case EV_REL: e.type = NC_EV_REL; break;
            case EV_ABS: e.type = NC_EV_ABS; break;
            default: continue;   /* EV_SYN, EV_MSC, ... */
            }
            snprintf(e.name, sizeof e.name, "%s", S.devname[i]);
            q_push(&e);
        }
    }
}

/* --------------------------------------------------------------- init */
static int window_visible(uint32_t wid, int vis) {
    uint32_t prop[16] = {0};
    if (!hoa_get_prop || !hoa_set_prop || hoa_get_prop(wid, prop) != 0) return -1;
    prop[1] = vis ? 1 : 0;   /* word 1 = bVisible (from gxtest) */
    return hoa_set_prop(wid, prop);
}

int nc_init(const nc_config *cfg) {
    if (S.inited) return 0;
    if (cfg) S.cfg = *cfg;
    g_log = dup(2);              /* libdbgfrwk hijacks stdout/stderr later */
    if (g_log < 0) g_log = 2;
    int saved_out = dup(1), saved_err = dup(2);   /* restored at the end of init */
    for (int i = 0; i < MAXDEV; i++) S.devfd[i] = -1;
    if (pipe(S.wake) == 0) {
        fcntl(S.wake[0], F_SETFL, O_NONBLOCK);
        fcntl(S.wake[1], F_SETFL, O_NONBLOCK);
    }
    guard_signals();
    if (S.cfg.timeout_s > 0) alarm(S.cfg.timeout_s);

    /* LG app framework, in dependency order */
    const char *pre[] = {"libdbus-1.so.3", "libsqlite3.so.0", "libexpat.so.1", "libstdc++.so.6"};
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) lg_open(pre[i]);
    void *dbg = lg_open("libdbgfrwk.so");
    if (!dbg) return -1;
    int (*dbg_init)(int, int) = (int (*)(int, int))dlsym(dbg, "dbgfrwk_init");
    if (dbg_init) dbg_init(1, 0);
    guard_signals();
    lg_open("libafcommon.so");   /* defines 'gendprog' needed by libscf */
    lg_open("libscf.so");
    void *af = lg_open("libafopenapi.so");
    if (!af) return -1;
    g_af = af;

#define B(var, name) (var = (__typeof__(var))dlsym(af, name))
    B(hoa_register, "HOA_PROC_RegisterService");
    B(hoa_unregister, "HOA_PROC_UnregisterService");
    B(hoa_set_ready, "HOA_PROC_SetReady");
    B(hoa_get_window_id, "HOA_GX_GetWindowID");
    B(hoa_get_prop, "HOA_GX_GetWindowProperty");
    B(hoa_set_prop, "HOA_GX_SetWindowProperty");
    B(hoa_raise, "HOA_GX_RaiseWindow");
    B(hoa_update, "HOA_GX_UpdateWindow");
    B(hoa_get_dispatch, "HOA_GX_GetWindowInputDispatchType");
    B(hoa_get_res, "HOA_GX_GetWindowResolution");
    B(hoa_set_res, "HOA_GX_SetWindowResolution");
#undef B
    if (!hoa_register || !hoa_get_window_id) { nc_log("libnc: HOA symbols missing"); return -1; }

    /* HOA_PROC_RegisterService parses PM_EXECARGS instead of argv when it's set. */
    const char *svc = S.cfg.service;
    char envsvc[128] = "";
    const char *pa = getenv("PM_EXECARGS");       /* run-app.sh puts "--service NAME" here */
    if (!svc && pa && (pa = strstr(pa, "--service ")) && sscanf(pa + 10, "%127s", envsvc) == 1) svc = envsvc;
    if (!svc) svc = "lg.service.normal.ncapp";
    char svcbuf[128];
    snprintf(svcbuf, sizeof svcbuf, "%s", svc);
    if (!getenv("PM_EXECARGS")) {
        char args[160];
        snprintf(args, sizeof args, "--service %s", svc);
        setenv("PM_EXECARGS", args, 1);
    }
    char *rargv[] = {(char *)"ncapp", (char *)"--service", svcbuf, 0};
    hoa_cb_t cb = {cb_msg, cb_key, cb_mouse};
    /* HOA_PROC_RegisterService uses getopt_long(): reset glibc's getopt state, which the app may
     * already have consumed (e.g. RetroArch), or LG's parser walks past our argv and crashes. */
    int saved_optind = optind, saved_opterr = opterr;
    optind = 0;   /* glibc: 0 = full reinitialisation */
    opterr = 0;
    int r = hoa_register(3, rargv, &cb);
    optind = saved_optind;
    opterr = saved_opterr;
    guard_signals();
    if (r != 0) { nc_log("libnc: HOA_PROC_RegisterService failed (%d); launched via run-app.sh?", r); return -1; }
    S.registered = 1;
    nc_log("libnc: registered (%s)", getenv("PM_EXECARGS"));
    if (getenv("NC_SET_READY") && hoa_set_ready) nc_log("libnc: SetReady rc=%d", hoa_set_ready());

    if (S.cfg.no_window) {
        S.cfg.no_egl = 1;
        S.cfg.no_evdev = 1;
        S.inited = 1;
        if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, 2); close(saved_err); }
        return 0;
    }
    r = hoa_get_window_id(&S.wid);
    if (r != 0) { nc_log("libnc: HOA_GX_GetWindowID failed (%d)", r); return -1; }
    if (hoa_get_dispatch) {
        uint32_t dt = 0xffffffff;
        int rr = hoa_get_dispatch(S.wid, &dt);
        nc_log("libnc: window %u, input dispatch type rc=%d type=%d", S.wid, rr, (int)dt);
    }

    if (hoa_get_res) {
        uint32_t w = 0, h = 0;
        int rr = hoa_get_res(S.wid, &w, &h);
        nc_log("libnc: window resolution rc=%d %ux%u", rr, w, h);
    }
    if (S.cfg.width > 0 && S.cfg.height > 0 && hoa_set_res) {
        int rr = hoa_set_res(S.wid, (uint32_t)S.cfg.width, (uint32_t)S.cfg.height);
        nc_log("libnc: SetWindowResolution(%d,%d) rc=%d", S.cfg.width, S.cfg.height, rr);
    }

#ifndef NC_NO_EGL
    if (!S.cfg.no_egl) {
        S.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint maj, min;
        if (!eglInitialize(S.dpy, &maj, &min)) { nc_log("libnc: eglInitialize 0x%x", eglGetError()); return -1; }
        EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig c;
        EGLint nc = 0;
        if (!eglChooseConfig(S.dpy, ca, &c, 1, &nc) || nc < 1) { nc_log("libnc: no EGL config"); return -1; }
        /* EGL native window on NetCast 4.0 = HOA window ID */
        S.surf = eglCreateWindowSurface(S.dpy, c, (EGLNativeWindowType)S.wid, 0);
        if (S.surf == EGL_NO_SURFACE) { nc_log("libnc: eglCreateWindowSurface 0x%x", eglGetError()); return -1; }
        EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        S.ctx = eglCreateContext(S.dpy, c, EGL_NO_CONTEXT, xa);
        if (S.ctx == EGL_NO_CONTEXT || !eglMakeCurrent(S.dpy, S.surf, S.surf, S.ctx)) {
            nc_log("libnc: EGL context 0x%x", eglGetError());
            return -1;
        }
        eglQuerySurface(S.dpy, S.surf, EGL_WIDTH, &S.w);
        eglQuerySurface(S.dpy, S.surf, EGL_HEIGHT, &S.h);
        nc_log("libnc: EGL %d.%d surface %dx%d", maj, min, S.w, S.h);
    }

#endif
    window_visible(S.wid, 1);
    if (hoa_raise) hoa_raise(S.wid);
    if (!S.cfg.no_evdev) evdev_scan();
    /* undo libdbgfrwk's stdout/stderr redirection so apps' printf/SDL_Log work */
    if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
    if (saved_err >= 0) { dup2(saved_err, 2); close(saved_err); }
    nc_log("libnc: init done");
    S.inited = 1;
    return 0;
}

/* --------------------------------------------------------------- loop */
int nc_poll(nc_event *ev, int timeout_ms) {
    if (q_pop(ev)) return 1;
    if (S.quit) { memset(ev, 0, sizeof *ev); ev->type = NC_EV_QUIT; ev->dev = -1; return 1; }
    if (!S.cfg.no_evdev && time(0) - S.last_scan >= 2) evdev_scan();   /* hotplug (USB kbd/mouse) */

    struct pollfd p[MAXDEV + 1];
    int map[MAXDEV + 1], n = 0;
    p[n].fd = S.wake[0]; p[n].events = POLLIN; map[n++] = -1;
    for (int i = 0; i < MAXDEV; i++)
        if (S.devfd[i] >= 0) { p[n].fd = S.devfd[i]; p[n].events = POLLIN; map[n++] = i; }
    if (poll(p, n, timeout_ms) > 0) {
        for (int k = 0; k < n; k++) {
            if (!(p[k].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            if (map[k] < 0) { char b[64]; while (read(S.wake[0], b, sizeof b) > 0) {} }
            else evdev_read(map[k]);
        }
    }
    if (q_pop(ev)) return 1;
    if (S.quit) { memset(ev, 0, sizeof *ev); ev->type = NC_EV_QUIT; ev->dev = -1; return 1; }
    return 0;
}

int nc_quit_requested(void) { return S.quit; }

void nc_update_window(void) {
    if (hoa_update) hoa_update(S.wid, 0);
}

void nc_swap(void) {
#ifndef NC_NO_EGL
    if (S.surf == EGL_NO_SURFACE) return;
    eglSwapBuffers(S.dpy, S.surf);
#endif
    if (S.frames++ == 0) nc_update_window();
}

void nc_set_visible(int v) {
    window_visible(S.wid, v);
    if (v && hoa_raise) hoa_raise(S.wid);
}

void nc_shutdown(void) {
    if (!S.inited && !S.registered) return;
    if (!S.cfg.no_window) window_visible(S.wid, 0);
#ifndef NC_NO_EGL
    if (S.dpy != EGL_NO_DISPLAY && S.dpy) {
        eglMakeCurrent(S.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (S.ctx) eglDestroyContext(S.dpy, S.ctx);
        if (S.surf) eglDestroySurface(S.dpy, S.surf);
        eglTerminate(S.dpy);
    }
#endif
    for (int i = 0; i < MAXDEV; i++) if (S.devfd[i] >= 0) close(S.devfd[i]);
    if (S.registered && hoa_unregister) {
        int r = hoa_unregister();
        nc_log("libnc: unregister rc=%d", r);
    }
    S.inited = S.registered = 0;
}

void nc_exit(int code) {
    nc_shutdown();
    _exit(code);   /* skip LG library destructors */
}

int nc_width(void) { return S.w; }
int nc_height(void) { return S.h; }
uint32_t nc_window_id(void) { return S.wid; }
EGLDisplay nc_egl_display(void) { return S.dpy; }
EGLSurface nc_egl_surface(void) { return S.surf; }
EGLContext nc_egl_context(void) { return S.ctx; }
