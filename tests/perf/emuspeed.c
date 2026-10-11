/*
 * How fast Emu68 runs the kinds of 68k code a draw is made of (M6, plan
 * 7.1): ogles2's draw costs about 22 us however little it copies, so this
 * times its pieces alone, in nanoseconds, on the Pi's 1 MHz system timer.
 * Nothing to see.
 *   usage: emuspeed
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#define CLO (*(volatile ULONG *)(0xF2000000 + 0x3004))

static ULONG now(void)
{
    ULONG v = CLO;
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static ULONG le32(ULONG x)
{
    return (x >> 24) | ((x >> 8) & 0xff00UL) | ((x << 8) & 0xff0000UL) | (x << 24);
}

/* Kept out of line, as draw.c's helpers are across files. */
__attribute__((noinline)) ULONG call4(ULONG a, ULONG b, ULONG c, ULONG d)
{
    return a + b + c + d;
}

#define N 20000

static void report(const char *what, ULONG us, ULONG n, ULONG per)
{
    /* nanoseconds per `per` units of work */
    printf("%-44s %8lu ns\n", what, (unsigned long)((double)us * 1000.0 / ((double)n / per)));
}

int main(void)
{
    ULONG *src = AllocVec(65536, MEMF_FAST | MEMF_CLEAR);
    ULONG *dst = AllocVec(65536, MEMF_FAST | MEMF_CLEAR);
    UWORD *idx = AllocVec(4096, MEMF_FAST | MEMF_CLEAR);
    volatile ULONG sink = 0;
    ULONG t0, i, k;
    if (!src || !dst || !idx) {
        printf("no memory\n");
        return 20;
    }
    for (k = 0; k < 68; k++)
        idx[k] = (UWORD)((k * 7) & 255);

    t0 = now();
    for (i = 0; i < N; i++)
        sink += now();
    report("one timer read", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        const UWORD *ui = idx;
        ULONG *d = dst;
        for (k = 68; k > 0; k--)
            *d++ = src[*ui++];
    }
    report("68 indexed words (a uniform stream)", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        const ULONG *s = src;
        ULONG *d = dst;
        for (k = 100; k > 0; k--)
            *d++ = le32(*s++);
    }
    report("100 words byte-swapped (400 vertex bytes)", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        const ULONG *s = src;
        ULONG *d = dst;
        for (k = 100; k > 0; k--)
            *d++ = *s++;
    }
    report("100 words copied", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N * 10; i++)
        sink += call4(i, 1, 2, 3);
    report("a call with 4 arguments", now() - t0, N * 10, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        volatile UBYTE *p = (UBYTE *)dst + (i & 1023);
        for (k = 0; k < 16; k++)
            p[k] = 0;
    }
    report("16 bytes zeroed one at a time (a packet)", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N * 10; i++)
        sink += (i & 7) == 3 ? 1 : (i & 7) == 5 ? 2 : 0;
    report("a loop pass with two compares", now() - t0, N * 10, 1);

    /* movem: four longs a load and a store. */
    t0 = now();
    for (i = 0; i < N; i++) {
        const ULONG *s = src;
        ULONG *d = dst;
        for (k = 25; k > 0; k--) {
            __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t"
                             "movem.l %%d0-%%d3,(%1)\n\t"
                             "lea (16,%1),%1"
                             : "+a"(s), "+a"(d) : : "d0", "d1", "d2", "d3", "memory");
        }
    }
    report("100 words copied with movem", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        const ULONG *s = src;
        ULONG *d = dst;
        for (k = 25; k > 0; k--) {
            __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t"
                             "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t"
                             "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t"
                             "ror.w #8,%%d2\n\tswap %%d2\n\tror.w #8,%%d2\n\t"
                             "ror.w #8,%%d3\n\tswap %%d3\n\tror.w #8,%%d3\n\t"
                             "movem.l %%d0-%%d3,(%1)\n\t"
                             "lea (16,%1),%1"
                             : "+a"(s), "+a"(d) : : "d0", "d1", "d2", "d3", "memory");
        }
    }
    report("100 words byte-swapped with movem", now() - t0, N, 1);

    /* The C library's (clib2's): gl4es compares and copies its state
     * structs with these on every draw. */
    t0 = now();
    for (i = 0; i < N; i++)
        sink += memcmp(src, dst, 256);
    report("clib2 memcmp, 256 equal bytes", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        memcpy(dst, src, 256);
        sink += dst[i & 63];
    }
    report("clib2 memcpy, 256 bytes", now() - t0, N, 1);

    t0 = now();
    for (i = 0; i < N; i++) {
        memset(dst, (int)i, 256);
        sink += dst[i & 63];
    }
    report("clib2 memset, 256 bytes", now() - t0, N, 1);

    printf("(%lu)\n", (unsigned long)sink);
    FreeVec(src);
    FreeVec(dst);
    FreeVec(idx);
    return 0;
}
