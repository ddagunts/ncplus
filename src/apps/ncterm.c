/*
 * ncterm: terminal for the NetCast TV (libvterm + libnc/GLES, TV's own Courier font).
 *
 *   ncterm [command...]     default: the ncplus chroot shell (netcast/enter.sh) if present,
 *                           otherwise the TV's /bin/sh
 *
 * Input: USB keyboard (full US layout, Ctrl/Shift/Alt, arrows, F-keys, PgUp/PgDn, Home/End).
 *        Remote: arrows, OK = Enter, digits, CH- = Ctrl-C, CH+ = Tab, BACK = Esc.
 * The app exits when the shell exits.
 */
#define _GNU_SOURCE
#include "nc.h"
#include "ncgfx.h"
#include "nctext.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmp.h>
#include "vterm.h"

#define FONT_PX 22

static VTerm *vt;
static VTermScreen *vs;
static int master = -1, dirty = 1, rows, cols, cw, ch, asc;
static pid_t child;
static int shift_l, shift_r, ctrl_l, ctrl_r, alt_l, alt_r, caps;

static void out_cb(const char *s, size_t len, void *u) {
    (void)u;
    while (len > 0) {
        ssize_t n = write(master, s, len);
        if (n <= 0) { if (errno == EINTR || errno == EAGAIN) continue; return; }
        s += n; len -= (size_t)n;
    }
}
static int damage_cb(VTermRect r, void *u) { (void)r; (void)u; dirty = 1; return 1; }
static int movecursor_cb(VTermPos p, VTermPos o, int v, void *u) { (void)p; (void)o; (void)v; (void)u; dirty = 1; return 1; }
static VTermScreenCallbacks cbs = {.damage = damage_cb, .movecursor = movecursor_cb};

static char ascii_for(int code, int shift) {
    static const char *row_lo = "1234567890-=", *row_hi = "!@#$%^&*()_+";
    static const char *q_lo = "qwertyuiop[]", *a_lo = "asdfghjkl;'`", *z_lo = "zxcvbnm,./";
    static const char *q_hi = "QWERTYUIOP{}", *a_hi = "ASDFGHJKL:\"~", *z_hi = "ZXCVBNM<>?";
    if (code >= KEY_1 && code <= KEY_EQUAL) return (shift ? row_hi : row_lo)[code - KEY_1];
    if (code >= KEY_Q && code <= KEY_RIGHTBRACE) return (shift ? q_hi : q_lo)[code - KEY_Q];
    if (code >= KEY_A && code <= KEY_GRAVE) return (shift ? a_hi : a_lo)[code - KEY_A];
    if (code >= KEY_Z && code <= KEY_SLASH) return (shift ? z_hi : z_lo)[code - KEY_Z];
    if (code == KEY_BACKSLASH) return shift ? '|' : '\\';
    if (code == KEY_SPACE) return ' ';
    if (code >= KEY_KP7 && code <= KEY_KP0) {   /* keypad digits */
        static const char kp[] = "789-456+1230";
        return code - KEY_KP7 < 12 ? kp[code - KEY_KP7] : 0;
    }
    return 0;
}

static void handle_key(int code, int value) {
    int down = value != 0;
    switch (code) {
    case KEY_LEFTSHIFT: shift_l = down; return;
    case KEY_RIGHTSHIFT: shift_r = down; return;
    case KEY_LEFTCTRL: ctrl_l = down; return;
    case KEY_RIGHTCTRL: ctrl_r = down; return;
    case KEY_LEFTALT: alt_l = down; return;
    case KEY_RIGHTALT: alt_r = down; return;
    case KEY_CAPSLOCK: if (value == 1) caps = !caps; return;
    default: break;
    }
    if (!down) return;
    VTermModifier mod = VTERM_MOD_NONE;
    int shift = shift_l || shift_r;
    if (shift) mod |= VTERM_MOD_SHIFT;
    if (ctrl_l || ctrl_r) mod |= VTERM_MOD_CTRL;
    if (alt_l || alt_r) mod |= VTERM_MOD_ALT;

    VTermKey k = VTERM_KEY_NONE;
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: case KEY_OK: k = VTERM_KEY_ENTER; break;
    case KEY_BACKSPACE: k = VTERM_KEY_BACKSPACE; break;
    case KEY_TAB: case KEY_CHANNELUP: k = VTERM_KEY_TAB; break;
    case KEY_ESC: k = VTERM_KEY_ESCAPE; break;
    case KEY_UP: k = VTERM_KEY_UP; break;
    case KEY_DOWN: k = VTERM_KEY_DOWN; break;
    case KEY_LEFT: k = VTERM_KEY_LEFT; break;
    case KEY_RIGHT: k = VTERM_KEY_RIGHT; break;
    case KEY_INSERT: k = VTERM_KEY_INS; break;
    case KEY_DELETE: k = VTERM_KEY_DEL; break;
    case KEY_HOME: k = VTERM_KEY_HOME; break;
    case KEY_END: k = VTERM_KEY_END; break;
    case KEY_PAGEUP: k = VTERM_KEY_PAGEUP; break;
    case KEY_PAGEDOWN: k = VTERM_KEY_PAGEDOWN; break;
    case KEY_CHANNELDOWN: vterm_keyboard_unichar(vt, 'c', VTERM_MOD_CTRL); return;   /* remote CH- = ^C */
    default: break;
    }
    if (code >= KEY_F1 && code <= KEY_F10) k = VTERM_KEY_FUNCTION(code - KEY_F1 + 1);
    if (code == KEY_F11) k = VTERM_KEY_FUNCTION(11);
    if (code == KEY_F12) k = VTERM_KEY_FUNCTION(12);
    if (k != VTERM_KEY_NONE) { vterm_keyboard_key(vt, k, mod); return; }

    char c = ascii_for(code, shift);
    if (!c) return;
    if (caps && c >= 'a' && c <= 'z') c = (char)(c - 32);
    else if (caps && c >= 'A' && c <= 'Z' && shift) c = (char)(c + 32);
    /* vterm applies Ctrl/Alt itself; shift is already folded into the character */
    vterm_keyboard_unichar(vt, (uint32_t)(unsigned char)c, mod & ~VTERM_MOD_SHIFT);
}

