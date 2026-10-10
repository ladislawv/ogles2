/*
 * M1 step 2 test: the first triangles (plan section 9, M1), drawn with the
 * built-in program from client-side arrays and checked by reading back what
 * is displayed.
 *   usage: m1_draw [BITMAP] [WINDOW] [FULLSCREEN] [QUICK]   (default: all three)
 *
 * The program is the GLSL of tests/common/smooth_shaders.h, linked from its
 * binary next to this program (PROGDIR:<hash>.v3dp). For each surface kind:
 *   smooth colours and orientation (red bottom left, green bottom right,
 *   blue top), a constant colour from a disabled array, the matrix, culling,
 *   the depth test, viewport, scissor, blending, colour mask, strip and fan,
 *   lines and points; clears the tile buffer cannot do (scissored, masked,
 *   after a draw, depth between draws) and the stencil test where there is
 *   a stencil buffer (the window: D24S8); 100 frames of 200 triangles timed,
 *   and the context's memory returned at destroy.
 * On a visible surface each picture stays 1 s (QUICK: no pauses).
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest timer_req;
static int failures, checks, quick;


static GLuint prog;
static GLint loc_pos, loc_col, loc_mvp;

struct readback {
    struct RastPort *rp;
    int x0, y0, w, h;
    int visible;
    struct RastPort own;
};

static void check(int ok, const char *what, const char *kind)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", kind, what);
}

static double now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000.0 + tv.tv_micro / 1000.0;
}

/* Colour at GL window coordinates (bottom-left origin), 0xRRGGBB. */
static unsigned long px(struct readback *r, int x, int y)
{
    return ReadRGBPixel(r->rp, (UWORD)(r->x0 + x), (UWORD)(r->y0 + r->h - 1 - y)) & 0xFFFFFF;
}

/* Colour at normalised device coordinates. */
static unsigned long ndc(struct readback *r, float x, float y)
{
    return px(r, (int)((x + 1.0f) * 0.5f * r->w), (int)((y + 1.0f) * 0.5f * r->h));
}

static int near(ULONG a, ULONG b, int tol)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int d = (int)((a >> i) & 255) - (int)((b >> i) & 255);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}

#define R(c) ((int)((c) >> 16) & 255)
#define G(c) ((int)((c) >> 8) & 255)
#define B(c) ((int)(c) & 255)

static void show(struct readback *r)
{
    aglSwapBuffers();
    if (r->visible && !quick)
        Delay(50);
}

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static void mvp(const GLfloat *m)
{
    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, m);
}

static void clear(float r, float g, float b)
{
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* Draws `n` vertices: positions xyz, colours rgba (NULL: the array is off). */
static void draw(GLenum mode, const GLfloat *pos, const GLfloat *col, int n)
{
    glVertexAttribPointer(loc_pos, 3, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(loc_pos);
    if (col) {
        glVertexAttribPointer(loc_col, 4, GL_FLOAT, GL_FALSE, 0, col);
        glEnableVertexAttribArray(loc_col);
    } else {
        glDisableVertexAttribArray(loc_col);
    }
    glDrawArrays(mode, 0, n);
}

/* The reference triangle, counter-clockwise: red bottom left, green bottom
 * right, blue top. */
static const GLfloat tri_pos[9] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f, 0.0f, 0.8f, 0.0f};
static const GLfloat tri_col[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};

static int build_program(void)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *s1 = smooth_vs, *s2 = smooth_fs;
    GLint ok = 0;
    glShaderSource(vs, 1, &s1, 0);
    glCompileShader(vs);
    glShaderSource(fs, 1, &s2, 0);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 3, "a_color"); /* a binding, to test it is honoured */
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); /* stay alive while attached */
    glDeleteShader(fs);
    if (!ok)
        return 0;
    glUseProgram(prog);
    loc_pos = glGetAttribLocation(prog, "a_position");
    loc_col = glGetAttribLocation(prog, "a_color");
    loc_mvp = glGetUniformLocation(prog, "u_mvp");
    return 1;
}

