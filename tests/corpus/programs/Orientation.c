/*
 * Orientation: the plan's section 10 orientation test on both kinds of
 * framebuffer (plan 7.11, M5 exit). One scene -- a gl_FragCoord gradient,
 * face culling with both windings, point sprites coloured by gl_PointCoord
 * and a marker in one corner -- drawn into a 160x160 texture through a
 * framebuffer object and shown on the left, and drawn straight onto the
 * screen on the right. The two halves must be the same, and the same as
 * Mesa's. Written for the ogles2 corpus, in the OpenGL ES 2.0 Programming
 * Guide's style (its esUtil). MIT licence, like the rest of tests/corpus.
 */
#include <stdlib.h>
#include "esUtil.h"

#define S 160

typedef struct
{
    GLuint grad_prog, flat_prog, point_prog, tex_prog;
    GLint grad_origin, flat_colour, tex_sampler;
    GLuint tex, fbo;
} UserData;

static int Init(ESContext *esContext)
{
    UserData *u = esContext->userData;
    const char pos_vs[] =
        "attribute vec2 a_pos;\n"
        "void main()\n"
        "{\n"
        "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    const char grad_fs[] =
        "precision mediump float;\n"
        "uniform vec2 u_origin;\n"
        "void main()\n"
        "{\n"
        "   vec2 p = (gl_FragCoord.xy - u_origin) / 160.0;\n"
        "   gl_FragColor = vec4(p.x, p.y, 0.25, 1.0);\n"
        "}\n";
    const char flat_fs[] =
        "precision mediump float;\n"
        "uniform vec4 u_colour;\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = u_colour;\n"
        "}\n";
    const char point_vs[] =
        "attribute vec2 a_pos;\n"
        "void main()\n"
        "{\n"
        "   gl_PointSize = 24.0;\n"
        "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    const char point_fs[] =
        "precision mediump float;\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = vec4(gl_PointCoord.x, gl_PointCoord.y, 1.0, 1.0);\n"
        "}\n";
    const char tex_vs[] =
        "attribute vec2 a_pos;\n"
        "varying vec2 v_tc;\n"
        "void main()\n"
        "{\n"
        "   v_tc = a_pos * 0.5 + 0.5;\n"
        "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    const char tex_fs[] =
        "precision mediump float;\n"
        "varying vec2 v_tc;\n"
        "uniform sampler2D s_tex;\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = texture2D(s_tex, v_tc);\n"
        "}\n";
    u->grad_prog = esLoadProgram(pos_vs, grad_fs);
    u->flat_prog = esLoadProgram(pos_vs, flat_fs);
    u->point_prog = esLoadProgram(point_vs, point_fs);
    u->tex_prog = esLoadProgram(tex_vs, tex_fs);
    if (!u->grad_prog || !u->flat_prog || !u->point_prog || !u->tex_prog)
        return GL_FALSE;
    u->grad_origin = glGetUniformLocation(u->grad_prog, "u_origin");
    u->flat_colour = glGetUniformLocation(u->flat_prog, "u_colour");
    u->tex_sampler = glGetUniformLocation(u->tex_prog, "s_tex");

    glGenTextures(1, &u->tex);
    glBindTexture(GL_TEXTURE_2D, u->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &u->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, u->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, u->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return GL_FALSE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return GL_TRUE;
}

static void draw2d(GLuint prog, const GLfloat *v, GLenum mode, int n)
{
    GLint a = glGetAttribLocation(prog, "a_pos");
    glUseProgram(prog);
    glEnableVertexAttribArray(a);
    glVertexAttribPointer(a, 2, GL_FLOAT, GL_FALSE, 0, v);
    glDrawArrays(mode, 0, n);
    glDisableVertexAttribArray(a);
}

static void triangle(UserData *u, const GLfloat *v, float r, float g, float b)
{
    glUseProgram(u->flat_prog);
    glUniform4f(u->flat_colour, r, g, b, 1.0f);
    draw2d(u->flat_prog, v, GL_TRIANGLES, 3);
}

/* The scene in the current viewport, whose lower left is `ox, oy` in
 * window coordinates. */
static void scene(UserData *u, float ox, float oy)
{
    static const GLfloat full[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    /* Upper row: front faces counter-clockwise. Left one counter-clockwise
     * (red, drawn), right one clockwise (green, culled). */
    static const GLfloat ul[] = {-0.9f, 0.1f, -0.1f, 0.1f, -0.5f, 0.8f};
    static const GLfloat ur[] = {0.1f, 0.1f, 0.5f, 0.8f, 0.9f, 0.1f};
    /* Lower row: front faces clockwise. Left clockwise (blue, drawn),
     * right counter-clockwise (yellow, culled). */
    static const GLfloat ll[] = {-0.9f, -0.8f, -0.5f, -0.1f, -0.1f, -0.8f};
    static const GLfloat lr[] = {0.1f, -0.8f, 0.9f, -0.8f, 0.5f, -0.1f};
    /* A marker in the top left corner, pointing right. */
    static const GLfloat mark[] = {-1.0f, 1.0f, -1.0f, 0.8f, -0.7f, 0.9f};
    static const GLfloat points[] = {-0.6f, -0.95f + 0.07f, 0.0f, 0.0f, 0.6f, 0.45f};

    glUseProgram(u->grad_prog);
    glUniform2f(u->grad_origin, ox, oy);
    draw2d(u->grad_prog, full, GL_TRIANGLE_STRIP, 4);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    triangle(u, ul, 1, 0, 0);
    triangle(u, ur, 0, 1, 0);
    glFrontFace(GL_CW);
    triangle(u, ll, 0, 0, 1);
    triangle(u, lr, 1, 1, 0);
    glFrontFace(GL_CCW);
    glDisable(GL_CULL_FACE);
    triangle(u, mark, 1, 1, 1);
    draw2d(u->point_prog, points, GL_POINTS, 3);
}

static void Draw(ESContext *esContext)
{
    UserData *u = esContext->userData;
    static const GLfloat full[] = {-1, -1, 1, -1, -1, 1, 1, 1};

    /* The scene into the texture. */
    glBindFramebuffer(GL_FRAMEBUFFER, u->fbo);
    glViewport(0, 0, S, S);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    scene(u, 0.0f, 0.0f);

    /* The screen: the texture on the left, the scene again on the right. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, esContext->width, esContext->height);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, S, S);
    glUseProgram(u->tex_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, u->tex);
    glUniform1i(u->tex_sampler, 0);
    draw2d(u->tex_prog, full, GL_TRIANGLE_STRIP, 4);
    glViewport(S, 0, S, S);
    scene(u, (float)S, 0.0f);

    eglSwapBuffers(esContext->eglDisplay, esContext->eglSurface);
}

static void ShutDown(ESContext *esContext)
{
    UserData *u = esContext->userData;
    glDeleteFramebuffers(1, &u->fbo);
    glDeleteTextures(1, &u->tex);
    glDeleteProgram(u->grad_prog);
    glDeleteProgram(u->flat_prog);
    glDeleteProgram(u->point_prog);
    glDeleteProgram(u->tex_prog);
}

int main(int argc, char *argv[])
{
    ESContext esContext;
    UserData userData;
    esInitContext(&esContext);
    esContext.userData = &userData;
    esCreateWindow(&esContext, "Orientation", 2 * S, S, ES_WINDOW_RGB);
    if (!Init(&esContext))
        return 1;
    esRegisterDrawFunc(&esContext, Draw);
    esMainLoop(&esContext);
    ShutDown(&esContext);
    return 0;
}
