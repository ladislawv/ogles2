/*
 * ogles2.library example 2: a textured cube turning in a window, with a
 * depth buffer.
 *
 * Shows what the triangle does not: a depth buffer (OGLES2_CCT_DEPTH), an
 * index buffer, a texture with mipmaps made by glGenerateMipmap, a matrix
 * uniform, and a window the user may resize: at IDCMP_NEWSIZE the program
 * gives ogles2 the window again (aglSetParams2 with OGLES2_CCT_WINDOW), which
 * resizes the surface, and sets the viewport to the new size.
 * Close the window, press Esc or Ctrl-C to quit.
 *
 * Build (see the Makefile):
 *   m68k-amigaos-gcc -O2 -fno-builtin -I../include cube.c -L../lib -logles2 -lm881 -o cube
 */
#include <stdio.h>
#include <math.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/ogles2.h>
#include "cube_shaders.h"

struct IntuitionBase *IntuitionBase;

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("shader: %s\n", log);
    }
    return s;
}

static GLuint link_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, cube_vs), fs = compile(GL_FRAGMENT_SHADER, cube_fs);
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_texcoord");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* A 64 x 64 checkerboard, two colours per square edge, with mipmaps. */
static GLuint make_texture(void)
{
    static GLubyte pixels[64 * 64 * 4];
    GLuint t;
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            GLubyte *p = pixels + (y * 64 + x) * 4;
            int on = ((x >> 3) ^ (y >> 3)) & 1;
            p[0] = on ? 240 : 40;
            p[1] = on ? 200 : 60;
            p[2] = on ? 60 : 160;
            p[3] = 255;
        }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}

/* m = a * b, 4 x 4, column-major as glUniformMatrix4fv takes them. */
static void mul(GLfloat *m, const GLfloat *a, const GLfloat *b)
{
    GLfloat r[16];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            GLfloat s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + j] * b[i * 4 + k];
            r[i * 4 + j] = s;
        }
    for (i = 0; i < 16; i++)
        m[i] = r[i];
}

/* Perspective (60 degrees vertically) times a translation 4 units away
 * times rotations about y and x. */
static void mvp(GLfloat *m, float aspect, float ay, float ax)
{
    const float f = 1.7320508f, n = 1.0f, fa = 10.0f; /* f = 1 / tan(30 degrees) */
    GLfloat p[16] = {0}, t[16] = {0}, ry[16] = {0}, rx[16] = {0};
    float cy = cos(ay), sy = sin(ay), cx = cos(ax), sx = sin(ax);
    p[0] = f / aspect;
    p[5] = f;
    p[10] = (fa + n) / (n - fa);
    p[11] = -1.0f;
    p[14] = 2.0f * fa * n / (n - fa);
    t[0] = t[5] = t[10] = t[15] = 1.0f;
    t[14] = -4.0f;
    ry[0] = cy, ry[2] = -sy, ry[5] = 1.0f, ry[8] = sy, ry[10] = cy, ry[15] = 1.0f;
    rx[0] = 1.0f, rx[5] = cx, rx[6] = sx, rx[9] = -sx, rx[10] = cx, rx[15] = 1.0f;
    mul(m, p, t);
    mul(m, m, ry);
    mul(m, m, rx);
}

/* Window events: FALSE when the user wants to quit; *resized when the
 * window changed size. */
static BOOL events(struct Window *w, BOOL *resized)
{
    struct IntuiMessage *m;
    BOOL go = TRUE;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        go = FALSE;
    while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
        if (m->Class == IDCMP_CLOSEWINDOW || (m->Class == IDCMP_VANILLAKEY && m->Code == 27))
            go = FALSE;
        if (m->Class == IDCMP_NEWSIZE)
            *resized = TRUE;
        ReplyMsg((struct Message *)m);
    }
    return go;
}

