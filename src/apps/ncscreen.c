/*
 * ncscreen: turn the TV's backlight off/on while everything keeps running (Wi-Fi included).
 *
 *   ncscreen off           backlight off
 *   ncscreen on            backlight on
 *   ncscreen off --wake    backlight off until any key/button is pressed (remote, USB keyboard, mouse)
 *   ncscreen saver [min]   screen saver daemon: backlight off after <min> minutes (default 20)
 *                          without any input; any key/button/mouse movement turns it back on.
 *                          Hot-plugged input devices are picked up. Stop it with SIGTERM
 *                          (the backlight is switched back on when it exits).
 *
 * How LG does it (reverse-engineered from RELEASE, confirmed with a live ptrace capture):
 *   API_DDM_BackLight_OnOff(on) -> DDI_GPIO_SetControl(25 "EnableLCDDisplayPanel")
 *   -> MICOM GPIO 0x2102 -> DDI_MICOM_GPIO_SetOutputPort: I2C write {0x36 on | 0x37 off, 0, 0}
 *      to the MICOM (LG ch 4 = MediaTek SIF bus 1, address 0x52, clkdiv 380), wait 10 ms,
 *      read a 1-byte ack (off requires ack 0), up to 3 tries.
 *   SIF transfers are MTAL ioctls on /dev/mtal:
 *      MTSIF_WriteMultipleSubAddr 0x40200C0B / MTSIF_ReadMultipleSubAddr 0x40200C0C with
 *      struct { bus, clkdiv, devaddr, subaddr_len, subaddr, buf*, count, pad } (8 x u32).
 * RAM-only effect: nothing is saved; LG may turn the backlight back on itself (e.g. on input change).
 */
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MTSIF_WRITE 0x40200C0B
#define MTSIF_READ  0x40200C0C
#define MICOM_BUS   1
#define MICOM_CLK   380
#define MICOM_ADDR  0x52
#define CMD_BL_ON   0x36
#define CMD_BL_OFF  0x37

typedef struct { uint32_t bus, clkdiv, devaddr, subaddr_len, subaddr; uint8_t *buf; uint32_t count, pad; } sif_req;

static int micom_backlight(int on) {
    int fd = open("/dev/mtal", O_RDWR);
    if (fd < 0) { perror("open /dev/mtal"); return -1; }
    int ok = 0;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        uint8_t cmd[3] = {on ? CMD_BL_ON : CMD_BL_OFF, 0, 0}, ack = 0xff;
        sif_req w = {MICOM_BUS, MICOM_CLK, MICOM_ADDR, 0, 0, cmd, 3, 0};
        int rw = ioctl(fd, MTSIF_WRITE, &w);
        usleep(10000);
        sif_req r = {MICOM_BUS, MICOM_CLK, MICOM_ADDR, 0, 0, &ack, 1, 0};
        int rr = ioctl(fd, MTSIF_READ, &r);
        if (getenv("NCSCREEN_DEBUG")) printf("try %d: write rc=%d read rc=%d ack=0x%02x\n", attempt, rw, rr, ack);
        if (rw < 0) { perror("SIF write"); usleep(10000); continue; }
        /* LG checks for ack 0 on "off", but from a separate process the ack reads back 0xff even
         * though the command works (verified visually): treat a successful write as success. */
        ok = 1;
    }
    close(fd);
    return ok ? 0 : -1;
}

static void wait_for_key(void) {
    struct pollfd p[32];
    int n = 0;
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        p[n].fd = fd; p[n].events = POLLIN; n++;
    }
    for (;;) {
        if (poll(p, n, -1) <= 0) { if (errno == EINTR) continue; return; }
        for (int i = 0; i < n; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            struct input_event ev[16];
            ssize_t k = read(p[i].fd, ev, sizeof ev);
            for (int j = 0; j < (int)(k / (ssize_t)sizeof *ev); j++)
                if (ev[j].type == EV_KEY && ev[j].value == 1) return;   /* any key/button press */
        }
    }
}

/* ------------------------------------------------------------------ saver */
#define MAXDEV 32
static volatile sig_atomic_t stop_req;
static void on_stop(int s) { (void)s; stop_req = 1; }

static int saver(int minutes) {
    int fds[MAXDEV];
    for (int i = 0; i < MAXDEV; i++) fds[i] = -1;
    signal(SIGTERM, on_stop); signal(SIGINT, on_stop); signal(SIGHUP, SIG_IGN);
    time_t last = time(0), last_scan = 0;
    int dark = 0;
    printf("ncscreen saver: backlight off after %d min without input\n", minutes);
    fflush(stdout);
    while (!stop_req) {
        if (time(0) - last_scan >= 5) {   /* (re)scan for input devices: remote, USB kbd/mouse/gamepad */
            last_scan = time(0);
            for (int i = 0; i < MAXDEV; i++) {
                if (fds[i] >= 0) continue;
                char path[32];
                snprintf(path, sizeof path, "/dev/input/event%d", i);
                fds[i] = open(path, O_RDONLY | O_NONBLOCK);
            }
        }
        struct pollfd p[MAXDEV];
        int map[MAXDEV], n = 0;
        for (int i = 0; i < MAXDEV; i++) if (fds[i] >= 0) { p[n].fd = fds[i]; p[n].events = POLLIN; map[n++] = i; }
        int r = poll(p, n, 1000);
        int activity = 0;
        for (int k = 0; r > 0 && k < n; k++) {
            if (p[k].revents & (POLLERR | POLLHUP | POLLNVAL)) { close(fds[map[k]]); fds[map[k]] = -1; continue; }
            if (!(p[k].revents & POLLIN)) continue;
            struct input_event ev[32];
            ssize_t got = read(p[k].fd, ev, sizeof ev);
            if (got < 0 && errno != EAGAIN) { close(fds[map[k]]); fds[map[k]] = -1; continue; }
            for (int j = 0; j < (int)(got / (ssize_t)sizeof *ev); j++)
                if ((ev[j].type == EV_KEY && ev[j].value == 1) || ev[j].type == EV_REL || ev[j].type == EV_ABS) activity = 1;
        }
        if (activity) {
            last = time(0);
            if (dark) { micom_backlight(1); dark = 0; printf("input: backlight on\n"); fflush(stdout); }
        } else if (!dark && time(0) - last >= (time_t)minutes * 60) {
            if (micom_backlight(0) == 0) { dark = 1; printf("idle %d min: backlight off\n", minutes); fflush(stdout); }
        }
    }
    if (dark) micom_backlight(1);
    printf("ncscreen saver: stopped\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "saver")) return saver(argc > 2 && atoi(argv[2]) > 0 ? atoi(argv[2]) : 20);
    if (argc < 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        fprintf(stderr, "usage: %s on|off [--wake] | saver [minutes]\n", argv[0]);
        return 2;
    }
    int on = !strcmp(argv[1], "on");
    int r = micom_backlight(on);
    printf("backlight %s%s\n", on ? "on" : "off", r == 0 ? "" : ": FAILED");
    fflush(stdout);
    if (!on && argc > 2 && !strcmp(argv[2], "--wake")) {
        wait_for_key();
        micom_backlight(1);
        printf("key pressed: backlight on\n");
    }
    return r == 0 ? 0 : 1;
}
