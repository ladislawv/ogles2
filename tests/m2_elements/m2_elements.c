/*
 * M2 test: glDrawElements with client indices (8, 16 and 32 bit), glReadPixels,
 * and an indirectly indexed uniform array (QUNIFORM_UBO_ADDR), on a hidden
 * 256x128 bitmap.
 *   usage: m2_elements [DIR]   (DIR: the prefix of shaders/, as m2_link)
 *
 *   smooth  (tests/common) drawn through 8- and 16-bit indices that start
 *           above 0 (the runtime copies only the range used and rebases)
 *   skin    (tools/v3dglslc/test/shaders) picks bone[] matrices by a vertex
 *           attribute: the shader reads its constant buffer through the TMU
 *   base    OES_draw_elements_base_vertex: the same indices drawn with base
 *           vertex 0, 4 and (from higher indices) -4, client and buffer indices
 *   minmax  EXT_blend_minmax: GL_MIN and GL_MAX against a grey background,
 *           with factors that would change the result if they were applied
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

#ifndef GL_MIN_EXT /* EXT_blend_minmax (gl2ext.h) */
#define GL_MIN_EXT 0x8007
#define GL_MAX_EXT 0x8008
#endif

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
static const char *dir = "";
static struct RastPort rp;
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

static char *read_text(const char *name, const char *ext)
{
    char path[160];
    FILE *f;
    long n;
    char *s;
    sprintf(path, "%sshaders/%s.%s", dir, name, ext);
    if (!(f = fopen(path, "rb")))
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if ((s = malloc(n + 1)) && fread(s, 1, n, f) == (size_t)n)
        s[n] = 0;
    fclose(f);
    return s;
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
    if (!ok) {
        char log[200];
        glGetProgramInfoLog(p, sizeof(log), 0, log);
        printf("     link failed: %s\n", log);
        return 0;
    }
    return p;
}

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static void test_elements(void)
{
    /* 12 vertices; the triangles use 4..9 only. Left quad red, right green. */
    static GLfloat pos[12 * 3], col[12 * 4];
    static const UBYTE i8[6] = {4, 5, 6, 6, 5, 7};          /* left quad */
    static const UWORD i16[6] = {8, 9, 6 + 4, 10, 9, 11};   /* right quad, from 8 */
    char msg[160];
    unsigned long a, b, c;
    GLuint p = link(smooth_vs, smooth_fs);
    GLint lp, lc;
    int k;
    check(p != 0, "smooth: links");
    if (!p)
        return;
    for (k = 0; k < 12; k++) {
        float x0 = k < 8 ? -0.9f : 0.1f;
        int q = (k - 4) & 3;
        pos[k * 3] = x0 + ((q & 1) ? 0.8f : 0.0f);
        pos[k * 3 + 1] = (q & 2) ? 0.8f : -0.8f;
        pos[k * 3 + 2] = 0;
        col[k * 4] = k < 8 ? 1.0f : 0.0f;
        col[k * 4 + 1] = k < 8 ? 0.0f : 1.0f;
        col[k * 4 + 2] = 0;
        col[k * 4 + 3] = 1;
    }
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(lp);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, col);
    glEnableVertexAttribArray(lc);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, i8);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, i16);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    c = ndc(0.0f, 0.95f);
    sprintf(msg, "glDrawElements 8-bit (indices 4-7) %06lx red, 16-bit (8-11) %06lx green, gap %06lx",
            a, b, c);
    check(a == 0xFF0000 && b == 0x00FF00 && c == 0, msg);
    {
        /* OES_element_index_uint: the right quad again, through 32-bit indices. */
        static const ULONG i32[6] = {8, 9, 10, 10, 9, 11};
        GLubyte px[2 * 4 * 2];
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        GLint fmt = 0, typ = 0;
        check(ext && strstr(ext, "GL_OES_element_index_uint") != 0, "GL_EXTENSIONS lists GL_OES_element_index_uint");
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, i32);
        /* glReadPixels renders the open job first; rows bottom-up. */
        memset(px, 0x55, sizeof(px));
        glReadPixels(W * 3 / 4, H / 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
        sprintf(msg, "glReadPixels in the open frame: %02x%02x%02x%02x green", px[0], px[1], px[2], px[3]);
        check(px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255 && px[12] == 0 && px[13] == 255, msg);
        glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        sprintf(msg, "glReadPixels where nothing was drawn: %02x%02x%02x black", px[0], px[1], px[2]);
        check(px[0] == 0 && px[1] == 0 && px[2] == 0, msg);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &typ);
        check(fmt == GL_RGBA && typ == GL_UNSIGNED_BYTE, "implementation read format RGBA, UNSIGNED_BYTE");
        glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        check(glGetError() == GL_INVALID_OPERATION, "glReadPixels GL_RGB: GL_INVALID_OPERATION");
        aglSwapBuffers();
        a = ndc(-0.5f, 0.0f);
        b = ndc(0.5f, 0.0f);
        sprintf(msg, "glDrawElements 32-bit (8-11): left %06lx empty, right %06lx green", a, b);
        check(a == 0 && b == 0x00FF00, msg);
    }
    glDisableVertexAttribArray(lc);
    glUseProgram(0);
    glDeleteProgram(p);
}

