/*
 * ncmusic: music player for ncplus (FLAC, MP3, WAV) on SDL2 (netcast video + mtkaudiosink audio).
 *
 *   ncmusic [--start N] [items...]    item = audio file, folder (scanned recursively), .m3u playlist,
 *                                     or http:// URL (e.g. the smbgw gateway for SMB shares)
 *   no items: scans the USB stick(s) for music
 *
 * Remote: UP/DOWN pick a track, OK play it / pause, LEFT/RIGHT seek 10 s, CH+ next, RED previous,
 *         GREEN screen off (music keeps playing), YELLOW shuffle, BLUE repeat, CH- previous, BACK quit.
 * Decoding runs on its own thread and feeds SDL's audio queue (SDL converts rate/channels/24-bit).
 */
#define _GNU_SOURCE
#include "sdlui.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ------------------------------------------------------------------ reader: local file or HTTP */
typedef struct {
    FILE *f;
    int http, sock;
    char host[256], port[8], path[4096];
    int64_t pos, size;         /* logical position, total size (-1 unknown) */
    int64_t buf_off;           /* file offset of buf[0] */
    int buf_len;
    int64_t sock_off;          /* file offset of the next byte the socket delivers */
    void *meta;                /* metadata target for the dr_libs meta callbacks (same user pointer) */
    unsigned char buf[65536];
} reader;

static int http_connect(reader *r, int64_t from) {
    if (r->sock >= 0) { close(r->sock); r->sock = -1; }
    struct addrinfo hints = {0}, *ai;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(r->host, r->port, &hints, &ai) != 0) return -1;
    int s = socket(ai->ai_family, SOCK_STREAM, 0);
    struct timeval tv = {15, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (s < 0 || connect(s, ai->ai_addr, ai->ai_addrlen) != 0) { if (s >= 0) close(s); freeaddrinfo(ai); return -1; }
    freeaddrinfo(ai);
    char req[4600];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nRange: bytes=%lld-\r\nUser-Agent: ncmusic\r\n\r\n",
                     r->path, r->host, (long long)from);
    if (send(s, req, (size_t)n, MSG_NOSIGNAL) != n) { close(s); return -1; }
    char h[8192];
    int hl = 0;
    char *end = NULL;
    while (hl < (int)sizeof h - 1) {
        ssize_t k = recv(s, h + hl, sizeof h - 1 - (size_t)hl, 0);
        if (k <= 0) { close(s); return -1; }
        hl += (int)k;
        h[hl] = 0;
        if ((end = strstr(h, "\r\n\r\n"))) break;
    }
    if (!end) { close(s); return -1; }
    int code = 0;
    sscanf(h, "HTTP/%*s %d", &code);
    if (code != 200 && code != 206) { close(s); return -1; }
    char *cr = strcasestr(h, "\r\nContent-Range:"), *cl = strcasestr(h, "\r\nContent-Length:");
    if (cr) { char *sl = strchr(cr, '/'); if (sl && sl < end) r->size = atoll(sl + 1); }
    else if (cl && code == 200) r->size = atoll(cl + 17);
    int64_t start = code == 206 ? from : 0;
    /* bytes after the header are body data */
    int body = hl - (int)(end + 4 - h);
    memcpy(r->buf, end + 4, (size_t)body);
    r->buf_off = start;
    r->buf_len = body;
    r->sock_off = start + body;
    r->sock = s;
    return 0;
}

static reader *rd_open(const char *src) {
    reader *r = calloc(1, sizeof *r);
    r->sock = -1;
    r->size = -1;
    if (!strncasecmp(src, "http://", 7)) {
        r->http = 1;
        const char *h = src + 7, *sl = strchr(h, '/');
        size_t hl = sl ? (size_t)(sl - h) : strlen(h);
        char hp[300];
        snprintf(hp, sizeof hp, "%.*s", (int)hl, h);
        char *c = strrchr(hp, ':');
        snprintf(r->port, sizeof r->port, "%s", c ? c + 1 : "80");
        if (c) *c = 0;
        snprintf(r->host, sizeof r->host, "%s", hp);
        snprintf(r->path, sizeof r->path, "%s", sl ? sl : "/");
        if (http_connect(r, 0) != 0) { free(r); return NULL; }
    } else {
        r->f = fopen(src, "rb");
        if (!r->f) { free(r); return NULL; }
        setvbuf(r->f, NULL, _IOFBF, 65536);
        struct stat st;
        if (fstat(fileno(r->f), &st) == 0) r->size = st.st_size;
    }
    return r;
}

