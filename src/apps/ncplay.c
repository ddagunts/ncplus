/*
 * ncplay: media player for the NetCast TV using its hardware decoders.
 *
 * GStreamer 0.10 (TV's own) playbin2 -> mtkvideosink (H.264/MPEG-2/MPEG-4 hardware decode on the
 * video plane) + mtkaudiosink render-mode=1. Our OSD window is cleared to alpha 0 so the video
 * plane shows through; a small info bar is drawn on top.
 *
 *   ncplay <list-file>     list of "Name=URI" lines (file:///..., http://..., rtsp://..., udp://...)
 *   ncplay <uri>           play one URI directly
 *
 * Keys: list: UP/DOWN + OK.  Playing: OK pause/resume, LEFT/RIGHT seek -/+10 s,
 *       UP/DOWN toggle info bar, BACK/Esc stop (back to the list).
 */
#define _GNU_SOURCE
#include "nc.h"
#include "ncgfx.h"
#include "nctext.h"
#include <dlfcn.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- GStreamer 0.10 / GObject, loaded at runtime ---- */
typedef struct { uint32_t domain; int code; char *message; } GError_;
#define GST_STATE_NULL 1
#define GST_STATE_PAUSED 3
#define GST_STATE_PLAYING 4
#define GST_FORMAT_TIME 3
#define GST_SEEK_FLAG_FLUSH 1
#define GST_SEEK_FLAG_KEY_UNIT 4
#define GST_MESSAGE_EOS 1
#define GST_MESSAGE_ERROR 2
#define GST_MESSAGE_BUFFERING 32
#define GST_SECOND 1000000000LL
#define MSG_TYPE(m) (*(int *)((char *)(m) + 24))   /* GstMessage.type (0.10, 32-bit) */

static int (*g_init_check)(int *, char ***, void **);
static void *(*g_factory_make)(const char *, const char *);
static int (*g_set_state)(void *, int);
static void *(*g_get_bus)(void *);
static void *(*g_bus_pop_filtered)(void *, int);
static void (*g_msg_parse_error)(void *, GError_ **, char **);
static void (*g_msg_parse_buffering)(void *, int *);
static void (*g_mini_unref)(void *);
static void (*g_obj_unref)(void *);
static int (*g_query_position)(void *, int *, int64_t *);
static int (*g_query_duration)(void *, int *, int64_t *);
static int (*g_seek_simple)(void *, int, int, int64_t);
static void (*g_object_set)(void *, const char *, ...);

static int gst_load(void) {
    const char *reg = getenv("GST_REGISTRY");
    if (!reg || strncmp(reg, "/tmp/", 5)) setenv("GST_REGISTRY", "/tmp/ncapp-gstreg.bin", 1);   /* never flash */
    void *go = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *gs = dlopen("libgstreamer-0.10.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!go || !gs) { nc_log("ncplay: GStreamer 0.10 not found: %s", dlerror()); return -1; }
#define L(h, v, n) if (!(*(void **)&v = dlsym(h, n))) { nc_log("ncplay: missing %s", n); return -1; }
    L(gs, g_init_check, "gst_init_check");
    L(gs, g_factory_make, "gst_element_factory_make");
    L(gs, g_set_state, "gst_element_set_state");
    L(gs, g_get_bus, "gst_element_get_bus");
    L(gs, g_bus_pop_filtered, "gst_bus_pop_filtered");
    L(gs, g_msg_parse_error, "gst_message_parse_error");
    L(gs, g_msg_parse_buffering, "gst_message_parse_buffering");
    L(gs, g_mini_unref, "gst_mini_object_unref");
    L(gs, g_obj_unref, "gst_object_unref");
    L(gs, g_query_position, "gst_element_query_position");
    L(gs, g_query_duration, "gst_element_query_duration");
    L(gs, g_seek_simple, "gst_element_seek_simple");
    L(go, g_object_set, "g_object_set");
#undef L
    return g_init_check(NULL, NULL, NULL) ? 0 : -1;
}

/* ---- playlist ---- */
#define MAXE 128
static char names[MAXE][96], uris[MAXE][512];
static int nent;

static void load_list(const char *path) {
    FILE *f = fopen(path, "r");
    char line[700];
    while (f && fgets(line, sizeof line, f) && nent < MAXE) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (eq && !strstr(line, "://") ) continue;
        if (eq && eq < strstr(line, "://")) {
            *eq = 0;
            snprintf(names[nent], sizeof names[0], "%s", line);
            snprintf(uris[nent], sizeof uris[0], "%s", eq + 1);
        } else {
            snprintf(uris[nent], sizeof uris[0], "%s", line);
            const char *s = strrchr(line, '/');
            snprintf(names[nent], sizeof names[0], "%s", s && s[1] ? s + 1 : line);
        }
        nent++;
    }
    if (f) fclose(f);
}

