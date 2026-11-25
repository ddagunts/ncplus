/*
 * ncapi: ncplus remote-control API. A small HTTP/1.1 + JSON server on the LAN (default port 9080) for
 * remote keys, ncplus apps and ncplus settings. Started by start.sh when [settings] API=1 (Settings >
 * Remote API); keeps running across app switches and after "Exit to TV", like ncnetkeep.
 *
 *   ncapi                        run the server (port: [api] PORT in netcast.conf, default 9080)
 *   ncapi token show|ensure|new  print / create if missing / replace the token ([api] TOKEN)
 *
 * Every request needs "Authorization: Bearer <token>" (32 hex characters from /dev/urandom). Plain HTTP:
 * the token crosses the LAN in the clear.
 *
 *   GET  /api/v1/status     {"netcast":true,"app":"doom","title":"Doom","screen":"on","version":...,
 *                           "id":"<MAC of the default-route interface>","uptime":s,"api_uptime":s}
 *   GET  /api/v1/keys       key names -> codes
 *   POST /api/v1/key        {"key":"up"} or {"code":115}; optional "hold_ms" (default 80), "repeat" (1),
 *                           "delay_ms" between repeats (200). Injected into /dev/input/event2 like the remote.
 *   GET  /api/v1/apps       ncplus apps (apps/<id>/app.ini)
 *   POST /api/v1/launch     {"app":"<id>"} | {"app":"launcher"} | {"app":"exit"} (back to LG's home)
 *   GET  /api/v1/settings   [settings] values with their allowed options
 *   PUT  /api/v1/settings   {"SAVER_MIN":20,"NETKEEP":1,...}: checked like the Settings app, applied live
 *   POST /api/v1/screen     {"on":false}: backlight off (any remote key turns it on again) / {"on":true}
 *
 * Launching: ncapi writes the target to <rundir>/ncmenu.cmd and sends SIGTERM to the app in front (its pid
 * and start time are in <rundir>/ncapp.fg, written by run-app.sh for the session loop's app); start.sh's
 * loop runs the target next. SIGTERM is repeated every 1.5 s while the app is still there (Doom asks
 * "quit?" on the first one), never SIGKILL. If ncplus isn't running, ncapi starts start.sh.
 *
 * Single-threaded: poll() over the listening socket, up to 16 clients and the key-event queue.
 * Environment (tests): NETCAST_DIR (default: this binary's ../), NETCAST_CONF, NC_RUNDIR, NC_KEYDEV.
 */
#define _GNU_SOURCE
#include "ncconf.h"
#include "nckeys.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 9080
#define IFNAMSIZ_ 15
#define MAXC 16
#define REQMAX 8192
#define CLIENT_MS 5000

static char N[PATH_MAX], conf[PATH_MAX], tools[PATH_MAX + 8], keydev[PATH_MAX];
static char token[80];
static time_t started;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "%s ncapi: ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void rpath(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", nc_rundir(), name); }

static int read_small(const char *path, char *buf, size_t n) {
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    size_t e = (size_t)r;
    while (e && isspace((unsigned char)buf[e - 1])) buf[--e] = 0;
    return 0;
}

static int write_small(const char *path, const char *s) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(s, f);
    if (fclose(f) != 0) { remove(tmp); return -1; }
    return rename(tmp, path);
}

/* ------------------------------------------------------------------ token */
static int token_ok(const char *t) {
    size_t n = strlen(t);
    if (n < 16 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)t[i])) return 0;
    return 1;
}

static int token_new(void) {
    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, r, sizeof r) != (ssize_t)sizeof r) { perror("/dev/urandom"); if (fd >= 0) close(fd); return -1; }
    close(fd);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", r[i]);
    if (ncconf_set(conf, "api", "TOKEN", hex) != 0) { fprintf(stderr, "ncapi: cannot write %s\n", conf); return -1; }
    snprintf(token, sizeof token, "%s", hex);
    return 0;
}

static int cmd_token(const char *what) {
    ncconf_get(conf, "api", "TOKEN", token, sizeof token);
    if (!strcmp(what, "show")) { if (!token_ok(token)) return 1; printf("%s\n", token); return 0; }
    if (!strcmp(what, "ensure")) { if (token_ok(token)) return 0; return token_new() ? 1 : 0; }
    if (!strcmp(what, "new")) return token_new() ? 1 : 0;
    fprintf(stderr, "usage: ncapi token show|ensure|new\n");
    return 2;
}

