/*
 * Minimal <fenv.h> for m68k-amigaos + libnix, which has none.
 *
 * Mesa uses it only in nir_constant_expressions.c, to hold floating-point
 * exceptions while it folds constants (feholdexcept ... fesetenv). The 68881
 * runs with all exception traps disabled by default (FPCR enable byte 0), so
 * there is nothing to hold: these do nothing and report success.
 * Rounding is round-to-nearest, the 68881 default; only that mode is offered.
 */
#ifndef OGLES2_COMPAT_FENV_H
#define OGLES2_COMPAT_FENV_H

typedef unsigned long fenv_t;
typedef unsigned long fexcept_t;

#define FE_INVALID    0x80
#define FE_DIVBYZERO  0x10
#define FE_OVERFLOW   0x40
#define FE_UNDERFLOW  0x20
#define FE_INEXACT    0x08
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0

#define FE_DFL_ENV    ((const fenv_t *)0)

static inline int feholdexcept(fenv_t *e) { if (e) *e = 0; return 0; }
static inline int fesetenv(const fenv_t *e) { (void)e; return 0; }
static inline int fegetenv(fenv_t *e) { if (e) *e = 0; return 0; }
static inline int feupdateenv(const fenv_t *e) { (void)e; return 0; }
static inline int feclearexcept(int x) { (void)x; return 0; }
static inline int fetestexcept(int x) { (void)x; return 0; }
static inline int feraiseexcept(int x) { (void)x; return 0; }
static inline int fegetround(void) { return FE_TONEAREST; }
static inline int fesetround(int r) { return r == FE_TONEAREST ? 0 : -1; }

#endif