static void run_kind(const char *kind, void *ctx, struct readback *r)
{
    char msg[200];
    unsigned long c, a, b2, t;
    GLint i, n;
    double t0, t1;

    aglMakeCurrent(ctx);
    if (!build_program()) {
        check(0, "link a program", kind);
        return;
    }
    sprintf(msg, "program: a_position at %ld, a_color at %ld (bound to 3), u_mvp at %ld",
            (long)loc_pos, (long)loc_col, (long)loc_mvp);
    check(loc_pos == 0 && loc_col == 3 && loc_mvp == 0, msg, kind);
    glGetIntegerv(GL_CURRENT_PROGRAM, &i);
    check(i == (GLint)prog, "GL_CURRENT_PROGRAM", kind);
    mvp(identity);

    /* 1. Smooth colours and orientation. */
    clear(0.25f, 0.25f, 0.25f);
    draw(GL_TRIANGLES, tri_pos, tri_col, 3);
    show(r);
    a = ndc(r, -0.7f, -0.75f);
    b2 = ndc(r, 0.7f, -0.75f);
    t = ndc(r, 0.0f, 0.7f);
    c = ndc(r, 0.0f, -0.2f);
    sprintf(msg, "smooth triangle: bottom left %06lx (red), bottom right %06lx (green), top %06lx (blue)",
            a, b2, t);
    check(R(a) > 200 && G(a) < 60 && B(a) < 60 && G(b2) > 200 && R(b2) < 60 && B(b2) < 60 &&
          B(t) > 200 && R(t) < 60 && G(t) < 60, msg, kind);
    sprintf(msg, "smooth triangle: inside %06lx is a mix, outside %06lx and %06lx are the clear colour",
            c, ndc(r, -0.95f, 0.95f), ndc(r, 0.95f, 0.95f));
    check(R(c) > 20 && G(c) > 20 && B(c) > 20 && !near(c, 0x404040, 8) &&
          near(ndc(r, -0.95f, 0.95f), 0x404040, 1) && near(ndc(r, 0.95f, 0.95f), 0x404040, 1), msg, kind);

    /* 2. A disabled array reads the current attribute: yellow. */
    clear(0, 0, 0);
    glVertexAttrib4f(loc_col, 1.0f, 1.0f, 0.0f, 1.0f);
    draw(GL_TRIANGLES, tri_pos, 0, 3);
    show(r);
    c = ndc(r, 0.0f, -0.2f);
    sprintf(msg, "constant colour from a disabled array: %06lx (ffff00)", c);
    check(near(c, 0xFFFF00, 1), msg, kind);

    /* 3. The matrix: scaled by 0.5 and moved right by 0.5. */
    {
        static const GLfloat m[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1};
        clear(0, 0, 0);
        mvp(m);
        draw(GL_TRIANGLES, tri_pos, tri_col, 3);
        show(r);
        mvp(identity);
        a = ndc(r, 0.5f, -0.2f);  /* inside the moved triangle */
        b2 = ndc(r, -0.5f, -0.2f); /* inside the old one only */
        sprintf(msg, "matrix (scale 0.5, x + 0.5): %06lx drawn, %06lx empty", a, b2);
        check(a != 0 && b2 == 0, msg, kind);
    }

    /* 4. Culling: the triangle is counter-clockwise, so front-facing. */
    glEnable(GL_CULL_FACE);
    clear(0, 0, 0);
    draw(GL_TRIANGLES, tri_pos, tri_col, 3);
    show(r);
    a = ndc(r, 0.0f, -0.2f);
    glCullFace(GL_FRONT);
    clear(0, 0, 0);
    draw(GL_TRIANGLES, tri_pos, tri_col, 3);
    show(r);
    b2 = ndc(r, 0.0f, -0.2f);
    glFrontFace(GL_CW);
    clear(0, 0, 0);
    draw(GL_TRIANGLES, tri_pos, tri_col, 3);
    show(r);
    t = ndc(r, 0.0f, -0.2f);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_CULL_FACE);
    sprintf(msg, "culling: back culled keeps it (%06lx), front culled removes it (%06lx), "
                 "front face CW keeps it (%06lx)", a, b2, t);
    check(a != 0 && b2 == 0 && t != 0, msg, kind);

    /* 5. Depth test: a near blue triangle first, then a far red one. */
    {
        static const GLfloat nearp[9] = {-0.8f, -0.8f, -0.5f, 0.8f, -0.8f, -0.5f, 0.0f, 0.8f, -0.5f};
        static const GLfloat farp[9] = {-0.8f, -0.6f, 0.5f, 0.8f, -0.6f, 0.5f, 0.0f, 1.0f, 0.5f};
        GLint bits = 0;
        glGetIntegerv(GL_DEPTH_BITS, &bits);
        glEnable(GL_DEPTH_TEST);
        clear(0, 0, 0);
        glVertexAttrib4f(loc_col, 0, 0, 1, 1);
        draw(GL_TRIANGLES, nearp, 0, 3);
        glVertexAttrib4f(loc_col, 1, 0, 0, 1);
        draw(GL_TRIANGLES, farp, 0, 3);
        show(r);
        a = ndc(r, 0.0f, 0.0f);   /* both cover it: the near one must win */
        b2 = ndc(r, 0.0f, 0.85f); /* only the far one */
        glDisable(GL_DEPTH_TEST);
        sprintf(msg, "depth test (%ld bits): overlap %06lx (blue), far only %06lx (red)", (long)bits,
                a, b2);
        check(bits ? (near(a, 0x0000FF, 1) && near(b2, 0xFF0000, 1)) : 1, msg, kind);
        clear(0, 0, 0);
        glVertexAttrib4f(loc_col, 0, 0, 1, 1);
        draw(GL_TRIANGLES, nearp, 0, 3);
        glVertexAttrib4f(loc_col, 1, 0, 0, 1);
        draw(GL_TRIANGLES, farp, 0, 3);
        show(r);
        a = ndc(r, 0.0f, 0.0f);
        sprintf(msg, "depth test off: the later triangle covers the earlier (%06lx, red)", a);
        check(near(a, 0xFF0000, 1), msg, kind);
    }

    /* 6. Viewport: the left half only. */
    clear(0, 0, 0);
    glViewport(0, 0, r->w / 2, r->h);
    draw(GL_TRIANGLES, tri_pos, tri_col, 3);
    glViewport(0, 0, r->w, r->h);
    show(r);
    a = px(r, r->w / 4, r->h / 2 - r->h / 10);
    b2 = px(r, r->w * 3 / 4, r->h / 2 - r->h / 10);
    sprintf(msg, "viewport (left half): left %06lx drawn, right %06lx empty", a, b2);
    check(a != 0 && b2 == 0, msg, kind);

    /* 7. Scissor: a box in the middle. */
    clear(0, 0, 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r->w / 4, r->h / 4, r->w / 2, r->h / 2);
    glVertexAttrib4f(loc_col, 1, 1, 1, 1);
    {
        static const GLfloat full[9] = {-3.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 3.0f, 0.0f};
        draw(GL_TRIANGLES, full, 0, 3); /* covers the whole viewport */
    }
    glDisable(GL_SCISSOR_TEST);
    show(r);
    a = px(r, r->w / 2, r->h / 2);
    b2 = px(r, r->w / 8, r->h / 2);
    t = px(r, r->w / 2, r->h - 1 - r->h / 8);
    sprintf(msg, "scissor box: inside %06lx white, left %06lx and top %06lx empty", a, b2, t);
    check(a == 0xFFFFFF && b2 == 0 && t == 0, msg, kind);

    /* 8. Blending: half-transparent red over blue. */
    clear(0, 0, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glVertexAttrib4f(loc_col, 1, 0, 0, 0.5f);
    draw(GL_TRIANGLES, tri_pos, 0, 3);
    glDisable(GL_BLEND);
    show(r);
    a = ndc(r, 0.0f, -0.2f);
    sprintf(msg, "blending (alpha 0.5 red over blue): %06lx (808080 with green 0, +-2)", a);
    check(R(a) >= 0x7E && R(a) <= 0x81 && G(a) == 0 && B(a) >= 0x7E && B(a) <= 0x81, msg, kind);

    /* 9. Colour mask: white with red masked off. */
    clear(0, 0, 0);
    glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
    glVertexAttrib4f(loc_col, 1, 1, 1, 1);
    draw(GL_TRIANGLES, tri_pos, 0, 3);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    show(r);
    a = ndc(r, 0.0f, -0.2f);
    sprintf(msg, "colour mask (red off): %06lx (00ffff)", a);
    check(a == 0x00FFFF, msg, kind);

    /* 10. Strip and fan: two quads, green (strip) left, magenta (fan) right. */
    {
        static const GLfloat strip[12] = {-0.9f, -0.5f, 0, -0.1f, -0.5f, 0, -0.9f, 0.5f, 0, -0.1f, 0.5f, 0};
        static const GLfloat fan[12] = {0.1f, -0.5f, 0, 0.9f, -0.5f, 0, 0.9f, 0.5f, 0, 0.1f, 0.5f, 0};
        clear(0, 0, 0);
        glVertexAttrib4f(loc_col, 0, 1, 0, 1);
        draw(GL_TRIANGLE_STRIP, strip, 0, 4);
        glVertexAttrib4f(loc_col, 1, 0, 1, 1);
        draw(GL_TRIANGLE_FAN, fan, 0, 4);
        show(r);
        a = ndc(r, -0.2f, 0.4f); /* the upper-right half of the strip's quad */
        b2 = ndc(r, 0.2f, 0.4f); /* the fan's second triangle */
        sprintf(msg, "triangle strip %06lx (00ff00) and fan %06lx (ff00ff)", a, b2);
        check(a == 0x00FF00 && b2 == 0xFF00FF, msg, kind);
    }

    /* 11. Lines and points. */
    {
        float y = 0.5f / r->h * 2.0f - 1.0f + 2.0f * (r->h / 2) / r->h; /* a pixel centre row */
        GLfloat line[6], pts[3];
        line[0] = -1.0f; line[1] = y; line[2] = 0;
        line[3] = 1.0f;  line[4] = y; line[5] = 0;
        pts[0] = 0.5f / r->w * 2.0f - 1.0f + 2.0f * (r->w / 4) / r->w;
        pts[1] = 0.5f / r->h * 2.0f - 1.0f + 2.0f * (r->h / 4) / r->h;
        pts[2] = 0;
        clear(0, 0, 0);
        glVertexAttrib4f(loc_col, 1, 1, 1, 1);
        draw(GL_LINES, line, 0, 2);
        draw(GL_POINTS, pts, 0, 1);
        show(r);
        a = px(r, r->w / 3, r->h / 2);
        b2 = px(r, r->w / 4, r->h / 4);
        t = px(r, r->w / 3, r->h / 2 + 3);
        sprintf(msg, "line %06lx and point %06lx white, beside the line %06lx empty", a, b2, t);
        check(a == 0xFFFFFF && b2 == 0xFFFFFF && t == 0, msg, kind);
    }

    /* 12. Clears drawn as rectangles. */
    clear(0, 0, 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r->w / 4, r->h / 4, r->w / 2, r->h / 2);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    show(r);
    a = px(r, r->w / 2, r->h / 2);
    b2 = px(r, r->w / 8, r->h / 2);
    sprintf(msg, "scissored clear: inside %06lx red, outside %06lx black", a, b2);
    check(a == 0xFF0000 && b2 == 0, msg, kind);

    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT); /* the whole framebuffer, at the job's start: the tile buffer */
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT); /* masked: a rectangle */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    show(r);
    a = px(r, r->w / 2, r->h / 2);
    sprintf(msg, "masked clear (green only) over blue: %06lx (00ffff)", a);
    check(a == 0x00FFFF, msg, kind);

    clear(0, 0, 0);
    draw(GL_TRIANGLES, tri_pos, tri_col, 3);
    glClearColor(0, 0.5f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT); /* after a draw: a rectangle over it */
    show(r);
    a = ndc(r, 0.0f, -0.2f);
    b2 = ndc(r, -0.95f, 0.95f);
    sprintf(msg, "clear after a draw covers it: %06lx and %06lx (008000)", a, b2);
    check(near(a, 0x008000, 1) && near(b2, 0x008000, 1), msg, kind);

    {
        static const GLfloat nearp[9] = {-0.8f, -0.8f, -0.5f, 0.8f, -0.8f, -0.5f, 0.0f, 0.8f, -0.5f};
        static const GLfloat farp[9] = {-0.8f, -0.8f, 0.5f, 0.8f, -0.8f, 0.5f, 0.0f, 0.8f, 0.5f};
        GLint bits = 0;
        glGetIntegerv(GL_DEPTH_BITS, &bits);
        glEnable(GL_DEPTH_TEST);
        clear(0, 0, 0);
        glVertexAttrib4f(loc_col, 0, 0, 1, 1);
        draw(GL_TRIANGLES, nearp, 0, 3);
        glClear(GL_DEPTH_BUFFER_BIT); /* between draws: a rectangle writing depth only */
        glVertexAttrib4f(loc_col, 1, 0, 0, 1);
        draw(GL_TRIANGLES, farp, 0, 3);
        glDisable(GL_DEPTH_TEST);
        show(r);
        a = ndc(r, 0.0f, -0.2f);
        sprintf(msg, "depth clear between draws: the far triangle now passes (%06lx, red)", a);
        check(bits ? near(a, 0xFF0000, 1) : 1, msg, kind);
    }

    /* 13. Stencil: mark the triangle, then draw a white square where it is
     * marked and a grey one where it is not. */
    {
        static const GLfloat square[12] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
        GLint sbits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &sbits);
        if (sbits) {
            glClearStencil(0);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            draw(GL_TRIANGLES, tri_pos, tri_col, 3);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilFunc(GL_EQUAL, 1, 0xff);
            glVertexAttrib4f(loc_col, 1, 1, 1, 1);
            draw(GL_TRIANGLE_STRIP, square, 0, 4);
            glStencilFunc(GL_NOTEQUAL, 1, 0xff);
            glVertexAttrib4f(loc_col, 0.5f, 0.5f, 0.5f, 1);
            draw(GL_TRIANGLE_STRIP, square, 0, 4);
            glDisable(GL_STENCIL_TEST);
            show(r);
            a = ndc(r, 0.0f, -0.2f);
            b2 = ndc(r, -0.95f, 0.95f);
            sprintf(msg, "stencil (%ld bits): marked %06lx (white), unmarked %06lx (808080)",
                    (long)sbits, a, b2);
            check(a == 0xFFFFFF && near(b2, 0x808080, 1), msg, kind);

            /* A scissored stencil clear, then the same test. */
            glClearStencil(1);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, r->w / 2, r->h);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 1, 0xff);
            glVertexAttrib4f(loc_col, 1, 1, 1, 1);
            draw(GL_TRIANGLE_STRIP, square, 0, 4);
            glDisable(GL_STENCIL_TEST);
            show(r);
            a = px(r, r->w / 4, r->h / 2);
            b2 = px(r, r->w * 3 / 4, r->h / 2);
            sprintf(msg, "scissored stencil clear: left %06lx (cleared to 0, black), right %06lx (white)",
                    a, b2);
            check(a == 0 && b2 == 0xFFFFFF, msg, kind);
        } else {
            printf("     %s: no stencil buffer, stencil test skipped\n", kind);
        }
    }

    check(glGetError() == GL_NO_ERROR, "no GL error", kind);

    /* 14. 100 frames of 200 small smooth triangles, timed. */
    {
        static GLfloat pos[200 * 9], col[200 * 12];
        int k, f;
        for (k = 0; k < 200; k++) {
            float x = -0.95f + (k % 20) * 0.095f, yy = -0.95f + (k / 20) * 0.19f;
            GLfloat *p = &pos[k * 9], *q = &col[k * 12];
            p[0] = x; p[1] = yy; p[2] = 0;
            p[3] = x + 0.09f; p[4] = yy; p[5] = 0;
            p[6] = x + 0.045f; p[7] = yy + 0.18f; p[8] = 0;
            memcpy(q, tri_col, sizeof(tri_col));
        }
        t0 = now_ms();
        for (f = 0; f < 100; f++) {
            clear(0, 0, (f & 31) / 64.0f);
            draw(GL_TRIANGLES, pos, col, 600);
            aglSwapBuffers();
        }
        t1 = now_ms();
        n = 100 * 200;
        printf("     %s: 100 frames of 200 triangles in %ld ms (%ld.%ld ms per frame)\n", kind,
               (long)(t1 - t0), (long)((t1 - t0) / 100), (long)((t1 - t0) / 10) % 10);
        (void)n;
        if (r->visible && !quick)
            Delay(50);
    }
    glUseProgram(0);
    glDeleteProgram(prog);
    check(glGetError() == GL_NO_ERROR, "no GL error after the timing run and deleting the program",
          kind);
}