/* Constant-time compare of the Authorization header's token. */
static int auth_ok(const char *hdr) {
    if (!hdr) return 0;
    while (*hdr == ' ') hdr++;
    if (strncasecmp(hdr, "Bearer ", 7)) return 0;
    hdr += 7;
    while (*hdr == ' ') hdr++;
    size_t n = strcspn(hdr, " \r\n"), m = strlen(token);
    unsigned d = (unsigned)(n ^ m);
    for (size_t i = 0; i < m; i++) d |= (unsigned char)token[i] ^ (unsigned char)(i < n ? hdr[i] : 0);
    return d == 0;
}

/* ------------------------------------------------------------------ JSON in */
typedef struct { char k[64]; char type; char s[256]; double n; } jkv;   /* type: s n b z */

static const char *jws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

static const char *jstring(const char *p, char *out, size_t n) {
    if (*p != '"') return NULL;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)p[i])) return NULL;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
                }
                p += 4;
                c = v < 0x80 ? (char)v : '?';
                break;
            }
            default: return NULL;
            }
        } else if ((unsigned char)c < 0x20) return NULL;
        if (o + 1 < n) out[o++] = c;
    }
    if (*p != '"') return NULL;
    out[o] = 0;
    return p + 1;
}

/* Flat object only (no nested objects/arrays). Returns the number of pairs, or -1. */
static int json_parse(const char *p, jkv *kv, int max) {
    int n = 0;
    p = jws(p);
    if (*p != '{') return -1;
    p = jws(p + 1);
    if (*p == '}') return *jws(p + 1) ? -1 : 0;
    for (;;) {
        if (n >= max) return -1;
        jkv *e = &kv[n];
        memset(e, 0, sizeof *e);
        if (!(p = jstring(jws(p), e->k, sizeof e->k))) return -1;
        p = jws(p);
        if (*p != ':') return -1;
        p = jws(p + 1);
        if (*p == '"') { if (!(p = jstring(p, e->s, sizeof e->s))) return -1; e->type = 's'; }
        else if (!strncmp(p, "true", 4)) { e->type = 'b'; e->n = 1; p += 4; }
        else if (!strncmp(p, "false", 5)) { e->type = 'b'; e->n = 0; p += 5; }
        else if (!strncmp(p, "null", 4)) { e->type = 'z'; p += 4; }
        else if (*p == '-' || isdigit((unsigned char)*p)) {
            char *end;
            e->n = strtod(p, &end);
            if (end == p) return -1;
            p = end;
            e->type = 'n';
        } else return -1;
        n++;
        p = jws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') return *jws(p + 1) ? -1 : n;
        return -1;
    }
}

static const jkv *jget(const jkv *kv, int n, const char *k) {
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].k, k)) return &kv[i];
    return NULL;
}

/* integer value of a number (or bool) field; 0 = ok, -1 = wrong type / not an integer */
static int jint(const jkv *e, int *out) {
    if (!e || (e->type != 'n' && e->type != 'b')) return -1;
    if (e->n != (double)(long long)e->n || e->n < -1e9 || e->n > 1e9) return -1;
    *out = (int)e->n;
    return 0;
}

/* ------------------------------------------------------------------ JSON out */
typedef struct { char *b; size_t n, cap; } sbuf;

static void sb_add(sbuf *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        size_t room = s->cap - s->n;
        va_start(ap, fmt);
        int w = vsnprintf(s->b ? s->b + s->n : NULL, s->b ? room : 0, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if (s->b && (size_t)w < room) { s->n += (size_t)w; return; }
        size_t nc = s->cap ? s->cap * 2 : 1024;
        while (nc < s->n + (size_t)w + 1) nc *= 2;
        char *nb = realloc(s->b, nc);
        if (!nb) return;
        s->b = nb;
        s->cap = nc;
    }
}

static void sb_str(sbuf *s, const char *v) {
    sb_add(s, "\"");
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p == '"' || *p == '\\') sb_add(s, "\\%c", *p);
        else if (*p == '\n') sb_add(s, "\\n");
        else if (*p == '\t') sb_add(s, "\\t");
        else if (*p < 0x20) sb_add(s, "\\u%04x", *p);
        else sb_add(s, "%c", *p);
    }
    sb_add(s, "\"");
}

