/*
 * ncmute: TV audio mute via LG's HOA_CTRL_GetMute/SetMute (through libnc), used by the screen saver.
 *   ncmute status      prints "muted" or "unmuted"
 *   ncmute on          mute; exit 3 if it was already muted (so the caller knows not to unmute later)
 *   ncmute off         unmute
 * Exit 0 = ok, 1 = usage, 2 = LG API unavailable/failed, 3 = already muted (on).
 *
 * The HOA_CTRL calls need a procmgr connection, so this registers a windowless background service
 * (like LG's own lginput/nlp services) for a moment. Run through apps/run-app.sh. RAM-only effect.
 */
#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "status") && strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        fprintf(stderr, "usage: ncmute status|on|off\n");
        return 1;
    }
    const char *svc = getenv("NCMUTE_SERVICE");   /* procmgr accepts a service name once per boot */
    nc_config cfg = {.service = svc && *svc ? svc : "lg.service.normal.ncmute", .no_window = 1, .timeout_s = 15};
    if (nc_init(&cfg) != 0) nc_exit(2);
    int muted = nc_get_mute(), rc = 0;
    if (muted < 0) { nc_log("ncmute: HOA_CTRL_GetMute failed"); nc_exit(2); }
    if (!strcmp(argv[1], "status")) {
        printf("%s\n", muted ? "muted" : "unmuted");
    } else if (!strcmp(argv[1], "on") && muted) {
        printf("already muted\n");
        rc = 3;
    } else {
        int want = !strcmp(argv[1], "on");
        if (nc_set_mute(want) != 0) { nc_log("ncmute: HOA_CTRL_SetMute(%d) failed", want); rc = 2; }
        else printf("%s\n", want ? "muted" : "unmuted");
    }
    fflush(stdout);
    nc_exit(rc);
}
