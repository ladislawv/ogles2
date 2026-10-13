/*
 * M6 test: the extensions (plan Appendix B.3), on a hidden 256x128 bitmap
 * with D24S8. Nothing to watch.
 *   usage: m6_ext
 *   strings     every extension in GL_EXTENSIONS
 *   npot        OES_texture_npot: 100x60 with REPEAT and a mip chain
 *   bgra        EXT_read_format_bgra: glReadPixels as BGRA, bytes and 16-bit
 *   aniso       EXT_texture_filter_anisotropic: parameter, limit, a draw
 *   lod         EXT_texture_lod_bias and SGIS_texture_lod on a 4x4 mip chain
 *               of red, green, blue levels: bias, base and max level, min and
 *               max LOD
 *   mirror      ARB_texture_mirror_clamp_to_edge
 *   deriv       OES_standard_derivatives: dFdx, dFdy, fwidth; the hint
 *   polygon     glPolygonMode: a triangle's edges and corners; glProvokingVertex
 *   bufferoes   glGetBufferParameterivOES
 *   binary      OES_get_program_binary: a program's binary into a new program
 *   float       OES_texture_float, _half_float(_linear): values beyond 0..1,
 *               a float and an RGBA8 texture in one draw (the program's
 *               32-bit-sampler variants, its .v3dw), formats, mipmaps
 *   depth       OES_depth_texture: uploaded and sampled; drawn into by a
 *               framebuffer object, then sampled; a depth-stencil texture
 *               (OES_packed_depth_stencil) for both, the stencil test in it
 *   rect        ARB_texture_rectangle: sampler2DRect in texels, its rules and
 *               errors, a float one, one drawn into
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>
#include <GLES2/gl2ext.h>
#include "../common/smooth_shaders.h"
#include "../common/tex_shaders.h"
#include "../common/ext_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
static struct RastPort rp;
static GLuint smooth, tex1, tex2, texscale, rectp;
#define W 256
#define H 128

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

/* The bound framebuffer at (x, y), GL's coordinates, as 0xRRGGBB. */
static unsigned long px(int x, int y)
{
    GLubyte p[4] = {1, 2, 3, 4};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
}

static int near(int v, int want, int tol)
{
    return v >= want - tol && v <= want + tol;
}

static GLuint link(const char *vs, const char *fs, const char *a0, const char *a1)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    glShaderSource(v, 1, &vs, 0);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(v);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, a0);
    if (a1)
        glBindAttribLocation(p, 1, a1);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* The viewport textured with the bound 2D texture, texture coordinates
 * s0..s1, t0..t1 (tex1 program). */
static void quad_st(float s0, float s1, float t0, float t1)
{
    GLfloat v[20];
    v[0] = -1; v[1] = -1; v[2] = 0;  v[3] = s0;  v[4] = t0;
    v[5] = 1;  v[6] = -1; v[7] = 0;  v[8] = s1;  v[9] = t0;
    v[10] = -1; v[11] = 1; v[12] = 0; v[13] = s0; v[14] = t1;
    v[15] = 1; v[16] = 1; v[17] = 0; v[18] = s1; v[19] = t1;
    glUseProgram(tex1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, v + 3);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1);
}