/* ------------------------------------------------------------------ processes */
/* Run argv detached (own session, stdin /dev/null, output to log). Returns the pid. */
static pid_t spawn(const char *log, char *const argv[]) {
    pid_t p = fork();
    if (p != 0) return p;
    setsid();
    signal(SIGPIPE, SIG_DFL);
    int z = open("/dev/null", O_RDWR);
    int l = open(log ? log : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (z >= 0) dup2(z, 0);
    if (l >= 0) { dup2(l, 1); dup2(l, 2); }
    execv(argv[0], argv);
    _exit(127);
}

/* pid + start time (field 22 of /proc/<pid>/stat), so a recycled pid is never signalled */
static int proc_start(pid_t pid, unsigned long long *st) {
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    if (read_small(p, b, sizeof b) != 0) return -1;
    char *s = strrchr(b, ')');
    if (!s) return -1;
    s += 2;                                  /* field 3 */
    for (int f = 3; f < 22; f++) { s = strchr(s, ' '); if (!s) return -1; s++; }
    *st = strtoull(s, NULL, 10);
    return 0;
}

static int proc_cmdline_has(pid_t pid, const char *needle) {
    char p[64], b[2048];
    snprintf(p, sizeof p, "/proc/%d/cmdline", (int)pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if (r <= 0) return 0;
    for (ssize_t i = 0; i < r; i++) if (!b[i]) b[i] = ' ';
    b[r] = 0;
    return strstr(b, needle) != NULL;
}

/* The start.sh session loop (it writes <rundir>/ncsession.pid). */
static int session_alive(void) {
    char p[PATH_MAX], b[32];
    rpath(p, sizeof p, "ncsession.pid");
    if (read_small(p, b, sizeof b) != 0) return 0;
    pid_t pid = (pid_t)atoi(b);
    return pid > 1 && kill(pid, 0) == 0 && proc_cmdline_has(pid, "start.sh");
}

/* The app in front: "<pid> <starttime>" in <rundir>/ncapp.fg (run-app.sh). */
static int fg_app(pid_t *pid, unsigned long long *st) {
    char p[PATH_MAX], b[64];
    rpath(p, sizeof p, "ncapp.fg");
    if (read_small(p, b, sizeof b) != 0) return -1;
    unsigned long long want = 0, have = 0;
    int x = 0;
    if (sscanf(b, "%d %llu", &x, &want) != 2 || x <= 1) return -1;
    if (proc_start((pid_t)x, &have) != 0 || have != want) return -1;
    *pid = (pid_t)x;
    *st = want;
    return 0;
}

/* ------------------------------------------------------------------ apps */
typedef struct { char id[64], title[64], desc[160]; int order; } app_t;
#define MAXAPPS 64

static int load_apps(app_t *a, int max) {
    char root[PATH_MAX + 8];
    snprintf(root, sizeof root, "%s/apps", N);
    DIR *d = opendir(root);
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof a->id) continue;
        char path[PATH_MAX * 2];
        snprintf(path, sizeof path, "%s/%s/app.ini", root, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        app_t *x = &a[n];
        memset(x, 0, sizeof *x);
        snprintf(x->id, sizeof x->id, "%s", de->d_name);
        snprintf(x->title, sizeof x->title, "%s", de->d_name);
        x->order = 50;
        char line[256];
        int has_exec = 0;
        while (fgets(line, sizeof line, f)) {   /* same rules as the launcher */
            char *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = line, *v = eq + 1;
            if (!strcmp(k, "title")) snprintf(x->title, sizeof x->title, "%s", v);
            else if (!strcmp(k, "desc")) snprintf(x->desc, sizeof x->desc, "%s", v);
            else if (!strcmp(k, "exec")) has_exec = 1;
            else if (!strcmp(k, "order")) x->order = atoi(v);
        }
        fclose(f);
        if (has_exec) n++;
    }
    if (d) closedir(d);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (a[j].order < a[i].order || (a[j].order == a[i].order && strcmp(a[j].title, a[i].title) < 0)) {
                app_t t = a[i]; a[i] = a[j]; a[j] = t;
            }
    return n;
}

/* ------------------------------------------------------------------ key queue */
static nck_dev kd = {.fd = -1};
static struct kev { long long at; int code, value; } kq[512];
static int kqn;

static int key_dev(void) {
    if (kd.fd >= 0) return 0;
    if (nck_open(&kd, keydev, 1) != 0) { logf_("%s: %s", keydev, strerror(errno)); return -1; }
    return 0;
}

static void key_step(long long now) {
    int i = 0;
    for (; i < kqn && kq[i].at <= now; i++) {
        if (key_dev() != 0) continue;
        if (nck_emit(&kd, kq[i].code, kq[i].value) != 0) { logf_("inject %d: %s", kq[i].code, strerror(errno)); nck_close(&kd); }
    }
    if (i) { memmove(kq, kq + i, sizeof *kq * (size_t)(kqn - i)); kqn -= i; }
}

static int key_queue(int code, int hold, int repeat, int delay) {
    if (kqn + 2 * repeat > (int)(sizeof kq / sizeof *kq)) return -1;
    long long t = now_ms();
    if (kqn && kq[kqn - 1].at + 40 > t) t = kq[kqn - 1].at + 40;   /* after what's queued */
    for (int r = 0; r < repeat; r++) {
        kq[kqn++] = (struct kev){t, code, 1};
        kq[kqn++] = (struct kev){t + hold, code, 0};
        t += hold + delay;
    }
    return 0;
}

/* ------------------------------------------------------------------ launch + screen */
static struct { int active; long long until, next; pid_t pid; unsigned long long st; int tries; } L;
static pid_t screen_waiter;

static void launch_step(long long now) {
    if (!L.active || now < L.next) return;
    char cmd[PATH_MAX];
    rpath(cmd, sizeof cmd, "ncmenu.cmd");
    if (access(cmd, F_OK) != 0) { L.active = 0; return; }   /* the session loop took it */
    if (now >= L.until) {
        L.active = 0;
        if (!session_alive()) { unlink(cmd); logf_("launch: ncplus did not start, command dropped"); }
        else logf_("launch: the app in front did not exit");
        return;
    }
    L.next = now + 1500;
    pid_t pid;
    unsigned long long st;
    if (fg_app(&pid, &st) != 0) return;
    if (pid != L.pid || st != L.st) { L.pid = pid; L.st = st; L.tries = 0; }
    if (L.tries >= 4) return;
    L.tries++;
    logf_("launch: SIGTERM to pid %d (try %d)", (int)pid, L.tries);
    kill(pid, SIGTERM);
}

/* target: "<N>/apps/<id>", "launcher" or "exit". Returns a note for the reply. */
static const char *launch(const char *target, int is_exit) {
    char cmd[PATH_MAX];
    rpath(cmd, sizeof cmd, "ncmenu.cmd");
    int running = session_alive();
    if (!running && is_exit) return "ncplus is not running";
    char line[PATH_MAX + 2];
    snprintf(line, sizeof line, "%s\n", target);
    if (write_small(cmd, line) != 0) return NULL;
    long long now = now_ms();
    L.active = 1;
    L.next = now;
    L.tries = 0;
    L.pid = 0;
    if (!running) {
        char sh[] = "/bin/sh", path[PATH_MAX + 16];
        snprintf(path, sizeof path, "%s/start.sh", N);
        char *argv[] = {sh, path, NULL};
        spawn(NULL, argv);
        L.until = now + 30000;
        logf_("launch %s: starting ncplus", target);
        return "starting ncplus";
    }
    L.until = now + 10000;
    logf_("launch %s", target);
    launch_step(now);
    return "switching";
}

static const char *screen_state(void) {
    char p[PATH_MAX], b[16];
    rpath(p, sizeof p, "ncscreen.state");
    if (read_small(p, b, sizeof b) == 0 && !strcmp(b, "off")) return "off";
    return "on";
}

static int screen_set(int on) {
    if (screen_waiter > 0) { kill(screen_waiter, SIGTERM); screen_waiter = 0; }
    char bin[PATH_MAX + 16], a1[] = "on", a2[] = "off", a3[] = "--wake";
    snprintf(bin, sizeof bin, "%s/ncscreen", tools);
    char *von[] = {bin, a1, NULL}, *voff[] = {bin, a2, a3, NULL};
    /* off --wake: stays running until any key (remote, or one sent through this API) turns it on */
    char log[PATH_MAX];
    rpath(log, sizeof log, "ncapi-screen.log");
    pid_t p = spawn(log, on ? von : voff);
    if (p < 0) return -1;
    if (!on) screen_waiter = p;
    return 0;
}

/* ------------------------------------------------------------------ HTTP */
typedef struct { int fd; size_t n; long long t0; char buf[REQMAX + 1]; } client;
static client cl[MAXC];

static const char *reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static void reply(int fd, int code, const char *body) {
    char h[512];
    size_t bl = strlen(body);
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n%sConnection: close\r\n\r\n",
                     code, reason(code), bl + 1, code == 401 ? "WWW-Authenticate: Bearer realm=\"ncapi\"\r\n" : "");
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    const char *parts[3] = {h, body, "\n"};
    size_t lens[3] = {(size_t)n, bl, 1};
    for (int i = 0; i < 3; i++) {
        const char *p = parts[i];
        size_t left = lens[i];
        while (left) {
            ssize_t w = send(fd, p, left, MSG_NOSIGNAL);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
            p += w;
            left -= (size_t)w;
        }
    }
}