static size_t rd_read(reader *r, void *out, size_t n) {
    if (!r->http) { size_t k = fread(out, 1, n, r->f); r->pos += (int64_t)k; return k; }
    size_t done = 0;
    while (done < n) {
        if (r->pos >= r->buf_off && r->pos < r->buf_off + r->buf_len) {
            size_t avail = (size_t)(r->buf_off + r->buf_len - r->pos), k = n - done < avail ? n - done : avail;
            memcpy((char *)out + done, r->buf + (r->pos - r->buf_off), k);
            done += k; r->pos += (int64_t)k;
            continue;
        }
        if (r->size >= 0 && r->pos >= r->size) break;
        /* far away from what the socket delivers next: new request (skip small forward gaps) */
        if (r->sock < 0 || r->pos < r->sock_off || r->pos > r->sock_off + 256 * 1024)
            if (http_connect(r, r->pos) != 0) break;
        if (r->pos >= r->buf_off && r->pos < r->buf_off + r->buf_len) continue;
        ssize_t k = recv(r->sock, r->buf, sizeof r->buf, 0);
        if (k <= 0) { close(r->sock); r->sock = -1; if (k == 0) break; if (http_connect(r, r->pos) != 0) break; continue; }
        r->buf_off = r->sock_off;
        r->buf_len = (int)k;
        r->sock_off += k;
    }
    return done;
}

static int rd_seek(reader *r, int64_t off, int whence) {
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? r->pos + off : (r->size < 0 ? -1 : r->size + off);
    if (p < 0) return -1;
    if (!r->http) { if (fseeko(r->f, (off_t)p, SEEK_SET) != 0) return -1; }
    r->pos = p;
    return 0;
}

static void rd_close(reader *r) {
    if (!r) return;
    if (r->f) fclose(r->f);
    if (r->sock >= 0) close(r->sock);
    free(r);
}

/* whole (small) file into memory, e.g. cover.jpg */
static unsigned char *rd_slurp(const char *src, int *len, int max) {
    reader *r = rd_open(src);
    if (!r) return NULL;
    if (r->size <= 0 || r->size > max) { rd_close(r); return NULL; }
    unsigned char *b = malloc((size_t)r->size);
    int n = (int)rd_read(r, b, (size_t)r->size);
    rd_close(r);
    if (n <= 0) { free(b); return NULL; }
    *len = n;
    return b;
}

/* dr_libs callbacks (identical signatures for flac / mp3 / wav) */
static size_t cb_read(void *u, void *out, size_t n) { return rd_read(u, out, n); }
static int cb_seek(void *u, int off, int origin) { return rd_seek(u, off, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END) == 0; }
static int cb_tell(void *u, int64_t *cur) { *cur = ((reader *)u)->pos; return 1; }

/* ------------------------------------------------------------------ playlist */
typedef struct { char *src; char *name; } item;
static item *items;
static int nitems, cap_items;

static int is_audio(const char *p) {
    const char *e = strrchr(p, '.');
    return e && (!strcasecmp(e, ".flac") || !strcasecmp(e, ".mp3") || !strcasecmp(e, ".wav"));
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = {s[1], s[2], 0}; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}

static void add_item(const char *src) {
    if (nitems == cap_items) { cap_items = cap_items ? cap_items * 2 : 256; items = realloc(items, sizeof *items * (size_t)cap_items); }
    const char *b = strrchr(src, '/');
    char name[512];
    snprintf(name, sizeof name, "%s", b ? b + 1 : src);
    url_decode(name);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    items[nitems].src = strdup(src);
    items[nitems].name = strdup(name);
    nitems++;
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void scan_dir(const char *dir, int depth) {
    if (depth > 5 || nitems > 5000) return;
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "netcast")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, sizeof *names * (size_t)cap); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n) qsort(names, (size_t)n, sizeof *names, cmp_str);
    for (int pass = 0; pass < 2; pass++)          /* files first, then subfolders (album order) */
        for (int i = 0; i < n; i++) {
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            struct stat st;
            if (stat(p, &st) != 0) continue;
            if (pass == 0 && S_ISREG(st.st_mode) && is_audio(p)) add_item(p);
            if (pass == 1 && S_ISDIR(st.st_mode)) scan_dir(p, depth + 1);
        }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

