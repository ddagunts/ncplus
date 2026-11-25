/*
 * sdldemo: plain SDL2 program running on the NetCast "netcast" video driver.
 * SDL_Renderer (GLES2) + keyboard/remote/mouse input. Nothing NetCast-specific here.
 *   arrows / mouse : move the cursor     Enter / click : burst of particles
 *   Back / Esc     : quit                (also quits after NC_TIMEOUT or argv[1] seconds)
 */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* --- sound: a short descending "pew" synthesised per burst --- */
static volatile float blip_env, blip_freq;
static float phase;
static int arate = 48000;
static void audio_cb(void *u, Uint8 *stream, int len) {
    Sint16 *s = (Sint16 *)stream;
    int frames = len / 4;
    for (int i = 0; i < frames; i++) {
        float v = 0;
        if (blip_env > 0.0005f) {
            phase += blip_freq / arate;
            if (phase > 1) phase -= 1;
            v = (phase < 0.5f ? 1.f : -1.f) * blip_env * 0.25f;
            blip_env *= 0.99985f;
            blip_freq *= 0.99997f;
        }
        s[2 * i] = s[2 * i + 1] = (Sint16)(v * 32767);
    }
}

#define NP 400
typedef struct { float x, y, vx, vy, life; Uint8 r, g, b; } particle;
static particle P[NP];

static void burst(float x, float y) {
    blip_freq = 600 + rand() % 900;
    blip_env = 1.0f;
    for (int i = 0, n = 0; i < NP && n < 60; i++) {
        if (P[i].life > 0) continue;
        float a = (rand() % 628) / 100.f, s = 2 + rand() % 600 / 100.f;
        P[i] = (particle){x, y, cosf(a) * s, sinf(a) * s, 1.f, (Uint8)(128 + rand() % 128), (Uint8)(rand() % 256), 255};
        n++;
    }
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Log("video driver: %s", SDL_GetCurrentVideoDriver());
    SDL_Window *win = SDL_CreateWindow("sdldemo", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
    if (!win) { SDL_Log("CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { SDL_Log("CreateRenderer: %s", SDL_GetError()); return 1; }
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(ren, &ri);
    int W, H;
    SDL_GetRendererOutputSize(ren, &W, &H);
    SDL_Log("renderer: %s, output %dx%d", ri.name, W, H);

    SDL_AudioSpec want = {0}, have;
    want.freq = 48000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (adev) { arate = have.freq; SDL_PauseAudioDevice(adev, 0); SDL_Log("audio: %s, %d Hz", SDL_GetCurrentAudioDriver(), have.freq); }
    else SDL_Log("audio: none (%s)", SDL_GetError());
    burst(W / 2.f, H / 2.f);   /* startup sound */

    /* a procedurally generated texture, to exercise texture upload */
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 256, 256, 32, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
        Uint8 *p = (Uint8 *)s->pixels + y * s->pitch + x * 4;
        p[0] = (Uint8)x; p[1] = (Uint8)y; p[2] = (Uint8)((x ^ y) & 255); p[3] = 255;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);

    float cx = W / 2.f, cy = H / 2.f;
    Uint32 t0 = SDL_GetTicks(), frames = 0;
    int running = 1, keys = 0;
    while (running && SDL_GetTicks() - t0 < (Uint32)secs * 1000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                keys++;
                SDL_Log("key %s (scancode %d)%s", SDL_GetScancodeName(e.key.keysym.scancode), e.key.keysym.scancode,
                        e.key.repeat ? " repeat" : "");
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP: cy -= 40; break;
                case SDL_SCANCODE_DOWN: cy += 40; break;
                case SDL_SCANCODE_LEFT: cx -= 40; break;
                case SDL_SCANCODE_RIGHT: cx += 40; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: burst(cx, cy); break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_AC_BACK: case SDL_SCANCODE_PAGEDOWN: running = 0; break;
                default: break;
                }
                break;
            case SDL_MOUSEMOTION: cx += e.motion.xrel; cy += e.motion.yrel; break;
            case SDL_MOUSEBUTTONDOWN: burst(cx, cy); break;
            case SDL_MOUSEWHEEL: SDL_Log("wheel %d", e.wheel.y); break;
            default: break;
            }
        }
        if (cx < 0) cx = 0; if (cx > W) cx = W; if (cy < 0) cy = 0; if (cy > H) cy = H;

        float t = (SDL_GetTicks() - t0) / 1000.f;
        SDL_SetRenderDrawColor(ren, 12, 16, 30, 255);
        SDL_RenderClear(ren);
        SDL_Rect tr = {W - 300, 40, 256, 256};
        SDL_RenderCopyEx(ren, tex, NULL, &tr, t * 30, NULL, SDL_FLIP_NONE);
        for (int i = 0; i < 12; i++) {   /* orbiting boxes */
            float a = t + i * 0.5236f;
            SDL_Rect r = {(int)(W / 2 + cosf(a) * 260) - 20, (int)(H / 2 + sinf(a) * 180) - 20, 40, 40};
            SDL_SetRenderDrawColor(ren, (Uint8)(120 + 10 * i), (Uint8)(60 + 15 * i), 220, 255);
            SDL_RenderFillRect(ren, &r);
        }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
        for (int i = 0; i < NP; i++) {
            if (P[i].life <= 0) continue;
            P[i].x += P[i].vx; P[i].y += P[i].vy; P[i].vy += 0.08f; P[i].life -= 0.012f;
            SDL_SetRenderDrawColor(ren, P[i].r, P[i].g, P[i].b, (Uint8)(P[i].life * 255));
            SDL_Rect r = {(int)P[i].x - 3, (int)P[i].y - 3, 6, 6};
            SDL_RenderFillRect(ren, &r);
        }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_Rect c = {(int)cx - 10, (int)cy - 10, 20, 20};
        SDL_RenderFillRect(ren, &c);
        SDL_RenderPresent(ren);
        frames++;
    }
    float el = (SDL_GetTicks() - t0) / 1000.f;
    SDL_Log("%u frames in %.1f s (%.1f fps), %d key presses", frames, el, frames / el, keys);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    _exit(0);   /* skip LG library destructors */
}
