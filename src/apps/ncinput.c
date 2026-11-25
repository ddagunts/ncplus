/*
 * ncinput: input probe. Logs every input event (HOA callbacks + evdev, including
 * hot-plugged USB keyboards/mice) and gives visual feedback:
 *   arrows / mouse move a square, any key press changes the background hue,
 *   mouse buttons / OK flash the square.
 * Usage: run-app.sh ncinput [seconds]   (default 90)
 */
#include "nc.h"
#include "ncgfx.h"
#include <linux/input.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

static uint32_t hue(float h) {
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
#define C(x) (uint32_t)(255 * ((x) < 0 ? 0 : (x) > 1 ? 1 : (x)) * 0.45f)
    return NC_RGBA(C(r), C(g), C(b), 255);
#undef C
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 90;
    nc_config cfg = {.timeout_s = secs + 15};
    if (nc_init(&cfg) != 0) nc_exit(2);
    ncgfx_init();
    nc_log("ncinput: window %u %dx%d, running %d s. Press remote keys / use USB keyboard+mouse.",
           nc_window_id(), nc_width(), nc_height(), secs);

    int W = nc_width(), H = nc_height();
    float x = W / 2, y = H / 2, h = 0.6f;
    int flash = 0, presses = 0;
    time_t end = time(0) + secs;

    while (time(0) < end) {
        nc_event e;
        while (nc_poll(&e, 0)) {
            switch (e.type) {
            case NC_EV_KEY:
                nc_log("evdev%d [%s] %s(%d) %s", e.dev, e.name, nc_key_name(e.code), e.code,
                       e.value == 1 ? "down" : e.value == 0 ? "up" : "repeat");
                if (e.value) {
                    int step = e.value == 2 ? 30 : 60;
                    if (e.code == KEY_UP) y -= step;
                    else if (e.code == KEY_DOWN) y += step;
                    else if (e.code == KEY_LEFT) x -= step;
                    else if (e.code == KEY_RIGHT) x += step;
                    if (e.value == 1) { h += 0.083f; presses++; }
                    if (e.code == KEY_ENTER || e.code == KEY_OK || e.code == BTN_LEFT || e.code == BTN_RIGHT) flash = 12;
                }
                break;
            case NC_EV_REL:
                if (e.code == REL_X) x += e.value;
                else if (e.code == REL_Y) y += e.value;
                else nc_log("evdev%d [%s] REL code=%d value=%d", e.dev, e.name, e.code, e.value);
                break;
            case NC_EV_ABS:
                nc_log("evdev%d [%s] ABS code=%d value=%d", e.dev, e.name, e.code, e.value);
                break;
            case NC_EV_HOA_KEY:
            case NC_EV_HOA_MOUSE:
                nc_log("HOA %s: %08x %08x %08x %08x %08x %08x", e.type == NC_EV_HOA_KEY ? "key  " : "mouse",
                       e.raw[0], e.raw[1], e.raw[2], e.raw[3], e.raw[4], e.raw[5]);
                if (e.type == NC_EV_HOA_KEY) { h += 0.03f; flash = 6; }
                break;
            case NC_EV_HOA_MSG:
                nc_log("HOA msg: %08x %08x %08x %08x", e.raw[0], e.raw[1], e.raw[2], e.raw[3]);
                break;
            case NC_EV_DEVICE_ADDED: nc_log("device + event%d \"%s\"", e.dev, e.name); break;
            case NC_EV_DEVICE_REMOVED: nc_log("device - event%d \"%s\"", e.dev, e.name); break;
            case NC_EV_QUIT: nc_log("ncinput: quit requested"); nc_exit(0); break;
            default: break;
            }
        }
        if (x < 0) x = 0; if (x > W - 80) x = W - 80;
        if (y < 0) y = 0; if (y > H - 80) y = H - 80;
        if (h > 1) h -= 1;

        ncgfx_begin(hue(h));
        /* frame + progress bar so it's obvious the app is alive */
        float left = (float)(end - time(0)) / secs;
        ncgfx_rect(40, H - 40, (int)((W - 80) * left), 12, NC_RGBA(255, 255, 255, 120));
        for (int i = 0; i < presses && i < 40; i++) ncgfx_rect(40 + i * 30, 40, 22, 22, NC_RGBA(255, 255, 255, 200));
        ncgfx_rect((int)x, (int)y, 80, 80, flash > 0 ? NC_RGBA(255, 255, 0, 255) : NC_RGBA(255, 255, 255, 230));
        if (flash > 0) flash--;
        nc_swap();
    }
    nc_log("ncinput: done, %d key presses seen", presses);
    nc_exit(0);
}