static void reply_err(int fd, int code, const char *msg) {
    sbuf s = {0};
    sb_add(&s, "{\"error\":");
    sb_str(&s, msg);
    sb_add(&s, "}");
    reply(fd, code, s.b ? s.b : "{}");
    free(s.b);
}

static void settings_json(sbuf *s) {
    sb_add(s, "{\"settings\":{");
    for (int i = 0; i < ncs_ndefs; i++) {
        const ncs_def *d = &ncs_defs[i];
        sb_add(s, "%s\"%s\":{\"value\":%d,\"default\":%d,\"options\":[", i ? "," : "", d->key, ncs_get(conf, d), d->def);
        for (int k = 0; k < d->nopts; k++) sb_add(s, "%s%d", k ? "," : "", d->opts[k]);
        sb_add(s, "],\"title\":");
        sb_str(s, d->title);
        sb_add(s, ",\"description\":");
        sb_str(s, d->desc);
        sb_add(s, ",\"readonly\":%s}", d->api_readonly ? "true" : "false");
    }
    sb_add(s, "}}");
}

/* A stable id for clients (Home Assistant's unique id): the MAC of the interface with the default route,
 * else of the first interface that has one. "" if none. */
static void device_id(char *out, size_t n) {
    char ifn[IFNAMSIZ_ + 1] = "", line[256], p[PATH_MAX], mac[32];
    out[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    while (f && fgets(line, sizeof line, f)) {
        char nm[64];
        unsigned long dst;
        if (sscanf(line, "%63s %lx", nm, &dst) == 2 && dst == 0 && strlen(nm) <= IFNAMSIZ_) { snprintf(ifn, sizeof ifn, "%s", nm); break; }
    }
    if (f) fclose(f);
    if (*ifn) {
        snprintf(p, sizeof p, "/sys/class/net/%s/address", ifn);
        if (read_small(p, mac, sizeof mac) == 0 && strlen(mac) == 17 && strcmp(mac, "00:00:00:00:00:00")) { snprintf(out, n, "%s", mac); return; }
    }
    DIR *d = opendir("/sys/class/net");
    struct dirent *de;
    char best[64] = "";
    while (d && (de = readdir(d))) {   /* lowest name, so the choice doesn't depend on readdir order */
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "lo") || strlen(de->d_name) > IFNAMSIZ_) continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/address", de->d_name);
        if (read_small(p, mac, sizeof mac) != 0 || strlen(mac) != 17 || !strcmp(mac, "00:00:00:00:00:00")) continue;
        if (!*best || strcmp(de->d_name, best) < 0) { snprintf(best, sizeof best, "%s", de->d_name); snprintf(out, n, "%s", mac); }
    }
    if (d) closedir(d);
}