static void load_m3u(const char *file) {
    FILE *f = fopen(file, "r");
    if (!f) return;
    char dir[2048], line[4096];
    snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (strstr(line, "://") || line[0] == '/') add_item(line);
        else { char p[6200]; snprintf(p, sizeof p, "%s/%s", dir, line); add_item(p); }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ metadata */
typedef struct {
    char title[256], artist[256], album[256];
    unsigned char *cover; int cover_len;      /* encoded image bytes */
    int cover_type;                           /* FLAC/ID3 picture type of what we kept (3 = front) */
} meta;

static void keep_picture(meta *m, const void *data, size_t len, int type) {
    if (!data || !len || len > 8u << 20) return;
    if (m->cover && (m->cover_type == 3 || type != 3)) return;   /* prefer the front cover */
    free(m->cover);
    m->cover = malloc(len);
    memcpy(m->cover, data, len);
    m->cover_len = (int)len;
    m->cover_type = type;
}

static void flac_meta(void *u, drflac_metadata *md) {
    meta *m = ((reader *)u)->meta;
    if (md->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        drflac_vorbis_comment_iterator it;
        drflac_init_vorbis_comment_iterator(&it, md->data.vorbis_comment.commentCount, md->data.vorbis_comment.pComments);
        drflac_uint32 len;
        const char *c;
        while ((c = drflac_next_vorbis_comment(&it, &len))) {
            char *dst = NULL; size_t kl = 0;
            if (len > 6 && !strncasecmp(c, "TITLE=", 6)) { dst = m->title; kl = 6; }
            else if (len > 7 && !strncasecmp(c, "ARTIST=", 7)) { dst = m->artist; kl = 7; }
            else if (len > 6 && !strncasecmp(c, "ALBUM=", 6)) { dst = m->album; kl = 6; }
            if (dst && !dst[0]) snprintf(dst, 256, "%.*s", (int)(len - kl), c + kl);
        }
    } else if (md->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
        keep_picture(m, md->data.picture.pPictureData, md->data.picture.pictureDataSize, (int)md->data.picture.type);
    }
}

/* ID3v2 text (encoding byte + data) -> UTF-8 */
static void id3_text(const unsigned char *p, size_t n, char *out, size_t on) {
    if (!n) return;
    int enc = p[0];
    p++; n--;
    size_t o = 0;
    if (enc == 0) {                           /* ISO-8859-1 */
        for (size_t i = 0; i < n && p[i] && o + 3 < on; i++) {
            if (p[i] < 0x80) out[o++] = (char)p[i];
            else { out[o++] = (char)(0xC0 | p[i] >> 6); out[o++] = (char)(0x80 | (p[i] & 0x3F)); }
        }
    } else if (enc == 3) {                    /* UTF-8 */
        for (size_t i = 0; i < n && p[i] && o + 1 < on; i++) out[o++] = (char)p[i];
    } else {                                  /* UTF-16 (1: BOM, 2: BE) */
        int be = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) { be = p[0] == 0xFE && p[1] == 0xFF; i = 2; }
        for (; i + 1 < n && o + 4 < on; i += 2) {
            unsigned c = be ? (unsigned)(p[i] << 8 | p[i + 1]) : (unsigned)(p[i + 1] << 8 | p[i]);
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {  /* surrogate pair */
                unsigned c2 = be ? (unsigned)(p[i + 2] << 8 | p[i + 3]) : (unsigned)(p[i + 3] << 8 | p[i + 2]);
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i += 2;
            }
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
            else if (o + 5 < on) { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        }
    }
    out[o] = 0;
}

static void parse_id3v2(const unsigned char *d, size_t n, meta *m) {
    if (n < 10 || memcmp(d, "ID3", 3)) return;
    int ver = d[3];
    size_t pos = 10, end = 10 + ((size_t)(d[6] & 0x7F) << 21 | (size_t)(d[7] & 0x7F) << 14 | (size_t)(d[8] & 0x7F) << 7 | (d[9] & 0x7F));
    if (end > n) end = n;
    if (d[5] & 0x40 && ver >= 3 && pos + 4 <= end) {          /* extended header */
        size_t es = ver == 4 ? ((size_t)(d[10] & 0x7F) << 21 | (size_t)(d[11] & 0x7F) << 14 | (size_t)(d[12] & 0x7F) << 7 | (d[13] & 0x7F))
                             : ((size_t)d[10] << 24 | (size_t)d[11] << 16 | (size_t)d[12] << 8 | d[13]) + 4;
        pos += es;
    }
    int idlen = ver == 2 ? 3 : 4, hdr = ver == 2 ? 6 : 10;
    while (pos + (size_t)hdr <= end && d[pos]) {
        const unsigned char *f = d + pos;
        size_t sz;
        if (ver == 2) sz = (size_t)f[3] << 16 | (size_t)f[4] << 8 | f[5];
        else if (ver == 4) sz = (size_t)(f[4] & 0x7F) << 21 | (size_t)(f[5] & 0x7F) << 14 | (size_t)(f[6] & 0x7F) << 7 | (f[7] & 0x7F);
        else sz = (size_t)f[4] << 24 | (size_t)f[5] << 16 | (size_t)f[6] << 8 | f[7];
        const unsigned char *p = f + hdr;
        if (pos + (size_t)hdr + sz > end) break;
        if (!memcmp(f, ver == 2 ? "TT2" : "TIT2", (size_t)idlen) && !m->title[0]) id3_text(p, sz, m->title, sizeof m->title);
        else if (!memcmp(f, ver == 2 ? "TP1" : "TPE1", (size_t)idlen) && !m->artist[0]) id3_text(p, sz, m->artist, sizeof m->artist);
        else if (!memcmp(f, ver == 2 ? "TAL" : "TALB", (size_t)idlen) && !m->album[0]) id3_text(p, sz, m->album, sizeof m->album);
        else if (!memcmp(f, ver == 2 ? "PIC" : "APIC", (size_t)idlen) && sz > 4) {
            int enc = p[0];
            size_t i = 1;
            if (ver == 2) i += 3;                                  /* 3-char image format */
            else { while (i < sz && p[i]) i++; i++; }              /* MIME type */
            int type = i < sz ? p[i] : 0;
            i++;
            if (enc == 1 || enc == 2) { while (i + 1 < sz && (p[i] || p[i + 1])) i += 2; i += 2; }   /* UTF-16 description */
            else { while (i < sz && p[i]) i++; i++; }
            if (i < sz) keep_picture(m, p + i, sz - i, type);
        }
        pos += (size_t)hdr + sz;
    }
}

static void mp3_meta(void *u, const drmp3_metadata *md) {
    if (md->type == DRMP3_METADATA_TYPE_ID3V2) parse_id3v2(md->pRawData, md->rawDataSize, ((reader *)u)->meta);
}

/* ------------------------------------------------------------------ decoder */
enum { DEC_NONE, DEC_FLAC, DEC_MP3, DEC_WAV };
typedef struct {
    int kind;
    reader *r;
    drflac *flac;
    drmp3 mp3;
    drwav wav;
    unsigned rate, ch, bits;
    uint64_t frames;     /* 0 = unknown */
} decoder;

static int dec_open(decoder *d, const char *src, meta *m) {
    memset(d, 0, sizeof *d);
    d->r = rd_open(src);
    if (!d->r) return -1;
    d->r->meta = m;
    unsigned char magic[4] = {0};
    rd_read(d->r, magic, 4);
    rd_seek(d->r, 0, SEEK_SET);
    const char *ext = strrchr(src, '.');
    if (!memcmp(magic, "fLaC", 4) || (ext && !strcasecmp(ext, ".flac"))) {
        d->flac = drflac_open_with_metadata(cb_read, (drflac_seek_proc)cb_seek, (drflac_tell_proc)cb_tell, flac_meta, d->r, NULL);
        if (d->flac) { d->kind = DEC_FLAC; d->rate = d->flac->sampleRate; d->ch = d->flac->channels; d->bits = d->flac->bitsPerSample; d->frames = d->flac->totalPCMFrameCount; }
    } else if (!memcmp(magic, "RIFF", 4) || !memcmp(magic, "RIFX", 4) || (ext && !strcasecmp(ext, ".wav"))) {
        if (drwav_init(&d->wav, cb_read, (drwav_seek_proc)cb_seek, (drwav_tell_proc)cb_tell, d->r, NULL)) {
            d->kind = DEC_WAV; d->rate = d->wav.sampleRate; d->ch = d->wav.channels; d->bits = d->wav.bitsPerSample; d->frames = d->wav.totalPCMFrameCount;
        }
    } else {
        if (drmp3_init(&d->mp3, cb_read, (drmp3_seek_proc)cb_seek, (drmp3_tell_proc)cb_tell, mp3_meta, d->r, NULL)) {
            d->kind = DEC_MP3; d->rate = d->mp3.sampleRate; d->ch = d->mp3.channels; d->bits = 0;
            d->frames = d->mp3.totalPCMFrameCount == DRMP3_UINT64_MAX ? 0 : d->mp3.totalPCMFrameCount;
            if (!d->frames && !d->r->http) { d->frames = drmp3_get_pcm_frame_count(&d->mp3); drmp3_seek_to_pcm_frame(&d->mp3, 0); }
        }
    }
    if (!d->kind || !d->rate || !d->ch) {
        if (d->kind == DEC_FLAC) drflac_close(d->flac);
        rd_close(d->r);
        d->r = NULL; d->kind = DEC_NONE;
        return -1;
    }
    return 0;
}

static uint64_t dec_read(decoder *d, int16_t *out, uint64_t frames) {
    switch (d->kind) {
    case DEC_FLAC: return drflac_read_pcm_frames_s16(d->flac, frames, out);
    case DEC_MP3: return drmp3_read_pcm_frames_s16(&d->mp3, frames, out);
    case DEC_WAV: return drwav_read_pcm_frames_s16(&d->wav, frames, out);
    }
    return 0;
}

static int dec_seek(decoder *d, uint64_t frame) {
    switch (d->kind) {
    case DEC_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame);
    case DEC_MP3: return drmp3_seek_to_pcm_frame(&d->mp3, frame);
    case DEC_WAV: return drwav_seek_to_pcm_frame(&d->wav, frame);
    }
    return 0;
}

