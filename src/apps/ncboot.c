/*
 * ncboot: "Starting Linux" splash for src/mainline/fire.sh.
 *
 * nckexec hands the panel over by taking OSD plane 3 from LG's compositor, which is only in
 * the known 1280x720 ARGB state while an app window is up. With nothing loaded (e.g. right
 * after a watchdog return from mainline) fire.sh starts this app first: it registers with
 * procmgr, puts an opaque full-screen window up and keeps it there until SIGTERM (fire.sh,
 * if nckexec refused) or the warm boot. After a few presented frames it creates the ready
 * file. That only means the swaps returned, not that the window is in front: fire.sh checks
 * plane 3 itself, and until it is done this app shows and raises its window again every second.
 *
 * Usage: run-app.sh ncboot [ready-file] [seconds]   (default: no file, 120 s safety timeout)
 */
#include "nc.h"
#include "ncgfx.h"
#include "nctext.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void draw(nctext_font *big, nctext_font *small, int tick) {
    static const char *dots[] = {"", ".", "..", "..."};
    const char *title = "Starting Linux";
    const char *sub = "The TV is handing over to the mainline kernel";
    int w = nc_width(), h = nc_height();
    int tw = nctext_width(big, title);
    char buf[32];

    ncgfx_begin(NC_RGBA(0, 0, 0, 255));
    snprintf(buf, sizeof buf, "%s%s", title, dots[tick & 3]);
    nctext_draw(big, (w - tw) / 2, h / 2, buf, NC_RGBA(255, 255, 255, 255));
    nctext_draw(small, (w - nctext_width(small, sub)) / 2, h / 2 + nctext_line_height(small) * 2, sub,
                NC_RGBA(160, 160, 160, 255));
    nc_swap();
}

int main(int argc, char **argv) {
    const char *ready = argc > 1 && argv[1][0] ? argv[1] : NULL;
    int secs = argc > 2 ? atoi(argv[2]) : 120;
    nc_config cfg = {.timeout_s = secs, .no_evdev = 1};
    nc_event ev;
    int tick = 0;
    time_t raised = time(NULL);

    if (nc_init(&cfg) != 0) nc_exit(2);
    ncgfx_init();
    nctext_font *big = nctext_open(NULL, 56), *small = nctext_open(NULL, 26);
    if (!big || !small) nc_exit(3);

    for (;;) {
        draw(big, small, tick);
        if (++tick == 3 && ready) {     /* a few frames presented: the window is on plane 3 */
            int fd = open(ready, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        }
        if (time(NULL) != raised) {     /* another screen may have kept or taken the front */
            nc_set_visible(1);
            raised = time(NULL);
        }
        while (nc_poll(&ev, tick < 3 ? 50 : 500))
            if (ev.type == NC_EV_QUIT) nc_exit(0);
        if (nc_quit_requested()) nc_exit(0);
    }
}
