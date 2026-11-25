/*
 * smbgw: read-only HTTP gateway to SMB shares (and the USB stick) for the other ncplus apps.
 *
 *   smbgw <shares.conf> [port]      listens on 127.0.0.1 only (default port 8445)
 *
 *   GET /s/<entry>/<share>/<path>   file or directory on share <share> of shares.conf entry <entry>
 *   GET /l/<path>                   local file under /mnt/usb
 *
 * Directories are returned as an HTML index of links (subdirectories end in '/'), which is the format
 * Photos already reads; files support HEAD and byte ranges (GStreamer/Music seek with them).
 * One SMB connection per share is kept open and shared under a mutex; it's reopened once after an error.
 */
#define _GNU_SOURCE
#include "smbcommon.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#define MAXE 64
static smb_entry ents[MAXE];
static int nents, gen;
static const char *conf_path;
static time_t conf_mtime;
static pthread_mutex_t ents_mu = PTHREAD_MUTEX_INITIALIZER;

/* Re-read shares.conf when it changed; connections of older generations are no longer used. */
static void maybe_reload(void) {
    struct stat st;
    if (stat(conf_path, &st) != 0) return;
    pthread_mutex_lock(&ents_mu);
    if (st.st_mtime != conf_mtime) {
        conf_mtime = st.st_mtime;
        nents = smb_load_conf(conf_path, ents, MAXE);
        gen++;
        fprintf(stderr, "smbgw: loaded %d share entries\n", nents);
    }
    pthread_mutex_unlock(&ents_mu);
}

typedef struct conn {
    int gen, entry;
    smb_entry e;
    char share[128];
    struct smb2_context *c;
    pthread_mutex_t mu;
    struct conn *next;
} conn;
static conn *conns;
static pthread_mutex_t conns_mu = PTHREAD_MUTEX_INITIALIZER;

static conn *get_conn(int entry, const char *share) {
    pthread_mutex_lock(&ents_mu);
    if (entry < 0 || entry >= nents) { pthread_mutex_unlock(&ents_mu); return NULL; }
    smb_entry e = ents[entry];
    int g = gen;
    pthread_mutex_unlock(&ents_mu);
    pthread_mutex_lock(&conns_mu);
    conn *k = conns;
    for (; k; k = k->next) if (k->gen == g && k->entry == entry && !strcmp(k->share, share)) break;
    if (!k) {
        k = calloc(1, sizeof *k);
        k->gen = g;
        k->entry = entry;
        k->e = e;
        snprintf(k->share, sizeof k->share, "%s", share);
        pthread_mutex_init(&k->mu, NULL);
        k->next = conns;
        conns = k;
    }
    pthread_mutex_unlock(&conns_mu);
    return k;
}

/* Lock the connection, (re)connecting if needed. */
static int lock_conn(conn *k, int reconnect) {
    pthread_mutex_lock(&k->mu);
    if (reconnect && k->c) { smb2_destroy_context(k->c); k->c = NULL; }
    if (!k->c) {
        char err[300] = "";
        k->c = smb_connect(&k->e, k->share, err, sizeof err);
        if (!k->c) { fprintf(stderr, "smbgw: %s\n", err); pthread_mutex_unlock(&k->mu); return -1; }
    }
    return 0;
}

static int sendall(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void reply_status(int fd, int code, const char *msg) {
    char b[300];
    int n = snprintf(b, sizeof b, "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s\n",
                     code, msg, strlen(msg) + 1, msg);
    sendall(fd, b, (size_t)n);
}

static const char *mime(const char *p) {
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    e++;
    static const char *m[][2] = {
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"}, {"gif", "image/gif"}, {"bmp", "image/bmp"},
        {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mkv", "video/x-matroska"}, {"ts", "video/mp2t"}, {"m2ts", "video/mp2t"},
        {"mpg", "video/mpeg"}, {"mpeg", "video/mpeg"}, {"avi", "video/x-msvideo"}, {"mov", "video/quicktime"}, {"flv", "video/x-flv"},
        {"flac", "audio/flac"}, {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"m3u", "audio/x-mpegurl"}, {"txt", "text/plain"},
    };
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (!strcasecmp(e, m[i][0])) return m[i][1];
    return "application/octet-stream";
}

/* ---- directory listings ---- */
typedef struct { char **names; int *dirs; int n, cap; } listing;
static void list_add(listing *l, const char *name, int dir) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->names = realloc(l->names, sizeof *l->names * (size_t)l->cap);
        l->dirs = realloc(l->dirs, sizeof *l->dirs * (size_t)l->cap);
    }
    l->names[l->n] = strdup(name);
    l->dirs[l->n++] = dir;
}
static void send_listing(int fd, listing *l, const char *title, int head) {
    size_t cap = 4096 + (size_t)l->n * 700, len = 0;
    char *b = malloc(cap);
    len += (size_t)snprintf(b + len, cap - len, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title></head><body><ul>\n", title);
    for (int i = 0; i < l->n; i++) {
        char enc[1600];
        url_encode_path(l->names[i], enc, sizeof enc);
        len += (size_t)snprintf(b + len, cap - len, "<li><a href=\"%s%s\">%s%s</a></li>\n",
                                enc, l->dirs[i] ? "/" : "", l->names[i], l->dirs[i] ? "/" : "");
        free(l->names[i]);
    }
    len += (size_t)snprintf(b + len, cap - len, "</ul></body></html>\n");
    char h[256];
    int hn = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len);
    sendall(fd, h, (size_t)hn);
    if (!head) sendall(fd, b, len);
    free(b); free(l->names); free(l->dirs);
}

