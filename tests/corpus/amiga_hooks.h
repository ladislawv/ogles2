/* Force-included (-include) into the corpus programs built for the Amiga:
 * the GL calls go through ogles2.library's dispatch table (proto/ogles2.h),
 * and the programs' own eglSwapBuffers calls end a frame of the harness. */
#include <proto/ogles2.h>
#define eglSwapBuffers corpus_eglSwapBuffers
/* The same pseudo-random numbers on both (corpus.c): the C libraries differ. */
#define rand corpus_rand
#define srand corpus_srand