static void dec_close(decoder *d) {
    if (d->kind == DEC_FLAC) drflac_close(d->flac);
    else if (d->kind == DEC_MP3) drmp3_uninit(&d->mp3);
    else if (d->kind == DEC_WAV) drwav_uninit(&d->wav);
    rd_close(d->r);
    memset(d, 0, sizeof *d);
}

/* ------------------------------------------------------------------ player state (shared) */
static SDL_mutex *mu;
static int want_track = -1, cur_track = -1, paused, shuffle, repeat_all = 1, quitting, loading;
static double want_seek = -1;
static double pos_sec, len_sec;
static char fmt_line[160], status[300];
static meta cur_meta;
static int meta_serial;
static unsigned char *cover_rgba;     /* decoded, handed to the UI thread */
static int cover_w, cover_h, cover_serial;
static int *order;                    /* shuffle order */

static SDL_AudioDeviceID dev;
static SDL_AudioSpec dev_spec;

static int next_index(int cur, int dir) {
    if (nitems == 0) return -1;
    int k = 0;
    for (int i = 0; i < nitems; i++) if (order[i] == cur) k = i;
    k += dir;
    if (k >= nitems) { if (!repeat_all) return -1; k = 0; }
    if (k < 0) k = nitems - 1;
    return order[k];
}

