/*
 * M7 test: glCopyTexImage2D and glCopyTexSubImage2D (GitHub issue 5:
 * OpenLara copies its screen into a refraction texture every frame). Four
 * coloured quadrants are copied and sampled back, so a flipped, mirrored
 * or red/blue-swapped copy shows; then the frame time with and without a
 * full-screen copy. The programs are the shipped smooth and tex1 ones.
 *   usage: m7_copy [BITMAP] [WINDOW] [BENCH]   (default: all three)
 *   BITMAP   the checks from a 320x240 bitmap context's framebuffer and
 *            from a framebuffer object
 *   WINDOW   the same from a 320x240 window (visible about a second)
 *   BENCH    640x480 window: 60 frames of a scene, then 60 with a copy of
 *            the whole screen into an RGBA texture and a quad drawn from it
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include <GLES2/gl2ext.h>
#include "../common/smooth_shaders.h"
#include "../common/tex_shaders.h"

struct GfxBase *GfxBase;
struct Device *TimerBase;
static struct timerequest timer_req;
static int failures, checks;
static GLuint p_smooth, p_tex, out_fb, out_tex;
static GLint mvp_loc, tex_loc;

static double now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000.0 + tv.tv_micro / 1000.0;
}

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
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
    glBindAttribLocation(p, 1, a1);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static const GLfloat ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/* A rectangle in normalized device coordinates, one colour (0xRRGGBB). */
static void rect(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1, unsigned long rgb)
{
    GLfloat v[12], c[16];
    int i;
    v[0] = x0; v[1] = y0; v[3] = x1; v[4] = y0; v[6] = x0; v[7] = y1; v[9] = x1; v[10] = y1;
    for (i = 0; i < 4; i++) {
        v[i * 3 + 2] = 0;
        c[i * 4 + 0] = ((rgb >> 16) & 255) / 255.0f;
        c[i * 4 + 1] = ((rgb >> 8) & 255) / 255.0f;
        c[i * 4 + 2] = (rgb & 255) / 255.0f;
        c[i * 4 + 3] = 1;
    }
    glUseProgram(p_smooth);
    glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, ident);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, c);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1);
}

/* Texture t drawn over the NDC rectangle, texture coordinates s0..s1, t0..t1. */
static void tex_rect(GLuint t, GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1, GLfloat s0, GLfloat t0, GLfloat s1,
                     GLfloat t1)
{
    GLfloat v[12], tc[8];
    int i;
    v[0] = x0; v[1] = y0; v[3] = x1; v[4] = y0; v[6] = x0; v[7] = y1; v[9] = x1; v[10] = y1;
    for (i = 0; i < 4; i++)
        v[i * 3 + 2] = 0;
    tc[0] = s0; tc[1] = t0; tc[2] = s1; tc[3] = t0; tc[4] = s0; tc[5] = t1; tc[6] = s1; tc[7] = t1;
    glUseProgram(p_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glUniform1i(tex_loc, 0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, tc);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1);
}

