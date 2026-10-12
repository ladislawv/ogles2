/*
 * ports/ioq3-gl2: what renderergl2's qgl.h takes from SDL_opengl.h, the GL
 * types, constants and APIENTRYP. The declarations come from the desktop GL
 * headers gl4es ships (headers only: nothing of gl4es is linked; every GL
 * call goes through the qgl table, amiga_glimp_gl2.c).
 */
#ifndef IOQ3_GL2_SDL_OPENGL_H
#define IOQ3_GL2_SDL_OPENGL_H
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#endif