/* A level of `n`x`n` texels of one colour. */
static void level(GLint l, int n, unsigned long rgb)
{
    GLubyte *p = malloc((size_t)n * n * 4);
    int i;
    for (i = 0; i < n * n; i++) {
        p[i * 4] = (GLubyte)(rgb >> 16);
        p[i * 4 + 1] = (GLubyte)(rgb >> 8);
        p[i * 4 + 2] = (GLubyte)rgb;
        p[i * 4 + 3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
    free(p);
}

static void test_strings(void)
{
    static const char *const want[] = {
        "GL_OES_texture_npot", "GL_EXT_read_format_bgra", "GL_EXT_texture_filter_anisotropic",
        "GL_EXT_texture_lod_bias", "GL_SGIS_texture_lod", "GL_ARB_texture_mirror_clamp_to_edge",
        "GL_OES_standard_derivatives", "GL_OES_get_program_binary", "GL_OES_texture_float",
        "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear", "GL_OES_depth_texture",
        "GL_ARB_texture_rectangle"};
    const char *e = (const char *)glGetString(GL_EXTENSIONS), *v = (const char *)glGetString(GL_VERSION);
    unsigned k;
    char msg[160];
    for (k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        sprintf(msg, "GL_EXTENSIONS has %s", want[k]);
        check(e && strstr(e, want[k]) != 0, msg);
    }
    sprintf(msg, "GL_VERSION \"%s\" (1.2)", v ? v : "(null)");
    check(v && strstr(v, "1.2") != 0, msg);
}

/* OES_texture_npot: REPEAT and a mipmapping filter on 100x60. Left half
 * red, right half green; drawn twice across the viewport (s 0..2). */
static void test_npot(void)
{
    GLuint t;
    GLubyte *p = malloc(100 * 60 * 4);
    unsigned long a, b, c, d;
    char msg[200];
    int x, y;
    for (y = 0; y < 60; y++)
        for (x = 0; x < 100; x++) {
            GLubyte *q = p + (y * 100 + x) * 4;
            q[0] = x < 50 ? 255 : 0;
            q[1] = x < 50 ? 0 : 255;
            q[2] = 0;
            q[3] = 255;
        }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 100, 60, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
    free(p);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_st(0, 2, 0, 1);
    a = px(W / 8, H / 2);       /* s 0.25: red */
    b = px(3 * W / 8, H / 2);   /* s 0.75: green */
    c = px(5 * W / 8, H / 2);   /* s 1.25: red again (REPEAT) */
    d = px(7 * W / 8, H / 2);   /* s 1.75: green */
    sprintf(msg, "100x60, REPEAT, mipmapped: s .25 %06lx .75 %06lx 1.25 %06lx 1.75 %06lx "
                 "(ff0000 00ff00 ff0000 00ff00)", a, b, c, d);
    check(a == 0xFF0000 && b == 0x00FF00 && c == 0xFF0000 && d == 0x00FF00, msg);
    glDeleteTextures(1, &t);
}

/* EXT_read_format_bgra */
static void test_bgra(void)
{
    GLubyte b[8];
    GLushort s[2];
    GLint f = 0, ty = 0;
    char msg[200];
    glViewport(0, 0, W, H);
    glClearColor(1.0f, 0.5f, 0.0f, 1.0f); /* ff8000 */
    glClear(GL_COLOR_BUFFER_BIT);
    memset(b, 0, sizeof(b));
    glReadPixels(10, 10, 2, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, b);
    sprintf(msg, "BGRA bytes: %02x %02x %02x %02x %02x %02x %02x %02x (00 80 ff ff twice)",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    check(b[0] == 0 && near(b[1], 0x80, 1) && b[2] == 0xFF && b[3] == 0xFF && b[4] == 0 && b[6] == 0xFF &&
          glGetError() == GL_NO_ERROR, msg);
    s[0] = s[1] = 0;
    glReadPixels(10, 10, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT, s);
    sprintf(msg, "BGRA 4444_REV: %04x (A=f R=f G=8 B=0 from the top nibble down: ff80 or ff70)", s[0]);
    check((s[0] & 0xFF0F) == 0xFF00 && ((s[0] >> 4) & 15) >= 7 && ((s[0] >> 4) & 15) <= 8, msg);
    glReadPixels(10, 10, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT, s);
    sprintf(msg, "BGRA 1555_REV: %04x (A1 R31 G15/16 B0: fe00 or fc00)", s[0]);
    check((s[0] & 0xFC1F) == 0xFC00, msg);
    glReadPixels(10, 10, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_SHORT_5_6_5, s);
    check(glGetError() == GL_INVALID_OPERATION, "BGRA with 5_6_5: GL_INVALID_OPERATION");
    glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT, s);
    check(glGetError() == GL_INVALID_OPERATION, "RGBA with 4444_REV: GL_INVALID_OPERATION");
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &f);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &ty);
    check(f == GL_RGBA && ty == GL_UNSIGNED_BYTE, "the implementation's read format stays RGBA bytes");
    glClearColor(0, 0, 0, 1);
}

/* A 4x4 texture whose levels are red (4x4), green (2x2), blue (1x1),
 * nearest within and between levels. */
static GLuint rgb_levels(void)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    level(0, 4, 0xFF0000);
    level(1, 2, 0x00FF00);
    level(2, 1, 0x0000FF);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* The 4x4 texture over a 4x4 viewport (LOD 0), its colour. */
static unsigned long lod_draw(void)
{
    glViewport(0, 0, 4, 4);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_st(0, 1, 0, 1);
    return px(1, 1);
}

static void test_aniso(void)
{
    GLuint t = rgb_levels();
    GLfloat f = 0, m = 0;
    GLint i = 0;
    char msg[160];
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m);
    glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &f);
    sprintf(msg, "max anisotropy %.1f (16), the texture's %.1f (1)", (double)m, (double)f);
    check(m == 16.0f && f == 1.0f, msg);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 0.5f);
    check(glGetError() == GL_INVALID_VALUE, "anisotropy 0.5: GL_INVALID_VALUE");
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 64.0f);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &i);
    check(i == 16, "anisotropy 64: clamped to 16");
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 16.0f);
    check(lod_draw() == 0xFF0000, "drawn with anisotropy 16: level 0, red");
    glDeleteTextures(1, &t);
}

