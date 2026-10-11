/*
 * M2 test: buffer objects (glGenBuffers, glBindBuffer, glBufferData,
 * glBufferSubData, glDeleteBuffers), vertex and element arrays read from
 * them, on a hidden 256x128 bitmap.
 *   usage: m2_buffers
 *
 *   objects   names, binding, sizes, usage, the errors GLES 2.0 2.9 asks for
 *   arrays    interleaved positions and colours (float and normalized
 *             bytes) from a buffer, drawn from a first vertex above 0
 *   rename    glBufferSubData between two draws of one frame: the first
 *             draw keeps the data it was given (plan 7.5)
 *   elements  8- and 16-bit indices from an element buffer, at offsets
 *   mixed     one attribute from a buffer, one from client memory
 *   delete    deleting a buffer that is still bound unbinds it everywhere
 *   reuse     30 frames from one unchanged buffer (one GPU copy, kept)
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
static GLuint prog;
static GLint lp, lc;
#define W 256
#define H 128

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
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

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/* An interleaved vertex: position, then colour. */
struct vtx
{
    GLfloat x, y, z;
    GLfloat r, g, b, a;
};

/* Quad k of four across the bitmap (x from -1 + 0.5k), as a strip. */
static void quad(struct vtx *v, int k, float r, float g, float b)
{
    int i;
    for (i = 0; i < 4; i++) {
        v[i].x = -0.95f + 0.5f * k + ((i & 1) ? 0.4f : 0.0f);
        v[i].y = (i & 2) ? 0.8f : -0.8f;
        v[i].z = 0;
        v[i].r = r;
        v[i].g = g;
        v[i].b = b;
        v[i].a = 1;
    }
}

/* The centre of quad k. */
static unsigned long at(int k)
{
    return ndc(-0.75f + 0.5f * k, 0.0f);
}

static void interleaved(void)
{
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (const void *)0);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (const void *)12);
    glEnableVertexAttribArray(lp);
    glEnableVertexAttribArray(lc);
}

static void test_objects(void)
{
    GLuint b[3] = {0, 0, 0};
    GLint v = -1;
    char msg[120];
    glGenBuffers(3, b);
    sprintf(msg, "glGenBuffers: three distinct names (%lu %lu %lu)", (unsigned long)b[0],
            (unsigned long)b[1], (unsigned long)b[2]);
    check(b[0] && b[1] && b[2] && b[0] != b[1] && b[1] != b[2] && b[0] != b[2], msg);
    check(!glIsBuffer(b[0]), "glIsBuffer: false until first bound");
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    check(glIsBuffer(b[0]), "glIsBuffer: true once bound");
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &v);
    check(v == (GLint)b[0], "GL_ARRAY_BUFFER_BINDING");
    glBufferData(GL_ARRAY_BUFFER, 100, 0, GL_DYNAMIC_DRAW);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &v);
    check(v == 100, "GL_BUFFER_SIZE 100");
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_USAGE, &v);
    check(v == GL_DYNAMIC_DRAW, "GL_BUFFER_USAGE GL_DYNAMIC_DRAW");
    check(glGetError() == GL_NO_ERROR, "no error so far");
    glBufferSubData(GL_ARRAY_BUFFER, 96, 8, identity);
    check(glGetError() == GL_INVALID_VALUE, "glBufferSubData past the end: GL_INVALID_VALUE");
    glBufferData(GL_ARRAY_BUFFER, 16, 0, GL_STATIC_DRAW + 1);
    check(glGetError() == GL_INVALID_ENUM, "glBufferData with a bad usage: GL_INVALID_ENUM");
    glBindBuffer(GL_TEXTURE_2D, b[1]);
    check(glGetError() == GL_INVALID_ENUM, "glBindBuffer with a bad target: GL_INVALID_ENUM");
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBufferData(GL_ARRAY_BUFFER, 16, 0, GL_STATIC_DRAW);
    check(glGetError() == GL_INVALID_OPERATION, "glBufferData with no buffer bound: GL_INVALID_OPERATION");
    glBindBuffer(GL_ARRAY_BUFFER, 77); /* a name glGenBuffers did not return */
    check(glGetError() == GL_NO_ERROR && glIsBuffer(77), "glBindBuffer creates an unused name");
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    {
        GLuint n77 = 77;
        glDeleteBuffers(1, &n77);
    }
    glDeleteBuffers(3, b);
    check(!glIsBuffer(b[0]) && !glIsBuffer(77), "glDeleteBuffers: the names are gone");
}

