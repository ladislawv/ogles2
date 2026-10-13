/*
 * M7 test: S3TC textures (docs/M7_EXTENSIONS_PLAN.md step 3), which the TMU
 * samples as BC1-BC3, compared texel by texel with the S3TC specification's
 * decoding (EXT_texture_compression_s3tc): exact for the end points and
 * DXT3's alpha, within 1 of the exact value where the specification
 * interpolates. Drawn 1:1, nearest, into an RGBA8 framebuffer object of a
 * bitmap context. Nothing to watch.
 *   usage: m7_s3tc
 *   formats   the extension strings and GL_COMPRESSED_TEXTURE_FORMATS
 *   decode    DXT1 RGB, DXT1 RGBA, DXT3, DXT5: 16x8 textures of hand-made
 *             blocks (each colour and alpha mode, every code) and random ones
 *   edges     a 6x5 texture: blocks reaching past the image
 *   mipmaps   an 8x8 chain down to 1x1, each level one colour
 *   update    glCompressedTexSubImage2D of blocks, before and after a draw
 *   errors    what these textures refuse
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/ogles2.h>
#include <GLES2/gl2ext.h>
#include "../common/tex_shaders.h"

struct GfxBase *GfxBase;
static int failures, checks;
static GLuint prog;
static GLint pos_loc, tc_loc;
#define FW 16
#define FH 8

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static unsigned long seed = 12345;
static UBYTE rnd8(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (UBYTE)(seed >> 16);
}

/* ---- the reference: the specification's decoding, in real numbers ---- */

static void rgb565(unsigned c, double *rgb)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

static unsigned long le32(const UBYTE *b)
{
    return (unsigned long)b[0] | ((unsigned long)b[1] << 8) | ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
}

/* Texel (i, j) of a colour block; dxt1: c0 <= c1 is the 3-colour mode
 * (code 3 black, transparent). *exact: no interpolation was involved. */
static void colour_texel(const UBYTE *b, int i, int j, int dxt1, double *out, int *exact)
{
    unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    int code = (int)(le32(b + 4) >> (2 * (4 * j + i))) & 3, k;
    double p0[3], p1[3];
    rgb565(c0, p0);
    rgb565(c1, p1);
    out[3] = 255;
    *exact = code < 2;
    for (k = 0; k < 3; k++) {
        if (code == 0)
            out[k] = p0[k];
        else if (code == 1)
            out[k] = p1[k];
        else if (!dxt1 || c0 > c1)
            out[k] = code == 2 ? (2 * p0[k] + p1[k]) / 3 : (p0[k] + 2 * p1[k]) / 3;
        else if (code == 2)
            out[k] = (p0[k] + p1[k]) / 2;
        else {
            out[k] = 0;
            out[3] = 0;
            *exact = 1;
        }
    }
}

static double dxt3_alpha(const UBYTE *b, int i, int j)
{
    int n = 4 * j + i, v = b[n / 2];
    return (double)(((n & 1) ? v >> 4 : v & 15) * 17);
}

/* Bit p of the 48 code bits after a DXT5 block's two alphas. */
static int dxt5_bit(const UBYTE *b, int p)
{
    return (b[2 + p / 8] >> (p % 8)) & 1;
}

static double dxt5_alpha(const UBYTE *b, int i, int j, int *exact)
{
    double a0 = b[0], a1 = b[1];
    int n = 4 * j + i, code;
    code = dxt5_bit(b, 3 * n) | (dxt5_bit(b, 3 * n + 1) << 1) | (dxt5_bit(b, 3 * n + 2) << 2);
    *exact = code < 2;
    if (code == 0)
        return a0;
    if (code == 1)
        return a1;
    if (a0 > a1)
        return ((8 - code) * a0 + (code - 1) * a1) / 7;
    if (code == 6)
        return (*exact = 1, 0);
    if (code == 7)
        return (*exact = 1, 255);
    return ((6 - code) * a0 + (code - 1) * a1) / 5;
}

/* Texel (x, y) of an image of `bw` blocks a row. */
static void ref_texel(GLenum fmt, const UBYTE *data, int bw, int x, int y, double *rgba, int *exact)
{
    int bs = fmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
    const UBYTE *b = data + ((y / 4) * bw + x / 4) * bs;
    int i = x & 3, j = y & 3, ex = 1;
    if (bs == 8) {
        colour_texel(b, i, j, 1, rgba, exact);
        if (fmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
            rgba[3] = 255;
        return;
    }
    colour_texel(b + 8, i, j, 0, rgba, exact);
    if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT)
        rgba[3] = dxt3_alpha(b, i, j);
    else
        rgba[3] = dxt5_alpha(b, i, j, &ex);
    *exact = *exact && ex;
}