static GLuint new_texture(GLenum format, GLenum type, int w, int h, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* Four quadrants: red bottom left, green bottom right, blue top left,
 * white top right (GL's orientation). */
static const unsigned long quad_rgb[4] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff};

static void quadrants(void)
{
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(-1, -1, 0, 0, quad_rgb[0]);
    rect(0, -1, 1, 0, quad_rgb[1]);
    rect(-1, 0, 0, 1, quad_rgb[2]);
    rect(0, 0, 1, 1, quad_rgb[3]);
}

static unsigned long read_rgb(int x, int y)
{
    GLubyte px[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return ((unsigned long)px[0] << 16) | ((unsigned long)px[1] << 8) | px[2];
}

static int close_rgb(unsigned long a, unsigned long b)
{
    int k;
    for (k = 0; k < 24; k += 8) {
        int d = (int)((a >> k) & 255) - (int)((b >> k) & 255);
        if (d < -8 || d > 8) /* RGB565's steps */
            return 0;
    }
    return 1;
}

/* Texture t (w x h) drawn whole into the w x h output, which stays bound:
 * its pixel (x, y) is then texel (x, y). */
static void to_output(GLuint t, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fb);
    glViewport(0, 0, w, h);
    tex_rect(t, -1, -1, 1, 1, 0, 0, 1, 1);
}

struct point { int x, y; unsigned long rgb; };

static void expect_points(const char *what, const struct point *p, int n)
{
    char msg[200];
    int i, bad = 0;
    unsigned long got = 0;
    for (i = 0; i < n; i++) {
        unsigned long g = read_rgb(p[i].x, p[i].y);
        if (!close_rgb(g, p[i].rgb) && !bad++)
            got = g;
    }
    if (bad)
        sprintf(msg, "%s: %d of %d points wrong, the first 0x%06lx", what, bad, n, got);
    else
        sprintf(msg, "%s", what);
    check(!bad, msg);
}

/* The checks, the source framebuffer bound (w x h, fb its name: 0 for the
 * default one). */
static void copy_checks(const char *src, GLuint fb, int w, int h)
{
    char what[160];
    struct point p[8];
    GLuint t;
    static GLubyte grey[64 * 64 * 4];
    int i;
    for (i = 0; i < 64 * 64 * 4; i++)
        grey[i] = 0x80;

    /* glCopyTexImage2D of the whole framebuffer, RGBA. */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, w, h);
    quadrants();
    t = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, 1, 1, 0);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w, h, 0);
    check(glGetError() == GL_NO_ERROR, "glCopyTexImage2D: no error");
    to_output(t, w, h);
    p[0].x = w / 4;     p[0].y = h / 4;     p[0].rgb = quad_rgb[0];
    p[1].x = 3 * w / 4; p[1].y = h / 4;     p[1].rgb = quad_rgb[1];
    p[2].x = w / 4;     p[2].y = 3 * h / 4; p[2].rgb = quad_rgb[2];
    p[3].x = 3 * w / 4; p[3].y = 3 * h / 4; p[3].rgb = quad_rgb[3];
    p[4].x = 0;         p[4].y = 0;         p[4].rgb = quad_rgb[0];
    p[5].x = w - 1;     p[5].y = h - 1;     p[5].rgb = quad_rgb[3];
    sprintf(what, "%s: glCopyTexImage2D RGBA, whole: the quadrants in place", src);
    expect_points(what, p, 6);
    glDeleteTextures(1, &t);

    /* glCopyTexSubImage2D of a 32x32 region around the middle into a grey
     * 64x64 texture at (8, 16). */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, w, h);
    t = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, 64, 64, grey);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 8, 16, w / 2 - 16, h / 2 - 16, 32, 32);
    check(glGetError() == GL_NO_ERROR, "glCopyTexSubImage2D: no error");
    to_output(t, 64, 64);
    p[0].x = 8 + 4;  p[0].y = 16 + 4;  p[0].rgb = quad_rgb[0];
    p[1].x = 8 + 28; p[1].y = 16 + 4;  p[1].rgb = quad_rgb[1];
    p[2].x = 8 + 4;  p[2].y = 16 + 28; p[2].rgb = quad_rgb[2];
    p[3].x = 8 + 28; p[3].y = 16 + 28; p[3].rgb = quad_rgb[3];
    p[4].x = 4;      p[4].y = 4;       p[4].rgb = 0x808080;
    p[5].x = 50;     p[5].y = 60;      p[5].rgb = 0x808080;
    p[6].x = 7;      p[6].y = 20;      p[6].rgb = 0x808080;
    p[7].x = 41;     p[7].y = 47;      p[7].rgb = 0x808080;
    sprintf(what, "%s: glCopyTexSubImage2D 32x32 at (8, 16): quadrants there, the rest kept", src);
    expect_points(what, p, 8);
    glDeleteTextures(1, &t);

    /* Into RGB565 and LUMINANCE levels. */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, w, h);
    t = new_texture(GL_RGB, GL_UNSIGNED_SHORT_5_6_5, w, h, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    to_output(t, w, h);
    p[0].x = w / 4;     p[0].y = h / 4;     p[0].rgb = quad_rgb[0];
    p[1].x = 3 * w / 4; p[1].y = 3 * h / 4; p[1].rgb = quad_rgb[3];
    p[2].x = w / 4;     p[2].y = 3 * h / 4; p[2].rgb = quad_rgb[2];
    sprintf(what, "%s: glCopyTexSubImage2D into RGB565", src);
    expect_points(what, p, 3);
    glDeleteTextures(1, &t);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, w, h);
    t = new_texture(GL_LUMINANCE, GL_UNSIGNED_BYTE, w, h, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    to_output(t, w, h);
    p[0].x = w / 4;     p[0].y = h / 4;     p[0].rgb = 0xffffff; /* red: L 1 */
    p[1].x = 3 * w / 4; p[1].y = h / 4;     p[1].rgb = 0x000000; /* green: L 0 */
    p[2].x = w / 4;     p[2].y = 3 * h / 4; p[2].rgb = 0x000000; /* blue */
    sprintf(what, "%s: glCopyTexSubImage2D into LUMINANCE (red)", src);
    expect_points(what, p, 3);
    glDeleteTextures(1, &t);

    /* Copy, draw on, copy again: the second copy sees the new drawing. */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, w, h);
    quadrants();
    t = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, w, h, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    rect(-0.5f, -0.5f, 0.5f, 0.5f, 0xffff00);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    to_output(t, w, h);
    p[0].x = w / 2;  p[0].y = h / 2;  p[0].rgb = 0xffff00;
    p[1].x = 2;      p[1].y = 2;      p[1].rgb = quad_rgb[0];
    p[2].x = w - 3;  p[2].y = h - 3;  p[2].rgb = quad_rgb[3];
    sprintf(what, "%s: a second copy after more drawing has it", src);
    expect_points(what, p, 3);

    /* And the source keeps its picture after the copies. */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    p[0].x = w / 2;  p[0].y = h / 2;  p[0].rgb = 0xffff00;
    p[1].x = 2;      p[1].y = 2;      p[1].rgb = quad_rgb[0];
    p[2].x = w - 3;  p[2].y = 3;      p[2].rgb = quad_rgb[1];
    sprintf(what, "%s: the framebuffer keeps its picture", src);
    expect_points(what, p, 3);
    glDeleteTextures(1, &t);
}

