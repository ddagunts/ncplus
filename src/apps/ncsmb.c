/*
 * ncsmb: Network Shares, a file browser for SMB2/3 shares (libsmb2) and the USB stick.
 *
 * Shares come from the [shares] section of netcast.conf (run.sh passes it as $NC_SHARES_CONF; fallback: shares.conf next to the binary):
 *     Name = smb://[DOMAIN;]user[:password]@host[:port]/[share[/folder]]
 * An entry without a share lists the server's shares.
 *
 * Remote: UP/DOWN move, CH+ page down, OK/RIGHT open, LEFT/BACK go up (BACK at the top quits),
 *         RED play all music in this folder, GREEN refresh, YELLOW play a video with ffplay.
 * Opening a file hands it to another NetCast+ app and comes back here afterwards:
 *   music (FLAC/MP3/WAV) -> Music, photos -> Photos (this folder), video -> Player (hardware decode).
 *   YELLOW on a video plays it with ffplay instead (software decode: codecs/containers the hardware can't do).
 * Those apps read SMB files through smbgw, a read-only HTTP gateway on 127.0.0.1:8445.
 */
#define _GNU_SOURCE
#include "sdlui.h"
#include "smbcommon.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#define GW_PORT 8445
#define STATE "/tmp/ncsmb.state"

enum { LOC_ROOT, LOC_LOCAL, LOC_HOST, LOC_SMB };
enum { T_DIR, T_AUDIO, T_IMAGE, T_VIDEO, T_OTHER };

typedef struct { int kind, entry; char share[128]; char path[2048]; } loc;
typedef struct { char *name; int type; unsigned long long size; int target_kind, target_entry; } ent;

static char self[1024], netcast[1100], conf[1200];
static smb_entry shares[64];
static int nshares;
static loc cur;
static ent *ents;
static int nents, sel, top;
static char status[400];

static struct smb2_context *ctx;
static int ctx_entry = -1;
static char ctx_share[128];

/* ------------------------------------------------------------------ helpers */
static int file_type(const char *n) {
    const char *e = strrchr(n, '.');
    if (!e) return T_OTHER;
    e++;
    static const char *au[] = {"flac", "mp3", "wav"};
    static const char *im[] = {"jpg", "jpeg", "png", "bmp", "gif"};
    static const char *vi[] = {"mp4", "m4v", "mkv", "ts", "m2ts", "mts", "mpg", "mpeg", "avi", "mov", "flv", "vob"};
    for (unsigned i = 0; i < sizeof au / sizeof *au; i++) if (!strcasecmp(e, au[i])) return T_AUDIO;
    for (unsigned i = 0; i < sizeof im / sizeof *im; i++) if (!strcasecmp(e, im[i])) return T_IMAGE;
    for (unsigned i = 0; i < sizeof vi / sizeof *vi; i++) if (!strcasecmp(e, vi[i])) return T_VIDEO;
    return T_OTHER;
}

static void clear_ents(void) {
    for (int i = 0; i < nents; i++) free(ents[i].name);
    free(ents);
    ents = NULL;
    nents = 0;
}

static void add_ent(const char *name, int type, unsigned long long size, int tk, int te) {
    ents = realloc(ents, sizeof *ents * (size_t)(nents + 1));
    ents[nents].name = strdup(name);
    ents[nents].type = type;
    ents[nents].size = size;
    ents[nents].target_kind = tk;
    ents[nents].target_entry = te;
    nents++;
}

