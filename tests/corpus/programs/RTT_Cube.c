/*
 * RTT_Cube: render to texture with a depth renderbuffer (plan section 9,
 * M5 exit). A cube coloured by its normals is drawn into a 256x256 RGBA
 * texture with a DEPTH_COMPONENT16 renderbuffer; that texture covers the
 * faces of a second cube drawn on the screen. Written for the ogles2
 * corpus, in the OpenGL ES 2.0 Programming Guide's style (its esUtil).
 * MIT licence, like the rest of tests/corpus.
 */
#include <stdlib.h>
#include "esUtil.h"

#define RTT 256

typedef struct
{
    GLuint colour_prog, tex_prog;
    GLint colour_mvp, tex_mvp, tex_sampler;
    GLuint tex, fbo, depth;
    GLfloat *vertices, *normals, *texcoords;
    GLuint *indices; /* esGenCube: 32-bit (OES_element_index_uint, as the book) */
    int nindices;
    float angle;
} UserData;

static int Init(ESContext *esContext)
{
    UserData *u = esContext->userData;
    const char colour_vs[] =
        "uniform mat4 u_mvp;\n"
        "attribute vec4 a_position;\n"
        "attribute vec3 a_normal;\n"
        "varying vec3 v_colour;\n"
        "void main()\n"
        "{\n"
        "   v_colour = a_normal * 0.5 + 0.5;\n"
        "   gl_Position = u_mvp * a_position;\n"
        "}\n";
    const char colour_fs[] =
        "precision mediump float;\n"
        "varying vec3 v_colour;\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = vec4(v_colour, 1.0);\n"
        "}\n";
    const char tex_vs[] =
        "uniform mat4 u_mvp;\n"
        "attribute vec4 a_position;\n"
        "attribute vec2 a_texcoord;\n"
        "varying vec2 v_texcoord;\n"
        "void main()\n"
        "{\n"
        "   v_texcoord = a_texcoord;\n"
        "   gl_Position = u_mvp * a_position;\n"
        "}\n";
    const char tex_fs[] =
        "precision mediump float;\n"
        "varying vec2 v_texcoord;\n"
        "uniform sampler2D s_texture;\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = texture2D(s_texture, v_texcoord);\n"
        "}\n";

    u->colour_prog = esLoadProgram(colour_vs, colour_fs);
    u->tex_prog = esLoadProgram(tex_vs, tex_fs);
    if (!u->colour_prog || !u->tex_prog)
        return GL_FALSE;
    u->colour_mvp = glGetUniformLocation(u->colour_prog, "u_mvp");
    u->tex_mvp = glGetUniformLocation(u->tex_prog, "u_mvp");
    u->tex_sampler = glGetUniformLocation(u->tex_prog, "s_texture");
    u->nindices = esGenCube(1.0f, &u->vertices, &u->normals, &u->texcoords, &u->indices);

    glGenTextures(1, &u->tex);
    glBindTexture(GL_TEXTURE_2D, u->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RTT, RTT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &u->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, u->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, RTT, RTT);

    glGenFramebuffers(1, &u->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, u->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, u->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, u->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        esLogMessage("RTT_Cube: the framebuffer object is not complete\n");
        return GL_FALSE;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    u->angle = 30.0f;
    return GL_TRUE;
}

static void Update(ESContext *esContext, float deltaTime)
{
    UserData *u = esContext->userData;
    u->angle += deltaTime * 40.0f;
    if (u->angle >= 360.0f)
        u->angle -= 360.0f;
}

static void mvp(ESMatrix *out, float aspect, float angle, float tilt)
{
    ESMatrix persp, model;
    esMatrixLoadIdentity(&persp);
    esPerspective(&persp, 60.0f, aspect, 1.0f, 20.0f);
    esMatrixLoadIdentity(&model);
    esTranslate(&model, 0.0f, 0.0f, -3.0f);
    esRotate(&model, tilt, 1.0f, 0.0f, 0.0f);
    esRotate(&model, angle, 0.0f, 1.0f, 0.0f);
    esMatrixMultiply(out, &model, &persp);
}

static void Draw(ESContext *esContext)
{
    UserData *u = esContext->userData;
    ESMatrix m;

    /* The texture: a cube coloured by its normals, with depth. */
    glBindFramebuffer(GL_FRAMEBUFFER, u->fbo);
    glViewport(0, 0, RTT, RTT);
    glClearColor(0.15f, 0.15f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(u->colour_prog);
    mvp(&m, 1.0f, u->angle * 2.0f, 25.0f);
    glUniformMatrix4fv(u->colour_mvp, 1, GL_FALSE, &m.m[0][0]);
    glEnableVertexAttribArray(glGetAttribLocation(u->colour_prog, "a_position"));
    glVertexAttribPointer(glGetAttribLocation(u->colour_prog, "a_position"), 3, GL_FLOAT, GL_FALSE,
                          3 * sizeof(GLfloat), u->vertices);
    glEnableVertexAttribArray(glGetAttribLocation(u->colour_prog, "a_normal"));
    glVertexAttribPointer(glGetAttribLocation(u->colour_prog, "a_normal"), 3, GL_FLOAT, GL_FALSE,
                          3 * sizeof(GLfloat), u->normals);
    glDrawElements(GL_TRIANGLES, u->nindices, GL_UNSIGNED_INT, u->indices);
    glDisableVertexAttribArray(glGetAttribLocation(u->colour_prog, "a_normal"));

    /* The screen: a cube wearing that texture. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, esContext->width, esContext->height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(u->tex_prog);
    mvp(&m, (float)esContext->width / esContext->height, u->angle, -20.0f);
    glUniformMatrix4fv(u->tex_mvp, 1, GL_FALSE, &m.m[0][0]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, u->tex);
    glUniform1i(u->tex_sampler, 0);
    glEnableVertexAttribArray(glGetAttribLocation(u->tex_prog, "a_position"));
    glVertexAttribPointer(glGetAttribLocation(u->tex_prog, "a_position"), 3, GL_FLOAT, GL_FALSE,
                          3 * sizeof(GLfloat), u->vertices);
    glEnableVertexAttribArray(glGetAttribLocation(u->tex_prog, "a_texcoord"));
    glVertexAttribPointer(glGetAttribLocation(u->tex_prog, "a_texcoord"), 2, GL_FLOAT, GL_FALSE,
                          2 * sizeof(GLfloat), u->texcoords);
    glDrawElements(GL_TRIANGLES, u->nindices, GL_UNSIGNED_INT, u->indices);
    glDisableVertexAttribArray(glGetAttribLocation(u->tex_prog, "a_texcoord"));
    glDisable(GL_DEPTH_TEST);

    eglSwapBuffers(esContext->eglDisplay, esContext->eglSurface);
}

static void ShutDown(ESContext *esContext)
{
    UserData *u = esContext->userData;
    glDeleteFramebuffers(1, &u->fbo);
    glDeleteRenderbuffers(1, &u->depth);
    glDeleteTextures(1, &u->tex);
    glDeleteProgram(u->colour_prog);
    glDeleteProgram(u->tex_prog);
    free(u->vertices);
    free(u->normals);
    free(u->texcoords);
    free(u->indices);
}

int main(int argc, char *argv[])
{
    ESContext esContext;
    UserData userData;
    esInitContext(&esContext);
    esContext.userData = &userData;
    esCreateWindow(&esContext, "RTT_Cube", 320, 240, ES_WINDOW_RGB | ES_WINDOW_DEPTH);
    if (!Init(&esContext))
        return 1;
    esRegisterDrawFunc(&esContext, Draw);
    esRegisterUpdateFunc(&esContext, Update);
    esMainLoop(&esContext);
    ShutDown(&esContext);
    return 0;
}
