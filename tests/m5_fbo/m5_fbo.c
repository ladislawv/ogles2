/*
 * M5 test: framebuffer and renderbuffer objects (plan 7.7), on a hidden
 * 256x128 bitmap with D24S8. Nothing to watch.
 *   usage: m5_fbo
 *   objects   names, binding, status without attachments, the queries
 *   rtt       render to an RGBA8 texture: its orientation read back
 *             (glReadPixels from the object) and sampled on the screen
 *   midframe  an object's pass in the middle of a screen frame: the
 *             screen's colour and depth carry on after it
 *   depth     D16 renderbuffer; depth kept from one job to the next
 *   stencil   D24S8 for both, and D16 + S8 apart
 *   colourrb  RGB565, RGBA4, RGB5_A1 and RGBA8_OES renderbuffers
 *   levels    level 1 (OES_fbo_render_mipmap), a cube face, glGenerateMipmap
 *             and glTexSubImage2D after rendering, glCopyTexImage2D from it
 *   big       512x512: tile pools of its own
 *   errors    incomplete attachments and sizes, GL_INVALID_FRAMEBUFFER_OPERATION
 *   churn     objects made, drawn into and deleted 40 times
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
#include "../common/tex_shaders.h"

#ifndef GL_RGBA8_OES
#define GL_RGB8_OES 0x8051
#define GL_RGBA8_OES 0x8058
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
static int failures, checks;
static struct RastPort rp;
static GLuint smooth, tex1;
static GLint lp, lc, tp, tt;
#define W 256
#define H 128

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

/* The screen at NDC (x, y). */
static unsigned long screen(float x, float y)
{
    int px = (int)((x + 1.0f) * 0.5f * W), py = (int)((y + 1.0f) * 0.5f * H);
    if (px >= W) px = W - 1;
    if (py >= H) py = H - 1;
    return ReadRGBPixel(&rp, (UWORD)px, (UWORD)(H - 1 - py)) & 0xFFFFFF;
}

/* Each channel of c within tol of grey 0x80 (16-bit formats round either way). */
static int greyish(unsigned long c, int tol)
{
    int k;
    for (k = 0; k < 3; k++) {
        int v = (int)((c >> (8 * k)) & 255);
        if (v < 128 - tol || v > 128 + tol)
            return 0;
    }
    return 1;
}

/* The bound framebuffer at (x, y), GL's coordinates. */
static unsigned long px(int x, int y)
{
    GLubyte p[4] = {1, 2, 3, 4};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
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

/* A rectangle x0..x1, y0..y1 (NDC) at depth z, one colour (smooth program). */
static void rect(float x0, float x1, float y0, float y1, float z, float r, float g, float b)
{
    GLfloat v[12];
    v[0] = x0; v[1] = y0; v[2] = z;
    v[3] = x1; v[4] = y0; v[5] = z;
    v[6] = x0; v[7] = y1; v[8] = z;
    v[9] = x1; v[10] = y1; v[11] = z;
    glUseProgram(smooth);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, r, g, b, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The whole viewport textured with texture t (tex1 program). */
static void textured(GLuint t)
{
    static const GLfloat v[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1};
    glUseProgram(tex1);
    glBindTexture(GL_TEXTURE_2D, t);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, v + 3);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(1);
}

static GLuint texture(int w, int h, GLenum filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* A framebuffer object with texture t (level l) as its colour. */
static GLuint fbo_tex(GLuint t, GLint l)
{
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, l);
    return f;
}

static GLuint renderbuffer(GLenum format, int w, int h)
{
    GLuint r;
    glGenRenderbuffers(1, &r);
    glBindRenderbuffer(GL_RENDERBUFFER, r);
    glRenderbufferStorage(GL_RENDERBUFFER, format, w, h);
    return r;
}

static void default_fb(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
}

static void test_objects(void)
{
    GLuint f;
    GLint v = -1;
    char msg[160];
    glGenFramebuffers(1, &f);
    check(f != 0 && !glIsFramebuffer(f), "glGenFramebuffers: a name, not a framebuffer until bound");
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
    check(glIsFramebuffer(f) && v == (GLint)f, "bound: glIsFramebuffer, GL_FRAMEBUFFER_BINDING");
    sprintf(msg, "no attachments: status 0x%04x (8cd7 INCOMPLETE_MISSING_ATTACHMENT)",
            glCheckFramebufferStatus(GL_FRAMEBUFFER));
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT, msg);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &v);
    check(v == GL_NONE, "attachment type GL_NONE");
    glClear(GL_COLOR_BUFFER_BIT);
    check(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION, "glClear on it: GL_INVALID_FRAMEBUFFER_OPERATION");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &v);
    check(glGetError() == GL_INVALID_OPERATION, "the default framebuffer's attachments: GL_INVALID_OPERATION");
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "the default framebuffer: complete");
    glDeleteFramebuffers(1, &f);
    check(!glIsFramebuffer(f), "deleted");
}

