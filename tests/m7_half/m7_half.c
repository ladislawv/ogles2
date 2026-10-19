/*
 * M7 test: EXT_color_buffer_half_float (GitHub issue 5: OpenLara's water
 * renders its waves into half-float textures). Half-float RGBA textures
 * and 16F renderbuffers as colour attachments, drawn into, cleared,
 * blended, sampled and read back. The programs are compiled on the Amiga
 * (the package's libs/v3dglslc). Nothing to watch.
 *   usage: m7_half
 *   api       the string, completeness (half-float RGBA textures; RGBA16F,
 *             RGB16F, RG16F and R16F renderbuffers; RGB half-float and
 *             32-bit float textures stay incomplete), the sizes and types
 *   clear     a whole clear of a 16F target, read and sampled
 *   draw      values outside [0,1] kept (sampled), clamped by glReadPixels;
 *             the renderbuffer formats without some components
 *   readback  a glTexSubImage2D after a draw: the drawn part read back from
 *             the GPU into the RAM copy, values above 1 kept
 *   blend     ONE + ONE past 1.0
 *   pingpong  eight passes between two 16F textures, each adding 0.375
 *   targets   four 16F targets at 300x200 (32x16 tiles, own tile pools), an
 *             RGBA8 and a 16F target together
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

#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A
#define GL_RGB16F_EXT  0x881B
#endif
#ifndef GL_R16F_EXT
#define GL_R16F_EXT  0x822D
#define GL_RG16F_EXT 0x822F
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT 0x8211
#define GL_UNSIGNED_NORMALIZED_EXT                   0x8C17
#endif

struct GfxBase *GfxBase;
static int failures, checks;
static GLuint p_col, p_tex, p_mrt, out_fb, out_tex;
static GLint col_loc, tex_loc, scale_loc, add_loc, mrt_loc;
#define W 100
#define H 70

static const char *vs =
    "attribute vec4 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_pos.xy * 0.5 + 0.5;\n"
    "    gl_Position = a_pos;\n"
    "}\n";
static const char *fs_col =
    "precision mediump float;\n"
    "uniform vec4 u_col;\n"
    "void main() {\n"
    "    gl_FragColor = u_col;\n"
    "}\n";
static const char *fs_tex =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_scale;\n"
    "uniform vec4 u_add;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv) * u_scale + u_add;\n"
    "}\n";
static const char *fs_mrt =
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

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static GLuint link(const char *fs)
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
    return p;
}

/* A texture of format and type, w x h, nearest filtering, no image data. */
static GLuint new_texture(GLenum format, GLenum type, int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, type, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* A framebuffer with texture t as its colour attachment 0. */
static GLuint fb_for(GLuint t)
{
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    return fb;
}

static void full_quad(void)
{
    static const GLfloat v[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The bound framebuffer filled with colour c. */
static void fill(const GLfloat *c)
{
    glUseProgram(p_col);
    glUniform4fv(col_loc, 1, c);
    full_quad();
}

/* The bound framebuffer drawn from texture t: t * scale + add. */
static void from_texture(GLuint t, const GLfloat *scale, const GLfloat *add)
{
    glUseProgram(p_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glUniform1i(tex_loc, 0);
    glUniform4fv(scale_loc, 1, scale);
    glUniform4fv(add_loc, 1, add);
    full_quad();
}

static int near_byte(int got, GLfloat want, int tol)
{
    int w = want <= 0.0f ? 0 : want >= 1.0f ? 255 : (int)(want * 255.0f + 0.5f), d = got - w;
    return d >= -tol && d <= tol;
}

/* Pixel (x, y) of the bound framebuffer is c, within tol (in 1/255). */
static int pixel_is(int x, int y, const GLfloat *c, int tol, GLubyte *px)
{
    int k;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (k = 0; k < 4; k++)
        if (!near_byte(px[k], c[k], tol))
            return 0;
    return 1;
}

/* Corners and middle of the bound w x h framebuffer are c. */
static void expect_fb(const char *what, int w, int h, const GLfloat *c, int tol)
{
    static const int xs[5] = {0, 1, 0, 1, 2}, ys[5] = {0, 0, 1, 1, 2};
    char msg[220];
    GLubyte px[4], first[4] = {0, 0, 0, 0};
    int k, bad = 0;
    for (k = 0; k < 5; k++) {
        int x = xs[k] == 0 ? 0 : xs[k] == 1 ? w - 1 : w / 2, y = ys[k] == 0 ? 0 : ys[k] == 1 ? h - 1 : h / 2;
        if (!pixel_is(x, y, c, tol, px) && !bad++)
            memcpy(first, px, 4);
    }
    if (bad)
        sprintf(msg, "%s: %d pixel(s) wrong, the first %d %d %d %d", what, bad, first[0], first[1], first[2], first[3]);
    else
        sprintf(msg, "%s", what);
    check(!bad, msg);
}

/* Texture t (w x h) sampled into the RGBA8 output (t * scale + add): c. */
static void expect_sampled(const char *what, GLuint t, int w, int h, const GLfloat *scale, const GLfloat *add,
                           const GLfloat *c)
{
    glBindTexture(GL_TEXTURE_2D, out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fb);
    glViewport(0, 0, w, h);
    from_texture(t, scale, add);
    expect_fb(what, w, h, c, 2);
}

static const GLfloat one4[4] = {1, 1, 1, 1}, zero4[4] = {0, 0, 0, 0};
/* Values in [-1, 4] into [0.1, 0.9]: x * 0.2 + 0.3. */
static const GLfloat sc[4] = {0.2f, 0.2f, 0.2f, 0.2f}, ad[4] = {0.3f, 0.3f, 0.3f, 0.3f};

static GLint status_of(GLuint t)
{
    GLuint fb = fb_for(t);
    GLint s = (GLint)glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    return s;
}

static void test_api(void)
{
    const char *e = (const char *)glGetString(GL_EXTENSIONS);
    GLuint t, fb, rb;
    GLint v = 0, r = 0, a = 0;
    check(e && strstr(e, "GL_EXT_color_buffer_half_float") != 0, "GL_EXTENSIONS has GL_EXT_color_buffer_half_float");
    t = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, 16, 16);
    check(status_of(t) == GL_FRAMEBUFFER_COMPLETE, "an RGBA half-float texture is colour-renderable");
    glDeleteTextures(1, &t);
    t = new_texture(GL_RGB, GL_HALF_FLOAT_OES, 16, 16);
    check(status_of(t) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT,
          "an RGB half-float texture is not (the extension names RGBA textures only)");
    glDeleteTextures(1, &t);
    t = new_texture(GL_RGBA, GL_FLOAT, 16, 16);
    check(status_of(t) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, "a 32-bit float texture is still not (no EXT_color_buffer_float)");
    glDeleteTextures(1, &t);
    t = new_texture(GL_LUMINANCE, GL_HALF_FLOAT_OES, 16, 16);
    check(status_of(t) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, "a LUMINANCE half-float texture is not");
    glDeleteTextures(1, &t);

    t = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, 16, 16);
    fb = fb_for(t);
    glGetIntegerv(GL_RED_BITS, &r);
    glGetIntegerv(GL_ALPHA_BITS, &a);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT, &v);
    check(r == 16 && a == 16 && v == GL_FLOAT, "its GL_RED_BITS and GL_ALPHA_BITS are 16, its component type GL_FLOAT");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glDeleteTextures(1, &t);
    t = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, 16, 16);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT, &v);
    check(v == GL_UNSIGNED_NORMALIZED_EXT, "an RGBA8 texture's component type is GL_UNSIGNED_NORMALIZED_EXT");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glDeleteTextures(1, &t);

    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA16F_EXT, 16, 16);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_RED_SIZE, &r);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_ALPHA_SIZE, &a);
    check(glGetError() == GL_NO_ERROR && r == 16 && a == 16, "an RGBA16F_EXT renderbuffer: 16 bits red and alpha");
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT, &v);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && v == GL_FLOAT,
          "attached: complete, component type GL_FLOAT");
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB16F_EXT, 16, 16);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_ALPHA_SIZE, &a);
    check(glGetError() == GL_NO_ERROR && a == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "an RGB16F_EXT renderbuffer: no alpha, complete");
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RG16F_EXT, 16, 16);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_GREEN_SIZE, &r);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_BLUE_SIZE, &a);
    check(glGetError() == GL_NO_ERROR && r == 16 && a == 0 &&
              glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "an RG16F_EXT renderbuffer: 16 bits green, no blue, complete");
    glRenderbufferStorage(GL_RENDERBUFFER, GL_R16F_EXT, 16, 16);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_RED_SIZE, &r);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_GREEN_SIZE, &a);
    check(glGetError() == GL_NO_ERROR && r == 16 && a == 0 &&
              glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "an R16F_EXT renderbuffer: 16 bits red, no green, complete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &rb);
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
}

