/*
 * nclite: stop / restart optional LG services to free RAM and CPU for NetCast+ apps.
 *
 *   nclite stop      HOA_PROC_DisableAndStopServices(list)   (procmgr won't respawn them)
 *   nclite start     HOA_PROC_EnableAndStartServices(list)   (back to normal)
 *   nclite list      show the list and which ones are running
 *
 * Uses LG's process manager (clean stop, no crash handling, no respawn), not kill.
 * Nothing is saved: a power cycle restores everything anyway.
 */
#include "nc.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *svcs[] = {
    "lg.service.normal.pwoff.nomem.webbrowser",   /* Chromium (4 processes) */
    "lg.service.normal.pwoff.nomem.browser",      /* lb6wk WebKit */
    "lg.service.normal.myappsmini",               /* fxui: My Apps bar */
    "lg.service.normal.voicenlp",                 /* fxui: voice UI */
    "lg.service.normal.nlp",                      /* voice recognition */
    "lg.service.normal.voicehost",                /* Skype / VCS */
    "lg.service.normal.videohost",
    "lg.service.normal.launcher",
    "lg.service.normal.dial",                     /* Netflix DIAL server */
    "lg.service.normal.alljoyn",
    "lg.service.normal.alljoynr",
    "lg.service.normal.acr",                      /* content recognition */
    "lg.service.normal.wfdd",                     /* Miracast */
    "lg.service.normal.camera",
    "lg.service.normal.camera_manager",
    "lg.service.normal.dmr2",                     /* DLNA renderer */
};
#define NSVC ((int)(sizeof svcs / sizeof *svcs))

static int running(const char *svc) {   /* is a process with "--service <svc>" alive? */
    DIR *d = opendir("/proc");
    struct dirent *de;
    int found = 0;
    while (d && !found && (de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[64], buf[1024];
        snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
        buf[n] = 0;
        char pat[256];
        snprintf(pat, sizeof pat, "--service %s ", svc);
        size_t pl = strlen(pat);
        found = strstr(buf, pat) != NULL || (n >= pl - 1 && !strncmp(buf + n - (pl - 1), pat, pl - 1));
    }
    if (d) closedir(d);
    return found;
}

static long mem_free_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long freek = 0, cached = 0, buffers = 0, v;
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemFree: %ld", &v) == 1) freek = v;
        else if (sscanf(line, "Cached: %ld", &v) == 1) cached = v;
        else if (sscanf(line, "Buffers: %ld", &v) == 1) buffers = v;
    }
    if (f) fclose(f);
    return freek + cached + buffers;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "list";
    if (!strcmp(cmd, "list")) {
        for (int i = 0; i < NSVC; i++) printf("%-45s %s\n", svcs[i], running(svcs[i]) ? "running" : "-");
        printf("available memory: %ld MB\n", mem_free_kb() / 1024);
        return 0;
    }
    int stop = !strcmp(cmd, "stop");
    if (!stop && strcmp(cmd, "start")) { fprintf(stderr, "usage: %s stop|start|list\n", argv[0]); return 2; }

    nc_config cfg = {.no_window = 1, .timeout_s = 60};
    if (nc_init(&cfg) != 0) return 3;
    int (*fn)(const char **, int) = nc_hoa_sym(stop ? "HOA_PROC_DisableAndStopServices" : "HOA_PROC_EnableAndStartServices");
    if (!fn) { fprintf(stderr, "HOA_PROC function missing\n"); nc_exit(4); }
    long before = mem_free_kb();
    /* one service per call: LG's HOA_PROC_*Services undersizes its message buffer for long lists */
    int r = 0;
    for (int i = 0; i < NSVC; i++) {
        const char *one[1] = {svcs[i]};
        int ri = fn(one, 1);
        if (ri != 0) r = ri;
        printf("  %s %-45s rc=%d\n", stop ? "stop " : "start", svcs[i], ri);
    }
    printf("%s %d services: rc=%d\n", stop ? "stopped" : "started", NSVC, r);
    for (int i = 0; i < NSVC; i++) printf("  %-45s %s\n", svcs[i], running(svcs[i]) ? "running" : "-");
    printf("available memory: %ld MB -> %ld MB\n", before / 1024, mem_free_kb() / 1024);
    fflush(stdout);
    nc_exit(r == 0 ? 0 : 1);
}
