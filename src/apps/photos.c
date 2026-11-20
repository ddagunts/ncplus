/*
 * photos: browse and view photos from an HTTP server on the NetCast TV.
 *
 * Works with plain directory listings (python3 -m http.server, nginx autoindex, Apache, ...):
 * folders are albums, images (jpg/jpeg/png/gif/bmp) open full-screen.
 *
 * Usage: photos <config-file>        config:  url=http://192.168.1.10:8000/
 *
 * Remote:  browser  UP/DOWN select, OK open, CH+ page, BACK back (BACK at the top exits)
 *          viewer   LEFT/RIGHT prev/next, OK slideshow on/off, UP info, BACK back to the list
 */
#define _GNU_SOURCE
#include "nc.h"
#include "ncgfx.h"
#include "nctext.h"
#include <GLES2/gl2.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define SCREEN_W 1280
#define SCREEN_H 720
#define MAX_BODY (64 * 1024 * 1024)
#define MAX_PIXELS (48u * 1000 * 1000)

/* ------------------------------------------------------------------ HTTP */
typedef struct { char *data; size_t len; char ctype[128]; int status; char err[160]; char final_url[1024]; } http_resp;

static int parse_url(const char *url, char *host, size_t hl, char *port, size_t pl, char *path, size_t ptl) {
    if (strncasecmp(url, "http://", 7) != 0) return -1;
    const char *h = url + 7, *p = strchr(h, '/');
    size_t hlen = p ? (size_t)(p - h) : strlen(h);
    char hp[300];
    if (hlen >= sizeof hp) return -1;
    memcpy(hp, h, hlen); hp[hlen] = 0;
    char *colon = strrchr(hp, ':');
    if (colon) { *colon = 0; snprintf(port, pl, "%s", colon + 1); } else snprintf(port, pl, "80");
    snprintf(host, hl, "%s", hp);
    snprintf(path, ptl, "%s", p ? p : "/");
    return 0;
}

static int connect_timeout(const char *host, const char *port, int ms, char *err, size_t el) {
    struct addrinfo hints = {0}, *ai = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, port, &hints, &ai);
    if (r != 0 || !ai) { snprintf(err, el, "can't resolve %s", host); return -1; }
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (r < 0 && errno != EINPROGRESS) { snprintf(err, el, "connect: %s", strerror(errno)); close(fd); return -1; }
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, ms) <= 0) { snprintf(err, el, "connection to %s:%s timed out", host, port); close(fd); return -1; }
    int soerr = 0; socklen_t sl = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr) { snprintf(err, el, "connect %s:%s: %s", host, port, strerror(soerr)); close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static size_t dechunk(char *b, size_t n) {   /* in-place Transfer-Encoding: chunked decode */
    size_t in = 0, out = 0;
    while (in < n) {
        char *end;
        unsigned long sz = strtoul(b + in, &end, 16);
        char *nl = memchr(b + in, '\n', n - in);
        if (!nl) break;
        in = (size_t)(nl - b) + 1;
        if (sz == 0 || in + sz > n) break;
        memmove(b + out, b + in, sz);
        out += sz; in += sz;
        if (in < n && b[in] == '\r') in++;
        if (in < n && b[in] == '\n') in++;
        (void)end;
    }
    return out;
}

static int http_get(const char *url0, http_resp *r) {
    char url[1024];
    snprintf(url, sizeof url, "%s", url0);
    memset(r, 0, sizeof *r);
    for (int redirects = 0; redirects < 4; redirects++) {
        char host[256], port[16], path[1024];
        if (parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path)) {
            snprintf(r->err, sizeof r->err, "unsupported URL (http:// only): %.100s", url);
            return -1;
        }
        int fd = connect_timeout(host, port, 5000, r->err, sizeof r->err);
        if (fd < 0) return -1;
        char req[1536];
        int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: NetCastPhotos/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host);
        if (write(fd, req, n) != n) { snprintf(r->err, sizeof r->err, "send failed"); close(fd); return -1; }
        size_t cap = 1 << 16, len = 0;
        char *buf = malloc(cap);
        for (;;) {
            if (len + 65536 > cap) {
                if (cap >= MAX_BODY) { snprintf(r->err, sizeof r->err, "file too large"); free(buf); close(fd); return -1; }
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) { snprintf(r->err, sizeof r->err, "out of memory"); free(buf); close(fd); return -1; }
                buf = nb;
            }
            ssize_t k = read(fd, buf + len, cap - len - 1);
            if (k < 0 && errno == EINTR) continue;
            if (k < 0) { snprintf(r->err, sizeof r->err, "read: %s", strerror(errno)); free(buf); close(fd); return -1; }
            if (k == 0) break;
            len += (size_t)k;
        }
        close(fd);
        buf[len] = 0;
        char *hend = strstr(buf, "\r\n\r\n");
        size_t hskip = 4;
        if (!hend) { hend = strstr(buf, "\n\n"); hskip = 2; }
        if (!hend) { snprintf(r->err, sizeof r->err, "bad HTTP response"); free(buf); return -1; }
        *hend = 0;
        int status = 0;
        sscanf(buf, "HTTP/%*s %d", &status);
        char location[1024] = "";
        int chunked = 0;
        for (char *line = strchr(buf, '\n'); line; line = strchr(line + 1, '\n')) {
            char *l = line + 1;
            if (!strncasecmp(l, "Content-Type:", 13)) { sscanf(l + 13, " %127[^\r\n;]", r->ctype); }
            else if (!strncasecmp(l, "Location:", 9)) { sscanf(l + 9, " %1023[^\r\n]", location); }
            else if (!strncasecmp(l, "Transfer-Encoding:", 18) && strcasestr(l, "chunked")) chunked = 1;
        }
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location[0]) {
            free(buf);
            if (location[0] == '/') snprintf(url, sizeof url, "http://%s:%s%s", host, port, location);
            else snprintf(url, sizeof url, "%s", location);
            continue;
        }
        r->status = status;
        size_t body = (size_t)(hend - buf) + hskip;
        r->len = len - body;
        memmove(buf, buf + body, r->len);
        if (chunked) r->len = dechunk(buf, r->len);
        buf[r->len] = 0;
        r->data = buf;
        snprintf(r->final_url, sizeof r->final_url, "%s", url);
        if (status != 200) { snprintf(r->err, sizeof r->err, "HTTP %d", status); return -1; }
        return 0;
    }
    snprintf(r->err, sizeof r->err, "too many redirects");
    return -1;
}

