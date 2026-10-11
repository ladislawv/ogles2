/*
 * M4 test: aglSetBitmap retargets a bitmap context (OGLES2_CCT_BITMAP).
 *   usage: m4_setbitmap
 * On hidden bitmaps A (256x128), B (160x96) and A2 (256x128), one D16
 * context:
 *   resize   A, then B: the frame fills B to its corners; A keeps its frame;
 *            the size queries answer 160x96; depth across glReadPixels at
 *            the new size
 *   refuse   a planar bitmap is refused and the context stays on B
 *   back     B, then A again at the old size
 *   same     A, then A2 of the same size (no resize)
 *   pending  a frame drawn without aglSwapBuffers goes to the old bitmap
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
static GLint lp, lc;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static struct BitMap *alloc32(UWORD w, UWORD h)
{
    return AllocBitMap(w, h, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
}

/* A pixel of bm, (x, y) from the top-left. */
static unsigned long px(struct BitMap *bm, UWORD x, UWORD y)
{
    struct RastPort rp;
    InitRastPort(&rp);
    rp.BitMap = bm;
    return ReadRGBPixel(&rp, x, y) & 0xFFFFFF;
}

/* Every corner and the centre of a w x h bitmap are c. */
static int filled(struct BitMap *bm, UWORD w, UWORD h, unsigned long c)
{
    return px(bm, 0, 0) == c && px(bm, w - 1, 0) == c && px(bm, 0, h - 1) == c &&
           px(bm, w - 1, h - 1) == c && px(bm, w / 2, h / 2) == c;
}

static void size_of(ULONG *w, ULONG *h)
{
    *w = *h = 0;
    aglSetParamsTags2(OGLES2_CCT_GET_WIDTH, (ULONG)w, OGLES2_CCT_GET_HEIGHT, (ULONG)h, TAG_DONE);
}

static void clear(float r, float g, float b)
{
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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

int main(void)
{
    static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    struct BitMap *a, *b, *a2, *planar;
    ULONG err = 0, before, after, w, h;
    unsigned long l, r;
    GLubyte px4[4];
    char msg[200];
    void *ctx;
    GLuint p;

    printf("ogles2 M4: aglSetBitmap\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    before = AvailMem(MEMF_ANY);
    a = alloc32(256, 128);
    b = alloc32(160, 96);
    a2 = alloc32(256, 128);
    planar = AllocBitMap(64, 64, 4, BMF_CLEAR, 0);
    if (!a || !b || !a2 || !planar ||
        !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)a, OGLES2_CCT_DEPTH, 16, TAG_DONE))) {
        printf("FAIL: create the bitmaps and a bitmap context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    p = link(smooth_vs, smooth_fs);
    check(p != 0, "smooth: links");
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");

    clear(1, 0, 0);
    aglSwapBuffers();
    check(filled(a, 256, 128, 0xFF0000), "A (256x128): red");

    /* Resize: A to B. */
    aglSetBitmap(b);
    size_of(&w, &h);
    sprintf(msg, "after aglSetBitmap(B): the size queries answer %lux%lu (160x96)", (unsigned long)w, (unsigned long)h);
    check(w == 160 && h == 96, msg);
    glViewport(0, 0, 160, 96);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    clear(0, 1, 0);
    check(filled(a, 256, 128, 0xFF0000), "A untouched by B's clear");
    rect(-1, 0, -0.5f, 0, 0, 1);                     /* left half near, blue */
    glReadPixels(40, 48, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px4); /* renders the job */
    rect(-1, 1, 0.5f, 1, 0, 1);                      /* far, magenta: hidden on the left */
    aglSwapBuffers();
    glDisable(GL_DEPTH_TEST);
    l = px(b, 40, 48);
    r = px(b, 120, 48);
    sprintf(msg, "B: depth across glReadPixels at the new size: left %06lx (0000ff), right %06lx (ff00ff), read %02x%02x%02x",
            l, r, px4[0], px4[1], px4[2]);
    check(l == 0x0000FF && r == 0xFF00FF && px4[2] == 255 && px4[0] == 0, msg);
    sprintf(msg, "B: the frame reaches B's corners: %06lx %06lx %06lx %06lx", px(b, 0, 0), px(b, 159, 0),
            px(b, 0, 95), px(b, 159, 95));
    check(px(b, 0, 0) == 0x0000FF && px(b, 159, 0) == 0xFF00FF && px(b, 0, 95) == 0x0000FF &&
              px(b, 159, 95) == 0xFF00FF, msg);
    check(filled(a, 256, 128, 0xFF0000), "A keeps its frame (red)");

    /* Refused: a planar bitmap. */
    aglSetBitmap(planar);
    size_of(&w, &h);
    clear(1, 1, 0);
    aglSwapBuffers();
    sprintf(msg, "a planar bitmap is refused: still %lux%lu, B yellow", (unsigned long)w, (unsigned long)h);
    check(w == 160 && h == 96 && filled(b, 160, 96, 0xFFFF00), msg);

    /* Back to A at the old size. */
    aglSetBitmap(a);
    size_of(&w, &h);
    glViewport(0, 0, 256, 128);
    clear(0, 1, 1);
    aglSwapBuffers();
    sprintf(msg, "back to A: %lux%lu, A cyan to its corners, B still yellow", (unsigned long)w, (unsigned long)h);
    check(w == 256 && h == 128 && filled(a, 256, 128, 0x00FFFF) && filled(b, 160, 96, 0xFFFF00), msg);

    /* The same size: A to A2. */
    aglSetBitmap(a2);
    clear(1, 0, 1);
    aglSwapBuffers();
    check(filled(a2, 256, 128, 0xFF00FF) && filled(a, 256, 128, 0x00FFFF),
          "A2 (same size): magenta, A still cyan");

    /* A frame not swapped yet goes to the bitmap it was drawn for. */
    clear(1, 1, 1);
    aglSetBitmap(a);
    check(filled(a2, 256, 128, 0xFFFFFF) && filled(a, 256, 128, 0x00FFFF),
          "an unswapped frame is rendered into the old bitmap (A2 white), A still cyan");
    clear(0, 0, 1);
    aglSwapBuffers();
    sprintf(msg, "A after that: blue: %06lx %06lx %06lx %06lx %06lx", px(a, 0, 0), px(a, 255, 0),
            px(a, 0, 127), px(a, 255, 127), px(a, 128, 64));
    check(filled(a, 256, 128, 0x0000FF), msg);

    glUseProgram(0);
    glDeleteProgram(p);
    check(glGetError() == GL_NO_ERROR, "no GL error");
    aglDestroyContext(ctx);
    FreeBitMap(planar);
    FreeBitMap(a2);
    FreeBitMap(b);
    FreeBitMap(a);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 16384 >= before, msg);

    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