static void test_lod(void)
{
    GLuint t = rgb_levels();
    GLfloat f = 0;
    GLint b = -1, m = -1;
    unsigned long c[8];
    char msg[240];
    glGetFloatv(GL_MAX_TEXTURE_LOD_BIAS, &f);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &b);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &m);
    sprintf(msg, "max LOD bias %.1f (15), base level %d (0), max level %d (1000)", (double)f, b, m);
    check(f == 15.0f && b == 0 && m == 1000, msg);
    c[0] = lod_draw();
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 1.0f);
    c[1] = lod_draw();
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 2.0f);
    c[2] = lod_draw();
    glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, &f);
    sprintf(msg, "LOD bias 0, 1, 2: %06lx %06lx %06lx (ff0000 00ff00 0000ff), queried %.1f", c[0], c[1], c[2],
            (double)f);
    check(c[0] == 0xFF0000 && c[1] == 0x00FF00 && c[2] == 0x0000FF && f == 2.0f, msg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    c[3] = lod_draw();                                        /* bias 2, max level 1: green */
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 0.0f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
    c[4] = lod_draw();                                        /* base level 1: green */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 2.0f);
    c[5] = lod_draw();                                        /* min LOD 2: blue */
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 2.0f);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1.0f);
    c[6] = lod_draw();                                        /* bias 2, max LOD 1: green */
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 0.0f);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 2);
    c[7] = lod_draw();                                        /* no mipmapping, base level 2: blue */
    sprintf(msg, "max level 1 with bias 2 %06lx (00ff00), base level 1 %06lx (00ff00), min LOD 2 %06lx (0000ff), "
                 "max LOD 1 with bias 2 %06lx (00ff00), base level 2 unmipmapped %06lx (0000ff)",
            c[3], c[4], c[5], c[6], c[7]);
    check(c[3] == 0x00FF00 && c[4] == 0x00FF00 && c[5] == 0x0000FF && c[6] == 0x00FF00 && c[7] == 0x0000FF, msg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, -1);
    check(glGetError() == GL_INVALID_VALUE, "base level -1: GL_INVALID_VALUE");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 3);
    check(lod_draw() == 0, "base level 3, beyond the chain: incomplete, black");
    glDeleteTextures(1, &t);
    glViewport(0, 0, W, H);
}

/* ARB_texture_mirror_clamp_to_edge: a 2x1 texture, red then green, s from
 * -2 to 2 across the viewport. Mirrored once: -0.75 reads 0.75 (green),
 * -0.25 reads 0.25 (red); beyond -1 and 1 the edge: green. */
static void test_mirror(void)
{
    static const GLubyte rg[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    GLuint t;
    GLint e = 0;
    unsigned long a, b, c, d;
    char msg[200];
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRROR_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &e);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_st(-2, 2, 0, 1);
    a = px(W * 5 / 16, H / 2); /* s -0.75 */
    b = px(W * 7 / 16, H / 2); /* s -0.25 */
    c = px(W / 16, H / 2);     /* s -1.75 */
    d = px(W * 15 / 16, H / 2); /* s 1.75 */
    sprintf(msg, "mirror clamp: s -0.75 %06lx (00ff00), -0.25 %06lx (ff0000), -1.75 %06lx (00ff00), "
                 "1.75 %06lx (00ff00); queried 0x%04x", a, b, c, d, e);
    check(a == 0x00FF00 && b == 0xFF0000 && c == 0x00FF00 && d == 0x00FF00 && e == GL_MIRROR_CLAMP_TO_EDGE, msg);
    glDeleteTextures(1, &t);
}

static void test_deriv(void)
{
    static const GLfloat v[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    GLuint p = link(deriv_vs, deriv_fs, "a_position", 0), f, t;
    GLint h = 0;
    unsigned long a, b;
    char msg[200];
    check(p != 0, "the derivatives program (#extension GL_OES_standard_derivatives) links");
    glHint(GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES, GL_NICEST);
    glGetIntegerv(GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES, &h);
    check(h == GL_NICEST && glGetError() == GL_NO_ERROR, "the derivative hint: set and queried");
    if (!p)
        return;
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    a = px(W / 3, H / 3);
    sprintf(msg, "default framebuffer: dFdx, dFdy, fwidth %06lx (each ~40)", a);
    check(near((int)(a >> 16), 64, 3) && near((int)((a >> 8) & 255), 64, 3) && near((int)(a & 255), 64, 3), msg);
    /* The same into a framebuffer object (not drawn upside down). */
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    b = px(W / 3, H / 3);
    sprintf(msg, "framebuffer object: %06lx (each ~40: dFdy positive there too)", b);
    check(near((int)(b >> 16), 64, 3) && near((int)((b >> 8) & 255), 64, 3) && near((int)(b & 255), 64, 3), msg);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);
    glUseProgram(0);
    glDeleteProgram(p);
}