/* ------------------------------------------------------------ listing */
typedef struct { char url[1024]; char name[256]; int is_dir; GLuint ttex; int tw, th, tstate; } entry;  /* tstate: 0 none 1 queued 2 ready 3 failed */
static entry *entries;
static int nentries;
static char cur_url[1024];

static int is_image(const char *s) {
    const char *dot = strrchr(s, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") ||
           !strcasecmp(dot, ".gif") || !strcasecmp(dot, ".bmp");
}

static void url_decode(const char *in, char *out, size_t ol) {
    size_t o = 0;
    for (; *in && o + 1 < ol; in++) {
        if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char h[3] = {in[1], in[2], 0};
            out[o++] = (char)strtol(h, 0, 16);
            in += 2;
        } else out[o++] = *in == '+' ? ' ' : *in;
    }
    out[o] = 0;
}

static void resolve(const char *base, const char *href, char *out, size_t ol) {
    if (!strncasecmp(href, "http://", 7)) { snprintf(out, ol, "%s", href); return; }
    char host[256], port[16], path[1024];
    parse_url(base, host, sizeof host, port, sizeof port, path, sizeof path);
    if (href[0] == '/') { snprintf(out, ol, "http://%s:%s%s", host, port, href); return; }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    snprintf(out, ol, "http://%s:%s%s%s", host, port, dir, href);
}