static void test_clear_draw(void)
{
    static const GLfloat cc[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    static const GLfloat big[4] = {2.0f, -1.0f, 0.5f, 3.0f};
    GLfloat want[4];
    GLuint t = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, W, H), fb = fb_for(t);
    int k;
    glViewport(0, 0, W, H);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    expect_fb("clear: a whole clear of a 16F target, read back", W, H, cc, 1);
    expect_sampled("clear: the same, sampled", t, W, H, one4, zero4, cc);

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, W, H);
    fill(big);
    for (k = 0; k < 4; k++)
        want[k] = big[k] * sc[k] + ad[k];
    expect_sampled("draw: (2, -1, 0.5, 3) kept in the 16F target (sampled x 0.2 + 0.3)", t, W, H, sc, ad, want);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    expect_fb("draw: glReadPixels clamps it to [0, 1]", W, H, big, 1);

    /* A glTexSubImage2D of a square at (8, 8), away from the pixels
     * expect_fb looks at: the rest is read back from the GPU first (the RAM
     * copy is stale), then everything stored again. */
    {
        GLushort px[4 * 4 * 4];
        GLubyte got[4];
        static const GLfloat square[4] = {0.5f * 0.2f + 0.3f, 0.5f * 0.2f + 0.3f, 0.5f * 0.2f + 0.3f,
                                          0.5f * 0.2f + 0.3f};
        for (k = 0; k < 64; k++)
            px[k] = 0x3800; /* 0.5 */
        glBindTexture(GL_TEXTURE_2D, t);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 8, 8, 4, 4, GL_RGBA, GL_HALF_FLOAT_OES, px);
        expect_sampled("readback: after a glTexSubImage2D the drawn part keeps (2, -1, 0.5, 3)", t, W, H, sc, ad, want);
        check(pixel_is(9, 9, square, 2, got), "readback: and the square holds the new 0.5");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &t);

    /* The renderbuffer formats without some components (kept as RGBA16F):
     * those read 0 (green, blue) and 1 (alpha). (0.5, 3, 2, -1) drawn. */
    {
        static const GLenum fmts[3] = {GL_RGB16F_EXT, GL_RG16F_EXT, GL_R16F_EXT};
        static const char *names[3] = {"RGB16F", "RG16F", "R16F"};
        static const GLfloat val[4] = {0.5f, 3.0f, 2.0f, -1.0f};
        static const GLfloat reads[3][4] = {{0.5f, 1, 1, 1}, {0.5f, 1, 0, 1}, {0.5f, 0, 0, 1}};
        GLuint rb;
        char what[120];
        for (k = 0; k < 3; k++) {
            glGenFramebuffers(1, &fb);
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, fmts[k], W, H);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            glViewport(0, 0, W, H);
            fill(val);
            sprintf(what, "draw: an %s renderbuffer reads its components, the others 0 and alpha 1", names[k]);
            expect_fb(what, W, H, reads[k], 1);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fb);
            glDeleteRenderbuffers(1, &rb);
        }
    }
}

