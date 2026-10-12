/*
 * M6 test: objects shared between contexts (OGLES2_CCT_SHARE_WITH, GLES 2.0
 * appendix C) and several contexts in one program with CPU/GPU overlap
 * (job.c: a framebuffer object's render left running while another context
 * of the task draws). Three hidden 64x64 bitmaps: A, B sharing with A, and
 * C alone. Nothing to watch.
 *   usage: m6_share
 *   names     textures, buffers and programs made in A are B's too, not C's;
 *             a name B makes is not one of A's
 *   draw      B draws with A's program, buffer and texture
 *   rename    B changes a texture A's open job reads: A shows the old image
 *   rtt       A renders into a texture (left running), B samples it; 20 times
 *   delete    B deletes a texture A has bound: A's binding goes to 0
 *   program   A deletes a program current in B: it lives until B lets go
 *   order     A is destroyed first; B keeps the objects
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"
#include "../common/tex_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
#define W 64
#define H 64

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
/* A quad over the viewport: position (3) and texture coordinate (2). */
static const GLfloat quad[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1};

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

/* Each channel of a within 1 of b's (a clear colour of 0.25 stores 3f or 40). */
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

/* The middle of a context's bitmap. */
static unsigned long middle(struct RastPort *rp)
{
    return ReadRGBPixel(rp, W / 2, H / 2) & 0xFFFFFF;
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

/* A texture of one colour (RGBA bytes r, g, b), w x h, or without pixels. */
static GLuint texture(int w, int h, int r, int g, int b, int empty)
{
    static GLubyte px[16 * 16 * 4];
    GLuint t;
    int k;
    for (k = 0; k < w * h && k < 256; k++) {
        px[k * 4] = (GLubyte)r;
        px[k * 4 + 1] = (GLubyte)g;
        px[k * 4 + 2] = (GLubyte)b;
        px[k * 4 + 3] = 255;
    }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, empty ? 0 : px);
    return t;
}