static void h_status(int fd) {
    sbuf s = {0};
    int running = session_alive();
    char cur[128] = "", title[64] = "", p[PATH_MAX], ver[64] = "";
    if (running) {
        rpath(p, sizeof p, "ncmenu.current");
        read_small(p, cur, sizeof cur);
        if (!*cur) snprintf(cur, sizeof cur, "launcher");
        if (!strcmp(cur, "launcher")) snprintf(title, sizeof title, "ncplus");
        else {
            app_t apps[MAXAPPS];
            int n = load_apps(apps, MAXAPPS);
            snprintf(title, sizeof title, "%s", cur);
            for (int i = 0; i < n; i++) if (!strcmp(apps[i].id, cur)) snprintf(title, sizeof title, "%s", apps[i].title);
        }
    } else {
        snprintf(cur, sizeof cur, "LG");
        snprintf(title, sizeof title, "LG");
    }
    snprintf(p, sizeof p, "%s/system/VERSION", N);
    read_small(p, ver, sizeof ver);
    struct sysinfo si;
    sysinfo(&si);
    sb_add(&s, "{\"netcast\":%s,\"app\":", running ? "true" : "false");
    sb_str(&s, cur);
    sb_add(&s, ",\"title\":");
    sb_str(&s, title);
    sb_add(&s, ",\"screen\":\"%s\",\"version\":", screen_state());
    sb_str(&s, *ver ? ver : "unknown");
    char id[32];
    device_id(id, sizeof id);
    sb_add(&s, ",\"id\":");
    sb_str(&s, id);
    sb_add(&s, ",\"uptime\":%ld,\"api_uptime\":%ld}", (long)si.uptime, (long)(time(NULL) - started));
    reply(fd, 200, s.b);
    free(s.b);
}

static void h_keys(int fd) {
    sbuf s = {0};
    sb_add(&s, "{\"keys\":[");
    for (int k = 0, first = 1; k < nck_nnames; k++) {
        if (!nck_names[k].label) continue;   /* aliases */
        sb_add(&s, "%s{\"name\":", first ? "" : ",");
        first = 0;
        sb_str(&s, nck_names[k].name);
        sb_add(&s, ",\"code\":%d,\"label\":", nck_names[k].code);
        sb_str(&s, nck_names[k].label);
        sb_add(&s, "}");
    }
    sb_add(&s, "]}");
    reply(fd, 200, s.b);
    free(s.b);
}