static void reshuffle(void) {
    for (int i = 0; i < nitems; i++) order[i] = i;
    if (!shuffle) return;
    for (int i = nitems - 1; i > 0; i--) { int j = rand() % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }
}

static void load_cover(const char *src, meta *m) {
    unsigned char *enc = m->cover;
    int len = m->cover_len, own = 0;
    if (!enc) {                           /* cover.jpg / folder.jpg next to the file */
        static const char *names[] = {"cover.jpg", "folder.jpg", "front.jpg", "Cover.jpg", "Folder.jpg", "cover.png", "AlbumArtSmall.jpg"};
        char dir[4096];
        snprintf(dir, sizeof dir, "%s", src);
        char *sl = strrchr(dir, '/');
        if (!sl) return;
        sl[1] = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names && !enc; i++) {
            char p[4200];
            snprintf(p, sizeof p, "%s%s", dir, names[i]);
            enc = rd_slurp(p, &len, 8 << 20);
        }
        own = 1;
    }
    if (!enc) return;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(enc, len, &w, &h, &n, 4);
    if (own) free(enc);
    if (!px) return;
    /* box-downscale big art to <= 512 px */
    int f = 1;
    while (w / f > 512 || h / f > 512) f++;
    if (f > 1) {
        int nw = w / f, nh = h / f;
        unsigned char *o = malloc((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                unsigned s[4] = {0};
                for (int dy = 0; dy < f; dy++)
                    for (int dx = 0; dx < f; dx++) {
                        unsigned char *q = px + ((size_t)(y * f + dy) * w + (size_t)(x * f + dx)) * 4;
                        for (int c = 0; c < 4; c++) s[c] += q[c];
                    }
                for (int c = 0; c < 4; c++) o[((size_t)y * nw + x) * 4 + c] = (unsigned char)(s[c] / (unsigned)(f * f));
            }
        stbi_image_free(px);
        px = o; w = nw; h = nh;
    }
    SDL_LockMutex(mu);
    free(cover_rgba);
    cover_rgba = px; cover_w = w; cover_h = h; cover_serial++;
    SDL_UnlockMutex(mu);
}

static int open_device(unsigned rate, unsigned ch) {
    if (dev && dev_spec.freq == (int)rate && dev_spec.channels == ch) return 0;
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_AudioSpec want = {0};
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)ch;
    want.samples = 2048;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &dev_spec, 0);   /* SDL converts to what the TV takes */
    if (!dev) { SDL_Log("ncmusic: audio: %s", SDL_GetError()); return -1; }
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}