static void test_rtt(void)
{
    GLuint t = texture(64, 32, GL_NEAREST), f = fbo_tex(t, 0);
    GLint v[6];
    unsigned long a, b, c, d;
    char msg[200];
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 texture 64x32: complete");
    glGetIntegerv(GL_RED_BITS, &v[0]);
    glGetIntegerv(GL_ALPHA_BITS, &v[1]);
    glGetIntegerv(GL_DEPTH_BITS, &v[2]);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &v[3]);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v[4]);
    sprintf(msg, "bits r%d a%d d%d (8 8 0), attachment type 0x%04x name %d", v[0], v[1], v[2], v[3], v[4]);
    check(v[0] == 8 && v[1] == 8 && v[2] == 0 && v[3] == GL_TEXTURE && v[4] == (GLint)t, msg);
    glViewport(0, 0, 64, 32);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(-1, 1, -1, 0, 0, 1, 0, 0);   /* the lower half red */
    rect(-1, -0.5f, -1, 1, 0, 0, 1, 0); /* the left quarter green */
    a = px(40, 4);
    b = px(40, 28);
    c = px(4, 28);
    sprintf(msg, "read from the object: (40,4) %06lx red, (40,28) %06lx blue, (4,28) %06lx green", a, b, c);
    check(a == 0xFF0000 && b == 0x0000FF && c == 0x00FF00, msg);
    default_fb();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    textured(t);
    aglSwapBuffers();
    a = screen(0.5f, -0.5f);
    b = screen(0.5f, 0.5f);
    c = screen(-0.9f, 0.5f);
    d = screen(-0.9f, -0.5f);
    sprintf(msg, "sampled on the screen: bottom %06lx red, top %06lx blue, left %06lx %06lx green",
            a, b, c, d);
    check(a == 0xFF0000 && b == 0x0000FF && c == 0x00FF00 && d == 0x00FF00, msg);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);
}

static void test_midframe(void)
{
    GLuint t = texture(32, 32, GL_NEAREST), f;
    unsigned long a, b;
    char msg[160];
    default_fb();
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    rect(-1, 0, -1, 1, -0.5f, 0, 1, 0);   /* screen: near green, left half */
    f = fbo_tex(t, 0);                    /* an object's pass in between */
    glViewport(0, 0, 32, 32);
    glDisable(GL_DEPTH_TEST);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(-1, 1, -1, 1, 0, 1, 1, 0);
    default_fb();
    glEnable(GL_DEPTH_TEST);
    rect(-1, 1, -1, 1, 0.5f, 1, 0, 0);    /* far red over everything: hidden on the left */
    glDisable(GL_DEPTH_TEST);
    aglSwapBuffers();
    a = screen(-0.5f, 0);
    b = screen(0.5f, 0);
    sprintf(msg, "an object's pass mid-frame: screen left %06lx (00ff00, depth kept), right %06lx (ff0000)",
            a, b);
    check(a == 0x00FF00 && b == 0xFF0000, msg);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    check(px(5, 5) == 0xFFFF00, "and the object holds its yellow");
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);
}

