# Quake III Arena with ioquake3's GLES2 renderer on ogles2.library

`ports/ioq3-gl2` builds Quake III Arena for AmigaOS 3 (68k) with ioquake3's
modern renderer, `renderergl2`, running on ogles2.library directly, without
gl4es. The plan and its status: `docs/IOQ3_GLES2_PORT_PLAN.md`.

## How it is put together

- **Engine:** Cowcat's Amiga ioquake3, the MiniGL build's tree
  (`$IOQ3_SRC`, as `ports/ioq3`). Its objects and game `.dll`s are the ones
  `ports/ioq3/build.sh` makes: run that first. Only the renderer differs.
- **Renderer:** upstream ioquake3's `code/renderergl2` and
  `code/renderercommon` (`$IOQ3_UP_SRC`, a clone of
  https://github.com/ioquake/ioq3) and its header-only `qcommon/json.h`,
  copied unchanged into `$B/ioq3gl2/src` beside the engine's sources and
  compiled against the engine's qcommon headers.
  - `include/ioq3_gl2_compat.h` (forced in): the few definitions upstream's
    newer `q_shared.h` has and the engine's lacks (`Q_ALIGN`, `quat_t`,
    `LERP`, `QuatCopy`, the printf attributes, `AVI_LINE_PADDING`).
  - `include/SDL_opengl.h`: the GL types and constants `qgl.h` takes from
    SDL, from the desktop GL headers gl4es ships (headers only).
  - `include/SDL.h`: tr_extensions.c's two SDL calls, answered by the glimp.
  - `tr_subs.c` and `puff.c` are left out (the engine has its own).
  - The GLSL files become C strings as upstream's CMake does it.
- **`amiga_glimp_gl2.c`** (in place of `sdl_glimp.c`): the ogles2 context
  (its own screen in a BGRA32/RGBA32 mode with `r_fullscreen 1`, a window
  with 0; depth 24, stencil 8), the OpenGL ES 2.0 function set by name from
  libogles2.a (`aglGetProcAddress`), and what the engine's Amiga code takes
  from a glimp (the window, its message port, the mouse pointer calls).
- **`ioq3_gl2_ref.c`:** both trees use `REF_API_VERSION 8` with the same
  `refexport_t` and shared types; upstream's `refimport_t` has ten more
  members. The engine's `GetRefAPI` call is adapted to upstream's
  (renamed `GL2_GetRefAPI` at compile time). Also `Q_acos`.
- **`patches/`** (none yet): changes to the copied upstream files.

Output: `$B/ioq3gl2/quake3-gl2` and the game `.dll`s. Package
`tests/a1200/m6-5` runs the timedemo (`Run-M1`) or the game (`Play-Quake`).

## Notes

- The first start compiles 109 GLSL programs on the Amiga (about 110 s);
  they are kept in `OGLES2Cache:` and load in 1.4 s afterwards.
- On OpenGL ES 2.0 the renderer has no framebuffer objects: no HDR, SSAO,
  sun shadows or post-processing; it reports the extensions it lacks
  (occlusion queries, depth reads, shadow samplers, standard derivatives).
- **The FPU's rounding mode:** bebbo's GCC 6.5 converts a float to a short,
  an unsigned short or an unsigned int without `fintrz`, so the FPU's mode
  decides; round-to-nearest packed vertex colours of 1.0 as 0 (invisible
  sprites) and normals of 1.0 as an overflow. The glimp sets round-to-zero
  for the game's task (`fpu_round_to_zero`), which makes those conversions
  truncate as C wants. `ports/ioq3`'s glimp does the same.
- **Sound:** the engine's own drivers (`code/amiga`, with the 2026-10-08
  Paula and AHI fixes): AHI by default (16 bit), Paula as the automatic
  fallback or with `s_driver paula`. A timedemo's sound is garbled by nature
  (the game runs faster than real time).
- The renderer streams dynamic geometry through one buffer it orphans and
  refills at every draw, and draws animated models from one static buffer
  per model at a moving offset; ogles2 has a path for each (buffer.c).

GPL v2 or later, as ioquake3; kept apart from the MIT library.