static void finish_kind(const char *kind, void *ctx, ULONG mem_before)
{
    char msg[120];
    ULONG after;
    aglDestroyContext(ctx);
    after = AvailMem(MEMF_ANY);
    sprintf(msg, "destroy returns the memory (%ld bytes difference)", (long)after - (long)mem_before);
    check(after + 16384 >= mem_before, msg, kind);
}

static void test_bitmap(void)
{
    ULONG err = 0, before;
    struct readback r;
    void *ctx;
    struct BitMap *bm = AllocBitMap(320, 240, 32,
                                    BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm) {
        check(0, "allocate a 320x240 BGRA32 bitmap", "bitmap");
        return;
    }
    before = AvailMem(MEMF_ANY);
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 16, TAG_DONE);
    check(ctx != 0, "create (BITMAP tag, 16-bit depth)", "bitmap");
    if (ctx) {
        InitRastPort(&r.own);
        r.own.BitMap = bm;
        r.rp = &r.own;
        r.x0 = r.y0 = 0;
        r.w = 320;
        r.h = 240;
        r.visible = 0;
        run_kind("bitmap", ctx, &r);
        finish_kind("bitmap", ctx, before);
    } else {
        printf("     error %lu\n", (unsigned long)err);
    }
    FreeBitMap(bm);
}