static void test_depth(void)
{
    GLuint t = texture(64, 32, GL_NEAREST), f = fbo_tex(t, 0), d = renderbuffer(GL_DEPTH_COMPONENT16, 64, 32);
    GLint bits = 0;
    unsigned long a, b;
    char msg[160];
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d);
    glGetIntegerv(GL_DEPTH_BITS, &bits);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && bits == 16,
          "texture + DEPTH_COMPONENT16 renderbuffer: complete, 16 depth bits");
    glViewport(0, 0, 64, 32);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    rect(-1, 0, -1, 1, -0.5f, 1, 0, 0);   /* near red, left */
    default_fb();                         /* the object's job is rendered when the screen draws */
    glClear(GL_COLOR_BUFFER_BIT);
    rect(-1, 1, -1, 1, 0, 0, 0, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, f); /* a new job: depth loaded */
    glViewport(0, 0, 64, 32);
    rect(-1, 1, -1, 1, 0.5f, 0, 1, 0);    /* far green: hidden on the left */
    glDisable(GL_DEPTH_TEST);
    a = px(10, 16);
    b = px(50, 16);
    sprintf(msg, "depth kept across the object's jobs: left %06lx (ff0000), right %06lx (00ff00)", a, b);
    check(a == 0xFF0000 && b == 0x00FF00, msg);
    {
        /* EXT_discard_framebuffer: depth discarded, colour still stored. */
        static const GLenum dep = GL_DEPTH_ATTACHMENT, bad = 0x1801; /* GL_DEPTH_EXT: the default's name */
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        rect(-1, 1, -1, 1, 0, 1, 1, 0);   /* yellow everywhere (depth test off) */
        glDiscardFramebufferEXT(GL_FRAMEBUFFER, 1, &dep);
        check(glGetError() == GL_NO_ERROR && ext && strstr(ext, "GL_EXT_discard_framebuffer"),
              "glDiscardFramebufferEXT(GL_DEPTH_ATTACHMENT): in GL_EXTENSIONS, no error");
        a = px(10, 16);
        sprintf(msg, "the colour is still stored: %06lx (ffff00)", a);
        check(a == 0xFFFF00, msg);
        glDiscardFramebufferEXT(GL_FRAMEBUFFER, 1, &bad);
        check(glGetError() == GL_INVALID_ENUM, "GL_DEPTH_EXT on an object: GL_INVALID_ENUM");
    }
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteRenderbuffers(1, &d);
    glDeleteTextures(1, &t);
}

static void stencil_case(GLuint f, const char *what)
{
    unsigned long a, b;
    char msg[200];
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glViewport(0, 0, 64, 32);
    glClearColor(0, 0, 0, 1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    rect(-1, 0, -1, 1, 0, 1, 1, 1);       /* mark the left half */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();                           /* a new job: stencil loaded */
    glStencilFunc(GL_EQUAL, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    rect(-1, 1, -1, 1, 0, 0, 1, 0);
    glDisable(GL_STENCIL_TEST);
    a = px(10, 16);
    b = px(50, 16);
    sprintf(msg, "%s: stencil across glFinish: left %06lx (00ff00), right %06lx (000000)", what, a, b);
    check(a == 0x00FF00 && b == 0, msg);
}

static void test_stencil(void)
{
    GLuint t = texture(64, 32, GL_NEAREST), f = fbo_tex(t, 0), ds = renderbuffer(GL_DEPTH24_STENCIL8_OES, 64, 32);
    GLuint d, s;
    GLint bits[2] = {0, 0};
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ds);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
    glGetIntegerv(GL_DEPTH_BITS, &bits[0]);
    glGetIntegerv(GL_STENCIL_BITS, &bits[1]);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && bits[0] == 24 && bits[1] == 8,
          "DEPTH24_STENCIL8 as depth and stencil: complete, 24 and 8 bits");
    stencil_case(f, "D24S8");
    d = renderbuffer(GL_DEPTH_COMPONENT16, 64, 32);
    s = renderbuffer(GL_STENCIL_INDEX8, 64, 32);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "D16 + S8 apart: complete");
    stencil_case(f, "D16 + S8");
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteRenderbuffers(1, &ds);
    glDeleteRenderbuffers(1, &d);
    glDeleteRenderbuffers(1, &s);
    glDeleteTextures(1, &t);
}

static void test_colourrb(void)
{
    static const GLenum fmt[4] = {GL_RGB565, GL_RGBA4, GL_RGB5_A1, GL_RGBA8_OES};
    static const char *name[4] = {"RGB565", "RGBA4", "RGB5_A1", "RGBA8_OES"};
    int k;
    for (k = 0; k < 4; k++) {
        GLuint f, r = renderbuffer(fmt[k], 40, 20);
        GLint v[3] = {0, 0, 0};
        unsigned long a, b;
        char msg[200];
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &v[0]);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &v[1]);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_GREEN_SIZE, &v[2]);
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, r);
        glViewport(0, 0, 40, 20);
        glClearColor(0.5f, 0.5f, 0.5f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        rect(-1, 0, -1, 1, 0, 1, 0, 0);
        a = px(5, 5);
        b = px(35, 15);
        sprintf(msg, "%s renderbuffer (width %d, format 0x%04x, green %d bits): red %06lx, grey %06lx",
                name[k], v[0], v[1], v[2], a, b);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && v[0] == 40 &&
                  v[1] == (GLint)fmt[k] && a == 0xFF0000 && greyish(b, 10), msg);
        default_fb();
        glDeleteFramebuffers(1, &f);
        glDeleteRenderbuffers(1, &r);
    }
}

