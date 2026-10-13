# dEQP-GLES2 on ogles2.library

The OpenGL ES 2.0 conformance tests of Khronos' VK-GL-CTS (the drawElements
Quality Program's `deqp-gles2`), built for AmigaOS 3 (68020 + 68881, libnix)
and run on ogles2.library. Milestone M7 of `docs/DEVELOPMENT_PLAN.md`.

## Building

In WSL, after `make` (which builds `libogles2.a`):

    bash ports/deqp/build.sh

The result is `$B/deqp/deqp-gles2` (`B` defaults to `~/ogles2-build/m68k`) with
its data in `$B/deqp/gles2`. The A1200 package `tests/a1200/m7-1` carries both.

- **Sources:** VK-GL-CTS `opengl-es-cts-3.2.14.1` (commit `067e883`). It is a
  shallow clone into `~/vk-gl-cts`, because the full history is several GB;
  `CTS_SRC` names another tree. zlib 1.2.13 and libpng 1.6.50 are the versions
  in the CTS's `external/fetch_sources.py`, downloaded with their checksums
  into `~/deqp-deps`.
- **Compiler:** GCC 16.2 (`TC16`), since the CTS is C++17, as for glmark2
  (`ports/glmark2`). `libogles2.a` from the GCC 6.5 build links as it is.
- **What is compiled:** only what `deqp-gles2` needs. The script compiles it
  itself rather than through the CTS's CMake, which would configure Vulkan,
  glslang and amber too:
  - the framework: delibs, qphelper, tcutil, glutil, the simple reference
    renderer, the reference rasterizer and the random shader generator;
  - `modules/glshared` and `modules/gles2`.
- **`patches/`** are applied to a copy of the sources in `$B/deqp/src`:
  - `0001-amiga-m68k.patch`: delibs gains a 68k CPU (32-bit, big-endian).
    An assertion prints on stdout, then aborts. Aligned allocation uses
    delibs' own header, since libnix has no `posix_memalign`. There is no
    signal crash handler, since libnix has no `sigaction`.
  - `0002-big-endian-pkm.patch`: the ETC1 `.pkm` loader swapped the bytes
    of a native 16-bit read, which is right only on a little-endian host.
    The 54 ETC1 texture cases failed on reading their files.
  - `0003-big-endian-point-check.patch`: `rasterization.limits.points` read
    RGBA8 pixels as `uint32_t` and took red from the low byte, so it saw the
    green it wanted as red. It now builds the colour from the bytes.
- **`src/`**:
  - `tcuAmigaPlatform.cpp`: the platform. A render context is an ogles2
    context with a window on the Workbench screen. It is RGBA8, depth 24,
    stencil 8, 4x multisampled for an `ms4` configuration, and has no vsync.
    The GL functions come from `aglGetProcAddress`, and Ctrl-C stops the run
    between tests.
  - `deqp-amiga-os.c`: delibs' OS layer where libnix has no Unix equivalent.
    - Threads: none. Every test runs in the one task, so mutexes, semaphores
      and thread-locals belong to that task.
    - The clock reads timer.device's E-clock.
    - No dynamic libraries.
  - `include/fenv.h`: `fegetround`/`fesetround` on the FPU's FPCR, which
    libnix lacks.
  - `amiga-compat.cpp`: the C-only locale and `__xpg_strerror_r`, as in
    glmark2's port.

## Traps (beyond glmark2's)

- **`int32_t` is `long`** in this toolchain (GCC's `__INT32_TYPE__`, which
  newlib's `<machine/_default_types.h>` follows). dEQP takes it to be `int`
  everywhere: `GLint` is `int32_t`, and it calls `std::max(int, int32_t)`.
  The build redefines `__INT32_TYPE__` and `__INT_LEAST32_TYPE__` as `int`.
- **`amiga` and `amigaos` are predefined macros** (1). Neither can name a
  namespace.
- **AmigaDOS quoting:** inside double quotes `*` escapes the next character.
  A test pattern such as `dEQP-GLES2.info.*` must stay unquoted in a script.

## Running

    deqp-gles2 --deqp-case=dEQP-GLES2.info.* --deqp-log-filename=info.qpa \
        --deqp-surface-width=256 --deqp-surface-height=256 \
        --deqp-gl-config-name=rgba8888d24s8ms0 \
        --deqp-log-images=disable --deqp-log-shader-sources=disable

`--deqp-archive-dir` must name the directory that holds `gles2/` with a
volume, such as `M7R1:bin`. Its default, `.`, makes paths like
`./gles2/data/...`, which AmigaDOS does not know. Package `m7-1`'s `Run-M1`
does all this, and takes the keys `LIST`, `CASES`, `NAME` and `MSAA`.

From the PC:
- `caselists.sh OUTDIR` splits the CTS's must-pass list for GLES2 (17,165
  cases) into a file per group, `shaders-<subgroup>.txt` for the shader
  tests. Package `m7-1` ships them as `caselists/`.
- `python ports/deqp/run_groups.py [GROUP...] [--shaders | --all]` runs
  groups on the A1200 through `amictl runpkg`, one package run per list.
  Without arguments it runs every non-shader group.
- `python ports/deqp/summary.py RESULTS...` totals the `.qpa` logs by result
  and group, and lists what did not pass. A later run of a case replaces an
  earlier one, and `--depth N` sets the group depth.
- `python ports/deqp/failed_list.py OUT.txt RESULTS...` writes a case list of
  what did not pass, to rerun just those: copy it into the built package's
  `caselists/` and run `LIST=<its name>`. Cases that are not supported for a
  reason that holds on ogles2 are left out.
- `python ports/deqp/qpa_images.py LOG.qpa OUTDIR [CASE]` writes a log's
  images (result, reference, error mask) as PNG files. It needs a run with
  `Run-M1 IMAGES=1`.

`Run-M1 TEST=points` runs ogles2's own point-size test (`tests/m7_points`)
instead of dEQP.

## Known deviation

V3D always filters cube maps seamlessly, across the faces' edges, as GLES3
requires. dEQP-GLES2 expects seamless filtering only from a context that
also answers GLES3's `GL_MAJOR_VERSION`, such as Mesa's v3d, a GLES 3.1
driver. From a GLES2-only context it expects each face clamped on its own.
So 25 cube cases fail by design:
- `texture.mipmap.cube.*` with linear filtering;
- `texture.vertex.cube.*`;
- `shaders.texture_functions.vertex.texturecubelod`.