/* glPolygonMode: a large triangle; its inside, an edge, a corner. */
static void test_polygon(void)
{
    static const GLfloat v[] = {-0.8f, -0.8f, 0, 0.8f, -0.8f, 0, -0.8f, 0.8f, 0};
    GLint m[2] = {0, 0}, pv = 0;
    unsigned long in[3], edge[3], corner[3];
    char msg[240];
    int k;
    static const GLenum modes[3] = {GL_FILL, GL_LINE, GL_POINT};
    glGetIntegerv(GL_POLYGON_MODE, m);
    check(m[0] == GL_FILL && m[1] == GL_FILL, "GL_POLYGON_MODE: GL_FILL at first");
    glViewport(0, 0, W, H);
    glUseProgram(smooth);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, 1, 1, 0, 1);
    glLineWidth(3.0f);
    for (k = 0; k < 3; k++) {
        glPolygonMode(GL_FRONT_AND_BACK, modes[k]);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        /* NDC to pixels: x (n+1)*128, y (n+1)*64 */
        in[k] = px(W / 4, H / 4);                         /* (-0.5, -0.5): inside */
        edge[k] = px(W / 2, 13);                          /* (0, -0.8) is y 12.8: the bottom edge */
        corner[k] = px((int)(0.1f * W), (int)(0.1f * H)); /* (-0.8, -0.8): a corner */
    }
    glLineWidth(1.0f);
    sprintf(msg, "fill: inside %06lx edge %06lx; line: inside %06lx edge %06lx; point: inside %06lx edge %06lx "
                 "corner %06lx (ffff00 ffff00 000000 ffff00 000000 000000 ffff00)",
            in[0], edge[0], in[1], edge[1], in[2], edge[2], corner[2]);
    check(in[0] == 0xFFFF00 && edge[0] == 0xFFFF00 && in[1] == 0 && edge[1] == 0xFFFF00 && in[2] == 0 &&
              edge[2] == 0 && corner[2] == 0xFFFF00, msg);
    glPolygonMode(GL_FRONT, GL_LINE);
    check(glGetError() == GL_INVALID_OPERATION, "GL_FRONT alone: GL_INVALID_OPERATION (one mode for both faces)");
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINTS);
    check(glGetError() == GL_INVALID_ENUM, "mode GL_POINTS: GL_INVALID_ENUM");
    glGetIntegerv(GL_POLYGON_MODE, m);
    check(m[0] == GL_POINT, "GL_POLYGON_MODE: GL_POINT");
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glClear(GL_COLOR_BUFFER_BIT); /* drawn as a rectangle when scissored: filled whatever the mode */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, W / 2, H);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    check(px(W / 4, H / 2) == 0x0000FF, "a scissored clear in GL_LINE mode still fills");
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glGetIntegerv(GL_PROVOKING_VERTEX, &pv);
    check(pv == GL_LAST_VERTEX_CONVENTION, "GL_PROVOKING_VERTEX: last at first");
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    glGetIntegerv(GL_PROVOKING_VERTEX, &pv);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    check(pv == GL_FIRST_VERTEX_CONVENTION && px(W / 4, H / 4) == 0xFFFF00, "first-vertex convention: set, draws");
    glProvokingVertex(GL_FILL);
    check(glGetError() == GL_INVALID_ENUM, "glProvokingVertex(GL_FILL): GL_INVALID_ENUM");
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
}

static void test_bufferoes(void)
{
    GLuint b;
    GLint a = 0, c = 0;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, 1234, 0, GL_STATIC_DRAW);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &a);
    glGetBufferParameterivOES(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &c);
    check(a == 1234 && c == 1234, "glGetBufferParameterivOES: the size, as glGetBufferParameteriv");
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &b);
}

/* OES_get_program_binary: tex1's binary into a new program, which draws
 * as tex1 does; a refused binary; the errors. */
static void test_binary(void)
{
    GLint len = 0, n = 0, fmt = 0, ok = -1, loc = -1, got = 0;
    GLenum bf = 0;
    GLuint p, t;
    void *bin;
    char log[200], msg[300];
    static const GLubyte texel[4] = {0, 0, 255, 255};
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_OES, &n);
    glGetIntegerv(GL_PROGRAM_BINARY_FORMATS_OES, &fmt);
    glGetProgramiv(tex1, GL_PROGRAM_BINARY_LENGTH_OES, &len);
    sprintf(msg, "%d format (1), 0x%08x (56334450), tex1's binary %d bytes", n, (unsigned)fmt, len);
    check(n == 1 && fmt == 0x56334450 && len > 100, msg);
    if (len <= 0 || !(bin = malloc((size_t)len)))
        return;
    glGetProgramBinaryOES(tex1, len - 1, &got, &bf, bin);
    check(glGetError() == GL_INVALID_OPERATION, "a buffer one byte short: GL_INVALID_OPERATION");
    glGetProgramBinaryOES(tex1, len, &got, &bf, bin);
    check(got == len && bf == 0x56334450 && !memcmp(bin, "V3DP", 4), "glGetProgramBinaryOES: the .v3dp file");
    p = glCreateProgram();
    glProgramBinaryOES(p, 0x1234, bin, len);
    check(glGetError() == GL_INVALID_ENUM, "another format: GL_INVALID_ENUM");
    glProgramBinaryOES(p, bf, bin, len);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    loc = glGetUniformLocation(p, "u_tex");
    check(ok == GL_TRUE && loc >= 0, "glProgramBinaryOES: linked, u_tex has a location");
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        GLuint keep = tex1;
        tex1 = p;
        glUseProgram(p);
        glUniform1i(loc, 0);
        quad_st(0, 1, 0, 1);
        tex1 = keep;
    }
    check(px(W / 2, H / 2) == 0x0000FF, "the program from the binary draws the texture (0000ff)");
    ((GLubyte *)bin)[40] ^= 0xFF; /* the CRC then fails */
    glProgramBinaryOES(p, bf, bin, len);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    log[0] = 0;
    glGetProgramInfoLog(p, sizeof(log), 0, log);
    sprintf(msg, "a damaged binary: not linked, no GL error; log \"%.80s\"", log);
    check(ok == GL_FALSE && log[0] && glGetError() == GL_NO_ERROR, msg);
    glUseProgram(0);
    glDeleteProgram(p);
    glDeleteTextures(1, &t);
    free(bin);
}

