/*
 * The corpus harness (plan section 13, M2 exit): runs unmodified GLES2
 * programs (es2gears, the OpenGL ES 2.0 Programming Guide samples) for a
 * fixed number of frames at a fixed time step, and captures one frame.
 *
 * Two backends with the same interface:
 *   corpus_amiga.c  ogles2.library through agl, in its own window
 *   corpus_host.c   Mesa (llvmpipe) through EGL into a framebuffer object:
 *                   the reference images, and the programs' exact shader
 *                   sources for the offline compiler (corpus_shaders.txt,
 *                   the format of T:ogles2_missing_shaders.txt)
 * Shared: corpus.c (frames, time, capture); the application frameworks on
 * top: esutil_corpus.c (the book's esUtil), eglut_corpus.c (Mesa's eglut).
 *
 * Environment, read by both:
 *   CORPUS_FRAMES  frames to run (default 60); 0: live, until Esc or Ctrl-C,
 *                  with the program's time from the clock instead of the fixed step
 *   CORPUS_SHOT    the frame to capture, 1-based (default: the last)
 *   CORPUS_OUT     the capture's file name (.ppm); none: no capture
 */
#ifndef CORPUS_H
#define CORPUS_H

#define CORPUS_DT_MS 33 /* the fixed time step: 30 frames a second of program time */

/* Backend. */
int corpus_backend_open(const char *title, int width, int height, int depth, int stencil);
int corpus_backend_present(void); /* 0: the user asked to stop */
void corpus_backend_close(void);
void corpus_backend_hold(void); /* after the last frame: keep it visible a moment */

/* corpus.c */
int corpus_open(const char *title, int width, int height, int depth, int stencil);
int corpus_frame_done(void);   /* captures if due, presents; 0: stop */
unsigned long corpus_time_ms(void); /* program time: frames x CORPUS_DT_MS, live the clock's */
float corpus_step_s(void);          /* seconds since the previous frame (fixed, or the clock's) */
int corpus_width(void);
int corpus_height(void);
void corpus_close(void);

#endif
