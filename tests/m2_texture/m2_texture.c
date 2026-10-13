/*
 * M2 test: textures v1 (plan section 7.6), drawn with compiled programs
 * (tests/common/tex_shaders.h; binaries next to this program) and checked
 * by reading back what is displayed.
 *   usage: m2_texture [BITMAP] [WINDOW] [QUICK]   (default: both)
 *
 * Orientation (texel row 0 at the bottom), every GLES2 upload format with
 * GL's conversion, nearest and linear filtering, REPEAT, CLAMP_TO_EDGE and
 * MIRRORED_REPEAT, uploaded and generated mipmaps, the incomplete-texture
 * fallback (0,0,0,1), glTexSubImage2D, the unpack alignment, two textures
 * on two units, a texture replaced between two draws of one frame, deleting
 * a bound texture, and the GL errors of glTexImage2D. In the window each
 * picture stays 1 s (QUICK: no pauses).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>
#include "../common/tex_shaders.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks, quick;

struct readback {
    struct RastPort *rp;
    int x0, y0, w, h, visible;
    struct RastPort own;
};
static struct readback *rb;
static const char *kind;
static GLuint prog1, prog2;
static GLint pos1, tc1, pos2, tc2;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", kind, what);
}

static unsigned long px(int x, int y)
{
    return ReadRGBPixel(rb->rp, (UWORD)(rb->x0 + x), (UWORD)(rb->y0 + rb->h - 1 - y)) & 0xFFFFFF;
}

static int near(unsigned long a, unsigned long b, int tol)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 255) - (int)((b >> i) & 255);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}

static void show(void)
{
    aglSwapBuffers();
    if (rb->visible && !quick)
        Delay(50);
}

static GLuint link(const char *vs, const char *fs, GLint *pos, GLint *tc)
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
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[200];
        glGetProgramInfoLog(p, sizeof(log), 0, log);
        printf("     link failed: %s\n", log);
        return 0;
    }
    *pos = glGetAttribLocation(p, "a_position");
    *tc = glGetAttribLocation(p, "a_texcoord");
    return p;
}

/* A rectangle from (x0,y0) to (x1,y1) in NDC, texture coordinates (0,0)-(s,t). */
static void quad(GLuint p, GLint pos, GLint tc, float x0, float y0, float x1, float y1, float s, float t)
{
    GLfloat v[12], c[8];
    v[0] = x0; v[1] = y0; v[2] = 0;   c[0] = 0; c[1] = 0;
    v[3] = x1; v[4] = y0; v[5] = 0;   c[2] = s; c[3] = 0;
    v[6] = x0; v[7] = y1; v[8] = 0;   c[4] = 0; c[5] = t;
    v[9] = x1; v[10] = y1; v[11] = 0; c[6] = s; c[7] = t;
    glUseProgram(p);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(tc, 2, GL_FLOAT, GL_FALSE, 0, c);
    glEnableVertexAttribArray(tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void full(float s, float t)
{
    quad(prog1, pos1, tc1, -1, -1, 1, 1, s, t);
}

static void clear(float r, float g, float b)
{
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static GLuint tex(int w, int h, GLenum fmt, GLenum type, const void *data, GLenum filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static const UBYTE rgbw[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};

static void run(void)
{
    char msg[200];
    unsigned long a, b, c, d;
    GLuint t, t2;
    int w = rb->w, h = rb->h;

    if (!(prog1 = link(tex1_vs, tex1_fs, &pos1, &tc1)) || !(prog2 = link(tex1_vs, tex2_fs, &pos2, &tc2))) {
        check(0, "link the textured programs from their binaries");
        return;
    }
    glUseProgram(prog1);
    glUniform1i(glGetUniformLocation(prog1, "u_tex"), 0);

    /* 1. Orientation: rows from the bottom (red green / blue white). */
    t = tex(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, rgbw, GL_NEAREST);
    clear(0, 0, 0);
    full(1, 1);
    show();
    a = px(w / 4, h / 4); b = px(w * 3 / 4, h / 4); c = px(w / 4, h * 3 / 4); d = px(w * 3 / 4, h * 3 / 4);
    sprintf(msg, "2x2 texture: bottom %06lx %06lx (red green), top %06lx %06lx (blue white)", a, b, c, d);
    check(a == 0xFF0000 && b == 0x00FF00 && c == 0x0000FF && d == 0xFFFFFF, msg);

    /* 4. Wraps, s from 0 to 2: at s = 1.25 and 1.75 on the bottom row. */
    {
        static const struct { GLenum m; unsigned long at125, at175; const char *n; } wr[3] = {
            {GL_REPEAT, 0xFF0000, 0x00FF00, "REPEAT"},
            {GL_CLAMP_TO_EDGE, 0x00FF00, 0x00FF00, "CLAMP_TO_EDGE"},
            {GL_MIRRORED_REPEAT, 0x00FF00, 0xFF0000, "MIRRORED_REPEAT"}};
        int k;
        for (k = 0; k < 3; k++) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wr[k].m);
            clear(0, 0, 0);
            full(2, 1);
            show();
            a = px(w * 5 / 8, h / 4);
            b = px(w * 7 / 8, h / 4);
            sprintf(msg, "%s: s 1.25 %06lx, s 1.75 %06lx (%06lx, %06lx)", wr[k].n, a, b, wr[k].at125, wr[k].at175);
            check(a == wr[k].at125 && b == wr[k].at175, msg);
        }
    }

    /* 8. glTexSubImage2D: the top right texel blue. */
    {
        static const UBYTE blue[4] = {0, 0, 255, 255};
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
        clear(0, 0, 0);
        full(1, 1);
        show();
        d = px(w * 3 / 4, h * 3 / 4);
        sprintf(msg, "glTexSubImage2D: top right %06lx (0000ff)", d);
        check(d == 0x0000FF && px(w / 4, h / 4) == 0xFF0000, msg);
    }
    glDeleteTextures(1, &t);
    {
        GLint bnd = -1;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bnd);
        check(bnd == 0, "deleting the bound texture binds 0");
    }

    /* 2. Formats, each 1x1. */
    {
        static const UBYTE rgb[3] = {10, 20, 30}, lum[1] = {0x40}, alp[1] = {0x80}, la[2] = {0x60, 0x90};
        static const UWORD p565 = 0xF800, p4444 = 0x0F0F, p5551 = 0x003F;
        static const struct { GLenum f, ty; const void *data; unsigned long want; const char *n; } fm[7] = {
            {GL_RGB, GL_UNSIGNED_BYTE, rgb, 0x0A141E, "RGB"},
            {GL_LUMINANCE, GL_UNSIGNED_BYTE, lum, 0x404040, "LUMINANCE"},
            {GL_ALPHA, GL_UNSIGNED_BYTE, alp, 0x000000, "ALPHA (0,0,0,A)"},
            {GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la, 0x606060, "LUMINANCE_ALPHA"},
            {GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &p565, 0xFF0000, "RGB 5_6_5 red"},
            {GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, &p4444, 0x00FF00, "RGBA 4_4_4_4 green"},
            {GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, &p5551, 0x0000FF, "RGBA 5_5_5_1 blue"}};
        int k;
        for (k = 0; k < 7; k++) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            t = tex(1, 1, fm[k].f, fm[k].ty, fm[k].data, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            clear(0.5f, 0.5f, 0.5f);
            full(1, 1);
            show();
            a = px(w / 2, h / 2);
            sprintf(msg, "format %s: %06lx (%06lx)", fm[k].n, a, fm[k].want);
            check(a == fm[k].want, msg);
            glDeleteTextures(1, &t);
        }
    }

    /* 9. Unpack alignment: RGB 3x2, rows padded to 12 bytes by default. */
    {
        static const UBYTE padded[24] = {255, 0, 0, 255, 0, 0, 255, 0, 0, 0, 0, 0,
                                         0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0};
        static const UBYTE tight[18] = {255, 0, 0, 255, 0, 0, 255, 0, 0,
                                        0, 0, 255, 0, 0, 255, 0, 0, 255};
        t = tex(3, 2, GL_RGB, GL_UNSIGNED_BYTE, padded, GL_NEAREST);
        clear(0, 0, 0);
        full(1, 1);
        show();
        a = px(w / 2, h * 3 / 4);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, tight);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        clear(0, 0, 0);
        full(1, 1);
        show();
        b = px(w / 2, h * 3 / 4);
        sprintf(msg, "unpack alignment 4 and 1: second row %06lx and %06lx (0000ff)", a, b);
        check(a == 0x0000FF && b == 0x0000FF, msg);
        glDeleteTextures(1, &t);
    }

    /* 3. Linear filtering between black and white. */
    {
        static const UBYTE bw[8] = {0, 0, 0, 255, 255, 255, 255, 255};
        t = tex(2, 1, GL_RGBA, GL_UNSIGNED_BYTE, bw, GL_LINEAR);
        clear(0, 0, 0);
        full(1, 1);
        show();
        a = px(w / 2, h / 2);
        b = px(2, h / 2);
        sprintf(msg, "linear filter: centre %06lx (about 808080), left edge %06lx (black)", a, b);
        check(near(a, 0x808080, 0x14) && near(b, 0, 0x14), msg);
        glDeleteTextures(1, &t);
    }

    /* 5. Uploaded mip levels: 64x64, one colour per level, drawn 8x8. */
    {
        static const unsigned long lc[7] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0xFF00FF};
        static UBYTE img[64 * 64 * 4];
        int l, i;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        for (l = 0; l < 7; l++) {
            int s = 64 >> l;
            for (i = 0; i < s * s; i++) {
                img[i * 4] = (UBYTE)(lc[l] >> 16); img[i * 4 + 1] = (UBYTE)(lc[l] >> 8);
                img[i * 4 + 2] = (UBYTE)lc[l]; img[i * 4 + 3] = 255;
            }
            glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        clear(0, 0, 0);
        glViewport(100, 0, 8, 8);
        full(1, 1);
        glViewport(0, 0, 64, 64);
        full(1, 1);
        glViewport(0, 0, w, h);
        show();
        a = px(104, 4);
        b = px(32, 40);
        sprintf(msg, "mip levels: 8x8 draw %06lx (level 3, ffffff), 64x64 draw %06lx (level 0, ff0000)", a, b);
        check(a == 0xFFFFFF && b == 0xFF0000, msg);

        /* 7. Incomplete: a mipmapping filter, level 0 only. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        glDeleteTextures(1, &t);
        t = tex(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        clear(0.5f, 0.5f, 0.5f);
        full(1, 1);
        show();
        a = px(w / 2, h / 2);
        sprintf(msg, "incomplete texture samples (0,0,0,1): %06lx (000000)", a);
        check(a == 0, msg);
        glDeleteTextures(1, &t);

        /* 6. glGenerateMipmap on a 1-texel checkerboard: grey levels. */
        for (i = 0; i < 64 * 64; i++) {
            UBYTE v = (UBYTE)((((i % 64) + (i / 64)) & 1) ? 255 : 0);
            img[i * 4] = img[i * 4 + 1] = img[i * 4 + 2] = v;
            img[i * 4 + 3] = 255;
        }
        t = tex(64, 64, GL_RGBA, GL_UNSIGNED_BYTE, img, GL_NEAREST);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        clear(0, 0, 0);
        glViewport(0, 0, 4, 4);
        full(1, 1);
        glViewport(0, 0, w, h);
        show();
        a = px(2, 2);
        sprintf(msg, "glGenerateMipmap: a 4x4 draw of a 64x64 checkerboard %06lx (about 808080)", a);
        check(near(a, 0x808080, 2), msg);
        glDeleteTextures(1, &t);
    }

    /* 10. Two textures on two units, multiplied. */
    {
        static const UBYTE ta[4] = {255, 128, 255, 255}, tb[4] = {255, 255, 0, 255};
        glUseProgram(prog2);
        glUniform1i(glGetUniformLocation(prog2, "u_a"), 0);
        glUniform1i(glGetUniformLocation(prog2, "u_b"), 1);
        glActiveTexture(GL_TEXTURE0);
        t = tex(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ta, GL_NEAREST);
        glActiveTexture(GL_TEXTURE1);
        t2 = tex(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, tb, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
        clear(0, 0, 0);
        quad(prog2, pos2, tc2, -1, -1, 1, 1, 1, 1);
        show();
        a = px(w / 2, h / 2);
        sprintf(msg, "two units: %06lx (ff8000)", a);
        check(near(a, 0xFF8000, 1), msg);
        glDeleteTextures(1, &t);
        glDeleteTextures(1, &t2);
    }

    /* 11. Replaced between two draws of one frame. */
    {
        static const UBYTE red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
        t = tex(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red, GL_NEAREST);
        clear(0, 0, 0);
        quad(prog1, pos1, tc1, -1, -1, 0, 1, 1, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
        quad(prog1, pos1, tc1, 0, -1, 1, 1, 1, 1);
        show();
        a = px(w / 4, h / 2);
        b = px(w * 3 / 4, h / 2);
        sprintf(msg, "texture replaced between draws: left %06lx (red), right %06lx (green)", a, b);
        check(a == 0xFF0000 && b == 0x00FF00, msg);
        glDeleteTextures(1, &t);
    }

    /* 11b. A texture changed after a draw of the same frame is renamed, not
     * rendered first: the frame keeps its depth. The same quad twice with
     * GL_LESS: the second fails the depth test and the quad stays red. */
    {
        static const UBYTE red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
        t = tex(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red, GL_NEAREST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        full(1, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
        full(1, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
        glDisable(GL_DEPTH_TEST);
        quad(prog1, pos1, tc1, 0, -1, 1, 1, 1, 1); /* right half: the sub-image (red again) */
        show();
        a = px(w / 4, h / 2);
        b = px(w * 3 / 4, h / 2);
        sprintf(msg, "texture changed mid-frame keeps the frame's depth: %06lx (ff0000); after glTexSubImage2D %06lx (ff0000)", a, b);
        check(a == 0xFF0000 && b == 0xFF0000, msg);
        glDeleteTextures(1, &t);
    }

    /* 12. Copies from the framebuffer: left half red, right half green
     * (scissored clears), copied across the boundary. */
    {
        static const UBYTE blue2[8] = {0, 0, 255, 255, 0, 0, 255, 255};
        unsigned long l0, l1;
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, w / 2, h);
        clear(1, 0, 0);
        glScissor(w / 2, 0, w - w / 2, h);
        clear(0, 1, 0);
        glDisable(GL_SCISSOR_TEST);
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w / 2 - 2, h / 2, 4, 1, 0);
        /* LUMINANCE takes red (GLES 2.0 table 3.15): white from the red half. */
        glGenTextures(1, &t2);
        glBindTexture(GL_TEXTURE_2D, t2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2, 2, 1, 1, 0);
        clear(0, 0, 0);
        glBindTexture(GL_TEXTURE_2D, t);
        quad(prog1, pos1, tc1, -1, -1, 0, 1, 1, 1);
        glBindTexture(GL_TEXTURE_2D, t2);
        quad(prog1, pos1, tc1, 0, -1, 1, 1, 1, 1);
        show();
        a = px(w / 16, h / 2);       /* texel 0: red */
        b = px(w * 7 / 16, h / 2);   /* texel 3: green */
        c = px(w * 3 / 4, h / 2);    /* the LUMINANCE copy */
        sprintf(msg, "glCopyTexImage2D: %06lx red, %06lx green; as LUMINANCE %06lx (ffffff)", a, b, c);
        check(a == 0xFF0000 && b == 0x00FF00 && c == 0xFFFFFF, msg);
        /* glCopyTexSubImage2D: texel 1 of a blue 2x1 texture from the green half. */
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue2);
        clear(0, 1, 0);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, w / 2, h / 2, 1, 1);
        clear(0, 0, 0);
        full(1, 1);
        show();
        l0 = px(w / 4, h / 2);
        l1 = px(w * 3 / 4, h / 2);
        sprintf(msg, "glCopyTexSubImage2D: %06lx blue kept, %06lx green copied", l0, l1);
        check(l0 == 0x0000FF && l1 == 0x00FF00, msg);
        glGetError();
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 1, 1, 1);
        a = glGetError();
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 2, 0, 0, 0, 1, 1); /* past the 2x1 texture */
        b = glGetError();
        sprintf(msg, "copy errors: border %04lx (0501), outside the texture %04lx (0501)", a, b);
        check(a == GL_INVALID_VALUE && b == GL_INVALID_VALUE, msg);
        glDeleteTextures(1, &t);
        glDeleteTextures(1, &t2);
    }

    /* 12b. ETC1 (OES_compressed_ETC1_RGB8_texture): one 4x4 block, individual
     * mode, no flip: left subblock base (15,0,0), right (0,15,0), modifier
     * row 0, every index 0 (+2): left (255,2,2), right (2,255,2). */
    {
        static const UBYTE block[8] = {0xF0, 0x0F, 0x00, 0x00, 0, 0, 0, 0};
        GLint n = 0, f[16], k, etc1 = 0;
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
        memset(f, 0, sizeof(f));
        if (n > 0 && n <= 16) /* ETC1 and, from M7, the four S3TC formats */
            glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, f);
        for (k = 0; k < n && k < 16; k++)
            etc1 |= f[k] == 0x8D64;
        check(ext && strstr(ext, "GL_OES_compressed_ETC1_RGB8_texture") && etc1,
              "ETC1: in GL_EXTENSIONS and GL_COMPRESSED_TEXTURE_FORMATS");
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, 0x8D64, 4, 4, 0, 8, block);
        clear(0, 0, 0);
        full(1, 1);
        show();
        a = px(w / 8, h / 2);
        b = px(w * 7 / 8, h / 2);
        sprintf(msg, "ETC1 block: left %06lx (ff0202), right %06lx (02ff02)", a, b);
        check(a == 0xFF0202 && b == 0x02FF02, msg);
        glGetError();
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, 0x8D64, 4, 4, 0, 7, block);
        a = glGetError();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgbw);
        b = glGetError();
        glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, 0x8D64, 8, block);
        c = glGetError();
        sprintf(msg, "ETC1 errors: wrong size %04lx (0501), TexSubImage %04lx (0502), CompressedTexSubImage %04lx (0502)",
                a, b, c);
        check(a == GL_INVALID_VALUE && b == GL_INVALID_OPERATION && c == GL_INVALID_OPERATION, msg);
        glDeleteTextures(1, &t);
    }

    /* 12c. Non-power-of-two sizes (GLES2: clamp, no mipmaps), RGBA8 and RGB565
     * storage: a colour per quadrant, sampled near each quadrant's centre. */
    {
        static const int sizes[2][2] = {{100, 60}, {50, 30}};
        int n;
        for (n = 0; n < 2; n++) {
            int tw = sizes[n][0], th = sizes[n][1], x, y;
            UBYTE *img = malloc((size_t)tw * th * 4);
            unsigned long q[4];
            if (!img)
                break;
            for (y = 0; y < th; y++)
                for (x = 0; x < tw; x++) {
                    UBYTE *p = img + ((size_t)y * tw + x) * (n ? 2 : 4);
                    int quad_ = (x >= tw / 2) + 2 * (y >= th / 2); /* 0 red, 1 green, 2 blue, 3 white */
                    if (!n) {
                        p[0] = quad_ == 0 || quad_ == 3 ? 255 : 0;
                        p[1] = quad_ == 1 || quad_ == 3 ? 255 : 0;
                        p[2] = quad_ >= 2 ? 255 : 0;
                        p[3] = 255;
                    } else {
                        UWORD v = quad_ == 0 ? 0xF800 : quad_ == 1 ? 0x07E0 : quad_ == 2 ? 0x001F : 0xFFFF;
                        p[0] = (UBYTE)(v >> 8); /* the application's (big-endian) shorts */
                        p[1] = (UBYTE)v;
                    }
                }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            t = n ? tex(tw, th, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, img, GL_NEAREST)
                  : tex(tw, th, GL_RGBA, GL_UNSIGNED_BYTE, img, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            clear(0, 0, 0);
            full(1, 1);
            show();
            q[0] = px(w / 4, h / 4);
            q[1] = px(w * 3 / 4, h / 4);
            q[2] = px(w / 4, h * 3 / 4);
            q[3] = px(w * 3 / 4, h * 3 / 4);
            sprintf(msg, "%dx%d %s texture: %06lx %06lx %06lx %06lx (ff0000 00ff00 0000ff ffffff)", tw, th,
                    n ? "RGB565" : "RGBA8", q[0], q[1], q[2], q[3]);
            check(q[0] == 0xFF0000 && q[1] == 0x00FF00 && q[2] == 0x0000FF && q[3] == 0xFFFFFF, msg);
            glDeleteTextures(1, &t);
            free(img);
        }
    }

    /* 12d. EXT_texture_format_BGRA8888: bytes blue, green, red, alpha. */
    {
        static const UBYTE bgra[4] = {0x10, 0x80, 0xF0, 0xFF};
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        t = tex(1, 1, 0x80E1, GL_UNSIGNED_BYTE, bgra, GL_NEAREST);
        clear(0, 0, 0);
        full(1, 1);
        show();
        a = px(w / 2, h / 2);
        sprintf(msg, "BGRA8888 texture: %06lx (f08010), in GL_EXTENSIONS", a);
        check(a == 0xF08010 && ext && strstr(ext, "GL_EXT_texture_format_BGRA8888"), msg);
        glDeleteTextures(1, &t);
    }

    /* 13. Errors. */
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glGetError();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbw);
    a = glGetError();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgbw);
    b = glGetError();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_INT, rgbw); /* GL_FLOAT: OES_texture_float */
    c = glGetError();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4, rgbw);
    d = glGetError();
    sprintf(msg, "glTexImage2D errors: %04lx %04lx %04lx %04lx (0502 0501 0500 0502)", a, b, c, d);
    check(a == GL_INVALID_OPERATION && b == GL_INVALID_VALUE && c == GL_INVALID_ENUM &&
          d == GL_INVALID_OPERATION, msg);
    glDeleteTextures(1, &t);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    glUseProgram(0);
    glDeleteProgram(prog1);
    glDeleteProgram(prog2);
}