static void h_key(int fd, const jkv *kv, int n) {
    const jkv *k = jget(kv, n, "key"), *c = jget(kv, n, "code");
    int code = -1, hold = 80, repeat = 1, delay = 200;
    if (k && k->type == 's') {
        for (int i = 0; i < nck_nnames; i++) if (!strcasecmp(k->s, nck_names[i].name)) code = nck_names[i].code;
        if (code < 0) { reply_err(fd, 400, "unknown key name (GET /api/v1/keys lists them)"); return; }
    } else if (c) {
        if (jint(c, &code) != 0) { reply_err(fd, 400, "\"code\" must be an integer"); return; }
    } else { reply_err(fd, 400, "need \"key\" (a name) or \"code\""); return; }
    if (code <= 0 || code > NCK_MAX) { reply_err(fd, 400, "code out of range"); return; }
    if (nck_refused(code)) { reply_err(fd, 403, "refused: 897/898 are IN-STOP / power-only service keys"); return; }
    const jkv *h = jget(kv, n, "hold_ms"), *r = jget(kv, n, "repeat"), *dl = jget(kv, n, "delay_ms");
    if ((h && (jint(h, &hold) || hold < 20 || hold > 10000)) || (r && (jint(r, &repeat) || repeat < 1 || repeat > 100)) ||
        (dl && (jint(dl, &delay) || delay < 0 || delay > 10000))) {
        reply_err(fd, 400, "hold_ms 20..10000, repeat 1..100, delay_ms 0..10000");
        return;
    }
    if (key_dev() != 0) { reply_err(fd, 500, "cannot open the remote's input device"); return; }
    if (nck_declared(&kd, code) == 0) { reply_err(fd, 400, "the remote's input device does not declare this key"); return; }
    if (key_queue(code, hold, repeat, delay) != 0) { reply_err(fd, 503, "key queue full"); return; }
    sbuf s = {0};
    const char *nm = nck_name_of(code);
    sb_add(&s, "{\"ok\":true,\"code\":%d,\"name\":", code);
    sb_str(&s, nm ? nm : "");
    sb_add(&s, ",\"repeat\":%d}", repeat);
    reply(fd, 202, s.b);
    free(s.b);
    key_step(now_ms());
}

static void h_apps(int fd) {
    app_t apps[MAXAPPS];
    int n = load_apps(apps, MAXAPPS);
    sbuf s = {0};
    sb_add(&s, "{\"apps\":[");
    for (int i = 0; i < n; i++) {
        sb_add(&s, "%s{\"id\":", i ? "," : "");
        sb_str(&s, apps[i].id);
        sb_add(&s, ",\"title\":");
        sb_str(&s, apps[i].title);
        sb_add(&s, ",\"desc\":");
        sb_str(&s, apps[i].desc);
        sb_add(&s, ",\"order\":%d}", apps[i].order);
    }
    sb_add(&s, "]}");
    reply(fd, 200, s.b);
    free(s.b);
}

static void h_launch(int fd, const jkv *kv, int n) {
    const jkv *a = jget(kv, n, "app");
    if (!a || a->type != 's' || !*a->s) { reply_err(fd, 400, "need \"app\": an app id, \"launcher\" or \"exit\""); return; }
    char target[PATH_MAX + 80];
    int is_exit = !strcmp(a->s, "exit");
    if (is_exit || !strcmp(a->s, "launcher")) snprintf(target, sizeof target, "%s", a->s);
    else {
        app_t apps[MAXAPPS];
        int na = load_apps(apps, MAXAPPS), found = 0;
        for (int i = 0; i < na; i++) if (!strcmp(apps[i].id, a->s)) found = 1;
        if (!found) { reply_err(fd, 404, "no such app (GET /api/v1/apps lists them)"); return; }
        snprintf(target, sizeof target, "%s/apps/%s", N, a->s);
    }
    const char *note = launch(target, is_exit);
    if (!note) { reply_err(fd, 500, "cannot write the session command file"); return; }
    sbuf s = {0};
    sb_add(&s, "{\"ok\":true,\"app\":");
    sb_str(&s, a->s);
    sb_add(&s, ",\"note\":");
    sb_str(&s, note);
    sb_add(&s, "}");
    reply(fd, 202, s.b);
    free(s.b);
}

