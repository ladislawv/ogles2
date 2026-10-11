/*
 * PostFX: a post-processing chain (plan section 9, M5 exit). The scene (a
 * cube coloured by its normals and a white bar, with depth) goes into
 * texture A; a horizontal blur of A into texture B; the screen gets a
 * vertical blur of B added to A, with a vignette. Three framebuffers a
 * frame, two of them objects. Written for the ogles2 corpus, in the OpenGL
 * ES 2.0 Programming Guide's style (its esUtil). MIT licence, like the rest
 * of tests/corpus.
 */
#include <stdlib.h>
#include "esUtil.h"

typedef struct
{
    GLuint scene_prog, blur_prog, final_prog;
    GLint scene_mvp, scene_colour, blur_src, blur_step, final_sharp, final_blur, final_step;
    GLuint tex[2], fbo[2], depth;
    GLfloat *vertices, *normals;
    GLuint *indices; /* esGenCube: 32-bit (OES_element_index_uint, as the book) */
    int nindices;
    float angle;
} UserData;

static const char quad_vs[] =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_tc;\n"
    "void main()\n"
    "{\n"
    "   v_tc = a_pos * 0.5 + 0.5;\n"
    "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static GLuint target(int w, int h, GLuint *tex)
{
    GLuint fbo;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    return fbo;
}

