/*
 * nctext: on-screen input console. Shows the last input events as text (LG font,
 * FreeType), so key codes can be read straight off the TV. Arrows move a selection
 * highlight through a demo menu.
 * Usage: run-app.sh nctext [seconds]   (default 120)
 */
#include "nc.h"
#include "ncgfx.h"
#include "nctext.h"
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NLINES 14
static char lines[NLINES][128];
static int nlines;

static void add_line(const char *s) {
    if (nlines == NLINES) { memmove(lines[0], lines[1], sizeof lines[0] * (NLINES - 1)); nlines--; }
    snprintf(lines[nlines++], sizeof lines[0], "%s", s);
    nc_log("%s", s);
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 120;
    nc_config cfg = {.timeout_s = secs + 15};
    if (nc_init(&cfg) != 0) nc_exit(2);
    ncgfx_init();
    nctext_font *big = nctext_open(NULL, 44), *small = nctext_open(NULL, 26);
    if (!big || !small) nc_exit(3);

    const char *menu[] = {"Hello from native code", "PowerVR SGX 544MP / GLES2", "FreeType + LG Display font",
                          "Use the arrows / USB keyboard", "Mouse works too"};
    int nmenu = 5, sel = 0, mx = -1, my = -1;
    char buf[160];
    time_t end = time(0) + secs;
    add_line("ready - press keys");

    while (time(0) < end) {
        nc_event e;
        while (nc_poll(&e, 0)) {
            switch (e.type) {
            case NC_EV_KEY:
                if (e.value == 0) break;
                snprintf(buf, sizeof buf, "%s  %s (%d)%s  [event%d %s]", e.code >= BTN_MISC && e.code < KEY_OK ? "btn" : "key",
                         nc_key_name(e.code), e.code, e.value == 2 ? " repeat" : "", e.dev, e.name);
                add_line(buf);
                if (e.code == KEY_ESC) nc_exit(0);   /* Back / Esc: leave */
                if (e.code == KEY_UP && sel > 0) sel--;
                if (e.code == KEY_DOWN && sel < nmenu - 1) sel++;
                break;
            case NC_EV_REL:
                if (e.code == REL_X) mx = (mx < 0 ? nc_width() / 2 : mx) + e.value;
                else if (e.code == REL_Y) my = (my < 0 ? nc_height() / 2 : my) + e.value;
                else { snprintf(buf, sizeof buf, "rel code=%d value=%d [event%d]", e.code, e.value, e.dev); add_line(buf); }
                break;
            case NC_EV_HOA_KEY:
                snprintf(buf, sizeof buf, "HOA key %08x %08x %08x %08x", e.raw[0], e.raw[1], e.raw[2], e.raw[3]);
                add_line(buf);
                break;
            case NC_EV_HOA_MOUSE:
                snprintf(buf, sizeof buf, "HOA mouse %08x %08x %08x %08x", e.raw[0], e.raw[1], e.raw[2], e.raw[3]);
                add_line(buf);
                break;
            case NC_EV_DEVICE_ADDED:
            case NC_EV_DEVICE_REMOVED:
                snprintf(buf, sizeof buf, "device %s event%d \"%s\"", e.type == NC_EV_DEVICE_ADDED ? "+" : "-", e.dev, e.name);
                add_line(buf);
                break;
            case NC_EV_QUIT: nc_exit(0); break;
            default: break;
            }
        }
        int W = nc_width(), H = nc_height();
        if (mx >= 0) { if (mx < 0) mx = 0; if (mx > W - 1) mx = W - 1; if (my < 0) my = 0; if (my > H - 1) my = H - 1; }

        ncgfx_begin(NC_RGBA(16, 20, 36, 255));
        nctext_draw(big, 60, 90, "NetCast native app", NC_RGBA(255, 255, 255, 255));
        snprintf(buf, sizeof buf, "window %u  %dx%d   BACK to exit (auto in %ld s)", nc_window_id(), W, H, (long)(end - time(0)));
        nctext_draw(small, 60, 130, buf, NC_RGBA(150, 170, 210, 255));
        for (int i = 0; i < nmenu; i++) {
            int y = 200 + i * 56;
            if (i == sel) ncgfx_rect(50, y - 38, 560, 50, NC_RGBA(60, 110, 220, 255));
            nctext_draw(small, 70, y, menu[i], NC_RGBA(255, 255, 255, i == sel ? 255 : 190));
        }
        ncgfx_rect(660, 170, 580, 500, NC_RGBA(0, 0, 0, 120));
        nctext_draw(small, 680, 205, "input events", NC_RGBA(255, 210, 90, 255));
        for (int i = 0; i < nlines; i++) nctext_draw(small, 680, 245 + i * 30, lines[i], NC_RGBA(220, 220, 220, 255));
        if (mx >= 0) ncgfx_rect(mx - 6, my - 6, 12, 12, NC_RGBA(255, 80, 80, 255));
        nc_swap();
    }
    nc_exit(0);
}