static void h_settings_put(int fd, const jkv *kv, int n) {
    if (n == 0) { reply_err(fd, 400, "empty object"); return; }
    int val[16];
    const ncs_def *def[16];
    if (n > 16) { reply_err(fd, 400, "too many keys"); return; }
    for (int i = 0; i < n; i++) {   /* check everything first: all or nothing */
        char m[160];
        def[i] = ncs_find(kv[i].k);
        if (!def[i]) { snprintf(m, sizeof m, "unknown setting %s", kv[i].k); reply_err(fd, 400, m); return; }
        if (def[i]->api_readonly) { snprintf(m, sizeof m, "%s can only be changed on the TV (Settings)", kv[i].k); reply_err(fd, 403, m); return; }
        if (jint(&kv[i], &val[i]) != 0 || !ncs_valid(def[i], val[i])) {
            int o = snprintf(m, sizeof m, "%s must be one of", kv[i].k);
            for (int k = 0; k < def[i]->nopts && o < (int)sizeof m - 8; k++) o += snprintf(m + o, sizeof m - (size_t)o, " %d", def[i]->opts[k]);
            reply_err(fd, 400, m);
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        if (ncs_get(conf, def[i]) == val[i]) continue;
        if (ncs_set(conf, def[i], val[i]) != 0) { reply_err(fd, 500, "cannot write netcast.conf"); return; }
        logf_("setting %s=%d", def[i]->key, val[i]);
        ncs_apply(def[i], val[i], tools);
    }
    sbuf s = {0};
    settings_json(&s);
    reply(fd, 200, s.b);
    free(s.b);
}

static void h_screen(int fd, const jkv *kv, int n) {
    const jkv *o = jget(kv, n, "on");
    if (!o || o->type != 'b') { reply_err(fd, 400, "need {\"on\": true|false}"); return; }
    if (screen_set((int)o->n) != 0) { reply_err(fd, 500, "cannot run ncscreen"); return; }
    logf_("screen %s", o->n ? "on" : "off");
    reply(fd, 202, o->n ? "{\"ok\":true,\"screen\":\"on\"}" : "{\"ok\":true,\"screen\":\"off\"}");
}

static const char *header(const char *req, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2] && !(p[2] == '\r' && p[3] == '\n'); p = strstr(p + 2, "\r\n")) {
        if (!strncasecmp(p + 2, name, nl) && p[2 + nl] == ':') {
            const char *v = p + 3 + nl;
            while (*v == ' ' || *v == '\t') v++;
            size_t l = strcspn(v, "\r\n");
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return out;
        }
    }
    return NULL;
}

/* 1 = request complete (handled + replied), 0 = need more data */
static int handle(client *c) {
    char *end = strstr(c->buf, "\r\n\r\n");
    if (!end) {
        if (c->n >= REQMAX) { reply_err(c->fd, 413, "request too large"); return 1; }
        return 0;
    }
    char clen[32];
    long bl = header(c->buf, "Content-Length", clen, sizeof clen) ? atol(clen) : 0;
    size_t hl = (size_t)(end + 4 - c->buf);
    if (bl < 0 || hl + (size_t)bl > REQMAX) { reply_err(c->fd, 413, "request too large"); return 1; }
    if (c->n < hl + (size_t)bl) return 0;
    char *body = end + 4;
    body[bl] = 0;

    char method[8], target[256];
    if (sscanf(c->buf, "%7s %255s", method, target) != 2) { reply_err(c->fd, 400, "bad request line"); return 1; }
    char *q = strchr(target, '?');
    if (q) *q = 0;
    char ah[256];
    if (!auth_ok(header(c->buf, "Authorization", ah, sizeof ah))) {
        logf_("401 %s %s", method, target);
        reply_err(c->fd, 401, "missing or wrong token (Authorization: Bearer <token>, see Settings > Remote API)");
        return 1;
    }
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), put = !strcmp(method, "PUT");
    jkv kv[24];
    int n = 0;
    if (post || put) {
        n = json_parse(body, kv, 24);
        if (n < 0) { reply_err(c->fd, 400, "body must be a flat JSON object"); return 1; }
    }
    const char *r = target;
    if (strncmp(r, "/api/v1/", 8)) { reply_err(c->fd, 404, "not found (the API is under /api/v1/)"); return 1; }
    r += 8;
#define ROUTE(path, cond, call) \
    if (!strcmp(r, path)) { if (cond) { call; } else reply_err(c->fd, 405, "method not allowed"); return 1; }
    ROUTE("status", get, h_status(c->fd));
    ROUTE("keys", get, h_keys(c->fd));
    ROUTE("key", post, h_key(c->fd, kv, n));
    ROUTE("apps", get, h_apps(c->fd));
    ROUTE("launch", post, h_launch(c->fd, kv, n));
    ROUTE("screen", post, h_screen(c->fd, kv, n));
    if (!strcmp(r, "settings")) {
        if (get) { sbuf s = {0}; settings_json(&s); reply(c->fd, 200, s.b); free(s.b); }
        else if (put) h_settings_put(c->fd, kv, n);
        else reply_err(c->fd, 405, "method not allowed");
        return 1;
    }
#undef ROUTE
    reply_err(c->fd, 404, "not found");
    return 1;
}

