/*
 * The few C library functions the library and v3dcore use. The library links
 * no C library (it is resident and has no startup code); GCC may also call
 * memcpy/memset for structure copies and clears.
 */
#include <stddef.h>

/* memcpy, memset and memcmp move long words while both addresses are
 * long-aligned: Emu68 runs about one 68k instruction in 3-5 ns, so a byte
 * at a time cost every draw microseconds (its 264-byte texture table alone,
 * M6's tests/perf/emuspeed). */
#define ALIGNED(a, b) ((((unsigned long)(a) | (unsigned long)(b)) & 3) == 0)

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (ALIGNED(d, s)) {
        unsigned long *dl = (unsigned long *)d;
        const unsigned long *sl = (const unsigned long *)s;
        /* movem: Emu68 takes one memory access per instruction dearly. */
        for (; n >= 16; n -= 16)
            __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t"
                             "movem.l %%d0-%%d3,(%1)\n\t"
                             "lea (16,%1),%1"
                             : "+a"(sl), "+a"(dl) : : "d0", "d1", "d2", "d3", "memory");
        for (; n >= 4; n -= 4)
            *dl++ = *sl++;
        d = (unsigned char *)dl;
        s = (const unsigned char *)sl;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    if (ALIGNED(d, 0)) {
        unsigned long v = (unsigned char)c, *dl = (unsigned long *)d;
        v |= v << 8;
        v |= v << 16;
        for (; n >= 16; n -= 16) {
            dl[0] = v;
            dl[1] = v;
            dl[2] = v;
            dl[3] = v;
            dl += 4;
        }
        for (; n >= 4; n -= 4)
            *dl++ = v;
        d = (unsigned char *)dl;
    }
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    if (ALIGNED(x, y)) {
        /* Equal long words skipped; the bytes of the first difference decide. */
        const unsigned long *xl = (const unsigned long *)x, *yl = (const unsigned long *)y;
        for (; n >= 4 && *xl == *yl; n -= 4) {
            xl++;
            yl++;
        }
        x = (const unsigned char *)xl;
        y = (const unsigned char *)yl;
    }
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}
