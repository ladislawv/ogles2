/*
 * M7 test: how wide V3D draws a point of a given gl_PointSize (dEQP-GLES2
 * rasterization.limits.points). Red points on black, each centred in a
 * 1200x8 framebuffer object; the red run of the middle row is measured.
 * Nothing to watch.
 *   usage: m7_points
 *   sizes     1, 8, 64, 256, 511, 512, 513, 528, 600: the width drawn, and
 *             whether it is the size asked, clamped to GL_ALIASED_POINT_SIZE_RANGE
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

struct GfxBase *GfxBase;
static int failures, checks;
#define FW 1200
#define FH 8

/* dEQP-GLES2's rasterization shaders (es2fRasterizationTests.cpp). */
static const char *vs =
    "attribute highp vec4 a_position;\n"
    "attribute highp vec4 a_color;\n"
    "varying highp vec4 v_color;\n"
    "uniform highp float u_size;\n"
    "void main (void) {\n"
    "    gl_Position = a_position;\n"
    "    gl_PointSize = u_size;\n"
    "    v_color = a_color;\n"
    "}\n";
static const char *fs =
    "varying mediump vec4 v_color;\n"
    "void main (void) {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static GLuint link(void)
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
    glBindAttribLocation(p, 1, "a_color");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* The red run through the middle row: its first and last x (-1: none). */
static void red_run(int *first, int *last)
{
    GLubyte *row = malloc(FW * 4);
    int x;
    *first = *last = -1;
    if (!row)
        return;
    glReadPixels(0, FH / 2, FW, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
    for (x = 0; x < FW; x++)
        if (row[x * 4] > 128) {
            if (*first < 0)
                *first = x;
            *last = x;
        }
    free(row);
}

int main(void)
{
    static const float sizes[] = {1, 8, 64, 256, 511, 512, 513, 528, 600};
    static const GLfloat centre[4] = {0, 0, 0, 1}, red_colour[4] = {1, 0, 0, 1};
    struct BitMap *bm;
    void *ctx;
    ULONG err = 0;
    GLuint p, fb, rb;
    GLfloat range[2] = {0, 0};
    char msg[160];
    unsigned i;
    printf("ogles2 M7: point sizes as drawn\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        printf("needs graphics 39 and %s\n", OGLES2NAME);
        return 20;
    }
    bm = AllocBitMap(64, 64, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE))) {
        printf("FAIL: no context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    if (!(p = link())) {
        printf("FAIL: the point program did not link\n");
        return 20;
    }
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, range);
    printf("GL_ALIASED_POINT_SIZE_RANGE [%g, %g]\n", range[0], range[1]);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, FW, FH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "a 1200x8 framebuffer object");
    glViewport(0, 0, FW, FH);
    glUseProgram(p);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, centre);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, red_colour);
    glEnableVertexAttribArray(1);
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int first, last, want = (int)(sizes[i] < range[1] ? sizes[i] : range[1]);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(glGetUniformLocation(p, "u_size"), sizes[i]);
        glDrawArrays(GL_POINTS, 0, 1);
        red_run(&first, &last);
        sprintf(msg, "size %g: drawn x %d..%d, %d wide (%d)", sizes[i], first, last,
                first < 0 ? 0 : last - first + 1, want);
        check(first >= 0 && last - first + 1 == want, msg);
    }
    glDeleteRenderbuffers(1, &rb);
    glDeleteFramebuffers(1, &fb);

    /* dEQP's case: a 264x4 framebuffer, a point of 528 (clamped to 512)
     * centred on its right edge (x = 1 in clip space), so x 8..263 red and
     * 0..7 untouched. */
    {
        static const GLfloat edge[4] = {1, 0, 0, 1};
        GLubyte px[264 * 4 * 4];
        int y, first[4];
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, 264, 4);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        glViewport(0, 0, 264, 4);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, edge);
        glUniform1f(glGetUniformLocation(p, "u_size"), 528.0f);
        glDrawArrays(GL_POINTS, 0, 1);
        glReadPixels(0, 0, 264, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (y = 0; y < 4; y++) {
            int x;
            first[y] = -1;
            for (x = 0; x < 264 && first[y] < 0; x++)
                if (px[(y * 264 + x) * 4] > 128)
                    first[y] = x;
        }
        sprintf(msg, "264x4, size 528 centred on the right edge: red from x %d %d %d %d (8)", first[0], first[1],
                first[2], first[3]);
        check(first[0] == 8 && first[1] == 8 && first[2] == 8 && first[3] == 8, msg);
        /* dEQP reads only the 4x4 box at the left: green, as the whole rows said. */
        {
            GLubyte box[4 * 4 * 4];
            int k, green = 0;
            memset(box, 0x55, sizeof(box));
            glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, box);
            for (k = 0; k < 16; k++)
                if (box[k * 4] < 12 && box[k * 4 + 1] > 243 && box[k * 4 + 2] < 12)
                    green++;
            sprintf(msg, "the 4x4 box at (0, 0) read alone: %d of 16 green; first %d,%d,%d,%d", green, box[0], box[1],
                    box[2], box[3]);
            check(green == 16, msg);
        }
        glDeleteRenderbuffers(1, &rb);
        glDeleteFramebuffers(1, &fb);
    }
    glDeleteProgram(p);
    aglDestroyContext(ctx);
    FreeBitMap(bm);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    OGLES2Close();
    CloseLibrary((struct Library *)GfxBase);
    return failures ? 10 : 0;
}