/* ---- drawing ---- */

static GLuint link(void)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    const char *vs = tex1_vs, *fs = tex1_fs;
    GLint ok = 0;
    glShaderSource(v, 1, &vs, 0);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(v);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
        return 0;
    pos_loc = glGetAttribLocation(p, "a_position");
    tc_loc = glGetAttribLocation(p, "a_texcoord");
    return p;
}

/* The bound texture drawn over a w x h viewport, read back. */
static void draw(int w, int h, GLubyte *px)
{
    static const GLfloat quad[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    static const GLfloat tc[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glViewport(0, 0, w, h);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(tc_loc, 2, GL_FLOAT, GL_FALSE, 0, tc);
    glEnableVertexAttribArray(tc_loc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static GLuint new_texture(void)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* The bound w x h texture of `fmt` (data: its blocks) drawn and compared
 * with the reference; one check named `what`. */
static void compare(const char *what, GLenum fmt, const UBYTE *data, int w, int h)
{
    GLubyte px[FW * FH * 4];
    char msg[200];
    int x, y, c, bad = 0, bx = -1, by = -1;
    double worst = 0, worst_exact = 0; /* printed in hundredths: printf here has no %f */
    draw(w, h, px);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double ref[4];
            int exact;
            ref_texel(fmt, data, (w + 3) / 4, x, y, ref, &exact);
            for (c = 0; c < 4; c++) {
                double d = px[(y * w + x) * 4 + c] - ref[c];
                if (d < 0)
                    d = -d;
                if (exact && d > worst_exact)
                    worst_exact = d;
                if (d > worst)
                    worst = d;
                /* exact values as such (to within the 8-bit rounding of
                 * the shader's output), the interpolated within 1 */
                if (d > (exact ? 0.5 : 1.0) && !bad++) {
                    bx = x;
                    by = y;
                }
            }
        }
    if (bad)
        sprintf(msg, "%s: %d channel(s) off, the first at (%d, %d): got %d %d %d %d; largest difference %d/100",
                what, bad, bx, by, px[(by * w + bx) * 4], px[(by * w + bx) * 4 + 1], px[(by * w + bx) * 4 + 2],
                px[(by * w + bx) * 4 + 3], (int)(worst * 100 + 0.5));
    else
        sprintf(msg, "%s: %dx%d as the specification decodes it (largest difference %d/100, exact texels %d/100)",
                what, w, h, (int)(worst * 100 + 0.5), (int)(worst_exact * 100 + 0.5));
    check(!bad, msg);
}

/* ---- the tests ---- */

static void put16(UBYTE *b, unsigned v)
{
    b[0] = (UBYTE)v;
    b[1] = (UBYTE)(v >> 8);
}

/* A colour block: c0, c1 and the 16 codes as given (texel order). */
static void colour_block(UBYTE *b, unsigned c0, unsigned c1, const int *codes)
{
    unsigned long bits = 0;
    int n;
    put16(b, c0);
    put16(b + 2, c1);
    for (n = 0; n < 16; n++)
        bits |= (unsigned long)(codes[n] & 3) << (2 * n);
    b[4] = (UBYTE)bits;
    b[5] = (UBYTE)(bits >> 8);
    b[6] = (UBYTE)(bits >> 16);
    b[7] = (UBYTE)(bits >> 24);
}

static void dxt5_alpha_block(UBYTE *b, int a0, int a1, const int *codes)
{
    int n, k;
    b[0] = (UBYTE)a0;
    b[1] = (UBYTE)a1;
    memset(b + 2, 0, 6);
    for (n = 0; n < 16; n++)
        for (k = 0; k < 3; k++)
            if ((codes[n] >> k) & 1)
                b[2 + (3 * n + k) / 8] |= (UBYTE)(1 << ((3 * n + k) % 8));
}

/* 16x8 (4x2 blocks): block 0 c0 > c1, block 1 c0 < c1, block 2 c0 == c1,
 * each with every code; the rest random. DXT3/5 alpha: block 0 a0 > a1,
 * block 1 a0 < a1, every code; DXT3 explicit nibbles 0..15. */
static void make_image(GLenum fmt, UBYTE *data)
{
    static const int codes4[16] = {0, 1, 2, 3, 3, 2, 1, 0, 1, 3, 0, 2, 2, 0, 3, 1};
    static const int codes8[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    int bs = fmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
    int n, k;
    for (n = 0; n < 8 * bs; n++)
        data[n] = rnd8();
    for (n = 0; n < 3; n++) {
        UBYTE *c = data + n * bs + (bs - 8);
        unsigned a = 0xF81F /* magenta */, b = 0x07E0 /* green */;
        if (n == 0)
            colour_block(c, a > b ? a : b, a > b ? b : a, codes4);
        else if (n == 1)
            colour_block(c, 0x2945, 0xD6BA, codes4);
        else
            colour_block(c, 0x7BEF, 0x7BEF, codes4);
    }
    if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT)
        for (k = 0; k < 8; k++)
            data[k] = (UBYTE)((2 * k) | ((2 * k + 1) << 4));
    if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) {
        dxt5_alpha_block(data, 200, 20, codes8);
        dxt5_alpha_block(data + 16, 30, 230, codes8);
    }
}

static void test_formats(void)
{
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    GLint n = 0, f[16], i, seen = 0;
    check(ext && strstr(ext, "GL_EXT_texture_compression_dxt1 ") && strstr(ext, "GL_EXT_texture_compression_s3tc ") &&
              strstr(ext, "GL_ANGLE_texture_compression_dxt3 ") && strstr(ext, "GL_ANGLE_texture_compression_dxt5"),
          "the four extension strings");
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    memset(f, 0, sizeof(f));
    if (n > 0 && n <= 16)
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, f);
    for (i = 0; i < n && i < 16; i++)
        if (f[i] >= 0x83F0 && f[i] <= 0x83F3)
            seen |= 1 << (f[i] - 0x83F0);
    check(n == 5 && seen == 15, "GL_COMPRESSED_TEXTURE_FORMATS: ETC1 and the four DXT formats");
}

static void test_decode(void)
{
    static const GLenum fmts[4] = {GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                   GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
    static const char *names[4] = {"DXT1 RGB", "DXT1 RGBA", "DXT3", "DXT5"};
    UBYTE data[8 * 16];
    int i;
    for (i = 0; i < 4; i++) {
        int bs = i < 2 ? 8 : 16;
        GLuint t = new_texture();
        make_image(fmts[i], data);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmts[i], FW, FH, 0, 8 * bs, data);
        check(glGetError() == GL_NO_ERROR, names[i]);
        compare(names[i], fmts[i], data, FW, FH);
        glDeleteTextures(1, &t);
    }
}

static void test_edges(void)
{
    UBYTE data[4 * 16];
    GLuint t = new_texture();
    make_image(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, data);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 6, 5, 0, sizeof(data), data);
    check(glGetError() == GL_NO_ERROR, "a 6x5 DXT5 image: 2x2 blocks");
    compare("6x5 DXT5", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, data, 6, 5);
    glDeleteTextures(1, &t);
}