static int parse_range(const char *range, uint64_t size, uint64_t *a, uint64_t *b) {
    *a = 0; *b = size ? size - 1 : 0;
    if (!range) return 0;
    unsigned long long x = 0, y = 0;
    if (sscanf(range, "bytes=%llu-%llu", &x, &y) == 2) { *a = x; *b = y < size ? y : size - 1; }
    else if (sscanf(range, "bytes=%llu-", &x) == 1) *a = x;
    else if (sscanf(range, "bytes=-%llu", &y) == 1) *a = y < size ? size - y : 0;
    else return 0;
    return *a < size ? 1 : -1;
}

static void send_file_header(int fd, int partial, uint64_t a, uint64_t b, uint64_t size, const char *path) {
    char h[512];
    int n;
    if (partial)
        n = snprintf(h, sizeof h, "HTTP/1.0 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                     "Content-Range: bytes %llu-%llu/%llu\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
                     mime(path), (unsigned long long)a, (unsigned long long)b, (unsigned long long)size, (unsigned long long)(b - a + 1));
    else
        n = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
                     mime(path), (unsigned long long)size);
    sendall(fd, h, (size_t)n);
}

/* ---- local files (USB stick only) ---- */
static void serve_local(int fd, const char *rel, const char *range, int head) {
    char want[PATH_MAX], real[PATH_MAX];
    snprintf(want, sizeof want, "/mnt/usb/%s", rel);
    if (!realpath(want, real) || strncmp(real, "/mnt/usb/", 9)) { reply_status(fd, 404, "not found"); return; }
    struct stat st;
    if (stat(real, &st) != 0) { reply_status(fd, 404, "not found"); return; }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(real);
        if (!d) { reply_status(fd, 403, "cannot read directory"); return; }
        listing l = {0};
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", real, e->d_name);
            struct stat s2;
            list_add(&l, e->d_name, stat(p, &s2) == 0 && S_ISDIR(s2.st_mode));
        }
        closedir(d);
        send_listing(fd, &l, rel, head);
        return;
    }
    int f = open(real, O_RDONLY);
    if (f < 0) { reply_status(fd, 403, "cannot open"); return; }
    uint64_t a, b, size = (uint64_t)st.st_size;
    int pr = parse_range(range, size, &a, &b);
    if (pr < 0) { reply_status(fd, 416, "range not satisfiable"); close(f); return; }
    send_file_header(fd, pr, a, b, size, real);
    if (!head && size) {
        const size_t cap = 256 * 1024;
        char *buf = malloc(cap);
        for (uint64_t off = a; buf && off <= b; ) {
            size_t want_n = (size_t)(b - off + 1 < cap ? b - off + 1 : cap);
            ssize_t r = pread(f, buf, want_n, (off_t)off);
            if (r <= 0 || sendall(fd, buf, (size_t)r)) break;
            off += (uint64_t)r;
        }
        free(buf);
    }
    close(f);
}

