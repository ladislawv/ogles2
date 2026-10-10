/*
 * The OpenGL ES 2.0 Programming Guide's esUtil framework (Common/esUtil.c's
 * window and loop part) on the corpus harness. The book's esShader.c,
 * esShapes.c and esTransform.c are used as they are.
 *
 * esMainLoop runs CORPUS_FRAMES frames with a fixed time step; a sample
 * that calls eglSwapBuffers in its draw function ends the frame there,
 * otherwise the loop does (the book's loop swaps a second time, which
 * shows an undefined back buffer on the PC as well: not repeated).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "esUtil.h"
#include "corpus.h"

static int swapped, running = 1;

EGLBoolean corpus_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    (void)surface;
    swapped = 1;
    running = corpus_frame_done();
    return EGL_TRUE;
}

void ESUTIL_API esInitContext(ESContext *esContext)
{
    if (esContext)
        memset(esContext, 0, sizeof(ESContext));
}

GLboolean ESUTIL_API esCreateWindow(ESContext *esContext, const char *title, GLint width, GLint height,
                                    GLuint flags)
{
    if (!esContext)
        return GL_FALSE;
    esContext->width = width;
    esContext->height = height;
    if (!corpus_open(title, width, height, (flags & ES_WINDOW_DEPTH) != 0,
                     (flags & ES_WINDOW_STENCIL) != 0))
        return GL_FALSE;
    atexit(corpus_close);
    return GL_TRUE;
}

void ESUTIL_API esMainLoop(ESContext *esContext)
{
    while (running) {
        swapped = 0;
        if (esContext->updateFunc)
            esContext->updateFunc(esContext, corpus_step_s());
        if (esContext->drawFunc)
            esContext->drawFunc(esContext);
        if (!swapped)
            running = corpus_frame_done();
    }
}

void ESUTIL_API esRegisterDrawFunc(ESContext *esContext, void(ESCALLBACK *drawFunc)(ESContext *))
{
    esContext->drawFunc = drawFunc;
}

void ESUTIL_API esRegisterUpdateFunc(ESContext *esContext,
                                     void(ESCALLBACK *updateFunc)(ESContext *, float))
{
    esContext->updateFunc = updateFunc;
}

void ESUTIL_API esRegisterKeyFunc(ESContext *esContext,
                                  void(ESCALLBACK *keyFunc)(ESContext *, unsigned char, int, int))
{
    esContext->keyFunc = keyFunc;
}

void ESUTIL_API esLogMessage(const char *formatStr, ...)
{
    va_list params;
    va_start(params, formatStr);
    vprintf(formatStr, params);
    va_end(params);
}

/* The book's loader, as it is: 24-bit uncompressed TGA, bottom row first
 * (which is GL's order), BGR bytes. */
char *ESUTIL_API esLoadTGA(char *fileName, int *width, int *height)
{
    char *buffer = NULL;
    FILE *f;
    unsigned char tgaheader[12];
    unsigned char attributes[6];
    unsigned int imagesize;

    f = fopen(fileName, "rb");
    if (f == NULL)
        return NULL;
    if (fread(&tgaheader, sizeof(tgaheader), 1, f) == 0) {
        fclose(f);
        return NULL;
    }
    if (fread(attributes, sizeof(attributes), 1, f) == 0) {
        fclose(f);
        return 0;
    }
    *width = attributes[1] * 256 + attributes[0];
    *height = attributes[3] * 256 + attributes[2];
    imagesize = attributes[4] / 8 * *width * *height;
    buffer = malloc(imagesize);
    if (buffer == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(buffer, 1, imagesize, f) != imagesize) {
        free(buffer);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buffer;
}