static void test_skin(void)
{
    /* One triangle left of the centre; bone[5] moves it right by 0.6,
     * bone[20] up by 0.5. The shade is p.y * 0.5 + 0.5. */
    static const GLfloat pos[12] = {-0.8f, -0.3f, 0, 1, -0.2f, -0.3f, 0, 1, -0.5f, 0.3f, 0, 1};
    static const GLfloat wts[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    GLfloat bones[12];
    GLfloat m[16];
    char msg[160], *vs = read_text("skin", "vert"), *fs = read_text("skin", "frag");
    unsigned long a, b, c;
    GLuint p;
    GLint loc, k, lpos, lw, lb;
    if (!vs || !fs) {
        check(0, "skin: read shaders/skin.vert and .frag");
        return;
    }
    p = link(vs, fs);
    check(p != 0, "skin: links (UBO_ADDR)");
    if (!p)
        return;
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "viewproj"), 1, GL_FALSE, identity);
    loc = glGetUniformLocation(p, "bone");
    for (k = 0; k < 24; k++)
        glUniformMatrix4fv(loc + k, 1, GL_FALSE, identity);
    memcpy(m, identity, sizeof(m));
    m[12] = 0.6f; /* bone[5]: x + 0.6 */
    glUniformMatrix4fv(loc + 5, 1, GL_FALSE, m);
    memcpy(m, identity, sizeof(m));
    m[13] = 0.5f; /* bone[20]: y + 0.5 */
    glUniformMatrix4fv(loc + 20, 1, GL_FALSE, m);
    lpos = glGetAttribLocation(p, "pos");
    lw = glGetAttribLocation(p, "weights");
    lb = glGetAttribLocation(p, "bones");
    glVertexAttribPointer(lpos, 4, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(lpos);
    glVertexAttribPointer(lw, 4, GL_FLOAT, GL_FALSE, 0, wts);
    glEnableVertexAttribArray(lw);
    glVertexAttribPointer(lb, 4, GL_FLOAT, GL_FALSE, 0, bones);
    glEnableVertexAttribArray(lb);

    for (k = 0; k < 12; k++)
        bones[k] = 5.0f;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    aglSwapBuffers();
    a = ndc(0.1f, -0.15f);  /* inside the moved triangle */
    b = ndc(-0.5f, -0.15f); /* where it was */
    sprintf(msg, "skin: bone[5] moves the triangle right: %06lx drawn, %06lx empty", a, b);
    check(a != 0 && b == 0, msg);

    for (k = 0; k < 12; k++)
        bones[k] = 20.0f;
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.35f);  /* inside the raised triangle */
    b = ndc(-0.5f, -0.25f); /* below it */
    c = ndc(-0.5f, 0.35f);
    sprintf(msg, "skin: bone[20] moves it up: %06lx drawn (grey about %02lx), %06lx empty", a,
            (unsigned long)((0.35f * 0.5f + 0.5f) * 255), b);
    check(a != 0 && b == 0 && ((c >> 16) & 255) > 0x90, msg);
    glUseProgram(0);
    glDeleteProgram(p);
    free(vs);
    free(fs);
}