static void test_mipmaps(void)
{
    static const unsigned colours[4] = {0xF800, 0x07E0, 0x001F, 0xFFE0}; /* red, green, blue, yellow */
    static const int zero[16];
    UBYTE level[4 * 8], px[FW * FH * 4];
    GLuint t = new_texture();
    char msg[120];
    int l, n, size;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    for (l = 0, size = 8; l < 4; l++, size /= 2) {
        int blocks = ((size + 3) / 4) * ((size + 3) / 4);
        for (n = 0; n < blocks; n++)
            colour_block(level + n * 8, colours[l], colours[l], zero);
        glCompressedTexImage2D(GL_TEXTURE_2D, l, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, size, size, 0, blocks * 8, level);
    }
    check(glGetError() == GL_NO_ERROR, "an 8x8 DXT1 chain down to 1x1 (levels of one block)");
    for (l = 0, size = 8; l < 4; l++, size /= 2) {
        double ref[3];
        int ok;
        rgb565(colours[l], ref);
        draw(size, size, px);
        ok = px[0] == (GLubyte)ref[0] && px[1] == (GLubyte)ref[1] && px[2] == (GLubyte)ref[2] && px[3] == 255;
        sprintf(msg, "level %d drawn %dx%d: %d %d %d %d", l, size, size, px[0], px[1], px[2], px[3]);
        check(ok, msg);
    }
    glDeleteTextures(1, &t);
}

