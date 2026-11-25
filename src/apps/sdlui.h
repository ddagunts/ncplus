/* sdlui: small SDL2 + SDL2_ttf helpers for ncplus apps (1280x720, cached text textures). */
#pragma once
#include <SDL.h>
#include <SDL_ttf.h>

extern SDL_Renderer *ui_r;
extern int ui_w, ui_h;

enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };

int  ui_init(const char *title, const char *fontdir);   /* 0 = ok; fontdir holds DejaVuSans{,-Bold}.ttf */
void ui_quit(void);
void ui_clear(SDL_Color c);
void ui_fill(int x, int y, int w, int h, SDL_Color c);
/* Draws UTF-8 text with its top at y; ellipsised to maxw (0 = no limit). Returns the drawn width. */
int  ui_text(const char *s, int x, int y, int px, int bold, SDL_Color c, int maxw, int align);
int  ui_text_width(const char *s, int px, int bold);
SDL_Texture *ui_texture_rgba(const unsigned char *rgba, int w, int h);
void ui_present(void);                                   /* also evicts unused cached text */
void ui_format_time(char *out, size_t n, double sec);
void ui_format_size(char *out, size_t n, unsigned long long bytes);

static inline SDL_Color ui_rgb(unsigned hex) {
    SDL_Color c = {(Uint8)(hex >> 16), (Uint8)(hex >> 8), (Uint8)hex, 255};
    return c;
}
