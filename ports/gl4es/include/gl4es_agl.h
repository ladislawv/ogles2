/*
 * The agl* functions of gl4es on AmigaOS 3 (ports/gl4es, agl_os3.c): a
 * program using desktop OpenGL through gl4es includes <GL/gl.h> (gl4es's)
 * and this, and links -lgl4es -logles2 -lm881. The context tags and error
 * codes are ogles2.library's (libraries/ogles2.h).
 */
#ifndef GL4ES_AGL_H
#define GL4ES_AGL_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <libraries/ogles2.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BitMap;

void *aglCreateContext2(ULONG *errcode, struct TagItem *tags);
void *aglCreateContextTags2(ULONG *errcode, ...);
void aglDestroyContext(void *context);
void aglMakeCurrent(void *context);
void aglSwapBuffers(void);
void aglSetParams2(struct TagItem *tags);
void aglSetParamsTags2(ULONG tag1, ...);
void aglSetBitmap(struct BitMap *bitmap);
void *aglGetProcAddress(const char *name);

#ifdef __cplusplus
}
#endif

#endif