static int cmp_entry(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void parse_listing(const char *base, const char *html) {
    free(entries);
    entries = NULL;
    nentries = 0;
    int cap = 0;
    for (const char *p = html; (p = strcasestr(p, "href=")); ) {
        p += 5;
        char q = *p;
        if (q != '"' && q != '\'') continue;
        const char *s = ++p, *e = strchr(s, q);
        if (!e || e - s >= 1000) continue;
        char href[1024];
        memcpy(href, s, (size_t)(e - s)); href[e - s] = 0;
        p = e;
        if (!href[0] || href[0] == '?' || href[0] == '#' || !strcmp(href, "../") || !strcmp(href, "./") ||
            !strncasecmp(href, "javascript:", 11) || !strncasecmp(href, "mailto:", 7) || strstr(href, "://") == href + 5 /* https */)
            continue;
        size_t hl = strlen(href);
        int dir = href[hl - 1] == '/';
        if (!dir && !is_image(href)) continue;
        char full[1024];
        resolve(base, href, full, sizeof full);
        if (dir && strlen(full) <= strlen(base) && !strncmp(base, full, strlen(full))) continue;   /* parent links */
        int dup = 0;
        for (int i = 0; i < nentries && !dup; i++) dup = !strcmp(entries[i].url, full);
        if (dup) continue;
        if (nentries == cap) { cap = cap ? cap * 2 : 64; entries = realloc(entries, sizeof *entries * cap); }
        entry *en = &entries[nentries++];
        memset(en, 0, sizeof *en);
        snprintf(en->url, sizeof en->url, "%s", full);
        char nm[1024];
        snprintf(nm, sizeof nm, "%s", href);
        if (dir) nm[strlen(nm) - 1] = 0;
        const char *last = strrchr(nm, '/');
        url_decode(last ? last + 1 : nm, en->name, sizeof en->name);
        en->is_dir = dir;
    }
    qsort(entries, nentries, sizeof *entries, cmp_entry);
}

/* -------------------------------------------------- image decode + scale */
typedef struct { uint8_t *rgba; int w, h; char err[160]; } image;

/* JPEG APP1 Exif: orientation (tag 0x0112) and embedded thumbnail (IFD1 tags 0x0201/0x0202). */
static void exif_parse(const uint8_t *d, size_t n, int *orient, size_t *toff, size_t *tlen) {
    *orient = 1; *toff = *tlen = 0;
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return;
    size_t i = 2;
    while (i + 4 < n && d[i] == 0xFF) {
        int marker = d[i + 1];
        size_t seg = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (marker == 0xE1 && i + 10 < n && !memcmp(d + i + 4, "Exif\0\0", 6)) {
            const uint8_t *t = d + i + 10;
            size_t tl = seg > 8 ? seg - 8 : 0;
            if (tl < 8 || i + 10 + tl > n) return;
            int le = t[0] == 'I';
#define U16(p) (le ? (p)[0] | (p)[1] << 8 : (p)[0] << 8 | (p)[1])
#define U32(p) (le ? (uint32_t)((p)[0] | (p)[1] << 8 | (p)[2] << 16 | (uint32_t)(p)[3] << 24) : (uint32_t)((uint32_t)(p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3]))
            uint32_t ifd = U32(t + 4);
            for (int which = 0; which < 2 && ifd && ifd + 2 <= tl; which++) {
                int cnt = U16(t + ifd);
                uint32_t joff = 0, jlen = 0;
                for (int k = 0; k < cnt; k++) {
                    size_t off = ifd + 2 + (size_t)k * 12;
                    if (off + 12 > tl) break;
                    int tag = U16(t + off);
                    if (which == 0 && tag == 0x0112) *orient = U16(t + off + 8);
                    if (which == 1 && tag == 0x0201) joff = U32(t + off + 8);
                    if (which == 1 && tag == 0x0202) jlen = U32(t + off + 8);
                }
                if (which == 1 && joff && jlen && joff + jlen <= tl) { *toff = (size_t)(t - d) + joff; *tlen = jlen; }
                size_t nxt = ifd + 2 + (size_t)cnt * 12;
                ifd = nxt + 4 <= tl ? U32(t + nxt) : 0;
            }
#undef U16
#undef U32
            return;
        }
        if (marker == 0xDA) break;
        i += 2 + seg;
    }
}

/* box-filter downscale RGBA src (sw x sh) to fit within maxw x maxh */
static uint8_t *downscale(const uint8_t *src, int sw, int sh, int maxw, int maxh, int *ow, int *oh) {
    double s = 1.0;
    if (sw > maxw) s = (double)maxw / sw;
    if (sh * s > maxh) s = (double)maxh / sh;
    int dw = (int)(sw * s + 0.5), dh = (int)(sh * s + 0.5);
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((double)y * sh / dh), y1 = (int)((double)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((double)x * sw / dw), x1 = (int)((double)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0, 0, 0, 0}, c = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *row = src + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, row += 4, c++) {
                    acc[0] += row[0]; acc[1] += row[1]; acc[2] += row[2]; acc[3] += row[3];
                }
            }
            uint8_t *o = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++) o[k] = (uint8_t)(acc[k] / c);
        }
    }
    *ow = dw; *oh = dh;
    return dst;
}

static uint8_t *rotate(const uint8_t *src, int w, int h, int orient, int *ow, int *oh) {
    int rw = (orient == 6 || orient == 8) ? h : w, rh = (orient == 6 || orient == 8) ? w : h;
    uint8_t *dst = malloc((size_t)rw * rh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int nx, ny;
            if (orient == 3) { nx = w - 1 - x; ny = h - 1 - y; }
            else if (orient == 6) { nx = h - 1 - y; ny = x; }          /* rotate 90 CW */
            else { nx = y; ny = w - 1 - x; }                           /* 8: rotate 90 CCW */
            memcpy(dst + ((size_t)ny * rw + nx) * 4, src + ((size_t)y * w + x) * 4, 4);
        }
    *ow = rw; *oh = rh;
    return dst;
}