static int cmp_ent(const void *a, const void *b) {
    const ent *x = a, *y = b;
    if ((x->type == T_DIR) != (y->type == T_DIR)) return x->type == T_DIR ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void join(char *out, size_t n, const char *a, const char *b) {
    if (!a[0]) snprintf(out, n, "%s", b);
    else snprintf(out, n, "%s/%s", a, b);
}

static void save_state(void) {
    FILE *f = fopen(STATE, "w");
    if (!f) return;
    fprintf(f, "%d\n%d\n%d\n%s\n%s\n", cur.kind, cur.entry, sel, cur.share, cur.path);
    fclose(f);
}

static void load_state(void) {
    FILE *f = fopen(STATE, "r");
    if (!f) return;
    char l[5][2100] = {{0}};
    for (int i = 0; i < 5 && fgets(l[i], sizeof l[i], f); i++) l[i][strcspn(l[i], "\n")] = 0;
    fclose(f);
    cur.kind = atoi(l[0]);
    cur.entry = atoi(l[1]);
    sel = atoi(l[2]);
    snprintf(cur.share, sizeof cur.share, "%s", l[3]);
    snprintf(cur.path, sizeof cur.path, "%s", l[4]);
    if ((cur.kind == LOC_SMB || cur.kind == LOC_HOST) && (cur.entry < 0 || cur.entry >= nshares)) memset(&cur, 0, sizeof cur);
}

/* ------------------------------------------------------------------ drawing */
static void breadcrumb(char *out, size_t n) {
    switch (cur.kind) {
    case LOC_ROOT: snprintf(out, n, "Network Shares"); break;
    case LOC_LOCAL: snprintf(out, n, "USB%s", cur.path + strlen("/mnt/usb")); break;
    case LOC_HOST: snprintf(out, n, "%s", shares[cur.entry].name); break;
    case LOC_SMB: snprintf(out, n, "%s  \xE2\x80\xBA  %s%s%s", shares[cur.entry].name, cur.share, cur.path[0] ? "/" : "", cur.path); break;
    }
}

#define ROWS 11
#define ROWH 46

static void draw(const char *overlay) {
    SDL_Color bg = ui_rgb(0x0e1220), fg = ui_rgb(0xffffff), dim = ui_rgb(0x9aa6c8);
    ui_clear(bg);
    char bc[2400];
    breadcrumb(bc, sizeof bc);
    ui_text(bc, 60, 32, 32, 1, fg, 1160, UI_LEFT);
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    for (int i = top; i < nents && i < top + ROWS; i++) {
        int y = 100 + (i - top) * ROWH;
        if (i == sel) ui_fill(40, y - 4, 1200, ROWH - 4, ui_rgb(0x2c3658));
        static const unsigned tagc[] = {0x3f7fd0, 0x43a047, 0xef8f2f, 0x8e44ad, 0x55607a};
        static const char *tagt[] = {"DIR", "MUSIC", "PHOTO", "VIDEO", "FILE"};
        int t = ents[i].type;
        const char *tag = tagt[t];
        if (cur.kind == LOC_ROOT) tag = ents[i].target_kind == LOC_LOCAL ? "USB" : "SMB";
        if (cur.kind == LOC_HOST) tag = "SHARE";
        ui_fill(60, y + 4, 90, 28, ui_rgb(tagc[t]));
        ui_text(tag, 105, y + 8, 16, 1, fg, 0, UI_CENTER);
        ui_text(ents[i].name, 170, y + 4, 24, 0, t == T_OTHER ? dim : fg, 880, UI_LEFT);
        if (t != T_DIR) {
            char sz[32];
            ui_format_size(sz, sizeof sz, ents[i].size);
            ui_text(sz, 1220, y + 8, 20, 0, dim, 0, UI_RIGHT);
        }
    }
    if (nents > ROWS) {                            /* scroll bar */
        int h = ROWS * ROWH * ROWS / nents;
        ui_fill(1246, 100 + (ROWS * ROWH - h) * top / (nents - ROWS), 4, h, dim);
    }
    if (!nents && !overlay) ui_text(cur.kind == LOC_ROOT ? "No shares configured" : "Empty folder", 640, 300, 28, 0, dim, 0, UI_CENTER);
    if (overlay) ui_text(overlay, 640, 300, 28, 1, ui_rgb(0x4fc3f7), 1100, UI_CENTER);
    if (status[0]) ui_text(status, 60, 622, 20, 0, ui_rgb(0xffb74d), 1160, UI_LEFT);
    ui_text("OK open   \xE2\x97\x80 / BACK up   CH+/CH- page   RED play folder   GREEN refresh   YELLOW ffplay", 640, 680, 18, 0, dim, 1200, UI_CENTER);
    ui_present();
}

/* ------------------------------------------------------------------ listing */
static int smb_use(int entry, const char *share) {
    if (ctx && ctx_entry == entry && !strcmp(ctx_share, share)) return 0;
    if (ctx) { smb2_destroy_context(ctx); ctx = NULL; }
    char err[300] = "";
    ctx = smb_connect(&shares[entry], share, err, sizeof err);
    if (!ctx) { snprintf(status, sizeof status, "%s", err); return -1; }
    ctx_entry = entry;
    snprintf(ctx_share, sizeof ctx_share, "%s", share);
    return 0;
}

static int list_smb(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt && ctx) { smb2_destroy_context(ctx); ctx = NULL; }
        if (smb_use(cur.entry, cur.share) != 0) return -1;
        struct smb2dir *d = smb2_opendir(ctx, cur.path);
        if (!d) { snprintf(status, sizeof status, "%s: %s", cur.path[0] ? cur.path : cur.share, smb2_get_error(ctx)); continue; }
        struct smb2dirent *e;
        while ((e = smb2_readdir(ctx, d))) {
            if (e->name[0] == '.') continue;
            int dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
            add_ent(e->name, dir ? T_DIR : file_type(e->name), e->st.smb2_size, 0, 0);
        }
        smb2_closedir(ctx, d);
        status[0] = 0;
        return 0;
    }
    return -1;
}