static void test_update(void)
{
    UBYTE data[8 * 8], blk[2 * 8];
    GLuint t = new_texture();
    int n;
    make_image(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, data);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, FW, FH, 0, sizeof(data), data);
    /* Before the texture was drawn: block (1, 1). */
    for (n = 0; n < 8; n++)
        blk[n] = rnd8();
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 4, 4, 4, 4, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8, blk);
    memcpy(data + (1 * 4 + 1) * 8, blk, 8);
    check(glGetError() == GL_NO_ERROR, "glCompressedTexSubImage2D of a block");
    compare("a block replaced before the first draw", GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, data, FW, FH);
    /* After: blocks (2, 0) and (3, 0) in one call. */
    for (n = 0; n < 16; n++)
        blk[n] = rnd8();
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 8, 0, 8, 4, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 16, blk);
    memcpy(data + 2 * 8, blk, 16);
    check(glGetError() == GL_NO_ERROR, "glCompressedTexSubImage2D of two blocks after a draw");
    compare("two blocks replaced after a draw", GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, data, FW, FH);
    glDeleteTextures(1, &t);
}

static GLenum err(void)
{
    return glGetError();
}

static void test_errors(GLuint fb)
{
    UBYTE data[8 * 8], rgba[64];
    GLuint t = new_texture(), f2;
    memset(data, 0, sizeof(data));
    memset(rgba, 0, sizeof(rgba));
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, FW, FH, 0, sizeof(data) - 1, data);
    check(err() == GL_INVALID_VALUE, "glCompressedTexImage2D: a wrong imageSize is GL_INVALID_VALUE");
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, FW, FH, 0, sizeof(data), data);
    check(err() == GL_NO_ERROR, "a 16x8 DXT1 image");
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    check(err() == GL_INVALID_OPERATION, "glTexSubImage2D on it: GL_INVALID_OPERATION");
    glGenerateMipmap(GL_TEXTURE_2D);
    check(err() == GL_INVALID_OPERATION, "glGenerateMipmap on it: GL_INVALID_OPERATION");
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 4, 4);
    check(err() == GL_INVALID_OPERATION, "glCopyTexSubImage2D on it: GL_INVALID_OPERATION");
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 2, 0, 4, 4, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8, data);
    check(err() == GL_INVALID_OPERATION, "glCompressedTexSubImage2D off the block grid: GL_INVALID_OPERATION");
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 4, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8, data);
    check(err() == GL_INVALID_OPERATION, "glCompressedTexSubImage2D of part of a block: GL_INVALID_OPERATION");
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16, data);
    check(err() == GL_INVALID_OPERATION, "glCompressedTexSubImage2D in another format: GL_INVALID_OPERATION");
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 16, data);
    check(err() == GL_INVALID_VALUE, "glCompressedTexSubImage2D: a wrong imageSize is GL_INVALID_VALUE");
    glGenFramebuffers(1, &f2);
    glBindFramebuffer(GL_FRAMEBUFFER, f2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE,
          "a framebuffer object with it as its colour is not complete");
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glDeleteFramebuffers(1, &f2);
    glDeleteTextures(1, &t);
    check(err() == GL_NO_ERROR, "no GL error left over");
}

int main(void)
{
    struct BitMap *bm;
    void *ctx;
    ULONG e = 0;
    GLuint fb, rb;
    printf("ogles2 M7: S3TC textures (DXT1, DXT3, DXT5)\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        printf("needs graphics 39 and %s\n", OGLES2NAME);
        return 20;
    }
    /* Every way out closes what was opened: a library left open cannot be
     * expunged, and the next build would not load until a reboot. */
    bm = AllocBitMap(64, 64, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm || !(ctx = aglCreateContextTags2(&e, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE))) {
        printf("FAIL: no context (error %lu)\n", (unsigned long)e);
        if (bm)
            FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    aglMakeCurrent(ctx);
    if (!(prog = link())) {
        printf("FAIL: the textured program did not link\n");
        aglDestroyContext(ctx);
        FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, FW, FH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "a 16x8 RGBA8 framebuffer object");

    test_formats();
    test_decode();
    test_edges();
    test_mipmaps();
    test_update();
    test_errors(fb);

    glDeleteRenderbuffers(1, &rb);
    glDeleteFramebuffers(1, &fb);
    glDeleteProgram(prog);
    aglDestroyContext(ctx);
    FreeBitMap(bm);
    OGLES2Close();
    CloseLibrary((struct Library *)GfxBase);
    if (failures)
        printf("FAILED: %d of %d checks passed\n", checks - failures, checks);
    else
        printf("ALL PASSED: %d of %d checks passed\n", checks, checks);
    return failures ? 10 : 0;
}
