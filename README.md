# ogles2.library: OpenGL ES 2.0 for the classic Amiga

**Modern GPU graphics for AmigaOS 3.x.** On an Amiga with a
[PiStorm](https://github.com/captain-amygdala/pistorm) and a Raspberry Pi 4, `ogles2.library`
gives 68k programs real OpenGL ES 2.0, with programmable shaders, drawn by the Pi's VideoCore
GPU (Broadcom V3D 4.2) under [Emu68](https://github.com/michalsc/Emu68).

It is the same API that phones, browsers and game engines use, and the same library interface
AmigaOS 4 has: GLES2 code from Linux, Android, the web or OS4 compiles and runs on the classic
Amiga.

**Version 1.3.** Ready-made archives are in [`release/dist/`](release/dist/).

## Why it matters

- **Real shaders.** Full GLSL ES 1.00, compiled by Mesa's own V3D compiler, running *on the
  Amiga*: a program hands the library its shader source, and it is compiled at link time and
  cached on disk. Or ship the compiled binaries with the program and start instantly.
- **Fast.** Quake III with its modern GLES2 renderer runs about **30% faster** than on MiniGL
  (timedemo `four` at 640x480: 80.4 against 61.3 fps). Raw throughput is in another league: the same gears
  scene at 1734 fps against MiniGL's 791, and with 32,000 triangles 445 fps against 47.
- **Proven against the standard.** Khronos' own dEQP-GLES2 test suite, all 17,165 cases on a real
  A1200: everything passes except 25 cube-map cases, where the GPU filters edges seamlessly (as
  GLES 3 requires). Code that works on other GLES2 systems works here.
- **Complete.** The whole GLES2 core plus 32 extensions (164 entry points): framebuffer objects,
  float and depth textures, S3TC/DXT and ETC1 compression, up to four render targets, clip
  control, anisotropic filtering, 4x MSAA with alpha-to-coverage, 32 texture units, shared
  contexts across tasks, and vsync without tearing.
- **Ready for your favourite libraries.**
  - **SDL2 and SDL3** with an ogles2 backend: SDL's 2D renderer moves onto the GPU (sprites
    6.5 times faster than SDL2's software renderer), and SDL games can use shaders. A developer
    archive ships them prebuilt ([`docs/SDL.md`](docs/SDL.md)).
  - **gl4es** runs desktop OpenGL 1.x/2.x programs (glxgears, Quake III's classic renderer) on
    top of ogles2.
  - **glmark2-es2** scores 251 at 800x600 with no dropped frames.
- **Easy to ship.** No installation needed: a program can carry `ogles2.library` and the shader
  compiler in its own drawer. Or install them once with the included installer.

## What it opens up

Most graphics software written in the last fifteen years targets GLES2 or something it maps onto:
game engines, emulators with shader-based video, SDL games, and the GPU paths of large
applications such as web-browser engines. With ogles2 the graphics side of such a port to the
classic Amiga is solved; what remains is the CPU and OS code.

## Getting it

| Archive | For | Contents |
|---|---|---|
| `ogles2-1.3.lha` | users | the library, the shader compiler, `Install-ogles2`, `OGLES2Log`, two examples |
| `ogles2-1.3-sdk.lha` | Amiga programmers | the same plus the SDK: headers, import libraries for GCC and vbcc, the autodoc `ogles2.doc`, the guide `GettingStarted.txt`, example sources |
| `ogles2-1.3-sdk.zip` | cross-development on a PC | the SDK alone |
| `ogles2-1.3-sdl.lha` / `.zip` | SDL programmers | SDL2 and SDL3 with the ogles2 backend, the MiniGL SDK SDL links, two examples; unpacks over the SDK |

### Requirements

| | |
|---|---|
| Hardware | An Amiga with a PiStorm and a Raspberry Pi 4B or CM4 |
| CPU emulation | Emu68 (tested: 1.1.0-beta.1) |
| OS | AmigaOS 3.x with Picasso96 or CyberGraphX (tested on 3.2) |
| Display | A 32-bit RTG screen |

## Writing a program

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

Link with `libogles2.a` (GCC) or `ogles2.lib` (vbcc). Without a window or screen tag the library
opens its own window; `OGLES2_CCT_WINDOW`, `OGLES2_CCT_MODEID` and `OGLES2_CCT_BITMAP` render into
yours (`include/libraries/ogles2.h`). AmigaOS 4 code compiles unchanged (recompile: the tag values
differ). With clib2 and `-m68881`, link maths with `-lm881`, not `-lm`
([CONTRIBUTING.md](CONTRIBUTING.md#traps)). The SDK's `GettingStarted.txt` walks through windows,
full screen, resizing, shaders, performance and shipping, and includes a prompt for working with an
AI assistant.

**Shaders.** With the compiler installed (`LIBS:v3dglslc`, or in the program's drawer) nothing is
needed: programs compile when they link (about 0.2 s, once), into `OGLES2Cache:` if that assign
exists, else `T:`; `glCompileShader` reports GLSL errors with the compiler's messages. To ship
binaries instead: run the program once and ship the `.v3dp` files from the cache in a `shaders/`
drawer beside it (`PROGDIR:shaders/`), or right next to it (`PROGDIR:`). Without a compiler, the library writes the sources it is missing to
`T:ogles2_missing_shaders.txt` for the offline compiler on a PC (`tools/v3dglslc`,
`v3dp/split_shaders.py`, `v3dp/v3dp_hashname.py`).

**Logging.** The library logs to `T:ogles2.log`, or to the file named in `ENV:OGLES2/LogFile`;
`OGLES2Log` writes the buffered log while a program is still running.

The full reference, with every call, tag, extension, limit and the deviations from GLES2 and from
AmigaOS 4's library, is [`release/ogles2.doc`](release/ogles2.doc).

## Building from source

Everything builds on Linux (Ubuntu 24.04; on Windows inside WSL). The m68k toolchains, Mesa (for
the shader compiler) and the host tools are set up by the scripts in
[`dev/host-setup/`](dev/host-setup/README.md), pinned to the commits ogles2 was tested with.

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

bash release/build_release.sh dist sdl     # the release archives, into release/dist/
```

The library is built with GCC 6.5.0b (the compiler PiStorm3D is built with), the shader compiler
with GCC 16.2 and software floating point. GCC 16.1 miscompiles and must not be used.

## Testing

Every change is tested on a real A1200, either by hand (copy a package from `tests/a1200/out/`,
run its `Run-*` script, send back `results/`) or remotely through the test station in
`tools/amictl` (a small server on the Amiga, reached over the LAN with a token and an address
allow list). Each package's `ReadMe.txt` says what it does and what to look for; the results of
every run are in `tests/a1200/results/`. Real programs are checked against Mesa's reference
images: es2gears and the *OpenGL ES 2.0 Programming Guide* samples match (`tests/corpus`).
[CONTRIBUTING.md](CONTRIBUTING.md) describes the workflow, and
[the development plan](docs/DEVELOPMENT_PLAN.md) the milestones (§9) and the current state (§13).

## Layout

| Path | What |
|---|---|
| `ogles2/` | The library: contexts, state, programs, uniforms, draws, textures, buffers, jobs, display |
| `v3dcore/` | PiStorm3D's V3D layer (control lists, memory, textures, submission), see `v3dcore/PROVENANCE.md` |
| `client/` | `libogles2.a`, the import library: `OGLES2Open/Close`, current context, `agl*` |
| `include/` | Public headers: `libraries/ogles2.h`, `clib/ogles2_protos.h`; Khronos `GLES2/` and `KHR/` |
| `gen/` | `gen_ogles2.py`: dispatch table, client wrappers, `aglGetProcAddress` thunks, stubs and ABI test, from Khronos `gl.xml` and the append-only `abi_order.txt` |
| `v3dp/` | The `.v3dp` program-binary format (`FORMAT.md`), writer, reader, helpers |
| `tools/v3dglslc/` | The GLSL compiler: Mesa's GL front end and V3D driver on a fake DRM device, built for the PC (offline) and for 68k (on the Amiga) |
| `tools/amictl/` | The remote test station: `amictld` (Amiga server) and `amictl` (PC client) |
| `ports/` | SDL2, SDL3, gl4es, Quake III (gl4es and GLES2 renderers), glmark2, dEQP-GLES2 |
| `tests/` | Unit and hardware tests, `a1200/` test packages and results, `corpus/` (real programs against reference images) |
| `cmd/` | `OGLES2Log` |
| `dev/` | Build-machine setup (`host-setup/`), the reference sources fetcher, the compiler's feasibility study (`spike-c/`) |
| `release/` | The release builder, ReadMe, installer, autodoc, guide, examples, and the built archives (`dist/`) |
| `docs/` | The development plan, the SDL guide |
| `licences/` | Licence texts and third-party notices |

## Licence

`ogles2.library` as built incorporates PiStorm3D's V3D backend (`v3dcore/`), so it is under the
Hyperion MiniGL Open Source License ([`LICENSE`](LICENSE), `licences/License_MiniGL_Library.txt`):
the library may only be built for AmigaOS, and the source of all modifications must be published
to the Amiga developer community. That licence covers `v3dcore/`, the six front-end files that
adapt PiStorm3D's code (listed in `LICENSE`) and `tools/builtin_shaders/`. Everything else written
for this project is MIT, including the import library and headers programs build against
(`client/`, `include/libraries/`, `include/clib/`), so a program of any licence, GPL included, may
link them. Third-party code and the ports keep their own licences and notices (`LICENSE` sections
3 and 4; `licences/License_V3D_ThirdParty.txt`).

## Credits

ogles2 is developed by R-Typer and Ladislawv, with Claude Opus 5.5 (Anthropic's AI assistant).

It builds on: PiStorm3D (`minigl.library` for PiStorm) by Steffen "MagicSN" Haeuser, and its V3D
backend, which this library builds on, by Dennis van der Boon; the V3D assembler and V3D documentation by Macoy Madson; Mesa's
V3D driver and compiler (Broadcom, VMware and the Mesa contributors); Emu68 by Michal Schulz;
PiStorm by Claude Schwarz and contributors; bebbo's amiga-gcc; SDL2_68k and SDL3_68k by Steffen
Haeuser.
