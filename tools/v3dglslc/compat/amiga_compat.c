/*
 * Functions Mesa's compiler code calls that libnix does not provide.
 * Spike versions: enough to link and to behave safely, not complete.
 */
#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <proto/exec.h>
#include "amiga_compat.h"

/* libnix's libm has log2 and exp but no log2f/exp2f. Computed in double and
 * rounded once. exp2f can differ from glibc in the last bit in rare cases,
 * which matters only when NIR folds exp2 of a constant (plan section 8.4). */
float log2f(float x)
{
    return (float)log2((double)x);
}

float exp2f(float x)
{
    return (float)exp((double)x * 0.69314718055994530942);
}

/* GL_ARB_shading_language_include (src/mesa/main/shaderapi.c). GLES2 has no
 * #include, so there are never any include paths. */
struct gl_shared_state;
struct gl_context;
size_t _mesa_get_shader_include_cursor(struct gl_shared_state *shared)
{
    (void)shared;
    return 0;
}

void _mesa_set_shader_include_cursor(struct gl_shared_state *shared, size_t cursor)
{
    (void)shared;
    (void)cursor;
}

const char *_mesa_lookup_shader_include(struct gl_context *ctx, char *path, bool error_check)
{
    (void)ctx;
    (void)path;
    (void)error_check;
    return NULL;
}

/* util/log.c writes a log file only when MESA_LOG_FILE asks for a temp file. */
int mkstemps(char *template_name, int suffixlen)
{
    (void)template_name;
    (void)suffixlen;
    errno = ENOSYS;
    return -1;
}

/* libstdc++ (system_error.o) calls the XSI strerror_r; libnix has only strerror. */
int __xpg_strerror_r(int err, char *buf, size_t len)
{
    const char *s = strerror(err);
    size_t n = strlen(s);
    if (len == 0)
        return ERANGE;
    if (n >= len)
        n = len - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return 0;
}

/* GCC does not inline the 1-byte compare-and-swap for this target. Forbid()
 * makes it atomic against other tasks; nothing here runs in interrupts. */
unsigned char __sync_val_compare_and_swap_1(volatile void *ptr, unsigned char expected,
                                            unsigned char desired)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    unsigned char old;
    Forbid();
    old = *p;
    if (old == expected)
        *p = desired;
    Permit();
    return old;
}

/* Used by Mesa's debug printing (util/memstream.c). Without it the callers
 * report failure and print nothing; a real version can come with logging. */
FILE *open_memstream(char **bufp, size_t *sizep)
{
    (void)bufp;
    (void)sizep;
    errno = ENOSYS;
    return NULL;
}

#ifdef HAVE_PTHREAD
/* util/cnd_monotonic.c asks for CLOCK_MONOTONIC condition variables. The AROS
 * pthreads port has one clock; accepting the request is harmless for the
 * single-threaded compiler. */
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
    (void)attr;
    (void)clock_id;
    return 0;
}

/* util/u_thread.c: per-thread CPU time is unavailable. */
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id)
{
    (void)thread;
    (void)clock_id;
    return ENOENT;
}
#endif
