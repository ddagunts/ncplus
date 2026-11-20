/*
 * libnc: minimal native app runtime for LG NetCast 4.x TVs (MT5398 / PowerVR SGX).
 *
 * Wraps the reverse-engineered LG HOA app-framework recipe:
 *   register with procmgr -> get assigned window -> EGL/GLES2 surface on it
 *   -> input from HOA callbacks and /dev/input/event* (remote, USB keyboard/mouse, gamepad).
 *
 * Run apps through apps/run-app.sh, which sets the LG app environment (PM_EXECARGS etc).
 * Single global instance; not thread-safe except where noted.
 */
#ifndef NC_H
#define NC_H

#include <stdint.h>
#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *service;   /* procmgr service name; default from PM_EXECARGS or "lg.service.normal.ncapp" */
    int timeout_s;         /* hard kill after N seconds (0 = none); safety net while developing */
    int no_egl;            /* 1 = don't create the EGL surface/context */
    int no_evdev;          /* 1 = don't read /dev/input/event* directly */
    int verbose;           /* 1 = log every event */
    int width, height;     /* if set: HOA_GX_SetWindowResolution(wid, width, height) before EGL setup */
    int no_window;         /* 1 = only register with procmgr (CLI tools): no window, EGL or evdev */
} nc_config;

typedef enum {
    NC_EV_NONE = 0,
    NC_EV_KEY,          /* evdev EV_KEY: code = KEY_* / BTN_*, value 1 press, 0 release, 2 repeat */
    NC_EV_REL,          /* evdev EV_REL: code = REL_X/REL_Y/REL_WHEEL..., value = delta */
    NC_EV_ABS,          /* evdev EV_ABS: code = ABS_*, value = position */
    NC_EV_HOA_KEY,      /* procmgr key callback: raw[] = callback args */
    NC_EV_HOA_MOUSE,    /* procmgr mouse callback: raw[] = callback args */
    NC_EV_HOA_MSG,      /* procmgr message callback: raw[] = first words of the message */
    NC_EV_DEVICE_ADDED, /* an input device appeared: dev, name */
    NC_EV_DEVICE_REMOVED,
    NC_EV_QUIT          /* SIGTERM/SIGINT/SIGHUP or timeout */
} nc_event_type;

typedef struct {
    nc_event_type type;
    int dev;            /* /dev/input/eventN number, or -1 */
    int code;
    int value;
    uint32_t raw[6];
    char name[64];      /* device name for evdev events */
} nc_event;

int  nc_init(const nc_config *cfg);          /* 0 on success; logs reasons on failure */
int  nc_poll(nc_event *ev, int timeout_ms);  /* 1 = event filled, 0 = timeout */
int  nc_quit_requested(void);
void nc_swap(void);                           /* eglSwapBuffers (+ first-frame window update) */
void nc_update_window(void);                  /* HOA_GX_UpdateWindow (done by nc_swap on frame 1) */
void nc_set_visible(int visible);
void nc_shutdown(void);                       /* hide window, tear down EGL, unregister */
void nc_exit(int code);                       /* nc_shutdown + _exit (skips LG destructors) */

int        nc_width(void);
int        nc_height(void);
uint32_t   nc_window_id(void);
EGLDisplay nc_egl_display(void);
EGLSurface nc_egl_surface(void);
EGLContext nc_egl_context(void);

/* Look up any symbol in LG's libafopenapi.so (HOA_*), for experiments. Valid after nc_init(). */
void *nc_hoa_sym(const char *name);

/* 1 = gamepads (xpad/xpadone layout, PS3) also produce navigation keys: d-pad/left stick -> arrows
 * (with auto-repeat), A/Start (PS3 Cross/Start) -> KEY_ENTER, B (Circle) -> KEY_ESC, LB/RB (L1/R1) ->
 * KEY_PAGEUP/KEY_PAGEDOWN. Raw pad events are still delivered. Off by default (SDL apps use SDL joysticks). */
void nc_pad_keys(int on);

/* TV audio mute through LG's HOA_CTRL_GetMute/SetMute (valid after nc_init, also with no_window).
 * nc_get_mute: 1 muted, 0 not, -1 unavailable. nc_set_mute: 0 ok, -1 failed. RAM-only. */
int  nc_get_mute(void);
int  nc_set_mute(int on);

void nc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *nc_key_name(int code);            /* evdev key code -> "KEY_UP" etc. */

#ifdef __cplusplus
}
#endif
#endif