static void finish(void *ctx, ULONG before)
{
    char msg[100];
    ULONG after;
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 16384 >= before, msg);
}

int main(int argc, char **argv)
{
    int i, bitmap = 0, window = 0;
    ULONG err = 0, before;
    void *ctx;
    struct readback r;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "QUICK")) quick = 1;
        if (!strcmp(argv[i], "BITMAP")) bitmap = 1;
        if (!strcmp(argv[i], "WINDOW")) window = 1;
    }
    if (!bitmap && !window)
        bitmap = window = 1;
    printf("ogles2 M2: textures\n");
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs intuition, graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    rb = &r;
    if (bitmap) {
        struct BitMap *bm = AllocBitMap(256, 128, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
        kind = "bitmap";
        before = AvailMem(MEMF_ANY);
        if (bm && (ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 16,
                                               TAG_DONE))) {
            aglMakeCurrent(ctx);
            InitRastPort(&r.own);
            r.own.BitMap = bm;
            r.rp = &r.own;
            r.x0 = r.y0 = 0;
            r.w = 256;
            r.h = 128;
            r.visible = 0;
            run();
            finish(ctx, before);
        } else {
            check(0, "create a bitmap context");
        }
        if (bm)
            FreeBitMap(bm);
    }
    if (window) {
        struct Window *win = 0;
        ULONG w = 0, h = 0;
        kind = "window";
        before = AvailMem(MEMF_ANY);
        ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 16, OGLES2_CCT_PISTORM_WIDTH, 320, OGLES2_CCT_PISTORM_HEIGHT, 240,
                                    OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                    OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
        if (ctx && win) {
            aglMakeCurrent(ctx);
            r.rp = win->RPort;
            r.x0 = win->BorderLeft;
            r.y0 = win->BorderTop;
            r.w = (int)w;
            r.h = (int)h;
            r.visible = 1;
            run();
            finish(ctx, before);
        } else {
            check(0, "create a window context");
        }
    }
    OGLES2Close();
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