static void test_arrays(void)
{
    struct vtx v[12];
    struct
    {
        GLfloat x, y, z;
        GLubyte r, g, b, a;
    } bv[4];
    GLuint b[2];
    GLint q = -1;
    unsigned long p0, p1, p2, p3;
    char msg[160];
    int i;
    /* Vertices 0-3: quad 0 white, never drawn; 4-7: quad 1 red; 8-11: quad 2 green. */
    quad(v, 0, 1, 1, 1);
    quad(v + 4, 1, 1, 0, 0);
    quad(v + 8, 2, 0, 1, 0);
    for (i = 0; i < 4; i++) { /* quad 3 blue, with byte colours */
        bv[i].x = -0.95f + 1.5f + ((i & 1) ? 0.4f : 0.0f);
        bv[i].y = (i & 2) ? 0.8f : -0.8f;
        bv[i].z = 0;
        bv[i].r = 0;
        bv[i].g = 0;
        bv[i].b = 255;
        bv[i].a = 255;
    }
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    interleaved();
    glGetVertexAttribiv(lc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &q);
    check(q == (GLint)b[0], "GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING names the buffer");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
    glBindBuffer(GL_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bv), bv, GL_STATIC_DRAW);
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, sizeof(bv[0]), (const void *)0);
    glVertexAttribPointer(lc, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(bv[0]), (const void *)12);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    aglSwapBuffers();
    p0 = at(0);
    p1 = at(1);
    p2 = at(2);
    p3 = at(3);
    sprintf(msg, "buffer arrays: quad 0 %06lx empty, 1 %06lx red, 2 %06lx green, 3 %06lx blue (bytes)",
            p0, p1, p2, p3);
    check(p0 == 0 && p1 == 0xFF0000 && p2 == 0x00FF00 && p3 == 0x0000FF, msg);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(2, b);
}

static void test_rename(void)
{
    struct vtx v[8];
    GLuint b;
    unsigned long p0, p1;
    char msg[160];
    int i;
    quad(v, 0, 1, 0, 0);
    quad(v + 4, 1, 1, 0, 0);
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    interleaved();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* quad 0, red */
    /* Quad 0 green (too late for the draw above), quad 1 blue. */
    for (i = 0; i < 4; i++) {
        v[i].r = 0;
        v[i].g = 1;
        v[4 + i].r = 0;
        v[4 + i].b = 1;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    aglSwapBuffers();
    p0 = at(0);
    p1 = at(1);
    sprintf(msg, "glBufferSubData between draws: quad 0 %06lx stays red, quad 1 %06lx blue", p0, p1);
    check(p0 == 0xFF0000 && p1 == 0x0000FF, msg);
    /* And glBufferData (new storage) the same way. */
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* quad 0, green now */
    quad(v, 0, 1, 1, 0);
    quad(v + 4, 1, 1, 1, 0);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    aglSwapBuffers();
    p0 = at(0);
    p1 = at(1);
    sprintf(msg, "glBufferData between draws: quad 0 %06lx green, quad 1 %06lx yellow", p0, p1);
    check(p0 == 0x00FF00 && p1 == 0xFFFF00, msg);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &b);
}

static void test_elements(void)
{
    struct vtx v[12];
    /* 8-bit indices at 0 (quad 1 from vertices 4-7), then two pad bytes,
     * 16-bit indices at 8 (quad 2 from 8-11). */
    struct
    {
        GLubyte i8[6];
        GLubyte pad[2];
        GLushort i16[6];
    } ix = {{4, 5, 6, 6, 5, 7}, {0, 0}, {8, 9, 10, 10, 9, 11}};
    GLuint b[2];
    GLint q = -1;
    unsigned long p0, p1, p2;
    char msg[160];
    quad(v, 0, 1, 1, 1);
    quad(v + 4, 1, 1, 0, 1);
    quad(v + 8, 2, 0, 1, 1);
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    interleaved();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(ix), &ix, GL_STATIC_DRAW);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &q);
    check(q == (GLint)b[1], "GL_ELEMENT_ARRAY_BUFFER_BINDING");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, (const void *)0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)8);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)16); /* past the end: nothing */
    aglSwapBuffers();
    p0 = at(0);
    p1 = at(1);
    p2 = at(2);
    sprintf(msg, "element buffer: quad 0 %06lx empty, 8-bit %06lx magenta, 16-bit at 8 %06lx cyan",
            p0, p1, p2);
    check(p0 == 0 && p1 == 0xFF00FF && p2 == 0x00FFFF, msg);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(2, b);
}

static void test_mixed(void)
{
    struct vtx v[4];
    static GLfloat col[16];
    GLuint b;
    unsigned long p2;
    char msg[160];
    int i;
    quad(v, 2, 1, 1, 1); /* white in the buffer, unused */
    for (i = 0; i < 4; i++) {
        col[i * 4] = 1.0f;
        col[i * 4 + 1] = 0.5f;
        col[i * 4 + 2] = 0;
        col[i * 4 + 3] = 1;
    }
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (const void *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, col); /* client memory */
    glEnableVertexAttribArray(lp);
    glEnableVertexAttribArray(lc);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 8); /* reads past the buffer: nothing drawn */
    aglSwapBuffers();
    p2 = at(2);
    sprintf(msg, "positions from a buffer, colours from client memory: %06lx orange", p2);
    check((p2 & 0xFF0000) == 0xFF0000 && ((p2 >> 8) & 255) >= 0x70 && ((p2 >> 8) & 255) <= 0x90 &&
              (p2 & 255) == 0,
          msg);

    /* Deleting it while attribute lp still reads it. */
    {
        GLint q = -1;
        glBindBuffer(GL_ARRAY_BUFFER, b);
        glDeleteBuffers(1, &b);
        glGetVertexAttribiv(lp, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &q);
        check(q == 0, "deleting a bound buffer unbinds it from the attribute");
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &q);
        check(q == 0, "... and from GL_ARRAY_BUFFER");
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* lp has no array now: nothing drawn */
        aglSwapBuffers();
        check(at(2) == 0, "and a draw that needs it draws nothing");
    }
}

