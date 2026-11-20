/* ncgfx: tiny GLES2 2D helper on top of libnc (pixel coordinates, origin top-left). */
#ifndef NCGFX_H
#define NCGFX_H
#include <stdint.h>

int  ncgfx_init(void);                      /* after nc_init(); 0 on success */
void ncgfx_begin(uint32_t clear_rgba);      /* set viewport, clear */
void ncgfx_rect(int x, int y, int w, int h, uint32_t rgba);
/* Draw an 8-bit alpha bitmap (e.g. a glyph) tinted with rgba. */
void ncgfx_alpha_bitmap(int x, int y, int w, int h, const uint8_t *alpha, int pitch, uint32_t rgba);
/* Draw an RGBA8888 image (straight alpha). */
void ncgfx_image(int x, int y, int w, int h, const uint8_t *rgba, int dst_w, int dst_h);

/* Draw an existing GL texture (alpha_only: GL_ALPHA tinted by rgba; else RGBA * rgba). */
void ncgfx_texture(unsigned tex, int x, int y, int w, int h, uint32_t rgba, int alpha_only);

/* Draw a sub-rectangle (u0,v0)-(u1,v1) of a GL texture. */
void ncgfx_texture_uv(unsigned tex, int x, int y, int w, int h, float u0, float v0, float u1, float v1,
                      uint32_t rgba, int alpha_only);

#define NC_RGBA(r, g, b, a) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))
#endif
