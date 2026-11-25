/* ncconf: see ncconf.h */
#include "ncconf.h"
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <unistd.h>

const char *nc_rundir(void) {
    const char *r = getenv("NC_RUNDIR");
    return r && *r ? r : "/tmp";
}

/* "[name]" (with optional whitespace) -> name, len */
static int is_header(const char *l, const char **name, int *len) {
    while (*l == ' ' || *l == '\t') l++;
    if (*l != '[') return 0;
    const char *e = strchr(l, ']');
    if (!e) return 0;
    *name = l + 1;
    *len = (int)(e - l - 1);
    return 1;
}

static int header_is(const char *l, const char *section) {
    const char *n; int len;
    return is_header(l, &n, &len) && len == (int)strlen(section) && !strncasecmp(n, section, (size_t)len);
}

/* "  KEY = value" -> pointer to value (untrimmed), or NULL */
static const char *key_value(const char *l, const char *key) {
    while (*l == ' ' || *l == '\t') l++;
    size_t k = strlen(key);
    if (strncmp(l, key, k)) return NULL;
    l += k;
    while (*l == ' ' || *l == '\t') l++;
    if (*l != '=') return NULL;
    l++;
    while (*l == ' ' || *l == '\t') l++;
    return l;
}

int ncconf_get(const char *conf, const char *section, const char *key, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(conf, "r");
    if (!f) return 0;
    char line[1024];
    int on = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        const char *nm; int len;
        if (is_header(line, &nm, &len)) { on = header_is(line, section); continue; }
        if (!on) continue;
        const char *v = key_value(line, key);
        if (!v) continue;
        snprintf(out, n, "%s", v);
        size_t e = strlen(out);
        while (e && isspace((unsigned char)out[e - 1])) out[--e] = 0;
        found = 1;
    }
    fclose(f);
    return found;
}

static int lock_conf(void) {
    char p[512];
    snprintf(p, sizeof p, "%s/ncconf.lock", nc_rundir());
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) flock(fd, LOCK_EX);
    return fd;
}

int ncconf_set(const char *conf, const char *section, const char *key, const char *value) {
    int lk = lock_conf();
    char tmp[600], line[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", conf);
    FILE *in = fopen(conf, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); if (lk >= 0) close(lk); return -1; }
    /* Write KEY=value where the first KEY line of the section was (dropping later duplicates), or at the
     * end of the section's content (before its trailing blank lines) if there was none. */
    int on = 0, done = 0, seen = 0;
    char held[64][1024];   /* blank lines held back at the end of the section */
    int nheld = 0;
    const char *eol = "\n";
    int nl = 1;                    /* the last line read ended with a newline (the file's last may not) */
    while (in && fgets(line, sizeof line, in)) {
        size_t L = strlen(line);   /* new lines get the line ending of the line before them */
        nl = L && line[L - 1] == '\n';
        if (nl) eol = L >= 2 && line[L - 2] == '\r' ? "\r\n" : "\n";
        const char *nm; int len;
        if (is_header(line, &nm, &len)) {
            if (on && !done) { fprintf(out, "%s=%s%s", key, value, eol); done = 1; }
            for (int i = 0; i < nheld; i++) fputs(held[i], out);
            nheld = 0;
            on = header_is(line, section);
            if (on) seen = 1;
            fputs(line, out);
            continue;
        }
        if (on) {
            if (key_value(line, key)) {
                if (!done) {
                    for (int i = 0; i < nheld; i++) fputs(held[i], out);
                    nheld = 0;
                    fprintf(out, "%s=%s%s", key, value, eol);
                    done = 1;
                }
                continue;
            }
            const char *s = line;
            while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
            if (!*s && !done && nheld < 64) { snprintf(held[nheld++], sizeof held[0], "%s", line); continue; }
            for (int i = 0; i < nheld; i++) fputs(held[i], out);
            nheld = 0;
        }
        fputs(line, out);
    }
    if (!done) {
        if (!nl) fputs(eol, out);
        if (!seen) fprintf(out, "%s[%s]%s", eol, section, eol);
        fprintf(out, "%s=%s%s", key, value, eol);
    }
    for (int i = 0; i < nheld; i++) fputs(held[i], out);
    if (in) fclose(in);
    int ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
    ok = fclose(out) == 0 && ok;
    if (ok) ok = rename(tmp, conf) == 0;
    else remove(tmp);
    if (lk >= 0) close(lk);
    return ok ? 0 : -1;
}