static int serve(void) {
    char v[16] = "";
    ncconf_get(conf, "api", "PORT", v, sizeof v);
    int port = *v ? atoi(v) : DEFAULT_PORT;
    if (port <= 0 || port > 65535) { logf_("bad [api] PORT %s", v); return 1; }
    ncconf_get(conf, "api", "TOKEN", token, sizeof token);
    if (!token_ok(token)) { logf_("no token in [api] TOKEN of %s (run: ncapi token ensure)", conf); return 1; }

    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int b = -1;
    for (int t = 0; t < 30 && (b = bind(s, (struct sockaddr *)&a, sizeof a)) != 0 && errno == EADDRINUSE; t++)
        usleep(100000);   /* a previous instance may still be exiting (Settings restarts us) */
    if (b != 0) {
        if (errno == EADDRINUSE) { logf_("port %d in use (already running?)", port); return 0; }
        logf_("bind %d: %s", port, strerror(errno));
        return 1;
    }
    listen(s, 16);
    fcntl(s, F_SETFL, O_NONBLOCK);
    started = time(NULL);
    logf_("listening on 0.0.0.0:%d (netcast %s, conf %s)", port, N, conf);
    for (int i = 0; i < MAXC; i++) cl[i].fd = -1;

    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
        long long now = now_ms();
        key_step(now);
        launch_step(now);
        if (screen_waiter > 0 && kill(screen_waiter, 0) != 0) screen_waiter = 0;

        struct pollfd p[MAXC + 1];
        int map[MAXC + 1], np = 0;
        p[np].fd = s; p[np].events = POLLIN; map[np++] = -1;
        long long next = now + 1000;
        for (int i = 0; i < MAXC; i++) {
            if (cl[i].fd < 0) continue;
            if (now - cl[i].t0 > CLIENT_MS) { close(cl[i].fd); cl[i].fd = -1; continue; }
            p[np].fd = cl[i].fd; p[np].events = POLLIN; map[np++] = i;
            if (cl[i].t0 + CLIENT_MS < next) next = cl[i].t0 + CLIENT_MS;
        }
        if (kqn && kq[0].at < next) next = kq[0].at;
        if (L.active && L.next < next) next = L.next;
        int to = (int)(next - now);
        if (to < 0) to = 0;
        int r = poll(p, (nfds_t)np, to);
        if (r < 0) { if (errno == EINTR) continue; logf_("poll: %s", strerror(errno)); return 1; }
        for (int k = 0; k < np && r > 0; k++) {
            if (!p[k].revents) continue;
            if (map[k] < 0) {
                for (;;) {
                    int fd = accept4(s, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (fd < 0) break;
                    int slot = -1;
                    for (int i = 0; i < MAXC; i++) if (cl[i].fd < 0) { slot = i; break; }
                    if (slot < 0) { reply_err(fd, 503, "busy"); close(fd); continue; }
                    cl[slot].fd = fd;
                    cl[slot].n = 0;
                    cl[slot].t0 = now_ms();
                }
                continue;
            }
            client *c = &cl[map[k]];
            ssize_t got = recv(c->fd, c->buf + c->n, REQMAX - c->n, 0);
            if (got <= 0) {
                if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                close(c->fd); c->fd = -1;
                continue;
            }
            c->n += (size_t)got;
            c->buf[c->n] = 0;
            if (handle(c)) { close(c->fd); c->fd = -1; }
        }
    }
}

int main(int argc, char **argv) {
    const char *nd = getenv("NETCAST_DIR");
    if (nd && *nd) snprintf(N, sizeof N, "%s", nd);
    else {
        char exe[PATH_MAX];
        ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (l <= 0) { perror("readlink /proc/self/exe"); return 1; }
        exe[l] = 0;
        char *sl = strrchr(exe, '/');
        if (sl) *sl = 0;                      /* .../netcast/tools */
        sl = strrchr(exe, '/');
        if (sl) *sl = 0;                      /* .../netcast */
        snprintf(N, sizeof N, "%s", exe);
    }
    const char *nc = getenv("NETCAST_CONF");
    if (nc && *nc) snprintf(conf, sizeof conf, "%s", nc);
    else {
        snprintf(conf, sizeof conf, "%s", N);
        char *sl = strrchr(conf, '/');
        if (sl) snprintf(sl, sizeof conf - (size_t)(sl - conf), "/netcast.conf");
    }
    snprintf(tools, sizeof tools, "%s/tools", N);
    const char *kdv = getenv("NC_KEYDEV");
    snprintf(keydev, sizeof keydev, "%s", kdv && *kdv ? kdv : NCK_DEFAULT_DEV);

    if (argc >= 2 && !strcmp(argv[1], "token")) return cmd_token(argc >= 3 ? argv[2] : "");
    if (argc >= 2) { fprintf(stderr, "usage: ncapi [token show|ensure|new]\n"); return 2; }
    return serve();
}
