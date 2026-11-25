/*
 * ncsettings: ncplus settings, built with LVGL 9 on the SDL2 "netcast" driver.
 *
 * Remote: UP/DOWN move between rows, OK toggles / opens, LEFT/RIGHT change values,
 *         BACK (Escape) closes. Settings are saved to the [settings] section of netcast.conf (on the USB stick)
 *         and applied immediately; start.sh applies them again at every startup.
 *
 *   SAVER_MIN=20   screen saver minutes (0 = off)
 *   NOSLEEP=1      switch LG's Auto Off / No-signal / No-operation timers off (RAM only)
 *   LITE=0         stop optional LG services (browsers, voice, Skype, ...)
 *   NETKEEP=1      Wi-Fi keep-alive (tools/ncnetkeep): ARP the gateway so the router doesn't lose the TV
 *   API=0          remote-control HTTP API (tools/ncapi); its token and port are in [api]
 * The keys, their options and how each is applied are shared with tools/ncapi (libnc/ncconf.c).
 */
#include "lvgl.h"
#include "ncconf.h"
#include <SDL.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <strings.h>

static char base[512], tools[512], conf[512];
static const ncs_def *d_saver, *d_nosleep, *d_lite, *d_netkeep, *d_api;
static lv_group_t *grp;
static lv_obj_t *status_lbl, *info_lbl, *api_lbl, *dd_saver, *sw_nosleep, *sw_lite, *sw_netkeep, *sw_api;

/* conf is netcast.conf (NETCAST_CONF, from run-app.sh): our keys live in its [settings] section.
 * Every change writes only its own key, so changes made through the API at the same time survive. */
static int get(const ncs_def *d) { return ncs_get(conf, d); }
static int set(const ncs_def *d, int v) {
    if (ncs_set(conf, d, v) == 0) return 0;
    SDL_Log("ncsettings: cannot write %s", conf);
    return -1;
}
static int checked(lv_event_t *e) { return lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); }

static void set_status(const char *s) { lv_label_set_text(status_lbl, s); }

static void run(const char *cmd) {
    SDL_Log("ncsettings: %s", cmd);
    int r = system(cmd);
    (void)r;
}

static void wlan_ip(char *ip, size_t n) {
    snprintf(ip, n, "?");
    FILE *p = popen("ifconfig wlan0 2>/dev/null | grep -o 'inet addr:[0-9.]*' | cut -d: -f2", "r");
    if (p) { if (fgets(ip, (int)n, p)) ip[strcspn(ip, "\n")] = 0; pclose(p); }
    if (!*ip) snprintf(ip, n, "?");
}

/* Remote API row: address and token (both needed in Home Assistant / curl) */
static void update_api(void) {
    char ip[64], port[16] = "", tok[80] = "", buf[300];
    ncconf_get(conf, "api", "PORT", port, sizeof port);
    ncconf_get(conf, "api", "TOKEN", tok, sizeof tok);
    if (!get(d_api)) snprintf(buf, sizeof buf, "Control the TV over the network (Home Assistant, curl). Off");
    else {
        wlan_ip(ip, sizeof ip);
        snprintf(buf, sizeof buf, "http://%s:%s  token %s", ip, *port ? port : "9080", *tok ? tok : "(none yet)");
    }
    lv_label_set_text(api_lbl, buf);
}

/* ---- event handlers ---- */
static void on_saver(lv_event_t *e) {
    int v = d_saver->opts[lv_dropdown_get_selected(lv_event_get_target(e))];
    if (set(d_saver, v)) { set_status("Could not save netcast.conf"); return; }
    ncs_apply(d_saver, v, tools);
    set_status(v ? "Screen saver updated" : "Screen saver off");
}

static void on_nosleep(lv_event_t *e) {
    int v = checked(e);
    if (set(d_nosleep, v)) { set_status("Could not save netcast.conf"); return; }
    ncs_apply(d_nosleep, v, tools);
    set_status(v ? "LG power-off timers disabled (until power cycle)"
                 : "LG timers return to LG's settings after the next power cycle");
}

static void on_lite(lv_event_t *e) {
    int v = checked(e);
    if (set(d_lite, v)) { set_status("Could not save netcast.conf"); return; }
    ncs_apply(d_lite, v, tools);
    set_status(v ? "Stopping optional LG services..." : "Restarting LG services...");
}

