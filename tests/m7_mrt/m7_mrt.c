/*
 * M7 test: EXT_draw_buffers and NV_fbo_color_attachments
 * (docs/M7_EXTENSIONS_PLAN.md step 5): a fragment shader's gl_FragData[0..3]
 * drawn into four textures at once, each read back through a framebuffer
 * object of its own. The program is compiled on the Amiga (the package's
 * libs/v3dglslc). Nothing to watch.
 *   usage: m7_mrt
 *   api       the strings, the limits, glDrawBuffersEXT's state and errors,
 *             completeness with several colour attachments
 *   draw      four colours into four targets (100x70: tiles of 32x32), two
 *             and three targets (64x32, 32x32 tiles), with a depth buffer
 *   clear     a whole clear and a scissored one reach every draw buffer
 *   blend     blending in every target
 *   none      GL_NONE draw buffers keep their images
 *   default   the default framebuffer's GL_NONE clears nothing
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
static GLuint prog, rfb;
static GLint col_loc;
#define W 100
#define H 70

static const char *vs =
    "attribute vec4 a_pos;\n"
    "void main() {\n"
    "    gl_Position = a_pos;\n"
    "}\n";
static const char *fs =
    "#extension GL_EXT_draw_buffers : require\n"
    "precision mediump float;\n"
    "uniform vec4 u_col[4];\n"
    "void main() {\n"
    "    gl_FragData[0] = u_col[0];\n"
    "    gl_FragData[1] = u_col[1];\n"
    "    gl_FragData[2] = u_col[2];\n"
    "    gl_FragData[3] = u_col[3];\n"
    "}\n";

static const GLenum bufs[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1_EXT, GL_COLOR_ATTACHMENT2_EXT,
                               GL_COLOR_ATTACHMENT3_EXT};
static const GLfloat colours[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};

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
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    col_loc = glGetUniformLocation(p, "u_col");
    return p;
}

static GLuint new_texture(int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* A quad over the viewport at depth z, colours c[0..3]. */
static void quad(GLfloat z, const GLfloat c[4][4])
{
    GLfloat v[16];
    int i;
    for (i = 0; i < 4; i++) {
        v[i * 4 + 0] = (i & 1) ? 1.0f : -1.0f;
        v[i * 4 + 1] = (i & 2) ? 1.0f : -1.0f;
        v[i * 4 + 2] = z;
        v[i * 4 + 3] = 1.0f;
    }
    glUseProgram(prog);
    glUniform4fv(col_loc, 4, &c[0][0]);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Pixel (x, y) of texture t (w x h), through the read framebuffer. */
static void texel(GLuint t, int x, int y, GLubyte *px, GLuint back)
{
    glBindFramebuffer(GL_FRAMEBUFFER, rfb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, back);
}

static int is(const GLubyte *px, const GLfloat *c)
{
    int k;
    for (k = 0; k < 4; k++) {
        int want = (int)(c[k] * 255.0f + 0.5f), d = px[k] - want;
        if (d < -1 || d > 1)
            return 0;
    }
    return 1;
}

/* Every texture of n at the four corners and the middle: colour c[i]. */
static void expect(const char *what, GLuint fb, const GLuint *tex, int n, int w, int h, const GLfloat c[4][4])
{
    static const int xs[5] = {0, 1, 0, 1, 2}, ys[5] = {0, 0, 1, 1, 2}; /* 0 low, 1 high, 2 middle */
    char msg[200];
    int i, k, bad = 0;
    GLubyte px[4], first[4] = {0, 0, 0, 0};
    for (i = 0; i < n; i++)
        for (k = 0; k < 5; k++) {
            int x = xs[k] == 0 ? 0 : xs[k] == 1 ? w - 1 : w / 2;
            int y = ys[k] == 0 ? 0 : ys[k] == 1 ? h - 1 : h / 2;
            texel(tex[i], x, y, px, fb);
            if (!is(px, c[i]) && !bad++)
                memcpy(first, px, 4);
        }
    if (bad)
        sprintf(msg, "%s: %d pixel(s) wrong, the first %d %d %d %d", what, bad, first[0], first[1], first[2], first[3]);
    else
        sprintf(msg, "%s", what);
    check(!bad, msg);
}

static void test_api(void)
{
    const char *e = (const char *)glGetString(GL_EXTENSIONS);
    GLint v = 0, v2 = 0;
    GLuint fb, t[2];
    GLenum two[2] = {GL_COLOR_ATTACHMENT1_EXT, GL_COLOR_ATTACHMENT0};
    check(e && strstr(e, "GL_EXT_draw_buffers") && strstr(e, "GL_NV_fbo_color_attachments"),
          "GL_EXTENSIONS has GL_EXT_draw_buffers and GL_NV_fbo_color_attachments");
    glGetIntegerv(GL_MAX_DRAW_BUFFERS_EXT, &v);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS_EXT, &v2);
    check(v == 4 && v2 == 4, "GL_MAX_DRAW_BUFFERS_EXT and GL_MAX_COLOR_ATTACHMENTS_EXT are 4");
    glGetIntegerv(GL_DRAW_BUFFER0_EXT, &v);
    glGetIntegerv(GL_DRAW_BUFFER1_EXT, &v2);
    check(v == GL_BACK && v2 == GL_NONE, "the default framebuffer: GL_DRAW_BUFFER0 GL_BACK, GL_DRAW_BUFFER1 GL_NONE");
    glDrawBuffersEXT(2, bufs);
    check(glGetError() == GL_INVALID_OPERATION, "the default framebuffer takes one draw buffer");
    glDrawBuffersEXT(1, &bufs[0]);
    check(glGetError() == GL_INVALID_OPERATION, "the default framebuffer takes GL_BACK or GL_NONE only");

    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGetIntegerv(GL_DRAW_BUFFER0_EXT, &v);
    glGetIntegerv(GL_DRAW_BUFFER3_EXT, &v2);
    check(v == GL_COLOR_ATTACHMENT0 && v2 == GL_NONE,
          "a framebuffer object: GL_DRAW_BUFFER0 COLOR_ATTACHMENT0, GL_DRAW_BUFFER3 GL_NONE");
    glDrawBuffersEXT(5, bufs);
    check(glGetError() == GL_INVALID_VALUE, "more than 4 draw buffers: GL_INVALID_VALUE");
    glDrawBuffersEXT(2, two);
    check(glGetError() == GL_INVALID_OPERATION, "COLOR_ATTACHMENT1 in position 0: GL_INVALID_OPERATION");
    two[0] = GL_BACK;
    glDrawBuffersEXT(1, two);
    check(glGetError() == GL_INVALID_OPERATION, "GL_BACK in an object: GL_INVALID_OPERATION");
    two[0] = GL_TEXTURE_2D;
    glDrawBuffersEXT(1, two);
    check(glGetError() == GL_INVALID_ENUM, "a value that names no buffer: GL_INVALID_ENUM");
    glDrawBuffersEXT(4, bufs);
    glGetIntegerv(GL_DRAW_BUFFER3_EXT, &v);
    check(glGetError() == GL_NO_ERROR && v == (GLint)GL_COLOR_ATTACHMENT3_EXT, "four draw buffers read back");

    t[0] = new_texture(16, 16);
    t[1] = new_texture(16, 8);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3_EXT, GL_TEXTURE_2D, t[1], 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS,
          "colour attachments of two sizes: GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS");
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3_EXT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
    check(v == (GLint)t[1], "COLOR_ATTACHMENT3's object reads back");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(2, t);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
}

