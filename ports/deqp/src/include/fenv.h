/*
 * dEQP-GLES2 on AmigaOS 3 (ports/deqp): the rounding-mode half of <fenv.h>,
 * which libnix lacks (delibs' deMath.c sets the mode for its float tests),
 * on the 68881/68882's FPCR: its rounding-mode bits are 4 and 5 (Emu68
 * honours them). The precision bits are kept.
 */
#ifndef DEQP_AMIGA_FENV_H
#define DEQP_AMIGA_FENV_H

#define FE_TONEAREST 0x00
#define FE_TOWARDZERO 0x10
#define FE_DOWNWARD 0x20
#define FE_UPWARD 0x30

static inline int fegetround(void)
{
    unsigned long c;
    __asm__ volatile("fmove.l %%fpcr,%0" : "=d"(c));
    return (int)(c & 0x30);
}

static inline int fesetround(int mode)
{
    unsigned long c;
    if (mode & ~0x30)
        return -1;
    __asm__ volatile("fmove.l %%fpcr,%0" : "=d"(c));
    c = (c & ~0x30UL) | (unsigned long)mode;
    __asm__ volatile("fmove.l %0,%%fpcr" : : "d"(c));
    return 0;
}

#endif
