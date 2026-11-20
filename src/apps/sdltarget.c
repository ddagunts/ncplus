/* sdltarget: checks SDL render-target textures (GL FBOs) on the netcast driver by reading pixels back. */
#include <SDL.h>
#include <stdio.h>
#include <unistd.h>

static void test(SDL_Renderer *ren, int w, int h, Uint32 fmt) {
    SDL_Texture *t = SDL_CreateTexture(ren, fmt, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t) { SDL_Log("%dx%d %s: CreateTexture failed: %s", w, h, SDL_GetPixelFormatName(fmt), SDL_GetError()); return; }
    int r1 = SDL_SetRenderTarget(ren, t);
    SDL_SetRenderDrawColor(ren, 255, 0, 0, 255);
    SDL_RenderClear(ren);
    int r2 = SDL_SetRenderTarget(ren, NULL);
    SDL_SetRenderDrawColor(ren, 0, 0, 255, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, t, NULL, NULL);
    Uint32 px = 0;
    SDL_Rect c = {640, 360, 1, 1};
    int r3 = SDL_RenderReadPixels(ren, &c, SDL_PIXELFORMAT_ARGB8888, &px, 4);
    SDL_RenderPresent(ren);
    SDL_Log("%4dx%-4d %-22s settarget=%d,%d read=%d pixel=%08x -> %s", w, h, SDL_GetPixelFormatName(fmt), r1, r2, r3, px,
            (px & 0xffffff) == 0xff0000 ? "OK (red)" : (px & 0xffffff) == 0x0000ff ? "FAIL (target not drawn)" : "FAIL (other)");
    SDL_DestroyTexture(t);
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_Window *win = SDL_CreateWindow("t", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_TARGETTEXTURE);
    if (!ren) { SDL_Log("renderer: %s", SDL_GetError()); return 1; }
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(ren, &ri);
    SDL_Log("renderer %s flags %x max texture %dx%d", ri.name, ri.flags, ri.max_texture_width, ri.max_texture_height);
    int sizes[][2] = {{256, 256}, {512, 512}, {320, 200}, {960, 800}, {1280, 800}, {1024, 1024}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        test(ren, sizes[i][0], sizes[i][1], SDL_PIXELFORMAT_ARGB8888);
        test(ren, sizes[i][0], sizes[i][1], SDL_PIXELFORMAT_ABGR8888);
    }
    SDL_Quit();
    _exit(0);
}