/* ---- SMB ---- */
static void serve_smb(int fd, int entry, const char *share, const char *path, const char *range, int head) {
    conn *k = get_conn(entry, share);
    if (!k) { reply_status(fd, 404, "no such share entry"); return; }
    struct smb2_stat_64 st;
    int ok = -1;
    for (int attempt = 0; attempt < 2 && ok != 0; attempt++) {
        if (lock_conn(k, attempt) != 0) { reply_status(fd, 502, "cannot connect to the SMB server"); return; }
        ok = smb2_stat(k->c, path, &st);
        if (ok != 0) {
            /* ENOENT, or still failing on a fresh connection: treat as not found */
            int nf = ok == -ENOENT || attempt == 1;
            pthread_mutex_unlock(&k->mu);
            if (nf) { reply_status(fd, 404, "not found"); return; }
        }
    }
    if (ok != 0) { reply_status(fd, 502, "SMB error"); return; }
    /* k->mu is held here */
    if (st.smb2_type == SMB2_TYPE_DIRECTORY) {
        struct smb2dir *d = smb2_opendir(k->c, path);
        listing l = {0};
        if (d) {
            struct smb2dirent *e;
            while ((e = smb2_readdir(k->c, d))) {
                if (e->name[0] == '.') continue;
                list_add(&l, e->name, e->st.smb2_type == SMB2_TYPE_DIRECTORY);
            }
            smb2_closedir(k->c, d);
        }
        pthread_mutex_unlock(&k->mu);
        if (!d) { reply_status(fd, 403, "cannot read directory"); return; }
        send_listing(fd, &l, path, head);
        return;
    }
    struct smb2fh *fh = smb2_open(k->c, path, O_RDONLY);
    uint32_t maxr = smb2_get_max_read_size(k->c);
    pthread_mutex_unlock(&k->mu);
    if (!fh) { reply_status(fd, 403, "cannot open"); return; }
    uint64_t a, b, size = st.smb2_size;
    int pr = parse_range(range, size, &a, &b);
    if (pr < 0) {
        reply_status(fd, 416, "range not satisfiable");
    } else {
        send_file_header(fd, pr, a, b, size, path);
        if (!head && size) {
            const size_t cap = 256 * 1024;
            uint8_t *buf = malloc(cap);
            size_t chunk = maxr && maxr < cap ? maxr : cap;
            for (uint64_t off = a; buf && off <= b; ) {
                uint32_t want_n = (uint32_t)(b - off + 1 < chunk ? b - off + 1 : chunk);
                pthread_mutex_lock(&k->mu);
                int r = k->c ? smb2_pread(k->c, fh, buf, want_n, off) : -EIO;
                pthread_mutex_unlock(&k->mu);
                if (r <= 0 || sendall(fd, buf, (size_t)r)) break;
                off += (uint64_t)r;
            }
            free(buf);
        }
    }
    pthread_mutex_lock(&k->mu);
    if (k->c) smb2_close(k->c, fh);
    pthread_mutex_unlock(&k->mu);
}

static void *client(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[8192];
    size_t n = 0;
    while (n < sizeof req - 1) {          /* read headers */
        ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[n] = 0;
    char method[8], target[4096];
    if (sscanf(req, "%7s %4095s", method, target) != 2) { close(fd); return NULL; }
    int head = !strcmp(method, "HEAD");
    if (!head && strcmp(method, "GET")) { reply_status(fd, 405, "read-only"); close(fd); return NULL; }
    char *range = NULL, *rh = strcasestr(req, "\r\nRange:");
    char rbuf[128];
    if (rh) {
        rh += 8;
        while (*rh == ' ') rh++;
        size_t l = strcspn(rh, "\r\n");
        snprintf(rbuf, sizeof rbuf, "%.*s", (int)l, rh);
        range = rbuf;
    }
    char *q = strchr(target, '?');
    if (q) *q = 0;
    url_decode(target);
    maybe_reload();
    size_t tl = strlen(target);
    if (strstr(target, "/../") || (tl >= 3 && !strcmp(target + tl - 3, "/.."))) { reply_status(fd, 403, "no"); close(fd); return NULL; }
    if (!strncmp(target, "/l/", 3)) {
        serve_local(fd, target + 3, range, head);
    } else if (!strncmp(target, "/s/", 3)) {
        char *p = target + 3, *s1 = strchr(p, '/');
        if (!s1) { reply_status(fd, 404, "bad path"); close(fd); return NULL; }
        *s1 = 0;
        int entry = atoi(p);
        char *share = s1 + 1, *s2 = strchr(share, '/');
        char *path = (char *)"";
        if (s2) { *s2 = 0; path = s2 + 1; }
        size_t l = strlen(path);
        while (l && path[l - 1] == '/') path[--l] = 0;
        serve_smb(fd, entry, share, path, range, head);
    } else {
        reply_status(fd, 404, "use /s/<entry>/<share>/<path> or /l/<path>");
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: smbgw <shares.conf> [port]\n"); return 2; }
    int port = argc > 2 ? atoi(argv[2]) : 8445;
    conf_path = argv[1];
    maybe_reload();
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno == EADDRINUSE) { fprintf(stderr, "smbgw: already running on %d\n", port); return 0; }
        perror("bind");
        return 1;
    }
    listen(s, 16);
    fprintf(stderr, "smbgw: %d share entries, http://127.0.0.1:%d/\n", nents, port);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        pthread_t t;
        if (pthread_create(&t, &at, client, (void *)(intptr_t)c) != 0) close(c);
    }
    return 0;
}
