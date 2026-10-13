/*
 * M7 test: EXT_clip_control (docs/M7_EXTENSIONS_PLAN.md step 4), first
 * the probe its design rested on: whether V3D 4.2's window-space Z planes
 * (CLIPPER_Z_MIN_MAX_CLIPPING_PLANES) clip what lies outside the depth
 * range or only clamp it. With the 0..w mapping in the viewport alone
 * (design A), a triangle part with clip-space z < 0 was drawn at depth n
 * (A1200 run ogles2-m7-3-20261013-222635): the planes clamp. So the program
 * clips it itself (design B, Mesa patch 0013: z becomes 2z - w), which a
 * program compiled before that patch does not, and the probe says so.
 * Drawn into 32x32 RGBA8 framebuffer objects and a 32x32 bitmap context;
 * each fragment writes its window depth (gl_FragCoord.z) as red. Nothing to
 * watch.
 *   usage: m7_clip
 *   state     the queries, the calls, the errors
 *   probe     ZERO_TO_ONE: a quad from z -1 (left) to 1 (right), with and
 *             without a depth test
 *   depth     the depth each mode gives, against the specification
 *   origin    UPPER_LEFT turns the picture over, in an object and in the
 *             default framebuffer; which faces are front does not change
 *   near      a quad cut by the near plane: the part the clipper makes is
 *             where the shader puts the rest (both framebuffers, both origins)
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
#include <GLES2/gl2ext.h>

struct GfxBase *GfxBase;
static int failures, checks;
static GLuint prog;
#define N 32

static const char *vs =
    "attribute vec4 a_pos;\n"
    "void main() {\n"
    "    gl_Position = a_pos;\n"
    "}\n";
static const char *fs =
    "precision highp float;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(gl_FragCoord.z, 1.0, 0.0, 1.0);\n"
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
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* A quad of 4 vertices (x, y, z, w each), as a strip, onto a black clear. */
static void quad(const GLfloat *v, GLubyte *px)
{
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

#define DRAWN(px, x, y) ((px)[((y) * N + (x)) * 4 + 1] > 128) /* green: a fragment was written */
#define RED(px, x, y) ((px)[((y) * N + (x)) * 4])

static void test_state(void)
{
    GLint o = 0, d = 0;
    glGetIntegerv(GL_CLIP_ORIGIN_EXT, &o);
    glGetIntegerv(GL_CLIP_DEPTH_MODE_EXT, &d);
    check(o == GL_LOWER_LEFT_EXT && d == GL_NEGATIVE_ONE_TO_ONE_EXT, "initially LOWER_LEFT, NEGATIVE_ONE_TO_ONE");
    glClipControlEXT(GL_UPPER_LEFT_EXT, GL_ZERO_TO_ONE_EXT);
    glGetIntegerv(GL_CLIP_ORIGIN_EXT, &o);
    glGetIntegerv(GL_CLIP_DEPTH_MODE_EXT, &d);
    check(o == GL_UPPER_LEFT_EXT && d == GL_ZERO_TO_ONE_EXT && glGetError() == GL_NO_ERROR,
          "glClipControlEXT(UPPER_LEFT, ZERO_TO_ONE) reads back");
    glClipControlEXT(GL_ZERO_TO_ONE_EXT, GL_ZERO_TO_ONE_EXT);
    check(glGetError() == GL_INVALID_ENUM, "a wrong origin is GL_INVALID_ENUM");
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_LOWER_LEFT_EXT);
    check(glGetError() == GL_INVALID_ENUM, "a wrong depth mode is GL_INVALID_ENUM");
    glGetIntegerv(GL_CLIP_ORIGIN_EXT, &o);
    check(o == GL_UPPER_LEFT_EXT, "an error leaves the state as it was");
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
}

/* The probe: in ZERO_TO_ONE mode, x -1..1 carries z -1..1 (w 1). The
 * pixel column x has z = (x + 0.5) / 16 - 1. */
