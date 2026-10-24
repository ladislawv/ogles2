# SDL3 on AmigaOS 3 with OpenGL ES 2.0 (ogles2.library)

SDL3 for 68k AmigaOS 3 is Steffen Haeuser's
[SDL3_68k](https://github.com/SteffenHaeuser/SDL3_68k) (the SDL3 of Aminet's
`dev/lib/SDL_68k`, SDL 3.4.14, based on capehill's AmigaOS 4 port): RTG
windows and screens, threads, timers, AHI audio, and OpenGL through
minigl.library. This directory adds an OpenGL ES 2.0 backend on
ogles2.library to its video driver, and SDL's GLES2 renderer, as
`ports/sdl2` does for SDL2_68k. It is the plan's M6 item "SDL2 / SDL3: an
ogles2 GL-context backend plus SDL's GLES2 renderer" (`docs/DEVELOPMENT_PLAN.md`
§9, §13).

## Building

In WSL, after `make`:

```sh
bash ports/sdl3/build.sh
```

`build.sh` clones SDL3_68k into `$SDL3_68K_SRC` (default `~/sdl3-msn`) at the
commit it names, resets it, applies `patches/`, copies `src/` in, and runs
its `Makefile.amigaos3` (clib2). It writes `$B/sdl3/libSDL3.a`,
`libSDL3_test.a`, and SDL3's `testgles2`, `testsprite`, `testrendercopyex`
and `testdraw` in `$B/sdl3/tests`. It needs PiStorm3D's SDK for the MiniGL
backend (`$PISTORM3D_SDK`, default `/mnt/c/- CPP/pistorm3d/SDK`).

A program links `-lSDL3 -logles2 -lminigl -ldebug -lamiga -lm881 -latomic
-lstubs` (clib2; `-lm881`, not `-lm`, which would be libnix's maths) and is
compiled with `-fshort-wchar` (below).

## How it is put together

- **`src/video/amigaos3/SDL_os3opengles2.c`** (zlib, as SDL; derived from
  AmigaOS 4's `SDL_os4opengles2.c`, and `ports/sdl2`'s with SDL3's API): an
  ogles2 context on the SDL window's Intuition window (ogles2's window mode:
  an offscreen bitmap, then `ClipBlit`), `SDL_GL_GetProcAddress` through
  libogles2.a's `aglGetProcAddress`, swap interval 0 or 1, and a resized
  window followed through `aglSetParams2(OGLES2_CCT_WINDOW)`. No `glFinish`
  before the swap: ogles2 overlaps the next frame's CPU work with the render.
- **The backend is chosen when SDL loads the GL library**, by the profile
  asked for, as AmigaOS 4's driver does: ES 2.x (which SDL's GLES2 renderer
  sets) gets ogles2.library; anything else minigl.library as before.
- **`patches/0001-gles2-on-ogles2.patch`**: the configuration
  (`SDL_VIDEO_OPENGL_ES2`, `SDL_VIDEO_RENDER_OGL_ES2`, not in the
  software-only variant), the device's `GL_LoadLibrary`, a `gl_es` flag in
  the window data, MiniGL's resize and destroy handing ogles2 contexts over
  (the window and event code call them directly), and `Makefile.amigaos3`
  building the GLES2 renderer with `$(OGLES2_CFLAGS)` and `-fshort-wchar`.
  The driver already keeps SDL from emulating the window surface with a
  texture (`checked_texture_framebuffer`), which SDL2_68k needed a patch for.
- **`patches/0002-os3-atomics-no-spin.patch`** (2026-10-24): SDL's atomics
  without spinlocks, as `ports/sdl2`'s patch 0005. SDL3_68k has no native
  atomics either (`HAVE_GCC_ATOMICS` off), so every atomic operation took one
  of 32 spinlocks and `SDL_LockSpinlock` waited with `SDL_Delay(0)`, which
  gives nothing to a holder of lower priority (the audio thread, 10, could
  spin for ever on a lock the application's task, 0, held, and freeze
  everything under it). On AmigaOS 3 the atomics now run under
  `Forbid()`/`Permit()`, `SDL_TryLockSpinlock` does too (instead of SDL's
  mutex fallback, whose mutex was created unguarded on first use), and
  `SDL_LockSpinlock` sleeps a tick (`SDL_Delay(1)`) between tries.
- **`-fshort-wchar`**: this toolchain's clib2 defines `wchar_t` as
  `unsigned short`, GCC as 4 bytes; SDL3's `SDL_string.c` asserts they agree.
  SDL3_68k does not build with it otherwise.
- **`tests/run_for.c`**: the test programs' start on the test station:
  `--seconds N` posts `SDL_EVENT_QUIT` N seconds after the first frame
  (checked at each present; no thread). On AmigaOS 3, `SDL_main.h` makes a
  `main()` only for programs with SDL3's main callbacks; for those it wraps
  `SDL_RunApp`, for the others it is `main()` and calls the test's, renamed.

SDL3_68k's own Amiga sources have no library call inside another's
arguments from a different library (bebbo GCC 6.5's inline headers load A6
before evaluating the arguments: ports/sdl2/README.md).

## Licences

SDL3_68k is SDL (zlib). The files here are zlib as SDL; the build script is
MIT like the rest of the tools. The PiStorm3D SDK has its own licences.