static int Init(ESContext *esContext)
{
    UserData *u = esContext->userData;
    int w = esContext->width, h = esContext->height;
    const char scene_vs[] =
        "uniform mat4 u_mvp;\n"
        "attribute vec4 a_position;\n"
        "attribute vec3 a_normal;\n"
        "varying vec3 v_colour;\n"
        "void main()\n"
        "{\n"
        "   v_colour = a_normal * 0.5 + 0.5;\n"
        "   gl_Position = u_mvp * a_position;\n"
        "}\n";
    const char scene_fs[] =
        "precision mediump float;\n"
        "uniform vec4 u_colour;\n"
        "varying vec3 v_colour;\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = mix(vec4(v_colour, 1.0), u_colour, u_colour.a);\n"
        "}\n";
    const char blur_fs[] =
        "precision mediump float;\n"
        "varying vec2 v_tc;\n"
        "uniform sampler2D s_src;\n"
        "uniform vec2 u_step;\n"
        "void main()\n"
        "{\n"
        "   vec4 c = texture2D(s_src, v_tc) * 0.375;\n"
        "   c += (texture2D(s_src, v_tc + u_step) + texture2D(s_src, v_tc - u_step)) * 0.25;\n"
        "   c += (texture2D(s_src, v_tc + 2.0 * u_step) + texture2D(s_src, v_tc - 2.0 * u_step)) * 0.0625;\n"
        "   gl_FragColor = c;\n"
        "}\n";
    const char final_fs[] =
        "precision mediump float;\n"
        "varying vec2 v_tc;\n"
        "uniform sampler2D s_sharp;\n"
        "uniform sampler2D s_blur;\n"
        "uniform vec2 u_step;\n"
        "void main()\n"
        "{\n"
        "   vec4 b = texture2D(s_blur, v_tc) * 0.375;\n"
        "   b += (texture2D(s_blur, v_tc + u_step) + texture2D(s_blur, v_tc - u_step)) * 0.25;\n"
        "   b += (texture2D(s_blur, v_tc + 2.0 * u_step) + texture2D(s_blur, v_tc - 2.0 * u_step)) * 0.0625;\n"
        "   vec2 d = v_tc - 0.5;\n"
        "   float vignette = 1.0 - dot(d, d) * 1.5;\n"
        "   gl_FragColor = vec4((texture2D(s_sharp, v_tc).rgb * 0.6 + b.rgb * 0.6) * vignette, 1.0);\n"
        "}\n";

    u->scene_prog = esLoadProgram(scene_vs, scene_fs);
    u->blur_prog = esLoadProgram(quad_vs, blur_fs);
    u->final_prog = esLoadProgram(quad_vs, final_fs);
    if (!u->scene_prog || !u->blur_prog || !u->final_prog)
        return GL_FALSE;
    u->scene_mvp = glGetUniformLocation(u->scene_prog, "u_mvp");
    u->scene_colour = glGetUniformLocation(u->scene_prog, "u_colour");
    u->blur_src = glGetUniformLocation(u->blur_prog, "s_src");
    u->blur_step = glGetUniformLocation(u->blur_prog, "u_step");
    u->final_sharp = glGetUniformLocation(u->final_prog, "s_sharp");
    u->final_blur = glGetUniformLocation(u->final_prog, "s_blur");
    u->final_step = glGetUniformLocation(u->final_prog, "u_step");
    u->nindices = esGenCube(1.0f, &u->vertices, &u->normals, NULL, &u->indices);

    u->fbo[0] = target(w, h, &u->tex[0]);
    glGenRenderbuffers(1, &u->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, u->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, u->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return GL_FALSE;
    u->fbo[1] = target(w, h, &u->tex[1]);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return GL_FALSE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    u->angle = 20.0f;
    return GL_TRUE;
}

static void Update(ESContext *esContext, float deltaTime)
{
    UserData *u = esContext->userData;
    u->angle += deltaTime * 50.0f;
    if (u->angle >= 360.0f)
        u->angle -= 360.0f;
}

static void quad(GLuint prog)
{
    static const GLfloat v[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    GLint a = glGetAttribLocation(prog, "a_pos");
    glEnableVertexAttribArray(a);
    glVertexAttribPointer(a, 2, GL_FLOAT, GL_FALSE, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(a);
}

static void Draw(ESContext *esContext)
{
    UserData *u = esContext->userData;
    int w = esContext->width, h = esContext->height;
    static const GLfloat bar[] = {-0.9f, -0.15f, 0.0f, 0.9f, -0.15f, 0.0f,
                                  -0.9f, 0.0f, 0.0f, 0.9f, 0.0f, 0.0f};
    GLint ap = glGetAttribLocation(u->scene_prog, "a_position");
    GLint an = glGetAttribLocation(u->scene_prog, "a_normal");
    ESMatrix persp, model, mvp, ident;

    /* 1: the scene into A. */
    glBindFramebuffer(GL_FRAMEBUFFER, u->fbo[0]);
    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.05f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(u->scene_prog);
    esMatrixLoadIdentity(&persp);
    esPerspective(&persp, 60.0f, (float)w / h, 1.0f, 20.0f);
    esMatrixLoadIdentity(&model);
    esTranslate(&model, 0.0f, 0.0f, -3.5f);
    esRotate(&model, 30.0f, 1.0f, 0.0f, 0.0f);
    esRotate(&model, u->angle, 0.0f, 1.0f, 0.0f);
    esMatrixMultiply(&mvp, &model, &persp);
    glUniformMatrix4fv(u->scene_mvp, 1, GL_FALSE, &mvp.m[0][0]);
    glUniform4f(u->scene_colour, 0.0f, 0.0f, 0.0f, 0.0f);
    glEnableVertexAttribArray(ap);
    glEnableVertexAttribArray(an);
    glVertexAttribPointer(ap, 3, GL_FLOAT, GL_FALSE, 0, u->vertices);
    glVertexAttribPointer(an, 3, GL_FLOAT, GL_FALSE, 0, u->normals);
    glDrawElements(GL_TRIANGLES, u->nindices, GL_UNSIGNED_INT, u->indices);
    glDisableVertexAttribArray(an);
    esMatrixLoadIdentity(&ident);
    glUniformMatrix4fv(u->scene_mvp, 1, GL_FALSE, &ident.m[0][0]);
    glUniform4f(u->scene_colour, 1.0f, 1.0f, 1.0f, 1.0f); /* the white bar, in front */
    glVertexAttrib3f(an, 0.0f, 0.0f, 1.0f);
    glVertexAttribPointer(ap, 3, GL_FLOAT, GL_FALSE, 0, bar);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(ap);
    glDisable(GL_DEPTH_TEST);

    /* 2: A blurred across into B. */
    glBindFramebuffer(GL_FRAMEBUFFER, u->fbo[1]);
    glUseProgram(u->blur_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, u->tex[0]);
    glUniform1i(u->blur_src, 0);
    glUniform2f(u->blur_step, 1.0f / w, 0.0f);
    quad(u->blur_prog);

    /* 3: B blurred down, added to A, on the screen. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(u->final_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, u->tex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, u->tex[1]);
    glUniform1i(u->final_sharp, 0);
    glUniform1i(u->final_blur, 1);
    glUniform2f(u->final_step, 0.0f, 1.0f / h);
    quad(u->final_prog);
    glActiveTexture(GL_TEXTURE0);

    eglSwapBuffers(esContext->eglDisplay, esContext->eglSurface);
}

static void ShutDown(ESContext *esContext)
{
    UserData *u = esContext->userData;
    glDeleteFramebuffers(2, u->fbo);
    glDeleteRenderbuffers(1, &u->depth);
    glDeleteTextures(2, u->tex);
    glDeleteProgram(u->scene_prog);
    glDeleteProgram(u->blur_prog);
    glDeleteProgram(u->final_prog);
    free(u->vertices);
    free(u->normals);
    free(u->indices);
}

int main(int argc, char *argv[])
{
    ESContext esContext;
    UserData userData;
    esInitContext(&esContext);
    esContext.userData = &userData;
    esCreateWindow(&esContext, "PostFX", 320, 240, ES_WINDOW_RGB);
    if (!Init(&esContext))
        return 1;
    esRegisterDrawFunc(&esContext, Draw);
    esRegisterUpdateFunc(&esContext, Update);
    esMainLoop(&esContext);
    ShutDown(&esContext);
    return 0;
}