static int decode_thread(void *arg) {
    (void)arg;
    decoder d = {0};
    int16_t *pcm = malloc(4096 * 8 * sizeof(int16_t));
    uint64_t decoded = 0;     /* frames handed to SDL for the current track */
    int track = -1, draining = 0;
    for (;;) {
        SDL_LockMutex(mu);
        int q = quitting, wt = want_track, pz = paused;
        double ws = want_seek;
        want_seek = -1;
        SDL_UnlockMutex(mu);
        if (q) break;

        if (wt != track) {
            if (d.kind) dec_close(&d);
            if (dev) SDL_ClearQueuedAudio(dev);
            track = wt; decoded = 0; draining = 0;
            if (track < 0) { SDL_Delay(20); continue; }
            meta m;
            memset(&m, 0, sizeof m);
            SDL_LockMutex(mu);
            loading = 1; cur_track = track; pos_sec = 0; len_sec = 0;
            snprintf(status, sizeof status, "Loading %s", items[track].name);
            SDL_UnlockMutex(mu);
            int ok = dec_open(&d, items[track].src, &m) == 0 && open_device(d.rate, d.ch) == 0;
            SDL_LockMutex(mu);
            loading = 0;
            free(cur_meta.cover);
            cur_meta = m;
            cur_meta.cover = NULL;
            meta_serial++;
            free(cover_rgba); cover_rgba = NULL; cover_w = cover_h = 0; cover_serial++;
            if (ok) {
                const char *k = d.kind == DEC_FLAC ? "FLAC" : d.kind == DEC_MP3 ? "MP3" : "WAV";
                char bits[16] = "";
                if (d.bits) snprintf(bits, sizeof bits, " \xC2\xB7 %u-bit", d.bits);
                snprintf(fmt_line, sizeof fmt_line, "%s \xC2\xB7 %.1f kHz%s \xC2\xB7 %s", k, d.rate / 1000.0, bits,
                         d.ch == 1 ? "mono" : d.ch == 2 ? "stereo" : d.ch == 6 ? "5.1" : "multichannel");
                len_sec = d.frames ? (double)d.frames / d.rate : 0;
                status[0] = 0;
            } else {
                snprintf(status, sizeof status, "Cannot play %s", items[track].name);
                fmt_line[0] = 0;
            }
            SDL_UnlockMutex(mu);
            if (ok) load_cover(items[track].src, &m);
            free(m.cover);
            if (!ok) {                                   /* skip to the next one after a moment */
                SDL_Delay(1500);
                SDL_LockMutex(mu);
                if (want_track == track) want_track = next_index(track, 1);
                SDL_UnlockMutex(mu);
            }
            continue;
        }
        if (!d.kind) { SDL_Delay(20); continue; }

        if (ws >= 0) {
            uint64_t f = (uint64_t)(ws * d.rate);
            if (d.frames && f >= d.frames) f = d.frames - 1;
            SDL_ClearQueuedAudio(dev);
            if (dec_seek(&d, f)) decoded = f;
            draining = 0;
        }
        SDL_PauseAudioDevice(dev, pz);
        unsigned bpf = d.ch * 2;
        uint64_t queued = SDL_GetQueuedAudioSize(dev) / bpf;
        SDL_LockMutex(mu);
        pos_sec = (double)(decoded > queued ? decoded - queued : 0) / d.rate;
        SDL_UnlockMutex(mu);
        if (pz) { SDL_Delay(20); continue; }
        if (draining) {
            if (queued == 0) {
                SDL_LockMutex(mu);
                if (want_track == track) want_track = next_index(track, 1);
                if (want_track < 0) { snprintf(status, sizeof status, "End of playlist"); cur_track = -1; }
                SDL_UnlockMutex(mu);
                if (want_track < 0) { dec_close(&d); track = -1; }
            } else SDL_Delay(20);
            continue;
        }
        if (queued > d.rate / 2) { SDL_Delay(10); continue; }   /* keep ~0.5 s queued */
        uint64_t got = dec_read(&d, pcm, 4096);
        if (got == 0) { draining = 1; continue; }
        SDL_QueueAudio(dev, pcm, (Uint32)(got * bpf));
        decoded += got;
    }
    if (d.kind) dec_close(&d);
    free(pcm);
    return 0;
}

