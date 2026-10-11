comit# Quake III on ogles2.library directly: porting ioquake3's GLES2 renderer

Status: plan, 2026-10-11. Nothing built yet. Parent plan: `DEVELOPMENT_PLAN.md` §13 (M6).

## 1. Goal

Run Quake III Arena on the A1200 (PiStorm, Raspberry Pi 4, Emu68) through
ogles2.library **without gl4es**, using ioquake3's modern renderer
(`renderergl2`), which can run on OpenGL ES 2.0 since 2024. Target: at least
MiniGL's speed on the same timedemo, with the same look.

### Why

| Build (timedemo `four`, 640x480, `r_primitives 0`) | fps | ms a frame |
|---|---|---|
| MiniGL (minigl.library 29.1, the user's `quake3`) | 60.8 | 16.4 |
| ogles2 through gl4es (`ports/ioq3`, M6) | 40.7-41.2 | 24.5 |

The gl4es build spends about 10 ms a frame on GL work: gl4es's fixed-function
emulation (about 5 ms, including 2 ms for Quake's 2,600 state calls a frame)
and ogles2's 130 draws (3.7 ms). MiniGL's GL work is about 2 ms (inferred from
its frame time). Two state-tracking layers cost on Emu68, where a memory
access or a call is dear (`tests/perf/emuspeed`: 3-5 ns a 68k instruction,
48 ns a call). The patches to gl4es (`ports/gl4es/patches`) gained 1 fps;
there is no single hot spot left in it. A renderer written for GLES2 removes
that layer, and keeps the world in vertex buffers, so its geometry is not
copied every frame (ogles2 converts a buffer's contents once, `buffer.c`).

## 2. What exists

- **Upstream ioquake3**, cloned to `C:\- CPP\Quake3\ioq3-upstream` (commit
  `83a7762`, 2026-09-17, `--depth 1`). The GLES2 support is in `code/renderergl2`
  ([PR #664](https://github.com/ioquake/ioq3/pull/664), merged 2024-06-06):
  - GLSL written as `#version 100` (ES 2.0) or `#version 300 es`
    (`tr_glsl.c`); the game "displays okay with the default cvars".
  - No framebuffer objects on the ES path: no HDR/auto-exposure, SSAO or sun
    shadows. Unsupported opengl1 cvars are reported and turned off.
  - ES guarantees 8 vertex attributes; vertex animation falls back to the CPU
    when fewer than 16 (ogles2 reports 16).
  - The PR's author measured about 5 fps on a Raspberry Pi 3B (VideoCore IV,
    a far weaker GPU), mostly put down to the renderer's design.
  - Built with CMake; SDL2 for the window, input and GL context
    (`code/sdl/sdl_glimp.c`, `r_preferOpenGLES`).
- **The user's Amiga ioquake3** (`C:\- CPP\Quake3\MiniGL_Library_Quake3\ioq3`,
  Cowcat's port with the PiStorm3D MiniGL build): AmigaOS 3 platform code in
  `code/amiga` (main, input, sound through AHI, DLL loading, console), the
  game modules as `.dll`s, the old renderer in `code/renderer`. Not modified
  by us; `ports/ioq3` copies it.
- **`ports/ioq3`** (M6): builds that tree on gl4es and ogles2; its
  `amiga_glimp.c` creates the ogles2 context (fullscreen by mode ID, or a
  window), and the m6-2 test package runs the timedemo with probes.
- **Both renderers use `REF_API_VERSION 8`**, so upstream's renderer may be
  dropped into the Amiga engine as it is (to be confirmed field by field,
  phase 0).

## 3. How it fits together

```
Quake III engine (Cowcat's Amiga ioq3: qcommon, client, server, code/amiga)
   |  refexport_t / refimport_t (REF_API_VERSION 8)
upstream renderergl2 + renderercommon      <- copied, unchanged where possible
   |  qgl* function pointers (renderercommon/qgl.h, 153 entries)
ports/ioq3-gl2/amiga_glimp_gl2.c           <- new: context, GLimp_*, qgl table
   |  OGLES2Dispatch (libogles2.a), as gl4es's glue (ports/gl4es/gen_glue.py)
ogles2.library
```

- **Engine:** keep Cowcat's tree (its Amiga code works, and the game data
  and `.dll`s are proven). Only if the renderer interface turns out to
  differ, consider the other way round (upstream engine with Cowcat's
  `code/amiga`).
- **GL entry points:** the renderer fetches every GL function by name
  (`GLimp_GetProcAddress`). Generate the table from `libraries/ogles2_dispatch.h`
  as `ports/gl4es/gen_glue.py` does: one small function per entry calling
  `OGLES2Dispatch->GLx(OGLES2CurrentContext, ...)`, and a name lookup.
  Desktop-only names (`glClearDepth`, `glDepthRange`, `glDrawBuffer`,
  `glPolygonMode`, ...) stay NULL; the ES path must not need them (check).
- **Context:** reuse `ports/ioq3/amiga_glimp.c` (fullscreen through a BGRA/RGBA
  mode ID, or a window, depth 24 and stencil 8), selecting the ES path where
  upstream reads `r_preferOpenGLES`, and reporting GL_VERSION "OpenGL ES 2.0".
- **Build:** a `ports/ioq3-gl2/build.sh` like `ports/ioq3/build.sh` (GCC 6.5,
  clib2, the user's CFLAGS, `-lm881`), not CMake. Output `quake3-gl2` and the
  same game `.dll`s.
- **Licence:** GPL v2 or later, as ioquake3; kept apart from the MIT library
  as `ports/ioq3` is.

## 4. Phases

Each phase ends with an A1200 run (a package under `tests/a1200`, one short
run with a frame limit, the full timedemo behind `FRAMES=0`, as m6-2).

### Phase 0: survey (a day; no Amiga run)
- Diff `renderercommon/tr_public.h` and `tr_types.h` between the two trees
  (refimport fields, `glconfig_t`, new calls such as `ri.Sys_GLimpInit`),
  and list what renderergl2 needs from the engine that Cowcat's lacks.
- List the qgl functions the ES path actually calls (`QGL_*_PROCS`,
  `tr_extensions.c`) and check each against ogles2 (142 GLES2 functions,
  `GL_OES_*` extensions; Appendix B of the main plan). Watch for
  `glMapBufferRange`, VAOs (`GL_OES_vertex_array_object`: not provided),
  `glDrawRangeElements`, float textures, depth textures.
- Count the GLSL programs `GLSL_InitGPUShaders` builds with the default
  cvars (variants of generic up to 64, lightall up to 128, fog 8, dlight 2,
  and a dozen single ones; many skipped): the first start compiles all of
  them on the target.
- **Exit:** a written list of gaps and the decision engine-first or
  renderer-first.

### Phase 1: it builds
- `ports/ioq3-gl2/build.sh`: Cowcat's tree with `code/renderer` replaced by
  upstream `renderergl2` + `renderercommon`, the generated qgl table, the new
  glimp. Compile errors fixed in our files only; changes to the copied
  upstream files kept as patches (as `ports/gl4es/patches`), each with its
  reason.
- **Exit:** `quake3-gl2` links; the game `.dll`s unchanged.

### Phase 2: first frame
- The ES context, GL_VERSION/extension strings as renderergl2 expects them,
  the shader compiles (first start: minutes; then the cache).
- Run to the main menu, then a map.
- **Exit:** menu and map drawn; screenshots compared with the MiniGL build;
  no GL errors in ogles2.log.

### Phase 3: correct
- The timedemo to the end and a clean exit (the M6 exit hang taught: input
  handler, screen lock).
- Visual comparison at a few demo frames with MiniGL's; missing features
  listed (expected: those needing FBOs).
- **Exit:** the timedemo looks right to the user; m5-1 still passes.

### Phase 4: fast
- Probe P and the draw split (as M6) on this renderer: draws a frame, ogles2
  per draw, buffer uploads (`glBufferSubData` per draw for the dynamic
  geometry: `tr_vbo.c` orphans the tess buffer with `glBufferData(NULL)` and
  writes each attribute), uniform calls.
- Likely work in ogles2: a cheap path for a buffer orphaned and refilled
  every draw (today a buffer change drops its GPU copies and the draw
  converts again), uniform upload cost, many small programs.
- Renderer-side options: cvars that cut work (`r_normalMapping 0`,
  `r_specularMapping 0`, `r_cubeMapping 0`, `r_dlightMode 0`, ...).
- **Exit:** the timedemo's fps against MiniGL's 60.8 and gl4es's 41, the
  split of a frame, and the decision whether this build replaces the gl4es
  one as the Quake III test.

### Phase 5: package and documents
- `tests/a1200/m6-3` (or the next free name): the build, Run script, ReadMe,
  notes template; README in `ports/ioq3-gl2`; the main plan's §13.

## 5. Risks

| Risk | Effect | Answer |
|---|---|---|
| Renderer interface differs from Cowcat's engine | Phase 1 grows | Phase 0 decides; fall back to upstream engine + `code/amiga` |
| The ES path needs something ogles2 lacks (VAOs, map buffer range, float or depth textures) | No first frame | Add to ogles2 where GLES2 extensions allow, or patch the renderer to the fallback |
| Many shader programs, each compiled on the target | Minutes at first start | Cache (done); could ship the cache in the package |
| renderergl2's CPU work per draw (uniform blocks, more state) | Slower than hoped | Phase 4 measures before any rewrite; cvars first |
| Per-draw buffer streaming expensive in ogles2 | Dynamic geometry slow | Optimise buffer orphaning in `buffer.c` (phase 4) |
| GLSL features beyond what the compiler handles | Shaders refused | Compiler fixes (v3dglslc) or renderer patches |

## 6. Measuring

Same as M6: m6-2's timedemo command line with `r_primitives` irrelevant (the
new renderer draws whole batches), probes T and P, the glue profile
(`GL4ES_GLUE_PROFILE` has no meaning here; a similar per-function profile in
the generated qgl table), `com_speeds` only with `logfile 1`. MiniGL's figure
is re-measured the same day on the same machine (`fs_homepath` in RAM:).

## 7. Decisions for the user

1. Engine: Cowcat's (preferred) or upstream's.
2. Whether this build replaces the gl4es Quake III build as the M6 test once
   it is faster, or both are kept (gl4es stays the general compatibility path
   either way).
