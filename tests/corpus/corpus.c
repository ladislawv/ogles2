/*
 * Frames, program time and the capture (corpus.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <GLES2/gl2.h>
#include "corpus.h"

static int frames_max = 60, shot, frame, width, height, opened, live;
static unsigned long start_ms, last_ms;
static const char *out;

static unsigned long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL;
}

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : dflt;
}

int corpus_open(const char *title, int w, int h, int depth, int stencil)
{
    frames_max = env_int("CORPUS_FRAMES", 60);
    live = frames_max <= 0;
    start_ms = last_ms = now_ms();
    shot = env_int("CORPUS_SHOT", frames_max);
    out = getenv("CORPUS_OUT");
    if (out && !*out)
        out = 0;
    width = w;
    height = h;
    frame = 0;
    if (!corpus_backend_open(title, w, h, depth, stencil)) {
        printf("corpus: %s: no context\n", title);
        return 0;
    }
    opened = 1;
    if (live)
        printf("corpus: %s, %dx%d, live: Esc in the window or Ctrl-C ends it\n", title, w, h);
    else
        printf("corpus: %s, %dx%d, %d frames, capture of frame %d%s%s\n", title, w, h, frames_max, shot,
               out ? " to " : "", out ? out : " off");
    return 1;
}

/* The frame as a binary PPM, top row first. */
static void capture(void)
{
    unsigned char *rgba = malloc((size_t)width * height * 4);
    FILE *f;
    int x, y;
    if (!rgba)
        return;
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (!(f = fopen(out, "wb"))) {
        printf("corpus: cannot write %s\n", out);
        free(rgba);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (y = height - 1; y >= 0; y--)
        for (x = 0; x < width; x++)
            fwrite(rgba + ((size_t)y * width + x) * 4, 1, 3, f);
    fclose(f);
    free(rgba);
    printf("corpus: frame %d captured\n", frame);
}

int corpus_frame_done(void)
{
    frame++;
    if (out && frame == shot)
        capture();
    if (!corpus_backend_present())
        return 0;
    return live || frame < frames_max;
}

unsigned long corpus_time_ms(void)
{
    return live ? now_ms() - start_ms : (unsigned long)frame * CORPUS_DT_MS;
}

float corpus_step_s(void)
{
    unsigned long t;
    if (!live)
        return CORPUS_DT_MS / 1000.0f;
    t = now_ms() - last_ms;
    last_ms += t;
    return t / 1000.0f;
}

int corpus_width(void)
{
    return width;
}

int corpus_height(void)
{
    return height;
}

void corpus_close(void)
{
    if (!opened)
        return;
    printf("corpus: %d frames, GL error 0x%04x at the end\n", frame, (unsigned)glGetError());
    corpus_backend_hold();
    corpus_backend_close();
    opened = 0;
}

/* rand() and srand() of the corpus programs (the hooks headers): one
 * generator for the PC and the Amiga, so both draw the same particles. */
static unsigned long seed = 1;

int corpus_rand(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (int)((seed >> 16) & 0x7fff);
}

void corpus_srand(unsigned int s)
{
    seed = s;
}