/* The colour at the viewport's centre with texture t, scaled by (a, b, c, d)
 * (texscale program). */
static unsigned long scaled(GLuint t, float a, float b, float c, float d, int x, int y)
{
    GLuint keep = tex1;
    glBindTexture(GL_TEXTURE_2D, t);
    glUseProgram(texscale);
    glUniform4f(glGetUniformLocation(texscale, "u_scale"), a, b, c, d);
    tex1 = texscale;
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_st(0, 1, 0, 1);
    tex1 = keep;
    return px(x, y);
}

static int rgb_near(unsigned long c, int r, int g, int b, int tol)
{
    return near((int)(c >> 16), r, tol) && near((int)((c >> 8) & 255), g, tol) && near((int)(c & 255), b, tol);
}

/* OES_texture_float and OES_texture_half_float. */
static void test_float(void)
{
    static const GLfloat rgba[8] = {3.0f, 1000.0f, -1.0f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    static const GLfloat lum[2] = {0.5f, 0.25f};
    static const GLfloat la[4] = {0.25f, 0.75f, 0.5f, 1.0f};
    GLushort half[8], hlin[8];
    static const GLubyte orange[4] = {255, 128, 0, 255};
    GLuint t, u, h, l;
    unsigned long a, b, c;
    char msg[240];
    int k;
    /* 2x1: (3, 1000, -1, 1) and (0.25, 0.5, 0.75, 1) */
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_FLOAT, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    check(glGetError() == GL_NO_ERROR, "RGBA float 2x1: glTexImage2D");
    a = scaled(t, 0.25f, 0.0005f, -0.5f, 1.0f, W / 4, H / 2);
    b = scaled(t, 1, 1, 1, 1, 3 * W / 4, H / 2);
    sprintf(msg, "sampled as floats: (3, 1000, -1) x (.25, .0005, -.5) %06lx (bf8080), (.25, .5, .75) %06lx (4080bf)",
            a, b);
    check(rgb_near(a, 191, 128, 128, 2) && rgb_near(b, 64, 128, 191, 2), msg);
    /* A float texture and an RGBA8 one in one draw: both read through the
     * 32-bit variants. (tex2: u_a times u_b) */
    glGenTextures(1, &u);
    glBindTexture(GL_TEXTURE_2D, u);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, orange);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, u);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    {
        GLuint keep = tex1;
        glUseProgram(tex2);
        glUniform1i(glGetUniformLocation(tex2, "u_a"), 0);
        glUniform1i(glGetUniformLocation(tex2, "u_b"), 1);
        tex1 = tex2;
        glViewport(0, 0, W, H);
        glClear(GL_COLOR_BUFFER_BIT);
        quad_st(0, 1, 0, 1);
        tex1 = keep;
    }
    c = px(3 * W / 4, H / 2); /* (.25, .5, .75) x (1, .5, 0) */
    sprintf(msg, "a float and an RGBA8 texture multiplied: %06lx (404000)", c);
    check(rgb_near(c, 64, 64, 0, 2), msg);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    /* the RGBA8 one alone again: the 16-bit variants */
    check(scaled(u, 1, 1, 1, 1, W / 2, H / 2) == 0xFF8000, "then an RGBA8 texture alone: ff8000");
    /* glTexSubImage2D, and data of another type refused */
    glBindTexture(GL_TEXTURE_2D, t);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 1, 1, GL_RGBA, GL_FLOAT, rgba);
    a = scaled(t, 0.25f, 0.0005f, -0.5f, 1.0f, 3 * W / 4, H / 2);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, orange);
    sprintf(msg, "glTexSubImage2D with floats %06lx (bf8080); with bytes: GL_INVALID_OPERATION", a);
    check(rgb_near(a, 191, 128, 128, 2) && glGetError() == GL_INVALID_OPERATION, msg);
    /* linear filtering asked for: 32-bit floats are sampled nearest */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    a = scaled(t, 0.25f, 0.0005f, -0.5f, 1.0f, W / 2 + 4, H / 2);
    sprintf(msg, "GL_LINEAR on 32-bit floats: nearest, %06lx (bf8080)", a);
    check(rgb_near(a, 191, 128, 128, 2), msg);
    glDeleteTextures(1, &t);
    glDeleteTextures(1, &u);
    /* LUMINANCE and LUMINANCE_ALPHA floats */
    glGenTextures(1, &l);
    glBindTexture(GL_TEXTURE_2D, l);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2, 1, 0, GL_LUMINANCE, GL_FLOAT, lum);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    a = scaled(l, 1, 1, 1, 1, W / 4, H / 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 2, 1, 0, GL_LUMINANCE_ALPHA, GL_FLOAT, la);
    b = scaled(l, 1, 1, 1, 1, 3 * W / 4, H / 2);
    sprintf(msg, "LUMINANCE float %06lx (808080), LUMINANCE_ALPHA float %06lx (808080)", a, b);
    check(rgb_near(a, 128, 128, 128, 1) && rgb_near(b, 128, 128, 128, 1), msg);
    /* glGenerateMipmap on floats: 4x4 of 0.5 drawn small (level 1 or 2) */
    {
        GLfloat q[64];
        for (k = 0; k < 64; k++)
            q[k] = 0.5f;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_FLOAT, q);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 2.0f);
        a = scaled(l, 1, 1, 1, 1, W / 2, H / 2);
        sprintf(msg, "glGenerateMipmap on floats, level 2 sampled: %06lx (808080)", a);
        check(rgb_near(a, 128, 128, 128, 1) && glGetError() == GL_NO_ERROR, msg);
    }
    glDeleteTextures(1, &l);
    /* Half floats: 3.0, 1000 and -1 again; and linear filtering, which they have */
    for (k = 0; k < 8; k++)
        half[k] = 0;
    half[0] = 0x4200; /* 3.0 */
    half[1] = 0x63D0; /* 1000 */
    half[2] = 0xBC00; /* -1.0 */
    half[3] = 0x3C00; /* 1.0 */
    glGenTextures(1, &h);
    glBindTexture(GL_TEXTURE_2D, h);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_HALF_FLOAT_OES, half);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    a = scaled(h, 0.25f, 0.0005f, -0.5f, 1.0f, W / 2, H / 2);
    for (k = 0; k < 8; k++)
        hlin[k] = 0x3C00; /* alpha and the rest 1 */
    hlin[0] = 0x0000;    /* red 0, then 1: halfway 0.5 */
    hlin[4] = 0x3C00;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_HALF_FLOAT_OES, hlin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    b = scaled(h, 1, 1, 1, 1, W / 2, H / 2);
    sprintf(msg, "half floats: (3, 1000, -1) scaled %06lx (bf8080); linear between 0 and 1 %06lx (~80ffff)", a, b);
    check(rgb_near(a, 191, 128, 128, 2) && near((int)(b >> 16), 128, 6) && (b & 0xFFFF) == 0xFFFF, msg);
    glDeleteTextures(1, &h);
    /* errors */
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, 1, 1, 0, GL_BGRA_EXT, GL_FLOAT, rgba);
    check(glGetError() == GL_INVALID_OPERATION, "BGRA floats: GL_INVALID_OPERATION");
    glDeleteTextures(1, &t);
}

