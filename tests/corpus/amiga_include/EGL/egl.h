/* What the corpus programs take from EGL, on the Amiga: the types esUtil's
 * ESContext holds and eglSwapBuffers, which ends a frame of the corpus
 * harness (amiga_hooks.h renames it). There is no EGL on AmigaOS. */
#ifndef CORPUS_EGL_H
#define CORPUS_EGL_H

typedef void *EGLNativeWindowType;
typedef void *EGLNativeDisplayType;
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_FALSE 0
#define EGL_TRUE 1

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);

#endif
