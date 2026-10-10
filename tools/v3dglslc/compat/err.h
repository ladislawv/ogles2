/*
 * BSD <err.h>, which libnix lacks. The V3D driver includes it; these print to
 * stderr like the BSD versions (err/errx do not exit here: a library must not
 * end the program; they log and return).
 */
#ifndef OGLES2_COMPAT_ERR_H
#define OGLES2_COMPAT_ERR_H

#include <stdarg.h>
#include <stdio.h>

static inline void vwarnx(const char *fmt, va_list ap)
{
    fputs("ogles2: ", stderr);
    if (fmt)
        vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static inline void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

#define warn warnx
#define err(code, ...) warnx(__VA_ARGS__)
#define errx(code, ...) warnx(__VA_ARGS__)

#endif
