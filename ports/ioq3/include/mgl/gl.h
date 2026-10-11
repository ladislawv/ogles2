/*
 * ioquake3 for AmigaOS 3 on gl4es (ports/ioq3): stands in for MiniGL's
 * <mgl/gl.h>, which the game's renderer and Amiga code include. Desktop GL
 * is gl4es's; the few MiniGL names the game uses map to their gl4es
 * equivalents or to nothing (ports/ioq3/README.md).
 *
 * GPL v2 or later, as ioquake3.
 */
#ifndef IOQ3_GL4ES_MGL_GL_H
#define IOQ3_GL4ES_MGL_GL_H

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES /* the extension functions too (glLockArraysEXT) */
#endif
#include <GL/gl.h>
#include <GL/glext.h>

/* MiniGL's capability for fast perspective: no such thing on GLES2. Disabling
 * GL_DITHER instead is harmless and raises no GL error. */
#define MGL_PERSPECTIVE_MAPPING GL_DITHER

/* MiniGL tuning calls with no GLES2 meaning. */
#define mglMinTriArea(area) ((void)(area))
#define mglSetZOffset(offset) ((void)(offset))

/* MiniGL's names for GL_EXT_compiled_vertex_array. */
#define glLockArrays glLockArraysEXT
#define glUnlockArrays glUnlockArraysEXT

/* The renderer's draws through amiga_glimp.c, which times them (gl4es and
 * ogles2.library together; reported at GLimp_Shutdown). */
void ioq3_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void ioq3_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void ioq3_glEnd(void);
void ioq3_glLockArraysEXT(GLint first, GLsizei count);
void ioq3_glUnlockArraysEXT(void);
#ifndef IOQ3_GL_PROFILE_IMPL
#define glDrawElements ioq3_glDrawElements
#define glDrawArrays ioq3_glDrawArrays
#define glEnd ioq3_glEnd
#define glLockArraysEXT ioq3_glLockArraysEXT
#define glUnlockArraysEXT ioq3_glUnlockArraysEXT

/* The state calls the renderer makes every frame, timed together (gl4es's
 * own work outside the draws; amiga_glimp.c reports it). A function-like
 * macro does not expand inside its own expansion: the call is gl4es's. */
extern unsigned long ioq3_state_us, ioq3_state_calls, ioq3_t0, ioq3_fn_us[32], ioq3_fn_calls[32];
static __inline__ unsigned long ioq3_now(void)
{
    unsigned long v = *(volatile unsigned long *)(0xF2000000 + 0x3004);
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
/* Each wrapped call's time, in total and by function (k, IOQ3_TIMED_NAMES). */
#define IOQ3_TIMED(k, call) \
    ((void)(ioq3_t0 = ioq3_now()), call, \
     (void)(ioq3_t0 = ioq3_now() - ioq3_t0, ioq3_state_us += ioq3_t0, ioq3_state_calls++, \
            ioq3_fn_us[k] += ioq3_t0, ioq3_fn_calls[k]++))
#define IOQ3_TIMED_NAMES "glEnable glDisable glEnableClientState glDisableClientState glTexCoordPointer glColorPointer glVertexPointer glBindTexture glActiveTextureARB glClientActiveTextureARB glMatrixMode glLoadMatrixf glLoadIdentity glDepthRange glDepthMask glDepthFunc glAlphaFunc glBlendFunc glCullFace glTexEnvi glTexEnvf glColor4f glPolygonOffset glShadeModel"
#define glEnable(a) IOQ3_TIMED(0, glEnable(a))
#define glDisable(a) IOQ3_TIMED(1, glDisable(a))
#define glEnableClientState(a) IOQ3_TIMED(2, glEnableClientState(a))
#define glDisableClientState(a) IOQ3_TIMED(3, glDisableClientState(a))
#define glTexCoordPointer(a, b, c, d) IOQ3_TIMED(4, glTexCoordPointer(a, b, c, d))
#define glColorPointer(a, b, c, d) IOQ3_TIMED(5, glColorPointer(a, b, c, d))
#define glVertexPointer(a, b, c, d) IOQ3_TIMED(6, glVertexPointer(a, b, c, d))
#define glBindTexture(a, b) IOQ3_TIMED(7, glBindTexture(a, b))
#define glActiveTextureARB(a) IOQ3_TIMED(8, glActiveTextureARB(a))
#define glClientActiveTextureARB(a) IOQ3_TIMED(9, glClientActiveTextureARB(a))
#define glMatrixMode(a) IOQ3_TIMED(10, glMatrixMode(a))
#define glLoadMatrixf(a) IOQ3_TIMED(11, glLoadMatrixf(a))
#define glLoadIdentity() IOQ3_TIMED(12, glLoadIdentity())
#define glDepthRange(a, b) IOQ3_TIMED(13, glDepthRange(a, b))
#define glDepthMask(a) IOQ3_TIMED(14, glDepthMask(a))
#define glDepthFunc(a) IOQ3_TIMED(15, glDepthFunc(a))
#define glAlphaFunc(a, b) IOQ3_TIMED(16, glAlphaFunc(a, b))
#define glBlendFunc(a, b) IOQ3_TIMED(17, glBlendFunc(a, b))
#define glCullFace(a) IOQ3_TIMED(18, glCullFace(a))
#define glTexEnvi(a, b, c) IOQ3_TIMED(19, glTexEnvi(a, b, c))
#define glTexEnvf(a, b, c) IOQ3_TIMED(20, glTexEnvf(a, b, c))
#define glColor4f(a, b, c, d) IOQ3_TIMED(21, glColor4f(a, b, c, d))
#define glPolygonOffset(a, b) IOQ3_TIMED(22, glPolygonOffset(a, b))
#define glShadeModel(a) IOQ3_TIMED(23, glShadeModel(a))
#endif

/* The mouse pointer of the game's window (amiga_glimp.c, ports/ioq3). */
void mglSetPointer(void);
void mglClearPointer(void);

#endif
