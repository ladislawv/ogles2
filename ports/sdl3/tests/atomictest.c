/*
 * atomictest: SDL3's atomics and spinlocks under contention between the audio
 * thread and the application's task (patches/0002). SDL3_68k emulated every
 * atomic with spinlocks that waited with SDL_Delay(0), so the audio thread
 * (higher priority) spun for ever on a lock the application held: the whole
 * Amiga froze at random. Here both sides hammer one counter, a CAS loop and a
 * spinlock for `--seconds N` (default 10) while a quiet tone plays.
 * --hold makes it deterministic: every 3 s the application's task keeps the
 * spinlock for 2 s while the audio thread asks for it (see main).
 *   usage: atomictest [--seconds N] [--driver NAME] [--hold]
 * Prints the counts every second, then PASS or FAIL (return code 0 or 10).
 * A machine that freezes never prints the result: that is the failure the
 * patch fixes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

static SDL_AtomicInt counter, cas_value, callbacks;
static SDL_SpinLock lock;
static int guarded;   /* changed only under `lock` */
static int phase;

/* Both sides do the same: the add, a CAS increment, and a short critical
 * section under the spinlock. */
static void hammer(int rounds)
{
    int i;
    for (i = 0; i < rounds; i++) {
        int old;
        SDL_AddAtomicInt(&counter, 1);
        do {
            old = SDL_GetAtomicInt(&cas_value);
        } while (!SDL_CompareAndSwapAtomicInt(&cas_value, old, old + 1));
        SDL_LockSpinlock(&lock);
        guarded++;
        SDL_UnlockSpinlock(&lock);
    }
}

static int hold_mode;   /* --hold: the deterministic case, see main */

static void SDLCALL feed(void *udata, SDL_AudioStream *stream, int additional, int total)
{
    static Sint16 buf[2048];
    (void)udata;
    (void)total;
    while (additional > 0) {
        int n = additional < (int)sizeof(buf) ? additional : (int)sizeof(buf), i;
        for (i = 0; i < n / 2; i++) {
            int t = phase % 100;
            buf[i] = (Sint16)((t < 50 ? t : 100 - t) * 80 - 2000);   /* quiet triangle */
            phase++;
        }
        SDL_PutAudioStreamData(stream, buf, n);
        additional -= n;
    }
    hammer(200);
    SDL_AddAtomicInt(&callbacks, 1);
}

int main(int argc, char **argv)
{
    SDL_AudioSpec spec = { SDL_AUDIO_S16BE, 2, 22050 };
    SDL_AudioStream *stream;
    int seconds = 10, i, adds_main = 0, ok;
    Uint64 t0;

    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--seconds"))
            seconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--driver"))
            SDL_SetHint(SDL_HINT_AUDIO_DRIVER, argv[i + 1]);
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--hold"))
            hold_mode = 1;
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        printf("FAIL: SDL_Init(SDL_INIT_AUDIO): %s\n", SDL_GetError());
        return 10;
    }
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!stream) {
        printf("FAIL: SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
        SDL_Quit();
        return 10;
    }
    printf("driver %s, %d seconds\n", SDL_GetCurrentAudioDriver(), seconds);
    fflush(stdout);
    SDL_ResumeAudioStreamDevice(stream);
    t0 = SDL_GetTicks();
    for (i = 1; i <= seconds; i++) {
        if (hold_mode && i % 3 == 1) {
            /* --hold: the application's task keeps the spinlock for 2 s of
             * busy work while the audio thread (higher priority) asks for
             * it in every callback. The task gets the CPU back only if the
             * waiting thread really sleeps; if it only yields, nothing of
             * lower priority runs and the machine freezes here. */
            Uint64 h0 = SDL_GetTicks();
            SDL_LockSpinlock(&lock);
            while (SDL_GetTicks() - h0 < 2000)
                SDL_AddAtomicInt(&counter, 0);
            guarded++;
            adds_main++;
            SDL_AddAtomicInt(&counter, 1);
            {
                int old;
                do {
                    old = SDL_GetAtomicInt(&cas_value);
                } while (!SDL_CompareAndSwapAtomicInt(&cas_value, old, old + 1));
            }
            SDL_UnlockSpinlock(&lock);
            printf("  held the spinlock 2 s: the audio thread waited, the task ran (%d callbacks)\n",
                   SDL_GetAtomicInt(&callbacks));
            fflush(stdout);
        }
        while (SDL_GetTicks() - t0 < (Uint64)i * 1000) {
            hammer(50);
            adds_main += 50;
        }
        printf("  %d s: %d callbacks, counter %d\n", i, SDL_GetAtomicInt(&callbacks), SDL_GetAtomicInt(&counter));
        fflush(stdout);
    }
    SDL_DestroyAudioStream(stream);
    SDL_Quit();
    /* Every increment counted once on each of the three, and the audio
     * thread kept running. */
    {
        int n = SDL_GetAtomicInt(&callbacks), expect = adds_main + n * 200;
        ok = n >= seconds * 5 && SDL_GetAtomicInt(&counter) == expect &&
             SDL_GetAtomicInt(&cas_value) == expect && guarded == expect;
        printf("%d callbacks; counter %d, CAS %d, spinlock %d, expected %d each\n", n,
               SDL_GetAtomicInt(&counter), SDL_GetAtomicInt(&cas_value), guarded, expect);
    }
    printf("%s\n", ok ? "PASS: no freeze, no lost update" : "FAIL");
    return ok ? 0 : 10;
}
