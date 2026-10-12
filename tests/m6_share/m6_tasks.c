/*
 * M6 test: contexts used from two tasks. The client's current context is
 * the program's (libogles2.a), so the second task calls through
 * OGLES2Dispatch with its context. Hidden 64x64 bitmaps; nothing to watch.
 *   usage: m6_tasks
 *   shared   a child task changes a texture shared with the main task's
 *            context (and makes and deletes textures, growing the name
 *            table) while the main task draws with it: every frame shows
 *            one of the colours written, none is dropped
 *   turns    both tasks render into framebuffer objects of their own
 *            contexts (renders left running: job.c's overlap) and read each
 *            result back: every one right, nobody waits out the timeout
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>
#include "../common/tex_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
#define W 64
#define H 64
#define ROUNDS 30

static const GLfloat quad[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1};

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static int close_to(unsigned long a, unsigned long b)
{
    int k;
    for (k = 0; k < 3; k++) {
        int x = (int)((a >> (8 * k)) & 255), y = (int)((b >> (8 * k)) & 255);
        if (x - y > 1 || y - x > 1)
            return 0;
    }
    return 1;
}

/* The colour of round k: never black, distinct for k < 32, each channel
 * 0x40 to 0xfd. */
static unsigned long colour(int k)
{
    return ((unsigned long)(k & 3) * 0x3F + 0x40) << 16 | ((unsigned long)((k >> 2) & 3) * 0x3F + 0x40) << 8 |
           ((unsigned long)((k >> 4) & 1) * 0x80 + 0x40);
}

/* ---- the child task: only exec and the dispatch table (no C library) ---- */
#define D(fn) OGLES2Dispatch->GL##fn

static struct {
    struct Task *parent;
    void *shared_ctx;   /* B: shares with the main task's A */
    void *own_ctx;      /* C: its own, for the turns */
    GLuint tex;         /* the shared texture both draw with */
    volatile int round; /* how many colours it has written */
    int made, wrong, errors;
} child;