static void test_window(void)
{
    ULONG err = 0, w = 0, h = 0, before = AvailMem(MEMF_ANY);
    struct readback r;
    struct Window *win = 0;
    void *ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_STENCIL, 8,
                                      OGLES2_CCT_PISTORM_WIDTH, 320,
                                      OGLES2_CCT_PISTORM_HEIGHT, 240, OGLES2_CCT_GET_WIDTH,
                                      (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                      OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
    check(ctx != 0 && win != 0, "create (own window, 24-bit depth, 8-bit stencil)", "window");
    if (!ctx || !win) {
        if (ctx)
            aglDestroyContext(ctx);
        return;
    }
    r.rp = win->RPort;
    r.x0 = win->BorderLeft;
    r.y0 = win->BorderTop;
    r.w = (int)w;
    r.h = (int)h;
    r.visible = 1;
    run_kind("window", ctx, &r);
    finish_kind("window", ctx, before);
}

static ULONG find_mode(void)
{
    ULONG id = NextDisplayInfo(INVALID_ID), best = INVALID_ID;
    while (id != (ULONG)INVALID_ID) {
        if (IsCyberModeID(id)) {
            ULONG f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
            ULONG w = GetCyberIDAttr(CYBRIDATTR_WIDTH, id);
            ULONG h = GetCyberIDAttr(CYBRIDATTR_HEIGHT, id);
            if ((f == PIXFMT_BGRA32 || f == PIXFMT_RGBA32) && w >= 320 && w <= 800 && h >= 240) {
                if (w == 640 && h == 480)
                    return id;
                if (best == (ULONG)INVALID_ID)
                    best = id;
            }
        }
        id = NextDisplayInfo(id);
    }
    return best;
}

static void test_fullscreen(void)
{
    ULONG err = 0, w = 0, h = 0, before = AvailMem(MEMF_ANY);
    struct readback r;
    struct Window *win = 0;
    void *ctx;
    ULONG mode = find_mode();
    if (mode == (ULONG)INVALID_ID) {
        printf("SKIP: fullscreen: no 32-bit RTG mode of 320x240 to 800x600\n");
        return;
    }
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_MODEID, mode, OGLES2_CCT_DEPTH, 32,
                                OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h,
                                OGLES2_CCT_PISTORM_GET_WINDOW, (ULONG)&win, TAG_DONE);
    check(ctx != 0 && win != 0, "create (MODEID tag, 32-bit depth)", "fullscreen");
    if (!ctx || !win) {
        if (ctx)
            aglDestroyContext(ctx);
        return;
    }
    r.rp = &win->WScreen->RastPort;
    r.x0 = r.y0 = 0;
    r.w = (int)w;
    r.h = (int)h;
    r.visible = 1;
    run_kind("fullscreen", ctx, &r);
    finish_kind("fullscreen", ctx, before);
}

int main(int argc, char **argv)
{
    int i, any = 0;

    printf("ogles2 M1 step 2: first triangles\n");
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0)) {
        printf("needs cybergraphics.library 41 and timer.device\n");
        return 20;
    }
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES2Open()) {
        printf("cannot open %s %d\n", OGLES2NAME, OGLES2_VERSION);
        return 20;
    }
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "QUICK"))
            quick = 1;
        else
            any = 1;
    for (i = 1; i < argc || !any; i++) {
        const char *a = any ? argv[i] : 0;
        if (!any || !strcmp(a, "BITMAP"))
            test_bitmap();
        if (!any || !strcmp(a, "WINDOW"))
            test_window();
        if (!any || !strcmp(a, "FULLSCREEN"))
            test_fullscreen();
        if (!any)
            break;
    }
    OGLES2Close();
    CloseDevice((struct IORequest *)&timer_req);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures,
           checks);
    return failures ? 10 : 0;
}