static void fmt_time(int64_t ns, char *out, size_t n) {
    if (ns < 0) { snprintf(out, n, "--:--"); return; }
    int s = (int)(ns / GST_SECOND);
    snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ncplay <list-file|uri>\n"); return 2; }
    if (strstr(argv[1], "://")) { snprintf(uris[0], sizeof uris[0], "%s", argv[1]); snprintf(names[0], sizeof names[0], "%s", argv[1]); nent = 1; }
    else load_list(argv[1]);

    nc_config cfg = {0};
    if (nc_init(&cfg) != 0) return 2;
    nc_pad_keys(1);
    ncgfx_init();
    nctext_font *fbig = nctext_open(NULL, 40), *fmed = nctext_open(NULL, 28), *fsm = nctext_open(NULL, 22);
    if (!fbig || !fmed || !fsm) nc_exit(3);
    if (gst_load() != 0) nc_exit(4);

    void *play = NULL, *bus = NULL;
    int sel = 0, playing = 0, paused = 0, info = 1, buffering = 100;
    char status[256] = "";
    time_t info_until = 0;
    if (nent == 1 && strstr(argv[1], "://")) goto start;   /* direct URI: start immediately */

    for (;;) {
        nc_event e;
        while (nc_poll(&e, playing ? 20 : 0)) {
            if (e.type == NC_EV_QUIT) { if (play) g_set_state(play, GST_STATE_NULL); nc_exit(0); }
            if (e.type != NC_EV_KEY || e.value == 0) continue;
            int c = e.code, back = c == KEY_ESC || c == KEY_BACKSPACE;
            int ok = c == KEY_ENTER || c == KEY_KPENTER || c == KEY_OK;
            if (!playing) {
                if (c == KEY_DOWN && sel < nent - 1) sel++;
                else if (c == KEY_UP && sel > 0) sel--;
                else if (back) nc_exit(0);
                else if (ok && nent > 0) {
                start:
                    play = g_factory_make("playbin2", "play");
                    void *vs = g_factory_make("mtkvideosink", "vsink");
                    void *as = g_factory_make("mtkaudiosink", "asink");
                    if (!play || !vs || !as) { snprintf(status, sizeof status, "GStreamer elements missing"); continue; }
                    g_object_set(as, "render-mode", 1, NULL);
                    g_object_set(play, "uri", uris[sel], "video-sink", vs, "audio-sink", as, NULL);
                    bus = g_get_bus(play);
                    g_set_state(play, GST_STATE_PLAYING);
                    playing = 1; paused = 0; buffering = 100; info = 1; info_until = time(0) + 5;
                    snprintf(status, sizeof status, "%s", names[sel]);
                    nc_log("ncplay: %s", uris[sel]);
                }
            } else {
                if (ok) { paused = !paused; g_set_state(play, paused ? GST_STATE_PAUSED : GST_STATE_PLAYING); info = 1; info_until = time(0) + 4; }
                else if (c == KEY_LEFT || c == KEY_RIGHT) {
                    int fmt = GST_FORMAT_TIME;
                    int64_t pos = 0;
                    if (g_query_position(play, &fmt, &pos)) {
                        pos += (c == KEY_RIGHT ? 10 : -10) * GST_SECOND;
                        if (pos < 0) pos = 0;
                        g_seek_simple(play, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, pos);
                    }
                    info = 1; info_until = time(0) + 4;
                } else if (c == KEY_UP || c == KEY_DOWN || c == KEY_INFO) { info = !info; info_until = info ? time(0) + 3600 : 0; }
                else if (back) {
                    g_set_state(play, GST_STATE_NULL);
                    g_obj_unref(bus); g_obj_unref(play);
                    play = bus = NULL; playing = 0;
                    if (nent == 1 && strstr(argv[1], "://")) nc_exit(0);
                }
            }
        }
        /* bus messages */
        if (playing && bus) {
            void *m;
            while ((m = g_bus_pop_filtered(bus, GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_BUFFERING))) {
                int t = MSG_TYPE(m);
                if (t == GST_MESSAGE_ERROR) {
                    GError_ *err = NULL; char *dbg = NULL;
                    g_msg_parse_error(m, &err, &dbg);
                    snprintf(status, sizeof status, "Error: %s", err && err->message ? err->message : "?");
                    nc_log("ncplay: %s (%s)", status, dbg ? dbg : "");
                } else if (t == GST_MESSAGE_EOS) {
                    snprintf(status, sizeof status, "End of stream");
                } else if (t == GST_MESSAGE_BUFFERING) {
                    g_msg_parse_buffering(m, &buffering);
                }
                g_mini_unref(m);
                if (t != GST_MESSAGE_BUFFERING) {   /* stop on EOS/error */
                    g_set_state(play, GST_STATE_NULL);
                    g_obj_unref(bus); g_obj_unref(play);
                    play = bus = NULL; playing = 0;
                    if (nent == 1 && strstr(argv[1], "://")) { nc_log("ncplay: %s", status); nc_exit(0); }
                    break;
                }
            }
        }

        /* ---- draw ---- */
        if (playing) {
            ncgfx_begin(NC_RGBA(0, 0, 0, 0));   /* transparent: the hardware video plane shows through */
            if (info && time(0) > info_until) info = 0;
            if (info || paused || buffering < 100) {
                char pos_s[32], dur_s[32], line[400];
                int fmt = GST_FORMAT_TIME;
                int64_t pos = -1, dur = -1;
                g_query_position(play, &fmt, &pos);
                fmt = GST_FORMAT_TIME;
                g_query_duration(play, &fmt, &dur);
                fmt_time(pos, pos_s, sizeof pos_s);
                fmt_time(dur, dur_s, sizeof dur_s);
                ncgfx_rect(0, 720 - 90, 1280, 90, NC_RGBA(0, 0, 0, 170));
                snprintf(line, sizeof line, "%s%s", paused ? "[paused]  " : "", status);
                nctext_draw(fmed, 40, 720 - 52, line, NC_RGBA(255, 255, 255, 255));
                if (buffering < 100) snprintf(line, sizeof line, "buffering %d%%", buffering);
                else snprintf(line, sizeof line, "%s / %s", pos_s, dur > 0 ? dur_s : "live");
                nctext_draw(fsm, 40, 720 - 18, line, NC_RGBA(200, 205, 220, 255));
                nctext_draw(fsm, 760, 720 - 18, "OK pause  < > seek  BACK stop", NC_RGBA(150, 160, 185, 255));
                if (dur > 0 && pos >= 0) {
                    ncgfx_rect(40, 720 - 86, 1200, 4, NC_RGBA(80, 80, 90, 255));
                    ncgfx_rect(40, 720 - 86, (int)(1200 * (double)pos / dur), 4, NC_RGBA(80, 140, 255, 255));
                }
            }
        } else {
            ncgfx_begin(NC_RGBA(14, 18, 32, 255));
            ncgfx_rect(0, 0, 1280, 100, NC_RGBA(22, 28, 50, 255));
            nctext_draw(fbig, 60, 66, "Media Player", NC_RGBA(255, 255, 255, 255));
            nctext_draw(fsm, 380, 62, "hardware decoding: H.264 / MPEG-2 / MPEG-4", NC_RGBA(150, 170, 210, 255));
            for (int i = 0; i < nent && i < 12; i++) {
                int y = 170 + i * 44;
                if (i == sel) ncgfx_rect(50, y - 32, 1180, 40, NC_RGBA(50, 90, 200, 255));
                nctext_draw(fmed, 80, y - 5, names[i], NC_RGBA(255, 255, 255, 255));
            }
            if (nent == 0) nctext_draw(fmed, 80, 200, "No entries: add Name=URI lines to [player] in netcast.conf", NC_RGBA(220, 220, 220, 255));
            ncgfx_rect(0, 720 - 50, 1280, 50, NC_RGBA(22, 28, 50, 255));
            nctext_draw(fsm, 60, 720 - 18, status[0] ? status : "OK play   BACK exit", NC_RGBA(200, 205, 220, 255));
        }
        nc_swap();
    }
}