static void on_netkeep(lv_event_t *e) {
    int v = checked(e);
    if (set(d_netkeep, v)) { set_status("Could not save netcast.conf"); return; }
    ncs_apply(d_netkeep, v, tools);
    set_status(v ? "Wi-Fi keep-alive on" : "Wi-Fi keep-alive off: the router may lose the TV every ~20 min");
}

static void on_api(lv_event_t *e) {
    int v = checked(e);
    if (set(d_api, v)) { set_status("Could not save netcast.conf"); return; }
    ncs_apply(d_api, v, tools);   /* creates the token on first use, (re)starts or stops ncapi */
    update_api();
    set_status(v ? "Remote API on: requests need the token shown above" : "Remote API off");
}

static void on_new_token(lv_event_t *e) {
    (void)e;
    char cmd[700];
    snprintf(cmd, sizeof cmd, "'%s/ncapi' token new", tools);
    run(cmd);
    if (get(d_api)) ncs_apply(d_api, 1, tools);   /* restart with the new token */
    update_api();
    set_status("New API token: update it in Home Assistant");
}

static void on_screen_off(lv_event_t *e) {
    (void)e;
    char cmd[700];
    snprintf(cmd, sizeof cmd, "%s/ncscreen off --wake > /dev/null 2>&1 &", tools);
    run(cmd);
    set_status("Screen off - press any key to wake");
}

static void on_close(lv_event_t *e) { (void)e; exit(0); }

/* Remote navigation, run before the widget's own key handling (LV_EVENT_PREPROCESS): otherwise LVGL
 * toggles switches on UP/DOWN and opens dropdowns on DOWN. UP/DOWN move focus unless a dropdown list
 * is open; LEFT/RIGHT/OK go to the widget; Escape (BACK) closes the list or the app. */
static void on_key(lv_event_t *e) {
    uint32_t k = lv_event_get_key(e);
    lv_obj_t *t = lv_event_get_target(e);
    int dd_open = lv_obj_check_type(t, &lv_dropdown_class) && lv_dropdown_is_open(t);
    if (dd_open) return;                    /* the open list handles UP/DOWN/OK/ESC itself */
    if (k == LV_KEY_ESC) exit(0);
    if (k == LV_KEY_DOWN || k == LV_KEY_UP) {
        if (k == LV_KEY_DOWN) lv_group_focus_next(grp);
        else lv_group_focus_prev(grp);
        lv_event_stop_processing(e);
    }
}

static void update_info(lv_timer_t *t) {
    (void)t;
    struct sysinfo si;
    struct utsname u;
    char ip[64], buf[512];
    sysinfo(&si);
    uname(&u);
    wlan_ip(ip, sizeof ip);
    snprintf(buf, sizeof buf,
             "Kernel %s %s\nUptime %ld h %02ld min\nMemory free %lu MB of %lu MB\nIP %s\nSettings %s",
             u.release, u.machine, si.uptime / 3600, si.uptime / 60 % 60,
             (unsigned long)((si.freeram + si.bufferram) * si.mem_unit >> 20),
             (unsigned long)(si.totalram * si.mem_unit >> 20), ip, conf);
    lv_label_set_text(info_lbl, buf);
}

static lv_obj_t *last_hint;   /* hint label of the last row() */
static lv_obj_t *row(lv_obj_t *parent, const char *title, const char *hint) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x1c2238), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 13, 0);
    lv_obj_t *col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(70), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_t *h = lv_label_create(col);
    lv_label_set_text(h, hint);
    lv_obj_set_style_text_color(h, lv_color_hex(0x9aa6c8), 0);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
    last_hint = h;
    return r;
}

