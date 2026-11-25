/* nckey: inject a key press+release into an evdev node by write()ing input_events to it
 * (kernel evdev_write -> input_inject_event), as if the remote sent it.
 *
 *   nckey [-d /dev/input/event2] [-n] [--force] <code|name> ...
 *   nckey -l                 list the key names
 *     -n       dry run: report whether the device declares the key, don't inject
 *     --force  inject even if the device's keybit doesn't include the code (kernel drops it anyway)
 *
 * Names and the refusal list are in libnc/nckeys.c (shared with tools/ncapi), e.g.
 *   instart = 899 (0x383)  service In-Start menu (toggle)
 * Codes 897/898 (0x381/0x382, IN-STOP / power-only paths in SUMODE_DoHotKey) are REFUSED:
 * IN-STOP resets the set to shipping state.
 */
#include "nckeys.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int usage(void) {
    fprintf(stderr, "usage: nckey [-d dev] [-n] [--force] <code|name>...   |   nckey -l\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *dev = NCK_DEFAULT_DEV;
    int dry = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-n")) dry = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "-l")) {
            for (int k = 0; k < nck_nnames; k++) printf("%-10s %d\n", nck_names[k].name, nck_names[k].code);
            return 0;
        } else return usage();
    }
    if (i >= argc) return usage();

    nck_dev d;
    if (nck_open(&d, dev, !dry) != 0) { fprintf(stderr, "%s: %s\n", dev, strerror(errno)); return 1; }
    printf("%s: \"%s\"\n", dev, d.name);

    for (; i < argc; i++) {
        int code = nck_lookup(argv[i]);
        if (code < 0) { fprintf(stderr, "bad key %s\n", argv[i]); return 2; }
        if (nck_refused(code)) { fprintf(stderr, "refusing %d (IN-STOP/power-only)\n", code); return 2; }
        int declared = nck_declared(&d, code) == 1;
        printf("key %d (0x%x): %s\n", code, code, declared ? "declared by device" : "NOT declared by device");
        if (dry) continue;
        if (!declared && !force) { fprintf(stderr, "skipping (use --force)\n"); return 1; }
        if (nck_emit(&d, code, 1)) { perror("write"); return 1; }
        usleep(80000);
        if (nck_emit(&d, code, 0)) { perror("write"); return 1; }
        printf("  injected\n");
        usleep(300000);
    }
    nck_close(&d);
    return 0;
}