static void list_local(void) {
    DIR *d = opendir(cur.path);
    if (!d) { snprintf(status, sizeof status, "%s: %s", cur.path, strerror(errno)); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4200];
        snprintf(p, sizeof p, "%s/%s", cur.path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        add_ent(e->d_name, S_ISDIR(st.st_mode) ? T_DIR : file_type(e->d_name), (unsigned long long)st.st_size, 0, 0);
    }
    closedir(d);
}

/* Fill ents for cur. select_name: entry to select afterwards (going up), or NULL. */
static void refresh(const char *select_name) {
    draw("Loading\xE2\x80\xA6");
    clear_ents();
    status[0] = 0;
    switch (cur.kind) {
    case LOC_ROOT:
        add_ent("USB storage", T_DIR, 0, LOC_LOCAL, 0);
        for (int i = 0; i < nshares; i++) add_ent(shares[i].name, T_DIR, 0, shares[i].share[0] ? LOC_SMB : LOC_HOST, i);
        if (!nshares) snprintf(status, sizeof status, "Add servers to %s (Name = smb://user:password@host/share)", conf);
        break;
    case LOC_LOCAL: list_local(); break;
    case LOC_HOST: {
        char *names[256], err[300] = "";
        int n = smb_list_shares(&shares[cur.entry], names, 256, err, sizeof err);
        if (n < 0) snprintf(status, sizeof status, "%s", err);
        for (int i = 0; i < n; i++) { add_ent(names[i], T_DIR, 0, 0, 0); free(names[i]); }
        break;
    }
    case LOC_SMB: list_smb(); break;
    }
    if (cur.kind != LOC_ROOT && nents > 1) qsort(ents, (size_t)nents, sizeof *ents, cmp_ent);
    if (select_name) {
        sel = 0;
        for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, select_name)) sel = i;
    }
    if (sel >= nents) sel = nents ? nents - 1 : 0;
    if (sel < 0) sel = 0;
    save_state();
}

static void enter(int i) {
    ent *e = &ents[i];
    switch (cur.kind) {
    case LOC_ROOT:
        cur.kind = e->target_kind;
        cur.entry = e->target_entry;
        if (cur.kind == LOC_LOCAL) snprintf(cur.path, sizeof cur.path, "/mnt/usb");
        else if (cur.kind == LOC_SMB) {
            snprintf(cur.share, sizeof cur.share, "%s", shares[cur.entry].share);
            snprintf(cur.path, sizeof cur.path, "%s", shares[cur.entry].path);
        }
        break;
    case LOC_HOST:
        cur.kind = LOC_SMB;
        snprintf(cur.share, sizeof cur.share, "%s", e->name);
        cur.path[0] = 0;
        break;
    case LOC_LOCAL: { char p[2100]; snprintf(p, sizeof p, "%s/%s", cur.path, e->name); snprintf(cur.path, sizeof cur.path, "%s", p); break; }
    case LOC_SMB: { char p[2100]; join(p, sizeof p, cur.path, e->name); snprintf(cur.path, sizeof cur.path, "%s", p); break; }
    }
    sel = 0; top = 0;
    refresh(NULL);
}

/* returns 0 when already at the top */
static int go_up(void) {
    char child[300] = "";
    switch (cur.kind) {
    case LOC_ROOT: return 0;
    case LOC_LOCAL: {
        if (!strcmp(cur.path, "/mnt/usb")) { cur.kind = LOC_ROOT; snprintf(child, sizeof child, "USB storage"); break; }
        char *s = strrchr(cur.path, '/');
        snprintf(child, sizeof child, "%s", s + 1);
        *s = 0;
        break;
    }
    case LOC_HOST:
        snprintf(child, sizeof child, "%s", shares[cur.entry].name);
        cur.kind = LOC_ROOT;
        break;
    case LOC_SMB:
        if (cur.path[0]) {
            char *s = strrchr(cur.path, '/');
            if (s) { snprintf(child, sizeof child, "%s", s + 1); *s = 0; }
            else { snprintf(child, sizeof child, "%s", cur.path); cur.path[0] = 0; }
        } else if (!shares[cur.entry].share[0]) {
            snprintf(child, sizeof child, "%s", cur.share);
            cur.kind = LOC_HOST;
        } else {
            snprintf(child, sizeof child, "%s", shares[cur.entry].name);
            cur.kind = LOC_ROOT;
        }
        break;
    }
    top = 0;
    refresh(child);
    return 1;
}

