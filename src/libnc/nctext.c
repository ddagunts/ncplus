#include "nctext.h"
#include "nc.h"
#include "ncgfx.h"
#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

/* Glyphs live in one power-of-two GL_ALPHA atlas; every upload is padded to a
 * 4-byte-aligned width (PowerVR drivers mishandle odd-width 1-byte uploads). */
#define ATLAS 1024
typedef struct glyph {
    uint32_t cp;
    int ok;
    int ax, ay;                 /* position in atlas */
    int w, h, left, top, adv;
    struct glyph *next;
} glyph;

#define NBUCKET 256
struct nctext_font {
    FT_Library lib;
    FT_Face face;
    glyph *bucket[NBUCKET];
    GLuint atlas;
    int pen_x, pen_y, row_h;    /* shelf packer */
};

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int n = 0;
    if (c >= 0xf0) { c &= 0x07; n = 3; }
    else if (c >= 0xe0) { c &= 0x0f; n = 2; }
    else if (c >= 0xc0) { c &= 0x1f; n = 1; }
    while (n-- > 0 && (*p & 0xc0) == 0x80) c = (c << 6) | (*p++ & 0x3f);
    *s = (const char *)p;
    return c;
}

nctext_font *nctext_open(const char *path, int px) {
    nctext_font *f = calloc(1, sizeof *f);
    if (!f) return 0;
    if (FT_Init_FreeType(&f->lib) || FT_New_Face(f->lib, path ? path : NCTEXT_DEFAULT_FONT, 0, &f->face)) {
        nc_log("nctext: cannot load font %s", path ? path : NCTEXT_DEFAULT_FONT);
        free(f);
        return 0;
    }
    FT_Set_Pixel_Sizes(f->face, 0, px);
    glGenTextures(1, &f->atlas);
    glBindTexture(GL_TEXTURE_2D, f->atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint8_t *zero = calloc(ATLAS, ATLAS);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE, zero);
    free(zero);
    f->pen_x = f->pen_y = 1;
    return f;
}

void nctext_close(nctext_font *f) {
    if (!f) return;
    for (int i = 0; i < NBUCKET; i++)
        for (glyph *g = f->bucket[i], *n; g; g = n) { n = g->next; free(g); }
    if (f->atlas) glDeleteTextures(1, &f->atlas);
    FT_Done_Face(f->face);
    FT_Done_FreeType(f->lib);
    free(f);
}

static glyph *get_glyph(nctext_font *f, uint32_t cp) {
    glyph **b = &f->bucket[cp % NBUCKET];
    for (glyph *g = *b; g; g = g->next) if (g->cp == cp) return g;
    glyph *g = calloc(1, sizeof *g);
    if (!g) return 0;
    g->cp = cp;
    if (FT_Load_Char(f->face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
        FT_GlyphSlot sl = f->face->glyph;
        g->w = sl->bitmap.width;
        g->h = sl->bitmap.rows;
        g->left = sl->bitmap_left;
        g->top = sl->bitmap_top;
        g->adv = (int)((sl->advance.x + 32) >> 6);
        int pw = (g->w + 3) & ~3;                     /* padded upload width */
        if (g->w > 0 && g->h > 0) {
            if (f->pen_x + pw + 1 > ATLAS) { f->pen_x = 1; f->pen_y += f->row_h + 1; f->row_h = 0; }
            if (f->pen_y + g->h + 1 <= ATLAS) {
                uint8_t *buf = calloc((size_t)pw, g->h);
                if (buf) {
                    for (int r = 0; r < g->h; r++) memcpy(buf + r * pw, sl->bitmap.buffer + r * sl->bitmap.pitch, g->w);
                    glBindTexture(GL_TEXTURE_2D, f->atlas);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, f->pen_x, f->pen_y, pw, g->h, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
                    free(buf);
                    g->ax = f->pen_x;
                    g->ay = f->pen_y;
                    g->ok = 1;
                    f->pen_x += pw + 1;
                    if (g->h > f->row_h) f->row_h = g->h;
                }
            } else nc_log("nctext: glyph atlas full");
        }
    }
    g->next = *b;
    *b = g;
    return g;
}

int nctext_draw(nctext_font *f, int x, int y, const char *s, uint32_t rgba) {
    if (!f || !s) return 0;
    int x0 = x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        glyph *g = get_glyph(f, cp);
        if (!g) continue;
        if (g->ok)
            ncgfx_texture_uv(f->atlas, x + g->left, y - g->top, g->w, g->h, (float)g->ax / ATLAS, (float)g->ay / ATLAS,
                             (float)(g->ax + g->w) / ATLAS, (float)(g->ay + g->h) / ATLAS, rgba, 1);
        x += g->adv;
    }
    return x - x0;
}

int nctext_width(nctext_font *f, const char *s) {
    int w = 0;
    while (f && s && *s) { glyph *g = get_glyph(f, utf8_next(&s)); if (g) w += g->adv; }
    return w;
}
int nctext_line_height(nctext_font *f) { return f ? (int)(f->face->size->metrics.height >> 6) : 0; }
int nctext_ascent(nctext_font *f) { return f ? (int)(f->face->size->metrics.ascender >> 6) : 0; }
