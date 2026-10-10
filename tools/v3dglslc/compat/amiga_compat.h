/*
 * Force-included (-include) into every Mesa file built for m68k-amigaos + libnix.
 *
 * libnix's own headers (which shadow newlib's in -mcrt=nix* builds) omit the
 * prototypes of several functions libnix does implement. Declaring them here
 * keeps Mesa's sources unchanged. Functions libnix lacks are implemented in
 * amiga_compat.c.
 */
#ifndef OGLES2_AMIGA_COMPAT_H
#define OGLES2_AMIGA_COMPAT_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif

/* AmigaOS has no exec(), so close-on-exec has no meaning; Mesa's drm.h uses it.
 * (Not via <fcntl.h>: newlib's defines FTRUNC, which clashes with V3D's
 * FTRUNC instruction in src/broadcom/compiler.) newlib never defines it. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* In libnix, prototype missing from its headers. */
char *strndup(const char *s, size_t n);
int vasprintf(char **strp, const char *fmt, va_list ap);
int nanosleep(const struct timespec *req, struct timespec *rem);

/* Not in libnix: amiga_compat.c. */
FILE *open_memstream(char **bufp, size_t *sizep);

/* Mesa util/log.c uses it without including util/u_process.h on this platform. */
const char *util_get_process_name(void);

#ifdef HAVE_PTHREAD
#include <pthread.h>
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id);
#endif

#ifdef __cplusplus
}
#endif

#endif