/* glBufferSubData between frames: the copy is patched where the bytes
 * changed (plan 7.5); the other quads keep theirs. */
static void test_partial(void)
{
    struct vtx v[16];
    unsigned long p[4];
    char msg[160];
    GLuint b;
    int i;
    quad(v, 0, 1, 0, 0);
    quad(v + 4, 1, 0, 1, 0);
    quad(v + 8, 2, 0, 0, 1);
    quad(v + 12, 3, 1, 1, 1);
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    interleaved();
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 4; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, i * 4, 4);
    aglSwapBuffers();
    /* Quad 2's colours only (bytes in the middle of the buffer). */
    for (i = 8; i < 12; i++) {
        v[i].r = 1;
        v[i].g = 0;
        v[i].b = 1;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 8 * sizeof(struct vtx), 4 * sizeof(struct vtx), v + 8);
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 4; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, i * 4, 4);
    aglSwapBuffers();
    for (i = 0; i < 4; i++)
        p[i] = at(i);
    sprintf(msg, "glBufferSubData of quad 2 between frames: %06lx %06lx %06lx %06lx (ff0000 00ff00 ff00ff ffffff)",
            p[0], p[1], p[2], p[3]);
    check(p[0] == 0xFF0000 && p[1] == 0x00FF00 && p[2] == 0xFF00FF && p[3] == 0xFFFFFF, msg);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &b);
}

/* OES_mapbuffer: the mapped buffer is written through the pointer. */
static void test_map(void)
{
    struct vtx v[4], *m;
    void *ptr = (void *)1;
    GLint mapped = -1, access = 0;
    unsigned long c;
    char msg[160];
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    GLuint b;
    int i;
    quad(v, 0, 1, 0, 0);
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    interleaved();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* red, then mapped and changed */
    aglSwapBuffers();
    m = (struct vtx *)glMapBufferOES(GL_ARRAY_BUFFER, 0x88B9 /* GL_WRITE_ONLY_OES */);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, 0x88BC /* GL_BUFFER_MAPPED_OES */, &mapped);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, 0x88BB /* GL_BUFFER_ACCESS_OES */, &access);
    glGetBufferPointervOES(GL_ARRAY_BUFFER, 0x88BD /* GL_BUFFER_MAP_POINTER_OES */, &ptr);
    check(ext && strstr(ext, "GL_OES_mapbuffer") && m && mapped == GL_TRUE && access == 0x88B9 && ptr == m,
          "glMapBufferOES: a pointer, GL_BUFFER_MAPPED_OES, the access, glGetBufferPointervOES");
    glMapBufferOES(GL_ARRAY_BUFFER, 0x88B9);
    check(glGetError() == GL_INVALID_OPERATION, "a second glMapBufferOES: GL_INVALID_OPERATION");
    if (m)
        for (i = 0; i < 4; i++) {
            m[i].r = 0;
            m[i].g = 0;
            m[i].b = 1;
        }
    check(glUnmapBufferOES(GL_ARRAY_BUFFER) == GL_TRUE, "glUnmapBufferOES returns GL_TRUE");
    glGetBufferParameteriv(GL_ARRAY_BUFFER, 0x88BC, &mapped);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    aglSwapBuffers();
    c = at(0);
    sprintf(msg, "drawn after the mapping: %06lx (0000ff), mapped now %ld", c, (long)mapped);
    check(c == 0x0000FF && mapped == GL_FALSE, msg);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &b);
}

static void test_reuse(void)
{
    struct vtx v[4];
    GLuint b;
    int f, good = 0;
    char msg[120];
    quad(v, 1, 0, 0, 1);
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    interleaved();
    for (f = 0; f < 30; f++) {
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        aglSwapBuffers();
        good += at(1) == 0x0000FF;
    }
    sprintf(msg, "30 frames from one unchanged buffer: %d drawn", good);
    check(good == 30, msg);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &b);
}

int main(void)
{
    ULONG err = 0, before, after;
    struct BitMap *bm;
    void *ctx;
    char msg[100];
    printf("ogles2 M2: buffer objects\n");
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
    prog = link(smooth_vs, smooth_fs);
    check(prog != 0, "smooth: links");
    if (prog) {
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "u_mvp"), 1, GL_FALSE, identity);
        lp = glGetAttribLocation(prog, "a_position");
        lc = glGetAttribLocation(prog, "a_color");
        glClearColor(0, 0, 0, 1);
        test_objects();
        test_arrays();
        test_rename();
        test_elements();
        test_mixed();
        test_partial();
        test_map();
        test_reuse();
        glUseProgram(0);
        glDeleteProgram(prog);
    }
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 16384 >= before, msg);
    FreeBitMap(bm);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