static void child_main(void)
{
    void *b = child.shared_ctx, *c = child.own_ctx;
    GLubyte px[16], rd[4];
    GLuint t[8], rt = 0, fb = 0;
    int k, i;
    /* shared: write colour k into the texture, and churn names */
    for (k = 0; k < ROUNDS; k++) {
        unsigned long v = colour(k);
        for (i = 0; i < 4; i++) {
            px[i * 4] = (GLubyte)(v >> 16);
            px[i * 4 + 1] = (GLubyte)(v >> 8);
            px[i * 4 + 2] = (GLubyte)v;
            px[i * 4 + 3] = 255;
        }
        D(BindTexture)(b, GL_TEXTURE_2D, child.tex);
        D(TexSubImage2D)(b, GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
        D(GenTextures)(b, 8, t);
        for (i = 0; i < 8; i++) {
            D(BindTexture)(b, GL_TEXTURE_2D, t[i]);
            D(TexImage2D)(b, GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        child.made += 8;
        D(DeleteTextures)(b, 8, t);
        D(Flush)(b);
        child.round = k + 1;
        Delay(1);
    }
    if (D(GetError)(b) != GL_NO_ERROR)
        child.errors++;
    /* turns: render colour k into its own object, read it back */
    D(GenTextures)(c, 1, &rt);
    D(BindTexture)(c, GL_TEXTURE_2D, rt);
    D(TexImage2D)(c, GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    D(GenFramebuffers)(c, 1, &fb);
    D(BindFramebuffer)(c, GL_FRAMEBUFFER, fb);
    D(FramebufferTexture2D)(c, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    for (k = 0; k < ROUNDS; k++) {
        unsigned long v = colour(31 - k), got;
        D(Viewport)(c, 0, 0, W, H);
        D(ClearColor)(c, (GLfloat)(v >> 16) / 255.0f, (GLfloat)((v >> 8) & 255) / 255.0f,
                      (GLfloat)(v & 255) / 255.0f, 1.0f);
        D(Clear)(c, GL_COLOR_BUFFER_BIT);
        D(Flush)(c); /* submitted and left running */
        D(ReadPixels)(c, W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rd);
        got = ((unsigned long)rd[0] << 16) | ((unsigned long)rd[1] << 8) | rd[2];
        if (!close_to(got, v))
            child.wrong++;
    }
    D(BindFramebuffer)(c, GL_FRAMEBUFFER, 0);
    D(DeleteFramebuffers)(c, 1, &fb);
    D(DeleteTextures)(c, 1, &rt);
    if (D(GetError)(c) != GL_NO_ERROR)
        child.errors++;
    Forbid(); /* the signal arrives after this code has run to its end */
    Signal(child.parent, SIGBREAKF_CTRL_F);
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
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_texcoord");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

int main(void)
{
    struct BitMap *bma, *bmb, *bmc;
    struct RastPort rpa;
    struct Process *proc;
    void *a, *b, *c;
    ULONG err = 0, before, after;
    GLuint tex1, rt, fb;
    GLubyte red[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}, rd[4];
    char msg[160];
    int frames = 0, odd = 0, wrong = 0, k;

    printf("ogles2 M6: contexts used from two tasks\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    bma = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    bmb = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    bmc = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bma || !bmb || !bmc)
        return 20;
    InitRastPort(&rpa);
    rpa.BitMap = bma;
    before = AvailMem(MEMF_ANY);
    a = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bma, TAG_DONE);
    b = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bmb, OGLES2_CCT_SHARE_WITH, (ULONG)a, TAG_DONE);
    c = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bmc, TAG_DONE);
    check(a && b && c, "contexts A, B sharing with A (the child's), C (the child's own)");
    if (!a || !b || !c)
        return 20;

    aglMakeCurrent(a);
    tex1 = link(tex1_vs, tex1_fs);
    check(tex1 != 0, "A: the program links (its .v3dp next to the test)");
    if (!tex1)
        return 20;
    glUseProgram(tex1);
    glUniform1i(glGetUniformLocation(tex1, "u_tex"), 0);
    glGenTextures(1, &child.tex);
    glBindTexture(GL_TEXTURE_2D, child.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, quad + 3);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    /* the main task's own object for the turns */
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &fb);

    child.parent = FindTask(0);
    child.shared_ctx = b;
    child.own_ctx = c;
    SetSignal(0, SIGBREAKF_CTRL_F);
    proc = CreateNewProcTags(NP_Entry, (ULONG)child_main, NP_Name, (ULONG) "m6_tasks child", NP_StackSize, 32768,
                             TAG_DONE);
    check(proc != 0, "the child task starts");
    if (!proc)
        return 20;

    /* shared: draw the texture while the child writes it */
    while (child.round < ROUNDS && frames < 2000) {
        unsigned long p;
        int seen = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, child.tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        aglSwapBuffers();
        frames++;
        p = ReadRGBPixel(&rpa, W / 2, H / 2) & 0xFFFFFF;
        if (p == 0xFF0000)
            seen = 1;
        for (k = 0; k < ROUNDS && !seen; k++)
            if (p == colour(k))
                seen = 1;
        if (!seen && ++odd <= 6)
            printf("  frame %d: %06lx (the child at colour %d)\n", frames, p, child.round);
    }
    sprintf(msg, "A drew %d frames while the child wrote %d colours: %d frames not one of them", frames,
            child.round, odd);
    check(frames > 0 && odd == 0, msg);

    /* turns: render into A's object while the child renders into C's */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    for (k = 0; k < 60; k++) {
        unsigned long v = colour(k & 31), got;
        glViewport(0, 0, W, H);
        glClearColor((GLfloat)(v >> 16) / 255.0f, (GLfloat)((v >> 8) & 255) / 255.0f, (GLfloat)(v & 255) / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rd);
        got = ((unsigned long)rd[0] << 16) | ((unsigned long)rd[1] << 8) | rd[2];
        if (!close_to(got, v) && ++wrong <= 6)
            printf("  render %d: %06lx (%06lx)\n", k, got, v);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sprintf(msg, "A rendered 60 objects alongside the child: %d wrong", wrong);
    check(wrong == 0, msg);

    Wait(SIGBREAKF_CTRL_F); /* the child has finished */
    sprintf(msg, "the child: %d colours written, %d textures made and deleted, %d of its %d renders wrong, "
            "%d GL errors", child.round, child.made, child.wrong, ROUNDS, child.errors);
    check(child.round == ROUNDS && child.made == 8 * ROUNDS && child.wrong == 0 && child.errors == 0, msg);

    /* after both: the texture holds the child's last colour */
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, child.tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    aglSwapBuffers();
    {
        unsigned long p = ReadRGBPixel(&rpa, W / 2, H / 2) & 0xFFFFFF;
        sprintf(msg, "A then shows the child's last colour: %06lx (%06lx)", p, colour(ROUNDS - 1));
        check(p == colour(ROUNDS - 1), msg);
    }
    check(glGetError() == GL_NO_ERROR, "A: no GL error left over");
    glDeleteFramebuffers(1, &fb);
    aglMakeCurrent(0);
    aglDestroyContext(b);
    aglDestroyContext(c);
    aglDestroyContext(a);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 65536 >= before, msg);
    FreeBitMap(bma);
    FreeBitMap(bmb);
    FreeBitMap(bmc);
    OGLES2Close();
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
