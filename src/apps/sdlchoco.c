/* sdlchoco: reproduces Chocolate Doom's SDL video setup step by step and reads pixels back
 * after presenting, to find which step makes the picture disappear on the netcast driver. */
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static SDL_Renderer *ren;
static void probe(const char *step) {
    Uint32 px[3] = {0, 0, 0};
    SDL_Rect pts[3] = {{640, 360, 1, 1}, {10, 10, 1, 1}, {200, 360, 1, 1}};
    for (int i = 0; i < 3; i++) SDL_RenderReadPixels(ren, &pts[i], SDL_PIXELFORMAT_ARGB8888, &px[i], 4);
    SDL_Log("%-42s centre %08x corner %08x left %08x", step, px[0], px[1], px[2]);
}
static void audio_cb(void *u, Uint8 *s, int len) { memset(s, 0, len); }

int main(void) {
    /* Doom inits audio (SDL_mixer) before video */
    SDL_Init(SDL_INIT_AUDIO);
    SDL_AudioSpec want = {0}, have;
    want.freq = 44100; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID a = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (a) SDL_PauseAudioDevice(a, 0);
    SDL_Log("audio %s", a ? "open" : SDL_GetError());

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { SDL_Log("video: %s", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow(NULL, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 600,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) { SDL_Log("window: %s", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 320, 240);
    SDL_SetWindowTitle(win, "Freedoom");
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { SDL_Log("renderer: %s", SDL_GetError()); return 1; }
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_Log("window %dx%d output %dx%d flags %08x", ww, wh, ow, oh, SDL_GetWindowFlags(win));

    /* step 1: plain clear, no logical size */
    SDL_SetRenderDrawColor(ren, 255, 0, 0, 255); SDL_RenderClear(ren); probe("1 clear red (no logical size)"); SDL_RenderPresent(ren);
    /* step 2: logical size like aspect_ratio_correct */
    SDL_RenderSetLogicalSize(ren, 320, 240);
    SDL_RenderSetIntegerScale(ren, SDL_FALSE);
    SDL_SetRenderDrawColor(ren, 0, 255, 0, 255); SDL_RenderClear(ren); probe("2 clear green + logical 320x240"); SDL_RenderPresent(ren);
    /* step 3: streaming ARGB texture like Doom's 'texture' (alpha 0 like palette conversion) */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    void *pix; int pitch;
    SDL_LockTexture(tex, NULL, &pix, &pitch);
    for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) ((Uint32 *)((Uint8 *)pix + y * pitch))[x] = 0x00ffff00; /* yellow, alpha 0 */
    SDL_UnlockTexture(tex);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Texture *up = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 960, 800);
    SDL_Log("textures: stream %p target %p (%s)", (void *)tex, (void *)up, SDL_GetError());
    /* step 4: Doom's I_FinishUpdate: copy to upscale target, then target -> screen */
    for (int f = 0; f < 3; f++) {
        SDL_SetRenderTarget(ren, up);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, up, NULL, NULL);
        if (f == 0) probe("4 doom finishupdate (yellow, src alpha 0)");
        SDL_RenderPresent(ren);
    }
    /* keep presenting for a few seconds so it can be seen */
    Uint32 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < 6000) {
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        SDL_SetRenderTarget(ren, up); SDL_RenderCopy(ren, tex, NULL, NULL); SDL_SetRenderTarget(ren, NULL);
        SDL_RenderClear(ren); SDL_RenderCopy(ren, up, NULL, NULL); SDL_RenderPresent(ren);
    }
    probe("5 after 6 s of frames");
    SDL_Quit();
    _exit(0);
}
