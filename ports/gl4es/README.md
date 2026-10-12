# gl4es on AmigaOS 3 (ogles2.library)

[gl4es](https://github.com/ptitSeb/gl4es) turns desktop OpenGL (up to 2.1: `glBegin`, display
lists, fixed-function lighting, ARB programs) into OpenGL ES 2.0. On AmigaOS 4 it runs on that
system's `ogles2.library`; this directory builds it for AmigaOS 3 on ours (plan §9, M6). gl4es
writes its shaders while the program runs, so the on-target compiler (M3, `LIBS:v3dglslc`) must
be installed.

## Building

In WSL, after `make`:

```sh
make gl4es          # runs ports/gl4es/build.sh, then builds tests/perf/gears_mgl.c as gears_gl4es
```

`build.sh` clones gl4es into `$GL4ES_SRC` (default `~/gl4es`) at the commit the plan references
and writes `$B/gl4es/libgl4es.a`. A program includes gl4es's `<GL/gl.h>` (`$GL4ES_SRC/include`)
and `gl4es_agl.h` (`ports/gl4es/include`), and links `-lgl4es -logles2 -lm881`, in that order.

## How it is put together

- **gl4es's sources**, built with its AmigaOS 4 code paths (`-DAMIGAOS4 -DNOX11
  -DNOEGL`: no X11, EGL or `dlopen`), GCC 6.5 and clib2, the library's CPU flags, with GCC's
  builtins on except sin and cos.
- **`patches/`**, applied in order on every build to a clean checkout (edits left in it are
  discarded; `GL4ES_EXTRA_PATCHES` adds more, for diagnosis). Each says why at its top:
  - `0001-mipmap-chain-given`: a mip chain the program uploads down to 1x1 counts as done.
    Quake III sets its mipmapping filter after the upload, so gl4es generated every chain again
    on the CPU at the texture's first draw (1.2 s over the timedemo).
  - `0002-fpe-hash-by-long-words`: the fixed-function state's hash reads long words, not bytes.
  - `0003-fpe-program-unchanged`: `fpe_program` remembers the raw fixed-function states of
    its last four programs and takes the program on a match, without `fpe_ReleventState`
    (bitfield clears for all 16 texture units, every draw). `agl_os3.c` clears the memo when
    a context is destroyed.
  - `0004-locked-range-draw`: a draw of the program's own arrays between `glLockArraysEXT` and
    `glUnlockArraysEXT` (Quake III locks every batch) goes to ogles2's `glDrawRangeElements`
    (ogles2 1.1) with the locked range, so ogles2 does not scan the indices for it (about
    3.5 us a Quake III draw). Without that entry point it is `glDrawElements` as before.
  - `0005-builtin-uniforms-unchanged`: a program's built-in matrix uniforms are not sent again
    while the matrices are those it last sent: `matrix.c` bumps a generation when a matrix's
    value changes (not for a `glLoadIdentity` of an identity or a `glLoadMatrixf` of the same
    values; Quake III resets its texture matrices around every stage), and the program
    remembers the generation, the glstate and the normal rescaling (cleared at link). The
    texture-environment and sampler loops skip uniforms the program does not have (16 units,
    three calls each). About 5 us a draw.
- **`os3_string.c`**: a `memcmp` comparing long words, linked before clib2's (a byte at a
  time, 2.4 us for 256 bytes on Emu68); gl4es compares its state structs with it every draw.
- **`os3_build.h`**, force-included: gl4es exports every GL name as a GCC `alias` of its
  `gl4es_*` function, which the Amiga's a.out objects do not support; an assembler equate in the
  same file (`_glFoo = _gl4es_glFoo`) gives the second name. It also names two signals clib2
  lacks.
- **`gen_glue.py`** writes `ogles2_glue.c` in place of gl4es's `src/agl/amigaos.c`: one static
  function per entry of ogles2.library's dispatch table (`libraries/ogles2_dispatch.h`), calling
  it with the program's current context, and `os4GetProcAddress` mapping the GLES2 names to
  them. Not named `gl*`, because gl4es exports those itself; libogles2.a's own `gl*` calls are
  inline wrappers, so the two never meet at link time.
- **`agl_os3.c`** in place of gl4es's `src/agl/agl.c`: the `agl*` functions a program calls,
  keeping gl4es's per-context state and libogles2.a's current context. gl4es is built without a
  constructor (`NO_INIT_CONSTRUCTOR`): the first `aglCreateContext2` starts it and an `atexit`
  handler stops it and closes the library.

## Status (2026-10-11)

Quake III Arena runs on it (`ports/ioq3`): the timedemo at 40 fps with `r_primitives 0` (MiniGL
61). Of a 25 ms frame, gl4es's own work is about 5 ms: the draw's fixed-function setup (texture
realisation 0.6 ms, program selection 0.9, built-in uniforms 1.1, attributes 0.3) and Quake's
2,600 state calls (2 ms; `glDisable` 336 a frame, 0.45 ms). Plan §13.

The gears comparison's scene (`tests/perf`, glxgears' geometry, lit) draws correctly through
gl4es: the same pixel counts as ogles2's and MiniGL's renders. A1200 run `ogles2-m6-1`, frames
per second at 640x480:

| | immediate mode | vertex arrays | display lists |
|---|---|---|---|
| bitmap, 800 triangles | 157 → 358 | 548 → 736 | 522 → 695 |
| bitmap, 8,000 triangles | 21 → 63 | 148 → 344 | 138 → 275 |
| window / screen, 800 triangles | | | 341 / 714 → 397 / 976 |

(First run → `ogles2-m6-1-20261011-204439`, after M6's per-draw work in ogles2 and the
patches.)

gl4es's start-up compiles three probe shaders for optional GLSL features that fail by design,
and finds that the compiler takes GLSL ES 3.00, which it then uses. Immediate mode costs about
2 microseconds of 68k time per vertex in gl4es.

## Licence

gl4es is MIT (Copyright (c) 2016-2018 Sebastien Chevalier and contributors); `agl_os3.c` is
derived from its `src/agl/agl.c`. The other files here are MIT like the rest of the tools.
