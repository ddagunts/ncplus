/* nctext: FreeType text rendering for libnc/ncgfx (glyphs cached as GL textures). */
#ifndef NCTEXT_H
#define NCTEXT_H
#include <stdint.h>

#define NCTEXT_DEFAULT_FONT "/mnt/lg/res/lgfont/LG_Display.ttf"

typedef struct nctext_font nctext_font;

nctext_font *nctext_open(const char *ttf_path, int pixel_size);   /* NULL on failure */
void nctext_close(nctext_font *f);
/* Draw UTF-8 text with the baseline at y. Returns the advance width in pixels. */
int  nctext_draw(nctext_font *f, int x, int y, const char *utf8, uint32_t rgba);
int  nctext_width(nctext_font *f, const char *utf8);
int  nctext_line_height(nctext_font *f);
int  nctext_ascent(nctext_font *f);
#endif