static void load_image(const char *url, int boxw, int boxh, int use_thumb, image *im) {
    memset(im, 0, sizeof *im);
    http_resp r;
    if (http_get(url, &r) != 0) { snprintf(im->err, sizeof im->err, "%s", r.err); free(r.data); return; }
    int orient; size_t toff, tlen;
    exif_parse((uint8_t *)r.data, r.len, &orient, &toff, &tlen);
    const uint8_t *src = (uint8_t *)r.data;
    size_t srclen = r.len;
    if (use_thumb && tlen > 100) { src += toff; srclen = tlen; }   /* tiny embedded JPEG: fast */
    int w, h, n;
    if (!stbi_info_from_memory(src, (int)srclen, &w, &h, &n)) {
        if (src != (uint8_t *)r.data) { src = (uint8_t *)r.data; srclen = r.len; }
        if (!stbi_info_from_memory(src, (int)srclen, &w, &h, &n)) {
            snprintf(im->err, sizeof im->err, "not a supported image (%s)", stbi_failure_reason());
            free(r.data); return;
        }
    }
    if ((unsigned)w * (unsigned)h > MAX_PIXELS) { snprintf(im->err, sizeof im->err, "image too large (%dx%d)", w, h); free(r.data); return; }
    uint8_t *px = stbi_load_from_memory(src, (int)srclen, &w, &h, &n, 4);
    free(r.data);
    if (!px) { snprintf(im->err, sizeof im->err, "decode failed (%s)", stbi_failure_reason()); return; }
    int maxw = boxw, maxh = boxh;
    if (orient == 6 || orient == 8) { maxw = boxh; maxh = boxw; }
    int dw = w, dh = h;
    uint8_t *small = NULL;
    if (w > maxw || h > maxh) small = downscale(px, w, h, maxw, maxh, &dw, &dh);
    else if ((small = malloc((size_t)w * h * 4))) memcpy(small, px, (size_t)w * h * 4);
    stbi_image_free(px);
    if (!small) { snprintf(im->err, sizeof im->err, "out of memory"); return; }
    if (orient == 3 || orient == 6 || orient == 8) {
        uint8_t *rot = rotate(small, dw, dh, orient, &dw, &dh);
        free(small);
        small = rot;
        if (!small) { snprintf(im->err, sizeof im->err, "out of memory"); return; }
    }
    im->rgba = small; im->w = dw; im->h = dh;
}

/* ---------------------------------------------------- loader thread
 * One worker, one prioritised queue: the photo being viewed, then the neighbours (prefetch),
 * then thumbnails for the visible grid. GL work (texture upload) stays on the main thread. */
enum { JOB_CUR = 0, JOB_PREFETCH = 1, JOB_THUMB = 2 };
typedef struct { int type, idx, gen; char url[1024]; } job;
typedef struct { int type, idx, gen; char url[1024]; image im; } result;
#define QMAX 96
static job q[QMAX];
static int qn;
static result res[QMAX];
static int rn;
static char working_url[1024];
static int working_type = -1;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

#define THUMB_W 192
#define THUMB_H 144

static void *loader(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (qn == 0) pthread_cond_wait(&cv, &lk);
        int best = 0;
        for (int i = 1; i < qn; i++) if (q[i].type < q[best].type) best = i;
        job j = q[best];
        memmove(&q[best], &q[best + 1], sizeof *q * (qn - best - 1));
        qn--;
        snprintf(working_url, sizeof working_url, "%s", j.url);
        working_type = j.type;
        pthread_mutex_unlock(&lk);

        image im;
        if (j.type == JOB_THUMB) load_image(j.url, THUMB_W, THUMB_H, 1, &im);
        else load_image(j.url, SCREEN_W, SCREEN_H, 0, &im);

        pthread_mutex_lock(&lk);
        working_url[0] = 0;
        working_type = -1;
        if (rn < QMAX) {
            result *r = &res[rn++];
            r->type = j.type; r->idx = j.idx; r->gen = j.gen; r->im = im;
            snprintf(r->url, sizeof r->url, "%s", j.url);
        } else free(im.rgba);
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

/* caller holds lk */
static int queued_locked(int type, const char *url) {
    if (working_type >= 0 && !strcmp(working_url, url) && (working_type == type || type != JOB_THUMB) && working_type != JOB_THUMB) return 1;
    if (type == JOB_THUMB && working_type == JOB_THUMB && !strcmp(working_url, url)) return 1;
    for (int i = 0; i < qn; i++)
        if (!strcmp(q[i].url, url) && ((q[i].type == JOB_THUMB) == (type == JOB_THUMB))) return 1;
    return 0;
}

static void enqueue(int type, const char *url, int idx, int gen) {
    pthread_mutex_lock(&lk);
    if (!queued_locked(type, url) && qn < QMAX) {
        job *j = &q[qn++];
        j->type = type; j->idx = idx; j->gen = gen;
        snprintf(j->url, sizeof j->url, "%s", url);
        pthread_cond_signal(&cv);
    } else if (type == JOB_CUR) {   /* promote a queued prefetch of the same photo */
        for (int i = 0; i < qn; i++) if (!strcmp(q[i].url, url) && q[i].type == JOB_PREFETCH) q[i].type = JOB_CUR;
    }
    pthread_mutex_unlock(&lk);
}

/* drop queued jobs of a type that fail keep(); keep NULL = drop all of that type */
static void drop_jobs(int type, int lo, int hi) {
    pthread_mutex_lock(&lk);
    int o = 0;
    for (int i = 0; i < qn; i++) {
        int keep = q[i].type != type || (q[i].idx >= lo && q[i].idx <= hi);
        if (keep) q[o++] = q[i];
    }
    qn = o;
    pthread_mutex_unlock(&lk);
}

/* ------------------------------------------------------------ texture cache */
#define NCACHE 4
static struct { char url[1024]; GLuint tex; int w, h; unsigned used; } cache[NCACHE];
static unsigned use_clock;

static GLuint make_tex(const image *im) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im->w, im->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im->rgba);
    return t;
}