static void test_blend_pingpong(void)
{
    static const GLfloat q[4] = {0.75f, 0.75f, 0.75f, 0.75f}, step[4] = {0.375f, 0.375f, 0.375f, 0.375f};
    static const GLfloat half4[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    GLfloat want[4];
    GLuint t[2], fb[2];
    int i, k;
    t[0] = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, W, H);
    fb[0] = fb_for(t[0]);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    fill(q);
    fill(q);
    glDisable(GL_BLEND);
    for (k = 0; k < 4; k++)
        want[k] = 1.5f * 0.5f;
    expect_sampled("blend: ONE + ONE of 0.75 twice is 1.5, past 1 (sampled x 0.5)", t[0], W, H, half4, zero4, want);

    /* OpenLara's waves: each pass reads one texture and writes the other. */
    t[1] = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, W, H);
    fb[1] = fb_for(t[1]);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 8; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb[(i + 1) & 1]);
        glViewport(0, 0, W, H);
        from_texture(t[i & 1], one4, step);
    }
    for (k = 0; k < 4; k++)
        want[k] = 3.0f * 0.2f + 0.3f;
    expect_sampled("pingpong: eight passes adding 0.375 make 3.0", t[0], W, H, sc, ad, want);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    glDeleteTextures(2, t);
}

