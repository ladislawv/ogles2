/*
 * M6 test: 4x MSAA on the default framebuffer (OGLES2_CCT_PISTORM_MSAA, plan
 * 7.2), on a hidden 256x128 bitmap with D24S8. Nothing to watch.
 *   usage: m6_msaa
 *   queries   GL_SAMPLE_BUFFERS 1, GL_SAMPLES 4; 0 and 0 in an FBO and
 *             without the tag
 *   edges     a slanted white triangle on black: pixels along its edge
 *             between black and white (antialiased), none without MSAA
 *   coverage  glSampleCoverage(0.5): half the samples, about grey; inverted,
 *             the other half (both together white)
 *   split     glReadPixels between two depth-tested triangles: the frame
 *             goes on in a second job with its depth (all four samples)
 *   stencil   stencil drawn, read back, a colour-only clear read back: the
 *             job between the two flushes draws nothing, yet the stencil
 *             outlives both (dEQP-GLES2 multisample.stencil)
 *   fbo       a framebuffer object in an MSAA context: single-sampled, drawn
 *             and sampled as before
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
#include "../common/smooth_shaders.h"
#include "../common/tex_shaders.h"

#ifndef OGLES2_CCT_PISTORM_MSAA
#define OGLES2_CCT_PISTORM_MSAA (OGLES2_CCT_PISTORM_MIN + 5)
#endif

struct GfxBase *GfxBase;
static int failures, checks;
static GLuint smooth, tex1;
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
    return ok ? p : 0;
}

static void programs(void)
{
    smooth = link(smooth_vs, smooth_fs, "a_position", "a_color");
    tex1 = link(tex1_vs, tex1_fs, "a_position", "a_texcoord");
    glUseProgram(smooth);
    glUniformMatrix4fv(glGetUniformLocation(smooth, "u_mvp"), 1, GL_FALSE, identity);
    glUseProgram(tex1);
    glUniform1i(glGetUniformLocation(tex1, "u_tex"), 0);
}

/* A triangle (NDC) at depth z, one colour. */
static void tri(const GLfloat *xy, float z, float r, float g, float b)
{
    GLfloat v[9];
    int i;
    for (i = 0; i < 3; i++) {
        v[i * 3] = xy[i * 2];
        v[i * 3 + 1] = xy[i * 2 + 1];
        v[i * 3 + 2] = z;
    }
    glUseProgram(smooth);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, r, g, b, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void full(float z, float r, float g, float b)
{
    static const GLfloat a[6] = {-1, -1, 1, -1, -1, 1}, c[6] = {1, -1, 1, 1, -1, 1};
    tri(a, z, r, g, b);
    tri(c, z, r, g, b);
}

/* Red channel of the bound framebuffer at (x, y), GL coordinates. */
static int red(int x, int y)
{
    GLubyte p[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return p[0];
}

/* The whole screen read back: how many red values are strictly between
 * 16 and 239 (an edge's mixes). */
static int mixed_pixels(void)
{
    GLubyte *p = malloc(W * H * 4);
    int i, n = 0;
    if (!p)
        return -1;
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, p);
    for (i = 0; i < W * H; i++)
        if (p[i * 4] > 16 && p[i * 4] < 239)
            n++;
    free(p);
    return n;
}

static void *context(struct BitMap *bm, ULONG msaa)
{
    ULONG err = 0;
    void *ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 24,
                                      OGLES2_CCT_STENCIL, 8, OGLES2_CCT_PISTORM_MSAA, msaa, TAG_DONE);
    if (ctx) {
        aglMakeCurrent(ctx);
        programs();
    } else {
        printf("FAIL: a bitmap context, MSAA %lu (error %lu)\n", (unsigned long)msaa, (unsigned long)err);
    }
    return ctx;
}

static const GLfloat slant[6] = {-0.9f, -0.9f, 0.9f, -0.6f, -0.2f, 0.9f};

