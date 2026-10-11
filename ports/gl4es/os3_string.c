/*
 * memcmp for programs on gl4es (ports/gl4es): clib2's compares a byte at a
 * time, 2.4 us for 256 equal bytes on Emu68 (tests/perf/emuspeed), and gl4es
 * compares its state structs with it on every draw (fpe_program's ~300-byte
 * state, the uniform caches). Linked from libgl4es.a, which comes before the
 * C library: the program's and gl4es's calls (and clib2's own) use this one.
 *
 * MIT, as the rest of ports/gl4es.
 */
#include <stddef.h>

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    /* Even addresses: the m68k ABI aligns structs to 2 bytes, and the 68020
     * (and Emu68) read a long word at any even address. */
    if (((((unsigned long)x) | ((unsigned long)y)) & 1) == 0) {
        /* Equal long words skipped; the first difference is found below, a byte at a time. */
        const unsigned long *xl = (const unsigned long *)x, *yl = (const unsigned long *)y;
        while (n >= 16 && xl[0] == yl[0] && xl[1] == yl[1] && xl[2] == yl[2] && xl[3] == yl[3]) {
            xl += 4;
            yl += 4;
            n -= 16;
        }
        while (n >= 4 && *xl == *yl) {
            xl++;
            yl++;
            n -= 4;
        }
        x = (const unsigned char *)xl;
        y = (const unsigned char *)yl;
    }
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}