static void test_targets(void)
{
    static const GLfloat cols[4][4] = {{2.0f, 0, 0, 1}, {0, 2.5f, 0, 1}, {0, 0, -1.0f, 1}, {3.0f, 3.0f, 0, 1}};
    static const GLfloat mixed[2][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {2.0f, 1.5f, -0.5f, 2.5f}};
    GLfloat want[4], c4[4][4];
    GLuint fb, t[4];
    int i, k, w = 300, h = 200;
    char what[120];

    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    for (i = 0; i < 4; i++) {
        t[i] = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, t[i], 0);
    }
    glDrawBuffersEXT(4, bufs);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "targets: four 16F targets complete");
    glViewport(0, 0, w, h);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p_mrt);
    glUniform4fv(mrt_loc, 4, &cols[0][0]);
    /* left half only: the right keeps the clear */
    {
        static const GLfloat v[8] = {-1, -1, 0, -1, -1, 1, 0, 1};
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    for (i = 0; i < 4; i++) {
        GLubyte px[4];
        static const GLfloat cleared[4] = {0.5f * 0.2f + 0.3f, 0.5f * 0.2f + 0.3f, 0.5f * 0.2f + 0.3f,
                                           0.5f * 0.2f + 0.3f};
        int bad = 0;
        for (k = 0; k < 4; k++)
            want[k] = cols[i][k] * sc[k] + ad[k];
        glBindTexture(GL_TEXTURE_2D, out_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, out_fb);
        glViewport(0, 0, w, h);
        from_texture(t[i], sc, ad);
        bad += !pixel_is(0, 0, want, 2, px) + !pixel_is(w / 2 - 1, h - 1, want, 2, px) + !pixel_is(w / 4, h / 2, want, 2, px);
        bad += !pixel_is(w / 2, 0, cleared, 2, px) + !pixel_is(w - 1, h - 1, cleared, 2, px);
        sprintf(what, "targets: four 16F at 300x200, target %d drawn on the left, cleared on the right", i);
        check(!bad, what);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(4, t);

    /* An RGBA8 target and a 16F one in one job. */
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    t[0] = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, W, H);
    t[1] = new_texture(GL_RGBA, GL_HALF_FLOAT_OES, W, H);
    glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[0], GL_TEXTURE_2D, t[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[1], GL_TEXTURE_2D, t[1], 0);
    glDrawBuffersEXT(2, bufs);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "targets: RGBA8 and 16F together complete");
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 4; i++)
        for (k = 0; k < 4; k++)
            c4[i][k] = i < 2 ? mixed[i][k] : 0.0f;
    glUseProgram(p_mrt);
    glUniform4fv(mrt_loc, 4, &c4[0][0]);
    full_quad();
    expect_sampled("targets: the RGBA8 one holds (0.25, 0.5, 0.75, 1)", t[0], W, H, one4, zero4, mixed[0]);
    for (k = 0; k < 4; k++)
        want[k] = mixed[1][k] * sc[k] + ad[k];
    expect_sampled("targets: the 16F one holds (2, 1.5, -0.5, 2.5)", t[1], W, H, sc, ad, want);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(2, t);
}

int main(void)
{
    struct BitMap *bm;
    void *ctx;
    ULONG e = 0;
    printf("ogles2 M7: EXT_color_buffer_half_float (rendering into half-float textures)\n");
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
    p_col = link(fs_col);
    p_tex = link(fs_tex);
    p_mrt = link(fs_mrt);
    if (!p_col || !p_tex || !p_mrt) {
        printf("FAIL: a program did not link (%u %u %u)\n", p_col, p_tex, p_mrt);
    } else {
        col_loc = glGetUniformLocation(p_col, "u_col");
        tex_loc = glGetUniformLocation(p_tex, "u_tex");
        scale_loc = glGetUniformLocation(p_tex, "u_scale");
        add_loc = glGetUniformLocation(p_tex, "u_add");
        mrt_loc = glGetUniformLocation(p_mrt, "u_col");
        out_tex = new_texture(GL_RGBA, GL_UNSIGNED_BYTE, W, H);
        out_fb = fb_for(out_tex);
        test_clear_draw();
        test_blend_pingpong();
        test_targets();
        check(glGetError() == GL_NO_ERROR, "no GL error left over");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &out_fb);
        glDeleteTextures(1, &out_tex);
    }
    if (p_col)
        glDeleteProgram(p_col);
    if (p_tex)
        glDeleteProgram(p_tex);
    if (p_mrt)
        glDeleteProgram(p_mrt);
    aglDestroyContext(ctx);
    FreeBitMap(bm);
    OGLES2Close();
    CloseLibrary((struct Library *)GfxBase);
    if (!p_col || !p_tex || !p_mrt)
        return 20;
    if (failures)
        printf("FAILED: %d of %d checks passed\n", checks - failures, checks);
    else
        printf("ALL PASSED: %d of %d checks passed\n", checks, checks);
    return failures ? 10 : 0;
}