/* Left quad red (vertices 0-3), right quad green (4-7). */
static GLfloat q_pos[8 * 3], q_col[8 * 4];

static void two_quads(void)
{
    int k;
    for (k = 0; k < 8; k++) {
        int q = k & 3;
        q_pos[k * 3] = (k < 4 ? -0.9f : 0.1f) + ((q & 1) ? 0.8f : 0.0f);
        q_pos[k * 3 + 1] = (q & 2) ? 0.8f : -0.8f;
        q_pos[k * 3 + 2] = 0;
        q_col[k * 4] = k < 4 ? 1.0f : 0.0f;
        q_col[k * 4 + 1] = k < 4 ? 0.0f : 1.0f;
        q_col[k * 4 + 2] = 0;
        q_col[k * 4 + 3] = 1;
    }
}

static void test_base_vertex(void)
{
    static const UWORD lo[6] = {0, 1, 2, 2, 1, 3}, hi[6] = {4, 5, 6, 6, 5, 7};
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    unsigned long a, b;
    char msg[160];
    GLuint p = link(smooth_vs, smooth_fs), eb;
    GLint lp, lc;
    if (!p)
        return;
    check(ext && strstr(ext, "GL_OES_draw_elements_base_vertex") != 0,
          "GL_EXTENSIONS lists GL_OES_draw_elements_base_vertex");
    two_quads();
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 0, q_pos);
    glEnableVertexAttribArray(lp);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, q_col);
    glEnableVertexAttribArray(lc);
    glClearColor(0, 0, 0, 1);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsBaseVertexOES(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, lo, 4);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "indices 0-3 with base vertex 4: left %06lx empty, right %06lx green", a, b);
    check(a == 0 && b == 0x00FF00, msg);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsBaseVertexOES(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, hi, -4);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "indices 4-7 with base vertex -4: left %06lx red, right %06lx empty", a, b);
    check(a == 0xFF0000 && b == 0, msg);

    /* From an element buffer, at an offset; base vertex 0 is glDrawElements. */
    glGenBuffers(1, &eb);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(lo) + sizeof(hi), 0, GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(lo), lo);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, sizeof(lo), sizeof(hi), hi);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsBaseVertexOES(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)sizeof(lo), 0);
    glDrawElementsBaseVertexOES(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0, 0);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "element buffer, base vertex 0: left %06lx red, right %06lx green", a, b);
    check(a == 0xFF0000 && b == 0x00FF00, msg);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &eb);

    glDrawElementsBaseVertexOES(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, lo, 0);
    glDrawElementsBaseVertexOES(7 + 100, 6, GL_UNSIGNED_SHORT, lo, 0);
    check(glGetError() == GL_INVALID_ENUM, "a bad mode: GL_INVALID_ENUM");
    aglSwapBuffers();
    glDisableVertexAttribArray(lc);
    glUseProgram(0);
    glDeleteProgram(p);
}

/* GLES 3.0's glDrawRangeElements (ogles2 1.1, through aglGetProcAddress):
 * the indices are not scanned for their range; the result is
 * glDrawElements'. */