static void test_probe(int depth_test)
{
    static const GLfloat v[16] = {-1, -1, -1, 1, 1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1};
    GLubyte px[N * N * 4];
    char msg[200];
    int first = -1, x;
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
    }
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_ZERO_TO_ONE_EXT);
    quad(v, px);
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
    glDisable(GL_DEPTH_TEST);
    for (x = 0; x < N; x++)
        if (DRAWN(px, x, N / 2)) {
            first = x;
            break;
        }
    /* clipped: the first column drawn is 16 (z 1/32); clamped: 0, at depth 0 */
    sprintf(msg, "probe, depth test %s: first column drawn %d (16 if z < 0 is clipped); its depth %d/255; "
                 "column 0 %s, depth %d/255; column 31 depth %d/255 (%d)",
            depth_test ? "on (ALWAYS)" : "off", first, first >= 0 ? RED(px, first, N / 2) : -1,
            DRAWN(px, 0, N / 2) ? "drawn" : "not drawn", RED(px, 0, N / 2), RED(px, N - 1, N / 2),
            (int)((31.5 / 16 - 1) * 255 + 0.5));
    printf("%s: %s\n", first == 16 ? "PROBE" : first == 0 ? "PROBE (clamped)" : "PROBE (?)", msg);
    check(first == 16, "ZERO_TO_ONE: what has clip-space z < 0 is clipped");
}

/* The depth of each mode at a few columns, against the specification:
 * NEGATIVE_ONE_TO_ONE (z + 1) / 2, ZERO_TO_ONE z (z >= 0), with a depth
 * range of 0.25..0.75 too. */
static void test_depth(void)
{
    static const GLfloat v[16] = {-1, -1, -1, 1, 1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1};
    static const int cols[4] = {1, 12, 20, 30};
    GLubyte px[N * N * 4];
    char msg[200];
    int mode, range, k;
    for (mode = 0; mode < 2; mode++)
        for (range = 0; range < 2; range++) {
            float n = range ? 0.25f : 0.0f, f = range ? 0.75f : 1.0f;
            int bad = 0, worst = 0;
            glDepthRangef(n, f);
            glClipControlEXT(GL_LOWER_LEFT_EXT, mode ? GL_ZERO_TO_ONE_EXT : GL_NEGATIVE_ONE_TO_ONE_EXT);
            quad(v, px);
            for (k = 0; k < 4; k++) {
                float z = (cols[k] + 0.5f) / 16 - 1, d;
                int want, got = RED(px, cols[k], N / 2), diff;
                if (mode && z < 0)
                    continue; /* clipped: the probe's business */
                d = mode ? n + (f - n) * z : n + (f - n) * (z + 1) / 2;
                want = (int)(d * 255 + 0.5f);
                diff = got > want ? got - want : want - got;
                if (diff > worst)
                    worst = diff;
                if (diff > 1 || !DRAWN(px, cols[k], N / 2))
                    bad++;
            }
            sprintf(msg, "%s, depth range %s: window depth as specified (largest difference %d/255)",
                    mode ? "ZERO_TO_ONE" : "NEGATIVE_ONE_TO_ONE", range ? "0.25..0.75" : "0..1", worst);
            check(!bad, msg);
        }
    glDepthRangef(0, 1);
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
}

/* The lower left quarter of clip space: drawn in the lower left of the
 * framebuffer with LOWER_LEFT, its upper left with UPPER_LEFT. Then a
 * triangle counter-clockwise in clip space, with back faces culled: drawn
 * with either origin (dEQP clip_control.face_culling: the facing must not
 * depend on the origin, though the window winding turns over). */