/* OES_depth_texture (and OES_packed_depth_stencil's texture). */
static void test_depth(void)
{
    static const GLushort d16[4] = {0x8000, 0x8000, 0x8000, 0x8000};
    static const GLuint d32[4] = {0x40000000, 0x40000000, 0x40000000, 0x40000000};
    GLuint t, c, f, ds;
    GLint bits = 0;
    unsigned long a, b;
    char msg[240];
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, d16);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    a = scaled(t, 1, 1, 1, 1, W / 2, H / 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, d32);
    b = scaled(t, 1, 1, 1, 1, W / 2, H / 2);
    sprintf(msg, "depth textures sampled as luminance: 16-bit 0.5 %06lx (808080), 24-bit 0.25 %06lx (404040)", a, b);
    check(rgb_near(a, 128, 128, 128, 1) && rgb_near(b, 64, 64, 64, 1), msg);
    glGenerateMipmap(GL_TEXTURE_2D);
    check(glGetError() == GL_INVALID_OPERATION, "glGenerateMipmap on a depth texture: GL_INVALID_OPERATION");
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, d16);
    check(glGetError() == GL_INVALID_OPERATION, "DEPTH_COMPONENT with UNSIGNED_BYTE: GL_INVALID_OPERATION");
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_DEPTH_COMPONENT, 2, 2, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_SHORT, d16);
    check(glGetError() == GL_INVALID_OPERATION, "a depth cube face: GL_INVALID_OPERATION");
    glDeleteTextures(1, &t);

    /* Rendered into: a 64x64 framebuffer object with an RGBA8 colour
     * texture and a depth texture; depth cleared to 1, a rectangle at
     * z 0 (depth 0.5) on the left half; then the depth texture sampled. */
    {
        static const GLenum types[2] = {GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        int k;
        for (k = 0; k < 2; k++) {
            glGenTextures(1, &c);
            glBindTexture(GL_TEXTURE_2D, c);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 64, 64, 0, GL_DEPTH_COMPONENT, types[k], 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenFramebuffers(1, &f);
            glBindFramebuffer(GL_FRAMEBUFFER, f);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t, 0);
            glGetIntegerv(GL_DEPTH_BITS, &bits);
            sprintf(msg, "%s depth texture attached: complete, depth bits %d",
                    k ? "a 24-bit" : "a 16-bit", bits);
            check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && bits == (k ? 24 : 16), msg);
            glViewport(0, 0, 64, 64);
            glEnable(GL_DEPTH_TEST);
            glClearDepthf(1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            {
                static const GLfloat v[12] = {-1, -1, 0, 0, -1, 0, -1, 1, 0, 0, 1, 0};
                glUseProgram(smooth);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
                glEnableVertexAttribArray(0);
                glDisableVertexAttribArray(1);
                glVertexAttrib4f(1, 1, 1, 1, 1);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            glDisable(GL_DEPTH_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            a = scaled(t, 1, 1, 1, 1, W / 4, H / 2);
            b = scaled(t, 1, 1, 1, 1, 3 * W / 4, H / 2);
            sprintf(msg, "%s depth texture drawn into, then sampled: left %06lx (808080), right %06lx (ffffff)",
                    k ? "24-bit" : "16-bit", a, b);
            check(rgb_near(a, 128, 128, 128, 2) && b == 0xFFFFFF, msg);
            glDeleteFramebuffers(1, &f);
            glDeleteTextures(1, &t);
            glDeleteTextures(1, &c);
        }
    }

    /* A DEPTH_STENCIL_OES texture for both depth and stencil: the stencil
     * test works in it, and its depth is sampled. */
    glGenTextures(1, &c);
    glBindTexture(GL_TEXTURE_2D, c);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenTextures(1, &ds);
    glBindTexture(GL_TEXTURE_2D, ds);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_STENCIL_OES, 64, 64, 0, GL_DEPTH_STENCIL_OES, GL_UNSIGNED_INT_24_8_OES, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ds, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds, 0);
    glGetIntegerv(GL_STENCIL_BITS, &bits);
    sprintf(msg, "a depth-stencil texture for both: complete, stencil bits %d", bits);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && bits == 8, msg);
    glViewport(0, 0, 64, 64);
    glClearStencil(0);
    glClearDepthf(1.0f);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    {
        static const GLfloat left[12] = {-1, -1, 0, 0, -1, 0, -1, 1, 0, 0, 1, 0};
        static const GLfloat all[12] = {-1, -1, 0.5f, 1, -1, 0.5f, -1, 1, 0.5f, 1, 1, 0.5f};
        glUseProgram(smooth);
        glEnableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glVertexAttrib4f(1, 0, 0, 1, 1);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, left);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* stencil 1, depth 0.5 on the left */
        glDisable(GL_DEPTH_TEST);
        glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
        glVertexAttrib4f(1, 0, 1, 0, 1);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, all);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* green where the stencil is not 1: the right */
        glDisable(GL_STENCIL_TEST);
    }
    a = px(16, 32);
    b = px(48, 32);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sprintf(msg, "in it: the stencil test, left %06lx (0000ff) right %06lx (00ff00)", a, b);
    check(a == 0x0000FF && b == 0x00FF00, msg);
    a = scaled(ds, 1, 1, 1, 1, W / 4, H / 2);
    b = scaled(ds, 1, 1, 1, 1, 3 * W / 4, H / 2);
    sprintf(msg, "its depth sampled: left %06lx (808080), right %06lx (ffffff)", a, b);
    check(rgb_near(a, 128, 128, 128, 2) && b == 0xFFFFFF, msg);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ds, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT,
          "a depth texture as the colour image: incomplete attachment");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &ds);
    glDeleteTextures(1, &c);
    glViewport(0, 0, W, H);
}