static void test_range(void)
{
    static const UWORD hi[6] = {4, 5, 6, 6, 5, 7};
    static const ULONG both[12] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    static const UBYTE lo8[6] = {0, 1, 2, 2, 1, 3};
    unsigned long a, b;
    char msg[160];
    GLuint p = link(smooth_vs, smooth_fs), eb;
    GLint lp, lc;
    if (!p)
        return;
    check(aglGetProcAddress("glDrawRangeElements") != 0, "aglGetProcAddress(\"glDrawRangeElements\") is found");
    two_quads();
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 0, q_pos);
    glEnableVertexAttribArray(lp);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, q_col);
    glEnableVertexAttribArray(lc);
    glClearColor(0, 0, 0, 1);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawRangeElements(GL_TRIANGLES, 4, 7, 6, GL_UNSIGNED_SHORT, hi);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "16-bit indices 4-7, range 4-7: left %06lx empty, right %06lx green", a, b);
    check(a == 0 && b == 0x00FF00, msg);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawRangeElements(GL_TRIANGLES, 0, 7, 12, GL_UNSIGNED_INT, both);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "32-bit indices 0-7, range 0-7: left %06lx red, right %06lx green", a, b);
    check(a == 0xFF0000 && b == 0x00FF00, msg);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawRangeElements(GL_TRIANGLES, 0, 3, 6, GL_UNSIGNED_BYTE, lo8);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "8-bit indices 0-3, range 0-3: left %06lx red, right %06lx empty", a, b);
    check(a == 0xFF0000 && b == 0, msg);

    /* A range wider than the indices draws the same. */
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawRangeElements(GL_TRIANGLES, 2, 7, 6, GL_UNSIGNED_SHORT, hi);
    aglSwapBuffers();
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "indices 4-7, range 2-7: right %06lx green", b);
    check(b == 0x00FF00, msg);

    /* From an element buffer, at an offset. */
    glGenBuffers(1, &eb);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(both), both, GL_STATIC_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawRangeElements(GL_TRIANGLES, 4, 7, 6, GL_UNSIGNED_INT, (void *)(6 * sizeof(ULONG)));
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "element buffer at an offset, range 4-7: left %06lx empty, right %06lx green", a, b);
    check(a == 0 && b == 0x00FF00, msg);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &eb);

    glDrawRangeElements(GL_TRIANGLES, 7, 4, 6, GL_UNSIGNED_SHORT, hi);
    check(glGetError() == GL_INVALID_VALUE, "end before start: GL_INVALID_VALUE");
    glDisableVertexAttribArray(lc);
    glUseProgram(0);
    glDeleteProgram(p);
}

static void test_minmax(void)
{
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    unsigned long a, b;
    char msg[200];
    GLint eq = 0;
    GLuint p = link(smooth_vs, smooth_fs);
    GLint lp, lc;
    if (!p)
        return;
    check(ext && strstr(ext, "GL_EXT_blend_minmax") != 0, "GL_EXTENSIONS lists GL_EXT_blend_minmax");
    two_quads();
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_mvp"), 1, GL_FALSE, identity);
    lp = glGetAttribLocation(p, "a_position");
    lc = glGetAttribLocation(p, "a_color");
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 0, q_pos);
    glEnableVertexAttribArray(lp);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, q_col);
    glEnableVertexAttribArray(lc);
    /* Grey 0x80 behind red (left) and green (right). MIN: (80,00,00) left,
     * (00,80,00) right; MAX: (ff,80,80), (80,ff,80). Factors ZERO, ZERO would
     * make both black if applied. */
    glClearColor(0.5f, 0.5f, 0.5f, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ZERO);
    glBlendEquation(GL_MIN_EXT);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq);
    check(eq == GL_MIN_EXT, "glBlendEquation(GL_MIN_EXT) is queried back");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "GL_MIN_EXT over grey, factors ZERO: left %06lx (800000), right %06lx (008000)", a, b);
    check((a == 0x800000 || a == 0x7F0000) && (b == 0x008000 || b == 0x007F00), msg);

    glBlendEquationSeparate(GL_MAX_EXT, GL_FUNC_ADD);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    aglSwapBuffers();
    a = ndc(-0.5f, 0.0f);
    b = ndc(0.5f, 0.0f);
    sprintf(msg, "GL_MAX_EXT over grey, factors ZERO: left %06lx (ff8080), right %06lx (80ff80)", a, b);
    check((a == 0xFF8080 || a == 0xFF7F7F) && (b == 0x80FF80 || b == 0x7FFF7F), msg);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    glDisableVertexAttribArray(lc);
    glUseProgram(0);
    glDeleteProgram(p);
}

int main(int argc, char **argv)
{
    ULONG err = 0, before, after;
    struct BitMap *bm;
    void *ctx;
    char msg[100];
    if (argc > 1)
        dir = argv[1];
    printf("ogles2 M2: glDrawElements, indirectly indexed uniforms\n");
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
    test_elements();
    test_skin();
    test_base_vertex();
    test_range();
    test_minmax();
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