static void test_origin(const char *fb)
{
    static const GLfloat q[16] = {-1, -1, 0, 1, 0, -1, 0, 1, -1, 0, 0, 1, 0, 0, 0, 1};
    static const GLfloat ccw[16] = {-1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1}; /* a strip: one triangle */
    GLubyte px[N * N * 4];
    char msg[160];
    int o;
    for (o = 0; o < 2; o++) {
        int low, high;
        glClipControlEXT(o ? GL_UPPER_LEFT_EXT : GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
        quad(q, px);
        low = DRAWN(px, 4, 4);
        high = DRAWN(px, 4, N - 5);
        sprintf(msg, "%s, %s: the lower left quarter drawn at the %s left (lower %d, upper %d)", fb,
                o ? "UPPER_LEFT" : "LOWER_LEFT", o ? "upper" : "lower", low, high);
        check(o ? high && !low : low && !high, msg);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        quad(ccw, px);
        glDisable(GL_CULL_FACE);
        sprintf(msg, "%s, %s: a counter-clockwise triangle with back faces culled is drawn", fb,
                o ? "UPPER_LEFT" : "LOWER_LEFT");
        check(DRAWN(px, N - 3, o ? N / 2 - 1 : N / 2), msg);
    }
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
}

/* A full quad whose top edge lies behind the near plane: z 0 at the bottom
 * (y -1), -3 at the top (y 1), so z = -1 at y = -1/3. The clipper cuts it
 * there; what it makes must stay in the lower third (rows 0..10 of 32;
 * the upper third of the framebuffer with UPPER_LEFT). */
static void test_near(const char *fb)
{
    static const GLfloat v[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, -3, 1, 1, 1, -3, 1};
    GLubyte px[N * N * 4];
    char msg[160];
    int o, y, lowest, highest;
    for (o = 0; o < 2; o++) {
        glClipControlEXT(o ? GL_UPPER_LEFT_EXT : GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
        quad(v, px);
        lowest = highest = -1;
        for (y = 0; y < N; y++)
            if (DRAWN(px, N / 2, y)) {
                if (lowest < 0)
                    lowest = y;
                highest = y;
            }
        /* rows whose centre has y < -1/3: (row + 0.5) / 16 - 1 < -1/3, rows 0..10 */
        sprintf(msg, "%s, %s: a quad cut by the near plane covers rows %d..%d (%s)", fb,
                o ? "UPPER_LEFT" : "LOWER_LEFT", lowest, highest, o ? "21..31" : "0..10");
        check(o ? lowest == 21 && highest == 31 : lowest == 0 && highest == 10, msg);
    }
    glClipControlEXT(GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT);
}

int main(void)
{
    struct BitMap *bm;
    void *ctx;
    ULONG err = 0;
    GLuint fb, rb[2];
    const char *ext;
    printf("ogles2 M7: EXT_clip_control, and the Z planes probe\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        printf("needs graphics 39 and %s\n", OGLES2NAME);
        return 20;
    }
    /* Every way out closes what was opened: a library left open cannot be
     * expunged, and the next build would not load until a reboot. */
    bm = AllocBitMap(N, N, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 16, TAG_DONE))) {
        printf("FAIL: no context (error %lu)\n", (unsigned long)err);
        if (bm)
            FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    aglMakeCurrent(ctx);
    ext = (const char *)glGetString(GL_EXTENSIONS);
    check(ext && strstr(ext, "GL_EXT_clip_control") != 0, "GL_EXTENSIONS has GL_EXT_clip_control");
    if (!(prog = link())) {
        printf("FAIL: the program did not link\n");
        aglDestroyContext(ctx);
        FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, N, N);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, N, N);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "a 32x32 RGBA8 + depth object");
    glViewport(0, 0, N, N);

    test_state();
    test_probe(0);
    test_probe(1);
    test_depth();
    test_origin("object");
    test_near("object");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    test_origin("default framebuffer");
    test_near("default framebuffer");
    check(glGetError() == GL_NO_ERROR, "no GL error left over");

    glDeleteRenderbuffers(2, rb);
    glDeleteFramebuffers(1, &fb);
    glDeleteProgram(prog);
    aglDestroyContext(ctx);
    FreeBitMap(bm);
    OGLES2Close();
    CloseLibrary((struct Library *)GfxBase);
    if (failures)
        printf("FAILED: %d of %d checks passed\n", checks - failures, checks);
    else
        printf("ALL PASSED: %d of %d checks passed\n", checks, checks);
    return failures ? 10 : 0;
}