static int setup(void)
{
    p_smooth = link(smooth_vs, smooth_fs, "a_position", "a_color");
    p_tex = link(tex1_vs, tex1_fs, "a_position", "a_texcoord");
    if (!p_smooth || !p_tex) {
        printf("FAIL: a program did not link (smooth %u, tex1 %u)\n", p_smooth, p_tex);
        return 0;
    }
    mvp_loc = glGetUniformLocation(p_smooth, "u_mvp");
    tex_loc = glGetUniformLocation(p_tex, "u_tex");
    out_tex = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, 16, 16, 0);
    glGenFramebuffers(1, &out_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

static void teardown(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (out_fb)
        glDeleteFramebuffers(1, &out_fb);
    if (out_tex)
        glDeleteTextures(1, &out_tex);
    if (p_smooth)
        glDeleteProgram(p_smooth);
    if (p_tex)
        glDeleteProgram(p_tex);
    out_fb = out_tex = p_smooth = p_tex = 0;
}

static void run_checks(void *ctx, const char *kind, int w, int h)
{
    char name[40];
    GLuint fb, t;
    aglMakeCurrent(ctx);
    if (!setup()) {
        failures++;
        teardown();
        return;
    }
    sprintf(name, "%s framebuffer", kind);
    copy_checks(name, 0, w, h);
    aglSwapBuffers();
    /* From a framebuffer object of the same size. */
    t = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, w, h, 0);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    copy_checks("framebuffer object", fb, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &t);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    teardown();
}

/* 60 frames: a scene of 40 rectangles; with copy, the screen copied into
 * an RGBA texture and drawn back over the lower third, as OpenLara's water
 * refracts what is behind it. */