/* The viewport drawn with the rect program and rectangle texture t. */
static void rect_draw(GLuint t)
{
    GLuint keep = tex1;
    glBindTexture(GL_TEXTURE_RECTANGLE, t);
    glUseProgram(rectp);
    tex1 = rectp;
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_st(0, 1, 0, 1);
    tex1 = keep;
}

/* ARB_texture_rectangle: a 4x2 texture addressed in texels (rect program:
 * texture2DRect at v_tc * (4, 2)); bottom row red, green, blue, white; top
 * row yellow, cyan, magenta, grey. */
static void test_rect(void)
{
    static const GLubyte tex[32] = {255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   255, 255, 255, 255,
                                    255, 255, 0, 255, 0, 255, 255, 255, 255, 0, 255, 255, 128, 128, 128, 255};
    static const unsigned long want[8] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF,
                                          0xFFFF00, 0x00FFFF, 0xFF00FF, 0x808080};
    static const GLfloat ftex[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    GLuint t, f, c;
    GLint v[4] = {0, 0, 0, 0}, mx = 0, b = 0;
    unsigned long got[8];
    char msg[300];
    int k, ok = 1;
    if (!rectp) {
        check(0, "the rect program (sampler2DRect) links");
        return;
    }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_RECTANGLE, t);
    glGetTexParameteriv(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, &v[0]);
    glGetTexParameteriv(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, &v[1]);
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &b);
    glGetIntegerv(GL_MAX_RECTANGLE_TEXTURE_SIZE, &mx);
    sprintf(msg, "a rectangle texture: min filter 0x%04x (2601 LINEAR), wrap 0x%04x (812f), bound %d, max size %d",
            v[0], v[1], b, mx);
    check(v[0] == GL_LINEAR && v[1] == GL_CLAMP_TO_EDGE && b == (GLint)t && mx == 4096, msg);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    rect_draw(t);
    for (k = 0; k < 8; k++) {
        got[k] = px((k % 4) * W / 4 + W / 8, (k / 4) * H / 2 + H / 4);
        if (got[k] != want[k])
            ok = 0;
    }
    sprintf(msg, "sampled in texels: %06lx %06lx %06lx %06lx / %06lx %06lx %06lx %06lx", got[0], got[1], got[2],
            got[3], got[4], got[5], got[6], got[7]);
    check(ok, msg);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 1, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    check(glGetError() == GL_INVALID_VALUE, "level 1: GL_INVALID_VALUE");
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_REPEAT);
    check(glGetError() == GL_INVALID_ENUM, "GL_REPEAT: GL_INVALID_ENUM");
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    check(glGetError() == GL_INVALID_ENUM, "a mipmapping filter: GL_INVALID_ENUM");
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_BASE_LEVEL, 1);
    check(glGetError() == GL_INVALID_OPERATION, "base level 1: GL_INVALID_OPERATION");
    glGenerateMipmap(GL_TEXTURE_RECTANGLE);
    check(glGetError() == GL_INVALID_ENUM, "glGenerateMipmap: GL_INVALID_ENUM");
    glBindTexture(GL_TEXTURE_2D, t);
    check(glGetError() == GL_INVALID_OPERATION, "the name bound as 2D: GL_INVALID_OPERATION");
    /* glTexSubImage2D: the top right texel black */
    {
        static const GLubyte black[4] = {0, 0, 0, 255};
        glBindTexture(GL_TEXTURE_RECTANGLE, t);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 3, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        rect_draw(t);
        check(px(7 * W / 8, 3 * H / 4) == 0 && px(W / 8, 3 * H / 4) == 0xFFFF00,
              "glTexSubImage2D: the top right texel black, the rest kept");
    }
    /* A float rectangle texture: the 32-bit variants */
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, ftex);
    rect_draw(t);
    sprintf(msg, "a float rectangle texture: %06lx (4080bf)", px(W / 2, H / 2));
    check(rgb_near(px(W / 2, H / 2), 64, 128, 191, 2), msg);
    /* Drawn into: a 16x8 rectangle texture as a framebuffer object's colour */
    glGenTextures(1, &c);
    glBindTexture(GL_TEXTURE_RECTANGLE, c);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 16, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, c, 0);
    ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glViewport(0, 0, 16, 8);
    glClearColor(0, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0, 0, 0, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    rect_draw(c); /* only its bottom-left 4x2 texels are reached: cyan */
    sprintf(msg, "a rectangle texture as a framebuffer object's colour: complete %d, sampled %06lx (00ffff)", ok,
            px(W / 2, H / 2));
    check(ok && px(W / 2, H / 2) == 0x00FFFF, msg);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &c);
    glDeleteTextures(1, &t);
    glUseProgram(0);
}

