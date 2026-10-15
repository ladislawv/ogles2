/*
 * ogles2.library example 1: a coloured triangle turning in a window.
 *
 * The program opens its own window (with a close gadget) and gives it to
 * ogles2 with OGLES2_CCT_WINDOW; ogles2 renders into it at each
 * aglSwapBuffers. The vertices go into a buffer object once; each frame
 * only sets the angle. Close the window, press Esc or Ctrl-C to quit.
 *
 * Build (see the Makefile):
 *   m68k-amigaos-gcc -O2 -I../include triangle.c -L../lib -logles2 -o triangle
 */
#include <stdio.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/ogles2.h>
#include "triangle_shaders.h"

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

/* The program from the two shaders, its attributes at locations 0 and 1. */
static GLuint link_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, triangle_vs), fs = compile(GL_FRAGMENT_SHADER, triangle_fs);
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_color");
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

/* Window events: FALSE when the user wants to quit. */
static BOOL events(struct Window *w)
{
    struct IntuiMessage *m;
    BOOL go = TRUE;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        go = FALSE;
    while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
        if (m->Class == IDCMP_CLOSEWINDOW || (m->Class == IDCMP_VANILLAKEY && m->Code == 27))
            go = FALSE;
        ReplyMsg((struct Message *)m);
    }
    return go;
}

int main(void)
{
    /* x, y, then r, g, b, for each corner */
    static const GLfloat vertices[] = {
        0.0f,   0.7f,  1.0f, 0.2f, 0.2f,
        -0.6f, -0.5f,  0.2f, 1.0f, 0.2f,
        0.6f,  -0.5f,  0.2f, 0.2f, 1.0f,
    };
    struct Window *win = NULL;
    void *ctx = NULL;
    ULONG err = 0;
    GLuint prog, vbo;
    GLint angle_loc;
    float angle = 0.0f;
    int rc = 20;

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39))) {
        printf("needs intuition.library 39\n");
        return 20;
    }
    if (!OGLES2Open()) {
        printf("needs %s 1.3 or newer: in LIBS:, or in this program's drawer or its libs/\n"
               "(an older one that another program is using cannot be replaced)\n", OGLES2NAME);
        goto out;
    }
    win = OpenWindowTags(NULL, WA_Title, (ULONG) "ogles2: triangle", WA_InnerWidth, 320, WA_InnerHeight, 240,
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY, TAG_DONE);
    if (!win) {
        printf("cannot open a window\n");
        goto out;
    }
    /* No depth buffer: one triangle needs none. */
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_WINDOW, (ULONG)win, OGLES2_CCT_DEPTH, 0, OGLES2_CCT_STENCIL, 0,
                                TAG_DONE);
    if (!ctx) {
        printf("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
        goto out;
    }
    aglMakeCurrent(ctx);
    printf("%s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (!(prog = link_program()))
        goto out;
    angle_loc = glGetUniformLocation(prog, "u_angle");

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glUseProgram(prog);
    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);

    while (events(win)) {
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(angle_loc, angle);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        aglSwapBuffers();
        angle += 0.02f;
    }
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    rc = 0;
out:
    if (ctx)
        aglDestroyContext(ctx);
    if (win)
        CloseWindow(win);
    OGLES2Close(); /* safe whether or not it opened */
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