/* ------------------------------------------------------------------ headless self-test (no SDL) */
static int selftest(void) {
    int fails = 0;
    for (int i = 0; i < nitems; i++) {
        decoder d;
        meta m;
        memset(&m, 0, sizeof m);
        if (dec_open(&d, items[i].src, &m) != 0) { printf("FAIL open %s\n", items[i].src); fails++; continue; }
        int cw = 0, chh = 0, cn = 0, coverlen = 0;
        unsigned char *enc = m.cover;
        const char *cs = "embedded";
        if (!enc) {
            char dir[4096], p[4200];
            snprintf(dir, sizeof dir, "%s", items[i].src);
            char *sl = strrchr(dir, '/');
            if (sl) { sl[1] = 0; snprintf(p, sizeof p, "%scover.jpg", dir); enc = rd_slurp(p, &coverlen, 8 << 20); cs = "cover.jpg"; }
        } else coverlen = m.cover_len;
        if (enc && !stbi_info_from_memory(enc, coverlen, &cw, &chh, &cn)) cw = -1;
        if (enc != m.cover) free(enc);
        int16_t *pcm = malloc(4096 * 8 * sizeof(int16_t));
        uint64_t total = 0, got;
        while ((got = dec_read(&d, pcm, 4096)) > 0) total += got;
        int seek_ok = dec_seek(&d, total / 2) && dec_read(&d, pcm, 4096) == 4096;
        printf("%s: kind=%d %u Hz %u ch %u bit, frames hdr=%llu decoded=%llu (%.2f s), seek=%s\n"
               "   title='%s' artist='%s' album='%s' cover=%s %dx%d\n",
               items[i].name, d.kind, d.rate, d.ch, d.bits, (unsigned long long)d.frames, (unsigned long long)total,
               d.rate ? (double)total / d.rate : 0, seek_ok ? "ok" : "FAIL", m.title, m.artist, m.album,
               enc ? cs : "none", cw, chh);
        if (!seek_ok || !total || (d.frames && (d.frames > total + d.rate / 10 || total > d.frames + d.rate / 10))) fails++;
        free(pcm);
        free(m.cover);
        dec_close(&d);
    }
    printf("%d items, %d failures\n", nitems, fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ UI */
static SDL_Texture *cover_tex;
static int sel;

static void draw(void) {
    SDL_Color bg = ui_rgb(0x0e1220), fg = ui_rgb(0xffffff), dim = ui_rgb(0x9aa6c8), acc = ui_rgb(0x4fc3f7), panel = ui_rgb(0x1c2238);
    ui_clear(bg);
    SDL_LockMutex(mu);
    if (cover_serial) {
        static int seen;
        if (seen != cover_serial) {
            seen = cover_serial;
            if (cover_tex) { SDL_DestroyTexture(cover_tex); cover_tex = NULL; }
            if (cover_rgba) cover_tex = ui_texture_rgba(cover_rgba, cover_w, cover_h);
        }
    }
    int ct = cur_track;
    char title[300], artist[300], album[300], fl[160], st[300];
    snprintf(title, sizeof title, "%s", cur_meta.title[0] ? cur_meta.title : ct >= 0 ? items[ct].name : "");
    snprintf(artist, sizeof artist, "%s", cur_meta.artist);
    snprintf(album, sizeof album, "%s", cur_meta.album);
    snprintf(fl, sizeof fl, "%s", fmt_line);
    snprintf(st, sizeof st, "%s", status);
    double p = pos_sec, l = len_sec;
    int pz = paused, sh = shuffle, rp = repeat_all, ld = loading;
    SDL_UnlockMutex(mu);

    /* cover */
    const int CX = 60, CY = 60, CS = 380;
    ui_fill(CX, CY, CS, CS, panel);
    if (cover_tex) {
        int w, h;
        SDL_QueryTexture(cover_tex, NULL, NULL, &w, &h);
        double s = (double)CS / (w > h ? w : h);
        SDL_Rect r = {CX + (CS - (int)(w * s)) / 2, CY + (CS - (int)(h * s)) / 2, (int)(w * s), (int)(h * s)};
        SDL_RenderCopy(ui_r, cover_tex, NULL, &r);
    } else {
        ui_text("\xE2\x99\xAA", CX + CS / 2, CY + CS / 2 - 80, 160, 0, dim, 0, UI_CENTER);
    }

    /* now playing */
    const int TX = 480, TW = 740;
    ui_text(ct >= 0 ? title : "Nothing playing", TX, 70, 40, 1, fg, TW, UI_LEFT);
    if (artist[0]) ui_text(artist, TX, 128, 28, 0, fg, TW, UI_LEFT);
    if (album[0]) ui_text(album, TX, 170, 24, 0, dim, TW, UI_LEFT);
    if (fl[0]) ui_text(fl, TX, 215, 20, 0, dim, TW, UI_LEFT);

    /* progress */
    ui_fill(TX, 290, TW, 8, panel);
    if (l > 0) ui_fill(TX, 290, (int)(TW * (p / l > 1 ? 1 : p / l)), 8, acc);
    char a[32], b[32];
    ui_format_time(a, sizeof a, p);
    ui_format_time(b, sizeof b, l);
    ui_text(a, TX, 306, 20, 0, dim, 0, UI_LEFT);
    if (l > 0) ui_text(b, TX + TW, 306, 20, 0, dim, 0, UI_RIGHT);
    char flags[120];
    snprintf(flags, sizeof flags, "%s%s%s", ld ? "Loading\xE2\x80\xA6   " : pz ? "Paused   " : "", sh ? "Shuffle   " : "", rp ? "Repeat" : "");
    ui_text(flags, TX + TW / 2, 306, 20, 1, acc, 0, UI_CENTER);
    if (st[0]) ui_text(st, TX, 345, 20, 0, ui_rgb(0xffb74d), TW, UI_LEFT);

    /* playlist */
    const int LY = 470, ROWH = 40, ROWS = 5;
    char cnt[64];
    snprintf(cnt, sizeof cnt, "Playlist \xC2\xB7 %d tracks", nitems);
    ui_text(cnt, 60, LY - 40, 22, 1, dim, 0, UI_LEFT);
    int first = sel - ROWS / 2;
    if (first > nitems - ROWS) first = nitems - ROWS;
    if (first < 0) first = 0;
    for (int i = first; i < nitems && i < first + ROWS; i++) {
        int y = LY + (i - first) * ROWH;
        if (i == sel) ui_fill(50, y - 4, 1180, ROWH - 2, ui_rgb(0x2c3658));
        char n[16];
        snprintf(n, sizeof n, "%d", i + 1);
        ui_text(n, 110, y, 22, 0, dim, 0, UI_RIGHT);
        if (i == ct) ui_text("\xE2\x96\xB6", 125, y, 22, 0, acc, 0, UI_LEFT);
        ui_text(items[i].name, 160, y, 22, i == ct, i == ct ? acc : fg, 1040, UI_LEFT);
    }
    ui_text("UP/DOWN pick   OK play/pause   \xE2\x97\x80\xE2\x96\xB6 seek   CH+/CH- next/prev   GREEN screen off   YELLOW shuffle   BLUE repeat   BACK quit",
            640, 690, 16, 0, dim, 1240, UI_CENTER);
    ui_present();
}

int main(int argc, char **argv) {
    char self[1024], fontdir[1100];
    snprintf(self, sizeof self, "%s", argv[0]);
    char *sl = strrchr(self, '/');
    if (sl) *sl = 0; else snprintf(self, sizeof self, ".");
    snprintf(fontdir, sizeof fontdir, "%s/../../system/fonts", self);
    srand((unsigned)time(NULL));

    int start = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--start") && i + 1 < argc) { start = atoi(argv[++i]); continue; }
        const char *a = argv[i];
        struct stat st;
        const char *e = strrchr(a, '.');
        if (strstr(a, "://")) add_item(a);
        else if (e && !strcasecmp(e, ".m3u")) load_m3u(a);
        else if (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) scan_dir(a, 0);
        else if (stat(a, &st) == 0) add_item(a);
    }
    if (argc < 2) {                                /* no arguments: music on the USB stick(s) */
        DIR *d = opendir("/mnt/usb");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[300];
            snprintf(p, sizeof p, "/mnt/usb/%s", e->d_name);
            scan_dir(p, 0);
        }
        if (d) closedir(d);
    }

    if (getenv("NCMUSIC_SELFTEST")) return selftest();
    if (ui_init("Music", fontdir) != 0) return 1;
    mu = SDL_CreateMutex();
    order = calloc((size_t)(nitems ? nitems : 1), sizeof *order);
    reshuffle();
    if (nitems == 0) snprintf(status, sizeof status, "No music found. Open some from Network Shares, or put FLAC/MP3/WAV files on the USB stick.");
    else { if (start < 0 || start >= nitems) start = 0; want_track = start; sel = start; }
    SDL_Thread *th = SDL_CreateThread(decode_thread, "decode", NULL);

    char screen_off[1200];
    snprintf(screen_off, sizeof screen_off, "%s/../../tools/ncscreen off --wake > /dev/null 2>&1 &", self);
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type != SDL_KEYDOWN) continue;
            SDL_LockMutex(mu);
            switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE: running = 0; break;
            case SDL_SCANCODE_UP: if (sel > 0) sel--; break;
            case SDL_SCANCODE_DOWN: if (sel < nitems - 1) sel++; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
                if (nitems == 0) break;
                if (sel == cur_track && ev.key.keysym.scancode != SDL_SCANCODE_SPACE) paused = !paused;
                else if (ev.key.keysym.scancode == SDL_SCANCODE_SPACE) paused = !paused;
                else { want_track = sel; paused = 0; }
                break;
            case SDL_SCANCODE_LEFT: if (cur_track >= 0) want_seek = pos_sec > 10 ? pos_sec - 10 : 0; break;
            case SDL_SCANCODE_RIGHT: if (cur_track >= 0 && (len_sec <= 0 || pos_sec + 10 < len_sec)) want_seek = pos_sec + 10; break;
            case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_N:
                if (nitems) { int n = next_index(cur_track >= 0 ? cur_track : sel, 1); if (n < 0) n = order[0]; want_track = n; sel = n; paused = 0; }
                break;
            case SDL_SCANCODE_F1: case SDL_SCANCODE_P: case SDL_SCANCODE_PAGEDOWN:
                if (nitems) {
                    if (pos_sec > 3 && cur_track >= 0) want_seek = 0;
                    else { int n = next_index(cur_track >= 0 ? cur_track : sel, -1); want_track = n; sel = n; paused = 0; }
                }
                break;
            case SDL_SCANCODE_F2: { int r = system(screen_off); (void)r; break; }
            case SDL_SCANCODE_F3: shuffle = !shuffle; reshuffle(); break;
            case SDL_SCANCODE_F4: repeat_all = !repeat_all; break;
            default: break;
            }
            SDL_UnlockMutex(mu);
        }
        draw();
    }
    SDL_LockMutex(mu);
    quitting = 1;
    SDL_UnlockMutex(mu);
    SDL_WaitThread(th, NULL);
    if (dev) SDL_CloseAudioDevice(dev);
    ui_quit();
    return 0;
}
