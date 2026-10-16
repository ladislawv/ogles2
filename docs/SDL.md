# SDL2 and SDL3 with ogles2.library

This project adds an OpenGL ES 2.0 backend to the 68k AmigaOS 3 versions of SDL2
and SDL3. With it, SDL programs draw through the PiStorm's VideoCore GPU
(ogles2.library) instead of the CPU or MiniGL.

| | SDL2 | SDL3 |
|---|---|---|
| Base | Steffen Haeuser's [SDL2_68k](https://github.com/SteffenHaeuser/SDL2_68k) (SDL 2.33.0, Aminet `dev/lib/SDL_68k`) | Steffen Haeuser's [SDL3_68k](https://github.com/SteffenHaeuser/SDL3_68k) (SDL 3.4.14, from capehill's AmigaOS 4 port) |
| This project's part | [`ports/sdl2`](../ports/sdl2/README.md) | [`ports/sdl3`](../ports/sdl3/README.md) |
| Output | `libSDL2.a`, `libSDL2_test.a` | `libSDL3.a`, `libSDL3_test.a` |
| Licence | zlib (as SDL) | zlib (as SDL) |

Both are static link libraries built with clib2; there is no `SDL2.library`.
They ship as a separate archive, `ogles2-1.3-sdl.lha` (or `.zip` for a PC), which unpacks
over the ogles2 SDK archive: `SDK/SDL2/`, `SDK/SDL3/`, `SDK/MiniGL/` (PiStorm3D's MiniGL SDK,
which SDL links for its desktop-GL path), this guide as `SDK/doc/SDL.md`, and two examples
(`SDK/examples-sdl/`: `sdl2_sprites`, the 2D renderer; `sdl3_gles2`, a program's own GLES2
code; built in `Examples/SDL/`). Or build them from the source tree (below).

## What it adds

1. **An ogles2 GL context backend** (`src/video/amigaos3/SDL_os3opengles2.c`,
   derived from AmigaOS 4's `SDL_os4opengles2.c`). A program that asks SDL for
   an OpenGL ES 2.0 context gets one from ogles2.library on its SDL window:
   - `SDL_GL_CreateContext`, `SDL_GL_SwapWindow`, `SDL_GL_GetProcAddress` work
     as on other platforms;
   - `SDL_GL_SetSwapInterval(1)` gives vsync (`OGLES2_CCT_VSYNC`), 0 turns it off;
   - depth and stencil sizes come from `SDL_GL_DEPTH_SIZE` and
     `SDL_GL_STENCIL_SIZE` (0 and 0 allocate none: cheaper for 2D);
   - resizing the window or toggling full screen keeps the context, its
     textures, buffers and shaders (`aglSetParams2(OGLES2_CCT_WINDOW)`).
2. **SDL's GLES2 renderer** (`SDL_Renderer` named `"opengles2"`) built in. Programs
   that use only the 2D API (`SDL_CreateRenderer`, textures, `SDL_RenderCopy` /
   `SDL_RenderTexture`, lines, rectangles, rotation, blending) are drawn by the GPU
   with no GL code of their own.
3. **The backend is chosen by profile**, as on AmigaOS 4: an ES 2.x request
   (`SDL_GL_CONTEXT_PROFILE_ES`, which the GLES2 renderer makes) goes to
   ogles2.library; any other GL request still goes to minigl.library. Existing
   MiniGL programs behave as before.
4. **Two fixes in SDL2_68k** that matter even without GLES2: a crash in the
   software renderer when built with bebbo GCC 6.5 (a library call nested in
   another's arguments), and a crash when SDL emulated the window surface with a
   GLES2 texture. SDL3_68k needed neither.

## The benefit

- **Speed.** The GPU does the drawing; the 68k CPU (Emu68) only issues the calls.
  On the A1200 (Pi 4B, Emu68), 640x480, frames per second:

  | Test | SDL2 | SDL3 |
  |---|---|---|
  | `testgles2` (GL directly), window | 549 | 510 |
  | `testgles2`, full screen | 489 | 452 |
  | `testsprite`, GLES2 renderer | 198 | 283 |
  | `testsprite`, software renderer | 30 | 235 |
  | `testrendercopyex` (rotated, scaled copies) | 131 | 102 |
  | `testdraw2` / `testdraw` (lines, points, rectangles) | 72 | 130 |
  | `testgles2 --vsync` | | 60 (no tearing) |

  With SDL2 the GLES2 renderer is about 6.5 times the software renderer for
  sprites. SDL3_68k's software renderer is much faster already, so the gain
  there is smaller. Rotation, scaling and blending cost almost nothing extra on
  the GPU, unlike in software.
- **Portability.** Code written for SDL with GLES2 on Linux, Windows, Android,
  AmigaOS 4 or the web compiles here unchanged: the same `SDL_GL_*` calls, the
  same GLSL ES shaders. That includes SDL's `testgles2` and the GLES2 renderer
  itself, which were not modified for AmigaOS 3.
- **Real shaders.** GLSL ES 1.00, framebuffer objects, float textures, 4x MSAA
  and the extensions of ogles2.library 1.3, none of which MiniGL offers.
- **Vsync without tearing**, in a window and full screen.
- **Nothing lost.** Audio, input, timers, threads and MiniGL programs work as in
  the SDL_68k releases.

## Requirements on the Amiga

- PiStorm with a Raspberry Pi 4B or CM4, Emu68, AmigaOS 3.x, an RTG system
  (Picasso96 or CyberGraphX), a 32-bit RTG screen (SDL's full screen also opens
  one).
- `ogles2.library` and, for any program that compiles shaders at run time, the
  compiler `v3dglslc`, installed in `LIBS:` (`Install-ogles2`) or in the
  program's drawer (`PROGDIR:libs/` or `PROGDIR:`).
- **SDL's GLES2 renderer writes its shaders at run time**, so 2D programs need
  the compiler too. The first run compiles each shader (about a second each);
  the results go to `OGLES2Cache:` (or `T:`) and later runs load them. To ship
  without the compiler, run the program once and ship the `.v3dp` files from
  the cache in the program's drawer (see `release/GettingStarted.txt`, the
  section on shaders).
- `minigl.library` is linked in (for the MiniGL path) but only opened by
  programs that ask for desktop GL.

## Building

The SDL archive: `bash release/build_release.sh sdl` (with `dist`, also into `release/dist/`).
The libraries alone, in WSL on the build machine, after `make` in the repository root (it
builds `libogles2.a`):

```sh
bash ports/sdl2/build.sh    # -> $B/sdl2/libSDL2.a, $B/sdl2/tests/
bash ports/sdl3/build.sh    # -> $B/sdl3/libSDL3.a, $B/sdl3/tests/
```

Each script clones SDL2_68k or SDL3_68k at a pinned commit, applies
`patches/0001-gles2-on-ogles2.patch`, copies the new backend from `src/` in, and
builds with clib2. They need PiStorm3D's SDK for the MiniGL backend
(`$PISTORM3D_SDK`).

## Using them in a program

`SDK/examples-sdl/Makefile` in the archive does all of the following. Compile flags (bebbo
m68k-amigaos-gcc 6.5, clib2):

```
-O2 -m68040 -mhard-float -mcrt=clib2 -fbbb=abcfhilmnprsz0 -D__AMIGAOS3__
SDL3 also: -fshort-wchar   (clib2's wchar_t is 2 bytes; SDL3 checks it)
```

Link:

```
SDL2: -lSDL2 -logles2 -lminigl -ldebug -lamiga -lm881 -lstubs
SDL3: -lSDL3 -logles2 -lminigl -ldebug -lamiga -lm881 -latomic -lstubs
```

Use `-lm881`, not `-lm`: with clib2, `-lm` links libnix's maths, which gives
wrong `sqrtf`/`cosf`. Build with `-fbbb=abcfhilmnprsz0` (no `e`): GCC 6.5's
pass `e` drops the rounding of float-to-integer conversions.

Include paths: the ogles2 SDK's `include/`, `SDL2/include` or `SDL3/include`, and
`MiniGL/minigl-shared-library/include` (SDL's `SDL_opengl.h` includes `<proto/minigl.h>`).

You do not call `OGLES2Open()` yourself: SDL opens ogles2.library when it loads
the GL library and closes it on `SDL_Quit`.

### 2D with the GLES2 renderer (SDL2)

```c
#include <SDL2/SDL.h>

int main(int argc, char **argv)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    SDL_Window *win = SDL_CreateWindow("demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       640, 480, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    /* textures, SDL_RenderCopyEx, SDL_RenderPresent ... as anywhere else */
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
```

### 2D with the GLES2 renderer (SDL3)

```c
#include <SDL3/SDL.h>

SDL_Init(SDL_INIT_VIDEO);
SDL_Window *win = SDL_CreateWindow("demo", 640, 480, SDL_WINDOW_RESIZABLE);
SDL_Renderer *r = SDL_CreateRenderer(win, "opengles2");
SDL_SetRenderVSync(r, 1);
```

Without the hint (SDL2) or the name (SDL3), SDL picks the first renderer that
works; naming `"opengles2"` makes sure it is the GPU one.

### Your own OpenGL ES 2.0 code (SDL2; SDL3 is the same with its names)

```c
SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

SDL_Window *win = SDL_CreateWindow("gl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
SDL_GLContext ctx = SDL_GL_CreateContext(win);
SDL_GL_SetSwapInterval(1);                 /* vsync */

/* GL functions: either call them directly (#include <GLES2/gl2.h>, linked
 * through -logles2), or look them up with SDL_GL_GetProcAddress("glDrawArrays"),
 * as portable SDL code does. */
while (running) {
    /* events, glClear, glDrawArrays ... */
    SDL_GL_SwapWindow(win);
}
SDL_GL_DeleteContext(ctx);                 /* SDL3: SDL_GL_DestroyContext */
```

The profile request is what selects ogles2.library. A program that does not set
`SDL_GL_CONTEXT_PROFILE_ES` and version 2 gets MiniGL.

## Things to know

- **Windows draw through an offscreen bitmap** blitted into the window
  (`ClipBlit`), so windows can overlap and move. SDL's full screen on these
  ports is a window on its own screen and takes the same path.
- **No `glFinish` before a swap is needed or wanted:** ogles2 overlaps the next
  frame's CPU work with the GPU render.
- **Shaders compile on the Amiga** the first time (about a second each, once).
  Ship the cache's `.v3dp` files to start instantly.
- **GLES2 only.** SDL's desktop-GL renderer and GLES3 code do not run on ogles2;
  for desktop GL code use gl4es (`ports/gl4es`) on ogles2, or SDL's MiniGL path.
- **The test programs** (`testgles2`, `testsprite`, `testrendercopyex`,
  `testdraw2` / `testdraw`) are built by the scripts and run by the A1200 test
  packages `m6-3` (SDL2) and `m6-4` (SDL3); `tests/run_for.c` adds
  `--seconds N` to quit them after N seconds.