static uint32_t rgba(VTermColor *c, int is_fg) {
    if (VTERM_COLOR_IS_DEFAULT_FG(c) && is_fg) return NC_RGBA(220, 222, 228, 255);
    if (VTERM_COLOR_IS_DEFAULT_BG(c) && !is_fg) return NC_RGBA(12, 14, 22, 255);
    vterm_screen_convert_color_to_rgb(vs, c);
    return NC_RGBA(c->rgb.red, c->rgb.green, c->rgb.blue, 255);
}

static void draw(nctext_font *f) {
    const int X0 = (1280 - cols * cw) / 2, Y0 = (720 - rows * ch) / 2;
    ncgfx_begin(NC_RGBA(12, 14, 22, 255));
    VTermPos cur;
    vterm_state_get_cursorpos(vterm_obtain_state(vt), &cur);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            VTermScreenCell cell;
            VTermPos p = {r, c};
            if (!vterm_screen_get_cell(vs, p, &cell)) continue;
            VTermColor fg = cell.fg, bg = cell.bg;
            if (cell.attrs.reverse) { VTermColor t = fg; fg = bg; bg = t; }
            int is_cur = r == cur.row && c == cur.col;
            uint32_t bgc = rgba(&bg, cell.attrs.reverse ? 1 : 0), fgc = rgba(&fg, cell.attrs.reverse ? 0 : 1);
            if (is_cur) { uint32_t t = bgc; bgc = NC_RGBA(200, 205, 215, 255); fgc = t; }
            if (bgc != NC_RGBA(12, 14, 22, 255)) ncgfx_rect(X0 + c * cw, Y0 + r * ch, cw * (cell.width ? cell.width : 1), ch, bgc);
            if (cell.chars[0] && cell.chars[0] != ' ') {
                char u8[8] = {0};
                uint32_t cp = cell.chars[0];
                if (cp < 0x80) u8[0] = (char)cp;
                else if (cp < 0x800) { u8[0] = (char)(0xC0 | cp >> 6); u8[1] = (char)(0x80 | (cp & 0x3F)); }
                else { u8[0] = (char)(0xE0 | cp >> 12); u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u8[2] = (char)(0x80 | (cp & 0x3F)); }
                nctext_draw(f, X0 + c * cw, Y0 + r * ch + asc, u8, fgc);
            }
            if (cell.width > 1) c += cell.width - 1;
        }
    }
    nc_swap();
}

int main(int argc, char **argv) {
    char self[512];
    snprintf(self, sizeof self, "%s", argv[0]);
    char *slash = strrchr(self, '/');
    if (slash) *slash = 0;

    nc_config cfg = {0};
    if (nc_init(&cfg) != 0) return 2;
    ncgfx_init();
    char mono[600];
    snprintf(mono, sizeof mono, "%s/DejaVuSansMono.ttf", self);   /* shipped next to the binary */
    nctext_font *f = nctext_open(mono, FONT_PX);
    if (!f) f = nctext_open(NULL, FONT_PX);                         /* LG's (proportional) font */
    if (!f) nc_exit(3);
    cw = nctext_width(f, "M");
    ch = nctext_line_height(f);
    asc = nctext_ascent(f);
    if (cw <= 0) cw = 13;
    cols = (1280 - 40) / cw;
    rows = (720 - 30) / ch;
    nc_log("ncterm: %dx%d cells of %dx%d px", cols, rows, cw, ch);

    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    child = forkpty(&master, NULL, NULL, &ws);
    if (child < 0) { nc_log("ncterm: forkpty failed: %s", strerror(errno)); nc_exit(4); }
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("LD_PRELOAD");
        if (argc > 1) { execvp(argv[1], argv + 1); _exit(127); }
        char enter[600];
        snprintf(enter, sizeof enter, "%s/../../enter.sh", self);
        if (access(enter, R_OK) == 0) execl("/bin/sh", "sh", enter, (char *)NULL);
        execl("/bin/sh", "sh", "-l", (char *)NULL);
        _exit(127);
    }
    fcntl(master, F_SETFL, O_NONBLOCK);

    vt = vterm_new(rows, cols);
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, out_cb, NULL);
    vs = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(vs, &cbs, NULL);
    vterm_screen_enable_altscreen(vs, 1);
    vterm_screen_reset(vs, 1);

    for (;;) {
        char buf[16384];
        ssize_t n;
        while ((n = read(master, buf, sizeof buf)) > 0) vterm_input_write(vt, buf, (size_t)n);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) break;   /* shell gone */
        int st;
        if (waitpid(child, &st, WNOHANG) == child) break;

        nc_event e;
        while (nc_poll(&e, dirty ? 0 : 15)) {
            if (e.type == NC_EV_QUIT) { kill(child, SIGHUP); nc_exit(0); }
            if (e.type == NC_EV_KEY) handle_key(e.code, e.value);
        }
        vterm_screen_flush_damage(vs);
        if (dirty) { dirty = 0; draw(f); }
    }
    nc_log("ncterm: shell exited");
    nc_exit(0);
}