/* n targets, w x h, with a depth buffer: two quads, the nearer in front. */
static void test_draw(int n, int w, int h, const char *name)
{
    static const GLfloat far_c[4][4] = {{0.2f, 0.2f, 0.2f, 1}, {0.4f, 0.4f, 0.4f, 1}, {0.6f, 0.6f, 0.6f, 1},
                                        {0.8f, 0.8f, 0.8f, 1}};
    GLuint fb, t[4], depth;
    int i;
    char what[120];
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    for (i = 0; i < n; i++) {
        t[i] = new_texture(w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, t[i], 0);
    }
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glDrawBuffersEXT(n, bufs);
    sprintf(what, "%s: complete", name);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, what);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    quad(-0.5f, colours); /* near */
    quad(0.5f, far_c);    /* far: hidden */
    glDisable(GL_DEPTH_TEST);
    sprintf(what, "%s: each target its own colour, the nearer quad in front", name);
    expect(what, fb, t, n, w, h, colours);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &depth);
    glDeleteTextures(n, t);
}

static void test_four(void)
{
    static const GLfloat red[4][4] = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
    static const GLfloat grey[4][4] = {{0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f},
                                       {0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f}};
    GLfloat mixed[4][4], sum[4][4];
    GLuint fb, t[4];
    GLubyte px[4];
    int i, k, bad;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    for (i = 0; i < 4; i++) {
        t[i] = new_texture(W, H);
        glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, t[i], 0);
    }
    glDrawBuffersEXT(4, bufs);
    glViewport(0, 0, W, H);

    /* A whole clear, before any draw: the tile buffer's, every target. */
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    expect("four targets: a whole clear reaches every draw buffer", fb, t, 4, W, H, red);

    /* After a draw: drawn as a rectangle into every target. */
    quad(0, colours);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, W / 2, H);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    bad = 0;
    for (i = 0; i < 4; i++) {
        texel(t[i], 2, H / 2, px, fb);
        bad += !is(px, grey[i]);
        texel(t[i], W - 3, H / 2, px, fb);
        bad += !is(px, colours[i]);
    }
    check(!bad, "four targets: a scissored clear after a draw reaches every draw buffer, its half only");

    /* Blending, ONE + ONE, in every target. */
    for (i = 0; i < 4; i++)
        for (k = 0; k < 4; k++)
            mixed[i][k] = 0.25f;
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    quad(0, colours);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    quad(0, mixed);
    glDisable(GL_BLEND);
    for (i = 0; i < 4; i++)
        for (k = 0; k < 4; k++)
            sum[i][k] = colours[i][k] + 0.25f > 1.0f ? 1.0f : colours[i][k] + 0.25f;
    expect("four targets: blending in each", fb, t, 4, W, H, sum);

    /* GL_NONE for outputs 1 and 3: those images are kept. */
    {
        GLenum some[4] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2_EXT, GL_NONE};
        GLfloat want[4][4];
        glDrawBuffersEXT(4, some);
        quad(0, grey);
        for (i = 0; i < 4; i++)
            for (k = 0; k < 4; k++)
                want[i][k] = (i & 1) ? sum[i][k] : grey[i][k];
        expect("GL_NONE draw buffers: outputs 0 and 2 drawn, 1 and 3 kept", fb, t, 4, W, H, want);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(4, t);
}

