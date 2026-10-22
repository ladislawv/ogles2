# SDL2 on AmigaOS 3 with OpenGL ES 2.0 (ogles2.library)

SDL2 for 68k AmigaOS 3 is Steffen Haeuser's
[SDL2_68k](https://github.com/SteffenHaeuser/SDL2_68k) (the SDL2 of Aminet's
`dev/lib/SDL_68k`, SDL 2.33.0): RTG and AGA, threads, timers, AHI and Paula
audio, and OpenGL through minigl.library. This directory adds an OpenGL ES
2.0 backend on ogles2.library to its video driver, and SDL's GLES2 renderer.
It is the plan's M6 item "SDL2 / SDL3: an ogles2 GL-context backend plus
SDL's GLES2 renderer" (`docs/DEVELOPMENT_PLAN.md` §9, §13).

## Building

In WSL, after `make`:

```sh
bash ports/sdl2/build.sh
```

`build.sh` clones SDL2_68k into `$SDL68K_SRC` (default `~/sdl2-msn`) at the
commit it names, resets it, applies `patches/`, copies `src/` in, and runs its
clib2 Makefile. It writes `$B/sdl2/libSDL2.a`, `libSDL2_test.a`, and test
programs in `$B/sdl2/tests`. It needs PiStorm3D's SDK for the MiniGL backend
(`$PISTORM3D_SDK`, default `/mnt/c/- CPP/pistorm3d/SDK`: `libminigl.a` and its
headers) and SDL 2.30.3 from upstream for `testgles2` and `testrendercopyex`
(`$SDLUP_SRC`, cloned if missing).

