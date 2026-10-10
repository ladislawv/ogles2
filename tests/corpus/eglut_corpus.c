/*
 * Mesa demos' eglut (src/egl/eglut) on the corpus harness, for es2gears:
 * one window, CORPUS_FRAMES frames, program time from the fixed step.
 */
#include <stdio.h>
#include <stdlib.h>
#include "eglut.h"
#include "corpus.h"

static int win_w = 300, win_h = 300;
static EGLUTidleCB idle_cb;
static EGLUTreshapeCB reshape_cb;
static EGLUTdisplayCB display_cb;

void eglutInitAPIMask(int mask)
{
    (void)mask; /* OpenGL ES 2.0 is all there is */
}

void eglutInitWindowSize(int width, int height)
{
    win_w = width;
    win_h = height;
}

void eglutInit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

int eglutGet(int state)
{
    return state == EGLUT_ELAPSED_TIME ? (int)corpus_time_ms() : -1;
}

void eglutIdleFunc(EGLUTidleCB func)
{
    idle_cb = func;
}

void eglutPostRedisplay(void)
{
}

int eglutCreateWindow(const char *title)
{
    if (!corpus_open(title, win_w, win_h, 1, 0)) /* eglut asks for a depth buffer */
        exit(1);
    return 1;
}

void eglutDestroyWindow(int win)
{
    (void)win;
}

int eglutGetWindowWidth(void)
{
    return win_w;
}

int eglutGetWindowHeight(void)
{
    return win_h;
}

void eglutDisplayFunc(EGLUTdisplayCB func)
{
    display_cb = func;
}

void eglutReshapeFunc(EGLUTreshapeCB func)
{
    reshape_cb = func;
}

void eglutKeyboardFunc(EGLUTkeyboardCB func)
{
    (void)func;
}

void eglutSpecialFunc(EGLUTspecialCB func)
{
    (void)func;
}

void eglutMainLoop(void)
{
    int running = 1;
    if (reshape_cb)
        reshape_cb(win_w, win_h);
    while (running) {
        if (idle_cb)
            idle_cb();
        if (display_cb)
            display_cb();
        running = corpus_frame_done();
    }
    corpus_close();
}