int main(void)
{
    /* Each face: four corners (x, y, z, s, t), its own texture coordinates. */
    static const GLfloat vertices[] = {
        -1, -1,  1, 0, 0,   1, -1,  1, 1, 0,   1,  1,  1, 1, 1,  -1,  1,  1, 0, 1, /* front */
         1, -1, -1, 0, 0,  -1, -1, -1, 1, 0,  -1,  1, -1, 1, 1,   1,  1, -1, 0, 1, /* back */
        -1, -1, -1, 0, 0,  -1, -1,  1, 1, 0,  -1,  1,  1, 1, 1,  -1,  1, -1, 0, 1, /* left */
         1, -1,  1, 0, 0,   1, -1, -1, 1, 0,   1,  1, -1, 1, 1,   1,  1,  1, 0, 1, /* right */
        -1,  1,  1, 0, 0,   1,  1,  1, 1, 0,   1,  1, -1, 1, 1,  -1,  1, -1, 0, 1, /* top */
        -1, -1, -1, 0, 0,   1, -1, -1, 1, 0,   1, -1,  1, 1, 1,  -1, -1,  1, 0, 1, /* bottom */
    };
    static GLushort indices[36];
    struct Window *win = NULL;
    void *ctx = NULL;
    ULONG err = 0, width = 0, height = 0;
    GLuint prog, buf[2], tex;
    GLint mvp_loc;
    GLfloat m[16];
    float a = 0.0f;
    BOOL resized = FALSE;
    int i, rc = 20;

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39))) {
        printf("needs intuition.library 39\n");
        return 20;
    }
    if (!OGLES2Open()) {
        printf("needs %s 1.3 or newer: in LIBS:, or in this program's drawer or its libs/\n"
               "(an older one that another program is using cannot be replaced)\n", OGLES2NAME);
        goto out;
    }
    win = OpenWindowTags(NULL, WA_Title, (ULONG) "ogles2: cube", WA_InnerWidth, 400, WA_InnerHeight, 300,
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE,
                         WA_SizeBBottom, TRUE, WA_MinWidth, 100, WA_MinHeight, 80, WA_MaxWidth, -1,
                         WA_MaxHeight, -1, WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_NEWSIZE, TAG_DONE);
    if (!win) {
        printf("cannot open a window\n");
        goto out;
    }
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_STENCIL, 0,
                                OGLES2_CCT_GET_WIDTH, (ULONG)&width, OGLES2_CCT_GET_HEIGHT, (ULONG)&height,
                                TAG_DONE);
    if (!ctx) {
        printf("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
        goto out;
    }
    aglMakeCurrent(ctx);
    if (!(prog = link_program()))
        goto out;
    mvp_loc = glGetUniformLocation(prog, "u_mvp");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    tex = make_texture();

    for (i = 0; i < 6; i++) { /* two triangles per face */
        indices[i * 6 + 0] = i * 4 + 0;
        indices[i * 6 + 1] = i * 4 + 1;
        indices[i * 6 + 2] = i * 4 + 2;
        indices[i * 6 + 3] = i * 4 + 0;
        indices[i * 6 + 4] = i * 4 + 2;
        indices[i * 6 + 5] = i * 4 + 3;
    }
    glGenBuffers(2, buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)(3 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glClearColor(0.15f, 0.15f, 0.2f, 1.0f);

    while (events(win, &resized)) {
        if (resized) {
            /* The surface takes the window's new inner size; the viewport
             * is the program's to set. */
            aglSetParamsTags2(OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_GET_WIDTH, (ULONG)&width,
                              OGLES2_CCT_GET_HEIGHT, (ULONG)&height, TAG_DONE);
            glViewport(0, 0, width, height);
            resized = FALSE;
        }
        mvp(m, height ? (float)width / height : 1.0f, a, a * 0.7f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, m);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, (const void *)0);
        aglSwapBuffers();
        a += 0.02f;
    }
    glDeleteBuffers(2, buf);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    rc = 0;
out:
    if (ctx)
        aglDestroyContext(ctx);
    if (win)
        CloseWindow(win);
    OGLES2Close();
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