int main(void)
{
    struct BitMap *bm;
    void *ctx;
    GLint sb = -1, sm = -1;
    ULONG before, after;
    int plain_mixed, msaa_mixed, a, b, c;
    char msg[160];
    printf("ogles2 M6: 4x MSAA on the default framebuffer\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        printf("needs graphics 39 and %s\n", OGLES2NAME);
        return 20;
    }
    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    before = AvailMem(MEMF_ANY);
    if (!bm)
        return 20;

    /* Without MSAA first: the edge's pixels are black or white. */
    if (!(ctx = context(bm, 0)))
        return 20;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sb);
    glGetIntegerv(GL_SAMPLES, &sm);
    sprintf(msg, "no MSAA: GL_SAMPLE_BUFFERS %ld, GL_SAMPLES %ld (0, 0)", (long)sb, (long)sm);
    check(sb == 0 && sm == 0, msg);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    tri(slant, 0, 1, 1, 1);
    plain_mixed = mixed_pixels();
    sprintf(msg, "no MSAA: the slanted edge has %d mixed pixels (0)", plain_mixed);
    check(plain_mixed == 0, msg);
    glDeleteProgram(smooth);
    glDeleteProgram(tex1);
    aglDestroyContext(ctx);

    /* 4x MSAA. */
    if (!(ctx = context(bm, 4)))
        return 20;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sb);
    glGetIntegerv(GL_SAMPLES, &sm);
    sprintf(msg, "MSAA: GL_SAMPLE_BUFFERS %ld, GL_SAMPLES %ld (1, 4)", (long)sb, (long)sm);
    check(sb == 1 && sm == 4, msg);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    tri(slant, 0, 1, 1, 1);
    msaa_mixed = mixed_pixels();
    a = red(W / 2, H / 2);
    b = red(2, H - 3);
    sprintf(msg, "MSAA: the slanted edge has %d mixed pixels (some), inside %d (255), outside %d (0)",
            msaa_mixed, a, b);
    check(msaa_mixed > 50 && a == 255 && b == 0, msg);

    /* Sample coverage. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SAMPLE_COVERAGE);
    glSampleCoverage(0.5f, GL_FALSE);
    full(0, 1, 1, 1);
    a = red(W / 2, H / 2);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glSampleCoverage(0.5f, GL_TRUE);
    full(0, 1, 0, 0);
    glSampleCoverage(0.5f, GL_FALSE);
    full(0, 0, 0, 1);
    b = red(W / 2, H / 2);
    {
        GLubyte p[4];
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        c = p[2];
    }
    glDisable(GL_SAMPLE_COVERAGE);
    sprintf(msg, "coverage 0.5: grey %d (about 128); inverted red %d and plain blue %d (about 128 each)", a, b, c);
    check(a > 100 && a < 156 && b > 100 && b < 156 && c > 100 && c < 156, msg);

    /* A mid-frame split: depth kept with all four samples. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    full(0.0f, 1, 0, 0);       /* red, nearer */
    a = red(W / 2, H / 2);     /* glReadPixels: the job is rendered, depth stored */
    full(0.5f, 0, 1, 0);       /* green, behind: must not show */
    tri(slant, -0.5f, 0, 0, 1); /* blue, in front, its edge antialiased */
    b = red(2, H - 3);         /* outside the blue: still red */
    c = mixed_pixels();
    glDisable(GL_DEPTH_TEST);
    sprintf(msg, "split: red %d before (255), red %d behind the green after (255), %d mixed edge pixels",
            a, b, c);
    check(a == 255 && b == 255 && c > 50, msg);

    /* Stencil across flushes, as dEQP-GLES2 multisample.stencil: a pattern
     * writes stencil 1 (read back: a flush), a colour-only clear (read back:
     * another), then a full quad with GL_EQUAL 1 must give the same picture. */
    {
        GLubyte *p1 = malloc(W * H * 4), *p2 = malloc(W * H * 4);
        int k, diff = 0, lit = 0;
        if (p1 && p2) {
            glClearColor(0, 0, 0, 1);
            glClearStencil(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 1);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            tri(slant, 0, 1, 1, 1);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, p1);
            glClear(GL_COLOR_BUFFER_BIT);
            a = red(W / 2, H / 2);
            glStencilFunc(GL_EQUAL, 1, 1);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            full(0, 1, 1, 1);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, p2);
            glDisable(GL_STENCIL_TEST);
            for (k = 0; k < W * H; k++) {
                if (p1[k * 4] > 128)
                    lit++;
                if (p1[k * 4] != p2[k * 4])
                    diff++;
            }
        }
        free(p1);
        free(p2);
        sprintf(msg, "stencil across two flushes: cleared to %d (0), %d pixels lit, %d differ (0)", a, lit, diff);
        check(a == 0 && lit > 1000 && diff == 0, msg);
    }

    /* An FBO in the MSAA context: single-sampled, drawn, then sampled. */
    {
        GLuint t, f;
        static const GLfloat v[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1};
        GLint fsb = -1;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        glGetIntegerv(GL_SAMPLE_BUFFERS, &fsb);
        glViewport(0, 0, 64, 64);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        tri(slant, 0, 1, 0, 1);
        a = red(32, 32);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(tex1);
        glBindTexture(GL_TEXTURE_2D, t);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, v);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, v + 3);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(1);
        b = red(W / 2, H / 2);
        sprintf(msg, "fbo: GL_SAMPLE_BUFFERS %ld there (0), its centre red %d (255), sampled on the screen %d (255)",
                (long)fsb, a, b);
        check(fsb == 0 && a == 255 && b == 255, msg);
        glDeleteFramebuffers(1, &f);
        glDeleteTextures(1, &t);
    }

    glUseProgram(0);
    glDeleteProgram(smooth);
    glDeleteProgram(tex1);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 32768 >= before, msg);
    FreeBitMap(bm);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
