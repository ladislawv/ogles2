# glmark2-es2 on ogles2.library

[glmark2](https://github.com/glmark2/glmark2) 2023.01 (commit `22c527c`), the
OpenGL ES 2.0 benchmark, for AmigaOS 3 on a 68020 with a 68881, drawn by
ogles2.library.

## Building

In WSL, after `make` (for `libogles2.a`):

    bash ports/glmark2/build.sh

The output is `$B/glmark2/glmark2-es2` with its data in `$B/glmark2/data`.
`$B` defaults to `~/ogles2-build/m68k`. The A1200 package is
`tests/a1200/m6-7`.

`build.sh` does the following:

1. Clones glmark2 into `~/glmark2` at the pinned commit, unless
   `GLMARK2_SRC` names a tree of your own. It uses `clone_pinned` from
   `ports/ioq3/sources.sh`.
2. Copies the tree to `$B/glmark2/src`, applies `patches/` and adds `src/`.
3. Compiles everything with up to `$J` jobs (8 by default).

### Compiler

glmark2 is C++17 and uses `std::filesystem`, which GCC 6.5 lacks. It is
built with GCC 16.2 (`$TC16`, by default `~/amiga-gcc162-install`) and
libnix, using `-mcpu=68020 -m68881`.

`libogles2.a` comes from the library's own GCC 6.5 build and is linked
unchanged:

- Both compilers write Amiga hunk objects.
- The import library is plain C and needs only `SysBase` and `strcmp`.

GCC 16.2 keeps the `fintrz` before float-to-integer moves, so the
`-fbbb` workaround that the GCC 6.5 builds need does not apply here.

### Bundled libraries

libpng, libjpeg-turbo and zlib are compiled from glmark2's own copies, as
its meson build does when no system libraries are available. The file lists
are the ones in their `meson.build` files.

## What the port adds

- **`patches/0001-amiga-flavour.patch`** adds two flavours to `main.cpp`:
  `GLMARK2_USE_AMIGA` for the native state and `GLMARK2_USE_OGLES2` for the
  GL state.
- **`patches/0002-amiga-errors-to-stdout.patch`** sends errors and warnings
  to stdout. An AmigaOS Shell cannot redirect stderr, and a remote run
  would lose them.
- **`patches/0003-amiga-resources.patch`** changes how data files are
  found and read:
  - `Util::list_files` uses libnix's `opendir`/`readdir`. GCC 16.2's
    `std::filesystem::directory_iterator` returns no entries there and
    reports no error, so every scene with a model or texture failed to set
    up without a message.
  - `Util::get_resource` reads the whole file through stdio into a memory
    stream, so the loaders' seeks never touch libstdc++'s file streams.
  - The 3DS loader leaves its chunk loop when a read returns nothing. A
    stream that failed without reaching end-of-file had kept it spinning
    forever.
- **`patches/0004-amiga-eclock-timestamps.patch`** makes
  `Util::get_timestamp_us` call `amiga_timestamp_us()` in
  `src/amiga-compat.cpp`, which reads timer.device's E-clock
  (`ReadEClock`). Every scene's animation, its duration and the FPS are
  timed through this function, and the standard clocks are too coarse:
  - GCC 16.2's libstdc++ builds `std::chrono::steady_clock::now()` on
    `time()`, so it counts whole seconds. Each scene moved about once a
    second while rendering at 300 fps, and the per-scene figures were off.
  - libnix's `clock_gettime` and `gettimeofday` tick at 50 Hz.
- **`src/native-state-amiga.*`**:
  - `create_window` creates the ogles2 context. ogles2 opens the window
    itself, on the Workbench screen, with depth 24, stencil 8 and no
    vsync.
  - The context is handed on as the native window.
  - Esc in the window or Ctrl-C ends the run.
  - `--fullscreen` is not supported yet; use `-s WxH` instead.
- **`src/gl-state-ogles2.*`** makes the context current. glad loads the
  GL functions through `aglGetProcAddress`, whose context-free thunks call
  the current context. `swap` calls `aglSwapBuffers`. It has no sync
  objects; off-screen runs fall back to `glFinish`.
- **`src/amiga-compat.cpp`** supplies what libnix lacks:
  - `getrusage`, which reports CPU time from `clock()`;
  - `statvfs`, which always fails;
  - `__xpg_strerror_r`.

  libnix's headers declare `getrusage` and `statvfs` without `extern "C"`,
  so the code calls them under their C++ names, and they are defined that
  way here.

  It also replaces libnix's locale with the C locale only (`setlocale`,
  `localeconv`, `__locale_mb_cur_max`), for this reason:
  - libnix's `setlocale.o` starts every program in the system locale from
    locale.library. On a machine whose locale uses a decimal comma,
    `printf` then writes `1,0`, and `setlocale(LC_ALL, "C")` does not
    undo it.
  - glmark2 writes its constants into shader source, so most scenes failed
    to compile.
  - With these three functions defined here, `setlocale.o` is not linked
    at all and the decimal point stays `.`.
- `shared-library.cpp` (dlopen) is left out, because only the EGL state
  uses it.

## Running

The data path is `PROGDIR:data`, set with `--data-path`. Two settings
matter:

- Give glmark2 a bigger stack: `Stack 262144`.
- Assign `OGLES2Cache:` to a directory. Each scene's shaders are compiled
  on the Amiga the first time they are used (`v3dglslc` in `LIBS:`) and
  are kept there.

`-b :duration=2` shortens every scene of the default list from 10 s to
2 s.

## Results

On the A1200 (Emu68 on a Raspberry Pi 4), on 2026-10-13, with an
800x600 window and the default list at 10 s a scene, glmark2 scored
**253**:

- 32 of 33 scenes ran, with 0 frames dropped in 94,230.
- Most scenes ran at 260–370 fps.
- `terrain` reports "Unsupported". It needs vertex texture fetch, and
  ogles2 has 0 vertex texture units.

Three scenes were slow at first:

| Scene | FPS | Cause |
|---|---|---|
| `refract` | 3, now 48 | It calls `glGenerateMipmap` on a 1600x1200 render target every frame. ogles2 did that on the CPU: it read back level 0, box-filtered on the 68k and re-uploaded the chain, about 500 ms. Since the same day the GPU's TFU makes the levels. |
| `buffer` (map) | 50–83 | Each frame updates half of a 200-column buffer. |
| `buffer` (subdata) | 50 | The same updates, through `glBufferSubData`. |

With the TFU, the whole list at 10 s a scene scored **251**, with 0 frames
dropped in 94,421. That is about the same as 253 before, because the score
is the average over 32 scenes: one scene going from 3 to 48 fps adds about
1.4 points, within the other scenes' run-to-run variation.
