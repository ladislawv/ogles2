/*
 * audiotest: SDL2's audio thread keeps playing (GitHub issue 5: with AHI the
 * thread waited for replies signalled to another task and hung after two
 * buffers). A quiet 441 Hz tone for `--seconds N` (default 3); the callback
 * counts the buffers it fills, which must keep pace with the time played.
 * The driver is SDL's choice (Paula first), or --driver's (ahi, paula):
 * SDL's hint, as this SDL reads no Amiga environment variable (its
 * SDL_getenv is SDL's own table on AmigaOS 3).
 *   usage: audiotest [--seconds N] [--driver NAME]
 * Prints PASS or FAIL; the return code is 0 or 10.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL.h"

static volatile int buffers;
static int phase;

static void SDLCALL fill(void *udata, Uint8 *stream, int len)
{
    Sint16 *s = (Sint16 *)stream;
    int i, n = len / 4; /* stereo, 16 bit */
    (void)udata;
    for (i = 0; i < n; i++) {
        /* a triangle wave, 2000 of 32767: quiet */
        int t = phase % 100, v = (t < 50 ? t : 100 - t) * 80 - 2000;
        s[2 * i] = s[2 * i + 1] = (Sint16)v;
        phase++;
    }
    buffers++;
}

int main(int argc, char **argv)
{
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID dev;
    int seconds = 3, expected, got, i;
    Uint32 t0, ms;

    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--seconds"))
            seconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--driver"))
            SDL_SetHint(SDL_HINT_AUDIODRIVER, argv[i + 1]);
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        printf("FAIL: SDL_Init(SDL_INIT_AUDIO): %s\n", SDL_GetError());
        return 10;
    }
    SDL_zero(want);
    want.freq = 22050; /* the tone: 50 samples a period, 441 Hz */
    want.format = AUDIO_S16MSB;
    want.channels = 2;
    want.samples = 1024;
    want.callback = fill;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        printf("FAIL: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        SDL_Quit();
        return 10;
    }
    printf("driver %s: %d Hz, %d channels, %d samples a buffer\n", SDL_GetCurrentAudioDriver(), have.freq,
           have.channels, have.samples);
    fflush(stdout);
    t0 = SDL_GetTicks();
    SDL_PauseAudioDevice(dev, 0);
    /* The count every half second, written at once: a run that hangs
     * shows how far it got. */
    for (i = 0; i < seconds * 2; i++) {
        SDL_Delay(500);
        printf("  %d ms: %d buffers\n", (int)(SDL_GetTicks() - t0), buffers);
        fflush(stdout);
    }
    got = buffers;
    ms = SDL_GetTicks() - t0;
    printf("closing the device\n");
    fflush(stdout);
    SDL_CloseAudioDevice(dev);
    printf("closed\n");
    fflush(stdout);
    SDL_Quit();
    /* At least half the buffers the time played asks for: a hung thread
     * fills two or three. */
    expected = (int)((double)ms * have.freq / 1000.0 / have.samples);
    printf("%d buffers in %lu ms, %d expected\n", got, (unsigned long)ms, expected);
    printf("%s: the audio thread %s\n", got * 2 >= expected ? "PASS" : "FAIL",
           got * 2 >= expected ? "kept playing" : "stopped");
    return got * 2 >= expected ? 0 : 10;
}
