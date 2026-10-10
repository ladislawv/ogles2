/* Force-included (-include) into the corpus programs built for the PC:
 * routes their shader sources to corpus_host.c's recorder. */
#define glShaderSource corpus_glShaderSource
#define glLinkProgram corpus_glLinkProgram
#define eglSwapBuffers corpus_eglSwapBuffers
/* The same pseudo-random numbers on both (corpus.c): the C libraries differ. */
#define rand corpus_rand
#define srand corpus_srand
