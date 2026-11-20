/*
 * launcher: the NetCast+ home screen. Scans <appsdir>/<name>/app.ini, shows tiles,
 * writes the chosen app dir to /tmp/ncmenu.choice ("exit" to leave) and exits;
 * start.sh runs the app and brings the launcher back afterwards.
 *
 * app.ini:   title=Doom
 *            desc=Freedoom: Phase 1 (Chocolate Doom)
 *            exec=doom.sh            (shell script in the app folder)
 *            color=c03028            (tile colour, hex RGB)
 *            order=10                (sort key, optional)
 *
 * Usage: launcher <appsdir>
 */
#include "nc.h"
#include "ncgfx.h"
#include "nctext.h"
#include <dirent.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXAPPS 32
typedef struct {
    char dir[256], title[64], desc[160];
    uint32_t color;
    int order;
} app_t;
static app_t apps[MAXAPPS];
static int napps;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
}

static void load_apps(const char *root) {
    DIR *d = opendir(root);
    struct dirent *de;
    while (d && (de = readdir(d)) && napps < MAXAPPS - 1) {
        if (de->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s/app.ini", root, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        app_t *a = &apps[napps];
        memset(a, 0, sizeof *a);
        snprintf(a->dir, sizeof a->dir, "%s/%s", root, de->d_name);
        snprintf(a->title, sizeof a->title, "%s", de->d_name);
        a->color = NC_RGBA(60, 90, 160, 255);
        a->order = 50;
        char line[256];
        int has_exec = 0;
        while (fgets(line, sizeof line, f)) {
            trim(line);
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = line, *v = eq + 1;
            if (!strcmp(k, "title")) snprintf(a->title, sizeof a->title, "%s", v);
            else if (!strcmp(k, "desc")) snprintf(a->desc, sizeof a->desc, "%s", v);
            else if (!strcmp(k, "exec")) has_exec = 1;
            else if (!strcmp(k, "order")) a->order = atoi(v);
            else if (!strcmp(k, "color")) { unsigned c = strtoul(v, 0, 16); a->color = (c << 8) | 0xff; }
        }
        fclose(f);
        if (has_exec) napps++;
    }
    if (d) closedir(d);
    /* sort by order, then title */
    for (int i = 0; i < napps; i++)
        for (int j = i + 1; j < napps; j++)
            if (apps[j].order < apps[i].order || (apps[j].order == apps[i].order && strcmp(apps[j].title, apps[i].title) < 0)) {
                app_t t = apps[i]; apps[i] = apps[j]; apps[j] = t;
            }
    /* final tile: leave */
    app_t *x = &apps[napps++];
    memset(x, 0, sizeof *x);
    snprintf(x->dir, sizeof x->dir, "exit");
    snprintf(x->title, sizeof x->title, "Exit to TV");
    snprintf(x->desc, sizeof x->desc, "Return to the LG home screen");
    x->color = NC_RGBA(70, 70, 80, 255);
}

static void choose(const char *dir) {
    FILE *f = fopen("/tmp/ncmenu.choice", "w");
    if (f) { fprintf(f, "%s\n", dir); fclose(f); }
    nc_log("launcher: chose %s", dir);
    nc_exit(0);
}

static uint32_t shade(uint32_t c, float k) {
    int r = (c >> 24) & 255, g = (c >> 16) & 255, b = (c >> 8) & 255;
    r = (int)(r * k); g = (int)(g * k); b = (int)(b * k);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return NC_RGBA(r, g, b, 255);
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    remove("/tmp/ncmenu.choice");
    load_apps(root);
    nc_config cfg = {0};
    if (nc_init(&cfg) != 0) return 2;
    nc_pad_keys(1);
    ncgfx_init();
    nctext_font *ftitle = nctext_open(NULL, 56), *fname = nctext_open(NULL, 30), *fsmall = nctext_open(NULL, 24),
                *fbig = nctext_open(NULL, 96);
    if (!ftitle || !fname || !fsmall || !fbig) nc_exit(3);

    /* Grid area between the header (ends at 143) and the footer (starts at FOOT): VIS rows are on
     * screen; the grid scrolls by whole rows so the selection always stays visible. */
    const int COLS = 5, TW = 212, TH = 180, GAP = 24, X0 = (1280 - (COLS * TW + (COLS - 1) * GAP)) / 2, Y0 = 172;
    const int FOOT = 610, VIS = (FOOT - Y0 + GAP) / (TH + GAP);
    const int rows = (napps + COLS - 1) / COLS;
    int sel = 0, top = 0;      /* top: first visible row */
    float scroll = 0;          /* animated row offset, follows top */
    float pulse = 0;
    time_t idle_since = time(0);

    for (;;) {
        nc_event e;
        while (nc_poll(&e, 0)) {
            if (e.type == NC_EV_QUIT) choose("exit");
            if (e.type != NC_EV_KEY || e.value == 0) continue;
            idle_since = time(0);
            switch (e.code) {
            case KEY_RIGHT: if (sel < napps - 1) sel++; break;
            case KEY_LEFT: if (sel > 0) sel--; break;
            case KEY_DOWN: if (sel + COLS < napps) sel += COLS; else if (sel / COLS < rows - 1) sel = napps - 1; break;
            case KEY_UP: if (sel - COLS >= 0) sel -= COLS; break;
            case KEY_CHANNELDOWN: case KEY_PAGEDOWN: sel += COLS * VIS; if (sel > napps - 1) sel = napps - 1; break;
            case KEY_CHANNELUP: case KEY_PAGEUP: sel -= COLS * VIS; if (sel < 0) sel = 0; break;
            case KEY_HOME: sel = 0; break;
            case KEY_END: sel = napps - 1; break;
            case KEY_ENTER: case KEY_KPENTER: case KEY_OK: case KEY_SPACE: case BTN_LEFT:
                choose(apps[sel].dir);
                break;
            default: break;
            }
        }
        int srow = sel / COLS;
        if (srow < top) top = srow;
        if (srow >= top + VIS) top = srow - VIS + 1;
        scroll += (top - scroll) * 0.3f;
        if (__builtin_fabsf(top - scroll) < 0.01f) scroll = (float)top;
        pulse += 0.06f;
        ncgfx_begin(NC_RGBA(14, 18, 32, 255));
        /* tiles (drawn first; header and footer cover the rows scrolling out) */
        for (int i = 0; i < napps; i++) {
            int r = i / COLS, c = i % COLS;
            int x = X0 + c * (TW + GAP), y = Y0 + (int)((r - scroll) * (TH + GAP));
            if (y + TH + 12 < 143 || y - 12 > FOOT) continue;
            int s = i == sel;
            if (s) {
                int g = 6 + (int)(3 * (1 + __builtin_sinf(pulse)));
                ncgfx_rect(x - g, y - g, TW + 2 * g, TH + 2 * g, NC_RGBA(255, 255, 255, 255));
            }
            ncgfx_rect(x, y, TW, TH, shade(apps[i].color, s ? 1.15f : 0.8f));
            ncgfx_rect(x, y + TH - 56, TW, 56, NC_RGBA(0, 0, 0, 255));
            char initial[2] = {apps[i].title[0], 0};
            int iw = nctext_width(fbig, initial);
            nctext_draw(fbig, x + (TW - iw) / 2, y + 112, initial, NC_RGBA(255, 255, 255, s ? 255 : 210));
            nctext_font *fn = fname;
            int tw = nctext_width(fn, apps[i].title);
            if (tw > TW - 12) { fn = fsmall; tw = nctext_width(fn, apps[i].title); }
            nctext_draw(fn, x + (TW - tw) / 2, y + TH - 18, apps[i].title, NC_RGBA(255, 255, 255, 255));
        }
        /* header */
        ncgfx_rect(0, 0, 1280, 140, NC_RGBA(22, 28, 50, 255));
        ncgfx_rect(0, 140, 1280, 3, NC_RGBA(80, 120, 230, 255));
        nctext_draw(ftitle, X0, 95, "NetCast+", NC_RGBA(255, 255, 255, 255));
        nctext_draw(fsmall, X0 + 300, 92, "native apps for your LG TV", NC_RGBA(150, 170, 210, 255));
        char pos[32];
        snprintf(pos, sizeof pos, "%d / %d", sel + 1, napps);
        nctext_draw(fsmall, X0 + COLS * TW + (COLS - 1) * GAP - nctext_width(fsmall, pos), 92, pos, NC_RGBA(150, 170, 210, 255));
        /* scroll hints: small triangles-as-bars at the right edge when rows are hidden */
        if (top > 0) nctext_draw(fsmall, 1280 - 40, 175, "^", NC_RGBA(200, 210, 240, 255));
        if (top + VIS < rows) nctext_draw(fsmall, 1280 - 40, FOOT - 12, "v", NC_RGBA(200, 210, 240, 255));
        /* footer: description of the selection + controls */
        int fy = FOOT + 40;
        ncgfx_rect(0, FOOT, 1280, 720 - FOOT, NC_RGBA(22, 28, 50, 255));
        nctext_draw(fname, X0, fy, apps[sel].desc[0] ? apps[sel].desc : apps[sel].title, NC_RGBA(235, 235, 245, 255));
        nctext_draw(fsmall, X0, fy + 40, rows > VIS ? "Arrows: move    OK: launch    CH+/CH-: page" : "Arrows: move    OK: launch", NC_RGBA(140, 150, 180, 255));
        nc_swap();
        (void)idle_since;
    }
}
