/*
 * main() for SDL's test programs on the A1200 test station (ports/sdl2):
 * "--seconds N" (default 6) ends the program with SDL_QUIT N seconds after
 * its first presented frame,
 * so a test package runs with nobody at the keyboard; the program then
 * quits as on Escape. The test's own main is compiled as test_main
 * (-Dmain=test_main); every other argument goes to it.
 *
 * No thread: the time is checked at every presented frame (the link wraps
 * SDL_RenderPresent and SDL_GL_SwapWindow: -Wl,--wrap=...), in the
 * program's own task. A first version used SDL_AddTimer; when a test left
 * without SDL_Quit, SDL's timer thread outlived the program's code and
 * crashed the machine (illegal instruction in "SDL_SDLTimer").
 *
 * Also: SDL's log goes to stdout (the package's result files), and SDL_Quit
 * runs at exit, whatever path the test leaves by.
 *
 * zlib licence, as SDL.
 */
#include <stdio.h>
#include <stdlib.h>
#include "SDL.h"

extern int test_main(int argc, char *argv[]);
extern void __real_SDL_RenderPresent(SDL_Renderer *renderer);
extern void __real_SDL_GL_SwapWindow(SDL_Window *window);

static int seconds = 6;  /* from the first presented frame; 0: no limit */
static Uint32 deadline;  /* SDL_GetTicks() value, set at the first frame */
static int quit_posted;

static void check_time(void)
{
    if (!seconds || quit_posted) {
        return;
    }
    if (!deadline) {
        deadline = SDL_GetTicks() + (Uint32)seconds * 1000;
        return;
    }
    if (SDL_GetTicks() >= deadline) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
        quit_posted = 1;
    }
}

void __wrap_SDL_RenderPresent(SDL_Renderer *renderer)
{
    __real_SDL_RenderPresent(renderer);
    check_time();
}

void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
    __real_SDL_GL_SwapWindow(window);
    check_time();
}

static void log_to_stdout(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    (void)category;
    (void)priority;
    printf("%s\n", message);
    fflush(stdout);
}

static void quit_sdl(void)
{
    SDL_Quit();
}

int main(int argc, char *argv[])
{
    char *args[64];
    int i, n = 0;

    for (i = 0; i < argc && n < 63; i++) {
        if (SDL_strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = SDL_atoi(argv[++i]);
            continue;
        }
        args[n++] = argv[i];
    }
    args[n] = NULL;
    SDL_LogSetOutputFunction(log_to_stdout, NULL);
    atexit(quit_sdl);
    if (seconds < 0) {
        seconds = 0;
    }
    return test_main(n, args);
}