static void test_levels(void)
{
    GLuint t, f, c, t2;
    GLubyte *img = calloc(64 * 64, 4);
    GLint v = -1;
    unsigned long a, b;
    char msg[200];
    int k;
    /* Level 1 of a mipmapped texture. */
    t = texture(64, 64, GL_NEAREST_MIPMAP_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    f = fbo_tex(t, 1);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &v);
    glViewport(0, 0, 32, 32);
    glClearColor(0, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(0, 1, -1, 1, 0, 1, 0, 0);
    a = px(4, 4);
    b = px(28, 4);
    sprintf(msg, "level 1 (level query %d): left %06lx (00ffff), right %06lx (ff0000)", v, a, b);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && v == 1 && a == 0x00FFFF &&
              b == 0xFF0000, msg);
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);

    /* A cube face. */
    glGenTextures(1, &c);
    glBindTexture(GL_TEXTURE_CUBE_MAP, c);
    for (k = 0; k < 6; k++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + k, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, c, 0);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &v);
    glViewport(0, 0, 16, 16);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    a = px(8, 8);
    sprintf(msg, "cube face -Y (face query 0x%04x): %06lx (ff00ff)", v, a);
    check(v == GL_TEXTURE_CUBE_MAP_NEGATIVE_Y && a == 0xFF00FF, msg);
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &c);

    /* glGenerateMipmap and glTexSubImage2D after rendering: the RAM copy is
     * read back first. */
    t = texture(64, 64, GL_NEAREST);
    f = fbo_tex(t, 0);
    glViewport(0, 0, 64, 64);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(-1, 1, 0, 1, 0, 0, 0, 1);        /* top half blue */
    default_fb();
    glBindTexture(GL_TEXTURE_2D, t);
    for (k = 0; k < 8 * 8; k++) {
        img[k * 4] = 0; img[k * 4 + 1] = 255; img[k * 4 + 2] = 0; img[k * 4 + 3] = 255;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, img); /* green corner */
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    a = px(2, 2);
    b = px(40, 2);
    {
        unsigned long top = px(40, 60);
        sprintf(msg, "after rendering: glTexSubImage2D corner %06lx (00ff00), rest %06lx (ff0000), top %06lx (0000ff)",
                a, b, top);
        check(a == 0x00FF00 && b == 0xFF0000 && top == 0x0000FF, msg);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 4); /* 4x4 */
    a = px(1, 0);
    b = px(1, 3);
    sprintf(msg, "glGenerateMipmap from the rendered image: level 4 bottom %06lx, top %06lx (red, blue)", a, b);
    check(a == 0xFF0000 && b == 0x0000FF, msg);

    /* glCopyTexImage2D from the object (level 0 again). */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glGenTextures(1, &t2);
    glBindTexture(GL_TEXTURE_2D, t2);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 16, 32, 32, 0); /* rows 16..47: red then blue */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    default_fb();
    glClear(GL_COLOR_BUFFER_BIT);
    textured(t2);
    aglSwapBuffers();
    a = screen(0, -0.5f);
    b = screen(0, 0.5f);
    sprintf(msg, "glCopyTexImage2D from the object, sampled: bottom %06lx (ff0000), top %06lx (0000ff)", a, b);
    check(a == 0xFF0000 && b == 0x0000FF, msg);
    glDeleteTextures(1, &t2);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);
    free(img);
}

static void test_big(void)
{
    GLuint t = texture(512, 512, GL_NEAREST), f = fbo_tex(t, 0);
    unsigned long a, b, c;
    char msg[160];
    glViewport(0, 0, 512, 512);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    rect(0, 1, 0, 1, 0, 1, 1, 1);          /* the top right quarter white */
    a = px(0, 0);
    b = px(511, 511);
    c = px(300, 300);
    sprintf(msg, "512x512 (its own tile pools): (0,0) %06lx blue, (511,511) %06lx white, (300,300) %06lx white",
            a, b, c);
    check(a == 0x0000FF && b == 0xFFFFFF && c == 0xFFFFFF, msg);
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(1, &t);
}

