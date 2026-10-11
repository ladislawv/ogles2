/*
 * M4 test: depth and stencil across a mid-frame flush (plan 7.2). A job
 * that ends before its frame (glReadPixels, glFlush, glFinish, glCopyTex*)
 * stores depth and stencil, and the next job of the frame loads them.
 *   usage: m4_depth
 * On hidden 256x128 bitmaps, with a D16 context and a D24S8 one:
 *   depth    a near red quad, glReadPixels, a far green quad over it: red stays
 *   stencil  the left half marked, glFlush, green where stencil == 1: left only
 *   clear    after a flush, a depth-only clear keeps the stencil marks
 *   frame    after aglSwapBuffers the next frame starts from its own clears
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
#include "../common/smooth_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
static struct RastPort rp;
static const char *kind;
static GLint lp, lc;
#define W 256
#define H 128

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", kind, what);
}

static unsigned long ndc(float x, float y)
{
    int px = (int)((x + 1.0f) * 0.5f * W), py = (int)((y + 1.0f) * 0.5f * H);
    return ReadRGBPixel(&rp, (UWORD)px, (UWORD)(H - 1 - py)) & 0xFFFFFF;
}

static GLuint link(const char *vs, const char *fs)
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
    return ok ? p : 0;
}

/* A rectangle x0..x1 (NDC), full height, at depth z (NDC), in one colour. */
static void rect(float x0, float x1, float z, float r, float g, float b)
{
    GLfloat v[12];
    v[0] = x0; v[1] = -1; v[2] = z;
    v[3] = x1; v[4] = -1; v[5] = z;
    v[6] = x0; v[7] = 1;  v[8] = z;
    v[9] = x1; v[10] = 1; v[11] = z;
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(lp);
    glDisableVertexAttribArray(lc);
    glVertexAttrib4f(lc, r, g, b, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void run(int depth, int stencil)
{
    static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    struct BitMap *bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    ULONG err = 0, before = AvailMem(MEMF_ANY), after;
    void *ctx;
    GLuint p;
    GLubyte px4[4];
    unsigned long a, b;
    char msg[160];

    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, depth,
                                             OGLES2_CCT_STENCIL, stencil, TAG_DONE))) {
        check(0, "create a bitmap context");
        return;
    }
    aglMakeCurrent(ctx);
    InitRastPort(&rp);
    rp.BitMap = bm;
    p = link(smooth_vs, smooth_fs);
    check(p != 0, "smooth: links");
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");

    /* Depth across glReadPixels. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    rect(-1, 1, -0.5f, 1, 0, 0);                     /* near, red */
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px4); /* renders the job */
    rect(-1, 1, 0.5f, 0, 1, 0);                      /* far, green: hidden */
    aglSwapBuffers();
    a = ndc(0, 0);
    sprintf(msg, "depth kept across glReadPixels: %06lx (ff0000), read mid-frame %02x%02x%02x", a,
            px4[0], px4[1], px4[2]);
    check(a == 0xFF0000 && px4[0] == 255 && px4[1] == 0, msg);
    glDisable(GL_DEPTH_TEST);

    if (stencil) {
        /* Stencil across glFlush: the left half marked 1. */
        glEnable(GL_STENCIL_TEST);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        rect(-1, 0, 0, 1, 1, 1);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glFlush();                                   /* ends the job mid-frame */
        glStencilFunc(GL_EQUAL, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        rect(-1, 1, 0, 0, 1, 0);                     /* green where marked */
        aglSwapBuffers();
        a = ndc(-0.5f, 0);
        b = ndc(0.5f, 0);
        sprintf(msg, "stencil kept across glFlush: left %06lx (00ff00), right %06lx (000000)", a, b);
        check(a == 0x00FF00 && b == 0, msg);

        /* A depth-only clear after a mid-frame flush keeps the stencil. */
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        rect(0, 1, 0, 1, 1, 1);                      /* right half marked */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glFinish();
        glClear(GL_DEPTH_BUFFER_BIT);                /* depth only */
        glStencilFunc(GL_EQUAL, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        rect(-1, 1, 0, 0, 0, 1);                     /* blue where marked */
        aglSwapBuffers();
        a = ndc(-0.5f, 0);
        b = ndc(0.5f, 0);
        sprintf(msg, "a depth-only clear after glFinish keeps the stencil: left %06lx (000000), right %06lx (0000ff)", a, b);
        check(a == 0 && b == 0x0000FF, msg);
        glDisable(GL_STENCIL_TEST);
    }

    /* The next frame: its own clears, nothing from before. */
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    rect(-1, 1, 0.5f, 1, 1, 0);                      /* far, yellow: drawn */
    aglSwapBuffers();
    a = ndc(0, 0);
    sprintf(msg, "the next frame starts from its own clear: %06lx (ffff00)", a);
    check(a == 0xFFFF00, msg);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(0);
    glDeleteProgram(p);
    check(glGetError() == GL_NO_ERROR, "no GL error");
    aglDestroyContext(ctx);
    FreeBitMap(bm);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 16384 >= before, msg);
}

int main(void)
{
    printf("ogles2 M4: depth and stencil across a mid-frame flush\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    kind = "D16";
    run(16, 0);
    kind = "D24S8";
    run(24, 8);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