int main(int argc, char **argv)
{
    ULONG err = 0, before, after;
    struct BitMap *bm;
    void *ctx;
    char msg[120];
    /* COMPILED: every program is compiled on the Amiga in this run, and the
     * compiler server that starts stays resident: no memory check. */
    int compiled = argc > 1 && !strcmp(argv[1], "COMPILED");
    printf("ogles2 M6: extensions%s\n", compiled ? ", every program compiled here" : "");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    before = AvailMem(MEMF_ANY);
    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 24,
                                             OGLES2_CCT_STENCIL, 8, TAG_DONE))) {
        printf("FAIL: create a bitmap context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    InitRastPort(&rp);
    rp.BitMap = bm;
    smooth = link(smooth_vs, smooth_fs, "a_position", "a_color");
    tex1 = link(tex1_vs, tex1_fs, "a_position", "a_texcoord");
    tex2 = link(tex1_vs, tex2_fs, "a_position", "a_texcoord");
    texscale = link(tex1_vs, texscale_fs, "a_position", "a_texcoord");
    rectp = link(tex1_vs, rect_fs, "a_position", "a_texcoord");
    check(smooth && tex1 && tex2 && texscale && rectp, "smooth, tex1, tex2, texscale, rect: link");
    glUseProgram(smooth);
    glUniformMatrix4fv(glGetUniformLocation(smooth, "u_mvp"), 1, GL_FALSE, identity);
    glUseProgram(tex1);
    glUniform1i(glGetUniformLocation(tex1, "u_tex"), 0);
    glClearColor(0, 0, 0, 1);

    test_strings();
    test_npot();
    test_bgra();
    test_aniso();
    test_lod();
    test_mirror();
    test_deriv();
    test_polygon();
    test_bufferoes();
    test_binary();
    test_float();
    test_depth();
    test_rect();

    glUseProgram(0);
    glDeleteProgram(smooth);
    glDeleteProgram(tex1);
    glDeleteProgram(tex2);
    glDeleteProgram(texscale);
    glDeleteProgram(rectp);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    if (compiled)
        printf("(not checked: %s, the compiler server included)\n", msg);
    else
        check(after + 32768 >= before, msg);
    FreeBitMap(bm);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
