# ogles2.library for AmigaOS 3.x on PiStorm

OpenGL ES 2.0 for 68k AmigaOS, drawn by the Raspberry Pi 4's GPU (Broadcom V3D 4.2) through
[PiStorm](https://github.com/captain-amygdala/pistorm) and
[Emu68](https://github.com/michalsc/Emu68). The API is source-compatible with AmigaOS 4's
`ogles2.library`: the GLES2 functions plus the `agl*` context calls.

**Status: version 1.3, release candidate.** Milestones M0 to M6 are complete; M7
(conformance and release) lacks only the publication (see
[the development plan](docs/DEVELOPMENT_PLAN.md), §9 for the milestones and §13 for the current
state and next steps).

- The GLES2 core, the `agl*` context calls and 32 extensions (164 entry points), framebuffer
  objects and 4x multisampling included; the list and the deviations from GLES2 and from
  AmigaOS 4's library are in [`release/ogles2.doc`](release/ogles2.doc).
- Conformance: Khronos dEQP-GLES2's must-pass list (17,165 cases) passes on an A1200 except
  25 cube-map cases (the GPU filters cube maps seamlessly, as GLES 3 requires) and 6 timing
  and quality warnings.
- Faster than MiniGL: the same gears scene at 640x480 runs full screen at 1734 fps on ogles2's
  vertex buffers against 791 fps for PiStorm3D 29.1's MiniGL (display lists, its fastest path),
  1171 against 161 fps with 8,000 triangles, and 445 against 47 fps with 32,000 triangles on a
  hidden bitmap (`tests/perf`, plan §13).
- Desktop OpenGL programs run through gl4es (`ports/gl4es`), which writes its shaders at run time
  for the on-target compiler.
- Real programs run unmodified: es2gears and ten samples from the *OpenGL ES 2.0 Programming
  Guide* match Mesa's reference images on a real A1200 (`tests/corpus`, 11 of 11).
- **Shaders compile on the Amiga** (M3): with the compiler installed as
  `LIBS:v3dglslc`, a program the library has no binary for is compiled at `glLinkProgram`
  (about 0.2 s, after a one-time start of the compiler) into the disk cache, bit-identical
  to the offline compiler's output. Without it, the library loads binaries compiled on a PC
  (`.v3dp` files, found by the hash of the GLSL sources) and writes the sources of missing
  ones to `T:ogles2_missing_shaders.txt`. `glCompileShader` reports GLSL errors with the
  compiler's messages; `OGLES2_CCT_DEBUG_SHADER_LOG` makes link logs describe each compiled
  variant (instructions, threads, spills).
- Contexts can share textures, buffers, shaders and programs, and renderbuffers
  (`OGLES2_CCT_SHARE_WITH`), also from several tasks: a group's calls run under
  its semaphore. The current context of `<proto/ogles2.h>`'s calls is per
  program, so a second task calls through `OGLES2Dispatch` with its context.
- The release archive (`release/build_release.sh`): the library, the compiler, an installer,
  examples, and the SDK (headers, import libraries for GCC and vbcc, autodoc).

## Requirements

| | |
|---|---|
| Hardware | An Amiga with a PiStorm and a Raspberry Pi 4B or CM4 (the GPU is the Pi's) |
| CPU emulation | Emu68 (tested: 1.1.0-beta.1) |
| OS | AmigaOS 3.x with an RTG system (Picasso96 or CyberGraphX); tested on 3.2 |
| Display | A 32-bit RTG screen (the library renders into 32-bit bitmaps) |

## Using it in a program

```c
#include <proto/ogles2.h>        /* GL ES 2.0 and agl*, through the library */

if (OGLES2Open()) {              /* libogles2.a: opens ogles2.library */
    ULONG err;
    void *ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 24, TAG_DONE);
    aglMakeCurrent(ctx);
    /* ... GLES2 calls, aglSwapBuffers() per frame ... */
    aglDestroyContext(ctx);
    OGLES2Close();
}
```

Link with `libogles2.a` (GCC) or `ogles2.lib` (vbcc). With clib2 and `-m68881`, link maths
with `-lm881`, not `-lm` (see [CONTRIBUTING.md](CONTRIBUTING.md#traps)). Without a window or
screen tag the library opens its own window; `OGLES2_CCT_WINDOW`, `OGLES2_CCT_MODEID` and
`OGLES2_CCT_BITMAP` render into yours (`include/libraries/ogles2.h`).

**Release:** `bash release/build_release.sh` (in WSL) builds the user archive with the SDK
(`~/ogles2-build/release/ogles2-<version>.lha`: the library, the compiler, an installer,
examples, headers, import libraries and the autodoc `ogles2.doc`). A program finds
`ogles2.library` and `v3dglslc` in its own drawer (`libs/`, or beside it) when they are not
installed, so it can ship them.

**Shaders:** with `LIBS:v3dglslc` installed (or `v3dglslc` in the program's drawer) nothing is
needed: programs are compiled when they link, into `OGLES2Cache:` if that assign exists (kept), else `T:`. To ship binaries instead (or
without the compiler): run the program once; the library appends each missing program's
sources to `T:ogles2_missing_shaders.txt`. On the PC, `v3dp/split_shaders.py` splits that
file, `v3dglslc` (`tools/v3dglslc`) compiles each pair to `.v3dp`, and
`v3dp/v3dp_hashname.py` names the files by hash. Put them next to the program (`PROGDIR:`)
or in `OGLES2Cache:`.

**Logging:** the library logs to `T:ogles2.log`, or to the file named in `ENV:OGLES2/LogFile`;
`OGLES2Log` (in `cmd/`) writes the buffered log while a program is still running.

## Building

Everything builds on Linux (Ubuntu 24.04; on Windows inside WSL). The m68k toolchains, Mesa
(for the shader compiler) and the host tools are set up by the scripts in
[`dev/host-setup/`](dev/host-setup/README.md); the toolchains are pinned to the commits
ogles2 was tested with.

```sh
make -j8                                   # ~/ogles2-build/m68k: ogles2.library, libogles2.a, tests, OGLES2Log
make amictl-test                           # host test of the remote test station (no Amiga needed)
bash tests/a1200/build_package.sh m2-4     # an A1200 test package -> tests/a1200/out/ogles2-m2-4.lha
bash tests/corpus/build_corpus.sh          # the corpus: Amiga and PC builds, reference images, .v3dp files

# The shader compiler (needs the Mesa build with tools/v3dglslc/patches applied)
python3 tools/v3dglslc/replay.py           # Mesa for 68k (GCC 16.2), ~900 files
bash tools/v3dglslc/build_m68k.sh          # -> ~/ogles2-build/v3dglslc/v3dglslc
bash tools/v3dglslc/build_host.sh          # -> ~/ogles2-build/v3dglslc/host/v3dglslc (the offline compiler)
bash tools/v3dglslc/test/compare_v3dp.sh   # x86-64 and 68k .v3dp files must be identical
```

The library is built with GCC 6.5.0b (the compiler PiStorm3D is built with), the shader
compiler with GCC 16.2 and software floating point. GCC 16.1 miscompiles and must not be used.

## Testing

Tests run on a real A1200, either by hand (copy a package from `tests/a1200/out/`, run its
`Run-*` script, send back `results/`) or remotely through the test station in `tools/amictl`
(a small server on the Amiga, reached over the LAN with a token and an address allow list).
Each package's `ReadMe.txt` says what it does and what to look for. Results of every run so
far are in `tests/a1200/results/`. [CONTRIBUTING.md](CONTRIBUTING.md) describes the workflow.

## Layout

| Path | What |
|---|---|
| `ogles2/` | The library: contexts, state, programs, uniforms, draws, textures, buffers, jobs, display |
| `v3dcore/` | PiStorm3D's V3D layer (control lists, memory, textures, submission), see `v3dcore/PROVENANCE.md` |
| `client/` | `libogles2.a`, the import library: `OGLES2Open/Close`, current context, `agl*` |
| `include/` | Public headers: `libraries/ogles2.h`, `clib/ogles2_protos.h`; Khronos `GLES2/` and `KHR/` |
| `gen/` | `gen_ogles2.py`: dispatch table, client wrappers, `aglGetProcAddress` thunks, stubs and ABI test, from Khronos `gl.xml` and the append-only `abi_order.txt` |
| `v3dp/` | The `.v3dp` program-binary format (`FORMAT.md`, frozen at 1.0), writer, reader, helpers |
| `tools/v3dglslc/` | The GLSL compiler: Mesa's GL front end and V3D driver on a fake DRM device, built for the PC (offline compiler) and for 68k (the seed of M3's on-target compiler) |
| `tools/amictl/` | The remote test station: `amictld` (Amiga server) and `amictl` (PC client) |
| `tests/` | Unit and hardware tests, `a1200/` test packages and results, `corpus/` (real programs against reference images) |
| `cmd/` | `OGLES2Log` |
| `dev/` | Build-machine setup (`host-setup/`), the reference sources fetcher, the stream C spike (`spike-c/`: the compiler's feasibility study) |
| `release/` | The release archive's builder, ReadMe, installer, autodoc and examples |
| `docs/` | The development plan |
| `licences/` | Licence texts and third-party notices |

## Licence

`ogles2.library` as built incorporates PiStorm3D's V3D backend (`v3dcore/`), so it is under the
Hyperion MiniGL Open Source License ([`LICENSE`](LICENSE), `licences/License_MiniGL_Library.txt`):
the library may only be built for AmigaOS, and the source of all modifications must be
published to the Amiga developer community. That licence covers `v3dcore/`, the six front-end
files that adapt PiStorm3D's code (listed in `LICENSE`) and `tools/builtin_shaders/`.
Everything else written for this project is MIT, including the import library and headers
programs build against (`client/`, `include/libraries/`, `include/clib/`), so a program of any
licence, GPL included, may link them. Third-party code and the ports keep their own licences
and notices (`LICENSE` sections 3 and 4; `licences/License_V3D_ThirdParty.txt`).

## Credits

ogles2 is developed by R-Typer and Ladislawv, with Claude Opus 5.5 (Anthropic's AI assistant).

It builds on: PiStorm3D (`minigl.library` for PiStorm) by Steffen "MagicSN" Haeuser, whose V3D backend this
library builds on; the V3D assembler and V3D documentation by Macoy Madson; Mesa's V3D driver
and compiler (Broadcom, VMware and the Mesa contributors); Emu68 by Michal Schulz; PiStorm by
Claude Schwarz and contributors; bebbo's amiga-gcc.
