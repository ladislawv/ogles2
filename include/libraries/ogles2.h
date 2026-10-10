#ifndef LIBRARIES_OGLES2_H
#define LIBRARIES_OGLES2_H
/*
 * ogles2.library for AmigaOS 3.x on PiStorm (Raspberry Pi 4, V3D 4.2):
 * names, versions, context tags and error codes.
 *
 * Tag NAMES follow AmigaOS 4's ogles2.library so that source written for it
 * compiles unchanged; their VALUES are this library's own (plan section 6.4).
 * The dispatch table is in <libraries/ogles2_dispatch.h> (generated).
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OGLES2NAME "ogles2.library"

/* Minimum library version a client opens (OpenLibrary). A client may define
 * a higher one on its compile line when it needs newer entry points. */
#ifndef OGLES2_VERSION
#define OGLES2_VERSION 1
#endif

/* Context creation and parameter tags (aglCreateContext2, aglSetParams2). */
#define OGLES2_CCT_MIN                      (TAG_USER + 0x4f4c0000UL)
#define OGLES2_CCT_WINDOW                   (OGLES2_CCT_MIN + 1)  /* struct Window *: render into it */
#define OGLES2_CCT_MODEID                   (OGLES2_CCT_MIN + 2)  /* ULONG: open a fullscreen screen */
#define OGLES2_CCT_DEPTH                    (OGLES2_CCT_MIN + 3)  /* ULONG: depth bits, 0 = none */
#define OGLES2_CCT_STENCIL                  (OGLES2_CCT_MIN + 4)  /* ULONG: stencil bits, 0 = none */
#define OGLES2_CCT_VSYNC                    (OGLES2_CCT_MIN + 5)  /* BOOL: wait for vertical blank */
#define OGLES2_CCT_SINGLE_GET_ERROR_MODE    (OGLES2_CCT_MIN + 6)  /* BOOL */
#define OGLES2_CCT_GET_WIDTH                (OGLES2_CCT_MIN + 7)  /* ULONG *: receives the width */
#define OGLES2_CCT_GET_HEIGHT               (OGLES2_CCT_MIN + 8)  /* ULONG *: receives the height */
#define OGLES2_CCT_BITMAP                   (OGLES2_CCT_MIN + 9)  /* struct BitMap *: render into it */
#define OGLES2_CCT_SHADER_COMPAT_PATCH      (OGLES2_CCT_MIN + 10) /* BOOL */
#define OGLES2_CCT_CONTEXT_FOR_MODEID       (OGLES2_CCT_MIN + 11) /* ULONG */
#define OGLES2_CCT_RESIZE_VIEWPORT          (OGLES2_CCT_MIN + 12) /* BOOL */
#define OGLES2_CCT_DEBUG_SHADER_LOG         (OGLES2_CCT_MIN + 13) /* BOOL */
#define OGLES2_CCT_SPIRV_OPLINES            (OGLES2_CCT_MIN + 14) /* accepted, ignored */
#define OGLES2_CCT_SPIRV_OPLINES_OFFSET     (OGLES2_CCT_MIN + 15) /* accepted, ignored */
#define OGLES2_CCT_SPIRV_OPTIMIZE           (OGLES2_CCT_MIN + 16) /* accepted, ignored */
#define OGLES2_CCT_SHARE_WITH               (OGLES2_CCT_MIN + 17) /* void *: share objects */
#define OGLES2_CCT_DETECT_UNINITIALIZED_GLSL_VARS (OGLES2_CCT_MIN + 18) /* accepted, ignored */
/* PiStorm-specific tags start here. */
#define OGLES2_CCT_PISTORM_MIN              (OGLES2_CCT_MIN + 0x1000)
/* BOOL, tests only: a context with no surface that does not need a GPU
 * (state only; nothing is drawn, aglSwapBuffers does nothing). Lets the API
 * tests run in an emulator and without opening windows. */
#define OGLES2_CCT_PISTORM_STATE_ONLY       (OGLES2_CCT_PISTORM_MIN + 1)
/* ULONG: size of the window ogles2 opens itself when none of WINDOW, BITMAP
 * and MODEID is given (default 640 x 480); with MODEID, the screen size
 * (default: the mode's own). */
#define OGLES2_CCT_PISTORM_WIDTH            (OGLES2_CCT_PISTORM_MIN + 2)
#define OGLES2_CCT_PISTORM_HEIGHT           (OGLES2_CCT_PISTORM_MIN + 3)
/* struct Window **, output (aglCreateContext2, aglSetParams2): the window
 * that receives input -- the one ogles2 opened, the application's own, or,
 * fullscreen, the backdrop window on ogles2's screen (its WScreen is the
 * screen); NULL for an application bitmap. */
#define OGLES2_CCT_PISTORM_GET_WINDOW       (OGLES2_CCT_PISTORM_MIN + 4)

/* Error codes written to aglCreateContext2's *errcode. */
#define OGLES2_CCE_OK              0
#define OGLES2_CCE_NOMEM           1  /* out of memory */
#define OGLES2_CCE_NO_GPU          2  /* V3D not found or not usable */
#define OGLES2_CCE_BAD_TAGS        3  /* contradictory or invalid tags */
#define OGLES2_CCE_NO_DISPLAY      4  /* window, screen or bitmap unusable */
#define OGLES2_CCE_UNSUPPORTED     5  /* a requested feature is not supported */
#define OGLES2_CCE_NOT_IMPLEMENTED 6  /* development builds: entry not written yet */

extern struct Library *OGLES2Base;

#ifdef __cplusplus
}
#endif

#endif