static int cache_find(const char *url) {
    for (int i = 0; i < NCACHE; i++) if (cache[i].tex && !strcmp(cache[i].url, url)) { cache[i].used = ++use_clock; return i; }
    return -1;
}

static void cache_put(const char *url, const image *im, const char *protect) {
    int slot = -1;
    for (int i = 0; i < NCACHE && slot < 0; i++) if (!cache[i].tex) slot = i;
    if (slot < 0) {   /* evict least recently used, never the photo on screen */
        unsigned best = ~0u;
        for (int i = 0; i < NCACHE; i++)
            if (cache[i].used < best && (!protect || strcmp(cache[i].url, protect))) { best = cache[i].used; slot = i; }
        if (slot < 0) slot = 0;
        glDeleteTextures(1, &cache[slot].tex);
    }
    cache[slot].tex = make_tex(im);
    cache[slot].w = im->w; cache[slot].h = im->h;
    cache[slot].used = ++use_clock;
    snprintf(cache[slot].url, sizeof cache[slot].url, "%s", url);
}

/* ------------------------------------------------------------------- UI */
static nctext_font *fbig, *fmed, *fsmall;
static char status_msg[256];
static int dir_gen;

static int load_dir(const char *url) {
    http_resp r;
    snprintf(status_msg, sizeof status_msg, "Loading %s ...", url);
    if (http_get(url, &r) != 0) { snprintf(status_msg, sizeof status_msg, "Error: %s", r.err); free(r.data); return -1; }
    drop_jobs(JOB_THUMB, 1, 0);
    for (int i = 0; i < nentries; i++) if (entries[i].ttex) glDeleteTextures(1, &entries[i].ttex);
    dir_gen++;
    parse_listing(r.final_url[0] ? r.final_url : url, r.data);
    snprintf(cur_url, sizeof cur_url, "%s", r.final_url[0] ? r.final_url : url);
    free(r.data);
    int nimg = 0;
    for (int i = 0; i < nentries; i++) nimg += !entries[i].is_dir;
    snprintf(status_msg, sizeof status_msg, "%d folder%s, %d photo%s", nentries - nimg, nentries - nimg == 1 ? "" : "s",
             nimg, nimg == 1 ? "" : "s");
    return 0;
}

static void parent_url(char *u) {
    size_t l = strlen(u);
    if (l && u[l - 1] == '/') u[--l] = 0;
    char *s = strrchr(u, '/');
    if (s && s > u + 7 && strchr(u + 7, '/') && s >= strchr(u + 7, '/')) s[1] = 0;
    else if (l) { u[l] = '/'; u[l + 1] = 0; }
}

static int next_image(int from, int step) {
    if (nentries == 0) return -1;
    int n = from;
    for (int k = 0; k < nentries; k++) {
        n = (n + step + nentries) % nentries;
        if (!entries[n].is_dir) return n;
    }
    return from;
}

static void draw_fit(GLuint tex, int w, int h, int x, int y, int bw, int bh) {
    double s = (double)bw / w;
    if (h * s > bh) s = (double)bh / h;
    int dw = (int)(w * s), dh = (int)(h * s);
    ncgfx_texture(tex, x + (bw - dw) / 2, y + (bh - dh) / 2, dw, dh, 0xffffffffu, 0);
}