/* ------------------------------------------------------------------ handing files to other apps */
static int gw_alive(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(GW_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

static int ensure_gw(void) {
    if (gw_alive()) return 0;
    char cmd[3000];
    snprintf(cmd, sizeof cmd, "nohup '%s/smbgw' '%s' %d > /tmp/smbgw.log 2>&1 &", self, conf, GW_PORT);
    int r = system(cmd);
    (void)r;
    for (int i = 0; i < 30; i++) { SDL_Delay(100); if (gw_alive()) return 0; }
    snprintf(status, sizeof status, "SMB gateway did not start (see /tmp/smbgw.log)");
    return -1;
}

/* URL (gateway or file path) of name in the current folder; dir_url: the folder itself, with '/' */
static void item_src(const char *name, int for_gst, char *out, size_t n) {
    char rel[4200], enc[12000];
    if (cur.kind == LOC_LOCAL) {
        snprintf(rel, sizeof rel, "%s%s%s", cur.path, name ? "/" : "/", name ? name : "");
        if (!for_gst) { snprintf(out, n, "%s", rel); return; }
        url_encode_path(rel, enc, sizeof enc);
        snprintf(out, n, "file://%s", enc);
        return;
    }
    char p[4200];
    join(p, sizeof p, cur.path, name ? name : "");
    snprintf(rel, sizeof rel, "%s/%s", cur.share, p);
    url_encode_path(rel, enc, sizeof enc);
    snprintf(out, n, "http://127.0.0.1:%d/s/%d/%s%s", GW_PORT, cur.entry, enc, name ? "" : "/");
}

static void folder_url(char *out, size_t n) {
    if (cur.kind != LOC_LOCAL) { item_src(NULL, 0, out, n); return; }
    const char *rel = cur.path + strlen("/mnt/usb");      /* the gateway's /l/ is /mnt/usb */
    while (*rel == '/') rel++;
    char enc[12000];
    url_encode_path(rel, enc, sizeof enc);
    snprintf(out, n, "http://127.0.0.1:%d/l/%s%s", GW_PORT, enc, enc[0] ? "/" : "");
}

static void shq(const char *s, char *out, size_t n) {   /* single-quote for sh */
    size_t o = 0;
    out[o++] = '\'';
    for (; *s && o + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
}

static int chain(const char *cmd) {          /* run cmd after we exit, then come back here */
    char me[1200];
    shq(self, me, sizeof me);
    FILE *f = fopen("/tmp/ncmenu.next.tmp", "w");
    if (!f) return -1;
    fprintf(f, "%s\nsh %s/run.sh\n", cmd, me);
    fclose(f);
    if (getenv("NETCAST_SESSION")) {          /* start.sh runs /tmp/ncmenu.next when we quit */
        rename("/tmp/ncmenu.next.tmp", "/tmp/ncmenu.next");
    } else {
        rename("/tmp/ncmenu.next.tmp", "/tmp/ncsmb.chain");
        int r = system("(sleep 1; sh /tmp/ncsmb.chain) > /tmp/ncsmb-chain.log 2>&1 &");
        (void)r;
    }
    return 0;
}

static int play_music(int from) {
    if (cur.kind == LOC_SMB && ensure_gw() != 0) return -1;
    FILE *f = fopen("/tmp/ncsmb.m3u", "w");
    if (!f) return -1;
    int k = 0, start = 0;
    for (int i = 0; i < nents; i++) {
        if (ents[i].type != T_AUDIO) continue;
        if (i == from) start = k;
        char src[13000];
        item_src(ents[i].name, 0, src, sizeof src);
        fprintf(f, "%s\n", src);
        k++;
    }
    fclose(f);
    if (!k) { snprintf(status, sizeof status, "No music in this folder"); return -1; }
    char app[1300], cmd[1500];
    snprintf(app, sizeof app, "%s/apps/music/run.sh", netcast);
    shq(app, cmd, sizeof cmd);
    char full[2000];
    snprintf(full, sizeof full, "sh %s --start %d /tmp/ncsmb.m3u", cmd, start);
    return chain(full);
}

static int play_video(int i, const char *player) {   /* player: "player" (hardware) or "ffplay" */
    char app[1300], q[1500], arg[13000], qa[13500], full[16000];
    if (cur.kind == LOC_SMB && ensure_gw() != 0) return -1;
    item_src(ents[i].name, 1, arg, sizeof arg);
    snprintf(app, sizeof app, "%s/apps/%s/run.sh", netcast, player);
    if (access(app, R_OK) != 0) { snprintf(status, sizeof status, "%s is not installed", player); return -1; }
    shq(app, q, sizeof q);
    shq(arg, qa, sizeof qa);
    snprintf(full, sizeof full, "sh %s %s", q, qa);
    return chain(full);
}

static int open_file(int i) {
    ent *e = &ents[i];
    char app[1300], q[1500], full[16000];
    switch (e->type) {
    case T_AUDIO: return play_music(i);
    case T_IMAGE: {
        if (ensure_gw() != 0) return -1;
        char url[13000];
        folder_url(url, sizeof url);
        FILE *f = fopen("/tmp/ncsmb-photos.conf", "w");
        if (!f) return -1;
        fprintf(f, "url=%s\n", url);
        fclose(f);
        snprintf(app, sizeof app, "%s/apps/photos/run.sh", netcast);
        shq(app, q, sizeof q);
        snprintf(full, sizeof full, "sh %s /tmp/ncsmb-photos.conf", q);
        return chain(full);
    }
    case T_VIDEO:
        return play_video(i, "player");
    default:
        snprintf(status, sizeof status, "No app for %s", e->name);
        return -1;
    }
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    (void)argc;
    char fontdir[1200];
    snprintf(self, sizeof self, "%s", argv[0]);
    char *sl = strrchr(self, '/');
    if (sl) *sl = 0; else snprintf(self, sizeof self, ".");
    snprintf(netcast, sizeof netcast, "%s/../..", self);
    /* run.sh extracts the [shares] section of netcast.conf; fall back to shares.conf next to the binary */
    const char *sc = getenv("NC_SHARES_CONF");
    if (sc && *sc) snprintf(conf, sizeof conf, "%s", sc);
    else snprintf(conf, sizeof conf, "%s/shares.conf", self);
    snprintf(fontdir, sizeof fontdir, "%s/system/fonts", netcast);
    nshares = smb_load_conf(conf, shares, 64);
    load_state();

    if (ui_init("Network Shares", fontdir) != 0) return 1;
    int keep_sel = sel;
    refresh(NULL);
    if (keep_sel < nents) sel = keep_sel;

    for (;;) {
        SDL_Event ev;
        int handed_off = 0;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) goto out;
            if (ev.type != SDL_KEYDOWN) continue;
            switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_UP: if (sel > 0) sel--; break;
            case SDL_SCANCODE_DOWN: if (sel < nents - 1) sel++; break;
            case SDL_SCANCODE_PAGEUP: sel += ROWS; if (sel >= nents) sel = nents ? nents - 1 : 0; break;
            case SDL_SCANCODE_PAGEDOWN: sel -= ROWS; if (sel < 0) sel = 0; break;
            case SDL_SCANCODE_HOME: sel = 0; break;
            case SDL_SCANCODE_END: sel = nents ? nents - 1 : 0; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_RIGHT:
                if (!nents) break;
                if (ents[sel].type == T_DIR) enter(sel);
                else if (ev.key.keysym.scancode != SDL_SCANCODE_RIGHT && open_file(sel) == 0) handed_off = 1;
                break;
            case SDL_SCANCODE_LEFT: case SDL_SCANCODE_BACKSPACE: go_up(); break;
            case SDL_SCANCODE_ESCAPE: if (!go_up()) goto out; break;
            case SDL_SCANCODE_F1: if (play_music(-1) == 0) handed_off = 1; break;
            case SDL_SCANCODE_F3:
                if (nents && ents[sel].type == T_VIDEO && play_video(sel, "ffplay") == 0) handed_off = 1;
                else if (nents && ents[sel].type != T_VIDEO) snprintf(status, sizeof status, "YELLOW plays videos with ffplay");
                break;
            case SDL_SCANCODE_F2:
                nshares = smb_load_conf(conf, shares, 64);
                if (ctx) { smb2_destroy_context(ctx); ctx = NULL; }
                refresh(nents ? ents[sel].name : NULL);
                break;
            default: break;
            }
            save_state();
            if (handed_off) goto out;
        }
        draw(NULL);
    }
out:
    save_state();
    if (ctx) smb2_destroy_context(ctx);
    ui_quit();
    return 0;
}