A program links `-lSDL2 -logles2 -lminigl -ldebug -lamiga -lm881 -lstubs`
(clib2; `-lm881`, not `-lm`, which would be libnix's maths).

## How it is put together

- **`src/video/amigaos3/SDL_os3opengles2.c`** (zlib, as SDL; derived from
  AmigaOS 4's `SDL_os4opengles2.c`): an ogles2 context on the SDL window's
  Intuition window (ogles2's window mode: an offscreen bitmap, then
  `ClipBlit`), `SDL_GL_GetProcAddress` through libogles2.a's
  `aglGetProcAddress` (functions without the context argument, as SDL's
  renderer calls them), swap interval 0 or 1 (`OGLES2_CCT_VSYNC`), and the
  window resized or replaced by a fullscreen toggle through
  `aglSetParams2(OGLES2_CCT_WINDOW)`, which keeps the context's objects.
  No `glFinish` before the swap: ogles2 overlaps the next frame's CPU work
  with the render.
- **The backend is chosen when SDL loads the GL library**, by the profile
  the program asks for, as AmigaOS 4's driver does: ES 2.x
  (`SDL_GL_CONTEXT_PROFILE_ES`, which SDL's GLES2 renderer sets) gets
  ogles2.library; anything else gets minigl.library as before.
- **`patches/0001-gles2-on-ogles2.patch`**: the configuration
  (`SDL_VIDEO_OPENGL_ES2`, `SDL_VIDEO_RENDER_OGL_ES2`), the device's
  `GL_LoadLibrary`, a `gl_es` flag in the window data, MiniGL's resize and
  delete handing ogles2 contexts over (the window and event code call them
  directly), and both Makefiles building the new files. Two fixes besides:
  - `SDL_video.c`: on AmigaOS 3 the window surface stays the driver's
    framebuffer. With an accelerated renderer present SDL would otherwise
    emulate it with a GLES2 texture, creating a GLES2 renderer inside the
    software renderer's creation (a crash).
  - `SDL_os3framebuffer.c`: `GetCyberMapAttr` moved out of `AllocBitMap`'s
    arguments. bebbo GCC 6.5's inline headers load A6 with the library base
    before evaluating the arguments, so a library call nested in another's
    arguments leaves the wrong base in A6: `AllocBitMap` ran through
    CyberGfxBase and the software renderer crashed. Unpatched SDL2_68k built
    with this toolchain crashes the same way (Aminet's binary does not).
- **`patches/0002-os3-fixes-issue5.patch`** (GitHub issue 5, from arczi84's
  patch, gist `18ddb47cc9059dd7cf6c6842e58bf6d5`; its header lists the
  changes): no `-DSDL_OS3_DEBUG` (SDL wrote logs on `WORK:`, a requester
  without that volume); AHI audio, whose thread now holds the message port
  while it runs (`ThreadInit`/`ThreadDeinit`; arczi84's version took it at the
  first I/O and gave it back only after the thread had ended) and whose
  availability check opens `AHI_NO_UNIT` (unit 0 opened, closed and opened
  again at once left AHI 6.7 on the test A1200 never completing the first
  write); a 32-bit fullscreen mode found in the display database; the
  display's `driverdata`, so fullscreen screens are closed; the Alt and Amiga
  rawkeys.
- **`patches/0003-os3-relative-mouse.patch`** (2026-10-21): SDL's relative
  mouse mode for the amigaos3 driver. The driver had none and its `WarpMouse`
  is a stub, so SDL's fallback (warping the pointer back to the window's
  centre) did nothing: the hidden pointer stopped at the screen's edge, and so
  did a game's mouse look. The event pump now sets `IDCMP_DELTAMOVE` on the
  windows while the mode is on and sends Intuition's movement deltas as
  relative motion (moves queued across a switch are dropped).
- **`patches/0004-os3-ahi-format.patch`** (2026-10-21): the AHI driver's
  format search (`OS3AHI_OpenDevice`) kept the format after the one it
  matched: its loop tested `!found` in its condition, so the increment ran
  once more, and `AUDIO_S16MSB` became `AUDIO_S16LSB`. SDL then converted an
  application's big-endian samples to little-endian, and AHI, told
  `AHIST_S16S`, played them byte-swapped: noise at every level. The loop now
  stops at the match. `tests/audiotest.c` did not show it: it counts the
  callback's buffers, it does not listen. The Paula driver was not affected.
- **`patches/0005-os3-atomics-no-spin.patch`** (2026-10-22): SDL's atomics
  without spinlocks. SDL2_68k has no native atomics (`HAVE_GCC_ATOMICS` off),
  so `SDL_atomic.c` emulated every atomic operation (`SDL_AtomicGet`, `Add`,
  `CAS`) with one of 32 spinlocks, and `SDL_AtomicLock` waited on them with
  `SDL_Delay(0)`. exec always runs the highest-priority ready task, so that
  gives nothing to a holder of lower priority: when the audio thread
  (priority 10) wanted the lock the application's task (0) held, it spun for
  ever, and everything under priority 10 stopped (the whole A1200 froze at
  random, seconds to minutes in; a flight recorder above priority 20 showed
  the application standing in `SDL_AtomicLock` under `SDL_AtomicAdd` and the
  audio thread ready in every sample). On AmigaOS 3 the atomics now run under
  `Forbid()`/`Permit()`, atomic against every task on its one CPU, and
  `SDL_AtomicLock`, still used for a few rare locks, sleeps a tick
  (`SDL_Delay(1)`) between tries.
- **`-fbbb=abcfhilmnprsz0`**: the library and tests are built with ogles2's
  optimiser passes (without "e").
- **`tests/run_for.c`**: a `main` for SDL's test programs on the test
  station: `--seconds N` posts `SDL_QUIT` after N seconds.
- **`tests/audiotest.c`**: a quiet tone for `--seconds N` through
  `--driver ahi` or `paula` (SDL's hint: this SDL reads no Amiga environment
  variable); the callback's count of buffers must keep pace with the time.
  **`tests/screens.c`**: Intuition's screens, size, depth and title (`NOSDL`:
  return 10 if one titled "SDL" is open). Package `m7-7` runs both.

## Licences

SDL2_68k is SDL (zlib). The files here are zlib as SDL; the build script is
MIT like the rest of the tools. The PiStorm3D SDK has its own licences.
