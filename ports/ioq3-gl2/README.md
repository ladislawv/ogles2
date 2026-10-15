# Quake III Arena with ioquake3's GLES2 renderer on ogles2.library

`ports/ioq3-gl2` builds Quake III Arena for AmigaOS 3 (68k) with ioquake3's
modern renderer, `renderergl2`, running on ogles2.library directly, without
gl4es. The plan and its status: `docs/IOQ3_GLES2_PORT_PLAN.md`.

## How it is put together

- **Engine:** Cowcat's AmigaOS 3 ioquake3 with the PiStorm changes
  (`$IOQ3_SRC`, as `ports/ioq3`: by default a clone of
  https://github.com/ladislawv/ioq3-amiga at a pinned commit,
  `ports/ioq3/sources.sh`). Its objects and game `.dll`s are the ones
  `ports/ioq3/build.sh` makes: run that first. Only the renderer differs.
- **Renderer:** upstream ioquake3's `code/renderergl2` and
  `code/renderercommon` (`$IOQ3_UP_SRC`: by default a clone of
  https://github.com/ioquake/ioq3 at commit `83a7762`, the same script) and its header-only `qcommon/json.h`,
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
  the glue `ports/gl4es/gen_glue.py` generates over ogles2.library's dispatch
  table (with `QUAKE3_GL_PROFILE` set, every call is timed and the split is
  printed at shutdown), `r_availableModes` for the video menu (every 32-bit
  RTG mode of 640x480 or more: 1920x1080 and the like are offered and set
  through `r_mode -1`), and what the engine's Amiga code takes from a glimp
  (the window, its message port, the mouse pointer calls).
- **`ioq3_gl2_ref.c`:** both trees use `REF_API_VERSION 8` with the same
  `refexport_t` and shared types; upstream's `refimport_t` has ten more
  members. The engine's `GetRefAPI` call is adapted to upstream's
  (renamed `GL2_GetRefAPI` at compile time). Also `Q_acos`.
- **`patches/`**: changes to the copied upstream files (made against the
  pinned clone, LF line ends). `0001-lighting-maps-off`: `r_normalMapping`
  and `r_specularMapping` default to 0. Quake III's data has neither map, so
  they only cost: at 1920x1080 the A1200's GPU took 43 ms a frame with them,
  10 ms without (17.7 to 44.8 fps); the menu can still turn them on.
  `0002-shader-progress`: `GLSL_InitGPUShader` calls the glimp's
  `GLimp_ShaderProgress` after each program; once one takes longer than a
  cache load, the glimp draws "Compiling shaders for this screen mode, once:
  N of about 109" and a progress bar into the window's RastPort (no GL
  state touched; the first frame covers it). Before, the screen stayed
  grey for about 90 s and looked stuck.

Output: `$B/ioq3gl2/quake3-gl2` and the game `.dll`s. Package
`tests/a1200/m6-5` runs the timedemo (`Run-M1`) or the game (`Play-Quake`).

## Notes

- The renderer builds 109 GLSL programs at start. Compiled on the Amiga
  they take about 90-110 s once, then load from `OGLES2Cache:` in 1.4 s.
  The screen size is part of every program's text (`r_FBufScale`), so each
  mode has its own set; package m6-5 ships the sets for 640x480 and
  1920x1080 with the default settings (`.v3dp` programs and `.v3ds` shader
  records, compiled on the A1200): a fresh install starts in 2.4 s.
- On OpenGL ES 2.0 the renderer has no framebuffer objects: no HDR, SSAO,
  sun shadows or post-processing; it reports the extensions it lacks
  (occlusion queries, depth reads, shadow samplers, standard derivatives).
- **`-fbbb=abcfhilmnprsz0`:** bebbo's GCC 6.5 drops the `fintrz` of
  float-to-integer conversions in its default optimiser pass `e`, so they
  round instead of truncating: vertex colours of 1.0 were packed as 0
  (invisible sprites) and normals of 1.0 overflowed. Both Quake ports build
  with the library's `-fbbb` set, which leaves that pass out.
- **Sound:** the engine's own drivers (`code/amiga`, with the 2026-10-08
  Paula and AHI fixes): AHI by default (16 bit), Paula as the automatic
  fallback or with `s_driver paula`. A timedemo's sound is garbled by nature
  (the game runs faster than real time).
- The renderer streams dynamic geometry through one buffer it orphans and
  refills at every draw, and draws animated models from one static buffer
  per model at a moving offset; ogles2 has a path for each (buffer.c).

GPL v2 or later, as ioquake3. It links `libogles2.a` (`client/`), which is MIT, and opens
ogles2.library at run time (LICENSE, sections 2 and 4).
