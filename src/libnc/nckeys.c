/* nckeys: see nckeys.h */
#include "nckeys.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

/* Codes the "LGE RCU" node (event2) sends, from evdump captures (FINDINGS.md "Input", "Remote keys") and
 * RELEASE's IR-to-evdev table (IR_KEY_HOME 773, IR_KEY_INFO 358, ...). First name per code is canonical. */
const nck_name nck_names[] = {
    {"up", KEY_UP, "Up"}, {"down", KEY_DOWN, "Down"}, {"left", KEY_LEFT, "Left"}, {"right", KEY_RIGHT, "Right"},
    {"ok", KEY_ENTER, "OK"}, {"enter", KEY_ENTER, NULL},
    {"back", 412, "Back"},
    {"exit", KEY_EXIT, "Exit"},
    {"home", 773, "Home"},
    {"menu", KEY_MENU, "Settings menu"},
    {"info", KEY_INFO, "Info"},
    {"guide", KEY_EPG, "Guide"},
    {"fav", KEY_FAVORITES, "Favourites"},
    {"volup", KEY_VOLUMEUP, "Volume up"}, {"voldown", KEY_VOLUMEDOWN, "Volume down"}, {"mute", KEY_MUTE, "Mute"},
    {"chup", KEY_CHANNELUP, "Channel up"}, {"chdown", KEY_CHANNELDOWN, "Channel down"},
    {"red", KEY_RED, "Red"}, {"green", KEY_GREEN, "Green"}, {"yellow", KEY_YELLOW, "Yellow"}, {"blue", KEY_BLUE, "Blue"},
    {"0", KEY_0, "0"}, {"1", KEY_1, "1"}, {"2", KEY_2, "2"}, {"3", KEY_3, "3"}, {"4", KEY_4, "4"},
    {"5", KEY_5, "5"}, {"6", KEY_6, "6"}, {"7", KEY_7, "7"}, {"8", KEY_8, "8"}, {"9", KEY_9, "9"},
    {"dash", 771, "Dash"},
    {"flashback", 772, "Flashback"},
    {"instart", 899, "Service In-Start menu"},
};
const int nck_nnames = (int)(sizeof nck_names / sizeof nck_names[0]);

int nck_refused(int code) { return code == 897 || code == 898; }
int nck_valid(int code) { return code > 0 && code <= NCK_MAX && !nck_refused(code); }

int nck_lookup(const char *name) {
    if (!name || !*name) return -1;
    for (int k = 0; k < nck_nnames; k++)
        if (!strcasecmp(name, nck_names[k].name)) return nck_names[k].code;
    char *end;
    long v = strtol(name, &end, 0);
    if (*end || v <= 0 || v > NCK_MAX) return -1;
    return (int)v;
}

const char *nck_name_of(int code) {
    for (int k = 0; k < nck_nnames; k++)
        if (nck_names[k].code == code) return nck_names[k].name;
    return NULL;
}

int nck_open(nck_dev *d, const char *path, int write) {
    memset(d, 0, sizeof *d);
    d->fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (d->fd < 0) return -1;
    if (ioctl(d->fd, EVIOCGNAME(sizeof d->name), d->name) < 0) d->name[0] = 0;
    d->have_bits = ioctl(d->fd, EVIOCGBIT(EV_KEY, sizeof d->bits), d->bits) >= 0;
    return 0;
}

void nck_close(nck_dev *d) {
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
}

int nck_declared(const nck_dev *d, int code) {
    if (!d->have_bits) return -1;
    if (code <= 0 || code > NCK_MAX) return 0;
    return d->bits[code / 8] >> (code % 8) & 1;
}

static int emit1(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    gettimeofday(&ev.time, NULL);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    return write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -1;
}

int nck_emit(const nck_dev *d, int code, int value) {
    if (!nck_valid(code)) { errno = EPERM; return -1; }
    if (emit1(d->fd, EV_KEY, code, value) || emit1(d->fd, EV_SYN, SYN_REPORT, 0)) return -1;
    return 0;
}
