#include "ncgfx.h"
#include "nc.h"
#include <GLES2/gl2.h>
#include <stdlib.h>

static GLuint prog_col, prog_tex;
static GLint col_pos, col_color, col_scale;
static GLint tex_pos, tex_uv, tex_scale, tex_tint, tex_mode, tex_samp;
static GLuint tex;

static GLuint sh(GLenum t, const char *s) {
    GLuint o = glCreateShader(t);
    glShaderSource(o, 1, &s, 0);
    glCompileShader(o);
    GLint ok = 0;
    glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512] = ""; glGetShaderInfoLog(o, sizeof l, 0, l); nc_log("ncgfx: shader: %s", l); }
    return o;
}
static GLuint link2(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "uv");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char l[512] = ""; glGetProgramInfoLog(p, sizeof l, 0, l); nc_log("ncgfx: link: %s", l); }
    return p;
}

int ncgfx_init(void) {
    /* pixel -> clip: x' = x*sx - 1, y' = 1 - y*sy (scale = (2/w, 2/h)) */
    const char *vcol = "attribute vec2 pos; uniform vec2 scale;\n"
                       "void main(){ gl_Position = vec4(pos.x*scale.x-1.0, 1.0-pos.y*scale.y, 0.0, 1.0); }\n";
    const char *fcol = "precision mediump float; uniform vec4 color;\n"
                       "void main(){ gl_FragColor = color; }\n";
    const char *vtex = "attribute vec2 pos; attribute vec2 uv; uniform vec2 scale; varying vec2 v;\n"
                       "void main(){ v = uv; gl_Position = vec4(pos.x*scale.x-1.0, 1.0-pos.y*scale.y, 0.0, 1.0); }\n";
    /* mode 0: alpha texture tinted; mode 1: rgba texture */
    const char *ftex = "precision mediump float; varying vec2 v; uniform sampler2D t; uniform vec4 tint; uniform float mode;\n"
                       "void main(){ vec4 s = texture2D(t, v);\n"
                       "  gl_FragColor = mode < 0.5 ? vec4(tint.rgb, tint.a * s.a) : s * tint; }\n";
    prog_col = link2(vcol, fcol);
    col_pos = 0;
    col_color = glGetUniformLocation(prog_col, "color");
    col_scale = glGetUniformLocation(prog_col, "scale");
    prog_tex = link2(vtex, ftex);
    tex_pos = 0;
    tex_uv = 1;
    tex_scale = glGetUniformLocation(prog_tex, "scale");
    tex_tint = glGetUniformLocation(prog_tex, "tint");
    tex_mode = glGetUniformLocation(prog_tex, "mode");
    tex_samp = glGetUniformLocation(prog_tex, "t");
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glEnable(GL_BLEND);
    /* The TV composites this plane over video/LG UI using framebuffer alpha: blend alpha
     * separately (srcA + dstA*(1-srcA)) so opaque stays opaque. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return 0;
}

static void col4(uint32_t c, float *f) {
    f[0] = (c >> 24 & 255) / 255.f; f[1] = (c >> 16 & 255) / 255.f;
    f[2] = (c >> 8 & 255) / 255.f;  f[3] = (c & 255) / 255.f;
}

void ncgfx_begin(uint32_t clear) {
    float c[4];
    col4(clear, c);
    glViewport(0, 0, nc_width(), nc_height());
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

void ncgfx_rect(int x, int y, int w, int h, uint32_t rgba) {
    float c[4];
    col4(rgba, c);
    GLfloat v[] = {x, y, x + w, y, x, y + h, x + w, y + h};
    glUseProgram(prog_col);
    glUniform2f(col_scale, 2.f / nc_width(), 2.f / nc_height());
    glUniform4fv(col_color, 1, c);
    glVertexAttribPointer(col_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(col_pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void tex_quad_uv(int x, int y, int w, int h, float u0, float v0, float u1, float v1, uint32_t tint, float mode) {
    float c[4];
    col4(tint, c);
    GLfloat v[] = {x, y, u0, v0, x + w, y, u1, v0, x, y + h, u0, v1, x + w, y + h, u1, v1};
    glUseProgram(prog_tex);
    glUniform2f(tex_scale, 2.f / nc_width(), 2.f / nc_height());
    glUniform4fv(tex_tint, 1, c);
    glUniform1f(tex_mode, mode);
    glUniform1i(tex_samp, 0);
    glVertexAttribPointer(tex_pos, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(tex_uv, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(tex_pos);
    glEnableVertexAttribArray(tex_uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(tex_uv);
}
static void tex_quad(int x, int y, int w, int h, uint32_t tint, float mode) {
    tex_quad_uv(x, y, w, h, 0, 0, 1, 1, tint, mode);
}

void ncgfx_alpha_bitmap(int x, int y, int w, int h, const uint8_t *a, int pitch, uint32_t rgba) {
    if (w <= 0 || h <= 0) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (pitch == w) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, a);
    } else {
        uint8_t *tmp = malloc((size_t)w * h);
        if (!tmp) return;
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) tmp[r * w + c] = a[r * pitch + c];
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, tmp);
        free(tmp);
    }
    tex_quad(x, y, w, h, rgba, 0.f);
}

void ncgfx_image(int x, int y, int w, int h, const uint8_t *rgba, int dw, int dh) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    tex_quad(x, y, dw, dh, 0xffffffffu, 1.f);
}

void ncgfx_texture(unsigned t, int x, int y, int w, int h, uint32_t rgba, int alpha_only) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    tex_quad(x, y, w, h, rgba, alpha_only ? 0.f : 1.f);
}

void ncgfx_texture_uv(unsigned t, int x, int y, int w, int h, float u0, float v0, float u1, float v1,
                      uint32_t rgba, int alpha_only) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    tex_quad_uv(x, y, w, h, u0, v0, u1, v1, rgba, alpha_only ? 0.f : 1.f);
}
