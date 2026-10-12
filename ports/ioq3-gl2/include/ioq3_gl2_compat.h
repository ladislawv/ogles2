/*
 * ports/ioq3-gl2: force-included in the renderer's files. Upstream
 * renderergl2 (2026) against the Amiga engine's older qcommon headers:
 * the macros upstream's q_shared.h has and the engine's lacks.
 */
#ifndef IOQ3_GL2_COMPAT_H
#define IOQ3_GL2_COMPAT_H
#ifndef Q_PRINTF_FUNC
#define Q_PRINTF_FUNC(fmt, va) __attribute__((format(printf, fmt, va)))
#endif
#ifndef Q_NO_RETURN
#define Q_NO_RETURN __attribute__((noreturn))
#endif
#ifndef Q_UNUSED_VAR
#define Q_UNUSED_VAR __attribute__((unused))
#endif
#ifndef Q_ALIGN
#define Q_ALIGN(x) __attribute__((aligned(x)))
#endif
#ifndef LERP
#define LERP(a, b, w) ((a) * (1.0f - (w)) + (b) * (w))
#endif
#ifndef AVI_LINE_PADDING
#define AVI_LINE_PADDING 4 /* qcommon.h upstream: cl_avi.c's row padding */
#endif
#ifndef QuatCopy
#define QuatCopy(a, b) ((b)[0] = (a)[0], (b)[1] = (a)[1], (b)[2] = (a)[2], (b)[3] = (a)[3])
#endif
typedef float quat_t[4]; /* vec_t quat_t[4] upstream; vec_t is float */
#endif
