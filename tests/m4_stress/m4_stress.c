/*
 * M4 test: a frame too big for its control-list and state blocks (plan
 * section 9, M4 exit: "a stress test that overflows the initial 4 MB binning
 * buffer and the state buffer within one frame shows no dropped frames").
 *   usage: m4_stress [DRAWS]   (default 400000)
 *
 * One frame of DRAWS quads on a hidden 256x128 bitmap, in 64 columns of 4
 * pixels, each draw with its own colour (a constant attribute, so every draw
 * also claims state-buffer memory). Each column must end in the colour of
 * its last draw; a dropped frame leaves the bitmap black. Then an ordinary
 * frame, to see the next frame start cleanly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
static struct RastPort rp;
#define W 256
#define H 128
#define COLS 64

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
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

/* The colour of draw i: 8 bits of red and green from i, blue 255. */
static unsigned long colour_of(unsigned long i)
{
    return ((i & 255) << 16) | (((i >> 8) & 255) << 8) | 255;
}

static unsigned long ticks(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (unsigned long)d.ds_Minute * 3000UL + (unsigned long)d.ds_Tick;
}

int main(int argc, char **argv)
{
    static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static GLfloat pos[COLS * 4 * 3];
    unsigned long draws = argc > 1 ? strtoul(argv[1], 0, 10) : 400000, i, t0, t1, bad = 0;
    ULONG err = 0, before, after;
    struct BitMap *bm;
    void *ctx;
    GLuint p, vbo;
    GLint lp, lc;
    char msg[160];
    int c;

    printf("ogles2 M4: one frame of %lu draws\n", draws);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    before = AvailMem(MEMF_ANY);
    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE))) {
        printf("FAIL: create a bitmap context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    InitRastPort(&rp);
    rp.BitMap = bm;
    p = link(smooth_vs, smooth_fs);
    check(p != 0, "smooth: links");
    if (!p)
        return 20;

    /* Column c: a quad (strip) at x = 4c .. 4c+4, the full height, in a buffer. */
    for (c = 0; c < COLS; c++) {
        int k;
        for (k = 0; k < 4; k++) {
            pos[(c * 4 + k) * 3] = -1.0f + (c * 4 + ((k & 1) ? 4 : 0)) * 2.0f / W;
            pos[(c * 4 + k) * 3 + 1] = (k & 2) ? 1.0f : -1.0f;
            pos[(c * 4 + k) * 3 + 2] = 0;
        }
    }
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glEnableVertexAttribArray(lp);
    glDisableVertexAttribArray(lc); /* the colour: a constant attribute per draw */

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    t0 = ticks();
    for (i = 0; i < draws; i++) {
        unsigned long col = colour_of(i);
        glVertexAttrib4f(lc, ((col >> 16) & 255) / 255.0f, ((col >> 8) & 255) / 255.0f, 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, (GLint)((i % COLS) * 4), 4);
    }
    t1 = ticks();
    aglSwapBuffers();
    printf("     %lu draws issued in %lu ms, %lu draws/s\n", draws, (t1 - t0) * 20,
           t1 > t0 ? draws * 50 / (t1 - t0) : 0);
    for (c = 0; c < COLS; c++) {
        unsigned long last = draws - 1 - ((draws - 1 - (unsigned long)c) % COLS);
        unsigned long want = draws > (unsigned long)c ? colour_of(last) : 0;
        unsigned long got = ReadRGBPixel(&rp, (UWORD)(c * 4 + 2), H / 2) & 0xFFFFFF;
        if (got != want) {
            if (bad < 3)
                printf("     column %d: %06lx, expected %06lx (draw %lu)\n", c, got, want, last);
            bad++;
        }
    }
    sprintf(msg, "every column shows its last draw's colour (%lu of %d wrong)", bad, COLS);
    check(bad == 0, msg);
    check(glGetError() == GL_NO_ERROR, "no GL error");

    /* An ordinary frame after it. */
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttrib4f(lc, 1, 0, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 10 * 4, 4);
    aglSwapBuffers();
    sprintf(msg, "the next frame: column 10 %06lx red, column 20 %06lx green",
            (unsigned long)(ReadRGBPixel(&rp, 42, H / 2) & 0xFFFFFF),
            (unsigned long)(ReadRGBPixel(&rp, 82, H / 2) & 0xFFFFFF));
    check((ReadRGBPixel(&rp, 42, H / 2) & 0xFFFFFF) == 0xFF0000 &&
              (ReadRGBPixel(&rp, 82, H / 2) & 0xFFFFFF) == 0x00FF00,
          msg);

    glDeleteBuffers(1, &vbo);
    glUseProgram(0);
    glDeleteProgram(p);
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 16384 >= before, msg);
    FreeBitMap(bm);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
