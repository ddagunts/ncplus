/*
 * ncnosleep: check / disable LG's automatic power-off timers, RAM only (nothing saved to flash).
 *
 *   ncnosleep status     show LG's Auto Off, No-signal power off and No-operation standby settings
 *   ncnosleep off        set them to Off in RELEASE's in-RAM settings copy (until the next power cycle)
 *
 * LG keeps its settings in a RAM shadow (base 0x03148440 in RELEASE) and saves individual bytes to
 * NVRAM via UI_SVC_REGISTRY_WriteNVRAM when changed from the menu. We change only the RAM copy via
 * ptrace (one word write per setting) and do not call the NVRAM writer, so nothing reaches flash.
 * LG then won't (re)arm those timers; a countdown that is already running may still fire once.
 *
 * Firmware-specific (NetCast 4.5, RELEASE v04.03.10, May 2017): code words are verified first and
 * the tool refuses to touch anything on a different build.
 */
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#define SETTINGS_BASE 0x03148440UL
static const struct { unsigned long off; const char *name; } settings[] = {
    {0x936d, "Auto Off (15 min)"},
    {0x937e, "No-signal power off"},
    {0x9380, "No-operation standby"},
};
static const struct { unsigned long addr, word; } signature[] = {   /* RELEASE code / literals */
    {0x6ca024, 0xe59f204c}, {0x6ca028, 0xe309336d}, {0x6ca034, 0xe7d23003},   /* MENU_GetCurrentAutoOff */
    {0x6c97b8, 0xe7cc4003},                                                  /* UI_MENU_SetAutoOff strb */
    {0x6ef1e8, 0x03148440}, {0x6ef0a0, 0x03148440},                           /* settings base literals */
};

static pid_t find_release(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t pid = 0;
    while (d && (e = readdir(d)) && !pid) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], buf[256] = "";
        snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        if (!strcmp(buf, "/mnt/lg/lgapp/RELEASE")) pid = atoi(e->d_name);
    }
    if (d) closedir(d);
    return pid;
}

int main(int argc, char **argv) {
    int do_off = argc > 1 && !strcmp(argv[1], "off");
    if (argc < 2 || (!do_off && strcmp(argv[1], "status"))) { fprintf(stderr, "usage: %s status|off\n", argv[0]); return 2; }
    pid_t pid = find_release();
    if (!pid) { fprintf(stderr, "RELEASE not running\n"); return 1; }
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) { perror("ptrace attach"); return 1; }
    waitpid(pid, 0, __WALL);
    int rc = 0;
    for (unsigned i = 0; i < sizeof signature / sizeof *signature; i++) {
        errno = 0;
        unsigned long w = (unsigned long)ptrace(PTRACE_PEEKTEXT, pid, (void *)signature[i].addr, 0);
        if (errno || w != signature[i].word) {
            fprintf(stderr, "firmware mismatch at %08lx (%08lx != %08lx): refusing\n", signature[i].addr, w, signature[i].word);
            rc = 3;
            goto out;
        }
    }
    for (unsigned i = 0; i < sizeof settings / sizeof *settings; i++) {
        unsigned long a = SETTINGS_BASE + settings[i].off, wa = a & ~3UL;
        int sh = (int)(a & 3) * 8;
        errno = 0;
        unsigned long w = (unsigned long)ptrace(PTRACE_PEEKDATA, pid, (void *)wa, 0);
        if (errno) { perror("peek"); rc = 1; goto out; }
        unsigned v = (w >> sh) & 0xff;
        printf("%-22s %s", settings[i].name, v ? "ON" : "off");
        if (do_off && v) {
            unsigned long nw = w & ~(0xffUL << sh);
            if (ptrace(PTRACE_POKEDATA, pid, (void *)wa, (void *)nw) < 0) { perror(" poke"); rc = 1; }
            else printf("  -> off (RAM only)");
        }
        printf("\n");
    }
out:
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return rc;
}