static void focusable(lv_obj_t *o) {
    lv_group_add_obj(grp, o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);   /* the list may be taller than the screen */
    lv_obj_add_event_cb(o, on_key, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    lv_obj_set_style_outline_width(o, 4, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, lv_color_hex(0xffffff), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, 4, LV_STATE_FOCUS_KEY);
}

int main(int argc, char **argv) {
    char self[512];
    snprintf(self, sizeof self, "%s", argv[0]);
    snprintf(base, sizeof base, "%s", dirname(self));
    snprintf(tools, sizeof tools, "%s/../../tools", base);
    const char *nc = getenv("NETCAST_CONF");   /* set by run-app.sh */
    if (nc && *nc) snprintf(conf, sizeof conf, "%s", nc);
    else snprintf(conf, sizeof conf, "%s/../../../netcast.conf", base);
    (void)argc;
    d_saver = ncs_find("SAVER_MIN"); d_nosleep = ncs_find("NOSLEEP"); d_lite = ncs_find("LITE");
    d_netkeep = ncs_find("NETKEEP"); d_api = ncs_find("API");

    lv_init();
    lv_tick_set_cb(SDL_GetTicks);
    lv_display_t *disp = lv_sdl_window_create(1280, 720);
    lv_indev_t *kb = lv_sdl_keyboard_create();
    lv_sdl_mouse_create();
    (void)disp;
    grp = lv_group_create();
    lv_group_set_wrap(grp, true);
    lv_indev_set_group(kb, grp);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0e1220), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, "Settings");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(hdr, lv_color_white(), 0);
    lv_obj_set_pos(hdr, 60, 30);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, 760, 560);
    lv_obj_set_pos(list, 50, 100);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);

    lv_obj_t *r = row(list, "Screen saver", "Backlight off after this long without input");
    dd_saver = lv_dropdown_create(r);
    lv_dropdown_set_options(dd_saver, "Off\n5 min\n10 min\n20 min\n30 min\n60 min");
    for (int i = 0; i < d_saver->nopts; i++)
        if (d_saver->opts[i] == get(d_saver)) lv_dropdown_set_selected(dd_saver, (uint32_t)i);
    lv_obj_set_width(dd_saver, 170);
    lv_obj_add_event_cb(dd_saver, on_saver, LV_EVENT_VALUE_CHANGED, NULL);
    focusable(dd_saver);

    r = row(list, "Keep TV awake", "Disable LG's Auto Off / No-signal timers (RAM only)");
    sw_nosleep = lv_switch_create(r);
    if (get(d_nosleep)) lv_obj_add_state(sw_nosleep, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_nosleep, on_nosleep, LV_EVENT_VALUE_CHANGED, NULL);
    focusable(sw_nosleep);

    r = row(list, "Lite mode", "Stop LG browsers, voice, Skype... to free memory");
    sw_lite = lv_switch_create(r);
    if (get(d_lite)) lv_obj_add_state(sw_lite, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_lite, on_lite, LV_EVENT_VALUE_CHANGED, NULL);
    focusable(sw_lite);

    r = row(list, "Wi-Fi keep-alive", "Stops the TV dropping off the network every ~20 min");
    sw_netkeep = lv_switch_create(r);
    if (get(d_netkeep)) lv_obj_add_state(sw_netkeep, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_netkeep, on_netkeep, LV_EVENT_VALUE_CHANGED, NULL);
    focusable(sw_netkeep);

    r = row(list, "Remote API", "");
    api_lbl = last_hint;
    sw_api = lv_switch_create(r);
    if (get(d_api)) lv_obj_add_state(sw_api, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_api, on_api, LV_EVENT_VALUE_CHANGED, NULL);
    focusable(sw_api);
    update_api();

    r = row(list, "New API token", "Replaces the token: clients using the old one stop working");
    lv_obj_t *bt = lv_button_create(r);
    lv_label_set_text(lv_label_create(bt), "New token");
    lv_obj_add_event_cb(bt, on_new_token, LV_EVENT_CLICKED, NULL);
    focusable(bt);

    r = row(list, "Screen off now", "Backlight off; any key turns it back on");
    lv_obj_t *b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Turn off");
    lv_obj_add_event_cb(b, on_screen_off, LV_EVENT_CLICKED, NULL);
    focusable(b);

    r = row(list, "Done", "Return to the ncplus home screen");
    b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Close");
    lv_obj_add_event_cb(b, on_close, LV_EVENT_CLICKED, NULL);
    focusable(b);

    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 400, 360);
    lv_obj_set_pos(panel, 840, 100);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2238), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_t *t = lv_label_create(panel);
    lv_label_set_text(t, "About this TV");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    info_lbl = lv_label_create(panel);
    lv_obj_set_pos(info_lbl, 0, 50);
    lv_obj_set_width(info_lbl, 360);
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(0xc8d0e8), 0);
    update_info(NULL);
    lv_timer_create(update_info, 3000, NULL);

    status_lbl = lv_label_create(scr);
    lv_obj_set_pos(status_lbl, 60, 675);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x9aa6c8), 0);
    set_status("UP/DOWN move  OK select  LEFT/RIGHT change  BACK close");

    for (;;) {
        uint32_t ms = lv_timer_handler();
        SDL_Delay(ms < 5 ? 5 : ms > 30 ? 30 : ms);
    }
}
