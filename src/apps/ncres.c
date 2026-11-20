/*
 * ncres: resolution probe. Prints what LG's window manager and the framebuffer driver
 * report, optionally requests a window resolution, then draws a pixel-exact test pattern:
 *   left:  1-px alternating vertical lines   right: 1-px alternating horizontal lines
 *   any scaling shows up as uneven stripes / moire.
 * Usage: run-app.sh ncres [seconds] [WxH]      e.g. ncres 20 1920x1080
 */
#include "nc.h"
#include "ncgfx.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void dump(const char *what, int rc, const void *p, int nwords) {
    const uint32_t *w = p;
    char b[512];
    int n = snprintf(b, sizeof b, "%s rc=%d:", what, rc);
    for (int i = 0; i < nwords && n < (int)sizeof b - 12; i++) n += snprintf(b + n, sizeof b - n, " %08x", w[i]);
    nc_log("%s", b);
}

static void fbinfo(int i) {
    char path[32];
    snprintf(path, sizeof path, "/dev/fb/%d", i);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    memset(&v, 0, sizeof v);
    memset(&f, 0, sizeof f);
    int r1 = ioctl(fd, FBIOGET_VSCREENINFO, &v), r2 = ioctl(fd, FBIOGET_FSCREENINFO, &f);
    nc_log("fb%d: var rc=%d %ux%u virt %ux%u bpp %u | fix rc=%d id=\"%.16s\" line %u smem %u", i, r1, v.xres, v.yres,
           v.xres_virtual, v.yres_virtual, v.bits_per_pixel, r2, f.id, f.line_length, f.smem_len);
    close(fd);
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 20, rw = 0, rh = 0;
    if (argc > 2) sscanf(argv[2], "%dx%d", &rw, &rh);
    nc_config cfg = {.timeout_s = secs + 15, .no_evdev = 1, .width = rw, .height = rh};
    if (nc_init(&cfg) != 0) nc_exit(2);
    uint32_t wid = nc_window_id();

    int (*get_res)(uint32_t, uint32_t *, uint32_t *) = nc_hoa_sym("HOA_GX_GetWindowResolution");
    int (*get_max)(uint32_t, uint32_t *, uint32_t *) = nc_hoa_sym("HOA_GX_GetWindowMaxResolution");
    int (*get_rect)(uint32_t, void *, void *) = nc_hoa_sym("HOA_GX_GetWindowViewAndDispRect");
    int (*get_cap)(void *) = nc_hoa_sym("HOA_GX_GetDeviceCapability");
    int (*get_prop)(uint32_t, void *) = nc_hoa_sym("HOA_GX_GetWindowProperty");
    uint32_t a[16], b[16];
    if (get_res) { memset(a, 0, sizeof a); memset(b, 0, sizeof b); dump("GetWindowResolution w", get_res(wid, a, b), a, 1); dump("  h", 0, b, 1); }
    if (get_max) { memset(a, 0, sizeof a); memset(b, 0, sizeof b); dump("GetWindowMaxResolution w", get_max(wid, a, b), a, 1); dump("  h", 0, b, 1); }
    if (get_rect) { memset(a, 0, sizeof a); memset(b, 0, sizeof b); int r = get_rect(wid, a, b); dump("ViewRect", r, a, 4); dump("DispRect", r, b, 4); }
    if (get_cap) { memset(a, 0, sizeof a); dump("DeviceCapability", get_cap(a), a, 16); }
    if (get_prop) { memset(a, 0, sizeof a); dump("WindowProperty", get_prop(wid, a), a, 12); }
    for (int i = 0; i < 4; i++) fbinfo(i);
    nc_log("EGL surface %dx%d", nc_width(), nc_height());

    ncgfx_init();
    int W = nc_width(), H = nc_height();
    time_t end = time(0) + secs;
    while (time(0) < end && !nc_quit_requested()) {
        nc_event e;
        while (nc_poll(&e, 0)) if (e.type == NC_EV_QUIT) end = 0;
        ncgfx_begin(NC_RGBA(0, 0, 0, 255));
        for (int x = 0; x < W / 2; x += 2) ncgfx_rect(x, 0, 1, H, NC_RGBA(255, 255, 255, 255));
        for (int y = 0; y < H; y += 2) ncgfx_rect(W / 2, y, W / 2, 1, NC_RGBA(255, 255, 255, 255));
        ncgfx_rect(W / 2 - 200, H / 2 - 60, 400, 120, NC_RGBA(200, 30, 30, 255));   /* centre marker */
        ncgfx_rect(0, 0, 8, 8, NC_RGBA(0, 255, 0, 255));                            /* corners */
        ncgfx_rect(W - 8, 0, 8, 8, NC_RGBA(0, 255, 0, 255));
        ncgfx_rect(0, H - 8, 8, 8, NC_RGBA(0, 255, 0, 255));
        ncgfx_rect(W - 8, H - 8, 8, 8, NC_RGBA(0, 255, 0, 255));
        nc_swap();
    }
    if (rw && rh) {   /* put the window back the way we found it */
        int (*set_res)(uint32_t, uint32_t, uint32_t) = nc_hoa_sym("HOA_GX_SetWindowResolution");
        if (set_res) nc_log("restore 1280x720 rc=%d", set_res(wid, 1280, 720));
    }
    nc_exit(0);
}