/* The viewport textured with t (program tex1), the quad from buffer vb. */
static void textured(GLuint tex1, GLuint vb, GLuint t)
{
    glUseProgram(tex1);
    glBindTexture(GL_TEXTURE_2D, t);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* The screen of the current context cleared to black, t drawn over it, swapped. */
static void show(GLuint tex1, GLuint vb, GLuint t)
{
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    textured(tex1, vb, t);
    aglSwapBuffers();
}

int main(void)
{
    struct BitMap *bma, *bmb, *bmc;
    struct RastPort rpa, rpb, rpc;
    void *a, *b, *c, *bad;
    ULONG err = 0, before, after;
    GLuint smooth, tex1, vb, t, t2, rt, fb, name;
    GLint bound = -1;
    unsigned long pa, pb;
    char msg[160];
    int k, wrong;

    printf("ogles2 M6: shared objects, several contexts\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    bma = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    bmb = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    bmc = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bma || !bmb || !bmc) {
        printf("FAIL: no bitmaps\n");
        return 20;
    }
    InitRastPort(&rpa);
    rpa.BitMap = bma;
    InitRastPort(&rpb);
    rpb.BitMap = bmb;
    InitRastPort(&rpc);
    rpc.BitMap = bmc;
    before = AvailMem(MEMF_ANY);

    a = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bma, TAG_DONE);
    check(a != 0, "context A");
    if (!a)
        return 20;
    bad = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bmc, OGLES2_CCT_SHARE_WITH, (ULONG)&rpc, TAG_DONE);
    sprintf(msg, "OGLES2_CCT_SHARE_WITH something else: no context, error %lu (%d)", (unsigned long)err,
            OGLES2_CCE_BAD_TAGS);
    check(!bad && err == OGLES2_CCE_BAD_TAGS, msg);
    b = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bmb, OGLES2_CCT_SHARE_WITH, (ULONG)a, TAG_DONE);
    check(b != 0, "context B, sharing with A");
    c = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bmc, TAG_DONE);
    check(c != 0, "context C, alone");
    if (!b || !c)
        return 20;

    /* ---- names: made in A ---- */
    aglMakeCurrent(a);
    smooth = link(smooth_vs, smooth_fs, "a_position", "a_color");
    tex1 = link(tex1_vs, tex1_fs, "a_position", "a_texcoord");
    check(smooth && tex1, "A: the programs link (their .v3dp next to the test)");
    if (!smooth || !tex1)
        return 20;
    glUseProgram(smooth);
    glUniformMatrix4fv(glGetUniformLocation(smooth, "u_mvp"), 1, GL_FALSE, identity);
    glUseProgram(tex1);
    glUniform1i(glGetUniformLocation(tex1, "u_tex"), 0);
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    t = texture(2, 2, 255, 0, 0, 0);

    aglMakeCurrent(b);
    check(glIsTexture(t) && glIsBuffer(vb) && glIsProgram(tex1) && glIsProgram(smooth),
          "B: A's texture, buffer and programs are B's names too");
    glGenTextures(1, &name);
    sprintf(msg, "B: a new texture name (%lu) is not A's (%lu)", (unsigned long)name, (unsigned long)t);
    check(name != t && name != 0, msg);
    glDeleteTextures(1, &name);
    aglMakeCurrent(c);
    check(!glIsTexture(t) && !glIsBuffer(vb) && !glIsProgram(tex1), "C: none of A's names");
    aglMakeCurrent(a);
    glGenTextures(1, &name);
    check(name != t, "A: a name B freed again is handed out, not one in use");
    glDeleteTextures(1, &name);

    /* ---- draw: B with A's objects ---- */
    aglMakeCurrent(b);
    show(tex1, vb, t);
    pb = middle(&rpb);
    sprintf(msg, "B draws A's texture with A's program and buffer: %06lx (ff0000)", pb);
    check(pb == 0xFF0000, msg);

    /* ---- rename: B changes what A's open job reads ---- */
    aglMakeCurrent(a);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    textured(tex1, vb, t); /* A's job, not rendered yet, reads t */
    aglMakeCurrent(b);
    {
        static const GLubyte green[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
        glBindTexture(GL_TEXTURE_2D, t);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, green);
    }
    show(tex1, vb, t);
    pb = middle(&rpb);
    aglMakeCurrent(a);
    aglSwapBuffers();
    pa = middle(&rpa);
    sprintf(msg, "B changes a texture A's open job reads: A %06lx (ff0000, before), B %06lx (00ff00)", pa, pb);
    check(pa == 0xFF0000 && pb == 0x00FF00, msg);

    /* ---- rtt: A renders into a texture, B samples it ---- */
    aglMakeCurrent(a);
    rt = texture(W, H, 0, 0, 0, 1);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "A: framebuffer on texture rt");
    wrong = 0;
    for (k = 0; k < 20; k++) {
        GLfloat r = (k & 1) ? 1.0f : 0.0f, g = (k & 2) ? 1.0f : 0.0f, bl = (k & 4) ? 1.0f : 0.25f;
        unsigned long want = ((unsigned long)(r * 255) << 16) | ((unsigned long)(g * 255) << 8) |
                             (unsigned long)(bl * 255.0f + 0.5f);
        aglMakeCurrent(a);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glViewport(0, 0, W, H);
        glClearColor(r, g, bl, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush(); /* submitted; an object's render is left running (job.c) */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        aglMakeCurrent(b);
        show(tex1, vb, rt); /* B's submission waits for A's render first */
        pb = middle(&rpb);
        if (!close_to(pb, want))
            wrong++;
        if (k == 0) {
            sprintf(msg, "B samples what A rendered into rt: %06lx (%06lx)", pb, want);
            check(close_to(pb, want), msg);
        }
    }
    sprintf(msg, "20 rounds of A rendering, B sampling: %d wrong", wrong);
    check(wrong == 0, msg);

    /* ---- delete: B deletes what A has bound ---- */
    aglMakeCurrent(a);
    glBindTexture(GL_TEXTURE_2D, t);
    aglMakeCurrent(b);
    glDeleteTextures(1, &t);
    check(!glIsTexture(t), "B: the texture deleted");
    aglMakeCurrent(a);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    sprintf(msg, "A: its binding of the texture B deleted is 0 (%ld), the name gone", (long)bound);
    check(bound == 0 && !glIsTexture(t), msg);
    show(tex1, vb, 0);
    pa = middle(&rpa);
    sprintf(msg, "A draws with texture 0: %06lx (000000)", pa);
    check(pa == 0, msg);

    /* ---- program: deleted in A while current in B ---- */
    aglMakeCurrent(b);
    glUseProgram(tex1);
    aglMakeCurrent(a);
    glUseProgram(0); /* A's own use of it ends */
    glDeleteProgram(tex1);
    check(glIsProgram(tex1), "A deletes a program current in B: it lives on");
    aglMakeCurrent(b);
    t2 = texture(2, 2, 0, 0, 255, 0);
    show(tex1, vb, t2);
    pb = middle(&rpb);
    sprintf(msg, "B draws with it: %06lx (0000ff)", pb);
    check(pb == 0x0000FF, msg);
    glUseProgram(0);
    check(!glIsProgram(tex1), "B lets go of it: gone");

    /* ---- order: A first ---- */
    aglMakeCurrent(a);
    glDeleteFramebuffers(1, &fb);
    aglMakeCurrent(0);
    aglDestroyContext(a);
    aglMakeCurrent(b);
    check(glIsBuffer(vb) && glIsTexture(t2) && glIsTexture(rt) && glIsProgram(smooth),
          "A destroyed: B keeps the buffer, textures and program");
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(smooth);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, 1, 1, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    aglSwapBuffers();
    pb = middle(&rpb);
    sprintf(msg, "B draws with A's program and buffer after A is gone: %06lx (ffff00)", pb);
    check(pb == 0xFFFF00, msg);
    check(glGetError() == GL_NO_ERROR, "B: no GL error left over");
    aglMakeCurrent(c);
    check(glGetError() == GL_NO_ERROR, "C: no GL error left over");
    aglMakeCurrent(0);
    aglDestroyContext(b);
    aglDestroyContext(c);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 32768 >= before, msg);
    FreeBitMap(bma);
    FreeBitMap(bmb);
    FreeBitMap(bmc);
    OGLES2Close();
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