/* ---- [settings] ---- */
static const int saver_opts[] = {0, 5, 10, 20, 30, 60};
static const int bool_opts[] = {0, 1};
#define B bool_opts, 2
const ncs_def ncs_defs[] = {
    {"SAVER_MIN", "Screen saver", "Backlight off after this many minutes without input (0 = off)", 20,
     saver_opts, (int)(sizeof saver_opts / sizeof *saver_opts), 0},
    {"NOSLEEP", "Keep TV awake", "Disable LG's Auto Off / No-signal / No-operation timers (RAM only)", 1, B, 0},
    {"LITE", "Lite mode", "Stop optional LG services (browsers, voice, Skype, ...) for more free RAM", 0, B, 0},
    {"NETKEEP", "Wi-Fi keep-alive", "ARP the gateway so the router doesn't lose the TV every ~20 min", 1, B, 0},
    {"API", "Remote API", "HTTP API for remote keys, apps and settings (tools/ncapi)", 0, B, 1},
};
#undef B
const int ncs_ndefs = (int)(sizeof ncs_defs / sizeof ncs_defs[0]);

const ncs_def *ncs_find(const char *key) {
    for (int i = 0; i < ncs_ndefs; i++)
        if (!strcmp(ncs_defs[i].key, key)) return &ncs_defs[i];
    return NULL;
}

int ncs_valid(const ncs_def *d, int v) {
    for (int i = 0; i < d->nopts; i++)
        if (d->opts[i] == v) return 1;
    return 0;
}

int ncs_get(const char *conf, const ncs_def *d) {
    char v[64];
    if (!ncconf_get(conf, "settings", d->key, v, sizeof v) || !*v) return d->def;
    char *end;
    long x = strtol(v, &end, 10);
    if (*end || !ncs_valid(d, (int)x)) return d->def;
    return (int)x;
}

int ncs_set(const char *conf, const ncs_def *d, int v) {
    if (!ncs_valid(d, v)) return -1;
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    return ncconf_set(conf, "settings", d->key, s);
}

static void run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void run(const char *fmt, ...) {
    char cmd[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    fprintf(stderr, "ncconf: %s\n", cmd);
    int r = system(cmd);
    (void)r;
}

void ncs_apply(const ncs_def *d, int v, const char *tools) {
    const char *R = nc_rundir();   /* logs */
    if (!strcmp(d->key, "SAVER_MIN")) {
        run("killall ncscreen 2>/dev/null");
        if (v > 0) run("nohup '%s/ncscreen' saver %d > '%s/ncscreen-saver.log' 2>&1 &", tools, v, R);
    } else if (!strcmp(d->key, "NOSLEEP")) {
        /* off again needs a power cycle: LG's timers return then */
        if (v) run("'%s/ncnosleep' off > '%s/ncnosleep.log' 2>&1", tools, R);
    } else if (!strcmp(d->key, "LITE")) {
        /* nclite registers with LG's process manager itself: start it through run-app.sh */
        run("sh '%s/../apps/run-app.sh' '%s/nclite' %s > '%s/nclite.log' 2>&1 &", tools, tools, v ? "stop" : "start", R);
    } else if (!strcmp(d->key, "NETKEEP")) {
        run("killall ncnetkeep 2>/dev/null");
        if (v) run("nohup '%s/ncnetkeep' > '%s/ncnetkeep.log' 2>&1 &", tools, R);
    } else if (!strcmp(d->key, "API")) {
        run("killall ncapi 2>/dev/null");
        if (v) run("'%s/ncapi' token ensure && { nohup '%s/ncapi' > '%s/ncapi.log' 2>&1 & }", tools, tools, R);
    }
}
