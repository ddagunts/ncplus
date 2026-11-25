#include "sdlui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SDL_Renderer *ui_r;
int ui_w = 1280, ui_h = 720;
static SDL_Window *win;
static char fdir[512];

#define NFONTS 24
static struct { int px, bold; TTF_Font *f; } fonts[NFONTS];

#define NCACHE 600
static struct {
    char *s;
    int px, bold, maxw;
    Uint32 rgba;
    SDL_Texture *t;
    int w, h;
    unsigned used;
} cache[NCACHE];
static unsigned frame;
static char *script_keys, *script_pos;   /* NC_KEYS="Down,Down,Return": test-only scripted key presses */
static Uint32 script_next;

int ui_init(const char *title, const char *fontdir) {
    snprintf(fdir, sizeof fdir, "%s", fontdir);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) { SDL_Log("TTF_Init: %s", TTF_GetError()); return -1; }
    win = SDL_CreateWindow(title, 0, 0, ui_w, ui_h, SDL_WINDOW_FULLSCREEN);
    if (!win) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return -1; }
    ui_r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ui_r) ui_r = SDL_CreateRenderer(win, -1, 0);
    if (!ui_r) { SDL_Log("SDL_CreateRenderer: %s", SDL_GetError()); return -1; }
    SDL_RenderSetLogicalSize(ui_r, ui_w, ui_h);
    SDL_SetRenderDrawBlendMode(ui_r, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);
    const char *k = getenv("NC_KEYS");
    if (k && *k) { script_keys = script_pos = SDL_strdup(k); script_next = SDL_GetTicks() + 1500; }
    unsetenv("NC_KEYS");          /* not for apps we start */
    return 0;
}

void ui_quit(void) {
    for (int i = 0; i < NCACHE; i++) if (cache[i].t) SDL_DestroyTexture(cache[i].t);
    for (int i = 0; i < NFONTS; i++) if (fonts[i].f) TTF_CloseFont(fonts[i].f);
    TTF_Quit();
    SDL_Quit();
}

static TTF_Font *font(int px, int bold) {
    int freei = -1;
    for (int i = 0; i < NFONTS; i++) {
        if (fonts[i].f && fonts[i].px == px && fonts[i].bold == bold) return fonts[i].f;
        if (!fonts[i].f && freei < 0) freei = i;
    }
    if (freei < 0) return fonts[0].f;
    char p[640];
    snprintf(p, sizeof p, "%s/%s", fdir, bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf");
    TTF_Font *f = TTF_OpenFont(p, px);
    if (!f) f = TTF_OpenFont("/mnt/lg/res/lgfont/LG_Display.ttf", px);
    if (!f) { SDL_Log("font %s: %s", p, TTF_GetError()); return NULL; }
    TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    fonts[freei].px = px; fonts[freei].bold = bold; fonts[freei].f = f;
    return f;
}

int ui_text_width(const char *s, int px, int bold) {
    TTF_Font *f = font(px, bold);
    int w = 0, h = 0;
    if (f && s && *s) TTF_SizeUTF8(f, s, &w, &h);
    return w;
}

/* Cut s (UTF-8) so that the prefix + "..." fits in maxw. */
static void ellipsise(TTF_Font *f, const char *s, int maxw, char *out, size_t n) {
    int w, h;
    snprintf(out, n, "%s", s);
    if (TTF_SizeUTF8(f, out, &w, &h) == 0 && w <= maxw) return;
    size_t len = strlen(out);
    while (len > 0) {
        do len--; while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80);   /* drop one character */
        snprintf(out, n, "%.*s\xE2\x80\xA6", (int)len, s);
        if (TTF_SizeUTF8(f, out, &w, &h) == 0 && w <= maxw) return;
    }
}

int ui_text(const char *s, int x, int y, int px, int bold, SDL_Color c, int maxw, int align) {
    if (!s || !*s) return 0;
    Uint32 rgba = (Uint32)c.r << 24 | (Uint32)c.g << 16 | (Uint32)c.b << 8 | c.a;
    int slot = -1, oldest = -1;
    for (int i = 0; i < NCACHE; i++) {
        if (cache[i].t && cache[i].px == px && cache[i].bold == bold && cache[i].rgba == rgba &&
            cache[i].maxw == maxw && !strcmp(cache[i].s, s)) { slot = i; break; }
        if (!cache[i].t) { if (oldest < 0 || cache[oldest].t) oldest = i; }
        else if (oldest < 0 || (cache[oldest].t && cache[i].used < cache[oldest].used)) oldest = i;
    }
    if (slot < 0) {
        TTF_Font *f = font(px, bold);
        if (!f) return 0;
        char buf[1024];
        if (maxw > 0) ellipsise(f, s, maxw, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "%s", s);
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *surf = TTF_RenderUTF8_Blended(f, buf, white);
        if (!surf) return 0;
        slot = oldest;
        if (cache[slot].t) { SDL_DestroyTexture(cache[slot].t); SDL_free(cache[slot].s); }
        cache[slot].t = SDL_CreateTextureFromSurface(ui_r, surf);
        cache[slot].w = surf->w; cache[slot].h = surf->h;
        SDL_FreeSurface(surf);
        if (!cache[slot].t) return 0;
        SDL_SetTextureColorMod(cache[slot].t, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(cache[slot].t, c.a);
        cache[slot].s = SDL_strdup(s);
        cache[slot].px = px; cache[slot].bold = bold; cache[slot].rgba = rgba; cache[slot].maxw = maxw;
    }
    cache[slot].used = frame;
    SDL_Rect d = {x, y, cache[slot].w, cache[slot].h};
    if (align == UI_CENTER) d.x -= d.w / 2;
    else if (align == UI_RIGHT) d.x -= d.w;
    SDL_RenderCopy(ui_r, cache[slot].t, NULL, &d);
    return d.w;
}

void ui_clear(SDL_Color c) {
    SDL_SetRenderDrawColor(ui_r, c.r, c.g, c.b, 255);
    SDL_RenderClear(ui_r);
}

void ui_fill(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ui_r, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ui_r, &r);
}

SDL_Texture *ui_texture_rgba(const unsigned char *rgba, int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, rgba, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void script_step(void) {
    if (!script_pos || !*script_pos || SDL_GetTicks() < script_next) return;
    char name[32];
    size_t n = strcspn(script_pos, ",");
    snprintf(name, sizeof name, "%.*s", (int)n, script_pos);
    script_pos += n + (script_pos[n] == ',');
    script_next = SDL_GetTicks() + 700;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc == SDL_SCANCODE_UNKNOWN) { SDL_Log("NC_KEYS: unknown key %s", name); return; }
    SDL_Log("NC_KEYS: %s", name);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

void ui_present(void) {
    SDL_RenderPresent(ui_r);
    script_step();
    frame++;
    for (int i = 0; i < NCACHE; i++)              /* drop text not drawn for ~10 s */
        if (cache[i].t && frame - cache[i].used > 600) {
            SDL_DestroyTexture(cache[i].t); cache[i].t = NULL;
            SDL_free(cache[i].s); cache[i].s = NULL;
        }
}

void ui_format_time(char *out, size_t n, double sec) {
    if (sec < 0) sec = 0;
    long s = (long)sec;
    if (s >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}

void ui_format_size(char *out, size_t n, unsigned long long b) {
    if (b >= 1ULL << 30) snprintf(out, n, "%.1f GB", b / 1073741824.0);
    else if (b >= 1ULL << 20) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else if (b >= 1024) snprintf(out, n, "%.0f KB", b / 1024.0);
    else snprintf(out, n, "%llu B", b);
}