static double bench(int w, int h, int copy, GLuint t)
{
    double t0;
    int f, i;
    t0 = now_ms();
    for (f = 0; f < 60; f++) {
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.2f, 0.3f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (i = 0; i < 40; i++) {
            GLfloat x = -1 + (GLfloat)((i * 37 + f * 3) % 180) / 100.0f, y = -1 + (GLfloat)((i * 53) % 180) / 100.0f;
            rect(x, y, x + 0.2f, y + 0.2f, 0x10101 * ((i * 41) & 255) ^ 0xff0000 * (i & 1));
        }
        if (copy) {
            glBindTexture(GL_TEXTURE_2D, t);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
            tex_rect(t, -1, -1, 1, -0.33f, 0, 0.02f, 1, 0.35f);
        }
        aglSwapBuffers();
    }
    glFinish();
    return (now_ms() - t0) / 60.0;
}

static void run_bench(void)
{
    ULONG e = 0;
    LONG w = 0, h = 0;
    void *ctx = aglCreateContextTags2(&e, OGLES2_CCT_PISTORM_WIDTH, 640, OGLES2_CCT_PISTORM_HEIGHT, 480,
                                      OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
    double a, b;
    GLuint t;
    if (!ctx) {
        check(0, "bench: a 640x480 window context");
        return;
    }
    aglMakeCurrent(ctx);
    if (setup()) {
        t = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, w, h, 0);
        bench(w, h, 0, t); /* warm up */
        a = bench(w, h, 0, t);
        b = bench(w, h, 1, t);
        printf("     bench %ldx%ld: scene %ld.%ld ms a frame; with a whole-screen copy %ld.%ld ms (+%ld.%ld)\n",
               (long)w, (long)h, (long)a, (long)(a * 10) % 10, (long)b, (long)(b * 10) % 10, (long)(b - a),
               (long)((b - a) * 10) % 10);
        glDeleteTextures(1, &t);
        check(glGetError() == GL_NO_ERROR, "bench: no GL error");
    } else {
        failures++;
    }
    teardown();
    aglDestroyContext(ctx);
}

int main(int argc, char **argv)
{
    int i, any = 0, do_bitmap = 0, do_window = 0, do_bench = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "BITMAP"))
            do_bitmap = any = 1;
        else if (!strcmp(argv[i], "WINDOW"))
            do_window = any = 1;
        else if (!strcmp(argv[i], "BENCH"))
            do_bench = any = 1;
    }
    if (!any)
        do_bitmap = do_window = do_bench = 1;
    printf("ogles2 M7: glCopyTexImage2D and glCopyTexSubImage2D\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0)) {
        printf("needs graphics 39 and timer.device\n");
        if (GfxBase)
            CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES2Open()) {
        printf("needs %s\n", OGLES2NAME);
        CloseDevice((struct IORequest *)&timer_req);
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    if (do_bitmap) {
        struct BitMap *bm = AllocBitMap(320, 240, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
        ULONG e = 0;
        void *ctx = bm ? aglCreateContextTags2(&e, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE) : 0;
        if (ctx) {
            run_checks(ctx, "bitmap", 320, 240);
            aglDestroyContext(ctx);
        } else {
            check(0, "a bitmap context");
        }
        if (bm)
            FreeBitMap(bm);
    }
    if (do_window) {
        ULONG e = 0;
        LONG w = 0, h = 0;
        void *ctx = aglCreateContextTags2(&e, OGLES2_CCT_PISTORM_WIDTH, 320, OGLES2_CCT_PISTORM_HEIGHT, 240,
                                          OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
        if (ctx) {
            run_checks(ctx, "window", (int)w, (int)h);
            Delay(50);
            aglDestroyContext(ctx);
        } else {
            check(0, "a window context");
        }
    }
    if (do_bench)
        run_bench();
    OGLES2Close();
    CloseDevice((struct IORequest *)&timer_req);
    CloseLibrary((struct Library *)GfxBase);
    if (failures)
        printf("FAILED: %d of %d checks passed\n", checks - failures, checks);
    else
        printf("ALL PASSED: %d of %d checks passed\n", checks, checks);
    return failures ? 10 : 0;
}