static void draw_clipped_text(nctext_font *f, int x, int y, int maxw, const char *s, uint32_t col) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    if (nctext_width(f, buf) > maxw) {
        size_t l = strlen(buf);
        while (l > 1 && nctext_width(f, buf) > maxw - nctext_width(f, "...")) {
            buf[--l] = 0;
            while (l > 0 && ((unsigned char)buf[l - 1] & 0xC0) == 0x80) buf[--l] = 0;   /* don't split UTF-8 */
        }
        strcat(buf, "...");
    }
    int tw = nctext_width(f, buf);
    nctext_draw(f, x + (maxw - tw) / 2, y, buf, col);
}

int main(int argc, char **argv) {
    char root[1024] = "";
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        char line[1100];
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "url=", 4)) sscanf(line + 4, "%1023s", root);
        if (f) fclose(f);
    }

    if (argc > 2 && !strcmp(argv[2], "--selftest")) {   /* headless: walk one level, decode full + thumb */
        if (!root[0]) { fprintf(stderr, "no url= in config\n"); return 1; }
        if (load_dir(root) != 0) { fprintf(stderr, "%s\n", status_msg); return 1; }
        int nd = nentries;
        entry *tops = malloc(sizeof *entries * (nd ? nd : 1));
        memcpy(tops, entries, sizeof *entries * nd);
        printf("root %s: %d entries\n", cur_url, nd);
        for (int i = 0; i < nd; i++) {
            if (!tops[i].is_dir) continue;
            if (load_dir(tops[i].url) != 0) { printf("  dir %s: %s\n", tops[i].name, status_msg); continue; }
            printf("  dir '%s': %s\n", tops[i].name, status_msg);
            for (int k = 0; k < nentries; k++) {
                if (entries[k].is_dir) continue;
                for (int thumb = 0; thumb < 2; thumb++) {
                    struct timespec t0, t1;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    image im;
                    load_image(entries[k].url, thumb ? THUMB_W : SCREEN_W, thumb ? THUMB_H : SCREEN_H, thumb, &im);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
                    if (im.rgba) printf("    %-14s %-5s -> %dx%d in %.0f ms\n", entries[k].name, thumb ? "thumb" : "full", im.w, im.h, ms);
                    else printf("    %-14s %-5s -> ERROR %s\n", entries[k].name, thumb ? "thumb" : "full", im.err);
                    free(im.rgba);
                }
            }
        }
        return 0;
    }

    nc_config cfg = {0};
    if (nc_init(&cfg) != 0) return 2;
    ncgfx_init();
    fbig = nctext_open(NULL, 40); fmed = nctext_open(NULL, 28); fsmall = nctext_open(NULL, 21);
    if (!fbig || !fmed || !fsmall) nc_exit(3);
    pthread_t th;
    pthread_create(&th, NULL, loader, NULL);

    const int COLS = 5, ROWS = 3, TW = THUMB_W, TH = THUMB_H, GX = 36, PITCH = 186;
    const int X0 = (SCREEN_W - (COLS * TW + (COLS - 1) * GX)) / 2, Y0 = 122;
    enum { BROWSE, VIEW } mode = BROWSE;
    int sel = 0, toprow = 0, cur = -1, slideshow = 0, info = 1;
    char img_err[160] = "";
    time_t slide_t = 0;
    float spin = 0;

    if (!root[0]) snprintf(status_msg, sizeof status_msg, "No server configured: put url=http://HOST:PORT/ in photos.conf");
    else load_dir(root);

    for (;;) {
        /* ------------------------------------------------ input */
        nc_event e;
        while (nc_poll(&e, 0)) {
            if (e.type == NC_EV_QUIT) nc_exit(0);
            if (e.type != NC_EV_KEY || e.value == 0) continue;
            int c = e.code;
            int back = c == KEY_ESC || c == KEY_BACKSPACE;
            int ok = c == KEY_ENTER || c == KEY_KPENTER || c == KEY_OK;
            if (mode == BROWSE) {
                if (c == KEY_RIGHT && sel < nentries - 1) sel++;
                else if (c == KEY_LEFT && sel > 0) sel--;
                else if (c == KEY_DOWN) { sel += COLS; if (sel > nentries - 1) sel = nentries - 1; }
                else if (c == KEY_UP) { if (sel >= COLS) sel -= COLS; }
                else if (c == KEY_CHANNELUP || c == KEY_PAGEUP) { sel -= COLS * ROWS; if (sel < 0) sel = 0; }
                else if (c == KEY_PAGEDOWN) { sel += COLS * ROWS; if (sel > nentries - 1) sel = nentries - 1; }
                else if (ok && nentries > 0) {
                    if (entries[sel].is_dir) {
                        char u[1024];
                        snprintf(u, sizeof u, "%s", entries[sel].url);
                        if (load_dir(u) == 0) { sel = 0; toprow = 0; }
                    } else {
                        mode = VIEW; cur = sel; img_err[0] = 0; slide_t = time(0);
                    }
                } else if (back) {
                    if (!root[0] || strlen(cur_url) <= strlen(root)) nc_exit(0);
                    char u[1024];
                    snprintf(u, sizeof u, "%s", cur_url);
                    parent_url(u);
                    if (load_dir(u) == 0) { sel = 0; toprow = 0; }
                }
            } else {
                int step = c == KEY_RIGHT ? 1 : c == KEY_LEFT ? -1 : 0;
                if (ok) { slideshow = !slideshow; slide_t = time(0); }
                else if (c == KEY_UP || c == KEY_DOWN || c == KEY_INFO) info = !info;
                else if (back) { mode = BROWSE; slideshow = 0; sel = cur; }
                if (step) { cur = next_image(cur, step); img_err[0] = 0; slide_t = time(0); }
            }
        }

        /* ------------------------------------------------ results from the loader */
        pthread_mutex_lock(&lk);
        result got[QMAX];
        int ngot = rn;
        memcpy(got, res, sizeof *res * rn);
        rn = 0;
        int busy = working_type >= 0 || qn > 0;
        pthread_mutex_unlock(&lk);
        for (int i = 0; i < ngot; i++) {
            result *r = &got[i];
            if (r->type == JOB_THUMB) {
                if (r->gen == dir_gen && r->idx < nentries && !strcmp(entries[r->idx].url, r->url)) {
                    entry *en = &entries[r->idx];
                    if (r->im.rgba) { en->ttex = make_tex(&r->im); en->tw = r->im.w; en->th = r->im.h; en->tstate = 2; }
                    else en->tstate = 3;
                }
            } else if (r->im.rgba) {
                if (cache_find(r->url) < 0) cache_put(r->url, &r->im, mode == VIEW && cur >= 0 ? entries[cur].url : NULL);
            } else if (mode == VIEW && cur >= 0 && !strcmp(r->url, entries[cur].url)) {
                snprintf(img_err, sizeof img_err, "%s", r->im.err);
            }
            free(r->im.rgba);
        }

        /* ------------------------------------------------ schedule work */
        if (mode == BROWSE) {
            if (sel < toprow * COLS) toprow = sel / COLS;
            if (sel >= (toprow + ROWS) * COLS) toprow = sel / COLS - ROWS + 1;
            int lo = toprow * COLS, hi = (toprow + ROWS) * COLS - 1;
            drop_jobs(JOB_THUMB, lo - COLS, hi + COLS);   /* keep one row of slack */
            for (int i = lo; i <= hi + COLS && i < nentries; i++) {
                if (entries[i].is_dir || entries[i].tstate) continue;
                entries[i].tstate = 1;
                enqueue(JOB_THUMB, entries[i].url, i, dir_gen);
            }
            for (int i = 0; i < nentries; i++)   /* re-queue thumbs dropped while off-screen */
                if (entries[i].tstate == 1 && (i < lo - COLS || i > hi + COLS)) entries[i].tstate = 0;
        } else if (cur >= 0) {
            int have = cache_find(entries[cur].url) >= 0;
            if (!have && !img_err[0]) enqueue(JOB_CUR, entries[cur].url, cur, dir_gen);
            if (have) {   /* prefetch neighbours once the current photo is on screen */
                int nx = next_image(cur, 1), pv = next_image(cur, -1);
                if (nx >= 0 && cache_find(entries[nx].url) < 0) enqueue(JOB_PREFETCH, entries[nx].url, nx, dir_gen);
                if (pv >= 0 && pv != nx && cache_find(entries[pv].url) < 0) enqueue(JOB_PREFETCH, entries[pv].url, pv, dir_gen);
            }
            if (slideshow && (have || img_err[0]) && time(0) - slide_t >= 6) {
                cur = next_image(cur, 1); img_err[0] = 0; slide_t = time(0);
            }
        }

        /* ------------------------------------------------ draw */
        spin += 0.15f;
        ncgfx_begin(NC_RGBA(0, 0, 0, 255));
        if (mode == BROWSE) {
            ncgfx_rect(0, 0, SCREEN_W, 96, NC_RGBA(22, 28, 50, 255));
            nctext_draw(fbig, 60, 62, "Photos", NC_RGBA(255, 255, 255, 255));
            char shown[128];
            snprintf(shown, sizeof shown, "%.110s", cur_url[0] ? cur_url : "(no server)");
            nctext_draw(fsmall, 230, 58, shown, NC_RGBA(150, 170, 210, 255));
            for (int i = toprow * COLS; i < nentries && i < (toprow + ROWS) * COLS; i++) {
                int r = i / COLS - toprow, c = i % COLS;
                int x = X0 + c * (TW + GX), y = Y0 + r * PITCH;
                entry *en = &entries[i];
                if (i == sel) ncgfx_rect(x - 5, y - 5, TW + 10, TH + 10, NC_RGBA(255, 255, 255, 255));
                ncgfx_rect(x, y, TW, TH, NC_RGBA(28, 32, 44, 255));
                if (en->is_dir) {   /* folder glyph */
                    ncgfx_rect(x + 40, y + 34, 50, 16, NC_RGBA(220, 165, 50, 255));
                    ncgfx_rect(x + 40, y + 46, 112, 70, NC_RGBA(240, 190, 70, 255));
                } else if (en->tstate == 2) {
                    draw_fit(en->ttex, en->tw, en->th, x, y, TW, TH);
                } else if (en->tstate == 3) {
                    nctext_draw(fsmall, x + 70, y + TH / 2 + 8, "?", NC_RGBA(200, 90, 90, 255));
                } else {   /* loading dots */
                    for (int k = 0; k < 3; k++)
                        ncgfx_rect(x + TW / 2 - 22 + k * 18, y + TH / 2 - 4, 8, 8,
                                   NC_RGBA(200, 200, 210, (int)(100 + 100 * __builtin_sinf(spin + k))));
                }
                draw_clipped_text(fsmall, x, y + TH + 26, TW, en->name, NC_RGBA(i == sel ? 255 : 200, i == sel ? 255 : 205, i == sel ? 255 : 215, 255));
            }
            if (nentries == 0 && root[0]) nctext_draw(fmed, 100, 200, "No folders or photos here.", NC_RGBA(200, 200, 200, 255));
            ncgfx_rect(0, SCREEN_H - 42, SCREEN_W, 42, NC_RGBA(22, 28, 50, 255));
            nctext_draw(fsmall, 60, SCREEN_H - 14, status_msg, NC_RGBA(200, 205, 220, 255));
            nctext_draw(fsmall, 760, SCREEN_H - 14, "OK open   BACK back   CH+ page up", NC_RGBA(140, 150, 180, 255));
            if (busy) for (int k = 0; k < 8; k++) {
                float a = spin + k * 0.785f;
                int sx = SCREEN_W - 60 + (int)(18 * __builtin_cosf(a)), sy = 48 + (int)(18 * __builtin_sinf(a));
                ncgfx_rect(sx - 3, sy - 3, 6, 6, NC_RGBA(255, 255, 255, 60 + k * 24));
            }
        } else if (cur >= 0) {
            int ci = cache_find(entries[cur].url);
            if (ci >= 0) draw_fit(cache[ci].tex, cache[ci].w, cache[ci].h, 0, 0, SCREEN_W, SCREEN_H);
            else if (entries[cur].tstate == 2)   /* show the thumbnail while the full photo loads */
                draw_fit(entries[cur].ttex, entries[cur].tw, entries[cur].th, 0, 0, SCREEN_W, SCREEN_H);
            if (img_err[0]) nctext_draw(fmed, 80, SCREEN_H / 2, img_err, NC_RGBA(255, 120, 120, 255));
            if (ci < 0 && !img_err[0]) for (int k = 0; k < 8; k++) {
                float a = spin + k * 0.785f;
                int sx = SCREEN_W - 70 + (int)(22 * __builtin_cosf(a)), sy = 60 + (int)(22 * __builtin_sinf(a));
                ncgfx_rect(sx - 4, sy - 4, 8, 8, NC_RGBA(255, 255, 255, 60 + k * 24));
            }
            if (info) {
                char cap[400];
                int nimg = 0, idx = 0;
                for (int i = 0; i < nentries; i++) if (!entries[i].is_dir) { nimg++; if (i == cur) idx = nimg; }
                snprintf(cap, sizeof cap, "%s   (%d/%d)%s", entries[cur].name, idx, nimg, slideshow ? "   [slideshow]" : "");
                ncgfx_rect(0, SCREEN_H - 56, SCREEN_W, 56, NC_RGBA(0, 0, 0, 160));
                nctext_draw(fmed, 40, SCREEN_H - 18, cap, NC_RGBA(255, 255, 255, 255));
                nctext_draw(fsmall, 840, SCREEN_H - 20, "< > browse  OK slideshow  BACK back", NC_RGBA(170, 175, 190, 255));
            }
        }
        nc_swap();
    }
}
