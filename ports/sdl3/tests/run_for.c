/*
 * The start of SDL3's test programs on the A1200 test station (ports/sdl3;
 * ports/sdl2/tests/run_for.c with SDL3's API): "--seconds N" (default 6)
 * ends the program with SDL_EVENT_QUIT N seconds after its first presented
 * frame, so a test package runs with nobody at the keyboard; the program
 * then quits as on Escape. "--seconds" is taken out, every other argument
 * goes to the test.
 *
 * Built twice. On AmigaOS 3, SDL_main.h makes a main() only for a program
 * with SDL3's main callbacks (testsprite), and that main() starts it
 * through SDL_RunApp: run_for_cb.o wraps SDL_RunApp (-Wl,--wrap=SDL_RunApp).
 * A program with its own main is compiled with -Dmain=test_main and linked
 * with run_for_main.o (-DRUN_FOR_MAIN), whose main() calls it.
 *
 * No thread: the time is checked at every presented frame (the link wraps
 * SDL_RenderPresent and SDL_GL_SwapWindow: -Wl,--wrap=...), in the
 * program's own task.
 *
 * Also: SDL's log goes to stdout (the package's result files), and SDL_Quit
 * runs at exit, whatever path the test leaves by.
 *
 * zlib licence, as SDL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED /* SDL_main_func only: no main() from here */
#include <SDL3/SDL_main.h>

extern bool __real_SDL_RenderPresent(SDL_Renderer *renderer);
extern bool __real_SDL_GL_SwapWindow(SDL_Window *window);

/* clib2's startup gives the program this much stack, whatever the Shell's
 * (as SDL2_68k's amiga_test_preamble.h). */
unsigned long __stack = 256 * 1024;

static int seconds = 6;  /* from the first presented frame; 0: no limit */
static Uint64 deadline;  /* SDL_GetTicks() value, set at the first frame */
static int quit_posted;

static void check_time(void)
{
    if (!seconds || quit_posted) {
        return;
    }
    if (!deadline) {
        deadline = SDL_GetTicks() + (Uint64)seconds * 1000;
        return;
    }
    if (SDL_GetTicks() >= deadline) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&e);
        quit_posted = 1;
    }
}

bool __wrap_SDL_RenderPresent(SDL_Renderer *renderer)
{
    bool ok = __real_SDL_RenderPresent(renderer);
    check_time();
    return ok;
}

bool __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
    bool ok = __real_SDL_GL_SwapWindow(window);
    check_time();
    return ok;
}

static void SDLCALL log_to_stdout(void *userdata, int category, SDL_LogPriority priority, const char *message)
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

/* "--seconds N" out of argv into `seconds`; the rest into args. */
static int start(int argc, char *argv[], char **args)
{
    int i, n = 0;
    for (i = 0; i < argc && n < 63; i++) {
        if (SDL_strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = SDL_atoi(argv[++i]);
            continue;
        }
        args[n++] = argv[i];
    }
    args[n] = NULL;
    if (seconds < 0) {
        seconds = 0;
    }
    SDL_SetLogOutputFunction(log_to_stdout, NULL);
    atexit(quit_sdl);
    return n;
}

#ifdef RUN_FOR_MAIN
extern int test_main(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    char *args[64];
    int n = start(argc, argv, args);
    return test_main(n, args);
}
#else
extern int __real_SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved);

int __wrap_SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved)
{
    char *args[64];
    int n = start(argc, argv, args);
    return __real_SDL_RunApp(n, args, mainFunction, reserved);
}
#endif