static void test_default(int w, int h)
{
    GLenum none = GL_NONE, back = GL_BACK;
    GLubyte px[4];
    glViewport(0, 0, w, h);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawBuffersEXT(1, &none);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(px[0] == 0 && px[1] == 255, "the default framebuffer with GL_NONE: a clear writes nothing");
    glDrawBuffersEXT(1, &back);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(px[0] == 255 && px[1] == 0 && glGetError() == GL_NO_ERROR, "and with GL_BACK again it clears");
}

int main(void)
{
    struct BitMap *bm;
    void *ctx;
    ULONG e = 0;
    printf("ogles2 M7: EXT_draw_buffers (several render targets)\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        printf("needs graphics 39 and %s\n", OGLES2NAME);
        return 20;
    }
    /* Every way out closes what was opened: a library left open cannot be
     * expunged, and the next build would not load until a reboot. */
    bm = AllocBitMap(64, 64, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm || !(ctx = aglCreateContextTags2(&e, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE))) {
        printf("FAIL: no context (error %lu)\n", (unsigned long)e);
        if (bm)
            FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    aglMakeCurrent(ctx);
    test_api();
    if (!(prog = link())) {
        printf("FAIL: the program did not link\n");
        aglDestroyContext(ctx);
        FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    glGenFramebuffers(1, &rfb);
    test_draw(4, W, H, "four targets 100x70");
    test_draw(3, W, H, "three targets 100x70");
    test_draw(2, W, H, "two targets 100x70");
    test_draw(2, 300, 200, "two targets 300x200");
    test_four();
    test_default(64, 64);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");

    glDeleteFramebuffers(1, &rfb);
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