static void test_errors(void)
{
    GLuint t = texture(32, 32, GL_NEAREST), l, f = fbo_tex(t, 0), r, d;
    GLenum st;
    char msg[160];
    glGenTextures(1, &l);
    glBindTexture(GL_TEXTURE_2D, l);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 32, 32, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l, 0);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    sprintf(msg, "a LUMINANCE texture: 0x%04x (8cd6 INCOMPLETE_ATTACHMENT)", st);
    check(st == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, msg);
    rect(-1, 1, -1, 1, 0, 1, 0, 0);
    check(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION, "drawing into it: GL_INVALID_FRAMEBUFFER_OPERATION");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    d = renderbuffer(GL_DEPTH_COMPONENT16, 16, 16);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    sprintf(msg, "32x32 colour, 16x16 depth: 0x%04x (8cd9 INCOMPLETE_DIMENSIONS)", st);
    check(st == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS, msg);
    glGenRenderbuffers(1, &r);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r);
    check(glGetError() == GL_INVALID_OPERATION, "a renderbuffer name never bound: GL_INVALID_OPERATION");
    glBindRenderbuffer(GL_RENDERBUFFER, r);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    sprintf(msg, "a renderbuffer without storage: 0x%04x (8cd6)", st);
    check(st == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, msg);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA, 8, 8);
    check(glGetError() == GL_INVALID_ENUM, "glRenderbufferStorage(GL_RGBA): GL_INVALID_ENUM");
    glDeleteTextures(1, &t); /* attached to the bound object: detached */
    {
        GLint type = -1;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        check(type == GL_NONE, "a deleted texture is detached from the bound object");
    }
    default_fb();
    glDeleteFramebuffers(1, &f);
    glDeleteRenderbuffers(1, &r);
    glDeleteRenderbuffers(1, &d);
    glDeleteTextures(1, &l);
}

static void test_churn(void)
{
    int i, bad = 0;
    char msg[120];
    for (i = 0; i < 40; i++) {
        GLuint t = texture(48, 48, GL_NEAREST), f = fbo_tex(t, 0), d = renderbuffer(GL_DEPTH24_STENCIL8_OES, 48, 48);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d);
        glViewport(0, 0, 48, 48);
        glClearColor((float)(i & 1), 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        rect(-0.5f, 0.5f, -0.5f, 0.5f, 0, 0, 1, 0);
        if (px(24, 24) != 0x00FF00 || px(2, 2) != ((i & 1) ? 0xFF00FFUL : 0x0000FFUL))
            bad++;
        default_fb();
        glClear(GL_COLOR_BUFFER_BIT);
        textured(t);
        aglSwapBuffers();
        glDeleteFramebuffers(1, &f);
        glDeleteRenderbuffers(1, &d);
        glDeleteTextures(1, &t);
    }
    sprintf(msg, "40 objects made, drawn into, sampled and deleted: %d wrong", bad);
    check(bad == 0, msg);
}

int main(void)
{
    ULONG err = 0, before, after;
    struct BitMap *bm;
    void *ctx;
    char msg[120];
    printf("ogles2 M5: framebuffer objects\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase || !OGLES2Open()) {
        printf("needs graphics, cybergraphics 41 and %s\n", OGLES2NAME);
        return 20;
    }
    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    before = AvailMem(MEMF_ANY);
    if (!bm || !(ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 24,
                                             OGLES2_CCT_STENCIL, 8, TAG_DONE))) {
        printf("FAIL: create a bitmap context (error %lu)\n", (unsigned long)err);
        return 20;
    }
    aglMakeCurrent(ctx);
    InitRastPort(&rp);
    rp.BitMap = bm;
    smooth = link(smooth_vs, smooth_fs, "a_position", "a_color");
    tex1 = link(tex1_vs, tex1_fs, "a_position", "a_texcoord");
    check(smooth && tex1, "smooth and tex1: link");
    glUseProgram(smooth);
    glUniformMatrix4fv(glGetUniformLocation(smooth, "u_mvp"), 1, GL_FALSE, identity);
    glUseProgram(tex1);
    glUniform1i(glGetUniformLocation(tex1, "u_tex"), 0);
    (void)lp; (void)lc; (void)tp; (void)tt;

    test_objects();
    test_rtt();
    test_midframe();
    test_depth();
    test_stencil();
    test_colourrb();
    test_levels();
    test_big();
    test_errors();
    test_churn();

    glUseProgram(0);
    glDeleteProgram(smooth);
    glDeleteProgram(tex1);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)before);
    check(after + 32768 >= before, msg);
    FreeBitMap(bm);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
