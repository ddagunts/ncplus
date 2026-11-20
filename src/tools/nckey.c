/* nckey: inject a key press+release into an evdev node by write()ing input_events to it
 * (kernel evdev_write -> input_inject_event), as if the remote sent it.
 *
 *   nckey [-d /dev/input/event2] [-n] [--force] <code|name> ...
 *     -n       dry run: report whether the device declares the key, don't inject
 *     --force  inject even if the device's keybit doesn't include the code (kernel drops it anyway)
 *
 * Names (LG internal key codes, from RELEASE's UI_SUMODE_GlobalKeyHandler):
 *   instart = 899 (0x383)  service In-Start menu (toggle)
 * Codes 897/898 (0x381/0x382, IN-STOP / power-only paths in SUMODE_DoHotKey) are REFUSED:
 * IN-STOP resets the set to shipping state.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define NC_KEY_MAX 0x3ff /* LG uses codes up to 0x3xx; SDK headers say 0x2ff */

static const struct { const char *name; int code; } names[] = {
    {"instart", 899}, {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
    {"ok", KEY_ENTER}, {"back", 412}, {"exit", KEY_EXIT},
    {"0", KEY_0}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4},
    {"5", KEY_5}, {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9},
};

static int emit(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    gettimeofday(&ev.time, NULL);
    ev.type = type; ev.code = code; ev.value = value;
    return write(fd, &ev, sizeof ev) == sizeof ev ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/input/event2";
    int dry = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-n")) dry = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else { fprintf(stderr, "usage: nckey [-d dev] [-n] [--force] <code|name>...\n"); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "usage: nckey [-d dev] [-n] [--force] <code|name>...\n"); return 2; }

    int fd = open(dev, dry ? O_RDONLY : O_RDWR);
    if (fd < 0) { fprintf(stderr, "%s: %s\n", dev, strerror(errno)); return 1; }
    char nm[80] = "";
    ioctl(fd, EVIOCGNAME(sizeof nm), nm);
    unsigned char bits[NC_KEY_MAX / 8 + 1];
    memset(bits, 0, sizeof bits);
    int have_bits = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0;
    printf("%s: \"%s\"\n", dev, nm);

    for (; i < argc; i++) {
        int code = -1;
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
            if (!strcasecmp(argv[i], names[k].name)) code = names[k].code;
        if (code < 0) code = (int)strtol(argv[i], NULL, 0);
        if (code <= 0 || code > NC_KEY_MAX) { fprintf(stderr, "bad key %s\n", argv[i]); return 2; }
        if (code == 897 || code == 898) { fprintf(stderr, "refusing %d (IN-STOP/power-only)\n", code); return 2; }
        int declared = have_bits && (bits[code / 8] >> (code % 8) & 1);
        printf("key %d (0x%x): %s\n", code, code, declared ? "declared by device" : "NOT declared by device");
        if (dry) continue;
        if (!declared && !force) { fprintf(stderr, "skipping (use --force)\n"); return 1; }
        if (emit(fd, EV_KEY, code, 1) || emit(fd, EV_SYN, SYN_REPORT, 0)) { perror("write"); return 1; }
        usleep(80000);
        if (emit(fd, EV_KEY, code, 0) || emit(fd, EV_SYN, SYN_REPORT, 0)) { perror("write"); return 1; }
        printf("  injected\n");
        usleep(300000);
    }
    close(fd);
    return 0;
}
